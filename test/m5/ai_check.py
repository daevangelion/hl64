#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ai_check.py -- the M5 AI acceptance check on a PROFILE ares run (M5:
monster AI cost bounded; Main's rule): no frame over 50 ms in which the mon
zone takes half the frame's CPU or more.

Per frame, from `seamprof: slow frame=F frame_us=U <zone>=<us> ...`
(engine/test/seamprof.c: every frame over 50 ms, its zones of 1 ms or
more, mon and the framebuffer wait always):
  CPU          frame_us - wait (the framebuffer wait is not CPU)
  mon-dominant mon * 2 >= CPU   (the gate: 0 such frames)
  AI-pushed    frame_us - mon < 50 ms < frame_us (without the monster
               thinks the frame would have made 50 ms; reported)
Frames are given to map visits in log order (`rec: new campaign <map>`,
`seam: switch A->B`).

Per map visit also mon p50 / p90 of the 1 s roll-up window means and the
worst mon frame (`prof mon_us avg= max=`; the roll-ups carry no per-frame
samples below 50 ms), and the worst AI frames with their zones.

Positive control: a run whose pacing histograms (`prof frames=... v1..v7+=`)
show frames of 4 vblanks or more (> 50 ms) but has no `seamprof: slow` line
was not instrumented: FAIL.

Usage: ai_check.py LOG [--name NAME]      (Markdown + `check NAME-ai PASS|FAIL`)
       ai_check.py --selftest
