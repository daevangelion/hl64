/* SPDX-License-Identifier: MIT */
/*
 * rec4_test.c -- host driver for the M4 level state of the record module
 * (engine/ent/rec.c; docs/architecture.md 4.1 "HLE slots for M4", 6.4, 14;
 * docs/specs/think-and-time.md 5): the real engine (carve, io, world
 * layers, components) on cooked content, with a test game standing in for
 * the record dispatch and GameLS's per-class delta (ld --wrap, Makefile
 * REC_WRAP + game_spawn_class). Run by test/m4/rec.sh.
 *
 *   hl64-rec4 --root CONTENT selftest [DELTA_BYTES]
 *       A1 async open (OPEN-priority reads, ready only when the chunk
 *       landed, nothing hydrates before, no sync read), A2 release by
 *       distance in 0.1 s batches + rehydrate round trip (the delta comes
 *       back), refusal retried after 0.5 s, pristine deltas store nothing,
 *       rec_release, A3 close -> frozen runtime -> reopen (record states,
 *       logic blocks, timers thaw with their remaining time), A4 twin
 *       aliases, A5 deferred uses into a closed map replayed in order on
 *       reopen (activator of the closed map absent, caller canonical), A6
 *       runtime ids (pickup released / rehydrated by the pump and across a
 *       close, held name, 64 cap, rec_rehome), A7 store sizing (every
 *       physical record of hldemo1 released with DELTA_BYTES deltas);
 *       prints "selftest ok" or FAIL lines, then prof / mem / store lines
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

int	__wrap_game_spawn_record(gid_t16 gid, int e);
int	__wrap_game_spawn_class(cls_t cls, int e);
void	__wrap_game_spawn_logic(gid_t16 gid);
void	__wrap_game_map_opened(int slot);
void	__wrap_game_map_closing(int slot);
void	__wrap_game_use(int e, int activator, int caller, int use_type, float value);
void	__wrap_game_logic_use(gid_t16 gid, int activator, int caller, int use_type, float value);
void	__wrap_game_logic_timer(gid_t16 gid, uint16_t arg, int activator);
uint32_t __wrap_game_dehydrate(int e, void *buf, uint32_t max);
void	__wrap_game_rehydrate(int e, const void *buf, uint32_t n);
void	__wrap_game_cut_refs(int slot);
int	__wrap_wld_layer_ready(const wld_layer_t *L);
wld_layer_t *__wrap_wld_layer_of_map(int map);

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT	(1.0 / 30)

/* ---- the test game ------------------------------------------------------------ */

typedef struct {
	uint8_t		ver;		/* 1 (0 = pristine, 1 byte) */
	uint8_t		pad;
	uint16_t	gid;
	uint16_t	rtid;
	uint16_t	pad2;
	float		org[3];
	int32_t		val;
} delta_t;

static struct {
	int		spawned, rt_spawned, logic, opened, closing, cut, uses, rehydrated, dehyd_calls;
	int		layer_ready;		/* wld_layer_ready answer */
	int		pristine;		/* val 0 entities answer pristine */
	uint32_t	delta;			/* delta bytes (>= sizeof(delta_t)) */
	gid_t16		refuse;			/* this record refuses */
	int		refuse_n;
	double		refuse_t[8];		/* when it was asked */
	int32_t		val[HL64_MAX_ENTITIES];	/* per-entity test state */
	gid_t16		last_use_gid;
	gid_t16		last_caller;
	int		last_act;
	int		nlog;
	gid_t16		ulog[64];
	int		alog[64];
	cls_t		last_cls;
	int		ntl;
	struct { gid_t16 gid; uint16_t arg; float t; } tl[16];
} G;

int __wrap_game_spawn_record(gid_t16 gid, int e)
{
	res_t m = rec_key_res(gid, RK_MODEL);

	/* brush records get their model's box (the game's SET_MODEL) */
	if (m != RES_NONE && RES_KIND(m) == RES_K_SUBMODEL)
		ent_set_model(e, m);
	ent_set_origin(e, ent_cv(e)->origin);
	G.val[e] = 0;
	G.spawned++;
	return 0;
}

int __wrap_game_spawn_class(cls_t cls, int e)
{
	ent_set_origin(e, ent_cv(e)->origin);
	G.val[e] = 0;
	G.last_cls = cls;
	G.rt_spawned++;
	return 0;
}

void __wrap_game_spawn_logic(gid_t16 gid)
{
	(void)gid;
	G.logic++;
}

