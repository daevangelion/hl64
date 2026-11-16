#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Saved-game determinism (docs/architecture.md 14), four runs of a scene:
#   S  saves at frame T (FlashRAM slot 1; --pak: the Controller Pak's
#      first entry, list index 3) and quits (the quit waits for the
#      write): the image B loads;
#   A  saves into slot 1 at T and goes on;
#   B  a fresh boot with S's FlashRAM / Pak image: loads S's save at frame
#      1 and goes on;
#   C  never saves (seam crossings still autosave, as in A and B).
# (a) Restore: B's load stages the restored game again at once (test
#     builds, engine/save/save.c restore_check: the snapshot's sections
#     right after the restore) and it must be S's saved state byte for byte.
# (b) Continuation: at frame T+N runs A and B save into slot 1 again: their
#     state streams (everything a save holds: components, private blocks,
#     records, timers, the game's globals, clock, random stream, ...) must
#     be equal byte for byte (test/save/svdump.py), and their `stats`
#     entity hash / player / record lines at frames T+1 and T+N equal.
#     SCENE:T:N:report reports (b) without failing on it: a window where a
#     map opens, whose completion frame follows the render pool's state
#     (warm in A, cold after B's load), not the game's.
# C against A tells whether the save itself (its settle: paused AI thinks,
# opens in flight) changed the game. Gate: every autosave (slot 0, any
# run) ok, and its cost per frame <= 67 ms: the larger of its `longest
# held frame` (frames it held: no game step, all of it ours) and its
# `worst step` (its own work in a frame the game also ran); its `longest
# frame` (game included) is in the run logs. A manual save's and a load's
# cost per frame and whole time are reported, not gated (architecture.md
# 14: the game is paused behind the menu's progress, the sound muted).
#
#   test/save/determinism.sh [--ares] [--pak] SCENE:T:N[:report] ...
#
# host (default): hl64-host at a fixed 1/30 s, image files --save / --pak.
# --ares: one HL64_TEST ROM per run (the ELF built once), each ares run
#   through test/run/ares.sh (HL64_ARES_HOST=ares-host), the images carried
#   from S to B by ARES_FLASH_OUT / ARES_FLASH_IN (ARES_PAK_OUT / _IN): B
#   is a fresh ares launch that loads what S's ares wrote (save, quit,
#   relaunch, load).
# Env: BUILD_DIR (default build), HL64_CONTENT (default
#   /scratch/hl64/tmp/Int9/c23), ARES_TIMEOUT (s per run, default 1800).
# Out: $BUILD_DIR/save/<host|ares>[-pak]/<scene>-<T>/{S,A,B,C}/ logs,
#   images; summary.md beside them. Exit 0 every scene equal, 1 a
#   difference or a failed gate, 2 usage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int9/c23}"
MODE=host
CASES=()
PAK=0
usage() { echo "usage: test/save/determinism.sh [--ares] [--pak] SCENE:T:N ..." >&2; exit 2; }
while [[ $# -gt 0 ]]; do
	case "$1" in
	--ares) MODE=ares ;;
	--host) MODE=host ;;
	--pak) PAK=1 ;;
	*:*:*) CASES+=("$1") ;;
	*) usage ;;
	esac
	shift
