#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Area links and touches against the GoldSrc probe fixtures
(docs/specs/area-links-and-touch.md 5).

Builds one script per map for the host harness (test/m2/area.c, the real
engine/ent grid, engine/phys touches and engine/coll traces), runs it and
compares the result with the fixtures:

  A1  linked boxes: every probe read-back (box entities per solid type and
      flag, yawed, brush entities rotated / pitched / angled triggers), every
      mover and bot box of the touch cases, and every map brush entity of
      the fixture's brush_ents line (exact floats).
  A3  box trigger vs FLY / bit-256 / SOLID_NOT / SOLID_TRIGGER / NONE /
      TOSS movers and the fake client at gaps -8 ... 18 u per axis.
  A4  set_origin touches nothing; the next physics link does.
  A5  brush trigger touched iff the mover's origin is in its brush for the
      mover's hull (player-size movers: hull 1), angles ignored (A-shape,
      hldemo1-3: 640 samples x bot + mover).
  A7  impact scenarios: touch order, A removed / SOLID_NOT rules, impact
      frame and stop point (A-touch cases >= 1000).
  A9  trigger touch order: the `_order` cases (runtime triggers in creation
      order: the higher index first) and, when present, the A-order
      captures: every map trigger the cook ranked is linked with its
      captured box and the cooked `trank` (CONTENT/maps/<m>.w64), every
      phase-1/3 placement's mover is linked with touch: "A9 touched hN"
      wants every trigger GoldSrc touched touched (per mover hull), "A9
      order" every two co-touched triggers in GoldSrc's order; the probe
      pairs (526) are tallied as match / not co-touched / game-created /
      not spawned by hl64 / known (KNOWN_ORDER) / fail (out of order).
  A2, A6  self-contained (harness `selftest`).

Usage: test/m2/area.py --bin BUILD/host/hl64-area --content CONTENT
       [--fixtures $HL64_FIXTURES/goldsrc] [--maps hldemo1,...]
Exit 0 when every check passes.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

SPEC = "area-links-and-touch"
SOLID_NOT, SOLID_TRIGGER, SOLID_BBOX, SOLID_SLIDEBOX = 0, 1, 2, 3
MOVETYPE_NONE, MOVETYPE_FLY, MOVETYPE_TOSS, MOVETYPE_BOUNCE = 0, 5, 6, 10
MOVE_NORMAL, MOVE_NOMONSTERS = 0, 1
FRAMETIME = 0.01
# Probe touch modes (test/oracle/probe: PT_LOG, PT_SELF_NOT, PT_REMOVE_OTHER,
# PT_REMOVE_SELF) as harness touch ids.
T_LOG, T_SELF_NOT, T_FREE_OTHER, T_FREE_SELF = 1, 2, 3, 4
# The probe's A-touch geometry (test/oracle/probe/probe_area.cpp): the
# order cases add T2 (-24,-40,-24)...(40,24,24) at the spot and T3 +-8 at
# spot + (4,0,0); impact scenarios: B +-16/+-36 at the spot, A +-8.
T2_BOX = ((-24, -40, -24), (40, 24, 24))
T3_BOX = ((-8, -8, -8), (8, 8, 8))
IMPACT_MODES = {
    "fly_into_box": (T_LOG, T_LOG),
    "fly_self_not": (T_SELF_NOT, T_LOG),
    "fly_solid_not": (T_LOG, T_LOG),
    "toss_onto_box": (T_LOG, T_LOG),
    "bounce_into_box": (T_LOG, T_LOG),
    "target_removes_mover": (T_LOG, T_FREE_OTHER),
    "mover_removes_self": (T_FREE_SELF, T_LOG),
}


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def v(x):
    return " ".join(repr(f32(c)) for c in x)


MAX_ENTITIES = 495     # include/hl64/limits.h HL64_MAX_ENTITIES


