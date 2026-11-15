#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Controller Pak driver checks on the host (plat/pakfs.c over
# plat/host/pak_host.c; test/save/paktest.c), cross-checked with
# libdragon's own Pak tool (cpaktool: an independent implementation of the
# same file system):
#   1. round trip on a fresh image; cpaktool finds no issue, lists our note
#      and extracts its exact bytes
#   2. foreign notes: a cpaktool-made Pak with two other games' notes, and
#      real-game Pak dumps (libdragon's cpaktool test goldens, DexDrive
#      header stripped; one has a broken note): ours created / deleted,
#      every foreign note's entry, chain and pages byte-identical, cpaktool
#      extracts them unchanged and reports no issue it did not before
#   3. a 4-bank Pak does not mount and is not written
#   4. power cut after every block of a create / write / close / delete
#      sequence (on the foreign-note Pak): every cut image mounts, foreign
#      notes untouched; cpaktool's issues on cut images are only the
#      unreferenced chain a cut between FAT and note leaves
#
#   test/save/paktest.sh [OUTDIR]    (BUILD_DIR: the make build directory)
# Exit 0 all good.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${1:-$BUILD_DIR/paktest}"
N64_INST="${N64_INST:-$HOME/sdks/n64-sdk-hl64-$(sed -n 's/^LIBDRAGON_REV := \(.\{12\}\).*/\1/p' "$ROOT/Makefile")}"
CPAK="$N64_INST/bin/cpaktool"
GOLD="${HL64_CPAK_GOLDEN:-$HOME/src/libdragon-hl64/tools/cpaktool/tests/golden}"

make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" paktest >/dev/null
case "$BUILD_DIR" in /*) BIN="$BUILD_DIR/host/hl64-paktest" ;; *) BIN="$ROOT/$BUILD_DIR/host/hl64-paktest" ;; esac
[[ -x "$CPAK" ]] || { echo "paktest: no cpaktool at $CPAK" >&2; exit 2; }
rm -rf "$OUT" && mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cd "$OUT"
fail() { echo "paktest: FAIL $*"; exit 1; }

# 1. fresh image
"$BIN" roundtrip fresh.pak
"$CPAK" test fresh.pak | grep -x 'No issues found' >/dev/null || fail "cpaktool test fresh.pak"
"$CPAK" list fresh.pak | grep -E '^HL64 +HL +HL64SAVE +0 +5000$' >/dev/null || fail "cpaktool list fresh.pak"
mkdir x && (cd x && "$CPAK" extract ../fresh.pak >/dev/null)
"$BIN" put want.pak 0 5000 1 >/dev/null 2>&1
(mkdir y && cd y && "$CPAK" extract ../want.pak >/dev/null)
cmp x/HL64.HL-HL64SAVE.0 y/HL64.HL-HL64SAVE.0 || fail "cpaktool extract of our note"
echo "paktest: cpaktool: fresh image no issues, note HL64.HL-HL64SAVE.0 5000 bytes extracted exact"

# 2. foreign notes
head -c 3000 /dev/urandom >NSME.01-MARIO.A
head -c 700 /dev/urandom >NO7P.69-TWINE.A
"$CPAK" format foreign.pak >/dev/null
"$CPAK" add foreign.pak NSME.01-MARIO.A NO7P.69-TWINE.A >/dev/null
images=(foreign.pak)
for g in forsaken_64.n64 bh-sram.n64; do
	[[ -f "$GOLD/$g" ]] || { echo "paktest: no golden $GOLD/$g (skipped)"; continue; }
	tail -c 32768 "$GOLD/$g" >"${g%.n64}.pak"
	images+=("${g%.n64}.pak")
done
for img in "${images[@]}"; do
	cp "$img" "work-$img"
	"$CPAK" test "$img" >"$img.fsck0" 2>&1 || true
	(mkdir "x-$img" && cd "x-$img" && "$CPAK" extract "../$img" >/dev/null 2>&1 || true)
	"$BIN" put "work-$img" 3 2000 3 2>/dev/null
	"$BIN" put "work-$img" 9 300 9 2>/dev/null
	"$BIN" del "work-$img" 3 2>/dev/null
	"$BIN" get "work-$img" 9 300 9 2>/dev/null
	"$BIN" same "$img" "work-$img"
	"$CPAK" test "work-$img" >"$img.fsck1" 2>&1 || true
	diff "$img.fsck0" "$img.fsck1" >/dev/null || { cat "$img.fsck0" "$img.fsck1"; fail "cpaktool issues changed on $img"; }
	(mkdir "y-$img" && cd "y-$img" && "$CPAK" extract "../work-$img" >/dev/null 2>&1 || true)
	for f in "x-$img"/*; do
		[[ -e "$f" ]] || continue
		cmp "$f" "y-$img/${f##*/}" || fail "foreign note ${f##*/} of $img changed"
	done
	echo "paktest: $img: foreign notes extract identical ($(ls "x-$img" | wc -l) files), cpaktool issues unchanged ($(tail -1 "$img.fsck0"))"
done
cmp foreign.pak work-foreign.pak >/dev/null && fail "work-foreign.pak was not written"

# 3. a 4-bank Pak does not mount (its first bank: the ID says 4 banks)
if [[ -f "$GOLD/gameshark.pak" ]]; then
	head -c 32768 "$GOLD/gameshark.pak" >banks.pak
	cp banks.pak banks0.pak
	if "$BIN" put banks.pak 0 100 0 >banks.log 2>&1; then fail "4-bank Pak mounted"; fi
	cmp banks0.pak banks.pak || fail "4-bank Pak written"
	echo "paktest: 4-bank Pak refused, unchanged ($(grep -o 'pak: [^;]*bank[^)]*)' banks.log | head -1))"
fi

# 4. power cuts
mkdir cuts
"$BIN" cut foreign.pak cuts 2>cuts.log
issues=0
for c in cuts/cut-*.pak; do
	r="$("$CPAK" test "$c" 2>&1 || true)"
	if [[ "$r" != 'No issues found' ]]; then
		grep -v '^Found' <<<"$r" | grep -v 'Orphaned chain\|rphan' >/dev/null && { echo "$r"; fail "cpaktool: $c"; }
		issues=$((issues + 1))
	fi
done
echo "paktest: cpaktool on $(ls cuts/cut-*.pak | wc -l) cut images: $issues with an unreferenced chain only, none other"
echo "paktest: PASS"
