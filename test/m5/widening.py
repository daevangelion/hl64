#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""widening.py -- the M5 gate's route report against the old tree's cfg
(docs/architecture.md 17 M5: "any WAIT_NEAR radius widened versus the old
route is listed with its GoldSrc-behaviour reason"; the M5 route rules).

From a route source (test/routes/NAME.route) and its oracle
(test/m5/oracle/NAME.txt, test/m5/checkpoints.py: the old cfg's
checkpoints and waitnear steps; the cfg -> route conversion of
docs/formats/route-bytecode.md keeps both verbs and operands):

  NAME-route-checkpoints  the route's checkpoint steps (waitmap waitmoved
              waitgone waitclass waitglobal waitend stats shot pass;
              timeouts aside, class names case-folded) equal the old cfg's,
              in order
  NAME-waitnear  each old waitnear is paired with the route's in the same
              checkpoint segment (same point first, then in order):
              - a wider radius needs `# widened: <old r> -> <new r>: <reason>`
                on the line or in the comment block right above it;
              - a moved point (> 0.5 u) needs an `# M5:` comment there;
              - an old waitnear the route dropped needs a comment
                `# M5: removed waitnear <x> <y> <z> <r>: <reason>` in that
                segment;
              narrower radii and added waitnear steps are listed only.
Every `# M5:` comment of the route is listed (the gate log's record of
route changes versus the old run).

Usage: widening.py --oracle FILE --route FILE [--name NAME]
       widening.py --selftest
