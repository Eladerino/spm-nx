#!/usr/bin/env python3
"""Names and ranks the guest profiler's raw stacks from a stderr.log, with the build's symbols.

When the "profile" Diagnostics switch is turned off, the runtime prints its hot list and then every
sampled stack of the game thread and of aurora's GX decode thread as module offsets ("[profile] raw"
lines, guest_profile_format.h RawLines). This script names those offsets with addr2line against the
spm-nx.elf of the same build, inlined functions included, and prints per thread:

  - self time by function (where the samples landed),
  - self time by innermost inlined function (a hot helper inside a big function),
  - inclusive time by function (samples with that function anywhere on the stack),
  - the hottest source lines,
  - the top call chains, by function.

Run it in WSL on a copy of the log taken off the SD card, with no rebuild since that log's build:
  python3 switch/scripts/profile-report.py /mnt/c/dev/stderr.log > /mnt/c/dev/profile-report.txt
addr2line runs in the spm-nx-build container (podman or docker), like symbolize.sh; --addr2line
uses a local one instead. The ELF defaults to $SPM_SWITCH_ELF, then ~/spm-switch/build/spm-nx.elf.
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

CONTAINER_ADDR2LINE = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line"

RE_BEGIN = re.compile(r"\[profile\] raw begin: (.*)$")
RE_PATH = re.compile(r"\[profile\] raw (\w+) path (\d+):(.*)$")
RE_LEAF = re.compile(r"\[profile\] raw (\w+) leaf (\d+) ((?:nro|abs)\+[0-9A-Fa-f]+) (\d+)\s*$")
RE_OTHER = re.compile(r"\[profile\] raw (\w+) other (\d+) samples in (\d+) stacks")
RE_ADDR = re.compile(r"^(nro|abs)\+([0-9A-Fa-f]+)$")


class Capture:
    def __init__(self):
        self.paths = {}  # number -> [address token]
        self.leaves = []  # (samples, leaf token, path number)
        self.other = 0


def parse_log(path):
    """The last profiling session's raw lines in the log: {thread: Capture}, and its begin line."""
    captures, begin = {}, None
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if "[profile] raw" not in line:
                continue
            if m := RE_BEGIN.search(line):
                captures, begin = {}, m.group(1)
            elif m := RE_PATH.search(line):
                tokens = m.group(3).split()
                captures.setdefault(m.group(1), Capture()).paths[int(m.group(2))] = [] if tokens == ["-"] else tokens
            elif m := RE_LEAF.search(line):
                captures.setdefault(m.group(1), Capture()).leaves.append((int(m.group(2)), m.group(3), int(m.group(4))))
            elif m := RE_OTHER.search(line):
                captures.setdefault(m.group(1), Capture()).other += int(m.group(2))
    return captures, begin


def lookup_offset(token, return_address):
    """The ELF address to name for a sampled token: a return address names its call instruction."""
    m = RE_ADDR.match(token)
    if not m or m.group(1) != "nro":
        return None
    offset = int(m.group(2), 16)
    return offset - 4 if return_address and offset >= 4 else offset


def run_addr2line(offsets, elf, local):
    """{offset: [(function, file:line), ...]} innermost first, for every offset."""
    if not offsets:
        return {}
    args = ["-a", "-f", "-C", "-i"]
    if local:
        cmd = [local] + args + ["-e", elf]
    else:
        engine = shutil.which("podman") or shutil.which("docker")
        if not engine:
            sys.exit("needs podman or docker for the build container's addr2line (or pass --addr2line)")
        elf_dir, elf_name = os.path.split(os.path.abspath(elf))
        cmd = [engine, "run", "--rm", "-i", "-v", f"{elf_dir}:/elf:ro", "spm-nx-build",
               CONTAINER_ADDR2LINE] + args + ["-e", f"/elf/{elf_name}"]
    stdin = "".join(f"0x{o:x}\n" for o in offsets)
    out = subprocess.run(cmd, input=stdin, capture_output=True, text=True, check=True).stdout
    names, current, pending = {}, None, None
    for line in out.splitlines():
        if re.fullmatch(r"0x[0-9a-fA-F]+", line.strip()):
            current = int(line.strip(), 16)
            names[current] = []
            pending = None
        elif current is not None:
            if pending is None:
                pending = line.strip()
            else:
                names[current].append((pending, line.strip()))
                pending = None
    return names


OPERATORS = ["operator<<=", "operator>>=", "operator<<", "operator>>", "operator<=>", "operator<=",
             "operator>=", "operator->*", "operator->", "operator<", "operator>"]


