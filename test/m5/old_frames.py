#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""old_frames.py -- the old tree's per-map frame numbers on the cart
(ares), from the log of one of its Uplink playthroughs (black box: the
log only), for the M5 CPU table (test/m5/cpu_table.py prints them beside
hl64's).

The old cart log has no per-zone roll-ups; it has
  `quake64: <fps> fps avg, ...`   one line per pacing window
  `route stats <label>: ... pace vbl=N fps=F v1..v7+=a/b/c/d/e/f/g ...`
                                  frames by vblanks taken since the last
                                  stats line (NTSC: 1 vblank 16.7 ms, the
                                  game presents every 2; >= 4 vblanks is a
                                  frame over 50 ms)
  `q64world: SWITCH A -> B ...`   the map switches.
Per map visit (the start map, then one per switch): the fps windows
(frame ms = 1000 / fps): count, mean, p90 and the worst window. Per stats
line: its window's vblank histogram and frames over 50 ms, with the map
at the line (a window can span a switch).

Usage: old_frames.py LOG --source TEXT -o FILE
Output lines: `visit <k> <map> windows=W avg_ms=A p90_ms=P worst_ms=M`
              `pace <label> <map> frames=F v1..v7+=... over50=X`
"""
import argparse
import re
import sys

FPS = re.compile(r"^quake64: ([0-9.]+) fps avg")
SWITCH = re.compile(r"^q64world: SWITCH (\S+) -> (\S+) ")
STATS = re.compile(r"^route stats ([^:]+): map=(\S+) .* v1\.\.v7\+=([0-9/]+)")
START = re.compile(r"^route: \d+: waitmap (\S+) ")


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--source", required=True)
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    visits, cur, pace = [], None, []
    for l in open(a.log, errors="replace"):
        l = l.rstrip("\r\n")
        if cur is None:
            m = START.match(l)
            if m:
                cur = [m.group(1), []]
                visits.append(cur)
            continue
        m = SWITCH.match(l)
        if m:
            cur = [m.group(2), []]
            visits.append(cur)
            continue
        m = FPS.match(l)
        if m and float(m.group(1)) > 0:
            cur[1].append(1000.0 / float(m.group(1)))
            continue
        m = STATS.match(l)
        if m:
            v = [int(x) for x in m.group(3).split("/")]
            pace.append("pace %s %s frames=%d v1..v7+=%s over50=%d" % (
                m.group(1).replace(" ", "_"), m.group(2), sum(v), m.group(3), sum(v[3:])))
    out = ["# old tree cart (ares) frame numbers per Uplink map visit: test/m5/old_frames.py",
           "# source: " + a.source]
    for k, (mp, ms) in enumerate(visits, 1):
        if ms:
            out.append("visit %d %s windows=%d avg_ms=%.1f p90_ms=%.1f worst_ms=%.1f" % (
                k, mp, len(ms), sum(ms) / len(ms), pct(ms, 0.9), max(ms)))
        else:
            out.append("visit %d %s windows=0" % (k, mp))
    out += pace
    with open(a.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
