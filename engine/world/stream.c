/* SPDX-License-Identifier: MIT */
/*
 * stream.c -- residency classes and the change-driven target set
 * (docs/architecture.md 5.2-5.4): open (the eye leaf's visible set and
 * the drawn eye's neighbouring leaves -- bob, punch, shake -- each with
 * the MOVR models the renderer can draw there: ENTS placements and live
 * brush entities, swept ahead; the collision window and the brush models
 * in it), pin (a seam's B set, window and arrival-eye set, point pins of
 * teleport / route destinations), prefetch (eye-leaf sets at eye + v *
 * {0.25, 0.5, 1} s, v the faster of the velocity and the observed motion,
 * the collision window + v * 0.5 s); teleport windows and pop-in
 * accounting; the want peak per pool; the async layer opens' stepping.
 *
 * The behaviour follows the old tree's n64/port/w64_stream.c as an oracle
 * (audit entry n64/port/w64_stream.c: CLEAN + QUAKE-INTERFACE): want
 * classes, pins with lapse, prefetch points, teleport tracking. Written
 * from the specs; none of its Quake bindings (edicts and their v.* fields,
 * cl_entities / cls / cl, r_refdef / vpn, sv_player, sv., Sys_Printf) is
 * used: the inputs are wld_stream_frame's home map, eye and velocity, the
 * collision window of engine/coll (wld_stream_coll) and the renderer's
 * missing count (wld_stream_missing).
 *
 * The target set is rebuilt (a new wld_tgen generation; every chunk named
 * with its strongest class) only when an input changed: the home layer's
 * eye leaf, the player's grid cell, the velocity bucket, the pins, the
 * layer set or the player's collision window's cells / brush models.
 * Otherwise the previous generation stands (`reassert`): the LRU never
 * evicts a chunk of it for a weaker class; the moving entities' windows
 * and hydration windows that changed are named into it (`delta`), and a
 * naming that could not be queued (no request slot / no room) is retried
 * in it.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/render/ren.h"
#include "engine/world/wld_int.h"

#define PREF_COLL_S	0.5f		/* collision window + v * 0.5 s */
#define PIN_LAPSE_S	3.0		/* pins lapse after 3 s ... */
#define PIN_ARRIVE	64.0f		/* ... or when the player arrives */
#define TELE_WINDOW_S	1.0		/* pop-in allowed for 1 s after a teleport */
#define TELE_SLACK	32.0f		/* u a frame's move may exceed v * dt by */
#define RIDE_SLACK	32.0f		/* u/s observed motion must exceed the velocity by */
#define VIEW_PTS	7		/* the drawn eye: last offset, +-VIEW_SLACK per axis */
#define VIEW_SLACK	24.0f		/* view bob / punch / shake reach (u) */
#define TELE_DT		0.1f		/* longest frame (HL64_DT_MAX_US) */
#define VEL_BUCKET	64.0f		/* u/s per velocity bucket step */
#define COL_WIN		128.0f		/* collision window margin (engine/coll COL_WINDOW) */
#define POINT_PINS	4
#define COLL_MODELS	64
#define EYE_Z		28.0f		/* view offset of a point pin's eye */
#define MOVER_LEAD_S	0.5f		/* a moving brush entity's box swept this far ahead */
#define MOVER_MARGIN	128.0f		/* ... and grown this much (a door about to move) */
#define POPIN_LOGS	16		/* `stream: popin` lines logged at most */
#define PVS_STEP	16.0f		/* u between the eye path's PVS row samples */
#define PVS_SAMPLES	24		/* ... at most (1 s ahead) */
#define PVS_NEAR	24.0f		/* u around the eye whose leaves' rows are named */
#define PVS_STEPUP	20.0f		/* ... and above it (a stair step: 18 u) */
#define DUCK_DZ		16.0f		/* standing eye - ducked eye (view offsets 28 / 12) */
#define ENT_WINDOWS	64		/* moving entities with a collision window */
#define ENT_LEAD_S	0.5f		/* an entity's window + v * 0.5 s */
#define EW_MODELS	12		/* brush models of an entity's window */
#define EW_REQUERY	30		/* frames between a window's model queries */
#define GOAL_SPAN	1024.0f		/* a route span is prefetched this far at most */
#define GOAL_MARGIN	40.0f		/* ... around the hull (a step up and down: 2 x 18 u) */
#define SPAWN_WANTS	26		/* hydration windows per layer (wld_stream_spawns: rec's
					   16 candidates, 6 ring, 4 pending spawners) */
#define SPAWN_MARGIN	32.0f		/* around a record's spawn box */
#define TELE_PINS	4		/* teleport destinations pinned at once */
#define SEEN_MODELS	64		/* brush entities' hulls named in the eye's sectors */
#define RETRY_MAX	128		/* open / pin namings retried (S.retry) */
#define PREF_RETRY	8		/* frames before a short prefetch naming is redone */
#define DSPAWN_MAX	(2 * SPAWN_WANTS)	/* new spawn windows named per frame (name_delta) */

typedef struct {
	uint8_t		on, win;
	uint16_t	n;
	uint16_t	e[WLD_PIN_POOL];
	float		box[6];		/* B-local player box (window) */
} seam_pin_t;

typedef struct {
	uint8_t		live;
	int16_t		map;
	float		org[3];
	double		t0;
} point_pin_t;

/* an entity's collision window (ent_window): grid cell ranges per layer
 * slot (cell_range; [0] > [3]: none) of its box and route span, and its
 * brush models */
typedef struct {
	int16_t		e, slot;
	int32_t		r[HL64_WORLD_LAYERS][6], g[HL64_WORLD_LAYERS][6];
	int32_t		nm;
	uint32_t	m[EW_MODELS];
} ewin_t;

typedef struct {
	int16_t		slot, pad;
	float		org[3];		/* the player origin it gives (map-local) */
} tele_pin_t;

/* a hydration candidate's spawn window (spawn_cells) */
typedef struct {
	int32_t		r[6];
	int32_t		sub;		/* its own brush model, -1 none */
} spawn_want_t;

static struct {
	/* inputs of the standing target set */
	int		home_slot;
	int32_t		leaf, cell;
	int32_t		vleaf[VIEW_PTS];	/* the drawn eye's other leaves */
	int		nvleaf;
	float		voff[3];	/* drawn eye - player eye (last frame) */
	int32_t		vb[3];
	uint32_t	pin_gen_seen, layer_gen_seen, coll_gen_seen;
	uint8_t		valid;
	/* namings that could not be queued (architecture.md 5.3: retried,
	 * not re-derived): open / pin chunks and MOVRs waiting to land on
	 * the retry list (over: it filled, the whole set is named again);
	 * prefetch ones degrade (pref_short: the whole set again after
	 * PREF_RETRY frames or an input change) */
	uint32_t	retry[RETRY_MAX];
	int		nretry;
	uint8_t		retry_over, pref_short;
	uint32_t	named_frame;	/* wld_frame_no of the last whole naming */
	uint32_t	measured_gen;	/* wld_tgen whose want was measured */
	/* this frame's inputs */
	float		eye[3], vel[3];
	/* collision window (wld_stream_coll) */
	uint8_t		have_coll;
	float		clo[3], chi[3];
	int32_t		ccell[HL64_WORLD_LAYERS][6];
	uint32_t	cmodel[COLL_MODELS];
	int		ncmodel;
	uint32_t	coll_gen;
	/* moving entities' collision windows, in entity order (ent_scan) */
	ewin_t		ew[ENT_WINDOWS];
	int		nwin;
	/* hydration windows per layer (wld_stream_spawns) */
	spawn_want_t		spawn[HL64_WORLD_LAYERS][SPAWN_WANTS];
	int		nspawn[HL64_WORLD_LAYERS];
	uint8_t		spawn_gate[HL64_WORLD_LAYERS];	/* wld_stream_spawn_gate */
	/* reachable teleport destinations (ent_scan) */
	tele_pin_t	tele[TELE_PINS];
	int		ntele;
	/* hulls of the brush entities the home eye leaf sees (ent_scan) */
	uint32_t	seen[SEEN_MODELS];
	int		nseen;
	/* changes named into the standing generation (name_delta) instead
	 * of a rebuild: the windows of S.ew that moved this frame (indices,
	 * ent_scan), new spawn windows (wld_stream_spawns; their slot), the
	 * seen hulls changed */
	uint8_t		dwin[ENT_WINDOWS];
	int		ndwin;
	spawn_want_t	dspawn[DSPAWN_MAX];
	int8_t		dspawn_slot[DSPAWN_MAX];
	int		ndspawn;
	uint8_t		dseen;
	/* the live brush entities (SUBMODEL model) of the open layers, in
	 * entity order (ent_scan, each frame): each_mover's entity part */
	uint16_t	brush[HL64_MAX_ENTITIES];
	int		nbrush;
	/* pins */
	seam_pin_t	seam[HL64_WORLD_LAYERS];
	point_pin_t	point[POINT_PINS];
	uint32_t	pin_gen;
	/* teleports */
	uint8_t		have_prev;
	float		prev[3], prev_speed, rebase[3];
	double		prev_t;
	double		tele_until;
	uint32_t	popin_logs;
	wld_stream_stats_t st;
} S;

