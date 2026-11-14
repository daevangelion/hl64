/* SPDX-License-Identifier: MIT */
/*
 * trans.c -- render modes, blend values and the translucent pass
 * (docs/specs/render-fx.md 1-3, docs/architecture.md 9.6), written from
 * the spec and HLSDK common/const.h's kRender* / kRenderFx* values. The
 * old tree's r64_trans.c (changed after the provenance audit) was not
 * opened.
 *
 * Blend (render-fx.md 1): renderamt, shaped per renderfx on the game clock;
 * fades 5-8 change renderamt itself at 30 / 120 per second. Pulse, strobe,
 * flicker, distort and hologram shapes are this module's own within the
 * spec's figures (periods, amplitudes, the 100-400 u hologram fade) until
 * retail capture X-fx calibrates them.
 *
 * Pass: sprite entities, temp-entity sprites, beams, translucent brush
 * movers (world.c hands them over) and translucent studio entities
 * (studio_draw hands them over) go into one list per frame; the nearest
 * HL64_FX_TRANS_MAX are kept, drawn back to front with depth test and no
 * depth write. Cutout sprites (Normal / TransAlpha) draw first, unsorted.
 * Additive items cap their blend at HL64_FX_ADD_CAP (the RDP adder wraps).
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/limits.h"
#include "hl64/seam.h"
#include "hl64/studio.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/render/fx_int.h"
#include "engine/render/gpu_studio.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/world/wld.h"

#define PI2		6.2831853f
#define GLOW_REF	200.0f	/* glow: drawn as large as at this distance [own] */

/* The RDP state per effects mode (X1); both platforms apply this table. */
const gpu_fx_rdp_t gpu_fx_rdp[GPU_FX_MODES] = {
	/* blender            combiner              zt zw zd ac tex */
	[GPU_FX_CUTOUT]      = { GPU_FX_BL_ALPHA, GPU_FX_CC_TEX_SHADE,  1, 1, 0, 1, 1, 0 },
	[GPU_FX_BLEND]       = { GPU_FX_BL_ALPHA, GPU_FX_CC_TEX_SHADE,  1, 0, 0, 0, 1, 0 },
	[GPU_FX_ADD]         = { GPU_FX_BL_ADD,   GPU_FX_CC_TEX_SHADE,  1, 0, 0, 0, 1, 0 },
	[GPU_FX_GLOW]        = { GPU_FX_BL_ADD,   GPU_FX_CC_TEX_SHADE,  0, 0, 0, 0, 1, 0 },
	[GPU_FX_DECAL]       = { GPU_FX_BL_ALPHA, GPU_FX_CC_SHADE_TEXA, 1, 0, 1, 0, 2, 0 },
	[GPU_FX_POINT]       = { GPU_FX_BL_NONE,  GPU_FX_CC_PRIM,       1, 1, 0, 0, 0, 0 },
	[GPU_FX_STREAK]      = { GPU_FX_BL_ADD,   GPU_FX_CC_SHADE,      1, 0, 0, 0, 0, 0 },
	[GPU_FX_BRUSH_BLEND] = { GPU_FX_BL_ALPHA, GPU_FX_CC_TEX_PRIM,   1, 0, 0, 0, 3, 0 },
	[GPU_FX_BRUSH_ADD]   = { GPU_FX_BL_ADD,   GPU_FX_CC_TEX_PRIM,   1, 0, 0, 0, 3, 0 },
	[GPU_FX_BRUSH_COLOR] = { GPU_FX_BL_ALPHA, GPU_FX_CC_PRIM_A,     1, 0, 0, 0, 0, 0 },
	[GPU_FX_SCREEN]      = { GPU_FX_BL_ALPHA, GPU_FX_CC_SHADE,      0, 0, 0, 0, 0, 0 },
};

const char *const gpu_fx_mode_name[GPU_FX_MODES] = {
	"cutout", "blend", "add", "glow", "decal", "point", "streak", "brush_blend", "brush_add",
	"brush_color", "screen"
};

typedef struct {
	float		d2;
	uint16_t	id;
	uint8_t		kind, mode, blend, pad[3];
} item_t;

