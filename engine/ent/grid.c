/* SPDX-License-Identifier: MIT */
/*
 * grid.c -- linked boxes and the spatial grid
 * (docs/specs/area-links-and-touch.md 1, 2).
 *
 * Mechanism: one hashed grid over global coordinates instead of a grid
 * per open layer, so its size does not depend on the maps (t0a0 alone
 * would need 19 x 18 x 8 cells) and entities outside every layer need no
 * special case. An entity is a member of every cell its linked box
 * touches, at the first level whose cells it spans at most two of on every
 * axis -- level 0 has 256 u cells (the COLL grid's), levels 1 and 2 have
 * 1024 u and 4096 u cells -- else in the "huge" list that every query
 * walks. A query walks the
 * cells its own box touches on each non-empty level: two boxes that meet
 * share a cell, so every entity whose box meets the query box is reached,
 * and a per-query stamp on the entity reports it once.
 *
 * Positions map to cells through one float-to-int conversion per
 * coordinate (coord()); exact floors are not needed, since any
 * non-decreasing map keeps "boxes that meet share a cell". Query boxes
 * that touch more than GRID_SCAN_CELLS cells on a level scan the slots.
 * Memberships beyond an entity's first come from a shared pool; when it
 * runs dry the entity goes to the huge list (slower, still exact).
 *
 * Cart costs (test/m2/grid_bench.sh): a bitmap of non-empty buckets
 * (128 B, stays cached) skips empty cells without touching the heads, and
 * the node (ent_node_t) keeps the kind, stamp and a rounded box in one
 * cache line, so a rejected candidate costs one miss.
 */
#include <math.h>
#include <string.h>

#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/mem/carve.h"

#define GRID_BUCKETS	1024		/* power of two */
#define GRID_HUGE	GRID_BUCKETS	/* extra head: the huge list */
#define GRID_LEVELS	3
#define GRID_HUGE_LEVEL	GRID_LEVELS
static const uint8_t grid_shift[GRID_LEVELS] = { 8, 10, 12 };	/* 256, 1024, 4096 u */
#define GRID_SCAN_CELLS	64
#define GRID_LIM	16777216.0f	/* coordinates are clamped to +-2^24 */
#define GRID_BIAS	16777216	/* makes clamped coordinates non-negative */
#define GRID_OCC_WORDS	((GRID_BUCKETS + 1 + 31) / 32)
/* Linear in x, so a probe row costs one add; the odd multipliers spread
 * neighbouring rows, slabs and levels over the table. */
#define GRID_HY		37u
#define GRID_HZ		293u
#define GRID_HL		509u

static struct {
	uint16_t		*head;		/* [GRID_BUCKETS + 1] first ent_cell_t */
	uint32_t		occ[GRID_OCC_WORDS];	/* non-empty buckets */
	uint16_t		freecell;	/* pool free list (ent_cell_t.next) */
	uint8_t			qstamp;
	uint16_t		nlevel[GRID_LEVELS + 1];
	ent_grid_stats_t	st;
} g;

void ent_grid_init(void)
{
	if (mem_region_bytes(MEM_THINK_GRID) < 2u * (GRID_BUCKETS + 1))
		sys_error("grid: think/grid region too small");
	g.head = mem_region(MEM_THINK_GRID);
}

void ent_grid_clear(void)
{
	int i;

	memset(g.head, 0xFF, 2u * (GRID_BUCKETS + 1));
	memset(g.occ, 0, sizeof(g.occ));
	memset(g.nlevel, 0, sizeof(g.nlevel));
	memset(&g.st, 0, sizeof(g.st));
	g.freecell = ENT_UNLINKED;
	for (i = ENT_CELLS - 1; i >= HL64_MAX_ENTITIES; i--)
	{
		ent_s.cell[i].next = g.freecell;
		g.freecell = (uint16_t)i;
	}
	for (i = 0; i < HL64_MAX_ENTITIES; i++)
		ent_s.node[i].stamp = 0;
	g.qstamp = 0;
}

/* Integer coordinate: x clamped to +-2^24 (NaN to -2^24), truncated, so
 * |x - coord(x)| < 1. */
