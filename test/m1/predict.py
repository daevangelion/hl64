# SPDX-License-Identifier: MIT
"""Cook-side prediction of the world draw list (M1 gate, architecture.md 17).

For every `shot N ren:` line of an hl64 log (host recorder or cart), walk
the cooked .w64 package the way docs/formats/w64-format.md "Runtime
requirements" says the runtime must -- eye leaf through the grid start
node and the hull-0 COLL nodes, the leaf's visible-sector list, sector and
meshlet boxes against the four frustum planes, texture animation
(world-surfaces.md 4), static movers from the ENTS chunk (origin key; at
the map origin they join the world list), page sort and binds -- and
compare the predicted sectors, meshlets, vertices, triangles and binds
with the renderer's `ren:` line and the draw-list recorder's `gpu:` line.
Written from the format documents; it shares no code with engine/.

  python3 test/m1/predict.py MAP.w64 campaign.cwg LOG [--fov DEG]

Frustum arithmetic is float32 in the runtime's operation order. Exit 0
when every shot matches, 1 on a mismatch, 2 on bad input.
"""
import argparse
import re
import struct
import sys

import ctypes
import ctypes.util

import numpy as np

# The runtime's float32 trig is libm's sinf/cosf/tanf/atanf (the host build;
# the cart's newlib may differ by an ulp at a frustum edge).
_LIBM = ctypes.CDLL(ctypes.util.find_library('m'))
for _fn in ('sinf', 'cosf', 'tanf', 'atanf'):
    getattr(_LIBM, _fn).restype = ctypes.c_float
    getattr(_LIBM, _fn).argtypes = [ctypes.c_float]


def _f32(fn):
    return lambda x: np.float32(fn(float(x)))


SIN, COS, TAN, ATAN = (_f32(getattr(_LIBM, n)) for n in ('sinf', 'cosf', 'tanf', 'atanf'))

F = np.float32
REND, MOVR, TEXP = 0x52454E44, 0x4D4F5652, 0x54455850
TEXF_ANIM = 4
SUB_MOVR = 2
NONE = 0xFFFF
GID_BITS = 12


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


