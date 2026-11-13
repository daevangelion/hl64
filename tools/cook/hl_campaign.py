# SPDX-License-Identifier: MIT
"""
hl_campaign.py — cook the campaign world graph (maps/campaign.cwg) from the
staged maps' entity lumps.

Every trigger_changelevel (`map`, `landmark`) and info_landmark of every
staged map becomes a graph edge / landmark record; nothing is listed by
hand. Connected maps form a component (Uplink, the Hazard Course, a retail
chapter run). Landmark-aligned component offsets are diagnostic only
(offset(to) = offset(from) + lm_from - lm_to, GoldSrc changelevel math).
CWG3 instead packs every map's world bounds disjoint in global space.

Start map ("New Game"): liblist.gam `startmap` when the data dir has one and
it is staged; otherwise the root of the first component, where components
and their roots order by name (c0a0 < hldemo1 < t0a0 — retail, Uplink and
Hazard Course all start at their smallest name). Train map: liblist.gam
`trainmap` when staged.

CWG3 binary (big-endian, read by the hl64 world module, engine/world):
  header 32 B   magic 'CWG3', version 3, flags 0, map/landmark/link counts,
                start, train, component/seam/twin/sector-pool counts, reserved,
                u32 string bytes (all other fields u16)
  map[]   48 B  name, component, first/count landmark, first/count link,
                global offset[3], local mins/maxs[3]
  lm[]    16 B  name, reserved, origin[3]
  link[]   8 B  destination, landmark name, spawnflags, seam (0xFFFF none)
  seam[] 136 B closed-switch proof, boxes, budgets, B CRC, portal translation[3]
  twin[]   4 B  source/destination BSP entity-lump indices (worldspawn = 0)
  sectorpool[] u16 B sector (<0x8000) or 0x8000|B MOVR model; then strings.
The graph cook leaves seam/twin/sector tables empty; w64seam.py fills them
after .w64 packaging. See docs/formats/cwg-format.md. The HLE record's old
twin field is now the trigger touch rank (tools/w64c/touchrank.py).
Maps are stored component by component, each in BFS order from its root;
links to maps that are not staged are dropped (reported).

Usage (from the repository root):
  python3 -m tools.cook.hl_campaign --maps-dir DATA/maps [--maps GLOB ...] \\
      [--liblist DATA/liblist.gam] -o maps/campaign.cwg [--json report.json]

A malformed input (a BSP or CWG3 that is truncated, has a lump outside the file,
bad magic or version, an index out of range, an absurd count, or an entity key
that is not a number where one is required) is reported as
`hl_campaign: error: <file>: <what>` on stderr with exit status 1.
"""
from __future__ import annotations

import argparse
import math
import json
import re
import struct
import sys
from collections import deque
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from tools.cook.hl_classes import discover_maps, map_entities

MAGIC = 0x43574733  # 'CWG3'
NONE = 0xFFFF
MAP_BYTES = 48
SEAM_BYTES = 136
PLACEMENT_GRID = 16
PLACEMENT_PAD = 128
MAX_ULP = 0.03125 / 16
MAP_NAME = re.compile(r"[A-Za-z0-9_+-][A-Za-z0-9_.+-]*")   # a plain file stem, never a path

Vec3 = Tuple[float, float, float]


def parse_origin(s: str) -> Optional[Vec3]:
    p = s.split()
    if len(p) < 3:
        return None
    try:
        o = (float(p[0]), float(p[1]), float(p[2]))
    except ValueError:
        raise ValueError(f"origin {s!r} is not three numbers") from None
    if not all(math.isfinite(x) for x in o):
        raise ValueError(f"origin {s!r} is not finite")
    return o


