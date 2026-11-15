/* SPDX-License-Identifier: MIT */
/*
 * seam.c -- campaign seams at run time (docs/architecture.md 5.4,
 * docs/formats/cwg-format.md; contract: include/hl64/seam.h).
 *
 * Boot: the whole CWG3 file into the campaign region, validated (sizes,
 * strings, indices, boxes, every seam's T against the map placements and
 * the landmark delta). The sector pool and the twin table are turned into
 * host order in place; everything else is decoded where it is read.
 *
 * Frame (step 7, after the active-set pass):
 *  - an armed touch seam switches when the eye (home-local) or the eye's
 *    swept segment since the last frame enters the switch box; it is
 *    forced when the changelevel was not touched this frame or has been
 *    touched for 1 s, still outside the box; it disarms when the player
 *    leaves its approach box;
 *  - the selected seam (pending, else armed, else the nearest approach box
 *    holding the player origin) opens B: async layer, then its HLE slot,
 *    then the twin aliases and B's seam pin;
 *  - a pending switch crosses once B is ready (late frames counted, never
 *    blocks): carried entities, twins, the player re-base
 *    (phy_player_rebase), the stream re-base, the changetarget in B;
 *  - a non-home layer no seam leads into from where the player stands, and
 *    with nothing live in its HLE slot, closes;
 *  - the approached seam's ghosts (seam_ghost): B's entities drawn into
 *    B's carry box that A's view shows at their place - T.
 *
 * Behaviour oracle: the old tree's seam controller (n64/port/q64_world.c,
 * docs/provenance-audit.md entry "n64/port/q64_world.c": CLEAN +
 * QUAKE-INTERFACE). Written for hl64's world layers, HLE records and
 * entity components; no binding of that file remains.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/seam.h"
#include "engine/core/eng.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"

#define CWG_MAGIC	0x43574733u	/* 'CWG3' */
#define CWG_NONE	0xFFFF
#define MAP_B		48
#define LM_B		16
#define LINK_B		8
#define SEAM_B		136
#define TWIN_B		4
#define POOL_B		2

/* seam record offsets (cwg-format.md "Seam record") */
#define S_FROM		0
#define S_TO		2
#define S_LANDMARK	4
#define S_TRIGGER	6
#define S_CTARGET	8
#define S_KIND		10
#define S_FLAGS		12
#define S_TWIN0		14
#define S_NTWIN		16
#define S_POOL0		18
#define S_NPOOL		20
#define S_NDRAW		22
#define S_DELTA		24
#define S_SWITCH	36
#define S_CARRY		60
#define S_APPROACH	84
#define S_NEED		108
#define S_BCRC		120
#define S_T		124

#define ARMED_TOUCH_MAX	1.0	/* s touching the trigger outside the box */
#define CONT_MAX	64	/* carry-box entities the continuity check holds */
#define CONT_MATCH	1.0f	/* u */
#define VIEW_Z		28.0f	/* HLSDK VEC_VIEW: eye above the origin */

static const char path[] = "maps/campaign.cwg";

/* the file in the campaign region */
static struct {
	uint8_t		*b;
	uint32_t	size, strbytes;
	uint16_t	nmaps, nlm, nln, start, train, ncomp, nseam, ntwin, npool;
	uint32_t	o_lm, o_ln, o_seam, o_twin, o_pool, o_str;
	uint16_t	*twins, *pool;		/* host order, in place */
} G;

typedef struct {
	cls_t		cls;
	int16_t		e;		/* a ghost: B's entity itself; -1 an A entity */
	float		pos[3];		/* B frame */
} cont_t;

static struct {
	uint8_t		loaded;
	int		home;			/* CWG map, -1 */
	int		near;			/* selected seam, -1 */
	int		armed, pending;
	const char	*why;			/* pending switch: use / touch / forced: ... */
	double		armed_time;
	uint32_t	tick;			/* seam_frame calls */
	uint32_t	touch_tick;		/* armed changelevel touched in this tick */
	uint8_t		settle;			/* changelevel touches ignored ... */
	uint32_t	settle_tick;		/* ... while one came in this tick */
	uint8_t		near_failed;		/* open_fail counted for this approach */
	uint8_t		near_dead;		/* ... and no retry until the next one */
	uint32_t	wait_frame;		/* B's open waits on a held neighbour since (+1), 0 not */
	int		lmap[HL64_WORLD_LAYERS];	/* open layers seen by map, -1 */
	uint8_t		ready;			/* the selected seam's B is ready */
	int		aliased;		/* seam whose twins are aliased */
	int		pinned;			/* seam whose B is pinned */
	uint8_t		hle_open[HL64_WORLD_LAYERS];
	uint8_t		have_eye;
	float		prev_eye[3];		/* home-local */
	uint32_t	last_switch;
	/* continuity: the switch's carry-box set, compared next frame */
	uint8_t		cont_pending;
	int		cont_slot, cont_n, cont_over;
	float		cont_mins[3], cont_maxs[3];	/* B frame */
	cont_t		cont[CONT_MAX];
	/* approach ghosts (seam_ghost): this frame's set, A's map, B's slot,
	 * the global offset (-T) and B's carry box (B frame) */
	int		ghost_n, ghost_map, ghost_slot;
	int16_t		ghost[CONT_MAX];
	float		ghost_off[3], ghost_mins[3], ghost_maxs[3];
	seam_stats_t	st;
	uint32_t	frames, us_sum, us_max;
} S;

/* ---- decoding ----------------------------------------------------------------- */

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static float bef(const uint8_t *p)
{
	uint32_t u = be32(p);
	float f;

	memcpy(&f, &u, sizeof(f));
	return f;
}

static void bev(const uint8_t *p, float v[3])
{
	int k;

	for (k = 0; k < 3; k++)
		v[k] = bef(p + 4 * k);
}

static const char *str(uint16_t ofs)
{
	return (const char *)G.b + G.o_str + ofs;
}

static uint8_t *seam_rec(int i)
{
	return G.b + G.o_seam + (uint32_t)SEAM_B * (uint32_t)i;
}

static int seam_from(int i)
{
	return be16(seam_rec(i) + S_FROM);
}

static int seam_to(int i)
{
	return be16(seam_rec(i) + S_TO);
}

static int seam_flags(int i)
{
	return be16(seam_rec(i) + S_FLAGS);
}

static void seam_box(int i, int ofs, float mins[3], float maxs[3])
{
	bev(seam_rec(i) + ofs, mins);
	bev(seam_rec(i) + ofs + 12, maxs);
}

static void seam_T(int i, float T[3])
{
	bev(seam_rec(i) + S_T, T);
}

static void bad(uint32_t ofs, const char *rule)
{
	sys_error("%s: offset %lu: %s", path, (unsigned long)ofs, rule);
}

static void check_str(uint32_t ofs, uint16_t s, int optional)
{
	if (optional && s == CWG_NONE)
		return;
	if (s >= G.strbytes || !str(s)[0])
		bad(ofs, "string offset");
}

