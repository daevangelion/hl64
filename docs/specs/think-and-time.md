# Game clock, think timing, active sets, entity lifetime

Behaviour: when thinks run and what time they see, as in GoldSrc, to the
frame. Mechanism: a GoldSrc-shaped game clock (double seconds, float
`nextthink`), a think heap and active sets instead of a per-frame scan of
every entity. Conventions and oracles: [README.md](README.md); frame
order: architecture.md 3.2. GoldSrc probe captures: `$HL64_FIXTURES/
goldsrc/think-and-time/K-*` (Oracle2, hl64 `d2bf3d8`, 10 maps, frame time
0.01 and 1/30).

## 1. Clock

The frame on which a think runs depends on float rounding, so the game
clock keeps GoldSrc's number formats; integer microseconds remain only for
engine-internal timing that no game code observes (I/O, pacing,
residency, profiling).

- `time`: **f64 seconds**, advanced by adding `dt` (f64) once per frame,
  starting at **1.0** at New Game **[probe:measured K-settle]**. The game
  reads it as f32 (HLSDK `gpGlobals->time`).
- `dt` (frame time): f64 seconds = elapsed vblanks × VI period, clamped to
  [1/60, 0.1] s on the cart (D6 i; GoldSrc allows 0.25 s — only frames
  under 10 fps differ); `host_fixed_dt` in test builds (1/30 s; 0.01 s
  when replaying probe fixtures) so host and cart advance identically.
- `nextthink` and pusher `ltime`: **f32**, exactly the values the game
  wrote (HLSDK entvars are float).
- Every engine→game call sees the frame's time, except a think or a logic
  timer callback, which sees its own due time (2, 5). `time` advances by
  `dt` after the active-set pass.
- No clock jump at seams; the clock is saved.

## 2. Think rule (every movetype except PUSH)

An entity thinks at most once per frame, at the point of its movetype step
(physics-movetypes.md); monster thinks may be held or continued at the
frame start instead (2.1):

- not due if nextthink ≤ 0, or if `(f64) nextthink > time + dt`
  **evaluated in double**, as GoldSrc evaluates it;
- due otherwise: nextthink := 0, then the think runs with game time =
  `(f32) max(nextthink, time)`;
- a think that schedules itself due again runs next frame (one think per
  frame); a lower-index entity that sets another entity's nextthink to a
  value due this frame makes it think **this** frame
  **[probe:measured K-think]**;
- freeing the entity ends its step.

Rounding is observable and reproduced, not smoothed: game code writes
`nextthink = time + delay` in float, so `float(t + ft)` set by a
lower-index entity ran the **next** frame at frame time 0.01 (the float sum
rounded above `t + ft` in double) and the **same** frame at 1/30 (rounded
below) **[probe:measured K-think]**. Scheduled times are kept, not rounded
to frames (a think rescheduling itself at +0.12 s with dt 1/30 runs at
frames 0, 3, 7, 10, 14 and sees t0 + 0.12 k in float sums)
**[probe:measured K-think]**; a think whose due time passed during a long
frame sees the frame time **[probe:measured K-think]**.

### 2.1 Monster thinks spread over frames (M5, `game/hl/aithink.c`)

HLSDK `CBaseMonster::MonsterThink` decides as in GoldSrc; what hl64 adds
is *when* its work runs, so that no frame carries more monster AI than a
budget. Budgets count work units, never time, so host and cart pause and
hold at the same points (K9, K10).

