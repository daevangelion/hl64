#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M4 gate (docs/architecture.md 17): streaming crossings. Route m4
# (test/routes/m4.route: hldemo1 -> hldemo2 -> hldemo3) with seams, twins,
# dehydrate / rehydrate, frozen timers and async residency.
#
#   host:  the earlier gates' host parts (test/run/m3.sh, which runs m0, m1,
#          m2 and coll), the test/m4 suites (the table below; a test/m4
#          entry point missing from it fails the gate), then route m4 on
#          hl64-host at a fixed 1/30 s frame time; test/m4/gate_check.py:
#          `route PASS` and exit 0, both seam switches in order
#          (hldemo1->hldemo2 use, hldemo2->hldemo3 touch), no forced switch,
#          every stats record over=0, open_fail=0, blk1ms=0 (no frame with
#          > 1 ms of blocked I/O), popin=0 (popin_tele allowed), one
#          continuity line per switch with missing=0 extra=0, sane rec
#          lines; K9: a second run stepping every live entity (`stepall 1`)
#          gives identical route ops and player / entity / seam / rec
#          records.
#   ares:  m4 on a PROFILE HL64_TEST ROM (`mem` at the last stats frame):
#          the same checks with forced switches reported instead of
#          failing; K10: route ops and player / entity records identical
#          to the host's; the host's heap after the carve equal to the
#          cart's; the run's CPU / memory table (test/m3/gate_table.py) and
#          the seam-window table (test/m4/gate_table.py: per crossing, the
#          worst frame of each CPU zone within +-2 s and each region's peak
#          in that window, region rule 90% / peak x 1.25, the old tree's
#          crossing numbers beside them) into the gate log.
#
# Usage: test/run/m4.sh [--ares] [--no-earlier]
#        --no-earlier skips test/run/m3.sh (iteration only: not a gate run)
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content, default the c6
#        cook /scratch/hl64/tmp/M3Lead2/c6), HL64_FIXTURES, HL64_DATA,
#        W1_FIXTURES (passed to m3.sh), ARES_BIN, ARES_TIMEOUT, DISPLAY_NUM.
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m4"
ARES=0
EARLIER=1
for a in "$@"; do
	case "$a" in
	--ares) ARES=1 ;;
	--no-earlier) EARLIER=0 ;;
	*) echo "usage: test/run/m4.sh [--ares] [--no-earlier]" >&2; exit 2 ;;
	esac
done
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/M3Lead2/c6}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
DATA="${HL64_DATA:-/scratch/hl1/valve}"
RTB="$CONTENT/campaign.rtb"
for m in hldemo1 hldemo2 hldemo3; do
	[[ -f "$CONTENT/maps/$m.w64" ]] || { echo "no $CONTENT/maps/$m.w64: cook the content" >&2; exit 2; }
done
[[ -f "$RTB" && -f "$CONTENT/maps/campaign.cwg" ]] || { echo "no $RTB / campaign.cwg: cook the content" >&2; exit 2; }
mkdir -p "$OUT"
GATE_LOG="$OUT/gate.log"
: >"$GATE_LOG"
say() { echo "$*" | tee -a "$GATE_LOG"; }
fail() { echo "M4 FAIL: $*" | tee -a "$GATE_LOG" >&2; exit 1; }
B="$BUILD_DIR/host"

# fixed frame time (think-and-time.md 1)
CMDS=(--cmd "@1 fixed_dt 33333")
# the route's crossings, in order (seams.json of the c6 cook)
SWITCHES=(--switch hldemo1:hldemo2:use --switch hldemo2:hldemo3:touch)

# ---- earlier gates (host parts): m3.sh runs m0, m1, m2 (+ coll) ----
if [[ $EARLIER -eq 1 ]]; then
	BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" HL64_FIXTURES="$FIX" HL64_DATA="$DATA" \
		W1_FIXTURES="${W1_FIXTURES:-$FIX/w1}" "$ROOT/test/run/m3.sh" \
		>"$OUT/m3.log" 2>&1 || { tail -20 "$OUT/m3.log" >&2; fail "m3 gate (host)"; }
	say "m3: $(tail -1 "$OUT/m3.log")"
else
	say "earlier gates skipped (--no-earlier): not a gate run"