static int finite3(const float v[3])
{
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static void check_box(uint32_t ofs, const uint8_t *p)
{
	float mins[3], maxs[3];
	int k;

	bev(p, mins);
	bev(p + 12, maxs);
	if (!finite3(mins) || !finite3(maxs))
		bad(ofs, "box not finite");
	for (k = 0; k < 3; k++)
		if (mins[k] > maxs[k])
			bad(ofs, "box mins > maxs");
}

/* landmark `name` of map m, -1 none */
static int find_landmark(int m, const char *name)
{
	const uint8_t *mp = G.b + 32 + MAP_B * m;
	int first = be16(mp + 4), n = be16(mp + 6), i;

	for (i = first; i < first + n; i++)
		if (!strcmp(str(be16(G.b + G.o_lm + LM_B * i)), name))
			return i;
	return -1;
}

static void validate_seam(int i)
{
	const uint8_t *p = seam_rec(i);
	uint32_t ofs = G.o_seam + SEAM_B * (uint32_t)i;
	int from = be16(p + S_FROM), to = be16(p + S_TO), flags = be16(p + S_FLAGS);
	int t0 = be16(p + S_TWIN0), nt = be16(p + S_NTWIN);
	int p0 = be16(p + S_POOL0), np = be16(p + S_NPOOL), nd = be16(p + S_NDRAW);
	int la, lb, j, k;
	float delta[3], T[3], oa[3], ob[3];

	if (from >= G.nmaps || to >= G.nmaps || from == to)
		bad(ofs, "seam maps");
	check_str(ofs + S_LANDMARK, be16(p + S_LANDMARK), 0);
	check_str(ofs + S_CTARGET, be16(p + S_CTARGET), 1);
	if (be16(p + S_TRIGGER) >= 1024)
		bad(ofs + S_TRIGGER, "trigger lump");
	if (be16(p + S_KIND) > 1)
		bad(ofs + S_KIND, "seam kind");
	if ((flags & ~15) || !(flags & SEAM_F_CLOSED) == !(flags & SEAM_F_OPEN))
		bad(ofs + S_FLAGS, "seam flags: exactly one of closed / OPEN");
	if (t0 + nt > G.ntwin)
		bad(ofs + S_TWIN0, "twin slice");
	if (p0 + np > G.npool)
		bad(ofs + S_POOL0, "sector-pool slice");
	if (nd > np || (nd && !(flags & SEAM_F_OPEN)))
		bad(ofs + S_NDRAW, "OPEN draw count");
	if (np > WLD_PIN_POOL)
		bad(ofs + S_NPOOL, "sector-pool slice over WLD_PIN_POOL");
	check_box(ofs + S_SWITCH, p + S_SWITCH);
	check_box(ofs + S_CARRY, p + S_CARRY);
	check_box(ofs + S_APPROACH, p + S_APPROACH);
	bev(p + S_DELTA, delta);
	bev(p + S_T, T);
	if (!finite3(delta) || !finite3(T))
		bad(ofs + S_DELTA, "delta / T not finite");
	/* delta = landmark B - landmark A; T = g_B + delta - g_A, rounded once */
	la = find_landmark(from, str(be16(p + S_LANDMARK)));
	lb = find_landmark(to, str(be16(p + S_LANDMARK)));
	if (la < 0 || lb < 0)
		bad(ofs + S_LANDMARK, "landmark not in both maps");
	bev(G.b + G.o_lm + LM_B * la + 4, oa);
	bev(G.b + G.o_lm + LM_B * lb + 4, ob);
	for (k = 0; k < 3; k++)
	{
		const float *ga = wld_map_origin(from), *gb = wld_map_origin(to);

		if (delta[k] != ob[k] - oa[k])
			bad(ofs + S_DELTA, "delta is not landmark B - landmark A");
		if (T[k] != (float)((double)gb[k] + (double)delta[k] - (double)ga[k]))
			bad(ofs + S_T, "T is not g_B + delta - g_A");
	}
	/* each entity at most once in the slice */
	for (j = t0; j < t0 + nt; j++)
	{
		int a = G.twins[2 * j], b = G.twins[2 * j + 1], m;

		if (a >= 1024 || b >= 1024)
			bad(G.o_twin + TWIN_B * j, "twin lump");
		for (m = t0; m < j; m++)
			if (G.twins[2 * m] == a || G.twins[2 * m + 1] == b)
				bad(G.o_twin + TWIN_B * j, "entity twice in a seam's twins");
	}
}

void seam_init(void)
{
	uint32_t rom, size, cap = mem_region_bytes(MEM_CAMPAIGN), i;
	uint8_t *b = mem_region(MEM_CAMPAIGN);

	memset(&G, 0, sizeof(G));
	memset(&S, 0, sizeof(S));
	S.home = S.near = S.armed = S.pending = S.aliased = S.pinned = -1;
	memset(S.lmap, 0xFF, sizeof(S.lmap));
	rom = sys_cart_addr(path, &size);
	if (!rom)
		return;
	if (size < 32 || ((size + 1) & ~1u) > cap)
		sys_error("%s: %lu B does not fit the campaign region (%lu B): raise "
			"HL64_CAMPAIGN_KB", path, (unsigned long)size, (unsigned long)cap);
	io_read_sync(b, rom, (size + 1) & ~1u, "cwg");
	G.b = b;
	G.size = size;
	if (be32(b) != CWG_MAGIC || be16(b + 4) != 3 || be16(b + 6))
		bad(0, "not CWG3");
	G.nmaps = be16(b + 8);
	G.nlm = be16(b + 10);
	G.nln = be16(b + 12);
	G.start = be16(b + 14);
	G.train = be16(b + 16);
	G.ncomp = be16(b + 18);
	G.nseam = be16(b + 20);
	G.ntwin = be16(b + 22);
	G.npool = be16(b + 24);
	G.strbytes = be32(b + 28);
	if (G.nmaps == CWG_NONE || G.nlm == CWG_NONE || G.nln == CWG_NONE || G.nseam == CWG_NONE
		|| G.ntwin == CWG_NONE || G.npool == CWG_NONE || G.ncomp == CWG_NONE)
		bad(8, "count 0xFFFF");
	G.o_lm = 32 + MAP_B * (uint32_t)G.nmaps;
	G.o_ln = G.o_lm + LM_B * (uint32_t)G.nlm;
	G.o_seam = G.o_ln + LINK_B * (uint32_t)G.nln;
	G.o_twin = G.o_seam + SEAM_B * (uint32_t)G.nseam;
	G.o_pool = G.o_twin + TWIN_B * (uint32_t)G.ntwin;
	G.o_str = G.o_pool + POOL_B * (uint32_t)G.npool;
	if ((uint64_t)G.o_str + G.strbytes != size || !G.strbytes || b[size - 1])
		bad(28, "table sizes");
	if (be16(b + 26))
		bad(26, "reserved");
	if (G.nmaps != wld_map_count())
		bad(8, "map count differs from the world's map table");
	if ((G.start != CWG_NONE && G.start >= G.nmaps) || (G.train != CWG_NONE && G.train >= G.nmaps))
		bad(14, "start / training map");
	for (i = 0; i < G.nmaps; i++)
	{
		const uint8_t *m = b + 32 + MAP_B * i;
		uint32_t ofs = 32 + MAP_B * i;
		float g[3];
		int k;

		check_str(ofs, be16(m), 0);
		if (strcmp(str(be16(m)), wld_map_name((int)i)))
			bad(ofs, "map name differs from the world's map table");
		if (be16(m + 2) >= G.ncomp)
			bad(ofs + 2, "component");
		if (be16(m + 4) + be16(m + 6) > G.nlm)
			bad(ofs + 4, "landmark range");
		if (be16(m + 8) + be16(m + 10) > G.nln)
			bad(ofs + 8, "link range");
		bev(m + 12, g);
		for (k = 0; k < 3; k++)
			if (g[k] != wld_map_origin((int)i)[k])
				bad(ofs + 12, "placement differs from the world's map table");
		check_box(ofs + 24, m + 24);
	}
	for (i = 0; i < G.nlm; i++)
	{
		const uint8_t *l = b + G.o_lm + LM_B * i;
		float o[3];

		check_str(G.o_lm + LM_B * i, be16(l), 0);
		if (be16(l + 2))
			bad(G.o_lm + LM_B * i + 2, "landmark reserved");
		bev(l + 4, o);
		if (!finite3(o))
			bad(G.o_lm + LM_B * i + 4, "landmark origin");
	}
	for (i = 0; i < G.nln; i++)
	{
		const uint8_t *l = b + G.o_ln + LINK_B * i;

		if (be16(l) >= G.nmaps)
			bad(G.o_ln + LINK_B * i, "link destination");
		check_str(G.o_ln + LINK_B * i + 2, be16(l + 2), 0);
		if (be16(l + 6) != CWG_NONE && be16(l + 6) >= G.nseam)
			bad(G.o_ln + LINK_B * i + 6, "link seam");
	}
	/* twins and the sector pool in host order (the pool goes to the stream
	 * as is, wld_pin_seam) */
	G.twins = (uint16_t *)(void *)(b + G.o_twin);
	G.pool = (uint16_t *)(void *)(b + G.o_pool);
	for (i = 0; i < 2u * G.ntwin; i++)
		G.twins[i] = be16(b + G.o_twin + 2 * i);
	for (i = 0; i < G.npool; i++)
		G.pool[i] = be16(b + G.o_pool + 2 * i);
	for (i = 0; i < G.nseam; i++)
		validate_seam((int)i);
	S.loaded = 1;
	log_printf("seam: campaign %u maps, %u landmarks, %u links, %u seams, %u twins, "
		"%u pool, %lu B\n", G.nmaps, G.nlm, G.nln, G.nseam, G.ntwin, G.npool,
		(unsigned long)size);
}

/* ---- queries ---------------------------------------------------------------------- */

int seam_graph(seam_graph_t *g)
{
	if (!S.loaded)
		return -1;
	g->nmaps = G.nmaps;
	g->nlandmarks = G.nlm;
	g->nlinks = G.nln;
	g->nseams = G.nseam;
	g->ntwins = G.ntwin;
	g->npool = G.npool;
	g->start = G.start;
	g->train = G.train;
	g->components = G.ncomp;
	g->strbytes = G.strbytes;
	g->size = G.size;
	return 0;
}

int seam_map(int map, seam_map_t *m)
{
	const uint8_t *p;

	if (!S.loaded || map < 0 || map >= G.nmaps)
		return -1;
	p = G.b + 32 + MAP_B * map;
	m->name = str(be16(p));
	m->component = be16(p + 2);
	m->first_landmark = be16(p + 4);
	m->nlandmarks = be16(p + 6);
	m->first_link = be16(p + 8);
	m->nlinks = be16(p + 10);
	bev(p + 12, m->gofs);
	bev(p + 24, m->mins);
	bev(p + 36, m->maxs);
	return 0;
}

int seam_landmark(int i, const char **name, float org[3])
{
	const uint8_t *p;

	if (!S.loaded || i < 0 || i >= G.nlm)
		return -1;
	p = G.b + G.o_lm + LM_B * i;
	*name = str(be16(p));
	bev(p + 4, org);
	return 0;
}

int seam_link(int i, uint16_t *dest, const char **landmark, uint16_t *spawnflags, uint16_t *seam)
{
	const uint8_t *p;

	if (!S.loaded || i < 0 || i >= G.nln)
		return -1;
	p = G.b + G.o_ln + LINK_B * i;
	*dest = be16(p);
	*landmark = str(be16(p + 2));
	*spawnflags = be16(p + 4);
	*seam = be16(p + 6);
	return 0;
}

int seam_info(int i, seam_info_t *s)
{
	const uint8_t *p;
	int k;

	if (!S.loaded || i < 0 || i >= G.nseam)
		return -1;
	p = seam_rec(i);
	s->from = be16(p + S_FROM);
	s->to = be16(p + S_TO);
	s->landmark = str(be16(p + S_LANDMARK));
	s->trigger = be16(p + S_TRIGGER);
	s->changetarget = be16(p + S_CTARGET) == CWG_NONE ? NULL : str(be16(p + S_CTARGET));
	s->kind = be16(p + S_KIND);
	s->flags = be16(p + S_FLAGS);
	s->ntwin = be16(p + S_NTWIN);
	s->twins = G.twins + 2 * be16(p + S_TWIN0);
	s->npool = be16(p + S_NPOOL);
	s->pool = G.pool + be16(p + S_POOL0);
	s->ndraw = be16(p + S_NDRAW);
	bev(p + S_DELTA, s->delta);
	bev(p + S_SWITCH, s->sw_mins);
	bev(p + S_SWITCH + 12, s->sw_maxs);
	bev(p + S_CARRY, s->carry_mins);
	bev(p + S_CARRY + 12, s->carry_maxs);
	bev(p + S_APPROACH, s->ap_mins);
	bev(p + S_APPROACH + 12, s->ap_maxs);
	for (k = 0; k < 3; k++)
		s->need[k] = be32(p + S_NEED + 4 * k);
	s->bcrc = be32(p + S_BCRC);
	bev(p + S_T, s->T);
	return 0;
}

void seam_stats(seam_stats_t *s)
{
	*s = S.st;
}

uint32_t seam_last_switch_frame(void)
{
	return S.last_switch;
}

void seam_prof_line(void)
{
	unsigned long n = S.frames ? S.frames : 1;

	log_printf("prof seam_us avg=%lu max=%lu frames=%lu\n", (unsigned long)S.us_sum / n,
		(unsigned long)S.us_max, (unsigned long)S.frames);
	S.frames = S.us_sum = S.us_max = 0;
}

void seam_mem_line(void)
{
	log_printf("mem seam: campaign used=%lu size=%lu\n", (unsigned long)((G.size + 1) & ~1u),
		(unsigned long)mem_region_bytes(MEM_CAMPAIGN));
}

/* ---- geometry ------------------------------------------------------------------- */

static int box_has(const float mins[3], const float maxs[3], const float p[3])
{
	return p[0] >= mins[0] && p[0] <= maxs[0] && p[1] >= mins[1] && p[1] <= maxs[1]
		&& p[2] >= mins[2] && p[2] <= maxs[2];
}

static int boxes_meet(const float amin[3], const float amax[3], const float bmin[3],
	const float bmax[3])
{
	return amin[0] <= bmax[0] && amax[0] >= bmin[0] && amin[1] <= bmax[1]
		&& amax[1] >= bmin[1] && amin[2] <= bmax[2] && amax[2] >= bmin[2];
}

/* segment a -> b meets the box (slab test) */
static int seg_hits_box(const float a[3], const float b[3], const float mins[3],
	const float maxs[3])
{
	float t0 = 0, t1 = 1;
	int k;

	for (k = 0; k < 3; k++)
	{
		float d = b[k] - a[k], lo, hi;

		if (d == 0)
		{
			if (a[k] < mins[k] || a[k] > maxs[k])
				return 0;
			continue;
		}
		lo = (mins[k] - a[k]) / d;
		hi = (maxs[k] - a[k]) / d;
		if (lo > hi)
		{
			float t = lo;

			lo = hi;
			hi = t;
		}
		if (lo > t0)
			t0 = lo;
		if (hi < t1)
			t1 = hi;
		if (t0 > t1)
			return 0;
	}
	return 1;
}

/* the player's origin and eye, home-local */
static void player_local(float org[3], float eye[3])
{
	const ent_player_t *pl = phy_player();
	int k;

	for (k = 0; k < 3; k++)
	{
		org[k] = pl->origin[k];
		eye[k] = pl->origin[k] + pl->view_ofs[k];
	}
}

static int in_approach(int i, const float org[3])
{
	float mins[3], maxs[3];

	seam_box(i, S_APPROACH, mins, maxs);
	return box_has(mins, maxs, org);
}

/* an entity's position for the continuity match: a brush model's linked
 * box centre (its origin is a displacement), else its origin */
static void ent_pos(int e, float p[3])
{
	const col_ent_t *c = ent_cv(e);
	int k;

	for (k = 0; k < 3; k++)
		p[k] = RES_KIND(ent_rnd(e)->model) == RES_K_SUBMODEL
			? 0.5f * (c->absmin[k] + c->absmax[k]) : c->origin[k];
}

/* the box an entity is drawn in (map-local): a rotated brush model's
 * bounds rotated (the linked box is the cube of the largest extent,
 * grid.c, broadphase only: a 560 u falling rafter 12 u outside the carry
 * box links ±560 around its hinge), else the linked box */
static void draw_box(int e, float mins[3], float maxs[3])
{
	const float k = 3.14159265358979323846f / 180.0f;
	const col_ent_t *c = ent_cv(e);
	float sp, cp, sy, cy, sr, cr, f[3], r[3], u[3], m[3], h[3];
	int i;

	if (RES_KIND(ent_rnd(e)->model) != RES_K_SUBMODEL
		|| (c->angles[0] == 0.0f && c->angles[1] == 0.0f && c->angles[2] == 0.0f))
	{
		memcpy(mins, c->absmin, 3 * sizeof(float));
		memcpy(maxs, c->absmax, 3 * sizeof(float));
		return;
	}
	sp = hl_sinf(c->angles[0] * k);
	cp = hl_cosf(c->angles[0] * k);
	sy = hl_sinf(c->angles[1] * k);
	cy = hl_cosf(c->angles[1] * k);
	sr = hl_sinf(c->angles[2] * k);
	cr = hl_cosf(c->angles[2] * k);
	/* model axes in the map: x = f, y = -r, z = u (HLSDK AngleVectors) */
	f[0] = cp * cy;
	f[1] = cp * sy;
	f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy;
	r[1] = -sr * sp * sy - cr * cy;
	r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy;
	u[1] = cr * sp * sy - sr * cy;
	u[2] = cr * cp;
	for (i = 0; i < 3; i++)
	{
		m[i] = 0.5f * (c->mins[i] + c->maxs[i]);
		h[i] = 0.5f * (c->maxs[i] - c->mins[i]);
	}
	for (i = 0; i < 3; i++)
	{
		float o = c->origin[i] + m[0] * f[i] - m[1] * r[i] + m[2] * u[i];
		float x = h[0] * fabsf(f[i]) + h[1] * fabsf(r[i]) + h[2] * fabsf(u[i]);

		mins[i] = o - x - 1.0f;	/* grid.c's 1 u */
		maxs[i] = o + x + 1.0f;
	}
}

/* rendered (a model, not EF_NODRAW) live entity of layer `slot` drawn
 * into the box (map-local) */
static int cont_member(int e, int slot, const float mins[3], const float maxs[3])
{
	float dmin[3], dmax[3];

	if (!ent_is_live(e) || ent_idc(e)->layer != slot || ent_rnd(e)->model == RES_NONE
		|| (ent_rnd(e)->effects & EF_NODRAW))
		return 0;
	draw_box(e, dmin, dmax);
	return boxes_meet(dmin, dmax, mins, maxs);
}

/* ---- the layers ---------------------------------------------------------------- */

/* the layers the seams know open (home, neighbours; loading ones too:
 * wld_layer(slot) is NULL until a layer is ready), by map */
static void known_add(int map)
{
	int k;

	for (k = 0; k < HL64_WORLD_LAYERS && S.lmap[k] != map; k++)
		;
	if (k < HL64_WORLD_LAYERS)
		return;
	for (k = 0; k < HL64_WORLD_LAYERS; k++)
		if (S.lmap[k] < 0)
		{
			S.lmap[k] = map;
			return;
		}
}

/* `leave`: the leave rule closes it (counted, logged); else an open that
 * failed */
static void close_layer(wld_layer_t *L, int leave)
{
	int slot = L->slot, map = L->map, k;

	wld_unpin_seam(L);
	if (S.hle_open[slot])
	{
		rec_twin_clear(slot);
		rec_map_close(slot);
		S.hle_open[slot] = 0;
	}
	wld_close(L);
	for (k = 0; k < HL64_WORLD_LAYERS; k++)
		if (S.lmap[k] == map)
			S.lmap[k] = -1;
	if (S.aliased >= 0 && seam_to(S.aliased) == map)
		S.aliased = -1;
	if (S.pinned >= 0 && seam_to(S.pinned) == map)
		S.pinned = -1;
	if (!leave)
		return;
	S.st.closes++;
	log_printf("seam: close %s\n", wld_map_name(map));
}

/* counted once per approach; `dead`: no retry until the next approach */
static void open_fail(int si, const char *why, int dead)
{
	if (!S.near_failed)
	{
		S.near_failed = 1;
		S.st.open_fail++;
		log_printf("seam: open_fail %s->%s (%s)\n", wld_map_name(seam_from(si)),
			wld_map_name(seam_to(si)), why);
	}
	if (dead)
		S.near_dead = 1;
}

/* B of the selected seam: layer, HLE slot, twin aliases and pins; 1 ready */
static int neighbour(int si)
{
	int to = seam_to(si), k, r;
	wld_layer_t *L = wld_layer_of_map(to);
	seam_info_t s;

	if (S.near_dead)
		return 0;
	if (!L)
	{
		int open = 0, other = -1;

		for (k = 0; k < HL64_WORLD_LAYERS; k++)
		{
			if (S.lmap[k] < 0)
				continue;
			if (!wld_layer_of_map(S.lmap[k]))
			{
				S.lmap[k] = -1;		/* closed elsewhere */
				continue;
			}
			open++;
			if (S.lmap[k] != S.home)
				other = S.lmap[k];
		}
		if (open >= HL64_WORLD_LAYERS)
		{
			wld_layer_t *N = other >= 0 ? wld_layer_of_map(other) : NULL;
			int live;

			if (!N)
				return 0;
			/* N's layer is needed now: what Rec's distance release left
			 * far away refused (quiescence) and goes with the closing
			 * release (rec_map_close); entities within the release radius
			 * of the player keep N open */
			live = S.hle_open[N->slot] ? rec_slot_held(N->slot) : 0;
			if (live)
			{
				char why[96];

				/* retried every frame: Rec releases N's entities by
				 * distance. A wait until it holds up a switch (B's
				 * changelevel touched or used: pending), then a failure */
				snprintf(why, sizeof(why), "both layers in use: %s held=%d",
					wld_map_name(N->map), live);
				if (S.pending == si)
					open_fail(si, why, 0);
				else if (!S.wait_frame)
				{
					S.wait_frame = eng_frame_count() + 1;
					S.st.open_wait++;
					log_printf("seam: open_wait %s->%s (%s)\n", wld_map_name(seam_from(si)),
						wld_map_name(to), why);
				}
				return 0;
			}
			close_layer(N, 1);
		}
		if (!(L = wld_open_async(to)))
		{
			open_fail(si, "no layer slot / layer-directory room", 1);
			return 0;
		}
		known_add(to);
		log_printf("seam: open %s layer=%d\n", wld_map_name(to), L->slot);
		if (S.wait_frame)
		{
			log_printf("seam: open_wait %s ended after %lu frames\n", wld_map_name(to),
				(unsigned long)(eng_frame_count() + 1 - S.wait_frame));
			S.wait_frame = 0;
		}
	}
	known_add(to);
	r = wld_layer_ready(L);
	if (r < 0)
	{
		open_fail(si, "layer open failed", 1);
		close_layer(L, 0);
		return 0;
	}
	if (!r)
		return 0;
	seam_info(si, &s);
	if (L->hdr.dir_crc != s.bcrc)
	{
		char why[96];

		/* sector indices belong to the cooked package: reject the seam */
		snprintf(why, sizeof(why), "directory CRC %08lx, seam %08lx: seam rejected",
			(unsigned long)L->hdr.dir_crc, (unsigned long)s.bcrc);
		open_fail(si, why, 1);
		seam_rec(si)[S_FLAGS] = 0;
		seam_rec(si)[S_FLAGS + 1] = 0;
		if (S.pending == si)
			S.pending = -1;
		if (S.armed == si)
			S.armed = -1;
		return 0;
	}
	/* B's seam pins as soon as its layer is ready (residency only; the
	 * records' readiness may wait on the open gate) */
	if (S.pinned != si)
	{
		float mins[3], maxs[3];

		wld_pin_seam(L, s.pool, s.npool);
		/* B's collision at the arrival: the switch eye box in B's frame
		 * minus the view offset, as player boxes (hull 0) */
		for (k = 0; k < 3; k++)
		{
			mins[k] = s.sw_mins[k] + s.delta[k] - (k == 2 ? VIEW_Z : 0) + phy_pm_mins[0][k];
			maxs[k] = s.sw_maxs[k] + s.delta[k] - (k == 2 ? VIEW_Z : 0) + phy_pm_maxs[0][k];
		}
		wld_pin_seam_window(L, mins, maxs);
		S.pinned = si;
	}
	if (!S.hle_open[L->slot])
	{
		rec_map_open(L->slot);
		S.hle_open[L->slot] = 1;
	}
	if (!rec_map_ready(L->slot))
		return 0;
	if (S.aliased != si)
	{
		const wld_layer_t *A = wld_layer_of_map(s.from);

		/* every pair before any crossing copies a twin */
		for (k = 0; A && k < s.ntwin; k++)
			rec_twin_alias(GID_MAKE(A->slot, s.twins[2 * k]),
				GID_MAKE(L->slot, s.twins[2 * k + 1]));
		S.aliased = si;
	}
	return 1;
}

/* the seam the player is at: pending, armed, else the nearest approach
 * box (by switch-box centre) holding the origin */
static int select_seam(const float org[3])
{
	int i, best = -1;
	float bestd = 0;

	if (S.pending >= 0)
		return S.pending;
	if (S.armed >= 0)
		return S.armed;
	for (i = 0; i < G.nseam; i++)
	{
		float mins[3], maxs[3], d = 0;
		int k;

		if (seam_from(i) != S.home || !seam_flags(i) || !in_approach(i, org))
			continue;
		seam_box(i, S_SWITCH, mins, maxs);
		for (k = 0; k < 3; k++)
		{
			float c = 0.5f * (mins[k] + maxs[k]) - org[k];

			d += c * c;
		}
		if (best < 0 || d < bestd)
		{
			best = i;
			bestd = d;
		}
	}
	return best;
}

static void approach(int si)
{
	if (si != S.near)
	{
		if (S.near >= 0 && (si < 0 || seam_to(si) != seam_to(S.near)))
		{
			wld_layer_t *L = wld_layer_of_map(seam_to(S.near));

			if (L)
				wld_unpin_seam(L);
			S.aliased = S.pinned = -1;
		}
		S.near = si;
		S.near_failed = S.near_dead = 0;
		S.wait_frame = 0;
		if (si >= 0)
		{
			S.st.approaches++;
			log_printf("seam: approach %s->%s\n", wld_map_name(seam_from(si)),
				wld_map_name(seam_to(si)));
		}
	}
	S.ready = si >= 0 && neighbour(si);
}

/* the leave rule: a non-home layer that no seam of home leads into from
 * here, with nothing live in its slot */
static void leave(const float org[3])
{
	int k, i;

	for (k = 0; k < HL64_WORLD_LAYERS; k++)
	{
		wld_layer_t *L = S.lmap[k] >= 0 ? wld_layer_of_map(S.lmap[k]) : NULL;

		if (!L)
		{
			S.lmap[k] = -1;
			continue;
		}
		if (L->map == S.home || (S.near >= 0 && seam_to(S.near) == L->map))
			continue;
		for (i = 0; i < G.nseam; i++)
			if (seam_from(i) == S.home && seam_to(i) == L->map && seam_flags(i)
				&& in_approach(i, org))
				break;
		if (i < G.nseam)
			continue;
		if (S.hle_open[L->slot] && rec_slot_live(L->slot))
			continue;
		close_layer(L, 1);
	}
}

/* ---- the crossing ---------------------------------------------------------------- */

/* e is a twin's copy on `side` (0 A, 1 B) of seam s, in slot */
static int is_twin(const seam_info_t *s, int slot, int e, int side)
{
	gid_t16 g = ent_idc(e)->gid;
	int i;

	if (g == GID_NONE || (int)GID_SLOT(g) != slot)
		return 0;
	for (i = 0; i < s->ntwin; i++)
		if (s->twins[2 * i + side] == GID_LUMP(g))
			return 1;
	return 0;
}

static float dist2(const float a[3], const float b[3])
{
	return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])
		+ (a[2] - b[2]) * (a[2] - b[2]);
}

