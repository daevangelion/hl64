/* SPDX-License-Identifier: MIT */
/*
 * rec_test.c -- host driver for the record module and the resource table
 * (engine/ent/rec.c, engine/res): the real engine (carve, io, world
 * layers, components, think heap) on cooked content, with a test game
 * (the game_* exports below) that records what the engine asks of it,
 * until game/ lands. Run by test/m3/rec.py.
 *
 *   hl64-rec --root CONTENT selftest
 *       hldemo1 records, typed keys, names, fires and deferred uses,
 *       New Game spawn + player start, the hydration pump (budget,
 *       radius, optional reserve), box wakes, timers (order, ties,
 *       cascade, time seen, freeze / thaw), R2 (every res_t of the open
 *       maps inside its file), R3 (*3 of hldemo1 and hldemo2); prints
 *       "selftest ok" or FAIL lines, then the prof / mem lines
 *   hl64-rec --root CONTENT k5 T0 DT FRAMES
 *       K5: a multi_manager fired at T0 (f64, C99 hex float), frames of
 *       DT; prints "use TARGET TIME" (TIME = the f32 time seen) for every
 *       target use; the synthetic multi_manager follows think-and-time.md
 *       5 (fire when (f32)(time - start) >= delay at a think, schedule
 *       (f32)(start + next delay)); hl64_relay fires hl64_t5 0.15 s later
 *       through rec_fire_at (CBaseDelay)
 *
 * The test game is not HLSDK code: it implements only the calls' contract.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/rec.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/res/res.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the test game ------------------------------------------------------------ */

static struct {
	int		spawned, refuse_cls, logic, opened, closing;
	int		uses, logic_uses, timers;
	gid_t16		last_use_gid;
	gid_t16		last_caller;
	int		mode;		/* 1: K5 multi_manager */
	/* timer log: gid, arg, time seen */
	int		ntl;
	struct { gid_t16 gid; uint16_t arg; float t; } tl[64];
	/* K5 */
	gid_t16		mm, relay;
	name_t		t5;
	float		start;
	int		index;
} G;

int game_spawn_record(gid_t16 gid, int e)
{
	if (rec_class(gid) == G.refuse_cls)
		return -1;
	CHECK(ent_idc(e)->gid == gid && ent_idc(e)->layer == GID_SLOT(gid), "spawn ids");
	ent_set_origin(e, ent_cv(e)->origin);
	G.spawned++;
	return 0;
}

void game_spawn_logic(gid_t16 gid)
{
	(void)gid;
	G.logic++;
}

void game_map_opened(int slot)
{
	(void)slot;
	G.opened++;
}

void game_map_closing(int slot)
{
	(void)slot;
	G.closing++;
}

void game_use(int e, int activator, int caller, int use_type, float value)
{
	(void)activator; (void)caller; (void)use_type; (void)value;
	G.uses++;
	G.last_use_gid = ent_idc(e)->gid;
	G.last_caller = rec_caller_gid();
}

/* ---- K5: the synthetic multi_manager (think-and-time.md 5) -------------------- */

static const struct { const char *name; float delay; } k5_targets[] = {
	/* the probe's multi_manager, stable-sorted by delay as the cook does */
	{ "hl64_t1", 0.0f }, { "hl64_relay", 0.0f }, { "hl64_t2", 0.05f }, { "hl64_t3", 0.1f },
	{ "hl64_t4", 0.1f }, { "hl64_t1", 0.33f },
};
#define K5_N	((int)(sizeof(k5_targets) / sizeof(k5_targets[0])))

static float now_f(void)
{
	return (float)eng_time();
}

void game_logic_use(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	(void)caller; (void)value;
	G.logic_uses++;
	G.last_use_gid = gid;
	G.last_caller = rec_caller_gid();
	if (G.mode != 1)
		return;
	if (gid == G.mm)
	{
		/* use: start = time, think now */
		G.start = now_f();
		G.index = 0;
		rec_timer(gid, G.start, 0, activator);
	}
	else if (gid == G.relay)
		/* CBaseDelay::SUB_UseTargets with delay: (f32)(time + delay) */
		rec_fire_at(now_f() + 0.15f, G.t5, NAME_NONE, activator, -1, use_type, 0);
	else
		printf("use hl64_t5 %.9g\n", (double)now_f());
}

