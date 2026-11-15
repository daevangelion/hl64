#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M5 gate (docs/architecture.md 17): the full Uplink route
# (test/routes/uplink.route: hldemo1 -> 2 -> 3 -> 2 -> 1, garg finale,
# trigger_endsection, restart) and the Hazard Course walk
# (test/routes/hazard.route: t0a0 -> t0a0a -> t0a0b1 -> t0a0b2 -> t0a0c ->
# t0a0b -> t0a0c -> t0a0d) at the same checkpoints as the old tree.
#
#   host:  the earlier gates' host parts (test/run/m4.sh, which runs m3,
#          m2, m1, m0 and coll), the test/m5 suites (the table below; a
#          test/m5 entry point missing from it fails the gate), the
#          regression scenes test/m5/*.route (each `route PASS`, exit 0,
#          `rec: wakeover=0`; every route there runs, --route or not), the
#          widening report per route (test/m5/widening.py: the route's
#          checkpoints equal the old cfg's; every waitnear radius widened
#          versus the old cfg carries `# widened: <old> -> <new>: <reason>`,
#          every moved point an `# M5:` comment; the list goes into the gate
#          log), then each route on hl64-host at a fixed 1/30 s:
#          `route PASS` and exit 0; test/m5/checkpoints.py: the checkpoint
#          sequence, the map-switch sequence (and the end section + New
#          Game) equal the old run's (test/m5/oracle/<route>.txt);
#          test/m4/gate_check.py (M4 criteria): the switches in order with
#          their kind, 0 forced, over=0, open_fail=0, blk1ms=0, popin=0
#          outside teleport windows, one continuity line per switch with
#          missing=0 extra=0, sane rec lines; K9: a second run stepping
#          every live entity (`stepall 1`) gives identical route ops and
#          player / entity / seam / rec records. Saved games
#          (test/save/determinism.sh, FlashRAM and --pak, on the save
#          scenes; test/save/pakfail.sh; test/save/flashcut.sh): A/B
#          streams and stats equal, every autosave ok with every frame
#          <= 67 ms, the Pak's no-room, too-large and failed-transfer
#          paths, FlashRAM power cuts in every flash operation of a save.
#   ares:  each route (uplink, hazard and m4: the M5 AI acceptance's
#          routes are uplink and m4; m4 is checked as by test/run/m4.sh,
#          with no old-tree oracle) on a PROFILE HL64_TEST ROM (PASS runs
#          `mem`): route PASS and harness exit 0 at the same checkpoints
#          (forced switches reported, not failed); K10: route ops and
#          player / entity records identical to the host's; the heap after
#          the carve of a PROFILE host built from the same tree right after
#          the first ROM equal to the cart's; M5 AI acceptance
#          (test/m5/ai_check.py, gated on uplink and m4, reported on
#          hazard): no frame over 50 ms with mon >= half its CPU, per map
#          mon p50 / p90 / max, AI-pushed frames, the worst AI frames; per
#          route the CPU / memory table per map visit (test/m5/cpu_table.py,
#          the old tree's cart numbers beside it), the run-wide region
#          table (test/m3/gate_table.py) and the seam-window table
#          (test/m4/gate_table.py) into the gate log; saved games
#          (determinism.sh --ares, FlashRAM and --pak: save, quit ares,
#          relaunch, load).
#
# Every route check runs (and is logged) even after an earlier one failed;
# the gate fails at the end, naming each failure.
#
# Usage: test/run/m5.sh [--ares] [--no-earlier] [--route uplink|hazard|m4 ...]
#        --no-earlier skips test/run/m4.sh, --route runs only the named
#        routes (iteration only: neither is a gate run)
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content, default the c20
#        cook /scratch/hl64/tmp/Int6/c20), HL64_FIXTURES, HL64_DATA,
#        W1_FIXTURES (passed to m4.sh), HL64_ARES_HOST / ARES_BIN (test/run/ares.sh), ARES_TIMEOUT (s, per
#        route, default 14400), DISPLAY_NUM, OLD_TREE (the old tree, for
#        the oracles' verify suites; absent: skipped), OLD_UPLINK_LOG,
#        OLD_CART_LOG (the old runs the oracles were made from).
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m5"
ARES=0
EARLIER=1
ROUTES=()
while [[ $# -gt 0 ]]; do
	case "$1" in
	--ares) ARES=1 ;;
	--no-earlier) EARLIER=0 ;;
	--route)
		[[ "${2:-}" =~ ^(uplink|hazard|m4)$ ]] || { echo "--route uplink|hazard|m4" >&2; exit 2; }
		ROUTES+=("$2")
		shift
		;;
	*) echo "usage: test/run/m5.sh [--ares] [--no-earlier] [--route uplink|hazard|m4 ...]" >&2; exit 2 ;;
	esac
	shift
