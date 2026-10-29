# Provenance of vendored binary files

This fork carries a few binary files that are not built from source in this repository. This page says
where each one came from, what is inside it, and how sure we are, so the question "is anything in here
derived from Nintendo's binaries?" has a written answer.

Everything listed here arrived in a single upstream commit, `b873f0b` ("New Super Paper Bros.: native
PC port of Super Paper Mario", JosiahG, 2026-10-17), which squashes earlier history. So git cannot tell
us more than that commit and the notes upstream wrote alongside it. Where a claim below rests on
upstream's word rather than a check we ran, it says so.

None of these files is new in this fork. They are all in the public upstream repository
(josiahg2502-glitch/NewSuperPaperBros) with the same bytes.

## Summary

| File | Origin | Contains Nintendo material? | Checked how |
| --- | --- | --- | --- |
| `wiicompiled/runtime/assets/dsp/dsp_coef.bin` | Dolphin `Data/Sys/GC/dsp_coef.bin`, GPL-2.0-or-later | No | Byte-identical to Dolphin master |
| `wiicompiled/runtime/assets/wii/shared2/wc24/**` (12 files) | Dolphin `Data/Sys/Wii/shared2/wc24`, GPL-2.0-or-later | No code or assets; see notes | All 12 byte-identical to Dolphin master |
| `wiicompiled/runtime/assets/certs/cacert.pem` | curl's extract of Mozilla's root store, MPL-2.0 | No | File header |
| `wiicompiled/translator/tests/.../payload.RMCPD00.bin` | RetroWFC payload for Mario Kart Wii, test fixture | Not Nintendo's, but MKW-specific; license unrecorded | Test source; not checked further |

Checks were run on 2026-10-24 against `dolphin-emu/dolphin` master.

## DSP coefficients: `dsp_coef.bin`

- 4096 bytes, SHA-256 `D7741279C2E8EC5C5FB318F8FBDD6DE6BF583520D288E836A5383233A4238179`.
- The coefficient table for the DSP's polyphase resampler. The real one lives in the Wii's DSP ROM. This is
  Dolphin's free replacement, which the Dolphin team generated themselves so that nobody needs a dump of
  the real ROM. That is why it is safe to ship and the real ROM is not.
- Byte-identical to Dolphin's `Data/Sys/GC/dsp_coef.bin`. The runtime also checks this hash at build time.
- License: GPL-2.0-or-later, which is compatible with this repository's GPLv3. Credited in
  `wiicompiled/THIRD-PARTY-NOTICES.md` and the root README.

## WiiConnect24 NAND seed: `shared2/wc24/**`

- 12 files: `misc.bin`, `nwc24dl.bin`, `nwc24fl.bin`, `nwc24fls.bin`, `nwc24msg.cbk`, `nwc24msg.cfg`, and the
  `mbox/` folder (`Readme.txt`, `wc24recv.ctl/.mbx`, `wc24send.ctl/.mbx`).
- Used to create a blank per-user Wii NAND on first run, because the game's SDK calls expect these files to
  exist even though WiiConnect24 has been offline for over a decade.
- All 12 are byte-identical to Dolphin's `Data/Sys/Wii/shared2/wc24` (hashes listed in
  `wiicompiled/THIRD-PARTY-NOTICES.md`). The mailboxes are shrunk placeholders (see `mbox/Readme.txt`).
- What is inside: Dolphin's default task list (`nwc24dl.bin`, which names Nintendo's old, dead WC24 server
  URLs) and placeholder account fields (`nwc24msg.cfg`), not anyone's real console data. A URL string is a
  fact about a service, not Nintendo code or art.
- License: GPL-2.0-or-later, as Dolphin ships it.

## Pipeline cache seed: removed

- The repository used to carry `wiicompiled/runtime/assets/pipeline/initial_pipeline_cache.db`
  (4.8 MB, 1156 rows of GX render-state descriptions recorded by Aurora while a game was being
  played). Which game it was recorded from was never certain, which made it the
  weakest-provenance file on this list.
- It has been removed, and nothing replaces it: the package ships no
  pipeline cache seed. Aurora logs that no bundled cache was found, builds pipelines as they
  come up (some stutter the first time an effect appears), and keeps the cache it builds on
  the SD card for later runs.
- A future release may add a seed again, re-recorded from Super Paper Mario itself with
  `wiicompiled/projects/nsmbw/tools/make_pipeline_seed.py`. Aurora picks the file up
  automatically under the same name.

## CA bundle: `cacert.pem`

- Mozilla's root certificate store as extracted by curl (header: "Certificate data from Mozilla as of: Thu
  Aug 13 03:12:01 2026 GMT", from https://curl.se/docs/caextract.html). Used for the runtime's HTTPS.
- License: MPL-2.0. Not related to Nintendo. It is listed in `wiicompiled/THIRD-PARTY-NOTICES.md`.

## Mario Kart Wii test payload: `payload.RMCPD00.bin`

- 29 KB fixture for `RetroWfcPayloadLoweringTests.cs` in the translator tests. It is a payload for the
  RetroWFC custom online service for Mario Kart Wii (game id RMCPD00), which is fan-made, not Nintendo's.
  Its license is not recorded anywhere in the repo.
- Nothing in the SPM build uses it. It is in the same bucket as the Mario Kart Wii map that was removed:
  leftover from WiiCompiled's MKW origins. Not deleted here because the translator test suite needs it.

## Text files that are derived from the game binaries

These are not binary blobs, but the audit's "derived from binaries" question covers them too:

- `wiicompiled/projects/spm/MAP.txt`, `MAP_rel.txt`, `pal/MAP.txt`, `us0_names.tsv`, `pal/spm.*.lst`,
  `bindiff*.json`, `datamap.json`: function start addresses and symbol names for Super Paper Mario's
  `main.dol` and REL. Names come from the spm-decomp community project (credited in the README). The
  translator needs these to know where functions begin, so they stay. They hold addresses and names, not
  code or bytes from the game.
- The Mario Kart Wii and NSMBW maps (`projects/mkwii/MAP.txt`, `projects/nsmbw/MAP.txt`) were removed in
  this fork because the SPM build does not need them.

## Not covered here

- `screenshots/` holds captures of the game running (Nintendo artwork on screen). The upstream release-note
  folders `v1.0.2/` and `v1.0.3/` were removed because nothing referenced them. The 5 README shots stay until
  Switch captures replace them. They remain in git history, as they do in upstream's public repo.
  `wiicompiled/docs/screenshots/` (WiiCompiled's own README images) is untouched.
- Game assets (`main.dol`, the disc) are never in the repository. The player supplies their own disc.
