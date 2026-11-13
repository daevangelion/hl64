#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Items slice tests on the host driver test/m3/items.c (`make itemstest`
-> hl64-items): the real engine frame at 1/30 s, the game's player,
weapons and items.

  tripmine   (HLSDK dlls/tripmine.cpp) hldemo1: weapon_tripmine pick-up
             and ammo (default give 1, a duplicate gives its ammo);
             PrimaryAttack against a wall within 128 u places
             monster_tripmine at the hit + normal * 8 and takes one mine;
             against nothing within 128 u it places none; the mine powers
             up 2.5 s after it is placed (+ the 0.1 s think step) and
             lays its beam; the player stepping into the beam breaks it
             (hit = the player), the mine explodes (the player is hurt)
             and goes away.
  weaponbox  (CWeaponBox::KeyValue / PackAmmo / Touch) c3a2d's two map
             weaponboxes: every ammo-name key a registered weapon uses is
             packed with its count in key order ("uranium", "rockets",
             "Satchel Charge" are not: MaxAmmoCarry -1); touching one
             gives exactly its ammo (the player clamps to the carry limit:
             7 "Trip Mine" -> 5) and removes it.
  longjump   (pm_shared PM_Jump "slj", items.cpp CItemLongJump) t0a0a:
             without the module a running duck-jump is a normal jump;
             item_longjump (with the suit) sets m_fLongJump and the
             player's physics key slj 1; then the same duck-jump leaves
             the ground at forward * 350 * 1.6 = 560 u/s with
             vz = sqrt(2 * 800 * 56) less the move's two half-gravity steps,
             punch -5; ducked but standing, or running without duck, is a
             normal jump.

c3a2d is not in the Uplink content: it is cooked from the Valve data
(--data) into --scratch unless --wb-content names a cooked c3a2d.

Usage: test/m3/items.py --bin BUILD/host/hl64-items --content DIR
         (--data VALVE --build-dir BUILD --scratch DIR | --wb-content DIR)
