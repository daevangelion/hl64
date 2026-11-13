/* SPDX-License-Identifier: MIT */
/*
 * ent.c -- host harness for docs/specs/think-and-time.md (K1-K3, K6-K9) and
 * the entity layer's own properties: the real engine/ent components, think
 * heap, active-set pass and game clock driven by a line script on stdin;
 * test/m3/ent.py builds the scripts from the GoldSrc probe fixtures
 * (K-think, K-alloc, K-settle) and compares the output. `selftest` runs
 * the self-contained tests (K7 reserves, heap / active-set unit test, P13
 * resting counters, K9 step-all equivalence, private slabs); `frames MAP
 * MAP` the two-layer frame test (needs --root).
 *
 *   hl64-ent [--root CONTENT]      script on stdin, results on stdout
 *
 * Script lines:
 *   newgame                   ent_reset, clock New Game (time 1.0, settle)
 *   frame DT                  frame start: eng_clock_frame(DT) (DT an f32)
 *   pass                      the active-set pass, then the clock advances
 *   clock                     prints "clock T FT" (f32)
 *   thinker I MODE PERIOD COUNT OTHER
 *                             slot I: MOVETYPE_NONE probe thinker; MODE 0
 *                             log, 1 reschedule at time + PERIOD (COUNT
 *                             thinks, 0 = never stops), 2 set OTHER's
 *                             nextthink to the time
 *   nt I V                    ent_think_at(I, V)
 *   state I                   prints "state I NT"
 *   free I | remove I | dirty I   ent_destroy | ent_remove | origin
 *                             (1,2,3) and FL_FLY (the probe before a free)
 *   freeif I                  ent_destroy when live
 *   seedused I SERIAL         slot I live (no components) with that serial
 *   seedfree I FT SERIAL      slot I free with free time FT and serial
 *   alloc                     ent_create(GAME): prints "alloc I SERIAL FT
 *                             OX OY OZ FL"
 *   freelist                  prints "freelist N I:FT:SERIAL ..." (free
 *                             slots below the highest ever used)
 * Thinks print "think FRAME I T NT" (FRAME = frames since newgame, T the
 * time the think sees, NT its nextthink when called).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int fails;
static const char *root;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- probe thinkers (test/oracle/probe TH_*) ----------------------------- */

enum { TH_LOG, TH_RESCHED, TH_SET_OTHER };

static struct {
	uint8_t		mode;
	float		period;
	int		count, tag;
	int		other;
} th[HL64_MAX_ENTITIES];

static int frameno, quiet;

static void on_think(int e)
{
	float t = (float)eng_time();

	if (!quiet)
		printf("think %d %d %.9g %.9g\n", frameno, e, (double)t,
			(double)ent_tm(e)->nextthink);
	th[e].tag++;
	switch (th[e].mode)
	{
	case TH_RESCHED:
		if (th[e].count == 0 || th[e].tag < th[e].count)
			ent_think_at(e, t + th[e].period);
		break;
	case TH_SET_OTHER:
		if (ent_is_live(th[e].other))
		{
			ent_think_at(th[e].other, t);
			if (!quiet)
				printf("set %d %d %.9g\n", e, th[e].other, (double)t);
		}
		break;
	}
}

static const phy_game_hooks_t probe_hooks = { on_think, NULL, NULL };

/* ---- script --------------------------------------------------------------- */

static double gdt;

