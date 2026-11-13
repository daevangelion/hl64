#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# GameMon host test: the hldemo1 dome scene (test/m3/mon-dome.route):
# start_script_trigger -> startdometalk_mm / dometalk_mm -> domesci
# scripts (domesci_script_3 'lean' -> domesci_script_4 'studycart') ->
# start_ele_unlock_mm enables multisource start_ele_lock_1. Prints the
# script / sentence times and the prof mon_us roll-up.
#
# Usage: test/m3/mon.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content with GameMon's
#        res.def: make cook ... MAPS='hldemo* t0a0*').
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
OUT="$BUILD_DIR/m3-mon"
mkdir -p "$OUT"
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }
fail() { echo "MON FAIL: $*" >&2; exit 1; }

cd "$ROOT"
make --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/build.log" 2>&1 \
	|| { tail -30 "$OUT/build.log" >&2; fail "build"; }
grep -E 'warning:' "$OUT/build.log" && fail "build has warnings"
python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" test/m3/mon-dome.route \
	-o "$OUT/mon-dome.rbc" >/dev/null || fail "routec"
"$BUILD_DIR/host/hl64-host" --root "$CONTENT" --route "$OUT/mon-dome.rbc" \
	--cmd "@1 fixed_dt 33333" --cmd "@2 prof 1" >"$OUT/dome.log" 2>&1 || fail "host run ($OUT/dome.log)"
L="$OUT/dome.log"
grep -E '^(script|sentence|multisource):' "$L"
grep -q '^route PASS' "$L" || fail "route"
grep -q 'takes lean' "$L" || fail "domesci_script_3 lean not taken"
grep -q 'done lean' "$L" || fail "lean not finished"
grep -q 'plays studycart' "$L" || fail "domesci_script_4 not played"
# start_ele_lock_1: the second multisource enabled on hldemo1 (after tele_init_master)
[[ $(grep -c '^multisource: map 0 lump .* enabled' "$L") -ge 2 ]] || fail "start_ele_lock_1 not enabled"
grep '^prof mon_us' "$L" | tail -3 || echo "mon: no prof mon_us roll-up line in $L"
echo "mon: dome scene PASS"
