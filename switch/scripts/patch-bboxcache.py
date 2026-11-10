#!/usr/bin/env python3
"""Patch the copied generated bodies for the hitGetMapEntryBbox cache.

build-switch.sh copies wiicompiled/generated into the build tree; this
script edits that copy (never the translator's output in the repo), the
same pattern as patch-anim-draw.py / patch-npc-guard.py / patch-hitcache.py.
It patches six bodies (spm-bbox design v2 s6), each at pinned anchors, and
hard-fails the build unless every anchor matches exactly once in its body:

func_80070298 (hitGetMapEntryBbox), two anchors:
  ENTRY: immediately AFTER the line `    r28 = r5;` in the body (the
  prologue's callee-saved stack saves are complete, the argument locals
  r30 = group index / r27 = out-min / r28 = out-max have just been
  captured, and the mapGetWork inline that follows has not run). The
  inserted block dispatches on the diag flags first, so with both keys
  off nothing else runs (design v2 s1.5 hard requirement, R2-F8): shadow
  on -> BBoxCacheShadowNote (stash the would-be served bits, never
  jump); else bboxcache on -> BBoxCacheTryServe, and on a serve it jumps
  to the common return block. Both on: shadow wins. BBoxCacheCallBegin
  latches the mode it found at entry (code review F1).
  RETURN: the head of the common return block loc_8007045C, the body's
  only exit, immediately after its label line (the block that follows
  restores r27/r28 from the stack, so the out pointers are still live in
  the locals there). The inserted block consults ONLY the entry latch
  (BBoxCacheCallBegun / BBoxCacheCallShadow, closed by BBoxCacheCallEnd)
  and never re-reads the diag flags, so a live flag flip mid-walk takes
  effect from the next call's entry: capture the walked result into the
  record, then in latched shadow mode compare the stash. With both keys
  off at entry the latch is unset and the block does nothing at all.

Writer hooks, one anchored insert per site, each a bare call to a bump
entry point with NO guest-state argument (design v2 s1.3: a bump is one
increment of the single global generation plus one per-class counter
increment; no hook reads guest memory):
  H1 hitEntry (func_8006B614) exit: before the body's sole `return;` ->
     BBoxCacheBumpStructure
  H2 hitReinit (func_8006ADB4) entry: after the entry label loc_8006ADB4:
     -> BBoxCacheBumpStructure
  H3 hitEntryMOBJ (func_8006B7B4) exit: before the body's sole `return;`
     -> BBoxCacheBumpMobj
  H4 hitDelete (func_8006B9A8): after the label loc_8006BA24: exactly -
     the sole entry+8 write's block and the sole convergence point of
     both successful-delete paths (the +220/+216 zeroing in loc_8006BA10
     falls through to it; the +220 == 0 path jumps to it from
     loc_8006BA0C) -> BBoxCacheBumpDelete
  H5 hitReCalcMatrix2 (func_8006D2E8): after the label loc_8006D360:
     exactly - the unique convergence of both primary PSMTXConcat
     branches (one jumps here, one falls through), before the two
     optional concats, executed once per gated node rewrite and not on
     the loc_8006D3FC skip path -> BBoxCacheBumpRecalc
  There is no fallback anchor for H4/H5 (design v2 s6): if either label
  does not match exactly once, the build fails loudly.

The helper itself lives in the runtime (wiicompiled/runtime/src/hle/gx/
bbox_cache.cpp, bbox_cache.h): the declarations below are its entry
points.

Anchors come from the guest addresses and the bodies' own lines, so they
survive regeneration; the shard file names are content hashes and are
found by search. Any missing or duplicated anchor fails the build loudly
rather than shipping without the cache.
"""
import pathlib
import sys

DECLS_BBOX = [
    'extern "C" uint64_t BBoxCacheCallBegin();',
    'extern "C" bool BBoxCacheCallBegun();',
    'extern "C" bool BBoxCacheCallShadow();',
    'extern "C" void BBoxCacheCallEnd();',
    'extern "C" bool BBoxCacheEnabled();',
    'extern "C" bool BBoxCacheShadowEnabled();',
    'extern "C" bool BBoxCacheTryServe(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin, uint32_t outMax);',
    'extern "C" void BBoxCacheShadowNote(CpuContext* ctx, uint32_t groupIdx, uint32_t outMin, uint32_t outMax);',
    'extern "C" void BBoxCacheCapture(uint32_t outMin, uint32_t outMax);',
    'extern "C" void BBoxCacheShadowCompare();',
]
DECL_STRUCTURE = 'extern "C" void BBoxCacheBumpStructure();'
DECL_MOBJ = 'extern "C" void BBoxCacheBumpMobj();'
DECL_DELETE = 'extern "C" void BBoxCacheBumpDelete();'
DECL_RECALC = 'extern "C" void BBoxCacheBumpRecalc();'