static int line(char *s)
{
	char cmd[32];
	int i, n;

	if (sscanf(s, "%31s%n", cmd, &n) != 1)
		return 0;
	s += n;
	if (!strcmp(cmd, "newgame"))
	{
		ent_reset();
		eng_clock_new_game();
		phy_set_game_hooks(&probe_hooks);
		memset(th, 0, sizeof(th));
		frameno = 0;
		return 0;
	}
	if (!strcmp(cmd, "frame"))
	{
		float dt;

		if (sscanf(s, "%f", &dt) != 1)
			return -1;
		frameno++;
		gdt = eng_clock_frame((double)dt);
		return 0;
	}
	if (!strcmp(cmd, "pass"))
	{
		phy_entities_frame(eng_clock_time(), gdt);
		eng_clock_advance();
		return 0;
	}
	if (!strcmp(cmd, "clock"))
	{
		printf("clock %.9g %.9g\n", (double)(float)eng_time(), (double)(float)eng_frametime());
		return 0;
	}
	if (!strcmp(cmd, "thinker"))
	{
		static const float m[3] = { -1, -1, -1 }, M[3] = { 1, 1, 1 }, z[3];
		int mode, count, other;
		float period;

		if (sscanf(s, "%d %d %f %d %d", &i, &mode, &period, &count, &other) != 5
			|| i < 2 || i >= HL64_MAX_ENTITIES)
			return -1;
		if (!ent_is_live(i) && !ent_alloc_at(i))
			return -1;
		ent_set_movetype(i, MOVETYPE_NONE);
		ent_set_solid(i, COL_SOLID_NOT);
		ent_set_size(i, m, M);
		ent_set_origin(i, z);
		ent_tm(i)->think = 1;
		th[i].mode = (uint8_t)mode;
		th[i].period = period;
		th[i].count = count;
		th[i].tag = 0;
		th[i].other = other;
		return 0;
	}
	if (!strcmp(cmd, "nt"))
	{
		float v;

		if (sscanf(s, "%d %f", &i, &v) != 2 || !ent_is_live(i))
			return -1;
		ent_think_at(i, v);
		return 0;
	}
	if (!strcmp(cmd, "state"))
	{
		if (sscanf(s, "%d", &i) != 1)
			return -1;
		if (ent_is_live(i))
			printf("state %d %.9g\n", i, (double)ent_tm(i)->nextthink);
		else
			printf("state %d free\n", i);
		return 0;
	}
	if (!strcmp(cmd, "seedused"))
	{
		int serial;

		if (sscanf(s, "%d %d", &i, &serial) != 2 || !ent_alloc_at(i))
			return -1;
		ent_idc(i)->serial = (uint16_t)serial;
		return 0;
	}
	if (!strcmp(cmd, "free") || !strcmp(cmd, "remove") || !strcmp(cmd, "dirty"))
	{
		if (sscanf(s, "%d", &i) != 1 || i < 0 || i >= HL64_MAX_ENTITIES)
			return -1;
		if (!ent_is_live(i))
			return -1;
		if (cmd[0] == 'f')
			ent_destroy(i);
		else if (cmd[0] == 'r')
			ent_remove(i);
		else
		{
			static const float o[3] = { 1, 2, 3 };

			ent_set_origin(i, o);
			ent_set_flags(i, FL_FLY);
		}
		return 0;
	}
	if (!strcmp(cmd, "freeif"))
	{
		if (sscanf(s, "%d", &i) != 1 || i < 0 || i >= HL64_MAX_ENTITIES)
			return -1;
		ent_destroy(i);		/* no-op when free */
		return 0;
	}
	if (!strcmp(cmd, "seedfree"))
	{
		float ft;
		int serial;

		if (sscanf(s, "%d %f %d", &i, &ft, &serial) != 3 || !ent_alloc_at(i))
			return -1;
		ent_destroy(i);
		ent_idc(i)->serial = (uint16_t)serial;
		ent_s.freetime[i] = ft;
		return 0;
	}
	if (!strcmp(cmd, "alloc"))
	{
		const col_ent_t *v;

		i = ent_create(ENT_ALLOC_GAME);
		v = ent_cv(i);
		printf("alloc %d %u %.9g %.9g %.9g %.9g %lu\n", i, ent_idc(i)->serial,
			(double)ent_s.freetime[i], (double)v->origin[0], (double)v->origin[1],
			(double)v->origin[2], (unsigned long)v->flags);
		return 0;
	}
	if (!strcmp(cmd, "freelist"))
	{
		int nf = 0;

		for (i = 2; i < ent_s.top; i++)
			nf += !ent_is_live(i);
		printf("freelist %d", nf);
		for (i = 2; i < ent_s.top; i++)
			if (!ent_is_live(i))
				printf(" %d:%.9g:%u", i, (double)ent_s.freetime[i], ent_idc(i)->serial);
		printf("\n");
		return 0;
	}
	return -1;
}

/* ---- self-contained tests ----------------------------------------------------- */

static uint32_t rng_state = 12345;

static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static float frnd(float a, float b)
{
	return a + (b - a) * (float)(rnd() & 0xFFFFFF) / (float)0x1000000;
}

static void run_frame(double dt)
{
	double g = eng_clock_frame(dt);

	frameno++;
	phy_entities_frame(eng_clock_time(), g);
	eng_clock_advance();
}

/* K7: optional hydration only while >= 48 slots are free, transients only
 * while >= 32; game allocations take every slot. */
