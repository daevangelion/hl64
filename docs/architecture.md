# hl64 engine architecture

Status: design, revision 2 (EngineSpec, 2026-11-13). Binding rules:
[PROVENANCE.md](PROVENANCE.md). Behaviour specs: [specs/](specs/README.md).
Reuse verdicts: [provenance-audit.md](provenance-audit.md) (ProvAudit).

hl64 is a single-player Nintendo 64 engine for Half-Life content. Two rules
shape every module:

1. **Behaviour target = GoldSrc as observed.** What the player sees, hears
   and plays must match Half-Life (HLSDK semantics, retail look and feel).
   The old quakegeneric tree is an oracle only where it matches GoldSrc;
   its Quake-derived quirks are not preserved (specs/README.md, "Oracles").
2. **Internals owe nothing to Quake's shape.** No string cvars, no command
   buffer, no float-typed entvars blob, no edict array scanned every frame,
   no client/server split, no precache-by-name string tables, no lightstyle
   strings at run time. Each module uses the cheapest N64 design that keeps
   rule 1: typed fixed-size records, integer time and ids, cooked indices
   instead of runtime strings, data-oriented loops over active sets, spatial
   structures sized to our sectors and cells, DMA-friendly layouts.

Where GoldSrc's data *is* its behaviour, we keep the data: collision uses
the compiler's clip hulls (cooked into our COLL format) because the hull
expansion, bevels and the 1/32 u stand-off are what players feel.

Game logic is our C reimplementation of the HLSDK game (`game/`, HLSDK
licence), retargeted from the Quake hooks it uses today onto the engine API
below. It runs one continuous streamed world over the existing cooked
formats (`.w64`, CWG3, HLE1, STB2 v4, wav64, ui8) plus new cooked tables
(resources, sentences, lightstyle patterns, compiled routes), within the
stream-arch-v2 budgets (old tree `n64/docs/stream-arch-v2.md`, "SA2").

Evidence labels: **measured** (old-tree log or cook report, cited),
**[EST]** (estimate with its reasoning; the milestone named replaces it with
a measurement).

Old-tree CPU baseline used throughout (**measured**): cart Uplink route,
`PROFILE=1`, ares, `/scratch/quakegeneric/tmp/Anim/final-prof/ares-uplink-playthrough.log`,
µs per frame over 170 one-second windows, p50 / p90 (zones nest: `trace`
is inside `phys`, `phys` inside `sv`):

| zone | p50 | p90 | | zone | p50 | p90 |
|---|---:|---:|---|---|---:|---:|
| `sv` server frame | 14538 | 18806 | | `smsg` send to client | 1851 | 2941 |
| `phys` entity physics | 8681 | 11955 | | `went` entity delta write (in smsg) | 1337 | 1531 |
| `pcl` player | 1122 | 1527 | | `cl` client read + relink | 1772 | 2666 |
| `step` monsters | 2339 | 6253 | | `wsf` stream frame | 3286 | 4094 |
| `toss` / `push` / `none` | 1111 / 959 / 720 | 1433 / 1360 / 1081 | | `wake` record wakes | 750 | 1432 |
| `think` | 1364 | 3807 | | `rview` render CPU | 7102 | 15536 |
| `trace` | 2131 | 4773 | | `r2d` 2D/HUD | 604 | 1517 |
| `link` / `leafs` | 974 / 529 | 1749 / 716 | | | | |

---

## 1. Scope

In scope: one process, one thread plus interrupt callbacks; renderer and
sound read entity components directly after the simulation step. One
continuous world per campaign component (every map at its CWG3 offset
`g_M`; seams are portals; nothing loads a level after New Game). N64 with
Expansion Pak; a host build (Linux, `-m32` and LP64) enforcing the same
limits. Content: Half-Life: Uplink and the Hazard Course as cooked from the
player's data; nothing may assume a map count or names (the full campaign is
the long-term aim).

Not in scope: multiplayer, network protocol, demos, mods/DLLs, raw BSP on
the console, software rendering, Android/SDL, 4 MB consoles.

---

## 2. Top-level structure

```
hl64/
  LICENSE                 MIT (everything except game/ and tools/hlsdk/)
  Makefile                rom (libdragon n64.mk + t3d.mk), host, cook, tests
  include/hl64/           MIT public headers (engine API seen by game/)
    limits.h              every cap and pool size (cart = host)
    types.h               vec3, ent_h, name_t, res_t, time_us, fixed types
    ent.h                 entity component layout (6)
    eng.h  game.h         engine services / game exports (7)
    pmove.h               player-move host contract (specs/pmove-host.md)
    settings.h            typed settings record (12)
  engine/                 MIT
    core/   boot, frame loop, clock, rng, log, settings, debug commands
    mem/    boot carve, arenas, first-fit pools, slabs
    io/     cart file table, async DMA scheduler
    world/  layers (.w64), campaign (CWG3), portals, vis, residency
    coll/   hull walks, traces, hitboxes, spatial grid
    phys/   active sets, movetypes, pushers, step moves, pmove host
    ent/    components, handles, think heap, records (HLE1), hydration,
            dehydrate store, timers, name index
    res/    cooked resource table, residency interest
    snd/    voices, spatialisation, sentences, wav64
    render/ frame, sectors, movers, studio, particles, translucent,
            sprites, beams, decals, lights, 2D
    ui/     menu, debug overlay
    save/   save/load (D4)
    test/   route VM, profiler zones, stats records
  plat/n64/  plat/host/   the only code that includes libdragon / tiny3d
  game/                   HLSDK licence (game/LICENSE-HLSDK + Valve notice)
    hl/                   the C reimplementation (retargeted)
    pm/                   HLSDK pm_shared (D2)
    studio/               pose maths from HLSDK StudioModelRenderer (D3)
    hud/                  HUD, view
    defs/                 classes.def, funcs.def, res.def (read by the cook)
  tools/                  MIT cook tools; tools/hlsdk/ HLSDK-licensed parts
  test/  routes/ (text sources) fixtures/ oracle/ (GoldSrc probe, HLSDK, 15) run/
  docs/  architecture.md PROVENANCE.md provenance-audit.md specs/ formats/
```

`game/` includes only `include/hl64/*.h` and its own headers; `engine/`
never includes `game/` headers except the generated tables from
`game/defs/*.def` (plain X-macro data: class ids, function ids, resource
ids), which the cook reads too.

---

## 3. Platform and frame loop

### 3.1 Boot (N64)

1. libdragon: ISViewer debug, DFS, timer, controllers, audio 22050 Hz,
   rspq, tiny3d; display 320×240×16, two framebuffers, 16-bit Z; banked
   libdragon (`~/sdks/libdragon-banked-bc24d3d5a7d2`, FB/Z in separate
   RDRAM banks).
2. `mem_boot_carve`: every fixed region of 4.1 in a fixed order; the
   remaining heap is measured and checked against `HL64_HEAP_MIN_KB`.
3. tiny3d strip-index self-check against the `.w64` header encoding
   (mismatch → in-place re-encode, SA2 §3.2).
4. Content check: `campaign.cwg`, `campaign.rtb` (resource table) and every
   `.w64` it names present with matching sizes and directory CRCs; missing
   player content → a text screen explaining how to cook it.
5. Menu, or a compiled route (test builds).

### 3.2 Frame loop

| step | module | work |
|---|---|---|
| 1 | plat | VI counter → `dt` (f64 s) = elapsed vblanks × VI period, clamped to [1/60, 0.1] s (D6 iii); `host_fixed_dt` in test builds |
| 2 | in | pad → `usercmd` |
| 3 | io | pump (also between steps 6/7, 9/10 and after the world draw) |
| 4 | game | `game_start_frame`; logic timers due by `now + dt` (think-and-time.md 4) |
| 5 | phys | player: prethink → think if due → pmove → link + touches → postthink |
| 6 | phys | **active set** in ascending entity index: each entity's GoldSrc movetype step (think + motion) — only entities that are due to think, moving, falling, or pushers in motion (6.3) |
| 7 | world | campaign: approach / arm / switch, portal crossing, layer open/close |
| 8 | ent | hydration pump / release under the entity budget (2 ms) |
| 9 | world | residency targets (recomputed only when the eye leaf, the player cell or a pin changes) → DMA queue |
| 10 | snd | listener, voices, sentence words, loops |
| 11 | render | view (`game_calc_view`), world, movers, studio, particles, translucent, decals, view model, HUD, debug overlay |
| 12 | plat | submit; present gated on VI (3.3) |
| 13 | test | route VM step, PROFILE zone roll-up, stats record |

