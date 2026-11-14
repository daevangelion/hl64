# Cooked studio models — `.s64` model, `.a64` animation chunks, version 2

`tools/studio/studiocook.py` cooks every staged Half-Life studio model
(`models/<name>.mdl` with its `<name>t.mdl` texture file and `<name>NN.mdl`
sequence-group files) into two cart files (architecture.md 9.4, D3(b)):

- `models/<name>.s64`: the **info block** (bones, controllers, sequences,
  events, attachments, hitboxes, body parts, eye position, transition
  graph) and the **draw mesh** (bone-local meshlets for tiny3d, CI4 skins);
- `models/<name>.a64`: the **animation chunks**, one per sequence: the
  MDL's run-length animation values regrouped per sequence, each chunk
  16-aligned and individually streamable, so only the sequences in use are
  resident.

`tools/restab` makes the `.s64` the MODEL resource's file and the `.a64` the
ANIM resource of the same index (rtb-format.md). The engine never sees an
MDL struct; `game/studio` (HLSDK licence) runs the HLSDK pose maths over
these blocks.

Conventions: big-endian (cart native; the host swaps once at load), every
offset is from the start of its block, every block and chunk starts
16-aligned and its size is padded to 16 with zero bytes (whole cache lines
for PI DMA, architecture.md 4.3). `f32` is IEEE single. Strings are
NUL-terminated Latin-1 in the info block's string pool; string offset 0 is
the empty string.

## `.s64` file

| off | field | rule |
|---:|---|---|
| 0 | `magic` u32 | `0x5336344D` (`S64M`) |
| 4 | `version` u16 | 2 (version 1: no texture carried the additive ramp) |
| 6 | `flags` u16 | bit 0 CHROME (a texture has STUDIO_NF_CHROME), bit 1 VIEW (`v_*.mdl`) |
| 8 | `info_ofs` u32 | 64 |
| 12 | `info_size` u32 | padded to 16 |
| 16 | `mesh_ofs` u32 | `info_ofs + info_size` |
| 20 | `mesh_size` u32 | padded to 16 |
| 24 | `anim_size` u32 | size of the `.a64` file |
| 28 | `anim_crc` u32 | CRC-32 (zlib) of the `.a64` file |
| 32 | `name` char[32] | basename without `.mdl`, NUL-padded (debug) |

File size = `mesh_ofs + mesh_size`. Info and mesh are separate residency
units: the info block is resident whenever an entity uses the model; the
mesh only while the model is drawn.

## Info block

Header, 128 B:

| off | field |
|---:|---|
| 0 | `magic` u32 `0x53494E46` (`SINF`) |
| 4 | `numbones` u8, `numctl` u8, `numhitboxes` u8, `numattach` u8 |
| 8 | `numseq` u16, `numbodyparts` u8, `numtransitions` u8 |
| 12 | `flags` u32 (studiohdr_t flags) |
| 16 | `eyeposition` f32[3] |
| 28 | `min` f32[3], `max` f32[3] (ideal movement hull) |
| 52 | `bbmin` f32[3], `bbmax` f32[3] (clipping box) |
| 76 | `numevents` u16, `nsub` u16 (submodels over all body parts) |
| 80 | `bone_ofs`, `ctl_ofs`, `hitbox_ofs`, `attach_ofs`, `seq_ofs`, `event_ofs`, `bodypart_ofs`, `trans_ofs`, `str_ofs` u32 |
| 116 | `str_size` u32, reserved u32[2] |

Tables (each 4-aligned, in this order):

- **bone** 56 B: `parent` s8 (−1 root), `ctl` s8[6] (controller table
  index per X, Y, Z, XR, YR, ZR, −1 none), `flags` u8, `value` f32[6],
  `scale` f32[6] (mstudiobone_t defaults and delta scales).
