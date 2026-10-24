"""Read-only inventory of SPM runtime guest addresses and inherited remap evidence.

Run with Python 3; --self-test checks the scanner without reading the game.
Corroborating an address does not verify the C++ structure offsets used with it.
"""
import argparse
import bisect
import collections
import csv
import json
import re
import struct
from pathlib import Path


class Dol:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if len(self.data) < 0x100:
            raise ValueError(f"Short DOL: {path}")
        self.sections = []
        for i in range(18):
            off, addr, size = (self.word(base + i * 4) for base in (0, 0x48, 0x90))
            if size:
                if off + size > len(self.data):
                    raise ValueError(f"DOL section {i} exceeds file")
                self.sections.append((addr, size, off, i < 7))
        self.bss = (self.word(0xD8), self.word(0xDC))

    def word(self, off):
        return struct.unpack_from(">I", self.data, off)[0]

    def read(self, addr, size):
        for start, length, off, _ in self.sections:
            if start <= addr and addr + size <= start + length:
                return self.data[off + addr - start:off + addr - start + size]
        raise ValueError(f"DOL address not backed by file: {addr:08X}+{size:X}")

    def region(self, addr):
        for start, size, _, code in self.sections:
            if start <= addr < start + size:
                return "text" if code else f"data@{start:08X}"
        start, size = self.bss
        return "bss" if start <= addr < start + size else "outside-dol"


def signed16(n):
    return n - 0x10000 if n & 0x8000 else n


def refs(words, start, sda=0x80574F20, sda2=0x80576280):
    """Conservative straight-line constant propagation; branch joins lose scratch values."""
    regs = {2: sda2, 13: sda}
    found = []
    for i, w in enumerate(words):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        value = None
        if op in (14, 15):
            base = 0 if ra == 0 else regs.get(ra)
            if base is not None:
                value = (base + (signed16(imm) << (16 if op == 15 else 0))) & 0xFFFFFFFF
            regs.pop(rd, None)
            if value is not None:
                regs[rd] = value
        elif op in (24, 25):
            base = regs.get(rd)
            regs.pop(ra, None)
            if base is not None:
                value = base | (imm << (16 if op == 25 else 0))
                regs[ra] = value
        elif 32 <= op <= 55:
            base = 0 if ra == 0 else regs.get(ra)
            if base is not None:
                value = (base + signed16(imm)) & 0xFFFFFFFF
            if op <= 43 and op not in (36, 37, 38, 39):
                regs.pop(rd, None)
            if op & 1:
                regs.pop(ra, None)
        elif op in (16, 18, 19):
            regs = {2: sda2, 13: sda}
        elif op not in (10, 11, 17, 47, 48, 50, 52, 54, 59, 63):
            # Losing an unmodelled arithmetic value is safer than inventing a pointer.
            regs.pop(rd, None)
            if op == 31:
                regs.pop(ra, None)
        if op != 15 and value is not None and 0x80004000 <= value < 0x81800000:
            found.append((value, start + i * 4))
    return found


def active_lines(text):
    """Remove comments/strings and literal #if 0 branches, retaining source line numbers.

    Other conditional branches remain candidates and are not claimed to be compiled.
    """
    text = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: "\n" * m.group().count("\n"), text, flags=re.S)
    stack = []
    for number, line in enumerate(text.splitlines(), 1):
        d = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if d:
            kind, expr = d.groups()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append(False if kind == "if" and re.fullmatch(r"\s*0\s*", expr) else None)
            elif kind in ("else", "elif") and stack and stack[-1] is not None:
                stack[-1] = not stack[-1] if kind == "else" else None
            elif kind == "endif" and stack:
                stack.pop()
            continue
        if False not in stack:
            yield number, line


def load_names(path):
    return {int(a, 16): n.strip() for line in path.read_text().splitlines()
            if len(p := line.split(None, 1)) == 2 for a, n in [p]}


