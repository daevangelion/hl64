/* SPDX-License-Identifier: MIT */
/*
 * dlight.c -- the dynamic light pool, the world falloff and the model
 * light terms (docs/specs/dlights.md 1-4, lightstyles.md 4), written from
 * the spec. Sources: entity effects (EF_MUZZLEFLASH / BRIGHTLIGHT /
 * DIMLIGHT; the local player's DIMLIGHT is night vision instead,
 * architecture.md D12) each frame here, game effects
 * (fx_dlight / fx_elight / muzzle flashes) through engine/render/fx.c.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/limits.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"

static ren_dlight_t	lights[HL64_DLIGHTS];
static uint8_t		live[HL64_DLIGHTS];
static uint32_t		src_rng = 0x9E3779B9u;	/* rand32 (render stream of the pool) */

ren_dlight_t *ren_dlight_alloc(int key, double time)
{
	int i, pick = 0;

	if (key)
	{
		for (i = 0; i < HL64_DLIGHTS; i++)
			if (live[i] && lights[i].key == key)
				break;
		if (i < HL64_DLIGHTS)
		{
			pick = i;
			goto out;
		}
	}
	for (i = 0; i < HL64_DLIGHTS; i++)
		if (!live[i] || lights[i].die < time)
		{
			pick = i;
			break;
		}
out:
	memset(&lights[pick], 0, sizeof(lights[pick]));
	lights[pick].key = key;
	lights[pick].world = 1;
	live[pick] = 1;
	return &lights[pick];
}

void ren_dlight_reset(void)
{
	memset(lights, 0, sizeof(lights));
	memset(live, 0, sizeof(live));
	src_rng = 0x9E3779B9u;
}

void ren_dlight_frame(double time, double dt)
{
	int i;

	for (i = 0; i < HL64_DLIGHTS; i++)
	{
		ren_dlight_t *l = &lights[i];

		if (!live[i])
			continue;
		if (l->die < time)
		{
			live[i] = 0;
			continue;
		}
		l->radius -= l->decay * (float)dt;
		if (l->radius <= 0)
			live[i] = 0;
	}
}

int ren_dlight_live(int i)
{
	return live[i];
}

ren_dlight_t *ren_dlight(int i)
{
	return &lights[i];
}

/* max(0, (radius - |p|) - q) when radius - |p| >= minlight, else 0 */
float ren_dlight_falloff(float radius, float minlight, float p, float q)
{
	float reach = radius - (p < 0 ? -p : p), c;

	if (reach < minlight)
		return 0;
	c = reach - q;
	return c > 0 ? c : 0;
}

int ren_model_light(int e, const float org_g[3], float base, int style_slot, int amount)
{
	float v = base;
	int i;

	if (style_slot >= 0 && style_slot < WLD_STYLE_SLOTS_ALL && amount)
		v += (float)amount * (float)wld_style_value(style_slot) / 264.0f;
	for (i = 0; i < HL64_DLIGHTS; i++)
	{
		const ren_dlight_t *l = &lights[i];
		float dx, dy, dz, a;

		if (!live[i] || (l->entity && l->entity != e))
			continue;
		dx = org_g[0] - l->origin[0];
		dy = org_g[1] - l->origin[1];
		dz = org_g[2] - l->origin[2];
		a = l->radius - sqrtf(dx * dx + dy * dy + dz * dz);
		if (a > 0)
			v += a;
	}
	if (v < 32)
		v = 32;
	if (v > 255)
		v = 255;
	return (int)v;
}

static uint32_t rand32(void)
{
	src_rng ^= src_rng << 13;
	src_rng ^= src_rng >> 17;
	src_rng ^= src_rng << 5;
	return src_rng & 31;
}

/* One-frame light: dead on the next frame (die < next time). */
static void one_frame(int key, const float org[3], float radius, double time)
{
	ren_dlight_t *l = ren_dlight_alloc(key, time);

	memcpy(l->origin, org, sizeof(l->origin));
	l->radius = radius;
	l->die = time;
	l->color[0] = l->color[1] = l->color[2] = 255;
}

void ren_dlight_sources(double time)
{
	int e, k;

	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
	{
		ent_rnd_t *r;
		const float *gm;
		float org[3];

		r = ent_rnd(e);
		if (!(r->effects & (EF_MUZZLEFLASH | EF_BRIGHTLIGHT | EF_DIMLIGHT | EF_LIGHT)))
			continue;
		gm = eng_layer_origin(ent_idc(e)->layer);
		for (k = 0; k < 3; k++)
			org[k] = ent_cv(e)->origin[k] + gm[k];
		if (r->effects & EF_MUZZLEFLASH)
		{
			float f[3], rt[3], u[3], p[3];
			ren_dlight_t *l;

			ren_angle_axes(ent_cv(e)->angles, f, rt, u);
			for (k = 0; k < 3; k++)
				p[k] = org[k] + 18.0f * f[k];
			p[2] += 16;
			l = ren_dlight_alloc(e, time);
			memcpy(l->origin, p, sizeof(p));
			l->radius = 200.0f + (float)rand32();
			l->minlight = 32;
			l->die = time + 0.1;
			l->color[0] = l->color[1] = l->color[2] = 255;
			/* consumed: one flash per set (the game sets it per shot) */
			r->effects &= (uint16_t)~EF_MUZZLEFLASH;
		}
		if (r->effects & EF_BRIGHTLIGHT)
		{
			float p[3] = { org[0], org[1], org[2] + 16 };

			one_frame(e, p, 400.0f + (float)rand32(), time);
		}
		else if ((r->effects & EF_DIMLIGHT) && e != ENT_PLAYER)
			one_frame(e, org, 200.0f + (float)rand32(), time);
		/* EF_LIGHT (the rpg rocket's): dlights.md 1 gives no radius:
		 * [EST] 120 u, one frame, kept small (meshlet copies scale with
		 * the sphere) */
		if ((r->effects & EF_LIGHT) && !(r->effects & (EF_BRIGHTLIGHT | EF_DIMLIGHT)))
			one_frame(e, org, 120.0f, time);
	}
}
