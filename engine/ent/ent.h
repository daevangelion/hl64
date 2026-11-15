/* SPDX-License-Identifier: MIT */
/*
 * ent.h -- engine/ent internals shared by the slot table and components
 * (ent.c), the spatial grid (grid.c), the think heap and active sets
 * (heap.c), the active-set pass (pass.c), the private slabs (priv.c) and
 * the touch pass (engine/phys/touch.c). The public API is
 * include/hl64/ent.h and include/hl64/grid.h.
 */
#ifndef HL64_ENT_INT_H
#define HL64_ENT_INT_H

#include <stdint.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"

#define ENT_UNLINKED	0xFFFF

/* Grid node of a slot (grid.c): the kind, the query stamp, the first cell
 * membership and the linked box (global: + g_M of the entity's layer)
 * rounded outwards to integers (clamped to s16), in one 16-byte cache
 * line, so a rejected candidate costs one miss; the exact test reads the
 * view's absmin / absmax only when the rounded boxes meet. */
typedef struct {
	uint8_t		kind;		/* ENT_Q_* | level << 4; 0 = not in the grid */
	uint8_t		stamp;		/* last query that met it */
	uint16_t	cells;		/* first membership (ent_cell_t), ENT_UNLINKED */
	int16_t		lo[3], hi[3];	/* the global linked box rounded outwards */
} ent_node_t;			/* 16 */

/* One cell membership: entry i < HL64_MAX_ENTITIES is slot i's own first
 * membership, the rest a shared pool. */
typedef struct {
	uint16_t	ent, next;	/* the bucket's chain */
	uint16_t	bucket, enext;	/* the entity's next membership */
} ent_cell_t;			/* 8 */

#define ENT_CELLS	(3 * HL64_MAX_ENTITIES)
/* 32-bit words of a per-slot bitset */
#define ENT_WORDS	((HL64_MAX_ENTITIES + 31) / 32)

/* Per-slot engine state beside the components; arrays of
 * HL64_MAX_ENTITIES (cells: ENT_CELLS) in the components region
 * (architecture.md 4.1), 16-byte aligned. */
typedef struct {
	ent_node_t	*node;
	ent_cell_t	*cell;
	uint32_t	*rank;		/* touch order key (spec 3.1) */
	float		*freetime;	/* game clock (f32 s) when freed; 0 never used */
	uint8_t		*contents;	/* handed to col_contents_add */
	uint32_t	seq;		/* allocation counter (runtime trigger order) */
	uint16_t	top;		/* highest slot ever allocated + 1 */
	/* every slot from here up is as boot left it (ent_init's clear, serial
	 * and free time 0): no create since boot, not even before a New Game
	 * (ent_reset keeps serials), nor a load's restore (ent_save) */
	uint16_t	touched;
	uint16_t	live;		/* live slots */
	uint16_t	live_peak;
	uint32_t	region_bytes;	/* components region used */
} ent_slots_t;

extern ent_slots_t	ent_s;

/* Rank key of an entity without a cooked rank: after every map trigger. */
#define ENT_RANK_RUNTIME	0x80000000u

static inline int ent_live(int i)
{
	return ent_is_live(i);
}

/* grid.c */
void	ent_grid_init(void);
void	ent_grid_clear(void);
/* ent_query in the frame of layer `frame` (-1: global): mins / maxs and
 * the exact box test are frame-local (a candidate of another layer is
 * moved by g_M(its layer) - g_M(frame)); cells and rounded boxes global. */
int	ent_query_in(int frame, const float mins[3], const float maxs[3], int mask,
		uint16_t *out, int max);
#ifdef HL64_TEST
/* grid_bench.c: registers the `bench` debug command */
void	ent_grid_bench_init(void);
#endif

/* ent.c: g_M(layer) - g_M(frame) (frame -1: global). */
void	ent_layer_delta(int layer, int frame, float d[3]);

/* heap.c: think heap and active sets (THINK_GRID region) */
void	ent_sets_init(void);
void	ent_sets_clear(void);
/* 16-aligned bytes from the THINK_GRID region (boot; sys_error when full). */
void	*ent_think_take(uint32_t bytes);
uint32_t ent_sets_region_bytes(void);
/* Linked (ent_link .. ent_unlink, any solid type: the searches). */
void	ent_sets_set_linked(int e, int on);
int	ent_sets_linked(int e);
/* The next linked slot >= from, -1 when none. */
int	ent_sets_next_linked(int from);
/* Re-evaluate e's heap entry (tm.nextthink) and movers / kill membership
 * from its components. */
void	ent_sets_update(int e);
/* Leave the heap and every set (freeing). */
void	ent_sets_remove(int e);
/* Movers membership rule (think-and-time.md 3). */
int	ent_needs_step(int e);
/* Live entities whose groundentity is e (being freed) join the movers
 * once (their next step re-evaluates). */
void	ent_sets_ground_lost(int e);
/* Pop every heap entry due by time + dt into the due set; heap_max stat. */
void	ent_heap_pop_due(double time, double dt);
/* Next slot >= from in due | movers | kill (| live when all), -1 none. */
int	ent_sets_next(int from, int all);
void	ent_sets_clear_due(int e);
int	ent_heap_count(void);
int	ent_movers_count(void);
/* heap / sets self-check (tests): 0 ok, else a description */
const char *ent_sets_check(void);

/* The active-set pass in progress (pass.c): the entity being stepped, the
 * frame's time and dt; ent_think_at makes a higher-index entity due this
 * frame through ent_sets_mark_due. */
typedef struct {
	int		active;
	int		cursor;
	double		time, dt;
} ent_pass_t;
extern ent_pass_t	ent_pass;
void	ent_sets_mark_due(int e);

/* priv.c: game private slabs (GAME_PRIVATE region) */
void	ent_priv_init(void);
void	ent_priv_reset(void);
void	ent_priv_free(int e);
typedef struct {
	uint32_t	live[6], peak[6];	/* blocks per class 32..1024 B */
	uint32_t	pages, pages_peak, pages_total, refused;
	uint32_t	bytes_live, bytes_peak;
} ent_priv_stats_t;
void	ent_priv_stats(ent_priv_stats_t *s);

/* The class name of an entity for messages (classes.def). */
const char *ent_class_name(int e);

#endif