Exit 0 when both checks pass; the last line names the first failure.
"""
import argparse
import contextlib
import difflib
import io
import math
import os
import re
import sys
import tempfile

import checkpoints as cpt

WIDENED = re.compile(r"widened:\s*([-+0-9.eE]+)\s*->\s*([-+0-9.eE]+)\s*:\s*\S")
REMOVED = re.compile(r"M5:\s*removed waitnear\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+"
                     r"([-+0-9.eE]+)\s*:\s*\S")


def route_steps(lines):
    """[(line no, code words, comment text)]: comment = the line's own
    comment plus the full-line comments right above it."""
    out, block = [], []
    for n, l in enumerate(lines, 1):
        code, _, com = l.partition("#")
        if not code.strip():
            block.append(com.strip() if com else "")
            if not com:
                block = []
            continue
        out.append((n, code.split(), " ".join(block + [com.strip()])))
        block = []
    return out


def m5_comments(lines):
    """[(line no, first line of the comment)] of every `# M5:` comment."""
    return [(n, l.strip()) for n, l in enumerate(lines, 1) if re.match(r"\s*#.*\bM5:", l)]


def report(oracle, route, name):
    o = cpt.load_oracle(oracle)
    src = cpt.read(route)
    steps = route_steps(src)
    res, out = [], []

    # checkpoints
    rcp = [c for c in (cpt.canon(" ".join(w)) for _, w, _ in steps) if c]
    ok, line = cpt.compare(name + "-route-checkpoints", o["cp"], rcp, "checkpoints")
    res.append((ok, line))

    # waitnear pairs by segment
    seg, new = 0, {}
    removed_notes = {}
    for n, w, com in steps:
        # a note above a checkpoint belongs to the segment it closes
        for m in REMOVED.finditer(com):
            removed_notes.setdefault(seg, []).append(tuple(float(x) for x in m.groups()))
        if cpt.canon(" ".join(w)):
            seg += 1
        elif w[0] == "waitnear":
            new.setdefault(seg, []).append((n, [float(x) for x in w[1:5]], float(w[5]), com))
    old = {}
    for t in o["waitnear"]:
        old.setdefault(t[0], []).append(([float(x) for x in t[1:5]], float(t[5]), t[6]))
    bad, widened, narrowed, moved, added, removed, same = [], [], [], [], [], [], 0
    for s in sorted(set(old) | set(new)):
        a, b = old.get(s, []), new.get(s, [])
        key = lambda p: "%.1f %.1f %.1f" % tuple(p[:3])
        sm = difflib.SequenceMatcher(a=[key(x[0]) for x in a], b=[key(x[1]) for x in b], autojunk=False)
        pairs = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            k = min(i2 - i1, j2 - j1)
            pairs += [(a[i1 + i], b[j1 + i]) for i in range(k)]
            removed += [(s, x) for x in a[i1 + k:i2]]
            added += [(s, y) for y in b[j1 + k:j2]]
        for (op, ot, ocl), (n, np_, nt, com) in pairs:
            dist = math.dist(op[:3], np_[:3])
            if np_[3] > op[3] + 1e-6:
                m = WIDENED.search(com)
                if m and float(m.group(1)) == op[3] and float(m.group(2)) == np_[3]:
                    widened.append("widened %s:%d (cfg line %d): r %g -> %g: %s" % (
                        os.path.basename(route), n, ocl, op[3], np_[3], com[m.start():]))
                else:
                    bad.append("%s:%d: radius %g -> %g without `# widened: %g -> %g: <reason>`" % (
                        os.path.basename(route), n, op[3], np_[3], op[3], np_[3]))
            elif np_[3] < op[3] - 1e-6:
                narrowed.append("narrowed %s:%d (cfg line %d): r %g -> %g" % (
                    os.path.basename(route), n, ocl, op[3], np_[3]))
            if dist > 0.5:
                if "M5:" in com:
                    moved.append("moved %s:%d (cfg line %d): %g %g %g -> %g %g %g (%.1f u)" % (
                        os.path.basename(route), n, ocl, *op[:3], *np_[:3], dist))
                else:
                    bad.append("%s:%d: waitnear point moved %.1f u from cfg line %d without an `# M5:` "
                               "comment" % (os.path.basename(route), n, dist, ocl))
            if dist <= 0.5 and abs(np_[3] - op[3]) <= 1e-6:
                same += 1
    for s, (op, ot, ocl) in removed:
        if any(math.dist(op[:3], r[:3]) <= 0.5 and abs(op[3] - r[3]) <= 1e-6
               for r in removed_notes.get(s, [])):
            out.append("removed (explained) cfg line %d: waitnear %g %g %g %g" % (ocl, *op))
        else:
            bad.append("cfg line %d: waitnear %g %g %g %g dropped without `# M5: removed waitnear "
                       "%g %g %g %g: <reason>`" % (ocl, *op, *op))
    for s, (n, np_, nt, com) in added:
        out.append("added %s:%d: waitnear %g %g %g %g" % (os.path.basename(route), n, *np_))
    total = sum(len(v) for v in old.values())
    res.append((not bad, "check %s-waitnear %s %d old waitnear: %d unchanged, %d widened (explained), "
                "%d narrowed, %d moved (M5), %d added%s" % (
                    name, "FAIL" if bad else "PASS", total, same, len(widened), len(narrowed), len(moved),
                    len(added), "; " + "; ".join(bad[:4]) if bad else "")))
    for ok, l in res:
        print(l)
    for l in widened + narrowed + moved + out:
        print("  " + l)
    for n, c in m5_comments(src):
        print("  M5 change %s:%d: %s" % (os.path.basename(route), n, c.lstrip("# ")[:150]))
    failed = [l.split()[1] for ok, l in res if not ok]
    print("widening %s (%d of %d failed)%s" % ("FAIL" if failed else "PASS", len(failed), len(res),
                                              "; first: " + failed[0] if failed else ""))
    return 1 if failed else 0


def selftest():
    d = tempfile.mkdtemp(prefix="m5wide.")
    orc = os.path.join(d, "oracle")
    with open(orc, "w") as f:
        f.write("cp waitmap a\nwaitnear 1 10 20 30 32 5 3\nwaitnear 1 40 50 60 16 5 4\ncp shot s\n"
                "waitnear 2 1 1 1 8 5 6\ncp pass\n")
    base = ["waitmap a 10", "setpos 0 0 0", "waitnear 10 20 30 32 5", "waitnear 40 50 60 16 5",
            "shot s", "waitnear 1 1 1 8 5", "pass"]
    bad = []

    def case(label, lines, want):
        p = os.path.join(d, "x.route")
        with open(p, "w") as f:
            f.write("\n".join(lines) + "\n")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = report(orc, p, "t")
        failed = sorted(l.split()[1] for l in buf.getvalue().splitlines()
                        if l.startswith("check ") and " FAIL " in l)
        if failed != sorted(want) or (code != 0) != bool(want):
            bad.append("%s: failed %s, want %s" % (label, failed, want))

    case("clean", base, [])
    case("widened, no reason", base[:2] + ["waitnear 10 20 30 48 5"] + base[3:], ["t-waitnear"])
    case("widened, wrong numbers", base[:2] + ["waitnear 10 20 30 48 5 # widened: 32 -> 40: x"] + base[3:],
         ["t-waitnear"])
    case("widened, explained", base[:2] + ["# widened: 32 -> 48: GoldSrc step height",
                                           "waitnear 10 20 30 48 5"] + base[3:], [])
    case("narrowed", base[:2] + ["waitnear 10 20 30 8 5"] + base[3:], [])
    case("moved, no comment", base[:3] + ["waitnear 40 50 90 16 5"] + base[4:], ["t-waitnear"])
    case("moved, M5", base[:3] + ["waitnear 40 50 90 16 5 # M5: 60 -> 90: GoldSrc lift top"] + base[4:], [])
    case("dropped", base[:3] + base[4:], ["t-waitnear"])
    case("dropped, explained", base[:3] + ["# M5: removed waitnear 40 50 60 16: merged"] + base[4:], [])
    case("added", base[:5] + ["waitnear 2 2 2 8 5"] + base[5:], [])
    case("checkpoint dropped", [l for l in base if not l.startswith("shot")], ["t-route-checkpoints",
                                                                               "t-waitnear"])
    for b in bad:
        print("selftest FAIL " + b)
    print("widening selftest %s (11 cases)" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--oracle")
    ap.add_argument("--route")
    ap.add_argument("--name", default="route")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    return report(a.oracle, a.route, a.name)


if __name__ == "__main__":
    sys.exit(main())
