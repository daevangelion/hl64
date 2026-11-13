#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""bench.py -- write coll/bench.bin for the `colbench` debug command
(engine/coll/colbench.c): every T-segments segment of one map from the
GoldSrc probe fixtures, map-local, big-endian ('CBN1', u32 count, then per
segment u32 hull, f32 start[3], f32 end[3])."""
import argparse
import json
import os
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fixtures", default=os.environ.get("HL64_FIXTURES",
                                                          "/scratch/hl64/fixtures"))
    ap.add_argument("--map", default="hldemo1")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    path = os.path.join(a.fixtures, "goldsrc", "trace-and-hulls", "T-segments", a.map + ".jsonl")
    recs = []
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if "hull" in r:
                recs.append(struct.pack(">I3f3f", r["hull"], *r["s"], *r["e"]))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(struct.pack(">4sI", b"CBN1", len(recs)) + b"".join(recs))
    print("bench: %d segments -> %s" % (len(recs), a.out))


if __name__ == "__main__":
    main()
