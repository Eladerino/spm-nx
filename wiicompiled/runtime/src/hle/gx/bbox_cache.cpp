// The hitGetMapEntryBbox aggregate-result cache (bbox_cache.h; spm-bbox
// design v2, s1/s3).
//
// The helper runs inside the one translated body of hitGetMapEntryBbox
// (guest 0x80070298), patched in on the build work copy by
// switch/scripts/patch-bboxcache.py: an entry block right after the body's
// argument captures (`r30 = r3; r27 = r4; r28 = r5;`, before the mapGetWork
// inline) and a capture/compare block at the head of the common return
// block loc_8007045C. Five writer hooks in the hit-family bodies bump the
// single global generation (design s1.3): hitEntry exit and hitReinit entry
// (structure), hitEntryMOBJ exit (mobj), hitDelete at loc_8006BA24
// (delete), hitReCalcMatrix2 at loc_8006D360 (recalc).
//
// Guest memory is read with the same MemoryInline flat primitives the
// generated body uses (memory_access.h). On the flat guest view any u32
// address is readable and unmapped touches read as zero, so the sanity
// bounds below are about lengths and counts only; "unreadable" does not
// exist as a case.
//
// Threading (design s1.6): guest bodies execute only on the scheduler/game
// thread, so no lock is taken. The invariant is not trusted blindly: the
// first call's thread id is recorded and any later call on a different
// thread declines (counted offthread) without touching the records.

#include "bbox_cache.h"

