#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build the N64 SDK prefix hl64 links (Makefile N64_INST, docs/architecture.md
# 18.1): libdragon from our fork at the Makefile's LIBDRAGON_REV (branch hl64
# of ~/src/libdragon-hl64: DragonMinded preview + the banked RDRAM overlay +
# the debugcpp debloat), its host tools, and tiny3d at TINY3D_REV, installed
# into ~/sdks/n64-sdk-hl64-<rev12> with the MANIFEST.json `make sdk-check`
# reads. Clean `git archive` trees in WORK: neither checkout is modified.
#
# Usage: plat/n64/build_sdk.sh
# Env:   LIBDRAGON_GIT (default ~/src/libdragon-hl64), TINY3D_GIT (default
#        ~/sdks/tiny3d), N64_GCCPREFIX (default ~/sdks/mips64-toolchain),
#        PREFIX (default ~/sdks/n64-sdk-hl64-<rev12>; must not exist),
#        WORK (default ${TMPDIR:-/tmp}/n64-sdk-hl64-<rev12>), JOBS.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pin() { sed -n "s/^$1 *:= *\([0-9a-f]\{40\}\)$/\1/p" "$ROOT/Makefile"; }
REV="$(pin LIBDRAGON_REV)"
T3D_REV="$(pin TINY3D_REV)"
[[ -n "$REV" && -n "$T3D_REV" ]] || { echo "no LIBDRAGON_REV / TINY3D_REV in $ROOT/Makefile" >&2; exit 2; }
LIBDRAGON_GIT="${LIBDRAGON_GIT:-$HOME/src/libdragon-hl64}"
TINY3D_GIT="${TINY3D_GIT:-$HOME/sdks/tiny3d}"
N64_GCCPREFIX="${N64_GCCPREFIX:-$HOME/sdks/mips64-toolchain}"
PREFIX="${PREFIX:-$HOME/sdks/n64-sdk-hl64-${REV:0:12}}"
WORK="${WORK:-${TMPDIR:-/tmp}/n64-sdk-hl64-${REV:0:12}}"
JOBS="${JOBS:-$(nproc)}"

[[ -x "$N64_GCCPREFIX/bin/mips64-elf-gcc" ]] || { echo "no toolchain at $N64_GCCPREFIX" >&2; exit 2; }
git -C "$LIBDRAGON_GIT" cat-file -e "$REV^{commit}" || { echo "$LIBDRAGON_GIT lacks $REV" >&2; exit 2; }
git -C "$TINY3D_GIT" cat-file -e "$T3D_REV^{commit}" || { echo "$TINY3D_GIT lacks $T3D_REV" >&2; exit 2; }
[[ ! -e "$PREFIX" ]] || { echo "$PREFIX exists" >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK/libdragon" "$WORK/tiny3d"
git -C "$LIBDRAGON_GIT" archive "$REV" | tar -x -C "$WORK/libdragon"
git -C "$TINY3D_GIT" archive "$T3D_REV" | tar -x -C "$WORK/tiny3d"
# tiny3d's -Werror against GCC 16 diagnostics (as the shared SDK builder)
sed -i '/-Wformat-security -Wformat-overflow -Wformat-truncation/a N64_CFLAGS += -Wno-error=type-limits -Wno-error=format' \
	"$WORK/tiny3d/Makefile"

export N64_INST="$PREFIX" N64_GCCPREFIX
mkdir -p "$PREFIX"
make -C "$WORK/libdragon" -j"$JOBS" install-mk libdragon
# tools one at a time: parallel cpaktool links race
make -C "$WORK/libdragon" -j1 tools
make -C "$WORK/libdragon" install tools-install
make -C "$WORK/tiny3d" -j"$JOBS"
make -C "$WORK/tiny3d" install

cat >"$PREFIX/MANIFEST.json" <<EOF
{
  "package": "n64-sdk-hl64",
  "libdragon_rev": "$REV",
  "libdragon_base": "$(git -C "$LIBDRAGON_GIT" merge-base "$REV" upstream/preview 2>/dev/null || echo unknown)",
  "tiny3d_rev": "$T3D_REV",
  "compiler": "$("$N64_GCCPREFIX/bin/mips64-elf-gcc" --version | head -1)"
}
EOF
rm -rf "$WORK"
echo "installed $PREFIX (libdragon $REV, tiny3d $T3D_REV)"
