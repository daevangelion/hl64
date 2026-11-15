#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Playable-ROM boot-and-play check (by hand, before delivering a ROM): ares
# (test/run/ares.sh: here, or on HL64_ARES_HOST=ares-host in about 2 min) boots
# a playable ROM (no HL64_TEST) to its title menu; the pad picks New Game,
# Easy (A, A: each pressed until the cart logs the menu's answer). Once
# the cart logs PLAY_AT (default the intro's `teleport: player`:
# hldemo1's title room hands the player to the corridor) and PLAY_DELAY s
# more have passed, the patched ares' ARES_TEST_INPUT pad file plays a
# fixed schedule (host seconds, 42 s):
# walk forward, turn right, strafe right (C-right), walk forward turning
# left, look up (C-up), walk back; TAIL s later ares is stopped. The VI
# framebuffer is saved every SHOT_MS ms of host time.
# PASS: `boot ok` and PLAY_AT logged; ares still running when the schedule
# ended; the cart saw the pad (`TESTHARNESS inject` lines); no `error:`
# line, libdragon exception or assertion text; the shots taken after the
# first press show at least 2 different frames (sheet.png: up to 20 shots).
#
# Usage: test/run/play.sh [ROM]   (no ROM: `make rom` of this tree on HL64_CONTENT)
# Env:   BUILD_DIR (scratch, default build), HL64_CONTENT (cooked content, for
#        the build), HL64_ARES_HOST / ARES_BIN (test/run/ares.sh), LIMIT (s
#        of ares at most, default 300 remote, 2400 local: a loaded
#        build-vm had not reached the teleport after 900 s), PLAY_AT
#        (log regex), PLAY_DELAY (s, default 3), TAIL (s, default 6),
#        SHOT_MS (default 4000), PLAY_TITLE (1 default: through the title
#        menu; 0: a ROM that boots straight into the game)
# Out:   $BUILD_DIR/play/{hl64.z64,pad.in,ares.log,rom.log,input.log,vi/,
#        sheet.png}
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
PLAY_AT="${PLAY_AT:-^teleport: player}"
PLAY_DELAY="${PLAY_DELAY:-3}"
TAIL="${TAIL:-6}"
SHOT_MS="${SHOT_MS:-4000}"
PLAY_TITLE="${PLAY_TITLE:-1}"
OUT="$BUILD_DIR/play"
fail() { echo "PLAY FAIL: $*" >&2; exit 1; }

