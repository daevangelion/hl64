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
bad = {k: v for k, v in ent_contents.items() if (k[0] != k[1] and k[0] != -16) or (k[1] == -2 and k[0] != -2)}
check("T1 / 1", "pfnPointContents in a func_ladder's bounds is the world's contents or LADDER (-16), SOLID where the world is SOLID",
      not bad, f"(engine, world true) -> count: {dict(ent_contents)}")

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
        check("T2 / 4.2", f"{kind} brush entity impacts report flPlaneDist in the model's frame (not world space)",
              nw > 0 and w <= 0.01 * (w + nw), f"{nw} model-frame, {w} equal to world space (planes through the model origin)")

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
            want = [1, 1, 1]
            check("A1 / 1", f"{m}: linked box of {name} = origin+mins/maxs widened by {want}",
                  all(abs(a - b) < 1e-3 for a, b in zip(pad, want)), f"widening {pad}")
        elif name.startswith("brush_bsp_"):
            half = [(e["absmax"][i] - e["absmin"][i]) / 2 - 1 for i in range(3)]
            per = [max(abs(e["mins"][i]), abs(e["maxs"][i])) for i in range(3)]
            want = max(per)
            check("A1 / 1.1", f"{m}: rotated SOLID_BSP linked cube half-size = largest per-axis max(|mins|,|maxs|) = {want:.2f}",
                  all(abs(h - want) < 0.01 for h in half),
                  f"half-size {half} (+1 widening); per-axis max {per}")
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

# ---------------------------------------------------------------- A-order (live)
ao = load("area-links-and-touch", "A-order")
consistent, inconsistent, higher_first, pairs_total, cycles = 0, [], 0, 0, []
flips, toggled, runtime = collections.Counter(), collections.Counter(), []
for m, L in ao.items():
    orders = collections.defaultdict(set)
    for r in L:
        if "place" in r and "order" in r:
            orders[(r["phase"], tuple(r["pair"]))].add(tuple(r["order"]))
    static = {p: o for (ph, p), o in orders.items() if ph in (1, 3)}
    count("A9 (A-order pairs ordered)", m, len(static))
    count("A9 (A-order placements)", m, sum(1 for r in L if "place" in r))
    for p, o in static.items():
        pairs_total += 1
        if len(o) == 1:
            consistent += 1
            a, b = next(iter(o))
            higher_first += a > b
        else:
            inconsistent.append((m, p, sorted(o)))
    # a rank exists iff the graph first -> second has no cycle
    g = collections.defaultdict(set)
    for o in static.values():
        for a, b in o:
            g[a].add(b)
    state = {}

    def cyclic(u):
        state[u] = 1
        for v in g[u]:
            if state.get(v) == 1 or (v not in state and cyclic(v)):
                return True
        state[u] = 2
        return False
    if any(u not in state and cyclic(u) for u in list(g)):
        cycles.append(m)
    for (ph, p), o in orders.items():
        if ph == 4 and p in static and len(o) == 1 and len(static[p]) == 1:
            toggled[m] += 1
            flips[m] += next(iter(o)) != next(iter(static[p]))
    rt = {r["runtime"]: r for r in L if "runtime" in r}
    if "A" in rt:
        o2 = {tuple(r["pair"]): r["order"] for r in L if r.get("phase") == 2 and "order" in r and r["mover"] == "fly"}
        runtime.append(f"{m}: map {rt['B']['map_trigger']}, B {rt['A']['B']} (created first), A {rt['A']['A']}: "
                       + ", ".join(f"{a}<{b}" for a, b in sorted(set(map(tuple, o2.values())))))
check("A9", "a trigger pair is touched in the same order by every mover size, origin and the player move",
      not inconsistent, f"{consistent}/{pairs_total} pairs consistent; inconsistent: {inconsistent[:5]}")
check("A9 / 3.1", "pairwise touch orders form no cycle (a per-map touch rank exists)", not cycles,
      f"maps with a cycle: {cycles or 'none'} ({len(ao)} maps)")
