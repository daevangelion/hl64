/* SPDX-License-Identifier: MIT */
/*
 * rec_int.h -- engine/ent record module internals (rec.c, rec_hle.c) and
 * the hooks the engine and the host tests use beside include/hl64/rec.h.
 */
#ifndef HL64_REC_INT_H
#define HL64_REC_INT_H

#include <stdint.h>

#include "hl64/limits.h"
#include "hl64/types.h"

/* game/defs key and class ids as the engine sees them */
enum {
#define KEY(id, name) RK_##id,
#include "game/defs/keys.def"
#undef KEY
	RK__N
};
enum {
#define CLASS(id, name) RC_##id,
#include "game/defs/classes.def"
#undef CLASS
	RC__N
};

/* HLE slots (architecture.md 4.1, 5.3): an open map's slot is its world
 * layer slot; a closed map keeps only its frozen runtime part (record
 * states + logic blocks, REC_FROZEN_MAPS of them) addressed by CWG map. */
#define REC_SLOTS		HL64_WORLD_LAYERS
#define REC_FROZEN_MAPS		2
#define REC_TIMERS		128	/* think-and-time.md 5 */
#define REC_CASCADE		1024	/* timer callbacks per frame */
#define REC_SYNC_WAKES		2	/* hydrations a frame's fires do in-call */
#define REC_DEFER		64	/* uses waiting for their record (canonical FIFO) */
#define REC_HOLDERS		64	/* runtime entities holding a map name */
#define REC_RUNTIME		64	/* runtime entities with a durable id (6.4) */
#define REC_TWINS		16	/* aliased twin pairs (cwg-format.md twins) */
/* architecture.md 6.4 radii (u, map-local box distance to an anchor) */
#define REC_READY_R		768.0f	/* must be live by here (`late` counts misses) */
#define REC_ARRIVAL_R		192.0f	/* arrival ring past it */
#define REC_SPAWN_R		(REC_READY_R + REC_ARRIVAL_R)
#define REC_RELEASE_R		1024.0f
#define REC_PREFETCH_R		256.0f	/* model-request ring past the spawn radius */
#define REC_RING		6
#define REC_RELEASE_PERIOD	0.1	/* s between release batches of a slot */
#define REC_RELEASE_BATCH	8	/* releases a batch may try past the budget */
#define REC_RELEASE_RETRY	0.5	/* s before a refused entity is asked again */
#define REC_BUDGET_US		2000u	/* playable builds: pump wall time per frame */
#define REC_BUDGET_HOST		8	/* HL64_TEST builds (host and cart): records
					   per frame, deterministic (K10) */

/* HLE region byte accounting (rec_init, open_chunk, freeze_runtime; rec.c
 * asserts these against the structs). The cook reads the defines and
 * proves the region against HL64_HLE_BLOBS_KB (tools/w64c/hleids.py,
 * architecture.md 4.1). A frozen map's block: 2-bit record states
 * ((records + 3) / 4 B, padded to 4) + REC_LOGIC_BYTES per logic record. */
#define REC_RT_BYTES		24	/* rrt_t, per record */
#define REC_LOGIC_BYTES		16	/* rec_logic_t, per logic record */
#define REC_TABLE_BYTES		20016	/* rec_init's tables before the slot heap */
/* dehydrate store entry header (map, lump, rtid, len, bounds) before each
 * delta (4-aligned) */
#define REC_STORE_HDR		20

/* infodecal `texture` ids (hle-format-v1.md "Ids"): below
 * REC_DECALS_GLOBAL the gsfx/decals.i4 entry (tools/cook/decals.py
 * GLOBAL_COUNT), then the entries of the map package's DECL chunk */
#define REC_DECALS_GLOBAL	42
#define REC_DECALS_MAP_MAX	64	/* entries of one DECL sheet */
#define REC_DECL_FOURCC		0x4445434Cu	/* 'DECL' chunk of a .w64 */
#define REC_DECL_MAGIC		0x44493402u	/* 'DI4' 2 sheet */

/* The id spaces an HLE1 chunk's typed ids are checked against. */
typedef struct {
	uint32_t	classes, patterns, globals, titles;
	uint32_t	kcount[8];	/* res_t kinds */
	uint32_t	nsub;		/* the layer's BSP models */
	uint32_t	ndecals;	/* REC_DECALS_GLOBAL + the layer's DECL entries */
} rec_limits_t;

/* Validate a chunk (HLE1 + optional HLN1 tail, docs/formats/
 * hle-format-v1.md), bring it to host order in place: hle_fixup, the id
 * rules ("Ids"), name / aux lists naming kept records, the node graph
 * (hln_fixup). NULL, or the rule broken. Pure: the fuzz test calls it. */
const char	*rec_hle_check(void *blob, uint32_t len, const rec_limits_t *lim,
			const void **graph, uint32_t *graph_size);

/* Finish an async rec_map_open now (New Game, tests): the outstanding
 * chunk reads complete as load I/O (io_set_load), the slot goes ready.
 * The world layer must be ready (wld_open). */
void		rec_map_finish(int slot);
/* Boot: bind the HLE and dehydrate regions, empty everything. */
void		rec_init(void);
/* Close every slot, drop timers, uses, holders and the store (New Game,
 * tests). */
void		rec_reset(void);
/* PROFILE / DBG_MEM lines: `prof rec_us ...`, `mem rec ...`. */
void		rec_prof_line(void);
void		rec_mem_line(void);

#endif