/* The carry-box sets of seam s (A in slot sa, B in sb), B frame: A's
 * members into a (CONT_MAX; more counted in *over), the count; and B's
 * ghosts into g (CONT_MAX; more dropped, never drawn), *ng: B's members
 * that are not a twin's B copy and that no A member of the same class
 * within CONT_MATCH stands for (one for one: A's copy is drawn). */
static int carry_sets(const seam_info_t *s, int sa, int sb, cont_t *a, int *over, int16_t *g,
	int *ng)
{
	uint8_t used[CONT_MAX];
	float bmin[3], bmax[3];
	int e, i, j, k, na = 0, n = 0;

	*over = 0;
	for (k = 0; k < 3; k++)
	{
		bmin[k] = s->carry_mins[k] + s->delta[k];
		bmax[k] = s->carry_maxs[k] + s->delta[k];
	}
	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		if (cont_member(e, sa, s->carry_mins, s->carry_maxs))
		{
			if (na == CONT_MAX)
			{
				(*over)++;
				continue;
			}
			a[na].cls = ent_idc(e)->cls;
			a[na].e = -1;
			ent_pos(e, a[na].pos);
			for (k = 0; k < 3; k++)
				a[na].pos[k] += s->delta[k];
			na++;
		}
		else if (n < CONT_MAX && cont_member(e, sb, bmin, bmax) && !is_twin(s, sb, e, 1))
			g[n++] = (int16_t)e;
	}
	memset(used, 0, sizeof(used));
	for (i = j = 0; i < n; i++)
	{
		float p[3];

		ent_pos(g[i], p);
		for (k = 0; k < na; k++)
			if (!used[k] && a[k].cls == ent_idc(g[i])->cls
				&& dist2(p, a[k].pos) <= CONT_MATCH * CONT_MATCH)
				break;
		if (k < na)
			used[k] = 1;
		else
			g[j++] = g[i];
	}
	*ng = j;
	return na;
}

