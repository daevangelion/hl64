# `.w64` layer package, version 3

Imported from the quakegeneric tree (`n64/docs/w64-format.md`, audit entry
CLEAN, docs/provenance-audit.md). Lines that pointed at old-engine functions
are restated as prose definitions; the old runtime section is replaced by
the hl64 runtime requirements.

One file per map, `maps/<map>.w64`, cooked from the map's BSP30 by
`tools/w64c/w64c.py` and read in place by the hl64 world module
(`engine/world`, architecture.md 5). Design history: the old tree's
`n64/docs/stream-arch-v2.md` §3. Version 1 held the directory + `REND` +
`MOVR` + `TEXP`; version 2 adds streamed collision (`COLL` + model table +
leaf contents + collision grid), the entity blob (`ENTS`), the PVS rows
(`VISR`) and per-sector light probes; version 3 adds the additive ramp of
the `TEXP` pages additive movers bind (`TEXF_RAMP`). Struct definitions:
the world format header (`include/hl64/` / `engine/world`, retargeted from
the old `w64_format.h`).

- Big-endian (N64 native). Every chunk starts 16-aligned, every table 8-aligned.
- Offsets in the directory are from the directory start; chunk offsets are
  from the file start; offsets inside a chunk are from the chunk start.
- CRC-32 (zlib polynomial) of the directory in the header, of every chunk in
  the chunk directory. The runtime checks the directory CRC at load.
- Self-contained chunks: every chunk is usable where it is DMA'd (no
  pointers, no fix-ups); cross-chunk references are chunk indices or gids.
- Version 1 fields keep their meaning.

## File

```
0      w64_header_t (64 B)
64     layer directory (dir_size B)
...    chunks, each 16-aligned, in this order:
       REND × nsect, MOVR × movers, TEXP × npages, COLL × ncoll, ENTS, VISR,
       DECL (optional; old-tree packages may add ZONE, DRYS, STRM; see below)
```

### `w64_header_t` (64 B)

| off | field | meaning |
|---:|---|---|
| 0 | `magic` u32 | `'W64L'` |
| 4 | `version` u16 | 3 |
| 6 | `flags` u16 | bit 0 `W64_HF_COLL`: COLL chunks, model table, leaf contents and grid present |
| 8 | `file_size` u32 | must equal the cart file size |
| 12 | `dir_ofs`, `dir_size` u32 | layer directory |
| 20 | `nchunks` u32 | |
| 24 | `dir_crc` u32 | CRC-32 of the directory bytes |
| 28 | `idx_stride` u16 | tiny3d `VERT_OUTPUT_SIZE` the strip indices are encoded for (36) |
| 30 | `idx_base` u16 | `RSP_T3D_VERT_BUFFER & 0xFFFF` (0x0408) |
| 32 | `style0` u16 | lightstyle-0 value baked into vertex colour (264 = `"m"`) |
| 34 | `lightscale_x100` u16 | light scale baked into vertex colour, ×100 (250 = ×2.5) |
| 36 | `bbox` i16[6] | world mins, maxs |
| 48 | `max_verts`, `max_idx` u16 | meshlet limits (64, 104) |

### Layer directory

`w64_dir_t` (96 B):

| off | field | meaning |
|---:|---|---|
| 0 | `nsect, nleaf, ntex, npages, nsub, nchunks, texp_first, nvislists` u16 | v1 counts (`nleaf` = visleafs + 1, `nsub` = BSP model count) |
| 16 | `chunkdir_ofs, sect_ofs, pagepool_ofs, tex_ofs, sub_ofs, leaf_ofs, vispool_ofs` u32 | v1 tables |
| 44 | `coll_first` u16 | chunk index of the first `COLL` chunk |
| 46 | `ncoll` u16 | `COLL` chunks are `[coll_first, coll_first + ncoll)` |
| 48 | `coll_pinned` u16 | the first `coll_pinned` `COLL` chunks hold the world hull-0 tree: resident while the layer is open |
| 50 | `nleafs_all` u16 | BSP leaf lump count (leaf 0 + visleafs + submodel leaves) |
| 52 | `ents_chunk` u16 | `ENTS` chunk index, `0xFFFF` none |
| 54 | `visr_chunk` u16 | `VISR` chunk index, `0xFFFF` none (no vis lump) |
| 56 | `model_ofs` u32 | `w64_model_t[nsub]` |
| 60 | `leafc_ofs` u32 | `int8_t contents[nleafs_all]` |
| 64 | `grid_ofs` u32 | `w64_grid_t` |
| 68 | `mat_ofs` u32 | `w64_mat_t` surface materials, 0 = none (packages cooked without `--materials`: every surface reads as concrete) |
| 72 | `pad` u8[24] | |