- **Work units** (`aiw_units`, ≈ 1 µs of cart CPU each): 340 per think
  begun, 580 per RunAI sensing run (Look / Listen / GetEnemy), 5 per Look
  box candidate, 160 per Look FVisible, 50 per collision trace, 2 per BSP
  node a trace visits, 16 per entity candidate of a move's box, 140 per
  `CheckLocalMove`, 175 per walk step, 760 per FindNearestNode search, 23
  per Dijkstra node expanded, 1150 per anim event, 1040 per sound or
  sentence started, 580 per decal, 55 per bone of a server pose computed,
  70 per server pose begun (its frame, controllers, root transform), 26
  per hitbox cull test, 62 per hitbox a line trace walks. A server pose
  computes only the bones asked for: a hitbox trace the chains of the
  hitboxes its line can meet (a bone's subtree is skipped when the line
  passes beyond the subtree's bound from the bone's parent;
  trace-and-hulls.md 4.5), an attachment or bone position its bone's
  chain; the game's memo keeps each pose's computed bones (4 poses), so a
  bone is computed once per pose (memo misses; the renderer never fills
  the game's memo, so the count is the same on every machine). Fitted to
  the PROFILE `ai: frame` lines (the cart's own counts) of ares runs of
  the 17 `test/scenes` scenes (4043 frames; non-negative least squares,
  r² 0.985, rms 0.9 ms), rounded up ~4 %: cart µs / units p50 0.96, p90
  1.06, p99 1.20 over the frames above 10000 units. The frame's budget is
  setting `ai_budget` (default `HL64_AI_BUDGET` 18000).
- **Contexts**: with `ai_spread 1` every monster think runs on one of 6
  cooperative contexts (own stack, own cover-search tables). A think
  nested in a think runs inline; thinks before game time 2.5 s run whole.
- **Pause points**: the start of each `CheckLocalMove` and each of its
  walk steps after the first (paused, the monster stands where it stood
  before the check: origin linked, flags, ground entity; the walk
  continues from its step), each cover candidate (FindCover /
  BuildNearestRoute), each FindNearestNode trace (a search whose heap
  another search used meanwhile rebuilds it from the candidates after
  the one about to be traced: the same order), each Look FVisible (a
  candidate freed or dead meanwhile is not seen) and each anim event
  (the event read afresh; the sequence must still be on). Never inside
  the Dijkstra search or an event's handler. At a pause point a think
  yields once the frame's units plus the point's reserve (an anim event:
  `AI_EVENT_RESERVE` 8000; else 0) reach the budget, except at its
  slice's first point. The CheckLocalMove memo scope, sight list and
  `gpGlobals` vectors are saved per think.
- **Per-frame bound**: paused thinks continue at the next frame start,
  oldest first; the oldest always runs (every frame it progresses at
  least one item), each later one only while the frame's units plus its
  point's reserve are under the budget. So a frame's think work is the
  budget plus one item per slice that ran past it: one walk step, trace,
  FVisible or cover candidate (≤ ~1 ms), or one anim event started under
  budget − reserve (a firing event: ~2–12 ms on the cart; its traces
  pose only the hitbox chains their lines can meet, ~40 bones for 3–4
  studio entities); plus the start
  of each think begun over the budget (below), up to its second pause
  point. A think still paused 2 s after it began is counted (`overruns`:
  demand over the budget for that long), not forced.
- **Held thinks**: a monster think dispatched once the frame's units have
  reached the budget, or while older thinks are queued, does not start:
  it joins a FIFO queue (≤ 64) and its nextthink stays due (its due time),
  so a dehydrate, twin copy or New Game sees an ordinary due think. At
  the next frame start, after the paused thinks' slices, queued thinks
  start oldest first, as the engine dispatches a think (nextthink := 0,
  the frame's time), while the frame's units are under the budget and a
  context is free. **Lateness bound**: over the budget, a think queued
  for `ai_hold` (`HL64_AI_HOLD` 0.2 s) of game time still starts, at most
  `HL64_AI_AGED_PER_FRAME` (2) such starts a frame, oldest first, so a
  think k-th in the queue starts at most `ai_hold` + ⌈k / 2⌉ frames
  (≤ 0.2 s + 32 frames) after it came due while a context is free; with
  all 6 contexts holding paused thinks it waits for one. The engine's
  dispatch of a queued think is absorbed.
- **Drops**: a paused think ends (its half-built route cleared) when its
  monster is freed, killed or changes layer, or its think function,
  schedule, task, monster state, enemy or target is changed by something
  else, at a map open / close, dehydrate, twin copy and New Game; paused
  at an anim event (MonsterThink's execution half, which reads the
  monster afresh) only the freeing, death, layer or think function
  change end it. A queued think leaves the queue unstarted when its
  entity is freed, its think function or nextthink is changed by
  something else, at a dehydrate / twin copy, map open / close, New Game
  or with `ai_spread` ≠ 1; the queue full: it starts at once.
- **Equivalence**: `ai_spread 2` (test) pauses at every point and
  continues at once after scribbling the shared scratch; its decision
  trace equals whole thinks (`ai_spread 0`) on routes m4, uplink and
  hazard. With the budget, decisions are HLSDK's for the world as it is
  when each slice runs.

## 3. Active sets (mechanism)

- **Think heap**: 4-ary min-heap of (nextthink f32 bit pattern, entity
  index), ≤ 512 entries (positive floats order like their bit patterns);
  setting or clearing nextthink updates the entry. Popping uses the same
  double comparison as rule 2, so the heap decides exactly the frames
  GoldSrc decides.
- **Movers bitset** (512 bits): set while the entity's movetype has motion
  to do (velocity or avelocity non-zero, falling types not on ground,
  BOUNCE sliding on the ground, pushers with velocity/avelocity, FOLLOW,
  PUSHSTEP not at rest); engine setters for velocity, flags, origin,
  groundentity and movetype keep it current; an entity leaves the set when
  its step ends at rest.
- Per frame: pop every due heap entry into a **due** bitset; walk
  `due | movers` word by word in ascending index and run each entity's
  movetype step. An entity created or made due during the walk at a higher
  index joins this frame, as GoldSrc's ascending-index loop would visit it
  (rule 2, lower-index case).
- Equivalence with GoldSrc's "visit every entity" loop: an entity that is
  neither due nor moving does nothing in its step; skipping it changes
  nothing (K9 checks this on full routes).

## 4. Pusher think
Pushers think on their local clock (physics-movetypes.md 4.1): due when
nextthink lies in (ltime before the move, ltime after]; the think sees the
frame time; a blocked pusher's ltime stalls and its think does not run.
Doors, plats and trains schedule `ltime + travel time` (HLSDK
`CBaseToggle::LinearMove`); a door's ltime ends exactly at its nextthink
(0.78125) **[probe:measured K-pusher]**.

## 5. Logic timers
Edict-less records (multi_manager steps, delayed fires, logic thinks;
architecture.md 6.4) are HLSDK entity thinks in GoldSrc, and their timing
is that of HLSDK's float arithmetic. hl64 reproduces it exactly:

- each timer stores the f32 `nextthink` the HLSDK code would have written
  and becomes due by rule 2 (double compare against `time + dt`);
- the callback runs the HLSDK comparison itself — a multi_manager fires a
  target when `(f32)(time − start) ≥ delay` in float at one of its think
  times, then schedules `start + next delay` as f32. Measured: a 0.33 s
  target fired at +0.34 s at frame time 0.01 (at the +0.33 think the float
  difference was just under 0.33) and at +0.333 s at 1/30
  **[probe:measured K-think]**. Timers must not be "simplified" to exact
  due times;
- due timers run at `game_start_frame` in due order (ties: insertion
  order), seeing `(f32) max(due, time)`; timers scheduled by a callback that
  are already due run in the same pass (≤ 1024 callbacks per frame, logged
  if reached);
- a closing map's timers freeze by remaining time (activator dropped) and
  are re-queued at time + remaining when it reopens (logic thinks and
  multi_manager steps when their record rehydrates);
- 128 entries; a full heap is fatal, naming map and record.

## 6. Entity lifetime

- Slot 0 world, slot 1 player, game entities from slot 2; cap 495.
- **Allocate**: lowest free slot ≥ 2 that was freed before time 2.0 s or at
  least 0.5 s ago; else the next never-used slot **[probe:measured
  K-alloc]** (90/90 allocations). The new entity's components are zeroed
  and a creation serial assigned (area-links-and-touch.md 3.1); the slot's
  serial is unchanged (it was incremented when the slot was freed): a
  reused slot reports the serial and free time of its last free
  **[probe:measured K-alloc]**. Slots taken by map entities between
  probe allocations follow the same rule (hldemo2: 5 of 5).
- Reserves: optional hydration only while ≥ 48 slots are free; transient
  effects (gibs, debris, shells) only while ≥ 32 are free (otherwise
  skipped); game allocations may use every slot; exhaustion is fatal with a
  class census.
- **Free**: unlink, remove from the heap and active sets, zero components
  (keep serial and free time), serial += 1; free time = the frame's time
  (f32). References held elsewhere are validated through handles.
  `FL_KILLME` (HLSDK `UTIL_Remove`) frees the entity when the active-set
  pass reaches its slot, after its step (set at StartFrame: freed in that
  frame's pass with that frame's time **[probe:measured K-alloc]**).
- **New Game settle**: after the start map's records spawn and precache at
  time 1.0, the first two frames run `game_start_frame` and the active-set
  pass with **time 1.0 and frame time 0** (not 0.1): nothing moves under
  gravity in them, thinks due at ≤ 1.0 run **[probe:measured K-settle]**.
  The clock starts advancing on the third frame. Hydration later gets no
  settle frames.

## 7. Tests

| # | property | data |
|---|---|---|
| K1 | +0.12 s self-rescheduling think at dt 1/30: frames 0, 3, 7, 10, 14; times t0 + 0.12 k (f32 sums) | **[probe:measured K-think]** + host |
| K2 | own think → next frame; set by a lower-index entity → this frame when due in the double compare; `float(t + ft)` set by a lower-index entity runs next frame at dt 0.01 and this frame at 1/30; nextthink ≤ 0 never | **[probe:measured K-think]** |
| K3 | a think whose due time passed during a long frame sees the frame time | **[probe:measured K-think]** |
| K4 | pusher thinks on ltime (door ltime = nextthink 0.78125 on arrival); blocked pusher's ltime stalls, no think | **[probe:measured K-pusher]** |
| K5 | logic timers reproduce HLSDK float arithmetic: multi_manager 0.33 s target at +0.34 (dt 0.01) and +0.333 (dt 1/30); order, cascade, time seen; freeze/thaw keep remaining time | **[probe:measured K-think]** + synthetic (freeze/thaw) |
| K6 | allocation: reuse delay, lowest index, serial increments | **[probe:measured K-alloc]** (90/90) |
| K7 | reserves: hydration stops at 48 free, transients at 32 | synthetic |
| K8 | settle: the first two frames after New Game have time 1.0 and frame time 0; the clock advances from the third | **[probe:measured K-settle]** |
| K9 | active set: identical results with a debug mode that steps every live entity every frame (route checksum of all components at each stats step) | host route |
| K10 | host and cart clocks bit-identical on a fixed-dt route | route stats |

Host runner: `test/m3/ent.py --bin BUILD/host/hl64-ent` (`make enttest`)
replays K-think scenarios 0–7 (K1–K3; 8–9 are K5), K-alloc (K6, slot
indices compressed in order to fit the 495 cap) and K-settle (K8) on the
real engine/ent code, and runs K7, K9 (a randomised NONE / NOCLIP /
FOLLOW world with thinks, spawns, removes and cross-entity nextthinks,
checksummed every frame with and without `phy_set_step_all`, plus a
control that bypasses the setters and must differ), the heap / active-set
unit test and P13's resting counters. K9 on full game routes waits for the
game's entities (M3 gate route).
