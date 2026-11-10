#pragma once

#include <cstdint>

struct CpuContext;

// ---- the hitNameToPtr persistent index (spm-hit-index design (b); Switch
// Diagnostics "hitindex" / "hitindexshadow"; state in src/hle/gx/hit_index.cpp) ----
//
// hitNameToPtr (guest 0x8006F808) resolves a map-object name to its hit-table
// entry by scanning the current work slot's groups and strcmp-ing every active
// entry's name: ~45 lookups a frame in he2_03, ~12,400 guest strcmp calls a
// frame, 1.6-2.4 ms/f of subtree (s76), and standing play is ~100% misses
// (s77: 113 hits in ~280k calls, all in load/transition windows) - a miss is
// the scan's most expensive case, a full-table walk. This helper maintains a
// persistent content-keyed index, name bytes -> the guest address of the first
// entry in scan order carrying that name, for one scope (workBase, slotIdx,
// and the group count at slot+0), stamped with a single global generation
// counter that the closed writer set bumps at build-time-patched hook sites
// (hitEntry / hitReinit / hitEntryMOBJ / _hitEnt / hitDelete / hitFlgOn /
// hitFlgOff and the four direct-writing flag wrappers hitObjFlagOn /
// hitObjFlagOff / hitGrpFlagOn / hitGrpFlagOff - census amendment H8-H11).
// A call is served iff scope and
// generation still match the build stamp; a miss is answered 0 from the table
// with NO fallback scan - "not in the index" is equivalent to "not in the
// table" for the indexed generation, by construction plus the writer census
// (spm-hit-index-census.md) plus the shadow gate. There is NO per-call
// validation of any kind (the s77 lesson: its whole-snapshot validation cost
// ~100 us/call against the ~44 us/call scan it replaced). Every uncertainty -
// an unclassifiable table state at build, an over-long name, an off-thread
// arrival, a churn latch - declines to the original translated body,
// unmodified; a failed build is sticky for its (scope, generation), so a
// pathological table costs one failed walk per generation, not one per call.
//
// "hitindexshadow" is the proof mode: the entry hook computes the would-be
// answer and stashes it, the scan always runs, and a compare at the body's
// return block counts a comparison or an exact mismatch. Both switches on:
// shadow wins. Both off: the patched bodies run exactly as unpatched - no
// helper work beyond the gating predicates is reachable from hitNameToPtr
// (two flag-predicate calls at its entry block, one latch call at its return
// block); the writer-hook bump inserts are unconditional (one atomic
// increment plus one counter increment each), so the generation is already
// correct the moment a mode turns on. HitIndexCallBegin runs only when a
// mode is on at entry, and it latches the mode for the call: the return
// block consults only that latch, never the flags again, so a live flag
// flip mid-scan takes effect from the next call's entry (bbox code
// review F1).

namespace HitIndex {

// True while the enforcing switch is on. One relaxed atomic load.
bool Enabled() noexcept;

// True while the shadow switch is on. One relaxed atomic load.
bool ShadowEnabled() noexcept;

// The [hitindex] line for a perftimer window, printed with the [calls] lines
// from PrintNativeOverrideCounts. Prints nothing while both switches were off
// for the whole window and no active-mode counter moved in it.
void PrintWindow(uint64_t firstFrame, uint64_t lastFrame);

} // namespace HitIndex

// The Diagnostics forwarding entries (device.cpp's diag_init / diag_set call
// them for the "hitindex" / "hitindexshadow" switches), the same shape as
// spm_bboxcache_set / spm_hitcache_set.
extern "C" void spm_hitindex_set(bool enabled);
extern "C" void spm_hitindexshadow_set(bool enabled);

// The entry points the build-time patch (switch/scripts/patch-hitindex.py)
// inserts into translated bodies on the build work copy. Declared here so the
// runtime side has one home for them; the patch script carries its own copy of
// these declarations for the shards, the same way patch-bboxcache.py does.

// Bumps the per-call sequence counter, counts the call, and latches the
// call's mode (begun / shadow) for the return block. The patched entry block
// calls this ONLY when a mode is on, so a shadow stash can only ever be
// compared by the call that produced it and the key-off body carries no
// stamp cost.
extern "C" uint64_t HitIndexCallBegin();

// The return block's latch accessors: HitIndexCallBegun reports whether
// HitIndexCallBegin ran for the call now reaching the return block;
// HitIndexCallShadow reports the mode latched at its entry. HitIndexCallEnd
// closes the latch at the end of the return block. None of them reads the
// diag flags.
extern "C" bool HitIndexCallBegun();
extern "C" bool HitIndexCallShadow();
extern "C" void HitIndexCallEnd();

// The patched entry block's mode predicates.
extern "C" bool HitIndexEnabled();
extern "C" bool HitIndexShadowEnabled();

// Enforcing path: on true, *outResult is the scan's answer (the first
// matching entry's guest address, or 0 for a proven miss or a null name
// pointer) and the caller jumps to the return block. On false the original
// scan runs.
extern "C" bool HitIndexTry(CpuContext* ctx, uint32_t namePtr, uint32_t* outResult);

// Shadow path: computes the would-be answer under the same rules and, only
// when it produces one, stashes it (with the query content) stamped with the
// current call's sequence value. A decline writes nothing.
extern "C" void HitIndexShadowNote(CpuContext* ctx, uint32_t namePtr);

// Shadow compare, at the head of the return block: consumes a stash whose
// stamp equals the current call's sequence value and compares it against the
// scan's result in r3. An absent, stale or foreign stash is skipped silently.
extern "C" void HitIndexShadowCompare(uint32_t scanResult);

// The writer-hook entry points. Each takes no argument and performs no guest
// read: one increment of the single global generation plus one increment of
// its per-class window counter. They are called unconditionally from the
// patched writer bodies, so the generation is correct the moment a mode
// turns on.
extern "C" void HitIndexBumpStructure();
extern "C" void HitIndexBumpFill();
extern "C" void HitIndexBumpDelete();
extern "C" void HitIndexBumpFlags();
