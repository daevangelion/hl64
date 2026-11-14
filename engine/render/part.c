/* SPDX-License-Identifier: MIT */
/*
 * part.c -- particles (docs/specs/render-fx.md 5, docs/architecture.md
 * 9.5): one pool of HL64_FX_PARTICLES 24-byte points in the particles
 * region (blood streams, spark and tracer streaks, explosion embers, dust),
 * gravity x sv_gravity per type, colour ramp, die time. Drawn once a frame:
 * points as one TinyPX batch (camera-relative s16, as the old tree's
 * n64/port/r64_part.c packed them: audit entry r64_part.c, QUAKE-INTERFACE
 * with CLEAN content; its D_* / particle_t / r_origin / vpn bindings are
 * replaced by this pool and the effects eye), streaks as additive
 * camera-facing quads.
 */
#include <math.h>
#include <string.h>

#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "engine/mem/carve.h"
#include "engine/render/fx_int.h"

typedef struct {
	float		org[3];
	int16_t		vel[3];		/* u/s */
	uint16_t	life;		/* ms left */
	uint8_t		rgb[3];
	uint8_t		type;		/* PT_* */
} part_t;				/* 24 */
_Static_assert(sizeof(part_t) == 24, "part_t");

#define STREAK_S	0.02f	/* streak length: travel in 20 ms [own] */
#define STREAK_W	0.75f	/* half width (u) [own] */
#define POINT_PIX	2

static part_t	*pool;		/* particles region */
static int	n;
static uint8_t	pal[768];
static int	have_pal;
static uint32_t	dropped;

void part_init(void)
{
	uint32_t size, rom;

	if (HL64_FX_PARTICLES * sizeof(part_t) > mem_region_bytes(MEM_PARTICLES))
		sys_error("fx: %d particles > particles region", HL64_FX_PARTICLES);
	pool = mem_region(MEM_PARTICLES);
	n = 0;
	dropped = 0;
	rom = sys_cart_addr("gsfx/palette.lmp", &size);
	have_pal = rom && size >= 768 && sys_cart_read(pal, sizeof(pal), rom, size, 0, 768);
}

void part_color(uint8_t index, uint8_t rgb[3])
{
	if (have_pal)
		memcpy(rgb, pal + 3 * index, 3);
	else
		rgb[0] = rgb[1] = rgb[2] = index;
}

static int16_t clamp16(float v)
{
	return (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : lrintf(v));
}

int part_add_rgb(const float org_g[3], const float vel[3], const uint8_t rgb[3], int type,
	float life)
{
	part_t *p;
	int k;

	if (n >= HL64_FX_PARTICLES)
	{
		dropped++;
		return 0;
	}
	if (life <= 0)
		return 1;
	p = &pool[n++];
	for (k = 0; k < 3; k++)
	{
		p->org[k] = org_g[k];
		p->vel[k] = clamp16(vel[k]);
		p->rgb[k] = rgb[k];
	}
	p->life = (uint16_t)(life >= 65.0f ? 65000 : life * 1000.0f + 0.5f);
	p->type = (uint8_t)type;
	return 1;
}

int part_live(void)
{
	return n;
}

uint32_t part_dropped(int reset)
{
	uint32_t d = dropped;

	if (reset)
		dropped = 0;
	return d;
}

void part_frame(void)
{
	float dt = fx_s.dt, g = fx_gravity();
	uint32_t ms = (uint32_t)(dt * 1000.0f + 0.5f);
	int i = 0, k;

	while (i < n)
	{
		part_t *p = &pool[i];
		float gz;

		if (p->life <= ms)
		{
			pool[i] = pool[--n];	/* swap-remove; order is not drawn-visible */
			continue;
		}
		p->life = (uint16_t)(p->life - ms);
		switch (p->type)
		{
		case PT_GRAV:
		case PT_FADE:
		case PT_TRACER_GRAV:
			gz = g;
			break;
		case PT_SLOWGRAV:
			gz = g * 0.5f;
			break;
		default:
			gz = 0;
		}
		if (gz != 0)
			p->vel[2] = clamp16((float)p->vel[2] - gz * dt);
		for (k = 0; k < 3; k++)
			p->org[k] += (float)p->vel[k] * dt;
		i++;
	}
}