done
SUBSET=0
if [[ ${#ROUTES[@]} -eq 0 ]]; then ROUTES=(uplink hazard m4); else SUBSET=1; fi
# routes the AI acceptance gates on ares (test/m5/ai_check.py); the others
# are reported
AI_GATED=" uplink m4 "
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int6/c20}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
DATA="${HL64_DATA:-/scratch/hl1/valve}"
OLD_TREE="${OLD_TREE:-$HOME/src/ports/quakegeneric}"
OLD_UPLINK_LOG="${OLD_UPLINK_LOG:-/scratch/hl64/tmp/Gate/old-uplink/host-uplink-parity.log}"
OLD_CART_LOG="${OLD_CART_LOG:-/scratch/quakegeneric/tmp/OldGreen3/r1/ares-uplink-playthrough.log}"
RTB="$CONTENT/campaign.rtb"
for m in hldemo1 hldemo2 hldemo3 t0a0 t0a0a t0a0b t0a0b1 t0a0b2 t0a0c t0a0d; do
	[[ -f "$CONTENT/maps/$m.w64" ]] || { echo "no $CONTENT/maps/$m.w64: cook the content (MAPS='hldemo* t0a0*')" >&2; exit 2; }
done
[[ -f "$RTB" && -f "$CONTENT/maps/campaign.cwg" ]] || { echo "no $RTB / campaign.cwg: cook the content" >&2; exit 2; }
mkdir -p "$OUT"
GATE_LOG="$OUT/gate.log"
: >"$GATE_LOG"
say() { echo "$*" | tee -a "$GATE_LOG"; }
die() { echo "M5 FAIL: $*" | tee -a "$GATE_LOG" >&2; exit 1; }
FAILS=()
fail() { echo "FAIL: $*" | tee -a "$GATE_LOG" >&2; FAILS+=("$*"); }
B="$BUILD_DIR/host"

# fixed frame time (think-and-time.md 1)
CMDS=(--cmd "@1 fixed_dt 33333")
# each route's crossings in order with their kind (the cooked seams: the
# hldemo1 <-> hldemo2 lift is USEONLY, every other seam on them touch)
switches() {
	case "$1" in
	uplink) echo --switch hldemo1:hldemo2:use --switch hldemo2:hldemo3:touch \
		--switch hldemo3:hldemo2:touch --switch hldemo2:hldemo1:use ;;
	hazard) echo --switch t0a0:t0a0a:touch --switch t0a0a:t0a0b1:touch \
		--switch t0a0b1:t0a0b2:touch --switch t0a0b2:t0a0c:touch --switch t0a0c:t0a0b:touch \
		--switch t0a0b:t0a0c:touch --switch t0a0c:t0a0d:touch ;;
	m4) echo --switch hldemo1:hldemo2:use --switch hldemo2:hldemo3:touch ;;
	esac
}

# ---- earlier gates (host parts): m4.sh runs m3, m2, m1, m0 and coll ----
if [[ $EARLIER -eq 1 ]]; then
	BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" HL64_FIXTURES="$FIX" HL64_DATA="$DATA" \
		W1_FIXTURES="${W1_FIXTURES:-$FIX/w1}" "$ROOT/test/run/m4.sh" \
		>"$OUT/m4.log" 2>&1 || { tail -20 "$OUT/m4.log" >&2; die "m4 gate (host)"; }
	say "m4: $(tail -1 "$OUT/m4.log")"