def parse_liblist(path: Optional[Path]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    if path and path.is_file():
        for line in path.read_text("latin-1").splitlines():
            m = re.match(r'\s*(\w+)\s+"?([^"\s]*)"?', line)
            if m and not line.lstrip().startswith("//"):
                out[m.group(1).lower()] = m.group(2)
    return out


def bsp_bounds(path: Path) -> list:
    """BSP29/30 dmodel 0, also used for the W64 world-model bounds."""
    b = path.read_bytes()
    if len(b) < 124 or struct.unpack_from("<i", b)[0] not in (29, 30):
        raise ValueError(f"{path}: invalid BSP header")
    off, size = struct.unpack_from("<ii", b, 4 + 14 * 8)
    if off < 124 or size < 64 or size % 64 or off + size > len(b):
        raise ValueError(f"{path}: invalid BSP model lump")
    bounds = list(struct.unpack_from("<6f", b, off))
    if (not all(math.isfinite(x) for x in bounds) or
            any(bounds[i] >= bounds[i + 3] for i in range(3))):
        raise ValueError(f"{path}: invalid world bounds")
    return bounds


def float_ulp(x: float) -> float:
    """Spacing of binary32 at |x| (not Python's binary64 math.ulp)."""
    return 2.0 ** max(-149, math.frexp(abs(x))[1] - 24) if x else 2.0 ** -149


def portal_translation(a: list, b: list, delta: list) -> list:
    """Use the stored binary32 delta, then round T once to binary32."""
    d = struct.unpack(">3f", struct.pack(">3f", *delta))
    return list(struct.unpack(">3f", struct.pack(">3f", *(b[i] + d[i] - a[i] for i in range(3)))))


def pack_maps(bounds: dict) -> dict:
    """Deterministic unrotated 3D shelves, all components disjoint."""
    if not bounds:
        return {}
    low = {m: [math.floor(x / PLACEMENT_GRID) * PLACEMENT_GRID for x in b[:3]]
           for m, b in bounds.items()}
    sizes = {m: [math.ceil(b[i + 3] / PLACEMENT_GRID) * PLACEMENT_GRID -
                 low[m][i] + PLACEMENT_PAD for i in range(3)] for m, b in bounds.items()}
    order = sorted(bounds, key=lambda m: (-sizes[m][2], -sizes[m][1], -sizes[m][0], m))
    base = max(max(max(s) for s in sizes.values()),
               math.ceil(sum(math.prod(s) for s in sizes.values()) ** (1 / 3) /
                         PLACEMENT_GRID) * PLACEMENT_GRID)
    best = None
    # Try a few shelf widths/depths, minimize the longest world axis first.
    for width in (base, base * 5 // 4 // 16 * 16, base * 3 // 2 // 16 * 16, base * 2):
        for depth in (base, base * 5 // 4 // 16 * 16, base * 3 // 2 // 16 * 16, base * 2):
            x = y = z = row = shelf = 0
            positions = {}
            extent = [0, 0, 0]
            for m in order:
                sx, sy, sz = sizes[m]
                if x + sx > width:
                    x, y, row = 0, y + row, 0
                if y + sy > depth:
                    x, y, z, row, shelf = 0, 0, z + shelf, 0, 0
                positions[m] = [x - low[m][0], y - low[m][1], z - low[m][2]]
                extent = [max(extent[i], (x, y, z)[i] + sizes[m][i]) for i in range(3)]
                x += sx
                row, shelf = max(row, sy), max(shelf, sz)
            score = (max(extent), math.prod(extent), sum(extent))
            if best is None or score < best[0]:
                best = score, positions
    positions = best[1]
    center = [math.floor((min(bounds[m][i] + positions[m][i] for m in bounds) +
                          max(bounds[m][i + 3] + positions[m][i] for m in bounds)) /
                         (2 * PLACEMENT_GRID)) * PLACEMENT_GRID for i in range(3)]
    result = {m: [positions[m][i] - center[i] for i in range(3)] for m in bounds}
    validate_placements([{"name": m, "gofs": result[m], "bounds": b} for m, b in bounds.items()])
    return result


def validate_placements(maps: list) -> None:
    for j, m in enumerate(maps):
        g, b = m["gofs"], m["bounds"]
        if (not all(math.isfinite(x) for x in [*g, *b]) or
                any(x % PLACEMENT_GRID for x in g) or
                any(b[i] >= b[i + 3] for i in range(3))):
            raise ValueError(f"map {m['name']}: invalid placement/bounds")
        if max(float_ulp(b[i] + g[i % 3]) for i in range(6)) > MAX_ULP:
            raise ValueError(f"map {m['name']}: placement loses float precision")
        for other in maps[:j]:
            h, c = other["gofs"], other["bounds"]
            if not any(b[i + 3] + g[i] + PLACEMENT_PAD <= c[i] + h[i] or
                       c[i + 3] + h[i] + PLACEMENT_PAD <= b[i] + g[i] for i in range(3)):
                raise ValueError(f"maps {m['name']}/{other['name']}: placement padding <128")


def build(maps_dir: Path, maps: List[str], liblist: Dict[str, str]) -> dict:
    staged = set(maps)
    info: Dict[str, dict] = {}
    dropped: List[str] = []
    for m in maps:
        lms: Dict[str, Vec3] = {}
        links: List[dict] = []
        path = maps_dir / f"{m}.bsp"
        for e in map_entities(path):
            cn = e.get("classname", "")
            try:
                if cn == "info_landmark" and e.get("targetname"):
                    o = parse_origin(e.get("origin", ""))
                    if o is not None:
                        lms.setdefault(e["targetname"], o)
                elif cn == "trigger_changelevel" and e.get("map"):
                    dest = e["map"]
                    if dest not in staged:
                        dropped.append(f"{m} -> {dest} (not staged)")
                        continue
                    links.append({"to": dest, "landmark": e.get("landmark", ""),
                                  "spawnflags": int(e.get("spawnflags", "0") or 0)})
            except ValueError as error:
                raise ValueError(f"{path}: {cn} entity: {error}") from None
        info[m] = {"landmarks": lms, "links": links}

    # components: undirected over changelevel edges; ordered by root name
    adj: Dict[str, List[str]] = {m: [] for m in maps}
    for m in maps:
        for l in info[m]["links"]:
            adj[m].append(l["to"])
            adj[l["to"]].append(m)
    comp_of: Dict[str, int] = {}
    comps: List[List[str]] = []
    for root in sorted(maps):
        if root in comp_of:
            continue
        order: List[str] = []
        q = deque([root])
        comp_of[root] = len(comps)
        while q:
            u = q.popleft()
            order.append(u)
            for v in adj[u]:
                if v not in comp_of:
                    comp_of[v] = len(comps)
                    q.append(v)
        comps.append(order)

    start = liblist.get("startmap")
    if start not in staged:
        start = comps[0][0] if comps else None
    train = liblist.get("trainmap")
    if train not in staged:
        train = None
    # the start map's component first, its root replaced by the start map
    if start is not None:
        ci = comp_of[start]
        comps.insert(0, comps.pop(ci))
        if comps[0][0] != start:
            comps[0].remove(start)
            comps[0].insert(0, start)
        for i, c in enumerate(comps):
            for m in c:
                comp_of[m] = i

    # placement: BFS from each component's first map through shared landmarks
    offset: Dict[str, Vec3] = {}
    mismatch: List[str] = []
    for c in comps:
        offset[c[0]] = (0.0, 0.0, 0.0)
        q = deque([c[0]])
        while q:
            u = q.popleft()
            for l in info[u]["links"]:
                v, lm = l["to"], l["landmark"]
                a = info[u]["landmarks"].get(lm)
                b = info[v]["landmarks"].get(lm)
                if a is None or b is None:
                    continue
                ov = tuple(offset[u][i] + a[i] - b[i] for i in range(3))
                if v not in offset:
                    offset[v] = ov  # type: ignore[assignment]
                    q.append(v)
                elif max(abs(ov[i] - offset[v][i]) for i in range(3)) > 0.5:
                    mismatch.append(f"{u}->{v} via {lm}: {ov} vs {offset[v]}")
    order = [m for c in comps for m in c]
    bounds = {m: bsp_bounds(maps_dir / f"{m}.bsp") for m in order}
    gofs = pack_maps(bounds)
    return {"order": order, "components": comps, "comp_of": comp_of,
            "start": start, "train": train, "info": info,
            "offset": offset, "bounds": bounds, "gofs": gofs,
            "dropped": dropped, "mismatch": mismatch}


def write_cwg(g: dict, path: Path) -> int:
    """Write CWG3, including optional seam proofs produced by w64seam."""
    strtab = bytearray()
    sofs: Dict[str, int] = {}

    def s(name: str) -> int:
        if name not in sofs:
            sofs[name] = len(strtab)
            strtab.extend(name.encode("latin-1") + b"\0")
            if len(strtab) > NONE:
                raise ValueError("campaign string table > 65535 bytes")
        return sofs[name]

    order = g["order"]
    idx = {m: i for i, m in enumerate(order)}
    validate_placements([{"name": m, "gofs": g["gofs"][m], "bounds": g["bounds"][m]}
                         for m in order])
    maps_b, lms_b, links_b, seams_b = (bytearray() for _ in range(4))
    twins, sectorpool = [], []
    nlm = nlink = 0
    for m in order:
        inf = g["info"][m]
        lms = sorted(inf["landmarks"].items())
        links = inf["links"]
        maps_b += struct.pack(">6H9f", s(m), g["comp_of"][m], nlm, len(lms),
                              nlink, len(links), *g["gofs"][m], *g["bounds"][m])
        for name, o in lms:
            lms_b += struct.pack(">HH3f", s(name), 0, *o)
        for link in links:
            links_b += struct.pack(">4H", idx[link["to"]], s(link["landmark"]),
                                   link["spawnflags"] & 0xFFFF, link.get("seam", NONE))
        nlm += len(lms)
        nlink += len(links)
    seams = g.get("seams", [])
    for seam in seams:
        pairs, sectors = seam["twins"], seam["sectors"]
        seams_b += struct.pack(
            ">12H21f4I3f", idx[seam["from"]], idx[seam["to"]], s(seam["landmark"]),
            seam["trigger_lump"], s(seam["changetarget"]) if seam["changetarget"] else NONE,
            seam["kind"], seam["flags"], len(twins), len(pairs), len(sectorpool), len(sectors),
            seam.get("draw", 0),
            *seam["delta"], *seam["switch_mins"], *seam["switch_maxs"],
            *seam["carry_mins"], *seam["carry_maxs"],
            *seam["approach_mins"], *seam["approach_maxs"],
            *seam["need"], seam["b_dir_crc"], *seam["portal"])
        twins.extend(pairs)
        sectorpool.extend(sectors)
    counts = (len(order), nlm, nlink, len(g["components"]), len(seams),
              len(twins), len(sectorpool))
    if any(c > 0xFFFE for c in counts):
        raise ValueError("campaign graph exceeds 0xFFFE counts")
    hdr = struct.pack(">I12HI", MAGIC, 3, 0, len(order), nlm, nlink,
                      idx[g["start"]] if g["start"] else NONE,
                      idx[g["train"]] if g["train"] else NONE,
                      len(g["components"]), len(seams), len(twins), len(sectorpool), 0,
                      len(strtab))
    blob = (hdr + maps_b + lms_b + links_b + seams_b +
            b"".join(struct.pack(">HH", *p) for p in twins) +
            b"".join(struct.pack(">H", sector) for sector in sectorpool) + strtab)
    path.write_bytes(blob)
    return len(blob)


def report(g: dict) -> dict:
    return {
        "start": g["start"], "train": g["train"],
        "components": g["components"],
        "global_bounds": (
            [min(g["bounds"][m][i] + g["gofs"][m][i] for m in g["order"]) for i in range(3)] +
            [max(g["bounds"][m][i + 3] + g["gofs"][m][i] for m in g["order"]) for i in range(3)]
        ) if g["order"] else [],
        "maps": {m: {"component": g["comp_of"][m],
                     "offset": list(g["offset"].get(m, (0, 0, 0))),
                     "gofs": g["gofs"][m], "bounds": g["bounds"][m],
                     "global_bounds": [v + g["gofs"][m][i % 3]
                                       for i, v in enumerate(g["bounds"][m])],
                     "farthest_corner_ulp": max(float_ulp(v + g["gofs"][m][i % 3])
                                                for i, v in enumerate(g["bounds"][m])),
                     "landmarks": {k: list(v) for k, v in
                                   sorted(g["info"][m]["landmarks"].items())},
                     "links": g["info"][m]["links"]}
                 for m in g["order"]},
        "dropped_links": g["dropped"], "landmark_mismatch": g["mismatch"],
    }


def read_cwg(path: Path) -> dict:
    """Read and validate CWG3, retaining seam data and the public map-list API."""
    b = path.read_bytes()
    if len(b) < 32:
        raise ValueError(f"{path}: truncated CWG3 header")
    (magic, ver, flags, nmaps, nlm, nlink, start, train, ncomp, nseams,
     ntwins, nsectpool, reserved, slen) = struct.unpack_from(">I12HI", b)
    need = (32 + MAP_BYTES * nmaps + 16 * nlm + 8 * nlink +
            SEAM_BYTES * nseams + 4 * ntwins + 2 * nsectpool)
    if (magic != MAGIC or ver != 3 or flags or reserved or
            max(nmaps, nlm, nlink, ncomp, nseams, ntwins, nsectpool) > 0xFFFE or
            len(b) != need + slen or not slen or b[-1] != 0):
        raise ValueError(f"{path}: invalid CWG3 graph")
    st = b[need:]

    def name(o: int) -> str:
        if o >= len(st):
            raise ValueError(f"{path}: string offset {o} out of range")
        return st[o:st.index(b"\0", o)].decode("latin-1")

    lmofs = 32 + nmaps * MAP_BYTES
    linkofs = lmofs + nlm * 16
    seamofs = linkofs + nlink * 8
    twinofs = seamofs + nseams * SEAM_BYTES
    sectorofs = twinofs + ntwins * 4
    maps = []
    for i in range(nmaps):
        n, comp, flm, nl, fli, nli, *coords = struct.unpack_from(">6H9f", b, 32 + i * MAP_BYTES)
        if comp >= ncomp or flm + nl > nlm or fli + nli > nlink:
            raise ValueError(f"{path}: map {i} table range")
        lms = {}
        for j in range(flm, flm + nl):
            ln, pad, *o = struct.unpack_from(">HH3f", b, lmofs + j * 16)
            if pad:
                raise ValueError(f"{path}: landmark reserved field")
            if not all(math.isfinite(x) for x in o):
                raise ValueError(f"{path}: landmark origin is not finite")
            lms[name(ln)] = o
        links = []
        for j in range(fli, fli + nli):
            to, ln, sf, seam = struct.unpack_from(">4H", b, linkofs + j * 8)
            if to >= nmaps or (seam != NONE and seam >= nseams):
                raise ValueError(f"{path}: link {j} index")
            links.append({"to": to, "landmark": name(ln), "spawnflags": sf, "seam": seam})
        if not MAP_NAME.fullmatch(name(n)):
            raise ValueError(f"{path}: map name {name(n)!r} is not a plain file name")
        maps.append({"name": name(n), "component": comp,
                     "gofs": coords[:3], "bounds": coords[3:9],
                     "landmarks": lms, "links": links})
    try:
        validate_placements(maps)
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from None
    if len({m["name"] for m in maps}) != nmaps:
        raise ValueError(f"{path}: duplicate map name")
    seams = []
    for i in range(nseams):
        v = struct.unpack_from(">12H21f4I3f", b, seamofs + SEAM_BYTES * i)
        a, dest, lm, lump, target, kind, sf, ft, nt, fs, ns, ndraw = v[:12]
        # flags: 1 closed, 2 narrowed, 4 transition volume, 8 open (exactly
        # one of closed / open); ndraw = leading sector entries drawn across
        # layers, open seams only
        if (a >= nmaps or dest >= nmaps or kind > 1 or sf & ~15 or
                bool(sf & 1) == bool(sf & 8) or ndraw > ns or (ndraw and not sf & 8) or
                ft + nt > ntwins or fs + ns > nsectpool):
            raise ValueError(f"{path}: seam {i} index/flags")
        if not all(math.isfinite(x) for x in v[12:33]):
            raise ValueError(f"{path}: seam {i} has a non-finite delta or box")
        expected = portal_translation(maps[a]["gofs"], maps[dest]["gofs"], v[12:15])
        if any(not math.isfinite(v[37 + k]) or v[37 + k] != expected[k] for k in range(3)):
            raise ValueError(f"{path}: seam {i} portal translation mismatch")
        seams.append({"from": maps[a]["name"], "to": maps[dest]["name"],
                      "landmark": name(lm), "trigger_lump": lump,
                      "changetarget": name(target) if target != NONE else "",
                      "kind": kind, "flags": sf, "delta": list(v[12:15]),
                      "portal": list(v[37:40]),
                      "switch_mins": list(v[15:18]), "switch_maxs": list(v[18:21]),
                      "carry_mins": list(v[21:24]), "carry_maxs": list(v[24:27]),
                      "approach_mins": list(v[27:30]), "approach_maxs": list(v[30:33]),
                      "need": list(v[33:36]), "b_dir_crc": v[36], "draw": ndraw,
                      "twins": [list(struct.unpack_from(">HH", b, twinofs + 4 * j))
                                for j in range(ft, ft + nt)],
                      "sectors": [struct.unpack_from(">H", b, sectorofs + 2 * j)[0]
                                  for j in range(fs, fs + ns)]})
    for m in maps:
        for link in m["links"]:
            link["to"] = maps[link["to"]]["name"]
            if link["seam"] != NONE:
                seam = seams[link["seam"]]
                if (seam["from"], seam["to"], seam["landmark"]) != (
                        m["name"], link["to"], link["landmark"]):
                    raise ValueError(f"{path}: link/seam mismatch")
    if any(v != NONE and v >= nmaps for v in (start, train)):
        raise ValueError(f"{path}: start/train out of range")
    return {"maps": maps, "components": ncomp, "seams": seams, "version": ver,
            "start": maps[start]["name"] if start != NONE else None,
            "train": maps[train]["name"] if train != NONE else None}


def run() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--maps-dir", type=Path)
    ap.add_argument("--maps", nargs="*", default=None,
                    help="map names / globs (default: every *.bsp)")
    ap.add_argument("--liblist", type=Path, default=None,
                    help="liblist.gam (default: <maps-dir>/../liblist.gam)")
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--json", type=Path, default=None, help="readable report")
    ap.add_argument("--list", type=Path, default=None, metavar="CWG",
                    help="print the map names of a cooked graph (graph order) and exit")
    args = ap.parse_args()
    if args.list:
        print(" ".join(m["name"] for m in read_cwg(args.list)["maps"]))
        return 0
    if not args.maps_dir or not args.output:
        ap.error("--maps-dir and -o are required")
    maps = discover_maps(args.maps_dir, args.maps)
    if not maps:
        raise ValueError(f"{args.maps_dir}: no maps")
    for name in maps:
        if not MAP_NAME.fullmatch(name):
            raise ValueError(f"{args.maps_dir / (name + '.bsp')}: map name is not a plain file name")
    lib = parse_liblist(args.liblist or args.maps_dir.parent / "liblist.gam")
    g = build(args.maps_dir, maps, lib)
    n = write_cwg(g, args.output)
    back = read_cwg(args.output)
    if [m["name"] for m in back["maps"]] != g["order"] or back["start"] != g["start"]:
        raise ValueError(f"{args.output}: read-back mismatch")
    print(f"campaign: {len(maps)} maps, {len(g['components'])} components, "
          f"start {g['start']}, train {g['train'] or '-'} → {args.output} ({n} B)")
    for c in g["components"]:
        print("  " + " → ".join(c))
    placement_report = report(g)
    print(f"  global bounds: {placement_report['global_bounds']}")
    for m, placement in placement_report["maps"].items():
        print(f"  {m}: g={placement['gofs']} global={placement['global_bounds']} "
              f"ulp={placement['farthest_corner_ulp']:.9g}")
    for d in g["dropped"]:
        print(f"  note: dropped link {d}")
    for d in g["mismatch"]:
        print(f"  WARNING: landmark placement mismatch {d}")
    if args.json:
        args.json.write_text(json.dumps(placement_report, indent=1) + "\n")
    return 0


def main() -> int:
    try:
        return run()
    except OSError as error:
        print(f"hl_campaign: error: {error.filename or ''}: {error.strerror or error}", file=sys.stderr)
        return 1
    except (ValueError, struct.error) as error:
        print(f"hl_campaign: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
