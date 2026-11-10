#!/usr/bin/env python3
"""Patch the copied generated bodies for the hitNameToPtr persistent index.

build-switch.sh copies wiicompiled/generated into the build tree; this
script edits that copy (never the translator's output in the repo), the
same pattern as patch-anim-draw.py / patch-npc-guard.py /
patch-hitcache.py / patch-bboxcache.py. It patches twelve bodies, each
at pinned anchors, and hard-fails the build unless every anchor matches
exactly once in its body (spm-hit-index design (b)):

func_8006F808 (hitNameToPtr), two anchors:
  ANCHOR A (entry): immediately AFTER the line `    r25 = r3;` in the body
  (prologue and stack saves complete by then; r25 holds the untouched name
  argument; the mapGetWork inline that follows, which clobbers r3, has not
  run). The inserted block dispatches on the diag flags first, so with both
  keys off nothing else runs: shadow on -> HitIndexShadowNote (compute and
  stash the would-be answer, never jump); else hitindex on ->
  HitIndexTry, and on an answer it presets r3 and jumps to the common
  return block. Both on: shadow wins. HitIndexCallBegin latches the mode
  it found at entry (bbox code review F1).
  ANCHOR B (return): the head of the common return block loc_8006F8A0, the
  body's only exit, immediately after its label line. The inserted block
  consults ONLY the entry latch (HitIndexCallBegun / HitIndexCallShadow,
  closed by HitIndexCallEnd) and never re-reads the diag flags, so a live
  flag flip mid-scan takes effect from the next call's entry: in latched
  shadow mode it hands the scan's result (the r3 local) to
  HitIndexShadowCompare. With both keys off at entry the latch is unset
  and the block does nothing at all.
  The fnprobe-style "before the first goto" anchor is FORBIDDEN here: it
  precedes the prologue, and jumping to the epilogue from there would
  restore unsaved registers and misalign the stack.

Writer hooks, one anchored insert per site, each a bare call to a bump
entry point with NO guest-state argument (a bump is one increment of the
single global generation plus one per-class counter increment; no hook
reads guest memory):
  H1 hitEntry (func_8006B614) exit: before the body's sole `return;` ->
     HitIndexBumpStructure
  H2 hitReinit (func_8006ADB4) entry: after the entry label
     loc_8006ADB4: -> HitIndexBumpStructure
  H3 hitEntryMOBJ (func_8006B7B4) exit: before the body's sole `return;`
     -> HitIndexBumpFill
  H4 _hitEnt (func_8006AE54) exit: before the body's sole `return;` ->
     HitIndexBumpFill
  H5 hitDelete (func_8006B9A8): after the label loc_8006BA24: - exactly
     the sole entry+8 write's block and the sole convergence point of
     both successful-delete paths -> HitIndexBumpDelete
  H6 hitFlgOn (func_8006BD78) exit: before the body's sole `return;` ->
     HitIndexBumpFlags
  H7 hitFlgOff (func_8006C088) exit: before the body's sole `return;` ->
     HitIndexBumpFlags
  H8 hitObjFlagOn (func_8006BC68) exit: before the body's sole `return;`
     -> HitIndexBumpFlags
  H9 hitObjFlagOff (func_8006BCF0) exit: before the body's sole
     `return;` -> HitIndexBumpFlags
  H10 hitGrpFlagOn (func_8006C39C) exit: before the body's sole
     `return;` -> HitIndexBumpFlags
  H11 hitGrpFlagOff (func_8006C594) exit: before the body's sole
     `return;` -> HitIndexBumpFlags
  H8-H11 are the census amendment (spm-hit-index-census.md): the four
  flag wrappers write the found entry's +0 DIRECTLY with their own
  caller mask - the Obj pair never calls hitFlgOn/hitFlgOff at all, and
  the Grp pair calls them only for sibling subtrees - so H6/H7 alone
  would leave a live stale-index path (an EVT hitObjFlagOn(name, 0x80)
  flipping scan visibility with no bump).
  There is no fallback anchor for any site: if an anchor does not match
  exactly once, the build fails loudly. (Exit anchors presuppose a sole
  return path per body, verified by the census close-out in
  spm-hit-index-census.md.)

The helper itself lives in the runtime (wiicompiled/runtime/src/hle/gx/
hit_index.cpp, hit_index.h): the declarations below are its entry points.

Anchors come from the guest addresses and the bodies' own lines, so they
survive regeneration; the shard file names are content hashes and are
found by search. Any missing or duplicated anchor fails the build loudly
rather than shipping without the index.
"""
import pathlib
import sys

