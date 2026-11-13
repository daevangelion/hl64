/* SPDX-License-Identifier: MIT */
/*
 * wld.h -- world: campaign map table, open layers, chunk residency, point
 * -> leaf, lightstyle values (docs/architecture.md 5.3-5.5,
 * docs/specs/lightstyles.md 2).
 *
 * M1 scope: maps open synchronously (header + directory in one read, the
 * pinned hull-0 COLL chunks after it); REND / MOVR / TEXP chunks are
 * streamed on demand at OPEN priority through engine/io and evicted LRU
 * behind a two-frame GPU fence. Prefetch / pin classes, seams and the
 * change-driven target set arrive with M4.
 */
#ifndef HL64_WLD_H
#define HL64_WLD_H

#include <stdint.h>

#include "engine/world/w64.h"

#define WLD_NO_MAP	0xFFFF

/* ---- campaign map table (CWG3 maps; docs/formats/cwg-format.md) -------- */

/* Read maps/campaign.cwg and set up the residency pools; 0 when the cart
 * has no player content (boot continues without a world), sys_error when
 * it is malformed. */
int		wld_init(void);
int		wld_map_count(void);
const char	*wld_map_name(int map);
/* Global placement g_M of a map. */
const float	*wld_map_origin(int map);
int		wld_map_find(const char *name);	/* -1 none */
int		wld_start_map(void);

/* ---- layers -------------------------------------------------------------- */

/* A placed brush entity drawn from its MOVR chunk (M1: spawn placement read
 * from the ENTS chunk; the entity components replace this in M3). */
typedef struct {
	float		origin[3];	/* map-local */
	uint16_t	sub;		/* BSP model index (w64_sub_t, MOVR) */
	uint16_t	pad;
} wld_mover_t;

typedef struct {
	void		*data;		/* resident bytes (pool block), NULL */
	struct mem_blk_s *blk;
	uint32_t	used;		/* frame of the last use */
	uint8_t		state;		/* WLD_CH_* */
	uint8_t		pad;
	uint16_t	want;		/* layer want generation that last named it */
	uint16_t	style_base;	/* first styled-meshlet slot, W64_NONE unassigned */
} wld_chunk_t;

enum { WLD_CH_ABSENT, WLD_CH_LOADING, WLD_CH_RESIDENT, WLD_CH_PINNED };

#define WLD_PATTERN_MAX	64

typedef struct {
	uint8_t			open;
	uint8_t			slot;		/* 0 .. HL64_WORLD_LAYERS - 1 */
	uint16_t		map;		/* campaign map index */
	float			gofs[3];	/* g_M */
	uint32_t		rom, size;	/* package on the cart */
	w64_header_t		hdr;
	const w64_dir_t		*dir;
	const w64_chunk_t	*chunks;
	const w64_sector_t	*sects;
	const uint16_t		*pagepool;
	const w64_tex_t		*texs;
	const w64_sub_t		*subs;
	const uint32_t		*leafvis;
	const uint16_t		*vispool;
	const w64_model_t	*models;
	const w64_grid_t	*grid;
	const w64_gcell_t	*cells;
	const int32_t		*starts;
	/* collision (engine/coll) */
	const int8_t		*leafc;		/* CONTENTS_* per BSP leaf [nleafs_all] */
	const uint16_t		*glists;	/* grid list pool: count, coll index... */
	const w64_mat_t		*mat;		/* surface materials, NULL none */
	const uint16_t		*mat_start;	/* [ncoll + 1] */
	const uint16_t		*mat_ent;
	const w64_matmix_t	*mat_mix;
	const w64_matface_t	*mat_face;
	uint16_t		coll_gen;	/* COLL want generation (wld_want_coll) */
	uint16_t		pad2;
	wld_chunk_t		*ch;		/* per chunk */
	uint32_t		*style_epoch;	/* per styled-meshlet slot: last colouring */
	uint16_t		nstyle_slots, style_used;
	uint8_t			*page_state;	/* per TEXP page: WLD_PAGE_STATE bytes (gpu) */
	wld_mover_t		*movers;
	uint16_t		nmovers;
	uint8_t			has_start;
	uint8_t			reencode;	/* running tiny3d encodes strips differently */
	uint8_t			want_pending;	/* a want could not be queued: retry */
	uint16_t		want_gen;	/* OPEN want generation */
	int32_t			want_leaf;	/* leaf the wants were built for */
	float			start_org[3], start_ang[3];	/* info_player_start, map-local */
	struct mem_blk_s	*mem;		/* layer region: directory block */
	struct mem_blk_s	*tables;	/* layer region: tables block */
	/* M1: map-start patterns of styles 32-63 from the map's `pattern`
	 * keys (the game's light entities own them from M3 on) */
	uint8_t			patterns[32][WLD_PATTERN_MAX];
} wld_layer_t;

