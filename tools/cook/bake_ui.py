# SPDX-License-Identifier: MIT
"""bake_ui: the player's gfx.wad pictures -> gfx/<name>.ui8 (CI8; fonts I8).

Imported from the quakegeneric tree (tools/ui_bake/bake_ui.py and
extract_conchars.py, provenance-audit.md entries CLEAN) and retargeted from
Quake's WAD2 (one global palette) to Half-Life's WAD3, where every lump
carries its own palette: each texel is mapped to the nearest colour of the
runtime 2D palette (the player's gfx/palette.lmp, staged as
gsfx/palette.lmp; squared RGB distance over indices 0-254, lowest index on
ties); lump index 255 and pure blue (the GoldSrc colour key) become the
transparent index 255.

Lump types: 0x42 picture (u32 w, h, texels, u16 colours, palette) and 0x46
font (u32 cell width, height, rowcount, rowheight, 256 x {u16 offset, u16
width}, a 256-texel-wide glyph sheet of `height` rows, u16 colours,
palette); a font bakes as its 256 x height glyph sheet plus its glyph
table (<name>.fnt, below). Fonts are drawn additively in the text's colour
(GoldSrc text messages), so a font sheet is an intensity sheet: each texel
is the brightest channel of its lump palette colour (creditsfont: 17 greys,
its anti-aliased edges) and the colour key is 0; a keyed sheet would turn
every non-key texel, the black cell background included, into full
coverage.

UI8 (big-endian): u32 magic 'UI8 ', u16 version 1, u16 flags (1 = index
255 transparent, 2 = I8 intensity), u16 w, h, u32 pix_ofs (68), u32
file_size, char name[48], then w x h bytes.

  python3 -m tools.cook.bake_ui --wad gfx.wad --palette palette.lmp -o DIR [LUMP ...]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional

UI8_MAGIC = 0x55493820  # 'UI8 '
UI8_FLAG_KEYED = 1
UI8_FLAG_INTENSITY = 2
TRANSPARENT = 255
TYP_PIC, TYP_FONT = 0x42, 0x46
FONT_SHEET_W = 256
MAX_SIDE = 1024
MAX_LUMPS = 4096


class UiError(ValueError):
    pass


def read_wad3(wad: bytes, what: str) -> Dict[str, tuple]:
    """{lower-case name: (type, bytes)} of the uncompressed lumps."""
    if len(wad) < 12 or wad[:4] != b"WAD3":
        raise UiError(f"{what}: not a WAD3")
    n, dirofs = struct.unpack_from("<ii", wad, 4)
    if n < 0 or n > MAX_LUMPS or dirofs < 0 or dirofs + 32 * n > len(wad):
        raise UiError(f"{what}: directory {dirofs}+{n} entries outside the file")
    out = {}
    for i in range(n):
        pos, _dsize, size, typ, comp, _pad, raw = struct.unpack_from(
            "<iiiBBH16s", wad, dirofs + 32 * i)
        name = raw.split(b"\0", 1)[0].decode("latin-1").lower()
        if pos < 0 or size < 0 or pos + size > len(wad):
            raise UiError(f"{what}: lump {name!r} {pos}+{size} outside the file")
        if comp == 0:
            out.setdefault(name, (typ, wad[pos:pos + size]))
    return out


def lump_image(typ: int, b: bytes, what: str):
    """(w, h, texels, palette bytes) of a picture or font lump."""
    try:
        if typ == TYP_PIC:
            w, h = struct.unpack_from("<ii", b, 0)
            pix = 8
        elif typ == TYP_FONT:
            w, h = FONT_SHEET_W, struct.unpack_from("<i", b, 4)[0]
            pix = 16 + 4 * 256
        else:
            raise UiError(f"{what}: lump type 0x{typ:02x} is not a picture or font")
        if w < 1 or h < 1 or w > MAX_SIDE or h > MAX_SIDE or pix + w * h + 2 > len(b):
            raise UiError(f"{what}: bad {w}x{h} picture")
        texels = b[pix:pix + w * h]
        ncol = struct.unpack_from("<H", b, pix + w * h)[0]
        po = pix + w * h + 2
        if ncol > 256 or po + 3 * ncol > len(b):
            raise UiError(f"{what}: bad palette ({ncol} colours)")
        return w, h, texels, b[po:po + 3 * ncol].ljust(768, b"\0")
    except struct.error:
        raise UiError(f"{what}: truncated lump") from None


def nearest(pal: bytes, r: int, g: int, b: int) -> int:
    best, bd = 0, 1 << 30
    for i in range(255):
        d = (pal[3 * i] - r) ** 2 + (pal[3 * i + 1] - g) ** 2 + (pal[3 * i + 2] - b) ** 2
        if d < bd:
            best, bd = i, d
            if d == 0:
                break
    return best


def ui8_bytes(name: str, w: int, h: int, pix: bytes, flags: int = UI8_FLAG_KEYED) -> bytes:
    nb = name.encode("ascii", "replace")[:47]
    head = struct.pack(">IHHHHII", UI8_MAGIC, 1, flags, w, h, 68, 68 + w * h)
    return head + nb + b"\0" * (48 - len(nb)) + pix


def bake(wad: bytes, palette: bytes, names: Optional[List[str]] = None,
         what: str = "gfx.wad") -> Dict[str, bytes]:
    """{"<name>.ui8": bytes} for the picture/font lumps (all or `names`)."""
    if len(palette) < 768:
        raise UiError("palette shorter than 768 bytes")
    lumps = read_wad3(wad, what)
    want = names if names else sorted(n for n, (t, _) in lumps.items() if t in (TYP_PIC, TYP_FONT))
    out = {}
    for n in want:
        if n.lower() not in lumps:
            raise UiError(f"{what}: no lump {n!r}")
        typ, b = lumps[n.lower()]
        w, h, texels, pal = lump_image(typ, b, f"{what}:{n}")
        lut = []
        for i in range(256):
            r, g, bl = pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]
            key = i == 255 or (r, g, bl) == (0, 0, 255)
            if typ == TYP_FONT:
                lut.append(0 if key else max(r, g, bl))
            else:
                lut.append(TRANSPARENT if key else nearest(palette, r, g, bl))
        out[n.lower() + ".ui8"] = ui8_bytes(
            n.lower(), w, h, bytes(lut[t] for t in texels),
            UI8_FLAG_INTENSITY if typ == TYP_FONT else UI8_FLAG_KEYED)
        if typ == TYP_FONT:
            out[n.lower() + ".fnt"] = font_table(b, h, f"{what}:{n}")
    return out


FNT_MAGIC = 0x464E5431  # 'FNT1'


def font_table(b: bytes, h: int, what: str) -> bytes:
    """gfx/<font>.fnt: u32 'FNT1', u16 row height, u16 rows, 256 x {u16
    offset (y * 256 + x in the glyph sheet), u16 width}, big-endian."""
    try:
        rows, rowh = struct.unpack_from("<ii", b, 8)
        out = bytearray(struct.pack(">IHH", FNT_MAGIC, rowh, rows))
        for i in range(256):
            ofs, wid = struct.unpack_from("<HH", b, 16 + 4 * i)
            if (ofs >> 8) + rowh > h or (ofs & 255) + wid > FONT_SHEET_W:
                ofs, wid = 0, 0
            out += struct.pack(">HH", ofs, wid)
    except struct.error:
        raise UiError(f"{what}: truncated font table") from None
    if not 0 < rowh <= h:
        raise UiError(f"{what}: bad row height {rowh}")
    return bytes(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wad", type=Path, required=True)
    ap.add_argument("--palette", type=Path, required=True)
    ap.add_argument("-o", "--outdir", type=Path, required=True)
    ap.add_argument("lumps", nargs="*")
    a = ap.parse_args(argv)
    try:
        files = bake(a.wad.read_bytes(), a.palette.read_bytes(), a.lumps, str(a.wad))
    except (UiError, OSError) as e:
        print(f"bake_ui: error: {e}", file=sys.stderr)
        return 1
    a.outdir.mkdir(parents=True, exist_ok=True)
    for n, data in files.items():
        (a.outdir / n).write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
