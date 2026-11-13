#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Monster sweep (slice GameMon) on the host driver test/m3/mon.c
(`make montest` -> hl64-mon).

Per map: a New Game on it, every physical record woken, every monstermaker,
monster_grunt_repel and monster_miniturret fired, then FRAMES frames at 1/30 s. Checks:
  - the driver exits 0 (an engine sys_error is a failure);
  - every monster_* class of the map's entity lump spawned at least as
    many entities as the lump has records of it;
  - every monster class that spawned thought (nextthink moved) during the
    run, except monster_grunt_repel (no think: it waits for its use);
  - no monster spawn refusal, no "has no sequence", "bad pose", "can't
    asses" or "no class for its monstertype" log line.
Prints one line per map and class (spawned / live / dead / thinks) and
the totals per class.

Usage: test/m3/mon_sweep.py --bin BUILD/host/hl64-mon --content DIR
       [--maps a,b] [--frames 600] [--logs DIR]
Exit 0 when every map passes.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

MAPS = "hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d"
NO_THINK = {"monster_grunt_repel"}
BAD = re.compile(r"(spawn: (monster_\S+|garg_stomp|streak_spiral|env_smoker) .*refused"
                 r"|has no sequence|bad pose|can't asses|no class for its monstertype)")
CLASS = re.compile(r"^mon class (\S+) spawned=(\d+) live=(\d+) dead=(\d+) thinks=(\d+)$")


def lump_monsters(content, m):
    """monster_* classnames of the map's entity lump (cooked .ent text)."""
    n = collections.Counter()
    with open(os.path.join(content, "maps", m + ".ent"), encoding="latin-1") as f:
        for c in re.findall(r'"classname"\s+"(monster_[a-z_]+)"', f.read()):
            n[c] += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--maps", default=MAPS)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--logs", default=None)
    a = ap.parse_args()
    fails = 0
    totals = collections.defaultdict(lambda: [0, 0, 0, 0])
    for m in a.maps.split(","):
        p = subprocess.run([a.bin, "--root", a.content, m, str(a.frames)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
        log = p.stdout.decode("latin-1")
        if a.logs:
            os.makedirs(a.logs, exist_ok=True)
            with open(os.path.join(a.logs, m + ".log"), "w") as f:
                f.write(log)
        got = {}
        for ln in log.splitlines():
            r = CLASS.match(ln)
            if r:
                got[r.group(1)] = [int(x) for x in r.group(2, 3, 4, 5)]
        for ln in log.splitlines():
            if ln.startswith("prof mon_us"):
                print(f"mon-sweep {m} {ln}")
        errs = []
        if p.returncode != 0:
            errs.append(f"exit {p.returncode}: " + " | ".join(log.splitlines()[-3:]))
        if "mon done" not in log:
            errs.append("no 'mon done' line")
        for ln in log.splitlines():
            if BAD.search(ln):
                errs.append(ln.strip())
        for c, n in sorted(lump_monsters(a.content, m).items()):
            if got.get(c, [0])[0] < n:
                errs.append(f"{c}: {got.get(c, [0])[0]} spawned of {n} records")
        for c, (sp, lv, dd, th) in sorted(got.items()):
            if c.startswith("monster_") and c not in NO_THINK and th == 0:
                errs.append(f"{c}: no think in {a.frames} frames")
            t = totals[c]
            t[0] += sp; t[1] += lv; t[2] += dd; t[3] += th
            print(f"mon-sweep {m} {c} spawned={sp} live={lv} dead={dd} thinks={th}")
        print(f"mon-sweep {m} {'PASS' if not errs else 'FAIL'} classes={len(got)} "
              f"monsters={sum(v[0] for v in got.values())}")
        for e in errs[:20]:
            print(f"mon-sweep {m} FAIL: {e}")
        fails += bool(errs)
    for c, (sp, lv, dd, th) in sorted(totals.items()):
        print(f"mon-sweep total {c} spawned={sp} live={lv} dead={dd} thinks={th}")
    print("mon-sweep " + ("PASS" if not fails else f"FAIL ({fails} maps)"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
