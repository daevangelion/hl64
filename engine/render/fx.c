/* SPDX-License-Identifier: MIT */
/*
 * fx.c -- the effects API (include/hl64/fx.h; docs/specs/render-fx.md 5,
 * dlights.md 1, lightstyles.md 2): the HLSDK TE_* catalogue as direct
 * calls, screen shake / fade / text state, lightstyles and dynamic lights
 * from the game, the effects clock and the render random stream.
 *
 * Written from render-fx.md 5 and the TE_* field descriptions of HLSDK
 * common/const.h (interface); the client effect builders of the old tree
 * (gsfx_cl.c / gsfx_cart.c) are not used. Every effect allocates from a
 * fixed pool (temp entities 48, particles 512, beams 32, decals 96); a
 * full pool drops the rest and counts it. Effect parameters not fixed by
 * the spec (counts, speeds, lives of sparks and debris) are this module's
 * choices, marked "[own]"; they are tuned against retail capture X7.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/settings.h"
#include "hl64/snd.h"
#include "engine/core/sys.h"
#include "engine/render/fx_int.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"

#define NO_TITLE	0xFFFF

fx_state_t	fx_s;

static struct {
	res_t		assets[FX_A_SETS][FX_ASSET_MAX];
	uint8_t		nassets[FX_A_SETS];
	/* shake (fx_view) */
	float		shake_amp, shake_freq;
	double		shake_start, shake_end, shake_next;
	float		shake_dir[3], shake_roll;
	/* fade */
	uint8_t		fade_rgba[4];
	uint8_t		fade_flags, fade_on;
	double		fade_start;
	float		fade_dur, fade_hold;
	/* text */
	uint16_t	title;
	double		title_start;
	/* profile window; frame_us = this frame's effects time so far (sim +
	 * draw), folded into us_max when the next frame starts or the window
	 * is read */
	uint32_t	us_sum, frames, frame_us;
} fx;

/* ---- clock, random stream, frames ------------------------------------------ */

uint32_t fx_rand(void)
{
	uint32_t x = fx_s.rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	fx_s.rng = x;
	return x;
}

float fx_randf(float lo, float hi)
{
	return lo + (hi - lo) * (float)(fx_rand() >> 8) * (1.0f / 16777216.0f);
}

int fx_randi(int lo, int hi)
{
	return hi <= lo ? lo : lo + (int)(fx_rand() % (uint32_t)(hi - lo + 1));
}

void fx_global(const float in[3], float out[3])
{
	const float *g = eng_layer_origin(eng_frame_layer());
	int k;

	for (k = 0; k < 3; k++)
		out[k] = in[k] + g[k];
}

void fx_trace(const float a[3], const float b[3], int type, col_trace_t *tr)
{
	int frame = eng_frame_layer();

	/* frame -1: no layer, the global frame (eng_layer_origin) */
	eng_set_frame_layer(-1);
	col_trace_line(a, b, type, NULL, tr);
	eng_set_frame_layer(frame);
}

float fx_gravity(void)
{
	return settings.gravity;
}

int fx_cam_pos(const float g[3], int16_t out[3])
{
	int k;

	for (k = 0; k < 3; k++)
	{
		float v = (g[k] - fx_s.eye[k]) * GPU_FX_POS_SCALE;

		if (v < -32767.0f || v > 32767.0f)
			return 0;
		out[k] = (int16_t)lrintf(v);
	}
	return 1;
}

void fx_assets(int set, const res_t *ids, int n)
{
	int i;

	if (set < 0 || set >= FX_A_SETS)
		return;
	if (n > FX_ASSET_MAX)
		n = FX_ASSET_MAX;
	for (i = 0; i < n; i++)
		fx.assets[set][i] = ids[i];
	fx.nassets[set] = (uint8_t)(n < 0 ? 0 : n);
}

static res_t asset(int set, int pick)
{
	int n = fx.nassets[set];

	if (!n)
		return RES_NONE;
	return fx.assets[set][pick < 0 ? fx_randi(0, n - 1) : pick % n];
}

void fx_play(int set, const float org_g[3], float vol, float attn)
{
	res_t s = asset(set, -1);
	int frame;

	if (!s)
		return;
	frame = eng_frame_layer();
	eng_set_frame_layer(-1);
	snd_ambient(org_g, s, vol, attn, 0, 95 + fx_randi(0, 10));
	eng_set_frame_layer(frame);
}

