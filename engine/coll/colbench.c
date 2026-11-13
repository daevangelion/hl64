/* SPDX-License-Identifier: MIT */
/*
 * colbench.c -- test builds: the `colbench MAP` debug command measures the
 * per-trace cost of engine/coll on the running platform (the ares PROFILE
 * run of test/coll/bench.sh). It opens MAP and runs every segment of the
 * cart file coll/bench.bin (test/coll/bench.py: the GoldSrc probe's
 * T-segments of that map, map-local) as world traces with its hull's box,
 * twice: cold (COLL chunks read on demand, counted as synchronous loads)
 * and warm; then the point contents of every start point. Per hull it logs
 * the count, CPU time, nodes visited and an FNV-1a hash of the results, so
 * the cart's results can be compared with the host's bit for bit.
 * Argument: map index | (r + 1) << 16 also logs 32 results from record r
 * of the cold pass and the points pass (bit patterns) and the points
 * pass's running hash per file chunk, to find a host / cart difference.
 *
 * coll/bench.bin, big-endian: 'CBN1', u32 count, then per segment u32 hull,
 * f32 start[3], f32 end[3].
 */
#include "hl64/coll.h"

#ifdef HL64_TEST

#include <string.h>

#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/world/wld.h"

#define BENCH_MAGIC	0x43424E31u	/* 'CBN1' */
#define BENCH_REC	28
#define BENCH_CHUNK	(BENCH_REC * 144)

static uint8_t buf[BENCH_CHUNK] __attribute__((aligned(16)));

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float bef(const uint8_t *p)
{
	uint32_t u = be32(p);
	float f;

	memcpy(&f, &u, 4);
	return f;
}

/* FNV-1a over 32-bit values, most significant byte first (the same on a
 * big-endian cart and a little-endian host) */
static uint32_t fnv(uint32_t h, uint32_t v)
{
	int k;

	for (k = 24; k >= 0; k -= 8)
		h = (h ^ ((v >> k) & 0xFF)) * 16777619u;
	return h;
}

static uint32_t fnv_trace(uint32_t h, const col_trace_t *t)
{
	uint32_t w[8];
	int k;

	memcpy(w, &t->frac, 4);
	memcpy(w + 1, t->end, 12);
	memcpy(w + 4, t->normal, 12);
	memcpy(w + 7, &t->dist, 4);
	for (k = 0; k < 8; k++)
		h = fnv(h, w[k]);
	return fnv(h, (uint32_t)t->allsolid << 24 | (uint32_t)t->startsolid << 16
		| (uint32_t)t->inopen << 8 | t->inwater) ^ (uint32_t)(uint16_t)t->ent;
}