`now` advances by `dt` after step 6.

### 3.3 Present pacing (30 fps VI cap)

A finished frame is shown no earlier than `HL64_PRESENT_VBL` (2) vblanks
after the previous show, otherwise on the first vblank after the RDP
finishes it: frame times sit on 33/50/67 ms tiers (NTSC). The RDP-done
callback shows the frame if the gate is open, else parks it; the vblank
handler shows a parked frame when the gate opens. With two framebuffers the
next frame's simulation overlaps a finished frame's wait. libdragon's
`display_set_fps_limit` is not used (any frame over 33 ms would become
67 ms). A pacing histogram is part of every stats record.

### 3.4 Host build

`make host` → `hl64-host`: same `engine/` and `game/`, `plat/host`: `pread`
with modelled PI cost (195 µs/KB = 5.25 MB/s, counted against the frame
budget), scripted pad, null audio that runs voice allocation and
spatialisation, draw-list recorder instead of the GPU (D5). Same carve, same
caps, one arena the size of the cart's post-carve heap: anything that fails
on the cart fails on the host. Components hold handles and ids, not
pointers, so `-m32` and LP64 sizes are identical.

---

## 4. Budgets

### 4.1 Memory (8 MB)

Measured columns: M3 gate, ares, route `m3-a` (hldemo1 only, one geometry
layer, 2400 frames; `/scratch/hl64/tmp/Gate/rest/table.md`). Carved heap
at boot: **3696 KB** in 166 allocations, 0 frees, 0 failures (peak = carve).
Peaks are one-map values; M4 re-measures every region with two layers.

| region | hl64 KB (carve) | M3 peak (use) | old tree | basis |
|---|---:|---:|---:|---|
| code + data + bss (cart image) | ≤ 900 **OVER** | 1475 KB cart image **measured** (M3Lead2) | 1428 **measured** (HL cart ELF) | flagged OVER until M3Lead2 reports the top contributors. `HL64_HOST_HEAP_KB` is now derived at build time from the cart image size, so the host heap tracks the cart's |
| framebuffers ×2 + Z | 450 | — | 450 | measured |
| rspq / tiny3d / matrices | 96 | not reported | (in heap) | [EST] |
| render / texture / collision pools | 240 / 128 / 208 | 117 KB (49%) / 99 KB (77%) / 55 KB (26%) | 240 / 128 / 208 | cook-proved worst sets (SA2 §8); **measured** one layer |
| layer directories (2 open) | 168 | not reported (hldemo1 dir 40.8 KB) | 2 × 38–84 | measured |
| studio pool | 1024 (D3 b) | 225 KB (22%) | 1536 | **measured** |
| studio skinned-mesh tables, view model scratch | 192 | not reported | 256 heap reserve | [EST] |
| entity components | 134 | 137168 of 137216 B (100%) | edicts 495 × 884 = 427 | **measured**, exact by construction: 495 × 228 B + 24308 B grid nodes, membership pool, touch rank, free times |
| game private data | 160 | 30.6 KB (19%), 98 live entities | (inside the edicts) | **measured** ≈ 320 B per live entity; at the 495 cap ≈ 155 KB, so 160 stays |
| HLE slots | 192 | 77.9 KB for **one** map (m3-a) | 72 KB text + 13–19 KB keys per map | **measured**: hldemo1 chunk 48912 + runtime 14278 B = one 63190 B resident slot, 67702 B while opening, plus ≈ 16.3 KB shared tables; 160 KB held 2 slots, not 4; cook-checked need below |
| dehydrate store | 64 | 0 (no crossing in m3-a) | similar | M4 measures |
| resource table + pattern table | 56 | 43.5 KB (78%) | precache name tables, sfx names | **measured** (the earlier 12 KB counted the 12 B entries only) |
| sentence table (cooked) | 24 | not reported | parsed text | [EST] |
| lightstyle patterns + state | 4 | not reported | strings + 96 × value | lightstyles.md |
| think heap, active bitsets, grid tables | 12 | 6.3 KB (52%) | area tree + leaf lists | **measured** |
| sound (16 voice buffers, 10 wav64, loops) | 96 | 76.1 KB (79%) | not measured | **measured** (route; the earlier `snd_ares.sh` peak was 78144 B) |
| sprite cache / decals / particles | 64 / 56 / 12 | — / 51.7 KB (92%) / — | 64 / 8 / 512 | decals **measured**: peak × 1.25 = 64.6 KB exceeds the 56 KB carve, so the decal region grows to **68 KB** (limits.h change owed) |
| dlight scratch + flashlight pool | 48 | not reported | same | |
| HUD sheets + font (ui8) | 128 | 71.5 KB (56%) | similar | **measured** |
| debug log ring (cart) | 8 | — | 16 KB console text | 12 |
| per-frame scratch arena | 96 | not reported | — | |
| stack + libdragon slack | 256 | 4.5 KB stack peak | — | measured stack only |
| **total** | **≈ 5.1 MB** (carved heap 3696 KB measured) | | old cart route low point: 259 KB heap free (PROFILE) **measured** | headroom ≈ 3 MB |

Rows still marked "not reported" print a `mem` line at boot and a
high-water mark in every stats record from M4; each carve becomes
peak × 1.25 (M4 for two layers, M6 for the full route). Region use above
90% (decals) or a peak × 1.25 over the carve is a gate failure.

**HLE slots for M4** (2 geometry layers + up to 4 HLE slots, 5.3). Four
full slots would need ≈ 4 × 65 KB + 16 KB ≈ 276 KB. hl64 instead defines
what a closing map keeps:

- **Open** map (geometry layer resident): full slot, chunk + runtime.
- **Frozen** map (closed with frozen timers or logic still referenced):
  runtime only — record state, timers, logic thinks, masters and the
  name→record index a cross-map fire or timer callback can reach (the
  cook puts those in the runtime part). The chunk (immutable cooked
  records and keys) is dropped and re-read from the cart when the map
  reopens: ≈ 50 KB on the OPEN DMA priority in the approach window, before
  any of its records can hydrate (no record of a closed map can).
- **Cook check** (`hleids`, measured by M3Lead2): need = shared tables +
  the **opening** map at its open-time need (the heap must hold it while
  it opens: hldemo1 67702 B, not its resident 63190 B) + one open map's
  resident slot + 2 × frozen runtime, each 16-aligned, over the worst maps
  of the campaign. Measured need: Uplink + Hazard **176176 B** (tables
  16688 + opening hldemo1 67702 + open hldemo1 63190 + 2 × 14278); 83
  retail campaign maps **185184 B**; worst single map c2a5f **182944 B**
  (its chunk is 51472 B). All fit the **192 KB** carve (196608 B,
  `HL64_HLE_BLOBS_KB`); the cook fails any campaign over it. Evidence:
  `/scratch/hl64/tmp/M3Lead2/cook4.log`, `/scratch/hl64/tmp/Rec/retail/`.

### 4.2 CPU per frame (33.3 ms at 30 fps)

Measured (M3 gate, ares, route `m3-a`, 80 one-second windows): the frame
holds the 30 fps VI cap (`frame` 33436 µs avg). Zones are independent
roll-up timers and may nest (phys inside the ent pass, mon inside game),
so their sum, 13.8 ms, is an upper bound on simulated CPU, against the old
tree's ≈ 27 ms p50. The old [EST] rows are replaced by this table.

| zone | avg / p90 / max µs (measured) | budget µs | status |
|---|---|---:|---|
| world (render list + submit) | 2385 / 5946 / 8319 | render CPU 8000 / 12000 total | world + studio + fx + hud avg 6441: within |
| studio | 1727 / 4309 / 11445 | (render) | |
| fx (translucent, decals, temp ents) | 1179 / 1985 / 3262 | (render) | |
| hud | 1150 / 2160 / 5727 | (render) | |
| pmove | 1862 / 1647 / 9470 | 1500 | **over** by 24% on average; p90 within, so a few heavy frames carry the mean |
| ent (active-set pass) | 1249 / 2069 / 20439 | — (new zone; set from M4) | max frame 20 ms |
| phys (toss, step, push, pushstep) | 1124 / 1816 / 20267 | 3500 | within |
| game (thinks) | 915 / 1540 / 24454 | 4000 / 8000 incl. mon | within on average; max frame 24.5 ms |
| mon (monster AI) | 676 / 1015 / 19981 | (game) | |
| rec (records, wakes, hydration) | 313 / 547 / 14086 | 400 + 2000 hydration cap | within |
| snd | 1228 / 1514 / 6025 | **1500** (was 500) | the 500 µs [EST] was wrong: 1097 µs of the 1228 is the wav64 stream pump with ≤ 2 voices and 10 open waves; M6 measures the cost at 16 voices |

