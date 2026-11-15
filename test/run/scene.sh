#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Scene runner (test/README.md): short test/scenes/*.route scenes (<= 60 s
# game time, one map visit or one seam switch each), a per-scene perf table
# (test/scenes/table.py over the scene's `stats begin` .. `stats end`
# window) and shots, so a change is checked in minutes on the scenes it
# touches; the whole routes (test/run/m5.sh) stay the final gate.
#
#   host (default): hl64-host per scene at a fixed 1/30 s, -j N in
#          parallel; route PASS, the table with --profile (host CPU, not the
#          cart's)
#   --ares: one HL64_TEST PROFILE ROM per scene (the ELF is built once; a
#          scene only re-packs the DFS with its route.rbc), each ares run
#          through test/run/ares.sh ares_run (a local slot lock, or on
#          HL64_ARES_HOST), up to -j N runs at a time, with its wall time;
#          shots as shots/NN-<label>.png
#
# Options:
#   --profile          host: PROFILE build (ares runs are always PROFILE)
#   --psamp            ares: PSAMP=1 ROM (a tree with plat/n64/psamp.c) and
#                      test/m6/psamp.py per scene, when the tree has it
#   --before TREE      also build TREE (another worktree) into
#                      $BUILD_DIR/before and run every scene on it (this
#                      tree's scene sources and routec, compiled against
#                      BEFORE_CONTENT's rtb, default HL64_CONTENT: set it
#                      when TREE needs another cook), before and after
#                      alternating per scene; tables side by side
#   --route-log R=LOG  compare each scene carrying `# scene-window: R FROM TO`
#                      with that window of a whole-route log (table.py
#                      LOG@FROM:TO beside the scene's own window); its
#                      `# scene-part: NAME FROM TO` sub-windows (SHOT / STATS
#                      labels the scene shares with route R) likewise
#   --keep-rom         keep each scene's hl64.z64 (default: removed after
#                      its run)
#   --cmd "[@F] CMD"   one more debug command for every scene (hl64-host
#                      --cmd, e.g. "@2 set brightness 50"); repeatable
#   -j N               scenes in parallel (default 4; ares runs also wait
#                      for a slot, test/run/ares.sh)
#   SCENE ... | all    names under test/scenes (without .route)
# Env:   BUILD_DIR (scratch, default build), HL64_CONTENT (cooked content,
#        default /scratch/hl64/tmp/Int6/c20), BEFORE_CONTENT,
#        ARES_TIMEOUT (s per scene, default 1800), HL64_ARES_HOST / ARES_BIN /
#        ARES_SLOTS (test/run/ares.sh), DISPLAY_NUM, MAKE_JOBS (default 4)
# Out:   $BUILD_DIR/scenes/<host|ares>/<scene>/{route.rbc,route.dump,
#        scene.cmd,host.log|ares.log,table.md,wall.txt,shots/,psamp.md}
#        (before: $BUILD_DIR/before/scenes/...), summary.md beside them.
# Exit:  0 every scene PASS, 1 a scene failed, 2 prerequisite / usage.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
CONTENT="${HL64_CONTENT:-/scratch/hl64/tmp/Int6/c20}"
usage() {
	echo "usage: test/run/scene.sh [--ares|--host] [--profile] [--psamp] [--before TREE]" \
		"[--route-log R=LOG ...] [--keep-rom] [--cmd CMD ...] [-j N] SCENE... | all" >&2
	exit 2
}
MODE=host
PROFILE=0
PSAMP=0
BEFORE=
KEEP=0
JOBS=4
SCENES=()
XCMDS=()
declare -A RLOG
while [[ $# -gt 0 ]]; do
	case "$1" in
	--ares) MODE=ares ;;
	--host) MODE=host ;;
	--profile) PROFILE=1 ;;
	--psamp) PSAMP=1 ;;
	--keep-rom) KEEP=1 ;;
	--before) [[ -n "${2:-}" ]] || usage; BEFORE="$(cd "$2" && pwd)"; shift ;;
	--route-log)
		[[ "${2:-}" == *=* ]] || usage
		[[ -f "${2#*=}" ]] || { echo "no route log ${2#*=}" >&2; exit 2; }
		RLOG[${2%%=*}]="${2#*=}"
		shift
		;;
	--cmd) [[ -n "${2:-}" ]] || usage; XCMDS+=(--cmd "$2"); shift ;;
	-j) [[ "${2:-}" =~ ^[0-9]+$ ]] || usage; JOBS=$2; shift ;;
	-*) usage ;;
	*) SCENES+=("$1") ;;
	esac
	shift