else
	say "earlier gates skipped (--no-earlier): not a gate run"
fi
[[ $SUBSET -eq 0 ]] || say "routes ${ROUTES[*]} only (--route): not a gate run"

# ---- host build, M5 suites ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/host-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-build.log" >&2; die "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && die "host build has warnings"
RAN=" "
# suite <entry point under test/m5> <label> <command...>
suite() {
	local entry="$1" label="$2" log="$OUT/suite-$2.log"
	shift 2
	RAN+="$entry "
	if (cd "$ROOT" && "$@") >"$log" 2>&1; then
		say "suite $label: PASS ($(tail -1 "$log"))"
	else
		tail -20 "$log" >&2
		fail "suite $label"
	fi
}
suite checkpoints.py checkpoints-selftest python3 test/m5/checkpoints.py selftest
suite widening.py widening-selftest python3 test/m5/widening.py --selftest
# the oracles still equal the old runs they were made from (skipped when
# those runs are not on this machine)
suite checkpoints.py oracle-uplink python3 test/m5/checkpoints.py verify --name uplink \
	--cfg "$OLD_TREE/n64/port/test/uplink_route.cfg" --log "$OLD_UPLINK_LOG" \
	--oracle test/m5/oracle/uplink.txt
suite checkpoints.py oracle-hazard python3 test/m5/checkpoints.py verify --name hazard \
	--cfg "$OLD_TREE/n64/port/test/out/hl-walk-t0a0_.cfg" --log "$OLD_TREE/n64/port/test/out/hl-walk-t0a0_.log" \
	--oracle test/m5/oracle/hazard.txt
if [[ -f "$OLD_CART_LOG" ]]; then
	suite old_frames.py oracle-cart-frames bash -c 'python3 test/m5/old_frames.py "$1" --source x \
		-o "$2" >/dev/null && diff <(grep -v "^#" "$2") <(grep -v "^#" test/m5/oracle/uplink-cart-frames.txt) \
		&& echo "uplink-cart-frames.txt equals $1"' _ "$OLD_CART_LOG" "$OUT/uplink-cart-frames.txt"
else
	RAN+="old_frames.py "
	say "suite oracle-cart-frames: $OLD_CART_LOG not on this machine (skipped)"
fi
# every entry point of test/m5 runs here (cpu_table.py: the ares table below)
suite ai_check.py ai-check-selftest python3 test/m5/ai_check.py --selftest
RAN+="cpu_table.py "
# the pad binding (test/m5/in_test.c); its ares check, test/m5/controls_ares.sh,
# runs by hand (docs/controls.md)
suite in_test.c pad-binding bash -c 'make --no-print-directory BUILD_DIR="$1" intest >/dev/null \
	&& "$1/host/hl64-in"' _ "$BUILD_DIR"
RAN+="controls_ares.sh "
# the front end (engine/ui/menu.c, architecture.md 13): title, New Game,
# pause frozen, Configuration stored, Quit to Title back to the boot
# title's memory, menu strings fit the screen; its ares walk,
# test/run/menu.sh, runs by hand (docs/controls.md)
suite ../m6/menu_host.sh menu-host bash test/m6/menu_host.sh "$BUILD_DIR/host/hl64-host" \
	"$CONTENT" "$OUT/menu-host"
# the AI regressions (test/m5/ai_layer.c): a task paused inside StartTask
# survives a refused release and restarts after a drop; a grunt of hldemo2
# neither sees, hears, tracks nor fires at the player home in hldemo1
suite ai_layer.c ai-layer bash -c 'make --no-print-directory BUILD_DIR="$1" ailayertest >/dev/null \
	&& "$1/host/hl64-ailayer" --root "$2" --stuck hldemo2 \
	&& "$1/host/hl64-ailayer" --root "$2" --layer hldemo1 hldemo2' _ "$BUILD_DIR" "$CONTENT"
