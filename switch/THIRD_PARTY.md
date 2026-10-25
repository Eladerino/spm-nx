# Third-party code in switch/

Everything else under `switch/` is this fork's own code and, like the rest of NewSuperPaperBros,
is under the repository's GPLv3 license.

| Path | Origin | License |
|---|---|---|
| `sdl3_shim/` (all files but `sdl3_shim_wiicompiled.c`) | [SwitchWaker](https://github.com/centollOS/SwitchWaker) `switch/aurora/sdl3_shim/` at `92307ff`, with the local changes in `sdl3_shim/PATCHES.md` | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `sdl3_shim/sdl3_shim_wiicompiled.c` | this fork | GPLv3 |
| `dawn/dawn.cmake` | SwitchWaker `switch/dawn/dawn.cmake` at `92307ff`, unchanged | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `dawn/patches/` | SwitchWaker `switch/dawn/patches/` at `92307ff`, unchanged; patches to Dawn, Tint and abseil | same as what they patch: BSD-3-Clause (Dawn, Tint), Apache-2.0 (abseil) |
| `aurora/sqlite_horizon.c` | SwitchWaker `switch/aurora/sqlite_horizon.c` at `92307ff`, prefix renamed | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `runtime/horizon_pthread.c` | SwitchWaker `switch/native/source/thread_wrap.c` at `92307ff`, reduced to the stack size, core, priority and detached-thread reaper parts | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `cmake/clang-launcher.sh` | SwitchWaker `switch/native/clang-launcher.sh` at `92307ff`, adapted (compiler override variables, extra flag filtering) | MIT (`licenses/SwitchWaker-MIT.txt`) |
| `scripts/Containerfile` | based on SwitchWaker `scripts/switch/Containerfile.native` (same image digest and packages, plus rsync) | MIT (`licenses/SwitchWaker-MIT.txt`) |

Fetched at build time, not stored here:

| Dependency | Version | License |
|---|---|---|
| [Dawn](https://github.com/encounter/dawn) (with Tint and abseil) | `266c1cf`, SwitchWaker's pin (`dawn/dawn.cmake`) | BSD-3-Clause, Apache-2.0 (abseil) |
| [SDL 3](https://github.com/libsdl-org/SDL) headers | 3.4.4, Aurora's pin in WiiCompiled | Zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) headers (audit build only) | v1.91.9b-docking | MIT |
| devkitPro devkitA64, libnx, deko3d | the image digest in `scripts/Containerfile` | GPL (toolchain), ISC (libnx), zlib (deko3d) |
