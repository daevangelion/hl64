# Traces, clip hulls and point contents

Covers what every collision query in hl64 returns. Format of the cooked
trees: `docs/formats/w64-format.md` (COLL chunk, model table, grid).
Conventions: [README.md](README.md).

## 1. Contents

A point in a map is classified by the leaf of a hull tree that holds it.
Values (HLSDK `const.h`): EMPTY −1, SOLID −2, WATER −3, SLIME −4, LAVA −5,
SKY −6, CURRENT_0…CURRENT_DOWN −9…−14, TRANSLUCENT −15, LADDER −16
(LADDER and the brush-entity contents below come only from entities).
CLIP (−8) and ORIGIN (−7) never appear in cooked leaves (the compiler folds
them into the clip hulls).

**Point contents** of the world at a point = the hull-0 leaf contents of
the layer containing the point, with every CURRENT_* value reported as
WATER. **True point contents** = the same without the CURRENT mapping. A
point inside no open layer's global box is SOLID (both functions).

A tree is walked from a root: at a node with plane (n, d), the signed
distance is `n·p − d` (for an axial node, `p[axis] − d`); distance ≥ 0 goes
to the front child, < 0 to the back child; a negative child is a leaf.
Hull 0 leaves are BSP leaf indices whose contents come from the layer's
leaf-contents table; hulls 1–3 leaves are contents values directly.

## 2. Hulls

| hull | box (mins … maxs) | used for |
|---|---|---|
| 0 | point | line traces, point contents, rendering leaf |
| 1 | (−16,−16,−36) … (16,16,36) | standing player, human-size monsters |
| 2 | (−32,−32,−32) … (32,32,32) | large monsters |
| 3 | (−16,−16,−18) … (16,16,18) | ducked player, small boxes |

(The standing and duck boxes are HLSDK `VEC_HULL_MIN/MAX`,
`VEC_DUCK_HULL_MIN/MAX`.) A clip hull is the map's solid space expanded by
the hull box, so tracing one point through hull k is equivalent to sweeping
the hull's box. A moving box (mins, maxs) at origin p is traced as the point
`p + (mins − hull.mins)`: its min corner is aligned with the hull box's min
corner (4.2 gives the same rule as an offset on the target).

### 2.1 Hull selection for a moving box against a BSP model

With `size = maxs − mins` of the moving box:

1. size.x ≤ 8 → hull 0;
2. else size.x ≤ 36 → hull 3 if size.z ≤ 36, otherwise hull 1;
3. else hull 2.

Only size.x and size.z are consulted. A box that matches no hull exactly
(e.g. a 24×24×48 monster) uses the selected hull's expansion, so its
effective collision box is the hull's, not its own: this is the GoldSrc
behaviour and the oracle's.

## 3. Segment trace result

A trace of the segment start→end returns:

| field | meaning |
|---|---|
| `allsolid` | no part of the segment examined lies in a non-solid leaf |
| `startsolid` | the start point lies in a SOLID leaf |
| `inopen`, `inwater` | some examined part lies in an EMPTY leaf / in a non-empty, non-solid leaf |
| `fraction` | 0…1, the part of the segment travelled before impact; 1 = no impact |
| `endpos` | `start + fraction·(end − start)` (world frame) |
| `plane` | normal and distance of the impacted surface, oriented to face the side the segment came from |
| `ent` | the entity hit (world = entity 0), or none when fraction = 1 and not startsolid |
| `hitgroup` | always 0 (no studio hitboxes; see D6) |

Impact rules (these are the observable semantics; implement them however
is clearest, e.g. an iterative walk with an explicit stack):

- The segment is split at every node plane it crosses. The impact is the
  first crossing, in travel order, from a non-solid region into a SOLID
  leaf.