static item_t		items[HL64_FX_TRANS_MAX], cuts[HL64_FX_TRANS_MAX];
static int		nitems, ncuts;
static int32_t		frac[HL64_MAX_ENTITIES];	/* fade remainders, 1/65536 (fx 5-8) */
static fx_draw_stats_t	ds, last;

void	tent_draw(int i, uint8_t blend, int mode);

/* ---- blend (render-fx.md 1) ---------------------------------------------------- */

static int clamp255(float v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f);
}

/* Per-entity phase so equal effects do not beat in step [own]. */
static float phase(int e)
{
	return e < 0 ? 0 : (float)e * 2.3999632f;
}

int trans_blend_amt(int rendermode, int renderfx, int amt, int e, float dist)
{
	float t = (float)fmod(fx_s.now, 3600.0), v;

	(void)rendermode;
	switch (renderfx)
	{
	case FX_RFX_PULSE_SLOW:
	case FX_RFX_PULSE_FAST:
	case FX_RFX_PULSE_SLOW_WIDE:
	case FX_RFX_PULSE_FAST_WIDE:
	{
		/* +-16 narrow / +-64 wide at ~1 Hz slow / ~4 Hz fast */
		float hz = (renderfx == FX_RFX_PULSE_SLOW || renderfx == FX_RFX_PULSE_SLOW_WIDE) ? 1 : 4;
		float a = renderfx >= FX_RFX_PULSE_SLOW_WIDE ? 64 : 16;

		return clamp255((float)amt + a * hl_sinf(PI2 * hz * t + phase(e)));
	}
	case FX_RFX_STROBE_SLOW:
	case FX_RFX_STROBE_FAST:
	case FX_RFX_STROBE_FASTER:
	{
		/* renderamt / 0 alternating at 4 / 8 / 16 changes per second */
		float rate = renderfx == FX_RFX_STROBE_SLOW ? 4 : renderfx == FX_RFX_STROBE_FAST ? 8 : 16;

		return ((long)floorf(t * rate + phase(e)) & 1) ? 0 : amt;
	}
	case FX_RFX_FLICKER_SLOW:
	case FX_RFX_FLICKER_FAST:
	{
		/* two incommensurate sines: ~2 Hz slow, ~5 Hz fast */
		float hz = renderfx == FX_RFX_FLICKER_SLOW ? 2.0f : 5.0f;

		v = hl_sinf(PI2 * hz * t + phase(e)) + hl_sinf(PI2 * hz * 1.618034f * t + 2 * phase(e));
		return v > -0.3f ? amt : 0;
	}
	case FX_RFX_DISTORT:
	case FX_RFX_HOLOGRAM:
		/* reduced by a random amount each frame (0..63) [own, X3] */
		v = (float)amt - (float)(fx_rand() & 63);
		if (renderfx == FX_RFX_HOLOGRAM)
		{
			/* full up to 100 u, down to 0 by 400 u */
			if (dist > 400)
				v = 0;
			else if (dist > 100)
				v *= (400 - dist) / 300.0f;
		}
		return clamp255(v);
	default:	/* 0, 5-8 (already applied), 14, 17, 18, 19 */
		return amt;
	}
}

/* An entity's draw origin (global): its layer's, + the seam ghost offset
 * when it shows in the eye's map as one (seam_ghost) */
static void ent_gm(int e, float gm[3])
{
	const float *o = eng_layer_origin(ent_idc(e)->layer);
	float off[3];
	int k, g = seam_ghost(e, fx_s.map, off);

	for (k = 0; k < 3; k++)
		gm[k] = o[k] + (g ? off[k] : 0);
}

int trans_blend(int e)
{
	const col_ent_t *cv = ent_cv(e);
	float gm[3], d = 0;
	int k;

	ent_gm(e, gm);
	for (k = 0; k < 3; k++)
	{
		float v = cv->origin[k] + gm[k] - fx_s.eye[k];

		d += v * v;
	}
	return trans_blend_amt(cv->rendermode, ent_rnd(e)->renderfx, ent_rnd(e)->renderamt, e,
		sqrtf(d));
}

int ren_blend(int e)
{
	return ent_is_live(e) ? trans_blend(e) : 0;
}