done
[[ ${#SCENES[@]} -gt 0 ]] || usage
if [[ "${SCENES[*]}" == all ]]; then
	SCENES=()
	for f in "$ROOT"/test/scenes/*.route; do SCENES+=("$(basename "$f" .route)"); done
fi
for s in "${SCENES[@]}"; do
	[[ -f "$ROOT/test/scenes/$s.route" ]] || { echo "no scene test/scenes/$s.route" >&2; exit 2; }
done
RTB="$CONTENT/campaign.rtb"
[[ -f "$RTB" && -f "$CONTENT/maps/campaign.cwg" ]] || { echo "no $RTB / campaign.cwg: cook the content" >&2; exit 2; }
[[ $MODE == ares ]] && PROFILE=1
TAB="$ROOT/test/scenes/table.py"
CMDS=(--cmd "@1 fixed_dt 33333" "${XCMDS[@]}")	# fixed frame time (think-and-time.md 1)

# tree id -> source tree, build dir, content; "after" is this tree
TREES=(after)
declare -A TREE BDIR CT
TREE[after]="$ROOT"
BDIR[after]="$BUILD_DIR"
CT[after]="$CONTENT"
if [[ -n "$BEFORE" ]]; then
	TREES=(before after)
	TREE[before]="$BEFORE"
	BDIR[before]="$BUILD_DIR/before"
	CT[before]="${BEFORE_CONTENT:-$CONTENT}"
	[[ -f "${CT[before]}/campaign.rtb" ]] || { echo "no ${CT[before]}/campaign.rtb" >&2; exit 2; }
fi
if [[ $PSAMP -eq 1 ]]; then
	[[ $MODE == ares ]] || { echo "--psamp needs --ares" >&2; exit 2; }
	for t in "${TREES[@]}"; do
		grep -q '^ifdef PSAMP' "${TREE[$t]}/Makefile" || { echo "${TREE[$t]}: no PSAMP in its Makefile" >&2; exit 2; }
	done
fi

# ---- builds: host (route compile check, --write-cmds, host runs); ROM ELF ----
HOSTSUB=sh
[[ $PROFILE -eq 1 && $MODE == host ]] && HOSTSUB=sh-prof
ROMSUB=sr
[[ $PSAMP -eq 1 ]] && ROMSUB=sr-psamp
MK=(make --no-print-directory -j"${MAKE_JOBS:-4}")
build() {	# build <tree id>
	local t=$1 d="${BDIR[$1]}" flags=()
	mkdir -p "$d"
	[[ $HOSTSUB == sh-prof ]] && flags=(PROFILE=1)
	echo "scene: build $t (${TREE[$t]}) -> $d"
	"${MK[@]}" -C "${TREE[$t]}" BUILD_DIR="$d/$HOSTSUB" "${flags[@]}" host >"$d/$HOSTSUB.log" 2>&1 \
		|| { tail -30 "$d/$HOSTSUB.log" >&2; echo "scene: host build failed ($t)" >&2; exit 2; }
	if [[ $MODE == ares ]]; then
		flags=(HL64_TEST=1 PROFILE=1)
		[[ $PSAMP -eq 1 ]] && flags+=(PSAMP=1)
		"${MK[@]}" -C "${TREE[$t]}" BUILD_DIR="$d/$ROMSUB" "${flags[@]}" elf >"$d/$ROMSUB-elf.log" 2>&1 \
			|| { tail -30 "$d/$ROMSUB-elf.log" >&2; echo "scene: ROM ELF build failed ($t)" >&2; exit 2; }
		rm -rf "$d/$ROMSUB/content"
		cp -al "${CT[$t]}" "$d/$ROMSUB/content"
	fi
}
for t in "${TREES[@]}"; do build "$t"; done

# ---- scene routes: this tree's sources and routec, against each tree's rtb ----
OUT="$BUILD_DIR/scenes/$MODE"
mkdir -p "$OUT"
for s in "${SCENES[@]}"; do
	for t in "${TREES[@]}"; do
		d="${BDIR[$t]}/scenes/$MODE/$s"
		rm -rf "$d"
		mkdir -p "$d"
		(cd "$ROOT" && python3 -m tools.routec.routec --rtb "${CT[$t]}/campaign.rtb" "test/scenes/$s.route" \
			-o "$d/route.rbc" && python3 -m tools.routec.routec --rtb "${CT[$t]}/campaign.rtb" \
			--dump "$d/route.rbc" >"$d/route.dump") >"$d/routec.log" 2>&1 \
			|| { cat "$d/routec.log" >&2; echo "scene: routec $s ($t) failed" >&2; exit 2; }
		"${BDIR[$t]}/$HOSTSUB/host/hl64-host" --write-cmds "$d/scene.cmd" "${CMDS[@]}"
	done
done

# ---- runs ----
host_run() {	# host_run <tree id> <scene>
	local d="${BDIR[$1]}/scenes/host/$2" code=0
	"${BDIR[$1]}/$HOSTSUB/host/hl64-host" --root "${CT[$1]}" --route "$d/route.rbc" "${CMDS[@]}" \
		>"$d/host.log" 2>&1 || code=$?
	echo "$code" >"$d/exit"
}

shots() {	# shots <dir>: ares-test-NNNN.ppm -> shots/NN-<SHOT label>.png
	python3 - "$1" <<'EOF'
import os, re, sys
from PIL import Image
d = sys.argv[1]
labels = [m.group(1) for m in (re.match(r"^route: \d+ SHOT (\S+)", l) for l in open(os.path.join(d, "ares.log"), errors="replace")) if m]
ppm = sorted(f for f in os.listdir(d) if re.match(r"ares-test-\d{4}\.ppm$", f))
os.makedirs(os.path.join(d, "shots"), exist_ok=True)
for i, f in enumerate(ppm):
    name = "%02d-%s.png" % (i, labels[i] if i < len(labels) else "extra")
    Image.open(os.path.join(d, f)).save(os.path.join(d, "shots", name))
    os.remove(os.path.join(d, f))
EOF
}

scene_ares() {	# scene_ares <tree id> <scene>: the ROM here, the ares run in the background
	local t=$1 s=$2 b="${BDIR[$1]}" d="${BDIR[$1]}/scenes/ares/$2"
	local rd="$b/$ROMSUB" flags=(HL64_TEST=1 PROFILE=1)
	[[ $PSAMP -eq 1 ]] && flags+=(PSAMP=1)
	rm -f "$rd/content/route.rbc"	# a hard link into the content after cp -al: never write through it
	cp "$d/route.rbc" "$rd/content/route.rbc"
	make -C "${TREE[$t]}" --no-print-directory BUILD_DIR="$rd" "${flags[@]}" \
		HL64_CONTENT="$rd/content" HL64_CMDS="$d/scene.cmd" rom >"$d/rom-build.log" 2>&1 \
		|| { tail -20 "$d/rom-build.log" >&2; echo 2 >"$d/exit"; return 0; }
	mv "$rd/hl64.z64" "$d/hl64.z64"
	throttle
	(
		code=0
		ares_run "$d" hl64.z64 ares.raw.log || code=$?
		echo "$ARES_WALL s (load $ARES_LOAD, $ARES_RAN_ON)" >"$d/wall.txt"
		tr -d '\r' <"$d/ares.raw.log" >"$d/ares.log"
		rm -f "$d/ares.raw.log"
		[[ $KEEP -eq 1 ]] || rm -f "$d/hl64.z64"
		shots "$d" || echo "scene: $s ($t): shot conversion failed" >&2
		echo "$code" >"$d/exit"
		if [[ $PSAMP -eq 1 && -f "${TREE[$t]}/test/m6/psamp.py" ]]; then
			python3 "${TREE[$t]}/test/m6/psamp.py" "$d/ares.log" "$rd/hl64.elf" --by-seg --top 15 >"$d/psamp.md" 2>&1 || true
		fi
		echo "scene: $s ($t): exit $code, $(cat "$d/wall.txt")"
	) &
	PIDS+=($!)
}

# the scene runs in the background (not the private Xvfb ares_setup starts
# as a job of this shell: it never ends)
PIDS=()
throttle() {
	local n p
	while :; do
		n=0
		for p in "${PIDS[@]}"; do kill -0 "$p" 2>/dev/null && n=$((n + 1)); done
		[[ $n -ge $JOBS ]] || return 0
		wait -n || true
	done
}

if [[ $MODE == ares ]]; then
	# shellcheck source=ares.sh
	. "$ROOT/test/run/ares.sh"
	ares_setup || exit 2
fi

for s in "${SCENES[@]}"; do
	for t in "${TREES[@]}"; do
		if [[ $MODE == ares ]]; then
			scene_ares "$t" "$s"
		else
			throttle
			host_run "$t" "$s" &
			PIDS+=($!)
		fi
	done
done
for p in "${PIDS[@]}"; do wait "$p" || true; done

# ---- results ----
FAILS=()
SUM="$OUT/summary.md"
{
	echo "# scenes ($MODE, $(date -u +%FT%TZ), content ${CT[after]}${BEFORE:+; before $BEFORE on ${CT[before]:-}})"
	echo
	echo "| scene | tree | result | game s | window frames | avg ms | p90 ms | max ms | > 50 ms | mon-dom | AI-pushed | wall |"
	echo "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"
} >"$SUM"
for s in "${SCENES[@]}"; do
	logs=()
	for t in "${TREES[@]}"; do
		d="${BDIR[$t]}/scenes/$MODE/$s"
		log="$d/$MODE.log"
		code="$(cat "$d/exit" 2>/dev/null || echo 2)"
		res=PASS
		if [[ $code -ne 0 ]] || ! grep -q '^route PASS' "$log" 2>/dev/null; then
			res="FAIL$(grep -m1 '^route FAIL' "$log" 2>/dev/null | sed 's/^route FAIL/:/' || true)"
			[[ $code -eq 124 ]] && res="FAIL: timeout"
			FAILS+=("$s ($t)")
		fi
		# game time: the last stats record's frame (fixed 1/30 s frames)
		gs=$(sed -n 's/^stats [0-9]* frame=\([0-9]*\) .*/\1/p' "$log" 2>/dev/null | tail -1)
		gs=$([[ -n "$gs" ]] && awk -v n="$gs" 'BEGIN{printf "%.1f", n / 30}' || echo -)
		wall="$(cat "$d/wall.txt" 2>/dev/null || echo -)"
		line=
		if [[ $PROFILE -eq 1 ]] && line="$(python3 "$TAB" "$log" --summary "$s" 2>/dev/null)"; then
			# scene NAME frames=F avg=A p90=P max=M over50=O slow=S mon_dom=D ai_pushed=X
			read -r -a f <<<"$line"
			v() { local k; for k in "${f[@]}"; do [[ $k == "$1="* ]] && echo "${k#*=}"; done; }
			echo "| $s | $t | $res | $gs | $(v frames) | $(v avg) | $(v p90) | $(v max) | $(v over50) | $(v mon_dom) | $(v ai_pushed) | $wall |" >>"$SUM"
			logs+=("$log")
		else
			# no PROFILE roll-up inside the window (a host run's roll-ups are
			# 1 s of host time: a short scene may have none)
			echo "| $s | $t | $res | $gs | - | - | - | - | - | - | - | $wall |" >>"$SUM"
		fi
	done
	[[ $PROFILE -eq 1 && ${#logs[@]} -gt 0 ]] || continue
	d="$OUT/$s"
	{
		if [[ ${#logs[@]} -eq 2 ]]; then
			python3 "$TAB" "${logs[0]}" "${logs[1]}" --title "$s (before / after)" --label before --label after
		else
			python3 "$TAB" "${logs[0]}" --title "$s ($MODE)" --label "$MODE"
		fi
		# the scene's window in a whole-route log, then its parts (sub-windows
		# by labels the scene shares with that route)
		win="$(sed -n 's/^# scene-window: //p' "$ROOT/test/scenes/$s.route" | head -1)"
		r=
		[[ -n "$win" ]] && read -r r from to <<<"$win"
		if [[ -n "$r" && -n "${RLOG[$r]:-}" ]]; then
			python3 "$TAB" "${RLOG[$r]}@$from:$to" "${logs[-1]}" --title "$s vs $r route ($from .. $to)" \
				--label "$r route" --label scene
		fi
		while read -r part from to; do
			if [[ -n "$r" && -n "${RLOG[$r]:-}" ]]; then
				python3 "$TAB" "${RLOG[$r]}@$from:$to" "${logs[-1]}@$from:$to" \
					--title "$s part $part vs $r route ($from .. $to)" --label "$r route" --label scene
			elif [[ ${#logs[@]} -eq 2 ]]; then
				python3 "$TAB" "${logs[0]}@$from:$to" "${logs[1]}@$from:$to" --title "$s part $part ($from .. $to)" \
					--label before --label after
			else
				python3 "$TAB" "${logs[0]}@$from:$to" --title "$s part $part ($from .. $to)" --label "$MODE"
			fi
		done < <(sed -n 's/^# scene-part: //p' "$ROOT/test/scenes/$s.route")
	} >"$d/table.md" 2>&1 || true
done
cat "$SUM"
for s in "${SCENES[@]}"; do [[ -f "$OUT/$s/table.md" ]] && cat "$OUT/$s/table.md"; done
if [[ ${#FAILS[@]} -ne 0 ]]; then
	echo "scene FAIL: ${FAILS[*]}"
	exit 1
fi
echo "scene PASS (${#SCENES[@]} scenes, $MODE)"
