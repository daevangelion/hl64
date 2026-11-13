# Linked boxes, spatial grid, touches

Behaviour: which entities a move collides with and which triggers it
touches, as in GoldSrc. Mechanism: a uniform grid per open layer instead of
GoldSrc's area-node tree (its node order is not observable except in the
touch order of overlapping triggers, which hl64 fixes to entity index).
Conventions and oracles: [README.md](README.md).

## 1. Linked box (absmin / absmax)

Recomputed on every link:

1. Rotated brush (SOLID_BSP, any angle non-zero): a cube centred on the
   origin with half-size = length of the vector of per-axis
   max(|mins|, |maxs|).
2. Otherwise `origin + mins … origin + maxs`.
3. FL_ITEM: x and y extend by 15 u each side; everything else: every axis
   extends by 1 u each side (moves stop 1/32 u short, contact must still
   overlap) **[probe A-touch]**.

## 2. Spatial grid (mechanism)

- Each open layer has a grid over its global box with 256 u cells (the COLL
  grid's cells): one u16 list head per cell, one u16 next per entity
  (singly linked per cell). An entity whose linked box spans more than 8
  cells goes to the layer's **large** list instead. Entities link to the
  layer containing their box centre; SOLID_NOT entities are not linked.
- Triggers and solids share the grid; each entity carries its solid type,
  so a query filters by one byte.
- A query (box) walks the touched cells plus the large list, deduplicates
  with a per-query stamp, and returns candidates in **ascending entity
  index**.
- Cost bound: a move box ≤ 512 u touches ≤ 27 cells; cell occupancy on
  the Uplink route is expected ≤ 8 [EST, M3 measures].
- Leaf/sector membership for visibility is not stored per link
  (architecture.md 5.5: sectors for rendering, exact leaf on demand).

## 3. Trigger touch

When an entity is linked **with touch** (all physics motion, the player
move, walk moves; not `set_origin`, not `drop_to_floor`, not a pusher's own
link), every SOLID_TRIGGER entity with a touch function whose linked box
overlaps the mover's linked box (touching faces count) is touched,
`self` = trigger, `other` = mover, in ascending entity index, provided:

- it is not the mover;
- for a brush trigger (the trigger has a BSP model): the mover's origin,
  traced as a point in the trigger's hull chosen for the mover's box
  (trace-and-hulls.md 2), is inside the trigger's solid — so an angled or
  non-box trigger fires only inside its shape, not anywhere in its bounding
  box **[probe A-shape]**. (The old tree tested boxes only; GoldSrc's
  behaviour wins.)

Touches may free or relink any entity; the pass continues over the
candidate list taken before the first call, skipping entities freed since.

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
if A has a touch function and is not SOLID_NOT, then B.touch(A) under the
same rule re-checked after the first call. A caller checks A's liveness
before continuing.

## 5. Tests

| # | property | data |
|---|---|---|
| A1 | linked box rules (unrotated, FL_ITEM, rotated cube, SOLID_NOT not linked) | self-contained + probe A-touch (absmin/absmax read back) |
| A2 | grid: every entity found by any box query that overlaps its linked box, none returned twice, order ascending; large-list entities found from every cell | randomised host test |
| A3 | trigger touch once per link for each overlapping trigger, including face contact; not for a trigger 0.01 u away | probe A-touch |
| A4 | `set_origin` into a trigger touches nothing; the next physics move does | probe A-touch |
| A5 | angled brush trigger: a player inside its bbox but outside its brush does not fire it; inside it does | probe A-shape |
| A6 | touch that frees the trigger / another trigger / the toucher: no crash, no touch of freed entities | synthetic |
| A7 | impact touch order and SOLID_NOT rule | probe A-touch |
| A8 | pending record hydrated and touched in the same frame | synthetic |
