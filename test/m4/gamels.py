#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Level-state test (slice GameLS, M4) on the host driver test/m4/gamels.c
(`make gamelstest` -> hl64-gamels): per map the close / mutate / runtime /
refuse / carry passes, then the hldemo1 <-> hldemo2 elevator twins and the
cross-slot reference cut. Prints the driver's per-class and per-map delta
lines and one PASS / FAIL line per run.

Usage: test/m4/gamels.py --bin BUILD/host/hl64-gamels --content DIR [--maps a,b]
Exit 0 when every run passes.
"""
import argparse
import subprocess
import sys

MAPS = "hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d"
# c6 seams.json hldemo1 -> hldemo2 twins (startele1 doors, elevator buttons)
TWINS = ("hldemo1", "hldemo2", "78:93,79:89,80:90,81:92")


def run(args, label):
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith("gamels ")]
    for l in lines:
        if l.startswith(("gamels FAIL", "gamels   at", "gamels map", "gamels closing", "gamels class",
                         "gamels twin", "gamels cut")):
            print(l)
    ok = p.returncode == 0 and not any(l.startswith("gamels FAIL") for l in lines) and \
        any(l.startswith("gamels done") for l in lines)
    print("%s: %s" % (label, "PASS" if ok else "FAIL (exit %d)" % p.returncode))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--maps", default=MAPS)
    a = ap.parse_args()
    ok = True
    for m in a.maps.split(","):
        ok &= run([a.bin, "--root", a.content, "--map", m], "gamels " + m)
    ok &= run([a.bin, "--root", a.content, "--twin"] + list(TWINS), "gamels twin")
    print("gamels: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
