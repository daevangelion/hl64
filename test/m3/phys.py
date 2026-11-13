#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Physics tests against the GoldSrc probe fixtures (docs/specs/physics-movetypes.md 8, K4).

Every scenario of P-toss, P-bounce, P-step, P-push, P-rotate, P-pushable,
P-walkmove, K-pusher and W-water (toss / step into a func_water) is rebuilt
on the cooked map from its records and replayed through the real movetype
steps (test/m3/phys.c): the map's brush entities in their spawn state
(`brush_ents`), the scenario's entities (`ents`) with their frame-0
state, the probe's per-frame actions (pusher starts, pushes, schedules,
the bot's zero moves) before the frames they precede, at the capture's
frame time. Each frame's state is compared for every tracked entity:

  origin and velocity +-TOL, angles +-0.01, flags, ground entity, water
  level and type, size exact, ltime / nextthink +-1e-6 (pushers);
  thinks (frame, time, ltime) and blocked calls (frame, other) exactly.

Walk moves, drops and move_to_origin calls are replayed one by one from
the probe's own before-state: result, end origin, flags and ground; the
probe's move_to_origin calls that left the straight line (the engine
random stream picked the direction) are checked as "a successful compass
step of the given distance" when the probe's succeeded, else as a failure.

P3 (slides) and P13 (resting counters) are the harness self-tests;
P15 runs `pair` with SOLID_BSP + MOVETYPE_TOSS and expects the fatal
error naming the class.

  phys.py --bin BUILD/host/hl64-phys --content CONTENT --fixtures FIXTURES
          [--maps hldemo1 ...] [--verbose]

