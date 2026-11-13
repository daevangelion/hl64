#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Rec M4 tests (host): the level-state driver test/m4/rec4_test.c on the
# cooked content (async open, release / rehydrate, frozen maps, twins,
# deferred uses, runtime ids, store sizing), once plain and once under
# ASan / UBSan / LeakSanitizer (a separate build directory sharing the
# host-cart image), the cook's HLE region proof with the frozen runtime
# part against the engine's mem lines, then test/m3/rec.sh (M3 still PASS).
#
# Usage: test/m4/rec.sh
# Env:   BUILD_DIR (scratch), HL64_CONTENT (cooked content with ids),
#        REC4_DELTA (test delta bytes for the store sizing, default 64),
#        REC_M3=0 skips test/m3/rec.sh.
# Exit:  0 pass, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:?set HL64_CONTENT to the cooked content directory}"
OUT="$BUILD_DIR/m4-rec"
ASAN_DIR="$BUILD_DIR/m4-rec-asan"
mkdir -p "$OUT"
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb" >&2; exit 2; }
fail() { echo "REC4 FAIL: $*" >&2; exit 1; }

cd "$ROOT"
make --no-print-directory BUILD_DIR="$BUILD_DIR" rec4test >"$OUT/build.log" 2>&1 \
	|| { tail -30 "$OUT/build.log" >&2; fail "build"; }
grep -E 'warning:' "$OUT/build.log" && fail "build has warnings"
"$BUILD_DIR/host/hl64-rec4" --root "$CONTENT" selftest "${REC4_DELTA:-64}" >"$OUT/selftest.log" 2>&1 \
	|| { grep -E '^FAIL' "$OUT/selftest.log" >&2 || tail -20 "$OUT/selftest.log" >&2; fail "selftest"; }
grep -E '^A[0-9] |^store:|^prof rec_us' "$OUT/selftest.log"
# sanitizers: the same driver, its own objects, the host-cart image shared
mkdir -p "$ASAN_DIR"
[[ -d "$ASAN_DIR/host-cart" ]] || cp -al "$BUILD_DIR/host-cart" "$ASAN_DIR/"
make --no-print-directory BUILD_DIR="$ASAN_DIR" \
	HOST_CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" \
	rec4test >"$OUT/asan-build.log" 2>&1 || { tail -30 "$OUT/asan-build.log" >&2; fail "asan build"; }
ASAN_OPTIONS=detect_leaks=1 "$ASAN_DIR/host/hl64-rec4" --root "$CONTENT" selftest \
	>"$OUT/asan.log" 2>&1 || { grep -E '^FAIL|ERROR|runtime error' "$OUT/asan.log" >&2; fail "asan run"; }
grep -qE 'LeakSanitizer|AddressSanitizer|runtime error' "$OUT/asan.log" && fail "sanitizer report"
echo "sanitizers: ASan / UBSan / LeakSanitizer clean"
# the cook's HLE proof: the frozen runtime part of the engine's frozen map
# (mem rec: frozen lines) equals hleids' frozen bytes for that map
F_ENG=$(sed -n 's/^mem rec: frozen \([^ ]*\) runtime=\([0-9]*\) .*/\1 \2/p' "$OUT/selftest.log" | head -1)
[[ -n "$F_ENG" ]] || fail "no frozen map in the mem lines"
F_COOK=$(python3 -c "
import os, sys
from tools.restab import rtb
from tools.w64c import hleids as h
c, m = sys.argv[1], sys.argv[2]
t = rtb.read(open(os.path.join(c, 'campaign.rtb'), 'rb').read())
p = os.path.join(c, t.files[t.map_files[t.map_names.index(m)]].path)
rt, lg, tab = h.accounting(h.DEFAULT_REC_INT)
s = h.slot_bytes(m, h.w64_ents(open(p, 'rb').read(), p), rt, lg)
print(m, s.frozen)" "$CONTENT" "${F_ENG%% *}") || fail "frozen proof (cook)"
[[ "$F_COOK" == "$F_ENG" ]] || fail "frozen proof: cook '$F_COOK', engine '$F_ENG'"
echo "HLE frozen proof: cook = engine ($F_ENG B)"
python3 -m tools.w64c.hleids --check "$CONTENT" --defs game/defs | grep -E 'HLE region need' \
	|| fail "hleids --check"
if [[ "${REC_M3:-1}" != 0 ]]; then
	test/m3/rec.sh >"$OUT/m3-rec.log" 2>&1 || { tail -20 "$OUT/m3-rec.log" >&2; fail "test/m3/rec.sh"; }
	echo "test/m3/rec.sh: $(tail -1 "$OUT/m3-rec.log")"
fi
echo "REC4 PASS"
