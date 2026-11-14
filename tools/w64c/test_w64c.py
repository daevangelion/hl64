# SPDX-License-Identifier: MIT
"""Unit tests for tools.w64c.w64c on self-contained synthetic data (no Valve
data): world-surfaces.md W2 (luxel addressing), W3 (probe walk), the cook
half of W4 (animation tables), W5 (animation cook errors), and hostile-input
rejection of the BSP / WCI4 / HLE loaders and the CLI.

  python3 -m unittest tools.w64c.test_w64c
"""
from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

from tools.w64c import w64c as W

ROOT = Path(__file__).resolve().parents[2]


# --------------------------------------------------------------------------
# synthetic BSP30 writer
# --------------------------------------------------------------------------

class Map:
    """Builds a BSP30 file from Python lists (lump order of the format)."""

    def __init__(self):
        self.ents = b'{\n"classname" "worldspawn"\n}\n\0'
        self.planes, self.verts, self.nodes, self.texinfo = [], [], [], []
        self.faces, self.leafs, self.mark, self.edges = [], [], [], [(0, 0)]
        self.surfedges, self.models, self.clip = [], [], []
        self.texnames = []
        self.light = b""
        self.vis = b""

    def plane(self, n, d):
        t = n.index(1.0) if sorted(n) == [0.0, 0.0, 1.0] else 3
        self.planes.append((*n, d, t))
        return len(self.planes) - 1

    def tex(self, name, s=(1, 0, 0, 0), t=(0, 1, 0, 0)):
        if name not in self.texnames:
            self.texnames.append(name)
        self.texinfo.append((*s, *t, self.texnames.index(name), 0))
        return len(self.texinfo) - 1

    def face(self, pts, plane, ti, styles=(0, 255, 255, 255), lightofs=-1):
        first = len(self.surfedges)
        base = len(self.verts)
        self.verts += pts
        for i in range(len(pts)):
            self.edges.append((base + i, base + (i + 1) % len(pts)))
            self.surfedges.append(len(self.edges) - 1)
        self.faces.append((plane, 0, first, len(pts), ti, *styles, lightofs))
        return len(self.faces) - 1

    def lightmap(self, values):
        """Append mono luxels as RGB (R = value, G = value / 2, B = 0);
        returns the byte offset."""
        ofs = len(self.light)
        self.light += bytes(c for v in values for c in (v, v // 2, 0))
        return ofs

    def build(self) -> bytes:
        tx = struct.pack("<i", len(self.texnames))
        mips = b""
        offs = []
        for nm in self.texnames:
            offs.append(4 + 4 * len(self.texnames) + len(mips))
            mips += struct.pack("<16sII4I", nm.encode(), 16, 16, 0, 0, 0, 0)
        tx += struct.pack("<%di" % len(offs), *offs) + mips
        lumps = [
            self.ents,
            b"".join(struct.pack("<4fi", *p) for p in self.planes),
            tx,
            b"".join(struct.pack("<3f", *v) for v in self.verts),
            self.vis,
            b"".join(struct.pack("<i2h6h2H", *n) for n in self.nodes),
            b"".join(struct.pack("<8fii", *t) for t in self.texinfo),
            b"".join(struct.pack("<HHiHH4Bi", *f) for f in self.faces),
            self.light,
            b"".join(struct.pack("<i2h", *c) for c in self.clip),
            b"".join(struct.pack("<ii6h2H4B", *lf) for lf in self.leafs),
            struct.pack("<%dH" % len(self.mark), *self.mark),
            b"".join(struct.pack("<2H", *e) for e in self.edges),
            struct.pack("<%di" % len(self.surfedges), *self.surfedges),
            b"".join(struct.pack("<9f7i", *m) for m in self.models),
        ]
        out = bytearray(struct.pack("<i", 30) + bytes(8 * 15))
        for i, b in enumerate(lumps):
            while len(out) % 4:
                out.append(0)
            struct.pack_into("<ii", out, 4 + 8 * i, len(out), len(b))
            out += b
        return bytes(out)


def square(x0, y0, x1, y1, z):
    return [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]


def floors_map(layers):
    """World of horizontal floors, top first: layers = [(z, texture name,
    (x0, y0, x1, y1), luxel value or None for no lightmap)]. Node i splits
    on z = layers[i].z and carries layer i's face; the front child is an
    empty leaf, the back child the next node (last: solid leaf 0)."""
    m = Map()
    n = len(layers)
    m.leafs.append((-2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))   # leaf 0 solid
    for i, (z, name, (x0, y0, x1, y1), lux) in enumerate(layers):
        pl = m.plane((0.0, 0.0, 1.0), float(z))
        ti = m.tex(name)
        ofs = -1
        if lux is not None:
            cols = (x1 - x0) // 16 + 1 + (1 if x0 % 16 else 0)
            rows = (y1 - y0) // 16 + 1 + (1 if y0 % 16 else 0)
            ofs = m.lightmap([lux] * (cols * rows))
        f = m.face(square(x0, y0, x1, y1, z), pl, ti, lightofs=ofs)
        m.leafs.append((-1, -1, 0, 0, 0, 0, 0, 0, len(m.mark), 1, 0, 0, 0, 0))
        m.mark.append(f)
        back = i + 1 if i + 1 < n else -1
        m.nodes.append((pl, -1 - (i + 1), back, 0, 0, 0, 0, 0, 0, f, 1))
    m.models.append((0, 0, 0, 256, 256, 256, 0, 0, 0, 0, 0, 0, 0, n, 0, n))
    return m


def write(tmp: Path, name: str, data: bytes) -> Path:
    p = tmp / name
    p.write_bytes(data)
    return p


def wci4(names, size=16) -> bytes:
    """A WCI4 v1 pack with one size x size page per name."""
    n = len(names)
    eo = 20
    data_ofs = eo + 44 * n
    ents = bytearray()
    blob = bytearray()
    for nm in names:
        ci4o = data_ofs + len(blob)
        blob += bytes(size * size // 2)
        tluto = data_ofs + len(blob)
        blob += bytes(32)
        ents += struct.pack(">16s4H2i3I", nm.encode(), size, size, size, size,
                            65536, 65536, ci4o, size * size // 2, tluto)
    total = data_ofs + len(blob)
    return struct.pack(">IHHIII", 0x57434934, 1, n, eo, data_ofs, total) + bytes(ents) + bytes(blob)


def silent_load(path: Path):
    return W.Bsp(path)


# --------------------------------------------------------------------------
# W2: luxel addressing
# --------------------------------------------------------------------------

class TestLuxelAddressing(unittest.TestCase):
    def test_extents_negative_fractional(self):
        mins, ext = W.face_extents([np.float32(-20.5), np.float32(30.25)],
                                   [np.float32(-0.5), np.float32(15.75)])
        self.assertEqual(mins, [-32, -16])
        self.assertEqual(ext, [64, 32])

    def test_vertex_luxel_truncates_and_clamps(self):
        mins, ext = [-32, -16], [64, 32]          # 5 x 3 luxels
        f = np.float32
        self.assertEqual(W.vertex_luxel(f(-20.5), f(-0.5), mins, ext), (0, 1))
        self.assertEqual(W.vertex_luxel(f(-0.5), f(15.75), mins, ext), (2, 1))
        self.assertEqual(W.vertex_luxel(f(30.25), f(-16.0), mins, ext), (3, 0))
        # outside the grid on every side: clamped
        self.assertEqual(W.vertex_luxel(f(500.0), f(500.0), mins, ext), (4, 2))
        self.assertEqual(W.vertex_luxel(f(-500.0), f(-500.0), mins, ext), (0, 0))

    def test_vertex_colour_formula(self):
        # one 16 x 16 face, luxel grid 2 x 2; vertex (0,0) -> luxel 0
        m = Map()
        m.leafs.append((-2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
        pl = m.plane((0.0, 0.0, 1.0), 0.0)
        ofs = m.lightmap([40, 41, 42, 43] + [200, 201, 202, 203])
        ti = m.tex("floor")
        m.face(square(0, 0, 16, 16, 0), pl, ti, styles=(0, 5, 255, 255), lightofs=ofs)
        ti2 = m.tex("!water")
        m.face(square(0, 0, 16, 16, 0), pl, ti2, lightofs=ofs)
        m.models.append((0, 0, 0, 16, 16, 16, 0, 0, 0, -1, 0, 0, 0, 0, 0, 2))
        with tempfile.TemporaryDirectory() as td:
            bsp = W.Bsp(write(Path(td), "c.bsp", m.build()))
            tex, _ = W.build_textures(bsp, W.TexPack(write(Path(td), "c.wci4",
                                                          wci4(["floor", "!water"]))))
            (p,) = W.cook_face(bsp, 0, tex)
            # vertex 0 at (0,0): raw = (40, 200); (240 * 264) >> 8 = 247 -> x2.5
            self.assertEqual(p.styles, (0, 5))
            self.assertEqual(p.raw[0], (40, 200))
            self.assertEqual(p.verts[0][4], 0xFFFFFFFF)
            # vertex 1 at (16,0): column 1 -> (41, 201)
            self.assertEqual(p.raw[1], (41, 201))
            # a lone style-0 luxel of 40: (40 * 264) >> 8 = 41 -> 41 * 2.5 + 0.5
            self.assertEqual(W.finish_light((40 * 264) >> 8), 103)
            (w,) = W.cook_face(bsp, 1, tex)          # water: fullbright, unstyled
            self.assertEqual(w.styles, ())
            self.assertTrue(all(v[4] == 0xFFFFFFFF for v in w.verts))


# --------------------------------------------------------------------------
# W3: probe walk
# --------------------------------------------------------------------------

class TestProbeWalk(unittest.TestCase):
    def probe(self, m, pts):
        with tempfile.TemporaryDirectory() as td:
            bsp = W.Bsp(write(Path(td), "p.bsp", m.build()))
            lf = W.LightFaces(bsp, W.style_values(W.parse_entities(bsp.entities)))
            return W.light_points(bsp, lf, np.array(pts)).tolist()

    def test_two_floors(self):
        m = floors_map([(128, "upper", (0, 0, 64, 64), 100),
                        (0, "lower", (0, 0, 128, 128), 50)])
        up, low = (100 * 264) >> 8, (50 * 264) >> 8
        self.assertEqual(self.probe(m, [(32, 32, 200),     # above the upper floor
                                        (100, 100, 200),   # beside it: lower floor
                                        (32, 32, 64),      # below it
                                        (32, 32, -64)]),   # below everything
                         [up, low, low, 0])

    def test_sky_above_water_skipped(self):
        m = floors_map([(128, "sky", (0, 0, 64, 64), 100),
                        (64, "!water", (0, 0, 64, 64), 90),
                        (0, "lower", (0, 0, 64, 64), 50)])
        self.assertEqual(self.probe(m, [(32, 32, 200)]), [(50 * 264) >> 8])

    def test_no_lightmap_gives_zero(self):
        m = floors_map([(128, "upper", (0, 0, 64, 64), None),
                        (0, "lower", (0, 0, 64, 64), 50)])
        m.light = bytes(3)                     # the map has a lighting lump
        self.assertEqual(self.probe(m, [(32, 32, 200)]), [0])

    def test_no_lighting_lump_gives_255(self):
        m = floors_map([(0, "lower", (0, 0, 64, 64), None)])
        self.assertEqual(self.probe(m, [(32, 32, 200)]), [255])

    def test_map_start_style_values(self):
        v = W.style_values([{"classname": "light", "targetname": "a", "style": "33",
                             "spawnflags": "1"},
                            {"classname": "light", "targetname": "b", "style": "34"},
                            {"classname": "light", "style": "35"},
                            {"classname": "light_fluoro", "style": "36", "spawnflags": "1"}])
        self.assertEqual((v[0], v[2], v[5], v[6], v[63], v[12]), (264, 0, 198, 286, 0, 256))
        self.assertEqual((v[33], v[34], v[35], v[36]), (0, 264, 256, 0))


# --------------------------------------------------------------------------
# W4 (cook half) and W5: texture animation tables
# --------------------------------------------------------------------------

def tex_list(names):
    return [{"name": n, "flags": 0, "total": 0, "min": 0, "max": 0, "next": 0xFFFF,
             "alt": 0xFFFF} for n in names]


class TestTextureAnimation(unittest.TestCase):
    def test_three_frames_and_alternate(self):
        tex = tex_list(["wall", "+1btn", "+abtn", "+0btn", "+2btn", "+0lone"])
        W.build_anim(tex)
        f0, f1, f2, alt = tex[3], tex[1], tex[4], tex[2]
        for j, (t, nxt) in enumerate(((f0, 1), (f1, 4), (f2, 3))):
            self.assertEqual((t["total"], t["min"], t["max"], t["next"], t["alt"]),
                             (6, 2 * j, 2 * j + 2, nxt, 2))
            self.assertTrue(t["flags"] & W.TEXF_ANIM)
        self.assertEqual((alt["total"], alt["min"], alt["max"], alt["next"], alt["alt"]),
                         (2, 0, 2, 2, 3))
        self.assertEqual((tex[5]["total"], tex[5]["next"], tex[5]["alt"]), (2, 5, 0xFFFF))
        self.assertEqual((tex[0]["flags"], tex[0]["next"]), (0, 0xFFFF))

    def test_letters_any_case(self):
        tex = tex_list(["+Abtn", "+bbtn"])
        W.build_anim(tex)
        self.assertEqual((tex[0]["total"], tex[0]["next"], tex[1]["next"]), (4, 1, 0))

    def test_missing_frame_names_texture(self):
        with self.assertRaises(W.InputError) as cm:
            W.build_anim(tex_list(["+0gap", "+2gap"]))
        self.assertIn("+2gap", str(cm.exception))
        self.assertIn("+1gap", str(cm.exception))

    def test_missing_frame_zero(self):
        with self.assertRaises(W.InputError) as cm:
            W.build_anim(tex_list(["+bgap"]))
        self.assertIn("+agap", str(cm.exception))

    def test_bad_letter_names_texture(self):
        with self.assertRaises(W.InputError) as cm:
            W.build_anim(tex_list(["+0ok", "+kbad"]))
        self.assertIn("+kbad", str(cm.exception))


class TestSubmodelKinds(unittest.TestCase):
    def test_translucent_walls_stay_movers(self):
        ents = [
            {"classname": "worldspawn"},
            {"classname": "func_wall", "model": "*1"},
            {"classname": "func_wall", "model": "*2", "rendermode": "2", "renderamt": "128"},
            {"classname": "func_illusionary", "model": "*3", "rendermode": "5"},
            {"classname": "func_wall", "model": "*4", "rendermode": "4", "renderfx": "16"},
            {"classname": "func_wall", "model": "*5", "rendermode": "0", "renderfx": "2",
             "renderamt": "100", "rendercolor": "255 0 0"},
            {"classname": "func_wall", "model": "*6", "targetname": "w"},
            {"classname": "trigger_once", "model": "*7"},
        ]
        kinds, cls = W.submodel_kinds(ents, 9)
        # Normal (*5) folds whatever its amount / colour / fx
        self.assertEqual(kinds, [W.SUB_NONE, W.SUB_FOLDED, W.SUB_MOVR, W.SUB_MOVR,
                                 W.SUB_MOVR, W.SUB_FOLDED, W.SUB_MOVR, W.SUB_NONE,
                                 W.SUB_NONE])
        self.assertEqual(cls[3], "func_illusionary")

    def test_one_translucent_user_makes_a_mover(self):
        ents = [{"classname": "func_wall", "model": "*1"},
                {"classname": "func_wall", "model": "*1", "rendermode": "4"},
                {"classname": "func_wall", "model": "*1"}]
        self.assertEqual(W.submodel_kinds(ents, 2)[0], [W.SUB_NONE, W.SUB_MOVR])


# --------------------------------------------------------------------------
# hostile input
# --------------------------------------------------------------------------

class TestHostileInput(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.tmp = Path(self.td.name)
        m = floors_map([(128, "upper", (0, 0, 64, 64), 100),
                        (0, "lower", (0, 0, 128, 128), 50)])
        m.vis = b"\xff"                        # one PVS row: both visleafs
        m.leafs = [m.leafs[0]] + [lf[:1] + (0,) + lf[2:] for lf in m.leafs[1:]]
        self.good = m.build()

    def tearDown(self):
        self.td.cleanup()

    def rejects(self, data: bytes, what: str):
        p = write(self.tmp, "h.bsp", data)
        with self.assertRaises(W.InputError) as cm:
            W.Bsp(p)
        self.assertIn(str(p), str(cm.exception))
        self.assertIn(what, str(cm.exception))

    def test_good_map_loads(self):
        bsp = W.Bsp(write(self.tmp, "g.bsp", self.good))
        self.assertEqual(len(bsp.faces), 2)

    def test_truncated(self):
        self.rejects(self.good[:60], "truncated")
        self.rejects(self.good[:len(self.good) - 9], "outside the file")

    def test_bad_version(self):
        self.rejects(struct.pack("<i", 29) + self.good[4:], "version 29")

    def test_lump_outside_file(self):
        b = bytearray(self.good)
        struct.pack_into("<ii", b, 4 + 8 * W.L_FACES, len(b) - 4, 40)
        self.rejects(bytes(b), "outside the file")
        struct.pack_into("<ii", b, 4 + 8 * W.L_FACES, -8, 20)
        self.rejects(bytes(b), "outside the file")

    def test_bad_element_size(self):
        b = bytearray(self.good)
        o, n = struct.unpack_from("<ii", b, 4 + 8 * W.L_FACES)
        struct.pack_into("<ii", b, 4 + 8 * W.L_FACES, o, n - 1)
        self.rejects(bytes(b), "not a multiple of 20")

    def patch_index(self, lump, fmt, rec, field, value, what):
        b = bytearray(self.good)
        o, _n = struct.unpack_from("<ii", b, 4 + 8 * lump)
        sz = struct.calcsize(fmt)
        t = list(struct.unpack_from(fmt, b, o + rec * sz))
        t[field] = value
        struct.pack_into(fmt, b, o + rec * sz, *t)
        self.rejects(bytes(b), what)

    def test_indices_out_of_range(self):
        self.patch_index(W.L_FACES, "<HHiHH4Bi", 0, 4, 99, "texinfo 99")
        self.patch_index(W.L_FACES, "<HHiHH4Bi", 0, 0, 99, "plane 99")
        self.patch_index(W.L_FACES, "<HHiHH4Bi", 1, 2, 1000, "surfedges")
        self.patch_index(W.L_FACES, "<HHiHH4Bi", 1, 9, 1 << 20, "lighting offset")
        self.patch_index(W.L_NODES, "<i2h6h2H", 0, 2, 0, "child node 0")
        self.patch_index(W.L_NODES, "<i2h6h2H", 0, 1, -50, "child leaf")
        self.patch_index(W.L_NODES, "<i2h6h2H", 1, 9, 7, "faces 7+1")
        self.patch_index(W.L_SURFEDGES, "<i", 0, 0, -100, "surfedge 0")
        self.patch_index(W.L_EDGES, "<2H", 1, 1, 999, "vertex 999")
        self.patch_index(W.L_TEXINFO, "<8fii", 0, 8, 7, "miptex 7")
        self.patch_index(W.L_MARKSURFACES, "<H", 0, 0, 77, "face 77")
        self.patch_index(W.L_LEAFS, "<ii6h2H4B", 1, 1, 5000, "vis offset")
        self.patch_index(W.L_LEAFS, "<ii6h2H4B", 1, 8, 9, "marksurfaces 9+1")
        self.patch_index(W.L_MODELS, "<9f7i", 0, 9, 50, "hull 0 head 50")
        self.patch_index(W.L_MODELS, "<9f7i", 0, 10, 3, "clip head 3")
        self.patch_index(W.L_MODELS, "<9f7i", 0, 13, 40, "visleafs 40")

    def test_miptex_offsets(self):
        b = bytearray(self.good)
        o, n = struct.unpack_from("<ii", b, 4 + 8 * W.L_TEXTURES)
        struct.pack_into("<i", b, o + 4, n)
        self.rejects(bytes(b), "miptex 0 offset")
        struct.pack_into("<i", b, o, 1 << 20)
        self.rejects(bytes(b), "texture count")

    def test_vis_row_overrun(self):
        m = floors_map([(0, "lower", (0, 0, 64, 64), 50)])
        m.vis = b"\x00"                        # zero byte without its count
        lf = list(m.leafs[1])
        lf[1] = 0
        m.leafs[1] = tuple(lf)
        bsp = W.Bsp(write(self.tmp, "v.bsp", m.build()))
        with self.assertRaises(W.InputError) as cm:
            W.build_visr(bsp)
        self.assertIn("runs past the vis lump", str(cm.exception))

    def test_empty_clip_hull_head(self):
        # heads one past the clipnode lump (no clipnodes): empty hulls
        bsp = W.Bsp(write(self.tmp, "e.bsp", self.good))
        self.assertEqual(bsp.models[0][10:13], (W.CONTENTS_EMPTY,) * 3)

    def test_wci4(self):
        good = wci4(["upper"])
        W.TexPack(write(self.tmp, "g.wci4", good))
        for data, what in ((good[:10], "truncated"), (b"XXXX" + good[4:], "not a WCI4"),
                           (good[:-1], "file size"),
                           (good[:8] + struct.pack(">I", 1 << 20) + good[12:], "entry table")):
            with self.assertRaises(W.InputError) as cm:
                W.TexPack(write(self.tmp, "b.wci4", data))
            self.assertIn(what, str(cm.exception))
        b = bytearray(good)
        struct.pack_into(">I", b, 20 + 16 + 16 + 4, 1 << 20)     # ci4 bytes
        with self.assertRaises(W.InputError):
            W.TexPack(write(self.tmp, "c.wci4", bytes(b)))

    def test_hle(self):
        hdr = b"HLE1" + struct.pack("<HHI", 1, 0, 96) + bytes(84)
        self.assertEqual(W.load_hle(write(self.tmp, "g.hle", hdr)), hdr)
        for data, what in ((hdr[:50], "truncated"), (b"HLX1" + hdr[4:], "not an HLE1"),
                           (hdr[:4] + struct.pack("<HHI", 2, 0, 96) + hdr[12:], "version 2"),
                           (hdr[:4] + struct.pack("<HHI", 1, 0, 500) + hdr[12:], "size 500"),
                           (hdr + b"junk" * 8, "not an HLN1")):
            with self.assertRaises(W.InputError) as cm:
                W.load_hle(write(self.tmp, "b.hle", data))
            self.assertIn(what, str(cm.exception))

    def test_cli_error_no_traceback(self):
        bsp = write(self.tmp, "t.bsp", self.good[:100])
        p = subprocess.run([sys.executable, "-m", "tools.w64c.w64c", str(bsp),
                            str(self.tmp / "x.wci4"), str(self.tmp / "x.w64")],
                           cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(p.returncode, 1)
        self.assertTrue(p.stderr.startswith(f"w64c: error: {bsp}: "), p.stderr)
        self.assertNotIn("Traceback", p.stderr)

    def test_cli_cooks_synthetic_map(self):
        bsp = write(self.tmp, "s.bsp", self.good)
        pack = write(self.tmp, "s.wci4", wci4(["upper", "lower"]))
        out = self.tmp / "s.w64"
        p = subprocess.run([sys.executable, "-m", "tools.w64c.w64c", str(bsp), str(pack),
                            str(out)], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        b = out.read_bytes()
        self.assertEqual(b[:6], b"W64L\x00\x03")
        self.assertEqual(struct.unpack_from(">I", b, 8)[0], len(b))


if __name__ == "__main__":
    unittest.main()
