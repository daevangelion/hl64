/* SPDX-License-Identifier: MIT */
/*
 * stream.c -- residency classes and the change-driven target set
 * (docs/architecture.md 5.2-5.4): open (the eye leaf's visible set, the
 * collision window and the brush models in it), pin (a seam's B set and
 * window, point pins of teleport / route destinations), prefetch (eye-leaf
 * sets at eye + v * {0.25, 0.5, 1} s, the collision window + v * 0.5 s);
 * teleport windows and pop-in accounting; the async layer opens' stepping.
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
 * layer set or the collision window's cells / brush models. Otherwise the
 * previous generation stands (`reassert`): the LRU never evicts a chunk of
 * it for a weaker class, and a naming that could not be queued (no request
 * slot / no room) is retried by re-naming the same generation.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/io/io.h"
#include "engine/world/wld_int.h"

#define PREF_COLL_S	0.5f		/* collision window + v * 0.5 s */
#define PIN_LAPSE_S	3.0		/* pins lapse after 3 s ... */
#define PIN_ARRIVE	64.0f		/* ... or when the player arrives */
#define TELE_WINDOW_S	1.0		/* pop-in allowed for 1 s after a teleport */
#define TELE_SLACK	32.0f		/* u a frame's move may exceed v * dt by */
#define TELE_DT		0.1f		/* longest frame (HL64_DT_MAX_US) */
#define VEL_BUCKET	64.0f		/* u/s per velocity bucket step */
#define COL_WIN		128.0f		/* collision window margin (engine/coll COL_WINDOW) */
#define POINT_PINS	4
#define COLL_MODELS	64
#define EYE_Z		28.0f		/* view offset of a point pin's eye */
#define POPIN_LOGS	16		/* `stream: popin` lines logged at most */

typedef struct {
	uint8_t		on, win, movr_wait;
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

static struct {
	/* inputs of the standing target set */
	int		home_slot;
	int32_t		leaf, cell;
	int32_t		vb[3];
	uint32_t	pin_gen_seen, layer_gen_seen, coll_gen_seen;
	uint8_t		valid, pending;
	/* this frame's inputs */
	float		eye[3], vel[3];
	/* collision window (wld_stream_coll) */
	uint8_t		have_coll;
	float		clo[3], chi[3];
	int32_t		ccell[HL64_WORLD_LAYERS][6];
	uint32_t	cmodel[COLL_MODELS];
	int		ncmodel;
	uint32_t	coll_gen;
	/* pins */
	seam_pin_t	seam[HL64_WORLD_LAYERS];
	point_pin_t	point[POINT_PINS];
	uint32_t	pin_gen;
	/* teleports */
	uint8_t		have_prev;
	float		prev[3], prev_speed, rebase[3];
	double		tele_until;
	uint32_t	popin_logs;
	wld_stream_stats_t st;
} S;

/* ---- naming helpers --------------------------------------------------------- */

static void name(wld_layer_t *L, int c, int cls)
{
	if (!wld_name(L, c, cls))
		S.pending = 1;
}

/* REND + TEXP pages of the sectors leaf sees. */
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
}

/* A texture's page and every page of its animation / alternate sequences. */
static void name_tex(wld_layer_t *L, int t, int cls)
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
				name(L, L->dir->texp_first + x->page, cls);
			if (!(x->flags & W64_TEXF_ANIM) || x->anim_next == (pass ? L->texs[t].alternate : t))
				break;
			u = x->anim_next;
		}
	}
}

/* BSP model n's MOVR chunk, and its pages once it is resident; 0 while it
 * is not (its pages follow when it lands). */
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
		return 0;
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

/* COLL lists of the grid cells a map-local box meets. */
static void name_box(wld_layer_t *L, const float lo[3], const float hi[3], int cls)
{
	const w64_grid_t *g = L->grid;
	int32_t r[6];
	int x, y, z, i;

	cell_range(L, lo, hi, r);
	for (z = r[2]; z <= r[5]; z++)
		for (y = r[1]; y <= r[4]; y++)
			for (x = r[0]; x <= r[3]; x++)
			{
				const uint16_t *v = L->glists + L->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

				for (i = 1; i <= v[0]; i++)
					name(L, L->dir->coll_first + v[i], cls);
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

/* A point pin's eye leaf set and its collision window (map-local org). */
static void name_point(wld_layer_t *L, const float org[3], int cls)
{
	float eye[3] = { org[0], org[1], org[2] + EYE_Z }, lo[3], hi[3];
	int k, leaf = wld_point_leaf(L, eye);

	if (leaf > 0)
		name_leaf(L, leaf, cls);
	for (k = 0; k < 3; k++)
	{
		float h = k == 2 ? 36.0f : 16.0f;

		lo[k] = org[k] - h - COL_WIN;
		hi[k] = org[k] + h + COL_WIN;
	}
	name_box(L, lo, hi, cls);
}

static void name_seam(wld_layer_t *L, seam_pin_t *p)
{
	float lo[3], hi[3];
	int i, k;

	p->movr_wait = 0;
	for (i = 0; i < p->n; i++)
	{
		uint16_t e = p->e[i];

		if (e & 0x8000)
		{
			if (!name_movr(L, e & 0x7FFF, WLD_CLS_PIN))
				p->movr_wait = 1;
		}
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
	}
}

/* ---- the target set ------------------------------------------------------------ */

/* Name the whole set, strongest class first (a stronger request may take
 * a weaker queued read's slot, never the reverse). */
static void name_all(void)
{
	static const float none[3] = { 0, 0, 0 }, ts[3] = { 0.25f, 0.5f, 1.0f };
	float d[3];
	int s, i, k, cls;

	S.pending = 0;
	for (cls = WLD_CLS_OPEN; cls >= WLD_CLS_PREFETCH; cls--)
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
				for (i = 0; i < POINT_PINS; i++)
					if (S.point[i].live && S.point[i].map == L->map)
						name_point(L, S.point[i].org, cls);
			}
			else if (s == S.home_slot)
			{
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
				if (S.have_coll && (S.vel[0] != 0 || S.vel[1] != 0 || S.vel[2] != 0))
				{
					for (k = 0; k < 3; k++)
						d[k] = S.vel[k] * PREF_COLL_S;
					name_window(L, d, cls);
				}
			}
		}
}