Open items for M4/M6: single frames of 14–24 ms in game, ent, phys, mon
and rec (with render on the same frame they can exceed 33 ms; causes to be
attributed with the zone maxima);
pmove's average over budget. RSP/RDP time runs in parallel (SA2 spike:
≤ 4.1 ms for a full dome visible set); `disp` waits are not CPU budget.

### 4.3 Allocation rules

- No allocation can fail on the steady path; everything is sized by
  `limits.h`, carved at boot, and proved by the cook where it depends on
  content (SA2 principle 2).
- Allocators: arena (per-frame scratch, boot tables), first-fit pool (16 B
  granularity, owner-pointer compaction, frame fences for GPU-visible
  blocks; SA2 §4.3), slab (fixed-size slots: components, game private
  size classes, voices, decals, particles, timers).
- No `malloc` after boot in `engine/` or `game/`.
- GPU-visible memory is freed only behind an rspq syncpoint two frames old.
- DMA destinations own whole 16 B cache lines while in flight (start
  16-aligned, length padded to 16): a shared line corrupted the old cart's
  layer header (SA2 §4.4).

---

## 5. Cart I/O and streaming

### 5.1 File table

Player-cooked content is packed into the ROM's DFS by the build
(`HL64_CONTENT`); Valve data never enters the repository. At boot `io`
resolves each file once to (cart offset, size) and the resource table (6.5)
refers to files by index; no path lookups happen afterwards.

### 5.2 Async DMA scheduler

Caller-owned requests (file, offset, length, destination, priority, done
flag); priorities AUDIO > OPEN > SEAM (pins) > PREFETCH; promotion when a
stronger class needs a queued request; one piece in flight ≤ 16 KB;
per-frame budget 64 KB (128 KB while a seam is approached); synchronous
reads only at boot/New Game and for a collision chunk missing mid-trace
(counted as blocked I/O, target 0 frames > 1 ms). Stats per route segment:
KB/frame p50/p90/p99, `budget_over` (must be 0), blocked µs, host
`pi_over`. ares models PI at a fixed 5.25 MB/s without bus contention;
real hardware is checked per milestone when a flashcart is available.

### 5.3 Layers and residency

A **layer** is an open map: its `.w64` directory (resident while open),
pinned hull-0 COLL chunks, HLE1 blob, placement `g_M`. At most 2 geometry
layers and 4 HLE slots (closing maps with frozen timers keep their logic
slot).

Residency classes (strongest first; a request evicts only weaker classes;
SA2 §4.2 as built):

- **open**: REND + TEXP of the eye leaf's visible sectors; MOVR of visible
  brush entities; COLL of grid cells within ±128 u of the player box and of
  solid/contents brushes touching that window; anything the renderer finds
  missing is requested at open priority and skipped (pop-in counted).
- **pin**: route look-ahead destinations (test builds), reachable
  teleport destinations, approached seam's B set, cook-generated script pins;
  lapse after 3 s or on arrival.
- **prefetch**: eye-leaf sets at eye + v·{0.25, 0.5, 1} s; collision window
  + v·0.5 s; cells of moving monsters/projectiles; hulls of brush entities
  in the box spanned by thinking monsters and projectiles.
- **pinned**: world hull-0 COLL while the layer is open.

Eviction: queued-not-started first, then LRU among chunks not wanted this
frame; prefetch never evicts a chunk used in the last 30 frames. The target
set is recomputed only when one of its inputs changed (eye leaf, player
cell, velocity bucket, pins, layer set); otherwise the previous frame's
wants are re-asserted by generation number (no rebuild).

### 5.4 Campaign, portals, seams

CWG3 (`docs/formats/cwg-format.md`): placements, links, seams (switch eye
box, carry box, approach box, twins, B sector set, OPEN draw set, portal
translation `T`). Requirements:

1. Approach box entered (A-local test) → open B's layer and HLE slot, pin
   B's seam set; nothing blocks.
2. Switch: a touch seam switches when the eye enters the switch box, or
   when an armed changelevel stops being touched / has been touched 1 s
   outside the box (`forced`); USEONLY seams switch when used.