static int cmd_colbench(const dbg_cmd_t *c)
{
	uint32_t rom, size, count, done, i, pass, dump = c->arg[0].u >> 16, map = c->arg[0].u & 0xFFFF;
	wld_layer_t *L;

	if ((int)map >= wld_map_count())
		return 1;
	rom = sys_cart_addr("coll/bench.bin", &size);
	if (!rom || size < 8 || !sys_cart_read(buf, sizeof(buf), rom, size, 0, 8)
		|| be32(buf) != BENCH_MAGIC)
	{
		log_printf("colbench: no coll/bench.bin\n");
		return 1;
	}
	count = be32(buf + 4);
	if ((uint64_t)count * BENCH_REC + 8 > size)
		return 1;
	L = wld_open((int)map);
	for (pass = 0; pass < 3; pass++)
	{
		uint64_t us[COL_HULLS + 1] = { 0 };
		uint32_t n[COL_HULLS + 1] = { 0 }, nodes[COL_HULLS + 1] = { 0 };
		uint32_t hash[COL_HULLS + 1], sync = 0, sync_us = 0;
		col_stats_t cs;

		for (i = 0; i <= COL_HULLS; i++)
			hash[i] = 2166136261u;
		col_stats(&cs);
		for (done = 0; done < count; )
		{
			uint32_t k, m = count - done < BENCH_CHUNK / BENCH_REC ? count - done
				: BENCH_CHUNK / BENCH_REC;

			if (!sys_cart_read(buf, sizeof(buf), rom, size, 8 + done * BENCH_REC,
				m * BENCH_REC))
				return 1;
			for (k = 0; k < m; k++)
			{
				const uint8_t *r = buf + k * BENCH_REC;
				uint32_t hull = be32(r);
				float s[3], e[3];
				col_trace_t t;
				uint64_t t0;
				int j;

				if (hull >= COL_HULLS)
					return 1;
				for (j = 0; j < 3; j++)
				{
					s[j] = bef(r + 4 + 4 * j) + L->gofs[j];
					e[j] = bef(r + 16 + 4 * j) + L->gofs[j];
				}
				if (pass < 2)
				{
					t0 = sys_time_us();
					col_trace_world(s, col_hull_mins[hull], col_hull_maxs[hull], e, &t);
					us[hull] += sys_time_us() - t0;
					col_stats(&cs);
					nodes[hull] += cs.nodes;
					sync += cs.sync_loads;
					sync_us += cs.sync_us;
					n[hull]++;
					hash[hull] = fnv_trace(hash[hull], &t);
					if (pass == 0 && dump && done + k + 1 >= dump && done + k + 1 < dump + 32)
					{
						uint32_t w[8];

						memcpy(w, &t.frac, 4);
						memcpy(w + 1, t.end, 12);
						memcpy(w + 4, t.normal, 12);
						memcpy(w + 7, &t.dist, 4);
						log_printf("colbench r %lu: %08lx %08lx %08lx %08lx %08lx %08lx %08lx "
							"%08lx %d%d%d%d %d\n", (unsigned long)(done + k),
							(unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2],
							(unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5],
							(unsigned long)w[6], (unsigned long)w[7], t.allsolid,
							t.startsolid, t.inopen, t.inwater, t.ent);
					}
				}
				else
				{
					int ct;

					t0 = sys_time_us();
					ct = col_contents(s);
					us[COL_HULLS] += sys_time_us() - t0;
					col_stats(&cs);
					nodes[COL_HULLS] += cs.nodes;
					n[COL_HULLS]++;
					hash[COL_HULLS] = fnv(hash[COL_HULLS], (uint32_t)ct);
					if (dump && done + k + 1 >= dump && done + k + 1 < dump + 32)
						log_printf("colbench p %lu: %d\n", (unsigned long)(done + k), ct);
				}
			}
			done += m;
			if (dump && pass == 2)
				log_printf("colbench chunk %lu: %08lx\n", (unsigned long)done,
					(unsigned long)hash[COL_HULLS]);
		}
		for (i = 0; i <= COL_HULLS; i++)
			if (n[i])
				log_printf("colbench %s %s%lu n=%lu us=%lu ns_per=%lu nodes_per=%lu.%lu "
					"hash=%08lx\n", pass == 0 ? "cold" : pass == 1 ? "warm" : "points",
					i < COL_HULLS ? "hull=" : "contents=", (unsigned long)(i < COL_HULLS ? i : 0),
					(unsigned long)n[i], (unsigned long)us[i],
					(unsigned long)(us[i] * 1000u / n[i]),
					(unsigned long)(nodes[i] / n[i]),
					(unsigned long)(nodes[i] * 10u / n[i] % 10u), (unsigned long)hash[i]);
		if (pass < 2)
			log_printf("colbench %s sync_loads=%lu sync_us=%lu\n", pass == 0 ? "cold" : "warm",
				(unsigned long)sync, (unsigned long)sync_us);
	}
	return 0;
}

void col_init(void)
{
	dbg_handler_set(DBG_COLBENCH, cmd_colbench);
}

#else

void col_init(void)
{
}

#endif
