#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# The SummerCart64 interface on ares (test/run/ares-build.sh: meeq/ares'
# SC64 emulation + test/ares-sc64.patch): test/sc64/sc64test.z64 runs on an
# SC64 whose SD card is test/sc64/mkimage.py's image, with
# ARES_SC64_SECTOR_US=100 (the ROM's SECTOR_US). The ROM checks KEY unlock,
# IDENTIFIER, CONFIG_GET/SET ROM_WRITE_ENABLE (writes ignored when off),
# SD_CARD_OP init/status, SD_READ into ROM space (busy time, PI DMA back),
# the same with CMD_IRQ_REQUEST, SD_WRITE round trip, an error past the
# card's end, and libdragon's libcart on the card (test/sc64/sc64test.c).
# PASS: `sc64: PASS` and the harness exit 0 in the log, no `sc64: FAIL`
# line, and the card image after the run holds the ROM's SD_WRITE sectors
# with every other byte unchanged (mkimage.py check).
#
# Usage: test/run/sc64.sh
# Env:   BUILD_DIR (scratch, default build), HL64_ARES_HOST / ARES_BIN
#        (test/run/ares.sh), ARES_TIMEOUT (default 120)
# Out:   $BUILD_DIR/sc64/{sc64test.z64,card-in.img,card-out.img,ares.log}
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/sc64"
fail() { echo "SC64 FAIL: $*" >&2; exit 1; }
mkdir -p "$OUT"

make -C "$ROOT/test/sc64" --no-print-directory BUILD_DIR="$OUT/rom" >"$BUILD_DIR/sc64-rom.log" 2>&1 \
	|| { tail -20 "$BUILD_DIR/sc64-rom.log" >&2; echo "sc64 ROM build failed" >&2; exit 2; }
cp "$OUT/rom/sc64test.z64" "$OUT/sc64test.z64"
python3 "$ROOT/test/sc64/mkimage.py" make "$OUT/card-in.img"

# shellcheck source=ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2
CODE=0
ARES_SD_IN=card-in.img ARES_SD_OUT=card-out.img ARES_SC64_SECTOR_US=100 \
	ares_run "$OUT" sc64test.z64 ares.log "${ARES_TIMEOUT:-120}" || CODE=$?
LOG="$OUT/ares.log"
grep -E '^sc64: ' "$LOG" || true
[[ $CODE -eq 0 ]] || fail "ares exit $CODE ($LOG)"
grep -q '^TESTHARNESS exit 0' "$LOG" || fail "no harness exit 0"
! grep -q '^sc64: FAIL' "$LOG" || fail "the ROM's checks failed"
grep -q '^sc64: PASS' "$LOG" || fail "no 'sc64: PASS'"
python3 "$ROOT/test/sc64/mkimage.py" check "$OUT/card-out.img" || fail "the card image after the run"
echo "sc64 PASS (ares on $(ares_host), ${ARES_WALL} s; $OUT)"