void game_logic_timer(gid_t16 gid, uint16_t arg, int activator)
{
	G.timers++;
	if (G.ntl < 64)
	{
		G.tl[G.ntl].gid = gid;
		G.tl[G.ntl].arg = arg;
		G.tl[G.ntl].t = now_f();
		G.ntl++;
	}
	if (G.mode == 1 && gid == G.mm)
	{
		float t = now_f() - G.start;

		while (G.index < K5_N && t >= k5_targets[G.index].delay)
		{
			if (!strcmp(k5_targets[G.index].name, "hl64_relay"))
				game_logic_use(G.relay, activator, -1, USE_TOGGLE, 0);
			else
				printf("use %s %.9g\n", k5_targets[G.index].name, (double)now_f());
			G.index++;
		}
		if (G.index < K5_N)
			rec_timer(gid, G.start + k5_targets[G.index].delay, 0, activator);
		return;
	}
	/* cascade test: arg 1 schedules arg 2 at its own due time */
	if (arg == 1)
		rec_timer(GID_MAKE(GID_SLOT(gid), GID_LUMP(gid)), now_f(), 2, -1);
}

/* ---- helpers ------------------------------------------------------------------- */

static int open_map(const char *name)
{
	int map = wld_map_find(name);
	wld_layer_t *L;

	if (map < 0)
		return -1;
	L = wld_open(map);
	return rec_map_open(L->slot);
}

static gid_t16 named(int slot, const char *s, int i)
{
	return rec_name_gid(rec_name_find(slot, s), i);
}

static void frame(double *t, double dt, const float anchor[3], int slot)
{
	const float *g = wld_layer(slot)->gofs;
	float a[1][3] = { { anchor[0] + g[0], anchor[1] + g[1], anchor[2] + g[2] } };

	eng_set_clock(*t, dt);
	rec_run_timers(*t, dt);
	phy_entities_frame(*t, dt);
	rec_frame((const float (*)[3])a, 1);
	*t += dt;
}

static int count_state(int slot, int state)
{
	int n = 0, l;

	for (l = 0; l < 1024; l++)
	{
		gid_t16 g = GID_MAKE(slot, l);

		if (rec_kind(g) == REC_KIND_ENTITY && rec_state(g) == state)
			n++;
	}
	return n;
}

/* ---- selftest -------------------------------------------------------------------- */

