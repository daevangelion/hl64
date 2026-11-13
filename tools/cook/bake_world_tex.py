# SPDX-License-Identifier: MIT
"""bake_world_tex: a BSP30 map's embedded miptex -> <map>.wci4 (CI4 + TLUT),
the texture source of the w64 TEXP pages (tools/w64c/w64c.py).

Imported from the quakegeneric tree (tools/world_tex_bake/bake_world_tex.py,
provenance-audit.md entry CLEAN) and retargeted to read the BSP30 texture
lump directly (the old tree read an intermediate `.q64` whose texels had
been remapped to the Quake palette).

Per texture, mip level 0 is point-sampled down to at most 64 x 64
(power-of-two sides); 16 palette entries are chosen (the most used colour,
then the most used colour of each of 15 luminance bands, then by use) and
every palette index is mapped to the nearest chosen colour (weighted
squared RGB distance, green x2). Fence textures (`{` prefix): CI4 slot 0 is
transparent (TLUT alpha 0); palette index 255 and pure blue map there.

Palette: by default each texture's own 256-colour palette (the BSP30
miptex palette after mip level 3). `--ref-palette FILE` (768-byte RGB)
first remaps every texel to the nearest colour of FILE (squared RGB
distance, lowest index on ties; fence index 255 and pure blue stay 255)
and quantises on FILE's colours: with the old tree's Quake palette this
reproduces the old cook's bytes exactly (W1, world-surfaces.md 5); hl64
never ships that palette.

Textures stored in WADs (miptex offsets 0) are read from the WAD3 files the
map's worldspawn `wad` key lists (their file names looked up in --wad-dir,
the game directory; first listed WAD holding the name wins); a texture
found in none is left out of the pack (w64c marks it missing) with a
warning.

WCI4 (big-endian): u32 magic 'WCI4', u16 version 1, u16 count, u32
ents_ofs (76), u32 data_ofs, u32 file_size, char mapname[56]; per texture
44 B: char name[16], u16 pw, ph, w, h, s32 uvu, uvv (16.16 page/source
scale), u32 ci4_ofs, ci4_bytes, tlut_ofs; data: per texture TLUT 16 x
RGBA5551 then the CI4 texels, 4-aligned.

  python3 -m tools.cook.bake_world_tex <map.bsp> -o <map.wci4> [--wad-dir DIR]
                                       [--ref-palette FILE]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import List, Optional, Tuple

from tools.cook import hl_classes

MAGIC = 0x57434934  # WCI4
BSP_VERSION = 30
LUMP_TEXTURES = 2
NUM_LUMPS = 15
MAX_MIPTEX = 512
MAX_SIDE = 512


class TexError(Exception):
    pass


def pow2floor(v: int) -> int:
    p = 1
    while p * 2 <= v:
        p *= 2
    return p


def build_ci4(pix: bytes, w: int, h: int, pal: bytes, fence: bool = False):
    pw = pow2floor(min(w, 64))
    ph = pow2floor(min(h, 64))
    uvu, uvv = pw / w, ph / h
    if pw != w or ph != h:
        src = bytearray(pw * ph)
        for y in range(ph):
            for x in range(pw):
                src[y * pw + x] = pix[(y * h // ph) * w + (x * w // pw)]
    else:
        src = bytearray(pix[: w * h])
    usage = [0] * 256
    for c in src:
        usage[c] += 1
    top: List[int] = []
    used: set = set()
    # fence: CI4 slot 0 = transparent (TLUT alpha 0), source index 255
    if fence:
        top.append(255)
        used.add(255)
    best = max(range(256), key=lambda c: usage[c] if c not in used else -1)
    if best not in used:
        top.append(best)
        used.add(best)
    for band in range(15):
        if len(top) >= 16:
            break
        lo, hi = band * 256 // 15, (band + 1) * 256 // 15
        bbest, buse = -1, 0
        for c in range(256):
            if usage[c] <= 0 or c in used:
                continue
            r, g, b = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
            yy = (r * 3 + g * 6 + b) // 10
            if lo <= yy < hi and usage[c] > buse:
                buse, bbest = usage[c], c
        if bbest >= 0:
            top.append(bbest)
            used.add(bbest)
    while len(top) < 16:
        c = max((i for i in range(256) if i not in used), key=lambda i: usage[i], default=0)
        top.append(c)
        used.add(c)
    tlut = []
    for s, c in enumerate(top):
        r, g, b = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
        a_bit = 0 if (fence and s == 0) else 1
        tlut.append(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | a_bit)
    remap = [0] * 256
    for i in range(256):
        if fence and i == 255:
            remap[i] = 0
            continue
        ir, ig, ib = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        if fence and ir == 0 and ig == 0 and ib == 255:
            remap[i] = 0
            continue
        bestd, bests = 10**9, 0
        start = 1 if fence else 0
        for s in range(start, len(top)):
            c = top[s]
            tr, tg, tb = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
            dist = (ir - tr) ** 2 + 2 * (ig - tg) ** 2 + (ib - tb) ** 2
            if dist < bestd:
                bestd, bests = dist, s
        remap[i] = bests
    ci4 = bytearray(pw * ph // 2)
    for y in range(ph):
        for x in range(0, pw, 2):
            a, b = remap[src[y * pw + x]], remap[src[y * pw + x + 1]]
            ci4[(y * pw + x) // 2] = (a << 4) | b
    return bytes(ci4), pw, ph, uvu, uvv, tlut


def nearest_index(pal: bytes, r: int, g: int, b: int) -> int:
    best, bestd = 0, 1 << 30
    for i in range(256):
        dr, dg, db = r - pal[i * 3], g - pal[i * 3 + 1], b - pal[i * 3 + 2]
        d = dr * dr + dg * dg + db * db
        if d < bestd:
            best, bestd = i, d
            if d == 0:
                break
    return best


def texture_lump(bsp: bytes, path: Path) -> bytes:
    if len(bsp) < 4 + 8 * NUM_LUMPS:
        raise TexError(f"{path}: shorter than a BSP30 header")
    version = struct.unpack_from("<i", bsp, 0)[0]
    if version != BSP_VERSION:
        raise TexError(f"{path}: BSP version {version}, want {BSP_VERSION}")
    ofs, length = struct.unpack_from("<ii", bsp, 4 + 8 * LUMP_TEXTURES)
    if ofs < 0 or length < 0 or ofs + length > len(bsp):
        raise TexError(f"{path}: texture lump {ofs}+{length} outside the {len(bsp)}-byte file")
    return bsp[ofs:ofs + length]


def parse_miptex(buf: bytes, tofs: int, what: str):
    """(name, w, h, mip0 indices, 768-byte palette, colours) of the miptex
    at buf[tofs:], or (name, w, h, None, None, 0) when its texels are in a
    WAD (offsets 0)."""
    if tofs + 40 > len(buf):
        raise TexError(f"{what}: header at {tofs} outside its lump")
    name = buf[tofs:tofs + 16].split(b"\0")[0].decode("latin-1")
    w, h, o0, o1, o2, o3 = struct.unpack_from("<6I", buf, tofs + 16)
    if w < 1 or h < 1 or w > MAX_SIDE or h > MAX_SIDE:
        raise TexError(f"{what} ({name!r}) is {w}x{h}")
    if o0 == 0:
        return name, w, h, None, None, 0
    m3 = (w >> 3) * (h >> 3)
    if tofs + o0 + w * h > len(buf) or tofs + o3 + m3 + 2 > len(buf):
        raise TexError(f"{what} ({name!r}) texels outside its lump")
    pix = buf[tofs + o0:tofs + o0 + w * h]
    pofs = tofs + o3 + m3
    ncolors = struct.unpack_from("<H", buf, pofs)[0]
    if ncolors > 256 or pofs + 2 + 3 * ncolors > len(buf):
        raise TexError(f"{what} ({name!r}) palette of {ncolors} colours outside its lump")
    pal = buf[pofs + 2:pofs + 2 + 3 * ncolors].ljust(768, b"\0")
    return name, w, h, pix, pal, ncolors


def read_miptex(lump: bytes, path: Path):
    """[(name, w, h, mip0 indices or None, palette, colours)] in lump order."""
    if len(lump) == 0:
        return []
    if len(lump) < 4:
        raise TexError(f"{path}: texture lump shorter than its count")
    n = struct.unpack_from("<i", lump, 0)[0]
    if n < 0 or n > MAX_MIPTEX or 4 + 4 * n > len(lump):
        raise TexError(f"{path}: texture lump count {n} invalid for {len(lump)} bytes")
    out = []
    for i in range(n):
        tofs = struct.unpack_from("<i", lump, 4 + 4 * i)[0]
        if tofs < 0:
            continue                       # no texture in this slot
        out.append(parse_miptex(lump, tofs, f"{path}: miptex {i}"))
    return out


class Wads:
    """Miptex lumps of the WAD3 files a map lists, looked up by name."""

    def __init__(self, bsp_path: Path, wad_dir: Optional[Path]):
        self.wads: List[Tuple[str, bytes, dict]] = []
        if wad_dir is None:
            return
        files = {p.name.lower(): p for p in Path(wad_dir).iterdir() if p.is_file()}
        ents = hl_classes.map_entities(bsp_path)
        listed = ents[0].get("wad", "") if ents else ""
        for item in listed.split(";"):
            base = item.replace("\\", "/").rsplit("/", 1)[-1].strip().lower()
            if not base or base not in files:
                continue
            data = files[base].read_bytes()
            self.wads.append((str(files[base]), data, self._dir(data, files[base])))

    @staticmethod
    def _dir(wad: bytes, path: Path) -> dict:
        if len(wad) < 12 or wad[:4] != b"WAD3":
            raise TexError(f"{path}: not a WAD3")
        n, dirofs = struct.unpack_from("<ii", wad, 4)
        if n < 0 or n > 1 << 16 or dirofs < 0 or dirofs + 32 * n > len(wad):
            raise TexError(f"{path}: directory outside the file")
        out = {}
        for i in range(n):
            pos, _ds, size, typ, comp, _pad, raw = struct.unpack_from("<iiiBBH16s", wad, dirofs + 32 * i)
            if typ != 0x43 or comp != 0:
                continue
            lname = raw.split(b"\0")[0].decode("latin-1").lower()
            if pos < 0 or size < 0 or pos + size > len(wad):
                raise TexError(f"{path}: lump {lname!r} outside the file")
            out.setdefault(lname, (pos, size))
        return out

    def find(self, name: str):
        for path, data, table in self.wads:
            hit = table.get(name.lower())
            if hit:
                pos, size = hit
                return parse_miptex(data[pos:pos + size], 0, f"{path}: {name}")
        return None


def remap_to(pix: bytes, pal: bytes, ncolors: int, ref: bytes, fence: bool) -> bytes:
    """Texels as indices into `ref`; entries past the palette's colours -> 0."""
    remap = [nearest_index(ref, pal[k * 3], pal[k * 3 + 1], pal[k * 3 + 2])
             if k < ncolors else 0 for k in range(256)]
    if fence:
        remap[255] = 255
        for k in range(ncolors):
            if pal[k * 3] == 0 and pal[k * 3 + 1] == 0 and pal[k * 3 + 2] == 255:
                remap[k] = 255
    return bytes(remap[c] for c in pix)


