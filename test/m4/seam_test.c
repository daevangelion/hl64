/* SPDX-License-Identifier: MIT */
/*
 * seam_test.c -- host driver for the seam runtime (engine/world/seam.c,
 * docs/architecture.md 5.4, docs/formats/cwg-format.md) over the real
 * campaign parse, world layers, player re-base (engine/phys/pmove.c) and
 * entity components. The sibling M4 services the seam drives (async layer
 * open, pins, stream re-base, HLE slots, twins, release, rehome, names,
 * game twin copy / carry) are this file's doubles (Makefile SEAM_WRAP,
 * --wrap): they record the calls and model the documented behaviour on the
 * synchronous M3 world (wld_open).
 *
 *   hl64-seam --root CONTENT dump      the decoded graph, one line per item
 *                                      (test/m4/seam_parse.py compares it
 *                                      with the cook's campaign.json and
 *                                      seams.json)
 *   hl64-seam --root CONTENT init      seam_init only (malformed files:
 *                                      sys_error, exit 1)
 *   hl64-seam --root CONTENT crc       B directory CRC mismatch: the seam
 *                                      is rejected at B open (a cook whose
 *                                      hldemo1->hldemo2 bcrc was altered)
 *   hl64-seam --root CONTENT selftest  approach / switch box / forced /
 *                                      swept-eye / late / settle / leave /
 *                                      open_fail / USEONLY crossing with
 *                                      twins, carried entities, the player
 *                                      re-base and continuity
 * Each test prints "seamtest ok" or FAIL lines; exit status 0 = pass.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/seam.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int fails;

#define CHECK(c, ...) do { \
	if (!(c)) { \
		fails++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- doubles (Makefile SEAM_WRAP) ------------------------------------------ */

wld_layer_t	*__wrap_wld_layer_of_map(int map);
wld_layer_t	*__wrap_wld_open_async(int map);
int		__wrap_wld_layer_ready(const wld_layer_t *L);
void		__wrap_wld_pin_seam(wld_layer_t *B, const uint16_t *pool, int n);
void		__wrap_wld_pin_seam_window(wld_layer_t *B, const float mins[3], const float maxs[3]);
void		__wrap_wld_unpin_seam(wld_layer_t *B);
void		__wrap_wld_stream_rebase(const float T[3]);
int		__wrap_rec_map_open(int layer);
int		__wrap_rec_map_ready(int slot);
void		__wrap_rec_map_close(int slot);
int		__wrap_rec_slot_live(int slot);
int		__wrap_rec_release(int e);
void		__wrap_rec_twin_alias(gid_t16 a, gid_t16 b);
void		__wrap_rec_twin_clear(int slot);
int		__wrap_rec_rehome(int e, int slot);
int		__wrap_rec_entity(gid_t16 gid);
int		__wrap_rec_wake(gid_t16 gid);
void		__wrap_rec_origin(gid_t16 gid, float out[3]);
name_t		__wrap_rec_name_find(int slot, const char *s);
void		__wrap_rec_fire(name_t n, int activator, int caller, gid_t16 caller_gid,
			int use_type, float value);
void		__wrap_rec_fire_at(float t, name_t target, name_t killtarget, int activator,
			int caller, int use_type, float value);
int		__wrap_rec_key_f(gid_t16 gid, key_t16 k, float *out);
void		__wrap_game_twin_copy(int from, int to);
int		__wrap_game_carry(int e);

static struct {
	int		ready, hle_ready, open_null;
	int		live[4];
	int		open_async, alias, twin_clear, rehome, release, pin, window, unpin;
	int		rebase, fire, fire_at, map_open, map_close, copies;
	int		pin_n;
	float		win_mins[3], win_maxs[3];
	float		T[3];
	name_t		fire_name;
	int		fire_use, fire_layer;
	int		copy_from[8], copy_to[8];
	uint8_t		carry[HL64_MAX_ENTITIES];
} D;

/* records the doubles know: gid -> entity, record origin */
#define RECS 32
static struct {
	gid_t16		gid;
	int		e;		/* -1 none */
	float		org[3];
} recs[RECS];
static int nrecs;

static int rec_find(gid_t16 g)
{
	int i;

	for (i = 0; i < nrecs; i++)
		if (recs[i].gid == g)
			return i;
	return -1;
}

static int rec_add(gid_t16 g, const float org[3])
{
	int i = rec_find(g);

	if (i < 0)
	{
		i = nrecs++;
		recs[i].gid = g;
	}
	recs[i].e = -1;
	memcpy(recs[i].org, org, sizeof(recs[i].org));
	return i;
}