BBOX_FUNC = "func_80070298"
BBOX_DEFN = 'extern "C" void func_80070298(CpuContext* MKW_RESTRICT ctx)'
ENTRY_ANCHOR = "    r28 = r5;"
ENTRY_BLOCK = (
    "    { if (BBoxCacheShadowEnabled() || BBoxCacheEnabled()) { (void)BBoxCacheCallBegin(); "
    "if (BBoxCacheCallShadow()) { BBoxCacheShadowNote(ctx, r30, r27, r28); } "
    "else if (BBoxCacheTryServe(ctx, r30, r27, r28)) { goto loc_8007045C; } } }"
)
RETURN_LABEL = "loc_8007045C:"
RETURN_BLOCK = (
    "    { if (BBoxCacheCallBegun()) { "
    "BBoxCacheCapture(r27, r28); if (BBoxCacheCallShadow()) { BBoxCacheShadowCompare(); } "
    "BBoxCacheCallEnd(); } }"
)

# Writer sites: (function, defn, kind, anchor, block, decl)
# kind "after": insert after the anchor label line.
# kind "before_return": insert before the body's sole `    return;` line.
WRITERS = [
    ("func_8006B614", 'extern "C" void func_8006B614(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { BBoxCacheBumpStructure(); }", DECL_STRUCTURE),
    ("func_8006ADB4", 'extern "C" void func_8006ADB4(CpuContext* MKW_RESTRICT ctx)',
     "after", "loc_8006ADB4:", "    { BBoxCacheBumpStructure(); }", DECL_STRUCTURE),
    ("func_8006B7B4", 'extern "C" void func_8006B7B4(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { BBoxCacheBumpMobj(); }", DECL_MOBJ),
    ("func_8006B9A8", 'extern "C" void func_8006B9A8(CpuContext* MKW_RESTRICT ctx)',
     "after", "loc_8006BA24:", "    { BBoxCacheBumpDelete(); }", DECL_DELETE),
    ("func_8006D2E8", 'extern "C" void func_8006D2E8(CpuContext* MKW_RESTRICT ctx)',
     "after", "loc_8006D360:", "    { BBoxCacheBumpRecalc(); }", DECL_RECALC),
]


def func_ranges(lines):
    """Yield (name, start, end) for each generated function in the shard."""
    starts = []
    for i, line in enumerate(lines):
        if line.startswith('extern "C" void func_'):
            name = line.split("(", 1)[0].split()[-1]
            starts.append((name, i))
    for k, (name, start) in enumerate(starts):
        end = starts[k + 1][1] if k + 1 < len(starts) else len(lines)
        yield name, start, end


def find_shard(shards, defn):
    for shard in shards:
        if defn in shard.read_text(encoding="utf-8", errors="replace"):
            return shard
    return None


def patch_file(path, jobs, ending_hint=None):
    """Apply jobs to one shard file. Each job: (func, defn, kind, anchor,
    block, decls). Returns (inserted, already_present) or raises."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    ending = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
    inserted = 0
    already = 0
    decls_needed = []
    # Process jobs in reverse body order so earlier indices stay valid:
    # sort by function start descending.
    jobs = sorted(jobs, key=lambda j: ranges[j[0]][0], reverse=True)
    for func, defn, kind, anchor, block, decls in jobs:
        if func not in ranges:
            raise ValueError(f"{func} range not found in {path.name}")
        start, end = ranges[func]
        body = lines[start:end]
        if any(block == l.rstrip("\r\n") for l in body):
            already += 1
            for d in decls:
                if d not in decls_needed:
                    decls_needed.append(d)
            continue
        if any("BBoxCache" in l for l in body):
            raise ValueError(
                f"{func} in {path.name} mentions BBoxCache but the expected block "
                "is not present exactly as inserted")
        if kind == "after":
            idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == anchor]
            if len(idx) != 1:
                raise ValueError(
                    f"anchor {anchor!r} matched {len(idx)} times in {func}, expected exactly 1")
            body.insert(idx[0] + 1, block + ending)
        elif kind == "before_return":
            idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == "    return;"]
            if len(idx) != 1:
                raise ValueError(
                    f"sole return matched {len(idx)} times in {func}, expected exactly 1")
            body.insert(idx[0], block + ending)
        else:
            raise ValueError(f"unknown job kind {kind!r}")
        lines[start:end] = body
        ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
        inserted += 1
        for d in decls:
            if d not in decls_needed:
                decls_needed.append(d)
    # Declarations: insert the needed ones (that are not already present in
    # the file) before the earliest patched function's definition.
    present = set(l.rstrip("\r\n") for l in lines)
    new_decls = [d for d in decls_needed if d not in present]
    if new_decls:
        first_defn = min(j[1] for j in jobs)
        defn_idx = next(i for i, line in enumerate(lines) if first_defn in line)
        for j, decl in enumerate(new_decls):
            lines.insert(defn_idx + j, decl + ending)
    if inserted or new_decls:
        path.write_text("".join(lines), encoding="utf-8")
    return inserted, already


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-bboxcache.py <generated dir>", file=sys.stderr)
        return 1
    gen = pathlib.Path(sys.argv[1])
    shards = sorted((gen / "build_shards" / "base_common").glob("*.cpp"))

    # Group jobs by the shard that defines each function.
    by_shard = {}

    def add_job(defn, job):
        shard = find_shard(shards, defn)
        if shard is None:
            print(f"patch-bboxcache: no shard defines {job[0]}", file=sys.stderr)
            return False
        by_shard.setdefault(shard, []).append(job)
        return True

    # The bbox walk: two inserts handled as one job pair inside its body.
    bbox_job = (BBOX_FUNC, BBOX_DEFN, "bbox", None, None, DECLS_BBOX)
    if not add_job(BBOX_DEFN, bbox_job):
        return 1
    for func, defn, kind, anchor, block, decl in WRITERS:
        if not add_job(defn, (func, defn, kind, anchor, block, [decl])):
            return 1

    total_inserted = 0
    total_already = 0
    for shard, jobs in sorted(by_shard.items()):
        # Split the bbox job out: it needs its own two-anchor handling.
        bbox_jobs = [j for j in jobs if j[2] == "bbox"]
        writer_jobs = [j for j in jobs if j[2] != "bbox"]
        try:
            if writer_jobs:
                ins, alr = patch_file(shard, writer_jobs)
                total_inserted += ins
                total_already += alr
            for _ in bbox_jobs:
                ins, alr = patch_bbox(shard)
                total_inserted += ins
                total_already += alr
        except ValueError as exc:
            print(f"patch-bboxcache: {exc}", file=sys.stderr)
            return 1
        print(f"patch-bboxcache: shard {shard.name}: jobs done "
              f"(cumulative inserted {total_inserted}, already present {total_already})")
    print(f"patch-bboxcache: {total_inserted} inserted, {total_already} already present")
    return 0


def patch_bbox(path):
    """The two-anchor patch of func_80070298 in its shard."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    ending = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
    if BBOX_FUNC not in ranges:
        raise ValueError(f"{BBOX_FUNC} range not found in {path.name}")
    start, end = ranges[BBOX_FUNC]
    body = lines[start:end]
    have_a = [l.rstrip("\r\n") for l in body].count(ENTRY_BLOCK)
    have_b = [l.rstrip("\r\n") for l in body].count(RETURN_BLOCK)
    if have_a or have_b:
        if have_a == 1 and have_b == 1:
            return 0, 2
        raise ValueError(
            f"{BBOX_FUNC} in {path.name} has a partial bbox patch "
            f"(entry block x{have_a}, return block x{have_b})")
    if any("BBoxCache" in l for l in body):
        raise ValueError(
            f"{BBOX_FUNC} in {path.name} mentions BBoxCache but the inserted "
            "blocks are not present exactly as inserted")
    a_idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == ENTRY_ANCHOR]
    if len(a_idx) != 1:
        raise ValueError(
            f"entry anchor ({ENTRY_ANCHOR!r}) matched {len(a_idx)} times "
            f"in {BBOX_FUNC}, expected exactly 1")
    b_idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == RETURN_LABEL]
    if len(b_idx) != 1:
        raise ValueError(
            f"return anchor ({RETURN_LABEL!r}) matched {len(b_idx)} times "
            f"in {BBOX_FUNC}, expected exactly 1")
    # Insert the return block first (later in the body) so the entry
    # index stays valid, then the entry block, then the declarations
    # before the function definition.
    body.insert(b_idx[0] + 1, RETURN_BLOCK + ending)
    body.insert(a_idx[0] + 1, ENTRY_BLOCK + ending)
    lines[start:end] = body
    defn_idx = next(i for i, line in enumerate(lines) if BBOX_DEFN in line)
    for j, decl in enumerate(DECLS_BBOX):
        lines.insert(defn_idx + j, decl + ending)
    path.write_text("".join(lines), encoding="utf-8")
    return 2, 0


if __name__ == "__main__":
    raise SystemExit(main())
