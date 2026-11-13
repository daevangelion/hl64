/* SPDX-License-Identifier: MIT */
/*
 * colltest.c -- host driver for the collision tests (docs/specs/
 * trace-and-hulls.md 5): reads one command per line on stdin, runs it on
 * the engine's collision module with the cooked content and prints one
 * result line ("R ...") per query; test/coll/trace_tests.py feeds it the
 * GoldSrc probe fixtures and compares. Engine log lines go to stdout
 * too and never start with "R ".
 *
 *   O map zero          open a map (zero = 1: g_M forced to 0, the probe's
 *                       map frame) -> R O slot
 *   E i solid sub o[3] a[3] mins[3] maxs[3] flags owner rendermode skin
 *     movetype
 *                       entity i (sub = BSP model or -1, owner -1 none),
 *                       linked through the entity grid
 *   X i                 free entity i
 *   P p[3]              -> R P contents true_contents
 *   Q slot hull p[3]    world hull contents -> R Q contents
 *   W s[3] e[3] mins[3] maxs[3]            world trace
 *   M s[3] e[3] mins[3] maxs[3] type pass   move query (pass -1 none)
 *   C i s[3] e[3] mins[3] maxs[3]           one entity (pfnTraceModel)
 *   L slot hull o[3] s[3] e[3]              world model of a layer at o
 *   N s[3] e[3] type pass                   line trace (pfnTraceLine)
 *   K idx s64 a64       bind MODEL index idx to cooked studio files
 *   A i idx seq frame c0 c1 c2 c3 b0 b1 body
 *                       entity i plays model idx in that pose: line traces
 *                       test its cooked hitboxes (engine/render/studio.c)
 *   H i                 entity i's hitboxes -> R H n, then n lines "bone[12]
 *                       mins[3] maxs[3] group"
 *   G i n               hitboxes of entity i (marks it a studio entity):
 *                       n lines follow, each bone[3][4] mins[3] maxs[3] group
 *   F                   frame boundary (residency fences)
 *   B slot n            bench: the next n lines are "hull s[3] e[3]";
 *                       world traces timed -> R B n us nodes hash
 *   traces print R T as ss io iw fr end[3] n[3] d hit hg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/grid.h"
#include "hl64/ent.h"
#include "hl64/studio.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

#define HB_MAX	64

static col_hitbox_t	hb[HB_MAX];
static int		hb_n, hb_ent = -1;
static uint32_t		frame;

static char		bound[64][2][256];

/* The synthetic hitboxes of hb_ent (G), else the studio pose (B / M). */
static int hitboxes(const col_ent_t *e, const col_hitbox_t **out)
{
	if (e->index != hb_ent)
		return studio_hitboxes(e, out);
	*out = hb;
	return hb_n;
}

static void put_trace(const col_trace_t *t)
{
	printf("R T %d %d %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d\n", t->allsolid,
		t->startsolid, t->inopen, t->inwater, t->frac, t->end[0], t->end[1], t->end[2],
		t->normal[0], t->normal[1], t->normal[2], t->dist, t->ent, t->hitgroup);
}

static int floats(char **s, float *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
	{
		char *end;

		v[i] = strtof(*s, &end);
		if (end == *s)
			return 0;
		*s = end;
	}
	return 1;
}

