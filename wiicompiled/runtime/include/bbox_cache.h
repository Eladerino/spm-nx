#pragma once

#include <cstdint>

struct CpuContext;

// ---- the hitGetMapEntryBbox aggregate-result cache (spm-bbox design v2; Switch
// Diagnostics "bboxcache" / "bboxshadow"; state in src/hle/gx/bbox_cache.cpp) ----
//
// hitGetMapEntryBbox (guest 0x80070298) computes one aggregate world-space AABB
// for a whole hit group by walking every entry and folding two transformed
// corner points per surviving entry into the outputs: ~19 calls a frame in
// he1_05, 1.85 ms/f of subtree (s76), and no hit/miss asymmetry inside the
// function. This helper memoises the reduction's output - six floats, stored
// as raw bit patterns - per (slot, group), keyed by a single global generation
// counter that the closed writer set bumps at build-time-patched hook sites
// (hitEntry / hitReinit / hitEntryMOBJ exits and entry, hitDelete's delete
// point, hitReCalcMatrix2's node-rewrite convergence). The per-call check is
// O(1): four guest reads for the scope tuple, integer compares, and on a hit
// six guest writes. No per-entry validation ever happens (the s77 lesson).
// Every uncertainty declines to the original translated walk, unmodified.
//
// "bboxshadow" is the proof mode: the entry hook stashes the would-be served
// bits when the record is servable, the walk always runs, and a compare at the
// body's return block counts a comparison or a bit-exact mismatch. Both
// switches on: shadow wins. Both off: the patched body runs exactly as
// unpatched (design v2 s1.5 hard requirement) - the entry and return blocks
// dispatch on the flags first, and BBoxCacheCallBegin / BBoxCacheCapture do
// no work at all while both are off.

namespace BBoxCache {

// True while the enforcing switch is on. One relaxed atomic load.
bool Enabled() noexcept;

// True while the shadow switch is on. One relaxed atomic load.
bool ShadowEnabled() noexcept;

// The [bboxcache] line for a perftimer window, printed with the [calls] lines
// from PrintNativeOverrideCounts. Prints nothing while both switches were off
// for the whole window and no active-mode counter moved in it.
void PrintWindow(uint64_t firstFrame, uint64_t lastFrame);

} // namespace BBoxCache

// The Diagnostics forwarding entries (device.cpp's diag_init / diag_set call
// them for the "bboxcache" / "bboxshadow" switches), the same shape as
// spm_hitcache_set / spm_dlprobe_set.
extern "C" void spm_bboxcache_set(bool enabled);
extern "C" void spm_bboxshadow_set(bool enabled);

// The entry points the build-time patch (switch/scripts/patch-bboxcache.py)
// inserts into translated bodies on the build work copy. Declared here so the
// runtime side has one home for them; the patch script carries its own copy of
// these declarations for the shards, the same way patch-hitcache.py does.

// Bumps the per-call sequence counter and counts the call. The patched entry
// block calls this ONLY when a mode is on (design v2 s1.5, R2-F8), so a
// shadow stash can only ever be compared by the call that produced it and the
// key-off body carries no stamp cost.
extern "C" uint64_t BBoxCacheCallBegin();

// The patched entry block's mode predicates.
extern "C" bool BBoxCacheEnabled();
extern "C" bool BBoxCacheShadowEnabled();

// Enforcing path: on true, the six cached floats have been written through
// the out pointers (raw bit copies) and the caller jumps to the return label.
// On false the original walk runs.
extern "C" bool BBoxCacheTryServe(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin, uint32_t outMax);

// Shadow path: classifies the call the same way TryServe does and, only when
// the record is servable, stashes its six bit patterns stamped with the
// current call's sequence value. A decline or a recompute stashes nothing.
extern "C" void BBoxCacheShadowNote(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin, uint32_t outMax);

// Recompute capture, at the head of the return block: runs on every call
// that executed the original walk while a mode is on (a served call skips
// it). Reads the six floats the walk just wrote through the out pointers and
// stores them as the record for this call's entry-time scope, stamped with
// the entry-time generation.
extern "C" void BBoxCacheCapture(uint32_t outMin, uint32_t outMax);

// Shadow compare, after the capture in the return block: consumes a stash
// whose stamp equals the current call's sequence value and compares it
// bit-exact against the six floats the walk just wrote. An absent, stale or
// foreign stash is skipped silently.
extern "C" void BBoxCacheShadowCompare();

// The writer-hook entry points (design v2 s1.3/s1.4). Each takes no argument
// and performs no guest read: one increment of the single global generation
// plus one increment of its per-class window counter. They are called
// unconditionally from the patched writer bodies, so the generation is
// correct the moment a mode turns on.
extern "C" void BBoxCacheBumpStructure();
extern "C" void BBoxCacheBumpMobj();
extern "C" void BBoxCacheBumpDelete();
extern "C" void BBoxCacheBumpRecalc();
