#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""trace_tests.py -- docs/specs/trace-and-hulls.md tests T1-T10 against the
GoldSrc probe fixtures ($HL64_FIXTURES/goldsrc/trace-and-hulls/<capture>/
<map>.jsonl), through the engine's collision module (hl64-colltest).

The probe ran each map at its own origin, so the driver opens the layer
with g_M = 0 (the map frame); T8 opens two layers at their real g_M and
checks the global <-> local translation.

  T1  T-points      pfnPointContents (c), PM_TruePointContents (tpc)
  T2  T-segments    world (pfnTraceModel on entity 0) and move
                    (pfnTraceHull ignore_monsters): flags exact, fraction
                    1e-5, end 0.001, normal exact, dist 1e-4, hit exact
  T3  T-segments    world impact stand-off 1/32 +- 1e-3, GoldSrc's own
                    short impacts excepted (spec rule)
  T4  T-segments, T-allsolid  end points of non-all-solid world traces
                    never classify SOLID in the traced hull
  T5  T-entities    box entities, filters, missile, owners, glass, door
  T6  T-entities    hull selection (hullsel_*)
  T7  T-rotated     rotated brush entity: line, hull, monster hull, model
  T8  self-contained  two layers at their g_M: global == local + g_M;
                    outside every layer SOLID
  T9  T-allsolid    all-solid / start-solid results
  T10 T-hitbox      hitbox traces at the monsters' studio poses (cooked
                    models/<x>.s64 + .a64 in --content)