void __wrap_game_map_opened(int slot)
{
	(void)slot;
	G.opened++;
}

void __wrap_game_map_closing(int slot)
{
	(void)slot;
	G.closing++;
}

void __wrap_game_cut_refs(int slot)
{
	(void)slot;
	G.cut++;
}

void __wrap_game_use(int e, int activator, int caller, int use_type, float value)
{
	(void)caller; (void)use_type; (void)value;
	G.uses++;
	G.last_use_gid = ent_idc(e)->gid;
	G.last_caller = rec_caller_gid();
	G.last_act = activator;
	if (G.nlog < 64)
	{
		G.ulog[G.nlog] = ent_idc(e)->gid;
		G.alog[G.nlog++] = activator;
	}
}

void __wrap_game_logic_use(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	(void)gid; (void)activator; (void)caller; (void)use_type; (void)value;
}

void __wrap_game_logic_timer(gid_t16 gid, uint16_t arg, int activator)
{
	(void)activator;
	if (G.ntl < 16)
	{
		G.tl[G.ntl].gid = gid;
		G.tl[G.ntl].arg = arg;
		G.tl[G.ntl].t = (float)eng_time();
		G.ntl++;
	}
}

uint32_t __wrap_game_dehydrate(int e, void *buf, uint32_t max)
{
	delta_t d;
	const ent_id_t *id = ent_idc(e);

	G.dehyd_calls++;
	if (id->gid != GID_NONE && id->gid == G.refuse)
	{
		if (G.refuse_n < 8)
			G.refuse_t[G.refuse_n] = eng_time();
		G.refuse_n++;
		return 0;
	}
	if (G.pristine && id->gid != GID_NONE && G.val[e] == 0 && max >= 1)
	{
		((uint8_t *)buf)[0] = 0;
		return 1;
	}
	if (max < G.delta)
		return 0;
	memset(buf, 0, G.delta);
	memset(&d, 0, sizeof(d));
	d.ver = 1;
	d.gid = id->gid;
	d.rtid = id->rtid;
	memcpy(d.org, ent_cv(e)->origin, sizeof(d.org));
	d.val = G.val[e];
	memcpy(buf, &d, sizeof(d));
	return G.delta;
}

void __wrap_game_rehydrate(int e, const void *buf, uint32_t n)
{
	delta_t d;

	if (n < sizeof(d))
	{
		CHECK(n == 1 && ((const uint8_t *)buf)[0] == 0, "rehydrate %u B", n);
		return;
	}
	memcpy(&d, buf, sizeof(d));
	CHECK(d.ver == 1 && d.gid == ent_idc(e)->gid && d.rtid == ent_idc(e)->rtid,
		"delta of another entity (gid %u/%u rtid %u/%u)", d.gid, ent_idc(e)->gid, d.rtid,
		ent_idc(e)->rtid);
	ent_set_origin(e, d.org);
	G.val[e] = d.val;
	G.rehydrated++;
}

int __wrap_wld_layer_ready(const wld_layer_t *L)
{
	return L ? G.layer_ready : 0;
}

wld_layer_t *__wrap_wld_layer_of_map(int map)
{
	int s;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (wld_layer(s) && wld_layer(s)->map == map)
			return wld_layer(s);
	return NULL;
}

/* ---- helpers ------------------------------------------------------------------- */

static double T = 1.0;

/* one frame around global anchors */
static void frame_g(const float (*ga)[3], int n)
{
	int k;

	io_frame_begin(33333);
	for (k = 0; k < 4; k++)
		io_pump();
	eng_set_clock(T, DT);
	rec_run_timers(T, DT);
	phy_entities_frame(T, DT);
	rec_frame(ga, n);
	T += DT;
}

/* one frame: io (4 pieces), timers, entities, step 8 around the anchors
 * (map-local points of `slot`; NULL: no anchors) */
static void frame_at(int slot, const float (*loc)[3], int n)
{
	float ga[4][3];
	int a, k;

	for (a = 0; a < n; a++)
		for (k = 0; k < 3; k++)
			ga[a][k] = loc[a][k] + wld_layer(slot)->gofs[k];
	io_frame_begin(33333);
	for (k = 0; k < 4; k++)
		io_pump();
	eng_set_clock(T, DT);
	rec_run_timers(T, DT);
	phy_entities_frame(T, DT);
	rec_frame((const float (*)[3])ga, n);
	T += DT;
}

