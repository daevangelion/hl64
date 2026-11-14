#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Spatial grid cost (docs/specs/area-links-and-touch.md 2) on the cart: an
# HL64_TEST ROM runs the `bench 1` debug command (engine/ent/grid_bench.c)
# in the patched ares and quits; the "bench grid:" line is printed. With
# --host the host build runs the same command (wall clock of a shared
# machine: indicative only).
#
# Usage: test/m2/grid_bench.sh [--host]
# Env:   BUILD_DIR (scratch), HL64_ARES_HOST / ARES_BIN (test/run/ares.sh), ARES_TIMEOUT, DISPLAY_NUM
# Exit:  0 with the bench line, 1 failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/grid-bench"
mkdir -p "$OUT"
fail() { echo "grid bench FAIL: $*" >&2; exit 1; }

make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/host-build.log" 2>&1 \
	|| { tail -40 "$OUT/host-build.log" >&2; fail "host build"; }
HOST="$BUILD_DIR/host/hl64-host"
if [[ "${1:-}" == "--host" ]]; then
	"$HOST" --cmd "@2 bench 1" --frames 4 | grep '^bench grid:' || fail "no bench line"
	exit 0
fi

# no player content: the ROM boots without a world; dbg.cmd is the only file
mkdir -p "$OUT/content"
"$HOST" --cmd "@2 bench 1" --cmd "@4 quit 0" --write-cmds "$OUT/bench.cmd"
ROMDIR="$OUT/rom"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 HL64_CONTENT="$OUT/content" \
	HL64_CMDS="$OUT/bench.cmd" rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "rom build"; }

# shellcheck source=../run/ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2

set +e
ares_run "$OUT" "$ROMDIR/hl64.z64" "$OUT/ares.log" "${ARES_TIMEOUT:-300}"
CODE=$?
set -e
tr -d '\r' <"$OUT/ares.log" >"$OUT/ares-clean.log"
[[ $CODE -eq 0 ]] || { tail -40 "$OUT/ares-clean.log" >&2; fail "ares exit $CODE"; }
grep '^bench grid:' "$OUT/ares-clean.log" || fail "ares: no bench line"