static float len3(const float v[3]);

/* ---- naming helpers --------------------------------------------------------- */

/* Retry list entries: RETRY_MOVR | slot << 16 | BSP model (a MOVR whose
 * pages follow when it lands), or slot << 16 | chunk; the chunk's retry
 * byte holds RETRY_CHUNK / RETRY_PAGES while it is listed. */
#define RETRY_MOVR	0x80000000u
#define RETRY_CHUNK	1
#define RETRY_PAGES	2

static void retry_add(wld_layer_t *L, int c, uint32_t e, int flag)
{
	if (L->ch[c].retry & flag)
		return;
	if (S.nretry == RETRY_MAX)
	{
		S.retry_over = 1;
		return;
	}
	L->ch[c].retry |= (uint8_t)flag;
	S.retry[S.nretry++] = e;
}

/* A chunk that could not be queued: listed when open / pin, degraded
 * when prefetch (its class: the strongest naming of the generation). */
static void failed(wld_layer_t *L, int c, uint32_t e, int flag)
{
	if (L->ch[c].cls <= WLD_CLS_PREFETCH)
		S.pref_short = 1;
	else
		retry_add(L, c, e, flag);
}

static void name(wld_layer_t *L, int c, int cls)
{
	if (!wld_name(L, c, cls))
		failed(L, c, (uint32_t)L->slot << 16 | (uint32_t)c, RETRY_CHUNK);
}

/* A map-local box meets a sector of list (the renderer's mover / entity
 * sector test, without the frustum). */
static int box_seen(const wld_layer_t *L, const float lo[3], const float hi[3],
	const uint16_t *list, int cnt)
{
	int i, k;

	for (i = 0; i < cnt; i++)
	{
		const int16_t *sb = L->sects[list[i]].bbox;

		for (k = 0; k < 3; k++)
			if (lo[k] > sb[k + 3] || hi[k] < sb[k])
				break;
		if (k == 3)
			return 1;
	}
	return 0;
}

/* The bounds of the sectors of list (ub: lo[3], hi[3]); lo > hi: none. */
static void list_bounds(const wld_layer_t *L, const uint16_t *list, int cnt, float ub[6])
{
	int i, k;

	for (k = 0; k < 3; k++)
	{
		ub[k] = 1e30f;
		ub[k + 3] = -1e30f;
	}
	for (i = 0; i < cnt; i++)
	{
		const int16_t *sb = L->sects[list[i]].bbox;

		for (k = 0; k < 3; k++)
		{
			if (sb[k] < ub[k])
				ub[k] = sb[k];
			if (sb[k + 3] > ub[k + 3])
				ub[k + 3] = sb[k + 3];
		}
	}
}

/* box_seen behind the list's bounds (most movers are far from the view) */
static int box_seen_b(const wld_layer_t *L, const float lo[3], const float hi[3],
	const uint16_t *list, int cnt, const float ub[6])
{
	int k;

	for (k = 0; k < 3; k++)
		if (lo[k] > ub[k + 3] || hi[k] < ub[k])
			return 0;
	return box_seen(L, lo, hi, list, cnt);
}

/* The MOVR models the renderer can draw in the sectors of list: the ENTS
 * placements (rest positions, opaque and translucent; a record not yet
 * hydrated, or woken and drawn in the same frame, is drawn there) and the
 * live brush entities of L (where they are now: doors, trains, the
 * elevator; the frame's list from ent_scan, entities freed since
 * skipped). fn(L, sub, cls) per model; 0 when fn returned 0 for any. */
static int each_mover(wld_layer_t *L, const uint16_t *list, int cnt, int cls,
	int (*fn)(wld_layer_t *L, int sub, int cls))
{
	int i, k, ok = 1;
	float ub[6];

	list_bounds(L, list, cnt, ub);
	for (i = 0; i < L->nmovers + L->ntrans_movers; i++)
	{
		const wld_mover_t *mv = &L->movers[i < L->nmovers ? i
			: L->dir->nsub - 1 - (i - L->nmovers)];
		const int16_t *mb = L->subs[mv->sub].bbox;
		float lo[3], hi[3];

		for (k = 0; k < 3; k++)
		{
			lo[k] = mb[k] + mv->origin[k] - MOVER_MARGIN;
			hi[k] = mb[k + 3] + mv->origin[k] + MOVER_MARGIN;
		}
		if (box_seen_b(L, lo, hi, list, cnt, ub) && !fn(L, mv->sub, cls))
			ok = 0;
	}
	for (i = 0; i < S.nbrush; i++)
	{
		int e = S.brush[i];
		res_t m;
		const col_ent_t *cv;
		const float *v;
		float lo[3], hi[3];

		if (!ent_is_live(e) || ent_idc(e)->layer != L->slot)
			continue;
		m = ent_rnd(e)->model;
		if (RES_KIND(m) != RES_K_SUBMODEL || RES_SUB_SLOT(m) != L->slot)
			continue;
		/* swept over its next MOVER_LEAD_S of travel and grown by
		 * MOVER_MARGIN: a door starting to open into the view is
		 * resident when it gets there */
		cv = ent_cv(e);
		v = ent_mot(e)->velocity;
		for (k = 0; k < 3; k++)
		{
			float d = v[k] * MOVER_LEAD_S;

			lo[k] = cv->absmin[k] + (d < 0 ? d : 0) - MOVER_MARGIN;
			hi[k] = cv->absmax[k] + (d > 0 ? d : 0) + MOVER_MARGIN;
		}
		if (box_seen_b(L, lo, hi, list, cnt, ub) && !fn(L, (int)RES_SUB_NUM(m), cls))
			ok = 0;
	}
	return ok;
}

static int name_movr(wld_layer_t *L, int n, int cls);

/* REND + TEXP pages of the sectors leaf sees, and the MOVR chunks (with
 * their pages once landed) of the movers in them. */
static void name_leaf(wld_layer_t *L, int leaf, int cls)
{
	const uint16_t *list;
	int i, k, cnt;

	list = wld_leaf_sectors(L, leaf, &cnt);
	for (i = 0; i < cnt; i++)
	{
		const w64_sector_t *s = &L->sects[list[i]];

		name(L, s->rend_chunk, cls);
		for (k = 0; k < s->page_count; k++)
			name(L, L->dir->texp_first + L->pagepool[s->page_first + k], cls);
	}
	each_mover(L, list, cnt, cls, name_movr);
}

/* A texture's page and every page of its animation / alternate
 * sequences: name them at cls (check 0), or 1 when all are resident
 * (check 1). */
static int tex_pages(wld_layer_t *L, int t, int cls, int check)
{
	int pass, steps, u;

	for (pass = 0; pass < 2; pass++)
	{
		u = pass ? L->texs[t].alternate : t;
		if (pass && (!(L->texs[t].flags & W64_TEXF_ANIM) || u == W64_NONE))
			break;
		for (steps = 0; steps < L->dir->ntex; steps++)
		{
			const w64_tex_t *x = &L->texs[u];

			if (x->page != W64_NONE)
			{
				int c = L->dir->texp_first + x->page;

				if (!check)
					name(L, c, cls);
				else if (L->ch[c].state < WLD_CH_RESIDENT)
					return 0;
			}
			if (!(x->flags & W64_TEXF_ANIM) || x->anim_next == (pass ? L->texs[t].alternate : t))
				break;
			u = x->anim_next;
		}
	}
	return 1;
}

