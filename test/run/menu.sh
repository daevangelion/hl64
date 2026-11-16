#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Front-end check on ares (by hand, docs/controls.md, engine/ui/menu.c): a
# playable ROM (no HL64_TEST) boots to the title menu and the patched
# ares' ARES_TEST_INPUT pad file walks it: title -> New Game -> Easy ->
# play (once the cart logs PLAY_AT, walk forward) -> Start (pause) ->
# Configuration (brightness +3 steps, sound volume -2, look speed +1) ->
# B (stored) -> Save Game -> slot 1 -> Load Game -> slot 1 (the save just
# made: `menu: load done`, play goes on) -> walk -> Start (pause) -> Start
# (resume) -> walk -> Start -> Quit to Title -> confirm, then a second
# session: New Game -> Easy -> play -> Start -> Quit to Title. Every press is
# a tap (test/ares-tap.patch): ares holds it until the cart logs the menu's
# answer (`menu: ...` lines) and releases it in that frame, so the script
# follows the cart at any emulation speed and no press auto-repeats. After
# each step that shows a screen the latest VI shot is kept by name
# (shots/<name>.ppm) and sheet.png lays them out with their names. The
# shots are ares' raw read of the framebuffer's first 216 rows (its VI
# output height with the CRT margin), so the description line (rows
# 214..232, all scanned out by the VI: plat/n64/main.c) is cut off in
# them; `menu: layout ok` proves it fits.
# PASS: every step answered; the load done; `menu: layout ok`; the second quit's title
# `mem: title same as first quit` (the first quit's against boot is
# reported: one-time allocations, eng.c title_settle); no `error:` line,
# libdragon exception or assertion text.
#
# Usage: test/run/menu.sh [ROM]   (no ROM: `make rom` of this tree on HL64_CONTENT)
# Env:   BUILD_DIR (scratch, default build), HL64_CONTENT (for the build),
#        HL64_ARES_HOST / ARES_BIN (test/run/ares.sh), LIMIT (s of ares at
#        most, default 600 remote, 3600 local), PLAY_AT (log regex, default
#        `teleport: player`), HOLD (s a screen is shown before its shot,
#        default 3), SHOT_MS (default 1500)
# Out:   $BUILD_DIR/menu/{hl64.z64,pad.in,ares.log,rom.log,input.log,vi/,
#        shots/,sheet.png}
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
PLAY_AT="${PLAY_AT:-^teleport: player}"
HOLD="${HOLD:-3}"
SHOT_MS="${SHOT_MS:-1500}"
OUT="$BUILD_DIR/menu"
fail() { echo "MENU FAIL: $*" >&2; exit 1; }

