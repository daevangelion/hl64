# Linked boxes, spatial grid, touches

Behaviour: which entities a move collides with and which triggers it
touches, and in which order, as in GoldSrc. Mechanism: a uniform grid per
open layer instead of GoldSrc's area-node tree, plus a cooked touch rank
that reproduces GoldSrc's trigger touch order (3.1).
Conventions and oracles: [README.md](README.md).

## 1. Linked box (absmin / absmax)

Recomputed on every link:

1. Rotated brush (SOLID_BSP, any angle non-zero): a cube centred on the
   origin whose half-size is the **largest** per-axis value of
   max(|mins[i]|, |maxs[i]|) — not the length of that vector, so the
   corners of a long rotated brush can stick out of its linked box — then
   widened as in 3; e.g. extents (124, 173, 17) give half-size 174
   **[probe:measured A-touch]** (rotated brush entities on all 10 maps).
2. Otherwise `origin + mins … origin + maxs`. Only SOLID_BSP entities use
   rule 1: a trigger (SOLID_TRIGGER) with angles keeps its unrotated box
   **[probe:measured A-shape]**.
3. Every linked box is then widened by 1 u on every axis, whatever its
   solid type and flags **[probe:measured A-touch]**. There is no item
   widening: HLSDK has no FL_ITEM, and bit 256 (FL_SKIPLOCALHOST) gets no
   extra 15 u (the old tree's Quake rule is not GoldSrc).

## 2. Spatial grid (mechanism)

- Each open layer has a grid over its global box with 256 u cells (the COLL
  grid's cells): one u16 list head per cell, one u16 next per entity
  (singly linked per cell). An entity whose linked box spans more than 8
  cells goes to the layer's **large** list instead. Entities link to the
  layer containing their box centre; SOLID_NOT entities are not linked.
- Triggers and solids share the grid; each entity carries its solid type,
  so a query filters by one byte.
- A query (box) walks the touched cells plus the large list and
  deduplicates with a per-query stamp. Solid candidates (move queries) are
  returned in ascending entity index (the order only matters for exact
  fraction ties, trace-and-hulls.md 4.4); trigger candidates are returned
  in touch-rank order (3.1).
- Cost bound: a move box ≤ 512 u touches ≤ 27 cells; cell occupancy on
  the Uplink route is expected ≤ 8 [EST, M3 measures].
- Leaf/sector membership for visibility is not stored per link
  (architecture.md 5.5: sectors for rendering, exact leaf on demand).

## 3. Trigger touch

When an entity is linked **with touch** (all physics motion, the player
move, walk moves; not `set_origin`, not `drop_to_floor`, not a pusher's own
link), every SOLID_TRIGGER entity with a touch function whose linked box
overlaps the mover's linked box (touching faces count) is touched,
`self` = trigger, `other` = mover, provided:

- it is not the mover;
- for a brush trigger (the trigger has a BSP model): the mover's origin is
  inside the trigger brush's **hull 1** (standing-player hull) solid,
  tested in the model's unrotated frame — the trigger's angles are ignored
  (a yaw-30° trigger fires on its unrotated shape) — so a non-box trigger
  fires only inside its shape, not anywhere in its bounding box
  **[probe:measured A-shape]** (960 samples, player-size movers). Movers of
  other sizes are not yet measured; until they are, hl64 uses hull 1 for
  every mover **[probe]**.

Because both linked boxes are widened by 1 u, a mover and a box trigger
that overlap on two axes touch while the gap between their real boxes on
the third is ≤ 2.0 u, and not at 2.01 u **[probe:measured A-touch]** (each
axis; FLY movers and the fake client).

Touches may free or relink any entity; the pass continues over the
candidate list taken before the first call, skipping entities freed since.

### 3.1 Touch order

GoldSrc touches overlapping triggers in its area-node order, not entity
index order (a trigger of index 180 was touched before one of index 10
**[probe:measured A-touch]**). The order is observable when two triggers
fire in the same link (one may kill or disable the other, or their fires
race). hl64 reproduces it without GoldSrc's tree:

- The cook lists, per map, every pair of triggers whose linked boxes are
  within 64 u of each other (a mover box can overlap both); the GoldSrc
  probe places a mover overlapping both and records which is touched first
  (capture **A-order**, owed before M3). The cook turns the pairwise orders
  into a per-map **touch rank** (u16 in the HLE1 record; a topological
  order of the pair graph, ties by lump) and fails if the pairs are
  inconsistent (a cycle) — then the spec needs revisiting.
- At run time trigger candidates are sorted by (map, rank); triggers
  created at run time (none in Uplink/Hazard) rank after all map triggers,
  by index.
- Known limit: in GoldSrc a trigger that is relinked (moved, re-enabled)
  can change its position in its area-node list; hl64's rank is static.
  Uplink and Hazard triggers do not move; the A-order capture also records
  re-enabled triggers (`trigger_multiple` toggled by `use`) so the limit is
  measured, not assumed.

**Force retouch**: for the next N frames every live entity in the active
set and every entity resting inside a trigger box is relinked with touch
(HLSDK sets `gpGlobals->force_retouch` when triggers are enabled).
Mechanism: the engine keeps, per trigger, nothing extra; it relinks the
entities whose boxes overlap any trigger (grid query per trigger), which is
what is observable.

**Pending-record wake**: before the touch pass, pending map records whose
cooked bounds meet the mover's box are hydrated (architecture.md 6.4) so a
trigger that was never spawned is touched in the same link.

## 4. Impact touch

When a physics move stops on entity B (trace `pHit` = B): first A.touch(B)
if A has a touch function and is not SOLID_NOT; then, **only if A still
exists**, B.touch(A) if B has a touch function and is not SOLID_NOT — an A
that removes itself in its own touch gets no B.touch(A)
**[probe:measured A-touch]**. A SOLID_NOT mover passes through box
entities without stopping and without any impact touch (its moves ignore
monsters, physics-movetypes.md 2) **[probe:measured A-touch]**. A caller
checks A's liveness before continuing.

## 5. Tests

| # | property | data |
|---|---|---|
| A1 | linked box: unrotated box + 1 u on every axis for every solid type and flag (no 15 u widening for bit 256); rotated SOLID_BSP cube half-size = largest per-axis extent + 1 (e.g. (124, 173, 17) → 174) on all 10 maps; angled triggers keep unrotated boxes | **[probe:measured A-touch]**, A-shape |
| A2 | grid: every entity found by any box query that overlaps its linked box, none returned twice; solids in ascending index, triggers in rank order; large-list entities found from every cell | randomised host test |
| A3 | box trigger touched once per link when the real-box gap is ≤ 2.0 u on any one axis (and overlapping on the others), including face contact; not at 2.01 u; same for FLY movers and the player | **[probe:measured A-touch]** |
| A4 | `set_origin` into a trigger touches nothing; the next physics move does | probe A-touch |
| A5 | brush trigger fires iff the mover's origin is inside its hull-1 brush, trigger angles ignored (a yaw-30° trigger uses its unrotated shape); inside the bbox but outside the brush never fires | **[probe:measured A-shape]** (960 samples) |
| A6 | touch that frees the trigger / another trigger / the toucher: no crash, no touch of freed entities | synthetic |
| A7 | impact touch: A.touch(B) then B.touch(A); no B.touch(A) when A removed itself in A.touch(B); a SOLID_NOT mover passes through box entities with no impact and no touch | **[probe:measured A-touch]** |
| A8 | pending record hydrated and touched in the same frame | synthetic |
| A9 | touch order of every trigger pair within 64 u equals GoldSrc's (cooked rank), including the measured case: index 180 before index 10 | **[probe:measured A-touch]** for the known case; A-order capture (owed) for all pairs |
