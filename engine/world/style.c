/* SPDX-License-Identifier: MIT */
/*
 * style.c -- lightstyle values (docs/specs/lightstyles.md 1-3), written
 * from the spec.
 *
 * 96 global slots: 0-31 common, then 32 per open geometry layer for its
 * switchable styles 32-63. A slot holds a cooked pattern (letters 0-25)
 * or nothing (value 256). Values are recomputed on the game clock's
 * 100 ms tick only: index = floor(time x 10) mod length, value = letter x
 * 22. A slot whose value changes is stamped with a new epoch; the renderer
 * recolours a styled meshlet when one of its slots carries a stamp newer
 * than the meshlet's last colouring, so frames between ticks and unchanged
 * values cost nothing.
 */
#include "engine/core/sys.h"
#include "engine/world/wld.h"

#define UNSET_VALUE	256

typedef struct {
	const uint8_t	*letters;
	uint32_t	changed;	/* epoch of the last value change */
	uint16_t	value;
	uint8_t		len;		/* 0: unset */
	uint8_t		pad;
} slot_t;

static slot_t	slots[WLD_STYLE_SLOTS_ALL];
static uint32_t	epoch = 1;
static uint64_t	tick = ~(uint64_t)0;	/* floor(time x 10) of the last recompute */

static uint16_t eval(const slot_t *s)
{
	uint64_t t = tick == ~(uint64_t)0 ? 0 : tick;

	return s->len ? (uint16_t)(s->letters[t % s->len] * 22u) : UNSET_VALUE;
}

static void store(slot_t *s, uint16_t v, uint32_t stamp)
{
	if (s->value == v)
		return;
	s->value = v;
	s->changed = stamp;
}

void wld_style_set(int slot, const uint8_t *letters, int len)
{
	slot_t *s;
	uint16_t v;
	int i;

	if (slot < 0 || slot >= WLD_STYLE_SLOTS_ALL || len < 0 || len > 255)
		return;
	for (i = 0; i < len; i++)
		if (letters[i] > 25)
			return;
	s = &slots[slot];
	s->letters = letters;
	s->len = (uint8_t)len;
	/* a set (a light switched) shows on the next drawn frame, not the
	 * next tick */
	v = eval(s);
	if (v != s->value)
	{
		epoch++;
		store(s, v, epoch);
	}
}

int wld_style_slot(const wld_layer_t *L, int s)
{
	if (s < 0 || s > 63)
		return -1;
	return s < 32 ? s : 32 + 32 * L->slot + (s - 32);
}

void wld_style_tick(uint64_t now_us)
{
	uint64_t t = now_us / 100000u;
	uint32_t next = epoch + 1;
	int i, any = 0;

	if (t == tick)
		return;
	tick = t;
	for (i = 0; i < WLD_STYLE_SLOTS_ALL; i++)
	{
		slot_t *s = &slots[i];
		uint16_t v;

		if (!s->len)
			continue;
		v = eval(s);
		if (v != s->value)
		{
			store(s, v, next);
			any = 1;
		}
	}
	if (any)
		epoch = next;
}

uint16_t wld_style_value(int slot)
{
	return slots[slot].value;
}

uint32_t wld_style_changed(int slot)
{
	return slots[slot].changed;
}

uint32_t wld_style_epoch(void)
{
	return epoch;
}

/* A REND / MOVR chunk landed with its cooked colours: give its styled
 * meshlets their slots (first landing) and mark them never coloured. */
void wld_style_chunk_landed(wld_layer_t *L, int c)
{
	const w64_rend_t *r = L->ch[c].data;
	wld_chunk_t *ch = &L->ch[c];
	uint32_t i;

	if (!r->nstyled)
		return;
	if (ch->style_base == W64_NONE)
	{
		if ((uint32_t)L->style_used + r->nstyled > L->nstyle_slots)
			sys_error("%s: more than %u styled meshlets", wld_map_name(L->map),
				(unsigned)L->nstyle_slots);
		ch->style_base = L->style_used;
		L->style_used = (uint16_t)(L->style_used + r->nstyled);
	}
	for (i = 0; i < r->nstyled; i++)
		L->style_epoch[ch->style_base + i] = 0;
}

void wld_style_reset(void)
{
	int i;

	for (i = 0; i < WLD_STYLE_SLOTS_ALL; i++)
	{
		slots[i].letters = 0;
		slots[i].len = 0;
		slots[i].value = UNSET_VALUE;
		slots[i].changed = 0;
	}
	epoch = 1;
	tick = ~(uint64_t)0;
}
