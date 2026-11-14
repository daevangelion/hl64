# SPDX-License-Identifier: MIT
"""Cook closed, budgeted campaign seams into CWG3 after the W64 packages exist.

Run from the repository root as `python3 -m tools.w64c.w64seam <campaign.cwg>
--packages <dir with *.w64> --maps-dir <dir with *.bsp> --report seams.json`.
No map names or campaign-specific rules
are embedded. Rays use a 6-unit lattice to 4096 units (400 Fibonacci directions),
accelerated by BSP interval traversal: empty intervals need no point samples.
A successful cook verifies every non-solid eye on a <=16-unit grid, including
box endpoints. The JSON report records failures as well as accepted proofs.
CWG3 uses 136-byte seam records; see docs/formats/cwg-format.md.

A malformed input (a .cwg, .w64 or .bsp that is truncated, has a table outside
the file, bad magic or version, an index out of range) is reported as
`w64seam: error: <file>: <what>` on stderr with exit status 1; the graph is
rewritten only when every seam was accepted.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import re
import struct
import sys
from pathlib import Path

import numpy as np

from tools.cook.hl_campaign import NONE, portal_translation, read_cwg, write_cwg
from tools.cook.hl_classes import ENTCOOK_H, map_entities
from tools.w64c.w64c import Bsp, read_limits, submodel_placements, SUBMODEL_PAD
from tools.w64c.w64check import W64, Bsp as CheckedBsp

VIEW = np.array([0., 0., 28.])
OCCLUDERS = {"func_door", "func_door_rotating", "func_wall", "func_breakable",
             "func_button", "func_rot_button", "momentary_door"}
# engine/world/stream.c's open + pin naming, mirrored by the crossing-window
# proof (docs/formats/cwg-format.md, Proof and residency policy)
MOVER_MARGIN = 128.     # a brush entity's box grown this much (MOVER_MARGIN)
MOVER_LEAD_S = .5       # ... and swept over this much of its motion (MOVER_LEAD_S)
VIEW_SLACK = 24.        # the drawn eye's neighbours: eye +- 24 u per axis (VIEW_SLACK)
VIEW_OFFSET = 32.       # the drawn eye's last offset is used below 32 u (TELE_SLACK)
DUCK_VIEW = 12.         # ducked view offset (DUCK_DZ below the standing 28)
ARRIVAL_CORNERS = ((0, 0), (-1, -1), (1, -1), (-1, 1), (1, 1))     # arrival_leaves
STAR = np.array([[0., 0., 0.]] + [list(np.eye(3)[k] * s * r) for r in (VIEW_SLACK, VIEW_OFFSET)
                                  for k in range(3) for s in (-1, 1)])
ROTATING = {"func_door_rotating", "func_rotating", "func_pendulum", "func_rot_button",
            "momentary_rot_button", "func_tank", "func_tankrocket", "func_tanklaser",
            "func_tankmortar"}
SLIDING = {"func_door", "func_water", "func_button", "momentary_door"}
PATHED = {"func_train": "path_corner", "func_guntarget": "path_corner",
          "func_tracktrain": "path_track"}


class EntityError(ValueError):
    """A malformed entity key; the cook adds the map file name."""


def key_floats(e, key, default, count=3):
    text = e.get(key, default)
    try:
        values = [float(v) for v in text.split()]
    except ValueError:
        values = []
    if len(values) != count or not all(math.isfinite(v) for v in values):
        raise EntityError(f"{e.get('classname', '?')}: {key} {text!r}: want {count} finite numbers")
    return values


def key_int(e, key):
    try:
        return int(e.get(key, "0") or 0)
    except ValueError:
        raise EntityError(f"{e.get('classname', '?')}: {key} {e.get(key)!r} is not an integer") from None


def origin(e):
    return np.array(key_floats(e, "origin", "0 0 0"))


def intersects(lo, hi, mn, mx):
    return np.all((np.asarray(lo) <= mx) & (mn <= np.asarray(hi)), axis=-1)


def model_index(bsp, e):
    """The BSP model number of a `*N` model key."""
    text = e["model"]
    if not text[1:].isdigit() or not 0 < int(text[1:]) < len(bsp.models):
        raise EntityError(f"{e.get('classname', '?')}: model {text!r} is not one of the {len(bsp.models)} BSP models")
    return int(text[1:])


def entity_box(bsp, e):
    o = origin(e)
    model = e.get("model", "")
    if model.startswith("*"):
        m = bsp.models[model_index(bsp, e)]
        return np.array(m[:3]) + o, np.array(m[3:6]) + o
    return o, o


def eye_grid(lo, hi, zstep=16):
    axes = []
    for axis, (a, b) in enumerate(zip(lo, hi)):
        values = np.arange(a, b, zstep if axis == 2 else 16.) if b > a else np.array([a])
        if values[-1] != b:
            values = np.append(values, b)
        axes.append(values)
    return np.array(list(itertools.product(*axes)), dtype=np.float64)


def directions(n):
    i = np.arange(n) + .5
    z = 1 - 2 * i / n
    theta = np.pi * (1 + 5 ** .5) * i
    r = np.sqrt(1 - z * z)
    return np.stack([np.cos(theta) * r, np.sin(theta) * r, z], axis=1)



def spawn_rotation(e):
    """Basis of a brush entity's model space after the brush class's spawn-time
    angle resets, as a 3x3 matrix whose columns are the GoldSrc `angles`
    (pitch yaw roll, degrees) forward, minus right and up vectors: a world
    offset v from the entity origin is `v @ basis` in model space
    ((v.forward, -v.right, v.up), trace-and-hulls.md 4.2), and column 0 is the
    direction `angles` points along."""
    angles = key_floats(e, "angles", f"0 {e.get('angle', '0')} 0")
    cn = e.get("classname")
    if cn in {"func_door", "func_button", "func_wall", "momentary_door"}:
        angles = [0., 0., 0.]  # SetMovedir / SP_func_wall
    elif cn == "func_breakable":
        angles[1] = 0.  # breakable_setup keeps the old yaw only as m_angle
    pitch, yaw, roll = (a * (math.pi * 2 / 360) for a in angles)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    sr, cr = math.sin(roll), math.cos(roll)
    forward = (cp * cy, cp * sy, -sp)
    right = (-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp)
    up = (cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp)
    return np.array([forward, [-x for x in right], up]).T

class ProofError(ValueError):
    def __init__(self, message, evidence):
        self.evidence = evidence
        super().__init__(message + ": " + json.dumps(evidence))


class Scene:
    def __init__(self, path):
        try:
            self.load(path)
        except EntityError as error:
            raise ValueError(f"{path}: {error}") from None

    def load(self, path):
        CheckedBsp(path)    # structure, index ranges and acyclic trees before any walk
        self.bsp = Bsp(path)
        self.ents = map_entities(path)
        b = self.bsp
        self.pn = np.array([p[:3] for p in b.planes])
        self.pd = np.array([p[3] for p in b.planes])
        self.np = np.array([n[0] for n in b.nodes])
        self.children = np.array([n[1:3] for n in b.nodes])
        self.lc = np.array([l[0] for l in b.leafs])
        self.leaf_lo = np.array([l[2:5] for l in b.leafs])
        self.leaf_hi = np.array([l[5:8] for l in b.leafs])
        self.occ = []
        for e in self.ents:
            cn = e.get("classname", "")
            if (cn not in OCCLUDERS or not e.get("model", "").startswith("*") or
                    key_int(e, "rendermode") or
                    (cn.startswith("func_door") and key_int(e, "spawnflags") & 1)):
                continue
            mi = model_index(b, e)
            model = b.models[mi]
            off = origin(e)
            if cn == "momentary_door" and key_int(e, "spawnflags") & 1:
                angles = key_floats(e, "angles", f"0 {e.get('angle', '0')} 0")
                if angles == [0, -1, 0] or angles == [0, -2, 0]:
                    movedir = np.array([0., 0., 1. if angles[1] == -1 else -1.])
                else:
                    movedir = spawn_rotation({**e, "classname": ""})[:, 0]
                # The BSP pads each submodel bound by 1; the door's spawn takes 2 off
                # the size again before it computes its full travel distance.
                distance = np.abs(movedir * (np.array(model[3:6]) - model[:3])).sum() - key_floats(e, "lip", "0", 1)[0]
                off += movedir * distance
            self.occ.append((model[9], off, spawn_rotation(e), mi))

    def leaf_indices(self, points, head=None):
        cur = np.full(len(points), self.bsp.models[0][9] if head is None else head, np.int64)
        while np.any(cur >= 0):
            ix = np.flatnonzero(cur >= 0)
            pl = self.np[cur[ix]]
            ds = np.einsum("ij,ij->i", points[ix], self.pn[pl]) - self.pd[pl]
            cur[ix] = self.children[cur[ix], (ds < 0).astype(int)]
        return -1 - cur

    def contents(self, points, head=None):
        return self.lc[self.leaf_indices(points, head)]

    def solid(self, points, movers):
        solid = self.contents(points) == -2
        if movers:
            for head, off, rotation, _ in self.occ:
                solid |= self.contents((points - off) @ rotation, head) == -2
        return solid

    def landmark(self, name):
        for e in self.ents:
            if e.get("classname") == "info_landmark" and e.get("targetname") == name:
                return origin(e)
        raise ValueError(f"missing info_landmark {name!r}")

    def leaves(self, lo, hi):
        # Runtime visleafs are 1-based; BSP leaf 0 is the solid sentinel.
        return (np.flatnonzero(intersects(self.leaf_lo[1:self.bsp.visleafs + 1],
                                         self.leaf_hi[1:self.bsp.visleafs + 1], lo, hi)) + 1).tolist()

    def first_hit(self, eyes, dirs, movers, details=False):
        E = np.repeat(eyes, len(dirs), axis=0)
        world_dirs = np.tile(dirs, (len(eyes), 1))
        count = len(E)
        distance = np.full(count, np.inf)
        kind = np.zeros(count, np.int8)
        surface = np.full(count, np.inf)
        hit_model = np.full(count, -1, np.int64)
        models = [(self.bsp.models[0][9], np.zeros(3), np.eye(3), 0)] + (self.occ if movers else [])
        for model_index, (head, off, rotation, mi) in enumerate(models):
            P = (E - off) @ rotation
            D = world_dirs @ rotation
            # Each entry is one BSP node and the ray intervals reaching it.
            stack = [(head, np.arange(count), np.zeros(count), np.full(count, 4096.))]
            while stack:
                node, ix, lo, hi = stack.pop()
                keep = (lo <= hi) & (lo <= distance[ix])
                ix, lo, hi = ix[keep], lo[keep], hi[keep]
                if not len(ix):
                    continue
                if node < 0:
                    c = self.lc[-1 - node]
                    if c != -2 and (model_index or c != -6):
                        continue
                    # Equivalent to point walking at 6,12,...,4092,4096.
                    t = np.maximum(6., np.ceil(lo / 6.) * 6.)
                    t = np.minimum(t, 4096.)
                    hit = (t <= hi) & ((t < distance[ix]) | ((t == distance[ix]) & (lo < surface[ix])))
                    distance[ix[hit]] = t[hit]
                    kind[ix[hit]] = 2 if c == -6 else 1
                    surface[ix[hit]] = lo[hit]
                    hit_model[ix[hit]] = mi
                    continue
                plane = self.np[node]
                n = self.pn[plane]
                start = P[ix] @ n - self.pd[plane]
                slope = D[ix] @ n
                dl, dh = start + slope * lo, start + slope * hi
                for side in (0, 1):
                    valid = ((dl >= 0) | (dh >= 0)) if side == 0 else ((dl < 0) | (dh < 0))
                    sub = np.flatnonzero(valid)
                    if not len(sub):
                        continue
                    l, h = lo[sub].copy(), hi[sub].copy()
                    crossing = (dl[sub] >= 0) != (dh[sub] >= 0)
                    sel = np.flatnonzero(crossing)
                    if len(sel):
                        cut = -start[sub[sel]] / slope[sub[sel]]
                        begins_here = (dl[sub[sel]] >= 0) if side == 0 else (dl[sub[sel]] < 0)
                        h[sel[begins_here]] = np.nextafter(cut[begins_here], -np.inf) if side else cut[begins_here]
                        l[sel[~begins_here]] = np.nextafter(cut[~begins_here], np.inf) if side else cut[~begins_here]
                    stack.append((int(self.children[node, side]), ix[sub], l, h))
        result = distance.reshape(len(eyes), -1), kind.reshape(len(eyes), -1)
        if details:
            valid = hit_model >= 0
            return (*result, {"points": E[valid] + world_dirs[valid] * surface[valid, None],
                              "models": hit_model[valid], "kind": kind[valid], "valid": valid})
        return result


def eye_agreement(a, b, delta, eyes, kind, dirs):
    if not len(eyes):
        raise ProofError("switch box contains no non-solid sampled eyes",
                         {"eyes": 0, "rays": 0, "disagreeing_rays": 0, "worst_rays": []})
    good, bad_count, worst = [], 0, []
    for start in range(0, len(eyes), 64):
        batch = eyes[start:start + 64]
        ta, ka = a.first_hit(batch, dirs, kind == 1)
        tb, kb = b.first_hit(batch + delta, dirs, kind == 1)
        # Unbounded rays are not a proof, and sky versus solid never agrees.
        agree = ((ka == 2) & (kb == 2)) | ((ka == 1) & (kb == 1) &
                    (np.abs(np.where(np.isfinite(ta), ta, -1e9) -
                            np.where(np.isfinite(tb), tb, 1e9)) <= 8))
        agree &= (~b.solid(batch + delta, kind == 1))[:, None]
        good.extend(np.all(agree, axis=1).tolist())
        bad_count += int((~agree).sum())
        score = np.abs(np.where(np.isfinite(ta), ta, -4096) -
                       np.where(np.isfinite(tb), tb, 4096))
        score = np.where(ka != kb, score + 8192, score)
        bad = np.flatnonzero(~agree)
        for flat in bad[np.argsort(score.ravel()[bad])[-8:]]:
            i, j = np.unravel_index(flat, agree.shape)
            worst.append({"eye": batch[i].tolist(), "direction": dirs[j].tolist(),
                          "a_distance": float(ta[i, j]) if np.isfinite(ta[i, j]) else None,
                          "b_distance": float(tb[i, j]) if np.isfinite(tb[i, j]) else None,
                          "a_kind": int(ka[i, j]), "b_kind": int(kb[i, j]),
                          "severity": float(score[i, j])})
        worst.sort(key=lambda ray: ray["severity"], reverse=True)
        worst = worst[:8]
    good = np.array(good)
    stats = {"eyes": len(eyes), "rays": len(eyes) * len(dirs),
             "disagreeing_rays": bad_count, "worst_rays": worst,
             "agreeing_eyes": int(good.sum())}
    return good, stats


def closed_check(a, b, delta, lo, hi, kind, dirs, zstep=16):
    eyes = eye_grid(lo, hi, zstep)
    eyes = eyes[~a.solid(eyes, kind == 1)]
    good, stats = eye_agreement(a, b, delta, eyes, kind, dirs)
    if not good.any():
        raise ProofError("no closed eyes", stats)
    if not good.all():
        narrowed_lo, narrowed_hi = eyes[good].min(axis=0), eyes[good].max(axis=0)
        # The bbox of all good eyes can contain a bad eye (holes or disconnected
        # regions). Retain a rectangular subset, never turn those holes into a
        # false proof. Each cut excludes a failing plane and keeps the largest
        # remaining agreeing sample set, breaking ties by geometric volume.
        kept = eyes[good]
        bad = eyes[~good]
        while True:
            inside = bad[intersects(bad, bad, narrowed_lo, narrowed_hi)]
            if not len(inside):
                break
            center = (narrowed_lo + narrowed_hi) / 2
            point = inside[np.argmin(np.linalg.norm(inside - center, axis=1))]
            choices = []
            for axis in range(3):
                for upper in (False, True):
                    subset = kept[kept[:, axis] < point[axis]] if upper else kept[kept[:, axis] > point[axis]]
                    if not len(subset):
                        continue
                    mn, mx = subset.min(axis=0), subset.max(axis=0)
                    choices.append((len(subset), float(np.prod(mx - mn)), mn, mx, subset))
            if not choices:
                raise ProofError("cannot isolate an agreeing eye box", stats)
            _, _, narrowed_lo, narrowed_hi, kept = max(choices, key=lambda c: c[:2])
        # Endpoints can add samples after narrowing; certify the stored box too.
        nlo, nhi, _, narrowed = closed_check(a, b, delta, narrowed_lo, narrowed_hi, kind, dirs, zstep)
        stats["narrowed_proof"] = narrowed
        return nlo, nhi, 3, stats
    return lo, hi, 1, stats


def certify_trigger(a, b, delta, lo, hi, kind, dirs):
    original_lo, original_hi = lo.copy(), hi.copy()
    lo, hi, flags, stats = closed_check(a, b, delta, lo, hi, kind, dirs)
    zstep = 16
    if hi[2] - lo[2] < 8:
        lo, hi, _, refined = closed_check(a, b, delta, original_lo, original_hi, kind, dirs, 4)
        stats["z_refinement"] = refined
        zstep, flags = 4, flags | 2
        if hi[2] - lo[2] < 8:
            raise ProofError("closed Z region has no slab at least 8 units thick", stats)
        eyes = eye_grid(lo, hi, 4)
        eyes = eyes[~a.solid(eyes, kind == 1)]
        _, _, floor = a.first_hit(eyes, np.array([[0., 0., -1.]]), False, details=True)
        standing = floor["points"][:, 2] + 64
        stats["standing_eye_range"] = [float(standing.min()), float(standing.max())] if len(standing) else []
        if len(standing) != len(eyes) or np.any(floor["kind"] != 1) or np.any((standing < lo[2]) | (standing > hi[2])):
            raise ProofError("closed Z slab excludes the standing eye height (floor + 64)", stats)
    return lo, hi, flags, stats, zstep


def walkable_eyes(scene, lo, hi):
    """Standing eyes over world floors, with the BSP's standing hull clear."""
    probes = eye_grid(lo, hi)
    probes = probes[~scene.solid(probes, False)]
    floors = []
    for start in range(0, len(probes), 4096):
        _, _, hits = scene.first_hit(probes[start:start + 4096],
                                    np.array([[0., 0., -1.]]), False, details=True)
        floors.extend(hits["points"][hits["kind"] == 1])
    if not floors:
        return np.empty((0, 3))
    eyes = np.unique(np.array(floors) + np.array([0., 0., 64.]), axis=0)
    eyes = eyes[intersects(eyes, eyes, lo, hi)]
    if not len(eyes):
        return eyes
    origins = eyes - VIEW
    bsp = scene.bsp
    cur = np.full(len(eyes), bsp.models[0][10], np.int64)
    planes = np.array([node[0] for node in bsp.clip])
    children = np.array([node[1:3] for node in bsp.clip])
    while np.any(cur >= 0):
        ix = np.flatnonzero(cur >= 0)
        pl = planes[cur[ix]]
        d = np.einsum("ij,ij->i", origins[ix], scene.pn[pl]) - scene.pd[pl]
        cur[ix] = children[cur[ix], (d < 0).astype(int)]
    return eyes[(cur != -2) & ~scene.solid(eyes, False)]


