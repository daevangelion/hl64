# SPDX-License-Identifier: MIT
# Sourced by the ares harness scripts: give this run its own Xvfb.
# Imported from the old tree's n64/port/test/xvfb_private.sh (audit entry
# CLEAN).
#
# ares exits with status 1 and prints nothing when its X server goes away
# (measured: kill the Xvfb under a running ares -> exit 1, empty stderr).
# Runs that shared a fixed display killed each other's emulator; a private
# server per run removes that coupling.
#
# xvfb_start: if DISPLAY_NUM is set and answers, use it as is (the caller
# owns it). Otherwise start Xvfb on a free display (-displayfd) and set
# DISPLAY_NUM and XVFB_PID; the caller kills XVFB_PID on exit.
xvfb_start() {
	XVFB_PID=
	if [[ -n "${DISPLAY_NUM:-}" ]] && xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1; then
		return 0
	fi
	local fifo n
	fifo="$(mktemp -u "${TMPDIR:-/tmp}/xvfb-fd.XXXXXX")"
	mkfifo "$fifo"
	Xvfb -displayfd 3 -screen 0 1280x800x24 3>"$fifo" >/dev/null 2>&1 &
	XVFB_PID=$!
	if ! read -r -t 15 n <"$fifo"; then
		rm -f "$fifo"
		kill "$XVFB_PID" 2>/dev/null || true
		XVFB_PID=
		echo "xvfb_start: Xvfb did not report a display" >&2
		return 1
	fi
	rm -f "$fifo"
	DISPLAY_NUM=":$n"
}
