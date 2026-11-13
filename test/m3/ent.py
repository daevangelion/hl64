#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Entity-layer tests (docs/specs/think-and-time.md 7) on the host driver
test/m3/ent.c (`make enttest` -> hl64-ent):

  K1-K3  K-think scenarios 0-7 replayed with the probe's thinkers on the
         game clock rebuilt from map load (time 1.0, two settle frames,
         then each frame's recorded frame time): every think (frame, slot,
         time seen, nextthink) and every recorded nextthink must match.
         Scenarios 8-9 (multi_manager) are K5, logic timers (slice Rec).
  K6     K-alloc: the start snapshot seeded (free slots with their free
         times and serials; slot indices compressed in order, the cap is
         495), then the probe's allocations, frees and FL_KILLME: every
         allocation's slot / serial / free time / cleared fields and every
         free-list snapshot must match.
  K8     K-settle: the first 40 frames' time and frame time; a think due
         at 1.0 runs in the first settle frame, one due at 1.005 in frame 3.
  selftest  K7 reserves, heap / active-set unit test, P13 counters, K9
         step-all equivalence (+ a control that must fail), private slabs.
  frames    two open layers: clip / query / delta in both frames.

Usage: PYTHONPATH=. test/m3/ent.py --bin BUILD/host/hl64-ent [--content DIR]
       [--fixtures DIR] [--maps a,b] [--data VALVE] (repo root on PYTHONPATH:
       the PVS check decodes the BSP vis lump with tools/w64c/w64check.py)
Exit 0 when everything passes.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

MAPS = "hldemo1,hldemo2,hldemo3,t0a0,t0a0a,t0a0b,t0a0b1,t0a0b2,t0a0c,t0a0d"


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fmt(x):
    return "%.9g" % f32(x)


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f]


def run(binary, script, root=None):
    cmd = [binary] + (["--root", root] if root else [])
    p = subprocess.run(cmd, input="\n".join(script) + "\n", capture_output=True, text=True)
    if p.returncode not in (0, 1) or "FAIL script" in p.stdout:
        sys.exit(f"{binary}: exit {p.returncode}\n{p.stdout[-2000:]}\n{p.stderr[-2000:]}")
    return p.stdout.splitlines()


def frame_numbers(frames, first_t):
    """Global StartFrame numbers of the scenario frame records: frames 1
    and 2 settle at 1.0, then 0.01 s frames until the first record's time
    (f32 compare), then one frame per record except where a scenario's
    first record shares the previous record's StartFrame."""
    T, n = 1.0, 3
    while f32(T) < f32(first_t):
        T += f32(0.01)
        n += 1
    out, prev = [], None
    for r in frames:
        if prev is not None and not (r["f"] == 0 and r["sc"] != prev["sc"]):
            n += 1
        out.append(n)
        prev = r
    return out


# ---------------------------------------------------------------- K-think

TH_LOG, TH_RESCHED, TH_SET_OTHER = 0, 1, 2