/* the switch frame: A's carry-box set and B's ghosts (drawn in A's view
 * until now: architecture.md 5.4 item 6), compared next frame */
static void continuity_before(const seam_info_t *s, int sa, int sb)
{
	int16_t g[CONT_MAX];
	int i, k, ng;

	S.cont_slot = sb;
	for (k = 0; k < 3; k++)
	{
		S.cont_mins[k] = s->carry_mins[k] + s->delta[k];
		S.cont_maxs[k] = s->carry_maxs[k] + s->delta[k];
	}
	S.cont_n = carry_sets(s, sa, sb, S.cont, &S.cont_over, g, &ng);
	for (i = 0; i < ng; i++)
	{
		cont_t *c;

		if (S.cont_n == CONT_MAX)
		{
			S.cont_over++;
			continue;
		}
		c = &S.cont[S.cont_n++];
		c->cls = ent_idc(g[i])->cls;
		c->e = g[i];
		ent_pos(g[i], c->pos);
	}
	S.cont_pending = 1;
}

/* the frame after the switch: B's carry-box set against the one before (a
 * ghost matches itself, wherever it walked in the frame; an A entity, a
 * B entity of its class within CONT_MATCH) */
static void continuity_after(void)
{
	uint8_t used[CONT_MAX];
	int e, i, matched = 0, extra = 0, missing;

	S.cont_pending = 0;
	memset(used, 0, sizeof(used));
	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		float p[3];

		if (!cont_member(e, S.cont_slot, S.cont_mins, S.cont_maxs))
			continue;
		ent_pos(e, p);
		for (i = 0; i < S.cont_n; i++)
			if (!used[i] && S.cont[i].e == e && S.cont[i].cls == ent_idc(e)->cls)
				break;
		if (i == S.cont_n)
			for (i = 0; i < S.cont_n; i++)
				if (!used[i] && S.cont[i].e < 0 && S.cont[i].cls == ent_idc(e)->cls
					&& dist2(p, S.cont[i].pos) <= CONT_MATCH * CONT_MATCH)
					break;
		if (i < S.cont_n)
		{
			used[i] = 1;
			matched++;
			continue;
		}
		extra++;
		log_printf("seam: carry-box extra #%d cls=%u at %.1f %.1f %.1f\n", e,
			(unsigned)ent_idc(e)->cls, (double)p[0], (double)p[1], (double)p[2]);
	}
	for (i = 0; i < S.cont_n; i++)
		if (!used[i])
			log_printf("seam: carry-box missing cls=%u at %.1f %.1f %.1f\n",
				(unsigned)S.cont[i].cls, (double)S.cont[i].pos[0],
				(double)S.cont[i].pos[1], (double)S.cont[i].pos[2]);
	missing = S.cont_n - matched + S.cont_over;
	S.st.continuity_bad += (uint32_t)(missing + extra);
	log_printf("seam: continuity carry=%d matched=%d missing=%d extra=%d\n",
		S.cont_n + S.cont_over, matched, missing, extra);
}

