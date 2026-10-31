#include "fifo.hpp"
#include "command_processor.hpp"
#include "../gfx/common.hpp"
#include "../internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "tracy/Tracy.hpp"

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
bool sAsyncDecode = false;
} // namespace detail

namespace {
// The decode thread's side of the recorded buffer, game thread only. With the decode thread on,
// sBufferData holds the FIFO bytes and, copied in between them, the display lists and raw draws
// the game called; sItems says which stretch is which.
enum class ItemKind : uint8_t { Fifo, DisplayList, RawDraw };

struct Item {
  ItemKind kind;
  bool bigEndian;
  uint8_t prim;
  uint8_t fmt;
  uint16_t vtxCount;
  uint32_t offset;
  uint32_t size;
};

std::vector<Item> sItems;
// Bytes of sBufferData that sItems already covers; the rest is FIFO bytes still being recorded.
uint32_t sItemsEnd = 0;
// Where sync mode's drain() would have decoded up to: the end of the last draw, display list or
// raw draw. See insert_at_drain_point.
uint32_t sDrainMark = 0;

void reset_items() noexcept {
  sItems.clear();
  sItemsEnd = 0;
  sDrainMark = 0;
}

void finish();
} // namespace

void init() {
  // The CP register cache below is the decoder's, so the decode thread must be idle first.
  sync();
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  reset_items();
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

void drain() {
  if (detail::sAsyncDecode) {
    finish();
    return;
  }
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  // NSMBW stage timers: the wait above is the game thread blocked on the frame worker;
  // wait_for_frame_worker_sealed returns its duration, so no clock read is needed here.
  if (gfx::stage_timers_enabled()) {
    gfx::stage_time_add_sealedwait(waited);
  }
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  if (detail::sBufferSize == 0) {
    return;
  }
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < detail::sBufferSize) {
    // NSMBW stage timers: process() is the producer's decode + staging work. The
    // SEALED wait above and any split_staging_batch() between batches stay outside.
    const bool timeProcess = gfx::stage_timers_enabled();
    const auto processStart =
        timeProcess ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto count = process(detail::sBufferData + consumed, detail::sBufferSize - consumed, true);
    if (timeProcess) {
      gfx::stage_time_add_producer(std::chrono::steady_clock::now() - processStart);
    }
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == detail::sBufferSize) break;
    // process returned with its renderer lock released. No recursive drain.
    gfx::split_staging_batch();
    retried = true;
  }
  detail::sBufferSize = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  detail::sBufferSize = 0;
  reset_items();
}

// process() stops at a draw that no longer fits the staging batch and returns where it stopped; submit the batch and
// resume there, as fifo::drain does. Ignoring that return dropped the rest of the list (SPM: back walls and floors of
// big map display lists never drew).
void process_display_list(const uint8_t* data, uint32_t nbytes, bool bigEndian) {
  struct ReplayScope {
    bool prev = g_replayingDisplayList;
    ReplayScope() { g_replayingDisplayList = true; }
    ~ReplayScope() { g_replayingDisplayList = prev; }
  } replayScope;
  struct TimeScope {
    const bool on = gfx::stage_timers_enabled();
    const std::chrono::steady_clock::time_point start =
        on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~TimeScope() {
      if (on) gfx::stage_time_add_dlprocess(std::chrono::steady_clock::now() - start);
    }
  } timeScope;
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < nbytes) {
    const uint32_t count = process(data + consumed, nbytes - consumed, bigEndian);
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("display list draw does not fit after capacity submission");
    consumed += count;
    if (consumed >= nbytes) break;
    gfx::split_staging_batch();
    retried = true;
  }
}

