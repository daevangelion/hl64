# SPDX-License-Identifier: MIT
"""decals: the cooked DI4 decal sheets and the decal region proof.

Two kinds of sheet, one format (DI4 2, big-endian; engine/render/decal.c
load_sheet reads both):

  gsfx/decals.i4   the decals the game shoots, in HLSDK decals.h order
                   (dlls/world.cpp gDecals: entry i = the game's decal
                   index i, DECAL_NAMES)
  DECL chunk       one per map package (docs/formats/w64-format.md "DECL"):
                   the map's placed (infodecal) textures that are not in
                   DECAL_NAMES, sorted by name; absent when there are none

Layout: u32 magic 'DI4'2, u16 count, u16 28, then per decal name[16]
(lower case), u8 texel w, h, world w, h (the lump's size), u8 tint r, g, b
(palette[255]), pad, u32 data offset (from the sheet start; a name listed
twice shares its texels); then I4 texels (high nibble first, 8-aligned):
the '{' decal's palette index is its alpha, quantised to 4 bits; sides over
DECAL_I4_MAX averaged down by 2 until they fit (scorch 96 -> 48 texels,
still 96 units); a texel width that is not a multiple of 16 (TMEM rows) is
resampled linearly to the nearest one, at least 16.
Decal ids (the infodecal `texture` key, docs/formats/hle-format-v1.md):
id < len(DECAL_NAMES) is the global sheet's entry, id >= len(DECAL_NAMES)
the entry id - len(DECAL_NAMES) of the record's own map's DECL chunk.

Region (architecture.md 4.1 decals): the global sheet at the start, two map
slots (one per open layer, each half of what the sheet and the piece
vertices leave), the piece vertices at the end:
align16(global) + 2 x align16(largest DECL) + HL64_FX_DECALS x
HL64_FX_DECAL_VERTS x 10 B must fit HL64_DECAL_TEXELS_KB.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple

from tools.restab import restab

# GoldSrc decals the game shoots, in HLSDK decals.h order (dlls/world.cpp
# gDecals: index -> decals.wad name): the sheet's entry i is decal index i,
# so the engine maps the game's index without names.
DECAL_NAMES = (
    [f"{{shot{i}" for i in range(1, 6)] + [f"{{lambda0{i}" for i in range(1, 7)]
    + ["{scorch1", "{scorch2"] + [f"{{blood{i}" for i in range(1, 7)]
    + [f"{{yblood{i}" for i in range(1, 7)] + [f"{{break{i}" for i in range(1, 4)]
    + [f"{{bigshot{i}" for i in range(1, 6)] + ["{spit1", "{spit2", "{bproof1", "{gargstomp"]
    + [f"{{smscorch{i}" for i in range(1, 4)] + ["{mommablob", "{mommablob"])
GLOBAL_COUNT = len(DECAL_NAMES)
DECAL_I4_MAX = 64            # texels per side (TMEM); larger lumps are halved
SHEET_MAGIC = 0x44493402
SHEET_ENTRY = 28
SHEET_MAX = 64               # engine/render/decal.c: decals in one sheet
DVERT_BYTES = 10             # engine/render/decal.c dvert_t (pos s16[3], st s16[2])
DECL = b"DECL"


class DecalError(Exception):
    pass


def _lerp(px: List[int], row: int, w: int, s: float) -> int:
    """Row `row`'s value at fractional texel s (clamped), rounded."""
    s = min(max(s, 0.0), w - 1.0)
    a = int(s)
    b = min(a + 1, w - 1)
    f = s - a
    return int(px[row + a] * (1.0 - f) + px[row + b] * f + 0.5)


def global_id(name: str) -> int:
    """The global sheet entry of a decals.wad name, -1 none (the first of
    a name listed twice)."""
    n = name.lower()
    return DECAL_NAMES.index(n) if n in DECAL_NAMES else -1


def wad_lumps(wad: bytes, what: str) -> Dict[str, Tuple[int, int]]:
    """decals.wad lump name (lower case) -> (offset, size) of its miptex."""
    if len(wad) < 12 or wad[:4] != b"WAD3":
        raise DecalError(f"{what}: not a WAD3")
    count, dirofs = struct.unpack_from("<ii", wad, 4)
    if count < 0 or dirofs < 0 or dirofs + 32 * count > len(wad):
        raise DecalError(f"{what}: directory outside the file")
    lumps: Dict[str, Tuple[int, int]] = {}
    for i in range(count):
        pos, _dsize, size, typ, _cmp, _pad, raw = struct.unpack_from(
            "<iiibbh16s", wad, dirofs + 32 * i)
        if typ == 0x43:
            lumps[raw.split(b"\0")[0].decode("latin-1").lower()] = (pos, size)
    return lumps