static int32_t coord(float x)
{
	if (!(x >= -GRID_LIM))
		x = -GRID_LIM;
	if (x > GRID_LIM)
		x = GRID_LIM;
	return (int32_t)x;
}

static int16_t s16_clamp(int32_t i)
{
	return (int16_t)(i < -32768 ? -32768 : i > 32767 ? 32767 : i);
}

static uint32_t row_of(uint32_t y, uint32_t z, int level)
{
	return y * GRID_HY + z * GRID_HZ + (uint32_t)level * GRID_HL;
}

/* Spec 1: rotated SOLID_BSP -> cube of the largest per-axis extent;
 * otherwise origin + mins / maxs; then 1 u on every axis. */
static void linked_box(col_ent_t *e)
{
	int i;

	if (e->solid == COL_SOLID_BSP
		&& (e->angles[0] != 0.0f || e->angles[1] != 0.0f || e->angles[2] != 0.0f))
	{
		float m = 0.0f;

		for (i = 0; i < 3; i++)
		{
			float a = fabsf(e->mins[i]), b = fabsf(e->maxs[i]);

			if (a > m)
				m = a;
			if (b > m)
				m = b;
		}
		for (i = 0; i < 3; i++)
		{
			e->absmin[i] = e->origin[i] - m;
			e->absmax[i] = e->origin[i] + m;
		}
	}
	else
		for (i = 0; i < 3; i++)
		{
			e->absmin[i] = e->origin[i] + e->mins[i];
			e->absmax[i] = e->origin[i] + e->maxs[i];
		}
	for (i = 0; i < 3; i++)
	{
		e->absmin[i] -= 1.0f;
		e->absmax[i] += 1.0f;
	}
}

static int kind_of(const col_ent_t *e)
{
	switch (e->solid)
	{
	case COL_SOLID_TRIGGER:
		return ENT_Q_TRIGGER;
	case COL_SOLID_BBOX:
	case COL_SOLID_SLIDEBOX:
	case COL_SOLID_BSP:
		return ENT_Q_SOLID;
	case COL_SOLID_NOT:
		return e->model != COL_NONE && e->skin < COL_EMPTY ? ENT_Q_CONTENTS : 0;
	}
	return 0;
}

/* ---- membership --------------------------------------------------------------- */

static void cell_add(int i, uint16_t c, uint32_t b)
{
	ent_cell_t *m = &ent_s.cell[c];

	m->ent = (uint16_t)i;
	m->bucket = (uint16_t)b;
	m->next = g.head[b];
	g.head[b] = c;
	g.occ[b >> 5] |= 1u << (b & 31);
	m->enext = ent_s.node[i].cells;
	ent_s.node[i].cells = c;
}

static void grid_remove(int i)
{
	ent_node_t *n = &ent_s.node[i];
	uint16_t c = n->cells;

	if (!n->kind)
		return;
	while (c != ENT_UNLINKED)
	{
		ent_cell_t *m = &ent_s.cell[c];
		uint16_t b = m->bucket, nc = m->enext, *p;

		for (p = &g.head[b]; *p != c && *p != ENT_UNLINKED; p = &ent_s.cell[*p].next)
			;
		if (*p == c)
			*p = m->next;
		if (g.head[b] == ENT_UNLINKED)
			g.occ[b >> 5] &= ~(1u << (b & 31));
		if (c >= HL64_MAX_ENTITIES)
		{
			m->next = g.freecell;
			g.freecell = c;
		}
		c = nc;
	}
	g.nlevel[n->kind >> 4]--;
	g.st.linked--;
	n->cells = ENT_UNLINKED;
	n->kind = 0;
}

void ent_unlink(col_ent_t *e)
{
	int i = e->index;

	grid_remove(i);
	if (ent_s.contents[i])
	{
		col_contents_remove(e);
		ent_s.contents[i] = 0;
	}
}

int ent_linked(const col_ent_t *e)
{
	return ent_s.node[e->index].kind != 0;
}

