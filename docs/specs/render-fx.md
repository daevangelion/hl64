# Render modes, sprites, decals, temporary effects

The old tree's code for these areas follows Xash3D sources (audit: r64_trans.c,
r64_decal.c, gsfx_cl.c, gsfx_cart.c), so hl64 rewrites them from this spec,
HLSDK's `const.h`/`effects.cpp`/`util.cpp` (game side, allowed), the SPR
format note in `docs/formats/` and black-box captures of retail Half-Life
(README "Oracles"). Conventions: [README.md](README.md).

Content census (cook of Uplink + Hazard entity lumps): render modes used
TransAlpha 4 (func_illusionary 173, func_wall 147, doors 36, breakables,
trains), Glow 3 (env_glow 116, env_sprite 2), TransAdd 5 (env_sprite 41,
monster_generic holograms 33, func_rotating 2), TransTexture 2 (breakables
21, lights 8, env_render 6), TransColor 1 (breakables 16, doors 5, …);
render fx used: NoDissipation 14 (env_glow 36, env_sprite 2), Hologram 16
(monster_generic 33, env_render 6), FadeFast 6 (env_render 8), Distort 15
(env_render 2). Those must be exact; the remaining fx (pulse, strobe,
flicker, glow shell…) are implemented for game effects and checked
against retail captures at lower priority.

## 1. Blend value

Every entity has a **blend** 0…255 computed each rendered frame from
`renderamt` and `renderfx`:

| fx | blend |
|---|---|
| None 0, NoDissipation 14 | renderamt |
| PulseSlow 1 / PulseFast 2 / PulseSlowWide 3 / PulseFastWide 4 | renderamt plus a sine of game time, slow ≈ 1 Hz / fast ≈ 4 Hz, amplitude ±16 (narrow) or ±64 (wide), phase offset per entity index; clamped 0…255 |
| FadeSlow 5 / FadeFast 6 | the entity's stored amount moves toward 0 at 30 / 120 per second and stays there (persistent: it changes `renderamt`) |
| SolidSlow 7 / SolidFast 8 | the same toward 255 |
| StrobeSlow 9 / Fast 10 / Faster 11 | renderamt or 0 alternating at ≈ 4 / 8 / 16 changes per second |
| FlickerSlow 12 / Fast 13 | renderamt or 0 driven by two sines of game time (irregular), ≈ 2 / 5 Hz |
| Distort 15 | renderamt reduced by a random amount each frame (distribution calibrated by fixture X3) |
| Hologram 16 | Distort, additionally reduced with distance from the viewer: full up to about 100 u, decreasing toward 0 by about 400 u (fixture X3) |
| DeadPlayer 17, Explode 18 | not used (player corpse, studio explode); blend = renderamt |
| GlowShell 19 | studio drawn a second time slightly inflated, additive in rendercolor (game effects only) |

Rates for fades are per second (GoldSrc applies them per rendered frame;
the per-second rates equal GoldSrc at 30 fps). Exact shapes of the sine and
random fx are calibrated from retail capture X-fx (the same entity
recorded over time).

An entity whose render mode is not Normal and whose blend is 0 is not
drawn (it stays solid).

## 2. Render modes

| mode | brush | studio | sprite |
|---|---|---|---|
| Normal 0 | opaque | opaque, lit | alpha-tested cutout, depth write |
| TransColor 1 | flat `rendercolor`, alpha = blend | as TransTexture | colour = rendercolor × texture, alpha = blend |
| TransTexture 2 | texture, unlit (fullbright), alpha = blend | texture lit, alpha = blend | alpha = blend |
| Glow 3 | as TransAdd | as TransAdd | additive, no depth test, constant screen size, faded by an occlusion test (3) |
| TransAlpha 4 | opaque pass, alpha compare (palette index 255 transparent) | as Normal | cutout with alpha = blend |
| TransAdd 5 | texture, additive × blend | additive × blend | additive × blend |

Translucent items (everything except Normal and TransAlpha) draw after the
opaque world, movers, studio models and particles, sorted back to front by
the distance of their origin (brushes: their box centre), with depth test
and without depth write; at most 64 per frame (nearest kept, overflow
counted). The RDP additive blender wraps instead of saturating: additive
items cap their blend at 128 and the colour sum is kept below wrap by
the CI4 TLUT range (old tree finding).

## 3. Sprites (SPR v2, cooked to CI4 frames)

