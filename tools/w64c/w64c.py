#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""w64c: cook a BSP30 map into a layer package (.w64).

Format: docs/formats/w64-format.md (version 2). Behaviour of the vertex
light, light probes and texture animation: docs/specs/world-surfaces.md.
Imported from the quakegeneric tree's tools/w64c/w64c.py (provenance-audit.md
entry tools/w64c/w64c.py); the audit-flagged ranges are rewritten from the
hl64 specs and the HLSDK texture-type reader lives in tools/hlsdk/.

Input : the map's BSP30 (cook input only) + the map's .wci4 CI4 texture pack
        (tools.cook.bake_world_tex; its textures become the TEXP pages) + the
        map's HLE1 entity blob (hlecook, docs/formats/hle-format-v1.md).
Output: maps/<map>.w64: header + layer directory + chunks:
  REND  one per sector: tiny3d-native meshlets (T3DVertPacked, baked style-0
        vertex light, pre-encoded tiny3d strip indices, per-style intensity
        blocks for lightstyled meshlets), static submodels folded in, and a
        64 u light-probe grid (world-surfaces.md 3)
  MOVR  one per drawn non-static submodel (doors, buttons, trains, ...),
        same layout as REND in the submodel's local space
  TEXP  one per texture page (CI4 + TLUT16)
  COLL  hull trees of every BSP model (hull 0 nodes + hull 1-3 clipnodes),
        each node stored once, ~2 KB chunks; world hull 0 pinned
  ENTS  the HLE1 entity blob
  VISR  the BSP's compressed PVS rows
  DECL  (--decl) the map's placed-decal sheet (tools/cook/decals.py), found
        by fourcc
The directory adds the model table, leaf contents and the collision grid
(256 u cells: hull 1-3 chunk lists + hull-0 point->leaf start nodes). The
old zone-system companions (ZONE / DRYS / STRM) are not emitted.

Vertex attributes are computed in float32 as w64-format.md 'Vertices' and
world-surfaces.md 1-2 define them.

Budget (w64-format.md 'Budget proof'): for every visleaf the render and
texture open sets (REND chunks of the leaf's visible sectors + MOVR chunks of
the movers it can see, the TEXP pages they use) and for every grid cell the
collision set (pinned + 3x3x3 neighbourhood lists + nearby submodels) must
fit the pools in include/hl64/limits.h; the cook fails otherwise.

Every input is untrusted: a malformed BSP, WCI4, HLE or materials file stops
the cook with `w64c: error: <file>: <what>` and exit status 1.

Usage (from the repository root):
  python3 -m tools.w64c.w64c <map.bsp> <map.wci4> <out.w64> --hle <map.hle>
          [--decl map.decl] [--materials sound/materials.txt] [--json report.json]
          [--no-budget]
"""
from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path

import numpy as np

from tools.cook import decals
from tools.hlsdk.materials import Materials, MaterialsError

ROOT = Path(__file__).resolve().parents[2]
LIMITS_H = ROOT / "include" / "hl64" / "limits.h"

F32 = np.float32

W64_MAGIC = b"W64L"
W64_VERSION = 2
W64_HF_COLL = 1

# tiny3d pin: strip indices are cooked as DMEM pointers
# (idx * VERT_OUTPUT_SIZE + (RSP_T3D_VERT_BUFFER & 0xFFFF)), t3d.c
# t3d_indexbuffer_convert. The runtime self-checks these at boot.
T3D_VERT_OUTPUT_SIZE = 36
T3D_VERT_BUFFER = 0x0408
IDX_RESTART = 0x8000
IDX_LAST = 0x4000

MESHLET_MAX_VERTS = 64     # 64 verts loaded leaves 6 slots = 104 indices of DMEM
MESHLET_MAX_IDX = 104
POLY_MAX_VERTS = 62        # bigger faces are split into fans of <= this

# Sector build (w64-format.md 'Sectors (cook)', build_sectors): units of CELL-sized
# cells x marking-leaf set, merged by least PVS overfetch into sectors of
# SECTOR_MIN..SECTOR_MAX bytes; neighbours = bboxes within SECTOR_GAP.
CELL = 256
SECTOR_MIN = 2 * 1024
SECTOR_MAX = 16 * 1024
SECTOR_GAP = 64

# Vertex light constants (world-surfaces.md 2): style 0 is pattern "m" =
# 12 * 22 = 264 (lightstyles.md 1), light scale 2.5 (`lightscale_x100` 250).
STYLE0_VALUE = 264
LIGHT_SCALE = F32(2.5)

# Faces never drawn (compile-tool textures that can survive in a BSP).
SKIP_TEX = {"clip", "origin", "null", "bevel", "skip", "hint", "aaatrigger"}

# Submodels folded into the static world: never move, never toggle (no
# targetname means nothing can fire them), drawn at identity.
FOLD_CLASSES = {"func_wall", "func_illusionary"}

CHUNK_REND, CHUNK_MOVR, CHUNK_TEXP = b"REND", b"MOVR", b"TEXP"
CHUNK_COLL, CHUNK_ENTS, CHUNK_VISR, CHUNK_DECL = b"COLL", b"ENTS", b"VISR", decals.DECL
NO_CHUNK = 0xFFFF
W64_MAT_MIXED = 15        # material code of a node with mixed faces

# Collision (COLL): chunk byte cap (~2 KB is the measured best trade-off
# between per-cell residency and header/extern overhead), node id split, grid
# cell, conservative cell/plane margin. The margin is above the trace
# stand-off DIST_EPSILON (1/32 u, trace-and-hulls.md): a trace evaluates the
# far child at a midpoint up to that far on the near side of the plane.
COLL_CAP = 2048
GID_LOCAL_BITS = 12
COLL_MAX_NODES = (1 << GID_LOCAL_BITS) - 1
GRID_CELL = 256
CELL_EPS = 0.05
SUBMODEL_PAD = 64          # submodel placement box margin (collision budget)

# Light probes (world-surfaces.md 3): 64 u lattice, 2048 u downward segment.
PROBE_STEP = 64
LIGHT_TRACE = 2048
# Lightstyle patterns at map start (world-surfaces.md 3, lightstyles.md 1):
# a style's value is its pattern's first letter, (letter - 'a') * 22. The
# standard styles worldspawn installs are GoldSrc facts (HLSDK
# dlls/world.cpp); only the first letter of each pattern matters here.
WORLD_STYLES = {0: "m", 1: "m", 2: "a", 3: "m", 4: "m", 5: "j", 6: "n",
                7: "m", 8: "m", 9: "a", 10: "m", 11: "a", 63: "a"}
STYLE_UNSET = 256          # a style nobody set
SWITCH_STYLE_FIRST = 32    # switchable light styles 32..

SUB_NONE, SUB_FOLDED, SUB_MOVR = 0, 1, 2

TEXF_FENCE, TEXF_SKY, TEXF_ANIM, TEXF_MISSING = 1, 2, 4, 8


class InputError(ValueError):
    """Malformed input file; the message starts with the file name."""


class CookError(Exception):
    """The map does not fit the package format's limits."""


def die(msg: str) -> None:
    raise CookError(msg)


def error(msg: str) -> None:
    print(f"w64c: error: {msg}", file=sys.stderr)
    sys.exit(1)


def align(v: int, a: int) -> int:
    return (v + a - 1) // a * a


POOL_KEYS = ("HL64_POOL_RENDER_KB", "HL64_POOL_TEXTURE_KB", "HL64_POOL_COLLISION_KB")


def read_limits() -> dict:
    """Pool sizes shared with the runtime (include/hl64/limits.h), in bytes."""
    try:
        txt = LIMITS_H.read_text()
    except OSError as e:
        raise InputError(f"{LIMITS_H}: {e.strerror}") from None
    want = {}
    for k in POOL_KEYS:
        m = re.search(r"#define\s+%s\s+(\d+)" % k, txt)
        if not m:
            raise InputError(f"{LIMITS_H}: {k} missing")
        want[k] = int(m.group(1)) * 1024
    return want


# --------------------------------------------------------------------------
# BSP30
# --------------------------------------------------------------------------

BSP_VERSION = 30
BSP_NLUMPS = 15
BSP_HDR = 4 + 8 * BSP_NLUMPS
(L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES, L_TEXINFO,
 L_FACES, L_LIGHTING, L_CLIPNODES, L_LEAFS, L_MARKSURFACES, L_EDGES, L_SURFEDGES,
 L_MODELS) = range(BSP_NLUMPS)
BSP_MAX_MIPTEX = 4096      # far above any map; a larger count is corrupt
MIPTEX_NAME = 16
CONTENTS_EMPTY = -1


