#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Player-move tests against the GoldSrc probe fixtures (docs/specs/pmove-host.md 7).

PM2  every PM-bot run replayed through the pmove host (test/m2/pm.c):
     per-frame origin within 0.01 u and flags equal for all its frames;
PM3  the T-segments pmove traces (PM_PlayerTrace, PM_WORLD_ONLY, usehull
     mapped from the hull) against the world physent: flags exact,
     fraction +-1e-5, end +-0.001 u, normal exact, distance +-1e-4, physent;
PM9  ladder climb speed 200 u/s at pitch 0 and 282.84 u/s at pitch -45
     (+-0.01), on every frame the probe measured that speed;
and the self-contained tests (PM1, PM3 rules, PM4-PM7) on each map, at the
first PM-bot run's start (clear level floor).

  pmbot.py --bin BUILD/host/hl64-pm --content CONTENT --fixtures FIXTURES
           [--maps hldemo1 ...] [--verbose]

FIXTURES is $HL64_FIXTURES/goldsrc. Exit 0 when every check passes.

The replay rebuilds each map's GoldSrc state: brush entities from the
fixture's `brush_ents` line in their spawn state, the probe's step brushes
for the step runs, non-brush entities from `ents` lines; entity slots keep
GoldSrc's index order. Each run starts from its `reset` state and chains
its frames (the command of each frame, the state carried over).
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

MOVE_TOL = 0.01
LADDER = {"ladder": 282.84271, "ladder_level": 200.0}
# FL_* bits pm_shared and the pmove host own; the probe's other bits
# (FL_FAKECLIENT, FL_CLIENT, FL_GODMODE) only ride along
USEHULL_OF_HULL = {1: 0, 3: 1, 0: 2, 2: 3}


