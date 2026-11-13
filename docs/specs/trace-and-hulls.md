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
  the world at p, with CURRENT_* reported as WATER — except that a point
  inside a contents brush entity returns the brush's contents
  **[probe:measured T-points, W-water]** — **unless the world is SOLID at
  p**: then SOLID (−2) wins **[probe:measured T-points]** (13 samples, all
  10 maps).
  - A **contents brush entity** is a brush entity with **SOLID_NOT** and a
    non-zero `skin`; its contents = skin. A skin on a SOLID_TRIGGER or
    SOLID_BSP brush entity is ignored (the point reads the world, −1 in the
    probe) **[probe:measured W-water]** (probe brushes with skins −2, −3,
    −4, −5, −9, −16 at solid 0 / 1 / 4, 7 maps). The skin value is returned
    as is: a SOLID_NOT brush with skin −9 (CURRENT_0) reads −9, one with
    skin −2 reads SOLID **[probe:measured W-water]**.
  - "Inside" = in a SOLID leaf of the entity's hull-0 model at its origin.
    Measured: `func_ladder` (skin LADDER) reads −16 (702 samples; the world
    there was EMPTY 689 / WATER 13) **[probe:measured T-points]**;
    `func_water` (SOLID_NOT, skin WATER) reads −3 from `pfnPointContents`
    and −3 from pmove's `PM_PointContents`, while true point contents reads
    the world (−1) **[probe:measured W-water]** (76–158 samples per map).
  Mechanism: each open layer keeps the short list of its contents brush
  entities (SOLID_NOT with skin ≠ 0; the list is updated when solid or
  skin changes); a point tests the world first (SOLID returns at once),
  then their linked boxes, then their hull 0.
