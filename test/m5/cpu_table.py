#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""cpu_table.py -- the M5 gate's per-map CPU / memory table from a PROFILE
run's log (test/run/m5.sh, ares), with the old tree's cart numbers beside
it (test/m5/oracle/*-cart-frames.txt, test/m5/old_frames.py).

Map visits: the New Game map (`rec: new campaign <map>`), then one per
`seam: switch A->B` (a later New Game starts a visit too).

CPU, from the 1 s roll-ups (`prof frames=N frame_us=U pace ... v1..v7+=...`
then one `prof <zone>_us avg= max=` line per zone; the first window holds
boot and is skipped; a window belongs to the visit it ends in):
  frame avg     frame-weighted mean of the window means (ms)
  p90           90th percentile of the window means (no per-frame samples)
  max           the worst single frame's zone time: the largest zone max
                of any window (a lower bound of that frame's time), with
                that window's dominant zone (below)
  >50ms         frames over 50 ms: >= 4 vblanks in the windows' pacing
                histograms (NTSC 16.7 ms, the game presents every 2)
  mon>50        windows in which the mon zone alone (monster AI: think,
                schedules, routes) took > 50 ms in one frame
  >50ms@mon     frames over 50 ms in windows whose dominant zone is mon:
                the largest zone max once the container zones (ent holds
                phys, game holds mon) are set aside
  worst frames  the windows with the largest zone max, their visit, the
                approximate frame (frames counted before the window) and
                the zones over 10 ms in it
Memory per visit, peaks of the stats records in it: the world pools'
resident KB (render / texture / coll, of their size), rec live entities
and dehydrate store, and the roll-ups' studio pool and sound peaks. The
run-wide region table is test/m3/gate_table.py's.

Usage: cpu_table.py LOG [--old FILE] [--title TEXT]
       (Markdown; exit 1 when the log has no roll-up window)
"""
import argparse
import re
import sys

CONTAINERS = ("ent", "phys", "game")
NEW = re.compile(r"^rec: new campaign (\S+):")
SWITCH = re.compile(r"^seam: switch (\S+)->(\S+) ")
WIN = re.compile(r"^prof frames=(\d+) frame_us=(\d+) .*?v1\.\.v7\+=([0-9/]+)")
ZONE = re.compile(r"^prof (\w+)_us (.*)")
WORLD = re.compile(r"^stats \d+ world: .*render=(\d+)/(\d+)KB texture=(\d+)/(\d+)KB coll=(\d+)/(\d+)KB")
REC = re.compile(r"^stats \d+ rec: live=(\d+) .*store=(\d+)/(\d+)")


def kv(text):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


class Visit:
    def __init__(self, k, mp):
        self.k, self.map, self.wins, self.mem = k, mp, [], {}

    def peak(self, key, v):
        self.mem[key] = max(self.mem.get(key, 0), v)


def parse(lines):
    visits, cur, win, frames = [], None, None, 0
    nwin = 0

    def visit(mp):
        v = Visit(len(visits) + 1, mp)
        visits.append(v)
        return v

    for i, l in enumerate(lines, 1):
        m = NEW.match(l)
        if m:
            cur = visit(m.group(1))
            continue
        m = SWITCH.match(l)
        if m:
            cur = visit(m.group(2))
            continue
        m = WIN.match(l)
        if m:
            nwin += 1
            n, us = int(m.group(1)), int(m.group(2))
            vh = [int(x) for x in m.group(3).split("/")]
            win = None
            if nwin > 1 and cur:
                win = {"frames": n, "us": us, "over50": sum(vh[3:]), "zones": {}, "line": i,
                       "at": frames}
                cur.wins.append(win)
            frames += n
            continue
        m = ZONE.match(l)
        if m and win is not None:
            d = kv(m.group(2))
            win["zones"][m.group(1)] = d.get("max", 0)
            if m.group(1) == "mon":
                win["mon_avg"] = d.get("avg", 0)
            if cur:
                if m.group(1) == "studio" and "pool_peak" in d:
                    cur.peak("studio_pool", d["pool_peak"])
                if m.group(1) == "snd" and "mem_peak" in d:
                    cur.peak("snd_mem", d["mem_peak"])
            continue
        if cur is None:
            continue
        m = WORLD.match(l)
        if m:
            g = [int(x) for x in m.groups()]
            for j, key in enumerate(("render", "texture", "coll")):
                cur.peak(key, g[2 * j])
                cur.mem[key + "_size"] = g[2 * j + 1]
            continue
        m = REC.match(l)
        if m:
            cur.peak("live", int(m.group(1)))
            cur.peak("store", int(m.group(2)))
    return visits


def dominant(z):
    leaf = {k: v for k, v in z.items() if k not in CONTAINERS}
    return max(leaf, key=leaf.get) if leaf else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--old")
    ap.add_argument("--title", default="run")
    a = ap.parse_args()
    lines = [l.rstrip("\r\n") for l in open(a.log, errors="replace")]
    visits = [v for v in parse(lines) if v.wins]
    if not visits:
        print("no roll-up windows in the log")
        return 1
    old = {}
    if a.old:
        for l in open(a.old):
            m = re.match(r"visit (\d+) (\S+) windows=(\d+)(?: avg_ms=(\S+) p90_ms=(\S+) worst_ms=(\S+))?", l)
            if m and m.group(4):
                old[int(m.group(1))] = (m.group(2), m.group(4), m.group(5), m.group(6))
    out = ["", "### M5 gate: CPU per map visit (%s; ares PROFILE, 1 s roll-ups; ms)" % a.title, "",
           "| # | map | windows | frames | frame avg | p90 | max (dominant) | >50ms | mon>50 | >50ms@mon | "
           "old cart avg / p90 / worst window |",
           "|---:|---|---:|---:|---:|---:|---|---:|---:|---:|---|"]
    allw = []
    tot = {"over50": 0, "mon50": 0, "over50mon": 0}
    for v in visits:
        n = sum(w["frames"] for w in v.wins)
        avg = sum(w["frames"] * w["us"] for w in v.wins) / max(n, 1) / 1000.0
        p90 = pct([w["us"] for w in v.wins], 0.9) / 1000.0
        worst = max(v.wins, key=lambda w: max(w["zones"].values(), default=0))
        wmax = max(worst["zones"].values(), default=0) / 1000.0
        wz = dominant(worst["zones"]) or "-"
        over50 = sum(w["over50"] for w in v.wins)
        mon50 = sum(1 for w in v.wins if w["zones"].get("mon", 0) > 50000)
        over50mon = sum(w["over50"] for w in v.wins if dominant(w["zones"]) == "mon")
        tot["over50"] += over50
        tot["mon50"] += mon50
        tot["over50mon"] += over50mon
        o = old.get(v.k)
        oc = "%s / %s / %s (%s)" % (o[1], o[2], o[3], o[0]) if o and o[0] == v.map else "—"
        out.append("| %d | %s | %d | %d | %.1f | %.1f | %.1f (%s) | %d | %d | %d | %s |" % (
            v.k, v.map, len(v.wins), n, avg, p90, wmax, wz, over50, mon50, over50mon, oc))
        allw += [(max(w["zones"].values(), default=0), v, w) for w in v.wins]
    out.append("")
    out.append("totals: frames >50 ms %d; windows with mon > 50 ms in a frame %d; frames >50 ms in "
               "mon-dominant windows %d" % (tot["over50"], tot["mon50"], tot["over50mon"]))
    out += ["", "#### worst frames (by the largest zone max of a window)", "",
            "| zone max ms | visit | ~frame | log line | dominant | zones > 10 ms |", "|---:|---|---:|---:|---|---|"]
    for mx, v, w in sorted(allw, key=lambda t: -t[0])[:8]:
        zs = ", ".join("%s %.0f" % (k, x / 1000.0) for k, x in sorted(w["zones"].items(), key=lambda t: -t[1])
                       if x > 10000)
        out.append("| %.1f | %d %s | %d | %d | %s | %s |" % (mx / 1000.0, v.k, v.map, w["at"], w["line"],
                                                           dominant(w["zones"]), zs))
    monw = [(v, w) for _, v, w in allw
            if (w["over50"] and dominant(w["zones"]) == "mon") or w["zones"].get("mon", 0) > 50000]
    monw.sort(key=lambda t: t[1]["at"])
    out += ["", "#### windows with frames > 50 ms where mon dominates, or with a mon frame > 50 ms "
            "(%d windows, %d frames > 50 ms)" % (len(monw), sum(w["over50"] for _, w in monw)), "",
            "| visit | ~frames | log line | frames > 50 ms | mon max ms | mon avg ms | dominant | zones > 10 ms |",
            "|---|---|---:|---:|---:|---:|---|---|"]
    for v, w in monw:
        zs = ", ".join("%s %.0f" % (k, x / 1000.0) for k, x in sorted(w["zones"].items(), key=lambda t: -t[1])
                       if x > 10000)
        out.append("| %d %s | %d-%d | %d | %d | %.1f | %.1f | %s | %s |" % (
            v.k, v.map, w["at"], w["at"] + w["frames"] - 1, w["line"], w["over50"],
            w["zones"].get("mon", 0) / 1000.0, w.get("mon_avg", 0) / 1000.0, dominant(w["zones"]), zs))
    out += ["", "#### memory per map visit (peaks of its stats records / roll-ups)", "",
            "| # | map | render KB | texture KB | coll KB | rec live | dehydrate store B | studio pool B | "
            "snd mem B |", "|---:|---|---:|---:|---:|---:|---:|---:|---:|"]
    for v in visits:
        m = v.mem

        def pool(k):
            return "%d/%d" % (m[k], m[k + "_size"]) if k in m else "—"
        out.append("| %d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            v.k, v.map, pool("render"), pool("texture"), pool("coll"), m.get("live", "—"), m.get("store", "—"),
            m.get("studio_pool", "—"), m.get("snd_mem", "—")))
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