/* A pinned MOVR landed since the set was named: its pages join. */
static int movr_landed(void)
{
	int s, i;

	for (s = 0; s < HL64_WORLD_LAYERS; s++)
	{
		wld_layer_t *L = wld_layer(s);
		seam_pin_t *p = &S.seam[s];

		if (!L || !p->on || !p->movr_wait)
			continue;
		for (i = 0; i < p->n; i++)
		{
			unsigned n = p->e[i] & 0x7FFF;

			if ((p->e[i] & 0x8000) && n < L->dir->nsub && L->subs[n].kind == W64_SUB_MOVR
				&& L->ch[L->subs[n].chunk].state >= WLD_CH_RESIDENT)
				return 1;
		}
	}
	return 0;
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
 * re-base opens the teleport window (the first frame too: New Game). */
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
	memcpy(S.prev, g, sizeof(g));
	S.prev_speed = speed;
	memset(S.rebase, 0, sizeof(S.rebase));
	S.have_prev = 1;
}

void wld_stream_frame(int home_map, const float eye[3], const float vel[3])
{
	uint64_t t0 = sys_time_us();
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
		vb[k] = vbucket(vel[k]);
	if (S.pending == 0 && movr_landed())
		S.pin_gen++;
	{
		int hs = H ? H->slot : -1;
		int32_t leaf = H ? wld_point_leaf(H, eye) : -1, cell = H ? eye_cell(H, eye) : -1;

		changed = !S.valid || hs != S.home_slot || leaf != S.leaf || cell != S.cell
			|| memcmp(vb, S.vb, sizeof(vb)) || S.pin_gen != S.pin_gen_seen
			|| wld_layer_gen != S.layer_gen_seen || S.coll_gen != S.coll_gen_seen;
		S.home_slot = hs;
		S.leaf = leaf;
		S.cell = cell;
	}
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
		/* the standing generation; a naming that could not be queued
		 * is retried in it */
		if (S.pending)
			name_all();
		S.st.reassert++;
	}
	/* the 128 KB seam budget while a seam pin is still missing chunks */
	for (s = 0; s < HL64_WORLD_LAYERS; s++)
		if (S.seam[s].on && wld_layer(s) && !wld_pin_resident(wld_layer(s)))
			seam_out = 1;
	io_set_seam(seam_out);

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
	p->movr_wait = 0;
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

void wld_stream_closed(int slot)
{
	if (S.seam[slot].on)
		S.pin_gen++;
	memset(&S.seam[slot], 0, sizeof(S.seam[slot]));
	memset(S.ccell[slot], 0, sizeof(S.ccell[slot]));
}

static int resident(const wld_layer_t *L, int c)
{
	return L->ch[c].state >= WLD_CH_RESIDENT;
}

int wld_pin_resident(const wld_layer_t *B)
{
	const seam_pin_t *p = &S.seam[B->slot];
	int i, k;

	if (!p->on)
		return 1;
	if (B->ready != 1)
		return 0;
	for (i = 0; i < p->n; i++)
	{
		uint16_t e = p->e[i];

		if (e & 0x8000)
		{
			const w64_sub_t *sb;
			const w64_meshlet_t *m;
			int nm;

			if ((e & 0x7FFF) >= B->dir->nsub || B->subs[e & 0x7FFF].kind != W64_SUB_MOVR)
				continue;
			sb = &B->subs[e & 0x7FFF];
			if (!resident(B, sb->chunk))
				return 0;
			m = (const w64_meshlet_t *)((const uint8_t *)B->ch[sb->chunk].data + 24);
			nm = ((const w64_rend_t *)B->ch[sb->chunk].data)->nmeshlets;
			for (k = 0; k < nm; k++)
				if (B->texs[m[k].tex].page != W64_NONE
					&& !resident(B, B->dir->texp_first + B->texs[m[k].tex].page))
					return 0;
		}
		else if (e < B->dir->nsect)
		{
			const w64_sector_t *s = &B->sects[e];

			if (!resident(B, s->rend_chunk))
				return 0;
			for (k = 0; k < s->page_count; k++)
				if (!resident(B, B->dir->texp_first + B->pagepool[s->page_first + k]))
					return 0;
		}
	}
	if (p->win)
	{
		const w64_grid_t *g = B->grid;
		float lo[3], hi[3];
		int32_t r[6];
		int x, y, z;

		for (k = 0; k < 3; k++)
		{
			lo[k] = p->box[k] - COL_WIN;
			hi[k] = p->box[k + 3] + COL_WIN;
		}
		cell_range(B, lo, hi, r);
		for (z = r[2]; z <= r[5]; z++)
			for (y = r[1]; y <= r[4]; y++)
				for (x = r[0]; x <= r[3]; x++)
				{
					const uint16_t *v = B->glists
						+ B->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

					for (i = 1; i <= v[0]; i++)
						if (!resident(B, B->dir->coll_first + v[i]))
							return 0;
				}
	}
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
	if (reset)
	{
		S.st.frames = 0;
		S.st.us_sum = 0;
		S.st.us_max = 0;
	}
}
