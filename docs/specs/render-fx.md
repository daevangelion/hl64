# Render modes, sprites, decals, temporary effects

**Oracle status: awaiting a retail-client capture plan.** Render modes,
sprites, decals and temporary effects are drawn by the client; the HLDS
probe cannot observe them. Every `[probe]` property here is to be measured
on the retail client (README "Oracles").

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
| Normal 0 | opaque | opaque, lit | palette colours × `rendercolor`; `renderamt` ignored; alpha-test sprites are cut out (last palette index transparent), depth write |
| TransColor 1 | flat `rendercolor`, alpha = blend | as TransTexture | drawn as Normal **without tint** (`rendercolor` ignored); visible only while blend > 0 (`renderamt` and the render fx act as a visibility flag, not an alpha) |
| TransTexture 2 | texture, unlit (fullbright), alpha = blend | texture lit, alpha = blend | texture × `rendercolor`, alpha = blend |
| Glow 3 | as TransAdd | as TransAdd | additive (2a) texture × `rendercolor` × blend, no depth test, constant screen size, faded by an occlusion test (3) |
| TransAlpha 4 | translucent pass, alpha compare (palette index 255 transparent), depth write | as Normal | as TransTexture (alpha-test sprites cut out) |
| TransAdd 5 | texture, additive × blend | additive × blend | additive texture × `rendercolor` × blend |

