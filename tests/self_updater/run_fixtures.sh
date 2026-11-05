#!/bin/sh
# Host fixture runner for the shared self-updater core (DESIGN §7).
# Builds the REAL core without SELFUPD_TRANSPORT_CURL (the fixture main
# provides the transport seam), against the fixture-local minizip shim
# (the host has zlib but no minizip), generates the fixture zips with
# python3's zipfile, and runs every case against temp dirs.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
WORK=$(mktemp -d /tmp/selfupd-fixtures.XXXXXX)
echo "work dir: $WORK"
python3 make_fixtures.py "$WORK"
g++ -std=c++17 -Wall -Wextra \
    -I"$ROOT/tests/self_updater" -I"$ROOT/switch/source" \
    "$ROOT/switch/source/self_updater.cpp" \
    fixture_main.cpp minizip_shim.cpp \
    -lz -o "$WORK/self_updater_fixtures"
"$WORK/self_updater_fixtures" "$WORK"
