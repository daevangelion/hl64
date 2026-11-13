/* SPDX-License-Identifier: MIT */
/*
 * heap.c -- the think heap and the active sets (docs/architecture.md 6.3,
 * docs/specs/think-and-time.md 3), in the THINK_GRID region.
 *
 * Think heap: 4-ary min-heap of (nextthink f32 bit pattern, slot); every
 * live entity with nextthink > 0 except pushers (their think runs on ltime
 * inside the push step) and the player (step 5) has exactly one entry,
 * keyed by its current nextthink (positive floats order like their bit
 * patterns). Bitsets (one bit per slot): movers (the movetype has motion
 * to do), due (popped this frame), kill (FL_KILLME), live, linked. The
 * pass walks due | movers | kill in ascending slot order.
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/mem/carve.h"

#define HEAP_MAX	512
#define HEAP_NONE	0xFFFF

static struct {
	uint8_t		*base, *cur, *end;
	uint32_t	*key;		/* [HEAP_MAX] f32 bits */
	uint16_t	*idx;		/* [HEAP_MAX] slot */
	uint16_t	*pos;		/* [HL64_MAX_ENTITIES] heap position, HEAP_NONE */
	uint32_t	*movers, *due, *kill, *live, *linked;	/* [ENT_WORDS] */
	int		n;
} H;

void *ent_think_take(uint32_t bytes)
{
	uint8_t *r = H.cur;

	bytes = (bytes + 15u) & ~15u;
	if ((uint32_t)(H.end - r) < bytes)
		sys_error("ent: think/grid region too small");
	H.cur = r + bytes;
	return r;
}

uint32_t ent_sets_region_bytes(void)
{
	return (uint32_t)(H.cur - H.base);
}

void ent_sets_init(void)
{
	H.base = H.cur = mem_region(MEM_THINK_GRID);
	H.end = H.base + mem_region_bytes(MEM_THINK_GRID);
	H.key = ent_think_take(HEAP_MAX * 4);
	H.idx = ent_think_take(HEAP_MAX * 2);
	H.pos = ent_think_take(HL64_MAX_ENTITIES * 2);
	H.movers = ent_think_take(ENT_WORDS * 4);
	H.due = ent_think_take(ENT_WORDS * 4);
	H.kill = ent_think_take(ENT_WORDS * 4);
	H.live = ent_think_take(ENT_WORDS * 4);
	H.linked = ent_think_take(ENT_WORDS * 4);
}

void ent_sets_clear(void)
{
	H.n = 0;
	memset(H.pos, 0xFF, HL64_MAX_ENTITIES * 2);
	memset(H.movers, 0, ENT_WORDS * 4);
	memset(H.due, 0, ENT_WORDS * 4);
	memset(H.kill, 0, ENT_WORDS * 4);
	memset(H.live, 0, ENT_WORDS * 4);
	memset(H.linked, 0, ENT_WORDS * 4);
}

static void bit_put(uint32_t *s, int e, int on)
{
	if (on)
		s[e >> 5] |= 1u << (e & 31);
	else
		s[e >> 5] &= ~(1u << (e & 31));
}

static int bit_get(const uint32_t *s, int e)
{
	return (s[e >> 5] >> (e & 31)) & 1;
}