def read(path, extra=()):
    """Records of a fixture, GoldSrc entity indices >= MAX_ENTITIES (the
    probe server's edicts go to 900) moved to free slots from the top;
    `extra` names further keys that hold entity indices."""
    with open(path) as f:
        recs = [json.loads(l) for l in f if l.strip()]
    used, refs = set(), []

    def walk(o):
        if isinstance(o, dict):
            for k, x in o.items():
                if k in ("i", "self", "other", "a", "b", "triggers") or k in extra:
                    refs.append((o, k))
                walk(x)
        elif isinstance(o, list):
            for x in o:
                walk(x)
    for r in recs:
        if "meta" not in r:
            walk(r)
    for o, k in refs:
        x = o[k]
        for n in (x if isinstance(x, list) else [x]):
            if isinstance(n, int):
                used.add(n)
    remap, free = {}, MAX_ENTITIES - 1
    for n in sorted(x for x in used if x >= MAX_ENTITIES):
        while free in used:
            free -= 1
        remap[n] = free
        free -= 1
    for o, k in refs:
        x = o[k]
        if isinstance(x, list):
            o[k] = [remap.get(n, n) if isinstance(n, int) else n for n in x]
        elif isinstance(x, int):
            o[k] = remap.get(x, x)
    return recs


class Script:
    def __init__(self):
        self.lines = []
        self.expect = []     # (tag, kind, payload) checks, matched by tag

    def add(self, s):
        self.lines.append(s)

    def tag(self, t):
        self.add("tag " + t)

    def ent(self, e, solid=None, origin=None, touch=0, model="-"):
        self.add("ent %d %d %s %s %s %s %d %s %d" % (
            e["i"], e["solid"] if solid is None else solid,
            v(e["o"] if origin is None else origin), v(e["a"]), v(e["mins"]),
            v(e["maxs"]), e["flags"], model, touch))