- **controller** 16 B: `bone` s8, `index` u8 (0–3 user, 4 mouth), `type`
  u16 (STUDIO_X … STUDIO_ZR, STUDIO_RLOOP), `start` f32, `end` f32, `rest`
  u8, pad u8[3].
- **hitbox** 28 B: `bone` u8, `group` u8, pad u16, `bbmin` f32[3], `bbmax`
  f32[3] (bone frame).
- **attachment** 16 B: `bone` u8, pad u8[3], `org` f32[3] (bone frame).
- **sequence** 96 B:

  | off | field |
  |---:|---|
  | 0 | `label` u16 (string), `numblends` u8, `motionbone` u8 |
  | 4 | `fps` f32 |
  | 8 | `flags` u32 (STUDIO_LOOPING) |
  | 12 | `activity` s16, `actweight` u16 |
  | 16 | `numframes` u16, `numevents` u16 |
  | 20 | `event_first` u16 (event table index), `entrynode` u8, `exitnode` u8 |
  | 24 | `motiontype` u32 |
  | 28 | `linearmovement` f32[3] |
  | 40 | `bbmin` f32[3], `bbmax` f32[3] |
  | 64 | `blendtype` u16[2] |
  | 68 | `blendstart` f32[2], `blendend` f32[2] |
  | 84 | `nodeflags` u8, pad u8, `nextseq` u16 |
  | 88 | `anim_ofs` u32, `anim_size` u32 (the chunk in the `.a64` file) |

- **event** 12 B: `frame` s32, `event` s32, `options` u16 (string),
  `res` u16 — the `res_t` the option names, written by `tools/restab`
  (SOUND for `*.wav`, SENTENCE for `!NAME`, GROUP for event 1009's group
  name; 0 when it names nothing in the table). Events of a sequence are
  contiguous, in MDL order.
