/* SPDX-License-Identifier: MIT */
/*
 * beam.c -- beams and beam rings (docs/specs/render-fx.md 5; HLSDK
 * TE_BEAMPOINTS / BEAMENTPOINT / BEAMENTS / BEAMCYLINDER fields): entity <->
 * entity, entity <-> point and point <-> point, width, noise amplitude
 * (random perpendicular offsets per segment, new every frame), colour,
 * brightness, sprite frame animation (starting frame, rate), texture
 * scroll, shade in / out (brightness ramped along the beam), life; drawn
 * additive in the translucent pass as camera-facing strips of at most
 * HL64_FX_BEAM_SEGS segments. Rings (TE_BEAMCYLINDER) are vertical bands
 * that grow to their radius over their life. Written from the spec;
 * segment counts, the noise scale, the stepped shade (one brightness per
 * segment, the additive combiner has no per-vertex shade) and the keyed
 * beams' pool priority are this module's choices [own].
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "engine/core/hlmath.h"
#include "engine/render/fx_int.h"

typedef struct {
	ent_h		h0, h1;		/* end entities (0: the point) */
	float		p0[3], p1[3];	/* points, global */
	double		start, die;	/* die 0: until killed */
	float		width, noise, speed, radius, framerate;
	res_t		sprite;
	uint8_t		rgb[3], bright;
	uint8_t		live, ring, frame0, flags;
	int16_t		key;		/* fx_beam's key, -1 none */
} fxbeam_t;

static fxbeam_t	pool[HL64_FX_BEAMS];
static int	nlive;
static uint32_t	dropped;

void beam_init(void)
{
	memset(pool, 0, sizeof(pool));
	nlive = 0;
	dropped = 0;
}

int beam_live(void)
{
	return nlive;
}

uint32_t beam_dropped(int reset)
{
	uint32_t d = dropped;

	if (reset)
		dropped = 0;
	return d;
}

/* A slot for a beam: the live beam holding key (replaced), else a free
 * one; a keyed beam (a game entity's, until killed) with the pool full
 * takes the unkeyed beam's that ends first (its strike would be gone
 * soon; the entity's beam would be gone for good) [own]. */
static fxbeam_t *alloc(int key)
{
	int i, at = -1, temp = -1;

	for (i = 0; i < HL64_FX_BEAMS; i++)
	{
		if (key >= 0 && pool[i].live && pool[i].key == key)
		{
			at = i;
			nlive--;
			break;
		}
		if (at < 0 && !pool[i].live)
			at = i;
		if (pool[i].live && pool[i].key < 0 && pool[i].die > 0
			&& (temp < 0 || pool[i].die < pool[temp].die))
			temp = i;
	}
	if (at < 0 && key >= 0 && temp >= 0)
	{
		at = temp;
		nlive--;
		dropped++;
	}
	if (at < 0)
	{
		dropped++;
		return NULL;
	}
	memset(&pool[at], 0, sizeof(pool[at]));
	pool[at].live = 1;
	pool[at].key = (int16_t)key;
	pool[at].start = fx_s.now;
	nlive++;
	return &pool[at];
}

int fx_beam(int key, int e0, const float p0[3], int e1, const float p1[3], res_t sprite,
	int frame, int framerate, int width, int noise, int r, int g, int b, int brightness,
	int speed, int life, int flags)
{
	fxbeam_t *bm;

	if (key < 0)
		key = -1;
	if (!(bm = alloc(key)))
		return key;
	bm->h0 = e0 >= 0 && ent_is_live(e0) ? ent_handle_of(e0) : 0;
	bm->h1 = e1 >= 0 && ent_is_live(e1) ? ent_handle_of(e1) : 0;
	if (p0)
		fx_global(p0, bm->p0);
	if (p1)
		fx_global(p1, bm->p1);
	bm->sprite = sprite;
	bm->width = (float)width * 0.1f;	/* const.h: line width in 0.1's */
	bm->noise = (float)noise * 0.01f;	/* const.h: amplitude in 0.01's */
	bm->speed = (float)speed * 0.1f;	/* scroll in 0.1's */
	bm->rgb[0] = (uint8_t)r;
	bm->rgb[1] = (uint8_t)g;
	bm->rgb[2] = (uint8_t)b;
	bm->bright = (uint8_t)brightness;
	bm->frame0 = (uint8_t)frame;
	bm->framerate = (float)framerate * 0.1f;	/* const.h: frame rate in 0.1's */
	bm->flags = (uint8_t)(flags & (FX_BEAM_SHADEIN | FX_BEAM_SHADEOUT));
	bm->die = life > 0 ? fx_s.now + (double)life * 0.1 : 0;
	return key;
}

void fx_beam_kill(int key)
{
	int i;

	if (key < 0)
		return;
	for (i = 0; i < HL64_FX_BEAMS; i++)
		if (pool[i].live && pool[i].key == key)
		{
			pool[i].live = 0;
			nlive--;
			return;
		}
}

