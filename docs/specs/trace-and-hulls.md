# Traces, clip hulls and point contents

Behaviour: what every collision query returns, as GoldSrc returns it.
Mechanism: cooked COLL trees (`docs/formats/w64-format.md`), iterative
walks with an explicit stack, map-local coordinates. Conventions and
oracles: [README.md](README.md).

## 1. Contents

Values (HLSDK `const.h`): EMPTY −1, SOLID −2, WATER −3, SLIME −4, LAVA −5,
SKY −6, CURRENT_0…CURRENT_DOWN −9…−14, TRANSLUCENT −15, LADDER −16 (LADDER
and brush-entity contents come only from entities). CLIP and ORIGIN never
appear in cooked leaves.

- **Point contents** at p (`pfnPointContents`): the hull-0 leaf contents of
  the world at p, with CURRENT_* reported as WATER. **True point contents**
  keeps CURRENT_*. Brush entities do not contribute (GoldSrc's point
  contents is world-only; pmove adds its own physent contents,
  pmove-host.md) **[probe]**.
- A point inside no open layer's global box is SOLID.
- Tree walk rule: at a node with plane (n, d) the signed distance is
  `n·p − d` (axial nodes: `p[axis] − d`); ≥ 0 → front child, < 0 → back
  child; a negative child is a leaf (hull 0: BSP leaf index → contents
  table; hulls 1–3: contents value).

## 2. Hulls

| hull | box | used for |
|---|---|---|
| 0 | point | line traces, point contents |
| 1 | (−16,−16,−36)…(16,16,36) | standing player, human-size monsters |
| 2 | (−32,−32,−32)…(32,32,32) | large monsters |
| 3 | (−16,−16,−18)…(16,16,18) | ducked player, small boxes |

(HLSDK `VEC_HULL_MIN/MAX`, `VEC_DUCK_HULL_MIN/MAX`.) A clip hull is solid
space expanded by the hull box; a moving box (mins, maxs) at p is traced as
the point `p + (mins − hull.mins)`.

**Hull selection** for a moving box against a BSP model, `size = maxs −
mins`: size.x ≤ 8 → hull 0; else size.x ≤ 36 → hull 3 if size.z ≤ 36,
otherwise hull 1; else hull 2. A box that matches no hull uses the selected
hull's expansion (a 24×24×48 box collides as hull 1) **[probe T-entities]**.

## 3. Segment trace result

| field (HLSDK `TraceResult`) | meaning |
|---|---|
| `fAllSolid` | no part of the examined segment is in a non-solid leaf |
| `fStartSolid` | the start point is in SOLID |
| `fInOpen`, `fInWater` | some examined part is in EMPTY / in a liquid leaf |
| `flFraction` | 0…1 travelled before impact; 1 = no impact |
| `vecEndPos` | `start + fraction·(end − start)` (world frame) |
| `vecPlaneNormal`, `flPlaneDist` | impacted surface, oriented to face the side the segment came from |
| `pHit` | entity hit (world = entity 0); none when fraction = 1 and not startsolid |
| `iHitgroup` | hitbox group (4.5), 0 otherwise |

Impact rules (behaviour; the mechanism is free):

- The impact is the first crossing, in travel order, from a non-solid
  region into a SOLID leaf.
- The end point stops short of the surface: its signed distance to the
  impact plane is `+1/32` u (DIST_EPSILON), computed on the node
  sub-segment being split — fraction `(t1 − ε)/(t1 − t2)` for a start in
  front, `(t1 + ε)/(t1 − t2)` behind, clamped to [0, 1], interpolated
  into the whole segment's fraction. This stand-off is what keeps
  players 1/32 u off walls.
- If that point still classifies SOLID (acute corners), it moves back in
  steps of 0.1 of the sub-segment until it does not, never past the start.
- Non-solid leaf changes (EMPTY ↔ WATER) are not impacts.
- A segment whose start is in SOLID reports `fStartSolid`; if it never
  reaches a non-solid leaf it also reports `fAllSolid`, with fraction 1 and
  end = end point **[probe T-allsolid]** (HLSDK game code tests the solid
  flags before the fraction).
- A start-solid trace that leaves the solid reports the first later impact
  and keeps `fStartSolid`.

## 4. Traces against entities

### 4.1 World and layers
The world is entity 0. Each open layer whose global box meets the move box
is traced in its own frame (global − `g_M`) and translated back; nearest
impact wins, ties to the lower slot.

### 4.2 Brush entities (SOLID_BSP)
Hull k by 2 from the moving box, placed at `o = origin + (hull.mins −
moving.mins)`.

- Unrotated: local = world − o; end point = local end + o; plane distance
  += normal·o.
