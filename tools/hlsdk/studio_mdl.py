#***
#*
#*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
#*
#*	This product contains software technology licensed from Id
#*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
#*	All Rights Reserved.
#*
#*   Use, distribution, and modification of this source code and/or resulting
#*   object code is restricted to non-commercial enhancements to products from
#*   Valve LLC.  All other use, distribution, or modification is prohibited
#*   without written permission from Valve LLC.
#*
#****
# Half Life 1 SDK LICENSE: tools/hlsdk/LICENSE-HLSDK.
"""Studio model (IDST version 10) reader: the record layouts of the Half
Life 1 SDK's engine/studio.h and engine/studio_event.h (studiohdr_t,
mstudiobone_t, mstudiobonecontroller_t, mstudiobbox_t, mstudioseqgroup_t,
mstudioseqdesc_t, mstudioevent_t, mstudioattachment_t, mstudioanim_t /
mstudioanimvalue_t, mstudiobodyparts_t, mstudiotexture_t, mstudiomodel_t,
mstudiomesh_t) and its STUDIO_* constants, read into plain Python records.

Used by tools.studio.studiocook (docs/formats/studio-format.md), which
regroups the data into hl64's cooked layout. A model whose textures live in
<name>t.mdl and whose sequences live in <name>NN.mdl (seqgroup 1..) is read
from all of its files.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Callable, List, Optional, Tuple

IDSTUDIOHEADER = 0x54534449        # "IDST" little-endian
IDSEQGRPHEADER = 0x51534449        # "IDSQ"
STUDIO_VERSION = 10

MAXSTUDIOBONES = 128
MAXSTUDIOCONTROLLERS = 8

# texture flags
STUDIO_NF_FLATSHADE = 0x0001
STUDIO_NF_CHROME = 0x0002
STUDIO_NF_FULLBRIGHT = 0x0004
STUDIO_NF_NOMIPS = 0x0008
STUDIO_NF_ALPHA = 0x0010
STUDIO_NF_ADDITIVE = 0x0020
STUDIO_NF_MASKED = 0x0040

# motion flags
STUDIO_X, STUDIO_Y, STUDIO_Z = 0x0001, 0x0002, 0x0004
STUDIO_XR, STUDIO_YR, STUDIO_ZR = 0x0008, 0x0010, 0x0020
STUDIO_LX, STUDIO_LY, STUDIO_LZ = 0x0040, 0x0080, 0x0100
STUDIO_TYPES = 0x7FFF
STUDIO_RLOOP = 0x8000
STUDIO_LOOPING = 0x0001


class MdlError(Exception):
    pass


@dataclass
class Bone:
    name: str
    parent: int
    flags: int
    bonecontroller: Tuple[int, ...]
    value: Tuple[float, ...]
    scale: Tuple[float, ...]


@dataclass
class Controller:
    bone: int
    type: int
    start: float
    end: float
    rest: int
    index: int


@dataclass
class Hitbox:
    bone: int
    group: int
    bbmin: Tuple[float, float, float]
    bbmax: Tuple[float, float, float]


@dataclass
class Event:
    frame: int
    event: int
    type: int
    options: str


@dataclass
class Attachment:
    name: str
    type: int
    bone: int
    org: Tuple[float, float, float]


@dataclass
class Seq:
    label: str
    fps: float
    flags: int
    activity: int
    actweight: int
    numframes: int
    motiontype: int
    motionbone: int
    linearmovement: Tuple[float, float, float]
    bbmin: Tuple[float, float, float]
    bbmax: Tuple[float, float, float]
    numblends: int
    blendtype: Tuple[int, int]
    blendstart: Tuple[float, float]
    blendend: Tuple[float, float]
    seqgroup: int
    entrynode: int
    exitnode: int
    nodeflags: int
    nextseq: int
    events: List[Event] = field(default_factory=list)
    # anim[blend][bone][6]: None (offset 0: the bone's default value) or the
    # run-length value words (signed 16-bit) of that channel, up to the end
    # of the channel's last run (mstudioanimvalue_t, HLSDK layout).
    anim: List[List[List[Optional[List[int]]]]] = field(default_factory=list)


@dataclass
class Texture:
    name: str
    flags: int
    width: int
    height: int
    pixels: bytes                  # width * height palette indices
    palette: bytes                 # 768 bytes RGB


@dataclass
class Mesh:
    skinref: int
    tricmds: List[Tuple[bool, List[Tuple[int, int, int, int]]]]  # (strip?, [(v, n, s, t)])


@dataclass
class SubModel:
    name: str
    verts: List[Tuple[float, float, float]]
    vertbone: List[int]
    norms: List[Tuple[float, float, float]]
    normbone: List[int]
    meshes: List[Mesh]


@dataclass
class BodyPart:
    name: str
    base: int
    models: List[SubModel]


@dataclass
class Model:
    name: str
    flags: int
    eyeposition: Tuple[float, float, float]
    min: Tuple[float, float, float]
    max: Tuple[float, float, float]
    bbmin: Tuple[float, float, float]
    bbmax: Tuple[float, float, float]
    bones: List[Bone]
    controllers: List[Controller]
    hitboxes: List[Hitbox]
    seqs: List[Seq]
    attachments: List[Attachment]
    bodyparts: List[BodyPart]
    textures: List[Texture]
    skinref: List[List[int]]       # [family][skinref] -> texture index
    numtransitions: int
    transitions: bytes             # numtransitions^2


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("latin-1")


class _Buf:
    def __init__(self, data: bytes, what: str):
        self.d, self.what = data, what

    def need(self, ofs: int, n: int) -> None:
        if ofs < 0 or n < 0 or ofs + n > len(self.d):
            raise MdlError(f"{self.what}: read [{ofs}, {ofs + n}) outside {len(self.d)} B")

    def unpack(self, fmt: str, ofs: int):
        n = struct.calcsize("<" + fmt)
        self.need(ofs, n)
        return struct.unpack_from("<" + fmt, self.d, ofs)

    def i16(self, ofs: int) -> int:
        return self.unpack("h", ofs)[0]

    def u16(self, ofs: int) -> int:
        return self.unpack("H", ofs)[0]


_HDR = "ii64si3f3f3f3f3fi" + "i" * 34
_HDR_SIZE = struct.calcsize("<" + _HDR)


def _header(b: _Buf):
    v = b.unpack(_HDR, 0)
    ident, version, name, length = v[0], v[1], _cstr(v[2]), v[3]
    f = v[4:19]
    rest = v[19:]
    keys = ["flags", "numbones", "boneindex", "numbonecontrollers", "bonecontrollerindex",
            "numhitboxes", "hitboxindex", "numseq", "seqindex", "numseqgroups",
            "seqgroupindex", "numtextures", "textureindex", "texturedataindex",
            "numskinref", "numskinfamilies", "skinindex", "numbodyparts", "bodypartindex",
            "numattachments", "attachmentindex", "soundtable", "soundindex", "soundgroups",
            "soundgroupindex", "numtransitions", "transitionindex"]
    h = dict(zip(keys, rest))
    h.update(ident=ident, version=version, name=name, length=length,
             eyeposition=tuple(f[0:3]), min=tuple(f[3:6]), max=tuple(f[6:9]),
             bbmin=tuple(f[9:12]), bbmax=tuple(f[12:15]))
    return h


def _channel(b: _Buf, ofs: int, numframes: int) -> List[int]:
    """The value words of one channel from its first run to the run that
    covers the last frame (the walk StudioCalcBoneQuaterion does)."""
    out: List[int] = []
    k = 0
    pos = ofs
    while True:
        w = b.u16(pos)
        valid, total = w & 0xFF, w >> 8
        if total == 0:
            raise MdlError(f"{b.what}: anim run with total 0 at {pos}")
        out.append(b.i16(pos))
        for i in range(valid):
            out.append(b.i16(pos + 2 + 2 * i))
        k += total
        pos += 2 * (valid + 1)
        if k >= numframes:
            break
    return out


def read(path_of: Callable[[str], Optional[bytes]], name: str) -> Model:
    """Read models/<name>.mdl through path_of(rel) -> bytes or None (the
    texture file <name>t.mdl and sequence-group files <name>NN.mdl are
    looked up the same way)."""
    base = name[:-4] if name.lower().endswith(".mdl") else name
    data = path_of(base + ".mdl")
    if data is None:
        raise MdlError(f"{base}.mdl: missing")
    b = _Buf(data, base + ".mdl")
    h = _header(b)
    if h["ident"] != IDSTUDIOHEADER or h["version"] != STUDIO_VERSION:
        raise MdlError(f"{b.what}: not an IDST v10 model")

    bones = []
    for i in range(h["numbones"]):
        v = b.unpack("32sii6i6f6f", h["boneindex"] + i * 112)
        bones.append(Bone(_cstr(v[0]), v[1], v[2], tuple(v[3:9]), tuple(v[9:15]),
                          tuple(v[15:21])))
    ctls = []
    for i in range(h["numbonecontrollers"]):
        v = b.unpack("iiffii", h["bonecontrollerindex"] + i * 24)
        ctls.append(Controller(*v))
    hbs = []
    for i in range(h["numhitboxes"]):
        v = b.unpack("ii3f3f", h["hitboxindex"] + i * 32)
        hbs.append(Hitbox(v[0], v[1], tuple(v[2:5]), tuple(v[5:8])))
    atts = []
    for i in range(h["numattachments"]):
        v = b.unpack("32sii3f9f", h["attachmentindex"] + i * 88)
        atts.append(Attachment(_cstr(v[0]), v[1], v[2], tuple(v[3:6])))

    groups: List[Optional[_Buf]] = [b]
    for g in range(1, h["numseqgroups"]):
        gd = path_of(f"{base}{g:02d}.mdl")
        if gd is None:
            raise MdlError(f"{b.what}: sequence group file {base}{g:02d}.mdl missing")
        gb = _Buf(gd, f"{base}{g:02d}.mdl")
        if gb.unpack("i", 0)[0] != IDSEQGRPHEADER:
            raise MdlError(f"{gb.what}: not an IDSQ file")
        groups.append(gb)

    seqs = []
    for i in range(h["numseq"]):
        o = h["seqindex"] + i * 176
        v = b.unpack("32sfiiiiiiiiii3fii3f3fii2i2f2fiiiiii", o)
        s = Seq(label=_cstr(v[0]), fps=v[1], flags=v[2], activity=v[3], actweight=v[4],
                numframes=v[7], motiontype=v[10], motionbone=v[11],
                linearmovement=tuple(v[12:15]), bbmin=tuple(v[17:20]), bbmax=tuple(v[20:23]),
                numblends=v[23], blendtype=tuple(v[25:27]), blendstart=tuple(v[27:29]),
                blendend=tuple(v[29:31]), seqgroup=v[32], entrynode=v[33], exitnode=v[34],
                nodeflags=v[35], nextseq=v[36])
        numevents, eventindex, animindex = v[5], v[6], v[24]
        for e in range(numevents):
            ev = b.unpack("iii64s", eventindex + e * 76)
            s.events.append(Event(ev[0], ev[1], ev[2], _cstr(ev[3])))
        if not (0 <= s.seqgroup < len(groups)):
            raise MdlError(f"{b.what}: sequence {s.label}: bad seqgroup {s.seqgroup}")
        gb = groups[s.seqgroup]
        for bl in range(max(1, s.numblends)):
            per_bone = []
            for bi in range(h["numbones"]):
                ao = animindex + (bl * h["numbones"] + bi) * 12
                offs = gb.unpack("6H", ao)
                per_bone.append([None if off == 0 else _channel(gb, ao + off, s.numframes)
                                 for off in offs])
            s.anim.append(per_bone)
        seqs.append(s)

    # textures: in this file, or in <name>t.mdl when this one has none
    tb = b
    th = h
    if h["numtextures"] == 0:
        td = path_of(base + "t.mdl")
        if td is None:
            raise MdlError(f"{b.what}: no textures and no {base}t.mdl")
        tb = _Buf(td, base + "t.mdl")
        th = _header(tb)
        if th["ident"] != IDSTUDIOHEADER:
            raise MdlError(f"{tb.what}: not an IDST file")
    texs = []
    for i in range(th["numtextures"]):
        v = tb.unpack("64siiii", th["textureindex"] + i * 80)
        w, hh, idx = v[2], v[3], v[4]
        tb.need(idx, w * hh + 768)
        texs.append(Texture(_cstr(v[0]), v[1], w, hh, tb.d[idx:idx + w * hh],
                            tb.d[idx + w * hh:idx + w * hh + 768]))
    skinref = []
    for f in range(th["numskinfamilies"]):
        skinref.append([tb.i16(th["skinindex"] + 2 * (f * th["numskinref"] + r))
                        for r in range(th["numskinref"])])

    bps = []
    for p in range(h["numbodyparts"]):
        v = b.unpack("64siii", h["bodypartindex"] + p * 76)
        models = []
        for m in range(v[1]):
            mv = b.unpack("64sifiiiiiiiiii", v[3] + m * 112)
            nummesh, meshindex, numverts, vertinfo, vertindex = mv[3], mv[4], mv[5], mv[6], mv[7]
            numnorms, norminfo, normindex = mv[8], mv[9], mv[10]
            verts = [b.unpack("3f", vertindex + 12 * k) for k in range(numverts)]
            b.need(vertinfo, numverts)
            b.need(norminfo, numnorms)
            vbone = list(b.d[vertinfo:vertinfo + numverts])
            norms = [b.unpack("3f", normindex + 12 * k) for k in range(numnorms)]
            nbone = list(b.d[norminfo:norminfo + numnorms])
            meshes = []
            for k in range(nummesh):
                numtris, triindex, sref, _, _ = b.unpack("iiiii", meshindex + 20 * k)
                cmds = []
                pos = triindex
                while True:
                    c = b.i16(pos)
                    pos += 2
                    if c == 0:
                        break
                    n = -c if c < 0 else c
                    pts = []
                    for _ in range(n):
                        pts.append(b.unpack("4h", pos))
                        pos += 8
                    cmds.append((c > 0, pts))
                meshes.append(Mesh(sref, cmds))
            models.append(SubModel(_cstr(mv[0]), verts, vbone, norms, nbone, meshes))
        bps.append(BodyPart(_cstr(v[0]), v[2], models))

    nt = h["numtransitions"]
    b.need(h["transitionindex"], nt * nt)
    trans = b.d[h["transitionindex"]:h["transitionindex"] + nt * nt]
    return Model(name=h["name"], flags=h["flags"], eyeposition=h["eyeposition"], min=h["min"],
                 max=h["max"], bbmin=h["bbmin"], bbmax=h["bbmax"], bones=bones,
                 controllers=ctls, hitboxes=hbs, seqs=seqs, attachments=atts, bodyparts=bps,
                 textures=texs, skinref=skinref, numtransitions=nt, transitions=trans)
