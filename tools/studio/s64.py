# SPDX-License-Identifier: MIT
"""Reader and validator of the cooked studio files (docs/formats/
studio-format.md): `.s64` (info block + draw mesh) and `.a64` (animation
chunks). Used by the cook (self-check), tools/restab (event resources) and
the tests."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Tuple

MODEL_MAGIC = 0x5336344D     # S64M
INFO_MAGIC = 0x53494E46      # SINF
MESH_MAGIC = 0x534D5348      # SMSH
ANIM_MAGIC = 0x41363441      # A64A
VERSION = 2                  # 2: textures may carry the additive ramp (TEXF_RAMP)
F_CHROME, F_VIEW = 1, 2
TEXF_FENCE, TEXF_RAMP = 1, 16   # texture flags (w64_texp_t.flags; w64-format.md TEXP)
INFO_HDR = 128
MESH_HDR = 64
SEQ_SIZE = 96
EVENT_SIZE = 12


class S64Error(Exception):
    pass


@dataclass
class EventRef:
    seq: int
    index: int          # event table index
    event: int
    frame: int
    options: str
    res_at: int         # byte offset of the `res` field in the .s64 file


@dataclass
class Info:
    ofs: int
    numbones: int
    numseq: int
    nsub: int
    labels: List[str]
    frames: List[int]
    anim: List[Tuple[int, int]]
    events: List[EventRef]
    numblends: List[int]


def _need(cond: bool, what: str) -> None:
    if not cond:
        raise S64Error(what)


def _str(b: bytes, pool: int, size: int, o: int) -> str:
    _need(o < size, f"string {o} outside the pool")
    end = b.find(b"\0", pool + o, pool + size)
    _need(end >= 0, f"string {o} not NUL-terminated")
    return b[pool + o:end].decode("latin-1")


def read_model(b: bytes) -> Info:
    _need(len(b) >= 64, "short file")
    magic, ver, flags, io, isz, mo, msz, asz, acrc = struct.unpack_from(">IHHIIIIII", b, 0)
    _need(magic == MODEL_MAGIC and ver == VERSION, "bad magic/version")
    _need(io == 64 and isz % 16 == 0 and mo == io + isz and msz % 16 == 0
          and mo + msz == len(b), "bad block layout")
    i = io
    h = struct.unpack_from(">IBBBBHBBI15fHH9I3I", b, i)
    _need(h[0] == INFO_MAGIC, "bad info magic")
    nb, nctl, nhb, natt, nseq, nbp, ntr = h[1:8]
    nev, nsub = h[24], h[25]
    bone_o, ctl_o, hb_o, att_o, seq_o, ev_o, bp_o, tr_o, str_o = h[26:35]
    str_size = h[35]

    def inside(o, n, what):
        _need(o + n <= isz, f"info {what} outside the block")
    inside(bone_o, 56 * nb, "bones")
    inside(ctl_o, 16 * nctl, "controllers")
    inside(hb_o, 28 * nhb, "hitboxes")
    inside(att_o, 16 * natt, "attachments")
    inside(seq_o, SEQ_SIZE * nseq, "sequences")
    inside(ev_o, EVENT_SIZE * nev, "events")
    inside(bp_o, 8 * nbp, "body parts")
    inside(tr_o, ntr * ntr, "transitions")
    inside(str_o, str_size, "strings")
    _need(str_size >= 1 and b[i + str_o] == 0, "string 0 is not empty")
    for k in range(nb):
        par = struct.unpack_from(">b", b, i + bone_o + 56 * k)[0]
        _need(-1 <= par < k, f"bone {k} parent {par}")
    for k in range(nctl):
        _need(struct.unpack_from(">b", b, i + ctl_o + 16 * k)[0] < nb, "controller bone")
    for k in range(nhb):
        _need(b[i + hb_o + 28 * k] < nb, "hitbox bone")
    for k in range(natt):
        _need(b[i + att_o + 16 * k] < nb, "attachment bone")
    for k in range(nbp):
        nm, base, sf, _ = struct.unpack_from(">HHHH", b, i + bp_o + 8 * k)
        _need(sf + nm <= nsub and base >= 1, f"body part {k}")
    labels, frames, anim, events, blends = [], [], [], [], []
    for k in range(nseq):
        so = i + seq_o + SEQ_SIZE * k
        lab, nbl = struct.unpack_from(">HB", b, so)
        nfr, nevs, evf = struct.unpack_from(">HHH", b, so + 16)
        aofs, asz2 = struct.unpack_from(">II", b, so + 88)
        _need(evf + nevs <= nev, f"sequence {k} events")
        _need(nbl >= 1, f"sequence {k} blends")
        labels.append(_str(b, i + str_o, str_size, lab))
        frames.append(nfr)
        anim.append((aofs, asz2))
        blends.append(nbl)
        for e in range(evf, evf + nevs):
            eo = i + ev_o + EVENT_SIZE * e
            fr, ev, opt, _ = struct.unpack_from(">iiHH", b, eo)
            events.append(EventRef(k, e, ev, fr, _str(b, i + str_o, str_size, opt), eo + 10))
    # mesh
    m = mo
    mh = struct.unpack_from(">IHHHHHHBBBBHH10I", b, m)
    _need(mh[0] == MESH_MAGIC, "bad mesh magic")
    msub, nruns, nml, nld, nslots, ntris, nbs, _, nsr, nfam, ntex = mh[1:12]
    bs_o, sub_o, run_o, ml_o, ld_o, v_o, t_o, sk_o, tx_o, nbn_o = mh[13:23]
    _need(msub == nsub, "mesh submodels != info")
    _need(nslots % 2 == 0, "odd slot count")

    def minside(o, n, what):
        _need(o + n <= msz, f"mesh {what} outside the block")
    minside(bs_o, nbs, "bslots")
    minside(sub_o, 4 * msub, "submodels")
    minside(run_o, 8 * nruns, "runs")
    minside(ml_o, 8 * nml, "meshlets")
    minside(ld_o, 4 * nld, "loads")
    minside(v_o, 16 * nslots, "verts")
    minside(t_o, 3 * ntris, "tris")
    minside(sk_o, 2 * nsr * nfam, "skins")
    minside(tx_o, 4 * ntex, "textures")
    _need(v_o % 16 == 0, "verts not 16-aligned")
    for k in range(nbs):
        _need(b[m + bs_o + k] < nb, "bslot bone")
    for k in range(msub):
        rf, rc = struct.unpack_from(">HH", b, m + sub_o + 4 * k)
        _need(rf + rc <= nruns, f"submodel {k} runs")
    slot = 0
    for k in range(nruns):
        sr, mf, mc, _ = struct.unpack_from(">HHHH", b, m + run_o + 8 * k)
        _need(sr < nsr and mf + mc <= nml, f"run {k}")
    for k in range(nml):
        lf, nl, nv, tf, nt = struct.unpack_from(">HBBHH", b, m + ml_o + 8 * k)
        _need(lf + nl <= nld and tf + nt <= ntris and 1 <= nv <= 64, f"meshlet {k}")
        cache = 0
        for li in range(lf, lf + nl):
            bsl, c, cnt = struct.unpack_from(">BBB", b, m + ld_o + 4 * li)
            _need(bsl < nbs and c == cache and cnt >= 1, f"meshlet {k} load {li}")
            cache += cnt
            slot += cnt + (cnt & 1)
        _need(cache == nv, f"meshlet {k} loads cover {cache} of {nv}")
        for ti in range(3 * tf, 3 * (tf + nt)):
            _need(b[m + t_o + ti] < nv, f"meshlet {k} tri index")
    _need(slot == nslots, "loads do not cover the slots")
    for k in range(nsr * nfam):
        _need(struct.unpack_from(">H", b, m + sk_o + 2 * k)[0] < ntex, "skin entry")
    for k in range(ntex):
        to = struct.unpack_from(">I", b, m + tx_o + 4 * k)[0]
        _need(to % 16 == 0, "texture alignment")
        w, hh, tf, ci4o = struct.unpack_from(">HHB3xI", b, m + to)
        first = 88 if tf & TEXF_RAMP else 48     # after the TLUT, or the 40-byte ramp
        _need(w and hh and not (w & (w - 1)) and not (hh & (hh - 1)) and ci4o == first,
              f"texture {k} shape")
        minside(to, first + w * hh // 2, f"texture {k}")
    if flags & F_CHROME:
        minside(nbn_o, nslots, "normal bones")
    return Info(io, nb, nseq, nsub, labels, frames, anim, events, blends)


def read_anim(a: bytes, info: Info) -> None:
    _need(len(a) >= 16, "short anim file")
    magic, nseq, nb, size, _ = struct.unpack_from(">IHHII", a, 0)
    _need(magic == ANIM_MAGIC and nseq == info.numseq and nb == info.numbones
          and size == len(a), "bad anim header")
    for k, (o, n) in enumerate(info.anim):
        _need(o % 16 == 0 and n % 16 == 0 and o + n <= size, f"chunk {k} range")
        nent = info.numblends[k] * nb * 6
        _need(2 * nent <= n, f"chunk {k} table")
        for e in range(nent):
            w = struct.unpack_from(">H", a, o + 2 * e)[0]
            _need(w == 0 or nent <= w < n // 2, f"chunk {k} channel {e} offset")
