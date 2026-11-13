#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M2 gate (docs/architecture.md 17): collision, player move, the walk.
#
#   host:  the collision gate's host part (test/run/coll.sh: T1-T10 on the
#          GoldSrc probe fixtures), the pmove bot replay (test/m2/pmbot.py:
#          PM1-PM13 on the PM-bot fixtures) and route m2-walk on hl64-host
#          at a fixed 1/30 s frame time: `route PASS`.
#   ares:  m2-walk on a PROFILE HL64_TEST ROM at the same fixed frame time:
#          `route PASS`, the route's op log and every player stats record
#          identical to the host's (host and cart move the player bit for
#          bit); the 1 s roll-ups over the walk give the player move CPU
#          (`prof pmove_us`: per frame, with the cost of the traces and
#          point contents under it) and the frame pacing histogram.
#
# Usage: test/run/m2.sh [--ares]
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content with
#        campaign.rtb and the hldemo1 package), HL64_FIXTURES (default
#        /scratch/hl64/fixtures), ARES_BIN, ARES_TIMEOUT, DISPLAY_NUM.
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m2"
ARES=0
[[ "${1:-}" == "--ares" ]] && ARES=1
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
RTB="$CONTENT/campaign.rtb"
[[ -f "$RTB" && -f "$CONTENT/maps/campaign.cwg" ]] || { echo "no $RTB / campaign.cwg: cook the content" >&2; exit 2; }
[[ -d "$FIX/goldsrc/pmove-host/PM-bot" ]] || { echo "no PM-bot fixtures under $FIX" >&2; exit 2; }
mkdir -p "$OUT"
fail() { echo "M2 FAIL: $*" >&2; exit 1; }

# fixed frame time (think-and-time.md 1: host_fixed_dt 1/30 s in test builds)
CMDS=(--cmd "@1 fixed_dt 33333")

# ---- host ----
BUILD_DIR="$BUILD_DIR" HL64_CONTENT="$CONTENT" HL64_FIXTURES="$FIX" "$ROOT/test/run/coll.sh" \
	|| fail "collision gate (host)"
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host pmtest >"$OUT/host-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
python3 "$ROOT/test/m2/pmbot.py" --bin "$BUILD_DIR/host/hl64-pm" --content "$CONTENT" \
	--fixtures "$FIX/goldsrc" >"$OUT/pmbot.log" 2>&1 || { tail -30 "$OUT/pmbot.log" >&2; fail "pmbot"; }
tail -1 "$OUT/pmbot.log"

RBC="$OUT/m2-walk.rbc"
(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$RTB" test/routes/m2-walk.route -o "$RBC") \
	|| fail "routec m2-walk"
set +e
"$BUILD_DIR/host/hl64-host" --root "$CONTENT" --route "$RBC" "${CMDS[@]}" >"$OUT/host-walk.log" 2>&1
CODE=$?
set -e
grep -q '^route PASS$' "$OUT/host-walk.log" || { grep '^route FAIL' "$OUT/host-walk.log" >&2 || tail -20 "$OUT/host-walk.log" >&2; fail "m2-walk (host)"; }
[[ $CODE -eq 0 ]] || fail "host exit $CODE"
echo "m2-walk: route PASS (host), $(grep -c '^stats [0-9]* player:' "$OUT/host-walk.log") player records"

if [[ $ARES -eq 0 ]]; then
	echo "M2 PASS (host)"
	exit 0
fi

# ---- ares ----
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; exit 2; }

# content + route.rbc, hard-linked (no copy of the player's data)
ROMDIR="$BUILD_DIR/m2-rom"
BC="$ROMDIR/content"
rm -rf "$BC"
mkdir -p "$ROMDIR"
cp -al "$CONTENT" "$BC"
cp "$RBC" "$BC/route.rbc"
"$BUILD_DIR/host/hl64-host" --write-cmds "$ROMDIR/m2.cmd" "${CMDS[@]}"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 \
	HL64_CONTENT="$BC" HL64_CMDS="$ROMDIR/m2.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "PROFILE rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"

# shellcheck source=xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT
set +e
(cd "$ROMDIR" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
	timeout "${ARES_TIMEOUT:-600}" "$ARES_BIN" --system "Nintendo 64" \
	--setting Video/Driver=Vulkan --setting Audio/Driver=None \
	--setting Input/Driver=SDL --setting General/HomebrewMode=true \
	hl64.z64 >ares.log 2>&1)
CODE=$?
set -e
tr -d '\r' <"$ROMDIR/ares.log" >"$OUT/ares-walk.log"
grep -q '^route PASS$' "$OUT/ares-walk.log" || { grep '^route FAIL' "$OUT/ares-walk.log" >&2 || tail -30 "$OUT/ares-walk.log" >&2; fail "m2-walk (ares)"; }
[[ $CODE -eq 0 ]] || fail "ares exit $CODE"
grep -q '^TESTHARNESS exit 0' "$OUT/ares-walk.log" || fail "no harness exit 0"
walk() { grep -E '^route: [0-9]|^stats [0-9]+ player: |^player: ' "$1"; }
diff <(walk "$OUT/host-walk.log") <(walk "$OUT/ares-walk.log") \
	|| fail "the cart's route ops / player positions differ from the host's"
echo "m2-walk: route PASS (ares), route ops and player records identical to the host"

# roll-ups over the walk (the first window holds boot and the layer open)
grep '^prof pmove_us' "$OUT/ares-walk.log" | tail -n +2 >"$OUT/prof-pmove.log"
grep '^prof frames=' "$OUT/ares-walk.log" | tail -n +2 >"$OUT/prof-frames.log"
[[ -s "$OUT/prof-pmove.log" ]] || fail "no pmove_us roll-ups from ares"
cat "$OUT/prof-pmove.log"
python3 - "$OUT/prof-pmove.log" "$OUT/prof-frames.log" "$OUT/ares-walk.log" <<'EOF'
import re, sys
kv = lambda l: {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', l)}
p = [kv(l) for l in open(sys.argv[1])]
n = sum(r['frames'] for r in p)
avg = lambda k: sum(r[k] * r['frames'] for r in p) / n
print("pmove CPU (ares, %d frames): mean %.0f us/frame, max %d us; traces %.0f us (%.1f/frame, "
      "%.1f clips), contents %.0f us (%.1f/frame), physents max %d"
      % (n, avg('avg'), max(r['max'] for r in p), avg('trace_us'), avg('traces'), avg('clips'),
         avg('contents_us'), avg('contents'), max(r['physents_max'] for r in p)))
f = [kv(l) for l in open(sys.argv[2])]
fr = sum(r['frames'] for r in f)
mean = sum(r['frame_us'] * r['frames'] for r in f) / max(fr, 1)
# Shown-frame intervals: every stats record and roll-up line takes the
# histogram since the previous one, so the walk's histogram is their sum
# from the route's first SETPOS on.
h, on = None, False
for l in open(sys.argv[3]):
    on = on or l.startswith('route: 1 ')
    m = re.search(r'pace .* v1\.\.v\d+\+=([\d/]+)', l)
    if on and m:
        b = [int(x) for x in m.group(1).split('/')]
        h = b if h is None else [x + y for x, y in zip(h, b)]
print("frame pacing (ares): mean frame %.0f us over %d frames; shown-frame intervals over the "
      "walk, in vblanks: %s" % (mean, fr, " ".join(
          "v%d%s=%d" % (i + 1, "+" if i == len(h) - 1 else "", c) for i, c in enumerate(h))))
EOF
echo "M2 PASS (host + ares)"
