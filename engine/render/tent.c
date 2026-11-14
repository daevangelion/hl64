/* SPDX-License-Identifier: MIT */
/*
 * tent.c -- temporary entities (docs/specs/render-fx.md 5): a fixed pool
 * of HL64_FX_TEMPS sprites and studio models (explosions, smoke, ricochet
 * and muzzle sprites, blood drops, gibs, shells, debris) living on the
 * effects clock: gravity, bounce on the world (the effect's bounce sound
 * set on impact), animation, die time, fade over the last second. Studio
 * temps draw under their own cap (HL64_FX_TEMPS_STUDIO, nearest first).
 * Written from the spec; the simulation rules (bounce damping, rest test,
 * spin) are this module's own.
 */
#include <math.h>
#include <string.h>

#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/snd.h"
#include "hl64/studio.h"
#include "engine/render/fx_int.h"
#include "engine/render/gpu_studio.h"

#define BOUNCE		0.5f	/* velocity kept after a bounce [own] */
#define REST_SPEED	40.0f	/* below this on a floor: rest [own] */
#define MAX_SOUNDS	3	/* impacts that play the bounce set [own] */

static fx_tent_t	pool[HL64_FX_TEMPS];
static int		nlive;
static uint32_t		dropped;

void tent_init(void)
{
	memset(pool, 0, sizeof(pool));
	nlive = 0;
	dropped = 0;
}

int tent_live(void)
{
	return nlive;
}

uint32_t tent_dropped(int reset)
{
	uint32_t d = dropped;

	if (reset)
		dropped = 0;
	return d;
}

fx_tent_t *tent_alloc(const float org_g[3], res_t model)
{
	int i;

	for (i = 0; i < HL64_FX_TEMPS; i++)
		if (!pool[i].live)
		{
			fx_tent_t *t = &pool[i];

			memset(t, 0, sizeof(*t));
			memcpy(t->org, org_g, sizeof(t->org));
			t->model = model;
			t->scale = 1;
			t->amt = 255;
			t->color[0] = t->color[1] = t->color[2] = 255;
			t->die = fx_s.now + 1.0;
			t->live = 1;
			nlive++;
			return t;
		}
	dropped++;
	return NULL;
}

static void kill(fx_tent_t *t)
{
	t->live = 0;
	nlive--;
}

static int nframes(const fx_tent_t *t)
{
	int n = res_model_frames(t->model);

	return n > 0 ? n : 1;
}

static void move(fx_tent_t *t, float dt)
{
	float to[3];
	int k;

	for (k = 0; k < 3; k++)
		to[k] = t->org[k] + t->vel[k] * dt;
	if (t->flags & TF_COLLIDE)
	{
		col_trace_t tr;

		fx_trace(t->org, to, COL_MOVE_NOMONSTERS, &tr);
		if (tr.frac < 1.0f && !tr.allsolid)
		{
			float vn = t->vel[0] * tr.normal[0] + t->vel[1] * tr.normal[1]
				+ t->vel[2] * tr.normal[2], sp;

			for (k = 0; k < 3; k++)
			{
				to[k] = tr.end[k] + tr.normal[k] * 0.25f;
				t->vel[k] = (t->vel[k] - 2.0f * vn * tr.normal[k]) * BOUNCE;
			}
			sp = sqrtf(t->vel[0] * t->vel[0] + t->vel[1] * t->vel[1] + t->vel[2] * t->vel[2]);
			if (t->bounce && t->hits < MAX_SOUNDS)
				fx_play(t->bounce - 1, tr.end, sp > 300 ? 0.8f : 0.4f, ATTN_NORM);
			t->hits++;
			if (tr.normal[2] > 0.7f && sp < REST_SPEED)
			{
				memset(t->vel, 0, sizeof(t->vel));
				memset(t->avel, 0, sizeof(t->avel));
				t->flags &= (uint16_t)~(TF_GRAVITY | TF_SLOWGRAV | TF_COLLIDE | TF_ROTATE);
				t->ang[0] = 0;
				t->ang[2] = 0;
			}
		}
	}
	memcpy(t->org, to, sizeof(to));
}

