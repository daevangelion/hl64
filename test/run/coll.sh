#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Collision gate (docs/specs/trace-and-hulls.md 5, architecture.md 17 M2).
#
#   host:  build hl64-colltest, run test/coll/trace_tests.py (T1-T10 on the
#          GoldSrc probe fixtures of every map; ORDER deviations reported
#          apart, T10's pose-dependent part counted as remaining).
#   ares:  a PROFILE HL64_TEST ROM runs `colbench` on hldemo1 (every
#          T-segments segment as a world trace, per hull, cold and warm,
#          plus point contents): the per-trace cost; the result hashes
#          must equal the host's (bit-identical collision on both).
#
# Usage: test/run/coll.sh [--ares]
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content), HL64_FIXTURES
#        (default /scratch/hl64/fixtures), ARES_BIN, ARES_TIMEOUT,
#        DISPLAY_NUM.
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/coll"
ARES=0
[[ "${1:-}" == "--ares" ]] && ARES=1
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
CWG="$CONTENT/maps/campaign.cwg"
[[ -f "$CWG" ]] || { echo "no $CWG: cook the content" >&2; exit 2; }
[[ -d "$FIX/goldsrc/trace-and-hulls" ]] || { echo "no probe fixtures under $FIX" >&2; exit 2; }
mkdir -p "$OUT"
fail() { echo "COLL FAIL: $*" >&2; exit 1; }

# ---- host ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host colltest >"$OUT/host-build.log" 2>&1 \
	|| { tail -30 "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
python3 "$ROOT/test/coll/trace_tests.py" --bin "$BUILD_DIR/host/hl64-colltest" --content "$CONTENT" \
	--fixtures "$FIX" | tee "$OUT/trace-tests.log"
grep -q '^trace-and-hulls: PASS' "$OUT/trace-tests.log" || fail "trace-and-hulls tests"

if [[ $ARES -eq 0 ]]; then
	echo "COLL PASS (host)"
	exit 0
fi

# ---- ares: per-trace cost on hldemo1 ----
MAP="$(python3 - "$CWG" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('>H', b, 8)[0]
nl, nk = struct.unpack_from('>2H', b, 10)
ns, nt, npl = struct.unpack_from('>3H', b, 20)
so = 32 + 48 * n + 16 * nl + 8 * nk + 136 * ns + 4 * nt + 2 * npl
for i in range(n):
    s = struct.unpack_from('>H', b, 32 + 48 * i)[0]
    if b[so + s:b.index(b'\0', so + s)] == b'hldemo1':
        print(i)
EOF
)"
[[ -n "$MAP" ]] || fail "hldemo1 not in $CWG"
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; exit 2; }

# content + coll/bench.bin, hard-linked (no copy of the player's data)
BC="$OUT/content"
rm -rf "$BC"
cp -al "$CONTENT" "$BC"
python3 "$ROOT/test/coll/bench.py" --fixtures "$FIX" --map hldemo1 -o "$BC/coll/bench.bin"

CMDS=(--cmd "@10 colbench $MAP" --cmd "@12 quit 0")
"$BUILD_DIR/host/hl64-host" --root "$BC" "${CMDS[@]}" >"$OUT/host-bench.log" 2>&1 \
	|| { tail -20 "$OUT/host-bench.log" >&2; fail "host colbench"; }

# shellcheck source=xvfb.sh
. "$ROOT/test/run/xvfb.sh"
xvfb_start || exit 2
trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT

ROMDIR="$BUILD_DIR/coll-rom"
mkdir -p "$ROMDIR"
"$BUILD_DIR/host/hl64-host" --write-cmds "$ROMDIR/coll.cmd" "${CMDS[@]}"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 \
	HL64_CONTENT="$BC" HL64_CMDS="$ROMDIR/coll.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "PROFILE rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"
set +e
(cd "$ROMDIR" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
	timeout "${ARES_TIMEOUT:-600}" "$ARES_BIN" --system "Nintendo 64" \
	--setting Video/Driver=Vulkan --setting Audio/Driver=None \
	--setting Input/Driver=SDL --setting General/HomebrewMode=true \
	hl64.z64 >ares.log 2>&1)
CODE=$?
set -e
tr -d '\r' <"$ROMDIR/ares.log" >"$OUT/ares.log"
[[ $CODE -eq 0 ]] || { tail -30 "$OUT/ares.log" >&2; fail "ares exit $CODE"; }
grep -q '^TESTHARNESS exit 0' "$OUT/ares.log" || fail "no harness exit 0"
grep '^colbench' "$OUT/ares.log" | tee "$OUT/ares-bench.log"
[[ -s "$OUT/ares-bench.log" ]] || fail "no colbench lines from ares"
hashes() { grep '^colbench' "$1" | grep -o '^colbench [a-z]* [a-z]*=[0-9]*\|hash=[0-9a-f]*' | paste -d' ' - -; }
diff <(hashes "$OUT/host-bench.log") <(hashes "$OUT/ares-bench.log") \
	|| fail "cart collision results differ from the host's"
echo "COLL PASS (host + ares)"
