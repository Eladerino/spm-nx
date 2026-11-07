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

The gn1_01 world_end scene script (0x80D567D8) resolves more names than the
original audit set: with set_position guarded, the Nov 6 run advanced one
script word and panicked in evt_npc_set_property, and every later
name-resolving handler in that scene (and in world_restart, 0x80D56B50)
would panic in turn. This script also covers the rest of that sequence (full
analysis: ~/workspace/spm-audit-findings/panic-npcdrv-2026-11-05/
SECOND-CALLER-2026-11-06.md): evt_npc_set_property, evt_npc_set_anim and
evt_npc_flag8_onoff fit the pattern above unchanged. evt_npc_set_axis_
movement_unit, func_801051B4 and evt_npc_finish_flip_instant are
single-block handlers with no epilogue label, so for them the check presets
r3 = 2 and jumps to a synthesised label placed immediately before the tail
restore sequence (HANDLERS_TAIL below); each body sets r3 = 2 on its success
path, so the preset yields the same completion code the command would have
returned on a hit.

Audited but deliberately NOT patched here:
- evt_npc_walk_to (0x800FF22C): also resolves through the asserting lookup
  and dereferences unchecked, but it is a multi-phase command with several
  return sites and its post-resolution phases were not fully decoded; a
  blind epilogue jump could corrupt its walk state. Follow-up audit item.
- func_800FD98C / func_800FDA58 / func_800FDB3C: unnamed part-accessor
  handlers that hand the result to npcGetPartById; a different command
  family from the position/rotation/scale handlers, left for the same
  follow-up audit.