static void name_tex(wld_layer_t *L, int t, int cls)
{
	tex_pages(L, t, cls, 0);
}

/* BSP model n's MOVR chunk, and its pages once it is resident; 0 while it
 * is not (its pages follow when it lands: the retry list). */
static int name_movr(wld_layer_t *L, int n, int cls)
{
	const w64_sub_t *sb;
	const w64_rend_t *r;
	const w64_meshlet_t *m;
	int k;

	if (n >= L->dir->nsub || L->subs[n].kind != W64_SUB_MOVR)
		return 1;
	sb = &L->subs[n];
	name(L, sb->chunk, cls);
	if (L->ch[sb->chunk].state < WLD_CH_RESIDENT)
	{
		failed(L, sb->chunk, RETRY_MOVR | (uint32_t)L->slot << 16 | (uint32_t)n, RETRY_PAGES);
		return 0;
	}
	r = (const w64_rend_t *)L->ch[sb->chunk].data;
	m = (const w64_meshlet_t *)((const uint8_t *)r + 24);
	for (k = 0; k < r->nmeshlets; k++)
		name_tex(L, m[k].tex, cls);
	return 1;
}

/* Grid cell range of a map-local box (c0 > c1 on an axis: empty). */
static void cell_range(const wld_layer_t *L, const float lo[3], const float hi[3], int32_t r[6])
{
	const w64_grid_t *g = L->grid;
	int k;

	for (k = 0; k < 3; k++)
	{
		float a = (lo[k] - g->origin[k]) / (float)g->cell;
		float b = (hi[k] - g->origin[k]) / (float)g->cell;

		r[k] = a < 0 ? 0 : a >= g->dims[k] ? g->dims[k] : (int32_t)a;
		r[k + 3] = b < 0 ? -1 : b >= g->dims[k] ? g->dims[k] - 1 : (int32_t)b;
	}
}

/* COLL lists of the grid cells in range r (cell_range). */
static void name_cells(wld_layer_t *L, const int32_t r[6], int cls)
{
	const w64_grid_t *g = L->grid;
	int x, y, z, i;

	for (z = r[2]; z <= r[5]; z++)
		for (y = r[1]; y <= r[4]; y++)
			for (x = r[0]; x <= r[3]; x++)
			{
				const uint16_t *v = L->glists + L->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

				for (i = 1; i <= v[0]; i++)
					name(L, L->dir->coll_first + v[i], cls);
			}
}

/* COLL lists of the grid cells a map-local box meets. */
static void name_box(wld_layer_t *L, const float lo[3], const float hi[3], int cls)
{
	int32_t r[6];

	cell_range(L, lo, hi, r);
	name_cells(L, r, cls);
}

/* A global box of L's map-local [lo, hi] (col_models_box). */
static int models_of(const wld_layer_t *L, const float lo[3], const float hi[3], uint32_t *out,
	int max)
{
	float a[3], b[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		a[k] = lo[k] + L->gofs[k];
		b[k] = hi[k] + L->gofs[k];
	}
	return col_models_box(a, b, out, max);
}

/* Entity e's collision window (architecture.md 5.3: cells of moving
 * monsters and projectiles, hulls of the brush entities in their box)
 * into w: its linked box + COL_WIN and + v * ENT_LEAD_S, and a moving
 * monster's route span (game_move_goal: the box swept to its waypoint,
 * at most GOAL_SPAN, + GOAL_MARGIN), as grid cell ranges of every open
 * layer; the brush models of both (from prev while its ranges are
 * prev's, a fresh query every EW_REQUERY frames). A moving pusher: its
 * own hull (a push tests the entities it meets against it). Toss
 * entities at rest too (a pusher shoves them through triggers). 0: no
 * window (a point traces hull 0, pinned). */
static int ent_window(int e, wld_layer_t *L, const ewin_t *prev, ewin_t *w)
{
	const col_ent_t *cv = ent_cv(e);
	const float *v = ent_mot(e)->velocity, *av = ent_mot(e)->avelocity;
	float lo[3], hi[3], glo[3], ghi[3], goal[3], d[3], len;
	int k, s, span = 0;

	switch (cv->movetype)
	{
	case MOVETYPE_PUSH:
		if ((v[0] == 0 && v[1] == 0 && v[2] == 0 && av[0] == 0 && av[1] == 0 && av[2] == 0)
			|| cv->model == COL_NONE || COL_MODEL_SLOT(cv->model) != L->slot)
			return 0;
		memset(w, 0, sizeof(*w));
		w->e = (int16_t)e;
		w->slot = (int16_t)L->slot;
		for (s = 0; s < HL64_WORLD_LAYERS; s++)
			w->r[s][0] = w->g[s][0] = 1;	/* no cells */
		w->nm = 1;
		w->m[0] = (uint32_t)L->slot << 16 | COL_MODEL_SUB(cv->model);
		return 1;
	case MOVETYPE_WALK: case MOVETYPE_STEP: case MOVETYPE_FLY: case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE: case MOVETYPE_FLYMISSILE: case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_PUSHSTEP:
		break;
	default:
		return 0;
	}
	for (k = 0; k < 3 && cv->mins[k] == 0 && cv->maxs[k] == 0; k++)
		;
	if (k == 3)
		return 0;
	memset(w, 0, sizeof(*w));
	w->e = (int16_t)e;
	w->slot = (int16_t)L->slot;
	for (k = 0; k < 3; k++)
	{
		float dv = v[k] * ENT_LEAD_S;

		lo[k] = cv->origin[k] + cv->mins[k] + (dv < 0 ? dv : 0) - COL_WIN;
		hi[k] = cv->origin[k] + cv->maxs[k] + (dv > 0 ? dv : 0) + COL_WIN;
	}
	if (game_move_goal(e, goal))
	{
		for (k = 0; k < 3; k++)
			d[k] = goal[k] - cv->origin[k];
		len = len3(d);
		for (k = 0; k < 3; k++)
		{
			float end = cv->origin[k] + (len > GOAL_SPAN ? d[k] * GOAL_SPAN / len : d[k]);

			glo[k] = (end < cv->origin[k] ? end : cv->origin[k]) + cv->mins[k] - GOAL_MARGIN;
			ghi[k] = (end > cv->origin[k] ? end : cv->origin[k]) + cv->maxs[k] + GOAL_MARGIN;
		}
		span = 1;
	}
	/* every open layer's cells (a world trace walks each layer whose box
	 * meets the move: a monster by a seam traces the neighbour map) */
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		const wld_layer_t *O = wld_layer(s);
		float a[3], b[3];

		w->r[s][0] = w->g[s][0] = 1;	/* none */
		if (!O)
			continue;
		for (k = 0; k < 3; k++)
		{
			a[k] = lo[k] + L->gofs[k] - O->gofs[k];
			b[k] = hi[k] + L->gofs[k] - O->gofs[k];
		}
		cell_range(O, a, b, w->r[s]);
		if (!span)
			continue;
		for (k = 0; k < 3; k++)
		{
			a[k] = glo[k] + L->gofs[k] - O->gofs[k];
			b[k] = ghi[k] + L->gofs[k] - O->gofs[k];
		}
		cell_range(O, a, b, w->g[s]);
	}
	/* the brush models: queried again when a range changed, and every
	 * EW_REQUERY frames (brush entities hydrate and release) */
	if (prev && prev->e == e && prev->slot == w->slot && !memcmp(prev->r, w->r, sizeof(w->r))
		&& !memcmp(prev->g, w->g, sizeof(w->g)) && (wld_frame_no + (uint32_t)e) % EW_REQUERY)
	{
		w->nm = prev->nm;
		memcpy(w->m, prev->m, sizeof(w->m));
		return 1;
	}
	w->nm = models_of(L, lo, hi, w->m, EW_MODELS);
	if (span && w->nm < EW_MODELS)
	{
		uint32_t gm[EW_MODELS];
		int n = models_of(L, glo, ghi, gm, EW_MODELS), i, j;

		for (i = 0; i < n && w->nm < EW_MODELS; i++)
		{
			for (j = 0; j < w->nm && w->m[j] != gm[i]; j++)
				;
			if (j == w->nm)
				w->m[w->nm++] = gm[i];
		}
	}
	return 1;
}

