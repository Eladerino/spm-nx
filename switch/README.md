# Super Paper Mario NX: the Switch layer

This folder ports NewSuperPaperBros (WiiCompiled's translated Super Paper Mario) to the Nintendo
Switch as a homebrew NRO. The upstream tree (`wiicompiled/`, `tools/`, the launcher) stays as
upstream wrote it apart from a few small hooks listed below, so upstream merges stay cheap.

No game data ships with this project, in the repository or in the releases: a release contains
the recompiled program, and the game's files are read from your own extracted copy of the game,
which you place on the SD card yourself. If you build from source, the translator runs on your
own PC against your own disc image, and only its generated C++ is compiled here.

## At a glance

| | |
|---|---|
| **Runs on** | A modded Nintendo Switch (Atmosphère), handheld or docked, launched by title takeover |
| **You supply** | Your own USA disc of Super Paper Mario (R8PE01, revision 0), extracted to `game/` |
| **Output** | 1280x720 in both modes, for now |
| **Settings** | The game's own Options menu: hold `-` for half a second, or press HOME |
| **Saves, settings, caches** | `sdmc:/switch/spm-nx/UserData/`, created at runtime, yours to keep |
| **First session tip** | Run **Options > Game > Prepare graphics** once to build the shader caches ahead of play (see "Prepare graphics" below) |
| **Known open problem** | Performance in heavy 3D areas (see "Known issues and performance") |

## Status

The game boots and plays on Switch hardware, handheld and docked, and saves work. Output is
1280x720 in both modes for now. Settings live in the game's Options menu (see "Settings" below); the PC version's settings
top bar is not part of the Switch build, and neither is the PC launcher's update flow. It is
a work in progress: performance is the standing problem (see "Known issues and performance").
Guest memory runs on the flat window (phase 2 in `docs/GUEST-MEMORY-PLAN.md`): guest RAM is
mapped as one flat window the way the PC build does it, which replaced the slower
checked-access path this port started on. It is public now so others can
follow along, try it, and offer a second pair of eyes while the performance work
continues.

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
3. **Add your own game.** No game data ships with this port: the NRO reads the game's files
   from your own dumped disc, extracted, and it will not read a disc image directly. Your
   image can be `.iso`, `.wbfs` or `.ciso`. The easy way to extract it is Dolphin (a free
   emulator, which also opens `.rvz`, `.wia` and `.gcz` images):

   - Open Dolphin. If the game is not in its game list yet, open **Config**, go to the
     **Paths** tab and **Add** the folder your image is in.
   - Right-click **Super Paper Mario** in the game list, choose **Properties**, then open
     the **Filesystem** tab.
   - Right-click the top entry, **Disc**, choose **Extract Entire Disc...**, and pick an
     empty folder.
   - When it finishes, open the `DATA` folder inside the folder you picked, and copy
     everything in `DATA` (the `files` and `sys` folders) into `sdmc:/switch/spm-nx/game/`.

   Done right, that corner of the SD card looks like this (the release's own files from
   step 2 are already there):

   ```
   sdmc:/switch/spm-nx/
       spm-nx.nro
       dsp_coef.bin, cacert.pem, wii_bootstrap/, licenses/
       UserData/
       game/
           files/
           sys/
   ```

   The project's launcher can extract too: its `--extract` command, shown in the root
   README's "Building from source", takes the same `.iso`, `.wbfs` and `.ciso` images.
4. Put the SD card back in the Switch and launch by title takeover: hold R while starting any
   game, then run `spm-nx.nro` from the homebrew menu. Do not launch it from the album (applet
   mode): the applet does not get enough memory.
5. First launch: a startup screen appears (it says "Hold - = Options"). The card holds for a
   few seconds, then shows its prompt; **one press of A starts the game** (the port keeps
   tapping A through the logo sequence for you, and the card reads "Starting..." while it
   does). The first session builds the shader and pipeline caches as new effects appear, so
   it can hitch in places the second session does not. Nothing is wrong when that happens,
   and you can build those caches ahead of play instead: see "Prepare graphics" below.

Settings, saves and caches live in `sdmc:/switch/spm-nx/UserData/` beside the NRO (the
package seeds it with `button_icons/`; the game creates the rest on the first run). Do not
delete that folder to "clean up": your saves and the caches that make later launches fast
are in it.

**Updating:** unzip the new release over the old `switch/spm-nx/` folder, **merging**, and
replace the NRO and its companion files. Never delete the `spm-nx` folder and replace it:
`UserData/` and `game/` are yours and must carry over, saves, settings and caches included,
and a fresh folder starts with none of them. The one release-owned corner is
`UserData/button_icons/`, which the unzip refreshes so a stale icon set cannot win over the
current one.

> [!IMPORTANT]
> **One manual step when updating from a build that had the tutorial prompt experiment:**
> if `UserData/button_icons/nintendo/marker7` exists on your card, delete that folder by
> hand. Unzipping a release cannot delete files, so without this step the old marker files
> stay live: the Shooting Star aiming cursor stays hidden and they can misfire elsewhere.
> Current builds stage no marker7 at all.

One catch: the game never rewrites an existing
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
| Y | A |
| X | 1 |
| Right stick | Pointer (Tippi) |
| R or ZR | Shake (hold), or shake the controller itself |
| + | + |
| -, L or ZL | - |

Two of those deserve a line each:

- **Shake:** hold R or ZR. The game receives repeated shake flicks for as long as you hold the
  button, which is what charging (for example a Thunder Rage) and breaking free use.
- **Tippi's pointer:** push the right stick to raise the pointer and aim with it; press X while
  it is up to use it (the Flip / Use Tippi button, WPAD A, with the default bindings). Centre
  the stick and the pointer goes away again after a couple of seconds.

## Settings

Holding `-` for about half a second opens the game's Options menu (upstream's, since
v1.0.6); a short press is still the game's `-`, and opens the game's Controls screen.
The HOME button opens the same menu, which is why the game's own on-screen hints point
you at "HOME > Options". It
has eight pages: Game, Display, Sound, Controls, Cheats, Mods, and this port's own
Developer and Diagnostics pages. Move with the D-pad or the left stick, switch pages with
L and R, select or adjust with A, B or X, and close the menu with Y or HOME. Those
buttons follow the Controls page bindings (select sits on the buttons bound to Jump and
to A, back sits on the button bound to 1), so remapping those actions remaps the menu
with them. While the menu is open the game pauses and does
not receive pad input, so nothing moves behind it. Rows that do not apply on the Switch are not shown: there are no keyboard
bindings, no window size row, no open-folder rows, and no quit-to-desktop row.