/* ---- temp-entity effects ------------------------------------------------------ */

static fx_tent_t *temp_sprite(const float org_g[3], res_t spr, int mode, int amt, float scale,
	float life)
{
	fx_tent_t *t;

	if (!spr || !(t = tent_alloc(org_g, spr)))
		return NULL;
	t->rendermode = (uint8_t)mode;
	t->amt = (uint8_t)amt;
	t->scale = scale;
	t->die = fx_s.now + life;
	t->color[0] = t->color[1] = t->color[2] = 255;
	return t;
}

/* An animated sprite that plays its frames once at framerate. */
static fx_tent_t *anim_sprite(const float org_g[3], res_t spr, int mode, int amt, float scale,
	float framerate)
{
	int frames = res_model_frames(spr);
	fx_tent_t *t;

	if (framerate <= 0)
		framerate = 10;
	t = temp_sprite(org_g, spr, mode, amt, scale, (float)frames / framerate);
	if (t)
	{
		t->framerate = framerate;
		t->flags |= TF_ANIMATE;
	}
	return t;
}

/* Spark streaks: n tracer particles with gravity [own: 120-260 u/s, 0.2-0.5 s]. */
static void spark_streaks(const float g[3], int n)
{
	static const uint8_t c[3] = { 255, 224, 128 };
	int i;

	for (i = 0; i < n; i++)
	{
		float v[3];

		v[0] = fx_randf(-1, 1);
		v[1] = fx_randf(-1, 1);
		v[2] = fx_randf(-0.5f, 1);
		{
			float s = fx_randf(120, 260) / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + 1e-6f);

			v[0] *= s;
			v[1] *= s;
			v[2] *= s;
		}
		part_add_rgb(g, v, c, PT_TRACER_GRAV, fx_randf(0.2f, 0.5f));
	}
}

void fx_sparks(const float org[3])
{
	float g[3];
	fx_tent_t *t;

	fx_global(org, g);
	spark_streaks(g, 8);
	t = temp_sprite(g, asset(FX_A_RICOCHET_SPR, 0), FX_RM_TRANSADD, 255, fx_randf(0.5f, 1.0f),
		0.1f);
	if (t)
		t->ang[2] = (float)(45 * fx_randi(0, 7));
	fx_play(FX_A_RICOCHET_SND, g, 0.7f, ATTN_NORM);
}

void fx_ricochet(const float org[3], float scale)
{
	float g[3];
	fx_tent_t *t;

	fx_global(org, g);
	t = temp_sprite(g, asset(FX_A_RICOCHET_SPR, 0), FX_RM_GLOW, 200, scale, 0.1f);
	if (t)
	{
		t->renderfx = FX_RFX_NO_DISSIPATION;
		t->ang[2] = (float)(45 * fx_randi(0, 7));
	}
	fx_play(FX_A_RICOCHET_SND, g, 0.7f, ATTN_NORM);
}

void fx_gunshot(const float org[3])
{
	static const uint8_t dust[3] = { 160, 150, 130 };
	float g[3];
	int i;

	fx_global(org, g);
	/* particle puff [own: 6 grey points, 0.3-0.6 s] + ricochet sound */
	for (i = 0; i < 6; i++)
	{
		float v[3];

		/* draws in order: initializer side effects are unsequenced (K10) */
		v[0] = fx_randf(-40, 40);
		v[1] = fx_randf(-40, 40);
		v[2] = fx_randf(0, 60);

		part_add_rgb(g, v, dust, PT_GRAV, fx_randf(0.3f, 0.6f));
	}
	spark_streaks(g, 2);
	fx_play(FX_A_RICOCHET_SND, g, 0.5f, ATTN_NORM);
}