| table | entry | content |
|---|---|---|
| chunk dir | `w64_chunk_t` 16 B | fourcc, file offset, size, CRC-32 |
| sectors | `w64_sector_t` 20 B | bbox i16[6], `REND` chunk index, `page_first/page_count` into the page pool |
| page pool | u16 | per sector: the `TEXP` pages its meshlets can bind (animation frames included) |
| textures | `w64_tex_t` 12 B | per BSP miptex index: page (`0xFFFF` none), flags (fence / sky / anim / missing), animation sequencing `anim_total/min/max/next` and `alternate` (defined in [world-surfaces.md](../specs/world-surfaces.md) 4: `anim_total` = 2 × frames, `anim_min` = 2 × frame index, `anim_max` = `anim_min` + 2, `next` cyclic, `alternate` = frame 0 of the other sequence) |
| submodels | `w64_sub_t` 16 B | per BSP model index: kind (`NONE` / `FOLDED` / `MOVR`), `MOVR` chunk, local bbox |
| leaf vis | u32 per leaf 0..visleafs | index (u16 units) into the vis pool |
| vis pool | u16 | lists `count, sector…`; identical lists are shared |
| models | `w64_model_t` 64 B | per BSP model, see Collision |
| leaf contents | i8 per BSP leaf | `CONTENTS_*` of every leaf (hull-0 point contents) |
| grid | `w64_grid_t` 32 B + cells + start table + list pool | see Collision |
| materials | `w64_mat_t` 24 B + start table + entries + mixed nodes + face boxes | see Surface materials |

`vis[leaf]` = every sector containing a face that is visible from `leaf`:
for each sector the cook keeps the set of leaves that mark its faces (world
faces: BSP marksurfaces; folded submodel faces: the world leaves their bbox
touches); the sector is listed for `leaf` when that set meets `PVS(leaf)`.
Leaf 0 and leaves without a PVS row list every sector.

## `REND` / `MOVR` chunk

```
w64_rend_t   24 B   nmeshlets, nstyled, vert_ofs, idx_ofs, style_ofs,
                    probe_ofs (0 = none: MOVR), probe_dims u8[3], pad
w64_meshlet_t[nmeshlets]   24 B each
w64_style_t[nstyled]        8 B each
verts        T3DVertPacked (32 B per vertex pair), each meshlet 16-aligned
indices      encoded strip, each meshlet 8-aligned
style data   per styled meshlet: nverts × nstyles u8
probes       (REND only, probe_ofs 8-aligned) int16 probe_org[3], then
             nx × ny × nz u8 (x fastest)
```

`w64_meshlet_t`: bbox i16[6] (sector: world space; `MOVR`: submodel local
space), `vert = vofs << 8 | nverts`, `idx = iofs << 8 | nidx`, `tex` (BSP
miptex index; every face of the meshlet has the same page), `style` (record
index or `0xFFFF`).

- **Meshlets**: ≤ 64 verts and ≤ 104 indices (DMEM with 64 verts loaded,
  `t3d.h`), faces grouped by texture page (animated textures by texture so
  the frame can swap) and style set, Morton-ordered by face centroid inside
  the sector so the per-meshlet bbox cull is tight. Faces over 62 verts are
  split into fans.
