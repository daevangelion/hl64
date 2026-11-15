/* SPDX-License-Identifier: MIT */
/*
 * limits.h -- every cap and fixed region size of hl64, shared by the cart
 * and the host build so that anything that fails on one fails on the
 * other (docs/architecture.md 3.4, 4.1, 4.3).
 *
 * Region sizes follow the 4.1 table. Rows marked [EST] there print a
 * "carve:" line with "est" at boot; M3/M4/M6 replace them with the
 * measured peak x 1.25. Pool sizes marked "cook-proved" are the smallest
 * 16 KB multiples >= 1.05 x the worst cooked open set of the Uplink maps
 * (stream-arch-v2 8; the world cook fails when an open set exceeds them).
 */
#ifndef HL64_LIMITS_H
#define HL64_LIMITS_H

/* Console RDRAM: the Expansion Pak is required (D7). */
#define HL64_RDRAM_BYTES		(8 * 1024 * 1024)

/* Heap that must remain after the boot carve for libdragon's own
 * allocations (rspq buffers, audio, surfaces of the debug paths) and
 * slack; the 4.1 "stack + libdragon slack" row. Boot fails below it. */
#define HL64_HEAP_MIN_KB		256

/*
 * The cart's heap after the boot carve, which the host heap arena
 * (plat/host/heap_host.c) mirrors so post-boot allocations fit or fail as
 * on the cart. libdragon's heap runs from the image end (__bss_end of the
 * linked ELF) to RDRAM top less its stack; boot then takes:
 *   - HL64_CART_BOOT_BYTES: libdragon's init (display, rdpq, audio, t3d,
 *     rba banks) and the engine before the carve, image-independent;
 *   - the carve block (sys_carve_alloc: carve total + 15 B, one dlmalloc
 *     chunk);
 *   - with content, the DFS mount: 16 N + 40 B for N files (the lookup
 *     table 8 + 16 N B plus the "rom:/" prefix's 24 B chunk).
 * Measured (ares, HL64_TEST ROMs of 403070a, "mem: heap free ... free N
 * B"): no content 2866008 B at __bss_end 0x801709f8; N = 10 / 501 / 1230:
 * 200 / 8056 / 19720 B less; PROFILE + 1230 files, __bss_end 0x80170da8:
 * 2845344 B -- every value exact. The libdragon preview SDK (fork ef44355,
 * m6-sdk) boots with 3448 B more (m3-a PROFILE ROM: cart 1886776 B after
 * the carve, host 1890224 B at the old value). The host build reads
 * __bss_end from its HL64_TEST cart ELF (Makefile, cart_image.h) and
 * plat/host/io_host.c mounts the same table for its content root.
 * test/run/m3.sh --ares asserts host == cart to the byte.
 */
#define HL64_CART_STACK_BYTES		0x10000		/* libdragon STACK_SIZE */
#define HL64_CART_BOOT_BYTES		757776

/* Image (code + data + bss) budget, 4.1 first row; boot logs the size. */
#define HL64_IMAGE_BUDGET_KB		900

/* ---- frame loop (3.2, 3.3) ------------------------------------------- */

/* A frame is shown no earlier than this many vblanks after the last. */
#define HL64_PRESENT_VBL		2
/* dt clamp (D6 i): [1/60 s, 0.1 s]. */
#define HL64_DT_MIN_US			16667u
#define HL64_DT_MAX_US			100000u
/* HL64_TEST builds without a route quit 0 after this many frames. */
#define HL64_TEST_IDLE_FRAMES		60

/* ---- debug channel (12) ---------------------------------------------- */

#define HL64_LOG_RING_BYTES		8192	/* power of two */
#define HL64_LOG_LINE_MAX		256
#define HL64_DBG_QUEUE			32

/* ---- world / entities ------------------------------------------------ */

#define HL64_WORLD_LAYERS		2
/* Entity slots: ceil(1.25 x 396), the old host route's peak at a portal
 * with both maps' sets live (hldemo3 arrival). */
#define HL64_MAX_ENTITIES		495

/* Campaign maps (CWG3 map table) and their names. */
#define HL64_MAX_MAPS			128
#define HL64_MAP_NAME			32
/* World chunk reads in flight or queued at once (engine/world). */
#define HL64_WORLD_REQS			16

/* Player move (specs/pmove-host.md 2, D2): physents, ladders, touch list,
 * the player's physics info string (HLSDK MAX_PHYSINFO_STRING is 256; the
 * HL game sets "\slj\N\hl\1"). */
#define HL64_PM_PHYSENTS		64
#define HL64_PM_MOVEENTS		8
#define HL64_PM_TOUCH			32
#define HL64_PM_PHYSINFO		32

