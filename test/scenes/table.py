#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""table.py -- per-scene perf table of a PROFILE run log (ares or host),
one log or two (before / after) side by side (test/README.md, scenes).

A log's window is given as LOG[@FROM:TO]:
  no @       the scene markers: from `route: N STATS begin` to
             `route: N STATS end` (test/scenes/*.route); a log without
             them: the whole log after the boot window
  FROM, TO   a route op number (`route: FROM ...`) or a SHOT / STATS label
             (`route: N SHOT FROM`); `@vent-flashlight:grunts-fight` is
             the whole-route span the hl2-yard scene replays
A 1 s roll-up window (`prof frames=N frame_us=U ... v1..v7+=...`, PROFILE)
belongs to the window when its line is printed after FROM's route line and
before TO's (test/m6/zones.py's rule: a roll-up belongs to the span it ends
in); the log's first roll-up (boot) never counts. Rows:
  frame avg / p90 / max  frame-weighted mean of the roll-up means, their
             90th percentile, the slowest frame (`seamprof: slow
             frame_us=`, frames over 50 ms; none: the largest roll-up
             mean) (ms)
  frames, >50 ms   frames of the window, those of 4 vblanks or more in the
             pacing histograms
  slow logged, mon-dominant, AI-pushed   `seamprof: slow` lines in the
             window; of them mon >= half the frame's CPU (frame_us - wait;
             test/m5/ai_check.py's gate) and frame_us - mon < 50 ms
  <zone> avg / max   `prof <zone>_us avg= max=` per zone (frame-weighted
             mean of the window means, largest window max); `zone.key`
             reads key= of that zone's line (e.g. studio.bones_us)

Usage: table.py LOG[@FROM:TO] [LOG2[@FROM:TO]] [--title T] [--zones a,b,...]
       [--summary NAME]   one line `scene NAME ...` (runner table) instead
       table.py --selftest
"""
import argparse
import os
import re
import sys
import tempfile

ROUTE = re.compile(r"^route: (\d+) (\w+)(?: (\S+))?\s*$")
WIN = re.compile(r"^prof frames=(\d+) frame_us=(\d+) .*?v1\.\.v7\+=([0-9/]+)")
ZONE = re.compile(r"^prof (\w+)_us (.*)")
SLOW = "seamprof: slow "
LIM = 50000
DEFAULT_ZONES = ("world,studio,studio.bones_us,fx,fx.trans_us,fx.decal,snd,snd.pump_us,stream,hud,"
                 "pmove,ent,phys,game,mon,rec,seam")


def kv(text):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", text)}


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


def marker(spec, n, op, label):
    """spec: None (scene markers), an op number or a label"""
    if spec is None:
        return None
    if spec.isdigit():
        return int(spec) == n
    return op in ("SHOT", "STATS") and label == spec


def parse(path, frm=None, to=None):
    """the window's roll-ups and slow frames; frm/to as for marker()"""
    if frm is None and to is None:
        frm, to, scene = "begin", "end", True
    else:
        scene = False
    state = 0 if frm else 1	# 0 before FROM, 1 inside, 2 after TO
    seen_from = seen_to = False
    nwin, cur = 0, None
    w = {"wins": [], "slow": [], "path": path}
    for raw in open(path, errors="replace"):
        l = raw.rstrip("\r\n")
        m = ROUTE.match(l)
        if m:
            n, op, label = int(m.group(1)), m.group(2), m.group(3)
            if state == 0 and marker(frm, n, op, label):
                state, seen_from = 1, True
            elif state == 1 and to and marker(to, n, op, label):
                state, seen_to = 2, True
            continue
        m = WIN.match(l)
        if m:
            nwin += 1
            cur = None
            if nwin > 1 and state == 1:
                vh = [int(x) for x in m.group(3).split("/")]
                cur = {"n": int(m.group(1)), "frame_us": int(m.group(2)), "over50": sum(vh[3:]),
                       "vh": vh, "z": {}}
                w["wins"].append(cur)
            continue
        if cur is not None:
            m = ZONE.match(l)
            if m:
                cur["z"][m.group(1)] = kv(m.group(2))
                continue
        if l.startswith(SLOW) and state == 1:
            w["slow"].append(kv(l[len(SLOW):]))
    if scene and not seen_from:
        # no scene markers: the whole log after boot
        return parse_all(path)
    if frm and not seen_from:
        raise SystemExit("table.py: %s: no route op %s" % (path, frm))
    if to and not seen_to:
        w["open"] = True	# the run ended inside the window
    return w


def parse_all(path):
    w = parse(path, "", "")
    w["whole"] = True
    return w


def cpu(d):
    return d.get("frame_us", 0) - d.get("wait", 0)


def stats(w, zones):
    wins = w["wins"]
    n = sum(x["n"] for x in wins)
    s = {"frames": n, "over50": sum(x["over50"] for x in wins), "slow": len(w["slow"]),
         "dom": sum(1 for d in w["slow"] if d.get("mon", 0) * 2 >= cpu(d)),
         "pushed": sum(1 for d in w["slow"] if d.get("frame_us", 0) - d.get("mon", 0) < LIM < d.get("frame_us", 0)),
         "z": {}}
    means = [x["frame_us"] for x in wins]
    smax = max([d.get("frame_us", 0) for d in w["slow"]] + means + [0])
    s["frame"] = (sum(x["frame_us"] * x["n"] for x in wins) / n if n else 0, pct(means, 0.9), smax)
    for z in zones:
        zn, _, key = z.partition(".")
        vals = [(x["z"][zn], x["n"]) for x in wins if zn in x["z"]]
        if not vals:
            s["z"][z] = None
            continue
        if key:
            avg = sum(v.get(key, 0) * k for v, k in vals) / max(1, sum(k for _, k in vals))
            mx = max(v.get(key, 0) for v, _ in vals)
        else:
            avg = sum(v.get("avg", 0) * k for v, k in vals) / max(1, sum(k for _, k in vals))
            mx = max(v.get("max", 0) for v, _ in vals)
        s["z"][z] = (avg, mx)
    return s


def split_spec(arg):
    path, at, win = arg.partition("@")
    if not at:
        return path, None, None
    frm, colon, to = win.partition(":")
    if not colon or not frm or not to:
        raise SystemExit("table.py: window %r: want @FROM:TO" % win)
    return path, frm, to


def load(arg, zones):
    path, frm, to = split_spec(arg)
    if not os.path.isfile(path):
        raise SystemExit("table.py: no log %s" % path)
    w = parse(path, frm, to)
    if not w["wins"]:
        raise SystemExit("table.py: %s: no PROFILE roll-up (`prof frames=`) in the window "
                         "(not a PROFILE build, or the run ended before it)" % arg)
    return stats(w, zones), w


def ms(v):
    return "%.1f" % (v / 1000.0)


def render(cols, zones, title):
    out = []
    if title:
        out += ["", "#### " + title, ""]
    hdr = "| | " + " | ".join(c[0] for c in cols) + (" | delta |" if len(cols) == 2 else " |")
    out += [hdr, "|---" * (len(cols) + 1 + (len(cols) == 2)) + "|"]

    def row(name, cells, delta=None):
        out.append("| %s | %s%s |" % (name, " | ".join(cells), "" if delta is None else " | " + delta))

    two = len(cols) == 2
    f = [c[1]["frame"] for c in cols]
    row("frame avg / p90 / max ms", ["%s / %s / %s" % tuple(ms(v) for v in x) for x in f],
        ("%+.1f / %+.1f / %+.1f" % tuple((b - a) / 1000.0 for a, b in zip(*f))) if two else None)
    for key, name in (("frames", "frames"), ("over50", "frames > 50 ms"), ("slow", "slow logged"),
                      ("dom", "mon-dominant"), ("pushed", "AI-pushed")):
        v = [c[1][key] for c in cols]
        row(name, [str(x) for x in v], ("%+d" % (v[1] - v[0])) if two else None)
    for z in zones:
        v = [c[1]["z"].get(z) for c in cols]
        cells = ["-" if x is None else "%s / %s" % (ms(x[0]), ms(x[1])) for x in v]
        d = None
        if two:
            d = "-" if None in v else "%+.1f / %+.1f" % ((v[1][0] - v[0][0]) / 1000.0, (v[1][1] - v[0][1]) / 1000.0)
        row(z + " avg / max ms", cells, d)
    return "\n".join(out)


def summary(name, s, w):
    a, p, x = s["frame"]
    return ("scene %s frames=%d avg=%s p90=%s max=%s over50=%d slow=%d mon_dom=%d ai_pushed=%d%s" % (
        name, s["frames"], ms(a), ms(p), ms(x), s["over50"], s["slow"], s["dom"], s["pushed"],
        " (window not closed)" if w.get("open") else ""))


def selftest():
    d = tempfile.mkdtemp(prefix="scenetab.")
    p = os.path.join(d, "log")
    lines = ["prof frames=10 frame_us=99000 pace v1..v7+=0/0/0/10/0/0/0",	# boot
             "route: 3 SETPOS",
             "prof frames=30 frame_us=40000 pace v1..v7+=0/30/0/0/0/0/0", "prof mon_us avg=1000 max=2000",
             "route: 4 STATS begin",
             "prof frames=30 frame_us=40000 pace v1..v7+=0/28/0/2/0/0/0", "prof mon_us avg=4000 max=30000",
             "seamprof: slow frame=40 frame_us=60000 wait=100 mon=30000",
             "route: 5 SHOT a",
             "prof frames=10 frame_us=70000 pace v1..v7+=0/0/0/10/0/0/0", "prof mon_us avg=8000 max=9000",
             "seamprof: slow frame=50 frame_us=55000 wait=0 mon=10000",
             "route: 6 STATS end",
             "prof frames=30 frame_us=33000 pace v1..v7+=0/30/0/0/0/0/0", "prof mon_us avg=0 max=0"]
    with open(p, "w") as f:
        f.write("\n".join(lines) + "\n")
    bad = []
    s, w = load(p, ["mon"])
    if s["frames"] != 40 or s["over50"] != 12 or s["slow"] != 2 or s["dom"] != 1 or s["pushed"] != 2:
        bad.append("scene markers: %r" % {k: s[k] for k in ("frames", "over50", "slow", "dom", "pushed")})
    if abs(s["frame"][0] - 47500) > 1 or s["frame"][2] != 70000:
        bad.append("frame avg/max %r" % (s["frame"],))
    if abs(s["z"]["mon"][0] - 5000) > 1 or s["z"]["mon"][1] != 30000:
        bad.append("mon %r" % (s["z"]["mon"],))
    s2, _ = load(p + "@3:a", ["mon"])
    if s2["frames"] != 60 or s2["slow"] != 1:
        bad.append("op/label window: frames %d slow %d" % (s2["frames"], s2["slow"]))
    try:
        load(p + "@99:a", ["mon"])
        bad.append("missing FROM op accepted")
    except SystemExit:
        pass
    for b in bad:
        print("selftest FAIL " + b)
    print("table selftest %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--title")
    ap.add_argument("--zones", default=DEFAULT_ZONES)
    ap.add_argument("--label", action="append", default=[], help="column label per log")
    ap.add_argument("--summary", metavar="NAME")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not 1 <= len(a.logs) <= 2:
        ap.error("one or two logs")
    zones = [z for z in a.zones.split(",") if z]
    cols = []
    for i, arg in enumerate(a.logs):
        s, w = load(arg, zones)
        label = a.label[i] if i < len(a.label) else ("before" if len(a.logs) == 2 and i == 0 else
                                                      "after" if len(a.logs) == 2 else "run")
        cols.append((label, s, w))
    if a.summary:
        for label, s, w in cols:
            print(summary(a.summary if len(cols) == 1 else "%s[%s]" % (a.summary, label), s, w))
        return 0
    print(render(cols, zones, a.title))
    for label, s, w in cols:
        if w.get("open"):
            print("\n%s: the run ended inside the window" % label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
