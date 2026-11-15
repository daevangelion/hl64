#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Front-end host test (engine/ui/menu.c, docs/architecture.md 13), a suite
# of test/run/m5.sh: hl64-host --title at the fixed 1/30 s, the pad driven
# by `pad` debug commands.
#  run 1: boot: `menu: layout ok` (every menu string and row fits the
#         framebuffer, menu.c layout_check);
#         title -> New Game -> Easy -> play; Start pauses: the game state
#         (stats t=, ents, player, rec) is the same at two frames of the
#         pause; Configuration: brightness +1 step (`menu: set
#         brightness=10`), B stores it (`settings store`); Start resumes and
#         the game moves on.
#  run 2: the same New Game, then Start -> Quit to Title -> confirm: `mem:
#         title same as boot` (every region and the heap back to the boot
#         title's); New Game again on the same frame offsets: the game
#         after the quit equals the one from boot 200 frames in (stats
#         ents / player / rec).
# Usage: test/m6/menu_host.sh HOST_BIN CONTENT OUTDIR
# Exit:  0 pass (last line `menu_host PASS`), 1 failure.
set -euo pipefail

BIN="$1"
CONTENT="$2"
OUT="$3"
mkdir -p "$OUT"
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
# pad bits (engine/core/sys.h SYS_PAD_*)
A=1 B=2 START=8 UP=1024 DOWN=2048 RIGHT=8192
ARGS=()
tap() { ARGS+=(--cmd "@$1 pad $2 0 0" --cmd "@$(($1 + 1)) pad 0 0 0"); }
# lines of stats record L that hold the game's state
state() { grep -E "^stats $1 (ents|player|rec):" "$2" | sed -E "s/^stats $1 //"; }
gtime() { grep -E "^stats $1 frame=" "$2" | sed -E 's/.* t=([0-9]+) .*/\1/'; }

# ---- run 1: play, pause, configuration, resume ----
ARGS=(--cmd "@1 fixed_dt 33333")
tap 10 $A
tap 14 $A
ARGS+=(--cmd "@216 stats 1")
tap 220 $START
ARGS+=(--cmd "@224 stats 2")
tap 226 $DOWN
tap 228 $DOWN
tap 230 $DOWN
tap 232 $A
tap 234 $RIGHT
ARGS+=(--cmd "@238 stats 3")
tap 240 $B
tap 242 $START
ARGS+=(--cmd "@300 stats 4")
"$BIN" --root "$CONTENT" --title "${ARGS[@]}" --frames 302 >"$OUT/run1.log" 2>&1 || fail "run 1 exit $?"
L1="$OUT/run1.log"
for want in '^menu: layout ok ' '^menu: title$' '^menu: screen newgame$' \
	'^menu: new game on [a-z0-9]+, skill 1$' \
	'^menu: play$' '^menu: pause$' '^menu: screen config$' '^menu: set brightness=10 ' \
	'^menu: settings store started$' '^menu: resume$'; do
	grep -qE "$want" "$L1" || fail "run 1: no line /$want/"
done
grep -qE '^snd [0-9]+ pause$' "$L1" || fail "run 1: sounds not paused"
grep -qE '^snd [0-9]+ resume$' "$L1" || fail "run 1: sounds not resumed"
[[ -n "$(state 2 "$L1")" && "$(state 2 "$L1")" == "$(state 3 "$L1")" ]] \
	|| fail "run 1: the game changed while paused"
[[ -n "$(gtime 2 "$L1")" && "$(gtime 2 "$L1")" == "$(gtime 3 "$L1")" ]] \
	|| fail "run 1: the game clock ran while paused ($(gtime 2 "$L1") -> $(gtime 3 "$L1"))"
[[ "$(gtime 4 "$L1")" -gt "$(gtime 3 "$L1")" ]] || fail "run 1: the clock did not resume"

# ---- run 2: quit to title, New Game again ----
ARGS=(--cmd "@1 fixed_dt 33333")
tap 10 $A
tap 14 $A
ARGS+=(--cmd "@216 stats 1")
tap 220 $START
tap 222 $UP
tap 224 $A
tap 226 $UP
tap 228 $A
tap 240 $A
tap 244 $A
ARGS+=(--cmd "@446 stats 5")
"$BIN" --root "$CONTENT" --title "${ARGS[@]}" --frames 448 >"$OUT/run2.log" 2>&1 || fail "run 2 exit $?"
L2="$OUT/run2.log"
grep -qE '^menu: quit to title$' "$L2" || fail "run 2: no quit to title"
grep -qE '^mem: title same as boot ' "$L2" || fail "run 2: $(grep -E '^mem: title ' "$L2" | tr '\n' ';')"
[[ "$(grep -cE '^menu: play$' "$L2")" -eq 2 ]] || fail "run 2: the second New Game did not start"
[[ -n "$(state 1 "$L2")" && "$(state 1 "$L2")" == "$(state 5 "$L2")" ]] \
	|| fail "run 2: New Game after the quit differs from New Game at boot"
[[ "$(state 1 "$L1")" == "$(state 1 "$L2")" ]] || fail "runs 1 and 2 differ before the pause"
if [[ $FAILS -eq 0 ]]; then
	echo "menu_host PASS (pause frozen at t=$(gtime 2 "$L1") ms; quit: $(grep -E '^mem: title same' "$L2"))"
else
	echo "menu_host FAIL ($FAILS)"
	exit 1
fi