wld_layer_t *__wrap_wld_layer_of_map(int map)
{
	int i;

	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		wld_layer_t *L = wld_layer(i);

		if (L && L->map == map)
			return L;
	}
	return NULL;
}

wld_layer_t *__wrap_wld_open_async(int map)
{
	wld_layer_t *L = __wrap_wld_layer_of_map(map);

	D.open_async++;
	if (L)
		return L;
	return D.open_null ? NULL : wld_open(map);
}

int __wrap_wld_layer_ready(const wld_layer_t *L)
{
	(void)L;
	return D.ready;
}

void __wrap_wld_pin_seam(wld_layer_t *B, const uint16_t *pool, int n)
{
	(void)B;
	(void)pool;
	D.pin++;
	D.pin_n = n;
}

void __wrap_wld_pin_seam_window(wld_layer_t *B, const float mins[3], const float maxs[3])
{
	(void)B;
	D.window++;
	memcpy(D.win_mins, mins, sizeof(D.win_mins));
	memcpy(D.win_maxs, maxs, sizeof(D.win_maxs));
}

void __wrap_wld_unpin_seam(wld_layer_t *B)
{
	(void)B;
	D.unpin++;
}

void __wrap_wld_stream_rebase(const float T[3])
{
	D.rebase++;
	memcpy(D.T, T, sizeof(D.T));
}

int __wrap_rec_map_open(int layer)
{
	(void)layer;
	D.map_open++;
	return 0;
}

int __wrap_rec_map_ready(int slot)
{
	(void)slot;
	return D.hle_ready;
}

void __wrap_rec_map_close(int slot)
{
	(void)slot;
	D.map_close++;
}

int __wrap_rec_slot_live(int slot)
{
	return D.live[slot];
}

int __wrap_rec_release(int e)
{
	int i;

	D.release++;
	for (i = 0; i < nrecs; i++)
		if (recs[i].e == e)
			recs[i].e = -1;
	ent_destroy(e);
	return 1;
}

void __wrap_rec_twin_alias(gid_t16 a, gid_t16 b)
{
	(void)a;
	(void)b;
	D.alias++;
}

void __wrap_rec_twin_clear(int slot)
{
	(void)slot;
	D.twin_clear++;
}

int __wrap_rec_rehome(int e, int slot)
{
	(void)slot;
	D.rehome++;
	ent_idc(e)->gid = GID_NONE;
	return 1;
}

int __wrap_rec_entity(gid_t16 gid)
{
	int i = rec_find(gid);

	return i < 0 ? -1 : recs[i].e;
}

/* a brush entity of the frame layer at org (submodel 1, 32 u cube) */
static int make_ent(const float org[3], int brush, gid_t16 gid)
{
	static const float mins[3] = { -16, -16, -16 }, maxs[3] = { 16, 16, 16 };
	int e = ent_create(ENT_ALLOC_GAME);

	if (e < 0)
		return -1;
	ent_idc(e)->gid = gid;
	ent_idc(e)->cls = 7;
	ent_rnd(e)->model = brush ? RES_SUBMODEL(eng_frame_layer(), 1) : RES_MAKE(RES_K_MODEL, 3);
	ent_set_size(e, mins, maxs);
	ent_set_origin(e, org);
	return e;
}

int __wrap_rec_wake(gid_t16 gid)
{
	int i = rec_find(gid);

	if (i < 0)
		return -1;
	if (recs[i].e < 0)
		recs[i].e = make_ent(recs[i].org, 1, gid);
	return recs[i].e;
}

void __wrap_rec_origin(gid_t16 gid, float out[3])
{
	int i = rec_find(gid);

	if (i < 0)
		memset(out, 0, 3 * sizeof(float));
	else
		memcpy(out, recs[i].org, 3 * sizeof(float));
}

name_t __wrap_rec_name_find(int slot, const char *s)
{
	return strcmp(s, "elestartmm") ? NAME_NONE : NAME_MAKE(slot, 7);
}

void __wrap_rec_fire(name_t n, int activator, int caller, gid_t16 caller_gid, int use_type,
	float value)
{
	(void)activator;
	(void)caller;
	(void)caller_gid;
	(void)value;
	D.fire++;
	D.fire_name = n;
	D.fire_use = use_type;
	D.fire_layer = eng_frame_layer();
}

