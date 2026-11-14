#!/bin/sh
# SPDX-License-Identifier: MIT
# Stream slice host tests (engine/world/stream.c, world.c, engine/io,
# engine/render OPEN-seam draw; docs/architecture.md 5.2-5.4,
# docs/formats/cwg-format.md "OPEN seams") on a cooked campaign:
#   1. hl64-stream (test/m4/stream.c): async open, seam pins, change-driven
#      recompute, eviction classes (E1-E5), teleport windows, the blocked
#      I/O positive control and the teleport-window count (blk1ms_tele);
#   2. OPEN seam t0a0c -> t0a0d (test/m4/stream_open.route): with
#      r_openseam 2 B's draw set draws (shot ren oseam > 0, `ren: open seam`
#      lines with the meshlet count) and nothing pops in; control with
#      r_openseam 0: oseam=0 and no lines;
#   3. home layer (test/m4/stream_home.route): a hldemo1 pose with hldemo2
#      open and hydrated: its studio entities are counted, never drawn
#      (other_ents > 0, oseam_ents = 0);
#   4. no synchronous studio read and no frame > 1 ms blocked after New
#      Game: routes m3-a (hldemo1) and m4 (hldemo1 -> hldemo2 -> hldemo3,
#      hydrating hldemo2 and hldemo3): studio_sync=0 blk1ms=0 on the last
#      stats stream line; m4 also popin=0 and route PASS.
#
#   test/m4/stream.sh [CONTENT]
#     CONTENT    cooked content (default $HL64_CONTENT or the c7 cook)
#     BUILD_DIR  (env) the build directory (default /scratch/hl64/builds/Stream)
set -eu
cd "$(dirname "$0")/../.."
CONTENT=${1:-${HL64_CONTENT:-/scratch/hl64/tmp/M5Lead2/c16}}
B=${BUILD_DIR:-/scratch/hl64/builds/Stream}
T=$(mktemp -d "${TMPDIR:-/tmp}/streamtest.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL $*"; fail=1; }

make -s BUILD_DIR="$B" host streamtest
HOST="$B/host/hl64-host"

# run NAME ROUTE [host args]: one route on the host, log $T/NAME.log
run() {
	name=$1
	route=$2
	shift 2
	python3 -m tools.routec.routec --rtb "$CONTENT/campaign.rtb" "$route" -o "$T/$name.rbc" \
		>"$T/$name.routec" 2>&1 || { cat "$T/$name.routec"; bad "routec $route"; return 0; }
	"$HOST" --root "$CONTENT" --route "$T/$name.rbc" --cmd "@1 fixed_dt 33333" "$@" \
		>"$T/$name.log" 2>&1 || true
	grep -q '^route PASS$' "$T/$name.log" || { grep '^route FAIL' "$T/$name.log"; bad "$name: route"; }
}
# kv LOG PREFIX KEY: KEY's value on the last line starting with PREFIX
kv() {
	grep "$2" "$1" | tail -1 | sed -n "s/.* $3=\([0-9]*\).*/\1/p"
}

# 1. driver
"$B/host/hl64-stream" "$CONTENT" >"$T/stream.log" 2>&1 || true
grep -q '^stream: 0 failed$' "$T/stream.log" || { grep 'FAIL' "$T/stream.log"; bad "hl64-stream"; }
echo "stream: $(grep -c ': PASS ' "$T/stream.log") driver checks PASS"

# 2. OPEN seam, and its control
run open test/m4/stream_open.route --cmd "@2 set r_openseam 2"
o=$(kv "$T/open.log" '^shot [0-9]* ren:' oseam)
n=$(grep -c '^ren: open seam t0a0c->t0a0d meshlets=[1-9]' "$T/open.log" || true)
p=$(kv "$T/open.log" '^stats [0-9]* stream:' popin)
[ "${o:-0}" -gt 0 ] && [ "$n" -gt 0 ] && [ "$p" = 0 ] \
	|| bad "open seam: oseam=$o, $n meshlet lines, popin=$p"
echo "stream: open seam t0a0c->t0a0d: shot oseam=$o meshlets, $n frames logged, popin=$p"
run open0 test/m4/stream_open.route --cmd "@2 set r_openseam 0"
o0=$(kv "$T/open0.log" '^shot [0-9]* ren:' oseam)
n0=$(grep -c '^ren: open seam' "$T/open0.log" || true)
[ "$o0" = 0 ] && [ "$n0" = 0 ] || bad "r_openseam 0 still draws (oseam=$o0, $n0 lines)"

# 3. home layer only
run home test/m4/stream_home.route
oe=$(kv "$T/home.log" '^shot [0-9]* ren:' other_ents)
de=$(kv "$T/home.log" '^shot [0-9]* ren:' oseam_ents)
grep -q '^rec: hldemo2 open in slot' "$T/home.log" || bad "home: hldemo2 did not open"
[ "${oe:-0}" -gt 0 ] && [ "$de" = 0 ] || bad "home: other_ents=$oe oseam_ents=$de"
echo "stream: hldemo1 pose, hldemo2 open: $oe hldemo2 studio entities not drawn, $de drawn"

# 4. no synchronous studio read, no blocked frame
run m3a test/routes/m3-a.route
run m4 test/routes/m4.route
for r in m3a m4; do
	ss=$(kv "$T/$r.log" '^stats [0-9]* stream:' studio_sync)
	bk=$(kv "$T/$r.log" '^stats [0-9]* stream:' blk1ms)
	bt=$(kv "$T/$r.log" '^stats [0-9]* stream:' blk1ms_tele)
	[ "$ss" = 0 ] && [ "$bk" = 0 ] || { grep '^studio: sync\|^io: frame' "$T/$r.log" | head; \
		bad "$r: studio_sync=$ss blk1ms=$bk"; }
	echo "stream: route $r: studio_sync=$ss blk1ms=$bk blk1ms_tele=$bt"
done
pp=$(kv "$T/m4.log" '^stats [0-9]* stream:' popin)
[ "$pp" = 0 ] || { grep '^stream: popin' "$T/m4.log" | head; bad "m4: popin=$pp"; }
grep -c '^seam: switch ' "$T/m4.log" | grep -q '^2$' || bad "m4: not 2 switches"

if [ $fail -eq 0 ]; then
	echo "stream.sh PASS"
else
	echo "stream.sh FAIL"
fi
exit $fail