def be16(v): return struct.pack(">H", v & 0xFFFF)
def be32(v): return struct.pack(">I", v & 0xFFFFFFFF)
def bei32(v): return struct.pack(">i", int(v))


def bake(bsp_path: Path, ref: Optional[bytes], wad_dir: Optional[Path] = None) -> bytes:
    texs = read_miptex(texture_lump(bsp_path.read_bytes(), bsp_path), bsp_path)
    wads = None
    ents = []
    for name, w, h, pix, pal, ncolors in texs:
        if not name:
            continue
        if pix is None:
            if wads is None:
                try:
                    wads = Wads(bsp_path, wad_dir)
                except hl_classes.BspError as e:
                    raise TexError(str(e)) from None
            hit = wads.find(name)
            if hit is None:
                print(f"bake_world_tex: warning: {bsp_path}: texture {name!r} is in no listed WAD",
                      file=sys.stderr)
                continue
            _n, w, h, pix, pal, ncolors = hit
        fence = name.startswith("{")
        if ref is not None:
            pix, pal = remap_to(pix, pal, ncolors, ref, fence), ref
        ci4, pw, ph, uvu, uvv, tlut = build_ci4(pix, w, h, pal, fence=fence)
        ents.append((name, pw, ph, w, h, uvu, uvv, ci4, tlut))
    if len(ents) > 0xFFFF:
        raise TexError(f"{bsp_path}: too many textures")
    hdr_size = 4 + 2 + 2 + 4 + 4 + 4 + 56  # 76
    ent_size = 16 + 2 * 4 + 4 * 2 + 4 * 3  # 44
    ents_ofs = hdr_size
    data_ofs = (ents_ofs + len(ents) * ent_size + 3) & ~3
    cursor = data_ofs
    recs = []
    for name, pw, ph, ow, oh, uvu, uvv, ci4, tlut in ents:
        tlut_ofs = cursor
        ci4_ofs = cursor + 32
        recs.append((name, pw, ph, ow, oh, uvu, uvv, ci4_ofs, len(ci4), tlut_ofs, tlut, ci4))
        cursor = (ci4_ofs + len(ci4) + 3) & ~3
    file_size = cursor
    nameb = ("maps/" + bsp_path.stem).encode("ascii", "replace")[:55]
    buf = bytearray()
    buf += be32(MAGIC) + be16(1) + be16(len(recs))
    buf += be32(ents_ofs) + be32(data_ofs) + be32(file_size)
    buf += nameb + b"\0" * (56 - len(nameb))
    for name, pw, ph, ow, oh, uvu, uvv, ci4_ofs, ci4_bytes, tlut_ofs, _t, _c in recs:
        nb = name.encode("latin-1")[:15]
        buf += nb + b"\0" * (16 - len(nb))
        buf += be16(pw) + be16(ph) + be16(ow) + be16(oh)
        buf += bei32(int(uvu * 65536)) + bei32(int(uvv * 65536))
        buf += be32(ci4_ofs) + be32(ci4_bytes) + be32(tlut_ofs)
    buf += b"\0" * (data_ofs - len(buf))
    for *_, tlut_ofs, tlut, ci4 in recs:
        buf += b"\0" * (tlut_ofs - len(buf))
        for t in tlut:
            buf += be16(t)
        buf += ci4
        buf += b"\0" * (-len(buf) & 3)
    return bytes(buf)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bsp", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--wad-dir", type=Path, default=None,
                    help="directory holding the WAD3 files the map lists (the game directory)")
    ap.add_argument("--ref-palette", type=Path, default=None,
                    help="768-byte RGB palette to quantise through (W1 reproduction)")
    a = ap.parse_args(argv)
    try:
        ref = None
        if a.ref_palette is not None:
            ref = a.ref_palette.read_bytes()
            if len(ref) < 768:
                raise TexError(f"{a.ref_palette}: palette shorter than 768 bytes")
            ref = ref[:768]
        out = bake(a.bsp, ref, a.wad_dir)
    except TexError as e:
        print(f"bake_world_tex: error: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"bake_world_tex: error: {e.filename}: {e.strerror}", file=sys.stderr)
        return 1
    a.output.write_bytes(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
