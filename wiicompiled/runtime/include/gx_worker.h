#pragma once

// The GX worker's record ring (GX worker design, stage 2).
//
// With the gxworker switch on, guest calls into the GX layer stop running inline at the call. Each
// call that can wait becomes a record: the function to run and a copy of its arguments, plus a copy
// of any small struct an argument points at. The records run back in the same order, either at once
// (Immediate: proves a record carries everything the call needs) or at the next sync point
// (Deferred: proves nothing between the call and its replay depends on it having run), or on a
// thread of their own on core 2 (Threaded, stage 3): the game thread runs on while they replay, and a
// sync point waits for that thread to catch up.
//
// A sync point is any native call that isn't queued (it may read or write state the records change),
// the frame boundary (VI_HLE_PresentFrame), and a full ring. The dispatch layer (abi_bridge.h) asks
// KindOf<address> how each override behaves:
//   Queue      void setters with scalar arguments: recorded from the argument registers.
//   SelfDefer  setters whose argument points at a small struct: the body reads it and defers itself
//              through DeferOrRun, so the record holds the struct's bytes rather than its address.
//   NoSync     calls that touch no state a record changes (a getter answered from a game-side
//              copy, a builder that only fills in a guest struct): they run in place, and
//              pending records stay pending.
//   Inline     everything else (other getters, waits, display list recording, every non-GX
//              native): pending records run first, then the call runs as it always has.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace GxWorker {

enum class Kind : uint8_t { Inline, Queue, SelfDefer, NoSync, QueueIfAsked };

// QueueIfAsked: a call that qualifies for Queue but is held back behind the invtexq experiment
// (Switch Diagnostics "invtexq"): Queue while the switch is on, Inline while it is off.
bool QueueIfAskedRequested() noexcept;