static void frame1(int slot, float x, float y, float z)
{
	float a[1][3] = { { x, y, z } };

	frame_at(slot, (const float (*)[3])a, 1);
}

static int open_sync(const char *name)
{
	wld_layer_t *L = wld_open(wld_map_find(name));

	G.layer_ready = 1;
	rec_map_finish(rec_map_open(L->slot));
	return rec_map_ready(L->slot) ? L->slot : -1;
}

/* async reopen of an open layer's HLE slot: frames until ready */
static int reopen_async(int slot, int *frames, io_stats_t *io)
{
	int f;

	io_stats(io, 1);
	CHECK(rec_map_open(slot) == slot, "rec_map_open");
	for (f = 0; f < 120 && !rec_map_ready(slot); f++)
	{
		CHECK(rec_kind(GID_MAKE(slot, 1)) < 0 && rec_wake(GID_MAKE(slot, 1)) < 0,
			"records before ready");
		frame_at(slot, NULL, 0);
	}
	io_frame_begin(33333);
	io_stats(io, 0);
	*frames = f;
	return rec_map_ready(slot);
}

static int count_state(int slot, int state)
{
	int n = 0, l;

	for (l = 0; l < 1024; l++)
		if (rec_kind(GID_MAKE(slot, l)) == REC_KIND_ENTITY && rec_state(GID_MAKE(slot, l)) == state)
			n++;
	return n;
}

static gid_t16 named(int slot, const char *s, int i)
{
	return rec_name_gid(rec_name_find(slot, s), i);
}

static uint16_t map_of(int slot)
{
	return (uint16_t)wld_layer(slot)->map;
}

/* ---- A1 async open ---------------------------------------------------------------- */

static void test_async_open(int slot)
{
	io_stats_t io;
	int f, i;

	rec_map_close(slot);
	/* the layer not ready yet: the slot waits, nothing queued */
	G.layer_ready = 0;
	CHECK(rec_map_open(slot) == slot, "open while the layer opens");
	for (i = 0; i < 5; i++)
		frame_at(slot, NULL, 0);
	CHECK(!rec_map_ready(slot) && io_queued() == 0, "waits for its layer (queued %u)", io_queued());
	G.layer_ready = 1;
	rec_map_close(slot);	/* an opening slot closes cleanly */
	CHECK(!rec_map_ready(slot), "aborted open");
	CHECK(reopen_async(slot, &f, &io), "async open never ready");
	CHECK(f >= 1, "ready in the call (%d frames)", f);
	CHECK(io.sync_calls == 0 && io.frames_blocked_1ms == 0 && io.async_bytes >= 52000,
		"async open: sync %u blocked1ms %u async %llu", io.sync_calls, io.frames_blocked_1ms,
		(unsigned long long)io.async_bytes);
	printf("A1 async open: ready after %d frames, %llu B async, %u sync reads, %u frames > 1 ms blocked\n",
		f, (unsigned long long)io.async_bytes, io.sync_calls, io.frames_blocked_1ms);
}

/* ---- A2 release / rehydrate ------------------------------------------------------- */

