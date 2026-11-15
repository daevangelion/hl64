#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# FlashRAM power cuts (engine/save/save.c, docs/architecture.md 14) on
# hl64-host, scene hazard-seam, every Flash save forced to its largest (2
# sectors, HL64_HOST_SAVE_SECTORS):
#   base: the autosave, slot 1 and slot 2 (2 sectors each), then a
#      settings change stored as a settings-only copy (1 sector): 7 of the
#      8 pool sectors held, the pool's worst case;
#   then from that image a new 2-sector save (case manual: slot 1; case
#      auto: the autosave), which must take the free sector and the
#      settings copy's; the full run first (it must succeed, its flash
#      operations counted), then one run per erase and program of that
#      save, the power cut in the middle of it (HL64_HOST_FLASH_CUT,
#      plat/host/flash_host.c). After every cut
#      a fresh boot must list all three slots good, the two untouched ones
#      at their old sequence, the saved one at its old or (complete write)
#      its new sequence, and a settings block (the settings copy's, or a
#      save's: at worst the latest settings change is lost, never a save);
#      the cut slot then loads.
#
#   test/save/flashcut.sh      (BUILD_DIR, HL64_CONTENT as determinism.sh)
# Out: $BUILD_DIR/save/flashcut/. Exit 0 pass, 1 fail, 2 prerequisite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int6/c20}"
T=85
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "flashcut: no $CONTENT/campaign.rtb: cook the content" >&2; exit 2; }

OUT="$BUILD_DIR/save/flashcut"
rm -rf "$OUT" && mkdir -p "$OUT"
make -C "$ROOT" --no-print-directory -j"${MAKE_JOBS:-4}" BUILD_DIR="$BUILD_DIR/sh" host >"$OUT/build.log" 2>&1 \
	|| { tail -30 "$OUT/build.log" >&2; echo "flashcut: host build failed" >&2; exit 2; }