class Package:
    """The tables of a .w64 package the draw list needs."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.b = b = f.read()
        magic, _ver, _flags, file_size, d = struct.unpack_from('>I2H2I', b, 0)
        if magic != 0x5736344C or file_size != len(b):
            raise SystemExit('%s: not a w64 package' % path)
        (self.nsect, self.nleaf, self.ntex, self.npages, self.nsub, self.nchunks,
         self.texp_first, _nvis) = struct.unpack_from('>8H', b, d)
        chunkdir, sect, _pagepool, tex, sub, leaf, vispool = struct.unpack_from('>7I', b, d + 16)
        self.coll_first, _ncoll, self.coll_pinned, _nleafs, self.ents_chunk, _visr = \
            struct.unpack_from('>6H', b, d + 44)
        model, _leafc, grid, _mat = struct.unpack_from('>4I', b, d + 56)
        self.chunks = [struct.unpack_from('>4I', b, d + chunkdir + 16 * i)
                       for i in range(self.nchunks)]
        self.sects = [(struct.unpack_from('>6h', b, d + sect + 20 * i),
                       u16(b, d + sect + 20 * i + 12)) for i in range(self.nsect)]
        self.texs = []
        for i in range(self.ntex):
            page, flags, total, amin, amax, _pad, nxt, alt = \
                struct.unpack_from('>H4BH2H', b, d + tex + 12 * i)
            self.texs.append((page, flags, total, amin, amax, nxt, alt))
        self.subs = [(u16(b, d + sub + 16 * i), u16(b, d + sub + 16 * i + 2),
                      struct.unpack_from('>6h', b, d + sub + 16 * i + 4)) for i in range(self.nsub)]
        self.leafvis = [struct.unpack_from('>I', b, d + leaf + 4 * i)[0] for i in range(self.nleaf)]
        self.vispool = d + vispool
        self.head0 = struct.unpack_from('>i', b, d + model + 36)[0]
        g = d + grid
        self.gorg = struct.unpack_from('>3h', b, g)
        self.gdims = struct.unpack_from('>3H', b, g + 6)
        self.gcell = u16(b, g + 12)
        cells, start, _lists = struct.unpack_from('>3I', b, g + 16)
        self.cells, self.starts = d + cells, d + start

    def chunk(self, c):
        _fc, ofs, size, _crc = self.chunks[c]
        return self.b[ofs:ofs + size]

    def leaf_sectors(self, leaf):
        o = self.vispool + 2 * self.leafvis[leaf if 0 <= leaf < self.nleaf else 0]
        return [u16(self.b, o + 2 + 2 * i) for i in range(u16(self.b, o))]

    def point_leaf(self, p):
        """Hull-0 leaf of map-local p (float32): grid start, d >= 0 -> child 0."""
        node = self.head0
        idx = []
        for k in range(3):
            f = F(F(p[k]) - F(self.gorg[k])) / F(self.gcell)
            if not f >= 0 or f >= self.gdims[k]:
                break
            idx.append(int(f))
        if len(idx) == 3:
            cell = idx[0] + self.gdims[0] * (idx[1] + self.gdims[1] * idx[2])
            s = u16(self.b, self.cells + 4 * cell + 2)
            node = struct.unpack_from('>i', self.b, self.starts + 4 * s)[0]
        while node >= 0:
            ci, li = node >> GID_BITS, node & ((1 << GID_BITS) - 1)
            ch = self.chunk(self.coll_first + ci)
            nn, nx, ny, nz, npl, _next = struct.unpack_from('>6H', ch, 0)
            o = 16 + 16 * npl + 8 * li
            if li < nz:
                axis = 0 if li < nx else 1 if li < ny else 2
                dist = F(struct.unpack_from('>f', ch, o)[0])
                dd = F(p[axis]) - dist
            else:
                n0, n1, n2, dist = (F(v) for v in struct.unpack_from(
                    '>4f', ch, 16 + 16 * struct.unpack_from('>I', ch, o)[0]))
                dd = F(F(F(n0 * F(p[0])) + F(n1 * F(p[1]))) + F(n2 * F(p[2]))) - dist
            c = struct.unpack_from('>2h', ch, o + 4)[0 if dd >= 0 else 1]
            if c < 0:
                leaf = -1 - c
                return leaf if leaf < self.nleaf else 0
            node = (ci << GID_BITS | c) if c < nn else \
                struct.unpack_from('>I', ch, 16 + 16 * npl + 8 * nn + 4 * (c - nn))[0]
        leaf = -1 - node
        return leaf if leaf < self.nleaf else 0

    def tex_anim(self, t, r10):
        """world-surfaces.md 4 with entity frame 0."""
        page, flags, total, amin, amax, nxt, alt = self.texs[t]
        if not flags & TEXF_ANIM or not total:
            return t
        k, start = r10 % total, t
        for _ in range(self.ntex):
            page, flags, total, amin, amax, nxt, alt = self.texs[t]
            if amin <= k < amax:
                return t
            t = nxt
        return start

    def movers(self):
        """(sub, origin) per kept ENTS record with a MOVR "*N" model and no
        render mode, record order (HLE1, little-endian)."""
        if self.ents_chunk == NONE:
            return []
        b = self.chunk(self.ents_chunk)
        nrecs, ncls, nkn = struct.unpack_from('<3H', b, 18)
        o_recs, o_cls, o_kn = struct.unpack_from('<I', b, 52)[0], *struct.unpack_from('<2I', b, 60)
        o_keys, o_fl, o_str = struct.unpack_from('<I', b, 76)[0], *struct.unpack_from('<2I', b, 88)

        def s(o):
            return b[o_str + o:b.index(b'\0', o_str + o)].decode('latin-1')
        names = [s(struct.unpack_from('<I', b, o_kn + 8 * i)[0]) for i in range(nkn)]
        out = []
        for r in range(nrecs):
            kf, kc = struct.unpack_from('<2H', b, o_recs + 24 * r + 8)
            sub, org, rmode = -1, (0.0, 0.0, 0.0), 0.0
            for k in range(kf, kf + kc):
                kn, _pad, val = struct.unpack_from('<2HI', b, o_keys + 8 * k)
                name = names[kn]
                if name == 'model':
                    m = s(val)
                    if re.fullmatch(r'\*\d+', m):
                        sub = int(m[1:])
                elif name == 'origin':
                    org = struct.unpack_from('<3f', b, o_fl + 4 * val)
                elif name == 'rendermode':
                    rmode = struct.unpack('<f', struct.pack('<I', val))[0]
            if 0 < sub < self.nsub and self.subs[sub][0] == SUB_MOVR and rmode == 0:
                out.append((sub, org))
        return out


def cwg_origin(path, name):
    b = open(path, 'rb').read()
    nmaps, nlm, nln = struct.unpack_from('>3H', b, 8)
    nseam, ntwin, npool = struct.unpack_from('>3H', b, 20)
    strofs = 32 + 48 * nmaps + 16 * nlm + 8 * nln + 136 * nseam + 4 * ntwin + 2 * npool
    for i in range(nmaps):
        o = 32 + 48 * i
        n = u16(b, o)
        if b[strofs + n:b.index(b'\0', strofs + n)].decode() == name:
            return struct.unpack_from('>3f', b, o + 12)
    raise SystemExit('%s: no map %s' % (path, name))


def axes(ang):
    k = F(3.14159265358979) / F(180)
    sp, cp = SIN(F(ang[0]) * k), COS(F(ang[0]) * k)
    sy, cy = SIN(F(ang[1]) * k), COS(F(ang[1]) * k)
    sr, cr = SIN(F(ang[2]) * k), COS(F(ang[2]) * k)
    f = [cp * cy, cp * sy, -sp]
    r = [-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp]
    u = [cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp]
    return [F(v) for v in f], [F(v) for v in r], [F(v) for v in u]


def frustum(ang, fov):
    f, r, u = axes(ang)
    k = F(3.14159265358979) / F(360)
    fy = ATAN(TAN(F(fov) * k) * F(0.75)) / k
    fx, fy = F(fov) * k, F(fy) * k
    sx, cx, sy, cy = SIN(fx), COS(fx), SIN(fy), COS(fy)
    return [[F(f[i] * sx + r[i] * cx) for i in range(3)],
            [F(f[i] * sx - r[i] * cx) for i in range(3)],
            [F(f[i] * sy + u[i] * cy) for i in range(3)],
            [F(f[i] * sy - u[i] * cy) for i in range(3)]]


def plane_d(planes, eye, org):
    e = [F(F(eye[i]) - F(org[i])) for i in range(3)]
    return [F(F(n[0] * e[0] + n[1] * e[1]) + n[2] * e[2]) for n in planes]


def culled(planes, d, bb):
    for n, di in zip(planes, d):
        v = F(F(n[0] * F(bb[3] if n[0] >= 0 else bb[0]) + n[1] * F(bb[4] if n[1] >= 0 else bb[1]))
              + n[2] * F(bb[5] if n[2] >= 0 else bb[2]))
        if v < di:
            return True
    return False


def predict(pkg, gofs, eye, ang, r10, fov):
    planes = frustum(ang, fov)
    page0 = [pkg.texs[pkg.tex_anim(t, r10)][0] for t in range(pkg.ntex)]
    local = [F(F(eye[i]) - F(gofs[i])) for i in range(3)]
    leaf = pkg.point_leaf(local)
    out = dict(leaf=leaf, sectors=0, movers=0, meshlets=0, verts=0, tris=0, binds=0)

    def listing(ch, d):
        nm = u16(ch, 0)
        items = []
        for k in range(nm):
            o = 24 + 24 * k
            bb = struct.unpack_from('>6h', ch, o)
            vert, idx, tex, _style = struct.unpack_from('>2I2H', ch, o + 12)
            if culled(planes, d, bb) or page0[tex] == NONE:
                continue
            ni = idx & 0xFF
            faces = sum(1 for j in range(ni) if u16(ch, (idx >> 8) + 2 * j) & 0x8000)
            items.append((page0[tex], vert & 0xFF, ni - 2 * faces))
        return items

    lists = []
    d = plane_d(planes, eye, gofs)
    world = []
    for s in pkg.leaf_sectors(leaf):
        bb, rc = pkg.sects[s]
        if culled(planes, d, bb):
            continue
        out['sectors'] += 1
        world += listing(pkg.chunk(rc), d)
    movers = pkg.movers()
    seen = [pkg.sects[s][0] for s in pkg.leaf_sectors(leaf)]

    def visible(sub, org):
        """placement box meets a visible sector's box (architecture.md 5.5)"""
        mb = pkg.subs[sub][2]
        return any(all(F(mb[k]) + F(org[k]) <= sb[k + 3] and F(mb[k + 3]) + F(org[k]) >= sb[k]
                       for k in range(3)) for sb in seen)
    for sub, org in movers:
        if org != (0.0, 0.0, 0.0) or not visible(sub, org) or culled(planes, d, pkg.subs[sub][2]):
            continue
        out['movers'] += 1
        world += listing(pkg.chunk(pkg.subs[sub][1]), d)
    lists.append(world)
    for sub, org in movers:
        if org == (0.0, 0.0, 0.0) or not visible(sub, org):
            continue
        g = [F(F(gofs[i]) + F(org[i])) for i in range(3)]
        dm = plane_d(planes, eye, g)
        if culled(planes, dm, pkg.subs[sub][2]):
            continue
        out['movers'] += 1
        items = listing(pkg.chunk(pkg.subs[sub][1]), dm)
        if items:
            lists.append(items)
    last = None
    for items in lists:
        for page, nv, tris in sorted(items, key=lambda x: x[0]):
            if page != last:
                out['binds'] += 1
                last = page
            out['meshlets'] += 1
            out['verts'] += nv
            out['tris'] += tris
    return out