- **Vertices**: `T3DVertPacked` as the old tree's v1 vertex bake defined
  them, replayed in float32 by the cook: s16 position (truncated), `rgba` =
  style-0 vertex light (the nearest luxel of the face's lightmap at the
  vertex, [world-surfaces.md](../specs/world-surfaces.md) 1–2; mono =
  max(R,G,B) of the BSP30 RGB lightmap, × style0 >> 8, × light scale + 0.5,
  clamp 255; 255 for water / unlit faces) in R, G, B and alpha 0xFF; `st` =
  s10.5 texel coordinates rebased per face by whole texture periods: per
  axis, with the face's s (or t) of world-surfaces.md 1, the page scale
  `k` = the `.wci4` 16.16 page/source scale ÷ 65536 and the period `P` =
  page side ÷ `k`, base = floor(min over the face's vertices of s ÷ P) × P
  and `st` = trunc((s − base) × k × 32), all in float32; `norm`
  = the face normal (`t3d_vert_pack_normal` 5.6.5, unused by the RSP with
  `T3D_FLAG_NO_LIGHT`, used by the CPU dynamic-light pass).
- **Indices**: one zig-zag strip per face (`0, 1, n-1, 2, n-2, …`: n indices,
  n-2 triangles, polygon winding), restart bit (`0x8000`) on each face's first
  index, pre-encoded as tiny3d DMEM pointers
  `idx * idx_stride + idx_base`, end marker `0x4000` on the meshlet's last
  index — exactly `t3d_indexbuffer_convert`'s output, so
  `t3d_tri_draw_strip` takes them straight from the package.
- **Styles**: a meshlet whose faces use lightstyles other than plain style 0
  has a `w64_style_t` (`styles[4]`, 255 = unused; `data_ofs`) and per vertex
  the raw luxel of every style. The runtime recolours it when any of its
  style values changes: `L = Σ luxel[k] · value(style[k]) >> 8`, finished
  as `min(255, floor(L × lightscale_x100 / 100 + 0.5))`
  ([lightstyles.md](../specs/lightstyles.md) 3).
- **Light probes** (REND): a 64 u lattice covering the sector bbox —
  `probe_org = floor(bbox_min / 64) · 64`, `n = floor(bbox_max / 64) −
  floor(bbox_min / 64) + 1` per axis (≤ 255), so the lattice spans both bbox
  ends and trilinear interpolation never leaves it. Each value is the static
  model light at the probe point, computed in float32 as defined in
  [world-surfaces.md](../specs/world-surfaces.md) 3: a line 2048 u straight
  down from the point through the world hull-0 nodes, the first lit surface
  hit (sky `"sky"`/`"SKY"` and water `*`/`!` faces skipped, no lightmap →
  0) sampled over all of the face's styles at their **map-start values**
  (worldspawn's standard styles at time 0: style 0 `"m"` = 264, 1–11 the
  value of their pattern's first letter, 63 `"a"` = 0; switchable lights,
  style ≥ 32, `"m"` or `"a"` with START_OFF — `light`/`light_spot` only with
  a targetname, since the entity cook prunes them otherwise, `light_fluoro`
  always; every other style 256), no hit → 0, clamp 255. No lighting lump:
  255. The runtime adds dynamic lights and applies the floor of 32 and the
  clamp of 255 itself ([dlights.md](../specs/dlights.md)). Probe format v3
  (lightstyles.md 4) splits the value into a base and one style term.

Static submodels — `func_wall` / `func_illusionary` with no `targetname`
(nothing can fire, move or kill them), no `origin`/`angles` and render
mode Normal (`rendermode` 0 or absent) — are cooked into the sectors
(`FOLDED`); their entities are skipped at draw time. A submodel used by an
entity with any other render mode is never folded: it draws in the
translucent pass (render-fx.md 2). (Cook: `tools/w64c/w64c.py`
`FOLD_CLASSES` test must add the render-mode condition.)
Every other submodel used by a non-`trigger_*` entity gets a `MOVR` chunk in
its local space, drawn with the entity's origin/angles matrix. Submodels used
only by triggers (or by nothing) are `NONE`.

## `TEXP` chunk (one texture page)

```
w64_texp_t 16 B   w, h (u16), flags (u8: FENCE 1 = TLUT[0] transparent,
                  RAMP 16 = the additive ramp follows the TLUT), ci4_ofs (u32)
16                TLUT: 16 × RGBA5551
48                (RAMP) the additive ramp, 40 B (sc4-format.md "Ramp")
ci4_ofs           CI4 texels, w × h / 2 bytes; 48, or 88 with RAMP
```

Bound as one texture per page: TLUT upload, CI4 `TILE0`, infinite wrap,
alpha compare for fence pages. Pages hold a single texture (vertex alpha
0xFF, `alpha_to_tile` off); only pages some cooked face can bind are
emitted.

RAMP pages: every page a `MOVR` submodel binds whose entity can draw
Glow 3 / TransAdd 5 — `rendermode` 3 or 5 in its keys, or its targetname
the target of an `env_render` with rendermode 3 / 5 and without
`SF_RENDER_MASKMODE` (4) (`tools/cook/hl_classes.py additive_entities`).
The ramp is `tools/cook/addramp.py`'s fit of the page's TLUT
([render-fx.md](../specs/render-fx.md) 2a); the cart binds it instead of
the TLUT when it draws the mover additive, and draws nothing additive for
a page without one (counted). Cost: 40 B per such page (Uplink + Hazard:
2 pages, hldemo1's spinning-light func_rotating `*32` / `*35`).

Texel source is the map's `.wci4` (`tools/cook/bake_world_tex.py`: the
BSP's embedded miptex and the WAD3 textures its worldspawn `wad` key
lists, quantised on each texture's own palette, or with `--ref-palette` on
a given palette — the old tree's palette reproduces the old cook's bytes),
not the old `.wats` atlas sheets (re-quantised and downsampled to 32 px; not
used by hl64).

## Collision

### `COLL` chunk

The hull trees of every BSP model — hull 0 from the BSP nodes, hulls 1–3 from
the clipnodes (hull 3 root = `headnode[3]`, the GoldSrc crouch hull) — each
node stored exactly once.

```
w64_coll_t     16 B   nnodes (<= 4095), nx, ny, nz, nplanes, nextern, pad u32
w64_cplane_t[nplanes]  16 B  normal f32[3], dist f32   (non-axial planes, deduped in the chunk)
w64_cnode_t[nnodes]     8 B  u (f32 dist | u32 plane index), child i16[2]
u32 extern[nextern]          gids of children in other chunks
```

- **Node order**: sorted by plane class — `[0, nx)` axial X, `[nx, ny)`
  axial Y, `[ny, nz)` axial Z, `[nz, nnodes)` non-axial (stable: BFS /
  preorder order inside a class). A node's class is its index range, so the
  node needs no type field.
- **Axial** = BSP plane type 0..2 with the normal exactly +1 on that axis
  (every hull plane of the 10 Uplink maps; the cook fails on a type < 3
  plane with another normal). An axial node stores the plane `dist` inline:
  `d = p[axis] − dist`. A non-axial node stores the index of its chunk
  plane: `d = n · p − dist`. All floats are the BSP plane floats
  bit-exact, so a walk of the encoding gives the same sides as a walk of the
  BSP's own planes (axial planes compare one coordinate, others the dot
  product).
