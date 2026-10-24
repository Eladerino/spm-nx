#!/usr/bin/env bash
# From SwitchWaker (switch/native/clang-launcher.sh, MIT; see switch/THIRD_PARTY.md), adapted.
#
# CMake compiler launcher for the Switch build (switch/cmake/SpmSwitch.cmake sets it for every
# target after Dawn and the SDL shim). CMake runs it as
#   clang-launcher.sh <devkitA64 gcc or g++> <arguments...>
# and it compiles with clang for the Switch's Cortex-A57 instead, because WiiCompiled's runtime
# and translated code only support Clang. SPM_SWITCH_CLANG / SPM_SWITCH_CLANGXX pick the compiler
# (default: clang-19/clang++-19 when installed, else clang/clang++; 18 or newer).
#
# - The C and C++ headers stay devkitA64's (newlib, libstdc++ 15), in GCC's
#   search order, so the objects link with the rest of the NRO (built by GCC).
#   clang's own resource directory replaces GCC's (intrinsics, stddef.h...).
# - GCC's thread pointer model (-mtp=soft: libnx's __aarch64_read_tp) has no
#   clang equivalent, so thread_local variables use emulated TLS
#   (__emutls_get_address from devkitA64's libgcc). The runtime has several
#   (guest CPU context, SEH-style recovery targets); emulated TLS costs a call
#   per access, which is worth measuring once the game runs.
set -euo pipefail

compiler=$1
shift
pick() { command -v "$1" >/dev/null 2>&1 && echo "$1" || echo "$2"; }
case $compiler in
    *g++) clang=${SPM_SWITCH_CLANGXX:-$(pick clang++-19 clang++)} cxx=1 ;;
    *gcc) clang=${SPM_SWITCH_CLANG:-$(pick clang-19 clang)} cxx=0 ;;
    *) exec "$compiler" "$@" ;;
esac

dkp=${DEVKITPRO:-/opt/devkitpro}
gcc_root=$dkp/devkitA64/aarch64-none-elf
cxx_include=$(echo "$gcc_root"/include/c++/*)
resource=$("$clang" -print-resource-dir)

args=()
for arg in "$@"; do
    case $arg in
        # GCC-only code generation options; the target is set below.
        # -fno-lifetime-dse: CMake checks it with devkitA64's GCC (GameConfig.cmake); clang 19
        # rejects it, and does not drop a destructor's stores, so the flag is not needed here.
        -mtp=*|-ftls-model=*|-march=*|-mtune=*|-fmax-errors=*|-fno-lifetime-dse) ;;
        *) args+=("$arg") ;;
    esac
done

includes=(-nostdinc)
if [[ $cxx == 1 ]]; then
    includes+=(-isystem "$cxx_include" -isystem "$cxx_include/aarch64-none-elf"
               -isystem "$cxx_include/backward")
fi
includes+=(-isystem "$resource/include" -isystem "$gcc_root/include")

exec "$clang" --target=aarch64-none-elf -mcpu=cortex-a57 -femulated-tls \
    "${includes[@]}" "${args[@]}"