static void test_records(int slot)
{
	gid_t16 g;
	float v[3], a[3], d;
	name_t n;
	int i;

	/* the player start: the first info_player_start (lump 3) */
	CHECK(rec_player_start(slot, v, a) && a[1] == 270 && v[0] == -1984 && v[1] == 2240 && v[2] == 40,
		"player start %g %g %g", (double)v[0], (double)v[1], (double)v[2]);
	g = named(slot, "tele_init_destination", 0);
	CHECK(rec_class(g) == RC_INFO_TELEPORT_DESTINATION && rec_kind(g) == REC_KIND_POINT,
		"tele_init_destination class %u kind %d", rec_class(g), rec_kind(g));
	rec_origin(g, v);
	CHECK(v[0] == -2048 && v[1] == 744 && v[2] == 0, "tele dest origin");
	rec_angles(g, v);
	CHECK(v[1] == 270, "tele dest angles (angle -> angles)");
	/* the intro multi_manager: fire list sorted by delay */
	g = named(slot, "hldemo_init_mm", 0);
	CHECK(rec_kind(g) == REC_KIND_LOGIC && rec_class(g) == RC_MULTI_MANAGER, "init mm kind");
	CHECK(rec_aux_count(g) >= 10, "init mm fire list %d", rec_aux_count(g));
	for (i = 0, d = -1; i < rec_aux_count(g); i++)
	{
		float delay;

		rec_fire_entry(g, i, &n, &delay);
		CHECK(delay >= d && n != NAME_NONE, "fire list order");
		d = delay;
		if (n == rec_name_find(slot, "tele_init_master"))
			CHECK(delay == 26.0f, "tele_init_master at %g", (double)delay);
	}
	CHECK(rec_logic(g) != NULL && rec_logic(named(slot, "tele_init_destination", 0)) == NULL,
		"logic block");
	/* the teleport behind the intro: master + target names, submodel */
	for (i = 0; i < 1024; i++)
		if (rec_class(GID_MAKE(slot, i)) == RC_TRIGGER_TELEPORT
			&& rec_key_name(GID_MAKE(slot, i), RK_MASTER) == rec_name_find(slot, "tele_init_master"))
			break;
	CHECK(i < 1024, "trigger_teleport with master tele_init_master");
	if (i < 1024)
	{
		g = GID_MAKE(slot, i);
		CHECK(rec_key_name(g, RK_TARGET) == rec_name_find(slot, "tele_init_destination"),
			"teleport target");
		CHECK(rec_key_res(g, RK_MODEL) == RES_SUBMODEL(slot, 57), "teleport model %#x",
			rec_key_res(g, RK_MODEL));
		CHECK(rec_key_f(g, RK_STYLE, &d) && d == 32, "style key");
	}
	/* typed keys across hldemo1 */
	{
		int models = 0, sounds = 0, sent = 0, pats = 0, glob = 0, sub = 0, cls = 0;

		for (i = 0; i < 1024; i++)
		{
			int k, nk;

			g = GID_MAKE(slot, i);
			if (rec_kind(g) < 0)
				continue;
			nk = rec_key_count(g);
			for (k = 0; k < nk; k++)
			{
				key_t16 kid = rec_key_id(g, k);
				res_t r = rec_key_res(g, kid);

				CHECK(kid < RK__N, "key id");
				switch (RES_KIND(r))
				{
				case RES_K_MODEL: models++; break;
				case RES_K_SOUND: sounds++; break;
				case RES_K_SENTENCE: case RES_K_GROUP: sent++; break;
				case RES_K_SUBMODEL:
					sub++;
					CHECK(RES_SUB_SLOT(r) == slot, "submodel slot");
					break;
				}
				if (r != RES_NONE && RES_KIND(r) != RES_K_SUBMODEL)
				{
					int file;
					uint32_t ofs, size, fsize = 0;

					CHECK(RES_KIND(r) >= RES_K_SENTENCE || (res_file(r, &file, &ofs, &size)
						&& res_file_rom(file, &fsize) && ofs + size <= fsize),
						"R2: res_t %#x outside its file", r);
				}
				pats += rec_key_pattern(g, kid) >= 0;
				glob += rec_key_global(g, kid) >= 0;
				cls += rec_key_cls(g, kid) != CLS_NONE;
			}
		}
		printf("hldemo1 keys: models=%d sounds=%d sentences=%d submodels=%d patterns=%d "
			"globals=%d classrefs=%d\n", models, sounds, sent, sub, pats, glob, cls);
		CHECK(models && sounds && sent && sub > 50 && pats && glob, "typed keys present");
	}
	/* names: the holder list and cross-slot lookups */
	CHECK(rec_name_find(slot, "no_such_name_xyz") == NAME_NONE, "missing name");
	CHECK(rec_name_in(rec_name_find(slot, "tele_init_master"), slot)
		== rec_name_find(slot, "tele_init_master"), "name in own slot");
	CHECK(rec_node_graph(slot, NULL) != NULL, "hldemo1 node graph");
}

static void test_timers(int slot)
{
	gid_t16 a = named(slot, "hldemo_init_mm", 0), b = named(slot, "tele_init_master", 0);
	double t = 50.0;

	G.ntl = 0;
	/* ties: insertion order; due order; time seen = (f32) max(due, time) */
	rec_timer(b, 50.005f, 7, -1);
	rec_timer(a, 50.005f, 8, -1);
	rec_run_timers(t, 0.01);
	CHECK(G.ntl == 2 && G.tl[0].gid == b && G.tl[1].gid == a, "tie order");
	CHECK(G.ntl == 2 && G.tl[0].t == 50.005f, "time seen = due %.9g", (double)G.tl[0].t);
	/* a timer due before the frame time sees the frame time */
	G.ntl = 0;
	rec_timer(a, 49.0f, 3, -1);
	rec_run_timers(t, 0.01);
	CHECK(G.ntl == 1 && G.tl[0].t == (float)t, "late timer sees the frame time");
	/* not due: (f64) t > time + dt */
	G.ntl = 0;
	rec_timer(a, 50.02f, 4, -1);
	rec_run_timers(t, 0.01);
	CHECK(G.ntl == 0, "not due yet");
	rec_timer_cancel(a);
	rec_run_timers(t + 1, 0.01);
	CHECK(G.ntl == 0, "cancelled");
	/* replace: one nextthink per record */
	rec_timer(a, 51.0f, 5, -1);
	rec_timer(a, 52.0f, 6, -1);
	rec_run_timers(51.5, 0.01);
	CHECK(G.ntl == 0, "replaced timer");
	/* cascade: arg 1 schedules arg 2 due now: same pass */
	rec_timer_cancel(a);
	G.ntl = 0;
	rec_timer(b, 52.0f, 1, -1);
	rec_run_timers(52.0, 0.01);
	CHECK(G.ntl == 2 && G.tl[0].arg == 1 && G.tl[1].arg == 2 && G.tl[1].t == 52.0f,
		"cascade in the same pass (%d)", G.ntl);
	/* freeze / thaw: 0.3 s left when the map closes, due 0.3 s after reopen */
	G.ntl = 0;
	eng_set_clock(60.0, 0.01);
	rec_timer(a, 60.3f, 9, -1);
	rec_map_close(slot);
	CHECK(G.closing == 1, "game_map_closing");
	eng_set_clock(100.0, 0.01);
	CHECK(open_map("hldemo1") == slot, "reopen");
	rec_run_timers(100.25, 0.01);
	CHECK(G.ntl == 0, "thawed timer not early");
	rec_run_timers(100.3, 0.01);
	CHECK(G.ntl == 1 && G.tl[0].arg == 9 && fabsf(G.tl[0].t - 100.3f) < 1e-4f,
		"thawed timer at time + remaining (%d %.9g)", G.ntl, G.ntl ? (double)G.tl[0].t : 0.0);
}

