# SPDX-License-Identifier: MIT
"""Unit tests of the content cook (tools/cook): synthetic inputs only, no
game data. Hostile-input cases check that every reader rejects malformed
files with its own error instead of a crash or an out-of-range read.

  python3 -m unittest tools.cook.test_cook
"""
from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from tools.cook import addramp, bake_hlhud, bake_ui, bake_world_tex, hl_classes
from tools.cook import hl_sound_classes, hl_sprite_cook
from tools.cook.data import Data, DataError


def pack(entries):
    """PACK archive of {name: bytes}."""
    body = b""
    table = b""
    ofs = 12
    for name, data in entries.items():
        table += struct.pack("<56sII", name.encode(), ofs, len(data))
        body += data
        ofs += len(data)
    return b"PACK" + struct.pack("<II", ofs, len(table)) + body + table


def bsp30(entities=b"", textures=b""):
    """BSP30 with an entity lump and a texture lump, other lumps empty."""
    lumps = [b""] * 15
    lumps[0] = entities
    lumps[2] = textures
    ofs = 4 + 8 * 15
    head = struct.pack("<i", 30)
    body = b""
    for lump in lumps:
        head += struct.pack("<ii", ofs + len(body), len(lump))
        body += lump
    return head + body


def miptex(name, w, h, pix, pal):
    """One embedded miptex: header, 4 mips, u16 colours, palette."""
    sizes = [w * h, (w // 2) * (h // 2), (w // 4) * (h // 4), (w // 8) * (h // 8)]
    ofs, o = [], 40
    for s in sizes:
        ofs.append(o)
        o += s
    mips = pix + bytes(sum(sizes[1:]))
    return (name.encode().ljust(16, b"\0") + struct.pack("<6I", w, h, *ofs)
            + mips + struct.pack("<H", 256) + pal)


def texlump(*tex):
    head = struct.pack("<i", len(tex))
    ofs = 4 + 4 * len(tex)
    body = b""
    for t in tex:
        head += struct.pack("<i", ofs + len(body))
        body += t
    return head + body


def spr(frames, palcount=256, texformat=0):
    head = b"IDSP" + struct.pack("<iiifiiiii", 2, 0, texformat, 1.0, 8, 8,
                                 len(frames), 0, 0)
    out = head + struct.pack("<h", palcount) + bytes(3 * palcount)
    for w, h, px in frames:
        out += struct.pack("<i4i", 0, 0, 0, w, h) + px
    return out


class TestData(unittest.TestCase):
    def test_loose_beats_pak_case_insensitive(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "pak0.pak").write_bytes(pack({"sound/A.wav": b"pak", "gfx/x.lmp": b"x"}))
            (root / "sound").mkdir()
            (root / "sound" / "a.wav").write_bytes(b"loose")
            data = Data(root)
            self.assertEqual(data.read("SOUND/a.WAV"), b"loose")
            self.assertEqual(data.read("gfx/X.lmp"), b"x")
            self.assertIsNone(data.read("nothing"))

    def test_bad_pack(self):
        cases = {
            "magic": b"KCAP" + bytes(8),
            "dir past end": b"PACK" + struct.pack("<II", 12, 640),
            "entry past end": b"PACK" + struct.pack("<II", 12, 64)
                               + struct.pack("<56sII", b"a", 0, 9999),
        }
        for what, blob in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                (Path(d) / "pak0.pak").write_bytes(blob)
                with self.assertRaises(DataError):
                    Data(Path(d))


class TestEntities(unittest.TestCase):
    def test_entities(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.bsp"
            p.write_bytes(bsp30(b'{\n"classname" "worldspawn"\n}\n{\n"a" "b"\n}\n\0'))
            self.assertEqual(hl_classes.map_entities(p),
                             [{"classname": "worldspawn"}, {"a": "b"}])

    def test_hostile_bsp(self):
        good = bsp30(b"{}")
        cases = {
            "short": good[:8],
            "version": struct.pack("<i", 31) + good[4:],
            "lump outside": good[:4] + struct.pack("<ii", 100, 1 << 20) + good[12:],
        }
        for what, blob in cases.items():
            with self.subTest(what), tempfile.TemporaryDirectory() as d:
                p = Path(d) / "m.bsp"
                p.write_bytes(blob)
                with self.assertRaises(hl_classes.BspError):
                    hl_classes.map_entities(p)


class TestWorldTex(unittest.TestCase):
    PAL = bytes(range(256)) * 3

    def bake(self, blob, ref=None):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.bsp"
            p.write_bytes(blob)
            return bake_world_tex.bake(p, ref)

    def test_one_texture(self):
        pix = bytes((x * 7 + y) & 255 for y in range(16) for x in range(16))
        out = self.bake(bsp30(textures=texlump(miptex("wall", 16, 16, pix, self.PAL))))
        magic, ver, n = struct.unpack_from(">IHH", out, 0)
        self.assertEqual((magic, ver, n), (bake_world_tex.MAGIC, 1, 1))
        name, pw, ph, w, h = struct.unpack_from(">16s4H", out, 76)
        self.assertEqual((name.rstrip(b"\0"), pw, ph, w, h), (b"wall", 16, 16, 16, 16))
        self.assertEqual(len(out) % 4, 0)

    def test_fence_slot0_transparent(self):
        pix = bytes([255] * 128 + [3] * 128)
        out = self.bake(bsp30(textures=texlump(miptex("{grate", 16, 16, pix, self.PAL))))
        tlut_ofs = struct.unpack_from(">I", out, 76 + 40)[0]
        self.assertEqual(struct.unpack_from(">H", out, tlut_ofs)[0] & 1, 0)
        ci4_ofs = struct.unpack_from(">I", out, 76 + 32)[0]
        self.assertEqual(out[ci4_ofs], 0)          # index 255 -> slot 0

    def test_fence_slot0_edge_fill(self):
        # index 255 blue (the retail '{' palettes); rows 0-3 holes, rows 4
        # and 15 (next to them, wrapping) colour 10, the rest colour 20:
        # slot 0 carries colour 10, not blue, so a bilinear alpha edge
        # blends towards the fence's own colour
        pal = bytearray(768)
        pal[255 * 3:256 * 3] = bytes((0, 0, 255))
        pal[10 * 3:11 * 3] = bytes((200, 100, 50))
        pal[20 * 3:21 * 3] = bytes((40, 40, 40))
        pix = bytes([255] * 64 + [10] * 16 + [20] * 160 + [10] * 16)
        out = self.bake(bsp30(textures=texlump(miptex("{grate", 16, 16, pix, bytes(pal)))))
        tlut_ofs = struct.unpack_from(">I", out, 76 + 40)[0]
        slot0 = struct.unpack_from(">H", out, tlut_ofs)[0]
        self.assertEqual(slot0, (200 >> 3) << 11 | (100 >> 3) << 6 | (50 >> 3) << 1)
        ci4_ofs = struct.unpack_from(">I", out, 76 + 32)[0]
        self.assertEqual(out[ci4_ofs:ci4_ofs + 32], bytes(32))   # holes stay slot 0
        # a non-fence texture with the same texels keeps every slot opaque
        out = self.bake(bsp30(textures=texlump(miptex("grate", 16, 16, pix, bytes(pal)))))
        tlut_ofs = struct.unpack_from(">I", out, 76 + 40)[0]
        self.assertTrue(all(t & 1 for t in struct.unpack_from(">16H", out, tlut_ofs)))

    def test_fence_fill_fallbacks(self):
        pal = bytearray(768)
        pal[255 * 3:256 * 3] = bytes((0, 0, 255))
        pal[7 * 3:8 * 3] = bytes((0, 0, 255))      # pure blue: transparent too
        pal[1 * 3:2 * 3] = bytes((10, 20, 30))
        pal[2 * 3:3 * 3] = bytes((30, 40, 51))
        pal = bytes(pal)
        self.assertEqual(bake_world_tex.fence_fill([(bytes([255, 7] * 8), 4, 4)], pal), (0, 0, 0))
        self.assertEqual(bake_world_tex.fence_fill([(bytes([1, 2] * 8), 4, 4)], pal), (20, 30, 41))
        # the edge mean ignores opaque texels with no transparent neighbour
        # (the hole at 0,0 sees rows 3, 0, 1 and columns 3, 0, 1: wrapping)
        img = bytes([7, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 2, 1])
        self.assertEqual(bake_world_tex.fence_fill([(img, 4, 4)], pal), (10, 20, 30))
        # several images sum; clear_index: only that index is transparent
        # (blue 7 then counts as opaque: no edge, the mean of all)
        self.assertEqual(bake_world_tex.fence_fill([(img, 4, 4), (bytes([2] * 16), 4, 4)], pal),
                         (10, 20, 30))
        self.assertEqual(bake_world_tex.fence_fill([(bytes([7, 1] * 8), 4, 4)], pal, 255),
                         (5, 10, 143))

    def test_wad_texture_skipped(self):
        wadtex = b"ext".ljust(16, b"\0") + struct.pack("<6I", 16, 16, 0, 0, 0, 0)
        out = self.bake(bsp30(textures=texlump(wadtex)))
        self.assertEqual(struct.unpack_from(">H", out, 6)[0], 0)

    def test_wad_texture_found(self):
        pix = bytes(range(256))
        wadtex = b"ext".ljust(16, b"\0") + struct.pack("<6I", 16, 16, 0, 0, 0, 0)
        lump = miptex("EXT", 16, 16, pix, self.PAL)
        wad = (b"WAD3" + struct.pack("<ii", 1, 12 + len(lump)) + lump
               + struct.pack("<iiiBBH16s", 12, len(lump), len(lump), 0x43, 0, 0, b"EXT"))
        ents = b'{\n"classname" "worldspawn"\n"wad" "\\half-life\\valve\\none.wad;\\half-life\\valve\\Tex.wad"\n}\n'
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "TEX.WAD").write_bytes(wad)
            p = Path(d) / "m.bsp"
            p.write_bytes(bsp30(ents, texlump(wadtex)))
            out = bake_world_tex.bake(p, None, Path(d))
            self.assertEqual(struct.unpack_from(">H", out, 6)[0], 1)
            (Path(d) / "TEX.WAD").write_bytes(b"WAD2" + bytes(8))
            with self.assertRaises(bake_world_tex.TexError):
                bake_world_tex.bake(p, None, Path(d))

    def test_hostile(self):
        tex = miptex("wall", 16, 16, bytes(256), self.PAL)
        cases = {
            "version": struct.pack("<i", 29) + bsp30()[4:],
            "count": bsp30(textures=struct.pack("<i", 100000)),
            "miptex outside": bsp30(textures=struct.pack("<ii", 1, 4000)),
            "texels outside": bsp30(textures=texlump(tex[:200])),
            "size": bsp30(textures=texlump(tex[:16] + struct.pack("<6I", 0, 16, 40, 0, 0, 0))),
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(bake_world_tex.TexError):
                    self.bake(blob)


class TestSprite(unittest.TestCase):
    def test_cook(self):
        out = hl_sprite_cook.cook(spr([(8, 8, bytes(range(64)))]))
        self.assertEqual(struct.unpack_from(">IHH", out, 0), (hl_sprite_cook.MAGIC, 3, 1))
        self.assertEqual(struct.unpack_from(">H", out, 10)[0], 1)
        self.assertEqual(len(out) % 8, 0)

    def test_group(self):
        # frame 0 single, frame 1 a group of 3 with cumulative intervals
        head = b"IDSP" + struct.pack("<iiifiiiii", 2, 0, 0, 1.0, 8, 8, 2, 0, 0)
        blob = head + struct.pack("<h", 256) + bytes(768)
        blob += struct.pack("<i4i", 0, 0, 0, 8, 8) + bytes(64)
        blob += struct.pack("<ii3f", 1, 3, 0.1, 0.3, 0.6)
        for k in range(3):
            blob += struct.pack("<4i", 0, 0, 8, 8) + bytes([k]) * 64
        out = hl_sprite_cook.cook(blob)
        nframes, nimages = struct.unpack_from(">H", out, 6)[0], struct.unpack_from(">H", out, 10)[0]
        self.assertEqual((nframes, nimages), (2, 4))
        fo = 88 + 16 * nimages
        self.assertEqual(struct.unpack_from(">4H", out, fo), (0, 1, 1, 3))
        self.assertEqual(struct.unpack_from(">4H", out, fo + 8), (0, 100, 300, 600))
        bad = blob.replace(struct.pack("<3f", 0.1, 0.3, 0.6), struct.pack("<3f", 0.3, 0.1, 0.6))
        with self.assertRaises(ValueError):
            hl_sprite_cook.cook(bad)

    def test_indexalpha(self):
        # INDEXALPHA (texformat 2): alpha = index, colour = the last colour
        pal = bytearray(768)
        pal[765:768] = bytes((200, 100, 50))
        head = b"IDSP" + struct.pack("<iiifiiiii", 2, 0, 2, 1.0, 16, 16, 1, 0, 0)
        blob = head + struct.pack("<h", 256) + bytes(pal)
        blob += struct.pack("<i4i", 0, 0, 0, 16, 16) + bytes(range(256))
        out = hl_sprite_cook.cook(blob)
        ia, lo, hi = addramp.unpack(out[48:88])
        self.assertEqual(ia, [0xFF00 | 17 * k for k in range(16)])
        self.assertEqual((lo, hi), ((200, 100, 50), (200, 100, 50)))
        self.assertEqual(struct.unpack_from(">H", out, 16)[0], 0)      # level 0 transparent
        ofs = struct.unpack_from(">I", out, 88 + 12)[0]
        texels = [v for b in out[ofs:ofs + 128] for v in (b >> 4, b & 15)]
        self.assertEqual(texels, [(i * 15 + 127) // 255 for i in range(256)])

    def test_alphatest_edge_fill(self):
        # ALPHTEST (texformat 3), index 255 blue as in blood.spr: the
        # transparent entry carries the colour next to the holes (index 9,
        # rows 0 and 3 of 4 wrap onto the hole rows 1-2), not blue
        pal = bytearray(768)
        pal[765:768] = bytes((0, 0, 255))
        pal[9 * 3:10 * 3] = bytes((160, 16, 8))
        pal[4 * 3:5 * 3] = bytes((60, 60, 60))
        head = b"IDSP" + struct.pack("<iiifiiiii", 2, 0, 3, 1.0, 8, 4, 1, 0, 0)
        px = bytes([9] * 8 + [255] * 16 + [9] * 8)
        out = hl_sprite_cook.cook(head + struct.pack("<h", 256) + bytes(pal)
                                  + struct.pack("<i4i", 0, 0, 0, 8, 4) + px)
        self.assertEqual(struct.unpack_from(">H", out, 16)[0],
                         (160 >> 3) << 11 | (16 >> 3) << 6 | (8 >> 3) << 1)

    def test_ramp_in_file(self):
        out = hl_sprite_cook.cook(spr([(8, 8, bytes(range(64)))]))
        self.assertEqual(out[48:88], addramp.ramp(struct.unpack_from(">16H", out, 16)))

    def test_hostile(self):
        good = spr([(8, 8, bytes(64))])
        cases = {
            "truncated": good[:50],
            "frame past end": good[:-10],
            "palette count": good[:40] + struct.pack("<h", 300) + good[42:],
            "magic": b"IDSQ" + good[4:],
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(ValueError):
                    hl_sprite_cook.cook(blob)


class TestAddRamp(unittest.TestCase):
    @staticmethod
    def rgba(r, g, b, a=1):
        return (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | a

    def test_greys_exact(self):
        tl = [self.rgba(17 * k, 17 * k, 17 * k) for k in range(16)]
        ia, lo, hi = addramp.unpack(addramp.ramp(tl))
        self.assertEqual((lo, hi), ((255, 255, 255), (255, 255, 255)))
        for k, t in enumerate(tl):
            self.assertEqual(ia[k] & 255, addramp.c8((t >> 11) & 31), k)

    def test_transparent_and_black(self):
        tl = [self.rgba(255, 0, 0, 0), 0] + [self.rgba(255, 8 * k, 0) for k in range(14)]
        ia, lo, hi = addramp.unpack(addramp.ramp(tl))
        self.assertEqual(ia[:2], [0, 0])
        self.assertEqual(addramp.unpack(addramp.ramp([0] * 16))[0], [0] * 16)

    def test_two_colour_ramp(self):
        # red .. yellow entries at full brightness: the ends are the
        # palette's own red and most yellow colour (RGBA5551-expanded),
        # every entry rebuilt within one RGBA5551 step
        tl = [self.rgba(255, 16 * k, 0) for k in range(16)]
        ia, lo, hi = addramp.unpack(addramp.ramp(tl))
        self.assertEqual({lo, hi}, {(255, 0, 0), (255, addramp.c8(240 >> 3), 0)})
        for k, t in enumerate(tl):
            c = [addramp.c8((t >> s) & 31) for s in (11, 6, 1)]
            a, i = (ia[k] & 255) / 255, (ia[k] >> 8) / 255
            v = [(lo[j] + (hi[j] - lo[j]) * i) * a for j in range(3)]
            self.assertTrue(all(abs(v[j] - c[j]) <= 8 for j in range(3)), (k, v, c))


class TestHud(unittest.TestCase):
    def test_hostile_sprite(self):
        with self.assertRaises(bake_hlhud.HudError):
            bake_hlhud.parse_hl_spr(spr([(8, 8, bytes(64))])[:60], "x.spr")

    def test_rects(self):
        rects = bake_hlhud.parse_hud_txt("2\nd_9mm 320 320hud1 0 0 32 16\nd_9mm 640 640hud1 0 0 1 1\n")
        self.assertEqual(rects, [("d_9mm", 0, 0, 0, 32, 16)])


class TestUi(unittest.TestCase):
    def wad(self, lumps):
        body, table = b"", b""
        ofs = 12
        for name, typ, data in lumps:
            table += struct.pack("<iiiBBH16s", ofs + len(body), len(data), len(data),
                                 typ, 0, 0, name.encode())
            body += data
        return b"WAD3" + struct.pack("<ii", len(lumps), ofs + len(body)) + body + table

    def test_pic(self):
        pal = bytearray(768)
        pal[3:6] = b"\xff\xff\xff"
        pal[6:9] = b"\x00\x00\xff"
        pic = struct.pack("<ii", 2, 2) + bytes([1, 2, 255, 1]) + struct.pack("<H", 256) + pal
        gpal = bytearray(768)
        gpal[30:33] = b"\xff\xff\xfe"
        out = bake_ui.bake(self.wad([("lambda", 0x42, pic)]), bytes(gpal))["lambda.ui8"]
        self.assertEqual(out[68:], bytes([10, 255, 255, 10]))

    def test_font_is_intensity(self):
        # creditsfont-like: black cell background (index 0), greys, the key;
        # a keyed bake made the background full coverage
        pal = bytearray(768)
        pal[3:6] = b"\x80\x80\x80"
        pal[6:9] = b"\xff\xff\xff"
        pal[9:12] = b"\x00\x00\xff"
        table = struct.pack("<HH", 0, 4) * 256
        sheet = bytes([0, 1, 2, 255] + [3, 0, 1, 2] + [0] * 248) + bytes(256)
        font = (struct.pack("<iiii", 4, 2, 1, 2) + table + sheet + struct.pack("<H", 256)
                + pal)
        out = bake_ui.bake(self.wad([("creditsfont", 0x46, font)]), bytes(768))
        ui8 = out["creditsfont.ui8"]
        self.assertEqual(struct.unpack_from(">HHH", ui8, 6), (bake_ui.UI8_FLAG_INTENSITY, 256, 2))
        self.assertEqual(ui8[68:76], bytes([0, 128, 255, 0, 0, 0, 128, 255]))
        self.assertIn("creditsfont.fnt", out)

    def test_hostile(self):
        cases = {
            "magic": b"WAD2" + bytes(8),
            "dir": b"WAD3" + struct.pack("<ii", 5, 1 << 20),
            "pic": self.wad([("x", 0x42, struct.pack("<ii", 64, 64) + bytes(10))]),
        }
        for what, blob in cases.items():
            with self.subTest(what):
                with self.assertRaises(bake_ui.UiError):
                    bake_ui.bake(blob, bytes(768))


class TestSound(unittest.TestCase):
    def wav(self, rate, cue=None):
        fmt = struct.pack("<HHIIHH", 1, 1, rate, rate, 1, 8)
        out = b"fmt " + struct.pack("<I", len(fmt)) + fmt
        out += b"data" + struct.pack("<I", 4) + bytes(4)
        if cue is not None:
            c = struct.pack("<I", 1) + struct.pack("<II4sIII", 0, 0, b"data", 0, 0, cue)
            out += b"cue " + struct.pack("<I", len(c)) + c
        return b"RIFF" + struct.pack("<I", 4 + len(out)) + b"WAVE" + out

    def test_plan(self):
        rules = hl_sound_classes.parse_rules(hl_sound_classes.RULES)
        self.assertEqual(hl_sound_classes.plan(rules, "scientist/x.wav", self.wav(44100))[:3],
                         ("voice", "vadpcm,huffman=true", 22050))
        self.assertEqual(hl_sound_classes.plan(rules, "misc/hum.wav", self.wav(11025, 77)),
                         ("ambience", "vadpcm,huffman=false", 0, 77))
        self.assertEqual(hl_sound_classes.plan(rules, "weapons/x.wav", self.wav(11025))[0], "fx")

    def test_hostile(self):
        for blob in (b"RIFX" + bytes(8), b"RIFF\0\0\0\0WAVE", self.wav(22050)[:12]):
            with self.subTest(blob[:4]):
                with self.assertRaises(hl_sound_classes.SoundError):
                    hl_sound_classes.wav_info(blob, "x.wav")


if __name__ == "__main__":
    unittest.main()
