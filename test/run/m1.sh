#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M1 gate (docs/architecture.md 17): the world renderer.
#
#   host:  build, unit tests (W4 runtime half, L1-L3, D1-D2), the three
#          hldemo1 poses on the host; the draw-list recorder's counts must
#          equal the cook-side prediction (test/m1/predict.py) at every pose;
#          the cook's world-surfaces tests (W1-W5, tools/w64c).
#   ares:  the same poses on an HL64_TEST ROM: SSIM >= 0.95 against the old
#          cart's world-only shots of the same poses (masks: HUD, harness
#          bars, the dome scientist), the cart's counts equal the prediction
#          too; a PROFILE ROM holds the dome pose: world CPU <= 2000 us
#          (mean of the 1 s roll-ups at the pose).
#
# Poses are the old cart's eye positions (black-box capture of the old
# cart: setpos origin + view height 28 + its 1/32 offset). The old cart's
# projection, measured at the dome pose (its world pass passes fov_y
# 61.9275 degrees to tiny3d over a 320x240 viewport: projection m00 1.25,
# m11 1.6667), is narrower than GoldSrc's 90 degrees; at 4:3 that is a
# horizontal fov of 2 atan(0.8) = 77.3196. The gate sets fov to that so
# both draw the same frustum; hl64's default stays 90.
#
# Usage: test/run/m1.sh [--ares]
# Env:   BUILD_DIR (scratch, e.g. /scratch/...), HL64_CONTENT (cooked
#        content with maps/hldemo1.w64 + maps/campaign.cwg; cook with
#        HL64_REF_PALETTE=<old id1/gfx/palette.lmp> for the SSIM gate),
#        M1_REF (old-cart shots start/dome/elevator-B.png), HL64_DATA and
#        W1_FIXTURES for the cook's W1, ARES_BIN, ARES_TIMEOUT, DISPLAY_NUM.
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m1"
ARES=0
[[ "${1:-}" == "--ares" ]] && ARES=1
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
M1_REF="${M1_REF:-}"
FOV=77.3196
W64="$CONTENT/maps/hldemo1.w64"
CWG="$CONTENT/maps/campaign.cwg"
[[ -f "$W64" && -f "$CWG" ]] || { echo "no $W64 / $CWG: cook the content" >&2; exit 2; }

mkdir -p "$OUT"
fail() { echo "M1 FAIL: $*" >&2; exit 1; }
FAILS=0
soft() { echo "M1 FAIL: $*" >&2; FAILS=$((FAILS + 1)); }

# map index of hldemo1 in the campaign table
MAP="$(python3 - "$CWG" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
n, nl, nk = struct.unpack_from('>3H', b, 8)
ns, nt, npl = struct.unpack_from('>3H', b, 20)
so = 32 + 48 * n + 16 * nl + 8 * nk + 136 * ns + 4 * nt + 2 * npl
for i in range(n):
    s = struct.unpack_from('>H', b, 32 + 48 * i)[0]
    if b[so + s:b.index(b'\0', so + s)] == b'hldemo1':
        print(i)
EOF
)"
[[ -n "$MAP" ]] || fail "hldemo1 not in $CWG"

# pose name, eye (map-local), pitch yaw roll
POSES=(
	"start -2047.96875 744.03125 64.03125 0 270 0"
	"dome -1999.96875 -619.96875 224.03125 0 270 0"
	"elevator -2499.96875 -99.96875 224.03125 0 0 0"
)
# Commands: fov, then per pose setpos, shot after 35 frames, stats.
CMDS=(--cmd "@2 set fov $FOV")
f=5
i=1
for p in "${POSES[@]}"; do
	read -r name x y z pi ya ro <<<"$p"
	CMDS+=(--cmd "@$f setpos $MAP $x $y $z $pi $ya $ro" --cmd "@$((f + 35)) shot $i"
		--cmd "@$((f + 36)) stats $i")
	f=$((f + 40))
	i=$((i + 1))
done
CMDS+=(--cmd "@$f quit 0")

# ---- host ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host unit >"$OUT/host-build.log" 2>&1 \
	|| { cat "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
"$BUILD_DIR/host/hl64-unit" | tee "$OUT/unit.log" | tail -1
grep -q '^unit: PASS' "$OUT/unit.log" || soft "unit tests"
set +e
"$BUILD_DIR/host/hl64-host" --root "$CONTENT" "${CMDS[@]}" >"$OUT/host.log" 2>&1
CODE=$?
set -e
[[ $CODE -eq 0 ]] || { tail -20 "$OUT/host.log" >&2; fail "host exit $CODE"; }
python3 "$ROOT/test/m1/predict.py" "$W64" "$CWG" "$OUT/host.log" --fov "$FOV" | tee "$OUT/predict-host.log"
grep -q '^predict: PASS' "$OUT/predict-host.log" || soft "host recorder counts differ from the prediction"

# cook tests W1-W5 (world-surfaces.md 5): tools/w64c
if (cd "$ROOT" && python3 -c 'import tools.w64c.test_w64c') 2>/dev/null; then
	(cd "$ROOT" && python3 -m unittest tools.w64c.test_w64c) >"$OUT/w2-w5.log" 2>&1 \
		|| { tail -20 "$OUT/w2-w5.log" >&2; soft "cook tests W2-W5"; }
	echo "cook W2-W5: $(tail -1 "$OUT/w2-w5.log")"
else
	soft "cook tests W2-W5 (tools.w64c.test_w64c) not present"
fi
if [[ -n "${W1_FIXTURES:-}" && -n "${HL64_DATA:-}" ]]; then
	make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" "$BUILD_DIR/tools/hlecook" >/dev/null
	(cd "$ROOT" && python3 -m tools.w64c.w1 --fixtures "$W1_FIXTURES" --data "$HL64_DATA" \
		--hlecook "$BUILD_DIR/tools/hlecook") >"$OUT/w1.log" 2>&1 \
		|| { tail -20 "$OUT/w1.log" >&2; soft "cook test W1"; }
	echo "cook W1: $(tail -1 "$OUT/w1.log")"
else
	soft "cook test W1 not run (set W1_FIXTURES and HL64_DATA)"
fi

if [[ $ARES -eq 0 ]]; then
	[[ $FAILS -eq 0 ]] || fail "$FAILS host check(s)"
	echo "M1 PASS (host)"
	exit 0
fi

# ---- ares ----
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; exit 2; }
[[ -n "$M1_REF" && -f "$M1_REF/dome-B.png" ]] || { echo "set M1_REF to the old-cart shots" >&2; exit 2; }