void trans_frame(double dt)
{
	int e;

	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
	{
		ent_rnd_t *r;
		int32_t rate, step;
		int amt;

		r = ent_rnd(e);
		switch (r->renderfx)
		{
		case FX_RFX_FADE_SLOW:	rate = -30; break;
		case FX_RFX_FADE_FAST:	rate = -120; break;
		case FX_RFX_SOLID_SLOW:	rate = 30; break;
		case FX_RFX_SOLID_FAST:	rate = 120; break;
		default:
			frac[e] = 0;
			continue;
		}
		/* exact per-second rates in 16.16 fixed point, the remainder
		 * kept between frames */
		step = (int32_t)lrint((double)rate * dt * 65536.0) + frac[e];
		amt = r->renderamt + step / 65536;
		frac[e] = step % 65536;
		/* at the bound it moves to: stay (the remainder dropped) */
		if (amt <= 0 && rate < 0)
		{
			amt = 0;
			frac[e] = 0;
		}
		if (amt >= 255 && rate > 0)
		{
			amt = 255;
			frac[e] = 0;
		}
		r->renderamt = (uint8_t)amt;
	}
}

/* ---- render mode -> GPU mode (render-fx.md 2, X1) ---------------------------------- */

int trans_gpu_mode(int rendermode, int kind, uint8_t *blend)
{
	int mode;

	if (kind == 1)		/* studio */
	{
		switch (rendermode)
		{
		case FX_RM_TRANSCOLOR:
		case FX_RM_TRANSTEXTURE:
			return GPU_STUDIO_BLEND;
		case FX_RM_GLOW:
		case FX_RM_TRANSADD:
			if (*blend > HL64_FX_ADD_CAP)
				*blend = HL64_FX_ADD_CAP;
			return GPU_STUDIO_ADD;
		default:
			return -1;	/* opaque studio pass */
		}
	}
	if (kind == 2)		/* brush */
	{
		switch (rendermode)
		{
		case FX_RM_TRANSCOLOR:
			return GPU_FX_BRUSH_COLOR;
		case FX_RM_TRANSTEXTURE:
			return GPU_FX_BRUSH_BLEND;
		case FX_RM_GLOW:
		case FX_RM_TRANSADD:
			if (*blend > HL64_FX_ADD_CAP)
				*blend = HL64_FX_ADD_CAP;
			return GPU_FX_BRUSH_ADD;
		default:
			return -1;	/* opaque world pass (TransAlpha: alpha compare) */
		}
	}
	switch (rendermode)	/* sprite */
	{
	case FX_RM_TRANSCOLOR:
	case FX_RM_TRANSTEXTURE:
		mode = GPU_FX_BLEND;
		break;
	case FX_RM_GLOW:
		mode = GPU_FX_GLOW;
		break;
	case FX_RM_TRANSADD:
		mode = GPU_FX_ADD;
		break;
	default:		/* Normal (blend forced 255), TransAlpha */
		if (rendermode == FX_RM_NORMAL)
			*blend = 255;
		return GPU_FX_CUTOUT;
	}
	if ((mode == GPU_FX_ADD || mode == GPU_FX_GLOW) && *blend > HL64_FX_ADD_CAP)
		*blend = HL64_FX_ADD_CAP;
	return mode;
}

/* ---- list ---------------------------------------------------------------------------- */

void trans_add(int kind, int id, float d2, uint8_t blend, uint8_t mode)
{
	item_t *it;
	int i;

	/* cutout sprites (entity or temp; studio temps never come as mode 0) */
	if ((kind == TK_SPRITE_ENT || kind == TK_TEMP) && mode == GPU_FX_CUTOUT)
	{
		if (ncuts >= HL64_FX_TRANS_MAX)
		{
			ds.dropped++;
			return;
		}
		it = &cuts[ncuts++];
	}
	else if (nitems < HL64_FX_TRANS_MAX)
		it = &items[nitems++];
	else
	{
		/* keep the nearest: replace the farthest when this one is nearer */
		int far = 0;

		for (i = 1; i < nitems; i++)
			if (items[i].d2 > items[far].d2)
				far = i;
		ds.dropped++;
		if (items[far].d2 <= d2)
			return;
		it = &items[far];
	}
	it->d2 = d2;
	it->id = (uint16_t)id;
	it->kind = (uint8_t)kind;
	it->mode = mode;
	it->blend = blend;
}

