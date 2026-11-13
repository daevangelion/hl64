#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Rec slice tests (host): the cook's id step and resource table tests,
# the record / resource driver (test/m3/rec.py: selftest, K5 on the
# GoldSrc K-think fixtures, the hostile-input fuzz under ASan / UBSan) and
# R6 (no string compare on the cart's resource paths).
#
# Usage: test/m3/rec.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content with ids),
#        HL64_FIXTURES (default /scratch/hl64/fixtures), REC_FUZZ (20000).
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
FIX="${HL64_FIXTURES:-/scratch/hl64/fixtures}"
OUT="$BUILD_DIR/m3-rec"
mkdir -p "$OUT"
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }
[[ -d "$FIX/goldsrc/think-and-time/K-think" ]] || { echo "no K-think fixtures under $FIX" >&2; exit 2; }
fail() { echo "REC FAIL: $*" >&2; exit 1; }

cd "$ROOT"
# R1 (restab determinism, unresolved names), the id step, titles
make --no-print-directory BUILD_DIR="$BUILD_DIR" "$BUILD_DIR/tools/hlecook" rectest \
	>"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log" >&2; fail "build"; }
grep -E 'warning:' "$OUT/build.log" && fail "build has warnings"
python3 -m unittest tools.restab.test_restab >"$OUT/unit.log" 2>&1 \
	&& HL64_HLECOOK="$BUILD_DIR/tools/hlecook" python3 -m unittest tools.w64c.test_hleids \
		tools.w64c.test_hlecook tools.w64c.test_touchrank >>"$OUT/unit.log" 2>&1 \
	|| { tail -30 "$OUT/unit.log" >&2; fail "cook unit tests"; }
echo "R1 / id step: cook unit tests PASS"
# R2: every res_t of every chunk and res.def inside its file
python3 -m tools.w64c.hleids --check "$CONTENT" --defs game/defs || fail "R2 (hleids --check)"
# R6: no string compare on the resource path
if grep -nE '\b(strcmp|strcasecmp|strncmp|strncasecmp|stricmp)\b' engine/res/*.c engine/snd/*.c \
	engine/render/*.c 2>/dev/null; then
	fail "R6: string compare in engine/res, engine/snd or engine/render"
fi
echo "R6: no string compare in engine/res, engine/snd, engine/render"
# the fuzz harness: native, sanitizers
cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror \
	-I. -Iinclude -o "$OUT/hl64-rec-fuzz" test/m3/rec_fuzz.c engine/res/rtb.c engine/ent/rec_hle.c \
	|| fail "fuzz build"
cp "$BUILD_DIR/host/hl64-rec" "$OUT/"
TMPDIR="$OUT" python3 test/m3/rec.py --bin "$OUT" --content "$CONTENT" --fixtures "$FIX/goldsrc" \
	--fuzz "${REC_FUZZ:-20000}" --scratch "$OUT" || fail "rec.py"
