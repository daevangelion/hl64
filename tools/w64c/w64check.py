# SPDX-License-Identifier: MIT
"""w64check — validate a version-3 .w64 against its BSP30 source.

Cook-side proof that the package is exact (docs/formats/w64-format.md):
  - header / directory / chunk CRCs and bounds, every directory table checked
    against the directory, meshlet and strip encoding, probe blocks;
  - model table = BSP dmodel_t (raw floats, visleafs), leaf contents;
  - collision: for random points (a quarter of them snapped to the 1/16 u
    lattice so plane ties are exercised) the point contents of hulls 0-3 of
    the world model and of hulls 0-3 of several submodels through the COLL
    encoding (gids, externs, axial/non-axial nodes) equal a direct walk of
    the BSP nodes / clipnodes, with identical path length; point -> leaf
    from the grid start node equals point -> leaf from the root; every node
    a world point walk visits lies in a pinned chunk (hull 0) or a chunk its
    cell lists (hulls 1-3); every node a submodel walk visits lies in that
    model's chunk range (hull 0 of a traced submodel: the pinned chunks);
  - exact, per grid cell (box arithmetic on the decoded trees, 0.01 u
    margin): the start node lies on the path the whole cell follows from
    the root, and every hull 1-3 node a box-pruned walk of the cell reaches
    lies in a pinned chunk or a chunk of the cell's list;
  - random short segments (<= 192 u, start inside the grid): the segment
    trace of docs/specs/trace-and-hulls.md 3 (Tracer below) on the COLL
    encoding gives the same result and visits the same number of nodes as on
    the BSP, and every node it visits lies in a pinned chunk or in the
    3x3x3 neighbourhood lists of the start cell;
  - VISR rows decompress to the BSP PVS rows; ENTS equals the hlecook blob;
    every chunk is a known kind (the old ZONE / DRYS / STRM companion chunks
    are skipped, at most once each, as w64-format.md says readers do; a DECL
    chunk, at most once, is a valid decal sheet).

Usage (from the repository root):
  python3 -m tools.w64c.w64check <map.w64> <map.bsp> [--hle map.hle]
              [--points N] [--segments N] [--submodels N] [--seed S]
Exit status 1 on any mismatch. A malformed .w64 or BSP (truncated, a lump,
table or chunk outside the file, bad magic or version, an index out of
range, an absurd count, a cyclic tree) is reported as
`w64check: error: <file>: <what>` on stderr, also exit status 1.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

from tools.cook import decals

F32 = np.float32
LOCAL_BITS = 12
DIST_EPSILON = 0.03125
BACKOFF_STEP = 0.1
CONTENTS_EMPTY, CONTENTS_SOLID = -1, -2
NONE = 0xFFFF
MAX_COLL_CHUNKS = 1024      # the dense gid tables below cost ~140 MB at this count
MAX_CELL_BITS = 1 << 26     # grid cells x COLL chunks (membership matrix)
KNOWN_CHUNKS = {b"REND", b"MOVR", b"TEXP", b"COLL", b"ENTS", b"VISR"}
SKIPPED_CHUNKS = {b"ZONE", b"DRYS", b"STRM"}


class Fail(Exception):
    """A verification mismatch: the package is not what its BSP says."""


class BadInput(ValueError):
    """A malformed input file; the message is `<file>: <what>`."""


def need(cond, msg):
    if not cond:
        raise Fail(msg)


def bad(cond, path, msg):
    if not cond:
        raise BadInput(f"{path}: {msg}")


def read_file(path: Path) -> bytes:
    try:
        return Path(path).read_bytes()
    except OSError as e:
        raise BadInput(f"{path}: {e.strerror or e}") from None


def check_acyclic(children: dict, path, what):
    """children: node -> (child, child); a value >= 0 names another node of the
    dict, a negative one is a leaf. Raises BadInput when a node can reach itself."""
    state = dict.fromkeys(children, 0)      # 0 new, 1 on the walk stack, 2 done
    for root in children:
        if state[root]:
            continue
        state[root] = 1
        stack = [(root, 0)]
        while stack:
            node, k = stack[-1]
            if k == 2:
                state[node] = 2
                stack.pop()
                continue
            stack[-1] = (node, k + 1)
            c = children[node][k]
            if c < 0:
                continue
            if state[c] == 1:
                raise BadInput(f"{path}: {what}: node {c} is its own descendant")
            if state[c] == 0:
                state[c] = 1
                stack.append((c, 0))


# --------------------------------------------------------------------------
# BSP30 (independent of the cook)
# --------------------------------------------------------------------------

class Bsp:
    def __init__(self, path: Path):
        d = read_file(path)
        bad(len(d) >= 4 + 15 * 8, path, "shorter than a BSP header")
        version = struct.unpack_from("<i", d, 0)[0]
        bad(version == 30, path, f"not BSP30 (version {version})")
        lumps = [struct.unpack_from("<ii", d, 4 + 8 * i) for i in range(15)]

        def lump(i, dt):
            o, n = lumps[i]
            dt = np.dtype(dt)
            bad(0 <= o and 0 <= n and o + n <= len(d), path,
                f"lump {i} ({o}+{n}) outside the {len(d)}-byte file")
            bad(n % dt.itemsize == 0, path,
                f"lump {i} size {n} is not a multiple of {dt.itemsize}")
            return np.frombuffer(d, dt, n // dt.itemsize, o)

        pl = lump(1, [("n", "<f4", 3), ("d", "<f4"), ("t", "<i4")])
        self.pn = pl["n"].astype(F32)
        self.pd = pl["d"].astype(F32)
        self.pt = pl["t"].astype(np.int64)
        bad(((self.pt >= 0) & (self.pt <= 5)).all(), path, "plane type outside 0..5")
        nd = lump(5, [("p", "<i4"), ("c", "<i2", 2), ("bb", "<i2", 6), ("f", "<u2", 2)])
        self.nplane = nd["p"].astype(np.int64)
        self.nchild = nd["c"].astype(np.int64)
        cl = lump(9, [("p", "<i4"), ("c", "<i2", 2)])
        self.cplane = cl["p"].astype(np.int64)
        self.cchild = cl["c"].astype(np.int64)
        lf = lump(10, [("c", "<i4"), ("v", "<i4"), ("bb", "<i2", 6), ("m", "<u2", 2), ("a", "u1", 4)])
        self.leafc = lf["c"].astype(np.int64)
        self.leafv = lf["v"].astype(np.int64)
        md = lump(14, [("f", "<f4", 9), ("h", "<i4", 4), ("v", "<i4"), ("ff", "<i4"), ("nf", "<i4")])
        self.mraw = md["f"]
        self.mhead = md["h"].astype(np.int64)
        self.mvis = md["v"].astype(np.int64)
        o, n = lumps[4]
        bad(0 <= o and 0 <= n and o + n <= len(d), path, f"lump 4 ({o}+{n}) outside the file")
        self.vis = d[o:o + n]
        nl, nn, nc, npl = len(self.leafc), len(self.nplane), len(self.cplane), len(self.pn)
        bad(len(self.mraw) >= 1, path, "no models")
        bad(nl >= 1, path, "no leaves")
        bad(np.isfinite(self.mraw).all(), path, "model bounds are not finite")
        self.visleafs = int(self.mvis[0])
        bad(0 <= self.visleafs < nl, path, f"world visleafs {self.visleafs} outside {nl} leaves")
        bad(((self.nplane >= 0) & (self.nplane < npl)).all(), path, "node plane index out of range")
        bad(((self.cplane >= 0) & (self.cplane < npl)).all(), path, "clipnode plane index out of range")
        for what, ch, count in (("node", self.nchild, nn), ("clipnode", self.cchild, nc)):
            pos = ch[ch >= 0]
            bad((pos < count).all(), path, f"{what} child index out of range")
        neg = self.nchild[self.nchild < 0]
        bad((-1 - neg < nl).all(), path, "node child names a leaf outside the leaf lump")
        for m in range(len(self.mhead)):
            for h in range(4):
                head = int(self.mhead[m, h])
                count = (nn, nc, nc, nc)[h]
                if head >= 0:
                    bad(head < count, path, f"model {m} hull {h} head out of range")
                elif h == 0:
                    bad(-1 - head < nl, path, f"model {m} hull 0 head names a leaf outside the leaf lump")
        bad(self.leafv.min() >= -1 and (not len(self.vis) or self.leafv.max() < len(self.vis)),
            path, "leaf vis offset outside the vis lump")
        self.path = path
        check_acyclic({i: (int(a), int(b)) for i, (a, b) in enumerate(self.nchild.tolist())},
                      path, "node tree")
        check_acyclic({i: (int(a), int(b)) for i, (a, b) in enumerate(self.cchild.tolist())},
                      path, "clipnode tree")

    def tree(self, hull):
        """(plane index per node, children (n, 2)) of hull 0 nodes or clipnodes."""
        return (self.nplane, self.nchild) if hull == 0 else (self.cplane, self.cchild)

    def pvs_row(self, leaf):
        o = self.leafv[leaf]
        nb = (self.visleafs + 7) >> 3
        if o < 0 or not self.vis:
            return None
        return decompress(self.vis, o, nb, self.path)


def decompress(v, i, nb, path, end=None):
    """Decode one RLE row (w64-format.md VISR): a non-zero byte is 8 literal
    bits, a zero byte plus a count stands for that many zero bytes; the row is
    `nb` bytes long. `end` bounds the bytes the row may use (default: all of v)."""
    end = len(v) if end is None else end
    out = bytearray()
    while len(out) < nb:
        bad(i < end, path, "a vis row runs past its data")
        if v[i]:
            out.append(v[i])
            i += 1
        else:
            bad(i + 1 < end, path, "a vis row runs past its data")
            out += bytes(v[i + 1])
            i += 2
    return bytes(out[:nb])


# --------------------------------------------------------------------------
# .w64 v2
# --------------------------------------------------------------------------

class W64:
    def __init__(self, path: Path):
        self.path = path
        b = read_file(path)
        self.b = b
        bad(len(b) >= 64, path, f"{len(b)} bytes: shorter than the 64-byte header")
        magic, ver, fl, fsz, dofs, dsz, nch, crc = struct.unpack_from(">4sHHIIIII", b, 0)
        bad(magic == b"W64L", path, f"bad magic {magic!r}")
        bad(ver == 3, path, f"version {ver}, want 3")
        bad(fl & 1, path, "W64_HF_COLL not set")
        bad(fsz == len(b), path, f"file_size {fsz} != file length {len(b)}")
        self.idx_stride, self.idx_base = struct.unpack_from(">HH", b, 28)
        bad(self.idx_stride > 0, path, "idx_stride is zero")
        bad(64 <= dofs and 96 <= dsz and dofs + dsz <= len(b), path,
            f"directory {dofs}+{dsz} outside the {len(b)}-byte file")
        d = b[dofs:dofs + dsz]
        bad(zlib.crc32(d) & 0xFFFFFFFF == crc, path, "directory CRC mismatch")
        self.d = d
        (self.nsect, self.nleaf, self.ntex, self.npages, self.nsub, nchunks, self.texp_first,
         self.nvislists) = struct.unpack_from(">8H", d, 0)
        bad(nchunks == nch, path, f"header chunk count {nch} != directory chunk count {nchunks}")
        (cd, self.sect_ofs, self.pp_ofs, self.tex_ofs, self.sub_ofs, self.leaf_ofs,
         self.vp_ofs) = struct.unpack_from(">7I", d, 16)
        (self.coll_first, self.ncoll, self.pinned, self.nleafs_all, self.ents_chunk,
         self.visr_chunk, self.model_ofs, self.leafc_ofs, self.grid_ofs) = \
            struct.unpack_from(">6H3I", d, 44)

        def table(name, ofs, size):
            bad(ofs + size <= dsz, path, f"directory table {name} ({ofs}+{size}) outside the directory")

        table("chunk directory", cd, 16 * nchunks)
        table("sectors", self.sect_ofs, 20 * self.nsect)
        table("page pool", self.pp_ofs, 0)
        table("textures", self.tex_ofs, 12 * self.ntex)
        table("submodels", self.sub_ofs, 16 * self.nsub)
        table("leaf vis", self.leaf_ofs, 4 * self.nleaf)
        table("vis pool", self.vp_ofs, 0)
        table("models", self.model_ofs, 64 * self.nsub)
        table("leaf contents", self.leafc_ofs, self.nleafs_all)
        table("grid header", self.grid_ofs, 32)
        self.chunks = []
        for i in range(nchunks):
            fcc, o, sz, ccrc = struct.unpack_from(">4sIII", d, cd + 16 * i)
            bad(o % 16 == 0 and o >= 64 and o + sz <= len(b), path, f"chunk {i} ({fcc!r} {o}+{sz}) outside the file")
            bad(zlib.crc32(b[o:o + sz]) & 0xFFFFFFFF == ccrc, path, f"chunk {i} CRC mismatch")
            self.chunks.append((fcc, o, sz))
        bad(self.ncoll <= MAX_COLL_CHUNKS, path, f"absurd COLL chunk count {self.ncoll} (limit {MAX_COLL_CHUNKS})")
        bad(self.coll_first + self.ncoll <= nchunks and self.pinned <= self.ncoll, path,
            "COLL chunk range outside the chunk directory")
        for i in range(self.ncoll):
            bad(self.chunks[self.coll_first + i][0] == b"COLL", path, f"COLL {i} fourcc")
        for what, c in (("ENTS", self.ents_chunk), ("VISR", self.visr_chunk)):
            bad(c == NONE or c < nchunks, path, f"{what} chunk index {c} outside the chunk directory")
        for c, (fcc, o, sz) in enumerate(self.chunks):
            if fcc in (b"REND", b"MOVR"):
                bad(sz >= 24, path, f"chunk {c} shorter than its header")
                bad(24 + 24 * struct.unpack_from(">H", b, o)[0] <= sz, path,
                    f"chunk {c} meshlet table outside the chunk")
        self.check_tables()
        self.models = []
        for m in range(self.nsub):
            o = self.model_ofs + 64 * m
            raw = d[o:o + 36]
            heads = struct.unpack_from(">4i", d, o + 36)
            vl, cf, cc = struct.unpack_from(">iHH", d, o + 52)
            self.models.append((raw, heads, vl, cf, cc))
        self.leafc = np.frombuffer(d, ">i1", self.nleafs_all, self.leafc_ofs).astype(np.int64)
        go = self.grid_ofs
        g = struct.unpack_from(">3h3HHHIII", d, go)
        self.gorg = np.array(g[0:3], np.float64)
        self.gdims = g[3:6]
        self.gcell = g[6]
        nstart = g[7]
        bad(min(self.gdims) >= 1 and self.gcell >= 1 and nstart >= 1, path, "empty collision grid")
        n = self.gdims[0] * self.gdims[1] * self.gdims[2]
        bad(n * max(self.ncoll, 1) <= MAX_CELL_BITS, path, f"absurd collision grid ({n} cells)")
        bad(g[8] + 4 * n <= dsz and g[9] + 4 * nstart <= dsz and g[10] < dsz, path,
            "collision grid table outside the directory")
        cells = np.frombuffer(d, ">u2", 2 * n, g[8]).reshape(n, 2).astype(np.int64)
        self.gstart = np.frombuffer(d, ">i4", nstart, g[9]).astype(np.int64)
        bad(cells[:, 1].max() < nstart, path, "grid cell start index out of range")
        npool = (len(d) - g[10]) // 2
        pool = np.frombuffer(d, ">u2", npool, g[10]).astype(np.int64)
        bad(pool[0] == 0, path, "grid list 0 not empty")
        bad(cells[:, 0].max() < npool, path, "grid cell list offset outside the pool")
        self.cell_start = self.gstart[cells[:, 1]]
        self.cell_mat = np.zeros((n, max(self.ncoll, 1)), bool)
        lists = {}
        for ofs in np.unique(cells[:, 0]):
            cnt = pool[ofs]
            lst = pool[ofs + 1:ofs + 1 + cnt]
            bad(len(lst) == cnt, path, "grid list runs past the pool")
            bad((lst >= self.pinned).all() and (lst < self.ncoll).all(), path, "grid list index out of range")
            lists[ofs] = lst
        for c in range(n):
            self.cell_mat[c, lists[cells[c, 0]]] = True
        self.decode_coll()
        size = self.ncoll << LOCAL_BITS
        gs = self.gstart[self.gstart >= 0]
        bad((gs < size).all() and (self.kind[gs] >= 0).all(), path, "grid start node is not a defined node")
        for m, (_raw, heads, _vl, _cf, _cc) in enumerate(self.models):
            for h, head in enumerate(heads):
                bad(head < 0 or (head < size and self.kind[head] >= 0), path,
                    f"model {m} hull {h} head is not a defined node")

    def check_tables(self):
        """Every directory table entry the cook and the seam proofs follow stays
        inside the directory, and indexes the right kind of chunk."""
        path, d = self.path, self.d
        nchunks = len(self.chunks)

        def fcc(c):
            return self.chunks[c][0]

        bad(self.texp_first + self.npages <= nchunks, path, "TEXP page range outside the chunk directory")
        for p in range(self.npages):
            bad(fcc(self.texp_first + p) == b"TEXP", path, f"page {p} is not a TEXP chunk")
        pool_n = 0
        for s in range(self.nsect):
            chunk, first, num = struct.unpack_from(">HHH", d, self.sect_ofs + 20 * s + 12)
            bad(chunk < nchunks and fcc(chunk) == b"REND", path, f"sector {s} is not a REND chunk")
            pool_n = max(pool_n, first + num)
        bad(self.pp_ofs + 2 * pool_n <= len(d), path, "page pool outside the directory")
        for s in range(self.nsect):
            first, num = struct.unpack_from(">HH", d, self.sect_ofs + 20 * s + 14)
            for j in range(first, first + num):
                bad(struct.unpack_from(">H", d, self.pp_ofs + 2 * j)[0] < self.npages, path,
                    f"sector {s} page index out of range")
        for t in range(self.ntex):
            page, nxt, alt = struct.unpack_from(">HxxxxxxHH", d, self.tex_ofs + 12 * t)
            bad(page == NONE or page < self.npages, path, f"texture {t} page out of range")
            bad((nxt == NONE or nxt < self.ntex) and (alt == NONE or alt < self.ntex), path,
                f"texture {t} animation link out of range")
        for m in range(self.nsub):
            kind, chunk = struct.unpack_from(">HH", d, self.sub_ofs + 16 * m)
            bad(kind <= 2, path, f"submodel {m} kind {kind}")
            if kind == 2:
                bad(chunk < nchunks and fcc(chunk) == b"MOVR", path, f"submodel {m} is not a MOVR chunk")
        end = 0
        for leaf in range(self.nleaf):
            index = struct.unpack_from(">I", d, self.leaf_ofs + 4 * leaf)[0]
            pos = self.vp_ofs + 2 * index
            bad(pos + 2 <= len(d), path, f"leaf {leaf} vis list outside the directory")
            count = struct.unpack_from(">H", d, pos)[0]
            bad(pos + 2 + 2 * count <= len(d), path, f"leaf {leaf} vis list outside the directory")
            for i in range(count):
                bad(struct.unpack_from(">H", d, pos + 2 + 2 * i)[0] < self.nsect, path,
                    f"leaf {leaf} vis list names a sector out of range")
            end = max(end, pos + 2 + 2 * count)

    def decode_coll(self):
        """Flat tables indexed by gid: axis (0-2, 3 = plane), normal, dist,
        children (gid >= 0 or leaf encoding < 0)."""
        path = self.path
        size = self.ncoll << LOCAL_BITS
        self.kind = np.full(size, -1, np.int8)
        self.nrm = np.zeros((size, 3), F32)
        self.dist = np.zeros(size, F32)
        self.ch = np.zeros((size, 2), np.int64)
        self.coll_nodes = 0
        for ci in range(self.ncoll):
            _f, o, sz = self.chunks[self.coll_first + ci]
            b = self.b
            bad(sz >= 16, path, f"COLL {ci} shorter than its header")
            nn, nx, ny, nz, npl, nex, _ = struct.unpack_from(">6HI", b, o)
            bad(nx <= ny <= nz <= nn <= (1 << LOCAL_BITS) - 1, path, f"COLL {ci} class bounds")
            po = o + 16
            no = po + 16 * npl
            eo = no + 8 * nn
            bad(eo + 4 * nex <= o + sz, path, f"COLL {ci} tables outside the chunk")
            planes = np.frombuffer(b, ">f4", 4 * npl, po).reshape(npl, 4).astype(F32)
            ext = np.frombuffer(b, ">u4", nex, eo).astype(np.int64)
            bad((ext < size).all(), path, f"COLL {ci} extern gid outside the COLL chunks")
            raw = np.frombuffer(b, [("u", ">u4"), ("c", ">i2", 2)], nn, no)
            base = ci << LOCAL_BITS
            for li in range(nn):
                g = base + li
                u, (c0, c1) = raw[li]["u"], raw[li]["c"]
                k = 0 if li < nx else 1 if li < ny else 2 if li < nz else 3
                self.kind[g] = k
                if k < 3:
                    self.dist[g] = np.frombuffer(struct.pack(">I", u), ">f4")[0]
                    self.nrm[g, k] = 1
                else:
                    bad(u < npl, path, f"COLL {ci} node {li} plane index out of range")
                    self.nrm[g] = planes[u, :3]
                    self.dist[g] = planes[u, 3]
                for s, c in enumerate((int(c0), int(c1))):
                    if c < 0:
                        v = c
                    elif c < nn:
                        v = base + c
                    else:
                        bad(c - nn < nex, path, f"COLL {ci} node {li} extern index out of range")
                        v = int(ext[c - nn])
                    self.ch[g, s] = v
            self.coll_nodes += nn
        defined = np.flatnonzero(self.kind >= 0)
        targets = self.ch[defined].ravel()
        targets = targets[targets >= 0]
        bad((targets < size).all() and (self.kind[targets] >= 0).all(), path, "child gid dangling")
        check_acyclic(dict(zip(defined.tolist(), (tuple(r) for r in self.ch[defined].tolist()))),
                      path, "COLL trees")

    def cell_of(self, P):
        c = np.floor((P - self.gorg) / self.gcell).astype(np.int64)
        ok = np.all((c >= 0) & (c < np.array(self.gdims)), axis=1)
        idx = c[:, 0] + self.gdims[0] * (c[:, 1] + self.gdims[1] * c[:, 2])
        return np.where(ok, idx, -1), c


# --------------------------------------------------------------------------
# point walks (float32, the runtime's arithmetic)
# --------------------------------------------------------------------------

def dot_minus(P, n, d):
    return P[:, 0] * n[:, 0] + P[:, 1] * n[:, 1] + P[:, 2] * n[:, 2] - d


def walk_coll(w: W64, P, head, record=None):
    """Point walk of the COLL encoding from head; returns (leaf child, steps)."""
    cur = np.full(len(P), head, np.int64)
    steps = np.zeros(len(P), np.int64)
    while True:
        a = np.flatnonzero(cur >= 0)
        if not len(a):
            return cur, steps
        g = cur[a]
        k = w.kind[g]
        need((k >= 0).all(), "walk reached an undefined gid")
        p = P[a]
        ax = np.minimum(k, 2)
        d = np.where(k < 3, p[np.arange(len(a)), ax] - w.dist[g], dot_minus(p, w.nrm[g], w.dist[g]))
        if record is not None:
            record.append((a, g))
        cur[a] = np.where(d < 0, w.ch[g, 1], w.ch[g, 0])
        steps[a] += 1


def walk_bsp(bsp: Bsp, hull, P, head):
    planes, ch = bsp.tree(hull)
    cur = np.full(len(P), head, np.int64)
    steps = np.zeros(len(P), np.int64)
    while True:
        a = np.flatnonzero(cur >= 0)
        if not len(a):
            return cur, steps
        n = cur[a]
        pi = planes[n]
        t = bsp.pt[pi]
        p = P[a]
        ax = np.minimum(t, 2)
        d = np.where(t < 3, p[np.arange(len(a)), ax] - bsp.pd[pi],
                     dot_minus(p, bsp.pn[pi], bsp.pd[pi]))
        cur[a] = np.where(d < 0, ch[n, 1], ch[n, 0])
        steps[a] += 1


def contents(hull, leafc, v):
    if hull != 0:
        return v
    idx = -1 - v
    need(np.all(idx < len(leafc)), "walk reached a leaf outside the leaf contents table")
    return leafc[idx]


# --------------------------------------------------------------------------
# Segment trace (docs/specs/trace-and-hulls.md 3), on either encoding
# --------------------------------------------------------------------------

class Tracer:
    """Trace of the segment p1 -> p2 through one hull tree.

    `node_fn(n)` returns (normal, dist, kind, child0, child1) for node n:
    kind 0-2 is an axial node (signed distance `p[kind] - dist`), kind 3 a
    plane node (`normal . p - dist`); child0 is the front side (distance >=
    0), child1 the back; a negative child is a leaf (hull 0: leaf index
    `-1 - c` into `leafc`; hulls 1-3: the contents value itself). Sub-segments
    are processed in travel order with an explicit stack; `visited` collects
    every node read, by the split walk and by the point-contents walks.

    trace() returns a dict: fraction, endpos, plane ((normal), dist) or None,
    allsolid, startsolid, inopen, inwater.
    """

    def __init__(self, hull, leafc, node_fn):
        self.hull = hull
        self.leafc = leafc
        self.node = node_fn
        self.visited = []

    def leaf_contents(self, c):
        if self.hull != 0:
            return c
        need(-1 - c < len(self.leafc), "trace reached a leaf outside the leaf contents table")
        return int(self.leafc[-1 - c])

    @staticmethod
    def side_distance(normal, dist, kind, p):
        if kind < 3:
            return p[kind] - dist
        return p[0] * normal[0] + p[1] * normal[1] + p[2] * normal[2] - dist

    def point_contents(self, node, p):
        """Contents of the hull leaf holding p, walking down from `node`."""
        while node >= 0:
            normal, dist, kind, c0, c1 = self.node(node)
            self.visited.append(node)
            node = c1 if self.side_distance(normal, dist, kind, p) < 0 else c0
        return self.leaf_contents(node)

    def trace(self, head, p1, p2):
        self.visited = []
        p1, p2 = tuple(p1), tuple(p2)
        delta = tuple(p2[i] - p1[i] for i in range(3))
        res = {"fraction": 1.0, "endpos": p2, "plane": None, "allsolid": True,
               "startsolid": False, "inopen": False, "inwater": False}
        # tasks, popped last-in first-out:
        #   ("seg", node, f1, f2, a, b): trace a -> b (whole-segment fractions f1 -> f2) below node
        #   ("far", ...): the near side of a split finished without an impact
        work = [("seg", head, 0.0, 1.0, p1, p2)]
        while work:
            task = work.pop()
            if task[0] == "far":
                (_, far, plane, front_first, f1, f2, a, b, frac, midf, mid) = task
                if self.point_contents(far, mid) != CONTENTS_SOLID:
                    work.append(("seg", far, midf, f2, mid, b))
                    continue
                if res["allsolid"]:
                    break                       # never left the solid: no impact, fraction 1
                # impact on this split plane, oriented towards the side we came from
                normal, dist = plane
                if not front_first:
                    normal, dist = tuple(-x for x in normal), -dist
                res["plane"] = (normal, dist)
                # the stand-off point can still be solid at acute corners: back off in
                # steps of 0.1 of the sub-segment; a step that would pass the start
                # keeps the last point tried
                while self.point_contents(head, mid) == CONTENTS_SOLID:
                    frac -= BACKOFF_STEP
                    if frac < 0:
                        break
                    midf = f1 + (f2 - f1) * frac
                    mid = tuple(a[i] + frac * (b[i] - a[i]) for i in range(3))
                res["fraction"] = midf
                res["endpos"] = tuple(p1[i] + midf * delta[i] for i in range(3))
                break
            _, node, f1, f2, a, b = task
            while True:
                if node < 0:
                    c = self.leaf_contents(node)
                    if c == CONTENTS_SOLID:
                        res["startsolid"] = True
                    else:
                        res["allsolid"] = False
                        if c == CONTENTS_EMPTY:
                            res["inopen"] = True
                        else:
                            # every other non-solid leaf (water, slime, lava, sky,
                            # currents, translucent) counts as "in a liquid" for the flag
                            res["inwater"] = True
                    break
                normal, dist, kind, c0, c1 = self.node(node)
                self.visited.append(node)
                ta = self.side_distance(normal, dist, kind, a)
                tb = self.side_distance(normal, dist, kind, b)
                if ta >= 0 and tb >= 0:
                    node = c0
                    continue
                if ta < 0 and tb < 0:
                    node = c1
                    continue
                # the sub-segment crosses the plane: split it so the stand-off
                # point lies DIST_EPSILON on the near side of the plane
                front_first = ta >= 0
                frac = ((ta - DIST_EPSILON) if front_first else (ta + DIST_EPSILON)) / (ta - tb)
                frac = min(1.0, max(0.0, frac))
                midf = f1 + (f2 - f1) * frac
                mid = tuple(a[i] + frac * (b[i] - a[i]) for i in range(3))
                near, far = (c0, c1) if front_first else (c1, c0)
                work.append(("far", far, (normal, dist), front_first, f1, f2, a, b, frac, midf, mid))
                node, f2, b = near, midf, mid
        return res


def coll_node_fn(w: W64):
    def fn(g):
        k = int(w.kind[g])
        need(k >= 0, "trace reached an undefined gid")
        return (tuple(float(x) for x in w.nrm[g]), float(w.dist[g]), k,
                int(w.ch[g, 0]), int(w.ch[g, 1]))
    return fn


def bsp_node_fn(bsp: Bsp, hull):
    planes, ch = bsp.tree(hull)

    def fn(i):
        pi = planes[i]
        t = int(bsp.pt[pi])
        return (tuple(float(x) for x in bsp.pn[pi]), float(bsp.pd[pi]), t if t < 3 else 3,
                int(ch[i, 0]), int(ch[i, 1]))
    return fn


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------

def check_render(w: W64):
    b, d = w.b, w.d
    nm = 0
    for c, (fcc, o, sz) in enumerate(w.chunks):
        if fcc not in (b"REND", b"MOVR"):
            continue
        nmesh, nsty, vofs0, iofs0, sofs0, pofs = struct.unpack_from(">HHIIII", b, o)
        pd = struct.unpack_from(">3B", b, o + 20)
        if fcc == b"REND":
            need(pofs and pofs % 8 == 0, f"chunk {c} probe_ofs")
            need(pofs + 6 + pd[0] * pd[1] * pd[2] <= sz and min(pd) >= 1, f"chunk {c} probes")
        else:
            need(pofs == 0, f"MOVR {c} has probes")
        for i in range(nmesh):
            vert, idx, tex, _sty = struct.unpack_from(">IIHH", b, o + 24 + 24 * i + 12)
            nv, vo, ni, io = vert & 255, vert >> 8, idx & 255, idx >> 8
            nm += 1
            need(3 <= nv <= 64 and 3 <= ni <= 104 and vo % 16 == 0 and io % 8 == 0
                 and vo + ((nv + 1) // 2) * 32 <= sz and io + 2 * ni <= sz and tex < w.ntex,
                 f"chunk {c} meshlet {i}")
            ix = struct.unpack_from(">%dH" % ni, b, o + io)
            for k, e in enumerate(ix):
                v = (e & 0x3FFF) - w.idx_base
                need(v % w.idx_stride == 0 and v // w.idx_stride < nv, f"chunk {c} idx")
                need((k == ni - 1) == bool(e & 0x4000), f"chunk {c} last bit")
            need(ix[0] & 0x8000, f"chunk {c} restart")
            page = struct.unpack_from(">H", d, w.tex_ofs + 12 * tex)[0]
            need(page == NONE or page < w.npages, f"chunk {c} page")
    return nm


def check_models(w: W64, bsp: Bsp):
    need(w.nsub == len(bsp.mraw), "model count")
    need(w.nleafs_all == len(bsp.leafc), "leaf count")
    need((w.leafc == bsp.leafc).all(), "leaf contents")
    for m, (raw, heads, vl, cf, cc) in enumerate(w.models):
        need(raw == bsp.mraw[m].astype(">f4").tobytes(), f"model {m} floats")
        need(vl == bsp.mvis[m], f"model {m} visleafs")
        need(cf + cc <= w.ncoll, f"model {m} coll range")
        for h in range(4):
            if bsp.mhead[m, h] < 0:
                need(heads[h] == bsp.mhead[m, h], f"model {m} hull {h} leaf head")
            else:
                ci = heads[h] >> LOCAL_BITS
                need(heads[h] >= 0 and (cf <= ci < cf + cc or (h == 0 and ci < w.pinned)),
                     f"model {m} hull {h} head outside its chunks")
    need(w.models[0][3] == 0 and w.models[0][4] >= w.pinned, "world chunk range")


def check_points(w: W64, bsp: Bsp, rng, npts):
    lo = w.gorg
    hi = w.gorg + np.array(w.gdims) * w.gcell
    P = rng.uniform(lo, hi, (npts, 3))
    q = npts // 4
    P[:q] = np.round(P[:q] * 16) / 16          # 1/16 u lattice: plane ties
    P = np.clip(P, lo, hi - 1e-3).astype(F32)
    cell, _ = w.cell_of(P.astype(np.float64))
    need((cell >= 0).all(), "sample outside grid")
    out = {"points": npts}
    for h in range(4):
        rec = []
        head = w.models[0][1][h]
        cv, cs = walk_coll(w, P, head, rec)
        bv, bs = walk_bsp(bsp, h, P, int(bsp.mhead[0, h]))
        need((contents(h, w.leafc, cv) == contents(h, bsp.leafc, bv)).all(),
             f"world hull {h} contents mismatch at {int(np.sum(cv != bv))} points")
        need((cs == bs).all(), f"world hull {h} path length mismatch")
        if h == 0:
            need((cv == bv).all(), "world hull 0 leaf mismatch")
        for a, g in rec:
            ch = g >> LOCAL_BITS
            if h == 0:
                need((ch < w.pinned).all(), "world hull 0 walk left the pinned chunks")
            else:
                ok = (ch < w.pinned) | w.cell_mat[cell[a], ch]
                need(ok.all(), f"world hull {h}: {int((~ok).sum())} visits outside the cell lists")
        out[f"hull{h}_visits"] = int(sum(len(a) for a, _ in rec))
    # grid start -> leaf
    st = w.cell_start[cell]
    sv = st.copy()
    inner = np.flatnonzero(st >= 0)
    if len(inner):
        sv[inner] = walk_from(w, P[inner], st[inner])
    rv, _ = walk_coll(w, P, w.models[0][1][0])
    need((sv == rv).all(), f"grid start leaf mismatch at {int((sv != rv).sum())} points")
    out["start_inner"] = int(len(inner))
    return out


def check_grid_exact(w: W64, margin=0.01):
    """Exact per-cell proof on the decoded COLL trees: (a) the start node of
    every cell lies on the root path that the whole cell (box) follows,
    i.e. every ancestor has the cell >= margin on one side; (b) every
    world hull 1-3 node a box-pruned walk of the cell reaches (a child is
    skipped only when the cell is >= margin on the other side) lies in a
    pinned chunk or in the cell's list."""
    dx, dy, dz = w.gdims
    n = dx * dy * dz
    ix = np.arange(n)
    c = np.stack([ix % dx, (ix // dx) % dy, ix // (dx * dy)], 1)
    Lo = w.gorg + c * w.gcell
    Hi = Lo + w.gcell

    def rng_(g, idx):
        k = w.kind[g]
        if k < 3:
            return Lo[idx, k] - float(w.dist[g]), Hi[idx, k] - float(w.dist[g])
        nv = w.nrm[g].astype(np.float64)
        a, b = Lo[idx] * nv, Hi[idx] * nv
        return np.minimum(a, b).sum(1) - float(w.dist[g]), np.maximum(a, b).sum(1) - float(w.dist[g])

    # (a) start on the cell's root path
    ok = np.zeros(n, bool)
    root = w.models[0][1][0]
    if root < 0:
        ok[:] = w.cell_start == root
    st = [(root, ix)] if root >= 0 else []
    while st:
        g, idx = st.pop()
        ok[idx[w.cell_start[idx] == g]] = True
        lo, hi = rng_(g, idx)
        for child, sel in zip(w.ch[g], (lo >= margin, hi <= -margin)):
            s = idx[sel]
            if not len(s):
                continue
            if child < 0:
                ok[s[w.cell_start[s] == child]] = True
            else:
                st.append((int(child), s))
    need(ok.all(), f"grid start not on the cell path for {int((~ok).sum())} cells")
    # (b) list completeness
    visits = 0
    for h in (1, 2, 3):
        root = w.models[0][1][h]
        st = [(root, ix)] if root >= 0 else []
        while st:
            g, idx = st.pop()
            ch = g >> LOCAL_BITS
            visits += len(idx)
            need(ch < w.pinned or w.cell_mat[idx, ch].all(),
                 f"hull {h} node {g} reachable from cells missing chunk {ch}")
            lo, hi = rng_(g, idx)
            for child, sel in zip(w.ch[g], (hi > -margin, lo < margin)):
                s = idx[sel]
                if child >= 0 and len(s):
                    st.append((int(child), s))
    return n, visits


def walk_from(w: W64, P, starts):
    cur = starts.copy()
    while True:
        a = np.flatnonzero(cur >= 0)
        if not len(a):
            return cur
        g = cur[a]
        k = w.kind[g]
        need((k >= 0).all(), "walk reached an undefined gid")
        p = P[a]
        ax = np.minimum(k, 2)
        d = np.where(k < 3, p[np.arange(len(a)), ax] - w.dist[g], dot_minus(p, w.nrm[g], w.dist[g]))
        cur[a] = np.where(d < 0, w.ch[g, 1], w.ch[g, 0])


def check_submodels(w: W64, bsp: Bsp, rng, npts, nmod):
    ms = list(range(1, w.nsub))
    if not ms:
        return 0
    sizes = [w.models[m][4] for m in ms]
    pick = sorted(ms, key=lambda m: -sizes[m - 1])[:nmod // 2]
    rest = [m for m in ms if m not in pick]
    pick += list(rng.choice(rest, min(len(rest), nmod - len(pick)), replace=False)) if rest else []
    for m in pick:
        m = int(m)
        f = bsp.mraw[m]
        lo = f[0:3].astype(np.float64) - 40
        hi = f[3:6].astype(np.float64) + 40
        P = rng.uniform(lo, hi, (npts, 3))
        P[:npts // 4] = np.round(P[:npts // 4] * 16) / 16
        P = P.astype(F32)
        _raw, heads, _vl, cf, cc = w.models[m]
        for h in range(4):
            if bsp.mhead[m, h] < 0:
                continue
            rec = []
            cv, cs = walk_coll(w, P, heads[h], rec)
            bv, bs = walk_bsp(bsp, h, P, int(bsp.mhead[m, h]))
            need((contents(h, w.leafc, cv) == contents(h, bsp.leafc, bv)).all(),
                 f"model {m} hull {h} contents mismatch")
            need((cs == bs).all(), f"model {m} hull {h} path length mismatch")
            for _a, g in rec:
                ch = g >> LOCAL_BITS
                ok = (ch >= cf) & (ch < cf + cc)
                if h == 0:
                    ok |= ch < w.pinned
                need(ok.all(), f"model {m} hull {h} walk left its chunks")
    return len(pick)


def check_segments(w: W64, bsp: Bsp, rng, nseg):
    lo = w.gorg
    hi = w.gorg + np.array(w.gdims) * w.gcell
    dx, dy, dz = w.gdims
    hits = 0
    for s in range(nseg):
        p1 = rng.uniform(lo, hi - 1e-3)
        dvec = rng.normal(size=3)
        dvec /= np.linalg.norm(dvec)
        p2 = p1 + dvec * rng.uniform(1, 192)
        if s % 4 == 0:
            p1, p2 = np.round(p1), np.round(p2)
        p1 = [float(F32(x)) for x in p1]
        p2 = [float(F32(x)) for x in p2]
        c = np.floor((np.array(p1) - w.gorg) / w.gcell).astype(int)
        if not ((c >= 0) & (c < np.array(w.gdims))).all():
            continue
        nb = np.zeros(w.ncoll, bool)
        for ox in (-1, 0, 1):
            for oy in (-1, 0, 1):
                for oz in (-1, 0, 1):
                    x, y, z = c[0] + ox, c[1] + oy, c[2] + oz
                    if 0 <= x < dx and 0 <= y < dy and 0 <= z < dz:
                        nb |= w.cell_mat[x + dx * (y + dy * z)]
        nb[:w.pinned] = True
        for h in range(4):
            tc = Tracer(h, w.leafc, coll_node_fn(w))
            rc = tc.trace(w.models[0][1][h], p1, p2)
            tb = Tracer(h, bsp.leafc, bsp_node_fn(bsp, h))
            rb = tb.trace(int(bsp.mhead[0, h]), p1, p2)
            need(rc == rb, f"segment {s} hull {h}: {rc} != {rb}")
            need(len(tc.visited) == len(tb.visited), f"segment {s} hull {h} path")
            chs = np.array(tc.visited, np.int64) >> LOCAL_BITS
            need(nb[chs].all(), f"segment {s} hull {h} visits a chunk outside the neighbourhood")
            hits += rc["fraction"] < 1.0
    return hits


def check_visr(w: W64, bsp: Bsp):
    if w.visr_chunk == NONE:
        need(not bsp.vis, "VISR missing")
        return 0
    _f, o, sz = w.chunks[w.visr_chunk]
    need(_f == b"VISR", "VISR fourcc")
    nv = bsp.visleafs
    need(4 * (nv + 1) <= sz, "VISR offset table outside the chunk")
    ofs = struct.unpack_from(">%dI" % (nv + 1), w.b, o)
    nb = (nv + 7) >> 3
    for L in range(1, nv + 1):
        a, e = ofs[L - 1], ofs[L]
        need(a <= e <= sz, f"VISR row {L} bounds")
        want = bsp.pvs_row(L)
        if a == e:
            need(want is None, f"VISR row {L} empty")
        else:
            need(decompress(w.b, o + a, nb, w.path, o + e) == want, f"VISR row {L}")
    return nv


def check_chunk_kinds(w: W64):
    """Every chunk is a known kind; the old companion kinds appear at most once
    and are skipped; a DECL chunk appears at most once and is a valid decal
    sheet. Returns the companion kinds present."""
    seen = []
    decl = 0
    for i, (fcc, o, sz) in enumerate(w.chunks):
        need(fcc in KNOWN_CHUNKS or fcc in SKIPPED_CHUNKS or fcc == decals.DECL,
             f"chunk {i} unknown fourcc {fcc!r}")
        if fcc == decals.DECL:
            decl += 1
            need(decl == 1, "duplicate DECL chunk")
            try:
                decals.sheet_names(w.b[o:o + sz], f"chunk {i} DECL")
            except decals.DecalError as e:
                raise Fail(str(e)) from None
        if fcc in SKIPPED_CHUNKS:
            need(fcc not in seen, f"duplicate {fcc.decode()} chunk")
            seen.append(fcc)
    return [f.decode() for f in seen]


def count_arg(text):
    v = int(text)
    if v < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return v


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="w64check", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("w64", type=Path)
    ap.add_argument("bsp", type=Path)
    ap.add_argument("--hle", type=Path)
    ap.add_argument("--points", type=count_arg, default=20000)
    ap.add_argument("--segments", type=count_arg, default=2000)
    ap.add_argument("--submodels", type=count_arg, default=8)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args(argv)
    rng = np.random.default_rng(a.seed)
    try:
        w = W64(a.w64)
        bsp = Bsp(a.bsp)
        blob = read_file(a.hle) if a.hle else None
        nm = check_render(w)
        check_models(w, bsp)
        pts = check_points(w, bsp, rng, a.points)
        ncell, cvis = check_grid_exact(w)
        nsub = check_submodels(w, bsp, rng, a.points // 4, a.submodels)
        hits = check_segments(w, bsp, rng, a.segments)
        nv = check_visr(w, bsp)
        if blob is not None:
            need(w.ents_chunk != NONE, "ENTS missing")
            _f, o, sz = w.chunks[w.ents_chunk]
            need(_f == b"ENTS", "ENTS fourcc")
            need(len(blob) <= sz and w.b[o:o + len(blob)] == blob and not any(w.b[o + len(blob):o + sz]),
                 "ENTS != hlecook blob")
        comp = check_chunk_kinds(w)
    except BadInput as e:
        print(f"w64check: error: {e}", file=sys.stderr)
        return 1
    except Fail as e:
        print(f"w64check: {a.w64.name}: FAIL: {e}")
        return 1
    print(f"w64check: {a.w64.name}: OK meshlets={nm} coll_chunks={w.ncoll} "
          f"coll_nodes={w.coll_nodes} points={pts['points']} "
          f"visits h0/h1/h2/h3={pts['hull0_visits']}/{pts['hull1_visits']}/"
          f"{pts['hull2_visits']}/{pts['hull3_visits']} grid_start_inner={pts['start_inner']} "
          f"cells_exact={ncell} (box visits {cvis}) "
          f"submodels={nsub}x4 hulls x{a.points // 4} segments={a.segments}x4 hulls "
          f"(hit {hits}) visr_rows={nv} ents={'yes' if a.hle else 'unchecked'} "
          f"skipped_chunks={','.join(comp) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