def eye_components(eyes):
    """16-unit floor-grid adjacency, allowing the HL 18-unit step height."""
    by_xy = {}
    for i, eye in enumerate(eyes):
        by_xy.setdefault(tuple(eye[:2]), []).append(i)
    neighbors = []
    for axis in (0, 1):
        coords = sorted(set(eyes[:, axis]))
        table = {}
        for i, value in enumerate(coords):
            table[value] = [coords[j] for j in (i - 1, i + 1)
                            if 0 <= j < len(coords) and abs(coords[j] - value) <= 16.00001]
        neighbors.append(table)
    todo = set(range(len(eyes)))
    components = []
    while todo:
        seed = todo.pop()
        component, stack = [seed], [seed]
        while stack:
            i = stack.pop()
            x, y, z = eyes[i]
            adjacent = [(xx, y) for xx in neighbors[0][x]] + [(x, yy) for yy in neighbors[1][y]]
            for xy in adjacent:
                for j in by_xy.get(xy, []):
                    if j in todo and abs(eyes[j, 2] - z) <= 18:
                        todo.remove(j)
                        component.append(j)
                        stack.append(j)
        components.append(eyes[component])
    return components


def search_switch(a, b, delta, trigger_lo, trigger_hi, dirs, trigger_stats):
    """Find a nearby closed corridor cross-section for an armed touch seam."""
    search_lo, search_hi = trigger_lo - 256, trigger_hi + 256
    eyes = walkable_eyes(a, search_lo, search_hi)
    if not len(eyes):
        raise ProofError("no walkable standing eyes within 256 units of the trigger",
                         {**trigger_stats, "search": {"radius": 256, "candidate_eyes": 0}})
    good, stats = eye_agreement(a, b, delta, eyes, 0, dirs)
    components = eye_components(eyes[good])
    candidates = []
    for component in components:
        for axis in (0, 1):
            for value in np.unique(component[:, axis]):
                cross = component[component[:, axis] == value]
                lo, hi = cross.min(axis=0), cross.max(axis=0)
                lo[2] -= 4
                hi[2] += 4
                distance = np.linalg.norm(np.maximum(0, np.maximum(trigger_lo - hi, lo - trigger_hi)))
                candidates.append((float(distance), -len(cross), axis, float(value), lo, hi))
    candidates.sort(key=lambda c: c[:4])
    rejected = []
    for distance, _, axis, value, lo, hi in candidates:
        try:
            mn, mx, _, proof = closed_check(a, b, delta, lo, hi, 0, dirs, 4)
            if mx[2] - mn[2] < 8:
                continue
            scan_lo, scan_hi = mn.copy(), mx.copy()
            across = 1 - axis
            scan_lo[across] -= 32
            scan_hi[across] += 32
            scan_lo[2], scan_hi[2] = search_lo[2], search_hi[2]
            walking = walkable_eyes(a, scan_lo, scan_hi)
            connected = [part for part in eye_components(walking)
                         if np.any(intersects(part, part, mn, mx))]
            if not connected:
                continue
            corridor = np.concatenate(connected)
            if not np.all(intersects(corridor, corridor, mn, mx)):
                continue
            evidence = {**trigger_stats, "search": {
                "radius": 256, "trigger_eye_mins": trigger_lo.tolist(),
                "trigger_eye_maxs": trigger_hi.tolist(), "candidate_eyes": len(eyes),
                "closed_components": len(components), "agreement": stats,
                "chosen_eye_mins": mn.tolist(), "chosen_eye_maxs": mx.tolist(),
                "distance_from_trigger": distance, "slice_axis": axis,
                "walkable_margin": 32, "walkable_slice_eyes": corridor.tolist(),
                "proof": proof}}
            return mn, mx, 3, evidence, 4
        except ProofError as error:
            if len(rejected) < 8:
                rejected.append({"slice_axis": axis, "slice": value, "error": str(error)})
    raise ProofError("no nearby closed standing corridor cross-section within 256 units",
                     {**trigger_stats, "search": {"radius": 256, "candidate_eyes": len(eyes),
                       "closed_components": len(components), "agreement": stats,
                       "rejected_slices": rejected}})


