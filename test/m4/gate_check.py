#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_check.py -- the M4 gate's log checks for one route run (host or
ares; test/run/m4.sh; docs/architecture.md 17 M4, local contract "Logs the
gate parses").

Checks (one `check <name> PASS|FAIL <detail>` line each):
  route       `route PASS` (else the `route FAIL` line: op, source line, cause)
  switch-K    the K-th expected seam switch `A->B` is logged, in order, with
              its kind (use / touch; with --forced-ok a forced switch is
              accepted and counted)
  switches    no switch the route does not expect
  forced      host: no forced switch (lines and stats seam forced=0);
              --forced-ok: the count is reported
  stats-K     a stats record before and after the K-th switch
  over        every stats record over=0 (I/O budget)
  open-fail   no `seam: open_fail` line, every stats seam open_fail=0
  blk1ms      every stats stream blk1ms=0 (no frame > 1 ms blocked I/O)
  popin       every stats stream popin=0 (popin_tele allowed, reported)
  continuity  one continuity line per switch, each missing=0 extra=0, and
              stats seam continuity_bad=0
  rec         every stats record has a sane rec line: live/pending/dry/
              released/store present, live <= HL64_MAX_ENTITIES, store used
              <= peak <= the dehydrate region

Usage: gate_check.py LOG --switch A:B:KIND [--switch ...] [--forced-ok]
Exit 0 when every check passes; the last line names the first failure.
"""
import argparse
import os
import re
import sys

import gate_parse as gp

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def limit(name):
    with open(os.path.join(ROOT, "include", "hl64", "limits.h")) as f:
        m = re.search(r"#define %s\s+(\d+)" % name, f.read())
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log")
    ap.add_argument("--switch", action="append", default=[], metavar="A:B:KIND")
    ap.add_argument("--forced-ok", action="store_true")
    a = ap.parse_args()
    lines = gp.read(a.log)
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print("check %s %s%s" % (name, "PASS" if ok else "FAIL", " " + detail if detail else ""))

    # route
    res, fail_line = gp.route_result(lines)
    check("route", res == "PASS", fail_line or ("route PASS" if res else "no route result line"))

    # switches
    want = [tuple(s.split(":")) for s in a.switch]
    got = gp.switches(lines)
    at = 0
    matched = []
    for k, (wa, wb, wkind) in enumerate(want, 1):
        hit = None
        for i in range(at, len(got)):
            if got[i]["a"] == wa and got[i]["b"] == wb:
                hit = i
                break
        if hit is None:
            check("switch-%d" % k, False, "no `seam: switch %s->%s` line (crossing not implemented "
                  "or not reached)" % (wa, wb))
            matched.append(None)
            continue
        s = got[hit]
        ok = s["kind"] == wkind or (a.forced_ok and s["kind"] == "forced")
        check("switch-%d" % k, ok, "%s->%s landmark %s (%s%s) frame=%d" % (
            s["a"], s["b"], s["landmark"], s["kind"], ": " + s["why"] if s["why"] else "", s["frame"])
              + ("" if ok else " (want %s)" % wkind))
        matched.append(s)
        at = hit + 1
    extra = [s for s in got if s not in matched]
    check("switches", not extra, "%d logged, %d expected%s" % (
        len(got), len(want), "; unexpected: " + ", ".join(
            "%s->%s frame=%d" % (s["a"], s["b"], s["frame"]) for s in extra) if extra else ""))

    st = gp.stats(lines)
    seam_recs = [r["seam"] for r in st.values() if "seam" in r]
    stream_recs = [r["stream"] for r in st.values() if "stream" in r]
    forced_lines = sum(1 for s in got if s["kind"] == "forced")
    forced_stats = max((r.get("forced", 0) for r in seam_recs), default=None)
    if a.forced_ok:
        check("forced", True, "forced switches: %d logged, stats forced=%s (reported)" % (
            forced_lines, forced_stats))
    else:
        check("forced", forced_lines == 0 and forced_stats == 0,
              "forced switches: %d logged, stats forced=%s" % (
                  forced_lines, "no stats seam line" if forced_stats is None else forced_stats))

    # stats records around each switch
    frames = sorted(r["frame"] for r in st.values() if "frame" in r)
    sw_frames = [s["frame"] for s in matched if s]
    for k, s in enumerate(matched, 1):
        if not s:
            check("stats-%d" % k, False, "switch %d not logged" % k)
            continue
        i = sw_frames.index(s["frame"])
        lo = sw_frames[i - 1] if i else -1
        hi = sw_frames[i + 1] if i + 1 < len(sw_frames) else 1 << 62
        before = [f for f in frames if lo < f < s["frame"]]
        after = [f for f in frames if s["frame"] < f < hi]
        check("stats-%d" % k, bool(before and after), "records at frames %s | switch %d | %s" % (
            before[-1:] or "none", s["frame"], after[:1] or "none"))

    mains = [(n, r["main"]) for n, r in st.items() if "main" in r]
    bad = [n for n, m in mains if m.get("over", 0) != 0]
    check("over", mains and not bad, "%d records%s" % (
        len(mains), "; over>0 in stats %s" % bad if bad else ""))

    of_lines = [l for l in lines if l.startswith("seam: open_fail")]
    of_bad = [r.get("open_fail") for r in seam_recs if r.get("open_fail", 0) != 0]
    check("open-fail", bool(seam_recs) and not of_lines and not of_bad,
          "no stats seam lines" if not seam_recs else "%d open_fail lines, stats open_fail %s" % (
              len(of_lines), max((r.get("open_fail", 0) for r in seam_recs), default=0)))

    blk = [r.get("blk1ms") for r in stream_recs]
    check("blk1ms", bool(stream_recs) and all(b == 0 for b in blk),
          "no stats stream lines" if not stream_recs else "blk1ms max %s over %d records" % (
              max(b or 0 for b in blk), len(blk)))
    pop = [r.get("popin") for r in stream_recs]
    tele = max((r.get("popin_tele", 0) for r in stream_recs), default=0)
    check("popin", bool(stream_recs) and all(p == 0 for p in pop),
          "no stats stream lines" if not stream_recs else "popin max %s, popin_tele %d (allowed)" % (
              max(p or 0 for p in pop), tele))

    cont = gp.continuity(lines)
    cbad = [c for c in cont if c.get("missing", 1) != 0 or c.get("extra", 1) != 0]
    cstat = max((r.get("continuity_bad", 0) for r in seam_recs), default=0)
    check("continuity", len(cont) == len(got) and len(got) > 0 and not cbad and cstat == 0,
          "%d continuity lines for %d switches%s, stats continuity_bad=%d" % (
              len(cont), len(got), "; bad: " + "; ".join(
                  "carry=%s missing=%s extra=%s" % (c.get("carry"), c.get("missing"), c.get("extra"))
                  for c in cbad) if cbad else "", cstat))

    cap = limit("HL64_DEHYDRATE_KB") * 1024
    emax = limit("HL64_MAX_ENTITIES")
    rbad = []
    for n, r in st.items():
        if "main" not in r:
            continue
        rec = r.get("rec")
        if rec is None:
            rbad.append("stats %d: no rec line" % n)
            continue
        miss = [k for k in ("live", "pending", "dry", "released", "store") if k not in rec]
        if miss:
            rbad.append("stats %d: no %s" % (n, "/".join(miss)))
            continue
        store = rec["store"]
        if not (isinstance(store, tuple) and store[0] <= store[1] <= cap):
            rbad.append("stats %d: store=%s (cap %d)" % (n, store, cap))
        if rec["live"] > emax:
            rbad.append("stats %d: live=%d > %d" % (n, rec["live"], emax))
    last = next((r["rec"] for r in reversed(list(st.values())) if "rec" in r), None)
    check("rec", bool(mains) and not rbad, "; ".join(rbad[:4]) if rbad else "last: live=%s pending=%s "
          "dry=%s released=%s store=%s" % tuple(
              last.get(k) for k in ("live", "pending", "dry", "released", "store")) if last else "")

    failed = [n for n, ok in results if not ok]
    print("gate-check %s (%d of %d failed)%s" % ("FAIL" if failed else "PASS", len(failed), len(results),
                                                 "; first: " + failed[0] if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