def kthink(binary, path):
    recs = load(path)
    scen = {}
    for r in recs:
        if "scenario" in r and "name" in r:
            scen[r["scenario"]] = r["name"]
    keep = {s for s, name in scen.items() if not name.startswith("multi_manager")}
    frames = [r for r in recs if "f" in r and "sc" in r and r["sc"] in keep]
    nums = frame_numbers(frames, frames[0]["t"])
    # actions per frame, in record order
    per = {}
    cur_sc, cur_frame = None, nums[0]
    owned = {}
    setter = {}
    want_think, want_state = [], []
    fi = 0
    for r in recs:
        sc = r.get("sc", r.get("scenario"))
        if "scenario" in r and "name" in r:
            if r["scenario"] not in keep:
                break
            cur_sc = r["scenario"]
            cur_frame = nums[fi] if fi < len(frames) else cur_frame
            acts = per.setdefault(cur_frame, [])
            for s, ents in owned.items():
                acts.extend(f"free {i}" for i in ents)
            owned = {cur_sc: []}
            continue
        if sc is not None and sc not in keep:
            continue
        if "f" in r and "sc" in r:
            cur_frame = nums[fi]
            fi += 1
            acts = per.setdefault(cur_frame, [])
            for e in r["e"]:
                if not e.get("free"):
                    acts.append(f"state {e['i']}")
                    want_state.append((cur_frame, e["i"], fmt(e["nt"])))
            continue
        acts = per.setdefault(cur_frame, [])
        name = scen.get(cur_sc, "")
        if "ents" in r and "sc" in r:
            for e in r["ents"]:
                i = e["i"]
                mode, period, count, other = TH_LOG, 0.0, 0, 0
                if name.startswith("resched012"):
                    mode, period = TH_RESCHED, 0.12
                elif name == "self_now":
                    mode, count = TH_RESCHED, 5
                elif setter.get(cur_sc, (None,))[0] == i:
                    mode, other = TH_SET_OTHER, setter[cur_sc][1]
                acts.append(f"thinker {i} {mode} {fmt(period)} {count} {other}")
                owned[cur_sc].append(i)
        elif "setter" in r:
            setter[cur_sc] = (r["setter"], r["other"])
        elif r.get("ev") == "schedule":
            i = r["i"]
            if i not in owned[cur_sc]:
                mode = TH_RESCHED if r["what"] == "now_resched_0.12" else TH_LOG
                acts.append(f"thinker {i} {mode} {fmt(0.12 if mode else 0)} 0 0")
                owned[cur_sc].append(i)
            acts.append(f"nt {i} {fmt(r['nt'])}")
        elif r.get("ev") == "think":
            want_think.append(("think", r["frame"], r["self"], fmt(r["t"]), fmt(r["nt"])))
        elif r.get("ev") == "set_nextthink":
            want_think.append(("set", r["self"], r["other"], fmt(r["nt"])))
    # the script: every frame from map load to the last record
    last = nums[-1]
    dts = {}
    for r, n in zip(frames, nums):
        dts[n] = r["ft"]
    script = ["newgame"]
    for n in range(1, last + 1):
        script.append(f"frame {fmt(dts.get(n, 0.01))}")
        script.extend(per.get(n, []))
        if n < last:
            script.append("pass")
    out = run(binary, script)
    got_think, got_state = [], []
    frame = 0
    for line in out:
        w = line.split()
        if w[0] == "think":
            got_think.append(("think", int(w[1]), int(w[2]), fmt(float(w[3])), fmt(float(w[4]))))
        elif w[0] == "set":
            got_think.append(("set", int(w[1]), int(w[2]), fmt(float(w[3]))))
        elif w[0] == "state":
            got_state.append((int(w[1]), w[2]))
    fails = []
    n_think = len(want_think)
    for k in range(max(len(want_think), len(got_think))):
        a = want_think[k] if k < len(want_think) else None
        b = got_think[k] if k < len(got_think) else None
        if a != b:
            fails.append(f"event {k}: want {a} got {b}")
            break
    ws = [(i, nt) for (_, i, nt) in want_state]
    gs = [(i, fmt(float(nt))) if nt != "free" else (i, nt) for (i, nt) in got_state]
    bad_state = sum(1 for a, b in zip(ws, gs) if a != b) + abs(len(ws) - len(gs))
    if bad_state:
        k = next((k for k, (a, b) in enumerate(zip(ws, gs)) if a != b), min(len(ws), len(gs)))
        fails.append(f"nextthink {k}: want {ws[k] if k < len(ws) else None} "
                     f"got {gs[k] if k < len(gs) else None}")
    names = [scen[s] for s in sorted(keep)]
    return fails, n_think, len(ws), names


# ---------------------------------------------------------------- K-alloc

