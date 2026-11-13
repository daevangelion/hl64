#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M3 gate (docs/architecture.md 17): entities, records, movetypes,
# resources, sound, studio, effects, HUD, the game; route segment A.
#
#   host:  earlier gates' host parts (test/run/m0.sh, m1.sh, m2.sh with
#          coll.sh inside it), every M3 slice suite (the table below; a
#          test/m3 entry point missing from it fails the gate; the monster
#          sweep must pass on all 10 maps), the route
#          VM op tests (test/m3/gate_ops.sh), then route m3-a on hl64-host
#          at a fixed 1/30 s frame time: `route PASS`, the game's
#          `changelevel: hldemo2` line, and K9: a second run with every
#          live entity stepped every frame (`stepall 1`) gives the same
#          route ops, player records and entity-state hashes.
#   ares:  m3-a on a PROFILE HL64_TEST ROM: `route PASS`, route ops and
#          stats records (player, ents) identical to the host's (K10), the
#          host's heap after the carve equal to the cart's; the
#          per-subsystem CPU / memory table from the 1 s `prof <zone>_us`
#          roll-ups and the `mem` lines (test/m3/gate_table.py) into the
#          gate log; a carve region above 90% use or with peak x 1.25 over
#          its size fails (architecture 4.1; components exempt).
#
# Usage: test/run/m3.sh [--ares]
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content: campaign.rtb,
#        hldemo*), HL64_FIXTURES (default /scratch/hl64/fixtures),
#        HL64_DATA (Valve data for ent.py's PVS oracle and m1.sh's W1,
#        default /scratch/hl1/valve), W1_FIXTURES (default
#        $HL64_FIXTURES/w1), ARES_BIN, ARES_TIMEOUT, DISPLAY_NUM.
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m3"
ARES=0
[[ "${1:-}" == "--ares" ]] && ARES=1
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
DATA="${HL64_DATA:-/scratch/hl1/valve}"
RTB="$CONTENT/campaign.rtb"
[[ -f "$RTB" && -f "$CONTENT/maps/hldemo1.w64" ]] || { echo "no $RTB / hldemo1: cook the content" >&2; exit 2; }
[[ -d "$FIX/goldsrc/think-and-time" && -d "$FIX/goldsrc/physics-movetypes" ]] \
	|| { echo "no GoldSrc fixtures under $FIX" >&2; exit 2; }
mkdir -p "$OUT"
GATE_LOG="$OUT/gate.log"
: >"$GATE_LOG"
say() { echo "$*" | tee -a "$GATE_LOG"; }
fail() { echo "M3 FAIL: $*" | tee -a "$GATE_LOG" >&2; exit 1; }
B="$BUILD_DIR/host"

# fixed frame time (think-and-time.md 1)
CMDS=(--cmd "@1 fixed_dt 33333")

# ---- earlier gates (host parts) ----
for g in m0 m1 m2; do
	BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" HL64_FIXTURES="$FIX" HL64_DATA="$DATA" \
		W1_FIXTURES="${W1_FIXTURES:-$FIX/w1}" "$ROOT/test/run/$g.sh" \
		>"$OUT/$g.log" 2>&1 || { tail -20 "$OUT/$g.log" >&2; fail "$g gate (host)"; }
	say "$g: $(tail -1 "$OUT/$g.log")"
done

