#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gate_table.py -- the M3 gate's per-subsystem CPU / memory table from a
PROFILE run's log (test/run/m3.sh, ares).

CPU: the 1 s roll-ups (`prof frames=N ...` opens a window, then one
`prof <zone>_us avg= max= ...` line per subsystem). The first window holds
boot and the map open and is skipped. Per zone: avg = frame-weighted mean
of the window means, p90 = 90th percentile of the window means (the
roll-ups carry no per-frame samples), max = the worst single frame.

Memory: peak bytes per carved region (the `carve:` table: the
include/hl64/limits.h sizes) from the reporting lines: `mem:` / `mem rec:`
/ `mem res:` / `mem fx:` / `mem world:` (the `mem` command the PROFILE
route's PASS runs with its last stats record), the roll-ups' pool / region peaks (studio,
sound). The world pools render / texture / collision are LRU caches that
fill to their size: their peak is the run's WANT peak (`mem world:
<pool>_want_peak=`: the open + pin class chunks wanted at once); the
stats records' resident high-water is printed beside it as information.
The studio pool is such a cache too (engine/render/studio.c alloc() evicts
any block not used this frame, soft-resident ones included, before it
fails): ruled on `prof studio_us want_peak=`, its pool_peak information.
The sound region's resident samples are a cache as well (engine/snd/snd.c
evict_cached closes them for an open that finds no room): ruled on `prof
snd_us want_peak=`, its mem_peak information. A region nothing reports is
listed as such.

Region rule (docs/architecture.md 4.1): a reported region whose peak is
above 90% of its carve, or whose peak x 1.25 exceeds its carve, fails the
gate. EXEMPT names the only region outside the rule.

Usage: gate_table.py LOG     (prints Markdown; exit 1 when a CPU zone the
       gate needs has no roll-up line or a region breaks the region rule;
       the last line names what failed)
"""
import re
import sys

ZONES = ["ent", "phys", "rec", "studio", "snd", "fx", "hud", "mon", "game", "pmove", "world"]
NEEDED = ["ent", "phys", "rec", "studio", "snd", "fx", "hud", "mon", "pmove", "world"]
# Region rule thresholds (architecture 4.1).
USE_MAX = 0.90
HEADROOM = 1.25
# The entity components region is fixed-size by design: 495 slots x 228 B
# plus the grid / membership / touch / free-time tables, carved for the
# HL64_MAX_ENTITIES cap whatever the map, so it always reads ~100% and is
# sized by the slot cap, not by peak x 1.25.
EXEMPT = {"components"}
# LRU caches: ruled on the want peak, resident peak informational
LRU = ("render", "texture", "collision")


def kv(line):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))] if s else 0


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    lines = [l.rstrip("\n") for l in open(sys.argv[1], errors="replace")]

    # ---- CPU ----
    windows = []                      # [(frames, frame_us, {zone: (avg, max)})]
    for l in lines:
        m = re.match(r"prof frames=(\d+) frame_us=(\d+)", l)
        if m:
            windows.append((int(m.group(1)), int(m.group(2)), {}))
            continue
        m = re.match(r"prof (\w+)_us (.*)", l)
        if m and windows:
            d = kv(m.group(2))
            windows[-1][2][m.group(1)] = (d.get("avg", 0), d.get("max", 0), d)
    windows = windows[1:]
    if not windows:
        print("no roll-up windows in the log")
        return 1
    nframes = sum(w[0] for w in windows)
    out = ["", "### M3 gate: CPU per subsystem (ares PROFILE, %d windows, %d frames; "
           "avg frame-weighted, p90 of 1 s window means, max = worst frame)" % (len(windows), nframes),
           "", "| zone | avg us | p90 us | max us | windows |", "|---|---:|---:|---:|---:|"]
    fw = [(w[0], w[1]) for w in windows]
    out.append("| frame | %.0f | %d | — | %d |" % (sum(f * u for f, u in fw) / nframes,
                                                   pct([u for _, u in fw], 0.9), len(fw)))
    missing = []
    for z in ZONES:
        got = [(w[0], w[2][z]) for w in windows if z in w[2]]
        if not got:
            out.append("| %s | — | — | — | 0 (no `prof %s_us` line) |" % (z, z))
            if z in NEEDED:
                missing.append(z)
            continue
        n = sum(f for f, _ in got)
        out.append("| %s | %.0f | %d | %d | %d |" % (
            z, sum(f * a[0] for f, a in got) / max(n, 1), pct([a[0] for _, a in got], 0.9),
            max(a[1] for _, a in got), len(got)))

    # ---- memory ----
    carve = {}
    for l in lines:
        m = re.match(r"carve: (\w+)\s+(\d+)\s+(\d+)", l)
        if m:
            carve[m.group(1)] = int(m.group(3)) * 1024
    peak = {}                         # region -> (bytes, source)
    resident = {}                     # LRU region -> resident peak B (information)

    def note(region, b, src):
        if region not in peak or b > peak[region][0]:
            peak[region] = (b, src)

    for l in lines:
        m = re.match(r"mem: ent components (\d+) B", l)
        if m:
            note("components", int(m.group(1)), "mem: ent components")
        m = re.match(r"mem: ent private \d+ B peak (\d+) B", l)
        if m:
            note("gameprivate", int(m.group(1)), "mem: ent private peak")
        m = re.search(r"mem: ent sets .*think/grid region (\d+) B", l)
        if m:
            note("thinkgrid", int(m.group(1)), "mem: ent sets")
        if l.startswith("mem res: "):
            note("restab", kv(l)["tables"], "mem res: tables")
        if l.startswith("mem rec: blobs="):
            d = kv(l)
            note("hle", d["blobs"] + d["runtime"] + d["tables"], "mem rec: blobs+runtime+tables")
            note("dehydrate", d["peak"], "mem rec: store peak")
        if l.startswith("mem fx: decals"):
            d = kv(l)
            note("decals", d["peak"], "mem fx: peak (sheet+verts+slot sheets)")
        m = re.match(r"mem: hud (\d+) of (\d+) B", l)
        if m:
            note("hud", int(m.group(1)), "mem: hud")
        # the studio pool is an LRU cache (studio.c alloc() evicts any block
        # not used this frame before it fails): ruled on its want peak, the
        # pool's resident peak is information
        if l.startswith("prof studio_us "):
            d = kv(l)
            note("studio", d.get("want_peak", d["pool_peak"]), "prof studio_us want_peak")
            resident["studio"] = max(resident.get("studio", 0), d["pool_peak"])
        # the sound region's resident samples (snd.h SND_RESIDENT) are a
        # cache: an open with no room evicts them (engine/snd/snd.c
        # evict_cached), so the region is ruled on the want peak (in use
        # without the cache), mem_peak is information
        if l.startswith("prof snd_us "):
            d = kv(l)
            note("sound", d.get("want_peak", d["mem_peak"]), "prof snd_us want_peak")
            resident["sound"] = max(resident.get("sound", 0), d["mem_peak"])
        m = re.search(r"^stats \d+ world: .*render=\d+/(\d+)KB texture=\d+/(\d+)KB coll=\d+/(\d+)KB", l)
        if m:
            for r, v in zip(LRU, m.groups()):
                resident[r] = max(resident.get(r, 0), int(v) * 1024)
        if l.startswith("mem world: "):
            d = kv(l)
            for r in LRU:
                note(r, d["%s_want_peak" % r], "mem world: %s_want_peak" % r)
    broken = []                       # (region, reason)
    out += ["", "### M3 gate: memory per region (peak over the run vs the carved limits.h size; "
            "rule: use <= %d%% and peak x %.2f <= size, %s exempt)" % (
                USE_MAX * 100, HEADROOM, ", ".join(sorted(EXEMPT))), "",
            "| region | peak B | size B | use | peak x %.2f B | rule | source |" % HEADROOM,
            "|---|---:|---:|---:|---:|---|---|"]
    for r, size in carve.items():
        if r in peak:
            b, src = peak[r]
            why = []
            if b > USE_MAX * size:
                why.append("use %.0f%% > %d%%" % (100.0 * b / size, USE_MAX * 100))
            if b * HEADROOM > size:
                why.append("peak x %.2f = %d B > %d B" % (HEADROOM, b * HEADROOM, size))
            if r in EXEMPT:
                verdict = "exempt (fixed-size)"
            elif why:
                verdict = "FAIL: " + "; ".join(why)
                broken.append((r, "; ".join(why)))
            else:
                verdict = "ok"
            if r in resident:
                src += "; resident peak %d B (info)" % resident[r]
            out.append("| %s | %d | %d | %.0f%% | %.0f | %s | %s |" % (
                r, b, size, 100.0 * b / size, b * HEADROOM, verdict, src))
        else:
            out.append("| %s | — | %d | — | — | — | not reported |" % (r, size))
    m = [l for l in lines if l.startswith("mem: allocs ")]
    if m:
        out.append("")
        out.append("heap: " + m[-1][len("mem: "):])
    print("\n".join(out))
    if missing:
        print("\nmissing roll-ups: " + ", ".join(missing))
    if broken:
        print("\nregion rule failed: " + ", ".join("%s (%s)" % rb for rb in broken))
    return 1 if missing or broken else 0


if __name__ == "__main__":
    sys.exit(main())
