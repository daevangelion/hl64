#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_selftest.py -- positive and negative controls of the M4 gate's
log checks and tables (test/m4/gate_check.py, test/m4/gate_table.py) on a
synthetic log in the contract's formats: the clean log must PASS, and each
planted fault must fail exactly the check that owns it (a forced switch on
the host, a missing switch, an extra switch, a wrong kind, over=1,
open_fail, blk1ms, popin outside a teleport window, a continuity miss, a
missing / insane rec line, no stats record after a crossing, route FAIL);
popin_tele and an ares forced switch (--forced-ok) must not fail. The
table must fail a region over 90% and pass components at 100%.

Usage: gate_selftest.py --scratch DIR
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SW = ["--switch", "hldemo1:hldemo2:use", "--switch", "hldemo2:hldemo3:touch"]
ZONES = ["frame", "seam", "render"]
REGIONS = [("render", 245760), ("components", 137216)]


def stats(n, frame, seam, rec="live=40 pending=3 dry=0 released=0 store=0/0 expired=0",
          stream="popin=0 popin_tele=0 blk1ms=0 pins=0 prefetch=0 recompute=1 reassert=0 late=0/0",
          over=0):
    return ["stats %d frame=%d t=1 heap=1/1KB live=1KB peak=1KB fails=0 ioq=0 blkmax=0us over=%d "
            "fpu=0 stack=-1 pace" % (n, frame, over),
            "stats %d player: map=x org=0,0,0 vel=0,0,0 flags=0 hull=0 ground=0 water=0" % n,
            "stats %d ents: live=1 hash=00000000" % n,
            "stats %d seam: %s" % (n, seam),
            "stats %d stream: %s" % (n, stream),
            "stats %d rec: %s" % (n, rec)]


def window(n, a, b, frame, render_peak=100000, comp=137216):
    out = ["seamprof: switch %d %s->%s frame=%d frames=121" % (n, a, b, frame)]
    out += ["seamprof: zone=%s max_us=900 frame=%d avg_us=300 switch_us=800" % (z, frame) for z in ZONES]
    out += ["seamprof: region=render peak=%d frame=%d size=245760" % (render_peak, frame),
            "seamprof: region=components peak=%d frame=%d size=137216" % (comp, frame)]
    return out


def clean():
    s0 = "switches=0 forced=0 open_fail=0 late=0 twins=0 carried=0 continuity_bad=0 closes=0"
    s1 = "switches=1 forced=0 open_fail=0 late=0 twins=4 carried=1 continuity_bad=0 closes=0"
    s2 = "switches=2 forced=0 open_fail=0 late=0 twins=4 carried=1 continuity_bad=0 closes=1"
    return (["route: 0 WAIT_MAP"] + stats(1, 100, s0)
            + ["seam: approach hldemo1->hldemo2", "seam: open hldemo2 layer=1",
               "seam: switch hldemo1->hldemo2 landmark hldemo1to2 (use) frame=500",
               "seam: continuity carry=2 matched=2 missing=0 extra=0"]
            + stats(2, 600, s1) + window(1, "hldemo1", "hldemo2", 500)
            + stats(3, 900, s1)
            + ["seam: switch hldemo2->hldemo3 landmark hldemo2_3 (touch) frame=1000",
               "seam: continuity carry=1 matched=1 missing=0 extra=0", "seam: close hldemo1"]
            + stats(4, 1100, s2) + window(2, "hldemo2", "hldemo3", 1000)
            + ["route PASS"])


def run(tool, path, *args):
    p = subprocess.run([sys.executable, os.path.join(HERE, tool), path] + list(args),
                       capture_output=True, text=True)
    return p.returncode, p.stdout


