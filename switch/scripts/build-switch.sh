#!/usr/bin/env bash
# Switch build of New Super Paper Bros. Run on Linux or in WSL with docker or podman, after
# tools\build-spm.ps1 has translated the game on Windows: like tools/build-linux.sh, this reuses that
# output (wiicompiled\generated) and only compiles it, here for Horizon inside switch/scripts/Containerfile.
#   bash switch/scripts/build-switch.sh [--jobs N] [--audit] [--out DIR|none] [--variant NAME]
# --variant NAME builds into build-NAME with that variant's CMake options and stages the NRO as
# spm-nx-NAME.nro: hotcold (SPM_HOTCOLD), o3 (SPM_O3_SHARDS), hotcold-o3 (both), pgo
# (SPM_PGO_INSTRUMENT + SPM_PGO_ALL, the PGO capture build: never quote its frame rates; a good
# capture build also records the training manifest into pgo/ at the repo root), pgo-use
# (SPM_PGO_USE: merges the .profraw files in pgo/ with llvm-profdata, builds with the profile,
# then enforces the function-level fit check in pgo-fit-check.py before staging anything).
# --audit compiles only the runtime's sources for the Switch (no game, no Dawn, no network beyond the
# SDL/ImGui header downloads): the quick check after an upstream merge.
# Sources are copied to ~/spm-switch first. Output: ~/spm-switch/build/spm-nx.nro
# After a good build the NRO and its companion files are also copied, laid out like the SD card, to
# <DIR>/switch/spm-nx/; DIR defaults to Downloads\spm-nx-sd in your Windows profile under WSL.
# Copy the NRO onto the card from there (plus UserData/button_icons when the icons changed),
# merging: do not replace the whole spm-nx folder, because the card's UserData also holds
# saves, settings and caches that the staging folder does not have. --out none skips the copy.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
jobs=6   # translated shards need ~1 GB each
audit=0
out=auto
variant=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --audit) audit=1 ;;
        --out) out=$2; shift ;;
        --variant) variant=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }
image=spm-nx-build
"$engine" build -t "$image" -f "$root/switch/scripts/Containerfile" "$root/switch/scripts"

# The clang major the build image carries (19 today). Raw PGO profiles are locked to it, so
# the capture build records it in the training manifest and pgo-use refuses another major.
pgo_clang_major() {
    "$engine" run --rm "$image" clang-19 --version 2>/dev/null | head -n 1 |
        sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p'
}

work=$HOME/spm-switch
mkdir -p "$work/wiicompiled"
echo "copying sources to $work"
rsync -a --delete "$root/switch/" "$work/switch/"
rsync -a --delete "$root/wiicompiled/runtime/" "$work/wiicompiled/runtime/"
rsync -a --delete "$root/wiicompiled/aurora-main/" "$work/wiicompiled/aurora-main/"
# save_state.cpp includes ../../projects/spm/resumable_functions.inc by relative
# path, so the projects tree has to sit next to runtime/ in the build tree too.
rsync -a --delete "$root/wiicompiled/projects/" "$work/wiicompiled/projects/"
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
    # Give the copied animPoseDrawMtx a hook into the runtime (spm_suppress_anim_pose in
    # spm_map_sweep.cpp). Log-only in this form: with poselog=1 in dk_diag.txt the hook
    # names the poses drawn from the n_controler archives; it never skips a draw.
    python3 "$root/switch/scripts/patch-anim-draw.py" "$work/wiicompiled/generated"
    # Give the copied evt NPC handlers their missing-NPC guard (spm_npc_guard_miss
    # in spm_map_sweep.cpp): a script that positions an NPC before he exists
    # otherwise trips the retail npcNameToPtr assert (npcdrv.c:7055) and kills
    # the session. See patch-npc-guard.py for the mechanism and handler list.
    python3 "$root/switch/scripts/patch-npc-guard.py" "$work/wiicompiled/generated"
    # Give the copied hitGetMapEntryBbox its aggregate-result cache hooks
    # (BBoxCacheTryServe / BBoxCacheShadowNote / BBoxCacheCapture /
    # BBoxCacheShadowCompare in the runtime's bbox_cache.cpp) plus the five
    # writer-generation bumps in the hit-family bodies: an entry block after
    # the body's argument captures and a capture/compare block at the head of
    # its common return block. Both diag switches default off, so an
    # unflagged boot runs the bodies exactly as unpatched. See
    # patch-bboxcache.py for the anchors and the design.
    python3 "$root/switch/scripts/patch-bboxcache.py" "$work/wiicompiled/generated"