// --- The decode thread -------------------------------------------------------------------------
//
// The game thread records as always and hands what it recorded over in batches at draws, display
// list calls and raw draws once enough has piled up; the decode thread decodes each batch the way
// drain() and GXCallDisplayList would have, in the same order. Anything on the game thread that
// reads or changes decoder state calls sync() first, which hands over what sync mode would have
// decoded by then and waits for it.
namespace {
// A batch is handed over at the next draw once it holds this much. Smaller batches start
// decoding sooner, and less is left to wait for at a sync; each handover costs a lock and, when
// the decode thread is waiting, a wake-up.
constexpr uint32_t kHandoffBytes = 16 * 1024;
constexpr size_t kHandoffItems = 64;
// How far the decode thread may fall behind before recording waits for it.
constexpr size_t kMaxQueuedBatches = 8;
constexpr uint32_t kBatchCapacity = 64 * 1024;
// A batch buffer that grew past this (a huge display list) is freed after decoding, not reused.
constexpr uint32_t kMaxSpareCapacity = 1024 * 1024;
constexpr uint8_t kLoadBpReg = 0x61;

struct Batch {
  uint8_t* data = nullptr;  // malloc'd, like sBufferData, which it was
  uint32_t size = 0;
  uint32_t capacity = 0;
  std::vector<Item> items;
};

struct Decoder {
  std::mutex mutex;
  std::condition_variable workCv; // the decode thread waits here for batches
  std::condition_variable idleCv; // the game thread waits here in sync() and when the queue is full
  std::deque<Batch> queue;
  std::vector<Batch> spare;       // decoded batches, back for the game thread to record into
  std::thread thread;
  bool started = false;
  bool stop = false;
  bool busy = false;           // decoding a batch it took off the queue
  bool decoderWaiting = false; // blocked on workCv
  bool gameWaiting = false;    // blocked on idleCv
};

// Leaked on purpose: a static holding a joinable std::thread ends the process at exit.
Decoder& decoder() {
  static Decoder* const d = new Decoder;
  return *d;
}

std::atomic<bool> g_requested{false};
std::atomic<bool> g_stopping{false};
// What the game thread's waits on the decode thread count under (AuroraGxBeginWaitSite).
AuroraGxWaitSite sWaitSite = AURORA_GX_WAIT_OTHER;
// The decode thread's kernel handle while it runs (Switch), for the guest profiler's sampler.
std::atomic<uint32_t> g_threadHandle{0};

void note_decode_error(const char* what) noexcept {
  static uint32_t s_count = 0; // decode thread only
  if (s_count < 16) {
    ++s_count;
    Log.warn("decode thread: a GX command failed to decode and the rest of its piece was skipped: {}", what);
  }
}

// drain()'s loop, over a stretch of a batch.
void decode_fifo_bytes(const uint8_t* data, uint32_t size) {
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < size) {
    const bool timeProcess = gfx::stage_timers_enabled();
    const auto processStart =
        timeProcess ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto count = process(data + consumed, size - consumed, true);
    if (timeProcess) {
      gfx::stage_time_add_producer(std::chrono::steady_clock::now() - processStart);
    }
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == size) break;
    gfx::split_staging_batch();
    retried = true;
  }
}

void decode_batch(const Batch& batch) {
  ZoneScopedN("GX decode thread batch");
  const bool timed = gfx::stage_timers_enabled();
  const auto waitStart = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  // SEALED, as drain() waits: until the frame worker has begun the frame this batch belongs to,
  // the renderer is its. Gives up when aurora shuts down, since no frame will begin then.
  while (!aurora::wait_for_frame_worker_sealed_for(std::chrono::milliseconds(1))) {
    if (g_stopping.load(std::memory_order_acquire)) return;
  }
  const auto decodeStart = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for (const Item& item : batch.items) {
    const uint8_t* data = batch.data + item.offset;
    try {
      switch (item.kind) {
      case ItemKind::Fifo:
        decode_fifo_bytes(data, item.size);
        break;
      case ItemKind::DisplayList:
        process_display_list(data, item.size, item.bigEndian);
        break;
      case ItemKind::RawDraw:
        decode_raw_draw(static_cast<GXPrimitive>(item.prim), static_cast<GXVtxFmt>(item.fmt), data, item.vtxCount,
                        item.size);
        break;
      }
    } catch (const std::exception& e) {
      note_decode_error(e.what());
    } catch (...) {
      note_decode_error("unknown exception");
    }
  }
  if (timed) {
    gfx::stage_time_add_cpbatch(std::chrono::steady_clock::now() - decodeStart, decodeStart - waitStart, batch.size);
  }
}