static void test_k7(void)
{
	int i, nfree, ok;

	ent_reset();
	eng_clock_new_game();
	ent_alloc_at(ENT_WORLD);
	ent_alloc_at(ENT_PLAYER);
	while (HL64_MAX_ENTITIES - (int)ent_s.live > 48)
		CHECK(ent_create(ENT_ALLOC_GAME) >= 0, "K7: game allocation refused");
	nfree = HL64_MAX_ENTITIES - (int)ent_s.live;
	CHECK(nfree == 48, "K7: %d free", nfree);
	ok = ent_create(ENT_ALLOC_OPTIONAL);
	CHECK(ok >= 0, "K7: optional refused at 48 free");
	CHECK(ent_create(ENT_ALLOC_OPTIONAL) < 0, "K7: optional allowed at 47 free");
	CHECK(ent_create(ENT_ALLOC_TRANSIENT) >= 0, "K7: transient refused at 47 free");
	while (HL64_MAX_ENTITIES - (int)ent_s.live > 32)
		CHECK(ent_create(ENT_ALLOC_GAME) >= 0, "K7: game allocation refused");
	CHECK(ent_create(ENT_ALLOC_OPTIONAL) < 0, "K7: optional allowed at 32 free");
	CHECK(ent_create(ENT_ALLOC_TRANSIENT) >= 0, "K7: transient refused at 32 free");
	CHECK(ent_create(ENT_ALLOC_TRANSIENT) < 0, "K7: transient allowed at 31 free");
	for (i = 0; HL64_MAX_ENTITIES - (int)ent_s.live > 0; i++)
		CHECK(ent_create(ENT_ALLOC_GAME) >= 0, "K7: game allocation refused at %d free",
			HL64_MAX_ENTITIES - (int)ent_s.live);
	CHECK(ent_alloc() == NULL, "K7: allocation past the cap");
	printf("K7 ok: optional stops at 48 free, transient at 32, game fills %d slots\n",
		HL64_MAX_ENTITIES);
	ent_reset();
}

/* Heap / active-set unit test: random nextthink changes, creations and
 * frees; after every operation the sets match the components, and each
 * frame the thinks that run are exactly the brute-force due set. */
static int unit_due[HL64_MAX_ENTITIES], unit_ran[HL64_MAX_ENTITIES];

static void unit_think(int e)
{
	unit_ran[e]++;
	if (rnd() % 3 == 0)
		ent_think_at(e, (float)eng_time() + frnd(0.0f, 0.3f));
}

static void test_heap(void)
{
	static const phy_game_hooks_t h = { unit_think, NULL, NULL };
	int round, i, checks = 0, thinks = 0;
	const char *err;

	ent_reset();
	eng_clock_new_game();
	phy_set_game_hooks(&h);
	rng_state = 777;
	for (round = 0; round < 3000; round++)
	{
		int op = (int)(rnd() % 10), e = 2 + (int)(rnd() % (HL64_MAX_ENTITIES - 2));
		double t, dt = (rnd() & 1) ? 1.0 / 30 : 0.01;

		if (op < 3 && !ent_is_live(e))
		{
			if (ent_alloc_at(e))
				ent_tm(e)->think = 1;
		}
		else if (op < 4 && ent_is_live(e))
			ent_destroy(e);
		else if (op < 8 && ent_is_live(e))
		{
			float v = rnd() % 5 == 0 ? 0.0f : (float)eng_clock_time() + frnd(-0.2f, 0.5f);

			ent_think_at(e, v);
		}
		else
		{
			/* a frame: compare with the brute-force due set */
			eng_clock_frame(dt);
			t = eng_clock_time();
			for (i = 0; i < HL64_MAX_ENTITIES; i++)
			{
				float nt = ent_is_live(i) ? ent_tm(i)->nextthink : 0;

				unit_due[i] = nt > 0 && (double)nt <= t + dt;
				unit_ran[i] = 0;
			}
			phy_entities_frame(t, dt);
			eng_clock_advance();
			for (i = 0; i < HL64_MAX_ENTITIES; i++)
			{
				CHECK(unit_ran[i] == unit_due[i], "heap round %d slot %d: ran %d due %d",
					round, i, unit_ran[i], unit_due[i]);
				thinks += unit_ran[i];
			}
		}
		err = ent_sets_check();
		CHECK(!err, "heap round %d: %s", round, err ? err : "");
		checks++;
	}
	printf("heap ok: %d operations checked, %d thinks\n", checks, thinks);
	phy_set_game_hooks(NULL);
	ent_reset();
}

/* P13: resting entities are not stepped; they react on the frame
 * something moves them. */
