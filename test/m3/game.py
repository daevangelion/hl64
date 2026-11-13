#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Game spawn-all test (slice GameCore) on the host driver test/m3/game.c
(`make gametest` -> hl64-game).

Per map, a New Game on it, then every physical record of the map woken
in lump order. Checks, per record:
  - every entity record's class spawns: a refusal is a failure of the
    class's owner (classes.def owner comments) unless the game logged it
    with a reason (`spawn: CLASS map S lump L refused: REASON`) and the
    reason is an HLSDK spawn-time removal, not "class not implemented" or
    a Spawn that gave no reason (game/hl/cbase.h SpawnRefuse);
  - P15: no live entity is SOLID_BSP without MOVETYPE_PUSH (the engine
    also stops on it: a driver exit != 0 is a failure);
  - no refusal without a logged reason.
Prints one summary line per map and per failing class (with its owner).

Usage: test/m3/game.py --bin BUILD/host/hl64-game --content DIR [--maps a,b]
Exit 0 when every map passes.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

MAPS = "hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d"
SOLID_BSP = 4
MOVETYPE_PUSH = 7
# refusal reasons that mean a missing implementation (game/hl/dispatch.c)
UNIMPLEMENTED = ("class not implemented", "logic class has no entity spawn",
                 "no logic class", "game private slab full")
# a Spawn refused or removed itself without its HLSDK reason (dispatch.c WHY_*)
REASONLESS = ("Spawn refused with no reason", "Spawn removed itself with no reason")
CLASSES_DEF = os.path.join(os.path.dirname(__file__), "..", "..", "game", "defs", "classes.def")


def owners():
    """classname -> slice from the `// Owners` block of classes.def."""
    text, slice_, out, pats = open(CLASSES_DEF).read(), None, {}, []
    for line in text.splitlines():
        m = re.match(r"//\s*(GameCore|GameMon|Items):(.*)", line)
        if m:
            slice_ = m.group(1)
            body = m.group(2)
        elif slice_ and line.startswith("//   "):
            body = line[2:]
        else:
            if not line.startswith("//"):
                break
            slice_ = None
            continue
        for item in re.split(r"[,.]", body):
            item = item.strip()
            if not item:
                continue
            pats.append((slice_, item))
    names = re.findall(r'CLASS\(\w+,\s*"([^"]+)"\)', text)
    for n in names:
        for s, item in pats:
            words = item.split()
            if item.startswith("every monster_* except "):
                if n.startswith("monster_") and n != words[-1]:
                    out.setdefault(n, s)
            elif item.startswith("every "):
                pre = words[1].rstrip("*")
                if n.startswith(pre):
                    out.setdefault(n, s)
            elif "/" in item:
                parts = [p.strip() for p in item.split("/")]
                base = parts[0].rsplit("_", 1)[0] + "_"
                full = [parts[0]] + [base + p for p in parts[1:]]
                if n in full:
                    out.setdefault(n, s)
            elif item.endswith("*"):
                if n.startswith(item[:-1]):
                    out.setdefault(n, s)
            elif n == item:
                out[n] = s
    return out


def run_map(binary, content, name):
    p = subprocess.run([binary, "--root", content, name], capture_output=True, text=True)
    return p.returncode, p.stdout.splitlines(), p.stderr


def check(name, rc, lines, err, own, fails):
    refusals = {}
    recs = []
    done = None
    for ln in lines:
        m = re.match(r"spawn: (\S+) map (\d+) lump (\d+) refused: (.*)", ln)
        if m:
            refusals[int(m.group(3))] = (m.group(1), m.group(4))
            continue
        m = re.match(r"rec (\d+) (\S+) (\S+)(.*)", ln)
        if m:
            recs.append((int(m.group(1)), m.group(2), m.group(3), m.group(4)))
            continue
        m = re.match(r"done (\d+)", ln)
        if m:
            done = int(m.group(1))
    ok = True
    count = collections.Counter()
    if rc != 0 or done is None:
        tail = "\n".join(lines[-6:])
        fails.append(f"{name}: driver exit {rc} (no 'done')\n{tail}\n{err[-800:]}")
        ok = False
    for lump, cn, state, rest in recs:
        count[state] += 1
        owner = own.get(cn, "?")
        if state == "live":
            m = re.search(r"solid=(\d+) movetype=(\d+)", rest)
            if m and int(m.group(1)) == SOLID_BSP and int(m.group(2)) != MOVETYPE_PUSH:
                fails.append(f"{name}: lump {lump} {cn} ({owner}): SOLID_BSP without MOVETYPE_PUSH")
                ok = False
        elif state in ("refused", "killed", "pending"):
            if lump not in refusals:
                fails.append(f"{name}: lump {lump} {cn} ({owner}): {state} with no logged reason")
                ok = False
    # a refusal for a missing implementation or without its reason fails,
    # logged or not (entity and logic records alike)
    for lump, (cn, why) in sorted(refusals.items()):
        if why in UNIMPLEMENTED or why.startswith(REASONLESS):
            fails.append(f"{name}: lump {lump} {cn} ({own.get(cn, '?')}): refused: {why}")
            ok = False
    print(f"{name}: {'PASS' if ok else 'FAIL'} records={done} "
          + " ".join(f"{k}={v}" for k, v in sorted(count.items()))
          + f" refusals_logged={len(refusals)}")
    for lump, (cn, why) in sorted(refusals.items()):
        print(f"  refusal {name} lump {lump} {cn} ({own.get(cn, '?')}): {why}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--maps", default=MAPS)
    a = ap.parse_args()
    own = owners()
    fails = []
    good = True
    for name in a.maps.split(","):
        rc, lines, err = run_map(a.bin, a.content, name)
        good &= check(name, rc, lines, err, own, fails)
    by_owner = collections.defaultdict(list)
    for f in fails:
        m = re.search(r"\((GameCore|GameMon|Items|\?)\)", f)
        by_owner[m.group(1) if m else "engine"].append(f)
    for o in sorted(by_owner):
        for f in by_owner[o]:
            print(f"FAIL [{o}] {f}")
    print("spawn-all PASS" if good else "spawn-all FAIL")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