void __wrap_rec_fire_at(float t, name_t target, name_t killtarget, int activator, int caller,
	int use_type, float value)
{
	(void)t;
	(void)target;
	(void)killtarget;
	(void)activator;
	(void)caller;
	(void)use_type;
	(void)value;
	D.fire_at++;
}

int __wrap_rec_key_f(gid_t16 gid, key_t16 k, float *out)
{
	(void)gid;
	(void)k;
	(void)out;
	return 0;
}

void __wrap_game_twin_copy(int from, int to)
{
	if (D.copies < 8)
	{
		D.copy_from[D.copies] = from;
		D.copy_to[D.copies] = to;
	}
	D.copies++;
}

int __wrap_game_carry(int e)
{
	return D.carry[e];
}

/* ---- helpers ------------------------------------------------------------------ */

static void reset_doubles(void)
{
	memset(&D, 0, sizeof(D));
	D.ready = D.hle_ready = 1;
	nrecs = 0;
}

/* the world's frame boundary (closed layers' blocks retire behind their
 * 2-frame fence, as in eng_frame) */
static uint32_t wframe;

static void retire(void)
{
	wld_frame(++wframe);
	wld_frame(++wframe);
	wld_frame(++wframe);
}

/* one frame's seam step */
static void step(void)
{
	seam_frame();
	wld_frame(++wframe);
	log_flush();
}

/* New Game on map with only its layer open; the player at org (local) */
static int start(const char *name, const float org[3])
{
	static const float zero[3];
	int map = wld_map_find(name), i, e;

	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		wld_layer_t *L = wld_layer(i);

		if (L && L->map != map)
			wld_close(L);
	}
	retire();
	for (e = ENT_PLAYER + 1; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e))
			ent_destroy(e);
	reset_doubles();
	if (map < 0 || phy_player_spawn(map, org, zero))
		return -1;
	seam_new_game(map);
	return map;
}

static int find_seam(const char *from, const char *to, const char *landmark, seam_info_t *s)
{
	int i;

	for (i = 0; seam_info(i, s) == 0; i++)
		if (!strcmp(wld_map_name(s->from), from) && !strcmp(wld_map_name(s->to), to)
			&& !strcmp(s->landmark, landmark))
			return i;
	return -1;
}

static void put_player(const float org[3])
{
	memcpy(phy_player()->origin, org, sizeof(float) * 3);
}

/* the game clock advances by dt (s) */
static void tick(double dt)
{
	eng_clock_frame(dt);
	eng_clock_advance();
}

static int slot_of(int map)
{
	const wld_layer_t *L = __wrap_wld_layer_of_map(map);

	return L ? L->slot : -1;
}

/* a player origin in the approach box, eye outside the switch box past
 * its x max by `out` units; eye z at the switch box's middle */
static void beside_switch(const seam_info_t *s, float out, float org[3])
{
	org[0] = s->sw_maxs[0] + out;
	org[1] = 0.5f * (s->sw_mins[1] + s->sw_maxs[1]);
	org[2] = 0.5f * (s->sw_mins[2] + s->sw_maxs[2]) - phy_player()->view_ofs[2];
}

static seam_stats_t stats(void)
{
	seam_stats_t st;

	seam_stats(&st);
	return st;
}

/* ---- tests ------------------------------------------------------------------- */

