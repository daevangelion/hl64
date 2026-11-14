#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PC samples of a PSAMP=1 cart log (plat/n64/psamp.c) per function.

  psamp.py <ares log> <hl64.elf> [--top N] [--seg A-B] [--by-seg]

Each `psamp <seg> <addr> <count>` bucket (64 bytes of text) goes to the
function holding its first byte (`nm -n` of the ELF); segment k is the time
between the k-th and the (k+1)-th SHOT (segment 0 starts at boot). Prints,
for the selected segments (default all), the functions by samples with
their share; --by-seg prints one table per segment, labelled with the
route's SHOT names.
"""
import argparse
import bisect
import os
import re
import subprocess
import sys

NM = os.path.expanduser("~/sdks/mips64-toolchain/bin/mips64-elf-nm")


def symbols(elf):
    out = subprocess.run([NM, "-n", elf], check=True, capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def parse(log):
    segs, tot, shots = {}, {}, []
    for line in open(log, errors="replace"):
        m = re.match(r"psamp (\d+) samples=(\d+)", line)
        if m:
            tot[int(m.group(1))] = int(m.group(2))
            continue
        m = re.match(r"psamp (\d+) ([0-9a-f]{8}) (\d+)$", line.strip())
        if m:
            segs.setdefault(int(m.group(1)), []).append((int(m.group(2), 16), int(m.group(3))))
            continue
        m = re.match(r"route: \d+ SHOT (\S+)", line)
        if m:
            shots.append(m.group(1))
    return segs, tot, shots


def table(buckets, addrs, names, total, top):
    by = {}
    for a, c in buckets:
        i = bisect.bisect_right(addrs, a) - 1
        n = names[i] if i >= 0 else "?"
        by[n] = by.get(n, 0) + c
    s = sum(by.values()) or 1
    lines = ["  samples %d (in text %d)" % (total, s)]
    for n, c in sorted(by.items(), key=lambda kv: -kv[1])[:top]:
        lines.append("  %6d %5.1f%%  %s" % (c, 100.0 * c / s, n))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("elf")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--seg", default=None, help="A-B (inclusive) or A")
    ap.add_argument("--by-seg", action="store_true")
    a = ap.parse_args()
    addrs, names = symbols(a.elf)
    segs, tot, shots = parse(a.log)
    keys = sorted(segs)
    if a.seg:
        lo, _, hi = a.seg.partition("-")
        lo, hi = int(lo), int(hi or lo)
        keys = [k for k in keys if lo <= k <= hi]
    if a.by_seg:
        for k in keys:
            label = shots[k] if k < len(shots) else "end"
            print("seg %d (up to SHOT %s)" % (k, label))
            print(table(segs[k], addrs, names, tot.get(k, 0), a.top))
        return 0
    allb = [b for k in keys for b in segs[k]]
    print("segments %s" % (",".join(str(k) for k in keys)))
    print(table(allb, addrs, names, sum(tot.get(k, 0) for k in keys), a.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
