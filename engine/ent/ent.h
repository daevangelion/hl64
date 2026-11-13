/* SPDX-License-Identifier: MIT */
/*
 * ent.h -- engine/ent internals shared by the slot table (ent.c), the
 * spatial grid (grid.c) and the touch pass (engine/phys/touch.c). The
 * public API is include/hl64/grid.h.
 */
#ifndef HL64_ENT_INT_H
#define HL64_ENT_INT_H

#include <stdint.h>

#include "hl64/grid.h"
#include "hl64/limits.h"

#define ENT_UNLINKED	0xFFFF

enum { ENT_FREE, ENT_LIVE };

/* Grid node of a slot (grid.c): the kind, the query stamp, the first cell
 * membership and the linked box rounded outwards to integers (clamped to
 * s16), in one 16-byte cache line, so a rejected candidate costs one
 * miss; the exact test reads the view's absmin / absmax only when the
 * rounded boxes meet. */
typedef struct {
	uint8_t		kind;		/* ENT_Q_* | level << 4; 0 = not in the grid */
	uint8_t		stamp;		/* last query that met it */
	uint16_t	cells;		/* first membership (ent_cell_t), ENT_UNLINKED */
	int16_t		lo[3], hi[3];	/* the linked box rounded outwards */
} ent_node_t;			/* 16 */

/* One cell membership: entry i < HL64_MAX_ENTITIES is slot i's own first
 * membership, the rest a shared pool. */
typedef struct {
	uint16_t	ent, next;	/* the bucket's chain */
	uint16_t	bucket, enext;	/* the entity's next membership */
} ent_cell_t;			/* 8 */

#define ENT_CELLS	(3 * HL64_MAX_ENTITIES)

/* Per-slot state beside the views; arrays of HL64_MAX_ENTITIES (cells:
 * ENT_CELLS) in the components region (architecture.md 4.1), 16-byte
 * aligned. */
typedef struct {
	col_ent_t	*v;
	ent_node_t	*node;
	ent_cell_t	*cell;
	uint16_t	*serial;
	uint8_t		*state;		/* ENT_FREE / ENT_LIVE */
	uint16_t	*touch;		/* touch function id, 0 none */
	uint32_t	*rank;		/* touch order key (spec 3.1) */
	uint8_t		*contents;	/* handed to col_contents_add */
	uint32_t	seq;		/* allocation counter (runtime trigger order) */
	uint16_t	top;		/* highest slot ever allocated + 1 */
} ent_slots_t;

extern ent_slots_t	ent_s;

/* Rank key of an entity without a cooked rank: after every map trigger. */
#define ENT_RANK_RUNTIME	0x80000000u

static inline int ent_live(int i)
{
	return i >= 0 && i < HL64_MAX_ENTITIES && ent_s.state[i] == ENT_LIVE;
}

/* grid.c */
void	ent_grid_init(void);
void	ent_grid_clear(void);
#ifdef HL64_TEST
/* grid_bench.c: registers the `bench` debug command */
void	ent_grid_bench_init(void);
#endif

#endif