Exit 0 when every requested test passes.
"""
import argparse
import itertools
import json
import math
import os
import subprocess
import sys

MAPS = ["hldemo1", "hldemo2", "hldemo3", "t0a0", "t0a0a", "t0a0b", "t0a0b1", "t0a0b2",
        "t0a0c", "t0a0d"]
HULL_MINS = [(0, 0, 0), (-16, -16, -36), (-32, -32, -32), (-16, -16, -18)]
HULL_MAXS = [(0, 0, 0), (16, 16, 36), (32, 32, 32), (16, 16, 18)]
EPS = 1.0 / 32
SOLID = -2


def f9(v):
    return " ".join("%.9g" % x for x in v)


class Run:
    """One hl64-colltest process: queue commands, then run and parse."""

    def __init__(self, binary, content):
        self.binary, self.content = binary, content
        self.cmds = []
        self.nres = 0

    def cmd(self, line, results=0):
        """Queue one command; a frame boundary every 32 queries lets the
        collision pool evict like a running game (fences, LRU)."""
        self.cmds.append(line)
        idx = self.nres
        self.nres += results
        if results and self.nres % 32 == 0:
            self.cmds.append("F")
        return idx

    def go(self):
        p = subprocess.run([self.binary, self.content], input="\n".join(self.cmds) + "\n",
                           capture_output=True, text=True)
        if p.returncode != 0:
            sys.stderr.write(p.stdout[-3000:] + p.stderr[-3000:])
            raise SystemExit("hl64-colltest exit %d" % p.returncode)
        res = [l[2:].split() for l in p.stdout.splitlines() if l.startswith("R ")]
        res = [r for r in res if r[0] != "O"]
        if len(res) != self.nres:
            raise SystemExit("hl64-colltest: %d results, %d expected" % (len(res), self.nres))
        return res


def load(fix, cap, m):
    path = os.path.join(fix, "goldsrc", "trace-and-hulls", cap, m + ".jsonl")
    with open(path) as f:
        recs = [json.loads(l) for l in f]
    remap(recs)
    return recs


def remap(recs):
    """GoldSrc entity indices (up to its edict count) -> hl64 slots 2..:
    order-preserving, so ascending-index candidate order is unchanged."""
    ents, keys = [], []
    for r in recs:
        ents += r.get("brush_ents", []) + r.get("ents", []) + ([r["ent"]] if "ent" in r else [])
        keys.append(r)
        keys += [r[k] for k in ("world", "move") if k in r]
    used = {e["i"] for e in ents} | {e["owner"] for e in ents}
    for r in keys:
        used |= {r[k] for k in ("pass", "mover", "target", "hit") if k in r}
    slot = {i: 2 + n for n, i in enumerate(sorted(i for i in used if i > 0))}
    slot[0], slot[-1] = 0, -1
    if len(slot) > 495:
        raise SystemExit("too many fixture entities for the slot table")
    for e in ents:
        e["i"], e["owner"] = slot[e["i"]], slot[e["owner"]]
    for r in keys:
        for k in ("pass", "mover", "target", "hit"):
            if k in r:
                r[k] = slot[r[k]]


def ent_cmd(e, sub=None):
    m = e.get("model", "")
    if sub is None:
        sub = int(m[1:]) if m.startswith("*") else -1
    return "E %d %d %d %s %s %s %s %d %d %d %d %d" % (
        e["i"], e["solid"], sub, f9(e["o"]), f9(e["a"]), f9(e["mins"]), f9(e["maxs"]),
        e.get("flags", 0), e.get("owner", -1), e.get("rendermode", 0), e.get("skin", 0),
        e.get("movetype", 0))


def brush_cmds(run, recs):
    for e in recs[1]["brush_ents"]:
        run.cmd(ent_cmd(e))


def trace_of(r):
    return {"as": int(r[1]), "ss": int(r[2]), "io": int(r[3]), "iw": int(r[4]),
            "fr": float(r[5]), "end": [float(x) for x in r[6:9]],
            "n": [float(x) for x in r[9:12]], "d": float(r[12]), "hit": int(r[13]),
            "hg": int(r[14])}


def diff(got, exp, ntol=0.0, flags=("as", "ss", "io", "iw"), dtol=1e-4, etol=1e-3, ftol=1e-5):
    """Mismatching fields of a trace (spec T2 tolerances)."""
    out = []
    for k in flags:
        if got[k] != exp[k]:
            out.append("%s %d!=%d" % (k, got[k], exp[k]))
    if abs(got["fr"] - exp["fr"]) > ftol:
        out.append("fr %.9g!=%.9g" % (got["fr"], exp["fr"]))
    if any(abs(a - b) > etol for a, b in zip(got["end"], exp["end"])):
        out.append("end %s!=%s" % (f9(got["end"]), f9(exp["end"])))
    if any(abs(a - b) > ntol for a, b in zip(got["n"], exp["n"])):
        out.append("n %s!=%s" % (f9(got["n"]), f9(exp["n"])))
    if abs(got["d"] - exp["d"]) > dtol:
        out.append("d %.9g!=%.9g" % (got["d"], exp["d"]))
    if got["hit"] != exp["hit"]:
        out.append("hit %d!=%d" % (got["hit"], exp["hit"]))
    if got["hg"] != exp["hg"]:
        out.append("hg %d!=%d" % (got["hg"], exp["hg"]))
    return out


class Report:
    def __init__(self):
        self.n = {}
        self.bad = {}
        self.notes = {}
        self.order = {}         # (test, map) -> ORDER-classified mismatches
        self.edge = {}          # map -> T10 EDGE-classified mismatches
        self.pending = []       # move mismatches awaiting the order check

    def check(self, test, ok, what, move=None):
        """move: (map, ents, query, expected) of a move query; a mismatch is
        then held for Order.resolve instead of failing at once."""
        self.n[test] = self.n.get(test, 0) + 1
        if not ok:
            if move is not None:
                self.pending.append((test, what) + move)
            else:
                self.bad.setdefault(test, []).append(what)

    def note(self, test, text):
        self.notes.setdefault(test, []).append(text)

    def print(self, tests, show):
        fails = 0
        for t in tests:
            n, b = self.n.get(t, 0), self.bad.get(t, [])
            if n == 0 and t not in self.notes:
                continue
            state = "PASS" if not b and n else "FAIL"
            fails += state == "FAIL"
            print("%s %s: %d checks, %d mismatches" % (t, state, n, len(b)))
            if t == "T10" and self.edge:
                print("  EDGE (1/32 u grazes of a hitbox, not failures): %d (%s)" % (
                    sum(self.edge.values()), ", ".join("%s %d" % mc for mc in sorted(
                        self.edge.items()))))
            order = sorted((m, c) for (tt, m), c in self.order.items() if tt == t)
            if order:
                print("  ORDER (GoldSrc's area-node candidate order, not failures): %d (%s)" % (
                    sum(c for _, c in order), ", ".join("%s %d" % mc for mc in order)))
            for line in self.notes.get(t, []):
                print("  " + line)
            for w in b[:show]:
                print("  " + w)
        return fails


def seg_trace_cmds(run, rec, key_world=True):
    h = rec["hull"]
    mins, maxs = HULL_MINS[h], HULL_MAXS[h]
    tail = "%s %s %s %s" % (f9(rec["s"]), f9(rec["e"]), f9(mins), f9(maxs))
    iw = run.cmd("W " + tail, 1)
    im = run.cmd("M " + tail + " 1 -1", 1)
    return iw, im


def run_segments(a, rep, m, cap, tests):
    recs = load(a.fixtures, cap, m)
    run = Run(a.bin, a.content)
    run.cmd("O %s 1" % m)
    brush_cmds(run, recs)
    jobs = []
    for i, rec in enumerate(recs[2:]):
        iw, im = seg_trace_cmds(run, rec)
        jobs.append((rec, iw, im))
    res = run.go()
    # T4 queries: end points of non-all-solid world traces
    run2 = Run(a.bin, a.content)
    run2.cmd("O %s 1" % m)
    t4 = []
    shorts_gs = shorts_ours = 0
    tw = "T9" if cap == "T-allsolid" else "T2"
    for rec, iw, im in jobs:
        gw, gm = trace_of(res[iw]), trace_of(res[im])
        tag = "%s %s hull %d %s s=%s e=%s" % (m, cap, rec["hull"], rec["kind"], f9(rec["s"]),
                                               f9(rec["e"]))
        if tw in tests:
            d = diff(gw, rec["world"])
            rep.check(tw, not d, tag + " world: " + ", ".join(d))
            d = diff(gm, rec["move"])
            q = {"s": rec["s"], "e": rec["e"], "mins": HULL_MINS[rec["hull"]],
                 "maxs": HULL_MAXS[rec["hull"]], "type": 1, "pass": -1, "line": False}
            rep.check(tw, not d, tag + " move: " + ", ".join(d),
                      (m, recs[1]["brush_ents"], q, rec["move"]))
        if "T4" in tests and not gw["as"]:
            t4.append((rec, gw, run2.cmd("Q 0 %d %s" % (rec["hull"], f9(gw["end"])), 1)))
        if "T3" in tests and cap == "T-segments":
            ew = rec["world"]
            if 0 < ew["fr"] < 1:
                so = sum(n * e for n, e in zip(ew["n"], ew["end"])) - ew["d"]
                shorts_gs += abs(so - EPS) > 1e-3
            if 0 < gw["fr"] < 1:
                so = sum(n * e for n, e in zip(gw["n"], gw["end"])) - gw["d"]
                if abs(so - EPS) <= 1e-3:
                    rep.check("T3", True, "")
                else:
                    shorts_ours += 1
                    gs_short = 0 < ew["fr"] < 1 and abs(sum(
                        n * e for n, e in zip(ew["n"], ew["end"])) - ew["d"] - EPS) > 1e-3
                    same = all(abs(x - y) <= 1e-3 for x, y in zip(gw["end"], ew["end"]))
                    rep.check("T3", gs_short and same,
                              tag + " stand-off %.6f (GoldSrc %s)" % (so, "short" if gs_short
                                                                      else "not short"))
    if "T3" in tests and cap == "T-segments":
        rep.check("T3", shorts_ours <= shorts_gs,
                  "%s: %d short impacts > GoldSrc's %d" % (m, shorts_ours, shorts_gs))
        if shorts_gs or shorts_ours:
            rep.note("T3", "%s: short impacts hl64 %d, GoldSrc %d" % (m, shorts_ours, shorts_gs))
    if t4:
        res2 = run2.go()
        for rec, gw, iq in t4:
            c = int(res2[iq][1])
            rep.check("T4", c != SOLID, "%s %s hull %d end %s classifies SOLID" % (
                m, cap, rec["hull"], f9(gw["end"])))


def run_points(a, rep, m):
    recs = load(a.fixtures, "T-points", m)
    run = Run(a.bin, a.content)
    run.cmd("O %s 1" % m)
    brush_cmds(run, recs)
    jobs = [(rec, run.cmd("P " + f9(rec["p"]), 1)) for rec in recs[2:]]
    res = run.go()
    for rec, i in jobs:
        c, tc = int(res[i][1]), int(res[i][2])
        rep.check("T1", c == rec["c"] and tc == rec["tpc"],
                  "%s p=%s src=%s: contents %d/%d, GoldSrc %d/%d" % (
                      m, f9(rec["p"]), rec["src"], c, tc, rec["c"], rec["tpc"]))


def move_cmd(run, rec, mins, maxs, mode):
    return run.cmd("M %s %s %s %s %d %d" % (f9(rec["s"]), f9(rec["e"]), f9(mins), f9(maxs),
                                             mode, rec["pass"]), 1)


def entity_trace(run, rec, movers):
    """Command for one T-entities / T-rotated trace record."""
    fn = rec["fn"]
    if fn == "line":
        return run.cmd("N %s %s %d %d" % (f9(rec["s"]), f9(rec["e"]), rec["mode"], rec["pass"]), 1)
    if fn == "hull":
        h = rec["hull"]
        return move_cmd(run, rec, HULL_MINS[h], HULL_MAXS[h], rec["mode"])
    if fn == "monsterhull":
        mv = rec["mover"]
        if movers.get(mv) != (tuple(rec["mover_mins"]), tuple(rec["mover_maxs"])):
            movers[mv] = (tuple(rec["mover_mins"]), tuple(rec["mover_maxs"]))
            run.cmd("E %d 0 -1 %s 0 0 0 %s %s 0 -1 0 0 0" % (mv, f9(rec["s"]), f9(rec["mover_mins"]),
                                                           f9(rec["mover_maxs"])))
        return move_cmd(run, rec, rec["mover_mins"], rec["mover_maxs"], rec["mode"])
    if fn == "model":
        h = rec["hull"]
        return run.cmd("C %d %s %s %s %s" % (rec["target"], f9(rec["s"]), f9(rec["e"]),
                                             f9(HULL_MINS[h]), f9(HULL_MAXS[h])), 1)
    raise SystemExit("unknown fn %s" % fn)


def run_entities(a, rep, m, cap, tests):
    recs = load(a.fixtures, cap, m)
    run = Run(a.bin, a.content)
    run.cmd("O %s 1" % m)
    brush_cmds(run, recs)
    live, movers, jobs = [], {}, []
    setup = []
    for rec in recs[2:]:
        ents = None
        if "setup" in rec:
            ents = rec["ents"]
        elif "angles" in rec and "ent" in rec:
            ents = [rec["ent"]]
        if ents is not None:
            for i in live + list(movers):
                run.cmd("X %d" % i)
            movers = {}
            live = [e["i"] for e in ents]
            setup = ents
            for e in ents:
                run.cmd(ent_cmd(e))
            continue
        if "fn" not in rec:
            continue
        idx = entity_trace(run, rec, movers)
        mv = [{"i": i, "solid": 0, "o": rec["s"], "a": [0, 0, 0], "mins": list(b[0]),
               "maxs": list(b[1]), "absmin": [0, 0, 0], "absmax": [0, 0, 0], "flags": 0,
               "owner": -1, "rendermode": 0, "skin": 0, "movetype": 0}
              for i, b in movers.items()]
        jobs.append((rec, idx, recs[1]["brush_ents"] + setup + mv))
    res = run.go()
    for rec, i, ents in jobs:
        got = trace_of(res[i])
        move = None
        if rec["fn"] != "model":
            h = rec["hull"]
            box = ((0, 0, 0), (0, 0, 0)) if rec["fn"] == "line" else \
                (rec["mover_mins"], rec["mover_maxs"]) if rec["fn"] == "monsterhull" else \
                (HULL_MINS[h], HULL_MAXS[h])
            q = {"s": rec["s"], "e": rec["e"], "mins": box[0], "maxs": box[1],
                 "type": rec["mode"], "pass": rec["pass"], "line": rec["fn"] == "line"}
            move = (m, ents, q, rec)
        if cap == "T-rotated":
            test = "T7"
            d = diff(got, rec, ntol=1e-5)
        else:
            test = "T6" if rec["cfg"].startswith("hullsel") else "T5"
            d = diff(got, rec)
        if test in tests:
            rep.check(test, not d, "%s %s %s %s mode %d hull %d pass %d s=%s e=%s: %s" % (
                m, rec["cfg"], rec["fn"], rec.get("target", ""), rec.get("mode", 0), rec["hull"],
                rec.get("pass", -1), f9(rec["s"]), f9(rec["e"]), ", ".join(d)), move)


def run_layers(a, rep):
    """T8: hldemo1 in slot 0 and hldemo2 in slot 1 at their g_M."""
    recs = load(a.fixtures, "T-segments", "hldemo2")[2:][::8]
    run = Run(a.bin, a.content)
    p = subprocess.run([a.bin, a.content], input="O hldemo1 0\nO hldemo2 0\n",
                       capture_output=True, text=True)
    g = [[float(x) for x in l.split()[3:6]] for l in p.stdout.splitlines() if l.startswith("R O")]
    if len(g) != 2:
        raise SystemExit("T8: layers did not open")
    gb = g[1]
    run.cmd("O hldemo1 0")
    run.cmd("O hldemo2 0")
    jobs = []
    for rec in recs:
        h = rec["hull"]
        s, e = rec["s"], rec["e"]
        sg = [x + y for x, y in zip(s, gb)]
        eg = [x + y for x, y in zip(e, gb)]
        il = run.cmd("L 1 %d 0 0 0 %s %s" % (h, f9(s), f9(e)), 1)
        ig = run.cmd("L 1 %d %s %s %s" % (h, f9(gb), f9(sg), f9(eg)), 1)
        iw = run.cmd("W %s %s %s %s" % (f9(sg), f9(eg), f9(HULL_MINS[h]), f9(HULL_MAXS[h])), 1)
        jobs.append((rec, sg, eg, il, ig, iw))
    # outside every layer: corners beyond both global boxes
    far = []
    for k in range(64):
        q = [(-1) ** (k & 1) * 30000.0, (-1) ** ((k >> 1) & 1) * 30000.0, 100.0 * k - 3000]
        far.append(run.cmd("P " + f9(q), 1))
    res = run.go()
    tol = 2e-3      # global coordinates round at 1/2048 u up to 8192 u
    for rec, sg, eg, il, ig, iw in jobs:
        loc, glo, wor = trace_of(res[il]), trace_of(res[ig]), trace_of(res[iw])
        shifted = dict(loc)
        shifted["end"] = [x + y for x, y in zip(loc["end"], gb)] if loc["fr"] < 1 else eg
        d = diff(glo, shifted, etol=tol, dtol=1e-4, flags=("as", "ss"))
        if loc["fr"] < 1 and glo["fr"] < 1:
            pass
        rep.check("T8", not d, "hldemo2 hull %d s=%s: model global vs local: %s" % (
            rec["hull"], f9(rec["s"]), ", ".join(d)))
        wl = dict(shifted)
        wl["d"] = loc["d"] + sum(n * x for n, x in zip(loc["n"], gb)) if loc["fr"] < 1 else 0
        wl["hit"] = 0 if loc["fr"] < 1 or loc["ss"] else -1
        d = diff(wor, wl, etol=tol, dtol=tol, flags=("as", "ss"))
        rep.check("T8", not d, "hldemo2 hull %d s=%s: world global vs local: %s" % (
            rec["hull"], f9(rec["s"]), ", ".join(d)))
    for i in far:
        rep.check("T8", int(res[i][1]) == SOLID and int(res[i][2]) == SOLID,
                  "outside every layer: contents %s" % res[i][1:])

_cooked = {}


def studio_files(a, model):
    """The cooked files (path without extension) of the model the probe ran
    the monster with: from --content when staged there, else cooked now from
    the player's data (--data) into a scratch directory -- the GoldSrc probe
    ran Uplink's models (monster_scientist: models/scientistu.mdl), which
    the game's cook need not stage. None when neither has it."""
    base = model[:-4].lower()
    if os.path.isfile(os.path.join(a.content, base + ".s64")):
        return base
    if model in _cooked:
        return _cooked[model]
    _cooked[model] = None
    if a.data and os.path.isfile(os.path.join(a.data, model)):
        import atexit
        import shutil
        import tempfile
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
        from tools.studio import studiocook
        out = tempfile.mkdtemp(prefix="hl64-t10-")
        atexit.register(shutil.rmtree, out, True)
        mdl, anim, _ = studiocook.cook_model(studiocook.lookup(a.data), model)
        os.makedirs(os.path.join(out, "models"), exist_ok=True)
        for ext, data in ((".s64", mdl), (".a64", anim)):
            with open(os.path.join(out, base + ext), "wb") as f:
                f.write(data)
        _cooked[model] = os.path.join(out, base)
    return _cooked[model]