- **Game** holds the port's **Prepare graphics** row, covered in its own section below.
- **Controls** remaps the gamepad, and holds the vibration setting. The block labelled
  "Gamepad:" binds player 1's actions, two slots each: ten actions are shown (the config
  table has twelve; the Tilt left/right pair is not shown, because the main game never
  reads tilt, and the reset row still covers them). Highlight a slot, select it, then
  press the button or push the stick direction to bind; changes apply immediately, no
  restart. The PC build's separate Controller rows for players 2 to 4 are not shown on
  Switch: this game is single-player, and player 1 is the Gamepad block.
- **Developer** and **Diagnostics** are this port's own two pages, added after upstream's
  six (they replaced a separate Developer shell, which has been retired). Developer holds
  Bloom, Disable copy filter, Skip draws while shaders compile, Texture replacements (the
  master switch for the replacement button icons and prompt pictures; takes effect after a
  restart), Frame graph, UI volume, the Audio mix worker toggle, Spinning coins, a Texture
  dumps switch (writes every unmatched texture to `Cache/texture_dumps` under the name a
  replacement would use, for making new replacement pictures; takes effect after a restart),
  and a Clear shader cache row (deletes the compiled shader cache on the next launch; the
  recovery step for a cache that has gone wrong). Diagnostics holds the renderer's
  eighteen diagnostic switches (each flip also writes all of them back to
  `UserData/dk_diag.txt`, the file the same switches are read from at boot) and Flush logs
  and Mark log rows for test runs. All of them start off except the GX decode thread
  (cpthread, on core 1), which is on unless turned off there. Three of them are the
  experimental GX call-record switches, all off by default and best left off for play:

  | Switch | What it does |
  |---|---|
  | `gxworker` | Records the game's GX calls and replays each one straight away. The picture should not change. |
  | `gxdefer` | As gxworker, but the records wait for the next sync point instead of replaying at once. |
  | `gxthread` | As gxdefer, but the records replay on their own thread on core 2. Experimental. |

  Diagnostics can hurt performance or distort the picture; they are for bug reports, not
  for play. If anything draws wrong with one of the GX switches on, turn it off and say
  which one in the bug report.

