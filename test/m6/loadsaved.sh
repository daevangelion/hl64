#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# player_loadsaved host test (game/hl/env.c CRevertSaved, engine/core/eng.c
# eng_reload, docs/architecture.md 14 Reload), a suite of test/run/m5.sh:
# hl64-host at the fixed 1/30 s, each run on its own FlashRAM image.
#  run 1: an erased chip, test/m6/loadsaved-kill.route (the dome scientist
#         shot dead -> player_loadsaved): the title GAMEOVER shows, the
#         reload finds no save (`eng: reload: no save to load, New Game on
#         hldemo1`) and New Game starts on hldemo1: the player is back at
#         its start (the New Game spawn of a fresh boot), route PASS.
#  run 2: test/m6/loadsaved-autosave.route on a second erased chip (the
#         autosave at trigger_autosave #257), then loadsaved-kill.route on
#         that chip: the reload loads the autosave (`eng: reload: slot 0
#         (... autosave, hldemo1)`; another route's save: the route goes
#         on), the player is where the autosave was taken, route PASS.
# Usage: test/m6/loadsaved.sh HOST_BIN CONTENT OUTDIR (from the tree root)
# Exit:  0 pass (last line `loadsaved PASS`), 1 failure.
set -euo pipefail

BIN="$1"
CONTENT="$2"
OUT="$3"
mkdir -p "$OUT"
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
for r in autosave kill; do
	python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "test/m6/loadsaved-$r.route" \
		-o "$OUT/$r.rbc" >/dev/null
done
# run <log> <route> <flash>
run() {
	local code=0
	"$BIN" --root "$CONTENT" --route "$OUT/$2.rbc" --save "$OUT/$3" --cmd "@1 fixed_dt 33333" \
		>"$OUT/$1" 2>&1 || code=$?
	[[ $code -eq 0 ]] && grep -q '^route PASS$' "$OUT/$1" || fail "$1: exit $code, no route PASS"
}
need() { grep -qE "$2" "$OUT/$1" || fail "$1: no line /$2/"; }
# the player's origin in the stats record `reloaded` (the last player line)
org() { grep -E '^stats [0-9]+ player: map=' "$OUT/$1" | tail -1 | sed -E 's/.* org=([^ ]+) .*/\1/'; }
near() {	# near <log> <x,y,z> <r>
	python3 - "$(org "$1")" "$2" "$3" <<'EOF' || fail "$1: player at $(org "$1"), not within $3 u of $2"
import sys
a = [float(v) for v in sys.argv[1].split(",")]
b = [float(v) for v in sys.argv[2].split(",")]
sys.exit(0 if sum((p - q) ** 2 for p, q in zip(a, b)) ** 0.5 <= float(sys.argv[3]) else 1)
EOF
}

rm -f "$OUT"/*.fla
# ---- run 1: no save ----
run none.log kill none.fla
need none.log '^player_loadsaved: map [0-9]+ lump [0-9]+: reload$'
# the GAMEOVER title (titles.txt: "SUBJECT:  FREEMAN ... EVALUATION TERMINATED")
need none.log '^message: title [0-9]+ SUBJECT:  FREEMAN'
need none.log '^eng: reload: no save to load, New Game on hldemo1$'
if grep -qE '^eng: reload: slot ' "$OUT/none.log"; then fail "none.log: a save loaded"; fi
# New Game's spawn: `stats start`, the first player line (fresh boot; the
# spawn is 4 u above the floor it drops to)
start="$(grep -E '^stats [0-9]+ player: map=hldemo1 ' "$OUT/none.log" | head -1 \
	| sed -E 's/.* org=([^ ]+) .*/\1/')"
if [[ -z "$start" || "$(grep -cE '^stats [0-9]+ player: map=hldemo1 ' "$OUT/none.log")" -ne 2 ]]; then
	fail "none.log: not two player lines"
else
	near none.log "$start" 8
fi
# ---- run 2: the autosave, then the reload ----
run autosave.log autosave save.fla
need autosave.log '^save: save slot 0 ok'
run save.log kill save.fla
need save.log '^eng: reload: slot 0 \(seq [0-9]+, autosave, hldemo1\)$'
need save.log '^route: the save was made by another route: not resumed$'
need save.log '^save: load slot 0 ok'
near save.log -1652,-160,36 1
if [[ $FAILS -eq 0 ]]; then
	echo "loadsaved PASS: no save -> New Game on hldemo1 (player $(org none.log)), autosave" \
		"-> reload at $(org save.log)"
else
	echo "loadsaved FAIL ($FAILS)"
	exit 1
fi