/* ---- renderer (9) ------------------------------------------------------ */

/* Meshlets listed per frame (per-frame draw list in the scratch region;
 * the cooked maps have 367-1728 meshlets in total). */
#define HL64_REN_VIS_MAX		1536
/* Movers drawn under their own matrix per layer per frame (overflow is
 * counted as dropped). */
#define HL64_REN_MOVERS_MAX		48
/* Texture pages and textures per layer (cooked maps: <= 134 / 142). */
#define HL64_REN_PAGES_MAX		512
#define HL64_REN_TEX_MAX		512
/* Studio models (engine/render/studio.c): cooked MODEL ids the residency
 * table covers, resident animation chunks (D3 b), pool block descriptors,
 * studio entities drawn per frame (nearest kept, overflow counted). */
#define HL64_STUDIO_MODELS		256
#define HL64_STUDIO_ANIMS		512
#define HL64_STUDIO_BLOCKS		1024
#define HL64_STUDIO_DRAWN		64
/* Dynamic lights (docs/specs/dlights.md 1). */
#define HL64_DLIGHTS			32
/* Effects (docs/specs/render-fx.md 2-5): temp entities and the studio
 * temps drawn per frame, particles (the particles region holds them),
 * beams and their segments, decals, the decal placement queue, the
 * translucent list per frame, the sprite models the sprite cache tracks.
 * (Additive items have no blend cap: render-fx.md 2a.) */
#define HL64_FX_TEMPS			48
#define HL64_FX_TEMPS_STUDIO		6
#define HL64_FX_PARTICLES		512
#define HL64_FX_BEAMS			16
#define HL64_FX_BEAM_SEGS		16
#define HL64_FX_DECALS			96
#define HL64_FX_DECAL_VERTS		24
#define HL64_FX_DECAL_QUEUE		16
#define HL64_FX_TRANS_MAX		64
#define HL64_FX_SPRITE_MODELS		16

/* ---- boot carve regions, KB (4.1) ------------------------------------ */

#define HL64_SCRATCH_KB			96	/* per-frame scratch arena; boot: the campaign.rtb cart part (67632 B measured, Uplink + Hazard with the full game res.def); cook-proved: restab fails when the cart part exceeds it */
#define HL64_RESTAB_KB			56	/* resource table: 44528 B measured (full res.def, Uplink + Hazard cook of main 1a9e4e8: 1192 files, 1015 sounds) x 1.25; cook-proved: restab computes engine/res's table bytes and fails above it */
/* Saved games (engine/save, architecture.md 14): one slot's compressed
 * image, kept while the flash writer programs it over the following
 * frames (include/hl64/save.h SAVE_SLOT_BYTES); the coder's work memory
 * is borrowed from the scratch arena during the snapshot / restore. */
#define HL64_SAVE_KB			112
#define HL64_LIGHTSTYLE_KB		4	/* patterns + state */
/* The whole CWG3 campaign graph (engine/world/seam.c; docs/formats/
 * cwg-format.md): Uplink + Hazard c6 cook 4730 B (10 maps, 20 seams, 72
 * twins, sector pool) x 1.25 = 5913 B; cook-proved: tools/cook/stage.py
 * fails when maps/campaign.cwg exceeds it. */
#define HL64_CAMPAIGN_KB		6
#define HL64_LAYER_DIRS_KB		267	/* cook-proved (tools/cook/layerdirs.py, every CWG link pair's two layers: directory + tables, 16-aligned, as engine/world/world.c allocates): worst t0a0b + t0a0a 218624 B (Uplink + Hazard c6 cook) x 1.25 */
/* Render / texture pools: cook-proved crossing window (tools/w64c/w64seam.py,
 * docs/formats/cwg-format.md Proof and residency policy): what engine/world/
 * stream.c names at open + pin while a seam is approached (A's open set at
 * every approach eye: eye leaf + drawn-eye leaves + movers on their travel
 * + teleport pins, plus B's seam pin: sector-pool slice + arrival leaves)
 * and after it switches (B's open set at the arrival eyes + the reverse
 * seam's pin), 16-aligned chunks, worst over every CWG seam and every
 * map's open set with no seam (worst hldemo2 361952 / t0a0d 130320 B)
 * x 1.25; the cook fails above the carve. Route m4 / hazard want peaks
 * (host, c8 cook): render 243104 / 254464 B, texture 173376 / 121168 B. */
