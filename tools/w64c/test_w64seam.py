# SPDX-License-Identifier: MIT
"""Focused CWG3 and ray-lattice regressions. Run from the repository root:
python3 -m unittest tools.w64c.test_w64seam"""
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import numpy as np

from tools.cook.hl_campaign import (NONE, MAP_BYTES, SEAM_BYTES, bsp_bounds, float_ulp,
                                    pack_maps, portal_translation, read_cwg,
                                    validate_placements, write_cwg)
from tools.cook.hl_classes import map_entities
from tools.cook.hl_campaign import main as campaign_main
from tools.cook.test_hl_campaign import cli, make_bsp, two_maps
from tools.w64c import w64seam
from tools.w64c.w64seam import (EntityError, Scene, edict_classes, entity_box, eye_grid, eye_components,
                                graph_for_write, model_index, spawn_rotation, twins)


class SeamTests(unittest.TestCase):
    def graph(self):
        seam = {"from": "a", "to": "b", "landmark": "portal", "trigger_lump": 7,
                "changetarget": "open", "kind": 1, "flags": 7, "delta": [1., 2., 3.],
                "portal": [257., 2., 3.],
                "switch_mins": [4., 5., 6.], "switch_maxs": [7., 8., 9.],
                "carry_mins": [10., 11., 12.], "carry_maxs": [13., 14., 15.],
                "approach_mins": [16., 17., 18.], "approach_maxs": [19., 20., 21.],
                "need": [123, 456, 789], "twins": [[9, 10]], "sectors": [1, 0x8004],
                "b_dir_crc": 0x12345678, "draw": 0}
        return {"order": ["a", "b"], "components": [["a", "b"]],
                "comp_of": {"a": 0, "b": 0}, "offset": {}, "start": "a", "train": None,
                "gofs": {"a": [-128, 0, 0], "b": [128, 0, 0]},
                "bounds": {"a": [-16, -16, -16, 16, 16, 16],
                           "b": [-16, -16, -16, 16, 16, 16]},
                "info": {"a": {"landmarks": {"portal": [0, 0, 0]},
                               "links": [{"to": "b", "landmark": "portal", "spawnflags": 2, "seam": 0}]},
                         "b": {"landmarks": {"portal": [1, 2, 3]}, "links": []}},
                "seams": [seam]}

    def test_exact_offsets_and_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            graph = self.graph()
            write_cwg(graph, path)
            b = path.read_bytes()
            self.assertEqual(b[:8], b"CWG3\0\3\0\0")
            self.assertEqual(struct.unpack_from(">3H", b, 20), (1, 1, 2))
            self.assertEqual(struct.unpack_from(">3f", b, 32 + 12), (-128, 0, 0))
            self.assertEqual(struct.unpack_from(">6f", b, 32 + 24), (-16, -16, -16, 16, 16, 16))
            seam_ofs = 32 + 2 * MAP_BYTES + 2 * 16 + 8
            self.assertEqual(struct.unpack_from(">3f", b, seam_ofs + 24), (1, 2, 3))
            self.assertEqual(struct.unpack_from(">3f", b, seam_ofs + 84), (16, 17, 18))
            self.assertEqual(struct.unpack_from(">3I", b, seam_ofs + 108), (123, 456, 789))
            self.assertEqual(struct.unpack_from(">I", b, seam_ofs + 120)[0], 0x12345678)
            self.assertEqual(struct.unpack_from(">3f", b, seam_ofs + 124), (257, 2, 3))
            self.assertEqual(struct.unpack_from(">2H", b, seam_ofs + SEAM_BYTES), (9, 10))
            self.assertEqual(struct.unpack_from(">2H", b, seam_ofs + SEAM_BYTES + 4), (1, 0x8004))
            back = read_cwg(path)
            self.assertEqual(back["seams"], graph["seams"])
            self.assertEqual(back["maps"][0]["links"][0]["seam"], 0)
            rewritten = graph_for_write(back)
            rewritten["seams"] = back["seams"]
            write_cwg(rewritten, path)
            self.assertEqual(path.read_bytes(), b)

    def test_uncooked_links_have_no_seam(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            graph = self.graph()
            graph.pop("seams")
            graph["info"]["a"]["links"][0].pop("seam")
            write_cwg(graph, path)
            back = read_cwg(path)
            self.assertEqual(back["seams"], [])
            self.assertEqual(back["maps"][0]["links"][0]["seam"], NONE)

    def test_dangling_seam_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            write_cwg(self.graph(), path)
            b = bytearray(path.read_bytes())
            struct.pack_into(">H", b, 32 + 2 * MAP_BYTES + 32 + 6, 1)
            path.write_bytes(b)
            with self.assertRaisesRegex(ValueError, "link.*index"):
                read_cwg(path)

    def test_legacy_graph_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            write_cwg(self.graph(), path)
            b = bytearray(path.read_bytes())
            struct.pack_into(">IH", b, 0, 0x43574732, 2)
            path.write_bytes(b)
            with self.assertRaisesRegex(ValueError, "invalid CWG3"):
                read_cwg(path)

    def test_corrupt_portal_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            write_cwg(self.graph(), path)
            b = bytearray(path.read_bytes())
            ofs = 32 + 2 * MAP_BYTES + 32 + 8 + 124
            for value in (258., float("nan"), float("inf")):
                struct.pack_into(">f", b, ofs, value)
                path.write_bytes(b)
                with self.assertRaisesRegex(ValueError, "portal translation"):
                    read_cwg(path)

    def test_portal_uses_serialized_delta_before_rounding(self):
        graph = self.graph()
        seam = graph["seams"][0]
        seam["delta"] = [-255.99999, 2., 3.]
        seam["portal"] = portal_translation(graph["gofs"]["a"], graph["gofs"]["b"], seam["delta"])
        self.assertEqual(seam["portal"][0], 2 ** -16)
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            write_cwg(graph, path)
            self.assertEqual(read_cwg(path)["seams"][0]["portal"], seam["portal"])

    def test_invalid_placement_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.cwg"
            write_cwg(self.graph(), path)
            original = path.read_bytes()
            for field, value in ((12, -127.), (12, float("nan")), (36, -16.)):
                b = bytearray(original)
                struct.pack_into(">f", b, 32 + field, value)
                path.write_bytes(b)
                with self.assertRaisesRegex(ValueError, "invalid placement"):
                    read_cwg(path)
            b = bytearray(original)
            struct.pack_into(">f", b, 32 + MAP_BYTES + 12, -128.)
            path.write_bytes(b)
            with self.assertRaisesRegex(ValueError, "padding"):
                read_cwg(path)

    def test_packing_is_disjoint_centered_and_exact(self):
        bounds = {str(i): [-1024.5, -2032., -256., 3071., 1008., 511.5]
                  for i in range(12)}
        g = pack_maps(bounds)
        self.assertEqual(g, pack_maps(dict(reversed(list(bounds.items())))))
        validate_placements([{"name": m, "gofs": g[m], "bounds": b} for m, b in bounds.items()])
        extent = [[b[i] + g[m][i % 3] for i in range(6)] for m, b in bounds.items()]
        for i in range(3):
            self.assertLess(abs(min(b[i] for b in extent) + max(b[i + 3] for b in extent)), 32)
        self.assertLessEqual(max(float_ulp(x) for b in extent for x in b), 0.03125 / 16)
        for offset in g.values():
            for local in (-32768., -0.5, 0., 32767.):
                shifted = struct.unpack(">f", struct.pack(">f", local + offset[0]))[0]
                self.assertEqual(shifted - offset[0], local)

    def test_world_bounds_from_bsp_model_zero(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.bsp"
            b = bytearray(124 + 128)
            struct.pack_into("<i", b, 0, 30)
            struct.pack_into("<ii", b, 4 + 14 * 8, 124, 128)
            struct.pack_into("<6f", b, 124, -1, -2, -3, 4, 5, 6)
            struct.pack_into("<6f", b, 188, -100, -200, -300, 400, 500, 600)
            path.write_bytes(b)
            self.assertEqual(bsp_bounds(path), [-1, -2, -3, 4, 5, 6])

    def test_six_unit_ray_lattice(self):
        scene = Scene.__new__(Scene)
        scene.bsp = SimpleNamespace(models=[[0] * 9 + [0]])
        scene.pn = np.array([[1., 0., 0.]])
        scene.pd = np.array([10.])
        scene.np = np.array([0])
        scene.children = np.array([[-1, -2]])
        scene.lc = np.array([-2, -1])
        scene.occ = []
        distance, kind = scene.first_hit(np.array([[0., 0., 0.]]),
                                          np.array([[1., 0., 0.], [-1., 0., 0.]]), False)
        self.assertEqual(distance[0, 0], 12)
        self.assertEqual(kind.tolist(), [[1, 0]])
        self.assertTrue(np.isinf(distance[0, 1]))
        _, _, hits = scene.first_hit(np.array([[0., 0., 0.]]),
                                     np.array([[1., 0., 0.]]), False, details=True)
        self.assertEqual(hits["points"].tolist(), [[10., 0., 0.]])
        self.assertEqual(hits["models"].tolist(), [0])
        scene.lc[0] = -6
        _, kind = scene.first_hit(np.array([[0., 0., 0.]]), np.array([[1., 0., 0.]]), False)
        self.assertEqual(kind[0, 0], 2)

    def test_grid_endpoints(self):
        points = eye_grid(np.array([1., 0., 0.]), np.array([36., 0., 0.]))
        self.assertEqual(points[:, 0].tolist(), [1, 17, 33, 36])

    def test_twins_are_one_to_one_and_exclude_logic(self):
        a = SimpleNamespace(ents=[{"classname": "func_button", "origin": "10 0 0", "targetname": "go"},
                                 {"classname": "func_button", "origin": "10 0 0", "targetname": "go"},
                                 {"classname": "info_landmark", "origin": "10 0 0", "targetname": "go"}],
                            bsp=SimpleNamespace(models=[]))
        b = SimpleNamespace(ents=[{"classname": "func_button", "origin": "110 0 0", "targetname": "go"},
                                 {"classname": "info_landmark", "origin": "110 0 0", "targetname": "go"}],
                            bsp=SimpleNamespace(models=[]))
        pairs, _ = twins(a, b, np.zeros(3), np.array([100., 0., 0.]))
        self.assertEqual(pairs, [[0, 0]])

    def test_quoted_braces_preserve_hle_lump_indices(self):
        text = (b'{"classname" "worldspawn" "message" "a {nested} value"}\n'
                b'{"classname" "trigger_changelevel" "map" "next"}\n')
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "test.bsp"
            path.write_bytes(struct.pack("<III", 30, 12, len(text)) + text)
            ents = map_entities(path)
            self.assertEqual(len(ents), 2)
            self.assertEqual(ents[0]["message"], "a {nested} value")
            self.assertEqual(ents[1]["classname"], "trigger_changelevel")

    def test_floor_components_include_grid_endpoints_and_steps(self):
        eyes = np.array([[0., 0., 64.], [16., 0., 82.],
                         [20., 0., 100.], [80., 0., 64.]])
        self.assertEqual(sorted(len(c) for c in eye_components(eyes)), [1, 3])


class SpawnBasisTests(unittest.TestCase):
    def test_columns_are_forward_minus_right_up(self):
        yaw = spawn_rotation({"classname": "func_rot_button", "angles": "0 90 0"})
        np.testing.assert_allclose(yaw, [[0, -1, 0], [1, 0, 0], [0, 0, 1]], atol=1e-12)
        roll = spawn_rotation({"classname": "func_rot_button", "angles": "0 0 90"})
        np.testing.assert_allclose(roll, [[1, 0, 0], [0, 0, -1], [0, 1, 0]], atol=1e-12)
        pitch = spawn_rotation({"classname": "func_rot_button", "angles": "90 0 0"})
        np.testing.assert_allclose(pitch[:, 0], [0, 0, -1], atol=1e-12)

    def test_proper_rotation_for_any_angles(self):
        rng = np.random.default_rng(1)
        for _ in range(200):
            a = " ".join(f"{x:.3f}" for x in rng.uniform(-720, 720, 3))
            m = spawn_rotation({"classname": "func_rot_button", "angles": a})
            np.testing.assert_allclose(m.T @ m, np.eye(3), atol=1e-12)
            self.assertAlmostEqual(np.linalg.det(m), 1.0, places=12)

    def test_angle_key_and_spawn_resets(self):
        m = spawn_rotation({"classname": "func_rot_button", "angle": "180"})
        np.testing.assert_allclose(m[:, 0], [-1, 0, 0], atol=1e-12)
        for cn in ("func_door", "func_button", "func_wall", "momentary_door"):
            np.testing.assert_array_equal(spawn_rotation({"classname": cn, "angles": "10 20 30"}), np.eye(3))
        b = spawn_rotation({"classname": "func_breakable", "angles": "0 77 0"})
        np.testing.assert_array_equal(b, np.eye(3))
        np.testing.assert_allclose(spawn_rotation({"classname": "func_breakable", "angles": "90 77 0"})[:, 0],
                                   [0, 0, -1], atol=1e-12)

    def test_malformed_entity_keys_are_rejected(self):
        for e in ({"angles": "1 2"}, {"angles": "1 2 x"}, {"angles": "0 nan 0"}, {"angles": "inf 0 0"},
                  {"angle": "north"}):
            with self.assertRaises(EntityError):
                spawn_rotation({"classname": "func_rot_button", **e})
        bsp = SimpleNamespace(models=[None, [0] * 10])
        for text in ("*0", "*2", "*-1", "*x", "*", "*1.5"):
            with self.assertRaises(EntityError):
                model_index(bsp, {"classname": "func_wall", "model": text})
        self.assertEqual(model_index(bsp, {"model": "*1"}), 1)
        with self.assertRaises(EntityError):
            entity_box(bsp, {"classname": "func_wall", "model": "*1", "origin": "1 2"})

    def test_edict_classes_come_from_the_class_table(self):
        classes = edict_classes()
        self.assertIn("func_button", classes)
        self.assertNotIn("light", classes)
        self.assertNotIn("info_landmark", classes)
        with tempfile.TemporaryDirectory() as t:
            header = Path(t) / "h.h"
            header.write_text("/* nothing */")
            with self.assertRaisesRegex(ValueError, "no hle_classes"):
                edict_classes(header)
            header.write_text("hle_classes[] = {\n\t{NULL, 0}\n};")
            with self.assertRaisesRegex(ValueError, "empty hle_classes"):
                edict_classes(header)


class MoverReachTests(unittest.TestCase):
    """mover_reach: where stream.c's each_mover can find a brush entity."""

    def reach(self, ents, kinds=(2, 2, 2)):
        # submodel 0 is the world; 1..3 carry a MOVR (kind 2) chunk 10 + n,
        # local bbox -8..8 X/Y, 0..64 Z
        d = bytearray(16 * 4)
        for n, kind in enumerate(kinds, 1):
            struct.pack_into(">HH6h", d, 16 * n, kind, 10 + n, -8, -8, 0, 8, 8, 64)
        w = SimpleNamespace(d=bytes(d), sub_ofs=0)
        scene = SimpleNamespace(ents=ents, bsp=SimpleNamespace(models=[None] * 4))
        return {chunk: (lo, hi) for chunk, lo, hi in w64seam.mover_reach(scene, w)}

    def test_static_wall_is_its_placement_grown(self):
        got = self.reach([{"classname": "func_wall", "model": "*1", "origin": "100 0 0"}])
        np.testing.assert_allclose(got[11][0], [92 - 128, -8 - 128, -128])
        np.testing.assert_allclose(got[11][1], [108 + 128, 8 + 128, 64 + 128])

    def test_door_sweeps_its_travel_and_lead(self):
        got = self.reach([{"classname": "func_door", "model": "*1", "angles": "0 -1 0",
                           "lip": "4", "speed": "200"}])
        pad = 128 + 200 * .5
        np.testing.assert_allclose(got[11][0], [-8 - pad, -8 - pad, -pad])
        np.testing.assert_allclose(got[11][1], [8 + pad, 8 + pad, 64 + 60 + pad])

    def test_rotating_and_train_reach(self):
        got = self.reach([
            {"classname": "func_door_rotating", "model": "*1"},
            {"classname": "func_train", "model": "*2", "target": "p1", "speed": "0"},
            {"classname": "path_corner", "targetname": "p1", "target": "p2", "origin": "500 0 0"},
            {"classname": "path_corner", "targetname": "p2", "target": "p1", "origin": "0 0 -300"},
            {"classname": "func_wall", "model": "*3"}], kinds=(2, 2, 0))
        r = np.linalg.norm([8, 8, 64])
        np.testing.assert_allclose(got[11][0], [-r - 128, -r - 128, -r - 128])
        np.testing.assert_allclose(got[12][0], [-8 - 128, -8 - 128, -300 - 32 - 128])
        np.testing.assert_allclose(got[12][1], [500 + 8 + 128, 8 + 128, 64 + 128])
        self.assertNotIn(13, got)


class SeamCliTests(unittest.TestCase):
    """w64seam rejects malformed input before proving anything."""

    def run_seam(self, graph, maps, packages, *extra):
        report = Path(graph).parent / "seams.json"
        out, err = io.StringIO(), io.StringIO()
        argv = ["w64seam", str(graph), "--maps-dir", str(maps), "--packages", str(packages),
                "--report", str(report), *extra]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = w64seam.main()
        return rc, err.getvalue()

    def rejected(self, graph, maps, packages, where):
        rc, err = self.run_seam(graph, maps, packages)
        self.assertEqual(rc, 1, err)
        self.assertTrue(err.startswith("w64seam: error: "), err)
        self.assertIn(where, err)
        self.assertNotIn("Traceback", err)
        return err

    def campaign(self, root):
        maps = two_maps(root)
        graph = Path(root) / "campaign.cwg"
        self.assertEqual(cli(campaign_main, "--maps-dir", maps, "-o", graph)[0], 0)
        return maps, graph

    def test_graph_problems(self):
        with tempfile.TemporaryDirectory() as t:
            maps, graph = self.campaign(t)
            good = graph.read_bytes()
            for data in (b"", good[:20], good[:-1], b"CWG3" + bytes(60)):
                graph.write_bytes(data)
                self.rejected(graph, maps, t, "campaign.cwg")
            graph.write_bytes(good)
            self.rejected(Path(t) / "gone.cwg", maps, t, "gone.cwg")
            b = bytearray(good)
            b[b.index(b"a\0")] = ord("/")
            graph.write_bytes(bytes(b))
            self.assertIn("plain file name", self.rejected(graph, maps, t, "campaign.cwg"))

    def test_map_problems(self):
        with tempfile.TemporaryDirectory() as t:
            maps, graph = self.campaign(t)
            good = (maps / "a.bsp").read_bytes()
            (maps / "a.bsp").unlink()
            self.rejected(graph, maps, t, "a.bsp")
            for data, msg in ((b"", "shorter than a BSP header"), (good[:90], "shorter than a BSP header"),
                              (struct.pack("<i", 29) + good[4:], "not BSP30"),
                              (good, "outside the")):
                (maps / "a.bsp").write_bytes(data)
                err = self.rejected(graph, maps, t, "a.bsp")
                if data is not good:
                    self.assertIn(msg, err)
            self.rejected(graph, Path(t) / "nomaps", t, "a.bsp")


if __name__ == "__main__":
    unittest.main()
