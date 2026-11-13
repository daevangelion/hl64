# Lightstyles

Conventions: [README.md](README.md). Cooked data: `REND`/`MOVR` style
blocks and REND light probes (`docs/formats/w64-format.md`).

## 1. Patterns and values

- A style is a pattern string of letters `a`…`z` set by the game
  (`eng_lightstyle(style, pattern)`). Letter c has value
  `(c − 'a') × 22`: `a` = 0 (dark), `m` = 264 (normal), `z` = 550 (double).
  A style that was never set, or set to an empty string, has value 256.
- Animation: at game time t every style's value is its pattern's letter at
  index `floor(t × 10) mod length` (10 letters per second, all styles in
  phase, one global clock). A single-letter pattern is constant.
- The standard styles 0–12 (and 63) are set by the game's worldspawn at
  New Game from the HLSDK table (`world.cpp`, game/); style 0 is `m`.
- Light entities with a targetname use switchable styles 32–63 of their
  map: the game sets them to `a` or `m` (spawnflag START_OFF) at spawn, or
  to their `pattern` key, and toggles them on use.

## 2. Global slots and per-map remap

The table has 96 global slots: 0–31 common (same meaning for every map),
and two banks of 32 for the switchable styles of the (at most two) open
geometry layers. Each open layer has a `stylemap[32..63] → global slot`,
filled when the layer opens from the styles its cooked chunks and records
use. A game call naming a style ≥ 32 is resolved through the stylemap of
the caller's source map. When a layer reopens, the game's cached on/off
state of its switchable lights is reapplied (old tree `HLE_LayerOpened`).
`stylepeak=N(common32)` is reported in `stats`.

## 3. Effect on the world (vertex-lit)

- Meshlets without styles carry their final colour (style 0 at `m` = 264
  baked by the cook, light scale 2.5, the package header's `style0` and
  `lightscale_x100`); they never change at run time.
- A styled meshlet has up to 4 styles and, per vertex, one raw luxel per
  style. Its vertex intensity is `L = Σ luxel[k] · value(style[k]) >> 8`,
  then finished as `min(255, floor(L × lightscale + 0.5))` into R, G and B
  (mono). It is recoloured lazily: only when drawn and only if one of its
  styles' values (or the light scale) changed since its last colouring.
- Brush movers (MOVR) follow the same rule with their own style blocks.

## 4. Effect on models

Studio and sprite lighting comes from the sector light probes (64 u
lattice, trilinear), which hold the **static** light at map-start style
values (switchable styles as their spawn state). Style animation does not
change model lighting (oracle behaviour; GoldSrc samples the lightmap under
the model each frame, so a flickering light flickers models too — accepted
difference, D6). Dynamic lights add on top (dlights.md); the result is
floored at 32 and clamped at 255.

## 5. Tests

| # | property | data |
|---|---|---|
| L1 | value table: every letter, empty string = 256, unset = 256 | self-contained |
| L2 | animation index: pattern "abc" at t = 1.0, 1.05, 1.15, 1.25, 1.35 gives b, b, c, a, b (index = floor(10t) mod 3) | self-contained |
| L3 | a styled meshlet's vertex colours equal the formula for given luxels/values; unchanged values cause no recolour (relit count 0 on a steady frame) | synthetic + `stats relit=` |
| L4 | two layers with switchable styles map to disjoint global banks; toggling style 33 in hldemo2 does not change hldemo1's style 33 | two cooked layers |
| L5 | hldemo3 (493 styled faces) and t0a0: screenshots at the oracle's `shot` steps match (SSIM ≥ 0.95, `r_drawentities 0`) at 3 animation phases | fixture screenshots |
| L6 | switchable lights along the Uplink route change on the same frames as the oracle (±1 frame) | fixture K-think (style changes are logged with map, style, pattern, time) |