#define HL64_POOL_RENDER_KB		458	/* cook-proved: seam hldemo2->hldemo3 (hldemo2_3b, approach 512) 375072 B (Uplink + Hazard c8 cook) x 1.25 */
#define HL64_POOL_TEXTURE_KB		214	/* cook-proved: seam hldemo2->hldemo1 (hldemo1to2, approach 512) 174496 B (Uplink + Hazard c8 cook) x 1.25 */
#define HL64_POOL_COLLISION_KB		208	/* cook-proved: seam hldemo3->hldemo2, 206592 B */
#define HL64_POOL_STUDIO_KB		1024	/* [EST] D3(b) per-sequence chunks */
#define HL64_STUDIO_SCRATCH_KB		192	/* [EST] skinned-mesh tables, view model */
/* Measured layout (engine/ent/ent.c ent_init, -m32 = cart): components
 * 228 B (cv 92, mot 36, phys 24, tm 16, rnd 44, id 16) + grid node 16,
 * membership pool 3 x 8, touch rank 4, free time 4, contents flag 1 per
 * slot, each array 16-aligned: 137168 B for HL64_MAX_ENTITIES 495. */
#define HL64_COMPONENTS_KB		134
#define HL64_GAME_PRIVATE_KB		160	/* [EST] game private slabs */
#define HL64_THINK_GRID_KB		12	/* think heap, active bitsets, grids */
/* HLE region (architecture 4.1): tables + the opening map at open_chunk's
 * open-time need + the open map's block + 2 frozen maps' runtime, each
 * 16-aligned. With infodecal / env_glow records kept: Uplink + Hazard need
 * 187232 B (hldemo1), retail estimate 194768 B (c2a5f); carve = retail x 1.25
 * (Main). Cook-proved: hleids --check fails when the need exceeds it. */
#define HL64_HLE_BLOBS_KB		240
#define HL64_DEHYDRATE_KB		64	/* dehydrate store */
/* Measured (ares, test/m3/snd_ares.sh): 78144 B peak = tables 3424 + AUDIO
 * bounce 2048 + 16 voice buffers (4096 + VADPCM state 48 + arena header)
 * + 10 open wav64 (state, codebook). M5 hazard route on ares: 82832 B peak
 * x 1.25 = 103540 B (test/run/m5.sh region table, m5-int2 eb47299; uplink
 * 82080 B). */
#define HL64_SOUND_KB			104
#define HL64_SPRITE_CACHE_KB		64
/* Decal region: HLSDK sheet (29856 B) + 96 x 24 piece vertices (23040 B)
 * + 2 per-map placed-decal (infodecal) sheet slots of the largest map's
 * size; cook-proved (hleids --check). Retail need 86528 B (largest map
 * c2a1 16816 B) x 1.25 = 108160 B; Uplink need 76672 B. */
#define HL64_DECAL_TEXELS_KB		106
#define HL64_PARTICLES_KB		12
#define HL64_DLIGHT_KB			32	/* dlight scratch: 2 x 512 vertex pairs x 32 B */
#define HL64_HUD_KB			128	/* [EST] HUD sheets + font */

/* Spread monster thinks (game/hl/aithink.c, engine/core/co.c): a think
 * that runs out of the frame's AI work budget pauses on its own context
 * and continues next frame. Each context: a stack + the cover search's
 * scratch (FindCover's two path-length tables, 2 x HLN_MAX_NODES 1024
 * x f32). More paused thinks than contexts defer a new think a frame.
 * Stack: peak measured 4552 B (cart, uplink + m4 routes) / 4532 B (host
 * x86-64); 8 KB keeps 1.8x; co.c logs a context past 3/4 and fails on
 * overflow into its guard words. */
#define HL64_AI_CONTEXTS		6
#define HL64_AI_STACK_BYTES		8192
#define HL64_AI_SCRATCH_BYTES		8192
#define HL64_AI_THINK_KB		((HL64_AI_CONTEXTS * (HL64_AI_STACK_BYTES + HL64_AI_SCRATCH_BYTES) + 1023) / 1024)
/* The frame's monster-think work budget (settings ai_budget default), in
 * work units (game/hl/monsters.c aiw_units, 1 unit ~ 1 us of cart CPU):
 * 18 ms of AI a frame, so that with the units' fit error (cart us / units
 * <= 1.15 in 99 % of frames) and one pause point's item past the budget
 * a frame's monster thinks stay under ~24 ms. */
#define HL64_AI_BUDGET			18000
/* A held (queued) monster think's lateness (settings ai_hold default,
 * seconds of game time): queued ai_hold, it may start over the frame's
 * budget, at most HL64_AI_AGED_PER_FRAME such starts a frame, oldest
 * first. */
#define HL64_AI_HOLD			0.2f
#define HL64_AI_AGED_PER_FRAME		2

#endif
