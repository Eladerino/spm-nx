# Stock-clock performance: handoff (Nov 8 2026)

Goal: 60 fps at stock clocks (1020/384/1331), meaning a game-thread frame under 16.7 ms. Handheld only.

## Where things stand

| Area | fps now | Bound by |
|---|---|---|
| Light 2D and 3D | 60 | nothing |
| Heavy 2D (he2_03, he1_05) | ~40 | GX worker replay ~15 ms, plus the end-of-frame hand-off |
| Heavy 3D (he1_05, he2_03) | 30 (36 with `gxsvclate=1`) | game thread: ~18.5-19 ms of its own work, plus a 2-3 ms CopyDisp wait |

Threads: game thread on core 0, Aurora's GX decode thread on core 1, and the GX worker plus Aurora's frame worker sharing core 2.

## Branches and PRs

- `main`: the release line (v0.2.0 = 7b9c6a28 on the card).
- PR #26, `claude/project-thread-4csxi6`: all switches are default off, and everything is measurement or opt-in:
  - `gxfutex`: lossless worker wake. It works (late wakes go to 0) but gives no measurable speedup. Keeping it is harmless.
  - `gxsvclate`: a drain waiting on the worker runs the guest timing service only after 4 ms. **This gave heavy 3D 30 to 36 fps on 2 of 2 boots, with clean audio.** One of its two boots showed a 2D lock at 2 retraces (~30 fps). Section 7 decides it.
  - Counters: worker CPU vs wall in replay, KB unreplayed at drains, owner wake causes, timing-service ms, renderer waits inside replay, and a retrace / frame-interval histogram.
- `claude/intrinsics`: native sqrt pasted into callers by the translator. It's bit-exact but gave no reliable gain, so it's parked.

## What was tried and ruled out (don't redo)

- Small translator tweaks: compare-branch fusion, sqrt intrinsic, and the CR, call-guard and callcount gates. All are below noise. The game thread's own work barely moves.
- Bare-fcvt single rounding: fcvtcheck said DIFFERENT, so the masked form stays.
- Lost worker wakes as the cause of the CopyDisp wait: wrong. They were real but tiny.
- Reverb off, native reverb: no gain, because it runs in retrace idle. Native reverb is also blocked by copyright.

## The slow-boot state (random, about 1 boot in 4-6)

Some boots run heavy 2D at ~32 fps instead of ~40 for the whole session. In B3 (svclate run):
- GX worker replay wall is 20.3 ms vs 15.3 ms, but its CPU time is unchanged (13.9 vs 14.3). The worker is blocked, not doing more work.
- The worker waits on Aurora's decode queue being full: 6.4 ms vs 0.9 ms. The decode thread itself waits on the frame worker's seal: 3.25 ms vs 0.19 ms.
- So the chain is: the frame worker (core 2) runs late, the decode thread stalls on the seal, the 8-batch decode queue fills, and the GX worker blocks.
- Open question: what makes the frame worker late in those boots. Look at `[frametime]` encode / frametotal in a slow boot. Suspects: core-2 contention between the frame worker and the GX worker (the GX worker yields every 1 ms), or the frame worker's GPU fence waits.

## Reading the section 7 run

Logs are in the section 7 folders under `live/` (one per boot; the runbook names them).

1. **2D lock rate with gxsvclate on:** for each boot's he1_05 2D and he2_03 2D, check the `[vi]` retraces per frame. Normal is about 1.5; the lock is about 1.95-2.0 with high guest idle. If it locks in 2 or more of 4 boots, gxsvclate needs a fix before it goes default (see below).
2. **3D smoothness:** use the retrace and frame-interval histogram line, together with Muse's eye report. 36 fps means a mix of 1-retrace and 2-retrace frames. If it feels worse than a steady 30, don't default it.
3. **Slow boots:** if any boot shows worker replay of about 20 ms, read the new renderer-wait line (decode queue, decode sync, frame worker phase, staging map, deko queue lock, GPU fence, shader) to see which wait grew.

Decision rule: if 3D holds 33+ fps, 2D doesn't lock more often than with it off, and it feels fine, make `gxsvclate` default on. That's a one-line change in `switch/deko/dk.h` (`gxSvcLate = true`), then `spm_gxsvclate_set(g_diag.gxSvcLate)` is already called at boot. Then merge PR #26.

## Likely fix if gxsvclate causes the 2D lock

Why it helps 3D (inferred): when the timing service runs during the CopyDisp wait, it processes a retrace that has just passed. The game's VIWaitForRetrace then waits for the next one, which forces 2 retraces per frame. Deferring the service lets the frame catch the retrace it's about to reach. The 2D lock may be the reverse case. Options:
- Lower `kServiceAfter` (`gx_worker.cpp`, 4 ms) to 2 ms, or make it relative to the next retrace time (service only if a retrace is due within the wait).
- Look at what VIWaitForRetrace does when a retrace was serviced mid-drain (`hle/vi.cpp`).

## Next big levers (after gxsvclate)

1. **Slow-boot fix:** stop the frame worker and the GX worker fighting over core 2. Try moving the GX worker's 1 ms yield, or giving the frame worker priority, after the renderer-wait line confirms the cause. Worth up to ~8 fps in 2D on affected boots.
2. **Overlap the frame** (design scoped, not built): make the CopyDisp drain non-blocking, with CopyDisp as an in-order present record, and drop the per-frame ring rewind (use a two-sided ring). The game then starts the next frame while the worker finishes. Estimate: 2-4 ms off the 3D game thread, at the cost of up to 1 frame of extra latency. Sync points that must stay: DrawDone/SetDrawSync tokens, bbox/peek, and CPU-read EFB copies.
3. **2D floor:** worker replay is ~15 ms, so 2D tops out around 50-60 fps even with perfect overlap. Profile the worker's replay (CallDisplayList cost) next.

## Rules (from Brandon)

- Measure at stock clocks only. Use godmode=1 and perftimer=1 for runs. Identify NROs by SHA-256.
- Follow the console claim protocol (`/agent-claim.txt`), and screenshot-check that SPM is the first tile before launching.
- Commits go out as Brandon.
- Restore the card to the release build and the dk_diag to its pre-run bytes after every run.
