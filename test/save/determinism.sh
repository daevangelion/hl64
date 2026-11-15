#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Saved-game determinism (docs/architecture.md 14): a scene saves at frame
# T into slot 0 and goes on (run A); a fresh boot of the same scene loads
# slot 0 at frame 1 and goes on (run B); a run that never saves (run C).
# At frame T+N runs A and B save again into slot 1: their state streams
# (everything a save holds: components, private blocks, records, timers,
# the game's globals, clock, random stream, ...) must be equal byte for
# byte (test/save/svdump.py), and their `stats` entity hash / player /
# record lines at frames T+1 and T+N equal. C against A tells whether the
# save itself (its settle: paused AI thinks, opens in flight) changed the
# game. Every frame a save or load holds is logged with its time
# (`save: ... worst frame`).
#
#   test/save/determinism.sh [--ares] SCENE:T:N [SCENE:T:N ...]
#
# host (default): hl64-host at a fixed 1/30 s, image files --save.
# --ares: one HL64_TEST ROM per run (the ELF built once), each ares run
#   through test/run/ares.sh (HL64_ARES_HOST=ares-host), the chip image carried
#   from A to B by ARES_FLASH_OUT / ARES_FLASH_IN: B is a fresh ares launch
#   that loads what A's ares wrote (save, quit, relaunch, load).
# Env: BUILD_DIR (default build), HL64_CONTENT (default
#   /scratch/hl64/tmp/M5Lead2/c16), ARES_TIMEOUT (s per run, default 1800).
# Out: $BUILD_DIR/save/<host|ares>/<scene>-<T>/{A,B,C}/ logs, images;
#   summary.md beside them. Exit 0 every scene equal, 1 a difference, 2 usage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/M5Lead2/c16}"
MODE=host
CASES=()
usage() { echo "usage: test/save/determinism.sh [--ares] SCENE:T:N ..." >&2; exit 2; }
while [[ $# -gt 0 ]]; do
	case "$1" in
	--ares) MODE=ares ;;
	--host) MODE=host ;;
	*:*:*) CASES+=("$1") ;;
	*) usage ;;
	esac
	shift
done
[[ ${#CASES[@]} -gt 0 ]] || usage
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb: cook the content" >&2; exit 2; }

MK=(make --no-print-directory -j"${MAKE_JOBS:-4}" -C "$ROOT")
HOST="$BUILD_DIR/sh/host/hl64-host"
"${MK[@]}" BUILD_DIR="$BUILD_DIR/sh" host >"$BUILD_DIR/sh.log" 2>&1 \
	|| { tail -30 "$BUILD_DIR/sh.log" >&2; echo "save: host build failed" >&2; exit 2; }
if [[ $MODE == ares ]]; then
	. "$ROOT/test/run/ares.sh"
	"${MK[@]}" BUILD_DIR="$BUILD_DIR/sr" HL64_TEST=1 PROFILE=1 elf >"$BUILD_DIR/sr-elf.log" 2>&1 \
		|| { tail -30 "$BUILD_DIR/sr-elf.log" >&2; echo "save: ROM ELF build failed" >&2; exit 2; }
	rm -rf "$BUILD_DIR/sr/content"
	cp -al "$CONTENT" "$BUILD_DIR/sr/content"
fi
BE=()
[[ $MODE == ares ]] && BE=(--be)

# run <dir> <A|B|C> <T> <N>: one run's commands, then host or ares
run() {
	local d=$1 r=$2 t=$3 n=$4 code=0 cmds
	local e=$((t + n + 3))
	mkdir -p "$d/$r"
	cmds=(--cmd "@1 fixed_dt 33333" --cmd "@$((t + 1)) stats $((1000 + t + 1))"
		--cmd "@$((t + n)) stats $((1000 + t + n))" --cmd "@$e quit 0")
	case $r in
	A) cmds+=(--cmd "@$t save 0" --cmd "@$((t + n)) save 1") ;;
	B) cmds+=(--cmd "@1 load 0" --cmd "@$((t + n)) save 1") ;;
	C) ;;
	esac
	if [[ $MODE == host ]]; then
		[[ $r == B ]] && cp "$d/A/flash.fla" "$d/B/flash.fla"
		"$HOST" --root "$CONTENT" --route "$d/route.rbc" --save "$d/$r/flash.fla" "${cmds[@]}" \
			>"$d/$r/run.log" 2>&1 || code=$?
	else
		local rd="$BUILD_DIR/sr"
		"$HOST" --write-cmds "$d/$r/run.cmd" "${cmds[@]}"
		rm -f "$rd/content/route.rbc"
		cp "$d/route.rbc" "$rd/content/route.rbc"
		"${MK[@]}" BUILD_DIR="$rd" HL64_TEST=1 PROFILE=1 HL64_CONTENT="$rd/content" \
			HL64_CMDS="$d/$r/run.cmd" rom >"$d/$r/rom.log" 2>&1 || { tail -20 "$d/$r/rom.log" >&2; return 2; }
		mv "$rd/hl64.z64" "$d/$r/hl64.z64"
		(
			[[ $r == B ]] && export ARES_FLASH_IN="$d/A/flash.fla"
			export ARES_FLASH_OUT="$d/$r/flash.fla"
			ares_run "$d/$r" hl64.z64 run.raw.log
		) || code=$?
		tr -d '\r' <"$d/$r/run.raw.log" >"$d/$r/run.log" || true
		rm -f "$d/$r/run.raw.log" "$d/$r/hl64.z64"
	fi
	echo "$code" >"$d/$r/exit"
}