static void test_release(int slot, const float start[3])
{
	gid_t16 g[64];
	int ng = 0, i, f, e, last_batch = -1, maxb = 0, bad_gap = 0;
	double tb = -1;
	float far[3] = { start[0], start[1], start[2] + 20000 };
	float org[64][3];
	rec_stats_t s;

	/* the live records near the start, each with its own state */
	for (i = 0; i < 1024 && ng < 64; i++)
		if ((e = rec_entity(GID_MAKE(slot, i))) >= 0)
		{
			g[ng] = GID_MAKE(slot, i);
			G.val[e] = 1000 + i;
			memcpy(org[ng], ent_cv(e)->origin, sizeof(org[ng]));
			ng++;
		}
	CHECK(ng > 0, "live records at the start");
	rec_stats(&s, 1);
	/* every anchor far away: batches every 0.1 s */
	for (f = 0; f < 60; f++)
	{
		frame1(slot, far[0], far[1], far[2]);
		rec_stats(&s, 1);
		if (s.released)
		{
			if (tb >= 0 && T - DT - tb < 0.1 - 1e-6)
				bad_gap++;
			tb = T - DT;
			last_batch = f;
			if ((int)s.released > maxb)
				maxb = (int)s.released;
		}
	}
	CHECK(!bad_gap, "release batches closer than 0.1 s (%d)", bad_gap);
	CHECK(maxb >= 1 && maxb <= REC_BUDGET_HOST, "batch size %d", maxb);
	for (i = 0; i < ng; i++)
		CHECK(rec_state(g[i]) == REC_DRY, "lump %d not dry (%d)", GID_LUMP(g[i]), rec_state(g[i]));
	rec_stats(&s, 0);
	CHECK(s.released_total >= (uint32_t)ng && s.store_used > 0 && rec_slot_live(slot) == 0,
		"released %u store %u live %d", s.released_total, s.store_used, rec_slot_live(slot));
	printf("A2 release: %d records dry in batches (max %d per batch, last at frame %d), store %u B\n",
		ng, maxb, last_batch, s.store_used);
	/* back at the start: the pump rehydrates them with their deltas */
	G.rehydrated = 0;
	for (f = 0; f < 30; f++)
		frame1(slot, start[0], start[1], start[2]);
	for (i = 0; i < ng; i++)
	{
		e = rec_entity(g[i]);
		CHECK(e >= 0 && G.val[e] == 1000 + (int)GID_LUMP(g[i])
			&& !memcmp(ent_cv(e)->origin, org[i], sizeof(org[i])),
			"lump %d round trip (e %d val %d)", GID_LUMP(g[i]), e, e >= 0 ? G.val[e] : -1);
	}
	rec_stats(&s, 0);
	CHECK(G.rehydrated == ng && s.store_used == 0 && s.dry == 0, "rehydrated %d of %d, store %u",
		G.rehydrated, ng, s.store_used);
	/* a refusal is asked again 0.5 s later */
	G.refuse = g[0];
	G.refuse_n = 0;
	for (f = 0; f < 50; f++)
		frame1(slot, far[0], far[1], far[2]);
	CHECK(rec_state(g[0]) == REC_LIVE && G.refuse_n >= 2, "refused record (%d asks)", G.refuse_n);
	for (i = 1; i < G.refuse_n && i < 8; i++)
		CHECK(G.refuse_t[i] - G.refuse_t[i - 1] >= REC_RELEASE_RETRY - 1e-6
			&& G.refuse_t[i] - G.refuse_t[i - 1] < REC_RELEASE_RETRY + 0.2,
			"retry gap %g", G.refuse_t[i] - G.refuse_t[i - 1]);
	G.refuse = GID_NONE;
	for (f = 0; f < 20; f++)
		frame1(slot, far[0], far[1], far[2]);
	CHECK(rec_state(g[0]) == REC_DRY, "released once it agrees");
	/* back; pristine deltas store nothing and respawn from the keys */
	for (f = 0; f < 30; f++)
		frame1(slot, start[0], start[1], start[2]);
	G.pristine = 1;
	for (i = 0; i < ng; i++)
		if ((e = rec_entity(g[i])) >= 0)
			G.val[e] = 0;
	for (f = 0; f < 30; f++)
		frame1(slot, far[0], far[1], far[2]);
	rec_stats(&s, 0);
	CHECK(count_state(slot, REC_DRY) == 0 && s.store_used == 0 && rec_state(g[0]) == REC_PENDING,
		"pristine: dry %d store %u", count_state(slot, REC_DRY), s.store_used);
	G.pristine = 0;
	for (f = 0; f < 30; f++)
		frame1(slot, start[0], start[1], start[2]);
	/* rec_release: now, one entity */
	e = rec_entity(g[0]);
	CHECK(e >= 0 && rec_release(e) == 1 && rec_state(g[0]) == REC_DRY, "rec_release");
	CHECK(rec_wake(g[0]) >= 0, "explicit wake of a dry record");
	printf("A2 refusal: %d asks, gaps >= %.2f s; pristine and rec_release ok\n", G.refuse_n,
		REC_RELEASE_RETRY);
}

/* ---- A3 close / frozen / reopen ---------------------------------------------------- */

