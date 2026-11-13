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
 * Host heap arena (plat/host/heap_host.c): the cart's free heap after the
 * carve, measured from the HL64_TEST ROM's "mem: heap free" boot line in
 * ares (M0: 4128 KB, image 293 KB), so post-boot allocations fit or fail
 * as on the cart. test/run/m0.sh reports drift between the two lines.
 */
#define HL64_HOST_HEAP_KB		4128

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

/* Player move (specs/pmove-host.md 2, D2): physents, ladders, touch list. */
#define HL64_PM_PHYSENTS		64
#define HL64_PM_MOVEENTS		8
#define HL64_PM_TOUCH			32

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

/* ---- boot carve regions, KB (4.1) ------------------------------------ */

#define HL64_SCRATCH_KB			64	/* per-frame scratch arena */
#define HL64_RESTAB_KB			28	/* resource table: 9.5 KB measured (Uplink + Hazard, no res.def) + the game's res.def (~500 ids x 20 B) x 1.25 */
#define HL64_SENTENCES_KB		24	/* [EST] word ids, no strings */
#define HL64_LIGHTSTYLE_KB		4	/* patterns + state */
#define HL64_LAYER_DIRS_KB		168	/* 2 open .w64 directories, measured */
#define HL64_POOL_RENDER_KB		240	/* cook-proved: t0a0b leaf 726, 223904 B */
#define HL64_POOL_TEXTURE_KB		128	/* cook-proved: t0a0d leaf 127, 112864 B */
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
#define HL64_HLE_BLOBS_KB		160	/* [EST] 4 slots x 40 KB */
#define HL64_DEHYDRATE_KB		64	/* dehydrate store */
#define HL64_SOUND_KB			96	/* [EST] 16 voices, 10 wav64, loops */
#define HL64_SPRITE_CACHE_KB		64
#define HL64_DECAL_TEXELS_KB		48
#define HL64_PARTICLES_KB		12
#define HL64_DLIGHT_KB			48	/* dlight scratch + flashlight pool */
#define HL64_HUD_KB			128	/* [EST] HUD sheets + font */

#endif