observations.add(f"A-order: {higher_first} of {consistent} map trigger pairs are touched higher entity index first")
observations.add("A-order toggle phase (ToggleUse off+on of the trigger touched first): order changed for "
                 + ", ".join(f"{m} {flips[m]}/{toggled[m]}" for m in sorted(toggled)))
for r in runtime:
    observations.add(f"A-order runtime triggers (fly mover touch order): {r}")


def scen(L):
    names, states, ev, extra = {}, collections.defaultdict(list), collections.defaultdict(list), collections.defaultdict(list)
    for r in L:
        if "scenario" in r and "name" in r:
            names[r["scenario"]] = r["name"]
        elif "sc" in r and "e" in r:
            states[r["sc"]].append(r)
        elif "ev" in r and "case" in r:
            ev[r["case"]].append(r)
        elif "sc" in r:
            extra[r["sc"]].append(r)
    skipped = {r["scenario"] for r in L if "skipped" in r and "scenario" in r}
    return {k: v for k, v in names.items() if k not in skipped}, states, ev, extra


def track(states, k, i):
    return [(r["f"], r["t"], next(e for e in r["e"] if e["i"] == i)) for r in states[k] if any(e["i"] == i for e in r["e"])]


def first_ground(tr):
    return next((f for f, t, e in tr if e["fl"] & 512), None)


FL_ONGROUND = 512

# ---------------------------------------------------------------- think and time
kt = load("think-and-time", "K-think")
k1, k2, k3, k5, bnd = [], [], [], [], collections.defaultdict(list)
for m, L in kt.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("K1-K5 (thinks / uses)", m, sum(1 for e in ev.values() for x in e if x["ev"] in ("think", "use")))
    for nm, ft in (("resched012_dt001", 0.01), ("resched012_dt30", 1 / 30)):
        th = [x for x in ev[byname[nm]] if x["ev"] == "think"]
        fr = [x["frame"] - th[0]["frame"] for x in th]
        ts = [x["t"] - th[0]["t"] for x in th]
        ok_t = all(abs(t - 0.12 * i) < 2e-4 for i, t in enumerate(ts))
        if nm.endswith("dt30"):
            k1.append(fr[:5] == [0, 3, 7, 10, 14] and ok_t)
        else:
            k1.append(ok_t)
    th = lambda nm: [x for x in ev[byname[nm]] if x["ev"] == "think"]
    a = th("lower_sets_higher")
    b = th("higher_sets_lower")
    s = th("self_now")
    k2.append(len(a) == 2 and a[0]["frame"] == a[1]["frame"] and len(b) == 2 and b[1]["frame"] == b[0]["frame"] + 1
              and [x["frame"] - s[0]["frame"] for x in s] == list(range(len(s))))
    for nm in ("boundary_dt001", "boundary_dt30"):
        sched = {x["i"]: x["what"] for x in ex[byname[nm]] if x.get("ev") == "schedule"}
        t = th(nm)
        f0 = min(x["frame"] for x in t)
        bnd[nm].append(tuple(sorted((sched[x["self"]], x["frame"] - f0) for x in t)) + tuple(
            sorted((w, None) for i, w in sched.items() if i not in {x["self"] for x in t})))
    sched = {x["i"]: x for x in ex[byname["dt01"]] if x.get("ev") == "schedule"}
    t = {x["self"]: x for x in th("dt01")}
    past = next(i for i, x in sched.items() if x["what"] == "t-0.05")
    fut = next(i for i, x in sched.items() if x["what"] == "t+0.05")
    k3.append(abs(t[past]["t"] - sched[past]["t"]) < 1e-6 and abs(t[fut]["t"] - sched[fut]["nt"]) < 1e-6)
    for nm in ("multi_manager_dt001", "multi_manager_dt30"):
        fire = next(x for x in ex[byname[nm]] if x.get("ev") == "fire")
        uses = [(x["target"], round(x["t"] - fire["t"], 4)) for x in ev[byname[nm]] if x["ev"] == "use"]
        k5.append(f"{m} {nm}: {uses}")
