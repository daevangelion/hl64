/* SPDX-License-Identifier: MIT */
/*
 * ent.c -- entity slots for M2 (include/hl64/grid.h): one collision view
 * per slot plus serials, touch function ids and touch-rank keys, all in
 * the boot carve's components region. Allocation takes the lowest free
 * slot >= 2; freeing unlinks, zeroes the view and bumps the serial
 * (docs/specs/think-and-time.md; its free-time rule arrives with M3).
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/mem/carve.h"

ent_slots_t	ent_s;

static uint8_t *take(uint8_t **p, uint8_t *end, uint32_t n)
{
	uint8_t *r = *p;

	n = (n + 15u) & ~15u;
	if ((uint32_t)(end - r) < n)
		sys_error("ent: components region too small");
	*p = r + n;
	return r;
}

void ent_init(void)
{
	uint8_t *p = mem_region(MEM_COMPONENTS), *start = p;
	uint8_t *end = p + mem_region_bytes(MEM_COMPONENTS);
	const uint32_t n = HL64_MAX_ENTITIES;

	ent_s.v = (col_ent_t *)take(&p, end, n * sizeof(col_ent_t));
	ent_s.node = (ent_node_t *)take(&p, end, n * sizeof(ent_node_t));
	ent_s.cell = (ent_cell_t *)take(&p, end, ENT_CELLS * sizeof(ent_cell_t));
	ent_s.serial = (uint16_t *)take(&p, end, n * 2);
	ent_s.state = take(&p, end, n);
	ent_s.touch = (uint16_t *)take(&p, end, n * 2);
	ent_s.rank = (uint32_t *)take(&p, end, n * 4);
	ent_s.contents = take(&p, end, n);
	memset(start, 0, (size_t)(p - start));
	ent_grid_init();
#ifdef HL64_TEST
	ent_grid_bench_init();
#endif
	ent_reset();
}

static void clear_slot(int i)
{
	col_ent_t *e = &ent_s.v[i];

	memset(e, 0, sizeof(*e));
	e->index = (uint16_t)i;
	e->owner = COL_NONE;
	e->model = COL_NONE;
	e->studio = COL_NONE;
	ent_s.state[i] = ENT_FREE;
	ent_s.node[i].kind = 0;
	ent_s.node[i].cells = ENT_UNLINKED;
	ent_s.touch[i] = 0;
	ent_s.rank[i] = ENT_RANK_RUNTIME;
	ent_s.contents[i] = 0;
}

void ent_reset(void)
{
	int i;

	for (i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		if (ent_s.contents[i])
			col_contents_remove(&ent_s.v[i]);
		clear_slot(i);
	}
	ent_grid_clear();
	ent_s.seq = 0;
	ent_s.top = 0;
}

col_ent_t *ent_alloc_at(int i)
{
	if (i < 0 || i >= HL64_MAX_ENTITIES || ent_s.state[i] != ENT_FREE)
		return NULL;
	clear_slot(i);
	ent_s.state[i] = ENT_LIVE;
	ent_s.rank[i] = ENT_RANK_RUNTIME | (ent_s.seq++ & 0x7FFFFFFFu);
	if (i >= ent_s.top)
		ent_s.top = (uint16_t)(i + 1);
	return &ent_s.v[i];
}

col_ent_t *ent_alloc(void)
{
	int i;

	for (i = ENT_PLAYER + 1; i < HL64_MAX_ENTITIES; i++)
		if (ent_s.state[i] == ENT_FREE)
			return ent_alloc_at(i);
	return NULL;
}

void ent_free(col_ent_t *e)
{
	int i = e->index;

	if (!ent_live(i))
		return;
	ent_unlink(e);
	clear_slot(i);
	ent_s.serial[i]++;
}

col_ent_t *ent_get(int i)
{
	return ent_live(i) ? &ent_s.v[i] : NULL;
}

ent_h ent_handle(const col_ent_t *e)
{
	return (ent_h)e->index | (ent_h)ent_s.serial[e->index] << 16;
}

col_ent_t *ent_resolve(ent_h h)
{
	int i = (int)(h & 0xFFFFu);

	if (!ent_live(i) || ent_s.serial[i] != (uint16_t)(h >> 16))
		return NULL;
	return &ent_s.v[i];
}

void ent_set_touch(col_ent_t *e, uint16_t fn)
{
	ent_s.touch[e->index] = fn;
}

uint16_t ent_touch_fn(const col_ent_t *e)
{
	return ent_s.touch[e->index];
}

void ent_set_rank(col_ent_t *e, int map_slot, uint16_t rank)
{
	ent_s.rank[e->index] = (uint32_t)map_slot << 16 | rank;
}
