#!/usr/bin/env bash
# Switch build of New Super Paper Bros. Run on Linux or in WSL with docker or podman, after
# tools\build-spm.ps1 has translated the game on Windows: like tools/build-linux.sh, this reuses that
# output (wiicompiled\generated) and only compiles it, here for Horizon inside switch/scripts/Containerfile.
#   bash switch/scripts/build-switch.sh [--jobs N] [--audit] [--out DIR|none]
# --audit compiles only the runtime's sources for the Switch (no game, no Dawn, no network beyond the
# SDL/ImGui header downloads): the quick check after an upstream merge.
# Sources are copied to ~/spm-switch first. Output: ~/spm-switch/build/spm-nx.nro
# After a good build the NRO and its companion files are also copied, laid out like the SD card, to
# <DIR>/switch/spm-nx/; DIR defaults to Downloads\spm-nx-sd in your Windows profile under WSL. Drag
# that switch folder onto the card. --out none skips the copy.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
jobs=6   # translated shards need ~1 GB each
audit=0
out=auto
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --audit) audit=1 ;;
        --out) out=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }
image=spm-nx-build
"$engine" build -t "$image" -f "$root/switch/scripts/Containerfile" "$root/switch/scripts"

work=$HOME/spm-switch
mkdir -p "$work/wiicompiled"
echo "copying sources to $work"
rsync -a --delete "$root/switch/" "$work/switch/"
rsync -a --delete "$root/wiicompiled/runtime/" "$work/wiicompiled/runtime/"
rsync -a --delete "$root/wiicompiled/aurora-main/" "$work/wiicompiled/aurora-main/"
if [[ $audit == 0 ]]; then
    src=$root/wiicompiled
    [[ -f $src/generated/build_shards/shards.cmake ]] ||
        { echo "no translated game in $src/generated: run tools\\build-spm.ps1 on Windows first" >&2; exit 1; }
    rsync -a --delete --exclude functions "$src/generated/" "$work/wiicompiled/generated/"
    # The translator wrote absolute Windows paths into the shard list and the data blobs' .incbin
    # lines; inside the container the tree is at /work.
    if command -v wslpath >/dev/null; then
        win=$(wslpath -m "$src")
        grep -rlF "$win" "$work/wiicompiled/generated" --include='*.cmake' --include='*.S' --include='*.cpp' --include='*.h' |
            xargs -r sed -i "s|$win|/work/wiicompiled|g"
    fi
fi

# Separate build folders: CMake caches SPM_SWITCH_AUDIT, so a full build in the audit's folder would
# still skip Dawn and Aurora.
products=ON; target=WiiCompiled_nro; build=build; extra=(-DSPM_SWITCH_AUDIT=OFF)
if [[ $audit == 1 ]]; then
    products=OFF; target=mkw_switch_audit; build=build-audit; extra=(-DSPM_SWITCH_AUDIT=ON)
fi

"$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
    set -e
    cmake -S wiicompiled/runtime -B $build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
        -DMKW_BUILD_PRODUCTS=$products -DMKW_TRANSLATED_COMPILE_JOBS=$jobs ${extra[*]}
    cmake --build $build --target $target --parallel $jobs -- -k 0
"
if [[ $audit == 1 ]]; then
    echo "DONE: runtime compiles for Horizon"
else
    echo "DONE: $work/build/spm-nx.nro"
    if [[ $out == auto ]]; then
        out=
        if command -v wslpath >/dev/null && command -v cmd.exe >/dev/null; then
            profile=$(cd /mnt/c 2>/dev/null && cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')
            [[ -n $profile ]] && out=$(wslpath -u "$profile")/Downloads/spm-nx-sd
        fi
    fi
    if [[ -n $out && $out != none ]]; then
        dest=$out/switch/spm-nx
        mkdir -p "$dest"
        cp "$work/build/spm-nx.nro" "$work/build/dsp_coef.bin" "$work/build/initial_pipeline_cache.db" \
            "$work/build/cacert.pem" "$dest/"
        cp -r "$work/build/wii_bootstrap" "$dest/"
        # The license pack travels with the binary: the root LICENSE (GPLv3), the third-party
        # notices, the upstream credits and the Switch layer's own notices and license texts.
        mkdir -p "$dest/licenses"
        cp "$root/LICENSE" "$root/wiicompiled/THIRD-PARTY-NOTICES.md" "$root/tools/CREDITS.txt" \
            "$root/switch/THIRD_PARTY.md" "$dest/licenses/"
        cp "$root"/switch/licenses/*.txt "$dest/licenses/"
        echo "COPIED to $dest ($(date -r "$dest/spm-nx.nro" '+%Y-%m-%d %H:%M'))"
        echo "drag its switch folder onto the SD card, then launch with title takeover"
    elif [[ $out != none ]]; then
        echo "copy $work/build/spm-nx.nro to sdmc:/switch/spm-nx/, then launch with title takeover"
    fi
fi
