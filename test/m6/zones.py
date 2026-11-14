#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""zones.py -- per-scene CPU zone tables of PROFILE ares route logs, one or
two runs (before / after) side by side (M6 render / CPU work).

Scenes are spans of the route between SHOT ops (`route: N SHOT label`),
merged by --scene NAME=LAST_SHOT (repeatable, in route order: a scene runs
from the previous scene's last SHOT to its own); a roll-up window belongs to
the span it ends in, the first window (boot) is skipped. Per zone (the
`prof <zone>_us avg= max=` roll-ups; `frame` is the window's frame_us):
  avg  frame-weighted mean of the window means (ms)
  p90  90th percentile of the window means (ms)
  max  the largest window max (ms)
Per scene also: frames, frames > 50 ms (>= 4 vblanks in the pacing
histograms), and from the `ai: frame` lines (PROFILE, logged for frames
whose period passes 50 ms): `>50 fr` frames whose period passed 50 ms and
`>50 non-AI` those whose period less the frame's mon time still did.

Usage: zones.py LOG [LOG2] [--scene NAME=SHOT ...] [--zones a,b,...]
"""
import argparse
import re
import sys

WIN = re.compile(r"^prof frames=(\d+) frame_us=(\d+) .*?v1\.\.v7\+=([0-9/]+)")
ZONE = re.compile(r"^prof (\w+)_us (.*)")
SHOT = re.compile(r"^route: \d+ SHOT (\S+)")
AI = re.compile(r"^ai: frame .*?\bus=(\d+) .*?\bfr=(\d+)")
ACCT = re.compile(r"^acct (.*)")
DEFAULT_ZONES = ("frame,world,studio,studio.bones_us,fx,fx.trans_us,fx.decal,snd,snd.pump_us,"
                 "stream,hud,pmove,ent,phys,game,mon,rec,seam,acct.rspq")


def kv(text):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


def parse(path):
    """[(shot label ending the span or None, [windows], ai_over, ai_nonai)]"""
    spans = [[None, [], 0, 0]]
    win, nwin = None, 0
    for l in open(path, errors="replace"):
        m = SHOT.match(l)
        if m:
            spans[-1][0] = m.group(1)
            spans.append([None, [], 0, 0])
            continue
        m = WIN.match(l)
        if m:
            nwin += 1
            vh = [int(x) for x in m.group(3).split("/")]
            win = None
            if nwin > 1:
                win = {"n": int(m.group(1)), "over50": sum(vh[3:]),
                       "z": {"frame": (int(m.group(2)), int(m.group(2)))}}
                spans[-1][1].append(win)
            continue
        m = ZONE.match(l)
        if m and win is not None:
            d = kv(m.group(2))
            win["z"][m.group(1)] = (d.get("avg", 0), d.get("max", 0))
            for k, v in d.items():
                if k not in ("avg", "max"):
                    win["z"]["%s.%s" % (m.group(1), k)] = (v, v)
            continue
        m = ACCT.match(l)
        if m and win is not None:
            for k, v in kv(m.group(1)).items():
                win["z"]["acct.%s" % k] = (v, v)
            continue
        m = AI.match(l)
        if m:
            us, fr = int(m.group(1)), int(m.group(2))
            if fr > 50000:
                spans[-1][2] += 1
                if fr - us > 50000:
                    spans[-1][3] += 1
    return spans


def scenes(spans, defs):
    """defs [(name, last shot)] -> {name: (windows, over_fr, over_nonai)}"""
    out, i = {}, 0
    for name, last in defs:
        w, a, b = [], 0, 0
        while i < len(spans):
            sp = spans[i]
            i += 1
            w += sp[1]
            a += sp[2]
            b += sp[3]
            if sp[0] == last:
                break
        out[name] = (w, a, b)
    return out


def stats(wins, zone):
    vals = [(w["n"], w["z"][zone]) for w in wins if zone in w["z"]]
    if not vals:
        return None
    n = sum(f for f, _ in vals) or 1
    avg = sum(f * v[0] for f, v in vals) / n
    return avg / 1000.0, pct([v[0] for _, v in vals], 0.9) / 1000.0, max(v[1] for _, v in vals) / 1000.0


def fmt(s):
    return "%.1f / %.1f / %.1f" % s if s else "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--scene", action="append", default=[])
    ap.add_argument("--zones", default=DEFAULT_ZONES)
    a = ap.parse_args()
    runs = [parse(p) for p in a.logs[:2]]
    if a.scene:
        defs = [tuple(s.split("=", 1)) for s in a.scene]
    else:
        defs = [(sp[0] or "end", sp[0]) for sp in runs[0]]
    zones = a.zones.split(",")
    sc = [scenes(r, defs) for r in runs]
    tot = [[0, 0, 0, 0] for _ in runs]
    for name, _ in defs:
        print("\n#### %s\n" % name)
        hdr = "| zone (ms) | " + " | ".join(
            "%s avg / p90 / max" % ("before" if j == 0 else "after") for j in range(len(runs))) + " |"
        print(hdr)
        print("|---|" + "---|" * len(runs))
        for z in zones:
            print("| %s | %s |" % (z, " | ".join(fmt(stats(s[name][0], z)) for s in sc)))
        cells = []
        for j, s in enumerate(sc):
            w, ao, an = s[name]
            fr = sum(x["n"] for x in w)
            o50 = sum(x["over50"] for x in w)
            tot[j][0] += fr
            tot[j][1] += o50
            tot[j][2] += ao
            tot[j][3] += an
            cells.append("frames %d, >50 ms %d, ai-line >50 %d, non-AI >50 %d" % (fr, o50, ao, an))
        print("| frames | %s |" % " | ".join(cells))
    print("\n#### totals\n")
    for j, t in enumerate(tot):
        print("- %s: frames %d, >50 ms (pacing) %d, period >50 ms %d, non-AI (period - mon) >50 ms %d"
              % (a.logs[j], t[0], t[1], t[2], t[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
