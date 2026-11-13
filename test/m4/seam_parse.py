#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Seam parse check: the runtime's decoded CWG3 graph (hl64-seam dump,
engine/world/seam.c) against the cook's own reports of the same file:
<staging>/campaign.json (maps, placements, landmarks, links) and
<staging>/seams.json (every seam, its boxes, delta, portal T, twins,
sector set, B directory CRC).

  python3 test/m4/seam_parse.py DUMP STAGING

Prints "seam_parse ok: ..." or FAIL lines; exit status 0 = pass.
"""
import json
import os
import sys

fails = []


def check(cond, what):
    if not cond:
        fails.append(what)


def vec(s):
    return [float(x) for x in s.split(",")]


def close(a, b, tol=1e-3):
    return len(a) == len(b) and all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def fields(words):
    """key=value words -> dict (values as text)"""
    return dict(w.split("=", 1) for w in words if "=" in w)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    dump, staging = argv[1], argv[2]
    with open(os.path.join(staging, "campaign.json")) as f:
        camp = json.load(f)
    with open(os.path.join(staging, "seams.json")) as f:
        sj = json.load(f)
    graph, maps, lms, links, seams = None, [], [], [], []
    with open(dump) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] == "graph":
                graph = {k: int(v) for k, v in fields(w[1:]).items()}
            elif w[0] == "map":
                maps.append((w[2], fields(w[3:])))
            elif w[0] == "landmark":
                lms.append((w[1], w[2], fields(w[3:])))
            elif w[0] == "link":
                links.append((w[1], w[2], w[3], fields(w[4:])))
            elif w[0] == "seam":
                seams.append((w[2], w[3], w[4], fields(w[5:])))
    if graph is None:
        print("FAIL no graph line in the dump")
        return 1

    jmaps = camp["maps"]
    jseams = sj["seams"]
    check(graph["maps"] == len(jmaps) == len(maps), f"maps {graph['maps']} / json {len(jmaps)}")
    check(graph["seams"] == len(jseams) == len(seams), f"seams {graph['seams']} / json {len(jseams)}")
    check(graph["twins"] == sum(len(s["twins"]) for s in jseams), "twin count")
    check(graph["pool"] == sum(len(s["sectors"]) for s in jseams), "sector-pool count")
    check(graph["landmarks"] == sum(len(m["landmarks"]) for m in jmaps.values()) == len(lms),
          "landmark count")
    check(graph["links"] == len(links), "link count")
    names = list(jmaps)
    check(graph["start"] == names.index(camp["start"]) if camp.get("start") else True, "start map")

    for name, m in maps:
        j = jmaps.get(name)
        check(j is not None, f"map {name} not in campaign.json")
        if j is None:
            continue
        check(close(vec(m["gofs"]), j["gofs"], 0), f"map {name} gofs {m['gofs']} / {j['gofs']}")
        check(int(m["landmarks"]) == len(j["landmarks"]), f"map {name} landmarks")
        check(close(vec(m["mins"]) + vec(m["maxs"]), j["bounds"], 16.0), f"map {name} bounds")
    for mname, lname, f in lms:
        j = jmaps[mname]["landmarks"].get(lname)
        check(j is not None and close(vec(f["org"]), j, 0), f"landmark {mname} {lname}")
    # links: every campaign.json link of a map appears (CWG drops nothing the
    # json kept; dropped_links are reported apart)
    for mname, m in jmaps.items():
        got = [(to, lm, int(f["spawnflags"])) for frm, to, lm, f in links if frm == mname]
        want = [(l["to"], l["landmark"], l["spawnflags"]) for l in m["links"]]
        check(sorted(got) == sorted(want), f"links of {mname}: {got} / {want}")
    # every executable non-self link has a seam whose maps / landmark agree
    for frm, to, lm, f in links:
        si = int(f["seam"])
        if si < 0:
            continue
        s = seams[si]
        check((s[0], s[1], s[2]) == (frm, to, lm), f"link {frm}->{to} {lm}: seam {si} is {s[:3]}")

    for i, ((frm, to, lm, f), j) in enumerate(zip(seams, jseams)):
        tag = f"seam {i} {frm}->{to} {lm}"
        check((frm, to, lm) == (j["from"], j["to"], j["landmark"]), f"{tag}: json {j['from']}->{j['to']}")
        check(int(f["trigger"]) == j["trigger_lump"], f"{tag}: trigger")
        check(f["changetarget"] == (j["changetarget"] or "-"), f"{tag}: changetarget")
        check(int(f["kind"]) == j["kind"] and int(f["flags"]) == j["flags"], f"{tag}: kind/flags")
        check(int(f["draw"]) == j["draw"], f"{tag}: draw")
        check(int(f["bcrc"]) == j["b_dir_crc"], f"{tag}: bcrc")
        check([int(x) for x in f["need"].split(",")] == j["need"], f"{tag}: need")
        for key, jk in (("delta", "delta"), ("T", "portal"), ("sw_mins", "switch_mins"),
                        ("sw_maxs", "switch_maxs"), ("carry_mins", "carry_mins"),
                        ("carry_maxs", "carry_maxs"), ("ap_mins", "approach_mins"),
                        ("ap_maxs", "approach_maxs")):
            check(close(vec(f[key]), j[jk]), f"{tag}: {key} {f[key]} / {j[jk]}")
        tw = [tuple(int(x) for x in p.split(":")) for p in f["twin_lumps"].split(",") if p]
        check(tw == [(t["a"], t["b"]) for t in j["twins"]], f"{tag}: twins")
        sec = [int(x) for x in f["sectors"].split(",") if x]
        check(sec == j["sectors"], f"{tag}: sectors")

    for x in fails:
        print("FAIL", x)
    if not fails:
        print(f"seam_parse ok: {graph['maps']} maps, {graph['landmarks']} landmarks, "
              f"{graph['links']} links, {graph['seams']} seams, {graph['twins']} twins, "
              f"{graph['pool']} pool entries match {staging}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
