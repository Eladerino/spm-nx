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
extern "C" uint32_t threadGetCurHandle(void);  // libnx (switch.h is not included here)
extern "C" void svcSleepThread(int64_t nanoseconds);
// Horizon's address arbiter (libnx svc.h), the futex the gxfutex hand-off sleeps on.
extern "C" uint32_t svcWaitForAddress(void* address, uint32_t arbType, int32_t value, int64_t timeout);
extern "C" uint32_t svcSignalToAddress(void* address, uint32_t signalType, int32_t value, int32_t count);
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
// The engaged span the [dstep] rate divides by: the same intervals the two tick buckets
// take, accumulated in microseconds at the moment each is closed, so ticks, shown frames
// and span always describe the same owned ticks. It grows only while the mode owns the
// frames — a disengagement (switch off, gate shut, yield to the spike) closes the span
// where it stands, and the next engagement's ticks start a fresh span from their own
// first boundary — and PrintDstepWindow takes its per-window delta like the buckets do.
struct SpanMicros {
    std::atomic<uint64_t> micros{0};
    uint64_t printed = 0;
    void Add(uint64_t us) noexcept {
        micros.store(micros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
    }
    uint64_t TakeDelta() noexcept {
        const uint64_t now = micros.load(std::memory_order_relaxed);
        const uint64_t delta = now - printed;
        printed = now;
        return delta;
    }
};
SpanMicros g_dstepSpan;

// ---- the dflushcoal experiment (gx_worker.h) ---------------------------------------------------
// The switch itself arrives through spm_dflushcoal_set (device.cpp's diag_init / diag_set),
// the same extern "C" hook pattern as the spike's. The witness it reads is owner-thread
// state down with the ring's other totals (NoteDcFlushDrained / DcFlushRedundant there).
std::atomic<bool> g_dflushCoalRequested{false};

// ---- the dcnosync experiment (gx_worker.h) -----------------------------------------------------
// The switch arrives through spm_dcnosync_set, the same hook pattern; os_cache.cpp's DcRangeOp
// reads it once per data-cache op and reports what it did through NoteDcNoSync.
std::atomic<bool> g_dcNoSyncRequested{false};

// ---- the invtexq experiment (gx_worker.h, Kind::QueueIfAsked) ----------------------------------
// The switch arrives through spm_invtexq_set; the dispatch layer reads it on every such call.
std::atomic<bool> g_queueIfAskedRequested{false};

// ---- the kpadnosync experiment (gx_worker.h, Kind::NoSyncIfAsked) -----------------------------
std::atomic<bool> g_noSyncIfAskedRequested{false};

// ---- the natnosync experiment (gx_worker.h, NativeSyncPoint) ----------------------------------
std::atomic<bool> g_natNoSyncRequested{false};  // Switch: on by default through diag_init
// This file's own __FILE__ must classify as GX layer, or the classification the registration
// macros rely on is broken in this build tree and the switch must stay inert.
constexpr bool kNatNoSyncClassifies = IsGxLayerFile(__FILE__);

// gxspin (on by default; dk_diag gxspin=0 turns it off): the caught-up worker polls briefly
// before sleeping (WorkerMain). Forwarded through spm_gxspin_set.
std::atomic<bool> g_gxSpinRequested{true};
// gxfutex (off by default; dk_diag gxfutex=1): the caught-up worker sleeps on Horizon's address
// arbiter (Worker::wakeSeq) instead of workCv, so Publish's wake cannot be lost. Switch only;
// elsewhere the switch changes nothing. Forwarded through spm_gxfutex_set.
std::atomic<bool> g_gxFutexRequested{false};

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
//
// The hand-off itself is lock-free: one 64-bit word holds both ends, published (high half, only the
// game thread adds to it) and consumed (low half, only the worker adds to it), so either side reads
// a consistent pair in one load and neither takes the mutex while the other is awake. The mutex and
// the two condition variables are only for sleeping: the worker sleeps when it has caught up, the
// game thread sleeps in a drain. Each sleeper raises its flag under the mutex before its last check,
// and the other side, after its update, notifies only when it sees that flag. All four accesses
// are seq_cst, so either the sleeper's last check sees the update or the updater sees the flag. The
// worker notifies under the mutex, so its wake cannot slip in before the game thread waits. The
// game thread (Publish) notifies without it, since it publishes often and was still queueing on
// the mutex in round 5; a wake it loses that way is caught by the worker's 1 ms bounded wait.
// Before this, every 32 KB publish took the mutex the worker also takes after each batch, and on
// Horizon a contended mutex is a kernel arbitration: 6.7% of the game thread at stock in he2_03
// (the Nov 7 three-thread profile, svcArbitrateLock under GxWorker::Commit).
struct Worker {
    std::mutex mutex;                // sleeping and waking only
    std::condition_variable workCv;  // the game thread published more
    std::condition_variable idleCv;  // the worker caught up with what was published
    std::atomic<uint64_t> ends{0};   // published << 32 | consumed, both byte offsets into the ring
    std::atomic<bool> workerAsleep{false};  // the worker is (about to be) waiting on workCv
    std::atomic<bool> ownerAsleep{false};   // the game thread is (about to be) waiting on idleCv
    // gxfutex: the worker sleeps on this word with Horizon's address arbiter instead of workCv.
    // Publish bumps it before signalling, and the kernel only sleeps the worker while the word
    // still holds the value it read before its last check, so a publish can never slip between
    // that check and the sleep (workCv's unlocked notify can, at a cost of up to 1 ms).
    alignas(4) std::atomic<int32_t> wakeSeq{0};
    bool started = false;
    std::thread thread;
};
constexpr uint32_t PublishedEnd(uint64_t ends) noexcept { return static_cast<uint32_t>(ends >> 32); }
constexpr uint32_t ConsumedEnd(uint64_t ends) noexcept { return static_cast<uint32_t>(ends); }
constexpr bool CaughtUp(uint64_t ends) noexcept { return PublishedEnd(ends) == ConsumedEnd(ends); }
static_assert(kRingBytes <= UINT32_MAX, "ring offsets must fit a half of Worker::ends");
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
// Written by the worker thread only (its own single writer): the time it spent replaying records,
// and the 1 ms yields it made to core 2's other threads. Busy against the frame time says whether
// the worker is saturated (busy close to the frame) or idles while the game thread runs ahead.
Stat g_workerBusyMicros;
Stat g_workerYields;
// The worker's sleeps that ended on the 1 ms timeout with work already published: a wake that
// was lost (workCv's unlocked notify) or never sent. Each one is up to a millisecond the frame
// waited for nothing. gxfutex should hold this at zero.
Stat g_workerLateWakes;

// The dflushcoal witness (gx_worker.h): owner-thread state, kept whether or not the
// switch is on (it records facts, so a mid-frame flip reads a current witness) and read
// only by the skip decision. The drain cause DCFlushRange carries is its guest address,
// the same key the cause table prints its name from.
constexpr uint32_t kDcFlushRangeCause = 0x8026D800u;  // DCFlushRange_801a162c
uint64_t g_dflushFrame = 0;          // the production interval in progress; FrameBoundary advances it
uint64_t g_dflushDrainFrame = 0;     // the interval of the last completed DCFlushRange drain
bool g_dflushDrainSeen = false;      // ... if one has completed at all
uint64_t g_dflushRecordsAtDrain = 0; // the g_records total at that completion
Stat g_dflushCoalesced;              // DCFlushRange drains skipped while the switch was on
Stat g_dcNoSyncFree;                 // dcnosync: data-cache ops that returned without waiting
Stat g_natNoSyncSkips;              // natnosync: non-GX natives that skipped a drain with records pending
Stat g_dcNoSyncCopyDrains;           // dcnosync: ops over a screen copy, which still drained

// Records that a DCFlushRange drain has completed in the current production interval:
// the worker has consumed everything submitted so far, and the record total says what
// "so far" was. Called on Drain's completion paths for the flush's cause only (the
// empty-ring return included: with nothing pending the guarantee the drain exists to
// give already holds).
void NoteDcFlushDrained() noexcept {
    g_dflushDrainSeen = true;
    g_dflushDrainFrame = g_dflushFrame;
    g_dflushRecordsAtDrain = g_records.value.load(std::memory_order_relaxed);
}

// True when a DCFlushRange arriving now would only re-wait for work an earlier flush
// in this same interval already waited out: the switch is on, a flush drain completed
// in this interval, and no record has been submitted since it completed (a Reserve is
// the only way in, and the first record after a drain must Reserve — a drained ring
// has no last record to extend). Nothing else about the call changes: the native body
// still runs, after the records that were actually pending.
bool DcFlushRedundant() noexcept {
    return g_dflushCoalRequested.load(std::memory_order_relaxed) && g_dflushDrainSeen &&
           g_dflushDrainFrame == g_dflushFrame &&
           g_records.value.load(std::memory_order_relaxed) == g_dflushRecordsAtDrain;
}
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

bool DcNoSyncRequested() noexcept { return g_dcNoSyncRequested.load(std::memory_order_relaxed); }

bool QueueIfAskedRequested() noexcept { return g_queueIfAskedRequested.load(std::memory_order_relaxed); }

bool NoSyncIfAskedRequested() noexcept { return g_noSyncIfAskedRequested.load(std::memory_order_relaxed); }

bool NatNoSyncRequested() noexcept {
    return kNatNoSyncClassifies && g_natNoSyncRequested.load(std::memory_order_relaxed);
}

void NoteNatNoSync() noexcept { g_natNoSyncSkips.Add(); }

void NoteDcNoSync(bool drained) noexcept {
    if (drained) {
        g_dcNoSyncCopyDrains.Add();
    } else {
        g_dcNoSyncFree.Add();
    }
}

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

// The worker's Horizon thread handle while it runs, for the guest profiler (0 = none yet):
// read by SpmGxWorkerThreadHandle below, written once by the worker as it starts.
std::atomic<uint32_t> g_workerThreadHandle{0};

// How long the caught-up worker polls before it sleeps (gxspin).
constexpr auto kWorkerSpin = std::chrono::microseconds(1000);

// Polls Worker::ends for up to kWorkerSpin, yielding every turn. True when work arrived. A
// millisecond covers the gap between 32 KB batches while the game draws (250 us did not:
// round 5 saw no change from gxspin), and every turn yields, so core 2's other threads lose
// nothing to it.
bool SpinForWork(Worker& w) {
    const auto until = std::chrono::steady_clock::now() + kWorkerSpin;
    for (uint32_t turn = 0;; ++turn) {
        if (!CaughtUp(w.ends.load(std::memory_order_seq_cst))) {
            return true;
        }
#if defined(__SWITCH__)
        svcSleepThread(-1);  // a yield: returns at once when nothing else on core 2 is ready
#else
        std::this_thread::yield();
#endif
        if ((turn & 7u) == 7u && std::chrono::steady_clock::now() >= until) {
            return false;
        }
    }
}

void WorkerMain() {
    t_worker = true;
#if defined(__SWITCH__)
    g_workerThreadHandle.store(threadGetCurHandle(), std::memory_order_release);
    // Core 2, beside Aurora's frame worker: the game thread has core 0 and the GX decode thread core
    // 1. Horizon does not time-slice threads of one priority, so a long replay yields (below).
    svcSetThreadCoreMask(0xFFFF8000u, 2, 1u << 2);
#endif
    Worker& w = *g_worker;
    auto lastBlocked = std::chrono::steady_clock::now();
    // Wakes a drain waiting for the worker to catch up (Worker's comment has the protocol).
    const auto wakeOwner = [&w] {
        if (w.ownerAsleep.load(std::memory_order_seq_cst)) {
            std::lock_guard lock(w.mutex);
            w.idleCv.notify_all();
        }
    };
    for (;;) {
        const uint64_t ends = w.ends.load(std::memory_order_seq_cst);
        if (CaughtUp(ends)) {
            wakeOwner();
            // gxspin: while the game thread is recording, the next 32 KB arrives a fraction of a
            // millisecond later, and sleeping in between makes every publish a kernel wake (the
            // Nov 7 a01aaf1 profile: 4.6% of the game thread in mutexLock under Publish). So
            // poll for a short while first, yielding to core 2's other threads on every turn
            // (Horizon does not time-slice one priority, so a bare spin would hold Aurora's
            // frame worker off the core).
            if (g_gxSpinRequested.load(std::memory_order_relaxed) && SpinForWork(w)) {
                continue;
            }
#if defined(__SWITCH__)
            if (g_gxFutexRequested.load(std::memory_order_relaxed)) {
                // Read the word first, then announce the sleep, then check once more: a Publish
                // after that check bumps the word, and the kernel refuses to sleep on a changed
                // word (it returns at once), so no wake is lost.
                const int32_t seq = w.wakeSeq.load(std::memory_order_seq_cst);
                w.workerAsleep.store(true, std::memory_order_seq_cst);
                if (CaughtUp(w.ends.load(std::memory_order_seq_cst))) {
                    constexpr uint32_t kArbitrationWaitIfEqual = 2;  // ArbitrationType_WaitIfEqual
                    // Still bounded (a 1 ms backstop, as the condition variable has), and counted
                    // when the timeout finds work waiting.
                    const auto sleepStart = std::chrono::steady_clock::now();
                    svcWaitForAddress(&w.wakeSeq, kArbitrationWaitIfEqual, seq, 1000000);
                    if (!CaughtUp(w.ends.load(std::memory_order_seq_cst)) &&
                        w.wakeSeq.load(std::memory_order_seq_cst) == seq &&
                        std::chrono::steady_clock::now() - sleepStart >= std::chrono::microseconds(900)) {
                        g_workerLateWakes.Add();
                    }
                }
                w.workerAsleep.store(false, std::memory_order_seq_cst);
                lastBlocked = std::chrono::steady_clock::now();
                continue;
            }
#endif
            std::unique_lock lock(w.mutex);
            w.workerAsleep.store(true, std::memory_order_seq_cst);
            // Bounded, as the backstop for Publish's unlocked notify: re-checks at least once a
            // millisecond while asleep. A timeout that finds work waiting is a lost wake: counted.
            while (CaughtUp(w.ends.load(std::memory_order_seq_cst))) {
                if (w.workCv.wait_for(lock, std::chrono::milliseconds(1)) == std::cv_status::timeout &&
                    !CaughtUp(w.ends.load(std::memory_order_seq_cst))) {
                    g_workerLateWakes.Add();
                }
            }
            w.workerAsleep.store(false, std::memory_order_relaxed);
            lastBlocked = std::chrono::steady_clock::now();
            continue;
        }
        const size_t from = ConsumedEnd(ends);
        const size_t to = PublishedEnd(ends);
#if defined(__SWITCH__)
        if (std::chrono::steady_clock::now() - lastBlocked > std::chrono::milliseconds(1)) {
            svcSleepThread(-1);  // YieldType_WithoutCoreMigration: let core 2's other threads run
            lastBlocked = std::chrono::steady_clock::now();
            g_workerYields.Add();
        }
#endif
        const auto replayStart = std::chrono::steady_clock::now();
        ReplayRange(from, to, false);
        g_workerBusyMicros.Add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - replayStart).count()));
        // Only the worker adds to the low half, and it never passes the high half, so no carry.
        const uint64_t after = w.ends.fetch_add(to - from, std::memory_order_seq_cst) + (to - from);
        if (CaughtUp(after)) {
            wakeOwner();
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
    Worker& w = *g_worker;
    w.ends.fetch_add(static_cast<uint64_t>(g_write - g_publishedLocal) << 32, std::memory_order_seq_cst);
    g_publishedLocal = g_write;
    if (w.workerAsleep.load(std::memory_order_seq_cst)) {
        // Notified without the mutex. Taking it here made the game thread queue behind the
        // worker's own sleep and wake on every hand-off (round 5, 11205df: still 6.3% of the
        // game thread in mutexLock under Publish). The price is a wake that can land between
        // the worker's last check and its wait; its wait is a 1 ms wait_for, so that costs at
        // most a millisecond, never a hang.
#if defined(__SWITCH__)
        if (g_gxFutexRequested.load(std::memory_order_relaxed)) {
            constexpr uint32_t kSignalTypeSignal = 0;  // SignalType_Signal
            w.wakeSeq.fetch_add(1, std::memory_order_seq_cst);
            svcSignalToAddress(&w.wakeSeq, kSignalTypeSignal, 0, 1);
            return;
        }
#endif
        w.workCv.notify_one();
    }
}