mkdir -p "$BUILD_DIR"
if [[ $# -ge 1 ]]; then
	ROM="$(realpath "$1")"
	[[ -f "$ROM" ]] || { echo "no ROM $1" >&2; exit 2; }
else
	CONTENT="${HL64_CONTENT:-}"
	[[ -f "$CONTENT/campaign.rtb" ]] || { echo "pass a ROM or set HL64_CONTENT to the cooked content" >&2; exit 2; }
	make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR/play-rom" HL64_CONTENT="$CONTENT" rom \
		>"$BUILD_DIR/play-rom.log" 2>&1 || { tail -30 "$BUILD_DIR/play-rom.log" >&2; echo "rom build failed" >&2; exit 2; }
	ROM="$BUILD_DIR/play-rom/hl64.z64"
fi
rm -rf "$OUT"
mkdir -p "$OUT/vi"
cp "$ROM" "$OUT/hl64.z64"
PAD="$OUT/pad.in"
LOG="$OUT/ares.log"
INLOG="$OUT/input.log"
# pad <buttons hex> <stick x> <stick y>: the controller word's bits
# (test/ares-testharness.patch): A 31, B 30, Z 29, Start 28, D-up..D-right
# 27-24, L 21, R 20, C-up..C-right 19-16; stick -85..85
pad() { echo "$1 $2 $3" >"$PAD.tmp" && mv "$PAD.tmp" "$PAD"; }
pad 0 0 0
: >"$LOG"

# shellcheck source=ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2
if [[ -z "${LIMIT:-}" ]]; then
	LIMIT=300
	[[ $(ares_host) != local ]] || LIMIT=2400
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

waitfor() {	# waitfor REGEX: 0 once the log has it, 1 when ares ended first
	until grep -qE "$1" "$LOG" 2>/dev/null; do
		kill -0 "$APID" 2>/dev/null || return 1
		sleep 0.5
	done
}
tap() {	# tap PAD REGEX: hold PAD until the log has one more REGEX line
	local n0
	n0=$(grep -cE "$2" "$LOG" 2>/dev/null || true)
	pad "$1" 0 0
	until (( $(grep -cE "$2" "$LOG" 2>/dev/null || true) > n0 )); do
		kill -0 "$APID" 2>/dev/null || { pad 0 0 0; return 1; }
		sleep 0.2
	done
	pad 0 0 0
	sleep 0.5
}
BOOT=-
AT=-
if waitfor '^boot ok'; then
	BOOT=$(el "$T0")
	echo "t=$BOOT boot ok" >>"$INLOG"
	# the title menu: New Game, Easy
	if [[ $PLAY_TITLE -eq 1 ]] && waitfor '^menu: title' &&
		tap 80000000 '^menu: screen newgame' && tap 80000000 '^menu: play'; then
		echo "t=$(el "$T0") title: New Game, Easy" >>"$INLOG"
	fi
	if waitfor "$PLAY_AT"; then
		AT=$(el "$T0")
		echo "t=$AT $PLAY_AT" >>"$INLOG"
	fi
fi
sleep "$PLAY_DELAY"
T1=$(date +%s.%N)
# <s after the start> <buttons hex> <stick x> <stick y> <label>
SCHED=(
	"0 0 0 80 stick-forward"
	"8 0 0 0 idle"
	"10 0 60 0 stick-right-turn"
	"14 0 0 0 idle"
	"16 00010000 0 0 c-right-strafe"
	"21 0 0 0 idle"
	"23 0 -60 80 forward+left-turn"
	"31 0 0 0 idle"
	"33 00080000 0 0 c-up-look"
	"36 0 0 -80 stick-back"
	"42 0 0 0 idle"
)
for e in "${SCHED[@]}"; do
	read -r t b x y l <<<"$e"
	while kill -0 "$APID" 2>/dev/null; do
		awk -v a="$(date +%s.%N)" -v b="$T1" -v t="$t" 'BEGIN{exit !(a - b >= t)}' && break
		sleep 0.2
	done
	kill -0 "$APID" 2>/dev/null || break
	pad "$b" "$x" "$y"
	echo "t=$(el "$T0") $l: $b $x $y" >>"$INLOG"
done
sleep "$TAIL"
PLAYED=0
if kill -0 "$APID" 2>/dev/null; then
	PLAYED=1
	kill "$APID"	# ares_run stops ares (remote: on the host) and copies back
fi
CODE=0
wait "$APID" || CODE=$?
# shellcheck disable=SC2064
trap "$XVFB_KILL" EXIT
echo "t=$(el "$T0") ares exit $CODE$([[ $PLAYED -eq 1 ]] && echo ' (stopped after the schedule)')" >>"$INLOG"
tr -d '\r' <"$LOG" >"$OUT/rom.log"
cat "$INLOG"

# shots after the first press (the log names them in order); a contact
# sheet of those (else of all shots)
read -r NSHOT NPLAY NDIST < <(python3 - "$OUT" <<'EOF'
import hashlib, os, re, sys
out = sys.argv[1]
vi = os.path.join(out, "vi")
seen, play = False, []
for l in open(os.path.join(out, "rom.log"), errors="replace"):
    if l.startswith("TESTHARNESS inject"):
        seen = True
    m = re.match(r"\[ares\] screenshot (\S+\.ppm) ", l)
    if m and seen:
        p = os.path.join(vi, os.path.basename(m.group(1)))
        if os.path.isfile(p):
            play.append(p)
shots = sorted(f for f in os.listdir(vi) if f.endswith(".ppm"))
dist = {hashlib.md5(open(p, "rb").read()).hexdigest() for p in play}
try:
    from PIL import Image
    ims = []
    for p in play or [os.path.join(vi, f) for f in shots]:
        try:
            im = Image.open(p)
        except OSError:
            continue
        if im.width >= 64:
            ims.append(im.convert("RGB"))
    if ims:
        step = max(1, len(ims) // 20)
        ims = ims[::step][:20]
        w, h = max(i.width for i in ims), max(i.height for i in ims)
        sheet = Image.new("RGB", (5 * w, ((len(ims) + 4) // 5) * h))
        for k, im in enumerate(ims):
            sheet.paste(im, ((k % 5) * w, (k // 5) * h))
        sheet.save(os.path.join(out, "sheet.png"))
except ImportError:
    pass
print(len(shots), len(play), len(dist))
EOF
)
INJ=$(grep -c '^TESTHARNESS inject' "$OUT/rom.log" || true)
echo "play: boot ok at t=$BOOT s, play from t=$AT s, $INJ pad changes seen by the cart, $NSHOT shots" \
	"($NPLAY after the first press, $NDIST different), ares exit $CODE, $(el "$T0") s in all on" \
	"$(ares_host); $OUT"
grep -q '^boot ok' "$OUT/rom.log" || fail "no 'boot ok'"
if grep -E '^error( \(nested\))?: |[Uu]nhandled exception|ASSERTION FAILED|[Aa]ssertion failed' \
	"$OUT/rom.log" >&2; then
	fail "error on the cart"
fi
[[ $AT != - ]] || fail "no '$PLAY_AT' line"
[[ $PLAYED -eq 1 ]] || fail "ares exited (status $CODE) before the schedule ended"
[[ $INJ -gt 0 ]] || fail "the cart never saw the pad"
[[ $NDIST -ge 2 ]] || fail "$NDIST different shots while playing"
echo "play PASS"
