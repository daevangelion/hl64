/* SPDX-License-Identifier: MIT */
/*
 * grid.h -- entity slots (M2), linked boxes, the spatial grid and touches
 * (docs/specs/area-links-and-touch.md, docs/architecture.md 6.2, 8).
 *
 * M2 storage: one col_ent_t (include/hl64/coll.h, the collision view) per
 * slot, owned by engine/ent; pointers stay valid for the slot's life. The
 * M3 components (architecture.md 6.2) replace this storage and keep the
 * functions. Slot 0 is the world, slot 1 the player.
 *
 * Callers write origin / angles / mins / maxs / solid / model / skin in
 * the view, then link: ent_link recomputes absmin / absmax (spec 1) and the
 * grid membership; phy_link_touch also runs the trigger touches (spec 3).
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
/* Lowest free slot >= 2: a zeroed view with index set, owner / model /
 * studio COL_NONE, no touch function, not linked. NULL when full. */
col_ent_t	*ent_alloc(void);
/* The given free slot (world, player, replays that keep GoldSrc indices);
 * NULL when it is taken or out of range. */
col_ent_t	*ent_alloc_at(int index);
/* Unlink (contents brushes leave col_contents too), serial += 1. */
void		ent_free(col_ent_t *e);
/* The slot's view, NULL when free or out of range. */
col_ent_t	*ent_get(int index);
ent_h		ent_handle(const col_ent_t *e);
/* NULL when the handle's slot was freed since. */
col_ent_t	*ent_resolve(ent_h h);

/* Touch function id (game dispatch; 0 = none). */
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
 * (touching faces count), no duplicates, at most max (the rest are
 * dropped and counted). Order: triggers alone by touch rank (spec 3.1),
 * anything else in ascending index. */
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

/* ---- touches (engine/phys/touch.c) ---------------------------------------------- */

/* The game's touch dispatch: fn = the toucher's function id. M3 points it
 * at the game's funcs.def dispatch; tests install their own. */
typedef void	(*phy_touch_call_fn)(uint16_t fn, col_ent_t *self, col_ent_t *other);
void		phy_set_touch_call(phy_touch_call_fn fn);

/* Link with touch (spec 3): ent_link, then every overlapping trigger with
 * a touch function, in touch-rank order, self = trigger, other = e; brush
 * triggers only when e's origin is inside their brush for the hull of e's
 * size (col_hull_for_size), angles ignored. A SOLID_NOT e touches nothing. */
void		phy_link_touch(col_ent_t *e);
/* self.touch(other) when both are live and self has a touch function and
 * is not SOLID_NOT (pmove touch lists). */
void		phy_touch(col_ent_t *self, col_ent_t *other);
/* Impact touch (spec 4): a.touch(b), then b.touch(a) only if a and b are
 * still live. */
void		phy_impact(col_ent_t *a, col_ent_t *b);

#endif