/* Once per frame, every live entity but the player: the moving ones'
 * collision windows (ent_window); the hulls of the brush entities
 * (triggers, pushers) in the sectors the home eye leaf sees (a door used,
 * a gib shoved through a trigger: their first move reads them before a
 * window could name them); the destinations of the trigger_teleports
 * there (architecture.md 5.3 pin class: reachable teleport
 * destinations); the brush entity list (each_mover). A window or seen
 * hull change is named into the standing generation (S.dwin / S.dseen:
 * name_delta); a teleport destination change bumps pin_gen. */
static void ent_scan(wld_layer_t *H, const float eye[3])
{
	static ewin_t nw[ENT_WINDOWS];
	uint32_t ns[SEEN_MODELS];
	tele_pin_t nt[TELE_PINS];
	const uint16_t *list = NULL;
	int e, n = 0, nsn = 0, ntl = 0, p = 0, cnt = 0, leaf, nd = 0;

	if (H && (leaf = wld_point_leaf(H, eye)) > 0)
		list = wld_leaf_sectors(H, leaf, &cnt);
	memset(nt, 0, sizeof(nt));
	S.nbrush = 0;
	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		const col_ent_t *cv;
		wld_layer_t *L;

		if (!(L = wld_layer(ent_idc(e)->layer)))
			continue;
		cv = ent_cv(e);
		if (RES_KIND(ent_rnd(e)->model) == RES_K_SUBMODEL)
			S.brush[S.nbrush++] = (uint16_t)e;
		if (L == H && list && cv->model != COL_NONE && COL_MODEL_SLOT(cv->model) == L->slot
			&& (cv->solid == COL_SOLID_TRIGGER || cv->movetype == MOVETYPE_PUSH)
			&& box_seen(H, cv->absmin, cv->absmax, list, cnt))
		{
			float org[3];

			if (nsn < SEEN_MODELS)
				ns[nsn++] = (uint32_t)L->slot << 16 | COL_MODEL_SUB(cv->model);
			if (cv->solid == COL_SOLID_TRIGGER && ntl < TELE_PINS && game_teleport_dest(e, org))
			{
				nt[ntl].slot = (int16_t)L->slot;
				memcpy(nt[ntl].org, org, sizeof(org));
				ntl++;
			}
		}
		if (cv->solid == COL_SOLID_TRIGGER || n >= ENT_WINDOWS)
			continue;
		/* S.ew is in entity order: the previous window of e */
		while (p < S.nwin && S.ew[p].e < e)
			p++;
		if (ent_window(e, L, p < S.nwin ? &S.ew[p] : NULL, &nw[n]))
		{
			/* a new window or one that moved: named this frame */
			if (!(p < S.nwin && S.ew[p].e == e && !memcmp(&S.ew[p], &nw[n], sizeof(nw[n]))))
				S.dwin[nd++] = (uint8_t)n;
			n++;
		}
	}
	/* windows gone need no naming: their chunks lapse with the
	 * generation (the next rebuild) */
	if (n != S.nwin || nd)
	{
		memcpy(S.ew, nw, (size_t)n * sizeof(nw[0]));
		S.nwin = n;
	}
	S.ndwin = nd;
	if (nsn != S.nseen || memcmp(ns, S.seen, (size_t)nsn * sizeof(ns[0])))
	{
		memcpy(S.seen, ns, (size_t)nsn * sizeof(ns[0]));
		S.nseen = nsn;
		S.dseen = 1;
	}
	if (ntl != S.ntele || memcmp(nt, S.tele, sizeof(nt)))
	{
		memcpy(S.tele, nt, sizeof(nt));
		S.ntele = ntl;
		S.pin_gen++;
	}
}

/* The collision window (global) shifted by d, in L's frame. */
static void name_window(wld_layer_t *L, const float d[3], int cls)
{
	float lo[3], hi[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = S.clo[k] + d[k] - L->gofs[k];
		hi[k] = S.chi[k] + d[k] - L->gofs[k];
	}
	name_box(L, lo, hi, cls);
}

static void name_model_coll(wld_layer_t *L, unsigned sub, int cls)
{
	const w64_model_t *m;
	unsigned c;

	if (sub >= L->dir->nsub)
		return;
	m = &L->models[sub];
	for (c = 0; c < m->coll_count && m->coll_first + c < L->dir->ncoll; c++)
		name(L, L->dir->coll_first + (int)(m->coll_first + c), cls);
}

/* COLL chunks of brush models (slot << 16 | submodel). */
static void name_models(const uint32_t *m, int n, int cls)
{
	int i;

	for (i = 0; i < n; i++)
	{
		wld_layer_t *L = wld_layer((int)(m[i] >> 16));

		if (L)
			name_model_coll(L, m[i] & 0xFFFF, cls);
	}
}

/* A point pin's eye leaf set and its collision window (map-local org):
 * the grid cells and the brush models live there (a destination's
 * triggers and doors; records not yet hydrated have none). */
static void name_point(wld_layer_t *L, const float org[3], int cls)
{
	float eye[3] = { org[0], org[1], org[2] + EYE_Z }, lo[3], hi[3];
	uint32_t m[COLL_MODELS];
	int k, leaf = wld_point_leaf(L, eye);

	if (leaf > 0)
	{
		name_leaf(L, leaf, cls);
		wld_pvs_want(L, leaf, cls);
	}
	for (k = 0; k < 3; k++)
	{
		float h = k == 2 ? 36.0f : 16.0f;

		lo[k] = org[k] - h - COL_WIN;
		hi[k] = org[k] + h + COL_WIN;
	}
	name_box(L, lo, hi, cls);
	name_models(m, models_of(L, lo, hi, m, COLL_MODELS), cls);
}

/* The leaves the arrival eye can be in: the seam window's player box
 * (B-local) sampled at its centre and its inset corners, at eye height;
 * distinct leaves > 0 into out, their count. */
#define ARRIVAL_PTS	5
static int arrival_leaves(const wld_layer_t *L, const seam_pin_t *p, int out[ARRIVAL_PTS])
{
	static const float sx[ARRIVAL_PTS] = { 0, -1, 1, -1, 1 }, sy[ARRIVAL_PTS] = { 0, -1, -1, 1, 1 };
	int i, j, n = 0;

	for (i = 0; i < ARRIVAL_PTS; i++)
	{
		float e[3];
		int leaf, k;

		for (k = 0; k < 2; k++)
		{
			float c = 0.5f * (p->box[k] + p->box[k + 3]), h = 0.5f * (p->box[k + 3] - p->box[k]) - 16;

			e[k] = c + (h > 0 ? h : 0) * (k ? sy[i] : sx[i]);
		}
		e[2] = 0.5f * (p->box[2] + p->box[5]) + EYE_Z;
		leaf = wld_point_leaf(L, e);
		for (j = 0; j < n && out[j] != leaf; j++)
			;
		if (leaf > 0 && j == n)
			out[n++] = leaf;
	}
	return n;
}

static void name_seam(wld_layer_t *L, seam_pin_t *p)
{
	float lo[3], hi[3];
	int i, k, leaves[ARRIVAL_PTS], nl;

	for (i = 0; i < p->n; i++)
	{
		uint16_t e = p->e[i];

		if (e & 0x8000)
			name_movr(L, e & 0x7FFF, WLD_CLS_PIN);
		else if (e < L->dir->nsect)
		{
			const w64_sector_t *s = &L->sects[e];

			name(L, s->rend_chunk, WLD_CLS_PIN);
			for (k = 0; k < s->page_count; k++)
				name(L, L->dir->texp_first + L->pagepool[s->page_first + k], WLD_CLS_PIN);
		}
	}
	if (p->win)
	{
		for (k = 0; k < 3; k++)
		{
			lo[k] = p->box[k] - COL_WIN;
			hi[k] = p->box[k + 3] + COL_WIN;
		}
		name_box(L, lo, hi, WLD_CLS_PIN);
		/* the arrival eye's own draw set (the cooked slice is the set
		 * seen through the seam, not what the eye sees after the switch:
		 * B's movers there were missing for 22 frames) */
		nl = arrival_leaves(L, p, leaves);
		for (i = 0; i < nl; i++)
		{
			name_leaf(L, leaves[i], WLD_CLS_PIN);
			wld_pvs_want(L, leaves[i], WLD_CLS_PIN);
		}
	}
}