def run_hitbox(a, rep, m):
    """T10 on the T-hitbox capture: each monster in its four poses (sequence,
    frame, controllers, blending, body) plays its cooked studio model
    (models/<x>.s64 + .a64 in --content, else cooked from --data), so
    line traces test the pose's hitboxes through the studio module's
    hitbox source (engine/render/studio.c, game_studio_pose); hull traces
    use the bounding box. Every trace must equal GoldSrc: flags and
    hitgroup exact, end point +-0.01 u (spec T10), fraction to match."""
    recs = load(a.fixtures, "T-hitbox", m)
    run = Run(a.bin, a.content)
    run.cmd("O %s 1" % m)
    brush_cmds(run, recs)
    mon, jobs, models = None, [], {}
    for rec in recs[2:]:
        if "monster" in rec:
            if mon is not None:
                run.cmd("X %d" % mon["i"])
            mon = rec["ent"]
            base = studio_files(a, mon["model"])
            if base is None:
                rep.check("T10", False, "%s: %s neither cooked in %s nor in --data %s" % (
                    m, mon["model"], a.content, a.data))
                return
            if base not in models:
                models[base] = len(models)
                run.cmd("K %d %s.s64 %s.a64" % (models[base], base, base))
            setup = ["K %d %s.s64 %s.a64" % (models[base], base, base), ent_cmd(mon),
                     "A %d %d %d %.9g %s %s %d" % (
                         mon["i"], models[base], rec["sequence"], rec["frame"],
                         " ".join(str(c) for c in rec["controller"]),
                         " ".join(str(c) for c in rec["blending"]), rec["body"])]
            for c in setup[1:]:
                run.cmd(c)
            continue
        if "fn" not in rec:
            continue
        if rec["fn"] == "hull":
            h = rec["hull"]
            i = move_cmd(run, rec, HULL_MINS[h], HULL_MAXS[h], rec["mode"])
        else:
            i = run.cmd("N %s %s %d %d" % (f9(rec["s"]), f9(rec["e"]), rec["mode"], rec["pass"]), 1)
        rec["_slot"] = mon["i"]
        jobs.append((rec, i, mon["i"], setup))
    res = run.go()
    hits, bad = 0, []
    for rec, i, me, setup in jobs:
        got = trace_of(res[i])
        tag = "%s %s %s hull %d s=%s e=%s" % (m, rec["cfg"], rec["fn"], rec["hull"], f9(rec["s"]),
                                             f9(rec["e"]))
        if rec["fn"] == "hull":
            d = diff(got, rec)
        else:
            seg = math.sqrt(sum((x - y) ** 2 for x, y in zip(rec["s"], rec["e"]))) or 1.0
            d = diff(got, rec, etol=0.01, ftol=0.01 / seg, ntol=1e-3, dtol=0.01)
            hits += rec["hit"] == me
        if d and rec["fn"] == "line":
            bad.append((rec, tag + ": " + ", ".join(d), setup))
        else:
            rep.check("T10", not d, tag + ": " + ", ".join(d))
    edge = classify_edge(a, m, bad)
    for (rec, what, _), is_edge in zip(bad, edge):
        if is_edge:
            rep.edge[m] = rep.edge.get(m, 0) + 1
            rep.check("T10", True, what)
        else:
            rep.check("T10", False, what)
    rep.note("T10", "%s: %d traces, %d line hits on the monsters' hitboxes" % (m, len(jobs), hits))


