#pragma once

// The GX worker's record ring (GX worker design, stage 2).
//
// With the gxworker switch on, guest calls into the GX layer stop running inline at the call. Each
// call that can wait becomes a record: the function to run and a copy of its arguments, plus a copy
// of any small struct an argument points at. The records run back in the same order, either at once
// (Immediate: proves a record carries everything the call needs) or at the next sync point
// (Deferred: proves nothing between the call and its replay depends on it having run). Stage 3 moves
// the replay onto a thread of its own; stage 2 replays on the game thread itself.
//
// A sync point is any native call that isn't queued (it may read or write state the records change),
// the frame boundary (VI_HLE_PresentFrame), and a full ring. The dispatch layer (abi_bridge.h) asks
// KindOf<address> how each override behaves:
//   Queue      void setters with scalar arguments: recorded from the argument registers.
//   SelfDefer  setters whose argument points at a small struct: the body reads it and defers itself
//              through DeferOrRun, so the record holds the struct's bytes rather than its address.
//   Inline     everything else (getters, waits, builders, display list recording, every non-GX
//              native): pending records run first, then the call runs as it always has.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace GxWorker {

enum class Kind : uint8_t { Inline, Queue, SelfDefer };

enum class Mode : uint8_t {
    Off = 0,
    Immediate = 1,  // each record replays as soon as it is written
    Deferred = 2,   // records wait for the next sync point
};

// Nonzero while records are on: the single load every native dispatch pays when the switch is off.
// Changes only at a frame boundary, on the thread that owns the ring.
extern std::atomic<uint8_t> g_engaged;
inline bool Engaged() noexcept { return g_engaged.load(std::memory_order_relaxed) != 0; }

// The mode to use from the next frame boundary on (the dk_diag switches and the settings menu).
void RequestMode(Mode mode) noexcept;
Mode RequestedMode() noexcept;

// Called at the start of every presented frame on the game thread: runs what is pending, then
// applies the requested mode. The first thread to call it owns the ring.
void FrameBoundary();

// True when the caller may append a record now: records are on, this thread owns the ring, no
// replay is running and no display list is being built (GxWorkerRecordingBlocked).
bool CanRecord() noexcept;

// Why pending records had to run: the guest address of the native that needed them (a call that
// isn't queued), or one of these. The [gxworker] lines count drains that had work by cause.
namespace DrainCause {
constexpr uint32_t kOther = 0;
constexpr uint32_t kFrame = 1;           // the frame boundary
constexpr uint32_t kRingFull = 2;        // no room for the next record
constexpr uint32_t kCommit = 3;          // Immediate mode: every record
constexpr uint32_t kFifo = 4;            // a guest FIFO store that couldn't be recorded
constexpr uint32_t kUnnamedNative = 5;   // a native reached without its guest address
constexpr uint32_t kSelfDefer = 6;       // a self-deferring call that couldn't be recorded
}

// Runs every pending record, oldest first. A no-op off the owning thread, with nothing pending, and
// during a replay: a sync point reached from inside a replayed call already has everything recorded
// before that call applied, which is exactly what it would have seen inline.
void Drain(uint32_t cause = DrainCause::kOther);

using ReplayFn = void (*)(void* payload);

// Reserves a record whose payload is `size` bytes aligned to `align` (at most 16), or returns null
// when it can't hold one (the caller then drains and runs the call inline). Construct the payload,
// then Commit(). Only valid when CanRecord().
void* Reserve(ReplayFn replay, size_t size, size_t align);
void Commit();

// Grows the newest record by `extra` bytes when it is the last one written and replays with
// `replay`; returns its payload and sets `*extraOut` to the new bytes. Null otherwise. Lets runs of
// small records (guest FIFO stores) share one header.
void* ExtendLast(ReplayFn replay, size_t extra, void** extraOut);

// Counts for the perftimer's [gxworker] lines, printed with the [calls] lines. `nameOf` names a
// cause's guest address (null: printed as hex).
void PrintPerfWindow(uint64_t firstFrame, uint64_t lastFrame, const char* (*nameOf)(uint32_t) = nullptr);

template <auto Fn, typename Tuple>
void ReplayTuple(void* payload) {
    Tuple* args = static_cast<Tuple*>(payload);
    std::apply(Fn, *args);
    args->~Tuple();
}