/* approach, the B open sequence, the leave rule and open_fail */
static void test_approach(void)
{
	seam_info_t s;
	seam_stats_t st0, st;
	float org[3], far[3] = { 0, 0, 0 };
	int si = find_seam("hldemo2", "hldemo3", "hldemo2_3", &s), a;

	CHECK(si >= 0, "no hldemo2->hldemo3 seam");
	if (si < 0)
		return;
	beside_switch(&s, 64, org);
	/* outside every approach box: nothing opens */
	far[0] = s.ap_mins[0] - 1024;
	if ((a = start("hldemo2", far)) < 0)
	{
		CHECK(0, "start hldemo2");
		return;
	}
	st0 = stats();
	step();
	CHECK(D.open_async == 0 && !__wrap_wld_layer_of_map(s.to), "opened outside the approach box");
	/* approach: B layer, HLE slot, twin aliases, pin + window, anchors */
	put_player(org);
	step();
	st = stats();
	CHECK(st.approaches == st0.approaches + 1, "approach not counted");
	CHECK(__wrap_wld_layer_of_map(s.to) != NULL, "B not opened");
	CHECK(D.map_open == 1 && D.pin == 1 && D.window == 1 && D.pin_n == s.npool,
		"open sequence map_open=%d pin=%d window=%d n=%d", D.map_open, D.pin, D.window,
		D.pin_n);
	CHECK(fabsf(D.win_mins[2] - (s.sw_mins[2] + s.delta[2] - 28 - 36)) < 1e-3f,
		"pin window z %g", (double)D.win_mins[2]);
	{
		float an[4][3];
		int n = seam_anchors(an, 4), k;
		const float *g = wld_map_origin(a);

		CHECK(n == 2, "anchors %d", n);
		for (k = 0; n == 2 && k < 3; k++)
			CHECK(an[0][k] == org[k] + g[k] && an[1][k] == an[0][k] + s.T[k],
				"anchor %d", k);
	}
	/* B not ready yet: nothing more; ready: pins once */
	step();
	CHECK(D.pin == 1 && D.map_open == 1, "pinned again pin=%d", D.pin);
	/* leave the approach box with B's slot live: unpinned, kept open */
	D.live[slot_of(s.to)] = 3;
	put_player(far);
	step();
	CHECK(D.unpin >= 1, "not unpinned on leaving the approach box");
	CHECK(__wrap_wld_layer_of_map(s.to) != NULL, "closed with live entities");
	/* nothing live: closes (twins cleared, HLE slot closed) */
	D.live[slot_of(s.to)] = 0;
	step();
	st = stats();
	CHECK(!__wrap_wld_layer_of_map(s.to) && st.closes == st0.closes + 1 && D.map_close == 1
		&& D.twin_clear == 1, "leave rule close: open=%d closes=%u map_close=%d",
		__wrap_wld_layer_of_map(s.to) != NULL, st.closes, D.map_close);

	/* both layers in use by home + another neighbour that is live:
	 * open_fail once, retried; released: that neighbour closes, B opens */
	{
		int other = wld_map_find("hldemo1");
		seam_info_t r;
		float o1[3];

		/* the approach of hldemo2->hldemo1 opens hldemo1 */
		if (find_seam("hldemo2", "hldemo1", "hldemo1to2", &r) < 0)
		{
			CHECK(0, "no hldemo2->hldemo1");
			return;
		}
		o1[0] = 0.5f * (r.ap_mins[0] + r.ap_maxs[0]);
		o1[1] = 0.5f * (r.ap_mins[1] + r.ap_maxs[1]);
		o1[2] = 0.5f * (r.ap_mins[2] + r.ap_maxs[2]);
		put_player(o1);
		step();
		CHECK(__wrap_wld_layer_of_map(other) != NULL, "hldemo1 not opened");
		st0 = stats();
		D.live[slot_of(other)] = 5;
		put_player(org);
		step();
		step();
		st = stats();
		CHECK(st.open_fail == st0.open_fail + 1, "open_fail %u (want %u)", st.open_fail,
			st0.open_fail + 1);
		CHECK(!__wrap_wld_layer_of_map(s.to), "B opened over a live neighbour");
		D.live[slot_of(other)] = 0;
		step();
		st = stats();
		CHECK(__wrap_wld_layer_of_map(s.to) && !__wrap_wld_layer_of_map(other),
			"released neighbour not replaced");
		CHECK(st.open_fail == st0.open_fail + 1, "open_fail counted again");
	}
	/* NULL from wld_open_async: open_fail, no retry in this approach */
	start("hldemo2", far);
	step();
	st0 = stats();
	D.open_null = 1;
	put_player(org);
	step();
	step();
	st = stats();
	CHECK(st.open_fail == st0.open_fail + 1 && D.open_async == 1,
		"NULL open: open_fail %u async %d", st.open_fail - st0.open_fail, D.open_async);
	/* wld_layer_ready -1: open_fail, closed */
	start("hldemo2", far);
	step();
	st0 = stats();
	D.ready = -1;
	put_player(org);
	step();
	st = stats();
	CHECK(st.open_fail == st0.open_fail + 1 && !__wrap_wld_layer_of_map(s.to),
		"failed layer open not closed");
}

/* armed touch seams: forced rules, swept eye, eye in the box, disarm,
 * late switch, settle window */