int beam_ring(const float org_g[3], float r, res_t sprite, float life, int width, int noise,
	const uint8_t rgb[3], int bright)
{
	fxbeam_t *bm;

	if (life <= 0 || !(bm = alloc(-1)))
		return -1;
	memcpy(bm->p0, org_g, sizeof(bm->p0));
	bm->ring = 1;
	bm->radius = r;
	bm->sprite = sprite;
	bm->width = (float)width;		/* band height [own] */
	bm->noise = (float)noise * 0.01f;
	memcpy(bm->rgb, rgb, 3);
	bm->bright = (uint8_t)bright;
	bm->die = fx_s.now + life;
	return 0;
}

/* End point now (global); 0 when its entity is gone. */
static int end_point(ent_h h, const float p[3], float out[3])
{
	int e, k;
	const float *gm;

	if (!h)
	{
		memcpy(out, p, 3 * sizeof(float));
		return 1;
	}
	if ((e = ent_of(h)) < 0)
		return 0;
	gm = eng_layer_origin(ent_idc(e)->layer);
	for (k = 0; k < 3; k++)
		out[k] = ent_cv(e)->origin[k] + gm[k];
	return 1;
}

void beam_frame(void)
{
	int i;

	for (i = 0; i < HL64_FX_BEAMS; i++)
	{
		fxbeam_t *b = &pool[i];
		float t[3];

		if (!b->live)
			continue;
		if ((b->die > 0 && fx_s.now >= b->die) || !end_point(b->h0, b->p0, t)
			|| !end_point(b->h1, b->p1, t))
		{
			b->live = 0;
			nlive--;
		}
	}
}

void beam_list(void)
{
	int i, k;

	for (i = 0; i < HL64_FX_BEAMS; i++)
	{
		fxbeam_t *b = &pool[i];
		float a[3], c[3], d = 0;
		uint8_t bl = b->bright;
		int mode;

		if (!b->live || !end_point(b->h0, b->p0, a) || !end_point(b->h1, b->p1, c))
			continue;
		for (k = 0; k < 3; k++)
		{
			float m = b->ring ? a[k] : 0.5f * (a[k] + c[k]);

			d += (m - fx_s.eye[k]) * (m - fx_s.eye[k]);
		}
		mode = trans_gpu_mode(FX_RM_TRANSADD, 0, &bl);
		if (bl)
			trans_add(TK_BEAM, i, d, bl, (uint8_t)mode);
	}
}