done
# S's slot and B's: FlashRAM slot 1, or the Pak's first entry (after the
# autosave and the two FlashRAM slots, include/hl64/save.h)
SL=1
[[ $PAK == 1 ]] && SL=3
[[ ${#CASES[@]} -gt 0 ]] || usage
[[ -f "$CONTENT/campaign.rtb" ]] || { echo "no $CONTENT/campaign.rtb: cook the content" >&2; exit 2; }

mkdir -p "$BUILD_DIR"
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

# run <dir> <S|A|B|C> <T> <N>: one run's commands, then host or ares
run() {
	local d=$1 r=$2 t=$3 n=$4 code=0 cmds
	local e=$((t + n + 3))
	# the cart codes a save over many frames: a T+N save queued behind an
	# autosave must start before the quit (a quit waits only for one running)
	[[ $MODE == ares ]] && e=$((t + n + 90))
	mkdir -p "$d/$r"
	cmds=(--cmd "@1 fixed_dt 33333" --cmd "@$((t + 1)) stats $((1000 + t + 1))"
		--cmd "@$((t + n)) stats $((1000 + t + n))")
	case $r in
	S) cmds+=(--cmd "@$t save $SL"); e=$((t + 1)) ;;
	A) cmds+=(--cmd "@$t save 1" --cmd "@$((t + n)) save 1") ;;
	B) cmds+=(--cmd "@1 load $SL" --cmd "@$((t + n)) save 1") ;;
	C) ;;
	esac
	cmds+=(--cmd "@$e quit 0")
	if [[ $MODE == host ]]; then
		local pk=()
		[[ $r == B ]] && cp "$d/S/flash.fla" "$d/B/flash.fla"
		if [[ $PAK == 1 && ( $r == S || $r == B ) ]]; then
			[[ $r == B ]] && cp "$d/S/pak.img" "$d/B/pak.img"
			pk=(--pak "$d/$r/pak.img")
		fi
		"$HOST" --root "$CONTENT" --route "$d/route.rbc" --save "$d/$r/flash.fla" "${pk[@]}" \
			"${cmds[@]}" >"$d/$r/run.log" 2>&1 || code=$?
	else
		local rd="$BUILD_DIR/sr"
		"$HOST" --write-cmds "$d/$r/run.cmd" "${cmds[@]}"
		rm -f "$rd/content/route.rbc"
		cp "$d/route.rbc" "$rd/content/route.rbc"
		"${MK[@]}" BUILD_DIR="$rd" HL64_TEST=1 PROFILE=1 HL64_CONTENT="$rd/content" \
			HL64_CMDS="$d/$r/run.cmd" rom >"$d/$r/rom.log" 2>&1 || { tail -20 "$d/$r/rom.log" >&2; return 2; }
		mv "$rd/hl64.z64" "$d/$r/hl64.z64"
		(
			[[ $r == B ]] && export ARES_FLASH_IN="$d/S/flash.fla"
			export ARES_FLASH_OUT="$d/$r/flash.fla"
			if [[ $PAK == 1 ]]; then
				[[ $r == B ]] && export ARES_PAK_IN="$d/S/pak.img"
				export ARES_PAK_OUT="$d/$r/pak.img"
			fi
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
[[ $PAK == 0 ]] || OUT+=-pak
mkdir -p "$OUT"
SUM="$OUT/summary.md"
echo "# saved-game determinism ($MODE, $(date -u +%FT%TZ), content $CONTENT)" >"$SUM"
echo >>"$SUM"
echo "| scene | T | N | (a) B's re-save = S's image | (b) streams A/B at T+N | stats T+1 | stats T+N | C vs A (save no-op) | save: longest frame, time | load: longest frame, time | autosaves: longest frame | image | latency |" >>"$SUM"
echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|" >>"$SUM"
# a save's, an autosave's or a load's cost per frame, us: max(longest held
# frame, worst step) of its log line; its whole time, ms
cost() { sed -n 's/.*worst step \([0-9]*\) us, longest held frame \([0-9]*\) us.*/\1 \2/p' \
	| awk '{print ($1 > $2 ? $1 : $2)}'; }
whole() { sed -n 's/.* \([0-9]*\) ms$/\1/p'; }
FRAME_MAX=67000
# save latency (test/save/latency.py, ares only: the host's image file ends
# every operation at once): an autosave's chip time (its flash writes) and a
# manual save's request -> commit. Measured on the meeq ares (2026-11-16,
# flash ticks): autosaves 0.6-0.8 s of chip time (13 KB), manual saves
# 1.9-7.2 s request -> commit (hl2-return-battle's 7.2 s is mostly its coder:
# 6.4 s with the old instant chip); limits ~1.25x those.
AUTO_CHIP_MS=1000
MANUAL_MS=9000
LAT=()
[[ $MODE == ares ]] && LAT=(--auto-chip-ms "$AUTO_CHIP_MS" --manual-ms "$MANUAL_MS")
FAIL=0
for c in "${CASES[@]}"; do
	IFS=: read -r s t n mode <<<"$c"
	# (b) fails the gate unless the case reports it only
	CF=1
	[[ ${mode:-} == report ]] && CF=0
	d="$OUT/$s-$t"
	rm -rf "$d"
	mkdir -p "$d"
	(cd "$ROOT" && python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "test/scenes/$s.route" \
		-o "$d/route.rbc") >"$d/routec.log" 2>&1 || { cat "$d/routec.log" >&2; exit 2; }
	run "$d" S "$t" "$n"
	run "$d" A "$t" "$n"
	run "$d" C "$t" "$n"
	run "$d" B "$t" "$n"
	# (a) the game B restored, saved again at once (test builds' re-save
	# right after the restore, engine/save/save.c restore_check) = the state S saved
	rs=$(sed -n "s/^load: slot $SL re-save: //p" "$d/B/run.log" | head -1)
	[[ $rs == equal* ]] || { rs="${rs:-none}"; FAIL=1; }
	cmp=$(python3 "$ROOT/test/save/svdump.py" "${BE[@]}" "$d/A/flash.fla:1" "$d/B/flash.fla:1" 2>&1) \
		&& eq=equal || { eq="DIFFER"; [[ $CF == 0 ]] || FAIL=1; }
	echo "$cmp" >"$d/compare.txt"
	st=()
	for k in $((1000 + t + 1)) $((1000 + t + n)); do
		if diff <(sim_lines "$d/A/run.log" "$k") <(sim_lines "$d/B/run.log" "$k") >"$d/stats-$k.diff" \
			&& [[ -n "$(sim_lines "$d/A/run.log" "$k")" ]]; then st+=(equal); else st+=(DIFFER); [[ $CF == 0 ]] || FAIL=1; fi
	done
	if diff <(sim_lines "$d/A/run.log" $((1000 + t + n))) <(sim_lines "$d/C/run.log" $((1000 + t + n))) \
		>"$d/noop.diff"; then noop=same; else noop="differs (settle)"; fi
	# longest whole frame of the saves (S's, A's two) and of B's load
	ws=$( { grep -hE "^save: save slot $SL (ok|FAILED)" "$d/S/run.log" || true;
		grep -hE "^save: save slot 1 (ok|FAILED)" "$d/A/run.log" || true; } \
		| cost | sort -n | tail -1)
	ts=$( { grep -hE "^save: save slot $SL (ok|FAILED)" "$d/S/run.log" || true;
		grep -hE "^save: save slot 1 (ok|FAILED)" "$d/A/run.log" || true; } \
		| whole | sort -n | tail -1)
	wl=$( (grep -E "^save: load slot $SL " "$d/B/run.log" || true) \
		| cost | head -1)
	tl=$( (grep -E "^save: load slot $SL " "$d/B/run.log" || true) \
		| whole | head -1)
	img=$( (grep -E "^save: slot 1 seq" "$d/A/run.log" || true) \
		| sed -n 's/.*: \([0-9]* B state -> [0-9]* B image\).*/\1/p' | head -1)
	nok=$(( $(grep -c "^save: save slot $SL ok" "$d/S/run.log" || true) \
		+ $(grep -c "^save: save slot 1 ok" "$d/A/run.log" || true) ))
	[[ $nok == 3 ]] || { ws="$ws (a save FAILED)"; FAIL=1; }
	grep -q "^save: load slot $SL ok" "$d/B/run.log" || { wl="$wl (load FAILED)"; FAIL=1; }
	ws="${ws:-?} us, ${ts:-?} ms"
	wl="${wl:-?} us, ${tl:-?} ms"
	# the autosaves of every run (slot 0: trigger_autosave, seam crossings):
	# each ok, each frame within the gate
	wa=$( (grep -hE "^save: save slot 0 (ok|FAILED)" "$d"/{S,A,B,C}/run.log || true) \
		| cost | sort -n | tail -1)
	na=$(cat "$d"/{S,A,B,C}/run.log | grep -c "^save: save slot 0 ok" || true)
	nf=$(cat "$d"/{S,A,B,C}/run.log | grep -c "^save: save slot 0 FAILED" || true)
	[[ -z $wa || $wa -le $FRAME_MAX ]] || FAIL=1
	[[ $nf == 0 ]] || FAIL=1
	wa="${na} ok${wa:+, $wa us}"
	[[ $nf == 0 ]] || wa="$wa, $nf FAILED"
	for r in S A B C; do
		[[ $(cat "$d/$r/exit") == 0 ]] || { eq="$eq, run $r exit $(cat "$d/$r/exit")"; FAIL=1; }
	done
	[[ $CF == 1 ]] || eq="$eq (reported)"
	# every save's coder frames, chip time and request -> commit
	lat=ok
	python3 "$ROOT/test/save/latency.py" "${LAT[@]}" "$d"/{S,A,B,C}/run.log >"$d/latency.txt" \
		|| { lat=FAIL; FAIL=1; }
	lat="$lat (autosave chip max $(sed -n 's/.* autosave ok: .*, chip \([0-9]*\) ms.*/\1/p' "$d/latency.txt" \
		| sort -n | tail -1) ms, manual request->commit max $(sed -n \
		's/.* manual [A-Z]*[a-z]*: .*request->commit \([0-9]*\) ms.*/\1/p' "$d/latency.txt" | sort -n | tail -1) ms)"
	echo "| $s | $t | $n | $rs | $eq | ${st[0]} | ${st[1]} | $noop | $ws | $wl | $wa | ${img:-?} | $lat |" >>"$SUM"
	echo "save: $s T=$t N=$n: restore $rs, streams $eq, stats ${st[*]}, C vs A $noop, save $ws, load $wl, autosaves $wa, ${img:-?}, latency $lat"
	sed 's/^/  /' "$d/latency.txt"
done
cat "$SUM"
exit $FAIL