check("K1", "a +0.12 s self-rescheduling think sees t0 + 0.12 k; at dt 1/30 it runs at frames 0, 3, 7, 10, 14", all(k1),
      f"{sum(k1)}/{len(k1)} runs")
check("K2", "nextthink = time set by a lower-index entity runs this frame, by a higher-index one next frame; own nextthink = time runs next frame",
      all(k2), f"{sum(k2)}/{len(k2)} maps")
check("K3", "a think due in the past sees the frame time; one due inside a 0.1 s frame runs that frame and sees its own time",
      all(k3), f"{sum(k3)}/{len(k3)} maps")
for nm, v in bnd.items():
    observations.add(f"K2 boundary ({nm}; frame offset of the think, None = never): {sorted(set(v))}")
for x in sorted(set(k5))[:4]:
    observations.add(f"K5 multi_manager / trigger_relay delays (target, seconds after the fire): {x}")

kp = load("think-and-time", "K-pusher")
for m, L in kp.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("K4 / P5 (pusher frames)", m, sum(len(v) for v in st.values()))
    for nm in ("door_dt001", "door_dt30"):
        if nm not in byname:
            continue
        k = byname[nm]
        thk = [x for x in ev[k] if x["ev"] == "think"]
        start = next(x for x in ex[k] if x.get("ev") == "schedule")
        pid = start["i"]
        tr = track(st, k, pid)
        z0 = tr[0][2]["o"][2]
        dz = tr[-1][2]["o"][2] - z0
        check("K4 / P5", f"{m} {nm}: a door moving 50 u at 64 u/s thinks once, ltime = nextthink, and stops 50 u up",
              len(thk) == 1 and abs(thk[0]["ltime"] - start["nt"]) < 1e-4 and abs(dz - 50) < 0.01,
              f"thinks {[(round(x['ltime'], 5), round(x['t'], 4)) for x in thk]}, nextthink {start['nt']:.5f}, moved {dz:.4f}")
    if "blocked" in byname:
        k = byname["blocked"]
        bl = [x for x in ev[k] if x["ev"] == "blocked"]
        lts = sorted({round(x["ltime"], 6) for x in bl})
        thk = [x for x in ev[k] if x["ev"] == "think"]
        check("K4 / P5", f"{m}: a blocked pusher keeps its ltime and its think waits",
              len(bl) > 0 and len(lts) == 1 and not thk, f"{len(bl)} blocked frames at ltime {lts}, thinks {len(thk)}")
    if "subframe" in byname:
        k = byname["subframe"]
        thk = [x for x in ev[k] if x["ev"] == "think"]
        observations.add(f"K-pusher subframe ({m}): nextthink ltime+0.005 -> think ltime {[round(x['ltime'], 6) for x in thk]}, "
                         f"origin z {[round(x['o'][2], 4) for x in thk]}")
    if "idle" in byname:
        tr = [r for r in st[byname["idle"]]]
        observations.add(f"K-pusher idle pusher ({m}): ltime {tr[0]['e'][0]['lt']:.4f} -> {tr[-1]['e'][0]['lt']:.4f} over {len(tr) - 1} frames")

ka = load("think-and-time", "K-alloc")
k6 = []
for m, L in ka.items():
    snaps = {r["snapshot"]: r for r in L if "snapshot" in r}
    allocs = [r for r in L if "alloc" in r and r["alloc"][0] == "f" and r["alloc"][1:].isdigit()]
    count("K6 (allocations)", m, len([r for r in L if "alloc" in r]))
    for a in allocs:
        s = snaps[a["alloc"]]
        ok = [i for i, ft, ser in s["free"] if ft < 2 or a["t"] - ft > 0.5]
        want = min(ok) if ok else None
        got = a["i"] if a["freetime"] != 0 or a["serial"] != 0 else None
        k6.append((m, a["alloc"], want, got, a["i"], a["serial"]))
bad = [x for x in k6 if x[2] != x[3]]
check("K6", "a new edict takes the lowest free slot freed before 2.0 s or more than 0.5 s ago, else a never-used slot",
      not bad, f"{len(k6) - len(bad)}/{len(k6)} allocations; mismatches (map, at, expected, got): {bad[:6]}")