void decode_thread_main() noexcept {
#if defined(__SWITCH__)
  // Core 1: the game thread runs on core 0 and the frame worker is pinned to core 2. Same
  // priority as both, so the yield below is what lets other threads on core 1 run.
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 1, 1u << 1);
  g_threadHandle.store(threadGetCurHandle(), std::memory_order_release);
#endif
  // The frame worker waits inside process() service the guest's timing, which only the game
  // thread may do; it does so itself while it waits for this thread.
  aurora::skip_frame_worker_wait_callback_on_this_thread();
  auto& d = decoder();
  Batch batch;
  auto lastBlocked = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::unique_lock lock(d.mutex);
      if (batch.data != nullptr) {
        if (batch.capacity > kMaxSpareCapacity) {
          std::free(batch.data);
          batch.data = nullptr;
          batch.capacity = 0;
        }
        batch.size = 0;
        batch.items.clear();
        d.spare.push_back(std::move(batch));
        batch = {};
      }
      d.busy = false;
      if (d.gameWaiting) {
        d.idleCv.notify_all();
      }
      if (d.queue.empty() && !d.stop) {
        d.decoderWaiting = true;
        d.workCv.wait(lock, [&d] { return d.stop || !d.queue.empty(); });
        d.decoderWaiting = false;
        lastBlocked = std::chrono::steady_clock::now();
      }
      if (d.stop) {
        break;
      }
      batch = std::move(d.queue.front());
      d.queue.pop_front();
      d.busy = true;
    }
#if defined(__SWITCH__)
    // Horizon does not time-slice threads of one priority: one that never blocks keeps its core.
    // After a millisecond of back-to-back batches, let anything else waiting on core 1 run.
    if (std::chrono::steady_clock::now() - lastBlocked > std::chrono::milliseconds(1)) {
      svcSleepThread(YieldType_WithoutCoreMigration);
      lastBlocked = std::chrono::steady_clock::now();
    }
#endif
    decode_batch(batch);
  }
  // Cleared before the thread ends: its handle is closed by the join, and the sampler must not
  // pick it up after that. A sample already in flight fails on the closed handle and is counted.
  g_threadHandle.store(0, std::memory_order_release);
}

bool start_decode_thread() {
  auto& d = decoder();
  std::lock_guard lock(d.mutex);
  if (d.started) {
    return true;
  }
  d.stop = false;
  g_stopping.store(false, std::memory_order_release);
  try {
    d.thread = std::thread(decode_thread_main);
  } catch (const std::system_error& e) {
    Log.warn("decode thread could not start ({}); GX decoding stays on the game thread", e.what());
    return false;
  }
  d.started = true;
  return true;
}

// Game thread: waits on the decode thread until `done`, servicing the guest's timing every
// millisecond as the frame worker waits do, and counts the wait under `site`. `lock` holds the
// decoder's mutex.
template <typename Done>
void wait_for_decoder(std::unique_lock<std::mutex>& lock, AuroraGxWaitSite site, Done done) {
  if (done()) {
    return;
  }
  ZoneScopedN("GX decode thread wait");
  auto& d = decoder();
  const bool timed = gfx::stage_timers_enabled();
  const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for (;;) {
    d.gameWaiting = true;
    const bool finished = d.idleCv.wait_for(lock, std::chrono::milliseconds(1), done);
    d.gameWaiting = false;
    if (finished) {
      break;
    }
    lock.unlock();
    aurora::service_frame_worker_wait_callback();
    lock.lock();
  }
  if (timed) {
    gfx::stage_time_add_cpwait(std::chrono::steady_clock::now() - start, site);
  }
}