- The reported point is not on the plane: it is pulled back toward the
  start so that its signed distance to the impact plane is `+DIST_EPSILON`
  = 1/32 u, measured on the sub-segment being split (fraction computed as
  `(t1 − ε)/(t1 − t2)` for a start on the front side, `(t1 + ε)/(t1 − t2)`
  for the back side, where t1, t2 are the sub-segment ends' signed
  distances; clamped to [0, 1]; the fraction of the whole segment is the
  linear interpolation of the sub-segment's own fractions).
- If the pulled-back point still classifies as SOLID in the hull (possible
  near acute corners), it is moved further back in steps of 0.1 of the
  sub-segment fraction until it is not; if that would go below 0, the
  trace reports the last computed point.
- Leaf crossings that do not enter SOLID (EMPTY↔WATER etc.) are not
  impacts; they only set `inopen` / `inwater`.
- `allsolid` is decided by the leaves the walk visits; a segment whose start
  and end are both in SOLID and that never visits a non-solid leaf is
  `allsolid` and `startsolid`.
- **allsolid keeps fraction 1 and endpos = end** in the oracle (and in
  hl64): callers must test `allsolid`/`startsolid` before trusting
  fraction. (Player movement applies its own rule, pmove-host.md.)
- A startsolid trace that leaves the solid later still reports the first
  later impact (fraction may be < 1) and keeps `startsolid`.

## 4. Traces against entities

### 4.1 The world

The world is entity 0. With several open layers (architecture.md 5.4),
the world part of a trace is computed per layer whose global box meets the
move box, in that layer's local frame (subtract its `g_M`), and translated
back; the nearest impact wins, ties to the lower layer slot. Maps are packed
≥ 128 u apart, so in practice one layer answers.

### 4.2 Brush entities (SOLID_BSP)

The entity's model hull k (chosen by 2.1 from the moving box) is placed at
offset `o = entity.origin + (hull_k.mins − moving.mins)` (hull 0: `mins` of
the hull is the point, so `o = origin − moving.mins` reduces to origin for a
point). The segment is traced in the model's frame:

- **Unrotated** (all angles zero, or SOLID not BSP): local = world − o.
  Result endpos = local endpos + o; plane distance += normal·o.
- **Rotated** (SOLID_BSP and any of pitch/yaw/roll non-zero): with
  forward/right/up of the entity's `angles` (README conventions, angles used
  as given), a world point p maps to local
  `((p−o)·forward, −(p−o)·right, (p−o)·up)`. Note the rotation is about
  `o`, not about the origin: for a moving box whose origin is not its
  centre (a monster at its feet), the hull expansion lies along the model's
  axes. After an impact: `endpos = start + fraction·(end − start)` computed
  in world space (not the rotated-back local endpos), the plane normal is
  rotated back (`n_w = n.x·forward − n.y·right + n.z·up`), and the plane
  distance is the local distance + `n_w·o`.
- `ent` = the entity if fraction < 1 or startsolid.

This rotated-brush rule is "the rotated-brush fix" of the old tree: before
it, rotated doors used their unrotated hull.

### 4.3 Box entities (SOLID_BBOX, SOLID_SLIDEBOX)

The target becomes a box hull from `target.mins − moving.maxs` to
`target.maxs − moving.mins` at the target's origin, and the moving box's
origin point is traced through it. Normals are axis-aligned. When the
segment enters exactly through an edge or corner, which face is reported is
oracle-defined (fixture T5 accepts either face when the entry point is
within 0.001 u of the edge).

### 4.4 Combining: the move query

`move(start, mins, maxs, end, type, pass)` returns one result:

1. Trace against the world (4.1). If allsolid, stop.
2. Collect entities whose linked box (area-links-and-touch.md) overlaps the
   box swept by the move up to the world's endpos, expanded by 1 u on every
   axis (with `type` MISSILE the sweep uses the ±15 box from rule 4).
3. Skip: SOLID_NOT; triggers (never in the solid lists); the pass entity;
   with type NOMONSTERS, everything that is not SOLID_BSP; if the pass entity
   has non-zero size.x and the candidate has zero size.x (box-vs-point never
   collide); entities owned by the pass entity; the pass entity's owner.
