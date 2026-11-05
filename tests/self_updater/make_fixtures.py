#!/usr/bin/env python3
"""Generate the fixture zips + manifest.txt for the self-updater host fixtures.

Usage: make_fixtures.py <workdir>
Writes <workdir>/*.zip and <workdir>/manifest.txt with one
"<zip name> <size> <sha256 hex>" line per archive. Fixture-local tooling only.
"""
import hashlib
import os
import sys
import zipfile


def nro(marker: bytes) -> bytes:
    b = bytearray(70000)
    for i in range(len(b)):
        b[i] = i % 251
    b[0x10:0x14] = b"NRO0"
    b[0x20:0x24] = marker
    return bytes(b)


ZIPS = {
    # Case c/g: a well-formed SPM release zip. UserData/ and game/ entries must
    # be Deny-skipped; staged set is exactly the NRO, cacert.pem and LICENSE.
    "spm_good.zip": [
        ("switch/spm-nx/", ""),
        ("switch/spm-nx/spm-nx.nro", nro(b"NEW!")),
        ("switch/spm-nx/cacert.pem", "CA-DATA"),
        ("switch/spm-nx/LICENSE", "license text"),
        ("switch/spm-nx/UserData/", ""),
        ("switch/spm-nx/UserData/save.dat", "save"),
        ("switch/spm-nx/UserData/button_icons/a.png", "png"),
        ("switch/spm-nx/game/data.bin", "game"),
    ],
    # Case d: one traversal entry among valid ones -> whole update aborts (Policy).
    "traversal.zip": [
        ("switch/spm-nx/spm-nx.nro", nro(b"NEW!")),
        ("switch/spm-nx/../evil.txt", "evil"),
        ("switch/spm-nx/cacert.pem", "CA-DATA"),
    ],
    # Case d: an entry outside the zip prefix -> whole update aborts (Layout).
    "offprefix.zip": [
        ("switch/other/file.txt", "x"),
        ("switch/spm-nx/spm-nx.nro", nro(b"NEW!")),
    ],
    # Case e: JSRF table material (options.txt Deny, game/ CreateOnly,
    # random.txt unlisted under a Deny default).
    "jsrf.zip": [
        ("switch/jsrf-unleashed/jsrf-unleashed.nro", nro(b"NEW!")),
        ("switch/jsrf-unleashed/READ-ME-FIRST.txt", "readme"),
        ("switch/jsrf-unleashed/options.txt", "fresh defaults"),
        ("switch/jsrf-unleashed/game/existing.bin", "ZIP-EXISTING"),
        ("switch/jsrf-unleashed/game/new.bin", "ZIP-NEW"),
        ("switch/jsrf-unleashed/random.txt", "random"),
    ],
    # Case f: apply-order material. Sorted staged order (NRO aside) is
    # aaa_asset.txt then zzz_asset.txt; the NRO must be applied last.
    "apply.zip": [
        ("switch/spm-nx/spm-nx.nro", nro(b"NEW!")),
        ("switch/spm-nx/aaa_asset.txt", "NEW-aaa"),
        ("switch/spm-nx/zzz_asset.txt", "NEW-zzz"),
    ],
}


def main() -> None:
    work = sys.argv[1]
    os.makedirs(work, exist_ok=True)
    lines = []
    for name, entries in ZIPS.items():
        path = os.path.join(work, name)
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for arcname, data in entries:
                if isinstance(data, str):
                    data = data.encode()
                z.writestr(arcname, data)
        blob = open(path, "rb").read()
        lines.append(f"{name} {len(blob)} {hashlib.sha256(blob).hexdigest()}")
    with open(os.path.join(work, "manifest.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