static void test_p13(void)
{
	phy_ent_stats_t st;
	static const float v[3] = { 10, 0, 0 };
	int e[5], i;

	ent_reset();
	eng_clock_new_game();
	phy_set_game_hooks(NULL);
	for (i = 0; i < 5; i++)
		e[i] = ent_create(ENT_ALLOC_GAME);
	ent_set_movetype(e[0], MOVETYPE_NONE);		/* idle logic */
	ent_set_movetype(e[1], MOVETYPE_TOSS);		/* resting crate */
	ent_set_flags(e[1], FL_ONGROUND);
	ent_set_movetype(e[2], MOVETYPE_BOUNCE);	/* resting gib */
	ent_set_flags(e[2], FL_ONGROUND);
	ent_set_movetype(e[3], MOVETYPE_PUSH);		/* idle func_wall */
	ent_set_solid(e[3], COL_SOLID_BSP);
	ent_set_movetype(e[4], MOVETYPE_NOCLIP);	/* still noclip */
	phy_ent_stats(&st, 1);
	for (i = 0; i < 30; i++)
		run_frame(0.01);
	phy_ent_stats(&st, 1);
	CHECK(st.stepped == 0 && st.thinks == 0, "P13: %lu steps / %lu thinks of resting entities",
		(unsigned long)st.stepped, (unsigned long)st.thinks);
	ent_set_velocity(e[4], v);
	run_frame(0.01);
	phy_ent_stats(&st, 1);
	CHECK(st.stepped == 1 && st.by_type[MOVETYPE_NOCLIP] == 1, "P13: noclip given velocity: %lu steps",
		(unsigned long)st.stepped);
	CHECK(fabsf(ent_cv(e[4])->origin[0] - 0.1f) < 1e-6f, "P13: noclip moved %.9g",
		(double)ent_cv(e[4])->origin[0]);
	ent_set_velocity(e[1], v);
	run_frame(0.01);
	phy_ent_stats(&st, 1);
	CHECK(st.stepped == 2, "P13: crate given velocity: %lu steps", (unsigned long)st.stepped);
	ent_think_at(e[0], (float)eng_clock_time());
	run_frame(0.01);
	phy_ent_stats(&st, 1);
	CHECK(st.thinks == 1 && st.by_type[MOVETYPE_NONE] == 1, "P13: due think %lu", (unsigned long)st.thinks);
	printf("P13 ok: 30 frames, 0 steps of 5 resting entities; noclip / crate / think react on "
		"their frame\n");
	ent_reset();
}

/* ---- K9: step-all equivalence on a randomised entity world ------------------- */

static uint32_t k9_rng;
/* control: case 3 writes a velocity directly, bypassing the setters (K9
 * must see the difference) */
static int k9_cheat;

static uint32_t k9_rnd(void)
{
	k9_rng ^= k9_rng << 13;
	k9_rng ^= k9_rng >> 17;
	k9_rng ^= k9_rng << 5;
	return k9_rng;
}

static float k9_f(float a, float b)
{
	return a + (b - a) * (float)(k9_rnd() & 0xFFFF) / 65536.0f;
}