mkdir -p "$BUILD_DIR"
if [[ $# -ge 1 ]]; then
	ROM="$(realpath "$1")"
	[[ -f "$ROM" ]] || { echo "no ROM $1" >&2; exit 2; }
else
	CONTENT="${HL64_CONTENT:-}"
	[[ -f "$CONTENT/campaign.rtb" ]] || { echo "pass a ROM or set HL64_CONTENT to the cooked content" >&2; exit 2; }
	make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR/menu-rom" HL64_CONTENT="$CONTENT" rom \
		>"$BUILD_DIR/menu-rom.log" 2>&1 || { tail -30 "$BUILD_DIR/menu-rom.log" >&2; echo "rom build failed" >&2; exit 2; }
	ROM="$BUILD_DIR/menu-rom/hl64.z64"
fi
rm -rf "$OUT"
mkdir -p "$OUT/vi" "$OUT/shots"
cp "$ROM" "$OUT/hl64.z64"
PAD="$OUT/pad.in"
LOG="$OUT/ares.log"
INLOG="$OUT/input.log"
SNAPS="$OUT/snaps.txt"
# pad <buttons hex> <stick x> <stick y>: the controller word's bits
# (test/ares-testharness.patch): A 31, B 30, Z 29, Start 28, D-up..D-right
# 27-24, L 21, R 20, C-up..C-right 19-16; stick -85..85
pad() { echo "$1 $2 $3" >"$PAD.tmp" && mv "$PAD.tmp" "$PAD"; }
A=80000000 B=40000000 START=10000000 UP=08000000 DOWN=04000000 LEFT=02000000 RIGHT=01000000
pad 0 0 0
: >"$LOG"
: >"$SNAPS"

# shellcheck source=ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2
if [[ -z "${LIMIT:-}" ]]; then
	LIMIT=600
	[[ $(ares_host) != local ]] || LIMIT=3600
fi
T0=$(date +%s.%N)
el() { awk -v a="$(date +%s.%N)" -v b="$1" 'BEGIN{printf "%.1f", a - b}'; }
(
	export ARES_TEST_INPUT="$PAD" ARES_HOST_SCREENSHOT=1 ARES_HOST_SHOT_EVERY_MS="$SHOT_MS" \
		ARES_HOST_SHOT_DIR="$OUT/vi"
	ares_run "$OUT" hl64.z64 ares.log "$LIMIT"
) &
APID=$!
XVFB_KILL='[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true'
# shellcheck disable=SC2064
trap "kill \"\$APID\" 2>/dev/null; $XVFB_KILL" EXIT
echo "ares on $(ares_host), limit $LIMIT s" >"$INLOG"

STEP=start
waitfor() {	# waitfor REGEX [N]: once the log has N (default 1) such lines;
		# fails when ares ended first
	until (( $(grep -cE "$1" "$LOG" 2>/dev/null || true) >= ${2:-1} )); do
		kill -0 "$APID" 2>/dev/null || fail "ares ended at step '$STEP' (waiting for $1)"
		sleep 0.3
	done
}
TAPN=0
tap() {	# tap PAD REGEX: one press of PAD, released by ares itself in the frame
	# the cart logs one more REGEX line (test/ares-tap.patch), so the
	# menu's auto-repeat never sees it held however fast ares runs; the
	# script waits for that line, then clears the file
	local n0
	STEP="$1 -> $2"
	n0=$(grep -cE "$2" "$LOG" 2>/dev/null || true)
	TAPN=$((TAPN + 1))
	echo "$1 0 0 $TAPN $2" >"$PAD.tmp" && mv "$PAD.tmp" "$PAD"
	until (( $(grep -cE "$2" "$LOG" 2>/dev/null || true) > n0 )); do
		kill -0 "$APID" 2>/dev/null || fail "ares ended at step '$STEP'"
		sleep 0.2
	done
	pad 0 0 0
	echo "t=$(el "$T0") $1: $2" >>"$INLOG"
	sleep 0.5
}
snap() {	# snap NAME: after HOLD s, the newest VI shot is NAME's
	local s
	sleep "$HOLD"
	s=$(grep -oE '\[ares\] screenshot \S+\.ppm' "$LOG" | tail -1 | awk '{print $3}')
	echo "$1 $(basename "${s:-none}")" >>"$SNAPS"
	echo "t=$(el "$T0") snap $1 ${s:-none}" >>"$INLOG"
}
walk() {	# walk S: stick forward for S s
	pad 0 0 80
	sleep "$1"
	pad 0 0 0
	sleep 1
}

STEP=boot
waitfor '^boot ok'
waitfor '^menu: title'
snap title
tap $A '^menu: screen newgame'
snap newgame
tap $A '^menu: play'
STEP=play
waitfor "$PLAY_AT"
echo "t=$(el "$T0") $PLAY_AT" >>"$INLOG"
sleep 3
walk 4
snap play
tap $START '^menu: pause'
snap pause
tap $DOWN '^menu: select main Save Game'
tap $DOWN '^menu: select main Load Game'
tap $DOWN '^menu: select main Configuration'
tap $A '^menu: screen config'
snap config
tap $RIGHT '^menu: set '
tap $RIGHT '^menu: set '
tap $RIGHT '^menu: set '
snap brightness
tap $DOWN '^menu: select config Sound Volume'
tap $LEFT '^menu: set '
tap $LEFT '^menu: set '
tap $DOWN '^menu: select config Music Volume'
tap $LEFT '^menu: set '
tap $DOWN '^menu: select config Look Speed'
tap $RIGHT '^menu: set '
snap settings
tap $B '^menu: settings store'
tap $UP '^menu: select main Load Game'
tap $UP '^menu: select main Save Game'
tap $A '^menu: screen save'
snap save
tap $A '^menu: save (done|failed)'
snap saved
tap $DOWN '^menu: select main Load Game'
tap $A '^menu: screen load'
snap load
tap $DOWN '^menu: select load Slot 1'
tap $A '^menu: load (done|failed)'
grep -q '^menu: load done' "$LOG" || fail "loading slot 1 failed"
sleep 2
walk 3
snap loaded
tap $START '^menu: pause'
tap $START '^menu: resume'
sleep 2
walk 3
snap resumed
tap $START '^menu: pause'
tap $UP '^menu: select main Quit to Title'
tap $A '^menu: screen quit'
snap quit
tap $UP '^menu: select quit Quit to Title'
tap $A '^menu: quit to title'
STEP=title-check
waitfor '^mem: title (same as|DIFFERS from) boot'
snap title2
# the second session: New Game again, play, Quit to Title; its title must
# equal the first quit's (one-time allocations stay, nothing grows)
tap $A '^menu: screen newgame'
tap $A '^menu: play'
STEP=play2
waitfor "$PLAY_AT" 2
sleep 3
walk 3
snap play2
tap $START '^menu: pause'
tap $UP '^menu: select main Quit to Title'
tap $A '^menu: screen quit'
tap $UP '^menu: select quit Quit to Title'
tap $A '^menu: quit to title'
STEP=title-check2
waitfor '^mem: title (same as|DIFFERS from) first quit'
snap title3
echo "t=$(el "$T0") done" >>"$INLOG"
kill "$APID" 2>/dev/null || true	# ares_run stops ares (remote: on the host), copies back
wait "$APID" 2>/dev/null || true
# shellcheck disable=SC2064
trap "$XVFB_KILL" EXIT
tr -d '\r' <"$LOG" >"$OUT/rom.log"
cat "$INLOG"

python3 - "$OUT" <<'EOF'
import os, shutil, sys
out = sys.argv[1]
names = []
for l in open(os.path.join(out, "snaps.txt")):
    n, f = l.split()
    p = os.path.join(out, "vi", f)
    if os.path.isfile(p):
        dst = os.path.join(out, "shots", n + ".ppm")
        shutil.copyfile(p, dst)
        names.append((n, dst))
try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit(0)
ims = [(n, Image.open(p).convert("RGB")) for n, p in names]
if ims:
    w, h = max(i.width for _, i in ims), max(i.height for _, i in ims) + 14
    cols = 4
    sheet = Image.new("RGB", (cols * w, ((len(ims) + cols - 1) // cols) * h), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for k, (n, im) in enumerate(ims):
        x, y = (k % cols) * w, (k // cols) * h
        sheet.paste(im, (x, y + 14))
        d.text((x + 4, y + 1), n, fill=(255, 255, 0))
    sheet.save(os.path.join(out, "sheet.png"))
print("menu: %d named shots -> %s" % (len(ims), os.path.join(out, "sheet.png")))
EOF
grep -E '^(menu|mem: title|eng: )' "$OUT/rom.log" | tail -60
if grep -E '^error( \(nested\))?: |[Uu]nhandled exception|ASSERTION FAILED|[Aa]ssertion failed' \
	"$OUT/rom.log" >&2; then
	fail "error on the cart"
fi
grep -q '^menu: layout ok ' "$OUT/rom.log" || fail "menu strings do not fit the screen"
grep -q '^mem: title same as first quit' "$OUT/rom.log" \
	|| fail "the second quit's title differs from the first quit's (a leak)"
echo "menu PASS ($(el "$T0") s on $(ares_host); $OUT)"