static void test_touch(void)
{
	seam_info_t s;
	seam_stats_t st0, st;
	float org[3], in[3], far[3] = { 0, 0, 0 };
	int si = find_seam("hldemo2", "hldemo3", "hldemo2_3", &s), a;
	gid_t16 trig;

	if (si < 0)
		return;
	far[0] = s.ap_mins[0] - 1024;
	beside_switch(&s, 64, org);
	memcpy(in, org, sizeof(in));
	in[0] = 0.5f * (s.sw_mins[0] + s.sw_maxs[0]);

	/* (a) forced: the armed changelevel not touched in a frame */
	a = start("hldemo2", org);
	trig = GID_MAKE(slot_of(a), s.trigger);
	eng_clock_new_game();
	tick(0.1);
	tick(0.1);
	step();
	st0 = stats();
	CHECK(seam_changelevel(trig, 0) == 1, "touch not taken");
	step();
	CHECK(phy_player_map() == a, "switched while touched outside the box");
	step();
	st = stats();
	CHECK(phy_player_map() == s.to && st.switches == st0.switches + 1
		&& st.forced == st0.forced + 1, "left-the-trigger force: map %s forced %u",
		wld_map_name(phy_player_map()), st.forced - st0.forced);

	/* (b) forced: touched 1 s outside the box */
	a = start("hldemo2", org);
	trig = GID_MAKE(slot_of(a), s.trigger);
	step();
	st0 = stats();
	{
		int f;

		for (f = 0; f < 40 && phy_player_map() == a; f++)
		{
			seam_changelevel(trig, 0);
			step();
			tick(1.0 / 30);
		}
		st = stats();
		CHECK(phy_player_map() == s.to && st.forced == st0.forced + 1 && f > 29 && f < 33,
			"in-the-trigger force after %d frames", f);
	}

	/* (c) swept eye segment through a thin box: a touch switch, not forced */
	a = start("hldemo2", org);
	trig = GID_MAKE(slot_of(a), s.trigger);
	step();
	st0 = stats();
	seam_changelevel(trig, 0);
	step();
	{
		float o2[3];

		memcpy(o2, org, sizeof(o2));
		o2[0] = s.sw_mins[0] - 32;	/* jumped across the box in one frame */
		put_player(o2);
		seam_changelevel(trig, 0);
		step();
	}
	st = stats();
	CHECK(phy_player_map() == s.to && st.forced == st0.forced, "swept switch: map %s forced %u",
		wld_map_name(phy_player_map()), st.forced - st0.forced);

	/* (d) eye in the box at the touch: switches at this frame's seam step */
	a = start("hldemo2", in);
	trig = GID_MAKE(slot_of(a), s.trigger);
	step();
	seam_changelevel(trig, 0);
	step();
	CHECK(phy_player_map() == s.to, "eye-in-box touch did not switch");
	/* (e) settle: touches ignored until a frame without one */
	{
		seam_info_t r;
		int ri = find_seam(wld_map_name(s.to), "hldemo2", "hldemo2_3", &r);
		gid_t16 rt = ri >= 0 ? GID_MAKE(slot_of(s.to), r.trigger) : GID_NONE;

		CHECK(ri >= 0, "no reverse seam");
		CHECK(seam_changelevel(rt, 0) == 0, "touch taken in the settle window");
		step();
		CHECK(seam_changelevel(rt, 0) == 0, "settle ended while touched");
		step();
		step();		/* a frame with no touch */
		CHECK(seam_changelevel(rt, 0) == 1, "settle never ended");
		/* the old home's trigger is not a home seam any more */
		CHECK(seam_changelevel(trig, 0) == 0, "A's changelevel taken after the switch");
	}

	/* (f) disarm on leaving the approach box */
	a = start("hldemo2", org);
	trig = GID_MAKE(slot_of(a), s.trigger);
	step();
	seam_changelevel(trig, 0);
	put_player(far);
	step();
	CHECK(phy_player_map() == a, "switched after leaving the approach box");
	step();
	CHECK(phy_player_map() == a, "armed after leaving the approach box");

	/* (g) late: B not ready at the switch; deferred, then crosses */
	a = start("hldemo2", in);
	trig = GID_MAKE(slot_of(a), s.trigger);
	D.hle_ready = 0;
	step();
	st0 = stats();
	seam_changelevel(trig, 0);
	step();
	step();
	st = stats();
	CHECK(phy_player_map() == a && st.late == st0.late + 2, "late %u", st.late - st0.late);
	D.hle_ready = 1;
	step();
	CHECK(phy_player_map() == s.to, "late switch never crossed");
}

