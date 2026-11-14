# SPDX-License-Identifier: MIT
"""w64check tests: the segment Tracer against hand-worked cases and random trees,
a hand-built minimal package + BSP the checker accepts, and negative tests that
mutate them (truncated files, tables and chunks outside the file, bad magic or
version, indices out of range, absurd counts, cyclic trees). Every malformed
input must give `w64check: error: <file>: <what>` on stderr, exit 1, no
traceback. Run from the repository root: python3 -m unittest tools.w64c.test_w64check
"""
import contextlib
import io
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import numpy as np

from tools.w64c import w64check as wc


def crc(b):
    return zlib.crc32(bytes(b)) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# a world with one splitting plane x = 0: x >= 0 empty, x < 0 solid
# ---------------------------------------------------------------------------

def mini_bsp(over=None):
    """BSP30 with node 0 (plane x = 0; front leaf 1 EMPTY, back leaf 0 SOLID),
    clipnode 0 (same, hull contents) and one model. `over` maps lump numbers to
    replacement lump bytes."""
    plane = struct.pack("<3ffi", 1., 0., 0., 0., 0)
    node = struct.pack("<i2h6h2H", 0, -2, -1, -64, -64, -64, 64, 64, 64, 0, 0)
    clip = struct.pack("<i2h", 0, -1, -2)
    leaves = (struct.pack("<ii6h2H4B", -2, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) +
              struct.pack("<ii6h2H4B", -1, -1, -64, -64, -64, 64, 64, 64, 0, 0, 0, 0, 0, 0))
    model = struct.pack("<9f7i", -64, -64, -64, 64, 64, 64, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0)
    lumps = {1: plane, 5: node, 9: clip, 10: leaves, 14: model}
    lumps.update(over or {})
    body = bytearray()
    table = []
    for i in range(15):
        data = lumps.get(i, b"")
        table.append((124 + len(body), len(data)))
        body += data
    return struct.pack("<i", 30) + b"".join(struct.pack("<ii", *t) for t in table) + bytes(body)


def mini_w64(**mut):
    """A package the checker accepts: one COLL chunk, nothing else. `mut` maps
    names of the knobs below to replacement values."""
    k = dict(version=3, flags=1, ncoll=1, pinned=1, coll_first=0, nn=2, nx=2, nex=0,
             c0=(-2, -1), c1=(-1, -2), dims=(1, 1, 1), cell=256, nstart=1, start=0,
             visr=0xFFFF, ents=0xFFFF, stride=36, extra_dir=0)
    k.update(mut)
    # directory: 96 B fixed part, then tables
    cd, sect, pp, tex, sub, leaf, vp, model, leafc, grid = 96, 112, 112, 112, 112, 128, 136, 144, 208, 216
    cells, start, lists = 248, 252, 256
    dsz = 260 + k["extra_dir"]
    d = bytearray(dsz)
    struct.pack_into(">8H", d, 0, 0, 2, 0, 0, 1, 1, 0, 0)
    struct.pack_into(">7I", d, 16, cd, sect, pp, tex, sub, leaf, vp)
    struct.pack_into(">6H3I", d, 44, k["coll_first"], k["ncoll"], k["pinned"], 2, k["ents"], k["visr"],
                     model, leafc, grid)
    struct.pack_into(">HH", d, sub, 0, 0)
    struct.pack_into(">9f", d, model, -64, -64, -64, 64, 64, 64, 0, 0, 0)
    struct.pack_into(">4i", d, model + 36, 0, 1, 1, 1)
    struct.pack_into(">iHH", d, model + 52, 1, 0, 1)
    struct.pack_into(">2b", d, leafc, -2, -1)
    struct.pack_into(">3h3HHHIII", d, grid, -64, -64, -64, *k["dims"], k["cell"], k["nstart"], cells, start, lists)
    struct.pack_into(">HH", d, cells, 0, 0)
    struct.pack_into(">i", d, start, k["start"])
    # COLL chunk: header, no planes, two axial X nodes (dist 0)
    coll = bytearray(32 + 4 * k["nex"])
    struct.pack_into(">6HI", coll, 0, k["nn"], k["nx"], k["nx"], k["nx"], 0, k["nex"], 0)
    struct.pack_into(">I2h", coll, 16, 0, *k["c0"])
    struct.pack_into(">I2h", coll, 24, 0, *k["c1"])
    chunk_ofs = 336
    struct.pack_into(">4sIII", d, cd, b"COLL", chunk_ofs, len(coll), crc(coll))
    out = bytearray(chunk_ofs) + coll
    out += bytes(-len(out) % 16)
    struct.pack_into(">4sHHIIIII", out, 0, b"W64L", k["version"], k["flags"], len(out), 64, dsz, 1, crc(d))
    struct.pack_into(">HH", out, 28, k["stride"], 0x408)
    out[64:64 + dsz] = d
    return out