/* GPU per-page state (prebuilt RDP bind list on the cart; the host keeps
 * the same bytes so the layer region use is identical). */
#define WLD_PAGE_STATE		96
/* Styled-meshlet slots per layer (the cooked maps have 0-482). */
#define WLD_STYLE_SLOTS		1024

/* Open map (synchronous; returns the open layer if it is already open);
 * sys_error on a bad package. */
wld_layer_t	*wld_open(int map);
void		wld_close(wld_layer_t *L);
wld_layer_t	*wld_layer(int slot);		/* NULL when closed */

/* BSP leaf of a map-local point (hull 0, grid start node). */
int		wld_point_leaf(const wld_layer_t *L, const float p[3]);
/* Sectors visible from a leaf (vis list: count, then sector indices). */
const uint16_t	*wld_leaf_sectors(const wld_layer_t *L, int leaf, int *count);

/* ---- residency ----------------------------------------------------------- */

/* Resident bytes of chunk c (marks it used this frame), or NULL after
 * queuing its read at OPEN priority. */
uint8_t		*wld_chunk_use(wld_layer_t *L, int c);
/* OPEN class wants (architecture.md 5.3): REND and every page of each
 * sector the leaf sees. Rebuilt only when the leaf changes (or while a
 * want could not be queued); otherwise the previous wants stand by
 * generation, and the LRU never evicts a chunk of the current set. */
void		wld_want_open(wld_layer_t *L, int leaf);
/* COLL wants (engine/coll, the collision window): the caller bumps
 * L->coll_gen (never 0) when its window changes and names each chunk;
 * an absent chunk is queued at OPEN priority, and no chunk named by the
 * current generation is evicted. */
void		wld_want_coll(wld_layer_t *L, int c);
/* Resident bytes of COLL chunk c, read synchronously when absent (a
 * chunk missing mid-trace: blocked I/O, architecture.md 5.2); a queued
 * read of it is finished first. */
uint8_t		*wld_chunk_sync(wld_layer_t *L, int c);
/* Frame boundary: retire the GPU fences two frames old. */
void		wld_frame(uint32_t frame);

typedef struct {
	uint32_t	loads, evictions, nofit, queued;
	uint32_t	pool_used[3], pool_peak[3];	/* render, texture, collision */
} wld_stats_t;
void		wld_stats(wld_stats_t *s);

/* Called by the residency when a REND / MOVR chunk lands (its vertex
 * colours are the cooked ones again). */
void		wld_style_chunk_landed(wld_layer_t *L, int c);

/* ---- lightstyles (lightstyles.md 2) ------------------------------------- */

#define WLD_STYLE_SLOTS_ALL	(32 + 32 * 2)	/* 0-31 common, 32 per open layer */

/* Every slot unset (value 256), epoch 1; wld_init calls it. */
void		wld_style_reset(void);
/* Set a global slot's pattern (letters 0-25, len 1-255); len 0 unsets it
 * (value 256). The letters must stay valid (cooked pattern table). */
void		wld_style_set(int slot, const uint8_t *letters, int len);
/* Global slot of style s (0-63) of layer L; -1 for an invalid style. */
int		wld_style_slot(const wld_layer_t *L, int s);
/* 100 ms tick on the game clock: recompute values, stamp changes. */
void		wld_style_tick(uint64_t now_us);
uint16_t	wld_style_value(int slot);
/* Epoch of the slot's last value change; the current epoch. */
uint32_t	wld_style_changed(int slot);
uint32_t	wld_style_epoch(void);

#endif