/* USEONLY hldemo1 -> hldemo2 with twins, carried entities, the re-base */
static void test_use(void)
{
	static const float arrive[3] = { -452, -100, 196 };
	seam_info_t s;
	seam_stats_t st0, st;
	float org[3], g0[3], g1[3], vel[3] = { 30, -20, 5 }, ang[3];
	const float *ga, *gb;
	int si = find_seam("hldemo1", "hldemo2", "hldemo1to2", &s), a, sa, sb, k, i;
	int door = -1, carried, kept, before_e[4];
	ent_player_t *pl;

	CHECK(si >= 0 && s.kind == 1 && s.ntwin == 4, "hldemo1to2 seam");
	if (si < 0)
		return;
	for (k = 0; k < 3; k++)
		org[k] = arrive[k] - s.delta[k];
	if ((a = start("hldemo1", org)) < 0)
	{
		CHECK(0, "start hldemo1");
		return;
	}
	sa = slot_of(a);
	pl = phy_player();
	memcpy(pl->velocity, vel, sizeof(vel));
	pl->view_angles[1] = 37;
	memcpy(ang, pl->view_angles, sizeof(ang));
	/* B's slot will be the other one */
	sb = sa ^ 1;
	/* the twins: A copies live (door displaced 10 u up), B records pending */
	eng_set_frame_layer(sa);
	for (i = 0; i < s.ntwin; i++)
	{
		static const float zero[3];
		float o[3] = { 0, 0, 10 };
		int r = rec_add(GID_MAKE(sa, s.twins[2 * i]), zero);

		rec_add(GID_MAKE(sb, s.twins[2 * i + 1]), zero);
		recs[r].e = before_e[i] = make_ent(o, 1, GID_MAKE(sa, s.twins[2 * i]));
		ent_set_velocity(recs[r].e, vel);
		ent_tm(recs[r].e)->ltime = 3.5f;
		ent_think_at(recs[r].e, 4.25f);
		ent_rnd(recs[r].e)->frame = 2;
		if (i == 1)
			door = recs[r].e;
	}
	pl->groundentity = (int16_t)door;
	/* a carried point entity and one the game keeps */
	{
		float o[3] = { org[0] + 8, org[1], org[2] };

		carried = make_ent(o, 0, GID_NONE);
		D.carry[carried] = 1;
		o[0] = org[0] - 8;
		kept = make_ent(o, 0, GID_MAKE(sa, 500));
	}
	/* the kept one is in the carry box: continuity reports it missing
	 * unless B has its own copy there: give B one at the mapped place */
	eng_set_frame_layer(-1);
	step();
	st0 = stats();
	for (k = 0; k < 3; k++)
		g0[k] = pl->origin[k] + wld_map_origin(a)[k];
	CHECK(seam_changelevel(GID_MAKE(sa, s.trigger), 1) == 1, "use not taken");
	{
		float o[3] = { org[0] - 8 + s.delta[0], org[1] + s.delta[1], org[2] + s.delta[2] };
		int e;

		/* B ready this frame: slot sb; its own copy of `kept` */
		step();
		CHECK(phy_player_map() == s.to, "USEONLY did not switch at the next seam_frame");
		eng_set_frame_layer(slot_of(s.to));
		e = make_ent(o, 0, GID_MAKE(slot_of(s.to), 501));
		CHECK(e >= 0, "B copy");
		eng_set_frame_layer(-1);
	}
	sb = slot_of(s.to);
	ga = wld_map_origin(a);
	gb = wld_map_origin(s.to);
	for (k = 0; k < 3; k++)
		g1[k] = pl->origin[k] + gb[k];
	st = stats();
	CHECK(st.switches == st0.switches + 1 && st.forced == st0.forced, "switch counted");
	/* the re-base: global moves by exactly T; arrival in hldemo2's lift */
	for (k = 0; k < 3; k++)
	{
		CHECK(g1[k] == g0[k] + s.T[k], "global[%d] %.9g != %.9g + T %.9g", k, (double)g1[k],
			(double)g0[k], (double)s.T[k]);
		CHECK(fabsf(pl->origin[k] - arrive[k]) <= 1, "arrival[%d] %.3f", k,
			(double)pl->origin[k]);
		CHECK(pl->velocity[k] == vel[k] && pl->view_angles[k] == ang[k], "move state kept");
		CHECK(s.T[k] == (float)((double)gb[k] + s.delta[k] - ga[k]), "T check");
	}
	CHECK(ent_idc(ENT_PLAYER)->layer == sb, "player entity layer %d", ent_idc(ENT_PLAYER)->layer);
	CHECK(D.rebase == 1 && !memcmp(D.T, s.T, sizeof(D.T)), "stream re-base");
	/* twins: copied (displacement kept), game copy, A released, ground */
	CHECK(D.copies == 4 && D.release == 4 && st.twins == st0.twins + 4,
		"twins copies=%d release=%d", D.copies, D.release);
	for (i = 0; i < 4 && i < D.copies; i++)
	{
		int b = D.copy_to[i];

		CHECK(D.copy_from[i] == before_e[i], "copy order");
		CHECK(ent_idc(b)->layer == sb && ent_cv(b)->origin[2] == 10
			&& ent_mot(b)->velocity[0] == vel[0] && ent_tm(b)->ltime == 3.5f
			&& ent_tm(b)->nextthink == 4.25f && ent_rnd(b)->frame == 2
			&& RES_SUB_SLOT(ent_rnd(b)->model) == (unsigned)sb,
			"twin %d components", i);
	}
	CHECK(D.copies > 1 && pl->groundentity == D.copy_to[1], "ground -> B twin (%d)",
		pl->groundentity);
	/* carried: in B's frame and slot; the kept one stays in A */
	CHECK(ent_idc(carried)->layer == sb && D.rehome == 1 && st.carried == st0.carried + 1
		&& ent_cv(carried)->origin[0] == org[0] + 8 + s.delta[0], "carried entity");
	CHECK(ent_idc(kept)->layer == sa, "kept entity moved");
	/* changetarget in B, USE_TOGGLE, B's frame */
	CHECK(D.fire == 1 && D.fire_name == NAME_MAKE(sb, 7) && D.fire_use == USE_TOGGLE
		&& D.fire_layer == sb, "changetarget fire=%d", D.fire);
	/* continuity next frame: twins, carried and B's own copy all match */
	step();
	st = stats();
	CHECK(st.continuity_bad == st0.continuity_bad, "continuity_bad %u",
		st.continuity_bad - st0.continuity_bad);
	CHECK(seam_last_switch_frame() == eng_frame_count(), "last switch frame");
	/* the old home stays open while the reverse seam's approach holds the
	 * player; a continuity miss is counted when B lacks a copy */
	CHECK(__wrap_wld_layer_of_map(a) != NULL, "A closed inside the reverse approach");
}

