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
| deko3d renderer | not started: see "Graphics" |
| First boot on hardware | `spm-spike.nro` passed; `spm-nx.nro` next |

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

**Graphics.** The plan is SwitchWaker's: Aurora records each frame against Dawn's Null device, Tint
turns Aurora's WGSL shaders into GLSL, uam compiles that to deko3d shaders, and a deko3d backend
draws. The catch found while scaffolding: WiiCompiled carries its own Aurora (encounter/aurora from
mid-May 2026 plus about 13.6k lines of WiiCompiled changes) with its own frame recording
(`SealedFrame`, `RenderPass`/`Command` in `lib/gfx/common.cpp`). SwitchWaker's backend hooks
upstream Aurora's newer `FramePacket` layer, which WiiCompiled's copy doesn't have. So the plan is to
reuse SwitchWaker's Aurora-independent half (device, memory, swapchain, shader translation, shader
cache, present) and write a new encoder against WiiCompiled's `SealedFrame`, rather than rebasing
13.6k lines onto upstream.

## Upstream hooks

Each is a few lines, guarded by `__SWITCH__` or `MKW_PLATFORM_SWITCH`:

- `runtime/CMakeLists.txt`: recognise the Switch toolchain and include `cmake/SpmSwitch.cmake`; the
  Clang-only check accepts it (clang runs through the launcher); calls to the `spm_switch_*` hooks.
- `runtime/cmake/PublicProducts.cmake`: `-mcpu=cortex-a57` as the baseline arch flag.
- `runtime/include/guest_flat_memory.h`: the flat base for Horizon.
- `runtime/include/memory_access.h`: `FlatWriteRam*` honor checked access (phase 1 needs it).
- `runtime/include/runtime_config.h`: graphics API `auto`; executable and data directories as on Apple/Android.
- `runtime/src/game_folder.cpp`: the game is the `game` folder beside the NRO (no folder picker).
- `runtime/src/main.cpp`: graphics backend list; no POSIX fault handler.
- `runtime/src/host_context.cpp`: the AArch64 context switch Android uses.
- `runtime/src/platform/host_platform.cpp`: runtime and data folders from the environment, as on Android.

## Licenses

The fork is GPLv3 like upstream. Code taken from SwitchWaker stays MIT and Dawn's patches stay
BSD-3-Clause. `THIRD_PARTY.md` lists each file and its origin.
