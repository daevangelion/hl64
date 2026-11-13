# Dynamic lights

Conventions: [README.md](README.md). Lighting model: the world is
vertex-lit from the cook (lightstyles.md); dynamic lights add to it at
draw time only.

## 1. Light records

A pool of 32 lights. A light has: key (entity number or 0), origin, radius,
die time, decay (radius units per second), minlight, colour (r, g, b; the
world is mono, so the world uses the brightness, models may use the
colour). Allocation with a non-zero key reuses that key's light (an
entity has at most one light per key); otherwise the first light whose die
time has passed; otherwise light 0. Each frame, before drawing: a light
with die < time is dead; a live light's radius decreases by decay·dt and it
dies at radius ≤ 0.

Sources (radii in units, `rand32` = a uniform integer 0…31 from the render
random stream):

| source | origin | radius | life |
|---|---|---|---|
| entity EF_MUZZLEFLASH | origin + (0,0,16) + 18·forward(angles) | 200 + rand32, minlight 32 | 0.1 s |
| entity EF_BRIGHTLIGHT | origin + (0,0,16) | 400 + rand32 | one frame |
| entity EF_DIMLIGHT (not the local player) | origin | 200 + rand32 | one frame |
| local player EF_DIMLIGHT | flashlight (2) | 80 | one frame |
| game effect "dlight" (HLSDK TE_DLIGHT: radius in tens, colour, life in tenths, decay in tens) | given | given ×10 | given ×0.1, decay ×10 |
| game effect "elight" (HLSDK TE_ELIGHT) | entity attachment | given | models only (does not light the world) |
| explosions, muzzle flashes of the view model | render-fx.md | | |

## 2. Flashlight (observable behaviour of the oracle and retail HL)

While the local player has EF_DIMLIGHT: a line trace (NORMAL, pass the
player) from the eye (origin + view height) 2000 u along the view
direction (pitch and yaw only). The light sits at the trace end point; if
the trace hit a studio or sprite entity, at that entity's origin instead.
Radius 80. Brightness 1 up to 500 u from the eye; beyond, (500 / d)² where
d = 2000·fraction, quantised to 1/255. Recomputed every frame.

## 3. On the world

- Only meshlets whose bounding box meets a live light's sphere are
  touched; they are copied into a per-frame scratch buffer (512 vertex
  pairs, double-buffered) and lit there; the cooked meshlet is never
  modified.
- Per vertex: with the face plane's signed distance p from the light and
  the in-plane distance q from the light's foot on that plane to the vertex
  (Euclidean), the contribution is `max(0, (radius − |p|) − q)` if
  `radius − |p|` ≥ minlight, else 0; contributions of all lights add to the
  vertex's base intensity before the light scale, then clamp 255.
  (Back faces are culled anyway.)
- Movers are not dynamically lit (oracle), except by the flashlight pool.
- **Flashlight pool**: because the world's faces are not subdivided, a
  vertex-only light leaves most of an 80 u spot dark. The flashlight is
  drawn instead as a second pass over the base triangles within its
  radius: on each face plane near the light, the falloff (radius − plane
  distance − in-plane distance) forms a cone over the light's foot,
  approximated by a fan of 8 wedge triangles (chord sag 7.6 % of the
  radius); each wedge piece cut by a base triangle is drawn with decal
  depth, shade = base light + falloff, texture coordinates from the base
  triangle's affine gradients, so it joins the base exactly where the
  falloff ends. Pool vertices are stored relative to the light at 1/64 u
  under one extra matrix so pieces stay on their base plane. Counters:
  pool verts and triangles drawn per frame.

## 4. On models

Model intensity = probe light (lightstyles.md 4) + Σ over live lights
(except "world only" lights) of `max(0, radius − |model origin − light
origin|)`, floored at 32 and clamped at 255. Elights apply only to the
entity they were attached to.

## 5. Tests

| # | property | data |
|---|---|---|
| D1 | pool: key reuse, oldest-dead reuse, decay to death at radius 0, one-frame lights vanish next frame | self-contained |
| D2 | vertex falloff formula for synthetic planes (p, q grid) including the minlight cutoff | self-contained |
| D3 | only meshlets meeting a light sphere are copied; the cooked meshlet bytes are unchanged after the frame | synthetic + memory compare |
| D4 | flashlight: end point and brightness for traces at 100, 500, 1000, 2000 u and on a monster | synthetic |
| D5 | flashlight screenshots at the route's `flashlight-on` step (dark duct, room) match the oracle (SSIM ≥ 0.9, same pose) | fixture screenshots |
| D6 | muzzle flash light appears for 0.1 s per shot; the 9mm in the dark duct brightens the walls in the oracle's frame range | fixture screenshots |
| D7 | cost: flashlight pass ≤ the oracle's measured per-frame µs in the dark duct (PROFILE zone) | M6 |