enum class Mode : uint8_t {
    Off = 0,
    Immediate = 1,  // each record replays as soon as it is written
    Deferred = 2,   // records wait for the next sync point
    Threaded = 3,   // records replay on the worker thread; a sync point waits for it to finish
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

// Runs every pending record, oldest first (Threaded: hands them to the worker and waits until it has
// run them all). A no-op off the owning thread, with nothing pending, and during a replay: a sync
// point reached from inside a replayed call already has everything recorded before that call
// applied, which is exactly what it would have seen inline.
void Drain(uint32_t cause = DrainCause::kOther);

// True on the worker thread. Code that must only run on the game thread (servicing the guest's
// VI and alarm timing) checks it: replayed calls that service timing inline skip it there, since
// the game thread is not stuck behind them.
bool OnWorkerThread() noexcept;

// What a Threaded drain calls about once a millisecond while it waits for the worker, on the game
// thread: the same guest timing service Aurora's frame waits use (main.cpp).
void SetWaitService(void (*service)()) noexcept;

// True when records are waiting on this thread: what a NoSync getter checks before answering from
// its game-side copy rather than from the state the records have yet to change.
bool Pending() noexcept;

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

// ---- the gxskip spike (Switch Diagnostics "gxskip"; state in gx_worker.cpp) --------------------
//
// A measurement, not a feature: while the switch is on, alternate gameplay frames (the gate is
// the fps30 cap's: in a map, no Prepare sweep running) have their GX output discarded. Nothing
// is recorded for a sunk frame, the FIFO sink in Aurora drops what its calls write, display
// lists are not walked, and the frame's picture is not shown (the present divider in
// aurora_switch_dk_present). The guest tick itself runs in full — input, audio, game logic —
// so the perftimer prices a tick whose GX costs (nearly) nothing: L, the number that decides
// whether a double-step full-speed mode is worth building. Sunk frames alternate with rendered
// ones starting rendered, and the parity resets whenever the switch is off or the gate shuts.

// True while the frame being produced is a sunk one. Owner (game) thread only, like CanRecord.
bool GxSkipSunk() noexcept;

// Called from VI_HLE_PresentFrame for every presented (XFB) frame, just after FrameBoundary:
// closes the present-to-present interval of the frame that just ended into the sunk or rendered
// bucket, decides the next frame's sink state, and hands it to Aurora's FIFO sink. The interval
// includes the ended frame's own pace wait; in a heavy scene that wait is zero (the frame
// missed its retrace), which is the case this spike measures.
void GxSkipPresentedFrame();

// The [gxskip] line for a perftimer window, printed with the [calls] lines: how many ticks in
// the window were sunk, and the mean present-to-present interval of the sunk and the rendered
// ticks in the same window, so L is read off one log with its own control group.
void PrintGxSkipWindow(uint64_t firstFrame, uint64_t lastFrame);
// ---- the gxdedup experiment (Switch Diagnostics "gxdedup"; state in gx_vertex.cpp) -----------
//
// While the switch is on, the vertex setters (GXSetVtxDesc / GXSetVtxAttrFmt / GXSetArray)
// skip re-emitting a value Aurora's vertex state already holds — never while a display
// list is being recorded (GxWorkerRecordingBlocked), where the call is list content. The
// [gxdedup] line for a perftimer window, printed with the [calls] lines: skipped/total
// calls per setter family while the switch was on, which is the no-change share the
// experiment exists to measure. Prints nothing while nothing was counted.
void PrintGxDedupWindow(uint64_t firstFrame, uint64_t lastFrame);

// ---- the dstep mode (Switch Diagnostics "dstep"; state in gx_worker.cpp) ----------------------
//
// The double-step full-speed mode, built on the gxskip sink: while the switch is on and the
// same gameplay gate is open, the guest runs TWO ticks per shown frame. The first tick of
// each pair is sunk — its GX output dies at the gxskip doors and its present is held by the
// dstep divider in aurora_switch_dk_present — and the second tick renders and shows. Exactly
// two ticks per shown frame, never more: there is no catch-up accumulator, so a slow pair
// just takes longer. The game therefore advances at up to twice the shown rate; whether it
// reaches full speed is the rendered tick's price, and the [dstep] window line reports the
// tick rate and game-speed percentage it actually reached.
//
// The pair driver lives at the same presented-frame boundary as the spike
// (DstepPresentedFrame, called just after GxSkipPresentedFrame) and has the last word on the
// sink there; when it owns the frames, the spike's hook stands down entirely. Pair state
// resets whenever the switch is off, the gate shuts (title, loads, map changes, saves, the
// Prepare sweep) or the mode disengages, so the first frame back always starts a pair with a
// sunk tick and the sink never leaks across a boundary it should not. If gxskip is on too,
// dstep yields: the spike owns the frames and dstep does nothing (the two alternations
// would otherwise stack into a stream with no shown frames at all). While dstep is on, the
// fps30 present divider stands down at its use site instead.

// Called from VI_HLE_PresentFrame for every presented (XFB) frame, just after
// GxSkipPresentedFrame: closes the ended tick's interval into the sunk or rendered bucket
// (a completed pair also closes a shown-frame interval), decides the next tick's sink
// state, and hands it to Aurora's FIFO sink.
void DstepPresentedFrame();

// The [dstep] line for a perftimer window, printed with the [calls] lines: shown frames,
// guest ticks, the effective tick rate and game speed as a percentage of 60 ticks/s over
// the engaged span, and the sunk / rendered / shown mean intervals. Prints nothing while
// no tick in the window was sunk by the mode.
void PrintDstepWindow(uint64_t firstFrame, uint64_t lastFrame);

// ---- the dflushcoal experiment (Switch Diagnostics "dflushcoal"; state in gx_worker.cpp) -------
//
// In Threaded mode the game's DCFlushRange (guest 0x8026D800) is the heaviest drain cause
// there is: it arrives ~46 times a frame in heavy 2D and every call that finds records
// pending drains the ring, which waits for the worker to chew through everything
// submitted so far (stderr-49: 15.5 of 18.4 ms of worker wait a frame). The drain is a
// sync point, there so the native body runs after everything recorded before it. While
// this switch is on, a DCFlushRange whose drain would find nothing new to wait for
// returns without draining: the witness kept in gx_worker.cpp remembers the frame of
// the last DCFlushRange drain to complete and the record total at that moment, and a
// later DCFlushRange in the same frame — the frame counter FrameBoundary advances at
// the presented-frame boundary — with no record submitted since skips its drain. The
// first flush of a frame always drains, and any record submission re-arms the next
// flush, so a skipped drain never lets the body run ahead of a record it had to wait
// for. While the switch is off the witness is still kept (it records facts, so a
// mid-frame flip takes effect at once) but never read: drains, counts and waits are
// exactly the unmodified behaviour. Skipped flushes are counted apart — never as
// drains or waits — and PrintPerfWindow's [gxworker] block reports the window's skip
// count, so a hardware A/B reads the DCFlushRange wait with the switch off against
// the same wait plus the skip count with it on.

// ---- the dcnosync experiment (Switch Diagnostics "dcnosync"; state in gx_worker.cpp) ---------
//
// The data-cache ops (DCInvalidateRange, DCFlushRange, DCStoreRange, DCFlushRangeNoSync) are
// NoSync in gx_call_kinds.inc, and their shared body (os_cache.cpp DcRangeOp) decides about the
// drain itself. With the switch off it drains first, exactly as the dispatch layer did when they
// were Inline, so nothing changes. With it on, an op drains only when its range overlaps a
// remembered GXCopyTex destination: those are the only ranges whose body calls into Aurora
// (downloading a pending screen copy for the CPU to read, or retiring one whose memory was
// reused), and Aurora belongs to the worker while records are pending. Every other op only marks
// game-side caches dirty (texture objects, TLUTs: game-thread state under their own mutexes, read
// by the next GXLoadTexObj / GXLoadTlut on the game thread) and bumps the guest-write generations
// (atomic, fenced: gx_guest_write.h), so it runs without waiting. Records pending at the op lose
// nothing: the guest wrote the bytes before flushing them, so those records replayed during the
// old drain already saw the new bytes, and see the same bytes replayed later.
bool DcNoSyncRequested() noexcept;

// Counts one data-cache op made under the switch for the perftimer's [gxworker] block: `drained`
// when its range overlapped a screen copy and it waited as before, otherwise it did not wait.
void NoteDcNoSync(bool drained) noexcept;

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

// The scalar form of DeferOrRun: the GX part of a self-deferring body whose arguments are all
// values, for a body that also keeps a game-side copy of what it sets.
template <auto Fn, typename... A>
void DeferCall(A... args) {
    if (TakeSelfDefer()) {
        if (RecordCall<Fn>(args...)) {
            return;
        }
        Drain(DrainCause::kSelfDefer);
    }
    Fn(args...);
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
    } else if constexpr (kind == Kind::NoSync) {
        return false;
    } else if constexpr (kind == Kind::QueueIfAsked) {
        static_assert(ReturnsVoid(Fn), "only void calls can be queued");
        if (QueueIfAskedRequested() && CanRecord() && RecordCall<Fn>(args...)) {
            return true;
        }
        Drain(Address);
        return false;
    } else {
        Drain(Address);
        return false;
    }
}

} // namespace GxWorker

// Defined by the GX layer: true while the game is building a display list (GXBeginDisplayList to
// GXEndDisplayList), when calls write the list into guest memory and so must run in place.
bool GxWorkerRecordingBlocked() noexcept;
