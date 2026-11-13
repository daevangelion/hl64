# SPDX-License-Identifier: MIT
"""hlecook unit tests on synthetic BSP30 files only (no game data): a
well-formed one-room map cooks to an HLE1 chunk with its HLN1 node graph,
and malformed input (truncated file, lump past the end, bad version,
plane / child / leaf / head index out of range, unordered or cyclic trees,
broken entity text, hostile node origins) is rejected with
"hlecook: error: <file>: <what>" and exit 1, never a crash.

Builds hlecook into a temporary directory with the project flags, or uses
the binary named by HL64_HLECOOK (e.g. an -fsanitize=address,undefined
build). Run from the repository root:

  python3 -m unittest tools.w64c.test_hlecook
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CFLAGS = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off"]

ENTS = (b'{\n"classname" "worldspawn"\n}\n'
        b'{\n"classname" "info_node"\n"origin" "0 0 32"\n}\n'
        b'{\n"classname" "info_node"\n"origin" "96 0 32"\n}\n'
        b'{\n"classname" "info_player_start"\n"origin" "0 0 40"\n'
        b'"angle" "90"\n}\n\0')


def plane(n, d, t):
    return struct.pack("<4fi", *n, d, t)


def bsp(ents=ENTS, version=30, planes=None, nodes=None, clip=None, leafs=None,
        models=None, extra=None) -> bytes:
    """A floor at z = 0 inside the box ±256: hull 0 and each clip hull is
    one plane (empty above, solid below), the clip hulls' floors raised by
    their box's half height (hull 1: 36, hull 2: 32, hull 3: 18)."""
    if planes is None:
        planes = b"".join(plane((0, 0, 1), d, 2) for d in (0, 36, 32, 18))
    if nodes is None:   # plane, children (leaf 1 = empty, leaf 0 = solid)
        nodes = struct.pack("<ihh6hHH", 0, -2, -1, 0, 0, 0, 0, 0, 0, 0, 0)
    if clip is None:    # plane, children (contents) for hulls 1, 2, 3
        clip = b"".join(struct.pack("<ihh", k, -1, -2) for k in (1, 2, 3))
    if leafs is None:
        leafs = (struct.pack("<i", -2) + bytes(24)
                 + struct.pack("<i", -1) + bytes(24))
    if models is None:
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256,
                             0, 0, 0, 0, 0, 1, 2, 0, 0, 0)
    lumps = {0: ents, 1: planes, 5: nodes, 9: clip, 10: leafs, 14: models}
    if extra:
        lumps.update(extra)
    out = bytearray(struct.pack("<i", version) + bytes(8 * 15))
    for i in range(15):
        data = lumps.get(i, b"")
        while len(out) % 4:
            out.append(0)
        struct.pack_into("<ii", out, 4 + 8 * i, len(out), len(data))
        out += data
    return bytes(out)