def certify_switch(a, b, delta, lo, hi, kind, dirs):
    try:
        return certify_trigger(a, b, delta, lo, hi, kind, dirs)
    except ProofError as error:
        if kind:
            raise
        return search_switch(a, b, delta, lo, hi, dirs, error.evidence)


def edict_classes(entcook_h=ENTCOOK_H):
    """Classes of the entity cook's `hle_classes[]` table that get an edict (not
    flagged HLE_CF_LOGIC or HLE_CF_POINT)."""
    text = Path(entcook_h).read_text("latin-1")
    start = text.find("hle_classes[] = {")
    end = text.find("{NULL", start)
    if start < 0 or end < 0:
        raise ValueError(f"{entcook_h}: no hle_classes[] table")
    classes = re.findall(r'\{"([^"]+)",\s*([^}]+)\}', text[start:end])
    if not classes:
        raise ValueError(f"{entcook_h}: empty hle_classes[] table")
    return {cn for cn, flags in classes if "HLE_CF_LOGIC" not in flags and "HLE_CF_POINT" not in flags}


def twins(a, b, landmark, delta):
    allowed = edict_classes()
    def candidates(scene, offset):
        out = []
        for i, e in enumerate(scene.ents):
            cn = e.get("classname", "")
            if (cn not in allowed or cn in {"worldspawn", "trigger_changelevel", "trigger_transition", "ambient_generic"}
                    or cn.startswith(("info_", "env_", "light")) or
                    not ("origin" in e or e.get("model", "").startswith("*"))):
                continue
            lo, hi = entity_box(scene.bsp, e)
            center = (lo + hi) / 2 - offset
            if np.linalg.norm(center - landmark) <= 512:
                out.append((i, e, center, hi - lo))
        return out
    aa, bb = candidates(a, np.zeros(3)), candidates(b, delta)
    used, pairs, report = set(), [], []
    for i, ea, ca, sa in aa:
        matches = []
        for j, eb, cb, sb in bb:
            if j in used or ea["classname"] != eb["classname"]:
                continue
            global_match = ea.get("globalname") and ea.get("globalname") == eb.get("globalname")
            names_match = ea.get("targetname", "") == eb.get("targetname", "")
            brush_a, brush_b = ea.get("model", "").startswith("*"), eb.get("model", "").startswith("*")
            position_match = np.linalg.norm(ca - cb) <= 8
            if global_match or (names_match and brush_a == brush_b and position_match and
                                (not brush_a or np.all(np.abs(sa - sb) <= 8))):
                matches.append((not bool(global_match), float(np.linalg.norm(ca - cb)), j, eb))
        if matches:
            _, _, j, eb = min(matches, key=lambda v: v[:3])
            used.add(j)
            pairs.append([i, j])
            report.append({"a": i, "b": j, "classname": ea["classname"],
                           "targetname": ea.get("targetname", ""), "model_a": ea.get("model", ""),
                           "model_b": eb.get("model", "")})
    return pairs, report


