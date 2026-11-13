#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""fx.py -- run the effects host tests (test/m3/fx.c: render-fx.md X1-X6,
dlights.md D1/D2/D4 + model term, lightstyles.md L1/L2 + model term) and
check that every test reported PASS.

  python3 test/m3/fx.py BUILD_DIR CONTENT

BUILD_DIR holds host/hl64-fx (`make BUILD_DIR=... fxtest`); CONTENT is a
cooked content root (`make cook`). Exit 0 when every test passed.
"""
import subprocess
import sys

TESTS = ("X1", "X2", "X3", "X4", "X5", "X6", "SCREEN", "DLIGHT", "LSTYLE")


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    exe = argv[1].rstrip("/") + "/host/hl64-fx"
    r = subprocess.run([exe, "--root", argv[2]], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    seen = {}
    for line in r.stdout.splitlines():
        w = line.split()
        if len(w) >= 2 and w[0] in TESTS and w[1] in ("PASS", "FAIL"):
            seen[w[0]] = w[1]
    missing = [t for t in TESTS if t not in seen]
    failed = [t for t in TESTS if seen.get(t) == "FAIL"]
    ok = r.returncode == 0 and not missing and not failed
    print("fx.py: %s (%d tests%s%s)" % ("PASS" if ok else "FAIL", len(TESTS),
          ", missing " + " ".join(missing) if missing else "",
          ", failed " + " ".join(failed) if failed else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
