# hl64 engine architecture

Status: design (EngineSpec, 2026-11-13). Binding rules: [PROVENANCE.md](PROVENANCE.md).
Behaviour specs: [specs/](specs/README.md). Reuse verdicts: [provenance-audit.md](provenance-audit.md) (ProvAudit).

hl64 is a single-player Nintendo 64 engine for Half-Life content. It runs one
continuous streamed world built from the cooked formats the quakegeneric N64
work already produces (`.w64`, CWG3, HLE1, STB2 v4, wav64, ui8), within the
budgets measured there (`n64/docs/stream-arch-v2.md` in the old tree, "SA2"
below). Game logic is our C reimplementation of the HLSDK game (`game/`,
HLSDK licence), re-targeted from the Quake hooks it uses today onto the
engine API in this document.

The old tree (`~/src/ports/quakegeneric`, branch `n64-port`) is the reference
implementation and test oracle only. Implementing agents read this document,
`docs/specs/`, `docs/formats/` and the allowed sources; they never open the
old tree's forbidden files (PROVENANCE.md). Where this document names an old
tree file, it is to tell a dirty-side agent where to capture oracle data, not
to tell an implementer where to look.

Evidence labels: **measured** (number from an old-tree run log or cook
report, source cited), **[INFERENCE]** (reasoned estimate, to be measured by
the milestone that owns it).

---

## 1. Scope and non-goals

In scope:

- Single player, one process, one thread (plus interrupt callbacks). No
  client/server split, no loopback, no network protocol, no demo format,
  no entity_state delta encoding. The renderer and sound read entity
  records directly after the simulation step of the same frame.
- One continuous world per campaign component: every map of the CWG3 graph
  sits at its cooked global offset `g_M`; seams are portals (translation
  `T`); nothing ever "loads a level" after New Game.
- N64 with the Expansion Pak (8 MB). A host build (Linux, `-m32` and LP64)
  enforces the same limits for tests.
- Content: Half-Life: Uplink (hldemo1→2→3→2→1) and the Hazard Course
  (`t0a0*`) as cooked by the existing tools. Full HL1 campaign is the
  long-term aim; nothing here may assume a map count or map names.

Not in scope: multiplayer, mods/plugins loading DLLs, the GoldSrc client
DLL boundary, raw BSP loading on the console, software rendering, Android/
SDL builds (the old tree keeps those), 4 MB consoles.

---

## 2. Top-level structure

```
hl64/
  LICENSE                 MIT (everything except game/ and tools/hlsdk/)
  Makefile                ROM (libdragon n64.mk) and host targets
  include/hl64/           MIT public headers: the engine API the game sees
    limits.h              every cap and pool size (one file, cart = host)
    types.h               vec3, handles (ent_h, str_h, mdl_h, snd_h), fixed ints
    eng.h                 engine services for game/ (section 7)
    game.h                game exports the engine calls (section 7)
    ent.h                 the entity core record (section 6)
    pmove.h               the player-move host contract (specs/pmove-host.md)
  engine/                 MIT
    core/                 boot, frame loop, clock, log, cvar, cmd, console, rng
    mem/                  boot carve, arenas, first-fit pools, slabs
    io/                   cart file table, async DMA scheduler
    world/                layers (.w64), campaign (CWG3), portals, vis, stream
    coll/                 hull walks, traces, area tree, entity clipping
    phys/                 movetypes, pushers, step moves, pmove host
    ent/                  entity table, records (HLE1), hydration, strings,
                          precache tables, dehydrate store, timers
    snd/                  channels, spatialisation, sentences, wav64 voices
    render/               frame, sectors, movers, studio, translucent, sprites,
                          particles, decals, beams, lights, 2D
    ui/                   menu, console overlay, text messages
    save/                 save/load (decision D4)
    test/                 route runner, profiler zones, stats lines
  plat/                   MIT; the only code that includes libdragon
    n64/                  main, VI pacing, PI DMA, pad, audio out, ares harness
    host/                 main, pread backend + PI cost model, scripted pad,
                          null audio, draw-list recorder
  game/                   HLSDK licence (game/LICENSE-HLSDK + Valve notice)
    hl/                   the C reimplementation (from old source/hl, re-targeted)
    pm/                   HLSDK pm_shared (decision D2)
    hud/                  HUD, view (client-DLL side logic)
    studio/               bone setup / animation from HLSDK StudioModelRenderer (D3)
  tools/                  MIT cook tools (imported per audit)
    hlsdk/                HLSDK-licensed tool code that needs SDK headers (studio)
  test/
    routes/               uplink.cfg, hazard-*.cfg, milestone routes
    fixtures/             oracle captures (data only, see specs/README.md)
    run/                  host + ares runner scripts
  docs/
```

Dependency rule: `game/` includes only `include/hl64/*.h` and its own
headers. `engine/` never includes `game/` headers; it calls the game through
`include/hl64/game.h`. `plat/` is the only directory that includes
`libdragon.h`/`t3d.h`; `engine/render` reaches tiny3d/rdpq through a thin
`plat/n64/gpu.h` so the host can link a recorder instead.

---

## 3. Platform, boot and frame loop

### 3.1 Boot (N64)

1. libdragon init: debug (ISViewer), DFS (`rom:/`), timer, controller,
   audio (22050 Hz), RSP queue, tiny3d.
2. Display: 320×240, 16 bpp, two framebuffers, 16 bpp Z, banked libdragon
   (FB/Z in separate RDRAM banks, as the old tree's
   `libdragon-banked-bc24d3d5a7d2`).
3. `mem_boot_carve`: every fixed pool of section 4 allocated once, in a fixed
   order, before any other allocation. The remaining heap is measured and
   printed (`mem: carve … heap_free=…`); boot fails if it is below
   `HL64_HEAP_MIN_KB`.
4. tiny3d self-check of the cooked strip index encoding (one probe index
   converted with the running tiny3d and compared to the `.w64` header's
   `idx_stride/idx_base`; mismatch → in-place re-encode path, SA2 §3.2).
5. Content check: `campaign.cwg` present and valid, every `.w64` named by
   it present with matching header size and directory CRC. Missing player
   content → a text screen explaining how to cook it; never a crash.
6. Console, cvars, commands registered; `autoexec` (ROM-packed text) run.
7. Menu (or `+map`/route boot from the test config).

### 3.2 Frame loop

One iteration per presented frame. Order is fixed (the specs assume it):

| step | module | notes |
|---|---|---|
| 1 | `plat` | read VI count; `dt` = elapsed VIs / VI rate (60 NTSC, 50 PAL), clamped to `[1/60, 0.1]` s; fixed `1/30` when `host_fixed_dt` is set (tests) |
| 2 | `in` | poll pads → `usercmd` (buttons, view angle deltas, move axes) |
| 3 | `io` | pump: retire finished DMA piece, launch next |
| 4 | `game` | `game_start_frame` (globals, timer queue: delayed fires, multi_manager steps) |
| 5 | `phys` | player: `game_player_prethink` → pmove → touch triggers → `game_player_postthink` |
| 6 | `phys` | every other live entity in index order: think + movetype step (specs/physics-movetypes.md, specs/think-and-time.md) |
| 7 | `world` | campaign: approach/arm/switch tests, portal crossing, layer open/close |
| 8 | `ent` | hydration pump / release under the entity budget (2 ms cart) |
| 9 | `world` | residency targets → DMA queue (open, pin, prefetch) |
| 10 | `snd` | listener, channel volumes, sentence words, loops, mixer pump |
| 11 | `render` | view (game `game_calc_view`), world, movers, studio, particles, translucent, decals, view model, HUD, console/menu |
| 12 | `plat` | submit; present gated on VI (3.3) |
| 13 | `test` | route runner step, PROFILE zone roll-up, stats lines |

`io` is also pumped between steps 6/7, 9/10 and after the world draw (five
points per frame as in the old tree; any piece landing is retired at the next
pump).

### 3.3 Present pacing (30 fps VI cap)

