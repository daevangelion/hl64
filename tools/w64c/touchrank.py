# SPDX-License-Identifier: MIT
"""touchrank: the cooked touch rank of every map trigger, written into the
HLE1 records' `trank` field (docs/specs/area-links-and-touch.md 3.1,
docs/formats/hle-format-v1.md). Runs after hlecook (tools/cook/stage.py).

The rank comes from geometry alone, so it works on any map:

  cells    the world model's box halved 4 times, each time along the longer
           horizontal axis (y when x and y are equal) at its midpoint: 31
           cells, numbered in pre-order with the upper child (coordinates
           above the split) first. A trigger belongs to the deepest cell
           whose box holds its linked box; a box that reaches a split plane
           (touching it counts) stays in the cell above the plane.
  boxes    the linked box GoldSrc gives the trigger when the map starts:
           brush triggers: their BSP model bounds + origin, +-2 (GoldSrc
           pads model bounds by 1, the link by 1 more); path_track origin
           +-9; items and ammo origin + (-17,-17,-1)...(17,17,17); weapons
           origin + (-24,-24,0)...(24,24,16) (CBasePlayerItem's own
           collision box, no padding). Cells split x/y only, so where an
           item settles vertically does not matter.
  order    in a cell, the order of the triggers' last link before play:
           brush triggers, path_track and items (linked at spawn), then
           ammo (relinked when it lands), then weapons (linked when they
           materialise); entity lump order inside each group.

  trank = cell << 11 | place in the cell's order; HLE_NONE (0xFFFF) for
  records that are not triggers. Ascending trank is touch order.

Usage (from the repository root):

  python3 -m tools.w64c.touchrank MAP.bsp MAP.hle [-o OUT.hle]
      rewrite trank (in place without -o; the file is only written when it
      changes)
  python3 -m tools.w64c.touchrank --check FIXTURES --maps-dir DIR
        [--content DIR] [--maps m1,m2]
      compare with the GoldSrc probe's A-order captures
      (FIXTURES/area-links-and-touch/A-order/<map>.jsonl): every map-trigger
      pair a placement touched (phases 1 and 3) must touch in rank order;
      with --content the cooked maps/<map>.w64 ENTS trank values must equal
      the computed ones. Exit 1 on a mismatch outside KNOWN.

Malformed input: "touchrank: error: <what>" on stderr, exit 1.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from tools.cook.hl_classes import BspError, map_entities

HLE_NONE = 0xFFFF
HLE_MAGIC = 0x31454C48           # "HLE1" read as a little-endian u32
HLE_HDR = 96
HLE_REC = 24
HLE_TRANK = 18                   # byte offset of trank in a record
HLE_MAX_LUMP = 1024
BSP_HDR = 4 + 8 * 15
BSP_MODELS = 14
DMODEL = 64

DEPTH = 4                        # 31 cells
CELL_SHIFT = 11                  # trank = cell << 11 | place
MAX_PLACE = (1 << CELL_SHIFT) - 1

# Classes GoldSrc links as SOLID_TRIGGER when the map spawns (HLSDK
# InitTrigger callers, CFrictionModifier, CFuncTankControls). hlecook drops
# some of them (hl_entcook.h hle_classes[]); they are ranked anyway, so the
# order of the kept ones is the same as in GoldSrc, and the A-order check can
# use every captured pair.
BRUSH_TRIGGERS = {
    "trigger_once", "trigger_multiple", "trigger_hurt", "trigger_push",
    "trigger_teleport", "trigger_changelevel", "trigger_endsection",
    "trigger_autosave", "trigger_gravity", "trigger_monsterjump",
    "trigger_cdaudio", "func_friction", "func_tankcontrols",
}
G_SPAWN, G_AMMO, G_WEAPON = 0, 1, 2

# GoldSrc A-order pairs the geometry cannot predict: (map, first lump,
# second lump) -> cause. Listed in area-links-and-touch.md 3.1.
KNOWN = {
    ("hldemo2", 509, 601): "item_healthkit (lump 601) spawns inside func_wall_toggle *80 and "
                           "falls 8 u after spawn (z 168 -> 160), relinking after ammo_9mmAR "
                           "(lump 509) lands",
}


class RankError(ValueError):
    pass


def finite_vec(text: str) -> Optional[List[float]]:
    """An entity vector as GoldSrc reads it (missing components are 0);
    None when a component is not a finite number."""
    out = [0.0, 0.0, 0.0]
    for i, tok in enumerate(text.split()[:3]):
        try:
            v = float(tok)
        except ValueError:
            break
        if not math.isfinite(v):
            return None
        out[i] = v
    return out


def read_models(bsp: Path) -> List[Tuple[float, ...]]:
    """dmodel bounds (mins, maxs) of a BSP29/30; the world's must be finite
    and non-empty."""
    try:
        b = bsp.read_bytes()
    except OSError as e:
        raise RankError(f"{bsp}: {e.strerror}") from None
    if len(b) < BSP_HDR or struct.unpack_from("<i", b)[0] not in (29, 30):
        raise RankError(f"{bsp}: not a BSP29/30 file")
    ofs, size = struct.unpack_from("<ii", b, 4 + 8 * BSP_MODELS)
    if ofs < 0 or size < DMODEL or size % DMODEL or ofs > len(b) - size:
        raise RankError(f"{bsp}: model lump {ofs}+{size} invalid")
    models = [struct.unpack_from("<6f", b, ofs + DMODEL * i) for i in range(size // DMODEL)]
    w = models[0]
    if not all(math.isfinite(x) for x in w) or any(w[i] >= w[i + 3] for i in range(3)):
        raise RankError(f"{bsp}: invalid world bounds")
    return models


def trigger_box(models, ent: Dict[str, str]):
    """(group, mins, maxs) of the linked box GoldSrc gives a trigger when the
    map starts, None when the entity is not a trigger (or has no box)."""
    cls = ent.get("classname", "").lower()
    org = finite_vec(ent.get("origin", ""))
    if org is None:
        return None
    if cls in BRUSH_TRIGGERS:
        m = ent.get("model", "")
        if not (m.startswith("*") and m[1:].isdigit()):
            return None
        n = int(m[1:])
        if not 0 < n < len(models) or not all(math.isfinite(x) for x in models[n]):
            return None
        md = models[n]
        return (G_SPAWN, [md[k] + org[k] - 2 for k in range(3)],
                [md[3 + k] + org[k] + 2 for k in range(3)])
    if cls == "path_track":
        return G_SPAWN, [c - 9 for c in org], [c + 9 for c in org]
    if cls.startswith("weapon_"):
        return (G_WEAPON, [org[0] - 24, org[1] - 24, org[2]],
                [org[0] + 24, org[1] + 24, org[2] + 16])
    if cls.startswith("item_") or cls.startswith("ammo_"):
        return (G_AMMO if cls.startswith("ammo_") else G_SPAWN,
                [org[0] - 17, org[1] - 17, org[2] - 1], [org[0] + 17, org[1] + 17, org[2] + 17])
    return None


def cell(world, mins, maxs) -> int:
    """Pre-order index (0-30) of the deepest cell holding [mins, maxs]."""
    lo, hi = list(world[:3]), list(world[3:6])
    idx = 0
    for depth in range(DEPTH):
        ax = 0 if hi[0] - lo[0] > hi[1] - lo[1] else 1
        mid = 0.5 * (lo[ax] + hi[ax])
        sub = (1 << (DEPTH - depth)) - 1     # cells in each child subtree
        if mins[ax] > mid:                   # upper child, visited first
            idx += 1
            lo[ax] = mid
        elif maxs[ax] < mid:
            idx += 1 + sub
            hi[ax] = mid
        else:
            break
    return idx


def ranks(models, ents: Sequence[Dict[str, str]]) -> Dict[int, int]:
    """lump -> trank for every trigger of the entity lump."""
    world = models[0]
    keyed = []
    for lump, e in enumerate(ents):
        tb = trigger_box(models, e)
        if tb:
            g, mn, mx = tb
            keyed.append((cell(world, mn, mx), g, lump))
    keyed.sort()
    out, place, prev = {}, 0, None
    for c, _g, lump in keyed:
        place = place + 1 if c == prev else 0
        prev = c
        if place > MAX_PLACE:
            raise RankError(f"more than {MAX_PLACE + 1} triggers in one cell")
        out[lump] = c << CELL_SHIFT | place
    return out


# ---- HLE1 records ------------------------------------------------------------

def hle_records(blob: bytes) -> Tuple[str, List[Tuple[int, int, str]], int]:
    """(byte order, [(record offset, lump, classname)], srchash), every
    offset and index checked."""
    if len(blob) < HLE_HDR:
        raise RankError("HLE1 chunk shorter than its header")
    for bo in ("<", ">"):
        if struct.unpack_from(bo + "I", blob)[0] == HLE_MAGIC:
            break
    else:
        raise RankError("not an HLE1 chunk")
    (_m, _v, _f, size, srchash, numlumps, numrecs, numclasses) = struct.unpack_from(bo + "IHHIIHHH", blob)
    strsize = struct.unpack_from(bo + "I", blob, 44)[0]
    ofs_recs, _l2r, ofs_classes = struct.unpack_from(bo + "3I", blob, 52)
    ofs_strings = struct.unpack_from(bo + "I", blob, 92)[0]
    if not HLE_HDR <= size <= len(blob):
        raise RankError(f"HLE1 size {size} outside the {len(blob)}-byte file")
    if (ofs_recs > size or numrecs * HLE_REC > size - ofs_recs
            or ofs_classes > size or numclasses * 8 > size - ofs_classes
            or ofs_strings > size or strsize > size - ofs_strings):
        raise RankError("HLE1 section outside the chunk")
    strings = blob[ofs_strings:ofs_strings + strsize]
    names = []
    for c in range(numclasses):
        s = struct.unpack_from(bo + "I", blob, ofs_classes + 8 * c)[0]
        end = strings.find(b"\0", s) if s < len(strings) else -1
        if end < 0:
            raise RankError(f"HLE1 class {c} string outside the string table")
        names.append(strings[s:end].decode("latin-1"))
    recs = []
    for r in range(numrecs):
        o = ofs_recs + HLE_REC * r
        lump, cls = struct.unpack_from(bo + "HH", blob, o)
        if lump >= numlumps or cls >= numclasses:
            raise RankError(f"HLE1 record {r}: lump {lump} / class {cls} out of range")
        recs.append((o, lump, names[cls]))
    return bo, recs, srchash


def entity_lump(bsp: Path) -> bytes:
    """Raw entity lump bytes (what hlecook hashes into srchash)."""
    b = bsp.read_bytes()
    ofs, size = struct.unpack_from("<ii", b, 4)
    if ofs < 0 or size < 0 or ofs > len(b) - size:
        raise RankError(f"{bsp}: entity lump {ofs}+{size} outside the file")
    return b[ofs:ofs + size]


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def apply(blob: bytes, bsp: Path) -> Tuple[bytes, int]:
    """blob with every record's trank set from the BSP; (new blob, triggers)."""
    models = read_models(bsp)
    try:
        ents = map_entities(bsp)
        src = entity_lump(bsp)
    except (BspError, OSError) as e:
        raise RankError(str(e)) from None
    bo, recs, srchash = hle_records(blob)
    if fnv1a(src) != srchash:
        raise RankError(f"{bsp}: entity text is not the one the HLE1 chunk was cooked from")
    rk = ranks(models, ents)
    out = bytearray(blob)
    n = 0
    for o, lump, cname in recs:
        if lump >= len(ents) or ents[lump].get("classname", "").lower() != cname.lower():
            raise RankError(f"{bsp}: HLE1 record lump {lump} ({cname}) is not BSP entity {lump}")
        t = rk.get(lump, HLE_NONE)
        n += t != HLE_NONE
        struct.pack_into(bo + "H", out, o + HLE_TRANK, t)
    return bytes(out), n