/* twin component copy, A copy a -> B copy b (cwg-format.md twins) */
static void twin_copy(int a, int b, gid_t16 ga, gid_t16 gb, const float delta[3])
{
	const col_ent_t *ca = ent_cv(a);
	col_ent_t *cb = ent_cv(b);
	const ent_rnd_t *ra = ent_rnd(a);
	ent_rnd_t *rb = ent_rnd(b);
	res_t model = rb->model;
	uint16_t sectors[4];
	float org[3], oa[3], ob[3];
	int k;

	if (RES_KIND(ra->model) == RES_K_SUBMODEL && RES_KIND(rb->model) == RES_K_SUBMODEL)
	{
		/* a brush model's geometry is map-local: keep its displacement from
		 * the record origin (B's geometry already sits at A's + delta) */
		rec_origin(ga, oa);
		rec_origin(gb, ob);
		for (k = 0; k < 3; k++)
			org[k] = ob[k] + (ca->origin[k] - oa[k]);
	}
	else
		for (k = 0; k < 3; k++)
			org[k] = ca->origin[k] + delta[k];
	memcpy(cb->angles, ca->angles, sizeof(cb->angles));
	cb->rendermode = ca->rendermode;
	ent_set_origin(b, org);
	ent_set_velocity(b, ent_mot(a)->velocity);
	ent_set_avelocity(b, ent_mot(a)->avelocity);
	ent_tm(b)->ltime = ent_tm(a)->ltime;
	ent_think_at(b, ent_tm(a)->nextthink);
	/* render state: B keeps its own model reference and sector cache */
	memcpy(sectors, rb->sectors, sizeof(sectors));
	*rb = *ra;
	rb->model = model;
	memcpy(rb->sectors, sectors, sizeof(sectors));
}