static void test_frozen(int slot, const float start[3])
{
	gid_t16 lg = named(slot, "hldemo_init_mm", 0), dryg = GID_NONE, killg = GID_NONE;
	int i, e, f, nf, logic0;
	uint32_t hle0, hle1, store0;
	io_stats_t io;
	double tclose, topen;
	float far[3] = { start[0], start[1], start[2] + 20000 };

	CHECK(lg != GID_NONE && rec_logic(lg), "logic record");
	rec_logic(lg)->a = 77;
	/* one record dry with a state, one killed */
	for (i = 0; i < 1024 && (dryg == GID_NONE || killg == GID_NONE); i++)
	{
		gid_t16 g = GID_MAKE(slot, i);

		if (dryg == GID_NONE && (e = rec_entity(g)) >= 0)
		{
			G.val[e] = 4711;
			CHECK(rec_release(e), "release");
			dryg = g;
		}
		else if (killg == GID_NONE && rec_kind(g) == REC_KIND_ENTITY && rec_state(g) == REC_PENDING)
		{
			rec_kill(g);
			killg = g;
		}
	}
	tclose = T;
	eng_set_clock(T, DT);
	rec_timer(lg, (float)(T + 0.3), 9, -1);
	G.cut = G.closing = 0;
	rec_mem_use(&hle0, &store0);
	rec_map_close(slot);
	rec_mem_use(&hle1, NULL);
	CHECK(G.cut == 1 && G.closing == 1, "game_cut_refs / closing");
	CHECK(rec_slot_live(slot) == 0 && hle1 < hle0 && hle1 > REC_TABLE_BYTES, "frozen: hle %u -> %u",
		hle0, hle1);
	rec_mem_line();
	/* time passes while it is closed */
	for (f = 0; f < 30; f++)
		frame1(slot, far[0], far[1], far[2]);
	logic0 = G.logic;
	G.ntl = 0;
	CHECK(reopen_async(slot, &nf, &io), "reopen");
	topen = T;
	CHECK(rec_logic(lg) && rec_logic(lg)->a == 77, "logic block thawed");
	CHECK(rec_state(killg) == REC_KILLED && rec_state(dryg) == REC_DRY, "record states thawed");
	frame_at(slot, NULL, 0);	/* logic_pending spawns run in rec_frame */
	CHECK(G.logic == logic0, "live logic records not respawned (%d)", G.logic - logic0);
	/* the timer: 0.3 s left at close, due 0.3 s after reopen */
	eng_set_clock(topen + 0.28, DT);
	rec_run_timers(topen + 0.28 - DT, 0.01);
	CHECK(G.ntl == 0, "thawed timer early");
	rec_run_timers(topen + 0.3, 0.01);
	CHECK(G.ntl == 1 && G.tl[0].arg == 9 && fabs(G.tl[0].t - (topen + 0.3)) < 1e-3,
		"thawed timer at reopen + remaining (%d %.4f vs %.4f, closed %.4f)", G.ntl,
		G.ntl ? (double)G.tl[0].t : 0.0, topen + 0.3, tclose);
	/* the dry record comes back with its delta */
	for (f = 0; f < 30; f++)
		frame1(slot, start[0], start[1], start[2]);
	e = rec_entity(dryg);
	CHECK(e >= 0 && G.val[e] == 4711, "dry record across close (%d)", e >= 0 ? G.val[e] : -1);
	printf("A3 frozen: hle %u B open -> %u B frozen, reopen in %d frames, timer due at +%.3f s\n",
		hle0, hle1, nf, G.ntl ? (double)G.tl[0].t - topen : -1.0);
}

/* ---- A4 twins ---------------------------------------------------------------------- */

/* map-local centre of a record (its brush model's box for brush records) */
static void rec_centre(gid_t16 g, float out[3])
{
	res_t m = rec_key_res(g, RK_MODEL);
	const wld_layer_t *L = wld_layer(GID_SLOT(g));
	int k;

	rec_origin(g, out);
	if (m != RES_NONE && RES_KIND(m) == RES_K_SUBMODEL)
		for (k = 0; k < 3; k++)
			out[k] += 0.5f * (L->models[RES_SUB_NUM(m)].mins[k] + L->models[RES_SUB_NUM(m)].maxs[k]);
}

/* global anchors at the centres of two records */
static void both(gid_t16 a, gid_t16 b, float ga[2][3])
{
	int k;

	rec_centre(a, ga[0]);
	rec_centre(b, ga[1]);
	for (k = 0; k < 3; k++)
	{
		ga[0][k] += wld_layer(GID_SLOT(a))->gofs[k];
		ga[1][k] += wld_layer(GID_SLOT(b))->gofs[k];
	}
}

static int logged(gid_t16 g)
{
	int i;

	for (i = 0; i < G.nlog; i++)
		if (G.ulog[i] == g)
			return 1;
	return 0;
}

