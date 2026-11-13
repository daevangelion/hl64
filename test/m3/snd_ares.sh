#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Snd cart smoke (ares): a PROFILE test ROM over cooked content whose
# debug command file plays sounds through the real libdragon backend:
# 7 loops plus one-shots, one-shots at the 130 % rate cap (22050 Hz waves
# at pitch 255: the voice buffer limit), more distinct waves than the 10
# open ones (LRU close), sentences. Passes when sounds start on the cart
# (event log over ISViewer) and the harness exits 0 (libdragon asserts on a
# voice buffer too small or a bad wave); prints the `prof snd_us` lines
# (CPU, voices, waves, SOUND region use). Start timing against the host
# run of the same commands is diffed for information only (one-shot ends,
# hence steals, follow real playback on the cart).
#
# Usage: test/m3/snd_ares.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content), ARES_BIN.
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
ROMDIR="$BUILD_DIR/m3-snd-rom"
fail() { echo "SND-ARES FAIL: $*" >&2; exit 1; }
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }
[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN" >&2; exit 2; }

cd "$ROOT"
make --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$BUILD_DIR/snd-host-build.log" 2>&1 \
	|| { tail -30 "$BUILD_DIR/snd-host-build.log" >&2; fail "host build"; }
rm -rf "$ROMDIR" && mkdir -p "$ROMDIR"
cp -al "$CONTENT" "$ROMDIR/content"
rm -f "$ROMDIR/content/route.rbc"

# the commands: ids from the content's own table and wav64 headers
mapfile -t CMDS < <(python3 - "$CONTENT" <<'EOF'
import struct, sys
from tools.restab import rtb
c = sys.argv[1]
t = rtb.read(open(c + "/campaign.rtb", "rb").read())
first, n = t.kind_first(rtb.KIND_SOUND), t.kind_count(rtb.KIND_SOUND)
loops, fast, slow = [], [], []
for i in range(n):
    e = t.res[first + i]
    with open(c + "/" + t.files[e.file].path, "rb") as f:
        h = f.read(28)
    freq, _, loop = struct.unpack(">iii", h[8:20])
    (loops if loop else fast if freq == 22050 else slow).append(0x6000 + i)
cmds, f = [], 20
for s in loops[:7]:
    cmds.append(f"@{f} sound {s} 0 0"); f += 1
f = 30
for s in fast[:6]:
    cmds.append(f"@{f} sound {s} 0 255"); f += 2
for s in slow[:12]:
    cmds.append(f"@{f} sound {s} 0 0"); f += 2
for k, s in enumerate(range(min(3, len(t.sentences)))):
    cmds.append(f"@{100 + 50 * k} sound {0xA000 + s} 0 0")
cmds.append("@420 quit 0")
for x in cmds[:31]:
    print(x)
EOF
)
ARGS=()
for c in "${CMDS[@]}"; do ARGS+=(--cmd "$c"); done
"$BUILD_DIR/host/hl64-host" --write-cmds "$ROMDIR/snd.cmd" "${ARGS[@]}" || fail "write-cmds"
"$BUILD_DIR/host/hl64-host" --root "$ROMDIR/content" "${ARGS[@]}" >"$ROMDIR/host.log" 2>&1 \
	|| { tail -20 "$ROMDIR/host.log" >&2; fail "host run"; }
make --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 PROFILE=1 HL64_CONTENT="$ROMDIR/content" \
	HL64_CMDS="$ROMDIR/snd.cmd" rom >"$ROMDIR/rom-build.log" 2>&1 \
	|| { tail -40 "$ROMDIR/rom-build.log" >&2; fail "rom build"; }
grep -E 'warning:' "$ROMDIR/rom-build.log" && fail "rom build has warnings"

# shellcheck source=../run/xvfb.sh
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
tr -d '\r' <"$ROMDIR/ares.log" >"$ROMDIR/ares-clean.log"
[[ $CODE -eq 0 ]] || { tail -30 "$ROMDIR/ares-clean.log" >&2; fail "ares exit $CODE"; }
grep -q '^TESTHARNESS exit 0' "$ROMDIR/ares-clean.log" || fail "no harness exit 0"
# the same starts in the same frames (names are host-only: compare ids)
starts() { grep -E '^snd [0-9]+ (start|word|loop) ' "$1" | sed -E 's/ s([0-9]+) [^ ]+/ s\1/'; }
n=$(starts "$ROMDIR/ares-clean.log" | grep -c ' start ' || true)
[[ $n -gt 0 ]] || fail "no sound started on the cart"
diff <(starts "$ROMDIR/host.log" | cut -d' ' -f1-8) <(starts "$ROMDIR/ares-clean.log" | cut -d' ' -f1-8) \
	>"$ROMDIR/starts.diff" || echo "note: cart / host start timing differs (see $ROMDIR/starts.diff)"
grep '^prof snd_us' "$ROMDIR/ares-clean.log"
echo "snd ares: PASS ($n starts on the cart; log $ROMDIR/ares-clean.log)"