# shellcheck source=xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT

# run_rom DIR: ares on DIR/hl64.z64 in DIR, log DIR/ares-clean.log
run_rom() {
	local dir="$1" code
	rm -f "$dir"/ares-test-*.ppm
	set +e
	(cd "$dir" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
		timeout "${ARES_TIMEOUT:-600}" "$ARES_BIN" --system "Nintendo 64" \
		--setting Video/Driver=Vulkan --setting Audio/Driver=None \
		--setting Input/Driver=SDL --setting General/HomebrewMode=true \
		hl64.z64 >ares.log 2>&1)
	code=$?
	set -e
	tr -d '\r' <"$dir/ares.log" >"$dir/ares-clean.log"
	[[ $code -eq 0 ]] || { tail -30 "$dir/ares-clean.log" >&2; fail "ares exit $code ($dir)"; }
	grep -q '^TESTHARNESS exit 0' "$dir/ares-clean.log" || fail "no harness exit 0 ($dir)"
}

# shots
ROMDIR="$BUILD_DIR/m1-rom"
mkdir -p "$ROMDIR"
"$BUILD_DIR/host/hl64-host" --write-cmds "$ROMDIR/m1.cmd" "${CMDS[@]}"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 HL64_CONTENT="$CONTENT" \
	HL64_CMDS="$ROMDIR/m1.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"
run_rom "$ROMDIR"
python3 "$ROOT/test/m1/predict.py" "$W64" "$CWG" "$ROMDIR/ares-clean.log" --fov "$FOV" \
	| tee "$OUT/predict-cart.log"
grep -q '^predict: PASS' "$OUT/predict-cart.log" || soft "cart counts differ from the prediction"
# masks (inclusive): harness bars, `say:` prompt, HUD health/suit, HUD
# flashlight icon; dome: the scientist
MASK=(--mask 0,0,319,3 --mask 8,0,47,7 --mask 11,216,115,229 --mask 295,11,309,19)
i=0
for p in "${POSES[@]}"; do
	read -r name _ <<<"$p"
	extra=()
	[[ $name == dome ]] && extra=(--mask 69,113,93,175)
	ppm="$ROMDIR/ares-test-$(printf %04d $i).ppm"
	[[ -f "$ppm" ]] || fail "no screenshot for $name"
	s="$(python3 "$ROOT/test/m1/ssim.py" "$ppm" "$M1_REF/$name-B.png" "${MASK[@]}" "${extra[@]}" \
		--diff "$OUT/diff-$name.png" | awk '{print $2}')"
	echo "ssim $name $s"
	python3 -c "import sys; sys.exit(0 if float('$s') >= 0.95 else 1)" || soft "SSIM $name $s < 0.95"
	cp "$ppm" "$OUT/$name.ppm"
	i=$((i + 1))
done

# world CPU on the dome pose (PROFILE): hold the pose 10 s, roll-ups each second
PROFDIR="$BUILD_DIR/m1-prof"
mkdir -p "$PROFDIR"
read -r _ x y z pi ya ro <<<"${POSES[1]}"
"$BUILD_DIR/host/hl64-host" --write-cmds "$PROFDIR/m1.cmd" --cmd "@2 set fov $FOV" \
	--cmd "@3 setpos $MAP $x $y $z $pi $ya $ro" --cmd "@60 prof 1" --cmd "@360 quit 0"
make -C "$ROOT" --no-print-directory BUILD_DIR="$PROFDIR" HL64_TEST=1 PROFILE=1 \
	HL64_CONTENT="$CONTENT" HL64_CMDS="$PROFDIR/m1.cmd" rom >"$OUT/prof-build.log" 2>&1 \
	|| { tail -40 "$OUT/prof-build.log" >&2; fail "PROFILE rom build"; }
run_rom "$PROFDIR"
grep '^prof world_us' "$PROFDIR/ares-clean.log" | tail -n +2 | tee "$OUT/prof-world.log"
avg="$(awk '{split($3, a, "="); split($5, f, "="); s += a[2] * f[2]; n += f[2]} END {if (n) printf "%d", s / n}' "$OUT/prof-world.log")"
[[ -n "$avg" ]] || fail "no world_us roll-ups"
echo "dome world CPU: mean $avg us (budget 2000)"
[[ $avg -le 2000 ]] || soft "dome world CPU $avg us > 2000"

[[ $FAILS -eq 0 ]] || fail "$FAILS check(s)"
echo "M1 PASS (host + ares)"
