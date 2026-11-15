# SPDX-License-Identifier: MIT
# Sourced by the ares harness scripts: every ares run goes through ares_run,
# here or on another machine (test/run/ares-remote.sh).
#
#   ares_setup               once, before the first run: local runs check
#                            ARES_BIN and start a private Xvfb (xvfb.sh,
#                            killed on EXIT); remote runs check the host
#   ares_run DIR ROM LOG [TIMEOUT]
#                            the patched ares on ROM with DIR as its working
#                            directory (relative ROM / LOG / ARES_TEST_INPUT
#                            paths: to DIR), ARES_TEST_HARNESS=1 and
#                            ARES_SETTINGS, stdout + stderr to LOG, at most
#                            TIMEOUT s (default ARES_TIMEOUT, else 1800);
#                            returns ares' status (124: timeout). Sets
#                            ARES_WALL (s the emulator ran, slot wait and
#                            copies excluded), ARES_LOAD (the 1 min load
#                            average on the machine that ran it at its end)
#                            and ARES_RAN_ON (local or the host).
#
# Live runs: LOG grows while ares runs (remote: streamed back) and
# ARES_TEST_INPUT may be rewritten during the run (remote: every change is
# sent over one ssh stream, ~0.1 s polling), so a script can start
# `ares_run ... &`, wait for log lines and press pad buttons as on a local
# run. SIGTERM / SIGINT to that background ares_run stops ares (remote: on
# the host) and still copies the results back.
#
# Saved games (FlashRAM; the ROM's header save type flashram, Makefile
# N64_ROM_SAVETYPE): a run starts from an erased chip, never from a save a
# previous run left. ARES_FLASH_IN=<file> seeds the chip from that 128 KB raw
# image before boot; ARES_FLASH_OUT=<file> gets the chip's image when the
# guest exits (quit through the harness; a timeout or kill leaves none --
# the stale file is removed before the run, and a run that wrote none says
# so on stderr). Relative paths: to DIR; remote runs stage the IN file and
# fetch the OUT file like every other result. The patched ares does the
# work (test/ares-flash.patch): its harness device loads the image at cart
# connect and dumps it at the guest's exit, which skips ares' own save.
#
# Where: HL64_ARES_HOST unset / empty / local: on this machine, holding one
# of ARES_SLOTS (default 2) slot locks /tmp/hl64-ares.slot<i> while ares
# runs (every ares user on the machine takes one). HL64_ARES_HOST=<host>
# (ares-host): test/run/ares-remote.sh, the same run on that host (its own
# slots, results copied back to the same paths).
# Env: ARES_BIN (local, default ~/src/products/ares/build/desktop-ui/ares),
# ARES_SLOTS, ARES_TIMEOUT, DISPLAY_NUM (xvfb.sh), and the harness variables
# ares reads (ARES_TEST_INPUT, ARES_HOST_SCREENSHOT, ARES_HOST_SHOT_EVERY_MS,
# ARES_HOST_SHOT_MAX, ARES_HOST_SHOT_DIR, ARES_HOST_PRINTF, ARES_TRACE_CPU),
# passed through (remote: forwarded).

# ares_flash_check DIR: ARES_FLASH_IN must be a file (else 2); ARES_FLASH_OUT's
# directory is made and a stale file there removed.
ares_flash_check() {
	local dir=$1 f
	if [[ -n "${ARES_FLASH_IN:-}" ]]; then
		f=$(ares_abs "$dir" "$ARES_FLASH_IN")
		[[ -f "$f" ]] || { echo "ares: ARES_FLASH_IN $f is not a file" >&2; return 2; }
	fi
	if [[ -n "${ARES_FLASH_OUT:-}" ]]; then
		f=$(ares_abs "$dir" "$ARES_FLASH_OUT")
		mkdir -p "$(dirname "$f")" && rm -f "$f"
	fi
}

