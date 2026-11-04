#!/usr/bin/env python3
# Function-level PGO fit check for build-switch.sh's pgo-use variant (SpmSwitch.cmake
# SPM_PGO_USE). CMake checks the profile's provenance (manifest: region, scope, compiler,
# recorded at all); this script checks the fit itself after the build.
#
# Why it exists: clang applies an instrumented profile per function, keyed by a hash of the
# function's control flow. A function whose code changed since training only warns
# ("profile data may be out of date") and compiles unprofiled, so a build against a stale
# profile succeeds quietly and keeps whatever fraction still happens to match: sa2-nx lost
# 29.7% of its release profile's match after one round of code changes. This check counts
# those warnings against the profile's function total (from llvm-profdata show) and fails
# the build when the stale share passes the limit, so a profile that no longer fits is a
# loud stop, never a silent partial.
#
# Usage: pgo-fit-check.py --log BUILD.log --show PROFDATA-SHOW.txt [--max-stale-pct 25]
# Exit: 0 = fit, 1 = unfit (nothing matched, or stale share over the limit),
#       2 = the check itself could not run (unparseable inputs: also a failed build,
#           because an unverifiable profile is not a verified one).
import argparse
import re
import sys


def total_functions(show_text):
    """The profile's function count, from llvm-profdata show's summary block."""
    m = re.search(r"^Total functions:\s*(\d+)\s*$", show_text, re.MULTILINE)
    return int(m.group(1)) if m else None


def _bodies(log_text, needles):
    """Distinct warning bodies (location and 'warning:' prefix stripped) whose line
    mentions a profile warning: one entry per distinct message, so the same function
    warned about from several translation units counts once."""
    out = set()
    for line in log_text.splitlines():
        low = line.lower()
        if "profile" not in low:
            continue
        if not any(n in low for n in needles):
            continue
        body = re.sub(r"^.*?warning:\s*", "", line)
        body = re.sub(r"\s*\[-W[^\]]*\]\s*$", "", body).strip()
        out.add(body)
    return out


def stale_warnings(log_text):
    # clang's -Wprofile-instr-out-of-date: "profile data may be out of date: ...".
    return _bodies(log_text, ("out of date", "out-of-date"))


def unprofiled_warnings(log_text):
    # clang's -Wprofile-instr-unprofiled: functions compiled with the use flag that the
    # profile has no data for. Informational only: they are not in the profile's total,
    # so they say nothing about how much of the profile still fits.
    return _bodies(log_text, ("unprofiled", "no profile data available"))


def main():
    ap = argparse.ArgumentParser(description="PGO profile fit check (see header).")
    ap.add_argument("--log", required=True, help="the pgo-use build's full output log")
    ap.add_argument("--show", required=True, help="llvm-profdata show output for the merged profile")
    ap.add_argument("--max-stale-pct", type=float, default=25.0,
                    help="fail when more than this percent of the profile is out of date")
    args = ap.parse_args()

    try:
        with open(args.show, errors="replace") as f:
            show_text = f.read()
        with open(args.log, errors="replace") as f:
            log_text = f.read()
    except OSError as e:
        print(f"PGO FIT CHECK COULD NOT RUN: {e}", file=sys.stderr)
        return 2

    total = total_functions(show_text)
    if not total:
        print("PGO FIT CHECK COULD NOT RUN: no 'Total functions:' line in the "
              "llvm-profdata show output; the profile's size is unknown, so its fit "
              "cannot be verified. Refusing to call this build profiled.", file=sys.stderr)
        return 2

    stale = stale_warnings(log_text)
    unprofiled = unprofiled_warnings(log_text)
    matched = total - len(stale)
    stale_pct = 100.0 * len(stale) / total

    print(f"[pgo-fit] profile: {total} functions; out of date in this build: "
          f"{len(stale)} ({stale_pct:.1f}%, limit {args.max_stale_pct:g}%); "
          f"matched: {matched}; compiled without profile data: {len(unprofiled)} "
          f"(informational)")

    if matched <= 0:
        print("[pgo-fit] FAIL: not one profiled function matched this tree. The profile "
              "belongs to other code (wrong tree, wrong region, or a full codegen change) "
              "and this NRO is not a PGO build in any honest sense. Re-train: "
              "build-switch.sh --variant pgo, a training session, then --variant pgo-use.",
              file=sys.stderr)
        return 1
    if stale_pct > args.max_stale_pct:
        print(f"[pgo-fit] FAIL: {stale_pct:.1f}% of the profile is out of date, over the "
              f"{args.max_stale_pct:g}% limit: the code moved too far since training for "
              "this profile to be called a fit. Re-train (build-switch.sh --variant pgo, "
              "a training session, then --variant pgo-use), or raise the limit knowingly "
              "with SPM_PGO_FIT_MAX_STALE_PCT.", file=sys.stderr)
        return 1
    print(f"[pgo-fit] OK: {matched} of {total} profiled functions fit this tree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
