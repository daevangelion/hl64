#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Route VM entity / record op tests (docs/formats/route-bytecode.md,
# engine/test/route.c): small routes on hldemo1, compiled by routec
# against the cooked campaign.rtb and run on hl64-host at a fixed 1/30 s
# frame time. Each case expects `route PASS` or one exact `route FAIL <op#>
# <src>:<line> <MNEMONIC> <reason>`, plus the VM's diagnostic lines where
# they prove the op saw the right entity. The positive WAIT_MOVED (an
# elevator door opened by the game's chain) is route m3-a (test/run/m3.sh).
#
# Usage: test/m3/gate_ops.sh
# Env:   BUILD_DIR (scratch, with host/hl64-host), HL64_CONTENT (cooked
#        content: campaign.rtb, hldemo1).
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
RTB="$CONTENT/campaign.rtb"
HOST="$BUILD_DIR/host/hl64-host"
OUT="$BUILD_DIR/m3-gate-ops"
[[ -f "$RTB" && -f "$CONTENT/maps/hldemo1.w64" ]] || { echo "no $RTB / hldemo1: cook the content" >&2; exit 2; }
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT.build.log" 2>&1 \
	|| { tail -30 "$OUT.build.log" >&2; exit 1; }
mkdir -p "$OUT"
FAILS=0
CASES=0

# case <name> <expected: PASS | FAIL line regex> [grep regex that must
# match the log]...; the route source on stdin
case_() {
	local name="$1" want="$2" src="$OUT/$1.route" log="$OUT/$1.log" got
	shift 2
	CASES=$((CASES + 1))
	cat >"$src"
	if ! (cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" "$src" -o "$OUT/$name.rbc") \
		>"$log" 2>&1; then
		echo "$name: routec failed: $(tail -1 "$log")"
		FAILS=$((FAILS + 1))
		return
	fi
	"$HOST" --root "$CONTENT" --route "$OUT/$name.rbc" --cmd "@1 fixed_dt 33333" >"$log" 2>&1 || true
	got="$(grep -E '^route (PASS|FAIL)' "$log" | head -1)"
	if [[ "$want" == PASS ]]; then
		[[ "$got" == "route PASS" ]] || { echo "$name: want PASS, got '${got:-no verdict}'"; FAILS=$((FAILS + 1)); return; }
	else
		[[ "$got" =~ ^route\ FAIL\ $want$ ]] || { echo "$name: want FAIL $want, got '${got:-no verdict}'"; FAILS=$((FAILS + 1)); return; }
	fi
	while (($#)); do
		grep -qE "$1" "$log" || { echo "$name: no line matching '$1'"; FAILS=$((FAILS + 1)); return; }
		shift
	done
	echo "$name: ok (${got})"
}

# DROP (CBaseEntity::Create, TOSS onto the floor at z 112), WAIT_CLASS on
# it once landed (radius 1), LOOK_AT by class (aims down at the item),
# WAIT_GLOBAL off (unset = OFF)
case_ drop-class-lookat PASS \
	'^route: drop entity [0-9]+ cls ' \
	'^route: class entity [0-9]+ cls [0-9]+ org -2000 -620 112\.0[0-9]* vel 0 0 0' \
	'^route: look_at angles [0-9.]+ ' <<'EOF'
waitmap hldemo1 600
setpos -2327 -230 196 0 180
drop weapon_9mmAR -2000 -620 196
waitclass weapon_9mmAR -2000 -620 112 1 5
lookat weapon_9mmAR
waitglobal hldemo_global_dish_ms 0 off
pass
EOF
# the dropped item must not satisfy WAIT_CLASS outside its radius
case_ class-radius '[0-9]+ [^ ]+:4 WAIT_CLASS timeout' <<'EOF'
waitmap hldemo1 600
setpos -2327 -230 196 0 180
drop weapon_9mmAR -2000 -620 196
waitclass weapon_9mmAR -2000 -500 112 32 2
pass
EOF
# a closed func_door (angle 90: HLSDK SetMovedir zeroes its angles at
# spawn) has not moved
case_ moved-at-rest '[0-9]+ [^ ]+:2 WAIT_MOVED timeout' <<'EOF'
waitmap hldemo1 600
waitmoved startele1 2
pass
EOF
# the doors are live and never removed
case_ gone-live '[0-9]+ [^ ]+:2 WAIT_GONE timeout' <<'EOF'
waitmap hldemo1 600
waitgone startele1 1
pass
EOF
# trigger_once start_script_trigger (two records: the dome entrance and the
# upper walkway) removes itself when the player touches it
case_ gone-trigger PASS <<'EOF'
waitmap hldemo1 600
setpos -2048 -496 70 0 270
wait 1
setpos -2300 -490 200 0 270
waitgone start_script_trigger 3
pass
EOF
# trigger_once #106 -> start_popping_panel_mm: the func_door_rotating of
# that name swings open
case_ moved-rotating PASS '^route: moved entity [0-9]+ ' <<'EOF'
waitmap hldemo1 600
setpos -2048 564 60 0 90
waitmoved start_popping_panel_mm 5
pass
EOF
case_ global-on '[0-9]+ [^ ]+:2 WAIT_GLOBAL timeout' <<'EOF'
waitmap hldemo1 600
waitglobal hldemo_global_dish_ms 1 on
pass
EOF
case_ end-not-fired '[0-9]+ [^ ]+:2 WAIT_END timeout' <<'EOF'
waitmap hldemo1 600
waitend 1
pass
EOF
case_ lookat-none '[0-9]+ [^ ]+:2 LOOK_AT no live entity' <<'EOF'
waitmap hldemo1 600
lookat monster_grunt_repel
pass
EOF
# LOOK_AT by name: the nearest live startele1 door (hydrated once the
# player stands next to it)
case_ lookat-name PASS '^route: look_at entity [0-9]+ ' <<'EOF'
waitmap hldemo1 600
setpos -2327 -230 196 0 0
wait 0.5
lookat startele1
pass
EOF

echo "route ops: $((CASES - FAILS))/$CASES cases pass"
[[ $FAILS -eq 0 ]]
