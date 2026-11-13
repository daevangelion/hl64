# SPDX-License-Identifier: MIT
"""Compiled route bytecode (.rbc): docs/formats/route-bytecode.md.

Opcode table, writer and a hostile-input-safe reader. The reader checks the
header, exact sizes, every opcode's operands (ids against the campaign.rtb
when one is given, finite floats, string offsets at string starts), the
debug line table and the CRC, and raises RbcError(offset, rule) only.
"""
from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from ..restab import rtb

MAGIC = 0x52424331          # 'RBC1'
VERSION = 1
HEADER_SIZE = 48
MAX_OPS = 0xFFFF
MAX_LABEL = 47

# operand word kinds: u = u32, f = finite f32, r = f32 >= 0, c = class id (u32)
OPS: Dict[int, Tuple[str, str]] = {
    0x01: ("WAIT", "u"),
    0x02: ("WAIT_MAP", "u"),
    0x03: ("WAIT_NEAR", "fffru"),
    0x04: ("WAIT_MOVED", "u"),
    0x05: ("WAIT_GONE", "u"),
    0x06: ("WAIT_CLASS", "cfffru"),
    0x07: ("WAIT_GLOBAL", "u"),
    0x08: ("WAIT_END", "u"),
    0x09: ("LOOK", "ff"),
    0x0A: ("LOOK_AT", "u"),
    0x0B: ("SETPOS", "fffff"),
    0x0C: ("BUTTONS", "u"),
    0x0D: ("IMPULSE", ""),
    0x0E: ("DROP", "cfff"),
    0x0F: ("GOD", ""),
    0x10: ("SHOT", ""),
    0x11: ("STATS", ""),
    0x12: ("PASS", ""),
    0x13: ("START", ""),
}
OPCODE = {name: code for code, (name, _) in OPS.items()}

# HLSDK in_buttons.h bit values (the usercmd buttons word)
BUTTONS = {
    "attack": 1 << 0, "jump": 1 << 1, "duck": 1 << 2, "forward": 1 << 3,
    "back": 1 << 4, "use": 1 << 5, "left": 1 << 7, "right": 1 << 8,
    "moveleft": 1 << 9, "moveright": 1 << 10, "attack2": 1 << 11,
    "speed": 1 << 12, "reload": 1 << 13,
}
BUTTON_MASK = 0x3FFF & ~(1 << 6)

GLOBAL_OFF, GLOBAL_ON, GLOBAL_DEAD = 0, 1, 2
LOOKAT_TARGET, LOOKAT_CLASS = 0, 1


class RbcError(Exception):
    def __init__(self, offset: int, rule: str):
        super().__init__(f"rbc: offset {offset}: {rule}")
        self.offset = offset
        self.rule = rule


@dataclass
class Op:
    code: int
    a: int = 0
    b: int = 0
    words: Tuple = ()          # ints for u/c, floats for f/r
    line: int = 0


@dataclass
class Program:
    rtb_id: int
    source: str
    ops: List[Op]
    strings: List[str]          # labels; Op.b of SHOT/STATS indexes by offset


def write(rtb_id: int, source: str, ops: List[Op]) -> bytes:
    """Serialise; SHOT/STATS ops carry their label as words[0] (a str)."""
    if not ops or len(ops) > MAX_OPS:
        raise ValueError("a route needs 1..65535 ops")
    pool = bytearray()
    at: Dict[str, int] = {}

    def add(s: str) -> int:
        if s not in at:
            at[s] = len(pool)
            pool.extend(s.encode("ascii") + b"\0")
        if at[s] > 0xFFFF:
            raise ValueError("string pool over 64 KiB")
        return at[s]

    src = add(source)
    code = bytearray()
    for op in ops:
        name, kinds = OPS[op.code]
        b = op.b
        words = op.words
        if op.code in (OPCODE["SHOT"], OPCODE["STATS"]):
            b, words = add(words[0]), ()
        code += struct.pack(">BBH", op.code, op.a, b)
        if len(words) != len(kinds):
            raise ValueError(f"{name}: {len(words)} operand words, want {len(kinds)}")
        for k, w in zip(kinds, words):
            code += struct.pack(">f" if k in "fr" else ">I", w)
    lines = b"".join(struct.pack(">H", op.line) for op in ops)
    lines += bytes(-len(lines) % 4)
    code_ofs = HEADER_SIZE
    lines_ofs = code_ofs + len(code)
    str_ofs = lines_ofs + len(lines)
    body = bytes(code) + lines + bytes(pool)
    file_size = HEADER_SIZE + len(body)
    hdr = struct.pack(">IHHIIIHHIIIIIHH", MAGIC, VERSION, 0, rtb_id, file_size,
                      zlib.crc32(body) & 0xFFFFFFFF, len(ops), 0, code_ofs, len(code),
                      lines_ofs, str_ofs, len(pool), src, 0)
    return hdr + body


