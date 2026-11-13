# Game clock, think timing, entity lifetime

Conventions: [README.md](README.md). Frame order: architecture.md 3.2.

## 1. Clock

- `time` (game seconds, f32 visible to the game; the engine accumulates in
  f64 and converts once per frame) starts at **1.0** at New Game.
- `dt` per frame: VI-derived and clamped to [1/60, 0.1] s on the cart
  (frames below 10 fps slow the game, as the oracle does; GoldSrc clamps at
  0.25), or fixed (`host_fixed_dt`, default 1/30 in test builds) so host
  and cart routes advance identically.
- During a frame every engine→game call sees `time` = the frame's time,
  except inside a think or timer callback, which sees its own due time
  (2, 4). `time` advances by `dt` after the entity loop (architecture.md 3.2
  step 6), before streaming, sound and rendering.
- The clock never jumps at seams (one world, no level loads) and is part
  of a saved game.

## 2. Think (every movetype except PUSH)

An entity's think runs at most once per frame, at the point of its
movetype step given in physics-movetypes.md (before motion for the toss
family, after the fall for STEP/PUSHSTEP, alone for NONE/NOCLIP):

- not due if `nextthink ≤ 0` or `nextthink > time + dt`;
- due otherwise: `nextthink := 0`, then the think function runs with the
  game's time = max(nextthink, time) — so a think due in the middle of this
  frame's interval runs this frame and sees its scheduled time, and a
  think whose time is in the past (scheduled before a long frame) sees the
  frame time;
- a think that sets `nextthink ≤ time + dt` again runs next frame, not
  again this frame;
- if the think frees the entity, its movetype step ends.

Consequence: a think runs in the first frame whose interval
`[time, time + dt]` reaches its due time, which can be the frame *before*
the clock passes it; inside the think the game sees the scheduled time
(which may be ahead of the frame time). Scheduled times are kept, not
rounded to frames: a think that reschedules itself at `time + 0.12` with
dt = 1/30 from frame 0 runs at frames 0, 3, 7, 10, … and sees times
t0, t0 + 0.12, t0 + 0.24, t0 + 0.36 (f32 sums of the scheduled values).

## 3. Pusher think

MOVETYPE_PUSH entities think on their local clock (physics-movetypes.md
4.1): due when `nextthink` lies in `(ltime before the move, ltime after
the move]`; the think sees the frame's `time`. A blocked pusher's `ltime`
does not advance, so its think is postponed while blocked. Doors, plats
and trains schedule `nextthink = ltime + travel time` (HLSDK
`CBaseToggle::LinearMove`).

## 4. Logic timers

Edict-less records (multi_manager steps, delayed fires of CBaseDelay
`delay` keys, logic thinks: architecture.md 6.3) live in one timer queue of
128 entries.

- At `game_start_frame` the queue runs every timer whose due time is
  ≤ time + dt, in increasing due time (ties: the earlier inserted first),
  each seeing game time = max(due, time). Timers scheduled by a callback
  that are themselves due run in the same pass (bounded to 1024 callbacks
  per frame; reaching the bound is logged).
- Timers of a map that closes are frozen with their remaining time
  (due − time) and their activator dropped; when the map reopens they are
  re-queued at time + remaining (logic thinks and multi_manager steps only
  when their record rehydrates).
- A full queue is a fatal error naming the map and record (the cap was
  sized from measured peaks; GoldSrc has no cap).

## 5. Entity lifetime

- Slot 0 is the world; slot 1 the player; game entities from slot 2.
- **Allocate**: the lowest-index free slot ≥ 2 whose free time is either
  < 2.0 (freed during the first second of the game) or at least 0.5 s
  before the current time; otherwise a new slot at the end of the used
  range. A new slot's record is zeroed (all fields 0, handles none,
  `gravity` 0 meaning 1), serial incremented.
- Reserves: optional hydration allocates only while ≥ 48 slots are free;
  transient effects (gibs, debris, shells) only while ≥ 32 are free and
  otherwise get none (the effect is skipped); game allocations may use
  every slot; exhausting the table is a fatal error with a census
  (classes and counts) on the log.
- **Free**: unlink; clear the record (keeping serial and free time);
  free time := time; serial += 1; owner, enemy and other references held
  by other entities are left for the game to validate (handles carry the
  serial). A map record's state becomes killed or dry as the game says
  (architecture.md 6.3).
- A free entity is never touched, thought, drawn or traced.
- **New Game settle**: after the start map's records are spawned, two
  simulation frames with dt = 0.1 run before the first presented frame
  (so items and monsters drop to the floor and triggers initialise).
  Hydration later in the game gets no settle frames.

## 6. Tests

| # | property | data |
|---|---|---|
| K1 | a think rescheduling itself at +0.12 s from frame 0 with dt = 1/30 runs at frames 0, 3, 7, 10, 14 … and sees times t0 + 0.12k (f32 sums) | synthetic |
| K2 | `nextthink = time` set inside the entity's own think runs next frame (one think per frame); set earlier in the frame by a lower-index entity it runs this frame; `nextthink ≤ 0` never runs; `nextthink = time + dt` is due this frame (boundary inclusive) | synthetic |
| K3 | a think whose due time passed during a 0.1 s frame sees the frame time, not the due time | synthetic with a 0.1 s frame |
| K4 | pusher: think fires on the frame `ltime` passes nextthink; a blocked pusher's think is postponed until unblocked | synthetic |
| K5 | timers: due-time order, same-frame cascade, time seen = max(due, time); freeze on close and thaw on reopen preserve remaining time to the frame | synthetic + fixture K-think: the oracle's multi_manager fire times along the Uplink route (map, record, fire time) |
| K6 | allocation: freed slot not reused for 0.5 s (except slots freed before time 2.0); lowest index wins; serial increments | synthetic |
| K7 | reserves: hydration stops at 48 free, transients at 32, game allocations succeed to the cap | synthetic |
| K8 | New Game settle: two 0.1 s frames run before frame 1; an item spawned 10 u above a floor is on the floor at frame 1 | synthetic + fixture (hldemo1 item origins after settle) |
| K9 | clock: `time` = 1.0 + Σdt; identical on host and cart for a fixed-dt route (stats `time=` field compared) | route |