# ---- M3 slice suites ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/host-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
# suite <entry point under test/m3> <driver make target or -> <command...>:
# runs when the entry point exists (slices land at different times)
RAN=" "
suite() {
	local entry="$1" target="$2" log="$OUT/suite-${1%.*}.log"
	shift 2
	RAN+="$entry "
	[[ -e "$ROOT/test/m3/$entry" ]] || { say "suite $entry: not on this tree"; return 0; }
	if [[ "$target" != - ]]; then
		make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" "$target" >"$log.build" 2>&1 \
			|| { tail -30 "$log.build" >&2; fail "suite $entry: make $target"; }
		grep -E 'warning:' "$log.build" && fail "suite $entry: make $target has warnings"
	fi
	if (cd "$ROOT" && PYTHONPATH="$ROOT" BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" \
		HL64_FIXTURES="$FIX" "$@") >"$log" 2>&1; then
		say "suite $entry: PASS ($(tail -1 "$log"))"
	else
		tail -20 "$log" >&2
		fail "suite $entry"
	fi
}
suite ent.py enttest python3 test/m3/ent.py --bin "$B/hl64-ent" --content "$CONTENT" \
	--fixtures "$FIX/goldsrc" --data "$DATA"
suite rec.sh - test/m3/rec.sh
suite phys.py phystest python3 test/m3/phys.py --bin "$B/hl64-phys" --content "$CONTENT" \
	--fixtures "$FIX/goldsrc"
suite game.py gametest python3 test/m3/game.py --bin "$B/hl64-game" --content "$CONTENT"
suite studio_test.py studiotest python3 test/m3/studio_test.py --bin "$B/hl64-studiotest" \
	--content "$CONTENT"
suite snd.sh - test/m3/snd.sh all
suite fx.py fxtest python3 test/m3/fx.py "$BUILD_DIR" "$CONTENT"
suite items.py itemstest python3 test/m3/items.py --bin "$B/hl64-items" --content "$CONTENT" \
	--data "$DATA" --build-dir "$BUILD_DIR" --scratch "$OUT/items"
suite mon.sh - test/m3/mon.sh
if [[ -e "$ROOT/test/m3/mon.sh" ]]; then
	# the monster sweep covers the 10 Uplink + Hazard maps (mon_sweep.py MAPS)
	ML="$OUT/suite-mon.log"
	mp="$(grep -cE '^mon-sweep [a-z0-9]+ PASS classes=' "$ML" || true)"
	mn="$(grep -cE '^mon-sweep [a-z0-9]+ (PASS|FAIL) classes=' "$ML" || true)"
	[[ "$mp" -eq 10 && "$mn" -eq 10 ]] || fail "monster sweep: $mp of $mn maps PASS, 10 required"
	say "mon sweep: $mp/10 maps PASS"
fi
suite gate_ops.sh - test/m3/gate_ops.sh
# every entry point of test/m3 runs here (ares ones in the ares part;
# rec.py is rec.sh's, mon_sweep.py and mon-dome.route are mon.sh's,
# gate_table.py the table below)
RAN+="snd_ares.sh rec.py mon_sweep.py mon-dome.route gate_table.py "
for f in "$ROOT"/test/m3/*.py "$ROOT"/test/m3/*.sh; do
	[[ "$RAN" == *" $(basename "$f") "* ]] || fail "test/m3/$(basename "$f") is not wired into test/run/m3.sh"
done

# ---- route m3-a (host) ----
RBC="$OUT/m3-a.rbc"
(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" test/routes/m3-a.route -o "$RBC") \
	>"$OUT/routec.log" 2>&1 || { cat "$OUT/routec.log" >&2; fail "routec m3-a"; }
run_host() {	# log, extra args...
	local log="$1" code
	shift
	set +e
	"$B/hl64-host" --root "$CONTENT" --route "$RBC" "${CMDS[@]}" "$@" >"$log" 2>&1
	code=$?
	set -e
	grep -q '^route PASS$' "$log" || { grep '^route FAIL' "$log" >&2 || tail -20 "$log" >&2; return 1; }
	[[ $code -eq 0 ]] || { echo "exit $code" >&2; return 1; }
}
# the lines both runs (and the cart) must agree on
records() { grep -E '^route: [0-9]|^stats [0-9]+ (player|ents): ' "$1"; }
run_host "$OUT/host-a.log" || fail "m3-a (host)"
grep -q '^changelevel: hldemo2 ' "$OUT/host-a.log" || fail "m3-a (host): no 'changelevel: hldemo2' line"
say "m3-a: route PASS (host), $(grep '^changelevel: hldemo2 ' "$OUT/host-a.log" | head -1)"
run_host "$OUT/host-a-stepall.log" --cmd "@1 stepall 1" || fail "m3-a with stepall (host)"
diff <(records "$OUT/host-a.log") <(records "$OUT/host-a-stepall.log") >"$OUT/k9.diff" \
	|| { head -20 "$OUT/k9.diff" >&2; fail "K9: stepping every entity changes the route's records"; }
say "K9: step-all run identical ($(records "$OUT/host-a.log" | grep -c ' ents: ') entity-state hashes)"

if [[ $ARES -eq 0 ]]; then
	say "M3 PASS (host)"
	exit 0
fi

# ---- ares ----
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; exit 2; }
ROMDIR="$BUILD_DIR/m3-rom"
BC="$ROMDIR/content"
rm -rf "$BC"
mkdir -p "$ROMDIR"
cp -al "$CONTENT" "$BC"
cp "$RBC" "$BC/route.rbc"
# `mem` at the route's last stats record (frames match the host's: K10)
LAST="$(sed -n 's/^stats [0-9]* frame=\([0-9]*\) .*/\1/p' "$OUT/host-a.log" | tail -1)"
[[ -n "$LAST" ]] || fail "no stats record in the host run"
"$B/hl64-host" --write-cmds "$ROMDIR/m3.cmd" "${CMDS[@]}" --cmd "@$LAST mem"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 \
	HL64_CONTENT="$BC" HL64_CMDS="$ROMDIR/m3.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "PROFILE rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"

# shellcheck source=xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT
set +e
(cd "$ROMDIR" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
	timeout "${ARES_TIMEOUT:-1800}" "$ARES_BIN" --system "Nintendo 64" \
	--setting Video/Driver=Vulkan --setting Audio/Driver=None \
	--setting Input/Driver=SDL --setting General/HomebrewMode=true \
	hl64.z64 >ares.log 2>&1)
CODE=$?
set -e
tr -d '\r' <"$ROMDIR/ares.log" >"$OUT/ares-a.log"
grep -q '^route PASS$' "$OUT/ares-a.log" || { grep '^route FAIL' "$OUT/ares-a.log" >&2 || tail -30 "$OUT/ares-a.log" >&2; fail "m3-a (ares)"; }
[[ $CODE -eq 0 ]] || fail "ares exit $CODE"
grep -q '^TESTHARNESS exit 0' "$OUT/ares-a.log" || fail "no harness exit 0"
grep -q '^changelevel: hldemo2 ' "$OUT/ares-a.log" || fail "m3-a (ares): no 'changelevel: hldemo2' line"
diff <(records "$OUT/host-a.log") <(records "$OUT/ares-a.log") >"$OUT/k10.diff" \
	|| { head -20 "$OUT/k10.diff" >&2; fail "K10: the cart's route ops / stats records differ from the host's"; }
say "m3-a: route PASS (ares), route ops and player / entity records identical to the host (K10)"
# the host arena is the cart's heap after the carve (limits.h HL64_HOST_HEAP_KB)
heapfree() { sed -n 's/^mem: heap free \([0-9]*\) KB.*/\1/p' "$1" | head -1; }
HH="$(heapfree "$OUT/host-a.log")"
CH="$(heapfree "$OUT/ares-a.log")"
[[ -n "$HH" && "$HH" == "$CH" ]] \
	|| fail "heap after carve: host ${HH:-?} KB, cart ${CH:-?} KB (set HL64_HOST_HEAP_KB to the cart's)"
say "heap after carve: host $HH KB = cart $CH KB"

if [[ -e "$ROOT/test/m3/snd_ares.sh" ]]; then
	BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" ARES_BIN="$ARES_BIN" "$ROOT/test/m3/snd_ares.sh" \
		>"$OUT/suite-snd_ares.log" 2>&1 || { tail -20 "$OUT/suite-snd_ares.log" >&2; fail "suite snd_ares.sh"; }
	say "suite snd_ares.sh: PASS ($(tail -1 "$OUT/suite-snd_ares.log"))"
fi

set +e
python3 "$ROOT/test/m3/gate_table.py" "$OUT/ares-a.log" >"$OUT/table.md"
CODE=$?
set -e
tee -a "$GATE_LOG" <"$OUT/table.md"
[[ $CODE -eq 0 ]] || fail "CPU / memory table: $(tail -1 "$OUT/table.md")"
say "M3 PASS (host + ares)"
