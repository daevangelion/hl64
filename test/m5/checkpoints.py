#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""checkpoints.py -- the M5 gate's old-tree checkpoint oracle and the
route checks against it (docs/architecture.md 17 M5: "PASS on host and
ares at the same checkpoints").

The old tree (~/src/ports/quakegeneric) is a black-box oracle: its route
files (n64/port/test/uplink_route.cfg, the generated Hazard walk
out/hl-walk-t0a0_.cfg; audit CLEAN data) and the logs of its runs. Its code
is never read.

Checkpoints are the route steps the old route passed, in order: waitmap,
waitmoved, waitgone, waitclass, waitglobal, waitend, stats, shot, pass
(timeouts left out: `<verb> <map|name|label> [class x y z r | state]`,
class names lower case). Events are the map switches (`switch A B`), the
end section (`end <section>`) and the New Game after it (`restart <map>`).
The cfg's waitnear steps are kept too (`waitnear <seg> <x> <y> <z> <r>
<timeout> <cfg line>`, seg = the number of checkpoints before it) for
test/m5/widening.py.

  oracle  --cfg OLD.cfg --log OLD.log --name NAME [--note TEXT] -o FILE
          writes the oracle data file (test/m5/oracle/NAME.txt) from an old
          run that passed (`route PASS`)
  verify  --cfg OLD.cfg --log OLD.log --oracle FILE
          the committed file still equals what the old run gives (skip
          with exit 0 when the old files are absent)
  check   --oracle FILE --log RUN.log --dump RUN.dump --route SRC.route
          [--name NAME]: one hl64 run (host or ares) against the oracle:
          `check NAME-checkpoints|NAME-switches|NAME-end PASS|FAIL ...`
          (RUN.dump: `routec --dump` of the run's .rbc, op -> source line)
  selftest  positive and negative controls on synthetic logs

Exit 0 when every check passes; the last line names the first failure.
"""
import argparse
import hashlib
import os
import re
import sys
import tempfile

CP_VERBS = ("waitmap", "waitmoved", "waitgone", "waitclass", "waitglobal", "waitend",
            "stats", "shot", "pass")


def num(s):
    return "%g" % float(s)


def canon(text):
    """The checkpoint form of a route / cfg step, None for other steps."""
    w = text.split("#", 1)[0].split()
    if not w or w[0] not in CP_VERBS:
        return None
    v = w[0]
    if v in ("waitend", "pass"):
        return v
    if v == "waitclass":
        return "waitclass %s %s %s %s %s" % (w[1].lower(), num(w[2]), num(w[3]), num(w[4]), num(w[5]))
    if v == "waitglobal":
        return "waitglobal %s %s" % (w[1], w[3] if len(w) > 3 else "on")
    return "%s %s" % (v, w[1])


def read(path):
    with open(path, errors="replace") as f:
        return [l.rstrip("\r\n") for l in f]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# ---- the old tree ------------------------------------------------------------------

OLD_STEP = re.compile(r"^route: \d+: (.*)$")
OLD_SWITCH = re.compile(r"^q64world: SWITCH (\S+) -> (\S+) ")
OLD_PORTAL = re.compile(r"^q64world: SEAMLESS portal (\S+) \u2192 (\S+) ")
OLD_END = re.compile(r"^trigger_endsection: (\S+)")
OLD_LEVEL = re.compile(r"^level: (\S+) ")


def old_events(lines):
    """[(kind, text)] in log order: cp / switch / end / restart; and the
    result line."""
    # the parity runner logs `SWITCH` per layer switch; the older walk
    # logs only `SEAMLESS portal`
    sw = OLD_SWITCH if any(OLD_SWITCH.match(l) for l in lines) else OLD_PORTAL
    out, ended, result = [], False, None
    for l in lines:
        m = OLD_STEP.match(l)
        if m:
            c = canon(m.group(1))
            if c:
                out.append(("cp", c))
            continue
        m = sw.match(l)
        if m:
            out.append(("switch", "%s %s" % (m.group(1), m.group(2))))
            continue
        m = OLD_END.match(l)
        if m:
            out.append(("end", m.group(1)))
            ended = True
            continue
        m = OLD_LEVEL.match(l)
        if m and ended:
            out.append(("restart", m.group(1)))
            ended = False
            continue
        if l.startswith("route PASS") or l.startswith("route FAIL"):
            result = l
    return out, result


def cfg_nears(lines):
    """[(seg, x, y, z, r, timeout, line)] of the cfg's waitnear steps."""
    out, seg = [], 0
    for n, l in enumerate(lines, 1):
        w = l.split("#", 1)[0].split()
        if not w:
            continue
        if canon(l):
            seg += 1
        elif w[0] == "waitnear":
            out.append((seg, num(w[1]), num(w[2]), num(w[3]), num(w[4]), num(w[5]), n))
    return out


def make_oracle(cfg, log, name, note):
    ev, result = old_events(read(log))
    if not result or not result.startswith("route PASS"):
        raise SystemExit("%s: the old run did not pass (%s)" % (log, result))
    cfgl = read(cfg)
    cps = [c for c in (canon(l) for l in cfgl) if c]
    run = [t for k, t in ev if k == "cp"]
    if run != cps:
        raise SystemExit("%s: the run's checkpoints differ from %s's: the log is not of that cfg"
                         % (log, cfg))
    out = ["# hl64 M5 checkpoint oracle: %s (test/m5/checkpoints.py oracle; old tree, black box)" % name,
           "# cfg: %s sha256 %s" % (cfg, sha256(cfg)),
           "# log: %s (%s)" % (log, result)]
    if note:
        out.append("# " + note)
    out += ["%s %s" % kv for kv in ev]
    out += ["waitnear %d %s %s %s %s %s %d" % t for t in cfg_nears(cfgl)]
    return out


def load_oracle(path):
    o = {"cp": [], "switch": [], "end": [], "restart": [], "waitnear": [], "seq": []}
    for l in read(path):
        if not l or l.startswith("#"):
            continue
        k, _, t = l.partition(" ")
        if k == "waitnear":
            w = t.split()
            o["waitnear"].append((int(w[0]), w[1], w[2], w[3], w[4], w[5], int(w[6])))
            continue
        o[k].append(t)
        o["seq"].append((k, t))
    return o


# ---- an hl64 run -------------------------------------------------------------------

DUMP = re.compile(r"^\s*(\d+)\s+(\d+)\s+\S")
RUN_OP = re.compile(r"^route: (\d+) [A-Z_]+")
RUN_SWITCH = re.compile(r"^seam: switch (\S+)->(\S+) ")
RUN_END = re.compile(r"^endsection: (\S+)")
RUN_NEW = re.compile(r"^rec: new campaign (\S+):")


def run_events(lines, dump, src):
    """[(kind, text)] of an hl64 run in log order, and its result line."""
    op_line = {}
    for l in dump:
        m = DUMP.match(l)
        if m:
            op_line[int(m.group(1))] = int(m.group(2))
    out, ended, result = [], False, None
    for l in lines:
        m = RUN_OP.match(l)
        if m:
            n = op_line.get(int(m.group(1)))
            c = canon(src[n - 1]) if n and n <= len(src) else None
            if c:
                out.append(("cp", c))
            continue
        m = RUN_SWITCH.match(l)
        if m:
            out.append(("switch", "%s %s" % (m.group(1), m.group(2))))
            continue
        m = RUN_END.match(l)
        if m:
            out.append(("end", m.group(1)))
            ended = True
            continue
        m = RUN_NEW.match(l)
        if m and ended:
            out.append(("restart", m.group(1)))
            ended = False
            continue
        if l == "route PASS" or l.startswith("route FAIL"):
            result = l
    return out, result


def compare(name, want, got, what):
    """One check line: the run's sequence equals the oracle's."""
    n = 0
    while n < min(len(want), len(got)) and want[n] == got[n]:
        n += 1
    if n == len(want) == len(got):
        return True, "check %s PASS %d %s in the old order" % (name, n, what)
    if n == len(got):
        return False, "check %s FAIL reached %d of %d %s; next expected: %s" % (
            name, n, len(want), what, want[n])
    if n == len(want):
        return False, "check %s FAIL %d %s as the old run, then %d more: %s" % (
            name, n, what, len(got) - n, got[n])
    return False, "check %s FAIL %s %d: old %r, here %r" % (name, what, n + 1, want[n], got[n])


def check(oracle, log, dump, route, name):
    o = load_oracle(oracle)
    ev, result = run_events(read(log), read(dump), read(route))
    res = []
    res.append(compare(name + "-checkpoints", o["cp"], [t for k, t in ev if k == "cp"], "checkpoints"))
    res.append(compare(name + "-switches", o["switch"], [t for k, t in ev if k == "switch"],
                       "map switches"))
    if o["end"] or o["restart"]:
        want = [(k, t) for k, t in o["seq"] if k in ("end", "restart")]
        got = [(k, t) for k, t in ev if k in ("end", "restart")]
        res.append(compare(name + "-end", ["%s %s" % kt for kt in want], ["%s %s" % kt for kt in got],
                           "end-section events"))
    for ok, line in res:
        print(line)
    print("run result: %s" % (result or "no route result line"))
    failed = [l.split()[1] for ok, l in res if not ok]
    print("checkpoints %s (%d of %d failed)%s" % ("FAIL" if failed else "PASS", len(failed), len(res),
                                                 "; first: " + failed[0] if failed else ""))
    return 1 if failed else 0


# ---- self test ---------------------------------------------------------------------

def selftest():
    d = tempfile.mkdtemp(prefix="m5cp.")
    cfg = ["waitmap a 10", "shot s0", "waitnear 1 2 3 32 5", "waitmoved door 5",
           "waitclass weapon_9mmar 1 2 3 16 5", "waitmap b 10", "waitglobal g 5", "waitend 5", "pass"]
    old = ["route: 1: waitmap a 10", "route: 2: shot s0", "route: 3: waitnear 1 2 3 32 5",
           "route: 4: waitmoved door 5", "route: 5: waitclass weapon_9mmar 1 2 3 16 5",
           "route: 6: waitmap b 10", "q64world: SWITCH a -> b seam 0 portal 0 0 0 twins 0",
           "route: 7: waitglobal g 5", "route: 8: waitend 5", "trigger_endsection: _end (end of demo)",
           "level: a \u2014 \"X\"", "route: 9: pass", "route PASS (1.0 s)"]
    src = ["startmap a"] + ["%s # c" % l if "weapon" in l else l
                            for l in [c.replace("weapon_9mmar", "weapon_9mmAR") for c in cfg]]
    dump = ["%5d %5d X" % (i, i + 1) for i in range(len(src))]
    run = (["route: 0 START", "route: 1 WAIT_MAP", "route: 2 SHOT s0", "route: 3 WAIT_NEAR",
            "route: 4 WAIT_MOVED", "route: 5 WAIT_CLASS", "route: 6 WAIT_MAP",
            "seam: switch a->b landmark l (touch) frame=9 twins=0 carried=0",
            "route: 7 WAIT_GLOBAL", "route: 8 WAIT_END", "endsection: _end",
            "rec: new campaign a: 1 spawned", "route: 9 PASS", "route PASS"])
    paths = {}
    for k, v in (("cfg", cfg), ("old", old), ("src", src), ("dump", dump)):
        paths[k] = os.path.join(d, k)
        with open(paths[k], "w") as f:
            f.write("\n".join(v) + "\n")
    orc = os.path.join(d, "oracle")
    with open(orc, "w") as f:
        f.write("\n".join(make_oracle(paths["cfg"], paths["old"], "t", "")) + "\n")
    o = load_oracle(orc)
    bad = []
    if o["waitnear"] != [(2, "1", "2", "3", "32", "5", 3)]:
        bad.append("waitnear row %s" % o["waitnear"])

    def case(label, lines, want_fail):
        p = os.path.join(d, "run")
        with open(p, "w") as f:
            f.write("\n".join(lines) + "\n")
        import io
        import contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = check(orc, p, paths["dump"], paths["src"], "t")
        failed = sorted(l.split()[1] for l in buf.getvalue().splitlines()
                        if l.startswith("check ") and " FAIL " in l)
        if failed != sorted(want_fail) or (code != 0) != bool(want_fail):
            bad.append("%s: failed %s, want %s" % (label, failed, want_fail))

    case("clean", run, [])
    case("missing checkpoint", [l for l in run if l != "route: 4 WAIT_MOVED"], ["t-checkpoints"])
    case("stopped early", run[:6] + ["route FAIL 5 src:6 WAIT_CLASS timeout"], ["t-checkpoints",
                                                                                "t-switches", "t-end"])
    case("extra switch", run[:8] + ["seam: switch b->a landmark l (touch) frame=10"] + run[8:],
         ["t-switches"])
    case("no restart", [l for l in run if not l.startswith("rec: new campaign")], ["t-end"])
    case("wrong section", [l.replace("_end", "_other") for l in run], ["t-end"])
    try:
        with open(paths["old"] + "f", "w") as f:
            f.write("\n".join(old[:-1] + ["route FAIL 3"]) + "\n")
        make_oracle(paths["cfg"], paths["old"] + "f", "t", "")
        bad.append("an old run that failed made an oracle")
    except SystemExit:
        pass
    for b in bad:
        print("selftest FAIL " + b)
    print("checkpoints selftest %s (%d cases)" % ("FAIL" if bad else "PASS", 6))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=("oracle", "verify", "check", "selftest"))
    ap.add_argument("--cfg")
    ap.add_argument("--log")
    ap.add_argument("--name", default="route")
    ap.add_argument("--note", default="")
    ap.add_argument("--oracle")
    ap.add_argument("--dump")
    ap.add_argument("--route")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.mode == "selftest":
        return selftest()
    if a.mode == "oracle":
        lines = make_oracle(a.cfg, a.log, a.name, a.note)
        with open(a.out, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("oracle %s: %d checkpoints, %d switches, %d waitnear" % (
            a.out, sum(1 for l in lines if l.startswith("cp ")),
            sum(1 for l in lines if l.startswith("switch ")),
            sum(1 for l in lines if l.startswith("waitnear "))))
        return 0
    if a.mode == "verify":
        if not (a.cfg and a.log and os.path.exists(a.cfg) and os.path.exists(a.log)):
            print("verify %s: old run files not on this machine (skipped)" % a.oracle)
            return 0
        want = [l for l in read(a.oracle) if not l.startswith("#")]
        got = [l for l in make_oracle(a.cfg, a.log, a.name, "") if not l.startswith("#")]
        if want != got:
            print("verify %s FAIL: the old run gives a different oracle" % a.oracle)
            return 1
        print("verify %s PASS (%d lines)" % (a.oracle, len(want)))
        return 0
    return check(a.oracle, a.log, a.dump, a.route, a.name)


if __name__ == "__main__":
    sys.exit(main())
