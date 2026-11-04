// The GX worker's record ring (gx_worker.h): storage, replay and the mode switch.
//
// One ring, owned by the game thread. Records are packed back to back, each a 16-byte header (the
// replay function and the payload size) followed by its payload, every record starting on a 16-byte
// boundary. Drain replays them oldest first and empties the ring; nothing is ever replayed twice or
// out of order.
//
// Threaded mode (stage 3): the game thread still writes every record, and the worker thread (core
// 2) replays them. The game thread publishes what it has written in batches (Publish); the worker
// replays up to the published end and reports how far it got. A record is only ever touched by one
// thread at a time: the game thread writes and extends records past the published end, the worker
// reads records before it. The ring is linear, never wrapped: Drain waits until the worker has run
// everything, and only then rewinds both ends to the start. Every exchange goes through one mutex,
// a few dozen times a frame, so each thread's writes are visible to the other when it takes over.

#include "gx_worker.h"

#include <dolphin/gx/GXAurora.h>  // AuroraGxSetSink: the gxskip spike's Aurora-side sink

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__SWITCH__)
// libnx's svc wrappers (switch/kernel/svc.h), declared here so this file need not include <switch.h>
// (its global Thread and Mutex types collide with the runtime's in a unity build).
extern "C" uint32_t svcSetThreadCoreMask(uint32_t handle, int32_t coreId, uint32_t affinityMask);
extern "C" void svcSleepThread(int64_t nanoseconds);
#endif

