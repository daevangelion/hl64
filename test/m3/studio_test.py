#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""studio_test.py -- the game's studio animation API (game/studio/anim.h)
and the engine's studio residency on cooked models (hl64-studiotest):
sequence lookup, activities, sequence info, animation events, frame
advance, event windows, body groups and controllers of the scientist,
checked against expectations computed here from the cooked info block with
the HLSDK's documented rules (dlls/animation.cpp, animating.cpp).

  python3 test/m3/studio_test.py --bin BUILD/host/hl64-studiotest \\
      --content DIR [--model models/scientist]

DIR holds models/<x>.s64 / .a64 (tools/studio/studiocook.py). Exit 0 when
every check passes.
"""
import argparse
import math
import os
import struct
import subprocess
import sys

SEQ = ">HBBfIhHHHHBBI3f3f3f2H2f2fBxHII"
LOOPING = 1
EVENT_CLIENT = 5000


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


class Info:
    def __init__(self, data):
        io = 64
        h = struct.unpack_from(">IBBBBHBBI15fHH9I3I", data, io)
        self.numbones, self.numctl = h[1], h[2]
        self.numseq, self.numbodyparts = h[5], h[6]
        ctl_o, seq_o, ev_o, bp_o, str_o = h[27], h[30], h[31], h[32], h[34]

        def string(o):
            a = io + str_o + o
            return data[a:data.index(b"\0", a)].decode("latin-1")
        self.seqs = []
        for k in range(self.numseq):
            v = struct.unpack_from(SEQ, data, io + seq_o + 96 * k)
            s = {"label": string(v[0]), "fps": v[3], "flags": v[4], "activity": v[5],
                 "actweight": v[6], "numframes": v[7], "numevents": v[8], "event_first": v[9],
                 "linear": v[13:16], "events": []}
            for e in range(s["event_first"], s["event_first"] + s["numevents"]):
                fr, ev, opt, res = struct.unpack_from(">iiHH", data, io + ev_o + 12 * e)
                s["events"].append((fr, ev, string(opt), res))
            self.seqs.append(s)
        self.ctls = [struct.unpack_from(">bBHffB", data, io + ctl_o + 16 * k)
                     for k in range(self.numctl)]
        self.bodyparts = [struct.unpack_from(">HHH", data, io + bp_o + 8 * k)
                          for k in range(self.numbodyparts)]


class Report:
    def __init__(self):
        self.n = self.bad = 0

    def check(self, ok, what):
        self.n += 1
        if not ok:
            self.bad += 1
            print("FAIL", what)


def run(binary, content, cmds):
    p = subprocess.run([binary, content], input="\n".join(cmds) + "\n", capture_output=True,
                       text=True)
    if p.returncode:
        sys.stderr.write(p.stdout[-2000:] + p.stderr[-2000:])
        raise SystemExit("hl64-studiotest exit %d" % p.returncode)
    return [l[2:].split(" ") for l in p.stdout.splitlines() if l.startswith("R ")]


def close(a, b, tol=1e-4):
    return abs(a - b) <= tol * max(1.0, abs(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--model", default="models/scientist")
    a = ap.parse_args()
    with open(os.path.join(a.content, a.model + ".s64"), "rb") as f:
        info = Info(f.read())
    rep = Report()
    cmds = ["B 0 %s.s64 %s.a64" % (a.model, a.model)]
    seqs = info.seqs
    labels = [s["label"] for s in seqs]

    # sequence lookup: first case-insensitive match, -1 absent
    for lab in sorted(set(labels)):
        cmds.append("L 0 %s" % lab)
        cmds.append("L 0 %s" % lab.upper())
    cmds.append("L 0 no_such_sequence")
    # activities
    acts = sorted({s["activity"] for s in seqs if s["activity"] > 0})
    for act in acts:
        cmds.append("H 0 %d" % act)
        cmds.append("Y 0 %d 16" % act)
    cmds.append("H 0 9999")
    for k in range(len(seqs)):
        cmds.append("I 0 %d" % k)
    cmds.append("I 0 %d" % len(seqs))
    # events in a few windows of every sequence that has them
    windows = [(0, 256), (0, 64), (64, 200), (200, 260), (-1, 10)]
    evseqs = [k for k, s in enumerate(seqs) if s["events"]]
    for k in evseqs:
        for w in windows:
            cmds.append("V 0 %d %g %g" % (k, w[0], w[1]))
    # frame advance on a looping and a non-looping sequence
    walk = labels.index("walk")
    once = next(k for k, s in enumerate(seqs) if not s["flags"] & LOOPING and s["numframes"] > 10)
    cmds += ["N 0 %d" % walk, "T 1.0", "R 2", "T 1.1", "A 2 0", "A 2 0", "T 1.25", "W 2",
             "A 2 0.5", "A 2 3.0", "A 2 7.5"]
    cmds += ["N 0 %d" % once, "T 2.0", "R 3", "A 3 0.25", "A 3 30", "W 3"]
    # body groups, controllers
    cmds += ["G 2 1 2", "G 2 1 3", "G 2 1 99", "G 2 0 0"]
    for c in range(info.numctl):
        cmds += ["K 0 %d 30" % info.ctls[c][1], "K 0 %d -400" % info.ctls[c][1]]
    cmds += ["F", "S"]
    res = run(a.bin, a.content, cmds)
    it = iter(res)

    def nxt(tag):
        r = next(it)
        assert r[0] == tag, (tag, r)
        return r[1:]

    for lab in sorted(set(labels)):
        want = next(i for i, x in enumerate(labels) if x.lower() == lab.lower())
        for _ in range(2):
            got = int(nxt("L")[0])
            rep.check(got == want, "lookup %s: %d != %d" % (lab, got, want))
    rep.check(int(nxt("L")[0]) == -1, "lookup of a missing label")
    for act in acts:
        weight, want = 0, -1
        for i, s in enumerate(seqs):
            if s["activity"] == act and s["actweight"] > weight:
                weight, want = s["actweight"], i
        got = int(nxt("H")[0])
        rep.check(got == want, "heaviest act %d: %d != %d" % (act, got, want))
        draws = [int(x) for x in nxt("Y")]
        ok = {i for i, s in enumerate(seqs) if s["activity"] == act}
        rep.check(all(d in ok for d in draws), "LookupActivity %d drew %s" % (act, draws))
    rep.check(int(nxt("H")[0]) == -1, "heaviest of a missing activity")
    for k, s in enumerate(seqs):
        fr, gs, fl, nf, ns = nxt("I")
        n = s["numframes"]
        if n > 1:
            wfr = 256 * s["fps"] / (n - 1)
            wgs = math.sqrt(sum(x * x for x in s["linear"])) * s["fps"] / (n - 1)
        else:
            wfr, wgs = 256.0, 0.0
        rep.check(close(float(fr), wfr) and close(float(gs), wgs) and int(fl) == s["flags"]
                  and int(nf) == n and int(ns) == len(seqs),
                  "sequence info %d: %s" % (k, (fr, gs, fl, nf)))
    r = nxt("I")
    rep.check(float(r[0]) == 0 and float(r[1]) == 0 and int(r[2]) == 0, "info of a bad sequence")
    for k in evseqs:
        s = seqs[k]
        n = s["numframes"]
        for w in windows:
            st, en = (w[0] * (n - 1) / 256.0, w[1] * (n - 1) / 256.0) if n > 1 else (0, 1.0)
            want = []
            for fr, ev, opt, resid in s["events"]:
                if ev >= EVENT_CLIENT:
                    continue
                if (st <= fr < en) or (s["flags"] & LOOPING and en >= n - 1 and fr < en - n + 1):
                    want.append("%d:%d:%s" % (ev, resid, opt))
            got = nxt("V")
            rep.check(got == want, "events %s %s: %s != %s" % (s["label"], w, got, want))
    # the scientist's walk carries footstep sound events with cooked ids
    walk_ev = [e for e in seqs[walk]["events"] if e[1] == 1004]
    rep.check(len(walk_ev) >= 2, "walk has footstep events")

    def fa_check(tag, got, want, what):
        rep.check(all(close(float(g), w) for g, w in zip(got, want)), "%s %s: %s != %s" % (
            tag, what, got, want))

    nxt("N")
    sw = seqs[walk]
    rate = f32(256 * sw["fps"] / (sw["numframes"] - 1))
    gspeed = math.sqrt(sum(x * x for x in sw["linear"])) * sw["fps"] / (sw["numframes"] - 1)
    fa_check("R", nxt("R"), [rate, gspeed, 1, 1.0, 1.0], "walk reset")
    frame = f32(f32(0.1) * rate)
    fa_check("A", nxt("A"), [f32(1.1) - 1.0, frame, 0, f32(1.1)], "advance 0.1 s")
    fa_check("A", nxt("A"), [0, frame, 0, f32(1.1)], "advance again at the same time")
    lc = f32(1.0)
    start = frame + (lc - f32(1.1)) * rate
    end = frame + 0.1 * rate
    fa_check("W", nxt("W"), [start, end, int(end >= 256 or end <= 0), f32(1.1) + 0.1], "window")
    for dt in (0.5, 3.0, 7.5):
        frame = frame + dt * rate
        fin = 0
        if frame >= 256:
            frame -= int(frame / 256.0) * 256.0
            fin = 1
        got = nxt("A")
        rep.check(close(float(got[1]), frame, 1e-3) and int(got[2]) == fin,
                  "advance %g: %s != frame %g finished %d" % (dt, got, frame, fin))
    nxt("N")
    so = seqs[once]
    rate = 256 * so["fps"] / (so["numframes"] - 1)
    got = nxt("R")
    rep.check(close(float(got[0]), rate) and int(got[2]) == 0 and close(float(got[3]), 2.0),
              "once reset: %s" % got)
    got = nxt("A")
    rep.check(close(float(got[1]), 0.25 * rate, 1e-3) and int(got[2]) == 0, "once advance %s" % got)
    got = nxt("A")
    rep.check(float(got[1]) == 255 and int(got[2]) == 1, "once clamps at 255: %s" % got)
    nxt("W")
    # body groups: part 1 (heads)
    nm, base = info.bodyparts[1][0], info.bodyparts[1][1]
    got = nxt("G")
    rep.check(int(got[1]) == (2 if nm > 2 else 0) and int(got[0]) == 2 * base, "bodygroup 2: %s" % got)
    got = nxt("G")
    rep.check(int(got[1]) == (3 if nm > 3 else 2) , "bodygroup 3: %s" % got)
    got2 = nxt("G")
    rep.check(got2[0] == got[0], "bodygroup out of range leaves body: %s" % got2)
    nxt("G")
    for c in range(info.numctl):
        bone, index, typ, cs, ce, rest = info.ctls[c]
        for v in (30.0, -400.0):
            x = v
            if typ & (0x8 | 0x10 | 0x20):
                if ce < cs:
                    x = -x
                if cs + 359.0 >= ce:
                    if x > ((cs + ce) / 2.0) + 180:
                        x -= 360
                    if x < ((cs + ce) / 2.0) - 180:
                        x += 360
                else:
                    if x > 360:
                        x -= int(x / 360.0) * 360.0
                    elif x < 0:
                        x += int((x / -360.0) + 1) * 360.0
            setting = int(255 * (x - cs) / (ce - cs))
            setting = max(0, min(255, setting))
            want = setting * (1.0 / 255.0) * (ce - cs) + cs
            got = nxt("K")
            rep.check(close(float(got[0]), want, 1e-3) and (index > 3 or int(got[1 + index]) == setting),
                      "controller %d value %g: %s != %g/%d" % (index, v, got, want, setting))
    s = nxt("S")
    st = dict(x.split("=") for x in s)
    rep.check(int(st["models"]) == 1 and int(st["sync"]) == 1 and int(st["pool"]) > 0,
              "stats %s" % s)
    print("studio: %d checks, %d failed: %s" % (rep.n, rep.bad, "PASS" if not rep.bad else "FAIL"))
    return 1 if rep.bad else 0


if __name__ == "__main__":
    sys.exit(main())
