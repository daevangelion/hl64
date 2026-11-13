# SPDX-License-Identifier: MIT
"""Tests of tools/studio (docs/formats/studio-format.md) on the player's
Half-Life models ($HL64_DATA, default /scratch/hl1/valve; skipped when
absent): the cooked blocks validate, carry the MDL's counts and values,
the animation chunks reproduce every channel, and tools/restab pairs MODEL
and ANIM ids and writes event resources idempotently.

  HL64_DATA=.../valve python3 -m unittest tools.studio.test_studiocook
"""
import os
import shutil
import struct
import tempfile
import unittest

from tools.hlsdk import studio_mdl as mdl
from tools.restab import restab, rtb
from tools.studio import s64, studiocook

DATA = os.environ.get("HL64_DATA", "/scratch/hl1/valve")
MODELS = ["models/scientist.mdl", "models/barney.mdl", "models/zombie.mdl",
          "models/hgrunt.mdl", "models/v_9mmhandgun.mdl"]


def read(path):
    with open(path, "rb") as f:
        return f.read()


@unittest.skipUnless(os.path.isfile(os.path.join(DATA, "models", "scientist.mdl")),
                     "player data with models/scientist.mdl")
class StudioCook(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.get = staticmethod(studiocook.lookup(DATA))
        cls.cooked = {n: studiocook.cook_model(cls.get, n) for n in MODELS}

    def test_validates_and_counts(self):
        for n, (model, anim, st) in self.cooked.items():
            m = mdl.read(self.get, n)
            info = s64.read_model(model)
            s64.read_anim(anim, info)
            self.assertEqual(info.numbones, len(m.bones), n)
            self.assertEqual(info.numseq, len(m.seqs), n)
            self.assertEqual(info.labels, [s.label for s in m.seqs], n)
            self.assertEqual(info.frames, [s.numframes for s in m.seqs], n)
            self.assertEqual(len(info.events), sum(len(s.events) for s in m.seqs), n)
            self.assertEqual(info.nsub, sum(len(p.models) for p in m.bodyparts), n)
            self.assertGreater(st["tris"], 100, n)
            self.assertEqual(len(model) % 16, 0)
            self.assertEqual(len(anim) % 16, 0)

    def test_anim_chunks_hold_every_channel(self):
        """Each chunk's channel words equal the MDL's run-length values
        (the first words: the walk reads from the first run on)."""
        model, anim, _ = self.cooked["models/scientist.mdl"]
        m = mdl.read(self.get, "models/scientist.mdl")
        info = s64.read_model(model)
        nb = len(m.bones)
        for k, s in enumerate(m.seqs):
            ofs, size = info.anim[k]
            for bl in range(max(1, s.numblends)):
                for b in range(nb):
                    for ch in range(6):
                        w = struct.unpack_from(">H", anim, ofs + 2 * ((bl * nb + b) * 6 + ch))[0]
                        vals = s.anim[bl][b][ch]
                        if vals is None:
                            self.assertEqual(w, 0)
                            continue
                        got = struct.unpack_from(">%dh" % len(vals), anim, ofs + 2 * w)
                        self.assertEqual(list(got), vals, (s.label, b, ch))

    def test_mesh_covers_every_body_part_and_family(self):
        model, _, _ = self.cooked["models/scientist.mdl"]
        m = mdl.read(self.get, "models/scientist.mdl")
        mo = struct.unpack_from(">I", model, 16)[0]
        nfam = model[mo + 19]
        nsref = model[mo + 18]
        self.assertEqual(nfam, len(m.skinref))
        self.assertEqual(nsref, len(m.skinref[0]))
        self.assertGreaterEqual(len(m.bodyparts[1].models), 4)     # the heads

    def test_deterministic(self):
        again = studiocook.cook_model(self.get, "models/barney.mdl")
        self.assertEqual(again[0], self.cooked["models/barney.mdl"][0])
        self.assertEqual(again[1], self.cooked["models/barney.mdl"][1])

    def test_scientist_events(self):
        info = s64.read_model(self.cooked["models/scientist.mdl"][0])
        walk = info.labels.index("walk")
        evs = [(e.event, e.options) for e in info.events if e.seq == walk]
        self.assertIn((1004, "common/npc_step1.wav"), evs)

    def test_triangles_wind_like_the_world(self):
        """Cooked studio triangles face the way BSP faces do: front faces
        wind clockwise seen from outside (edge cross product against the
        normal < 0), so the cart culls both with one flag (plat/n64/n64.h
        N64_CULL). A studio pass culling the other side shows the inside
        of the far half: a model that seems to turn with the view."""
        model, _, _ = self.cooked["models/barney.mdl"]
        mo = struct.unpack_from(">I", model, 16)[0]
        (nmesh,) = struct.unpack_from(">H", model, mo + 8)
        (ml_ofs, ld_ofs, vt_ofs, tri_ofs) = struct.unpack_from(">4I", model, mo + 24 + 12)

        def nrm(p):
            def sx(v, bits):
                return v - (1 << bits) if v & (1 << (bits - 1)) else v
            return (sx(p >> 11, 5) / 15.5, sx((p >> 5) & 63, 6) / 31.5, sx(p & 31, 5) / 15.5)

        def vert(s):
            o = mo + vt_ofs + 32 * (s >> 1) + 8 * (s & 1)
            x, y, z, n = struct.unpack_from(">3hH", model, o)
            return (x, y, z), nrm(n)

        def wind(p, n):
            a = [p[1][i] - p[0][i] for i in range(3)]
            b = [p[2][i] - p[0][i] for i in range(3)]
            c = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
            return sum(c[i] * n[i] for i in range(3))

        cw = ccw = 0
        slot = 0
        for k in range(nmesh):
            lf, nl, _, tf, nt = struct.unpack_from(">HBBHH", model, mo + ml_ofs + 8 * k)
            cache = {}
            for ld in range(lf, lf + nl):
                bs, c0, cnt = struct.unpack_from(">BBB", model, mo + ld_ofs + 4 * ld)
                for j in range(cnt):
                    cache[c0 + j] = (bs,) + vert(slot + j)
                slot += cnt + (cnt & 1)
            for t in range(tf, tf + nt):
                vs = [cache[i] for i in model[mo + tri_ofs + 3 * t:mo + tri_ofs + 3 * t + 3]]
                if len({v[0] for v in vs}) != 1:
                    continue
                d = wind([v[1] for v in vs], [sum(v[2][i] for v in vs) for i in range(3)])
                cw += d < 0
                ccw += d > 0
        self.assertGreater(cw, 9 * ccw, (cw, ccw))

        # the world rule on the map the studio models stand in
        bsp = read(os.path.join(DATA, "maps", "hldemo1.bsp"))
        lump = [struct.unpack_from("<ii", bsp, 4 + 8 * i) for i in range(15)]
        cw = ccw = 0
        for f in range(lump[7][1] // 20):
            pn, side, fe, ne = struct.unpack_from("<HhiH", bsp, lump[7][0] + 20 * f)
            pts = []
            for k in range(fe, fe + 3):
                se = struct.unpack_from("<i", bsp, lump[13][0] + 4 * k)[0]
                ed = struct.unpack_from("<HH", bsp, lump[12][0] + 4 * abs(se))
                pts.append(struct.unpack_from("<3f", bsp, lump[3][0] + 12 * ed[0 if se >= 0 else 1]))
            n = struct.unpack_from("<3f", bsp, lump[1][0] + 20 * pn)
            d = wind(pts, [-c for c in n] if side else n)
            cw += d < -1e-6
            ccw += d > 1e-6
        self.assertGreater(cw, 9 * ccw, (cw, ccw))


@unittest.skipUnless(os.path.isfile(os.path.join(DATA, "models", "scientist.mdl")),
                     "player data with models/scientist.mdl")
class RestabAnim(unittest.TestCase):
    def test_model_anim_pairs_and_event_ids(self):
        tmp = tempfile.mkdtemp(prefix="hl64-studio-")
        try:
            get = studiocook.lookup(DATA)
            os.makedirs(os.path.join(tmp, "models"))
            os.makedirs(os.path.join(tmp, "sound", "common"))
            os.makedirs(os.path.join(tmp, "maps"))
            for n in ("models/scientist.mdl", "models/barney.mdl"):
                model, anim, _ = studiocook.cook_model(get, n)
                with open(os.path.join(tmp, n[:-4] + ".s64"), "wb") as f:
                    f.write(model)
                with open(os.path.join(tmp, n[:-4] + ".a64"), "wb") as f:
                    f.write(anim)
            for w in ("npc_step1", "npc_step2"):
                with open(os.path.join(tmp, "sound", "common", w + ".wav64"), "wb") as f:
                    f.write(b"\0" * 16)
            # an empty campaign: CWG3 header with no maps
            cwg = bytearray(32)
            struct.pack_into(">IHH", cwg, 0, 0x43574733, 3, 0)
            with open(os.path.join(tmp, "maps", "campaign.cwg"), "wb") as f:
                f.write(cwg)
            defs = os.path.join(tmp, "defs")
            os.makedirs(defs)
            with open(os.path.join(defs, "res.def"), "w") as f:
                f.write('RES(MODEL, SCI, "models/scientist.mdl")\n'
                        'RES(MODEL, BARNEY, "models/barney.mdl")\n')
            t = restab.cook(tmp, defs)
            data = rtb.write(t)
            t2 = rtb.read(data)
            models = [n for e, n in zip(t2.res, t2.res_names) if e.kind == rtb.KIND_MODEL]
            anims = [n for e, n in zip(t2.res, t2.res_names) if e.kind == rtb.KIND_ANIM]
            self.assertEqual(models, anims)
            files = [f.path for f in t2.files]
            for e, n in zip(t2.res, t2.res_names):
                if e.kind == rtb.KIND_MODEL:
                    self.assertTrue(files[e.file].endswith(".s64"))
                    self.assertEqual(e.flags, 0)
                if e.kind == rtb.KIND_ANIM:
                    self.assertTrue(files[e.file].endswith(".a64"))
            sci = read(os.path.join(tmp, "models", "scientist.s64"))
            step = rtb.res_t(rtb.KIND_SOUND, sorted(
                n for e, n in zip(t2.res, t2.res_names) if e.kind == rtb.KIND_SOUND)
                .index("common/npc_step1.wav"))
            info = s64.read_model(sci)
            got = {struct.unpack_from(">H", sci, e.res_at)[0] for e in info.events
                   if e.options == "common/npc_step1.wav"}
            self.assertEqual(got, {step})
            none = {struct.unpack_from(">H", sci, e.res_at)[0] for e in info.events
                    if e.options == "common/npc_step3.wav"}
            self.assertEqual(none, {0})          # not staged: no resource
            restab.cook(tmp, defs)
            self.assertEqual(read(os.path.join(tmp, "models", "scientist.s64")), sci)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