static void cross(int si)
{
	seam_info_t s;
	wld_layer_t *A, *B;
	int sa, sb, e, i, k, frame_layer = eng_frame_layer();
	int ground = phy_player()->groundentity, ground_b = ground == 0 ? 0 : -1;
	int twins = 0, carried = 0;
	const char *why = S.why;

	seam_info(si, &s);
	A = wld_layer_of_map(s.from);
	B = wld_layer_of_map(s.to);
	sa = A->slot;
	sb = B->slot;
	continuity_before(&s, sa, sb);

	/* (1) carried entities: into B's frame and slot */
	eng_set_frame_layer(sa);
	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		const col_ent_t *c;
		float org[3];

		if (ent_idc(e)->layer != sa || is_twin(&s, sa, e, 0))
			continue;
		c = ent_cv(e);
		if (!boxes_meet(c->absmin, c->absmax, s.carry_mins, s.carry_maxs) || !game_carry(e))
			continue;
		for (k = 0; k < 3; k++)
			org[k] = c->origin[k] + s.delta[k];
		ent_idc(e)->layer = (int8_t)sb;
		eng_set_frame_layer(sb);
		ent_set_origin(e, org);
		if (rec_rehome(e, sb) < 0)
			log_printf("seam: carried #%d: no runtime id (table full)\n", e);
		game_carried(e, s.delta);
		eng_set_frame_layer(sa);
		if (ground == e)
			ground_b = e;
		carried++;
	}

	/* (2) twins: B's copy takes A's state, A's retires silently */
	for (i = 0; i < s.ntwin; i++)
	{
		gid_t16 ga = GID_MAKE(sa, s.twins[2 * i]), gb = GID_MAKE(sb, s.twins[2 * i + 1]);
		int a = rec_entity(ga), b;

		if (a < 0)
		{
			int st = rec_state(ga);

			/* A's copy dehydrated: its delta holds the state B takes
			 * (aliased, B's copy never hydrated on its own) */
			if (st == REC_DRY)
				a = rec_wake(ga);
			/* removed for good in A: gone in B too */
			else if (st == REC_KILLED && rec_state(gb) != REC_KILLED)
			{
				rec_kill(gb);
				log_printf("seam: twin %u:%u removed in %s: %u:%u killed\n",
					(unsigned)sa, (unsigned)GID_LUMP(ga), wld_map_name(s.from),
					(unsigned)sb, (unsigned)GID_LUMP(gb));
			}
			/* pending: B's own record spawns the same state */
			if (a < 0)
				continue;
		}
		eng_set_frame_layer(sb);
		b = rec_wake(gb);
		if (b < 0)
		{
			log_printf("seam: twin %u:%u -> %u:%u: B copy refused, A copy kept\n",
				(unsigned)sa, (unsigned)GID_LUMP(ga), (unsigned)sb, (unsigned)GID_LUMP(gb));
			eng_set_frame_layer(sa);
			continue;
		}
		twin_copy(a, b, ga, gb, s.delta);
		game_twin_copy(a, b);
		if (ground == a)
			ground_b = b;
		eng_set_frame_layer(sa);
		if (!rec_release(a))
			log_printf("seam: twin %u:%u: A copy not released (game refused)\n",
				(unsigned)sa, (unsigned)GID_LUMP(ga));
		twins++;
	}

	/* (3) the player, (4) the stream: the global position moves by T */
	if (phy_player_rebase(s.to, s.T, ground_b))
		sys_error("seam: %s->%s: player re-base without B's layer", wld_map_name(s.from),
			wld_map_name(s.to));
	wld_stream_rebase(s.T);
	wld_unpin_seam(B);		/* pins lapse on arrival */
	S.home = s.to;

	/* (5) changetarget in B (HLSDK CFireAndDie after changedelay) */
	eng_set_frame_layer(sb);
	if (s.changetarget)
	{
		name_t n = rec_name_find(sb, s.changetarget);
		float delay = 0;

		rec_key_f(GID_MAKE(sa, s.trigger), RK_CHANGEDELAY, &delay);
		if (n == NAME_NONE)
			log_printf("seam: changetarget %s not in %s\n", s.changetarget,
				wld_map_name(s.to));
		else if (delay > 0)
			rec_fire_at((float)eng_clock_time() + delay, n, NAME_NONE, -1, -1,
				USE_TOGGLE, 0);
		else
			rec_fire(n, -1, -1, GID_NONE, USE_TOGGLE, 0);
	}
	eng_set_frame_layer(frame_layer);

	/* (6) changelevel touches ignored until a frame with none */
	S.settle = 1;
	S.settle_tick = S.tick;
	S.pending = S.armed = S.near = S.aliased = S.pinned = -1;
	S.ready = 0;
	S.why = NULL;
	S.have_eye = 0;
	S.st.switches++;
	S.st.twins += (uint32_t)twins;
	S.st.carried += (uint32_t)carried;
	if (!strncmp(why, "forced", 6))
		S.st.forced++;
	S.last_switch = eng_frame_count();
	log_printf("seam: switch %s->%s landmark %s (%s) frame=%lu twins=%d carried=%d\n",
		wld_map_name(s.from), wld_map_name(s.to), s.landmark, why,
		(unsigned long)S.last_switch, twins, carried);
}