3. Crossing: home := B; the player and riders re-base once into B's frame
   (`local += g_M(A) − g_M(B) + T`: the global position moves by exactly
   the seam's `T`, cwg-format.md); velocity,
   angles, punch, inventory and the entity itself kept; twins copy A's state
   to B (origin in B's frame, mover phase, think time, velocity) and A's copy retires
   silently; `changetarget` fires in B; changelevel touches ignored until a
   frame with none.
4. Leave: A's chunks drop by LRU; A's records dehydrate by distance
   (release radius 1024 u, batches every 0.1 s); A closes when nothing of
   it is resident or live; its timers freeze by remaining time and thaw on
   reopen.
5. OPEN seams draw B's draw set translated by −T, scaled about the eye by
   1 + 1/256, limited to meshlets touching the carry box.

GoldSrc shows a loading pause and a level transition at these points; the
continuous world is a deliberate improvement, and every state rule above
reproduces what GoldSrc's transition would leave the player with
(entities in the transition volume carried over, others frozen).

**Coordinate frames.** Simulation runs map-local, as GoldSrc runs in the
one loaded map's coordinates; f32 at global coordinates (≈ ±5000 u and
more) rounds differently, and with pmove in global coordinates 81 PM-bot
runs failed PM2 against 2 home-local (M2Close, measured).

- The player and pmove use the home layer's frame
  (specs/pmove-host.md 2.1): physents are home-frame copies (other
  layers' worlds at `g_M(L) − g_M(home)`), every pmove service takes and
  returns home-local points, the camera is the view + `g_M(home)`, route
  `WAIT_NEAR` compares map-local.
- Every other entity uses the frame of its own layer (`id.layer`), the
  same rule (8, "Frames").
- Global = local + `g_M(layer)` only for the spatial grid, linking and
  touch candidate tests, residency and dehydration distances, rendering
  (per-layer camera-relative matrices, 9) and sound distances.
- A home or layer change re-bases the origin once (step 3 above).

### 5.5 Visibility

Point → leaf: the layer's 256 u grid start node + a short walk of the
pinned hull-0 tree, memoised per entity until its origin moves. Leaf →
visible sectors: cooked u16 list, turned into a per-frame **sector
bitset** (one bit per sector of each open layer; the cooked maps have 36–172
sectors, measured from the old `.w64` directories, so ≤ 2 × 32 B). Rendering
tests an entity by its ≤ 4 overlapped sectors against the bitset (one AND
each). Game PVS queries (find client in PVS, check visibility) use the exact
leaf PVS row from `VISR`, decompressed into a one-row cache per layer, as
GoldSrc does.

---

## 6. Entity system

### 6.1 Choice

GoldSrc's `edict_t` + `entvars_t` (≈ 800 B ILP32) and the old tree's 884 B
Quake edicts are one float-heavy blob per slot, scanned every frame. hl64
stores entities as **typed components in parallel arrays** (structure of
arrays) indexed by slot, with per-class game data in slabs, and runs every
per-frame loop over **active sets** only. The game sees HLSDK names and
semantics through accessors and component pointers. (D1.)

### 6.2 Components (`include/hl64/ent.h`)

Slot index u16, cap 495 (ceil(1.25 × measured host peak 396)). A handle
`ent_h` = index | serial << 16 (u32); `ent_valid(h)` checks the serial.

| component | fields (HLSDK names) | bytes | loops that touch it |
|---|---|---:|---|
| `xf` | origin f32[3], angles f32[3] | 24 | physics, render, sound, traces |
| `mot` | velocity, avelocity, basevelocity f32[3] | 36 | physics |
| `box` | mins, maxs, absmin, absmax f32[3] | 48 | traces, grid, touches |
| `phys` | movetype u8, solid u8, waterlevel s8, watertype s8, flags u32, groundentity / owner / aiment u16, gravity f32 (0 = 1), friction f32 (the grid node is a separate 16 B array, 8) | 32 | physics, traces |
| `tm` | nextthink f32 s (0 = none), ltime f32 s (pushers), think / touch / use / blocked u16 function ids | 16 | think heap, pushers, dispatch |
| `rnd` | modelindex `res_t` u16, sequence u16, frame f32, animtime f32 s, framerate f32, scale u16 (8.8), skin s16, body u8, effects u16, controller u8[4], blending u8[2], rendermode, renderfx, renderamt u8, rendercolor u8[3], sectors u16[4] | 48 | render, studio |
| `id` | serial u16, state u8, layer s8, class u16 (from `classes.def`), private handle u16, record gid u16, runtime id u16, targetname `name_t` u16 | 16 | names, hydration, save |
| **total** | | **228 × 495 ≈ 110 KB measured** (`rnd` 44, `cv` = `xf` + `box` + collision fields 92) | |

- **Game clock in GoldSrc's formats**: `time` is f64 seconds advanced by
  `dt` (f64), starting at 1.0; `nextthink`, `ltime` and `animtime` are the
  f32 values the game writes; a think is due when `(f64) nextthink ≤ time +
  dt` in double. The probe showed the frame a think runs on depends on that
  float rounding (specs/think-and-time.md 1–2), so integer µs would change
  results; µs integers remain for engine-internal timing only (I/O,
  pacing, residency, profiling).
- **SOLID_BSP requires MOVETYPE_PUSH**: setting any other pair is
  `sys_error` naming the class, as GoldSrc stops with a fatal error
  (specs/physics-movetypes.md 2).
- **Ids instead of strings**: targetnames are cooked `name_t` (per-map name
  index from HLE1; global form = slot << 12 | index); classnames are
  `classes.def` ids; models and sounds are `res_t` (6.5). There is no string
  comparison on the cart's steady path. Debug builds keep a name table for
  printing only.
- Floats stay where GoldSrc semantics live (positions, velocities, angles,
  frame, gravity, friction): collision and movement must match GoldSrc to the
  1/32 u, which fixed point cannot do without changing results.
- Game-only state (health, takedamage, enemy, target, noise ids, dmg,
  armor, items…) lives in the game's per-class private structs (HLSDK
  `CBaseEntity`/`CBaseMonster`/`CBaseToggle`/`CBasePlayer` member layout),
  allocated from game slabs (sizes 32–1024 B, budget 160 KB); a full slab
  refuses optional spawns like the spatial reserve.
- The player block (`ent_player_t`: pmove state, punchangle, view_ofs,
  v_angle, buttons, fall velocity, duck state) is one engine record shared
  with pmove.

### 6.3 Active sets and the think heap

- **Think heap**: 4-ary min-heap of (nextthink f32 bit pattern, index),
  ≤ 512 entries; due test in double as above. Setting `nextthink` updates
  the entity's heap entry.
- **Movers set** (bitset, 512 bits): an entity enters when its movetype
  needs a step — velocity or avelocity non-zero, or a falling type not on
  ground, or a pusher with motion, or FOLLOW — and leaves when it comes to
  rest (velocity zeroed and on ground). Setting velocity / origin / flags
  through the API updates membership.
- Each frame: pop every heap entry due by `now + dt` into a **due** bitset;
  active = due ∪ movers; walk the bitset words in index order (16 words) and
  run each entity's movetype step (specs/physics-movetypes.md). Entities
  created during the walk at a higher index join this frame, as in
  GoldSrc's ascending-index physics loop.
- Cost is O(active), typically 30–80 of ≈ 400 live entities [EST from the
  route's monster/mover counts], with no per-entity visit for static props,
  resting items or idle logic.

### 6.4 Records, hydration, dehydration

Map entities exist first as HLE1 records (24 B; `docs/formats/hle-format-v1.md`):
logic (no entity), point (queried by name), physical (become entities when
hydrated). `ent/` owns blob residency, record state (pending / live /
killed / dry), the name index (name id → records of the caller's source
map, plus twin aliasing), the pending grid (512 cells of 512 u per map) and
the budgeted pump. The game owns meaning: `game_spawn_record(gid, e)`
reads typed keys (string-valued keys arrive as `name_t` / `res_t` /
sentence ids, cooked); `game_dehydrate(e, w)` / `game_rehydrate(e, r)`
write the per-class delta (origin, health, mover state, flags, think
time, names, references as (map, lump) or runtime id).

Requirements kept from the old tree (they reproduce GoldSrc's level
semantics in one world; each is an M4 test): spawn radius 768 + 192 u
around anchors (player; player mapped through approached portals), release
1024 u, 256 u prefetch ring requesting the 6 nearest pending records'
models; optional hydration only while 48 slots stay free, transient
effects keep their own 32-slot reserve; named fires and class searches
wake pending records synchronously (≤ 2 hydrate in the frame, the rest
queue first); moves and traces wake pending records in their box via the
pending grid; deferred uses into a closed map as a canonical (map, lump) /
runtime-id FIFO, never entity pointers; closing a map freezes its timers
(128) by remaining time and cuts cross-boundary references; runtime
pickups and monstermaker children persist by (map, runtime id), ≤ 64
pickups; entity budget 2 ms cart / 8 records host, shared in the order
neighbour release, arrival pump, home pump, closing release.

### 6.5 Resources

The cook builds one **resource table** (`campaign.rtb`) for the whole
campaign: every model, sprite and sound any map or the game can use, with
a stable `res_t` (u16), its file index, size and kind; plus the sentence
table and the lightstyle pattern table. Names come from the maps' entity
lumps and from `game/defs/res.def` (the game's own precache lists). Map
keys holding resource names are cooked to `res_t`. At run time there are no
name tables, no precache slots and no slot reuse: `precache` only declares
residency interest (prefetch), and the index cap problem of the old tree
(455 / 347 slots shared by two open maps) disappears. Brush submodels are
(map, submodel) pairs, not table entries; an entity's `modelindex` names
one with the `res_t` kind SUBMODEL, index = map slot << 10 | submodel.
Specs: precache-and-resources.md.

---

## 7. Engine ↔ game interface

A slim, statically linked C API in our own names covering the HLSDK
`enginefuncs_t` / `DLL_FUNCTIONS` subset the Uplink and Hazard game uses
(old game's engine calls: `g_*` QuakeC-style builtins 437 sites, `SV_*`
78, `ED_*` 29, `MSG_*` 40, Cvar/Cmd/Con ≈ 250, `Mod_*` 14, `GSFX_*` 78,
`HLE_*`). Prototypes are written in M0.

### 7.1 Engine services (`eng.h`)

| area | services | HLSDK counterpart |
|---|---|---|
| entities | alloc, free, handle valid, component access, set_origin, set_size, set_model(`res_t` or submodel), link (with/without touch), set velocity/flags (keeps active sets), think_at/think_in | pfnCreateEntity, pfnRemoveEntity, pfnSetOrigin, pfnSetSize, pfnSetModel |
| search | next by `name_t` / class id in a map slot (with wakes), in sphere, in box, client in PVS | pfnFindEntityByString, pfnFindEntityInSphere, UTIL_EntitiesInBox, pfnFindClientInPVS |
| traces | line (with hitboxes, hitgroup), hull 0–3, box, single entity, point contents, material at hit | pfnTraceLine, pfnTraceHull, pfnTraceModel, pfnPointContents, pfnTraceTexture |
| movement | walk_move, move_to_origin, drop_to_floor, change_yaw/pitch | pfnWalkMove, pfnMoveToOrigin, pfnDropToFloor, pfnChangeYaw |
| vis | point/entity in PVS, leaf of point | pfnCheckVisibility |
| resources | precache (interest), model kind/bounds/frames, studio sequence lookup by cooked id or name hash (spawn only), sequence info, attachments, bone positions | pfnPrecacheModel/Sound, pfnModelIndex, pfnGetAttachment, pfnGetBonePosition |
| sound | emit(entity, channel, `res_t`, volume, attenuation, flags, pitch), ambient, stop, sentence by id / group id | pfnEmitSound, pfnEmitAmbientSound |
| light | lightstyle(style, pattern id) | pfnLightStyle |
| effects | direct calls: sparks, ricochet, blood stream/sprite, bullet decal, decal trace, explosion, smoke, beams, tracer, break model, gibs, dlight, elight, muzzle flash, shake, fade, text message (cooked titles ids) | `TE_*` / user messages |
| HUD/2D | ui8 sub-rect with colour/mode, fill, glyphs, screen size | client SPR_*, FillRGBA |
| time, rng | `time` (f64 s), `dt` (f64 s), float `time`; random long/float (one stream, seeded at New Game) | gpGlobals->time, pfnRandomLong |
| settings | typed `settings.h` record (skill, movevars, volume…) | cvars |
| records | typed record keys, record origin/angles/bounds, fire name in slot, fire later, use-targets, master state, timers, map slot of entity, dehydrate/rehydrate | (replaces keyvalue parsing and level transitions) |
| campaign | home map, map slot ids, global offsets | — |

### 7.2 Game exports (`game.h`)

`game_init`, `game_new_campaign`, `game_map_opened/closing(slot)`,
`game_spawn_record(gid, e)`, `game_spawn_logic(gid)`, dispatch of
think/touch/use/blocked by function id (`funcs.def`), `game_start_frame`,
`game_player_spawn/prethink/postthink`, `game_player_command(usercmd)`,
`game_pm_move` (D2), `game_dehydrate/rehydrate`, `game_save/load_globals`,
`game_calc_view`, `game_hud_frame/draw`, `game_studio_event(e, event,
options)`, `game_studio_setup_bones(e, out)` (D3).

### 7.3 Retargeting `game/`

The old `source/hl` (21 files, ≈ 32k lines) runs on Quake's progs entvars
(repurposed fields `lip`, `cnt`, `walkframe`, `state`, `lefty`…), the `g_*`
QuakeC-builtin layer, QuakeC function handles and string names. Per file,
following the audit's §1.5 ranges (16.2; the Quake-interface glue is D9):

1. Quake fields → components (HLSDK names) or the class's private member
   (`lip` "behaviour class" → `mon.ai_class`, …).
2. `g_*`, `SV_*`, `ED_*`, `MSG_*` → `eng.h`; QuakeC helpers rewritten in
   `game/util.c` from HLSDK `util.cpp`, `subs.cpp`, `cbase.cpp`,
   `monsters.cpp`.
3. QuakeC think handles → `funcs.def` ids; float times → `think_in`.
4. String names, classnames, model and sound paths → cooked ids.
5. Protocol writes → effects API.
6. Engine-gap workarounds removed now (the target is GoldSrc, not the old
   engine): grenades become MOVETYPE_BOUNCE with gravity 0.5 / friction 0.8
   as HLSDK sets them, conveyors use basevelocity, etc.

---

## 8. Collision, traces, physics

- **Clip hulls are the collision model** (GoldSrc's compiler output; specs
  in trace-and-hulls.md). COLL chunks: 8 B nodes, axial node classes by
  index range (no plane fetch for axial nodes), non-axial planes per chunk,
  extern gids, grid start nodes. Walks are iterative with an explicit stack
  of **192** entries, in map-local coordinates (global − `g_M`), leaf
  contents from the layer table. Measured maximum root-to-leaf depth over
  every model and hull: hldemo3 146, the other nine maps 32–48 (hldemo1 37,
  hldemo2 46, t0a0 40, t0a0a 44, t0a0b 48, t0a0b1 39, t0a0b2 48, t0a0c 42,
  t0a0d 32); stack overflow is `sys_error`, and the cook rejects a map
  deeper than 192. Retail campaign depths are not measured yet.
- **Precision**: non-axial plane distances and split fractions are
  computed in IEEE double on host and cart (GoldSrc uses x87 extended
  precision; float missed the ±1e-5 fraction tolerance on t0a0 by 1.1e-5,
  trace-and-hulls.md 2); positions and results stay float.
- **Hitboxes**: line traces against studio-model monsters test the bounding
  box, then the current pose's hitboxes (GoldSrc hitgroups for damage);
  poses are computed on demand and cached per frame (trace-and-hulls.md 4.5).
- **Spatial grid instead of an area tree** (as built, M2): one hashed grid
  over global coordinates with three levels — 256 u (the COLL grid's
  cells), 1024 u and 4096 u — plus a huge-object list. An entity is a
  member of every cell its linked box touches at the first level where the
  box spans at most two cells per axis; further memberships come from a
  shared pool of 8 B entries (1485); a 16 B node per entity holds kind,
  query stamp and a rounded box; a 128 B bitmap of non-empty buckets skips
  empty cells. Queries dedupe by per-query stamp; solids come back in
  ascending index, triggers in GoldSrc touch order (cooked `trank` for map
  triggers, creation order for runtime ones). Cart cost (ares): 18.6 µs
  per query, 17.2 µs per relink (measured). specs/area-links-and-touch.md 2.
- **Physics**: per-movetype GoldSrc semantics over the active set
  (physics-movetypes.md, think-and-time.md); the player is pmove
  (pmove-host.md, D2).
- **Frames** (rule for M3 and later, same reason as pmove, 5.4): every
  entity is simulated in the map-local frame of its own layer
  (`id.layer`). `xf.origin` and `box` (mins, maxs, absmin, absmax) are
  stored map-local; movetype steps, traces, point contents, walk moves,
  pushers and all game code see map-local values, as GoldSrc's game does,
  so the P-*/K-* probe captures (map-local) compare directly and record
  origins hydrate without translation. Global (`+ g_M(layer)`) only in the
  grid node box, link/touch candidate tests, residency, rendering and
  sound distances. An interaction across layers (a trace, push, touch or
  rider near a seam) converts the other entity into the mover's frame by
  `g_M(other) − g_M(own)`, as pmove's physents do. An entity changes layer
  only when carried across a seam, re-basing once (5.4 step 3); twins are
  already in B's frame.
- Surface materials come from the cooked material table (no faces or
  texinfo on the cart).

---

## 9. Renderer

All GPU work through tiny3d/rdpq in `plat/n64/gpu.c`; `engine/render`
builds per-frame lists straight from components (no client entity copies).

1. **View**: `game_calc_view` → camera; each open layer gets a
   camera-relative `T3DMat4FP` (`g_M − eye`, s16 map-local verts in range).
2. **World**: eye leaf → sector bitset → sector bbox and meshlet bbox
   frustum cull → counting sort by texture page → per page one TLUT+CI4
   bind, per meshlet one `t3d_vert_load` + one `t3d_tri_draw_strip` of the
   pre-encoded strip. Styled meshlets recoloured only when a style's value
   changes (≤ 10 Hz). Dynamic lights on a scratch copy of touched meshlets;
   flashlight as a decal-Z pool pass. OPEN-seam cross-layer draw per 5.4.
   Texture animation from the cooked sequence fields (world-surfaces.md).
3. **Movers**: visible brush entities with their MOVR meshlets under the
   entity matrix; static submodels are baked into sectors.
4. **Studio**: STB2 v4 bone-local meshlets recorded once per model into an
   rspq block (packing budget 2 ms/frame); per entity per frame the bone
   matrices from `game_studio_setup_bones` (D3) into a double-buffered
   pool and one block replay. Lighting: probe (base + one style term,
   lightstyles.md 4) + dlights. View model CPU-skinned (chrome needs
   bone-space normals). STEP entities interpolated between think steps
   (physics-movetypes.md 7), as GoldSrc's client does.
5. **Particles**: one pool, TinyPX ucode, one call, s16 view-relative.
6. **Translucent**: render modes/fx per render-fx.md; back to front; depth
   test without write; additive through the RDP adder with the wrap guard.
7. **Decals**: on cooked faces of resident sectors/movers, decal Z, I4
   texels from the cooked decal sheet (render-fx.md 4).
8. **HUD/2D**: ui8 sheets, numbers, crosshair, damage indicators, text
   messages, fades; pause menu and debug overlay last.

Vertex lighting replaces GoldSrc's lightmaps (SA2 decision 1, accepted
there): the closest the cart can afford to Half-Life's look; recorded as
D6 (iv).

---

## 10. Audio

libdragon mixer, 16 voices, 22050 Hz; wav64 (VADPCM) streamed from the
cart (`WAV64_STREAMING_FULL`); ≤ 10 open waves with LRU close (never under a
voice); wave reads in the `AUDIO` I/O class. GoldSrc channel semantics,
attenuation and stereo model, loops, sentences: specs/sound.md. Sounds are
`res_t`; sentences are cooked (word `res_t` lists with VOX modifiers),
nothing is parsed at run time. Positions are global, so both open layers
spatialise without conversion.

## 11. Input

Pad → `usercmd` (stick move/look per settings, C-buttons strafe/look, Z
attack, R attack2/jump; table in `engine/core/bind.c`); absolute stick
turning scales by `dt`. The route VM and `ARES_TEST_INPUT` inject at the
same point.

## 12. Settings and debug commands

- **Settings** are one typed record (`settings.h`): physics movevars
  (gravity, friction, maxspeed…, HL defaults), skill, audio volumes,
  controls, render toggles. The game and engine read fields directly; the
  menu edits the persistent subset; saves store it. A static descriptor
  table (field id, type, offset, range) exists for the debug channel only.
- **Debug commands**: a fixed enum of ≈ 20 commands with binary arguments
  (setpos, look, god, noclip, give, impulse, use-name, set-setting, shot,
  stats, timescale, dump-entity, mem, prof) executed at a frame boundary.
  Sources: the route VM, the host CLI (text parsed on the host only), and
  on the cart an optional flashcart USB channel (later). There is no console
  text buffer, no command buffer and no notify overlay on the cart (the old
  cart spent 4–15 ms/frame drawing notify lines near seams); log output goes
  to an 8 KB ring flushed to ISViewer, shown by the debug overlay on demand.
- `setpos`, `god`, `noclip`, `give` exist only in test builds.

## 13. Menu

Text menu in the player's HL font (cooked from their data) over a dark
fill; a built-in 8×8 MIT bitmap font for the boot/error screen. Items: New
Game (Uplink / Hazard Course from the CWG start and training fields),
Load/Save (D4), Options (settings subset), Quit to title.

## 14. Save and level state

Level state is the dehydrate store (6.4). A saved game (D4) = clock and
rng, home map and layer set, every live entity's components, game private
blocks (by class, versioned), dehydrate stores, record states, timers,
lightstyle patterns, player block, settings. Load = New Game shell, open the
saved layers, restore without spawn functions, relink, resume. Size
40–90 KB [EST]; components and ids are position-independent, so saving is
mostly block copies.

## 15. Test and profiling

- **Compiled routes**: route sources stay text (`test/routes/*.route`; the
  old `uplink_route.cfg` converts mechanically), compiled by
  `tools/routec` against the cooked HLE tables into `.rbc` bytecode: ops
  `WAIT us`, `WAIT_MAP map`, `WAIT_NEAR map pos r timeout`, `WAIT_MOVED
  map lump timeout`, `WAIT_GONE map lump timeout`, `WAIT_CLASS class pos r
  timeout`, `WAIT_GLOBAL id timeout`, `LOOK pitch yaw`, `LOOK_AT map lump`,
  `SETPOS map pos angles`, `BUTTONS down up`, `IMPULSE n`, `DROP class pos`,
  `GOD on`, `SHOT label`, `STATS label`, `PASS`. Names are resolved at
  compile time, so a renamed entity fails the compile, not the run. The VM
  executes ops at frame boundaries, logs `route: <op#> <label>` and
  reports `route FAIL` with the op's source line (kept in a debug table).
- **Stats records**: fixed struct (entities live/peak, records pending,
  pools used/peak, heap free/largest, fps + pacing histogram, DMA
  p50/p99, pop-in, blocked I/O, position, time) printed as one text line
  per `STATS` op; field names follow the old tree's where comparable so
  old/new logs compare.
- **PROFILE zones**: enum of ≈ 40 zones (frame, upd, phys, think, trace,
  hitbox, touch, link, pmove, stream, io, hydrate, wake, rnd, world,
  movers, studio, sbuild, bones, trans, decal, hud, snd …), µs and counts,
  rolled up once per second; names map onto the old `quake64:` zones where
  comparable (table in `engine/test/prof.c`) so 4.2 can be re-measured.
- **ares harness**: our PI test peripheral (PPM dump, exit code) and
  input-file injection (`test/ares-testharness.patch`); ISViewer log.
- **Oracles**: GoldSrc probe (HLDS + HLSDK probe DLL), retail captures,
  old tree where it matches GoldSrc: specs/README.md.

---

## 16. Module reuse plan

Verdicts from ProvAudit's final audit (2026-11-13; the binding text is
[provenance-audit.md](provenance-audit.md) — where this table differs, the
audit wins). Labels: **CLEAN**; **CLEAN+QI** (audit QUAKE-INTERFACE: our
code, written against Quake engine interfaces); **QUAKE** / **XASH**
(contains code derived from them); **HLSDK** (→ `game/` or
`tools/hlsdk/`); **FORBIDDEN** (in `9fd3580`, or QuakeC translations).
Actions: **import** (copy, audit entry named in the commit, licence header);
**retarget** (import, replace Quake-interface glue); **split** (import the
audit's "ours" parts, rewrite the flagged ranges from the named spec in the
same commit); **rewrite** (from `docs/specs`, `docs/formats`, allowed
sources); **drop**.

**Policy gate:** PROVENANCE.md admits only CLEAN files today; every
retarget/split row needs D9.

Revision 2 changes the plan: modules whose value was their algorithm keep
it; modules whose shape was Quake's (edict loops, string tables, console,
area tree, protocol) are rewritten even where the audit would allow a
retarget.

