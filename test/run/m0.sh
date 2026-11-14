#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# M0 gate (docs/architecture.md 17): the host build boots to "boot ok",
# prints the carve table and exits 0; with --ares the HL64_TEST ROM does
# the same in the patched ares and exits 0 through the test harness, and
# its carve table must equal the host's line for line. Also reports drift
# between the cart's heap after the carve and the host's (derived from the
# cart ELF, include/hl64/limits.h HL64_CART_*).
#
# Usage: test/run/m0.sh [--ares]
# Env:   BUILD_DIR (default build; put scratch on /scratch),
#        HL64_ARES_HOST / ARES_BIN (test/run/ares.sh; ARES_BIN default
#        ~/src/products/ares/build/desktop-ui/ares),
#        ARES_TIMEOUT seconds (default 300), DISPLAY_NUM (optional X server)
# Exit:  0 pass, 1 gate failure, 2 prerequisite missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m0"
ARES=0
[[ "${1:-}" == "--ares" ]] && ARES=1

mkdir -p "$OUT"
fail() { echo "M0 FAIL: $*" >&2; exit 1; }

# ---- host ----
make -C "$ROOT" --no-print-directory BUILD_DIR="$BUILD_DIR" host >"$OUT/host-build.log" 2>&1 \
	|| { cat "$OUT/host-build.log" >&2; fail "host build"; }
grep -E 'warning:' "$OUT/host-build.log" && fail "host build has warnings"
set +e
"$BUILD_DIR/host/hl64-host" --frames 60 --cmd "@30 stats 1" >"$OUT/host.log" 2>&1
CODE=$?
set -e
[[ $CODE -eq 0 ]] || { cat "$OUT/host.log" >&2; fail "host exit $CODE"; }
grep -q '^boot ok$' "$OUT/host.log" || fail "host: no 'boot ok'"
grep '^carve:' "$OUT/host.log" >"$OUT/host-carve.txt"
[[ -s "$OUT/host-carve.txt" ]] || fail "host: no carve table"
echo "host: boot ok, $(wc -l <"$OUT/host-carve.txt") carve lines, exit 0"

[[ $ARES -eq 1 ]] || { echo "M0 PASS (host)"; exit 0; }

# ---- ares ----
ROMDIR="$BUILD_DIR/rom-test"
make -C "$ROOT" --no-print-directory BUILD_DIR="$ROMDIR" HL64_TEST=1 rom >"$OUT/rom-build.log" 2>&1 \
	|| { tail -40 "$OUT/rom-build.log" >&2; fail "rom build"; }
grep -E 'warning:' "$OUT/rom-build.log" && fail "rom build has warnings"

# shellcheck source=ares.sh
. "$ROOT/test/run/ares.sh"
ares_setup || exit 2

set +e
ares_run "$OUT" "$ROMDIR/hl64.z64" "$OUT/ares.log" "${ARES_TIMEOUT:-300}"
CODE=$?
set -e
tr -d '\r' <"$OUT/ares.log" >"$OUT/ares-clean.log"
[[ $CODE -eq 0 ]] || { tail -40 "$OUT/ares-clean.log" >&2; fail "ares exit $CODE"; }
grep -q '^boot ok$' "$OUT/ares-clean.log" || fail "ares: no 'boot ok'"
grep '^carve:' "$OUT/ares-clean.log" >"$OUT/ares-carve.txt"
diff -u "$OUT/host-carve.txt" "$OUT/ares-carve.txt" || fail "carve tables differ"
echo "ares: boot ok, carve table identical to host, exit 0"

cart_heap="$(sed -n 's/^mem: heap free .* free \([0-9]*\) B$/\1/p' "$OUT/ares-clean.log")"
host_heap="$(sed -n 's/^mem: heap free .* free \([0-9]*\) B$/\1/p' "$OUT/host.log")"
echo "heap after carve: cart ${cart_heap} B, host ${host_heap} B"
[[ "$cart_heap" == "$host_heap" ]] || echo "note: host heap differs from the cart's (drift $((host_heap - cart_heap)) B): recalibrate HL64_CART_BOOT_BYTES in include/hl64/limits.h"
echo "M0 PASS (host + ares)"