#include "memory_access.h"
#include "ppc_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace BBoxCache {
namespace {

// ---- bounds and layout (design s1.1/s1.5) ------------------------------------
// The record table the helper can represent: slot indices below kMaxSlots,
// group indices below kMaxGroups (every known call site passes group 0; a
// slot holds 2 groups). Anything outside declines as unrepresentable.
constexpr uint32_t kMaxSlots = 16;
constexpr uint32_t kMaxGroups = 2;

constexpr uint32_t kSlotStride = 844;
constexpr uint32_t kGroupStride = 420;
constexpr uint32_t kSlotIndexOffset = 1688; // workBase+1688: the current slot index
constexpr uint32_t kGroupCountOffset = 392; // group+392: the entry count
constexpr uint32_t kGroupBaseOffset = 396;  // group+396: the entries base pointer
constexpr int32_t kSdaWorkOffset = -32528;  // r13-32528: the map work pointer slot

std::atomic<bool> g_bboxCacheOn{false};
std::atomic<bool> g_bboxShadowOn{false};

// ---- the cache state (design s1.1/s1.3) ---------------------------------------
// One record per (slotIdx, groupIdx). workBase is part of the compared scope,
// so a record computed under a previous map's work base is never served.
struct Record {
    bool valid = false;
    uint32_t workBase = 0;
    uint32_t slotIdx = 0;
    uint32_t groupIdx = 0;
    uint32_t entriesBase = 0;
    int32_t entryCount = 0;
    uint64_t generation = 0;
    uint32_t minBits[3] = {0, 0, 0};
    uint32_t maxBits[3] = {0, 0, 0};
};

Record g_records[kMaxSlots][kMaxGroups];

// The single global generation. Every writer hook bumps it; a record is
// servable only while its stamped generation equals it. Never reset.
// Atomic (relaxed) so a bump from any thread is a defined operation even
// outside the s1.6 game-thread invariant (code review F2).
std::atomic<uint64_t> g_generation{0};

// ---- counters (design s3; windowed like DlProbe's, reset at each print) -------
struct Counters {
    uint64_t calls = 0;
    uint64_t hits = 0;
    uint64_t recomputes = 0;
    uint64_t declineOffthread = 0;
    uint64_t declineInsane = 0;
    uint64_t declineUnrepresentable = 0;
    uint64_t declineOverlap = 0;
    uint64_t bumpsStructure = 0;
    uint64_t bumpsMobj = 0;
    uint64_t bumpsDelete = 0;
    uint64_t bumpsRecalc = 0;
    uint64_t shadowComparisons = 0;
    uint64_t shadowMismatches = 0;
    // Realised serve-path cost, for the Phase-2 h back-out (design s4):
    // nanoseconds accumulated inside BBoxCacheTryServe on served calls only.
    uint64_t serveNs = 0;
};
Counters g_win;
uint64_t g_mismatchTotal = 0; // cumulative: the first-16 detail lines gate on this

// ---- thread guard (design s1.6) ------------------------------------------------
// The owner is recorded through std::call_once (code review F2): two racing
// first calls cannot both observe "no owner yet" and install the wrong one.
std::once_flag g_ownerOnce;
std::thread::id g_owner{};
std::atomic<bool> g_ownerSet{false};

bool OnOwnerThread(bool record) {
    const std::thread::id id = std::this_thread::get_id();
    if (record) {
        std::call_once(g_ownerOnce, [&] {
            g_owner = id;
            g_ownerSet.store(true, std::memory_order_release);
        });
        return id == g_owner;
    }
    if (!g_ownerSet.load(std::memory_order_acquire)) return false;
    return id == g_owner;
}

// ---- the per-call scope, read once at the entry hook ---------------------------
// Classification of a call, decided before records[] is ever indexed
// (design s1.5 pinned order: scope, bounds/sanity, only then records[]).
enum class Verdict : uint8_t { Servable, Recompute, Offthread, Insane, Unrepresentable, Overlap };

struct Scope {
    uint32_t workBase = 0;
    uint32_t slotIdx = 0;
    uint32_t groupIdx = 0;
    uint32_t entriesBase = 0;
    int32_t entryCount = 0;
    uint64_t generation = 0;
};

Verdict Classify(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin, uint32_t outMax,
                 Scope& scope) {
    if (!OnOwnerThread(true)) return Verdict::Offthread;
    scope.workBase = MemoryInline::FlatRead32(static_cast<uint32_t>(ctx->gpr[13] + kSdaWorkOffset));
    scope.slotIdx = MemoryInline::FlatRead32(scope.workBase + kSlotIndexOffset);
    const uint32_t group = scope.workBase + scope.slotIdx * kSlotStride + groupIdx * kGroupStride;
    scope.entriesBase = MemoryInline::FlatRead32(group + kGroupBaseOffset);
    scope.entryCount = static_cast<int32_t>(MemoryInline::FlatRead32(group + kGroupCountOffset));
    scope.groupIdx = groupIdx;
    // Bounds/sanity BEFORE any records[] or generation/record-state access
    // (R1-F4 pinned order: scope -> bounds/sanity -> records[]/generation).
    if (scope.slotIdx >= kMaxSlots || groupIdx >= kMaxGroups) return Verdict::Unrepresentable;
    if (scope.workBase == 0 || scope.entryCount < 0 ||
        (scope.entriesBase == 0 && scope.entryCount > 0)) {
        return Verdict::Insane;
    }
    scope.generation = g_generation.load(std::memory_order_relaxed);
    const Record& rec = g_records[scope.slotIdx][groupIdx];
    if (rec.valid && rec.workBase == scope.workBase && rec.slotIdx == scope.slotIdx &&
        rec.groupIdx == groupIdx && rec.entriesBase == scope.entriesBase &&
        rec.entryCount == scope.entryCount && rec.generation == scope.generation) {
        // The served copy-out interleaves min[i]/max[i] per index, like
        // the original's alternating folds; overlapping out ranges still
        // decline even on an otherwise servable record (R1-F6), because
        // the original's result there is a specific interleaving artefact
        // of its fold order, not of the copy-out's. Two compares, still
        // O(1). The walk then runs, but its result is an aliasing artefact
        // of that call, so the entry path leaves the scope unkeyed and
        // the capture stores nothing for it.
        if (outMin < outMax + 12 && outMax < outMin + 12) return Verdict::Overlap;
        return Verdict::Servable;
    }
    return Verdict::Recompute;
}

void CountVerdict(Verdict v) {
    switch (v) {
        case Verdict::Recompute: ++g_win.recomputes; break;
        case Verdict::Offthread: ++g_win.declineOffthread; break;
        case Verdict::Insane: ++g_win.declineInsane; break;
        case Verdict::Unrepresentable: ++g_win.declineUnrepresentable; break;
        case Verdict::Overlap: ++g_win.declineOverlap; break;
        case Verdict::Servable: break;
    }
}

// The scope of the call currently inside the body, remembered at the entry
// hook for the return block's capture (which must key the record by the
// entry-time scope and generation, design s1.5).
Scope g_callScope;
bool g_callScopeOk = false;   // the entry hook classified a keyable scope
bool g_callServed = false;    // the entry hook served this call (capture skips it)

// ---- the per-call mode latch (code review F1) ----------------------------------
// BBoxCacheCallBegin latches, at entry, that it ran for this call and which
// mode was on. The return block consults ONLY this latch (through
// BBoxCacheCallBegun / BBoxCacheCallShadow, closed by BBoxCacheCallEnd) and
// never re-reads the diag flags, so a live flag flip landing mid-walk takes
// effect from the next call's entry and can neither make the return block
// act on stale per-call state from a previous call nor strand a shadow
// stash into a later call's compare.
bool g_callBegun = false;  // CallBegin ran for the call currently inside the body
bool g_callShadow = false; // the latched mode of that call was shadow

// ---- the shadow stash (design s3) ----------------------------------------------
uint64_t g_callSeq = 0; // bumped by BBoxCacheCallBegin on every call while a mode is on
uint64_t g_curSeq = 0;  // the sequence value of the call currently inside the body
struct Stash {
    bool valid = false;
    uint64_t stamp = 0;
    uint32_t outMin = 0;
    uint32_t outMax = 0;
    uint32_t minBits[3] = {0, 0, 0};
    uint32_t maxBits[3] = {0, 0, 0};
    Scope scope;
};
Stash g_stash;

} // namespace