def path_points(scene, e, classname):
    """Origins of every `classname` entity reachable from e's target over
    target / altpath links."""
    named = {}
    for x in scene.ents:
        if x.get("classname") == classname and x.get("targetname"):
            named.setdefault(x["targetname"], []).append(x)
    todo, seen, out = [e.get("target", "")], set(), []
    while todo:
        name = todo.pop()
        if not name or name in seen:
            continue
        seen.add(name)
        for x in named.get(name, []):
            out.append(origin(x))
            todo.extend((x.get("target", ""), x.get("altpath", "")))
    return out


def mover_reach(scene, w):
    """(MOVR chunk, lo, hi) per brush entity whose model has a MOVR: the box
    stream.c's each_mover can test against a leaf's sectors. Its ENTS rest
    placement (sub bbox + origin key) and, live, anywhere on its travel
    (sliding doors / buttons: movedir x (size - lip); plats: their height
    down; trains / gun targets / track trains: every path point of their
    chain; rotating classes: the sphere about their origin), grown by its
    speed x MOVER_LEAD_S and by MOVER_MARGIN. A func_pushable is bounded by
    its placement (where players push it is not a cook fact)."""
    out = []
    for e in scene.ents:
        if not e.get("model", "").startswith("*"):
            continue
        mi = model_index(scene.bsp, e)
        kind, chunk = struct.unpack_from(">HH", w.d, w.sub_ofs + 16 * mi)
        if kind != 2:
            continue
        bb = np.array(struct.unpack_from(">6h", w.d, w.sub_ofs + 16 * mi + 4), np.float64)
        o = origin(e)
        lo, hi = bb[:3] + o, bb[3:] + o
        cn = e.get("classname", "")
        radius = np.linalg.norm(np.maximum(np.abs(bb[:3]), np.abs(bb[3:])))
        speed = 0.
        if cn in SLIDING:
            angles = key_floats(e, "angles", f"0 {e.get('angle', '0')} 0")
            if angles in ([0, -1, 0], [0, -2, 0]):
                movedir = np.array([0., 0., 1. if angles[1] == -1 else -1.])
            else:
                movedir = spawn_rotation({**e, "classname": ""})[:, 0]
            d = movedir * (np.abs(movedir * (bb[3:] - bb[:3])).sum() - key_floats(e, "lip", "0", 1)[0])
            lo, hi = np.minimum(lo, lo + d), np.maximum(hi, hi + d)
            speed = key_floats(e, "speed", "100", 1)[0]
        elif cn in ("func_plat", "func_platrot"):
            lo = lo - [0, 0, abs(key_floats(e, "height", "0", 1)[0]) or bb[5] - bb[2]]
            speed = key_floats(e, "speed", "150", 1)[0]
        elif cn in ROTATING:
            lo, hi = np.minimum(lo, o - radius), np.maximum(hi, o + radius)
        elif cn in PATHED:
            for p in path_points(scene, e, PATHED[cn]):
                if cn == "func_tracktrain":
                    plo, phi = p - radius, p + radius + [0, 0, abs(key_floats(e, "height", "0", 1)[0])]
                else:
                    plo, phi = p - (bb[3:] - bb[:3]) / 2, p + (bb[3:] - bb[:3]) / 2
                lo, hi = np.minimum(lo, plo), np.maximum(hi, phi)
            speed = key_floats(e, "speed", "100", 1)[0]
        pad = MOVER_MARGIN + abs(speed) * MOVER_LEAD_S
        out.append((chunk, lo - pad, hi + pad))
    return out


def teleports(scene):
    """(lo, hi, destination eyes) per trigger_teleport: its brush box and the
    eye stream.c's name_point names for the player origin game_teleport_dest
    gives (each entity its target names: origin + 1 + 36, eye + 28)."""
    out = []
    for e in scene.ents:
        if e.get("classname") != "trigger_teleport" or not e.get("model", "").startswith("*"):
            continue
        lo, hi = entity_box(scene.bsp, e)
        dests = [origin(x) + [0., 0., 1. + 36.] + VIEW for x in scene.ents
                 if e.get("target") and x.get("targetname") == e.get("target")]
        if dests:
            out.append((lo, hi, np.array(dests)))
    return out


def player_fits(scene, eyes):
    """Eyes a player can have: standing (hull 1 clear at eye - 28) or ducked
    (hull 3 clear at eye - 12); (standing, ducked) masks."""
    bsp = scene.bsp
    planes = np.array([node[0] for node in bsp.clip])
    children = np.array([node[1:3] for node in bsp.clip])
    out = []
    for hull, view in ((1, VIEW[2]), (3, DUCK_VIEW)):
        points = eyes - [0., 0., view]
        cur = np.full(len(points), bsp.models[0][9 + hull], np.int64)
        while np.any(cur >= 0):
            ix = np.flatnonzero(cur >= 0)
            pl = planes[cur[ix]]
            d = np.einsum("ij,ij->i", points[ix], scene.pn[pl]) - scene.pd[pl]
            cur[ix] = children[cur[ix], (d < 0).astype(int)]
        out.append(cur != -2)
    return out