// Waits until the worker has replayed everything published, servicing the guest's timing about once
// a millisecond as Aurora's own waits do, then rewinds the ring. Returns the microseconds waited
// (the same figure g_waitMs totals), or zero when the worker had already caught up. Reuses the
// start time read below: timing a drain costs no extra clock read.
uint64_t WaitForWorker() {
    Worker& w = *g_worker;
    const auto start = std::chrono::steady_clock::now();
    const auto caughtUp = [&w] { return CaughtUp(w.ends.load(std::memory_order_seq_cst)); };
    bool waited = false;
    if (!caughtUp()) {
        waited = true;
        std::unique_lock lock(w.mutex);
        w.ownerAsleep.store(true, std::memory_order_seq_cst);
        while (!caughtUp()) {
            // The 1 ms timeout is also the backstop: a wake that went astray costs a millisecond.
            if (w.idleCv.wait_for(lock, std::chrono::milliseconds(1), caughtUp)) {
                break;
            }
            if (g_waitService != nullptr) {
                lock.unlock();
                g_waitService();
                lock.lock();
            }
        }
        w.ownerAsleep.store(false, std::memory_order_relaxed);
    }
    // Rewind. The worker has replayed everything published and only adds to `ends` after a
    // replay, so nothing races this store: at most it is reading the caught-up pair on its way to
    // sleep, and the rewound pair is just as caught up.
    w.ends.store(0, std::memory_order_seq_cst);
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
    if (!t_owner) {
        return;
    }
    if (cause == kDcFlushRangeCause && DcFlushRedundant()) {
        // dflushcoal: an earlier flush this frame already drained, and nothing has
        // been recorded since, so there is nothing new for this flush to wait out.
        // Counted apart: never as a drain, never as wait.
        g_dflushCoalesced.Add();
        return;
    }
    if (g_write == 0) {
        if (cause == kDcFlushRangeCause) {
            NoteDcFlushDrained();
        }
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
    } else {
        ReplayRange(0, g_write, CurrentMode() == Mode::Immediate);
    }
    if (cause == kDcFlushRangeCause) {
        // The flush's drain completed (an Immediate replay that throws does not reach
        // this: the drain did not complete, and the witness keeps its earlier word).
        NoteDcFlushDrained();
    }
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
    // The production interval that starts now gets the next frame number: the
    // dflushcoal witness only speaks for the interval its drain completed in. This is
    // the presented-frame boundary every [gxworker] window is counted in — the same
    // one the mode switch below applies at.
    ++g_dflushFrame;
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
            g_dstepSpan.Add(us);  // the span takes exactly the intervals the buckets take
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
    const uint64_t spanMicros = g_dstepSpan.TakeDelta();
    if (sunk.first == 0) {
        return;  // the mode was off (or never in gameplay) for this whole window
    }
    const uint64_t ticks = sunk.first + rendered.first;
    // The engaged span is the ticks' own intervals end to end (accumulated in
    // microseconds as each tick closed, g_dstepSpan), so the rate below is the mode's
    // speed while it was on, untouched by whatever else shared the window. The first
    // hardware print divided a millisecond sum by a million and reported a rate 1000x
    // too high (28078% of full speed for 56 ticks); the span is now counted in the
    // same unit it is divided in, from the same Add calls as the tick buckets, so
    // ticks, shown frames and span cannot drift apart across a disengagement.
    const double spanSec = static_cast<double>(spanMicros) / 1000000.0;
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

extern "C" void spm_vi_retrace_stats(uint64_t* retraces, uint64_t* sleeperWakes, uint64_t* idleNanos);  // vi.cpp

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
    const uint64_t workerBusyMicros = g_workerBusyMicros.TakeDelta();
    const uint64_t workerYields = g_workerYields.TakeDelta();
    const uint64_t workerLateWakes = g_workerLateWakes.TakeDelta();
    const uint64_t coalesced = g_dflushCoalesced.TakeDelta();
    const uint64_t dcFree = g_dcNoSyncFree.TakeDelta();
    const uint64_t dcCopyDrains = g_dcNoSyncCopyDrains.TakeDelta();
    const uint64_t natSkips = g_natNoSyncSkips.TakeDelta();
    uint64_t viRetraces = 0, viSleeperWakes = 0, viIdleNanos = 0;
    spm_vi_retrace_stats(&viRetraces, &viSleeperWakes, &viIdleNanos);
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
    if (workerBusyMicros != 0) {
        // Measurement only: the worker's own replay time (wall clock inside ReplayRange, so a
        // preemption by core 2's other threads counts as busy) and its voluntary yields.
        std::fprintf(stderr,
                     "[gxworker] frames %llu-%llu: worker replayed records %.2f ms a frame, %.1f yields a frame, "
                     "%.2f late wakes a frame (gxfutex %d)\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<double>(workerBusyMicros) / 1000.0 / frames, static_cast<double>(workerYields) / frames,
                     static_cast<double>(workerLateWakes) / frames,
                     g_gxFutexRequested.load(std::memory_order_relaxed) ? 1 : 0);
    }
    if (natSkips != 0) {
        // natnosync's scoreboard: non-GX natives reached with records pending that ran without
        // draining. The wait-by-cause lines below say where the wait landed instead.
        std::fprintf(stderr,
                     "[gxworker] frames %llu-%llu: natnosync ran %.1f non-GX natives a frame without waiting\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<double>(natSkips) / frames);
    }
    if (viRetraces != 0) {
        // Retrace pacing (vi.cpp): about 2 sleeper wakes a frame with the game thread mostly
        // idle means the game paces itself to every other retrace; about 1 means it is late.
        std::fprintf(stderr,
                     "[vi] frames %llu-%llu: %.2f retraces a frame, %.2f found a VIWaitForRetrace sleeper, "
                     "guest idle %.2f ms a frame\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<double>(viRetraces) / frames, static_cast<double>(viSleeperWakes) / frames,
                     static_cast<double>(viIdleNanos) / 1e6 / frames);
    }
    if (coalesced != 0) {
        // dflushcoal's scoreboard (gx_worker.h): flushes that returned without draining
        // in this window. They are in no drain count and no wait figure above or below;
        // the A/B is this count and the DCFlushRange entry in the wait-by-cause lines
        // against the same window with the switch off.
        std::fprintf(stderr,
                     "[gxworker] frames %llu-%llu: dflushcoal skipped %llu DCFlushRange drains (%.1f a frame)\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<unsigned long long>(coalesced), static_cast<double>(coalesced) / frames);
    }
    if (dcFree != 0 || dcCopyDrains != 0) {
        // dcnosync's scoreboard (gx_worker.h): data-cache ops that marked their range dirty
        // without waiting for the worker, and the ones over a screen copy that still waited
        // (those also appear in the drain and wait-by-cause figures, as before).
        std::fprintf(stderr,
                     "[gxworker] frames %llu-%llu: dcnosync ran %llu data-cache ops without waiting (%.1f a frame), "
                     "%llu over screen copies waited as before\n",
                     static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame),
                     static_cast<unsigned long long>(dcFree), static_cast<double>(dcFree) / frames,
                     static_cast<unsigned long long>(dcCopyDrains));
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

// The dflushcoal experiment's hook from the deko diag layer, declared and forwarded
// exactly like the spike's: diag_init hands it the dk_diag.txt value once, diag_set
// forwards every flip. Drain reads it on the flush's own path, so a flip lands on the
// next DCFlushRange.
extern "C" void spm_dflushcoal_set(bool enabled) {
    GxWorker::g_dflushCoalRequested.store(enabled, std::memory_order_relaxed);
}

// The dcnosync experiment's hook from the deko diag layer, declared and forwarded exactly
// like the others: diag_init hands it the dk_diag.txt value once, diag_set forwards every
// flip. DcRangeOp reads it on each data-cache op, so a flip lands on the next one.
extern "C" void spm_dcnosync_set(bool enabled) {
    GxWorker::g_dcNoSyncRequested.store(enabled, std::memory_order_relaxed);
}

// The gxspin switch's hook, the same pattern: the worker reads it each time it catches up.
extern "C" void spm_gxfutex_set(bool enabled) {
    GxWorker::g_gxFutexRequested.store(enabled, std::memory_order_relaxed);
}

extern "C" void spm_gxspin_set(bool enabled) {
    GxWorker::g_gxSpinRequested.store(enabled, std::memory_order_relaxed);
}

// The kpadnosync experiment's hook, the same pattern: NoSyncIfAsked calls read it at dispatch.
extern "C" void spm_kpadnosync_set(bool enabled) {
    GxWorker::g_noSyncIfAskedRequested.store(enabled, std::memory_order_relaxed);
}

// The natnosync experiment's hook, the same pattern: NativeSyncPoint reads it at every non-GX
// native. Logs once if this build tree's file classification fails, since then it stays inert.
extern "C" void spm_natnosync_set(bool enabled) {
    GxWorker::g_natNoSyncRequested.store(enabled, std::memory_order_relaxed);
    if (enabled && !GxWorker::kNatNoSyncClassifies) {
        std::fprintf(stderr, "[gxworker] natnosync stays off: this build's file paths do not classify (%s)\n", __FILE__);
    }
}

// The guest profiler (guest_profiler.cpp) samples the GX worker on the same ticks as the game
// and decode threads; 0 while no worker thread has started.
extern "C" uint32_t SpmGxWorkerThreadHandle(void) {
    return GxWorker::g_workerThreadHandle.load(std::memory_order_acquire);
}

// The invtexq experiment's hook, the same pattern: QueueIfAsked calls read it at dispatch.
extern "C" void spm_invtexq_set(bool enabled) {
    GxWorker::g_queueIfAskedRequested.store(enabled, std::memory_order_relaxed);
}