// Ends the FIFO stretch being recorded at `end`.
void close_fifo_item(uint32_t end) {
  if (end > sItemsEnd) {
    sItems.push_back({ItemKind::Fifo, true, 0, 0, 0, sItemsEnd, end - sItemsEnd});
    sItemsEnd = end;
  }
}

// Appends to the recorded buffer even while a display list is being recorded.
void append_recorded(const void* data, uint32_t length) {
  if (detail::sBufferSize + length <= detail::sBufferCapacity) {
    std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
    detail::sBufferSize += length;
  } else {
    write_data_grow(data, length);
  }
}

// Hands what is recorded up to the drain point over to the decode thread (everything, with
// `all`), and starts on a fresh buffer. What was recorded after the drain point is not decoded
// yet in sync mode either, so it moves to the front of the fresh buffer. A wait for room in the
// queue counts under `site`.
void hand_off(bool all, AuroraGxWaitSite site) {
  if (all) {
    sDrainMark = detail::sBufferSize;
  }
  close_fifo_item(sDrainMark);
  if (sItems.empty()) {
    return;
  }
  auto& d = decoder();
  Batch next;
  bool wake = false;
  {
    std::unique_lock lock(d.mutex);
    wait_for_decoder(lock, site, [&d] { return d.queue.size() < kMaxQueuedBatches; });
    if (all) { // in case the guest timing serviced during the wait recorded something
      sDrainMark = detail::sBufferSize;
      close_fifo_item(sDrainMark);
    }
    if (!d.spare.empty()) {
      next = std::move(d.spare.back());
      d.spare.pop_back();
    }
    // Copied before the batch is queued: once it is, the decode thread may free it.
    const uint32_t carried = detail::sBufferSize - sDrainMark;
    if (next.data == nullptr || next.capacity < carried) {
      std::free(next.data);
      next.capacity = std::max(kBatchCapacity, carried);
      next.data = static_cast<uint8_t*>(std::malloc(next.capacity));
    }
    if (carried != 0) {
      std::memcpy(next.data, detail::sBufferData + sDrainMark, carried);
    }
    next.size = carried;
    Batch& queued = d.queue.emplace_back();
    queued.data = detail::sBufferData;
    queued.size = sDrainMark;
    queued.capacity = detail::sBufferCapacity;
    queued.items.swap(sItems);
    wake = d.decoderWaiting;
  }
  if (wake) {
    d.workCv.notify_one();
  }
  detail::sBufferData = next.data;
  detail::sBufferSize = next.size;
  detail::sBufferCapacity = next.capacity;
  sItems.swap(next.items);
  reset_items();
}

// Hands over everything recorded and waits until the decode thread has decoded it.
void finish() {
  if (gfx::stage_timers_enabled()) {
    gfx::stage_time_add_cpsync(sWaitSite);
  }
  hand_off(true, sWaitSite);
  auto& d = decoder();
  std::unique_lock lock(d.mutex);
  wait_for_decoder(lock, sWaitSite, [&d] { return d.queue.empty() && !d.busy; });
}

// Called at a draw, display list or raw draw, so the drain point is the end of the buffer.
void hand_off_if_full() {
  if (detail::sBufferSize >= kHandoffBytes || sItems.size() >= kHandoffItems) {
    if (gfx::stage_timers_enabled()) {
      gfx::stage_time_add_cpsync(AURORA_GX_WAIT_QUEUE);
    }
    hand_off(false, AURORA_GX_WAIT_QUEUE);
  }
}
} // namespace

void set_decode_thread_requested(bool on) noexcept { g_requested.store(on, std::memory_order_relaxed); }

bool decode_thread_requested() noexcept { return g_requested.load(std::memory_order_relaxed); }