def approach_eyes(scene, lo, hi, pad):
    """Eyes of the player origins in the approach box at `pad` (origin box
    lo - VIEW - pad .. hi - VIEW + pad around switch eye box [lo, hi]),
    standing or ducked, on a 16-unit grid with its endpoints, and for each
    the smallest approach expansion that holds it."""
    base_lo, base_hi = lo - VIEW, hi - VIEW
    eyes = eye_grid(base_lo - pad + [0., 0., DUCK_VIEW], base_hi + pad + VIEW)
    stand, duck = player_fits(scene, eyes)
    need = np.full(len(eyes), np.inf)
    for ok, view in ((stand, VIEW[2]), (duck, DUCK_VIEW)):
        o = eyes - [0., 0., view]
        out = np.maximum(np.maximum(base_lo - o, o - base_hi), 0).max(axis=1)
        need = np.where(ok & (out <= pad), np.minimum(need, out), need)
    keep = np.isfinite(need)
    return eyes[keep], need[keep]


def view_bytes(budget, eyes, teleport=True):
    """Per eye, the render / texture bytes stream.c names at OPEN on a home
    layer: the view_set of the eye leaf and of each drawn-eye leaf (STAR: the
    eye +- VIEW_SLACK and +- VIEW_OFFSET per axis), plus the teleport point
    pins of the eye leaf; -1 where the eye leaf is 0 (no set). Also the
    eye leaf per eye."""
    scene, nleaf = budget.scene, budget.w.nleaf
    out = np.full((len(eyes), 2), -1, np.int64)
    eye_leaf = np.zeros(len(eyes), np.int64)
    memo = {}
    for start in range(0, len(eyes), 16384):
        batch = eyes[start:start + 16384]
        leaves = scene.leaf_indices((batch[:, None, :] + STAR[None]).reshape(-1, 3)).reshape(len(batch), -1)
        leaves[(leaves < 1) | (leaves >= nleaf)] = 0
        rows = np.concatenate([leaves[:, :1], np.sort(leaves[:, 1:], axis=1)], axis=1)
        uniq, inverse = np.unique(rows, axis=0, return_inverse=True)
        sizes = np.full((len(uniq), 2), -1, np.int64)
        for u, row in enumerate(uniq):
            if not row[0]:
                continue
            key = (int(row[0]), frozenset(int(x) for x in row if x))
            if key not in memo:
                sets = [budget.view_set(leaf) for leaf in key[1]]
                if teleport:
                    sets += budget.tele_set(key[0])
                memo[key] = (budget.chunk_bytes(frozenset().union(*(s[0] for s in sets))),
                             budget.chunk_bytes(frozenset().union(*(s[1] for s in sets))))
            sizes[u] = memo[key]
        out[start:start + len(batch)] = sizes[inverse.ravel()]
        eye_leaf[start:start + len(batch)] = leaves[:, 0]
    return out, eye_leaf


def pin_sets(budget, seamset, lo, hi, delta):
    """B's seam pin as stream.c's name_seam names it: the cooked sector-pool
    slice (sectors' REND + pages, MOVRs + pages) and the view_set of each
    arrival eye's leaf (arrival_leaves: the switch box's centre and X/Y
    corners at its mid height, mapped into B); render and texture chunk sets."""
    render, texture = set(seamset[0]), set(seamset[1])
    centre, half = (lo + hi) / 2 + delta, (hi - lo) / 2
    points = np.array([centre + [sx * half[0], sy * half[1], 0.] for sx, sy in ARRIVAL_CORNERS])
    leaves = sorted({int(x) for x in budget.scene.leaf_indices(points) if 0 < x < budget.w.nleaf})
    for leaf in leaves:
        r, t = budget.view_set(leaf)
        render |= r
        texture |= t
    return frozenset(render), frozenset(texture), leaves