- **True point contents** (pmove's `PM_TruePointContents`) stays
  world-only and keeps CURRENT_* **[probe:measured PM-bot]**; pmove adds
  physent contents itself (pmove-host.md).
- A point inside no open layer's global box is SOLID.
- Tree walk rule: at a node with plane (n, d) the signed distance is
  `n·p − d` (axial nodes: `p[axis] − d`); ≥ 0 → front child, < 0 → back
  child; a negative child is a leaf (hull 0: BSP leaf index → contents
  table; hulls 1–3: contents value).
- Depth: the deepest root-to-leaf path over every model and hull of the 10
  Uplink/Hazard maps is 146 nodes (hldemo3; the others 32–48), **measured**
  on the cooked COLL trees (Coll, 2026-11-13). Walks use an explicit stack of
  192 entries; overflow is `sys_error`, and the cook rejects any map deeper
  than 192. Depths of the retail campaign maps are not yet measured.

## 2. Hulls

| hull | box | used for |
|---|---|---|
| 0 | point | line traces, point contents |
| 1 | (−16,−16,−36)…(16,16,36) | standing player, human-size monsters |
| 2 | (−32,−32,−32)…(32,32,32) | large monsters |
| 3 | (−16,−16,−18)…(16,16,18) | ducked player, small boxes |

(HLSDK `VEC_HULL_MIN/MAX`, `VEC_DUCK_HULL_MIN/MAX`.) A clip hull is solid
space expanded by the hull box; for hulls 1–3 a moving box (mins, maxs) at
p is traced as the point `p + (mins − hull.mins)`. **Hull 0 traces the
box's origin itself**, with no `mins − hull.mins` offset: a box that
selects hull 0 (size.x ≤ 8) collides as a point at its origin, e.g. a
0 × 0 × 18 box moving up stops with its origin 1/32 u below the ceiling,
its top inside the ceiling **[probe:measured T-entities]** (`hullsel_0_*`
cases, all 10 maps).

**Precision**: signed distances to non-axial planes and the split
fractions are computed in IEEE double (GoldSrc evaluates them in x87
extended precision); single-precision float misses the ±1e-5 fraction
tolerance (t0a0 by 1.1e-5) **[probe:measured T-segments]**. Axial nodes
(`p[axis] − d`) are exact in float. The host and the cart both use IEEE
double for these steps (the VR4300 FPU has double precision in hardware);
positions and results stay float.

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
| `pHit` | entity hit (world = entity 0). On a miss (fraction 1, not start-solid): `pfnTraceLine` reports the world (0); `pfnTraceHull`, `pfnTraceModel` and `pfnTraceMonsterHull` report none (−1) **[probe:measured T-entities]** |
| `iHitgroup` | hitbox group (4.5), 0 otherwise |

Impact rules (behaviour; the mechanism is free):

- The impact is the first crossing, in travel order, from a non-solid
  region into a SOLID leaf.
- The end point stops short of the surface: its signed distance to the
  impact plane is `+1/32` u (DIST_EPSILON), computed on the node
  sub-segment being split — fraction `(t1 − ε)/(t1 − t2)` for a start in
  front, `(t1 + ε)/(t1 − t2)` behind, clamped to [0, 1], interpolated
  into the whole segment's fraction. This stand-off is what keeps
  players 1/32 u off walls. GoldSrc itself occasionally lands closer
  (3 of 26 890 world impacts at 0.0023–0.0197 u; cause not established)
  **[probe:measured T-segments]**; the rule above is expected to reproduce
  them, and T3 states how they are compared.
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
moving.mins)` for hulls 1–3 and `o = origin` for hull 0 (2: hull 0 traces
the moving origin) **[probe:measured T-entities]**.

- Unrotated: local = world − o; end point = local end + o (world space);
  the plane normal is the model-frame normal (equal to world space here);
  **the plane distance is not translated** — it stays the distance in the
  brush model's own frame (a door face at world x = −1264 reports
  d = −204) **[probe:measured T-entities]** (4699 unrotated brush-entity
  impacts).
- Rotated (any angle non-zero): with forward/right/up of `angles`, world p
  → local `((p−o)·forward, −(p−o)·right, (p−o)·up)` (rotation about o, so a
  box with its origin at its feet keeps its expansion along the model's
  axes); after an impact, end point = `start + fraction·(end − start)` in
  world space, normal rotated back to world space (`n.x·forward −
  n.y·right + n.z·up`), plane distance = the model-frame (local) distance,
  untranslated and unrotated **[probe:measured T-rotated]** (25 616 rotated
  impacts). Consequence for callers: for brush-entity hits, `vecPlaneNormal
  · vecEndPos − flPlaneDist` is not the stand-off; HLSDK game code only
  uses the normal and end point.
- `pHit` = the entity if fraction < 1 or start solid.

### 4.3 Box entities (SOLID_BBOX, SOLID_SLIDEBOX)
The target becomes the box `target.mins − moving.maxs … target.maxs −
moving.mins` at its origin; the moving origin point is traced through it.
Normals are axis-aligned; at exact edge/corner entries either face is
accepted within 0.001 u.

### 4.4 Move query
`move(start, mins, maxs, end, type, pass)`:

1. World first (4.1), giving world fraction `fw` and end point `ew`.
2. Candidates: entities in the spatial grid cells the move box (swept from
   start to `ew`, +1 u) touches (area-links-and-touch.md 2), in ascending
   entity index (6).
3. Skip: SOLID_NOT and SOLID_TRIGGER; the pass entity; entities owned by
   the pass entity; the pass entity's owner; box-vs-point pairs (pass
   entity has size, candidate has zero size); with the ignore-glass bit
   (0x100) entities whose rendermode is not Normal **[probe T-entities]**;
   - with type `ignore_monsters` (1): everything except SOLID_BSP entities
     **and SOLID_BBOX entities with MOVETYPE_PUSHSTEP** (`func_pushable`
     still blocks) **[probe:measured T-entities]**;
   - brush entities flagged FL_MONSTERCLIP (1 << 23, `func_monsterclip`)
     unless the pass entity also has FL_MONSTERCLIP (a `func_monsterclip`
     blocks only movers that carry the flag) **[probe:measured T-entities]**.
4. Type `missile` (2): candidates with FL_MONSTER are clipped as if the
   moving box were ±15 on every axis.
5. Each candidate is clipped against the segment **start → `ew`** (not the
   original end); its fraction `fe` on that segment is reported as
   `fe · fw`, its end point as `start + fe·fw·(end − start)`. A candidate
   that holds the start reports start-solid and all-solid at fraction
   `fw` and end point `ew` **[probe:measured T-entities]**, T-allsolid.
6. A candidate replaces the current result if it is all-solid, start-solid
   or nearer; a start-solid current result stays start-solid; a
   non-replacing start-solid candidate marks the result start-solid.
   **No early stop**: an all-solid result does not end the candidate loop;
   every candidate is clipped **[probe:measured T-entities]**.
7. Order: GoldSrc visits candidates in its area-node order, hl64 in
   ascending entity index. The combined result can differ only when two
   candidates tie or both hold the start (which one's plane or `pHit` is
   reported). This is a **known deviation**, classified separately by the
   tests (T12) rather than fixed: measured counts so far T2 6 / 176 000,
   T9 25 / 80 000, T5 9 / 48 600 traces **[probe:measured T-segments,
   T-allsolid, T-entities]**.

### 4.5 Hitboxes
A zero-size line trace against a SOLID_SLIDEBOX or SOLID_BBOX entity with a
studio model is tested against that model's hitboxes at the entity's
current pose (sequence, frame, controllers, blending) whenever the
segment meets the entity's **linked box** (absmin/absmax,
area-links-and-touch.md 1) — the gate is the linked box, not a hit on the
bounding box: an arm that reaches past the bbox face is hit even where the
segment misses the bbox **[probe:measured T-hitbox]**. Each hitbox is an
oriented box on its bone; the nearest hitbox entry is the impact and sets
`iHitgroup` to its group (head, chest, stomach, arms, legs per the model);
a segment that meets the linked box but misses every hitbox does not hit
the entity. Hull traces (non-zero boxes) use the bounding box only.
Mechanism: pose computed on demand through `game_studio_setup_bones`
(engine hook `col_set_hitbox_source`) and cached for the frame (D6 ii).

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
| T1 | point contents equals GoldSrc: LADDER inside every `func_ladder` (702 samples) where the world is EMPTY or WATER, SOLID where the world is SOLID (13 samples); `func_water` −3 / PM −3 / true −1; skins count only on SOLID_NOT brush entities and are returned unconverted (probe brushes: skins −2…−16 × solid 0/1/4); true point contents (pmove) stays world-only | **[probe:measured T-points, W-water]**, PM-bot |
| T2 | segment traces equal GoldSrc: flags exact, fraction ±1e-5 (non-axial plane maths in double, 2), end ±0.001 u, normal exact, distance ±1e-4; ORDER mismatches counted under T12, not as failures | **[probe:measured T-segments]**: 2000 segments ≤ 192 u per map per hull + 200 ≤ 4096 u |
| T3 | world impacts with fraction in (0,1): `normal·end − dist` ∈ [1/32 − 1e-3, 1/32 + 1e-3], except where the GoldSrc fixture for the same segment is itself short — GoldSrc has 3 of 26 890 such impacts (stand-off 0.0023–0.0197). Rule: a short impact passes only if T-segments has the same segment and hl64's end point equals GoldSrc's within T2's tolerance; any other short impact fails; the count of short impacts must not exceed GoldSrc's on the same inputs. Brush-entity impacts are excluded (their distance is model-frame, 4.2) | **[probe:measured T-segments]** + self-contained |
| T4 | end points never classify SOLID in the traced hull | self-contained |
| T5 | box entities, filters, missile ±15, owner skips, ignore-glass; `ignore_monsters` still hits SOLID_BBOX + MOVETYPE_PUSHSTEP; FL_MONSTERCLIP brushes skipped unless the passer has FL_MONSTERCLIP; candidate fractions = fraction on start → world end × world fraction, a start-stuck candidate all-solid at the world's fraction and end; no early stop after an all-solid candidate; `pHit` on a miss 0 for TraceLine, −1 for TraceHull / TraceModel / TraceMonsterHull; unrotated brush-entity impacts: end point and normal world space, distance model-frame (4699 impacts) | **[probe:measured T-entities]** |
| T6 | hull selection for sizes {0, 8, 8.01, 32, 36, 36.01, 64} × z {18, 36, 36.01, 72}; hull-0 boxes collide at their origin with no offset (a 0 × 0 × 18 box stops with its origin 1/32 u off the ceiling) | **[probe:measured T-entities]** (`hullsel_*` cases) |
| T7 | rotated brushes at yaw 0/30/90/135° and pitched with a feet-origin box: end point and normal world space, distance model-frame (25 616 impacts) | **[probe:measured T-rotated]** |
| T8 | layers: outside every layer is SOLID; a B-layer trace in global coordinates equals the B-local trace + `g_B` | self-contained |
| T9 | all-solid result fields, including start-stuck entity candidates (fraction and end of the world trace) | **[probe:measured T-allsolid]**, T-entities |
| T10 | hitboxes: hitgroups and end points ±0.01 u equal GoldSrc for line traces at scientist, barney, hgrunt, zombie in 4 poses, including hits on limbs outside the bbox but inside the linked box. The remaining 9 473 line traces per map need M3 studio poses (installed through `col_set_hitbox_source`) and are pending until then | **[probe:measured T-hitbox]** |
| T11 | no allocation, hull-walk stack ≤ 192 entries (overflow → `sys_error`), no read outside a chunk for any input incl. NaN/inf (NaN → start-solid all-solid, logged once) | host fuzz |
| T12 | ORDER classification: a mismatch counts as ORDER (known deviation, 4.4 step 7) only if GoldSrc's result equals the step-6 combination of hl64's own per-candidate results taken in some permutation; every other mismatch fails. ORDER counts are reported per capture and must not grow (baseline: T2 6 / 176 000, T9 25 / 80 000, T5 9 / 48 600) | **[probe:measured T-segments, T-allsolid, T-entities]** |