void fx_explosion(const float org[3], res_t sprite, int scale, int framerate, int flags)
{
	static const uint8_t fire[3] = { 255, 160, 48 };
	float g[3];
	fx_tent_t *t;
	int i;

	fx_global(org, g);
	if (scale > 0)
	{
		t = anim_sprite(g, sprite, (flags & FX_EXPL_NOADDITIVE) ? FX_RM_NORMAL : FX_RM_TRANSADD,
			255, (float)scale * 0.1f, (float)framerate);
		if (t)
			t->vel[2] = 8;		/* HLSDK: "move vertically 8 pps" */
	}
	if (!(flags & FX_EXPL_NODLIGHTS))
	{
		/* [own] bright core then a dimmer, longer glow */
		fx_dlight(org, 25, 255, 192, 96, 2, 60);
		fx_dlight(org, 15, 255, 128, 32, 5, 20);
	}
	if (!(flags & FX_EXPL_NOPARTICLES))
		for (i = 0; i < 16; i++)
		{
			float v[3];

			v[0] = fx_randf(-120, 120);	/* in order (K10) */
			v[1] = fx_randf(-120, 120);
			v[2] = fx_randf(0, 200);

			part_add_rgb(g, v, fire, PT_FADE, fx_randf(0.4f, 0.9f));
		}
	if (!(flags & FX_EXPL_NOSOUND))
		fx_play(FX_A_EXPLODE_SND, g, 1.0f, 0.3f);
}

void fx_smoke(const float org[3], res_t sprite, int scale, int framerate)
{
	float g[3];
	fx_tent_t *t;

	fx_global(org, g);
	/* HLSDK: "alphablend sprite, move vertically 30 pps"; grey tint
	 * (TransTexture tints, render-fx.md 2) */
	t = anim_sprite(g, sprite, FX_RM_TRANSTEXTURE, 200, (float)scale * 0.1f, (float)framerate);
	if (t)
	{
		t->vel[2] = 30;
		t->color[0] = t->color[1] = t->color[2] = (uint8_t)fx_randi(80, 120);
	}
}

void fx_tracer(const float start[3], const float end[3])
{
	static const uint8_t c[3] = { 255, 240, 160 };
	float a[3], b[3], d[3], len;
	int k;

	fx_global(start, a);
	fx_global(end, b);
	for (k = 0; k < 3; k++)
		d[k] = b[k] - a[k];
	len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len < 1)
		return;
	/* [own] 6000 u/s streak, alive until it reaches the end */
	for (k = 0; k < 3; k++)
		d[k] *= 6000.0f / len;
	part_add_rgb(a, d, c, PT_TRACER, len / 6000.0f);
}

void fx_sprite(const float org[3], res_t sprite, int scale, int brightness)
{
	float g[3];

	fx_global(org, g);
	/* HLSDK: "additive sprite, plays 1 cycle" [own: 10 fps] */
	anim_sprite(g, sprite, FX_RM_TRANSADD, brightness, (float)scale * 0.1f, 10);
}

void fx_glow_sprite(const float org[3], res_t sprite, int life, int scale, int brightness)
{
	float g[3];
	fx_tent_t *t;

	fx_global(org, g);
	t = temp_sprite(g, sprite, FX_RM_GLOW, brightness, (float)scale * 0.1f, (float)life * 0.1f);
	if (t)
	{
		t->renderfx = FX_RFX_NO_DISSIPATION;
		t->flags |= TF_FADEOUT;
	}
}

static void palette_rgb(int index, uint8_t rgb[3])
{
	part_color((uint8_t)index, rgb);
}

void fx_blood_sprite(const float org[3], res_t spray, res_t drop, int color, int scale)
{
	float g[3];
	uint8_t rgb[3];
	fx_tent_t *t;
	int i, n;

	fx_global(org, g);
	palette_rgb(color, rgb);
	/* the spray animates over 0.25 s (render-fx.md 5); the blood colour
	 * is a tint, so TransTexture (TransColor sprites ignore rendercolor) */
	t = anim_sprite(g, spray, FX_RM_TRANSTEXTURE, 255, (float)scale * 0.1f,
		(float)res_model_frames(spray) / 0.25f);
	if (t)
	{
		memcpy(t->color, rgb, 3);
		t->ang[2] = fx_randf(0, 360);
	}
	/* up to 5 falling drops [own: count 1 + scale / 4] */
	n = 1 + scale / 4;
	if (n > 5)
		n = 5;
	for (i = 0; i < n; i++)
	{
		t = temp_sprite(g, drop, FX_RM_TRANSTEXTURE, 255, (float)scale * 0.05f + 0.2f,
			fx_randf(1.0f, 2.0f));
		if (!t)
			break;
		memcpy(t->color, rgb, 3);
		t->vel[0] = fx_randf(-96, 96);
		t->vel[1] = fx_randf(-96, 96);
		t->vel[2] = fx_randf(-32, 96);
		t->frame = (float)fx_randi(0, res_model_frames(drop) - 1);
		t->flags |= TF_GRAVITY | TF_COLLIDE;
	}
}