class Budget:
    def __init__(self, scene, path):
        self.scene, self.w = scene, W64(path)
        w = self.w
        if w.nleaf != scene.bsp.visleafs + 1 or w.nsub != len(scene.bsp.models):
            raise ValueError(f"{path}: BSP/package leaf or model count mismatch")
        if w.ents_chunk == NONE:
            raise ValueError(f"{path}: no HLE chunk to verify entity-lump numbering")
        _, hle, length = w.chunks[w.ents_chunk]
        magic = w.b[hle:hle + 4]
        if length < 96 or magic not in (b"HLE1", b"1ELH"):
            raise ValueError(f"{path}: invalid HLE header")
        endian = "<" if magic == b"HLE1" else ">"
        if struct.unpack_from(endian + "H", w.b, hle + 16)[0] != len(scene.ents):
            raise ValueError(f"{path}: BSP/HLE entity-lump numbering mismatch")
        self.pinned = set(range(w.coll_first, w.coll_first + w.pinned))
        self.placements = submodel_placements(scene.bsp, scene.ents, True)
        self.sect_box = np.array([struct.unpack_from(">6h", w.d, w.sect_ofs + 20 * s)
                                  for s in range(w.nsect)], np.float64).reshape(-1, 6)
        reach = mover_reach(scene, w)
        self.reach_chunk = [chunk for chunk, _, _ in reach]
        self.reach_lo = np.array([lo for _, lo, _ in reach]).reshape(-1, 3)
        self.reach_hi = np.array([hi for _, _, hi in reach]).reshape(-1, 3)
        self.teleports = teleports(scene)
        self.cell_cache = {}
        self.page_cache = {}
        self.view_cache = {}
        self.tele_cache = {}

    def pages(self, chunk):
        if chunk in self.page_cache:
            return self.page_cache[chunk]
        w = self.w
        _, off, _ = w.chunks[chunk]
        nm = struct.unpack_from(">H", w.b, off)[0]
        pages = set()
        for i in range(nm):
            tex = struct.unpack_from(">H", w.b, off + 24 + i * 24 + 20)[0]
            todo, seen = [tex], set()
            while todo:
                t = todo.pop()
                if t in seen or t == NONE:
                    continue
                if t >= w.ntex:
                    raise ValueError("MOVR texture index out of range")
                seen.add(t)
                page, _, _, _, _, _, nxt, alt = struct.unpack_from(">HBBBBHHH", w.d, w.tex_ofs + 12 * t)
                if page != NONE:
                    pages.add(w.texp_first + page)
                todo.extend((nxt, alt))
        self.page_cache[chunk] = pages
        return pages

    def window_box(self, eye_lo, eye_hi):
        """Union of cell lists touched by a player's 128-unit padded window."""
        w = self.w
        lo = np.asarray(eye_lo) - VIEW - np.array([16, 16, 36]) - 128
        hi = np.asarray(eye_hi) - VIEW + np.array([16, 16, 36]) + 128
        key = tuple(lo), tuple(hi)
        if key in self.cell_cache:
            return self.cell_cache[key]
        first = np.maximum(0, np.floor((lo - w.gorg) / w.gcell).astype(int))
        last = np.minimum(np.array(w.gdims) - 1, np.floor((hi - w.gorg) / w.gcell).astype(int))
        chunks = set(self.pinned)
        dx, dy, _ = w.gdims
        for x, y, z in itertools.product(*(range(a, b + 1) for a, b in zip(first, last))):
            ci = x + dx * (y + dy * z)
            chunks.update(w.coll_first + int(c) for c in np.flatnonzero(w.cell_mat[ci]))
        for mi, placements in enumerate(self.placements[1:], 1):
            if any(intersects(a, b, lo - SUBMODEL_PAD, hi + SUBMODEL_PAD) for a, b in placements):
                _, _, _, start, count = w.models[mi]
                chunks.update(range(w.coll_first + start, w.coll_first + start + count))
        self.cell_cache[key] = chunks
        return chunks

    def leaf_sectors(self, leaf):
        """wld_leaf_sectors: the sectors visleaf `leaf` sees (its vis list)."""
        w = self.w
        index = struct.unpack_from(">I", w.d, w.leaf_ofs + leaf * 4)[0]
        pos = w.vp_ofs + 2 * index
        count = struct.unpack_from(">H", w.d, pos)[0]
        return list(struct.unpack_from(f">{count}H", w.d, pos + 2))

    def view_set(self, leaf):
        """What stream.c's name_leaf names for `leaf`: every sector of its vis
        list (REND + TEXP pages) and the MOVR (+ every page of its textures'
        animation / alternate chains) of each brush entity whose reach
        (mover_reach) meets one of those sectors' boxes (each_mover)."""
        if leaf in self.view_cache:
            return self.view_cache[leaf]
        w = self.w
        sectors = self.leaf_sectors(leaf)
        render, texture = set(), set()
        for s in sectors:
            chunk, first, num = struct.unpack_from(">HHH", w.d, w.sect_ofs + 20 * s + 12)
            render.add(chunk)
            texture.update(w.texp_first + struct.unpack_from(">H", w.d, w.pp_ofs + 2 * j)[0]
                           for j in range(first, first + num))
        if sectors and self.reach_chunk:
            box = self.sect_box[sectors]
            meet = np.all((self.reach_lo[:, None, :] <= box[None, :, 3:]) &
                          (box[None, :, :3] <= self.reach_hi[:, None, :]), axis=2).any(axis=1)
            for i in np.flatnonzero(meet):
                render.add(self.reach_chunk[i])
                texture.update(self.pages(self.reach_chunk[i]))
        self.view_cache[leaf] = result = (frozenset(render), frozenset(texture))
        return result

    def tele_set(self, leaf):
        """The point pins of the trigger_teleports eye leaf `leaf` sees (stream.c
        ent_scan: a teleport whose box meets a sector of the eye leaf's list;
        name_point: the destination eye's leaf), as view_set pairs."""
        if leaf in self.tele_cache:
            return self.tele_cache[leaf]
        out = []
        sectors = self.leaf_sectors(leaf)
        if sectors:
            box = self.sect_box[sectors]
            for lo, hi, eyes in self.teleports:
                if np.any(np.all((lo <= box[:, 3:]) & (box[:, :3] <= hi), axis=1)):
                    for dest in self.scene.leaf_indices(eyes):
                        if 0 < dest < self.w.nleaf:
                            out.append(self.view_set(int(dest)))
        self.tele_cache[leaf] = out
        return out

    def chunk_bytes(self, chunks):
        """Pool bytes of `chunks` as world.c allocates them (16-aligned)."""
        return sum((self.w.chunks[c][2] + 15) & ~15 for c in chunks)

    def coll_point(self, eye):
        return sum(self.w.chunks[c][2] for c in self.window_box(eye, eye))

    def coll_leaf_max(self, leaf, approach_lo, approach_hi):
        """Conservative worst collision window, clipped to this approach/leaf overlap."""
        w = self.w
        result = sum(w.chunks[c][2] for c in self.pinned)
        lo = np.maximum(self.scene.leaf_lo[leaf], approach_lo)
        hi = np.minimum(self.scene.leaf_hi[leaf], approach_hi)
        first = np.maximum(0, np.floor((lo - w.gorg) / w.gcell).astype(int))
        last = np.minimum(np.array(w.gdims) - 1, np.floor((hi - w.gorg) / w.gcell).astype(int))
        for xyz in itertools.product(*(range(a, b + 1) for a, b in zip(first, last))):
            cell_lo = w.gorg + np.array(xyz) * w.gcell
            cell_hi = cell_lo + w.gcell
            chunks = self.window_box(np.maximum(lo, cell_lo) + VIEW, np.minimum(hi, cell_hi) + VIEW)
            result = max(result, sum(w.chunks[c][2] for c in chunks))
        return result

    def sector_set(self, entries):
        w = self.w
        sets = [set(), set(), set(self.pinned)]
        for entry in entries:
            if entry & 0x8000:
                mi = entry & 0x7FFF
                kind, chunk = struct.unpack_from(">HH", w.d, w.sub_ofs + 16 * mi)
                if kind != 2:
                    raise ValueError(f"sector-pool model {mi} has no MOVR")
                sets[0].add(chunk)
                sets[1].update(self.pages(chunk))
            else:
                chunk, first, num = struct.unpack_from(">HHH", w.d, w.sect_ofs + 20 * entry + 12)
                sets[0].add(chunk)
                sets[1].update(w.texp_first + struct.unpack_from(">H", w.d, w.pp_ofs + 2 * j)[0]
                               for j in range(first, first + num))
        return sets

    def visible_set(self, eyes, dirs, kind, lo, hi):
        w, scene = self.w, self.scene
        if w.nsect > 0x8000 or w.nsub > 0x8000:
            raise ValueError("sector/model indices exceed 0x7FFF tagged sector-pool limit")
        entries = set()
        bounds = np.array([struct.unpack_from(">6h", w.d, w.sect_ofs + 20 * s) for s in range(w.nsect)])
        for start in range(0, len(eyes), 64):
            _, _, hits = scene.first_hit(eyes[start:start + 64], dirs, kind == 1, details=True)
            points = hits["points"]
            for sector, box in enumerate(bounds):
                if np.any(intersects(points, points, box[:3] - 1, box[3:] + 1)):
                    entries.add(sector)
            for mi in np.unique(hits["models"]):
                if mi and struct.unpack_from(">H", w.d, w.sub_ofs + 16 * int(mi))[0] == 2:
                    entries.add(0x8000 | int(mi))
        if kind == 0:
            placements = submodel_placements(scene.bsp, scene.ents, False)
            for mi in range(1, w.nsub):
                if struct.unpack_from(">H", w.d, w.sub_ofs + 16 * mi)[0] == 2 and any(
                        intersects(a, b, lo - 128, hi + 128) for a, b in placements[mi]):
                    entries.add(0x8000 | mi)
        return sorted(entries)

    def draw_set(self, a, eyes, delta, dirs, kind):
        """B sectors / movers hit by the rays where A and B disagree.

        An OPEN seam (no closed switch point) draws these across layers while
        the player is near it: exactly the B surfaces a home-A view gets wrong.
        """
        w, scene = self.w, self.scene
        bounds = np.array([struct.unpack_from(">6h", w.d, w.sect_ofs + 20 * s) for s in range(w.nsect)])
        entries = set()
        for start in range(0, len(eyes), 64):
            batch = eyes[start:start + 64]
            ta, ka = a.first_hit(batch, dirs, kind == 1)
            tb, kb, hits = scene.first_hit(batch + delta, dirs, kind == 1, details=True)
            agree = ((ka == 2) & (kb == 2)) | ((ka == 1) & (kb == 1) &
                        (np.abs(np.where(np.isfinite(ta), ta, -1e9) -
                                np.where(np.isfinite(tb), tb, 1e9)) <= 8))
            bad = (~agree).ravel()[hits["valid"]]
            points, models = hits["points"][bad], hits["models"][bad]
            if not len(points):
                continue
            for sector, box in enumerate(bounds):
                if np.any(intersects(points, points, box[:3] - 1, box[3:] + 1)):
                    entries.add(sector)
            for mi in np.unique(models):
                if mi and struct.unpack_from(">H", w.d, w.sub_ofs + 16 * int(mi))[0] == 2:
                    entries.add(0x8000 | int(mi))
        return sorted(entries)


def graph_for_write(graph):
    maps = graph["maps"]
    comps = [[] for _ in range(graph["components"])]
    for m in maps:
        comps[m["component"]].append(m["name"])
    return {"order": [m["name"] for m in maps], "components": comps,
            "comp_of": {m["name"]: m["component"] for m in maps},
            "gofs": {m["name"]: m["gofs"] for m in maps},
            "bounds": {m["name"]: m["bounds"] for m in maps},
            "info": {m["name"]: m for m in maps}, "start": graph["start"], "train": graph["train"]}


def map_open(budget):
    """The worst open set of a home layer with no seam pinned: view_bytes at
    every player eye (player_fits) of a 16-unit grid (endpoints included)
    over the world model's bounds; ([render, texture], [their eyes])."""
    md = budget.scene.bsp.models[0]
    lo, hi = np.array(md[:3], np.float64), np.array(md[3:6], np.float64)
    need, where = [0, 0], [None, None]
    for z in np.append(np.arange(lo[2], hi[2], 16.), hi[2]):
        eyes = eye_grid(np.array([lo[0], lo[1], z]), np.array([hi[0], hi[1], z]))
        stand, duck = player_fits(budget.scene, eyes)
        eyes = eyes[stand | duck]
        if not len(eyes):
            continue
        view, _ = view_bytes(budget, eyes)
        for pool in (0, 1):
            i = int(np.argmax(view[:, pool]))
            if view[i, pool] > need[pool]:
                need[pool], where[pool] = int(view[i, pool]), eyes[i].tolist()
    return need, where


