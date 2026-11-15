/* SPDX-License-Identifier: MIT */
/*
 * trail.c -- entity-following beam trails (TE_BEAMFOLLOW; HLSDK
 * common/const.h fields: entity, sprite, life in 0.1 s, width in 0.1
 * units, colour, brightness). The engine samples the followed entity's
 * position into a fixed ring of HL64_FX_BEAM_SEGS points, each dying
 * life after it was taken; the trail runs from the entity through its
 * points, each segment's brightness falling linearly with its age, and
 * outlives the entity until its last point dies. Drawn additive in the
 * translucent pass as beam strips (beam.c beam_strip). The sampling step
 * (life / HL64_FX_BEAM_SEGS, a new point only after 1 unit of movement),
 * the linear fade and its 8 brightness steps (one strip per step) are
 * this module's choices [own].
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "engine/render/fx_int.h"

#define NPT	HL64_FX_BEAM_SEGS

typedef struct {
	ent_h		h;		/* followed entity; 0 once it is gone */
	res_t		sprite;
	float		life, step, width;
	float		cur[3];		/* the entity's position this frame (global) */
	float		pt[NPT][3];	/* samples, global; ring, newest at head */
	double		t[NPT];		/* sample times */
	uint8_t		rgb[3], bright;
	uint8_t		live, n, head;
} fxtrail_t;

static fxtrail_t	pool[HL64_FX_TRAILS];

void trail_init(void)
{
	memset(pool, 0, sizeof(pool));
}

static void ent_pos(int e, float out[3])
{
	const float *gm = eng_layer_origin(ent_idc(e)->layer);
	int k;

	for (k = 0; k < 3; k++)
		out[k] = ent_cv(e)->origin[k] + gm[k];
}

static void push(fxtrail_t *tr, const float p[3])
{
	tr->head = (uint8_t)((tr->head + 1) % NPT);
	memcpy(tr->pt[tr->head], p, sizeof(tr->pt[0]));
	tr->t[tr->head] = fx_s.now;
	if (tr->n < NPT)
		tr->n++;
}

void fx_beam_follow(int e, res_t sprite, int life, int width, int r, int g, int b,
	int brightness)
{
	fxtrail_t *tr = NULL;
	ent_h h;
	int i;

	if (e < 0 || !ent_is_live(e) || life <= 0)
		return;
	h = ent_handle_of(e);
	/* the entity's trail is replaced, else a free one; a full pool drops */
	for (i = 0; i < HL64_FX_TRAILS; i++)
	{
		if (pool[i].live && pool[i].h == h)
		{
			tr = &pool[i];
			break;
		}
		if (!tr && !pool[i].live)
			tr = &pool[i];
	}
	if (!tr)
		return;
	memset(tr, 0, sizeof(*tr));
	tr->live = 1;
	tr->h = h;
	tr->sprite = sprite;
	tr->life = (float)life * 0.1f;
	tr->step = tr->life / (float)NPT;
	tr->width = (float)width * 0.1f;	/* const.h: width in 0.1's, as fx_beam */
	tr->rgb[0] = (uint8_t)r;
	tr->rgb[1] = (uint8_t)g;
	tr->rgb[2] = (uint8_t)b;
	tr->bright = (uint8_t)brightness;
	tr->head = NPT - 1;
	ent_pos(e, tr->cur);
	push(tr, tr->cur);
}

void trail_frame(void)
{
	int i;

	for (i = 0; i < HL64_FX_TRAILS; i++)
	{
		fxtrail_t *tr = &pool[i];
		int e;

		if (!tr->live)
			continue;
		/* the oldest points die */
		while (tr->n > 0 && fx_s.now - tr->t[(tr->head + NPT + 1 - tr->n) % NPT] >= tr->life)
			tr->n--;
		if (tr->h && (e = ent_of(tr->h)) >= 0)
		{
			const float *last = tr->pt[tr->head];
			float d2 = 0;
			int k;

			ent_pos(e, tr->cur);
			for (k = 0; k < 3; k++)
				d2 += (tr->cur[k] - last[k]) * (tr->cur[k] - last[k]);
			if (tr->n == 0 ? d2 >= 1 : fx_s.now - tr->t[tr->head] >= tr->step && d2 >= 1)
				push(tr, tr->cur);
		}
		else if (tr->h)
		{
			/* gone: its last position ends the trail */
			tr->h = 0;
			push(tr, tr->cur);
		}
		if (!tr->h && tr->n == 0)
			tr->live = 0;
	}
}