void fx_blood_stream(const float org[3], const float dir[3], int color, int speed)
{
	float g[3], d[3], len;
	uint8_t rgb[3];
	int i, n, k;

	fx_global(org, g);
	palette_rgb(color, rgb);
	len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	for (k = 0; k < 3; k++)
		d[k] = len > 0 ? dir[k] / len : 0;
	/* [own] speed / 4 droplets, spread 15 %, 1-2 s */
	n = speed / 4;
	if (n < 4)
		n = 4;
	if (n > 48)
		n = 48;
	for (i = 0; i < n; i++)
	{
		float s = (float)speed * fx_randf(0.4f, 1.0f), v[3];
		uint8_t c[3];

		for (k = 0; k < 3; k++)
		{
			v[k] = (d[k] + fx_randf(-0.15f, 0.15f)) * s;
			c[k] = (uint8_t)(rgb[k] * fx_randi(200, 255) / 255);
		}
		if (!part_add_rgb(g, v, c, PT_GRAV, fx_randf(1.0f, 2.0f)))
			break;
	}
}

static int break_sound(int flags)
{
	if (flags & FX_BREAK_GLASS)
		return FX_A_GLASS_SND + 1;
	if (flags & FX_BREAK_METAL)
		return FX_A_METAL_SND + 1;
	if (flags & FX_BREAK_FLESH)
		return FX_A_FLESH_SND + 1;
	if (flags & FX_BREAK_WOOD)
		return FX_A_WOOD_SND + 1;
	if (flags & FX_BREAK_CONCRETE)
		return FX_A_CONCRETE_SND + 1;
	return 0;
}

void fx_break_model(const float org[3], const float size[3], const float vel[3], int rand_vel,
	res_t model, int count, int life, int flags)
{
	float g[3];
	int i, k, bodies = res_model_frames(model);

	fx_global(org, g);
	if (count <= 0)
	{
		/* [own] one piece per 24^3 u of volume, at least 2 */
		count = (int)(size[0] * size[1] * size[2] / (24.0f * 24 * 24));
		if (count < 2)
			count = 2;
	}
	/* [own] a break never takes more than a third of the pool */
	if (count > HL64_FX_TEMPS / 3)
		count = HL64_FX_TEMPS / 3;
	for (i = 0; i < count; i++)
	{
		float p[3];
		fx_tent_t *t;

		for (k = 0; k < 3; k++)
			p[k] = g[k] + fx_randf(-0.5f, 0.5f) * size[k];
		if (!(t = tent_alloc(p, model)))
			break;
		for (k = 0; k < 3; k++)
		{
			t->vel[k] = vel[k] + fx_randf(-1, 1) * (float)rand_vel;
			t->avel[k] = fx_randf(-256, 256);
			t->ang[k] = fx_randf(0, 360);
		}
		t->body = (uint8_t)fx_randi(0, bodies > 0 ? bodies - 1 : 0);
		t->die = fx_s.now + (double)life * 0.1 + fx_randf(0, 1);
		t->flags |= TF_GRAVITY | TF_COLLIDE | TF_ROTATE | TF_FADEOUT;
		if (flags & FX_BREAK_SMOKE)
			t->flags |= TF_SMOKE;
		t->bounce = (uint8_t)break_sound(flags);
		t->rendermode = FX_RM_NORMAL;
		t->amt = 255;
		if (flags & (FX_BREAK_GLASS | FX_BREAK_TRANS))
		{
			t->rendermode = FX_RM_TRANSTEXTURE;
			t->amt = 128;
		}
	}
}