Fresh installs start at Resolution 1.5x (a 1280x720 render, the screen's native size in
handheld), Aspect 16:9 and frame interpolation off; the output itself is a fixed 60 Hz,
there is no frame rate setting on Switch. If you upgraded from an older release and the
menu shows 2.25x and cinematic aspect, that is your old config file being kept, as
promised above: set Resolution and Aspect here once and it stays.

## Prepare graphics

> [!NOTE]
> **New, and still being tested.** Prepare graphics shipped recently and its first full
> runs are still being evaluated. It is optional, nothing about normal play depends on
> it, and the caution under "Before you run it" below is worth the minute it takes.

A new install ships with no shader or pipeline cache (see
[`PROVENANCE.md`](../PROVENANCE.md): the old seed was removed on purpose), so every effect
compiles the first time it appears, and that first appearance can stutter. **Prepare
graphics** does that compiling ahead of play, on your own console, from your own copy of
the game:

1. Load a save and open **Options > Game > Prepare graphics** (hold `-`, or press HOME).
   The title screen also points at the row, once, on an install that has never run it.
2. The port copies your save folder to `UserData/PrepareGraphics/save` first. The sweep
   itself never saves, and the copy is put back when it ends.
3. It then visits every room in the game (426 of them), one warp at a time, and waits in
   each room until the shader pipeline queue has drained: 2 seconds for the room to draw,
   then a second of quiet queue. Mario's HP is kept full while it runs, and the console
   is kept awake.
4. Everything compiled lands in the caches already kept in `UserData/`, so the next
   launches prewarm it and the effects are ready when you meet them in play.

**Time and stopping.** Expect roughly half an hour to an hour, once. The card on screen
shows the room, how many are done, and an estimate of the minutes left once the pace is
known. To stop, **hold B for a second or so**: progress is kept in
`UserData/PrepareGraphics/state.txt`, and the row reads "Continue" next time, picking up
from the next room. A room that will not load is skipped; if warps stop being taken, the
sweep stops and carries on from the next room on a later run.

**Before you run it.** Back up your save first: copy the `UserData` folder (or at least
its save data) somewhere off the card. The sweep is built to leave your save exactly as
it found it, but it is new code walking every room in the game, and a backup turns any
surprise into a non-event. Keep the console charged or docked, too: it stays awake for
the whole run.

## Known issues and performance

- **Button icons.** Not every button prompt has a replacement picture yet, so some prompts
  still show the original icons.
- **The tutorial Wii Remote.** The floating Wii Remote the game shows while teaching
  pointing (the Shooting Star tutorial, the arcade how-to) is still the game's own model:
  no route that hides it without also hiding things it should not is in the current
  build, so it stays visible for now. The game's own instruction line already reads
  "Aim with the right stick!".
- **Stale marker files after an update.** If the Shooting Star aiming cursor is missing,
  check for `UserData/button_icons/nintendo/marker7` on the card and delete that folder
  (see the Updating note above); it is left over from an older build, not part of this one.