fi

# Separate build folders: CMake caches SPM_SWITCH_AUDIT, so a full build in the audit's folder would
# still skip Dawn and Aurora.
products=ON; target=WiiCompiled_nro; build=build; extra=(-DSPM_SWITCH_AUDIT=OFF)
if [[ $audit == 1 ]]; then
    products=OFF; target=mkw_switch_audit; build=build-audit; extra=(-DSPM_SWITCH_AUDIT=ON)
fi
# A variant gets its own build folder (CMake caches options per folder, so sharing one would
# silently keep the first variant's flags) and its own staged NRO name, so the A/B set can sit
# on the card side by side (test build spec D0).
nro_name=spm-nx.nro
if [[ -n $variant ]]; then
    build=build-$variant
    nro_name=spm-nx-$variant.nro
    case "$variant" in
        hotcold) extra+=(-DSPM_HOTCOLD=ON) ;;
        o3) extra+=(-DSPM_O3_SHARDS=ON) ;;
        hotcold-o3) extra+=(-DSPM_HOTCOLD=ON -DSPM_O3_SHARDS=ON) ;;
        pgo) extra+=(-DSPM_PGO_INSTRUMENT=ON -DSPM_PGO_ALL=ON) ;;
        pgo-use) extra+=(-DSPM_PGO_USE=ON -DSPM_PGO_PROFILE=/work/pgo/spm.profdata) ;;
        pgo-use-fcvt) extra+=(-DSPM_PGO_USE=ON -DSPM_PGO_PROFILE=/work/pgo/spm.profdata -DSPM_FCVT_FZ=ON) ;;
        *) echo "unknown variant $variant (hotcold|o3|hotcold-o3|pgo|pgo-use|pgo-use-fcvt)" >&2; exit 1 ;;
    esac
fi

