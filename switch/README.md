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
1280x720 in both modes for now. Settings live in the game's Options menu (see "Settings" below); the PC version's settings
top bar is not part of the Switch build, and neither is the PC launcher's update flow. It is
a work in progress: performance is the standing problem (see "Known issues and performance"),
and guest memory runs on its first phase, a checked-access path that is slower than the flat
window planned for phase 2 (`docs/GUEST-MEMORY-PLAN.md`).

## Installing from a release

You need a Switch running custom firmware (a current Atmosphère; running homebrew in an emuMMC
is recommended) and your own copy of **Super Paper Mario, USA disc (R8PE01, revision 0)**. The
European disc (R8PP01) does not work with the Switch build yet.

1. Download the Switch release zip and unzip it on your computer.
2. Copy the `switch` folder from the zip to the root of the SD card, merging with the `switch`
   folder already there. You should end up with `sdmc:/switch/spm-nx/` holding `spm-nx.nro`,
   its companion files (`dsp_coef.bin`, `cacert.pem` and `wii_bootstrap/`), a `licenses/`
   folder with the license texts for everything in the package, and
   `UserData/button_icons/` with the Switch button prompt icons (see Licenses).
3. Extract your disc image and put the extracted disc (the folder that itself contains `files`
   and `sys`) in `sdmc:/switch/spm-nx/game/`. One way to extract it is the upstream launcher's
   `--extract` command, shown in the root README's "Building from source".
4. Put the SD card back in the Switch and launch by title takeover: hold R while starting any
   game, then run `spm-nx.nro` from the homebrew menu. Do not launch it from the album (applet
   mode): the applet does not get enough memory.
5. First launch: a startup screen appears (it says "Hold - = Options"); press A to start.
   The first session builds the shader and pipeline caches as new effects appear, so it can
   hitch in places the second session does not. Nothing is wrong when that happens.

Settings, saves and caches live in `sdmc:/switch/spm-nx/UserData/` beside the NRO (the
package seeds it with `button_icons/`; the game creates the rest on the first run). Do not
delete that folder to "clean up": your saves and the caches that make later launches fast
are in it.

**Updating:** unzip the new release over the old `switch/spm-nx/` folder, replacing the NRO and
its companion files. `UserData/` and `game/` are yours and carry over, saves, settings and
caches included; the one release-owned corner is `UserData/button_icons/`, which the unzip
refreshes so a stale icon set cannot win over the current one. One catch: the game never rewrites an existing
`UserData/Config.toml`, so an install that started on an older release keeps the settings it
already has (including the old PC-style defaults, if it has them) until you change them in the
settings menu. Fresh installs get the current Switch defaults.

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
| R or ZR | Shake (hold) |
| + | + |
| -, L or ZL | - |

Two of those deserve a line each:

- **Shake:** hold R or ZR. The game receives repeated shake flicks for as long as you hold the
  button, which is what charging (for example a Thunder Rage) and breaking free use.
- **Tippi's pointer:** push the right stick to raise the pointer and aim with it; press Y while
  it is up to use it (WPAD A). Centre the stick and the pointer goes away again after a couple
  of seconds.

## Settings

Holding `-` for about half a second opens the game's Options menu (upstream's, since
v1.0.6); a short press is still the game's `-`. It has eight pages: Game, Display, Sound,
Controls, Cheats, Mods, and this port's own Developer and Diagnostics pages. Use the D-pad or left stick to move, A to select or adjust, and B
to close. While the menu is open the game pauses and does not receive pad input, so nothing
moves behind it. Rows that do not apply on the Switch are not shown: there are no keyboard
bindings, no window size row, no open-folder rows, and no quit-to-desktop row.

- **Controls** remaps the gamepad. The page's lower block, labelled "Gamepad:", binds
  player 1's twelve actions (two slots each): highlight a slot, press A, then press the
  button or push the stick direction to bind; changes apply immediately, no restart. The
  page's own Controller rows above it drive players 2 to 4 (unused by this single-player
  game) and its vibration setting.
- **Developer** and **Diagnostics** are this port's own two pages, added after upstream's
  six (they replaced a separate Developer shell, which has been retired). Developer holds
  Bloom, Disable copy filter, Skip draws while shaders compile, Texture replacements (the
  master switch for the replacement button icons and prompt pictures; takes effect after a
  restart), Frame graph, UI volume, the Audio mix worker toggle, Spinning coins, a Texture
  dumps switch (writes every unmatched texture to `Cache/texture_dumps` under the name a
  replacement would use, for making new replacement pictures; takes effect after a restart),
  and a Clear shader cache row (deletes the compiled shader cache on the next launch; the
  recovery step for a cache that has gone wrong). Diagnostics holds the renderer's fourteen
  diagnostic switches (each one also writes back to `UserData/dk_diag.txt`, the file the
  same switches are read from at boot) and Flush logs and Mark log rows for test runs.
  Diagnostics can hurt performance or distort the picture; they are for bug reports, not
  for play.