def _cstr(pool: bytes, base: int, off: int, what: str) -> str:
    if not 0 <= off < len(pool) or (off and pool[off - 1] != 0):
        raise RbcError(base, f"{what}: string offset {off} is not a string start")
    end = pool.find(b"\0", off)
    if end < 0:
        raise RbcError(base + off, f"{what}: string not NUL-terminated")
    s = pool[off:end]
    if not all(0x20 < c < 0x7F for c in s):
        raise RbcError(base + off, f"{what}: string not printable ASCII")
    return s.decode("ascii")


def read(data: bytes, table: Optional[rtb.Table] = None) -> Program:
    """Validate and decode; with `table`, ids are checked against it and the
    rtb build id must match."""
    n = len(data)
    if n < HEADER_SIZE:
        raise RbcError(0, "file shorter than the header")
    (magic, version, flags, rtb_id, file_size, crc, nops, res0, code_ofs, code_size,
     lines_ofs, str_ofs, str_size, src, res1) = struct.unpack_from(
        ">IHHIIIHHIIIIIHH", data, 0)
    if magic != MAGIC:
        raise RbcError(0, "bad magic")
    if version != VERSION:
        raise RbcError(4, f"unsupported version {version}")
    if flags or res0 or res1:
        raise RbcError(6, "reserved header field not zero")
    if file_size != n:
        raise RbcError(12, f"file_size {file_size} != bytes present {n}")
    if zlib.crc32(data[HEADER_SIZE:]) & 0xFFFFFFFF != crc:
        raise RbcError(16, "CRC mismatch")
    if nops == 0:
        raise RbcError(20, "no ops")
    lines_size = (2 * nops + 3) & ~3
    if (code_ofs != HEADER_SIZE or lines_ofs != code_ofs + code_size
            or str_ofs != lines_ofs + lines_size or str_ofs + str_size != n
            or code_size % 4):
        raise RbcError(24, "section layout does not follow the header")
    if table is not None and rtb_id != table.build_id:
        raise RbcError(8, f"compiled against rtb {rtb_id:08x}, loaded {table.build_id:08x}")
    pool = data[str_ofs:n]
    if not pool or pool[-1] != 0:
        raise RbcError(str_ofs, "string pool does not end with NUL")
    if any(data[lines_ofs + 2 * nops:str_ofs]):
        raise RbcError(lines_ofs + 2 * nops, "line table padding not zero")
    source = _cstr(pool, str_ofs, src, "source name")

    nmaps = len(table.map_names) if table else None
    ntarg = len(table.targets) if table else None
    ncls = len(table.class_names) if table else None
    nglob = len(table.global_names) if table else None

    def lim(at: int, v: int, cap: Optional[int], what: str) -> None:
        if cap is not None and v >= cap:
            raise RbcError(at, f"{what} {v} not in the rtb (count {cap})")

    ops: List[Op] = []
    labels: List[str] = []
    p = code_ofs
    end = code_ofs + code_size
    for i in range(nops):
        if p + 4 > end:
            raise RbcError(p, f"op {i}: runs past the code")
        code, a, b = struct.unpack_from(">BBH", data, p)
        if code not in OPS:
            raise RbcError(p, f"op {i}: unknown opcode {code:#x}")
        name, kinds = OPS[code]
        if p + 4 + 4 * len(kinds) > end:
            raise RbcError(p, f"op {i} {name}: operands run past the code")
        words = []
        for j, k in enumerate(kinds):
            at = p + 4 + 4 * j
            if k in "fr":
                (v,) = struct.unpack_from(">f", data, at)
                if not math.isfinite(v) or (k == "r" and v < 0):
                    raise RbcError(at, f"op {i} {name}: bad float operand")
            else:
                (v,) = struct.unpack_from(">I", data, at)
                if k == "c":
                    if v > 0xFFFF:
                        raise RbcError(at, f"op {i} {name}: class id over u16")
                    lim(at, v, ncls, "class")
            words.append(v)
        (line,) = struct.unpack_from(">H", data, lines_ofs + 2 * i)
        if line == 0:
            raise RbcError(lines_ofs + 2 * i, f"op {i}: source line 0")
        # per-opcode a / b rules
        ok_a = a == 0
        if name in ("WAIT_MAP", "WAIT_NEAR", "WAIT_CLASS", "SETPOS", "DROP", "START"):
            lim(p, b, nmaps, "map")
            if name == "SETPOS":
                ok_a = a <= 1
            if name == "START" and i:
                raise RbcError(p, f"op {i}: START is not the first op")
        elif name in ("WAIT_MOVED", "WAIT_GONE"):
            lim(p, b, ntarg, "target")
        elif name == "WAIT_GLOBAL":
            ok_a = a <= GLOBAL_DEAD
            lim(p, b, nglob, "global")
        elif name == "LOOK_AT":
            ok_a = a in (LOOKAT_TARGET, LOOKAT_CLASS)
            if words[0] > 0xFFFF:
                raise RbcError(p + 4, f"op {i} LOOK_AT: map over u16")
            lim(p + 4, words[0], nmaps, "map")
            if a == LOOKAT_TARGET:
                lim(p, b, ntarg, "target")
                if table is not None and table.targets[b][0] != words[0]:
                    raise RbcError(p, f"op {i} LOOK_AT: target is not in map {words[0]}")
            else:
                lim(p, b, ncls, "class")
        elif name == "BUTTONS":
            if b & ~BUTTON_MASK or words[0] & ~BUTTON_MASK or b & words[0]:
                raise RbcError(p, f"op {i} BUTTONS: bad masks")
        elif name == "IMPULSE":
            ok_a = True
        elif name == "GOD":
            ok_a = a <= 1
        elif name in ("SHOT", "STATS"):
            labels.append(_cstr(pool, str_ofs, b, f"op {i} {name} label"))
            if len(labels[-1]) > MAX_LABEL:
                raise RbcError(p, f"op {i} {name}: label longer than {MAX_LABEL}")
            words = [labels[-1]]
        elif name == "PASS":
            if i != nops - 1:
                raise RbcError(p, f"op {i}: PASS before the last op")
        if name in ("WAIT", "WAIT_END", "LOOK", "IMPULSE", "GOD", "PASS") and b != 0:
            raise RbcError(p, f"op {i} {name}: b must be 0")
        if not ok_a:
            raise RbcError(p, f"op {i} {name}: bad a operand {a}")
        ops.append(Op(code, a, b, tuple(words), line))
        p += 4 + 4 * len(kinds)
    if p != end:
        raise RbcError(p, "code bytes after the last op")
    if ops[-1].code != OPCODE["PASS"]:
        raise RbcError(end - 4, "last op is not PASS")
    return Program(rtb_id, source, ops, labels)


