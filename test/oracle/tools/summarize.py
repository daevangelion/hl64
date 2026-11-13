#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Coverage and spec checks for GoldSrc probe fixtures.

Reads $HL64_FIXTURES/goldsrc/<spec>/<capture>/<map>.jsonl (written by the
probe, test/oracle/README.md) and prints, per spec test id, how many
records each map contributed, followed by the checks of spec statements
that the captures can confirm or contradict. Exit status 0; the report is
for people, the fixtures are for the hl64 tests.

usage: test/oracle/tools/summarize.py [fixtures-root]
"""
import collections
import glob
import json
import math
import os
import sys

ROOT = os.path.join(sys.argv[1] if len(sys.argv) > 1 else
                    os.environ.get("HL64_FIXTURES", "/scratch/hl64/fixtures"), "goldsrc")


def load(spec, cap):
    out = {}
    for p in sorted(glob.glob(os.path.join(ROOT, spec, cap, "*.jsonl"))):
        with open(p) as f:
            out[os.path.basename(p)[:-6]] = [json.loads(l) for l in f]
    return out


checks = []  # (spec test, statement, verdict, evidence)
observations = set()  # measured facts no spec statement covers


def check(test, statement, ok, evidence):
    checks.append((test, statement, "confirmed" if ok else "CONTRADICTED", evidence))


cov = collections.OrderedDict()  # test id -> {map: count}


def count(test, m, n):
    cov.setdefault(test, collections.Counter())[m] += n


# ---------------------------------------------------------------- traces
pts = load("trace-and-hulls", "T-points")
ent_contents = collections.Counter()
for m, L in pts.items():
    rows = [r for r in L if "p" in r]
    count("T1", m, len(rows))
    for r in rows:
        if r["src"] == "b":
            ent_contents[(r["c"], r.get("tpc"))] += 1
diff = {k: v for k, v in ent_contents.items() if k[0] != k[1]}
check("T1 / 1", "pfnPointContents is world-only (brush entities do not contribute)",
      not diff, f"points inside contents brush entities, (engine, world true) -> count: {dict(diff) or dict(ent_contents)}")

segs = load("trace-and-hulls", "T-segments")
alls = load("trace-and-hulls", "T-allsolid")
standoff = collections.Counter()
allsolid_end = collections.Counter()
pmrule = collections.Counter()
for src, name in ((segs, "T2"), (alls, "T9")):
    for m, L in src.items():
        rows = [r for r in L if "hull" in r]
        count(name, m, len(rows))
        for r in rows:
            w = r["world"]
            if not w["ss"] and 0 < w["fr"] < 1:
                d = sum(a * b for a, b in zip(w["n"], w["end"])) - w["d"]
                standoff["1/32" if abs(d - 1 / 32) < 1e-3 else "backed-off" if d > 1 / 32 else "other"] += 1
            if w["as"]:
                allsolid_end[(w["fr"] == 1, w["end"] == r["e"])] += 1
            if "pm" in r:
                p = r["pm"]
                count("PM3", m, 1)
                pmrule[(bool(p["as"]) <= bool(p["ss"]), (not p["ss"]) or p["fr"] == 0)] += 1
check("T3", "impact end points stand 1/32 u off the plane (or back off further at acute corners)",
      standoff["other"] == 0, dict(standoff))
check("T9 / 3", "all-solid traces report fraction 1 and end = end point",
      set(allsolid_end) <= {(True, True)}, {str(k): v for k, v in allsolid_end.items()})
check("PM3", "pmove player trace: allsolid => startsolid, startsolid => fraction 0",
      set(pmrule) <= {(True, True)}, {str(k): v for k, v in pmrule.items()})

ents = load("trace-and-hulls", "T-entities")
for m, L in ents.items():
    count("T5", m, sum(1 for r in L if "fn" in r and not r["cfg"].startswith("hullsel")))
    count("T6", m, sum(1 for r in L if "fn" in r and r["cfg"].startswith("hullsel")))
rot = load("trace-and-hulls", "T-rotated")
for m, L in rot.items():
    count("T7", m, sum(1 for r in L if "fn" in r))
hb = load("trace-and-hulls", "T-hitbox")
groups = collections.Counter()
for m, L in hb.items():
    count("T10", m, sum(1 for r in L if r.get("fn") == "line"))
    for r in L:
        if r.get("fn") == "line" and r["hit"] > 0:
            groups[r["hg"]] += 1
check("T10", "line traces against studio monsters are refined to hitboxes (iHitgroup set)",
      len(groups) > 1, f"hitgroup histogram {dict(sorted(groups.items()))}")

# 4.2: plane distance of brush entity impacts in world space (end point 1/32 in front)
pd = collections.Counter()
for src, label in ((ents, "unrotated"), (rot, None)):
    for m, L in src.items():
        bsp = {r["ent"]["i"]: r["ent"]["a"] for r in L if "angles" in r}
        for r in L:
            if "ss" not in r or r["ss"] or not 0 < r["fr"] < 1 or r["hit"] <= 0:
                continue
            if label is None and r["hit"] not in bsp:
                continue
            if label and not r["cfg"].startswith("hullsel_ref"):
                continue
            kind = label or ("unrotated" if bsp[r["hit"]] == [0, 0, 0] else "rotated")
            off = sum(a * b for a, b in zip(r["n"], r["end"])) - r["d"]
            pd[(kind, "world" if abs(off - 1 / 32) < 1e-3 else "not world")] += 1
for kind in ("unrotated", "rotated"):
    w, nw = pd[(kind, "world")], pd[(kind, "not world")]
    if w + nw:
        check("T2 / 4.2", f"{kind} brush entity impacts report the plane distance in world space (local + normal.o)",
              nw == 0, f"{w} world-space, {nw} not (GoldSrc leaves flPlaneDist in the model's frame)")

# T5 filters: expected hit on the target box for the centre line (offset 0,0)
t5 = collections.defaultdict(set)
for m, L in ents.items():
    setups = [r for r in L if "setup" in r and r["setup"] != "hullsel_door"]
    if not setups:
        continue
    B, P = setups[0]["ents"][0]["i"], setups[0]["ents"][1]["i"]
    spot = next(r["spot"] for r in L if "spot" in r)
    for r in L:
        if r.get("fn") != "line" or r["cfg"].startswith(("hullsel", "rot")):
            continue
        off = (round(r["s"][1] - spot[1], 3), round(r["s"][2] - spot[2], 3))
        t5[(r["cfg"], r["mode"], "self" if r["pass"] == B else "P" if r["pass"] == P else "none", off)].add(r["hit"] == B)
expect = {
    ("bbox", 0, "none", (0, 0)): True, ("bbox", 1, "none", (0, 0)): False,
    ("not", 0, "none", (0, 0)): False, ("trigger", 0, "none", (0, 0)): False,
    ("bbox", 0, "self", (0, 0)): False, ("owned_by_pass", 0, "P", (0, 0)): False,
    ("owner_of_pass", 0, "P", (0, 0)): False, ("glass", 0x100, "none", (0, 0)): False,
    ("glass", 0, "none", (0, 0)): True, ("bbox", 0x100, "none", (0, 0)): True,
    ("monster", 2, "none", (0, 31)): True, ("monster", 2, "none", (0, 31.5)): False,
    ("bbox", 2, "none", (0, 20)): False, ("zero_size", 0, "none", (0, 0)): False,
}
for k, want in expect.items():
    got = t5.get(k)
    if got is not None:
        check("T5", f"cfg={k[0]} mode={k[1]:#x} pass={k[2]} offset={k[3]} hits={want}", got == {want}, sorted(got))

# ---------------------------------------------------------------- links and touches
at = load("area-links-and-touch", "A-touch")
a4 = collections.Counter()
for m, L in at.items():
    lb = {r["linkbox"]: r["ent"] for r in L if "linkbox" in r}
    count("A1", m, len(lb))
    cases = {r["case"]: r for r in L if "name" in r}
    mover_idx = next((r["mover"]["i"] for r in L if r.get("setup") == "touch"), -1)
    touches = collections.defaultdict(list)
    for r in L:
        if r.get("ev") == "touch":
            touches[r["case"]].append(r)
    count("A3", m, sum(1 for c in cases.values() if c["case"] < 1000))
    count("A7", m, sum(1 for c in cases.values() if c["case"] >= 1000))
    for name, e in lb.items():
        pad = [e["o"][i] + e["mins"][i] - e["absmin"][i] for i in range(3)]
        if name.startswith("box"):
            want = [15, 15, 1] if "item" in name else [1, 1, 1]
            check("A1 / 1", f"{m}: linked box of {name} = origin+mins/maxs widened by {want}",
                  all(abs(a - b) < 1e-3 for a, b in zip(pad, want)), f"widening {pad}")
        elif name.startswith("brush_bsp_"):
            half = [(e["absmax"][i] - e["absmin"][i]) / 2 - 1 for i in range(3)]
            per = [max(abs(e["mins"][i]), abs(e["maxs"][i])) for i in range(3)]
            want = math.sqrt(sum(x * x for x in per))
            check("A1 / 1.1", f"{m}: rotated SOLID_BSP linked cube half-size = |per-axis max(|mins|,|maxs|)| = {want:.2f}",
                  all(abs(h - want) < 0.01 for h in half),
                  f"half-size {half} (+1 widening) = largest per-axis max {max(per)}; per-axis max {per}")
    for k, c in cases.items():
        if k >= 1000:
            continue
        movers = {mover_idx, 1}  # the probe mover, the fake client
        phys = [t for t in touches[k] if t["phase"] in ("physics", "pmove") and t["other"] in movers]
        so = [t for t in touches[k] if t["phase"] == "set_origin"]
        if c["kind"] in (0, 1, 6):  # fly, item, bot
            check("A3", f"{m}: {c['name']} touches iff the box gap <= 2 (both boxes widened 1)",
                  bool(phys) == (c["gap"] <= 2.0), f"{len(phys)} touches")
        a4[bool(so)] += 1
    imp = {k: c for k, c in cases.items() if k >= 1000}
    for k, c in imp.items():
        seq = ["A" if t["self"] == c["a"] else "B" for t in touches[k]]
        if c["name"] in ("fly_into_box", "toss_onto_box", "bounce_into_box", "fly_self_not"):
            check("A7", f"{m}: {c['name']} impact order A.touch(B) then B.touch(A)", seq[:2] == ["A", "B"], seq)
        if c["name"] == "fly_solid_not":
            check("A7", f"{m}: SOLID_NOT mover never calls its own touch", "A" not in seq, seq)
        if c["name"] == "mover_removes_self":
            check("A7", f"{m}: B.touch(A) skipped once A removed itself in A.touch(B)", seq == ["A"], seq)

check("A4", "set_origin into a trigger touches nothing (the next physics move or player move does)",
      a4[True] == 0 and a4[False] > 0, f"{a4[False]} cases without, {a4[True]} with set_origin touches")

sh = load("area-links-and-touch", "A-shape")
for m, L in sh.items():
    var = {r["variant"]: r["trigger"]["i"] for r in L if "trigger" in r and "variant" in r}
    samples = {r["case"]: r for r in L if "in_hull1" in r}
    count("A5", m, len(samples))
    hit = collections.defaultdict(set)
    for r in L:
        if r.get("ev") == "touch" and r["phase"] == "physics" and r["case"] in samples:
            if r["self"] == var.get(samples[r["case"]]["variant"]):
                hit[r["case"]].add(1)
    for v in var:
        rows = [s for s in samples.values() if s["variant"] == v]
        agree = sum(1 for s in rows if bool(hit.get(s["case"])) == bool(s["in_hull1_unrotated"]))
        check("A5", f"{m}: brush trigger (variant {v}, yaw {0 if v == 0 else 30}) fires iff the mover origin is inside "
              "its hull-1 brush (unrotated shape)", agree == len(rows), f"{agree}/{len(rows)} samples agree")
        inbox = sum(1 for s in rows if s["box_overlap"] and not hit.get(s["case"]))
        if v == 0:
            check("A5", f"{m}: overlapping the bounds but outside the brush does not fire", inbox > 0,
                  f"{inbox} in-bounds samples did not fire")

# ---------------------------------------------------------------- pmove
pm = load("pmove-host", "PM-bot")
kinds = collections.Counter()
for m, L in pm.items():
    hdr = {r["run"]: r for r in L if "kind" in r}
    step_ok, last = {}, None  # a {"step"} line precedes its run's header
    for r in L:
        if "step" in r:
            last = r
        elif "kind" in r:
            step_ok[r["run"]] = last is not None and "top_z" in last
            last = None
    frames = collections.defaultdict(list)
    for r in L:
        if "f" in r:
            frames[r["run"]].append(r)
    for k, h in hdr.items():
        F = frames[k]
        kinds[(h["kind"], h["msec"])] += 1
        count("PM2", m, len(F))
        sp = [math.hypot(r["v"][0], r["v"][1]) for r in F]
        if h["kind"] == "run" and h["msec"] == 10:
            t99 = next((r["f"] for r in F if math.hypot(*r["v"][:2]) >= 0.99 * 320), None)
            check("PM2 / 5", f"{m}: run caps at 320 and reaches 99 % in < 0.5 s",
                  abs(max(sp) - 320) < 0.01 and t99 is not None and (t99 + 1) * 10 < 500, f"max {max(sp):.3f}, frame {t99}")
        if h["kind"] == "standjump":
            dz = max(r["o"][2] for r in F) - h["start"][2]
            # a ceiling stops the rise early: vz drops from > 50 to <= 0 in one frame
            capped = any(a["v"][2] > 50 and b["v"][2] <= 0 for a, b in zip(F, F[1:]))
            if not capped:
                check("PM2 / 5", f"{m}: standing jump apex 45 u (msec {h['msec']})", abs(dz - 45) < 0.5, f"apex {dz:.3f}")
        if h["kind"] == "duck" and h["msec"] == 10:
            b = min(r["f"] for r in F if r["cmd"]["b"] & 4)
            d = min(r["f"] for r in F if r["fl"] & (1 << 14))
            check("PM2 / 5", f"{m}: ducking on the ground switches hulls after 0.4 s", abs((d - b) * 10 - 400) <= 10,
                  f"{(d - b) * 10} ms")
        if h["kind"] == "ladder_level":
            check("PM2 / 5", f"{m}: ladder climb speed 200 u/s facing it level", abs(max(r["v"][2] for r in F) - 200) < 0.5,
                  f"max vz {max(r['v'][2] for r in F):.3f}")
        if h["kind"] == "ladder":
            observations.add(f"ladder climb at view pitch -45: max vz {max(r['v'][2] for r in F):.3f} (msec {h['msec']})")
        if h["kind"] == "water":
            swim = [s for s, r in zip(sp, F) if r["wl"] >= 2]
            check("PM2 / 5", f"{m}: swimming speed <= 0.8 x maxspeed (msec {h['msec']})", max(swim or [0]) <= 256.01,
                  f"max {max(swim or [0]):.3f}")
        if h["kind"] in ("step18", "step19") and step_ok.get(k):
            dz = max(r["o"][2] for r in F) - h["start"][2]
            want = 18 if h["kind"] == "step18" else 0
            check("PM2 / 5", f"{m}: {h['kind']} {'climbed' if want else 'not climbed'} without jumping",
                  abs(dz - want) < 0.1, f"max rise {dz:.3f}")

# ---------------------------------------------------------------- report
maps = sorted({m for c in cov.values() for m in c})
print("| test | " + " | ".join(maps) + " | total |")
print("|---|" + "---|" * (len(maps) + 1))
for t, c in cov.items():
    print(f"| {t} | " + " | ".join(str(c.get(m, 0)) for m in maps) + f" | {sum(c.values())} |")
print()
print("PM-bot runs (kind, msec) -> maps:", dict(sorted(kinds.items())))
print()
for test, stmt, verdict, ev in checks:
    print(f"[{verdict}] {test}: {stmt} -- {ev}")
print()
for o in sorted(observations):
    print(f"[observed] {o}")