### 16.1 Engine

| hl64 module | old-tree source | verdict | action | note |
|---|---|---|---|---|
| plat/n64 boot, carve | `n64/port/quakegeneric_n64.c` | CLEAN+QI | retarget | 3.1 |
| VI pacing, TLUT | `n64/port/r64_vid.c` | CLEAN+QI except 162–172 (`VID_Init`), 252–258 | split | 3.3 |
| sys (error, clock, RSP-crash wrap) | `n64/port/sys_n64.c` | QUAKE (≈ 185 lines) | rewrite | ours (169–250) re-typed from description |
| FPU shim | `n64/port/fpu_shim.c` | CLEAN | import | |
| settings, debug commands, menu, frame loop | `source/{console,cvar,cmd,keys,menu,host,host_cmd}.c` | FORBIDDEN | rewrite | 3.2, 12, 13 (typed design) |
| input | `source/in_*.c` | FORBIDDEN | rewrite | 11 |
| pools | `q64_pool.c`, `q64_pool.h` | CLEAN | import | |
| limits | `q64_limits.h` | CLEAN | retarget | → `limits.h`; slot-table caps (models/sounds/sfx/styles) removed by 6.5 |
| heap probes, cart DMA, io stats | `r64_malloc.c`, `r64_cart.c`, `r64_iostat.c` | CLEAN | import | |
| hunk/zone shims, zone budget | `r64_mem.c`, `r64_hunk.c`, `q64_budget.c` | CLEAN+QI | drop | |
| DMA scheduler | `q64_io.c/.h` | CLEAN | import | 5.2 |
| host io, heap, libdragon shim, audio | `host/q64_io_host.c`, `q64_heap_host.c` (dlmalloc, PD), `host/libdragon.h`, `q64_host_audio.c` | CLEAN | import | |
| host render mirror | `host/q64_host_render.c` | QUAKE | drop | recorder |
| world format headers | `w64_format.h`, `w64_node.h`, `w64_world.h` | CLEAN / CLEAN+QI | retarget | |
| layer load, hull walk, PVS, point→leaf | `n64/port/w64_world.c` | CLEAN+QI except 9 QUAKE regions (≈ 286 lines) | split | regions from trace-and-hulls.md, 5.5 |
| residency scheduler | `n64/port/w64_stream.c` | CLEAN+QI | retarget | + change-driven recompute (5.3) |
| campaign, seams, portals | `n64/port/q64_world.c/.h` | CLEAN+QI | retarget | |
| v1 world | `q64_map.c`, `q64_brush.c`, `r64_world*.c`, `r64_light.c` | mixed | drop | |
| collision, traces | `source/world.c` | FORBIDDEN | rewrite | trace-and-hulls.md |
| spatial grid, links, touches | `source/world.c` (area tree) | FORBIDDEN | rewrite | area-links-and-touch.md (grid) |
| physics, step moves, think | `source/sv_phys.c`, `sv_move.c` | FORBIDDEN | rewrite | physics-movetypes.md, think-and-time.md |
| player movement | `source/sv_user.c` | FORBIDDEN | replace | HLSDK pm_shared (D2) |
| entity components, handles, heap | `source/pr_edict.c`, `progs.h` | FORBIDDEN | rewrite | 6 |
| resources | `source/sv_main.c` precache, `snd_n64.c` name table | FORBIDDEN / CLEAN+QI | rewrite | cooked table (6.5), precache-and-resources.md |
| HLE records, hydration, wakes, timers | `source/hl/hl_ents.c/.h`, `hl_entcook.h` | CLEAN+QI (2486–2524 HLSDK) | retarget | heavier: names/keys become ids, timers keep HLSDK f32 nextthink (specs/think-and-time.md 5); 2486–2524 → `game/` |
| level state | `source/hl/hl_levelstate.c/.h` | CLEAN+QI | retarget | |
| node graph | `source/hl/hl_nodecook.h` | CLEAN+QI except 280–578 | split | tracing on the hl64 API |
| mixer, voices | `n64/port/snd_n64.c` | CLEAN+QI | retarget | spatialisation and channel rules per sound.md (GoldSrc), names → `res_t` |
| sentences | `source/hl/hl_sentences.c` 1–505 | CLEAN | retarget → tool | parser moves into the cook (`tools/sentc`), runtime reads ids |
| sector renderer | `n64/port/r64_sector.c` | CLEAN+QI except 431–453, 634–649 | split | world-surfaces.md 4, dlights.md 3 |
| studio draw | `r64_studio_bake.c`, `r64_studio.c` | CLEAN+QI except 828–879 (XASH), 2036–2048 / 599–609 (HLSDK) | split | chrome and frame wrap from `game/studio` |
| studio bones | `source/studio_geom.c` | XASH/HLSDK 425 lines; ours 36–75, 519–584, 627–1010 | rewrite + split | D3 |
| studio residency | `source/mod_studio.c` | CLEAN+QI | retarget | D3(b) chunks change its units |
| studio structs | `source/studio.h` | HLSDK 8–207, ours 209–285 | split | HLSDK part → `tools/hlsdk/`, `game/studio` |
| software studio, alias models | `studio_soft.c`, `r64_studio_sw.c`, `r64_studio_stubs.c`, `r64_alias*.c` | mixed | drop | |
| translucent, sprites | `n64/port/r64_trans.c` | CLEAN+QI except 87–150, 592–644 (XASH) | split | render-fx.md 1–3 |
| decals | `n64/port/r64_decal.c` | CLEAN+QI except 403–421, 701–764, 944–945 (XASH), 592–619 (QUAKE) | split | render-fx.md 4 |
| sprite parse | `gsfx/gsfx_spr.c` | CLEAN except 16 QUAKE lines | split | moves into the cook (`hl_sprite_cook.py` already converts) |
| particles | `n64/port/r64_part.c` | CLEAN+QI | retarget | |
| 2D | `n64/port/r64_draw.c` | CLEAN+QI except ≈ 48 QUAKE lines | split | 9.8 |
| textures | `n64/port/r64_tex.c` | CLEAN+QI except 725–762 | split | world-surfaces.md 4 |
| render main | `n64/port/r64_main.c` | QUAKE | rewrite | 9 |
| client effects | `gsfx_cart.c`, `gsfx_cl.c` | XASH + HLSDK; our object/draw-list parts CLEAN | rewrite + split | render-fx.md 5 |
| effect helpers | `gsfx_util.c` | CLEAN | import | |
| profiler | `r64_prof.c`, `q64_prof.h` | CLEAN+QI / CLEAN | retarget / import | 15 |
| route runner | `n64/port/q64_route.c` | CLEAN+QI | rewrite | compiled VM (15); the old text syntax is the compiler's input |
| frame stats, selftest | `frame_stats.c`, `q64_selftest.c` | CLEAN+QI | retarget | `engine/test` |
| ares harness, patches | `test_harness.h`, `test/ares-*.patch`, `libdragon-patches/` | CLEAN | import | |
| SDL / Android / GLES | `gles/`, `sdl/`, `android/` | — | drop | N64 only |

