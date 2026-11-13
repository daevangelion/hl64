/* SPDX-License-Identifier: MIT */
/*
 * area.c -- host harness for docs/specs/area-links-and-touch.md: the real
 * engine/ent grid, engine/phys touches and engine/coll traces driven by a
 * line script on stdin; test/m2/area.py builds the scripts from the
 * GoldSrc probe fixtures (A1, A3, A4, A5, A7, A9) and compares the output.
 * `selftest` runs the self-contained tests (A2 randomised grid, A6 frees
 * during touches) and prints "selftest ok" or FAIL lines.
 *
 *   hl64-area [--root CONTENT]      script on stdin, results on stdout
 *
 * Positions in the script are map-local: the harness adds the open map's
 * placement g_M (0 without a map) and subtracts it when printing.
 *
 * Script lines (vectors are three floats):
 *   map NAME                  open the map's layer (needs --root)
 *   ent I SOLID ORG ANG MINS MAXS FLAGS MODEL TOUCH
 *                             (re)set slot I (allocated in script order);
 *                             MODEL "-" or "*N"; TOUCH 0 none, 1 log,
 *                             2 set self SOLID_NOT, 3 free other,
 *                             4 free self
 *   org I ORG                 set the origin (no link)
 *   link I | linkt I          ent_link | phy_link_touch
 *   free I
 *   impact A B                phy_impact
 *   move I END TYPE           col_move from I's origin, I as pass entity;
 *                             origin := end, link with touch, impact touch
 *                             on a hit; prints "move I frac hit END"
 *   box I                     prints "box I ABSMIN ABSMAX"
 *   query MASK MINS MAXS      prints "query n I..."
 *   tag TEXT                  echoed (case markers)
 * Touches print "touch SELF OTHER".
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/grid.h"
#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static float gofs[3];
static int layer_slot = -1;
/* touches seen by the self-contained tests (self << 16 | other) */
static uint32_t seen[64];
static int nseen;
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- touch modes (the probe's PT_* behaviours) --------------------------- */

static int quiet;

static void on_touch(uint16_t fn, col_ent_t *self, col_ent_t *other)
{
	if (nseen < 64)
		seen[nseen++] = (uint32_t)self->index << 16 | other->index;
	if (!quiet)
		printf("touch %u %u\n", self->index, other->index);
	switch (fn)
	{
	case 2:
		self->solid = COL_SOLID_NOT;
		ent_link(self);
		break;
	case 3:
		ent_free(other);
		break;
	case 4:
		ent_free(self);
		break;
	case 5:		/* A6: free entity 11 (another trigger) */
		if (ent_get(11))
			ent_free(ent_get(11));
		break;
	}
}

/* ---- script ---------------------------------------------------------------- */

static int vec(char **s, float v[3], int global)
{
	int i;

	for (i = 0; i < 3; i++)
	{
		char *end;

		v[i] = strtof(*s, &end);
		if (end == *s)
			return -1;
		*s = end;
		if (global)
			v[i] += gofs[i];
	}
	return 0;
}

static col_ent_t *slot(int i)
{
	col_ent_t *e = ent_get(i);

	if (!e)
		e = ent_alloc_at(i);
	if (!e)
	{
		printf("FAIL script: slot %d\n", i);
		exit(1);
	}
	return e;
}

static void print_vec(const float v[3], int global)
{
	printf(" %.9g %.9g %.9g", (double)(v[0] - (global ? gofs[0] : 0)),
		(double)(v[1] - (global ? gofs[1] : 0)), (double)(v[2] - (global ? gofs[2] : 0)));
}

static int line(char *s)
{
	char op[16], name[64];
	int n, i, j;
	float a[3], b[3];

	if (sscanf(s, "%15s%n", op, &n) != 1)
		return 0;
	s += n;
	if (!strcmp(op, "map"))
	{
		wld_layer_t *L;
		int m;

		if (sscanf(s, "%63s", name) != 1 || (m = wld_map_find(name)) < 0)
			return -1;
		L = wld_open(m);
		memcpy(gofs, L->gofs, sizeof(gofs));
		layer_slot = L->slot;
		return 0;
	}
	if (!strcmp(op, "ent"))
	{
		col_ent_t *e;
		unsigned long flags;
		int solid, touch;

		if (sscanf(s, "%d %d%n", &i, &solid, &n) != 2)
			return -1;
		s += n;
		e = slot(i);
		if (ent_linked(e))
			ent_unlink(e);
		e->solid = (uint8_t)solid;
		if (vec(&s, e->origin, 1) || vec(&s, e->angles, 0) || vec(&s, e->mins, 0)
			|| vec(&s, e->maxs, 0))
			return -1;
		if (sscanf(s, "%lu %63s %d", &flags, name, &touch) != 3)
			return -1;
		e->flags = (uint32_t)flags;
		e->model = COL_NONE;
		if (name[0] == '*')
		{
			if (layer_slot < 0)
				return -1;
			e->model = COL_MODEL(layer_slot, atoi(name + 1));
		}
		ent_set_touch(e, (uint16_t)touch);
		return 0;
	}
	if (!strcmp(op, "org"))
	{
		if (sscanf(s, "%d%n", &i, &n) != 1)
			return -1;
		s += n;
		return vec(&s, slot(i)->origin, 1);
	}
	if (!strcmp(op, "link") || !strcmp(op, "linkt") || !strcmp(op, "free")
		|| !strcmp(op, "box"))
	{
		col_ent_t *e;

		if (sscanf(s, "%d", &i) != 1)
			return -1;
		e = ent_get(i);
		if (!e)
		{
			printf("gone %d\n", i);
			return 0;
		}
		if (op[0] == 'f')
			ent_free(e);
		else if (op[0] == 'b')
		{
			printf("box %d", i);
			print_vec(e->absmin, 1);
			print_vec(e->absmax, 1);
			printf("\n");
		}
		else if (op[4] == 't')
			phy_link_touch(e);
		else
			ent_link(e);
		return 0;
	}
	if (!strcmp(op, "impact"))
	{
		col_ent_t *ea, *eb;

		if (sscanf(s, "%d %d", &i, &j) != 2)
			return -1;
		ea = ent_get(i);
		eb = ent_get(j);
		if (ea && eb)
			phy_impact(ea, eb);
		return 0;
	}
	if (!strcmp(op, "move"))
	{
		col_ent_t *e;
		col_trace_t tr;
		int type;

		if (sscanf(s, "%d%n", &i, &n) != 1)
			return -1;
		s += n;
		if (vec(&s, a, 1) || sscanf(s, "%d", &type) != 1)
			return -1;
		e = ent_get(i);
		if (!e)
		{
			printf("gone %d\n", i);
			return 0;
		}
		col_move(e->origin, e->mins, e->maxs, a, type, e, &tr);
		memcpy(e->origin, tr.end, sizeof(tr.end));
		printf("move %d %.9g %d", i, (double)tr.frac, tr.ent);
		print_vec(tr.end, 1);
		printf("\n");
		phy_link_touch(e);
		if (tr.ent >= 0 && ent_get(i) == e && ent_get(tr.ent))
			phy_impact(e, ent_get(tr.ent));
		return 0;
	}
	if (!strcmp(op, "query"))
	{
		uint16_t out[HL64_MAX_ENTITIES];
		int mask, k;

		if (sscanf(s, "%d%n", &mask, &n) != 1)
			return -1;
		s += n;
		if (vec(&s, a, 1) || vec(&s, b, 1))
			return -1;
		n = ent_query(a, b, mask, out, HL64_MAX_ENTITIES);
		printf("query %d", n);
		for (k = 0; k < n; k++)
			printf(" %u", out[k]);
		printf("\n");
		return 0;
	}
	if (!strcmp(op, "tag"))
	{
		printf("tag%s", s);
		return 0;
	}
	if (!strcmp(op, "reset"))
	{
		ent_reset();
		return 0;
	}
	return -1;
}

/* ---- self-contained tests ---------------------------------------------------- */

static uint32_t rng = 12345;

static float frand(float lo, float hi)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return lo + (hi - lo) * (float)(rng >> 8) / 16777216.0f;
}

