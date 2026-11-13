#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_table.py -- the M4 gate's seam-window table from a PROFILE run's
log (test/run/m4.sh, ares): one column group per crossing, from the
seam-window profiler's lines (engine/test/seamprof.h).

CPU: per zone, the worst frame within +-2 s of the switch frame (us and
its frame), the mean over the window and the switch frame's own.
Memory: per carve region, the peak bytes in use in the window against
the carved size (the `seamprof: region` size), with the region rule of
docs/architecture.md 4.1: a peak above 90% of the region, or whose peak x
1.25 exceeds it, fails (components exempt: fixed-size by design, as in
test/m3/gate_table.py). The world pools render / texture / collision are
LRU caches that fill to their size: the rule takes the run's WANT peak
(`mem world: <pool>_want_peak=`, the open + pin class chunks wanted at
once; the `mem` command the PROFILE route's PASS runs with its last stats record, so it covers
every window), the window's resident peak is shown as information.

Old-tree baseline (doc only, not measured here): the old tree's cart
Uplink route (n64/docs/stream-arch-v2.md 6.8 "Cart Uplink route (ares,
PROFILE=1, Crossings tree, run f1)"), the same two crossings: crossing
work 12.8 ms (hldemo1->hldemo2) and 35.5 ms (hldemo2->hldemo3), switch
frames 44 ms and 322 ms; hldemo2->hldemo3 forced on the cart (host 0).

Usage: gate_table.py LOG --switch A:B:KIND [--switch ...]
       (prints Markdown; exit 1 when a crossing has no window or a region
       breaks the rule; the last line names what failed)
"""
import argparse
import re
import sys

import gate_parse as gp

USE_MAX = 0.90
HEADROOM = 1.25
EXEMPT = {"components"}
LRU = ("render", "texture", "collision")
# n64/docs/stream-arch-v2.md 6.8, by crossing: (crossing work ms, switch frame ms)
BASELINE = {("hldemo1", "hldemo2"): (12.8, 44), ("hldemo2", "hldemo3"): (35.5, 322)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--switch", action="append", default=[], metavar="A:B:KIND")
    a = ap.parse_args()
    lines = gp.read(a.log)
    want = [tuple(s.split(":")[:2]) for s in a.switch]
    wins = [w for w in gp.seamprof(lines) if not w["mark"]]
    cols = []                         # (title, window or None)
    at = 0
    for k, (wa, wb) in enumerate(want, 1):
        hit = None
        for i in range(at, len(wins)):
            if (wins[i]["a"], wins[i]["b"]) == (wa, wb):
                hit = i
                break
        cols.append(("%d: %s->%s" % (k, wa, wb), wins[hit] if hit is not None else None))
        if hit is not None:
            at = hit + 1
    missing = [t for t, w in cols if w is None]
    got = [(t, w) for t, w in cols if w is not None]
    out = ["", "### M4 gate: seam-window CPU per zone (ares PROFILE; frames within +-2 s of each "
           "switch frame; max = worst frame (its frame), avg = window mean, at = the switch frame)", ""]
    if got:
        out.append("| zone | " + " | ".join("%s max us (frame) | avg | at" % t for t, _ in got) + " |")
        out.append("|---|" + "---:|---:|---:|" * len(got))
        for z in got[0][1]["zones"]:
            row = []
            for _, w in got:
                mx, fr, avg, sw = w["zones"].get(z, (0, 0, 0, 0))
                row.append("%d (%d) | %d | %d" % (mx, fr, avg, sw))
            out.append("| %s | %s |" % (z, " | ".join(row)))
        out.append("")
        out.append("windows: " + "; ".join("%s switch frame %d, %d frames%s" % (
            t, w["frame"], w["frames"], ", cut by the next switch" if w["cut"] else "")
            for t, w in got))
    else:
        out.append("no seam-window lines (`seamprof: switch`) for the route's crossings")

    broken = []
    want_run = {}                     # LRU pool -> the run's want peak B
    for l in lines:
        if l.startswith("mem world: "):
            d = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", l)}
            for r in LRU:
                want_run[r] = max(want_run.get(r, 0), d.get(r + "_want_peak", 0))
    out += ["", "### M4 gate: memory per region at each crossing (peak in the +-2 s window vs the "
            "carved size; rule: use <= %d%% and peak x %.2f <= size, %s exempt)" % (
                USE_MAX * 100, HEADROOM, ", ".join(sorted(EXEMPT))), ""]
    if got:
        out.append("| region | size B | " + " | ".join("%s peak B (frame) | use | rule" % t
                                                      for t, _ in got) + " |")
        out.append("|---|---:|" + "---:|---:|---|" * len(got))
        for r in got[0][1]["regions"]:
            size = got[0][1]["regions"][r][2]
            row = []
            for t, w in got:
                peak, fr, _ = w["regions"].get(r, (0, 0, size))
                ruled = want_run[r] if r in want_run else peak
                why = []
                if ruled > USE_MAX * size:
                    why.append("use %.0f%% > %d%%" % (100.0 * ruled / size, USE_MAX * 100))
                if ruled * HEADROOM > size:
                    why.append("peak x %.2f = %d B > %d B" % (HEADROOM, ruled * HEADROOM, size))
                if r in EXEMPT:
                    verdict = "exempt"
                elif why:
                    verdict = "FAIL: " + "; ".join(why)
                    broken.append("%s at %s (%s)" % (r, t, "; ".join(why)))
                else:
                    verdict = "ok"
                if r in want_run:
                    row.append("want %d (run); resident %d (%d) | %.0f%% | %s" % (
                        ruled, peak, fr, 100.0 * ruled / size, verdict))
                else:
                    row.append("%d (%d) | %.0f%% | %s" % (peak, fr, 100.0 * peak / size, verdict))
            out.append("| %s | %d | %s |" % (r, size, " | ".join(row)))

    out += ["", "### Old tree (doc only: n64/docs/stream-arch-v2.md 6.8, cart Uplink route, ares "
            "PROFILE, run f1)", "",
            "| crossing | old crossing work ms | old switch frame ms | here: switch frame ms (seam zone ms) |",
            "|---|---:|---:|---:|"]
    for t, w in cols:
        ab = tuple(t.split(": ", 1)[1].split("->"))
        old = BASELINE.get(ab)
        here = "—"
        if w is not None:
            here = "%.1f (%.1f)" % (w["zones"].get("frame", (0, 0, 0, 0))[3] / 1000.0,
                                    w["zones"].get("seam", (0, 0, 0, 0))[3] / 1000.0)
        out.append("| %s | %s | %s | %s |" % (t, old[0] if old else "—", old[1] if old else "—", here))
    print("\n".join(out))
    if missing:
        print("\nno seam window for: " + ", ".join(missing))
    if broken:
        print("\nregion rule failed: " + ", ".join(broken))
    return 1 if missing or broken else 0


if __name__ == "__main__":
    sys.exit(main())