def disasm(prog: Program, table: Optional[rtb.Table] = None) -> str:
    """One line per op: index, source line, mnemonic, operands (names when
    the rtb is given)."""
    def m(i: int) -> str:
        return table.map_names[i] if table else f"map#{i}"

    def tg(i: int) -> str:
        return f"{m(table.targets[i][0])}:{table.target_names[i]}" if table else f"tgt#{i}"

    def cl(i: int) -> str:
        return table.class_names[i] if table else f"class#{i}"

    out = []
    for i, op in enumerate(prog.ops):
        name = OPS[op.code][0]
        w = op.words
        if name == "WAIT":
            arg = f"{w[0]}us"
        elif name == "WAIT_MAP":
            arg = f"{m(op.b)} {w[0]}us"
        elif name == "WAIT_NEAR":
            arg = f"{m(op.b)} ({w[0]:g} {w[1]:g} {w[2]:g}) r{w[3]:g} {w[4]}us"
        elif name in ("WAIT_MOVED", "WAIT_GONE"):
            arg = f"{tg(op.b)} {w[0]}us"
        elif name == "WAIT_CLASS":
            arg = f"{m(op.b)} {cl(w[0])} ({w[1]:g} {w[2]:g} {w[3]:g}) r{w[4]:g} {w[5]}us"
        elif name == "WAIT_GLOBAL":
            g = table.global_names[op.b] if table else f"glob#{op.b}"
            arg = f"{g} state{op.a} {w[0]}us"
        elif name == "WAIT_END":
            arg = f"{w[0]}us"
        elif name == "LOOK":
            arg = f"{w[0]:g} {w[1]:g}"
        elif name == "LOOK_AT":
            arg = tg(op.b) if op.a == LOOKAT_TARGET else f"{m(w[0])} {cl(op.b)}"
        elif name == "SETPOS":
            arg = f"{m(op.b)} ({w[0]:g} {w[1]:g} {w[2]:g})" + \
                (f" {w[3]:g} {w[4]:g}" if op.a else "")
        elif name == "BUTTONS":
            arg = f"down {op.b:#06x} up {w[0]:#06x}"
        elif name in ("IMPULSE", "GOD"):
            arg = str(op.a)
        elif name == "DROP":
            arg = f"{m(op.b)} {cl(w[0])} ({w[1]:g} {w[2]:g} {w[3]:g})"
        elif name in ("SHOT", "STATS"):
            arg = w[0]
        elif name == "START":
            arg = m(op.b)
        else:
            arg = ""
        out.append(f"{i:5d} {op.line:5d} {name} {arg}".rstrip())
    return "\n".join(out) + "\n"
