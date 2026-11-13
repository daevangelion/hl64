# SPDX-License-Identifier: MIT
"""bake_hlhud: Half-Life HUD sprite sheets + hud.txt -> ui8 sheets and the
named HUD rectangle table.

Imported from the quakegeneric tree (tools/hlhud_bake/bake_hlhud.py,
provenance-audit.md entry CLEAN); retargeted to take the palette the
runtime draws ui8 sheets with (the player's gfx/palette.lmp, staged as
gsfx/palette.lmp) instead of the Quake palette.

Outputs:
  <gfxdir>/320hud1.ui8, 320hud2.ui8, crosshairs.ui8   (CI8, keyed)
  <gfxdir>/320hud4.ui8, 320_pain.ui8                  (I8 intensity)
  <header>  hl_hud_rects.h                            (named 320-res rects)

HL sprites are SPR v2 (IDSP/ver2): a 40-byte header, an embedded
256-colour palette, then indexed frames. GoldSrc draws the HUD additively,
so for the CI8 sheets:

  * pure/near-black is transparent
  * crosshairs.spr fills each 24x24 cell with flat gray (~77,77,77) - also
    transparent
  * remaining texels become a solid white silhouette (the palette entry
    nearest white) that the 2D renderer tints with the HUD colour

Colour-key index is 255 (TLUT alpha 0).

The damage-type icons (320hud4, grey) and the pain compass (320_pain, four
frames, pure red) keep their shading: each texel is its palette entry's
brightness (ui8 flag 2, I8), drawn as tint x brightness coverage; the hue
is applied by the caller. The pain frames are packed into one sheet and get
rects pain_up, pain_right, pain_down, pain_left (frames 0-3, the HL client's
pain indicator order).

UI8 (big-endian): u32 magic 'UI8 ', u16 version 1, u16 flags (1 keyed,
2 intensity), u16 w, h, u32 pix_ofs (68), u32 file_size, char name[48],
then w x h bytes.

The rectangle header is generated from the player's data: the cook writes
it into its staging directory, never into the repository.

  python3 -m tools.cook.bake_hlhud --hud DIR --palette FILE --gfxdir DIR --header FILE
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


class HudError(ValueError):
    pass


UI8_MAGIC = 0x55493820  # 'UI8 '
TRANSPARENT = 255       # ui8 colour-key index

# sheet name -> sheet id used by the HUD rects (the 2D renderer's sheet table)
SHEETS = ["320hud1", "320hud2", "crosshairs", "320hud4", "320_pain"]
# I8 brightness sheets (ui8 flag 2) instead of keyed CI8 silhouettes
INTENSITY = {"320hud4", "320_pain"}
UI8_FLAG_KEYED = 1
UI8_FLAG_INTENSITY = 2
# 320_pain.spr frame order (the HL client's pain indicator)
PAIN_NAMES = ["pain_up", "pain_right", "pain_down", "pain_left"]


def load_palette(d: bytes, what: str):
    if len(d) < 768:
        raise HudError(f"palette {what} too small ({len(d)} bytes)")
    return [(d[i * 3], d[i * 3 + 1], d[i * 3 + 2]) for i in range(256)]


def is_transparent_rgb(rgb):
    """Additive-style keys used by HL HUD sheets (including crosshair cell fill)."""
    r, g, b = rgb
    # Near-black (additive zero)
    if max(r, g, b) < 28:
        return True
    # Flat medium gray: crosshairs.spr pads each 24×24 cell with ~#4D4D4D
    if abs(r - g) <= 8 and abs(g - b) <= 8 and r < 100:
        return True
    return False


def nearest_index(qpal, rgb):
    best, bd = 0, 1 << 30
    r, g, b = rgb
    for i in range(255):  # skip transparent index 255
        pr, pg, pb = qpal[i]
        d = (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2
        if d < bd:
            bd, best = d, i
            if d == 0:
                break
    return best


def build_remap_cache(qpal):
    """Map an SPR palette colour -> CI8: transparent or solid white silhouette."""
    white = nearest_index(qpal, (255, 255, 255))
    cache = {}

    def remap(rgb):
        v = cache.get(rgb)
        if v is not None:
            return v
        if is_transparent_rgb(rgb):
            cache[rgb] = TRANSPARENT
            return TRANSPARENT
        # Solid silhouette for PRIM tint (ignore source luminance/AA)
        cache[rgb] = white
        return white

    return remap


def parse_hl_spr(d: bytes, what: str):
    """Return (palette, [(w, h, pixels)]) of a single-frame-type SPR v2."""
    try:
        ident, ver = struct.unpack_from("<4sI", d, 0)
        if ident != b"IDSP" or ver != 2:
            raise HudError(f"{what}: not an HL SPR v2 (ident={ident!r} ver={ver})")
        # header is 40 bytes: ident,ver,type,texfmt,radius,maxw,maxh,nframes,beamlen,sync
        numframes = struct.unpack_from("<i", d, 28)[0]
        if numframes < 1 or numframes > 64:
            raise HudError(f"{what}: {numframes} frames")
        off = 40
        palcount = struct.unpack_from("<H", d, off)[0]
        off += 2
        if palcount > 256 or off + palcount * 3 > len(d):
            raise HudError(f"{what}: bad palette ({palcount} colours)")
        pal = [(d[off + i * 3], d[off + i * 3 + 1], d[off + i * 3 + 2])
               for i in range(palcount)]
        off += palcount * 3
        frames = []
        for _ in range(numframes):
            # int type (SPR_SINGLE), origin[2], width, height, pixels
            ftype = struct.unpack_from("<i", d, off)[0]
            if ftype != 0:
                raise HudError(f"{what}: frame group (type {ftype}) not supported")
            off += 4
            _ox, _oy, w, h = struct.unpack_from("<iiii", d, off)
            off += 16
            if w < 1 or h < 1 or w > 1024 or h > 1024 or off + w * h > len(d):
                raise HudError(f"{what}: truncated or oversized {w}x{h} frame")
            frames.append((w, h, d[off:off + w * h]))
            off += w * h
    except struct.error:
        raise HudError(f"{what}: truncated sprite") from None
    return pal, frames


def ui8_bytes(name: str, w: int, h: int, pix: bytes, flags: int = UI8_FLAG_KEYED) -> bytes:
    pix_ofs = 68
    file_size = pix_ofs + w * h
    buf = bytearray()
    buf += struct.pack(">I", UI8_MAGIC)
    buf += struct.pack(">HH", 1, flags)        # ver=1, flags
    buf += struct.pack(">HH", w, h)
    buf += struct.pack(">II", pix_ofs, file_size)
    nb = name.encode("ascii")[:47]
    buf += nb + b"\0" * (48 - len(nb))
    assert len(buf) == pix_ofs
    buf += pix
    return bytes(buf)


def pack_frames(frames):
    """Wide frames stacked in a column at x 0, tall ones side by side right
    of it, all from y 0. Returns (w, h, [(x, y)])."""
    pos = [None] * len(frames)
    col_w = max((w for w, h, _ in frames if w >= h), default=0)
    y = 0
    for i, (w, h, _) in enumerate(frames):
        if w >= h:
            pos[i] = (0, y)
            y += h
    x, sheet_h = col_w, y
    for i, (w, h, _) in enumerate(frames):
        if w < h:
            pos[i] = (x, 0)
            x += w
            sheet_h = max(sheet_h, h)
    return x, sheet_h, pos


def bake_sheet(name: str, spr: bytes, remap):
    """Bake one sheet: (ui8 bytes, extra rects for multi-frame sheets)."""
    pal, frames = parse_hl_spr(spr, name + ".spr")
    if name in INTENSITY:
        lut = [max(pal[i]) if i < len(pal) else 0 for i in range(256)]
        flags = UI8_FLAG_INTENSITY
    else:
        lut = [remap(pal[i]) if i < len(pal) else TRANSPARENT
               for i in range(256)]
        flags = UI8_FLAG_KEYED
    if len(frames) == 1:
        w, h, pixels = frames[0]
        if w > 0xFFFF or h > 0xFFFF:
            raise HudError(f"{name}: {w}x{h} too large")
        return ui8_bytes(name, w, h, bytes(lut[p] for p in pixels), flags), []
    if name != "320_pain" or len(frames) != len(PAIN_NAMES):
        raise HudError(f"{name}: unexpected {len(frames)}-frame sheet")
    sw, sh, pos = pack_frames(frames)
    fill = 0 if flags == UI8_FLAG_INTENSITY else TRANSPARENT
    out = bytearray([fill]) * (sw * sh)
    rects = []
    sid = SHEETS.index(name)
    for i, ((w, h, pixels), (x, y)) in enumerate(zip(frames, pos)):
        for row in range(h):
            out[(y + row) * sw + x:(y + row) * sw + x + w] = \
                bytes(lut[p] for p in pixels[row * w:(row + 1) * w])
        rects.append((PAIN_NAMES[i], sid, x, y, w, h))
    return ui8_bytes(name, sw, sh, bytes(out), flags), rects


def parse_hud_txt(text: str):
    """Return ordered list of (name, sheet_id, x, y, w, h) for res==320."""
    rects = []
    seen = set()
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 7:
            continue
        name, res, sheet = parts[0], parts[1], parts[2]
        if res != "320":
            continue
        if sheet not in SHEETS:
            continue  # only sheets we bake
        try:
            x, y, w, h = (int(parts[3]), int(parts[4]),
                          int(parts[5]), int(parts[6]))
        except ValueError:
            continue
        if name in seen:
            continue
        seen.add(name)
        rects.append((name, SHEETS.index(sheet), x, y, w, h))
    return rects


def cident(name: str) -> str:
    return "HR_" + "".join(c if c.isalnum() else "_" for c in name)


def header_text(rects) -> str:
    lines = []
    lines.append("/* Generated by tools/cook/bake_hlhud.py from the player's hud.txt - do not edit. */")
    lines.append("#ifndef HL_HUD_RECTS_H")
    lines.append("#define HL_HUD_RECTS_H")
    lines.append("")
    lines.append("typedef struct { short sheet, x, y, w, h; } hl_rect_t;")
    lines.append("")
    lines.append("enum {")
    for i, (name, *_ ) in enumerate(rects):
        lines.append(f"\t{cident(name)} = {i},")
    lines.append(f"\tHR_COUNT = {len(rects)}")
    lines.append("};")
    lines.append("")
    lines.append("static const hl_rect_t hl_rects[HR_COUNT] = {")
    for (name, sheet, x, y, w, h) in rects:
        lines.append(f"\t{{ {sheet}, {x}, {y}, {w}, {h} }}, /* {name} */")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* HL_HUD_RECTS_H */")
    return "\n".join(lines) + "\n"


def bake(sprites, hud_txt: str, palette: bytes):
    """sprites: {sheet name: SPR bytes}. Returns ({file name: ui8 bytes},
    header text)."""
    remap = build_remap_cache(load_palette(palette, "palette"))
    out = {}
    extra = []
    for name in SHEETS:
        if name not in sprites:
            raise HudError(f"missing sprites/{name}.spr")
        data, rects = bake_sheet(name, sprites[name], remap)
        out[name + ".ui8"] = data
        extra += rects
    return out, header_text(parse_hud_txt(hud_txt) + extra)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hud", type=Path, required=True,
                    help="directory with the five sheet .spr files and hud.txt")
    ap.add_argument("--palette", type=Path, required=True)
    ap.add_argument("--gfxdir", type=Path, required=True)
    ap.add_argument("--header", type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        sprites = {n: (a.hud / (n + ".spr")).read_bytes() for n in SHEETS}
        files, header = bake(sprites, (a.hud / "hud.txt").read_text("latin-1"),
                             a.palette.read_bytes())
    except (HudError, OSError) as e:
        print(f"bake_hlhud: error: {e}", file=sys.stderr)
        return 1
    a.gfxdir.mkdir(parents=True, exist_ok=True)
    for n, data in files.items():
        (a.gfxdir / n).write_bytes(data)
    a.header.parent.mkdir(parents=True, exist_ok=True)
    a.header.write_text(header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
