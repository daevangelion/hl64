# SPDX-License-Identifier: MIT
"""addramp: the additive ramp of a 16-entry RGBA5551 TLUT (render-fx.md 2a).

The RDP blender keeps the low 8 bits of in x a + mem x b, so hl64 draws
every additive item through the alpha blender as c = n x a (n a unit
colour, a its brightness): out = c + mem x (1 - a) <= 255. A CI4 texture
can only take a per-texel alpha from its TLUT, so an additive texture binds
an IA16 "ramp" TLUT: A = the entry's brightness, I = its place between two
unit colours lo (ENV) and hi (PRIM) that the combiner interpolates
(rgb = ENV + (PRIM - ENV) x I, a = A x PRIM alpha).

The cook fits the ramp once per palette (sprites, additive brush pages,
additive studio skins); the cart only binds it. Cooked layout, 40 bytes
(engine/render/addblend.h add_ramp_t, big-endian):

  0   u16 ia[16]   IA16 TLUT, I in the high byte
  32  u8  lo[3]    ramp end ENV (unit colour)
  35  u8  hi[3]    ramp end PRIM (unit colour)
  38  u8  pad[2]

Fit: the unit colours of the lit entries (alpha bit set, not black) are the
candidate ends; a pair (lo, hi) (one colour when lo = hi) scores the sum
over lit entries of brightness x the squared residual of the best
non-negative x lo + y hi ~ c (normal equations; on the cone's edge one end
alone). Search: the best single end, then alternately the best hi for lo
held and the best lo for hi held until neither moves. On the cooked sprites
this picks the same pair as trying every pair. A = x + y (clamped to 1),
I = y / (x + y).

Index-alpha sprites (SPR texformat INDEXALPHA: alpha = palette index, colour
= the palette's last colour) bind the same 40-byte record built by
`indexalpha`: ia = (I 255, A level), lo = hi = the colour itself (not a unit
colour; the renderer derives the draw's ends from it).
"""
from __future__ import annotations

import struct
from typing import List, Sequence, Tuple

SIZE = 40


def c8(v: int) -> int:
    """5-bit channel to 8 bits as the RDP expands an RGBA5551 texel."""
    return (v << 3) | (v >> 2)


def byte(v: float) -> int:
    return 0 if v <= 0 else 255 if v >= 1 else int(v * 255.0 + 0.5)


def _dot(a: Sequence[float], b: Sequence[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class _Fit:
    def __init__(self, tlut: Sequence[int]):
        self.c: List[List[float]] = []     # lit entries: colour 0..1
        self.w: List[float] = []           # brightness
        self.cc: List[float] = []          # |c|^2
        self.ent: List[int] = []           # TLUT index
        self.q: List[Tuple[int, int, int]] = []   # candidate unit colours, 8-bit
        n: List[List[float]] = []
        for i, t in enumerate(tlut):
            c = (c8((t >> 11) & 31), c8((t >> 6) & 31), c8((t >> 1) & 31))
            m = max(c)
            if not (t & 1) or not m:
                continue
            self.ent.append(i)
            self.w.append(m / 255.0)
            col = [k / 255.0 for k in c]
            self.c.append(col)
            self.cc.append(_dot(col, col))
            q = tuple((k * 255 + m // 2) // m for k in c)
            if q not in self.q:
                self.q.append(q)
                n.append([k / m for k in c])
        nc = len(self.q)
        self.nn = [[_dot(n[a], n[b]) for b in range(nc)] for a in range(nc)]
        self.inv = [1.0 / self.nn[a][a] for a in range(nc)]
        self.cn = [[_dot(cj, n[a]) for a in range(nc)] for cj in self.c]

    def idet(self, a: int, b: int) -> float:
        """1 / det of the pair's normal equations; 0 when the ends are parallel."""
        nn = self.nn
        det = nn[a][a] * nn[b][b] - nn[a][b] * nn[a][b]
        return 1.0 / det if det > 1e-4 * nn[a][a] * nn[b][b] else 0.0

    def fit2(self, j: int, a: int, b: int, idet: float) -> Tuple[float, float, float]:
        """Best non-negative x n_a + y n_b ~ c_j: (x, y, squared residual)."""
        cu, cv, uv = self.cn[j][a], self.cn[j][b], self.nn[a][b]
        if idet > 0:
            p = (cu * self.nn[b][b] - cv * uv) * idet
            q = (cv * self.nn[a][a] - cu * uv) * idet
            if p >= 0 and q >= 0:
                return p, q, self.cc[j] - p * cu - q * cv
        s = cu * self.inv[a] if cu > 0 else 0.0
        best = (s, 0.0, self.cc[j] - s * cu)
        s = cv * self.inv[b] if cv > 0 else 0.0
        e = self.cc[j] - s * cv
        return (0.0, s, e) if e < best[2] else best

    def score(self, a: int, b: int, lim: float) -> float:
        """The pair's brightness-weighted error, abandoned once past lim."""
        idet, e = self.idet(a, b), 0.0
        for j in range(len(self.c)):
            if e >= lim:
                break
            e += self.w[j] * self.fit2(j, a, b, idet)[2]
        return e


def ramp(tlut: Sequence[int]) -> bytes:
    """The 40-byte ramp record of a 16-entry RGBA5551 TLUT (native values);
    entries with the alpha bit clear or black get A = 0."""
    if len(tlut) != 16:
        raise ValueError("TLUT of %d entries" % len(tlut))
    f = _Fit(tlut)
    ia = [0] * 16
    if not f.c:
        return pack(ia, (0, 0, 0), (0, 0, 0))
    best, lo, hi = float("inf"), 0, 0
    for a in range(len(f.q)):
        e = f.score(a, a, best)
        if e < best:
            best, lo, hi = e, a, a
    moved = True
    while moved:
        moved = False
        for a in range(len(f.q)):
            e = f.score(lo, a, best)
            if e < best:
                best, hi, moved = e, a, True
        for a in range(len(f.q)):
            e = f.score(a, hi, best)
            if e < best:
                best, lo, moved = e, a, True
    idet = f.idet(lo, hi)
    for j, i in enumerate(f.ent):
        x, y, _ = f.fit2(j, lo, hi, idet)
        s = x + y
        ia[i] = byte(y / s if s > 0 else 0) << 8 | byte(s)
    return pack(ia, f.q[lo], f.q[hi])


def indexalpha(levels: Sequence[int], colour: Sequence[int]) -> bytes:
    """The record of an index-alpha CI4: entry k has alpha levels[k] (0..255)
    and the one colour (8-bit RGB) of the sprite."""
    if len(levels) != 16:
        raise ValueError("%d levels" % len(levels))
    return pack([0xFF00 | (v & 255) for v in levels], colour, colour)


def pack(ia: Sequence[int], lo: Sequence[int], hi: Sequence[int]) -> bytes:
    return struct.pack(">16H3B3B2x", *ia, *lo, *hi)


def unpack(b: bytes) -> Tuple[List[int], Tuple[int, ...], Tuple[int, ...]]:
    v = struct.unpack(">16H3B3B2x", b[:SIZE])
    return list(v[:16]), v[16:19], v[19:22]