def sheet(wad: bytes, names: Sequence[str], what: str) -> bytes:
    """A DI4 sheet of the named decals.wad lumps, entry i = names[i]."""
    lumps = wad_lumps(wad, what)
    missing = [n for n in names if n not in lumps]
    if missing:
        raise DecalError(f"{what}: decals missing: " + ", ".join(missing))
    if not 0 < len(names) <= SHEET_MAX:
        raise DecalError(f"{what}: {len(names)} decals in one sheet (1..{SHEET_MAX})")
    entries, blobs, done = [], [], {}
    ofs = 8 + SHEET_ENTRY * len(names)
    ofs = (ofs + 7) & ~7
    for name in names:
        if name in done:
            entries.append(done[name])
            continue
        pos, size = lumps[name]
        if pos < 0 or size < 40 or pos + size > len(wad):
            raise DecalError(f"{what}: decal {name} outside the file")
        w, h, o0, _o1, _o2, o3 = struct.unpack_from("<II4I", wad, pos + 16)
        if w < 1 or h < 1 or w > 255 or h > 255:
            raise DecalError(f"{what}: decal {name} is {w}x{h}")
        pal = pos + o3 + (w // 8) * (h // 8) + 2
        if pos + o0 + w * h > pos + size or pal + 256 * 3 > pos + size:
            raise DecalError(f"{what}: decal {name} texels or palette outside its lump")
        tint = wad[pal + 255 * 3:pal + 256 * 3]
        px = list(wad[pos + o0:pos + o0 + w * h])
        tw, th = w, h
        while tw > DECAL_I4_MAX or th > DECAL_I4_MAX:
            px = [(px[(2 * y) * tw + 2 * x] + px[(2 * y) * tw + 2 * x + 1]
                   + px[(2 * y + 1) * tw + 2 * x] + px[(2 * y + 1) * tw + 2 * x + 1] + 2) // 4
                  for y in range(th // 2) for x in range(tw // 2)]
            tw, th = tw // 2, th // 2
        if tw % 16 and tw > 0:
            # TMEM rows: the texel width is a multiple of 16; resample each
            # row linearly to the nearest one (the world size stays the lump's)
            nw = max(16, (tw + 8) // 16 * 16)
            px = [_lerp(px, y * tw, tw, (x + 0.5) * tw / nw - 0.5)
                  for y in range(th) for x in range(nw)]
            tw = nw
        if tw < 16 or th < 1:
            raise DecalError(f"{what}: decal {name}: {tw}x{th} texels not storable")
        q = [(v * 15 + 127) // 255 for v in px]
        blob = bytes((q[k] << 4) | q[k + 1] for k in range(0, len(q), 2))
        entries.append(name.encode("latin-1").ljust(16, b"\0")
                       + bytes((tw, th, w, h)) + tint + b"\0"
                       + struct.pack(">I", ofs))
        done[name] = entries[-1]
        blobs.append(blob.ljust((len(blob) + 7) & ~7, b"\0"))
        ofs += len(blobs[-1])
    head = struct.pack(">IHH", SHEET_MAGIC, len(names), SHEET_ENTRY) + b"".join(entries)
    return head.ljust((len(head) + 7) & ~7, b"\0") + b"".join(blobs)


def sheet_names(b: bytes, what: str) -> List[str]:
    """Entry names of a DI4 sheet, checked with decal.c load_sheet's rules."""
    if len(b) < 8 or struct.unpack_from(">I", b)[0] != SHEET_MAGIC \
            or struct.unpack_from(">H", b, 6)[0] != SHEET_ENTRY:
        raise DecalError(f"{what}: not a DI4 2 sheet")
    n = struct.unpack_from(">H", b, 4)[0]
    if not 0 < n <= SHEET_MAX or 8 + n * SHEET_ENTRY > len(b):
        raise DecalError(f"{what}: {n} entries")
    out = []
    for i in range(n):
        e = 8 + SHEET_ENTRY * i
        tw, th, ww, wh = b[e + 16:e + 20]
        ofs = struct.unpack_from(">I", b, e + 24)[0]
        if not tw or not th or tw > 64 or th > 64 or tw & 15 or ofs & 7 \
                or ofs > len(b) or tw * th // 2 > len(b) - ofs or not ww or not wh:
            raise DecalError(f"{what}: entry {i}")
        out.append(b[e:e + 16].split(b"\0")[0].decode("latin-1"))
    return out


@dataclass
class Region:
    global_bytes: int
    map_name: str        # the largest DECL ("" none)
    map_bytes: int
    verts: int
    total: int


def _a16(n: int) -> int:
    return (n + 15) & ~15


def region_need(global_bytes: int, decl: Dict[str, int], limits: str) -> Region:
    """The decal region the runtime takes for this campaign (module doc)."""
    name = max(decl, key=lambda m: (decl[m], m)) if decl else ""
    big = decl[name] if name else 0
    verts = (restab.read_limit(limits, "HL64_FX_DECALS")
             * restab.read_limit(limits, "HL64_FX_DECAL_VERTS") * DVERT_BYTES)
    return Region(global_bytes, name, big, verts, _a16(global_bytes) + 2 * _a16(big) + verts)


def check_region(r: Region, limits: str) -> int:
    """Fail above HL64_DECAL_TEXELS_KB; returns the carve in bytes."""
    kb = restab.read_limit(limits, "HL64_DECAL_TEXELS_KB")
    if r.total > kb * 1024:
        raise DecalError(f"decal region need {r.total} B ({describe(r)}) exceeds "
                         f"HL64_DECAL_TEXELS_KB {kb} ({kb * 1024} B): raise it in {limits}")
    return kb * 1024


def describe(r: Region) -> str:
    return (f"global sheet {r.global_bytes} + 2 x map sheet {r.map_name or '-'} {r.map_bytes}, "
            f"16-aligned, + piece vertices {r.verts}")
