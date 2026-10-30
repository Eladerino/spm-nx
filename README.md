# New Super Paper Bros.

> **This repository is spm-nx**, a Nintendo Switch port of [New Super Paper Bros](https://github.com/josiahg2502-glitch/NewSuperPaperBros) by JosiahG. The Switch port is by Elad (Eladerino). It carries the Switch port of that project (the deko3d renderer and Horizon support); the original PC project lives at the link above, and full credits are in the [Credits](#credits) section below.

A native PC port of **Super Paper Mario** (Wii, 2007), made by statically recompiling the game's PowerPC code to C++
with [WiiCompiled](https://github.com/patchzyy/Wiicompiled). No emulator: the game runs as a Windows or Linux program.
A native port of **Super Paper Mario** (Wii, 2007), made by statically recompiling the game's PowerPC code to C++
with [WiiCompiled](https://github.com/patchzyy/Wiicompiled). No emulator: the game runs as a Windows, Linux, macOS or
Android program, or as a RetroArch core on Android.

![Flipside in cinematic 2.39:1 widescreen](screenshots/mac_02_cinematic.png)

| | |
|---|---|
| ![Gloam Valley](screenshots/mi1_01.png) | ![Lineland Road](screenshots/he1_02.png) |
| ![The Bitlands](screenshots/ta1_02.png) | ![Sammer's Kingdom](screenshots/wa1_02.png) |

## Nintendo Switch port

This fork also runs on a modded Nintendo Switch as a homebrew NRO (`spm-nx.nro`): the game boots
and plays on hardware, rendered by a deko3d interposer that draws Aurora's WebGPU frames on the
Switch GPU. The easy path is the release: download it, copy it to the SD card, add the files from
your own disc, and launch. Building the NRO from source instead is the alternative, and it is
documented in full as well.

Two things differ from the PC build. The Switch build currently needs the **USA disc (R8PE01,
revision 0)**; the European disc (R8PP01) is PC-only for now, because only the USA game has been
translated for the Switch. And there is no launcher on the Switch: the NRO boots by title
takeover to a startup screen, and settings live in an in-game menu made for the Switch pad
(hold `-` for about half a second to open it). Fresh Switch installs start from Switch defaults
(1.5x rendering, which is the 720p screen's native size, 16:9, frame interpolation off), not
the PC ones above. Installing, building and the state of the port are documented in
[`switch/README.md`](switch/README.md).

## Features

- Up to 4K, borderless 1080p by default
- Widescreen: 16:9, fill or cinematic 2.39:1, or the original 4:3
- Frame interpolation up to 240 fps (120, 180 or 240)
- Xbox, PlayStation and Nintendo gamepads plus keyboard, with press-to-bind remapping
- The Wii Remote pointer (Tippi, the HOME menu) on the right stick or the mouse
- On-screen buttons match your controller (Xbox, PlayStation or Nintendo), with a controls screen for it
- Mods: drop-in folders that replace game files or textures
- A launcher that installs the game from your own disc image
- Android (a RetroArch libretro core with save states, or a standalone app) and macOS builds from source

**No game data is included**: the game's files are read from your own copy of the game.

## How to play

You need **Windows 10/11 (64-bit)**, a graphics card with DirectX 12 or Vulkan, and your own dump of
**Super Paper Mario, USA Wii disc (R8PE01, revision 0) or European disc (R8PP01: English, French, German, Spanish, Italian)** as `.iso`, `.ciso` or `.wbfs`.

1. Download `NewSuperPaperBros.zip` from [Releases](../../releases) and unzip it.
2. Open `New Super Paper Bros.exe`. Windows may show "Windows protected your PC" because the exe isn't signed:
   click **More info**, then **Run anyway**.
3. On the **Play** page choose your disc image. The game files are copied into the `Disc` folder once; the image
   isn't needed after that.
4. Press **PLAY**.

In the game, F10 opens the settings menu and F11 switches between fullscreen and a window. (That
is the PC build; on Switch, hold `-` instead. See [`switch/README.md`](switch/README.md).)

**Controls (Xbox layout):** left stick or D-pad = Wii Remote D-pad, A = 2 (jump), B or X = 1, Y = A, RB or RT = shake,
Start = +, Back or LB = -, right stick = pointer. Keyboard and mouse work too. Change them on the launcher's
**Controls** page.

**Mods:** on the launcher's **Mods** page, click **Open mods folder** and put each mod in its own folder:
`files\` mirrors the disc (replacing game files) and/or `textures\` holds texture replacements. Tick or untick mods
to switch them on and off.

**Saves from a Wii or Dolphin:** on the launcher's **Play** page click **Import Wii / Dolphin save...** and pick
`data.bin` (copied to an SD card from the Wii Menu's Data Management) or `wiimario00` in Dolphin's
`Wii\title\00010000\52385045\data` folder. Your current save is kept as a backup.

**Updating:** unzip a new release over the old folder. Your disc, saves, settings and mods are kept.

**Linux:** download `NewSuperPaperBros-linux.tar.gz` instead, unpack it and open `New Super Paper Bros` (the same
launcher). You need 64-bit Linux from 2022 or newer (glibc 2.35) with Vulkan graphics drivers. Linux support is new:
please report problems in [Issues](../../issues).

## Building from source

You need Windows, Python 3, a .NET 8 runtime (a `dotnet` on your PATH: `build-spm.ps1` stops
without one) and your own disc image. The rest of the toolchain (LLVM/MinGW, CMake, Ninja) and
the pinned dependencies are downloaded into the repository's `toolchain\` and
`wiicompiled\Dependencies\` folders on the first build; nothing else is installed system-wide.

**Preparing the game files.** The translation reads the game's code from `game\main.dol` and the
pre-linked REL produced by the commands below. `main.dol` must carry two code patches from the
"SPM High Definition Audio" mod (by Toadette Hack Fan and PaperToadette, with game patches by
JohnP55): `wiicompiled/projects/spm/recomp.yml` (project `spm-r8pe01`) pins the SHA-256 of the
patched DOL, and an unpatched `main.dol` from a clean dump does not match it. The mod's
canonical listing is the Super Paper Mario page of the Dolphin Emulator Wiki, under Enhancements
("HD Audio Mod"). Get the mod there and follow the mod's own instructions to apply its patches
to your disc's `main.dol` before copying it into `game\`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-launcher.ps1
# Extract your disc image to Disc\DATA (the translation reads the game's code from it):
& "launcher\publish\New Super Paper Bros.exe" --extract "C:\path\to\Super Paper Mario.wbfs" Disc\DATA
# The game's code inputs in game\: the disc's main.dol (with the HQ music mod's two code patches; recomp.yml pins
# its hash) and relF.bin pre-linked at its boot address:
copy Disc\DATA\sys\main.dol game\main.dol
python wiicompiled\projects\spm\tools\prelink_spm_rel.py Disc\DATA\files\rel\relF.bin game\spm_linked.rel game\spm_rel_layout.json
powershell -ExecutionPolicy Bypass -File tools\build-spm.ps1
powershell -ExecutionPolicy Bypass -File tools\package-spm.ps1 -Zip
```

The packager also expects the HQ music mod in `Disc\DATA\files\sound` (originals in `Disc\sound_original`) and the
HD texture pack in `Play\UserData\texture_replacements\SPM_HD_4x_BC3`.

For Linux, after those steps run `bash tools/build-linux.sh` and `bash tools/build-linux.sh --pal` in WSL (Ubuntu
22.04) or on Linux, then `bash tools/package-linux.sh` for `dist/NewSuperPaperBros-linux.tar.gz`.

For Android and macOS (USA disc only), see
[`wiicompiled/docs/android-and-macos.md`](wiicompiled/docs/android-and-macos.md).

`build-spm.ps1` translates the game and compiles it into `Play\SuperPaperMario.exe`; `package-spm.ps1` assembles the
release folder in `dist\` (`-Zip` also writes the zip). `tools\sweep-spm-maps.py` loads every map and screenshots it
for regression checks.

| Folder | What it is |
|---|---|
| `wiicompiled/translator` | PowerPC to C++ static recompiler (C#) |
| `wiicompiled/runtime` | Runtime and Wii SDK HLE (OS, DVD, NAND, WPAD/KPAD, AX audio, VI, GX) |
| `wiicompiled/aurora-main` | Aurora: Wii GX on WebGPU (Dawn / Direct3D 12) |
| `wiicompiled/projects/spm` | This game's project: symbols and function map |
| `launcher` | The launcher (C# WinForms): disc setup, display options, controls, mods |
| `launcher/linux` | The same launcher for Linux (C# Avalonia) |
| `tools` | Build, package and test scripts (`tools/android`: Android and macOS) |

For the Switch build, the translation above is the input: the Building section of
[`switch/README.md`](switch/README.md) runs the same preparation and translation, then compiles
the result for Horizon instead of Windows.

## Credits

- **Original project and PC port:** JosiahG, creator of New Super Paper Bros, the PC recomp this
  repository ports to the Switch. The PC project lives at
  [josiahg2502-glitch/NewSuperPaperBros](https://github.com/josiahg2502-glitch/NewSuperPaperBros);
  this repository is the Switch port of it.
- **Switch port (spm-nx):** Elad (Eladerino)
- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** (the recompiler and runtime) by patchzyy and
  contributors, GPLv3
- **[Aurora](https://github.com/encounter/aurora)** by Luke Street and the Aurora contributors, MIT; Dawn, SDL3 and the other components listed in
  [`wiicompiled/THIRD-PARTY-NOTICES.md`](wiicompiled/THIRD-PARTY-NOTICES.md)
- **[SwitchWaker](https://github.com/centollOS/SwitchWaker) and
  [SwitchWakerHD](https://github.com/centollOS/SwitchWakerHD)** by centollOS, MIT: the Switch
  layer's SDL shim and Dawn recipe come from SwitchWaker, the deko3d renderer descends from
  SwitchWakerHD's backend through it, and the Switch settings menu is modelled on
  SwitchWakerHD's overlay. Per-file origins are in
  [`switch/THIRD_PARTY.md`](switch/THIRD_PARTY.md)
- **[deko3d](https://github.com/devkitPro/deko3d) and [uam](https://github.com/devkitPro/uam)** by
  devkitPro and contributors, zlib: the GPU API and the GLSL to DKSH shader compiler the Switch
  renderer is built on
- **Higher-quality music:** the "SPM High Definition Audio" mod by Toadette Hack Fan and PaperToadette, with game
  patches by JohnP55
- **HD textures:** converted from the "SPM Textures v1.0" HD texture pack
- **Launcher background:** ["Super Paper Mario Wallpaper"](https://www.deviantart.com/fawfulthegreat64/art/Super-Paper-Mario-Wallpaper-891704524) by FawfulTheGreat64
- **Controller button icons and pictures:** "Input Prompts" by [Kenney](https://kenney.nl), CC0
- **Dolphin Emulator** data files (DSP coefficients, Wii system files), GPL-2.0-or-later
- Symbol names from the Super Paper Mario decompilation community ([spm-decomp](https://github.com/SeekyCt/spm-decomp))

Full credits and licenses ship in the PC release as `CREDITS.txt`, `LICENSE.txt` and `engine\licenses\`;
the Switch package carries them in the `licenses\` folder beside the NRO.

## AI assistance

This project is developed with help from AI coding tools. All code is reviewed and tested by the maintainer before it is committed.

## License

GPLv3, see [`LICENSE`](LICENSE). Bundled third-party components keep their own licenses.

Super Paper Mario and all related characters are trademarks and copyrights of Nintendo. This is an unofficial fan
project, not affiliated with or endorsed by Nintendo. This repository contains no game code or data; the release
contains the recompiled program, the community mods credited above and Nintendo character artwork used by the
launcher, but none of the game's disc data, which is read from your own copy of the game.

## Legal notice

This is an unofficial, non-commercial fan project. It is not affiliated with, endorsed by, or
sponsored by Nintendo.

Super Paper Mario, Nintendo, and Wii are trademarks of Nintendo. The game, its assets, its
characters (including Tippi), its music, and its artwork are copyrighted material belonging to
Nintendo and its respective owners. All rights reserved.

Screenshots in this project's documentation depict Nintendo's game and are shown for identification
and documentation purposes. The launcher icon artwork is Nintendo's.

The GPLv3 license in this repository applies to the project's own code only. It grants no rights in
Nintendo's material.

This project distributes no game data. A legally obtained copy of the game is required, and users
extract the files from their own disc.
