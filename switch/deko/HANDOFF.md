# deko3d renderer: handoff notes (2026-10-25)

Status: **written, not wired into the build, never compiled.** Everything below is what the next agent
needs to finish it. Branch `claude/project-thread-qr1uyn` (Eladerino/spm-nx). No PR open yet.

## Goal

The game already runs on hardware (boot 9: audio, input, ~58 fps), but the screen is black. Aurora
draws through WebGPU on Dawn's **Null** device (`kSurfaceless` in
`wiicompiled/aurora-main/lib/webgpu/gpu.hpp`), so nothing reaches the GPU. This renderer makes Aurora's
WebGPU calls draw on the Switch GPU through deko3d.

## Approach (decided, don't revisit)

A **link-time interposer**. The linker's `--wrap=wgpuXxx` routes every caller in the NRO to our
`__wrap_wgpuXxx`. Each wrap calls Dawn's `__real_wgpuXxx` where Dawn needs to see the call, and keeps a
deko3d "shadow" object in step. This replaced the older plan of writing an encoder against Aurora's
SealedFrame layer: the interposer is independent of Aurora's internals, so upstream Aurora changes
don't break it.

Pipeline: WGSL (Aurora) -> Tint GLSL writer (`shader_translate.cpp`) -> uam (`shaders.cpp`, one worker
thread, 8 MB stack) -> DKSH -> deko3d. Compiled shaders are cached in
`/switch/spm-nx/UserData/dksh_cache.bin`. Failures are dumped to
`UserData/Logs/shaderfail_<key>_<stage>.{wgsl,glsl}`.

## Files (all under switch/)

| File | What | State |
|---|---|---|
| `deko3d/` | deko3d 0.5.0 + SwitchWaker patches (zlib). CMake targets `deko3d_release`, `deko3d_debug` | vendored |
| `uam/` | uam 1.1.0 + patches 1-9 (zlib, mesa parts MIT). CMake target `uamlib` | vendored |
| `deko/dk.h` | the internal API: Renderer `R`, queue_mutex, serials, memory, descriptors, shaders, logging | done |
| `deko/device.cpp` | `aurora_switch_dk_init/shutdown`, swapchain (3x RGBA8 1280x720), logging `[dk]` to stderr | done |
| `deko/memory.cpp` | fence ring (32), command/upload chunk pools, keepalive until serial done, image heap, blocks | done |
| `deko/descriptors.cpp` | image/sampler descriptor slots, sampler LRU, the empty image (slot 1) | done |
| `deko/shader_translate.*`, `deko/shaders.cpp` | WGSL->GLSL->DKSH, binding table per stage, SD cache | done |
| `deko/wgpu_objects.hpp/.cpp` | shadows + wraps: textures, views, samplers, buffers (+MapAsync/GetMappedRange), bind group layouts, bind groups, shader modules, render/compute pipelines | first draft |
| `deko/wgpu_encode.cpp` | encoders/passes recorded as op lists, replay at QueueSubmit, WriteBuffer/WriteTexture, `aurora_switch_dk_present` | first draft |
| `deko/dk_aurora.h` | C API: init, present(void* WGPUTexture), shutdown | done |