def crossing_windows(windows, budgets, pools, report, no_budget):
    """The arrival half of each seam's crossing window and the render /
    texture pool gate. After a switch A->B, B is home: its open set at the
    arrival eyes (the final eyes mapped by delta) plus the pin of a seam from
    B whose approach box holds the arrival origin, standing or ducked
    (seam.c select_seam picks one: the largest is taken). A seam's render /
    texture need is the larger of its approach and arrival windows; the cook
    fails when a need x1.25 exceeds the pool's carve (16-aligned bytes).
    Every map's open set with no seam pinned (map_open) is gated the same."""
    worst, wide = [(0, ""), (0, "")], [(0, ""), (0, "")]
    for label, seam, entry, eyes in windows:
        bb = budgets[seam["to"]]
        arrivals = eyes + np.array(seam["delta"])
        view, leaf = view_bytes(bb, arrivals)
        pins = np.zeros((len(arrivals), 2), np.int64)
        reverse = []
        for _, other, other_entry, _ in windows:
            if other["from"] != seam["to"]:
                continue
            lo, hi = np.array(other["approach_mins"]), np.array(other["approach_maxs"])
            inside = np.zeros(len(arrivals), bool)
            for view_z in (VIEW[2], DUCK_VIEW):
                o = arrivals - [0., 0., view_z]
                inside |= np.all((lo <= o) & (o <= hi), axis=1)
            if inside.any():
                reverse.append(f"{other['from']}->{other['to']} via {other['landmark']} lump {other['trigger_lump']}")
                pins = np.where(inside[:, None], np.maximum(pins, other_entry["seam_pin"]), pins)
        arrival, arrival_eyes = [0, 0], {}
        ok = view[:, 0] >= 0
        for pool, key in ((0, "render"), (1, "texture")):
            if ok.any():
                total = np.where(ok, view[:, pool] + pins[:, pool], -1)
                i = int(np.argmax(total))
                arrival[pool] = int(total[i])
                arrival_eyes[key] = {"eye": arrivals[i].tolist(), "b_leaf": int(leaf[i]),
                                     "b_bytes": int(view[i, pool]), "pin_bytes": int(pins[i, pool])}
        entry.update(arrival_need=arrival, arrival_eyes=arrival_eyes, arrival_pins=reverse)
        widest = next(iter(entry["approach_windows"].values()), [0, 0])
        for pool in (0, 1):
            seam["need"][pool] = max(seam["need"][pool], arrival[pool])
            if seam["need"][pool] > worst[pool][0]:
                worst[pool] = (seam["need"][pool], label)
            if max(widest[pool], arrival[pool]) > wide[pool][0]:
                wide[pool] = (max(widest[pool], arrival[pool]), label)
        over = [f"{key} window {n} B x1.25 > {p} B" for key, n, p in
                zip(("render", "texture"), seam["need"][:2], pools[:2]) if n * 5 > p * 4]
        print(f"window {label}: approach {entry['approach_need'][:2]} arrival {arrival} "
              f"need {seam['need'][:2]}", flush=True)
        if over:
            entry["budget_errors"] = entry["budget_errors"] + over
            if not no_budget:
                report["errors"].append({"seam": label, "from": seam["from"], "to": seam["to"],
                                         "landmark": seam["landmark"], "trigger_lump": seam["trigger_lump"],
                                         "kind": seam["kind"], "error": "; ".join(over)})
                print(f"seam ERROR {label}: {'; '.join(over)}", flush=True)
    # no seam pinned: a home layer's open set at every eye of the map
    report["map_open"] = {}
    for name, budget in budgets.items():
        need, eyes = map_open(budget)
        label = f"map {name} (no seam)"
        report["map_open"][name] = {"need": need, "eyes": eyes}
        print(f"window {label}: open {need}", flush=True)
        for pool in (0, 1):
            for best in (worst, wide):
                if need[pool] > best[pool][0]:
                    best[pool] = (need[pool], label)
        over = [f"{key} open set {n} B x1.25 > {p} B" for key, n, p in
                zip(("render", "texture"), need, pools[:2]) if n * 5 > p * 4]
        if over and not no_budget:
            report["errors"].append({"map": name, "error": "; ".join(over)})
            print(f"map ERROR {name}: {'; '.join(over)}", flush=True)
    report["window_worst"] = {}
    for (n, label), (wn, wlabel), key, p in zip(worst, wide, ("render", "texture"), pools):
        report["window_worst"][key] = {"seam": label, "bytes": n, "widest_approach_seam": wlabel,
                                       "widest_approach_bytes": wn}
        print(f"window: worst {key} {label}: {n} B; x1.25 = {(n * 5 // 4 + 1023) // 1024} KB; "
              f"carve {p // 1024} KB; every approach at its widest: {wlabel} {wn} B, "
              f"x1.25 = {(wn * 5 // 4 + 1023) // 1024} KB", flush=True)


