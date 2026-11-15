#!/bin/sh
# SPDX-License-Identifier: MIT
# Seam slice host tests (engine/world/seam.c, docs/architecture.md 5.4,
# docs/formats/cwg-format.md) on a cooked campaign:
#   1. CWG3 parse: the runtime's decoded graph (hl64-seam dump) against the
#      cook's campaign.json and seams.json (test/m4/seam_parse.py), plus a
#      negative control (a changed dump must fail the comparison);
#   2. selftest: approach / switch box / forced rules on synthetic eye
#      paths, late switch, settle window, leave rule, open_fail, USEONLY
#      hldemo1 -> hldemo2 crossing with twins, carried entities, the player
#      re-base (arrival hldemo2-local -452 -100 196) and continuity, approach
#      ghosts (a B entity in B's carry box before the switch: drawn in A's
#      view, extra=0 after; one new after the switch: extra=1);
#   3. validation: a cook whose seam T or delta was changed fails at boot
#      (sys_error naming the rule); a changed B directory CRC rejects the
#      seam at B open (open_fail, no switch);
#   4. cook proof: tools/cook/stage.py check_campaign_region passes with
#      include/hl64/limits.h and fails with HL64_CAMPAIGN_KB one KB under
#      the file.
#
#   test/m4/seam.sh [CONTENT [STAGING]]
#     CONTENT  cooked content (default $HL64_CONTENT or the c6 cook)
#     STAGING  its staging dir (default CONTENT.stage)
#     BUILD_DIR (env) the build directory (default /scratch/hl64/builds/Seam)
set -eu
cd "$(dirname "$0")/../.."
CONTENT=${1:-${HL64_CONTENT:-/scratch/hl64/tmp/Int8/c22}}
STAGING=${2:-$CONTENT.stage}
B=${BUILD_DIR:-/scratch/hl64/builds/Seam}
T=$(mktemp -d "${TMPDIR:-/tmp}/seamtest.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL $*"; fail=1; }

make -s BUILD_DIR="$B" seamtest
SEAM="$B/host/hl64-seam"

# 1. parse
"$SEAM" --root "$CONTENT" dump >"$T/dump.txt" || bad "dump"
python3 test/m4/seam_parse.py "$T/dump.txt" "$STAGING" || bad "parse"
sed '0,/twin_lumps=\([0-9]*\):/s//twin_lumps=\1999:/' "$T/dump.txt" >"$T/dump-bad.txt"
cmp -s "$T/dump.txt" "$T/dump-bad.txt" && bad "negative control did not change the dump"
if python3 test/m4/seam_parse.py "$T/dump-bad.txt" "$STAGING" >/dev/null; then
	bad "seam_parse accepted a changed twin"
fi

# 2. selftest
"$SEAM" --root "$CONTENT" selftest >"$T/selftest.txt" 2>&1 || true
grep -q "^seamtest ok selftest" "$T/selftest.txt" || { grep "FAIL\|error" "$T/selftest.txt"; bad "selftest"; }

# 3. validation: a content copy (hard links) with a changed campaign.cwg
variant() {	# name field: t / delta / bcrc
	d="$T/$1"
	cp -al "$CONTENT" "$d"
	rm "$d/maps/campaign.cwg"
	python3 - "$CONTENT/maps/campaign.cwg" "$d/maps/campaign.cwg" "$1" <<'EOF'
import struct, sys
b = bytearray(open(sys.argv[1], "rb").read())
nm, nl, nk = struct.unpack_from(">3H", b, 8)
seam0 = 32 + 48 * nm + 16 * nl + 8 * nk          # seam 0: hldemo1 -> hldemo2
ofs = {"t": 124, "delta": 24, "bcrc": 120}[sys.argv[3]]
if sys.argv[3] == "bcrc":
    struct.pack_into(">I", b, seam0 + ofs, struct.unpack_from(">I", b, seam0 + ofs)[0] ^ 1)
else:
    struct.pack_into(">f", b, seam0 + ofs, struct.unpack_from(">f", b, seam0 + ofs)[0] + 1.0)
open(sys.argv[2], "wb").write(b)
EOF
}
"$SEAM" --root "$CONTENT" init >"$T/init.txt" 2>&1 || bad "init on the cook failed"
for v in t delta; do
	variant $v
	if "$SEAM" --root "$T/$v" init >"$T/$v.txt" 2>&1; then
		bad "changed $v accepted"
	fi
	case $v in
	t) want="T is not g_B + delta - g_A" ;;
	delta) want="delta is not landmark B - landmark A" ;;
	esac
	grep -q "$want" "$T/$v.txt" || bad "changed $v: no '$want' error"
done
variant bcrc
"$SEAM" --root "$T/bcrc" crc >"$T/crc.txt" 2>&1 || { grep "FAIL" "$T/crc.txt"; bad "crc"; }
grep -q "seam: open_fail hldemo1->hldemo2 (directory CRC" "$T/crc.txt" || bad "no CRC open_fail line"

# 4. cook proof of the campaign region
python3 - "$CONTENT/maps/campaign.cwg" "$T" <<'EOF' || bad "campaign region cook proof"
import re, sys
from pathlib import Path
from tools.cook import stage
from tools.restab import restab
graph, tmp = Path(sys.argv[1]), Path(sys.argv[2])
need = stage.check_campaign_region(graph)
text = Path(restab.DEFAULT_LIMITS).read_text()
tight, n = re.subn(r"^(#define\s+HL64_CAMPAIGN_KB\s+)\d+", lambda m: m.group(1) + str((need - 1) // 1024),
                   text, flags=re.M)
assert n == 1
lim = tmp / "limits.h"
lim.write_text(tight)
try:
    stage.check_campaign_region(graph, str(lim))
except stage.CookError as e:
    print(f"campaign region: need {need} B; tight carve fails: {e}")
else:
    sys.exit("a carve under the need passed")
EOF

if [ $fail -eq 0 ]; then
	echo "seam.sh PASS"
else
	echo "seam.sh FAIL"
fi
exit $fail
