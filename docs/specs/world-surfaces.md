# World surfaces: cooked vertex light, light probes, texture animation

Behaviour of the world cook (`tools/w64c`) and of the runtime texture
animation, for the parts of the old cook and renderer that the audit found
Quake-derived (texture animation sequencing, the light-point trace). Input
format: the player's BSP30 maps (public format: faces, texinfo, planes,
nodes, lighting lump of RGB luxels, miptex names). Output format:
`docs/formats/w64-format.md`. Conventions: [README.md](README.md).

## 1. Face lightmap addressing (BSP30)

For a face with texinfo vectors S = (s.xyz, s.offset), T = (t.xyz,
t.offset): each vertex p has texture coordinates `s = p·s.xyz + s.offset`,
`t = p·t.xyz + t.offset` (f32). The face's luxel grid starts at
`smin = 16·floor(min s / 16)`, `tmin = 16·floor(min t / 16)` and has
`(extent_s / 16) + 1` × `(extent_t / 16) + 1` luxels, where
`extent = 16·(ceil(max/16) − floor(min/16))`. The lighting lump offset of
the face points at its first style map; up to 4 style maps (style 255 ends
the list) follow each other, each `columns × rows` RGB triples. Mono
intensity of a luxel = max(R, G, B).

## 2. Cooked vertex colour (REND/MOVR)

Per face vertex:

- Fullbright (255) when the face's texture is water/turbulent (`*` or `!`
  prefix), has no lighting (offset < 0) or has no styles.
- Otherwise luxel column = `(trunc(s) − smin) >> 4`, row =
  `(trunc(t) − tmin) >> 4`, each clamped into the grid. For every style k
  of the face: raw[k] = mono luxel of style map k (0 if outside the lump).
  The cooked colour is `finish((Σ raw[k] · 264) >> 8)`, i.e. all styles at
  their normal value `m`, where `finish(x) = clamp(trunc(x · 2.5 + 0.5),
  0, 255)` (the light scale stored as `lightscale_x100` = 250 and
  `style0` = 264 in the package header).
- A face whose style list is not exactly (0) is **styled**: its meshlet
  also stores raw[k] per vertex and the style numbers, for runtime
  recolouring (lightstyles.md 3).
- Sky faces and textures listed as skipped are not cooked.

## 3. Light probes (REND)

For each probe point (64 u lattice per sector, w64-format.md): a segment
from the point straight down 2048 u is walked through the map's render BSP
(nodes and their faces), near side first. At the first node plane the
segment crosses where a face on that node contains the crossing point —
contains meaning the crossing point's (s, t) relative to the face's
(smin, tmin) are both within `[0, extent]` — and the face is not sky
(`sky`/`SKY` prefix) or water (`*`/`!`): the probe value is that face's
luxel at `((s − smin) >> 4, (t − tmin) >> 4)` summed over its styles with
the **map-start** style values (style 0 = 264; standard styles 1–11 their
pattern's first letter; style 63 = 0; switchable styles 32–62: 264, or 0
when the light that owns them starts off; any other 256), `>> 8`,
clamped 0…255 (a face with no lighting gives 0). If no face is found on
that crossing the walk continues on the far side; no hit at all → 0. A
map without a lighting lump gives 255 everywhere. The runtime applies
the 32 floor, dlights and the 255 clamp (dlights.md 4). Probe format v3
(lightstyles.md 4) splits this sum into a base (style 0 and styles that
never change) and one (style, amount) term for the strongest animated or
switchable style; the base + term at map-start values equals the v2 value.

## 4. Texture animation

Texture names `+<c><name>` form sequences per `<name>`: digits 0–9 the
primary sequence, letters A–J (any case) the alternate sequence. A sequence
must be complete from frame 0 (a gap is a cook error). Cooked per texture:
`anim_total` = 2 × frame count of its sequence, `anim_min` = 2 × its
frame index, `anim_max` = anim_min + 2, `next` = the following frame
(cyclic), `alternate` = frame 0 of the other sequence if it exists.

Runtime, per drawn surface: start from the face's texture; if the drawing
entity's `frame` is non-zero and the texture has an alternate, start from
the alternate. Then with `r = floor(time × 10) mod anim_total`, follow
`next` until `anim_min ≤ r < anim_max`. Net effect: 5 frames per second
**[retail W-texanim]**,
all animated textures in phase with the game clock; buttons and other
brush entities switch to the alternate sequence when the game sets their
`frame` to 1. Every page a sequence can show is part of the sector's page
list (residency).

## 5. Tests

| # | property | data |
|---|---|---|
| W1 | rewritten cook produces REND, MOVR, TEXP, styles and texture tables byte-identical to the old cook for all 10 Uplink/Hazard maps; v3 probes evaluate to the old v2 probe values at map-start style values | old-tree cook outputs (local fixtures) |
| W2 | luxel addressing: synthetic faces with negative and fractional s/t, clamping at grid edges | self-contained |
| W3 | probe walk: synthetic two-floor BSP — the upper floor is found; a sky face above water is skipped; no lightmap → 0 | self-contained |
| W4 | texture animation: a 3-frame sequence at t = 1.0, 1.15, 1.2, 1.45, 1.6 shows frames 2, 2, 0, 1, 2 (`floor(10t)` = 10, 11, 12, 14, 16 → r = 4, 5, 0, 2, 4); frame ≠ 0 switches to the alternate; the renderer binds it: a brush entity of a cooked map with frame 1 binds the alternate's page (at rest and moved), each r10 the page of its sequence frame (test/m3/fx.c TEXANIM) | self-contained; cooked map |
| W5 | missing animation frame and bad letter fail the cook with the texture name | self-contained |