Exit 0 when every check passes; prints per-test counts.
"""
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

TOL = 0.001
ANG_TOL = 0.01
TIME_TOL = 1e-6

TEST_OF = {
    ("P-toss", "drop16"): "P1", ("P-toss", "drop64_spin"): "P1", ("P-toss", "drop200"): "P1",
    ("P-toss", "arc"): "P1", ("P-toss", "fly_down"): "P1",
    ("P-toss", "onto_monster"): "P3",
    ("P-toss", "conveyor"): "P12", ("P-toss", "basevelocity"): "P12",
    ("P-step", "fall"): "P4", ("P-step", "pushed"): "P4", ("P-step", "ledge48"): "P4",
    ("P-step", "fly"): "P4",
    ("P-step", "water_step"): "P14", ("P-step", "water_toss"): "P14",
    ("P-step", "water_bounce"): "P14",
    ("P-push", "corpse"): "P8",
    ("W-water", "func_water_toss"): "P14", ("W-water", "func_water_step"): "P14",
}
DEFAULT_TEST = {"P-bounce": "P2", "P-push": "P6", "P-rotate": "P7", "P-pushable": "P9",
                "P-walkmove": "P10", "K-pusher": "K4"}
CAPTURES = [("physics-movetypes", c) for c in
            ("P-toss", "P-bounce", "P-step", "P-push", "P-rotate", "P-pushable", "P-walkmove")]
CAPTURES += [("think-and-time", "K-pusher"), ("trace-and-hulls", "W-water")]
FL_ONGROUND = 512


def load(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def vec(v):
    return " ".join(repr(float(x)) for x in v)


def f32(x):
    import struct
    return struct.unpack("f", struct.pack("f", x))[0]


class Scenario:
    def __init__(self, hdr):
        self.hdr = hdr
        self.name = hdr.get("name")
        self.setup = []      # setup records in order
        self.ents = []
        self.frames = []     # state records
        self.ticks = {}      # f -> [record] (probe actions after state f)
        self.events = []     # (frame, record): touches / thinks / blocks during frame f
        self.bot = {}        # f -> bot record (P-rotate)


def parse(recs):
    scen = {}
    cur = None
    for r in recs[3:]:
        if "scenario" in r:
            cur = Scenario(r)
            scen[r["scenario"]] = cur
            continue
        if cur is None:
            continue
        if "ev" in r and "case" in r:
            f = cur.frames[-1]["f"] + 1 if cur.frames else 0
            cur.events.append((f, r))
        elif "e" in r and "f" in r:
            cur.frames.append(r)
        elif "bot" in r:
            cur.bot[r["f"]] = r["bot"]
        elif "ents" in r:
            cur.ents = r["ents"]
            cur.setup.append(r)
        elif "ev" in r:
            f = cur.frames[-1]["f"] if cur.frames else -1
            cur.ticks.setdefault(f, []).append(r)
        else:
            cur.setup.append(r)
    return scen


# GoldSrc entity indices -> hl64 slots (ascending order kept; 0, 1 fixed)
SLOT = {}


def build_slots(recs):
    idx = {e["i"] for e in recs[1].get("brush_ents", [])}
    for r in recs[3:]:
        for e in r.get("ents", []):
            idx.add(e["i"])
        for e in r.get("e", []):
            idx.add(e["i"])
    order = sorted(i for i in idx if i > 1)
    if len(order) + 2 > 495:
        raise SystemExit(f"{len(order)} entities over the slot cap")
    SLOT.clear()
    SLOT.update({i: 2 + n for n, i in enumerate(order)})
    SLOT.update({0: 0, 1: 1, -1: -1})


def sl(i):
    return SLOT.get(i, -2)


def ent_line(e, probe):
    m = e.get("model") or ""
    sub = int(m[1:]) if m.startswith("*") else -1
    own = sl(e["owner"]) if e["owner"] > 1 else e["owner"]
    return (f"ent {sl(e['i'])} {sub} {vec(e['o'])} {vec(e['a'])} {vec(e['mins'])} {vec(e['maxs'])} "
            f"{e['solid']} {e['movetype']} {e['flags']} {e['skin']} {e['rendermode']} "
            f"{own} {1 if probe else 0}")


def dyn_line(d, grav=0.0, fric=0.0):
    return (f"dyn {sl(d['i'])} {vec(d['o'])} {vec(d['a'])} {vec(d['v'])} {vec(d['av'])} {vec(d['bv'])} "
            f"{d['fl']} {sl(d['ge'])} {d['lt']!r} {d['nt']!r} {grav!r} {fric!r}")


# probe-set entvars the state lines do not carry (probe_phys.cpp / probe_time.cpp)
def extras(cap, sc, i):
    if cap == "P-bounce":
        fr = {"bounce_f0": 0, "bounce_f055": 0.55, "bounce_f08": 0.8, "bounce_f1": 1.0,
              "bounce_vert_f08": 0.8, "grenade_g05_f08": 0.8, "bounce_box_f08": 0.8}
        return (0.5 if sc.name == "grenade_g05_f08" else 0.0), f32(fr.get(sc.name, 0.0))
    if cap == "P-step" and sc.name == "water_bounce":
        return 0.0, f32(0.8)
    if cap == "P-pushable":
        if sc.name == "pushstep_f1":
            return 0.0, 1.0
        for s in sc.setup:
            if "map_pushable" in s and s["map_pushable"] == i:
                return 0.0, s["friction"]
    return 0.0, 0.0


def tick_lines(cap, sc, recs):
    out = []
    for r in recs:
        ev = r.get("ev")
        if ev == "pusher_start":
            out.append(f"setvel {sl(r['i'])} {vec(r['v'])} {vec(r['av'])} {r['nt']!r}")
            out.append(f"think {sl(r['i'])} 3 0 0")
        elif ev == "push":
            out.append(f"setvel {sl(r['i'])} {vec(r['v'])} 0 0 0 none")
        elif ev == "schedule" and cap == "K-pusher":
            v = {"door_dt001": (0, 0, 64), "door_dt30": (0, 0, 64), "blocked": (100, 0, 0),
                 "subframe": (0, 0, 64)}.get(sc.name)
            if sc.name == "think_zero_velocity":
                out.append(f"setvel {sl(r['i'])} 0 0 0 0 0 0 {r['nt']!r}")
                out.append(f"think {sl(r['i'])} 1 0.25 4")
            else:
                out.append(f"setvel {sl(r['i'])} {vec(v)} 0 0 0 {r['nt']!r}")
                out.append(f"think {sl(r['i'])} 3 0 0")
    return out


def close(a, b, tol):
    return all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def ang_close(a, b):
    for x, y in zip(a, b):
        d = (float(x) - float(y)) % 360.0
        if min(d, 360.0 - d) > ANG_TOL:
            return False
    return True


def compare_state(want, got, is_player):
    """List of mismatching fields."""
    bad = []
    w = want
    g = got
    if not close(w["o"], g["o"], TOL):
        bad.append(f"o {g['o']} want {w['o']}")
    if not close(w["v"], g["v"], TOL):
        bad.append(f"v {g['v']} want {w['v']}")
    fl_mask = FL_ONGROUND if is_player else 0xFFFFFFFF
    if (w["fl"] & fl_mask) != (g["fl"] & fl_mask):
        bad.append(f"fl {g['fl']} want {w['fl']}")
    if w["ge"] != g["ge"] and not (is_player and not (w["fl"] & FL_ONGROUND)):
        bad.append(f"ge {g['ge']} want {w['ge']}")
    if is_player:
        return bad
    if not ang_close(w["a"], g["a"]):
        bad.append(f"a {g['a']} want {w['a']}")
    if not close(w["av"], g["av"], TOL) or not close(w["bv"], g["bv"], TOL):
        bad.append(f"av/bv {g['av']}/{g['bv']} want {w['av']}/{w['bv']}")
    if abs(w["lt"] - g["lt"]) > TIME_TOL or abs(w["nt"] - g["nt"]) > TIME_TOL:
        bad.append(f"lt/nt {g['lt']}/{g['nt']} want {w['lt']}/{w['nt']}")
    if w["frame"] > 0 and (w["wl"] != g["wl"] or w["wt"] != g["wt"]):
        bad.append(f"wl/wt {g['wl']}/{g['wt']} want {w['wl']}/{w['wt']}")
    if w["mins"] != g["mins"] or w["maxs"] != g["maxs"]:
        bad.append(f"size {g['mins']}{g['maxs']} want {w['mins']}{w['maxs']}")
    return bad


def parse_state(tok):
    i = int(tok[1])
    if tok[2] == "free":
        return i, None
    x = [float(t) for t in tok[2:]]
    return i, {"o": x[0:3], "a": x[3:6], "v": x[6:9], "av": x[9:12], "bv": x[12:15],
               "fl": int(x[15]), "ge": int(x[16]), "lt": x[17], "nt": x[18], "wl": int(x[19]),
               "wt": int(x[20]), "mins": x[21:24], "maxs": x[24:27]}


def frame_scenario(cap, sc, brush_cmds, mp):
    """Script for a frame scenario; returns (script, tracked ids)."""
    out = [f"map {mp}"] + brush_cmds
    f0 = sc.frames[0]
    out.append(f"clock {f0['t']!r}")
    ids = []
    bot = False
    for e in sc.ents:
        if e["i"] == 1:
            continue
        out.append(ent_line(e, True))
    for d in f0["e"]:
        ids.append(sl(d["i"]))
        if d["i"] == 1:
            out.append(f"player {vec(d['o'])} {d['fl']} {sl(d['ge'])}")
            bot = True
            continue
        g, fr = extras(cap, sc, d["i"])
        out.append(dyn_line(d, g, fr))
    if cap == "P-toss" and sc.name == "conveyor":
        for e in sc.ents:
            if e["flags"] & 4:
                out.append(f"conveyor {sl(e['i'])} 100 0 0")
    out.append("track " + " ".join(str(i) for i in ids))
    for prev, fr in zip(sc.frames, sc.frames[1:]):
        out += tick_lines(cap, sc, sc.ticks.get(fr["f"] - 1, []))
        out.append(f"clock {prev['t']!r}")
        out.append(f"frame {prev['ft']!r} {1 if bot else 0}")
    for e in sc.ents:
        if e["i"] != 1:
            out.append(f"free {sl(e['i'])}")
    return out, ids


def run(binp, content, script):
    p = subprocess.run([binp, "--root", content], input="\n".join(script) + "\n",
                       capture_output=True, text=True)
    return p


def check_frames(cap, sc, ids, lines, verbose):
    """(frames compared, failures list, event failures)."""
    got_frames = []
    cur = {}
    ev = []
    nf = 0
    for l in lines:
        t = l.split()
        if not t:
            continue
        if t[0] == "s":
            i, st = parse_state(t)
            cur[i] = st
        elif t[0] == "end":
            nf += 1
            got_frames.append(cur)
            cur = {}
        elif t[0] == "ev":
            ev.append((nf + 1, t[1:]))
    fails = []
    for k, fr in enumerate(sc.frames[1:]):
        if k >= len(got_frames):
            fails.append(f"f{fr['f']}: no output")
            break
        g = got_frames[k]
        for w in fr["e"]:
            if w.get("free"):
                if g.get(sl(w["i"])) is not None:
                    fails.append(f"f{fr['f']} i{w['i']}: live, want freed")
                continue
            gs = g.get(sl(w["i"]))
            if gs is None:
                fails.append(f"f{fr['f']} i{w['i']}: freed")
                continue
            w = dict(w)
            w["frame"] = fr["f"]
            w["ge"] = sl(w["ge"])
            bad = compare_state(w, gs, w["i"] == 1)
            if bad:
                fails.append(f"f{fr['f']} i{w['i']}: " + "; ".join(bad))
    # thinks and blocks: the probe entities' own (frame, self[, other])
    want_ev = sorted((f, r["ev"], sl(r["self"]), sl(r.get("other", -1))) for f, r in sc.events
                     if r["ev"] in ("think", "blocked"))
    got_ev = sorted((f, t[0], int(t[1]), int(t[2]) if t[0] == "blocked" else -1)
                    for f, t in ev if t[0] in ("think", "blocked"))
    evfail = []
    if want_ev != got_ev:
        evfail.append(f"events {got_ev[:6]} want {want_ev[:6]} ({len(got_ev)} vs {len(want_ev)})")
    for f, r in sc.events:
        if r["ev"] == "think" and "ltime" in r:
            for gf, t in ev:
                if gf == f and t[0] == "think" and int(t[1]) == sl(r["self"]):
                    if abs(float(t[3]) - r["ltime"]) > TIME_TOL or abs(float(t[2]) - r["t"]) > 1e-4:
                        evfail.append(f"think f{f}: t {t[2]} ltime {t[3]} want {r['t']} {r['ltime']}")
    ntouch_w = sum(1 for _, r in sc.events if r["ev"] == "touch")
    ntouch_g = sum(1 for _, t in ev if t[0] == "touch")
    return len(sc.frames) - 1, fails, evfail, (ntouch_g, ntouch_w)


def walk_scenario(sc, brush_cmds, mp, floor):
    """Script and expected results for the P-walkmove ops of one scenario."""
    out = [f"map {mp}"] + brush_cmds
    mon = None
    enemy = None
    for e in sc.ents:
        out.append(ent_line(e, True))
        if e["movetype"] == 4:
            mon = sl(e["i"])
        elif e["solid"] == 0 and e["movetype"] == 0:
            enemy = sl(e["i"])
    want = []
    water = None
    for s in sc.setup:
        if "water" in s:
            water = s["water"]
    for s in sc.setup:
        if "wm" in s:
            if enemy is not None:
                if s["wm"] == "fly_enemy_above":
                    out.append(f"setorg {enemy} {vec([floor[0] + 64, floor[1], floor[2] + 200])}")
                    out.append(f"enemy {mon} {enemy}")
                elif s["wm"] == "fly_enemy_below":
                    out.append(f"setorg {enemy} {vec([floor[0] + 64, floor[1], floor[2] + 8])}")
                    out.append(f"enemy {mon} {enemy}")
                elif s["wm"] == "fly_no_enemy":
                    out.append(f"enemy {mon} -1")
                elif s["wm"].startswith("swim") and water:
                    out.append(f"setorg {enemy} {vec([water[0] + 32, water[1], water[2] + 100])}")
                    out.append(f"enemy {mon} {enemy}")
            out.append(f"state {mon} {vec(s['from'])} {s['fl_before']} {sl(s['ge'])}")
            out.append(f"walk {mon} {s['yaw']!r} {s['dist']!r} {s['mode']}")
            want.append(("wm", s))
        elif "settle" in s:
            out.append(f"state {mon} {vec(s['settle'])} {s['fl'] & ~FL_ONGROUND} {sl(s['ge'])}")
            out.append(f"drop {mon}")
            want.append(("drop", s))
        elif "drop" in s:
            out.append(f"state {mon} {vec(s['from'])} 32 -1")
            out.append(f"drop {mon}")
            want.append(("drop", s))
        elif "mto" in s:
            out.append(f"state {mon} {vec(s['from'])} {s['fl']} 0")
            out.append(f"mto {mon} {vec(s['goal'])} 8")
            want.append(("mto", s))
    for e in sc.ents:
        out.append(f"free {sl(e['i'])}")
    return out, want


def check_walk(want, lines):
    res = [l.split() for l in lines if l.split()[:1] in (["wm"], ["drop"], ["mto"])]
    fails = []
    counts = {"P10": [0, 0], "P11": [0, 0]}
    for k, (kind, w) in enumerate(want):
        if k >= len(res):
            fails.append(("P10", f"op {k}: no output"))
            break
        g = res[k]
        test = "P11" if kind == "drop" else "P10"
        ok = True
        detail = ""
        if kind == "wm" or kind == "drop":
            ret = int(g[1])
            o = [float(x) for x in g[2:5]]
            fl, ge = int(g[5]), int(g[6])
            if ret != w["ret"] or not close(o, w["to"], TOL) or fl != w["fl"] or ge != sl(w["ge"]):
                ok = False
                detail = (f"{w.get('wm', 'drop')} ret {ret} to {o} fl {fl} ge {ge}; want ret "
                          f"{w['ret']} to {w['to']} fl {w['fl']} ge {w['ge']}")
        else:
            o = [float(x) for x in g[2:5]]
            fr, to, goal = w["from"], w["to"], w["goal"]
            moved_w = math.hypot(to[0] - fr[0], to[1] - fr[1])
            gx, gy = goal[0] - fr[0], goal[1] - fr[1]
            gl = math.hypot(gx, gy) or 1
            straight = moved_w > 0 and abs((to[0] - fr[0]) * gy - (to[1] - fr[1]) * gx) / gl < 1e-3 \
                and (to[0] - fr[0]) * gx + (to[1] - fr[1]) * gy > 0
            if straight:
                ok = close(o, to, TOL)
            elif moved_w > 0:
                d = math.hypot(o[0] - fr[0], o[1] - fr[1])
                a = math.degrees(math.atan2(o[1] - fr[1], o[0] - fr[0])) % 45
                ok = abs(d - 8) < 1e-3 and min(a, 45 - a) < 1e-3
            else:
                ok = close(o, fr, TOL)
            if not ok:
                detail = f"mto from {fr} to {o}; probe to {to} (straight {straight})"
        counts[test][0 if ok else 1] += 1
        if not ok:
            fails.append((test, detail))
    return counts, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--fixtures", required=True, type=Path)
    ap.add_argument("--maps", nargs="*")
    ap.add_argument("--captures", nargs="*")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    maps = a.maps or sorted(p.stem for p in (a.fixtures / "physics-movetypes" / "P-toss").glob("*.jsonl"))
    tot = {}
    frames = 0
    fails = 0
    touches = [0, 0]

    def count(test, ok):
        tot.setdefault(test, [0, 0])[0 if ok else 1] += 1

    for spec, cap in CAPTURES:
        if a.captures and cap not in a.captures:
            continue
        for mp in maps:
            path = a.fixtures / spec / cap / f"{mp}.jsonl"
            if not path.exists():
                continue
            recs = load(path)
            if "brush_ents" not in recs[1] or "floor" not in recs[2]:
                continue
            build_slots(recs)
            brush = [ent_line(e, False) for e in recs[1]["brush_ents"]]
            floor = recs[2]["floor"]
            for k, sc in sorted(parse(recs).items()):
                if "skipped" in sc.hdr:
                    continue
                if cap == "W-water" and sc.name not in ("func_water_toss", "func_water_step"):
                    continue
                test = TEST_OF.get((cap, sc.name), DEFAULT_TEST.get(cap))
                tag = f"{cap} {mp} {k} {sc.name}"
                if cap == "P-walkmove":
                    script, want = walk_scenario(sc, brush, mp, floor)
                    p = run(a.bin, a.content, script)
                    if p.returncode or "FAIL" in p.stdout:
                        fails += 1
                        print(f"{test} FAIL {tag}: harness rc {p.returncode}: "
                              f"{(p.stdout + p.stderr)[-600:]}")
                        continue
                    counts, wf = check_walk(want, p.stdout.splitlines())
                    for t, (ok, bad) in counts.items():
                        if ok or bad:
                            tot.setdefault(t, [0, 0])
                            tot[t][0] += ok
                            tot[t][1] += bad
                    for t, d in wf:
                        fails += 1
                        print(f"{t} FAIL {tag}: {d}")
                    continue
                if len(sc.frames) < 2:
                    continue
                script, ids = frame_scenario(cap, sc, brush, mp)
                p = run(a.bin, a.content, script)
                if p.returncode or "FAIL" in p.stdout:
                    fails += 1
                    count(test, False)
                    print(f"{test} FAIL {tag}: harness rc {p.returncode}: {(p.stdout + p.stderr)[-600:]}")
                    continue
                n, ff, evf, (tg, tw) = check_frames(cap, sc, ids, p.stdout.splitlines(), a.verbose)
                frames += n
                touches[0] += tg
                touches[1] += tw
                ok = not ff and not evf
                count(test, ok)
                if cap == "K-pusher":
                    count("P5", ok)
                if not ok:
                    fails += 1
                    print(f"{test} FAIL {tag}: {len(ff)}/{n} frames; " + "; ".join((ff[:3] + evf[:2])))
                elif a.verbose:
                    print(f"{test} ok   {tag}: {n} frames, touches {tg}/{tw}")
    # self-tests (P3 slides, P13 counters) on each map's floor spot; P15
    for mp in maps:
        path = a.fixtures / "physics-movetypes" / "P-toss" / f"{mp}.jsonl"
        recs = load(path)
        if "floor" not in recs[2]:
            continue
        fl = recs[2]["floor"]
        p = run(a.bin, a.content, [f"map {mp}", f"selftest {vec([fl[0], fl[1], fl[2] + 0.03125])}"])
        ok = "selftest ok" in p.stdout and p.returncode == 0
        for t in ("P3", "P13"):
            count(t, ok and f"{t} done" in p.stdout)
        if not ok:
            fails += 1
            print(f"selftest FAIL {mp}:\n  " + "\n  ".join(p.stdout.splitlines()[-12:]))
        elif a.verbose:
            print(f"selftest ok {mp}: " + " ".join(l for l in p.stdout.splitlines() if "counters" in l))
        p = run(a.bin, a.content, [f"map {mp}",
                                   "ent 300 -1 0 0 0 0 0 0 -8 -8 -8 8 8 8 0 6 0 0 0 -1 0",
                                   "pair 300 4 6"])
        ok = p.returncode != 0 and "pair ok" not in p.stdout and "SOLID_BSP" in (p.stdout + p.stderr)
        count("P15", ok)
        if not ok:
            fails += 1
            print(f"P15 FAIL {mp}: rc {p.returncode} {(p.stdout + p.stderr)[-300:]}")
    print(f"phys: {frames} frames, touches {touches[0]} (probe {touches[1]}), "
          + ", ".join(f"{t} {v[0]} ok / {v[1]} fail" for t, v in
                      sorted(tot.items(), key=lambda kv: int(kv[0][1:])))
          + ": " + ("PASS" if not fails else f"FAIL ({fails})"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