void fx_temp_model(const float org[3], const float vel[3], float yaw, res_t model,
	int bounce_sound, int life)
{
	float g[3];
	fx_tent_t *t;
	int k;

	fx_global(org, g);
	if (!(t = tent_alloc(g, model)))
		return;
	for (k = 0; k < 3; k++)
	{
		t->vel[k] = vel[k];
		t->avel[k] = fx_randf(-255, 255);
	}
	t->ang[1] = yaw;
	t->die = fx_s.now + (double)life * 0.1;
	t->flags |= TF_GRAVITY | TF_COLLIDE | TF_ROTATE;
	t->bounce = (uint8_t)(bounce_sound == 1 ? FX_A_SHELL_SND + 1
		: bounce_sound == 2 ? FX_A_SHOTSHELL_SND + 1 : 0);
	t->rendermode = FX_RM_NORMAL;
	t->amt = 255;
}

void fx_particles(const float org[3], const float dir[3], int color, int count)
{
	float g[3];
	uint8_t rgb[3];
	int i, k;

	fx_global(org, g);
	palette_rgb(color, rgb);
	for (i = 0; i < count; i++)
	{
		float v[3];

		for (k = 0; k < 3; k++)
			v[k] = dir[k] * 15.0f + fx_randf(-16, 16);
		if (!part_add_rgb(g, v, rgb, PT_SLOWGRAV, fx_randf(0.5f, 1.5f)))
			break;
	}
}

void fx_implosion(const float org[3], int radius, int count, int life)
{
	static const uint8_t c[3] = { 255, 255, 192 };
	float g[3], t = (float)life * 0.1f;
	int i, k;

	if (t <= 0)
		return;
	fx_global(org, g);
	for (i = 0; i < count; i++)
	{
		float d[3], p[3], v[3], len;

		d[0] = fx_randf(-1, 1);
		d[1] = fx_randf(-1, 1);
		d[2] = fx_randf(-1, 1);
		len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
		for (k = 0; k < 3; k++)
		{
			p[k] = g[k] + d[k] / len * (float)radius;
			v[k] = (g[k] - p[k]) / t;
		}
		if (!part_add_rgb(p, v, c, PT_TRACER, t))
			break;
	}
}

/* TE_FIZZ bubbles, [EST] values (render-fx.md 5: the client side is not
 * in the HLSDK): at most 16 a call, rising 30-50 u/s, scale 0.3-0.6,
 * alive until the water surface over them (liquid at the spawn point) or
 * the solid above them (a NOMONSTERS trace, 1024 u at most). */
#define FIZZ_MAX	16

static int liquid(int c)
{
	return c == COL_WATER || c == COL_SLIME || c == COL_LAVA;
}

void fx_fizz(int e, res_t sprite, int density)
{
	const col_ent_t *cv;
	const ent_rnd_t *r;
	float lo[3], hi[3], cur, yaw;
	int n, i, frame = eng_frame_layer();

	if (e < 0 || !ent_is_live(e) || !sprite)
		return;
	cv = ent_cv(e);
	r = ent_rnd(e);
	fx_global(cv->absmin, lo);
	fx_global(cv->absmax, hi);
	/* HLSDK CBubbling::Spawn's speed hack */
	cur = (float)((int)r->rendercolor[0] << 8 | r->rendercolor[1]);
	if (r->rendercolor[2])
		cur = -cur;
	yaw = cv->angles[1] * (3.14159265f / 180.0f);
	n = density < 0 || density >= FIZZ_MAX ? FIZZ_MAX : density + 1;
	eng_set_frame_layer(-1);
	for (i = 0; i < n; i++)
	{
		float o[3], up[3], top, rise;
		col_trace_t tr;
		fx_tent_t *t;
		int k;

		o[0] = fx_randf(lo[0], hi[0]);
		o[1] = fx_randf(lo[1], hi[1]);
		o[2] = fx_randf(lo[2], hi[2]);
		up[0] = o[0];
		up[1] = o[1];
		up[2] = o[2] + 1024.0f;
		col_trace_line(o, up, COL_MOVE_NOMONSTERS, NULL, &tr);
		if (tr.startsolid)
			continue;
		top = tr.end[2];
		if (liquid(col_contents(o)))
		{
			float wet = o[2], dry = top;

			/* the surface between: 6 halvings */
			for (k = 0; k < 6; k++)
			{
				float m[3] = { o[0], o[1], 0.5f * (wet + dry) };

				if (liquid(col_contents(m)))
					wet = m[2];
				else
					dry = m[2];
			}
			top = wet;
		}
		rise = fx_randf(30, 50);
		if (top - o[2] < 1.0f || !(t = tent_alloc(o, sprite)))
			continue;
		t->rendermode = FX_RM_TRANSALPHA;
		t->amt = 255;
		t->scale = fx_randf(0.3f, 0.6f);
		t->vel[0] = cur * cosf(yaw);
		t->vel[1] = cur * sinf(yaw);
		t->vel[2] = rise;
		t->frame = (float)fx_randi(0, res_model_frames(sprite) > 1
			? res_model_frames(sprite) - 1 : 0);
		t->die = fx_s.now + (double)((top - o[2]) / rise);
	}
	eng_set_frame_layer(frame);
}