static void k9_think(int e)
{
	float t = (float)eng_time(), v[3];
	int k, r = (int)(k9_rnd() % 16), o;

	switch (r)
	{
	case 0:		/* spawn a mover at a random slot (ascending-index join) */
		o = ent_create(ENT_ALLOC_TRANSIENT);
		if (o >= 0)
		{
			ent_set_movetype(o, k9_rnd() & 1 ? MOVETYPE_NOCLIP : MOVETYPE_NONE);
			for (k = 0; k < 3; k++)
				v[k] = k9_f(-500, 500);
			ent_set_origin(o, v);	/* spawns place through the setters */
			for (k = 0; k < 3; k++)
				v[k] = k9_f(-50, 50);
			ent_set_velocity(o, v);
			ent_tm(o)->think = 1;
			ent_think_at(o, t + k9_f(0, 0.3f));
		}
		break;
	case 1:
		ent_remove(e);
		break;
	case 2:
		o = 2 + (int)(k9_rnd() % (HL64_MAX_ENTITIES - 2));
		if (ent_is_live(o))
			ent_think_at(o, t);	/* another's think, this frame if ahead */
		break;
	case 3:
		for (k = 0; k < 3; k++)
			v[k] = 0;
		o = 2 + (int)(k9_rnd() % (HL64_MAX_ENTITIES - 2));
		if (k9_cheat && ent_is_live(o) && o != e && ent_cv(o)->movetype == MOVETYPE_NONE)
		{
			/* another entity's motion without ent_motion_changed */
			ent_cv(o)->movetype = MOVETYPE_NOCLIP;
			ent_mot(o)->velocity[0] = 5;
		}
		else
			ent_set_velocity(e, v);
		break;
	case 4:
		o = 2 + (int)(k9_rnd() % (HL64_MAX_ENTITIES - 2));
		if (ent_is_live(o) && o != e)
		{
			ent_set_movetype(e, MOVETYPE_FOLLOW);
			ent_set_aiment(e, o);
		}
		break;
	case 5:
		o = 2 + (int)(k9_rnd() % (HL64_MAX_ENTITIES - 2));
		if (ent_is_live(o) && o != e)
			ent_destroy(o);
		break;
	default:
		break;
	}
	if (ent_is_live(e) && r != 1)
		ent_think_at(e, t + k9_f(0.0f, 0.2f));
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static uint32_t checksum(void)
{
	uint32_t h = 2166136261u;
	int i;

	for (i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		h = fnv(h, ent_cv(i), sizeof(col_ent_t));
		h = fnv(h, ent_mot(i), sizeof(ent_mot_t));
		h = fnv(h, ent_phys(i), sizeof(ent_phys_t));
		h = fnv(h, ent_tm(i), sizeof(ent_tm_t));
		h = fnv(h, ent_rnd(i), sizeof(ent_rnd_t));
		h = fnv(h, ent_idc(i), sizeof(ent_id_t));
		h = fnv(h, &ent_s.freetime[i], 4);
	}
	return h;
}

/* A copy of every component (the K9 mismatch report). */
typedef struct {
	col_ent_t	cv[HL64_MAX_ENTITIES];
	ent_mot_t	mot[HL64_MAX_ENTITIES];
	ent_phys_t	phys[HL64_MAX_ENTITIES];
	ent_tm_t	tm[HL64_MAX_ENTITIES];
	ent_rnd_t	rnd[HL64_MAX_ENTITIES];
	ent_id_t	id[HL64_MAX_ENTITIES];
} k9_snap_t;
static k9_snap_t k9_snap[2];

static void k9_take(k9_snap_t *s)
{
	memcpy(s->cv, ent_c.cv, sizeof(s->cv));
	memcpy(s->mot, ent_c.mot, sizeof(s->mot));
	memcpy(s->phys, ent_c.phys, sizeof(s->phys));
	memcpy(s->tm, ent_c.tm, sizeof(s->tm));
	memcpy(s->rnd, ent_c.rnd, sizeof(s->rnd));
	memcpy(s->id, ent_c.id, sizeof(s->id));
}

static void k9_report(const k9_snap_t *a, const k9_snap_t *b)
{
	int i;

	for (i = 0; i < HL64_MAX_ENTITIES; i++)
	{
		const char *what = memcmp(&a->cv[i], &b->cv[i], sizeof(col_ent_t)) ? "cv"
			: memcmp(&a->mot[i], &b->mot[i], sizeof(ent_mot_t)) ? "mot"
			: memcmp(&a->phys[i], &b->phys[i], sizeof(ent_phys_t)) ? "phys"
			: memcmp(&a->tm[i], &b->tm[i], sizeof(ent_tm_t)) ? "tm"
			: memcmp(&a->rnd[i], &b->rnd[i], sizeof(ent_rnd_t)) ? "rnd"
			: memcmp(&a->id[i], &b->id[i], sizeof(ent_id_t)) ? "id" : NULL;

		if (what)
		{
			printf("K9: slot %d %s differs: mt %d/%d o %.9g/%.9g absmin %.9g/%.9g nt %.9g/%.9g "
				"serial %u/%u\n", i, what, a->cv[i].movetype, b->cv[i].movetype,
				(double)a->cv[i].origin[0], (double)b->cv[i].origin[0],
				(double)a->cv[i].absmin[0], (double)b->cv[i].absmin[0],
				(double)a->tm[i].nextthink, (double)b->tm[i].nextthink,
				a->id[i].serial, b->id[i].serial);
			return;
		}
	}
}

/* Run the world; sums[f] = checksum after frame f; snapshot at frame
 * `snap` into s. */
static phy_ent_stats_t k9_last;

static void k9_run(int step_all, uint32_t *sums, int frames, uint32_t *stepped, int snap,
	k9_snap_t *s)
{
	static const phy_game_hooks_t h = { k9_think, NULL, NULL };
	phy_ent_stats_t st;
	int i, f, k;

	ent_reset();
	eng_clock_new_game();
	phy_set_game_hooks(&h);
	phy_set_step_all(step_all);
	k9_rng = 4242;
	for (i = 0; i < HL64_MAX_ENTITIES; i++)
		ent_c.id[i].serial = 0;		/* ent_reset keeps serials: same start */
	ent_alloc_at(ENT_WORLD);
	for (i = 0; i < 200; i++)
	{
		int e = ent_create(ENT_ALLOC_GAME), mt = (int)(k9_rnd() % 4);
		float v[3];

		ent_set_movetype(e, mt == 0 ? MOVETYPE_NONE : mt == 1 ? MOVETYPE_NOCLIP
			: mt == 2 ? MOVETYPE_NONE : MOVETYPE_FOLLOW);
		if (mt == 3)
			ent_set_aiment(e, 2 + (int)(k9_rnd() % (i + 1)));
		for (k = 0; k < 3; k++)
			v[k] = k9_f(-500, 500);
		ent_set_origin(e, v);
		for (k = 0; k < 3; k++)
			v[k] = (k9_rnd() & 3) ? 0 : k9_f(-100, 100);
		if (mt == 1)
			ent_set_velocity(e, v);
		ent_tm(e)->think = 1;
		if (k9_rnd() & 1)
			ent_think_at(e, 1.0f + k9_f(0, 1));
	}
	phy_ent_stats(&st, 1);
	for (f = 0; f < frames; f++)
	{
		run_frame(f % 7 == 3 ? 0.1 : f & 1 ? 0.01 : 1.0 / 30);
		sums[f] = checksum();
		if (f == snap)
			k9_take(s);
	}
	phy_ent_stats(&st, 1);
	*stepped = st.stepped;
	k9_last = st;
	phy_set_step_all(0);
	phy_set_game_hooks(NULL);
}

static void test_k9(void)
{
	enum { FRAMES = 600 };
	static uint32_t a[FRAMES], b[FRAMES];
	uint32_t sa, sb;
	phy_ent_stats_t pa, pb;
	int f, first = -1;

	k9_run(0, a, FRAMES, &sa, -1, NULL);
	pa = k9_last;
	k9_run(1, b, FRAMES, &sb, -1, NULL);
	pb = k9_last;
	for (f = 0; f < FRAMES; f++)
		if (a[f] != b[f])
		{
			first = f;
			break;
		}
	CHECK(first < 0, "K9: component checksums differ from frame %d", first);
	if (first >= 0)
	{
		k9_run(0, a, FRAMES, &sa, first, &k9_snap[0]);
		k9_run(1, b, FRAMES, &sb, first, &k9_snap[1]);
		k9_report(&k9_snap[0], &k9_snap[1]);
	}
	CHECK(sb > sa, "K9: step-all stepped %lu, active sets %lu", (unsigned long)sb,
		(unsigned long)sa);
	if (first < 0)
		printf("K9 ok: %d frames, component checksums equal every frame; steps active-set %lu "
			"vs step-all %lu; host us/frame avg %lu max %lu vs %lu %lu; thinks %lu, "
			"movers_max %lu, heap_max %lu\n", FRAMES, (unsigned long)sa, (unsigned long)sb,
			(unsigned long)(pa.us / pa.frames), (unsigned long)pa.us_max,
			(unsigned long)(pb.us / pb.frames), (unsigned long)pb.us_max,
			(unsigned long)pa.thinks, (unsigned long)pa.movers_max,
			(unsigned long)pa.heap_max);
	/* control: a write that skips the setters must show */
	k9_cheat = 1;
	k9_run(0, a, FRAMES, &sa, -1, NULL);
	k9_run(1, b, FRAMES, &sb, -1, NULL);
	k9_cheat = 0;
	for (f = 0; f < FRAMES && a[f] == b[f]; f++)
		;
	CHECK(f < FRAMES, "K9 control: a direct velocity write went unnoticed");
	if (f < FRAMES)
		printf("K9 control ok: a velocity written without the setters shows from frame %d\n", f);
	ent_reset();
}

/* Private slabs: zeroed blocks per size class, pages returned when empty,
 * refusal when the region is full. */
static void test_priv(void)
{
	ent_priv_stats_t ps;
	int i, n = 0, refused = 0;
	uint8_t *p;

	ent_reset();
	eng_clock_new_game();
	for (i = 2; i < HL64_MAX_ENTITIES; i++)
	{
		int e = ent_create(ENT_ALLOC_GAME);
		uint32_t size = 32u << (i % 6);

		p = ent_priv_alloc(e, size - (uint32_t)(i % 7));
		if (!p)
		{
			refused++;
			continue;
		}
		n++;
		CHECK(p[0] == 0 && p[size - 1 - (uint32_t)(i % 7)] == 0, "priv: block not zeroed");
		memset(p, 0xA5, size - (uint32_t)(i % 7));
		CHECK(ent_priv(e) == p, "priv: ent_priv");
	}
	ent_priv_stats(&ps);
	CHECK(refused > 0 && ps.refused == (uint32_t)refused, "priv: refused %d / %lu", refused,
		(unsigned long)ps.refused);
	CHECK(ps.pages == ps.pages_total, "priv: %lu of %lu pages", (unsigned long)ps.pages,
		(unsigned long)ps.pages_total);
	for (i = 2; i < HL64_MAX_ENTITIES; i++)
		ent_destroy(i);
	ent_priv_stats(&ps);
	CHECK(ps.pages == 0 && ps.bytes_live == 0, "priv: %lu pages / %lu B left after freeing",
		(unsigned long)ps.pages, (unsigned long)ps.bytes_live);
	printf("priv ok: %d blocks, %d refused when the %lu pages were full, peak %lu B; all pages "
		"back after the frees\n", n, refused, (unsigned long)ps.pages_total,
		(unsigned long)ps.bytes_peak);
	ent_reset();
}

/* ---- frames: two open layers (architecture.md 8) -------------------------------- */

static void test_frames(const char *m0, const char *m1)
{
	int a = wld_map_find(m0), b = wld_map_find(m1), la, lb, e, k, n;
	const wld_layer_t *A, *B;
	float ga[3], gb[3], d[3], p[3], s[3], en[3];
	static const float mins[3] = { -16, -16, -16 }, maxs[3] = { 16, 16, 16 }, z[3];
	col_trace_t tr, tg;
	uint16_t out[8];

	if (a < 0 || b < 0 || !(A = wld_open(a)) || !(B = wld_open(b)))
	{
		printf("FAIL frames: cannot open %s / %s\n", m0, m1);
		fails++;
		return;
	}
	la = A->slot;
	lb = B->slot;
	memcpy(ga, A->gofs, sizeof(ga));
	memcpy(gb, B->gofs, sizeof(gb));
	ent_reset();
	eng_clock_new_game();
	/* a box in layer B at B-local p, high above B's world model */
	eng_set_frame_layer(lb);
	e = ent_create(ENT_ALLOC_GAME);
	CHECK(ent_idc(e)->layer == lb, "frames: created in layer %d, frame %d", ent_idc(e)->layer, lb);
	for (k = 0; k < 3; k++)
		p[k] = 0.5f * (B->models[0].mins[k] + B->models[0].maxs[k]);
	ent_set_size(e, mins, maxs);
	ent_set_solid(e, COL_SOLID_BBOX);
	ent_set_origin(e, p);
	/* from A's frame: the same box at p + g_M(B) - g_M(A) */
	eng_set_frame_layer(la);
	CHECK(ent_frame_delta(e, d) == ((ga[0] != gb[0]) || (ga[1] != gb[1]) || (ga[2] != gb[2])),
		"frames: delta flag");
	for (k = 0; k < 3; k++)
		CHECK(d[k] == gb[k] - ga[k], "frames: delta %d %.9g", k, (double)d[k]);
	for (k = 0; k < 3; k++)
	{
		s[k] = p[k] + d[k];
		en[k] = s[k];
	}
	s[0] -= 200;
	en[0] += 200;
	/* clip against the entity alone (stored view: moved into A's frame) */
	col_clip(ent_cv(e), s, z, z, en, &tr);
	CHECK(tr.ent == e && fabsf(tr.end[0] - (p[0] + d[0] - 16 - 1.0f / 32)) < 0.01f,
		"frames: clip from A's frame ent %d end %.9g want %.9g", tr.ent, (double)tr.end[0],
		(double)(p[0] + d[0] - 16));
	/* grid query in A's frame meets it; in B's frame at A-local coordinates not */
	n = ent_query_in(la, s, en, ENT_Q_SOLID, out, 8);
	CHECK(n == 1 && out[0] == e, "frames: query in A's frame %d", n);
	/* global: the same box at p + g_M(B) */
	eng_set_frame_layer(-1);
	for (k = 0; k < 3; k++)
	{
		s[k] = p[k] + gb[k];
		en[k] = s[k];
	}
	s[0] -= 200;
	en[0] += 200;
	col_clip(ent_cv(e), s, z, z, en, &tg);
	CHECK(tg.ent == e && fabsf(tg.end[0] - (tr.end[0] + ga[0])) < 0.01f,
		"frames: global clip end %.9g vs A-frame %.9g + %.9g", (double)tg.end[0],
		(double)tr.end[0], (double)ga[0]);
	n = ent_query(s, en, ENT_Q_SOLID, out, 8);
	CHECK(n == 1 && out[0] == e, "frames: global query %d", n);
	/* in its own frame: unchanged local coordinates */
	eng_set_frame_layer(lb);
	CHECK(ent_frame_delta(e, d) == 0 && d[0] == 0 && d[1] == 0 && d[2] == 0, "frames: own frame");
	printf("frames ok: %s (layer %d, g_M %.9g %.9g %.9g) / %s (layer %d, g_M %.9g %.9g %.9g): "
		"clip, query and delta agree in both frames and globally\n", m0, la, (double)ga[0],
		(double)ga[1], (double)ga[2], m1, lb, (double)gb[0], (double)gb[1], (double)gb[2]);
	eng_set_frame_layer(-1);
	ent_reset();
}

/* Searches (7.1): sphere / box / class against brute force. */
static void test_searches(void)
{
	int i, k, round, checks = 0;
	uint16_t out[HL64_MAX_ENTITIES];

	ent_reset();
	eng_clock_new_game();
	rng_state = 99;
	for (i = 0; i < 300; i++)
	{
		int e = ent_create(ENT_ALLOC_GAME);
		float o[3], mn[3], mx[3];

		for (k = 0; k < 3; k++)
		{
			o[k] = frnd(-1000, 1000);
			mn[k] = -frnd(0, 40);
			mx[k] = frnd(0, 40);
		}
		ent_idc(e)->cls = (cls_t)(rnd() % 5);
		ent_set_size(e, mn, mx);
		ent_set_flags(e, rnd() & 1 ? FL_MONSTER : 0);
		if (i % 5 == 0)
			ent_cv(e)->origin[0] = o[0];	/* placed but never linked: no box */
		else
		{
			ent_set_solid(e, (int)(rnd() % 4));
			ent_set_origin(e, o);
		}
	}
	for (round = 0; round < 200; round++)
	{
		float c[3], r = frnd(0, 400), lo[3], hi[3];
		int e = -1, n, want;
		uint32_t mask = round & 1 ? FL_MONSTER : 0;

		for (k = 0; k < 3; k++)
		{
			c[k] = frnd(-1000, 1000);
			lo[k] = c[k] - frnd(0, 300);
			hi[k] = c[k] + frnd(0, 300);
		}
		/* sphere: ascending, box distance <= r, every live slot from 1 */
		for (i = 1; i < HL64_MAX_ENTITIES; i++)
		{
			float d2 = 0;

			if (!ent_is_live(i))
				continue;
			for (k = 0; k < 3; k++)
			{
				float a = ent_cv(i)->absmin[k], b = ent_cv(i)->absmax[k];
				float x = c[k] < a ? a - c[k] : c[k] > b ? c[k] - b : 0;

				d2 += x * x;
			}
			if (d2 > r * r)
				continue;
			e = ent_next_in_sphere(e, c, r);
			CHECK(e == i, "sphere round %d: got %d want %d", round, e, i);
			checks++;
		}
		CHECK(ent_next_in_sphere(e, c, r) < 0, "sphere round %d: extra result", round);
		/* box: linked entities only, flag mask, ascending */
		n = ent_in_box(lo, hi, mask, out, HL64_MAX_ENTITIES);
		for (i = 1, want = 0; i < HL64_MAX_ENTITIES; i++)
		{
			const col_ent_t *v = ent_cv(i);

			if (!ent_is_live(i) || !ent_sets_linked(i) || (mask && !(v->flags & mask)))
				continue;
			for (k = 0; k < 3; k++)
				if (v->absmin[k] > hi[k] || v->absmax[k] < lo[k])
					break;
			if (k < 3)
				continue;
			CHECK(want < n && out[want] == i, "box round %d: result %d", round, want);
			want++;
			checks++;
		}
		CHECK(n == want, "box round %d: %d results, want %d", round, n, want);
		/* class */
		e = -1;
		for (i = 0; i < HL64_MAX_ENTITIES; i++)
			if (ent_is_live(i) && ent_idc(i)->cls == (cls_t)(round % 5))
			{
				e = ent_next_class(e, (cls_t)(round % 5), -1);
				CHECK(e == i, "class round %d: got %d want %d", round, e, i);
				checks++;
			}
		CHECK(ent_next_class(e, (cls_t)(round % 5), -1) < 0, "class round %d: extra", round);
	}
	printf("searches ok: 200 rounds, %d results checked (sphere, box with flag masks and "
		"unlinked entities, class)\n", checks);
	ent_reset();
}

/* pvs MAP EYE P: a viewer of the map's layer at EYE; prints "pvs LE LP V". */
static int cmd_pvs(const char *s)
{
	char m[64];
	float eye[3], p[3];
	const wld_layer_t *L;
	int map, e, v;

	if (sscanf(s, "pvs %63s %f %f %f %f %f %f", m, &eye[0], &eye[1], &eye[2], &p[0], &p[1],
		&p[2]) != 7 || (map = wld_map_find(m)) < 0 || !(L = wld_open(map)))
		return -1;
	eng_set_frame_layer(L->slot);
	e = ent_create(ENT_ALLOC_GAME);
	ent_set_origin(e, eye);
	v = ent_point_in_pvs(e, p);
	printf("pvs %d %d %d\n", wld_point_leaf(L, eye), wld_point_leaf(L, p), v);
	ent_destroy(e);
	eng_set_frame_layer(-1);
	return 0;
}

int main(int argc, char **argv)
{
	char buf[1024];
	int ln = 0;

	if (argc == 3 && !strcmp(argv[1], "--root"))
		root = argv[2];
	else if (argc != 1)
	{
		fprintf(stderr, "usage: hl64-ent [--root CONTENT] < script\n");
		return 2;
	}
	host_io_init(root);
	mem_boot_carve();
	io_init();
	if (root && !wld_init())
	{
		fprintf(stderr, "hl64-ent: no content in %s\n", root);
		return 2;
	}
	ent_init();
	while (fgets(buf, sizeof(buf), stdin))
	{
		char m0[64], m1[64];

		ln++;
		if (!strncmp(buf, "selftest", 8))
		{
			quiet = 1;
			test_k7();
			test_heap();
			test_p13();
			test_k9();
			test_priv();
			test_searches();
			quiet = 0;
			printf(fails ? "selftest FAIL\n" : "selftest ok\n");
			continue;
		}
		if (!strncmp(buf, "pvs ", 4))
		{
			if (cmd_pvs(buf))
			{
				printf("FAIL script line %d: %s", ln, buf);
				return 1;
			}
			continue;
		}
		if (sscanf(buf, "frames %63s %63s", m0, m1) == 2)
		{
			test_frames(m0, m1);
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
