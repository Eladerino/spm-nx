# deko3d renderer: status and conventions

## Status

The renderer is wired into the build and runs on hardware: the game boots, plays, and presents
through the deko3d swapchain. The large doors' open animation, previously the standing open
defect, is fixed. Performance is CPU-bound in the guest execution on the game thread, not
in this replay. Changes land on main, some as direct pushes and some through pull requests
(the recent GX record series, PRs #11 to #20, came in that way).

## What it is

Aurora draws through WebGPU on Dawn's **Null** device (`kSurfaceless` in
`wiicompiled/aurora-main/lib/webgpu/gpu.hpp`), so without this renderer nothing reaches the GPU.
The renderer is a **link-time interposer**: the linker's `--wrap=wgpuXxx` routes every caller in
the NRO to `__wrap_wgpuXxx` in this folder. Each wrap calls Dawn's `__real_wgpuXxx` where Dawn
needs to see the call, and keeps a deko3d "shadow" object in step. The design is deliberate and
settled: the interposer sits below Aurora entirely, so upstream Aurora changes cannot break it.
(It replaced an earlier plan to write an encoder against Aurora's SealedFrame layer, which
WiiCompiled's Aurora does not have.)

Pipeline: WGSL (Aurora) -> Tint GLSL writer (`shader_translate.cpp`) -> uam (`shaders.cpp`, one
worker thread, 8 MB stack) -> DKSH -> deko3d. Compiled shaders are cached in
`/switch/spm-nx/UserData/dksh_cache.bin`; failures are dumped to
`UserData/Logs/shaderfail_<key>_<stage>.{wgsl,glsl}`.

## Files

| File | What |
|---|---|
| `dk.h` | the internal API: Renderer `R`, queue_mutex, serials, memory, descriptors, shaders, logging, and the diagnostic switches |
| `device.cpp` | `aurora_switch_dk_init/shutdown`, the swapchain (3x RGBA8 1280x720), `[dk]` logging to stderr, the diag switch reader |
| `memory.cpp` | fence ring (32), command/upload chunk pools, keepalive until serial done, image heap, blocks |
| `descriptors.cpp` | image/sampler descriptor slots, sampler LRU, the empty image (slot 1) |
| `shader_translate.*`, `shaders.cpp` | WGSL->GLSL->DKSH, binding table per stage, SD cache |
| `wgpu_objects.hpp/.cpp` | shadows + wraps: textures, views, samplers, buffers (+MapAsync/GetMappedRange), bind group layouts, bind groups, shader modules, render/compute pipelines |
| `wgpu_encode.cpp` | encoders/passes recorded as op lists, replay at QueueSubmit, WriteBuffer/WriteTexture, `aurora_switch_dk_present`; the frame pacing, per-frame CPU and texture dump diagnostics |
| `dk_aurora.h` | C API: init, present(void* WGPUTexture), shutdown |

The header comments of `wgpu_objects.hpp` and `wgpu_encode.cpp` are the detailed reference:
lifetimes (shared_ptr shadows, Aurora's refcount mirrored, keepalive until the GPU passes the
serial), barriers and serials.

## Conventions

- Upstream files keep their original commentary; only this folder's own files carry the project's
  comments, and they are written as maintained documentation, not session notes.
- Diagnostics follow one pattern: a switch in `UserData/dk_diag.txt`, read once at renderer init
  (see `dk.h`), then flippable at runtime from the Options menu's Diagnostics page and the
  `UserData/dk_cmd.txt` command channel. A flip in the menu writes all twenty-one page switches back
  to `dk_diag.txt`; the file knows twenty-five keys in all, four of them (zcull,
  guestcore, dkserver, rttlog) boot-time only. No file means a normal build. Every switch is off by default except
  `cpthread` (Aurora's GX decode thread, core 1), `gxthread` and `zcull`, which are on. The lines that move a build
  away from normal are the ones to name in a test report: `cpthread=0`, `gxthread=0`,
  `zcull=0`, and the experimental
  GX record switches `gxworker=1`, `gxdefer=1` (records replay on core 2).
- The debug server is a second consumer of the same switches, and the scripted-run front
  end: `source/debug_server.{h,cpp}` is the shared core vendored from SwitchWakerHD (see
  `../THIRD_PARTY.md`), and `source/spm_debug_server.cpp` registers this port's commands
  (info, diag, set, flush, mark, warp, place; shot, peek, poke, quit and reload answer
  that they are not in this build) and the input provider the SDL shim's weak
  `cos_switch_debug_input` expects. It is gated by `dkserver=1` in dk_diag.txt
  (boot-time only: started from `diag_init`, stopped from `userAppExit`), has no menu
  row and no settings_overlay code, feeds its log stream by tailing
  `UserData/Logs/stderr.log` into the core's ring, and runs its threads at priority
  0x3B.
- Changes land on main with the commit message stating what was actually compiled and tested.

## Known limitations

- **Front face** is mapped as-is, with an `SPM_DK_FLIP_FRONT` env escape hatch if culling ever
  looks inverted.
- **Memory**: Dawn Null still allocates its own buffer backing, so every buffer exists twice
  (Aurora's staging is 3x63 MB). If memory runs out, shrink the staging buffers.
- **MapAsync blocks** until the GPU finishes the buffer's last submit. That is fine for staging;
  depth peek and EFB readbacks may stall a frame.
- **MSAA is ignored**: textures are always single-sampled and resolves become copies. Aurora uses
  msaa=1 here anyway.
- **Not implemented**: render bundles, immediates, ClearBuffer, indirect draws, storage textures,
  override constants (all logged when hit; Aurora does not use them).
- **Command validation by Dawn is skipped** for pass/copy commands; the wraps do not call
  `__real` for them. That is intentional, for speed.
- **Docked mode** stays at 1280x720 for now.