reused = [r for m, L in ka.items() for r in L if "alloc" in r and (r["serial"] or r["freetime"])]
observations.add(f"K6 reused slots: serial numbers {sorted({r['serial'] for r in reused})}, "
                 f"fields zero on reuse: {all(r['o'] == [0, 0, 0] and r['fl'] == 0 and r['classname'] == 0 for r in reused)}")

ks = load("think-and-time", "K-settle")
for m, L in ks.items():
    pre = next(r for r in L if "precache_time" in r)
    early = [r for r in L if "early" in r]
    count("K8 (early frames)", m, len(early))
    land = {}
    for r in early:
        for it in r["items"]:
            if it["fl"] & FL_ONGROUND and it["i"] not in land:
                land[it["i"]] = r["early"]
    items = [r for r in L if "item" in r]
    observations.add(f"K-settle {m}: precache time {pre['precache_time']:.4f}; first StartFrames (t, ft): "
                     f"{[(round(r['t'], 4), round(r['ft'], 4)) for r in early[:3]]}; items on ground by StartFrame "
                     f"{sorted(collections.Counter(land.values()).items())} of {len(items)}")

# ---------------------------------------------------------------- physics
G = 800.0
pt = load("physics-movetypes", "P-toss")
p1 = []
for m, L in pt.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("P1/P3/P12 (P-toss frames)", m, sum(len(v) for v in st.values()))
    for nm, h in (("drop16", 16), ("drop64_spin", 64), ("drop200", 200)):
        k = byname[nm]
        tr = track(st, k, st[k][0]["e"][0]["i"])
        f = first_ground(tr)
        want = math.sqrt(2 * h / G) / 0.01
        if any(e["wl"] for _, _, e in tr):
            observations.add(f"P1 {m} {nm}: the drop passed through liquid contents {sorted({e['wt'] for _, _, e in tr if e['wl']})}; excluded")
            continue
        p1.append((m, nm, f, round(want, 2), f is not None and abs(f - want) <= 1.5))
    k = byname["onto_monster"]
    mon, box = st[k][0]["e"][0]["i"], st[k][0]["e"][1]["i"]
    last = track(st, k, box)[-1][2]
    check("P3", f"{m}: a toss box landing on a monster is not on ground", not last["fl"] & FL_ONGROUND,
          f"flags {last['fl']}, groundentity {last['ge']} (monster {mon})")
    if "conveyor" in byname:
        k = byname["conveyor"]
        ids = [e["i"] for e in st[k][0]["e"]]
        box = ids[-1]
        tr = track(st, k, box)
        g = first_ground(tr)
        dx = tr[-1][2]["o"][0] - tr[g][2]["o"][0] if g is not None else None
        check("P12", f"{m}: an item resting on a func_conveyor (speed 100) moves at the conveyor speed",
              dx is not None and abs(dx - 100 * (tr[-1][0] - g) * 0.01) < 1,
              f"moved {dx} u in {(tr[-1][0] - (g or 0)) * 0.01:.2f} s on ground; basevelocity {tr[-1][2]['bv']}")
    k = byname["basevelocity"]
    tr = track(st, k, st[k][0]["e"][0]["i"])
    vx = [e["v"][0] for f, t, e in tr if f > 0][:6]
    bvx = [e["bv"][0] for f, t, e in tr if f > 0][:6]
    check("2 (base velocity)", f"{m}: basevelocity moves the entity without being added into its velocity",
          max(vx) == 0, f"frames 1-6 velocity.x {vx}, basevelocity.x {bvx}")
bad = [x for x in p1 if not x[4]]
check("P1", "a toss dropped from rest lands after sqrt(2h/g) +- one frame and rests on the world", not bad,
      f"{len(p1) - len(bad)}/{len(p1)} drops; (map, case, landing frame, expected): {[x[:4] for x in bad[:4]] or [x[:4] for x in p1[:3]]}")