def sdk_evidence(dol, wc):
    def word(pc):
        return struct.unpack('>I', dol.read(pc, 4))[0]

    def lis_addi(pc, low=None):
        hi, lo = word(pc), word(low if low is not None else pc + 4)
        assert hi >> 26 == 15 and lo >> 26 == 14
        assert (hi >> 21) & 31 == (lo >> 16) & 31
        return ((hi & 0xFFFF) << 16) + signed16(lo & 0xFFFF)

    ptr_load = word(0x80291310)
    assert ptr_load >> 26 == 32 and (ptr_load >> 16) & 31 == 2
    gxptr = 0x80576280 + signed16(ptr_load & 0xFFFF)
    gxdata = word(gxptr)
    fifo = lis_addi(0x80291314)
    # These offsets come from the exact GXBeginDisplayList load/store instructions.
    offsets = {pc: signed16(word(pc) & 0xFFFF) for pc in
               (0x80291348, 0x80291368, 0x8029136C, 0x80291370, 0x80291374, 0x80291378, 0x8029137C)}
    expected = {
        'src/hle/gx/gx_internal.h': {gxptr, fifo, fifo + offsets[0x80291378], fifo + offsets[0x80291374]},
        'src/hle/gx/gx_init.cpp': {gxdata, fifo + offsets[0x80291348], fifo + offsets[0x80291368],
                                   fifo + offsets[0x8029136C], fifo + offsets[0x80291370],
                                   fifo + offsets[0x80291374], fifo + offsets[0x80291378], fifo + offsets[0x8029137C]},
    }
    horver = lis_addi(0x80281450, 0x80281458)
    expected['src/hle/vi.cpp'] = {horver + signed16(word(pc) & 0xFFFF) for pc in
                                 (0x80281478, 0x80280BE4, 0x8028147C, 0x8028148C, 0x80281ED0)}
    expected['src/hle/os/os_thread.cpp'] = {
        lis_addi(0x80274F64) + signed16(word(0x80274F6C) & 0xFFFF)}
    queue_init = word(0x8028025C)  # VIInit: OSInitThreadQueue(&retraceQueue).
    assert queue_init >> 26 == 14 and (queue_init >> 16) & 31 == 13
    retrace_queue = 0x80574F20 + signed16(queue_init & 0xFFFF)
    expected['src/fiber_manager.cpp'] = {retrace_queue, retrace_queue + 4}
    expected['src/hle/audio/ax_internal.h'] = {
        0x80574F20 + signed16(word(pc) & 0xFFFF) for pc in (0x80288F94, 0x80288F90, 0x802890B8)}
    expected['src/hle/audio/ax_internal.h'].add(lis_addi(0x80286714, 0x80286734))
    expected['src/hle/audio/ax_effects.cpp'] = {0x80576280 + signed16(word(0x80288250) & 0xFFFF)}
    expected['src/hle/storage/dvd.cpp'] = {
        0x80574F20 + signed16(word(pc) & 0xFFFF) for pc in (0x80279A80, 0x80279E50, 0x80279E44)}
    checks = []
    for file, addresses in expected.items():
        source = '\n'.join(line for _, line in active_lines((wc / 'runtime' / file).read_text()))
        actual = {int(m.group(1), 16) for m in ADDRESS.finditer(source)}
        for addr in sorted(addresses):
            checks.append(dict(file=file, address=f'{addr:08X}', passed=addr in actual,
                               evidence='Decoded SPM SDK instruction anchors; see sdk_evidence() for PCs'))
    return checks


ADDRESS = re.compile(r"(?<![\w])0x([89CD][0-9a-fA-F]{7})[uUlL]*\b", re.I)
HOOK = re.compile(r"\b(?:PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?|"
                  r"REGISTER_TRANSLATED_FUNCTION|MKW_KNOWN_TYPED_NATIVE|MKW_KNOWN_NATIVE_CPU_CALL)"
                  r"\s*\(\s*(?:0x)?([0-9a-fA-F]{8})\b")