fi

# ---- host build, M4 suites ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/host-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
# suite <entry point under test/m4> <driver make target or -> <command...>
RAN=" "
suite() {
	local entry="$1" target="$2" log="$OUT/suite-${1%.*}.log"
	shift 2
	RAN+="$entry "
	[[ -e "$ROOT/test/m4/$entry" ]] || { say "suite $entry: not on this tree"; return 0; }
	if [[ "$target" != - ]]; then
		make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" "$target" >"$log.build" 2>&1 \
			|| { tail -30 "$log.build" >&2; fail "suite $entry: make $target"; }
		grep -E 'warning:' "$log.build" && fail "suite $entry: make $target has warnings"
	fi
	if (cd "$ROOT" && BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" HL64_FIXTURES="$FIX" "$@") \
		>"$log" 2>&1; then
		say "suite $entry: PASS ($(tail -1 "$log"))"
	else
		tail -20 "$log" >&2
		fail "suite $entry"
	fi
}
suite gate_seamprof.py seamproftest python3 test/m4/gate_seamprof.py --bin "$B/hl64-seamprof"
suite gate_selftest.py - python3 test/m4/gate_selftest.py --scratch "$OUT/selftest"
# every entry point of test/m4 runs here (gate_parse.py is the parsers'
# module, gate_check.py the route checks and gate_table.py the ares table
# below; gate_seamprof.c is gate_seamprof.py's driver)
RAN+="gate_parse.py gate_check.py gate_table.py "
for f in "$ROOT"/test/m4/*.py "$ROOT"/test/m4/*.sh; do
	[[ -e "$f" ]] || continue
	[[ "$RAN" == *" $(basename "$f") "* ]] || fail "test/m4/$(basename "$f") is not wired into test/run/m4.sh"
done

# ---- route m4 (host) ----
RBC="$OUT/m4.rbc"
(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" test/routes/m4.route -o "$RBC") \
	>"$OUT/routec.log" 2>&1 || { cat "$OUT/routec.log" >&2; fail "routec m4"; }
# run_host <log> [extra args]: one route run; its exit code
run_host() {
	local log="$1"
	shift
	"$B/hl64-host" --root "$CONTENT" --route "$RBC" "${CMDS[@]}" "$@" >"$log" 2>&1
}
# check <log> <name> [gate_check args]: every M4 log check; the failing
# ones (and the route FAIL line: op, source line, cause) on stderr
check() {
	local log="$1" name="$2" code=0
	shift 2
	python3 "$ROOT/test/m4/gate_check.py" "$log" "${SWITCHES[@]}" "$@" >"$OUT/check-$name.txt" \
		|| code=$?
	tee -a "$GATE_LOG" <"$OUT/check-$name.txt" >/dev/null
	if [[ $code -ne 0 ]]; then
		grep ' FAIL' "$OUT/check-$name.txt" >&2
		fail "m4 ($name): $(tail -1 "$OUT/check-$name.txt")"
	fi
}
# the lines two runs must agree on: route ops and stats records of <kinds>
records() { grep -E "^route: [0-9]|^stats [0-9]+ ($1): " "$2" || true; }

CODE=0
run_host "$OUT/host.log" || CODE=$?
[[ $CODE -eq 0 ]] || say "m4 (host): hl64-host exit $CODE"
check "$OUT/host.log" host
[[ $CODE -eq 0 ]] || fail "m4 (host): exit $CODE"
say "m4: route PASS (host), $(grep -c '^seam: switch ' "$OUT/host.log") switches, 0 forced"
CODE=0
run_host "$OUT/host-stepall.log" --cmd "@1 stepall 1" || CODE=$?
grep -q '^route PASS$' "$OUT/host-stepall.log" && [[ $CODE -eq 0 ]] \
	|| { grep '^route FAIL' "$OUT/host-stepall.log" >&2 || true; fail "m4 with stepall (host): exit $CODE"; }
diff <(records 'player|ents|seam|rec' "$OUT/host.log") \
	<(records 'player|ents|seam|rec' "$OUT/host-stepall.log") >"$OUT/k9.diff" \
	|| { head -20 "$OUT/k9.diff" >&2; fail "K9: stepping every entity changes the route's records"; }
say "K9: step-all run identical ($(records ents "$OUT/host.log" | grep -c ' ents: ') entity-state hashes)"

if [[ $ARES -eq 0 ]]; then
	say "M4 PASS (host)"
	exit 0
fi

# ---- ares ----
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; exit 2; }
ROMDIR="$BUILD_DIR/m4-rom"
BC="$ROMDIR/content"
rm -rf "$BC"
mkdir -p "$ROMDIR"
cp -al "$CONTENT" "$BC"
cp "$RBC" "$BC/route.rbc"
# `mem` at the route's last stats record (frames match the host's: K10)
LAST="$(sed -n 's/^stats [0-9]* frame=\([0-9]*\) .*/\1/p' "$OUT/host.log" | tail -1)"
[[ -n "$LAST" ]] || fail "no stats record in the host run"
"$B/hl64-host" --write-cmds "$ROMDIR/m4.cmd" "${CMDS[@]}" --cmd "@$LAST mem"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 \
	HL64_CONTENT="$BC" HL64_CMDS="$ROMDIR/m4.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "PROFILE rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"

# shellcheck source=xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT
CODE=0
(cd "$ROMDIR" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
	timeout "${ARES_TIMEOUT:-3600}" "$ARES_BIN" --system "Nintendo 64" \
	--setting Video/Driver=Vulkan --setting Audio/Driver=None \
	--setting Input/Driver=SDL --setting General/HomebrewMode=true \
	hl64.z64 >ares.log 2>&1) || CODE=$?
tr -d '\r' <"$ROMDIR/ares.log" >"$OUT/ares.log"
check "$OUT/ares.log" ares --forced-ok
[[ $CODE -eq 0 ]] || fail "ares exit $CODE"
grep -q '^TESTHARNESS exit 0' "$OUT/ares.log" || fail "no harness exit 0"
say "m4: route PASS (ares), $(grep -c '^seam: switch ' "$OUT/ares.log") switches," \
	"$(grep -c '^seam: switch .*(forced' "$OUT/ares.log" || true) forced (host 0)"
diff <(records 'player|ents' "$OUT/host.log") <(records 'player|ents' "$OUT/ares.log") >"$OUT/k10.diff" \
	|| { head -20 "$OUT/k10.diff" >&2; fail "K10: the cart's route ops / stats records differ from the host's"; }
say "m4: route ops and player / entity records identical to the host (K10)"
# host heap = the cart's heap after the carve (test/run/m3.sh)
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" PROFILE=1 host >"$OUT/host-profile-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-profile-build.log" >&2; fail "PROFILE host build"; }
"$ROMDIR/host/hl64-host" --root "$BC" "${CMDS[@]}" --frames 1 >"$OUT/host-profile-boot.log" 2>&1 \
	|| { tail -20 "$OUT/host-profile-boot.log" >&2; fail "PROFILE host boot"; }
heapfree() { sed -n 's/^mem: heap free .* free \([0-9]*\) B$/\1/p' "$1" | head -1; }
HH="$(heapfree "$OUT/host-profile-boot.log")"
CH="$(heapfree "$OUT/ares.log")"
[[ -n "$HH" && "$HH" == "$CH" ]] \
	|| fail "heap after carve: host ${HH:-?} B, cart ${CH:-?} B (recalibrate limits.h HL64_CART_BOOT_BYTES)"
say "heap after carve: host $HH B = cart $CH B"

CODE=0
CODE2=0
python3 "$ROOT/test/m3/gate_table.py" "$OUT/ares.log" >"$OUT/table-run.md" || CODE=$?
python3 "$ROOT/test/m4/gate_table.py" "$OUT/ares.log" "${SWITCHES[@]}" >"$OUT/table-seams.md" \
	|| CODE2=$?
tee -a "$GATE_LOG" <"$OUT/table-run.md"
tee -a "$GATE_LOG" <"$OUT/table-seams.md"
[[ $CODE -eq 0 ]] || fail "CPU / memory table: $(tail -1 "$OUT/table-run.md")"
[[ $CODE2 -eq 0 ]] || fail "seam-window table: $(tail -1 "$OUT/table-seams.md")"
say "M4 PASS (host + ares)"
