#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build the patched ares every harness run uses (test/run/ares.sh,
# ares-remote.sh): ares as Christopher Bonhage's fork meeq/ares has it at
# ARES_PIN (branch sc64: ares master of 2026-09 plus his SummerCart64
# emulation, ares/n64/sc64: SD card image, USB host, registers; ares and the
# fork are ISC-licensed), then our patches in this order:
#   test/ares-testharness.patch        harness device, ISViewer to stdout,
#                                      pad input file, host VI shots
#   test/ares-large-rom-isviewer.patch ISViewer at any ROM size / on the SC64
#   test/ares-flash.patch              ARES_FLASH_IN / ARES_FLASH_OUT
#   test/ares-pak.patch                ARES_PAK_IN / ARES_PAK_OUT
#   test/ares-sc64.patch               ARES_SC64_SECTOR_US (SD command busy time)
# The fork's code is fetched, never copied into this repo.
#
#   test/run/ares-build.sh [DIR]   here: DIR (default ~/hl64-ares-sc64) gets
#                                  src/ (the pinned checkout, re-made from
#                                  scratch each time: no local edits survive),
#                                  build/desktop-ui/ares, xdg/ (XDG_DATA_HOME)
#   HL64_ARES_HOST=ares-host test/run/ares-build.sh
#                                  the same on that host (this script and the
#                                  patches are copied there; DIR on the host)
# Same CMake options as the earlier harness builds: Release, ARES_CORES=n64,
# ARES_BUILD_LOCAL=ON. Needs git, cmake, ninja, a C++20 compiler, ares' build
# dependencies (SDL2, Vulkan headers, ...).
set -euo pipefail

ARES_URL=https://github.com/meeq/ares
ARES_PIN=00ebb5bb9d4f1da5b1a5233919fbf2e89e7e7e2f
PATCHES=(ares-testharness ares-large-rom-isviewer ares-flash ares-pak ares-sc64)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${HL64_ARES_HOST:-local}" != local && -z "${ARES_BUILD_REMOTE_SIDE:-}" ]]; then
	case "$HL64_ARES_HOST" in ares-host) to="${HL64_ARES_SSH:-ares-host}" ;; *) to="${HL64_ARES_SSH:-$HL64_ARES_HOST}" ;; esac
	dir=${1:-hl64-ares-sc64}
	files=("${BASH_SOURCE[0]}")
	for p in "${PATCHES[@]}"; do files+=("$HERE/../$p.patch"); done
	ssh -o BatchMode=yes "$to" "mkdir -p $dir/patches"
	rsync -t "${files[@]}" "$to:$dir/patches/"
	exec ssh -o BatchMode=yes "$to" "cd $dir && ARES_BUILD_REMOTE_SIDE=1 ARES_PATCH_DIR=\$PWD/patches bash patches/ares-build.sh \$PWD"
fi

DIR="${1:-$HOME/hl64-ares-sc64}"
PD="${ARES_PATCH_DIR:-$HERE/..}"
mkdir -p "$DIR/xdg"
cd "$DIR"
[[ -d src/.git ]] || git init -q src
git -C src fetch -q --depth 1 "$ARES_URL" "$ARES_PIN"
git -C src checkout -q -f --detach "$ARES_PIN"
git -C src clean -fdxq
for p in "${PATCHES[@]}"; do
	patch -p1 -d src --forward --no-backup-if-mismatch -s <"$PD/$p.patch" \
		|| { echo "ares-build: $p.patch does not apply on $ARES_PIN" >&2; exit 1; }
done
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DARES_CORES=n64 -DARES_BUILD_LOCAL=ON >cmake.log 2>&1 \
	|| { tail -20 cmake.log >&2; exit 1; }
ninja -C build >ninja.log 2>&1 || { grep -m 20 -E 'error|Error' ninja.log >&2; exit 1; }
echo "ares-build: $DIR/build/desktop-ui/ares (meeq/ares ${ARES_PIN:0:10} + ${#PATCHES[@]} patches)"
