# Sound: channels, attenuation, stereo, sentences

**Oracle status: awaiting a retail-client capture plan.** Mixing,
attenuation and channel stealing happen in the client; the HLDS probe
sees only the emit calls. Every `[probe]` property about what is heard is
to be measured on the retail client (README "Oracles").

Behaviour: what plays, where, how loud and on which ear, as in Half-Life.
Mechanism: 16 libdragon mixer voices over cart-streamed wav64, sounds and
sentences as cooked ids (architecture.md 10). Conventions:
[README.md](README.md). HLSDK `const.h`: channels AUTO 0, WEAPON 1, VOICE
2, ITEM 3, BODY 4, STREAM 5, STATIC 6; attenuation NONE 0, NORM 0.8, IDLE
2, STATIC 1.25; flags STOP 1<<5, CHANGE_VOL 1<<6, CHANGE_PITCH 1<<7,
SPAWNING 1<<8; pitch NORM 100.

## 1. Emitting

`emit(entity, channel, res_t sound, volume 0…1, attenuation, flags, pitch
1…255)`:

- STOP: stop every voice and loop of (entity, channel); nothing starts.
- CHANGE_VOL / CHANGE_PITCH: update the (entity, channel) voice or loop
  in place; nothing starts.
- Otherwise a channel other than AUTO first stops what that entity plays on
  it (one sound per entity channel — HLSDK speech and weapon code relies on
  it); AUTO never replaces.
- A sample with a loop region (wav64 loop, from the WAV cue) loops until
  stopped; otherwise it plays once. A one-shot that would start silent
  (2) is dropped.
- Source position: the entity's origin, updated every frame while the
  sound plays (moving monsters carry their voices) **[retail S-mix]**;
  brush entities (origin at the map origin: doors, buttons, trains) sound
  from the centre of their box; the listener's own sounds are not
  spatialised. `emit_ambient(point, …)` is fixed.
- Playback rate = sample rate × pitch / 100, capped at 130 % of 22050 Hz
  (mechanism limit: voice buffers sized for the cap).

## 2. Spatialisation (behaviour)

Listener = view origin and view right vector. For a source at distance d
with attenuation a > 0: `g = max(0, 1 − d · a / 1000)` (1000 u nominal
clip distance: NORM silent at 1250 u, IDLE at 500 u, STATIC at 800 u);
with `s` = right · unit(source − listener) (0 when d ≤ 1):
left = volume · g · (1 − s), right = volume · g · (1 + s), each clamped to
1 — a centred source plays at full volume in both ears, a source to the
right is full right and silent left **[retail S-mix]**. Attenuation 0 or
the listener's own sounds: both ears = volume. Then × master volume.
(The old tree halved centred sources; that is not kept.)

## 3. Voices and loops (mechanism)

- Voice choice: an idle voice; else the playing one-shot started least
  recently; a loop's voice only when every voice holds a loop (that loop
  goes virtual).
- ≤ 32 loop slots; a loop holds a voice only while its spatialised volume
  sum ≥ 0.002, otherwise it is virtual (no voice, wave closable) and
  resumes from its loop start when audible again; a full table drops the
  new loop and logs it.
- ≤ 10 open wav64 streams, LRU close of waves no voice uses (never under a
  voice: stop, wait for the RSP, close). Wave reads use the AUDIO I/O
  class.

## 4. Sentences

- `tools/sentc` cooks the player's `sound/sentences.txt` into: sentence id
  → word list (word `res_t` + VOX modifiers pitch / volume / end %), group
  id → member range. Group picks with HLSDK's no-repeat lists are game
  logic (`SENTENCEG_*` in game/).
- Words play in sequence on one voice; the next starts on the first frame
  after the previous ends (≤ one frame gap). `(p)`, `(v)`, `(e)` apply;
  `(s)` start offset and `(t)` time compression are not supported by the
  mixer (accepted difference, logged by the cook when used).
- Stopping the sentence's (entity, channel) stops the remaining words.

## 5. Tests

| # | property | data |
|---|---|---|
| S1 | channel replacement, AUTO stacking, STOP of loops, CHANGE_PITCH without restart | host null-audio event log |
| S2 | voice stealing order with 17 one-shots and with 16 loops | host |
| S3 | ear gains for sources on a circle at d = 0, 500, 1000, 1250 for each attenuation | self-contained + retail S-mix (recorded stereo levels) |
| S4 | moving source: a walking scientist's speech pans as he crosses the view | retail S-mix + host |
| S5 | loops virtualise beyond audibility and resume from the loop start | host |
| S6 | pitch mapping and cap | self-contained |
| S7 | sentence word order, modifiers, ≤ one frame gap, stop mid-sentence | host + retail capture of the dome scene |
| S8 | ≤ 10 open waves, none closed under a voice | host + cart stats |