def failed_checks(out):
    return {l.split()[1] for l in out.splitlines() if l.startswith("check ") and " FAIL" in l}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--scratch", required=True)
    a = ap.parse_args()
    os.makedirs(a.scratch, exist_ok=True)
    fails = []
    n = 0

    def case(name, lines, want, *extra):
        nonlocal n
        n += 1
        path = os.path.join(a.scratch, "selftest-%s.log" % name)
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        code, out = run("gate_check.py", path, *(SW + list(extra)))
        got = failed_checks(out)
        if got != set(want) or (code != 0) != bool(want):
            fails.append("%s: failed %s (exit %d), want %s" % (name, sorted(got), code, sorted(want)))

    def sub(lines, old, new):
        out = [l.replace(old, new) for l in lines]
        if out == lines:
            raise SystemExit("selftest bug: %r not in the clean log" % old)
        return out

    c = clean()
    case("clean", c, [])
    case("clean-ares", c, [], "--forced-ok")
    forced = sub(sub(c, "(touch) frame=1000", "(forced: timeout 2.0 s) frame=1000"),
                 "switches=2 forced=0", "switches=2 forced=1")
    case("forced-host", forced, ["forced", "switch-2"])
    case("forced-ares", forced, [], "--forced-ok")
    case("no-switch", [l for l in c if not l.startswith("seam: switch hldemo2->")],
         ["switch-2", "stats-2", "continuity"])
    case("no-crossing", [l for l in c if not l.startswith("seam: ")],
         ["switch-1", "switch-2", "stats-1", "stats-2", "continuity"])
    case("extra-switch", c[:-1] + ["seam: switch hldemo3->hldemo2 landmark hldemo2_3 (touch) frame=1200",
                                   "seam: continuity carry=0 matched=0 missing=0 extra=0", "route PASS"],
         ["switches"])
    case("wrong-kind", sub(c, "(use) frame=500", "(touch) frame=500"), ["switch-1"])
    case("over", sub(c, "over=0 fpu=0 stack=-1 pace", "over=1 fpu=0 stack=-1 pace"), ["over"])
    case("open-fail-line", c[:3] + ["seam: open_fail hldemo2 (no layer)"] + c[3:], ["open-fail"])
    case("open-fail-stat", sub(c, "open_fail=0 late=0 twins=4 carried=1 continuity_bad=0 closes=1",
                               "open_fail=1 late=0 twins=4 carried=1 continuity_bad=0 closes=1"),
         ["open-fail"])
    case("blk1ms", sub(c, "blk1ms=0", "blk1ms=2"), ["blk1ms"])
    case("io-late", sub(c, "late=0/0", "late=1/3"), ["blk1ms"])
    case("rec-expired", sub(c, "expired=0", "expired=1"), ["rec"])
    case("popin", sub(c, "popin=0 popin_tele=0", "popin=3 popin_tele=0"), ["popin"])
    case("popin-tele", sub(c, "popin=0 popin_tele=0", "popin=0 popin_tele=5"), [])
    case("continuity", sub(c, "carry=1 matched=1 missing=0 extra=0", "carry=1 matched=0 missing=1 extra=0"),
         ["continuity"])
    case("rec-missing", [l for l in c if not l.startswith("stats 3 rec:")], ["rec"])
    case("rec-store", sub(c, "store=0/0", "store=70000/70000"), ["rec"])
    case("no-stats-after", [l for l in c if not l.startswith("stats 4 ")],
         ["stats-2"])
    case("route-fail", c[:-1] + ["route FAIL 3 m4.route:52 WAIT_MAP timeout"], ["route"])
    case("no-seam-stats", [l for l in c if " seam: " not in l], ["forced", "open-fail"])
    case("no-stream-stats", [l for l in c if " stream: " not in l], ["blk1ms", "popin"])

    # tables: pass with components at 100%; a region over 90% fails; a
    # crossing without a window fails
    def table(name, lines, want_fail):
        nonlocal n
        n += 1
        path = os.path.join(a.scratch, "selftest-table-%s.log" % name)
        with open(path, "w") as f:
            f.write("\n".join(lines) + "\n")
        code, out = run("gate_table.py", path, *SW)
        if (code != 0) != want_fail or "| render |" not in out and "no seam-window" not in out:
            fails.append("table %s: exit %d, want %s\n%s" % (name, code, "fail" if want_fail else "pass",
                                                            out[-400:]))

    table("clean", c, False)
    table("render-full", sub(c, "region=render peak=100000", "region=render peak=230000"), True)
    table("no-window", [l for l in c if not l.startswith("seamprof: switch 2 ")
                        and not (l.startswith("seamprof:") and "frame=1000" in l)], True)

    for f in fails:
        print("  fail " + f)
    print("gate_selftest %s: %d cases, %d failed" % ("FAIL" if fails else "PASS", n, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
