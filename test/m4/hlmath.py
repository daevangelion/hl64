#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic transcendentals (engine/core/hlmath.h).

1. No C file under engine/, game/ or include/ calls a libm transcendental
   (sin, cosf, atan2, powl, ...) directly: host glibc and cart newlib
   round them differently, which splits host and cart routes (K10). Calls
   go through hl_sin / hl_sinf ... instead. Exact or correctly rounded
   libm functions (sqrt, fabs, floor, ceil, fmod) stay allowed. Comments
   and string literals are ignored.
2. The accuracy driver (test/m4/hlmath.c, `make mathtest`) passes.

Usage: test/m4/hlmath.py --bin <hl64-mathtest> [--root <hl64 tree>]
       test/m4/hlmath.py --selftest   (the scanner on planted snippets)
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCAN_DIRS = ("engine", "game", "include")
NAMES = ("sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh "
         "exp exp2 expm1 log log2 log10 log1p logb pow cbrt hypot erf erfc "
         "tgamma lgamma sincos").split()
CALL = re.compile(r"(?<![\w.>])(?:__builtin_)?(%s)[fl]?\s*\(" % "|".join(NAMES))
# comments, string and character literals (replaced by blanks, newlines kept)
STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def blank(m):
    return re.sub(r"[^\n]", " ", m.group(0))


def calls(text):
    """[(line, name)] of direct libm transcendental calls in C text."""
    code = STRIP.sub(blank, text)
    return [(code.count("\n", 0, m.start()) + 1, m.group(0).rstrip("( \t"))
            for m in CALL.finditer(code)]


def scan(root):
    found = []
    for d in SCAN_DIRS:
        for dp, _, fs in os.walk(os.path.join(root, d)):
            for f in sorted(fs):
                if not f.endswith((".c", ".h")):
                    continue
                p = os.path.join(dp, f)
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for line, name in calls(fh.read()):
                        found.append("%s:%d: %s" % (os.path.relpath(p, root), line, name))
    return found


def selftest():
    cases = [
        ("x = sinf(a);", ["sinf"]),
        ("x = sin (a) + cos(b);", ["sin", "cos"]),
        ("x = atan2(y, x) * 180;", ["atan2"]),
        ("x = __builtin_powf(a, 2);", ["__builtin_powf"]),
        ("x = hl_sinf(a);", []),
        ("x = asinh(a);", ["asinh"]),
        ("x = sqrtf(a) + floorf(b) + fmod(c, d) + fabs(e);", []),
        ("/* sin(x) */ x = 1; // cos(y)", []),
        ('puts("tan(x)");', []),
        ("x = s.sin(a); y = p->cos(b);", []),
        ("float sinus(float); x = cosine(a);", []),
        ("x = logf(a);", ["logf"]),
    ]
    bad = 0
    for text, want in cases:
        got = [n for _, n in calls(text)]
        if got != want:
            print("selftest FAIL %r: got %s, want %s" % (text, got, want))
            bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin")
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if selftest():
        return 1
    if a.selftest:
        print("hlmath selftest PASS")
        return 0
    if not a.bin:
        ap.error("--bin required")
    found = scan(a.root)
    for f in found:
        print("direct libm transcendental (use engine/core/hlmath.h): " + f)
    r = subprocess.run([a.bin], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if found or r.returncode != 0:
        print("hlmath FAIL: %d direct call(s), driver exit %d" % (len(found), r.returncode))
        return 1
    worst = [l.split()[1] + "=" + l.split()[2] for l in r.stdout.splitlines() if l.startswith("max ") and "ulp" in l]
    print("hlmath PASS: no direct libm transcendental in %s; max ulp %s" % ("/".join(SCAN_DIRS), " ".join(worst)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