/* Colour now: PT_FADE ramps down over its last 400 ms. */
static void colour(const part_t *p, uint8_t out[4])
{
	int k, s = 255;

	if (p->type == PT_FADE && p->life < 400)
		s = p->life * 255 / 400;
	for (k = 0; k < 3; k++)
		out[k] = (uint8_t)(p->rgb[k] * s / 255);
	out[3] = 255;
}

/* Visible: in front of the eye and inside the reachable quarter-unit range. */
static int ahead(const float o[3])
{
	float d = (o[0] - fx_s.eye[0]) * fx_s.fwd[0] + (o[1] - fx_s.eye[1]) * fx_s.fwd[1]
		+ (o[2] - fx_s.eye[2]) * fx_s.fwd[2];

	return d > 4.0f;
}

static void flush_streaks(gpu_fx_vert_t *v, uint8_t *idx, int *nv)
{
	if (*nv)
		gpu_fx_tris(v, *nv, idx, *nv / 2);
	*nv = 0;
}

void part_draw(void)
{
	static gpu_fx_point_t pts[HL64_FX_PARTICLES];
	gpu_fx_vert_t v[64];
	uint8_t idx[96];
	int i, np = 0, nv = 0, k, mode_set = 0;

	for (i = 0; i < 16; i++)
	{
		idx[6 * i + 0] = (uint8_t)(4 * i);
		idx[6 * i + 1] = (uint8_t)(4 * i + 1);
		idx[6 * i + 2] = (uint8_t)(4 * i + 2);
		idx[6 * i + 3] = (uint8_t)(4 * i);
		idx[6 * i + 4] = (uint8_t)(4 * i + 2);
		idx[6 * i + 5] = (uint8_t)(4 * i + 3);
	}
	for (i = 0; i < n; i++)
	{
		const part_t *p = &pool[i];

		if (!ahead(p->org))
			continue;
		if (p->type == PT_TRACER || p->type == PT_TRACER_GRAV)
		{
			float tail[3], d[3], to[3], s[3], len;
			uint8_t c[4];
			int16_t q[4][3];
			int ok = 1;

			for (k = 0; k < 3; k++)
			{
				d[k] = (float)p->vel[k] * STREAK_S;
				tail[k] = p->org[k] - d[k];
				to[k] = p->org[k] - fx_s.eye[k];
			}
			/* side = d x (p - eye), normalised to the half width */
			s[0] = d[1] * to[2] - d[2] * to[1];
			s[1] = d[2] * to[0] - d[0] * to[2];
			s[2] = d[0] * to[1] - d[1] * to[0];
			len = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
			if (len < 1e-4f)
				continue;
			for (k = 0; k < 3; k++)
				s[k] *= STREAK_W / len;
			{
				float c0[3], c1[3], c2[3], c3[3];

				for (k = 0; k < 3; k++)
				{
					c0[k] = p->org[k] + s[k];
					c1[k] = p->org[k] - s[k];
					c2[k] = tail[k] - s[k];
					c3[k] = tail[k] + s[k];
				}
				ok = fx_cam_pos(c0, q[0]) && fx_cam_pos(c1, q[1]) && fx_cam_pos(c2, q[2])
					&& fx_cam_pos(c3, q[3]);
			}
			if (!ok)
				continue;
			colour(p, c);
			/* additive head-to-tail fade as unit colour x brightness (render-fx.md 2a) */
			add_split(c, 255, c);
			if (!mode_set)
			{
				gpu_fx_mode(GPU_FX_STREAK, NULL);
				mode_set = 1;
			}
			for (k = 0; k < 4; k++)
			{
				gpu_fx_vert_t *o = &v[nv + k];

				memcpy(o->pos, q[k], sizeof(o->pos));
				o->st[0] = o->st[1] = 0;
				memcpy(o->rgba, c, 3);
				o->rgba[3] = k < 2 ? c[3] : 0;	/* bright head, dark tail */
			}
			nv += 4;
			if (nv == 64)
				flush_streaks(v, idx, &nv);
			continue;
		}
		if (!fx_cam_pos(p->org, pts[np].pos))
			continue;
		pts[np].size = POINT_PIX;
		colour(p, pts[np].rgba);
		np++;
	}
	flush_streaks(v, idx, &nv);
	if (np)
	{
		gpu_fx_mode(GPU_FX_POINT, NULL);
		gpu_fx_points(pts, np);
	}
}