- **body part** 8 B: `nummodels` u16, `base` u16, `sub_first` u16 (first
  submodel of the part in the mesh's submodel table), pad u16. Body part
  `b` draws submodel `sub_first + (body / base) % nummodels`.
- **transitions**: `numtransitions²` u8 (the MDL transition graph).
- **strings**: the pool (labels, event options); `str_size` bytes.

Hitbox and attachment frames, controller and blend decoding are those of
the HLSDK (`dlls/animation.cpp`, `cl_dll/StudioModelRenderer.cpp`).

## Animation chunks (`.a64`)

Header 16 B: `magic` u32 `0x41363441` (`A64A`), `numseq` u16, `numbones`
u16, `size` u32 (file size), reserved u32. Then one chunk per sequence at
its `anim_ofs`, 16-aligned, in sequence order:

- `numblends × numbones × 6` u16 channel offsets (blend-major, then bone,
  then X, Y, Z, XR, YR, ZR), in 16-bit words from the chunk start; 0 = the
  channel is constant (the bone's `value`);
- the channels' value words: the MDL's mstudioanimvalue_t runs copied
  verbatim from the channel's first run through the run covering the last
  frame, plus the two words that follow it in the MDL (the HLSDK walk reads
  `value[valid + 2]` at a run's end). A run header word holds `valid` in
  bits 0–7 and `total` in bits 8–15; value words are s16.

A chunk is at most 65 535 words; the cook fails otherwise. Identical
channels are not shared (chunks are independent).

## Draw mesh

Header 64 B:

| off | field |
|---:|---|
| 0 | `magic` u32 `0x534D5348` (`SMSH`) |
| 4 | `nsub` u16, `nruns` u16 |
| 8 | `nmeshlets` u16, `nloads` u16 |
| 12 | `nslots` u16 (packed vertex slots, even), `ntris` u16 |
| 16 | `nbslots` u8 (bone matrix slots), `pos_shift` u8, `nskinref` u8, `nfamilies` u8 |
| 20 | `ntex` u16, reserved u16 |
| 24 | `bslot_ofs`, `sub_ofs`, `run_ofs`, `meshlet_ofs`, `load_ofs`, `vert_ofs`, `tri_ofs`, `skin_ofs`, `tex_ofs`, `nbone_ofs` u32 |

- **bslot** u8[nbslots]: the MDL bone of each matrix slot (the bones some
  vertex uses, ascending).
- **sub** 4 B: `run_first` u16, `run_count` u16 (per submodel, body part
  order then model order).
- **run** 8 B: `skinref` u16, `meshlet_first` u16, `meshlet_count` u16, pad
  u16: the meshlets of one submodel drawn with one texture.
- **meshlet** 8 B: `load_first` u16, `nloads` u8, `nverts` u8 (≤ 64),
  `tri_first` u16, `ntris` u16.
- **load** 4 B: `bslot` u8, `cache` u8 (first vertex-cache index), `count`
  u8, pad u8. A meshlet's vertices are sorted by bone; each load moves its
  `count` vertices (rounded up to a pair) from the packed buffer into the
  vertex cache at `cache` under the bone's matrix. A load's packed slot is
  the running sum of the earlier loads' counts rounded up to even, so
  every load starts on a vertex pair; an odd load's pad half duplicates its
  last vertex and is overwritten in the cache by the next load (the cache
  index of a vertex is its position in the meshlet).
- **vert** (16-aligned): `nslots / 2` tiny3d `T3DVertPacked` pairs
  (`w64_vert_t`): position = bone-local × 2^`pos_shift` rounded to s16
  (`pos_shift` = 4 unless the model's extent needs less), packed normal
  5.6.5 (`t3d_vert_pack_normal`), rgba `0xFFFFFFFF`, st = s10.5 texel
  coordinates on the cooked skin (`round(st × skin side / source side ×
  32)`; chrome vertices: the skin's centre texel).
- **tri** u8[3 × ntris]: vertex-cache indices.
- **skin** u16[nfamilies × nskinref]: texture of each skin reference per
  skin family.
- **tex** u32[ntex] offsets to the textures (16-aligned): a `w64_texp_t`
  header (`w`, `h`, `flags` W64_TEXF_FENCE 1 for STUDIO_NF_MASKED and
  W64_TEXF_RAMP 16, `ci4_ofs` 48 or 88 with RAMP, `pad2` = the MDL texture
  flags), the 16-entry RGBA5551 TLUT, with RAMP the 40-byte additive ramp
  ([sc4-format.md](sc4-format.md) "Ramp", render-fx.md 2a), then
  `w × h / 2` CI4 bytes. Side ≤ 64, powers of two; quantisation as the
  world's TEXP pages (tools/cook/bake_world_tex.py build_ci4; masked: slot 0
  transparent). RAMP is set on every texture of a model some entity of the
  cooked maps can draw Glow / TransAdd (its `model` key, its class's model
  or an env_shooter's `shootmodel`, for an entity with `rendermode` 3 / 5
  or named by an env_render that sets one: `tools/cook/hl_classes.py
  additive_models`; Uplink + Hazard: holo.mdl); the cart draws a skin
  without one additive as nothing (counted).
- **nbone** u8[nslots] (0 when the model has no chrome): the normal's bone
  of each slot (chrome texture coordinates use the normal bone).

## Validation (readers)

`tools/studio/s64.py` and the engine reject a file unless: magics and
versions as above; every offset/size inside its block; info counts
consistent (bone parents < own index, controller / hitbox / attachment
bones < `numbones`, sequence events inside the event table, body part
submodels inside `nsub`, event / label strings inside the pool and
NUL-terminated); mesh tables inside the mesh, loads and tris inside their
meshlets, `bslot` < `nbslots`, skin entries < `ntex`; `.a64` chunk ranges
inside the file and channel offsets inside their chunk.

## Sizes

Measured on the Uplink staging (tools/studio/studiocook.py `--report`):
see the cook's report line per model (`info`, `mesh`, `anim` bytes,
largest chunk); the sum feeds `HL64_POOL_STUDIO_KB`.
