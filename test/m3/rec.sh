#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Rec slice tests (host): the cook's id step and resource table tests,
# the record / resource driver (test/m3/rec.py: selftest, K5 on the
# GoldSrc K-think fixtures, the hostile-input fuzz under ASan / UBSan),
# R6 (no string compare on the cart's resource paths), the cook's RESTAB
# and HLE region proofs against the engine's mem lines, and a route's
# `startmap` (HL64_TEST New Game on another map) on hl64-host.
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
make --no-print-directory BUILD_DIR="$BUILD_DIR" "$BUILD_DIR/tools/hlecook" rectest host \
	>"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log" >&2; fail "build"; }
grep -E 'warning:' "$OUT/build.log" && fail "build has warnings"
python3 -m unittest tools.restab.test_restab tools.cook.test_decals tools.routec.test_routec \
	>"$OUT/unit.log" 2>&1 \
	&& HL64_HLECOOK="$BUILD_DIR/tools/hlecook" HL64_CONTENT="$CONTENT" python3 -m unittest \
		tools.w64c.test_hleids tools.w64c.test_hlecook tools.w64c.test_touchrank >>"$OUT/unit.log" 2>&1 \
	|| { tail -30 "$OUT/unit.log" >&2; fail "cook unit tests"; }
echo "R1 / id step: cook unit tests PASS"
# R2: every res_t of every chunk and res.def inside its file; decal ids; the
# HLE and decal region proofs
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
"$OUT/hl64-rec" --root "$CONTENT" selftest >"$OUT/selftest.log" 2>&1 || fail "selftest"
# the cook's proof of the RESTAB region equals what the engine takes
T_COOK=$(python3 -c "from tools.restab import restab, rtb; \
print(restab.engine_table_bytes(rtb.read(open('$CONTENT/campaign.rtb', 'rb').read())))")
T_ENG=$(sed -n 's/^mem res: .* tables=\([0-9]*\) .*/\1/p' "$OUT/selftest.log")
[[ -n "$T_ENG" && "$T_COOK" == "$T_ENG" ]] || fail "RESTAB proof: cook $T_COOK B, engine $T_ENG B"
echo "RESTAB proof: cook $T_COOK B = engine $T_ENG B"
# the cook's HLE region proof takes the engine's bytes: slot 0's chunk and
# runtime, rec_init's tables (engine/ent/rec.c mem rec lines)
S_ENG=$(sed -n 's/^mem rec: slot 0 \([^ ]*\) chunk=\([0-9]*\) runtime=\([0-9]*\) .*/\1 \2 \3/p' \
	"$OUT/selftest.log")
S_ENG="$S_ENG $(sed -n 's/^mem rec: blobs=.* tables=\([0-9]*\) .*/\1/p' "$OUT/selftest.log")"
S_COOK=$(python3 -c "
import os, sys
from tools.restab import rtb
from tools.w64c import hleids as h
c, m = sys.argv[1], sys.argv[2]
t = rtb.read(open(os.path.join(c, 'campaign.rtb'), 'rb').read())
p = os.path.join(c, t.files[t.map_files[t.map_names.index(m)]].path)
rt, lg, tab = h.accounting(h.DEFAULT_REC_INT)
s = h.slot_bytes(m, h.w64_ents(open(p, 'rb').read(), p), rt, lg)
print(m, s.chunk, s.runtime, tab)" "$CONTENT" "${S_ENG%% *}") || fail "HLE region proof (cook)"
[[ "$S_COOK" == "$S_ENG" ]] || fail "HLE region proof: cook '$S_COOK', engine '$S_ENG'"
echo "HLE region proof: cook = engine (map chunk runtime tables: $S_ENG)"
# startmap: New Game on hldemo2 (its records live, the player at its first
# info_player_start); control: the same route without it starts on the
# campaign start and its `waitmap hldemo2 0` fails
startroute() {	# name, first line
	{ [[ -n "$2" ]] && echo "$2"; printf 'waitmap hldemo2 0\nwait 0.5\nstats start\npass\n'; } \
		>"$OUT/$1.route"
	python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "$OUT/$1.route" -o "$OUT/$1.rbc" \
		>/dev/null || fail "routec $1"
	"$BUILD_DIR/host/hl64-host" --root "$CONTENT" --route "$OUT/$1.rbc" --cmd "@10 mem" \
		>"$OUT/$1.log" 2>&1
}
startroute startmap "startmap hldemo2" || { tail -20 "$OUT/startmap.log" >&2; fail "startmap route"; }
N=$(sed -n 's/^rec: new campaign hldemo2: \([0-9]*\) spawned.*/\1/p' "$OUT/startmap.log")
[[ -n "$N" && "$N" -gt 0 ]] || fail "startmap: hldemo2 New Game spawned no records"
grep -q '^route PASS' "$OUT/startmap.log" || fail "startmap: no route PASS"
grep -q '^mem rec: slot 0 hldemo2 ' "$OUT/startmap.log" || fail "startmap: hldemo2 not in HLE slot 0"
grep -q '^mem rec: slot [1-9]' "$OUT/startmap.log" && fail "startmap: a second HLE slot is open"
if startroute startdefault ""; then fail "control: the route without startmap passed"; fi
grep -q '^route FAIL 0 .*WAIT_MAP' "$OUT/startdefault.log" || fail "control: no WAIT_MAP failure"
echo "startmap: hldemo2 New Game, $N records spawned, route PASS (control without it: WAIT_MAP FAIL)"
echo "REC PASS"
