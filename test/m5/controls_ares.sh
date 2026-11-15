#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Pad controls check on ares (docs/controls.md; run by hand, not part of
# test/run/m5.sh): an HL64_TEST ROM runs test/m5/controls.route (hldemo1,
# impulse 101, the 9mm handgun selected and fired once, then `stats
# padready`), and the patched ares' ARES_TEST_INPUT file presses the pad:
# D-up twice, D-right, D-left, Start twice, D-down (reload), each held
# until the cart logs its event. Passes when, after the padready line, the
# cart logs in this order: `flashlight: on`, `flashlight: off`, a deploy
# of another weapon (next), a deploy of the handgun (previous), `menu:
# pause` (Start), `menu: resume` (Start again) and `weapon: reload
# weapon_9mmhandgun`, with no other flashlight, deploy or menu line among
# them.
#
# Usage: test/m5/controls_ares.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content, default the c22
#        cook /scratch/hl64/tmp/Int8/c22), READY_TIMEOUT (s of real
#        time from ares' first log line until padready, default 900),
#        EVENT_TIMEOUT (s a button is held at most waiting for its event,
#        default 300), GAP (s released after each press, default 8), and
#        test/run/ares.sh's (HL64_ARES_HOST=ares-host: ares runs there, the
#        log streamed back and the pad file forwarded live; ARES_BIN,
#        ARES_SLOTS).
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int8/c22}"
READY_TIMEOUT="${READY_TIMEOUT:-900}"
EVENT_TIMEOUT="${EVENT_TIMEOUT:-300}"
GAP="${GAP:-8}"
ROMDIR="$BUILD_DIR/controls-rom"
fail() { echo "CONTROLS-ARES FAIL: $*" >&2; exit 1; }
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }

cd "$ROOT"
rm -rf "$ROMDIR" && mkdir -p "$ROMDIR"
cp -al "$CONTENT" "$ROMDIR/content"
rm -f "$ROMDIR/content/route.rbc"
python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" test/m5/controls.route \
	-o "$ROMDIR/content/route.rbc" >/dev/null || fail "routec"
make --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 HL64_CONTENT="$ROMDIR/content" rom \
	>"$ROMDIR/rom-build.log" 2>&1 || { tail -40 "$ROMDIR/rom-build.log" >&2; fail "rom build"; }
grep -E 'warning:' "$ROMDIR/rom-build.log" && fail "rom build has warnings"

# the pad file: "<buttons hex> <stick x> <stick y>", the controller word's
# bits (test/ares-testharness.patch): A 31, B 30, Z 29, Start 28, D-up 27,
# D-down 26, D-left 25, D-right 24, L 21, R 20, C-up..C-right 19-16
PAD="$ROMDIR/pad.in"
echo "0 0 0" >"$PAD"
pad() { echo "$1 0 0" >"$PAD.tmp" && mv "$PAD.tmp" "$PAD"; }
LOG="$ROMDIR/ares.log"
: >"$LOG"
# shellcheck source=../run/ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2
(
	export ARES_TEST_INPUT="$PAD"
	ares_run "$ROMDIR" hl64.z64 ares.log $((READY_TIMEOUT + 7 * (EVENT_TIMEOUT + GAP) + 600))
) &
APID=$!
XVFB_KILL='[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true'
# shellcheck disable=SC2064
trap "kill \"\$APID\" 2>/dev/null; $XVFB_KILL" EXIT
# padready: counted from ares' first log line (not the slot wait)
until [[ -s "$LOG" ]]; do
	kill -0 "$APID" 2>/dev/null || fail "ares_run ended before ares logged ($(ares_host))"
	sleep 1
done
t0=$SECONDS
until grep -q 'padready' "$LOG" 2>/dev/null; do
	kill -0 "$APID" 2>/dev/null || { tail -20 "$LOG" >&2; fail "ares exited before padready"; }
	(( SECONDS - t0 < READY_TIMEOUT )) || fail "no padready line in ${READY_TIMEOUT} s"
	sleep 1
done
echo "padready after $((SECONDS - t0)) s" >"$ROMDIR/input.log"
# press <buttons hex> <regex> <label>: held until the cart logs one more
# line matching regex than before the press (EVENT_TIMEOUT s at most: ares
# under load runs ~1 frame/s, and impulses wait out the weapon's attack
# delay), then released GAP s so the cart sees the release before the
# next press (the impulse buttons act on the press edge)
press() {
	local n0 t1
	n0=$(grep -cE "$2" "$LOG" || true)
	echo "t=$((SECONDS - t0)) $3 $1" >>"$ROMDIR/input.log"
	pad "$1"
	t1=$SECONDS
	until (( $(grep -cE "$2" "$LOG" || true) > n0 )); do
		kill -0 "$APID" 2>/dev/null || fail "ares exited during the pad input ($3)"
		(( SECONDS - t1 < EVENT_TIMEOUT )) || { pad 0; echo "t=$((SECONDS - t0)) no event" \
			>>"$ROMDIR/input.log"; break; }
		sleep 0.5
	done
	pad 0
	echo "t=$((SECONDS - t0)) released" >>"$ROMDIR/input.log"
	sleep "$GAP"
}
press 08000000 '^flashlight: on' "D-up (flashlight)"
press 08000000 '^flashlight: off' "D-up (flashlight)"
press 01000000 '^weapon: deploy ' "D-right (next weapon)"
press 02000000 '^weapon: deploy ' "D-left (previous weapon)"
press 10000000 '^menu: pause' "Start (pause)"
press 10000000 '^menu: resume' "Start (resume)"
press 04000000 '^weapon: reload ' "D-down (reload)"
echo "t=$((SECONDS - t0)) done" >>"$ROMDIR/input.log"
kill "$APID" 2>/dev/null || true	# ares_run stops ares (remote: on the host), copies back
wait "$APID" 2>/dev/null || true
# shellcheck disable=SC2064
trap "$XVFB_KILL" EXIT
tr -d '\r' <"$LOG" >"$ROMDIR/ares-clean.log"

# the cart's events after padready, in order
sed -n '/padready/,$p' "$ROMDIR/ares-clean.log" \
	| grep -E '^(flashlight: (on|off)|weapon: (deploy|reload) |menu: (pause|resume))' \
	>"$ROMDIR/events.log" || true
cat "$ROMDIR/events.log"
python3 - "$ROMDIR/events.log" "$ROMDIR/ares-clean.log" <<'EOF' || fail "pad events (see $ROMDIR/events.log)"
import re, sys
ev = [l.split(",")[0].strip() for l in open(sys.argv[1])]
log = open(sys.argv[2]).read()
pre = log[:log.index("padready")]
deploys = re.findall(r"^weapon: deploy (\S+)$", pre, re.M)
if not deploys or deploys[-1] != "weapon_9mmhandgun":
    sys.exit("the route did not leave the handgun active: %s" % deploys[-1:])
h = "weapon_9mmhandgun"
if len(ev) < 7:
    sys.exit("%d events, want 7: %s" % (len(ev), ev))
nxt = ev[2].split()[-1]
want = ["flashlight: on", "flashlight: off", "weapon: deploy " + nxt, "weapon: deploy " + h,
        "menu: pause", "menu: resume", "weapon: reload " + h]
if nxt == h or ev[:7] != want or len(ev) != 7:
    sys.exit("events %s, want %s" % (ev, want))
print("controls ares: PASS (next weapon %s)" % nxt)
EOF
