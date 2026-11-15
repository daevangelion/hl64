/* SPDX-License-Identifier: MIT */
/*
 * wld.h -- world: campaign map table, open layers, chunk residency, point
 * -> leaf, lightstyle values (docs/architecture.md 5.3-5.5,
 * docs/specs/lightstyles.md 2).
 *
 * Maps open synchronously at boot / New Game / setpos (wld_open, load
 * I/O) or asynchronously for seams (wld_open_async: header, directory,
 * tables, pinned hull 0 and the ENTS scan through engine/io, never
 * blocking). Residency (engine/world/stream.c): one target set of chunks
 * named per class (open > pin > prefetch), rebuilt only when an input
 * changes, evicted LRU behind a two-frame GPU fence by stronger classes
 * only.
 */
#ifndef HL64_WLD_H
#define HL64_WLD_H

#include <stdint.h>

#include "engine/world/w64.h"
#include "hl64/limits.h"
#include <stddef.h>

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
	uint8_t		cls;		/* WLD_CLS_* of the target generation that named it */
	uint16_t	want;		/* target generation that last named it */
	uint16_t	style_base;	/* first styled-meshlet slot, W64_NONE unassigned */
	uint8_t		retry;		/* engine/world/stream.c: on its retry list (RETRY_*) */
} wld_chunk_t;

enum { WLD_CH_ABSENT, WLD_CH_LOADING, WLD_CH_RESIDENT, WLD_CH_PINNED };
/* Residency classes, weakest first (architecture.md 5.3): a request
 * evicts only chunks of weaker classes (unwanted = NONE). LOAD: a layer
 * open's own reads (pinned hull 0, the ENTS scan), which may evict open
 * chunks not drawn this frame or the last: a fragmented pool must not
 * stall an open whose sum fits. */
enum { WLD_CLS_NONE, WLD_CLS_PREFETCH, WLD_CLS_PIN, WLD_CLS_OPEN, WLD_CLS_LOAD };

#define WLD_PATTERN_MAX	64