- Rotated (any angle non-zero): with forward/right/up of `angles`, world p
  → local `((p−o)·forward, −(p−o)·right, (p−o)·up)` (rotation about o, so a
  box with its origin at its feet keeps its expansion along the model's
  axes); after an impact, end point = `start + fraction·(end − start)` in
  world space, normal rotated back (`n.x·forward − n.y·right + n.z·up`),
  plane distance = local distance + normal·o **[probe T-rotated]**.
- `pHit` = the entity if fraction < 1 or start solid.

### 4.3 Box entities (SOLID_BBOX, SOLID_SLIDEBOX)
The target becomes the box `target.mins − moving.maxs … target.maxs −
moving.mins` at its origin; the moving origin point is traced through it.
Normals are axis-aligned; at exact edge/corner entries either face is
accepted within 0.001 u.

### 4.4 Move query
`move(start, mins, maxs, end, type, pass)`:

1. World first; if all-solid, stop.
2. Candidates: entities in the spatial grid cells the move box (swept to the
   world's end point, +1 u) touches (area-links-and-touch.md 2), in
   ascending entity index.
3. Skip: SOLID_NOT and SOLID_TRIGGER; the pass entity; with type
   `ignore_monsters` (1) everything not SOLID_BSP; with the ignore-glass
   bit (0x100) entities whose rendermode is not Normal; box-vs-point pairs
   (pass entity has size, candidate has zero size); entities owned by the
   pass entity; the pass entity's owner **[probe T-entities]**.
4. Type `missile` (2): candidates with FL_MONSTER are clipped as if the
   moving box were ±15 on every axis.
5. A candidate replaces the current result if it is all-solid, start-solid
   or nearer; a start-solid current result stays start-solid; a
   non-replacing start-solid candidate marks the result start-solid.

### 4.5 Hitboxes
A zero-size line trace that hits the bounding box of a SOLID_SLIDEBOX or
SOLID_BBOX entity with a studio model is refined against that model's
hitboxes at the entity's current pose (sequence, frame, controllers,
blending): each hitbox is an oriented box on its bone; the nearest hitbox
entry replaces the bbox impact and sets `iHitgroup` to its group (head,
chest, stomach, arms, legs per the model); a trace that passes the bbox but
misses every hitbox does not hit the entity. Hull traces (non-zero boxes)
use the bounding box only. **[probe T-hitbox]**; mechanism: pose computed on
demand through `game_studio_setup_bones` and cached for the frame (D6 ii).

### 4.6 Position test
An entity is stuck when `move(origin, mins, maxs, origin, 0, self)` is
start-solid.

### 4.7 Game-facing variants
Line (zero box, hitboxes as 4.5), hull k, single model (4.2/4.3 against one
entity), texture: the cooked material code of the first hull-0 crossing out
of a non-solid leaf on that entity's model (w64-format.md "Surface
materials"; HLSDK `TEXTURETYPE` letters).

## 5. Tests

| # | property | data |
|---|---|---|
| T0 | cooked COLL walks equal BSP clipnode walks (contents, path) for every point/segment of the old cook proof | self-contained on the player's BSPs (w64check rules) |
| T1 | point contents and true point contents equal GoldSrc | probe T-points: 20 000 points per map, ¼ on the 1/16 u lattice |
| T2 | segment traces equal GoldSrc: flags exact, fraction ±1e-5, end ±0.001 u, normal exact, distance ±1e-4 | probe T-segments: 2000 segments ≤ 192 u per map per hull + 200 ≤ 4096 u |
| T3 | non-solid impacts with fraction in (0,1) and no back-off: plane distance of the end point ∈ [1/32 ± 1e-3] | self-contained |
| T4 | end points never classify SOLID in the traced hull | self-contained |
| T5 | box entities, filters, missile ±15, owner skips, ignore-glass | probe T-entities |
| T6 | hull selection for sizes {0, 8, 8.01, 32, 36, 36.01, 64} × z {18, 36, 36.01, 72} | probe T-entities (entities of those sizes traced against a door) |
| T7 | rotated brushes at yaw 0/30/90/135° and pitched with a feet-origin box | probe T-rotated |
| T8 | layers: outside every layer is SOLID; a B-layer trace in global coordinates equals the B-local trace + `g_B` | self-contained |
| T9 | all-solid result fields | probe T-allsolid |
| T10 | hitboxes: head/chest/leg shots on scientist, barney, hgrunt, zombie at 4 poses give GoldSrc's hitgroups and end points ±0.01 u | probe T-hitbox |
| T11 | no allocation, bounded stack, no read outside a chunk for any input incl. NaN/inf (NaN → start-solid all-solid, logged once) | host fuzz |