pb = load("physics-movetypes", "P-bounce")
ratios, slide, missile = collections.defaultdict(list), collections.defaultdict(set), []
for m, L in pb.items():
    names, st, ev, ex = scen(L)
    count("P2 (P-bounce frames)", m, sum(len(v) for v in st.values()))
    for k, nm in names.items():
        tr = track(st, k, st[k][0]["e"][0]["i"])
        for (f0, t0, a), (f1, t1, b) in zip(tr, tr[1:]):
            if a["v"][2] < -60 and b["v"][2] > 0:
                ratios[nm].append(round(b["v"][2] / -a["v"][2], 3))
        last = tr[-1][2]
        if last["fl"] & FL_ONGROUND:
            slide[nm].add(round(math.hypot(last["v"][0], last["v"][1]), 3))
        if nm == "bouncemissile_down":
            missile.append(ratios[nm][-1:] + [max(e["v"][2] for f, t, e in tr)])
for nm, r in sorted(ratios.items()):
    observations.add(f"P2 {nm}: rebound vz/-vz per impact (first 6) {r[:6]}")
check("P2", "BOUNCE comes to rest on the floor once slow (spec: below about 30 u/s)",
      all(v == {0.0} for nm, v in slide.items() if not nm.startswith("bounce_f0")),
      f"horizontal speed at the end while FL_ONGROUND: {dict((k, sorted(v)) for k, v in slide.items())} (started 60 u/s)")
observations.add(f"P2 BOUNCEMISSILE: rebound ratio and max vz after the floor hit (no gravity): {missile[:3]}")

ps = load("physics-movetypes", "P-step")
wl = collections.Counter()
for m, L in ps.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("P4 (P-step frames)", m, sum(len(v) for v in st.values()))
    k = byname["pushed"]
    tr = track(st, k, st[k][0]["e"][0]["i"])
    sp = [e["v"][0] for f, t, e in tr]
    rule = []
    for a, b in zip(sp[1:], sp[2:]):
        if a <= 0:
            break
        rule.append(abs(b - max(0.0, a - 0.01 * max(a, 100) * 4)) < 0.01)
    check("P4", f"{m}: a pushed STEP monster on the floor slows by ground friction (s' = s - dt * max(s, 100) * 4)",
          all(rule) and sp[-1] == 0, f"{sum(rule)}/{len(rule)} frames, first speeds {[round(x, 2) for x in sp[:4]]}")
    for nm in ("water_step", "water_toss", "water_bounce"):
        if nm in byname:
            tr = track(st, byname[nm], st[byname[nm]][0]["e"][0]["i"])
            wl[(nm, max(e["wl"] for f, t, e in tr))] += 1
if wl:
    check("5", "non-player entities in water get waterlevel 1 (never 2 or 3)", all(lv <= 1 for nm, lv in wl),
          f"(case, max waterlevel) -> maps {dict(wl)}")

# entering liquid: vertical velocity after the transition frame / before it (+ that frame's gravity)
entry = collections.Counter()
for spec_cap in (("physics-movetypes", "P-toss"), ("physics-movetypes", "P-step"), ("trace-and-hulls", "W-water")):
    for m, L in load(*spec_cap).items():
        names, st, ev, ex = scen(L)
        for k in names:
            for i in {e["i"] for r in st[k] for e in r["e"] if e.get("mt") in (4, 6, 10)}:
                tr = track(st, k, i)
                for (fa, ta, a), (fb, tb, b) in zip(tr, tr[1:]):
                    if a["wl"] == 0 and b["wl"] > 0 and a["v"][2] < -50:
                        entry[(spec_cap[1], names[k], round(b["v"][2] / (a["v"][2] - 8), 2))] += 1
observations.add(f"5: entering liquid, vz(after) / (vz(before) - g dt): {dict(sorted(entry.items()))}")

