// The GX worker's record ring (gx_worker.h): storage, replay and the mode switch.
//
// One ring, owned by the game thread. Records are packed back to back, each a 16-byte header (the
// replay function and the payload size) followed by its payload, every record starting on a 16-byte
// boundary. Drain replays them oldest first and empties the ring; nothing is ever replayed twice or
// out of order. Stage 3 hands the ring to a thread on core 2; until then every replay runs here.

#include "gx_worker.h"

#include <cstdio>
#include <exception>

namespace GxWorker {

std::atomic<uint8_t> g_engaged{0};
thread_local bool t_selfDeferArmed = false;

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
std::atomic<uint64_t> g_peakBytes{0};  // since the last print; the printer swaps it back to zero

Mode CurrentMode() noexcept { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

constexpr size_t AlignUp(size_t value) { return (value + (kAlign - 1)) & ~(kAlign - 1); }

RecordHeader* HeaderAt(size_t offset) { return reinterpret_cast<RecordHeader*>(g_ring + offset); }

} // namespace

void RequestMode(Mode mode) noexcept { g_requestedMode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed); }

Mode RequestedMode() noexcept { return static_cast<Mode>(g_requestedMode.load(std::memory_order_relaxed)); }

bool CanRecord() noexcept {
    return t_owner && CurrentMode() != Mode::Off && !g_draining && !GxWorkerRecordingBlocked();
}

void Drain() {
    if (!t_owner || g_draining || g_write == 0) {
        return;
    }
    g_draining = true;
    g_drains.Add();
    if (g_write > g_peakBytes.load(std::memory_order_relaxed)) {
        g_peakBytes.store(g_write, std::memory_order_relaxed);
    }
    const bool immediate = CurrentMode() == Mode::Immediate;
    struct Done {
        ~Done() {
            g_write = 0;
            g_lastRecord = kRingBytes;
            g_draining = false;
        }
    } done;
    size_t read = 0;
    const size_t end = g_write;
    while (read < end) {
        RecordHeader* header = HeaderAt(read);
        void* payload = g_ring + read + sizeof(RecordHeader);
        read = AlignUp(read + sizeof(RecordHeader) + header->size);
        if (immediate) {
            // The call it stands for is still on the stack: an exception unwinds into the game
            // exactly where the inline call's would have.
            header->replay(payload);
            continue;
        }
        // Deferred: the game has already run past this call and every later one, so a failure is
        // logged and the rest still replay, as they would have inline.
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

void* Reserve(ReplayFn replay, size_t size, size_t align) {
    if (align > kAlign || size > kRingBytes / 4) {
        return nullptr;
    }
    size_t start = AlignUp(g_write);
    if (start + sizeof(RecordHeader) + size > kRingBytes) {
        g_ringFull.Add();
        Drain();
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
        Drain();
    }
}

void* ExtendLast(ReplayFn replay, size_t extra, void** extraOut) {
    if (g_lastRecord == kRingBytes || g_reserved) {
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
    Drain();
    const Mode requested = RequestedMode();
    const Mode current = CurrentMode();
    if (requested != current) {
        std::fprintf(stderr, "[gxworker] mode %u -> %u\n", static_cast<unsigned>(current), static_cast<unsigned>(requested));
        g_mode.store(static_cast<uint8_t>(requested), std::memory_order_relaxed);
        g_engaged.store(requested != Mode::Off ? 1 : 0, std::memory_order_relaxed);
    }
}

void PrintPerfWindow(uint64_t firstFrame, uint64_t lastFrame) {
    if (lastFrame < firstFrame) {
        return;
    }
    const uint64_t records = g_records.TakeDelta();
    const uint64_t drains = g_drains.TakeDelta();
    const uint64_t full = g_ringFull.TakeDelta();
    const uint64_t errors = g_replayErrors.TakeDelta();
    const uint64_t peak = g_peakBytes.exchange(0, std::memory_order_relaxed);
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
}

} // namespace GxWorker