- func_80108A00: resolves a name, then returns a predicate computed from
  the NPC by func_801C8680 ((nonzero) ? 2 : 0). evtmgrCmd treats 0 as
  "run me again" and 2 as "complete", so the return value is data, not a
  completion code: on a miss there is no NPC to evaluate, 0 would re-poll
  the missing NPC forever and 2 would fabricate a finished movement.
  No provable no-op skip value exists, so it is left unpatched rather
  than guessed (audit in SECOND-CALLER-2026-11-06.md's follow-up).
- Every other caller of evtNpcNameToPtr (228 call sites tree-wide): the
  guard is scoped to the evt command handlers above on purpose; changing
  the shared resolver would change semantics for engine callers that were
  not audited.

Anchors come from guest addresses, so they survive regeneration; shard file
names are content hashes and are found by search. Any missing anchor fails
the build loudly rather than shipping without the guard.
"""
import pathlib
import re
import sys

DECL = 'extern "C" void spm_npc_guard_miss(uint32_t nameAddr, const char* site);'
ASSERT_CALL = "MKW_STATIC_TRANSLATED_CALL(0x800FD4E8u, func_800FD4E8, ctx);"
NOASSERT_CALL = "MKW_STATIC_TRANSLATED_CALL(0x800FD628u, func_800FD628, ctx);"
RELOAD_END = "    xer = ctx->xer;"

# Tail-form anchors (HANDLERS_TAIL): the saved-LR reload in the tail restore
# sequence, in generated form, and the register reload lines that follow a
# translated call (the xer reload is last when the handler carries one).
TAIL_LR_READ = "    r0 = MemoryInline::FlatRead32((r1 + 20));"
TAIL_LR_STORE = "    ctx->lr = r0;"
TAIL_LR_SAVE = "MemoryInline::FlatWriteRam32((r1 + 20), r0);"
RELOAD_RE = re.compile(
    r"    (?:r\d+ = ctx->gpr\[\d+\];|f\d+ = ctx->fpr\[\d+\];"
    r"|cr = ctx->cr;|ctr = ctx->ctr;|xer = ctx->xer;)"
)
RESTORE_RE = re.compile(r"    (r\d+) = MemoryInline::FlatRead32\(\(r1 \+ (\d+)\)\);")
SAVE_RE = re.compile(r"MemoryInline::FlatWriteRam32\(\(r1 \+ (\d+)\), (r30|r31)\);")

# func -> (site name, epilogue label or None for swap-only, name expression,
#          expected resolver calls, set r3=2 before the epilogue jump)
HANDLERS = {
    "func_800FD870": ("evt_npc_set_position", "loc_800FD960", "r30", 2, False),
    "func_800FD778": ("evt_npc_get_position", "loc_800FD854", "r30", 1, False),
    "func_800FDC0C": ("evt_npc_get_rotation", "loc_800FDCA0", "0", 1, False),
    "func_800FE584": ("evt_npc_set_scale", "loc_800FE62C", "r30", 1, False),
    "func_800FDCBC": ("evt_npc_rotate", None, None, 1, False),
    "func_800FFB14": ("evt_npc_jump_to", "loc_800FFD10", "0", 1, True),
    "func_801042B4": ("evt_npc_set_property", "loc_801044D8", "0", 1, False),
    "func_80102728": ("evt_npc_set_anim", "loc_801027A0", "0", 1, False),
    "func_80102CAC": ("evt_npc_flag8_onoff", "loc_80102D18", "0", 1, False),
}

# Single-block handlers with no epilogue label to jump to. Same fields as
# HANDLERS, except the label field holds the synthesised label the patch
# places immediately before the function's tail restore sequence; set_r3 is
# always True here because the jump skips the body's own r3 = 2;.
HANDLERS_TAIL = {
    "func_80104FF0": ("evt_npc_set_axis_movement_unit", "loc_npc_guard_tail_80104FF0", "0", 1, True),
    "func_801051B4": ("func_801051B4", "loc_npc_guard_tail_801051B4", "0", 1, True),
    "func_80104BC4": ("evt_npc_finish_flip_instant", "loc_npc_guard_tail_80104BC4", "0", 1, True),
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


def patch_tail_body(body, name, ending):
    """Patch one HANDLERS_TAIL handler's body in place.

    Returns an error string, or None on success (including the
    already-patched case, verified rather than assumed). Every anchor is
    checked before anything is edited, so a mismatch aborts the run
    without touching the body.
    """
    site, label, name_expr, expect, set_r3 = HANDLERS_TAIL[name]

    def err(msg):
        return f"patch-npc-guard: {name}: {msg}"

    if not set_r3:
        return err("tail-form entries must preset r3 = 2; the jump skips the body's own r3 = 2;")
    calls = [i for i, l in enumerate(body) if ASSERT_CALL in l]
    swapped = [i for i, l in enumerate(body) if NOASSERT_CALL in l]
    checks = [l for l in body if "spm_npc_guard_miss" in l and f'"{site}"' in l]
    labels = [i for i, l in enumerate(body) if l.rstrip("\r\n") == label + ":"]
    if not calls and len(swapped) == expect:
        # Already patched on a previous run: the guard check and the
        # synthesised label must both still be there, exactly as inserted.
        if len(checks) == expect and len(labels) == 1:
            return None
        return err("resolver already swapped but the guard check or tail label is missing")
    if len(calls) != expect:
        return err(
            f"expected {expect} asserting resolver calls, found {len(calls)} "
            f"(and {len(swapped)} already swapped)"
        )
    if labels:
        return err(f"synthesised label {label} already present in an unpatched body")
    if checks:
        return err("guard check already present in an unpatched body")
    # Frame shape: a 16-byte frame with a single return site.
    if sum(1 for l in body if l.rstrip("\r\n") == "    r1 = (r1 + -16);") != 1:
        return err("expected exactly one 16-byte frame allocation")
    if sum(1 for l in body if l.strip() == "return;") != 1:
        return err("expected exactly one return (single-block handler)")
    saves = {}
    for l in body:
        m = SAVE_RE.search(l)
        if m:
            saves[m.group(2)] = int(m.group(1))
    if "r31" not in saves:
        return err(f"prologue saves {sorted(saves)} are not the expected r31[/r30] frame")
    if sum(1 for l in body if TAIL_LR_SAVE in l) != 1:
        return err("expected exactly one saved-LR store in the prologue")
    # The tail restore anchor: the saved-LR reload and its ctx->lr store,
    # exactly once, followed by the frame release.
    pairs = [
        i
        for i in range(len(body) - 1)
        if body[i].rstrip("\r\n") == TAIL_LR_READ and body[i + 1].rstrip("\r\n") == TAIL_LR_STORE
    ]
    if len(pairs) != 1:
        return err(f"expected exactly one tail LR restore, found {len(pairs)}")
    tail = pairs[0]
    if tail + 2 >= len(body) or body[tail + 2].rstrip("\r\n") != "    r1 = (r1 + 16);":
        return err("tail LR restore is not followed by the frame release")
    # Walk back over the restore sequence the synthesised label must
    # precede: the callee-saved restores and the body's own r3 = 2;,
    # in whichever order the translator emitted them.
    restores = {}
    r3_sets = 0
    j = tail - 1
    while j >= 0:
        m = RESTORE_RE.fullmatch(body[j].rstrip("\r\n"))
        if m:
            restores[m.group(1)] = int(m.group(2))
            j -= 1
            continue
        if body[j].rstrip("\r\n") == "    r3 = 2;":
            r3_sets += 1
            j -= 1
            continue
        break
    if r3_sets != 1:
        return err(f"expected exactly one r3 = 2; before the tail restore, found {r3_sets}")
    if restores != saves:
        return err(f"tail restores {restores} do not match prologue saves {saves}")
    label_at = j + 1
    # The resolver call's register reload: the consecutive reload lines
    # right after the call, ending within the same 40-line window the
    # epilogue form uses, and reloading r3 (the resolved NPC pointer).
    ci = calls[0]
    ins = ci
    while ins + 1 < len(body) and RELOAD_RE.fullmatch(body[ins + 1].rstrip("\r\n")):
        ins += 1
    if ins == ci:
        return err(f"register reload after call at body line {ci} not found")
    if ins > ci + 40:
        return err(f"register reload after call at body line {ci} ends past the 40-line window")
    if not any(l.rstrip("\r\n") == "    r3 = ctx->gpr[3];" for l in body[ci + 1 : ins + 1]):
        return err(f"register reload after call at body line {ci} does not reload r3")
    if label_at <= ins:
        return err("tail restore does not follow the resolver reload")
    # All anchors verified: swap the call, insert the check after the
    # reload, and place the synthesised label before the tail restore
    # sequence (its index shifts by one for the inserted check line).
    body[ci] = body[ci].replace(ASSERT_CALL, NOASSERT_CALL)
    check = (
        f"    if (r3 == 0) {{ spm_npc_guard_miss({name_expr}, "
        f'"{site}"); r3 = 2; goto {label}; }}' + ending
    )
    body.insert(ins + 1, check)
    body.insert(label_at + 1, label + ":" + ending)
    return None


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch-npc-guard.py <generated dir>", file=sys.stderr)
        return 1
    if set(HANDLERS) & set(HANDLERS_TAIL):
        print("patch-npc-guard: a function is listed in both HANDLERS and HANDLERS_TAIL", file=sys.stderr)
        return 1
    all_handlers = {**HANDLERS, **HANDLERS_TAIL}
    gen = pathlib.Path(sys.argv[1])
    shards = sorted((gen / "build_shards" / "base_common").glob("*.cpp"))

    # Locate each target function's shard.
    home = {}
    for name in all_handlers:
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
            if name not in ranges:
                print(f"patch-npc-guard: {name} range not found in {shard.name}", file=sys.stderr)
                return 1
            start, end = ranges[name]
            body = lines[start:end]
            if name in HANDLERS_TAIL:
                err = patch_tail_body(body, name, ending)
                if err is not None:
                    print(err, file=sys.stderr)
                    return 1
                lines[start:end] = body
                # Ranges after this function shifted; recompute for the next handler.
                ranges = {n: (s, e) for n, s, e in func_ranges(lines)}
                continue
            site, epilogue, name_expr, expect, set_r3 = HANDLERS[name]
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