def fields(line):
    return dict(kv.split('=', 1) for kv in line.split() if '=' in kv)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('w64')
    ap.add_argument('cwg')
    ap.add_argument('log')
    ap.add_argument('--fov', type=float, default=90.0)
    a = ap.parse_args()
    pkg = Package(a.w64)
    gofs = None
    lines = open(a.log, encoding='latin-1').read().replace('\r', '').splitlines()
    gpu = {m.group(1): fields(m.group(2)) for m in
           (re.match(r'shot (\d+) gpu: (.*)', ln) for ln in lines) if m}
    bad = shots = 0
    for ln in lines:
        m = re.match(r'shot (\d+) ren: (.*)', ln)
        if not m:
            continue
        shots += 1
        r = fields(m.group(2))
        if gofs is None:
            gofs = cwg_origin(a.cwg, r['map'])
        eye = [float(v) for v in r['eye'].split(',')]
        ang = [float(v) for v in r['ang'].split(',')]
        p = predict(pkg, gofs, eye, ang, int(r['r10']), a.fov)
        g = gpu.get(m.group(1), {})
        row = []
        for k in ('leaf', 'sectors', 'movers', 'meshlets', 'verts', 'tris', 'binds'):
            got = int(r[k])
            ok = got == p[k] and (k not in g or int(g[k]) == p[k] or k == 'tris' and g[k] == '0')
            row.append('%s=%d%s' % (k, p[k], '' if ok else '(ren %d gpu %s)' % (got, g.get(k, '-'))))
            bad += not ok
        if r.get('missing', '0') != '0':
            row.append('missing=%s' % r['missing'])
            bad += 1
        print('shot %s predict %s' % (m.group(1), ' '.join(row)))
    if not shots:
        print('predict: no shot lines in %s' % a.log)
        return 2
    print('predict: %s (%d shots)' % ('PASS' if not bad else 'FAIL', shots))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