HOST="$BUILD_DIR/sh/host/hl64-host"
SVDUMP="$ROOT/test/save/svdump.py"
(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" test/scenes/hazard-seam.route \
	-o "$OUT/route.rbc") >"$OUT/routec.log" 2>&1 || { cat "$OUT/routec.log" >&2; exit 2; }
cd "$OUT"
FAILS=0
fail() { echo "flashcut: FAIL $*"; FAILS=$((FAILS + 1)); }
export HL64_HOST_SAVE_SECTORS=1

host() {	# host <image> <log> <cmd...>: exit status in $code
	local img=$1 log=$2
	shift 2
	local c=() a
	for a in "$@"; do c+=(--cmd "$a"); done
	code=0
	"$HOST" --root "$CONTENT" --route route.rbc --save "$img" --cmd "@1 fixed_dt 33333" "${c[@]}" \
		>"$log" 2>&1 || code=$?
}
seqs() {	# seqs <image>: "slot:seq" of each slot's current copy (its newest good one)
	python3 "$SVDUMP" "$1" | sed -n 's/^sectors [0-9,]*: slot \([0-9]\) seq \([0-9]*\) ok .*/\1 \2/p' \
		| sort -k1,1n -k2,2nr | awk '!seen[$1]++ {print $1 ":" $2}'
}
sects() { python3 "$SVDUMP" "$1" | sed -n "s/^sectors \([0-9,]*\): slot $2 seq $3 ok.*/\1/p"; }

# ---- base: autosave, slot 1, slot 2, a settings copy: 7 sectors held ----
# (field 20 = fov, include/hl64/settings.h)
# (the autosave waits for the seam crossing's quiet frame: ~95 frames)
host base.fla base.log "@$T save 1" "@$((T + 1)) save 2" "@$((T + 2)) save 0" \
	"@$((T + 115)) set 20 99" "@$((T + 116)) flash 2" "@$((T + 180)) quit 0"
[[ $code == 0 ]] || { tail -5 base.log; echo "flashcut: base run exit $code"; exit 1; }
python3 "$SVDUMP" base.fla >base.txt
n2=$(grep -c "^sectors [0-9],[0-9]: slot [012] seq [0-9]* ok" base.txt || true)
ns=$(grep -c "^sectors [0-9]: settings seq [0-9]* ok" base.txt || true)
[[ $n2 == 3 && $ns == 1 ]] || { cat base.txt; echo "flashcut: base: want 3 two-sector saves and 1 settings copy"; exit 1; }
cat base.txt
BASE=$(seqs base.fla)

# ---- one case: a full run, then the cuts ----
cut_case() {	# cut_case <name> <slot>
	local name=$1 sl=$2 ops tail k old new got s nold=0 nnew=0 ncopy=0
	cp base.fla "$name-full.fla"
	host "$name-full.fla" "$name-full.log" "@$T save $sl" "@$((T + 180)) quit 0"
	grep -q "^save: save slot $sl ok" "$name-full.log" || { fail "$name: the full save did not succeed"; return; }
	# the save's flash operations (its two writes: the tail sector's, then
	# the header's; a later autosave's are not cut)
	ops=$(sed -n 's/^flash: wrote 0x[0-9a-f]* + [0-9]* B: \([0-9]*\) erases, \([0-9]*\) programs.*/\1 \2/p' \
		"$name-full.log" | head -2 | awk '{t += $1 + $2} END {print t + 0}')
	tail=$(sed -n 's/^flash: wrote 0x[0-9a-f]* + [0-9]* B: \([0-9]*\) erases, \([0-9]*\) programs.*/\1 \2/p' \
		"$name-full.log" | head -1 | awk '{print $1 + $2}')
	new=$(sed -n "s/^save: slot $sl seq \([0-9]*\):.*/\1/p" "$name-full.log" | head -1)
	old=$(echo "$BASE" | grep "^$sl:" | cut -d: -f2)
	s=$(sects "$name-full.fla" "$sl" "$new")
	echo "flashcut: $name: slot $sl seq $old -> $new in sectors $s, $ops flash operations (tail sector $tail)"
	# a cut in every erase and program of the save
	for k in $(seq 1 "$ops"); do
		cp base.fla "$name-cut$k.fla"
		HL64_HOST_FLASH_CUT=$k host "$name-cut$k.fla" "$name-cut$k.log" "@$T save $sl" "@$((T + 180)) quit 0"
		[[ $code == 3 ]] || { fail "$name cut $k: no power cut (exit $code)"; continue; }
		# a fresh boot: the directory
		host "$name-cut$k.fla" "$name-boot$k.log" "@2 quit 0"
		got=$(seqs "$name-cut$k.fla")
		dir=$(sed -n 's/^save: FlashRAM: autosave + 2 slots, 8 pool sectors: //p' "$name-boot$k.log")
		local want_old want_new
		want_old=$BASE
		want_new=$(echo "$BASE" | sed "s/^$sl:.*/$sl:$new/")
		if [[ "$got" != "$want_old" && "$got" != "$want_new" ]]; then
			fail "$name cut $k: slots $(echo $got) (base $(echo $BASE), new $sl:$new)"
		elif [[ $dir != *", settings copy" && $dir != *", settings in a save" ]] || [[ $dir == *empty* || $dir == *corrupt* ]]; then
			fail "$name cut $k: directory '$dir'"
		elif [[ "$got" == "$want_old" ]]; then
			nold=$((nold + 1)); [[ $dir == *copy ]] && ncopy=$((ncopy + 1))
		else
			nnew=$((nnew + 1)); [[ $dir == *copy ]] && ncopy=$((ncopy + 1))
		fi
	done
	echo "flashcut: $name: $ops cuts: $nold left the old copy, $nnew the new one; settings copy kept in $ncopy, the settings of a save in $((nold + nnew - ncopy))"
	# the cut slot loads from the last cut's image
	host "$name-cut$k.fla" "$name-load.log" "@1 load $sl" "@40 quit 0"
	grep -q "^save: load slot $sl ok" "$name-load.log" || fail "$name: slot $sl does not load after a cut"
}

cut_case manual 1
cut_case auto 0
[[ $FAILS == 0 ]] || { echo "flashcut: $FAILS failures"; exit 1; }
echo "flashcut: PASS"
