# Super Paper Mario NX: the Switch layer

This folder ports NewSuperPaperBros (WiiCompiled's translated Super Paper Mario) to the Nintendo
Switch as a homebrew NRO. The upstream tree (`wiicompiled/`, `tools/`, the launcher) stays as
upstream wrote it apart from a few small hooks listed below, so upstream merges stay cheap.

No game data ships with this project, in the repository or in the releases: a release contains
the recompiled program, and the game's files are read from your own extracted copy of the game,
which you place on the SD card yourself. If you build from source, the translator runs on your
own PC against your own disc image, and only its generated C++ is compiled here.

## Status

The game boots and plays on Switch hardware, handheld and docked, and saves work. Output is
1280x720 in both modes for now. It is a work in progress: performance is the standing problem
(see "Known issues and performance"), and guest memory runs on its first phase, a checked-access
path that is slower than the flat window planned for phase 2 (`docs/GUEST-MEMORY-PLAN.md`).

## Installing from a release

You need a Switch running custom firmware (a current Atmosphère; running homebrew in an emuMMC
is recommended) and your own copy of **Super Paper Mario, USA disc (R8PE01, revision 0)**. The
European disc (R8PP01) does not work with the Switch build yet.

1. Download the Switch release and unzip it.
2. Copy the `switch` folder to the root of the SD card. You should end up with
   `sdmc:/switch/spm-nx/` holding `spm-nx.nro`, its companion files (`dsp_coef.bin`,
   `cacert.pem` and `wii_bootstrap/`) and a `licenses/` folder.
3. Extract your disc image and put the extracted disc (the folder containing `files` and `sys`)
   in `sdmc:/switch/spm-nx/game/`. One way to extract it is the upstream launcher's `--extract`
   command, shown in the root README's "Building from source".
4. Launch by title takeover: hold R while starting any game, then run `spm-nx.nro` from the
   homebrew menu. Do not launch it from the album (applet mode): the applet does not get enough
   memory.

Settings and saves live in `sdmc:/switch/spm-nx/UserData/`, beside the NRO.

## Playing

Player 1's controller (the handheld Joy-Cons, a paired Joy-Con pair or a Pro Controller) plays;
Super Paper Mario is single-player. The game was designed for a sideways Wii Remote, and the
mapping keeps the same jobs on the Switch buttons:

| Switch | Wii Remote |
|---|---|
| Left stick or D-pad | D-pad (move) |
| A or B | 2 (jump) |
| X | 1 |
| Y | A |
| Right stick | Pointer (Tippi) |
| R or ZR | Shake |
| + | + |
| -, L or ZL | - |

## Known issues and performance

- **Doors.** The open animation of the game's large doors renders bigger than the door itself
  and off centre. It is visual only; the door still works. The cause is under investigation.
- **Performance.** The port is CPU-bound: the translated game code, not the renderer, sets the
  frame rate. At a high CPU overclock (around 1963 MHz) lighter 2D areas run at or near 60 fps
  and busy 3D areas around 40 fps. At stock clocks everything runs slower, so a CPU overclock
  (for example with sys-clk) is the single biggest lever if you want more frames.
- **Docked output** is 1280x720, the same as handheld, for now.

## Building from source

Building takes two stages: translate the game on a Windows PC, then compile the translation for
the Switch in WSL or Linux. Only the USA disc works: **Super Paper Mario (R8PE01, revision 0)**.
The European disc (R8PP01) can be translated for the PC build, but there is no PAL path into
the NRO.

**Stage 1: prepare and translate (Windows).** You need Python 3 and a .NET 8 runtime (a `dotnet`
on your PATH; the translation step stops without one). From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
# Extract your disc image to Disc\DATA (the translation reads the game's code from it):
& "launcher\publish\New Super Paper Bros.exe" --extract "C:\path\to\Super Paper Mario.wbfs" Disc\DATA
# The translation inputs, in game\: the disc's main.dol and relF.bin pre-linked at its boot address:
copy Disc\DATA\sys\main.dol game\main.dol
python wiicompiled\projects\spm\tools\prelink_spm_rel.py Disc\DATA\files\rel\relF.bin game\spm_linked.rel game\spm_rel_layout.json
powershell -ExecutionPolicy Bypass -File tools\build-spm.ps1
```

**A note on `main.dol`.** The `main.dol` you copy in must carry two code patches from the
"SPM High Definition Audio" mod (by Toadette Hack Fan and PaperToadette, with game patches by
JohnP55). `wiicompiled/projects/spm/recomp.yml` (project `spm-r8pe01`) pins the SHA-256 of the
patched DOL, and the translator accepts only that file: a clean dump's unpatched `main.dol`
does not match the pin. The mod's canonical listing is the Super Paper Mario page of the
Dolphin Emulator Wiki, under Enhancements ("HD Audio Mod"). Get the mod there and follow the
mod's own instructions to apply its patches before the copy step above.

`build-spm.ps1` translates roughly 28,000 functions. It uses one job per CPU core by default and
is the slow step of the whole build. The PC executables it also produces are not used by the
Switch build; only `wiicompiled\generated\` carries over.

**Stage 2: compile for the Switch (WSL or Linux, with podman or docker).**

```
bash switch/scripts/build-switch.sh --audit    # runtime only, quick, no game needed
bash switch/scripts/build-switch.sh            # the NRO: ~/spm-switch/build/spm-nx.nro
```

The build compiles 6 jobs in parallel by default, and each translated shard needs roughly 1 GB
of RAM while it compiles, so allow about 6 GB free (pass `--jobs N` to lower that on a smaller
machine), plus a few GB of disk for the copied sources and build output under `~/spm-switch`.

After a full build the script also stages the SD card layout for you: under WSL it lands in the
Windows profile's `Downloads\spm-nx-sd\switch\spm-nx\` (the NRO plus `dsp_coef.bin`,
`cacert.pem`, `wii_bootstrap/` and a `licenses/` folder). Drag that
`switch` folder onto the SD card. (`--out DIR` chooses the staging folder, `--out none` skips
it; without staging, copy those files from `~/spm-switch/build/` to `sdmc:/switch/spm-nx/` by
hand.) Put your extracted disc (`Disc\DATA`, the folder with `files` and `sys`) in
`sdmc:/switch/spm-nx/game/`. Launch it with title takeover (hold R while starting a game), not
from the album: the applet doesn't get enough memory. Logs land in
`sdmc:/switch/spm-nx/UserData/Logs/`.

The package ships no prebuilt pipeline cache. On the first run the port builds its pipeline
cache as new effects appear, so the first session can stutter in places; the cache it builds
is kept on the SD card and reused, so later runs do not repeat it.

## Troubleshooting

- **It exits straight back to the homebrew menu, or shows a black screen.** Launch with title
  takeover (hold R while starting a game), not from the album, and check that the extracted disc
  is in `sdmc:/switch/spm-nx/game/` (that folder itself contains `files` and `sys`).
- **Reporting a bug.** Attach `UserData/Logs/stderr.log` from the SD card; it is written fresh
  every run. If the game crashed, also attach `UserData/Logs/crash.txt`, which holds the crash
  handler's register dump and backtrace.
- **Graphics glitches after an update.** Delete `UserData/dksh_cache.bin` to force every shader
  to recompile on the next run. Shaders that fail to compile are dumped to
  `UserData/Logs/shaderfail_*`; attach those to a graphics bug report.
- **Starting over.** Saves, settings and caches all live in `UserData/` beside the NRO. Deleting
  that folder resets all of them, saves included.

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
aurora/                      Horizon shims for Aurora: the ImGui SDL backends and the sqlite journal
deko/                        the WebGPU-to-deko3d interposer that draws Aurora's frames (see "Graphics")
deko3d/, uam/                vendored: the GPU API itself, and the GLSL-to-DKSH shader compiler
licenses/                    license texts for the code adapted from SwitchWaker (THIRD_PARTY.md)
scripts/                     build container and build-switch.sh
tests/                       host-side tests for the Horizon pieces
docs/GUEST-MEMORY-PLAN.md    how guest RAM works on Horizon, phase 1 and phase 2
```

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

The renderer's own status, conventions and known limitations live in `deko/HANDOFF.md`, including
its diagnostic switches: a `dk_diag.txt` in `UserData` can force pieces of the replay off or read
state back (the presented picture, the most-sampled textures) to files in `UserData/Logs`. With no
such file the build behaves normally.

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
BSD-3-Clause. The `deko/` interposer is MPL-2.0 (text in `licenses/MPL-2.0.txt`), as is the
Mozilla CA bundle (`cacert.pem`) the runtime's HTTPS uses. `THIRD_PARTY.md` lists each file and
its origin.