def short_name(name):
    """A demangled name without template arguments and its parameter list, for grouping and reading."""
    if not name or name == "??":
        return "??"
    name = name.replace("(anonymous namespace)", "{anon}")
    for i, op in enumerate(OPERATORS):
        name = name.replace(op, f"\x01{i}\x02")
    out, depth = [], 0
    for ch in name:
        if ch == "<":
            if depth == 0:
                out.append("<>")
            depth += 1
        elif ch == ">" and depth > 0:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    name = "".join(out)
    for suffix in (" const", " volatile", " &&", " &"):
        while name.endswith(suffix):
            name = name[: -len(suffix)]
    if name.endswith(")"):
        depth = 0
        for i in range(len(name) - 1, -1, -1):
            if name[i] == ")":
                depth += 1
            elif name[i] == "(":
                depth -= 1
                if depth == 0:
                    if i > 0:
                        name = name[:i]
                    break
    name = re.sub("\x01(\\d+)\x02", lambda m: OPERATORS[int(m.group(1))], name)
    return name if len(name) <= 140 else name[:137] + "..."


def short_line(location):
    if not location or location.startswith("??"):
        return "??"
    path, _, line = location.rpartition(":")
    line = line.split(" ")[0]
    return f"{os.path.basename(path)}:{line}"


def frame_names(token, return_address, names):
    """(innermost name, concrete name, source line) of one stack frame."""
    offset = lookup_offset(token, return_address)
    chain = names.get(offset) if offset is not None else None
    if not chain or chain[0][0] == "??":
        return (f"?? {token}", f"?? {token}", f"?? {token}", [f"?? {token}"])
    inner = short_name(chain[0][0])
    concrete = short_name(chain[-1][0])
    return (inner, concrete, short_line(chain[0][1]), [short_name(n) for n, _ in chain])


def share(count, total):
    return f"{100.0 * count / total:5.1f}%" if total else "  -  "


def report_thread(thread, capture, names, top, out):
    total = sum(s for s, _, _ in capture.leaves) + capture.other
    out.write(f"\n==== {thread} thread: {total} samples")
    if capture.other:
        out.write(f" ({capture.other} in stacks the log did not list)")
    out.write("\n")
    if total == 0:
        return
    self_concrete = collections.Counter()
    self_inner = collections.Counter()
    self_line = collections.Counter()
    inclusive = collections.Counter()
    chains = collections.Counter()
    for samples, leaf, path_number in capture.leaves:
        stack = [(leaf, False)] + [(t, True) for t in capture.paths.get(path_number, [])]
        frames = [frame_names(t, ret, names) for t, ret in stack]
        inner, concrete, line, _ = frames[0]
        self_concrete[concrete] += samples
        self_inner[inner if inner == concrete else f"{inner}  [in {concrete}]"] += samples
        self_line[f"{line}  {inner}"] += samples
        seen = set()
        for frame in frames:
            seen.update(frame[3])
        for name in seen:
            inclusive[name] += samples
        labels = []
        for f in frames:
            if not labels or labels[-1] != f[1]:
                labels.append(f[1])
        chains[" < ".join(labels[:5])] += samples

    def section(title, counter, limit):
        out.write(f"\n-- {thread}: {title}\n")
        for name, count in counter.most_common(limit):
            out.write(f"{share(count, total)} {count:7d}  {name}\n")

    section("self time by function", self_concrete, top)
    section("self time by innermost inlined function", self_inner, top)
    section("inclusive time by function (anywhere on the stack, inlined included)", inclusive, top)
    section("hottest source lines", self_line, top)
    section("top call chains (by function, leaf first)", chains, max(10, top // 2))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("log")
    parser.add_argument("--elf", default=os.environ.get("SPM_SWITCH_ELF",
                                                        os.path.expanduser("~/spm-switch/build/spm-nx.elf")))
    parser.add_argument("--addr2line", help="a local aarch64 addr2line instead of the build container's")
    parser.add_argument("--top", type=int, default=50, help="lines per section (default 50)")
    args = parser.parse_args()
    if not os.path.isfile(args.log):
        sys.exit(f"no such log: {args.log}")
    if not os.path.isfile(args.elf):
        sys.exit(f"no ELF at {args.elf} (set SPM_SWITCH_ELF or pass --elf)")
    captures, begin = parse_log(args.log)
    if not captures:
        sys.exit("no [profile] raw lines in the log: turn the profile switch on, then off, in the same run")
    offsets = set()
    for capture in captures.values():
        for _, leaf, _ in capture.leaves:
            offsets.add(lookup_offset(leaf, False))
        for tokens in capture.paths.values():
            for token in tokens:
                offsets.add(lookup_offset(token, True))
    offsets.discard(None)
    names = run_addr2line(sorted(offsets), args.elf, args.addr2line)
    out = sys.stdout
    out.write(f"profile report for {os.path.basename(args.log)} against {os.path.basename(args.elf)}\n")
    if begin:
        out.write(f"capture: {begin}\n")
    out.write("Shares are of the thread's own samples. Self time is where a sample landed; inclusive time\n"
              "counts a sample once for every function on its stack. Chains can end early where code\n"
              "keeps no frame pointer.\n")
    for thread in ["game", "decode"] + sorted(set(captures) - {"game", "decode"}):
        if thread in captures:
            report_thread(thread, captures[thread], names, args.top, out)


if __name__ == "__main__":
    main()