static void test_twins(int sa, int sb)
{
	/* cooked hldemo1to2 twins (c6.stage/seams.json): the startele1 doors */
	gid_t16 a[2] = { GID_MAKE(sa, 79), GID_MAKE(sa, 80) }, b[2] = { GID_MAKE(sb, 89), GID_MAKE(sb, 90) };
	name_t na = rec_name_find(sa, "startele1"), nb = rec_name_find(sb, "startele1");
	float ga[2][3];
	int ea[2], i, f;

	for (i = 0; i < 2; i++)
	{
		CHECK(rec_class(a[i]) == rec_class(b[i]) && rec_targetname(a[i]) == na
			&& rec_targetname(b[i]) == nb && na != NAME_NONE && nb != NAME_NONE,
			"cooked twin %d", i);
		ea[i] = rec_wake(a[i]);
		CHECK(ea[i] >= 0 && rec_state(b[i]) == REC_PENDING, "a live, b pending");
		rec_twin_alias(a[i], b[i]);
		CHECK(rec_twin_of(a[i]) == b[i] && rec_twin_of(b[i]) == a[i], "rec_twin_of");
	}
	/* a fire in B's slot reaches the live copies (A's) */
	rec_run_timers(T, DT);
	G.nlog = 0;
	rec_fire(nb, -1, -1, GID_NONE, USE_TOGGLE, 0);
	CHECK(G.nlog == 2 && logged(a[0]) && logged(a[1]) && rec_state(b[0]) == REC_PENDING,
		"B fire -> A copies (%d uses)", G.nlog);
	/* the pump at b does not hydrate the non-live copy (anchors at both
	 * copies, as the seam's player + mapped player) */
	both(a[0], b[0], ga);
	for (f = 0; f < 5; f++)
		frame_g((const float (*)[3])ga, 2);
	CHECK(rec_state(b[0]) == REC_PENDING && rec_state(b[1]) == REC_PENDING,
		"twin copies blocked from the pump");
	/* the seam's handoff: wake b, retire a */
	for (i = 0; i < 2; i++)
		CHECK(rec_wake(b[i]) >= 0 && rec_release(ea[i]) == 1 && rec_state(a[i]) == REC_DRY,
			"handoff %d", i);
	rec_run_timers(T, DT);
	G.nlog = 0;
	rec_fire(na, -1, -1, GID_NONE, USE_TOGGLE, 0);
	CHECK(G.nlog == 2 && logged(b[0]) && logged(b[1]) && rec_state(a[0]) == REC_DRY,
		"A fire -> B copies (%d uses)", G.nlog);
	for (f = 0; f < 5; f++)
		frame_g((const float (*)[3])ga, 2);
	CHECK(rec_state(a[0]) == REC_DRY && rec_state(a[1]) == REC_DRY,
		"retired copies stay dry under the pump");
	rec_twin_clear(sa);
	rec_twin_clear(sa);
	CHECK(rec_twin_of(a[0]) == GID_NONE && rec_twin_of(b[1]) == GID_NONE, "twin clear");
	printf("A4 twins: fires in either slot reach the live copy; pump blocked; cleared\n");
}

/* ---- A5 deferred uses ------------------------------------------------------------- */

static void test_deferred(int sb)
{
	name_t n = NAME_NONE;
	int best = 0, j, k, f, uses0, act;
	gid_t16 caller = GID_NONE, glist[32];
	io_stats_t io;

	/* the name of B with the most pending entity records */
	for (j = 0; j < 4096; j++)
	{
		name_t c = NAME_MAKE(sb, j);
		int p = 0;

		for (k = 0; k < rec_name_count(c); k++)
			p += rec_kind(rec_name_gid(c, k)) == REC_KIND_ENTITY
				&& rec_state(rec_name_gid(c, k)) == REC_PENDING;
		if (p > best && p <= 32 && rec_name_count(c) == p)
		{
			best = p;
			n = c;
		}
	}
	CHECK(best >= 3, "a name with >= 3 pending records (%d)", best);
	if (best < 3)
		return;
	for (k = 0; k < best; k++)
		glist[k] = rec_name_gid(n, k);
	for (j = 0; j < 1024 && caller == GID_NONE; j++)
		if (rec_kind(GID_MAKE(sb, j)) == REC_KIND_LOGIC)
			caller = GID_MAKE(sb, j);
	/* an activator of map B: gone once B closes */
	act = -1;
	for (j = 0; j < 1024 && act < 0; j++)
		if (rec_kind(GID_MAKE(sb, j)) == REC_KIND_ENTITY && rec_state(GID_MAKE(sb, j)) == REC_PENDING
			&& rec_name_count(rec_targetname(GID_MAKE(sb, j))) == 0)
			act = rec_wake(GID_MAKE(sb, j));
	CHECK(act >= 0, "activator");
	rec_run_timers(T, DT);	/* frame start: 2 in-call wakes */
	G.nlog = 0;
	rec_fire(n, act, -1, caller, USE_ON, 0);
	CHECK(G.nlog == 2, "in-call uses %d", G.nlog);
	/* B closes before the pump delivers: the uses wait, canonical */
	rec_map_close(sb);
	uses0 = G.nlog;
	for (f = 0; f < 10; f++)
		frame_at(sb == 0 ? 1 : 0, NULL, 0);
	CHECK(G.nlog == uses0, "nothing delivered into a closed map");
	CHECK(reopen_async(sb, &f, &io), "reopen B");
	for (f = 0; f < 10; f++)
		frame_at(sb, NULL, 0);
	CHECK(G.nlog == best, "deferred uses replayed (%d of %d)", G.nlog - uses0, best - 2);
	for (k = 2; k < G.nlog && k < best; k++)
	{
		CHECK(G.ulog[k] == glist[k], "replay order %d: %04x vs %04x", k, G.ulog[k], glist[k]);
		CHECK(G.alog[k] == -1, "activator of the closed map absent (%d)", G.alog[k]);
	}
	CHECK(G.last_caller == caller, "caller canonical (%04x vs %04x)", G.last_caller, caller);
	printf("A5 deferred: %d uses waited across close, replayed in order on reopen\n", best - 2);
}