static void cross(const float a[3], const float b[3], float o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static float normalize(float v[3])
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (l > 1e-6f)
	{
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
	return l;
}

/* Strip over points pts[0..n] with per-point side offsets; s along. */
static void strip(const float (*pts)[3], int n, const float (*side)[3], const float *s, float th,
	const uint8_t rgba[4])
{
	static uint8_t idx[6 * HL64_FX_BEAM_SEGS];
	gpu_fx_vert_t v[2 * (HL64_FX_BEAM_SEGS + 1)];
	int i, k;

	for (i = 0; i <= n; i++)
	{
		float a[3], b[3];

		for (k = 0; k < 3; k++)
		{
			a[k] = pts[i][k] + side[i][k];
			b[k] = pts[i][k] - side[i][k];
		}
		if (!fx_cam_pos(a, v[2 * i].pos) || !fx_cam_pos(b, v[2 * i + 1].pos))
			return;
		v[2 * i].st[0] = v[2 * i + 1].st[0] = (int16_t)(s[i] * 32.0f);
		v[2 * i].st[1] = 0;
		v[2 * i + 1].st[1] = (int16_t)(th * 32.0f);
		memcpy(v[2 * i].rgba, rgba, 4);
		memcpy(v[2 * i + 1].rgba, rgba, 4);
	}
	for (i = 0; i < n; i++)
	{
		idx[6 * i + 0] = (uint8_t)(2 * i);
		idx[6 * i + 1] = (uint8_t)(2 * i + 1);
		idx[6 * i + 2] = (uint8_t)(2 * i + 3);
		idx[6 * i + 3] = (uint8_t)(2 * i);
		idx[6 * i + 4] = (uint8_t)(2 * i + 3);
		idx[6 * i + 5] = (uint8_t)(2 * i + 2);
	}
	gpu_fx_tris(v, 2 * (n + 1), idx, 2 * n);
}

/* Segments of a shaded beam (one brightness step each) [own]. */
#define SHADE_SEGS	8

/* The sprite's entity frame at time t: from the starting frame at the
 * frame rate, wrapping. */
static int frame_at(const fxbeam_t *b, const fx_sprite_t *spr, double t)
{
	int f = (int)b->frame0 + (int)((t - b->start) * (double)b->framerate);

	return spr->nframes > 1 ? f % spr->nframes : 0;
}

#ifdef HL64_HOST
int beam_test_frame(int key, double t)
{
	const fx_sprite_t *spr;
	int i;

	for (i = 0; i < HL64_FX_BEAMS; i++)
		if (pool[i].live && pool[i].key == key)
			return (spr = sprite_get(pool[i].sprite)) ? frame_at(&pool[i], spr, t) : -1;
	return -1;
}
#endif

void beam_draw(int id, uint8_t blend)
{
	const fxbeam_t *b = &pool[id];
	const fx_sprite_t *spr;
	const fx_sprframe_t *fr;
	float a[3], c[3], d[3], len, pts[HL64_FX_BEAM_SEGS + 1][3];
	float side[HL64_FX_BEAM_SEGS + 1][3], s[HL64_FX_BEAM_SEGS + 1], rep, scroll;
	uint8_t rgba[4];
	int n, i, k;

	if (!b->live || !(spr = sprite_get(b->sprite)) || !end_point(b->h0, b->p0, a)
		|| !end_point(b->h1, b->p1, c))
		return;
	fr = &spr->frames[sprite_image(spr, frame_at(b, spr, fx_s.now), fx_s.now)];
	memcpy(rgba, b->rgb, 3);
	rgba[3] = blend;
	sprite_bind(spr, fr, GPU_FX_ADD, rgba);
	scroll = (float)fmod(fx_s.now * b->speed, 1.0) * (float)fr->tw;
	if (b->ring)
	{
		/* vertical band around p0: radius grows to r over the life */
		float life = (float)(b->die - b->start), f = life > 0
			? (float)((fx_s.now - b->start) / life) : 1, r = b->radius * (f > 1 ? 1 : f);

		n = HL64_FX_BEAM_SEGS;
		for (i = 0; i <= n; i++)
		{
			float t = 6.2831853f * (float)i / (float)n;

			pts[i][0] = a[0] + r * hl_cosf(t);
			pts[i][1] = a[1] + r * hl_sinf(t);
			pts[i][2] = a[2] + 0.5f * b->width;
			side[i][0] = side[i][1] = 0;
			side[i][2] = 0.5f * b->width;
			s[i] = scroll + (float)fr->tw * 4.0f * (float)i / (float)n;
		}
		strip((const float (*)[3])pts, n, (const float (*)[3])side, s, fr->th, rgba);
		return;
	}
	for (k = 0; k < 3; k++)
		d[k] = c[k] - a[k];
	len = normalize(d);
	if (len < 1)
		return;
	n = b->noise > 0 ? (int)(len / 16.0f) : 1;
	if (b->flags && n < SHADE_SEGS)
		n = SHADE_SEGS;
	if (n < 1)
		n = 1;
	if (n > HL64_FX_BEAM_SEGS)
		n = HL64_FX_BEAM_SEGS;
	/* the texture repeats along the beam, at most 8 times */
	rep = len / 8.0f > (float)fr->tw ? len / 8.0f : (float)fr->tw;
	for (i = 0; i <= n; i++)
	{
		float t = (float)i / (float)n, to[3], sd[3], perp[3], amp;

		for (k = 0; k < 3; k++)
			pts[i][k] = a[k] + d[k] * len * t;
		/* random perpendicular offset inside the strip, ends fixed */
		amp = (i > 0 && i < n) ? b->noise * len * 0.25f : 0;
		if (amp > 0)
		{
			float rnd[3];

			/* draws in order: initializer side effects are unsequenced (K10) */
			rnd[0] = fx_randf(-1, 1);
			rnd[1] = fx_randf(-1, 1);
			rnd[2] = fx_randf(-1, 1);

			cross(d, rnd, perp);
			for (k = 0; k < 3; k++)
				pts[i][k] += perp[k] * amp;
		}
		for (k = 0; k < 3; k++)
			to[k] = pts[i][k] - fx_s.eye[k];
		cross(d, to, sd);
		normalize(sd);
		for (k = 0; k < 3; k++)
			side[i][k] = sd[k] * 0.5f * (b->width > 0.5f ? b->width : 0.5f);
		s[i] = scroll + len * t * (float)fr->tw / rep;
	}
	if (!b->flags)
	{
		strip((const float (*)[3])pts, n, (const float (*)[3])side, s, fr->th, rgba);
		return;
	}
	/* shade in: dark at the start, full at the end; shade out: the reverse */
	for (i = 0; i < n; i++)
	{
		float t = ((float)i + 0.5f) / (float)n;

		if (b->flags & FX_BEAM_SHADEOUT)
			t = 1.0f - t;
		rgba[3] = (uint8_t)((float)blend * t + 0.5f);
		if (!rgba[3])
			continue;
		sprite_bind(spr, fr, GPU_FX_ADD, rgba);
		strip((const float (*)[3])pts + i, 1, (const float (*)[3])side + i, s + i, fr->th,
			rgba);
	}
}