bool Enabled() noexcept { return g_bboxCacheOn.load(std::memory_order_relaxed); }

bool ShadowEnabled() noexcept { return g_bboxShadowOn.load(std::memory_order_relaxed); }

void PrintWindow(uint64_t firstFrame, uint64_t lastFrame) {
    // Bumps alone do not open a window line: the writer hooks fire in both
    // A/B arms (and with both keys off), and the key-off control must stay
    // quiet (design s3: nothing prints in windows where both keys were off
    // throughout and nothing was counted by an active mode). A suppressed
    // window's counters are NOT reset (code review F5): in a suppressed
    // window the only nonzero counters can be the bump counts, and those
    // carry into the next printed window so the dirty-event census does
    // not silently lose bump-only windows (e.g. a load/warp window).
    const bool anyActivity = g_win.calls != 0 || g_win.hits != 0 || g_win.recomputes != 0 ||
                             g_win.declineOffthread != 0 || g_win.declineInsane != 0 ||
                             g_win.declineUnrepresentable != 0 || g_win.declineOverlap != 0 ||
                             g_win.shadowComparisons != 0 || g_win.shadowMismatches != 0;
    if (!Enabled() && !ShadowEnabled() && !anyActivity) return;
    const Counters win = g_win;
    g_win = Counters{};
    const uint64_t serveAvgNs = win.hits != 0 ? win.serveNs / win.hits : 0;
    std::fprintf(stderr,
                 "[bboxcache] frames %llu-%llu: %llu calls, %llu hits, %llu recomputes, "
                 "declines offthread %llu insane %llu unrepresentable %llu overlap %llu, "
                 "bumps structure %llu mobj %llu delete %llu recalc %llu, "
                 "shadow %llu comparisons %llu mismatches, serve avg %llu ns/hit\n",
                 static_cast<unsigned long long>(firstFrame),
                 static_cast<unsigned long long>(lastFrame),
                 static_cast<unsigned long long>(win.calls),
                 static_cast<unsigned long long>(win.hits),
                 static_cast<unsigned long long>(win.recomputes),
                 static_cast<unsigned long long>(win.declineOffthread),
                 static_cast<unsigned long long>(win.declineInsane),
                 static_cast<unsigned long long>(win.declineUnrepresentable),
                 static_cast<unsigned long long>(win.declineOverlap),
                 static_cast<unsigned long long>(win.bumpsStructure),
                 static_cast<unsigned long long>(win.bumpsMobj),
                 static_cast<unsigned long long>(win.bumpsDelete),
                 static_cast<unsigned long long>(win.bumpsRecalc),
                 static_cast<unsigned long long>(win.shadowComparisons),
                 static_cast<unsigned long long>(win.shadowMismatches),
                 static_cast<unsigned long long>(serveAvgNs));
}

} // namespace BBoxCache