- **gid** (global node id) = `(coll_index << 12) | local`, `coll_index` =
  chunk index − `coll_first` (`W64_GID`, `W64_GID_COLL`, `W64_GID_LOCAL`).
- **Child** `c`: `0 <= c < nnodes` → local node; `c >= nnodes` →
  `extern[c − nnodes]` is a gid; `c < 0` → leaf: hull 0 → BSP leaf `−1 − c`
  (contents in the leaf-contents table), hulls 1–3 → `CONTENTS_*` = `c`
  (BSP clipnode semantics). `d >= 0` takes `child[0]`.
- **Partition**, per tree group: world hull 0 alone first (these are the
  `coll_pinned` chunks), then world hulls 1–3, then per submodel all of its
  hulls (a submodel never shares a chunk with another model). Inside a
  group: a subtree whose estimate (8 B/node + 16 B/non-axial node + 16 B
  header) exceeds the 2 KB cap is cut top-down in BFS order into a chunk
  that grows while its exact size (header + unique planes + nodes + externs)
  stays ≤ 2 KB; the BFS frontier is re-queued. Subtrees that fit are packed
  whole, in DFS preorder, into chunks of ≤ 2 KB. Measured (cap 1–3 KB,
  volume-priority growth and Morton packing tried): the total moves < 5 %,
  the worst neighbourhood set does not improve.

### Model table `w64_model_t` (64 B, one per BSP model)

| off | field | meaning |
|---:|---|---|
| 0 | `mins, maxs, origin` f32[3] each | `dmodel_t` raw floats |
| 36 | `head` i32[4] | hull roots: gid (≥ 0) or a leaf child encoding (< 0) |
| 52 | `visleafs` i32 | |
| 56 | `coll_first, coll_count` u16 | the model's `COLL` chunks, relative to `coll_first` (world: pinned + hull 1–3 chunks) |
| 60 | pad u8[4] | |

### Collision grid

```
w64_grid_t 32 B:  origin i16[3] (min corner, multiple of cell), dims u16[3],
                  cell u16 (256), nstart u16, cells_ofs, start_ofs, lists_ofs u32
                  (from directory start), pad u32
cells:  w64_gcell_t { u16 list; u16 start } [dims0 × dims1 × dims2], x fastest
start:  i32[nstart]
lists:  u16 pool
```

