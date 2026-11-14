#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Remote ares runner: test/run/ares.sh's ares_run on another machine
# (HL64_ARES_HOST=ares-host), results copied back to the paths a local run
# writes, so the harness scripts cannot tell the two apart.
#
#   test/run/ares-remote.sh DIR ROM LOG [TIMEOUT]   one run (ares_run's contract)
#   test/run/ares-remote.sh --check                 the host answers, its ares exists
#
# A run: ROM (relative paths: to DIR), the ARES_TEST_INPUT file when set
# (copied once at the start: a static file; a pad file written while ares
# runs needs a local run) and test/run/{ares,ares-remote,xvfb}.sh go to
# HOST:~/hl64-runs/<id>/ (the ROM as a delta against the last ROM sent);
# there ares runs in <id>/run on its own Xvfb with ares.sh's ARES_SETTINGS
# and the forwarded harness variables, holding one of
# HL64_ARES_REMOTE_SLOTS (default 8) slot locks ~/hl64-runs/.slot<i>; then
# every file ares wrote in its working directory (ares-test-*.ppm, host
# shots without ARES_HOST_SHOT_DIR) comes back into DIR, its output to LOG,
# ARES_HOST_SHOT_DIR's shots to that directory. The last 50 run
# directories are kept on the host.
# Exit: ares' status (124: timeout), 255: the host could not be reached.
# Env:  HL64_ARES_HOST (default ares-host), HL64_ARES_SSH (ssh target, default
#       ares-host for ares-host, else HL64_ARES_HOST), HL64_ARES_REMOTE_SLOTS,
#       HL64_ARES_REMOTE_ICD (the host's Vulkan ICD file for parallel-RDP,
#       default /usr/share/vulkan/icd.d/nvidia_icd.json),
#       ARES_TIMEOUT, ARES_STAT_FILE (written: "<wall s> <load> <host>").
# Host setup (once): the patched ares at ~/hl64-ares/build/desktop-ui/ares
# (test/ares-testharness.patch, test/ares-large-rom-isviewer.patch; same
# CMake options as the local build), its settings in ~/hl64-ares/xdg/ares
# (XDG_DATA_HOME of every remote run), Xvfb, a Vulkan ICD. Measured on
# ares-host (hl2-yard scene ROM): the NVIDIA ICD and Mesa lavapipe (what a
# GPU-less machine like build-vm runs) give the same log and shots;
# NVIDIA ran it in 63 s alone, 144 s each with 8 at once (lavapipe 86 s /
# 283 s).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEEP_RUNS=50

# ---- the host side: bin/ares-remote.sh --remote-side ID TIMEOUT ROM [VAR=VALUE ...] ----
if [[ "${1:-}" == --remote-side ]]; then
	id=$2 to=$3 rom=$4
	shift 4
	R="$HOME/hl64-runs/$id"
	for kv in "$@"; do export "${kv?}"; done
	# shellcheck source=ares.sh
	. "$R/bin/ares.sh"
	ARES_BIN="$HOME/hl64-ares/build/desktop-ui/ares"
	export XDG_DATA_HOME="$HOME/hl64-ares/xdg"
	export VK_ICD_FILENAMES="${HL64_ARES_REMOTE_ICD:-/usr/share/vulkan/icd.d/nvidia_icd.json}"
	[[ -n "${ARES_TEST_INPUT:-}" ]] && export ARES_TEST_INPUT="$R/input/$ARES_TEST_INPUT"
	[[ -n "${ARES_HOST_SHOT_DIR:-}" ]] && { mkdir -p "$R/shots"; export ARES_HOST_SHOT_DIR="$R/shots"; }
	mkdir -p "$R/run"
	ares_slot_take "$HOME/hl64-runs/.slot" "${HL64_ARES_REMOTE_SLOTS:-8}"
	echo "ares-remote: $(hostname -s) run $id on ${ARES_SLOT##*/}" >&2
	DISPLAY_NUM=
	xvfb_start || { echo 2 >"$R/exit"; exit 2; }
	c=0
	ares_exec "$R/run" "$R/rom/$rom" "$R/ares.log" "$to" || c=$?
	kill "$XVFB_PID" 2>/dev/null || true
	ares_slot_drop
	echo "$ARES_WALL $ARES_LOAD $(hostname -s)" >"$R/wall"
	echo "$c" >"$R/exit"
	# the next run's ROM delta basis
	mkdir -p "$HOME/hl64-runs/.basis/rom"
	ln -f "$R/rom/$rom" "$HOME/hl64-runs/.basis/rom/.$rom.$$" \
		&& mv -f "$HOME/hl64-runs/.basis/rom/.$rom.$$" "$HOME/hl64-runs/.basis/rom/$rom"
	# keep the newest KEEP_RUNS run directories (ids sort by start time)
	find "$HOME/hl64-runs" -mindepth 1 -maxdepth 1 -type d -name '2*' -printf '%f\n' | sort -r \
		| tail -n +$((KEEP_RUNS + 1)) | while read -r old; do rm -rf "${HOME:?}/hl64-runs/$old"; done
	exit "$c"