/* ---- the target set ------------------------------------------------------------ */

/* The PVS rows the eye will read (game FindClientInPVS: ent_client_in_pvs)
 * before the next rebuild: the leaves next to the eye (PVS_NEAR around,
 * a stair step up: a strafe, a step, a fall start cross into them before
 * the next rebuild), along eye + v * t, t <= 1 s, every PVS_STEP u, and
 * the ducked eye's leaf. */
static void name_pvs_ahead(wld_layer_t *L)
{
	static const float nx[3] = { -PVS_NEAR, 0, PVS_NEAR }, nz[3] = { -PVS_NEAR, 0, PVS_STEPUP };
	float sp = len3(S.vel), p[3];
	int n = (int)(sp / PVS_STEP), i, k, leaf, last = S.leaf;

	for (i = 0; i < 27; i++)
	{
		p[0] = S.eye[0] + nx[i % 3];
		p[1] = S.eye[1] + nx[i / 3 % 3];
		p[2] = S.eye[2] + nz[i / 9];
		leaf = wld_point_leaf(L, p);
		if (leaf > 0 && leaf != S.leaf)
			wld_pvs_want(L, leaf, WLD_CLS_OPEN);	/* the next frames' */
	}
	if (n > PVS_SAMPLES)
		n = PVS_SAMPLES;
	for (i = 1; i <= n; i++)
	{
		for (k = 0; k < 3; k++)
			p[k] = S.eye[k] + S.vel[k] * (float)i / (float)n;
		leaf = wld_point_leaf(L, p);
		if (leaf > 0 && leaf != last)
			wld_pvs_want(L, leaf, WLD_CLS_PREFETCH);
		last = leaf;
	}
	p[0] = S.eye[0];
	p[1] = S.eye[1];
	p[2] = S.eye[2] - DUCK_DZ;
	wld_pvs_want(L, wld_point_leaf(L, p), WLD_CLS_PREFETCH);
}

static void retry_clear(void);

/* Window w's cells in layer L (slot s) and, in its own layer, its brush
 * models. */
static void name_ewin(wld_layer_t *L, int s, const ewin_t *w, int cls)
{
	name_cells(L, w->r[s], cls);
	name_cells(L, w->g[s], cls);
	if (w->slot == s)
		name_models(w->m, w->nm, cls);
}

static void name_spawn(wld_layer_t *L, const spawn_want_t *w, int cls)
{
	name_cells(L, w->r, cls);
	if (w->sub >= 0)
		name_model_coll(L, (unsigned)w->sub, cls);
}

/* Name the whole set, strongest class first (a stronger request may take
 * a weaker queued read's slot, never the reverse). The want peak is
 * measured after the open set and the seam pins: the test route's point
 * pins (look-ahead SETPOS) are not a play need. */
static void name_all(void)
{
	static const float none[3] = { 0, 0, 0 }, ts[3] = { 0.25f, 0.5f, 1.0f };
	float d[3];
	int s, i, k, cls;

	retry_clear();
	S.named_frame = wld_frame_no;
	for (cls = WLD_CLS_OPEN; cls >= WLD_CLS_PREFETCH; cls--)
	{
		if (cls == WLD_CLS_PREFETCH)
		{
			/* once per generation: a whole naming again in the
			 * standing generation (retry_over / pref_short) would
			 * count the point pins the last pass named below */
			if (S.measured_gen != wld_tgen)
			{
				wld_want_measure();
				S.measured_gen = wld_tgen;
			}
			for (s = 0; s < HL64_WORLD_LAYERS; s++)
			{
				for (i = 0; wld_layer(s) && i < POINT_PINS; i++)
					if (S.point[i].live && S.point[i].map == wld_layer(s)->map)
						name_point(wld_layer(s), S.point[i].org, WLD_CLS_PIN);
				/* a map's open gate waits on its spawn windows:
				 * they may evict prefetch (wld_stream_spawn_gate) */
				for (i = 0; wld_layer(s) && S.spawn_gate[s] && i < S.nspawn[s]; i++)
					name_spawn(wld_layer(s), &S.spawn[s][i], WLD_CLS_PIN);
			}
		}
		for (s = 0; s < HL64_WORLD_LAYERS; s++)
		{
			wld_layer_t *L = wld_layer(s);

			if (!L)
				continue;
			if (cls == WLD_CLS_OPEN)
			{
				/* leaf 0: the eye is in solid / outside, no set */
				if (s == S.home_slot && S.leaf > 0)
				{
					L->want_leaf = S.leaf;
					name_leaf(L, S.leaf, cls);
					wld_pvs_want(L, S.leaf, cls);
					for (i = 0; i < S.nvleaf; i++)
						name_leaf(L, S.vleaf[i], cls);
				}
				if (S.have_coll)
					name_window(L, none, cls);
				for (i = 0; i < S.ncmodel; i++)
					if ((int)(S.cmodel[i] >> 16) == s)
						name_model_coll(L, S.cmodel[i] & 0xFFFF, cls);
			}
			else if (cls == WLD_CLS_PIN)
			{
				if (S.seam[s].on)
					name_seam(L, &S.seam[s]);
				/* reachable teleport destinations: a play need,
				 * measured with the open set */
				for (i = 0; i < S.ntele; i++)
					if (S.tele[i].slot == s)
						name_point(L, S.tele[i].org, cls);
			}
			else
			{
				/* moving entities' windows and the hydration
				 * candidates' spawn windows: prefetch (an OPEN set
				 * this wide overflowed the collision pool in the
				 * warehouse fight; the sync fallback evicts them) */
				for (i = 0; i < S.nwin; i++)
					name_ewin(L, s, &S.ew[i], cls);
				for (i = 0; i < S.nspawn[s]; i++)
					name_spawn(L, &S.spawn[s][i], cls);
				if (s != S.home_slot)
					continue;
				name_models(S.seen, S.nseen, cls);
				for (i = 0; i < 3; i++)
				{
					float p[3];
					int leaf;

					for (k = 0; k < 3; k++)
						p[k] = S.eye[k] + S.vel[k] * ts[i];
					leaf = wld_point_leaf(L, p);
					if (leaf > 0 && leaf != S.leaf)
						name_leaf(L, leaf, cls);
				}
				name_pvs_ahead(L);
				if (S.have_coll && (S.vel[0] != 0 || S.vel[1] != 0 || S.vel[2] != 0))
				{
					for (k = 0; k < 3; k++)
						d[k] = S.vel[k] * PREF_COLL_S;
					name_window(L, d, cls);
				}
			}
		}
	}
}

/* The prefetch parts that changed while the other inputs stood: the
 * windows that moved, new spawn windows, the seen hulls, named into the
 * standing generation as name_all would (a monster crossing grid cells
 * no longer renames the whole set; names it no longer needs lapse with
 * the generation). */
static void name_delta(void)
{
	int s, i;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L = wld_layer(s);

		if (!L)
			continue;
		for (i = 0; i < S.ndwin; i++)
			name_ewin(L, s, &S.ew[S.dwin[i]], WLD_CLS_PREFETCH);
		for (i = 0; i < S.ndspawn; i++)
			if (S.dspawn_slot[i] == s)
				name_spawn(L, &S.dspawn[i], S.spawn_gate[s] ? WLD_CLS_PIN : WLD_CLS_PREFETCH);
		if (S.dseen && s == S.home_slot)
			name_models(S.seen, S.nseen, WLD_CLS_PREFETCH);
	}
}

