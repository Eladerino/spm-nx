#!/usr/bin/env python3
"""Patch the copied generated evt NPC handlers so a missing NPC cannot panic.

build-switch.sh copies wiicompiled/generated into the build tree; this script
edits that copy (never the translator's output in the repo), the same pattern
as patch-anim-draw.py.

Why: npcNameToPtr (0x801B5EE4) ends in the retail not-found assert at
npcdrv.c:7055 ("There is no name [ %s ]"). evt_npc_set_position resolves its
name through evtNpcNameToPtr (0x800FD4E8), whose fallthrough calls the
asserting lookup, and the handler then writes through the result with no NULL
check. gn1_01's opening script positions "hiroshi" before he exists; reached
from he2_03's far end (dkserver warp world_end), that assert ended the Nov 4
PGO training session and Brandon's Nov 5 recorder run. Full mechanism:
~/workspace/spm-audit-findings/panic-npcdrv-2026-11-05/INVESTIGATION.md.

The game ships the non-asserting pair for exactly this: evtNpcNameToPtr_
NoAssert (0x800FD628) is token-for-token identical to evtNpcNameToPtr except
its fallthrough calls npcNameToPtr_NoAssert (0x801B5F8C), which returns 0 on a
miss. So, per handler below, this patch swaps the resolver call to the
NoAssert variant and inserts a NULL check right after the call's register
reload: on a miss it calls the runtime hook spm_npc_guard_miss (which logs,
rate-limited) and jumps to the handler's own epilogue, so the evt command
completes with its normal return code (2) and no write happens. Handlers
whose result is already NULL-checked (evt_npc_rotate) get the swap only.

Audited but deliberately NOT patched here:
- evt_npc_walk_to (0x800FF22C): also resolves through the asserting lookup
  and dereferences unchecked, but it is a multi-phase command with several
  return sites and its post-resolution phases were not fully decoded; a
  blind epilogue jump could corrupt its walk state. Follow-up audit item.
- func_800FD98C / func_800FDA58 / func_800FDB3C: unnamed part-accessor
  handlers that hand the result to npcGetPartById; a different command
  family from the position/rotation/scale handlers, left for the same
  follow-up audit.
- Every other caller of evtNpcNameToPtr (228 call sites tree-wide): the
  guard is scoped to the evt command handlers above on purpose; changing
  the shared resolver would change semantics for engine callers that were
  not audited.

Anchors come from guest addresses, so they survive regeneration; shard file
names are content hashes and are found by search. Any missing anchor fails
the build loudly rather than shipping without the guard.
"""
import pathlib
import sys

DECL = 'extern "C" void spm_npc_guard_miss(uint32_t nameAddr, const char* site);'
ASSERT_CALL = "MKW_STATIC_TRANSLATED_CALL(0x800FD4E8u, func_800FD4E8, ctx);"
NOASSERT_CALL = "MKW_STATIC_TRANSLATED_CALL(0x800FD628u, func_800FD628, ctx);"
RELOAD_END = "    xer = ctx->xer;"

# func -> (site name, epilogue label or None for swap-only, name expression,
#          expected resolver calls, set r3=2 before the epilogue jump)
HANDLERS = {
    "func_800FD870": ("evt_npc_set_position", "loc_800FD960", "r30", 2, False),
    "func_800FD778": ("evt_npc_get_position", "loc_800FD854", "r30", 1, False),
    "func_800FDC0C": ("evt_npc_get_rotation", "loc_800FDCA0", "0", 1, False),
    "func_800FE584": ("evt_npc_set_scale", "loc_800FE62C", "r30", 1, False),
    "func_800FDCBC": ("evt_npc_rotate", None, None, 1, False),
    "func_800FFB14": ("evt_npc_jump_to", "loc_800FFD10", "0", 1, True),
}


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


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-npc-guard.py <generated dir>", file=sys.stderr)
        return 1
    gen = pathlib.Path(sys.argv[1])
    shards = sorted((gen / "build_shards" / "base_common").glob("*.cpp"))

    # Locate each target function's shard.
    home = {}
    for name in HANDLERS:
        defn = f'extern "C" void {name}(CpuContext* MKW_RESTRICT ctx)'
        for shard in shards:
            if defn in shard.read_text(encoding="utf-8", errors="replace"):
                home[name] = shard
                break
        else:
            print(f"patch-npc-guard: no shard defines {name}", file=sys.stderr)
            return 1

    by_shard = {}
    for name, shard in home.items():
        by_shard.setdefault(shard, []).append(name)

    for shard, names in by_shard.items():
        text = shard.read_text(encoding="utf-8")
        lines = text.splitlines(keepends=True)
        ending = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
        ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
        decl_done = "spm_npc_guard_miss" in text
        for name in names:
            site, epilogue, name_expr, expect, set_r3 = HANDLERS[name]
            if name not in ranges:
                print(f"patch-npc-guard: {name} range not found in {shard.name}", file=sys.stderr)
                return 1
            start, end = ranges[name]
            body = lines[start:end]
            calls = [i for i, l in enumerate(body) if ASSERT_CALL in l]
            swapped = [i for i, l in enumerate(body) if NOASSERT_CALL in l]
            if not calls and len(swapped) == expect:
                continue  # already patched on a previous run
            if len(calls) != expect:
                print(
                    f"patch-npc-guard: {name}: expected {expect} asserting resolver "
                    f"calls, found {len(calls)} (and {len(swapped)} already swapped)",
                    file=sys.stderr,
                )
                return 1
            if epilogue is not None and not any(epilogue + ":" in l for l in body):
                print(f"patch-npc-guard: {name}: epilogue {epilogue} not found", file=sys.stderr)
                return 1
            if name_expr == "r30":
                # The guard logs the NPC name from r30; that only works if every
                # resolver call in this handler round-trips r30 through ctx
                # (the resolver restores the caller's callee-saved registers).
                for ci in calls:
                    window = body[max(0, ci - 40):ci]
                    if not any("ctx->gpr[30] = r30;" in l for l in window):
                        print(
                            f"patch-npc-guard: {name}: call at body line {ci} does not "
                            "preserve r30 through ctx; refusing to log from it",
                            file=sys.stderr,
                        )
                        return 1
            # Swap the calls (bottom-up so indices stay valid), then insert the
            # NULL check after each call's register reload.
            for ci in reversed(calls):
                body[ci] = body[ci].replace(ASSERT_CALL, NOASSERT_CALL)
                if epilogue is None:
                    continue
                ins = None
                for j in range(ci + 1, min(ci + 40, len(body))):
                    if body[j].rstrip("\r\n") == RELOAD_END:
                        ins = j
                        break
                if ins is None:
                    print(
                        f"patch-npc-guard: {name}: register reload after call at "
                        f"body line {ci} not found",
                        file=sys.stderr,
                    )
                    return 1
                r3 = "r3 = 2; " if set_r3 else ""
                check = (
                    f"    if (r3 == 0) {{ spm_npc_guard_miss({name_expr}, "
                    f'"{site}"); {r3}goto {epilogue}; }}' + ending
                )
                body.insert(ins + 1, check)
            lines[start:end] = body
            # Ranges after this function shifted; recompute for the next handler.
            ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
        if not decl_done:
            first = min(ranges[n][0] for n in names)
            lines.insert(first, DECL + ending)
        shard.write_text("".join(lines), encoding="utf-8")
        print(f"patch-npc-guard: patched {shard.name}: {', '.join(sorted(names))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