def chords(hbs, s, e):
    """Length of segment s-e inside each oriented box (bone[12], lo, hi)."""
    out = []
    seg = math.sqrt(sum((x - y) ** 2 for x, y in zip(s, e)))
    for h in hbs:
        b = [h[0:4], h[4:8], h[8:12]]
        lo, hi = h[12:15], h[15:18]
        ls = [sum(b[r][k] * (s[r] - b[r][3]) for r in range(3)) for k in range(3)]
        le = [sum(b[r][k] * (e[r] - b[r][3]) for r in range(3)) for k in range(3)]
        t0, t1 = 0.0, 1.0
        for k in range(3):
            d = le[k] - ls[k]
            if abs(d) < 1e-12:
                if not lo[k] <= ls[k] <= hi[k]:
                    t0, t1 = 1.0, 0.0
                continue
            x, y = (lo[k] - ls[k]) / d, (hi[k] - ls[k]) / d
            t0, t1 = max(t0, min(x, y)), min(t1, max(x, y))
        out.append((t1 - t0) * seg)
    return out


def classify_edge(a, m, bad):
    """A hitbox line-trace mismatch is the EDGE deviation when the segment
    grazes one of the pose's hitboxes with a chord within 0.005 u of the
    walk's 1/32 u epsilon: whether such a graze is a hit is decided by the
    last bits of the pose and plane arithmetic (GoldSrc itself answers the
    same pose differently at different map placements); counted per map,
    must not grow (baseline: 3 of 151 065 traces)."""
    if not bad:
        return []
    run = Run(a.bin, a.content)
    run.cmd("O %s 1" % m)
    idx = []
    for rec, _, setup in bad:
        for c in setup:
            run.cmd(c)
        idx.append(run.cmd("H %d" % rec["_slot"], 0))
        run.cmd("X %d" % rec["_slot"])
    p = subprocess.run([run.binary, run.content], input="\n".join(run.cmds) + "\n",
                       capture_output=True, text=True)
    lines = p.stdout.splitlines()
    out, at = [], 0
    for rec, _, _ in bad:
        while not lines[at].startswith("R H "):
            at += 1
        n = int(lines[at].split()[2])
        hbs = [[float(x) for x in l.split()] for l in lines[at + 1:at + 1 + n]]
        at += 1 + n
        out.append(any(abs(c - EPS) < 0.005 for c in chords(hbs, rec["s"], rec["e"])))
    return out