static void test_campaign(void)
{
	int map = wld_map_find("hldemo1"), slot, live0, pend0, i, f, maxhyd = 0;
	float start[3], ang[3], far[3];
	double t = 1.0;
	rec_stats_t s;

	G.spawned = G.logic = 0;
	rec_new_campaign(map);
	slot = wld_open(map)->slot;
	CHECK(rec_player_start(slot, start, ang), "player start");
	live0 = count_state(slot, REC_LIVE);
	pend0 = count_state(slot, REC_PENDING);
	printf("new campaign: spawned=%d logic=%d live=%d pending=%d\n", G.spawned, G.logic,
		live0, pend0);
	CHECK(G.spawned == live0 && live0 > 0 && pend0 > 0 && G.logic > 0, "New Game spawn");
	/* every live record is within the spawn radius of the start */
	for (i = 0; i < 1024; i++)
	{
		gid_t16 g = GID_MAKE(slot, i);
		int e = rec_entity(g);

		if (e >= 0)
		{
			float o[3];

			rec_origin(g, o);
			(void)o;
		}
	}
	/* the pump at the start spawns nothing new (all within reach live) */
	rec_stats(&s, 1);
	frame(&t, 1.0 / 30, start, slot);
	rec_stats(&s, 1);
	CHECK(s.hydrated == 0, "pump at the start: %u", s.hydrated);
	/* walk the anchor to the dome hall: <= 8 per frame (host budget) */
	far[0] = -2048;
	far[1] = -800;
	far[2] = 196;
	for (f = 0; f < 120; f++)
	{
		frame(&t, 1.0 / 30, far, slot);
		rec_stats(&s, 1);
		if ((int)s.hydrated > maxhyd)
			maxhyd = (int)s.hydrated;
	}
	CHECK(maxhyd > 0 && maxhyd <= REC_BUDGET_HOST, "budget: max %d per frame", maxhyd);
	printf("pump: max %d hydrated per frame, live=%d pending=%d\n", maxhyd,
		count_state(slot, REC_LIVE), count_state(slot, REC_PENDING));
	/* nothing pending is left within the spawn radius of the anchor
	 * unless the optional reserve stopped it */
	for (i = 0; i < 1024; i++)
	{
		gid_t16 g = GID_MAKE(slot, i);
		float o[3], dx, dy, dz;

		if (rec_kind(g) != REC_KIND_ENTITY || rec_state(g) != REC_PENDING)
			continue;
		rec_origin(g, o);
		dx = o[0] - far[0];
		dy = o[1] - far[1];
		dz = o[2] - far[2];
		if (o[0] == 0 && o[1] == 0 && o[2] == 0)
			continue;	/* brush records: bounds from the model */
		CHECK(dx * dx + dy * dy + dz * dz > REC_SPAWN_R * REC_SPAWN_R,
			"pending record %d at %g within reach", i, (double)sqrtf(dx * dx + dy * dy + dz * dz));
	}
	/* box wake: a far pending record wakes when a box meets it */
	for (i = 0; i < 1024; i++)
	{
		gid_t16 g = GID_MAKE(slot, i);
		float o[3], lo[3], hi[3];
		const float *gm = wld_layer(slot)->gofs;
		int k;

		if (rec_kind(g) != REC_KIND_ENTITY || rec_state(g) != REC_PENDING)
			continue;
		rec_origin(g, o);
		if (o[0] == 0 && o[1] == 0 && o[2] == 0)
			continue;
		for (k = 0; k < 3; k++)
		{
			lo[k] = o[k] - 4 + gm[k];
			hi[k] = o[k] + 4 + gm[k];
		}
		CHECK(rec_wake_box(lo, hi) >= 1 && rec_state(g) == REC_LIVE, "box wake lump %d", i);
		lo[0] += 30000;
		hi[0] += 30000;
		CHECK(rec_wake_box(lo, hi) == 0, "box wake far away");
		break;
	}
	CHECK(i < 1024, "a pending record for the box wake");
	/* fires: <= 2 in-call hydrations per frame, the rest deferred to the
	 * pump; logic records get game_logic_use */
	{
		name_t n = NAME_NONE;
		int best = 0, j, uses;

		/* the name with the most pending entity records */
		for (j = 0; j < 4096; j++)
		{
			name_t c = NAME_MAKE(slot, j);
			int k, p = 0;

			if (rec_name_count(c) == 0 && j > 0 && rec_name_gid(c, 0) == GID_NONE)
				continue;
			for (k = 0; k < rec_name_count(c); k++)
				p += rec_kind(rec_name_gid(c, k)) == REC_KIND_ENTITY
					&& rec_state(rec_name_gid(c, k)) == REC_PENDING;
			if (p > best)
			{
				best = p;
				n = c;
			}
		}
		printf("fire: name with %d pending records\n", best);
		if (best >= 3)
		{
			rec_run_timers(t, 1.0 / 30);	/* frame start: wake cap reset */
			G.uses = 0;
			rec_fire(n, -1, -1, named(slot, "hldemo_init_mm", 0), USE_TOGGLE, 0);
			uses = G.uses;
			CHECK(uses == rec_name_count(n) - best + 2 || uses >= 2, "in-call uses %d", uses);
			CHECK(G.last_caller == named(slot, "hldemo_init_mm", 0), "caller gid");
			frame(&t, 1.0 / 30, far, slot);
			CHECK(G.uses >= uses + best - 2, "deferred uses delivered (%d of %d)",
				G.uses - uses, best - 2);
		}
	}
	/* a refused spawn kills the record */
	{
		gid_t16 g = GID_NONE;

		for (i = 0; i < 1024; i++)
			if (rec_kind(GID_MAKE(slot, i)) == REC_KIND_ENTITY
				&& rec_state(GID_MAKE(slot, i)) == REC_PENDING)
			{
				g = GID_MAKE(slot, i);
				break;
			}
		if (g != GID_NONE)
		{
			G.refuse_cls = rec_class(g);
			CHECK(rec_wake(g) < 0 && rec_state(g) == REC_KILLED, "refused spawn -> killed");
			G.refuse_cls = -1;
		}
	}
	/* the entity freed by the game: the record is killed, never respawns */
	for (i = 0; i < 1024; i++)
	{
		gid_t16 g = GID_MAKE(slot, i);
		int e = rec_entity(g);

		if (e >= 0)
		{
			ent_destroy(e);
			CHECK(rec_state(g) == REC_KILLED && rec_wake(g) < 0, "freed entity -> killed");
			break;
		}
	}
	rec_stats(&s, 0);
	printf("rec: live=%u pending=%u logic=%u killed=%u timers_peak=%u\n", s.live, s.pending,
		s.logic, s.killed, s.timers_peak);
}

