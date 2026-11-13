# Lightstyles

**Oracle status: awaiting a retail-client capture plan.** The HLDS probe
cannot observe lighting: `GetEntityIllum` returns 128 everywhere on a
dedicated server (capture L-illum, Oracle2, hl64 `d2bf3d8`, 10 maps), and
style values are only applied by the client. Every `[probe]` property here
is to be measured on the retail client (README "Oracles").

Behaviour: animated and switchable lights look and time as in Half-Life.
Mechanism: patterns cooked to value arrays with ids; the world recolours
only styled meshlets, only when a value changes. Conventions:
[README.md](README.md). Cooked data: `REND`/`MOVR` style blocks, REND light
probes (`docs/formats/w64-format.md`).

## 1. Patterns and values (behaviour)

- A pattern is a sequence of letters `a`…`z`; letter c has value
  `(c − 'a') × 22` (`a` = 0 dark, `m` = 264 normal, `z` = 550 double).
  An unset style or empty pattern has value 256.
- All styles advance together at 10 letters per second on the game clock:
  index = floor(time × 10) mod length **[retail L-anim]**.
- Styles 0–12 and 63 hold HLSDK's standard patterns (`world.cpp`), style 0
  `m`. Named lights use their map's switchable styles 32–63: `a` or `m` per
  spawnflag START_OFF at spawn, or their `pattern` key; toggled on use.

## 2. Mechanism

- `tools/restab` cooks every pattern string (HLSDK standard table from
  `game/defs/`, every map's `pattern` keys) into a pattern table: id u16 →
  (length u8, values u8[length] as letter index 0…25). The game calls
  `eng_lightstyle(style, pattern_id)`; no strings at run time.
- Every 100 ms tick (not every frame) the engine recomputes the 96 style
  values (32 common + 2 × 32 for the open layers) and marks the styles
  whose value changed; between ticks nothing is done.
- Global slots: 0–31 common; each open geometry layer has a
  `stylemap[32..63] → global slot` filled when it opens; a game call naming
  a style ≥ 32 resolves through the caller's source map. Reopened layers get
  their switchable lights' cached state back.

## 3. World

- Unstyled meshlets carry their final cooked colour and never change.
- A styled meshlet (≤ 4 styles) stores per vertex one raw luxel per style;
  intensity `L = Σ luxel[k] · value(style[k]) >> 8`, finished as
  `min(255, floor(L × 2.5 + 0.5))` (header `lightscale_x100`). It is
  recoloured when drawn and one of its styles changed since its last
  colouring (at most 10 times a second).
- MOVR chunks follow the same rule.

## 4. Models

GoldSrc lights a model from the lightmap below it at the current style
values, so models flicker with animated lights and darken when a switchable
light goes off. hl64 (D6 iii): probe format v3 stores per probe a base
value (style 0 and every style at its map-start value excluded from the
term below) plus one (style, amount) term for the strongest non-zero
animated/switchable style at that point; model intensity = base +
amount × value(style) / 264, plus dlights, floored at 32, clamped at 255.
Probes on faces lit by more than one non-zero animated style keep the
strongest; the rest is baked at map-start values [EST: covers the Uplink
and Hazard rooms; the cook reports probes that lose a term].

`pfnGetEntityIllum` (engine `ent_illum`, used by e.g. monster_cockroach's
light test) returns this model light at the entity: the light a studio
entity is drawn with, for other entities the same formula at their origin.
hl64 follows single-player Half-Life (a listen server: server and client
in one process share the lighting). A dedicated HLDS has no client
lighting and answers 128 everywhere (capture L-illum, Oracle2, hl64
`d2bf3d8`), so that capture is not hl64's oracle for this call.

## 5. Tests

| # | property | data |
|---|---|---|
| L1 | value table: every letter, empty / unset 256 | self-contained |
| L2 | pattern "abc" at t = 1.0, 1.05, 1.15, 1.25, 1.35 → b, b, c, a, b | self-contained |
| L3 | styled vertex colours follow the formula; no recolour work on frames between ticks or when values are unchanged (`relit` 0) | synthetic + stats |
| L4 | two layers' switchable styles map to disjoint global slots; toggling hldemo2's style 33 leaves hldemo1's unchanged | two cooked layers |
| L5 | flicker rate and phase of an Uplink flickering light match retail over 3 s | retail L-anim |
| L6 | a model under a light switched off darkens to the probe base on the same frame the world does | synthetic + retail screenshot |
| L7 | cook: probe v3 terms and the loss report on the 10 maps | cook report |