DECLS_INDEX = [
    'extern "C" uint64_t HitIndexCallBegin();',
    'extern "C" bool HitIndexCallBegun();',
    'extern "C" bool HitIndexCallShadow();',
    'extern "C" void HitIndexCallEnd();',
    'extern "C" bool HitIndexEnabled();',
    'extern "C" bool HitIndexShadowEnabled();',
    'extern "C" bool HitIndexTry(CpuContext* ctx, uint32_t namePtr, uint32_t* outResult);',
    'extern "C" void HitIndexShadowNote(CpuContext* ctx, uint32_t namePtr);',
    'extern "C" void HitIndexShadowCompare(uint32_t scanResult);',
]
DECL_STRUCTURE = 'extern "C" void HitIndexBumpStructure();'
DECL_FILL = 'extern "C" void HitIndexBumpFill();'
DECL_DELETE = 'extern "C" void HitIndexBumpDelete();'
DECL_FLAGS = 'extern "C" void HitIndexBumpFlags();'

INDEX_FUNC = "func_8006F808"
INDEX_DEFN = 'extern "C" void func_8006F808(CpuContext* MKW_RESTRICT ctx)'
ANCHOR_A_LINE = "    r25 = r3;"
BLOCK_A = (
    "    { if (HitIndexShadowEnabled() || HitIndexEnabled()) { (void)HitIndexCallBegin(); "
    "if (HitIndexCallShadow()) { HitIndexShadowNote(ctx, r25); } "
    "else if (HitIndexEnabled()) { uint32_t _hi; if (HitIndexTry(ctx, r25, &_hi)) { r3 = _hi; "
    "goto loc_8006F8A0; } } } }"
)
ANCHOR_B_LABEL = "loc_8006F8A0:"
BLOCK_B = (
    "    { if (HitIndexCallBegun()) { "
    "if (HitIndexCallShadow()) { HitIndexShadowCompare(r3); } HitIndexCallEnd(); } }"
)

