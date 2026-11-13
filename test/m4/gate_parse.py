#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_parse.py -- parsers for the M4 gate's log lines (local contract
"Logs the gate parses"; engine/test/seamprof.h). Imported by
test/m4/gate_check.py, test/m4/gate_table.py and test/m4/gate_seamprof.py.

Lines:
  seam: switch A->B landmark L (use|touch|forced: why) frame=F
  seam: continuity carry=N matched=M missing=X extra=Y
  seam: open_fail ...
  stats N frame=F ... over=O ...                       (eng.c stats record)
  stats N seam: switches= forced= open_fail= late= twins= carried= continuity_bad= closes=
  stats N stream: popin= popin_tele= blk1ms= pins= prefetch= recompute= reassert=
  stats N rec: live= pending= dry= released= store=used/peak
  route: <op> ...  /  route PASS  /  route FAIL ...
  seamprof: switch N A->B frame=F frames=K [mark] [cut]
  seamprof: zone=<z> max_us=<v> frame=<f> avg_us=<v> switch_us=<v>
  seamprof: region=<r> peak=<bytes> frame=<f> size=<bytes>
"""
import re

SWITCH = re.compile(r"^seam: switch (\S+)->(\S+) landmark (\S+) \((use|touch|forced)(?::([^)]*))?\) frame=(\d+)")
CONTINUITY = re.compile(r"^seam: continuity (.*)")
STATS_MAIN = re.compile(r"^stats (\d+) frame=(\d+) ")
STATS_SUB = re.compile(r"^stats (\d+) (seam|stream|rec): (.*)")
SP_SWITCH = re.compile(r"^seamprof: switch (\d+) (\S+?)->(\S+) frame=(\d+) frames=(\d+)((?: mark| cut)*)$")
SP_ZONE = re.compile(r"^seamprof: zone=(\w+) max_us=(\d+) frame=(\d+) avg_us=(\d+) switch_us=(\d+)$")
SP_REGION = re.compile(r"^seamprof: region=(\w+) peak=(\d+) frame=(\d+) size=(\d+)$")


def kv(text):
    """key=int pairs; key=a/b gives (a, b)."""
    out = {}
    for k, v in re.findall(r"(\w+)=(\d+(?:/\d+)?)", text):
        out[k] = tuple(int(x) for x in v.split("/")) if "/" in v else int(v)
    return out


def read(path):
    with open(path, errors="replace") as f:
        return [l.rstrip("\r\n") for l in f]


def switches(lines):
    """[{a, b, landmark, kind, why, frame, line}] in log order."""
    out = []
    for i, l in enumerate(lines):
        m = SWITCH.match(l)
        if m:
            out.append({"a": m.group(1), "b": m.group(2), "landmark": m.group(3),
                        "kind": m.group(4), "why": (m.group(5) or "").strip(),
                        "frame": int(m.group(6)), "line": i + 1})
    return out


def continuity(lines):
    out = []
    for i, l in enumerate(lines):
        m = CONTINUITY.match(l)
        if m:
            d = kv(m.group(1))
            d["line"] = i + 1
            out.append(d)
    return out


def stats(lines):
    """{label: {"main": kv, "frame": F, "seam": kv, "stream": kv, "rec": kv}}
    in record order (dict order)."""
    out = {}
    for l in lines:
        m = STATS_MAIN.match(l)
        if m:
            rec = out.setdefault(int(m.group(1)), {})
            rec["main"] = kv(l[m.end(1):])
            rec["frame"] = int(m.group(2))
            continue
        m = STATS_SUB.match(l)
        if m:
            out.setdefault(int(m.group(1)), {})[m.group(2)] = kv(m.group(3))
    return out


def seamprof(lines):
    """[{n, a, b, frame, frames, mark, cut, zones: {z: (max, frame, avg, at_switch)},
    regions: {r: (peak, frame, size)}}] in log order."""
    out = []
    cur = None
    for l in lines:
        m = SP_SWITCH.match(l)
        if m:
            flags = m.group(6).split()
            cur = {"n": int(m.group(1)), "a": m.group(2), "b": m.group(3),
                   "frame": int(m.group(4)), "frames": int(m.group(5)),
                   "mark": "mark" in flags, "cut": "cut" in flags, "zones": {}, "regions": {}}
            out.append(cur)
            continue
        m = SP_ZONE.match(l)
        if m and cur is not None:
            cur["zones"][m.group(1)] = tuple(int(m.group(k)) for k in (2, 3, 4, 5))
            continue
        m = SP_REGION.match(l)
        if m and cur is not None:
            cur["regions"][m.group(1)] = tuple(int(m.group(k)) for k in (2, 3, 4))
    return out


def route_result(lines):
    """("PASS"|"FAIL"|None, the FAIL line or None)."""
    for l in lines:
        if l == "route PASS":
            return "PASS", None
        if l.startswith("route FAIL"):
            return "FAIL", l
    return None, None


def records(lines, kinds=("player", "ents")):
    """The lines two runs must agree on (K9 / K10): route ops and the
    stats record lines of the given kinds."""
    pat = re.compile(r"^route: [0-9]|^stats [0-9]+ (%s): " % "|".join(kinds))
    return [l for l in lines if pat.match(l)]