Prints one PASS / FAIL line per check and a summary line; exit 0 when
every check passes, 2 on a missing prerequisite.
"""
import argparse
import math
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
IN_ATTACK, IN_JUMP, IN_DUCK, IN_FORWARD = 1, 2, 4, 8
AMMO_9MM, AMMO_357, AMMO_BUCKSHOT, AMMO_ARGRENADES, AMMO_HANDGRENADE, AMMO_TRIPMINE = range(1, 7)
WEAPON_TRIPMINE = 13
TRIPMINE_MAX_CARRY = 5
GRAVITY = 800.0
MSEC = 33                      # 1/30 s fixed frames: in_frame's round(33.333)

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    return ok


def run(binary, content, mapname, script):
    p = subprocess.run([binary, "--root", content, mapname], input="\n".join(script) + "\n",
                       capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stdout[-3000:] + p.stderr[-2000:])
        raise SystemExit(f"items: hl64-items {mapname} exited {p.returncode}")
    return p.stdout.splitlines()


def grab(lines, prefix):
    return [ln for ln in lines if ln.startswith(prefix)]


def parse_pl(ln):
    f = ln.split()
    return {"org": [float(x) for x in f[1:4]], "vel": [float(x) for x in f[4:7]],
            "punch": float(f[7]), "flags": int(f[8]), "health": float(f[9]),
            "lj": int(f[10]), "physinfo": f[11]}


def parse_inv(ln):
    m = re.match(r"inv weapons=0x([0-9a-f]+) active=(\d+) ammo=([-\d,]+)", ln)
    return int(m.group(1), 16), int(m.group(2)), [int(x) for x in m.group(3).split(",")]


def parse_trace(ln):
    f = ln.split()
    return float(f[1]), [float(x) for x in f[2:5]], [float(x) for x in f[5:8]], int(f[8])


def player_start(lines):
    m = re.search(r"^player: \S+ (\S+) (\S+) (\S+) ang (\S+) (\S+)", "\n".join(lines), re.M)
    return [float(m.group(i)) for i in (1, 2, 3)], float(m.group(5))


def fmt(v):
    return " ".join(f"{x:.4f}" for x in v)


# ---- tripmine -------------------------------------------------------------

def test_tripmine(binary, content):
    mapname = "hldemo1"
    # where the player stands, and a wall within 128 u / open air within 128 u
    aims = [(p, y) for p in (0, -30, -60) for y in range(0, 360, 10)]
    probe = ["frames 30", "pl"] + [f"trace {p} {y} 128" for p, y in aims]
    lines = run(binary, content, mapname, probe)
    org = parse_pl(grab(lines, "pl ")[0])["org"]
    traces = [parse_trace(t) for t in grab(lines, "trace ")]
    wall = air = None
    for (p, y), (frac, end, n, ent) in zip(aims, traces):
        if wall is None and p == 0 and ent == 0 and frac * 128 < 100 and abs(n[2]) < 0.01:
            wall = (y, end, n)
        if air is None and frac == 1.0:
            air = (p, y)
    if not check("tripmine probe", wall is not None and air is not None,
                 f"wall {wall and wall[0]} air {air} from {fmt(org)}"):
        return
    wy, wend, wn = wall
    # aside: 64 u along the wall (the beam runs along the wall's normal)
    side = [org[0] - wn[1] * 64, org[1] + wn[0] * 64, org[2]]
    script = ["give weapon_tripmine", "inv", "give weapon_tripmine", "inv",
              f"setpos {fmt(org)} {air[0]} {air[1]}", "frames 20",
              f"buttons {IN_ATTACK} 0", "frames 1", f"buttons 0 {IN_ATTACK}", "inv",
              f"setpos {fmt(org)} 0 {wy}", "frames 20",
              f"buttons {IN_ATTACK} 0", "frames 1", f"buttons 0 {IN_ATTACK}", "inv",
              "find monster_tripmine",
              f"setpos {fmt(side)} 0 {wy}", "frames 100", "find monster_tripmine", "pl",
              f"setpos {fmt(org)} 0 {wy}", "frames 45", "find monster_tripmine", "pl"]
    lines = run(binary, content, mapname, script)
    invs = [parse_inv(x) for x in grab(lines, "inv ")]
    check("tripmine pick-up", invs[0][0] & (1 << WEAPON_TRIPMINE) and invs[0][1] == WEAPON_TRIPMINE
          and invs[0][2][AMMO_TRIPMINE] == 1 and invs[1][2][AMMO_TRIPMINE] == 2,
          f"weapons=0x{invs[0][0]:x} active={invs[0][1]} mines {invs[0][2][AMMO_TRIPMINE]}"
          f" then {invs[1][2][AMMO_TRIPMINE]}")
    placed = [re.match(r"tripmine: placed (\d+) at (\S+) (\S+) (\S+) on (\d+) at (\S+), (\d+) left",
                       x) for x in grab(lines, "tripmine: placed")]
    check("tripmine no mount: nothing placed", invs[2][2][AMMO_TRIPMINE] == 2
          and len(placed) == 1, f"mines {invs[2][2][AMMO_TRIPMINE]}")
    if not check("tripmine placed", len(placed) == 1 and invs[3][2][AMMO_TRIPMINE] == 1,
                 f"{len(placed)} placed, mines left {invs[3][2][AMMO_TRIPMINE]}"):
        return
    m = placed[0]
    mine, porg, t_place = int(m.group(1)), [float(m.group(i)) for i in (2, 3, 4)], float(m.group(6))
    want = [wend[i] + wn[i] * 8 for i in range(3)]
    check("tripmine placement: hit + normal * 8", max(abs(porg[i] - want[i]) for i in range(3)) < 0.06
          and int(m.group(5)) == 0, f"{fmt(porg)} want {fmt(want)}")
    pw = [re.match(r"tripmine: (\d+) powered up at (\S+), beam (-?\d+) length (\S+)", x)
          for x in grab(lines, "tripmine: ") if " powered up " in x]
    ok = len(pw) == 1 and int(pw[0].group(1)) == mine and int(pw[0].group(3)) >= 0
    dt = float(pw[0].group(2)) - t_place if ok else -1
    check("tripmine power-up 2.5 s, beam", ok and 2.5 <= dt <= 2.5 + 0.1 + 0.04,
          f"after {dt:.3f} s, beam {pw[0].group(3) if pw else '-'}"
          f" length {pw[0].group(4) if pw else '-'}")
    br = [re.match(r"tripmine: (\d+) beam broken at (\S+) \(hit (-?\d+)\)", x)
          for x in grab(lines, "tripmine: ") if " beam broken " in x]
    pls = [parse_pl(x) for x in grab(lines, "pl ")]
    finds = [x for x in lines if x.startswith("find ")]
    check("tripmine beam broken by the player", len(br) == 1 and int(br[0].group(3)) == 1,
          br[0].group(0) if br else "no break")
    check("tripmine detonation", finds[-1] == "find 0" and pls[-1]["health"] < pls[0]["health"],
          f"{finds[-1]}, health {pls[0]['health']} -> {pls[-1]['health']}")


# ---- weaponbox ------------------------------------------------------------

def test_weaponbox(binary, content):
    mapname = "c3a2d"
    script = ["setpos 1600 -668 -200 0 180", "frames 30", "wake weaponbox", "frames 10",
              "find weaponbox", "inv"]
    lines = run(binary, content, mapname, script)
    boxes = {}
    for x in grab(lines, "ent "):
        f = x.split()
        boxes[(round(float(f[2])), round(float(f[3])))] = (int(f[1]), "onground=1" in x)
    a, b = boxes.get((1584, -696)), boxes.get((1584, -640))
    if not check("weaponbox spawned", a is not None and b is not None and a[1] and b[1],
                 f"{boxes}"):
        return
    packed = {}
    for x in grab(lines, "weaponbox: "):
        m = re.match(r"weaponbox: (\d+) packed (\d+) (.+)$", x)
        if m:
            packed.setdefault(int(m.group(1)), []).append((m.group(3), int(m.group(2))))
    unrec = re.findall(r"MaxAmmoCarry\(\) doesn't recognize '([^']+)'", "\n".join(lines))
    check("weaponbox keys packed", packed.get(a[0]) == [("Trip Mine", 7)]
          and packed.get(b[0]) == [("ARgrenades", 8), ("357", 30)]
          and sorted(unrec) == ["Satchel Charge", "rockets", "uranium"],
          f"A {packed.get(a[0])} B {packed.get(b[0])} unrecognized {unrec}")
    script += [f"tp {b[0]} 36", "frames 2", "inv", f"live {b[0]}",
               f"tp {a[0]} 36", "frames 2", "inv", f"live {a[0]}"]
    lines = run(binary, content, mapname, script)
    invs = [parse_inv(x)[2] for x in grab(lines, "inv ")]
    picks = [x for x in grab(lines, "item: ") if x.endswith(" picked up")]
    d1 = [invs[1][i] - invs[0][i] for i in range(len(invs[0]))]
    d2 = [invs[2][i] - invs[1][i] for i in range(len(invs[0]))]
    want1 = [0] * len(d1)
    want1[AMMO_357], want1[AMMO_ARGRENADES] = 30, 8
    want2 = [0] * len(d2)
    want2[AMMO_TRIPMINE] = min(7, TRIPMINE_MAX_CARRY)
    lives = grab(lines, "live ")
    check("weaponbox touch gives its ammo", d1 == want1 and d2 == want2
          and picks == [f"item: weaponbox {b[0]} picked up", f"item: weaponbox {a[0]} picked up"]
          and lives == [f"live {b[0]} 0 -", f"live {a[0]} 0 -"],
          f"B {d1} A {d2} pick-ups {picks} {lives}")


# ---- long jump --------------------------------------------------------------

def jump_script(start, yaw, duck_frames, run_frames=10, forward=True, jump=True):
    s = [f"setpos {fmt(start)} 0 {yaw}", "frames 10"]
    if forward:
        s += [f"buttons {IN_FORWARD} 0", f"frames {run_frames}"]
    s += [f"buttons {IN_DUCK} 0", f"frames {duck_frames}", "pl"]
    if jump:
        s += [f"buttons {IN_JUMP} 0", "frames 1", "pl"]
    s += [f"buttons 0 {IN_FORWARD | IN_DUCK | IN_JUMP}", "frames 1"]
    return s


def test_longjump(binary, content):
    mapname = "t0a0a"
    ft = MSEC * 0.001
    lj_vz = math.sqrt(2 * GRAVITY * 56.0) - GRAVITY * ft
    n_vz = math.sqrt(2 * GRAVITY * 45.0) - GRAVITY * ft
    # a run-up: from the start, the yaw with the most room at eye and step height
    probe = ["frames 30", "pl"] + [f"trace 0 {y} 600 {dz}" for y in range(0, 360, 15)
                                   for dz in (0, -46)]
    lines = run(binary, content, mapname, probe)
    start = parse_pl(grab(lines, "pl ")[0])["org"]
    tr = [parse_trace(t) for t in grab(lines, "trace ")]
    room = {y: 600 * min(tr[2 * i][0], tr[2 * i + 1][0]) for i, y in enumerate(range(0, 360, 15))}
    yaw = max(room, key=lambda y: room[y])
    if not check("longjump run-up", room[yaw] >= 200, f"yaw {yaw}, {room[yaw]:.0f} u from {fmt(start)}"):
        return
    base = ["give item_suit", "frames 1"]
    module = base + ["wake item_longjump", "tp item_longjump 36", "frames 3", "pl"]
    for name, pre, duck, fwd, want_lj in (
            ("no module", base, 1, True, False),
            ("module", module, 1, True, True),
            ("module, standing duck-jump", module, 1, False, False),
            ("module, running jump without duck", module, 0, True, False)):
        script = list(pre) + jump_script(start, yaw, duck, forward=fwd)
        if not duck:
            script = [x for x in script if x not in (f"buttons {IN_DUCK} 0", "frames 0")]
        lines = run(binary, content, mapname, script)
        pls = [parse_pl(x) for x in grab(lines, "pl ")]
        if pre is module:
            got = pls.pop(0)
            if not check("longjump pick-up", got["lj"] == 1 and "\\slj\\1" in got["physinfo"],
                         f"m_fLongJump {got['lj']} physinfo {got['physinfo']}"):
                return
        else:
            check(f"longjump {name}: physics key", "\\slj\\0" in pls[0]["physinfo"],
                  pls[0]["physinfo"])
        before, after = pls[-2], pls[-1]
        sp0 = math.hypot(before["vel"][0], before["vel"][1])
        sp = math.hypot(after["vel"][0], after["vel"][1])
        vz = after["vel"][2]
        onground = bool(before["flags"] & (1 << 9))
        if want_lj:
            ok = (onground and sp0 > 50 and abs(sp - 560.0) < 0.01 and abs(vz - lj_vz) < 0.01
                  and after["punch"] == -5.0)
            want = f"560 u/s, vz {lj_vz:.4f}, punch -5"
        else:
            ok = onground and sp < 330 and abs(vz - n_vz) < 0.01 and after["punch"] == 0.0
            want = f"< 330 u/s, vz {n_vz:.4f}, punch 0"
        check(f"longjump {name}", ok,
              f"run-up {sp0:.2f} u/s on ground {int(onground)} -> {sp:.4f} u/s, vz {vz:.4f},"
              f" punch {after['punch']} (want {want})")


def cook_c3a2d(a):
    out = os.path.join(a.scratch, "c3a2d")
    log = os.path.join(a.scratch, "cook-c3a2d.log")
    with open(log, "w") as f:
        r = subprocess.run(["make", "--no-print-directory", f"BUILD_DIR={a.build_dir}", "cook",
                            f"HL64_DATA={a.data}", f"HL64_CONTENT={out}", "MAPS=c3a2d"],
                           cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        raise SystemExit(f"items: c3a2d cook failed ({log})")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--content", required=True)
    ap.add_argument("--wb-content", help="a cooked c3a2d (default: cook it from --data)")
    ap.add_argument("--data", help="the Valve data directory (c3a2d cook)")
    ap.add_argument("--build-dir", help="BUILD_DIR of the cook's hlecook")
    ap.add_argument("--scratch", help="where the c3a2d cook goes")
    a = ap.parse_args()
    wb = a.wb_content
    if not wb:
        if not (a.data and a.build_dir and a.scratch):
            print("items: the weaponbox test needs --wb-content or --data, --build-dir, --scratch")
            return 2
        os.makedirs(a.scratch, exist_ok=True)
        wb = cook_c3a2d(a)
    test_tripmine(a.bin, a.content)
    test_weaponbox(a.bin, wb)
    test_longjump(a.bin, a.content)
    good = all(results)
    print(f"items {'PASS' if good else 'FAIL'} ({sum(results)}/{len(results)} checks)")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