A finished frame is shown no earlier than `HL64_PRESENT_VBL` (2) vblanks
after the previous show, and otherwise on the first vblank after the RDP
finishes it; frame times therefore sit on 33/50/67 ms tiers (NTSC). The RDP
done callback shows the frame when the gate is open, else parks it; the
vblank handler shows a parked frame when the gate opens. With two
framebuffers the next frame's simulation runs while a finished frame waits.
libdragon's `display_set_fps_limit` is not used (it turns any frame over
33 ms into 67 ms). The pacing histogram (shows per interval in vblanks) is
part of every `stats` line. In the old `r64_vid.c` the pacing, palette and
TLUT code is ours and the `VID_Init` setup is Quake-derived (audit), so the
file is split (16.1); this paragraph is the spec for the pacing.

### 3.4 Host build

`make host` builds `build-host/hl64-host` from the same `engine/` and `game/`
sources with `plat/host`: files read with `pread` and a modelled PI cost
(195 µs/KB = 5.25 MB/s, counted against the frame's budget), a scripted pad
(route runner and `ARES_TEST_INPUT`-style files), null audio that still runs
channel allocation and spatialisation, and a draw-list recorder in place of
the GPU (meshlets, binds, triangles, studio draws per frame; optional PPM of
a flat-shaded raster is decision D5). The host carves the same pools with the
same sizes from one arena whose size is the cart's post-carve heap; any
allocation failure, table overflow or "not found" that would fail on the
cart fails on the host. `-m32` is the parity default; LP64 builds use the
same caps (records hold handles, not pointers, so their sizes match —
section 6).

---

## 4. Memory

### 4.1 Rules

- No allocation can fail on the steady path. Everything sized by
  `include/hl64/limits.h`, carved at boot, and proved by the cook where it
  depends on content (SA2 principle 2: the cook fails when a leaf's or
  seam's open set exceeds a pool).
- Three allocator kinds only: **arena** (bump, reset as a whole: per-frame
  scratch, boot tables), **first-fit pool** (16 B granularity, compaction
  that rewrites one owner pointer per block, frame fences for GPU-visible
  blocks; SA2 §4.3 as built in the old `q64_pool.c`), **slab** (fixed-size
  slots with a free list: entity records, game private data size classes,
  channels, decals, particles).
- `malloc` is not used by `engine/` or `game/` after boot. libdragon's own
  allocations (rspq, display) happen during boot.
- GPU-visible memory (vertex/index data, TLUT/texels, matrices) is freed
  only behind an rspq syncpoint two frames old.
- DMA destinations own whole 16 B cache lines while a piece is in flight
  (start 16-aligned, length padded to 16). A CPU access to a shared line
  during DMA corrupts data (measured on the old cart: `file_size 0` at the
  hldemo1 elevator until the header buffer was 16-aligned).

### 4.2 Budget (8 MB)

| region | KB | basis |
|---|---:|---|
| code + data + bss | ≤ 1000 target | old HL cart ELF 1428 KB **measured** (text 1009944 + data 184688 + bss 268360) includes the Quake engine, protocol and QC translations hl64 does not have [INFERENCE: 900–1000] |
| framebuffers ×2 + Z (320×240×16) | 450 | measured |
| rspq/tiny3d/display lists, matrices (double-buffered) | 96 | [INFERENCE] |
| render pool | 240 | measured route peak 240; worst cooked t0a0b leaf 726 = 223904 B |
| texture pool | 128 | route peak 127; worst t0a0d leaf 127 = 112864 B |
| collision pool | 208 | worst seam hldemo3→hldemo2 206592 B (`w64seam.py`) |
| layer directories (2 open) | 2 × 84 | measured 38–84 KB per map |
| studio pool (MDL/IDSQ/STB2 or own anim format) | 1536 | old cart route fits with ≥ 259 KB heap left; garg no-fit after hldemo1-return still open (SA2 §8.1) |
| entity core records | 495 × 256 B = 124 | section 6.2; cap 495 = ceil(1.25 × host peak 396) |
| game private arena | 192 | [INFERENCE: ~60 monsters × ≤ 1 KB + 150 movers × ≤ 256 B + items]; overflow refuses spawns like the spatial reserve |
| HLE1 blobs (4 slots) | 4 × 48 | [INFERENCE until measured per map; old: 72 KB text + 13–19 KB keys per map before HLE1] |
| dehydrate store | 64 | per-map records + 64 runtime pickups (old: 168 B each + names) |
| strings (interned) | 32 | [INFERENCE] |
| precache tables (455 models, 347 sounds, 96 styles) | 24 | names as offsets into layer precache tables |
| sound (16 voice buffers, 10 open wav64, sentence table) | 96 | [INFERENCE: old not measured] |
| sprite frame cache / decal texel cache / particles (512) | 64 / 48 / 16 | [INFERENCE] |
| dlight scratch (512 vertex pairs ×2) + flashlight pool | 48 | as old renderer |
| HUD sheets + font (ui8) | 128 | [INFERENCE] |
| per-frame scratch arena | 64 | [INFERENCE] |
| stack + libdragon heap slack | 256 | |
| **total** | ≈ 5.0 MB | leaves ≈ 3 MB; M6 decides whether studio pool grows (fixes the garg no-fit) |

Every number in `[INFERENCE]` gets a `mem:` line at boot and a high-water
mark in every route `stats` line; M3/M4/M6 gates replace the estimate with
the measured peak × 1.25, as the old tree did for its caps.

---

## 5. Cart I/O and streaming

### 5.1 File table

Cooked content is packed into the ROM's DFS by the build (`HL64_CONTENT=<dir
of player-cooked files>`); the ROM itself never contains Valve data in the
repository. At boot `io` resolves each needed file once to (cart offset,
size); afterwards no path lookups happen. Small config/text files may use
DFS reads at boot only.

### 5.2 Async DMA scheduler

- Caller-owned requests (file, offset, length, destination, priority,
  completion flag). Priorities: `IO_AUDIO` > `IO_OPEN` > `IO_SEAM` (pins) >
  `IO_PREFETCH`. A queued request a stronger class now needs is promoted.
- One piece in flight, ≤ 16 KB; per-frame budget 64 KB (seam budget 128 KB
  while a seam is approached); audio never waits behind more than one piece.
- Synchronous reads exist only for boot, New Game, and a collision chunk
  missing mid-trace (counted as blocked I/O; target 0 frames > 1 ms).
- Cache-line ownership as 4.1.
- Stats per route segment: per-frame KB histogram p50/p90/p99,
  `budget_over` (must be 0), blocked µs per frame, host `pi_over`.

ares models PI DMA at a fixed 5.25 MB/s without bus contention (measured);
real hardware is checked once per milestone on a flashcart when available.

### 5.3 World layers and residency

A **layer** is one open map: its `.w64` directory (resident while open),
its pinned COLL hull-0 chunks, its HLE1 blob (logic records), and its
global offset `g_M`. At most `HL64_LAYERS_OPEN` = 2 geometry layers and
`HL64_MAP_SLOTS` = 4 HLE slots (logic of closing maps with frozen timers).

Residency classes per frame (strongest first; a request evicts only weaker
classes; SA2 §4.2 as built):

- **open**: REND + TEXP of the eye leaf's cooked visible-sector list; MOVR
  (+ animated texture pages) of visible brush entities; COLL lists of grid
  cells within ±128 u of the player box; COLL of every SOLID_BSP / contents
  brush touching that window. Anything the renderer finds missing while
  drawing is requested at open priority and skipped (counted as pop-in).
- **pin**: announced destinations (route `setpos` look-ahead, reachable
  `trigger_teleport` destinations, approached seam's B set, cook-generated
  script pins), lapse after 90 frames or on arrival.
- **prefetch**: eye-leaf sets at eye + v·{0.25, 0.5, 1} s; collision window
  + v·0.5 s; cells of moving monsters/projectiles; hulls of brush entities in
  the box spanned by thinking monsters and projectiles.
- **pinned**: world hull-0 COLL while the layer is open.

Eviction: queued-not-started loads first, then LRU among chunks not wanted
this frame; prefetch never evicts a chunk used in the last 30 frames.
Pop-in accounting, teleport charging (eye jump > 128 u charges 16 frames),
`missing/popf/popmax` and blocked-I/O lines are kept from SA2 §4.2 because
the old-tree route logs report them and the M4/M5 gates compare them.

### 5.4 Campaign, portals, seams

From `campaign.cwg` (CWG3, `docs/formats/cwg-format.md`): map placement
`g_M`, links, seams (switch eye box, carry box, approach box, twins, B
sector set, OPEN draw set, portal translation `T`). Runtime rules (old
`cwg-format.md` "Runtime" and SA2 §6.8, restated as requirements):

1. Approach: the player origin enters a seam's approach box (in A-local
   coordinates) → open B's layer and HLE slot, pin B's seam set at seam
   priority. Nothing blocks.
2. Arming/switch: a touch seam switches when the eye enters the switch box,
   or (fallback) when an armed changelevel trigger stops being touched or
   has been touched for 1 s outside the box (counted as `forced`). A
   USEONLY seam switches when its changelevel is used.
3. Crossing: home := B; every position-bearing record of the player (and
   riders) gets `+T`; velocity, angles, punch, inventory and the entity
   itself are kept; twins copy A's state to B (origin + T, mover phase, think
   time, velocity), A's copy retires silently; `changetarget` fires in B.
   After a switch, changelevel touches are ignored until a frame with none.
4. Leave: A's sectors drop by LRU; A's physical records dehydrate by
   distance (release radius 1024 u, batches every 0.1 s); `layer_close(A)`
   when no A chunk is needed and no A record is live; A's timers freeze by
   remaining time and thaw when A reopens.
5. OPEN seams draw B's draw set translated by `−T`, scaled about the eye by
   1 + 1/256, clipped to meshlets touching the carry box.

### 5.5 Visibility

Point → leaf: the layer's 256 u grid gives a start node per cell, then a
short walk of the pinned hull-0 tree (O(1)-ish; memoised per entity, recomputed
only when its origin moves). Leaf → visible sectors: the cooked `vis[leaf]`
u16 list (PVS collapsed at cook). Eye in solid keeps the last valid leaf.
Entity PVS tests (sight checks, "is this entity drawable") use the cooked
`VISR` rows of the entity's layer, decompressed on demand into a one-row
cache per layer (BSP run-length format, `docs/formats/w64-format.md`).

---

## 6. Entity system

### 6.1 Options considered

| | A: HLSDK `edict_t` + `entvars_t` verbatim | B: compact core record + game private data (chosen) |
|---|---|---|
| size | `entvars_t` ≈ 676 B (ILP32) + edict header (area link, 48 leaf numbers, free/serial, private pointer) ≈ 800 B | core ≈ 256 B + per-class private (0–1 KB, only for classes that need it) |
| 495 slots | ≈ 390 KB, all resident | ≈ 124 KB core + ≤ 192 KB private arena |
| old tree for comparison | 884 B × 495 ≈ 427 KB (Quake progs entvars + HL extras) **measured** sizing | — |
| licence | headers are HLSDK: the layout would live only in `game/`, so the engine could not use it without an MIT re-declaration | MIT header owned by us; game names fields after HLSDK so ported code reads the same |
| LP64 host parity | pointers in entvars grow → host sizes diverge | handles (u16) and function ids (u16) → identical sizes on cart, `-m32` and LP64 |
| serialisation (dehydrate/save) | pointers and function pointers need fix-up tables | records are position-independent; core copies as bytes |

Option A buys literal source compatibility with HLSDK C++ code we do not
use (our game is C); option B saves ≈ 270 KB of RDRAM at the 495 cap and
makes dehydration and host parity exact. **Chosen: B.** (Decision D1 lets the
user overrule.)

### 6.2 Core record (`include/hl64/ent.h`, engine-owned)

Field groups, ≈ 256 B, 16-aligned, hot fields first (link + box in the first
two D-cache lines, as the old tree measured for area scans):

| group | fields (HLSDK names where one exists) |
|---|---|
| link | area list prev/next (u16 slot indices), absmin/absmax (the linked box: already includes the 1 u / item expansion, specs/area-links-and-touch.md) |
| identity | serial (u16, bumped on free), state (free/live/dying), layer (s8: owning map slot), class id (u16, game-defined), private handle (u16), source record gid (u16, 0xFFFF runtime) + runtime id (u16) |
| motion | origin, oldorigin, velocity, basevelocity, angles, avelocity, mins, maxs, view_ofs (vec3 each) |
| physics | movetype (u8), solid (u8), flags (u32 `FL_*`), gravity, friction (f32), groundentity, owner, aiment (ent_h), waterlevel, watertype (s8) |
| timing | nextthink, ltime (f32); think, touch, use, blocked (u16 function ids into the game's tables; 0 = none) |
| model | modelindex (u16 global precache index), skin (s16; brush contents when < −1), body (u8), sequence (u16), frame, animtime, framerate, scale (f32), controller[4], blending[2] (u8), effects (u16 `EF_*`) |
| render | rendermode, renderfx, renderamt (u8), rendercolor[3] (u8) |
| names | targetname, classname (str_h) — the engine resolves names for wakes and fires |
| vis | leaf set: up to 8 leaf numbers, or "large" (test by box) |

Everything else the HLSDK keeps in `entvars_t` (health, takedamage,
deadflag, enemy, target, noise*, dmg*, armor, items, message, netname,
`iuser*`…) belongs to the game: the generic ones in a game "base" private
struct, the rest in per-class private structs (the HLSDK class member
layout: `CBaseMonster`, `CBaseToggle`, `CBasePlayer` …). The player's
movement state (`bInDuck`, `flDuckTime`, `flTimeStepSound`, punchangle,
fallvelocity, oldbuttons, maxspeed …) lives in the engine's single player
block (`ent_player_t`), shared with pmove.

Handles: `ent_h` = slot index (u16); a game that keeps a reference across
frames stores (index, serial) and validates with `ent_valid`. Freed slots
are not reused until `HL64_ENT_REUSE_DELAY` (0.5 s of game time) has passed,
except during the first 2 s of a new game (specs/think-and-time.md).

Function ids: the game declares its think/touch/use/blocked functions in one
X-macro list (`game/funcs.def`) that generates the dispatch tables and
stable ids; ids are what records, the dehydrate store and saves hold.

### 6.3 Records, hydration, dehydration

Map entities exist first as HLE1 **records** in the map's blob (24 B each,
`docs/formats/hle-format-v1.md`): logic (no entity: relays, multi_managers,
multisources, lights…), point (landmarks, path corners: queried by name),
and physical (become core entities when hydrated). The `ent/` module owns
the blob residency, record state (pending/live/killed/dry), the name index
(targetname → records, scoped to the caller's source map, plus twin
aliasing), the spatial pending grid (512 cells of 512 u per map) and the
budgeted pump. The game owns what a record means: `game_spawn_record(gid,
e)` interprets the cooked keys into the entity and its private data;
`game_dehydrate(e, w)` / `game_rehydrate(e, r)` write/read the per-class
delta the level state keeps (origin, health, mover state, flags, think
time, names, references as (map, lump) or runtime id).

Requirements carried from the old tree (SA2 §6.8 "as built"; each is a
test in specs/think-and-time.md or the M4 gate):

- Spawn radius 768 + 192 u margin around anchors (player; player mapped
  through each approached portal); release 1024 u; a 256 u prefetch ring
  requests studio models of the 6 nearest pending records.
- Spatial reserve: optional hydration only while 48 slots stay free;
  transient effects (gibs, debris) keep their own 32-slot reserve.
- Named fires and class searches wake pending records synchronously
  (at most two hydrate in the frame; the rest queue ahead of the pump).
- Every trace wakes pending records in its move box (via the pending grid;
  the box test is O(1) when no counted cell meets the box).
- Deferred physical uses into a closed map: canonical (map, lump) /
  runtime-id FIFO replayed in order on rehydration; never edict pointers.
- Closing a map freezes its timers by remaining time (128 entries);
  cross-boundary references are cut as if out of range; riders pin.
- Runtime-created pickups and monstermaker children persist by (map,
  runtime id), up to 64 pickups.
- Entity budget per frame: 2 ms cart / 8 records host, shared in order:
  neighbour release, arrival pump, home pump, closing release; each pump
  still spawns one record a frame when the budget is gone.

### 6.4 Precache and resources

Global tables (models 455, sounds 347, backend sfx 359, lightstyles 96)
whose entries are owned by maps and by the common/player set; a slot is
reused when its owner maps are closed and nothing live references it.
Indices are 16-bit. Carried-weapon assets are owned by the player (the old
tree died with `v_crowbar.mdl not precached` before this rule). See
specs/precache-and-resources.md.

---

## 7. Engine ↔ game interface

A slim C API in our own names, statically linked (no function-pointer
table, no DLL boundary). It covers the HLSDK `enginefuncs_t` /
`DLL_FUNCTIONS` subset the Uplink + Hazard game uses (inventory of the old
game's engine calls: `g_*` QuakeC-style builtins 437 call sites, `SV_*` 78,
`ED_*` 29, `MSG_*` 40, `Cvar/Cmd/Con` ≈ 250, `Mod_*` 14, `GSFX_*` 78, `HLE_*`
API). Table of contents; exact prototypes are written in M0 in
`include/hl64/eng.h` and `game.h`.

### 7.1 Engine services (`eng.h`)

| area | services | HLSDK counterpart |
|---|---|---|
| entities | alloc, free (deferred reuse), num↔handle, valid, set_origin (relink), set_size, set_model (index, bounds from model), link (with/without trigger touch), unlink | pfnCreateEntity, pfnRemoveEntity, pfnSetOrigin, pfnSetSize, pfnSetModel |
| search | next by targetname / classname in a map slot (with wakes), in sphere, in box, client in PVS | pfnFindEntityByString, pfnFindEntityInSphere, UTIL_EntitiesInBox, pfnFindClientInPVS |
| traces | line, hull (0–3), box, single entity, point contents, texture/material at hit | pfnTraceLine, pfnTraceHull, pfnTraceModel, pfnPointContents, pfnTraceTexture |
| movement | walk_move, move_to_origin (step toward goal), drop_to_floor, change_yaw/pitch, monster step | pfnWalkMove, pfnMoveToOrigin, pfnDropToFloor, pfnChangeYaw |
| vis | entity/point in PVS of another point, leaf of point | pfnCheckVisibility, pfnSetView (no) |
| resources | precache model/sound (index), model index by name, model type/bounds/frames, studio: sequence lookup/info (frames, fps, flags, linear movement, events list), bone/attachment positions | pfnPrecacheModel/Sound, pfnModelIndex, pfnGetModelPtr (as query functions), pfnGetAttachment, pfnGetBonePosition |
| sound | emit (entity, channel, sound index, volume, attenuation, flags, pitch), ambient at point, stop, sentence by index/group name | pfnEmitSound, pfnEmitAmbientSound |
| light | set lightstyle pattern | pfnLightStyle |
| effects | direct calls, no messages: sparks, ricochet, blood stream/sprite, bullet decal, decal trace, explosion, smoke, beam ent↔point/ent↔ent, tracer, gibs/model shower (break model), dlight, elight, muzzle flash, screen shake, screen fade, text message (titles) | the `TE_*` / `SVC_*` user-message set HLSDK sends |
| HUD/2D | draw sub-rect of a ui8 sheet (colour, mode), fill, character/string, screen size | client-DLL SPR_* and FillRGBA |
| time & rng | game time, frametime, random long/float (one engine stream, reseeded by New Game; tests can fix the seed) | gpGlobals->time, pfnRandomLong |
| console | cvar register/get/set, command add + argc/argv, print/dprint | pfnCVarRegister, pfnAddServerCommand, pfnAlertMessage |
| records | record keys (str/float/vec/name), origin/angles/bounds of a record, fire name in slot, fire later, use-targets, master state, timers, map slot of entity, dehydrate/rehydrate hooks | (no counterpart: replaces keyvalue parsing and level transitions) |
| campaign | current home map, map name of slot, global offset of slot | — |

### 7.2 Game exports (`game.h`)

`game_init`, `game_new_campaign(start_map)`, `game_map_opened(slot)`,
`game_map_closing(slot)`, `game_spawn_record(gid, e)` → kept/removed,
`game_spawn_logic(gid)`, dispatch of think/touch/use/blocked by function
id, `game_start_frame`, `game_player_spawn`, `game_player_prethink`,
`game_player_postthink`, `game_player_command(usercmd)` (impulses, weapon
select, `+use`), `game_pm_move(player block, physents)` (pmove, decision
D2), `game_dehydrate`/`game_rehydrate`, `game_save_globals`/
`game_load_globals` (env_global states, squads), `game_calc_view` (bob,
punch, view model placement; client-DLL V_CalcRefdef), `game_hud_draw`,
`game_hud_frame`, `game_studio_event(e, event, options)` (client-side anim
events: muzzle flashes, shells), `game_studio_setup_bones(e, out)` (pose →
bone matrices for the renderer, D3).

### 7.3 Re-targeting `game/`

The old `source/hl` (21 files, ≈ 32k lines) runs on Quake's progs entvars
(fields repurposed: `lip`, `cnt`, `walkframe`, `state`, `lefty`…), the
`g_*` QuakeC-builtin layer and QuakeC function handles (`G_Func/G_Call`).
Those layers are forbidden. Re-targeting follows the audit's §1.5 per file
(ranges in 16.2; the Quake-interface glue in these files falls under D9):

1. Replace `edict_t->v.<quake field>` by core fields (HLSDK names) or by the
   class's private struct member; repurposed Quake fields become named
   members (`lip` "behaviour class" → `mon.ai_class`, etc.).
2. Replace `g_*` builtins and `SV_*/ED_*/MSG_*` calls by `eng.h` services.
3. Replace QuakeC-style `G_Func` think handles by `funcs.def` ids.
4. Replace protocol writes (`svc_hltent`, `MSG_Write*`) by the effects API.
5. Delete the workarounds for engine gaps the new engine fills (e.g. the
   grenade integrating its own gravity 0.5 / backoff 1.2 because Quake's
   MOVETYPE_BOUNCE had neither: hl64 implements `gravity` and `friction`;
   keep the old behaviour until M5 parity passes, then switch).
6. Each file keeps its HLSDK-derived names and comments citing HLSDK
   classes; nothing from `source/g_*.c` is carried over (rewrite any helper
   that came from there from HLSDK `util.cpp`/`subs.cpp` instead).

---

## 8. Collision, traces, physics

- `coll/`: hull point contents and segment traces over COLL chunks (8 B
  nodes, axial classes by index range, non-axial planes per chunk, extern
  gids) — format `docs/formats/w64-format.md`, semantics
  [specs/trace-and-hulls.md](specs/trace-and-hulls.md). Box traces against
  non-BSP entities use a box hull built on the fly. Brush entities trace in
  their local space (translation by origin, rotation by angles: the
  rotated-brush rules are in the spec). Traces against a map layer are made
  in that map's local frame (global − `g_M`) and translated back.
- Area tree: one tree over the home component's global box, splitting the
  longest of the three axes down to ≤ 1024 u cells (depth ≤ 7, ≤ 256
  nodes) — the old tree measured Quake's x/y depth-4 tree putting every
  stacked map's entities in the same leaves. Solid and trigger lists per
  node. [specs/area-links-and-touch.md](specs/area-links-and-touch.md).
- `phys/`: per-movetype stepping in the order of 3.2,
  [specs/physics-movetypes.md](specs/physics-movetypes.md); think timing in
  [specs/think-and-time.md](specs/think-and-time.md).
- Player movement: the engine builds the physent list (world layers near the
  player, SOLID_BSP and SOLID_BBOX/SLIDEBOX entities within the move box,
  ladders and contents brushes as move-ents) and calls `game_pm_move`;
  [specs/pmove-host.md](specs/pmove-host.md). With decision D2 = HLSDK
  `pm_shared` that code is in `game/pm/` unchanged except for its array caps.
- Surface materials for footsteps and impact sounds come from the cooked
  material table (`w64-format.md` "Surface materials"): no faces or texinfo
  on the cart.

---

## 9. Renderer

All drawing goes through tiny3d/rdpq in `plat/n64/gpu.c`; `engine/render`
builds per-frame lists. Order:

1. **View**: `game_calc_view` gives origin, angles, FOV, view model state;
   the renderer sets one camera; each open layer gets a camera-relative
   `T3DMat4FP` (`g_M − eye`, so s16 map-local verts stay in range).
2. **World** (`render/sector.c`): eye leaf → visible sectors → sector bbox
   and meshlet bbox frustum cull → counting sort by texture page → per page
   one TLUT+CI4 bind, per meshlet one `t3d_vert_load` + one
   `t3d_tri_draw_strip` of the pre-encoded strip. Styled meshlets recoloured
   when a style value changes ([specs/lightstyles.md](specs/lightstyles.md));
   dynamic lights add into a scratch copy of touched meshlets and the
   flashlight is drawn as a decal-Z pool pass
   ([specs/dlights.md](specs/dlights.md)). OPEN-seam cross-layer draw as 5.4.
   Spike numbers (measured): 806 µs CPU submit and ≤ 4.1 ms RSP+RDP for a
   full dome PVS set of 142 meshlets.
3. **Movers**: visible brush entities (MOVR chunks) with the entity matrix,
   same page submit; static submodels are baked into sectors.
4. **Studio** (`render/studio.c`): STB2 v4 bone-local meshlets recorded once
   per model into an rspq block (packing budget 2 ms/frame), per entity per
   frame only the bone matrices (one `T3DMat4FP` per used bone,
   double-buffered) and a block replay. Sequence/frame come from the
   entity's `sequence`, `frame`, `animtime`, `framerate`; bone controllers
   and blending from the core record. Lighting: per-sector ambient probes
   (64 u lattice, trilinear) + dlights. View model: CPU skinned (chrome
   needs bone-space normals) into one of two buffers. STEP-movetype
   entities are interpolated between their last two think positions
   (specs/physics-movetypes.md "Render interpolation"). Bone matrices come
   from `game_studio_setup_bones(e, out)` in `game/studio/` (HLSDK
   `StudioModelRenderer`-derived, D3 option a): the engine owns meshes,
   pools and drawing; the game owns pose maths, as in GoldSrc where the
   client DLL does studio rendering.
5. **Particles**: one pool (512), drawn with the TinyPX particle ucode in
   one call (s16 view-relative).
6. **Translucent pass**: entities whose render mode is not Normal (or
   TransAlpha brushes, which stay opaque with alpha compare), sprites
   (SPR v2 converted to CI4 frames by the cook), beams; sorted back to
   front, depth test without depth write; additive via the RDP adder with
   the wrap guard. Semantics in [specs/render-fx.md](specs/render-fx.md).
7. **Decals**: placed on cooked faces of resident sectors/movers, drawn
   after the opaque world with decal Z; I4 texels from the cooked decal
   sheet, LRU texel cache ([specs/render-fx.md](specs/render-fx.md)).
8. **HUD and 2D**: ui8 sheets (CI8 or brightness), copy/blend modes,
   numbers, crosshair, pain arrows, text messages, centerprint, fades.
9. **Console/menu overlay**, then present (3.3).

---

## 10. Audio

- libdragon mixer, 16 voices at 22050 Hz; samples are wav64 (VADPCM)
  streamed from the cart with `WAV64_STREAMING_FULL` (never expanded to
  RAM); ≤ 10 open wav64 streams with LRU close (never close a wave a voice
  owns: stop voices, `rspq_wait`, then close). Wave reads go through the
  `IO_AUDIO` class so world streaming can never starve audio [design;
  the old tree kept libdragon's blocking DFS reads for audio].
- Channels and attenuation: [specs/sound.md](specs/sound.md) (entity
  channel replacement, CHAN_AUTO stealing, ATTN_* → distance, stereo pan,
  pitch 1..255 % → mixer frequency, loops virtualised to 32 slots).
- Sentences: `sound/sentences.txt` cooked to a table (group, index, words,
  VOX pitch/volume/end); words play in sequence on one voice; up to 24
  word slots outside the precache table.
- Spatialisation uses global coordinates (both open layers are already in
  one space after CWG placement).
- `audioconv64` VADPCM Huffman bug: the cook keeps the old tree's
  workaround (`n64/docs/bug-audioconv64-vadpcm-huffman-silence.md`) until
  libdragon fixes it.

---

## 11. Input

Pad → `usercmd`: stick = move or look (configurable), C-buttons strafe/look,
Z = attack, R = attack2/jump (layout table in `engine/in/bind.c`, cvars for
sensitivity, invert, deadzone). Absolute stick turning scales by `dt`. The
route runner and `ARES_TEST_INPUT` inject at the same point (raw pad
state), so scripted and real input share code.

## 12. Console, cvars, commands

Our own small implementation: cvar table (name, string, float, flags:
archive/cheat), command table, line buffer executed once per frame,
`exec` of ROM text files. The console overlay prints the last N lines
(cost-bounded: the old tree measured 4–15 ms/frame of notify drawing near
seams with diagnostic spam → on the cart, diagnostics go to ISViewer only;
the overlay shows only `con_notify`-class lines). Test-only commands
(`setpos`, `god`, `noclip`, `impulse`, `hl_drop`, `stats`, `shot`,
`ent_dump`) compile into every build but `setpos/god/noclip` require
`sv_cheats 1` outside test builds.

## 13. Menu

Minimal, our own art-free design: text menu in the player's HL font (cooked
from their `gfx.wad`/fonts) over a dark fill. Items: New Game (Uplink /
Hazard Course, from the CWG start and training map fields), Load/Save
(decision D4), Options (volume, music, look sensitivity, invert, stick
mode), Quit to title. Pause on Start.

## 14. Save and level state

- Level state is the dehydrate store (6.3): per map, keyed by BSP lump or
  durable runtime id. No per-seam save/teardown exists.
- A saved game (D4) = engine globals (time, rng, home map, layer set),
  every live core record, every game private block (by class, versioned),
  the dehydrate stores, HLE record states, frozen and live timers,
  precache owner sets, lightstyle patterns, player block. Load = New Game
  shell + replay: open the saved layers, restore records without running
  spawn functions, relink, then resume. Size estimate 40–90 KB [INFERENCE],
  which fixes the media choice.

## 15. Test and profiling

- **Route runner** (`engine/test/route.c`): the old route language is kept
  verbatim so `uplink_route.cfg` and the Hazard walk routes run unmodified:
  `wait`, `waitmap`, `waitnear`, `waitglobal`, `waitmoved`, `waitgone`,
  `waitclass`, `look`, `lookat`, `shot`, `stats`, `pass`, anything else =
  console command (`setpos x y z pitch yaw`, `+use`, `impulse N`, `god`,
  `hl_drop class x y z`). Positions are map-local to the player's home
  layer. `waitmap <map>` means "home map is <map>". Failures print
  `route FAIL …`; exit code 0/1 on host, ares harness exit register on cart.
- **Stats line** (one line per `stats` step): edicts live/peak, records
  pending, pools used/peak per pool, heap free/largest, fps and pace
  histogram, DMA p50/p99, pop-in, blocked I/O, precache peaks, position.
  Field names follow the old tree's `stats` lines where the same quantity
  exists, so `tools/prof_compare`-style comparisons work.
- **PROFILE zones**: compile-time `PROFILE=1`; per-zone µs/frame and counts
  rolled up once per second (`hl64: prof …`): frame, upd, phys, think,
  trace, touch, pmove, stream, io, hydrate, rnd, world, movers, studio,
  sbuild, trans, decal, hud, snd. Zone names map 1:1 onto the old tree's
  `quake64:` zones where comparable (table in `engine/test/prof.c`).
- **ares harness**: test-harness PI peripheral at `0x13fe0000`
  (framebuffer PPM dump, exit code) and input file injection, from our ares
  patch (`test/ares-testharness.patch`, imported if CLEAN). ISViewer log.
- **Oracle**: the old tree's `hl_parity` host build and cart ROM produce
  reference logs, screenshots and fixtures (specs/README.md describes the
  fixture capture process, which only dirty-side agents run).

---

## 16. Module reuse plan

Verdicts from ProvAudit's final audit (2026-11-13; the binding text is
[provenance-audit.md](provenance-audit.md) — where this table differs, the
audit wins). Labels: **CLEAN** (ours, importable), **CLEAN+QI** (the
audit's QUAKE-INTERFACE: our own code, written against Quake engine
interfaces — `edict_t`/`v.*`, `sv.`/`cl.`, `model_t`, Cvar/Cmd/Con, Hunk),
**QUAKE** / **XASH** (contains code derived from them), **HLSDK** (derived
from the SDK: lives in `game/` or `tools/hlsdk/`), **FORBIDDEN** (present
in `9fd3580`, or QuakeC translations).

Actions: **import** = copy, history note naming the audit entry, licence
header; **retarget** = import, then replace the Quake-interface glue by
hl64 APIs; **split** = import only what the audit entry marks as ours and
rewrite the listed ranges from the named spec in the same commit;
**rewrite** = new code from `docs/specs`, `docs/formats` and allowed
sources only; **drop** = not needed in hl64.

**Policy gate:** PROVENANCE.md today admits only files marked CLEAN.
Every *retarget* and *split* row below needs the owner of PROVENANCE.md to
admit QUAKE-INTERFACE files (decision D9). If D9 says no, those rows
become *rewrite*, and the streaming scheduler, campaign seams, HLE record
runtime, level state, sound backend, studio residency, sector and studio
renderers and route runner each need a behaviour spec first (none exists
for them yet; the formats and SA2 cover most of their observable rules).

### 16.1 Engine

| hl64 module | old-tree source | verdict | action | rewrite source / note |
|---|---|---|---|---|
| plat/n64 boot, carve order | `n64/port/quakegeneric_n64.c` | CLEAN+QI | retarget | 3.1 |
| plat/n64 VI pacing, palette/TLUT, frame bracket | `n64/port/r64_vid.c` | CLEAN+QI except 162–172 (Quake `VID_Init`) and 252–258 stubs | split | pacing per 3.3; own vid struct replaces `VID_Init` |
| plat/n64 sys (error, clock, log, RSP-crash wrap) | `n64/port/sys_n64.c` | QUAKE (≈ 185 lines) | rewrite | the RSP-crash `--wrap` and on-screen error painter (ours, 169–250) re-typed from their description |
| FPU exception shim | `n64/port/fpu_shim.c` | CLEAN | import | |
| core console, cvars, commands, keys, menu, host loop | `source/{console,cvar,cmd,keys,menu,host,host_cmd}.c` | FORBIDDEN | rewrite | 3.2, 12, 13 |
| input | `source/in_*.c`, `sdl_input.c` | FORBIDDEN / n/a | rewrite | 11 |
| mem pools | `n64/port/q64_pool.c`, `source/q64_pool.h` | CLEAN | import | |
| limits | `source/q64_limits.h` | CLEAN | retarget | → `include/hl64/limits.h`, Quake zone/hunk constants removed |
| heap probes | `n64/port/r64_malloc.c` | CLEAN | import | |
| hunk/zone shims, zone budget | `n64/port/r64_mem.c`, `r64_hunk.c`, `q64_budget.c` | CLEAN+QI | drop | hl64 has no hunk/zone |
| io DMA scheduler | `n64/port/q64_io.c`, `source/q64_io.h` | CLEAN | import | 5.2 |
| cart file table, io stats | `n64/port/r64_cart.c`, `r64_iostat.c` | CLEAN | import | |
| host io, heap, libdragon shim | `n64/port/host/q64_io_host.c`, `q64_heap_host.c` (dlmalloc, public domain), `host/libdragon.h` | CLEAN | import | |
| host render mirror | `n64/port/host/q64_host_render.c` | QUAKE | drop | replaced by the draw-list recorder |
| world format headers | `source/w64_format.h`, `w64_node.h`, `w64_world.h` | CLEAN+QI | retarget | |
| world layer load, hull walk, PVS, point→leaf | `n64/port/w64_world.c` | CLEAN+QI except 9 QUAKE regions (≈ 286 lines: 443–458, 706–721, 797–799, 859–996, 1077–1099, 1115–1144, 1157–1182, 1233–1267) | split | regions from trace-and-hulls.md, area-links-and-touch.md 2, 5.5 and the BSP vis row format |
| world residency / stream pump | `n64/port/w64_stream.c` | CLEAN+QI | retarget | 5.3 |
| campaign, seams, portals | `n64/port/q64_world.c/.h` | CLEAN+QI | retarget | 5.4 |
| v1 world (q64, page caches, atlases, v1 renderer) | `q64_map.c`, `q64_brush.c`, `r64_world.c`, `r64_world_atlas.c`, `r64_world_tex.c`, `r64_light.c` | mixed | drop | |
| collision, traces, area tree | `source/world.c` | FORBIDDEN | rewrite | trace-and-hulls.md, area-links-and-touch.md |
| physics movetypes, step moves | `source/sv_phys.c`, `sv_move.c` | FORBIDDEN | rewrite | physics-movetypes.md, think-and-time.md |
| player movement | `source/sv_user.c` | FORBIDDEN | replace | HLSDK `pm_shared` in `game/pm/` + pmove-host.md (D2) |
| entity table, alloc/free | `source/pr_edict.c`, `progs.h` | FORBIDDEN | rewrite | 6.2, think-and-time.md 5 |
| precache tables | `source/sv_main.c` | FORBIDDEN | rewrite | precache-and-resources.md |
| HLE records, hydration, wakes, timers | `source/hl/hl_ents.c/.h`, `hl_entcook.h` | CLEAN+QI (hl_ents.c 2486–2524 HLSDK) | retarget | 2486–2524 (multi_manager shape) → `game/`; entcook field table retargeted to the core record |
| level state / dehydrate store | `source/hl/hl_levelstate.c/.h` | CLEAN+QI | retarget | game hooks 7.2 |
| node graph cook + runtime | `source/hl/hl_nodecook.h` | CLEAN+QI except 280–578 (QUAKE/XASH tracing) | split | 280–578 rewritten on the hl64 trace API |
| mixer, wav64 voices | `n64/port/snd_n64.c` (VADPCM decoding is libdragon's) | CLEAN+QI | retarget | sound.md; `S_*` API → `snd_` API, rename the two cvars at 105–106 and the Quake channel field vocabulary |
| host audio | `n64/port/host/q64_host_audio.c` | CLEAN | import | |
| HL sound names | `source/hl/hl_sounds.c/.h` | CLEAN+QI | retarget | |
| sentences | `source/hl/hl_sentences.c` 1–505 (parser), `hl_sentences.h` | CLEAN | retarget | entity part of the file is HLSDK → `game/` |
| sector renderer | `n64/port/r64_sector.c` | CLEAN+QI except 431–453 (texture animation) and 634–649 (dlight falloff) | split | world-surfaces.md 4, dlights.md 3 |
| studio draw (STB2) | `n64/port/r64_studio_bake.c` | CLEAN+QI except 828–879 `chrome_axes` (XASH) and 2036–2048 (HLSDK frame wrap) | split | chrome and frame wrap from `game/studio` (D3) |
| studio entity draw | `n64/port/r64_studio.c` | CLEAN+QI except 599–609 (HLSDK frame wrap idiom) | split | frame wrap from `game/studio` |
| studio bone setup, animation | `source/studio_geom.c` | XASH/HLSDK 425 lines (77–188, 229–391, 399–517, 586–623); ours 36–75, 519–584, 627–1010 (Cephes constants need attribution) | rewrite (bones) + split (ours) | D3 |
| studio residency pool | `source/mod_studio.c` | CLEAN+QI | retarget | SA2 §8.1 |
| studio format structs | `source/studio.h` | HLSDK 8–207, ours 209–285 | split | HLSDK part → `game/` / `tools/hlsdk/` |
| software studio paths | `studio_soft.c`, `r64_studio_sw.c`, `r64_studio_stubs.c` | QUAKE / CLEAN+QI | drop | |
| translucent pass, sprites | `n64/port/r64_trans.c` | CLEAN+QI except 87–150 (fx blend) and 592–644 (sprite orientation) (XASH) | split | render-fx.md 1–3; SC4 cache, RDP modes, sort are ours |
| decals | `n64/port/r64_decal.c` | CLEAN+QI except 403–421, 701–764, 944–945 (XASH) and 592–619 (Quake texture-axis table) | split | render-fx.md 4 (own overlap policy, axes from the cooked face's texel coordinates) |
| sprite file parse | `n64/port/gsfx/gsfx_spr.c` | CLEAN except 12-line group-frame selection and 4-line origin→bounds (QUAKE) | split | render-fx.md 3 |
| particles (TinyPX submit) | `n64/port/r64_part.c` | CLEAN+QI | retarget | particle simulation itself: render-fx.md 5 |
| 2D draw (ui8 sheets, fades) | `n64/port/r64_draw.c` | CLEAN+QI except ≈ 48 Quake lines (`Draw_Character`, `Draw_String`, `Draw_CachePic`, copied comments) | split | 9 step 8 |
| texture upload / TMEM bind | `n64/port/r64_tex.c` | CLEAN+QI except 725–762 (texture animation) | split | world-surfaces.md 4 |
| render main (view setup) | `n64/port/r64_main.c` | QUAKE 45–54, 87–129; rest Quake-API-shaped | rewrite | 9 |
| renderer internals header | `r64_local.h` | CLEAN+QI | retarget | |
| Quake alias models | `r64_alias.c`, `r64_alias_bake.c` | QUAKE / CLEAN+QI | drop | |
| client effects (temp ents, beams, blood…) | `n64/port/gsfx/gsfx_cart.c`, `gsfx_cl.c` | XASH (501 + 721 lines) + HLSDK; our names/objects/attachments/draw lists/palette code is CLEAN | rewrite (effect builders), split (ours) | render-fx.md 5; HLSDK temp-entity physics → `game/` |
| effect helpers | `gsfx_util.c` | CLEAN | import | |
| profiler | `r64_prof.c` / `source/q64_prof.h` | CLEAN+QI / CLEAN | retarget / import | 15 |
| route runner | `n64/port/q64_route.c` | CLEAN+QI | retarget | 15 |
| frame stats, selftest | `frame_stats.c`, `q64_selftest.c` | CLEAN+QI | retarget | `engine/test` |
| ares harness | `n64/port/test_harness.h`, `test/ares-testharness.patch`, `ares-large-rom-isviewer.patch` | CLEAN | import | |
| SDL / Android / GLES | `n64/port/gles/`, `sdl/` | — | drop | N64 only |

### 16.2 Game (`game/`, HLSDK licence)

| files | verdict | action |
|---|---|---|
| `hl_grunt`, `hl_monsters`, `hl_monstermaker`, `hl_ambient`, `hl_scripted`, `hl_sentences` (entities), `hl_nodes`, `hl_movers`, `hl_breakable`, `hl_items`, `hl_weapons(.h)`, `hl_logic`, `hl_env` (372–1166), `hl_hud`, `hl_use(.h)`, `hl_material(.h)`, `hl_ai` | HLSDK translations, built on forbidden QuakeC-translation helpers | retarget per 7.3; rewrite inside: `hl_ai.c` 746–917 (QuakeC walkmonster thinkers) and every call into `g_*`/`FoundTarget`/`M_Visible`/`ai_run`/`walkmonster_start`; `hl_material.c` 236–290, 554–568; `hl_use.c` 120–176 (delete) and 369–386; `hl_monsters.c` 255–269 (→ `ent_alloc`); `hl_nodes.c` 228–243 and `hl_scripted.c` 239–256 (→ engine `drop_to_floor`); `hl_weapons.c` 303–306 (`crandom`); `hl_ents.c` 2486–2524 |
| `hl_view.c` | HLSDK-side view code; 62–93 (shake) Valve algorithm | retarget into `game/hud`, rewrite 62–93 from HLSDK |
| `hl_input.h`, `hl_view.h`, `hl_hud_rects.h` | CLEAN | import |
| `gsfx_sv.c`, `gsfx_te.c` | HLSDK (effects.cpp, util.cpp…); `te_trace` QUAKE | retarget onto the effects API (7.1), rewrite `te_trace` with `eng` traces |
| `source/g_*.c`, `game.c`, `game_weapons.c`, `g_entvars.h`, `g_fielddefs.c`, `g_api.c` | FORBIDDEN | not used; helpers they provided are rewritten in `game/util.c` from HLSDK `util.cpp`, `subs.cpp`, `cbase.cpp`, `monsters.cpp` |
| HLSDK `pm_shared.c`, `pm_defs.h`, `pm_math.c`, `pm_movevars.h`, `pm_materials.h` | HLSDK | import into `game/pm/`, caps per pmove-host.md 2 (D2) |

### 16.3 Tools and docs

| item | verdict | action |
|---|---|---|
| `tools/w64c/w64c.py` | CLEAN except QUAKE 344–373 (texture animation), 424–431, 838–850, 861–944 (light-point walk), 1524–1534 (vis row scan), 121–128 (stock style strings); HLSDK 1349–1446 (materials.txt); `cook_face` 405–519 replicates the old v1 renderer's surface bake, so its verdict follows that file | split: rewrite those ranges (and `cook_face`) from world-surfaces.md 1–4 and the BSP vis row format, proven by W1 (byte-identical packages); style strings come from game/'s HLSDK table; 1349–1446 → `tools/hlsdk/` |
| `tools/w64c/w64check.py` | CLEAN except 296–368 (tracer) | split: tracer from trace-and-hulls.md |
| `tools/w64c/w64seam.py` | CLEAN except 77–83 (AngleVectors) | split: specs/README.md conventions |
| `tools/w64c/hlecook.c` (+ `hl_entcook.h`) | CLEAN | import |
| `tools/studio_bake` (`bake_studio.c`, `bake_meshopt.cpp`) | CLEAN, links `studio_geom.c` (XASH) | retarget onto the D3 bone code |
| `hl_campaign.py`, `ui_bake/`, `hlhud_bake/`, `world_tex_bake/`, `hl_stage_content.py`, `hl_sprite_cook.py`, `hl_gsfx.py`, `convert_hl_sounds.sh`, `extract_hl_sounds.sh` | CLEAN | import (`hl_stage_content.py` retargeted to the hl64 content layout) |
| `q64mapc`, `q64zone`, `hl_world/{stream_plan,zone_pack,stitch_and_carve}.py` | — | drop (v1 and zone system; `.w64` keeps no ZONE/DRYS/STRM chunks in hl64) |
| `n64/docs/w64-format.md`, `cwg-format.md`, `hle-format-v1.md` | CLEAN | import into `docs/formats/`, scrubbing the 7 + 4 lines that name Quake/old-engine functions (restate as prose) |
| `n64/docs/stream-arch-v2.md` | CLEAN | cite as history (measurements); not imported |
| `q64-format-v1.md`, `q64-map-streaming.md` | — | drop (obsolete; the latter quotes Quake source) |
| route files `test/uplink_route.cfg`, Hazard walk routes | CLEAN data | import as `test/routes/`; their console commands exist in hl64 with the same names |

---

## 17. Milestones and gates

Every gate runs on the host (`make host && test/run/<gate>.sh`) and on ares
(`make rom && test/run/<gate>.sh --ares`); a gate passes only when both
pass. Oracle numbers come from the old tree's logs captured once per
milestone by a dirty-side agent into `test/fixtures/oracle/`.

| M | deliverable | gate (host + ares) |
|---|---|---|
| M0 | Makefile (rom on `n64.mk` + tiny3d, host), plat layers, boot carve, console/cvars, DFS pack of player-cooked content, ares harness, provenance lint | ROM boots in ares to a `boot ok` line with the memory carve table and exits 0 via the harness; host prints the identical carve table; `tools/provcheck` passes (no file from the forbidden list, no line-level match against `git -C quakegeneric show 9fd3580:` files above a threshold, all headers carry the right licence) |
| M1 | io scheduler, layer open (eager is acceptable), sector renderer, movers static, textures + texture animation, lightstyles, free camera; cook ranges rewritten (16.3) | 3 fixed poses in hldemo1 (start room, dome, elevator): ares screenshots SSIM ≥ 0.95 vs the old cart's at the same pose and `r_drawentities 0`; host recorder meshlet/bind counts equal the cook's visible-set prediction; world CPU ≤ 3 ms on the dome pose; world-surfaces.md W1–W5 (rewritten cook byte-identical) |
| M2 | COLL residency, traces, area tree, pmove host + game pmove, input, view | trace fixture parity (specs/trace-and-hulls.md T1–T11) on all 10 maps; specs/pmove-host.md PM1–PM7; area-links-and-touch.md A1–A6; route `m2-walk-hldemo1.cfg` (stairs, duck crawl, ladder, drop) passes its `waitnear` checks (PM8) |
| M3 | entity system, HLE records, hydration (single layer), physics movetypes, think timing, precache, sound, studio, translucent/decals/sprites, HUD, game/ re-targeted | uplink route segment A from `waitmap hldemo1` to the elevator button `+use` with `waitmoved startele1` and the changelevel fire observed (`route: changelevel fired changetohldemo2`) — no crossing yet; specs tests for area/think/physics/precache/lightstyles/dlights/sound pass |
| M4 | streaming crossings: CWG3, approach/arm/switch, portals, twins, dehydrate/rehydrate, timers freeze/thaw, async residency classes | route through hldemo1→2→3 seams: PASS with 0 forced switches on host, `budget_over=0`, `open_fail=0`, 0 frames > 1 ms blocked I/O, pop-in only inside teleport windows; edicts continuous across seams (stats before/after) |
| M5 | full Uplink route parity (old `uplink_route.cfg` unmodified) + Hazard Course routes | PASS on host and ares, same checkpoints, same `waitnear` tolerances; Hazard `t0a0*` walk routes PASS; precache/edict peaks ≤ caps |
| M6 | performance parity | per route checkpoint, ares median fps ≥ the old cart's (same pacing build) at every `stats` step; frame-time target: no frame > 50 ms within ±2 s of any seam switch (report each miss with zones); memory: every `[INFERENCE]` row of 4.2 replaced by measured peak; real-hardware run if a flashcart is available |

---

## 18. Build and conventions

### 18.1 Build

- `Makefile` includes `$(N64_INST)/include/n64.mk` and `t3d.mk`;
  `N64_INST` = `~/sdks/n64-sdk`, libdragon replaced by the banked prefix
  `~/sdks/libdragon-banked-bc24d3d5a7d2` (its MANIFEST rev must equal the
  SDK's `libdragon_rev`; the build refuses otherwise), toolchain
  `~/sdks/mips64-toolchain`. Out-of-tree `BUILD_DIR` supported (scratch on
  `/scratch`).
- Targets: `rom` (`hl64.z64`), `host` (`hl64-host`, `HOST_M32=1` default),
  `cook` (runs `tools/` over `HL64_DATA=<player's HL install>` into
  `HL64_CONTENT`), `test-host`, `test-ares`, `provcheck`.
- Flags: `-std=c99 -O2 -Wall -Wextra -Werror` for `engine/` and `plat/`;
  `game/` may relax warnings it inherits from HLSDK style. `PROFILE=1`,
  `HL64_TEST=1` (harness, route runner, fixed dt) are the only variant
  switches; no per-feature `#ifdef` forests.
- ROM pack: `n64.mk` DFS of `HL64_CONTENT` filtered by `DFS_MAPS` globs
  (campaign graph always included); ROM size ≤ 64 MiB (flashcart limit;
  Uplink + Hazard measured ~58 MiB in the old tree).

### 18.2 Code conventions

- C99, no VLAs, no recursion deeper than the BSP depth bound (hull walks
  iterate with an explicit stack of ≤ 64 entries).
- djb-style minimalism: small modules, one obvious data path, no
  speculative abstraction, no allocation on the steady path, no
  unbounded loops over untrusted counts.
- Hostile input: every cooked file is untrusted. Loaders check magic,
  version, header size, every count against `limits.h` and against the
  bytes present, every offset + length inside its chunk, alignment, CRC
  (directory at open; chunk CRCs on the host always, on the cart in debug
  builds), string termination inside the string table, node children
  inside the node range (or a valid extern/leaf encoding), tree depth.
  Failure → `sys_error` with the file, offset and rule; never read out of
  range. The host runs the loaders under a fuzz harness (M0+).
- Big-endian cooked data is read in place on the cart; the host swaps once
  at load into the same layout.
- Floats: single precision everywhere except the clock accumulator; no
  `-ffast-math` in `coll/` and `phys/` (results must match on host and
  cart bit for bit; the host builds with `-ffp-contract=off` and SSE2, the
  cart has no FMA).
- Names: module prefix per directory (`io_`, `wld_`, `col_`, `phy_`, `ent_`,
  `snd_`, `ren_`, `in_`, `con_`, `ui_`, `sav_`, `rt_`, `prof_`, `sys_`).
  Engine code does not use Quake/GoldSrc engine identifiers (`SV_*`,
  `CL_*`, `R_*`, `edict_t`, `progs`) — provcheck flags them.
- Every file starts with its licence line (MIT; HLSDK files carry the SDK
  notice) and a one-paragraph purpose comment.

---

## 19. Decisions for the user

Each has a recommendation; work proceeds on the recommendation unless the
user overrules. D9 blocks the M0 imports; D1, D2 and D3 block M2/M3
design work; the rest can wait until the milestone named.

| # | decision | options | recommendation |
|---|---|---|---|
| D1 | Entity representation (M0 headers) | (a) compact engine core record ≈ 256 B + per-class game private data, handles and function ids, HLSDK field names (6.1); (b) HLSDK `edict_t` + `entvars_t` layout verbatim (≈ 800 B per slot, layout confined to `game/`, engine re-declares what it reads) | (a): ≈ 270 KB less RDRAM at the 495 cap, exact host/cart sizes, trivially serialisable; the game is C, so (b)'s source compatibility buys little |
| D2 | Player movement (M2) | (a) HLSDK `pm_shared` in `game/pm/` (HLSDK licence), caps cut to 64 physents / 8 move-ents / 32 touches; (b) engine movement rewritten from a new behaviour spec of the oracle (Quake movement + GoldSrc patches), MIT | (a): it *is* GoldSrc player movement, allowed by PROVENANCE, no spec/rewrite risk; route radii absorb the differences from the oracle (pmove-host.md 6). Cost: walked route legs may need radius review |
| D3 | Studio animation runtime (M3) | (a) bone setup from HLSDK `StudioModelRenderer` in `game/studio/` (HLSDK licence), reading MDL animation data from the studio pool as the old tree does; (b) cook animations into our own per-sequence format (SA2 §3.6 per-sequence chunks) and write MIT bone code from that format; ≈ 60 KB per NPC model vs 93–142 KB | (a) for M3 (fast, allowed, removes the Xash-derived `studio_geom.c`), (b) as an M6 memory option if the studio pool is the bottleneck. Either way the old `studio_geom.c` bone code is not used |
| D4 | Saved games (after M5) | (a) none — campaign state lives only in the session; (b) FlashRAM 128 KB (flashcarts and ares support it): 1 slot + quick save; (c) SRAM 32 KB; (d) Controller Pak 32 KB | (b): the estimated 40–90 KB save (14) does not fit (c)/(d); defer the format until M5 fixes the record layouts |
| D5 | Host renderer output (M1) | (a) draw-list recorder only (counts, hashes); pixels only from ares; (b) also a small software rasteriser on the host for PPM screenshots | (a); add (b) only if ares turnaround blocks M1/M3 iteration |
| D6 | Where the oracle differs from documented GoldSrc | (a) oracle-exact through M5 (route parity), then GoldSrc-faithful changes behind cvars one by one; (b) GoldSrc-faithful from the start | (a). Known differences: allsolid trace fraction, box-only trigger shapes, bbox (not hitbox) bullet traces, BOUNCE without friction/gravity, model light not following animated styles, sentence `(s)`/`(t)` modifiers, conveyors handled in game |
| D7 | Console requirement | (a) Expansion Pak (8 MB) required; (b) also 4 MB | (a): the studio pool alone is 1.5 MB; the old tree required 8 MB for HL content too |
| D8 | Imported file history | (a) copy CLEAN files with a commit note naming the audit entry (PROVENANCE.md); (b) subtree-import with filtered history | (a): history of those files is interleaved with Quake-derived commits |
| D9 | Admit QUAKE-INTERFACE files (our code written against Quake engine interfaces) — blocks M0 imports | (a) amend PROVENANCE.md: such files may be imported when the same commit retargets every Quake binding the audit lists and rewrites the audit's flagged ranges from specs; provcheck verifies no Quake identifier survives; (b) keep CLEAN-only: rewrite all of them from new behaviour specs | (a): the audit found no copied Quake logic in them (similarity and comment-overlap scans), only interface use; (b) adds ≈ 8 more specs and ≈ 20k lines of rewrite (w64_stream 2.3k, q64_world 1.5k, hl_ents 4.3k, hl_levelstate 2.4k, snd_n64 1.9k, mod_studio 2.2k, r64_sector 2.1k, r64_studio + r64_studio_bake 3k, q64_route 0.5k …) for no provenance gain the audit can measure |