void fx_beam_cylinder(const float org[3], float radius, res_t sprite, int life, int width,
	int noise, int r, int g, int b, int bright)
{
	float o[3];
	uint8_t rgb[3] = { (uint8_t)r, (uint8_t)g, (uint8_t)b };

	fx_global(org, o);
	beam_ring(o, radius, sprite, (float)life * 0.1f, width, noise, rgb, bright);
}

/* ---- lights --------------------------------------------------------------------- */

void fx_dlight(const float org[3], int radius, int r, int g, int b, int life, int decay)
{
	ren_dlight_t *l = ren_dlight_alloc(0, fx_s.now);

	fx_global(org, l->origin);
	l->radius = (float)radius * 10.0f;
	l->die = fx_s.now + (double)life * 0.1;
	l->decay = (float)decay * 10.0f;
	l->color[0] = (uint8_t)r;
	l->color[1] = (uint8_t)g;
	l->color[2] = (uint8_t)b;
	fx_s.st.dlights++;
}

void fx_elight(int e, const float org[3], float radius, int r, int g, int b, int life, float decay)
{
	ren_dlight_t *l = ren_dlight_alloc(e > 0 ? e : 0, fx_s.now);

	fx_global(org, l->origin);
	l->radius = radius;
	l->die = fx_s.now + (double)life * 0.1;
	l->decay = decay;
	l->color[0] = (uint8_t)r;
	l->color[1] = (uint8_t)g;
	l->color[2] = (uint8_t)b;
	l->world = 0;		/* models only (dlights.md 1) */
	l->entity = (uint16_t)(e > 0 ? e : 0);
	fx_s.st.dlights++;
}

void fx_muzzle_flash(int e, int attachment, int type)
{
	float p[3], g[3], f[3], r[3], u[3];
	fx_tent_t *t;
	ren_dlight_t *l;
	int k;

	if (!ent_is_live(e))
		return;
	/* the attachment (game/studio) in e's frame, else origin + 18 forward
	 * at eye height (dlights.md 1 EF_MUZZLEFLASH placement) */
	if (!game_attachment(e, attachment, p))
	{
		ren_angle_axes(ent_cv(e)->angles, f, r, u);
		for (k = 0; k < 3; k++)
			p[k] = ent_cv(e)->origin[k] + 18.0f * f[k];
		p[2] += 16;
	}
	{
		const float *gm = eng_layer_origin(ent_idc(e)->layer);

		for (k = 0; k < 3; k++)
			g[k] = p[k] + gm[k];
	}
	t = temp_sprite(g, asset(FX_A_MUZZLE_SPR, type < 0 ? 0 : type), FX_RM_TRANSADD, 255,
		fx_randf(0.4f, 0.6f), 0.05f);
	if (t)
		t->ang[2] = fx_randf(0, 360);
	/* EF_MUZZLEFLASH light: 200 + rand32, minlight 32, 0.1 s (dlights.md 1) */
	l = ren_dlight_alloc(e, fx_s.now);
	memcpy(l->origin, g, sizeof(g));
	l->radius = 200.0f + (float)(fx_rand() & 31);
	l->minlight = 32;
	l->die = fx_s.now + 0.1;
	l->color[0] = 255;
	l->color[1] = 224;
	l->color[2] = 160;
	fx_s.st.dlights++;
}