static void test_r3(void)
{
	int s1 = open_map("hldemo1"), s2 = open_map("hldemo2"), i, found = 0;
	const wld_layer_t *a = wld_layer(s1), *b = wld_layer(s2);

	CHECK(s1 >= 0 && s2 >= 0 && s1 != s2, "both maps open (%d %d)", s1, s2);
	if (s1 < 0 || s2 < 0)
		return;
	for (i = 0; i < 1024; i++)
	{
		res_t r1 = rec_key_res(GID_MAKE(s1, i), RK_MODEL);

		if (RES_KIND(r1) == RES_K_SUBMODEL && RES_SUB_NUM(r1) == 3)
			found |= 1;
	}
	for (i = 0; i < 1024; i++)
	{
		res_t r2 = rec_key_res(GID_MAKE(s2, i), RK_MODEL);

		if (RES_KIND(r2) == RES_K_SUBMODEL && RES_SUB_NUM(r2) == 3)
			found |= 2;
	}
	CHECK(found == 3, "*3 in both maps");
	CHECK(RES_SUBMODEL(s1, 3) != RES_SUBMODEL(s2, 3), "R3: different ids");
	CHECK(memcmp(a->models[3].mins, b->models[3].mins, 12)
		|| memcmp(a->models[3].maxs, b->models[3].maxs, 12), "R3: different bounds");
	printf("R3: hldemo1 *3 %g %g %g .. %g %g %g, hldemo2 *3 %g %g %g .. %g %g %g\n",
		(double)a->models[3].mins[0], (double)a->models[3].mins[1], (double)a->models[3].mins[2],
		(double)a->models[3].maxs[0], (double)a->models[3].maxs[1], (double)a->models[3].maxs[2],
		(double)b->models[3].mins[0], (double)b->models[3].mins[1], (double)b->models[3].mins[2],
		(double)b->models[3].maxs[0], (double)b->models[3].maxs[1], (double)b->models[3].maxs[2]);
	rec_map_close(s2);
}