def refresh(b):
    """Recompute directory and chunk CRCs and the file size after a mutation."""
    b = bytearray(b)
    dofs, dsz, nch = struct.unpack_from(">III", b, 12)
    cd = struct.unpack_from(">I", b, dofs + 16)[0]
    if dofs + cd + 16 * nch <= len(b):
        for i in range(nch):
            fcc, o, sz, _ = struct.unpack_from(">4sIII", b, dofs + cd + 16 * i)
            if o + sz <= len(b):
                struct.pack_into(">I", b, dofs + cd + 16 * i + 12, crc(b[o:o + sz]))
    struct.pack_into(">I", b, 24, crc(b[dofs:dofs + dsz]))
    return b


def run_check(w64, bsp, *extra):
    with tempfile.TemporaryDirectory() as t:
        a, b = Path(t) / "m.w64", Path(t) / "m.bsp"
        a.write_bytes(bytes(w64))
        b.write_bytes(bytes(bsp))
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = wc.main([str(a), str(b), "--points", "400", "--segments", "60", *extra])
        return rc, out.getvalue(), err.getvalue()


class Minimal(unittest.TestCase):
    def test_minimal_package_passes(self):
        rc, out, err = run_check(mini_w64(), mini_bsp())
        self.assertEqual((rc, err), (0, ""), out)
        self.assertIn("OK", out)

    def rejected(self, w64=None, bsp=None, where="m.w64"):
        rc, out, err = run_check(w64 if w64 is not None else mini_w64(), bsp if bsp is not None else mini_bsp())
        self.assertEqual(rc, 1, (out, err))
        self.assertEqual(out, "")
        self.assertTrue(err.startswith("w64check: error: "), err)
        self.assertIn(where, err)
        self.assertNotIn("Traceback", err)
        return err

    def test_missing_files(self):
        with tempfile.TemporaryDirectory() as t:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = wc.main([str(Path(t) / "nope.w64"), str(Path(t) / "nope.bsp")])
            self.assertEqual(rc, 1)
            self.assertIn("w64check: error: ", err.getvalue())
            self.assertIn("nope.w64", err.getvalue())

    def test_w64_truncated_everywhere(self):
        good = bytes(mini_w64())
        for n in (0, 1, 63, 64, 100, 300, 335, 336, 350, len(good) - 1):
            self.rejected(w64=good[:n])

    def test_w64_header(self):
        good = mini_w64()
        for off, fmt, val, msg in ((0, ">4s", b"W64X", "bad magic"), (4, ">H", 2, "version"),
                                   (6, ">H", 0, "W64_HF_COLL"), (8, ">I", 99999, "file_size"),
                                   (12, ">I", 10**9, "directory"), (12, ">I", 8, "directory"),
                                   (16, ">I", 50, "directory"), (16, ">I", 10**9, "directory"),
                                   (20, ">I", 5, "chunk count"), (24, ">I", 1, "directory CRC"),
                                   (28, ">H", 0, "idx_stride")):
            b = bytearray(good)
            struct.pack_into(fmt, b, off, val)
            self.assertIn(msg, self.rejected(w64=b), (off, val))

    def test_w64_directory_tables(self):
        good = mini_w64()
        base = 64
        cases = ((base + 16, ">I", 10**6, "chunk directory"),        # chunkdir_ofs
                 (base + 20, ">I", 10**6, "sectors"), (base + 28, ">I", 10**6, "textures"),
                 (base + 32, ">I", 10**6, "submodels"), (base + 36, ">I", 10**6, "leaf vis"),
                 (base + 56, ">I", 10**6, "models"), (base + 60, ">I", 10**6, "leaf contents"),
                 (base + 64, ">I", 10**6, "grid header"),
                 (base + 46, ">H", 5000, "absurd COLL"), (base + 44, ">H", 1, "COLL chunk range"),
                 (base + 48, ">H", 2, "COLL chunk range"), (base + 52, ">H", 7, "ENTS chunk index"),
                 (base + 54, ">H", 7, "VISR chunk index"), (base + 0, ">H", 100, "sectors"),
                 (base + 4, ">H", 100, "textures"))
        for off, fmt, val, msg in cases:
            b = bytearray(good)
            struct.pack_into(fmt, b, off, val)
            self.assertIn(msg, self.rejected(w64=refresh(b)), (off, val))

    def test_w64_chunk_directory(self):
        good = mini_w64()
        cd = 64 + 96
        for off, fmt, val, msg in ((cd + 4, ">I", 10**6, "outside the file"), (cd + 4, ">I", 337, "outside the file"),
                                   (cd + 4, ">I", 0, "outside the file"), (cd + 8, ">I", 10**6, "outside the file"),
                                   (cd + 12, ">I", 7, "CRC")):
            b = bytearray(good)
            struct.pack_into(fmt, b, off, val)
            b = refresh(b) if off != cd + 12 else b
            if off == cd + 12:      # only the directory CRC follows the mutation
                struct.pack_into(">I", b, 24, crc(b[64:64 + 260]))
            self.assertIn(msg, self.rejected(w64=b), (off, val))
        b = bytearray(good)
        b[336] ^= 0xFF           # chunk payload no longer matches its CRC
        self.assertIn("CRC", self.rejected(w64=b))

    def test_w64_coll_chunk(self):
        for mut, msg in ((dict(nn=5000, nx=5000), "class bounds"), (dict(nx=3), "class bounds"),
                         (dict(c0=(7, -1)), "extern index"),
                         (dict(c0=(0, -1)), "own descendant")):
            self.assertIn(msg, self.rejected(w64=refresh(mini_w64(**mut))), mut)
        b = mini_w64()
        struct.pack_into(">H", b, 336 + 10, 1000)           # extern count larger than the chunk
        self.assertIn("tables outside", self.rejected(w64=refresh(b)))

    def test_w64_grid(self):
        for mut, msg in ((dict(dims=(0, 1, 1)), "empty collision grid"), (dict(cell=0), "empty collision grid"),
                         (dict(nstart=0), "empty collision grid"), (dict(dims=(60000, 60000, 60000)), "absurd"),
                         (dict(dims=(100, 1, 1)), "outside the directory"), (dict(nstart=9), "outside the directory")):
            self.assertIn(msg, self.rejected(w64=refresh(mini_w64(**mut))), mut)
        b = refresh(mini_w64(start=77))     # grid start gid that is not a node
        self.assertIn("not a defined node", self.rejected(w64=b))

    def test_mismatch_is_a_fail_not_an_error(self):
        b = bytearray(mini_w64())
        b[64 + 208] = 0xFD       # leaf 0 contents differs from the BSP
        rc, out, err = run_check(refresh(b), mini_bsp())
        self.assertEqual((rc, err), (1, ""))
        self.assertIn("FAIL: leaf contents", out)

    def test_bsp_malformed(self):
        for data, msg in ((b"", "shorter"), (b"\x1e\0\0\0" + bytes(50), "shorter"),
                          (struct.pack("<i", 29) + bytes(120), "not BSP30"),
                          (mini_bsp()[:-1], "outside")):
            self.assertIn(msg, self.rejected(bsp=data, where="m.bsp"))
        # lump 1 (planes) past the end of the file / not a whole number of records
        b = bytearray(mini_bsp())
        struct.pack_into("<ii", b, 4 + 8, 100000, 20)
        self.assertIn("outside", self.rejected(bsp=b, where="m.bsp"))
        b = bytearray(mini_bsp())
        struct.pack_into("<ii", b, 4 + 8, 124, 19)
        self.assertIn("multiple", self.rejected(bsp=b, where="m.bsp"))

    def test_bsp_indices_and_cycles(self):
        node = lambda p, c0, c1: struct.pack("<i2h6h2H", p, c0, c1, 0, 0, 0, 0, 0, 0, 0, 0)
        clip = lambda p, c0, c1: struct.pack("<i2h", p, c0, c1)
        cases = ((dict(**{"5": node(3, -2, -1)}), "plane index"),
                 (dict(**{"5": node(0, 9, -1)}), "child index"),
                 (dict(**{"5": node(0, -2, -77)}), "leaf outside"),
                 (dict(**{"5": node(0, 0, -1)}), "own descendant"),
                 (dict(**{"9": clip(0, 0, -1)}), "own descendant"),
                 (dict(**{"9": clip(4, -1, -2)}), "plane index"),
                 (dict(**{"1": struct.pack("<3ffi", 1., 0., 0., 0., 9)}), "plane type"),
                 (dict(**{"14": struct.pack("<9f7i", float("nan"), 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0)}), "not finite"),
                 (dict(**{"14": struct.pack("<9f7i", -64, -64, -64, 64, 64, 64, 0, 0, 0, 5, 0, 0, 0, 1, 0, 0)}), "head out of range"),
                 (dict(**{"14": struct.pack("<9f7i", -64, -64, -64, 64, 64, 64, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0)}), "visleafs"),
                 (dict(**{"4": b"\x01", "10": struct.pack("<ii6h2H4B", -2, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) * 2}), "vis offset"))
        for mut, msg in cases:
            bsp = mini_bsp({int(k): v for k, v in mut.items()})
            self.assertIn(msg, self.rejected(bsp=bsp, where="m.bsp"), mut)

    def test_hle_blob_must_match(self):
        with tempfile.TemporaryDirectory() as t:
            a, b, h = Path(t) / "m.w64", Path(t) / "m.bsp", Path(t) / "m.hle"
            a.write_bytes(bytes(mini_w64()))
            b.write_bytes(mini_bsp())
            h.write_bytes(b"HLE1")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = wc.main([str(a), str(b), "--hle", str(h), "--points", "100", "--segments", "10"])
            self.assertEqual(rc, 1)
            self.assertIn("ENTS missing", out.getvalue())
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = wc.main([str(a), str(b), "--hle", str(Path(t) / "gone.hle")])
            self.assertEqual(rc, 1)
            self.assertIn("gone.hle", err.getvalue())

    def test_vis_rows(self):
        self.assertEqual(wc.decompress(bytes([0xFF, 0, 2, 0x01]), 0, 4, "p"), bytes([0xFF, 0, 0, 1]))
        self.assertEqual(wc.decompress(bytes([0, 5]), 0, 3, "p"), bytes(3))
        self.assertEqual(wc.decompress(b"", 0, 0, "p"), b"")
        for data, nb in ((b"", 1), (bytes([1]), 2), (bytes([0]), 1), (bytes([1, 0]), 3)):
            with self.assertRaisesRegex(wc.BadInput, "p: a vis row runs past its data"):
                wc.decompress(data, 0, nb, "p")
        with self.assertRaises(wc.BadInput):
            wc.decompress(bytes([1, 2, 3, 4]), 0, 4, "p", end=2)