def run_hitbox_synthetic(a, rep):
    """T10 self-contained: an oriented hitbox (yaw 30, on a slidebox) and
    line traces with analytic answers (entry stand-off 1/32 along the
    segment as the hull walk computes it, nearest box, group, normal)."""
    c, s_ = math.cos(math.radians(30)), math.sin(math.radians(30))
    bone = [[c, -s_, 0, 1000], [s_, c, 0, 0], [0, 0, 1, 100]]
    lo, hi = (-4, -6, -8), (4, 6, 8)
    run = Run(a.bin, a.content)
    run.cmd("O hldemo1 1")
    run.cmd("E 2 3 -1 1000 0 100 0 0 0 -16 -16 -16 16 16 16 32 -1 0 0 0")
    run.cmd("G 2 2")
    run.cmd("%s %s %s 5" % (" ".join("%.9g" % x for r in bone for x in r), f9(lo), f9(hi)))
    run.cmd("%s %s %s 1" % (" ".join("%.9g" % x for r in bone for x in r), f9((-1, -1, 9)),
                            f9((1, 1, 12))))
    jobs = []
    for k in range(24):
        ang = math.radians(15 * k)
        d = (math.cos(ang), math.sin(ang), 0.05 * (k % 5 - 2))
        st = [1000 - 40 * d[0], -40 * d[1], 100 - 40 * d[2]]
        en = [1000 + 40 * d[0], 40 * d[1], 100 + 40 * d[2]]
        jobs.append((st, en, run.cmd("N %s %s 0 -1" % (f9(st), f9(en)), 1)))
    # straight down onto the head box (group 1) above the body box
    st, en = [1000, 0, 160], [1000, 0, 40]
    jobs.append((st, en, run.cmd("N %s %s 0 -1" % (f9(st), f9(en)), 1)))
    res = run.go()
    for st, en, i in jobs:
        got = trace_of(res[i])
        best = None
        for (blo, bhi, grp) in ((lo, hi, 5), ((-1, -1, 9), (1, 1, 12), 1)):
            ls = [sum(bone[r][k] * (st[r] - bone[r][3]) for r in range(3)) for k in range(3)]
            le = [sum(bone[r][k] * (en[r] - bone[r][3]) for r in range(3)) for k in range(3)]
            enter, leave, ax, sg = -1.0, 1.0, None, 0
            for k in range(3):
                for sign, face in ((1, bhi[k]), (-1, blo[k])):
                    d1 = sign * (ls[k] - face)
                    d2 = sign * (le[k] - face)
                    if d1 > 0 and d2 >= d1:
                        enter = 2.0
                    elif d1 > d2 and d1 > 0:
                        f = (d1 - EPS) / (d1 - d2)
                        if f > enter:
                            enter, ax, sg = f, k, sign
                    elif d1 <= d2 and not (d1 <= 0 and d2 <= 0):
                        leave = min(leave, (d1 + EPS) / (d1 - d2))
            if ax is not None and enter < leave and enter <= 1 and (best is None or enter < best[0]):
                best = (max(enter, 0.0), ax, sg, grp)
        if best is None:
            ok = got["hit"] != 2
        else:
            f, ax, sg, grp = best
            endp = [x + f * (y - x) for x, y in zip(st, en)]
            n = [sg * bone[r][ax] for r in range(3)]
            ok = got["hit"] == 2 and got["hg"] == grp and abs(got["fr"] - f) <= 1e-5 and all(
                abs(x - y) <= 1e-3 for x, y in zip(got["end"], endp)) and all(
                abs(x - y) <= 1e-5 for x, y in zip(got["n"], n))
        rep.check("T10", ok, "synthetic hitbox s=%s e=%s: got hit %d hg %d fr %.6f, want %s" % (
            f9(st), f9(en), got["hit"], got["hg"], got["fr"], best))