# Writer sites: (function, defn, kind, anchor, block, decl)
# kind "after": insert after the anchor label line.
# kind "before_return": insert before the body's sole `    return;` line.
WRITERS = [
    ("func_8006B614", 'extern "C" void func_8006B614(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpStructure(); }", DECL_STRUCTURE),
    ("func_8006ADB4", 'extern "C" void func_8006ADB4(CpuContext* MKW_RESTRICT ctx)',
     "after", "loc_8006ADB4:", "    { HitIndexBumpStructure(); }", DECL_STRUCTURE),
    ("func_8006B7B4", 'extern "C" void func_8006B7B4(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFill(); }", DECL_FILL),
    ("func_8006AE54", 'extern "C" void func_8006AE54(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFill(); }", DECL_FILL),
    ("func_8006B9A8", 'extern "C" void func_8006B9A8(CpuContext* MKW_RESTRICT ctx)',
     "after", "loc_8006BA24:", "    { HitIndexBumpDelete(); }", DECL_DELETE),
    ("func_8006BD78", 'extern "C" void func_8006BD78(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
    ("func_8006C088", 'extern "C" void func_8006C088(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
    ("func_8006BC68", 'extern "C" void func_8006BC68(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
    ("func_8006BCF0", 'extern "C" void func_8006BCF0(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
    ("func_8006C39C", 'extern "C" void func_8006C39C(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
    ("func_8006C594", 'extern "C" void func_8006C594(CpuContext* MKW_RESTRICT ctx)',
     "before_return", None, "    { HitIndexBumpFlags(); }", DECL_FLAGS),
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
        if any("HitIndex" in l for l in body):
            raise ValueError(
                f"{func} in {path.name} mentions HitIndex but the expected block "
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


def patch_index(path):
    """The two-anchor patch of func_8006F808 in its shard."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    ending = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
    if INDEX_FUNC not in ranges:
        raise ValueError(f"{INDEX_FUNC} range not found in {path.name}")
    start, end = ranges[INDEX_FUNC]
    body = lines[start:end]
    have_a = [l.rstrip("\r\n") for l in body].count(BLOCK_A)
    have_b = [l.rstrip("\r\n") for l in body].count(BLOCK_B)
    if have_a or have_b:
        if have_a == 1 and have_b == 1:
            return 0, 2
        raise ValueError(
            f"{INDEX_FUNC} in {path.name} has a partial hitindex patch "
            f"(entry block x{have_a}, return block x{have_b})")
    if any("HitIndex" in l for l in body):
        raise ValueError(
            f"{INDEX_FUNC} in {path.name} mentions HitIndex but the inserted "
            "blocks are not present exactly as inserted")
    a_idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == ANCHOR_A_LINE]
    if len(a_idx) != 1:
        raise ValueError(
            f"entry anchor ({ANCHOR_A_LINE!r}) matched {len(a_idx)} times "
            f"in {INDEX_FUNC}, expected exactly 1")
    b_idx = [i for i, l in enumerate(body) if l.rstrip("\r\n") == ANCHOR_B_LABEL]
    if len(b_idx) != 1:
        raise ValueError(
            f"return anchor ({ANCHOR_B_LABEL!r}) matched {len(b_idx)} times "
            f"in {INDEX_FUNC}, expected exactly 1")
    # Insert the return block first (later in the body) so the entry
    # index stays valid, then the entry block, then the declarations
    # before the function definition.
    body.insert(b_idx[0] + 1, BLOCK_B + ending)
    body.insert(a_idx[0] + 1, BLOCK_A + ending)
    lines[start:end] = body
    defn_idx = next(i for i, line in enumerate(lines) if INDEX_DEFN in line)
    for j, decl in enumerate(DECLS_INDEX):
        lines.insert(defn_idx + j, decl + ending)
    path.write_text("".join(lines), encoding="utf-8")
    return 2, 0


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-hitindex.py <generated dir>", file=sys.stderr)
        return 1
    gen = pathlib.Path(sys.argv[1])
    shards = sorted((gen / "build_shards" / "base_common").glob("*.cpp"))

    # Group jobs by the shard that defines each function.
    by_shard = {}

    def add_job(defn, job):
        shard = find_shard(shards, defn)
        if shard is None:
            print(f"patch-hitindex: no shard defines {job[0]}", file=sys.stderr)
            return False
        by_shard.setdefault(shard, []).append(job)
        return True

    # The index target: two inserts handled as one job pair inside its body.
    index_job = (INDEX_FUNC, INDEX_DEFN, "index", None, None, DECLS_INDEX)
    if not add_job(INDEX_DEFN, index_job):
        return 1
    for func, defn, kind, anchor, block, decl in WRITERS:
        if not add_job(defn, (func, defn, kind, anchor, block, [decl])):
            return 1

    total_inserted = 0
    total_already = 0
    for shard, jobs in sorted(by_shard.items()):
        # Split the index job out: it needs its own two-anchor handling.
        index_jobs = [j for j in jobs if j[2] == "index"]
        writer_jobs = [j for j in jobs if j[2] != "index"]
        try:
            if writer_jobs:
                ins, alr = patch_file(shard, writer_jobs)
                total_inserted += ins
                total_already += alr
            for _ in index_jobs:
                ins, alr = patch_index(shard)
                total_inserted += ins
                total_already += alr
        except ValueError as exc:
            print(f"patch-hitindex: {exc}", file=sys.stderr)
            return 1
        print(f"patch-hitindex: shard {shard.name}: jobs done "
              f"(cumulative inserted {total_inserted}, already present {total_already})")
    print(f"patch-hitindex: {total_inserted} inserted, {total_already} already present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