# ---------------------------------------------------------------------------
# Tracer
# ---------------------------------------------------------------------------

def tree_fn(nodes):
    """nodes: [(normal, dist, kind, child0, child1)]"""
    return lambda n: nodes[n]


PLANE_X = [((1., 0., 0.), 0., 0, -1, -2)]       # x >= 0 EMPTY, x < 0 SOLID (hull contents)


class TracerTests(unittest.TestCase):
    def trace(self, nodes, p1, p2, hull=1, leafc=None, head=0):
        t = wc.Tracer(hull, leafc, tree_fn(nodes))
        return t.trace(head, p1, p2), t

    def test_impact_stands_off_by_dist_epsilon(self):
        r, t = self.trace(PLANE_X, (10, 0, 0), (-10, 0, 0))
        frac = (10 - wc.DIST_EPSILON) / 20
        self.assertEqual(r["fraction"], frac)
        self.assertAlmostEqual(r["endpos"][0], wc.DIST_EPSILON, places=12)
        self.assertEqual(r["plane"], ((1., 0., 0.), 0.))
        self.assertEqual((r["allsolid"], r["startsolid"], r["inopen"], r["inwater"]),
                         (False, False, True, False))
        self.assertEqual(t.visited, [0, 0])             # the split walk, then the far-side contents walk

    def test_impact_from_behind_a_plane_flips_the_normal(self):
        # x < 0 EMPTY, x >= 0 SOLID: travelling +x from x=-10 stops 1/32 short on the back side
        nodes = [((1., 0., 0.), 0., 0, -2, -1)]
        r, _ = self.trace(nodes, (-10, 0, 0), (10, 0, 0))
        self.assertEqual(r["fraction"], (10 - wc.DIST_EPSILON) / 20)
        self.assertAlmostEqual(r["endpos"][0], -wc.DIST_EPSILON, places=12)
        self.assertEqual(r["plane"], ((-1., -0., -0.), -0.))

    def test_no_impact_keeps_the_end_point(self):
        r, _ = self.trace(PLANE_X, (10, 3, 4), (1, 3, 4))
        self.assertEqual((r["fraction"], r["endpos"], r["plane"]), (1.0, (1, 3, 4), None))
        self.assertTrue(r["inopen"] and not r["allsolid"] and not r["startsolid"])
        r, _ = self.trace(PLANE_X, (10, 0, 0), (0, 0, 0))      # ending exactly on the plane is front
        self.assertEqual(r["fraction"], 1.0)

    def test_start_solid_that_leaves_the_solid(self):
        r, _ = self.trace(PLANE_X, (-10, 0, 0), (10, 0, 0))
        self.assertTrue(r["startsolid"] and not r["allsolid"] and r["inopen"])
        self.assertEqual((r["fraction"], r["plane"]), (1.0, None))

    def test_all_solid_reports_fraction_one_and_the_end_point(self):
        r, _ = self.trace(PLANE_X, (-10, 0, 0), (-20, 5, 0))
        self.assertEqual((r["allsolid"], r["startsolid"], r["fraction"], r["endpos"], r["plane"]),
                         (True, True, 1.0, (-20, 5, 0), None))
        # solid at the start, far side solid at the split: still never left the solid
        nodes = [((1., 0., 0.), 0., 0, -2, -2)]
        r, _ = self.trace(nodes, (-10, 0, 0), (10, 0, 0))
        self.assertTrue(r["allsolid"] and r["fraction"] == 1.0)

    def test_start_solid_then_later_impact_keeps_start_solid(self):
        # x<0 solid, 0<=x<4 empty, x>=4 solid: from the solid side through the gap into the far wall
        nodes = [((1., 0., 0.), 0., 0, 1, -2), ((1., 0., 0.), 4., 0, -2, -1)]
        r, _ = self.trace(nodes, (-5, 0, 0), (9, 0, 0))
        self.assertTrue(r["startsolid"] and not r["allsolid"])
        self.assertEqual(r["plane"], ((-1., -0., -0.), -4.))
        self.assertAlmostEqual(r["endpos"][0], 4 - wc.DIST_EPSILON, places=9)

    def test_water_and_hull0_leaf_table(self):
        # hull 0: children are leaf indices, contents through the table
        nodes = [((1., 0., 0.), 0., 0, -2, -3)]            # front leaf 1, back leaf 2
        leafc = np.array([-2, -1, -3])
        r, _ = self.trace(nodes, (5, 0, 0), (-5, 0, 0), hull=0, leafc=leafc)
        self.assertEqual((r["fraction"], r["inwater"], r["inopen"]), (1.0, True, True))
        leafc = np.array([-2, -1, -2])
        r, _ = self.trace(nodes, (5, 0, 0), (-5, 0, 0), hull=0, leafc=leafc)
        self.assertLess(r["fraction"], 1.0)
        with self.assertRaises(wc.Fail):
            self.trace(nodes, (5, 0, 0), (-5, 0, 0), hull=0, leafc=np.array([-2]))

    def test_plane_node_uses_the_dot_product(self):
        n = (0.6, 0.8, 0.0)
        nodes = [(n, 10., 3, -1, -2)]
        r, _ = self.trace(nodes, (20, 0, 0), (0, 0, 0))     # distance 2 -> -10
        self.assertAlmostEqual(r["fraction"], (2 - wc.DIST_EPSILON) / 12, places=12)
        self.assertEqual(r["plane"], (n, 10.))

    def test_end_points_never_classify_solid_on_random_trees(self):
        rng = np.random.default_rng(5)
        checked = impacts = 0
        for _ in range(60):
            nodes = []

            def grow(depth):
                if depth == 0 or rng.random() < 0.15:
                    return -int(rng.choice([1, 2, 2, 3]))
                v = rng.normal(size=3)
                v /= np.linalg.norm(v)
                axial = rng.random() < 0.5
                nrm = (1., 0., 0.) if axial else tuple(float(x) for x in v)
                idx = len(nodes)
                nodes.append(None)
                a, b = grow(depth - 1), grow(depth - 1)
                nodes[idx] = (nrm, float(rng.uniform(-30, 30)), 0 if axial else 3, a, b)
                return idx
            head = grow(6)
            if head < 0:
                continue
            for _ in range(60):
                p1 = tuple(float(x) for x in rng.uniform(-40, 40, 3))
                p2 = tuple(float(x) for x in rng.uniform(-40, 40, 3))
                t = wc.Tracer(1, None, tree_fn(nodes))
                r = t.trace(head, p1, p2)
                self.assertTrue(0.0 <= r["fraction"] <= 1.0)
                checked += 1
                if r["fraction"] < 1.0:
                    impacts += 1
                    if not r["startsolid"]:
                        probe = wc.Tracer(1, None, tree_fn(nodes))
                        self.assertNotEqual(probe.point_contents(head, r["endpos"]), wc.CONTENTS_SOLID)
        self.assertGreater(impacts, 200)


if __name__ == "__main__":
    unittest.main()