typedef struct {
	uint8_t			open;		/* slot taken (async open may be in flight) */
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
	int8_t			ready;		/* 1 ready, 0 async open in flight, -1 failed */
	uint8_t			pad2[3];
	wld_chunk_t		*ch;		/* per chunk */
	uint32_t		*style_epoch;	/* per styled-meshlet slot: last colouring */
	uint16_t		nstyle_slots, style_used;
	uint8_t			*page_state;	/* per TEXP page: WLD_PAGE_STATE bytes (gpu) */
	wld_mover_t		*movers;
	uint16_t		nmovers;
	/* translucent ENTS placements (drawn by the translucent pass, not as
	 * M1 movers): movers[nsub - ntrans_movers .. nsub), for the stream's
	 * residency of the MOVR chunks a brush entity will draw */
	uint16_t		ntrans_movers;
	uint8_t			has_start;
	uint8_t			reencode;	/* running tiny3d encodes strips differently */
	uint8_t			high;		/* layer-region blocks at the high end */
	int32_t			want_leaf;	/* OPEN leaf of the target set (-1 none) */
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

/* Open map (synchronous; returns the open layer if it is already open,
 * finishing an async open of it); sys_error on a bad package or no room.
 * Boot / New Game / setpos only (load I/O). */
wld_layer_t	*wld_open(int map);
/* wld_open into layer slot `slot` (a load restores every layer in its
 * saved slot: entity gids, names and style slots carry it); NULL when
 * that slot is taken by another map. */
wld_layer_t	*wld_open_in(int map, int slot);
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
 * flight (every layer loop sees whole layers only). Inline: every
 * subsystem asks it per entity / per frame. */
extern wld_layer_t	wld_layers[HL64_WORLD_LAYERS];
static inline wld_layer_t *wld_layer(int slot)
{
	return (unsigned)slot < HL64_WORLD_LAYERS && wld_layers[slot].open
		&& wld_layers[slot].ready == 1 ? &wld_layers[slot] : NULL;
}
/* Layer holding map, ready or still opening; NULL when not open. */
wld_layer_t	*wld_layer_of_map(int map);
/* Bytes a layer takes of the layer region: the directory block plus the
 * tables block, 16-aligned (the cook proof tools/cook/layerdirs.py
 * computes the same sum per CWG link pair). */
uint32_t	wld_layer_bytes(uint32_t dir_size, uint32_t nchunks, uint32_t npages,
			uint32_t nsub);

/* BSP leaf of a map-local point (hull 0, grid start node). */
int		wld_point_leaf(const wld_layer_t *L, const float p[3]);
/* Sectors visible from a leaf (vis list: count, then sector indices). */
const uint16_t	*wld_leaf_sectors(const wld_layer_t *L, int leaf, int *count);
/* The decoded PVS row of BSP leaf `leaf` (bit i = visleaf i + 1) from the
 * layer's VISR chunk (architecture.md 5.5), one decoded row cached per
 * layer: the compressed row comes from the row cache the stream fills
 * asynchronously (the eye leaf and the leaves ahead, pin / arrival eyes:
 * engine/world/stream.c); a row not cached is read synchronously (the
 * offset pair and the row, one 16-byte-line DMA each; counted as blocked
 * I/O). *visleafs = the map's visleaf count (0: no vis). NULL when
 * everything is visible (no vis, leaf 0 or past visleafs, empty row). The
 * row stays valid until the next call for this layer. */
const uint8_t	*wld_pvs_row(wld_layer_t *L, int leaf, int *visleafs);

/* ---- residency ----------------------------------------------------------- */

/* Resident bytes of chunk c (marks it used this frame), or NULL after
 * queuing its read at OPEN priority (the renderer's missing chunks). */
uint8_t		*wld_chunk_use(wld_layer_t *L, int c);
/* A standalone OPEN target set (harnesses without wld_stream_frame): REND
 * and every page of each sector the leaf sees, named in a new target
 * generation; nothing of it is evicted until the next one. */
void		wld_want_open(wld_layer_t *L, int leaf);
/* Resident bytes of COLL chunk c, read synchronously when absent (a
 * chunk missing mid-trace: blocked I/O, architecture.md 5.2); a queued
 * read of it is finished first. */
uint8_t		*wld_chunk_sync(wld_layer_t *L, int c);
/* Frame boundary: retire the GPU fences two frames old. */
void		wld_frame(uint32_t frame);

/* ---- M4 residency classes (architecture.md 5.3; engine/world/stream.c) --- */

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
/* New Game after every layer closed: the stream's boot state (pins,
 * teleport window, previous position), stats kept. */
void		wld_stream_new_game(void);
/* engine/coll col_want: this frame's collision window (global box, the
 * player box + COL_WINDOW) and the brush models touching it (slot << 16 |
 * BSP model); an input of the target set (OPEN COLL, and + v * 0.5 s for
 * prefetch). */
void		wld_stream_coll(const float lo[3], const float hi[3], const uint32_t *models, int n);
/* engine/ent rec, once per pump scan of a slot: the spawn boxes (map-local
 * lo[3], hi[3]; a DROP_TO_FLOOR's reach below included) of its hydration
 * candidates and prefetch ring, nearest first, and a brush record's own
 * submodel (sub[i], -1 none): the grid cells of each box + 128 u (its
 * first steps) and the submodel's COLL are prefetched (an input of the
 * target set; n 0 clears). */
void		wld_stream_spawns(int slot, const float (*box)[6], const int *sub, int n);
/* rec's map-open hold of `slot` waits on its spawn windows (on 1, off 0):
 * they are named at the pin class (after the want measure, as the route's
 * point pins), so they may evict prefetch chunks; else the windows of a
 * fight standing in a full collision pool time the gate out. */
void		wld_stream_spawn_gate(int slot, int on);
/* 1 when every COLL chunk of a spawn window (as wld_stream_spawns names
 * it; a brush record's: its submodel's alone) is resident (rec's map-open
 * hold). */
int		wld_spawn_resident(int slot, const float box[6], int sub);
/* The renderer, once per drawn frame: chunks of the visible set it wanted
 * and found missing (pop-in, counted outside / inside teleport windows). */
void		wld_stream_missing(uint32_t n);

typedef struct {
	uint32_t	loads, evictions, nofit, queued;
	uint32_t	pool_used[3], pool_peak[3];	/* render, texture, collision */
	/* the most bytes the open + pin classes of one target generation
	 * wanted (prefetch excluded): the pools' need; pool_peak is the
	 * LRU cache's high water */
	uint32_t	want_peak[3];
	uint32_t	dirs_used, dirs_peak, dirs_size;	/* layer region */
} wld_stats_t;
void		wld_stats(wld_stats_t *s);

/* wld_stream_frame's sub-steps (seam-window profiler zones): async layer
 * opens + PVS row step, entity windows / inputs scan, the target set's
 * naming (rebuild or retry), the standing movers, the seam pins'
 * residency check. */
enum { WLD_SS_OPEN, WLD_SS_SCAN, WLD_SS_NAME, WLD_SS_MOVERS, WLD_SS_PINS, WLD_SS_N };

/* Stream counters since boot (stats record `stream:`); us / frames since
 * the last reset (prof roll-up). delta: frames whose moved entity /
 * spawn windows were named into the standing generation (no rebuild).
 * sub_us (per WLD_SS_*) and room_us (chunk
 * requests' and sync reads' room making: weaker queued reads, LRU
 * search, anywhere in the frame) are cumulative since boot (u32, wrap). */
typedef struct {
	uint32_t	popin, popin_tele, pins, prefetch, recompute, reassert, delta;
	uint32_t	teleports, promoted, evict_queued, open_async, open_fail;
	uint32_t	frames, us_sum, us_max;
	uint32_t	sub_us[WLD_SS_N], room_us;
} wld_stream_stats_t;
void		wld_stream_stats(wld_stream_stats_t *s, int reset);

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
