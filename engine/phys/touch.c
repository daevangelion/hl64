/* SPDX-License-Identifier: MIT */
/*
 * touch.c -- trigger touches and impact touches
 * (docs/specs/area-links-and-touch.md 3, 4).
 *
 * The trigger pass takes its candidate list (with serials) before the
 * first call; a touch may free or relink anything, so each later call
 * re-checks that both entities are still the same live slots. Overlaps are
 * tested in the mover's frame (architecture.md 8): a trigger of another
 * layer is moved by g_M(its layer) - g_M(mover's layer). Dispatch goes to
 * the game's touch hook (phy_dispatch_touch), frame layer = the toucher's.
 */
#include "hl64/ent.h"
#include "hl64/rec.h"
#include "engine/core/log.h"
#include "engine/ent/ent.h"
#include "engine/phys/phys.h"

/* Triggers touched in one link; more are dropped and logged. */
#define PHY_TOUCH_MAX	64

static const float zero3[3];

static void call(col_ent_t *self, col_ent_t *other)
{
	if (ent_c.tm[self->index].touch)
		phy_dispatch_touch(self->index, other->index);
}

void phy_link_touch(col_ent_t *e)
{
	uint16_t list[PHY_TOUCH_MAX], serial[PHY_TOUCH_MAX];
	ent_h me;
	int n, i, layer = ent_c.id[e->index].layer;

	ent_link(e);
	if (e->solid == COL_SOLID_NOT)
		return;
	/* pending records in the box hydrate first (area-links-and-touch.md) */
	{
		const float *gm = eng_layer_origin(layer);
		float gmins[3], gmaxs[3];
		int k;

		for (k = 0; k < 3; k++)
		{
			gmins[k] = e->absmin[k] + gm[k];
			gmaxs[k] = e->absmax[k] + gm[k];
		}
		rec_wake_box(gmins, gmaxs);
	}
	n = ent_query_in(layer, e->absmin, e->absmax, ENT_Q_TRIGGER, list, PHY_TOUCH_MAX);
	if (n == PHY_TOUCH_MAX)
		log_printf("touch: %d triggers at entity %u, extra dropped\n", n, e->index);
	for (i = 0; i < n; i++)
		serial[i] = ent_c.id[list[i]].serial;
	me = ent_handle(e);
	for (i = 0; i < n; i++)
	{
		col_ent_t *t = &ent_c.cv[list[i]];

		if (ent_resolve(me) != e)
			return;
		if (list[i] == e->index || !ent_live(list[i]) || ent_c.id[list[i]].serial != serial[i])
			continue;
		if (!ent_c.tm[list[i]].touch)
			continue;
		/* brush trigger: the mover's origin inside the trigger brush's
		 * hull for the mover's size, placed as for a trace
		 * (trace-and-hulls.md 2), in the model's unrotated frame */
		if (t->model != COL_NONE)
		{
			int hull = col_hull_for_size(e->mins, e->maxs), k, tl = ent_c.id[list[i]].layer;
			float o[3], base[3];

			if (tl != layer)
			{
				float d[3];

				ent_layer_delta(tl, layer, d);
				for (k = 0; k < 3; k++)
					base[k] = t->origin[k] + d[k];
			}
			else
				for (k = 0; k < 3; k++)
					base[k] = t->origin[k];
			for (k = 0; k < 3; k++)
				o[k] = hull ? base[k] + (col_hull_mins[hull][k] - e->mins[k]) : base[k];
			if (col_model_contents(t->model, o, zero3, hull, e->origin) != COL_SOLID)
				continue;
		}
		call(t, e);
	}
}

void phy_touch(col_ent_t *self, col_ent_t *other)
{
	if (ent_get(self->index) == self && ent_get(other->index) == other
		&& self->solid != COL_SOLID_NOT)
		call(self, other);
}

void phy_impact(col_ent_t *a, col_ent_t *b)
{
	ent_h ha = ent_handle(a), hb = ent_handle(b);

	if (a->solid != COL_SOLID_NOT)
		call(a, b);
	if (ent_resolve(ha) != a || ent_resolve(hb) != b)
		return;
	if (b->solid != COL_SOLID_NOT)
		call(b, a);
}