/* Empty the retry list (a whole naming lists its failures again). */
static void retry_clear(void)
{
	int i;

	for (i = 0; i < S.nretry; i++)
	{
		wld_layer_t *L = wld_layer((int)(S.retry[i] >> 16 & 0xFF));
		unsigned n = S.retry[i] & 0xFFFF;

		if (!L)
			continue;
		if (S.retry[i] & RETRY_MOVR)
		{
			if (n < L->dir->nsub)
				L->ch[L->subs[n].chunk].retry = 0;
		}
		else if (n < L->dir->nchunks)
			L->ch[n].retry = 0;
	}
	S.nretry = 0;
	S.retry_over = 0;
	S.pref_short = 0;
}

/* Name the listed chunks again (a slot or room may have come free) and
 * the pages of listed MOVRs that landed; failures list themselves
 * again. Entries of a generation since replaced are dropped. */
static void retry_names(void)
{
	uint32_t e[RETRY_MAX];
	int n = S.nretry, i;

	memcpy(e, S.retry, (size_t)n * sizeof(e[0]));
	S.nretry = 0;
	for (i = 0; i < n; i++)
	{
		wld_layer_t *L = wld_layer((int)(e[i] >> 16 & 0xFF));
		unsigned x = e[i] & 0xFFFF;
		int c;

		if (!L)
			continue;
		if (e[i] & RETRY_MOVR)
		{
			if (x >= L->dir->nsub)
				continue;
			c = L->subs[x].chunk;
			L->ch[c].retry &= (uint8_t)~RETRY_PAGES;
			if (L->ch[c].want == wld_tgen)
				name_movr(L, (int)x, L->ch[c].cls);
		}
		else if (x < L->dir->nchunks)
		{
			L->ch[x].retry &= (uint8_t)~RETRY_CHUNK;
			if (L->ch[x].want == wld_tgen)
				name(L, (int)x, L->ch[x].cls);
		}
	}
}

static int32_t eye_cell(const wld_layer_t *L, const float p[3])
{
	const w64_grid_t *g = L->grid;
	int32_t ix[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		float f = (p[k] - g->origin[k]) / (float)g->cell;

		if (!(f >= 0) || f >= g->dims[k])
			return -1;
		ix[k] = (int32_t)f;
	}
	return ix[0] + g->dims[0] * (ix[1] + g->dims[1] * ix[2]);
}

static int32_t vbucket(float v)
{
	float b = floorf(v / VEL_BUCKET);

	return b > 1e6f ? 1000000 : b < -1e6f ? -1000000 : (int32_t)b;
}

static float len3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* A player global jump not explained by its velocity nor by a seam
 * re-base opens the teleport window (the first frame too: New Game).
 * Otherwise a frame's motion clearly faster than the velocity (riding a
 * train / lift / pushed: the push moves the origin, not the velocity) is
 * the prefetch velocity. */
static void teleport_check(int home_map, double now)
{
	const float *gm = wld_map_origin(home_map);
	float g[3], d[3], speed = len3(S.vel);
	int k, jump = !S.have_prev;

	for (k = 0; k < 3; k++)
	{
		g[k] = S.eye[k] + gm[k];
		d[k] = g[k] - S.prev[k] - S.rebase[k];
	}
	if (S.have_prev && len3(d) > (speed + S.prev_speed) * TELE_DT + TELE_SLACK)
		jump = 1;
	if (jump)
	{
		S.tele_until = now + TELE_WINDOW_S;
		S.st.teleports++;
	}
	else if (now > S.prev_t && len3(d) > (speed + RIDE_SLACK) * (float)(now - S.prev_t))
		for (k = 0; k < 3; k++)
			S.vel[k] = d[k] / (float)(now - S.prev_t);
	memcpy(S.prev, g, sizeof(g));
	S.prev_t = now;
	S.prev_speed = speed;
	memset(S.rebase, 0, sizeof(S.rebase));
	S.have_prev = 1;
}

/* CPU since *t into sub-step z (wld_stream_stats sub_us) */
static void sub_mark(uint64_t *t, int z)
{
	uint64_t n = sys_time_us();

	S.st.sub_us[z] += (uint32_t)(n - *t);
	*t = n;
}

void wld_stream_frame(int home_map, const float eye[3], const float vel[3])
{
	uint64_t t0 = sys_time_us(), tm = t0;
	double now = eng_time();
	wld_layer_t *H;
	int32_t vb[3];
	int s, k, seam_out = 0, changed;
	uint32_t us;

	/* async opens: one stage (and <= 16 KB of directory CRC) per frame */
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L = wld_slot(s);

		if (L->open && !L->ready)
			wld_open_step(L, 16 * 1024);
	}
	/* landed PVS offset pairs go on to their rows */
	wld_pvs_step();
	sub_mark(&tm, WLD_SS_OPEN);
	/* the drawn eye's offset from the player's eye (bob, punch, landing
	 * offset): last frame's view minus last frame's eye */
	if (home_map >= 0 && ren_cam.map == home_map && S.have_prev)
	{
		const float *gm = wld_map_origin(home_map);

		for (k = 0; k < 3; k++)
			S.voff[k] = ren_cam.org[k] - gm[k] - S.eye[k];
	}
	else
		memset(S.voff, 0, sizeof(S.voff));
	memcpy(S.eye, eye, sizeof(S.eye));
	memcpy(S.vel, vel, sizeof(S.vel));
	/* point pins lapse after 3 s or on arrival */
	for (k = 0; k < POINT_PINS; k++)
	{
		point_pin_t *p = &S.point[k];
		float d[3] = { eye[0] - p->org[0], eye[1] - p->org[1], eye[2] - EYE_Z - p->org[2] };

		if (p->live && (now - p->t0 > PIN_LAPSE_S || (p->map == home_map && len3(d) < PIN_ARRIVE)))
		{
			p->live = 0;
			S.pin_gen++;
		}
	}
	if (home_map >= 0 && home_map < wld_map_count())
		teleport_check(home_map, now);

	/* inputs */
	H = home_map >= 0 ? wld_layer_of_map(home_map) : NULL;
	if (H && H->ready != 1)
		H = NULL;
	for (k = 0; k < 3; k++)
		vb[k] = vbucket(S.vel[k]);
	/* moving entities' windows (a rebuild when one's cells changed) and
	 * reachable teleport destinations */
	ent_scan(H, eye);
	{
		int hs = H ? H->slot : -1;
		int32_t leaf = H ? wld_point_leaf(H, eye) : -1, cell = H ? eye_cell(H, eye) : -1;
		int32_t vl[VIEW_PTS];
		int nv = 0, i, j;

		/* the drawn eye (bob, punch, shake: last frame's offset, and
		 * VIEW_SLACK around the eye) can sit in a neighbouring leaf */
		for (i = 0; H && i < VIEW_PTS; i++)
		{
			float v[3];
			int32_t l;

			for (k = 0; k < 3; k++)
				v[k] = eye[k] + (i == 0 ? (len3(S.voff) < TELE_SLACK ? S.voff[k] : 0)
					: (i - 1) / 2 == k ? ((i & 1) ? VIEW_SLACK : -VIEW_SLACK) : 0);
			l = wld_point_leaf(H, v);
			for (j = 0; j < nv && vl[j] != l; j++)
				;
			if (l > 0 && l != leaf && j == nv)
				vl[nv++] = l;
		}
		changed = !S.valid || hs != S.home_slot || leaf != S.leaf || cell != S.cell
			|| nv != S.nvleaf || memcmp(vl, S.vleaf, (size_t)nv * sizeof(vl[0]))
			|| memcmp(vb, S.vb, sizeof(vb)) || S.pin_gen != S.pin_gen_seen
			|| wld_layer_gen != S.layer_gen_seen || S.coll_gen != S.coll_gen_seen;
		S.home_slot = hs;
		S.leaf = leaf;
		memcpy(S.vleaf, vl, (size_t)nv * sizeof(vl[0]));
		S.nvleaf = nv;
		S.cell = cell;
		if (dbg_trace())
			log_printf("trace %lu stream ch=%d hs=%d leaf=%ld cell=%ld nv=%d vl=%ld,%ld,%ld "
				"vb=%ld,%ld,%ld pin=%lu/%lu lgen=%lu/%lu cgen=%lu/%lu voff=%.9g,%.9g,%.9g "
				"eye=%.9g,%.9g,%.9g retry=%d\n", (unsigned long)dbg_trace_frame(), changed,
				hs, (long)leaf, (long)cell, nv, (long)(nv > 0 ? vl[0] : 0),
				(long)(nv > 1 ? vl[1] : 0), (long)(nv > 2 ? vl[2] : 0), (long)vb[0],
				(long)vb[1], (long)vb[2], (unsigned long)S.pin_gen,
				(unsigned long)S.pin_gen_seen, (unsigned long)wld_layer_gen,
				(unsigned long)S.layer_gen_seen, (unsigned long)S.coll_gen,
				(unsigned long)S.coll_gen_seen, (double)S.voff[0], (double)S.voff[1],
				(double)S.voff[2], (double)eye[0], (double)eye[1], (double)eye[2],
				S.nretry);
	}
	sub_mark(&tm, WLD_SS_SCAN);
	if (changed)
	{
		memcpy(S.vb, vb, sizeof(vb));
		S.pin_gen_seen = S.pin_gen;
		S.layer_gen_seen = wld_layer_gen;
		S.coll_gen_seen = S.coll_gen;
		S.valid = 1;
		if (!++wld_tgen)
			wld_tgen = 1;
		name_all();
		S.st.recompute++;
	}
	else
	{
		/* the standing generation: the windows that moved are named
		 * into it; the namings that could not be queued are retried
		 * in it (the listed ones each frame; the whole set when the
		 * list filled, or PREF_RETRY frames after a prefetch naming
		 * fell short) */
		if (S.retry_over || (S.pref_short && wld_frame_no - S.named_frame >= PREF_RETRY))
			name_all();
		else
		{
			if (S.ndwin || S.ndspawn || S.dseen)
			{
				name_delta();
				S.st.delta++;
			}
			if (S.nretry)
				retry_names();
		}
		S.st.reassert++;
	}
	S.ndwin = S.ndspawn = 0;
	S.dseen = 0;
	sub_mark(&tm, WLD_SS_NAME);
	/* brush entities move without changing an input (doors, trains, the
	 * elevator): the ones now in the eye's sectors join the standing set */
	if (H && S.leaf > 0)
	{
		const uint16_t *list;
		int cnt;

		list = wld_leaf_sectors(H, S.leaf, &cnt);
		each_mover(H, list, cnt, WLD_CLS_OPEN, name_movr);
	}
	sub_mark(&tm, WLD_SS_MOVERS);
	/* the 128 KB seam budget while a seam pin is still missing chunks */
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (S.seam[s].on && wld_layer(s) && !wld_pin_resident(wld_layer(s)))
			seam_out = 1;
	io_set_seam(seam_out);
	sub_mark(&tm, WLD_SS_PINS);

	us = (uint32_t)(sys_time_us() - t0);
	S.st.frames++;
	S.st.us_sum += us;
	if (us > S.st.us_max)
		S.st.us_max = us;
}

