#!/usr/bin/env bash
# Turns the code offsets in a log from the console into function names and source lines, with
# addr2line from the build container and the spm-nx.elf of the last build (build-switch.sh). It reads
# both forms the runtime prints: spm-nx.elf+0x... (crash.txt, crash lines in stderr.log) and nro+...
# (the profiler's hot list). Run in WSL, on the log copied off the SD card:
#   bash switch/scripts/symbolize.sh /mnt/c/Users/<you>/Downloads/stderr.log
# The ELF must be the build that produced the log: rebuilding first gives wrong names.
set -euo pipefail

log=${1:?usage: symbolize.sh LOG}
elf=${SPM_SWITCH_ELF:-$HOME/spm-switch/build/spm-nx.elf}
[[ -f $log ]] || { echo "no such log: $log" >&2; exit 1; }
[[ -f $elf ]] || { echo "no ELF at $elf (set SPM_SWITCH_ELF)" >&2; exit 1; }
engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }

# Both forms are offsets from the module's load base, which is address 0 in the ELF.
mapfile -t offsets < <(grep -aoE 'spm-nx\.elf\+0x[0-9a-fA-F]+|nro\+[0-9a-fA-F]+' "$log" |
    sed -E 's/.*\+(0x)?/0x/' | awk '!seen[tolower($0)]++')
[[ ${#offsets[@]} -gt 0 ]] || { echo "no spm-nx.elf+0x... or nro+... offsets in $log" >&2; exit 1; }

# One addr2line run for all of them: -a starts each line with its offset, -p keeps an offset and its
# function on that line, -i adds the functions it was inlined into.
"$engine" run --rm -v "$(dirname "$elf"):/elf:ro" spm-nx-build bash -c '
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -a -f -C -i -p -e /elf/'"$(basename "$elf")"' "$@"' _ "${offsets[@]}"
