# Sound channels, attenuation, sentences

Conventions: [README.md](README.md). Backend: libdragon mixer + wav64
(architecture.md 10). Constants from HLSDK `const.h`: channels AUTO 0,
WEAPON 1, VOICE 2, ITEM 3, BODY 4, STREAM 5, STATIC 6; attenuation NONE 0,
NORM 0.8, IDLE 2, STATIC 1.25; flags STOP 1<<5, CHANGE_VOL 1<<6,
CHANGE_PITCH 1<<7, SPAWNING 1<<8; pitch NORM 100.

## 1. Emitting

`emit(entity, channel, sound, volume 0…1, attenuation, flags, pitch 1…255)`:

- STOP: stop every voice and loop of (entity, channel); nothing starts.
- CHANGE_VOL / CHANGE_PITCH: update volume / pitch of the voice and loop
  slots playing on (entity, channel) (any sample); nothing starts.
- Otherwise, if channel ≠ AUTO: every voice and loop of (entity, channel)
  stops first (one sound per entity channel). AUTO never replaces.
- A sample whose wav64 has a loop region is a **loop** (2); otherwise a
  **one-shot**: if spatialised to silence at the start (1.2) it is dropped,
  else it takes a voice (1.1).
- The source origin is the entity's origin at emit time (the player's
  sounds instead follow the listener every frame); a one-shot does not
  follow a moving entity. `emit_ambient(point, …)` has no entity.
- Volume and pitch are clamped to their ranges; pitch maps to playback
  rate = sample rate × pitch / 100, capped at 130 % of 22050 Hz (voice
  buffers are sized for that cap).

### 1.1 Voice selection (16 voices)

1. A voice that is idle (not playing, not between two sentence words).
2. Else steal a playing one-shot: the one whose sample was least recently
   started; a voice held by a loop is stolen only if every voice is a loop
   (the loop goes virtual).

### 1.2 Spatialisation (each frame for every voice, and at start)

Listener = view origin and view right vector. For a source at distance d
with attenuation a > 0 (and not the listener's own sounds):
`gain = clamp(1 − d·a/1000, 0, 1) × volume × master volume`; with
`s = (source − listener)/d · right` (0 when d ≤ 1),
`pan = clamp(0.5 + 0.5·s, 0, 1)`, left = gain·(1 − pan), right = gain·pan.
Attenuation 0 or the listener's own sounds: left = right = volume × master.
(1000 u is the nominal clip distance: ATTN_NORM is silent at 1250 u,
ATTN_IDLE at 500 u, ATTN_STATIC at 800 u.)

## 2. Loops and ambients

- Up to 32 loop slots (entity sounds with loop regions, `ambient_generic`
  looping sounds, `emit_ambient` loops). A slot holds a voice only while
  its spatialised volume sum ≥ 0.002; out of range it is **virtual** (no
  voice, wave may be closed) and restarts from its loop start when it
  becomes audible again. Loops only take idle voices; a one-shot may steal
  a loop's voice (it goes virtual and returns on a later frame).
- A full loop table drops the new loop and logs it.

## 3. Sample data

- One open wav64 per active sample, at most 10 open (LRU close of
  samples no voice uses; a wave is never closed under a voice: stop the
  voice, wait for the RSP, then close).
- Backend name table: 359 names (precache-and-resources.md); a retired
  name keeps its slot until its voice or loop releases it; a full table is
  fatal on cart and host alike.
- Opening a sample on the cart is a DMA read of its header; the first
  audio buffers stream through the `IO_AUDIO` class.

## 4. Sentences

- `sound/sentences.txt` (player data) is cooked into a table: sentence
  name → group, index, list of words with per-word modifiers. Group names
  (`!HG_ALERT`) pick a random member (game logic: HLSDK
  `SENTENCEG_PlayRndSz`, with its no-repeat lists, lives in game/).
- A sentence plays word after word on one voice; the next word starts on
  the first frame after the previous ends (≤ one frame of silence).
  Modifiers per HLSDK VOX syntax: `(p<n>)` pitch, `(v<n>)` volume, `(e<n>)`
  end at n % of the word; `(s<n>)` start and `(t<n>)` time compression are
  ignored (no mixer support; accepted difference).
- Words that are not precached sounds use 24 transient word slots outside
  the name table; the least recently used idle one is recycled.
- Stopping the sentence's (entity, channel) stops the remaining words.

## 5. Tests

| # | property | data |
|---|---|---|
| S1 | channel replacement: two emits on (e, VOICE) leave one voice; two on (e, AUTO) leave two; STOP silences loops too; CHANGE_PITCH changes the rate without restarting | host null-audio backend (records voice events) |
| S2 | stealing: 17 one-shots → the least recently started is stolen; with 16 loops audible a one-shot steals a loop voice, the loop resumes later | host |
| S3 | gain/pan formula for points on a circle around the listener at d = 0, 500, 1000, 1250 and each attenuation; silent starts are dropped | self-contained |
| S4 | loops go virtual beyond audibility and restart from the loop start | host |
| S5 | pitch mapping and the 130 % cap | self-contained |
| S6 | sentences: word order, (p)(v)(e) applied, gap ≤ one frame, stop mid-sentence | host + fixture (oracle sentence word start frames on the dome scene) |
| S7 | Uplink route: the sequence of (frame, entity class, channel, sample) emits matches the oracle's sound log up to AI randomness (scripted scenes exact) | fixture S-route |
| S8 | no wav64 is closed while a voice owns it; ≤ 10 open; name table peak ≤ 359 on both routes | host + cart stats |