static uint32_t f32_bits(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

static float bits_f32(uint32_t u)
{
	float f;

	memcpy(&f, &u, 4);
	return f;
}

/* ---- heap ------------------------------------------------------------------- */

static void place(int p, uint32_t key, uint16_t idx)
{
	H.key[p] = key;
	H.idx[p] = idx;
	H.pos[idx] = (uint16_t)p;
}

static void sift_up(int p, uint32_t key, uint16_t idx)
{
	while (p > 0)
	{
		int parent = (p - 1) >> 2;

		if (H.key[parent] <= key)
			break;
		place(p, H.key[parent], H.idx[parent]);
		p = parent;
	}
	place(p, key, idx);
}

static void sift_down(int p, uint32_t key, uint16_t idx)
{
	for (;;)
	{
		int c = 4 * p + 1, best = -1, k;
		uint32_t bk = key;

		for (k = 0; k < 4 && c + k < H.n; k++)
			if (H.key[c + k] < bk)
			{
				bk = H.key[c + k];
				best = c + k;
			}
		if (best < 0)
			break;
		place(p, H.key[best], H.idx[best]);
		p = best;
	}
	place(p, key, idx);
}

static void heap_del(int e)
{
	int p = H.pos[e];
	uint32_t key;
	uint16_t idx;

	if (p == HEAP_NONE)
		return;
	H.pos[e] = HEAP_NONE;
	if (--H.n == p)
		return;
	key = H.key[H.n];
	idx = H.idx[H.n];
	if (p > 0 && H.key[(p - 1) >> 2] > key)
		sift_up(p, key, idx);
	else
		sift_down(p, key, idx);
}

static void heap_set(int e, uint32_t key)
{
	int p = H.pos[e];

	if (p == HEAP_NONE)
	{
		if (H.n == HEAP_MAX)
			sys_error("ent: think heap full (%d)", HEAP_MAX);
		sift_up(H.n++, key, (uint16_t)e);
		return;
	}
	if (H.key[p] == key)
		return;
	if (key < H.key[p])
		sift_up(p, key, (uint16_t)e);
	else
		sift_down(p, key, (uint16_t)e);
}

int ent_heap_count(void)
{
	return H.n;
}

void ent_heap_pop_due(double time, double dt)
{
	const double limit = time + dt;

	while (H.n > 0 && (double)bits_f32(H.key[0]) <= limit)
	{
		int e = H.idx[0];

		heap_del(e);
		bit_put(H.due, e, 1);
	}
}

/* ---- sets -------------------------------------------------------------------- */

static int any3(const float v[3])
{
	return v[0] != 0 || v[1] != 0 || v[2] != 0;
}

/* A ground entity that keeps a resting entity busy: a conveyor sets its
 * base velocity every frame, a box resting on a monster or a client lands
 * again every frame [probe:measured P-toss], and a freed ground leaves it
 * to fall (until its step clears groundentity). The world (slot 0) is
 * still ground whether or not its slot is allocated. */
static int busy_ground(int e)
{
	int g = ent_c.phys[e].groundentity;

	if (g == ENT_NONE || g == ENT_WORLD)
		return 0;
	return !ent_is_live(g) || (ent_c.cv[g].flags & (FL_CONVEYOR | FL_MONSTER | FL_CLIENT));
}

int ent_needs_step(int e)
{
	const col_ent_t *v = &ent_c.cv[e];
	const ent_mot_t *m = &ent_c.mot[e];
	int ground = (v->flags & FL_ONGROUND) != 0;
	int moving = any3(m->velocity) || any3(m->avelocity);
	int base = any3(m->basevelocity) || (v->flags & FL_BASEVELOCITY);

	if (e == ENT_PLAYER)
		return 0;
	switch (v->movetype)
	{
	case MOVETYPE_NOCLIP:
		return moving;
	case MOVETYPE_FOLLOW:
		return ent_c.phys[e].aiment != ENT_NONE;
	case MOVETYPE_PUSH:
		/* idle pushers do nothing (movetime 0 without a nextthink) */
		return moving || ent_c.tm[e].nextthink > 0;
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_PUSHSTEP:
		/* at rest = on ground without velocity (physics-movetypes.md 3),
		 * no base velocity to fold, on a ground that stays still */
		return !ground || moving || base || busy_ground(e);
	case MOVETYPE_STEP:
		return (!ground && !(v->flags & (FL_FLY | FL_SWIM))) || moving || base
			|| busy_ground(e);
	default:
		return 0;
	}
}

/* Entities resting on e (being freed) step once more: their ground is
 * gone. */
void ent_sets_ground_lost(int e)
{
	int i;

	for (i = 0; i < ent_s.top; i++)
		if (ent_c.phys[i].groundentity == e && ent_is_live(i) && i != e)
			bit_put(H.movers, i, 1);
}

void ent_sets_update(int e)
{
	const ent_tm_t *tm = &ent_c.tm[e];

	if (!ent_is_live(e))
		return;
	bit_put(H.live, e, 1);
	if (tm->nextthink > 0 && ent_c.cv[e].movetype != MOVETYPE_PUSH && e != ENT_PLAYER)
	{
		heap_set(e, f32_bits(tm->nextthink));
		/* made due during the pass ahead of the cursor: this frame */
		if (ent_pass.active && e > ent_pass.cursor
			&& (double)tm->nextthink <= ent_pass.time + ent_pass.dt)
			bit_put(H.due, e, 1);
	}
	else
		heap_del(e);
	bit_put(H.movers, e, ent_needs_step(e));
	bit_put(H.kill, e, (ent_c.cv[e].flags & FL_KILLME) != 0);
}

void ent_sets_remove(int e)
{
	heap_del(e);
	bit_put(H.movers, e, 0);
	bit_put(H.due, e, 0);
	bit_put(H.kill, e, 0);
	bit_put(H.live, e, 0);
	bit_put(H.linked, e, 0);
}

void ent_sets_mark_due(int e)
{
	bit_put(H.due, e, 1);
}

void ent_sets_clear_due(int e)
{
	bit_put(H.due, e, 0);
}

void ent_sets_set_linked(int e, int on)
{
	bit_put(H.linked, e, on);
}

int ent_sets_linked(int e)
{
	return bit_get(H.linked, e);
}

int ent_sets_next(int from, int all)
{
	int w;

	if (from >= HL64_MAX_ENTITIES)
		return -1;
	for (w = from >> 5; w < ENT_WORDS; w++)
	{
		uint32_t b = H.due[w] | H.movers[w] | H.kill[w] | (all ? H.live[w] : 0);

		if (w == from >> 5)
			b &= ~0u << (from & 31);
		if (b)
		{
			int i = w << 5;

			while (!(b & 1))
			{
				b >>= 1;
				i++;
			}
			return i < HL64_MAX_ENTITIES ? i : -1;
		}
	}
	return -1;
}

int ent_movers_count(void)
{
	int w, n = 0;

	for (w = 0; w < ENT_WORDS; w++)
	{
		uint32_t b = H.movers[w];

		while (b)
		{
			b &= b - 1;
			n++;
		}
	}
	return n;
}

const char *ent_sets_check(void)
{
	int p, e;

	for (p = 1; p < H.n; p++)
		if (H.key[(p - 1) >> 2] > H.key[p])
			return "heap order";
	for (p = 0; p < H.n; p++)
		if (H.pos[H.idx[p]] != p)
			return "heap position";
	for (e = 0; e < HL64_MAX_ENTITIES; e++)
	{
		int live = ent_is_live(e);
		int want = live && ent_c.tm[e].nextthink > 0
			&& ent_c.cv[e].movetype != MOVETYPE_PUSH && e != ENT_PLAYER;

		if (bit_get(H.live, e) != live)
			return "live bit";
		if ((H.pos[e] != HEAP_NONE) != want)
			return "heap membership";
		if (want && H.key[H.pos[e]] != f32_bits(ent_c.tm[e].nextthink))
			return "heap key";
		/* an extra bit (ground lost) only costs one step; a missing one
		 * skips motion */
		if (live && ent_needs_step(e) && !bit_get(H.movers, e))
			return "movers bit";
		if (!live && bit_get(H.movers, e))
			return "movers bit of a free slot";
		if (bit_get(H.kill, e) != (live && (ent_c.cv[e].flags & FL_KILLME) != 0))
			return "kill bit";
		if (!live && (bit_get(H.due, e) || bit_get(H.linked, e)))
			return "free slot in a set";
	}
	return 0;
}