ares_abs() {	# ares_abs DIR PATH: PATH, relative ones to DIR
	case "$2" in /*) echo "$2" ;; *) echo "$1/$2" ;; esac
}

ARES_SH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=xvfb.sh
. "$ARES_SH_DIR/xvfb.sh"
ARES_BIN="${ARES_BIN:-$HOME/src/products/ares/build/desktop-ui/ares}"
ARES_SETTINGS=(--system "Nintendo 64" --setting Video/Driver=Vulkan --setting Audio/Driver=None
	--setting Input/Driver=SDL --setting General/HomebrewMode=true)
ARES_FWD_VARS=(ARES_HOST_SCREENSHOT ARES_HOST_SHOT_EVERY_MS ARES_HOST_SHOT_MAX ARES_HOST_PRINTF
	ARES_TRACE_CPU)

ares_host() {	# the host ares_run uses: local or HL64_ARES_HOST
	case "${HL64_ARES_HOST:-local}" in
	'' | local | localhost) echo local ;;
	*) echo "$HL64_ARES_HOST" ;;
	esac
}

ares_setup() {
	if [[ $(ares_host) == local ]]; then
		[[ -x "$ARES_BIN" ]] || { echo "patched ares not found at $ARES_BIN (set ARES_BIN)" >&2; return 2; }
		xvfb_start || return 2
		trap '[[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" 2>/dev/null || true' EXIT
	else
		"$ARES_SH_DIR/ares-remote.sh" --check
	fi
}

# ares_wait PID: wait for the child PID, passing SIGTERM / SIGINT on to it
# (ARES_STOP=1 once one came; one that came before also stops it); returns
# its status. The caller's TERM / INT traps are restored after.
ares_wait() {
	local pid=$1 c=0 old
	old="$(trap -p TERM INT)"
	# shellcheck disable=SC2064
	trap "ARES_STOP=1; kill -TERM $pid 2>/dev/null" TERM INT
	[[ ${ARES_STOP:-0} -eq 0 ]] || kill -TERM "$pid" 2>/dev/null || true
	while :; do
		c=0
		wait "$pid" || c=$?
		kill -0 "$pid" 2>/dev/null || break
	done
	trap - TERM INT
	eval "$old"
	return "$c"
}

# ares_slot_take PREFIX N: hold one of the N lock files PREFIX0..PREFIX<N-1>
# (flock on fd ARES_SLOT_FD), waiting until one is free (or ARES_STOP: then
# returns 1); ares_slot_drop releases it.
ares_slot_take() {
	local prefix=$1 n=$2 i said=0
	while [[ ${ARES_STOP:-0} -eq 0 ]]; do
		for ((i = 0; i < n; i++)); do
			exec {ARES_SLOT_FD}>>"$prefix$i"
			if flock -n "$ARES_SLOT_FD"; then
				ARES_SLOT="$prefix$i"
				return 0
			fi
			exec {ARES_SLOT_FD}>&-
		done
		[[ $said -eq 1 ]] || echo "ares: all $n slots ${prefix}* busy, waiting" >&2
		said=1
		sleep 2
	done
	return 1
}
ares_slot_drop() {
	exec {ARES_SLOT_FD}>&-
}

# ares_exec DIR ROM LOG TIMEOUT: ares itself, on DISPLAY_NUM (no slot, no
# copies); its pid to ARES_PID_FILE when set; with ARES_TAIL=1 LOG is also
# copied to stdout while ares runs. Sets ARES_WALL / ARES_LOAD; returns
# ares' status.
ares_exec() {
	local dir=$1 rom=$2 log=$3 to=$4 t0 c=0 pid tpid=
	t0=$(date +%s.%N)
	(cd "$dir" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
		exec timeout -k 3 "$to" "$ARES_BIN" "${ARES_SETTINGS[@]}" "$rom" >"$log" 2>&1) &
	pid=$!
	[[ -z "${ARES_PID_FILE:-}" ]] || echo "$pid" >"$ARES_PID_FILE"
	if [[ ${ARES_TAIL:-0} -eq 1 ]]; then
		local tl=$log
		[[ $tl == /* ]] || tl="$dir/$tl"
		tail -n +1 -F --pid="$pid" "$tl" 2>/dev/null &
		tpid=$!
	fi
	ares_wait "$pid" || c=$?
	[[ -z "$tpid" ]] || wait "$tpid" || true
	ARES_WALL=$(echo "$(date +%s.%N) $t0" | awk '{printf "%.0f", $1 - $2}')
	ARES_LOAD=$(cut -d' ' -f1 /proc/loadavg)
	return "$c"
}

ares_run() {
	local dir=$1 rom=$2 log=$3 to=${4:-${ARES_TIMEOUT:-1800}} c=0
	ARES_STOP=0
	ares_flash_check "$dir" || { ARES_WALL=0; ARES_LOAD=-; return 2; }
	ARES_RAN_ON=$(ares_host)
	if [[ $ARES_RAN_ON != local ]]; then
		local st
		st=$(mktemp "${TMPDIR:-/tmp}/ares-stat.XXXXXX")
		ARES_STAT_FILE="$st" "$ARES_SH_DIR/ares-remote.sh" "$dir" "$rom" "$log" "$to" &
		ares_wait $! || c=$?
		read -r ARES_WALL ARES_LOAD _ <"$st" || { ARES_WALL=-; ARES_LOAD=-; }
		rm -f "$st"
		return "$c"
	fi
	if [[ -z "${DISPLAY_NUM:-}" ]]; then
		echo "ares_run: no display (call ares_setup first)" >&2
		return 2
	fi
	local old
	old="$(trap -p TERM INT)"
	trap 'ARES_STOP=1' TERM INT
	ares_slot_take /tmp/hl64-ares.slot "${ARES_SLOTS:-2}" || c=143
	trap - TERM INT
	eval "$old"
	[[ $c -eq 0 ]] || { ARES_WALL=0; ARES_LOAD=-; return "$c"; }
	ares_exec "$dir" "$rom" "$log" "$to" || c=$?
	ares_slot_drop
	if [[ -n "${ARES_FLASH_OUT:-}" && ! -f "$(ares_abs "$dir" "$ARES_FLASH_OUT")" ]]; then
		echo "ares: the run left no flash image $ARES_FLASH_OUT (ROM without save type flashram, or no exit through the harness)" >&2
	fi
	return "$c"
}