The grid covers the world model bbox: cell `(x, y, z)` =
`[origin + 256·(x, y, z), + 256]`; a point maps to
`floor((p − origin) / 256)`; points outside the grid walk from the root.

- `start[cell.start]`: the deepest world hull-0 node (gid) whose region
  holds the whole cell, or a leaf encoding when the cell lies inside one
  leaf; point → leaf for any point in the cell walks from there (identical
  result to a walk from the root). Values are shared.
- `cell.list`: u16 index into the list pool of `count, coll index…`
  (relative to `coll_first`): every world hull 1–3 chunk a point or box
  inside the cell can reach (box-pruned walk of hulls 1–3). Identical
  lists are shared; pool entry 0 is the empty list (`count` 0).
- Conservative classification: a cell is on one side of a plane only when
  the whole cell is ≥ 0.05 u away (above the trace stand-off `DIST_EPSILON`
  = 1/32 u of [trace-and-hulls.md](../specs/trace-and-hulls.md), by which a
  trace's far-side midpoint can lie on the near side).

A trace that starts in cell C and moves ≤ 256 u stays inside C's 3 × 3 × 3
neighbourhood, so the runtime keeps resident: the pinned chunks, the union
of the 27 neighbourhood lists, and the chunks of nearby traced submodels.

### Surface materials

The game's texture-type lookup (HLSDK `TEXTURETYPE_PlaySound`, the texture
type behind bullet / crowbar impact sounds) walks a brush model's hull 0
front to back along the line; the surface is on the node where the leaf
contents first change out of a non-solid leaf. The package says which
material that node's faces have, so the cart needs no faces or texinfo.
`w64c.py --materials <materials.txt | pak>` cooks it from `sound/materials.txt`
exactly as HLSDK reads it (`PM_InitTextureTypes`: type letter + name per line,
an unterminated last line dropped, names cut to 12 chars, exchange-sorted;
`PM_FindTextureType`'s binary search, case-insensitive over 12 chars; the
texture name stripped of `-x` / `+x`, then one of `{ ! ~ space`). Material
codes index `W64_MAT_CHARS` = `CMDVGTSWPYFN` (`CHAR_TEX_*`, 0 = concrete);
other letters read as concrete.

```
w64_mat_t 24 B:     nent, nmix, nface, pad u16; start_ofs, ent_ofs, mix_ofs,
                    face_ofs u32 (from directory start)
start:  u16[ncoll + 1]  entries of COLL chunk c (gid coll index) are
                        [start[c], start[c + 1])
ent:    u16[nent]       local << 4 | code, sorted per chunk
mix:    w64_matmix_t 8 B { u32 gid; u16 first, count } sorted by gid
face:   w64_matface_t 10 B { i16 lo[2], hi[2]; u8 code; u8 pad }
```

- A hull-0 node (any model's) is listed when its faces (BSP `dnode_t`
  `firstface`/`numfaces`) are not all concrete; unlisted nodes and nodes
  without faces read concrete. Faces all one material: that code.
- Otherwise code 15 (`W64_MAT_MIXED`): the node's `mix` record lists every
  face in BSP order as its vertex bounds on the two axes other than the
  node plane's dominant one (largest |normal| component, first on ties;
  lower axis first), `floor(min) − 1 .. ceil(max) + 1`. The runtime takes
  the first box holding the crossing point, else the nearest box
  (squared distance, first on ties).
- A cook-side check computes the same rule from the faces and
  `sound/materials.txt` and compares it with the encoding trace for trace.
- Uplink + Hazard sizes: 1.0 KB (t0a0d) to 6.8 KB (t0a0a: 1431 nodes, 50
  mixed / 304 faces) of directory per map (Uplink 2.5–3.8 KB), resident with
  the directory while the layer is open; the layer slot grows by the largest
  map's table (+3.8 KB Uplink-only, +4.5 KB with Hazard) × 2 slots.

## `ENTS` chunk

The map's entity chunk as the cook (`tools/w64c/hlecook`) makes it: the
HLE1 blob of the BSP entity lump ([hle-format-v1.md](hle-format-v1.md)),
then, 16-aligned, the map's HLN1 node graph when it has `info_node`s
(hle-format-v1.md § Node graph); little-endian, the runtime fixes byte
order in place with `hle_fixup` / `hln_fixup` — zero-padded to 16.

