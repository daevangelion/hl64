#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Snd slice tests (host; docs/specs/sound.md 5): S1-S3, S5, S6, S8 and the
# host parts of S4 / S7, on a synthetic content set made here (raw 16-bit
# wav64 files with the libdragon header layout, a sentences.txt, a test
# res.def naming them), cooked by tools/restab into its own campaign.rtb,
# then test/m3/snd.c over the real engine with the null audio backend and
# the event log on. No Valve data, no captures (the retail S-mix parts of
# S3 / S4 / S7 are out of scope until a capture plan exists).
#
# Usage: test/m3/snd.sh [S1 ... S8 | all]
# Env:   BUILD_DIR (scratch)
# Exit:  0 pass, 1 failure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT/$BUILD_DIR" ;; esac
OUT="$BUILD_DIR/m3-snd"
CONTENT="$OUT/content"
DEFS="$OUT/defs"
fail() { echo "SND FAIL: $*" >&2; exit 1; }

cd "$ROOT"
mkdir -p "$BUILD_DIR"
make --no-print-directory BUILD_DIR="$BUILD_DIR" sndtest >"$OUT.build.log" 2>&1 \
	|| { tail -30 "$OUT.build.log" >&2; fail "build"; }
grep -E 'warning:' "$OUT.build.log" && fail "build has warnings"

rm -rf "$CONTENT" "$DEFS"     # synthetic set is rebuilt from scratch each run
python3 - "$CONTENT" "$DEFS" <<'EOF'
import os, struct, sys
from tools.restab.testdata import make_cwg
content, defs = sys.argv[1], sys.argv[2]
for d in ("maps", os.path.join("sound", "t")):
    os.makedirs(os.path.join(content, d), exist_ok=True)
os.makedirs(defs, exist_ok=True)
with open(os.path.join(content, "maps", "campaign.cwg"), "wb") as f:
    f.write(make_cwg([]))     # no maps: the sound tests need resources only

def wav64(name, freq, seconds, loop=0, nsamples=None):
    n = nsamples if nsamples is not None else int(freq * seconds)
    # libdragon wav64 header: id, version 10, format 0 (raw), channels 1,
    # 16 bits, rate, length, loop length, loop end (0: the length), data
    # offset, state size
    hdr = b"WV64" + struct.pack(">bbbbiiiiII", 10, 0, 1, 16, freq, n, loop, 0, 32, 0)
    with open(os.path.join(content, "sound", "t", name + ".wav64"), "wb") as f:
        f.write(hdr + bytes(2 * n))

wav64("a", 11025, 2.0)
wav64("b", 22050, 2.0)
wav64("loop", 11025, 0, loop=11025, nsamples=22050)
wav64("short", 11025, 0.1)
for i in range(16):
    wav64("d%d" % i, 11025, 2.0)
wav64("w1", 11025, 0.3)
wav64("w2", 11025, 0.3)
wav64("w3", 11025, 0.4)
with open(os.path.join(content, "sound", "sentences.txt"), "w") as f:
    f.write("TST0 t/w1 w2(p120 v80) w3(e50)\n")
    f.write("TST1 t/w1 w2 w1 w2 w1 w2\n")
lines = ['RES(SOUND, A, "t/a.wav")', 'RES(SOUND, B, "t/b.wav")',
         'RES(SOUND, LOOP, "t/loop.wav")', 'RES(SOUND, SHORT, "t/short.wav")']
lines += ['RES(SOUND, D%d, "t/d%d.wav")' % (i, i) for i in range(16)]
lines += ['RES(SENTENCE, TST0, "TST0")', 'RES(SENTENCE, TST1, "TST1")']
with open(os.path.join(defs, "res.def"), "w") as f:
    f.write("\n".join(lines) + "\n")
EOF
python3 -m tools.restab.restab --content "$CONTENT" --defs "$DEFS" -o "$CONTENT/campaign.rtb" \
	>"$OUT/restab.log" 2>&1 || { cat "$OUT/restab.log" >&2; fail "restab"; }

rc=0
for t in "${@:-all}"; do
	if "$BUILD_DIR/host/hl64-snd" --root "$CONTENT" "$t" >"$OUT/$t.log" 2>&1; then
		grep -E '^snd test ' "$OUT/$t.log"
	else
		grep -E '^CHECK FAIL|^snd test |error' "$OUT/$t.log" >&2 || tail -20 "$OUT/$t.log" >&2
		rc=1
	fi
done
[[ $rc -eq 0 ]] || fail "see $OUT/*.log"
echo "snd: PASS (logs in $OUT)"