/* a carry-box entity with no counterpart after the switch */
static void test_continuity_bad(void)
{
	seam_info_t s;
	seam_stats_t st0, st;
	float org[3];
	int a, k;

	if (find_seam("hldemo1", "hldemo2", "hldemo1to2", &s) < 0)
		return;
	for (k = 0; k < 3; k++)
		org[k] = 0.5f * (s.carry_mins[k] + s.carry_maxs[k]);
	org[2] = s.carry_mins[2] + 40;
	if ((a = start("hldemo1", org)) < 0)
		return;
	eng_set_frame_layer(slot_of(a));
	make_ent(org, 0, GID_MAKE(slot_of(a), 500));
	eng_set_frame_layer(-1);
	step();
	st0 = stats();
	seam_changelevel(GID_MAKE(slot_of(a), s.trigger), 1);
	step();
	step();
	st = stats();
	CHECK(phy_player_map() == s.to && st.continuity_bad == st0.continuity_bad + 1,
		"continuity_bad %u", st.continuity_bad - st0.continuity_bad);
}

static void selftest(void)
{
	test_approach();
	test_touch();
	test_use();
	test_continuity_bad();
}

/* ---- dump ---------------------------------------------------------------------- */

static void pv(const char *k, const float v[3])
{
	printf(" %s=%.9g,%.9g,%.9g", k, (double)v[0], (double)v[1], (double)v[2]);
}

