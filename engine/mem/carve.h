/* SPDX-License-Identifier: MIT */
/*
 * carve.h -- the boot carve (docs/architecture.md 3.1 step 2, 4.1, 4.3):
 * every fixed region, in this fixed order, from one block taken at boot.
 * Offsets are relative to the block, so the cart and the host print the
 * same table. The heap left afterwards is measured and must be at least
 * HL64_HEAP_MIN_KB.
 */
#ifndef HL64_CARVE_H
#define HL64_CARVE_H

#include <stdint.h>

/* X(id, name, kb, est): est = an [EST] row of 4.1 */
#define MEM_REGIONS(X) \
	X(SCRATCH, "scratch", HL64_SCRATCH_KB, 0) \
	X(RESTAB, "restab", HL64_RESTAB_KB, 0) \
	X(SENTENCES, "sentences", HL64_SENTENCES_KB, 1) \
	X(LIGHTSTYLE, "lightstyle", HL64_LIGHTSTYLE_KB, 0) \
	X(LAYER_DIRS, "layerdirs", HL64_LAYER_DIRS_KB, 0) \
	X(POOL_RENDER, "render", HL64_POOL_RENDER_KB, 0) \
	X(POOL_TEXTURE, "texture", HL64_POOL_TEXTURE_KB, 0) \
	X(POOL_COLLISION, "collision", HL64_POOL_COLLISION_KB, 0) \
	X(POOL_STUDIO, "studio", HL64_POOL_STUDIO_KB, 1) \
	X(STUDIO_SCRATCH, "studioscratch", HL64_STUDIO_SCRATCH_KB, 1) \
	X(COMPONENTS, "components", HL64_COMPONENTS_KB, 0) \
	X(GAME_PRIVATE, "gameprivate", HL64_GAME_PRIVATE_KB, 1) \
	X(THINK_GRID, "thinkgrid", HL64_THINK_GRID_KB, 0) \
	X(HLE_BLOBS, "hle", HL64_HLE_BLOBS_KB, 1) \
	X(DEHYDRATE, "dehydrate", HL64_DEHYDRATE_KB, 0) \
	X(SOUND, "sound", HL64_SOUND_KB, 1) \
	X(SPRITE_CACHE, "sprites", HL64_SPRITE_CACHE_KB, 0) \
	X(DECAL_TEXELS, "decals", HL64_DECAL_TEXELS_KB, 0) \
	X(PARTICLES, "particles", HL64_PARTICLES_KB, 0) \
	X(DLIGHT, "dlight", HL64_DLIGHT_KB, 0) \
	X(HUD, "hud", HL64_HUD_KB, 1)

enum {
#define MEM_REGION_ENUM(id, name, kb, est) MEM_##id,
	MEM_REGIONS(MEM_REGION_ENUM)
#undef MEM_REGION_ENUM
	MEM_REGION_COUNT
};

/* Carve every region, log the table and the heap left; sys_error when the
 * block does not fit or the heap left is under HL64_HEAP_MIN_KB. */
void		mem_boot_carve(void);
/* Region base (16-byte aligned) and size; valid after mem_boot_carve. */
void		*mem_region(int id);
uint32_t	mem_region_bytes(int id);
/* Print the table again (DBG_MEM). */
void		mem_carve_log(void);

#endif