if [[ $variant == pgo-use* ]]; then
    # Provenance first, while failing is still cheap. The manifest the capture build wrote
    # sits in $root/pgo beside where the session's raw profiles were copied; the hard stops
    # here (no manifest, no profiles, wrong region, wrong clang major) are the ones no fit
    # percentage could rescue. Commit drift and a moved shard list are loud warnings only:
    # profiles are iterative, and the function-level fit check after the build arbitrates.
    pgo_dir=$root/pgo
    [[ -f $pgo_dir/manifest.txt ]] || { echo "pgo-use: no $pgo_dir/manifest.txt: run a --variant pgo capture build first (it records the manifest), then a training session" >&2; exit 1; }
    shopt -s nullglob
    pgo_raws=("$pgo_dir"/*.profraw)
    (( ${#pgo_raws[@]} > 0 )) || { echo "pgo-use: no .profraw files in $pgo_dir: copy the last spm-*.profraw of each training session from the SD card's UserData/pgo/ into $pgo_dir first" >&2; exit 1; }
    shopt -u nullglob
    pgo_mf() { sed -n "s/^$1=//p" "$pgo_dir/manifest.txt" | head -n 1; }
    pgo_region=$(pgo_mf region); pgo_commit=$(pgo_mf commit); pgo_clang=$(pgo_mf clang_major)
    pgo_dirty=$(pgo_mf tree_dirty); pgo_shards=$(pgo_mf shards_fingerprint)
    image_clang=$(pgo_clang_major)
    [[ -n $image_clang ]] || { echo "pgo-use: could not read the build image's clang version" >&2; exit 1; }
    [[ $pgo_region == R8PE01-USA ]] || { echo "pgo-use: the profile manifest says region '$pgo_region'; the Switch build is the USA game (R8PE01) only and generated code differs per region. Train a USA profile." >&2; exit 1; }
    [[ $pgo_clang == "$image_clang" ]] || { echo "pgo-use: the profile was recorded with clang major '$pgo_clang' but the build image has clang $image_clang: raw profile formats are version-locked. Re-train." >&2; exit 1; }
    head_commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
    if [[ $pgo_commit != "$head_commit" ]]; then
        echo "============================================================" >&2
        echo "PGO WARNING: the profile was trained on tree $pgo_commit" >&2
        echo "but this tree is $head_commit. That is allowed (profiles are" >&2
        echo "iterative); the fit check after the build is the arbiter, and" >&2
        echo "too much drift fails the build instead of shipping quietly." >&2
        echo "============================================================" >&2
    fi
    [[ $pgo_dirty == 1 ]] && echo "PGO WARNING: the capture build ran on a dirty tree; its recorded commit does not fully describe the trained code." >&2
    if [[ -n $pgo_shards && -f $root/wiicompiled/generated/build_shards/shards.cmake ]]; then
        now_shards=$(sha256sum "$root/wiicompiled/generated/build_shards/shards.cmake" | cut -d' ' -f1)
        [[ $now_shards == "$pgo_shards" ]] || echo "PGO WARNING: the translated game's shard list changed since training (the generated code moved); the fit check will judge how much of the profile still lands." >&2
    fi
    rsync -a --delete "$pgo_dir/" "$work/pgo/"
    echo "pgo-use: merging ${#pgo_raws[@]} raw profile(s) with llvm-profdata"
    "$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
        set -e
        profdata=\$(command -v llvm-profdata-19 || command -v llvm-profdata)
        \$profdata merge -o /work/pgo/spm.profdata /work/pgo/*.profraw
        \$profdata show /work/pgo/spm.profdata > /work/pgo/profdata-show.txt
    "
fi

if [[ $variant == pgo-use* ]]; then
    # The build's output is kept whole: the fit check below reads clang's profile warnings
    # out of it. pipefail (set above) keeps a failed build a failed script through the tee.
    "$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
        set -e
        cmake -S wiicompiled/runtime -B $build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
            -DMKW_BUILD_PRODUCTS=$products -DMKW_TRANSLATED_COMPILE_JOBS=$jobs ${extra[*]}
        cmake --build $build --target $target --parallel $jobs -- -k 0
    " 2>&1 | tee "$work/pgo-use-build.log"
else
    "$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
        set -e
        cmake -S wiicompiled/runtime -B $build -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
            -DMKW_BUILD_PRODUCTS=$products -DMKW_TRANSLATED_COMPILE_JOBS=$jobs ${extra[*]}
        cmake --build $build --target $target --parallel $jobs -- -k 0
    "
fi

if [[ $variant == pgo-use* ]]; then
    if ! python3 "$root/switch/scripts/pgo-fit-check.py" \
            --log "$work/pgo-use-build.log" --show "$work/pgo/profdata-show.txt" \
            --max-stale-pct "${SPM_PGO_FIT_MAX_STALE_PCT:-25}"; then
        echo "============================================================" >&2
        echo "PGO FIT CHECK FAILED: the NRO above is NOT staged. It compiled," >&2
        echo "but the profile does not fit this tree well enough to carry" >&2
        echo "the PGO name. Re-train (--variant pgo, a training session," >&2
        echo "--variant pgo-use), or raise SPM_PGO_FIT_MAX_STALE_PCT knowingly." >&2
        echo "============================================================" >&2
        exit 1
    fi
fi

if [[ $variant == pgo && $audit == 0 ]]; then
    # Record the training manifest beside where the raw profiles will land. It is the
    # profile's provenance (WakerHD's shape: region, commit, compiler, scope, fingerprint
    # of the generated code): pgo-use checks it before trusting any profile, and CMake
    # checks it again beside the merged profile.
    mkdir -p "$root/pgo"
    pgo_clang_written=$(pgo_clang_major)
    [[ -n $pgo_clang_written ]] || { echo "pgo: could not read the build image's clang version for the manifest" >&2; exit 1; }
    pgo_dirty_written=0
    [[ -n $(git -C "$root" status --porcelain 2>/dev/null) ]] && pgo_dirty_written=1
    {
        echo "# spm-nx PGO profile manifest. Written by build-switch.sh --variant pgo;"
        echo "# checked by SpmSwitch.cmake (SPM_PGO_USE) and the pgo-use variant."
        echo "region=R8PE01-USA"
        echo "commit=$(git -C "$root" rev-parse HEAD)"
        echo "tree_dirty=$pgo_dirty_written"
        echo "clang_major=$pgo_clang_written"
        echo "scope=game+runtime+aurora"
        if [[ -f $root/wiicompiled/generated/build_shards/shards.cmake ]]; then
            echo "shards_fingerprint=$(sha256sum "$root/wiicompiled/generated/build_shards/shards.cmake" | cut -d' ' -f1)"
        fi
        echo "trained_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$root/pgo/manifest.txt"
    echo "pgo: training manifest written to $root/pgo/manifest.txt"
    echo "pgo: after the training session, copy the LAST spm-*.profraw of each session from the SD card's UserData/pgo/ into $root/pgo/, then build --variant pgo-use"
fi
if [[ $audit == 1 ]]; then
    echo "DONE: runtime compiles for Horizon"
else
    echo "DONE: $work/$build/spm-nx.nro (staged as $nro_name)"
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
        cp "$work/$build/spm-nx.nro" "$dest/$nro_name"
        cp "$work/$build/dsp_coef.bin" "$work/$build/cacert.pem" "$dest/"
        cp -r "$work/$build/wii_bootstrap" "$dest/"
        # The license pack travels with the binary: the root LICENSE (GPLv3), the third-party
        # notices, the upstream credits and the Switch layer's own notices and license texts.
        mkdir -p "$dest/licenses"
        cp "$root/LICENSE" "$root/wiicompiled/THIRD-PARTY-NOTICES.md" "$root/tools/CREDITS.txt" \
            "$root/switch/THIRD_PARTY.md" "$dest/licenses/"
        cp "$root"/switch/licenses/*.txt "$dest/licenses/"
        # The button prompt icons: the clean half of the Kenney set, generated by
        # tools/make-button-icons.py --clean from the vendored CC0 SVGs alone (no HD pack).
        # The copy list is explicit on purpose: the generator's full mode also composites
        # glyphs into the HD texture pack's artwork, and those files must never ship, so
        # nothing is copied by wildcard. The names mirror the generator's SIDEWAYS_TEXTURE,
        # REMOTE_TEXTURES, MARKERS and ui action map. The marker files are transparent on
        # purpose: they only report that the game's Controls screen is up, and the
        # replacement Controls panel draws the labels itself. The license travels with the icons. If the host
        # lacks python3, Pillow or ImageMagick the icons are skipped with a warning; the game
        # runs without them (its own prompt art, text labels). The list also carries the
        # prompt texture from the generator's PROMPT_CLEAN_KINDS (the shake icon), the prompt
        # picture it can draw without the pack's artwork.
        icons_gen=$work/button-icons
        icons_dest=$dest/UserData/button_icons
        if python3 -c 'import PIL' >/dev/null 2>&1 && command -v magick >/dev/null &&
           python3 "$root/tools/make-button-icons.py" --clean --out "$icons_gen"; then
            # The folder is all generated content, so rebuild it from scratch: copying
            # over the top let files the staging lists had dropped live on in the
            # staged set.
            rm -rf "$icons_dest"
            mkdir -p "$icons_dest/nintendo/ui" "$icons_dest/nintendo/ui/buttons" "$icons_dest/nintendo/composite" \
                     "$icons_dest/nintendo/composite/letters" "$icons_dest/nintendo/composite/faces" \
                     "$icons_dest/nintendo/marker1" \
                     "$icons_dest/nintendo/marker2" "$icons_dest/nintendo/marker3" \
                     "$icons_dest/nintendo/marker4" "$icons_dest/nintendo/marker5" \
                     "$icons_dest/nintendo/marker6"
            cp "$root/tools/button-art/Kenney-License.txt" "$icons_dest/"
            for f in controller.png panel.txt buttons.txt tex1_464x288_36a6f13147d63352_14.png \
                     tex1_96x40_61b1a30a17d36df5_5.png \
                     ui/move.png ui/flip.png ui/jump.png ui/pixl.png ui/tippi.png ui/stylish.png \
                     ui/buttons/a.png ui/buttons/b.png ui/buttons/x.png ui/buttons/y.png ui/buttons/rightshoulder.png \
                     ui/buttons/righttrigger.png ui/buttons/leftstick.png ui/buttons/rightstick.png \
                     marker1/tex1_256x64_ddc0097f76e8016b_14.png \
                     marker1/tex1_256x64_a2fd9566973811d6_14.png \
                     marker1/tex1_64x32_3b9919c664469aa9_14.png \
                     marker1/tex1_80x44_c113b191224defc0_14.png \
                     marker2/tex1_248x184_fd844c6227d275cf_5.png \
                     marker2/tex1_248x200_55ef64a9d7e806f7_5.png \
                     marker3/tex1_152x144_eba7359cd27cf8b6_5.png \
                     marker4/tex1_176x168_6208c1ec2d692b4f_5.png \
                     marker5/tex1_200x152_8394740b60e9c228_5.png \
                     marker6/tex1_168x136_42b4c0a037f4bbee_5.png; do
                cp "$icons_gen/nintendo/$f" "$icons_dest/nintendo/$f"
            done
            # The runtime compositor's overlays (generator composite/): just the new buttons for
            # the disc, face, glyph and plaindisc prompts, drawn over the game's own textures
            # at runtime, so the pack-free set covers those prompts too. The list mirrors
            # COMPOSITE_KINDS in the generator; the +/- discs are absent on purpose (nintendo
            # keeps the game's own). letters/ and faces/ are the per-button masks the
            # compositor draws a bound button's letter from at runtime (COMPOSITE_TOKENS in
            # the generator), so a prompt follows the player's own bindings.
            for f in composite.txt \
                     tex1_152x144_eba7359cd27cf8b6_5.png tex1_176x168_6208c1ec2d692b4f_5.png \
                     tex1_200x152_8394740b60e9c228_5.png tex1_208x48_3566a7d381dc2f35_5.png \
                     tex1_48x48_42bca1c8ab4b3dfb_5.png tex1_48x48_169f2321518eafa5_5.png \
                     tex1_56x56_4986da0fff2e544e_5.png \
                     tex1_56x56_ff3270a64a8b87b2_5.png tex1_56x72_38dcf921a993679a_5.png \
                     tex1_40x40_21ecf92a70736554_14.png tex1_40x40_db14bc3d6c7b16a5_14.png \
                     tex1_40x40_e73bd2aadadde9d9_14.png \
                     tex1_40x40_c4a6d5884f8e8519_5.png tex1_40x40_341c9fd2189acc61_5.png \
                     tex1_40x40_9385da9c84917672_5.png tex1_40x40_7cdc488c8aac0ffe_5.png \
                     tex1_40x40_6b099107b1c20a06_5.png tex1_40x40_4cd31df78a31c8d4_5.png \
                     letters/a.png letters/b.png letters/x.png letters/y.png \
                     letters/rightshoulder.png letters/righttrigger.png \
                     faces/a.png faces/b.png faces/x.png faces/y.png \
                     faces/rightshoulder.png faces/righttrigger.png; do
                cp "$icons_gen/nintendo/composite/$f" "$icons_dest/nintendo/composite/$f"
            done
            echo "button icons staged to $icons_dest/nintendo"
        else
            echo "WARNING: button icons not staged (generation needs python3 + Pillow + ImageMagick 'magick' on the host)" >&2
        fi
        echo "COPIED to $dest ($(date -r "$dest/spm-nx.nro" '+%Y-%m-%d %H:%M'))"
        echo "copy the NRO onto the SD card, merging (do not replace the spm-nx folder: its UserData holds your saves), then launch with title takeover"
    elif [[ $out != none ]]; then
        echo "copy $work/build/spm-nx.nro to sdmc:/switch/spm-nx/, then launch with title takeover"
    fi
fi