void wld_stream_rebase(const float T[3])
{
	int k;

	for (k = 0; k < 3; k++)
		S.rebase[k] += T[k];
}

void wld_stream_coll(const float lo[3], const float hi[3], const uint32_t *models, int n)
{
	int s, changed = !S.have_coll;

	if (n > COLL_MODELS)
		n = COLL_MODELS;
	memcpy(S.clo, lo, sizeof(S.clo));
	memcpy(S.chi, hi, sizeof(S.chi));
	S.have_coll = 1;
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L = wld_layer(s);
		float a[3], b[3];
		int32_t r[6] = { 0, 0, 0, -1, -1, -1 };
		int k;

		if (L)
		{
			for (k = 0; k < 3; k++)
			{
				a[k] = lo[k] - L->gofs[k];
				b[k] = hi[k] - L->gofs[k];
			}
			cell_range(L, a, b, r);
		}
		if (memcmp(r, S.ccell[s], sizeof(r)))
		{
			memcpy(S.ccell[s], r, sizeof(r));
			changed = 1;
		}
	}
	if (n != S.ncmodel || memcmp(models, S.cmodel, (size_t)n * sizeof(*models)))
	{
		memcpy(S.cmodel, models, (size_t)n * sizeof(*models));
		S.ncmodel = n;
		changed = 1;
	}
	if (changed)
		S.coll_gen++;
}

void wld_stream_missing(uint32_t n)
{
	if (!n)
		return;
	if (eng_time() < S.tele_until)
		S.st.popin_tele++;
	else
	{
		S.st.popin++;
		if (S.popin_logs < POPIN_LOGS)
		{
			S.popin_logs++;
			log_printf("stream: popin t=%.3f missing=%lu leaf=%ld\n", eng_time(),
				(unsigned long)n, (long)S.leaf);
		}
	}
}

/* ---- pins ------------------------------------------------------------------------ */

void wld_pin_seam(wld_layer_t *B, const uint16_t *pool, int n)
{
	seam_pin_t *p = &S.seam[B->slot];

	if (n > WLD_PIN_POOL)
	{
		log_printf("stream: seam pin of %s: %d entries, %d kept\n", wld_map_name(B->map), n,
			WLD_PIN_POOL);
		n = WLD_PIN_POOL;
	}
	if (n < 0)
		n = 0;
	memcpy(p->e, pool, (size_t)n * sizeof(*pool));
	p->n = (uint16_t)n;
	p->on = 1;
	p->win = 0;
	S.pin_gen++;
	S.st.pins++;
}

void wld_pin_seam_window(wld_layer_t *B, const float mins[3], const float maxs[3])
{
	seam_pin_t *p = &S.seam[B->slot];

	memcpy(p->box, mins, 3 * sizeof(float));
	memcpy(p->box + 3, maxs, 3 * sizeof(float));
	p->win = 1;
	p->on = 1;
	S.pin_gen++;
}

void wld_unpin_seam(wld_layer_t *B)
{
	seam_pin_t *p = &S.seam[B->slot];

	if (!p->on)
		return;
	memset(p, 0, sizeof(*p));
	S.pin_gen++;
}

void wld_stream_new_game(void)
{
	wld_stream_stats_t st = S.st;

	/* boot state (pins, teleport window, the previous player position,
	 * the standing set) with the stats kept; the target set is named
	 * again from the next frame's inputs */
	memset(&S, 0, sizeof(S));
	S.st = st;
}

void wld_stream_closed(int slot)
{
	if (S.seam[slot].on)
		S.pin_gen++;
	memset(&S.seam[slot], 0, sizeof(S.seam[slot]));
	memset(S.ccell[slot], 0, sizeof(S.ccell[slot]));
	/* the slot's cell ranges index its grid: none outlives the map; the
	 * changes not named yet go with a rebuild */
	S.nspawn[slot] = 0;
	S.spawn_gate[slot] = 0;
	S.ndwin = S.ndspawn = 0;
	S.dseen = 0;
	S.coll_gen++;
	{
		int i, n = 0;

		for (i = 0; i < S.nwin; i++)
			if (S.ew[i].slot != slot)
			{
				S.ew[n] = S.ew[i];
				S.ew[n].r[slot][0] = S.ew[n].g[slot][0] = 1;
				S.ew[n].r[slot][3] = S.ew[n].g[slot][3] = 0;
				n++;
			}
		S.nwin = n;
		for (i = n = 0; i < S.ntele; i++)
			if (S.tele[i].slot != slot)
				S.tele[n++] = S.tele[i];
		if (n != S.ntele)
			S.pin_gen++;
		memset(S.tele + n, 0, sizeof(S.tele[0]) * (size_t)(TELE_PINS - n));
		S.ntele = n;
		for (i = n = 0; i < S.nseen; i++)
			if ((int)(S.seen[i] >> 16) != slot)
				S.seen[n++] = S.seen[i];
		S.nseen = n;
	}
}

static int resident(const wld_layer_t *L, int c)
{
	return L->ch[c].state >= WLD_CH_RESIDENT;
}

/* BSP model n's MOVR and every page its meshlets can show (animation /
 * alternate frames) resident (not a MOVR: 1). */