void ent_link(col_ent_t *e)
{
	int i = e->index, k, a, level;
	int32_t lo[3], hi[3];
	ent_node_t *n = &ent_s.node[i];

	grid_remove(i);
	linked_box(e);
	k = kind_of(e);
	if (k == ENT_Q_CONTENTS && !ent_s.contents[i])
	{
		col_contents_add(e);
		ent_s.contents[i] = 1;
	}
	else if (k != ENT_Q_CONTENTS && ent_s.contents[i])
	{
		col_contents_remove(e);
		ent_s.contents[i] = 0;
	}
	if (!k)
		return;
	g.st.links++;
	for (a = 0; a < 3; a++)
	{
		lo[a] = coord(e->absmin[a]);
		hi[a] = coord(e->absmax[a]);
		n->lo[a] = s16_clamp(lo[a] - 1);
		n->hi[a] = s16_clamp(hi[a] + 1);
		lo[a] += GRID_BIAS;
		hi[a] += GRID_BIAS;
	}
	for (level = 0; level < GRID_LEVELS; level++)
	{
		int s = grid_shift[level];
		uint32_t x0 = (uint32_t)lo[0] >> s, x1 = (uint32_t)hi[0] >> s;
		uint32_t y0 = (uint32_t)lo[1] >> s, y1 = (uint32_t)hi[1] >> s;
		uint32_t z0 = (uint32_t)lo[2] >> s, z1 = (uint32_t)hi[2] >> s, x, y, z;
		uint16_t c = (uint16_t)i;

		if (x1 - x0 > 1 || y1 - y0 > 1 || z1 - z0 > 1)
			continue;
		for (z = z0; z <= z1; z++)
			for (y = y0; y <= y1; y++)
			{
				uint32_t r = row_of(y, z, level);

				for (x = x0; x <= x1; x++)
				{
					if (c == ENT_UNLINKED)
						goto dry;
					cell_add(i, c, (x + r) & (GRID_BUCKETS - 1));
					c = g.freecell;
					if (c != ENT_UNLINKED)
						g.freecell = ent_s.cell[c].next;
				}
			}
		/* the pool entry taken for a next membership that did not come */
		if (c != ENT_UNLINKED && c != i)
		{
			ent_s.cell[c].next = g.freecell;
			g.freecell = c;
		}
		break;
	}
	if (level == GRID_LEVELS)
		cell_add(i, (uint16_t)i, GRID_HUGE);
	n->kind = (uint8_t)(k | level << 4);
	g.nlevel[level]++;
	g.st.linked++;
	return;
dry:
	/* pool exhausted: undo, then the huge list through the own entry */
	n->kind = (uint8_t)(k | level << 4);
	g.nlevel[level]++;
	g.st.linked++;
	grid_remove(i);
	g.st.dry++;
	cell_add(i, (uint16_t)i, GRID_HUGE);
	n->kind = (uint8_t)(k | GRID_HUGE_LEVEL << 4);
	g.nlevel[GRID_HUGE_LEVEL]++;
	g.st.linked++;
}

/* ---- queries ---------------------------------------------------------------- */

typedef struct {
	const float	*mins, *maxs;
	int16_t		lo[3], hi[3];	/* the query box rounded outwards */
	int		mask, max, n;
	uint16_t	*out;
} query_t;

static void visit(query_t *q, int i)
{
	ent_node_t *n = &ent_s.node[i];
	const col_ent_t *e;

	if (n->stamp == g.qstamp)
		return;
	n->stamp = g.qstamp;
	g.st.candidates++;
	if (!(n->kind & q->mask)
		|| n->lo[0] > q->hi[0] || n->hi[0] < q->lo[0]
		|| n->lo[1] > q->hi[1] || n->hi[1] < q->lo[1]
		|| n->lo[2] > q->hi[2] || n->hi[2] < q->lo[2])
		return;
	/* exact; positive form: a NaN box meets nothing */
	e = &ent_s.v[i];
	if (!(e->absmin[0] <= q->maxs[0] && e->absmax[0] >= q->mins[0]
		&& e->absmin[1] <= q->maxs[1] && e->absmax[1] >= q->mins[1]
		&& e->absmin[2] <= q->maxs[2] && e->absmax[2] >= q->mins[2]))
		return;
	if (q->n < q->max)
		q->out[q->n++] = (uint16_t)i;
	else
		g.st.overflow++;
}

static void walk(query_t *q, uint32_t b)
{
	uint16_t c;

	g.st.cells++;
	for (c = g.head[b]; c != ENT_UNLINKED; c = ent_s.cell[c].next)
		visit(q, ent_s.cell[c].ent);
}

