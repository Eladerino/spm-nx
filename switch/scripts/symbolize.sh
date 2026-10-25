#!/usr/bin/env bash
# Turns the spm-nx.elf+0x... offsets in a log from the console (stderr.log, crash.txt) into function
# names and source lines, with addr2line from the build container and the spm-nx.elf of the last
# build (build-switch.sh). Run in WSL, on the log copied off the SD card:
#   bash switch/scripts/symbolize.sh /mnt/c/Users/<you>/Downloads/stderr.log
# The ELF must be the build that produced the log: rebuilding first gives wrong names.
set -euo pipefail

log=${1:?usage: symbolize.sh LOG}
elf=${SPM_SWITCH_ELF:-$HOME/spm-switch/build/spm-nx.elf}
[[ -f $log ]] || { echo "no such log: $log" >&2; exit 1; }
[[ -f $elf ]] || { echo "no ELF at $elf (set SPM_SWITCH_ELF)" >&2; exit 1; }
engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }

mapfile -t offsets < <(grep -ao 'spm-nx\.elf+0x[0-9a-fA-F]*' "$log" | sed 's/.*+//' | awk '!seen[$0]++')
[[ ${#offsets[@]} -gt 0 ]] || { echo "no spm-nx.elf+0x... offsets in $log" >&2; exit 1; }

"$engine" run --rm -v "$(dirname "$elf"):/elf:ro" spm-nx-build bash -c '
    for offset in "$@"; do
        echo "== $offset"
        /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -i -e /elf/'"$(basename "$elf")"' "$offset"
    done' _ "${offsets[@]}"