def cook(args):
    graph = read_cwg(args.graph)
    g = graph_for_write(graph)
    scenes, budgets = {}, {}
    for name in g["order"]:
        scenes[name] = Scene(args.maps_dir / f"{name}.bsp")
        budgets[name] = Budget(scenes[name], args.packages / f"{name}.w64")
    limits = read_limits()
    pools = [limits[k] for k in ("HL64_POOL_RENDER_KB", "HL64_POOL_TEXTURE_KB", "HL64_POOL_COLLISION_KB")]
    dirs = directions(args.rays)
    report = {"format": "CWG3", "seam_bytes": 136, "pools": pools,
              "ray_step": 6, "ray_max": 4096, "directions": args.rays,
              "seams": [], "notes": [], "errors": []}
    g["seams"], windows = [], []
    for name in g["order"]:
        a = scenes[name]
        triggers = [(i, e) for i, e in enumerate(a.ents)
                    if e.get("classname") == "trigger_changelevel" and e.get("map") in scenes]
        links = g["info"][name]["links"]
        if len(triggers) != len(links):
            raise ValueError(f"{name}: BSP changelevel/graph link count mismatch")
        for link, (lump, e) in zip(links, triggers):
            link["seam"] = NONE
            dest, lm = e["map"], e.get("landmark", "")
            label = f"{name}->{dest} via {lm} lump {lump}"
            if link["to"] != dest or link["landmark"] != lm:
                raise ValueError(f"{label}: graph link order mismatch")
            try:
                kind = 1 if key_int(e, "spawnflags") & 2 else 0
            except EntityError as error:
                raise ValueError(f"{args.maps_dir / (name + '.bsp')}: {error}") from None
            if name == dest or (kind and not e.get("targetname")):
                report["notes"].append({"seam": label, "reason": "self link" if name == dest else "USEONLY without targetname cannot fire"})
                continue
            try:
                b = scenes[dest]
                landmark = a.landmark(lm)
                delta = b.landmark(lm) - landmark
                transitions = [ent for ent in a.ents if ent.get("classname") == "trigger_transition" and ent.get("targetname") == lm]
                if len(transitions) > 1:
                    raise ValueError("multiple matching trigger_transition volumes cannot be represented by one carry box")
                volume = entity_box(a.bsp, transitions[0]) if transitions else None
                if kind:
                    lo, hi = volume if volume else (landmark - 128, landmark + 128)
                else:
                    lo, hi = entity_box(a.bsp, e)
                    lo, hi = lo - np.array([16, 16, 36]) + VIEW, hi + np.array([16, 16, 36]) + VIEW
                opened = None
                try:
                    lo, hi, flags, stats, zstep = certify_switch(a, b, delta, lo, hi, kind, dirs)
                except ProofError as proof:
                    # No closed switch point: an OPEN seam switches at the
                    # trigger and, while the player is near, draws B's
                    # disagreeing surfaces across layers (docs/formats/cwg-format.md, OPEN seams).
                    flags, stats, zstep, opened = 8, dict(proof.evidence), 16, str(proof)
                carry_lo, carry_hi = volume if volume else (lo - VIEW, hi - VIEW)
                if volume:
                    flags |= 4
                pairs, pair_report = twins(a, b, landmark, delta)
                ba, bb = budgets[name], budgets[dest]
                eyes = eye_grid(lo, hi, zstep)
                eyes = eyes[~a.solid(eyes, kind == 1)]
                if opened:
                    eyes = eyes[~b.solid(eyes + delta, kind == 1)]
                    if not len(eyes):
                        raise ValueError("open seam: no eye is empty in both maps")
                aleaves, bleaves = a.leaf_indices(eyes), b.leaf_indices(eyes + delta)
                if np.any((aleaves < 1) | (aleaves >= ba.w.nleaf)) or np.any((bleaves < 1) | (bleaves >= bb.w.nleaf)):
                    raise ValueError("switch eye does not resolve to a world visleaf")
                leaves = sorted(set(int(leaf) for leaf in bleaves))
                entries = bb.visible_set(eyes + delta, dirs, kind, lo + delta, hi + delta)
                draw = []
                if opened:
                    draw = bb.draw_set(a, eyes, delta, dirs, kind)
                    entries = draw + [x for x in entries if x not in set(draw)]
                seamset = bb.sector_set(entries)
                pin_r, pin_t, arrival = pin_sets(bb, seamset, lo, hi, delta)
                pin = [bb.chunk_bytes(pin_r), bb.chunk_bytes(pin_t)]
                # collision: A's window plus B's at each final eye
                coll, bcoll, coll_eye = 0, 0, None
                for eye, al, bl in zip(eyes, aleaves, bleaves):
                    an, bn = ba.coll_point(eye), bb.coll_point(eye + delta)
                    bcoll = max(bcoll, bn)
                    if coll_eye is None or an + bn > coll:
                        coll = an + bn
                        coll_eye = {"eye": eye.tolist(), "a_leaf": int(al), "b_leaf": int(bl),
                                    "a_bytes": an, "b_bytes": bn}
                # render / texture: A's open set at every approach eye (the
                # final eyes included) plus B's seam pin
                ap_eyes, ap_pad = approach_eyes(a, lo, hi, 512)
                ap_eyes = np.concatenate([ap_eyes, eyes])
                ap_pad = np.concatenate([ap_pad, np.zeros(len(eyes))])
                ap_view, ap_leaf = view_bytes(ba, ap_eyes)
                expansion, approach_worst, pre_eyes, pad_windows = None, [0, 0, 0], {}, {}
                for pad in (512, 384, 256, 128, 0):
                    search = None if opened else stats.get("search")
                    if search and (np.any(lo - pad > search["trigger_eye_mins"]) or
                                   np.any(hi + pad < search["trigger_eye_maxs"])):
                        continue  # An armed player must not disarm before reaching the box.
                    worst, worst_eyes = [pin[0], pin[1], 0], {}
                    sel = np.flatnonzero((ap_pad <= pad) & (ap_view[:, 0] >= 0))
                    for pool, key in ((0, "render"), (1, "texture")):
                        if len(sel):
                            i = sel[np.argmax(ap_view[sel, pool])]
                            worst[pool] += int(ap_view[i, pool])
                            worst_eyes[key] = {"eye": ap_eyes[i].tolist(), "a_leaf": int(ap_leaf[i]),
                                               "a_bytes": int(ap_view[i, pool]), "b_bytes": pin[pool]}
                    for leaf in a.leaves(lo - VIEW - pad, hi - VIEW + pad):
                        worst[2] = max(worst[2], ba.coll_leaf_max(leaf, lo - VIEW - pad, hi - VIEW + pad) + bcoll)
                    pad_windows[pad] = worst
                    approach_worst, pre_eyes = worst, worst_eyes
                    if all(n * 5 <= p * 4 for n, p in zip(worst[:2], pools[:2])) and worst[2] <= pools[2]:
                        expansion = pad
                        break
                need = [approach_worst[0], approach_worst[1], coll]
                over = [f"coll {coll}>{pools[2]}"] if coll > pools[2] else []
                if expansion is None:
                    over.append(f"no approach expansion both contains the firing region and fits pools "
                                f"(render / texture x1.25, coll); last window {approach_worst}, pools {pools}")
                    expansion = 0
                seam = {"from": name, "to": dest, "landmark": lm, "trigger_lump": lump,
                        "changetarget": e.get("changetarget", ""), "kind": kind, "flags": flags,
                        "delta": delta.tolist(), "switch_mins": lo.tolist(), "switch_maxs": hi.tolist(),
                        "portal": portal_translation(g["gofs"][name], g["gofs"][dest], delta),
                        "carry_mins": carry_lo.tolist(), "carry_maxs": carry_hi.tolist(),
                        "approach_mins": (lo - VIEW - expansion).tolist(), "approach_maxs": (hi - VIEW + expansion).tolist(),
                        "need": need, "twins": pairs, "sectors": entries, "draw": len(draw),
                        "b_dir_crc": struct.unpack_from(">I", bb.w.b, 24)[0]}
                entry = {**seam, **stats, "closed": not opened, "open_reason": opened,
                         "twins": pair_report,
                         "pools": pools, "approach_expansion": expansion,
                         "approach_need": approach_worst, "approach_windows": pad_windows,
                         "budget_errors": over,
                         "worst_eyes": {**pre_eyes, "coll": coll_eye},
                         "approach_eyes": int(np.count_nonzero(ap_pad <= expansion)),
                         "seam_pin": pin, "arrival_leafs": arrival,
                         "destination_leafs": leaves, "destination_need": [pin[0], pin[1], bcoll],
                         "final_eye_count": len(eyes), "z_step": zstep}
                report["seams"].append(entry)
                if over and not args.no_budget:
                    raise ValueError("; ".join(over))
                link["seam"] = len(g["seams"])
                g["seams"].append(seam)
                windows.append((label, seam, entry, eyes))
                print(f"seam {label}: kind {kind} flags {flags} eyes {stats['eyes']} rays {stats['rays']} bad {stats['disagreeing_rays']} twins {len(pairs)} leaves {len(leaves)} approach {expansion} window {need}/{pools}", flush=True)
            except ValueError as error:
                report["errors"].append({"seam": label, "from": name, "to": dest,
                                         "landmark": lm, "trigger_lump": lump, "kind": kind,
                                         "error": str(error),
                                         **(error.evidence if isinstance(error, ProofError) else {})})
                if isinstance(error, ProofError):
                    report["seams"].append({"from": name, "to": dest, "landmark": lm,
                                            "trigger_lump": lump, "kind": kind, "flags": 0,
                                            "closed": False, "twins": [], "sectors": [],
                                            "need": None, "pools": pools, "approach_expansion": None,
                                            **error.evidence})
                print(f"seam ERROR {label}: {error}", flush=True)
    crossing_windows(windows, budgets, pools, report, args.no_budget)
    args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    # Do not replace a graph by a partially certified campaign on failure.
    if report["errors"]:
        raise ValueError(f"{len(report['errors'])} seams rejected; see {args.report}")
    write_cwg(g, args.graph)
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("graph", type=Path)
    ap.add_argument("--maps-dir", required=True, type=Path)
    ap.add_argument("--packages", type=Path, help="default: graph directory")
    ap.add_argument("--report", required=True, type=Path)
    ap.add_argument("--rays", type=int, default=400)
    ap.add_argument("--no-budget", action="store_true", help="report over-budget proofs instead of rejecting them")
    args = ap.parse_args()
    if args.rays < 400:
        ap.error("--rays must be at least 400")
    args.packages = args.packages or args.graph.parent
    try:
        cook(args)
    except OSError as error:
        print(f"w64seam: error: {error.filename or ''}: {error.strerror or error}", file=sys.stderr)
        return 1
    except (ValueError, struct.error) as error:
        print(f"w64seam: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
