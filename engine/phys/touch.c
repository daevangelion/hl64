/* SPDX-License-Identifier: MIT */
/*
 * touch.c -- trigger touches and impact touches
 * (docs/specs/area-links-and-touch.md 3, 4).
 *
 * The trigger pass takes its candidate list (with serials) before the
 * first call; a touch may free or relink anything, so each later call
 * re-checks that both entities are still the same live slots.
 */
#include "engine/core/log.h"
#include "engine/ent/ent.h"

/* Triggers touched in one link; more are dropped and logged. */
#define PHY_TOUCH_MAX	64

static phy_touch_call_fn touch_call;
static const float zero3[3];

void phy_set_touch_call(phy_touch_call_fn fn)
{
	touch_call = fn;
}

static void call(col_ent_t *self, col_ent_t *other)
{
	uint16_t fn = ent_s.touch[self->index];

	if (fn && touch_call)
		touch_call(fn, self, other);
}

void phy_link_touch(col_ent_t *e)
{
	uint16_t list[PHY_TOUCH_MAX], serial[PHY_TOUCH_MAX];
	ent_h me;
	int n, i;

	ent_link(e);
	if (e->solid == COL_SOLID_NOT)
		return;
	n = ent_query(e->absmin, e->absmax, ENT_Q_TRIGGER, list, PHY_TOUCH_MAX);
	if (n == PHY_TOUCH_MAX)
		log_printf("touch: %d triggers at entity %u, extra dropped\n", n, e->index);
	for (i = 0; i < n; i++)
		serial[i] = ent_s.serial[list[i]];
	me = ent_handle(e);
	for (i = 0; i < n; i++)
	{
		col_ent_t *t = &ent_s.v[list[i]];

		if (ent_resolve(me) != e)
			return;
		if (list[i] == e->index || !ent_live(list[i]) || ent_s.serial[list[i]] != serial[i])
			continue;
		if (!ent_s.touch[list[i]])
			continue;
		/* brush trigger: the mover's origin inside its hull-1 brush,
		 * in the model's unrotated frame */
		if (t->model != COL_NONE
			&& col_model_contents(t->model, t->origin, zero3, 1, e->origin) != COL_SOLID)
			continue;
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
