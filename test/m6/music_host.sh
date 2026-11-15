#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# CD music host test (docs/specs/sound.md 6, S9), a suite of
# test/run/m5.sh: hl64-host at the fixed 1/30 s over a cook with the
# Uplink tracks (media/cd02.wav64, media/cd03.wav64).
#  run 1: New Game on hldemo1: hldemo_init_mm fires target_cdaudio
#         start_cd_audio at once: track 3 starts in the first two seconds
#         of the map and plays on; `set vol_music 50` gives the music voice
#         0.50.
#  run 2: hldemo3 (test/scenes/hl3-welder.route): the dish button fires
#         dish_mm, whose dome_cd_track (+1.5 s) plays track 2; `music 3`
#         cuts it; `music 0` stops it.
#  run 3: title, `music 2` at 0.1 s per frame (HL64_DT_MAX_US): the track
#         ends at its length (131.9 s) and its stream closes.
#  run 4: title -> New Game (track 3 starts) -> Quit to Title: the music
#         stops with the session and the title is the boot title's.
# Usage: test/m6/music_host.sh HOST_BIN CONTENT OUTDIR [ROUTEC_RTB]
# Exit:  0 pass (last line `music_host PASS`), 1 failure, 2 prerequisite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$1"
CONTENT="$2"
OUT="$3"
mkdir -p "$OUT"
for f in media/cd02.wav64 media/cd03.wav64 campaign.rtb; do
	[[ -f "$CONTENT/$f" ]] || { echo "music_host: no $CONTENT/$f (re-cook)" >&2; exit 2; }
done
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
has() { grep -qE "$2" "$1" || fail "$3: no line /$2/"; }
# frame of the first line matching $2
at() { { grep -m1 -E "$2" "$1" || true; } | sed -E 's/^snd ([0-9]+) .*/\1/'; }
route() {
	(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "$1" -o "$2") \
		>"$2.log" 2>&1 || { cat "$2.log"; echo "music_host: routec $1 failed" >&2; exit 2; }
}

# ---- run 1: hldemo1's start plays track 3; music volume ----
cat >"$OUT/r1.route" <<'EOF'
startmap hldemo1
waitmap hldemo1 600
wait 8
pass
EOF
route "$OUT/r1.route" "$OUT/r1.rbc"
"$BIN" --root "$CONTENT" --route "$OUT/r1.rbc" --cmd "@1 fixed_dt 33333" \
	--cmd "@120 set vol_music 50" >"$OUT/run1.log" 2>&1 || fail "run 1 exit $?"
L="$OUT/run1.log"
has "$L" '^snd: music: 2 tracks on the cart, voice sized on track 2 ' "run 1"
has "$L" '^snd [0-9]+ music 3 start 22050 Hz 2 ch [0-9]+ samples$' "run 1"
has "$L" '^snd [0-9]+ music 3 vol 0\.50$' "run 1"
s=$(at "$L" ' music 3 start ')
[[ -n "$s" && "$s" -le 60 ]] || fail "run 1: track 3 started at snd frame '${s:-none}' (want <= 60)"
grep -qE ' music 3 (stop|end|cut)$' "$L" && fail "run 1: track 3 stopped early"

# ---- run 2: hldemo3's dish button fires dish_mm -> dome_cd_track (+1.5 s): track 2
# (test/scenes/hl3-welder.route); `music 3` cuts it; `music 0` stops it ----
route "$ROOT/test/scenes/hl3-welder.route" "$OUT/r2.rbc"
"$BIN" --root "$CONTENT" --route "$OUT/r2.rbc" --cmd "@1 fixed_dt 33333" >"$OUT/run2a.log" 2>&1 \
	|| fail "run 2 exit $?"
has "$OUT/run2a.log" '^snd [0-9]+ music 2 start 22050 Hz 2 ch ' "run 2"
s=$(at "$OUT/run2a.log" ' music 2 start ')
if [[ -n "$s" ]]; then
	"$BIN" --root "$CONTENT" --route "$OUT/r2.rbc" --cmd "@1 fixed_dt 33333" \
		--cmd "@$((s + 10)) music 3" --cmd "@$((s + 20)) music 0" >"$OUT/run2.log" 2>&1 \
		|| fail "run 2 (cut) exit $?"
	has "$OUT/run2.log" '^snd [0-9]+ music 2 cut$' "run 2"
	has "$OUT/run2.log" '^snd [0-9]+ music 3 start ' "run 2"
	has "$OUT/run2.log" '^snd [0-9]+ music 3 stop$' "run 2"
fi

# ---- run 3: a track ends at its length ----
"$BIN" --root "$CONTENT" --title --cmd "@1 fixed_dt 100000" --cmd "@2 music 2" --frames 1400 \
	>"$OUT/run3.log" 2>&1 || fail "run 3 exit $?"
has "$OUT/run3.log" '^snd [0-9]+ music 2 start ' "run 3"
has "$OUT/run3.log" '^snd [0-9]+ music 2 end$' "run 3"
s=$(at "$OUT/run3.log" ' music 2 start '); e=$(at "$OUT/run3.log" ' music 2 end$')
[[ -n "$s" && -n "$e" && $((e - s)) -ge 1319 && $((e - s)) -le 1322 ]] \
	|| fail "run 3: track 2 (131.9 s) ended after $((${e:-0} - ${s:-0})) frames of 0.1 s"

# ---- run 4: the session's end stops the music ----
# pad bits (engine/core/sys.h SYS_PAD_*); taps as test/m6/menu_host.sh run 2
A=1 START=8 UP=1024
ARGS=(--cmd "@1 fixed_dt 33333")
tap() { ARGS+=(--cmd "@$1 pad $2 0 0" --cmd "@$(($1 + 1)) pad 0 0 0"); }
tap 10 $A
tap 14 $A
tap 220 $START
tap 222 $UP
tap 224 $A
tap 226 $UP
tap 228 $A
"$BIN" --root "$CONTENT" --title "${ARGS[@]}" --frames 300 >"$OUT/run4.log" 2>&1 || fail "run 4 exit $?"
has "$OUT/run4.log" '^snd [0-9]+ music 3 start ' "run 4"
has "$OUT/run4.log" '^menu: quit to title$' "run 4"
has "$OUT/run4.log" '^snd [0-9]+ music 3 stop$' "run 4"
has "$OUT/run4.log" '^mem: title same as boot ' "run 4"

if [[ $FAILS -eq 0 ]]; then
	echo "music_host PASS (hldemo1 track 3 at snd frame $(at "$L" ' music 3 start '), hldemo3" \
		"track 2, cut, stop, end after $((e - s)) frames of 0.1 s, session end)"
else
	echo "music_host FAIL ($FAILS)"
	exit 1
fi
