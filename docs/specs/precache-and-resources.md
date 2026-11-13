# Precache tables and resource ownership

Conventions: [README.md](README.md). Caps: `include/hl64/limits.h`
(models 455, sounds 347, backend sound names 359, lightstyles 96 — the
old tree's measured host peaks × 1.25: models 364, sounds 277, sfx 287,
styles 82).

## 1. Tables

Three global tables map names to 16-bit indices: **models**, **sounds**,
**lightstyles** (lightstyles.md). Index 0 means "none" in every table.
Names are normalised before lookup: lowercase, `\` → `/`, no leading `/`.

Model entries are of three kinds: a map's world (`maps/<name>.bsp`, one per
open layer), a brush submodel of a map (written `*<map>:<n>`: submodel n
of that map — submodel numbers are per map, `*3` of hldemo1 is not `*3` of
hldemo2; the game may use the bare `*n` form, which resolves in the
caller's source map), and file models (`models/*.mdl` studio,
`sprites/*.spr` sprite).

## 2. Precache

- `precache_model(name)` / `precache_sound(name)` return the existing index
  when the name is present, else take the lowest free index; a full table
  is a fatal error naming the table and listing owners' counts.
- Precache is allowed whenever a spawn function runs (New Game, record
  hydration, runtime creation by the game) — unlike GoldSrc, where it is
  only legal while a level loads, because in one world maps keep opening.
- `set_model(e, name)` and sound emits with an unprecached name are a
  fatal error naming the entity class and the name (the oracle fails the
  same way: `SV_ModelIndex: model … not precached`; the route gates treat
  any such line as failure).
- Precache does not load anything; it registers a name. Studio and sprite
  data stream into their pools on first use or prefetch (architecture.md
  9, render-fx.md); sound data streams per play (sound.md).

## 3. Ownership and reuse

Each entry carries an owner set: one bit per open map slot (4), plus
**common** (New Game global spawn: the player, HUD, view models of the
starting inventory, sounds the engine itself plays) and **player** (assets
of weapons the player carries: view model, world model, fire/reload
sounds, and for the MP5 and the hand grenade their grenade and explosion
assets — HLSDK `W_Precache` keeps these loaded for every level).

- A precache made while a map's record spawns adds that map's bit; while
  the game handles a pickup it adds **player** for that weapon's asset
  list.
- When a map closes, its bit clears. An entry with an empty owner set and
  no live reference (an entity's modelindex, a playing or looping sound,
  the view model, a pending request) is **free** and its index may be
  reused by the next precache.
- Consequence: indices are stable only while referenced. Dehydrated records
  and saved games store model and sound **names**, never indices; the
  rehydrating spawn precaches again.
- The world model of a closed layer and its submodels follow the same rule
  (owner = that map).

## 4. Reporting

`stats` lines carry `precache=models/sounds` (live) and
`precachepeak=models/sounds` (run high-water); the M5 gate requires the
peaks ≤ caps on both routes.

## 5. Tests

| # | property | data |
|---|---|---|
| R1 | precache returns the same index for equivalent names (case, slashes) and distinct indices for distinct names; lowest free index is reused | self-contained |
| R2 | `*3` from a hldemo1 record and from a hldemo2 record are different models with the right bounds | two cooked layers |
| R3 | closing a map frees entries owned only by it once nothing references them; an entry still referenced by a live entity of another map survives; a looping sound keeps its entry | synthetic |
| R4 | carried weapons: after picking up the MP5 in hldemo1 and closing hldemo1 and hldemo2, switching to every weapon works (the oracle once died here with `v_crowbar.mdl not precached`) | route segment hldemo3 → hldemo2-return |
| R5 | a dehydrated monster rehydrated after its model's index was reused draws with the right model | synthetic |
| R6 | Uplink and Hazard routes: `precachepeak` ≤ 455/347 and within ±10 % of the oracle's peaks (364/277 Uplink, 219/99 Hazard) | route |
| R7 | unprecached `set_model` is fatal with class and name in the message | synthetic |