static int meets(const col_ent_t *e, const float mn[3], const float mx[3], int mask)
{
	int k = e->solid == COL_SOLID_TRIGGER ? ENT_Q_TRIGGER
		: e->solid == COL_SOLID_NOT ? (e->model != COL_NONE && e->skin < -1 ? ENT_Q_CONTENTS : 0)
		: ENT_Q_SOLID;

	return (k & mask) && ent_linked(e)
		&& e->absmin[0] <= mx[0] && e->absmax[0] >= mn[0]
		&& e->absmin[1] <= mx[1] && e->absmax[1] >= mn[1]
		&& e->absmin[2] <= mx[2] && e->absmax[2] >= mn[2];
}

/* A query equals a brute-force scan; no duplicates; ascending index unless
 * triggers alone (rank order, checked by A9). */
static void check_query(const float mn[3], const float mx[3], int mask, int round)
{
	uint16_t out[HL64_MAX_ENTITIES];
	int n, want, i;

	n = ent_query(mn, mx, mask, out, HL64_MAX_ENTITIES);
	for (want = 0, i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		col_ent_t *e = ent_get(i);

		if (e && meets(e, mn, mx, mask))
			want++;
	}
	CHECK(n == want, "A2 round %d: %d found, %d overlap", round, n, want);
	for (i = 0; i < n; i++)
	{
		col_ent_t *e = ent_get(out[i]);

		CHECK(e && meets(e, mn, mx, mask), "A2 round %d: %u does not overlap", round, out[i]);
		if (i && mask != ENT_Q_TRIGGER)
			CHECK(out[i - 1] < out[i], "A2 round %d: order/dup at %d", round, i);
	}
}