def read_tranks(blob: bytes) -> Dict[int, int]:
    """lump -> trank of every record (HLE_NONE for non-triggers)."""
    bo, recs, _h = hle_records(blob)
    return {lump: struct.unpack_from(bo + "H", blob, o + HLE_TRANK)[0] for o, lump, _c in recs}


def w64_ents(path: Path) -> bytes:
    """The ENTS chunk of a .w64 (docs/formats/w64-format.md)."""
    b = path.read_bytes()
    if len(b) < 16 or b[:4] != b"W64L":
        raise RankError(f"{path}: not a W64L package")
    dofs = struct.unpack_from(">I", b, 12)[0]
    if dofs > len(b) - 56:
        raise RankError(f"{path}: directory outside the file")
    nchunks = struct.unpack_from(">H", b, dofs + 10)[0]
    cd = dofs + struct.unpack_from(">I", b, dofs + 16)[0]
    ents = struct.unpack_from(">H", b, dofs + 52)[0]
    if ents >= nchunks or cd > len(b) - 16 * nchunks:
        raise RankError(f"{path}: no ENTS chunk")
    _fcc, o, sz, _crc = struct.unpack_from(">4sIII", b, cd + 16 * ents)
    if o > len(b) or sz > len(b) - o:
        raise RankError(f"{path}: ENTS chunk outside the file")
    return b[o:o + sz]