int trail_live(void)
{
	int i, n = 0;

	for (i = 0; i < HL64_FX_TRAILS; i++)
		n += pool[i].live;
	return n;
}

void trail_list(void)
{
	int i, k;

	for (i = 0; i < HL64_FX_TRAILS; i++)
	{
		const fxtrail_t *tr = &pool[i];
		float d = 0;
		uint8_t bl = tr->bright;
		int mode;

		if (!tr->live || tr->n == 0)
			continue;
		for (k = 0; k < 3; k++)
			d += (tr->cur[k] - fx_s.eye[k]) * (tr->cur[k] - fx_s.eye[k]);
		mode = trans_gpu_mode(FX_RM_TRANSADD, 0, &bl);
		if (bl)
			trans_add(TK_TRAIL, i, d, bl, (uint8_t)mode);
	}
}

void trail_draw(int id, uint8_t blend)
{
	const fxtrail_t *tr = &pool[id];
	const fx_sprite_t *spr;
	const fx_sprframe_t *fr;
	float pts[NPT + 1][3], side[NPT + 1][3], s[NPT + 1], age[NPT + 1], len = 0, rep;
	uint8_t q[NPT], rgba[4];
	int n = 0, i, k, a;

	if (!tr->live || !(spr = sprite_get(tr->sprite)))
		return;
	fr = &spr->frames[0];
	/* newest first: the entity (while it lives), then its samples */
	if (tr->h)
	{
		memcpy(pts[n], tr->cur, sizeof(pts[0]));
		age[n++] = 0;
	}
	for (i = 0; i < tr->n; i++)
	{
		int j = (tr->head + NPT - i) % NPT;

		memcpy(pts[n], tr->pt[j], sizeof(pts[0]));
		age[n++] = (float)(fx_s.now - tr->t[j]);
	}
	if (n < 2)
		return;
	n--;	/* segments */
	s[0] = 0;
	for (i = 1; i <= n; i++)
	{
		float d2 = 0;

		for (k = 0; k < 3; k++)
			d2 += (pts[i][k] - pts[i - 1][k]) * (pts[i][k] - pts[i - 1][k]);
		len += sqrtf(d2);
		s[i] = len;
	}
	if (len < 1)
		return;
	/* the texture repeats along the trail, at most 8 times (as beams) */
	rep = len / 8.0f > (float)fr->tw ? len / 8.0f : (float)fr->tw;
	for (i = 0; i <= n; i++)
	{
		const float *p0 = pts[i > 0 ? i - 1 : 0], *p1 = pts[i < n ? i + 1 : n];
		float dir[3], to[3], sd[3], l;

		for (k = 0; k < 3; k++)
		{
			dir[k] = p1[k] - p0[k];
			to[k] = pts[i][k] - fx_s.eye[k];
		}
		sd[0] = dir[1] * to[2] - dir[2] * to[1];
		sd[1] = dir[2] * to[0] - dir[0] * to[2];
		sd[2] = dir[0] * to[1] - dir[1] * to[0];
		l = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
		l = l > 1e-6f ? 0.5f * (tr->width > 0.5f ? tr->width : 0.5f) / l : 0;
		for (k = 0; k < 3; k++)
			side[i][k] = sd[k] * l;
		s[i] = s[i] * (float)fr->tw / rep;
	}
	/* each segment's brightness: blend x the life left at its middle,
	 * in 8 steps; a run of equal steps is one strip */
	for (i = 0; i < n; i++)
	{
		float f = 1.0f - 0.5f * (age[i] + age[i + 1]) / tr->life;

		q[i] = (uint8_t)(f <= 0 ? 0 : f >= 1 ? 7 : (int)(f * 8.0f));
	}
	memcpy(rgba, tr->rgb, 3);
	for (a = 0; a < n; a = i)
	{
		for (i = a + 1; i < n && q[i] == q[a]; i++)
			;
		rgba[3] = (uint8_t)(blend * (q[a] + 1) / 8);
		sprite_bind(spr, fr, GPU_FX_ADD, rgba);
		beam_strip((const float (*)[3])pts[a], i - a, (const float (*)[3])side[a], &s[a],
			fr->th, rgba);
	}
}