## `DECL` chunk (placed decals)

The map's placed-decal sheet: the `texture` values of its kept `infodecal`
records that are not in the global sheet `gsfx/decals.i4` (the 42 HLSDK
decals.h decals), sorted by name, cooked from the player's `decals.wad` in
the global sheet's format (DI4 2, `tools/cook/decals.py`; the engine's
`load_sheet` rules hold), zero-padded to 16. An infodecal's decal id
42 + i is entry i of this chunk (hle-format-v1.md "Ids"). At most one,
after `VISR`; absent when the map places no such decal. Found by fourcc
(no directory index), so packages without it keep the layout and version.
Runtime (architecture.md 4.1 decal region): the global sheet at the region
start, two map slots (one per open layer, each half of what the global
sheet and the piece vertices leave), the piece vertices at the end; a
layer's `DECL` is read into its slot when the layer opens. The cook proves
align16(global) + 2 × align16(largest `DECL` of the campaign) +
`HL64_FX_DECALS` × `HL64_FX_DECAL_VERTS` × 10 B ≤ `HL64_DECAL_TEXELS_KB`
(`hleids --check`).

## `VISR` chunk

`u32 ofs[visleafs + 1]` (from chunk start; the row of leaf `L` (1..visleafs)
is bytes `[ofs[L−1], ofs[L])`; an empty row = no vis → everything visible)
followed by the BSP's compressed PVS rows, unchanged. Row encoding (BSP vis
lump): bit `i` of the decoded row is visleaf `i + 1`; a non-zero byte is 8
literal bits; a zero byte is followed by a count byte `n` and stands for
`n` zero bytes; a row decodes to `ceil(visleafs / 8)` bytes.

## `ZONE` / `DRYS` / `STRM` chunks (old zone-system companions)

The exact bytes of the old tree's per-map `.zones` (Q64Z), `.dry` (DRY1)
and `.stream` (Q64S) files, little-endian as those formats are; the chunk
`size` is the file length (the next chunk starts 16-aligned after it). Each
appears at most once, after `VISR`; the directory has no index for them
(found by fourcc), so packages without them keep the same layout and
version. hl64 does not use them (the zone system is dropped,
architecture.md 16.3): the hl64 cook (`tools/w64c/w64c.py`) does not emit
them and readers skip them.

## Sectors (cook)

Faces (world + folded) are grouped into units — one 256 u cell of face
centroids × one marking-leaf set — and every unit under 2 KB merges into the
nearby sector (bboxes within 64 u) whose union adds the least PVS overfetch,
`bytes(A) · |leaves that see B but not A| + bytes(B) · |leaves that see A
but not B|`, capped at 16 KB (anything larger is split on its longest axis).

## Budget proof (cook)

Pools from `include/hl64/limits.h`; every chunk size is a multiple of 16 B.
The cook fails if any open set exceeds its pool (`--no-budget` warns
instead, for sizing runs; the `--json` report is written either way):

- **render**, per visleaf: Σ size of the `REND` chunks in `vis[leaf]` (probes
  included) + Σ `MOVR` of the movers visible from the leaf (placement box =
  model bbox + the entity's `origin` key, one per entity using the model;
  visible when the box touches a leaf in `PVS(leaf)`) ≤ the render pool.
- **texture**, per visleaf: Σ `TEXP` pages of those sectors and movers ≤
  the texture pool.
- **collision**, per grid cell: Σ pinned + Σ union of the 3 × 3 × 3
  neighbourhood lists + Σ chunks of the submodels whose placement box
  (+ 64 u) touches the neighbourhood ≤ the collision pool. Only traced
  submodels count: entities of class `trigger_*` (SOLID_TRIGGER) and
  `func_illusionary` (SOLID_NOT) without a contents skin (`skin` absent, 0
  or −1) are left out — the old runtime streamed a submodel's `COLL` only
  for SOLID_BSP entities or a contents skin (`skin < −1`). Their chunks stay
  in the package. hl64 tests a brush trigger against its hull
  ([area-links-and-touch.md](../specs/area-links-and-touch.md) 3), so the
  rewritten cook (M2) counts trigger `COLL` in this proof.