Sprite colour, from black-box documentation of GoldSrc (the engine draws
sprites; HLSDK's client and server code only set the fields):
`rendercolor` tints a sprite (texture × colour) in every mode except
TransColor, Glow included; `0 0 0` means untinted. In TransColor the sprite
keeps its palette colours and `renderamt` only toggles visibility; in
Normal the amount is ignored. Sources: TWHL "env_sprite" (render modes and
sprite texture formats, with screenshots of each format in each mode at
amount 255 / 63 and colour 255 0 0) and TWHL "Tutorial: Render
Properties" (sprite table: Color mode "fully opaque if > 0", colour "no
effect"). HLSDK `const.h`'s formula for TransColor, `c*a + dest*(1-a)`,
describes brushes, the only type that draws the flat colour. Map content
relies on the tint: hldemo1's red env_glows are glow02 × 238 30 46, its
fire is TransAdd at `0 0 0`, and HLSDK's turret eye glow is flare3 set to
255 0 0 in Glow mode (`turret.cpp`). Confirmed on retail by capture X-glow
**[probe]** (retail client).

Brush entities whose render mode is not Normal are never folded into the
cooked static world (w64-format.md, `FOLDED`): they stay movers and draw
in the translucent pass below, even when nothing can move or fire them.

Translucent items (every entity whose mode is not Normal, except studio models in TransAlpha, which draw as Normal) draw after the
opaque world, movers, studio models and particles, sorted back to front by
the distance of their origin (brushes: their box centre), with depth test
and without depth write (TransAlpha brushes write depth, opaque texels
only, and draw first, unsorted, with the cutout sprites: nothing blended
lies under them yet); at most 64 per frame (nearest kept, overflow
counted). Additive items follow 2a.

### 2a. Additive on the RDP

The RDP blender keeps the low 8 bits of in × a + mem × b (no saturation):
an additive item over a framebuffer already brighter than 255 − c wraps to
black or a false colour (measured on ares, whose parallel-RDP blender is
the angrylion integer model (P a₀ + M (b₀ + 1)) >> 5 truncated to u8;
hldemo2 shots: glow01 over a lit wall, the orange HUD digits turning
green over a grey wall). A blend cap does not help (the framebuffer can
be at 255 already). No additive item uses the adder:

- every additive term c (GoldSrc's texture × tint × blend) is drawn as
  c = n × a, n a unit colour (max channel 255), a its brightness, through
  the alpha blender: out = c + mem × (1 − a) ≤ 255 for every framebuffer
  value and any number of stacked items. The background dims by (1 − a)
  instead of saturating (a screen-like blend): dim sources over dark
  surfaces look additive, bright cores keep their own colour instead of
  burning to white;
- textures (CI4 + 16-entry RGBA5551 TLUT: sprites, beams, brush and studio
  pages) draw through an IA16 ramp TLUT: A = the entry's brightness, I =
  its place between two unit colours lo, hi of the palette (the best pair
  of the palette's unit colours by brightness-weighted squared error of
  the non-negative fit); combiner rgb = ENV + (PRIM − ENV) × I, a = A ×
  PRIM alpha; PRIM = hi × tint with alpha = blend, ENV = lo × tint. The
  cook fits the ramp (`tools/cook/addramp.py`; fitting on the cart cost up
  to 4 ms per sprite load on ares) and stores it in every sprite
  ([sc4-format.md](../formats/sc4-format.md)), in the TEXP pages of the
  movers some entity can draw Glow / TransAdd and in the skins of the
  studio models some entity can draw so (keys `rendermode` 3 / 5, or
  named by an env_render that sets one; w64-format.md TEXP,
  studio-format.md). A page drawn additive without a cooked ramp adds
  nothing (counted, `add: miss`);
- untextured and I4 colours (tracer streaks, HUD additive pictures, text
  and fills) are split per vertex / per draw the same way;
- the full blend applies (no cap).

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
- Texture formats (SPR `texformat`): normal / additive / alpha-test use the
  palette; **index-alpha** (2) takes the texel's alpha from its palette
  index and its colour from the palette's last colour (× tint, 2); it
  alpha-blends in Normal / TransColor / TransTexture / TransAlpha (alpha =
  index × blend, blend 1 in Normal and in a visible TransColor) and adds
  colour × index × blend in Glow / TransAdd (the explosion and TE_SMOKE
  puffs, steam1). Cooked as 16 alpha levels.
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
- Cost cap: 3 ms per frame for placement work on the cart; excess
  placements wait. Counted as work units (meshlets tested + indices
  walked, 350 a frame ≈ 3 ms measured on the cart), not wall time, so host
  and cart place the same decals in the same frames.

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
  sprite frame (starting frame, rate in 0.1 frames/s, HLSDK `TE_BEAM*`),
  scroll speed, life, shade in / out (`FBEAM_SHADEIN` / `SHADEOUT`: the
  brightness ramps along the beam); drawn additive as camera-facing strips
  of ≤ 16 segments. A shaded beam draws as brightness steps, one per
  segment, one step per 10 u of length, 2 to 8 [own: the additive ramp
  combiner has no per-vertex shade; each step is a batch]. Pool
  32; a game entity's beam (keyed, alive until killed) arriving at a full
  pool takes the temporary beam that ends first [own]. Beam decals
  (`env_beam` / `env_laser` `SF_BEAM_DECALS`, HLSDK `CBeam::BeamDamage`:
  a `{bigshot` decal every damage think, 10 a second for a laser) are
  skipped within 8 u of the beam's previous decal [own: the hldemo3
  welder's laser lays a 16 u decal every 2.5 u; half a decal apart keeps
  the trail and a third of the decals]; the game random draw for the
  decal is made either way.
- **TE_FIZZ** (`env_bubbles`, HLSDK `CBubbling`). HLSDK fixes the game
  side: the message carries the entity, `sprites/bubble.spr` and the
  `density` key as a byte; the first think 2 s after spawn (0.1 s after a
  use turns it on), then every `2.5 − 0.1 × frequency` s, 0.5 s when
  `frequency` > 19; `current` is stored as |speed| in `rendercolor.x`
  (high byte) / `.y` (low byte) and its sign in `.z`. The client side
  (`R_FizzEffect`, engine code, not in the HLSDK) is unknown; hl64 uses
  **[EST]** values until a retail-client capture: `density + 1` bubbles
  per message (at most 16), at random points of the brush's box, each
  rising 30–50 u/s at scale 0.3–0.6 and drifting at `current` u/s along
  the entity's yaw, removed at the water surface above it (liquid at its
  spawn point) or at the first solid above (≤ 1024 u); a frame of the
  sprite at random, alpha-blended (TransAlpha, 255).
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