namespace GxWorker {

std::atomic<uint8_t> g_engaged{0};
thread_local bool t_selfDeferArmed = false;

// ---- the gxskip spike (gx_worker.h) -----------------------------------------------------------
// The switch itself arrives through spm_gxskip_set (device.cpp's diag_init / diag_set), the
// same extern "C" hook pattern as the pose and rtt logs. Everything else here is owner-thread
// state, written at presented-frame boundaries in GxSkipPresentedFrame.
std::atomic<bool> g_gxSkipRequested{false};
bool g_frameSunk = false;   // the frame being produced has its GX output discarded
bool g_skipNext = false;    // parity: the next gameplay frame is the sunk one
bool g_haveBoundary = false;
std::chrono::steady_clock::time_point g_lastBoundary{};
// Present-to-present intervals of ended frames, by bucket, for the [gxskip] window line. The
// owner writes; the perftimer's printing thread reads and keeps what it last printed (the Stat
// pattern below, with microseconds in place of a plain count).
struct SkipBucket {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> micros{0};
    uint64_t printedFrames = 0;
    uint64_t printedMicros = 0;
    void Add(uint64_t us) noexcept {
        frames.store(frames.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        micros.store(micros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    }
    // {frames, mean ms} since the last call.
    std::pair<uint64_t, double> TakeDelta() noexcept {
        const uint64_t f = frames.load(std::memory_order_relaxed);
        const uint64_t us = micros.load(std::memory_order_relaxed);
        const uint64_t df = f - printedFrames;
        const uint64_t dus = us - printedMicros;
        printedFrames = f;
        printedMicros = us;
        return {df, df != 0 ? static_cast<double>(dus) / 1000.0 / static_cast<double>(df) : 0.0};
    }
};
SkipBucket g_sunkBucket;
SkipBucket g_renderedBucket;

// ---- the dstep mode (gx_worker.h) -------------------------------------------------------------
// The switch arrives through spm_dstep_set, the same hook pattern as gxskip's. The rest is
// owner-thread state written at presented-frame boundaries in DstepPresentedFrame: while the
// mode owns the frames, presented frames pair up as (sunk tick, rendered tick) and only the
// rendered tick is shown.
std::atomic<bool> g_dstepRequested{false};
bool g_dstepEngaged = false;     // the mode owned the frame now being produced
bool g_dstepSunkNext = true;     // pair parity: the next produced frame is the pair's sunk tick
bool g_dstepCurValid = false;    // the frame that just ended was produced under the mode
bool g_dstepCurSunk = false;     // ... and was its pair's sunk tick
bool g_dstepHaveBoundary = false;
std::chrono::steady_clock::time_point g_dstepLastBoundary{};
uint64_t g_dstepPairSunkMicros = 0;  // the open pair's sunk tick, waiting for its rendered tick
SkipBucket g_dstepSunkBucket;
SkipBucket g_dstepRenderedBucket;
SkipBucket g_dstepShownBucket;  // one entry per completed pair: the pair's whole interval

namespace {

constexpr size_t kRingBytes = size_t{2} << 20;  // 2 MiB: a heavy frame's worth is about 1 MiB
constexpr size_t kAlign = 16;

struct RecordHeader {
    ReplayFn replay;
    uint32_t size;  // payload bytes
    uint32_t reserved;
};
static_assert(sizeof(RecordHeader) == kAlign, "a record's payload starts one alignment unit in");

alignas(kAlign) uint8_t g_ring[kRingBytes];
size_t g_write = 0;              // end of the last record's payload
size_t g_lastRecord = kRingBytes; // offset of the newest record's header, or kRingBytes for none
bool g_reserved = false;         // between Reserve and Commit
bool g_draining = false;
// Written only by the owner at a frame boundary; read by the perftimer's printing thread too.
std::atomic<uint8_t> g_mode{static_cast<uint8_t>(Mode::Off)};
std::atomic<uint8_t> g_requestedMode{static_cast<uint8_t>(Mode::Off)};
thread_local bool t_owner = false;
bool g_hasOwner = false;
thread_local bool t_worker = false;
void (*g_waitService)() = nullptr;

// Threaded mode: the hand-off to the worker thread. Heap-allocated and never freed, so nothing
// destroys a joinable thread at exit (libnx runs static destructors before its own exit hooks).
constexpr size_t kPublishBytes = 32u << 10;  // the game thread hands over at least this much at once
struct Worker {
    std::mutex mutex;
    std::condition_variable workCv;  // the game thread published more
    std::condition_variable idleCv;  // the worker caught up with what was published
    size_t published = 0;            // end of what the worker may replay
    size_t consumed = 0;             // end of what it has replayed
    bool started = false;
    std::thread thread;
};
Worker* g_worker = nullptr;
size_t g_publishedLocal = 0;  // the owner's copy of Worker::published

// Totals for the [gxworker] line. The owner is the only writer (a relaxed load and store, as in
// NativeOverrideCounter); the printing thread reads them and keeps what it last printed.
struct Stat {
    std::atomic<uint64_t> value{0};
    uint64_t printed = 0;
    void Add(uint64_t n = 1) noexcept { value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
    uint64_t TakeDelta() noexcept {
        const uint64_t now = value.load(std::memory_order_relaxed);
        const uint64_t delta = now - printed;
        printed = now;
        return delta;
    }
};
Stat g_records;
Stat g_drains;
Stat g_ringFull;
Stat g_replayErrors;
Stat g_waitMs;           // Threaded: microseconds the game thread waited on the worker, totalled per drain
Stat g_unsafeDrains;     // Threaded: a drain reached while one was already waiting; the call ran unsynchronised
std::atomic<uint64_t> g_peakBytes{0};  // since the last print; the printer swaps it back to zero

// Drains that had work, by cause (DrainCause or a guest address): an open-addressed table the owner
// fills and the printing thread reads. A cause that finds the table full counts as kOther.
constexpr uint32_t kNoCause = 0xFFFFFFFFu;
constexpr size_t kCauseSlots = 512;
struct CauseSlot {
    std::atomic<uint32_t> key{kNoCause};
    Stat count;
    Stat waitMicros;                         // Threaded: this cause's share of the g_waitMs total
    std::atomic<uint64_t> waitMaxMicros{0};  // longest single wait; the printer exchanges it to zero
    void AddWait(uint64_t micros) noexcept {
        waitMicros.Add(micros);
        if (micros > waitMaxMicros.load(std::memory_order_relaxed)) {
            waitMaxMicros.store(micros, std::memory_order_relaxed);
        }
    }
};
CauseSlot g_causes[kCauseSlots];

// Finds the slot a cause counts in, claiming an empty one if needed. Null only when the table is
// full and the cause is not already in it.
CauseSlot* FindCauseSlot(uint32_t cause) noexcept {
    size_t index = (cause * 2654435761u) % kCauseSlots;
    for (size_t probe = 0; probe < kCauseSlots; ++probe) {
        CauseSlot& slot = g_causes[(index + probe) % kCauseSlots];
        const uint32_t key = slot.key.load(std::memory_order_relaxed);
        if (key == cause) {
            return &slot;
        }
        if (key == kNoCause) {
            slot.key.store(cause, std::memory_order_release);
            return &slot;
        }
    }
    return nullptr;
}

// Counts a drain under its cause and returns the slot it counted in (a full table folds the cause
// into kOther's slot, as before; null only when even that has no slot, in which case the drain is
// not counted at all, also as before).
CauseSlot* CountCause(uint32_t cause) noexcept {
    CauseSlot* slot = FindCauseSlot(cause);
    if (slot == nullptr && cause != DrainCause::kOther) {
        slot = FindCauseSlot(DrainCause::kOther);
    }
    if (slot != nullptr) {
        slot->count.Add();
    }
    return slot;
}

const char* FixedCauseName(uint32_t cause) {
    switch (cause) {
    case DrainCause::kOther: return "(other)";
    case DrainCause::kFrame: return "(frame end)";
    case DrainCause::kRingFull: return "(ring full)";
    case DrainCause::kCommit: return "(immediate)";
    case DrainCause::kFifo: return "(FIFO store)";
    case DrainCause::kUnnamedNative: return "(native)";
    case DrainCause::kSelfDefer: return "(self-defer)";
    default: return nullptr;
    }
}

// The [gxworker] cause lines list at most this many, this many to a line.
constexpr size_t kCauseMaxEntries = 24;
constexpr size_t kCausesPerLine = 6;

Mode CurrentMode() noexcept { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

constexpr size_t AlignUp(size_t value) { return (value + (kAlign - 1)) & ~(kAlign - 1); }

RecordHeader* HeaderAt(size_t offset) { return reinterpret_cast<RecordHeader*>(g_ring + offset); }

} // namespace

void RequestMode(Mode mode) noexcept { g_requestedMode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed); }

Mode RequestedMode() noexcept { return static_cast<Mode>(g_requestedMode.load(std::memory_order_relaxed)); }

bool CanRecord() noexcept {
    // gxskip: a sunk frame records nothing (gx_worker.h); its calls run inline into the sink.
    return t_owner && CurrentMode() != Mode::Off && !g_draining && !g_frameSunk && !GxWorkerRecordingBlocked();
}

bool Pending() noexcept { return t_owner && g_write != 0; }

namespace {

// Replays the records in [from, to). Immediate: an exception reaches the caller, as the inline
// call's would have; the rest log it and go on, since the game has already run past the call.
void ReplayRange(size_t from, size_t to, bool immediate) {
    size_t read = AlignUp(from);  // `from` is a payload end (a published end); the next record is aligned
    while (read < to) {
        RecordHeader* header = HeaderAt(read);
        void* payload = g_ring + read + sizeof(RecordHeader);
        read = AlignUp(read + sizeof(RecordHeader) + header->size);
        if (immediate) {
            header->replay(payload);
            continue;
        }
        try {
            header->replay(payload);
        } catch (const std::exception& e) {
            g_replayErrors.Add();
            std::fprintf(stderr, "[gxworker] a deferred GX call threw: %s\n", e.what());
        } catch (...) {
            g_replayErrors.Add();
            std::fprintf(stderr, "[gxworker] a deferred GX call threw\n");
        }
    }
}

void WorkerMain() {
    t_worker = true;
#if defined(__SWITCH__)
    // Core 2, beside Aurora's frame worker: the game thread has core 0 and the GX decode thread core
    // 1. Horizon does not time-slice threads of one priority, so a long replay yields (below).
    svcSetThreadCoreMask(0xFFFF8000u, 2, 1u << 2);
#endif
    Worker& w = *g_worker;
    auto lastBlocked = std::chrono::steady_clock::now();
    for (;;) {
        size_t from = 0;
        size_t to = 0;
        {
            std::unique_lock lock(w.mutex);
            if (w.consumed == w.published) {
                w.idleCv.notify_all();
                w.workCv.wait(lock, [&w] { return w.published != w.consumed; });
                lastBlocked = std::chrono::steady_clock::now();
            }
            from = w.consumed;
            to = w.published;
        }
#if defined(__SWITCH__)
        if (std::chrono::steady_clock::now() - lastBlocked > std::chrono::milliseconds(1)) {
            svcSleepThread(-1);  // YieldType_WithoutCoreMigration: let core 2's other threads run
            lastBlocked = std::chrono::steady_clock::now();
        }
#endif
        ReplayRange(from, to, false);
        {
            std::lock_guard lock(w.mutex);
            w.consumed = to;
            if (w.consumed == w.published) {
                w.idleCv.notify_all();
            }
        }
    }
}

void StartWorker() {
    if (g_worker == nullptr) {
        g_worker = new Worker;
    }
    if (g_worker->started) {
        return;
    }
    try {
        g_worker->thread = std::thread(WorkerMain);
        g_worker->started = true;
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "[gxworker] worker thread could not start (%s); records replay on the game thread\n",
                     e.what());
    }
}

bool Threaded() noexcept { return CurrentMode() == Mode::Threaded && g_worker != nullptr && g_worker->started; }

// Hands everything written so far to the worker.
void Publish() {
    if (g_write == g_publishedLocal) {
        return;
    }
    {
        std::lock_guard lock(g_worker->mutex);
        g_worker->published = g_write;
    }
    g_publishedLocal = g_write;
    g_worker->workCv.notify_one();
}

// Waits until the worker has replayed everything published, servicing the guest's timing about once
// a millisecond as Aurora's own waits do, then rewinds the ring. Returns the microseconds waited
// (the same figure g_waitMs totals), or zero when the worker had already caught up. Reuses the
// start time read below: timing a drain costs no extra clock read.
uint64_t WaitForWorker() {
    Worker& w = *g_worker;
    const auto start = std::chrono::steady_clock::now();
    bool waited = false;
    std::unique_lock lock(w.mutex);
    while (w.consumed != w.published) {
        waited = true;
        if (w.idleCv.wait_for(lock, std::chrono::milliseconds(1), [&w] { return w.consumed == w.published; })) {
            break;
        }
        if (g_waitService != nullptr) {
            lock.unlock();
            g_waitService();
            lock.lock();
        }
    }
    w.consumed = 0;
    w.published = 0;
    lock.unlock();
    g_publishedLocal = 0;
    if (waited) {
        const uint64_t micros =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        g_waitMs.Add(micros);
        return micros;
    }
    return 0;
}

} // namespace

bool OnWorkerThread() noexcept { return t_worker; }

void SetWaitService(void (*service)()) noexcept { g_waitService = service; }

void Drain(uint32_t cause) {
    if (!t_owner || g_write == 0) {
        return;
    }
    if (g_draining) {
        // A sync point reached while the game thread waits on the worker (from the timing service):
        // it cannot wait twice, so the call runs alongside the worker. Counted so a log shows it.
        if (Threaded()) {
            g_unsafeDrains.Add();
        }
        return;
    }
    g_draining = true;
    g_drains.Add();
    CauseSlot* causeSlot = CountCause(cause);
    if (g_write > g_peakBytes.load(std::memory_order_relaxed)) {
        g_peakBytes.store(g_write, std::memory_order_relaxed);
    }
    struct Done {
        ~Done() {
            g_write = 0;
            g_lastRecord = kRingBytes;
            g_draining = false;
        }
    } done;
    if (Threaded()) {
        Publish();
        const uint64_t waitedMicros = WaitForWorker();
        if (causeSlot != nullptr && waitedMicros != 0) {
            causeSlot->AddWait(waitedMicros);
        }
        return;
    }
    ReplayRange(0, g_write, CurrentMode() == Mode::Immediate);
}

void* Reserve(ReplayFn replay, size_t size, size_t align) {
    if (align > kAlign || size > kRingBytes / 4) {
        return nullptr;
    }
    size_t start = AlignUp(g_write);
    if (start + sizeof(RecordHeader) + size > kRingBytes) {
        g_ringFull.Add();
        Drain(DrainCause::kRingFull);
        start = 0;
        if (g_draining) {
            return nullptr;
        }
    }
    RecordHeader* header = HeaderAt(start);
    header->replay = replay;
    header->size = static_cast<uint32_t>(size);
    header->reserved = 0;
    g_lastRecord = start;
    g_write = start + sizeof(RecordHeader) + size;
    g_reserved = true;
    g_records.Add();
    return g_ring + start + sizeof(RecordHeader);
}

void Commit() {
    g_reserved = false;
    if (CurrentMode() == Mode::Immediate) {
        Drain(DrainCause::kCommit);
    } else if (Threaded() && g_write - g_publishedLocal >= kPublishBytes) {
        Publish();
    }
}

void* ExtendLast(ReplayFn replay, size_t extra, void** extraOut) {
    // A published record may be replaying on the worker right now: only an unpublished one grows.
    if (g_lastRecord == kRingBytes || g_reserved || g_lastRecord < g_publishedLocal) {
        return nullptr;
    }
    RecordHeader* header = HeaderAt(g_lastRecord);
    if (header->replay != replay || g_write + extra > kRingBytes) {
        return nullptr;
    }
    *extraOut = g_ring + g_write;
    header->size += static_cast<uint32_t>(extra);
    g_write += extra;
    return g_ring + g_lastRecord + sizeof(RecordHeader);
}

void FrameBoundary() {
    if (!g_hasOwner) {
        g_hasOwner = true;
        t_owner = true;
    }
    if (!t_owner || g_draining) {
        return;
    }
    Drain(DrainCause::kFrame);
    const Mode requested = RequestedMode();
    const Mode current = CurrentMode();
    if (requested != current) {
        if (requested == Mode::Threaded) {
            StartWorker();  // the ring is empty and the worker idle until the first publish
        }
        std::fprintf(stderr, "[gxworker] mode %u -> %u\n", static_cast<unsigned>(current), static_cast<unsigned>(requested));
        g_mode.store(static_cast<uint8_t>(requested), std::memory_order_relaxed);
        g_engaged.store(requested != Mode::Off ? 1 : 0, std::memory_order_relaxed);
    }
}

// The fps30 cap's gate, defined by the runtime (spm_prepare_graphics.cpp) and declared the
// same way in the deko present path: true only in a map with no Prepare sweep running.
extern "C" bool spm_gameplay_present();

bool GxSkipSunk() noexcept { return g_frameSunk; }

// True while the dstep mode (gx_worker.h), not the spike, owns the presented frames: the
// mode is requested, the spike is not (both on: dstep yields — the two alternations would
// stack into a stream with no shown frames at all), and the gameplay gate is open. Read at
// presented-frame boundaries on the owner thread; the gate cannot move between the two
// boundary hooks of the same frame, so both hooks see the same answer.
bool DstepOwnsFrames() {
    return g_dstepRequested.load(std::memory_order_relaxed) &&
           !g_gxSkipRequested.load(std::memory_order_relaxed) && spm_gameplay_present();
}

void GxSkipPresentedFrame() {
    if (!t_owner) {
        return;
    }
    if (DstepOwnsFrames()) {
        return;  // dstep's boundary hook (called just after this one) does the accounting
                 // and the sink decision for these frames; the [gxskip] buckets stay the
                 // spike's own, so its line never reports another mode's ticks.
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_haveBoundary) {
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - g_lastBoundary).count());
        (g_frameSunk ? g_sunkBucket : g_renderedBucket).Add(us);
    }
    g_lastBoundary = now;
    g_haveBoundary = true;
    // The next frame's sink state. FrameBoundary has just drained the ring, and the copy that
    // ended this frame already waited for the decode thread (GXCopyDisp's GXDrawDone), so
    // flipping Aurora's sink here cannot catch the frame that just ended mid-replay.
    if (!g_gxSkipRequested.load(std::memory_order_relaxed) || !spm_gameplay_present()) {
        g_frameSunk = false;
        g_skipNext = false;  // parity reset: the first sunk frame is never the first frame back
    } else {
        g_frameSunk = g_skipNext;
        g_skipNext = !g_skipNext;
    }
    AuroraGxSetSink(g_frameSunk);
}

void PrintGxSkipWindow(uint64_t firstFrame, uint64_t lastFrame) {
    if (lastFrame < firstFrame) {
        return;
    }
    const auto sunk = g_sunkBucket.TakeDelta();
    const auto rendered = g_renderedBucket.TakeDelta();
    if (sunk.first == 0) {
        return;  // the spike was off (or never in gameplay) for this whole window
    }
    std::fprintf(stderr,
                 "[gxskip] frames %llu-%llu: %llu sunk ticks at %.2f ms mean, %llu rendered ticks at %.2f ms "
                 "mean (present-to-present intervals, game thread)\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned long long>(sunk.first), sunk.second,
                 static_cast<unsigned long long>(rendered.first), rendered.second);
}

void DstepPresentedFrame() {
    if (!t_owner) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (DstepOwnsFrames()) {
        // Close the ended tick's interval. The first boundary after the mode engages has
        // nothing of the mode's to close: the frame that just ended was produced before
        // it (g_dstepCurValid is false), and the frame that starts now is the pair's
        // first, sunk, tick.
        if (g_dstepEngaged && g_dstepHaveBoundary && g_dstepCurValid) {
            const uint64_t us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - g_dstepLastBoundary).count());
            if (g_dstepCurSunk) {
                g_dstepSunkBucket.Add(us);
                g_dstepPairSunkMicros = us;  // the pair stays open until its rendered tick
            } else {
                g_dstepRenderedBucket.Add(us);
                g_dstepShownBucket.Add(g_dstepPairSunkMicros + us);
                g_dstepPairSunkMicros = 0;
            }
        }
        g_dstepEngaged = true;
        g_dstepCurSunk = g_dstepSunkNext;
        g_dstepSunkNext = !g_dstepSunkNext;
        g_dstepCurValid = true;
        // The sink decision for the frame that starts now. FrameBoundary has just drained
        // the ring and the ended frame's copy already waited for the decode thread, the
        // same ordering the spike's flip relies on. GxSkipPresentedFrame ran just before
        // this hook and stood down (DstepOwnsFrames), so this is the boundary's only
        // decision.
        g_frameSunk = g_dstepCurSunk;
        AuroraGxSetSink(g_frameSunk);
    } else {
        // Off, gate shut (title, loads, map changes, saves, the Prepare sweep), or yielded
        // to the spike: the pair state resets, so the first frame back starts a pair and a
        // half-finished pair is dropped uncounted. There is no sink to undo here:
        // GxSkipPresentedFrame has already settled this boundary's sink for every
        // non-dstep case, and a tick in production keeps the state it started with, the
        // same rule the spike's flips follow.
        g_dstepEngaged = false;
        g_dstepSunkNext = true;
        g_dstepCurValid = false;
        g_dstepPairSunkMicros = 0;
    }
    g_dstepLastBoundary = now;
    g_dstepHaveBoundary = true;
}