for f in "$ROOT"/test/m5/*.py "$ROOT"/test/m5/*.sh; do
	[[ -e "$f" ]] || continue
	[[ "$RAN" == *" $(basename "$f") "* ]] || die "test/m5/$(basename "$f") is not wired into test/run/m5.sh"
done

# ---- regression scenes (host): every test/m5/*.route (a short route
# from New Game on one map: a game behaviour a whole route can miss, e.g.
# dish-pending: the dish turned from the control room first) at the fixed
# 1/30 s: `route PASS`, exit 0, no frame whose box wakes went over the
# entity budget (stats `rec: wakeover=0`) ----
for f in "$ROOT"/test/m5/*.route; do
	[[ -e "$f" ]] || continue
	s="$(basename "$f" .route)"
	(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" "test/m5/$s.route" -o "$OUT/scene-$s.rbc") \
		>"$OUT/routec-scene-$s.log" 2>&1 || { cat "$OUT/routec-scene-$s.log" >&2; fail "routec scene $s"; continue; }
	CODE=0
	"$B/hl64-host" --root "$CONTENT" --route "$OUT/scene-$s.rbc" "${CMDS[@]}" >"$OUT/scene-$s.log" 2>&1 || CODE=$?
	if [[ $CODE -ne 0 ]] || ! grep -q '^route PASS$' "$OUT/scene-$s.log"; then
		grep '^route FAIL' "$OUT/scene-$s.log" | tee -a "$GATE_LOG" >&2 || true
		fail "scene $s (host): exit $CODE"
	elif grep -qE '^stats [0-9]+ rec: .* wakeover=[1-9]' "$OUT/scene-$s.log"; then
		fail "scene $s (host): $(grep -E '^rec: frame [0-9]+: box wakes' "$OUT/scene-$s.log" | head -1)"
	else
		say "scene $s: route PASS (host)"
	fi
done

# ---- saved games (host; architecture.md 14): test/save/determinism.sh on
# the save scenes, FlashRAM and Controller Pak (A/B streams and stats
# equal, every autosave ok with every frame <= 67 ms; manual saves' and
# loads' frames reported), test/save/pakfail.sh (the Pak's no-room and
# failed-transfer paths) and test/save/flashcut.sh (FlashRAM power cuts in
# every erase / program of a save in the pool's worst case: no save lost,
# at worst the latest settings change). seam-2to3 saves at 810, right after the
# hldemo2 -> hldemo3 switch, with room for the cart's ~72-frame save before
# T+N; earlier windows meet residency-dependent game rules (infodecal
# retries, the hitbox pose memo) that differ warm (A) / cold (B) ----
SAVE_CASES=(hl2-yard:600:150 hazard-seam:85:150 seam-2to3:810:90 hl2-return-battle:400:300:report)
for pk in flash pak; do
	CODE=0
	BUILD_DIR="$BUILD_DIR/save-gate" HL64_CONTENT="$CONTENT" "$ROOT/test/save/determinism.sh" \
		$([[ $pk == pak ]] && echo --pak) "${SAVE_CASES[@]}" >"$OUT/save-host-$pk.log" 2>&1 || CODE=$?
	grep '^save: ' "$OUT/save-host-$pk.log" | tee -a "$GATE_LOG" || true
	[[ $CODE -eq 0 ]] || fail "saved games (host, $pk): determinism.sh exit $CODE"
done
CODE=0
BUILD_DIR="$BUILD_DIR/save-gate" HL64_CONTENT="$CONTENT" "$ROOT/test/save/pakfail.sh" \
	>"$OUT/save-pakfail.log" 2>&1 || CODE=$?
grep '^pakfail: ' "$OUT/save-pakfail.log" | tee -a "$GATE_LOG" || true
[[ $CODE -eq 0 ]] || fail "Pak fallbacks (host): pakfail.sh exit $CODE"
CODE=0
BUILD_DIR="$BUILD_DIR/save-gate" HL64_CONTENT="$CONTENT" "$ROOT/test/save/flashcut.sh" \
	>"$OUT/save-flashcut.log" 2>&1 || CODE=$?
grep '^flashcut: ' "$OUT/save-flashcut.log" | tee -a "$GATE_LOG" || true
[[ $CODE -eq 0 ]] || fail "FlashRAM power cuts (host): flashcut.sh exit $CODE"

# ---- routes (host) ----
# the lines two runs must agree on: route ops and stats records of <kinds>
records() { grep -E "^route: [0-9]|^stats [0-9]+ ($1): " "$2" || true; }
# route_checks <route> <log> <name> [gate_check args]: checkpoints vs the
# old run, then the M4 log checks; each result into the gate log
route_checks() {
	local r="$1" log="$2" name="$3" code
	shift 3
	# route m4 (the AI acceptance's second route) has no old-tree oracle:
	# its checkpoints are test/run/m4.sh's
	if [[ -f "$ROOT/test/m5/oracle/$r.txt" ]]; then
		code=0
		python3 "$ROOT/test/m5/checkpoints.py" check --name "$r" --oracle "$ROOT/test/m5/oracle/$r.txt" \
			--log "$log" --dump "$OUT/$r.dump" --route "$ROOT/test/routes/$r.route" \
			>"$OUT/checkpoints-$name.txt" || code=$?
		tee -a "$GATE_LOG" <"$OUT/checkpoints-$name.txt" >/dev/null
		[[ $code -eq 0 ]] || fail "$r ($name): $(tail -1 "$OUT/checkpoints-$name.txt")"
	fi
	code=0
	# shellcheck disable=SC2046
	python3 "$ROOT/test/m4/gate_check.py" "$log" $(switches "$r") "$@" >"$OUT/check-$name.txt" || code=$?
	tee -a "$GATE_LOG" <"$OUT/check-$name.txt" >/dev/null
	if [[ $code -ne 0 ]]; then
		grep ' FAIL' "$OUT/check-$name.txt" >&2 || true
		fail "$r ($name) M4 criteria: $(tail -1 "$OUT/check-$name.txt")"
	fi
}
declare -A HOSTOK
for r in "${ROUTES[@]}"; do
	HOSTOK[$r]=0
	if [[ -f "$ROOT/test/m5/oracle/$r.txt" ]]; then
		python3 "$ROOT/test/m5/widening.py" --name "$r" --oracle "$ROOT/test/m5/oracle/$r.txt" \
			--route "$ROOT/test/routes/$r.route" >"$OUT/widening-$r.txt" \
			|| fail "$r widening: $(tail -1 "$OUT/widening-$r.txt")"
		tee -a "$GATE_LOG" <"$OUT/widening-$r.txt" >/dev/null
	fi
	(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" "test/routes/$r.route" -o "$OUT/$r.rbc" \
		&& python3 -m tools.routec.routec --rtb "$RTB" --dump "$OUT/$r.rbc" >"$OUT/$r.dump") \
		>"$OUT/routec-$r.log" 2>&1 || { cat "$OUT/routec-$r.log" >&2; fail "routec $r"; continue; }
	CODE=0
	"$B/hl64-host" --root "$CONTENT" --route "$OUT/$r.rbc" "${CMDS[@]}" >"$OUT/host-$r.log" 2>&1 || CODE=$?
	grep '^route FAIL' "$OUT/host-$r.log" | tee -a "$GATE_LOG" || true
	[[ $CODE -eq 0 ]] || fail "$r (host): hl64-host exit $CODE"
	nf=${#FAILS[@]}
	route_checks "$r" "$OUT/host-$r.log" "host-$r"
	[[ $CODE -eq 0 && ${#FAILS[@]} -eq $nf ]] || continue
	HOSTOK[$r]=1
	say "$r: route PASS (host), $(grep -c '^seam: switch ' "$OUT/host-$r.log") switches, 0 forced$(
		[[ -f "$ROOT/test/m5/oracle/$r.txt" ]] && echo ", checkpoints = old run's")"
	CODE=0
	"$B/hl64-host" --root "$CONTENT" --route "$OUT/$r.rbc" "${CMDS[@]}" --cmd "@1 stepall 1" \
		>"$OUT/host-$r-stepall.log" 2>&1 || CODE=$?
	if ! grep -q '^route PASS$' "$OUT/host-$r-stepall.log" || [[ $CODE -ne 0 ]]; then
		grep '^route FAIL' "$OUT/host-$r-stepall.log" >&2 || true
		fail "$r with stepall (host): exit $CODE"
	elif ! diff <(records 'player|ents|seam|rec' "$OUT/host-$r.log") \
		<(records 'player|ents|seam|rec' "$OUT/host-$r-stepall.log") >"$OUT/k9-$r.diff"; then
		head -20 "$OUT/k9-$r.diff" >&2
		fail "K9 $r: stepping every entity changes the route's records"
	else
		say "K9 $r: step-all run identical ($(records ents "$OUT/host-$r.log" | grep -c ' ents: ') entity-state hashes)"
	fi
done

if [[ $ARES -eq 0 ]]; then
	if [[ ${#FAILS[@]} -ne 0 ]]; then
		say "M5 FAIL (host): ${#FAILS[@]} failure(s): $(IFS=';'; echo "${FAILS[*]}")"
		exit 1
	fi
	say "M5 PASS (host)"
	exit 0
fi

# ---- ares ----
ROMDIR="$BUILD_DIR/m5-rom"
BC="$ROMDIR/content"
mkdir -p "$ROMDIR"
# the PROFILE route's PASS runs `mem` with the last stats record (the
# cart's frame numbers need not be the host's: async opens)
"$B/hl64-host" --write-cmds "$ROMDIR/m5.cmd" "${CMDS[@]}"
# shellcheck source=ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2
HEAP_DONE=0
for r in "${ROUTES[@]}"; do
	[[ -f "$OUT/$r.rbc" ]] || continue
	rm -rf "$BC"
	cp -al "$CONTENT" "$BC"
	cp "$OUT/$r.rbc" "$BC/route.rbc"
	make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 \
		HL64_CONTENT="$BC" HL64_CMDS="$ROMDIR/m5.cmd" rom >"$OUT/rom-build-$r.log" 2>&1 \
		|| { tail -40 "$OUT/rom-build-$r.log" >&2; die "PROFILE rom build ($r)"; }
	grep -E 'warning:' "$OUT/rom-build-$r.log" && die "rom build has warnings ($r)"
	if [[ $HEAP_DONE -eq 0 ]]; then
		# the PROFILE host from the same tree as the ROM, built now (not
		# after the hour-long ares run): its heap after the carve must equal
		# the cart's (test/run/m3.sh)
		make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" PROFILE=1 host \
			>"$OUT/host-profile-build.log" 2>&1 \
			|| { tail -30 "$OUT/host-profile-build.log" >&2; die "PROFILE host build"; }
		"$ROMDIR/host/hl64-host" --root "$BC" "${CMDS[@]}" --frames 1 >"$OUT/host-profile-boot.log" 2>&1 \
			|| { tail -20 "$OUT/host-profile-boot.log" >&2; die "PROFILE host boot"; }
	fi
	CODE=0
	ares_run "$ROMDIR" hl64.z64 ares.log "${ARES_TIMEOUT:-14400}" || CODE=$?
	tr -d '\r' <"$ROMDIR/ares.log" >"$OUT/ares-$r.log"
	grep '^route FAIL' "$OUT/ares-$r.log" | tee -a "$GATE_LOG" || true
	nf=${#FAILS[@]}
	route_checks "$r" "$OUT/ares-$r.log" "ares-$r" --forced-ok
	[[ $CODE -eq 0 ]] || fail "$r (ares): exit $CODE"
	grep -q '^TESTHARNESS exit 0' "$OUT/ares-$r.log" || fail "$r (ares): no harness exit 0"
	grep -q '^mem world: ' "$OUT/ares-$r.log" || fail "$r (ares): no \`mem world:\` line (PASS's mem)"
	[[ ${#FAILS[@]} -eq $nf ]] && say "$r: route PASS (ares), $(grep -c '^seam: switch ' "$OUT/ares-$r.log")" \
		"switches, $(grep -c '^seam: switch .*(forced' "$OUT/ares-$r.log" || true) forced (host 0)"
	if [[ ${HOSTOK[$r]:-0} -eq 1 ]]; then
		if diff <(records 'player|ents' "$OUT/host-$r.log") <(records 'player|ents' "$OUT/ares-$r.log") \
			>"$OUT/k10-$r.diff"; then
			say "K10 $r: route ops and player / entity records identical to the host"
		else
			head -20 "$OUT/k10-$r.diff" >&2
			fail "K10 $r: the cart's route ops / stats records differ from the host's"
		fi
	else
		fail "K10 $r: no passing host run to compare with"
	fi
	if [[ $HEAP_DONE -eq 0 ]]; then
		heapfree() { sed -n 's/^mem: heap free .* free \([0-9]*\) B$/\1/p' "$1" | head -1; }
		HH="$(heapfree "$OUT/host-profile-boot.log")"
		CH="$(heapfree "$OUT/ares-$r.log")"
		if [[ -n "$HH" && "$HH" == "$CH" ]]; then
			say "heap after carve: host $HH B = cart $CH B"
		else
			fail "heap after carve: host ${HH:-?} B, cart ${CH:-?} B (recalibrate limits.h HL64_CART_BOOT_BYTES)"
		fi
		HEAP_DONE=1
	fi
	# M5 AI acceptance: no frame over 50 ms with mon >= half its CPU
	CODE=0
	python3 "$ROOT/test/m5/ai_check.py" "$OUT/ares-$r.log" --name "$r" >"$OUT/table-ai-$r.md" || CODE=$?
	if [[ $CODE -ne 0 && "$AI_GATED" == *" $r "* ]]; then
		fail "$r AI frame budget: $(tail -1 "$OUT/table-ai-$r.md")"
	else
		say "$r AI frame budget$([[ "$AI_GATED" == *" $r "* ]] || echo ' (reported)'): $(tail -1 "$OUT/table-ai-$r.md")"
	fi
	OLD=()
	[[ -f "$ROOT/test/m5/oracle/$r-cart-frames.txt" ]] && OLD=(--old "$ROOT/test/m5/oracle/$r-cart-frames.txt")
	python3 "$ROOT/test/m5/cpu_table.py" "$OUT/ares-$r.log" --title "$r" "${OLD[@]}" >"$OUT/table-maps-$r.md" \
		|| fail "$r per-map CPU table: $(tail -1 "$OUT/table-maps-$r.md")"
	CODE=0
	python3 "$ROOT/test/m3/gate_table.py" "$OUT/ares-$r.log" >"$OUT/table-run-$r.md" || CODE=$?
	[[ $CODE -eq 0 ]] || fail "$r CPU / memory table: $(tail -1 "$OUT/table-run-$r.md")"
	CODE=0
	# shellcheck disable=SC2046
	python3 "$ROOT/test/m4/gate_table.py" "$OUT/ares-$r.log" $(switches "$r") >"$OUT/table-seams-$r.md" || CODE=$?
	[[ $CODE -eq 0 ]] || fail "$r seam-window table: $(tail -1 "$OUT/table-seams-$r.md")"
	{ echo; echo "## $r (ares)"; cat "$OUT/table-maps-$r.md" "$OUT/table-run-$r.md" "$OUT/table-seams-$r.md"; } \
		| tee -a "$GATE_LOG"
done
# saved games on the cart: save, quit ares, relaunch, load (FlashRAM and
# Controller Pak images carried between the launches)
for pk in flash pak; do
	CODE=0
	BUILD_DIR="$BUILD_DIR/save-gate" HL64_CONTENT="$CONTENT" "$ROOT/test/save/determinism.sh" --ares \
		$([[ $pk == pak ]] && echo --pak) "${SAVE_CASES[@]}" >"$OUT/save-ares-$pk.log" 2>&1 || CODE=$?
	grep '^save: ' "$OUT/save-ares-$pk.log" | tee -a "$GATE_LOG" || true
	[[ $CODE -eq 0 ]] || fail "saved games (ares, $pk): determinism.sh exit $CODE"
done
if [[ ${#FAILS[@]} -ne 0 ]]; then
	say "M5 FAIL: ${#FAILS[@]} failure(s): $(IFS=';'; echo "${FAILS[*]}")"
	exit 1
fi
say "M5 PASS (host + ares)"
