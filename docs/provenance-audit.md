# Provenance audit of the quakegeneric N64 tree

Audit of every file in `~/src/ports/quakegeneric` that is not in the Quake
first commit (`9fd3580`), to decide what hl64 may import. The binding rules
are in `PROVENANCE.md`. Only files marked CLEAN here may be copied into the
MIT tree. QUAKE-INTERFACE files are our own expression, but they must be
re-targeted to hl64 APIs before import. Every other verdict needs a rewrite
from a prose spec, or a move to `game/` under the HLSDK licence.

- **Snapshot**: working tree of `~/src/ports/quakegeneric` (branch `n64-port`,
  HEAD `8d04977`), 2026-11-13. The tree is dirty: 165 of the 373 audited
  files are untracked, so they have no git history and their verdicts rest
  on content comparison alone. Line numbers refer to this snapshot. Agents
  FXPerf and Corpse were editing r64_trans.c, r64_studio.c, r64_decal.c,
  r64_tex.c, gsfx_cart.c, gsfx.h and two tools during the audit. Re-run the
  scanner on those files before importing anything from them.
- **History boundaries**: `9fd3580` is the Quake/quakegeneric import. Up to
  `1305210` (2025-01-27) every commit is upstream quakegeneric (erysdren et
  al., GPL). Our work begins with the 2026-07 commits by "Ubuntu" and
  "Automated Developer", plus all untracked files.
- **Out of scope**: `qc/` (QuakeC sources and the copper/qw-qc mods) and our
  edits inside Quake files (§4). Both are forbidden regardless.

## Method

1. **Inventory.** Candidates are `git ls-files -co --exclude-standard` minus
   `git ls-tree -r 9fd3580`, excluding `qc/`: 373 files, 164,759 lines.
2. **Token similarity** (C/C++/headers, 179 files). A k-gram index was built
   over five reference corpora: QUAKE = first commit + upstream quakegeneric
   at `1305210` + id-Software/Quake (WinQuake, QW, GLQuake) + Quake 2;
   QCTRANS = our `source/g_*.c` and `game*.c`; XASH = FWGS xash3d-fwgs
   `engine`, `ref/gl`, `ref/soft`, `common`, `pm_shared`, `public`,
   `filesystem`, `utils`; HLSDK = ValveSoftware/halflife + FWGS
   hlsdk-portable; LIBDRAGON = libdragon + tiny3d, used as a control for
   generic code. Two passes ran: exact 24-token grams plus
   identifier/number-normalised 48-token grams, then exact 16 plus
   normalised 30. A hit counts only when its reference span is compact.
   Hits on regions that also match libdragon were dropped as generic.
3. **Comment and prose overlap.** Word 8-grams of our comments, docs and
   scripts were compared against every comment in the Quake, Xash and
   HLSDK corpora. This finds copied comments that token scans miss.
4. **History.** `git log -C -C -M` copy and rename detection at 30% over our
   commits. The one real copy it found is `sys_null.c` → `n64/port/sys_n64.c`.
   Files with an "Id Software" header were checked back to their creating
   commit, and those first versions were scanned as well.
5. **Structural review.** Eight review slices (renderer A/B, GLES, FX and
   sound, world, game A/B, tools and studio) plus this platform/docs slice
   compared every function that implements an engine algorithm with a
   known Quake, Xash or HLSDK counterpart. They looked at control flow,
   constants, ordering and naming, so translations with renamed variables
   were caught as well as copies.
6. **Bindings.** Each file was grepped for Quake API categories (QuakeC
   entvars/progs, `edict_t`, `entity_t`/`cl.`, `model_t`/`Mod_*`,
   refdef/`r_*`, `Cvar_*`, `Cmd_*`/`Cbuf_*`, `Con_*`, `Hunk_*`/`Cache_*`,
   `MSG_*`/`svc_*`, `sv.`/`SV_*`, `COM_*`, `S_*`, `Host_*`, `Draw_*`/`SCR_*`).

Evidence files are on the dirty side and are not part of hl64:
`/scratch/quakegeneric/tmp/ProvAudit/{evidence.md, evidence_k16.md, comment_overlap.md, comment_refs.txt, review/*.md, sim.py, cmt.py}`.
Appendix A carries the per-region evidence from those reviews. It holds no
code from any source, only names, line ranges and reference locations.

**Limitations.**
- The token scanner reads C only. Python and shell tools were reviewed by
  reading them and by the comment/prose overlap check.
- The Source SDK is not in the corpus. The `hl_view.c` shake attribution
  is a reviewer's recollection: [INFERENCE].
- Matches against HLSDK are partly structural judgement, so the HLSDK line
  counts are estimates (≈).
- Files shared with Xash that are Quake-heritage are attributed to Quake
  when our text follows the Quake form.

## Verdicts

- **CLEAN**: our expression with no Quake engine bindings. Import as-is;
  the GPL "quakegeneric contributors" header is replaced with the MIT
  header (we hold the copyright).
- **QUAKE-INTERFACE**: our expression, but it calls or implements Quake APIs
  or types. Import after re-targeting the listed bindings.
- **CONTAINS-QUAKE / CONTAINS-XASH**: named functions or blocks were copied
  or translated from Quake (incl. quakegeneric) or Xash3D/FWGS. Those
  regions must be re-specified in prose and re-implemented clean. The rest
  of the file takes its own verdict.
- **HLSDK**: translated from the Half-Life SDK. It goes to `game/` under the
  HLSDK licence, never into the MIT engine.
- **CONTAINS-QCTRANS**: derived from our QuakeC translations (`g_*.c`), which
  are Quake-derived and forbidden.
- **FORBIDDEN**: a whole-file Quake/quakegeneric/QuakeC-translation origin.

The per-file table gives each file a *primary class*, meaning what to do
with the file as a whole. The contaminated-line columns count every
region, whatever the file's primary class.

**Policy gap.** `PROVENANCE.md` as written allows only files marked CLEAN.
QUAKE-INTERFACE files are our own expression, but under that wording they
stay forbidden until `PROVENANCE.md` admits them. Recommended rule: a
QUAKE-INTERFACE file may be imported if the importing commit re-targets
every listed binding and rewrites, from a spec, every region this audit
names in it. That is a policy decision for the owner of `PROVENANCE.md`;
this audit does not make it.

## 1. Summary

### 1.1 Lines per verdict (primary class)

| Primary class | Files | All lines | of which code (.c/.h/.cpp/.py/.sh) |
|---|---:|---:|---:|
| CLEAN | 180 | 34,246 | 30,046 |
| QUAKE-INTERFACE (ours, retarget) | 48 | 31,177 | 31,177 |
| CONTAINS-QUAKE (rewrite named regions) | 29 | 26,046 | 26,046 |
| CONTAINS-XASH (rewrite named regions) | 13 | 13,377 | 13,377 |
| HLSDK (→ game/) | 24 | 24,482 | 24,482 |
| FORBIDDEN (Quake / upstream quakegeneric / QuakeC translation) | 41 | 13,413 | 12,695 |
| Docs (see §3) | 15 | 4,966 | 0 |
| Third-party (own licence) | 12 | 6,575 | 6,206 |
| Data / logs / build artefacts | 11 | 10,477 | 0 |
| **Total** | **373** | **164,759** | **144,029** |

Contaminated regions across all files, from the region tables. HLSDK and
Xash figures for files where both apply (studio bone code) are not added
twice:

| Source | Lines in named regions | Where |
|---|---:|---|
| Quake / quakegeneric | ≈2,757 | w64_world.c 286, r64_light.c 277, hl_nodecook.h 283, sys_n64.c 185, q64_brush.c 163, w64c.py 152, r64_world.c 143, r64_alias.c 115, q64mapc.c 115, sys_sdl.c 85, w64check.py 73, hl_material.c 70, q64_map.c 69, r64_main.c 55, glr_main.c 51, r64_draw.c 48, r64_sector.c 47, glr_alias.c 45, hl_use.c 45, gsfx_te.c 40, r64_tex.c 38, glr_world.c 36, small items elsewhere |
| Xash3D / FWGS | ≈2,805 | gsfx_cl.c 721, gsfx_cart.c 501, studio_geom.c 425, glr_beam.c 332, xash_ref_stream.c 322, r64_trans.c 117, r64_decal.c 85, glr_decal.c 83, r64_studio_bake.c 52, glr_hlpart.c 51, glr_sprite.c 43, glr_world.c 31 (low confidence), hl_use.c 17, glr_trans.c 14, hl_nodecook.h 11 |
| HLSDK (game logic, format structs, constants) | ≈16,229 | source/hl ≈14,400; gsfx_sv.c 711, gsfx_te.c 432, gsfx_cl.c 166, gsfx_cart.c 141; studio.h 200 (MDL structs); w64c.py 98 (PM texture types); small constant/idiom items |
| QuakeC translations (g_*.c) | ≈164 | hl_ai.c ≈130, hl_movers.c 21 (also HLSDK-identical), hl_grunt.c 9, hl_monsters.c 4 |

### 1.2 Import now (CLEAN)

Each import replaces the GPL "quakegeneric contributors" header, or adds an
MIT header where the file has none, and names this audit entry in the
commit (`PROVENANCE.md`). Small residual calls to a log function are noted
per file.

| Module | Files | Notes |
|---|---|---|
| Cart I/O scheduler | n64/port/q64_io.c, source/q64_io.h | stream-arch-v2 PI read scheduler (budget, seam, pump, priorities). No bindings. |
| Residency pools | n64/port/q64_pool.c, source/q64_pool.h | Address-ordered first-fit pools with retire/pack. No bindings. |
| Cart DMA read | n64/port/r64_cart.c | DFS file → PI address, aligned DMA with fallback. Pattern from our OpenLara N64 port (OpenLara tree is BSD-2-Clause; the N64 platform code is ours). Includes r64_local.h only for `qboolean`. |
| Limits | source/q64_limits.h | Memory envelope and table limits. Rename the Quake `MAX_*` macros it overrides. |
| Profiling | source/q64_prof.h, n64/port/r64_iostat.c, n64/port/r64_malloc.c | Zone timers, I/O counters, `--wrap=malloc` tracker. r64_malloc.c calls `Con_Printf` → hl64 log. |
| FPU trap shim | n64/port/fpu_shim.c | VR4300 unimplemented-operation emulation. |
| Test harness | n64/port/test_harness.h, n64/port/test/ares-*.patch | ares test-harness register map and our ares patches (ISC upstream). |
| libdragon patches | n64/port/libdragon-patches/*.patch | rspq race fixes (libdragon is public domain). |
| Parity host | n64/port/host/q64_heap_host.c/.h, host/q64_io_host.c/.h, host/libdragon.h, host/q64_host_audio.c | q64_heap_host.c re-implements newlib mallocr.c = dlmalloc 2.6.4 (Doug Lea, public domain); say so in the commit. q64_io_host.c calls `Sys_Error` → hl64 fatal. |
| SDL/Android shell | n64/port/sdl/sdl_content.c, sdl_local.h, sdl_audio.c, n64/port/android/** (minus Gradle wrapper), gles/glr.h, glr_shot.c, glr_fx.h | sdl_audio.c 329-461 is a declared port of libdragon's reference VADPCM/Huffman decoder (public domain): keep the attribution comment. |
| Format headers (ours) | source/w64_format.h, source/studio_geom.h, studio_soft.h, studio_bake.h, ui_bake.h, world_atlas.h, world_tex_bake.h, alias_bake.h; source/hl/hl_levelstate.h, hl_sentences.h, hl_input.h, hl_view.h, hl_hud_rects.h; n64/port/q64_route.h; n64/port/r64_sys.h | w64_format.h 110-120 holds the HLSDK material-letter set (pm_shared): a data constant; keep it or move it to game/. hl_hud_rects.h is generated from the user's HUD sprite data (rectangles, facts). |
| Small runtime helpers | n64/port/gsfx/gsfx_util.c, source/hl/hl_sounds.h (ATTN constants are HLSDK values) | — |
| Cook and analysis tools | tools/w64c/hlecook.c, tools/q64zone/q64zone.c + bake_dry.py, tools/studio_bake/bake_studio.c + bake_meshopt.cpp (link the rewritten studio geometry, not studio_geom.c), tools/hl_world/*.py, hl_campaign.py, hl_campaign_sizing.py, hl_image.py, hl_stage_content.py, hl_sprite_cook.py, hl_sound_refs.py, hl_sound_classes.py, hl_classes.py and hl_gsfx.py (HLSDK-informed data tables: class lists, no code), bake_world_atlas.py, analyze_hl_tex_atlases.py, world_tex_bake, hlhud_bake, ui_bake, convert/extract sound scripts, wav tools, profdb.py, prof_compare.py, mk_white_room.py, n64_content_pipeline.py, paktool.py, rebuild_n64_paks.py, mdl_size_report.py | Full list in §2. Quake-only tools (alias_bake, q64mapc, progs_* audits) are not needed. |
| Studio test rig | tools/studio_compare/** except soft_studio.c and xash_ref_stream.c | CLEAN tests, but they link the old studio code; re-point them at the hl64 implementation. |
| Model viewer | n64/port/modelviewer/** except viewer.c | hostfs_9p.c/.h, sc64_link.c/.h, sc64_*.py are near-copies of the sibling Homeworld N64 tree: ours, not Quake/Xash/HLSDK. Confirm ownership in the commit. viewer.c 33-39 holds Quake alias-MDL constants. |

### 1.3 Import after re-targeting (QUAKE-INTERFACE; our expression)

| Module | Files | Bindings to re-target | Regions to rewrite first |
|---|---|---|---|
| Seamless world / campaign seams | n64/port/q64_world.c/.h | `edict_t` + QuakeC `v.*` fields, `sv.`/`svs.`, `SV_LinkEdict`, `Con_*`/`Sys_*`, `COM_FOpenFile`, `Host_Error`, `qboolean`, `struct edict_s` | — |
| Stream scheduler | n64/port/w64_stream.c | edicts (`v.*`), `cl_entities`/`cls`, `r_refdef`/`vup`, `sv_player`, `Sys_Printf` | — |
| .w64 runtime API | source/w64_world.h, w64_node.h | `model_t`, `mleaf_t`, `hull_t`, `trace_t`, `dmodel_t`, `edict_t`, `byte` | w64_world.c itself is CONTAINS-QUAKE (§1.4). |
| HLE entity runtime | source/hl/hl_ents.c/.h, hl_entcook.h, hl_levelstate.c | QuakeC entvars field defs (`pr_fielddefs`, `ddef_t`, `ED_*`, `PR_*`, `pr_global_struct`, `progs`), `edict_t`, `sv.`/`svs.`, `Cvar_*`, `Cmd_AddCommand`, `Hunk_AllocName`, `Mod_ForName`; hl_entcook.h field-name table 449-494 | hl_ents.c 2486-2524 (multi_manager think shape, HLSDK): move to game/ or rewrite from spec. |
| Sound backend | n64/port/snd_n64.c | Implements the Quake `S_*` API (`S_Init`, `S_StartSound`, `S_StaticSound`, `S_StopSound`, `S_Update`, `S_PrecacheSound`, `sfx_t`, ambient hooks), `Cvar_*`, `Cmd_*`, `cl.`/`sv.` | None. Spatialisation 611-667 is our own linear-attenuation/pan code, and the Quake attenuation behaviour it reproduces is public. Lines 105-106 declare two cvars named as in snd_dma.c: rename them. |
| Studio model loader / residency | source/mod_studio.c | `model_t`, `Mod_*`, `COM_OpenFile`/`Sys_File*`, `sv.`/`cl.`, `Con_*`, `host_framecount` | — (bone maths lives in studio_geom.c, §1.4) |
| v2 world renderer pieces | n64/port/r64_sector.c, r64_world_atlas.c, r64_world_tex.c, r64_local.h, r64_part.c, frame_stats.c, r64_alias_bake.c, r64_studio_sw.c, r64_studio_stubs.c, r64_studio.c; gles/glr_local.h, glr_part.c, glr_gl.c | `entity_t`, `cl_visedicts`, `cl_dlights`, `d_lightstylevalue`, `r_refdef`/`r_origin`/`vup`/`vpn`, `model_t`/`msurface_t`, `Cvar_*`, `Con_*`, `COM_LoadHunkFile` | r64_sector.c 431-453 (texture animation = Quake `R_TextureAnimation`), 634-649 (dlight falloff); r64_studio.c 599-609 (HLSDK frame wrap idiom). |
| Platform entry / host | n64/port/quakegeneric_n64.c, sdl/sdl_main.c, sdl/sdl_input.c, host/q64_host.c/.h, q64_platform_cfg.h, sdl_platform_cfg.h | quakegeneric `QG_*` platform API, `Host_Init/Host_Frame`, `COM_InitArgv`, `Cbuf_AddText` + console scripts, Quake `K_*` key codes, `cl.viewangles`, `Cvar_*`, `key_dest`, `quakeparms_t` | sdl_input.c 128-139 `look_turn` pitch clamp (in_win.c IN_MouseMove). Remove the Id Software header from quakegeneric_n64.c (template only). |
| Memory/budget instrumentation | n64/port/q64_budget.c, r64_mem.c, r64_hunk.c, r64_prof.c, q64_selftest.c | `Hunk_*Mark`, `Cache_Flush`, `COM_FOpenFile`, `com_gamedir`, `host_parms`, `Cmd_*`, `Cvar_*` (`r_profile`), per-module reporters | — |
| HL sound and view glue | source/hl/hl_sounds.c, source/hl/hl_view.c | `edict_t`, `Cvar_FindVar`, `Con_*`, `MSG_Read*`, `SCR_CenterPrint`, `cl.`, `host_frametime` | hl_view.c 62-93 screen shake follows Valve's algorithm (Source SDK, [INFERENCE]): rewrite from a behaviour spec or move it to game/. |
| Route runner | n64/port/q64_route.c, test/*.cfg, test/*.sh | edict `v.*`, `cl.`, `Cbuf_AddText`, `pr_strings`, console commands | — |
| GoldSrc effects protocol header | n64/port/gsfx/gsfx.h | Prototypes on `edict_t`/`entity_t`/`client_t`/`trace_t`/`model_t`. Values of `kRender*`, `kRenderFx*`, `FBEAM_*`, `TE_*`, `BREAK_*`, `BLOOD_COLOR_*` are HLSDK protocol constants: re-declare them in a `game/` (HLSDK-licensed) header or a neutral protocol header. | — |
| Header-only | source/hl/hl_ai.h, hl_monsters.h, hl_items.h, hl_movers.h, hl_env.h | `edict_t` | They carry HLSDK constants (13/23/0/0/4 lines); keep those values in game/. |

### 1.4 Rewrite from a prose spec (CONTAINS-QUAKE / CONTAINS-XASH)

Each region below is a behaviour hl64 still needs. The specification
agents write it up in prose (docs/), and implementers re-implement it
without opening the old file. The rest of each file is ours: see §2 and
Appendix A.

| Behaviour to re-specify | Contaminated regions (file:lines → origin) |
|---|---|
| Hull point contents, recursive hull trace, point-in-leaf, PVS decompression, touched leafs / fat PVS, submodel hull setup | w64_world.c 443-458, 706-721, 797-799, 859-996, 1077-1099, 1115-1144, 1157-1182, 1233-1267 (Quake world.c `SV_HullPointContents`/`SV_RecursiveHullCheck`, model.c `Mod_PointInLeaf`/`Mod_DecompressVis`/`Mod_LoadSubmodels`, sv_main.c `SV_AddToFatPVS`/`SV_FindTouchedLeafs`; the file header itself says "line-for-line replicas"); q64_map.c 3347-3379; hl_nodecook.h 280-578 (also `SV_Move`/`SV_ClipToLinks`, `SV_CheckBottom`, `SV_movestep`, Xash `SV_HullForBsp`/`SV_MoveTest`); hl_material.c 236-290, 554-568; gsfx_te.c 479-518 (`SV_Move` trace merge); tools/w64c/w64check.py 296-368 |
| BSP lump loading semantics (texinfo mip adjust, face flags, plane signbits, clipnode hulls, submodels) | q64_brush.c 240-264, 269-351, 511-527, 557-579, 765-801; q64_map.c 1960-1967, 2435-2455, 2968-2977; tools/q64mapc/q64mapc.c 172-251, 1024-1069 (`CalcSurfaceExtents`, `Mod_LoadFaces`) |
| Texture animation sequencing (`+0..9`, `+a..j`) | q64_brush.c 269-351; r64_tex.c 725-762; r64_sector.c 431-453; glr_world.c 743-761; w64c.py 344-373 |
| Lightstyle animation, dynamic lights, light-point sampling | r64_light.c 199-216, 275-325, 349-360, 410-450, 469-637; r64_alias.c 508-572, 612, 640-645 (`RecursiveLightPoint`/`R_LightPoint`); glr_light.c 29-40, 55-64; glr_world.c 215-231; r64_sector.c 634-649; w64c.py 838-944 (cook-time lightmap trace); w64c.py 121-128 (stock lightstyle strings) |
| View / leaf marking / culling | r64_world.c 1555-1612 (`R_MarkLeaves`, surface facing), 2300-2316 and weak 359-385, 2081-2121, 2413-2494; r64_main.c 45-54, 87-129, glr_main.c 538-569 (`R_SetVrect`); r64_vid.c 162-172, glr_main.c 263-268, host/q64_host_render.c 49-81 (`VID_Init`/notexture setup) |
| 2D drawing (`Draw_*`) | r64_draw.c 463-500, 521-526, 561-567, 621-632, 674-676 (incl. copied comments); glr_draw.c 399-419, 587-614; host/q64_host_render.c 461-462 |
| Alias models (Quake .mdl) | r64_alias.c 33-37 (anorms), 673-699, 787-797; glr_alias.c 227-282, 375-380; gsfx_spr.c (`R_GetSpriteframe` / `Mod_LoadSpriteFrame`, 28 lines). Not needed for HL content. |
| Platform file layer | sys_n64.c 26-168, 264-305 (sys_null.c); sdl/sys_sdl.c 19-84, 137-178 |
| GoldSrc client effects (tempents, blood, sparks, explosions, smoke, break models, sprite spray, tracers) | gsfx_cl.c ≈721 lines, gsfx_cart.c ≈501 lines (Xash cl_tent.c / cl_efx.c function by function: `CL_PrepareTEnt`, `R_BloodStream`, `R_Blood`, `R_BloodSprite`, `R_Explosion`, `R_Sprite_Smoke`, `R_BreakModel`, `R_TempModel`, `R_SparkStreaks`, `R_StreakSplash`, `R_Spray_Generic`, tracer colour table from gl_rpart.c); glr_hlpart.c 146-196 (`CL_DrawTracers`) |
| Render-mode blending and glow | r64_trans.c 87-150, gsfx_cl.c 386-478 (Xash `CL_FxBlend`); gsfx_cl.c 482-512 (`R_SpriteGlowBlend`); glr_trans.c ≈14 lines; glr_sprite.c 117-143 (low confidence) |
| Sprite orientation | r64_trans.c 592-644; glr_sprite.c 282-328 (Xash `R_DrawSpriteModel`, Quake r_sprite.c lineage) |
| Decals | r64_decal.c 403-421, 701-764, 944-945; glr_decal.c 242-260, 449-512 (Xash `R_DecalAlloc`/`R_DecalIntersect`); r64_decal.c 592-619, glr_decal.c 324-351 (Quake-tools `baseaxis` table, also in HLSDK utils/qcsg) |
| Beams | glr_beam.c ≈332 lines (Xash gl_beams.c: noise, segments, rings, torus, disk, cylinder) |
| Studio bone setup / blending / chrome | source/studio_geom.c 77-188, 229-391, 399-517, 586-623 (header: "ported from xash3d"); tools/studio_compare/xash_ref_stream.c 24-371; r64_studio_bake.c 828-879, glr_studio.c 139-149 (chrome basis, Xash operand order). The HLSDK `cl_dll` StudioModelRenderer.cpp is an allowed source for `game/`. GoldSrc does its studio rendering in the client DLL, so hl64 can host the studio code in `game/` (HLSDK licence) behind an engine triangle/bone API. |
| Misc | studio_soft.c 287-306, tools/studio_compare/soft_studio.c 40-56, w64seam.py 77-83 (`AngleVectors`); spike/fputest.c 9-18 (`CalcFov`); hl_monsters.c 255-269 (`ED_Alloc` reuse rule); hl_nodes.c 228-243, hl_scripted.c 239-256 (`drop_to_floor`); hl_weapons.c 303-306 (`crandom`); hl_use.c 120-176 (QuakeC `SUB_UseTargets` path for Quake maps: delete), 369-386 (Xash helper) |

### 1.5 HLSDK → `game/` (none importable as-is)

source/hl translates HLSDK game logic in hl_grunt, hl_monsters,
hl_monstermaker, hl_ai, hl_ambient, hl_scripted, hl_sentences (entities;
the parser at 1-505 is ours), hl_nodes, the HLSDK part of hl_nodecook.h,
hl_movers, hl_breakable, hl_items, hl_weapons(.h), hl_logic, hl_env
(372-1166), hl_hud, hl_use(.h) and hl_material(.h). The same applies to
n64/port/gsfx/gsfx_sv.c (env_beam/env_laser/env_sprite) and most of
gsfx_te.c (gibs, blood, grenades, decals: combat.cpp, ggrenade.cpp,
explode.cpp, util.cpp), studio.h 8-207 (MDL structs and macros copied from
HLSDK engine/studio.h) and w64c.py 1349-1446 (PM texture types). Before
they move:

- **Forbidden dependencies.** Every source/hl `.c` file calls our QuakeC
  translations: `FoundTarget`, `M_Visible`, `ai_run`, `walkmonster_start`
  and other `g_*` helpers. hl_ai.c 746-917 is itself a renamed translation
  of qctrans g_ai.c/g_monsters.c (stand/walk/run thinkers, die logic).
  Hostile chase and spawn must be re-implemented from HLSDK
  `schedule.cpp`/`monsters.cpp`, not re-pointed (AuditGameA).
- **Engine coupling.** Re-target all QuakeC entvars access (`ent->v.*`,
  `pr_global_struct`, `EDICT_TO_PROG`, `PR_*`) to the hl64 game API.
- **Licence.** Add the HLSDK LICENSE notice. None of these files carries
  one today.

### 1.6 Forbidden wholesale

- Upstream quakegeneric files (CMakeLists.txt, meson*, build-*.sh,
  clean.sh, source/makefile.*, quakegeneric_{dos,null,w32}.c, quakekeys.h).
- source/g_*.c, g_*.h, game.c, game.h, game_weapons.c (QuakeC
  translations).
- tools/hl_headless.c (Quake host runner).
- tools/test_bsp_load.c.
- Our edits inside Quake files (§4).

### 1.7 Cross-cutting findings

- **Id Software headers** on r64_alias.c, r64_draw.c, r64_light.c,
  r64_main.c, r64_part.c, r64_tex.c, r64_world.c, sys_n64.c and
  quakegeneric_n64.c. Scanning the creating commits gives these exact
  Quake token shares:

  | First version | Exact Quake tokens |
  |---|---:|
  | sys_n64 (e599929) | 50% |
  | r64_main (8343727) | 41% |
  | r64_light (b92a230) | 37% |
  | r64_alias (e749e31) | 22% |
  | r64_draw (8343727) | 13% |
  | r64_tex (3ca6f55) | 6% |
  | r64_world (3ca6f55) | 5% (its `R_MarkLeaves` was verbatim) |
  | r64_part (1ccc03a) | 0% |
  | quakegeneric_n64 | 0% |

  r64_part.c and quakegeneric_n64.c are our code under a template header.
  r64_vid.c, under a contributors header, started at 28%.
- **GPL "quakegeneric contributors" headers** sit on 16 of our files
  (fpu_shim.c, frame_stats.c, q64_brush.c, q64_map.c, q64_map.h,
  q64_route.c, q64_selftest.c, r64_decal.c, r64_hunk.c, r64_local.h,
  r64_malloc.c, r64_mem.c, r64_prof.c, r64_trans.c, r64_vid.c,
  test_harness.h). They are our copyright; replace them on import.
- **Third-party material** to keep attributed:
  - libdragon (public domain), tiny3d (MIT), meshoptimizer (MIT, vendored).
  - dlmalloc (public domain) in q64_heap_host.c.
  - Cephes sin/cos polynomial constants in studio_geom.c SG_SinCos.
  - Khronos GLES headers; Gradle wrapper (Apache-2.0).
  - ares patches (ISC upstream).
- **Data, not code.** spike/streamv2/fs/dome.sct and tools/hl_world/examples
  contain Valve-derived data or reports generated from the user's game
  files. Do not commit them to hl64.
- **Build artefact.** tools/w64c/hlecook is an untracked ELF.

## 2. Per-file verdicts

Columns: lines in this snapshot; git creation commit (or `untracked`); verdict(s); contaminated lines (Quake / Xash / HLSDK / QuakeC-translation, with ranges where they are short); import note. *Review* names the slice whose region-level evidence is in Appendix A (`ProvAudit` = this document, §3/§1.6).

### repository root (upstream quakegeneric + CI)

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| .github/quakegeneric.png | 156 | 3b77b30 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| CMakeLists.txt | 300 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| azure-pipelines.yml | 45 | 3b6a795 2026-08-01 | CL | CLEAN | 0 | CI scaffold (ours). | ProvAudit |
| build-cmake.sh | 18 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| build-meson.sh | 17 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| clean.sh | 4 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| meson.build | 76 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| meson/linux-x86-cross.txt | 11 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| meson/windows-x86-cross.txt | 11 | 0d3b412 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| t12.sum | 309 | untracked | DA | DATA | — | Route run log; not code. | ProvAudit |

### n64/docs

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/docs/bug-audioconv64-vadpcm-huffman-silence.md | 171 | 8942704 2026-07-26 | DOC | CLEAN | 0 | libdragon audioconv64 bug report. Keep as a note if hl64 uses VADPCM+Huffman. | ProvAudit |
| n64/docs/campaign-sizing.md | 142 | untracked | DOC | CLEAN | 0 | Measurements. Optional import. | ProvAudit |
| n64/docs/cwg-format.md | 197 | untracked | DOC | CLEAN | 0 | CWG3 campaign graph format. No copied text. Import to docs/formats/; the one runtime reference (`q64_world.c` fallback, line 124) describes old-tree behaviour, so restate it as a requirement. | ProvAudit |
| n64/docs/e1m3-zone-prototype.md | 137 | 8c3bbf5 2026-07-19 | DOC | CLEAN | 0 | Quake e1m3 zone prototype. Not relevant. | ProvAudit |
| n64/docs/future-qc-native.md | 109 | 8942704 2026-07-26 | DOC | CLEAN | 0 | QuakeC→C notes. Not relevant. | ProvAudit |
| n64/docs/hle-format-v1.md | 135 | untracked | DOC | CLEAN | 0 | HLE1 entity chunk format. No copied text. Import to docs/formats/ after scrubbing 4 old-engine references (model.c `Mod_LoadEntities`, `edict_t.hl_lump`, progs, lines 91 and 105-110). HLSDK class names (`CBaseDelay`, `CTestHull::BuildNodeGraph`) cite behaviour only; keep them. | ProvAudit |
| n64/docs/profiling.md | 207 | 5c5eea5 2026-07-18 | DOC | CLEAN | 0 | How-to for the old tree's PROFILE build. Not needed. | ProvAudit |
| n64/docs/q64-format-v1.md | 504 | a19f97e 2026-07-12 | DOC | CLEAN (obsolete) | 0 | .q64 v1 format, superseded by .w64 v2 and deleted for HL builds. Struct layouts "match" Quake bspfile.h records (dplane_t/dmodel_t layout facts; no copied text). Do not import. | ProvAudit |
| n64/docs/q64-map-streaming.md | 551 | 1ab2c0f 2026-07-11 | DOC | CONTAINS-QUAKE | ~15 (77-81, 89-103) | v1 design doc. Quotes Quake model.c `Mod_LoadLighting` lines and Quake model.h struct members. Do not import. | ProvAudit |
| n64/docs/seamless-uplink.md | 159 | a33b268 2026-07-19 | DOC | CLEAN | 0 | Design notes on removing changelevel. No copied text. Optional historical import. | ProvAudit |
| n64/docs/stream-arch-v2.md | 542 | untracked | DOC | CLEAN | 0 | Architecture and measurements. No copied text. 19 lines name old-tree functions (SV_SpawnServer, SV_Move, world.c, ED_RoomFor, Mod_Studio*). Import as a design/history doc (docs/, not docs/formats/), noting that function names refer to the old tree. | ProvAudit |
| n64/docs/w64-format.md | 432 | untracked | DOC | CLEAN | 0 | .w64 v2 layer package format. No copied text. Import to docs/formats/. Restate 7 lines in prose that point at old-engine functions as their semantics: texture animation sequencing "as Mod_LoadTextures" (73), Quake PVS RLE (318), SV_RecursiveHullCheck epsilon (254, 427), SV_TouchLinks (361), PM_InitTextureTypes/PM_FindTextureType (264-270, HLSDK behaviour, keep as a citation), g_world.c lightstyle strings (142). | ProvAudit |

### n64/port (core)

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/.gitignore | 30 | 8942704 2026-07-26 | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/HL_UPLINK.md | 1207 | 5cfba32 2026-07-14 | DOC | CLEAN | 0 | Old-tree status/design log (1207 lines). No copied text; describes Quake-engine internals. Not importable as a spec. | ProvAudit |
| n64/port/Makefile | 500 | e599929 2026-07-11 | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/build.sh | 69 | e599929 2026-07-11 | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/build_libdragon_banked.sh | 80 | untracked | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/fpu_shim.c | 319 | e599929 2026-07-11 | CL | CLEAN | 0 | Import as-is (relicense header). VR4300 unimplemented-op trap emulator, no engine bindings. | AuditPlatform |
| n64/port/frame_stats.c | 184 | 8343727 2026-07-11 | QI | CLEAN, QUAKE-INTERFACE (trivial) | 0 | Importable; reads r_refdef.vieworg/viewangles only. | AuditRendA |
| n64/port/hl_sources.cmake | 150 | untracked | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/libdragon-patches/0001-rspq-highpri-requested-race.patch | 39 | untracked | CL | CLEAN | 0 | Our patch to libdragon rspq (libdragon is public domain). Import if hl64 pins the same libdragon. | ProvAudit |
| n64/port/libdragon-patches/0002-rspq-lowpri-handoff-atomic.patch | 108 | untracked | CL | CLEAN | 0 | Our patch to libdragon rspq (libdragon is public domain). Import if hl64 pins the same libdragon. | ProvAudit |
| n64/port/per-cluster-geom-wip.patch | 753 | 8942704 2026-07-26 | CL | inherits CONTAINS-QUAKE | see q64_map.c, q64mapc.c | WIP patch to q64_map.c/.h and tools/q64mapc/q64mapc.c (obsolete v1). Do not import. | ProvAudit |
| n64/port/q64_brush.c | 820 | a19f97e 2026-07-12 | CQ | CONTAINS-QUAKE; QUAKE-INTERFACE (heavy) | ~163 Quake: 226-265 (partial, 14 lines counted: 240-248, 253-255, 263-264), 269-351, 511-527, 557-579, 765-779, 790-801 / 0 / 0 / 0 | NOT importable as-is. v1 loader, entire file deleted for HL builds per stream-arch-v2 doc, so recommend not importing at all; if any part is wanted, rewrite from the .q64 spec. | AuditWorld |
| n64/port/q64_budget.c | 674 | de80814 2026-07-14 | QI | QUAKE-INTERFACE | 0 | Retarget: `COM_FOpenFile`, `com_gamedir`, `Hunk_LowMark/HighMark`, `Cache_Flush`, `host_parms`, `Sys_GetEngineMemsize`, `edict_t` sizeof. Budget logic for .q64/.w64 companions is ours. Xash hit 216-217 (big-endian u32 read) is a false positive. | AuditPlatform |
| n64/port/q64_io.c | 374 | untracked | CL | CLEAN | 0 | Import as-is (stream-arch-v2 PI read scheduler). | AuditPlatform |
| n64/port/q64_map.c | 3500 | a19f97e 2026-07-12 | CQ | CLEAN (page/slot/LRU stream caches are ours); CONTAINS-QUAKE (4 small blocks); QUAKE-INTERFACE | ~69 Quake: 1960-1967, 2435-2447, 2453-2455, 2968-2977, 3217, 3298, 3347-3379 / 0 / 0 / 0 | Strip or rewrite the 4 flagged blocks (small field decoders of Mod_LoadTexinfo, Mod_LoadFaces, Mod_LoadPlanes, Mod_DecompressVis). Rest is importable once Quake structs (mplane_t, mleaf_t, msurface_t, mtexinfo_t, texture_t, dclipnode_t, model_t) are retargeted. Also v1-only per docs (deleted for HL v2 builds). | AuditWorld |
| n64/port/q64_map.h | 395 | a19f97e 2026-07-12 | QI | CLEAN (the .q64 on-disk format structs/flags); QUAKE-INTERFACE (API signatures use Quake types) | 0 / 0 / 0 / 0 (all token hits are false positives) | Format part (lines 26-166) importable. Runtime API part (168-389) needs retargeting from Quake types. Node/leaf structs mirror mnode_t/mleaf_t field order (format mimicry, not expression). | AuditWorld |
| n64/port/q64_platform_cfg.h | 43 | untracked | QI | QUAKE-INTERFACE | 0 | Console-command strings for Quake input cvars (`joyadvaxis*`, `+mlook`, binds). Rewrite for the hl64 input layer. | AuditPlatform |
| n64/port/q64_pool.c | 375 | untracked | CL | CLEAN | 0 | Import as-is (address-ordered first-fit residency pools). | AuditPlatform |
| n64/port/q64_route.c | 541 | untracked | QI | CLEAN; QUAKE-INTERFACE (heavy: edicts, cl., Cbuf, pr_strings) | 0 / 0 / 0 / 0 | Test harness; import only if a route runner is wanted in hl64, after retargeting edict/cvar/Cbuf bindings. Carries GPL header. | AuditWorld |
| n64/port/q64_route.h | 23 | untracked | CL | CLEAN (uses qboolean only) | 0 / 0 / 0 / 0 | Importable as-is (qboolean typedef). | AuditWorld |
| n64/port/q64_selftest.c | 193 | untracked | QI | QUAKE-INTERFACE | 0 | Retarget `Con_Printf`, `Hunk_*Mark`, `host_parms`, `Sys_FloatTime`, `COM_*`; route driver hooks. | AuditPlatform |
| n64/port/q64_world.c | 1475 | a33b268 2026-07-19 | QI | CLEAN (own design: CWG3 campaign seams, seam switch, entity pump); QUAKE-INTERFACE (heavy) | 0 / 0 / 0 / 0 | Importable after retargeting the Quake/engine bindings (edict_t + `v.*`, sv./svs., SV_LinkEdict etc.). Algorithm and format are ours. Not Quake-derived. | AuditWorld |
| n64/port/q64_world.h | 66 | a33b268 2026-07-19 | QI | CLEAN; QUAKE-INTERFACE (light: qboolean, `struct edict_s`) | 0 / 0 / 0 / 0 | Importable as-is after replacing the edict/qboolean types. | AuditWorld |
| n64/port/quakegeneric_n64.c | 524 | e599929 2026-07-11 | QI | QUAKE-INTERFACE | 0 (Id header only) | Rewrite as the hl64 N64 entry point; reusable ideas: stack paint, pad mapping, 8-bit→5551 blit (ours). Remove the Id Software header. | AuditPlatform |
| n64/port/r64_alias.c | 1461 | e749e31 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE (id header is real: file began as Quake-derived code) | QUAKE 115 lines: 33-37 (anorms include, data lives in source/anorms.h = id file), 508-572, 612, 640-645, 673-699, 787-797. XASH 0. HLSDK 0. QCtrans 0. | NOT importable as-is: GPL+Id header; RecursiveLightPoint, R_LightPoint remnants, R_AliasSetupFrame group logic and the anorms table must be rewritten. Rest (arena/mesh-cache/frustum/lerp/viewmodel T3D pass) is ours but QUAKE-INTERFACE (entity_t, aliashdr_t, cl., r_refdef). | AuditRendB |
| n64/port/r64_alias_bake.c | 380 | be67cb5 2026-07-14 | QI | QUAKE-INTERFACE (thin), otherwise CLEAN | 0 | Importable after re-targeting model_t/mdl_t/COM_LoadHunkFile/Con_*; GPL-header-less (no header at all). | AuditRendB |
| n64/port/r64_cart.c | 94 | 828c982 2026-07-14 | CL | CLEAN (QUAKE-INTERFACE only via r64_local.h + qboolean) | 0 | Importable after replacing `r64_local.h`/qboolean. Header says "Ported from the OpenLara N64 port (room_stream.cpp, level_prefetch.cpp)": a separate project; its licence must be checked (not Quake/Xash/HLSDK). [INFERENCE: OpenLara is BSD-2-Clause; not verified here.] | AuditRendB |
| n64/port/r64_decal.c | 1547 | untracked | CX | QUAKE-INTERFACE, CONTAINS-XASH, CONTAINS-QUAKE (qbsp texture-axis table) | XASH 85 lines: 403-421, 701-764, 944-945. QUAKE 28 lines: 592-619 (same table/logic in HLSDK utils/qcsg/map.c = Quake-heritage). HLSDK 0 (as HLSDK; same table seen there). QCtrans 0. | NOT importable as-is. Pool/texture residency/block allocator/clipping/draw/pending queue are ours; the 85+28 flagged lines must be re-derived (own overlap-replace policy, own axis-from-normal). Placement geometry source is the cooked w64 format (W64 API), not Quake BSP. | AuditRendB |
| n64/port/r64_draw.c | 695 | 8343727 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE, HLSDK(const) | Quake 48 (463-469, 472-478, 492-500, 521-526, 561-567, 621-632+674-676); Xash 0; HLSDK 2 (296-297 constant); QCtrans 0 | CI8/ui8 pipeline, HUD sheets and fades are original (importable). Cut/rewrite Draw_Character/Draw_String/Draw_CachePic/comment copies; confirmed copied comments per comment_overlap.md. | AuditRendA |
| n64/port/r64_hunk.c | 315 | 9860f4d 2026-07-19 | QI | QUAKE-INTERFACE | 0 | Hunk_AllocName instrumentation; only useful if hl64 keeps a named hunk. Bindings: zone.c hunk marks, `Con_Printf`. | AuditPlatform |
| n64/port/r64_iostat.c | 161 | untracked | CL | CLEAN | 0 | Import (PROFILE-only fread/fseek wraps + PI DMA counters). | AuditPlatform |
| n64/port/r64_light.c | 653 | b92a230 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE | Quake 277 (199-216, 275-325, 349-360, 410-424+436-450, 469-518, 520-548, 550-617, 619-637); Xash 0 (hit 603-609 is Quake lineage); HLSDK 0; QCtrans 0 | Started as "mirrors source/r_light.c" (b92a230). Only the dirty-tracking, hash marks, key fingerprints, overbright scale, v2 branch and FinishLight are new. Rewrite the Quake-shaped functions from the lighting spec, or leave behind. | AuditRendA |
| n64/port/r64_local.h | 371 | 8343727 2026-07-11 | QI | CLEAN, QUAKE-INTERFACE | 0 | Importable as a pattern; includes quakedef.h and uses entity_t/model_t/msurface_t/texture_t/mdl_t/dlight limits. Retarget types. | AuditRendA |
| n64/port/r64_main.c | 215 | 8343727 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE | Quake 53 strong (45-54, 87-129) + weak 2 (147-148); Xash 0; HLSDK 0; QCtrans 0 | Not importable as-is. R_SetVrect and the notexture checker are Quake r_main.c copies. Remainder (R_RenderView sequence, stubs, hooks) is ours but Quake-API-shaped (R_*/D_* seam). | AuditRendA |
| n64/port/r64_malloc.c | 545 | eb3d59c 2026-07-19 | CL | CLEAN (Con_Printf only) | 0 | Import; replace `Con_Printf` with the hl64 log call. `--wrap=malloc` tracker. | AuditPlatform |
| n64/port/r64_mem.c | 450 | 6d4e164 2026-07-11 | QI | QUAKE-INTERFACE | 0 | Retarget: `Cmd_AddCommand/Cmd_Argc/Cmd_Argv` (memstats command), `cl.worldmodel`/`model_t` walks, `Hunk_LowMark/HighMark`, `host_parms`, per-module reporters. Reporters are tied to old modules; mostly rewrite. | AuditPlatform |
| n64/port/r64_part.c | 185 | 1ccc03a 2026-07-11 | QI | QUAKE-INTERFACE (CLEAN content) | weak 3 (108-110); Quake 0 strong; Xash 0; HLSDK 0; QCtrans 0 | TPX packing and draw are original; importable once D_StartParticles/D_DrawParticle/D_EndParticles/particle_t/r_origin/vpn are re-targeted. Header residue. | AuditRendA |
| n64/port/r64_prof.c | 646 | a19f97e 2026-07-12 | QI | CLEAN core + QUAKE-INTERFACE (cvar) | 0 | Import the stage timer/counter core; retarget `Cvar_RegisterVariable/Cvar_Set` (`r_profile`) and the old-module reporter externs. Automated hit 284-293 is a string table, false positive. | AuditPlatform |
| n64/port/r64_sector.c | 2089 | untracked | QI | CLEAN (with QUAKE-INTERFACE, CONTAINS-QUAKE minor) | Quake 40 (431-453 = 23 TexAnim; 634-649 = 16 dlight falloff sequence; +1 header cmt) + weak 7 (540-546); Xash 0; HLSDK 0; QCtrans 0 | Mostly original (w64 meshlet renderer, flashlight pool, RDP bind lists, seam cross-draw). Excise TexAnim and the dlight falloff block or rewrite; re-target ~25 Quake bindings. Untracked: no history. | AuditRendA |
| n64/port/r64_studio.c | 899 | dcce7e3 2026-07-14 | QI | QUAKE-INTERFACE, HLSDK (small idiom) | HLSDK 11 lines: 599-609 (looping/clamp wrap of StudioEstimateFrame; also Xash R_StudioEstimateFrame). XASH 0. QUAKE 0. | Importable after re-targeting entity_t/model_t/cl.*; the 11 lines may go to game/ (HLSDK licence) or be rewritten. Bones/chrome math is NOT in this file (studio_geom.c, other slice). | AuditRendB |
| n64/port/r64_studio_bake.c | 2142 | bcc05dc 2026-07-14 | CX | QUAKE-INTERFACE, CONTAINS-XASH (chrome basis), HLSDK (small idiom) | XASH 52 lines: 828-879 (chrome_axes; algorithm also in HLSDK utils/mdlviewer StudioModel::Chrome but with the Xash cross-product order). HLSDK 13 lines: 2036-2048 (frame wrap idiom). QUAKE 0. | NOT importable as-is until chrome_axes is rewritten (or sourced from HLSDK mdlviewer Chrome under the HLSDK licence with its own sign convention). The rest (STB2 loader, async bake IO, rspq block build, meshlets, vm skin tables) is ours. | AuditRendB |
| n64/port/r64_studio_stubs.c | 122 | 0fe9d97 2026-07-14 | QI | QUAKE-INTERFACE (trivial) | 0 | Importable (no-op stubs). Bindings: model_t, entity_t, cl.time, Mod_StudioUnload. | AuditRendB |
| n64/port/r64_studio_sw.c | 449 | b6ea17c 2026-07-14 | QI | QUAKE-INTERFACE | 0 | Importable after re-targeting cvar_t/Cvar_*, vid.*, r_refdef, entity_t, cl.*. Compare harness is ours. Calls studio_geom/studio_soft (other slices). | AuditRendB |
| n64/port/r64_sys.h | 39 | untracked | CL | CLEAN | 0 | Importable as-is (pure declarations, no Quake types, no licence header). Untracked. | AuditRendB |
| n64/port/r64_tex.c | 801 | 3ca6f55 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE | Quake 38 (725-762 R64_TextureAnimation); Xash 0; HLSDK 0; QCtrans 0 | Everything else (CI4/CI8 quantiser, slot table, TMEM bind) is original. Replace R64_TextureAnimation (or take it from clean spec) and r_notexture-independent code is importable. | AuditRendA |
| n64/port/r64_trans.c | 950 | untracked | CX | QUAKE-INTERFACE, CONTAINS-XASH | XASH 117 lines: 87-150 (CL_FxBlend), 592-644 (sprite orientation, R_DrawSpriteModel). Quake-lineage: the sprite orientation also exists in Quake r_sprite.c R_DrawSprite. HLSDK 0. QCtrans 0. | NOT importable as-is. FxBlend + SpriteQuad must be redone from the public behaviour spec (HLSDK const.h enums) without the Xash structure. SC4 cache/ModeBegin RDP blend/pass/sort are ours. | AuditRendB |
| n64/port/r64_vid.c | 258 | 8343727 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE minor | Quake 11 (162-172) + 2 stubs (252-258); Xash 0; HLSDK 0; QCtrans 0 | Present pacing, palette, TLUT, frame bracket are original (importable). VID_Init's viddef_t setup is vid_null.c-derived; rewrite for new engine's own vid struct. | AuditRendA |
| n64/port/r64_world.c | 2977 | 3ca6f55 2026-07-11 | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE | Quake strong 67 (40 = 1; MarkLeaves 1555-1566+1575-1602 = 40; SurfaceFacing 1604-1612 = 9; ModelSpaceView 2300-2316 = 17) + weak 76 (359-372, 379-385, 2081-2098, 2120-2121, 2413-2433, 2479-2494); Xash 0; HLSDK 0; QCtrans 0 | Not importable as-is. v1 world path is Quake-derived (MarkLeaves was a verbatim copy at the first commit). Arena/bake/batch code (~1900 lines) is ours but bound to model_t/msurface_t/cl.*. v2 dispatch + Setup3D/projection/matrix helpers (2136-2260, v2 parts of 2620-2977) are clean once the v1 `#if Q64_WORLD_V1` blocks are cut. | AuditRendA |
| n64/port/r64_world_atlas.c | 379 | 5c5eea5 2026-07-18 | QI | CLEAN, QUAKE-INTERFACE | 0 | Importable after re-targeting COM_LoadHunkFile, Con_*, MAX_QPATH. No licence header present. Hit vs Xash lib_apple.c is a false positive (big-endian load). | AuditRendA |
| n64/port/r64_world_tex.c | 220 | 64e3131 2026-07-14 | QI | CLEAN, QUAKE-INTERFACE | 0 | Same as r64_world_atlas.c; depends on world_tex_bake.h (not in slice). Xash hit false positive. | AuditRendA |
| n64/port/snd_n64.c | 1862 | da63eca 2026-07-18 | QI | QUAKE-INTERFACE; CLEAN (backend expression ours); Quake borderline noted | 0/0/0/0. Borderline: 105-106 (2 cvar declarations identical to Quake snd_dma.c 71-72); 611-667 (Quake SND_Spatialize attenuation model, rewritten) | Implements the Quake `sound.h` S_* API over the libdragon mixer + wav64. There is no Quake mixer / resampler / DMA / ambient code and no Xash s_*.c code. Re-target the S_* API and cvar/cmd/con/cl./sv. bindings. Rename the Quake channel_t field vocabulary (master_vol, dist_mult) when it is re-expressed. VADPCM decoding is done by libdragon (RSP); this file only selects the format. | AuditFxSound |
| n64/port/sys_n64.c | 305 | e599929 2026-07-11 | CQ | CONTAINS-QUAKE, QUAKE-INTERFACE | Quake ~185 (26-168, 264-305) | Do not import. Our RSP-crash `--wrap` (205-250) and the on-screen error painter (169-203) are ours and small enough to re-type from the description. | AuditPlatform |
| n64/port/test_harness.h | 59 | 31301d0 2026-07-11 | CL | CLEAN | 0 | Import (ares test-harness register map). | AuditPlatform |
| n64/port/w64_stream.c | 2286 | untracked | QI | CLEAN; QUAKE-INTERFACE (edict/sv/cl/r_refdef reads) | 0 / 0 / 0 / 0 | Importable after retargeting the engine reads (list below). Residency pools/steal/slide/pin scheduler are original; collision-grid want logic is not Quake areanodes. | AuditWorld |
| n64/port/w64_world.c | 1334 | untracked | CQ | CONTAINS-QUAKE; QUAKE-INTERFACE; rest CLEAN (package open/CRC/directory/layer slots/grid start node/PVS row cache) | ~286 Quake: 443-458, 706-721, 797-799, 859-878, 880-996, 1077-1099, 1115-1144, 1157-1182, 1233-1267 / 0 / 0 / 0 | NOT importable as-is. File header (lines 6-8) and w64_world.h (15-18) state the hull walks are "line-for-line replicas" of world.c / model.c / sv_main.c; verified. Replace the 9 regions with a clean-room implementation; the remaining ~1050 lines are ours. | AuditWorld |

### n64/port/android

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/android/.gitignore | 5 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/app/build.gradle.kts | 85 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/app/src/main/AndroidManifest.xml | 27 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/app/src/main/cpp/CMakeLists.txt | 42 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/app/src/main/java/com/jshield/hluplink/HLActivity.java | 37 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/app/src/main/res/values/strings.xml | 4 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/build.gradle.kts | 3 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/build_ares-host.sh | 48 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/gradle.properties | 3 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/gradle/wrapper/gradle-wrapper.jar | 158 | untracked | TP | THIRD-PARTY | — | Khronos GLES/KHR headers (Khronos MIT-style notice) or Gradle wrapper (Apache-2.0). Take fresh copies from upstream if needed. | ProvAudit |
| n64/port/android/gradle/wrapper/gradle-wrapper.properties | 5 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/gradlew | 185 | untracked | TP | THIRD-PARTY | — | Khronos GLES/KHR headers (Khronos MIT-style notice) or Gradle wrapper (Apache-2.0). Take fresh copies from upstream if needed. | ProvAudit |
| n64/port/android/settings.gradle.kts | 16 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |
| n64/port/android/stage_assets.sh | 49 | untracked | CL | CLEAN | 0 | Our Android shell / build (package com.example.hluplink). Reusable for an hl64 Android build. | ProvAudit |

### n64/port/gles

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/gles/glr.h | 43 | untracked | CL | CLEAN | 0 | importable as-is; platform seam only | AuditGles |
| n64/port/gles/glr_alias.c | 474 | untracked | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE | Quake 227-247, 266-282, 375-380 (≈ 45); Xash 0; HLSDK 0; QCtrans 0 | rewrite PoseVerts/SetupFrames group-frame selection and the seam/ST decode independently; depends on `anorms.h` (id data file) | AuditGles |
| n64/port/gles/glr_beam.c | 514 | untracked | CX | CONTAINS-XASH (almost the whole file), QUAKE-INTERFACE (3 refs) | Xash 43-66, 133-229, 231-295, 297-349, 351-384, 386-422, 435-446, 451-452, 483-486, 504-507 (≈ 332) | **do not import**; re-derive from the GoldSrc behaviour spec / own design | AuditGles |
| n64/port/gles/glr_decal.c | 873 | untracked | CX | CONTAINS-XASH, CONTAINS-HLSDK (Quake-tools heritage), QUAKE-INTERFACE | Xash 242-260, 449-512 (≈ 83); Quake-tools/HLSDK 324-351 (28); Quake 0 | import after rewriting DecalAlloc, Intersect and BaseAxes; rest (WAD3 reader, clipper, pool, pending retry, drawing) is ours | AuditGles |
| n64/port/gles/glr_draw.c | 711 | untracked | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE (minor, API-shaped) | Quake 399-408, 415-419, 587-593, 609-614 (≈ 24, all minor) | importable after re-typing the ~24 lines; 2D queue/shader/HUD/overlay code is ours | AuditGles |
| n64/port/gles/glr_fx.h | 55 | untracked | CL | CLEAN (generic macro) | 0 | importable; comment cites Xash behaviour only | AuditGles |
| n64/port/gles/glr_gl.c | 387 | untracked | QI | CLEAN, QUAKE-INTERFACE | 0 | importable; re-target `COM_OpenFile/Sys_FileRead/Sys_Error/AngleVectors/entity_t/r_origin` | AuditGles |
| n64/port/gles/glr_hlpart.c | 256 | untracked | CX | CONTAINS-XASH (tracers), QUAKE-INTERFACE (r_origin/vpn…) | Xash 146-196 (≈ 51) | point-particle half (1-145, 198-256) is CLEAN; rewrite tracer quad | AuditGles |
| n64/port/gles/glr_light.c | 91 | untracked | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE (small) | Quake 29-40, 55-64 (≈ 22) | rewrite lightstyle animation + dlight accumulation; `r_lightscale`/probe code is ours | AuditGles |
| n64/port/gles/glr_local.h | 232 | untracked | QI | QUAKE-INTERFACE | 0 | header is an interface list: includes `quakedef.h`, uses `entity_t/mleaf_t/cvar_t/dlight_t`; retarget when moving | AuditGles |
| n64/port/gles/glr_main.c | 707 | untracked | CQ | CONTAINS-QUAKE (exact copy), QUAKE-INTERFACE (heavy) | Quake 460-465, 538-569, 263-268, 624-625, +data 34-36 (≈ 51) | **R_SetVrect (538-569) is a verbatim copy of Quake**; rest ours but VID_/R_/D_ seam is Quake API; `anorms.h` is an id file | AuditGles |
| n64/port/gles/glr_part.c | 138 | untracked | QI | CLEAN, QUAKE-INTERFACE (D_ hooks) | 0 | importable; retarget `D_StartParticles/D_DrawParticle/D_EndParticles`, `particle_t` | AuditGles |
| n64/port/gles/glr_shot.c | 73 | untracked | CL | CLEAN | 0 | importable (PNG/PPM writers; zlib) | AuditGles |
| n64/port/gles/glr_sprite.c | 361 | untracked | CX | CONTAINS-XASH (orientation + quad), possible-XASH (FxMode table), CLEAN rest | Xash 282-320, 325-328 (≈ 43); low-confidence 117-143 (27) | rewrite GLR_SpriteDraw's orientation switch; shader/frame-tex/Dilate/stream plumbing is ours | AuditGles |
| n64/port/gles/glr_studio.c | 1330 | untracked | CX | QUAKE-INTERFACE, CONTAINS-HLSDK/XASH (small: chrome, frame wrap) | HLSDK/Xash 139-149, 635-648, 1104-1107 (≈ 29) | bake/skin-on-GPU/selection is ours; re-derive the 3 small blocks from HLSDK (allowed under HLSDK licence into `game/`, or re-derive clean) | AuditGles |
| n64/port/gles/glr_trans.c | 200 | untracked | CX | CONTAINS-XASH (small), QUAKE-INTERFACE | Xash 62-65, 85, 100-105, 138-142, 160 (≈ 14, low-medium) | importable after re-deriving sort key / shell-scale formula | AuditGles |
| n64/port/gles/glr_world.c | 1623 | untracked | CQ | CONTAINS-QUAKE (2 blocks), possible-XASH (1 block), QUAKE-INTERFACE; rest CLEAN | Quake 215-231, 743-761 (≈ 36); Xash low-confidence 1284-1314 (31) | w64-stream world renderer is ours; rewrite TexAnim and the dlight falloff shader; tied to `w64_*` cart format, not to Quake BSP | AuditGles |
| n64/port/gles/include/GLES3/gl3.h | 1211 | untracked | TP | THIRD-PARTY | — | Khronos GLES/KHR headers (Khronos MIT-style notice) or Gradle wrapper (Apache-2.0). Take fresh copies from upstream if needed. | ProvAudit |
| n64/port/gles/include/GLES3/gl3platform.h | 38 | untracked | TP | THIRD-PARTY | — | Khronos GLES/KHR headers (Khronos MIT-style notice) or Gradle wrapper (Apache-2.0). Take fresh copies from upstream if needed. | ProvAudit |
| n64/port/gles/include/KHR/khrplatform.h | 271 | untracked | TP | THIRD-PARTY | — | Khronos GLES/KHR headers (Khronos MIT-style notice) or Gradle wrapper (Apache-2.0). Take fresh copies from upstream if needed. | ProvAudit |
| n64/port/gles/test/glr_sheet.py | 44 | untracked | CL | CLEAN | 0 | importable (PIL contact sheet) | AuditGles |
| n64/port/gles/test/run_glr_uplink.sh | 78 | untracked | CL | CLEAN | 0 | importable only if hl64 keeps the same cart/hl_sdl layout | AuditGles |

### n64/port/gsfx

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/gsfx/gsfx.h | 668 | untracked | QI | QUAKE-INTERFACE; HLSDK interface constants (not counted as contamination) | 0/0/0/0. Interface values: HLSDK const.h/beamdef.h/cbase.h in 78-96, 109-127, 142-221; Quake spritegn.h values in 99-103 | Prose is ours. Constant values are protocol / file-format interface: keep the values, re-declare them in an HLSDK-licensed `game/` header or a neutral "hl protocol" header. Prototypes use edict_t / entity_t / client_t / trace_t / model_t, so they need re-targeting. | AuditFxSound |
| n64/port/gsfx/gsfx_cart.c | 1492 | untracked | CX | QUAKE-INTERFACE, CONTAINS-XASH, HLSDK | 0/501/141/0 | Not importable as-is. Effect builders (blood, explosion, impact, debris, sprite spray, multigunshot, tent alloc, ricochet chance) are structural translations of Xash cl_tent.c / cl_efx.c, and must be clean-room rewritten from a behaviour spec. The TentsUpdate physics and the spark shower are HLSDK and go to `game/`, or get rewritten. | AuditFxSound |
| n64/port/gsfx/gsfx_cl.c | 2198 | untracked | CX | QUAKE-INTERFACE, CONTAINS-XASH, HLSDK | 0/721/166/0 | Same as gsfx_cart.c. In addition, GSFXC_FxBlend and GlowBlend are translations of Xash CL_FxBlend / R_SpriteGlowBlend. Our own parts can be kept: names / objects / attachments / draw lists / client models / palette. | AuditFxSound |
| n64/port/gsfx/gsfx_spr.c | 260 | untracked | CQ | CLEAN (mostly), CONTAINS-QUAKE (minor) | 28/0/0/0 | Importable after rewriting the 12-line group-frame selection (Quake R_GetSpriteframe) and the 4-line origin→bounds mapping (Quake Mod_LoadSpriteFrame). The SPR v2 parser itself is ours. | AuditFxSound |
| n64/port/gsfx/gsfx_sv.c | 1617 | untracked | HL | QUAKE-INTERFACE, HLSDK | 0/0/711/0 | The name table, svc_hlfx sync, FX-object store and flush budget are ours, but bound to the Quake server (sv./svs./MSG_/SV_Move/COM_Parse). The env_sprite/glow/beam/laser/infodecal logic translates HLSDK effects.cpp: it belongs in `game/` under the HLSDK licence. | AuditFxSound |
| n64/port/gsfx/gsfx_te.c | 1045 | untracked | HL | QUAKE-INTERFACE, CONTAINS-QUAKE, HLSDK | 40/0/432/0 | The TE writers are ours (protocol = HLSDK const.h payloads). The game helpers are HLSDK translations and go to `game/`. te_trace is a translation of Quake SV_Move / SV_ClipToLinks and must be rewritten. | AuditFxSound |
| n64/port/gsfx/gsfx_util.c | 56 | untracked | CL | CLEAN | 0/0/0/0 | Importable. The only Quake binding is Q_strncasecmp, a trivial swap. The xorshift generator is a public algorithm. | AuditFxSound |

### n64/port/host

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/host/libdragon.h | 82 | untracked | CL | CLEAN | 0 | Host stub of libdragon types; import. | AuditPlatform |
| n64/port/host/q64_heap_host.c | 702 | untracked | CL | CLEAN (third-party algorithm: dlmalloc 2.6.4, public domain) | 0 | Import; note in the commit that it re-implements newlib's mallocr.c (Doug Lea, public domain) step for step. | AuditPlatform |
| n64/port/host/q64_heap_host.h | 23 | untracked | CL | CLEAN | 0 | Import. | AuditPlatform |
| n64/port/host/q64_host.c | 93 | untracked | QI | QUAKE-INTERFACE | 0 | Retarget `QG_Create` boot order, `Cmd_Argc/Argv`, `Hunk_*Mark`, `Mod_*` stats, `S_*` stats, `quakeparms_t`. | AuditPlatform |
| n64/port/host/q64_host.h | 14 | untracked | QI | QUAKE-INTERFACE | 0 | `quakeparms_t`. | AuditPlatform |
| n64/port/host/q64_host_audio.c | 131 | untracked | CL | CLEAN | 0/0/0/0 | Importable. It is a stub of the libdragon mixer/wav64 API, and the header layout comes from libdragon wav64_internal.h (Unlicense). Bindings: Sys_Error, realtime. | AuditFxSound |
| n64/port/host/q64_host_render.c | 506 | untracked | CQ | QUAKE-INTERFACE, CONTAINS-QUAKE (small) | Quake ~25 (49-67 VID_Init body, 76-81 notexture setup, 461-462 Draw_PicFromWad) | Do not import; it implements the Quake `R_*`/`VID_*`/`Draw_*` seam for the headless parity host. Rewrite against the hl64 renderer seam. | AuditPlatform |
| n64/port/host/q64_io_host.c | 256 | untracked | CL | CLEAN (Sys_Error only) | 0 | Import; replace `Sys_Error`. pread-backed virtual PI address space. | AuditPlatform |
| n64/port/host/q64_io_host.h | 30 | untracked | CL | CLEAN | 0 | Import. | AuditPlatform |

### n64/port/modelviewer

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/modelviewer/Makefile | 56 | 8942704 2026-07-26 | CL | CLEAN | none | libdragon/tiny3d build glue. | AuditToolsStudio |
| n64/port/modelviewer/README.md | 45 | 8942704 2026-07-26 | CL | CLEAN | none | Own doc. | AuditToolsStudio |
| n64/port/modelviewer/gen_index.py | 100 | 8942704 2026-07-26 | CL | CLEAN | none | Directory index generator. | AuditToolsStudio |
| n64/port/modelviewer/hostfs_9p.c | 631 | 8942704 2026-07-26 | CL | CLEAN | none | 9P2000 client (public protocol). Near-copy of ~/src/ports/homeworld/n64/src/hostfs_9p.c (5 diff lines): sibling-project provenance, not Quake/Xash/HLSDK. | AuditToolsStudio |
| n64/port/modelviewer/hostfs_9p.h | 34 | 8942704 2026-07-26 | CL | CLEAN | none | Own header; byte-identical to ~/src/ports/homeworld/n64/src/hostfs_9p.h (sibling-project reuse). | AuditToolsStudio |
| n64/port/modelviewer/main.c | 84 | 8942704 2026-07-26 | CL | CLEAN | none | Own libdragon main. | AuditToolsStudio |
| n64/port/modelviewer/run_ares_shot.sh | 57 | 8942704 2026-07-26 | CL | CLEAN | none | Shell glue. | AuditToolsStudio |
| n64/port/modelviewer/sc64_9p_agent.py | 412 | 8942704 2026-07-26 | CL | CLEAN | none | 9P server over SC64 USB; 19 diff lines vs homeworld/n64/sc64_9p_agent.py (sibling-project copy). | AuditToolsStudio |
| n64/port/modelviewer/sc64_link.c | 322 | 8942704 2026-07-26 | CL | CLEAN | none | Header says "Simplified from Homeworld n64/src/sc64_link.c" (836 diff lines vs it); SC64 register map from SummerCart64 docs. Cross-project provenance only. | AuditToolsStudio |
| n64/port/modelviewer/sc64_link.h | 23 | 8942704 2026-07-26 | CL | CLEAN | none | Own header. | AuditToolsStudio |
| n64/port/modelviewer/sc64_session.py | 259 | 8942704 2026-07-26 | CL | CLEAN | none | Remote input/screenshot client; mostly different from homeworld/n64/sc64_session.py (791 diff lines). | AuditToolsStudio |
| n64/port/modelviewer/viewer.c | 1546 | 8942704 2026-07-26 | CQ | CONTAINS-QUAKE (format constants only) | QUAKE 7 (33-39 ALIAS_* constants = modelgen.h) + comment 497 (daliasframe_t layout) | Own model browser on libdragon/tiny3d; reads AMB2/STB2 bakes and IDPO MDL. Replace the constants with own spec values. | AuditToolsStudio |
| n64/port/modelviewer/viewer.h | 20 | 8942704 2026-07-26 | CL | CLEAN | none | Own header. | AuditToolsStudio |

### n64/port/sdl

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/sdl/hl_sdl_sources.cmake | 46 | untracked | CL | CLEAN | 0 | Build glue for the old tree; lists Quake sources (QUAKE_SRC_DIR). Pattern reusable, file not. | ProvAudit |
| n64/port/sdl/sdl_audio.c | 702 | untracked | CL | CLEAN + LIBDRAGON-derived (Unlicense) | 0/0/0/0. LIBDRAGON: 329-461 | Importable (Unlicense is MIT-compatible; add an attribution comment). The mixer callback and SDL device are ours. The VADPCM frame decoder and Huffman front end are a port of libdragon wav64_vadpcm.c (vadpcm_decode, wav64_vadpcm_init_huffman, huffv_decompress), as its header states. Bindings: Sys_Printf, Sys_Error. | AuditFxSound |
| n64/port/sdl/sdl_content.c | 199 | untracked | CL | CLEAN (Sys_Printf only) | 0 | Import (Android asset → internal storage copier). | AuditPlatform |
| n64/port/sdl/sdl_input.c | 1130 | untracked | QI | QUAKE-INTERFACE, CONTAINS-QUAKE (tiny) | Quake ~11 (128-139 `look_turn` pitch clamp 80/-70 + `V_StopPitchDrift`, as in_win.c IN_MouseMove) | Retarget: Quake key codes `K_*`, `cl.viewangles`, cvars `m_pitch`/`sensitivity` (`Cvar_*`), `key_dest`, `host_frametime`, `QG_GetKey/QG_GetMouseMove/QG_GetJoyAxes`. Touch overlay and weapon wheel are ours. Replace `look_turn` with a spec-derived clamp. | AuditPlatform |
| n64/port/sdl/sdl_local.h | 30 | untracked | CL | CLEAN | 0 | Import. | AuditPlatform |
| n64/port/sdl/sdl_main.c | 492 | untracked | QI | QUAKE-INTERFACE | 0 | Retarget `COM_InitArgv`, `Host_Init/Host_Frame/Host_Shutdown`, `Cbuf_AddText`, `quakeparms_t`, `Con_Printf`, `Sys_Printf`. SDL/GL window, log thread, route loading are ours. HLSDK hit 362-369 (argv parsing) false positive. | AuditPlatform |
| n64/port/sdl/sdl_platform_cfg.h | 50 | untracked | QI | QUAKE-INTERFACE | 0 | Quake console-command strings; rewrite. | AuditPlatform |
| n64/port/sdl/sys_sdl.c | 178 | untracked | CQ | CONTAINS-QUAKE, QUAKE-INTERFACE | Quake ~85 (19-84 file-handle table + Sys_File* from sys_null.c/sys_win.c; 137-178 stub tail) | Do not import. | AuditPlatform |

### n64/port/spike

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/spike/Makefile | 40 | e599929 2026-07-11 | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| n64/port/spike/blit2d.c | 69 | 8343727 2026-07-11 | CL | CLEAN | none | Own libdragon repro. | AuditToolsStudio |
| n64/port/spike/fputest.c | 58 | e599929 2026-07-11 | CQ | CONTAINS-QUAKE (test fixture) | QUAKE 10 (9-18 calcfov = Quake screen.c CalcFov, renamed, bad-fov check removed) | Throwaway FPU test; do not import. | AuditToolsStudio |
| n64/port/spike/main.c | 158 | e599929 2026-07-11 | CL | CLEAN | none | Own risk-retirement spike. | AuditToolsStudio |
| n64/port/spike/streamv2/Makefile | 19 | untracked | CL | CLEAN | none | Build glue (untracked). | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/bsp30.py | 175 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/clipcut.py | 42 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/cook_sector.py | 151 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/mdl.py | 22 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/overlap.py | 47 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/parse_prof.py | 40 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/scene.py | 146 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/analysis/spine.py | 43 | untracked | CL | CLEAN | none | Throwaway analysis; BSP30 format reader / own measurement code. | AuditToolsStudio |
| n64/port/spike/streamv2/fs/dome.sct | 190 | untracked | DA | ASSET (Valve-derived data) | n/a (binary) | Cooked sector blob from HL map geometry (hldemo/uplink content): game asset, not code; do not import. | AuditToolsStudio |
| n64/port/spike/streamv2/main.c | 360 | untracked | CL | CLEAN | none | Own PI/DMA benchmark. | AuditToolsStudio |
| n64/port/spike/streamv2/out/cart-route-profile-96k.json | 1 | untracked | DA | DATA (run output) | n/a | Profiling output; numbers only. | AuditToolsStudio |
| n64/port/spike/streamv2/out/cart-route-profile-clean-256k.json | 1 | untracked | DA | DATA (run output) | n/a | Profiling output; numbers only. | AuditToolsStudio |
| n64/port/spike/streamv2/out/scene-working-sets.json | 4939 | untracked | DA | DATA (run output) | n/a | Per-scene size statistics from HL demo maps (numbers); not code. | AuditToolsStudio |
| n64/port/spike/t3dtex.c | 160 | e7383c7 2026-07-11 | CL | CLEAN | none | Own tiny3d spike. | AuditToolsStudio |

### n64/port/test

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/port/test/README.md | 404 | 31301d0 2026-07-11 | DOC | CLEAN | 0 | Test-harness how-to for the old tree. | ProvAudit |
| n64/port/test/ares-large-rom-isviewer.patch | 59 | untracked | CL | CLEAN | 0 | Our patch to the ares emulator (ISC) for the test harness / ISViewer. Import with the test harness. | ProvAudit |
| n64/port/test/ares-testharness.patch | 207 | 31301d0 2026-07-11 | CL | CLEAN | 0 | Our patch to the ares emulator (ISC) for the test harness / ISViewer. Import with the test harness. | ProvAudit |
| n64/port/test/capture_video.sh | 73 | d2bc31a 2026-07-15 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/dry_named_route.cfg | 32 | untracked | CL | CLEAN (data) | 0 | Route script in old-engine console commands (setpos, wait*, fire, impulse). Reusable only as a behaviour script for an hl64 route runner. | ProvAudit |
| n64/port/test/elf-only.mk | 13 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/gen_hl_walk_route.py | 369 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/hl_material_probe.py | 154 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/hl_material_route.cfg | 25 | untracked | CL | CLEAN (data) | 0 | Route script in old-engine console commands (setpos, wait*, fire, impulse). Reusable only as a behaviour script for an hl64 route runner. | ProvAudit |
| n64/port/test/npc_anim_route.cfg | 98 | untracked | CL | CLEAN (data) | 0 | Route script in old-engine console commands (setpos, wait*, fire, impulse). Reusable only as a behaviour script for an hl64 route runner. | ProvAudit |
| n64/port/test/run_hl_material_host.sh | 57 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_hl_walk_host.sh | 130 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_hldemo_walk.sh | 461 | 8ca78f4 2026-07-18 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_host_studio_regress.sh | 93 | 8ca78f4 2026-07-18 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_selftest.sh | 51 | 31301d0 2026-07-11 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_studio_compare.sh | 243 | bcc05dc 2026-07-14 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_uplink_host_playthrough.sh | 190 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_uplink_playthrough.sh | 198 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/run_white_viewmodel.sh | 79 | b6ea17c 2026-07-14 | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |
| n64/port/test/uplink_route.cfg | 738 | untracked | CL | CLEAN (data) | 0 | Route script in old-engine console commands (setpos, wait*, fire, impulse). Reusable only as a behaviour script for an hl64 route runner. | ProvAudit |
| n64/port/test/xvfb_private.sh | 31 | untracked | CL | CLEAN | 0 | Test driver for the old tree (log prefixes, console commands, ares harness). No similarity or comment hits; not read line by line beyond a provenance grep. | ProvAudit |

### n64/smoketest

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| n64/smoketest/Makefile | 22 | e599929 2026-07-11 | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| n64/smoketest/main.c | 23 | e599929 2026-07-11 | CL | CLEAN | none | libdragon+tiny3d init smoke test. | AuditToolsStudio |

### source (other)

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| source/alias_bake.h | 58 | be67cb5 2026-07-14 | CL | CLEAN | none | Own big-endian bake file layouts (STB2/AMB2/UI8/WATS/WCI4); no Quake/Xash/HLSDK text. No licence header. | AuditToolsStudio |
| source/makefile.wat | 86 | 9e8ce48 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/makefile.win | 78 | ebfdff5 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/mod_studio.c | 2171 | dcce7e3 2026-07-14 | QI | QUAKE-INTERFACE | none | Engine-integrated loader/pool (model_t, sv./cl., COM_*, Sys_*, Con_*); retarget to the new engine. IDST swap functions are format code, not copies. No licence header present. | AuditToolsStudio |
| source/q64_io.h | 95 | untracked | CL | CLEAN | 0 | Import. | AuditPlatform |
| source/q64_limits.h | 253 | untracked | CL | CLEAN (numbers refer to Quake MAX_* names) | 0 | Import; rename the Quake limit macros it overrides (`MAX_EDICTS` etc.). | AuditPlatform |
| source/q64_pool.h | 80 | untracked | CL | CLEAN | 0 | Import. | AuditPlatform |
| source/q64_prof.h | 127 | untracked | CL | CLEAN | 0 | Import (zone enum names mention old functions in comments only). | AuditPlatform |
| source/qc/README.md | 69 | 66bcd22 2026-07-14 | DOC | CLEAN (describes forbidden code) | 0 | QuakeC→C port status. Not relevant. | ProvAudit |
| source/quakegeneric_dos.c | 96 | 250d1a9 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/quakegeneric_null.c | 61 | 9e8ce48 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/quakegeneric_w32.c | 317 | 2dd3e94 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/quakekeys.h | 122 | 226d091 2023-07-07 | FB | FORBIDDEN (upstream quakegeneric, GPL) | — | Upstream quakegeneric (erysdren et al., 2023-07..2025-01, before our first commit); GPL. Not importable. | ProvAudit |
| source/studio.h | 287 | dcce7e3 2026-07-14 | HL | HLSDK (format structs); QUAKE-INTERFACE (209-285) | HLSDK/XASH 8-207 (~200): macros + all struct field lists copied, comments dropped | Interface/format definitions copied from HLSDK/hlsdk-portable engine/studio.h; can live in game/ under HLSDK licence, MIT engine must re-declare from the format spec. 209-285 is ours (Mod_Studio* residency API on struct model_s). | AuditToolsStudio |
| source/studio_bake.h | 123 | bcc05dc 2026-07-14 | CL | CLEAN | none | Own big-endian bake file layouts (STB2/AMB2/UI8/WATS/WCI4); no Quake/Xash/HLSDK text. No licence header. | AuditToolsStudio |
| source/studio_geom.c | 1010 | b6ea17c 2026-07-14 | CX | CONTAINS-XASH; CONTAINS-HLSDK (same code, Xash derives from HLSDK); QUAKE-INTERFACE (types only) | XASH 425 / HLSDK 425 (identical ranges: 77-172,174-188,229-391,399-517,586-623); generic maths 190-225 (36) not counted | NOT importable as-is. Ours and importable after rewrite: 36-75 (SG_Short, SG_SinCos), 519-584 (ClockFrame/ClockBlend), 627-701, 703-880 (tricmd walk, format-driven), 882-914, 916-1010. Cephes sin/cos constants need attribution. | AuditToolsStudio |
| source/studio_geom.h | 130 | b6ea17c 2026-07-14 | CL | CLEAN | none | Draw-stream/param/clock types, ours. Includes studio.h (HLSDK structs). Comments say "Xash-faithful" (intent, not copy). | AuditToolsStudio |
| source/studio_soft.c | 605 | b6ea17c 2026-07-14 | CQ | CONTAINS-QUAKE; QUAKE-INTERFACE (quakedef.h types only) | QUAKE 20 (287-306 angle_vectors = Quake AngleVectors, renamed); comment 466 "stupid quake bug" echoes Xash/Quake | Everything else (framebuffer, edge-function raster, skin expand, projection) is ours. Replace angle_vectors with own derivation (or drop dependency) before import. | AuditToolsStudio |
| source/studio_soft.h | 82 | b6ea17c 2026-07-14 | CL | CLEAN | none | Types + prototypes, ours. | AuditToolsStudio |
| source/ui_bake.h | 34 | 64e3131 2026-07-14 | CL | CLEAN | none | Own big-endian bake file layouts (STB2/AMB2/UI8/WATS/WCI4); no Quake/Xash/HLSDK text. No licence header. | AuditToolsStudio |
| source/w64_format.h | 297 | untracked | CL | CLEAN; HLSDK data constant at 110-120 (material letters) | 0 / 0 / ~3 HLSDK (110-114 comment, 119 constant string; letter set from hlsdk/pm_shared/pm_shared.c:93-103) / 0 | Importable (HLSDK data goes under HLSDK licence if it lands in game/; it is only a letter table). Token hits are false positives. | AuditWorld |
| source/w64_node.h | 81 | untracked | QI | CLEAN; QUAKE-INTERFACE (hull_t, `w64_layers`/hull markers from model.h) | 0 / 0 / 0 / 0 | Importable after retargeting hull_t. | AuditWorld |
| source/w64_world.h | 287 | untracked | QI | CLEAN (struct/API declarations); QUAKE-INTERFACE (model_t, mleaf_t, edict_t, hull_t, trace_t, dmodel_t, byte) | 0 / 0 / 0 / 0 (comment at 15-18 documents that w64_world.c replicates Quake code) | Importable after retargeting types; fix the comment. | AuditWorld |
| source/world_atlas.h | 95 | 5c5eea5 2026-07-18 | CL | CLEAN | none | Own big-endian bake file layouts (STB2/AMB2/UI8/WATS/WCI4); no Quake/Xash/HLSDK text. No licence header. | AuditToolsStudio |
| source/world_tex_bake.h | 45 | 64e3131 2026-07-14 | CL | CLEAN | none | Own big-endian bake file layouts (STB2/AMB2/UI8/WATS/WCI4); no Quake/Xash/HLSDK text. No licence header. | AuditToolsStudio |

### source/g_* + game*.c (QuakeC translations)

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| source/g_ai.c | 700 | bfc2bb4 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_anim.h | 87 | 4615531 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_api.c | 504 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_api.h | 93 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_boss.c | 391 | 02727a7 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_buttons.c | 377 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_client.c | 633 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_combat.c | 568 | 9644f71 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_defs.h | 91 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_doors.c | 631 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_entvars.h | 163 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_fielddefs.c | 351 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_items.c | 737 | 68507e7 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_level.c | 302 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_level.h | 45 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_local.h | 321 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_misc.c | 314 | 02727a7 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_monsters.c | 1509 | bfc2bb4 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_plats.c | 296 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_progs.c | 89 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_subs.c | 157 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_triggers.c | 952 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_weapons.c | 1214 | 68507e7 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/g_world.c | 233 | 66bcd22 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/game.c | 850 | 1eb4851 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/game.h | 36 | 1eb4851 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |
| source/game_weapons.c | 416 | 290bbdf 2026-07-14 | FB | FORBIDDEN (QuakeC translation) | whole file | Listed as forbidden in PROVENANCE.md (QuakeC → C translations, Quake-derived). Some also hold our early HL drafts (`hlb_*` in g_buttons.c, Uplink weapon drafts in g_weapons.c, `hl_hurt` in g_triggers.c; AuditGameB); those are superseded by source/hl and are not importable either. | ProvAudit |

### source/hl

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| source/hl/hl_ai.c | 1736 | 8ca78f4 2026-07-18 | HL | HLSDK, CONTAINS-QCTRANS, QUAKE-INTERFACE (heavy) | 0 / 0 / ≈598 / ≈130 | game/ (HLSDK). Remove the walkmonster-pattern thinkers (752-917) and replace the `ai_run`/`walkmonster_start`/`FoundTarget` dependence; the hostile chase is currently Quake `ai_run`. | AuditGameA |
| source/hl/hl_ai.h | 196 | 8ca78f4 2026-07-18 | QI | CLEAN (+HLSDK constants), QUAKE-INTERFACE (edict_t) | 0 / 0 / 13 (constants) / 0 | game/. Retype edict_t/vec3_t/qboolean. | AuditGameA |
| source/hl/hl_ambient.c | 569 | untracked | HL | HLSDK, QUAKE-INTERFACE | 0 / 0 / ≈269 / 0 | game/ (HLSDK). The Quake static-sound shortcut (`g_ambientsound`, svc_spawnstaticsound) must be re-targeted. | AuditGameA |
| source/hl/hl_breakable.c | 632 | untracked | HL | HLSDK + QUAKE-INTERFACE | Q 0 / X 0 / H ~524 / QC 0 | → game/. | AuditGameB |
| source/hl/hl_entcook.h | 1635 | untracked | QI | CLEAN (own cooker; data tables) + QUAKE-INTERFACE (entvars field-name table) | 0/0/0/0 | Importable. Class list = Valve classnames (facts). Field list 449-494 must be re-targeted to the hl64 entvars. | AuditGameB |
| source/hl/hl_ents.c | 4349 | untracked | QI | CLEAN (own architecture) + QUAKE-INTERFACE; small HLSDK-structural region | Q 0 / X 0 / H 39 (2486-2524) / QC 0 | Engine-side; importable after re-targeting the bindings (progs fielddefs, edict_t, sv.*). Move hle_run_mm (ManagerThink) semantics to game/ or rewrite it from spec. | AuditGameB |
| source/hl/hl_ents.h | 250 | untracked | QI | CLEAN + QUAKE-INTERFACE (edict_t, vec3_t) | 0/0/0/0 | Importable as-is (API header). | AuditGameB |
| source/hl/hl_env.c | 1181 | untracked | HL | HLSDK + QUAKE-INTERFACE; own titles parser and render-tuple store | Q 0 / X 0 / H ~795 (372-1166) / QC 0 | → game/ (HLSDK licence). Own parts 43-370 could stay engine-side. | AuditGameB |
| source/hl/hl_env.h | 88 | untracked | QI | CLEAN + QUAKE-INTERFACE; 4 HLSDK constants | H 4 (52-55) | Header goes with hl_env.c. | AuditGameB |
| source/hl/hl_grunt.c | 1958 | untracked | HL | HLSDK, QUAKE-INTERFACE (heavy), CONTAINS-QCTRANS (trivial) | 0 / 0 / ≈1780 / 9 | game/ (HLSDK). Drop the non-SDL TE_LIGHTNING2 path (1002-1013); re-target `FoundTarget`, `g_movetogoal` and `monster_use`. | AuditGameA |
| source/hl/hl_hud.c | 897 | d2bc31a 2026-07-15 | HL | HLSDK (cl_dll HUD translation) + own wheel + QUAKE-INTERFACE | H ~720 / others 0 | → game/ (client) under HLSDK licence. Wheel 852-893 is ours. | AuditGameB |
| source/hl/hl_hud_rects.h | 128 | d2bc31a 2026-07-15 | CL | CLEAN (generated from Uplink sprites/hud.txt data) | 0/0/0/0 | Importable (generated data). Regenerate with tools/hlhud_bake. | AuditGameB |
| source/hl/hl_input.h | 30 | d2bc31a 2026-07-15 | CL | CLEAN | 0 | Importable. | AuditGameB |
| source/hl/hl_items.c | 1739 | untracked | HL | HLSDK + QUAKE-INTERFACE (incl. Quake TE_LIGHTNING2 protocol) | Q 0 (14 lines protocol interface 1549-1562; 12 lines Quake-heritage ClipVelocity also in HLSDK pm_shared) / X 0 / H ~1420 / QC 0 | → game/ under HLSDK licence. Replace the TE_LIGHTNING2 fallback. | AuditGameB |
| source/hl/hl_items.h | 97 | untracked | QI | CLEAN + QUAKE-INTERFACE | 0/0/0/0 | Goes with hl_items.c. | AuditGameB |
| source/hl/hl_levelstate.c | 2396 | untracked | QI | CLEAN (own dehydration/persistence design) + heavy QUAKE-INTERFACE | 0/0/0/0 | Importable once entvars/func_t/PR_* are re-targeted. Not derived from Xash sv_save.c. | AuditGameB |
| source/hl/hl_levelstate.h | 134 | untracked | CL | CLEAN | 0/0/0/0 | Importable. | AuditGameB |
| source/hl/hl_logic.c | 409 | untracked | HL | HLSDK (triggers.cpp/buttons.cpp/lights.cpp logic) + own record glue | H ~340 (14-357) / others 0 | → game/. | AuditGameB |
| source/hl/hl_material.c | 762 | untracked | HL | HLSDK + CONTAINS-QUAKE + own N64 path + QUAKE-INTERFACE | Q 70 (236-290, 554-568) / X 0 / H ~265 (47-165, 422-446, 576-696) / QC 0 | Split: HLSDK parts → game/; rewrite Mat_Walk and the rotate-into-model block clean; own cooked path is importable. | AuditGameB |
| source/hl/hl_material.h | 77 | untracked | HL | HLSDK constants (CHAR_TEX_*, CBTEXTURENAMEMAX, BULLET_* names) | H 27 (15-41) | → game/ or shared HLSDK header. | AuditGameB |
| source/hl/hl_monstermaker.c | 438 | untracked | HL | HLSDK, QUAKE-INTERFACE | 0 / 0 / ≈267 / 0 | game/ (HLSDK). The registry and persistence code (39-114, 398-431) is ours. | AuditGameA |
| source/hl/hl_monsters.c | 2062 | 8ca78f4 2026-07-18 | HL | HLSDK, CONTAINS-QUAKE (small), CONTAINS-QCTRANS (small), QUAKE-INTERFACE (heavy) | 15 / 0 / ≈1721 / 4 | game/ (HLSDK). Rewrite `HL_FreeEdicts` (255-269) against the new allocator. Replace the Quake gib stand-ins (33-49, 403-408) and the Mod_LeafPVS client-PVS code (595-674). | AuditGameA |
| source/hl/hl_monsters.h | 184 | untracked | QI | CLEAN (+HLSDK constants), QUAKE-INTERFACE (edict_t) | 0 / 0 / 23 (constants) / 0 | game/. | AuditGameA |
| source/hl/hl_movers.c | 1784 | untracked | HL | HLSDK + QUAKE-INTERFACE; de-minimis QCTRANS-identical text | Q 0 / X 0 / H ~1600 / QC 21 (49-58, 61-71) | → game/. Rewrite 49-71 from HLSDK LinearMoveDone/AngularMoveDone (trivial). | AuditGameB |
| source/hl/hl_movers.h | 62 | untracked | QI | CLEAN + QUAKE-INTERFACE | 0/0/0/0 | Goes with hl_movers.c. | AuditGameB |
| source/hl/hl_nodecook.h | 1314 | untracked | HL | CONTAINS-QUAKE, CONTAINS-XASH, HLSDK, CLEAN (format) | 283 / 11 (+86 shared with Quake) / ≈486 / 0 | Split it. The format part (1-196) and the lump/entity parsers are CLEAN. The hull trace and step code (280-578) must be rewritten clean-room or taken from the new engine's own tracer. The graph build (667-748, 869-1253) belongs in the HLSDK-licensed tool. | AuditGameA |
| source/hl/hl_nodes.c | 1244 | untracked | HL | HLSDK, CONTAINS-QUAKE+XASH (tiny), QUAKE-INTERFACE | 16 (also Xash, QCtrans) / 16 (same lines) / ≈437 / 16 (same lines) | game/ (HLSDK). Replace `drop_to_floor` (228-243) with an engine DROP_TO_FLOOR service. The job/budget architecture is ours. | AuditGameA |
| source/hl/hl_scripted.c | 1240 | untracked | HL | HLSDK, CONTAINS-XASH/QUAKE (tiny), QUAKE-INTERFACE | 18 / 18 (same lines) / ≈831 / 0 | game/ (HLSDK). Replace `drop_to_floor` (239-256). Twin handoff and dehydrate (434-489, 1059-1232) are ours. | AuditGameA |
| source/hl/hl_sentences.c | 831 | untracked | HL | CLEAN (parser), HLSDK (entities), QUAKE-INTERFACE | 0 / 0 / ≈315 / 0 | Importable. The parser and lookup (1-505) are independent of Xash s_vox.c and CLEAN. The scripted_sentence and speaker code (510-826) goes to game/. | AuditGameA |
| source/hl/hl_sentences.h | 38 | untracked | CL | CLEAN | 0 / 0 / 0 / 0 | Importable. Retype qboolean and MAX_QPATH. | AuditGameA |
| source/hl/hl_sounds.c | 498 | 8ca78f4 2026-07-18 | QI | CLEAN, QUAKE-INTERFACE | 0 / 0 / 0 / 0 | Importable after re-targeting the sound calls. The Xash hit is a false positive. | AuditGameA |
| source/hl/hl_sounds.h | 119 | 8ca78f4 2026-07-18 | CL | CLEAN (+HLSDK ATTN constants) | 0 / 0 / 6 (constants) / 0 | Importable. | AuditGameA |
| source/hl/hl_use.c | 524 | untracked | HL | HLSDK + CONTAINS-QUAKE (QuakeC SUB_UseTargets path) + de-minimis Xash-identical helper + QUAKE-INTERFACE | Q 45 (120-127, 140-176) / X 17 (370-386) / H ~330 / QC 0 | → game/. Delete the Quake-map branch (120-176). Replace in_sphere with hl_env.c HL_BoxDistSq. | AuditGameB |
| source/hl/hl_use.h | 105 | untracked | HL | HLSDK constants + own API | H 11 (16-19, 72-74, 82-84) | → game/. | AuditGameB |
| source/hl/hl_view.c | 235 | untracked | QI | Own client effects + QUAKE-INTERFACE; shake follows the Valve algorithm | Q 0 / X 0 (see note) / Valve(H-family) 32 (62-93) / QC 0 | Rewrite the shake (62-93) from a behavioural spec for the MIT engine, or put it under game/client. Fade, parsing and title code are ours. | AuditGameB |
| source/hl/hl_view.h | 20 | untracked | CL | CLEAN | 0 | Importable. | AuditGameB |
| source/hl/hl_weapons.c | 1879 | d2bc31a 2026-07-15 | HL | HLSDK + QUAKE-INTERFACE; 4-line Quake idiom | Q 4 (303-306 crandom) / X 0 / H ~1530 / QC 0 | → game/. Rewrite crandom/spread (303-306, 636-638) with HLSDK FireBulletsPlayer spread. | AuditGameB |
| source/hl/hl_weapons.h | 152 | d2bc31a 2026-07-15 | HL | HLSDK constants + own types | H 40 (19-58) | → game/. | AuditGameB |

### tools

| File | Lines | Git | Primary class | Verdict(s) | Contaminated lines | Import note | Review |
|---|---:|---|---|---|---|---|---|
| tools/.gitignore | 22 | 8942704 2026-07-26 | CL | CLEAN | none | Ignore list. | AuditToolsStudio |
| tools/Makefile | 74 | a19f97e 2026-07-12 | CL | CLEAN | none | Top-level tool build/targets. | AuditToolsStudio |
| tools/README.md | 106 | a19f97e 2026-07-12 | CL | CLEAN | none | Own doc. | AuditToolsStudio |
| tools/alias_bake/Makefile | 58 | be67cb5 2026-07-14 | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| tools/alias_bake/bake_alias.c | 636 | be67cb5 2026-07-14 | CQ | CONTAINS-QUAKE (format defs) | QUAKE 35 (19-53: IDPO constants + stvert_t/dtriangle_t/mdl_t, modelgen.h names) | Rest (CI4 bake, AMB2 writer) is ours. | AuditToolsStudio |
| tools/alias_bake/trim_alias_mdl.c | 295 | d7a244a 2026-07-14 | CQ | CONTAINS-QUAKE (format constants) | QUAKE 5 (27-31 IDPO constants) | Rest ours. | AuditToolsStudio |
| tools/analyze_hl_tex_atlases.py | 699 | 5c5eea5 2026-07-18 | CL | CLEAN | none | Own TMEM atlas analysis; vis RLE reader 140-155 is format code. | AuditToolsStudio |
| tools/bake_world_atlas.py | 616 | 5c5eea5 2026-07-18 | CL | CLEAN | none | Own skyline pack/WATS writer. | AuditToolsStudio |
| tools/convert_hl_sounds.sh | 157 | da63eca 2026-07-18 | CL | CLEAN | none | audioconv64 wrapper. | AuditToolsStudio |
| tools/deps/meshoptimizer/LICENSE.md | 21 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/README.md | 5 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/src/indexgenerator.cpp | 675 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/src/meshoptimizer.h | 1138 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/src/simplifier.cpp | 2332 | c69adf1 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/src/vcacheoptimizer.cpp | 467 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/deps/meshoptimizer/src/vfetchoptimizer.cpp | 74 | d184bf0 2026-07-23 | TP | THIRD-PARTY (meshoptimizer, MIT, Arseny Kapoulkine) | — | Vendored; not audited. Import from upstream with its LICENSE. | AuditToolsStudio |
| tools/extract_hl_sounds.sh | 215 | 8ca78f4 2026-07-18 | CL | CLEAN | none | pak extraction wrapper. | AuditToolsStudio |
| tools/extract_pak_sounds.py | 65 | 8942704 2026-07-26 | CL | CLEAN | none | PACK format reader. | AuditToolsStudio |
| tools/hl_campaign.py | 474 | untracked | CL | CLEAN | none | CWG cooker; changelevel/landmark math is documented behaviour. | AuditToolsStudio |
| tools/hl_campaign_sizing.py | 257 | untracked | CL | CLEAN | none | Size report. | AuditToolsStudio |
| tools/hl_classes.py | 299 | untracked | CL | CLEAN (HLSDK-informed data table) | none | Class->model name table (asset names), own comments. | AuditToolsStudio |
| tools/hl_gsfx.py | 307 | untracked | CL | CLEAN (HLSDK-informed data table) | none | Effect sprite/model name tables with own descriptions citing HLSDK file names. | AuditToolsStudio |
| tools/hl_headless.c | 599 | untracked | QI | QUAKE-INTERFACE | none (main() 481-545 follows sys_null.c main pattern: ~25 lines of boilerplate) | Host test runner on the Quake engine (edict_t/ent->v.*, sv./svs./cl./cls., SV_Move, SV_LinkEdict, SV_TestEntityPosition, SV_PointContents, SV_HullPointContents, Mod_PointInLeaf, Cbuf_AddText, Key_Event, Host_Init/Host_Frame, COM_InitArgv, Hunk_*); not importable. | AuditToolsStudio |
| tools/hl_image.py | 233 | untracked | CL | CLEAN | none | Content pruner. | AuditToolsStudio |
| tools/hl_image_keep.txt | 138 | untracked | CL | CLEAN | none | Own keep list. | AuditToolsStudio |
| tools/hl_sound_classes.py | 85 | untracked | CL | CLEAN | none | WAV chunk reader (RIFF cue chunk is the WAV spec). | AuditToolsStudio |
| tools/hl_sound_classes.txt | 32 | untracked | CL | CLEAN | none | Own rules. | AuditToolsStudio |
| tools/hl_sound_refs.py | 330 | untracked | CL | CLEAN | none | Reference scanner. | AuditToolsStudio |
| tools/hl_sprite_cook.py | 193 | untracked | CL | CLEAN | none | SPR v2 format cooker; mirrors r64_tex.c PickPal16 (follows that file verdict). | AuditToolsStudio |
| tools/hl_stage_content.py | 1037 | untracked | CL | CLEAN | none | Pipeline driver; DECAL_NAMES/effect names are asset-name facts. | AuditToolsStudio |
| tools/hl_world/README.md | 160 | 8c3bbf5 2026-07-19 | CL | CLEAN | none | Own doc. | AuditToolsStudio |
| tools/hl_world/examples/hldemo1.carve-compare.json | 198 | 4630333 2026-07-19 | DA | DATA (generated) | n/a | Generated zone/stream plans from Valve HL maps: numbers/classnames/bounds; not code. | AuditToolsStudio |
| tools/hl_world/examples/hldemo1.stream.json | 582 | ffc24eb 2026-07-19 | DA | DATA (generated) | n/a | Generated zone/stream plans from Valve HL maps: numbers/classnames/bounds; not code. | AuditToolsStudio |
| tools/hl_world/examples/uplink-stitched-carve.json | 1489 | a0e0c2e 2026-07-19 | DA | DATA (generated) | n/a | Generated zone/stream plans from Valve HL maps: numbers/classnames/bounds; not code. | AuditToolsStudio |
| tools/hl_world/examples/uplink.world.json | 2602 | 8c3bbf5 2026-07-19 | DA | DATA (generated) | n/a | Generated zone/stream plans from Valve HL maps: numbers/classnames/bounds; not code. | AuditToolsStudio |
| tools/hl_world/examples/uplink.zones.md | 100 | 8c3bbf5 2026-07-19 | DA | DATA (generated) | n/a | Generated report. | AuditToolsStudio |
| tools/hl_world/stitch_and_carve.py | 1229 | 8c3bbf5 2026-07-19 | CL | CLEAN | none | Own cluster/zone merge (union-find, AABB). | AuditToolsStudio |
| tools/hl_world/stream_plan.py | 443 | ffc24eb 2026-07-19 | CL | CLEAN | none | Own plan cooker. | AuditToolsStudio |
| tools/hl_world/trial_stitched_carve.py | 489 | a0e0c2e 2026-07-19 | CL | CLEAN | none | Own trial script. | AuditToolsStudio |
| tools/hl_world/zone_pack.py | 319 | 2528d90 2026-07-19 | CL | CLEAN | none | Own LZ4 pack cooker. | AuditToolsStudio |
| tools/hlhud_bake/bake_hlhud.py | 283 | d2bc31a 2026-07-15 | CL | CLEAN | none | SPR/hud.txt cooker. | AuditToolsStudio |
| tools/mdl_size_report.py | 98 | 8942704 2026-07-26 | CL | CLEAN | none | Size report. | AuditToolsStudio |
| tools/mk_white_room.py | 409 | b6ea17c 2026-07-14 | CL | CLEAN | none | Hand-built BSP29 writer (format). | AuditToolsStudio |
| tools/n64_content_pipeline.py | 410 | 66bcd22 2026-07-14 | CL | CLEAN | none | Pipeline driver. | AuditToolsStudio |
| tools/paktool.py | 193 | a19f97e 2026-07-12 | CL | CLEAN | none | PACK format tool. | AuditToolsStudio |
| tools/prof_compare.py | 125 | 5c5eea5 2026-07-18 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/profdb.py | 1293 | 5c5eea5 2026-07-18 | CL | CLEAN | none | Own SQLite profile DB. | AuditToolsStudio |
| tools/progs_string_audit.py | 381 | 8942704 2026-07-26 | CL | CLEAN | none | progs.dat format reader. | AuditToolsStudio |
| tools/progs_string_liveness.py | 275 | 8942704 2026-07-26 | CL | CLEAN | none | progs.dat format reader. | AuditToolsStudio |
| tools/q64mapc/.gitignore | 3 | a19f97e 2026-07-12 | CL | CLEAN | none | Ignore list. | AuditToolsStudio |
| tools/q64mapc/Makefile | 12 | a19f97e 2026-07-12 | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| tools/q64mapc/est_ent_pack.py | 1080 | 8942704 2026-07-26 | CL | CLEAN | none | Estimator, own. | AuditToolsStudio |
| tools/q64mapc/q64mapc.c | 1416 | a19f97e 2026-07-12 | CQ | CONTAINS-QUAKE; QUAKE-INTERFACE (bspfile.h) | QUAKE 115 (172-251, 1024-1053, 1065-1069); 172-251 also matches HLSDK utils/qrad/lightmap.c (Quake heritage) | Includes source/bspfile.h (id Quake) + q64_map.h. BSP->.q64 cook (clusters, LE helpers, string table, texture conversion, file assembly) is ours. | AuditToolsStudio |
| tools/q64zone/Makefile | 11 | 8c3bbf5 2026-07-19 | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| tools/q64zone/bake_dry.py | 143 | 64e3131 2026-07-14 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/q64zone/q64zone.c | 422 | 8c3bbf5 2026-07-19 | CL | CLEAN | none | Own zone merger; only needs q64_map.h structs. | AuditToolsStudio |
| tools/rebuild_n64_paks.py | 443 | a19f97e 2026-07-12 | CL | CLEAN | none | Pipeline driver. | AuditToolsStudio |
| tools/studio_bake/.gitignore | 2 | d184bf0 2026-07-23 | CL | CLEAN | none | Ignore list. | AuditToolsStudio |
| tools/studio_bake/Makefile | 90 | bcc05dc 2026-07-14 | CL | CLEAN | none | Links source/studio_geom.c (contaminated) and vendored meshoptimizer. | AuditToolsStudio |
| tools/studio_bake/bake_meshopt.cpp | 522 | d184bf0 2026-07-23 | CL | CLEAN | none | Uses meshoptimizer API (MIT); evidence hits false positive. | AuditToolsStudio |
| tools/studio_bake/bake_studio.c | 1247 | bcc05dc 2026-07-14 | CL | CLEAN (links CONTAINS-XASH studio_geom.c) | none; 331-474 tricmd walk is format-driven (same expression family as SG_EmitSubmodel, not matched to Xash/HLSDK text) | Bake tool: STB2 writer, CI4 build, meshlet layout, class budget are ours. Build dependency on studio_geom.c: re-link to the rewritten geometry. | AuditToolsStudio |
| tools/studio_compare/Makefile | 94 | b6ea17c 2026-07-14 | CL | CLEAN | none | Build glue; names the Xash ref harness. | AuditToolsStudio |
| tools/studio_compare/README.md | 134 | b6ea17c 2026-07-14 | CL | CLEAN | none | Own doc. | AuditToolsStudio |
| tools/studio_compare/dump_bounds.c | 62 | bcc05dc 2026-07-14 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/studio_compare/dump_seqs.c | 47 | 8ca78f4 2026-07-18 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/studio_compare/mdl_load_host.c | 364 | b6ea17c 2026-07-14 | CL | CLEAN | none | Host IDST loader; swap tables mirror source/mod_studio.c (format code). | AuditToolsStudio |
| tools/studio_compare/soft_fb.c | 248 | bcc05dc 2026-07-14 | CL | CLEAN | none | Own framebuffer/diff writer. | AuditToolsStudio |
| tools/studio_compare/soft_fb.h | 41 | bcc05dc 2026-07-14 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/studio_compare/soft_studio.c | 335 | bcc05dc 2026-07-14 | CQ | CONTAINS-QUAKE | QUAKE 17 (40-56 angle_vectors, labelled "Quake AngleVectors"); comment 221 cites Xash/Quake pitch flip | Rest ours (duplicate of studio_soft.c logic). | AuditToolsStudio |
| tools/studio_compare/soft_studio.h | 6 | b6ea17c 2026-07-14 | CL | CLEAN | none | Own. | AuditToolsStudio |
| tools/studio_compare/studio_compare.h | 27 | b6ea17c 2026-07-14 | CL | CLEAN | none | Own; includes studio.h/studio_geom.h. | AuditToolsStudio |
| tools/studio_compare/test_studio_anim.c | 520 | untracked | CL | CLEAN | none | Own test; links studio_geom.c. | AuditToolsStudio |
| tools/studio_compare/test_studio_bake_policy.c | 224 | 8ca78f4 2026-07-18 | CL | CLEAN | none | Own test. | AuditToolsStudio |
| tools/studio_compare/test_studio_modelspace.c | 170 | 8ca78f4 2026-07-18 | CL | CLEAN | none | Own test. | AuditToolsStudio |
| tools/studio_compare/test_studio_pixels.c | 327 | b6ea17c 2026-07-14 | CL | CLEAN | none | Own test comparing Xash-ref vs production. | AuditToolsStudio |
| tools/studio_compare/test_studio_stream.c | 208 | b6ea17c 2026-07-14 | CL | CLEAN | none | Own test. | AuditToolsStudio |
| tools/studio_compare/test_studio_trim.c | 134 | 32589db 2026-07-14 | CL | CLEAN | none | Own test. | AuditToolsStudio |
| tools/studio_compare/xash_ref_stream.c | 504 | b6ea17c 2026-07-14 | CX | CONTAINS-XASH; CONTAINS-HLSDK | XASH 322 / HLSDK 322 (24-294, 321-371) | Explicit Xash3D reference re-implementation (header: formulas match xash mathlib/matrixlib/gl_studio). Keep out of hl64 repo; at most a GPL-side oracle. | AuditToolsStudio |
| tools/test_bsp_load.c | 97 | 5cfba32 2026-07-14 | QI | QUAKE-INTERFACE | none | Smoke test on Quake engine (Host_Init, Mod_ForName, model_t); not importable. | AuditToolsStudio |
| tools/ui_bake/bake_ui.py | 56 | 64e3131 2026-07-14 | CL | CLEAN | none | UI8 cooker. | AuditToolsStudio |
| tools/ui_bake/extract_conchars.py | 24 | 64e3131 2026-07-14 | CL | CLEAN | none | WAD2 reader. | AuditToolsStudio |
| tools/w64c/Makefile | 12 | untracked | CL | CLEAN | none | Build glue. | AuditToolsStudio |
| tools/w64c/hlecook | 66 | untracked | DA | BUILD ARTIFACT | n/a | Untracked x86-64 ELF of hlecook.c; not source; do not import. | AuditToolsStudio |
| tools/w64c/hlecook.c | 86 | untracked | CL | CLEAN | none | Thin driver over hl_entcook.h/hl_nodecook.h (those files are in other slices). | AuditToolsStudio |
| tools/w64c/test_w64seam.py | 214 | untracked | CL | CLEAN | none | Own unit test. | AuditToolsStudio |
| tools/w64c/w64c.py | 2015 | untracked | CQ | CONTAINS-QUAKE; CONTAINS-QCTRANS (data); CONTAINS-HLSDK | QUAKE 152 (344-373, 424-431, 838-850, 861-944, 1524-1534) + QUAKE/QCTRANS data 121-128 (8, counted in 152); HLSDK 98 (1349-1446) | Rest (w64 format, meshlets, sectors, collision COLL/grid, encoders) ours; cook_face 405-519 replicates r64_world.c (verdict follows that file). | AuditToolsStudio |
| tools/w64c/w64check.py | 722 | untracked | CQ | CONTAINS-QUAKE | QUAKE 73 (296-368 Tracer = SV_RecursiveHullCheck); 109-119 vis RLE (format, minor) | Verifier: Python port of SV_RecursiveHullCheck; remainder ours. | AuditToolsStudio |
| tools/w64c/w64seam.py | 849 | untracked | CQ | CONTAINS-QUAKE (small) | QUAKE 7 (77-83 AngleVectors basis) | Remainder (ray proofs, budgets, CWG3 seams) ours. | AuditToolsStudio |
| tools/wav64_rate_report.py | 54 | 8942704 2026-07-26 | CL | CLEAN | none | wav64 header reader. | AuditToolsStudio |
| tools/wav_probe.py | 73 | 8942704 2026-07-26 | CL | CLEAN | none | WAV reader. | AuditToolsStudio |
| tools/world_tex_bake/bake_world_tex.py | 256 | 64e3131 2026-07-14 | CL | CLEAN | none | WCI4 cooker. | AuditToolsStudio |

Primary-class codes: CL = CLEAN, QI = QUAKE-INTERFACE, CQ = CONTAINS-QUAKE, CX = CONTAINS-XASH, HL = HLSDK, FB = forbidden, DOC = documentation, TP = third-party, DA = data/log/artefact.

## 3. Format and design documents (n64/docs and others)

Checks applied to every document:
- word 8-gram overlap against all Quake, Xash and HLSDK comments: 0 hits
  in any document;
- token scan of embedded code blocks (16/30-token grams): one real hit,
  q64-map-streaming.md 77-81 ← Quake model.c 511-512;
- a grep for old-engine identifiers that would have to be restated before
  the text can serve as a clean-room spec.

| Document | Lines | Copied text | Old-engine references | Import to hl64 |
|---|---:|---|---|---|
| n64/docs/w64-format.md | 432 | none | 7 lines: `Mod_LoadTextures` (anim sequencing, 73), Quake PVS RLE (318), `SV_RecursiveHullCheck` epsilon (254, 427), `SV_TouchLinks` (361), `Mod_PointInLeaf`/`Mod_DecompressVis`, `COM_FOpenFile`, `g_world.c` lightstyle values (142); HLSDK `PM_InitTextureTypes`/`PM_FindTextureType` (264-270, a behaviour citation; keep) | **Yes → docs/formats/w64.md** after restating the Quake-function references as prose definitions (texture animation order, RLE vis row, trace epsilon, touch-link bbox rule, style-0 "m" = 264). |
| n64/docs/cwg-format.md | 197 | none | 1 line (124): runtime fallback described via `q64_world.c` | **Yes → docs/formats/cwg3.md**; restate line 124 as a requirement. |
| n64/docs/hle-format-v1.md | 135 | none | 4 lines: `source/model.c`/`Mod_LoadEntities` (109), `edict_t.hl_lump` (91), progs; HLSDK `CTestHull::BuildNodeGraph`, `CBaseDelay` (behaviour citations) | **Yes → docs/formats/hle1.md** after the scrub. |
| n64/docs/stream-arch-v2.md | 542 | none | 19 lines name old-tree functions (`SV_SpawnServer` ×6, `world.c` ×3, `SV_Move`, `SV_LayerWorld`, `ED_RoomFor`, `Mod_Studio*`, `SV_ModelIndex`) and old-tree file paths | **Yes, as design/history (docs/design/)**, not under formats/. Measurements and budgets are ours. Mark function names as old-tree references. The sector/meshlet/COLL/leaf-grid layouts in §3 duplicate w64-format.md; w64-format.md is the normative one. |
| n64/docs/q64-format-v1.md | 504 | none (layouts "match" `dplane_t`/`dmodel_t` from Quake bspfile.h: layout facts) | 10 lines cite bspfile.h/model.h/`SV_HullPointContents` | **No.** Obsolete v1 format, deleted for HL builds. |
| n64/docs/q64-map-streaming.md | 551 | **Yes:** 77-81 quote Quake model.c `Mod_LoadLighting`; 89-103 list Quake model.h `msurface_t`/`mnode_t` members (N64_PORT edits of a Quake file) | many | **No.** Obsolete v1 design. |
| n64/docs/seamless-uplink.md | 159 | none | some | Optional, as history. |
| n64/docs/campaign-sizing.md | 142 | none | — | Optional (measurements). |
| n64/docs/profiling.md | 207 | none | old PROFILE build | No. |
| n64/docs/e1m3-zone-prototype.md | 137 | none | Quake e1m3 | No. |
| n64/docs/future-qc-native.md | 109 | none | QuakeC | No. |
| n64/docs/bug-audioconv64-vadpcm-huffman-silence.md | 171 | none | — | Optional (libdragon bug note). |
| n64/port/HL_UPLINK.md | 1207 | none | describes Quake-engine internals throughout | No (status log). Read it for behaviour only, through the spec process. |
| tools/README.md, tools/hl_world/README.md, tools/studio_compare/README.md, n64/port/test/README.md, n64/port/modelviewer/README.md | — | none | tool usage | With their tools. |

## 4. Our edits inside Quake files (not importable)

Our commits add 6,575 lines inside first-commit Quake files
(`git diff --numstat 1305210 -- <first-commit source files>`): model.c +1163, sv_main.c +897, world.c +656, sv_phys.c +513, cl_parse.c +386, menu.c +373, pr_edict.c +322, sv_user.c +282, common.c +277, model.h +236, cl_main.c +174, host.c +160, host_cmd.c +137, sv_move.c +104, progs.h +96, view.c +79, server.h +71, zone.c +62, r_part.c +56, screen.c +52, quakegeneric.c +51, protocol.h +49, quakedef.h +44, snd_null.c +37, cl_input.c +33, pr_cmds.c +31, pr_exec.c +28, render.h +27, world.h +22, sbar.c +20, client.h +17, sound.h +16, cl_tent.c +15, in_null.c +12, menu.h +10, draw.h +10, common.h +10, progdefs.h +9, d_edge.c +8, r_main.c +6, net.h +5, wad.c +5, r_surf.c +3, mathlib.c +3, d_iface.h +3, bspfile.h +2, chase.c +1, view.h +1, sys.h +1.

They implement the HL integration hooks: BSP30 loading, `.w64` worlds,
HLE entities, studio models, seams and layers, edict streaming, protocol
extensions, menus. They live inside GPL Quake files and cannot be copied.
Where hl64 needs one of these behaviours, the spec agents describe it in
prose from black-box observation or from these diffs, and implementers
work from that spec only.

## Appendix A. Region-level evidence per review slice

These are the slice reports, minus their summary tables, which are merged into §2. They give each file's region table (lines, function, verdict, counterpart file:lines, basis) and the disposition of every automated hit. Reference paths are relative to the dirty-side evidence root `/scratch/quakegeneric/tmp/ProvAudit/` (`first/source` = Quake first commit, `quake/`, `quake2/`, `xash/`, `hlsdk/`, `hlsdk-portable/`, `qctrans/`).

### A.AuditPlatform — Platform, memory, I/O and host glue



##### Evidence notes

- **quakegeneric_n64.c**: created e599929 from a Quake-style file header (Id Software copyright + GPL). git `-C` copy detection finds no source file; the body is libdragon glue written for the port. The `QG_*` functions implement the upstream quakegeneric platform interface (`quakegeneric.h`: QG_Init/QG_Quit/QG_SetPalette/QG_DrawFrame/QG_GetKey/QG_GetMouseMove/QG_GetJoyAxes, QG_Create/QG_Tick). The key queue is a different design from upstream `quakegeneric_sdl2.c` KeyPush/KeyPop. The only k16 hit (184-190, `QG_GetMouseMove` zeroing outputs) is trivial. Bindings: QG_* API, `Cbuf_AddText` with console scripts, Quake `K_*` key codes, HL weapon-wheel bridge (`hl/hl_input.h`).
- **sys_n64.c**: git copy detection `C068 source/sys_null.c → n64/port/sys_n64.c` (e599929). Exact hits: 26-93 ← sys_null.c 22-142 (188 tok; findhandle/filelength/Sys_FileOpenRead/OpenWrite/Close/Seek/Tell), 127-171 ← sys_null.c 114-149, 283-305 ← sys_null.c 187-195. Ours: Sys_Error's libdragon on-screen painter (169-203), `rsp_crash_stdout`/`__wrap_console_init`/`__wrap_console_render` (205-250), `Sys_WalkHb` (251-262), `timer_ticks` clock.
- **sys_sdl.c** (untracked): exact hits 19-28 ← sys_win.c 100-104 (findhandle), 43-56/58-79/79-84 ← sys_null.c 74-114, 166-178 ← sys_null.c 197-200.
- **q64_host_render.c** (untracked): VID_Init field assignments 57-67 follow vid_null.c VID_Init 53-60 line for line (fullbright from colormap); `R_Init` notexture offsets 76-81 ← r_misc.c R_InitTextures (exact 77 tok); `Draw_PicFromWad` 461-462 ← draw.c 54-56. The rest (studio draw mirror, parity hunk accounting) is ours but implements Quake's `R_*` seam: 36 Quake function names defined here.
- **sdl_input.c** (untracked): exact hit 133-137 ← in_win.c 711-713. The SDL→`K_*` table (830-851) maps SDL keys onto Quake key-code constants; upstream quakegeneric_sdl2.c has a different table. Normalised hit vs net_bw.c is a false positive.
- **q64_heap_host.c**: header states it reproduces newlib 4.4 mallocr.c (dlmalloc 2.6.4). dlmalloc is public domain; newlib's mallocr.c carries Doug Lea's public-domain notice. No Quake/Xash similarity.
- **r64_malloc.c** hit 54-68 (normalised, vs snd_gus.c) is a run of static counters, false positive. **r64_prof.c** hit 284-293 is a string array, false positive. **fpu_shim.c** LIBDRAGON hit (opcode macros) is a generic MIPS decode macro, false positive.
- **GPL headers**: fpu_shim.c, frame_stats.c, r64_hunk.c, r64_malloc.c, r64_mem.c, r64_prof.c, q64_selftest.c, test_harness.h carry "Copyright (C) 2026 quakegeneric contributors" GPL-2+ headers. They are our own code, so the copyright holder can relicense it, but each import has to replace the header with the MIT header.


### A.AuditWorld — World, collision, PVS, streaming, route


Tree audited: ~/src/ports/quakegeneric working tree as of this review (read-only). Reference trees: Quake first-commit (`first/source`), id Quake (`quake/WinQuake`, `quake/QW`), Xash (`xash/`), HLSDK (`hlsdk/`), QC translations (`qctrans/`).
No prose or tables below quote code; only function names, line ranges, ref file:line and short descriptions.

Lineage note used throughout: Xash `common/pm_trace.c`, `common/mod_bmodel.c`, `server/sv_world.c` are themselves Quake descendants. Wherever one of our regions matches both Xash and Quake, I checked that the same code exists in `first/source` / `quake/WinQuake` (it does in every case below) and that our region follows the Quake form rather than Xash's modified form. Attribution is therefore **Quake**; there is no Xash-specific or HLSDK-specific expression in this slice (HLSDK appears only as data constants, see w64_format.h).

Licence-header note: q64_brush.c, q64_map.c, q64_map.h, q64_route.c carry a GPLv2+ "quakegeneric contributors" header; the other files carry no licence header (comment banner only). Headers need relicensing on import whatever the verdict.

#### 1. Summary table


Totals of flagged code: Quake ≈ 517 lines across 4 files (q64_brush.c 163, w64_world.c 286, q64_map.c 69; the other 9 files 0). Xash 0, HLSDK expression 0, QCtrans 0.

---

#### 2. Per-file sections

##### n64/port/q64_brush.c (820 lines; git: a19f97e 2026-07-12, modified in worktree; history: 22b293a, d6ad366, 5cfba32 (adds anim sequencing, BSP30 hull sizes), 8491b51, 6c08c21, 0e5a7cf, 0c8c05c, cf5c3a9)
Verdicts: **CONTAINS-QUAKE**, **QUAKE-INTERFACE**. Single function Q64_LoadBrushModel (72-820) plus 2 small statics. It is a reader of the `.q64` skeleton that fills model_t the way model.c Mod_LoadBrushModel fills it, and several blocks were lifted from model.c. Anim-sequence block was added in commit 5cfba32 ("BSP30/HL Uplink map load"); hull and submodel blocks date from the first commit a19f97e. The only comment that cites Quake is line 268 ("same as Mod_LoadTextures").

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 38-57 | Q64_BuildPath, Q64_LittleFloat | CLEAN | — | ours (path rewrite to .q64; float byte-swap helper). |
| 72-145 | header read/validate, prologue | CLEAN | — | .q64 format specific. |
| 153-224 | miptex dir/stream bind | CLEAN | — | ours (Q64_TexBind streams); 217-218 dataofs swap is trivial. |
| 226-265 (counted 240-248, 253-255, 263-264) | per-texture header load | CONTAINS-QUAKE (partial, structural translation) | first/source/model.c:Mod_LoadTextures 376-403 (dataofs==-1 skip, width/height/offsets byte-swap, 16-alignment check with same message text "is not 16 aligned", name copy, sky check → R_InitSky) | Same sequence and identical Sys_Error wording; pixels copy removed (headers-only). Weak individually; combined with the next block it is a derivative of Mod_LoadTextures. |
| 268-351 (269-351 counted) | animation sequencing | CONTAINS-QUAKE (renamed/near-exact copy) | first/source/model.c:Mod_LoadTextures 405-496 (also quake/WinQuake/model.c); same in Xash common/mod_bmodel.c:3024-3100 (Quake heritage) | Same variables (anims/altanims arrays, max/altmax/num), same name-parsing ('+', 'A'-'J', '0'-'9'), same loop order, same error strings ("Bad animating texture", "Missing frame"), same ANIM_CYCLE constant and link fields. Only differences: tx2 vs tx in one message, brace layout. Automated hit: 530 tokens exact+normalised. REAL. |
| 358-366 | fallback to Mod_LoadTextures | QUAKE-INTERFACE | — | calls model.c loader. |
| 368-385, 442-503 | planes / faces / texinfo / marksurfaces stream binds | CLEAN | — | ours (fault-in caches). |
| 387-409 | node read (field copy) | CLEAN (format-forced) | model.c Mod_LoadNodes 844-870 | field-by-field copy from a different record layout; no shared structure beyond field names. Not flagged. |
| 411-440 | leaf stream bind | CLEAN | — | ours. |
| 505-528 (counted 511-527) | submodels read | CONTAINS-QUAKE (renamed copy) | first/source/model.c:Mod_LoadSubmodels 598-612 | Same loop: 3-axis spread of mins/maxs by one unit, origin, MAX_MAP_HULLS headnode copy, visleafs/firstface/numfaces; only change is the byte-swap helper name. The ±1 spread idiom is also in Xash mod_bmodel.c:2271-2276 (Quake heritage). REAL. |
| 530-583 (counted 557-579) | clip hull setup | CONTAINS-QUAKE (renamed copy of constants+structure) | first/source/model.c:Mod_LoadClipnodes 962-990 | Same two hull blocks (hull 1 and hull 2: firstclipnode 0, lastclipnode count-1, planes pointer, clip_mins/maxs with Quake's -16/-16/-24 / 16/16/32 and -32/-32/-24 / 32/32/64). clipnodes pointer set to NULL (stream) differs. REAL (hull_t fill order/values identical). 562-579 values are Quake data; 581-582 ours. |
| 585-660 | entity chunk read, GoldSrc hull override (629-647) | CLEAN | — | ours (HLE chunk magic, hull 3, pad logic). Numeric hull extents are GoldSrc facts: hull1 z -36..36, hull2 z -32..32, hull3 ±16/±18 (cf. hlsdk/dlls/util.h:414 VEC_HULL_MIN etc.); data, not expression. |
| 664-706 | vis stream bind, pad logic | CLEAN | — | ours. |
| 708-757 | geom/light bind | CLEAN | — | ours. |
| 759-761 | Mod_SetParentIdx + Mod_MakeHull0 call | QUAKE-INTERFACE | model.c Mod_MakeHull0 | calls only. |
| 763-801 (counted 765-779 and 790-801, ≈26 lines) | submodel loop of Mod_LoadBrushModel | CONTAINS-QUAKE (renamed copy) | first/source/model.c:Mod_LoadBrushModel 1185-1217 (also Xash mod_bmodel.c:2153-2177, Quake heritage) | Same loop order: hulls[0].firstclipnode, per-hull firstclipnode/lastclipnode, firstmodelsurface/nummodelsurfaces, VectorCopy maxs/mins, radius via RadiusFromBounds, `numleafs = visleafs`, then clone to "*%i" model via Mod_FindName + struct copy + strcpy name (792-801). Our additions: 780-789 comment and visleafs sanity error. Original comments dropped, structure and name format retained. REAL. |
| 803-820 | log lines, restore loadmodel | CLEAN | — | ours. |

Automated hits:
- QUAKE n 269-351 (530 tok), e 271-299, e 301-324, e 324-351 vs model.c 408-492: REAL (anim sequencing).
- XASH e 275-277 (mod_bmodel.c:3024-3027), n 332-336 / 344-348 (mod_bmodel.c:3098-3100): same Quake-heritage anim code present in `first/source/model.c`; attribute to Quake.
- QUAKE n/e 560-582 vs model.c 962-979: REAL (hull setup).
- QUAKE e 520-523 vs model.c 604-606: REAL (submodel origin copy, part of the Mod_LoadSubmodels block).
- QUAKE/XASH e,n 765-779, 789-801 vs model.c 1191-1215 / mod_bmodel.c 2153-2158: REAL (Quake origin).
- QUAKE e 161-164 (decl of an int `di`) : false positive.

Quake bindings (retarget list): model_t fields (type, numframes, flags, textures, numtextures, planes/numplanes, nodes/numnodes, leafs/numleafs, vertexes/edges/surfedges, texinfo, surfaces/numsurfaces, marksurfaces, clipnodes/numclipnodes, hulls[], submodels, visdata, lightdata, entities, hle/hlelen, firstmodelsurface/nummodelsurfaces, mins/maxs/radius, name); mnode_t/mleaf_t/msurface_t member names; hull_t; dclipnode_t; dmodel_t; dheader_t, lump_t, LUMP_TEXTURES/VERTEXES/EDGES/SURFEDGES (BSP29 bspfile); texture_t, miptex_t, MIPLEVELS; loadmodel/loadname/mod_base globals; Mod_LoadTextures, Mod_MakeHull0, Mod_SetParentIdx, Mod_FindName, Mod_LeafSurface, RadiusFromBounds, CalcSurfaceExtents (declared, unused, line 34); R_InitSky; Sys_Error (52 sites), Con_Printf, Hunk_AllocName, COM_FOpenFile, LittleLong/LittleShort, Q_strncmp, ED_LoadFromFile (comment).

---

##### n64/port/q64_map.c (3500 lines; git: a19f97e 2026-07-12, modified; later: 0e5a7cf, 0c8c05c, cf5c3a9)
Verdicts: **CLEAN** (bulk), **CONTAINS-QUAKE** (4 blocks), **QUAKE-INTERFACE**. The file is a set of fault-in caches (light pages, geom pages, texture mips, face/mark/texinfo/leaf/plane/clip streams, vis stream) over a cart file. The cache machinery (slot arrays, LRU clocks, hash for faces, evict functions, bounce buffers, page prefetch, cluster graph) is original. The 4 flagged blocks are record decoders that replicate model.c loader bodies.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 1-215 | page cache open/close, LoadClusterMeta | CLEAN | — | ours. |
| 219-720 | cluster meta, page slots, Q64_TouchLight, prefetch, Q64_OnViewLeaf | CLEAN | — | ours; uses Mod_LeafNum/Mod_LeafSurface calls only. |
| 722-1042 | geom stream bind, GEOM_GLOBAL probe | CLEAN | — | ours. |
| 1044-1273 | geom pages, vertex/edge/surfedge readers | CLEAN | — | ours. |
| 1275-1356 | Q64_GatherFacePositions | CLEAN (format idiom) | model.c CalcSurfaceExtents 721-730 (first/source) | uses the BSP surfedge sign convention to pick vertex 0/1 of an edge: file-format semantics shared by every BSP reader; control flow and bounds checks differ. Low risk, not counted. |
| 1366-1583 | texture mip stream (Q64_TexBind, Q64_TexRegister, Q64_TexReadMip) | CLEAN | — | ours. |
| 1600-1615 | malloc'd entities bookkeeping | CLEAN | — | ours. |
| 1617-1910 | face stream: hash + cache + bind | CLEAN | — | ours. |
| 1912-1934 | FaceDecode | CLEAN (format-forced field copy) | model.c Mod_LoadFaces 753-800 | field copy from our record layout; no shared structure. |
| 1939-1968 (counted 1960-1967) | FaceApplyTexFlags | CONTAINS-QUAKE (structural translation) | first/source/model.c:Mod_LoadFaces 806-826 | Same two cases: texture name starting "sky" → SURF_DRAWSKY\|SURF_DRAWTILED; name starting '*' → SURF_DRAWTURB\|SURF_DRAWTILED with extents 16384 and texturemins -8192. Lookup glue is ours. Small but identical constants/flag pair. REAL. |
| 1970-2098 | face evict/load/touch/prefetch | CLEAN | — | ours. |
| 2100-2258 | marksurface stream | CLEAN | — | ours. |
| 2259-2382 | texinfo stream bind | CLEAN | — | ours. |
| 2383-2465 (counted 2435-2447, 2453-2455) | Q64_TexinfoTouch | CONTAINS-QUAKE (renamed copy) | first/source/model.c:Mod_LoadTexinfo 650-697 | Byte-swap of the 8 vector floats, Length() of both vec rows, mean, mipadjust thresholds 0.32/0.49/0.99 → 4/3/2/1 (identical constants and order); notexture fallback sets r_notexture_mip and flags 0 (2453-2455). Cache/stream glue (2383-2434, 2459-2464) ours. Automated hit: 34-token exact at 2436-2439. REAL. |
| 2467-2735 | leaf stream: Q64_LeafTouch, SetContents/SetParent/Pin | CLEAN (format-forced field copy) | model.c Mod_LoadLeafs 886-935 | field-by-field copy into mleaf_t; no structure shared. The leaf record layout in q64_map.h mirrors dleaf_t; not expression. |
| 2737-2985 (counted 2968-2977) | plane stream, Q64_PlaneTouch | CONTAINS-QUAKE (renamed copy) | first/source/model.c:Mod_LoadPlanes 1089-1112 | Per-plane: 3 normal floats swapped, signbit accumulation (bit j set when component < 0), dist swap, type, signbits. Automated hit 30-token exact at 2971-2975. It is the standard plane decode and tiny, but identical in structure/ordering. REAL (minor). |
| 2986-3201 | clipnode stream (ClipBind, ClipLoadSlot, Q64_ClipTouch) | CLEAN | — | ours. |
| 3203-3380 (counted 3217, 3298, 3347-3379) | vis stream; Q64_VisLeafPVS | CONTAINS-QUAKE (structural translation) | first/source/model.c:Mod_DecompressVis 115-146 (+ Mod_LeafPVS 148-155) | Comment at 3347 says "Same RLE as Mod_DecompressVis". Loop: non-zero byte copied; zero byte followed by run count of zero bytes; do-while until row filled; row = (numleafs+7)>>3; static decompress buffer sized MAX_MAP_LEAFS/8; all-0xff for missing vis. Our additions: input-bounds checks, bounce buffer, per-leaf cache. The RLE is the documented vis format but the code follows the Quake function line-for-line modulo checks. REAL. |
| 3382-3500 | page cache release, preallocation | CLEAN | — | ours. |

Automated hits: QUAKE e 2436-2439 vs model.c:666-668 REAL; QUAKE e 2971-2975 vs model.c:1109-1110 REAL; QUAKE n 2439-2447 (k16) vs model.c:669-672 REAL (mipadjust); QUAKE e 2707-2709 vs model.c:932-934 (compressed_vis cleared to NULL on leaf fault-in): trivial, false positive.

Quake bindings: model_t, mleaf_t (visframe, key, parent, firstmarksurface, compressed_vis, ambient_sound_level), mplane_t, msurface_t (plane/texinfo indices, flags SURF_DRAWSKY/DRAWTURB/DRAWTILED, extents, texturemins, styles, lightofs), mtexinfo_t, texture_t, miptex_t, dclipnode_t; r_notexture_mip; Mod_LeafNum, Mod_LeafSurface; Length, VectorCopy; LittleLong/Short/Float; Hunk_AllocName, Host_ClearMemory, Sys_GetEngineMemsize, Sys_Error, Con_Printf/DPrintf, COM_FOpenFile, com_gamedir, MAX_QPATH/MAX_OSPATH, MAX_MAP_LEAFS, MAXLIGHTMAPS, MIPLEVELS, Q_strncmp.

---

##### n64/port/q64_map.h (395 lines; git: a19f97e, modified)
Verdicts: **CLEAN** (26-166 format definitions), **QUAKE-INTERFACE** (168-389 runtime API: mtexinfo_t, mleaf_t, mplane_t, msurface_t, dclipnode_t, model_s, texture_s, qboolean, byte). Defines the `.q64` v1 on-disk layout; node/leaf/face/texinfo/plane/clipnode/submodel records mirror the BSP29 record fields (field order of mnode_t/mleaf_t members, dplane_t, dclipnode_t, dmodel_t) because the loader fills Quake structs; the layout is our packed format (extra fields cluster_id, face_flags, reserved).
Automated hits: XASH n 28-52 (studio.h:46-62), QCTRANS n 35-51 (g_local.h:80), HLSDK n 54-81 (APIProxy.h:39-105), QUAKE n 54-81 (QW scitech mgraph.h:779-789): all false positives (runs of #define lines and `typedef struct __attribute__((packed))` declaration openings; no shared content).

---

##### n64/port/q64_world.c (1475 lines; git: a33b268 2026-07-19 (new file in that commit, 143 lines then), ae46c44; modified in worktree)
Verdicts: **CLEAN**, **QUAKE-INTERFACE**. Seamless-campaign controller: CWG3 graph loader (cwg_load, 178-326, big-endian record decode with extensive validation), placement queries (340-398), map presence (402-419), seam switch profiler (421-665: ring of frame times, per-span profile, frame split reports), box_has and seg_hits_box (688-729; textbook slab test, independent), Q64_WorldNewMap/PortalCross/Bootstrap, entity residency (911-1043), W_Portal (1045-1244; twin handoff, entity dehydrate, player origin shift, SV_WorldSetHome), near_seam, Q64_WorldFrame (1287-1429), Q64_WorldCrossView, stats line. Original design documented in n64/docs/stream-arch-v2.md §6 and seamless-uplink.md. The a33b268 initial version was written fresh (no Quake sv_main changelevel code present in it). No structure shared with Quake sv_main.c SV_SpawnServer / Host_Changelevel, Xash sv_main.c level-change code, or HLSDK dlls/triggers.cpp CHANGELEVEL/landmark logic (checked conceptually: ours moves the player by a cooked per-seam delta and keeps both maps live; HLSDK builds a ChangeList and reloads).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 1-135, 339-419 | CWG decode helpers, queries | CLEAN | — | ours. Hits: XASH e 100-101 (lib_apple.c:42) = big-endian 4-byte combine idiom, false positive. |
| 178-326 | cwg_load | CLEAN | — | ours. LIBDRAGON n 262-274 hit = run of consecutive field reads, false positive. |
| 421-686 | switch profiling | CLEAN | — | ours. |
| 688-729 | box_has, seg_hits_box | CLEAN | — | generic slab test; no Quake/Xash match. |
| 739-1043 | init, new map, seam find, PortalCross, entity open/close | CLEAN | — | ours. |
| 1045-1244 | W_Portal | CLEAN | — | ours (QCTRANS n 1081-1084 / XASH n 1081-1085 = printf argument lists naming `->v.*` fields: false positives). |
| 1247-1429 | near_seam, Q64_WorldFrame | CLEAN | — | ours. |
| 1431-1475 | CrossView, stats | CLEAN | — | ours. |

Quake/engine bindings (to retarget): edict_t (free, w_layer, v.* fields: origin, oldorigin, absmin, absmax, mins, maxs, view_ofs, velocity, avelocity, angles, ltime, nextthink, state, movetype, health, armorvalue, items, weapon, frame, model, classname, targetname, enemy, goalentity, groundentity, owner), EDICT_NUM, EDICT_TO_PROG, sv.name/time/num_edicts/active, svs.maxclients/clients, SV_Physics, SV_LinkEdict, SV_SpawnServer, SV_WorldSetHome, SV_WorldMapOpened/Closing, SV_PortalNotify, Host_Error, Host_ServerFrame, Sys_Printf/Sys_FloatTime/Sys_Error, Con_Printf/DPrintf, COM_FOpenFile, va, Q_strcasecmp, Q_strncpy, S_SfxNamesPeak, vec3_t helpers; plus our game layer (G_Str, HLE_*, HL_* from hl_ents/hl_levelstate/hl_ai/hl_use, Mod_StudioPrefetch*).

---

##### n64/port/q64_world.h (66 lines; git: a33b268, modified)
**CLEAN**; QUAKE-INTERFACE (light): `struct edict_s`, `qboolean`, `struct w64_layer_s`. API declarations for q64_world.c. No contamination.

---

##### n64/port/w64_world.c (1334 lines; git: untracked)
Verdicts: **CONTAINS-QUAKE**, **QUAKE-INTERFACE**; remainder CLEAN. Header comment (lines 6-8) and w64_world.h (15-18) openly say the hull walks are "line-for-line replicas of the stock engine functions". Verified against `first/source/world.c`, `model.c`, `sv_main.c`; confirmed identical to `quake/WinQuake/*`. Comment_overlap.md also flags 934-978 (45 words copied verbatim from world.c:640-). The rest (package open pipeline, byte swap of the big-endian package, CRC32 with slicing tables, layer slot allocator, directory parse, async Advance state machine, grid start-node lookup, PVS row cache with ROM reads, light probe) is ours.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 29-174 | byte-swap helpers, SwapDir/SwapColl | CLEAN | — | ours (package-specific). |
| 176-345 | CRC table/run, layer slot alloc, W64_LayerMap/Find/Close, ForgetModels | CLEAN | — | ours. |
| 355-441 | Locate, CheckHeader, ParseDirectory (setup) | CLEAN | — | ours. |
| 443-458 | ParseDirectory submodel table | CONTAINS-QUAKE (renamed copy; comment names Mod_LoadSubmodels) | first/source/model.c:Mod_LoadSubmodels 598-612 | ±1 spread of mins/maxs, origin, headnode copy for MAX_MAP_HULLS, visleafs. Same Xash mod_bmodel.c:2271-2276 (Quake heritage). Automated hits: XASH n 451-457, QUAKE e 453-456. REAL (small). |
| 461-659 | CountRend, Fetch, Advance state machine, FreeSlot, LayerOpenAsync/Step/CountIdle/Finish | CLEAN | — | ours. QUAKE n 535-540 hit (quake2 p_client.c memset): false positive. |
| 691-704 | SetHull | CLEAN (our helper); numbers: see 785-800 | — | ours. |
| 706-721 | SetSubmodel | CONTAINS-QUAKE (renamed copy) | first/source/model.c:Mod_LoadBrushModel 1191-1204 (and Xash mod_bmodel.c:2153-2177, Quake heritage) | hulls[j].firstclipnode from headnode, firstmodelsurface/nummodelsurfaces reset, VectorCopy maxs then mins, RadiusFromBounds, numleafs = visleafs: same order. Auto hits QUAKE e 711-714, 716-720; XASH n 716-720. REAL (small). |
| 727-811 | W64_LoadBrushModel | CLEAN (model_t field setup is ours) / QUAKE-INTERFACE | — | ours; 785-800 hull extents: lines 797-799 reproduce Quake's two hull sets (-16/-24/32, -32/-24/64) from Mod_LoadClipnodes (CONTAINS-QUAKE data constants, 3 lines); 791-793 GoldSrc hulls (HL facts; cf. hlsdk/dlls/util.h:414). |
| 815-855 | W64_LoadSubmodel, W64_ModelLayer | CLEAN | — | ours. |
| 859-878 | W64_HullPointContents | CONTAINS-QUAKE (renamed copy) | first/source/world.c:SV_HullPointContents 490-515 (non-asm version) | Same loop: axial planes use p[type]-dist else dot product - dist; d<0 → child[1] else child[0]; returns leaf contents. Only the node fetch is replaced. Node-number bound check removed. REAL. |
| 880-996 | W64_RecursiveHullCheck | CONTAINS-QUAKE (renamed copy, comments retained) | first/source/world.c:SV_RecursiveHullCheck 576-704 (identical in quake/WinQuake/world.c; Xash pm_trace.c:206-317 is a modified descendant — our code follows the Quake form: "if (t1 < 0)" branch for frac, `side=(t1<0)`) | Everything: DIST_EPSILON definition and 1/32 value, empty-leaf handling (allsolid/inopen/inwater/startsolid), t1/t2 computation with axial shortcut, both-sides recursion, epsilon crosspoint with clamp, midf/mid interpolation, move-up/go-past recursion, impact plane (normal/dist, negated for back side with vec3_origin subtract), the "shouldn't really happen" backup loop with 0.1 frac step and the "backup past 0" debug print, and the section/banner comments ("put the crosspoint DIST_EPSILON pixels on the near side", "move up to the node", "go past the node", "never got out of the solid area", "the other side of the node is solid, this is the impact point"). Differences: node fetched via FetchNode into a local copy, normal copied once (916), bad-node check dropped, LeafContents indirection. Automated hits: QUAKE e/n 897-908, 930-951, 957-967, 973-996 (123 tok). REAL. |
| 998-1051 | W64_NodeMaterial (binary search over material entries) | CLEAN | — | ours; HLSDK texture-type concept only. |
| 1055-1075 | StartNode (grid start node) | CLEAN | — | ours. |
| 1077-1099 | W64_PointInLeafNum | CONTAINS-QUAKE (structural translation; comment cites it) | first/source/model.c:Mod_PointInLeaf 84-103 | descent on d>0 → child[0] else child[1] with dot-product minus dist; start node via grid is our addition; returns leaf index. Small, comment says "model.c Mod_PointInLeaf". REAL (small). |
| 1101-1113 | W64_LeafRef | CLEAN | — | ours. |
| 1115-1144 | TouchedLeafs / W64_FindTouchedLeafs | CONTAINS-QUAKE (structural translation; comment cites it) | first/source/world.c:SV_FindTouchedLeafs 328-364 | solid skip, MAX_ENT_LEAFS check, leafnums[num_leafs++] = leaf-1, BOX_ON_PLANE_SIDE on absmin/absmax, recurse on sides&1 / sides&2. Uses a synthesised plane struct. REAL. |
| 1157-1182 | Decompress (vis RLE) | CONTAINS-QUAKE (structural translation; comment cites it) | first/source/model.c:Mod_DecompressVis 115-146 | same RLE loop, all-0xff for NULL input, do-while until row filled. REAL. |
| 1184-1231 | W64_LeafPVS | CLEAN | — | ours (ROM offset table read, LRU rows, uses mod_novis). Uses Quake's mod_novis global (binding). |
| 1233-1274 (counted 1233-1267) | FatPVS, W64_AddToFatPVS | CONTAINS-QUAKE (structural translation; comment cites sv_main.c) | first/source/sv_main.c:SV_AddToFatPVS 367-399, SV_FatPVS 410-418 | leaf: OR the PVS row of non-solid leaf; node: d>8 front, d<-8 back, else recurse both (the ±8 margin and "go down both" logic). REAL. |
| 1276-1333 | W64_LightProbe | CLEAN | — | ours (probe grid bilinear). |

Automated hits dispositions: QUAKE e 711-714, 716-720 vs model.c 1194-1205: REAL; XASH n 716-720, e 718-720, n 451-457: attributed to Quake lineage; QUAKE e/n 882-884, 897-908, 930-951, 957-967, 973-996: REAL; XASH e 897-908, 935-939/944, 989-996 (pm_trace.c): Quake heritage (same text in quake/WinQuake/world.c; Xash's own variants differ, e.g. branch on `side`), attribute to Quake; QUAKE e 453-456 (model.c:604-606): REAL (part of 443-458); QUAKE n 535-540 (quake2 p_client.c): false positive.

Quake bindings: model_t (type, w64_layer/w64_submodel fields, hulls[], numleafs, radius, mins/maxs, hle/hlelen, needload NL_*), mod_known/mod_numknown, mod_brush, hull_t, trace_t, dmodel_t, mplane_t, mleaf_t (contents/key), dclipnode_t, edict_t (num_leafs, leafnums, v.absmin/absmax, w_layer), sv.worldmodel, mod_novis, MAX_MAP_HULLS, MAX_MAP_LEAFS, MAX_ENT_LEAFS, MAX_MAP_LIGHTSTYLES, CONTENTS_*, BOX_ON_PLANE_SIDE, DotProduct/VectorCopy/VectorSubtract, vec3_origin, Sys_Error/Printf, Con_DPrintf, RadiusFromBounds, Q_strncpy, BigShort/BigLong, com_gamedir, SV_WorldMapOpened, SV_LayerAt; SV_FindTouchedLeafs / SV_AddToFatPVS / Mod_PointInLeaf are the replaced originals (names only).

---

##### n64/port/w64_stream.c (2286 lines; git: untracked)
Verdicts: **CLEAN**, **QUAKE-INTERFACE**. Residency scheduler for the three chunk pools: want classes (open/pin/prefetch), request slots on q64_io, MakeRoom (evict/slide/steal with hole search), fence retirement, collision-grid cell wants (CellSpan/WantCellRange/WantBodyCells), link-time edict classification (W64_StreamLinked), pin/teleport tracking, per-frame pump, seam set streaming, stats. No similarity hits at 24/48 or 16/30 tokens. Structural review: collision wanting is a uniform grid of COLL lists (w64_grid_t), not Quake's areanode tree (SV_CreateAreaNode/SV_LinkEdict/SV_TouchLinks); entity classification (W64_StreamLinked 1243-1283, WantCollision 1445-1600) is our policy; frustum plane construction in W64_CountMissing (2105-2152) builds side-plane normals from view vectors by a direct sin/cos form and a corner-select box test, not Quake R_SetFrustum (TurnVector/RotatePointAroundVector) or BoxOnPlaneSide. SP_* timing macros, FloorI and CRC are generic. No Xash/HLSDK match.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 1-139 | header, constants, profile structs | CLEAN | — | ours. |
| 140-1107 | pool/slot/steal/slide/load/wants for chunks | CLEAN | — | ours. |
| 1108-1138 | WantMovers | CLEAN / QUAKE-INTERFACE | — | reads cl.num_entities, cl_entities[], entity_t model/msgtime, cl.mtime[0]. |
| 1140-1220 | FloorI, CellSpan, WantCellRange, WantCells | CLEAN | — | ours. |
| 1222-1327 | W64_StreamLinked, WantBodyCells | CLEAN / QUAKE-INTERFACE | Quake sv_world.c SV_LinkEdict (concept only) | ours (kind bitmaps), reads edict fields. |
| 1329-1443 | BoxesTouch, WantModel, W64_StreamPin, WantPins, InPin | CLEAN / QUAKE-INTERFACE | — | ours. |
| 1445-1600 | WantCollision | CLEAN / QUAKE-INTERFACE | — | ours. |
| 1602-1647 | W64_ViewLeaf, WantRender | CLEAN / QUAKE-INTERFACE | — | ours. Uses r_refdef.vieworg, cl.velocity, cls.signon, cl.worldmodel. |
| 1649-1900 | RetireFences, DropStale, StepLayers, StreamFrame, pump | CLEAN | — | ours. |
| 1901-2100 | open/close/home switch/seam wants | CLEAN | — | ours. |
| 2101-2286 | frustum miss counting, render-miss accounting, stats | CLEAN / QUAKE-INTERFACE | — | ours; frustum math independent. Uses vpn/vup/vright, r_refdef.fov_x/fov_y/vieworg. |

Quake bindings: edict_t and its v.* fields (movetype, solid, modelindex, skin, flags, nextthink, velocity, absmin/absmax, mins/maxs, view_ofs, origin; constants SOLID_TRIGGER, SOLID_BSP, SOLID_NOT, MOVETYPE_TOSS/BOUNCE/FLYMISSILE/FLY, FL_MONSTER), EDICT_NUM, NUM_FOR_EDICT, sv.active/num_edicts/models, sv_player, MAX_EDICTS, ED_ClearEdict and SV_LinkEdict (hook comments), SV_LayerAt, SV_SeamAsleep, SV_PrecachePeaks, SV_LightstylePeak, G_TeleportDestination; cl.num_entities/mtime/velocity/worldmodel, cl_entities[]/entity_t, cls.signon/SIGNONS; model_t (w64_layer/w64_submodel), Mod_IsW64; r_refdef (vieworg, fov_x, fov_y), vpn/vup/vright; Sys_Error/Sys_Printf.

---

##### n64/port/q64_route.c (541 lines; git: untracked)
Verdicts: **CLEAN**, **QUAKE-INTERFACE**. Script runner for scripted playthroughs (route text parse 78-129, wait/waitmap/waitnear/waitglobal/waitmoved/waitgone/waitclass/waitend/look/lookat/shot/stats/pass steps, setpos pin lookahead 298-325, stats line 232-271). Original; no automated hits at either granularity. Console command passthrough uses Cbuf.

Quake/engine bindings: sv.active/name/time/num_edicts, svs.maxclients/clients[0].active, cls.state==ca_connected, cls.signon==SIGNONS, cl.viewangles (PITCH/YAW/ROLL), edict_t + v.targetname/classname/solid/origin/pos1/velocity/absmin/absmax/view_ofs/health/armorvalue/items/weapon, pr_strings (QuakeC string pool), EDICT_NUM, Q_strcasecmp/Q_atof, Cbuf_AddText, Con_Printf, va, SOLID_NOT, MAX_EDICTS, HLE_EntitySlot/HLE_Primary (our game), HL_GlobalState, HL_EndSectionFired, HL_Weapons_StatsLine, Q64_WorldStatsLine, W64_StreamPin, w64_home->gofs. GPL "quakegeneric contributors" header present.

##### n64/port/q64_route.h (23 lines; git: untracked)
**CLEAN**. Declares Route_Load/Route_Frame/Route_Step and platform hooks; needs `qboolean` typedef only.

---

##### source/w64_format.h (297 lines; git: untracked)
Verdicts: **CLEAN** (all structs, flags, chunk fourcc, gid macros, static asserts); one **HLSDK-data** item: W64_MAT_CHARS (line 119; comment 110-118) — the set of texture-type letters is the HLSDK CHAR_TEX_* letter set (hlsdk/pm_shared/pm_shared.c:93-103, dlls/sound.cpp), table index of surface material. Data constant, not code expression; if it goes under game/ it is covered by the HLSDK licence. No Quake/Xash expression. Automated hits QUAKE n 14-60 (asm_i386.h:39-142) and XASH n 14-60 (studio.h:36-97): false positives (consecutive #define lines).

##### source/w64_node.h (81 lines; git: untracked)
Verdicts: **CLEAN**; **QUAKE-INTERFACE** (hull_t, `->clipnodes`, `->w64_layer` fields of hull_t, W64_HULL0_MARK from model.h). Inline decoder of the COLL node format (DecodeNode, FetchNode, CollChunk, LeafContents) — ours, tied to the w64 format.

##### source/w64_world.h (287 lines; git: untracked)
Verdicts: **CLEAN** (layer struct, chunk residency enums, stream API, stats struct); **QUAKE-INTERFACE** (model_t* worldmodel at 98; dmodel_t* submodels at 122; byte* pvs_rows at 124; W64_LoadBrushModel/W64_LoadSubmodel/W64_ModelLayer at 152-155; hull_t/trace_t at 182-189; mleaf_t at 193-194; edict_t at 196 and 219; vec3_t at 184, 234). Comment at 15-18 documents that w64_world.c replicates Quake functions — that statement becomes false once the Quake regions are rewritten. No contamination in the header itself.

---

#### 3. Cross-cutting findings

1. **The only genuinely Quake-derived expression in this slice is the BSP-loader / hull-walk / PVS layer**, spread over three files: w64_world.c (~286 lines, the most substantial: SV_RecursiveHullCheck 117 lines incl. verbatim comments), q64_brush.c (~163: Mod_LoadTextures animation sequencing, Mod_LoadClipnodes hulls, Mod_LoadSubmodels, Mod_LoadBrushModel submodel loop), q64_map.c (~69: Mod_LoadTexinfo mipadjust, Mod_LoadFaces sky/turb flags, Mod_LoadPlanes, Mod_DecompressVis).
2. **Xash and HLSDK add nothing**: every Xash hit is on code that exists identically in id Quake (pm_trace.c, mod_bmodel.c are Quake descendants), and our text follows the Quake form. No Xash sv_world.c area-node or common/world.c code was reproduced. No HLSDK dlls code appears.
3. **Original, importable designs**: CWG3 seam/campaign layer (q64_world.c), w64 package format and residency scheduler (w64_stream.c, w64_format.h, w64_node.h, most of w64_world.c), .q64 fault-in caches (q64_map.c bulk), route runner (q64_route.c). All depend on Quake engine types; retargeting list per file above. The main retarget surfaces are: edict_t/entvars (`v.*`), sv./svs./cl./cls., model_t/hull_t/mleaf_t/mplane_t/msurface_t/mtexinfo_t/texture_t, Hunk_*, COM_FOpenFile, Con_/Sys_ output, Mod_* helpers, r_refdef/vpn/vup/vright.
4. **Clean-room guidance for the replaced regions**: the behaviours to reimplement from spec (not from Quake text) are: BSP clip-hull point contents and segment trace with epsilon crosspoint and impact-plane reporting; point-to-leaf descent; entity-box-to-leaf marking; fat PVS union with ±8 unit margin; compressed vis row expansion; BSP29/30 anim-sequence naming ('+0'..'+9', '+A'..'+J'); texinfo mip adjust; plane sign bits. These are documented behaviours/formats; the problem is only that our current text reproduces Quake's wording and structure.
5. **Tracked-file history**: q64_brush.c, q64_map.c, q64_map.h were created in a19f97e as new files (no Quake file was copied wholesale); contaminated blocks entered in a19f97e (hull setup, submodel loop, texinfo mipadjust, sky/turb flags) and 5cfba32 (anim sequencing, "Same RLE as Mod_DecompressVis" vis decoder), cf5c3a9 (plane decode). No "Id Software" header in any file of this slice. q64_world.c/h created in a33b268 with no Quake text.


### A.AuditRendA — Renderer A (world, sector, light, tex, 2D)


Slice: r64_world.c, r64_sector.c, r64_light.c, r64_tex.c, r64_world_atlas.c, r64_world_tex.c, r64_main.c, r64_vid.c, r64_draw.c, r64_part.c, r64_local.h, frame_stats.c (all `~/src/ports/quakegeneric/n64/port/`). Working tree audited as it is now; history via `git log/show` only.

Method: automated evidence.md / evidence_k16.md / comment_refs.txt / comment_overlap.md as the start; then full read of every file; structural comparison against first/source (WinQuake software), quake/WinQuake (GLQuake gl_rsurf.c/gl_rmain.c/world.c), Xash ref/gl gl_rsurf.c, HLSDK cl_dll/hud.h; `git show` of first commits (3ca6f55, b92a230, 8343727, 1ccc03a) for origin.

Conventions: "strong" = renamed/near-verbatim copy or step-for-step structural translation of Quake code. "weak" = same idiom / format-mandated shape (BSP edge walk, LIFO texture chain, per-entity surface loop) that a clean re-implementation from the BSP spec would plausibly reproduce; flagged so the clean-room reviewer can decide. Xash/ref-soft copies of the same code exist (Xash gl_rsurf.c R_TextureAnimation :513, R_AddDynamicLights :566, R_MarkLeaves :3769; ref/soft r_main.c:1032) but are Quake-lineage: in every case the same code is in quake/ or first/source, so the source is attributed to Quake. No Xash-only, no HLSDK-only code found in the slice, except one HLSDK constant (r64_draw.c:296-297).

All files carry the GPL header with "Copyright (C) 2026 quakegeneric contributors" (r64_world_atlas.c / r64_world_tex.c carry no licence header at all). The Id Software line survives in r64_world/light/main/draw/part/tex as header residue from the first commit; r64_vid.c and frame_stats.c never carried it.

#### 2. Per-file sections

##### n64/port/r64_world.c (2977 lines; git: 3ca6f55, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE. Origin: first commit 3ca6f55 ("hardware-rendered BSP world via tiny3d") was a fresh tiny3d renderer whose MarkLeaves, SurfaceFacing, brush-entity facing loop and texture-chain loop were Quake renderer logic written straight into the file; commit message states "Animated textures resolve through the GLQuake R_TextureAnimation logic". Since then the file grew by ~2500 lines of original arena/bake/streaming/batch code. What remains of the Quake code is listed below. The Id Software header is residue of that origin.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-37, 39-132|header, includes, SoA/bake pool types, WS_* accessors|CLEAN|—|ours (sparse bake pool; no Quake equivalent)|
|40|BACKFACE_EPSILON 0.01|CONTAINS-QUAKE (const)|first/source r_local.h BACKFACE_EPSILON; gl_rsurf.c use|same name and value|
|133-296|r64surf_t, vert arena, matrices, globals|CLEAN|—|ours|
|277-291|VertexLightCtx|CLEAN|—|ours (per-vertex lightmap sampling, not luxel texture build)|
|300-327|InitWorldSurfMeta|CLEAN|—|texinfo→texture index lookup is format-trivial|
|336-476|BuildSurfaceVerts|QUAKE-INTERFACE; weak echo 359-372, 379-385|quake/WinQuake/gl_rsurf.c:BuildSurfaceDisplayList(1469-1486)|edge-walk (surfedges sign selects vertex 0/1) and s/t = dot(vecs)+offset are BSP-format-mandated; the first-commit BuildSurface (3ca6f55) had the same shape; the rest (gather callback, atlas UV, period rebase, s10.5 pack, hoisted light contexts) is original|
|474-545|InitSurfaceMeta, BuildSurface|CLEAN|—|ours|
|546-592|WS_BakeKept/WS_Use/WS_AgeStamps/WorldArenaInit|CLEAN|—|ours|
|594-661|WorldMatsPrealloc, R64_PreallocUncachedPools|CLEAN|—|ours|
|662-1010|WorldArena* (free list, compact, evict)|CLEAN|—|ours; textbook free-list allocator|
|1009-1140|EnsureWorldSurface|CLEAN|—|ours|
|1141-1260|SurfaceStyleKey, RelightSurface|CLEAN (QUAKE-INTERFACE)|—|per-vertex relight is our design; uses R64_*Surf* helpers (see r64_light.c for Quake-lineage inside them)|
|1261-1470|FreeStandaloneBrushes, R64_WorldNewMap, R64_WorldLayerSwitch|CLEAN|—|ours|
|1479-1553|memstats/arena stats accessors|CLEAN|—|ours|
|1555-1602 (1567-1574 hooks ours)|MarkLeaves|CONTAINS-QUAKE (strong)|first/source r_main.c:R_MarkLeaves(488-515); quake/WinQuake/gl_rsurf.c:R_MarkLeaves(1347-1384); (Xash ref/soft r_main.c:1032 / ref/gl gl_rsurf.c:3774 same lineage)|identical control flow, same names (r_visframecount, r_oldviewleaf, vis[i>>3]&(1<<(i&7)), parent climb with visframe test); index-based parent climb and Q64 hooks are the only changes. First commit 3ca6f55:210-238 was a verbatim copy.|
|1604-1612|SurfaceFacing|CONTAINS-QUAKE (strong, 9 lines)|first/source r_bsp.c R_RecursiveWorldNode / quake gl_rsurf.c R_DrawBrushModel plane-side test|renamed copy: SURF_PLANEBACK with ±BACKFACE_EPSILON|
|1614-1753|LeafDistBand, RebuildMarkCandidates, ChainMarkCandidates|CLEAN|—|ours (near/mid/far PVS banding, candidate cache, two-phase stamping)|
|1755-1860|BatchInit/Flush/AddVerts, DrawSurface|CLEAN|—|ours (RSP vertex-cache batching)|
|1861-1966|DrawWorldSurface, DrawOneSurfaceChain|CLEAN|—|ours|
|1967-2099|DrawTextureChains|CLEAN main path (1986-2078, atlas sheet sort); weak echo 2081-2098|quake/WinQuake/gl_rsurf.c:DrawTextureChains|per-texture loop "skip empty chain → animate → bind → draw chain" is the GL idiom; main path is sheet-sorted and unlike GL|
|2101-2134|ChainSurface, ChainWorldSurface|weak echo 2120-2121|gl_rsurf.c R_RecursiveWorldNode texturechain push|LIFO per-texture chain link (index arrays, not surf->texturechain)|
|2136-2260|Setup3D, R64_SetViewProjection, R64_SetEyeSpaceView, R64_RestoreWorldView|CLEAN|—|ours (tiny3d viewport)|
|2273-2298|BrushEntityMatrix|CLEAN|quake/WinQuake/world.c:SV_ClipMoveToEntity QUAKE2 block(750-790) convention|forward/-right/up axes is the AngleVectors rotation convention; comment cites SV_ClipMoveToEntity; matrix assembly is tiny3d-specific|
|2300-2316|ModelSpaceView|CONTAINS-QUAKE (strong, renamed)|quake/WinQuake/gl_rsurf.c:R_DrawBrushModel(1075-1150; modelorg = vieworg - origin, rotated by forward, -right, up at ~1103-1115)|renamed copy of the modelorg rotation (with a zero-angle shortcut)|
|2323-2389|EnsureStandaloneBrush|CLEAN|—|ours|
|2391-2466|DrawStandaloneBrushEntity|weak echo 2413-2433|gl_rsurf.c:R_DrawBrushModel surface loop|loop over firstmodelsurface..nummodelsurfaces with SurfaceFacing then chain; ours chains per texture, uses local_chain|
|2468-2512|DrawWorldBrushEntity|weak echo 2479-2494|same|same shape; ours defers draw through shared DrawTextureChains|
|2515-2552|BrushEntityCulled|CLEAN|gl_rsurf.c:R_DrawBrushModel cull box(mins/maxs, radius)|ours: bounding sphere about rotated AABB centre + R64_CullSphere (different method)|
|2554-2618|IsStaticWorldBrush, ChainStaticWorldBrush, DrawBrushEntity|CLEAN|—|ours (static *N fold into world chains)|
|2620-2790|R64_IdentityMat, stats, WorldTimeNote, DrawWorldV2|CLEAN|—|ours; DrawWorldV2 uses W64 index leaves|
|2784-2977|R64_DrawWorld|CLEAN (QUAKE-INTERFACE)|—|frame orchestration ours; call order world→alias→studio→particles→translucent→viewweapon is the cart's; uses Mod_PointInLeaf (2845), R_DrawParticles (2958)|

Quake bindings (retarget list):
- Model/BSP: cl.worldmodel (46 uses), model_t, msurface_t (20), mleaf_t, mnode_t, mplane_t, mtexinfo_t, texture_t, Mod_Surface/Mod_Leaf/Mod_LeafNum/Mod_SurfaceTexinfo/Mod_SurfacePlane/Mod_NodeParent/Mod_PointInLeaf/Mod_LeafPVS/Mod_LoadModel (index-based variants of Quake Mod_*), SURF_DRAWSKY, SURF_PLANEBACK, MOD_PARENT_NONE, MAX_LIGHTSTYLES, MAXLIGHTMAPS, mod_brush, model->marksurfaces/leafs/nodes/submodels/firstmodelsurface/nummodelsurfaces.
- Refresh state: r_refdef.vieworg/viewangles/vrect/fov_y, r_origin, vpn/vup/vright, AngleVectors, entity_t, cl_visedicts/cl_numvisedicts, R_PushDlights/R_NewMap/R_LayerSwitch/R_DrawParticles (the R_* seam), d_lightstylevalue.
- Memory/console: Hunk_AllocName (10), Hunk_Alloc, Con_Printf/Con_DPrintf, Sys_Error, vid.width/height.

Automated hits: 1555-1565 / 1575-1580 QUAKE e, 1577-1580 XASH e (r_main.c MarkLeaves / ref-soft r_main.c:1032): REAL — see MarkLeaves row; Xash is Quake-lineage. 1235-1236 (QUAKE n r_light.c:193; XASH n sv_world.c:1535) texinfo-projection formula: false positive (format formula, in RelightSurface only through helper). 1290-1302 (HLSDK hud_servers.cpp), 2216-2224 (HLSDK hud_spectator, XASH img_main), 2150-2156 / 2250-2257 (XASH vgl_shim), 2880-2887 (QUAKE pmove.c, XASH gnm_shim): false positives (generic vector zeroing / vpn assignment / memcpy of tex_chain).

##### n64/port/r64_sector.c (2089 lines; git: untracked)
Verdict: CLEAN (+QUAKE-INTERFACE, + two CONTAINS-QUAKE fragments). Original stream-arch-v2 renderer over the cooked .w64 package; comment cites of GoldSrc (cl_main.c CL_UpdateFlashlight, line ~665) and Quake face order describe behaviour only; no Xash/HLSDK code or structure found in the flashlight pool (checked against Xash/HLSDK; the pool is a custom wedge/clip algorithm).

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-135|header, tables, mover slots, globals, Meshlets()|CLEAN|—|ours|
|136-346|Page, stats, Reencode, rend_tables, R64_SectorOnInstall, IndexSelfCheck, Tables, BindLayer, R64_SectorNewMap(347-383)|CLEAN|—|ours (tiny3d index re-encode)|
|386-406|SetFrustum|CLEAN|quake gl_rmain.c:R_SetFrustum (concept)|four side planes built from fov half-angles with sin/cos on vpn/vright/vup, different math to Quake's rotate-point-around-vector; shared concept only|
|407-429|BoxCulledP, BoxCulled|CLEAN|—|p-vertex AABB vs plane; not Quake's BoxOnPlaneSide|
|430-453|TexAnim|CONTAINS-QUAKE (renamed copy of R64_TextureAnimation, itself a renamed copy of R_TextureAnimation)|first/source r_surf.c:R_TextureAnimation(205-232); quake gl_rsurf.c; Xash gl_rsurf.c:513-560|same %anim_total modulus of cl.time*10, same anim_min/anim_max walk and 100-step guard; variable renamed (rel for the original misspelling); index arrays instead of pointers|
|454-513|FrameBegin|CLEAN|—|ours|
|515-551|Relight|CLEAN; weak echo 540-546|first/source r_surf.c:R_BuildLightMap (style accumulate)|luxel×style value sum then >>8: lightmap-format arithmetic; ours recolours cooked per-vertex luxels|
|553-579|NormComp, NormTables|CLEAN|—|ours|
|581-672|DlightVerts|CONTAINS-QUAKE for 634-649 (falloff sequence); remainder CLEAN|first/source r_surf.c:R_AddDynamicLights(55-135); quake gl_rsurf.c:R_AddDynamicLights(75-130); Xash gl_rsurf.c:566|step-for-step: rad = radius minus abs(plane distance); skip if below minlight; minlight reuse as rad minus minlight; impact projected onto the plane; add if in-plane distance < the reused value. Metric replaced (Euclidean, squared compare) and per-vertex normals come from the cook; sequence and field names (radius, minlight) are Quake's. Light-vs-meshlet bbox culling and the scratch copy are ours.|
|675-1307 (FL_* types 706-747, FlashFrame 749, FlCut 829, FlFlush 861, FlRestore 889, FlEnd 900, FlVertex 912, FlPiece 957, FlPos 980, FlAttr 997, FlSector 1008, FlVSec 1035, FlWhole 1044, FlTri 1096)|flashlight pool|CLEAN|GoldSrc CL_UpdateFlashlight (behaviour only, in comment)|original wedge/cut geometry; falloff expression cites "radius minus plane distance minus in-plane distance" (same as Quake dlight model: documented behaviour)|
|1308-1452|FlFace (1308), FlashPool (1407)|CLEAN|—|ours|
|1453-1538|RdlShape, RdlBuild, BindPage|CLEAN|libdragon rdpq_tex.c (behaviour replicate)|raw RDP command list replicating libdragon's CI4+TLUT upload; library, not Quake/Xash|
|1539-1613|DrawList, SetDrawFlags|CLEAN|—|ours|
|1614-1773|MoverIndex, MoverPage, ListMover, R64_SectorMarkWorld|CLEAN|—|ours|
|1774-1900|ClipTouch, DrawCross, R64_SectorDrawWorld|CLEAN|—|ours (seam cross-draw)|
|1901-2089|MoverSlot, SlotMatrix, ListEntMover, R64_SectorDrawMovers, R64_SectorDrawMover|CLEAN|quake world.c:SV_ClipMoveToEntity (axes convention)|AngleVectors forward/-right/up axes in ListEntMover is the convention; frustum into submodel space via transposed axes is ours|

Quake bindings: entity_t (5), cl_visedicts/cl_numvisedicts, cl.time, cl.worldmodel (via Mod_PointInLeaf in sibling), r_refdef.vieworg/fov_x/fov_y, r_origin, vpn/vright/vup, AngleVectors, dlight_t + cl_dlights + MAX_DLIGHTS (radius/minlight/die/bright/origin fields), cl_flashlight (cl_main.c, other slice), d_lightstylevalue, cvar_t + Cvar_RegisterVariable (r_flashpool, r_crosslayer, r_lightscale), Con_Printf/Con_DPrintf, Sys_Error, R_NewMap (comment), VectorCompare/VectorSubtract/DotProduct/CrossProduct (q_shared math).

Automated hits: 1131-1137 (HLSDK studio_util.cpp:76, QUAKE mathlib.c:365): false positive (2-D cross product of triangle vertices, FlTri).

##### n64/port/r64_light.c (653 lines; git: b92a230, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE. `git show b92a230` shows the file began as "Mirrors source/r_light.c without the software-renderer headers": R_AnimateLight (same lightstyle map and 22 scale), R_MarkLights (recursive, surf->dlightbits/dlightframe), R_PushDlights, and R64_SampleLightmap/R64_SampleDlights built over R_BuildLightMap/RecursiveLightPoint/R_AddDynamicLights. Later commits (hoisting, hash marks, dlight fingerprint 1c78118, style dirty tracking, index-based nodes b9e829e/9a37c3d, v2 branch) replaced storage and added optimisation, but the arithmetic and control flow of the Quake functions persists.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-170 (hash marks, DL_*, SurfaceDlightKey/Bits/HasDlight)|dlight mark store|CLEAN|—|ours (sparse marks + open-addressed hash replace surf->dlightbits fields; fingerprint key is ours). Concept of per-face dlight mask derives from Quake R_MarkLights surf->dlightbits (idea only)|
|171-231 (199-216 core)|R64_AnimateLight|CONTAINS-QUAKE (strong, 18 lines)|first/source r_light.c:R_AnimateLight(24-49); quake WinQuake r_light.c; Xash ref/gl (same lineage)|same i%length, map[k]-'a', *22, 256 for length 0; ours adds style tick cap, constant-style shortcut, dirty snapshots|
|233-273|DlightsJustCleared, SurfaceStyleDirty, StylesDirty, NeedWorldRelight, AnyDlights|CLEAN|—|ours|
|275-325|MarkLights (iterative)|CONTAINS-QUAKE (structural translation, 51 lines)|first/source r_light.c:R_MarkLights(60-100); quake/WinQuake/gl_rlight.c:R_MarkLights|same plane-distance classification against light radius (front child / back child / mark node surfaces then both children), same early-out on leaf; recursion turned into an explicit stack|
|327-364 (349-360 core)|R_PushDlights|CONTAINS-QUAKE (12 lines) ; v2 branch 335-345 ours|first/source r_light.c:R_PushDlights(105-120)|same loop over MAX_DLIGHTS skipping die<cl.time or !radius, MarkLights(l,1<<i,nodes)|
|366-385|R64_DlightFrame, R64_UploadDlightsToT3D|CLEAN|—|ours|
|391-451 (410-424, 436-450)|R64_LightmapSurfBegin/Sample|CONTAINS-QUAKE (structural, 30 lines)|first/source r_light.c:RecursiveLightPoint tail(205-228); r_surf.c:R_BuildLightMap(135-170)|smax/tmax=(extents>>4)+1, loop styles!=255 up to MAXLIGHTMAPS, scale from d_lightstylevalue[style], accumulate sample*scale, advance by size, >>8; clamp and hoisting ours. Format-mandated layout, but the loop is a translation|
|453-459|R64_SampleLightmap|CLEAN|—|wrapper|
|469-518|R64_DlightSurfBegin|CONTAINS-QUAKE (50 lines)|first/source r_surf.c:R_AddDynamicLights(55-110); quake gl_rsurf.c:R_AddDynamicLights(75-130); Xash gl_rsurf.c:566|renamed copy of the per-light setup: rad=radius, dist=plane dist, rad-=|dist|, minlight test and reuse, impact point, local[] = dot(impact,vecs)+offset-texturemins; per-luxel loop hoisted out|
|520-548|R64_DlightSurfSample|CONTAINS-QUAKE (29 lines)|r_surf.c:R_AddDynamicLights inner loop(115-131)|same absolute sd/td, same approximate distance metric (larger + half smaller), same (rad-dist) add under minlight|
|550-617|R64_SampleDlights|CONTAINS-QUAKE (renamed copy, 68 lines)|r_surf.c:R_AddDynamicLights|full copy with single point instead of luxel grid|
|619-637|R64_PointDlights|CONTAINS-QUAKE (structural, 19 lines)|first/source r_main.c:R_DrawEntitiesOnList(569-580); quake gl_rmain.c:480-495|same entity-vs-dlight loop (origin - light origin, Length, radius - dist, add if > 0); ours multiplies by dlight bright|
|639-653|R64_FinishLight|CLEAN|—|ours (overbright scale r_lightscale, clamp 0..255)|
|cvar r_lightscale (31-33, 177-181, 230-236)|overbright cvar|CLEAN|GLQuake gl_overbright (concept)|ours|

Quake bindings: cl_dlights/dlight_t/MAX_DLIGHTS, cl_lightstyle[] (length/map), MAX_LIGHTSTYLES, MAXLIGHTMAPS, cl.time, cl.worldmodel, mnode_t/mplane_t/msurface_t/mtexinfo_t/model_t, msurface fields (styles, extents, texturemins, flags SURF_DRAWTILED), Mod_NodePlane/Mod_NodeChild/Mod_SurfaceSamples/Mod_SurfaceTexinfo/Mod_SurfacePlane/Mod_SurfaceNum (index-based wrappers), d_lightstylevalue (Quake global), cvar_t + Cvar_RegisterVariable (r_lightscale), R_PushDlights seam, Length/VectorSubtract/DotProduct.

Automated hits: 198-203, 213-217 (QUAKE e first/source r_light.c:40-49): REAL (AnimateLight). 299-303 (r_light.c:79-80): REAL (MarkLights splitplane). 350-355 (r_light.c:120-127; gl_rlight.c:120-127): REAL (PushDlights loop). 494-498, 499-503, 572-576, 578-582 (r_surf.c:80-87): REAL (dlight setup). 509-512, 588-593 (quake2 gl_light.c:400-401): REAL, same algorithm in Quake 2 lineage; first/source is the origin. 603-608 / 603-609 (r_surf.c:113-117; XASH gl_rsurf.c:645-648): REAL, same code in Quake; Xash copies Quake. 625-628 (r_main.c:568-570): REAL (PointDlights).

##### n64/port/r64_tex.c (801 lines; git: 3ca6f55, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE (one function). First commit's header comment: "Quake miptex..." and R_TextureAnimation "from GLQuake, world frame 0".

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-78|header, slot table, R64_TexPrealloc(48), Pow2Floor(59)|CLEAN|—|ours|
|79-219|PickPal16 (CI4 palette selection)|CLEAN|—|ours (luminance-band + popularity; no Quake equivalent)|
|220-309|PackCI4, BuildCI4Pal, BuildCI4|CLEAN|—|ours|
|311-391|CI8 variants (BuildCI8Pal, BuildCI8)|CLEAN|—|ours|
|394-452|FindOrAdd, TakeSlot, R64_TexForgetRange|CLEAN|—|ours|
|454-516|R64_TexForTexture|CLEAN (QUAKE-INTERFACE)|—|uses texture_t name/width/height/offsets, MIPLEVELS; mip-shift search is ours|
|517-549|R64_TexForPixels, R64_TexForPixelsPal|CLEAN|—|ours|
|551-725|R64_TexInstallCI4, R64_TexReset(627), R64_BakeCachesInvalidate(664)/Reinstall(686), R64_Tex_MemStats(704)|CLEAN|—|ours|
|725-762|R64_TextureAnimation|CONTAINS-QUAKE (renamed copy, 38 lines incl. comment)|first/source r_surf.c:R_TextureAnimation(205-232); quake/WinQuake/gl_rsurf.c:R_TextureAnimation; Xash ref/gl gl_rsurf.c:470-513 (same lineage)|same variable (including the original's misspelled "reletive"), same (int)(cl.time*10)%anim_total, same anim_min/anim_max loop, same "broken cycle" / "infinite cycle" errors and 100-count guard; frame argument replaces currententity->frame; comment says "see R_TextureAnimation"|
|764-801|R64_TexInvalidateBind, R64_TexUse|CLEAN|—|ours; comment cites OpenLara TEX_SKIP (idea)|

Quake bindings: texture_t (name, width, height, offsets, alternate_anims, anim_total/min/max/next), MIPLEVELS, cl.time, Sys_Error, Con_DPrintf, Q64_TexReadMip (our .q64 stream).

Automated hits: 745-755/745-761 (QUAKE r_surf.c:224-232), 750-754 (XASH gl_rsurf.c:498-500): REAL (R64_TextureAnimation).

##### n64/port/r64_world_atlas.c (379 lines; git: 5c5eea5)
Verdict: CLEAN + QUAKE-INTERFACE. Loader and binder for our own .wats multi-tile CI4 sheet format (world_atlas.h) and rdpq/tiny3d alpha-to-tile; no counterpart in Quake/Xash/HLSDK.
|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-54|header comment, types, state|CLEAN|—|ours|
|55-67 be32/be16 helpers|CLEAN|—|false positive|
|69-379|Shutdown, Lookup, BindSheet, BindForMap|CLEAN|—|ours|
Quake bindings: COM_LoadHunkFile, Con_Printf/Con_DPrintf, MAX_QPATH, qboolean/byte. Hit 55-58 (XASH lib_apple.c:42-44): false positive (big-endian byte read). Lookup by Quake texture name (16 chars) is a data-key, not code.

##### n64/port/r64_world_tex.c (220 lines; git: 64e3131)
Verdict: CLEAN + QUAKE-INTERFACE. Loader for our .wci4 pack (world_tex_bake.h).
|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-220|everything|CLEAN|—|ours; be32/be16 helpers (34-40) false positive vs Xash lib_apple.c|
Quake bindings: COM_LoadHunkFile, Con_Printf/Con_DPrintf, MAX_QPATH, texture-name key; r64tex_t install via R64_TexInstallCI4.

##### n64/port/r64_main.c (215 lines; git: 8343727, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE. Origin: created in 8343727 as "R_*/D_* stubs with real R_SetVrect math" replacing r_main.c; Id header residue.
|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-24|header, includes|CLEAN|—|—|
|26-39|globals (refdef_t r_refdef; vec3 r_origin/vpn/vright/vup; reinit_surfcache; r_cache_thrash; r_notexture_buf)|QUAKE-INTERFACE|first/source r_main.c globals; render.h externs|names/types required by the Quake seam; static buffer replaces the hunk alloc|
|41-61 (45-54)|R_Init notexture setup|CONTAINS-QUAKE (renamed copy, 10 lines)|first/source r_main.c:R_InitTextures(150-165)|same 16x16 default, offsets[0..3] chain 16*16, 8*8, 4*4, 2*2, checker fill by pixel index; static buffer instead of Hunk_AllocName|
|55-61|R_InitParticles/R64_*Init calls, fov defaults|CLEAN|—|ours|
|63-85|R_InitTextures, R_InitEfrags, R_NewMap, R_LayerSwitch|QUAKE-INTERFACE|—|ours bodies|
|87-129|R_SetVrect|CONTAINS-QUAKE (exact copy, 43 lines including comment; comment says "Real implementation (from r_main.c)")|first/source r_main.c:R_SetVrect(302-337); evidence_k16 e 84-132 n=210|exact logic, constants and variable names (size/h/lineadj, 96 min, &~7, &~1, lcd_x halve)|
|131-134|R_ViewChanged|CLEAN|—|calls R_SetVrect only|
|136-170 (147-148)|R_RenderView|CLEAN; weak echo 147-148|first/source r_misc.c:R_SetupFrame(425-430)|vieworg→r_origin and AngleVectors(viewangles→vpn/vright/vup): standard Quake idiom; rest is our PZ_/R64_ sequencing|
|172-196|R_InitSky, efrag stubs|QUAKE-INTERFACE (empty bodies)|—|signatures are Quake's|
|198-215|D_EnableBackBufferAccess, D_DisableBackBufferAccess, D_FlushCaches, D_UpdateRects|QUAKE-INTERFACE (ours bodies)|d_iface.h|signatures only|
Quake bindings: refdef_t r_refdef (vrect/vieworg/viewangles/fov_x/fov_y), vrect_t, scr_viewsize, lcd_x, cl.intermission, texture_t, entity_t, efrag_t, R_*/D_* entry points, S_ExtraUpdate, R_InitParticles/R_ClearParticles/R_KillParticles (source/r_part.c), AngleVectors, vpn/vup/vright, r_origin.
Automated hits: 45-51 (QUAKE e/n r_main.c:156-161): REAL. 85-122 (r_main.c:302-337): REAL (R_SetVrect; the hit range is shifted because the detector matched the body). 155-158 (r_misc.c:429-430, `PZ_EXIT(PZ_RFBEGIN)`): false positive (our instrumentation macro; the PZ_ macros do not exist in id code; only the surrounding R_SetupFrame idiom at 147-148 counts).

##### n64/port/r64_vid.c (258 lines; git: 8343727, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE (minor). No Id header at any point (first commit already "2026 quakegeneric contributors"), though the file "replaces vid_null.c".
|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-58|header, globals, BASEWIDTH/BASEHEIGHT, BuildPalette|CLEAN (QUAKE-INTERFACE: viddef_t vid)|first/source vid_null.c:30-35|BASEWIDTH macro name/320 (BASEHEIGHT changed 200→240); palette→RGBA5551 build ours|
|59-150|VID_SetPalette, present pacing (pace_*), R64_PaceStats|CLEAN|—|ours (libdragon vblank gating)|
|151-157|VID_ShiftPalette|CLEAN|vid_null.c:VID_ShiftPalette|empty by design (body differs)|
|158-185 (162-172)|VID_Init|CONTAINS-QUAKE (11 lines, near-copy with altered values) + QUAKE-INTERFACE|first/source vid_null.c:VID_Init(48-62)|same field assignments (maxwarpwidth/width/conwidth chain, maxwarpheight/height/conheight, aspect, colormap=host_colormap, fullbright computed from colormap word 2048 with LittleLong); numpages, buffer, rowbytes differ; t3d_init/R64_PartInit/vblank hook/QG_Init ours|
|186-188|VID_Shutdown|QUAKE-INTERFACE (empty)|—|signature|
|189-237|R64_UiTlutBind, R64_Mode2D, R64_FrameBegin|CLEAN|—|ours|
|238-251|VID_Update|CLEAN|—|ours (present, stats)|
|252-258|D_BeginDirectRect, D_EndDirectRect|QUAKE-INTERFACE (empty)|vid_null.c|signatures|
Quake bindings: viddef_t vid (maxwarp*, width/height, conwidth/conheight, aspect, numpages, colormap, fullbright, buffer/conbuffer, rowbytes/conrowbytes), host_colormap, LittleLong, VID_* / D_*DirectRect entry points, QG_Init (quakegeneric.h).
Automated hit: 162-167/162-172 (QUAKE e/n vid_null.c:53-55): REAL (VID_Init).

##### n64/port/r64_draw.c (695 lines; git: 8343727, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE (small) + HLSDK constant. Origin: created in 8343727 as the rdpq replacement of draw.c; the original draw.c bodies were replaced by blits, but function list, several comments and skeletons persist.
|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-47|header, draw_disc, char_surf, convpic/cachepic tables|QUAKE-INTERFACE|first/source draw.c:cachepic_t/menu_cachepics/draw_disc|struct and names (cachepic_t, menu_cachepics, menu_numcachepics) from Quake; fields changed (name + qpic_t hdr)|
|48-245|MemStats, ui stub, reclaim, FillPic8, TryLoadUi8(.ui8 loader)|CLEAN|—|ours (offline CI8/I4 .ui8 format)|
|246-375|HUD sprite sheets: Draw_SubPicRelease/Color/SubPic|CLEAN; 296-297 HLSDK const|hlsdk cl_dll/hud.h:24 (RGB_YELLOWISH 0x00FFA000), :39 (MIN_ALPHA 100)|only the colour constant 255,160,0 and the ~100 alpha idea are taken; named in a comment|
|377-433|R64_ConvertPic8, SurfForPic|CLEAN|—|ours|
|436-446|Draw_Init|QUAKE-INTERFACE|first/source draw.c:Draw_Init(110-125)|W_GetLumpName("conchars"/"disc") is the API|
|457-461|R64_DrawCharsInvalidate|CLEAN|—|ours|
|463-490 (463-469 cmt, 472-478)|Draw_Character|CONTAINS-QUAKE (comment copy + prologue, 14 lines)|first/source draw.c:Draw_Character(128-160); comment_overlap.md 465-467|doc sentence "Draws one 8*8 graphics character with 0 being transparent" verbatim; num&=255, y<=-8 early-out, row=num>>4, col=num&15 identical; body (rdpq upload + texrect) ours|
|492-500|Draw_String|CONTAINS-QUAKE (exact copy, 9 lines)|first/source draw.c:Draw_String(231-240)|identical loop|
|502-504|Draw_DebugChar|QUAKE-INTERFACE (empty)|—|signature|
|506-519|Draw_Pic, Draw_TransPic|CLEAN|—|ours|
|521-545|Draw_TransPicTranslate|CONTAINS-QUAKE (comment phrase only, 6 lines)|quake/WinQuake/gl_draw.c:653; comment_overlap.md 522-524|"only used for the player color selection menu" phrase; body ours|
|547-559|Draw_ConsoleBackground|CLEAN|—|ours|
|561-571|Draw_TileClear|CONTAINS-QUAKE (comment, 7 lines)|first/source draw.c:Draw_TileClear(731-740); comment_overlap.md 562-565|comment sentence "repeats a 64*64 tile graphic to fill the screen around a sized down refresh window" copied; body ours|
|573-601|Draw_Fill, Draw_FadeScreen, Draw_HLFade|CLEAN|—|ours; Draw_HLFade cites GoldSrc env_fade (behaviour)|
|603-609|Draw_BeginDisc, Draw_EndDisc|QUAKE-INTERFACE (empty)|—|signature|
|611-695 (621-632, 674-676)|Draw_PicFromWad, Draw_CachePic|CONTAINS-QUAKE (skeleton, 12 lines of 80)|first/source draw.c:Draw_CachePic(64-105); quake gl_draw.c:Draw_CachePic(232-262)|name lookup loop with strcmp over menu_cachepics, "menu_numcachepics == MAX_CACHED_PICS" Sys_Error text, strcpy of name, COM_LoadTempFile + "failed to load" error + SwapPic; cache/convert logic and .ui8 path ours|
Quake bindings: qpic_t, W_GetLumpName, SwapPic, COM_LoadTempFile, Sys_Error, Con_Printf/Con_DPrintf, vid.width/height, MAX_QPATH, the whole Draw_* seam (draw.h).
Automated hits: 301-304 / 301-309 (QUAKE quake2 gl_light.c:595-598): false positive (generic 0..255 clamp in Draw_SubPicColor). 358-360 (LIBDRAGON rdpq_font.c:49-50): libdragon API call, not in scope. 672-675 (quake gl_draw.c:251-252): REAL, trivially (COM_LoadTempFile + cast). Comment overlaps 465-467, 522-524, 562-565 confirmed (see rows).

##### n64/port/r64_part.c (185 lines; git: 1ccc03a, modified in worktree)
Verdict: QUAKE-INTERFACE (content CLEAN). 1ccc03a began as a tiny3d quad billboard writer with the same hook layout; c3cc968 moved to TinyPX. Id header is residue.
|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-25|header, includes|CLEAN|—|—|
|26-27|xscaleshrink/yscaleshrink|QUAKE-INTERFACE|first/source r_main.c:73|Quake globals defined so r_part.c links; values unused|
|29-78|PART_* constants, buffers, R64_PartPrealloc/PartInit/MemStats|CLEAN|—|ours (libdragon/TPX)|
|80-92|D_StartParticles|QUAKE-INTERFACE|first/source d_part.c:D_StartParticles|hook name only|
|94-137 (108-110 weak)|D_DrawParticle|CLEAN; weak echo 108-110|first/source d_part.c:D_DrawParticle(55-75)|camera-relative vector + near-clip: Quake computes three axis dots; ours one dot against vpn and clips at 4 (Quake PARTICLE_Z_CLIP); rest (s16 pack, palette colour) ours|
|139-185|D_EndParticles|CLEAN|—|ours (TPX state)|
Quake bindings: particle_t (org, color), D_StartParticles/D_DrawParticle/D_EndParticles (d_iface.h), r_origin, vpn, R_DrawParticles (source/r_part.c sim, other slice), r64_pal32 (our palette from VID_SetPalette), Sys_Error.
Automated: none for this file.

##### n64/port/r64_local.h (371 lines; git: 8343727, modified in worktree)
Verdict: CLEAN, QUAKE-INTERFACE. Declarations only; 8343727 onwards "2026 quakegeneric contributors" header.
|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-371|palette externs, frame bracket, profiler enums, r64tex_t, atlas/studio/world/light/trans/decal/sector prototypes|CLEAN|—|ours; profiler stage list is OpenLara-style (comment: concept only)|
Quake bindings: `#include "quakedef.h"`; types entity_t, model_t, mleaf_t, msurface_t, texture_t, mdl_t, qboolean, byte, vec3_t; MAXLIGHTMAPS and MAX_DLIGHTS array dimensions (r64_lmsurf_t/r64_dlsurf_t); `extern int d_lightstylevalue[256]` (name from Quake render/d_iface); R_PushDlights comment. Prototypes of the Quake-derived helpers (R64_TextureAnimation, R64_SampleDlights, R64_DlightSurf*, R64_LightmapSurf*) are interfaces only; implementations are in the CONTAINS-QUAKE files above.
Automated: none.

##### n64/port/frame_stats.c (184 lines; git: 8343727, modified in worktree)
Verdict: CLEAN, QUAKE-INTERFACE (trivial). Telemetry/SELFTEST hook; no Quake counterpart.
|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-184|R64_FrameStats|CLEAN|—|ours (fps/FPU-trap print, debug capture markers, TH_Screenshot, TH_Exit)|
Quake bindings: r_refdef.vieworg/viewangles reads (14 uses), `quakedef.h` via r64_local.h. Automated: none.


### A.AuditRendB — Renderer B (alias, decal, trans, studio)


Method: read every line of all 10 files (working tree), compared against Quake (first/source, quake/WinQuake), Xash (ref/gl, ref/soft), HLSDK (cl_dll, utils/mdlviewer, utils/qcsg), automated evidence.md / evidence_k16.md / comment_refs.txt, and `git log --follow` (tracked files). comment_overlap.md has no entries for this slice. No code from any source is reproduced below.

Conventions: "Quake binding" lists = types/APIs of the old engine the file depends on (re-target list). Line counts of contamination are line ranges of the working-tree file.

#### 1. Summary


Totals: QUAKE 143, XASH 254, HLSDK 24 (small idioms).

Header note: r64_alias.c carries "Copyright (C) 1996-1997 Id Software, Inc." + GPL (git e749e31: header added at creation; the first revision already contained RecursiveLightPoint written with node->plane/children — a direct Quake copy — and a comment "Frame group handling from R_AliasSetupFrame"). r64_decal.c and r64_trans.c carry GPL header "quakegeneric contributors" (untracked). r64_alias_bake/cart/studio*/sys.h carry no licence header.

---

#### 2. Per-file sections

##### n64/port/r64_alias.c (1461 lines; git: e749e31 2026-07-11, 40 later commits, modified in worktree)
Verdict: QUAKE-INTERFACE + CONTAINS-QUAKE. Not XASH, not HLSDK, not QCTRANS.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|33-37|r_avertexnormals[162][3] via `#include "anorms.h"`|CONTAINS-QUAKE (data)|source/anorms.h (id header; md5 06c20258… identical to quake/WinQuake/anorms.h; used by Quake r_alias/r_part/gl_rmain)|exact data include. The 162-normal table is id's. Non-static because Quake r_part.c R_EntityParticles uses it.|
|39-131|POS_SCALE, arena, mesh cache (AliasMeshCache*), AliasEnsureVCache|CLEAN/QUAKE-INTERFACE|—|ours (Tiny3D arena design).|
|132-184|AliasSetFrustum, R64_CullSphere|CLEAN (QUAKE-INTERFACE: r_refdef, vpn/vup/vright/r_origin)|compared with Quake R_SetFrustum (r_main.c, turn-vector construction) — different formulation (sin/cos half-angle planes)|independent. R64_CullSphere is reused by studio/trans/decal.|
|186-197|AliasRadius|CLEAN|model->radius (Quake model_t field)|ours.|
|199-309|AliasSTCache (per-model ST tables)|CLEAN|—|ours; uses AMB2 bake.|
|311-346|budgets/consts|CLEAN|—|ours.|
|348-356|r_lerpmodels cvar, aliaspose_t|QUAKE-INTERFACE|—|cvar_t binding.|
|366-393|R64_AliasPrealloc|CLEAN|—|ours.|
|461-479|AliasSubmitPacked|CLEAN|—|T3D submit.|
|481-496|R64_AliasFrameBegin|CLEAN|—|ours.|
|507-572|RecursiveLightPoint (#if Q64_WORLD_V1)|CONTAINS-QUAKE|first/source/r_light.c:RecursiveLightPoint (141-~213; same body in WinQuake)|renamed/accessor-ised copy: identical node-contents test, plane distance front/back, side, frac midpoint, descend front, impact test per surface (SURF_DRAWTILED skip, texinfo s/t from vecs, texturemins/extents rejection, !samples → 0), then back side. Only substitutions: Mod_NodePlane/Mod_NodeChild/Mod_Surface/Mod_SurfaceTexinfo accessors and the sample sum replaced by R64_SampleLightmap(ds>>4, dt>>4). git: e749e31 first version was the raw Quake text (node->children[side] etc.).|
|574-589|alias_light_cache|CLEAN|—|ours (16u-quantised ring + epoch).|
|591-663|R64_LightPoint|CONTAINS-QUAKE (7 lines: 612, 640-645) / rest CLEAN|first/source/r_light.c:R_LightPoint (~222-244)|612 (no lightdata → 255), 640-645 (end = p with z-2048, RecursiveLightPoint from nodes, -1→0) follow R_LightPoint structurally. The W64 probe branch (600-608) and cache (614-634, 652-660) are ours.|
|665-699|PoseVerts|CONTAINS-QUAKE|first/source/r_alias.c:R_AliasSetupFrame (648-693; also gl_rmain.c GL_GetAliasFrame/R_SetupAliasFrame 150-176)|renamed copy of the ALIAS_GROUP branch: frame-range fallback, ALIAS_SINGLE direct return, group intervals/fullinterval, time = cl.time + syncbase, targettime by int-division wrap, first interval > targettime. Same locals (fullinterval/targettime/time). Commit e749e31 comment: "Frame group handling from R_AliasSetupFrame".|
|701-746|UpdateAliasLerp|CLEAN (QUAKE-INTERFACE: entity_t prevframe/lastframe/lerpmodel/frame_start, cls.timedemo, cl.time)|—|ours (Quake has no model lerp; QuakeSpasm-like idea only). Not compared with QuakeSpasm (not in reference trees).|
|748-828|SetupFrames|CONTAINS-QUAKE (787-797 only) / rest CLEAN|r_alias.c:R_AliasSetupFrame group branch|787-797 same group-interval search as PoseVerts. 798-827 (sub-lerp between interval poses) ours.|
|830-859|SetupSkin|CLEAN (QUAKE-INTERFACE: maliasskindesc_t/maliasskingroup_t/ALIAS_SKIN_GROUP)|r_alias.c:R_AliasSetupSkin (560-600)|skinnum range fallback + skin-group pointer chase is the MDL format semantics; no animated skin selection, different structure. False positive for copying.|
|861-893|AliasEntityMatrix|CLEAN|comment names Quake R_AliasSetUpTransform|behaviour match only (negated pitch, X forward, Y −right, Z up) written against Tiny3D matrix layout.|
|895-1144|DrawAliasEntity|CLEAN/QUAKE-INTERFACE|Quake R_AliasDrawModel / GL_DrawAliasFrame / shadedots|own two-pass unique-vert pack, own shading (light*(0.8+0.45*normal·fixed dir), uses only the anorms table), own ST packing from stverts/onseam (format semantics: onseam = skinwidth/2 shift).|
|1146-1302|aliascand_t, SortAliasCands, R64_DrawAliasEntities|CLEAN/QUAKE-INTERFACE|—|ours (budgeting, nearest-first). Uses cl_visedicts, cl_static_entities, EF_GIB flags.|
|1304-1413|DrawViewWeaponT3D, R64_DrawViewWeapon|CLEAN (comment cites Xash behaviour)|comments cite Xash R_DrawViewModel (zNear 4, glDepthRange 0..0.3, glCullFace GL_FRONT)|behaviour facts only; code is T3D/rdpq. STAT_HEALTH/chase_active conditions follow the shape of Quake R_DrawViewModel (r_main.c:606-627) trivially (1 condition line, not flagged).|
|1415-1461|R64_Alias_MemStats/ResetPeaks|CLEAN|—|ours.|

Quake bindings: entity_t (model, frame, prevframe/lastframe/lerpmodel/frame_start [our extension fields], skinnum, syncbase, origin, angles), cl.viewent/cl.time/cl.stats[STAT_HEALTH]/cl.worldmodel/cl.num_statics, cls.timedemo, cl_visedicts/cl_numvisedicts/cl_static_entities, MAX_VISEDICTS, EF_GIB/EF_ZOMGIB; model_t (type, flags, radius, name), mod_alias/mod_studio, Mod_Extradata, Mod_NodePlane, Mod_NodeChild, Mod_Surface, Mod_SurfaceTexinfo, Mod_SurfaceSamples, Mod_IsW64, Mod_NoV1World, mnode_t/mplane_t/msurface_t/mtexinfo_t, SURF_DRAWTILED; aliashdr_t, mdl_t, stvert_t, mtriangle_t, trivertx_t, maliasgroup_t, maliasskindesc_t, maliasskingroup_t, ALIAS_SINGLE/ALIAS_SKIN_GROUP/ALIAS_ONSEAM; r_refdef (fov_x/fov_y/vieworg), r_origin, vpn/vup/vright, AngleVectors, DotProduct/VectorSubtract; cvar_t r_lerpmodels + Cvar_RegisterVariable, chase_active; Con_Printf/Con_DPrintf/Sys_Error; hunk-free (uses malloc).

Automated hit dispositions:
- QUAKE e/n 508-518, 523-529, 529-537, 537-543, 553-564 (r_light.c) — REAL (RecursiveLightPoint copy).
- HLSDK n 530-537 (qrad/trace.c), HLSDK e 530-535, XASH n 533-535 (gl_decals.c:314), XASH n 553-564 (sv_world.c:1533) — same Quake RecursiveLightPoint/mid-point lerp text found in Quake-heritage code; not separate sources (attribute to Quake).
- QUAKE n 654-659 (r_draw.c:856-858) — false positive (ring-index idiom `i = clock++ % N`).
- HLSDK/XASH n 932-936 (mdlviewer/studio_render.cpp, ref/soft/r_studio.c) — false positive: header-offset cast for the MDL format, identical to Quake alias header access (format-defined).
- k16 additions (r_light.c:141-149/162-165/168-173/176-178) — same real region.

git history: header present from e749e31; no other file origin. The file never contained a verbatim copy of R_AliasDrawModel/gl_mesh (draw is Tiny3D from scratch).

##### n64/port/r64_alias_bake.c (380 lines; git: be67cb5 2026-07-14, + 3 commits; modified in worktree)
Verdict: QUAKE-INTERFACE (thin); no contamination.

|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-75|rt structs, cache grow|CLEAN|—|ours.|
|77-89|be32u/be16u/be16i|CLEAN|xash lib_apple.c:42 (hit)|false positive: generic big-endian read.|
|91-166|AmbInstallSkins, AmbBuildScaledST|CLEAN|—|ours; AMB2 format is ours (source/alias_bake.h).|
|168-197|release/reinstall/invalidate|CLEAN|—|ours.|
|199-311|AmbFind (path .mdl→.amb2, header parse, validation)|CLEAN/QUAKE-INTERFACE|—|ours; uses COM_LoadHunkFile, Con_*.|
|313-380|R64_AliasBakeSkin/ST/GetScaledST/Has|CLEAN/QUAKE-INTERFACE|—|ours.|

Quake bindings: model_t (name), mdl_t (numverts), MAX_QPATH, COM_LoadHunkFile (hunk file), Con_Printf/Con_DPrintf, qboolean/byte; r64tex_t/R64_TexInstallCI4 (ours). Hit XASH e/n 79-82: false positive (see above).

##### n64/port/r64_decal.c (1547 lines; git: untracked — no history)
Verdict: QUAKE-INTERFACE + CONTAINS-XASH + CONTAINS-QUAKE(qbsp table lineage). Not QCtrans.
Relationship: header comment says placement follows gles/glr_decal.c ("Xash3D gl_decals.c behaviour"); glr_decal.c (other slice) has the same R_DecalAlloc / R_DecalIntersect functions (lines 242-, 449-511), i.e. the Xash translation was carried over.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|87-211|constants, dtex_t/decal_t/cand_t/dpool_t, cvars, Be32|CLEAN (values: DECAL_DISTANCE 4, MAX_OVERLAP_DECALS 6 and "60%" equal Xash gl_decals.c:19,25,490)|xash ref/gl/gl_decals.c:19,25|numeric GoldSrc behaviour constants (short); struct layout, pool, block arena are ours.|
|213-335|LoadDirectory, PoolGet, R_DecalClear, TexFind|CLEAN/QUAKE-INTERFACE|—|ours (decals.i4 format, COM_OpenFile).|
|337-386|SpDone, TexLoad (LRU budget)|CLEAN|—|ours.|
|388-401|DecalFree|CLEAN|xash R_DecalUnlink(64-94)|different data model (blocks, syncpoints).|
|403-421|DecalAlloc|CONTAINS-XASH|xash ref/gl/gl_decals.c:R_DecalAlloc (96-128)|structural translation, comment names R_DecalAlloc: reuse pick else do-while advance round-robin over the pool skipping FDECAL_PERMANENT entries, bounded by a count against an infinite loop, then free/unlink the chosen slot. Same shape (count guard, wrap at limit).|
|423-465|BlockAlloc|CLEAN|—|ours.|
|467-590|MoverSub, BoxTouch, VPos/VSt/VLight, Collect (face polygons from cooked REND/MOVR chunks, Newell normal, plane-distance test)|CLEAN/QUAKE-INTERFACE (W64 API)|xash R_DecalNode(704-731) compared|per-face plane distance test within DECAL_DISTANCE is the only shared concept; traversal is meshlet/strip based, not BSP-node recursion.|
|592-619|BaseAxes (6x3x3 table + best-dot pick)|CONTAINS-QUAKE|HLSDK utils/qcsg/map.c:23-56 baseaxis/TextureAxisFromPlane (Quake qbsp heritage; same table and loop shape); quake2 ref_soft/r_rast.c:86-88 partial row hit|table row-for-row identical (floor, ceiling, west, east, south, north) with same normal-dot argmax; comment says "Quake's texture axes of a plane (TextureAxisFromPlane)". HLSDK-licensed copy exists but the code is Quake heritage.|
|621-665|TexAxes (axes from texel coordinates)|CLEAN|—|ours (solves 2x2 for the s/t gradients over the first proper triangle); falls back to BaseAxes.|
|667-692|cv_t, ClipEdge (generic Sutherland-Hodgman, axis/sign/limit)|CLEAN|xash SHClip/R_ClipInside/R_ClipIntersect (264-383)|textbook algorithm, different structure (one generic function over axis+sign, 6-float verts, light interpolated) vs Xash's per-edge switch. Only coincidence: edge order L,R,T,B (natural). Not flagged.|
|694-699|Pack8|CLEAN|—|ours.|
|701-764|Intersect|CONTAINS-XASH|xash ref/gl/gl_decals.c:R_DecalIntersect (430-505)|structural translation (comment names R_DecalIntersect): decal extents = basis·(world size/2) about the shot; for each existing non-permanent decal project min/max extents into that decal's [0,1] space (+0.5 centring, minus its offset dx/dy); clamp to [0,1]; intersection test `min<1 && max>0`; area=(umax-umin)*(vmax-vmin); count those with area>0.6; remember the one with smallest area (`!last || area<=lastarea`, lastarea initialised 2); return it only if count>=MAX_OVERLAP_DECALS. Differences: fixed-point axis storage, entnum/rom filters, DECAL_DISTANCE plane test.|
|766-1009|Place|CLEAN (944-945: CONTAINS-XASH)|xash R_DecalCreate (572-608) / R_DecalShoot (734-820)|943-945: Alloc(Intersect(...)) pattern = R_DecalCreate's alloc(intersect) with count threshold (2 lines). Everything else (candidate collection over resident sectors, layer seam handling, clipping, packing T3DVertPacked, light interpolation) ours.|
|1011-1106|SamePos, PlaceTimed, R_DecalShoot (API), Retry (pending queue)|CLEAN|—|ours (R_DecalShoot name/flags from gsfx contract).|
|1108-1442|dlayer_t, NewSpace/WorldSpace/MoverSpace/SpaceMatrix, Submit, R64_DecalsDraw, R64_DecalsDrawEntity|CLEAN|xash DrawSurfaceDecals/DrawDecalsBatch compared|ours (RDP I4*PRIM*SHADE, depth-offset, rspq syncpoints, batching).|
|1444-1518|R_Decal_f, R_DecalStats_f, R64_DecalInit|QUAKE-INTERFACE|—|dev commands; SV_Move/sv_player/NUM_FOR_EDICT/MOVE_NOMONSTERS/trace_t, Cmd_*, Cvar_*.|
|1520-1547|stub path (!R64_WORLD_V2)|CLEAN|—|—|

Quake bindings: entity_t / cl_entities[] / cl_visedicts / cl_numvisedicts, cl.time, MAX_EDICTS, model_t + mod_brush, Mod_IsW64 (our), r_refdef.vieworg/viewangles, r_origin, AngleVectors, cvar_t + Cvar_RegisterVariable, Cmd_AddCommand/Cmd_Argc/Cmd_Argv, COM_OpenFile/COM_CloseFile/Sys_FileRead/Sys_FileSeek, Q_strcasecmp/Q_atoi/Q_atof, Con_Printf/Con_DPrintf, sv.active/sv_player/SV_Move/MOVE_NOMONSTERS/trace_t/NUM_FOR_EDICT (dev command only, 1459-1490), SV_LayerAt (our), edict link (tr.ent).

Automated hit dispositions: XASH e 210 (lib_apple.c:42) FP (Be32 helper); QUAKE e 601-602 (quake2 r_rast.c:86-88) and HLSDK e 597-602 (qcsg/map.c) — REAL partial hits of the baseaxis table (see BaseAxes). The K16/xash comparison found no other token overlap; Intersect/DecalAlloc were found by structural review, not token scan (renamed identifiers).

##### n64/port/r64_trans.c (950 lines; git: untracked — no history)
Verdict: QUAKE-INTERFACE + CONTAINS-XASH.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|15-60|header, constants|CLEAN|comments cite Xash R_DrawEntitiesOnList / CL_FxBlend / ref/gl|descriptive citations.|
|60-73|R64_EntTranslucent|CLEAN|—|ours (kRender* from HLSDK const.h via gsfx.h).|
|77-85|FxRand (xorshift)|CLEAN|—|ours.|
|87-150|FxBlend|CONTAINS-XASH|xash ref/gl/gl_rmain.c:CL_FxBlend (1181-1298); ref/soft/r_main.c:~1400|structural translation (comment: "engine CL_FxBlend (Xash3D R_ComputeFxBlend)"): offset = entity index*363 de-sync, same switch cases and same constants (pulse 0x40/0x10 with sin(t*2|8+offset), strobe 4/16/36, flicker 2/17 and 16/23, hologram/distort: view-axis distance, 180 amt, 100/400 fade, ±random), the Xash idiom "20*sin(..)<0 → 0 else renderamt". Omitted: fade/solid-state cases (documented), clamp at the end. Not in HLSDK/Quake (GoldSrc engine code).|
|152-200|ModeBegin/ModeEnd (RDP blenders: MULTIPLY / additive / PRIM combiners)|CLEAN|xash R_SetRenderMode compared|ours, RDP specific; semantics (TransColor flat colour, TransAdd/Glow additive cap) only.|
|202-545|sprite cache (.sc4 format, atlas, PI DMA, LRU/budget), SpriteTex/Parse/Fail/Landed/Path/Request/SpriteCache, R_SpritePrefetch|CLEAN|—|ours (tools/hl_sprite_cook.py format).|
|547-590|titem_t, SetVert|CLEAN|—|ours.|
|592-653|SpriteQuad (orientation by sprite type: oriented, facing-upright, VP-parallel-upright with 0.999848 guard, parallel-oriented rotation by roll, default view axes; origin −0.01*forward for oriented; auto roll promotion)|CONTAINS-XASH (592-644; 645-652 vertex emission ours)|xash ref/gl/gl_sprite.c:R_DrawSpriteModel (466-518); also Quake first/source/r_sprite.c:R_DrawSprite (285-380) and quake WinQuake/gl_rmain.c|same case set, same per-case vector construction, same 0.999848 (cos 1°) invisibility test; `angles[ROLL]!=0 && PARALLEL → ORIENTED` and the 0.01 z-fight offset exist in Xash only (not in Quake); sprite type names are Quake's SPR_VP_* (spritegn.h). Quake lineage for the algorithm; attribute to Xash because of the Xash-only extras.|
|655-698|SpriteItem (kind selection cutout/blend/screen, PRIM tint)|CLEAN|xash R_DrawSpriteModel alpha/render mode logic compared|ours (RDP-specific; follows GoldSrc semantics: index-alpha always blends).|
|700-764|SameSpriteState, SpriteDraw, SpriteModeBegin, CutoutOrder|CLEAN|—|ours.|
|766-950|R64_DrawTranslucent pass, R_SprStats_f, R64_TransInit, DrawTranslucent|CLEAN/QUAKE-INTERFACE|xash R_DrawEntitiesOnList translucent sort compared|ours (cull sphere, back-to-front insertion sort, cutouts first).|

Not present: Xash glow-sprite occlusion (R_SpriteGlowBlend/R_SpriteOccluded) and flashlight — kRenderGlow is drawn as additive. No match found for these.

Quake bindings: entity_t (model, rendermode/renderamt/renderfx/rendercolor/renderscale — our U_HLRENDER fields, frame, origin, angles), cl_entities, cl_visedicts/cl_numvisedicts, cl.viewent/cl.time, MAX_EDICTS, model_t (type, mins/maxs, radius, name), mod_sprite/mod_brush/mod_studio, r_refdef.vieworg, r_origin, vpn/vup/vright, AngleVectors/VectorNormalize/VectorMA, cvar_t/Cvar_RegisterVariable/Cmd_AddCommand, COM_OpenFile/COM_CloseFile/Sys_FileRead/COM_FileRom, MAX_QPATH, Con_Printf; spr type constants SPR_VP_PARALLEL*/SPR_ORIENTED/SPR_FACING_UPRIGHT (Quake spritegn.h).

Automated hits: none for this file in evidence.md/k16. Both CONTAINS-XASH regions were found by structural review.

##### n64/port/r64_studio.c (899 lines; git: dcce7e3 2026-07-14, + 10 commits, modified in worktree)
Verdict: QUAKE-INTERFACE; HLSDK idiom (small). Bone setup / blending / chrome / lighting math is delegated to studio_geom.c (other slice) — nothing of it here.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|20-138|arena, mats, static-pose cache|CLEAN|—|ours.|
|140-158|StudioSkin|CLEAN/format|—|mstudiotexture_t index/palette layout is the IDST format (HLSDK studio.h).|
|160-188|StudioEntityMatrix|CLEAN|comment: "Soft entity_xform"|same axis convention as r64_alias AliasEntityMatrix; ours.|
|190-262|prealloc, EmitVert|CLEAN|—|ours.|
|264-365|StudioDrawVerts/Group/SubmitStream|CLEAN|—|ours (bump arena).|
|367-532|StudioStaticBuild/Draw/Find/Get, rspq blocks|CLEAN|—|ours.|
|534-679|R64_DrawStudioEntity|CLEAN; 599-609 HLSDK (idiom)|HLSDK cl_dll/StudioModelRenderer.cpp:StudioEstimateFrame (711-760); Xash gl_studio.c R_StudioEstimateFrame|599-609: LOOPING wraps by integer-quotient subtraction over numframes−1 with negative fix-up; non-looping clamps. Same idiom/constant structure but the clock (free-run time*fps, viewent nonce restart) is ours. Low risk; HLSDK licence allows.|
|681-899|StudioCands/Temp entry/DrawTemps/R64_DrawStudioEntities|CLEAN|—|ours (budgeted candidates, nearest-first).|

Quake bindings: entity_t (model, skinnum as sequence, body, framef, frame, origin, angles), cl.viewent, cl_visedicts/cl_numvisedicts, cl_temp_entities/MAX_TEMP_ENTITIES, MAX_VISEDICTS, model_t (type, radius, name), mod_studio, Mod_StudioHeader/Mod_StudioAnimData (model.c/mod_studio.c), r_refdef.vieworg, r_origin, vpn, AngleVectors, Con_DPrintf, Sys_Error; studiohdr_t/mstudioseqdesc_t/mstudiotexture_t (studio.h, HLSDK layout).
Automated hit: QUAKE e 674 (cl_main.c:263) — false positive (debugf format string).

##### n64/port/r64_studio_bake.c (2142 lines; git: bcc05dc 2026-07-14, + 9 commits, modified in worktree)
Verdict: QUAKE-INTERFACE; CONTAINS-XASH (chrome basis); HLSDK idiom (small).

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|33-210|constants, bake_skin_t/bake_rt_t, vm structures|CLEAN|—|ours (STB2 format).|
|210-290|FrameBegin, sk_mats_alloc, be16/be32, Prealloc|CLEAN|—|ours.|
|290-706|sk_free, async STB2 load, bake_install/layout checks, bake_request/load|CLEAN/QUAKE-INTERFACE|—|ours (Mod_StudioRes*, COM_FileRom, host_framecount).|
|709-826|Preload/Request/Release/Reinstall/Invalidate/Unload|CLEAN|—|ours.|
|828-879|chrome_axes|CONTAINS-XASH|xash ref/gl/gl_studio.c:R_StudioSetupChrome (1116-1153) (+ref/soft/r_studio.c:1165); same algorithm in HLSDK utils/mdlviewer/studio_render.cpp:StudioModel::Chrome (441-474)|structural translation (comment cites Xash R_StudioSetupChrome): vector from viewer to bone (origin offset dropped: entity-relative), normalise, up = tmp × view-right, normalise, right = up × tmp (Xash order; HLSDK mdlviewer uses tmp × up, i.e. opposite sign), rotate both into the bone frame (transpose-rotate), later chrome ST = (n·axis+1)*32 (Xash/HLSDK constants, in vm_skin_pose 1224-1225). Added degenerate-length guards are ours. Token hits at 854-857/870-873 (studio_util.cpp:76, mathlib.c:365 CrossProduct) are the cross-product expansions inside this block.|
|881-935|bake_entity_matrix, sk_bone_matrix|CLEAN|Soft entity_xform comment|ours.|
|937-995|pack_vert, shade_rgba, vm_i16|CLEAN|—|ours.|
|997-1249|vm_skin_build/prefill/pose (CPU skin tables, CSR slot lists, changed-bone rewrite)|CLEAN (1224-1225 ST formula is the chrome constants)|—|ours; positions = matrix·(bone-local·scale), not HLSDK StudioModelRenderer code.|
|1256-1293|sk_neutral_controllers|CLEAN (format-derived)|HLSDK utils/mdlviewer/studio_render.cpp:SetController (321-)|computes the byte that decodes to 0°, i.e. the inverse of the bone-controller decode. Non-loop branch equals the SetController formula at value 0 (one expression); RLOOP branch uses the 360/256 rotational mapping. Format semantics; not flagged.|
|1295-1476|sk_alloc, sk_step (meshlet pack + rspq block record)|CLEAN|—|ours.|
|1478-1665|sk_pose, sk_hold, sk_draw|CLEAN|—|ours; uses StudioGeom_ClockFrame/Blend/SetupBones (studio_geom.c, other slice) — bone setup, slerp, blending live there.|
|1667-1991|draw_meshlet_from, vm_draw_cached, vm_layout_tex, vm_layout|CLEAN|—|ours (greedy ≤64-vert meshlets).|
|1993-2092|vm_draw|CLEAN; 2036-2048 HLSDK (idiom)|HLSDK cl_dll/StudioModelRenderer.cpp:StudioEstimateFrame (743-760)|same looping wrap/clamp idiom as r64_studio.c 599-609.|
|2094-2142|R64_DrawStudioBake|CLEAN|—|ours.|

Quake bindings: entity_t (model, skinnum, framef, frame, origin, angles), cl_entities, cl.viewent, MAX_EDICTS, model_t (name, type), mod_studio, Mod_StudioHeader/Mod_StudioAnim*/Mod_StudioRes*/Mod_StudioLoadBake/Mod_StudioUnload/Mod_StudioResTouch, host_framecount, COM_FileRom, Con_Printf/Con_DPrintf/Sys_Printf, r_origin, AngleVectors, vright array (studio_draw_params_t).
Automated hits: LIBDRAGON n 514-524 FP; HLSDK/QUAKE n 854-857 & 870-873 (see chrome_axes: cross products, generic math); HLSDK n 1205-1214 (utils/light/light.c:75-79) FP (similar posB assignments); QUAKE e 2136-2137 FP (debug print).

##### n64/port/r64_studio_stubs.c (122 lines; git: 0fe9d97 2026-07-14, + 5 commits, modified in worktree)
Verdict: QUAKE-INTERFACE (trivial). All functions are empty/constant stubs (QUAKE_NO_STUDIO builds). Bindings: model_t, entity_t, cl.time, Mod_StudioUnload (called from Mod_ClearAll). No contamination.

##### n64/port/r64_studio_sw.c (449 lines; git: b6ea17c 2026-07-14, 2 later commits, clean in worktree)
Verdict: QUAKE-INTERFACE.

|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-135|cvars, SoftFB setup, RGBA5551 pack/blit|CLEAN|—|ours.|
|136-182|StudioSW_FillOpts|CLEAN (QUAKE-INTERFACE: r_refdef, r_origin, vpn/vup/vright, entity_t)|—|ours; constants (fov fallback 73.7, zNear 4) are project facts.|
|184-219|R64_StudioCompareAnimTime, StudioSW_ViewFrame|CLEAN|HLSDK StudioEstimateFrame compared|loop wrap by repeated subtraction — trivial, not flagged.|
|221-294|StudioSW_Render, Draw/Enabled|CLEAN|—|ours; calls StudioGeom_Emit / SoftStudio_DrawStream (other slices).|
|296-449|compare path select, rgb555 compare, FB readback|CLEAN|—|ours.|

Quake bindings: cvar_t r_studiosw/r_studiocompare + Cvar_RegisterVariable, vid.width/height, r_refdef.fov_y, r_origin, vpn/vup/vright, entity_t, cl.time/cl.viewent, model_t, Mod_StudioHeader, Con_Printf. Hit LIBDRAGON n 170-181: false positive (cam_origin assignment).

##### n64/port/r64_sys.h (39 lines; git: untracked)
Verdict: CLEAN. Declarations of instrumentation functions only (R64_MallocStats, R64_Probe*, R64_Hunk*); no Quake types, no licence header, no Quake code. The names "Hunk_AllocName" appear only in comments (the implementing r64_hunk.c / r64_malloc.c are other slices).

##### n64/port/r64_cart.c (94 lines; git: 828c982 2026-07-14, modified in worktree)
Verdict: CLEAN (QUAKE-INTERFACE only through `#include "r64_local.h"`, `qboolean`).

|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|1-21|header|—|OpenLara room_stream.cpp / level_prefetch.cpp (named)|states the approach (PI DMA, address precondition checked, invalidate before DMA) was proven in OpenLara; no code of OpenLara reproduced was verified here (not in reference trees).|
|26-58|CartDfsPath, R64_CartRomAddr|CLEAN|—|libdragon dfs_rom_addr/dfs_rom_size; comment mentions "Quake hands us rom:/id1/…" (path convention only).|
|60-94|R64_CartRead|CLEAN|—|libdragon dma_read/data_cache_*; ours.|

Bindings: qboolean, `rom:/` path convention (COM_* callers), R64_IoNoteDma (ours). No automated hits.

---

#### 3. Re-target / rewrite list (for import)

1. r64_alias.c — rewrite: RecursiveLightPoint (508-572), R_LightPoint skeleton (612, 640-645), PoseVerts / group-pose search (673-699, 787-797), anorms table (33-37 + source/anorms.h; replace with own normal table, 162-entry id table is Quake data). Remove Id header.
2. r64_decal.c — rewrite DecalAlloc (403-421), Intersect (701-764), the alloc(intersect) pairing (944-945), and BaseAxes (592-619); keep constants as plain behaviour parameters.
3. r64_trans.c — rewrite FxBlend (87-150) from the documented renderfx semantics, and SpriteQuad orientation (592-644) as own vector construction.
4. r64_studio_bake.c — rewrite chrome_axes (828-879) (either own derivation or HLSDK mdlviewer Chrome under HLSDK licence with its sign convention); frame-wrap idiom 2036-2048 and r64_studio.c 599-609 can move to game/ (HLSDK) or be rewritten.
5. Interface-only files (alias_bake, stubs, studio_sw, studio.c, remainder of the above): re-target the listed Quake bindings to hl64 equivalents.


### A.AuditGles — GLES renderer


Scope: 19 files, working tree as of the audit, **all untracked** (`git status`: `?? n64/port/gles/`; no history). None carries an Id Software header or any GPL header (no licence header at all) — contrast with the repo's `quakegeneric contributors` GPL headers: relicensing is a non-issue for the file texts, but see per-file contamination. `comment_overlap.md` has **no** hits for this slice (the cites below are explicit "Xash …" comments, not copied comment text).

Method: read every line of every file; opened the counterpart for every algorithmic block (Xash `ref/gl/gl_beams.c, gl_decals.c, gl_sprite.c, gl_rmain.c, gl_rsurf.c, gl_rpart.c, gl_studio.c, gl_triapi.c`; GLQuake `WinQuake/gl_draw.c, gl_rmain.c, gl_mesh.c`; first-commit `r_main.c, r_light.c, r_surf.c, r_alias.c, r_sprite.c, draw.c, vid_null.c`; HLSDK `cl_dll/StudioModelRenderer.cpp, utils/mdlviewer/studio_render.cpp, utils/qcsg/map.c, dlls/animation.cpp`); dispositioned every automated hit in `evidence.md` and `evidence_k16.md`.

Lineage notes used below: Xash `ref/gl/gl_beams.c`, `gl_decals.c`, `gl_sprite.c`, `gl_rpart.c`, `gl_triapi.c` are Uncle Mike's GPLv3 re-implementations of closed GoldSrc engine code (not in HLSDK). Xash `gl_studio.c` chrome/frame code descends from HLSDK `utils/mdlviewer/studio_render.cpp` (same code exists in both). `qcsg/map.c baseaxis` is Quake-tools (id, GPL) heritage that ships in HLSDK `utils/`.

#### 1. Summary table


Totals (rounded, minor/low items included): Quake ≈ 180 lines, Xash ≈ 560 (of which ≈ 332 in glr_beam.c), HLSDK/Quake-tools ≈ 57, QCtrans 0.

---

#### 2. Per-file sections

##### n64/port/gles/glr.h (43 lines; git: untracked)
Verdict: **CLEAN**. Declares the platform seam (`QGP_GL_Swap`, `QGP_GL_DrawableSize`, `GLR_ContextLost/Restored`, `GLR_Screenshot`, overlay calls). No Quake type, no copied text.
Automated hits: none.

##### n64/port/gles/glr_alias.c (474 lines; git: untracked)
Verdict: **QUAKE-INTERFACE + CONTAINS-QUAKE (structural, small)**.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|227-247|PoseVerts (group-frame pick)|CONTAINS-QUAKE|first/source/r_alias.c R_AliasSetupFrame 659-695 (also quake/WinQuake/gl_rmain.c R_SetupAliasFrame 150-180)|renamed copy: group pointer, `intervals[n-1]`, `time = cl.time + syncbase`, `target = time - (int)(time/full)*full`, break-on-`intervals[i] > target` loop|
|266-282|SetupFrames (single/group branch)|CONTAINS-QUAKE|same|renamed copy of the same block, plus our lerp extension|
|375-380|per-vertex s/t decode|CONTAINS-QUAKE (minor)|quake/WinQuake/gl_mesh.c GL_MakeAliasModelDisplayLists 264-269|structural: seam shift `s += skinwidth/2` for back faces on onseam verts, `(s+0.5)/skinwidth`; this is the MDL format's seam rule restated, expression differs (float vs int, no command stream)|
|196-225|UpdateLerp|CLEAN|none (uses our `entity_t` fields `prevframe/lastframe/lerpmodel/frame_start`, declared in repo `source/render.h:84`, not in Quake)|own design|
|304-388|AliasDraw lighting|CLEAN|none: shade = normal·(0,0.3,0.95), `0.8+0.45*shade`; Quake uses `shadedots`/`shadelight` tables (gl_rmain.c 280-329)|own formula|
|395-461|SortCands/Gather/DrawEntities selection|CLEAN|cart's selection rule (1536/512 units, max 64/40)|own|
|59-192|skins/AMB2 bake reader|CLEAN|AMB2 is our bake format|own|

Quake bindings: `entity_t` (ent->model/frame/skinnum/origin/syncbase…, `cl.viewent`, `cl.time`, `cl.num_statics`, `cl_visedicts`, `cl_numvisedicts`, `cl_static_entities`, `cls.timedemo`), `model_t`/`Mod_Extradata`, `aliashdr_t/mdl_t/maliasgroup_t/maliasskindesc_t/maliasskingroup_t/stvert_t/mtriangle_t/trivertx_t` (Quake's in-memory MDL structs), `ALIAS_SINGLE/ALIAS_SKIN_GROUP/ALIAS_ONSEAM`, `EF_GIB/EF_ZOMGIB`, `cvar_t/Cvar_RegisterVariable`, `r_refdef.vieworg`, `vpn`, `MAX_VISEDICTS`, `Q_strncpy`, `MAX_QPATH`, **`r_avertexnormals` (table from id's `anorms.h`, glr_main.c 34-36)**.
Automated hits: k16 `322-326` (HLSDK/Quake/Xash `return false;`) — false positive (generic `if (!tex) return false;`).

##### n64/port/gles/glr_beam.c (514 lines; git: untracked)
Verdict: **CONTAINS-XASH** (file is a translation of Xash `gl_beams.c` into GLES vertex emission; its own header says so), QUAKE-INTERFACE minor (`r_origin`, `vpn/vright/vup`, `cl.time`, `Length/VectorMA…`).

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|43-57|FracNoise|CONTAINS-XASH|xash/ref/gl/gl_beams.c FracNoise 45-58|renamed copy (midpoint = mean + `divs * rand(-0.125, 0.125)`, recursion halves, `div2 > 1`)|
|59-66|SineNoise|CONTAINS-XASH|gl_beams.c SineNoise 60-70|renamed (one-line loop form)|
|133-229|DrawSegs|CONTAINS-XASH|gl_beams.c R_DrawSegs 158-334|structural translation, same constants and order: `segments<2`, `maxw = width*0.5`, `length*div < maxw*1.414` → `(int)(length/(maxw*1.414))+1`, clamp NOISE_DIVISIONS, `length*=0.01`, `vlast = fmod(freq*speed,1)`, SINENOISE branch (≥16 segs, `scale*=100`, `length=segments*0.1`) else `scale*=length*2`, `noisestep=(int)((N-1)*div*65536)`, perp1 = vforward×dir, per-point noise along perp or `sin/cos(fraction*pi*length+freq)` on vup/vright, normal = (pos[k]-pos[k+1])×(pos[k]-eye), neighbour-average normals, fraction shade ramps for SHADEIN/OUT. Only the vertex emission (strip pairs instead of immediate-mode) and a precomputed normal array differ|
|231-295|DrawRing|CONTAINS-XASH|gl_beams.c R_DrawRing 681-802|structural translation: `segments*=pi`, clamp 8*N, `length=len*0.01*pi`, `vstep=length*div/8`, `scale=amp*length/8`, `noisestep*8`, centre/xaxis/yaxis (xaxis[1], -xaxis[0], 0), `cos(fraction*pi*24+freq)`, `j=segments/8` renoise|
|297-349|DrawTorus|CONTAINS-XASH|gl_beams.c R_DrawTorus 336-425|structural: radius `freq*delta[2]`, `cos(fraction*pi*3+freq)`, screen-space normal (-y,x)|
|351-384|DrawDisk|CONTAINS-XASH|gl_beams.c R_DrawDisk 427-477|structural: `w = fmod(freq, width*0.1)*delta[2]`, centre/rim vertex pairs|
|386-422|DrawCylinder|CONTAINS-XASH|gl_beams.c R_DrawCylinder 479-529|structural: `z+width` dark rim, `(delta[2]+width)*freq` lit rim|
|435-446|noise regen per frame|CONTAINS-XASH|gl_beams.c R_BeamDraw 913-924|structural: zero endpoints, Sine vs Frac by flag|
|451-452|segment count|CONTAINS-XASH|gl_beams.c 944-946|`len*0.25+3` if amplitude ≥ 0.5 else `len*0.075+3`|
|483-486|render mode choice|CONTAINS-XASH|gl_beams.c 1009|FBEAM_SOLID → kRenderNormal else kRenderTransAdd|
|504-507|solid-first / additive-second passes|CONTAINS-XASH|gl_beams.c R_DrawBeamsList ~1215-1240|same pass split by FBEAM_SOLID|
|70-118|Emit/Screen/ScreenNormal|CLEAN (ours)|Xash uses TriWorldToScreen/TriVertex; ours is a GLES vertex builder|own helper code|
|35-39|statics|CLEAN|(k16 hit gl_beams.c 41-48 is a false positive: unrelated declaration shapes)|—|
Notable deviation (not a clearing factor): NOISE_DIVISIONS is 128 here vs 64 in Xash; no beam follow / hose code (R_DrawBeamFollow, R_BeamDraw TE_BEAMHOSE) was taken.
Automated hits (`xash 147-157 n=73`, `150-157 n=36`, `160-170 n=39`, k16 `39-51`): first three **real** (DrawSegs segment-count / SINENOISE blocks); k16 39-51 false positive.

##### n64/port/gles/glr_decal.c (873 lines; git: untracked)
Verdict: **CONTAINS-XASH (two functions) + CONTAINS-HLSDK/Quake-tools (axis table) + QUAKE-INTERFACE**.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|242-260|DecalAlloc|CONTAINS-XASH|xash/ref/gl/gl_decals.c R_DecalAlloc 99-128|renamed copy: reuse pick else next pool slot with `do { wrap; take; count++ } while (permanent && count < limit)`, then unlink/free|
|449-512|Intersect|CONTAINS-XASH|gl_decals.c R_DecalIntersect 431-505|structural translation: new decal's half extents `w*0.5, h*0.5` along basis, project min/max into the old decal's [0,1] space with `+0.5` bias and `-dx/-dy`, clamp to [0,1], area test `> 0.6`, `lastArea = 2` initial, replace when `area <= lastArea`; caller replaces when `count >= MAX_OVERLAP_DECALS (6)` (gl_decals.c R_DecalCreate 577-578). Constants DECAL_DISTANCE 4, MAX_OVERLAP_DECALS 6 also match gl_decals.c 19, 25|
|50-53, 592|constants / use|CONTAINS-XASH (constants)|gl_decals.c 19, 25|values identical to Xash|
|324-351|BaseAxes (TextureAxisFromPlane)|CONTAINS-HLSDK (Quake-tools heritage)|hlsdk/utils/qcsg/map.c `baseaxis` 25-34 + TextureAxisFromPlane 36-55 (Quake qbsp heritage)|6-row axis table identical to baseaxis (floor/ceil/W/E/S/N, same signs); selection loop re-typed; ours comment cites "Quake's TextureAxisFromPlane"|
|85-146|WadTexture (WAD3 reader)|CLEAN|WAD3 format|own parser, own bounds checks|
|395-416, 420-447|ClipEdge / ClipCandidate (Sutherland-Hodgman)|CLEAN|Xash uses 4 separate edge tests (R_ClipInside/R_ClipIntersect/SHClip 264-383); ours is a single signed-distance clip|textbook algorithm, different structure|
|292-322, 355-385, 515-625|Collect / TexAxes / Place (per-triangle candidates from cooked w64 geometry)|CLEAN|Xash walks BSP nodes (R_DecalNode 704-731, R_DecalSurface 610-673); ours has no BSP|own design; header comment cites Xash behaviour/names only|
|632-721|GLR_DecalShoot / PendRemove / Retry (pending queue, retries, permanent repeat check)|CLEAN|no Xash counterpart (Xash has save/restore lists instead)|own|
|766-871|GLR_DecalsDraw|CLEAN|Xash DrawDecalsBatch 1017-1057 uses GL1 state; ours sets src_alpha/one_minus_src_alpha, polygon offset (-1,-2), no depth write — documented behaviour|own code; state values generic|
Quake bindings: `entity_t` (`origin/angles/model`, `GSFXC_EntityNum`), `model_t`, `cl.time`, `cl_visedicts/cl_numvisedicts`, `Con_Printf`, `Q_strcasecmp/Q_strncpy`, `AngleVectors`, `SV_LayerAt` (our w64), `r_origin`, `VectorX/DotProduct/CrossProduct/Length`.
Automated hits: k16 HLSDK `301-305` (qbsp2 `return;`) false positive; HLSDK `329-334` (qcsg map.c baseaxis) **real**; QUAKE `333-334` (quake2 r_rast.c box_vecs row) false positive (same literal row only); evidence QUAKE `333-334 n=35` same.

##### n64/port/gles/glr_draw.c (711 lines; git: untracked)
Verdict: **QUAKE-INTERFACE + CONTAINS-QUAKE (minor, forced by the Draw_ API)**.

|lines|function|verdict|counterpart|basis|
|---|---|---|---|---|
|403-408|Draw_Character|CONTAINS-QUAKE (minor)|quake/WinQuake/gl_draw.c Draw_Character 506-519 (also first/source/draw.c)|same guards and index math (`num==32`, `num&=255`, `y<=-8`, `row=num>>4`, `col=num&15`), different draw|
|415-419|Draw_String|CONTAINS-QUAKE (trivial)|gl_draw.c 540-548|same loop (+8 per char)|
|587-593|Draw_CachePic head|CONTAINS-QUAKE (minor)|gl_draw.c Draw_CachePic 232-246|renamed: linear `strcmp(path, name)` scan, full-table `Sys_Error`, count++|
|609-614|Draw_CachePic load|CONTAINS-QUAKE (minor)|gl_draw.c 251-262|load → fail message `"Draw_CachePic: failed to load %s"` (identical text), `SwapPic`, width/height copy|
|571-574|Draw_PicFromWad|API one-liner|gl_draw.c/draw.c|`W_GetLumpName(name)`; forced by Quake's lump API|
|392-397|Draw_Init|API|first/source/draw.c|`W_GetLumpName("conchars"/"disc")`; forced|
|442-464|Draw_TransPicTranslate|CLEAN|software draw.c 427-… copies pixels; ours converts through the table into a texture|own|
|89-120, 214-255, 292-345, 471-561, 633-711|2D shader/queue/HUD sheets/fades/overlay|CLEAN|no counterpart (GLQuake uses immediate-mode quads)|own|
Quake bindings: `qpic_t`, `W_GetLumpName`, `SwapPic`, `vid` (width/height), `Draw_*` API, `Sys_Error`, `Q_strncpy`, `MAX_QPATH`, `draw_disc`.
Automated hits: k16 HLSDK `241-245` (qbsp2 gldraw.c `glUseProgram`) false positive; QUAKE `364-367` (QW sbar.c `memset(&dp…)`) false positive; QUAKE `571-579` (first draw.c Draw_PicFromWad) = the API one-liner above (trivial).

##### n64/port/gles/glr_fx.h (55 lines; git: untracked)
Verdict: **CLEAN**. `FX_SET` is a generic 3-component set macro (hit vs xash3d_mathlib.h:122 is a trivial-idiom false positive). Comment cites "Xash gl_sprite.c, TriRenderMode" for the render-mode semantics (behaviour, see glr_sprite.c).
Automated hits: XASH `15 n=43/45` false positive.

##### n64/port/gles/glr_gl.c (387 lines; git: untracked)
Verdict: **CLEAN + QUAKE-INTERFACE** (no copied code). Memory wrappers, file load, matrices, shader build, streaming ring with `glMapBufferRange` (own), `GLR_EntityMatrix` (cart convention: negated pitch, X→forward, Y→-right, Z→up), RGBA5551/CI4/CI8 decoders, generic textured program: all ours.
Quake bindings: `Sys_Error`, `COM_OpenFile/COM_CloseFile`, `Sys_FileRead`, `AngleVectors`, `entity_t` (`angles/origin`), `r_origin`, `glr_pal`.
Automated hits: XASH `71-73` (gnm_shim `memset(m,0,sizeof(glr_mat4))`) false positive.

##### n64/port/gles/glr_hlpart.c (256 lines; git: untracked)
Verdict: **CONTAINS-XASH (tracers)**; points CLEAN.

|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|146-186|AddTracer|CONTAINS-XASH|xash/ref/gl/gl_rpart.c CL_DrawTracers 152-262 (lines 183-239)|structural translation of the tracer quad: start/end = org, org + vel*len, project both to screen, screen-space direction (-y,x) normal scaled by tracer size, `verts[0]=start-normal, [1]=start+normal, [2]=[0]+delta, [3]=[1]+delta`, texcoords (0,0.8),(1,0.8),(1,0),(0,0) in the same vertex order, additive blend, no depth write. Only the delta source (`vel*len` vs `vel*ramp*atten`) and the culling differ|
|189-196, 245-249|DotTex + additive draw|CONTAINS-XASH (minor)|gl_rpart.c 170-177|dot sprite texture, `GL_SRC_ALPHA, GL_ONE`, no depth write|
|63-141, 198-243|point instancing (hp_vs, DrawPoints, GLR_HLPartsDraw loop)|CLEAN|Xash CL_DrawParticles 48-107 sizes by `partsize`/depth scale and `GL_POINTS`; ours is glr_part.c's `PART_K/depth` clamp|own; header cites Xash blend state only (generic)|
Quake bindings: `r_origin`, `vpn/vright/vup`, `vec3` math, `glr_vp`.
Automated hits: none.

##### n64/port/gles/glr_light.c (91 lines; git: untracked)
Verdict: **QUAKE-INTERFACE + CONTAINS-QUAKE (small)**.

|lines|function|verdict|counterpart|basis|
|---|---|---|---|---|
|29-40|GLR_AnimateLight|CONTAINS-QUAKE|first/source/r_light.c R_AnimateLight 31-47|structural: loop over MAX_LIGHTSTYLES; `length==0 → 256`; `(map[k]-'a')*22`; `k = i % length`; our additions: tick cap (20-28) and a `length==1` branch|
|55-64|GLR_PointDlights|CONTAINS-QUAKE (minor)|first/source/r_main.c R_DrawEntitiesOnList 572-579 and R_DrawViewModel 648-655|same accumulation: skip if `die < cl.time`/`!radius`, `add = radius - Length(origin - dl.origin)`, `if add > 0` accumulate; ours multiplies by `dl.bright`|
|13, 67-79|r_lightscale / GLR_FinishLight|CLEAN|cart|own|
|83-91|GLR_LightPoint|CLEAN|cooked probe + dlights|own|
Quake bindings: `cl.time`, `cl_lightstyle[]`, `cl_dlights[]`, `MAX_LIGHTSTYLES/MAX_DLIGHTS`, `cvar_t`, `Length`.
Automated hits: QUAKE `28-31 n=25` (r_light.c 40-41) = the AnimateLight block (real, structural); QUAKE `53-57 n=23` (r_main.c 568-570) = the dlight accumulation (real, minor).

##### n64/port/gles/glr_local.h (232 lines; git: untracked)
Verdict: **QUAKE-INTERFACE**. Includes `quakedef.h`; uses `entity_t`, `mleaf_t`, `cvar_t`, `vec3_t`, `byte`, `qboolean`, `dlight_t`; documents the VID_/R_/D_ seam. No copied code.
Automated hits: none.

##### n64/port/gles/glr_main.c (707 lines; git: untracked)
Verdict: **CONTAINS-QUAKE (exact copy of R_SetVrect) + QUAKE-INTERFACE**.

|lines|function|verdict|counterpart|basis|
|---|---|---|---|---|
|538-569|R_SetVrect|**CONTAINS-QUAKE (exact copy)**|first/source/r_main.c R_SetVrect 302-337 (= quake/WinQuake/r_main.c 307-345)|this is the 253-token hit; identical statement for statement (size clamp, intermission override, `size /= 100`, `h = height - lineadj`, 96-wide minimum, `&= ~7`, `&= ~1`, centring, `lcd_x` halving); only the comment `// min for icons` and blank lines removed|
|460-465|R_Init notexture setup|CONTAINS-QUAKE (renamed copy)|first/source/r_main.c R_InitTextures 156-162|identical `width=height=16` and the four `offsets[]` assignments (`sizeof(texture_t)`, +16*16, +8*8, +4*4); our checkerboard fill and static storage differ (466-467)|
|263-268|VID_Init body|CONTAINS-QUAKE (renamed, minor)|first/source/vid_null.c VID_Init 50-62 (and vid_x.c/vid_win.c)|`vid.aspect=1.0`, `vid.colormap=host_colormap`, `vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048))` (verbatim line), `vid.buffer=vid.conbuffer`, `rowbytes`|
|624-625|R_RenderView prologue|CONTAINS-QUAKE (trivial idiom)|first/source/r_main.c R_SetupFrame / R_RenderView `VectorCopy(vieworg, r_origin); AngleVectors(viewangles, vpn, vright, vup)`|2-line standard idiom (k16 QUAKE `623-626 n=27` vs r_misc.c is this)|
|25, 34-36|`r_avertexnormals` ← `#include "anorms.h"`|data dependency on id file|first/source/anorms.h (header "Copyright (C) 1996-1997 Id Software"), same include pattern as r_alias.c:72 / gl_rmain.c:267|162 vertex-normal table; GPL id file; consumed by glr_alias.c|
|352-452|SetProjection / SetFrustum / CullSphere / SetViewport / SetFov (Hor+)|CLEAN|GLQuake R_SetupGL/MYgluPerspective, Quake R_SetFrustum use different formulas|own|
|122-177, 281-298, 312-348|FBO / present / screenshot|CLEAN|—|own|
|478-516|CarveLayerTables / R_NewMap / R_LayerSwitch|CLEAN (cart)|—|own|
|577-616|view-weapon pass|CLEAN|comment cites Xash behaviour (shared projection)|own|
Quake bindings (heavy): `viddef_t vid`, `refdef_t r_refdef`, `r_origin/vpn/vright/vup`, `mleaf_t *r_viewleaf/r_oldviewleaf`, `texture_t *r_notexture_mip`, `r_cache_thrash`, `cvar_t/Cvar_RegisterVariable`, `Cmd_AddCommand`, `Con_Printf`, `Sys_Error`, `host_colormap`, `LittleLong`, `Q_strcasecmp`, `cl.worldmodel/cl.viewent/cl.stats[STAT_HEALTH]/cl.intermission`, `scr_fov/scr_viewsize/lcd_x/chase_active`, `mod_alias/mod_studio`, `R_InitParticles/R_ClearParticles/R_KillParticles/R_DrawParticles`, and the whole `VID_*/R_*/D_*` entry-point set.
Automated hits: QUAKE `460-466` real (notexture); QUAKE `535-563 n=174/210` real (R_SetVrect — the 253-token hit); QUAKE `623-626` trivial idiom (above).

##### n64/port/gles/glr_part.c (138 lines; git: untracked)
Verdict: **CLEAN + QUAKE-INTERFACE**. Instanced square particles sized `PART_K/depth` clamped (calibrated against the cart, comment 11-16); Quake's GL R_DrawParticles (`scale = 1 + dist*0.004`, textured quads) is not used.
Quake bindings: `D_StartParticles/D_DrawParticle/D_EndParticles` (the software renderer's hooks), `particle_t` (`org`, `color`), `r_origin`, `vpn`, `glr_pal`.
Automated hits: none.

##### n64/port/gles/glr_shot.c (73 lines; git: untracked)
Verdict: **CLEAN**. PPM/PNG writers (zlib `compress2`, filter 0, CRC chunks). No Quake/Xash/HLSDK relation (Quake `WritePCXfile`/`SCR_ScreenShot` not used).
Automated hits: none.

##### n64/port/gles/glr_sprite.c (361 lines; git: untracked)
Verdict: **CONTAINS-XASH (sprite orientation switch + quad), low-confidence XASH (render-mode table), rest CLEAN**.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|282-320|GLR_SpriteDraw orientation|CONTAINS-XASH|xash/ref/gl/gl_sprite.c R_DrawSpriteModel 464-512|structural translation: parallel+roll promoted to parallel-oriented, `SPR_ORIENTED` (angles → axes, origin pushed back `0.01` along forward), `FACING_UPRIGHT` with `right = (org.y - eye.y, -(org.x - eye.x), 0)`, `up=(0,0,1)`, `PARALLEL_UPRIGHT` (`0.999848` cos(1°) invisibility, `right=(vf.y,-vf.x,0)`), `PARALLEL_ORIENTED` (`angle*(pi/180)`, `right = vright*cr + vup*sr`, `up = vright*-sr + vup*cr`), default vright/vup. The upright / oriented / parallel-oriented cases also exist in first/source/r_sprite.c 296-380 (software Quake) from which Xash drew; the roll-promotion and the exact `FACING_UPRIGHT` expression are Xash's|
|325-328|quad corners and UVs|CONTAINS-XASH/QUAKE (minor)|gl_sprite.c R_DrawSpriteQuad 292-307; quake/WinQuake/gl_rmain.c R_DrawSpriteModel 214-250|same four corners `down/up × left/right`, UV (0,1),(0,0),(1,0),(1,1) in the same order|
|117-143|GLR_FxMode / GLR_FxModeEnd|possible-XASH (low confidence)|xash/ref/gl/gl_triapi.c TriRenderMode 40-72; gl_sprite.c 404-426|mode→blend table: TransColor/TransTexture(/Alpha) src_alpha,1-src_alpha; Glow/TransAdd src_alpha,one; translucent no depth write; same case grouping. These are the documented render-mode semantics, but the grouping follows Xash; re-derive from the semantics|
|14-114, 150-263, 332-359|fx program, stream plumbing, frame textures (INDEXALPHA/ALPHTEST), Dilate, init/ctx|CLEAN|—|own; texture format mapping follows the SPR format semantics|
Quake bindings: `r_origin`, `vpn/vright/vup`, `AngleVectors`, `VectorMA/VectorNormalize`, `glr_vp`.
Automated hits: none (scanner missed this; found structurally).

##### n64/port/gles/glr_studio.c (1330 lines; git: untracked)
Verdict: **QUAKE-INTERFACE; small CONTAINS-HLSDK/XASH blocks; rest CLEAN** (the renderer is built on our STB2 bakes and `studio_geom.c`; skinning on GPU via a bone-matrix texture has no HLSDK/Xash counterpart — they skin on the CPU in StudioSetupBones/R_StudioDrawPoints).

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|139-149|chrome ST in studio_vs|CONTAINS-HLSDK / XASH (structural, GLSL)|hlsdk/utils/mdlviewer/studio_render.cpp StudioModel::Chrome 441-471; xash/ref/gl/gl_studio.c R_StudioSetupChrome 1117-1155|same maths: `tmp` = normalised bone origin offset (camera-relative: our origin is the eye), `up = norm(tmp × vright)`, `right = up × tmp` (Xash's operand order; mdlviewer uses `tmp × up`, sign flip may be our mirrored Y axis), bone-space `iup/irt` by transposed rotation, `s = (n·right + 1) * 32`, `t = (n·up + 1) * 32`. HLSDK lineage ⇒ allowed to `game/`; the Xash operand order cannot be ruled out|
|635-648|ViewFrame wrap/clamp|CONTAINS-XASH/HLSDK (minor)|xash gl_studio.c frame calc 574-587; hlsdk/utils/mdlviewer/studio_render.cpp AdvanceFrame 317|looping: `f -= (int)(f/(n-1))*(n-1); if (f<0) f += n-1`; non-looping clamp (Xash clamps at n-1.001, ours at n-1, then ≥0)|
|1104-1107|LiveDraw looping frame|CONTAINS-XASH/HLSDK (minor)|same|same idiom|
|568-598|NeutralControllers|CLEAN (format-derived)|hlsdk/dlls/animation.cpp SetController 321-371 (line 366: `255*(v-start)/(end-start)`)|inverse of the controller byte↔angle mapping at value 0; formula is the format definition, structure (RLOOP branch, clamps) is ours|
|733-747, 853-866, 1029-1075|StudioModeBegin / glow-shell second pass / shell offset along normals|CLEAN (behaviour, comment cites)|Xash gl_studio.c GL_StudioSetRenderMode, R_StudioRenderModel 2819-2845, shell scale 2003-2008; HLSDK StudioModelRenderer.cpp StudioRenderModel 1969-2000|same two-pass behaviour, implemented differently (flat colour uniform, per-vertex normal offset); `shellscale` formula itself is in glr_trans.c|
|694-731|StudioFx (attachments → client)|CLEAN|HLSDK StudioSetupBones attachment transform is a plain 3x4 transform|trivial|
|35-180, 183-332, 345-565, 760-950, 1186-1330|STB2 bake cache, GPU upload, selection, temp entities, live path|CLEAN|— (cart/our formats)|own|
Quake bindings: `entity_t` (`skinnum/body/frame/framef/origin/angles/model`, `cl.time`, `cl.viewent`, `cl_entities`, `cl_visedicts`, `MAX_EDICTS/MAX_VISEDICTS`), `model_t`, `Mod_StudioHeader/Mod_StudioLoadBake/Mod_StudioResTouch/…` (our extensions of Quake's Mod_), `cvar_t`, `Q_strncpy`, `r_refdef.vieworg`, `vpn`, `LittleShort`, `studiohdr_t/mstudio*` (HLSDK `studio.h`).
Automated hits: none.

##### n64/port/gles/glr_trans.c (200 lines; git: untracked)
Verdict: **QUAKE-INTERFACE + minor CONTAINS-XASH**.

|lines|block|verdict|counterpart|basis|
|---|---|---|---|---|
|100-105, 138-142, 160|TransCompare / dist key / qsort|CONTAINS-XASH (minor)|xash/ref/gl/gl_rmain.c R_TransEntityCompare 86-127 (+ qsort 522)|key = squared distance from eye to `origin + avg(mins,maxs)`, brush kRenderTransAlpha forced to `1e9` (Xash `1000000000`), descending order|
|62-65|GLR_EntTrans|CONTAINS-XASH (low)|gl_rmain.c R_OpaqueEntity 63-77|rendermode≠normal or GlowShell|
|85|shellscale|CONTAINS-XASH (one line)|xash/ref/gl/gl_studio.c 2005-2006|`max(1/128, renderamt/128)` (comment cites it)|
|14-60, 108-200|GLR_SetBlend (constant-alpha blend), RenderMode fill, TransDraw ordering of items/beams/particles|CLEAN|Xash R_DrawEntitiesOnList 807-952 draws solid → sprites → EFX → trans ents → trans EFX → view model; ours order is different and uses a unified item list|own|
Quake bindings: `entity_t` (`rendermode/renderamt/renderfx/rendercolor`, `cl.viewent`, `cl_visedicts`), `model_t` (`mins/maxs/type`), `r_refdef.vieworg`.
Automated hits: none.

##### n64/port/gles/glr_world.c (1623 lines; git: untracked)
Verdict: **QUAKE-INTERFACE + CONTAINS-QUAKE (two blocks) + low-confidence XASH (one block); rest CLEAN**. This is a renderer for our cooked `.w64` stream arch (sector list → meshlets → per page draw); there is no BSP traversal, so none of `R_RecursiveWorldNode`, `R_BuildLightMap`, `R_RenderBrushPoly`, `SV_RecursiveHullCheck`, Xash `R_DecalNode`, `R_DrawWorld`, `R_MarkLeaves` appears.

|lines|function/block|verdict|counterpart|basis|
|---|---|---|---|---|
|743-761|TexAnim (texture animation)|CONTAINS-QUAKE|first/source/r_surf.c R_TextureAnimation 213-240|structural translation over the cooked table: alternate when frame≠0, `anim_total` early out, `rel = (int)(cl.time*10) % anim_total`, `while (anim_min > rel || anim_max <= rel)` follow `anim_next`, guard `> 100`|
|215-231|world_fs dlight falloff|CONTAINS-QUAKE (GLSL port)|first/source/r_surf.c R_AddDynamicLights 61-123|renamed translation: `rad = radius - |plane distance|; if rad < minlight skip; minlight = rad - minlight; if inplane dist < minlight: add (rad - dist) * brightness` — same unusual threshold; replaces Quake's `sd + td/2` metric with Euclidean distance and per luxel→per fragment. Inherited from the cart's r64_sector.c DlightVerts (other slice)|
|1284-1314|GLR_WorldDrawEntity render-mode switch|possible-XASH (low confidence)|xash/ref/gl/gl_rsurf.c R_SetRenderMode 1762-1807|per-mode state table: TransColor flat rendercolor alpha blend; TransAdd `blend,blend,blend` colour with GL_ONE,GL_ONE and no depth write; TransAlpha alpha test **0.25**; TransTexture/Glow alpha blend, no depth write; comment cites it. Behaviour is GoldSrc-documented; constants and case set match Xash|
|767-781|BoxCulledP|CLEAN|standard p-vertex box/plane test (Quake R_CullBox uses BoxOnPlaneSide)|generic|
|160-232 (world_vs, rest of world_fs)|world shaders|CLEAN|style summing/`floor(floor(s/256)*scale+0.5)` follows the lightstyle format but expression is own|own|
|241-486|GPU arena / chunk records / install hook|CLEAN|—|own|
|511-735|GeomDecode / GeomIndices / GeomUpload / Page / PageTex|CLEAN|w64/tiny3d formats|own|
|875-1081|MarkWorld / DrawList|CLEAN|own cooked-vis list, no PVS decompress (Quake Mod_DecompressVis not used)|own|
|1082-1271, 1321-1360|DrawCross / DrawMover / DrawMovers / GLR_WorldDraw|CLEAN|cart's q64_world.c / r64_sector.c|own|
|1361-1416|GLR_LightProbe|CLEAN|cooked probe grid (Quake R_LightPoint ray-casts the BSP; not used)|own|
|1417-1623|WorldContextLost, GLR_WorldTris/Ex/WTriChunk|CLEAN|—|own|
Quake bindings: `entity_t`, `model_t`/`Mod_IsW64/Mod_LeafNum`, `mleaf_t *r_viewleaf`, `dlight_t cl_dlights[]/MAX_DLIGHTS`, `cl.time/cl.worldmodel`, `cl_visedicts`, `cvar_t` (`r_crosslayer`, `gl_cullworld`, `r_lightscale`), `Con_Printf/Con_DPrintf`, `SV_LayerAt`, `r_refdef.vieworg`, `AngleVectors`, `d_lightstylevalue`, plus the repo's own `w64_*`/`W64_*`/`Q64_*` (cart format; separate slice).
Automated hits: none.

##### n64/port/gles/test/glr_sheet.py (44 lines; git: untracked)
Verdict: **CLEAN**. PIL side-by-side contact sheet. No counterpart.

##### n64/port/gles/test/run_glr_uplink.sh (78 lines; git: untracked)
Verdict: **CLEAN**. Test driver (Xvfb + hl_sdl + route shots). No counterpart.

---

#### 3. Cross-cutting findings for hl64

1. **Verbatim Quake code exists in glr_main.c** (R_SetVrect 538-569; notexture offsets 460-465; VID_Init fullbright line). Must be rewritten; a clean R_SetVrect needs only the documented "viewsize%" behaviour. The notexture offsets are Quake's mip layout; replace with own constants.
2. **glr_beam.c is a Xash translation** (≈ 332 lines) — highest-risk file in the slice; cannot move to MIT hl64. Other Xash-shaped code is small: decal alloc/intersect (83 lines), tracer quad (≈ 50), sprite orientation switch (≈ 43), trans sort key (≈ 14), brush render-mode table and FxMode table (low confidence).
3. **id data file dependency**: `anorms.h` (162 normals, id Software header) is `#include`d by glr_main.c for `r_avertexnormals`; the table is also needed to decode MDL `lightnormalindex`. Regenerating it independently would still produce the same numbers (it is the MDL format's normal set) but the file text is id's.
4. **Quake-API binding is thin and enumerated per file**; the three structural Quake blocks (alias group frames, lightstyle animation/dlight accumulate, texture animation, dlight falloff) are short and rewritable in an hour.
5. Everything not listed above (2D renderer, GL plumbing, streaming ring, screenshot, particle instancing, the whole w64 world renderer, STB2 studio skinning, WAD3 decals reader, sprite shader plumbing, test scripts) is original code with no Quake/Xash/HLSDK expression found by token scan or structural comparison.


### A.AuditFxSound — GoldSrc effects and sound


Audited the working tree (`n64/port/`) as it stood on 2026-11-13. All gsfx files, `host/q64_host_audio.c` and `sdl/sdl_audio.c` are untracked. `snd_n64.c` is tracked: created in da63eca (2026-07-18), modified in 8942704, and modified again in the worktree. None of the 10 files carries an Id Software header, and none carries a GPL header either: the `GPL-header=False` flags in evidence.md are correct. Because there is no licence header at all, every file needs an MIT header added on import.
The structural review used these references:
- Xash `engine/client/cl_tent.c`, `cl_efx.c`, `ref/gl/gl_rmain.c` (CL_FxBlend), `ref/gl/gl_sprite.c`
- HLSDK `cl_dll/entity.cpp` (HUD_TempEntUpdate, HUD_StudioEvent) and `dlls/effects.cpp`, `explode.cpp`, `ggrenade.cpp`, `combat.cpp`, `util.cpp`
- Quake first-commit `world.c`, `r_sprite.c`, `model.c`, `snd_dma.c`
- libdragon `src/audio/wav64_vadpcm.c` and `mixer.c` at bc24d3d5a. libdragon is Unlicense / public domain.

#### 1. Summary


Totals across the slice (lines marked CONTAINS-* or HLSDK): Quake 68, Xash 1222, HLSDK 1450, QCtrans 0.

The main finding for this slice is the two client modules. gsfx_cl.c and gsfx_cart.c are near-parallel copies of each other, and both re-express Xash's GoldSrc effect presets function by function: the same constants and ordering, with renamed fields.

---

#### 2. Per-file sections

##### n64/port/gsfx/gsfx.h (668 lines; git: untracked)
Verdicts: QUAKE-INTERFACE; HLSDK interface constants (not counted as contamination).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-70|header prose|CLEAN|—|Ours. Lines 65-69 cite HLSDK/Xash as behaviour references ("do not paste").|
|78-83|rendermode enum kRender*|HLSDK interface|hlsdk common/const.h kRender*; identical in xash common/const.h|Public SDK enum. Values are stored in map entity lumps, so they are needed for data compatibility.|
|86-96|renderfx enum kRenderFx*|HLSDK interface|hlsdk common/const.h 690-716; xash common/const.h 695-720|Exact names and ordering (evidence hit). Interface constants.|
|99-103|SPR_* orientation|Quake interface (file format)|quake spritegn.h 82-86|File-format values.|
|104-107|SPR_NORMAL..SPR_ALPHTEST|HL file-format values|xash engine/sprite.h 51|File-format values.|
|109-121|FBEAM_* flags|HLSDK interface|hlsdk common/beamdef.h 21-31 (xash beamdef.h is identical)|Exact names and values. Interface constants, but only used internally (object/TE flags byte), so they could be renamed freely.|
|123-127|BLOOD_COLOR_*, DONT_BLEED|HLSDK interface|hlsdk dlls/cbase.h|Palette-index values.|
|129-225|TE_* numbering + payload comments, TE_EXPLFLAG_*, TE_BOUNCE_*, BREAK_*|HLSDK interface (protocol)|hlsdk common/const.h (TE numbering, BREAK_* at 655 ff., TE_EXPLFLAG at 160 ff.)|Numbers and names are interface. The payload descriptions are our own prose. TE_GSFX_SPARKSHOWER is ours.|
|227-298|svc_hlfx ops, gsfx_obj_t, GSFX_OF_*|CLEAN (ours)|—|Our protocol design.|
|300-668|prototypes, draw-list structs, renderer API|QUAKE-INTERFACE|—|Ours. Bound to Quake types.|

Bindings:
- Types: edict_t, entity_t, client_t, trace_t, model_t, studiohdr_t, qboolean.
- Comments name sv.datagram, pr_global_struct->trace_*, SV_Move, SV_SpawnServer, SV_Physics, Host_Init, CL_Init, CL_ClearState, CL_RelinkEntities, cl_visedicts and cl_entities.

Hit dispositions:
- XASH e 83-95 ← xash/common/const.h 695-708: real, but these are public renderfx constants (the same in HLSDK const.h). Interface, not expression.
- XASH n 96-127 ← xash/ref/gl/gl_export.h 57-97: false positive (identifier-normalised #define list against GL enums).
- XASH e 110-121 ← xash/common/beamdef.h 21-25: real. FBEAM_* constants, which originate in HLSDK common/beamdef.h. Interface constants.
- HLSDK n 127-262 ← hlsdk/cl_dll/tf_defs.h 14-162: false positive (normalised #define runs).

##### n64/port/gsfx/gsfx_cart.c (1492 lines; git: untracked)
Verdicts: QUAKE-INTERFACE, CONTAINS-XASH, HLSDK.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-50|header, externs, CART_* budgets|CLEAN|—|Ours. The comment names Xash/GoldSrc functions as the reference.|
|52-123|RF/RL, names, SpriteOf/StudioOf|CLEAN / QUAKE-INTERFACE|—|Ours. Uses Mod_ForName, Mod_FindName, Mod_StudioPrefetch, R_SpritePrefetch.|
|130-180|NearestPal/QColor/PaletteLoad|CLEAN / QUAKE-INTERFACE|—|Ours. Uses host_basepal and COM_OpenFile.|
|182-187|tracer_colors[12][3]|CONTAINS-XASH (data table)|xash ref/gl/gl_rpart.c 25-35 (GoldSrc gTracerColors)|Exact data copy, 12 RGB triples. Arguably factual data, but flagged.|
|190-274|TracerColor, SoundExists, sndsets, SoundPick, PlaySet|CLEAN / QUAKE-INTERFACE|—|Ours. HL sample names are content names. Uses S_PrecacheSound and S_StartSoundPitch.|
|278-293|Part|QUAKE-INTERFACE|—|Ours. Uses R_AllocParticle, particle_t and pt_gsfx.|
|296-309|SparkStreaks|CONTAINS-XASH|xash cl_efx.c R_SparkStreaks 1883-1900|Structural translation: count loop, 3 axes of random velocity, life 0.1-0.5, tracer colour 5, gravity.|
|313-321|FTENT_* (own values)|CLEAN|—|Values differ from HLSDK r_efx.h.|
|346-379|TentAlloc|CONTAINS-XASH (partial)|xash cl_tent.c CL_PrepareTEnt 227-259, CL_TempEntAlloc 438-465|Default set copied: die +0.75, fadespeed 0.5, bounce 1, renderamt 255, framemax = numframes-1. The ring-hint allocator is ours.|
|384-403|TentAllocHigh|CLEAN (behaviour)|xash cl_tent.c CL_TempEntAllocHigh 474-506|Own eviction policy; the comment explains how it differs.|
|405-420|TempSprite, SpriteNamed|CLEAN|—|Ours.|
|423-436|RicochetSprite|CONTAINS-XASH|cl_tent.c R_RicochetSprite 783-800|Same field settings: kRenderGlow, amt 200, NoDissipation, roll 45×rand(0..7), die = duration.|
|439-443|SparkEffect|CONTAINS-XASH|cl_tent.c R_SparkEffect 1443-1454|Ricochet sprite 0.1 s with scale rand 0.5-1, plus spark streaks.|
|446-473|BulletImpact|CONTAINS-XASH|cl_efx.c R_BulletImpactParticles 1718-1748|Distance clamp 1000, quantity (1000-d)/100 (minimum 1), 2 spark streaks at ±200, quantity×4 points. Capped for the cart.|
|476-507|BloodStream|CONTAINS-XASH|cl_efx.c R_BloodStream 1244-1310|Arc 0.05 / 0.005 step, 100-particle arc, speed/5 scatter with arc 0.075, num×1.7, colour+rand(0..9), lives 2/3 s. Subsampled.|
|510-531|Blood|CONTAINS-XASH (partial)|cl_efx.c R_Blood 1208-1235|Same ±3 position jitter, life 1.5, colour+rand(0..9), speed/2 base count. Loop structure restated.|
|536-576|BloodSprite|CONTAINS-XASH|cl_tent.c R_BloodSprite 872-944|TransTexture + ClampMinScale, scale rand(size/25, size/35), framerate = framemax×4, die = framemax/framerate, roll 0-360, bounce 0; drops at vel ±96/-32..95, die 1-3 s, random frame.|
|579-627|Explosion (+ R_FlickerParticles)|CONTAINS-XASH|cl_tent.c R_Explosion 1579-1622, R_Sprite_Explode 1190-1212, cl_efx.c R_FlickerParticles 1756-1772|Same dlight pair (200 / +0.01 s / decay 800; 150 / decay 200), vel z 8, origin +10, additive unless NOADDITIVE, sound attenuation 0.3, 15 particles at ±32 / 80-143, colour 254.|
|630-645|Smoke|CONTAINS-XASH|cl_tent.c R_Sprite_Smoke 1221-1234 + R_DefaultSprite 1091-1118|Grey 20-35, TransAlpha, vel z 30, origin +20, default framerate 10.|
|648-664|TempModel|CONTAINS-XASH|cl_tent.c R_TempModel 1040-1082|Flags COLLIDEWORLD\|GRAVITY\|ROTATE, avel ±255, yaw, hit sound by bounce type, die = life.|
|668-724|BreakModel|CONTAINS-XASH|cl_tent.c R_BreakModel 953-1031|Surface-area count /(3·12·12), cap 100 (cart cap 12), random spot ±0.5·size, hitsound = type, random body, 200/255 rotate at ±256, 100/255 smoke trail, glass/trans → TransTexture 128, vel dir ± random, life + rand(0,1).|
|726-742|SparkShower|HLSDK|hlsdk dlls/explode.cpp CShower::Spawn/Think 39-71|Velocity rand 200-300 × normal, xy ±100, z ±200, speed rand 0.5-1.5, think 0.1, bounce 0.6.|
|747-781|BeamCylinder (particle ring), Decal|CLEAN|—|Ours (cart substitute).|
|785-888|ReadVec, GSFXC_ParseFX, SkipBeamTail, cvars, Trace, stats, GSFXC_ParseTEnt wrapper|CLEAN / QUAKE-INTERFACE|—|Ours. Protocol decode of our own svc_hlfx.|
|890-947|ParseTEnt beam cases (skip), BEAMCYLINDER decode, LIGHTNING, KILLBEAM|CLEAN (protocol decode)|—|Payload order is the HLSDK const.h interface.|
|948-953|TE_GUNSHOT|CONTAINS-XASH (minor)|cl_tent.c 2210-2220 (TE_GUNSHOTDECAL ricochet chance)|rand(0..0x7fff) < 0x3fff ricochet.|
|954-969|TE_EXPLOSION, TE_SMOKE|CLEAN (decode)|cl_tent.c 1906-1937|Protocol decode only.|
|970-981|TE_TRACER|CONTAINS-XASH (minor)|cl_efx.c R_TracerEffect 1823-1840|Speed 6000 (GoldSrc tracerspeed), life = len/speed, tracer colour 4.|
|982-986|TE_SPARKS|CLEAN (decode)|—|Calls SparkEffect.|
|987-1020|TE_SPRITE, TE_GLOWSPRITE|CONTAINS-XASH (minor)|cl_tent.c 2050-2072 (R_TempSprite args)|Render mode / fx choices: TransAdd + SPRANIMATE at 10 fps; Glow + NoDissipation + FADEOUT.|
|1021-1037|TE_STREAK_SPLASH|CONTAINS-XASH|cl_efx.c R_StreakSplash 1781-1797|vel = dir·speed + rand(±random), life 0.1-0.5, gravity.|
|1038-1081|TE_DLIGHT, BLOODSTREAM, BLOOD, DECAL, WORLDDECAL|CLEAN (decode)|cl_tent.c 2086-2151|Protocol scale factors (×10, ×0.1) are interface.|
|1082-1091|TE_GUNSHOTDECAL|CONTAINS-XASH (minor)|cl_tent.c 2210-2220|Decal + impact particles + ricochet at rand < 0x3fff.|
|1092-1121|TE_MODEL, TE_BREAKMODEL|CLEAN (decode)|—|—|
|1122-1152|TE_SPRITE_SPRAY|CONTAINS-XASH|cl_tent.c R_Spray_Generic 1236-1299|SLOWGRAVITY + FADEOUT, fadespeed 2, renderfx NoDissipation, speed ×0.8-1.2, die 0.35, random frame.|
|1153-1195|TE_ARMOR_RICOCHET, TE_BLOODSPRITE, TE_PARTICLEBURST|CLEAN (decode / own burst)|—|The particle burst is our own simplification, unlike Xash R_ParticleBurst.|
|1196-1228|TE_MULTIGUNSHOT|CONTAINS-XASH|cl_tent.c R_MultiGunshot 1763-1809|Same two-sum gaussian x/y, dir + x·nx·right + y·ny·up, trace, impact + decal. Our deviations: no rejection loop, 2048 instead of 4096.|
|1229-1238|TE_GSFX_SPARKSHOWER, default|CLEAN|—|—|
|1241-1263|TentHit|CONTAINS-XASH (partial)|cl_tent.c CL_TempEntPlaySound 268-343|Volume 0.8 × speed/450. The sound-set mapping, chance and pitch range are ours.|
|1265-1388|TentsUpdate|HLSDK|hlsdk cl_dll/entity.cpp HUD_TempEntUpdate 446-725|Same sequence: SPRANIMATE frame advance/kill, FADEOUT → TransTexture, ROTATE, spark-shower think (−0.1, 0.1 s), collision damp ×0.5 / floor test normal>0.9 && vel≤0 && ≥gravity·3 → stop and clear flags, reflect −2·proj, mirror yaw, angles ×0.9, gravity / half gravity, smoke trail. Trace parity throttling is ours.|
|1392-1490|TentDraw, GSFXC_Frame, stats, Clear, Init|CLEAN / QUAKE-INTERFACE|—|Ours.|

Bindings:
- entity / client: CL_NewTempEntity, CL_AllocDlight / dlight_t, cl.time, cl.oldtime, r_refdef.vieworg, entity_t fields (rendermode / renderamt / renderfx / renderscale / body).
- model: model_t, mod_sprite / mod_studio, Mod_ForName, Mod_FindName, Mod_StudioPrefetch, Mod_StudioHeader, R_SpritePrefetch, R_DecalShoot, R_DecalClear, studiohdr_t, mstudiobodyparts_t.
- particles: R_AllocParticle, particle_t, pt_gsfx.
- msg: MSG_ReadByte / Short / Coord / Angle / String.
- server: SV_ClipMoveToEntity, sv.edicts, sv.active, sv_gravity.
- sound: S_PrecacheSound, S_StartSoundPitch, sfx_t.
- cvar / cmd / con: Cvar_RegisterVariable, Cmd_AddCommand, Con_Printf, Con_DPrintf.
- files: COM_OpenFile, COM_CloseFile, Sys_FileRead.
- host / sys: host_basepal, Host_Error, Sys_FloatTime.
- math: VectorMA, VectorNormalize, PerpendicularVector, CrossProduct, Length.

Hit dispositions:
- QCTRANS n 183-186 ← g_fielddefs.c 132-133: false positive (normalised numeric initialiser).
- XASH e 183-185 and 185-186 ← gl_rpart.c 25-35: real (tracer_colors data table).
- QUAKE n 184-186 ← anorms.h: false positive.
- XASH e 595-601, 601-606 ← cl_tent.c 1589-1602: real (Explosion dlight block).
- XASH e 687-688 ← cl_tent.c 966: real (BreakModel count formula).
- k16:
  - HLSDK e 33-35 ← in_camera.cpp: false positive (extern declaration).
  - XASH n 570-574, 658-662, 707-710 ← cl_efx.c 1737-1739: weak normalised matches, but the regions are real Xash translations (BloodSprite / TempModel / BreakModel); see the table.
  - QUAKE e 1048-1051 ← cl_tent.c 198-199: false positive (MSG_ReadByte run).
  - HLSDK n 1248-1258 ← monsters.cpp 1518: false positive (if/else chain shape).

##### n64/port/gsfx/gsfx_cl.c (2198 lines; git: untracked)
Verdicts: QUAKE-INTERFACE, CONTAINS-XASH, HLSDK. This is the SDL twin of gsfx_cart.c: same effect builders with fuller counts, plus fx blend, glow, beams, objects and studio events.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-73|header, externs, RF/RL, names, SpriteOf|CLEAN / QUAKE-INTERFACE|—|Ours. Lines 25-28 redefine VectorCopy as an expression macro (trivial).|
|75-205|client studio models (ReadWhole, FxModel), PaletteLoad|CLEAN / QUAKE-INTERFACE|—|Ours. Uses model_t, mod_studio, NL_PRESENT, studiohdr_t, IDSTUDIOHEADER, COM_OpenFile and host_basepal.|
|207-212|tracer_colors|CONTAINS-XASH (data)|xash ref/gl/gl_rpart.c 25-35|Exact data table.|
|214-292|SoundExists, sndsets, SoundPick, PlaySet, RicochetSound|CLEAN / QUAKE-INTERFACE|—|Ours.|
|294-374|attachments, EntIndex, GSFXC_EntityNum, EntPoint|CLEAN / QUAKE-INTERFACE|—|Ours. Entity | attachment<<12 is the HL encoding (interface).|
|386-478|GSFXC_FxBlend|CONTAINS-XASH|xash ref/gl/gl_rmain.c CL_FxBlend 1186-1298|Structural translation: index×363 offset, pulse 0x40/0x10 at ×2/×8, strobe ×4/16/36 with 20·sin<0 test, flicker (2,17)/(16,23), hologram/distort 180 / 100 / (1-(d-100)/400) + rand(-32..31), clamp. Fade/solid is rewritten time-based (409-430).|
|482-512|Occluded, GlowBlend|CONTAINS-XASH|xash ref/gl/gl_sprite.c R_SpriteGlowBlend (GLARE_FALLOFF 19000) 215-244|Same occlusion test ((1-frac)·dist>8), NoDissipation → 1, 19000/d² clamped 0.05..1, scale ×d/200.|
|516-530|FTENT_* defines|HLSDK interface|hlsdk common/r_efx.h 67 ff.|Exact values. Interface constants, used privately (can be renumbered as in the cart).|
|532-553|tent_t|CLEAN|—|Own struct.|
|555-588|TentAlloc|CONTAINS-XASH (partial)|cl_tent.c CL_PrepareTEnt 227-259|Same defaults as the cart version.|
|590-643|TempSprite, hlpart pool, PartAlloc, PartColor, Tracer|CLEAN|—|Ours.|
|646-662|SparkStreaks|CONTAINS-XASH|cl_efx.c R_SparkStreaks 1883-1900|As in the cart (here the 0.5 tracer size = ramp).|
|665-680|RicochetSprite|CONTAINS-XASH|cl_tent.c R_RicochetSprite 783-800|—|
|683-687|SparkEffect|CONTAINS-XASH|cl_tent.c R_SparkEffect 1443|—|
|690-718|BulletImpact|CONTAINS-XASH|cl_efx.c R_BulletImpactParticles 1718-1748|—|
|720-732|PaletteParticles|CLEAN|—|—|
|735-762|BloodStream|CONTAINS-XASH|cl_efx.c R_BloodStream 1244-1310|Close structural translation (the 0.00001 accel decrement is retained).|
|765-786|Blood|CONTAINS-XASH (partial)|cl_efx.c R_Blood 1208-1235|—|
|789-844|Explosion + flicker particles|CONTAINS-XASH|cl_tent.c R_Explosion 1579-1622, R_Sprite_Explode 1190-1212, cl_efx.c R_FlickerParticles 1756|—|
|847-864|Smoke|CONTAINS-XASH|cl_tent.c R_Sprite_Smoke 1221-1234|—|
|866-897|tbeam pool, BeamAlloc|CLEAN|—|—|
|901-945|BeamTail|CONTAINS-XASH (partial)|cl_efx.c R_BeamSetup 559-588, CL_ParseViewBeam 655 ff.|The protocol decode is interface. Field initialisation (freq = speed·cl.time, die = time+life, cylinder byte width) follows Xash.|
|947-1064|cobj_t, ReadVec, ReadObj, GSFXC_ParseFX|CLEAN / QUAKE-INTERFACE|—|Our own protocol.|
|1069-1113|BloodSprite|CONTAINS-XASH|cl_tent.c R_BloodSprite 872-944|Here 14 drops (GoldSrc count).|
|1116-1134|TempModel|CONTAINS-XASH|cl_tent.c R_TempModel 1040-1082|—|
|1137-1183|BreakModel|CONTAINS-XASH|cl_tent.c R_BreakModel 953-1031|—|
|1185-1201|SparkShower|HLSDK|hlsdk dlls/explode.cpp CShower 39-71|—|
|1203-1297|TraceDecal; ParseTEnt beams / lightning / killbeam / gunshot / explosion / smoke|CLEAN (decode)|—|The TE_LIGHTNING setup (white, brightness 1) is minor.|
|1298-1314|TE_TRACER|CONTAINS-XASH (minor)|cl_efx.c R_TracerEffect 1823|—|
|1315-1318|TE_SPARKS|CLEAN|—|—|
|1319-1352|TE_SPRITE, TE_GLOWSPRITE|CONTAINS-XASH (minor)|cl_tent.c 2050-2072|—|
|1353-1372|TE_STREAK_SPLASH|CONTAINS-XASH|cl_efx.c R_StreakSplash 1781|—|
|1373-1421|TE_DLIGHT, BLOODSTREAM, BLOOD, DECAL, WORLDDECAL|CLEAN (decode)|—|—|
|1422-1436|TE_GUNSHOTDECAL|CONTAINS-XASH (minor)|cl_tent.c 2210-2220|—|
|1437-1464|TE_MODEL, TE_BREAKMODEL|CLEAN (decode)|—|—|
|1465-1497|TE_SPRITE_SPRAY|CONTAINS-XASH|cl_tent.c R_Spray_Generic 1236-1299|—|
|1498-1532|TE_ARMOR_RICOCHET, TE_BLOODSPRITE, TE_PARTICLEBURST|CLEAN|—|The burst is ours.|
|1533-1565|TE_MULTIGUNSHOT|CONTAINS-XASH|cl_tent.c R_MultiGunshot 1763-1809|—|
|1566-1574|TE_GSFX_SPARKSHOWER, default|CLEAN|—|—|
|1578-1583|GSFXC_Gunshot|CONTAINS-XASH (minor)|cl_tent.c 2217-2220|Ricochet chance.|
|1585-1619|evstate, SprAdd|CLEAN|—|—|
|1621-1648|MuzzleFlash|CONTAINS-XASH|cl_tent.c R_MuzzleFlash 835-862|index = type%10, scale = (type/10)·0.1 (default 0.5), random frame, roll 0-20 for index 0 else 0-359, TransAdd.|
|1650-1668|StudioEvent|HLSDK|hlsdk cl_dll/entity.cpp HUD_StudioEvent 335-373|Same event switch: 5001/5011/5021/5031 → muzzle flash on attachment n, 5002 → spark effect ±100.|
|1670-1724|GSFXC_StudioEvents|CLEAN (behaviour)|cf. xash gl_studio.c R_StudioClientEvents 2433-2480|Own state machine. Only the −0.01 sequence-start convention is shared.|
|1730-1753|TentHit|CONTAINS-XASH (partial)|cl_tent.c CL_TempEntPlaySound 268-343|—|
|1755-1884|TentsUpdate|HLSDK|hlsdk cl_dll/entity.cpp HUD_TempEntUpdate 446-725|Same as the cart version.|
|1886-2196|PartsUpdate, SpriteFromTent, ObjSprite, ObjBeam, BeamsUpdate, GSFXC_Frame, lists, Clear, Init|CLEAN / QUAKE-INTERFACE|—|Ours. They call the Xash-derived FxBlend/GlowBlend.|

Bindings:
- entity / client: cl_entities, cl.viewent, cl.viewentity, cl.num_entities, cl.mtime, cl.time, cl_visedicts / cl_numvisedicts, CL_AllocDlight / dlight_t, entity_t render fields, r_refdef.vieworg, vpn.
- model: model_t, mod_studio, NL_PRESENT, studiohdr_t, mstudioseqdesc_t, mstudioevent_t, mstudiobodyparts_t, IDSTUDIOHEADER.
- msg: MSG_Read*.
- server: SV_Move / MOVE_NOMONSTERS, sv.active, sv_gravity.
- sound: S_PrecacheSound, S_StartSoundPitch.
- files: COM_OpenFile, COM_CloseFile, Sys_FileRead.
- con / sys / host: Con_Printf, Host_Error, Sys_Error, host_basepal.
- math: VectorMA, DotProduct, CrossProduct, PerpendicularVector, Length.
- renderer: GLR_DecalShoot, GLR_DecalClear.

Hit dispositions:
- XASH e 27-28 ← xash3d_mathlib.h 103-104: false positive (generic VectorCopy macro).
- QCTRANS n 208-211, QUAKE n 209-211: false positives.
- XASH e 208-211 ← gl_rpart.c: real (tracer_colors).
- LIBDRAGON n 247-252 and QUAKE n 247-253 ← menu.c: false positives (struct-array initialiser shape).
- HLSDK e and XASH e 516-526 ← r_efx.h FTENT_*: real, interface constants.
- XASH e 808-819 ← cl_tent.c 1589-1602: real (Explosion).
- XASH e 1146-1147 ← cl_tent.c 966: real (BreakModel).
- k16:
  - XASH e 459-461 ← gl_rmain.c 1284-1285: real (FxBlend hologram).
  - QUAKE n 623-626, 1081-1085, 1102-1106 ← sv_phys.c 927-930: false positives (assignment runs).
  - XASH n 1106-1110, 1126-1130, 1166-1169 ← cl_efx.c 1737-1739: regions are real Xash translations (BloodSprite / TempModel / BreakModel); the matched span itself is weak.
  - QUAKE e 1383-1386 ← cl_tent.c 198-199: false positive.
  - HLSDK n 1737-1747 ← monsters.cpp: false positive.

##### n64/port/gsfx/gsfx_spr.c (260 lines; git: untracked)
Verdicts: CLEAN (mostly), CONTAINS-QUAKE (minor).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-13|header (SPR v2 format description)|CLEAN|—|Format description is ours.|
|14-59|cursor reader Int/Float|CLEAN|—|Ours (bounds-checked).|
|62-84|Frame|CLEAN, except 70-73|quake model.c Mod_LoadSpriteFrame 1669-1700 (up/down/left/right at 1694-1697)|The 4-line origin→up/down/left/right mapping matches Quake (cited in gsfx.h 320-323). It is file-format semantics, but expressed identically.|
|86-212|FreeFrames, Parse, GSFX_SpriteForName|CLEAN|cf. quake model.c Mod_LoadSpriteModel / Mod_LoadSpriteGroup 1726-1760|Own parser. The "interval ≤ 0 rejected" rule matches Quake behaviour only.|
|219-242|GSFX_SpriteFrame|CONTAINS-QUAKE (minor)|quake r_sprite.c R_GetSpriteframe 236-275 (group branch 257-274)|Same group selection: fullinterval = last cumulative interval, target = t − int(t/full)·full, first interval > target. The frame clamp/wrap differs.|
|244-258|GSFX_SpriteContextLost|CLEAN|—|—|

Bindings: Q_strcasecmp, Q_strncpy, LittleLong / LittleShort / LittleFloat, Con_Printf, qboolean, and GLR_Alloc / GLR_LoadFile (our renderer).
Hit dispositions: no automated hits. The structural finding comes from manual review.

##### n64/port/gsfx/gsfx_sv.c (1617 lines; git: untracked)
Verdicts: QUAKE-INTERFACE, HLSDK.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-37|header, allocator externs|CLEAN|—|—|
|39-52|SF_BEAM_* / SF_SPRITE_* / DMG_ENERGYBEAM|HLSDK interface|hlsdk dlls/effects.h, cbase.h|Spawnflag values are needed for map data.|
|54-138|config, name table (FNV hash, GSFX_Name), sv_client_t|CLEAN / QUAKE-INTERFACE|—|Ours. Uses sv.datagram and MSG_Write*.|
|140-298|SpriteFrames, object store, ObjSame/Create/Update/Kill/Get, OwnerGone, Trace|CLEAN / QUAKE-INTERFACE|—|Ours. Uses COM_OpenFile, Sys_FileRead, EDICT_NUM, edict freetime and SV_Move.|
|302-333|DecalAdd|HLSDK|hlsdk effects.cpp CDecal::StaticDecal / TriggerDecal|±5 diagonal trace (ignore_monsters) picks the carrying entity.|
|335-419|sidecar structs, Persist, CopyStr, ParseVec|CLEAN|—|—|
|422-493|SetKey|HLSDK (key names / KeyValue semantics)|hlsdk effects.cpp CLightning/CLaser/CSprite::KeyValue; the "angle" −1/−2 rule as in xash sv_game.c 5005|Key-name mapping (interface); the "angle" rule is documented behaviour.|
|496-568|LoadSidecar|CLEAN / QUAKE-INTERFACE|—|Ours. Uses COM_Parse / com_token.|
|570-626|Ends, RandomEnd|HLSDK (581-626)|effects.cpp CBeam::RandomTargetname 339-353|RandomEnd is the HLSDK random-of-matches. Ends' resident/HLE lookup is ours.|
|629-668|ShouldToggle, DoSparks, BeamDamage|HLSDK|cbase ShouldToggle; effects.cpp CBeam::DoSparks 355, CBeam::BeamDamage 745-765|Same logic. Decal {bigshot1-5}, DMG_ENERGYBEAM.|
|672-697|BaseObj, EntObj, ObjSet|CLEAN|—|—|
|703-744|SpriteObj, SpriteTurn, SpriteOnce|HLSDK|effects.cpp CSprite::TurnOn 1317, CSprite::Animate 1292, CGlow::Spawn 1138|Animate condition (framerate && maxframe>1 \|\| ONCE); once → off past the last frame.|
|749-817|BeamStatic, BeamEnds, BeamObj|HLSDK|effects.cpp CLightning::Spawn 458 (ServerSide), BeamUpdateVars 884-946|Point-end swap rule, shade flags from spawnflags.|
|820-950|BeamZap, RandomDir, BeamRandomArea, BeamRandomPoint, BeamStrike|HLSDK|effects.cpp CLightning::Zap, RandomArea 818-857, RandomPoint 860-880, StrikeThink 639-743|Close structural translations (10 tries, dot>0 retry, radius·0.1 rejects, restrike timing, ring rule, BeamDamageInstant).|
|953-997|BeamDamageThink, BeamUse|HLSDK|effects.cpp CLightning::DamageThink 767, ToggleUse 583, StrikeUse 607|—|
|1001-1066|LaserObj, LaserEnd, LaserTurn, LaserThink|HLSDK|effects.cpp CLaser::Spawn 960, TurnOn 1061, Use 1071, StrikeThink 1098|—|
|1070-1123|EntSpawn|HLSDK|CSprite::Spawn 1182 (yaw → roll), CLightning::Spawn, CLaser::Spawn|—|
|1125-1243|EntSave, EntRestore, MapOpened, MapClosed|CLEAN|—|Our persistence design.|
|1247-1285|GSFX_SV_Fire|HLSDK (dispatch)|CSprite::Use 1330, CLaser::Use 1071, CDecal::TriggerDecal|ShouldToggle dispatch.|
|1287-1334|GSFX_SV_Render|HLSDK (env_render semantics)|hlsdk triggers / effects CRenderFxManager::Use (SF_RENDER_MASK*)|—|
|1340-1616|TmpBuf, Put, WriteObj, Flush, ClientSpawn, NewMap, SV_Frame, SV_Init|CLEAN / QUAKE-INTERFACE|—|Ours. Init lists HL asset names (content).|

Bindings:
- server: sv.datagram, sv.time, sv.edicts, sv.num_edicts, sv.active, svs.clients, svs.maxclients, client_t / cl->message, SV_Move, MOVE_NOMONSTERS / MOVE_NORMAL, SOLID_BSP.
- edict: edict_t, ->free, ->freetime, e->v.origin / modelindex / takedamage / solid, EDICT_NUM, NUM_FOR_EDICT, T_Damage, g_damage_type, G_ClassIs, HLE_* (our HLE layer).
- msg: MSG_WriteByte / Short / Coord / Float / Angle / String, SZ_Write, sizebuf_t, svc_hlfx.
- files: COM_OpenFile, COM_CloseFile, COM_Parse, com_token, Sys_FileRead.
- con: Con_DPrintf, Con_Printf.
- misc: USE_*, SF_RENDER_MASK*, MAX_SCOREBOARD, MAX_QPATH.

Hit dispositions:
- QCTRANS e 190-193 ← g_local.h 57-59 (VecSame): false positive. A trivial three-component equality; independent of the QC translation.
- QCTRANS n 320-326 ← g_doors.c 243: false positive (pos±5 assignments; the code is CDecal's ±5 box).
- LIBDRAGON n and QUAKE n 1577-1593 ← exception.c / menu.c: false positives (string-array shape).
- k16:
  - XASH n 431-435 ← sv_save.c 1726: false positive (strcmp key chain).
  - QUAKE n 1380-1385 ← quake2 common.c 653: false positive.
  - HLSDK n 1595-1598 ← client.cpp 1356: false positive (struct-array shape).

##### n64/port/gsfx/gsfx_te.c (1045 lines; git: untracked)
Verdicts: QUAKE-INTERFACE, CONTAINS-QUAKE, HLSDK.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-24|header, externs|CLEAN|—|—|
|26-36|MAT_* enum, SF_ENVEXPLOSION_*|HLSDK interface|hlsdk func_break.h Materials; explode.h|Values are needed for map data.|
|38-388|RNG wrappers, te_begin / te_vec / te_byte / te_tenths / te_name / te_beamtail, all GSFX_TE_* writers|CLEAN / QUAKE-INTERFACE|—|Ours. Payload order is the HLSDK const.h protocol (interface).|
|390-473|timed queue, beam_owned, GSFX_TE_Frame|CLEAN|—|Ours.|
|479-518|te_trace|CONTAINS-QUAKE|first/source/world.c SV_Move 923-961 + SV_ClipToLinks 814-885 (merge block 861-874)|Structural translation: world clip first, then per-entity SV_ClipMoveToEntity with the identical allsolid/startsolid/fraction merge, including the "keep startsolid" sub-case. Box by start/end ±1 (SV_MoveBounds 893-916).|
|520-524|GSFX_Trace|CLEAN|—|—|
|526-538|GSFX_TraceFromGlobals|QUAKE-INTERFACE|quake pr_cmds.c PF_traceline (inverse)|Reads pr_global_struct->trace_* and PROG_TO_EDICT.|
|540-587|decal name tables, te_classes|HLSDK|hlsdk monster Spawn m_bloodColor, HasHumanGibs / HasAlienGibs (combat.cpp 256 ff.)|Class → blood / gib table from HLSDK class behaviour (data).|
|589-613|te_class, GSFX_BloodColor|HLSDK|CBaseEntity / CBaseMonster::BloodColor|—|
|615-629|GSFX_SpawnBlood|HLSDK|util.cpp UTIL_BloodDrips 1166-1200 (clamp 255, size clamp 3..16 of amount/10)|—|
|631-656|te_is_bsp, GSFX_DecalTrace, te_blood_decal|HLSDK|util.cpp UTIL_DecalTrace / UTIL_BloodDecalTrace|—|
|658-693|GSFX_TraceBleed|HLSDK|combat.cpp CBaseEntity::TraceBleed|Noise / count tiers <10 / <25, −dir jitter, 172-unit trace.|
|695-718|GSFX_DamageDecal|HLSDK|CBaseEntity::DamageDecal / CBreakable::DamageDecal|TransAlpha → none, non-normal → bproof, shot1-5.|
|720-760|GSFX_BulletHit|HLSDK|combat.cpp DecalGunshot / CBaseMonster::TraceAttack|—|
|766-799|GSFX_Tracer, GSFX_EjectBrass, GSFX_PlayerBrass|HLSDK|combat.cpp FireBullets tracer count; weapons.cpp EjectBrass; ev_hldm EV_GetDefaultShellInfo (rand 50-70 / 100-150, forward 25)|—|
|804-866|te_spark_showers, te_scorch, GSFX_GrenadeExplode, GSFX_EnvExplosion|HLSDK|ggrenade.cpp CGrenade::Explode 51-128 / Smoke 129; explode.cpp CEnvExplosion::Use 150 / Smoke 235|(dmg−50)·0.6 / ·0.8, framerate 15/12, WXplo1 under water, smoke after 0.3 s, 0-3 spark showers, scorch decals.|
|870-914|GSFX_GibMonster|HLSDK|combat.cpp GibMonster 300-330, CGib::SpawnRandomGibs / SpawnHeadGib, m_lifeTime 25 (799)|Health −50/−200 velocity tiers.|
|916-952|GSFX_BreakModel|HLSDK|func_break.cpp CBreakable::Die 582 ff.|Material → shard model / BREAK flags, attackdir×200.|
|957-1043|GSFX_BeamObj*, runtime beams|HLSDK (957-990) / CLEAN|effects.cpp CBeam::PointEntInit|The object plumbing (992-1043) is ours.|

Bindings:
- server / edict: sv.datagram, sv.edicts, sv.time, edict_t, e->v.{classname, flags, solid, takedamage, size, absmin, absmax, health, origin, velocity, view_ofs, v_angle}, pr_strings, pr_global_struct->trace_*, PROG_TO_EDICT, NUM_FOR_EDICT, SV_ClipMoveToEntity, SV_AreaEdicts, SV_PointContents, CONTENTS_WATER, SOLID_BSP, FL_MONSTER / FL_CLIENT.
- msg: MSG_Write*, MSG_COORD_BYTES, svc_hltent.
- cvar / con: Cvar_VariableValue, Con_Printf.
- math: AngleVectors, VectorMA, VectorNormalize.
- ours: HL_RenderGet, HL_BreakableMaterial.

Hit dispositions:
- XASH n 560-579 ← in_keys.c 58-60: false positive (struct-table shape).
- k16 HLSDK e 21-24 ← in_camera.cpp: false positive (extern prototype).
- The te_trace Quake derivation was found by structural review (no token hit).

##### n64/port/gsfx/gsfx_util.c (56 lines; git: untracked)
Verdict: CLEAN.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|9-33|GSFX_Rand (xorshift32 13/17/5), RandFloat, RandLong|CLEAN|Marsaglia xorshift (public algorithm)|Ours.|
|35-56|GSFX_AssetPath|CLEAN|—|Ours.|

Bindings: Q_strncasecmp, plus quakedef.h for its types. Hit dispositions: none.

##### n64/port/snd_n64.c (1862 lines; git: da63eca, then 8942704, modified in worktree)
Verdicts: QUAKE-INTERFACE; CLEAN expression; two borderline Quake items (not counted as contamination).
History: `git log -p` shows the file was created in da63eca as a new 717-line libdragon backend replacing snd_null, with no Id header. Header and design are ours from the first commit. It was never a copy of snd_dma.c / snd_mix.c / snd_mem.c.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-101|header, config|CLEAN|—|Ours.|
|105-106|bgmvolume / volume cvar_t|QUAKE-INTERFACE (exact 2-line copy)|first/source/snd_dma.c 71-72|Engine-required globals (menu.c), copied with names and defaults.|
|108-201|n64_sfx_t / n64_chan_t / loops / registry state|QUAKE-INTERFACE|quake sound.h sfx_t / channel_t|Own structs. Field vocabulary entnum / entchannel / origin / master_vol / dist_mult follows Quake channel_t; listener_* globals are declared extern in sound.h.|
|203-404|profiling, PumpProf, Pump, QueuedMs|CLEAN (LIBDRAGON note)|libdragon src/audio/mixer.c mixer_try_play|The PumpProf loop re-expresses mixer_try_play's 5-line body (Unlicense); the rest is ours.|
|406-608|N64_SoundPath, wave open / LRU / reclaim / EnsureOpen|CLEAN|—|Ours. Uses the libdragon wav64 API (WAV64_STREAMING_FULL).|
|611-667|N64_Spatialize|Borderline QUAKE (behaviour model; expression rewritten)|first/source/snd_dma.c SND_Spatialize 398-450, S_StartDynamicSound 478 (dist_mult = attenuation / sound_nominal_clip_dist)|Same linear attenuation 1 − dist·(atten/1000) and dot with listener_right. The pan law (0.5±0.5·dot), clamping and no-attenuation path are our own. Recommend re-expressing from the GoldSrc/behaviour spec with new identifiers.|
|669-834|SetPitch, FreeVoice, StopEntChannel, FreeChannel, PickChannel, PlayOnChannel|CLEAN|cf. snd_dma.c SND_PickChannel 354-396|Own voice policy (free → oldest one-shot by LRU → loop voice). Only the (entnum, entchannel) override semantics match Quake/GoldSrc behaviour.|
|836-943|loops: LoopUpdate, StartLoop, N64_Start|CLEAN|cf. snd_dma.c static/ambient channels|Own virtualised-loop design. No Quake ambient (S_UpdateAmbientSounds) code.|
|945-1149|registry (djb2 hash), FindName, FreeName, ReportNameUnion, RegisterSfx, RegisterWord|CLEAN / QUAKE-INTERFACE|—|Ours. Uses cl.sound_precache, sv.sound_precache and MAX_SOUNDS.|
|1151-1185|N64_SentenceAdvance|CLEAN|cf. xash s_vox.c|Word sequencing over our HL_SentenceWord API. No s_vox.c parsing here (VOX parse lives in hl_sentences.c, outside this slice).|
|1187-1309|snd_names, VoiceLimits, snd_bench|CLEAN|—|Ours.|
|1315-1398|S_Init, S_Startup, S_Shutdown|QUAKE-INTERFACE|snd_dma.c S_Init 167 (-nosound check)|API shape only.|
|1400-1488|S_Ambient*, S_TouchSound, S_ClearBuffer (stubs), SfxOnCart, S_StaticSound|CLEAN / QUAKE-INTERFACE|snd_dma.c S_StaticSound 620|Own deferred-open loop design.|
|1490-1600|S_StartSoundPitch, S_StartSound, S_StartSentence, S_ChangeSound, S_StopSound, S_PrecacheSound, S_PrecacheMapSound, S_ClearPrecache|QUAKE-INTERFACE|—|Own bodies.|
|1602-1757|FreeMapNames, Begin/EndPrecaching, CollectUnusedSounds, WorldMapClosing, SfxNames*|CLEAN|—|Ours.|
|1759-1832|S_Update|CLEAN / QUAKE-INTERFACE|cf. snd_dma.c S_Update|Own (per-voice respatialise, sentence advance, loop update, pump). No mixing / paint code.|
|1834-1849|S_StopAllSounds, S_ExtraUpdate|CLEAN|—|—|
|1850-1862|S_LocalSound|QUAKE-INTERFACE (near-identical 3-statement body)|snd_dma.c S_LocalSound 985-1000|Precache then start on cl.viewentity, channel −1, vec3_origin (our attenuation is 0). Trivial API glue.|

Bindings:
- S_* API (sound.h): S_Init, S_Startup, S_Shutdown, S_StartSound, S_StartSoundPitch, S_StaticSound, S_StopSound, S_StopAllSounds, S_ChangeSound, S_PrecacheSound, S_PrecacheMapSound, S_Begin / EndPrecaching, S_ClearPrecache, S_CollectUnusedSounds, S_Update, S_ExtraUpdate, S_LocalSound, S_AmbientOn / Off, S_TouchSound, S_ClearBuffer, S_StartSentence, S_WorldMapClosing; plus sfx_t, listener_*, snd_initialized, snd_blocked.
- cvar / cmd: cvar_t, Cvar_RegisterVariable, Cmd_AddCommand, Cmd_Argc, Cmd_Argv, COM_CheckParm.
- con / sys: Con_Printf, Con_DPrintf, Sys_Printf, Sys_Error, Sys_FloatTime.
- client / server: cl.viewentity, cl.sound_precache, sv.active, sv.sound_precache, MAX_SOUNDS, MAX_QPATH, MAX_OSPATH.
- misc: Q_strcasecmp, Q_strncpy, VectorCopy, VectorSubtract, DotProduct.
- ours: HL_SentenceWord, HL_SentenceNumWords.

VADPCM / wav64 provenance:
- No codec code in this file.
- VADPCM (and Opus, `-DN64_SND_OPUS`) decoding is libdragon's wav64 / RSP mixer (Unlicense).
- Samples are produced offline by libdragon audioconv64 (tools/convert_hl_sounds.sh, outside this slice).
- The header probe reads the wav64 format byte (header[5]); that layout is the libdragon interface.

Hit dispositions:
- QUAKE e 105-106 ← snd_dma.c 71: real (cvar declarations; interface).
- QUAKE n 1337-1341 ← cl_main.c 72-75: false positive (memset run).

##### n64/port/host/q64_host_audio.c (131 lines; git: untracked)
Verdict: CLEAN.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-131|libdragon audio / mixer / wav64 API stubs on the realtime clock, wav64 header read|CLEAN|libdragon audio.h / mixer.h / wav64.h API; wav64_internal.h header layout|API implementation and format reading only (Unlicense interface).|

Bindings: Sys_Error, realtime, plus quakedef.h for its types. Hit dispositions: none.

##### n64/port/sdl/sdl_audio.c (702 lines; git: untracked)
Verdicts: CLEAN, LIBDRAGON-derived (Unlicense, MIT-compatible).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-75|header, types, state|CLEAN|—|The header credits the libdragon port (rev bc24d3d5a7d2).|
|77-147|mix_callback|CLEAN|—|Own linear-interpolation float mixer. Nothing from Quake snd_mix.c (no paintbuffer / S_PaintChannels / transfer), and nothing from Xash s_mix.c.|
|149-317|SDL device, libdragon mixer API (mixer_ch_*)|CLEAN|libdragon mixer.h API|Ours.|
|319-340|be32, be16s, vadpcm_ext4, clamp16|LIBDRAGON (329-340)|libdragon wav64_vadpcm.c vadpcm_ext4 46-48, vadpcm_clamp16 51-56|Port.|
|343-387|vadpcm_frame|LIBDRAGON|wav64_vadpcm.c vadpcm_decode 59-120 (reference decoder)|Port of the per-frame body.|
|392-461|huff_tables, huff_expand|LIBDRAGON|wav64_vadpcm.c wav64_vadpcm_init_huffman 382, huffv_decompress 189|Port.|
|463-597|read_file, wav64_decode|CLEAN (LIBDRAGON format interface)|libdragon wav64 header / ext header layout|Ours.|
|599-702|wav64_load, set_loop, close, SDLA_Stats|CLEAN|—|Ours.|

Bindings: Sys_Printf and Sys_Error, plus quakedef.h for its types. libdragon types: waveform_t, wav64_t.
Hit dispositions: no automated hits. The libdragon origin is self-declared in the header and confirmed against ~/sdks/libdragon/src/audio/wav64_vadpcm.c. Provenance note: libdragon's vadpcm reference decoder is itself modelled on Dietrich Epp's vadpcm codec, which is MIT. Both licences are MIT-compatible, so keep an attribution comment.


### A.AuditGameA — Game A (AI, monsters, nodes, scripted, sentences, sounds)


Scope: 14 files in `~/src/ports/quakegeneric/source/hl/`, audited as they are now in the working tree. Reference trees: `first/source` (Quake), `qctrans` (our QuakeC translations), `xash/engine`, `hlsdk/dlls`. All ref paths are relative to `/scratch/quakegeneric/tmp/ProvAudit/`. No code from any source appears in this report.

None of these 14 files carries an "Id Software" or GPL header. All of them are bound to the old tree's QuakeC-edict game API. The HLSDK-derived files carry no HLSDK/Valve licence notice.

Throughout this report, "QCTRANS-API" means calls into our QuakeC-translation code (`source/g_*.c`, which is Quake-derived and forbidden). It includes the `G_SELF`/`G_OTHER`/`G_TIME`/`G_Func`/`G_Call`/`G_Str`/`G_IsWorld`/`G_ClassIs`/`G_SetStr` macros and `g_*` helpers from `g_local.h`/`g_api.c`, and the AI routines `ai_run`, `ai_stand`, `ai_face`, `walkmonster_start`, `FoundTarget`, `M_Visible`, `M_Infront`, `monster_use`, `T_Damage`, `SUB_*`, `ThrowGib`, `g_movetogoal`, `g_walkmove`, `g_changeyaw`, `g_checkclient`, `g_droptofloor`, `g_find`, `g_findradius`, `g_particle` and `g_ambientsound`. The new engine must not import these. Every call site must be re-targeted to new-engine services.

#### 1. Summary


Slice totals (approximate): Quake 332 lines (283 in hl_nodecook.h), Xash 45 distinct (hl_nodecook.h 11, plus the dual-attributed drop-to-floor lines), QCtrans 159 (mostly hl_ai.c), HLSDK ≈5,100. Every .c file depends heavily on QCTRANS-API.

---

##### source/hl/hl_ai.c (1736 lines; git: 8ca78f4, modified in worktree)

Verdicts: HLSDK; CONTAINS-QCTRANS; QUAKE-INTERFACE.

The header (1-8) states that the file mirrors the Quake walkmonster pattern. The hostile think chain delegates chasing and attacking to qctrans `ai_run` (855) and starts through `walkmonster_start` (1674). The talk-monster half (1007-1544) is a C translation of the HLSDK CTalkMonster, CBarney and CScientist logic.

| lines | function/block | verdict | counterpart file:function(lines) | basis |
|---|---|---|---|---|
|34-212|hl_rt_t/hl_def_t class tables|CLEAN (data)|—|Our tables. They hold HL model sequence names and hulls/healths from HLSDK Spawn/skill (facts).|
|214-345|HL_ClassModelAdd, HL_ClassShooterModel, Game_ClassModels|CLEAN|—|Ours. The table hit is a false positive.|
|348-426|HL_AI_FindSeq, HL_Hdr, HL_AI_LookupSeq, HL_AI_SeqDesc|CLEAN|hlsdk/dlls/animation.cpp LookupSequence|Independent; implements documented MDL lookup.|
|428-440|HL_AI_GroundSpeed|HLSDK|hlsdk/dlls/animation.cpp GetSequenceInfo(226-)|Ground-speed formula (behaviour).|
|442-483|hl_mdlevent_t, HL_EventSample, HL_AI_PrecacheSeqSounds|CLEAN|—|MDL format.|
|485-519|HL_AnimEvent|HLSDK|hlsdk/dlls/monsters.cpp CBaseMonster::HandleAnimEvent(2630-)|Structural translation: script event ids and the 25% SENTENCE_RND1 rule.|
|521-668|SeqEvents, SetSeq, HL_RT, HL_Resolve, IsHLMonster, ActivitySeq, IdleSeq/MoveSeq|CLEAN|—|Ours (ActivitySeq is a simplified LookupActivity).|
|670-730|HL_AI_StepToward|CLEAN + QUAKE-INTERFACE|—|Own deflection walker. Relies on Quake SV_movestep semantics (FL_PARTIALGROUND, STEPSIZE 18).|
|746-759|HL_Dt, HL_ApplyView|CONTAINS-QCTRANS|qctrans/g_ai.c walkmonster_start_go(541-590)|Same view_ofs reset (hit 756-758).|
|761-797|HL_Waiting, HL_WaitLook, hl_monster_use|HLSDK|hlsdk/dlls/monsters.cpp StartMonster(2065-) / MonsterUse|SCHED_WAIT_TRIGGER behaviour.|
|799-868|hl_stand_think, hl_walk_think, hl_run_think, hl_back_to_run|CONTAINS-QCTRANS|qctrans/g_ai.c mon_stand_think+ai_stand(497-506, 258-269), mon_walk_think+ai_walk(508-517, 250-256), mon_run_think(519-528), mon_back_to_run(530-538)|Structural translation with renamed calls: set frames, re-arm think, look-or-pausetime→th_walk, movedist+movetogoal, run via ai_run. HL Look replaces FindTarget.|
|870-917|hl_die, hl_die_think|CONTAINS-QCTRANS|qctrans/g_monsters.c Mon_BeginDie(210-237), mon_die_think(186-208)|Same field sequence and termination block (hits 910-916).|
|919-961|hl_pain|CLEAN + QUAKE-INTERFACE|—|Own. Uses the Quake pain_finished/th_* idiom and HLSDK AI-trigger semantics.|
|963-1005|hl_idle_think, HL_AI_MoveEvents|CLEAN|—|Ours.|
|1007-1058|talk constants, m_szGrp tables, talk_group|HLSDK|hlsdk/dlls/scientist.cpp / barney.cpp TalkInit; talkmonster.cpp Precache|Group tables and the UseSentence/UnUseSentence keys.|
|1060-1076|HL_TraceCvar/HL_Trace|CLEAN|—|—|
|1078-1164|talk_ok_to_speak, talk_say, HL_AI_IsTalking, HL_AI_Talk, talk_leader, talk_stop/start_following|HLSDK|talkmonster.cpp FOkToSpeak(913-), PlaySentence(1151-), StopFollowing(1364-), StartFollowing(1385-)|Structural translation.|
|1166-1196|hl_talk_use|HLSDK|talkmonster.cpp FollowerUse(1416-1446)|Same branch order. The console string is reproduced verbatim from HLSDK.|
|1198-1271|HL_Dist2D, talk_look, talk_enemy, barney_shoot|HLSDK|barney.cpp BarneyFirePistol(343-)|Translation: origin+55 muzzle, 2° cone, 1024 range. The EF_MUZZLEFLASH idiom is a Quake binding.|
|1273-1374|barney_fight, sci_flee, talk_follow|HLSDK|barney.cpp slBarneyRangeAttack / CheckRangeAttack1; talkmonster.cpp slFollow / TASK_MOVE_TO_TARGET_RANGE|Behaviour plus HLSDK constants (128/190/270). Our own control flow.|
|1376-1444|hl_talk_think|HLSDK|talkmonster.cpp / scientist.cpp schedule selection|Own scheduler carrying HLSDK semantics.|
|1446-1505|hl_talk_pain|HLSDK|barney.cpp TakeDamage(509-547); scientist.cpp TakeDamage(771-)|Structural translation: suspicious/IsFacing → BA_MAD vs BA_SHOT.|
|1507-1544|HL_AI_CanPlaySequence, HL_AI_ScriptTaken|HLSDK|monsters.cpp CanPlaySequence(3110-)|Translation.|
|1546-1557|hl_npc_start|HLSDK (also QCTRANS pattern)|monsters.cpp StartMonster; qctrans/g_ai.c walkmonster_start_go|Raise one unit, then drop to floor.|
|1559-1610|hl_flee_think, hl_run_think_sight|CLEAN|—|Ours.|
|1612-1734|HL_AI_Start|CLEAN + QUAKE-INTERFACE (1646-1649 QCTRANS-trivial)|qctrans/g_ai.c walkmonster_start_go (yaw_speed default, view_ofs)|Own spawn dispatcher.|

Quake / QCTRANS bindings:
- **QuakeC entvars (v.\*):** skin (as sequence index), frame, walkframe, lip (class id), aflag, state, ideal_yaw, yaw_speed, view_ofs, th_stand/th_walk/th_run/th_die/th_pain/th_melee/th_missile, think, nextthink, use, touch, enemy, oldenemy, goalentity, pausetime, pain_finished, attack_finished, attack_state, search_time, fly_sound, lefty, dest2, deadflag, takedamage, solid, movetype, flags, spawnflags, health, modelindex, model, classname, targetname, v_angle, effects, velocity.
- **pr_global_struct:** deathmatch, total_monsters, trace_fraction, trace_ent, v_forward.
- **Edict glue:** PROG_TO_EDICT, EDICT_TO_PROG, NUM_FOR_EDICT, ED_Free.
- **Server/model:** sv.models, sv.state, sv.edicts, SV_FindSound, Mod_StudioHeader, Mod_FindName.
- **Cvar/Con:** Cvar_FindVar, Con_Printf, Con_DPrintf.
- **QCTRANS-API:** ai_run, walkmonster_start, g_movetogoal/g_movedist, g_walkmove, g_changeyaw, g_checkclient, M_Visible, M_Infront, g_droptofloor, g_traceline, g_makevectors, g_vectoyaw, g_normalize, g_vlen, g_random, g_setmodel/g_setsize/g_setorigin/g_precache_model, T_Damage, SUB_Null, AS_STRAIGHT, anglemod.

Automated hits:
- 268-282 QCTRANS/QUAKE/XASH/HLSDK, n mode: false positive (classname/type initialiser table shape).
- 733-746 QCTRANS, n mode, vs g_doors.c: false positive (forward declarations).
- 756-758 QCTRANS, e mode: real but trivial. Included in the QCTRANS region 746-759.
- 910-916 QCTRANS, e mode, vs g_monsters.c mon_die_think: real. Included in 870-917.
- 1646-1649 QCTRANS, e mode: real, trivial (walkmonster_start_go defaults).
- k16 1184-1187 (g_combat.c), 1214-1218 (g_boss.c): false positive (guard/return idiom).
- k16 1268 (EF_MUZZLEFLASH): Quake API idiom (interface).
- k16 1293-1294 (anglemod delta, g_ai.c M_FacingIdeal): idiom with a different threshold rule. Not counted.

##### source/hl/hl_ai.h (196 lines; git: 8ca78f4, modified in worktree)

Verdicts: CLEAN; QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|15-38|HLMT_*/HLAI_* enums|CLEAN|—|Ours.|
|40-47|HL_SF_MONSTER_* values|HLSDK constants|hlsdk/dlls/monsters.h spawnflags|Numeric facts.|
|88-92|HL_ACT_* values|HLSDK constants|hlsdk/dlls/activity.h|Numeric facts.|
|49-192|prototypes and comments|CLEAN|—|Ours. Comments cite HLSDK function names only.|

Bindings: edict_t (QuakeC edict), vec3_t, qboolean, uint16_t.

Automated hits: none.

##### source/hl/hl_grunt.c (1958 lines; git: untracked)

Verdicts: HLSDK; QUAKE-INTERFACE; CONTAINS-QCTRANS (trivial).

This is a C reimplementation of hgrunt.cpp, CHGruntRepel, CDeadHGrunt and islave.cpp. The node-less cover search is our own. The control loop is an own state machine that approximates CHGrunt::GetSchedule; event handling and attack math are translated from HLSDK.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|35-63|weapon bits, clip sizes, sentence volume, cones, state enum|HLSDK constants + own enum|hgrunt.cpp defines; util.h VECTOR_CONE_*|Facts.|
|65-131|roster, same_squad, is_leader|CLEAN|—|Own squad registry.|
|133-230|squad_recruit, squad_count_state, squad_alert, squad_notify_death, squad_member_near|HLSDK|squadmonster.cpp SquadRecruit(308-), SquadMakeEnemy(244-); monsters.cpp FCheckAITrigger squad conditions|Behavioural translation. squad_alert calls qctrans FoundTarget.|
|232-295|voice_pitch, grunt_say, grunt_idle_sound|HLSDK|hgrunt.cpp Spawn voice pitch; FOkToSpeak(331-), JustSpoke(355-), IdleSound(686-)|Structural translation (g_fGruntQuestion check/question/answer).|
|297-327|seq, set_view, gun_position, body_target|HLSDK|hgrunt.cpp GetGunPosition; BodyTarget|Constants 60/48.|
|329-455|slave beam slots, slave_clear_beams, slave_add_beam, slave_arm_beam, slave_powerup|HLSDK|islave.cpp ClearBeams, ArmBeam(723-), BeamGlow, HandleAnimEvent ZAP_POWERUP(310-)|Translation: three random 512-unit traces, nearest kept.|
|457-506|set_state, next_think, enemy_of, face_enemy, facing_dot|CLEAN|—|Ours (enemy_of k16 hit is a false positive).|
|507-606|fire_bullets, can_shoot|HLSDK|hgrunt.cpp Shoot(787-) / Shotgun (EjectBrass offsets); FireBullets; CheckRangeAttack1|Behavioural.|
|608-702|grenade_explode, grenade_timed_think, grenade_bounce, grenade_contact, grenade_spawn|HLSDK|ggrenade.cpp ShootTimed / ShootContact / BounceTouch|Behavioural, own code.|
|704-845|danger_near, check_toss, check_throw, grenade_check|HLSDK|h_ai.cpp VecCheckToss(79-), VecCheckThrow(165-); hgrunt.cpp CheckRangeAttack2(474-)|Structural translation (500-unit limit, randomised target, mid-point apex traces).|
|847-917|find_cover, start_cover|CLEAN|—|Own, without nodes.|
|919-975|start_fire, start_reload, start_melee, start_grenade, start_zap|HLSDK|hgrunt.cpp schedules / sequence names|Behaviour.|
|977-1028|zap_beam|HLSDK + QUAKE-INTERFACE|islave.cpp ZapBeam(814-)|Translation. 1002-1013 is the non-SDL Quake TE_LIGHTNING2 datagram (protocol-dictated, see hits).|
|1030-1271|combat_done, state_anim_think|HLSDK|hgrunt.cpp HandleAnimEvent(842-) (BURST, KICK, DROP_GUN, grenade frames); islave.cpp HandleAnimEvent (CLAW, ZAP)|Event frames and damage translated.|
|1273-1309|cover_think|CLEAN|—|Own (g_walkmove binding).|
|1311-1446|grunt_combat_think|HLSDK (approximation)|hgrunt.cpp GetSchedule(1975-)|Own state machine carrying HLSDK priorities.|
|1448-1514|grunt_sight, start of grunt_stand_think|HLSDK|hgrunt.cpp AlertSound / squad leader HG_ALERT|Behaviour.|
|1515-1519|ai_stand tail inside grunt_stand_think|CONTAINS-QCTRANS (trivial)|qctrans/g_ai.c ai_stand(258-269)|Look, else pausetime→th_walk.|
|1522-1536|grunt_walk_think|CONTAINS-QCTRANS (1532-1535 trivial)|qctrans/g_ai.c ai_walk(250-256)|Look, then movedist and movetogoal.|
|1538-1701|grunt_pain, grunt_drop_gun, grunt_die_think, grunt_die|HLSDK|hgrunt.cpp PainSound, TakeDamage/light-damage schedule, DROP_GUN event, DeathSound; islave.cpp|Behavioural translation.|
|1703-1827|grunt_precache, soldier_start, SP_*, SP_monster_hgrunt_dead|HLSDK|hgrunt.cpp Spawn/Precache; CDeadHGrunt::Spawn(2467-)|Spawn values and pose names.|
|1829-1956|repel_think, repel_use, SP_monster_grunt_repel|HLSDK|hgrunt.cpp CHGruntRepel::RepelUse(2405-)|Translation (rope beam, slide time). Uses qctrans monster_use.|

Quake / QCTRANS bindings:
- **QuakeC entvars:** cnt, weapon, currentammo, t_length, t_width, dmgtime, pos1, pos2, dest, inpain, height, waitmin, waitmax, netname, owner, dmg, avelocity, punchangle, waterlevel, lip, skin, frame, walkframe, th_*, think, nextthink, use, touch, enemy, oldenemy, goalentity, state, spawnflags, max_health, yaw_speed, view_ofs, movetype, solid, flags, deadflag, takedamage, absmin/absmax, origin, angles, velocity.
- **pr_global_struct:** v_forward, v_right, v_up, trace_fraction, trace_ent, trace_endpos.
- **Network:** MSG_WriteByte/Short/Coord, sv.datagram, svc_temp_entity, TE_LIGHTNING2, g_datagram_space (non-SDL path).
- **Server:** SV_Move, SV_LinkEdict, SV_InternString, PR_SetEngineString, NUM_FOR_EDICT, sv.edicts, sv_gravity.
- **QCTRANS-API:** FoundTarget, monster_use, g_movetogoal/g_movedist, g_walkmove, g_changeyaw, g_droptofloor, g_traceline, g_makevectors, g_spawn, g_remove, g_setmodel/size/origin, g_precache_model, M_Visible, T_Damage, g_damage_type, g_damage_attacker.
- **Con:** Con_Printf, Con_DPrintf.

Automated hits:
- 1002-1013 QCTRANS, e/n mode, vs g_weapons.c(681-690): real overlap in the Quake TE_LIGHTNING2 message layout. This is protocol-dictated, so it is classed as interface and not counted as expression. Delete the path on import.
- 1724-1738 QCTRANS, n mode, vs g_world.c: false positive (precache list shape).
- k16 476-480 (g_boss.c): false positive.
- k16 531 EF_MUZZLEFLASH: Quake API idiom.
- k16 875-876 (g_items.c): false positive.
- k16 1006-1010 QUAKE sv_main.c MSG_WriteShort: protocol idiom (same block as above).

##### source/hl/hl_monsters.c (2062 lines; git: 8ca78f4 via 794051d, modified in worktree)

Verdicts: HLSDK; CONTAINS-QUAKE (small); CONTAINS-QCTRANS (small); QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|30-49|mon_hide_quake_gibs|QUAKE-INTERFACE|—|Own. Hides Quake progs/gib edicts (Quake assets plus SUB_Remove).|
|53-71|SP_monster_scientist_dead|HLSDK|scientist.cpp CDeadScientist::Spawn(1155-)|Pose table and corpse health.|
|75-122|mon_sequence_box, mon_corpse_fall_think|HLSDK|animating.cpp SetSequenceBox(258-); monsters.cpp CorpseFallThink(3246-)|Structural translation (four-corner yaw rotation, z 0..1).|
|124-186|HL_MonDeadPose, HL_MonInitDead, HL_MonDeadTakeDamage|HLSDK|CDead*::Spawn bad-pose path; monsters.cpp MonsterInitDead(3260-); combat.cpp BecomeDead(518-), DeadTakeDamage(974-)|Translation (DMG_GIB_CORPSE, the 0.1 damage factor).|
|188-253|HL_MonTrace, HL_MonSkill, HL_MonSeqInfo, HL_MonPlay, HL_MonStep, HL_MonEvent|CLEAN|—|Ours.|
|255-269|HL_FreeEdicts|CONTAINS-QUAKE|first/source/pr_edict.c ED_Alloc(87-100)|Structural copy of the ED_Alloc slot loop and freetime reuse predicate (hit 261-265).|
|271-298|HL_AI_CheckTrigger|HLSDK|monsters.cpp FCheckAITrigger(3009-)|Translation.|
|300-315|HL_MonOkToSpeak, HL_MonJustSpoke|HLSDK|hgrunt.cpp FOkToSpeak(331-), JustSpoke(355-)|Translation.|
|317-385|HL_MonMeleeHit, HL_MonCorpse, mon_drop_riders|CLEAN|combat.cpp CheckTraceHullAttack (own approximation)|Ours.|
|387-402, 407-425|HL_MonGibCheck, HL_MonGib|HLSDK|combat.cpp GibMonster(303-)|Behaviour.|
|403-406|three ThrowGib calls with Quake gib models|CONTAINS-QCTRANS (small)|qctrans/g_monsters.c GibPlayer(147-153) / Mon_CheckGib(160-)|Same call sequence and Quake asset names.|
|427-444|HL_MonDamageScale|HLSDK|barnacle.cpp TakeDamage; gargantua.cpp TakeDamage(866-)|Translation (club kills the barnacle; garg takes 1%).|
|446-489|NewMap, capture_die, resume_run, attack_ok|CLEAN|—|Ours.|
|491-593|Classify enums, iEnemy table, class/fov tables, HL_MonRelation, mon_rank, HL_MonIsFoe|HLSDK|monsters.cpp IRelationship iEnemy(2199-2230); hgrunt.cpp IRelationship(264-); islave.cpp|Table copied without the bioweapon columns.|
|595-674|client PVS copy, mon_client_pvs, HL_MonEyeInClientPVS|QUAKE-INTERFACE|first/source/sv_main.c(458) leafnums bit test|API-dictated idiom over Quake edict leafnums and Mod_LeafPVS.|
|676-818|HL_MonBestEnemy, mon_see_client, HL_MonLook, HL_MonRetarget|HLSDK|monsters.cpp Look(305-), BestVisibleEnemy(2428-), GetEnemy|Own code carrying HLSDK semantics. Calls qctrans FoundTarget.|
|820-974|houndeye squad count, sonic attack, attack think|HLSDK|houndeye.cpp SonicAttack(559-), WriteBeamColor(510-), CheckRangeAttack1|Translation: falloff, half damage behind cover for clients, beam colour table.|
|976-1328|gargantua flame slots/create/update/damage/stop/think/start, stomp, anim, play_attack|HLSDK|gargantua.cpp FlameCreate(492-), FlameUpdate(552-), StompAttack(472-), HandleAnimEvent(978-)|Translation of the parameters. g_particle (non-SDL) is a Quake binding.|
|1331-1412|crab_leap_touch, crab_leap_think, crab_leap|HLSDK|headcrab.cpp LeapTouch(343-), HandleAnimEvent JUMPATTACK(214-)|Structural translation of the ballistic jump.|
|1414-1507|zombie_claw, zombie_attack_think, zombie_attack|HLSDK|zombie.cpp HandleAnimEvent(189-)|Translation (punch/velocity per side).|
|1509-1572|HL_MonCheckAttacks|HLSDK|CheckMeleeAttack1/RangeAttack1 constants per class|Behaviour.|
|1574-1855|barnacle holder/release/victim/touch/bite/think, die, pain|HLSDK|barnacle.cpp BarnacleThink(145-), TongueTouchEnt(394-)|Structural translation (lift, bite at body height, kill delay, 1-in-50 chew).|
|1857-2060|SP_* spawns, monster_generic|HLSDK|genericmonster.cpp Spawn(96-); per-class Spawn/Precache; monsters.cpp StartMonster|Behaviour and values.|

Quake / QCTRANS bindings:
- **QuakeC entvars:** lip, cnt, lefty, t_width, t_length, dmgtime, air_finished, healtype, max_health, punchangle, velocity, absmin/absmax, size, netname, groundentity, armorvalue, view_ofs, angles, skin, frame, walkframe, th_*, think, nextthink, enemy, oldenemy, goalentity, attack_state, attack_finished, pain_finished, model, modelindex, effects, flags, spawnflags, solid, movetype, deadflag, takedamage.
- **edict_t internals:** free, freetime, num_leafs, leafnums, w_layer.
- **pr_global_struct:** v_forward, v_right, trace_fraction, trace_ent.
- **Server/model:** sv.models, sv.worldmodel, sv.num_edicts, sv.edicts, sv.time, svs.maxclients, EDICT_NUM, NUM_FOR_EDICT, ED_Free, SV_LinkEdict, SV_LayerAt, Mod_StudioHeader, Mod_LeafPVS, Mod_PointInLeaf, Mod_LeafNum, MAX_EDICTS.
- **Cvar/Con:** skill, sv_gravity, Cvar_VariableValue, Con_Printf, Con_DPrintf.
- **QCTRANS-API:** FoundTarget, ai_face, ThrowGib, SUB_Remove, M_Visible, T_Damage, g_damage_type, g_damage_attacker, g_traceline, g_makevectors, g_particle, g_droptofloor, g_checkclient, g_setmodel/size/origin, g_precache_model.

Automated hits:
- 261-265 QUAKE, e mode, pr_edict.c: real (ED_Alloc predicate) → CONTAINS-QUAKE 255-269.
- 37-42, 377-380, 1588-1593 QUAKE/QCTRANS, e mode: edict-iteration idiom. Interface, not counted.
- 403-406 QCTRANS, e mode, g_monsters.c GibPlayer: real (ThrowGib triple).
- k16 403-408 XASH sv_client.c: false positive.
- 506-517 HLSDK, n mode, monsters.cpp(2203): real (iEnemy table).
- k16 1138-1144 XASH sv_client.c: false positive.

##### source/hl/hl_monsters.h (184 lines; git: untracked)

Verdicts: CLEAN; QUAKE-INTERFACE (edict_t).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|21-36|AITRIGGER_* and SF_MONSTER_* values|HLSDK constants|monsters.h|Facts.|
|128-134|R_AL..R_NM values|HLSDK constants|cbase.h / monsters.h|Facts.|
|rest|prototypes and comments|CLEAN|—|Ours. Comment 72-76 notes that Quake gib models stand in for hgibs.|

Bindings: edict_t, qboolean, vec3_t.

Automated hits: none.

##### source/hl/hl_monstermaker.c (438 lines; git: untracked)

Verdicts: HLSDK; QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|31-33|SF_MONSTERMAKER_*|HLSDK constants|monstermaker.cpp|Facts.|
|35-114|child registry (Owner/Detach/Attach)|CLEAN|—|Ours (streaming/HLE).|
|116-162|maker_delay, maker_live_children|HLSDK|monstermaker.cpp DeathNotice / m_fFadeChildren|Behaviour.|
|164-281|maker_track, maker_make|HLSDK|monstermaker.cpp MakeMonster(161-240)|Structural translation (ground altitude trace, ±34 occupancy box, spawnflag MONSTERCLIP, netname→targetname, fire target, disable at zero). One comment paraphrases the HLSDK "stack of monsters" comment.|
|283-327|maker_think, maker_use|HLSDK|monstermaker.cpp MakerThink, CyclicUse(241-), ToggleUse(249-)|Translation.|
|329-358|maker_precache_child|HLSDK concept|util.cpp UTIL_PrecacheOther|Own code.|
|360-396|maker_spawn|HLSDK|monstermaker.cpp Spawn(104-)|Translation.|
|398-438|save/restore, NewMap, class table|CLEAN|—|Ours.|

Quake / QCTRANS bindings:
- **QuakeC entvars:** classname, angles, spawnflags, targetname, state, health, deadflag, flags, solid, absmin/absmax, think, nextthink.
- **edict_t internals:** freetime.
- **pr_global_struct:** total_monsters, trace_endpos.
- **Edict glue:** EDICT_NUM, NUM_FOR_EDICT, ED_Free, sv.edicts, sv.num_edicts.
- **Server:** SV_InternString, PR_SetEngineString.
- **QCTRANS-API:** g_spawn, g_remove, g_setorigin, g_traceline, g_vlen, G_SELF/G_SetSelf, SUB_Remove.
- **Con:** Con_Printf, Con_DPrintf.

Automated hits: none.

##### source/hl/hl_nodes.c (1244 lines; git: untracked)

Verdicts: HLSDK; CONTAINS-QUAKE+XASH (tiny); QUAKE-INTERFACE.

The resumable, frame-budgeted route job (85-289, 746-1242) is our own design. The pieces it wraps are translations of HLSDK route code.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-49|header|CLEAN|—|Prose citing HLSDK names.|
|59-63|ROUTE_SIZE, MAX_PATH_SIZE, LOCAL_STEP_SIZE, CUT_CORNER_DIST, NODE_HEIGHT|HLSDK constants|monsters.h, nodes.h|Facts.|
|85-224|job/route structs, route_trace, mon_name, dist2d, graph_of, node_org|CLEAN|—|Ours.|
|228-243|drop_to_floor|CONTAINS-QUAKE / XASH / QCTRANS|first/source/pr_cmds.c PF_droptofloor(1189-1212); xash/engine/server/sv_game.c pfnDropToFloor(1852-1878); qctrans/g_api.c g_droptofloor(350-365)|Structural copy of the engine builtin (256-unit down trace, endpos and groundentity).|
|245-289|budget_frame, step_timed, budget_step|CLEAN|—|Ours.|
|291-401|check_local_move|HLSDK|monsters.cpp CheckLocalMove(1300-)|Structural translation (step-size formula, 64-unit DONT_TRIANGULATE) made resumable. Calls Quake SV_movestep.|
|405-425|hull_mask, node_types|HLSDK|nodes.cpp HullIndex(442-), NodeType(459-)|Translation.|
|427-474|nearest_node|HLSDK (own code)|nodes.cpp FindNearestNode(864-)|Behaviour (32-candidate limit is ours).|
|476-491|link_passable|HLSDK|nodes.cpp HandleLinkEnt(216-)|Behaviour.|
|493-609|heap_up/down, shortest_path|HLSDK (own implementation)|nodes.cpp FindShortestPath(583-)|Own binary-heap Dijkstra.|
|611-659|opens_doors, node_route|HLSDK|monsters.cpp FGetNodeRoute(2774-); bits_CAP_OPEN_DOORS|Translation.|
|661-670|job_drop|CLEAN|—|—|
|672-744|route_simplify|HLSDK|monsters.cpp RouteSimplify(827-)|Structural translation (skip vertex, test midpoint, split+test), made resumable.|
|746-790|job_walked, seg_dist2d, route_resume|CLEAN|—|Ours.|
|791-826|walk_route|HLSDK|monsters.cpp Move / MONSTER_CUT_CORNER_DIST|Behaviour.|
|828-1242|route_left, walk_building, job_start/finish/run, route slots, RouteFrame, HL_AI_MoveTo|CLEAN|monsters.cpp BuildRoute(1535-) concept|Own architecture.|

Quake bindings:
- **QuakeC entvars:** origin, mins, maxs, flags, groundentity, solid, movetype, waterlevel, lip, targetname, classname.
- **edict_t internals:** free, freetime.
- **Trace/move:** SV_Move, SV_movestep, trace_t, MOVE_NOMONSTERS.
- **Server:** sv.time, EDICT_TO_PROG, PROG_TO_EDICT, NUM_FOR_EDICT.
- **System/Cvar/Con:** Sys_FloatTime, Sys_Error, Cvar_FindVar, Con_Printf.
- **QCTRANS-API:** g_vlen, g_vectoyaw, G_TIME, G_Str.
- **Own HLE layer:** HLE_MapGraph, HLE_EntitySlot.

Automated hits:
- 237-238 QCTRANS, e mode, g_api.c(357); k16 233-238 QUAKE pr_cmds.c(1195-1200); k16 237 XASH sv_game.c(1864): all real → CONTAINS 228-243.
- k16 377-378, 1201-1203, 1219-1221, 1237-1238 QCTRANS game.c: false positive (Con_Printf argument lists).

##### source/hl/hl_nodecook.h (1314 lines; git: untracked)

Verdicts: CONTAINS-QUAKE; CONTAINS-XASH; HLSDK; CLEAN (format and parsers).

Comments 19-23 cite the Xash engine sv_world.c and sv_move.c as the model for the traces. The trace/step block is a translation of the Quake engine collision code, with GoldSrc/Xash variants.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-38|header|CLEAN (cites Xash)|—|Prose.|
|39-196|HLN1 format: magic, structs, byte-swap, hln_fixup|CLEAN|—|Ours.|
|49-67|MAX_NODES, node/link bit values, door spawnflags|HLSDK constants|nodes.h, doors.h|Facts.|
|205-278|cook constants, BSP structs, clip-mins table, readers|CLEAN (GoldSrc hull sizes are facts)|—|—|
|280-302|hlnc_pc|CONTAINS-QUAKE|first/source/world.c SV_HullPointContents(491-); also xash/engine/common/pm_trace.c PM_HullPointContents(111-)|Structural translation.|
|304-379|hlnc_rhc|CONTAINS-QUAKE|first/source/world.c SV_RecursiveHullCheck(581-710); also xash pm_trace.c PM_RecursiveHullCheck(194-)|Structural translation: DIST_EPSILON crosspoint, mid-fraction recursion, back-off-by-0.1 loop (hit 346-354). Depth/range guards are ours.|
|381-404|hlnc_clip_model|CONTAINS-QUAKE|world.c SV_HullForEntity(129-) offset, SV_ClipMoveToEntity(722-)|Translation.|
|406-416|hlnc_hull_of|CONTAINS-XASH|xash/engine/server/sv_world.c SV_HullForBsp(174-)|GoldSrc hull-selection thresholds as in Xash.|
|418-469|hlnc_move|CONTAINS-QUAKE (+Xash world-only variant)|world.c SV_Move(923-), SV_MoveBounds(893-), SV_ClipToLinks(814-); xash sv_world.c SV_MoveNoEnts(1349-)|Structural translation (±1 move bounds, startsolid merge rule).|
|471-478|hlnc_contents|CONTAINS-QUAKE|world.c SV_PointContents|current→water fold.|
|480-526|hlnc_checkbottom|CONTAINS-QUAKE + CONTAINS-XASH|first/source/sv_move.c SV_CheckBottom(37-104); xash/engine/server/sv_move.c SV_CheckBottom(34-104)|Structural copy, including the goto realcheck shape (hits 495-523). The stepsize lift and world-only mode follow Xash.|
|528-565|hlnc_movestep|CONTAINS-QUAKE + CONTAINS-XASH|sv_move.c SV_movestep(110-); xash sv_move.c SV_MoveTest(342-)|Structural translation of the step-up/step-down probe.|
|567-578|hlnc_yaw|CONTAINS-QUAKE|first/source/pr_cmds.c PF_vectoyaw|Integer-degree vectoyaw.|
|580-665|hlnc_ent_t, hlnc_next_ent|CLEAN|—|Own entity-lump tokenizer.|
|667-748|hlnc_is_solid, hlnc_wbrush, hlnc_lent, hlnc_door_open|HLSDK|per-class Spawn solidity; func_break.cpp; doors.cpp CBaseDoor::Spawn + subs.cpp SetMovedir|Behaviour and translation (movedir, lip travel).|
|750-855|hlnc_load, hlnc_free|CLEAN|—|BSP30 format reader (hull-0 leaf→contents mirrors the Mod_MakeHull0 concept; own code).|
|857-1253|hln_build|HLSDK|nodes.cpp CTestHull::BuildNodeGraph(1601-), LinkVisibleNodes(1143-), RejectInlineLinks(1342-)|Structural translation: reverse-trace link entity, per-hull 16-unit walk with the 64-unit end check, fly-hull trace, 0.998 inline rejection, NODE_HEIGHT lift/drop.|
|1255-1310|hle_cook_bsp|CLEAN|—|Ours.|

Bindings: none (pure C99 offline cook).

Automated hits:
- 346-354 QUAKE, e/n mode, world.c(644-652): real.
- 495-501, 506-510, 512-519, k16 519-523 QUAKE sv_move.c: real.
- 497-500, 506-509, 515-518, k16 519-522 XASH sv_move.c: real.
- k16 1060-1063 HLSDK pm_shared.c: false positive (2D distance).

##### source/hl/hl_scripted.c (1240 lines; git: untracked)

Verdicts: HLSDK; CONTAINS-XASH/QUAKE (tiny); QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-29|header|CLEAN|—|Field map plus deviations.|
|38-50|SF_SCRIPT_* values, CINE bits, SS enum|HLSDK constants|scripted.h(22-)|Facts.|
|52-160|possession pool, scr_trace, cine_key, ent_name, is_cine, cine_is_ai, poss_of, HL_AI_InScript|CLEAN (+HLSDK)|scripted.cpp CineCleanup "punt" path|Own pool.|
|162-237|cine_monster, delay_one/cine_delay_start, cine_start_seq|HLSDK|scripted.cpp DelayStart(725-), StartSequence(493-)|Translation.|
|239-256|drop_to_floor|CONTAINS-XASH / QUAKE|xash/engine/server/sv_game.c pfnDropToFloor(1852-1878) (tri-state result order); first/source/pr_cmds.c PF_droptofloor(1189-)|Structural copy of the engine builtin.|
|258-367|scr_restore_control, scr_cleanup|HLSDK|scripted.cpp CineCleanup(796-)|Structural translation (DEAD_DYING corpse box, LEAVECORPSE vs fade, move by sequence, drop and restore). Line 336 quotes an HLSDK comment.|
|369-432|cine_sequence_done, cine_entity_cancel, cine_cancel, HL_Script_CancelFor|HLSDK|scripted.cpp SequenceDone(561-), ScriptEntityCancel, CancelScript(704-)|Translation.|
|434-489|HL_Script_PrepareDehydrate|CLEAN|—|Ours (streaming).|
|491-498|HL_Script_CanInterrupt|HLSDK|scripted.cpp CanInterrupt|—|
|500-807|scr_enter_wait, scr_plant, scr_move, scr_face, scr_wait, scr_play, scr_think, scr_pain, scr_die, HL_Script_AnimEvent|HLSDK|schedule.cpp / defaultai.cpp SCHED_AISCRIPT tasks (WALK/RUN_TO_TARGET, PLANT_ON_SCRIPT, FACE_SCRIPT, WAIT_FOR_SCRIPT, PLAY_SCRIPT); monsters.cpp HandleAnimEvent SCRIPT_EVENT_*|Behaviour (own task stepping; stuck/no-route planting is ours).|
|809-1057|cine_find, cine_possess, cine_think, cine_use, precache helpers, cine_spawn, SP_*|HLSDK|scripted.cpp FindEntity(272-), PossessEntity(321-), CineThink(476-), Use(181-), Spawn/KeyValue|Translation. The "found but can't play" console string is reproduced verbatim from HLSDK.|
|1059-1238|scr_twin_gid, HL_Script_TwinHandoff, HL_ScriptNewMap|CLEAN|—|Ours (seamless-map twin handoff).|

Quake / QCTRANS bindings:
- **QuakeC entvars on the script entity:** enemy, pausetime, cnt, count, speed, lip, state, dest1, t_length, target, targetname, effects, angles, origin, spawnflags, think, nextthink, use.
- **QuakeC entvars on the monster:** movetype, solid, effects, th_pain, th_die, th_stand, goalentity, enemy, skin, frame, walkframe, deadflag, health, max_health, takedamage, flags, mins, maxs, velocity, avelocity, chain.
- **edict_t internals:** free, freetime.
- **Server:** SV_Move, SV_LinkEdict, sv.state (ss_loading), sv.edicts, sv.num_edicts, EDICT_NUM, EDICT_TO_PROG, PROG_TO_EDICT.
- **Cvar/Con:** Cvar_FindVar, Con_Printf, Con_DPrintf.
- **QCTRANS-API:** g_find, FOFS, g_findradius, g_setorigin, g_setsize, g_changeyaw, g_remove, anglemod, SUB_Remove, G_Func/G_GetFunc/G_Call/G_SetSelf/G_SetOther, g_activator, g_use_type/value, g_damage_attacker.
- **Own HLE layer:** HL_UseTargets.

The evidence's "draw" binding count (54) is a false categorisation: it matches the local `scr_` prefix. The file has no Draw_ or SCR_ calls.

Automated hits:
- 248-249 QCTRANS, e mode, g_api.c(357); k16 248 XASH sv_game.c(1864): real → 239-256.
- 1146-1154 and 1177-1195 QCTRANS, n mode, g_world.c: false positive (field-copy lists).
- k16 32-50 QUAKE net_ser.c: false positive (includes/defines).
- k16 38-46 HLSDK scripted.h: real (constants).
- k16 883-888 XASH cl_tent.c: false positive.
- comment_overlap 336 HLSDK scripted.cpp(864): real (quoted HLSDK comment).

##### source/hl/hl_sentences.c (831 lines; git: untracked)

Verdicts: CLEAN (1-505); HLSDK (510-826); QUAKE-INTERFACE.

The parser was compared structurally with xash/engine/client/sound/s_vox.c (VOX_GetDirectory, VOX_ParseString, VOX_ParseWordParams 223-438). The control flow, data model and default handling all differ: ours is a two-pass count/alloc parser into a pooled table, with standalone modifiers setting defaults and comma/period emitted as pause words at parse time. Only the documented sentences.txt/VOX syntax is shared, so this part is not contamination.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-21|header|CLEAN|—|States the VOX rules are [INFERENCE] from GoldSrc behaviour.|
|32-304|types, VoxParmDefaults/ParseParms, pool, AddWord, ParseBody, Parse, Init|CLEAN|xash s_vox.c (different)|Independent.|
|306-431|Count/Name/NumWords/Word/Find, group pick, resolve, Exists|CLEAN|hlsdk util.cpp USENTENCEG_Pick (behaviour only)|Own no-repeat mechanism.|
|433-508|PlayChan/PlayPitch/PlayOrigin/Play/Stop|CLEAN + QUAKE-INTERFACE|—|Clamps are generic.|
|510-524|talk wait time|HLSDK|talkmonster.cpp g_talkWaitTime|Concept.|
|526-750|scripted_sentence logic record|HLSDK|scripted.cpp CScriptedSentence FindThink(1073-), DelayThink, AcceptableSpeaker(1103-), StartSentence(1184-), KeyValue (attenuation map)|Translation.|
|752-826|speaker|HLSDK|sound.cpp CSpeaker SpeakerThink(1858-), ToggleUse(1925-)|Translation (ANNOUNCE_MINUTES constants).|
|828-831|NewMap|CLEAN|—|—|

Quake / QCTRANS bindings:
- **QuakeC entvars:** classname, targetname, message, health, spawnflags, goalentity, flags, deadflag, takedamage, solid, movetype, think, use, nextthink.
- **Sound/server:** SV_StartSentence, SV_StartSentenceOrigin, SV_StopSound (our engine extensions on Quake sv_), sv.num_edicts, EDICT_NUM, NUM_FOR_EDICT.
- **Memory/files:** COM_LoadTempFile, Hunk_AllocName, MAX_QPATH.
- **Cvar/Con:** Cvar_FindVar, Con_Printf, Con_DPrintf.
- **QCTRANS-API:** g_random, g_remove, G_TIME, g_use_type.
- **Own HLE layer:** HLE_* logic-record API.

Automated hits: k16 446-458 QCTRANS/QUAKE/XASH n mode: false positive (volume/attenuation/pitch clamp idiom).

##### source/hl/hl_sentences.h (38 lines; git: untracked)

Verdict: CLEAN.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-38|HL_SENT_MAXWORDS (= GoldSrc CVOXWORDMAX, a fact), hl_sentword_t, prototypes|CLEAN|—|Ours.|

Bindings: MAX_QPATH, qboolean.

Automated hits: none.

##### source/hl/hl_sounds.c (498 lines; git: 8ca78f4, modified in worktree)

Verdicts: CLEAN; QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-247|per-class sample lists and the class table|CLEAN (data)|—|HL asset paths (facts).|
|251-301|Find, PrecacheList, Pick, PlayList|CLEAN|—|Ours.|
|305-497|public precache/play API (PlayPitch, PlayOrigin, Stop, class helpers)|CLEAN + QUAKE-INTERFACE|—|Wrapper over Quake sv sound calls. The EMIT_SOUND_DYN "!" sentence dispatch is documented behaviour.|

Bindings:
- **Sound:** SV_StartSoundPitch, SV_StartSoundOrigin, SV_StopSound.
- **QCTRANS-API:** g_sound, g_precache_sound, g_random, G_Str.
- **Other:** ATTN_NORM (Quake), NUM_FOR_EDICT, Cvar_FindVar, Con_Printf.

Automated hits:
- 234-243/245 XASH, n mode, vs snd_utils.c: false positive (struct-initialiser table shape).
- k16 366-383 QCTRANS/QUAKE/XASH: false positive (clamp idiom).

##### source/hl/hl_sounds.h (119 lines; git: 8ca78f4, modified in worktree)

Verdict: CLEAN.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|55-60|HL_ATTN_*, HL_CHAN_STATIC, HL_PITCH_NORM|HLSDK constants|dlls/util.h|Facts.|
|rest|prototypes and comments|CLEAN|—|Ours.|

Bindings: edict_t forward declaration, vec3_t, qboolean.

Automated hits: none.

##### source/hl/hl_ambient.c (569 lines; git: untracked)

Verdicts: HLSDK; QUAKE-INTERFACE.

The header (8-11) claims the RampThink and InitModulationParms logic was written "from memory". Structurally it matches the HLSDK code closely, so it is treated as an HLSDK translation.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|23-28|AMBIENT_SOUND_* flags|HLSDK constants|sound.cpp|Facts.|
|29-132|roster, HL_AmbOwned/Amb/Audible/Alloc, key readers|CLEAN|—|Ours.|
|134-146|HL_AmbAttenuation|HLSDK|sound.cpp CAmbientGeneric::Spawn(163-)|Flag→attenuation mapping.|
|148-163|HL_AmbStaticEligible|CLEAN|—|Own static-sound optimisation predicate.|
|165-170|HL_AmbRate|HLSDK|sound.cpp KeyValue / InitModulationParms rate fixup (469-479, 648-)|Same rate-key transform and constants.|
|172-180|HL_AmbSample, HL_AmbChannel|CLEAN|—|—|
|182-203|HL_AmbInitModulation|HLSDK|sound.cpp InitModulationParms(452-)|Translation.|
|205-235|Emit, Stop, Trace|CLEAN + QUAKE-INTERFACE|—|—|
|237-335|ambient_think|HLSDK|sound.cpp RampThink(259-)|Structural translation of the pitchfrac/volfrac envelope (k16 hits 289-295, 316-322). The client-spawned wait is ours.|
|337-401|ambient_use|HLSDK|sound.cpp ToggleUse(536-)|Translation (USE_SET pitch, spindown/fadeout on off, one-shot restart).|
|403-470|SP_ambient_generic|HLSDK (except 413-432)|sound.cpp Spawn(163-)|413-432 is the own Quake static-sound shortcut (interface).|
|472-569|persistence, HomeChanged, StaticLayerOpened, NewMap|CLEAN + QUAKE-INTERFACE|—|Ours.|

Quake / QCTRANS bindings:
- **QuakeC entvars:** message, health, spawnflags, targetname, cnt, use, think, nextthink, solid, movetype, origin.
- **edict_t internals:** hl_lump (own extension), free.
- **Sound:** g_ambientsound (Quake ambientsound → svc_spawnstaticsound), SV_ChangeSound.
- **Server:** svs.clients[0].spawned.
- **QCTRANS-API:** g_remove, g_use_type, g_use_value, G_SELF, G_TIME, G_Func.
- **Cvar/Con:** Cvar_FindVar, Con_Printf, Con_DPrintf.
- **Own HLE layer:** HLE_*.

Automated hits: k16 289-295 and 316-322 HLSDK sound.cpp(298-304, 340-346): real (RampThink).


### A.AuditGameB — Game B (entities, env, items, weapons, movers, levelstate, HUD, material)


Working tree audited as of 2026-11-13. Files hl_weapons.c/.h, hl_hud.c, hl_hud_rects.h and hl_input.h are tracked (created in d2bc31a, modified in the working tree). All other files are untracked. **None of the 24 files carries a licence header** (no Id Software, GPL or "quakegeneric contributors" text, so no header needs stripping). No file began life as a copy of an id `.c` file.

Method: started from the automated hits (evidence.md, evidence_k16.md, comment_refs.txt, comment_overlap.md). Then did a structural review of every function that has a known Quake/Xash/HLSDK counterpart, against hlsdk/dlls (doors, plats, buttons, func_break, triggers, items, h_battery, healthkit, ggrenade, tripmine, weapons, player, combat, effects, explode, lights, sound.cpp), hlsdk/pm_shared/pm_shared.c, hlsdk/cl_dll (health, battery, ammo, ammohistory, flashlight, hud_redraw), quake first-commit world.c/r_light.c/sv_phys.c, quake/qw-qc/subs.qc, and xash engine/client/dll_int/cl_game.c, cl_parse.c and engine/server/sv_game.c.

**Corpus caveat, important for every QCTRANS hit below:** the qctrans corpus is not purely Quake-derived. qctrans/g_buttons.c (hlb_* functions, HL_SP_func_button, SP_func_rot_button), g_weapons.c (Uplink pl_gun/hks weapon drafts), g_triggers.c (hl_hurt_*) and game.c contain our own earlier Half-Life port code. Each QCTRANS hit was therefore traced to the specific qctrans function. It counts as Quake-derived only when that function is a translation of a .qc function.

#### Summary


Totals: Quake 119 lines (hl_material.c 70, hl_use.c 45, hl_weapons.c 4). Xash 17 lines (hl_use.c, de minimis). QCtrans 21 lines (hl_movers.c, de minimis, HLSDK-equivalent). Valve shake algorithm 32 lines (hl_view.c). HLSDK-derived ≈ 7,600 of 19,565 lines (≈39% of the slice). By file, the HLSDK share is ≈85-90% of hl_movers/hl_breakable/hl_items/hl_logic, ≈80% of hl_weapons/hl_hud, ≈67% of hl_env, ≈63% of hl_use and ≈35% of hl_material. hl_ents.c, hl_levelstate.c and hl_entcook.h (8,380 lines) are our own architecture.

---

##### source/hl/hl_ents.c (4349 lines; git: untracked)
Verdicts: CLEAN (own expression), QUAKE-INTERFACE. This is the runtime for our HLE1 entity-chunk format: key hashing, name lists, the timer queue, streaming/wake grids, the logic table and twin aliasing. It has no Quake/Xash code counterpart. HLSDK *behaviour* appears in multi_manager timing, master gating and SUB_UseTargets ordering, but only 2486-2524 follows an HLSDK control-flow shape.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-4349 (except below)|chunk runtime, streaming pump, wakes, timers|CLEAN|— (HLE1 is our format; ED_ParseEdict cited only as "later keys win" behaviour, 258)|own design|
|606-632|hle_pin_strings|CLEAN / QUAKE-INTERFACE|— (walks progs fielddefs)|own; binds pr_fielddefs, PR_SetEngineString|
|1223-1276|HLE_FillEdict|CLEAN / QUAKE-INTERFACE|Quake pr_edict.c ED_ParseEdict does text parsing; ours copies pre-typed binary values|different algorithm|
|2486-2524|hle_run_mm|HLSDK (structural)|hlsdk dlls/triggers.cpp CMultiManager::ManagerThink|same "fire all due, then reschedule at next delay" loop, adapted to timers|
|2629-2669|hle_use_targets / HLE_UseTargetsEdict / HLE_LUseTargets|CLEAN (HLSDK semantics)|hlsdk cbase.cpp SUB_UseTargets|behaviour only (delay → timer, killtarget, target)|
|3587-3604|hle_candidate (sorted insert)|CLEAN|—|generic insertion shift|

Quake bindings: progs/QC-edict (pr_global_struct, G_Str/G_SetStr, PR_SetEngineString, pr_fielddefs/progs, ED_FindField, ED_Free, ED_RoomFor, ED_AllocSerial, ED_NewString/ED_ParseEdict/ED_LoadFromFile references, EDICT_NUM/NUM_FOR_EDICT, string_t, ddef_t, g_activator/g_use_type/g_use_value, G_TIME); edict_t; sv (sv.time, sv.edicts, sv.num_edicts, sv.models, sv.worldmodel, svs.maxclients, SV_Move, SV_RunThink, SV_PrecacheContext/GetContext, SV_SubmodelName, SV_LayerAt); model (Mod_ForName, Mod_StudioPrefetch, model_t); cvar (Cvar_FindVar, cvar_t); cmd (Cmd_AddCommand); con/sys (Con_Printf/DPrintf, Sys_Printf/Error/FloatTime); hunk (Hunk_AllocName); host (host_frametime); string (Q_strcasecmp/Q_strncpy/Q_strncasecmp).

Automated hits: XASH n 551-559 ← vgl_shim.c (keyname hash insert vs GL shim): false positive. QUAKE n 3597-3603 ← quake2 p_hud.c sort shift: false positive (generic array shift).

##### source/hl/hl_ents.h (250 lines; git: untracked)
Verdict: CLEAN, QUAKE-INTERFACE. Own API declarations: hl_logic_t, hl_logic_class_t, HLE_GID macros, HLL_* flags.
Bindings: edict_t, vec3_t, qboolean, string_t in prototypes.
Automated hits: none.

##### source/hl/hl_entcook.h (1635 lines; git: untracked)
Verdicts: CLEAN, QUAKE-INTERFACE. This is our own offline/runtime cooker for the HLE1 binary format (doc n64/docs/hle-format-v1.md): byte swapping, bounds validation, block parser, string pools, pruning. The tables embed data only, no copied code. Lines 375-433 list GoldSrc classnames with our flags (Valve entity names are facts). Lines 449-494 list QuakeC entvars field names, which is an interface. The "\n" escape expansion (624-658) reproduces ED_NewString *behaviour* in our own code.

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
|126-366|hash, byteswap, hle_fixup validation|CLEAN|—|own format|
|375-447|hle_classes / needs_name / needs_ref tables|CLEAN (data)|names occur in hlsdk client.cpp precache lists and q2 ctf|classname facts|
|449-494|hle_fields (entvars names/kinds)|QUAKE-INTERFACE|source/g_entvars.h (QuakeC entvars)|re-target to hl64 entvars|
|573-1633|hlec_* parser/builder|CLEAN|Quake COM_Parse/ED_ParseEdict are different (token parser); ours scans raw text|own|

Automated hits: XASH n 203-208 ← xash filesystem/wad.c (byteswap loop): false positive. HLSDK/QUAKE n 386-397 ← client.cpp / q2 g_ctf.c (classname string list): false positive (data). HLSDK k16 427-432 ← qlumpy.c, XASH ← sv_client.c (string list): false positive. LIBDRAGON 949-957: false positive.

##### source/hl/hl_env.c (1181 lines; git: untracked)
Verdicts: HLSDK, QUAKE-INTERFACE. The header comment states the HLSDK behaviour was reproduced from memory of the SDK.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|25-41|trace/key helpers|CLEAN|—|own|
|43-199|titles.txt loader HL_Titles_*|CLEAN (own parser of Valve file format)|xash engine titles parsing is structured differently; message-time formula 165-167 follows cl_dll message.cpp behaviour|own expression|
|201-231|HL_MsgClient|CLEAN / QUAKE-INTERFACE|—|svs.clients + MSG_* writer|
|232-370|render tuple store (HL_Render*)|CLEAN; 319-331 constants|xash CL_FxBlend (comment says "checked against Xash3D's")|constant facts (0/255/180), not expression|
|372-435|env_render_use|HLSDK|effects.cpp CRenderFxManager::Use|structural translation|
|440-574|env_sprite (CSprite Spawn/Animate/TurnOn/Use)|HLSDK|effects.cpp CSprite|structural translation|
|579-595|HL_BoxDistSq|CLEAN|xash sv_game.c pfnFindEntityInSphere (opposite sign convention)|generic point-box distance|
|597-648|HL_RadiusDamage|HLSDK|combat.cpp ::RadiusDamage (HLSDK comment "in case grenade is lying on the ground" at 611)|structural translation|
|650-744|env_explosion, HL_DoExplosion, ExplosionCreate|HLSDK|explode.cpp CEnvExplosion::Use / ExplosionCreate|structural translation|
|746-817|env_shake, HL_ScreenShake|HLSDK|util.cpp UTIL_ScreenShake, effects.cpp CShake|structural translation|
|819-856|env_fade|HLSDK|effects.cpp CFade / UTIL_ScreenFade|structural translation|
|858-926|env_message|HLSDK|effects.cpp CMessage|structural translation|
|928-993|env_spark|HLSDK|effects.cpp CEnvSpark / DoSpark|structural translation|
|995-1165|env_shooter / gibshooter|HLSDK|effects.cpp CGibShooter / CEnvShooter|structural translation|
|1167-1181|HL_Env_NewMap|CLEAN|—|own|

Quake bindings: progs (pr_global_struct, G_SELF, G_Func, G_Str/G_SetStr, PROG_TO_EDICT, NUM_FOR_EDICT/EDICT_NUM, g_* builtins: g_random, g_traceline, g_setmodel/size/origin, g_makevectors, g_remove, g_precache_model, g_damage_type, T_Damage, SetMovedir, ED_Free, ED_AllocTransient); edict_t, client_t; sv (sv.edicts, sv.models, sv.num_edicts, svs.clients/maxclients); msg/svc (MSG_WriteByte/Float/String, svc_hlshake/hlfade/hltitle/hltent); model (Mod_FindName, model_t); COM_LoadTempFile; Hunk_AllocName; Cvar_FindVar; Con_Printf/DPrintf; Q_strcasecmp.

Automated hits: HLSDK k16 306-310 ← cl_dll hud.cpp (byte packing): false positive. XASH k16 587-593 ← sv_game.c FindEntityInSphere: generic point-box distance, our own sign convention; false positive. comment_refs: Xash cited only for the renderfx settle constants.

##### source/hl/hl_env.h (88 lines; git: untracked)
Verdicts: CLEAN, QUAKE-INTERFACE; HLSDK constants 52-55 (SF_RENDER_MASK*, effects.cpp).
Bindings: edict_t, vec3_t, byte, qboolean.
Automated hits: QUAKE k16 63-79 ← quake2 gl_local.h (prototype list): false positive.

##### source/hl/hl_items.c (1739 lines; git: untracked)
Verdicts: HLSDK, QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|37-57|skill tables / HL_SkillValue|HLSDK data|Uplink skill.cfg / gamerules skill|values|
|64-88|HL_SuitSay|HLSDK|player.cpp EMIT_SOUND_SUIT|translation|
|90-190|suit update queue|HLSDK|player.cpp SetSuitUpdate / CheckSuitUpdate|structural translation|
|192-304|HL_PlayerTakeDamage|HLSDK|player.cpp CBasePlayer::TakeDamage (HLSDK comments "automedic on", "morphine shot")|structural translation|
|306-346|HL_WriteDamage|HLSDK + QUAKE-INTERFACE|player.cpp UpdateClientData "Damage" msg|translation onto MSG_*/svc_hldamage|
|348-418|item spawn/touch shell|HLSDK + own|items.cpp CItem::Spawn/ItemTouch|structural; invisible-shell logic is ours|
|420-604|item_suit/healthkit/battery/longjump, HL_LongJump, world_items|HLSDK|items.cpp, healthkit.cpp, h_battery (in items.cpp), pm_shared.c PM_Jump long-jump|structural translation|
|606-676|env_beverage / item_sodacan|HLSDK|items.cpp CEnvBeverage / CItemSoda|structural translation|
|678-799|ammo classes/touch/spawn|HLSDK|weapons.cpp CBasePlayerAmmo, *_GIVE|structural translation|
|800-847|HL_PrecacheItemClass / HL_SpawnItemByClass|HLSDK semantics|CBaseEntity::Create / UTIL_PrecacheOther|own expression|
|848-904|HL_ItemDrop_f, HL_Hurt_f|CLEAN|—|own debug commands|
|906-1040|func_healthcharger / func_recharge|HLSDK|healthkit.cpp CWallHealth, h_battery.cpp CRecharge|structural translation|
|1042-1063|player_weaponstrip|HLSDK|CStripWeapons|translation|
|1065-1137|HL_Explode / ExplodeOffSurface / Vanish|HLSDK|ggrenade.cpp CGrenade::Explode|structural translation|
|1156-1167|HL_ClipVelocity|HLSDK (Quake heritage)|pm_shared.c PM_ClipVelocity ≡ Quake sv_phys.c ClipVelocity|compact restatement; HLSDK has the same code, so the HLSDK licence covers it|
|1169-1239|Detonate / ExplodeTouch / BounceTouch|HLSDK|ggrenade.cpp (HLSDK comments 1217, 1227)|structural translation|
|1241-1390|HL_GrenadeMove / in_world / grenade_think|CLEAN (implements GoldSrc MOVETYPE_BOUNCE behaviour)|not Quake SV_Physics_Toss/SV_FlyMove (different loop and constants)|own integrator|
|1392-1461|HL_GrenadeSpawn / ShootTimed / ShootContact|HLSDK|ggrenade.cpp ShootTimed/ShootContact|translation|
|1463-1737|monster_tripmine|HLSDK + own beam slots (1486-1526)|tripmine.cpp CTripmineGrenade|structural translation|
|1549-1562|TE_LIGHTNING2 beam write|QUAKE-INTERFACE|Quake protocol svc_temp_entity/TE_LIGHTNING2 (as in QuakeC W_FireLightning)|protocol-forced message sequence; replace in hl64|

Quake bindings: progs (pr_global_struct trace_* globals, G_SELF/G_OTHER/G_Func/G_Str/G_SetStr/G_TIME, EDICT_TO_PROG/PROG_TO_EDICT, NUM_FOR_EDICT/EDICT_NUM, g_* builtins incl. g_droptofloor, g_traceline, g_pointcontents, g_vectoyaw, g_spawn, g_remove, SUB_Remove, SUB_UseTargets, T_Damage, g_damage_type/attacker, g_activator, func_t); sv (sv.datagram, sv.edicts, sv.active, svs.maxclients, SV_SpawnServer, sv_gravity cvar); msg/svc (MSG_WriteByte/Coord/Long, svc_hldamage, svc_temp_entity, TE_LIGHTNING2, sizebuf_t); cmd (Cmd_Argc/Argv); con (Con_Printf/DPrintf); cvar (Cvar_VariableValue); Q_strcasecmp; AngleVectors; trace_t; IT_SUIT items bit.

Automated hits: QCTRANS e 1015-1019 ← g_buttons.c 238-240 (angles zero + pusher spawn boilerplate in our own HL button code inside qctrans): false positive. QCTRANS e 1554-1557 ← g_weapons.c 683 (TE_LIGHTNING2 writes): protocol interface, not copied logic. QCTRANS e 1640-1641, 1701-1703, k16 1103-1104 ← game.c 550 (Con_Printf origin formats): false positive. HLSDK/QUAKE e 1408 ← view.cpp 200 / sv_phys.c 1411 (atan2 pitch formula): generic. QUAKE n 1559-1562 ← sv_main.c (MSG_WriteCoord triple): interface. QCTRANS k16 1575-1578 ← g_weapons.c tripmine explode (our own HL code in qctrans): false positive. QCTRANS n 1712-1720 ← g_monsters.c: false positive. HLSDK n 791-802 ← ev_hldm.cpp: generic. XASH n 325-327: loop, false positive. comment_overlap 1217 ← ggrenade.cpp:225: HLSDK comment, consistent with the HLSDK verdict.

##### source/hl/hl_items.h (97 lines; git: untracked)
Verdicts: CLEAN, QUAKE-INTERFACE (edict_t, vec3_t prototypes). HL_IT_LONGJUMP is our bit.
Automated hits: HLSDK n 75-93 ← bsp5.h, QUAKE n ← d_local.h (prototype lists): false positives.

##### source/hl/hl_weapons.c (1879 lines; git: d2bc31a, modified in worktree)
Verdicts: HLSDK, QUAKE-INTERFACE, plus a 4-line Quake idiom. History: d2bc31a created a wheel/bucket table for the Quake weapons. 8ca78f4 ported the HL Uplink weapons, then da63eca, ae46c44 and working-tree edits. The file never held an id file copy.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|32-61|sequence enums, skill|HLSDK data|weapons.h anim enums / skill.cfg|facts|
|65-161|state struct, StatsLine, HUD weapon/ammo rect tables|CLEAN (Uplink weapon_*.txt data)|—|own/data|
|163-224|pickup classes, HL_ViewModel, ClassModels|HLSDK data + own|LINK_ENTITY_TO_CLASS names|facts|
|226-299|input bridge, HL_BucketDir, HL_WheelPick|CLEAN|—|own N64 wheel|
|303-306|crandom|CONTAINS-QUAKE (de minimis)|Quake weapons.qc crandom (also qctrans)|renamed one-line helper; used for spread at 636-638|
|308-396|HL_SendAnim / DeployAnim / SyncClient|CLEAN / QUAKE-INTERFACE|—|own network bridge onto Quake clientdata|
|398-535|ownership, clip/ammo pointers, max carry, default give, IsUseable, CanHolster, NextBestWeapon|HLSDK|weapons.cpp/h, gamerules GetNextBestWeapon|structural translation|
|537-613|flashlight|HLSDK|player.cpp FlashlightTurnOn/Off, UpdateClientData (comment 539-540 HLSDK-identical, per comment_overlap)|structural translation|
|615-674|HL_TraceBullets|HLSDK (+Quake idiom)|player.cpp FireBulletsPlayer + ev_hldm EV_HLDM_FireBullets; spread loop also matches Quake weapons.qc FireBullets|merged lineage; use the HLSDK shared-random spread form|
|676-1295|crowbar, glock, python, mp5, shotgun, M203, hand grenade, reload, shotgun reload, idle|HLSDK|crowbar.cpp, glock.cpp, python.cpp, mp5.cpp, shotgun.cpp, handgrenade.cpp, weapons.cpp DefaultReload/ItemPostFrame|structural translation|
|1297-1584|select/holster/retire/remove/give/give ammo/AddPrimaryAmmo/precache|HLSDK + own N64 precache (1431-1466)|weapons.cpp, player.cpp GiveNamedItem/RemoveAllItems|translation|
|1586-1760|HL_MenuUpdate, Primary/Secondary dispatch, HL_Weapons_Think|HLSDK (ItemPostFrame, ImpulseCommands) + own wheel/impulse mapping|weapons.cpp ItemPostFrame, player.cpp ImpulseCommands|structural|
|1762-1879|map weapon pickups|HLSDK|weapons.cpp CBasePlayerItem DefaultTouch/FallInit/FallThink|translation|

Quake bindings: progs (pr_global_struct v_forward/v_right/v_up and trace_*, G_TIME, G_SELF/G_OTHER/G_Func/G_Str/G_SetStr, PROG_TO_EDICT, EDICT_NUM, g_makevectors, g_aim (Quake autoaim builtin), g_traceline, g_precache_model, g_stuffcmd, g_setmodel/size/origin, g_remove, T_Damage, g_damage_type, ED_NewString); entvars mirroring into Quake clientdata (v.items, weapon, weaponmodel, weaponframe, currentammo, ammo_nails/rockets/shells/cells, EF_MUZZLEFLASH, EF_DIMLIGHT, IT_SUIT); sv (SV_PrecacheContext/GetContext, SV_WorldMapClosing, SV_ModelIndex, sv.active, svs.maxclients); Cvar_VariableValue/SetValue; Con_Printf; Q_strcasecmp.

Automated hits: QCTRANS e 784-790 and 851-859 ← g_weapons.c 303-357. Those qctrans lines are our own earlier Uplink weapon draft (HL_Sound_Play pl_gun/hks picks, not Quake QC), so false positive vs Quake. QCTRANS e 298-315 / n 634-639 ← g_monsters.c: generic loops, false positive. HLSDK/QUAKE n 409-416, 433-441 ← input_goldsource.cpp / in_win.c (switch returning pointers): false positive. XASH n 435-441 ← gnm_shim: false positive. LIBDRAGON 138-143: data table, false positive.

##### source/hl/hl_weapons.h (152 lines; git: d2bc31a, modified in worktree)
Verdicts: HLSDK constants (19-58: weapon ids, *_MAX_CARRY, clip sizes, damage values from weapons.h/skill.cfg), CLEAN own types (61-152). QUAKE-INTERFACE: HL_WEAPON_SUIT_BIT = IT_SUIT.
Automated hits: QCTRANS n 136-148 ← game.h, HLSDK n 139-148 ← hud_servers_priv.h (prototype lists): false positives.

##### source/hl/hl_movers.c (1784 lines; git: untracked)
Verdicts: HLSDK, QUAKE-INTERFACE, de-minimis QCTRANS. Every mover follows its HLSDK class line for line in our Quake-edict idiom: doors.cpp CBaseDoor/CRotDoor/CMomentaryDoor, buttons.cpp CMomentaryRotButton/ButtonSound, plats.cpp CFuncTrain/CGunTarget, bmodels.cpp CFuncRotating/CPendulum/CFuncWallToggle, triggers.cpp CLadder, pm_shared.c PM_LadderMove. The HLSDK-distinctive markers are present: size-2 lip formula, door-swing cross product, realign hack, corner centring, fan friction/attenuation, pendulum damp.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|31-44|brush_show, key_render_visible|CLEAN|—|own|
|49-58|hl_linear_done|HLSDK; CONTAINS-QCTRANS (text-identical)|HLSDK doors.cpp/subs.cpp CBaseToggle::LinearMoveDone; text = qctrans g_subs.c SUB_CalcMoveDone (Quake subs.qc)|renamed copy of qctrans using QuakeC field names finaldest/think1; logic identical to HLSDK|
|61-71|hl_angular_done|HLSDK; CONTAINS-QCTRANS (text-identical except added link)|CBaseToggle::AngularMoveDone; qctrans SUB_CalcAngleMoveDone|as above|
|73-120|call_done_now, HL_LinearMove, HL_AngularMove|HLSDK|subs.cpp CBaseToggle::LinearMove/AngularMove (immediate-done when already there)|translation; differs from Quake SUB_CalcMove|
|122-166|AxisDir, AxisDelta, ShouldToggle, crush|HLSDK|subs.cpp/cbase.h|translation|
|168-226|door/button sound tables, ButtonSound|HLSDK (data)|doors.cpp Precache, buttons.cpp ButtonSound|data translation|
|228-537|CBaseDoor/CRotDoor|HLSDK|doors.cpp DoorHitTop/HitBottom/GoUp/GoDown/Activate/Use/Touch/Blocked/Spawn, CRotDoor::Spawn|structural translation|
|539-609|momentary_door|HLSDK|doors.cpp CMomentaryDoor|structural translation|
|611-803|momentary_rot_button|HLSDK|buttons.cpp CMomentaryRotButton|structural translation|
|805-1021|func_train|HLSDK|plats.cpp CFuncTrain Wait/Next/Use/Blocked/Activate/Spawn|structural translation|
|1023-1169|func_guntarget|HLSDK|plats.cpp CGunTarget|structural translation|
|1171-1404|func_rotating|HLSDK (+own rotating_wrap 1193-1201, parse_vec 1329-1334)|bmodels.cpp CFuncRotating|structural translation|
|1406-1562|func_pendulum|HLSDK|bmodels.cpp CPendulum|structural translation|
|1564-1595|func_wall_toggle|HLSDK|bmodels.cpp CFuncWallToggle|translation|
|1597-1655|ladder roster|CLEAN / QUAKE-INTERFACE|triggers.cpp CLadder::Spawn (concept)|own storage|
|1657-1703|HL_LadderFind|HLSDK semantics, own expression + QUAKE-INTERFACE|pm_shared.c PM_Ladder; hull offset 1690-1697 follows the Quake world.c SV_HullForEntity convention|interface formula, not copied text|
|1705-1784|HL_LadderMove|HLSDK|pm_shared.c PM_LadderMove (2059-2187)|structural translation; trace via SV_RecursiveHullCheck instead of PM_TraceModel|

Quake bindings: progs (G_SELF/G_OTHER/G_SetSelf, G_Func/G_Call/G_GetFunc, G_Str/G_SetStr, EDICT_TO_PROG/PROG_TO_EDICT, pr_global_struct v_forward, entvars state/pos1/pos2/aflag/lefty/waitmin/waitmax/t_width/height/finaldest/finalangle/think1/ltime as field storage, g_setorigin/setmodel/setsize, g_sound, g_find+FOFS, g_precache_sound, g_makevectors, g_vlen, g_normalize, G_IsWorld, G_ClassIs, T_Damage, g_damage_type, SetMovedir, ED_Free); sv/world (SV_LinkEdict, SV_HullPointContents, SV_PointContents, SV_RecursiveHullCheck, sv.edicts, sv.models, sv.model_precache, hull_t, model_t, mod_brush, trace_t, MOVETYPE_PUSH ltime clock relied on SV_PushMove/SV_PushRotate); Con_Printf; AngleVectors.

Automated hits (the 742 QCTRANS tokens):
- e 49-58 and 61-67 ← g_subs.c 55-61 / 104-108 (SUB_CalcMoveDone / SUB_CalcAngleMoveDone, Quake subs.qc): real but de minimis. Recorded as CONTAINS-QCTRANS 21 lines; HLSDK has the same statements.
- e 133-136, 1370-1374, k16 129-136, 1374-1377, n 1366-1377 ← g_buttons.c 284-292: that region is SP_func_rot_button, our own HLSDK CBaseToggle::AxisDir code inside the qctrans file. HLSDK sibling, not Quake.
- e 484-493 (+n), 514-516, 786-788, k16 768-779, 1550-1552 ← g_buttons.c 255-312 (HL_SP_func_button / SP_func_rot_button defaults and pos1/pos2): our own HLSDK code in qctrans; false positive vs Quake.
- k16 253-255 ← g_buttons.c 211-213 (hlb_rearm_touch, ours): false positive.
- e 403-409 ← g_doors.c 31 (door_blocked damage + reverse): the HLSDK CBaseDoor::Blocked has the same shape (Quake heritage inside HLSDK). Ours follows HLSDK (dmg guard, DMG_CRUSH, multi-door realign loop). HLSDK; false positive.
- e 479-482, 518-521, 793-797, 1154-1157, 1377-1381, 1535-1539, 1618-1621 ← g_doors.c 392-394 (solid/movetype/setorigin/setmodel boilerplate): API boilerplate also in every HLSDK Spawn; false positive.
- e 969-973, 981-986 ← g_plats.c 210-245 (train target lookup / pos1): HLSDK CFuncTrain::Activate has the same lookup (Quake heritage). Ours centres on the corner and uses WAIT_RETRIGGER as in HLSDK. False positive.
- k16 1492-1508 ← g_buttons.c 19-51: generic think scheduling; false positive.
- QUAKE n 1676-1685 ← world.c 290-295 and QCTRANS n 1678-1685 ← g_triggers.c 582-586 (AABB overlap test): generic; false positive.
- HLSDK n 193-223 ← buttons.cpp ButtonSound and HLSDK n 1770-1779 ← pm_shared.c PM_LadderMove: real HLSDK (expected).

##### source/hl/hl_movers.h (62 lines; git: untracked)
Verdicts: CLEAN, QUAKE-INTERFACE (edict_t, vec3_t, gfunc_t).
Automated hits: XASH e 10 ← xash3d_mathlib.h VectorSet (one-line 3-component set macro): generic, de minimis. QCTRANS n 10-23 ← g_local.h and HLSDK n 13-26 ← bsp5.h (prototype lists): false positives.

##### source/hl/hl_breakable.c (632 lines; git: untracked)
Verdicts: HLSDK, QUAKE-INTERFACE. This is a structural translation of func_break.cpp CBreakable/CPushable, with HLSDK comments preserved: "Don't fire something that could fire myself" (278), "can be broken when run into" (427), "can be broken when stood upon" (444).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|23-113|SF flags, Materials enum, pSpawnObjects, sound sets|HLSDK data|func_break.cpp 37-50 and MaterialSoundPrecache lists|data copy (exact HLSDK hit 38-61)|
|127-181|material/IsBreakable/irand/set helpers|CLEAN|—|own|
|183-226|Precache, DamageSound, HL_BreakableMaterial|HLSDK|CBreakable::Precache/DamageSound|translation|
|227-255|break_drop_riders|HLSDK semantics, own loop|CBreakable::Die UTIL_EntitiesInBox FL_ONGROUND|own expression|
|257-470|Die, Use, TraceAttack, TakeDamage, BreakTouch, KeyValue/Spawn|HLSDK|func_break.cpp|structural translation|
|472-632|SP_func_breakable, CPushable Move/Touch/Use/Spawn|HLSDK|func_break.cpp CPushable|structural translation|

Quake bindings: progs (G_SELF/G_OTHER/G_SetSelf, G_Func, G_Str/G_SetStr, PROG_TO_EDICT, NUM_FOR_EDICT/EDICT_NUM, pr_global_struct trace_ent/trace_endpos/v_forward, g_sound, g_random, g_setorigin/setmodel, g_precache_sound, g_makevectors, g_vlen, g_normalize, T_Damage, g_damage_type, g_activator, SUB_Remove, entvars cnt/impulse/lefty/noise4/mangle/movedir/pausetime/aflag as field storage); sv (SV_LinkEdict, sv.num_edicts, svs.clients, client_t cmd.forwardmove, MOVETYPE_PUSHSTEP relies on Quake SV_Physics); Cvar_VariableValue; Con_Printf.

Automated hits: HLSDK e 38-61 ← func_break.cpp 37-50: real (spawn object table). QCTRANS e 480-486 / k16 ← g_doors.c 391-394: spawn boilerplate, false positive. QCTRANS e 544-545 ← g_combat.c 406 (2-D speed sqrt): generic; it is CPushable::Move's length formula, false positive. QCTRANS k16 322-324 ← g_subs.c 35-36 (copy angles, makevectors): generic, false positive.

##### source/hl/hl_levelstate.c (2396 lines; git: untracked)
Verdicts: CLEAN, QUAKE-INTERFACE. This is our own per-(map, lump) dehydration/persistence design: gone bitsets, FNV fingerprints, compact monster/entity/aux records, reference fix-ups, runtime records, twin handoff. It is **not** derived from Xash sv_save.c or HLSDK save/restore. Nothing resembles ENTITYTABLE, SAVERESTOREDATA, the TYPEDESCRIPTION field tables, landmark-relative save of all entities, or the .HL1/.HL2 file layouts. Landmark handling (2305-2340, HL_LevelStateTwin) is a plain offset add with our own semantics.

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
|1-160|record types, ls_refofs offsetof table|CLEAN / QUAKE-INTERFACE (entvars_t offsets)|xash sv_game.c / hlsdk util.cpp offset tables are unrelated|generic offsetof list|
|162-2340|all ls_* and HL_LevelState*|CLEAN|— (xash engine/server/sv_save.c has a different architecture)|own|
|2342-2396|console command, init|CLEAN|—|own|

Quake bindings: progs/QC (entvars_t field offsets, func_t think/think1/touch/use/blocked stored and restored as progs function indices, string_t, PR_SetEngineString, G_Str, EDICT_TO_PROG/PROG_TO_EDICT, NUM_FOR_EDICT/EDICT_NUM, ED_Free/ED_Alloc, pr_global_struct, g_activator/g_use_*, SUB_Remove, g_setmodel/setsize, G_GetFunc); sv (sv.time, sv.edicts, sv.num_edicts, sv.active, svs.maxclients, SV_LinkEdict, SV_InternString, SV_Move, SV_PrecacheContext/GetContext); host (Host_Error); cmd (Cmd_AddCommand/Argc/Argv); con/sys (Con_Printf/DPrintf, Sys_Printf/Error).

Automated hits: HLSDK/XASH n 48-53 ← util.cpp / sv_game.c (offsetof tables): false positive. QCTRANS n 920-927, 927-940; QUAKE n 922-927 ← QW pr_cmds.c; QUAKE n 1131-1137, 1145-1161, 1171-1177 ← QW sv_ents.c; HLSDK n 1147-1161 ← client.cpp; QCTRANS n 1147-1161, 1171-1176 ← g_client.c; XASH n 1171-1176, 1439-1447 ← sv_pmove.c; QCTRANS n 1439-1445; LIBDRAGON n 927-940. All are VectorCopy/field-assignment runs, false positives. QUAKE e 2134-2138 ← pr_edict.c 90-92 and QCTRANS e 2135-2138 ← game.c 109 (memset + edict loop idiom): false positive. QCTRANS n 2386-2388 ← game.c 550 (Con_Printf args): false positive.

##### source/hl/hl_levelstate.h (134 lines; git: untracked)
Verdict: CLEAN (own API; hl_runtime_info_t). Bindings: struct edict_s pointers only.
Automated hits: none.

##### source/hl/hl_logic.c (409 lines; git: untracked)
Verdicts: HLSDK, QUAKE-INTERFACE (light). The header cites hlsdk-portable triggers.cpp, buttons.cpp and lights.cpp.

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
|14-46|multi_manager use|HLSDK|triggers.cpp CMultiManager::ManagerUse (clone/thread semantics)|structural adaptation to logic records|
|48-99|trigger_relay, trigger_auto|HLSDK|triggers.cpp CRelay, CAutoTrigger, triggerstate KeyValue|translation|
|101-146|env_global|HLSDK|triggers.cpp CEnvGlobal|translation|
|148-291|multisource (incl. the "MultiSrc: used by non member" quirk)|HLSDK + own save/restore hooks (259-287)|buttons.cpp CMultiSource Use/Register/IsTriggered|structural translation|
|293-357|light / light_spot|HLSDK|lights.cpp CLight ("m"/"a" style strings, style >= 32)|translation|
|358-409|fluoro ambients, layer-open hook|CLEAN|—|own|

Quake bindings: g_lightstyle, g_ambientsound, g_precache_sound, g_activator, G_TIME, sv.edicts, Con_Printf/DPrintf.
Automated hits: none.

##### source/hl/hl_use.c (524 lines; git: untracked)
Verdicts: HLSDK, CONTAINS-QUAKE, CONTAINS-XASH (de minimis), QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|17-53|globals, HL_IsMap, HL_EntKey*, use_name|CLEAN|—|own|
|55-118|HL_UseEntity, HL_FireTargets, G_FireTargets|HLSDK semantics + own|cbase.cpp FireTargets / Use dispatch|own expression over the QC self/other globals|
|120-127|hl_delay_think|CONTAINS-QUAKE|quake/qw-qc/subs.qc DelayThink (180)|structural translation (QuakeC → C)|
|129-139|HL_UseTargets HL-map branch|CLEAN|—|delegates to hl_ents.c|
|140-176|HL_UseTargets Quake-map branch|CONTAINS-QUAKE|qw-qc/subs.qc SUB_UseTargets (DelayedUse spawn, centerprint + misc/talk.wav, killtarget loop that returns from the function)|structural translation; comments say "stock QC"|
|177-181|target fire|HLSDK/Quake shared|cbase.cpp SUB_UseTargets|generic|
|183-253|global state, endsection/newgame store|HLSDK|cbase.cpp CGlobalState (gGlobalState)|translation|
|255-299|HL_MasterTriggered|HLSDK|util.cpp UTIL_IsMasterTriggered|translation|
|301-357|trigger_endsection|HLSDK|triggers.cpp CTriggerEndSection|translation|
|359-368|caps|CLEAN|—|own|
|369-386|in_sphere|CONTAINS-XASH (de minimis)|xash engine/server/sv_game.c pfnFindEntityInSphere (1576-1586)|same per-axis loop and sign convention (origin minus absmin/absmax), renamed; generic math, replace|
|388-453|HL_PlayerUse|HLSDK|player.cpp CBasePlayer::PlayerUse (1569+): search radius 64, VIEW_FIELD_NARROW, clamp-to-box LOS, continuous/impulse/onoff caps|structural translation|
|455-463|HL_FireAndDie|HLSDK|triggers.cpp CFireAndDie|translation|
|465-524|ent_fire command, init|CLEAN|—|own|

Quake bindings: progs (G_SELF/G_OTHER/G_SetSelf/G_SetOther, G_Func/G_Call/G_GetFunc, SUB_Null, SUB_UseTargets, G_Str/G_SetStr, EDICT_TO_PROG/PROG_TO_EDICT, EDICT_NUM/NUM_FOR_EDICT, pr_global_struct v_forward, g_find+FOFS, g_spawn, g_remove, g_centerprint, g_sound, g_makevectors, G_ClassIs, G_IsWorld, g_activator, entvars delay/killtarget/message/noise/count); sv (sv.edicts, sv.num_edicts, sv.active, svs.maxclients); host (Host_EndSection); cmd (Cmd_AddCommand/Argc/Argv, Cbuf_AddText); cvar (cvar_t hl_trace, Cvar_RegisterVariable); Con_Printf/DPrintf; Q_strcasecmp/Q_strncpy.

Automated hits: XASH n 377-384 ← sv_game.c 1581-1582: real but de minimis. Recorded as CONTAINS-XASH 369-386; hl_env.c HL_BoxDistSq is the clean replacement.

##### source/hl/hl_use.h (105 lines; git: untracked)
Verdicts: HLSDK constants (USE_OFF/ON/SET/TOGGLE 16-19, FCAP_IMPULSE/CONTINUOUS/ONOFF_USE values 72-74, GLOBAL_OFF/ON/DEAD 82-84, all from cbase.h), CLEAN own API, QUAKE-INTERFACE (edict_t).
Automated hits: none.

##### source/hl/hl_hud.c (897 lines; git: d2bc31a, modified in worktree)
Verdicts: HLSDK (cl_dll client HUD), QUAKE-INTERFACE. History: d2bc31a added a 165-line HUD that replaced the sbar.c call. 794051d and later commits added the HLSDK layout math. No sbar.c code was carried over (sbar.c only routes to HL_DrawHUD).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|25-83|pickup history store, HL_Pickup_f|HLSDK|ammohistory.cpp HistoryResource::AddToHistory|translation|
|85-114|flashlight messages|HLSDK|flashlight.cpp MsgFunc_Flashlight/FlashBat|translation|
|116-231|damage icons, CalcDamageDirection, MsgFunc_Damage|HLSDK|health.cpp CHudHealth UpdateTiles/CalcDamageDirection/MsgFunc_Damage|structural translation|
|233-302|init, N64 colour/blit helpers, RightEdge|CLEAN|—|own|
|304-347|HL_DrawHudNumber|HLSDK|hud_redraw.cpp CHud::DrawHudNumber|structural translation (DHN flags)|
|352-402|active weapon/reserve/crosshair rect|CLEAN (Uplink data)|—|own|
|404-471|suit battery, flashlight draw|HLSDK|battery.cpp Draw, flashlight.cpp Draw|translation|
|473-575|DrawDamage / DrawPain|HLSDK|health.cpp DrawDamage/DrawPain|translation|
|577-770|health, ammo, number string, history draw|HLSDK|health.cpp, ammo.cpp, hud_redraw DrawHudStringReverse, ammohistory.cpp DrawAmmoHistory|translation|
|772-850|HL_DrawHUD|HLSDK layout + own|CHud::Redraw ordering|adapted|
|852-893|HL_DrawWheel|CLEAN|—|own N64 radial wheel|

Quake bindings: client (cl.time, cl.stats STAT_HEALTH/STAT_ARMOR, cl.items, cl.viewentity, cl.viewangles, cl.intermission, cls.state ca_connected, cl_entities); draw (Draw_SubPic/SubPicColor/SubPicRelease/Fill/Character, vid.width/height, scr_copyeverything); svc_hldamage; cmd (Cmd_AddCommand/Argc/Argv, Q_atoi); Cvar_VariableValue; Con_Printf; AngleVectors.
Automated hits: none.

##### source/hl/hl_hud_rects.h (128 lines; git: d2bc31a, modified in worktree)
Verdict: CLEAN (generated by tools/hlhud_bake from Uplink sprites/hud.txt; sprite names and rects are game-data facts). Embeds no copied code.
Automated hits: XASH n 5-65 ← img_dds.h (struct of shorts + enum list): false positive.

##### source/hl/hl_view.c (235 lines; git: untracked)
Verdicts: CLEAN (fade, title, message parsing), QUAKE-INTERFACE, plus one region that follows the Valve screen-shake algorithm. The header says the client behaviour was "reproduced from memory [INFERENCE]".

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|45-58|trace, clear|CLEAN|—|own|
|62-93|HL_View_UpdateShake (core)|Valve-algorithm match (HLSDK-family; not Quake; not Xash)|Xash engine/client/dll_int/cl_game.c pfnCalcShake (2185-2236) has the same skeleton but different next-shake/fraction/freq formulas. Our formulas (next = t + 1/freq; frac = (end - t)/duration; freq/frac; frac² · sin; amplitude decay by frametime/(duration·frequency)) match Valve's Source SDK CViewEffects::CalcShake [INFERENCE: Source SDK not in the reference corpora]|structural match to the Valve algorithm; recommend rewriting from a behavioural spec or moving to game/ under the HLSDK-family licence|
|94-118|trace print, ApplyShake|CLEAN|xash pfnApplyShake (trivial add)|generic|
|123-169|HL_View_FadeColor / DrawFade|CLEAN|xash cl_game.c 506-517 (signed fadeSpeed form)|independent linear ramp|
|173-235|HL_View_ParseMessage|CLEAN / QUAKE-INTERFACE|xash CL_ParseScreenShake/Fade use a different wire format|own protocol (floats)|

Quake bindings: client (cl.time); msg (MSG_ReadFloat/Byte/String, svc_hlshake/hlfade/hltitle); screen (SCR_CenterPrint, scr_centertime_off); host_frametime; Cvar_FindVar; Con_Printf; Draw_HLFade (r64_draw.c); rand().
Automated hits: none.

##### source/hl/hl_view.h (20 lines; git: untracked)
Verdict: CLEAN; QUAKE-INTERFACE (vec3_t, qboolean). Automated hits: none.

##### source/hl/hl_material.c (762 lines; git: untracked)
Verdicts: HLSDK, CONTAINS-QUAKE, CLEAN (cooked N64 path), QUAKE-INTERFACE.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
|34-75|codes, lower/strnicmp/isspace helpers|CLEAN|—|own|
|47-137|Mat_Load|HLSDK|pm_shared.c PM_InitTextureTypes + PM_SortTextures (memfgets, skip comments/non-alpha, toupper type, 12-char names, exchange sort)|structural translation|
|139-165|Mat_ForName|HLSDK|sound.cpp TEXTURETYPE_PlaySound prefix strip + pm_shared.c PM_FindTextureType binary search|structural translation|
|169-230|walk state, Mat_Node, Mat_Contents, Mat_Axis|CLEAN / QUAKE-INTERFACE (dclipnode_t, mplane_t, hull_t)|—|own|
|236-290|Mat_Walk|CONTAINS-QUAKE|first/source/world.c SV_RecursiveHullCheck (p1f/p2f/midf parameters, t1/t2 plane distances, frac = t1/(t1-t2), per-axis mid lerp, front-then-back recursion) combined with r_light.c RecursiveLightPoint (side = front<0; same-side shortcut; front recursion, then check node, then back)|structural translation with the same variable names; DIST_EPSILON removed, tail iteration replaces back recursion|
|292-388|Mat_Pick, Mat_NodeType|CLEAN / QUAKE-INTERFACE (mnode_t, msurface_t, surfedges/edges/vertexes)|— (HLSDK/engine uses texture-extent tests; ours uses vertex bounding boxes)|own|
|390-420|Mat_Surface, Mat_HasHull|CLEAN|—|own|
|422-446|mat_nobody, Mat_IsBody|HLSDK semantics|CBaseEntity::Classify / EV_IndexFromTrace|own expression|
|448-535|Mat_World (layer slab clipping, nearest hit)|CLEAN|—|own (N64 multi-layer)|
|537-553|HL_TextureType dispatch|CLEAN|sound.cpp TEXTURETYPE_PlaySound (TRACE_TEXTURE)|own|
|554-568|rotate start/end into model frame|CONTAINS-QUAKE|first/source/world.c SV_ClipMoveToEntity rotated-bmodel block (QUAKE2 ifdef; AngleVectors, temp, forward / -right / up dot products)|exact structure, renamed (comment 554 cites it)|
|569-572|surface call|CLEAN|—|own|
|576-617|sample table, set enum, precache|HLSDK (data)|sound.cpp TEXTURETYPE_PlaySound sample names/counts|data|
|619-696|Mat_PlaySound, HL_PlayTextureSound, HL_EV_PlayTextureSound|HLSDK|sound.cpp TEXTURETYPE_PlaySound (fvol/fvolbar/attn per type, breakable quieter, computer sparks), cl_dll ev_hldm.cpp EV_HLDM_PlayTextureSound|structural translation|
|698-762|hl_material test command, init|CLEAN|—|own|

Quake bindings: model (model_t, mod_brush, hull_t, dclipnode_t, mplane_t, mnode_t, msurface_t, mod->surfedges/edges/vertexes/texinfo, Mod_MakeHull0 leaf convention, sv.models, sv.worldmodel); sv/world (SV_Move, SV_ClipWorld/SV_ClipMoveToEntity conventions, SV_PrecacheContext/GetContext, SV_WorldMapClosing, sv.edicts, sv.active, trace_t, CONTENTS_*); files (COM_LoadTempFile, com_filesize); cmd (Cmd_AddCommand/Argc/Argv, Q_atof/Q_atoi); Cvar_VariableValue; Con_Printf/DPrintf; Sys_FloatTime; AngleVectors; G_Str.

Automated hits: QUAKE e 276-279 (and k16 n) ← world.c 648-652: **real**, part of the Mat_Walk CONTAINS-QUAKE region. QUAKE n 559-568 ← world.c 750-755: **real**, rotate-into-model block.

##### source/hl/hl_material.h (77 lines; git: untracked)
Verdict: HLSDK constants (CBTEXTURENAMEMAX 13 and CHAR_TEX_* letters from pm_materials.h; BULLET_* enumerator names from weapons.h, 15-41); CLEAN prototypes.
Automated hits: none.

##### source/hl/hl_input.h (30 lines; git: d2bc31a, modified in worktree)
Verdict: CLEAN (own N64/SDL input bridge globals). Bindings: none (IN_SDL_HudInsetRight is ours).
Automated hits: none.

#### Re-targeting notes (slice-wide)
- Every game-logic file uses the QuakeC-in-C idiom: `self->v.*` entvars as storage for HLSDK members, think/touch/use/blocked as progs function indices (G_Func/G_Call), the pr_global_struct trace_* and v_forward globals, g_* builtins, MOVETYPE_PUSH with the ltime clock, and T_Damage. Moving to hl64 game/ means mapping these onto an HLSDK-style CBaseEntity/entvars API. The HLSDK structure is already visible in the function-level comments, so the port is mechanical.
- Quake protocol/client couplings to replace: svc_hldamage/hlshake/hlfade/hltitle/hltent over MSG_*, TE_LIGHTNING2 (hl_items.c 1549-1562), Quake clientdata mirroring (hl_weapons.c 348-396), and cl.stats/cl.items/Draw_* (hl_hud.c).
- To clean before import: hl_material.c 236-290 and 554-568 (Quake), hl_use.c 120-176 (QuakeC subs.qc; drop the Quake-map branch entirely), hl_use.c 369-386 (Xash, trivial), hl_weapons.c 303-306 (Quake idiom), hl_movers.c 49-71 (QCtrans text; rewrite from HLSDK LinearMoveDone), and hl_view.c 62-93 (Valve shake algorithm, if it is to live in the MIT engine).


### A.AuditToolsStudio — Studio code, tools, model viewer, spikes


Slice: `source/{mod_studio.c,studio.h,studio_geom.c,studio_geom.h,studio_soft.c,studio_soft.h,studio_bake.h,alias_bake.h,ui_bake.h,world_atlas.h,world_tex_bake.h}`, all non-first-commit files under `tools/` (excluding `tools/deps/meshoptimizer`), `n64/port/modelviewer/**`, `n64/port/spike/**`, `n64/smoketest/**`. File list: 129 (listed in the table; enumeration saved at `ProvAudit/AuditToolsStudio_files.txt`). Audited on the working tree as of this run; reference trees as given in the brief. Output contains no copied code - only function names, line ranges and descriptions.

#### 1. Summary table


Totals (counted ranges): XASH 425 (studio_geom.c) + 322 (xash_ref_stream.c) = 747; HLSDK 425 + 322 + 98 (w64c.py Materials) + ~200 (studio.h format structs) = ~1045 (same code as XASH for the first two); QUAKE 20 (studio_soft.c) + 17 (soft_studio.c) + 115 (q64mapc.c) + 35 (bake_alias.c) + 5 (trim_alias_mdl.c) + 7 (viewer.c) + 10 (fputest.c) + 152 (w64c.py) + 73 (w64check.py) + 7 (w64seam.py) = 441; QCTRANS 8 (w64c.py lightstyle table, also in the QUAKE count). Everything not listed in those ranges is CLEAN / QUAKE-INTERFACE / DATA as per the table.

#### Legend and method

- Verdict vocabulary as assigned. "Lineage": Xash3D FWGS studio/mathlib code is itself derived from HLSDK (`cl_dll/StudioModelRenderer.cpp`, `studio_util.cpp`, `utils/mdlviewer/studio_render.cpp`); no Quake alias/BSP code is in the studio path. Where a region matches both Xash and HLSDK, both are listed; "Quake" is named only where the same code exists in `first/source` / `quake/` (Quake has no studio code, so studio regions never carry a Quake verdict).
- Evidence used: `evidence.md` (slice entries, saved in `AuditToolsStudio/evidence_slice.md`), `comment_refs.txt`, `comment_overlap.md` (no entry for any file of this slice), plus my own word-8-gram comment/docstring overlap over **all** slice files (py/sh/c/h/txt/md) vs all comments in Quake/Quake2/QW/Xash/HLSDK/hlsdk-portable (468,766 reference grams): only one hit, a power-of-two list in `tools/w64c/w64c.py` vs `quake2/game/q_shared.h` (false positive, a numeric run). So no copied comment prose anywhere in the slice.
- The token scanner is C-tokenised; Python/shell tools were **not** token-scanned. They were reviewed structurally: docstring/cite scan, `def` inventory, and a full read of every function that names a Quake/Xash/HLSDK counterpart (w64c.py, w64check.py, w64seam.py) plus the ones whose purpose could hide a ported algorithm (stitch_and_carve.py, mk_white_room.py, hl_campaign.py, analyze_hl_tex_atlases.py, bake_world_atlas.py, streamv2/analysis/*).
- No file in the slice carries a GPL/"quakegeneric contributors" or Id header (every `IdSoftware-header`/`GPL-header` = False in evidence): nothing to strip, but nothing to relicense either - headers must be added on import.
- Counts of "contaminated lines" are the sum of the region ranges below marked CONTAINS-*/HLSDK (generic one-line maths and file-format record layouts that are forced by the format are listed but not counted unless stated).

#### Summary of provenance findings (top-level)

1. **`source/studio_geom.c` (1010 lines): 425 lines are renamed translations of Xash3D/HLSDK studio code** (bone calc, quaternion/slerp, matrices, bone controllers, frame estimate, calc-rotations, setup-bones, chrome). The file header itself says "Algorithms ported from xash3d". NOT importable. Rest of the file (clock/blend, stream helpers, tricmd walk, compare) is ours.
2. **`tools/studio_compare/xash_ref_stream.c` (504 lines)**: second, independent-looking but equally translated copy of the same Xash/HLSDK functions (322 lines); the harness intentionally mirrors Xash. Keep out of the MIT repo.
3. **`source/studio.h`**: HLSDK/Xash MDL structs copied field-for-field (names, order, macro values) with comments removed. Format definitions: usable under the HLSDK licence in `game/`; the MIT engine should re-declare from the format spec (own names).
4. **Quake translations in tools**: `angle_vectors` (Quake `AngleVectors`) in `studio_soft.c` and `studio_compare/soft_studio.c`; `CalcSurfaceExtents` + `Mod_LoadFaces` flag/extents block in `q64mapc.c`; in `w64c.py` the `Mod_LoadTextures` animation sequencing, `CalcSurfaceExtents`, `RecursiveLightPoint`/`R_LightPoint`, and the Quake lightstyle string table; `SV_RecursiveHullCheck` in `w64check.py`; `AngleVectors` in `w64seam.py`; `CalcFov` in `n64/port/spike/fputest.c`; modelgen.h format definitions in `bake_alias.c`/`trim_alias_mdl.c`/`viewer.c`.
5. **HLSDK translation**: `tools/w64c/w64c.py` `Materials` class = Python port of `pm_shared.c` `PM_InitTextureTypes`/`PM_SortTextures`/`PM_FindTextureType` (1349-1446), reproduces its quirks on purpose. Belongs under the HLSDK licence if kept.
6. **Everything else is CLEAN** (format cookers/readers, own file formats, pipeline drivers, benchmarks) or QUAKE-INTERFACE (mod_studio.c, hl_headless.c, test_bsp_load.c).
7. **Cross-project provenance (not Quake/Xash/HLSDK)**: `n64/port/modelviewer/{hostfs_9p.c,hostfs_9p.h,sc64_9p_agent.py,sc64_link.c,sc64_session.py}` derive from `~/src/ports/homeworld/n64/*` (different project: confirm authorship/licence of that glue before importing; the Homeworld tree itself ships under the Sierra/Relic Homeworld licence, but the `n64/` glue there was added in commit a8f97a8 "n64: add fleet viewer and asset pipeline" - same-owner authorship is [INFERENCE] from the commit, not verified).
8. **Valve-derived data (not code)**: `n64/port/spike/streamv2/fs/dome.sct` (cooked HL map geometry), `tools/hl_world/examples/*.json|md` and `spike/streamv2/out/*.json` (stats/bounds from HL maps). Exclude from the new repo regardless of code verdicts.

---

#### source/

##### source/studio_geom.c (1010 lines; git: b6ea17c, modified in worktree)

Verdict: **CONTAINS-XASH + CONTAINS-HLSDK** (425 lines each; same ranges); QUAKE-INTERFACE only through `quakedef.h` types (`byte`, `vec3_t`) and `Mod_StudioAnimData` (line 418).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 14-34 | includes, standalone type shim | CLEAN | - | ours |
| 36-41 | SG_Short | CLEAN | - | ours (LE short read; Xash has `UnalignedShort` in mathlib but different) |
| 43-75 | SG_SinCos, SG_Sin | CLEAN (third-party constants) | Cephes `sinf`/`cosf` minimax constants named in comment 46 | ours; coefficients are Cephes (S. Moshier) - attribute. Not in reference trees; identification from the comment [INFERENCE] |
| 77-90 | SG_AngleQuaternion | CONTAINS-XASH, CONTAINS-HLSDK | `xash/public/xash3d_mathlib.h:538-558` AngleQuaternion (studio branch); `hlsdk/cl_dll/studio_util.cpp:134-157` AngleQuaternion | renamed copy: same 6 locals, same roll/yaw/pitch order, 4 output expressions token-identical (evidence XASH e 86-92 n=70) |
| 92-111 | SG_QuaternionAlign | CONTAINS-XASH, CONTAINS-HLSDK | `xash/public/xash3d_mathlib.c:283-305` QuaternionAlign; `hlsdk/cl_dll/studio_util.cpp:171-184` (inline in QuaternionSlerp) | renamed copy (a/b accumulators, flip test; evidence HLSDK e 95-103 n=91, XASH e 96-114 n=126) |
| 113-151 | SG_QuaternionSlerp (align + no-align slerp, antipodal branch) | CONTAINS-XASH, CONTAINS-HLSDK | `xash/public/xash3d_mathlib.c:312-370`; `hlsdk/cl_dll/studio_util.cpp:162-224` | renamed copy incl. the 4-component antipodal pattern and half-pi constants; comment 113/141 names Xash |
| 153-172 | SG_FromOriginQuat | CONTAINS-XASH, CONTAINS-HLSDK | `xash/public/matrixlib.c:64-` Matrix3x4_FromOriginQuat; `hlsdk/cl_dll/studio_util.cpp` QuaternionMatrix | renamed copy (quaternion->3x4 formula, same term order) |
| 174-188 | SG_Concat | CONTAINS-XASH, CONTAINS-HLSDK (also matches Quake `R_ConcatTransforms`, `first/source/mathlib.c`) | `xash/public/matrixlib.c:28-62`, `xash/ref/common/ref_math.c:25-66`; `hlsdk/cl_dll/studio_util.cpp:100-125` | token-exact (699 tokens) modulo parameter names/spacing. The 3x4 product is mathematically forced, but term order/layout is identical; same expansion exists in Quake mathlib.c - rewrite as a loop |
| 190-225 | SG_VectorTransform, SG_VectorIRotate, SG_Normalize, SG_Cross, SG_Dot | generic maths (not counted) | `xash3d_mathlib.h:246-253`; HLSDK `CrossProduct`/`VectorTransform`; Quake `CrossProduct` | one-line algebraic identities; flagged by scanner only because the expansions are unique forms of the same identity |
| 229-317 | SG_CalcBone (RLE channel decode, pos lerp, angle->quat slerp) | CONTAINS-XASH, CONTAINS-HLSDK | `xash/public/xash3d_mathlib.c:434-511` R_StudioCalcBones; `hlsdk/cl_dll/StudioModelRenderer.cpp:178-330` (StudioCalcBoneQuaterion/Position) | structural translation: same channel loop, same valid/total span walk, same four-way v1/v2 selection, same scale/value/adj combine; changes = LE short reads, a span guard (255-271), loop shape. Evidence: XASH e 240-295, HLSDK e 266-268 |
| 319-365 | SG_CalcBoneAdj | CONTAINS-XASH, CONTAINS-HLSDK | `xash/ref/gl/gl_studio.c:658-704` R_StudioCalcBoneAdj; `hlsdk/cl_dll/StudioModelRenderer.cpp:111-175` | reduced structural translation: mouth/index<4 split, start/end lerp, STUDIO_*R/STUDIO_* switch with deg->rad; dadt interpolation and RLOOP wrap simplified. Evidence XASH/HLSDK e 347-354 n=53 |
| 367-391 | SG_WrapFrame | CONTAINS-XASH, CONTAINS-HLSDK | `xash/ref/gl/gl_studio.c:577-589` R_StudioEstimateFrame tail; `hlsdk/cl_dll/StudioModelRenderer.cpp:~740-765` | same branch structure and the 1.001 clamp constant; loop reformulated as while |
| 399-437 | SG_CalcPose | CONTAINS-XASH, CONTAINS-HLSDK | `xash/ref/gl/gl_studio.c:720-753` R_StudioCalcRotations; `hlsdk` StudioCalcRotations:583- | per-bone calc loop + three motion-bone zeroing statements (431-436) identical in meaning and order |
| 439-517 | StudioGeom_SetupBones | CONTAINS-XASH, CONTAINS-HLSDK | `xash/ref/gl/gl_studio.c:816-900` R_StudioSetupBones; `xash3d_mathlib.h:583-593` R_StudioSlerpBones; `hlsdk` StudioSetupBones:774- | structure (sequence clamp, calc rotations, previous-sequence slerp blend, local matrix from quat+pos, parent==-1 test then concat) follows the Xash function; blend weighting/params are ours |
| 519-584 | StudioGeom_ClockFrame, StudioGeom_ClockBlend | CLEAN | emulates GoldSrc client behaviour only | ours (behaviour-derived, no counterpart text) |
| 586-623 | SG_SetupChrome | CONTAINS-XASH, CONTAINS-HLSDK | `xash/ref/gl/gl_studio.c:1116-1155` R_StudioSetupChrome; `hlsdk/utils/mdlviewer/studio_render.cpp:441-` StudioModel::Chrome | structural translation: same vector chain (negated origin + bone origin, normalise, cross, normalise, cross, normalise, inverse-rotate by bone, dot with normal, (n+1)*32); fallbacks added |
| 627-701 | draw-stream init/reserve/reset/free/push, SG_TexIndex | CLEAN | - | ours |
| 703-880 | SG_EmitSubmodel (vertex transform + tricmd strip/fan walk expanded to triangles) | CLEAN (format-driven; influence from DrawPoints noted) | cf. `xash/ref/gl/gl_studio.c:998-1104, 1975-2075` | tricmd stream layout and strip/fan semantics are the MDL format; expression (guards, triangle expansion into an explicit stream, idx arrays) is independent; no token hit |
| 882-914 | StudioGeom_Emit | CLEAN | body-part index selection is the documented `(body / base) % nummodels` | ours |
| 916-1010 | StudioDrawStream_Compare | CLEAN | - | ours (test oracle) |

Contaminated lines: XASH 425 (77-90, 92-111, 113-151, 153-172, 174-188, 229-317, 319-365, 367-391, 399-437, 439-517, 586-623); HLSDK 425 (same); QUAKE 0 (the Quake `R_ConcatTransforms` coincidence is covered by the SG_Concat row); QCTRANS 0.

Quake bindings: `quakedef.h` (types `byte`, `vec3_t`), `Mod_StudioAnimData` (declared in studio.h, defined in mod_studio.c). Everything else is standalone (STUDIO_GEOM_STANDALONE shim).

Automated hits: HLSDK/XASH n/e 84-103, 120-136, 174-188, 197-202, 240-295, 266-270, 347-354, 490-495 = real (rows above). HLSDK/QUAKE n 215-220 (SG_Cross) = false positive/identity maths. QCTRANS e 206 (sqrtf normalise) = false positive. LIBDRAGON e 222-225 (SG_Dot) = false positive. QUAKE e 295 (`quake2/game/q_shared.c` vector compare), QUAKE n 297-299 (`r_sprite.c` lerp) = false positive (generic lerp/compare, and the compare itself is part of the Xash/HLSDK CalcBone translation).

##### source/studio.h (287 lines; git: dcce7e3, modified)

Verdict: **HLSDK (+XASH variant) format definitions**; QUAKE-INTERFACE in the runtime half.

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
| 5-26 | header guard, STUDIO_VERSION/IDSTUDIOHEADER/IDSEQGRPHEADER, MAXSTUDIO*, STUDIO_* flags | HLSDK/XASH | `hlsdk-portable/engine/studio.h:17-37`, `xash/engine/studio.h:31-33`, `hlsdk/engine/studio.h:36-70,213-318` | macro names and values copied (IDST/IDSQ macro lines incl. the "IDST LE" gloss; guard name STUDIO_H = hlsdk-portable). Subset (ours picks 8 of the limits) |
| 28-64, 77-125, 129-207 | studiohdr_t, mstudiobone_t, mstudiobonecontroller_t, mstudioseqdesc_t, mstudioevent_t, mstudioattachment_t, mstudioseqgroup_t, mstudioanim_t, mstudioanimvalue_t, mstudiobodyparts_t, mstudiotexture_t, mstudiomodel_t, mstudiomesh_t | HLSDK/XASH | `hlsdk/engine/studio.h:60-318` | field-for-field verbatim (names, order, types) with all trailing comments removed; `ident` for `id`, split `unsigned short flags/unused` in mstudiotexture_t as in hlsdk-portable/xash (not original HLSDK). Evidence HLSDK e 36-71, 71-91, 94-113, 113-125, 135-207 |
| 66-75 | studioseqhdr_t + comment | HLSDK | `hlsdk/engine/studio.h:~130-140` | struct copied; comment (66-68, IDSQ start-of-file note) ours |
| 209-285 | Mod_LoadStudioModel/Header/Prefetch/Unload/pool API (`struct model_s`, `N64_PORT` pool decls) | QUAKE-INTERFACE (ours) | - | ours |

Contaminated lines: HLSDK/XASH ~200 (8-207 excluding our own comments at 66-68 and 127-128). Disposition of evidence: all real except HLSDK n 36-91 vs `dlls/sound.cpp` and QUAKE n 36-64 vs `quake2/qcommon/qfiles.h` (false positives: normalised field-list shapes).

Import: the file formats are interfaces, so a clean rewrite is easy (declare from the format spec, own names/comments); alternatively keep as-is in `game/` under the HLSDK licence.

Quake bindings (209-285): `struct model_s`, `Mod_*` API, `byte`, `q64` pool types (`N64_PORT`).

##### source/studio_geom.h (130 lines; git: b6ea17c, modified)

CLEAN. Types for the draw stream, draw params, clock; prototypes. Bindings: includes `studio.h` only. Comments describe Xash faithfulness as intent (lines 2-6, 52, 86). Evidence: no hit. Importable once studio.h is replaced.

##### source/studio_soft.c (605 lines; git: b6ea17c)

Verdict: **CONTAINS-QUAKE (20 lines)**; QUAKE-INTERFACE only via `quakedef.h` (`byte`, `vec3_t`).

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 32-153 | framebuffer init/clear/compare/PPM writers | CLEAN | - | ours |
| 155-268 | sample_tex, mod_color, SoftFB_Tri (edge-function raster, z-test, nearest texture) | CLEAN | generic barycentric rasteriser, no Quake d_polyse structure | ours |
| 272-285 | SoftStudio_DefaultOpts | CLEAN | comment 278 cites Xash (behaviour) | ours |
| 287-306 | angle_vectors | CONTAINS-QUAKE | `first/source/mathlib.c:295-312` AngleVectors (also hlsdk common mathlib) | renamed copy: same sin/cos order (yaw, pitch, roll), forward/right/up expressions incl. the odd `-1 * ... + -1 * ...` forms are character-identical (evidence QUAKE n 295-306 n=148, HLSDK n same) |
| 308-327 | entity_xform, n64_quantize_pos | CLEAN | - | ours |
| 329-355 | project_vert | CLEAN | plain perspective divide | ours |
| 357-567 | skins build, chrome darken, SoftStudio_DrawStream | CLEAN | pitch flip with "stupid quake bug" comment (466) mirrors Xash gl_studio.c:540 (one assignment; comment text echoed) | ours; rewrite the comment |
| 569-605 | SoftStudio_RenderFrame (path 1 hooks Xash ref) | CLEAN | - | ours |

Contaminated lines: QUAKE 20 (287-306). Other hits: XASH n 488-494 (s_dsp.c) = false positive (zero-init of three floats). Quake bindings: type shims only.

##### source/studio_soft.h (82 lines; git: b6ea17c)

CLEAN (types/prototypes; comments mention Xash/Quake bug conventions only).

##### source/studio_bake.h, source/alias_bake.h, source/ui_bake.h, source/world_atlas.h, source/world_tex_bake.h

CLEAN. Own on-disk layouts (STB2 v4 header/skin/vert/tri/meshlet; AMB2; UI8; WATS; WCI4); no Quake/Xash/HLSDK text or struct copy. `studio_bake.h:79` comment says chrome bone "matches Soft/Xash DrawPoints" (cite). `alias_bake.h` describes how the N64 loads Quake alias MDLs (interface only). No hits in evidence. Lines: 123, 58, 34, 95, 45.

##### source/mod_studio.c (2171 lines; git: dcce7e3, modified)

Verdict: **QUAKE-INTERFACE**; no CONTAINS-* region.

| lines | function/block | verdict | counterpart | basis |
|---|---|---|---|---|
| 1-125 | header, model.c NL_* mirror, prefetch/seqbag/pool structs | QUAKE-INTERFACE | `model.c` needload flags (values only) | ours |
| 127-176, 180-812 | studio pool, eviction (StudioClassify/StudioBlockCost/StudioCompact/StudioMakeRoom...), pool stats | QUAKE-INTERFACE | no counterpart (N64 residency design) | ours |
| 815-836 | Mod_StudioHeader | QUAKE-INTERFACE | - | ours |
| 838-1250 | seqgroup bags, path derivation, async IDSQ reads, Mod_StudioAnimData | QUAKE-INTERFACE | behaviour of `xash/engine/common/mod_studio.c:541-621` (group file name pattern NN.mdl via `%s%i%i.mdl`, comment 940) | ours: name pattern is a file-naming convention; code structure (bags, LRU, async) is independent |
| 1250-1422 | SwapStudioHdr/Bones/BoneControllers/Seqs/Textures/Bodyparts | QUAKE-INTERFACE (format code) | Xash uses a table-driven swap (`xash/engine/common/mod_studio.c:60-150`, `le_struct_*`); Quake alias swap in `model.c` | independent: manual field loops using Quake `LittleLong/LittleFloat/LittleShort`; evidence hits XASH n 1308-1336/1354-1360 vs `filesystem/wad.c`, HLSDK n 1291 vs `bspfile.c`, QUAKE n 1354 vs `pr_edict.c` = false positives (repeated swap-assignment shape) |
| 1426-1760 | companion `<name>t.mdl` merge, trim, StudioAllocBlob, StudioFinishLoad, Mod_LoadStudioModel, Mod_LoadStudioFile | QUAKE-INTERFACE | behaviour of `xash/engine/common/mod_studio.c:1139-1230` (numtextures==0 -> load T.mdl) | independent expression (relocation of companion tables into the blob tail vs Xash realloc+memcpy); Mod_LoadStudioModel/File are not translations of Quake `Mod_LoadAliasModel` (no hunk alloc/frames/skins code) |
| 1762-2171 | prefetch pump, unload, resident reports | QUAKE-INTERFACE | - | ours |

Contaminated lines: 0.

Quake bindings: model_t fields (`type`, `cache.data`, `needload`, `name`, `mins/maxs/radius/numframes/flags`), `mod_known`/`mod_numknown`/`Mod_FindName`, `NL_*`, `MAX_MODELS`/`MAX_MOD_KNOWN`; server: `sv.active/edicts/num_edicts/models/name`, `svs.maxclients`, `edict_t` + `ent->v.modelindex/flags/weaponmodel`, `pr_strings`, `FL_MONSTER/FL_CLIENT`, `EDICT_NUM/NEXT_EDICT`, `SV_ModelHomeOwned`; client: `cl.viewent/mtime/num_entities/num_statics/model_precache`, `cl_entities`, `cl_static_entities`; host/system: `host_framecount`, `Sys_FloatTime/Sys_Printf/Sys_Error/Sys_FileRead`, `COM_OpenFile/CloseFile/LoadFileWith/FileRom`, `Con_Printf/Con_DPrintf`, `LittleLong/Float/Short`, `Q_strcasecmp`. Re-target to the new engine's file/model/entity APIs.

---

#### n64/port/modelviewer, spike, smoketest

##### n64/port/modelviewer/viewer.c (1546 lines; git: 8942704)

Verdict: CLEAN except **CONTAINS-QUAKE (format constants, 7 lines)**. Lines 33-39 (`ALIAS_VERSION/ONSEAM/SKIN_*/ALIAS_SINGLE/GROUP`, `MDL_DISK_SIZE`) and the daliasframe_t layout comment (497) mirror `modelgen.h`; viewer logic, STB2/AMB2 decode, T3D drawing, UI are ours (libdragon/tiny3d APIs). Evidence hits: LIBDRAGON/XASH n 11-49 (`#include` run) and XASH e 74-75 (`be32` shift expression) = false positives.

##### n64/port/modelviewer/hostfs_9p.c, hostfs_9p.h, sc64_link.c, sc64_link.h, sc64_9p_agent.py, sc64_session.py

CLEAN w.r.t. Quake/Xash/HLSDK (9P2000 client/server, SC64 USB). Provenance caveat: `hostfs_9p.c` differs from `~/src/ports/homeworld/n64/src/hostfs_9p.c` by 5 diff lines and `hostfs_9p.h` is byte-identical; `sc64_9p_agent.py` differs by 19 lines from `homeworld/n64/sc64_9p_agent.py`; `sc64_link.c` says "Simplified from Homeworld n64/src/sc64_link.c" (836 diff lines vs it); `sc64_session.py` is mostly rewritten (791 diff lines). Those Homeworld files came from commit a8f97a8 ("n64: add fleet viewer and asset pipeline"); this looks like same-owner sibling-project reuse [INFERENCE], but confirm before relicensing. Evidence hits (hostfs_9p.c: LIBDRAGON/XASH n 9-24, XASH n 31-55 vs `img_dds.h`, QUAKE n 38-53 vs scitech `mgraph.h`) = false positives (include lists, constant enum shapes).

##### n64/port/modelviewer/main.c, viewer.h, Makefile, README.md, gen_index.py, run_ares_shot.sh

CLEAN, ours.

##### n64/port/spike/fputest.c (58 lines; git: e599929)

**CONTAINS-QUAKE (10 lines)**: lines 9-18 `calcfov` is `CalcFov` from `first/source/screen.c:195-210` (same three statements/constants, bad-fov check removed); the file's comment says it replicates CalcFov's register pattern on purpose. Throwaway spike, do not import. Rest of file ours.

##### n64/port/spike/main.c, blit2d.c, t3dtex.c, Makefile, streamv2/main.c, streamv2/Makefile, streamv2/analysis/*.py

CLEAN. Own libdragon/tiny3d benchmarks and throwaway BSP30/analysis scripts (`bsp30.py` `pvs()` and `marked_nodes()` are independent set-based re-expressions of PVS decode / leaf marking; BSP30 lump reading is format). Evidence: streamv2/main.c XASH e 21 (`be32`) = false positive.

##### n64/port/spike/streamv2/fs/dome.sct, out/*.json

ASSET/DATA: cooked sector blob and run statistics from Valve HL demo maps; not code and not ours to relicense. Exclude.

##### n64/smoketest/main.c, Makefile

CLEAN (libdragon+tiny3d init).

---

#### tools/ (C)

##### tools/studio_compare/xash_ref_stream.c (504 lines; git: b6ea17c)

Verdict: **CONTAINS-XASH + CONTAINS-HLSDK**: 322 lines each. This is the explicit Xash3D reference harness; header (lines 1-7): "Formulas match xash3d public/xash3d_mathlib.c, public/matrixlib.c, ref/gl/gl_studio.c". Judgement: it is a second line-by-line-style translation (renamed `XR_*`), not an independent clean-room derivation; it shares provenance with `studio_geom.c`, so using it as an oracle for an MIT reimplementation only proves equivalence to the GPL/HLSDK algorithm.

| lines | function | verdict | counterpart | basis |
|---|---|---|---|---|
| 18-22 | XR_LE16 | CLEAN | - | ours |
| 24-36 | XR_AngleQuat | CONTAINS-XASH/HLSDK | `xash3d_mathlib.h:538-558`; `hlsdk/cl_dll/studio_util.cpp:139-153` | renamed copy (evidence HLSDK n 27-36, XASH e/n 32-38) |
| 38-88 | XR_QAlign, XR_QSlerp | CONTAINS-XASH/HLSDK | `xash3d_mathlib.c:283-370`; `studio_util.cpp:162-224` | renamed copy; comment 81 names Xash (evidence XASH n 60-76, HLSDK e 69-76) |
| 90-130 | XR_MatFromQ, XR_Concat, XR_XForm, XR_IRot | CONTAINS-XASH/HLSDK (matrix identities) | `matrixlib.c:28-90`; HLSDK `studio_util.cpp:100-125`; `utils/studiomdl/studiomdl.c:307-311`, `utils/common/mathlib.c:248-251` | same formulas, loop-compacted Concat |
| 132-137 | XR_Norm | generic | - | identity |
| 139-204 | XR_BoneChannel | CONTAINS-XASH/HLSDK | `xash3d_mathlib.c:434-511`; `StudioModelRenderer.cpp:178-330` | structural translation (evidence HLSDK n 160-172, XASH n 161-172) |
| 206-248 | XR_BoneAdj | CONTAINS-XASH/HLSDK | `gl_studio.c:658-704` | reduced translation |
| 250-266 | XR_Frame | CONTAINS-XASH/HLSDK | `gl_studio.c:577-589` | same branches and 1.001 clamp |
| 268-294 | XR_Chrome | CONTAINS-XASH/HLSDK | `gl_studio.c:1116-1155`; `mdlviewer/studio_render.cpp:441-` | structural translation |
| 296-319 | XR_Push, XR_Skin | CLEAN | - | ours |
| 321-371 | StudioXashRef_Emit (sequence select, calc rotations, motion-bone zero, parent concat) | CONTAINS-XASH/HLSDK | `gl_studio.c:720-753, 816-900` | structural |
| 374-503 | body-part select, vertex xform, tricmd walk | CLEAN (format-driven, same family as SG_EmitSubmodel) | cf. DrawPoints | independent expression |

Evidence: QUAKE n 192-194 (`r_sprite.c` lerp), n 279-286 (`mathlib.c` CrossProduct), QCTRANS e 134 = false positives (identities).

##### tools/studio_compare/soft_studio.c (335 lines; git: bcc05dc)

**CONTAINS-QUAKE (17 lines)**: 40-56 `angle_vectors` (comment 50 says "Quake AngleVectors"), same source as `studio_soft.c` 287-306 (`first/source/mathlib.c:295-312`). Comment 221 echoes Xash's pitch flip. Rest is a duplicate of the studio_soft.c logic, ours. Evidence: LIBDRAGON/QCTRANS/XASH n 23-37 (zero-init) = false positive; HLSDK/QUAKE n 48-60 = the real angle_vectors hit.

##### tools/studio_compare/{soft_fb.c, soft_fb.h, soft_studio.h, studio_compare.h, mdl_load_host.c, dump_bounds.c, dump_seqs.c, test_studio_anim.c, test_studio_bake_policy.c, test_studio_modelspace.c, test_studio_pixels.c, test_studio_stream.c, test_studio_trim.c, Makefile, README.md}

CLEAN (ours; test code and framebuffer/PPM helpers). `mdl_load_host.c` swap code is the same format code as mod_studio.c (independent loops). Build dependency: these link `source/studio_geom.c` and `xash_ref_stream.c` (contaminated), so the tests exercise the GPL/HLSDK-derived code. Evidence hits are false positives: `test_studio_anim.c`/`test_studio_pixels.c` XASH e (`#include <math.h>` run vs `gnm_shim.c`), `test_studio_bake_policy.c` QUAKE n 86-103 (`menu.c` case table), `test_studio_modelspace.c` XASH e 27-28 (`be32`), QCTRANS/QUAKE/XASH n 72-81 (`static const case_t cases[]` table shape).

##### tools/q64mapc/q64mapc.c (1416 lines; git: a19f97e, modified)

Verdict: **CONTAINS-QUAKE (115 lines)** + **QUAKE-INTERFACE** (includes `source/bspfile.h`, id Quake, for `dheader_t/lump_t/dface_t/texinfo_t/miptex_t/dmodel_t/dclipnode_t`, `LUMP_*`, `BSPVERSION`, `MAX_MAP_HULLS`).

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
| 1-170, 253-453 | LE helpers, crc32, buf_t, palette load, HL BSP30 texture lump -> Quake miptex conversion (nearest palette remap, fence key handling) | CLEAN (format conversion) | - | ours (miptex layout is the BSP texture format) |
| 172-178 | SURF_* flag defines ("matching model.h") | CONTAINS-QUAKE (values) | `first/source/model.h:79-85` SURF_* | same names/values |
| 180-251 | calc_face_extents | CONTAINS-QUAKE | `first/source/model.c:709-758` CalcSurfaceExtents; also `hlsdk/utils/qrad/lightmap.c` (Quake heritage) | structural translation: same min/max init constants (999999/-99999), edge-sign vertex fetch, s/t dot with texinfo vecs, floor/ceil of /16, texturemins/extents from bmins/bmaxs; reads from raw lumps (evidence QUAKE e 237-246, HLSDK e 237-245) |
| 453-1010 | main: lump reads, HL RGB->mono light conversion (max channel), cluster grid binning (768 u cells, 26-neighbour lists), string table, planes/nodes/leafs tables | CLEAN | no counterpart (own .q64 format/streaming design) | ours |
| 1011-1081 | faces cook | CONTAINS-QUAKE (partial): 1024-1053 flag selection (side->PLANEBACK, "sky" prefix -> DRAWSKY|DRAWTILED, '*' prefix -> DRAWTURB|DRAWTILED, TEX_SPECIAL->DRAWTILED), 1065-1069 turbulent override (texturemins -8192, extents 16384) | `first/source/model.c:766-828` Mod_LoadFaces | same decisions and constants, restructured (string checks moved to cook time; '!' prefix added for HL) |
| 1082-1416 | texinfo/marksurface/submodel/clipnode copy, entity/vis embed, GEOM_GLOBAL, cluster dir, file assembly, CRC | CLEAN | - | ours; the 1133-1177 entity chunk uses our hl_entcook.h (other slice) |

Contaminated lines: QUAKE 115 (172-251 = 80; 1024-1053 = 30; 1065-1069 = 5). Disposition of evidence: QUAKE n 519-535 vs `quake2/server/sv_game.c` = false positive (LUMP macro block).

##### tools/alias_bake/bake_alias.c (636 lines; git: be67cb5)

**CONTAINS-QUAKE (format definitions, 35 lines)**: 19-53 reproduce `modelgen.h` (ALIAS_VERSION, ALIAS_ONSEAM, ALIAS_SKIN_SINGLE/GROUP, IDPOLYHEADER, `stvert_t`, `dtriangle_t`, trimmed `mdl_t`; evidence: names/values equal `first/source/modelgen.h:45-132`). The comment says "Minimal IDPO constants (avoid full modelgen.h)". The bake itself (CI4 palette banding, ST tables, AMB2 writer; the on-seam half-width shift at 455-456 is the documented seam rule) is ours. Importable after re-declaring the format from spec.

##### tools/alias_bake/trim_alias_mdl.c (295 lines; git: d7a244a)

**CONTAINS-QUAKE (5 lines)**: 27-31 IDPO constants (as above). Rest ours (slim MDL rewriter). Makefile CLEAN.

##### tools/hl_headless.c (599 lines; untracked)

Verdict: **QUAKE-INTERFACE**. Test runner built on the Quake engine. Bindings by category: edict/QuakeC (`edict_t`, `EDICT_NUM`, `e->free`, `ent->v.{classname,targetname,model,origin,oldorigin,velocity,mins,maxs,absmin,absmax,angles,view_ofs,v_angle,flags,health,movetype,solid,nextthink,modelindex,ideal_yaw}`, `pr_strings`, `string_t`, `e->hl_caps/hl_lump`); server (`sv.*`, `svs.maxclients`, `SV_Move`, `SV_LinkEdict`, `SV_TestEntityPosition`, `SV_PointContents`, `SV_HullPointContents`, `trace_t`, `MOVE_NOMONSTERS`); model/world (`model_t`, `sv.worldmodel`, `Mod_PointInLeaf`, `Mod_LeafNum`, `hull_t`); client/refdef (`cl.viewangles`, `cls.signon`, `r_refdef.vieworg`); console/cmd/keys (`Con_Printf`, `Cbuf_AddText`, `Key_Event`, `Key_StringToKeynum`); host (`Host_Init`, `Host_Frame`, `quakeparms_t`, `COM_InitArgv`, `com_argc/argv`, `Hunk_LowMark/HighMark`, `host_parms`, `QUAKEGENERIC_MEMSIZE`). Contaminated lines 0: the census/probe/leafdump/postrace code is ours; main() 481-545 repeats the standard parms/COM_InitArgv/Host_Init skeleton of `first/source/sys_null.c:216-229` (boilerplate, ~25 lines; not counted). Not importable (depends on the old engine); a replacement runner is needed for the new engine.

##### tools/test_bsp_load.c (97 lines; git: 5cfba32)

QUAKE-INTERFACE: `Host_Init`, `quakeparms_t`, `COM_InitArgv`, `Mod_ForName`, `model_t` fields, `host_basepal`, `QG_*` stubs. Ours, boilerplate parms setup as above. Not importable.

##### tools/q64zone/q64zone.c (422 lines; git: 8c3bbf5)

CLEAN. Own cluster-to-zone merger (union-find, target zone count, binary .zones writer); depends on `n64/port/q64_map.h` struct layout (other slice). No Quake/Xash/HLSDK text. Makefile CLEAN.

##### tools/studio_bake/bake_studio.c (1247 lines; git: bcc05dc, modified), bake_meshopt.cpp (522 lines; git: d184bf0), Makefile, .gitignore

CLEAN. bake_studio.c: STB2 writer, CI4 band/palette build (same algorithm as bake_alias.c, ours), per-class simplify budgets, bind-pose compaction, meshlet layout (717-848), world-override table (comment 480 cites HLSDK CTripmineGrenade behaviour). The tricmd walk `emit_submodel_topo` (331-474) is format-driven and the same expression family as `SG_EmitSubmodel` (no token hit vs Xash/HLSDK). It links `source/studio_geom.c` and `studio_compare/mdl_load_host.c` (Makefile 53-54; `compact_mesh` uses `StudioGeom_SetupBones`), so it inherits studio_geom.c's contamination at link time only. bake_meshopt.cpp only calls the vendored MIT meshoptimizer API (evidence QUAKE n 90-99 vs `model.c`, LIBDRAGON n 92-100 = false positives: quantised key helper shape).

##### tools/w64c/hlecook.c (86 lines; untracked)

CLEAN. Thin driver over `hl_entcook.h`/`hl_nodecook.h` (other slices). `tools/w64c/hlecook` is an untracked x86-64 ELF build artifact, not source; `tools/w64c/Makefile` CLEAN.

---

#### tools/ (Python, shell, data)

##### tools/w64c/w64c.py (2015 lines; untracked)

Verdict: **CONTAINS-QUAKE (152 lines, incl. a QCTRANS/Quake data table), CONTAINS-HLSDK (98 lines)**. Remainder is ours (w64 v2 package cooker: meshlets 541-690, sectors 691-774, collision COLL partition/encode/grid 968-1306, SUBMODEL placements, CRC/IO).

| lines | block | verdict | counterpart | basis |
|---|---|---|---|---|
| 121-128 | WORLD_STYLES (lightstyles 0-11 and 63) | CONTAINS-QUAKE (data), QCTRANS | `quake/QW/progs/world.qc:300-345`, `qctrans/g_world.c:211-` (Quake set incl. style 63; HLSDK `dlls/world.cpp:559-600` has the same strings for 0-12 and no 63) | exact data copy of the stock tables; our g_world.c is the QuakeC translation, so the lineage is Quake-via-QCTRANS (and independently HLSDK for HL) |
| 344-373 | animation sequencing inside build_textures | CONTAINS-QUAKE | `first/source/model.c:355-497` Mod_LoadTextures (the `+0..9` / `+A..J` anim/alt passes, 2-tick frame cycle `total/min/max`, next-frame ring, alt chain) | self-described "verbatim semantics of Mod_LoadTextures" (comment 318, 344); structural translation to Python |
| 424-431, 838-850 | CalcSurfaceExtents (cook_face; LightFaces.get) | CONTAINS-QUAKE | `first/source/model.c:709-758` | same constants and steps as q64mapc (comment 424 "(model.c)") |
| 861-876 | LightFaces.sample (lightmap index `lofs + lt*smax + ls`, per-style base advance, `>> 8` after style scale) | CONTAINS-QUAKE | `first/source/r_light.c` RecursiveLightPoint tail (lines ~200-235) | same lightmap addressing and style accumulation, vectorised |
| 879-944 | _light_walk, light_points | CONTAINS-QUAKE | `first/source/r_light.c:141-259` RecursiveLightPoint + R_LightPoint (2048-unit downward trace, front/back distances, midpoint by distance ratio, near side first, node-face test with `ds/dt` vs texturemins/extents, far side last) | structural translation (numpy-vectorised, "same op order" per comment 880) |
| 1349-1446 | Materials: file parse, exchange sort, binary search, texture prefix strip | CONTAINS-HLSDK | `hlsdk/pm_shared/pm_shared.c:170-188` PM_SortTextures, `190-258` PM_InitTextureTypes, `260-290` PM_FindTextureType | faithful port incl. line-length cap 510, comment/space skipping, isalpha/toupper, name length cap CBTEXTURENAMEMAX-1, unterminated-last-name drop, bubble sort, strnicmp binary search |
| 1510-1539 (loop 1524-1534) | build_visr RLE walk | CONTAINS-QUAKE (minor, format) | `model.c` Mod_DecompressVis shape ("as Mod_DecompressVis reads it") | tiny RLE decode shape (zero-byte run count); counted in the 152 |
| 405-519 | cook_face vertex/light/UV attributes | follows `n64/port/r64_world.c` BuildSurfaceVerts (comment 28, 379) | - | verdict follows that file in another slice; lightmap sampling at 469-483 is stated replica |
| rest | | CLEAN | - | ours |

Contaminated lines: QUAKE 152 (344-373 = 30; 424-431 = 8; 838-850 = 13; 861-876 = 16; 879-944 = 66; 121-128 = 8; 1524-1534 = 11) ; QCTRANS 8 (121-128, same lines); HLSDK 98 (1349-1446).

##### tools/w64c/w64check.py (722 lines; untracked)

**CONTAINS-QUAKE (73 lines)**: 296-368 class `Tracer` (point contents walk + recursive segment trace) is a Python port of `first/source/world.c` SV_HullPointContents + SV_RecursiveHullCheck (world.c:487-505 and 581-711; same empty/solid/water flags, plane-type fast path, DIST_EPSILON near-side crosspoint, `frac` clamp, mid-point stepping, the `frac -= 0.1` solid-backup loop, allsolid early out). The file header says so ("a Quake SV_RecursiveHullCheck on the COLL encoding"). 109-119 `decompress` is the vis RLE decode (format-level, 11 lines, not counted). The remaining checks (COLL/grid/render proofs) are ours and use independent numpy BSP tree walks.

##### tools/w64c/w64seam.py (849 lines; untracked)

**CONTAINS-QUAKE (7 lines)**: 77-83 `spawn_rotation` builds the AngleVectors basis (forward/right/up sign-distributed form of `first/source/mathlib.c:295-312`). Remainder ours (Fibonacci ray directions, BSP interval traversal, seam proofs, budgets, CWG3 writer). `test_w64seam.py` CLEAN.

##### tools/hl_world/ (stitch_and_carve.py, stream_plan.py, trial_stitched_carve.py, zone_pack.py, README.md)

CLEAN. Own landmark placement (documented GoldSrc changelevel offset rule), cluster/act-anchor zone carving (union-find + AABB), stream plan/dry stub writers, LZ4 pack cooker. No BSP-compiler/vis/light algorithm ports (inventoried all `def`s).

##### tools/hl_world/examples/*.json, uplink.zones.md

DATA (generated): zone/stream reports from Valve HL maps (class names, bounds, counts). Not code; derived from Valve map data - leave out of the new repo.

##### tools/ remaining Python/shell/text (all CLEAN)

`analyze_hl_tex_atlases.py`, `bake_world_atlas.py`, `convert_hl_sounds.sh`, `extract_hl_sounds.sh`, `extract_pak_sounds.py`, `hl_campaign.py`, `hl_campaign_sizing.py`, `hl_classes.py`, `hl_gsfx.py`, `hl_image.py`, `hl_image_keep.txt`, `hl_sound_classes.py`, `hl_sound_classes.txt`, `hl_sound_refs.py`, `hl_sprite_cook.py`, `hl_stage_content.py`, `hlhud_bake/bake_hlhud.py`, `mdl_size_report.py`, `mk_white_room.py`, `n64_content_pipeline.py`, `paktool.py`, `prof_compare.py`, `profdb.py`, `progs_string_audit.py`, `progs_string_liveness.py`, `q64mapc/est_ent_pack.py`, `q64zone/bake_dry.py`, `rebuild_n64_paks.py`, `ui_bake/bake_ui.py`, `ui_bake/extract_conchars.py`, `wav64_rate_report.py`, `wav_probe.py`, `world_tex_bake/bake_world_tex.py`, `Makefile`, `README.md`, `.gitignore`, `q64mapc/Makefile`, `q64mapc/.gitignore`, `q64zone/Makefile`, `w64c/Makefile`, `alias_bake/Makefile`, `studio_bake/Makefile`, `studio_bake/.gitignore`, `studio_compare/Makefile`, `studio_compare/README.md`.

Notes: they read/write file formats (PACK, WAD2, WAV/RIFF cue chunk, BSP29/30 lumps incl. vis RLE, SPR v2, progs.dat, wav64) and drive other tools. Format parsing and RLE decoders (`analyze_hl_tex_atlases.py:140-155`, `w64check.py:109-119`) are format code, not ported engine algorithms. `hl_classes.py` / `hl_gsfx.py` / `hl_stage_content.py` hold tables of asset names/classnames and short descriptions citing HLSDK file names (our prose; asset names are facts) - HLSDK-informed but not copied text. `hl_sprite_cook.py` and `bake_*` CI4 palette banding mirror `r64_tex.c` PickPal16 (verdict follows that file). `mk_white_room.py` writes a hand-built BSP29 box (no BSP compiler logic). `paktool.py` is the PACK container (id's format, own code). `hl_image_keep.txt` comments cite our own sources.

##### tools/deps/meshoptimizer/**

Vendored third-party, MIT (c) 2016-2024 Arseny Kapoulkine (LICENSE.md present). Not audited; keep its licence file if kept.