def load(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def vec(v):
    return " ".join(repr(float(x)) for x in v)


class Replay:
    def __init__(self, recs):
        self.recs = recs
        self.meta = recs[0]["meta"]
        self.brush = recs[1]["brush_ents"]
        idx = {e["i"] for e in self.brush}
        self.steps = {}
        for r in recs:
            if "step" in r and "ent" in r:
                self.steps[r["step"]] = r["ent"]
                idx.add(r["ent"]["i"])
            for e in r.get("ents", []):
                idx.add(e["i"])
        order = sorted(i for i in idx if i > 1)
        if len(order) + 2 > 495:
            raise SystemExit(f"{self.meta['map']}: {len(order)} entities over the slot cap")
        self.slot = {i: 2 + n for n, i in enumerate(order)}
        self.slot[0] = 0
        self.unslot = {s: i for i, s in self.slot.items()}

    def ent_line(self, e):
        m = e.get("model") or ""
        sub = int(m[1:]) if m.startswith("*") else -1
        owner = self.slot.get(e.get("owner", -1), -1) if e.get("owner", -1) > 0 else -1
        return (f"ent {self.slot[e['i']]} {sub} {vec(e['o'])} {vec(e['a'])} {vec(e['mins'])} "
                f"{vec(e['maxs'])} {e['solid']} {e['skin']} {e['rendermode']} {owner} "
                f"{e.get('flags', 0)}")

    def ge(self, i):
        return -1 if i < 0 else self.slot.get(i, -2)

    def script(self, selftest):
        out = [f"map {self.meta['map']}"]
        out += [self.ent_line(e) for e in self.brush]
        runs = {}
        placed_step = None
        frames = []
        for r in self.recs[2:]:
            if "ents" in r:
                out += [self.ent_line(e) for e in r["ents"]]
            if "kind" in r:
                runs[r["run"]] = r
                if placed_step is not None:
                    out.append(f"free {self.slot[placed_step]}")
                    placed_step = None
                if r["kind"] in ("step18", "step19"):
                    st = self.steps.get(int(r["kind"][4:]))
                    if st:
                        out.append(self.ent_line(st))
                        placed_step = st["i"]
                s = r["reset"]
                hull = 1 if s["mins"][2] == -18 else 0
                out.append(f"reset {s['t']!r} {vec(s['o'])} {vec(s['v'])} {vec(s['bv'])} {s['fl']} "
                           f"{vec(s['vo'])} {s['wl']} {s['wt']} {self.ge(s['ge'])} {s['ind']} "
                           f"{s['dt']!r} {s['fv']!r} {vec(s['pa'])} {s['mt']} {hull}")
            if "cmd" in r:
                c = r["cmd"]
                out.append(f"cmd {r['t']!r} {vec(c['a'])} {c['fm']!r} {c['sm']!r} {c['um']!r} "
                           f"{c['b']} {c['ms']}")
                frames.append(r)
        if selftest and runs:
            out.append(f"selftest {vec(runs[min(runs)]['start'])}")
        return "\n".join(out) + "\n", runs, frames


def compare_runs(rep, runs, frames, flines, verbose):
    res = {}
    for r, f in zip(frames, flines):
        v = f.split()
        org = [float(x) for x in v[1:4]]
        vel = [float(x) for x in v[4:7]]
        fl = int(v[7])
        ge = int(v[11])
        mt = int(v[18])
        st = res.setdefault(r["run"], {"n": 0, "bad": 0, "first": None, "maxd": 0.0,
                                       "flags": 0, "ge": 0, "maxv": 0.0, "ladder": [0, 0]})
        st["n"] += 1
        d = max(abs(a - b) for a, b in zip(org, r["o"]))
        dv = max(abs(a - b) for a, b in zip(vel, r["v"]))
        st["maxd"] = max(st["maxd"], d)
        st["maxv"] = max(st["maxv"], dv)
        geg = rep.ge(r["ge"])
        ok = d <= MOVE_TOL and fl == r["fl"]
        if fl != r["fl"]:
            st["flags"] += 1
        if ge != geg:
            st["ge"] += 1
        if not ok:
            st["bad"] += 1
            if st["first"] is None:
                st["first"] = (r["f"], org, r["o"], fl, r["fl"], vel, r["v"])
        kind = runs[r["run"]]["kind"]
        if kind in LADDER and r["mt"] == 5 and abs(r["v"][2] - LADDER[kind]) <= 0.01:
            st["ladder"][0] += 1
            if abs(vel[2] - LADDER[kind]) > 0.01 or mt != 5:
                st["ladder"][1] += 1
    return res


def check_pm3(fx, mp, binp, content, verbose):
    """T-segments pmove traces through the world physent."""
    path = fx / "trace-and-hulls" / "T-segments" / f"{mp}.jsonl"
    if not path.exists():
        return None
    recs = [r for r in load(path) if "pm" in r]
    lines = [f"map {mp}"] + [f"pmtrace {USEHULL_OF_HULL[r['hull']]} {vec(r['s'])} {vec(r['e'])}"
                             for r in recs]
    p = subprocess.run([binp, "--root", content], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    out = [l for l in p.stdout.splitlines() if l.startswith("pm ")]
    if len(out) != len(recs):
        return (len(recs), len(recs), f"harness: {p.stdout[-300:]}{p.stderr[-300:]}")
    bad = 0
    first = None
    for r, l in zip(recs, out):
        v = l.split()
        g = r["pm"]
        got = dict(as_=int(v[1]), ss=int(v[2]), io=int(v[3]), iw=int(v[4]), fr=float(v[5]),
                   end=[float(x) for x in v[6:9]], n=[float(x) for x in v[9:12]], d=float(v[12]),
                   pe=int(v[13]))
        ok = (got["as_"] == g["as"] and got["ss"] == g["ss"] and got["io"] == g["io"]
              and got["iw"] == g["iw"] and abs(got["fr"] - g["fr"]) <= 1e-5
              and max(abs(a - b) for a, b in zip(got["end"], g["end"])) <= 1e-3
              and got["pe"] == g["pe"])
        if ok and g["fr"] < 1 and not g["as"]:
            ok = (max(abs(a - b) for a, b in zip(got["n"], g["n"])) <= 1e-6
                  and abs(got["d"] - g["d"]) <= 1e-4)
        if not ok:
            bad += 1
            if first is None:
                first = f"hull {r['hull']} s {r['s']} e {r['e']}: got {got} want {g}"
    return (len(recs), bad, first)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--fixtures", required=True, type=Path)
    ap.add_argument("--maps", nargs="*")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    fdir = a.fixtures / "pmove-host" / "PM-bot"
    maps = a.maps or sorted(p.stem for p in fdir.glob("*.jsonl"))
    fails = 0
    tot = {"runs": 0, "frames": 0, "ladder": 0, "pm3": 0}
    for mp in maps:
        rep = Replay(load(fdir / f"{mp}.jsonl"))
        text, runs, frames = rep.script(selftest=True)
        p = subprocess.run([a.bin, "--root", a.content], input=text, capture_output=True, text=True)
        flines = [l for l in p.stdout.splitlines() if l.startswith("f ")]
        if len(flines) != len(frames):
            print(f"PM2 FAIL {mp}: harness gave {len(flines)} of {len(frames)} frames")
            print(p.stdout[-2000:], p.stderr[-1000:])
            fails += 1
            continue
        res = compare_runs(rep, runs, frames, flines, a.verbose)
        for r in sorted(res):
            st, h = res[r], runs[r]
            tot["runs"] += 1
            tot["frames"] += st["n"]
            tag = f"{mp} run {r:2d} {h['kind']:<12} msec {h['msec']:2d}"
            if st["bad"]:
                fails += 1
                f0 = st["first"]
                print(f"PM2 FAIL {tag}: {st['bad']}/{st['n']} frames (flags {st['flags']}), "
                      f"max |d| {st['maxd']:.4g}; first f{f0[0]}: org {f0[1]} want {f0[2]} "
                      f"fl {f0[3]} want {f0[4]} vel {f0[5]} want {f0[6]}")
            elif a.verbose:
                print(f"PM2 ok   {tag}: {st['n']} frames, max |d| {st['maxd']:.3g} "
                      f"max |dv| {st['maxv']:.3g} ground mismatches {st['ge']}")
            if h["kind"] in LADDER:
                n, bad = st["ladder"]
                tot["ladder"] += n
                if n == 0 or bad:
                    fails += 1
                    print(f"PM9 FAIL {tag}: {bad} of {n} climb frames off {LADDER[h['kind']]}")
                else:
                    print(f"PM9 ok   {tag}: {n} climb frames at {LADDER[h['kind']]} +-0.01")
        st = [l for l in p.stdout.splitlines() if l.startswith(("selftest", "FAIL", "PM"))]
        if "selftest ok" not in st:
            fails += 1
            print(f"selftest FAIL {mp}:", *st, sep="\n  ")
        elif a.verbose:
            print(f"selftest ok {mp} (PM1, PM3 rules, PM4-PM7)")
        r3 = check_pm3(a.fixtures, mp, a.bin, a.content, a.verbose)
        if r3:
            n, bad, first = r3
            tot["pm3"] += n
            if bad:
                fails += 1
                print(f"PM3 FAIL {mp}: {bad}/{n} pmove traces; first: {first}")
            elif a.verbose:
                print(f"PM3 ok   {mp}: {n} pmove traces")
    print(f"pmbot: {tot['runs']} runs / {tot['frames']} frames (PM2), {tot['ladder']} ladder "
          f"frames (PM9), {tot['pm3']} pmove traces (PM3), selftests on {len(maps)} maps: "
          + ("PASS" if not fails else f"FAIL ({fails})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