# ---- A-order check --------------------------------------------------------------

def check_map(fixture: Path, bsp: Path, content: Optional[Path]):
    """(report lines, counts, failures) of one A-order capture."""
    m = fixture.stem
    models = read_models(bsp)
    ents = map_entities(bsp)
    rk = ranks(models, ents)
    trig, pairs = {}, {}
    for line in fixture.read_text().splitlines():
        r = json.loads(line) if line.strip() else {}
        if "trigger" in r:
            trig[r["trigger"]["i"]] = r["trigger"]
        elif "order" in r and r.get("phase") in (1, 3):
            pairs.setdefault(tuple(sorted(r["pair"])), set()).add(tuple(r["order"]))

    def lump_of(i):
        """The trigger's BSP lump; None for entities the game created
        (no lump, or a slot whose position is not its lump's)."""
        t = trig.get(i)
        if not t or not 0 <= t["lump"] < len(ents):
            return None
        e = ents[t["lump"]]
        if e.get("classname", "").lower() != t["class"].lower():
            return None
        if not t["model"].startswith("*"):
            o = finite_vec(e.get("origin", ""))
            if o is None or o[:2] != t["o"][:2]:
                return None
        return t["lump"]

    n = {"pairs": len(pairs), "match": 0, "runtime": 0, "known": 0, "fail": 0}
    lines, fails = [], []
    for p, orders in sorted(pairs.items()):
        la, lb = lump_of(p[0]), lump_of(p[1])
        if la is None or lb is None:
            n["runtime"] += 1
            continue
        if len(orders) != 1:
            n["fail"] += 1
            fails.append(f"{m} {p}: GoldSrc order differs between placements {sorted(orders)}")
            continue
        a, b = next(iter(orders))
        first, second = lump_of(a), lump_of(b)
        if first not in rk or second not in rk:
            n["fail"] += 1
            fails.append(f"{m} {a}<{b}: lumps {first}/{second} not ranked as triggers")
        elif rk[first] < rk[second]:
            n["match"] += 1
        elif (m, first, second) in KNOWN:
            n["known"] += 1
            lines.append(f"  known {m} {a}<{b} (lumps {first}<{second}): {KNOWN[(m, first, second)]}")
        else:
            n["fail"] += 1
            fails.append(f"{m} {a}<{b}: lumps {first}<{second}, tranks {rk[first]:#x} {rk[second]:#x}")
    if content is not None:
        cooked = read_tranks(w64_ents(content / "maps" / f"{m}.w64"))
        bad = [lump for lump, t in sorted(cooked.items()) if t != rk.get(lump, HLE_NONE)]
        n["cooked"] = sum(t != HLE_NONE for t in cooked.values())
        if bad:
            n["fail"] += len(bad)
            fails.append(f"{m}: cooked trank differs from the computed one at lumps {bad[:8]}")
    return lines, n, fails