static void dump(void)
{
	seam_graph_t g;
	seam_map_t m;
	seam_info_t s;
	int i, j;

	if (seam_graph(&g))
	{
		printf("FAIL no campaign\n");
		fails++;
		return;
	}
	printf("graph maps=%u landmarks=%u links=%u seams=%u twins=%u pool=%u start=%u train=%u "
		"components=%u strbytes=%lu size=%lu\n", g.nmaps, g.nlandmarks, g.nlinks, g.nseams,
		g.ntwins, g.npool, g.start, g.train, g.components, (unsigned long)g.strbytes,
		(unsigned long)g.size);
	for (i = 0; seam_map(i, &m) == 0; i++)
	{
		printf("map %d %s component=%u landmarks=%u links=%u", i, m.name, m.component,
			m.nlandmarks, m.nlinks);
		pv("gofs", m.gofs);
		pv("mins", m.mins);
		pv("maxs", m.maxs);
		printf("\n");
		for (j = m.first_landmark; j < m.first_landmark + m.nlandmarks; j++)
		{
			const char *n;
			float o[3];

			seam_landmark(j, &n, o);
			printf("landmark %s %s", m.name, n);
			pv("org", o);
			printf("\n");
		}
		for (j = m.first_link; j < m.first_link + m.nlinks; j++)
		{
			uint16_t dest, sf, si;
			const char *n;

			seam_link(j, &dest, &n, &sf, &si);
			printf("link %s %s %s spawnflags=%u seam=%d\n", m.name, wld_map_name(dest), n,
				sf, si == 0xFFFF ? -1 : si);
		}
	}
	for (i = 0; seam_info(i, &s) == 0; i++)
	{
		printf("seam %d %s %s %s trigger=%u changetarget=%s kind=%u flags=%u twins=%u "
			"pool=%u draw=%u bcrc=%lu need=%lu,%lu,%lu", i, wld_map_name(s.from),
			wld_map_name(s.to), s.landmark, s.trigger, s.changetarget ? s.changetarget : "-",
			s.kind, s.flags, s.ntwin, s.npool, s.ndraw, (unsigned long)s.bcrc,
			(unsigned long)s.need[0], (unsigned long)s.need[1], (unsigned long)s.need[2]);
		pv("delta", s.delta);
		pv("T", s.T);
		pv("sw_mins", s.sw_mins);
		pv("sw_maxs", s.sw_maxs);
		pv("carry_mins", s.carry_mins);
		pv("carry_maxs", s.carry_maxs);
		pv("ap_mins", s.ap_mins);
		pv("ap_maxs", s.ap_maxs);
		printf(" twin_lumps=");
		for (j = 0; j < s.ntwin; j++)
			printf("%s%u:%u", j ? "," : "", s.twins[2 * j], s.twins[2 * j + 1]);
		printf(" sectors=");
		for (j = 0; j < s.npool; j++)
			printf("%s%u", j ? "," : "", s.pool[j]);
		printf("\n");
	}
}

/* the hldemo1 -> hldemo2 seam with a cook whose bcrc differs: B opens,
 * the seam is rejected (open_fail, flags 0) and never switches */
static void crc(void)
{
	seam_info_t s;
	seam_stats_t st;
	float org[3];
	int si = find_seam("hldemo1", "hldemo2", "hldemo1to2", &s), a, k;

	if (si < 0)
	{
		CHECK(0, "no seam");
		return;
	}
	for (k = 0; k < 3; k++)
		org[k] = 0.5f * (s.sw_mins[k] + s.sw_maxs[k]);
	org[2] -= 28;
	if ((a = start("hldemo1", org)) < 0)
		return;
	step();
	seam_changelevel(GID_MAKE(slot_of(a), s.trigger), 1);
	step();
	step();
	st = stats();
	seam_info(si, &s);
	CHECK(phy_player_map() == a && st.open_fail == 1 && s.flags == 0 && D.map_open == 0,
		"CRC mismatch: map %s open_fail %u flags %u", wld_map_name(phy_player_map()),
		st.open_fail, s.flags);
	CHECK(seam_changelevel(GID_MAKE(slot_of(a), s.trigger), 1) == 0, "rejected seam taken");
}

int main(int argc, char **argv)
{
	if (argc != 4 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-seam --root CONTENT dump|init|crc|selftest\n");
		return 2;
	}
	host_io_init(argv[2]);
	mem_boot_carve();
	settings_reset();
	io_init();
	dbg_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-seam: no content in %s\n", argv[2]);
		return 2;
	}
	ent_init();
	in_init();
	phy_player_init();
	seam_init();
	reset_doubles();
	if (!strcmp(argv[3], "dump"))
		dump();
	else if (!strcmp(argv[3], "crc"))
		crc();
	else if (!strcmp(argv[3], "selftest"))
		selftest();
	else if (strcmp(argv[3], "init"))
	{
		fprintf(stderr, "hl64-seam: unknown test %s\n", argv[3]);
		return 2;
	}
	log_flush();
	printf(fails ? "seamtest FAIL %s\n" : "seamtest ok %s\n", argv[3]);
	return fails ? 1 : 0;
}