Fresh installs start at Resolution 1.5x (a 1280x720 render, the screen's native size in
handheld), Aspect 16:9 and frame rate 60. If you upgraded from an older release and the
menu shows 2.25x and cinematic aspect, that is your old config file being kept, as
promised above: set Resolution and Aspect here once and it stays.

## Known issues and performance

- **Doors.** The open animation of the game's large doors renders bigger than the door itself
  and off centre. It is visual only; the door still works. The cause is under investigation.
- **Performance.** The port is CPU-bound on the guest side: the translated game code, not the
  renderer, sets the frame rate. Set expectations by area, not by a single number. At a high
  overclock (CPU 1963 MHz, GPU 1152 MHz, memory 2366 MHz, the clocks the port is
  developed and measured at) 2D areas run at or near full speed. The heavy 3D side is
  much slower for now: Flipside currently runs at about 20 fps. The game steps its physics once
  per frame, so a lower frame rate is a slower game, not just a less smooth one, and at stock
  clocks everything runs slower still. Overclocking is the single biggest lever if you want
  more speed; the numbers above come from a machine overclocked with Horizon-OC
  (https://github.com/Horizon-OC/Horizon-OC/releases).
- **First launch after installing** builds the shader and pipeline caches and can hitch (see
  step 5 of the install). Warm launches are fast. The caches live in `UserData/`; deleting
  that folder brings the slow first launch back, along with deleting your saves.
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

The package ships no prebuilt shader or pipeline cache. On the first run the port builds both
as new effects appear, so the first session can stutter in places; the caches it builds are
kept in `UserData/` on the SD card and reused, so later runs do not repeat it.

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
  that folder resets all of them, saves included, and the next launch is a slow first launch
  again while the caches rebuild. Treat it as a last resort, not a cleanup step.

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
- `runtime/src/settings_overlay.cpp`: on Switch the PC top bar is never drawn (its DrawTopBar
  seam is inert there), F10 and Escape toggle the Options menu, hold-Minus opens it too, and
  the guest input block and status window offsets follow the Options menu's visibility. The
  Options menu also gains this port's Developer and Diagnostics pages on Switch. A handful of rows do not apply on the Switch and are not
  shown (keyboard bindings, the window row, open-folder rows, quit-to-desktop); the updater
  is an inert stub (`switch/runtime/updater_horizon.cpp`), so its rows hide themselves.
- `runtime/src/kpad.cpp`: one call routes guest pad input through the overlay's menu gate
  while a menu is open, at the same place upstream put it.
- `runtime/src/mod_runtime.cpp`: the native code-mod loader is compiled out on Switch; data
  mods, Gecko codes and Riivolution patches still load.
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
its origin. An earlier Developer shell in `menu/`, its framework (window, scrolling page,
gamepad navigation, widget helpers and style) adapted from SwitchWakerHD's settings overlay
by [centollOS](https://github.com/centollOS/SwitchWakerHD), which builds on the
ZeldaWWHDRecomp overlay, has been retired: its rows moved into the Options menu's Developer
and Diagnostics pages, and the framework code no longer ships in this tree.

The button prompt icons staged into `UserData/button_icons/` are "Input Prompts" by
[Kenney](https://kenney.nl), CC0, generated at build time from the SVGs vendored in
`tools/button-art/`; the CC0 text travels with them as `UserData/button_icons/Kenney-License.txt`.
Only the files drawn from that CC0 art alone ship, plus fully transparent marker files: the
game reports when a marker texture is drawn, which is how the replacement Controls panel knows
the game's own controls screen is up, and the panel then draws the controller and its labels
itself, with the buttons from the mapping table above. The generator's other outputs composite
the glyphs into the HD texture pack's redrawn game textures, and those are never staged. The
prompt pictures that can be drawn from scratch do ship: the small button glyphs inside text
boxes and the shake prompt's icon show Switch buttons in the package. The bigger prompt
pictures (the round button pictures in signs and tutorials, the 3D buttons' faces, the Use
Tippi picture) composite into the pack's artwork, so those stay Wii in the package. The full
set (every prompt picture replaced) stays a do-it-yourself drop-in: run
`tools/make-button-icons.py` against your own copy of the HD pack and copy the resulting
`button_icons/` into `UserData/`.

For the project's position on Nintendo's trademarks and copyrighted material, see the Legal notice
in the root [`README.md`](../README.md).