def run(binary, content, script):
    args = [binary] + (["--root", content] if content else [])
    p = subprocess.run(args, input="\n".join(script.lines) + "\n",
                       capture_output=True, text=True)
    if p.returncode not in (0, 1) or "FAIL script" in p.stdout:
        sys.exit("hl64-area failed (%d): %s%s" % (p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
    out, cur = {}, None
    for l in p.stdout.splitlines():
        if l.startswith("tag "):
            cur = l[4:].strip()
            out.setdefault(cur, [])
        elif cur is not None:
            out[cur].append(l.split())
    return out


class Report:
    def __init__(self):
        self.n = {}
        self.fails = []

    def check(self, test, ok, what):
        p, f = self.n.get(test, (0, 0))
        self.n[test] = (p + bool(ok), f + (not ok))
        if not ok and len(self.fails) < 60:
            self.fails.append("%s: %s" % (test, what))


def box_ok(rep, test, got, ent, what):
    want = [f32(c) for c in ent["absmin"] + ent["absmax"]]
    have = [f32(c) for c in got[2:8]] if got and got[0] == "box" else None
    rep.check(test, have == want, "%s: box %s, want %s" % (what, have, want))


def touches(lines):
    return [(int(l[1]), int(l[2])) for l in lines if l[0] == "touch"]


# ---- A-touch -----------------------------------------------------------------

def a_touch(recs, sc, sci):
    """Script and expectations for one A-touch file; the impact scenarios
    go to sci, which opens the map (col_move traces the world first)."""
    sci.add("map %s" % recs[0]["meta"]["map"])
    exp = []        # (tag, test, kind, data)
    ev = {}         # case -> touch events
    for r in recs:
        if r.get("ev") == "touch":
            ev.setdefault(r["case"], []).append(r)
    recs = [r for r in recs if r.get("ev") != "touch"]
    # A1: map brush entities as linked by GoldSrc
    for e in recs[1]["brush_ents"]:
        t = "be%d" % e["i"]
        sc.ent(e)
        sc.add("link %d" % e["i"])
        sc.tag(t)
        sc.add("box %d" % e["i"])
        exp.append((t, "A1", "box", (e, "brush ent %d %s" % (e["i"], e["class"]))))
        sc.add("free %d" % e["i"])
    i = 2
    spot = park = T1 = M = None
    cases = {}
    while i < len(recs):
        r = recs[i]
        i += 1
        if "spot" in r:
            spot, park = r["spot"], r["park"]
        elif "linkbox" in r:
            e, t = r["ent"], "lb_" + r["linkbox"]
            sc.ent(e)
            sc.add("link %d" % e["i"])
            sc.tag(t)
            sc.add("box %d" % e["i"])
            sc.add("free %d" % e["i"])
            exp.append((t, "A1", "box", (e, r["linkbox"])))
        elif r.get("setup") == "touch":
            T1, M = r["trigger"], r["mover"]
            sc.ent(T1, touch=T_LOG)
            sc.add("link %d" % T1["i"])
            sc.ent(M)
            sc.add("link %d" % M["i"])
        elif r.get("setup") == "order":
            t2, t3 = r["triggers"][1], r["triggers"][2]
            for idx, (mn, mx), ofs in ((t2, T2_BOX, (0, 0, 0)), (t3, T3_BOX, (4, 0, 0))):
                o = [spot[k] + ofs[k] for k in range(3)]
                sc.add("ent %d %d %s 0 0 0 %s %s 0 - %d" % (idx, SOLID_TRIGGER, v(o), v(mn), v(mx), T_LOG))
                sc.add("link %d" % idx)
            cases["order"] = r["triggers"]
        elif "case" in r and "name" in r and r["case"] < 1000:
            c = r["case"]
            # trigger touches of this case's physics / pmove link (world
            # impacts of a mover resting on t0a0d's floor are physics, M3)
            want = [(x["self"], x["other"]) for x in ev.get(c, [])
                    if x["phase"] in ("physics", "pmove") and x["other"] != 0 and x["self_solid"] == SOLID_TRIGGER
                    and x["self"] != M["i"]]
            setorig = [x for x in ev.get(c, []) if x["phase"] == "set_origin"]
            exp.append(("c%d" % c, "A4", "none_at_set_origin", setorig))
            test = "A9" if r["name"].endswith("_order") else "A3"
            if r["kind"] == 6:      # bot: player box, set_origin then pmove
                b = recs[i]
                i += 1
                assert b.get("ev") == "bot" and b["case"] == c, b
                bot = {"i": 1, "solid": SOLID_SLIDEBOX, "o": b["pos"], "a": [0, 0, 0],
                       "mins": [-16, -16, -36], "maxs": [16, 16, 36], "flags": 0}
                sc.ent(bot)
                sc.tag("c%d_set" % c)
                sc.add("link 1")
                sc.add("org 1 %s" % v(b["after"]))
                sc.tag("c%d" % c)
                sc.add("linkt 1")
                sc.tag("c%d_box" % c)
                sc.add("box 1")
                exp.append(("c%d" % c, test, "touches", (want, r["name"])))
                exp.append(("c%d_set" % c, "A4", "touches", ([], r["name"] + " set_origin")))
                exp.append(("c%d_box" % c, "A1", "box", ({"absmin": b["absmin"], "absmax": b["absmax"]}, r["name"])))
                sc.add("free 1")
                continue
            placed, after = recs[i], recs[i + 1]
            i += 2
            assert placed.get("ev") == "placed" and after.get("ev") == "after", (placed, after)
            pe, ae = placed["ent"], after["ent"]
            sc.ent(pe)
            sc.tag("c%d_set" % c)
            sc.add("link %d" % pe["i"])
            sc.tag("c%d_placed" % c)
            sc.add("box %d" % pe["i"])
            sc.add("org %d %s" % (pe["i"], v(ae["o"])))
            sc.tag("c%d" % c)
            # physics frame: FLY / TOSS link with touch; NONE links without
            # (docs/specs/physics-movetypes.md); SOLID_NOT touches nothing
            sc.add(("link %d" if pe["movetype"] == MOVETYPE_NONE else "linkt %d") % pe["i"])
            sc.tag("c%d_after" % c)
            sc.add("box %d" % pe["i"])
            exp.append(("c%d" % c, test, "touches", (want, r["name"])))
            exp.append(("c%d_set" % c, "A4", "touches", ([], r["name"] + " set_origin")))
            exp.append(("c%d_placed" % c, "A1", "box", (pe, r["name"] + " placed")))
            exp.append(("c%d_after" % c, "A1", "box", (ae, r["name"] + " after")))
            # park (set_origin)
            sc.add("ent %d %d %s 0 0 0 %s %s 0 - 0" % (M["i"], M["solid"], v(park), v(M["mins"]), v(M["maxs"])))
            sc.add("link %d" % M["i"])
        elif "case" in r and "name" in r:
            # impact scenarios; previous case's entities are freed first
            i = impact(recs, i, r, ev, sci, exp, cases)
    return exp


def impact(recs, i, r, ev, sc, exp, cases):
    c, a, b = r["case"], r["a"], r["b"]
    for k in cases.pop("impact", ()):
        sc.add("free %d" % k)
    am, bm = IMPACT_MODES[r["name"]]
    frames = []
    while i < len(recs) and recs[i].get("case") == c and "ev" in recs[i]:
        frames.append(recs[i])
        i += 1
    bs = [x for x in frames if x["ev"] == "b"][0]["ent"]
    seq = [x for x in frames if x["ev"] in ("a", "a_removed")]
    a0 = seq[0]["ent"]
    sc.ent(bs, touch=bm)
    sc.add("link %d" % b)
    sc.ent(a0, touch=am)
    sc.add("link %d" % a)
    cases["impact"] = (a, b)
    want = [(x["self"], x["other"]) for x in ev.get(c, []) if x["other"] != 0 and x["self"] != 0]
    hitf = sorted({x["frame"] for x in ev.get(c, [])})
    typ = MOVE_NOMONSTERS if a0["solid"] == SOLID_NOT else MOVE_NORMAL
    sc.tag("i%d" % c)
    prev = None
    for k in range(len(seq) - 1):
        cur, nxt = seq[k], seq[k + 1]
        if cur["ev"] != "a":
            break
        o, vel = cur["ent"]["o"], cur["v"]
        if cur["frame"] in hitf or nxt["ev"] != "a":
            g = 800.0 * FRAMETIME if cur["ent"]["movetype"] in (MOVETYPE_TOSS, MOVETYPE_BOUNCE) else 0.0
            end = [o[0] + vel[0] * FRAMETIME, o[1] + vel[1] * FRAMETIME, o[2] + (vel[2] - g) * FRAMETIME]
        else:
            end = nxt["ent"]["o"]
        sc.add("tag i%d_f%d" % (c, cur["frame"]))
        sc.add("org %d %s" % (a, v(o)))
        sc.add("move %d %s %d" % (a, v(end), typ))
        if cur["frame"] in hitf and nxt["ev"] == "a" and cur["ent"]["movetype"] != MOVETYPE_BOUNCE:
            exp.append(("i%d_f%d" % (c, cur["frame"]), "A7", "stop", (nxt["ent"]["o"], r["name"])))
        prev = cur
    exp.append(("i%d" % c, "A7", "impact_touches", (want, r["name"], hitf)))
    return i


# ---- A-shape -----------------------------------------------------------------

def a_shape(recs, sc):
    exp = []
    if any("skipped" in r for r in recs):
        return exp, False
    src = [r for r in recs if "source" in r][0]["source"]
    sc.add("map %s" % recs[0]["meta"]["map"])
    ev = {}
    for r in recs:
        if r.get("ev") == "touch":
            ev.setdefault(r["case"], []).append(r)
    recs = [r for r in recs if r.get("ev") != "touch"]
    T = None
    i = 0
    while i < len(recs):
        r = recs[i]
        i += 1
        if "variant" in r and "trigger" in r:
            if T:
                sc.add("free %d" % T["i"])
            T = r["trigger"]
            sc.ent(T, touch=T_LOG, model=T["model"])
            sc.add("link %d" % T["i"])
        elif "case" in r and "p" in r:
            c = r["case"]
            bot, after = recs[i], recs[i + 1]
            i += 2
            assert bot.get("ev") == "bot" and after.get("ev") == "after", (bot, after)
            # the trigger's touches only (a mover placed in a wall also
            # touches the world: physics impacts, M3)
            want_pm = [(x["self"], x["other"]) for x in ev.get(c, [])
                       if x["phase"] == "pmove" and x["self_solid"] == SOLID_TRIGGER]
            want_ph = [(x["self"], x["other"]) for x in ev.get(c, [])
                       if x["phase"] == "physics" and x["self_solid"] == SOLID_TRIGGER]
            sc.add("ent 1 %d %s 0 0 0 -16 -16 -36 16 16 36 0 - 0" % (SOLID_SLIDEBOX, v(r["p"])))
            sc.add("link 1")
            sc.add("org 1 %s" % v(bot["after"]))
            sc.tag("s%d_pm" % c)
            sc.add("linkt 1")
            sc.add("free 1")
            me = after["ent"]
            sc.ent(me, origin=r["p"])
            sc.add("link %d" % me["i"])
            sc.add("org %d %s" % (me["i"], v(me["o"])))
            sc.tag("s%d_ph" % c)
            sc.add("linkt %d" % me["i"])
            sc.add("free %d" % me["i"])
            what = "variant %d case %d p %s in_hull1_unrotated %d" % (r["variant"], c, r["p"], r["in_hull1_unrotated"])
            exp.append(("s%d_pm" % c, "A5", "touches", (want_pm, what + " bot")))
            exp.append(("s%d_ph" % c, "A5", "touches", (want_ph, what + " mover")))
            # the GoldSrc rule itself: touched iff inside the unrotated
            # hull-1 brush and the boxes meet (the bot only where its zero
            # move left it at p; a bot started in solid is pushed out)
            inside = bool(r["in_hull1_unrotated"]) and bool(r["box_overlap"])
            ok = bool(want_ph) == inside and (bot["after"] != r["p"] or bool(want_pm) == inside)
            exp.append((None, "A5", "rule", (ok, what + " src %s" % src["model"])))
    return exp, True

# ---- A-order (A9: map triggers in cooked trank order) --------------------------

# Hull boxes of the probe's movers (hull index -> mins, maxs).
HULLS = {0: ((0, 0, 0), (0, 0, 0)), 1: ((-16, -16, -36), (16, 16, 36)),
         2: ((-32, -32, -32), (32, 32, 32)), 3: ((-16, -16, -18), (16, 16, 18))}
MOVER = 1
HLE_NONE = 0xFFFF
# GoldSrc pairs no geometry predicts (map, first lump, second lump); the
# cause is in docs/specs/area-links-and-touch.md 3.1 (tools/w64c/touchrank.py
# KNOWN).
KNOWN_ORDER = {("hldemo2", 509, 601): "item_healthkit 601 relinks after it falls (func_wall_toggle *80)"}


def cooked_tranks(w64):
    """lump -> trank of the cooked package's HLE1 records
    (docs/formats/w64-format.md ENTS, hle-format-v1.md)."""
    with open(w64, "rb") as f:
        b = f.read()
    dofs = struct.unpack_from(">I", b, 12)[0]
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    ents = struct.unpack_from(">H", b, dofs + 52)[0]
    assert b[:4] == b"W64L" and ents < nchunks, w64
    o = struct.unpack_from(">I", b, cd + 16 * ents + 4)[0]
    bo = "<" if b[o:o + 4] == b"HLE1" else ">"
    numrecs = struct.unpack_from(bo + "H", b, o + 18)[0]
    recs = o + struct.unpack_from(bo + "I", b, o + 52)[0]
    return {struct.unpack_from(bo + "H", b, recs + 24 * r)[0]:
            struct.unpack_from(bo + "H", b, recs + 24 * r + 18)[0] for r in range(numrecs)}


def entity_lump(path):
    """The staged entity lump text (maps/<m>.ent) as key/value dicts, later
    keys win (tools/cook/hl_classes.py map_entities)."""
    with open(path, encoding="latin-1") as f:
        text = f.read()
    return [dict(re.findall(r'"([^"]*)"\s*"([^"]*)"', blk))
            for blk in re.findall(r'\{((?:"[^"]*"|[^"{}])*)\}', text)]


def a_order(recs, content):
    """Script and cases of one A-order capture: every map trigger the cook
    ranked, linked with its captured box and cooked trank, then every
    phase-1/3 placement's mover linked with touch."""
    m = recs[0]["meta"]["map"]
    tranks = cooked_tranks(os.path.join(content, "maps", m + ".w64"))
    ents = entity_lump(os.path.join(content, "maps", m + ".ent"))
    sc = Script()
    sc.add("map %s" % m)
    trig, lump = {}, {}
    for r in recs:
        if "trigger" not in r:
            continue
        t = r["trigger"]
        trig[t["i"]] = t
        if not 0 <= t["lump"] < len(ents) or ents[t["lump"]].get("classname", "").lower() != t["class"].lower():
            continue
        if not t["model"].startswith("*"):
            o = (ents[t["lump"]].get("origin", "") + " 0 0 0").split()[:2]
            if [float(c) for c in o] != t["o"][:2]:
                continue        # a game-created trigger in a freed map slot
        lump[t["i"]] = t["lump"]
        rank = tranks.get(t["lump"], HLE_NONE)
        if rank == HLE_NONE:
            continue            # class hl64 does not spawn
        mn = [t["absmin"][k] + 1 - t["o"][k] for k in range(3)]
        mx = [t["absmax"][k] - 1 - t["o"][k] for k in range(3)]
        assert t["i"] != MOVER
        sc.add("ent %d %d %s %s %s %s 0 %s %d" % (t["i"], SOLID_TRIGGER, v(t["o"]), v(t["a"]), v(mn), v(mx),
                                                  t["model"] if t["model"].startswith("*") else "-", T_LOG))
        sc.add("rank %d %d" % (t["i"], rank))
        sc.add("link %d" % t["i"])
    cases = []
    for r in recs:
        if "place" not in r or r.get("phase") not in (1, 3):
            continue
        mn, mx = HULLS[r["hull"]]
        sc.add("ent %d %d %s 0 0 0 %s %s 0 - 0" % (MOVER, SOLID_SLIDEBOX if r["mover"] == "bot" else SOLID_BBOX,
                                                    v(r["p"]), v(mn), v(mx)))
        sc.add("link %d" % MOVER)
        sc.add("org %d %s" % (MOVER, v(r["after"])))
        sc.tag("o%d" % r["place"])
        sc.add("linkt %d" % MOVER)
        sc.add("free %d" % MOVER)
        cases.append(r)
    ranked = {i for i in lump if tranks.get(lump[i], HLE_NONE) != HLE_NONE}
    return sc, cases, lump, ranked


def a_order_compare(rep, m, cases, lump, ranked, out, tally):
    """Per placement: every two co-touched ranked map triggers touch in
    GoldSrc's order (A9 order); per probe pair (526 over the 10 maps): in
    order at every placement, or counted as game-created / not spawned by
    hl64 / known."""
    pairs = {}
    for r in cases:
        have = [int(l[1]) for l in out.get("o%d" % r["place"], []) if l[0] == "touch" and int(l[2]) == MOVER]
        want = [x for x in r["touched"] if x in ranked]
        where = "%s place %d %s hull %d" % (m, r["place"], r["mover"], r["hull"])
        pos = {x: k for k, x in enumerate(have)}
        missing = [x for x in want if x not in pos]
        extra = [x for x in have if x in ranked and x not in want]
        rep.check("A9 touched h%d" % r["hull"], not missing and not extra,
                  "%s: GoldSrc touched %s, hl64 %s" % (where, want, have))
        ok = True
        for a_, b_ in ((want[j], want[k]) for j in range(len(want)) for k in range(j + 1, len(want))):
            if a_ not in pos or b_ not in pos:
                continue
            elif pos[a_] > pos[b_]:
                if (m, lump[a_], lump[b_]) in KNOWN_ORDER:
                    tally["known placements"] = tally.get("known placements", 0) + 1
                else:
                    rep.check("A9 order", False, "%s: GoldSrc %d before %d, hl64 %s" % (where, a_, b_, have))
                    ok = False
        if ok:
            rep.check("A9 order", True, "")
        p = tuple(sorted(r["pair"]))
        a_, b_ = r["order"]
        if a_ not in lump or b_ not in lump:
            kind = "game-created"
        elif a_ not in ranked or b_ not in ranked:
            kind = "not spawned by hl64"
        elif (m, lump[a_], lump[b_]) in KNOWN_ORDER:
            kind = "known"
        elif a_ not in pos or b_ not in pos:
            kind = "not co-touched"     # this placement: A9 touched reports it
        else:
            kind = "match" if pos[a_] < pos[b_] else "fail"
        rank = {"fail": 3, "match": 2, "not co-touched": 1}
        if rank.get(kind, 0) >= rank.get(pairs.get(p), 0):
            pairs[p] = kind
    for kind in pairs.values():
        tally[kind] = tally.get(kind, 0) + 1
    return pairs


def compare(rep, exp, out):
    for tag, test, kind, data in exp:
        got = out.get(tag, []) if tag else []
        if kind == "box":
            e, what = data
            box_ok(rep, test, got[0] if got else None, e, what)
        elif kind == "touches":
            want, what = data
            have = touches(got)
            rep.check(test, have == want, "%s: touches %s, want %s" % (what, have, want))
        elif kind == "none_at_set_origin":
            rep.check(test, not data, "GoldSrc touched during set_origin: %s" % data)
        elif kind == "rule":
            ok, what = data
            rep.check(test, ok, "GoldSrc fixture disagrees with the spec rule: " + what)
        elif kind == "stop":
            want, what = data
            mv = [l for l in got if l[0] == "move"]
            have = [float(c) for c in mv[0][4:7]] if mv else None
            ok = have is not None and all(abs(have[k] - want[k]) <= 1e-3 for k in range(3))
            rep.check(test, ok, "%s: stop %s, want %s" % (what, have, want))
        elif kind == "impact_touches":
            want, what, hitf = data
            have = []
            for t, lines in out.items():
                if t.startswith(tag + "_f"):
                    have += touches(lines)
            rep.check(test, have == want, "%s: impact touches %s, want %s (frames %s)" % (what, have, want, hitf))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--fixtures", default=os.path.join(os.environ.get("HL64_FIXTURES", "/scratch/hl64/fixtures"), "goldsrc"))
    ap.add_argument("--maps", default="hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d")
    args = ap.parse_args()
    rep = Report()
    st = subprocess.run([args.bin], input="selftest\n", capture_output=True, text=True)
    rep.check("A2+A6", st.returncode == 0 and "selftest ok" in st.stdout, "selftest: " + st.stdout[-1500:])
    shapes = 0
    order_pairs = []
    for m in args.maps.split(","):
        base = os.path.join(args.fixtures, SPEC)
        p = os.path.join(base, "A-touch", m + ".jsonl")
        if not os.path.exists(p):
            sys.exit("missing fixture " + p)
        sc, sci = Script(), Script()
        exp = a_touch(read(p), sc, sci)
        out = run(args.bin, None, sc)
        out.update(run(args.bin, args.content, sci))
        compare(rep, exp, out)
        p = os.path.join(base, "A-shape", m + ".jsonl")
        if os.path.exists(p):
            sc = Script()
            exp, ran = a_shape(read(p), sc)
            if ran:
                shapes += 1
                compare(rep, exp, run(args.bin, args.content, sc))
        p = os.path.join(base, "A-order", m + ".jsonl")
        if os.path.exists(p):
            sc, cases, lump, ranked = a_order(read(p, ("pair", "order", "touched", "ent")), args.content)
            tally = {}
            pairs = a_order_compare(rep, m, cases, lump, ranked, run(args.bin, args.content, sc), tally)
            order_pairs.append((m, len(pairs), tally))
    tot = {}
    for _m, n, tally in order_pairs:
        for k, x in list(tally.items()) + [("pairs", n)]:
            tot[k] = tot.get(k, 0) + x
    if order_pairs:
        rep.check("A9 pairs", not tot.get("fail"), "A-order pairs out of trank order: %d" % tot.get("fail", 0))
    for f in rep.fails:
        print("FAIL " + f)
    for t in sorted(rep.n):
        p, f = rep.n[t]
        print("%-6s %s  %d pass, %d fail" % (t, "PASS" if not f else "FAIL", p, f))
    print("A5 maps with a non-box brush trigger: %d" % shapes)
    for m, n, tally in order_pairs:
        print("A9 %s: %d pairs: %s" % (m, n, ", ".join("%s %d" % kv for kv in sorted(tally.items()))))
    if order_pairs:
        print("A9 A-order total: %s" % ", ".join("%s %d" % kv for kv in sorted(tot.items())))
    return 1 if rep.fails else 0


if __name__ == "__main__":
    sys.exit(main())