def kalloc(binary, path):
    recs = load(path)
    frames = [r for r in recs if "f" in r and "sc" in r]
    nums = frame_numbers(frames, frames[0]["t"])
    start = next(r for r in recs if r.get("snapshot") == "start")
    free0 = sorted(start["free"])
    top = max([start["highest_used"]] + [e[0] for e in free0]) + 1
    # map entities that free themselves during the capture (slot used at
    # the start, free later, not freed by the probe): seeded live with the
    # serial before their free, freed on the frame of their free time
    probe_freed = {r["i"] for r in recs if isinstance(r.get("free"), str) or "killme" in r}
    known = {e[0] for e in free0}
    mapfree = {}
    for r in recs:
        if isinstance(r.get("free"), list):
            for i, ft, s in r["free"]:
                if i < top and i not in known and i not in probe_freed:
                    mapfree.setdefault(i, (ft, s))
    low = sorted(known | set(mapfree))
    remap = {i: 2 + k for k, i in enumerate(low)}
    top_c = 2 + len(low)

    def m(i):
        if i in remap:
            return remap[i]
        if i < top:
            raise SystemExit(f"{path}: slot {i} below the top {top} and never free")
        c = top_c + (i - top)
        if c >= 495:
            raise SystemExit(f"{path}: compressed slot {c} past the cap")
        return c

    tframe = {fmt(r["t"]): n for r, n in zip(frames, nums)}

    per, want = {}, []
    allocated = []
    cur = None
    fi = 0
    setup_frame = nums[0]
    last_free, since = None, set()
    n_map = 0
    for r in recs:
        if "f" in r and "sc" in r:
            cur = nums[fi]
            fi += 1
            continue
        n = cur if cur is not None else setup_frame
        acts = per.setdefault(n, [])
        if "snapshot" in r:
            now = {e[0]: e for e in r["free"]}
            if last_free is not None:
                # slots taken since the last snapshot by the map's own
                # entities (not the probe): allocated here, just before
                # this snapshot, and they must be the slots the rule picks
                for i in sorted(set(last_free) - set(now) - since):
                    _, ft, s = last_free[i]
                    acts.append("alloc")
                    want.append(("alloc", m(i), s, fmt(ft), (fmt(0),) * 3, 0))
                    n_map += 1
            last_free, since = now, set()
            if r["snapshot"] == "start":
                continue
            acts.append("freelist")
            want.append(("freelist", sorted((m(i), fmt(ft), s) for i, ft, s in r["free"])))
        elif "alloc" in r:
            acts.append("alloc")
            since.add(r["i"])
            allocated.append(m(r["i"]))
            want.append(("alloc", m(r["i"]), r["serial"], fmt(r["freetime"]),
                         tuple(fmt(x) for x in r["o"]), r["fl"]))
        elif "free" in r and isinstance(r["free"], str):
            acts.append(f"dirty {m(r['i'])}")
            acts.append(f"free {m(r['i'])}")
        elif "killme" in r:
            acts.append(f"remove {m(r['i'])}")
    for i, (ft, s) in mapfree.items():
        per.setdefault(tframe[fmt(ft)], []).insert(0, f"free {m(i)}")
    # frame 79: every allocation freed (a reused slot once), before "end"
    end_frame = nums[[k for k, r in enumerate(frames) if r["f"] == 79][0]]
    acts = per[end_frame]
    frees = [f"freeif {i}" for i in dict.fromkeys(allocated)]
    k = acts.index("freelist")
    per[end_frame] = acts[:k] + frees + acts[k:]
    script = ["newgame"]
    last = nums[-1]
    for n in range(1, last + 1):
        script.append(f"frame {fmt(0.01)}")
        if n == setup_frame:
            for i, ft, s in free0:
                script.append(f"seedfree {m(i)} {fmt(ft)} {s}")
            for i, (ft, s) in mapfree.items():
                script.append(f"seedused {m(i)} {s - 1}")
        script.extend(per.get(n, []))
        script.append("pass")
    out = run(binary, script)
    got = []
    for line in out:
        w = line.split()
        if w[0] == "alloc":
            got.append(("alloc", int(w[1]), int(w[2]), fmt(float(w[3])),
                        tuple(fmt(float(x)) for x in w[4:7]), int(w[7])))
        elif w[0] == "freelist":
            lst = []
            for e in w[2:]:
                i, ft, s = e.split(":")
                lst.append((int(i), fmt(float(ft)), int(s)))
            got.append(("freelist", sorted(lst)))
    fails = []
    for k in range(max(len(want), len(got))):
        a = want[k] if k < len(want) else None
        b = got[k] if k < len(got) else None
        if a != b:
            fails.append(f"record {k}: want {str(a)[:300]} got {str(b)[:300]}")
            break
    n_alloc = sum(1 for w in want if w[0] == "alloc")
    n_snap = sum(1 for w in want if w[0] == "freelist")
    return fails, n_alloc, n_snap, n_map


# ---------------------------------------------------------------- K-settle

def ksettle(binary, path):
    recs = load(path)
    early = [r for r in recs if "early" in r]
    script = ["newgame", "thinker 300 0 0 0 0", "nt 300 1", "thinker 301 0 0 0 0", "nt 301 1.005"]
    for r in early:
        script += [f"frame {fmt(0.01)}", "clock", "pass"]
    out = run(binary, script)
    clocks = [l.split()[1:] for l in out if l.startswith("clock")]
    thinks = [l.split() for l in out if l.startswith("think")]
    fails = []
    for r, c in zip(early, clocks):
        if (fmt(r["t"]), fmt(r["ft"])) != (fmt(float(c[0])), fmt(float(c[1]))):
            fails.append(f"frame {r['frame']}: want t {fmt(r['t'])} ft {fmt(r['ft'])} got {c}")
            break
    if len(clocks) != len(early):
        fails.append(f"{len(clocks)} clock lines for {len(early)} frames")
    want_th = [["think", "1", "300", "1", "0"], ["think", "3", "301", fmt(1.005), "0"]]
    if [[t[0], t[1], t[2], fmt(float(t[3])), t[4]] for t in thinks] != want_th:
        fails.append(f"settle thinks {thinks}, want {want_th}")
    return fails, len(early)


