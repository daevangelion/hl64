#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Controller Pak fallbacks of engine/save/save.c (docs/architecture.md 14)
# on hl64-host, scene hl2-yard, on a Pak holding a 13000 B foreign note
# (cpaktool), so one save fits beside it and two do not:
#   1. no room for a second note: save slot 3 at T, again at T+150: the
#      second one deletes the old note first and writes over it (`no room
#      for a second note`); both ok, the foreign note byte-identical,
#      cpaktool finds no issue; a fresh run loads slot 3 (the second
#      save's sequence)
#   2. forced remount: the same with HL64_PAK_FAIL_WRITE (a device write in
#      the first save's transfer fails): that save FAILED, the Pak mounted
#      and listed again, the second save ok over the headerless note, a
#      fresh run loads it, cpaktool finds no issue
#   3. too large: with the note cap lowered (HL64_HOST_PAK_IMAGE_MAX) the
#      save fails with SAVE_ERR_PAK_SIZE, the Pak unchanged; Flash takes it
#
#   test/save/pakfail.sh      (BUILD_DIR, HL64_CONTENT as determinism.sh)
# Out: $BUILD_DIR/save/pakfail/. Exit 0 pass, 1 fail, 2 prerequisite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int7/c21}"
N64_INST="${N64_INST:-$HOME/sdks/n64-sdk-hl64-$(sed -n 's/^LIBDRAGON_REV := \(.\{12\}\).*/\1/p' "$ROOT/Makefile")}"
CPAK="$N64_INST/bin/cpaktool"
T=600
[[ -x "$CPAK" ]] || { echo "pakfail: no cpaktool at $CPAK" >&2; exit 2; }
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "pakfail: no $CONTENT/campaign.rtb: cook the content" >&2; exit 2; }

OUT="$BUILD_DIR/save/pakfail"
rm -rf "$OUT" && mkdir -p "$OUT"
make -C "$ROOT" --no-print-directory -j"${MAKE_JOBS:-4}" BUILD_DIR="$BUILD_DIR/sh" host >"$OUT/build.log" 2>&1 \
	|| { tail -30 "$OUT/build.log" >&2; echo "pakfail: host build failed" >&2; exit 2; }
