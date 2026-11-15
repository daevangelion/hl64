/* SPDX-License-Identifier: MIT */
/*
 * grid.h -- entity slots, linked boxes, the spatial grid and touches
 * (docs/specs/area-links-and-touch.md, docs/architecture.md 6.2, 8).
 *
 * Storage: the col_ent_t collision view is the `cv` component of
 * include/hl64/ent.h; pointers stay valid for the slot's life. Slot 0 is
 * the world, slot 1 the player. These calls are the M2 slot API over the
 * M3 components (ent_alloc = ent_create(ENT_ALLOC_GAME) without the fatal
 * exhaustion, ent_free = ent_destroy).
 *
 * Callers write origin / angles / mins / maxs / solid / model / skin in
 * the view (map-local to the entity's layer, ent_id_t.layer; -1 = the
 * global frame), then link: ent_link recomputes absmin / absmax (spec 1,
 * map-local) and the grid membership (global: + g_M(layer));
 * phy_link_touch also runs the trigger touches (spec 3).
 * SOLID_NOT entities are not in the grid, except contents brushes (brush
 * model, skin < -1: func_ladder, func_water), which the grid keeps for
 * ENT_Q_CONTENTS and hands to col_contents_add.
 */
#ifndef HL64_GRID_H
#define HL64_GRID_H

#include <stdint.h>

#include "hl64/coll.h"

/* index | serial << 16 */
typedef uint32_t	ent_h;

#define ENT_WORLD	0
#define ENT_PLAYER	1

/* ---- slots ------------------------------------------------------------------ */

/* Bind the components and grid regions (boot carve) and free every slot. */
void		ent_init(void);
/* Free every slot and empty the grid (New Game, tests). */
void		ent_reset(void);
/* ent_create(ENT_ALLOC_GAME) (the K-alloc slot rule, components zeroed,
 * owner / model / studio COL_NONE, no touch function, not linked), NULL
 * when no slot is free. */
col_ent_t	*ent_alloc(void);
/* The given free slot (world, player, replays that keep GoldSrc indices),
 * initialised as ent_alloc does; NULL when it is taken or out of range. */
col_ent_t	*ent_alloc_at(int index);
/* ent_destroy: unlink (contents brushes leave col_contents too), leave
 * the heap and active sets, serial += 1. */
void		ent_free(col_ent_t *e);
/* The slot's view, NULL when free or out of range. */
col_ent_t	*ent_get(int index);
ent_h		ent_handle(const col_ent_t *e);
/* NULL when the handle's slot was freed since. */
col_ent_t	*ent_resolve(ent_h h);

/* Touch function id (tm.touch: game dispatch; 0 = none). */
void		ent_set_touch(col_ent_t *e, uint16_t fn);
uint16_t	ent_touch_fn(const col_ent_t *e);
/* Cooked touch rank of a map trigger (HLE1 record `trank`, spec 3.1);
 * map_slot = the record's HLE slot. Entities without one (runtime
 * triggers) rank after every map trigger, in allocation order. */
void		ent_set_rank(col_ent_t *e, int map_slot, uint16_t rank);

/* ---- links and queries -------------------------------------------------------- */

/* Linked box and grid membership, no touches. */
void		ent_link(col_ent_t *e);
void		ent_unlink(col_ent_t *e);
int		ent_linked(const col_ent_t *e);

#define ENT_Q_SOLID	1	/* SOLID_BBOX, SOLID_SLIDEBOX, SOLID_BSP */
#define ENT_Q_TRIGGER	2	/* SOLID_TRIGGER */
#define ENT_Q_CONTENTS	4	/* contents brushes (SOLID_NOT) */

/* Linked entities of the kinds in mask whose linked box meets [mins, maxs]
 * (global coordinates; touching faces count), no duplicates, at most max
 * (the rest are dropped and counted). Order: triggers alone by touch rank
 * (spec 3.1), anything else in ascending index. */
int		ent_query(const float mins[3], const float maxs[3], int mask,
			uint16_t *out, int max);

/* Counters since ent_reset (cells = non-empty buckets walked, candidates =
 * entities tested, scans = queries too large for the cells, dry = links
 * sent to the huge list because the membership pool was empty); linked
 * now; peak_bucket = the longest bucket now. */
typedef struct {
	uint32_t	links, queries, cells, candidates, overflow, scans, dry;
	uint16_t	linked, peak_bucket;
} ent_grid_stats_t;
void		ent_grid_stats(ent_grid_stats_t *s);
/* ent_grid_stats' queries alone (cheap: the AI work units count them) */
uint32_t	ent_grid_queries(void);

/* ---- touches (engine/phys/touch.c) ---------------------------------------------- */

/* Dispatch: phy_game_hooks_t.touch (include/hl64/ent.h), self = the
 * toucher, frame layer = self's layer. */

/* Link with touch (spec 3): ent_link, then every overlapping trigger with
 * a touch function, in touch-rank order, self = trigger, other = e; brush
 * triggers only when e's origin is inside their brush for the hull of e's
 * size (col_hull_for_size), angles ignored; overlaps are tested in e's
 * frame. A SOLID_NOT e touches nothing. */
void		phy_link_touch(col_ent_t *e);
/* self.touch(other) when both are live and self has a touch function and
 * is not SOLID_NOT (pmove touch lists). */
void		phy_touch(col_ent_t *self, col_ent_t *other);
/* Impact touch (spec 4): a.touch(b), then b.touch(a) only if a and b are
 * still live. */
void		phy_impact(col_ent_t *a, col_ent_t *b);

#endif