### 16.2 Game (`game/`, HLSDK licence)

| files | verdict | action |
|---|---|---|
| `hl_grunt`, `hl_monsters`, `hl_monstermaker`, `hl_ambient`, `hl_scripted`, `hl_sentences` (entities), `hl_nodes`, `hl_movers`, `hl_breakable`, `hl_items`, `hl_weapons(.h)`, `hl_logic`, `hl_env` (372–1166), `hl_hud`, `hl_use(.h)`, `hl_material(.h)`, `hl_ai` | HLSDK translations on forbidden helpers | retarget per 7.3; rewrite inside: `hl_ai.c` 746–917 and every `g_*`/`FoundTarget`/`M_Visible`/`ai_run`/`walkmonster_start` call; `hl_material.c` 236–290, 554–568; `hl_use.c` 120–176 (delete), 369–386; `hl_monsters.c` 255–269; `hl_nodes.c` 228–243 and `hl_scripted.c` 239–256 (→ `drop_to_floor`); `hl_weapons.c` 303–306 |
| `hl_view.c` | HLSDK-side; 62–93 Valve shake | retarget into `game/hud`, rewrite 62–93 from HLSDK |
| `hl_input.h`, `hl_view.h`, `hl_hud_rects.h` | CLEAN | import |
| `gsfx_sv.c`, `gsfx_te.c` | HLSDK; 479–518 QUAKE | retarget onto the effects API, rewrite 479–518 |
| `source/g_*.c`, `game.c`, `game_weapons.c`, `g_entvars.h`, `g_fielddefs.c`, `g_api.c` | FORBIDDEN | not used; `game/util.c` from HLSDK |
| HLSDK `pm_shared` files | HLSDK | `game/pm/`, caps per pmove-host.md (D2) |
| HLSDK `StudioModelRenderer` pose maths | HLSDK | `game/studio/` (D3) |