void PrintDstepWindow(uint64_t firstFrame, uint64_t lastFrame) {
    if (lastFrame < firstFrame) {
        return;
    }
    const auto sunk = g_dstepSunkBucket.TakeDelta();
    const auto rendered = g_dstepRenderedBucket.TakeDelta();
    const auto shown = g_dstepShownBucket.TakeDelta();
    if (sunk.first == 0) {
        return;  // the mode was off (or never in gameplay) for this whole window
    }
    const uint64_t ticks = sunk.first + rendered.first;
    // The engaged span is the ticks' own intervals end to end, so the rate below is the
    // mode's speed while it was on, untouched by whatever else shared the window.
    const double spanSec = (sunk.second * static_cast<double>(sunk.first) +
                            rendered.second * static_cast<double>(rendered.first)) /
                           1000.0 / 1000.0;
    const double tickRate = spanSec > 0.0 ? static_cast<double>(ticks) / spanSec : 0.0;
    std::fprintf(stderr,
                 "[dstep] frames %llu-%llu: %llu shown frames, %llu guest ticks (%.1f ticks/s, %.0f%% of full "
                 "speed); sunk ticks at %.2f ms mean, rendered ticks at %.2f ms mean, shown frames at %.2f ms "
                 "mean (%.1f shown fps) (intervals over the engaged span, game thread)\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned long long>(shown.first), static_cast<unsigned long long>(ticks), tickRate,
                 tickRate / 60.0 * 100.0, sunk.second, rendered.second, shown.second,
                 shown.second > 0.0 ? 1000.0 / shown.second : 0.0);
}

