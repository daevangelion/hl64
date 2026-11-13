# SPDX-License-Identifier: MIT
"""Rec slice tests on the host (run by test/m3/rec.sh):

  selftest  hl64-rec --root CONTENT selftest (records, keys, names, fires,
            New Game spawn, pump budget, wakes, timers incl. freeze / thaw,
            R2, R3)
  K5        the K-think fixtures' multi_manager scenarios (dt 0.01 and
            1/30, all 10 maps): every target use at the f32 time GoldSrc
            logged, including the 0.33 s target at +0.34 / +0.333. The
            probe's f64 clock is not in the capture, only its f32 values,
            so the driver runs at the lowest, middle and highest f64 start
            consistent with every captured frame time (time += f32 frame
            time, as GoldSrc advances it) and each must match.
  fuzz      hl64-rec-fuzz on campaign.rtb and the hldemo1 ENTS chunk

  python3 test/m3/rec.py --bin DIR --content CONTENT --fixtures DIR [--fuzz N]
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
from typing import Dict, List, Tuple

TARGETS = {"hl64_t1": [0, 0.33], "hl64_t2": 0.05, "hl64_t3": 0.1, "hl64_t4": 0.1,
           "hl64_relay": 0, "hl64_t5": "relay+0.15"}


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def scenarios(path: str) -> List[dict]:
    """multi_manager scenarios of one K-think capture: frame times, the
    fire frame, the logged uses."""
    rows = [json.loads(line) for line in open(path)]
    names = {r["scenario"]: r["name"] for r in rows[1:] if "scenario" in r and "name" in r}
    out = []
    for sc, name in sorted(names.items()):
        if not name.startswith("multi_manager"):
            continue
        frames = [(r["f"], r["t"], r["ft"]) for r in rows[1:] if r.get("sc") == sc and "f" in r]
        fire = [r["t"] for r in rows[1:] if r.get("sc") == sc and r.get("ev") == "fire"]
        uses = [(r["target"], r["t"]) for r in rows[1:] if r.get("case") == sc
                and r.get("ev") == "use"]
        spec = [r["targets"] for r in rows[1:] if r.get("sc") == sc and "targets" in r]
        out.append({"name": name, "frames": sorted(frames), "fire": fire[0], "uses": uses,
                    "targets": spec[0] if spec else None})
    return out


def start_interval(frames: List[Tuple[int, float, float]], fire_t: float) -> Tuple[float, float, float]:
    """(lo, hi, dt) of the f64 times at the fire frame that reproduce every
    captured f32 frame time from there on (time += dt per frame)."""
    k0 = next(i for i, (_f, t, _ft) in enumerate(frames) if t == fire_t)
    dt = f32(frames[k0 + 1][2])
    seq = [f32(t) for _f, t, _ft in frames[k0:]]   # the capture prints f32 values
    fire_t = seq[0]

    def ok(t0: float) -> bool:
        t = t0
        for want in seq:
            if f32(t) != want:
                return False
            t += dt
        return True

    # every f64 start within +-2 us of the first frame's f32 value, on a
    # 1 ns grid: the feasible ones form the interval
    feasible = [fire_t + k * 1e-9 for k in range(-2000, 2001) if ok(fire_t + k * 1e-9)]
    if not feasible:
        raise SystemExit(f"no f64 clock reproduces the capture's frame times near {fire_t!r}")
    lo, hi = feasible[0], feasible[-1]
    return lo, hi, dt


def feasible_mid(lo: float, hi: float, sc: dict, dt: float) -> float:
    """a feasible start between lo and hi (the grid's middle point)"""
    seq = [f32(t) for _f, t, _ft in sc["frames"] if t >= sc["fire"]]
    for k in range(0, 4001):
        for m in ((lo + hi) / 2 + k * 1e-9, (lo + hi) / 2 - k * 1e-9):
            t, good = m, True
            for want in seq:
                if f32(t) != want:
                    good = False
                    break
                t += dt
            if good:
                return m
    return lo


def run_k5(binary: str, content: str, fixtures: str) -> int:
    fails = cases = 0
    kdir = os.path.join(fixtures, "think-and-time", "K-think")
    for fn in sorted(os.listdir(kdir)):
        for sc in scenarios(os.path.join(kdir, fn)):
            if sc["targets"] != TARGETS:
                print(f"K5 FAIL {fn} {sc['name']}: fixture targets {sc['targets']} differ")
                fails += 1
                continue
            lo, hi, dt = start_interval(sc["frames"], sc["fire"])
            want = sorted((n, f32(t)) for n, t in sc["uses"])
            nframes = len(sc["frames"])
            for t0 in (lo, feasible_mid(lo, hi, sc, dt), hi):
                r = subprocess.run([binary, "--root", content, "k5", t0.hex(), dt.hex(), str(nframes)],
                                   capture_output=True, text=True, timeout=60)
                got = sorted((ln.split()[1], f32(float(ln.split()[2])))
                             for ln in r.stdout.splitlines() if ln.startswith("use "))
                cases += 1
                if r.returncode or got != want:
                    fails += 1
                    print(f"K5 FAIL {fn} {sc['name']} t0={t0!r}: got {got} want {want} "
                          f"{r.stdout[-300:]} {r.stderr[-300:]}")
            mm = [t for n, t in want if n == "hl64_t1"]
            if fn == "hldemo1.jsonl":
                print(f"K5 {sc['name']}: 0.33 s target at +{mm[1] - mm[0]:.6f} "
                      f"(fire {sc['fire']!r}, f64 start in [{lo!r}, {hi!r}])")
    print(f"K5: {cases - fails}/{cases} PASS" if not fails else f"K5 FAIL: {fails}/{cases}")
    return fails


def w64_ents(path: str) -> bytes:
    """The ENTS chunk of a .w64 (docs/formats/w64-format.md)."""
    b = open(path, "rb").read()
    dofs = struct.unpack_from(">I", b, 12)[0]
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    ents = struct.unpack_from(">H", b, dofs + 52)[0]
    assert ents < nchunks
    _fcc, o, sz, _crc = struct.unpack_from(">4sIII", b, cd + 16 * ents)
    return b[o:o + sz]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="directory with hl64-rec and hl64-rec-fuzz")
    ap.add_argument("--content", required=True)
    ap.add_argument("--fixtures", required=True, help=".../fixtures/goldsrc")
    ap.add_argument("--fuzz", type=int, default=20000)
    ap.add_argument("--scratch", default=os.environ.get("TMPDIR", "/tmp"))
    a = ap.parse_args()
    fails = 0
    r = subprocess.run([os.path.join(a.bin, "hl64-rec"), "--root", a.content, "selftest"],
                       capture_output=True, text=True, timeout=600)
    out = [ln for ln in r.stdout.splitlines() if not ln.startswith(("carve:", "mem: heap", "io:"))]
    print("\n".join(out[-40:]))
    if r.returncode or "selftest ok" not in r.stdout:
        print(r.stderr[-2000:])
        fails += 1
    fails += run_k5(os.path.join(a.bin, "hl64-rec"), a.content, a.fixtures)
    ents = os.path.join(a.scratch, "hldemo1.ents")
    with open(ents, "wb") as f:
        f.write(w64_ents(os.path.join(a.content, "maps", "hldemo1.w64")))
    r = subprocess.run([os.path.join(a.bin, "hl64-rec-fuzz"), os.path.join(a.content, "campaign.rtb"),
                        ents, str(a.fuzz), "12345"], capture_output=True, text=True, timeout=3600)
    print(r.stdout.strip())
    if r.returncode or "runtime error" in r.stderr or "Sanitizer" in r.stderr:
        print(r.stderr[-3000:])
        fails += 1
    print("rec tests: PASS" if not fails else f"rec tests: FAIL ({fails})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
