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
 * sys_error on a bad package. Boot / New Game / setpos only (load I/O). */
wld_layer_t	*wld_open(int map);
/* Seam open (M4, architecture.md 5.4 req. 1): never blocks. Takes a free
 * layer slot and queues the header, directory, tables, pinned hull-0 COLL
 * and ENTS reads at OPEN priority (stepped by wld_stream_frame); returns
 * the open layer if the map is already open (ready or not), NULL when no
 * slot is free (the caller counts open_fail). */
wld_layer_t	*wld_open_async(int map);
/* 1 once an async-opened layer has its directory, tables and pinned hull 0
 * resident (point->leaf, traces and HLE open are then valid; wld_open
 * layers are ready at return); 0 while in flight; -1 when the open failed
 * (no layer-region room after the fenced frees retired): count open_fail
 * and wld_close it. */
int		wld_layer_ready(const wld_layer_t *L);
void		wld_close(wld_layer_t *L);
/* Ready layer of a slot: NULL when closed or while its async open is in
 * flight (every layer loop sees whole layers only). */
wld_layer_t	*wld_layer(int slot);
/* Layer holding map, ready or still opening; NULL when not open. */
wld_layer_t	*wld_layer_of_map(int map);

/* BSP leaf of a map-local point (hull 0, grid start node). */
int		wld_point_leaf(const wld_layer_t *L, const float p[3]);
/* Sectors visible from a leaf (vis list: count, then sector indices). */
const uint16_t	*wld_leaf_sectors(const wld_layer_t *L, int leaf, int *count);
/* The decoded PVS row of BSP leaf `leaf` (bit i = visleaf i + 1) from the
 * layer's VISR chunk, cached one row per layer (architecture.md 5.5): the
 * offset pair and the row's compressed bytes are read synchronously on a
 * cache miss. *visleafs = the map's visleaf count (0: no vis). NULL when
 * everything is visible (no vis, leaf 0 or past visleafs, empty row). The
 * row stays valid until the next call for this layer. */
const uint8_t	*wld_pvs_row(wld_layer_t *L, int leaf, int *visleafs);

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

/* ---- M4 residency classes (architecture.md 5.3) -------------------------- */

/* Pin class: a seam's B set (CWG sector-pool entries: sector < 0x8000,
 * 0x8000 | n = BSP model n's MOVR, each with its TEXP pages) at SEAM
 * priority, plus the B hull-0 window; one seam pin per layer (a new call
 * replaces it). io_set_seam(1) while any seam pin is not resident. */
/* pool: native byte order, copied (at most WLD_PIN_POOL entries). */
#define WLD_PIN_POOL	128
void		wld_pin_seam(wld_layer_t *B, const uint16_t *pool, int n);
void		wld_unpin_seam(wld_layer_t *B);
/* The seam pin's B window (with wld_pin_seam; wld_unpin_seam clears it):
 * a B-local player box (switch eye box + delta - view offset, hull
 * extents); the COLL grid lists of that box +- the collision window are
 * pinned at SEAM priority so B's first traces never read synchronously. */
void		wld_pin_seam_window(wld_layer_t *B, const float mins[3], const float maxs[3]);
/* 1 when nothing of B's seam pin (set and window) is missing. */
int		wld_pin_resident(const wld_layer_t *B);
/* Point pin (teleport destinations, route look-ahead SETPOS): the eye-leaf
 * set and the collision window around map-local org of an open map, PIN
 * class; lapses after 3 s or when the player arrives within 64 u. A call
 * for the same map and point re-arms it; at most 4 live (the oldest goes). */
void		wld_pin_point(int map, const float org[3]);
/* Step 9: the target-set inputs of the home layer (eye map-local, velocity);
 * the set (open, pin lapse, prefetch at eye + v * {0.25, 0.5, 1} s,
 * collision window + v * 0.5 s) is rebuilt only when an input changed
 * (eye leaf, player cell, velocity bucket, pins, layer set), otherwise
 * re-asserted by generation. Also detects teleports (a player jump that is
 * not a seam re-base) and opens the teleport window for pop-in accounting. */
void		wld_stream_frame(int home_map, const float eye[3], const float vel[3]);
/* A seam crossing moved the player's global position by T (not a teleport). */
void		wld_stream_rebase(const float T[3]);

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
/* 100 ms tick on the game clock (f64 s, think-and-time.md 1): recompute
 * values at index floor(time x 10), stamp changes. */
void		wld_style_tick(double time);
uint16_t	wld_style_value(int slot);
/* Epoch of the slot's last value change; the current epoch. */
uint32_t	wld_style_changed(int slot);
uint32_t	wld_style_epoch(void);

#endif