/* ---- A6 runtime ids -------------------------------------------------------------- */

static void test_runtime(int sa, int sb, const float start[3])
{
	float far[3] = { start[0], start[1], start[2] + 20000 };
	float o[3] = { start[0] + 64, start[1], start[2] };
	name_t hn = rec_name_find(sa, "startele1");
	uint16_t rt, ids[REC_RUNTIME + 1];
	int e, f, i, ents[REC_RUNTIME + 1], prev;
	io_stats_t io;

	prev = eng_frame_layer();
	eng_set_frame_layer(sa);
	e = ent_create(ENT_ALLOC_GAME);
	eng_set_frame_layer(prev);
	ent_idc(e)->cls = RC_ITEM_HEALTHKIT;
	ent_set_origin(e, o);
	G.val[e] = 4242;
	rec_name_hold(e, hn);
	rt = rec_runtime_id(e);
	CHECK(rt && rec_runtime_id(e) == rt && rec_ref_entity(map_of(sa), rt, 1) == e, "runtime id");
	for (f = 0; f < 20; f++)
		frame1(sa, far[0], far[1], far[2]);
	CHECK(!ent_is_live(e) || ent_idc(e)->rtid != rt, "released");
	CHECK(rec_ref_entity(map_of(sa), rt, 1) < 0, "dry runtime entity is absent");
	G.rt_spawned = 0;
	for (f = 0; f < 10; f++)
		frame1(sa, start[0], start[1], start[2]);
	e = rec_ref_entity(map_of(sa), rt, 1);
	CHECK(e >= 0 && G.rt_spawned == 1 && G.last_cls == RC_ITEM_HEALTHKIT && G.val[e] == 4242
		&& ent_idc(e)->rtid == rt && fabsf(ent_cv(e)->origin[0] - o[0]) < 0.01f,
		"runtime rehydrated by the pump");
	/* its held name came back: a fire reaches it */
	{
		int it = 0, hit = 0, x;

		while ((x = rec_next_named(hn, &it)) >= 0)
			hit |= x == e;
		CHECK(hit, "held name after rehydration");
	}
	/* across a close of its map */
	rec_map_close(sa);
	CHECK(rec_ref_entity(map_of(sa), rt, 1) < 0, "absent while closed");
	CHECK(reopen_async(sa, &f, &io), "reopen A");
	for (f = 0; f < 10; f++)
		frame1(sa, start[0], start[1], start[2]);
	e = rec_ref_entity(map_of(sa), rt, 1);
	CHECK(e >= 0 && G.val[e] == 4242, "runtime entity across close / reopen");
	/* 64 durable ids */
	for (i = 0; i <= REC_RUNTIME; i++)
	{
		eng_set_frame_layer(sa);
		ents[i] = ent_create(ENT_ALLOC_GAME);
		eng_set_frame_layer(prev);
		ent_set_origin(ents[i], start);
		ids[i] = rec_runtime_id(ents[i]);
	}
	CHECK(ids[REC_RUNTIME - 2] != 0 && ids[REC_RUNTIME] == 0, "64 ids, then none");
	for (i = 0; i <= REC_RUNTIME; i++)
		ent_destroy(ents[i]);
	/* re-homing a record entity into B */
	{
		gid_t16 g = GID_NONE;
		int r;

		for (i = 0; i < 1024 && g == GID_NONE; i++)
			if (rec_entity(GID_MAKE(sa, i)) >= 0)
				g = GID_MAKE(sa, i);
		e = rec_entity(g);
		r = rec_rehome(e, sb);
		CHECK(r > 0 && rec_state(g) == REC_KILLED && ent_idc(e)->layer == sb
			&& ent_idc(e)->gid == GID_NONE && rec_ref_entity(map_of(sb), (uint16_t)r, 1) == e,
			"rec_rehome (%d)", r);
		ent_destroy(e);
	}
	printf("A6 runtime: id %u released / rehydrated / across close, %d-id cap, rehome ok\n", rt,
		REC_RUNTIME);
}

