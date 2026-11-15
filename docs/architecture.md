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
   rspq, tiny3d; display 320×240×16, two framebuffers, 16-bit Z; our
   libdragon fork (18.1: DragonMinded `preview` + the banked RDRAM
   overlay, FB/Z in separate RDRAM banks).
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
| 3 | io | plan / publish (`io_frame_begin`, `io_pump`; the PI interrupt chains the plan's pieces through the frame; `io_pump` again after steps 6, 9 and 10 resumes a chain a sync / audio read paused) |
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
Peaks are one-map values; M4 re-measured the two-layer regions (ares, route
`m4`, worst over the ±2 s windows of both crossings, `test/m4/gate_table.py`;
`/scratch/hl64/builds/SeamPerf/g4/m4/table-seams.md`). The render, texture
and collision pools are LRU caches: the 90% / peak × 1.25 rule applies to
their **want** peak (open + pin classes), the resident high-water is
information only.

| region | hl64 KB (carve) | M3 peak (use) | old tree | basis |
|---|---:|---:|---:|---|
| code + data + bss (cart image) | ≤ 900 **OVER** | **M4: 1674 KB** (`mem: image`; PROFILE + `HL64_TEST` cart, text 1009752 + data 158304 + bss 545496 B, main `f913147`); M3: 1475 KB | 1428 **measured** (HL cart ELF) | flagged OVER until M3Lead2 reports the top contributors (the playable image drops route VM, debug and PROFILE code: not yet measured). `HL64_HOST_HEAP_KB` is derived at build time from the cart image size, so the host heap tracks the cart's. With overlays (7.4) the resident image drops by ≈ 106 KB and stops growing with the campaign's class count |
| code overlay arena (TLB frames, 7.4) | 192 [EST] | — (M6) | — | cook-proved: Uplink pair 132 KB / 18 TLB entries, retail [EST] ≈ 160 KB / ≈ 22 entries; 48 × 4 KB frames, ≤ 30 wired entries |
| framebuffers ×2 + Z | 450 | — | 450 | measured |
| rspq / tiny3d / matrices | 96 | not reported | (in heap) | [EST] |
| render / texture / collision pools | 458 / 214 / 208 | M3: 117 KB (49%) / 99 KB (77%) / 55 KB (26%); **M4 want peak** 243104 B (52%) / 173376 B (79%) / 86736 B (41%) on ares (route m4, c7 cook; host c7 and c8 cooks the same); route hazard host 254464 / 121168 B | 240 / 128 / 208 | render and texture **cook-proved** (`tools/w64c/w64seam.py` crossing window, cwg-format.md: A's open set at every approach eye + B's seam pin and arrival leaves, or B's open set at the arrival eyes + the reverse seam's pin, 16-aligned, × 1.25): worst render seam hldemo2→hldemo3 (hldemo2_3b) 375072 B, texture hldemo2→hldemo1 174496 B; proof = measured at every fresh want generation of routes m4 and hazard; collision cook-proved (seam hldemo3→hldemo2, 206592 B) |
| layer directories (2 open) | 267 | M4: 175456 B (64%) at hldemo2 + hldemo3 | 2 × 38–84 | cook-proved (`tools/cook/layerdirs.py`, every linked pair): worst t0a0b + t0a0a 218624 B × 1.25 |
| campaign graph (CWG3) | 6 | 4730 B (Uplink + Hazard) | (parsed per map) | cook-proved (`stage.py` fails above the region) |
| studio pool | 1024 (D3 b) | M3 225 KB (22%); **M4** resident 635104 B (61%) at hldemo2→hldemo3 with soft residency, want (live + pinned) 410032 B | 1536 | **measured**. Soft residency: meshes and animations of models no live entity uses are freed after 5 s unused (crossing-2 peak 94% → 61%) |
| studio skinned-mesh tables, view model scratch | 192 | not reported | 256 heap reserve | [EST] |
| entity components | 134 | 137168 of 137216 B (100%) | edicts 495 × 884 = 427 | **measured**, exact by construction: 495 × 228 B + 24308 B grid nodes, membership pool, touch rank, free times |
| game private data | 160 | 30.6 KB (19%), 98 live entities | (inside the edicts) | **measured** ≈ 320 B per live entity; at the 495 cap ≈ 155 KB, so 160 stays |
| HLE slots | 240 | M3: 77.9 KB for **one** map (m3-a, before infodecal/env_glow); **M4**: 151244 B (62%) with hldemo1 + hldemo2 open, 132716 B at hldemo2→hldemo3 (hldemo1 frozen: 2780 B) | 72 KB text + 13–19 KB keys per map | **measured** (cook5, infodecal and env_glow kept): hldemo1 chunk 52000 B, resident slot 67344 B, 72512 B while opening, plus 20016 B shared tables; cook-checked need below (M4 cook: 165440 B) |
| dehydrate store | 64 | M4: 14364 B (22%) at hldemo2→hldemo3 (host route end 15300 B) | similar | **measured**; closing maps store every entity, released pristine records store nothing |
| resource table + pattern table | 56 | 43.5 KB (78%) | precache name tables, sfx names | **measured** (the earlier 12 KB counted the 12 B entries only) |
| saved games (`HL64_SAVE_KB`, 14) | 160 | largest staged state 70418 B (Uplink route, hldemo2 → hldemo1 autosave, host), image ≤ 32256 B | — (no saves) | by construction: header + settings block + 32256 B image cap (2 flash sectors, written over frames) + 24 KB coder work area + the staged state (≤ 106496 B); replaces the unused 24 KB sentence-table carve |
| lightstyle patterns + state | 4 | not reported | strings + 96 × value | lightstyles.md |
| think heap, active bitsets, grid tables | 12 | 6.3 KB (52%) | area tree + leaf lists | **measured** |
| sound (16 voice buffers, 10 wav64, loops) | 104 | 76.1 KB (79% of 96) | M5 hazard route 80.9 KB (82832 B) × 1.25 > 100 KB → 104 | **measured** (route, m5-int2 `eb47299`; uplink 82080 B; the earlier `snd_ares.sh` peak was 78144 B) |
| sprite cache / decals / particles | 64 / 106 / 12 | — / 51.7 KB (92% of the old 56 KB, m3-a) / — | 64 / 8 / 512 | decals: global sheet + 2 per-map placed-decal sheets (`DECL`, one per open layer) + piece vertices, cook-proved (`tools/cook/decals.py`): Uplink 76672 B (29856 + 2 × 11888 t0a0b + 23040), retail 86528 B; carve = retail × 1.25 → **106 KB** (`HL64_DECAL_TEXELS_KB`) |
| dlight scratch | 32 | not reported | same | 2 × 512 vertex pairs × 32 B by construction (the flashlight pool's 14 KB went with D12) |
| HUD sheets + font (ui8) | 128 | 63.5 KB (50%; 65024 B, font creditsfont) | similar | **measured** |
| debug log ring (cart) | 8 | — | 16 KB console text | 12 |
| per-frame scratch arena | 96 | not reported | — | |
| aithink (spread monster thinks: 6 contexts × (8 KB stack + 8 KB cover tables)) | 96 | stack peak 4552 B of 8192 (cart, routes uplink + m4); cover tables 2 × 1024 × f32 by construction | — (thinks ran whole) | **measured** stack (`prof ai_ctx stack=`; co.c logs a context past 3/4, overflow into its guard words is fatal); M5 `game/hl/aithink.c`, think-and-time.md 2.1 |
| stack + libdragon slack | 256 | 4.5 KB stack peak | — | measured stack only |
| **total** | **≈ 5.1 MB** (carved heap 3696 KB measured) | | old cart route low point: 259 KB heap free (PROFILE) **measured** | headroom ≈ 3 MB |

Rows still marked "not reported" print a `mem` line at boot and a
high-water mark in every stats record from M4; each carve becomes
peak × 1.25 (M4 for two layers, M6 for the full route). Region use above
90% (decals) or a peak × 1.25 over the carve is a gate failure.

**HLE slots for M4** (2 geometry layers with full slots + 2 frozen maps, 5.3). Four
full slots would need ≈ 4 × 65 KB + 16 KB ≈ 276 KB. hl64 instead defines
what a closing map keeps:

- **Open** map (geometry layer resident): full slot, chunk + runtime.
- **Frozen** map (closed with frozen timers or logic still referenced):
  runtime only — record states (2 bits each: pending / live logic /
  killed / dry) and logic blocks (16 B per logic record): 2780 B for
  hldemo1 (488 records, 166 logic). No master table or name index: names
  belong to open slots. Timers
  frozen by remaining time, deferred uses by (map, lump) / runtime id and
  runtime entities by (map, runtime id) sit in the shared tables; deltas in
  the dehydrate store. The name index comes back with the chunk: no fire
  can reach a frozen map (names carry the open slot; uses into it wait in
  the FIFO). The chunk (immutable cooked records and keys) is dropped and
  re-read from the cart when the map reopens: ≈ 50 KB on the OPEN DMA
  priority in the approach window, before any of its records can hydrate
  (no record of a closed map can). Two frozen maps are kept; a third
  freeze forgets the oldest (it reopens as cooked).
- **Cook check** (`hleids`): need = shared tables + the **opening** map at
  its open-time need (the heap must hold it while it opens) + one open
  map's resident slot + 2 × frozen runtime part, each 16-aligned, over the
  worst maps of the campaign. Measured (M4 Rec, c6 cook, Uplink + Hazard):
  **165440 B** (tables 20016 + opening hldemo1 72512 + open hldemo1 67344
  + 2 × frozen hldemo1 2780). Earlier (M3, runtime-sized frozen slots):
  187232 B Uplink + Hazard, retail estimate 194768 B (worst c2a5f).
  `HL64_HLE_BLOBS_KB` stays **240 KB**; the cook proof is the hard bound
  and fails any campaign over the carve. Evidence:
  `/scratch/hl64/tmp/M3Lead2/cook4.log`, `cook5.log`,
  `/scratch/hl64/tmp/Rec/retail/`, Rec commits `48e213e` (M3) and the
  M4 Rec commit (test/m4/rec.sh frozen proof).

### 4.2 CPU per frame (33.3 ms at 30 fps)

**Acceptance (user):** a steady 30 fps; dips to ≈ 17 fps (≈ 60 ms frames)
are acceptable if infrequent. The measurable form is the M6 gate (17):
frames > 50 ms ≤ 2% of a route's frames and never more than 5 in a row
outside seam / teleport windows, no frame > 67 ms outside teleport /
route-`SETPOS` windows, distribution reported per map.

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
| overlay map (7.4, M6) | — | [EST] ≈ 0.05 ms CPU per unit on a frame that maps one, DMA async | at most one unit mapped per frame; measured in M6 |
| snd | 1228 / 1514 / 6025 | **1500** (was 500) | the 500 µs [EST] was wrong: 1097 µs of the 1228 is the wav64 stream pump with ≤ 2 voices and 10 open waves; M6 measures the cost at 16 voices |

Open items for M6 (serious). M4 on ares (route `m4`, main `f913147`,
`/scratch/hl64/tmp/FinalGate/final-f913147/`): frame avg **46.6 ms**,
p90 93 ms, so the route runs well below 30 fps. The worst frames,
**245–384 ms**, come from `monster_human_grunt` AI in hldemo2/3
(`Mon_RunAI`: walk-move traces, node-graph cover search, route
triangulation): 383969 µs in ent/phys/mon at hldemo2 frame ≈ 4780 (phys
`step_us` avg 150747 µs, a 4.9 fps window), and 294 ms at the second seam
switch + 14 frames (ent/mon 245 ms). M3's single 14–24 ms frames and
pmove's average over budget remain. RSP/RDP time runs in parallel (SA2
spike: ≤ 4.1 ms for a full dome visible set); `disp` waits are not CPU
budget.

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
stronger class needs a queued request; pieces ≤ 2 KB (0.4 ms of PI),
planned into an array and chained by the PI completion interrupt (the
handler only lands the piece and launches the plan's next one: no queue
work, no callbacks, no allocation; the main thread shares the plan with
interrupts off; on the cart a piece goes through libdragon's DMA queue,
which may hold it behind the mixer's or DFS's transfers, so it lands by
its queue ticket, not by the PI's status); a sync / audio read pauses the
chain until the next `io_pump` (so one frame step's reads wait for one
piece at most) and
cart file lookups hold it;
per-frame budget 64 KB (128 KB while a seam is approached); synchronous
reads only at boot/New Game and for a collision chunk missing mid-trace
(counted as blocked I/O, target 0 frames > 1 ms). Stats per route segment:
KB/frame p50/p90/p99, `budget_over` (must be 0), blocked µs, host
`pi_over`. ares models PI at a fixed 5.25 MB/s without bus contention;
real hardware is checked per milestone when a flashcart is available.
`HL64_TEST` builds plan each frame's pieces at its start and publish
their completions at the next frame's start (15, K10).

### 5.3 Layers and residency

A **layer** is an open map: its `.w64` directory (resident while open),
pinned hull-0 COLL chunks, HLE1 blob, placement `g_M`. At most 2 geometry
layers, each with a full HLE slot, plus 2 **frozen** maps (runtime part
only, 4.1; a third freeze forgets the oldest, which reopens as cooked).

Residency classes (strongest first; a request evicts only weaker classes;
SA2 §4.2 as built):

- **open**: REND + TEXP of the eye leaf's visible sectors; MOVR of visible
  brush entities; COLL of grid cells within ±128 u of the player box and of
  solid/contents brushes touching that window; anything the renderer finds
  missing is requested at open priority and skipped (pop-in counted). A
  spawn (New Game, save load, `SETPOS`) reads the window's COLL, and the
  submodel hulls of the pending records in it (a trigger the first frame's
  touch wakes), as load I/O before the first frame: the move runs before
  step 9 names the set, so frame 1 would read them synchronously.
- **pin**: route look-ahead destinations (test builds), reachable
  teleport destinations, approached seam's B set, cook-generated script pins;
  lapse after 3 s or on arrival.
- **prefetch**: eye-leaf sets at eye + v·{0.25, 0.5, 1} s; collision window
  + v·0.5 s; cells of moving monsters/projectiles; hulls of brush entities
  in the box spanned by thinking monsters and projectiles; cells of the
  hydration candidates' spawn boxes and of the boxes the thinks of a
  neighbour map's spawner logic records due within 1 s will create
  entities in (`game_spawner_box`: a monstermaker's child far from the
  player).
- **pinned**: world hull-0 COLL while the layer is open.

Eviction: queued-not-started first; then, the pools being first-fit over
address-ordered blocks, the cheapest run of consecutive free, fenced and
evictable blocks that spans the request (weakest strongest victim class,
then youngest victim oldest, then fewest bytes: the LRU order of single
evictions, made to form a hole; one at a time they free scattered blocks a
fragmented pool never joins); with the hole there but the pool's block
descriptors used up, one victim whose free merges with a neighbour. A
search classifies each block once per frame and target generation (its
memo; naming clears it, a victim is re-checked before its drop), and the
requests that would wait on the same fenced bytes (the pool's longest
free + fenced run covers them, until it allocates) skip the search: one
full walk per request was 55 ms of stream time in hl2-return-battle.
Victims are chunks not used this frame or the last; prefetch never evicts
a chunk used in the last 30 frames. The target
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
   angles, punch, inventory and the entity itself kept; carried entities
   re-base by the landmark delta, their game state as GoldSrc's transition
   restore leaves it (position fields moved, a monster's schedule and route
   anew: `game_carried`); twins copy A's state
   to B (origin in B's frame, mover phase, think time, velocity) and A's copy retires
   silently; `changetarget` fires in B; changelevel touches ignored until a
   frame with none.
4. Leave: A's chunks drop by LRU; A's records dehydrate by distance
   (release radius 1024 u, batches every 0.1 s). An entity in a quiescent
   state that must not be interrupted (a monster in a script) refuses
   this **distance** release only. A **closes** when nothing of it is live
   within 1024 u of the player (`rec_slot_held`), not when nothing of it
   is live at all; closing stores **every** remaining entity in the
   dehydrate store (scripted monsters keep their script; M4: 0 dropped),
   freezes its timers by remaining time and keeps its frozen runtime part
   (4.1); everything thaws on reopen.
5. OPEN seams draw B's draw set translated by −T, scaled about the eye by
   1 + 1/256, limited to meshlets touching the carry box.
6. Approach ghosts: while the approached seam's B is ready (B runs while
   the player approaches), B's live entities drawn into B's carry box —
   not a twin's B copy, not one an A entity of the same class within 1 u
   of the mapped place stands for — draw in A's view at their global
   position − T (studio, brush and sprite entities; culled by A's visible
   sectors), so what the arrival shows in the carry box shows before the
   switch (seam.h `seam_ghost`). Drawing only: not solid, no touches or
   sound in A. The switch's continuity check holds them in its "before"
   set and matches each to itself after the switch.

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
  The order stays GoldSrc's ascending index; grouping entities by class
  (for instruction-cache locality) is not done unless a measurement shows
  a large icache win that is behaviour-neutral (same route results and
  entity records).
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
pending grid, a moving pusher also those up to 256 u above its swept box
(what rests on it: GoldSrc's had fallen there at map start), settled by
DROP_TO_FLOOR before it moves (physics-movetypes.md 4.2); deferred uses
into a closed map as a canonical (map, lump) / runtime-id FIFO, never
entity pointers; closing a map freezes its timers
(128) by remaining time and cuts cross-boundary references; runtime
pickups and monstermaker children persist by (map, runtime id), ≤ 64
pickups; entity budget 2 ms cart / 8 records host, shared in the order
box wakes (links, pushers: synchronous, a frame where they alone exceed
the budget is logged and counted, stats `rec: wakeover=`), neighbour
release, arrival pump, home pump, closing release.

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

### 7.4 Code residency (overlays)

User direction: only the code in use is resident (active monster and
entity code, the menu while it is open), in dedicated arenas. No
libdragon DSOs: `dlopen` reads synchronously, allocates from the general
heap and keeps name-based symbol tables, which break 4.3 and the
no-stall rule.

**Units.** Sizes are text + data of the cart objects (main `8532e97`).

| resident (always) | overlay units |
|---|---|
| engine (render 91 KB, ent 58, pm 34, …), libdragon 139, tiny3d 19 | one per **monster class** with helpers only it uses: grunt 18.2, garg 13.4, turret 9.7, scientist 9.0, houndeye 7.4, islave 6.8, headcrab 6.6, barney 5.8, roach 4.4, monster_tripmine 4.2, barnacle 4.1, generic 2.7, rat 0.7 (Uplink 93 KB; gman and zombie not written yet, [EST] 8 KB each; the retail campaign adds ≈ 39 classes) |
| shared AI base: monsters 40.4, talkmonster 14.3, aicombat 12.5, schedule 11.5, scripted 7.2, aisquad 6.9, nodes 6.8, grenade 5.6, defaultai 3.6, sentences 3.6, aisound 0.8 (≈ 113 KB) | **map-only logic sets** (classes used by few maps, e.g. retail `func_tank`, `func_tracktrain`, Xen bosses) when ≥ 4 KB |
| always-used game code: player 18.6, util 19.9, dispatch 10.0, triggers, doors, plats, buttons, bmodels, breakable, env, ambient, items, chargers, logic, lights, world, material, monstermaker, HUD, view | **front-end** (title, new game, options, save/load browser) and the in-game save/load screen (not written yet; [EST] ≈ 12 KB) |
| **weapons** (base 16.5 + per-weapon 1.3–2.8; Uplink 29.5 KB, retail [EST] ≈ 50 KB) | — |

Weapons stay resident: a full inventory pins every weapon unit, so
overlaying them saves nothing at the worst point and costs mapping
entries. Never in playable ROMs (build flag, not overlays): route VM
(10.1 KB), test hooks, debug commands beyond the log ring (`dbg` 2.4 KB).

**Mechanism: TLB-mapped fixed-virtual overlays.** libdragon never
programs the VR4300 TLB (our fork only reports TLB exceptions), so all 32
entries are free; hl64 writes them as **wired** entries (C0 `Wired`) and
needs no refill handler.

- Each unit is linked once, at its own unique virtual address in mapped
  kseg2 (from `0xC0000000`; kuseg stays unmapped so NULL faults), 4 KB
  aligned, with `-G 0` (grunt.o has 159 gp-relative accesses today, which
  cannot reach an overlay) and `-mlong-calls` for calls to resident code
  (`jal` reaches only within the current 256 MB segment, and kseg2 is not
  kseg0's; [EST] +10% unit size, M6 measures). Resident references are
  absolute and resolved by the linker against the final resident ELF
  (`--just-symbols`); nothing is relocated at run time and there is no
  relocation table.
- Overlay file (built by `tools/ovlx`, host tool, MIT, provcheck-clean,
  from the unit's linked ELF): header (virtual base, page count, bss size,
  build hash of the resident ELF — a mismatch is `sys_error`), the text +
  data blob, and the dispatch entries (`funcs.def` id → virtual address,
  `classes.def` id → class ops address).
- **Code arena**: a carved region of 4 KB physical frames with a frame
  bitmap. A unit's pages may land in any free frames (no contiguity
  needed), so the arena never fragments and never compacts.
- **Load**: take free frames; invalidate the data cache over them; PI DMA
  the blob page by page into the frames' physical addresses (async, 5.2);
  zero bss pages; write the wired TLB entries (cacheable noncoherent, as
  kseg0); invalidate the instruction cache over the unit's virtual range;
  fill the dispatch slots. CPU cost ≈ cache-line operations only ([EST]
  ≈ 0.05 ms for 18 KB).
- **Unload**: point the unit's dispatch slots at a trap (`sys_error`
  naming the function id and unit), mark its entries invalid, free its
  frames. A stray call into an unloaded unit then takes a TLB exception
  whose bad virtual address names the unit (fixed layout), instead of
  running stale code.
- One entry maps an even/odd pair of 4 KB pages, each page to any frame:
  a unit of n pages needs ⌈n/2⌉ entries. 30 entries serve overlays; 2
  are reserved (stack guard in debug builds, spare).

Cost of mapped execution: the TLB translates in the pipeline beside the
physically tagged cache lookup, so a hit costs what a kseg0 access costs;
instruction fetch goes through the VR4300's small instruction micro-TLB,
refilled from the joint TLB in a few cycles when execution moves to
another mapped page **[to verify on hardware and in the M6 bench; ares
translates through the joint TLB (`n64/cpu/tlb.cpp`) and has no
micro-TLB model]**. GPU-visible buffers stay in contiguous kseg0 memory:
PI, RSP and RDP use physical addresses and gain nothing from the TLB.

| | TLB fixed-virtual (chosen) | relocating loader | fixed-slot overlays |
|---|---|---|---|
| load CPU (18 KB class) | cache ops + TLB writes ≈ 0.05 ms | + ≈ 600 relocations ≈ 0.2 ms; 0.3–3 KB relocation table per unit (measured own-base relocations: roach 40 … grunt 427, before `-G 0`) | cache ops only |
| memory waste | page rounding: Uplink pair 132 KB of frames for ≈ 109 KB of code (≈ 20%); no fragmentation | 16 B rounding plus a × 1.25 fragmentation margin | each slot as large as its largest unit; co-resident units need distinct slots |
| complexity | VA layout tool, wired entries, `-mlong-calls`; libdragon untouched | relocation extractor and applier, move/compaction | slot-assignment solver redone for every new class |
| debuggability | best: one fixed address per symbol, clean fault on stray calls | addresses change per load | units sharing a slot share addresses |
| limits | 30 entries (Uplink pair 18, retail [EST] ≈ 22) | arena size only | slot count and sizes |

If a campaign's entry need exceeds 30, the fallbacks are, in order: the
cook merges small classes that always occur together into one unit; then
a software refill handler for the overlay region instead of wired
entries (designed, not built unless needed).

Data arenas: the TLB is **not** extended to CPU-only data (game private
slabs, HLE slots, level state, collision chunks). Remapping individual
4 KB frames costs one entry per page pair, so a 160–240 KB region would
need 20–30 entries; with 64 KB or larger pages it saves nothing over a
contiguous carve; and those regions already avoid fragmentation (page
slabs, fixed slots). The one data use is a stack guard page in debug
builds (the stack then lives in mapped space, one entry).

**Binding rules.**

- Resident code never calls into an overlay except through the dispatch
  table (`funcs.def` ids for think/touch/use/blocked, `classes.def` class
  ops for spawn and the class methods), looked up at each call, never
  cached. Overlays call resident functions directly.
- No overlay references another overlay's symbols (`ovlx` rejects it).
- No overlay has writable static data that must outlive its residency:
  state HLSDK keeps in class statics (talk timers, squad questions) lives
  in a resident per-class statics block that saves with the game; `ovlx`
  rejects other writable statics outside a declared scratch section.
- No code address in data that outlives a frame: components, private
  data, timers, saved games and level state hold ids only (`ent_tm_t`
  already holds `fn_t` ids, 6.2). This rule holds from now on, so M6 needs
  no retrofit.

**What is resident when.** The cook already knows each map's class set
(HLE1 records + `restab`). Per map it adds the classes named by
`monstermaker` `monstertype` keys and the **spawn closure** (classes a
class can create at run time: a `SPAWNS(...)` list per class in
`classes.def`, e.g. grunt → grenade, gibs). Uplink sets (BSP entity
lumps): hldemo1 barnacle, barney, roach, gargantua, gman, headcrab,
houndeye, scientist, zombie + makers barney, gargantua, headcrab; hldemo2
barney, roach, headcrab, grunt, tripmine, zombie + makers alien slave,
grunt, scientist.

- Approach (5.4 step 1) requests B's layer **and** B's overlay set (OPEN
  priority, after AUDIO). B's records may hydrate only when its units are
  mapped; the approach window covers the DMA.
- New Game maps the start map's set at boot (blocking is allowed there),
  before the settle frames.
- A unit is **pinned** while any live entity has its class or holds one
  of its function ids (a scan of 495 × 4 `tm` ids), while a pending timer
  references it, and while its map's layer is open or approached.
  Followers carried across a seam keep their class pinned.
- The front-end maps when no map is open; the in-game save/load screen
  maps into free frames while the simulation is paused.

**Budget** (cook-proved, like the HLE region): frames = the largest union
over two simultaneously open maps of their units' pages + the save/load
screen; entries = the same union's ⌈pages/2⌉ per unit ≤ 30. Uplink
(hldemo1 ∪ hldemo2, 12 classes, gman and zombie [EST]): 30 class pages +
3 for the save screen = **132 KB of frames, 18 entries**. Retail [EST,
the 125 BSPs' class sets, unwritten classes at 8–20 KB]: worst
same-chapter pair (c3a1 + c3a1a, c2a5f + c2a5x) ≈ 103 KB in ≈ 13 units
≈ 33 pages + 3 + 10% for `-mlong-calls` ≈ 160 KB, ≈ 22 entries. Carve
**192 KB** (48 frames); the cook fails a campaign over the frames or the
entries.

Resident image: 1674 KB at M4 (PROFILE + `HL64_TEST` cart, all game code
resident; 1475 KB at M3). Moving the Uplink class units out (≈ 93 KB) and
dropping the route VM and debug commands from playable ROMs (12.5 KB)
saves ≈ 106 KB against a 192 KB arena — about 87 KB **more** memory than
all-resident for Uplink alone. The gain is the full campaign: without
overlays the ≈ 39 retail classes add [EST] ≈ 310 KB more resident code;
with them the resident image stops growing and the arena stays 192 KB
however many classes exist.

**Load cost** [EST, M6 measures]: PI DMA at 195 µs/KB — grunt (≈ 20 KB
with long calls) ≈ 4 ms of PI time, async; CPU ≈ 0.05 ms per unit; the
whole hldemo2 set (≈ 80 KB) ≈ 16 ms of DMA spread over the approach
window. Host builds link every unit statically but run the same pin
bookkeeping and dispatch-slot clearing, so a call into an unloaded unit
traps on host too.

**Verification.** ares translates mapped addresses through its TLB model
(`n64/cpu/tlb.cpp`: page masks, even/odd pairs, ASID/global). M6 adds a
test ROM (`test/m6/tlb`) run on ares and on a flashcart: wire two
entries, DMA a unit into scattered frames, call it through the dispatch
table, unmap it and expect a TLB exception with the unit's address,
repeat with 16 KB pages. Real-hardware risk is low (the TLB is standard
VR4300 hardware) but only the flashcart run proves it.

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
  `g_M(other) − g_M(own)`, as pmove's physents do; game code reading
  another entity's position (an enemy, target, squad mate, inflictor)
  converts it the same way (game/hl/util.h `OriginIn`, `CenterIn`,
  `EyePositionIn`, `BodyTargetIn`). AI perception is per layer: a monster
  neither sees (`FVisible`) nor hears (the sound list records each sound's
  layer) an entity of another layer. The packed layers are disjoint and
  sealed, so a converted trace could only end in a wall, and GoldSrc runs
  one level at a time (an OPEN seam shows B, drawing only). An entity
  changes layer only when carried across a seam, re-basing once (5.4
  step 3); twins are already in B's frame.
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
   changes (≤ 10 Hz). Dynamic lights on a scratch copy of touched meshlets.
   Night vision (D12, dlights.md 2) is a GPU mode of the frame: texel ×
   green in the same one-cycle RDP mode, no dlight or restyle work.
   OPEN-seam cross-layer draw per 5.4.
   Texture animation from the cooked sequence fields (world-surfaces.md).
3. **Movers**: visible brush entities with their MOVR meshlets under the
   entity matrix; static opaque submodels are baked into sectors (a
   `func_wall` / `func_illusionary` with a non-normal rendermode stays a
   mover in the translucent pass, w64-format.md).
4. **Studio**: STB2 v4 bone-local meshlets recorded once per model into an
   rspq block (packing budget 2 ms/frame); per entity per frame the bone
   matrices from `game_studio_setup_bones` (D3) into a double-buffered
   pool and one block replay. Lighting: probe (base + one style term,
   lightstyles.md 4) + dlights (night vision: the NVG colour instead).
   View model: chrome runs CPU-skinned (chrome needs bone-space normals),
   the other runs replayed from their blocks with
   the pose's bone matrices; posed at the eye with the view's angles (pitch
   negated as a model's, HLSDK V_CalcGunAngle) and drawn last in the
   front 3/10 of the depth range (GoldSrc's view model depth range): it
   turns with the view and never goes into a wall. STEP entities
   interpolated between think steps (physics-movetypes.md 7), as
   GoldSrc's client does.
5. **Particles**: one pool, TinyPX ucode, one call, s16 view-relative.
6. **Translucent**: render modes/fx per render-fx.md; back to front; depth
   test without write; additive as unit colour × brightness through the
   alpha blender (render-fx.md 2a: the RDP adder wraps; ramp TLUTs fitted
   by the cook, bound by the cart).
7. **Decals**: on cooked faces of resident sectors/movers, decal Z, I4
   texels from the cooked decal sheet (render-fx.md 4).
8. **HUD/2D**: ui8 sheets, numbers, crosshair, damage indicators, text
   messages, fades; pause menu and debug overlay last.

Vertex lighting replaces GoldSrc's lightmaps (SA2 decision 1, accepted
there): the closest the cart can afford to Half-Life's look; recorded as
D6 (iv).

---

## 10. Audio

libdragon mixer, 16 voices, 22050 Hz; wav64 version 10 (mono VADPCM,
decoded by the mixer's RSP ucode) streamed from the cart
(`WAV64_STREAMING_FULL`); ≤ 10 open waves with LRU close (never under a
voice); wave reads in the `AUDIO` I/O class. The frame's pump is
`mixer_try_play`, which queues the mixer's RSP rounds without waiting for
them unless the AI is down to its last buffer (160 ms of AI buffers).
GoldSrc channel semantics, attenuation and stereo model, loops,
sentences: specs/sound.md. Sounds are
`res_t`; sentences are cooked (word `res_t` lists with VOX modifiers),
nothing is parsed at run time. Positions are global, so both open layers
spatialise without conversion.

## 11. Input

Pad → `usercmd` (stick turn + forward/back, C-buttons strafe/look, Z
attack, R attack2, A jump, B use, L duck, D-down reload; D-up night vision
(the flashlight impulse, D12), D-left/D-right previous/next weapon as one
impulse per press; Start opens the pause menu (13); one table in
`engine/in/in.c`, documented in `docs/controls.md`);
absolute stick turning scales by `dt`. The route VM injects held buttons
and impulses into the same command; `ARES_TEST_INPUT` injects at the pad.

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

`engine/ui/menu.c` (its own unit so it can move into an overlay, 7.4; it
keeps no code address in anything saved), drawn through draw2d in the
style of the 1998 Half-Life menu: black background, the player's
`media/logo.avi` banner at the top, a column of dark buttons with orange
labels in the player's creditsfont, values to their right, a description
line for the highlighted button, the menu sounds of the player's
`sound/common` (`res.def` Menu block).

- **Banner**: `tools/cook/bake_logo.py` decodes logo.avi (MS RLE, 8-bit
  grey, 640×100, 24 fps) with ffmpeg into `gfx/logo.lg4`: 110 frames,
  320×50 I4, dithered to 16 greys (880 KB of ROM); without ffmpeg or the
  file, one still frame of `gfx/lambda.bmp` (the cook log says which).
  Two 8000 B buffers in the HUD region: one drawn while the next frame is
  read from the cart at prefetch priority.
- **Screens**: the title (playable builds boot to it; New Game → Easy /
  Medium / Difficult into `settings.skill`, Hazard Course =
  `wld_training_map()`, Load Game, Configuration); the pause menu (Start in
  play: Resume Game, Save Game, Load Game, Configuration, Quit to Title with
  a confirmation). Load Game lists every slot of `save_slots()` (14): the
  autosave, the Flash slots, the Controller Pak's notes; Save Game the
  manual ones; a dimmed Controller Pak row gives `save_pak_status()` when
  no Pak slot can show (none, full, a Rumble Pak, damaged). Lists longer
  than five buttons scroll with the highlight.
- **Pause**: the simulation is skipped (no game step, route, AI or
  physics; eng.c `menu_only_frame` keeps I/O, the sound pump and the 2D);
  the game's sounds pause: loops restart at their loop start, a sentence
  goes on with its next word, an interrupted one-shot ends (a VADPCM wave
  only seeks to its start or loop start). Buttons held as a menu closes
  are ignored until released.
- **Configuration**: brightness (0..100), sound volume (`vol_sfx`), look
  speed (`look_sens`, 25..200 %), invert look; applied as they change,
  stored through `settings_store` when the screen is left, loaded at boot
  in playable builds. Brightness lifts the black level: `gpu_view_t.lift`
  = `SETTINGS_BRIGHT_LIFT` (0..48 of 255) is added as ENV by the world's,
  the TransAlpha brushes' and the opaque / TransTexture models' combiner
  (texel × shade or PRIM + ENV), the same one-cycle mode, not under night
  vision; additive passes and the HUD are not lifted. Measured on ares-host
  ares (scene.sh `--cmd "@2 set brightness N"`): hl1-start 33.4 / 33.5 ms
  avg / p90 at 0, 50 and 100; hl2-yard 51.5 / 77.0, 51.5 / 76.6,
  51.5 / 77.0: no cost. The Configuration screen previews it on a grey
  ramp. No music volume: the engine has no music path.
- **Layout**: framebuffer pixels; the VI scans out all 320×240 inside
  libdragon's CRT-safe area (`VI_CRT_MARGIN`; the cart logs `vi: ...
  columns 0..319, rows 0..239 shown`), so a string is on screen when it is
  inside the framebuffer. `layout_check` measures every label, value,
  description and note at boot (`menu: layout ok`, else each string that
  does not fit). ares' raw shots read only the first 216 rows (its VI
  output height), which cuts off the description line (rows 214..232).
- **Quit to Title** and, in playable builds, an end of section: the
  session ends as an end of section does (eng.c `session_end`; every voice
  stops and every wave closes), then `studio_flush` empties the model
  pools. At the settled title eng.c `title_settle` compares 13 fields
  (heap blocks / KB, entities, game private data, HLE / dehydrate, the
  world pools, layer directories, studio pool, sound voices and region)
  with boot. The first session leaves one-time allocations the first 3D
  frame makes lazily (cart only; heap map at the first quit, symbolised):
  plat/n64/gpu.c's tiny3d viewport matrices (256 B), matrix ring (12 KB)
  and projections (256 B), gpu_fx.c's vertex ring (148 KB), point buffer
  (12 KB), scale matrix (64 B) and tpx matrix stack (512 B): 7 blocks,
  173 KB. The first quit's
  title is then the reference: every later one must equal it (`mem: title
  same as first quit`). The host has none of them: its first quit equals
  boot.
- **Tests**: `test/m6/menu_host.sh` (a suite of `test/run/m5.sh`): the
  layout, the pause freezing game state and clock, Configuration stored,
  Quit to Title back to the boot title, New Game after it equal to one
  from boot. `test/run/menu.sh` (ares, by hand): a playable ROM walked by
  the pad through every screen and two sessions.

## 14. Save and level state

Level state is the dehydrate store (6.4). A saved game (D4, `engine/save/`,
`include/hl64/save.h`) is a 128 B header (magic, format 4, layout id,
logical slot, sequence, its sectors, sizes, CRC-32 of the image, display
info, the open layers with their directory CRCs), on the flash a 384 B
settings block (its own CRC), and a compressed image of the sections every
engine module writes (`engine/save/svmod.h`, in `save.c` order: clock,
engine frame, rng, settings; records with HLE slots, timers, uses, frozen
maps and the dehydrate store; entity components, sets, grid and private
blocks; contents brushes, player, input, conveyors, render fades, decals,
seam state, lightstyles, looping sounds), then the game's globals
(`game/hl/savegame.c`) and, in test builds, the route VM's position.
Entity rows are stored as differences from their cooked records; empty
slots, capacity-sized tables, grid query stamps and timing stats are not
saved. Coder (`engine/save/sv.c`): LZ with 4-byte hash chains (depth 16,
8 KB window) and a price-driven lazy parse, then an adaptive binary range
coder with LZMA-style token models (literals, matches, 4 repeat
distances), integer-only so host and cart write the same bytes; 10.7 %
smaller than the previous LZ + static Huffman coder over the Uplink and
Hazard routes' autosaves (late Uplink 31795 → 28687 B), host CPU ×1.8 to
code and ×2.0 to decode, still in 8 ms steps; 3.3 KB of model tables in
its statics, the work area unchanged (24 KB; `test/save/svbench.c`
measures it on raw state dumps).

**Placed decals** (`engine/render/decal.c`) are saved as their
placements, not their pieces: per live pool entry its index, order,
mover, layer, map, chunk, texture, the point and normal in the model's
frame (quantized: quarter units, normal × 16384) and the winding (28 B),
plus the pending placements and each layer slot's map. A load restores
them at their own pool indices; each is rebuilt (`walk_chunk` on its one
chunk) once that chunk is resident and its sheet read, under its own work
cap (175 units a frame), never the placement budget, so the game's
placements and infodecal's retries go as in the run that saved. A full
pool (96) is 2702 B → ≈ 850 B coded; after the late-Uplink load all 96
rebuild within 100 frames (host).

**Media** (user decisions 2026-11-15):

- **FlashRAM** 128 KB, 16 KB sectors, all eight a pool shared by the
  autosave, 2 manual slots and the settings. A save takes 1 or 2 whole
  sectors (any two: its header names them), so its image is at most
  32256 B (`SAVE_IMAGE_MAX`). A logical slot is its newest copy whose
  header and image check out. A new save goes to sectors no slot's
  current copy holds, so the copy it replaces stays good until the new
  one is complete; of 2 sectors the second is written first, the header's
  last. Settings: every Flash save carries the settings block of its
  time; `settings_store` writes a settings-only copy (header + block, one
  sector) to a sector no slot holds, never over the current settings
  copy. The newest good settings block of any build is the settings
  (`settings_load`). **Worst case** (checked by `test/save/flashcut.sh`):
  three 2-sector saves (6 sectors) and a settings copy (1): a new
  2-sector save takes the free sector and, as its header's sector, the
  settings copy's (it carries the settings itself): the other slots
  hold 4 sectors and its own old copy 2, so 2 are always left and no save
  is ever blocked or written over; a settings copy always finds 1 (6
  held by the slots, 1 by the current settings copy). A power cut while
  that save writes its header's sector loses only the latest settings
  change (the settings fall back to the newest save's), never a save.
  The old layout (settings in sector 0, saves in a 7-sector pool) could
  not do this: 2 + 2 + 2 + 2 > 7.
- **Controller Pak** (controller 1): one save per note
  (`HL64`/`HL`/`HL64SAVE.<id>`, header + image, no settings block), up to
  4 entries listed after the Flash slots. The image is written first and
  the header last; the replaced note is deleted after the new one is
  closed. With no room for both notes the old note is deleted first and
  the new one created in its place: from that delete until the header
  write (one frame per 256 B, ≤ 121 frames for the largest image) a power
  cut or a Pak pull loses the slot (the headerless note lists as corrupt
  and can be written over); a delete that succeeds followed by a failed
  create loses it at once (logged `no room`). Any Pak failure, and any
  change of accessory between frames, forces an unmount, a remount and a
  rescan; an operation whose Pak changed fails. A note's image is at most
  30720 B (`SAVE_PAK_IMAGE_MAX`); a bigger one fails before the Pak is
  touched with `SAVE_ERR_PAK_SIZE` (the menu: "Save too large for
  Controller Pak."; `SAVE_ERR_SIZE`: too large for any save).
- Measured (host, c19 cook, state → image): hl2-yard 600 28040 → 10137 B,
  hazard-seam 12469 → 4454 B, seam-2to3 39563 → 15402 B,
  hl2-return-battle 37604 → 14066 B; the routes' autosaves: Uplink 7, the
  largest at the hldemo2 → hldemo1 return 70418 → 29573 B (2 sectors; it
  also fits a Pak note), Hazard 7, largest 38607 → 13275 B. The save
  region (`HL64_SAVE_KB` 160) stages up to 104 KB of state.

**Save** (`save_write`, `save_autosave`): at the next frame boundary
(`save_frame`, first in `eng_frame`) what is in flight settles (paused
monster thinks finish, layer / HLE slot opens complete, precache holds
land; the same settle runs in every load of it), the snapshot frame copies
the state into the save region and writes the header's description of it
(layers, map, time), then the coder runs in 8 ms steps over the following
frames and the flash writer programs it (4 ms per frame) or the Pak writer
writes 256 B per frame (≈ 6 ms on ares, 0.72 ms per 32 B block). The
directory is updated from the read-back of every write. A manual save
mutes the sound (`snd_mute`: gains 0, no sound's state changes) and holds
10 shown frames first, so the 160 ms output queue is silence when the
long snapshot frame lets it run dry; a layer open in flight finishes in
held frames before the snapshot (a synchronous open in a running game
cannot wait for its pools' fenced frees). Staging: entity rows from
`ent_s.touched` down (every slot above it is as boot left it), one pass;
delta rows with word loads; private blocks with an owner map and each
class's converted reference kept in the coder's idle work area
(`sv_work_area`). **Autosave**: `trigger_autosave` and every finished
seam crossing ask for one; it is taken at the first frame boundary that
is quiet (a player, no seam window or crossing) and settled (no paused
or queued think, no open in flight: the autosave changes nothing and
holds one frame, `SAVE_HOLD_QUIET`, which shows nothing new); a load
drops the request.

**Load** (`save_load`, works from the title): the sound is muted, the
image read and checked first (a bad one leaves the session alone),
decoded in 8 ms steps, then (at least 10 frames after the mute) the
session is dropped as at an end section, the world's fenced frees retire
over 3 held frames, the saved layers reopen in their saved slots and
every section is read back: entities into their own slots with their
serials, private blocks at their own handles, HLE slots at their own
blocks, the clock (before the engine frame) and the random stream exactly.
No spawn function runs; the game resumes at the next frame (placed decals
rebuild over the next frames as their chunks land). The menu shows
"Saving..." / "Loading..." over the held frames (`SAVE_HOLD`).

**Gate** (`test/save/determinism.sh [--ares] [--pak] SCENE:T:N[:report]`,
run by `test/run/m5.sh`): run S saves at T and quits; A saves at T and
goes on; B is a fresh boot (host process / ares launch) with S's FlashRAM
or Pak image that loads at frame 1; C never saves. (a) Restore: B's load
stages the restored game again at once (test builds' `restore_check`)
and it must equal S's saved state byte for byte, every scene. (b)
Continuation: A's and B's save streams at T+N equal byte for byte and
their `stats` player / entity lines at T+1 and T+N equal — required for
hl2-yard (600:150), hazard-seam (85:150) and seam-2to3 (810:90). A
manual save on the cart takes 28–72 frames (the coder's 8 ms steps), and
an autosave asked meanwhile waits for it, so in A (which saved at T) the
autosaves come later than in B: T+N lies after them and after A's own
save (hazard-seam: after the return crossing's autosave; seam-2to3:
after the switch, room for the 72-frame save at T); on the cart the T+N
save may queue behind an autosave, so ares runs quit 90 frames later.
Earlier seam-2to3 windows (before the switch) meet game rules that
follow residency, warm in A, cold
after B's load: infodecal retries change free times, the hitbox pose
memo forgets entries whose animation is not resident. Reported only for
hl2-return-battle (`:report`), where a map opens inside the window and
its completion frame follows the render pool's state, so hydration and
AI-think drops land at other frames. Cost: every autosave ok and every
frame it holds or step it takes ≤ 67 ms (gated); a manual save's and a
load's frames and whole time are reported (the game is paused behind the
menu, the sound muted). `test/save/pakfail.sh`: the Pak's no-room,
failed-transfer and too-large (`SAVE_ERR_PAK_SIZE`, the cap lowered by
`HL64_HOST_PAK_IMAGE_MAX`) paths. `test/save/flashcut.sh`: the pool's
worst case above (every save forced to 2 sectors by
`HL64_HOST_SAVE_SECTORS`), then a manual save and an autosave cut by a
power cut in the middle of each of their 130 flash operations
(`HL64_HOST_FLASH_CUT`, `plat/host/flash_host.c`): after every cut a
fresh boot lists all three slots good (the cut slot's old or new copy)
and a settings block. Known gap: test builds' `restore_check` stages the
re-save beside the loaded state, so states over half the staging room
(52 KB, the late Uplink route) log `no room` instead of comparing; the
gate scenes are ≤ 40 KB.

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
  `WAIT_GONE` is met when every record of the name is killed or its live
  entity is `SOLID_NOT` (GoldSrc keeps an `ambient_generic` sharing a
  breakable's name; route-bytecode.md). `SETPOS` is a test-build teleport
  (refused by builds without `HL64_TEST`); its 30-frame window counts
  blocked frames separately (`blk1ms_tele=`, not `blk1ms=`).
- **Stats records**: fixed struct (entities live/peak, records pending,
  pools used/peak, heap free/largest, fps + pacing histogram, DMA
  p50/p99, pop-in, blocked I/O, position, time) printed as one text line
  per `STATS` op; field names follow the old tree's where comparable so
  old/new logs compare.
- **Test-build determinism (K10: host = cart)**: in `HL64_TEST` builds
  no simulation-affecting decision depends on PI timing, pump counts or
  wall time, so the host's PI model and the cart's DMA give identical
  route ops and entity records. Budgets count work (records per frame,
  `REC_BUDGET_HOST`). Transcendentals (sin, cos, tan, atan, atan2, asin,
  acos; float and double) come from `engine/core/hlmath.c`, built from
  IEEE basic operations without `-ffast-math`, never from libm: glibc
  and newlib round them differently (m4 stats 84: a yawed studio box 1
  ulp apart). Every engine/, game/ and include/ call site uses them,
  renderer and sound included; `test/m4/hlmath.py` fails on a direct
  libm transcendental there and runs the accuracy driver (float results
  correctly rounded on its arguments, double within 1-2.5 ulp). No
  `-ffast-math` in any simulation directory. Random draws are sequenced
  (no draw inside an argument list or initializer whose evaluation order
  C leaves unspecified), and the decal cap counts work units, not wall
  time (infodecal retries differed host/cart). I/O
  completes in frames (engine/io, 5.2): at
  `io_frame_begin` the scheduler plans the frame's pieces from the queue
  alone (priority order, the frame's byte budget, at most
  `IO_PLAN_UNITS` requests and 16 KB units), the PI completion interrupt
  chains them through the frame (host: they move at its first io call),
  and their completions (request done, callbacks,
  chunk RESIDENT, studio info / anim landed) are published only at the
  next `io_frame_begin`, before anything else of that frame; a planned
  piece not moved by then is read there (late: `io: frame N late`,
  stats `stream: late=frames/pieces`, blocked I/O; the M4 gate fails on
  it). Synchronous reads (boot, New Game, loads, audio, `io_finish`,
  `wld_chunk_sync`) keep their immediate semantics. Every residency-
  driven decision is then the same frame on both: the map-open gate, the
  precache hold, seam pins, studio info / anim availability and the
  hitbox source (the box while an animation is not resident). A map
  opened by a seam has one open gate after its layer and HLE reads: its
  records hydrate once its precache infos and spawn windows are
  resident, bounded (`REC_PRECACHE_FRAMES`; an expiry is logged, counted
  in `stats N rec: expired=` and fails the M4 gate); while it waits on
  spawn windows they are named at the pin class (after the want measure),
  so they may evict prefetch chunks of a full collision pool, and no
  spawner box is named. Seam pins B as soon
  as its layer is ready. Playable builds publish a request as soon as its
  last piece lands (at the next io call), plan again once a plan has
  landed, and chain across frame boundaries. Triage:
  `entsdump 1` makes every `stats` record also log one line per live
  entity (`stats N ent E cls= gid= tn= id= cv= mot= ph= tm= rnd=`, part
  hashes + render fields); the first differing line of two runs names
  the entity and the component.
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
| M1 | io scheduler, layer open, sector renderer, movers static, textures + animation, lightstyles, free camera; cook ranges rewritten | 3 hldemo1 poses: ares SSIM ≥ 0.95 vs the old cart (world only; both are the same cooked world, so the old tree is a valid oracle here; dithering off, the old cart's undithered output), and dithering on: 3.6 per channel brighter on average (the truncation residual restored, `test/m1/dither.py`); recorder counts = cook prediction; world CPU ≤ 2 ms on the dome pose; W1–W5 |
| M2 | COLL residency, traces + hitboxes, spatial grid, pmove host + pm_shared, input, view | trace-and-hulls.md tests on GoldSrc probe fixtures; area-links A1–A6; pmove PM1–PM7 against probe bot runs; route `m2-walk` PASS |
| M3 | components, active sets, think heap, records, hydration (one layer), movetypes, resources, sound, studio, translucent/decals/sprites, HUD, game/ retargeted | compiled Uplink route segment A to the elevator `+use`, `WAIT_MOVED startele1`, changelevel fire logged; physics/think/sound/lightstyle/dlight/render-fx tests pass on their fixtures |
| M4 | streaming crossings: CWG3, seams, twins, dehydrate/rehydrate, timers, async residency with change-driven recompute | route through hldemo1→2→3: PASS, 0 forced switches on host, `budget_over=0`, `open_fail=0`, 0 frames > 1 ms blocked I/O, pop-in only in teleport windows, entities continuous across seams |
| M5 | full Uplink route (converted `uplink_route.cfg`) + Hazard routes | PASS on host and ares at the same checkpoints; any `WAIT_NEAR` radius widened versus the old route is listed with its GoldSrc-behaviour reason |
| M6 | performance; code residency (7.4: TLB-mapped overlays, `tools/ovlx`, cook frame/entry proof) | **Frame rate (user acceptance, 4.2)** on the Uplink and Hazard routes in ares: frames > 50 ms ≤ 2% of the route's frames and never more than 5 in a row outside seam and teleport windows; frames > 67 ms = 0 outside teleport / route-`SETPOS` windows (seam windows included); the frame-time distribution reported per map (zones named for every frame > 50 ms). ares median fps ≥ the old cart's at every stats step (same pacing); 4.1 and 4.2 [EST] rows replaced by measurements; flashcart run if available. Overlays: `test/m6/tlb` passes on ares (and the flashcart when available); on the Uplink route the resident code size, arena frame peak and TLB entry peak are reported and within 7.4's budget (≤ 192 KB, ≤ 30 entries), 0 frames > 1 ms blocked on overlay I/O, 0 calls into unmapped units (trap and TLB-exception count 0). **Starting point (M4, ares, main `f913147`):** route `m4` frame avg 46.6 ms, p90 93 ms; 245–384 ms frames from `monster_human_grunt` AI (`Mon_RunAI` walk-move traces, node-graph cover search, route triangulation) in hldemo2/3; cart image 1674 KB vs the 900 KB budget (4.1, 4.2) |

---

## 18. Build and conventions

### 18.1 Build

`Makefile` includes `$(N64_INST)/include/n64.mk` and `t3d.mk`; `N64_INST`
= `~/sdks/n64-sdk-hl64-<rev12>`: libdragon from our fork
(`~/src/libdragon-hl64`, branch `hl64`: DragonMinded `preview` plus the
banked RDRAM overlay — `surface.c` / `display.c` / `rba_ld` /
`rdram_bank`, FB / Z / stream in separate banks — the debugcpp debloat,
and the PI dispatch fix: preview's DMA queue accounts a completion before
the `register_PI_handler` handlers run, and hands them the completions it
polls itself, so engine/io's chain, launched from that handler, keeps
one piece in the queue, landed by its ticket) at the Makefile's
`LIBDRAGON_REV`, its tools, and tiny3d at
`TINY3D_REV`, built by `plat/n64/build_sdk.sh`; `make sdk-check` compares
the prefix's `MANIFEST.json` with both pins. Toolchain
`~/sdks/mips64-toolchain`, out-of-tree `BUILD_DIR` (scratch on
`/scratch`). Targets: `rom`, `host` (`HOST_M32=1` default), `cook`
(`HL64_DATA` → `HL64_CONTENT`), `routes`, `test-host`, `test-ares`,
`provcheck`. `-std=c99 -O2 -Wall -Wextra -Werror` for `engine/` and
`plat/`. Variants: `PROFILE=1`, `HL64_TEST=1` only. ROM ≤ 64 MiB
(flashcart limit; Uplink + Hazard ≈ 58 MiB in the old tree). Cooked
sounds are keyed on the `audioconv64` binary, so a new SDK reconverts
them (wav64 versions are not interchangeable).

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
| D4 | Saved games (after M5) | none; FlashRAM 128 KB; SRAM 32 KB; Controller Pak 32 KB | **accepted 2026-11-15**: FlashRAM sector 0 settings + 7-sector pool for the autosave and 2 manual slots, Controller Pak notes for extra manual slots; autosave at `trigger_autosave` and after seam crossings. Measured images 4.9–16 KB (≤ 30 KB cap, one Pak note each); 14 |
| D5 | Host renderer output (M1) | draw-list recorder only; plus a host software rasteriser | recorder; rasteriser only if ares turnaround blocks iteration |
| D6 | Behaviour target | **settled by the user: GoldSrc as observed.** Remaining sub-decisions: (i) frame-time clamp 0.1 s (stability on the cart) vs GoldSrc's 0.25 s; (ii) hitbox traces for line traces vs bbox only (cost ≈ 0.1 ms on firing frames); (iii) model lighting following animated lightstyles via the probe style term (cook format v3) vs static; (iv) vertex lighting instead of lightmaps (SA2 decision 1) | (i) 0.1 s; (ii) hitboxes; (iii) style term; (iv) keep vertex lighting |
| D7 | Console requirement | Expansion Pak required; also 4 MB | Expansion Pak |
| D8 | Imported file history | copy with a commit note naming the audit entry; subtree import | copy with note |
| D9 | Admit QUAKE-INTERFACE files (blocks M0 imports) | (a) amend PROVENANCE.md: importable when the same commit replaces every Quake binding the audit lists and rewrites its flagged ranges; provcheck verifies; (b) CLEAN-only | (a), now for fewer files: revision 2 already rewrites the route runner, console, area links and resource tables; (a) still saves ≈ 15k lines of streaming, seam, record, level-state, sound, studio-residency and sector/studio renderer code whose algorithms are ours |
| D10 | GoldSrc oracle (blocks M2 fixtures) | (a) run Valve's Linux dedicated server (steamcmd app 90, anonymous) with a HLSDK-built probe game DLL and the player's Uplink maps on this machine, plus retail client captures when the user's Steam login is available; (b) no GoldSrc runs: specs from HLSDK and documentation only, old tree as fallback oracle | (a): server-side behaviour (traces, touches, physics, think timing, pmove via bots) becomes measured ground truth; black-box observation is allowed by PROVENANCE |
| D11 | Code residency (M6; user direction: only active code resident, no libdragon DSOs) | (a) TLB-mapped fixed-virtual overlays in a 4 KB-frame arena, wired entries; (b) relocating loader into a first-fit arena; (c) fixed-slot overlays | (a): no relocation, no fragmentation, fixed addresses for debugging, clean faults; limit 30 entries (Uplink 18, retail [EST] ≈ 22), fallbacks in 7.4 |
| D12 | Flashlight look (user decision 2026-11-15, **accepted**; a deliberate departure from GoldSrc) | (a) GoldSrc's flashlight: an 80 u light at the end of the view trace, drawn as a decal-Z pool pass over the world's base triangles (world zone 9.4–12.8 ms with it on vs 1.8–4.5 ms off on ares; 14 KB scratch); (b) Opposing Force-style night-vision goggle: the same `impulse 100` toggle, battery, HUD icon and sound, the frame drawn texel × bright green + green floor with distance fog to black in the world's one-cycle RDP mode, no light | (b): no extra pass or geometry; measured on ares within 0.3 ms of the lit frame (dlights.md 2, D6); the flashlight pool, its trace and its scratch are gone (dlight region 48 → 32 KB). Spec: dlights.md 2 |
