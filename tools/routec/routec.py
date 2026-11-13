# SPDX-License-Identifier: MIT
"""routec: compile a test route (.route text) to route bytecode (.rbc).

The route language is the old route text syntax (docs/formats/
route-bytecode.md, "Source language"). Maps, targetnames, classes and global
states are resolved at compile time against campaign.rtb, so a renamed
entity fails the compile, not the run (architecture.md 15). Positions are
map-local to the map named by the latest `waitmap`.

  python3 -m tools.routec.routec --rtb campaign.rtb X.route -o X.rbc
  python3 -m tools.routec.routec --rtb campaign.rtb --dump X.rbc
"""
from __future__ import annotations

import argparse
import math
import os
import re
import sys
from typing import List, Optional

from ..restab import rtb
from . import rbc

MAX_SOURCE = 1 << 20
MAX_TIME_S = 4000.0                 # u32 microseconds hold ~4294 s
LABEL_RE = re.compile(r"[A-Za-z0-9_.\-]{1,%d}" % rbc.MAX_LABEL)
NUM_RE = re.compile(r"[-+]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]{1,2})?")


class CompileError(Exception):
    pass


def compile_route(text: str, source: str, table: rtb.Table) -> List[rbc.Op]:
    ops: List[rbc.Op] = []
    cur_map: Optional[int] = None

    for ln, raw in enumerate(text.splitlines(), 1):
        s = raw.split("#", 1)[0].strip()
        if not s:
            continue
        tok = s.split()
        verb, args = tok[0], tok[1:]

        def err(msg: str) -> CompileError:
            return CompileError(f"{source}:{ln}: {msg}")

        if not s.isascii():
            raise err("non-ASCII text outside a comment")

        def nargs(*counts: int) -> None:
            if len(args) not in counts:
                raise err(f"{verb} takes {' or '.join(map(str, counts))} arguments")

        def num(i: int, what: str) -> float:
            if not NUM_RE.fullmatch(args[i]):
                raise err(f"{what} {args[i]!r} is not a number")
            v = float(args[i])
            if not math.isfinite(v) or abs(v) > 1e9:
                raise err(f"{what} {args[i]!r} out of range")
            return v

        def seconds(i: int, what: str = "time") -> int:
            v = num(i, what)
            if not 0 <= v <= MAX_TIME_S:
                raise err(f"{what} {v} s outside 0..{MAX_TIME_S:g}")
            return int(round(v * 1e6))

        def radius(i: int) -> float:
            v = num(i, "radius")
            if v < 0:
                raise err("negative radius")
            return v

        def need_map() -> int:
            if cur_map is None:
                raise err(f"{verb} before the first waitmap (positions are map-local)")
            return cur_map

        def cls(name: str) -> int:
            c = table.class_id(name)
            if c is None:
                raise err(f"class {name!r} not in classes.def")
            return c

        def target(name: str) -> int:
            m = need_map()
            t = table.target_id(m, name)
            if t is None:
                raise err(f"no entity named {name!r} in map {table.map_names[m]}")
            return t

        def op(name: str, a: int = 0, b: int = 0, words: tuple = ()) -> None:
            ops.append(rbc.Op(rbc.OPCODE[name], a, b, words, ln))

        if verb[0] in "+-" and len(verb) > 1:
            nargs(0)
            bit = rbc.BUTTONS.get(verb[1:])
            if bit is None:
                raise err(f"unknown button {verb}")
            op("BUTTONS", 0, bit if verb[0] == "+" else 0, (0 if verb[0] == "+" else bit,))
        elif verb == "wait":
            nargs(1)
            op("WAIT", words=(seconds(0),))
        elif verb == "waitmap":
            nargs(2)
            m = table.map_id(args[0])
            if m is None:
                raise err(f"map {args[0]!r} not in the campaign")
            cur_map = m
            op("WAIT_MAP", 0, m, (seconds(1, "timeout"),))
        elif verb == "waitnear":
            nargs(5)
            op("WAIT_NEAR", 0, need_map(),
               (num(0, "x"), num(1, "y"), num(2, "z"), radius(3), seconds(4, "timeout")))
        elif verb in ("waitmoved", "waitgone"):
            nargs(2)
            op("WAIT_MOVED" if verb == "waitmoved" else "WAIT_GONE", 0, target(args[0]),
               (seconds(1, "timeout"),))
        elif verb == "waitclass":
            nargs(6)
            op("WAIT_CLASS", 0, need_map(),
               (cls(args[0]), num(1, "x"), num(2, "y"), num(3, "z"), radius(4),
                seconds(5, "timeout")))
        elif verb == "waitglobal":
            nargs(2, 3)
            g = table.global_id(args[0])
            if g is None:
                raise err(f"global state {args[0]!r} not set by any map")
            states = {"off": rbc.GLOBAL_OFF, "on": rbc.GLOBAL_ON, "dead": rbc.GLOBAL_DEAD}
            st = states.get(args[2] if len(args) == 3 else "on")
            if st is None:
                raise err("global state must be on, off or dead")
            op("WAIT_GLOBAL", st, g, (seconds(1, "timeout"),))
        elif verb == "waitend":
            nargs(1)
            op("WAIT_END", words=(seconds(0, "timeout"),))
        elif verb == "look":
            nargs(2)
            op("LOOK", words=(num(0, "pitch"), num(1, "yaw")))
        elif verb == "lookat":
            nargs(1)
            m = need_map()
            t = table.target_id(m, args[0])
            if t is not None:
                op("LOOK_AT", rbc.LOOKAT_TARGET, t, (m,))
            elif table.class_id(args[0]) is not None:
                op("LOOK_AT", rbc.LOOKAT_CLASS, cls(args[0]), (m,))
            else:
                raise err(f"{args[0]!r} is neither an entity name in "
                          f"{table.map_names[m]} nor a class")
        elif verb == "setpos":
            nargs(3, 5)
            ang = (num(3, "pitch"), num(4, "yaw")) if len(args) == 5 else (0.0, 0.0)
            op("SETPOS", 1 if len(args) == 5 else 0, need_map(),
               (num(0, "x"), num(1, "y"), num(2, "z")) + ang)
        elif verb == "impulse":
            nargs(1)
            if not re.fullmatch(r"[0-9]{1,3}", args[0]) or int(args[0]) > 255:
                raise err("impulse must be 0..255")
            op("IMPULSE", int(args[0]))
        elif verb == "drop":
            nargs(4)
            op("DROP", 0, need_map(), (cls(args[0]), num(1, "x"), num(2, "y"), num(3, "z")))
        elif verb == "god":
            nargs(1)
            if args[0] not in ("0", "1"):
                raise err("god takes 0 or 1")
            op("GOD", int(args[0]))
        elif verb in ("shot", "stats"):
            nargs(1)
            if not LABEL_RE.fullmatch(args[0]):
                raise err(f"label {args[0]!r}: 1-{rbc.MAX_LABEL} of A-Z a-z 0-9 _ . -")
            op(verb.upper(), words=(args[0],))
        elif verb == "pass":
            nargs(0)
            op("PASS")
        else:
            raise err(f"unknown verb {verb!r}")

        if len(ops) > rbc.MAX_OPS:
            raise err("more than 65535 ops")
        if ln > 0xFFFF:
            raise err("source longer than 65535 lines")

    if not ops or ops[-1].code != rbc.OPCODE["PASS"]:
        raise CompileError(f"{source}: a route must end with pass")
    if sum(1 for o in ops if o.code == rbc.OPCODE["PASS"]) != 1:
        raise CompileError(f"{source}: pass must appear once, as the last step")
    return ops


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rtb", required=True, help="campaign.rtb")
    ap.add_argument("--dump", action="store_true", help="validate and disassemble an .rbc")
    ap.add_argument("input")
    ap.add_argument("-o", "--out")
    a = ap.parse_args(argv)
    try:
        with open(a.rtb, "rb") as f:
            table = rtb.read(f.read())
        with open(a.input, "rb") as f:
            data = f.read(MAX_SOURCE + 1)
        if a.dump:
            sys.stdout.write(rbc.disasm(rbc.read(data, table), table))
            return 0
        if not a.out:
            ap.error("-o is required when compiling")
        if len(data) > MAX_SOURCE:
            raise CompileError(f"{a.input}: larger than 1 MiB")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            raise CompileError(f"{a.input}: not UTF-8 text") from None
        source = os.path.basename(a.input)
        ops = compile_route(text, source, table)
        out = rbc.write(table.build_id, source, ops)
        rbc.read(out, table)                 # the compiler's output must validate
    except (CompileError, rtb.RtbError, rbc.RbcError, ValueError, OSError) as e:
        print(f"routec: {e}", file=sys.stderr)
        return 1
    tmp = a.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, a.out)
    print(f"routec: {a.out}: {len(ops)} ops, {len(out)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