void fx_lightstyle(int style, uint16_t pattern)
{
	const uint8_t *letters;
	int len = 0, slot;

	if (style < 0 || style > 63)
		return;
	if (style < 32)
		slot = style;
	else
	{
		const wld_layer_t *L = wld_layer(eng_frame_layer());

		if (!L)
			return;
		slot = wld_style_slot(L, style);
	}
	letters = res_pattern(pattern, &len);
	if (!letters)
		len = 0;
	wld_style_set(slot, letters, len);
}

/* ---- screen: shake, fade, text ------------------------------------------------- */

void fx_shake(const float org[3], float amplitude, float frequency, float duration, float radius)
{
	float amp = 0;

	if (!ent_is_live(1) || !(ent_cv(1)->flags & FL_ONGROUND))
		return;		/* HLSDK UTIL_ScreenShake: players on the ground only */
	if (radius <= 0)
		amp = amplitude;
	else
	{
		float g[3], d = 0;
		const float *gm = eng_layer_origin(ent_idc(1)->layer);
		int k;

		fx_global(org, g);
		for (k = 0; k < 3; k++)
		{
			float v = g[k] - (ent_cv(1)->origin[k] + gm[k]);

			d += v * v;
		}
		if (d < radius * radius)
			amp = amplitude;
	}
	if (amp <= 0 || duration <= 0)
		return;
	/* a weaker shake does not cut a stronger one short */
	if (fx_s.now < fx.shake_end && fx.shake_amp
		* (float)((fx.shake_end - fx_s.now) / (fx.shake_end - fx.shake_start)) > amp)
		return;
	fx.shake_amp = amp > 16 ? 16 : amp;	/* HLSDK amplitude range 0-16 */
	fx.shake_freq = frequency > 0 ? frequency : 1;
	fx.shake_start = fx_s.now;
	fx.shake_end = fx_s.now + duration;
	fx.shake_next = fx_s.now;
}

void fx_fade(int r, int g, int b, int a, float duration, float hold, int flags)
{
	fx.fade_rgba[0] = (uint8_t)r;
	fx.fade_rgba[1] = (uint8_t)g;
	fx.fade_rgba[2] = (uint8_t)b;
	fx.fade_rgba[3] = (uint8_t)a;
	fx.fade_flags = (uint8_t)flags;
	fx.fade_start = fx_s.now;
	fx.fade_dur = duration;
	fx.fade_hold = hold;
	fx.fade_on = 1;
}

void fx_text(uint16_t title)
{
	fx.title = title;
	fx.title_start = fx_s.now;
}

static void shake_step(void)
{
	if (fx_s.now >= fx.shake_end)
	{
		fx.shake_amp = 0;
		return;
	}
	/* a new random direction at the shake frequency */
	if (fx_s.now >= fx.shake_next)
	{
		int k;

		for (k = 0; k < 3; k++)
			fx.shake_dir[k] = fx_randf(-1, 1);
		fx.shake_roll = fx_randf(-1, 1);
		fx.shake_next = fx_s.now + 1.0 / fx.shake_freq;
	}
}

/* Fade alpha now (FFADE_OUT: up to a over duration, hold, then off unless
 * STAYOUT; FFADE_IN: a for hold, then down to 0 over duration). */
static int fade_alpha(void)
{
	float t = (float)(fx_s.now - fx.fade_start), a = fx.fade_rgba[3], v;

	if (!fx.fade_on)
		return 0;
	if (fx.fade_flags & FX_FADE_OUT)
	{
		if (t < fx.fade_dur)
			v = a * t / fx.fade_dur;
		else if ((fx.fade_flags & FX_FADE_STAYOUT) || t < fx.fade_dur + fx.fade_hold)
			v = a;
		else
		{
			fx.fade_on = 0;
			v = 0;
		}
	}
	else
	{
		if (t < fx.fade_hold)
			v = a;
		else if (t < fx.fade_hold + fx.fade_dur)
			v = a * (1.0f - (t - fx.fade_hold) / fx.fade_dur);
		else
		{
			fx.fade_on = 0;
			v = 0;
		}
	}
	return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f);
}