- **The pointer during flips.** The Wii Remote pointer (Tippi) is blocked out during the
  flip transition between 2D and 3D; it comes back once the flip finishes.
- **Performance.** The port is CPU-bound on the guest side: the translated game code, not the
  renderer, sets the frame rate. Set expectations by area, not by a single number. At a high
  overclock (CPU 1963 MHz, GPU 1152 MHz, memory 2366 MHz, the clocks the port is
  developed and measured at) 2D areas run at or near full speed; the heavy 3D side is
  much slower, so keep expectations low there for now. The game steps its physics once
  per frame, so a lower frame rate is a slower game, not just a less smooth one, and at stock
  clocks everything runs slower still. Overclocking is the single biggest lever if you want
  more speed; the numbers above come from a machine overclocked with Horizon-OC
  (https://github.com/Horizon-OC/Horizon-OC/releases).
- **First launch after installing** builds the shader and pipeline caches and can hitch (see
  step 5 of the install). Warm launches are fast. The caches live in `UserData/`; deleting
  that folder brings the slow first launch back, along with deleting your saves. To build
  the caches ahead of play instead, run **Prepare graphics** once (see its section above).
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
`cacert.pem`, `wii_bootstrap/`, a `licenses/` folder and `UserData/button_icons/`). Copy
that staging `switch` folder over the SD card's, **merging**: do not delete the card's
`spm-nx` folder first, because its `UserData/` (saves, settings, caches) and `game/` are
not in the staging folder and a replace wipes them. For a dev build where only the code
changed, copying just `spm-nx.nro` over is enough (plus `UserData/button_icons/` when the
icons changed). (`--out DIR` chooses the staging folder, `--out none` skips
it; without staging, copy those files from `~/spm-switch/build/` to `sdmc:/switch/spm-nx/` by
hand.) Put your extracted disc (`Disc\DATA`, the folder with `files` and `sys`) in
`sdmc:/switch/spm-nx/game/`. Launch it with title takeover (hold R while starting a game), not
from the album: the applet doesn't get enough memory. Logs land in
`sdmc:/switch/spm-nx/UserData/Logs/`.

The package ships no prebuilt shader or pipeline cache. On the first run the port builds both
as new effects appear, so the first session can stutter in places; the caches it builds are
kept in `UserData/` on the SD card and reused, so later runs do not repeat it. The
**Prepare graphics** row (Options > Game) builds them ahead of play instead, on the
player's own console; see its section above.

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
scripts/                     build container, build-switch.sh, symbolize.sh (names the code offsets in a
                             crash log) and profile-report.py (names and ranks a guest profiler capture)
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
page protection catching the odd cases. The Switch build does the same now (phase 2): guest RAM
sits in heap blocks remapped as code memory and mirrored into a flat window with the Horizon
SVCs, so a flat read is one load and a byte swap. It started (phase 1) on the runtime's checked
access path, plain heap blocks with the checks done in software: slower, with no kernel
dependencies, and still the mode to reach for when a memory bug needs chasing. Details:
`docs/GUEST-MEMORY-PLAN.md`.

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
itself, with the buttons from the mapping table above. The
prompt pictures that can be drawn from scratch do ship: the small button glyphs inside text
boxes and the shake prompt's icon show Switch buttons in the package. The bigger prompt
pictures split in two at runtime: the round button pictures in signs and tutorials and the
3D buttons' faces are composited by the port itself, drawing the Switch glyph over the
game's own texture when it appears, so those show Switch buttons with no texture pack at
all. The Use Tippi picture is the exception: it is only covered by compositing the glyphs
into the HD texture pack's redrawn game textures, and those composited results are never
staged, so without the pack that one picture stays Wii in the package. The full
set (every prompt picture replaced) stays a do-it-yourself drop-in: run
`tools/make-button-icons.py` against your own copy of the HD pack and copy the resulting
`button_icons/` into `UserData/`.

For the project's position on Nintendo's trademarks and copyrighted material, see the Legal notice
in the root [`README.md`](../README.md).