void apply_decode_thread_request() {
  const bool want = g_requested.load(std::memory_order_relaxed);
  if (want == detail::sAsyncDecode) {
    return;
  }
  if (!want) {
    finish();
    detail::sAsyncDecode = false;
    Log.info("decode thread off: GX commands decode on the game thread");
    return;
  }
  // Called after end_frame's drain, so nothing recorded is left to decode; drain() makes sure.
  drain();
  if (!start_decode_thread()) {
    g_requested.store(false, std::memory_order_relaxed);
    return;
  }
  reset_items();
  detail::sAsyncDecode = true;
  Log.info("decode thread on: GX commands decode on a thread of their own");
}

void sync() {
  if (!detail::sAsyncDecode) {
    return;
  }
  if (gfx::stage_timers_enabled()) {
    gfx::stage_time_add_cpsync(sWaitSite);
  }
  hand_off(false, sWaitSite);
  auto& d = decoder();
  std::unique_lock lock(d.mutex);
  wait_for_decoder(lock, sWaitSite, [&d] { return d.queue.empty() && !d.busy; });
}

void drain_if_pending() {
  if (detail::sAsyncDecode) {
    finish();
  } else if (detail::sBufferSize != 0) {
    drain();
  }
}

void flush() {
  if (!detail::sAsyncDecode) {
    drain();
    return;
  }
  sDrainMark = detail::sBufferSize;
  hand_off_if_full();
}

void queue_display_list(const uint8_t* data, uint32_t nbytes, bool bigEndian) {
  if (data == nullptr || nbytes == 0) {
    return;
  }
  close_fifo_item(detail::sBufferSize);
  sItems.push_back({ItemKind::DisplayList, bigEndian, 0, 0, 0, detail::sBufferSize, nbytes});
  append_recorded(data, nbytes);
  sItemsEnd = detail::sBufferSize;
  sDrainMark = detail::sBufferSize;
  hand_off_if_full();
}

void queue_raw_draw(uint8_t prim, uint8_t fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vertexBytes) {
  close_fifo_item(detail::sBufferSize);
  sItems.push_back({ItemKind::RawDraw, true, prim, fmt, vtxCount, detail::sBufferSize, vertexBytes});
  append_recorded(vertices, vertexBytes);
  sItemsEnd = detail::sBufferSize;
  sDrainMark = detail::sBufferSize;
  hand_off_if_full();
}

void insert_at_drain_point(const void* data, uint32_t length) {
  const uint32_t needed = detail::sBufferSize + length;
  if (needed > detail::sBufferCapacity) {
    const uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
    detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
    detail::sBufferCapacity = newCap;
  }
  uint8_t* at = detail::sBufferData + sDrainMark;
  std::memmove(at + length, at, detail::sBufferSize - sDrainMark);
  std::memcpy(at, data, length);
  detail::sBufferSize = needed;
  sDrainMark += length;
}

void queue_bp_write(uint32_t value) {
  DirectCommand cmd;
  cmd.put8(kLoadBpReg);
  cmd.put32(value);
  insert_at_drain_point(cmd);
}

AuroraGxWaitSite begin_wait_site(AuroraGxWaitSite site) noexcept {
  const AuroraGxWaitSite previous = sWaitSite;
  if (previous == AURORA_GX_WAIT_OTHER) {
    sWaitSite = site;
  }
  return previous;
}

void end_wait_site(AuroraGxWaitSite previous) noexcept { sWaitSite = previous; }

uint32_t decode_thread_handle() noexcept { return g_threadHandle.load(std::memory_order_acquire); }

void stop_decode_thread() noexcept {
  auto& d = decoder();
  {
    std::lock_guard lock(d.mutex);
    if (!d.started) {
      return;
    }
    d.stop = true;
  }
  g_stopping.store(true, std::memory_order_release);
  d.workCv.notify_all();
  if (d.thread.joinable()) {
    d.thread.join();
  }
  std::lock_guard lock(d.mutex);
  d.started = false;
  d.busy = false;
  // What was still queued is dropped with its memory: this runs once, at shutdown.
  d.queue.clear();
  detail::sAsyncDecode = false;
  detail::sBufferSize = 0;
  reset_items();
}

} // namespace aurora::gx::fifo
