#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_seamprof.py -- the seam-window profiler host test (test/run/m4.sh):
runs hl64-seamprof (test/m4/gate_seamprof.c: window logic on synthetic
frames, counter differencing, the engine hooks and `seamprof_mark`), needs
every case PASS, then parses its `seamprof:` lines with the gate's parser
(gate_parse.seamprof) and checks the blocks against what the cases fed:
every zone and region listed once per window, and the numbers of the
switch case (frames 40..160 of a switch at 100, zone value = frame).

Usage: gate_seamprof.py --bin BUILD/host/hl64-seamprof
"""
import argparse
import subprocess
import sys

import gate_parse as gp

ZONES = ["frame", "input", "io", "logic", "player", "active", "seam", "hydrate", "stream", "scene",
         "fxsim", "snd", "wait", "render", "hud", "submit", "test", "pmove", "ent", "phys",
         "studio", "fx", "game", "mon", "st_open", "st_scan", "st_name", "st_movers", "st_pins",
         "st_room", "world"]
# (windows in log order: case, n, frame, frames, mark, cut)
WINDOWS = [("switch", 1, 100, 121, False, False), ("spike", 1, 300, 239, False, False),
           ("early", 1, 5, 65, False, False), ("overlap", 1, 100, 111, False, True),
           ("overlap", 2, 130, 121, False, False), ("overlap", 3, 150, 121, False, False),
           ("overlap", 4, 300, 121, True, False), ("hooks", 1, 10, 70, True, False)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    a = ap.parse_args()
    p = subprocess.run([a.bin], capture_output=True, text=True)
    lines = p.stdout.splitlines()
    fails = []
    for l in lines:
        if l.startswith("case ") and not l.endswith(" PASS"):
            fails.append(l)
    if p.returncode != 0 or not any(l.startswith("seamprof PASS") for l in lines):
        fails.append("driver exit %d: %s" % (p.returncode, lines[-1] if lines else p.stderr[-200:]))
    blocks = gp.seamprof(lines)
    regions = None
    if len(blocks) != len(WINDOWS):
        fails.append("%d seamprof windows parsed, %d expected" % (len(blocks), len(WINDOWS)))
    for b, (case, n, frame, frames, mark, cut) in zip(blocks, WINDOWS):
        got = (b["n"], b["frame"], b["frames"], b["mark"], b["cut"])
        if got != (n, frame, frames, mark, cut):
            fails.append("%s window %d: n/frame/frames/mark/cut %s, want %s" % (
                case, n, got, (n, frame, frames, mark, cut)))
        if list(b["zones"]) != ZONES:
            fails.append("%s window %d: zones %s" % (case, n, list(b["zones"])))
        if regions is None:
            regions = list(b["regions"])
        if not regions or list(b["regions"]) != regions or any(
                r[2] <= 0 for r in b["regions"].values()):
            fails.append("%s window %d: regions %s" % (case, n, b["regions"]))
    if blocks:
        b = blocks[0]
        # zone value = frame: worst = the last frame of the window, mean and
        # the switch frame's own = the switch frame
        if b["zones"].get("render") != (160, 160, 100, 100):
            fails.append("switch window: render zone %s, want (160, 160, 100, 100)" % (
                b["zones"].get("render"),))
        if b["regions"].get("render", (0,))[:2] != (160 * 16, 160):
            fails.append("switch window: render region %s" % (b["regions"].get("render"),))
    for f in fails:
        print("  fail " + f)
    print("gate_seamprof %s: %d windows, %d zones, regions %s" % (
        "FAIL" if fails else "PASS", len(blocks), len(ZONES), ",".join(regions or [])))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