def check(fixtures: Path, maps_dir: Path, content: Optional[Path], only: Optional[List[str]]) -> int:
    base = fixtures / "area-links-and-touch" / "A-order"
    files = sorted(base.glob("*.jsonl"))
    if only:
        files = [f for f in files if f.stem in only]
    if not files:
        raise RankError(f"{base}: no A-order captures")
    tot = {}
    fails = []
    for f in files:
        lines, n, fl = check_map(f, maps_dir / f"{f.stem}.bsp", content)
        cooked = f", cooked tranks {n['cooked']} equal" if "cooked" in n and not fl else ""
        print(f"{f.stem}: {n['match']}/{n['pairs']} pairs in rank order, {n['runtime']} with a "
              f"game-created trigger, {n['known']} known{cooked}")
        for l in lines:
            print(l)
        fails += fl
        for k, v in n.items():
            tot[k] = tot.get(k, 0) + v
    for l in fails:
        print("FAIL " + l)
    print(f"total: {tot['match']}/{tot['pairs']} pairs in rank order, {tot['runtime']} with a "
          f"game-created trigger, {tot['known']} known, {tot['fail']} failures")
    print("touchrank check: " + ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="touchrank", description=__doc__.split("\n")[0])
    ap.add_argument("bsp", nargs="?", type=Path)
    ap.add_argument("hle", nargs="?", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--check", type=Path, metavar="FIXTURES")
    ap.add_argument("--maps-dir", type=Path)
    ap.add_argument("--content", type=Path)
    ap.add_argument("--maps")
    a = ap.parse_args(argv)
    try:
        if a.check:
            if not a.maps_dir:
                ap.error("--check needs --maps-dir")
            return check(a.check, a.maps_dir, a.content, a.maps.split(",") if a.maps else None)
        if not a.bsp or not a.hle:
            ap.error("MAP.bsp and MAP.hle are required")
        try:
            blob = a.hle.read_bytes()
        except OSError as e:
            raise RankError(f"{a.hle}: {e.strerror}") from None
        new, n = apply(blob, a.bsp)
        out = a.out or a.hle
        if out != a.hle or new != blob:
            tmp = out.with_name(out.name + ".tmp")
            tmp.write_bytes(new)
            os.replace(tmp, out)
        print(f"touchrank: {a.hle.name}: {n} triggers ranked")
        return 0
    except (RankError, BspError, OSError, json.JSONDecodeError, KeyError, struct.error) as e:
        print(f"touchrank: error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
