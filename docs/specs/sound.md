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
  class. A sound started with `SND_RESIDENT` [own] (the sparks'
  ricochets, up to 10 a second) is read whole at open and held in the
  SOUND region (VADPCM, when the region has room; else streamed): no
  cart reads per start; streamed waves close before resident ones.
- The held samples are a cache [own]: an open that finds no room closes
  a resident wave (the least recently used no voice plays, else the least
  recently used one, its voices stopped) and retries, so the cache never
  fails an open while it holds bytes; the SOUND region rule (architecture
  4.1) takes the want peak (the region in use without the cache, `prof
  snd_us want_peak=`), mem_peak is information. A closed resident wave's
  per-voice decoder states leave the region with it.

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
| S9 | CD music: hldemo1 start plays track 3, hldemo3 dome_cd_track track 2, `cd stop`, a cut, the end of a track, the volume setting, the session end | host event log (test/m6/music_host.sh) + cart scene |
| S10 | resident samples are a cache: a streamed open into a full cache (idle, then playing) evicts one cached wave and plays, nothing dropped; the want peak leaves the cache out; with no cache the open fails (control) | host (room model in plat/host/audio_host.c) |

## 6. CD music

- Behaviour (HLSDK dlls/triggers.cpp): `trigger_cdaudio` (brush; the
  player's touch or a use) and `target_cdaudio` (point; a use, or every
  0.5 s from 1 s after spawn while the player is within its `radius` key,
  `pev->scale`) call PlayCDTrack(`(int)health`) once and remove
  themselves. Track −1 = the client's `cd stop`; N < −1 or N > 30 = "out
  of range", nothing; otherwise `cd play N`: N plays once (not looped).
- Track N → the player's `valve/media/` file (documentation, no engine
  source: TWHL "Tutorial: Making Music Play in Maps" track table; VDC
  "List of Half-Life 1 music"): 2 Half-Life01, 3 Prospero01, 4
  Half-Life12, 5 Half-Life07, 6 Half-Life10, 7 Suspense01, 8 Suspense03,
  9 Half-Life09, 10 Half-Life02, 11 Half-Life13, 12 Half-Life04, 13
  Half-Life15, 14 Half-Life14, 15 Half-Life16, 16 Suspense02, 17
  Half-Life03, 18 Half-Life08, 19 Prospero02, 20 Half-Life05, 21
  Prospero04, 22 Half-Life11, 23 Half-Life06, 24 Prospero03, 25
  Half-Life17, 26 Prospero05, 27 Suspense05, 28 Suspense07, 29
  gamestartup (played by the client at launch; not in retail
  `valve/media`); 1 (data track) and 30 have no file. Uplink plays 3
  (hldemo1 `start_cd_audio` at hldemo_init_mm + 0 s, `end_cd_trigger_1`
  at trigger_once \*149) and 2 (hldemo3 `dome_cd_track`,
  dish_mm + 1.5 s); the Hazard Course none.
- A play cuts the playing track. A track plays on through level
  changes, loads (the client's music is not part of a saved game;
  **[retail: music across a load]**) and the pause menu; it stops at its
  end, at `cd stop` and when the session ends (New Game, end of section,
  Quit to Title). Volume = master × `vol_music` (the Configuration
  screen's Music Volume, GoldSrc's MP3 volume), separate from the sounds'
  `vol_sfx`.
- Music level (default `vol_music` 100): GoldSrc's defaults are
  `MP3Volume` 0.8 (public cvar lists: txdv.github.io/cstrike-cvarlist,
  gsu.site.nfoservers.com cvar list) and `volume` 0.7 (retail
  `valve/default.cfg`), so a track plays 20·log10(0.8/0.7) = 1.2 dB over
  a full-volume sound **[INFERENCE: Miles applies MP3Volume as a linear
  gain; not measured]**. Here a sound at full volume and the music at
  `vol_music` 100 both play at gain 1 (× master × `vol_sfx` /
  `vol_music`), so 100 (the slider's top) is the nearest: 1.2 dB
  quieter against the sounds than GoldSrc; `vol_sfx` 88 would match.
  Levels (active RMS, 50 ms windows above −50 dBFS, per ear): the 26 retail
  tracks the maps play median −15.8 dBFS (−25.0 Prospero04 … −12.5
  Half-Life03; peaks 0 dBFS), the 1050 cooked sounds median −14.1 dBFS
  (scientist speech −14.1, barney −18.3, HEV −15.8, weapons −12.5,
  footsteps −19.1, monsters −7.0); the cart's VADPCM keeps the level
  (Half-Life01 −17.73 / −17.75 dB, Prospero01 −16.64 / −16.64 dB,
  reference / cart clip). So at the defaults the music sits at speech
  level, as in GoldSrc.
- Campaign size (retail `valve/maps`, every trigger_cdaudio /
  target_cdaudio): 26 tracks with a file, 34.9 min (the campaign maps 25
  tracks, 32.7 min; Uplink 2 tracks, 4.4 min; t0a0 none; track 5
  Half-Life07 is never played). VADPCM is 9 B (4-bit) or 7 B (3-bit) per
  16 samples per channel: 4-bit stereo 22050 Hz 1.42 MiB/min (campaign
  46.4 MiB), 4-bit mono 23.2 MiB, 3-bit mono 18.1 MiB, 4-bit stereo
  16 kHz 33.7 MiB, 4-bit mono 16 kHz 16.9 MiB, 3-bit mono 16 kHz
  13.1 MiB.
- Mechanism: the cook converts only the tracks the cooked maps' entities
  name (`tools/cook/hl_music.py`): `media/cdNN.wav64`, 22050 Hz stereo
  VADPCM 4-bit without Huffman (1.49 MB of ROM per minute). The music has
  its own mixer voice (channels 16 and 17: a stereo VADPCM ring pair,
  sized at boot on the first track) and stream slot, never a sound voice
  or wave; its reads are AUDIO-class, ahead of the queue (≈ 25 KB/s, 2 KB
  pieces).