static uint32_t sort_key(int i, int by_rank)
{
	return by_rank ? ent_s.rank[i] : (uint32_t)i;
}

int ent_query(const float mins[3], const float maxs[3], int mask, uint16_t *out, int max)
{
	query_t q;
	int32_t lo[3], hi[3];
	int level, a, i, j, by_rank = mask == ENT_Q_TRIGGER;

	g.st.queries++;
	for (a = 0; a < 3; a++)
		if (!(mins[a] <= maxs[a]))	/* inverted or NaN box: empty */
			return 0;
	q.mins = mins;
	q.maxs = maxs;
	for (a = 0; a < 3; a++)
	{
		lo[a] = coord(mins[a]);
		hi[a] = coord(maxs[a]);
		q.lo[a] = s16_clamp(lo[a] - 1);
		q.hi[a] = s16_clamp(hi[a] + 1);
		lo[a] += GRID_BIAS;
		hi[a] += GRID_BIAS;
	}
	q.mask = mask;
	q.max = max;
	q.n = 0;
	q.out = out;
	if (++g.qstamp == 0)
	{
		for (i = 0; i < HL64_MAX_ENTITIES; i++)
			ent_s.node[i].stamp = 0;
		g.qstamp = 1;
	}
	for (level = 0; level < GRID_LEVELS; level++)
	{
		int s = grid_shift[level];
		uint32_t x0 = (uint32_t)lo[0] >> s, x1 = (uint32_t)hi[0] >> s;
		uint32_t y0 = (uint32_t)lo[1] >> s, y1 = (uint32_t)hi[1] >> s;
		uint32_t z0 = (uint32_t)lo[2] >> s, z1 = (uint32_t)hi[2] >> s, x, y, z;

		if (!g.nlevel[level])
			continue;
		if (x1 - x0 >= GRID_SCAN_CELLS || y1 - y0 >= GRID_SCAN_CELLS || z1 - z0 >= GRID_SCAN_CELLS
			|| (x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > GRID_SCAN_CELLS)
		{
			/* a box this large: every slot once */
			g.st.scans++;
			for (i = 0; i < ent_s.top; i++)
				if (ent_s.node[i].kind)
					visit(&q, i);
			goto sort;
		}
		for (z = z0; z <= z1; z++)
			for (y = y0; y <= y1; y++)
			{
				uint32_t r = row_of(y, z, level);

				for (x = x0; x <= x1; x++)
				{
					uint32_t b = (x + r) & (GRID_BUCKETS - 1);

					if (g.occ[b >> 5] & 1u << (b & 31))
						walk(&q, b);
				}
			}
	}
	if (g.nlevel[GRID_HUGE_LEVEL])
		walk(&q, GRID_HUGE);
sort:
	/* insertion sort: a handful of candidates */
	for (i = 1; i < q.n; i++)
	{
		uint16_t v = out[i];
		uint32_t k = sort_key(v, by_rank);

		for (j = i; j > 0 && sort_key(out[j - 1], by_rank) > k; j--)
			out[j] = out[j - 1];
		out[j] = v;
	}
	return q.n;
}

int col_ents_box(const float mins[3], const float maxs[3], const col_ent_t **out, int max)
{
	static uint16_t idx[HL64_MAX_ENTITIES];
	int n, i;

	if (max > HL64_MAX_ENTITIES)
		max = HL64_MAX_ENTITIES;
	n = ent_query(mins, maxs, ENT_Q_SOLID | ENT_Q_TRIGGER, idx, max);
	for (i = 0; i < n; i++)
		out[i] = &ent_s.v[idx[i]];
	return n;
}

/* Counters, plus the longest bucket now (walks every bucket). */
void ent_grid_stats(ent_grid_stats_t *s)
{
	uint32_t b;
	uint16_t c, len;

	*s = g.st;
	s->peak_bucket = 0;
	for (b = 0; b <= GRID_HUGE; b++)
	{
		for (len = 0, c = g.head[b]; c != ENT_UNLINKED; c = ent_s.cell[c].next)
			len++;
		if (len > s->peak_bucket)
			s->peak_bucket = len;
	}
}
