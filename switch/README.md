# Super Paper Mario NX: the Switch layer

This folder ports NewSuperPaperBros (WiiCompiled's translated Super Paper Mario) to the Nintendo
Switch as a homebrew NRO. The upstream tree (`wiicompiled/`, `tools/`, the launcher) stays as
upstream wrote it apart from a few small hooks listed below, so upstream merges stay cheap.

No game data lives in this repository. The translator runs on your own PC against your own disc
image, and only its generated C++ is compiled here.

## Status

| Piece | State |
|---|---|
| Runtime sources (WiiCompiled `runtime/`) for Horizon | compiles, 0 errors (`--audit`) |
| Crypto++, pugixml, toml11, mbed TLS for Horizon | compile |
| Guest memory, phase 1 (heap stores, checked access) | written, tested off-device |
| SDL 3 on libnx (SwitchWaker's shim + WiiCompiled's extra calls) | compiles, links clean against 3.4.4 headers |
| Crash handler and logging (`source/spm_switch_init.cpp`) | written |
| Dawn (Null device + Tint GLSL), Aurora, the translated game | build and link into `spm-nx.nro` |
| deko3d renderer | wired into the build and running on hardware: see "Graphics" |
| First boot on hardware | `spm-nx.nro` boots and plays on hardware |

## Layout

```
cmake/SpmSwitch.cmake        the Switch half of the runtime's CMake (hooks called from runtime/CMakeLists.txt)
cmake/clang-launcher.sh      compiles with clang against devkitA64's newlib/libstdc++ (WiiCompiled needs Clang)
include/                     Horizon stand-ins for POSIX headers newlib lacks (sys/mman.h, sys/un.h, ifaddrs.h),
                             a Crypto++ OS RNG, the mbed TLS config, and spm_switch_compat.h (force-included)
runtime/                     guest memory for Horizon, mmap/mprotect over svcSetMemoryPermission,
                             mbed TLS entropy/clock/recv, getifaddrs stub
source/spm_switch_init.cpp   libnx startup: SD folders, stderr log, sockets, crash dump
sdl3_shim/                   SDL 3 implemented on libnx (sdl3_shim/PATCHES.md)
dawn/                        SwitchWaker's Dawn recipe and Horizon patches
deko/                        the WebGPU-to-deko3d interposer that draws Aurora's frames (see "Graphics")
deko3d/, uam/                vendored: the GPU API itself, and the GLSL-to-DKSH shader compiler
scripts/                     build container and build-switch.sh
tests/                       host-side tests for the Horizon pieces
docs/GUEST-MEMORY-PLAN.md    how guest RAM works on Horizon, phase 1 and phase 2
```

## Building

On the PC with the disc image, translate the game first (`tools\build-spm.ps1`, as for the PC
build). Then, in WSL or Linux with podman or docker:

```
bash switch/scripts/build-switch.sh --audit    # runtime only, quick, no game needed
bash switch/scripts/build-switch.sh            # the NRO: ~/spm-switch/build/spm-nx.nro
```

Copy `spm-nx.nro` and its companion files from the build folder (`dsp_coef.bin`,
`initial_pipeline_cache.db`, `cacert.pem`, `wii_bootstrap/`) to `sdmc:/switch/spm-nx/`, and your
extracted disc (`Disc\DATA`, the folder with `files` and `sys`) to `sdmc:/switch/spm-nx/game/`.
Launch it with title takeover (hold R while starting a game), not from the album: the applet doesn't get enough memory. Logs land in
`sdmc:/switch/spm-nx/UserData/Logs/`.

## How the port works

**Compilers.** WiiCompiled only supports Clang, but libnx's link specs belong to devkitA64's GCC. So
GCC stays CMake's compiler and drives the link, and `clang-launcher.sh` swaps clang in for each
compile, pointed at devkitA64's own newlib and libstdc++ headers with `-mcpu=cortex-a57` and
`-femulated-tls`. Both compilers produce AArch64 ELF objects against the same headers, so they link
together. Dawn and the SDL shim compile with GCC, which is how SwitchWaker proved them.

**Missing POSIX.** Horizon has no signals and no `mmap`. The runtime only needs anonymous memory
from `mmap` (an aligned heap block works), `mprotect` maps onto `svcSetMemoryPermission`, and
`sigsetjmp` maps onto `setjmp` because nothing here has signal masks to save. The POSIX fault
handler is compiled out: Horizon reports faults through libnx's exception handler instead, which
`spm_switch_init.cpp` uses to write a register dump.

**Guest memory.** On the PC every guest load is one add and a byte swap into a 4 GiB window, with
page protection catching the odd cases. Horizon can't do that page trickery the same way, so phase 1
keeps guest RAM in plain heap blocks and turns on the runtime's checked access path, which does
those checks in software. It's slower but has no kernel dependencies. Phase 2 brings the flat
window back with `svcMapProcessMemory`. Details: `docs/GUEST-MEMORY-PLAN.md`.

**Graphics.** Aurora records each frame against Dawn's Null device, exactly as on the other
surfaceless builds; what differs on the Switch is that the frames are drawn. `deko/` is a link-time
interposer: the linker's `--wrap=wgpuXxx` (the list lives in `cmake/SpmSwitch.cmake`, generated
from the `__wrap_` definitions) routes Aurora's WebGPU calls to `deko/wgpu_objects.cpp` and
`deko/wgpu_encode.cpp`. Each wrapper lets Dawn's Null device see the call where it needs to, and
keeps a deko3d "shadow" of the object in step; command encoders are recorded as op lists and
replayed onto the real GPU at `QueueSubmit`. Tint turns Aurora's WGSL shaders into GLSL, uam
compiles the GLSL to DKSH, and deko3d draws; compiled shaders are cached on the SD card
(`UserData/dksh_cache.bin`), and shaders that fail are dumped to `UserData/Logs/shaderfail_*` for
diagnosis. At present time Aurora hands its finished frame to `aurora_switch_dk_present`, which
blits it to the deko3d swapchain (1280x720, also when docked, for now); MSAA is ignored and resolves
become copies, which Aurora's msaa=1 does not notice.