pp = load("physics-movetypes", "P-push")
for m, L in pp.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("P6/P8 (P-push frames)", m, sum(len(v) for v in st.values()))
    if "lift" in byname:
        k = byname["lift"]
        crate = st[k][0]["e"][-1]["i"]
        tr = track(st, k, crate)
        rise = tr[-1][2]["o"][2] - tr[30][2]["o"][2]
        check("P6", f"{m}: a crate on a lift rises with it (50 u)", abs(rise - 50) < 0.1, f"rose {rise:.4f}")
    if "door_side" in byname:
        k = byname["door_side"]
        door = next(e for e in st[k][0]["e"] if e["mt"] == 7)["i"]
        crate = st[k][0]["e"][-1]["i"]
        tr, dt = track(st, k, crate), track(st, k, door)
        dx = tr[-1][2]["o"][0] - tr[0][2]["o"][0]
        gap = tr[-1][2]["o"][0] - 12 - (dt[-1][2]["absmax"][0] - 1)
        check("P6", f"{m}: a crate beside a door is pushed along and ends against the door's face (door 100 u, gap 4)",
              dx > 90 and abs(gap) < 0.01, f"crate moved {dx:.4f}, final gap to the door face {gap:.4f}")
        observations.add(f"P6 {m}: crate 4 u in front of a door moving 100 u ends {dx:.4f} u further (pushed by each frame's move once the door overlaps it)")
    for nm in ("door_block_rider", "monster_block", "down_onto_crate"):
        if nm in byname:
            bl = [x for x in ev[byname[nm]] if x["ev"] == "blocked"]
            frames = sorted({x["frame"] for x in bl})
            check("P6", f"{m} {nm}: the jammed pusher's blocked runs once per blocked frame",
                  len(bl) > 0 and len(bl) == len(frames), f"{len(bl)} calls in {len(frames)} frames")
    if "corpse" in byname:
        k = byname["corpse"]
        c = next(e for e in st[k][0]["e"] if e["solid"] == 1)["i"]
        tr = track(st, k, c)
        bl = [x for x in ev[k] if x["ev"] == "blocked"]
        check("P8", f"{m}: a SOLID_TRIGGER crate jammed by a door collapses to zero size instead of blocking",
              not bl and tr[-1][2]["mins"] == tr[-1][2]["maxs"], f"blocked {len(bl)}, final mins {tr[-1][2]['mins']} maxs {tr[-1][2]['maxs']}")

pr = load("physics-movetypes", "P-rotate")
for m, L in pr.items():
    names, st, ev, ex = scen(L)
    if not names:
        continue
    k = 0
    count("P7 (P-rotate frames)", m, len(st[k]))
    ids = [e["i"] for e in st[k][0]["e"]]
    pid, rider = ids[0], ids[1]
    p = track(st, k, pid)
    r = track(st, k, rider)
    bot = [x for x in ex[k] if "bot" in x]
    on = [e for f, t, e in r if f == 30]
    if not on or on[0]["ge"] != pid:
        observations.add(f"P7 {m}: the crate did not land on the rotating pusher (groundentity {on[0]['ge'] if on else None}); not judged")
        continue
    dyaw_p = p[-1][2]["a"][1] - p[0][2]["a"][1]
    dyaw_r = r[-1][2]["a"][1] - r[0][2]["a"][1]
    check("P7", f"{m}: a crate riding a rotating pusher turns with it (yaw follows); the client's view yaw does not",
          abs(dyaw_p - dyaw_r) < 0.5 and (not bot or abs(bot[-1]["bot"]["v_angle"][1] - bot[0]["bot"]["v_angle"][1]) < 0.01),
          f"pusher yaw {dyaw_p:.3f}, crate yaw {dyaw_r:.3f}, client v_angle yaw {[round(b['bot']['v_angle'][1], 3) for b in (bot[:1] + bot[-1:])]}, "
          f"client angles yaw {[round(b['bot']['a'][1], 3) for b in (bot[:1] + bot[-1:])]}, client on pusher "
          f"{any(b['bot']['ge'] == pid for b in bot)}")