void fx_view(float org_ofs[3], float ang_ofs[3], uint8_t fade_rgba[4])
{
	int k;

	for (k = 0; k < 3; k++)
		org_ofs[k] = ang_ofs[k] = 0;
	if (fx.shake_amp > 0 && fx_s.now < fx.shake_end)
	{
		float left = (float)((fx.shake_end - fx_s.now) / (fx.shake_end - fx.shake_start));
		float a = fx.shake_amp * left;

		for (k = 0; k < 3; k++)
			org_ofs[k] = fx.shake_dir[k] * a;
		ang_ofs[2] = fx.shake_roll * a * 0.25f;
	}
	memcpy(fade_rgba, fx.fade_rgba, 3);
	fade_rgba[3] = (uint8_t)fade_alpha();
}

uint16_t fx_text_current(float *time)
{
	const res_title_t *t;
	float s, end;

	*time = 0;
	if (fx.title == NO_TITLE)
		return NO_TITLE;
	s = (float)(fx_s.now - fx.title_start);
	t = res_title(fx.title);
	/* titles.txt: fade in, hold, fade out; under effect 2 (write out)
	 * the fade-in time is per character and the hold starts once the text
	 * has scanned out. Without a table entry: 2 s [own]. */
	end = !t ? 2.0f : (t->effect == 2 ? t->fadein * t->text_len : t->fadein) + t->holdtime
		+ t->fadeout;
	if (s > end)
	{
		fx.title = NO_TITLE;
		return NO_TITLE;
	}
	*time = s;
	return fx.title;
}

/* ---- engine side ------------------------------------------------------------------- */

/* Every effect ends; the game's assets (fx_assets, registered once by
 * game_init) stay: a disconnect or a load keeps the same game. */
void fx_init(void)
{
	static res_t assets[FX_A_SETS][FX_ASSET_MAX];
	static uint8_t nassets[FX_A_SETS];

	memcpy(assets, fx.assets, sizeof(assets));
	memcpy(nassets, fx.nassets, sizeof(nassets));
	memset(&fx_s, 0, sizeof(fx_s));
	memset(&fx, 0, sizeof(fx));
	memcpy(fx.assets, assets, sizeof(assets));
	memcpy(fx.nassets, nassets, sizeof(nassets));
	fx_s.rng = 0x2545F491u;
	fx.title = NO_TITLE;
	part_init();
	tent_init();
	beam_init();
	sprite_init();
	decal_init();
}

void fx_frame(double time, double dt)
{
	uint64_t t0 = sys_time_us();
	uint32_t us;

	fx_s.now = time;
	fx_s.dt = (float)dt;
	fx_s.frame++;
	trans_frame(dt);
	tent_frame();
	part_frame();
	beam_frame();
	decal_frame();
	sprite_frame();
	shake_step();
	if (tent_live() > (int)fx_s.st.temps_peak)
		fx_s.st.temps_peak = (uint32_t)tent_live();
	if (part_live() > (int)fx_s.st.particles_peak)
		fx_s.st.particles_peak = (uint32_t)part_live();
	if (beam_live() > (int)fx_s.st.beams_peak)
		fx_s.st.beams_peak = (uint32_t)beam_live();
	us = (uint32_t)(sys_time_us() - t0);
	/* the previous frame (its sim and its draw) is complete */
	if (fx.frame_us > fx_s.st.us_max)
		fx_s.st.us_max = fx.frame_us;
	fx.frame_us = us;
	fx_s.st.us += us;
	fx_s.st.sim_us += us;
	fx.frames++;
}

/* Draw-side time, added by the renderer (fx_draw_*) to the frame's. */
void fx_add_us(int part, uint32_t us)
{
	fx_s.st.us += us;
	fx.frame_us += us;
	if (part == FX_US_PART)
		fx_s.st.part_us += us;
	else if (part == FX_US_TRANS)
		fx_s.st.trans_us += us;
	else
		fx_s.st.decal_us += us;
}

void fx_stats(fx_stats_t *s, int reset)
{
	decal_stats_t d;

	decal_stats(&d, 0);
	/* read after the frame's draw (the roll-up): the frame counts whole */
	if (fx.frame_us > fx_s.st.us_max)
		fx_s.st.us_max = fx.frame_us;
	*s = fx_s.st;
	s->temps = (uint32_t)tent_live();
	s->decals = d.live;
	s->decal_drops = d.dropped;
	s->frames = fx.frames;
	if (reset)
	{
		memset(&fx_s.st, 0, sizeof(fx_s.st));
		fx.frames = 0;
		fx.frame_us = 0;	/* its time is in the window just read */
	}
}