static float ent_dist2(int e, float org[3])
{
	float gm[3], d = 0;
	int k;

	ent_gm(e, gm);
	for (k = 0; k < 3; k++)
	{
		org[k] = ent_cv(e)->origin[k] + gm[k];
		d += (org[k] - fx_s.eye[k]) * (org[k] - fx_s.eye[k]);
	}
	return d;
}

void ren_trans_studio(int e, float dist2)
{
	uint8_t b;
	int mode;

	if (!ent_is_live(e))
		return;
	b = (uint8_t)trans_blend(e);
	mode = trans_gpu_mode(ent_cv(e)->rendermode, 1, &b);
	if (mode < 0)
		return;
	if (!b)
	{
		ds.invisible++;
		return;
	}
	trans_add(TK_STUDIO_ENT, e, dist2, b, (uint8_t)mode);
}

void ren_trans_brush(int e, float dist2)
{
	uint8_t b;
	int mode;

	if (!ent_is_live(e))
		return;
	b = (uint8_t)trans_blend(e);
	mode = trans_gpu_mode(ent_cv(e)->rendermode, 2, &b);
	if (mode < 0)
		return;
	if (!b)
	{
		ds.invisible++;
		return;
	}
	trans_add(TK_BRUSH_ENT, e, dist2, b, (uint8_t)mode);
}

/* Sprite entities of the eye's map (and the seam ghosts it shows) in front
 * of the eye. */
static void list_sprite_ents(void)
{
	int e;

	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
	{
		const ent_rnd_t *r;
		const col_ent_t *cv;
		float org[3], d2, fwd;
		uint8_t b;
		int mode;

		r = ent_rnd(e);
		if (RES_KIND(r->model) != RES_K_SPRITE || (r->effects & EF_NODRAW))
			continue;
		if (!wld_layer(ent_idc(e)->layer) || (wld_layer(ent_idc(e)->layer)->map != fx_s.map
			&& !seam_ghost(e, fx_s.map, org)))
			continue;
		cv = ent_cv(e);
		d2 = ent_dist2(e, org);
		fwd = (org[0] - fx_s.eye[0]) * fx_s.fwd[0] + (org[1] - fx_s.eye[1]) * fx_s.fwd[1]
			+ (org[2] - fx_s.eye[2]) * fx_s.fwd[2];
		if (fwd < -256)
			continue;
		b = (uint8_t)trans_blend(e);
		mode = trans_gpu_mode(cv->rendermode, 0, &b);
		if (!b && cv->rendermode != FX_RM_NORMAL)
		{
			ds.invisible++;
			continue;
		}
		trans_add(TK_SPRITE_ENT, e, d2, b, (uint8_t)mode);
	}
}

/* ---- draw ---------------------------------------------------------------------------- */

static void draw_sprite_ent(int e, uint8_t blend, int mode)
{
	const ent_rnd_t *r = ent_rnd(e);
	const col_ent_t *cv = ent_cv(e);
	const fx_sprite_t *s = sprite_get(r->model);
	float org[3], d, scale = r->scale > 0 ? r->scale : 1;
	uint8_t rgba[4] = { 255, 255, 255, blend };

	if (!s)
		return;
	d = sqrtf(ent_dist2(e, org));
	if (cv->rendermode == FX_RM_TRANSCOLOR)
		memcpy(rgba, r->rendercolor, 3);
	if (cv->rendermode == FX_RM_GLOW)
	{
		col_trace_t tr;

		/* occlusion: a line trace from the eye; blocked = invisible
		 * (render-fx.md 3); constant screen size */
		fx_trace(fx_s.eye, org, COL_MOVE_NOMONSTERS, &tr);
		if (tr.frac < 1.0f)
		{
			ds.glow_occluded++;
			return;
		}
		if (d < 32)
			rgba[3] = (uint8_t)(blend * d / 32.0f);
		scale *= d / GLOW_REF;
	}
	sprite_draw(s, (int)r->frame, org, cv->angles, scale, mode, rgba);
	ds.sprites++;
}

