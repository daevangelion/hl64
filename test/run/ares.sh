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
# Where: HL64_ARES_HOST unset / empty / local: on this machine, holding one
# of ARES_SLOTS (default 2) slot locks /tmp/hl64-ares.slot<i> while ares
# runs (every ares user on the machine takes one). HL64_ARES_HOST=<host>
# (ares-host): test/run/ares-remote.sh, the same run on that host (its own
# slots, results copied back to the same paths).
# Env: ARES_BIN (local, default ~/src/products/ares/build/desktop-ui/ares),
# ARES_SLOTS, ARES_TIMEOUT, DISPLAY_NUM (xvfb.sh), and the harness variables
# ares reads (ARES_TEST_INPUT, ARES_HOST_SCREENSHOT, ARES_HOST_SHOT_EVERY_MS,
# ARES_HOST_SHOT_MAX, ARES_HOST_SHOT_DIR, ARES_HOST_PRINTF, ARES_TRACE_CPU),
# passed through (remote: forwarded, the input file copied once at start).

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

# ares_slot_take PREFIX N: hold one of the N lock files PREFIX0..PREFIX<N-1>
# (flock on fd ARES_SLOT_FD), waiting until one is free; ares_slot_drop
# releases it.
ares_slot_take() {
	local prefix=$1 n=$2 i said=0
	while :; do
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
}
ares_slot_drop() {
	exec {ARES_SLOT_FD}>&-
}

# ares_exec DIR ROM LOG TIMEOUT: ares itself, on DISPLAY_NUM (no slot, no
# copies); sets ARES_WALL / ARES_LOAD; returns ares' status.
ares_exec() {
	local dir=$1 rom=$2 log=$3 to=$4 t0 c=0
	t0=$(date +%s.%N)
	(cd "$dir" && DISPLAY="$DISPLAY_NUM" ARES_TEST_HARNESS=1 \
		exec timeout -k 10 "$to" "$ARES_BIN" "${ARES_SETTINGS[@]}" "$rom" >"$log" 2>&1) || c=$?
	ARES_WALL=$(echo "$(date +%s.%N) $t0" | awk '{printf "%.0f", $1 - $2}')
	ARES_LOAD=$(cut -d' ' -f1 /proc/loadavg)
	return "$c"
}

ares_run() {
	local dir=$1 rom=$2 log=$3 to=${4:-${ARES_TIMEOUT:-1800}} c=0
	ARES_RAN_ON=$(ares_host)
	if [[ $ARES_RAN_ON != local ]]; then
		local st
		st=$(mktemp "${TMPDIR:-/tmp}/ares-stat.XXXXXX")
		ARES_STAT_FILE="$st" "$ARES_SH_DIR/ares-remote.sh" "$dir" "$rom" "$log" "$to" || c=$?
		read -r ARES_WALL ARES_LOAD _ <"$st" || { ARES_WALL=-; ARES_LOAD=-; }
		rm -f "$st"
		return "$c"
	fi
	if [[ -z "${DISPLAY_NUM:-}" ]]; then
		echo "ares_run: no display (call ares_setup first)" >&2
		return 2
	fi
	ares_slot_take /tmp/hl64-ares.slot "${ARES_SLOTS:-2}"
	ares_exec "$dir" "$rom" "$log" "$to" || c=$?
	ares_slot_drop
	return "$c"
}
