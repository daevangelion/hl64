#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""fx.py -- run the effects host tests (test/m3/fx.c: render-fx.md X1-X6,
dlights.md D1/D2/D4 + model term, lightstyles.md L1/L2 + model term, ADD
(render-fx.md 2a: the additive ramp and the RDP blender model), and
PLACED: the infodecal / env_glow records of hldemo1 and hldemo2 placed
from their DECL sheets) and check that every test reported PASS.

  python3 test/m3/fx.py BUILD_DIR CONTENT

BUILD_DIR holds host/hl64-fx (`make BUILD_DIR=... fxtest`); CONTENT is a
cooked content root (`make cook`). Exit 0 when every test passed.
"""
import subprocess
import sys

TESTS = ("X1", "X2", "X3", "X4", "X5", "X6", "SCREEN", "DLIGHT", "LSTYLE", "ADD")
PLACED_MAPS = ("hldemo1", "hldemo2")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    return r


def verdicts(out, names):
    seen = {}
    for line in out.splitlines():
        w = line.split()
        if len(w) >= 2 and w[0] in names and w[1] in ("PASS", "FAIL"):
            seen[w[0]] = w[1]
    return seen


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    exe = argv[1].rstrip("/") + "/host/hl64-fx"
    r = run([exe, "--root", argv[2]])
    seen = verdicts(r.stdout, TESTS)
    missing = [t for t in TESTS if t not in seen]
    failed = [t for t in TESTS if seen.get(t) == "FAIL"]
    ok = r.returncode == 0
    for m in PLACED_MAPS:
        r = run([exe, "--root", argv[2], "--placed", m])
        v = verdicts(r.stdout, ("PLACED",)).get("PLACED")
        if v is None:
            missing.append("PLACED:" + m)
        elif v == "FAIL" or r.returncode != 0:
            failed.append("PLACED:" + m)
    ok = ok and not missing and not failed
    n = len(TESTS) + len(PLACED_MAPS)
    print("fx.py: %s (%d tests%s%s)" % ("PASS" if ok else "FAIL", n,
          ", missing " + " ".join(missing) if missing else "",
          ", failed " + " ".join(failed) if failed else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