/* ---- approach ghosts ------------------------------------------------------------- */

/* This frame's ghosts (seam_ghost): the approached seam with B ready. What
 * the arrival shows in B's carry box shows in A's view before the switch
 * (A's own copies and the twins draw there already). */
static void ghost_frame(void)
{
	cont_t a[CONT_MAX];
	seam_info_t s;
	const wld_layer_t *A, *B;
	int k, over;

	S.ghost_n = 0;
	if (S.near < 0 || !S.ready || seam_info(S.near, &s) || !(A = wld_layer_of_map(s.from))
		|| !(B = wld_layer_of_map(s.to)) || !S.hle_open[B->slot])
		return;
	carry_sets(&s, A->slot, B->slot, a, &over, S.ghost, &S.ghost_n);
	S.ghost_map = s.from;
	S.ghost_slot = B->slot;
	for (k = 0; k < 3; k++)
	{
		S.ghost_off[k] = -s.T[k];
		S.ghost_mins[k] = s.carry_mins[k] + s.delta[k];
		S.ghost_maxs[k] = s.carry_maxs[k] + s.delta[k];
	}
}

/* ---- game -> engine, frame ------------------------------------------------------- */

int seam_changelevel(gid_t16 trigger, int use)
{
	const wld_layer_t *H;
	float org[3], eye[3], mins[3], maxs[3];
	int i, si = -1;

	if (!S.loaded || S.home < 0 || trigger == GID_NONE || !phy_player_active())
		return 0;
	H = wld_layer_of_map(S.home);
	if (!H || (int)GID_SLOT(trigger) != H->slot)
		return 0;
	for (i = 0; i < G.nseam; i++)
		if (seam_from(i) == S.home && be16(seam_rec(i) + S_TRIGGER) == GID_LUMP(trigger)
			&& seam_flags(i))
		{
			si = i;
			break;
		}
	if (si < 0)
		return 0;
	if (S.pending >= 0)
		return 1;
	if (!use && S.settle)
	{
		S.settle_tick = S.tick;
		return 0;
	}
	player_local(org, eye);
	seam_box(si, S_SWITCH, mins, maxs);
	if (use || be16(seam_rec(si) + S_KIND) == 1 || (seam_flags(si) & SEAM_F_OPEN)
		|| box_has(mins, maxs, eye))
	{
		S.pending = si;
		S.why = use ? "use" : "touch";
		return 1;
	}
	if (S.armed != si)
	{
		S.armed = si;
		S.armed_time = eng_clock_time();
		log_printf("seam: armed %s->%s\n", wld_map_name(S.home), wld_map_name(seam_to(si)));
	}
	S.touch_tick = S.tick;
	return 1;
}