4. With type MISSILE, candidates flagged FL_MONSTER are clipped as if the
   moving box were (−15,−15,−15)…(15,15,15) (missiles hit monsters
   generously); everything else uses the real box.
5. Each candidate's result replaces the current one if it is allsolid,
   startsolid, or has a smaller fraction; if the current result was
   startsolid the replacement keeps `startsolid` set. A candidate that does
   not replace but is startsolid sets `startsolid` on the current result.
6. Iteration order over candidates is the area tree order
   (area-links-and-touch.md 3); it only matters for exact fraction ties
   (first found wins).

Type values: NORMAL 0, NOMONSTERS 1, MISSILE 2 (HLSDK `dont_ignore_monsters`,
`ignore_monsters`, `missile`). GoldSrc's ignore-glass bit (0x100) is
accepted and, when set, also skips entities whose rendermode is not Normal
[GoldSrc documented behaviour; not exercised by the oracle].

### 4.5 Position test

An entity is **stuck** at its origin when `move(origin, mins, maxs, origin,
NORMAL, self)` is startsolid.

### 4.6 Line traces by the game

`trace_line(a, b, type, skip)` = move with zero box (hull 0 for BSP, points
vs boxes as 4.3). `trace_hull(a, b, type, k, skip)` uses the box of hull k.
`trace_model(a, b, k, e)` = 4.2/4.3 against one entity only.
`trace_texture(e, a, b)` returns the surface material code from the cooked
material table at the first hull-0 crossing out of a non-solid leaf on
that entity's model (w64-format.md "Surface materials").

## 5. Tests

| # | property | data |
|---|---|---|
| T1 | point contents and true point contents equal the oracle for every point; hulls 0–3 contents of the world and of 8 submodels equal the oracle | fixture T-points: 20 000 random points per Uplink/Hazard map, ¼ snapped to the 1/16 u lattice |
| T2 | for every segment: allsolid, startsolid, inopen, inwater equal; fraction ±1e-5; endpos ±0.001 u; plane normal exact (bitwise) and distance ±1e-4 | fixture T-segments: 2000 segments ≤ 192 u per map per hull 0–3, plus 200 ≤ 4096 u |
| T3 | for every non-startsolid impact with fraction in (0,1) and no back-off: `plane.normal·endpos − plane.dist` ∈ [1/32 − 1e-3, 1/32 + 1e-3] | self-contained on T-segments inputs |
| T4 | endpos of any impact classifies as non-SOLID in the traced hull | self-contained |
| T5 | box entity clipping: start outside → first contact at the Minkowski box; normals axis-aligned; edge ties as 4.3 | synthetic + fixture T-entities (doors, crates, monsters, items) |
| T6 | hull selection table 2.1 for sizes {0, 8, 8.01, 32, 36, 36.01, 64} × z {18, 36, 36.01, 72} | self-contained |
| T7 | rotated brush: a trace against a rotated door equals a trace of the inverse-rotated segment through the unrotated door (synthetic); normal rotated back; endpos from world fraction; fixture of `func_door_rotating` at 0°, 30°, 90°, 135° yaw and a pitched (fallen) door with a monster box whose origin is at its feet | synthetic + fixture T-rotated |
| T8 | move filters: NOMONSTERS ignores boxes; MISSILE vs monster uses ±15 box; owner/owned skipped both ways; point vs box skipped only when the mover has size; pass entity skipped | synthetic scene in the host test harness |
| T9 | layers: a point outside every open layer is SOLID; a trace in layer B at global coordinates equals the same trace in B-local coordinates translated by `g_B` | self-contained, two cooked layers |
| T10 | allsolid: a segment entirely inside solid reports allsolid, startsolid, fraction 1, endpos = end | self-contained |
| T11 | no allocation, no recursion beyond the depth bound, no read outside a COLL chunk for any input including NaN/inf coordinates (NaN input → startsolid allsolid result, logged once) | fuzz on host |