static void draw_item(const item_t *it)
{
	switch (it->kind)
	{
	case TK_SPRITE_ENT:
		draw_sprite_ent(it->id, it->blend, it->mode);
		break;
	case TK_STUDIO_ENT:
		studio_draw_trans(it->id, it->mode, it->blend);
		gpu_fx_state_lost();
		ds.studio++;
		break;
	case TK_BRUSH_ENT:
	{
		const uint8_t *c = ent_rnd(it->id)->rendercolor;
		uint8_t prim[4] = { 255, 255, 255, it->blend };

		if (it->mode == GPU_FX_BRUSH_COLOR)
			memcpy(prim, c, 3);
		ren_mover_draw(it->id, it->mode, prim);
		ds.brushes++;
		break;
	}
	case TK_TEMP:
		tent_draw(it->id, it->blend, it->mode);
		ds.temps++;
		break;
	case TK_BEAM:
		beam_draw(it->id, it->blend);
		ds.beams++;
		break;
	}
}

void fx_draw_begin(void)
{
	float a[3];
	int k;

	nitems = ncuts = 0;
	memset(&ds, 0, sizeof(ds));
	memcpy(fx_s.eye, ren_cam.org, sizeof(fx_s.eye));
	for (k = 0; k < 3; k++)
		a[k] = ren_cam.ang[k];
	ren_angle_axes(a, fx_s.fwd, fx_s.right, fx_s.up);
	fx_s.map = ren_cam.map;
	gpu_fx_begin();
}

void fx_draw_particles(void)
{
	uint64_t t0 = sys_time_us();

	tent_draw_studio();
	gpu_fx_state_lost();
	part_draw();
	fx_add_us(FX_US_PART, (uint32_t)(sys_time_us() - t0));
}

void fx_draw_trans(void)
{
	uint64_t t0 = sys_time_us();
	int i, j;

	list_sprite_ents();
	tent_list_sprites();
	beam_list();
	for (i = 0; i < ncuts; i++)
		draw_item(&cuts[i]);
	/* back to front: insertion sort by distance, farthest first */
	for (i = 1; i < nitems; i++)
	{
		item_t x = items[i];

		for (j = i; j > 0 && items[j - 1].d2 < x.d2; j--)
			items[j] = items[j - 1];
		items[j] = x;
	}
	for (i = 0; i < nitems; i++)
		draw_item(&items[i]);
	ds.items = (uint32_t)nitems;
	ds.cutouts = (uint32_t)ncuts;
	fx_s.st.trans_drawn += (uint32_t)(nitems + ncuts);
	fx_s.st.trans_dropped += ds.dropped;
	ds.us = (uint32_t)(sys_time_us() - t0);
	fx_add_us(FX_US_TRANS, ds.us);
}

void fx_draw_decals(void)
{
	static uint32_t prev_drops;
	uint64_t t0 = sys_time_us();
	gpu_fx_stats_t g;

	decal_draw(&ds.decals, &ds.decal_verts);
	gpu_fx_end();
	/* a full vertex ring drops batches (counted per frame): logged on the
	 * first frame of each run of full frames */
	gpu_fx_stats(&g);
	if (g.drops && !prev_drops)
		log_printf("fx: effects vertex ring full: %lu batches dropped (%lu of %d pairs)\n",
			(unsigned long)g.drops, (unsigned long)g.pairs, GPU_FX_RING_PAIRS);
	prev_drops = g.drops;
	fx_add_us(FX_US_DECAL, (uint32_t)(sys_time_us() - t0));
	last = ds;
}

void fx_draw_stats(fx_draw_stats_t *s)
{
	*s = last;
}

#ifdef HL64_HOST
/* X2: the order the last pass drew (kinds and ids), for the test driver. */
int trans_last_order(uint8_t *kind, uint16_t *id, uint8_t *blend, int max)
{
	int i;

	for (i = 0; i < nitems && i < max; i++)
	{
		kind[i] = items[i].kind;
		id[i] = items[i].id;
		blend[i] = items[i].blend;
	}
	return nitems;
}
#endif
