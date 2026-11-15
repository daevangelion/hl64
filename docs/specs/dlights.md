# Dynamic lights

**Oracle status: awaiting a retail-client capture plan.** Dynamic lights
exist only in the client; the HLDS probe cannot observe them
(`GetEntityIllum` = 128 everywhere, capture L-illum). Every `[probe]`
property here is to be measured on the retail client (README "Oracles").

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
| local player EF_DIMLIGHT | none: night vision (2) | | |
| game effect "dlight" (HLSDK TE_DLIGHT: radius in tens, colour, life in tenths, decay in tens) | given | given ×10 | given ×0.1, decay ×10 |
| game effect "elight" (HLSDK TE_ELIGHT) | entity attachment | given | models only (does not light the world) |
| explosions, muzzle flashes of the view model | render-fx.md | | |

## 2. Night vision (hl64 departure, architecture.md D12)

**Not GoldSrc.** Retail Half-Life draws the HEV flashlight as a light at
the end of a 2000 u view trace ([retail D-flash]). hl64 replaces it, by
the user's decision D12 (2026-11-15), with an Opposing Force-style
night-vision goggle: the same `impulse 100` toggle, battery drain /
recharge, HUD icon and sound (HLSDK player.cpp FlashlightTurnOn / Off,
the game sets EF_DIMLIGHT on the player), but while the local player has
EF_DIMLIGHT the frame is drawn in night vision instead of lighting
anything:

- The world, its movers (opaque and TransAlpha) and the opaque /
  TransTexture studio models draw rgb = texel × a bright green constant
  + a dim green floor, ignoring vertex light and model light; alpha is
  the texel's (fence cutouts stay; studio models keep their blend
  alpha). The opaque world and opaque models darken to black with eye
  depth from 64 u to 1024 u (RDP fog); blended models and TransAlpha
  brushes get no fog. Constants: `engine/render/gpu.h` `GPU_NVG_*`.
- No dynamic light touches the world (no vertex dlight scratch copies)
  and styled meshlets are not recoloured while it is on (they catch up
  on the first lit frame).
- Additive / glow / sprite / particle / decal passes and the HUD draw as
  without it.
- Cost: the same one-cycle RDP mode as the lit world, no framebuffer
  read (cart SOM read back: cycle 1, fog blender, image read off, lit
  and night vision alike), no extra pass or geometry; ares does not time
  the RDP, so that read-back is the fill-rate evidence. ares, ares-host, cook
  c16, against the same scene without `impulse 100` (D6): dark duct
  frame avg / p90 33.5 / 34.0 ms both, world 2.4 / 2.4 ms, studio 4.7 /
  4.5 ms; hldemo1 corridor 33.4 / 33.5 both; dome talk 38.0 / 42.0 vs
  37.8 / 41.8 ms. With the cone flashlight it replaced, the world zone
  took 9.4–12.8 ms in the same windows (duct frames 41.6 / 48.1 ms).

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
- Brush movers are lit by the same rule in their local frame (GoldSrc
  lights brush entities' lightmaps too).

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
| D4 | night vision: the local player's EF_DIMLIGHT makes no light; the frame draws night vision (`ren` stats `nvg`), no world vertex dlight, no restyle; the same dlight without it lights vertices | self-contained |
| D5 | muzzle flash light lasts 0.1 s per shot and brightens nearby walls as in retail | retail D-flash |
| D6 | cost: a night-vision frame ≤ the lit frame + 1 ms in the dark duct (`hl2-duct-flashlight` against the same scene without `impulse 100`, ares) | M6 |