/* ---- A7 store sizing ------------------------------------------------------------- */

static void test_store(int slot, const float start[3])
{
	float far[3] = { start[0], start[1], start[2] + 20000 };
	int i, f, n = 0, nb = 0, maxb = 0, gap = 0;
	double tb = -1;
	rec_stats_t s;
	uint32_t hle;

	for (i = 0; i < 1024; i++)
		if (rec_kind(GID_MAKE(slot, i)) == REC_KIND_ENTITY && rec_wake(GID_MAKE(slot, i)) >= 0)
			n++;
	rec_stats(&s, 1);
	for (f = 0; f < 300 && rec_slot_live(slot); f++)
	{
		frame1(slot, far[0], far[1], far[2]);
		rec_stats(&s, 1);
		if (!s.released)
			continue;
		/* batches: every 0.1 s, at most the host budget past the first 8 */
		if (tb >= 0 && T - DT - tb < REC_RELEASE_PERIOD - 1e-6)
			gap++;
		tb = T - DT;
		nb++;
		if ((int)s.released > maxb)
			maxb = (int)s.released;
	}
	CHECK(!gap && maxb == REC_RELEASE_BATCH && nb >= n / REC_RELEASE_BATCH,
		"batches: %d closer than 0.1 s, max %d, %d batches", gap, maxb, nb);
	rec_stats(&s, 0);
	rec_mem_use(&hle, NULL);
	CHECK(rec_slot_live(slot) == 0, "all released (%d left)", rec_slot_live(slot));
	printf("store: %s %d physical records dry with %u B deltas: store %u B (peak %u) of %u, "
		"%d frames, %d batches of <= %d, hle %u B\n", wld_map_name(map_of(slot)), n, G.delta,
		s.store_used, s.store_peak, (unsigned)HL64_DEHYDRATE_KB * 1024, f, nb, maxb, hle);
}

int main(int argc, char **argv)
{
	const char *root;
	float start[3], ang[3];
	int sa, sb;

	if (argc < 4 || strcmp(argv[1], "--root") || strcmp(argv[3], "selftest"))
	{
		fprintf(stderr, "usage: hl64-rec4 --root CONTENT selftest [DELTA_BYTES]\n");
		return 2;
	}
	root = argv[2];
	G.delta = argc > 4 ? (uint32_t)atoi(argv[4]) : 64;
	if (G.delta < sizeof(delta_t))
		G.delta = sizeof(delta_t);
	G.refuse = GID_NONE;
	host_io_init(root);
	mem_boot_carve();
	io_init();
	ent_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-rec4: no content in %s\n", root);
		return 2;
	}
	res_init();
	rec_init();
	col_init();
	G.layer_ready = 1;
	rec_new_campaign(wld_map_find("hldemo1"));
	sa = wld_open(wld_map_find("hldemo1"))->slot;
	CHECK(rec_player_start(sa, start, ang), "player start");
	T = 1.0;
	test_async_open(sa);
	rec_map_close(sa);
	rec_new_campaign(wld_map_find("hldemo1"));
	test_release(sa, start);
	test_frozen(sa, start);
	sb = open_sync("hldemo2");
	CHECK(sb >= 0 && sb != sa, "hldemo2 open");
	test_twins(sa, sb);
	test_deferred(sb);
	test_runtime(sa, sb, start);
	rec_prof_line();
	test_store(sa, start);
	rec_prof_line();
	rec_mem_line();
	log_flush();
	printf(fails ? "selftest FAIL\n" : "selftest ok\n");
	return fails != 0;
}
