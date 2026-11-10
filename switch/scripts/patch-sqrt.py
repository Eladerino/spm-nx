#!/usr/bin/env python3
"""Patch the copied generated body for the sqrt native (section 81, G3).

build-switch.sh copies wiicompiled/generated into the build tree; this
script edits that copy (never the translator's output in the repo), the
same pattern as patch-anim-draw.py / patch-npc-guard.py /
patch-bboxcache.py.

Gate G3 found that no runtime registration reaches the sqrt thunk's
static call sites on Switch, so routing is a body replacement here:

  - The generated body of func_80261BBC (__ieee754_sqrt, in the base
    shard found by search) is renamed to func_80261BBC_orig, keeping
    the original implementation linkable under that name. The native
    routes positive denormals under guest FPSCR[NI] to it.
  - A new func_80261BBC definition, inserted immediately after the
    renamed body's RECOMP_GUEST_ABI line, forwards to mkw_sqrt_native
    (wiicompiled/runtime/src/hle/sqrt_native.cpp, branch sqrt-native).
    The thunk func_80261E08, the traits entry and both registration /
    dispatch records keep naming func_80261BBC, so every path -- the
    164 static call sites, thunk entry, and direct indirect-dispatch
    entry at 0x80261E08 or 0x80261BBC -- lands in the wrapper.

Anchors are the body's own definition line and its ABI line, so they
survive regeneration; the shard file name is a content hash and is
found by search. Any missing or duplicated anchor fails the build
loudly rather than shipping an unrouted or double-routed body.
"""
import pathlib
import sys

DEFN = 'extern "C" void func_80261BBC(CpuContext* MKW_RESTRICT ctx)'
RENAMED_DEFN = 'extern "C" void func_80261BBC_orig(CpuContext* MKW_RESTRICT ctx)'
THUNK_CALL = 'MKW_STATIC_TRANSLATED_CALL(0x80261BBCu, func_80261BBC, ctx);'
DECL = 'extern "C" void mkw_sqrt_native(CpuContext* ctx);'
WRAPPER = (
    '\n'
    '// Section 81 work-copy routing patch (patch-sqrt.py): the generated\n'
    '// body above is preserved as func_80261BBC_orig; all traffic naming\n'
    '// func_80261BBC lands here and is served by the host native.\n'
    'extern "C" void func_80261BBC(CpuContext* MKW_RESTRICT ctx)\n'
    '{\n'
    '    mkw_sqrt_native(ctx);\n'
    '    return;\n'
    '}\n'
)


def fail(msg):
    print(f"patch-sqrt: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def main():
    generated = pathlib.Path(sys.argv[1])
    candidates = []
    for shard in generated.rglob("*.cpp"):
        # The generated tree also carries CTTR's own copy of this MSL
        # unit at the same guest address (generated/cttr/...); it is a
        # different product's tree and never part of the SPM build's
        # shard set. Scope the search to the SPM tree (review F1).
        if "cttr" in shard.parts:
            continue
        text = shard.read_text(errors="strict")
        if DEFN in text:
            candidates.append((shard, text))
    if len(candidates) != 1:
        fail(f"expected exactly 1 shard defining func_80261BBC, found {len(candidates)}")
    shard, text = candidates[0]

    if "func_80261BBC_orig" in text:
        fail(f"{shard.name}: func_80261BBC_orig already present (double patch?)")
    if text.count(DEFN) != 1:
        fail(f"{shard.name}: definition anchor count {text.count(DEFN)} != 1")
    if text.count(THUNK_CALL) != 1:
        fail(f"{shard.name}: thunk call anchor count {text.count(THUNK_CALL)} != 1")

    # The body's RECOMP_GUEST_ABI line is the first one after the
    # definition; the wrapper goes immediately after it, and the native
    # declaration immediately before the renamed definition.
    defn_at = text.index(DEFN)
    abi_marker = "\n// RECOMP_GUEST_ABI"
    abi_at = text.index(abi_marker, defn_at)
    abi_line_end = text.index("\n", abi_at + 1) + 1

    text = text[:defn_at] + RENAMED_DEFN + text[defn_at + len(DEFN):]
    # Recompute offsets after the rename (same line, longer by 5 chars).
    shift = len(RENAMED_DEFN) - len(DEFN)
    abi_line_end += shift
    text = text[:abi_line_end] + WRAPPER + text[abi_line_end:]
    text = text[:defn_at] + DECL + "\n" + text[defn_at:]

    if text.count(RENAMED_DEFN) != 1 or text.count(DECL) != 1:
        fail(f"{shard.name}: post-patch anchor verification failed")
    shard.write_text(text)
    print(f"patch-sqrt: {shard.name}: func_80261BBC renamed to func_80261BBC_orig, "
          f"wrapper + mkw_sqrt_native declaration inserted")


if __name__ == "__main__":
    main()