def self_test():
    sample = '// 0x80400000\n#if 0\n#if X\n0x80400001\n#endif\n#else\n0x80500000\n#endif\n"0x80600000"\n'
    hits = [(n, m.group(1)) for n, line in active_lines(sample) for m in ADDRESS.finditer(line)]
    assert hits == [(7, '80500000')], hits
    assert not refs([0x3C608052, 0x7C6300D0, 0x80631520], 0)  # neg clobbers the pointer
    assert (0x80521520, 4) in refs([0x3C608052, 0x38631520], 0)
    assert (0x80574F24, 0) in refs([0x806D0004], 0)
    assert not refs([0x3C608052, 0x48000004, 0x80631520], 0)[-1:] == [(0x80521520, 8)]
    print('scanner self-checks passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--out', type=Path)
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    root = args.root.resolve()
    wc = root / 'wiicompiled'
    out = args.out or root / 'autotest/address-audit'
    out.mkdir(parents=True, exist_ok=True)
    dol = Dol(root / 'game/main.dol')
    names = load_names(wc / 'projects/spm/MAP.txt')
    proofs = sdk_evidence(dol, wc)
    verified = {(r['file'], int(r['address'], 16)): r['evidence'] for r in proofs if r['passed']}
    (out / 'sdk-verification.json').write_text(json.dumps(proofs, indent=2), encoding='utf-8')
    # The NSMBW map only annotates leftover NSMBW function addresses; it is not kept in this fork.
    nsmbw_map = wc / 'projects/nsmbw/MAP.txt'
    nsmbw = load_names(nsmbw_map) if nsmbw_map.exists() else {}
    mapped = {int(k, 16): (int(v[0], 16), v[1], v[2]) for k, v in
              json.loads((wc / 'projects/spm/datamap.json').read_text()).items()}
    inherited = collections.defaultdict(list)
    with (wc / 'projects/spm/data_actions.csv').open(newline='') as fh:
        for row in csv.DictReader(fh):
            if row['spm']:
                inherited[(row['file'], int(row['spm'], 16))].append(row)
    xrefs = collections.defaultdict(list)
    for start, size, off, code in dol.sections:
        if code:
            for address, pc in refs(struct.unpack(f'>{size // 4}I', dol.data[off:off+size]), start):
                xrefs[address].append(pc)
    rows = []
    paths = sorted(p for folder in ('src', 'include') for p in (wc / 'runtime' / folder).rglob('*'))
    for path in paths:
        if path.suffix not in ('.cpp', '.c', '.h', '.hpp', '.inc') or 'third_party' in path.parts:
            continue
        file = path.relative_to(wc / 'runtime').as_posix()
        lines = list(active_lines(path.read_text(encoding='utf-8', errors='replace')))
        for number, line in lines:
            hits = {int(m.group(1), 16): False for m in ADDRESS.finditer(line)}
            hits.update({int(m.group(1), 16): True for m in HOOK.finditer(line)})
            for address, hook in hits.items():
                region = dol.region(address)
                status, evidence = 'unverified', ''
                if address < 0x80004000 or address >= 0xC0000000 or address in (0x81800000, 0x90000000, 0x90000800, 0x94000000):
                    status = 'platform-address'
                elif hook:
                    status = 'spm-function' if address in names and region == 'text' else 'review-hook'
                    evidence = names.get(address, '')
                elif address in xrefs:
                    status = 'spm-reference'
                    evidence = 'PPC references at ' + ' '.join(f'{pc:08X}' for pc in xrefs[address][:5])
                elif address in names and region == 'text':
                    status, evidence = 'spm-function-literal', names[address]
                old = inherited.get((file, address), [])
                uncertain = [r for r in old if r['how'] != 'exact' and r['how'] != 'none']
                for r in old:
                    previous = int(r['nsmbw'], 16)
                    match = mapped.get(previous)
                    if match and match[1] != match[2]:
                        status = 'review-conflicting-remap'
                        evidence += f'; old {previous:08X} had only {match[1]}/{match[2]} agreeing matches'
                if uncertain and status not in ('review-conflicting-remap',):
                    status = 'review-inferred-remap'
                    evidence += '; ' + ', '.join(sorted({r['how'] for r in uncertain}))
                if address in mapped and mapped[address][0] != address and not old and not hook:
                    status = 'review-old-address'
                    evidence += f'; NSMBW candidate maps to {mapped[address][0]:08X}'
                if address in nsmbw and address not in names and not hook:
                    evidence += '; NSMBW function ' + nsmbw[address]
                if (file, address) in verified:
                    status, evidence = 'spm-sdk-evidence', verified[(file, address)]
                rows.append(dict(file=file, line=number, address=f'{address:08X}', status=status,
                                 region=region, evidence=evidence.strip('; '), code=line.strip()[:220]))
    with (out / 'addresses.csv').open('w', newline='', encoding='utf-8') as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    counts = collections.Counter(r['status'] for r in rows)
    summary = dict(sites=len(rows), distinct_addresses=len({r['address'] for r in rows}),
                   status=dict(counts), sdk_checks_passed=sum(r['passed'] for r in proofs),
                   sdk_checks_failed=sum(not r['passed'] for r in proofs),
                   maps=len(list((root / 'Disc/DATA/files/map').glob('*.bin'))))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    report = ['# SPM runtime address audit', '', f'{len(rows)} address sites; {summary["distinct_addresses"]} distinct addresses.',
              '', '## Classification', '', *[f'- {key}: {value}' for key, value in sorted(counts.items())], '',
              'This inventories source candidates, including unresolved platform/config branches. Literal #if 0 blocks, comments, strings and third-party code are excluded.',
              'A PPC reference corroborates an address, not the C++ layout, ownership, callback ABI or reachability. Inferred remaps need verification before changes.', '',
              f'Specific GX/VI/OS/AX/DVD globals: {summary["sdk_checks_passed"]} checks passed; {summary["sdk_checks_failed"]} failed. See sdk-verification.json and sdk_evidence() for instruction evidence.', '',
              'NSMBW menu discovery is gated off for SPM. MKW dynamic-aspect and sound-manager memory paths were already disabled. Their leftover helper constants still appear in the inventory.', '',
              '## Review candidates', '', '| File | Line | Address | Status | Evidence |', '|---|---:|---|---|---|']
    report += [f'| {r["file"]} | {r["line"]} | {r["address"]} | {r["status"]} | {r["evidence"].replace("|", "/")} |'
               for r in rows if r['status'].startswith('review')]
    (out / 'REPORT.md').write_text('\n'.join(report) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    print(out / 'REPORT.md')
    if counts['review-hook'] or summary['sdk_checks_failed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