extern "C" void spm_bboxcache_set(bool enabled) {
    BBoxCache::g_bboxCacheOn.store(enabled, std::memory_order_relaxed);
}

extern "C" void spm_bboxshadow_set(bool enabled) {
    BBoxCache::g_bboxShadowOn.store(enabled, std::memory_order_relaxed);
}

extern "C" uint64_t BBoxCacheCallBegin() {
    ++BBoxCache::g_callSeq;
    BBoxCache::g_curSeq = BBoxCache::g_callSeq;
    ++BBoxCache::g_win.calls;
    BBoxCache::g_callScopeOk = false;
    BBoxCache::g_callServed = false;
    // Latch this call's mode at entry (code review F1). Any earlier stash
    // was consumed (or discarded as stamp-foreign) by its own call's
    // return block; clear the slot so none can outlive its call.
    BBoxCache::g_callBegun = true;
    BBoxCache::g_callShadow = BBoxCache::ShadowEnabled();
    BBoxCache::g_stash.valid = false;
    return BBoxCache::g_curSeq;
}

extern "C" bool BBoxCacheCallBegun() { return BBoxCache::g_callBegun; }

extern "C" bool BBoxCacheCallShadow() {
    return BBoxCache::g_callBegun && BBoxCache::g_callShadow;
}

extern "C" void BBoxCacheCallEnd() {
    BBoxCache::g_callBegun = false;
    BBoxCache::g_callShadow = false;
    BBoxCache::g_callScopeOk = false;
    BBoxCache::g_callServed = false;
}

extern "C" bool BBoxCacheEnabled() { return BBoxCache::Enabled(); }

extern "C" bool BBoxCacheShadowEnabled() { return BBoxCache::ShadowEnabled(); }

extern "C" bool BBoxCacheTryServe(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin,
                                  uint32_t outMax) {
    using namespace BBoxCache;
    const auto t0 = std::chrono::steady_clock::now();
    Scope scope;
    const Verdict v = Classify(ctx, groupIdx, outMin, outMax, scope);
    if (v != Verdict::Servable) {
        CountVerdict(v);
        // A keyable scope (servable or recompute) is remembered for the
        // return block's capture; a decline leaves nothing to key by.
        if (v == Verdict::Recompute) {
            g_callScope = scope;
            g_callScopeOk = true;
        }
        return false;
    }
    const Record& rec = g_records[scope.slotIdx][groupIdx];
    for (int i = 0; i < 3; ++i) {
        MemoryInline::FlatWrite32(outMin + static_cast<uint32_t>(i * 4), rec.minBits[i]);
        MemoryInline::FlatWrite32(outMax + static_cast<uint32_t>(i * 4), rec.maxBits[i]);
    }
    ++g_win.hits;
    g_callServed = true;
    const auto t1 = std::chrono::steady_clock::now();
    g_win.serveNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return true;
}

extern "C" void BBoxCacheShadowNote(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin,
                                    uint32_t outMax) {
    using namespace BBoxCache;
    Scope scope;
    const Verdict v = Classify(ctx, groupIdx, outMin, outMax, scope);
    if (v == Verdict::Servable) {
        const Record& rec = g_records[scope.slotIdx][groupIdx];
        Stash& stash = g_stash;
        stash.valid = true;
        stash.stamp = g_curSeq;
        stash.outMin = outMin;
        stash.outMax = outMax;
        for (int i = 0; i < 3; ++i) {
            stash.minBits[i] = rec.minBits[i];
            stash.maxBits[i] = rec.maxBits[i];
        }
        stash.scope = scope;
        ++g_win.hits; // design s3: in shadow mode, hits = servable-record calls
        g_callScope = scope;
        g_callScopeOk = true;
        return;
    }
    CountVerdict(v);
    if (v == Verdict::Recompute) {
        g_callScope = scope;
        g_callScopeOk = true;
    }
    // A decline or a recompute writes no stash: the walk's result is captured
    // at the return block instead, and any older stash is stamp-foreign.
}

