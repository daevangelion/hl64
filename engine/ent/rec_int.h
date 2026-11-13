/* SPDX-License-Identifier: MIT */
/*
 * rec_int.h -- engine/ent record module internals (rec.c, rec_hle.c) and
 * the hooks the engine and the host tests use beside include/hl64/rec.h.
 */
#ifndef HL64_REC_INT_H
#define HL64_REC_INT_H

#include <stdint.h>

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

#define REC_SLOTS		4	/* HLE slots (architecture.md 5.3) */
#define REC_TIMERS		128	/* think-and-time.md 5 */
#define REC_CASCADE		1024	/* timer callbacks per frame */
#define REC_SYNC_WAKES		2	/* hydrations a frame's fires do in-call */
#define REC_DEFER		64	/* uses waiting for their record */
#define REC_HOLDERS		64	/* runtime entities holding a map name */
#define REC_SPAWN_R		(768.0f + 192.0f)	/* architecture.md 6.4 */
#define REC_PREFETCH_R		256.0f	/* model-request ring past it */
#define REC_RING		6
#define REC_BUDGET_US		2000u	/* cart pump budget per frame */
#define REC_BUDGET_HOST		8	/* host pump budget: records */

/* The id spaces an HLE1 chunk's typed ids are checked against. */
typedef struct {
	uint32_t	classes, patterns, globals, titles;
	uint32_t	kcount[8];	/* res_t kinds */
	uint32_t	nsub;		/* the layer's BSP models */
} rec_limits_t;

/* Validate a chunk (HLE1 + optional HLN1 tail, docs/formats/
 * hle-format-v1.md), bring it to host order in place: hle_fixup, the id
 * rules ("Ids"), name / aux lists naming kept records, the node graph
 * (hln_fixup). NULL, or the rule broken. Pure: the fuzz test calls it. */
const char	*rec_hle_check(void *blob, uint32_t len, const rec_limits_t *lim,
			const void **graph, uint32_t *graph_size);

/* Boot: bind the HLE and dehydrate regions, empty everything. */
void		rec_init(void);
/* Close every slot, drop timers, uses, holders and the store (New Game,
 * tests). */
void		rec_reset(void);
/* PROFILE / DBG_MEM lines: `prof rec_us ...`, `mem rec ...`. */
void		rec_prof_line(void);
void		rec_mem_line(void);

#endif