### 16.3 Tools and docs

| item | verdict | action |
|---|---|---|
| `tools/w64c/w64c.py` | CLEAN except QUAKE 121–128, 344–373, 424–431, 838–850, 861–944, 1524–1534; HLSDK 1349–1446; `cook_face` 405–519 follows the v1 renderer | split: rewrite from world-surfaces.md and the BSP vis row format, prove with W1; 1349–1446 → `tools/hlsdk/`; probe format v3 (lightstyles.md 4) |
| `tools/w64c/w64check.py`, `w64seam.py` | CLEAN except 296–368 / 77–83 | split |
| `tools/w64c/hlecook.c` + `hl_entcook.h` | CLEAN | import; extended to cook names/classes/resources to ids |
| `tools/studio_bake` | CLEAN, links `studio_geom.c` | retarget onto `game/studio`; D3(b) adds per-sequence animation chunks |
| `hl_campaign.py`, `ui_bake`, `hlhud_bake`, `world_tex_bake`, `hl_stage_content.py`, `hl_sprite_cook.py`, `hl_gsfx.py`, sound scripts | CLEAN | import (staging retargeted to the hl64 layout) |
| new tools | — | `restab` (campaign resource table + pattern table), `sentc` (sentences), `routec` (routes), `touchrank` (HLE1 `trank` computed from trigger geometry with the measured cell model, after `hlecook`; the GoldSrc A-order captures are its regression test), all MIT |
| `q64mapc`, `q64zone`, `hl_world/*` | — | drop |
| `w64-format.md`, `cwg-format.md`, `hle-format-v1.md` | CLEAN | import into `docs/formats/` with the audit's scrub; add `rtb-format.md`, `route-bytecode.md` in M0 |
| `stream-arch-v2.md` | CLEAN | cite as history |
| `uplink_route.cfg`, Hazard walk routes | CLEAN data | convert to `.route` sources, compile with `routec` |

---

## 17. Milestones and gates

Every gate passes on host (`make host && test/run/<gate>.sh`) and ares
(`--ares`). GoldSrc probe fixtures are captured before the milestone that
uses them (specs/README.md).

