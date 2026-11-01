#!/usr/bin/env python3
"""Patch the copied generated animPoseDrawMtx so it consults the runtime hook.

build-switch.sh copies wiicompiled/generated into the build tree; this script
edits that copy (never the translator's output in the repo). It finds the shard
that defines func_800496B0 (animPoseDrawMtx), declares the runtime hook
spm_suppress_anim_pose next to the definition, and inserts the check just
before the function's first goto, where the translated code has loaded its
registers but has not touched anything yet. If the hook returns true the draw
is skipped: the demo's Wii Remote model never renders.

Anchors come from the guest address, so they survive regeneration; the shard
file name is a content hash and is found by search, not assumed. A missing
anchor fails the build loudly rather than shipping without the hook.
"""
import pathlib
import sys

DECL = 'extern "C" bool spm_suppress_anim_pose(CpuContext* ctx);'
CHECK = '    if (spm_suppress_anim_pose(ctx)) { return; }'
DEFN = 'extern "C" void func_800496B0(CpuContext* MKW_RESTRICT ctx)'
GOTO = '    goto loc_800496B0;'


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-anim-draw.py <generated dir>", file=sys.stderr)
        return 1
    gen = pathlib.Path(sys.argv[1])
    shards = sorted((gen / "build_shards" / "base_common").glob("*.cpp"))
    target = None
    for shard in shards:
        if DEFN in shard.read_text(encoding="utf-8", errors="replace"):
            target = shard
            break
    if target is None:
        print("patch-anim-draw: no shard defines func_800496B0", file=sys.stderr)
        return 1
    text = target.read_text(encoding="utf-8")
    if "spm_suppress_anim_pose" in text:
        print(f"patch-anim-draw: {target.name} already patched")
        return 0
    lines = text.splitlines(keepends=True)
    ending = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    defn_idx = next(i for i, line in enumerate(lines) if DEFN in line)
    goto_idx = next(
        (i for i in range(defn_idx, len(lines)) if lines[i].rstrip("\r\n") == GOTO),
        None,
    )
    if goto_idx is None:
        print("patch-anim-draw: entry goto not found after the definition", file=sys.stderr)
        return 1
    lines.insert(defn_idx, DECL + ending)
    # The declaration inserted above shifted the goto down one line; inserting
    # at its new index puts the check immediately before it.
    lines.insert(goto_idx + 1, CHECK + ending)
    target.write_text("".join(lines), encoding="utf-8")
    print(f"patch-anim-draw: patched {target.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