# the `stats L` lines that are simulation state (the player, the entity
# hash; the other lines carry counters since boot, wall time, heap, I/O)
sim_lines() {	# sim_lines <log> <label>
	grep -E "^stats $2 (player:|ents:)" "$1" || true
}

OUT="$BUILD_DIR/save/$MODE"
mkdir -p "$OUT"
SUM="$OUT/summary.md"
echo "# saved-game determinism ($MODE, $(date -u +%FT%TZ), content $CONTENT)" >"$SUM"
echo >>"$SUM"
echo "| scene | T | N | streams A/B at T+N | stats T+1 | stats T+N | C vs A (save no-op) | worst save frame | worst load frame | image |" >>"$SUM"
echo "|---|---|---|---|---|---|---|---|---|---|" >>"$SUM"
FAIL=0
for c in "${CASES[@]}"; do
	IFS=: read -r s t n <<<"$c"
	d="$OUT/$s-$t"
	rm -rf "$d"
	mkdir -p "$d"
	(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "test/scenes/$s.route" \
		-o "$d/route.rbc") >"$d/routec.log" 2>&1 || { cat "$d/routec.log" >&2; exit 2; }
	run "$d" A "$t" "$n"
	run "$d" C "$t" "$n"
	run "$d" B "$t" "$n"
	cmp=$(python3 "$ROOT/test/save/svdump.py" "${BE[@]}" "$d/A/flash.fla:1" "$d/B/flash.fla:1" 2>&1) \
		&& eq=equal || { eq="DIFFER"; FAIL=1; }
	echo "$cmp" >"$d/compare.txt"
	st=()
	for k in $((1000 + t + 1)) $((1000 + t + n)); do
		if diff <(sim_lines "$d/A/run.log" "$k") <(sim_lines "$d/B/run.log" "$k") >"$d/stats-$k.diff" \
			&& [[ -n "$(sim_lines "$d/A/run.log" "$k")" ]]; then st+=(equal); else st+=(DIFFER); FAIL=1; fi
	done
	if diff <(sim_lines "$d/A/run.log" $((1000 + t + n))) <(sim_lines "$d/C/run.log" $((1000 + t + n))) \
		>"$d/noop.diff"; then noop=same; else noop="differs (settle)"; fi
	ws=$(grep -E "^save: save slot 0 ok" "$d/A/run.log" | sed -n 's/.*worst frame \([0-9]*\) us.*/\1 us/p' | head -1)
	wl=$(grep -E "^save: load slot 0 " "$d/B/run.log" | sed -n 's/.*worst frame \([0-9]*\) us.*/\1 us/p' | head -1)
	img=$(grep -E "^save: slot 0 seq" "$d/A/run.log" | sed -n 's/.*: \([0-9]* B state -> [0-9]* B image\).*/\1/p' | head -1)
	echo "| $s | $t | $n | $eq | ${st[0]} | ${st[1]} | $noop | ${ws:-?} | ${wl:-?} | ${img:-?} |" >>"$SUM"
	echo "save: $s T=$t N=$n: streams $eq, stats ${st[*]}, C vs A $noop, save ${ws:-?}, load ${wl:-?}, ${img:-?}"
done
cat "$SUM"
exit $FAIL