| M | deliverable | gate |
|---|---|---|
| M0 | Makefile (rom + host), plat, boot carve, settings, debug commands, log ring, ares harness, resource-table and route compilers (stubs of the formats), provcheck | ROM boots in ares to `boot ok` with the carve table and exits 0 via the harness; host prints the identical table; `tools/provcheck`: no forbidden file, no line-level match against `9fd3580` files above threshold, licence headers right, no Quake engine identifiers in `engine/` |
| M1 | io scheduler, layer open, sector renderer, movers static, textures + animation, lightstyles, free camera; cook ranges rewritten | 3 hldemo1 poses: ares SSIM ≥ 0.95 vs the old cart (world only; both are the same cooked world, so the old tree is a valid oracle here); recorder counts = cook prediction; world CPU ≤ 2 ms on the dome pose; W1–W5 |
| M2 | COLL residency, traces + hitboxes, spatial grid, pmove host + pm_shared, input, view | trace-and-hulls.md tests on GoldSrc probe fixtures; area-links A1–A6; pmove PM1–PM7 against probe bot runs; route `m2-walk` PASS |
| M3 | components, active sets, think heap, records, hydration (one layer), movetypes, resources, sound, studio, translucent/decals/sprites, HUD, game/ retargeted | compiled Uplink route segment A to the elevator `+use`, `WAIT_MOVED startele1`, changelevel fire logged; physics/think/sound/lightstyle/dlight/render-fx tests pass on their fixtures |
| M4 | streaming crossings: CWG3, seams, twins, dehydrate/rehydrate, timers, async residency with change-driven recompute | route through hldemo1→2→3: PASS, 0 forced switches on host, `budget_over=0`, `open_fail=0`, 0 frames > 1 ms blocked I/O, pop-in only in teleport windows, entities continuous across seams |
| M5 | full Uplink route (converted `uplink_route.cfg`) + Hazard routes | PASS on host and ares at the same checkpoints; any `WAIT_NEAR` radius widened versus the old route is listed with its GoldSrc-behaviour reason |
| M6 | performance | ares median fps ≥ the old cart's at every stats step (same pacing); no frame > 50 ms within ±2 s of a seam switch (misses reported with zones); 4.1 and 4.2 [EST] rows replaced by measurements; flashcart run if available |

---

## 18. Build and conventions

### 18.1 Build

`Makefile` includes `$(N64_INST)/include/n64.mk` and `t3d.mk`; `N64_INST`
= `~/sdks/n64-sdk`, libdragon replaced by the banked prefix (its MANIFEST
rev must equal the SDK's `libdragon_rev`), toolchain
`~/sdks/mips64-toolchain`, out-of-tree `BUILD_DIR` (scratch on
`/scratch`). Targets: `rom`, `host` (`HOST_M32=1` default), `cook`
(`HL64_DATA` → `HL64_CONTENT`), `routes`, `test-host`, `test-ares`,
`provcheck`. `-std=c99 -O2 -Wall -Wextra -Werror` for `engine/` and
`plat/`. Variants: `PROFILE=1`, `HL64_TEST=1` only. ROM ≤ 64 MiB
(flashcart limit; Uplink + Hazard ≈ 58 MiB in the old tree).

### 18.2 Code conventions

- C99, no VLAs; recursion only where the depth is bounded by data limits
  (hull walks use an explicit 192-entry stack, 8).
- djb-style minimalism: small modules, one data path, no speculative
  abstraction, no allocation on the steady path.
- Data-oriented: per-frame loops iterate arrays of one component over an
  active set; no per-frame walk over all entities; ids (u16) instead of
  pointers in anything stored, saved or cooked.
- Integer where it saves cycles or bytes without changing GoldSrc-visible
  results: ids, colours, light values, quantised render data (s16 verts,
  8.8 scale); the game clock keeps GoldSrc's f64 time / f32 nextthink.
  Floats for positions, velocities, angles and
  collision results, IEEE double for non-axial plane distances and split
  fractions in hull walks (8); no `-ffast-math` in `coll/`, `phys/`; host
  builds with `-ffp-contract=off` and SSE2 so host and cart agree bit for
  bit.
- Hostile input: every cooked file is untrusted — magic, version, counts
  against `limits.h` and against bytes present, offsets inside their chunk,
  alignment, CRC (directory at open; chunks on the host always, on the
  cart in debug builds), ids inside their tables, node children valid,
  depth bounded. Failure → `sys_error(file, offset, rule)`. Host fuzz
  harness for every loader from M0.
- Big-endian cooked data read in place on the cart; the host swaps once.
- Module prefixes per directory (`io_`, `wld_`, `col_`, `phy_`, `ent_`,
  `res_`, `snd_`, `ren_`, `in_`, `dbg_`, `ui_`, `sav_`, `rt_`, `prof_`,
  `sys_`); no Quake/GoldSrc engine identifiers in `engine/` (provcheck).
- Every file starts with its licence line and a purpose paragraph.

---

## 19. Decisions

All recommendations below were accepted by the user on 2026-11-13 (D1 a,
D2 a, D3 b, D4 FlashRAM, D5 recorder, D6 i-iv as recommended, D7 Expansion
Pak, D8 copy with note, D9 a, D10 a).

Update 2026-11-13 (user): the full campaign may span several carts if it
does not fit one 64 MiB cart. Moving between carts uses a Controller Pak
save (32 KB): the GoldSrc changelevel carry set (player state, inventory,
global state, landmark-relative transition) plus the next cart/chapter id.
Within-cart saves stay on FlashRAM per D4. Cart split points follow the
campaign graph's chapter boundaries; sizing is redone with the retail data.

| # | decision | options | recommendation |
|---|---|---|---|
| D1 | Entity representation (M0) | (a) typed components in parallel arrays (220 B per slot), active sets, think heap, ids, HLSDK field names via accessors (6); (b) HLSDK `edict_t` + `entvars_t` verbatim (≈ 800 B per slot, scanned per frame) | (a): ≈ 329 KB less than the old edicts at the 495 cap, O(active) frame loops, exact host/cart sizes, saves by block copy |
| D2 | Player movement (M2) | (a) HLSDK `pm_shared` in `game/pm/` (HLSDK licence), caps 64 physents / 8 ladders / 32 touches; (b) a new clean engine movement from a GoldSrc behaviour spec | (a): it is GoldSrc's movement, allowed by PROVENANCE; probe bots verify the adapter |
| D3 | Studio animation (M3) | (a) HLSDK `StudioModelRenderer` pose maths in `game/studio/` over whole MDL animation data resident in the studio pool (as the old tree does); (b) the same pose maths over cooked per-sequence animation chunks (the MDL's run-length values regrouped per sequence, 16-aligned, streamed individually) so only the sequences in use are resident | (b): identical poses (same values), ≈ 60 KB instead of 93–142 KB per NPC model, so ≈ 500 KB less studio pool [EST, SA2 §3.6]; costs one cook tool and a format doc in M3 |
| D4 | Saved games (after M5) | none; FlashRAM 128 KB; SRAM 32 KB; Controller Pak 32 KB | FlashRAM: 40–90 KB [EST] does not fit 32 KB |
| D5 | Host renderer output (M1) | draw-list recorder only; plus a host software rasteriser | recorder; rasteriser only if ares turnaround blocks iteration |
| D6 | Behaviour target | **settled by the user: GoldSrc as observed.** Remaining sub-decisions: (i) frame-time clamp 0.1 s (stability on the cart) vs GoldSrc's 0.25 s; (ii) hitbox traces for line traces vs bbox only (cost ≈ 0.1 ms on firing frames); (iii) model lighting following animated lightstyles via the probe style term (cook format v3) vs static; (iv) vertex lighting instead of lightmaps (SA2 decision 1) | (i) 0.1 s; (ii) hitboxes; (iii) style term; (iv) keep vertex lighting |
| D7 | Console requirement | Expansion Pak required; also 4 MB | Expansion Pak |
| D8 | Imported file history | copy with a commit note naming the audit entry; subtree import | copy with note |
| D9 | Admit QUAKE-INTERFACE files (blocks M0 imports) | (a) amend PROVENANCE.md: importable when the same commit replaces every Quake binding the audit lists and rewrites its flagged ranges; provcheck verifies; (b) CLEAN-only | (a), now for fewer files: revision 2 already rewrites the route runner, console, area links and resource tables; (a) still saves ≈ 15k lines of streaming, seam, record, level-state, sound, studio-residency and sector/studio renderer code whose algorithms are ours |
| D10 | GoldSrc oracle (blocks M2 fixtures) | (a) run Valve's Linux dedicated server (steamcmd app 90, anonymous) with a HLSDK-built probe game DLL and the player's Uplink maps on this machine, plus retail client captures when the user's Steam login is available; (b) no GoldSrc runs: specs from HLSDK and documentation only, old tree as fallback oracle | (a): server-side behaviour (traces, touches, physics, think timing, pmove via bots) becomes measured ground truth; black-box observation is allowed by PROVENANCE |