An earlier plan wrote a new encoder against WiiCompiled's own frame recording (`SealedFrame`), the
way SwitchWaker's backend hooks upstream Aurora's `FramePacket` layer, which WiiCompiled's Aurora
doesn't have. The interposer replaced it: it sits below Aurora entirely, so upstream Aurora changes
cannot break it, and nothing of Aurora's 13.6k lines of local changes had to move.

## Upstream hooks

Each is a few lines, guarded by `__SWITCH__` or `MKW_PLATFORM_SWITCH`:

- `runtime/CMakeLists.txt`: recognise the Switch toolchain and include `cmake/SpmSwitch.cmake`; the
  Clang-only check accepts it (clang runs through the launcher); calls to the `spm_switch_*` hooks.
- `runtime/cmake/PublicProducts.cmake`: `-mcpu=cortex-a57` as the baseline arch flag.
- `runtime/include/guest_flat_memory.h`: the flat base for Horizon.
- `runtime/include/memory_access.h`: `FlatWriteRam*` honor checked access (phase 1 needs it).
- `runtime/include/runtime_config.h`: graphics API `auto`; executable and data directories as on Apple/Android;
  `sdmc:/` paths count as absolute.
- `runtime/src/game_folder.cpp`: the game is the `game` folder beside the NRO (no folder picker).
- `runtime/src/main.cpp`: graphics backend list; no POSIX fault handler.
- `aurora-main/lib/webgpu/gpu.{hpp,cpp}`, `aurora-main/lib/aurora.cpp`: surfaceless on the Null device (`kSurfaceless`):
  no WebGPU surface or SDL renderer. On `__SWITCH__`, `gpu.cpp` starts the deko3d renderer right
  after the device queue exists (and stops it first in `shutdown()`), and `aurora.cpp` hands each
  presented frame to it instead of dropping the image.
- `aurora-main/lib/sqlite_utils.hpp` (`CachePragmas`): the GPU caches use a PERSIST journal with no
  syncs, as SwitchWaker does, since WAL is compiled out.
- `runtime/src/host_context.cpp`: the AArch64 context switch Android uses.
- `runtime/src/platform/host_platform.cpp`: runtime and data folders from the environment, as on Android.

## Licenses

The fork is GPLv3 like upstream. Code taken from SwitchWaker stays MIT and Dawn's patches stay
BSD-3-Clause. `THIRD_PARTY.md` lists each file and its origin.
