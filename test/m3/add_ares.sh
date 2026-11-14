#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Additive cart check (ares; docs/specs/render-fx.md 2a, 3): an HL64_TEST
# ROM over cooked content runs test/routes/addblend.route (hldemo1 glows
# over lit walls, a red glow, an MP5 grenade's smoke). Passes when the
# route passes, the harness exits 0, every glow shot's core is free of the
# RDP adder's wrap (test/m3/add_shot.py: darker than its surroundings or
# a false colour), and no page was drawn additive without its cooked ramp
# (`add: miss=0` on the cart and on the host run of the same route). The
# shots are left as PNG in $BUILD_DIR/m3-add-rom/shots for viewing (the
# smoke ones are viewed, not checked).
#
# Usage: test/m3/add_ares.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content), ARES_BIN,
#        ARES_TIMEOUT.
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
ROUTE="$ROOT/test/routes/addblend.route"
ROMDIR="$BUILD_DIR/m3-add-rom"
fail() { echo "ADD-ARES FAIL: $*" >&2; exit 1; }
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN" >&2; exit 2; }

cd "$ROOT"
make --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$BUILD_DIR/add-host-build.log" 2>&1 \
	|| { tail -30 "$BUILD_DIR/add-host-build.log" >&2; fail "host build"; }
rm -rf "$ROMDIR" && mkdir -p "$ROMDIR/shots"
cp -al "$CONTENT" "$ROMDIR/content"
rm -f "$ROMDIR/content/route.rbc"
python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "$ROUTE" -o "$ROMDIR/content/route.rbc" \
	>/dev/null || fail "routec"

# the host run of the route: the same ramp binds and misses as the cart
"$BUILD_DIR/host/hl64-host" --root "$ROMDIR/content" --route "$ROMDIR/content/route.rbc" \
	>"$ROMDIR/host.log" 2>&1 || { tail -20 "$ROMDIR/host.log" >&2; fail "host route run"; }
grep -q '^route PASS$' "$ROMDIR/host.log" || fail "host: no route PASS"

make --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 HL64_CONTENT="$ROMDIR/content" rom \
	>"$ROMDIR/rom-build.log" 2>&1 || { tail -40 "$ROMDIR/rom-build.log" >&2; fail "rom build"; }
grep -E 'warning:' "$ROMDIR/rom-build.log" && fail "rom build has warnings"

# shellcheck source=../run/xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT
rm -f "$ROMDIR"/ares-test-*.ppm
set +e
(cd "$ROMDIR" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
	timeout "${ARES_TIMEOUT:-900}" "$ARES_BIN" --system "Nintendo 64" \
	--setting Video/Driver=Vulkan --setting Audio/Driver=None \
	--setting Input/Driver=SDL --setting General/HomebrewMode=true \
	hl64.z64 >ares.log 2>&1)
CODE=$?
set -e
tr -d '\r' <"$ROMDIR/ares.log" >"$ROMDIR/ares-clean.log"
grep -q '^route PASS$' "$ROMDIR/ares-clean.log" \
	|| { grep '^route FAIL' "$ROMDIR/ares-clean.log" >&2 || tail -30 "$ROMDIR/ares-clean.log" >&2; fail "route (ares)"; }
[[ $CODE -eq 0 ]] || fail "ares exit $CODE"
grep -q '^TESTHARNESS exit 0' "$ROMDIR/ares-clean.log" || fail "no harness exit 0"

# ramp binds: no page drawn additive without its cooked ramp, the counts
# the host's (the `add:` lines of every shot)
adds() { grep -E '^shot [0-9]+ add: ' "$1" || true; }
[[ -n "$(adds "$ROMDIR/ares-clean.log")" ]] || fail "no 'add:' lines on the cart"
adds "$ROMDIR/ares-clean.log" | grep -v ' miss=0$' && fail "a page drawn additive without a cooked ramp"
diff <(adds "$ROMDIR/host.log") <(adds "$ROMDIR/ares-clean.log") >"$ROMDIR/add.diff" \
	|| fail "cart / host 'add:' lines differ ($ROMDIR/add.diff)"

# the shots, in route order: glow shots checked, every shot kept as PNG
mapfile -t LABELS < <(awk '$1 == "shot" { print $2 }' "$ROUTE")
GLOWS=()
for i in "${!LABELS[@]}"; do
	ppm="$ROMDIR/ares-test-$(printf %04d "$i").ppm"
	[[ -f "$ppm" ]] || fail "no harness dump for shot ${LABELS[$i]} ($ppm)"
	mv "$ppm" "$ROMDIR/shots/${LABELS[$i]}.ppm"
	[[ "${LABELS[$i]}" == g* ]] && GLOWS+=("$ROMDIR/shots/${LABELS[$i]}.ppm")
done
python3 - "$ROMDIR/shots" <<'EOF'
import pathlib, sys
from PIL import Image
for p in pathlib.Path(sys.argv[1]).glob("*.ppm"):
    Image.open(p).save(p.with_suffix(".png"))
EOF
python3 "$ROOT/test/m3/add_shot.py" "${GLOWS[@]}" || fail "a glow core wrapped (add_shot.py)"
echo "add ares: PASS (${#GLOWS[@]} glow shots unwrapped, add: miss=0, shots $ROMDIR/shots)"