void PrintPerfWindow(uint64_t firstFrame, uint64_t lastFrame, const char* (*nameOf)(uint32_t)) {
    if (lastFrame < firstFrame) {
        return;
    }
    const uint64_t records = g_records.TakeDelta();
    const uint64_t drains = g_drains.TakeDelta();
    const uint64_t full = g_ringFull.TakeDelta();
    const uint64_t errors = g_replayErrors.TakeDelta();
    const uint64_t peak = g_peakBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t waitMicros = g_waitMs.TakeDelta();
    const uint64_t unsafeDrains = g_unsafeDrains.TakeDelta();
    const Mode mode = CurrentMode();
    if (mode == Mode::Off && records == 0) {
        return;
    }
    const double frames = static_cast<double>(lastFrame - firstFrame + 1);
    std::fprintf(stderr,
                 "[gxworker] frames %llu-%llu: mode %u, %.0f records and %.0f drains a frame, at most %llu KB "
                 "pending, ring full %llu times, %llu replay errors\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned>(mode), static_cast<double>(records) / frames,
                 static_cast<double>(drains) / frames, static_cast<unsigned long long>(peak / 1024),
                 static_cast<unsigned long long>(full), static_cast<unsigned long long>(errors));
    if (mode == Mode::Threaded || waitMicros != 0 || unsafeDrains != 0) {
        std::fprintf(stderr,
                     "[gxworker] frames %llu-%llu: game thread waited on the worker %.2f ms a frame, %llu unsynchronised "
                     "drains\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<double>(waitMicros) / 1000.0 / frames, static_cast<unsigned long long>(unsafeDrains));
    }

    // What made pending records run, busiest first, as drains a frame. Immediate mode's per-record
    // drains are left out: they say nothing about what the worker will have to wait for. The wait
    // deltas are taken for every claimed slot here (kCommit included), so each window starts clean
    // even for a cause the lines leave out.
    struct CauseWindow {
        uint64_t count;
        uint64_t waitMicros;
        uint64_t waitMaxMicros;
        uint32_t key;
    };
    std::vector<CauseWindow> causes;
    for (CauseSlot& slot : g_causes) {
        const uint32_t key = slot.key.load(std::memory_order_acquire);
        if (key == kNoCause) {
            continue;
        }
        CauseWindow window{slot.count.TakeDelta(), slot.waitMicros.TakeDelta(),
                           slot.waitMaxMicros.exchange(0, std::memory_order_relaxed), key};
        if (window.count != 0 && key != DrainCause::kCommit) {
            causes.push_back(window);
        }
    }
    if (causes.empty()) {
        return;
    }
    char hex[16];
    const auto nameFor = [&hex, nameOf](uint32_t key) -> const char* {
        const char* name = FixedCauseName(key);
        if (name == nullptr && nameOf != nullptr) {
            name = nameOf(key);
        }
        if (name == nullptr) {
            std::snprintf(hex, sizeof(hex), "%08X", key);
            name = hex;
        }
        return name;
    };
    std::sort(causes.begin(), causes.end(),
              [](const CauseWindow& a, const CauseWindow& b) { return a.count > b.count; });
    if (causes.size() > kCauseMaxEntries) {
        causes.resize(kCauseMaxEntries);
    }
    std::fprintf(stderr, "[gxworker] frames %llu-%llu: drains a frame by cause, busiest first:\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame));
    std::string line;
    for (size_t i = 0; i < causes.size(); ++i) {
        char item[160];
        std::snprintf(item, sizeof(item), "%s%s %.1f", line.empty() ? "" : ", ", nameFor(causes[i].key),
                      static_cast<double>(causes[i].count) / frames);
        line += item;
        if ((i + 1) % kCausesPerLine == 0 || i + 1 == causes.size()) {
            std::fprintf(stderr, "[gxworker]   %s\n", line.c_str());
            line.clear();
        }
    }

    // How long those drains kept the game thread waiting on the worker, busiest first, as ms a
    // frame; max is the longest single wait in ms. The entries sum to the window's share of the
    // "game thread waited on the worker" line above, so the two lines cross-check.
    std::vector<const CauseWindow*> waits;
    for (const CauseWindow& window : causes) {
        if (window.waitMicros != 0) {
            waits.push_back(&window);
        }
    }
    if (waits.empty()) {
        return;
    }
    std::sort(waits.begin(), waits.end(),
              [](const CauseWindow* a, const CauseWindow* b) { return a->waitMicros > b->waitMicros; });
    std::fprintf(stderr,
                 "[gxworker] frames %llu-%llu: drain wait ms a frame by cause, busiest first (max = longest single wait, ms):\n",
                 static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame));
    for (size_t i = 0; i < waits.size(); ++i) {
        char item[192];
        std::snprintf(item, sizeof(item), "%s%s %.2f (max %.2f)", line.empty() ? "" : ", ",
                      nameFor(waits[i]->key), static_cast<double>(waits[i]->waitMicros) / 1000.0 / frames,
                      static_cast<double>(waits[i]->waitMaxMicros) / 1000.0);
        line += item;
        if ((i + 1) % kCausesPerLine == 0 || i + 1 == waits.size()) {
            std::fprintf(stderr, "[gxworker]   %s\n", line.c_str());
            line.clear();
        }
    }
}

} // namespace GxWorker

// The gxskip switch's hook from the deko diag layer (switch/deko/device.cpp), declared there
// the same way as this runtime's other hooks (spm_pose_log_set, spm_rtt_log_set): diag_init
// hands it the dk_diag.txt value once, diag_set forwards every flip. The frame boundary does
// the rest (GxSkipPresentedFrame).
extern "C" void spm_gxskip_set(bool enabled) {
    GxWorker::g_gxSkipRequested.store(enabled, std::memory_order_relaxed);
}

// The dstep mode's hook from the deko diag layer, declared and forwarded exactly like the
// spike's (spm_gxskip_set above): diag_init hands it the dk_diag.txt value once, diag_set
// forwards every flip. The frame boundary does the rest (DstepPresentedFrame).
extern "C" void spm_dstep_set(bool enabled) {
    GxWorker::g_dstepRequested.store(enabled, std::memory_order_relaxed);
}