- Orientation types: parallel-upright (faces the view plane, stays
  vertical), facing-upright (faces the viewer's position, stays vertical),
  parallel (faces the view plane), oriented (uses the entity's angles),
  parallel-oriented (faces the view plane, rolled by the entity's roll).
- Size = frame size × `scale` (0 means 1); the frame's origin offset
  places it relative to the entity origin.
- Frames: the entity's `frame` selects; group frames pick by time within
  the group's cumulative intervals. env_sprite animation itself is game
  logic (HLSDK `CSprite::Animate`).
- Glow occlusion: a line trace (NOMONSTERS) from the eye to the sprite
  origin; blocked → blend × 0; otherwise blend × min(1, distance-based
  factor) so glows stay visible far away at constant screen size (fixture
  X4).
- Cache: CI4 frames load on first draw into an LRU cache (8 sprite models
  / 64 KB on the old cart; frames unused for 90 frames are freed); a sprite
  whose data is not resident is skipped that frame.

## 4. Decals

- Pool of 96 decals; the oldest is replaced when full. A decal is
  (texture, surface, position, owner model).
- Placement (`decal_trace` / `decal_at(model, point, texture)`): every
  cooked face of the hit model (world sectors resident around the point, or
  the mover) whose plane passes within 4 u of the point receives the decal:
  a rectangle of the decal texture's world size (texels = units), centred
  on the point projected onto the face plane, aligned with the face's
  texture axes, clipped to the face polygon (≤ 8 clip vertices kept, ≤ 24
  vertices per decal). Faces facing away from the point are skipped.
- Decals on movers move with the mover. A decal whose surface's sector is
  evicted is kept as data and redrawn when the sector returns.
- A placement whose sector is not resident is queued (16 pending, 4 tried
  per frame, dropped after 2 s).
- Drawing: after the opaque world, depth test with decal Z offset, texel =
  I4 alpha × the decal's tint (decal sheet cooked from the player's
  `decals.wad`).
- Cost cap: 3 ms per frame for placement work; excess placements wait.

## 5. Temporary effects (game → engine effect API)

All in fixed pools, randomness from the render random stream:

- **Temp entities**: 48 (sprites, studio gibs/shells/debris). Studio
  temps draw under their own cap of 6 (nearest first). Gravity, bounce on
  world (shells and gibs: bounce sound sets per material), die time, fade
  over the last second (alpha) for gibs.
- **Particles**: 512 points (blood stream, sparks fallback, tracers),
  gravity × `sv_gravity` per type, colour ramp, die time.
- **Beams**: entity↔entity, entity↔point, point↔point; width, noise
  amplitude (random perpendicular offsets per segment), colour, brightness,
  scroll speed, life; drawn additive as camera-facing strips of ≤ 16
  segments.
- **Effects catalogue** (one function each, HLSDK `TE_*` semantics):
  sparks, ricochet (sprite + sound), bullet impact (decal + sparks +
  material sound), blood sprite (bloodspray animated 0.25 s + up to 5
  falling drops), blood stream, explosion (animated sprite, light, sound,
  scorch decal), smoke, break model (debris shower by material), gib
  shower, muzzle flash (view model attachment sprite + light), tracer,
  dlight / elight (dlights.md), screen shake, screen fade, text message.
- Blood colour comes from the victim class (game supplies it).

## 6. Tests

| # | property | data |
|---|---|---|
| X1 | render mode table: per mode and model kind, the RDP mode chosen (blend formula, depth write, alpha compare) | host recorder |
| X2 | translucent ordering back to front, cap 64 nearest, invisible at blend 0 but still solid (trace hits it) | synthetic |
| X3 | fx blend over 5 s for renderamt 255 and 128, each fx 0–19, at distances 50/200/500 u: fade/solid exact rates; pulse/strobe/flicker period and amplitude within 10 %; distort/hologram mean and spread within 10 % | retail X-fx |
| X4 | glow sprites: occluded → invisible; screen size constant across 100–2000 u ± 5 % | synthetic + fixture screenshots (env_glow in hldemo2) |
| X5 | decals: a shot at a wall corner splits across both faces; decal clipped to each face polygon; decal on a door moves with it; pool of 96 replaces oldest | synthetic |
| X6 | effect catalogue: each effect allocates within its pool caps and frees on time; a burst beyond a cap degrades by dropping, never crashes | synthetic |
| X7 | hldemo2 firefight: screenshots at 3 steps SSIM ≥ 0.9 vs retail at the same poses; translucent + effects CPU ≤ the old cart's `rview` share for the same scene | retail screenshots + PROFILE |
