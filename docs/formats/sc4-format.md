# Cooked sprites — `.sc4`, version 3

`tools/cook/hl_sprite_cook.py` cooks every staged GoldSrc SPR v2 sprite
(`sprites/<name>.spr`) into `sprites/<name>.sc4`, which the effects renderer
(`engine/render/sprite.c`, [render-fx.md](../specs/render-fx.md) 3) reads in
place: one PI DMA into one sprite-cache block, no conversion on the cart.
`tools/restab` makes it the SPRITE resource's file (rtb-format.md).

Version history: 2 held a brightness-only IA16 TLUT and a mean tint; 3
replaces both with the cooked additive ramp (render-fx.md 2a) and the
index-alpha levels.

## Layout

Big-endian; offsets absolute; every data table 8-aligned; the file size is
a multiple of 8.

| off | field | meaning |
|---:|---|---|
| 0 | `magic` u32 | `0x53433431` (`SC41`) |
| 4 | `version` u16 | 3 |
| 6 | `nframes` u16 | entity frames (a frame group counts once; GoldSrc ModelFrames) |
| 8 | `type` u8 | SPR orientation type 0–4 (render-fx.md 3) |
| 9 | `texformat` u8 | SPR texture format: 0 normal, 1 additive, 2 index-alpha, 3 alpha-test |
| 10 | `nimages` u16 | images: single frames and every member of every group |
| 12 | pad u32 | 0 |
| 16 | `tlut` u16[16] | RGBA5551 TLUT (the CI4 palette) |
| 48 | `ramp` (40 B) | the additive ramp (below) |
| 88 | `images` 16 B × `nimages` | `left` s16, `up` s16 (frame origin offset), `w`, `h` u16 (source size, world units at scale 1), `tw`, `th` u8 (CI4 sides), pad u16, `ci4` u32 (file offset, 8-aligned) |
| … | `frames` 4 B × `nframes` | `first` u16 (image), `count` u16 (1, or the group's member count) |
| … | `ends` u16 × `nimages` | a group member's cumulative interval end in ms (the SPR group's interval table; 0 for single frames), increasing within a group |
| … | CI4 payloads | `tw × th / 2` bytes each, high nibble first, 8-aligned |

## Texels and TLUT

- Every image is resampled nearest to power-of-two sides ≤ 64 (TMEM
  wrapping).
- Normal, additive, alpha-test: 16 palette colours picked from the
  palette-index usage summed over every frame at full size (the most used
  colour, then the most used of each of 15 luminance bands, then
  popularity); every index maps to the nearest picked colour
  (green-weighted). Alpha-test: index 255 owns entry 0, alpha bit clear;
  no other index maps to it.
- Index-alpha (GoldSrc: the texel's alpha is its palette index, its colour
  the palette's last colour): index *i* → entry round(*i* × 15 / 255), the
  16 alpha levels 17 *k*; `tlut[k]` = the colour × 17 *k* / 255 with the
  alpha bit set for *k* > 0 (entry 0 transparent).

## Ramp (40 B, `engine/render/addblend.h` `add_ramp_t`)

| off | field | meaning |
|---:|---|---|
| 0 | `ia` u16[16] | IA16 TLUT, I in the high byte |
| 32 | `lo` u8[3] | ramp end bound as ENV |
| 35 | `hi` u8[3] | ramp end bound as PRIM |
| 38 | pad u8[2] | 0 |

- Palette formats: the additive ramp of `tlut` (`tools/cook/addramp.py`,
  render-fx.md 2a): `lo`, `hi` unit colours (max channel 255) of the
  palette; entry *k* has A = its brightness (x + y of the best non-negative
  fit x lo + y hi ≈ colour, clamped to 1), I = y / (x + y); transparent
  or black entries A = 0. The cart binds `ia` with PRIM = hi × tint
  (alpha = blend), ENV = lo × tint for Glow / TransAdd draws and beams.
- Index-alpha: `ia[k]` = I 255, A 17 *k*; `lo` = `hi` = the colour (not a
  unit colour). Every draw of the sprite binds `ia`: alpha-blended modes
  with PRIM = ENV = colour × tint, alpha = blend; additive modes with the
  colour × tint split into unit colour × brightness (render-fx.md 2a).

## Validation (reader)

`engine/render/sprite.c` rejects a file unless: magic and version as
above; `nframes` ≥ 1, `nimages` ≥ `nframes`, the image, frame and end
tables inside the file; `type` ≤ 4; every image's sides powers of two in
2…64 and its CI4 bytes inside the file, offset 8-aligned; every frame's
images inside `nimages`; a group's ends non-zero and increasing.