void tent_frame(void)
{
	float dt = fx_s.dt, g = fx_gravity();
	int i, k;

	for (i = 0; i < HL64_FX_TEMPS; i++)
	{
		fx_tent_t *t = &pool[i];

		if (!t->live)
			continue;
		if (fx_s.now >= t->die)
		{
			kill(t);
			continue;
		}
		if (t->flags & (TF_ANIMATE | TF_LOOP))
		{
			int n = nframes(t);

			t->frame += t->framerate * dt;
			if (t->frame >= (float)n)
			{
				if (!(t->flags & TF_LOOP))
				{
					kill(t);
					continue;
				}
				t->frame = fmodf(t->frame, (float)n);
			}
		}
		if (t->flags & TF_ROTATE)
			for (k = 0; k < 3; k++)
				t->ang[k] = fmodf(t->ang[k] + t->avel[k] * dt, 360.0f);
		if (t->flags & TF_GRAVITY)
			t->vel[2] -= g * dt;
		else if (t->flags & TF_SLOWGRAV)
			t->vel[2] -= g * 0.5f * dt;
		if (t->vel[0] != 0 || t->vel[1] != 0 || t->vel[2] != 0)
			move(t, dt);
		if ((t->flags & TF_SMOKE) && (t->vel[0] != 0 || t->vel[1] != 0 || t->vel[2] != 0))
		{
			static const uint8_t grey[3] = { 90, 90, 90 };
			static const float up[3] = { 0, 0, 16 };

			part_add_rgb(t->org, up, grey, PT_STATIC, 0.5f);
		}
	}
}

/* Alpha with the last-second fade. */
static int alpha(const fx_tent_t *t)
{
	float left = (float)(t->die - fx_s.now);

	if ((t->flags & TF_FADEOUT) && left < 1.0f)
		return (int)((float)t->amt * (left > 0 ? left : 0));
	return t->amt;
}

static float dist2(const float o[3])
{
	float d = 0;
	int k;

	for (k = 0; k < 3; k++)
		d += (o[k] - fx_s.eye[k]) * (o[k] - fx_s.eye[k]);
	return d;
}

static int in_front(const float o[3], float r)
{
	return (o[0] - fx_s.eye[0]) * fx_s.fwd[0] + (o[1] - fx_s.eye[1]) * fx_s.fwd[1]
		+ (o[2] - fx_s.eye[2]) * fx_s.fwd[2] > -r;
}

void tent_list_sprites(void)
{
	int i;

	for (i = 0; i < HL64_FX_TEMPS; i++)
	{
		const fx_tent_t *t = &pool[i];
		uint8_t b;
		int mode;

		if (!t->live || RES_KIND(t->model) != RES_K_SPRITE || !in_front(t->org, 256))
			continue;
		b = (uint8_t)trans_blend_amt(t->rendermode, t->renderfx, alpha(t), -1,
			sqrtf(dist2(t->org)));
		mode = trans_gpu_mode(t->rendermode, 0, &b);
		if (!b && t->rendermode != FX_RM_NORMAL)
			continue;
		trans_add(TK_TEMP, i, dist2(t->org), b, (uint8_t)mode);
	}
}

void tent_draw_studio(void)
{
	int pick[HL64_FX_TEMPS_STUDIO], n = 0, i, j;

	/* nearest HL64_FX_TEMPS_STUDIO studio temps in front of the eye */
	for (i = 0; i < HL64_FX_TEMPS; i++)
	{
		const fx_tent_t *t = &pool[i];

		if (!t->live || RES_KIND(t->model) != RES_K_MODEL || !in_front(t->org, 64))
			continue;
		if (n < HL64_FX_TEMPS_STUDIO)
			pick[n++] = i;
		else
		{
			int far = 0;

			for (j = 1; j < n; j++)
				if (dist2(pool[pick[j]].org) > dist2(pool[pick[far]].org))
					far = j;
			fx_s.st.trans_dropped++;
			if (dist2(pool[pick[far]].org) > dist2(t->org))
				pick[far] = i;
		}
	}
	for (i = 0; i < n; i++)
	{
		const fx_tent_t *t = &pool[pick[i]];
		int a = alpha(t);

		if (t->rendermode == FX_RM_NORMAL && a >= 255)
			studio_draw_temp(t->model, t->body, 0, t->org, t->ang, GPU_STUDIO_OPAQUE, 255);
		else if (a > 0)
			trans_add(TK_TEMP, pick[i], dist2(t->org), (uint8_t)a, GPU_STUDIO_BLEND);
	}
}

/* Translucent pass callback (trans.c). */
void tent_draw(int i, uint8_t blend, int mode)
{
	const fx_tent_t *t = &pool[i];

	if (!t->live)
		return;
	if (RES_KIND(t->model) == RES_K_MODEL)
	{
		studio_draw_temp(t->model, t->body, 0, t->org, t->ang, mode, blend);
		gpu_fx_state_lost();
		return;
	}
	{
		const fx_sprite_t *s = sprite_get(t->model);
		uint8_t rgba[4];

		if (!s)
			return;
		/* render-fx.md 2: TransColor sprites keep their palette colours */
		if (t->rendermode == FX_RM_TRANSCOLOR)
			rgba[0] = rgba[1] = rgba[2] = 255;
		else
			memcpy(rgba, t->color, 3);
		rgba[3] = blend;
		sprite_draw(s, (int)t->frame, t->org, t->ang, t->scale, mode, rgba);
	}
}