void seam_new_game(int map)
{
	const wld_layer_t *L;

	S.home = map;
	S.near = S.armed = S.pending = S.aliased = S.pinned = -1;
	S.why = NULL;
	S.settle = S.have_eye = S.ready = S.near_failed = S.near_dead = S.cont_pending = 0;
	S.wait_frame = 0;
	S.ghost_n = 0;
	memset(S.hle_open, 0, sizeof(S.hle_open));
	memset(S.lmap, 0xFF, sizeof(S.lmap));
	if (map >= 0)
		S.lmap[0] = map;
	L = map >= 0 ? wld_layer_of_map(map) : NULL;
	if (L)
		S.hle_open[L->slot] = 1;	/* rec_new_campaign opened it */
}

void seam_frame(void)
{
	float org[3], eye[3];
	uint64_t t0;
	uint32_t us;

	S.ghost_n = 0;
	if (!S.loaded || !phy_player_active())
		return;
	t0 = sys_time_us();
	if (phy_player_map() != S.home)
	{
		/* a home change that was not a crossing (New Game, setpos) */
		log_printf("seam: home %s (not a crossing)\n", wld_map_name(phy_player_map()));
		seam_new_game(phy_player_map());
	}
	if (S.cont_pending)
		continuity_after();
	if (S.settle && S.settle_tick != S.tick)
		S.settle = 0;
	player_local(org, eye);

	/* the armed touch seam: switch box, forced, disarm */
	if (S.pending < 0 && S.armed >= 0)
	{
		float mins[3], maxs[3];

		seam_box(S.armed, S_SWITCH, mins, maxs);
		if (!in_approach(S.armed, org))
		{
			log_printf("seam: disarmed %s->%s\n", wld_map_name(S.home),
				wld_map_name(seam_to(S.armed)));
			S.armed = -1;
		}
		else if (box_has(mins, maxs, eye)
			|| (S.have_eye && seg_hits_box(S.prev_eye, eye, mins, maxs)))
		{
			S.pending = S.armed;
			S.why = "touch";
		}
		else if (S.touch_tick != S.tick)
		{
			S.pending = S.armed;
			S.why = "forced: left the trigger outside the switch box";
		}
		else if (eng_clock_time() - S.armed_time > ARMED_TOUCH_MAX)
		{
			S.pending = S.armed;
			S.why = "forced: in the trigger outside the switch box";
		}
	}

	approach(select_seam(org));
	if (S.pending >= 0)
	{
		if (S.near == S.pending && S.ready)
		{
			cross(S.pending);
			player_local(org, eye);
		}
		else
			S.st.late++;
	}
	leave(org);
	ghost_frame();
	memcpy(S.prev_eye, eye, sizeof(eye));
	S.have_eye = 1;
	S.tick++;

	us = (uint32_t)(sys_time_us() - t0);
	S.frames++;
	S.us_sum += us;
	if (us > S.us_max)
		S.us_max = us;
}

int seam_ghost(int e, int map, float off[3])
{
	int i;

	if (!S.ghost_n || map != S.ghost_map
		|| !cont_member(e, S.ghost_slot, S.ghost_mins, S.ghost_maxs))
		return 0;
	for (i = 0; i < S.ghost_n; i++)
		if (S.ghost[i] == e)
		{
			memcpy(off, S.ghost_off, sizeof(S.ghost_off));
			return 1;
		}
	return 0;
}

int seam_ghosts(int map, int16_t ents[SEAM_GHOSTS], float off[3])
{
	int i, n = 0;

	if (!S.ghost_n || map != S.ghost_map)
		return 0;
	for (i = 0; i < S.ghost_n && n < SEAM_GHOSTS; i++)
		if (cont_member(S.ghost[i], S.ghost_slot, S.ghost_mins, S.ghost_maxs))
			ents[n++] = S.ghost[i];
	memcpy(off, S.ghost_off, sizeof(S.ghost_off));
	return n;
}

int seam_anchors(float (*out)[3], int max)
{
	const float *gm;
	const ent_player_t *pl;
	float T[3];
	int n = 0, k;

	if (!phy_player_active() || phy_player_map() >= wld_map_count() || max < 1)
		return 0;
	gm = wld_map_origin(phy_player_map());
	pl = phy_player();
	for (k = 0; k < 3; k++)
		out[0][k] = pl->origin[k] + gm[k];
	n = 1;
	if (S.loaded && S.near >= 0 && max > 1)
	{
		seam_T(S.near, T);
		for (k = 0; k < 3; k++)
			out[1][k] = out[0][k] + T[k];
		n = 2;
	}
	return n;
}