class Bsp:
    """BSP30 map, every lump range, count and index checked before use
    (architecture.md 18.2 'Hostile input'); InputError on the first fault."""

    def __init__(self, path: Path):
        self.path = path
        try:
            d = path.read_bytes()
        except OSError as e:
            raise InputError(f"{path}: {e.strerror}") from None

        def fail(what):
            raise InputError(f"{path}: {what}")

        if len(d) < BSP_HDR:
            fail(f"truncated header ({len(d)} B < {BSP_HDR})")
        ver = struct.unpack_from("<i", d, 0)[0]
        if ver != BSP_VERSION:
            fail(f"version {ver}, not BSP{BSP_VERSION}")
        lumps = [struct.unpack_from("<ii", d, 4 + 8 * i) for i in range(BSP_NLUMPS)]
        for i, (o, n) in enumerate(lumps):
            if o < 0 or n < 0 or o + n > len(d):
                fail(f"lump {i} (offset {o}, size {n}) outside the file ({len(d)} B)")

        def lump(i):
            o, n = lumps[i]
            return d[o:o + n]

        def arr(i, fmt, what):
            b = lump(i)
            sz = struct.calcsize(fmt)
            if len(b) % sz:
                fail(f"{what} lump size {len(b)} not a multiple of {sz}")
            return [t for t in struct.iter_unpack(fmt, b)]

        self.entities = lump(L_ENTITIES).split(b"\0")[0].decode("latin1")
        self.planes = arr(L_PLANES, "<4fi", "plane")
        self.verts = [tuple(F32(c) for c in v) for v in arr(L_VERTEXES, "<3f", "vertex")]
        self.vis = lump(L_VISIBILITY)
        self.nodes = arr(L_NODES, "<i2h6h2H", "node")
        self.texinfo = arr(L_TEXINFO, "<8fii", "texinfo")
        self.faces = arr(L_FACES, "<HHiHH4Bi", "face")
        self.light = lump(L_LIGHTING)
        self.leafs = arr(L_LEAFS, "<ii6h2H4B", "leaf")
        mb = lump(L_MARKSURFACES)
        if len(mb) % 2:
            fail(f"marksurface lump size {len(mb)} not a multiple of 2")
        self.mark = struct.unpack_from("<%dH" % (len(mb) // 2), mb, 0)
        self.edges = arr(L_EDGES, "<2H", "edge")
        sb = lump(L_SURFEDGES)
        if len(sb) % 4:
            fail(f"surfedge lump size {len(sb)} not a multiple of 4")
        self.surfedges = struct.unpack_from("<%di" % (len(sb) // 4), sb, 0)
        self.models = arr(L_MODELS, "<9f7i", "model")
        self.clip = arr(L_CLIPNODES, "<i2h", "clipnode")
        tx = lump(L_TEXTURES)
        self.texnames = []
        if tx:
            if len(tx) < 4:
                fail("texture lump truncated")
            n = struct.unpack_from("<i", tx, 0)[0]
            if not 0 <= n <= BSP_MAX_MIPTEX or 4 + 4 * n > len(tx):
                fail(f"texture count {n} does not fit the texture lump ({len(tx)} B)")
            for i, o in enumerate(struct.unpack_from("<%di" % n, tx, 4)):
                if o < 0:
                    self.texnames.append("")
                    continue
                if o + MIPTEX_NAME > len(tx):
                    fail(f"miptex {i} offset {o} outside the texture lump ({len(tx)} B)")
                self.texnames.append(tx[o:o + MIPTEX_NAME].split(b"\0")[0].decode("latin1"))
        self._check(fail)
        self.visleafs = self.models[0][9 + 3 + 1]  # dmodel_t visleafs
        lb = np.frombuffer(self.light, dtype=np.uint8)
        n3 = len(lb) // 3
        # BSP30 RGB luxels -> mono = max(R, G, B) (world-surfaces.md 1)
        self.mono = lb[:n3 * 3].reshape(n3, 3).max(axis=1) if n3 else lb[:0]

    def _check(self, fail):
        """Every index the cook follows stays inside its table; node and
        clipnode children point forward (acyclic trees, bounded walks)."""
        npl, nn, nf, nl = len(self.planes), len(self.nodes), len(self.faces), len(self.leafs)
        nc, nti, ne, nse = len(self.clip), len(self.texinfo), len(self.edges), len(self.surfedges)
        nv, nmt = len(self.verts), len(self.texnames)
        if not self.models:
            fail("no models")
        if not nl:
            fail("no leafs")
        for i, p in enumerate(self.planes):
            if not all(math.isfinite(c) for c in p[:4]):
                fail(f"plane {i}: non-finite value")
        for i, v in enumerate(self.verts):
            if not all(-32768.0 <= float(c) < 32768.0 for c in v):
                fail(f"vertex {i}: coordinate outside the s16 range or non-finite")
        for i, t in enumerate(self.texinfo):
            if not all(math.isfinite(c) for c in t[:8]):
                fail(f"texinfo {i}: non-finite vector")
            if not 0 <= t[8] < nmt:
                fail(f"texinfo {i}: miptex {t[8]} outside 0..{nmt - 1}")
        for i, nd in enumerate(self.nodes):
            if not 0 <= nd[0] < npl:
                fail(f"node {i}: plane {nd[0]} outside 0..{npl - 1}")
            for c in nd[1:3]:
                if c >= 0 and not i < c < nn:
                    fail(f"node {i}: child node {c} not in {i + 1}..{nn - 1}")
                if c < 0 and -1 - c >= nl:
                    fail(f"node {i}: child leaf {-1 - c} outside 0..{nl - 1}")
            if nd[9] + nd[10] > nf:
                fail(f"node {i}: faces {nd[9]}+{nd[10]} outside 0..{nf}")
        for i, cn in enumerate(self.clip):
            if not 0 <= cn[0] < npl:
                fail(f"clipnode {i}: plane {cn[0]} outside 0..{npl - 1}")
            for c in cn[1:3]:
                if c >= 0 and not i < c < nc:
                    fail(f"clipnode {i}: child {c} not in {i + 1}..{nc - 1}")
        for i, e in enumerate(self.edges):
            if e[0] >= nv or e[1] >= nv:
                fail(f"edge {i}: vertex {max(e)} outside 0..{nv - 1}")
        for i, e in enumerate(self.surfedges):
            if not -ne < e < ne:
                fail(f"surfedge {i}: edge {e} outside -{ne - 1}..{ne - 1}")
        nlight = len(self.light)
        for i, fc in enumerate(self.faces):
            if not 0 <= fc[0] < npl:
                fail(f"face {i}: plane {fc[0]} outside 0..{npl - 1}")
            if fc[3] < 3 or fc[2] < 0 or fc[2] + fc[3] > nse:
                fail(f"face {i}: surfedges {fc[2]}+{fc[3]} outside 0..{nse} or < 3")
            if fc[4] >= nti:
                fail(f"face {i}: texinfo {fc[4]} outside 0..{nti - 1}")
            if fc[9] >= 0 and fc[9] >= nlight:
                fail(f"face {i}: lighting offset {fc[9]} outside the lighting lump ({nlight} B)")
        nmark, nvis = len(self.mark), len(self.vis)
        for i, lf in enumerate(self.leafs):
            if not -128 <= lf[0] <= 127:
                fail(f"leaf {i}: contents {lf[0]} outside -128..127")
            if lf[8] + lf[9] > nmark:
                fail(f"leaf {i}: marksurfaces {lf[8]}+{lf[9]} outside 0..{nmark}")
            if lf[1] >= 0 and lf[1] >= nvis and nvis:
                fail(f"leaf {i}: vis offset {lf[1]} outside the vis lump ({nvis} B)")
        for i, m in enumerate(self.mark):
            if m >= nf:
                fail(f"marksurface {i}: face {m} outside 0..{nf - 1}")
        for i, md in enumerate(self.models):
            if not all(math.isfinite(c) for c in md[:9]):
                fail(f"model {i}: non-finite bounds")
            h0 = md[9]
            if (h0 >= 0 and h0 >= nn) or (h0 < 0 and -1 - h0 >= nl):
                fail(f"model {i}: hull 0 head {h0} outside the node / leaf tables")
            for h in md[10:13]:
                if h > nc:
                    fail(f"model {i}: clip head {h} outside 0..{nc}")
            if any(h == nc for h in md[10:13]):
                # a hull without clipnodes (head one past the clipnode
                # lump): an empty hull, as GoldSrc treats it
                self.models[i] = md[:10] + tuple(CONTENTS_EMPTY if h == nc else h
                                                 for h in md[10:13]) + md[13:]
            if md[14] < 0 or md[15] < 0 or md[14] + md[15] > nf:
                fail(f"model {i}: faces {md[14]}+{md[15]} outside 0..{nf}")
        vl = self.models[0][13]
        if not 0 <= vl < nl:
            fail(f"visleafs {vl} outside 0..{nl - 1}")

    def face_points(self, f):
        fc = self.faces[f]
        out = []
        for i in range(fc[3]):
            e = self.surfedges[fc[2] + i]
            vi = self.edges[e][0] if e >= 0 else self.edges[-e][1]
            out.append(self.verts[vi])
        return out

    def vis_row_end(self, ofs: int) -> int:
        """End (exclusive) of the compressed PVS row at vis offset ofs: the
        row decodes to ceil(visleafs / 8) bytes, a non-zero byte is one
        literal byte, a zero byte plus a count byte n stands for n zero
        bytes (w64-format.md VISR)."""
        v = self.vis
        want = (self.visleafs + 7) >> 3
        got, i = 0, ofs
        while got < want:
            if i >= len(v) or (v[i] == 0 and i + 1 >= len(v)):
                raise InputError(f"{self.path}: PVS row at {ofs} runs past the vis lump")
            if v[i]:
                got += 1
                i += 1
            else:
                got += v[i + 1]
                i += 2
        return i

    def leaf_pvs(self, leaf):
        """Visible leaf indices (1-based) from leaf, incl. itself."""
        ofs = self.leafs[leaf][1]
        n = self.visleafs
        if ofs < 0 or not self.vis:
            return None  # everything visible
        out = 0
        v = self.vis
        i = ofs
        bit = 0
        end = self.vis_row_end(ofs)
        while bit < n and i < end:
            c = v[i]
            i += 1
            if c == 0:
                bit += 8 * v[i]
                i += 1
                continue
            for k in range(8):
                if c & (1 << k) and bit + k < n:
                    out |= 1 << (bit + k + 1)
            bit += 8
        return out | (1 << leaf)

    def box_leafs(self, mins, maxs):
        """World leaves a box touches (both children when it straddles a
        node plane)."""
        out = set()
        stack = [self.models[0][9]]
        while stack:
            n = stack.pop()
            if n < 0:
                out.add(-1 - n)
                continue
            nd = self.nodes[n]
            px, py, pz, pd, pt = self.planes[nd[0]]
            if pt < 3:
                lo, hi = mins[pt] - pd, maxs[pt] - pd
            else:
                lo = hi = 0.0
                for k, nk in enumerate((px, py, pz)):
                    a, b = mins[k] * nk, maxs[k] * nk
                    lo += min(a, b)
                    hi += max(a, b)
                lo -= pd
                hi -= pd
            if hi >= 0:
                stack.append(nd[1])
            if lo < 0:
                stack.append(nd[2])
        return out


def parse_entities(text: str):
    ents = []
    for blk in re.findall(r"\{([^}]*)\}", text):
        kv = {}
        for k, v in re.findall(r'"([^"]*)"\s*"([^"]*)"', blk):
            kv[k] = v
        ents.append(kv)
    return ents


# --------------------------------------------------------------------------
# Textures (.wci4): one TEXP page per texture the cooked faces use
# --------------------------------------------------------------------------

WCI4_MAGIC = 0x57434934   # 'WCI4'
WCI4_HDR = 20
WCI4_ENTRY = 44


class TexPack:
    """maps/<map>.wci4 (tools.cook.bake_world_tex): per-texture CI4 + TLUT.
    Pages are single-texture (infinite wrap, bilinear). Header, entry table
    and every texel / TLUT range are checked against the file."""

    def __init__(self, path: Path):
        try:
            b = path.read_bytes()
        except OSError as e:
            raise InputError(f"{path}: {e.strerror}") from None

        def fail(what):
            raise InputError(f"{path}: {what}")

        if len(b) < WCI4_HDR:
            fail(f"truncated WCI4 header ({len(b)} B)")
        magic, ver, n, eo, _do, fsz = struct.unpack_from(">IHHIII", b, 0)
        if magic != WCI4_MAGIC or ver != 1:
            fail("not a WCI4 v1 pack")
        if fsz != len(b):
            fail(f"header file size {fsz} != {len(b)} B")
        if eo < WCI4_HDR or eo + WCI4_ENTRY * n > fsz:
            fail(f"entry table {eo}+{n}x{WCI4_ENTRY} outside the file")
        self.pages = []
        self.lookup = {}
        for i in range(n):
            o = eo + WCI4_ENTRY * i
            name = b[o:o + 16].split(b"\0")[0].decode("latin1")
            w, h, _ow, _oh, uvu, uvv, ci4o, ci4b, tluto = \
                struct.unpack_from(">4H2i3I", b, o + 16)
            if w < 2 or h < 2:
                continue
            if ci4b < w * h // 2 or ci4o + ci4b > fsz or tluto + 32 > fsz:
                fail(f"texture {i} ({name!r}): texel or TLUT range outside the file")
            if uvu <= 0 or uvv <= 0:
                fail(f"texture {i} ({name!r}): page scale {uvu}/{uvv} not positive")
            self.lookup[name[:16]] = len(self.pages)
            # 16.16 page/source ratio as the runtime reads it: f32(i32) / 65536
            self.pages.append({
                "name": name, "w": w, "h": h, "flags": 0,
                "tlut": b[tluto:tluto + 32],
                "ci4": b[ci4o:ci4o + w * h // 2],
                "uvu": F32(uvu) / F32(65536.0), "uvv": F32(uvv) / F32(65536.0),
            })

    def find(self, name: str):
        return self.lookup.get(name[:16])


# --------------------------------------------------------------------------
# Textures: page + animation sequencing (world-surfaces.md 4)
# --------------------------------------------------------------------------

def anim_frame(name: str):
    """'+<c><base>': (sequence 0 primary / 1 alternate, frame, base)."""
    c = name[1:2]
    if "0" <= c <= "9":
        return 0, ord(c) - ord("0"), name[2:]
    lc = c.lower()
    if "a" <= lc <= "j":
        return 1, ord(lc) - ord("a"), name[2:]
    raise InputError(f"texture {name!r}: bad animation frame letter {c!r}")


def build_anim(tex) -> None:
    """Fill anim_total/min/max/next/alternate of every '+' texture: frames
    0-9 the primary sequence, A-J (any case) the alternate, per base name;
    a sequence must be complete from frame 0."""
    seqs = defaultdict(lambda: ({}, {}))     # base -> (primary, alternate)
    for i, t in enumerate(tex):
        if t["name"].startswith("+"):
            which, frame, base = anim_frame(t["name"])
            seqs[base][which].setdefault(frame, i)
    for base, frames in seqs.items():
        lists = []
        for which in (0, 1):
            got = frames[which]
            for f in range(max(got) + 1 if got else 0):
                if f not in got:
                    have = tex[got[max(got)]]["name"]
                    want = "+" + (str(f) if which == 0 else chr(ord("a") + f)) + base
                    raise InputError(f"texture {have!r}: animation frame {want!r} missing")
            lists.append([got[f] for f in range(len(got))])
        for which, seq in enumerate(lists):
            other = lists[1 - which]
            for j, ti in enumerate(seq):
                t = tex[ti]
                t["flags"] |= TEXF_ANIM
                t["total"] = 2 * len(seq)
                t["min"] = 2 * j
                t["max"] = 2 * j + 2
                t["next"] = seq[(j + 1) % len(seq)]
                t["alt"] = other[0] if other else 0xFFFF


def build_textures(bsp: Bsp, pack: TexPack):
    tex = []
    missing = []
    for i, nm in enumerate(bsp.texnames):
        t = {"name": nm, "sheet": 0xFFFF, "uvu": F32(0), "uvv": F32(0),
             "tw": 0, "th": 0, "flags": 0, "total": 0, "min": 0, "max": 0,
             "next": 0xFFFF, "alt": 0xFFFF}
        if nm.lower().startswith("sky"):
            t["flags"] |= TEXF_SKY
        if nm.startswith("{"):
            t["flags"] |= TEXF_FENCE
        pg = pack.find(nm) if nm else None
        if pg is not None:
            p = pack.pages[pg]
            t["sheet"], t["uvu"], t["uvv"] = pg, p["uvu"], p["uvv"]
            t["tw"], t["th"] = p["w"], p["h"]
            if t["flags"] & TEXF_FENCE:
                p["flags"] |= TEXF_FENCE
        elif nm and not (t["flags"] & TEXF_SKY) and nm.lower() not in SKIP_TEX:
            t["flags"] |= TEXF_MISSING
            missing.append(nm)
        tex.append(t)
    try:
        build_anim(tex)
    except InputError as e:
        raise InputError(f"{bsp.path}: {e}") from None
    return tex, sorted(set(missing))


# --------------------------------------------------------------------------
# Face cook (float32, world-surfaces.md 1-2, w64-format.md 'Vertices')
# --------------------------------------------------------------------------

def i16(v) -> int:
    """C float -> int16_t (truncate toward zero, wrap)."""
    x = int(v)
    return ((x + 32768) & 0xFFFF) - 32768


def pack_normal(nx, ny, nz) -> int:
    """t3d_vert_pack_normal (5.6.5)."""
    xi = max(-16, min(15, int(round(nx * 15.5))))
    yi = max(-32, min(31, int(round(ny * 31.5))))
    zi = max(-16, min(15, int(round(nz * 15.5))))
    return ((xi & 31) << 11) | ((yi & 63) << 5) | (zi & 31)


def finish_light(light: int) -> int:
    v = int(F32(light) * LIGHT_SCALE + F32(0.5))
    return 0 if v < 0 else 255 if v > 255 else v


class Poly:
    __slots__ = ("face", "tex", "key", "styles", "verts", "raw", "centroid")


def tex_coord(p, v):
    """Texture coordinate p . v.xyz + v.offset, float32 left to right
    (world-surfaces.md 1)."""
    return p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + v[3]


def face_extents(ss, ts):
    """(smin, tmin), (extent_s, extent_t) of a face's luxel grid from its
    vertices' texture coordinates (world-surfaces.md 1)."""
    mins, ext = [], []
    for vals in (ss, ts):
        lo = math.floor(min(vals) / 16)
        hi = math.ceil(max(vals) / 16)
        mins.append(16 * lo)
        ext.append(16 * (hi - lo))
    return mins, ext


def face_styles(fc):
    """The face's style list (255 ends it)."""
    out = []
    for s in fc[5:9]:
        if s == 255:
            break
        out.append(s)
    return out


def luxel_raw(bsp: Bsp, lofs: int, idx: int, size: int, nstyles: int):
    """Mono luxel idx of every style map of a face whose lightmap starts
    at luxel lofs; 0 outside the lighting lump."""
    mono = bsp.mono
    out = []
    for k in range(nstyles):
        j = lofs + idx + k * size
        out.append(int(mono[j]) if 0 <= j < len(mono) else 0)
    return tuple(out)


def vertex_luxel(s, t, mins, ext):
    """Luxel (column, row) of a vertex: (trunc(s) - smin) >> 4 and
    (trunc(t) - tmin) >> 4, each clamped into the grid (world-surfaces.md 2)."""
    cols, rows = (ext[0] >> 4) + 1, (ext[1] >> 4) + 1
    c = (int(s) - mins[0]) >> 4
    r = (int(t) - mins[1]) >> 4
    return min(max(c, 0), cols - 1), min(max(r, 0), rows - 1)


def texel_coords(ss, tex):
    """s10.5 texel coordinates of a face's vertices on its page (w64-format.md
    'Vertices'), float32: the texture period in texture units is page size /
    page scale; the face is rebased by the whole periods below its smallest
    coordinate, then scaled to page texels x 32."""
    w, uv = tex
    if not w:
        return [0] * len(ss)
    period = F32(w) / uv
    base = F32(math.floor(min(ss) / period)) * period
    return [i16((s - base) * uv * F32(32)) for s in ss]


def cook_face(bsp: Bsp, f: int, tex):
    """Polys (<= POLY_MAX_VERTS verts) of one face with their T3DVertPacked
    attributes (x, y, z, normal, rgba, s, t); [] for faces not drawn (no
    texture, sky, skipped compile-tool textures)."""
    fc = bsp.faces[f]
    ti = bsp.texinfo[fc[4]]
    mt = ti[8]
    t = tex[mt]
    name = t["name"]
    if not name or t["flags"] & TEXF_SKY or name.lower() in SKIP_TEX:
        return []
    pts = bsp.face_points(f)
    vs = [F32(c) for c in ti[0:4]]
    vt = [F32(c) for c in ti[4:8]]
    ss = [tex_coord(p, vs) for p in pts]
    ts = [tex_coord(p, vt) for p in pts]
    mins, ext = face_extents(ss, ts)
    styles = face_styles(fc)
    lit = name[:1] not in ("*", "!") and fc[9] >= 0 and bool(styles)
    styled = tuple(styles) if lit and styles != [0] else ()
    pl = bsp.planes[fc[0]]
    # face normal (plane normal, flipped for back-side faces), packed from
    # the plane floats in double precision
    nrm = [-c if fc[1] else c for c in pl[:3]]
    norm = pack_normal(*nrm)
    su = texel_coords(ss, (t["tw"], t["uvu"]))
    sv = texel_coords(ts, (t["th"], t["uvv"]))
    lofs = fc[9] // 3
    size = ((ext[0] >> 4) + 1) * ((ext[1] >> 4) + 1)
    verts, raws = [], []
    for k, p in enumerate(pts):
        if lit:
            c, r = vertex_luxel(ss[k], ts[k], mins, ext)
            raw = luxel_raw(bsp, lofs, r * ((ext[0] >> 4) + 1) + c, size, len(styles))
            col = finish_light((sum(raw) * STYLE0_VALUE) >> 8)
        else:
            raw, col = (), 255
        rgba = (col << 24) | (col << 16) | (col << 8) | 0xFF
        verts.append((i16(p[0]), i16(p[1]), i16(p[2]), norm, rgba, su[k], sv[k]))
        raws.append(raw)
    # meshlet group: page for still textures, the texture itself for
    # animated ones (its frame swaps at draw time); animated groups last
    if t["flags"] & TEXF_ANIM:
        key = ((1, mt), styled)
    else:
        key = ((0, t["sheet"]), styled)
    out = []
    n = len(verts)
    step = POLY_MAX_VERTS - 2
    for a in range(1, n - 1, step):
        idx = [0] + list(range(a, min(n, a + step + 1)))
        p = Poly()
        p.face, p.tex, p.key, p.styles = f, mt, key, styled
        p.verts = [verts[i] for i in idx]
        p.raw = [raws[i] for i in idx] if styled else []
        p.centroid = tuple(sum(float(pts[i][k]) for i in idx) / len(idx) for k in range(3))
        out.append(p)
    return out


# --------------------------------------------------------------------------
# Meshlets + REND/MOVR chunk
# --------------------------------------------------------------------------

def poly_bytes(p: Poly) -> int:
    n = len(p.verts)
    return n * 16 + n * 2 + (n * len(p.styles))


def morton(c, org):
    x = max(0, min(1023, int((c[0] - org[0]) / 8)))
    y = max(0, min(1023, int((c[1] - org[1]) / 8)))
    z = max(0, min(1023, int((c[2] - org[2]) / 8)))
    m = 0
    for b in range(10):
        m |= ((x >> b) & 1) << (3 * b) | ((y >> b) & 1) << (3 * b + 1) | ((z >> b) & 1) << (3 * b + 2)
    return m


def zigzag(n: int):
    """Strip order of an n-gon fan (0,1,n-1,2,n-2,...): n indices, n-2 tris,
    same winding as the polygon."""
    out = [0, 1]
    lo, hi = 2, n - 1
    take_hi = True
    while lo <= hi:
        if take_hi:
            out.append(hi)
            hi -= 1
        else:
            out.append(lo)
            lo += 1
        take_hi = not take_hi
    return out


def build_meshlets(polys):
    """Group by (sheet/anim texture, style tuple), spatially order, pack."""
    groups = defaultdict(list)
    for p in polys:
        groups[p.key].append(p)
    if not polys:
        return []
    org = (min(p.centroid[0] for p in polys), min(p.centroid[1] for p in polys),
           min(p.centroid[2] for p in polys))
    meshlets = []
    for key in sorted(groups, key=lambda k: (k[0][0], k[0][1], k[1])):
        g = sorted(groups[key], key=lambda p: (morton(p.centroid, org), p.face))
        cur = []
        nv = 0
        for p in g:
            n = len(p.verts)
            if cur and nv + n > MESHLET_MAX_VERTS:
                meshlets.append((key, cur))
                cur, nv = [], 0
            cur.append(p)
            nv += n
        if cur:
            meshlets.append((key, cur))
    return meshlets


def encode_rend(meshlets, probes=None):
    """REND/MOVR chunk bytes. Layout in docs/formats/w64-format.md.
    probes: (org[3], dims[3], u8 values x fastest) or None (MOVR)."""
    nm = len(meshlets)
    styled = [m for m in meshlets if m[1][0].styles]
    hdr_sz = 24
    mtab_sz = nm * 24
    stab_sz = len(styled) * 8
    ofs = align(hdr_sz + mtab_sz + stab_sz, 16)
    vblobs, iblobs, sblobs = [], [], []
    recs = []
    vcur = ofs
    # vertex data (16-aligned per meshlet)
    for key, ps in meshlets:
        vlist = [v for p in ps for v in p.verts]
        nv = len(vlist)
        b = bytearray()
        for k in range(0, nv, 2):
            a = vlist[k]
            c = vlist[k + 1] if k + 1 < nv else a
            b += struct.pack(">3hH3hHIIhhhh", a[0], a[1], a[2], a[3],
                             c[0], c[1], c[2], c[3], a[4], c[4], a[5], a[6], c[5], c[6])
        vblobs.append(bytes(b))
        recs.append({"vofs": vcur, "nv": nv, "vl": vlist, "ps": ps, "key": key})
        vcur = align(vcur + len(b), 16)
    icur = vcur
    for r in recs:
        idx = []
        base = 0
        for p in r["ps"]:
            for j, z in enumerate(zigzag(len(p.verts))):
                e = (base + z) * T3D_VERT_OUTPUT_SIZE + T3D_VERT_BUFFER
                if j == 0:
                    e |= IDX_RESTART
                idx.append(e)
            base += len(p.verts)
        idx[-1] |= IDX_LAST
        if len(idx) > MESHLET_MAX_IDX:
            die("meshlet index overflow")
        b = struct.pack(">%dH" % len(idx), *idx)
        iblobs.append(b)
        r["iofs"] = icur
        r["ni"] = len(idx)
        icur = align(icur + len(b), 8)
    scur = icur
    srecs = []
    for r in recs:
        st = r["ps"][0].styles
        if not st:
            r["sidx"] = 0xFFFF
            continue
        raw = [x for p in r["ps"] for x in p.raw]
        b = bytes(v for rr in raw for v in rr)
        r["sidx"] = len(srecs)
        srecs.append((st, scur))
        sblobs.append(b)
        scur += len(b)
    pcur = 0
    if probes is not None:
        pcur = align(scur, 8)
        porg, pdims, pvals = probes
        pblob = struct.pack(">3h", *porg) + bytes(pvals)
        total = align(pcur + len(pblob), 16)
    else:
        total = align(scur, 16)
    out = bytearray(total)
    pd = probes[1] if probes is not None else (0, 0, 0)
    struct.pack_into(">HHIIII3Bx", out, 0, nm, len(srecs), ofs, vcur, icur, pcur, *pd)
    if probes is not None:
        out[pcur:pcur + len(pblob)] = pblob
    for i, r in enumerate(recs):
        vl = r["vl"]
        mn = [min(v[k] for v in vl) for k in range(3)]
        mx = [max(v[k] for v in vl) for k in range(3)]
        tex = r["ps"][0].tex
        struct.pack_into(">6hIIHH", out, hdr_sz + 24 * i, *mn, *mx,
                         (r["vofs"] << 8) | r["nv"], (r["iofs"] << 8) | r["ni"],
                         tex, r["sidx"])
        if r["vofs"] >= 1 << 24 or r["iofs"] >= 1 << 24:
            die("chunk > 16 MB")
    for i, (st, so) in enumerate(srecs):
        s4 = list(st) + [255] * (4 - len(st))
        struct.pack_into(">4BI", out, hdr_sz + mtab_sz + 8 * i, *s4, so)
    for r, b in zip(recs, vblobs):
        out[r["vofs"]:r["vofs"] + len(b)] = b
    for r, b in zip(recs, iblobs):
        out[r["iofs"]:r["iofs"] + len(b)] = b
    for (st, so), b in zip(srecs, sblobs):
        out[so:so + len(b)] = b
    stats = {"meshlets": nm, "verts": sum(r["nv"] for r in recs),
             "idx": sum(r["ni"] for r in recs),
             "tris": sum(len(p.verts) - 2 for r in recs for p in r["ps"]),
             "styled": len(srecs), "bytes": total,
             "vert_bytes": vcur - ofs, "idx_bytes": icur - vcur,
             "style_bytes": scur - icur,
             "probe_bytes": (total - pcur) if probes is not None else 0}
    bbox = None
    if recs:
        allv = [v for r in recs for v in r["vl"]]
        bbox = [min(v[k] for v in allv) for k in range(3)] + \
               [max(v[k] for v in allv) for k in range(3)]
    return bytes(out), stats, bbox

# --------------------------------------------------------------------------
# Sectors
# --------------------------------------------------------------------------

def build_sectors(polys, face_leaves, pvs):
    """Visibility-coherent sectors. Units = faces of one CELL-sized cell
    marked by the same leaf set; a unit under SECTOR_MIN merges into the
    nearby sector (bboxes within SECTOR_GAP) whose union adds the least
    overfetch: bytes(A) x (leaves that see B but not A) + the converse.
    pvs[l] is leaf l's PVS bitmask (bit k = leaf k visible from l).
    Plain 256/512 u grids overfetch 2x the exact PVS bytes on hldemo2 (worst
    leaf 206 KB vs 117 KB); this gets 130 KB."""
    units = defaultdict(list)
    for p in polys:
        c = p.centroid
        units[(int(c[0] // CELL), int(c[1] // CELL), int(c[2] // CELL),
               frozenset(face_leaves.get(p.face, ())))].append(p)
    secs = []
    for k, ps in units.items():
        view = 0
        own = 0
        for l in k[3]:
            view |= pvs[l]
            own |= 1 << l
        vs = [v for p in ps for v in p.verts]
        secs.append({"ps": ps, "b": sum(poly_bytes(p) for p in ps), "V": view,
                     "L": own, "final": False,
                     "bb": [min(v[i] for v in vs) for i in range(3)]
                     + [max(v[i] for v in vs) for i in range(3)]})

    def near(a, b):
        A, B = a["bb"], b["bb"]
        return all(A[i] - SECTOR_GAP <= B[i + 3] and B[i] - SECTOR_GAP <= A[i + 3]
                   for i in range(3))

    def cost(a, b):
        return (a["b"] * (b["V"] & ~a["V"]).bit_count()
                + b["b"] * (a["V"] & ~b["V"]).bit_count())

    while True:
        small = [s for s in secs if s["b"] < SECTOR_MIN and not s["final"]]
        if not small:
            break
        s = min(small, key=lambda x: x["b"])
        best = None
        for n in secs:
            if n is s or s["b"] + n["b"] > SECTOR_MAX or not near(s, n):
                continue
            c = cost(s, n)
            if best is None or c < best[0]:
                best = (c, n)
        if best is None:
            s["final"] = True
            continue
        n = best[1]
        n["ps"] += s["ps"]
        n["b"] += s["b"]
        n["V"] |= s["V"]
        n["L"] |= s["L"]
        n["bb"] = [min(n["bb"][i], s["bb"][i]) for i in range(3)] + \
                  [max(n["bb"][i + 3], s["bb"][i + 3]) for i in range(3)]
        n["final"] = False
        secs.remove(s)

    out = []

    def split(ps):
        if sum(poly_bytes(p) for p in ps) <= SECTOR_MAX or len(ps) < 2:
            out.append(ps)
            return
        ext = [max(p.centroid[k] for p in ps) - min(p.centroid[k] for p in ps) for k in range(3)]
        ax = ext.index(max(ext))
        ps = sorted(ps, key=lambda p: (p.centroid[ax], p.face))
        half = len(ps) // 2
        split(ps[:half])
        split(ps[half:])

    for s in secs:
        split(s["ps"])
    # deterministic order: by min centroid
    out.sort(key=lambda ps: min((p.centroid[2], p.centroid[1], p.centroid[0], p.face) for p in ps))
    return out


# --------------------------------------------------------------------------
# TEXP
# --------------------------------------------------------------------------

def encode_texp(s):
    """One texture page: hdr (w, h, flags, ci4_ofs) | TLUT16 @16 | CI4 @48."""
    out = bytearray(48)
    struct.pack_into(">HHB3xI", out, 0, s["w"], s["h"], s["flags"], 48)
    out[16:48] = s["tlut"]
    out += s["ci4"]
    while len(out) & 15:
        out.append(0)
    return bytes(out)


# --------------------------------------------------------------------------
# Light probes (world-surfaces.md 3, float32)
# --------------------------------------------------------------------------

def atof(s) -> float:
    """C atof: leading number, 0 when none."""
    m = re.match(r"\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?", s or "")
    return float(m.group(0)) if m else 0.0


def style_values(ents):
    """Style values at map start (world-surfaces.md 3): worldspawn's
    standard styles at time 0, switchable lights (style >= 32: "m", or "a"
    with START_OFF; light / light_spot only with a targetname, since the
    entity cook prunes them otherwise, light_fluoro always), every other
    style unset = 256."""
    v = [STYLE_UNSET] * 256
    for s, m in WORLD_STYLES.items():
        v[s] = (ord(m[0]) - ord("a")) * 22
    for e in ents:
        cls = e.get("classname", "")
        if cls not in ("light", "light_spot", "light_fluoro"):
            continue
        if cls != "light_fluoro" and not e.get("targetname"):
            continue
        st = int(atof(e.get("style")))
        if SWITCH_STYLE_FIRST <= st < 256:
            v[st] = 0 if int(atof(e.get("spawnflags"))) & 1 else STYLE0_VALUE
    return v


class LightFaces:
    """Per-face lightmap context of the probe walk: texinfo vectors, luxel
    grid (smin, tmin) and extents (float32, world-surfaces.md 1), mono luxel
    offset, styles. None = sky ("sky"/"SKY") or water ('*'/'!'): skipped."""

    def __init__(self, bsp: Bsp, values):
        self.bsp = bsp
        self.values = values
        self.cache = {}

    def get(self, f):
        if f in self.cache:
            return self.cache[f]
        bsp = self.bsp
        fc = bsp.faces[f]
        ti = bsp.texinfo[fc[4]]
        name = bsp.texnames[ti[8]] if 0 <= ti[8] < len(bsp.texnames) else ""
        info = None
        if not (name.startswith(("sky", "SKY")) or name[:1] in ("*", "!")):
            vs = [F32(c) for c in ti[0:4]]
            vt = [F32(c) for c in ti[4:8]]
            pts = bsp.face_points(f)
            tmin, ext = face_extents([tex_coord(p, vs) for p in pts],
                                     [tex_coord(p, vt) for p in pts])
            styles = face_styles(fc)
            lofs = fc[9] // 3 if fc[9] >= 0 else -1
            info = (vs, vt, tmin, ext, lofs, styles)
        self.cache[f] = info
        return info

    def sample(self, f, mid):
        """Probe value of face f at crossing point mid, or None when the
        face does not contain it (or is sky / water)."""
        info = self.get(f)
        if info is None:
            return None
        vs, vt, mins, ext, lofs, styles = info
        ds = int(tex_coord(mid, vs)) - mins[0]
        dt = int(tex_coord(mid, vt)) - mins[1]
        if ds < 0 or dt < 0 or ds > ext[0] or dt > ext[1]:
            return None
        if lofs < 0:
            return 0
        cols = (ext[0] >> 4) + 1
        size = cols * ((ext[1] >> 4) + 1)
        raw = luxel_raw(self.bsp, lofs, (dt >> 4) * cols + (ds >> 4), size, len(styles))
        v = sum(r * self.values[s] for r, s in zip(raw, styles)) >> 8
        return min(max(v, 0), 255)


def light_point(bsp: Bsp, lf: LightFaces, p) -> int:
    """Static light below point p (world-surfaces.md 3): the segment p ..
    p - (0, 0, LIGHT_TRACE) walked through the world's render nodes, near
    side first; at each node plane it crosses, the first face of that node
    containing the crossing point gives the value, else the walk goes on
    behind the plane. No hit: 0."""
    start = tuple(F32(c) for c in p)
    end = (start[0], start[1], start[2] - F32(LIGHT_TRACE))
    stack = [(bsp.models[0][9], start, end, None)]
    while stack:
        node, a, b, far = stack.pop()
        if far is not None:
            # near side gave nothing: this node's faces at the crossing a
            nd = bsp.nodes[node]
            for fi in range(nd[9], nd[9] + nd[10]):
                v = lf.sample(fi, a)
                if v is not None:
                    return v
            stack.append((far, a, b, None))
            continue
        if node < 0:
            continue
        nd = bsp.nodes[node]
        pl = bsp.planes[nd[0]]
        n = (F32(pl[0]), F32(pl[1]), F32(pl[2]), F32(pl[3]))
        fa = a[0] * n[0] + a[1] * n[1] + a[2] * n[2] - n[3]
        fb = b[0] * n[0] + b[1] * n[1] + b[2] * n[2] - n[3]
        side = 1 if fa < 0 else 0
        if (1 if fb < 0 else 0) == side:
            stack.append((nd[1 + side], a, b, None))
            continue
        frac = fa / (fa - fb)
        mid = tuple(a[k] + (b[k] - a[k]) * frac for k in range(3))
        stack.append((node, mid, b, nd[2 - side]))
        stack.append((nd[1 + side], a, mid, None))
    return 0


def light_points(bsp: Bsp, lf: LightFaces, pts) -> np.ndarray:
    """Probe values of every point (rows of pts)."""
    if not len(bsp.light):
        return np.full(len(pts), 255, np.int64)
    return np.array([light_point(bsp, lf, p) for p in pts.tolist()], np.int64)


def probe_grid(bbox):
    """64 u probe lattice covering bbox: org, dims, points (x fastest)."""
    lo = [math.floor(bbox[k] / PROBE_STEP) for k in range(3)]
    hi = [math.floor(bbox[k + 3] / PROBE_STEP) for k in range(3)]
    dims = [hi[k] - lo[k] + 1 for k in range(3)]
    if max(dims) > 255:
        die(f"probe grid {dims} > 255 per axis")
    org = [lo[k] * PROBE_STEP for k in range(3)]
    z, y, x = np.meshgrid(*(np.arange(dims[k]) for k in (2, 1, 0)), indexing="ij")
    pts = np.stack([org[0] + x.ravel() * PROBE_STEP, org[1] + y.ravel() * PROBE_STEP,
                    org[2] + z.ravel() * PROBE_STEP], axis=1)
    return org, dims, pts


# --------------------------------------------------------------------------
# Collision: COLL chunks, model table, grid
# --------------------------------------------------------------------------

CLIP_KEY = 1 << 20   # hull-tree node key: hull-0 node i -> i, clipnode j -> CLIP_KEY + j


class HullGraph:
    """Hull-0 nodes and hull 1-3 clipnodes as one keyed graph. Children are
    keys (>= 0) or the BSP leaf child (< 0: hull 0 -1-leaf, clip contents)."""

    def __init__(self, bsp: Bsp):
        self.bsp = bsp
        self._pc = {}

    def kids(self, k):
        if k < CLIP_KEY:
            nd = self.bsp.nodes[k]
            return nd[1], nd[2]
        cn = self.bsp.clip[k - CLIP_KEY]
        return tuple(c + CLIP_KEY if c >= 0 else c for c in cn[1:3])

    def plane(self, k):
        return self.bsp.nodes[k][0] if k < CLIP_KEY else self.bsp.clip[k - CLIP_KEY][0]

    def pclass(self, k):
        """0/1/2 axial X/Y/Z (BSP type < 3, normal exactly +unit), 3 other."""
        pi = self.plane(k)
        c = self._pc.get(pi)
        if c is None:
            pl = self.bsp.planes[pi]
            c = 3
            if pl[4] < 3:
                unit = [0.0, 0.0, 0.0]
                unit[pl[4]] = 1.0
                if list(pl[:3]) != unit:
                    die(f"hull plane {pi} type {pl[4]} normal {pl[:3]} is not +unit")
                c = pl[4]
            self._pc[pi] = c
        return c

    def box_range(self, k, Lo, Hi):
        """Min / max plane distance of boxes (float64)."""
        pl = self.bsp.planes[self.plane(k)]
        c = self.pclass(k)
        if c < 3:
            return Lo[:, c] - pl[3], Hi[:, c] - pl[3]
        n = np.array(pl[:3])
        a, b = Lo * n, Hi * n
        return np.minimum(a, b).sum(1) - pl[3], np.maximum(a, b).sum(1) - pl[3]


def coll_partition(hg: HullGraph, roots, cap):
    """One tree group -> chunks (lists of node keys). Top-down BFS chunks
    while a subtree exceeds the cap, then the small subtrees packed in DFS
    preorder."""
    pre, order = {}, []
    for r in roots:
        st = [r]
        while st:
            k = st.pop()
            if k in pre:
                continue
            pre[k] = len(order)
            order.append(k)
            c0, c1 = hg.kids(k)
            if c1 >= 0:
                st.append(c1)
            if c0 >= 0:
                st.append(c0)
    est = {}
    for k in reversed(order):   # tree: children follow parents in preorder
        e = 8 + (16 if hg.pclass(k) == 3 else 0)
        for c in hg.kids(k):
            if c >= 0:
                e += est[c]
        est[k] = e
    chunks, small, done = [], [], set()
    queue = list(roots)
    qi = 0
    while qi < len(queue):
        r = queue[qi]
        qi += 1
        if r in done:
            continue
        if 16 + est[r] <= cap:
            small.append(r)
            continue
        nodes, planes, ext = [], set(), 0
        fr = [r]
        fi = 0
        while fi < len(fr):
            k = fr[fi]
            np_ = planes
            if hg.pclass(k) == 3:
                np_ = planes | {tuple(hg.bsp.planes[hg.plane(k)][:4])}
            kn = [c for c in hg.kids(k) if c >= 0]
            ne = ext - (1 if nodes else 0) + len(kn)
            if nodes and 16 + 16 * len(np_) + 8 * (len(nodes) + 1) + 4 * ne > cap:
                break
            fi += 1
            nodes.append(k)
            done.add(k)
            planes, ext = np_, ne
            fr.extend(kn)
        chunks.append(nodes)
        queue.extend(fr[fi:])
    small.sort(key=pre.get)
    cur, cur_b = [], 16
    for r in small:
        if cur and cur_b + est[r] > cap:
            chunks.append(cur)
            cur, cur_b = [], 16
        st = [r]
        while st:
            k = st.pop()
            cur.append(k)
            done.add(k)
            c0, c1 = hg.kids(k)
            if c1 >= 0:
                st.append(c1)
            if c0 >= 0:
                st.append(c0)
        cur_b += est[r]
    if cur:
        chunks.append(cur)
    return chunks


def coll_encode(hg: HullGraph, chunks):
    """COLL chunk blobs + gid of every node key."""
    gid, sorted_chunks = {}, []
    for ci, nodes in enumerate(chunks):
        if len(nodes) > COLL_MAX_NODES:
            die(f"COLL chunk {ci}: {len(nodes)} nodes > {COLL_MAX_NODES}")
        sn = sorted(nodes, key=hg.pclass)   # stable: keeps BFS / preorder
        for li, k in enumerate(sn):
            gid[k] = (ci << GID_LOCAL_BITS) | li
        sorted_chunks.append(sn)
    blobs = []
    for sn in sorted_chunks:
        local = {k: i for i, k in enumerate(sn)}
        cls = [hg.pclass(k) for k in sn]
        nx = cls.count(0)
        ny = nx + cls.count(1)
        nz = ny + cls.count(2)
        planes, pidx, ext, eidx = [], {}, [], {}
        nb = bytearray()
        for k, c in zip(sn, cls):
            pl = hg.bsp.planes[hg.plane(k)]
            if c < 3:
                head = struct.pack(">f", pl[3])
            else:
                key = tuple(pl[:4])
                if key not in pidx:
                    pidx[key] = len(planes)
                    planes.append(key)
                head = struct.pack(">I", pidx[key])
            ch = []
            for v in hg.kids(k):
                if v < 0:
                    ch.append(v)
                elif v in local:
                    ch.append(local[v])
                else:
                    g = gid[v]
                    if g not in eidx:
                        eidx[g] = len(ext)
                        ext.append(g)
                    ch.append(len(sn) + eidx[g])
            if not all(-32768 <= v <= 32767 for v in ch):
                die("COLL child out of int16 range")
            nb += head + struct.pack(">hh", *ch)
        b = struct.pack(">6HI", len(sn), nx, ny, nz, len(planes), len(ext), 0)
        b += b"".join(struct.pack(">4f", *p) for p in planes) + bytes(nb)
        b += struct.pack(">%dI" % len(ext), *ext)
        blobs.append(b + bytes(align(len(b), 16) - len(b)))
    return blobs, gid


def head_key(md, h):
    """Hull h root of a dmodel_t as a HullGraph key, or the raw leaf child."""
    v = md[9 + h]
    if v < 0:
        return v
    return v + CLIP_KEY if h else v


def coll_node_name(k) -> str:
    return f"node {k}" if k < CLIP_KEY else f"clipnode {k - CLIP_KEY}"


def trees_holding(hg: HullGraph, roots, k) -> str:
    """'model M hull H' of every root in roots whose tree reaches key k."""
    out = []
    for mi, h, r in roots:
        st, seen = [r], set()
        while st:
            x = st.pop()
            if x < 0 or x in seen:
                continue
            if x == k:
                out.append(f"model {mi} hull {h}")
                break
            seen.add(x)
            st.extend(hg.kids(x))
    return ", ".join(out) or "?"


def build_coll(bsp: Bsp, traced):
    """Partition per tree group (pinned: world hull 0 + the hull 0 of every
    traced submodel; world hulls 1-3; each submodel's remaining hulls) and
    encode. Hull 0 is what point traces walk (sight lines, bullets, grenade
    checks, contents): pinned, a trace through any traced brush entity never
    loads synchronously (Uplink: 15-22 KB per map). Returns blobs, gid,
    per-model (first, count) chunk range (submodels: hulls 1-3, plus hull 0
    when not traced), pinned count, world hull 1-3 chunk range."""
    hg = HullGraph(bsp)
    pin0 = [mi for mi in range(1, len(bsp.models)) if traced[mi]]
    # roots per group as (model, hull, key)
    groups = [[(0, 0, head_key(bsp.models[0], 0))]
              + [(mi, 0, head_key(bsp.models[mi], 0)) for mi in pin0],
              [(0, h, head_key(bsp.models[0], h)) for h in (1, 2, 3)]]
    for mi, md in enumerate(bsp.models[1:], 1):
        groups.append([(mi, h, head_key(md, h))
                       for h in (range(1, 4) if traced[mi] else range(4))])
    chunks, ranges, owner = [], [], {}
    for gi, roots in enumerate(groups):
        keys = [r for _m, _h, r in roots if r >= 0]
        first = len(chunks)
        for c in coll_partition(hg, keys, COLL_CAP):
            for k in c:
                if k in owner:
                    die(f"hull node {coll_node_name(k)} shared by "
                        f"{trees_holding(hg, groups[owner[k]], k)} and "
                        f"{trees_holding(hg, roots, k)}")
                owner[k] = gi
            chunks.append(c)
        ranges.append((first, len(chunks) - first))
    blobs, gid = coll_encode(hg, chunks)
    pinned = ranges[0][1]
    model_range = [(0, ranges[0][1] + ranges[1][1])] + ranges[2:]
    return hg, blobs, gid, model_range, pinned, ranges[1]


def coll_baseline(bsp: Bsp, hg: HullGraph):
    """8 B x unique hull nodes reachable from model heads + 16 B x unique
    planes they use."""
    seen, planes = set(), set()
    for md in bsp.models:
        st = [head_key(md, h) for h in range(4)]
        while st:
            k = st.pop()
            if k < 0 or k in seen:
                continue
            seen.add(k)
            planes.add(hg.plane(k))
            st.extend(hg.kids(k))
    return 8 * len(seen) + 16 * len(planes), len(seen), len(planes)


def build_grid(bsp: Bsp, hg: HullGraph, gid, w13):
    """Collision grid over the world bbox: per cell the world hull 1-3 COLL
    chunks a box-pruned walk can reach and the deepest hull-0 node whose
    region holds the whole cell. Conservative: a cell is on one side of a
    plane only when all of it is >= CELL_EPS away."""
    wm = bsp.models[0]
    lo = [math.floor(wm[k] / GRID_CELL) for k in range(3)]
    hi = [math.floor(wm[k + 3] / GRID_CELL) for k in range(3)]
    dims = [hi[k] - lo[k] + 1 for k in range(3)]
    org = [lo[k] * GRID_CELL for k in range(3)]
    if not all(-32768 <= o <= 32767 for o in org):
        die(f"grid origin {org} out of int16")
    n = dims[0] * dims[1] * dims[2]
    ix = np.arange(n)
    cxyz = np.stack([ix % dims[0], (ix // dims[0]) % dims[1], ix // (dims[0] * dims[1])], 1)
    Lo = (np.array(org) + cxyz * GRID_CELL).astype(np.float64)
    Hi = Lo + GRID_CELL
    w13_first, w13_n = w13
    mark = np.zeros((n, w13_n), bool)
    for h in (1, 2, 3):
        root = head_key(wm, h)
        if root < 0:
            continue
        st = [(root, ix)]
        while st:
            k, idx = st.pop()
            mark[idx, (gid[k] >> GID_LOCAL_BITS) - w13_first] = True
            dlo, dhi = hg.box_range(k, Lo[idx], Hi[idx])
            c0, c1 = hg.kids(k)
            if c0 >= 0:
                s = idx[dhi > -CELL_EPS]
                if len(s):
                    st.append((c0, s))
            if c1 >= 0:
                s = idx[dlo < CELL_EPS]
                if len(s):
                    st.append((c1, s))
    start = np.zeros(n, np.int64)
    root = head_key(wm, 0)
    if root < 0:
        start[:] = root
    else:
        st = [(root, ix)]
        while st:
            k, idx = st.pop()
            dlo, dhi = hg.box_range(k, Lo[idx], Hi[idx])
            front = dlo >= CELL_EPS
            back = dhi <= -CELL_EPS
            start[idx[~front & ~back]] = gid[k]
            for c, sel in zip(hg.kids(k), (front, back)):
                s = idx[sel]
                if not len(s):
                    continue
                if c < 0:
                    start[s] = c
                else:
                    st.append((c, s))
    svals, sidx = np.unique(start, return_inverse=True)
    rows, rinv = np.unique(mark, axis=0, return_inverse=True)
    rinv = rinv.ravel()
    pool, row_ofs = [0], []
    for row in rows:
        lst = (np.flatnonzero(row) + w13_first).tolist()
        if not lst:
            row_ofs.append(0)
            continue
        row_ofs.append(len(pool))
        pool += [len(lst)] + lst
    if len(pool) > 0xFFFF or len(svals) > 0xFFFF:
        die(f"grid list pool {len(pool)} / start table {len(svals)} > 65535")
    cells = np.stack([np.array(row_ofs, np.int64)[rinv], sidx.ravel()], 1)
    return {"org": org, "dims": dims, "n": n, "Lo": Lo, "Hi": Hi, "mark": mark,
            "cells": cells, "start": svals.tolist(), "pool": pool,
            "nlists": int(sum(1 for o in row_ofs if o))}


def coll_budget(grid, coll_sizes, pinned, w13, model_range, placements):
    """Per cell: pinned + union of the 3x3x3 neighbourhood lists + every
    submodel whose placement box (+SUBMODEL_PAD) touches the neighbourhood."""
    dx, dy, dz = grid["dims"]
    w13_first, w13_n = w13
    sizes = np.array(coll_sizes, np.int64)
    M = grid["mark"].reshape(dz, dy, dx, w13_n)
    P = np.zeros((dz + 2, dy + 2, dx + 2, w13_n), bool)
    P[1:-1, 1:-1, 1:-1] = M
    U = np.zeros_like(M)
    for a in range(3):
        for b in range(3):
            for c in range(3):
                U |= P[a:a + dz, b:b + dy, c:c + dx]
    U = U.reshape(grid["n"], w13_n)
    nb = U.astype(np.int64) @ sizes[w13_first:w13_first + w13_n]
    pin = int(sizes[:pinned].sum())
    pad = GRID_CELL + SUBMODEL_PAD
    Lo, Hi = grid["Lo"] - pad, grid["Hi"] + pad
    sub = np.zeros(grid["n"], np.int64)
    for mi in range(1, len(model_range)):
        f, c = model_range[mi]
        mb = int(sizes[f:f + c].sum())
        if not mb:
            continue
        touch = np.zeros(grid["n"], bool)
        for pmin, pmax in placements[mi]:
            touch |= np.all((Lo <= np.array(pmax)) & (np.array(pmin) <= Hi), axis=1)
        sub += touch * mb
    tot = pin + nb + sub
    w = int(np.argmax(tot))
    return {"bytes": int(tot[w]), "cell": int(w),
            "cell_xyz": [w % dx, (w // dx) % dy, w // (dx * dy)],
            "pinned": pin, "lists": int(nb[w]), "submodels": int(sub[w]),
            "lists_max": int(nb.max()), "submodels_max": int(sub.max())}


def never_traced(e) -> bool:
    """Entities whose brush model the runtime never streams collision for
    (w64-format.md 'Budget proof'): trigger_* (SOLID_TRIGGER, bbox-only
    touch) and func_illusionary (SOLID_NOT) without a contents skin
    (skin < -1, e.g. -16 ladder / -3 water, makes the engine trace its hull)."""
    cls = e.get("classname", "")
    if cls.startswith("trigger_"):
        return True
    return cls == "func_illusionary" and int(atof(e.get("skin"))) >= -1


def submodel_placements(bsp: Bsp, ents, traced_only: bool):
    """World-space boxes of the submodels: model bbox + origin key of each
    entity using it. traced_only: only entities whose hull the runtime can
    trace (collision proof; a model no such entity uses has no box)."""
    out = [[] for _ in bsp.models]
    for e in ents:
        m = e.get("model", "")
        if not m.startswith("*"):
            continue
        try:
            mi = int(m[1:])
        except ValueError:
            continue
        if not 0 < mi < len(bsp.models) or (traced_only and never_traced(e)):
            continue
        o = [atof(x) for x in (e.get("origin", "0 0 0").split() + ["0", "0", "0"])[:3]]
        md = bsp.models[mi]
        out[mi].append(([md[k] + o[k] for k in range(3)], [md[3 + k] + o[k] for k in range(3)]))
    return out


# --------------------------------------------------------------------------
# VISR
# --------------------------------------------------------------------------

def build_visr(bsp: Bsp):
    """ofs[visleafs + 1] (from chunk start) + every visleaf's compressed PVS
    row, bytes unchanged (w64-format.md VISR); empty row = no vis."""
    if not bsp.vis:
        return None
    nv = bsp.visleafs
    rows = []
    for L in range(1, nv + 1):
        o = bsp.leafs[L][1] if L < len(bsp.leafs) else -1
        if o < 0:
            rows.append(b"")
            continue
        rows.append(bsp.vis[o:bsp.vis_row_end(o)])
    ofs = [4 * (nv + 1)]
    for r in rows:
        ofs.append(ofs[-1] + len(r))
    b = struct.pack(">%dI" % len(ofs), *ofs) + b"".join(rows)
    return b + bytes(align(len(b), 16) - len(b))


# --------------------------------------------------------------------------
# Surface materials (w64-format.md 'Surface materials'; the texture-type
# reader is tools/hlsdk/materials.py)
# --------------------------------------------------------------------------

def build_materials(bsp: Bsp, gid, ncoll: int, mats: Materials):
    """w64_mat_t parts: per COLL chunk start, u16 entries (local << 4 | code)
    of every hull-0 node whose faces are not all concrete, the mixed nodes
    (gid, first, count) and their face boxes."""
    texcode = [mats.code(n) for n in bsp.texnames]

    def face_code(f):
        mt = bsp.texinfo[bsp.faces[f][4]][8]
        return texcode[mt] if 0 <= mt < len(texcode) else 0

    per_chunk = defaultdict(list)
    mixed = []
    for k, g in gid.items():
        if k >= CLIP_KEY:
            continue
        nd = bsp.nodes[k]
        faces = range(nd[9], nd[9] + nd[10])
        codes = [face_code(f) for f in faces]
        if not codes or set(codes) == {0}:
            continue
        if len(set(codes)) == 1:
            per_chunk[g >> GID_LOCAL_BITS].append(((g & 0xFFF) << 4) | codes[0])
            continue
        per_chunk[g >> GID_LOCAL_BITS].append(((g & 0xFFF) << 4) | W64_MAT_MIXED)
        n = bsp.planes[nd[0]][:3]
        ax = 0
        if abs(n[1]) > abs(n[ax]):
            ax = 1
        if abs(n[2]) > abs(n[ax]):
            ax = 2
        a0, a1 = (1 if ax == 0 else 0), (1 if ax == 2 else 2)
        recs = []
        for f, c in zip(faces, codes):
            pts = bsp.face_points(f)
            box = []
            for a in (a0, a1):
                box.append((math.floor(min(p[a] for p in pts)) - 1,
                            math.ceil(max(p[a] for p in pts)) + 1))
            recs.append((box[0][0], box[1][0], box[0][1], box[1][1], c))
        mixed.append((g, recs))
    start, ents = [], []
    for c in range(ncoll):
        start.append(len(ents))
        ents.extend(sorted(per_chunk.get(c, ())))
    start.append(len(ents))
    mixed.sort()
    mix_b, face_b = bytearray(), bytearray()
    nface = 0
    for g, recs in mixed:
        mix_b += struct.pack(">IHH", g, nface, len(recs))
        for r in recs:
            face_b += struct.pack(">4hBx", *r)
        nface += len(recs)
    if len(ents) > 0xFFFF or nface > 0xFFFF or len(mixed) > 0xFFFF:
        die(f"material table too large: {len(ents)} nodes, {nface} faces")
    return {"start": struct.pack(">%dH" % len(start), *start),
            "ents": struct.pack(">%dH" % len(ents), *ents),
            "mix": bytes(mix_b), "faces": bytes(face_b),
            "nent": len(ents), "nmix": len(mixed), "nface": nface}


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def tex_pages(tex, ids):
    """Pages a set of textures can bind, every animation/alternate frame
    included (they swap at draw time)."""
    out, seen, stack = set(), set(), list(ids)
    while stack:
        j = stack.pop()
        if j in seen or j == 0xFFFF:
            continue
        seen.add(j)
        if tex[j]["sheet"] != 0xFFFF:
            out.add(tex[j]["sheet"])
        if tex[j]["flags"] & TEXF_ANIM:
            stack += [tex[j]["next"], tex[j]["alt"]]
    return sorted(out)


HLE_HDR = 96      # hle_header_t
HLN_HDR = 24      # hln_header_t


def load_hle(path: Path) -> bytes:
    """The HLE1 entity blob (docs/formats/hle-format-v1.md) plus its
    optional HLN1 node-graph tail: magic, version and sizes checked."""
    try:
        b = path.read_bytes()
    except OSError as e:
        raise InputError(f"{path}: {e.strerror}") from None
    if len(b) < HLE_HDR:
        raise InputError(f"{path}: truncated HLE1 header ({len(b)} B)")
    if b[:4] == b"HLE1":
        bo = "<"
    elif b[:4] == b"1ELH":
        bo = ">"
    else:
        raise InputError(f"{path}: not an HLE1 blob")
    ver, _flags, size = struct.unpack_from(bo + "HHI", b, 4)
    if ver != 1:
        raise InputError(f"{path}: HLE1 version {ver}, not 1")
    if not HLE_HDR <= size <= len(b):
        raise InputError(f"{path}: HLE1 size {size} outside {HLE_HDR}..{len(b)}")
    tail = align(size, 16)
    if len(b) > size:
        if len(b) < tail + HLN_HDR or b[tail:tail + 4] != (b"HLN1" if bo == "<" else b"1NLH"):
            raise InputError(f"{path}: {len(b) - size} B after the HLE1 chunk are not an HLN1 graph")
        nver, _nf, nsize = struct.unpack_from(bo + "HHI", b, tail + 4)
        if nver != 1 or tail + nsize != len(b) or nsize < HLN_HDR:
            raise InputError(f"{path}: HLN1 version {nver} / size {nsize} does not end the file")
    return b


def load_decl(path: Path) -> bytes:
    """A map's placed-decal sheet (tools/cook/decals.py, DI4 2), checked
    with the engine's sheet rules."""
    try:
        b = path.read_bytes()
        decals.sheet_names(b, str(path))
    except OSError as e:
        raise InputError(f"{path}: {e.strerror}") from None
    except decals.DecalError as e:
        raise InputError(str(e)) from None
    return b


def cook(bsp_path: Path, wci4_path: Path, out_path: Path, hle,
         mats: "Materials | None" = None, decl: "bytes | None" = None):
    """Cook one map; hle = the ENTS blob bytes or None; decl = the DECL
    chunk (the map's placed-decal sheet) or None."""
    bsp = Bsp(bsp_path)
    pack = TexPack(wci4_path)
    tex, missing = build_textures(bsp, pack)
    ents = parse_entities(bsp.entities)

    # submodel classification
    nmod = len(bsp.models)
    sub_kind = [SUB_NONE] * nmod
    sub_class = [""] * nmod
    for e in ents:
        m = e.get("model", "")
        if not m.startswith("*"):
            continue
        try:
            mi = int(m[1:])
        except ValueError:
            continue
        if not 0 < mi < nmod:
            continue
        cls = e.get("classname", "")
        sub_class[mi] = cls
        if cls.startswith("trigger_"):
            continue
        static = (cls in FOLD_CLASSES and not e.get("targetname")
                  and e.get("origin", "0 0 0").split() in (["0", "0", "0"], [])
                  and e.get("angles", "0 0 0").split() in (["0", "0", "0"], []))
        kind = SUB_FOLDED if static else SUB_MOVR
        # one non-static user of *N makes it a mover
        if kind == SUB_MOVR or sub_kind[mi] == SUB_MOVR:
            sub_kind[mi] = SUB_MOVR
        else:
            sub_kind[mi] = SUB_FOLDED

    # world + folded faces
    polys = []
    face_leaves = defaultdict(set)
    for leaf in range(1, min(bsp.visleafs + 1, len(bsp.leafs))):
        lf = bsp.leafs[leaf]
        for k in range(lf[9]):
            face_leaves[bsp.mark[lf[8] + k]].add(leaf)
    m0 = bsp.models[0]
    world_faces = range(m0[14], m0[14] + m0[15])
    for f in world_faces:
        polys.extend(cook_face(bsp, f, tex))
    folded_faces = 0
    for mi in range(1, nmod):
        if sub_kind[mi] != SUB_FOLDED:
            continue
        md = bsp.models[mi]
        for f in range(md[14], md[14] + md[15]):
            ps = cook_face(bsp, f, tex)
            if not ps:
                continue
            pts = bsp.face_points(f)
            mins = [min(float(p[k]) for p in pts) - 1 for k in range(3)]
            maxs = [max(float(p[k]) for p in pts) + 1 for k in range(3)]
            face_leaves[f] |= {l for l in bsp.box_leafs(mins, maxs) if 0 < l <= bsp.visleafs}
            polys.extend(ps)
            folded_faces += 1

    # PVS bitmasks per leaf (bit k = leaf k visible); no vis row = all
    nleaf = min(bsp.visleafs + 1, len(bsp.leafs))
    all_bits = (1 << nleaf) - 2
    pvs_list = [all_bits] + [bsp.leaf_pvs(l) for l in range(1, nleaf)]
    pvs_list = [all_bits if p is None else p for p in pvs_list]
    sectors = build_sectors(polys, face_leaves, pvs_list)
    # light probes: one 64 u lattice per sector bbox, all sampled at once
    lf = LightFaces(bsp, style_values(ents))
    grids = []
    for ps in sectors:
        bb = [min(v[k] for p in ps for v in p.verts) for k in range(3)] + \
             [max(v[k] for p in ps for v in p.verts) for k in range(3)]
        grids.append(probe_grid(bb))
    pv = light_points(bsp, lf, np.concatenate([g[2] for g in grids])) if grids else []
    rend = []
    pc = 0
    for ps, (porg, pdims, pts) in zip(sectors, grids):
        vals = pv[pc:pc + len(pts)].astype(np.uint8).tobytes()
        pc += len(pts)
        blob, st, bbox = encode_rend(build_meshlets(ps), (porg, pdims, vals))
        leaves = 0
        for p in ps:
            for l in face_leaves.get(p.face, ()):
                leaves |= 1 << l
        rend.append({"blob": blob, "stats": st, "bbox": bbox, "leaves": leaves,
                     "sheets": tex_pages(tex, {p.tex for p in ps}),
                     "probes": len(pts)})

    movr = {}
    for mi in range(1, nmod):
        if sub_kind[mi] != SUB_MOVR:
            continue
        md = bsp.models[mi]
        ps = []
        for f in range(md[14], md[14] + md[15]):
            ps.extend(cook_face(bsp, f, tex))
        if not ps:
            sub_kind[mi] = SUB_NONE
            continue
        blob, st, bbox = encode_rend(build_meshlets(ps))
        movr[mi] = {"blob": blob, "stats": st, "bbox": bbox,
                    "sheets": tex_pages(tex, {p.tex for p in ps})}

    # only the pages some cooked face can bind; remap to TEXP order
    used = sorted({s for r in rend for s in r["sheets"]}
                  | {s for m in movr.values() for s in m["sheets"]})
    remap = {p: i for i, p in enumerate(used)}
    for t in tex:
        t["sheet"] = remap.get(t["sheet"], 0xFFFF)
    for r in rend:
        r["sheets"] = [remap[s] for s in r["sheets"]]
    for m in movr.values():
        m["sheets"] = [remap[s] for s in m["sheets"]]
    texp = [encode_texp(pack.pages[p]) for p in used]

    # vis table: leaf -> visible sectors (leaf 0 / no vis row: all)
    all_list = tuple(range(len(rend)))
    lists = {}
    leaf_list = []
    for leaf in range(nleaf):
        pvs = bsp.leaf_pvs(leaf) if leaf else None
        if pvs is None:
            vl = all_list
        else:
            vl = tuple(i for i, r in enumerate(rend) if r["leaves"] & pvs)
        leaf_list.append(lists.setdefault(vl, len(lists)))
    uniq = sorted(lists, key=lambda v: lists[v])

    # collision
    placements = submodel_placements(bsp, ents, False)
    traced = submodel_placements(bsp, ents, True)
    hg, coll, gid, model_range, pinned, w13 = build_coll(bsp, traced)
    mat = build_materials(bsp, gid, len(coll), mats) if mats is not None else None
    grid = build_grid(bsp, hg, gid, w13)
    csz = [len(b) for b in coll]
    base_b, base_nodes, base_planes = coll_baseline(bsp, hg)

    # movers' visibility: placement box touches a leaf in PVS(leaf)
    mover_leaves = {}
    for mi in movr:
        bits = 0
        for pmin, pmax in placements[mi]:
            for l in bsp.box_leafs(pmin, pmax):
                if 0 < l < nleaf:
                    bits |= 1 << l
        mover_leaves[mi] = bits

    # budget: per visleaf render + texture open sets (movers included),
    # per grid cell the collision set
    limits = read_limits()
    worst_r = worst_t = (0, -1, None)
    rsz = [len(r["blob"]) for r in rend]
    tsz = [len(t) for t in texp]
    for leaf in range(1, nleaf):
        vl = uniq[leaf_list[leaf]]
        rb = sum(rsz[s] for s in vl)
        pb = sum(rend[s]["stats"]["probe_bytes"] for s in vl)
        sh = set()
        for s in vl:
            sh.update(rend[s]["sheets"])
        mb = nm = 0
        for mi, bits in mover_leaves.items():
            if bits & pvs_list[leaf]:
                mb += len(movr[mi]["blob"])
                nm += 1
                sh.update(movr[mi]["sheets"])
        tb = sum(tsz[k] for k in sh)
        parts = {"sectors": len(vl), "rend": rb, "probe_part": pb, "movers": nm,
                 "movr": mb, "pages": len(sh)}
        worst_r = max(worst_r, (rb + mb, leaf, parts), key=lambda w: w[:2])
        worst_t = max(worst_t, (tb, leaf, parts), key=lambda w: w[:2])
    cb = coll_budget(grid, csz, pinned, w13, model_range, traced)
    over = []
    for key, what, (amount, where) in (
            ("HL64_POOL_RENDER_KB", "render open set", (worst_r[0], f"leaf {worst_r[1]}")),
            ("HL64_POOL_TEXTURE_KB", "texture open set", (worst_t[0], f"leaf {worst_t[1]}")),
            ("HL64_POOL_COLLISION_KB", "collision set", (cb["bytes"], f"cell {cb['cell_xyz']}"))):
        if amount > limits[key]:
            over.append(f"{what} {amount} B at {where} > {key} ({limits[key]} B)")

    # ---- chunks: REND, MOVR, TEXP, COLL, ENTS, VISR ----
    chunks = []  # (fourcc, blob)
    for r in rend:
        chunks.append((CHUNK_REND, r["blob"]))
    movr_chunk = {}
    for mi in sorted(movr):
        movr_chunk[mi] = len(chunks)
        chunks.append((CHUNK_MOVR, movr[mi]["blob"]))
    texp_first = len(chunks)
    for t in texp:
        chunks.append((CHUNK_TEXP, t))
    coll_first = len(chunks)
    for b in coll:
        chunks.append((CHUNK_COLL, b))
    ents_chunk = NO_CHUNK
    if hle is not None:
        ents_chunk = len(chunks)
        chunks.append((CHUNK_ENTS, hle + bytes(align(len(hle), 16) - len(hle))))
    visr = build_visr(bsp)
    visr_chunk = NO_CHUNK
    if visr is not None:
        visr_chunk = len(chunks)
        chunks.append((CHUNK_VISR, visr))
    if decl is not None:
        # found by fourcc (w64-format.md DECL): no directory index
        chunks.append((CHUNK_DECL, decl + bytes(align(len(decl), 16) - len(decl))))
    if len(chunks) >= NO_CHUNK:
        die(f"{len(chunks)} chunks > u16 chunk index")

    # ---- layer directory ----
    sheet_pool = []
    sect_recs = bytearray()
    for i, r in enumerate(rend):
        bb = r["bbox"]
        sect_recs += struct.pack(">6hHHHH", *bb, i, len(sheet_pool), len(r["sheets"]), 0)
        sheet_pool += r["sheets"]
    tex_recs = bytearray()
    for t in tex:
        tex_recs += struct.pack(">HBBBBHHH", t["sheet"], t["flags"], t["total"],
                                t["min"], t["max"], 0, t["next"], t["alt"])
    sub_recs = bytearray()
    for mi in range(nmod):
        if sub_kind[mi] == SUB_MOVR:
            bb = movr[mi]["bbox"]
            sub_recs += struct.pack(">HH6h", SUB_MOVR, movr_chunk[mi], *bb)
        else:
            sub_recs += struct.pack(">HH6h", sub_kind[mi], 0xFFFF, 0, 0, 0, 0, 0, 0)
    vis_pool = []
    vis_ofs = []
    for vl in uniq:
        vis_ofs.append(len(vis_pool))
        vis_pool.append(len(vl))
        vis_pool += vl
    leaf_recs = struct.pack(">%dI" % nleaf, *[vis_ofs[k] for k in leaf_list])
    model_recs = bytearray()
    for mi, md in enumerate(bsp.models):
        heads = []
        for h in range(4):
            k = head_key(md, h)
            heads.append(gid[k] if k >= 0 else k)
        f, c = model_range[mi]
        model_recs += struct.pack(">9f4iiHH4x", *md[0:9], *heads, md[13], f, c)
    leafc = struct.pack(">%db" % len(bsp.leafs), *[lf_[0] for lf_ in bsp.leafs])
    gcells = grid["cells"]
    cells_b = struct.pack(">%dH" % (2 * grid["n"]), *gcells.ravel().tolist())
    start_b = struct.pack(">%di" % len(grid["start"]), *grid["start"])
    lists_b = struct.pack(">%dH" % len(grid["pool"]), *grid["pool"])

    DIR_HDR = 96
    nchunks = len(chunks)
    parts = []
    cur = DIR_HDR

    def place(b, a=8):
        nonlocal cur
        cur = align(cur, a)
        o = cur
        parts.append((o, b))
        cur += len(b)
        return o

    chunkdir_ofs = place(bytes(16 * nchunks))
    sect_ofs = place(bytes(sect_recs))
    sheetpool_ofs = place(struct.pack(">%dH" % len(sheet_pool), *sheet_pool))
    tex_ofs = place(bytes(tex_recs))
    sub_ofs = place(bytes(sub_recs))
    leaf_ofs = place(leaf_recs)
    vispool_ofs = place(struct.pack(">%dH" % len(vis_pool), *vis_pool))
    model_ofs = place(bytes(model_recs))
    leafc_ofs = place(leafc)
    grid_ofs = place(bytes(32))
    cells_ofs = place(cells_b)
    start_ofs = place(start_b)
    lists_ofs = place(lists_b)
    mat_ofs = 0
    if mat is not None:
        mat_ofs = place(bytes(24))
        mat_start_ofs = place(mat["start"])
        mat_ent_ofs = place(mat["ents"])
        mat_mix_ofs = place(mat["mix"])
        mat_face_ofs = place(mat["faces"])
    dir_size = align(cur, 16)

    HDR = 64
    dir_base = HDR
    file_cur = align(dir_base + dir_size, 16)
    chunk_ofs = []
    for fourcc, b in chunks:
        chunk_ofs.append(file_cur)
        file_cur = align(file_cur + len(b), 16)
    file_size = file_cur

    d = bytearray(dir_size)
    for o, b in parts:
        d[o:o + len(b)] = b
    struct.pack_into(">HHHHHHHH", d, 0, len(rend), nleaf, len(tex), len(texp), nmod,
                     nchunks, texp_first, len(uniq))
    struct.pack_into(">IIIIIII", d, 16, chunkdir_ofs, sect_ofs, sheetpool_ofs, tex_ofs,
                     sub_ofs, leaf_ofs, vispool_ofs)
    struct.pack_into(">6H4I", d, 44, coll_first, len(coll), pinned, len(bsp.leafs),
                     ents_chunk, visr_chunk, model_ofs, leafc_ofs, grid_ofs, mat_ofs)
    if mat is not None:
        struct.pack_into(">4H4I", d, mat_ofs, mat["nent"], mat["nmix"], mat["nface"], 0,
                         mat_start_ofs, mat_ent_ofs, mat_mix_ofs, mat_face_ofs)
    struct.pack_into(">3h3HHHIII", d, grid_ofs, *grid["org"], *grid["dims"], GRID_CELL,
                     len(grid["start"]), cells_ofs, start_ofs, lists_ofs)
    for i, (fourcc, b) in enumerate(chunks):
        struct.pack_into(">4sIII", d, chunkdir_ofs + 16 * i, fourcc, chunk_ofs[i], len(b),
                         zlib.crc32(b) & 0xFFFFFFFF)

    wb = [min(r["bbox"][k] for r in rend) for k in range(3)] + \
         [max(r["bbox"][k + 3] for r in rend) for k in range(3)] if rend else [0] * 6
    h = bytearray(HDR)
    struct.pack_into(">4sHHIIII", h, 0, W64_MAGIC, W64_VERSION, W64_HF_COLL, file_size,
                     dir_base, dir_size, nchunks)
    struct.pack_into(">I", h, 24, zlib.crc32(bytes(d)) & 0xFFFFFFFF)
    struct.pack_into(">HHHH", h, 28, T3D_VERT_OUTPUT_SIZE, T3D_VERT_BUFFER, STYLE0_VALUE,
                     int(float(LIGHT_SCALE) * 100))
    struct.pack_into(">6h", h, 36, *wb)
    struct.pack_into(">HH", h, 48, MESHLET_MAX_VERTS, MESHLET_MAX_IDX)

    out = bytearray(file_size)
    out[0:HDR] = h
    out[dir_base:dir_base + dir_size] = d
    for (fourcc, b), o in zip(chunks, chunk_ofs):
        out[o:o + len(b)] = b
    out_path.write_bytes(out)

    def agg(items):
        s = defaultdict(int)
        for it in items:
            for k, v in it.items():
                s[k] += v
        return dict(s)

    def hist(sizes):
        bins = [0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768]
        out = {}
        for lo, hi in zip(bins, bins[1:] + [None]):
            n = sum(1 for s in sizes if s >= lo and (hi is None or s < hi))
            if n:
                out[f"{lo}-{hi}" if hi else f"{lo}+"] = n
        return out

    kind_sizes = defaultdict(list)
    for fourcc, b in chunks:
        kind_sizes[fourcc.decode()].append(len(b))
    rs = agg(r["stats"] for r in rend)
    ms = agg(m["stats"] for m in movr.values())
    sect_sizes = sorted(rsz)
    coll_total = sum(csz)
    rep = {
        "map": bsp_path.stem, "file_bytes": file_size, "dir_bytes": dir_size,
        "chunk_bytes": {k: sum(v) for k, v in kind_sizes.items()},
        "chunk_counts": {k: len(v) for k, v in kind_sizes.items()},
        "chunk_hist": {k: hist(v) for k, v in kind_sizes.items()},
        "sectors": len(rend), "rend": rs, "movr": ms, "movr_count": len(movr),
        "probes": sum(r["probes"] for r in rend),
        "folded_submodels": sum(1 for k in sub_kind if k == SUB_FOLDED),
        "folded_faces": folded_faces,
        "skipped_submodels": sum(1 for mi in range(1, nmod) if sub_kind[mi] == SUB_NONE),
        "texp_sheets": len(texp), "texp_bytes": sum(tsz),
        "sector_bytes_p50": sect_sizes[len(sect_sizes) // 2] if sect_sizes else 0,
        "sector_bytes_max": sect_sizes[-1] if sect_sizes else 0,
        "visleafs": nleaf - 1, "vis_lists": len(uniq),
        "vis_bytes": len(leaf_recs) + 2 * len(vis_pool),
        "worst_leaf_render": worst_r[0], "worst_leaf_render_leaf": worst_r[1],
        "worst_leaf_render_parts": worst_r[2], "worst_leaf_tex_parts": worst_t[2],
        "worst_leaf_tex": worst_t[0], "worst_leaf_tex_leaf": worst_t[1],
        "coll_bytes": coll_total, "coll_chunks": len(coll),
        "coll_pinned_chunks": pinned, "coll_pinned_bytes": sum(csz[:pinned]),
        "coll_baseline_bytes": base_b, "coll_baseline_nodes": base_nodes,
        "coll_baseline_planes": base_planes,
        "coll_ratio": round(coll_total / base_b, 4) if base_b else 0,
        "grid_dims": grid["dims"], "grid_origin": grid["org"],
        "grid_lists": grid["nlists"], "grid_pool_entries": len(grid["pool"]),
        "grid_start_entries": len(grid["start"]),
        "worst_cell_coll": cb,
        "pools": {k: v for k, v in limits.items()},
        "ents_bytes": len(hle) if hle is not None else 0,
        "visr_bytes": len(visr) if visr is not None else 0,
        "decl_bytes": len(decl) if decl is not None else 0,
        "missing_textures": missing,
        "mat_bytes": (24 + sum(len(mat[k]) for k in ("start", "ents", "mix", "faces"))
                      if mat is not None else 0),
        "mat_nodes": mat["nent"] if mat is not None else 0,
        "mat_mixed": mat["nmix"] if mat is not None else 0,
        "mat_faces": mat["nface"] if mat is not None else 0,
        "over_budget": over,
    }
    print(f"w64c: {out_path.name}: {file_size} B (dir {dir_size}) sectors={len(rend)} "
          f"meshlets={rs.get('meshlets', 0)} verts={rs.get('verts', 0)} tris={rs.get('tris', 0)} "
          f"styled={rs.get('styled', 0)} probes={rep['probes']} | movr={len(movr)} "
          f"({ms.get('bytes', 0)} B) folded={rep['folded_submodels']} | texp={len(texp)} "
          f"({sum(tsz)} B) | coll={len(coll)} ({coll_total} B, {rep['coll_ratio']}x base, "
          f"pinned {rep['coll_pinned_bytes']}) | worst leaf render {worst_r[0]} B "
          f"tex {worst_t[0]} B, worst cell coll {cb['bytes']} B | materials "
          f"{rep['mat_nodes']} nodes ({rep['mat_mixed']} mixed, {rep['mat_faces']} faces) "
          f"{rep['mat_bytes']} B")
    if mat is None:
        print("w64c: warning: no --materials: every surface reads as concrete",
              file=sys.stderr)
    if missing:
        print(f"w64c: warning: {len(missing)} textures not in {wci4_path.name}: "
              + ", ".join(missing), file=sys.stderr)
    return rep


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="w64c", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bsp", type=Path)
    ap.add_argument("wci4", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--hle", type=Path, default=None,
                    help="HLE1 entity blob (hlecook) for the ENTS chunk")
    ap.add_argument("--json", type=Path, default=None, help="write a size/budget report")
    ap.add_argument("--decl", type=Path, default=None,
                    help="the map's placed-decal sheet (DI4) for the DECL chunk")
    ap.add_argument("--no-budget", action="store_true",
                    help="report pool overruns instead of failing (sizing runs only)")
    ap.add_argument("--materials", type=Path, default=None,
                    help="sound/materials.txt (a loose file): per hull-0 node surface "
                         "materials")
    a = ap.parse_args(argv)
    try:
        hle = load_hle(a.hle) if a.hle else None
        decl = load_decl(a.decl) if a.decl else None
        mats = Materials.load(a.materials) if a.materials else None
        with np.errstate(all="ignore"):
            rep = cook(a.bsp, a.wci4, a.out, hle, mats, decl)
    except (InputError, MaterialsError) as e:
        error(str(e))
    except CookError as e:
        error(f"{a.bsp}: {e}")
    except (struct.error, IndexError, KeyError, ValueError, ArithmeticError,
            RecursionError) as e:
        error(f"{a.bsp}: malformed input ({type(e).__name__}: {e})")
    except OSError as e:
        error(f"{e.filename}: {e.strerror}")
    if a.json:
        a.json.write_text(json.dumps(rep, indent=1))
    if rep["over_budget"] and not a.no_budget:
        a.out.unlink()
        error(f"{a.bsp}: " + "; ".join(rep["over_budget"]))
    elif rep["over_budget"]:
        print("w64c: warning (--no-budget): " + "; ".join(rep["over_budget"]),
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