/* A2: random entities (sizes 1 u to 6000 u, so every grid level and the
 * huge list are used), random relinks and frees, random query boxes, then
 * the membership pool run dry; every query equals a brute-force scan. */
static void test_a2(void)
{
	uint16_t out[HL64_MAX_ENTITIES];
	int round, i, k, n, mask;

	ent_reset();
	for (round = 0; round < 4000; round++)
	{
		int op = (int)frand(0, 10);

		if (op < 6)
		{
			col_ent_t *e = ent_get(2 + (int)frand(0, HL64_MAX_ENTITIES - 2));
			float size, c[3];

			if (!e)
				e = ent_alloc();
			if (!e)
				continue;
			size = frand(0, 1) < 0.8f ? frand(1, 300) : frand(300, 6000);
			for (k = 0; k < 3; k++)
			{
				c[k] = frand(-8000, 8000);
				e->origin[k] = c[k];
				e->mins[k] = -frand(0, size);
				e->maxs[k] = frand(0, size);
				e->angles[k] = frand(0, 1) < 0.8f ? 0 : frand(-180, 180);
			}
			e->solid = (uint8_t)frand(0, 5);
			ent_link(e);
		}
		else if (op < 7)
		{
			col_ent_t *e = ent_get(2 + (int)frand(0, HL64_MAX_ENTITIES - 2));

			if (e)
				ent_free(e);
		}
		else
		{
			float mn[3], mx[3];

			for (k = 0; k < 3; k++)
			{
				float c = frand(-9000, 9000), h = frand(0, 1) < 0.9f ? frand(0, 300) : frand(0, 5000);

				mn[k] = c - h;
				mx[k] = c + h;
			}
			mask = 1 + (int)frand(0, 7);
			if (mask > 7)
				mask = 7;
			check_query(mn, mx, mask, round);
		}
	}
	/* membership pool exhaustion: every slot straddles a cell corner (8
	 * cells each), so most fall back to the huge list; then half are
	 * freed and the rest relinked into the recovered pool */
	ent_reset();
	for (i = 2; i < HL64_MAX_ENTITIES; i++)
	{
		col_ent_t *e = ent_alloc();

		for (k = 0; k < 3; k++)
		{
			e->origin[k] = 256.0f * (float)((i * (k + 3)) % 7) + frand(-2, 2);
			e->mins[k] = -10;
			e->maxs[k] = 10;
		}
		e->solid = (uint8_t)(1 + i % 4);
		ent_link(e);
	}
	{
		ent_grid_stats_t st;

		ent_grid_stats(&st);
		CHECK(st.dry > 0, "A2: the membership pool never ran dry");
	}
	for (round = 0; round < 3; round++)
	{
		for (i = 0; i < 300; i++)
		{
			float mn[3], mx[3];

			for (k = 0; k < 3; k++)
			{
				float c = frand(-100, 1900), h = frand(0, 300);

				mn[k] = c - h;
				mx[k] = c + h;
			}
			check_query(mn, mx, 1 + i % 7, 10000 + round * 1000 + i);
		}
		for (i = 2 + round; i < HL64_MAX_ENTITIES; i += 2)
		{
			col_ent_t *e = ent_get(i);

			if (round == 0 && e)
				ent_free(e);
			else if (e)
			{
				e->origin[0] += 300;
				ent_link(e);
			}
		}
	}
	/* trigger order: allocation order (runtime), cooked ranks first */
	ent_reset();
	{
		static const float zero[3], m[3] = { -10, -10, -10 }, M[3] = { 10, 10, 10 };
		col_ent_t *t[4];

		for (i = 0; i < 4; i++)
		{
			t[i] = ent_alloc_at(i == 0 ? 300 : i == 1 ? 7 : i == 2 ? 200 : 100);
			memcpy(t[i]->origin, zero, sizeof(zero));
			memcpy(t[i]->mins, m, sizeof(m));
			memcpy(t[i]->maxs, M, sizeof(M));
			t[i]->solid = COL_SOLID_TRIGGER;
			ent_link(t[i]);
		}
		ent_set_rank(t[3], 0, 5);
		ent_set_rank(t[2], 0, 2);
		n = ent_query(m, M, ENT_Q_TRIGGER, out, 8);
		CHECK(n == 4 && out[0] == 200 && out[1] == 100 && out[2] == 300 && out[3] == 7,
			"A9 synthetic order %d: %u %u %u %u", n, out[0], out[1], out[2], out[3]);
	}
	ent_reset();
}