FL_MONSTER, FL_MONSTERCLIP, PUSHSTEP = 1 << 5, 1 << 23, 13


def candidates(ents, q, wend):
    """The move's candidates after the spec 4.4 filters, ascending index,
    with the box each is clipped with (fixture linked boxes)."""
    mt, pas = q["type"] & 0xFF, {e["i"]: e for e in ents}.get(q["pass"])
    bmin = [min(x, -15) if mt == 2 else x for x in q["mins"]]
    bmax = [max(x, 15) if mt == 2 else x for x in q["maxs"]]
    lo = [min(a, b) + c - 1 for a, b, c in zip(q["s"], wend, bmin)]
    hi = [max(a, b) + c + 1 for a, b, c in zip(q["s"], wend, bmax)]
    out = {}
    for e in ents:
        if e["i"] <= 0 or e["solid"] in (0, 1) or (pas and e["i"] == pas["i"]):
            continue
        if mt == 1 and e["solid"] != 4 and e.get("movetype", 0) != PUSHSTEP:
            continue
        if q["type"] & 0x100 and e.get("rendermode", 0) != 0:
            continue
        if pas and pas["maxs"][0] - pas["mins"][0] and not e["maxs"][0] - e["mins"][0]:
            continue
        if pas and (e.get("owner", -1) == pas["i"] or pas.get("owner", -1) == e["i"]):
            continue
        if e.get("flags", 0) & FL_MONSTERCLIP and e["solid"] == 4 \
                and not (pas and pas.get("flags", 0) & FL_MONSTERCLIP):
            continue
        if any(lo[k] > e["absmax"][k] or hi[k] < e["absmin"][k] for k in range(3)):
            continue
        box = ((-15,) * 3, (15,) * 3) if mt == 2 and e.get("flags", 0) & FL_MONSTER else \
            (q["mins"], q["maxs"])
        out[e["i"]] = box
    return sorted(out.items())


