#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The Hazard Course end tram against GoldSrc (test/run/m5.sh suite tram).

Runs test/m5/tram-ride.route on hl64-host at the fixture's frame time
(0.01 s) and compares the ride with the GoldSrc capture
physics-movetypes/P-tracktrain t0a0d, scenario `drive` (a fake client on
func_tracktrain *23's deck: +use, then +forward at frames 50, 150, 170,
190):

  controls  the player takes the train's controls (`player: train E
            controls`);
  notches   the speed steps equal GoldSrc's (50, 100, 150, 200 u/s), each
            at GoldSrc's train ltime (the route presses on the fixture's
            schedule);
  stop      the dead end at GoldSrc's train ltime (8.25: 8.05 s after the
            first notch), the train's origin and angles then equal
            GoldSrc's last record (184 -2506 -64, yaw 90: 18 u short of
            train1a, the 0.01 s frames' exact look-ahead);
  rider     the player stands at the train's origin + GoldSrc's rel
            (0 0 36.03), on it (ground = the train).

  python3 test/m5/tram.py BIN --root CONTENT --fixtures DIR

Exit 0 when every check passes, 1 otherwise.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

CAPTURE = "physics-movetypes/P-tracktrain/t0a0d.jsonl"
ROUTE = "test/m5/tram-ride.route"
TOL = 0.01
LT_TOL = 0.005	# the log's ltime is to 0.01 s
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fixture(path):
    """GoldSrc's drive: frame time, train, notches [(frame, speed)], stop
    frame, the train's last origin / angles and the rider's rel."""
    name = None
    ft = None
    train = None
    notches = []
    stop = None
    last = None
    rel = None
    lt = {}
    with open(path) as f:
        for line in f:
            d = json.loads(line)
            if "scenario" in d:
                name = d.get("name")
                continue
            if name != "drive" or d.get("sc") is None:
                continue
            if "ft" in d and ft is None:
                ft = d["ft"]
            if d.get("ev") == "train":
                if train is None:
                    train = d["i"]
                if d["i"] == train and d["speed"] != d["was"]:
                    if d["speed"] == 0:
                        stop = d["f"]
                    else:
                        notches.append((d["f"], d["speed"]))
            elif "e" in d and isinstance(d["e"], list):
                for e in d["e"]:
                    lt[e["i"], d["f"]] = e["lt"]
                    if e["i"] == train:
                        last = e
            elif "pl" in d and d["pl"].get("ontrain"):
                rel = d["pl"]["rel"]
    if None in (ft, train, stop, last, rel) or not notches:
        sys.exit(f"{path}: no drive scenario (ft {ft}, train {train}, stop {stop})")
    # the train's ltime at each step: a notch's Use runs before the frame's
    # physics (the previous record's ltime), the stop in its think (the
    # frame's own)
    try:
        notches = [(lt[train, f - 1], v) for f, v in notches]
        stop = lt[train, stop]
    except KeyError as k:
        sys.exit(f"{path}: no train record at frame {k}")
    return ft, notches, stop, last["o"], last["a"], rel


def vec(s):
    return [float(x) for x in s.split(",")]


def ours(binary, content, rbc, ft):
    p = subprocess.run([binary, "--root", content, "--route", rbc,
                        "--cmd", f"@1 fixed_dt {round(ft * 1e6)}", "--cmd", "@1 entsdump 1"],
                       capture_output=True, text=True)
    out = p.stdout + p.stderr
    r = {"exit": p.returncode, "pass": "route PASS" in out.splitlines(), "notches": [],
         "train": None, "stop": None, "torg": None, "tang": None, "porg": None, "pground": None}
    for line in out.splitlines():
        m = re.match(r"player: train (\d+) controls", line)
        if m and r["train"] is None:
            r["train"] = int(m.group(1))
        t = r["train"]
        if t is None:
            continue
        m = re.match(rf"game: train {t} speed (-?[\d.]+) lt=([\d.]+)", line)
        if m:
            r["notches"].append((float(m.group(2)), float(m.group(1))))
        m = re.match(rf"game: train {t} dead end lt=([\d.]+)", line)
        if m:
            r["stop"] = float(m.group(1))
        m = re.match(rf"stats \S+ entcv {t} org=(\S+) ang=(\S+) ", line)
        if m:
            r["torg"], r["tang"] = vec(m.group(1)), vec(m.group(2))
        m = re.match(r"stats \S+ player: map=\S+ org=(\S+) .* ground=(-?\d+)", line)
        if m:
            r["porg"], r["pground"] = vec(m.group(1)), int(m.group(2))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--root", required=True, help="cooked content")
    ap.add_argument("--fixtures", required=True)
    a = ap.parse_args()
    ft, notches, stop, end_o, end_a, rel = fixture(os.path.join(a.fixtures, "goldsrc", CAPTURE))
    with tempfile.TemporaryDirectory() as tmp:
        rbc = os.path.join(tmp, "tram-ride.rbc")
        c = subprocess.run([sys.executable, "-m", "tools.routec.routec", "--rtb",
                            os.path.join(a.root, "campaign.rtb"), ROUTE, "-o", rbc],
                           cwd=ROOT, capture_output=True, text=True)
        if c.returncode:
            sys.exit(f"routec {ROUTE}: {c.stdout}{c.stderr}")
        r = ours(a.binary, a.root, rbc, ft)
    bad = []
    if r["exit"] or not r["pass"]:
        bad.append(f"route: exit {r['exit']}, PASS {r['pass']}")
    if r["train"] is None:
        bad.append("controls: the player never took a train's controls")
    gs = [s for _, s in notches]
    hs = [s for _, s in r["notches"]]
    if hs != gs:
        bad.append(f"notches: speeds {hs}, GoldSrc {gs}")
    else:
        for (got, v), (want, _) in zip(r["notches"], notches):
            if abs(got - want) > LT_TOL:
                bad.append(f"notches: {v:g} at train ltime {got:.2f}, GoldSrc {want:.2f}")
    if r["stop"] is None:
        bad.append("stop: no dead end")
    elif abs(r["stop"] - stop) > LT_TOL:
        bad.append(f"stop: dead end at train ltime {r['stop']:.2f}, GoldSrc {stop:.2f}")
    if r["torg"] is None or any(abs(x - y) > TOL for x, y in zip(r["torg"], end_o)):
        bad.append(f"stop: train origin {r['torg']}, GoldSrc {end_o}")
    if r["tang"] is None or any(abs(x - y) > TOL for x, y in zip(r["tang"], end_a)):
        bad.append(f"stop: train angles {r['tang']}, GoldSrc {end_a}")
    if r["porg"] is None or r["torg"] is None:
        bad.append("rider: no player / train record")
    else:
        d = [p - t for p, t in zip(r["porg"], r["torg"])]
        if any(abs(x - y) > TOL for x, y in zip(d, rel)) or r["pground"] != r["train"]:
            bad.append(f"rider: player - train {d} on {r['pground']}, GoldSrc {rel} on the train")
    for b in bad:
        print(f"FAIL {b}")
    if bad:
        return 1
    print(f"tram: PASS (notches {gs} at GoldSrc's train ltimes, dead end at {stop:.2f} "
          f"({stop - notches[0][0]:.2f} s after the first) at {end_o}, rider +{rel[2]:.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