/* A6: touches that free the trigger, another trigger, or the toucher. */
static void test_a6(void)
{
	static const float m[3] = { -10, -10, -10 }, M[3] = { 10, 10, 10 };
	col_ent_t *t1, *t2, *t3, *mv;
	int pass;

	quiet = 1;
	for (pass = 0; pass < 3; pass++)
	{
		ent_reset();
		t1 = ent_alloc_at(10);
		t2 = ent_alloc_at(11);
		t3 = ent_alloc_at(12);
		mv = ent_alloc_at(20);
		memcpy(t1->mins, m, 12), memcpy(t1->maxs, M, 12);
		memcpy(t2->mins, m, 12), memcpy(t2->maxs, M, 12);
		memcpy(t3->mins, m, 12), memcpy(t3->maxs, M, 12);
		memcpy(mv->mins, m, 12), memcpy(mv->maxs, M, 12);
		t1->solid = t2->solid = t3->solid = COL_SOLID_TRIGGER;
		mv->solid = COL_SOLID_BBOX;
		ent_link(t1);
		ent_link(t2);
		ent_link(t3);
		/* pass 0: t1 frees itself; 1: t1 frees t2 (another trigger);
		 * 2: t1 frees the mover. Runtime triggers touch in allocation
		 * order: t1, t2, t3. */
		ent_set_touch(t1, (uint16_t)(pass == 0 ? 4 : pass == 1 ? 5 : 3));
		ent_set_touch(t2, 1);
		ent_set_touch(t3, 1);
		nseen = 0;
		phy_link_touch(mv);
		CHECK(!ent_get(10) == (pass == 0), "A6 pass %d: t1 state", pass);
		CHECK(!ent_get(11) == (pass == 1), "A6 pass %d: t2 state", pass);
		CHECK(!ent_get(20) == (pass == 2), "A6 pass %d: mover state", pass);
		/* touches: t1 always; t2 unless freed (pass 1) or the mover
		 * is gone (pass 2); t3 unless the mover is gone */
		CHECK(nseen == (pass == 0 ? 3 : pass == 1 ? 2 : 1), "A6 pass %d: %d touches", pass, nseen);
		CHECK(nseen >= 1 && seen[0] == (10u << 16 | 20), "A6 pass %d: first touch %08x", pass,
			(unsigned)seen[0]);
		if (pass == 0)
			CHECK(seen[1] == (11u << 16 | 20) && seen[2] == (12u << 16 | 20), "A6 pass 0 order");
		if (pass == 1)
			CHECK(seen[1] == (12u << 16 | 20), "A6 pass 1: freed t2 touched");
	}
	quiet = 0;
	ent_reset();
}

int main(int argc, char **argv)
{
	char buf[1024];
	const char *root = NULL;
	int ln = 0;

	if (argc == 3 && !strcmp(argv[1], "--root"))
		root = argv[2];
	else if (argc != 1)
	{
		fprintf(stderr, "usage: hl64-area [--root CONTENT] < script\n");
		return 2;
	}
	host_io_init(root);
	mem_boot_carve();
	io_init();
	if (root && !wld_init())
	{
		fprintf(stderr, "hl64-area: no content in %s\n", root);
		return 2;
	}
	ent_init();
	phy_set_touch_call(on_touch);
	while (fgets(buf, sizeof(buf), stdin))
	{
		ln++;
		if (!strncmp(buf, "selftest", 8))
		{
			test_a2();
			test_a6();
			printf(fails ? "selftest FAIL\n" : "selftest ok\n");
			continue;
		}
		if (line(buf))
		{
			printf("FAIL script line %d: %s", ln, buf);
			return 1;
		}
	}
	return fails ? 1 : 0;
}
