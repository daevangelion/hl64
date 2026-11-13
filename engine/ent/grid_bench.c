/* SPDX-License-Identifier: MIT */
/*
 * grid_bench.c -- test builds: the `bench 1` debug command measures the
 * spatial grid (grid.c) on the running platform, for the ares cost run
 * (docs/specs/area-links-and-touch.md 2: insert and query cost).
 *
 * A fixed pseudo-random population in a 6144 x 4096 x 1024 u volume (the
 * size of an Uplink map): 400 entities, 70 % monster/item boxes (32 x 32 x
 * 72), 30 % brushes whose half extent is drawn per axis (70 % 8-64 u,
 * 25 % 64-256 u, 5 % 256-768 u: walls, doors and triggers are long on one
 * axis; the Uplink maps have 7-35 brush entities wider than two 256 u
 * cells); 25 % of all are triggers. Timed per operation, including the
 * random draws that make it (~0.5 us each on the cart): first links (with
 * the slot allocation), relinks after a move of up to 8 u (unlink +
 * insert), solid move queries (a player box swept by up to 64 u) and
 * trigger queries (the touch pass's box). Every slot is freed afterwards.
 */
#ifdef HL64_TEST

#include <string.h>

#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"

#define BENCH_ENTS	400
#define BENCH_RELINKS	4
#define BENCH_QUERIES	2000

static uint32_t seed;

static float frand(float lo, float hi)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return lo + (hi - lo) * (float)(seed >> 8) * (1.0f / 16777216.0f);
}

static unsigned per_op_ns(uint64_t us, uint32_t ops)
{
	return (unsigned)(us * 1000u / ops);
}

static int cmd_bench(const dbg_cmd_t *c)
{
	static uint16_t out[HL64_MAX_ENTITIES];
	ent_grid_stats_t st0, st1;
	uint64_t t, t_link, t_relink, t_solid, t_trig;
	uint32_t found_s = 0, found_t = 0;
	int i, k, r;

	if (c->arg[0].u != 1)
		return 1;
	ent_reset();
	seed = 0x9E3779B9u;
	t = sys_time_us();
	for (i = 0; i < BENCH_ENTS; i++)
	{
		col_ent_t *e = ent_alloc();
		float p = frand(0, 1), h[3];

		for (k = 0; k < 3; k++)
		{
			float r = frand(0, 1);

			h[k] = p < 0.7f ? (k == 2 ? 36.0f : 16.0f)
				: r < 0.7f ? frand(8, 64) : r < 0.95f ? frand(64, 256) : frand(256, 768);
		}
		for (k = 0; k < 3; k++)
		{
			e->origin[k] = frand(-3072, 3072) * (k == 0 ? 1.0f : k == 1 ? 0.6667f : 0.1667f);
			e->mins[k] = -h[k];
			e->maxs[k] = h[k];
		}
		e->solid = frand(0, 1) < 0.25f ? COL_SOLID_TRIGGER
			: p < 0.7f ? COL_SOLID_SLIDEBOX : COL_SOLID_BSP;
		ent_link(e);
	}
	t_link = sys_time_us() - t;
	t = sys_time_us();
	for (r = 0; r < BENCH_RELINKS; r++)
		for (i = 2; i < BENCH_ENTS + 2; i++)
		{
			col_ent_t *e = ent_get(i);

			e->origin[0] += frand(-8, 8);
			e->origin[1] += frand(-8, 8);
			ent_link(e);
		}
	t_relink = sys_time_us() - t;
	ent_grid_stats(&st0);
	t = sys_time_us();
	for (i = 0; i < BENCH_QUERIES; i++)
	{
		float mn[3], mx[3], d[3];

		for (k = 0; k < 3; k++)
		{
			float o = frand(-3072, 3072) * (k == 0 ? 1.0f : k == 1 ? 0.6667f : 0.1667f);

			d[k] = frand(-64, 64);
			mn[k] = o + (d[k] < 0 ? d[k] : 0) - (k == 2 ? 37 : 17);
			mx[k] = o + (d[k] > 0 ? d[k] : 0) + (k == 2 ? 37 : 17);
		}
		found_s += (uint32_t)ent_query(mn, mx, ENT_Q_SOLID, out, HL64_MAX_ENTITIES);
	}
	t_solid = sys_time_us() - t;
	ent_grid_stats(&st1);
	t = sys_time_us();
	for (i = 0; i < BENCH_QUERIES; i++)
	{
		float mn[3], mx[3];

		for (k = 0; k < 3; k++)
		{
			float o = frand(-3072, 3072) * (k == 0 ? 1.0f : k == 1 ? 0.6667f : 0.1667f);

			mn[k] = o - (k == 2 ? 37 : 17);
			mx[k] = o + (k == 2 ? 37 : 17);
		}
		found_t += (uint32_t)ent_query(mn, mx, ENT_Q_TRIGGER, out, HL64_MAX_ENTITIES);
	}
	t_trig = sys_time_us() - t;
	log_printf("bench grid: ents=%d link_ns=%u relink_ns=%u query_ns=%u trigger_query_ns=%u "
		"cells/query=%lu.%02lu cand/query=%lu.%02lu found/query=%lu.%02lu "
		"trig_found/query=%lu.%02lu peak_bucket=%u scans=%lu dry=%lu\n",
		BENCH_ENTS, per_op_ns(t_link, BENCH_ENTS),
		per_op_ns(t_relink, BENCH_ENTS * BENCH_RELINKS),
		per_op_ns(t_solid, BENCH_QUERIES), per_op_ns(t_trig, BENCH_QUERIES),
		(unsigned long)((st1.cells - st0.cells) / BENCH_QUERIES),
		(unsigned long)((st1.cells - st0.cells) % BENCH_QUERIES * 100 / BENCH_QUERIES),
		(unsigned long)((st1.candidates - st0.candidates) / BENCH_QUERIES),
		(unsigned long)((st1.candidates - st0.candidates) % BENCH_QUERIES * 100 / BENCH_QUERIES),
		(unsigned long)(found_s / BENCH_QUERIES),
		(unsigned long)(found_s % BENCH_QUERIES * 100 / BENCH_QUERIES),
		(unsigned long)(found_t / BENCH_QUERIES),
		(unsigned long)(found_t % BENCH_QUERIES * 100 / BENCH_QUERIES),
		st1.peak_bucket, (unsigned long)(st1.scans - st0.scans), (unsigned long)st1.dry);
	ent_reset();
	return 0;
}

void ent_grid_bench_init(void)
{
	dbg_handler_set(DBG_BENCH, cmd_bench);
}

#endif