fi

# ---- this side ----
HOST="${HL64_ARES_HOST:-ares-host}"
case "$HOST" in ares-host) SSH_TO="${HL64_ARES_SSH:-ares-host}" ;; *) SSH_TO="${HL64_ARES_SSH:-$HOST}" ;; esac
SSH=(ssh -o BatchMode=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=10)
export RSYNC_RSH="${SSH[*]}"

if [[ "${1:-}" == --check ]]; then
	"${SSH[@]}" -n "$SSH_TO" 'test -x ~/hl64-ares/build/desktop-ui/ares && command -v Xvfb >/dev/null' \
		|| { echo "ares-remote: $SSH_TO unreachable or no ~/hl64-ares/build/desktop-ui/ares / Xvfb there" >&2; exit 2; }
	exit 0
fi

[[ $# -ge 3 && $# -le 4 ]] || { echo "usage: test/run/ares-remote.sh DIR ROM LOG [TIMEOUT] | --check" >&2; exit 2; }
DIR="$(cd "$1" && pwd)"
abs() { case "$1" in /*) echo "$1" ;; *) echo "$DIR/$1" ;; esac; }
ROM="$(abs "$2")"
LOG="$(abs "$3")"
TO="${4:-${ARES_TIMEOUT:-1800}}"
[[ -f "$ROM" ]] || { echo "ares-remote: no ROM $ROM" >&2; exit 2; }
ID="$(date -u +%Y%m%d-%H%M%S)-$(hostname -s)-$$-$RANDOM"
RR="hl64-runs/$ID"

# stage: bin/, rom/, input/ (symlinks; rsync -L sends the files)
ST="$(mktemp -d "${TMPDIR:-/tmp}/ares-remote.XXXXXX")"
trap 'rm -rf "$ST"' EXIT
mkdir -p "$ST/bin" "$ST/rom"
for f in ares.sh ares-remote.sh xvfb.sh; do ln -s "$HERE/$f" "$ST/bin/$f"; done
ln -s "$ROM" "$ST/rom/${ROM##*/}"
FWD=()
if [[ -n "${ARES_TEST_INPUT:-}" ]]; then
	in="$(abs "$ARES_TEST_INPUT")"
	[[ -f "$in" ]] || { echo "ares-remote: no ARES_TEST_INPUT file $in" >&2; exit 2; }
	mkdir -p "$ST/input"
	ln -s "$in" "$ST/input/${in##*/}"
	FWD+=("ARES_TEST_INPUT=${in##*/}")
fi
SHOTS=
if [[ -n "${ARES_HOST_SHOT_DIR:-}" ]]; then
	SHOTS="$(abs "$ARES_HOST_SHOT_DIR")"
	FWD+=(ARES_HOST_SHOT_DIR=1)
fi
# shellcheck source=ares.sh
. "$HERE/ares.sh"
for v in "${ARES_FWD_VARS[@]}"; do
	[[ -n "${!v:-}" ]] && FWD+=("$v=${!v}")
done
[[ -n "${HL64_ARES_REMOTE_SLOTS:-}" ]] && FWD+=("HL64_ARES_REMOTE_SLOTS=$HL64_ARES_REMOTE_SLOTS")
[[ -n "${HL64_ARES_REMOTE_ICD:-}" ]] && FWD+=("HL64_ARES_REMOTE_ICD=$HL64_ARES_REMOTE_ICD")

"${SSH[@]}" -n "$SSH_TO" "mkdir -p hl64-runs/.basis" || { echo "ares-remote: $SSH_TO unreachable" >&2; exit 255; }
rsync -rtLz --mkpath --copy-dest=../.basis "$ST/" "$SSH_TO:$RR/" \
	|| { echo "ares-remote: copy to $SSH_TO:$RR failed" >&2; exit 255; }
args=(bash "$RR/bin/ares-remote.sh" --remote-side "$ID" "$TO" "${ROM##*/}" "${FWD[@]}")
c=0
"${SSH[@]}" -n "$SSH_TO" "$(printf '%q ' "${args[@]}")" || c=$?

# results: ares' working directory into DIR, its output to LOG, the shots
mkdir -p "$ST/back"
rsync -tz "$SSH_TO:$RR/ares.log" ":$RR/wall" ":$RR/exit" "$ST/back/" 2>/dev/null \
	|| { echo "ares-remote: run $ID on $SSH_TO left no result (ssh status $c)" >&2; exit 255; }
rsync -rtz "$SSH_TO:$RR/run/" "$DIR/" || { echo "ares-remote: copy back from $SSH_TO:$RR/run failed" >&2; exit 255; }
if [[ -n "$SHOTS" ]]; then
	mkdir -p "$SHOTS"
	rsync -rtz "$SSH_TO:$RR/shots/" "$SHOTS/" || { echo "ares-remote: copy back of the shots failed" >&2; exit 255; }
fi
mv -f "$ST/back/ares.log" "$LOG"
[[ -n "${ARES_STAT_FILE:-}" ]] && cp "$ST/back/wall" "$ARES_STAT_FILE"
exit "$(cat "$ST/back/exit")"