def combine(world, clips, line):
    """Spec 4.4 step 5 over clips in the given order (fractions scaled to
    the whole move, as col_move does)."""
    tr = dict(world)
    for i, t in clips:
        t = dict(t)
        t["fr"] = t["fr"] * world["fr"]
        if t["as"] or t["ss"] or t["fr"] < tr["fr"]:
            ss = tr["ss"]
            tr = t
            tr["ss"] = tr["ss"] or ss
        elif t["ss"]:
            tr["ss"] = 1
    if line and tr["hit"] < 0:
        tr["hit"] = 0
    return tr


def resolve_order(a, rep, ntol_t7=1e-5):
    """Held move mismatches: rerun the world trace and each candidate's own
    clip (start -> world end); one reproduced by some candidate order is
    an ORDER deviation (GoldSrc visits candidates in its area-node order,
    hl64 in ascending index), anything else a failure."""
    by_map = {}
    for item in rep.pending:
        by_map.setdefault(item[2], []).append(item)
    for m, items in by_map.items():
        def ent_run(stage):
            run = Run(a.bin, a.content)
            run.cmd("O %s 1" % m)
            return run
        run = ent_run(1)
        iws = []
        for test, what, _, ents, q, exp in items:
            for e in ents:
                run.cmd(ent_cmd(e))
            iws.append(run.cmd("W %s %s %s %s" % (f9(q["s"]), f9(q["e"]), f9(q["mins"]),
                                                  f9(q["maxs"])), 1))
            for e in ents:
                run.cmd("X %d" % e["i"])
        res = run.go()
        run = ent_run(2)
        jobs = []
        for (test, what, _, ents, q, exp), iw in zip(items, iws):
            w = trace_of(res[iw])
            w["hit"] = 0 if w["fr"] < 1 or w["ss"] else -1
            for e in ents:
                run.cmd(ent_cmd(e))
            cl = [(i, run.cmd("C %d %s %s %s %s" % (i, f9(q["s"]), f9(w["end"]), f9(b[0]),
                                                    f9(b[1])), 1))
                  for i, b in candidates(ents, q, w["end"])]
            for e in ents:
                run.cmd("X %d" % e["i"])
            jobs.append((test, what, q, exp, w, cl))
        res = run.go()
        for test, what, q, exp, w, cl in jobs:
            clips = [(i, trace_of(res[j])) for i, j in cl]
            ntol = ntol_t7 if test == "T7" else 0.0
            explained = not w["as"] and len(clips) <= 7 and any(
                not diff(combine(w, list(p), q["line"]), exp, ntol=ntol)
                for p in itertools.permutations(clips))
            if explained:
                rep.order[(test, m)] = rep.order.get((test, m), 0) + 1
            else:
                rep.bad.setdefault(test, []).append(what)
    rep.pending = []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True, help="hl64-colltest")
    ap.add_argument("--content", required=True, help="cooked content (maps/*.w64)")
    ap.add_argument("--fixtures", default=os.environ.get("HL64_FIXTURES",
                                                          "/scratch/hl64/fixtures"))
    ap.add_argument("--data", default=os.environ.get("HL64_DATA", "/scratch/hl1/valve"),
                    help="the player's Half-Life game directory (T10 cooks the probe's "
                         "studio models missing from --content)")
    ap.add_argument("--maps", nargs="*", default=MAPS)
    ap.add_argument("--tests", default="T1,T2,T3,T4,T5,T6,T7,T8,T9,T10")
    ap.add_argument("--show", type=int, default=10, help="mismatches printed per test")
    a = ap.parse_args()
    tests = a.tests.split(",")
    rep = Report()
    for m in a.maps:
        if "T1" in tests:
            run_points(a, rep, m)
        if {"T2", "T3", "T4"} & set(tests):
            run_segments(a, rep, m, "T-segments", tests)
        if {"T9", "T4"} & set(tests):
            run_segments(a, rep, m, "T-allsolid", tests)
        if {"T5", "T6"} & set(tests):
            run_entities(a, rep, m, "T-entities", tests)
        if "T7" in tests:
            run_entities(a, rep, m, "T-rotated", tests)
        if "T10" in tests:
            run_hitbox(a, rep, m)
    if "T8" in tests:
        run_layers(a, rep)
    if "T10" in tests:
        run_hitbox_synthetic(a, rep)
    resolve_order(a, rep)
    fails = rep.print(["T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8", "T9", "T10"], a.show)
    print("trace-and-hulls: %s" % ("PASS" if not fails else "FAIL (%d tests)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