// Records a call of Fn with these arguments.
template <auto Fn, typename... A>
bool RecordCall(A... args) {
    using Tuple = std::tuple<A...>;
    void* payload = Reserve(&ReplayTuple<Fn, Tuple>, sizeof(Tuple), alignof(Tuple));
    if (payload == nullptr) {
        return false;
    }
    new (payload) Tuple(args...);
    Commit();
    return true;
}

// A self-deferring call: the dispatch layer arms it, and the body takes the arming when it reaches
// the part that touches GX state. Only one call is ever armed, and only on the owning thread.
extern thread_local bool t_selfDeferArmed;
inline bool TakeSelfDefer() noexcept {
    const bool armed = t_selfDeferArmed;
    t_selfDeferArmed = false;
    return armed;
}
struct SelfDeferScope {
    SelfDeferScope() noexcept { t_selfDeferArmed = true; }
    ~SelfDeferScope() { t_selfDeferArmed = false; }
    SelfDeferScope(const SelfDeferScope&) = delete;
    SelfDeferScope& operator=(const SelfDeferScope&) = delete;
};

template <typename... A>
struct BytesRecord {
    std::tuple<A...> args;
    uint32_t size;
};

template <auto Fn, typename... A>
void ReplayWithBytes(void* payload) {
    auto* record = static_cast<BytesRecord<A...>*>(payload);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(record + 1);
    std::apply([bytes](A... args) { Fn(bytes, args...); }, record->args);
    record->~BytesRecord<A...>();
}

// The GX part of a self-deferring body, Fn(bytes, args...), where `bytes` is the struct an argument
// points at. When armed it is recorded with a copy of those `size` bytes, so the replay reads the
// struct as it was at the call; otherwise it runs now on `bytes` itself.
template <auto Fn, typename... A>
void DeferOrRun(const uint8_t* bytes, uint32_t size, A... args) {
    if (TakeSelfDefer()) {
        if (bytes == nullptr) {
            Drain(DrainCause::kSelfDefer);
            Fn(bytes, args...);
            return;
        }
        using Record = BytesRecord<A...>;
        constexpr size_t kHeader = (sizeof(Record) + 15u) & ~size_t{15};
        void* payload = Reserve(&ReplayWithBytes<Fn, A...>, kHeader + size, alignof(Record) > 16 ? 16 : alignof(Record));
        if (payload != nullptr) {
            auto* record = new (payload) Record{std::tuple<A...>(args...), size};
            uint8_t* copy = reinterpret_cast<uint8_t*>(record + 1);
            for (uint32_t i = 0; i < size; ++i) {
                copy[i] = bytes[i];
            }
            Commit();
            return;
        }
        Drain(DrainCause::kSelfDefer);
    }
    Fn(bytes, args...);
}

template <typename R, typename... A>
constexpr bool ReturnsVoid(R (*)(A...)) {
    return std::is_void_v<R>;
}

template <uint32_t Address>
struct KindOf {
    static constexpr Kind kKind = Kind::Inline;
};

// Which overrides queue and which defer themselves, by guest address.
#include "gx_call_kinds.inc"

// The typed-catalog path (native_cpu_calls.inc): true when the call was recorded or ran through
// its self-deferring body, false when the caller should now run it inline (after a sync point).
template <uint32_t Address, auto Fn, typename... A>
inline bool DispatchCall(A... args) {
    constexpr Kind kind = KindOf<Address>::kKind;
    if constexpr (kind == Kind::Queue) {
        static_assert(ReturnsVoid(Fn), "only void calls can be queued");
        if (CanRecord() && RecordCall<Fn>(args...)) {
            return true;
        }
        Drain(Address);
        return false;
    } else if constexpr (kind == Kind::SelfDefer) {
        if (!CanRecord()) {
            Drain(Address);
            return false;
        }
        SelfDeferScope armed;
        Fn(args...);
        return true;
    } else {
        Drain(Address);
        return false;
    }
}

} // namespace GxWorker

// Defined by the GX layer: true while the game is building a display list (GXBeginDisplayList to
// GXEndDisplayList), when calls write the list into guest memory and so must run in place.
bool GxWorkerRecordingBlocked() noexcept;