"""
import argparse
import contextlib
import io
import os
import re
import sys
import tempfile

LIM = 50000
NEW = re.compile(r"^rec: new campaign (\S+):")
SWITCH = re.compile(r"^seam: switch (\S+)->(\S+) ")
WIN = re.compile(r"^prof frames=\d+ .*?v1\.\.v7\+=([0-9/]+)")
MON = re.compile(r"^prof mon_us avg=(\d+) max=(\d+)")


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


def kv(text):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", text)}


def parse(lines):
    visits, cur, slow_v4, nwin = [], None, 0, 0

    def visit(mp):
        v = {"k": len(visits) + 1, "map": mp, "slow": [], "mon_avg": [], "mon_max": 0}
        visits.append(v)
        return v

    for l in lines:
        m = NEW.match(l)
        if m:
            cur = visit(m.group(1))
            continue
        m = SWITCH.match(l)
        if m:
            cur = visit(m.group(2))
            continue
        if cur is None:
            continue
        if l.startswith("seamprof: slow "):
            cur["slow"].append(kv(l[len("seamprof: slow "):]))
            continue
        m = WIN.match(l)
        if m:
            nwin += 1
            if nwin > 1:	# the first window holds boot
                slow_v4 += sum(int(x) for x in m.group(1).split("/")[3:])
            continue
        m = MON.match(l)
        if m and nwin > 1:
            cur["mon_avg"].append(int(m.group(1)))
            cur["mon_max"] = max(cur["mon_max"], int(m.group(2)))
    return visits, slow_v4


def cpu(d):
    return d["frame_us"] - d.get("wait", 0)


def dom(d):
    return d.get("mon", 0) * 2 >= cpu(d)


def pushed(d):
    return d["frame_us"] - d.get("mon", 0) < LIM < d["frame_us"]


def zones(d):
    return " ".join("%s=%.1f" % (k, v / 1000.0) for k, v in sorted(d.items(), key=lambda t: -t[1])
                    if k not in ("frame", "frame_us") and v >= 1000)


def check(path, name):
    lines = [l.rstrip("\r\n") for l in open(path, errors="replace")]
    visits, v4 = parse(lines)
    allslow = [(v, d) for v in visits for d in v["slow"]]
    out = ["", "### M5 AI frame budget (%s; ares PROFILE; frames > 50 ms from `seamprof: slow`, "
           "mon p50 / p90 of 1 s window means)" % name, "",
           "| # | map | mon p50 ms | mon p90 ms | mon max ms | frames > 50 ms | mon-dominant | AI-pushed |",
           "|---:|---|---:|---:|---:|---:|---:|---:|"]
    for v in visits:
        out.append("| %d | %s | %.1f | %.1f | %.1f | %d | %d | %d |" % (
            v["k"], v["map"], pct(v["mon_avg"], 0.5) / 1000.0, pct(v["mon_avg"], 0.9) / 1000.0,
            v["mon_max"] / 1000.0, len(v["slow"]), sum(1 for d in v["slow"] if dom(d)),
            sum(1 for d in v["slow"] if pushed(d))))
    nd = [(v, d) for v, d in allslow if dom(d)]
    npush = [(v, d) for v, d in allslow if pushed(d)]
    out += ["", "frames > 50 ms: %d; mon-dominant (mon >= half the frame CPU): %d; AI-pushed "
            "(frame - mon < 50 ms): %d" % (len(allslow), len(nd), len(npush)), "",
            "#### worst AI frames (by mon)", "", "| frame | visit | frame ms | mon ms | CPU ms | zones ms |",
            "|---:|---|---:|---:|---:|---|"]
    for v, d in sorted(allslow, key=lambda t: -t[1].get("mon", 0))[:12]:
        out.append("| %d | %d %s | %.1f | %.1f | %.1f | %s |" % (
            d["frame"], v["k"], v["map"], d["frame_us"] / 1000.0, d.get("mon", 0) / 1000.0,
            cpu(d) / 1000.0, zones(d)))
    print("\n".join(out))
    if not allslow and v4:
        line = "check %s-ai FAIL no `seamprof: slow` line, but the pacing shows %d frames of >= 4 vblanks " \
               "(seamprof not logging slow frames)" % (name, v4)
    elif not visits:
        line = "check %s-ai FAIL no map visit in the log" % name
    elif nd:
        line = "check %s-ai FAIL %d frames > 50 ms with mon >= half the frame CPU: %s" % (
            name, len(nd), ", ".join("%d (%s, mon %.0f of %.0f ms)" % (d["frame"], v["map"], d.get("mon", 0) / 1e3,
                                                                    cpu(d) / 1e3) for v, d in nd[:8]))
    else:
        line = "check %s-ai PASS 0 of %d frames > 50 ms mon-dominant; %d AI-pushed" % (name, len(allslow), len(npush))
    print(line)
    return 0 if " PASS " in line else 1


def selftest():
    d = tempfile.mkdtemp(prefix="m5ai.")
    base = ["rec: new campaign a: 1 spawned", "prof frames=30 frame_us=33000 pace v1..v7+=0/30/0/0/0/0/0",
            "prof frames=30 frame_us=34000 pace v1..v7+=0/29/0/1/0/0/0", "prof mon_us avg=3000 max=20000",
            "seamprof: slow frame=40 frame_us=60000 wait=100 mon=20000 render=30000",
            "seam: switch a->b landmark l (touch) frame=50",
            "prof frames=30 frame_us=33000 pace v1..v7+=0/30/0/0/0/0/0", "prof mon_us avg=2000 max=9000"]
    cases = [("clean", base, True),
             ("mon-dominant", base + ["seamprof: slow frame=60 frame_us=70000 wait=0 mon=35000 render=20000"], False),
             ("pushed only", base + ["seamprof: slow frame=61 frame_us=55000 wait=0 mon=10000 render=40000"], True),
             ("wait not CPU", base + ["seamprof: slow frame=62 frame_us=80000 wait=40000 mon=20000"], False),
             ("uninstrumented", [l for l in base if not l.startswith("seamprof")], False)]
    bad = []
    for label, lines, want in cases:
        p = os.path.join(d, "log")
        with open(p, "w") as f:
            f.write("\n".join(lines) + "\n")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = check(p, "t")
        if (code == 0) != want:
            bad.append("%s: exit %d" % (label, code))
    for b in bad:
        print("selftest FAIL " + b)
    print("ai_check selftest %s (%d cases)" % ("FAIL" if bad else "PASS", len(cases)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?")
    ap.add_argument("--name", default="route")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    return check(a.log, a.name)


if __name__ == "__main__":
    sys.exit(main())