static int movr_resident(const wld_layer_t *L, unsigned n)
{
	const w64_sub_t *sb;
	const w64_meshlet_t *m;
	int k, nm;

	if (n >= L->dir->nsub || L->subs[n].kind != W64_SUB_MOVR)
		return 1;
	sb = &L->subs[n];
	if (!resident(L, sb->chunk))
		return 0;
	m = (const w64_meshlet_t *)((const uint8_t *)L->ch[sb->chunk].data + 24);
	nm = ((const w64_rend_t *)L->ch[sb->chunk].data)->nmeshlets;
	/* tex_pages only reads L when checking */
	for (k = 0; k < nm; k++)
		if (!tex_pages((wld_layer_t *)(uintptr_t)L, m[k].tex, 0, 1))
			return 0;
	return 1;
}

static int sector_resident(const wld_layer_t *L, int e)
{
	const w64_sector_t *s = &L->sects[e];
	int k;

	if (!resident(L, s->rend_chunk))
		return 0;
	for (k = 0; k < s->page_count; k++)
		if (!resident(L, L->dir->texp_first + L->pagepool[s->page_first + k]))
			return 0;
	return 1;
}

/* name_leaf's MOVR models resident (each_mover's fn). */
static int movr_ok(wld_layer_t *L, int sub, int cls)
{
	(void)cls;
	return movr_resident(L, (unsigned)sub);
}

static int leaf_resident(const wld_layer_t *L, int leaf)
{
	const uint16_t *list;
	int i, cnt;

	list = wld_leaf_sectors(L, leaf, &cnt);
	for (i = 0; i < cnt; i++)
		if (!sector_resident(L, list[i]))
			return 0;
	/* each_mover only reads L here (movr_ok) */
	return each_mover((wld_layer_t *)(uintptr_t)L, list, cnt, 0, movr_ok);
}

/* Every COLL chunk the grid cells in range r list resident. */
static int cells_resident(const wld_layer_t *L, const int32_t r[6])
{
	const w64_grid_t *g = L->grid;
	int x, y, z, i;

	for (z = r[2]; z <= r[5]; z++)
		for (y = r[1]; y <= r[4]; y++)
			for (x = r[0]; x <= r[3]; x++)
			{
				const uint16_t *v = L->glists + L->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

				for (i = 1; i <= v[0]; i++)
					if (!resident(L, L->dir->coll_first + v[i]))
						return 0;
			}
	return 1;
}

int wld_pin_resident(const wld_layer_t *B)
{
	const seam_pin_t *p = &S.seam[B->slot];
	int i, k, leaves[ARRIVAL_PTS], nl;

	if (!p->on)
		return 1;
	if (B->ready != 1)
		return 0;
	for (i = 0; i < p->n; i++)
	{
		uint16_t e = p->e[i];

		if (e & 0x8000)
		{
			if (!movr_resident(B, e & 0x7FFF))
				return 0;
		}
		else if (e < B->dir->nsect && !sector_resident(B, e))
			return 0;
	}
	if (p->win)
	{
		float lo[3], hi[3];
		int32_t r[6];

		for (k = 0; k < 3; k++)
		{
			lo[k] = p->box[k] - COL_WIN;
			hi[k] = p->box[k + 3] + COL_WIN;
		}
		cell_range(B, lo, hi, r);
		if (!cells_resident(B, r))
			return 0;
		nl = arrival_leaves(B, p, leaves);
		for (i = 0; i < nl; i++)
			if (!leaf_resident(B, leaves[i]))
				return 0;
	}
	return 1;
}

/* ---- hydration windows ------------------------------------------------------------- */

/* A record's spawn window: its spawn box grown by SPAWN_MARGIN as grid
 * cells of L, and a brush record's own submodel (-1 none). */
static void spawn_cells(const wld_layer_t *L, const float box[6], int sub, spawn_want_t *w)
{
	float lo[3], hi[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = box[k] - SPAWN_MARGIN;
		hi[k] = box[k + 3] + SPAWN_MARGIN;
	}
	cell_range(L, lo, hi, w->r);
	w->sub = sub >= 0 && (unsigned)sub < L->dir->nsub ? sub : -1;
}

void wld_stream_spawns(int slot, const float (*box)[6], const int *sub, int n)
{
	const wld_layer_t *L = wld_layer(slot);
	spawn_want_t w[SPAWN_WANTS];
	int i, j, m = 0;

	for (i = 0; L && i < n && m < SPAWN_WANTS; i++)
	{
		spawn_cells(L, box[i], sub[i], &w[m]);
		for (j = 0; j < m && memcmp(&w[j], &w[m], sizeof(w[m])); j++)
			;
		if (j == m)
			m++;
	}
	if (m != S.nspawn[slot] || memcmp(w, S.spawn[slot], (size_t)m * sizeof(w[0])))
	{
		/* the new ones are named this frame (name_delta); too many:
		 * a rebuild */
		for (i = 0; i < m; i++)
		{
			for (j = 0; j < S.nspawn[slot] && memcmp(&S.spawn[slot][j], &w[i], sizeof(w[i])); j++)
				;
			if (j < S.nspawn[slot])
				continue;
			if (S.ndspawn == DSPAWN_MAX)
			{
				S.coll_gen++;
				break;
			}
			S.dspawn[S.ndspawn] = w[i];
			S.dspawn_slot[S.ndspawn++] = (int8_t)slot;
		}
		memcpy(S.spawn[slot], w, (size_t)m * sizeof(w[0]));
		S.nspawn[slot] = m;
	}
}

void wld_stream_spawn_gate(int slot, int on)
{
	if (slot < 0 || slot >= HL64_WORLD_LAYERS || S.spawn_gate[slot] == (on != 0))
		return;
	S.spawn_gate[slot] = (uint8_t)(on != 0);
	S.coll_gen++;	/* a rebuild names its windows at the new class */
}

int wld_spawn_resident(int slot, const float box[6], int sub)
{
	const wld_layer_t *L = wld_layer(slot);
	const w64_model_t *md;
	spawn_want_t w;
	unsigned c;

	if (!L)
		return 1;
	spawn_cells(L, box, sub, &w);
	if (w.sub < 0)
		return cells_resident(L, w.r);
	/* a brush record: its own submodel only. A brush entity's Spawn
	 * traces nothing (HLSDK InitTrigger, CBaseDoor::Spawn,
	 * CBreakable::Spawn: SET_MODEL + UTIL_SetOrigin); the world cells
	 * around it stay a prefetch (a trigger_hurt spanning 40 cells of a
	 * map opened beside a full collision pool never landed them) */
	md = &L->models[w.sub];
	for (c = 0; c < md->coll_count && md->coll_first + c < L->dir->ncoll; c++)
		if (!resident(L, L->dir->coll_first + (int)(md->coll_first + c)))
			return 0;
	return 1;
}

void wld_pin_point(int map, const float org[3])
{
	point_pin_t *p = NULL;
	int i;

	for (i = 0; i < POINT_PINS; i++)
		if (S.point[i].live && S.point[i].map == map && fabsf(S.point[i].org[0] - org[0]) < 1
			&& fabsf(S.point[i].org[1] - org[1]) < 1 && fabsf(S.point[i].org[2] - org[2]) < 1)
		{
			S.point[i].t0 = eng_time();	/* re-armed: the set is unchanged */
			return;
		}
	for (i = 0; i < POINT_PINS; i++)
		if (!S.point[i].live && !p)
			p = &S.point[i];
	if (!p)
		for (p = &S.point[0], i = 1; i < POINT_PINS; i++)
			if (S.point[i].t0 < p->t0)
				p = &S.point[i];
	p->live = 1;
	p->map = (int16_t)map;
	memcpy(p->org, org, sizeof(p->org));
	p->t0 = eng_time();
	S.pin_gen++;
	S.st.pins++;
}

/* ---- stats -------------------------------------------------------------------------- */

void wld_stream_stats(wld_stream_stats_t *s, int reset)
{
	*s = S.st;
	s->prefetch = wld_n_prefetch_req;
	s->promoted = wld_n_promoted;
	s->evict_queued = wld_n_evict_queued;
	s->open_async = wld_n_open_async;
	s->open_fail = wld_n_open_fail;
	s->room_us = wld_us_room;
	if (reset)
	{
		S.st.frames = 0;
		S.st.us_sum = 0;
		S.st.us_max = 0;
	}
}