Read the header comments of `wgpu_objects.hpp` and `wgpu_encode.cpp` first: they explain lifetimes
(shared_ptr shadows, Aurora's refcount mirrored, keepalive until the GPU passes the serial), barriers
and serials.

## Remaining steps, in order

1. **Aurora hooks** (guard each with `#if defined(__SWITCH__)`, include `deko/dk_aurora.h`):
   - `lib/webgpu/gpu.cpp`, right after `g_queue = g_device.GetQueue();` (around line 942): `aurora_switch_dk_init();`
     It must run before Aurora creates any texture or buffer.
   - `lib/webgpu/gpu.cpp` `shutdown()`: call `aurora_switch_dk_shutdown();` first.
   - `lib/aurora.cpp` `present_presentation_job`, the `if constexpr (webgpu::kSurfaceless)` branch
     (around line 926): after `wait_until_precise(...)`, call
     `aurora_switch_dk_present(job.image->texture.texture.Get());`.
   - The presentation texture is RGBA8 with RenderAttachment|TextureBinding|CopySrc|CopyDst. The
     shadow adds `DkImageFlags_Usage2DEngine` to every 2D-capable format, which the present's blit needs.
2. **CMake** (`switch/cmake/SpmSwitch.cmake`):
   - Right after `include(dawn/dawn.cmake)` (still GCC, before the clang launcher is set), add
     `switch/deko3d` and `switch/uam` (their own CMakeLists) and an `spm_deko` static library of
     `switch/deko/*.cpp`.
   - `spm_deko` needs: the Dawn source dir include (for `src/tint/...`), the generated webgpu headers,
     and links to `deko3d_release uamlib tint_api tint_lang_glsl_writer tint_lang_wgsl_reader
     tint_lang_wgsl_inspector nx`. C++20.
   - In `spm_switch_configure_products`, replace `deko3d` (devkitPro's lib) with `spm_deko
     deko3d_release`. Then add `LINKER:--wrap=<fn>` for **every** `__wrap_` function in
     `wgpu_objects.cpp` and `wgpu_encode.cpp`. Generate the list with
     `grep -ho '__wrap_wgpu[A-Za-z]*' switch/deko/*.cpp | sort -u | sed 's/__wrap_//'`.
     A missing wrap = that call silently goes to Dawn only.
   - Reference: SwitchWaker `native/CMakeLists.txt` (wrap list, uam/deko3d build).
3. **Compile-check** in a container: image
   `docker.io/devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`.
   The repo's Containerfile apt step fails in the cloud, so use the base image as is. Start dockerd by hand.
   Dawn: `git fetch` commit 266c1cf from github.com/encounter/dawn (curl archives are blocked) and apply
   `switch/dawn/patches/dawn-switch-tint-*.patch`. Generate the headers with
   `python3 generator/dawn_json_generator.py --dawn-json src/dawn/dawn.json --template-dir generator/templates --targets headers,cpp_headers,dawn_headers --output-dir <gen>`.
   Then compile each `switch/deko/*.cpp` with `aarch64-none-elf-g++ -std=c++20 -D__SWITCH__` plus the
   libnx, deko3d, Dawn source and gen includes. A full NRO link only happens on Brandon's PC.
4. **Docs**: add deko3d and uam to `switch/THIRD_PARTY.md` (deko3d zlib; uam zlib + MIT mesa-imported;
   adapted deko files MPL-2.0 from SwitchWaker/SwitchWakerHD). Update `switch/README.md`: the Graphics
   section (around line 76) still describes the SealedFrame plan, and the status row says "deko3d
   renderer | not started".
5. **PR** to main. The body starts with:
   ```
   <!-- claude-projects-attribution: {"github_login":"Eladerino"} -->
   _Requested by **Brandon** · [project thread](https://claude.ai/code/project/chan_01QR4L4q3CZMLtSNGHE3bq6a?thread=cmsg_01QR4L4q3CZMLtSNGHE3bq6a9TMK5o582jZCvBPNPLBj2X)_
   ```
   Assign Eladerino. The review request fails because PRs open as him, which is fine.
6. **Test steps for Brandon**: numbered, one command per code box (he's on mobile).
   1. Pull and run `build-switch.sh`.
   2. Copy `Downloads\spm-nx-sd` to the SD card (never write the SD directly from the build).
   3. Boot with title takeover.
   4. Send back `UserData/Logs/stderr.log`, `crash.txt` and any `shaderfail_*` files.

   Warn him that the first boot is slow because shaders compile; later boots use the cache.

## Known risks / first things to check on hardware

- **Front face**: mapped as-is, with an `SPM_DK_FLIP_FRONT` env escape hatch. If geometry vanishes,
  culling is inverted.
- **uam rejecting Tint output**: check the shaderfail dumps.
- **Memory**: Dawn Null still allocates its own buffer backing, so every buffer exists twice (Aurora's
  staging is 3x63 MB). If memory runs out, shrink the staging buffers.
- **MapAsync blocks** until the GPU finishes the buffer's last submit. That's fine for staging; depth
  peek and EFB readbacks may stall a frame.
- **MSAA is ignored**: textures are always single-sampled and resolves become copies. Aurora uses
  msaa=1 here anyway.
- **Not implemented**: render bundles, immediates, ClearBuffer, indirect draws, storage textures,
  override constants (all logged when hit; Aurora doesn't use them).
- **Command validation by Dawn is skipped** for pass/copy commands; we don't call `__real` for them.
  That is intentional, for speed.
- **Docked mode** stays at 1280x720 for now.