def pvs_check(binary, content, data, mp, n=4000):
    """ent_point_in_pvs over the engine's VISR rows against the map's own
    BSP vis lump (Valve data, decoded by tools/w64c/w64check.py): random
    eye / point pairs in the world bounds (leaves are the engine's point
    -> leaf). Needs the repo root on PYTHONPATH."""
    import random
    from pathlib import Path
    from tools.w64c.w64check import Bsp

    bsp = Bsp(Path(data) / "maps" / (mp + ".bsp"))
    lo, hi = [float(x) for x in bsp.mraw[0][0:3]], [float(x) for x in bsp.mraw[0][3:6]]
    rng = random.Random(mp)
    # points in open leaves (the engine's point -> leaf), then random pairs
    pts = [[rng.uniform(lo[k], hi[k]) for k in range(3)] for _ in range(6 * n)]
    probe = ["pvs %s %s" % (mp, " ".join(fmt(x) for x in q + q)) for q in pts]
    leaves = [int(l.split()[1]) for l in run(binary, probe, content) if l.startswith("pvs ")]
    open_pts = [q for q, lf in zip(pts, leaves) if lf > 0]
    if len(open_pts) < 2:
        return [f"{len(open_pts)} points in open leaves"], 0, 0, 0
    script = []
    for _ in range(n):
        e, p = rng.choice(open_pts), rng.choice(open_pts)
        script.append("pvs %s %s" % (mp, " ".join(fmt(x) for x in e + p)))
    out = [l.split() for l in run(binary, script, content) if l.startswith("pvs ")]
    fails, rows, vis = [], 0, 0
    for k, (_, le, lp, v) in enumerate(out):
        le, lp, v = int(le), int(lp), int(v)
        if lp <= 0:
            want = 0
        else:
            row = bsp.pvs_row(le) if 0 < le <= bsp.visleafs else None
            rows += row is not None
            want = 1 if row is None or lp > bsp.visleafs else (row[(lp - 1) >> 3] >> ((lp - 1) & 7)) & 1
        vis += v
        if v != want:
            fails.append(f"pair {k}: leaves {le} -> {lp} got {v} want {want}")
            break
    if len(out) != n:
        fails.append(f"{len(out)} answers for {n} pairs")
    return fails, n, rows, vis


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content")
    ap.add_argument("--fixtures", default=os.path.join(
        os.environ.get("HL64_FIXTURES", "/scratch/hl64/fixtures"), "goldsrc"))
    ap.add_argument("--maps", default=MAPS)
    ap.add_argument("--data", default="/scratch/hl1/valve",
                    help="Valve data (maps/*.bsp: the PVS oracle)")
    a = ap.parse_args()
    tt = os.path.join(a.fixtures, "think-and-time")
    bad = 0
    totals = {"think": 0, "state": 0, "alloc": 0, "snap": 0, "settle": 0}
    for mp in a.maps.split(","):
        f, nt, ns, names = kthink(a.bin, os.path.join(tt, "K-think", mp + ".jsonl"))
        totals["think"] += nt
        totals["state"] += ns
        print(f"K1-K3 {mp}: {nt} think/set events, {ns} nextthinks"
              + (" FAIL " + "; ".join(f) if f else " ok"))
        bad += bool(f)
        f, na, nsn, nmap = kalloc(a.bin, os.path.join(tt, "K-alloc", mp + ".jsonl"))
        totals["alloc"] += na
        totals["snap"] += nsn
        print(f"K6 {mp}: {na} allocations ({nmap} by map entities), {nsn} free-list snapshots"
              + (" FAIL " + "; ".join(f) if f else " ok"))
        bad += bool(f)
        f, ne = ksettle(a.bin, os.path.join(tt, "K-settle", mp + ".jsonl"))
        totals["settle"] += ne
        print(f"K8 {mp}: {ne} frames" + (" FAIL " + "; ".join(f) if f else " ok"))
        bad += bool(f)
    out = run(a.bin, ["selftest"])
    print("\n".join(out))
    bad += "selftest ok" not in out
    if a.content:
        out = run(a.bin, ["frames hldemo1 hldemo2"], a.content)
        print("\n".join(out))
        bad += not any(l.startswith("frames ok") for l in out)
        for mp in ("hldemo1", "t0a0"):
            f, n, rows, vis = pvs_check(a.bin, a.content, a.data, mp)
            print(f"pvs {mp}: {n} pairs ({rows} with a vis row, {vis} visible) against the BSP "
                  "vis lump" + (" FAIL " + "; ".join(f) if f else " ok"))
            bad += bool(f)
    print(f"ent: K1-K3 {totals['think']} events + {totals['state']} nextthinks, K6 "
          f"{totals['alloc']} allocations + {totals['snap']} snapshots, K8 {totals['settle']} "
          f"frames on {len(a.maps.split(','))} maps: " + ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