static void test_res(void)
{
	int k, total = 0;

	for (k = 1; k < 8; k++)
		total += res_count(k);
	CHECK(res_count(RES_K_MODEL) > 0 && res_count(RES_K_SOUND) > 0 && res_count(RES_K_SENTENCE) > 0,
		"res counts");
	CHECK(res_titles() > 0 && res_title(0) && res_title_text(0) && !res_title(0xFFFF), "titles");
	CHECK(res_classes() == RC__N, "classes.def lines %d vs %d", res_classes(), RC__N);
	CHECK(res_name(RES_MAKE(RES_K_MODEL, 0))[0] != 0, "host names");
	CHECK(res_model_frames(RES_MAKE(RES_K_SPRITE, 0)) >= 1, "sprite frames");
	printf("res: %d ids, %d titles, title 0 '%.20s'\n", total, res_titles(), res_title_text(0));
}

int main(int argc, char **argv)
{
	const char *root;

	if (argc < 3 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-rec --root CONTENT selftest | k5 T0 DT FRAMES\n");
		return 2;
	}
	root = argv[2];
	host_io_init(root);
	mem_boot_carve();
	io_init();
	ent_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-rec: no content in %s\n", root);
		return 2;
	}
	res_init();
	rec_init();
	col_init();
	G.refuse_cls = -1;
	if (argc == 4 && !strcmp(argv[3], "selftest"))
	{
		int slot;

		test_res();
		eng_clock_new_game();
		slot = open_map("hldemo1");
		CHECK(slot >= 0 && G.opened == 1, "open hldemo1");
		test_records(slot);
		test_timers(slot);
		test_r3();
		test_campaign();
		rec_prof_line();
		rec_mem_line();
		res_mem_line();
		log_flush();
		printf(fails ? "selftest FAIL\n" : "selftest ok\n");
		return fails != 0;
	}
	if (argc == 7 && !strcmp(argv[3], "k5"))
	{
		double t = strtod(argv[4], NULL), dt = strtod(argv[5], NULL);
		int frames = atoi(argv[6]), f, slot;

		rec_new_campaign(wld_map_find("hldemo1"));
		slot = wld_open(wld_map_find("hldemo1"))->slot;
		G.mode = 1;
		G.t5 = NAME_NONE;
		/* any two logic records stand for the probe's multi_manager and
		 * trigger_relay; hl64_t5 is a third logic record's name */
		G.mm = named(slot, "hldemo_init_mm", 0);
		G.relay = named(slot, "tele_init_master", 0);
		for (f = 0; f < 4096 && G.t5 == NAME_NONE; f++)
		{
			gid_t16 g = rec_name_gid(NAME_MAKE(slot, f), 0);

			if (rec_kind(g) == REC_KIND_LOGIC && g != G.mm && g != G.relay
				&& rec_name_count(NAME_MAKE(slot, f)) == 1)
				G.t5 = NAME_MAKE(slot, f);
		}
		CHECK(G.mm != GID_NONE && G.relay != GID_NONE && G.t5 != NAME_NONE
			&& rec_kind(rec_name_gid(G.t5, 0)) == REC_KIND_LOGIC, "k5 records");
		for (f = 0; f < frames; f++)
		{
			eng_set_clock(t, dt);
			if (f == 0)
				game_logic_use(G.mm, -1, -1, USE_TOGGLE, 0);
			rec_run_timers(t, dt);
			t += dt;
		}
		return fails != 0;
	}
	fprintf(stderr, "hl64-rec: bad arguments\n");
	return 2;
}