pa = load("physics-movetypes", "P-pushable")
for m, L in pa.items():
    names, st, ev, ex = scen(L)
    count("P9 (P-pushable frames)", m, sum(len(v) for v in st.values()))
    for k, nm in names.items():
        push = next((x for x in ex[k] if x.get("ev") == "push"), None)
        if not push:
            continue
        tr = track(st, k, push["i"])
        after = [(f, e) for f, t, e in tr if f > 10]
        sp = [round(math.hypot(e["v"][0], e["v"][1]), 2) for f, e in after]
        rest = next((f for f, e in after if math.hypot(e["v"][0], e["v"][1]) == 0), None)
        observations.add(f"P9 {m} {nm}: speed after push frames 1-5 {sp[:5]}, at rest from frame {rest}, "
                         f"final z drop {round(tr[-1][2]['o'][2] - tr[0][2]['o'][2], 3)}")

pw = load("physics-movetypes", "P-walkmove")
for m, L in pw.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    count("P10/P11 (walk_move / drop_to_floor calls)", m, sum(1 for r in L if "wm" in r or "drop" in r or "mto" in r))
    drops = {r["drop"]: r["ret"] for r in L if "drop" in r}
    observations.add(f"P11 {m}: drop_to_floor (height above floor -> code) {drops}")
    stairs = [r for r in L if r.get("wm") == "stairs"]
    if stairs:
        rises = [round(b["to"][2] - a["to"][2], 3) for a, b in zip(stairs, stairs[1:]) if b["ret"]]
        observations.add(f"P10 {m}: walk_move up world stairs (8 u per call): {sum(r['ret'] for r in stairs)}/{len(stairs)} succeeded, "
                         f"rise {stairs[-1]['to'][2] - stairs[0]['from'][2]:.3f}, per-call rises {sorted(set(rises))}")
    steps = [r for r in L if r.get("wm") in ("step", "step_dist")]
    if steps:
        observations.add(f"P10 {m}: walk_move into an 18/19 u probe brush-entity step (sc 1: 18, sc 2: 19; ret, climbed) "
                         f"{dict(collections.Counter((r['sc'], r['ret'], r['to'][2] > r['from'][2] + 1) for r in steps))}")
    if steps:
        check("P10", f"{m}: walk_move steps up onto an 18 u probe brush-entity step",
              any(r["sc"] == 1 and r["to"][2] > r["from"][2] + 1 for r in steps),
              "never climbed (world stairs are climbed: see observations)")

# ---------------------------------------------------------------- contents and light
ww = load("trace-and-hulls", "W-water")
for m, L in ww.items():
    names, st, ev, ex = scen(L)
    byname = {v: k for k, v in names.items()}
    pts = [r for r in ex.get(byname.get("func_water_points"), []) if "p" in r]
    count("W (func_water points)", m, len(pts))
    c = collections.Counter((r["c"], r["pmc"], r["pmtc"], r["tpc"]) for r in pts)
    observations.add(f"W-water {m}: func_water point contents (engine, pmove, pmove true, pmove world) -> count {dict(c)}")
    sk = [(r["skin"], r["solid"], r["c_centre"]) for r in ex.get(byname.get("skins"), []) if "skin" in r]
    observations.add(f"W-water {m}: probe brush (skin, solid, contents at centre) {sk}")
    for nm in ("func_water_toss", "func_water_step", "func_water_bot"):
        if nm in byname:
            k = byname[nm]
            i = st[k][0]["e"][-1]["i"]
            tr = track(st, k, i)
            observations.add(f"W-water {m} {nm}: final waterlevel {tr[-1][2]['wl']} watertype {tr[-1][2]['wt']}")

li = load("lightstyles", "L-illum")
vals = collections.Counter()
for m, L in li.items():
    for r in L:
        if "illum" in r:
            vals.update(r["illum"])
        if "bot_illum" in r:
            vals[("client", r["bot_illum"], r["light_level"])] += 1
    count("L (illum samples)", m, sum(len(r["illum"]) for r in L if "illum" in r))
observations.add(f"L-illum: pfnGetEntityIllum values over every point, frame, style change and EF_BRIGHTLIGHT: {dict(vals)}")
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
