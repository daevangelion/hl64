#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Brush entity boxes against GoldSrc (test/run/m5.sh suite brushbox).

test/m5/brushbox.c (`make brushboxtest` -> hl64-brushbox) spawns every
record of a map and prints each brush entity's origin, mins / maxs and
absmin / absmax before the first frame and after 3 s (0.01 s frames). The
fixture's `brush_ents` line (A-shape capture, docs/specs/area-links-and-
touch.md) holds GoldSrc's after its settle. Per brush entity (matched by
its submodel `*N`):

  box     mins / maxs equal (the model's bounds 1 u wider on every side:
          col_model_bounds), absmin / absmax - origin equal (the link's
          own 1 u);
  spawn   the origin before the first frame equal (a START_OPEN door at
          its open end: HLSDK's travel size - 2 - lip), func_pushable
          excepted (it spawns 1 u up and drops);
  settled the origin after 3 s equal for every one still live (the
          pushables on the floor).

Brush classes the engine does not spawn (cooked into the world or not
ported) are listed, not failed.

  python3 test/m5/brushbox.py BIN --root CONTENT --fixtures DIR [--maps a,b]

Exit 0 when every check passes, 1 otherwise.
"""
import argparse
import collections
import json
import os
import subprocess
import sys

MAPS = "hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d"
CAPTURE = "area-links-and-touch/A-shape"
TOL = 0.01


def ours(binary, root, m):
    p = subprocess.run([binary, "--root", root, "--map", m], capture_output=True, text=True)
    if p.returncode:
        sys.exit(f"{binary} {m}: exit {p.returncode}\n{p.stderr[-2000:]}")
    rows = collections.defaultdict(dict)
    for line in p.stdout.splitlines():
        if not line.startswith("bb "):
            continue
        t = line.split()
        d = {"class": t[3]}
        for kv in t[4:]:
            k, v = kv.split("=")
            d[k] = [float(x) for x in v.split(",")]
        rows[int(t[1])][int(t[2])] = d
    return rows


def differs(a, b):
    return any(abs(x - y) > TOL for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--root", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--maps", default=MAPS)
    a = ap.parse_args()
    fails = []
    per = collections.Counter()
    unspawned = collections.Counter()
    settled = 0
    for m in a.maps.split(","):
        path = os.path.join(a.fixtures, "goldsrc", CAPTURE, m + ".jsonl")
        with open(path) as f:
            f.readline()
            gs = {int(e["model"][1:]): e for e in json.loads(f.readline())["brush_ents"]}
        rows = ours(a.bin, a.root, m)
        if len(rows) != 2:
            fails.append(f"{m}: no driver output")
            continue
        first, last = (rows[k] for k in sorted(rows))
        for sub, g in sorted(gs.items()):
            c = g["class"]
            d = first.get(sub)
            if d is None:
                unspawned[c] += 1
                continue
            if d["class"] != c:
                fails.append(f"{m} *{sub}: class {d['class']} vs GoldSrc {c}")
                continue
            per[c] += 1
            for k in ("mins", "maxs"):
                if d[k] != [float(x) for x in g[k]]:
                    fails.append(f"{m} *{sub} {c}: {k} {d[k]} vs GoldSrc {g[k]}")
            for k in ("absmin", "absmax"):
                if differs([x - o for x, o in zip(d[k], d["o"])], [x - o for x, o in zip(g[k], g["o"])]):
                    fails.append(f"{m} *{sub} {c}: {k} - origin {d[k]} - {d['o']} vs GoldSrc "
                                 f"{g[k]} - {g['o']}")
            if c != "func_pushable" and differs(d["o"], g["o"]):
                fails.append(f"{m} *{sub} {c}: spawn origin {d['o']} vs GoldSrc {g['o']}")
            e = last.get(sub)
            if e is not None:
                settled += 1
                if differs(e["o"], g["o"]):
                    fails.append(f"{m} *{sub} {c}: origin after 3 s {e['o']} vs GoldSrc {g['o']}")
            elif c == "func_pushable":
                fails.append(f"{m} *{sub} {c}: not live after 3 s")
    for c in sorted(per):
        print(f"brushbox class {c}: {per[c]}")
    if unspawned:
        print("brushbox not spawned (world / not ported): "
              + ", ".join(f"{c} {n}" for c, n in sorted(unspawned.items())))
    for f in fails[:40]:
        print("brushbox FAIL " + f)
    if fails:
        print(f"brushbox: {len(fails)} differences")
        return 1
    print(f"brushbox: {len(a.maps.split(','))} maps, {sum(per.values())} brush entities: boxes, "
          f"abs boxes and spawn origins equal GoldSrc's, {settled} origins after 3 s equal")
    return 0


if __name__ == "__main__":
    sys.exit(main())
