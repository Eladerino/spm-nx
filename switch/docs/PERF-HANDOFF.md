# Performance: state, method and findings (Nov 8 2026)

Goal: 60 fps at stock clocks (1020/384/1331), meaning a game-thread frame under 16.7 ms. Handheld only.

Threads: the game thread on core 0, Aurora's GX decode thread on core 1, and the GX worker plus Aurora's frame worker sharing core 2. Frames quantize to the retrace grid: a frame that misses its retrace waits a whole one, so own work plus waits have to fit under 16.7 ms nearly every frame, not on average.

## Where things stand (stock clocks, 1.5x render scale unless noted)

| Area | fps | Bound by |
|---|---|---|
| Light 2D and 3D | 60 | nothing |
| Heavy 2D (he2_03, he1_05) | ~40 | the game-side chain: GX worker replay ~14 ms plus decode, and the end-of-frame waits |
| Heavy 3D (he1_05, he2_03) | ~35-36 with `gxsvclate` on (30 with it off) | game thread: ~18.5 ms of its own work, plus the CopyDisp wait |

Lowering the render scale helps heavy 2D only until the game chain binds (at 0.75x, heavy 2D sits at ~47 fps with gxsvclate on): fill is not the whole story anywhere heavy.

## What shipped, and what it measured

- **Door hitch** (v0.2.0): the message-string patcher swept all of MEM1 for every patched line on every area change (~600 ms per door). `pmsmemo` (default on) memoizes the patched addresses; the sweep now runs only for lines whose check fails.
- **Drain avoidance** (v0.2.0): `natnosync`, `dcnosync` and `invtexq` (all default on) removed the GX worker drains that most guest calls used to pay; in the Nov 7 stock A/B they took he2_03 from 27.8 to 36.3 fps and he1_05 from 29.0 to 36.7. What is left is the end-of-frame wait at CopyDisp.
- **`gxsvclate`** (default on, PR #26): a drain waiting on the worker runs the guest timing service only after 4 ms instead of at every 1 ms timeout. Heavy 3D rose from ~30 to ~36 fps at stock, judged smoother in play, audio clean. `gxsvclate=0` turns it off.
- **`gxfutex`** (default off, PR #26): the worker sleeps on Horizon's address arbiter instead of a condition variable. Lost wakes go to zero; frame time measures neutral, so it stays an opt-in switch.
- **Save-load hitch** (fixed in v0.3.0): every boot on every build had one frame of ~6.5 s in the load window, a second or two after the player got control. The hunt is the method section below; the culprit was the VI stall watchdog, a bring-up diagnostic that dumped the guest thread table unconditionally 30 s after startup. The dump holds the fiber table mutex while it walks every fiber's guest stack, and at load time that walk took ~6.5 s, so every guest thread switch queued behind it. Two-part fix: the watchdog only dumps when nothing has been presented or the picture is black, and the dump now walks a snapshot taken under the lock instead of holding the lock for the walk. Proof: three boots, load-window longest frame 6,483-7,257 ms before, 234-259 ms after, zero dumps.

## Method

- **Stock-clock gate.** A run only counts at stock clocks. There is no clock readback in the debug server, so stock is proven by the run's own segment rates (heavy 2D ~36-42 fps, heavy 3D ~30 off / ~36 on gxsvclate); a segment sitting at a flat ~60 presents/s means the clocks were boosted and the run is void.
- **One variable per A/B.** Flips go through dk_diag.txt on one build wherever the change allows it (the same boot can re-test a flip), and builds are identified by SHA-256 everywhere. ON/OFF/ON across separate boots for anything that ships on a measurement.
- **Instrument before theorizing.** The counters that settled questions here: the [vi] pacing windows (longest frame per 600 presents), the [gxworker] wait-by-cause lines, and targeted one-off instrumentation (lock-site tracking in the fiber manager) added for a single hunt and removed after. Several confident theories died to a single instrumented run; the ruled-out list below is mostly those.
- **The ear is a metric.** Any change that touches wait or service cadence gets a play pass for audio before it counts as done.
- Console work follows the claim protocol (`/agent-claim.txt` on the card, plus a screenshot of the foreground before the first input), and the card is restored to the release build and the pre-run dk_diag bytes after every run.

## Ruled out (don't redo)

- Small translator tweaks: compare-branch fusion, the CR / call-guard / callcount gates, the EABI exit-flag liveness batch, and the call-site sqrt intrinsic (bit-exact, no reliable gain). The game thread's own work barely moves for any of them.
- Bare-fcvt single rounding: `fcvtcheck` says DIFFERENT (the saturation and flush edges), so the masked form stays. A guarded variant (bare fcvt only above the NI flush threshold, where the mask cannot fire) is built and waits on its own console proof (`fcvtguard`, criterion zero differences in every FZ and rounding state) before any A/B.
- Lost worker wakes as the cause of the CopyDisp wait: real but tiny; gxfutex above is the whole story.
- Retrace starvation theories for the save-load hitch: polling retraces from the scheduler, from fiber switches, and from a wall-clock thread (unsafe: it crashed in the audio deferred path) all left the stall standing, because the executor was blocked on the fiber mutex, not short of polls.
- Depth pre-pass and culling as 2D costs: exonerated by direct measurement. Pass merging: there is one main scene pass; nothing to merge.
- Reverb off / native reverb: no gain (it runs in retrace idle), and a hand native is blocked anyway because the callback is table-called.

## The slow boot (open, roughly 1 boot in 4)

Some boots run the whole session slower (heavy 2D ~32 instead of ~40) with no code difference. The chain is measured: on a slow boot the GX worker's replay wall time grows (~20 ms vs ~15) while its CPU time is unchanged, because it blocks on Aurora's decode queue, which stalls on the frame worker's seal, whose staging-buffer map waits on a GPU fence. GPU timestamps then reverse the apparent causality: on slow boots the GPU runs the same scene work about 25% slower (the ratio matches one clock step, 384 to 307.2 MHz), so the fence is late because the GPU is slow, not the other way round. Leading hypothesis: the GPU (or EMC) starts one clock step down on those boots. The decisive test is a census build that reads the GPU and EMC clocks via clkrst at startup across a dozen boots and pairs them with the [gpu] lines; if every slow boot is exactly one step down, the fix is to pin the clock at startup.

## Next levers

1. **Frame overlap.** Make the frame-end chain overlap the next frame's guest work: a two-sided GX ring (no per-drain rewind) as the precondition, then defer the frame worker join. The prize is the 2.8-4.7 ms end-of-frame wait. Step 0 is a per-call-type split of the worker's replay (which record types own its ~14 ms), because the wait has three creditors (worker replay, Aurora decode, frame worker) and the split says which one is worth attacking. One constraint from Aurora's source: the display copy reads the live EFB at encode time, so the copy itself cannot move after the next frame's draws; any deferral has to hold two frame contexts in Aurora's frame worker.
2. **fcvt guarded variant** (see Ruled out): worth an estimated 1.0-1.3 ms in 3D if its proof passes.
3. **Slow-boot census**, above.

## Rules (from Brandon)

- Measure at stock clocks only. Use godmode=1 and perftimer=1 for runs. Identify NROs by SHA-256.
- Follow the console claim protocol, and screenshot-check the foreground before launching or sending input.
- Commits go out as Brandon.
- Restore the card to the release build and the dk_diag to its pre-run bytes after every run.