extern "C" void BBoxCacheCapture(uint32_t outMin, uint32_t outMax) {
    using namespace BBoxCache;
    if (!g_callBegun) return;      // no CallBegin for this call: no-op (F1 latch)
    if (g_callServed) return;      // served calls never ran the walk
    if (!g_callScopeOk) return;    // a decline keyed no scope; nothing to store
    if (!OnOwnerThread(false)) return;
    Record& rec = g_records[g_callScope.slotIdx][g_callScope.groupIdx];
    rec.workBase = g_callScope.workBase;
    rec.slotIdx = g_callScope.slotIdx;
    rec.groupIdx = g_callScope.groupIdx;
    rec.entriesBase = g_callScope.entriesBase;
    rec.entryCount = g_callScope.entryCount;
    rec.generation = g_callScope.generation;
    for (int i = 0; i < 3; ++i) {
        rec.minBits[i] = MemoryInline::FlatRead32(outMin + static_cast<uint32_t>(i * 4));
        rec.maxBits[i] = MemoryInline::FlatRead32(outMax + static_cast<uint32_t>(i * 4));
    }
    rec.valid = true;
}

extern "C" void BBoxCacheShadowCompare() {
    using namespace BBoxCache;
    if (!g_callBegun || !g_callShadow) return; // latched-mode gate (F1)
    if (!OnOwnerThread(false)) return;
    Stash& stash = g_stash;
    if (!stash.valid) return;
    if (stash.stamp != g_curSeq) {
        // Stale or foreign: discard it and count nothing. A stash is only
        // ever compared by the call whose entry block produced it (with
        // the F1 latch the return block runs for exactly that call).
        stash.valid = false;
        return;
    }
    const Stash mine = stash;
    stash.valid = false; // consume
    ++g_win.shadowComparisons;
    bool mismatch = false;
    uint32_t walkedMin[3], walkedMax[3];
    for (int i = 0; i < 3; ++i) {
        walkedMin[i] = MemoryInline::FlatRead32(mine.outMin + static_cast<uint32_t>(i * 4));
        walkedMax[i] = MemoryInline::FlatRead32(mine.outMax + static_cast<uint32_t>(i * 4));
        if (walkedMin[i] != mine.minBits[i] || walkedMax[i] != mine.maxBits[i]) mismatch = true;
    }
    if (mismatch) {
        ++g_win.shadowMismatches;
        ++g_mismatchTotal;
        if (g_mismatchTotal <= 16) {
            std::fprintf(stderr,
                         "[bboxcache] mismatch %llu: scope workBase 0x%08X slot %u group %u "
                         "generation %llu: walked min %08X %08X %08X max %08X %08X %08X vs "
                         "cached min %08X %08X %08X max %08X %08X %08X\n",
                         static_cast<unsigned long long>(g_mismatchTotal), mine.scope.workBase,
                         mine.scope.slotIdx, mine.scope.groupIdx,
                         static_cast<unsigned long long>(mine.scope.generation),
                         walkedMin[0], walkedMin[1], walkedMin[2], walkedMax[0], walkedMax[1],
                         walkedMax[2], mine.minBits[0], mine.minBits[1], mine.minBits[2],
                         mine.maxBits[0], mine.maxBits[1], mine.maxBits[2]);
        }
    }
}

extern "C" void BBoxCacheBumpStructure() {
    BBoxCache::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++BBoxCache::g_win.bumpsStructure;
}

extern "C" void BBoxCacheBumpMobj() {
    BBoxCache::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++BBoxCache::g_win.bumpsMobj;
}

extern "C" void BBoxCacheBumpDelete() {
    BBoxCache::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++BBoxCache::g_win.bumpsDelete;
}

extern "C" void BBoxCacheBumpRecalc() {
    BBoxCache::g_generation.fetch_add(1, std::memory_order_relaxed);
    ++BBoxCache::g_win.bumpsRecalc;
}