static int ints(char **s, long *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
	{
		char *end;

		v[i] = strtol(*s, &end, 0);
		if (end == *s)
			return 0;
		*s = end;
	}
	return 1;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static void bad(const char *line)
{
	fprintf(stderr, "colltest: bad command: %s", line);
	exit(2);
}

int main(int argc, char **argv)
{
	static char line[4096];
	const char *root = argc > 1 ? argv[1] : NULL;

	if (argc != 2)
	{
		fprintf(stderr, "usage: hl64-colltest CONTENT_DIR < commands\n");
		return 2;
	}
	/* the boot steps collision needs (not eng_boot: no frame loop, game or
	 * renderer state) */
	host_io_init(root);
	mem_boot_carve();
	io_init();
	if (!wld_init())
	{
		fprintf(stderr, "colltest: no maps/campaign.cwg under %s\n", root);
		return 2;
	}
	ent_init();
	studio_init();
	col_set_hitbox_source(hitboxes);
	log_flush();
	while (fgets(line, sizeof(line), stdin))
	{
		char *s = line + 1;
		float v[24];
		long n[8];
		col_trace_t t;

		switch (line[0])
		{
		case 'O':
		{
			char name[64];
			int zero, map;
			wld_layer_t *L;

			if (sscanf(s, "%63s %d", name, &zero) != 2 || (map = wld_map_find(name)) < 0)
				bad(line);
			L = wld_open(map);
			if (zero)
				memset(L->gofs, 0, sizeof(L->gofs));
			log_flush();
			printf("R O %d %.9g %.9g %.9g\n", L->slot, L->gofs[0], L->gofs[1], L->gofs[2]);
			break;
		}
		case 'E':
		{
			col_ent_t *e;

			if (!ints(&s, n, 3) || !floats(&s, v, 12) || !ints(&s, n + 3, 5))
				bad(line);
			e = ent_get((int)n[0]);
			if (!e)
				e = ent_alloc_at((int)n[0]);
			if (!e)
				bad(line);
			memcpy(e->origin, v, 12);
			memcpy(e->angles, v + 3, 12);
			memcpy(e->mins, v + 6, 12);
			memcpy(e->maxs, v + 9, 12);
			e->solid = (uint8_t)n[1];
			e->model = n[2] < 0 ? COL_NONE : COL_MODEL(0, n[2]);
			e->flags = (uint32_t)n[3];
			e->owner = n[4] < 0 ? COL_NONE : (uint16_t)n[4];
			e->rendermode = (uint8_t)n[5];
			e->skin = (int16_t)n[6];
			e->movetype = (uint8_t)n[7];
			ent_link(e);
			break;
		}
		case 'K':
		{
			int i;

			if (sscanf(s, "%d", &i) != 1 || i < 0 || i >= 64
				|| sscanf(s, "%*d %255s %255s", bound[i][0], bound[i][1]) != 2)
				bad(line);
			studio_test_bind(i, bound[i][0], bound[i][1]);
			break;
		}
		case 'A':
		{
			ent_rnd_t *r;
			long q[10];
			int k;

			/* A slot model seq frame c0 c1 c2 c3 b0 b1 body: studio pose */
			if (!ints(&s, q, 3) || !floats(&s, v, 1) || !ints(&s, q + 3, 7)
				|| !ent_get((int)q[0]))
				bad(line);
			r = ent_rnd((int)q[0]);
			r->model = RES_MAKE(RES_K_MODEL, q[1]);
			r->sequence = (uint16_t)q[2];
			r->frame = v[0];
			for (k = 0; k < 4; k++)
				r->controller[k] = (uint8_t)q[3 + k];
			r->blending[0] = (uint8_t)q[7];
			r->blending[1] = (uint8_t)q[8];
			r->body = (uint8_t)q[9];
			ent_get((int)q[0])->studio = r->model;
			break;
		}
		case 'H':
		{
			const col_hitbox_t *h;
			int i, k, cnt;

			/* H slot: the entity's hitboxes -> R H n, then n lines */
			if (!ints(&s, n, 1) || !ent_get((int)n[0]))
				bad(line);
			cnt = hitboxes(ent_get((int)n[0]), &h);
			printf("R H %d\n", cnt);
			for (i = 0; i < cnt; i++)
			{
				for (k = 0; k < 12; k++)
					printf("%.9g ", (double)h[i].bone[k / 4][k % 4]);
				printf("%.9g %.9g %.9g %.9g %.9g %.9g %d\n", (double)h[i].mins[0],
					(double)h[i].mins[1], (double)h[i].mins[2], (double)h[i].maxs[0],
					(double)h[i].maxs[1], (double)h[i].maxs[2], h[i].group);
			}
			break;
		}
		case 'X':
			if (!ints(&s, n, 1) || !ent_get((int)n[0]))
				bad(line);
			ent_free(ent_get((int)n[0]));
			if (hb_ent == n[0])
				hb_ent = -1;
			break;
		case 'P':
			if (!floats(&s, v, 3))
				bad(line);
			printf("R P %d %d\n", col_contents(v), col_true_contents(v));
			break;
		case 'Q':
		{
			wld_layer_t *L;

			if (!ints(&s, n, 2) || !floats(&s, v, 3) || !(L = wld_layer((int)n[0])))
				bad(line);
			printf("R Q %d\n", col_model_contents(COL_MODEL(n[0], 0), L->gofs, NULL, (int)n[1], v));
			break;
		}
		case 'W':
			if (!floats(&s, v, 12))
				bad(line);
			col_trace_world(v, v + 6, v + 9, v + 3, &t);
			put_trace(&t);
			break;
		case 'N':
			if (!floats(&s, v, 6) || !ints(&s, n, 2))
				bad(line);
			col_trace_line(v, v + 3, (int)n[0], n[1] < 0 ? NULL : ent_get((int)n[1]), &t);
			put_trace(&t);
			break;
		case 'M':
			if (!floats(&s, v, 12) || !ints(&s, n, 2))
				bad(line);
			col_move(v, v + 6, v + 9, v + 3, (int)n[0], n[1] < 0 ? NULL : ent_get((int)n[1]),
				&t);
			put_trace(&t);
			break;
		case 'C':
			if (!ints(&s, n, 1) || !floats(&s, v, 12) || !ent_get((int)n[0]))
				bad(line);
			col_clip(ent_get((int)n[0]), v, v + 6, v + 9, v + 3, &t);
			put_trace(&t);
			break;
		case 'L':
			if (!ints(&s, n, 2) || !floats(&s, v, 9))
				bad(line);
			col_trace_model(COL_MODEL(n[0], 0), v, NULL, (int)n[1], v + 3, v + 6, &t);
			put_trace(&t);
			break;
		case 'G':
		{
			int i;

			if (!ints(&s, n, 2) || n[1] < 0 || n[1] > HB_MAX)
				bad(line);
			if (!ent_get((int)n[0]))
				bad(line);
			ent_get((int)n[0])->studio = 0;	/* any studio model: hitboxes apply */
			hb_ent = (int)n[0];
			hb_n = (int)n[1];
			for (i = 0; i < hb_n; i++)
			{
				char *p = line + 0;
				long g;

				if (!fgets(line, sizeof(line), stdin))
					bad("G: missing hitbox line\n");
				if (!floats(&p, &hb[i].bone[0][0], 12) || !floats(&p, hb[i].mins, 3)
					|| !floats(&p, hb[i].maxs, 3) || !ints(&p, &g, 1))
					bad(line);
				hb[i].group = (uint8_t)g;
			}
			break;
		}
		case 'F':
			wld_frame(++frame);
			break;
		case 'B':
		{
			static float seg[4096][7];
			col_stats_t cs;
			uint64_t t0, us;
			uint32_t h = 2166136261u;
			int i, cnt;

			if (!ints(&s, n, 2) || n[1] < 0 || n[1] > 4096)
				bad(line);
			cnt = (int)n[1];
			for (i = 0; i < cnt; i++)
			{
				char *p = line;

				if (!fgets(line, sizeof(line), stdin) || !floats(&p, seg[i], 7))
					bad("B: bad segment line\n");
			}
			col_stats(&cs);
			t0 = sys_time_us();
			for (i = 0; i < cnt; i++)
			{
				int hull = (int)seg[i][0];

				col_trace_world(seg[i] + 1, col_hull_mins[hull], col_hull_maxs[hull], seg[i] + 4,
					&t);
				h = fnv(h, &t, sizeof(t));
			}
			us = sys_time_us() - t0;
			col_stats(&cs);
			printf("R B %d %llu %lu %08lx\n", cnt, (unsigned long long)us,
				(unsigned long)cs.nodes, (unsigned long)h);
			break;
		}
		case '#':
		case '\n':
			break;
		default:
			bad(line);
		}
		log_flush();
	}
	log_flush();
	return 0;
}