class HleCook(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.mkdtemp(prefix="test_hlecook.")
        exe = os.environ.get("HL64_HLECOOK")
        if not exe:
            exe = os.path.join(cls.tmp, "hlecook")
            cc = os.environ.get("CC") or shutil.which("cc") or "gcc"
            subprocess.run([cc, *CFLAGS, "-o", exe,
                            os.path.join(HERE, "hlecook.c"), "-lm"],
                           check=True)
        cls.exe = exe

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_cook(self, data: bytes):
        src = os.path.join(self.tmp, "in.bsp")
        dst = os.path.join(self.tmp, "out.hle")
        with open(src, "wb") as f:
            f.write(data)
        if os.path.exists(dst):
            os.remove(dst)
        r = subprocess.run([self.exe, src, dst], capture_output=True,
                           text=True, timeout=60)
        out = None
        if os.path.exists(dst):
            with open(dst, "rb") as f:
                out = f.read()
        return r, src, out

    def rejects(self, data: bytes, what: str) -> None:
        r, src, _ = self.run_cook(data)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertTrue(r.stderr.startswith("hlecook: error: %s: " % src),
                        r.stderr)
        self.assertIn(what, r.stderr)
        self.assertNotIn("Sanitizer", r.stderr)
        self.assertNotIn("runtime error", r.stderr)

    # ---- well-formed input

    def test_cooks_entities_and_graph(self) -> None:
        r, _, out = self.run_cook(bsp())
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, "")
        self.assertEqual(out[:4], b"HLE1")
        size = struct.unpack_from("<I", out, 8)[0]
        at = (size + 15) & ~15
        self.assertEqual(out[at:at + 4], b"HLN1")
        magic, ver, flags, gsize, nn, nl, ofs_n, ofs_l = struct.unpack_from(
            "<IHHIHHII", out, at)
        self.assertEqual((ver, nn, nl), (1, 2, 2))
        for i in range(nn):
            org = struct.unpack_from("<3f", out, at + ofs_n + 16 * i)
            # land nodes on the floor, DIST_EPSILON above it
            self.assertAlmostEqual(org[2], 0.03125, places=5)
        hulls = {out[at + ofs_l + 6 * i + 2] for i in range(nl)}
        self.assertEqual(hulls, {15})   # small, human, large, fly

    def test_no_nodes_no_graph(self) -> None:
        ents = b'{\n"classname" "worldspawn"\n}\n'
        r, _, out = self.run_cook(bsp(ents=ents))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(out), struct.unpack_from("<I", out, 8)[0])

    def test_deterministic(self) -> None:
        a = self.run_cook(bsp())[2]
        b = self.run_cook(bsp())[2]
        self.assertEqual(a, b)

    # ---- file and lump structure

    def test_empty_file(self) -> None:
        self.rejects(b"", "truncated BSP header")

    def test_truncated_header(self) -> None:
        self.rejects(bsp()[:50], "truncated BSP header")

    def test_truncated_body(self) -> None:
        data = bsp()
        self.rejects(data[:len(data) - 30], "lump outside the file")

    def test_lump_past_eof(self) -> None:
        data = bytearray(bsp())
        struct.pack_into("<ii", data, 4 + 8 * 10, len(data) - 8, 56)
        self.rejects(bytes(data), "lump outside the file")

    def test_lump_offset_overflow(self) -> None:
        data = bytearray(bsp())
        struct.pack_into("<I", data, 4 + 8 * 1, 0xFFFFFFF0)
        self.rejects(bytes(data), "lump outside the file")

    def test_bad_version(self) -> None:
        self.rejects(bsp(version=29), "bad version")
        self.rejects(bsp(version=-1), "bad version")

    def test_partial_record(self) -> None:
        self.rejects(bsp(planes=plane((0, 0, 1), 0, 2) * 4 + b"\0\0\0"),
                     "whole number of records")

    def test_no_collision(self) -> None:
        self.rejects(bsp(models=b""), "no collision")

    def test_absurd_entity_lump(self) -> None:
        self.rejects(bsp(ents=b""), "bad entity lump size")

    # ---- indices

    def test_node_plane_out_of_range(self) -> None:
        nodes = struct.pack("<ihh6hHH", 7, -2, -1, *([0] * 8))
        self.rejects(bsp(nodes=nodes), "node plane out of range")

    def test_node_child_out_of_range(self) -> None:
        nodes = struct.pack("<ihh6hHH", 0, 5, -1, *([0] * 8))
        self.rejects(bsp(nodes=nodes), "node child out of range")

    def test_node_cycle(self) -> None:
        nodes = struct.pack("<ihh6hHH", 0, 0, -1, *([0] * 8))
        self.rejects(bsp(nodes=nodes), "hull tree has a cycle")

    def test_node_leaf_out_of_range(self) -> None:
        nodes = struct.pack("<ihh6hHH", 0, -40, -1, *([0] * 8))
        self.rejects(bsp(nodes=nodes), "node leaf out of range")

    def test_clipnode_plane_out_of_range(self) -> None:
        self.rejects(bsp(clip=struct.pack("<ihh", -3, -1, -2) * 3),
                     "clipnode plane out of range")

    def test_clipnode_child_out_of_range(self) -> None:
        self.rejects(bsp(clip=struct.pack("<ihh", 0, 9, -2) * 3),
                     "clipnode child out of range")

    def test_clipnode_shared_subtree(self) -> None:
        # compilers share clipnode subtrees: a child before its parent is
        # fine as long as there is no cycle
        clip = (struct.pack("<ihh", 1, -1, -2) + struct.pack("<ihh", 2, -1, -2)
                + struct.pack("<ihh", 3, -1, -2) + struct.pack("<ihh", 1, 0, 0))
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256,
                             0, 0, 0, 0, 0, 3, 2, 0, 0, 0)
        r, _, out = self.run_cook(bsp(clip=clip, models=models))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_clipnode_cycle(self) -> None:
        clip = (struct.pack("<ihh", 1, 1, -2) + struct.pack("<ihh", 2, -1, 2)
                + struct.pack("<ihh", 3, 0, -2))
        self.rejects(bsp(clip=clip), "hull tree has a cycle")

    def test_model_head_out_of_range(self) -> None:
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256,
                             0, 0, 0, 0, 0, 4, 2, 0, 0, 0)
        self.rejects(bsp(models=models), "model head node out of range")
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256,
                             0, 0, 0, 1, 0, 1, 2, 0, 0, 0)
        self.rejects(bsp(models=models), "model head node out of range")

    def test_model_head_one_past_clipnodes(self) -> None:
        # compilers write numclipnodes as the clip heads of models without
        # clip hulls (retail c2a2a, c3a2c): an empty hull
        models = struct.pack("<9f4i3i", -256, -256, -256, 256, 256, 256,
                             0, 0, 0, 0, 0, 1, 2, 0, 0, 0) * 2
        models = models[:64] + struct.pack(
            "<9f4i3i", -8, -8, -8, 8, 8, 8, 0, 0, 0, 0, 3, 3, 3, 0, 0, 0)
        r, _, _ = self.run_cook(bsp(models=models))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_deep_tree(self) -> None:
        # a 1100-node chain, children always after their parent
        n = 1100
        nodes = b"".join(struct.pack("<ihh6hHH", 0, i + 1 if i + 1 < n else -2,
                                     -1, *([0] * 8)) for i in range(n))
        self.rejects(bsp(nodes=nodes), "hull tree too deep")

    # ---- entity text

    def test_unterminated_block(self) -> None:
        self.rejects(bsp(ents=b'{\n"classname" "worldspawn"\n'),
                     "unterminated entity block")

    def test_unterminated_value(self) -> None:
        self.rejects(bsp(ents=b'{\n"classname" "worldspawn\n}\n'),
                     "unterminated entity block")

    def test_nested_block(self) -> None:
        self.rejects(bsp(ents=b'{\n"classname" "worldspawn"\n{\n}\n'),
                     "inside an entity block")

    def test_text_outside_blocks(self) -> None:
        self.rejects(bsp(ents=b'{\n"classname" "worldspawn"\n}\nxyz\n'),
                     "expected '{'")

    def test_hostile_node_origin(self) -> None:
        for org in (b"1e30 0 0", b"nan 0 0", b"0 inf 0"):
            ents = (b'{\n"classname" "worldspawn"\n}\n'
                    b'{\n"classname" "info_node"\n"origin" "' + org
                    + b'"\n}\n')
            self.rejects(bsp(ents=ents), "node origin out of range")

    def test_garbage_does_not_crash(self) -> None:
        good = bsp()
        for i in range(0, len(good), 7):
            data = bytearray(good)
            data[i] ^= 0xA5
            r, _, _ = self.run_cook(bytes(data))
            self.assertIn(r.returncode, (0, 1), r.stderr)
            if r.returncode:
                self.assertTrue(r.stderr.startswith("hlecook: error: "),
                                r.stderr)
            self.assertNotIn("Sanitizer", r.stderr)
            self.assertNotIn("runtime error", r.stderr)


if __name__ == "__main__":
    unittest.main()