HOST="$BUILD_DIR/sh/host/hl64-host"
(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" test/scenes/hl2-yard.route \
	-o "$OUT/route.rbc") >"$OUT/routec.log" 2>&1 || { cat "$OUT/routec.log" >&2; exit 2; }
cd "$OUT"
fail() { echo "pakfail: FAIL $*"; exit 1; }

head -c 13000 /dev/urandom >NSME.01-MARIO.A
"$CPAK" format base.pak >/dev/null
"$CPAK" add base.pak NSME.01-MARIO.A >/dev/null
mkdir x0 && (cd x0 && "$CPAK" extract ../base.pak >/dev/null)

host() {	# host <pak> <log> <cmd...>
	local pak=$1 log=$2
	shift 2
	local c=() a
	for a in "$@"; do c+=(--cmd "$a"); done
	"$HOST" --root "$CONTENT" --route route.rbc --save "${log%.log}.fla" --pak "$pak" \
		--cmd "@1 fixed_dt 33333" "${c[@]}" >"$log" 2>&1
}
check_pak() {	# check_pak <pak>: the foreign note unchanged, no issue
	local d="x-${1%.pak}"
	rm -rf "$d" && mkdir "$d"
	(cd "$d" && "$CPAK" extract "../$1" >/dev/null)
	cmp x0/NSME.01-MARIO.A "$d/NSME.01-MARIO.A" || fail "$1: the foreign note changed"
	[[ "$("$CPAK" test "$1" 2>&1)" == 'No issues found' ]] || { "$CPAK" test "$1"; fail "cpaktool test $1"; }
}
# the sequence of the last `save slot 3` (the slot's seq line), and the load's
seq_saved() { sed -n 's/^save: slot 3 seq \([0-9]*\).*/\1/p' "$1" | tail -1; }
seq_loaded() { sed -n 's/^load: slot 3 seq \([0-9]*\).*/\1/p' "$1" | head -1; }

# 1. no room for a second note
cp base.pak room.pak
host room.pak room-save.log "@$T save 3" "@$((T + 150)) save 3" "@$((T + 250)) quit 0" \
	|| fail "room: hl64-host exit $?"
[[ $(grep -c '^save: save slot 3 ok' room-save.log) == 2 ]] || fail "room: two saves ok"
grep -q '^save: Controller Pak: no room for a second note: over note' room-save.log \
	|| fail "room: the second save did not take the delete-first path"
check_pak room.pak
host room.pak room-load.log "@1 load 3" "@60 quit 0" || fail "room load: hl64-host exit $?"
grep -q '^save: load slot 3 ok' room-load.log || fail "room: load"
[[ -n "$(seq_saved room-save.log)" && "$(seq_saved room-save.log)" == "$(seq_loaded room-load.log)" ]] \
	|| fail "room: loaded seq $(seq_loaded room-load.log), saved $(seq_saved room-save.log)"
echo "pakfail: no room for two notes: second save over note $(sed -n 's/.*over note \([0-9]*\).*/\1/p' room-save.log), foreign note intact, reload seq $(seq_loaded room-load.log) ok"

# 2. a failed transfer forces a remount
cp base.pak fail.pak
HL64_PAK_FAIL_WRITE=${PAKFAIL_WRITE:-20} host fail.pak fail-save.log "@$T save 3" "@$((T + 150)) save 3" \
	"@$((T + 250)) quit 0" || fail "fail: hl64-host exit $?"
grep -q '^pak: injected write failure' fail-save.log || fail "fail: no injected failure"
grep -q '^save: save slot 3 FAILED' fail-save.log || fail "fail: the failed save reported ok"
[[ $(grep -c '^save: save slot 3 ok' fail-save.log) == 1 ]] || fail "fail: the save after the failure"
check_pak fail.pak
host fail.pak fail-load.log "@1 load 3" "@60 quit 0" || fail "fail load: hl64-host exit $?"
grep -q '^save: load slot 3 ok' fail-load.log || fail "fail: load"
[[ "$(seq_saved fail-save.log)" == "$(seq_loaded fail-load.log)" ]] || fail "fail: loaded seq"
# after the failure: the Pak mounted again (forced remount) and listed
# (grep reads to the end: an early `grep -q` exit can SIGPIPE sed, which
# pipefail turns into a failure)
sed -n '/^pak: injected write failure/,$p' fail-save.log | grep '^pak: mounted' >/dev/null \
	|| fail "fail: no remount"
sed -n '/^pak: injected write failure/,$p' fail-save.log | grep '^save: Controller Pak: [0-9]* of our notes' \
	>/dev/null || fail "fail: the Pak not listed again"
echo "pakfail: failed transfer: save FAILED, Pak remounted and listed again, next save ok over the headerless note, reload seq $(seq_loaded fail-load.log) ok"

# 3. too large for a note (the cap lowered to 1000 B, HL64_HOST_PAK_IMAGE_MAX):
# the save fails with SAVE_ERR_PAK_SIZE (-3, the menu's "Save too large for
# Controller Pak"), the Pak untouched; a Flash save of the same state is ok
cp base.pak big.pak
HL64_HOST_PAK_IMAGE_MAX=1000 host big.pak big-save.log "@$T save 3" "@$((T + 60)) save 1" \
	"@$((T + 120)) quit 0" || fail "big: hl64-host exit $?"
grep -q '^save: slot 3: too big for the Controller Pak' big-save.log || fail "big: no too-big line"
grep -q '^save: save slot 3 FAILED (-3)' big-save.log || fail "big: not SAVE_ERR_PAK_SIZE"
grep -q '^save: save slot 1 ok' big-save.log || fail "big: the Flash save"
cmp base.pak big.pak || fail "big: the Pak changed"
echo "pakfail: too large for a note: save FAILED (-3, SAVE_ERR_PAK_SIZE), Pak unchanged, Flash save ok"
echo "pakfail: PASS"