Report (`--json`): file / dir bytes, bytes, count and size histogram per
chunk kind, worst visleaf render / texture set with its parts (sectors,
probe bytes, movers), `coll_bytes`, `coll_pinned_bytes`,
`coll_baseline_bytes` (8 B × unique hull nodes reachable from the model
heads + 16 B × unique planes they use) and `coll_ratio`, the worst grid
cell's collision set (pinned / lists / submodels), grid sizes.

## Runtime requirements (hl64)

- Layer open: one read of the header and directory (architecture.md 5.3);
  magic, version, `file_size`, directory CRC, every table offset and count
  and every chunk range checked against the file before use; the tiny3d
  self-check converts a probe with the running `t3d_indexbuffer_convert`
  and compares it with `idx_stride/idx_base` — on a mismatch every meshlet
  is decoded with the cooked constants and re-encoded in place.
- Frame: eye leaf (point → leaf through the grid start node) → `vis[leaf]`
  → sector bbox and meshlet bbox against the four frustum planes → counting
  sort by page → per page one bind, per meshlet one `t3d_vert_load` + one
  `t3d_tri_draw_strip`, RSP back-face cull on. Movers: per visible `*N`
  entity, sphere cull, entity matrix, same per-page submit.
- Lightstyles: styled meshlets are recoloured lazily when drawn and one of
  their style values changed (lightstyles.md 3). Unstyled meshlets keep the
  cooked colour.
- Dynamic lights: meshlets whose bbox meets a light sphere are copied to a
  per-frame scratch buffer (512 vertex pairs, double-buffered) and lit there
  with the falloff of [dlights.md](../specs/dlights.md), in world space from
  the cooked normal. Movers are not dynamically lit.
- Stats records carry `w64=<KB> sectors meshlets verts binds dlit relit`.

## Tools

```
make cook ...                         # builds $(BUILD_DIR)/tools/hlecook
$(BUILD_DIR)/tools/hlecook <map.bsp> <out.hle>       # HLE1 + HLN1 (ENTS input)
python3 -m tools.cook.bake_world_tex <map.bsp> -o <map.wci4> [--wad-dir DIR]
                   [--ref-palette palette.lmp]
python3 -m tools.w64c.w64c <map.bsp> <map.wci4> <out.w64> --hle <map.hle>
                   [--decl <map.decl>] [--materials sound/materials.txt]
                   [--json report.json] [--no-budget]
python3 -m tools.w64c.w64check <map.w64> <map.bsp> [--hle map.hle]
                   [--points N] [--segments N] [--submodels N] [--seed S]
python3 -m tools.w64c.w1 --fixtures DIR --data DIR [--hlecook PATH] [--w64c-only]
```

The content staging (`tools/cook/stage.py`, run by `make cook`) cooks every
staged map's intermediates (`.wci4`, `.hle`) into its staging directory and
runs `w64c --hle` into the content directory: an HL map ships only its
`.w64` (plus `maps/<map>.ent`, restab's input). `tools.w64c.w1` is the W1
test (world-surfaces.md 5): the packages, `.wci4` and `.hle` byte-equal to
the old tree's tools on all 10 Uplink/Hazard maps (fixtures made once from
the old tree with `--make-fixtures`).

`w64check.py` is the cook-side exactness proof (exit 1 on any mismatch):
header / directory / chunk CRCs and bounds, meshlet and strip encoding,
probe blocks; model table and leaf contents against the BSP; for ≥ 20000
random points (a quarter snapped to the 1/16 u lattice so plane ties are
hit) the contents of world hulls 0–3 and of hulls 0–3 of 8 submodels (the
largest half plus random ones) through the `COLL` encoding equal a direct
walk of the BSP nodes / clipnodes with the same path length, point → leaf
from the grid start equals the walk from the root, every visited node lies
in a pinned chunk (hull 0), a chunk its cell lists (hulls 1–3) or the
model's chunk range (submodels); exactly, per grid cell with box
arithmetic, the start node lies on the path the whole cell follows and
every hull 1–3 node a box-pruned walk of the cell reaches is in its list;
2000 random segments ≤ 192 u give the same trace result
(fraction, flags, visited path) on the encoding as on the BSP and visit
only pinned or 3 × 3 × 3 neighbourhood chunks; `VISR` rows decompress to
the BSP rows; `ENTS` equals the `hlecook` blob; every chunk is a known kind
(old-tree ZONE / DRYS / STRM companions are skipped, at most once each; a
`DECL` chunk, at most once, passes the sheet rules).
