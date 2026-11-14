/* SPDX-License-Identifier: MIT */
/*
 * fx.c -- host harness for docs/specs/render-fx.md (X1, X2, X3 exact-rate
 * part, X4 synthetic part, X5, X6, ADD: 2a over the cooked sprites),
 * dlights.md (D1, D2, D4 and the model term of 4) and lightstyles.md (L1,
 * L2, the model term of 4) on the real
 * engine/render effects modules, the draw-list recorder and a cooked map
 * (the player start of the campaign's start map); test/m3/fx.py runs it.
 *
 *   hl64-fx --root CONTENT        prints "<test> PASS|FAIL ..." lines
 *
 * Every test is self-contained or synthetic (entities, sprites and decals
 * made here); the retail-capture properties (X3 shapes, X4 screenshots,
 * X7, D5-D7, L5-L7) are not run.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/fx.h"
#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/rec.h"
#include "hl64/settings.h"
#include "hl64/studio.h"
#include "engine/core/eng.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/render/fx_int.h"
#include "engine/render/gpu_fx.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int fails, tfails;
static double now = 1.0;
static wld_layer_t *L;

#define CHECK(cond, ...) do { if (!(cond)) { tfails++; printf("  fail %s:%d: ", __FILE__, \
	__LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void begin(void)
{
	tfails = 0;
}

static void finish(const char *name, const char *note)
{
	printf("%s %s%s%s\n", name, tfails ? "FAIL" : "PASS", note ? " " : "", note ? note : "");
	fails += tfails != 0;
}

static void step(double dt)
{
	now += dt;
	fx_frame(now, dt);
}

static void draw(void)
{
	fx_draw_begin();
	fx_draw_particles();
	fx_draw_trans();
	fx_draw_decals();
}

static void eye(const float g[3], float pitch, float yaw)
{
	memcpy(ren_cam.org, g, sizeof(ren_cam.org));
	ren_cam.ang[0] = pitch;
	ren_cam.ang[1] = yaw;
	ren_cam.ang[2] = 0;
	ren_cam.map = L->map;
}

/* An entity of the start layer at global g. */
static int make_ent(const float g[3])
{
	float o[3];
	int e = ent_create(ENT_ALLOC_GAME), k;

	ent_idc(e)->layer = (int8_t)L->slot;
	for (k = 0; k < 3; k++)
		o[k] = g[k] - L->gofs[k];
	eng_set_frame_layer(L->slot);
	ent_set_origin(e, o);
	eng_set_frame_layer(-1);
	return e;
}

static void kill_all(void)
{
	int e;

	for (e = 2; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e))
			ent_destroy(e);
}

/* ---- a cooked sprite (.sc4, tools/cook/hl_sprite_cook.py layout) ---------- */

static uint8_t sc4[1024] __attribute__((aligned(16)));
static res_t test_spr;

static void be16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
	be16(p, v >> 16);
	be16(p + 2, v & 0xFFFF);
}

/* v3: entity frame 0 = image 0; frame 1 = a group of images 1-3 with
 * cumulative interval ends 100, 300, 600 ms; 16 x 16 units each; images
 * at 88, groups at 152, ends at 160, CI4 from 168 */
static void make_sprite(int type)
{
	static const uint16_t ends[4] = { 0, 100, 300, 600 };
	int i;

	memset(sc4, 0, sizeof(sc4));
	be32(sc4, 0x53433431u);
	be16(sc4 + 4, 3);
	be16(sc4 + 6, 2);
	sc4[8] = (uint8_t)type;
	be16(sc4 + 10, 4);
	for (i = 0; i < 16; i++)
		be16(sc4 + 16 + 2 * i, 0xFFFF);
	for (i = 0; i < 4; i++)
	{
		uint8_t *f = sc4 + 88 + 16 * i;

		be16(f, (unsigned)(uint16_t)-8);
		be16(f + 2, 8);
		be16(f + 4, 16);
		be16(f + 6, 16);
		f[8] = 16;
		f[9] = 16;
		be32(f + 12, 168 + 128 * (uint32_t)i);
		be16(sc4 + 160 + 2 * i, ends[i]);
	}
	be16(sc4 + 152, 0);
	be16(sc4 + 154, 1);
	be16(sc4 + 156, 1);
	be16(sc4 + 158, 3);
	/* a sprite id no cooked file uses: past the table's sprites */
	test_spr = RES_MAKE(RES_K_SPRITE, 8000);
	if (sprite_test_inject(test_spr, sc4, 168 + 4 * 128))
	{
		printf("setup FAIL sprite inject\n");
		exit(1);
	}
}

static int sprite_ent(const float g[3], int rm, int amt)
{
	int e = make_ent(g);

	ent_rnd(e)->model = test_spr;
	ent_cv(e)->rendermode = (uint8_t)rm;
	ent_rnd(e)->renderamt = (uint8_t)amt;
	return e;
}

/* ---- X1 render-mode table ---------------------------------------------------- */

static void test_x1(void)
{
	static const char *kinds[3] = { "sprite", "studio", "brush" };
	/* expected (render-fx.md 2, 2a): blender (0 none, 1 alpha, -1 opaque
	 * pass; studio: the GPU_STUDIO mode), Z test, Z write, alpha compare,
	 * flat colour; additive rows go through the alpha blender. Sprite
	 * TransColor draws as Normal; brush TransAlpha draws in the
	 * translucent pass, opaque with Z write (its pages' fence flags set
	 * the alpha compare) */
	static const int want[3][6][5] = {
		{ { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 }, { 1, 1, 0, 0, 0 }, { 1, 0, 0, 0, 0 },
		  { 1, 1, 1, 1, 0 }, { 1, 1, 0, 0, 0 } },
		{ { -1 }, { 1 }, { 1 }, { 2 }, { -1 }, { 2 } },
		{ { -1 }, { 1, 1, 0, 0, 1 }, { 1, 1, 0, 0, 0 }, { 1, 1, 0, 0, 0 }, { 0, 1, 1, 0, 0 },
		  { 1, 1, 0, 0, 0 } },
	};
	float g[3];
	gpu_fx_stats_t gs;
	int k, rm, e[6], i;

	begin();
	for (k = 0; k < 3; k++)
		for (rm = 0; rm < 6; rm++)
		{
			uint8_t b = 255;
			int m = trans_gpu_mode(rm, k, &b);

			if (k == 1)
			{
				printf("x1 %s rm=%d studio_mode=%d blend=%d\n", kinds[k], rm, m, b);
				CHECK(want[k][rm][0] == -1 ? m < 0 : m == want[k][rm][0], "studio rm %d", rm);
			}
			else if (m < 0)
			{
				printf("x1 %s rm=%d opaque-pass\n", kinds[k], rm);
				CHECK(want[k][rm][0] == -1, "%s rm %d not opaque", kinds[k], rm);
			}
			else
			{
				const gpu_fx_rdp_t *r = &gpu_fx_rdp[m];

				printf("x1 %s rm=%d mode=%s blend=%d blender=%d ztest=%d zwrite=%d acmp=%d "
					"cc=%d tex=%d\n", kinds[k], rm, gpu_fx_mode_name[m], b, r->blender,
					r->ztest, r->zwrite, r->acompare, r->combiner, r->textured);
				CHECK(r->blender == want[k][rm][0] && r->ztest == want[k][rm][1]
					&& r->zwrite == want[k][rm][2] && r->acompare == want[k][rm][3]
					&& (r->combiner == GPU_FX_CC_PRIM_A) == want[k][rm][4],
					"%s rm %d state", kinds[k], rm);
			}
			/* additive (Glow 3, TransAdd 5): full blend, the ramp combiner
			 * over the IA16 ramp TLUT (studio: GPU_STUDIO_ADD, checked above) */
			if (rm == 3 || rm == 5)
			{
				CHECK(b == 255, "%s rm %d blend %d (no cap)", kinds[k], rm, b);
				if (k != 1 && m >= 0)
					CHECK(gpu_fx_rdp[m].combiner == GPU_FX_CC_RAMP
						&& gpu_fx_rdp[m].tlut == GPU_FX_TLUT_IA16,
						"%s rm %d not the ramp", kinds[k], rm);
			}
		}
	/* sprite TransColor: the blend only switches it on (full, or off) */
	{
		uint8_t b0 = 0, b1 = 60;

		CHECK(trans_gpu_mode(FX_RM_TRANSCOLOR, 0, &b0) == GPU_FX_CUTOUT && b0 == 0
			&& trans_gpu_mode(FX_RM_TRANSCOLOR, 0, &b1) == GPU_FX_CUTOUT && b1 == 255,
			"sprite TransColor blend %d %d", b0, b1);
		CHECK(gpu_fx_rdp[GPU_FX_BRUSH_SOLID].combiner == GPU_FX_CC_TEX_SHADE
			&& gpu_fx_rdp[GPU_FX_BRUSH_SOLID].textured == 3, "brush TransAlpha: lit page");
	}
	/* recorder: one sprite per render mode, drawn in the pass */
	make_sprite(FX_SPR_PARALLEL);
	eye(L->start_org, 0, 0);
	for (rm = 0; rm < 6; rm++)
	{
		g[0] = L->start_org[0] + L->gofs[0] + 40 + 4 * rm;
		g[1] = L->start_org[1] + L->gofs[1] + 4 * rm - 12;
		g[2] = L->start_org[2] + L->gofs[2];
		e[rm] = sprite_ent(g, rm, 200);
	}
	{
		float eg[3];

		for (i = 0; i < 3; i++)
			eg[i] = L->start_org[i] + L->gofs[i];
		eye(eg, 0, 0);
	}
	draw();
	gpu_fx_stats(&gs);
	for (i = 0; i < GPU_FX_MODES; i++)
		if (gs.calls[i])
			printf("x1 recorder mode=%s calls=%lu\n", gpu_fx_mode_name[i],
				(unsigned long)gs.calls[i]);
	CHECK(gs.calls[GPU_FX_CUTOUT] == 3 && gs.calls[GPU_FX_BLEND] == 1 && gs.calls[GPU_FX_GLOW] == 1
		&& gs.calls[GPU_FX_ADD] == 1, "recorder calls cut %lu blend %lu glow %lu add %lu",
		(unsigned long)gs.calls[GPU_FX_CUTOUT], (unsigned long)gs.calls[GPU_FX_BLEND],
		(unsigned long)gs.calls[GPU_FX_GLOW], (unsigned long)gs.calls[GPU_FX_ADD]);
	(void)e;
	kill_all();
	/* brush TransAlpha joins the translucent pass (blend 0: not drawn) */
	for (i = 0; i < L->dir->nsub; i++)
		if (L->subs[i].kind == W64_SUB_MOVR)
			break;
	CHECK(i < L->dir->nsub, "no MOVR submodel");
	if (i < L->dir->nsub)
	{
		static const int amt[2] = { 255, 0 };
		fx_draw_stats_t ds;
		float z[3] = { 0, 0, 0 };
		int b = make_ent(z), j;

		eng_set_frame_layer(L->slot);
		ent_set_model(b, RES_SUBMODEL(L->slot, i));
		eng_set_frame_layer(-1);
		ent_cv(b)->rendermode = FX_RM_TRANSALPHA;
		for (j = 0; j < 2; j++)
		{
			ent_rnd(b)->renderamt = (uint8_t)amt[j];
			fx_draw_begin();
			ren_trans_brush(b, 100.0f);
			fx_draw_particles();
			fx_draw_trans();
			fx_draw_decals();
			fx_draw_stats(&ds);
			printf("x1 brush TransAlpha amt=%d first=%lu sorted=%lu brushes=%lu\n", amt[j],
				(unsigned long)ds.cutouts, (unsigned long)ds.items, (unsigned long)ds.brushes);
			/* drawn first, unsorted (Z write), before the blended items */
			CHECK(ds.brushes == (amt[j] ? 1u : 0u) && ds.cutouts == ds.brushes && !ds.items,
				"brush TransAlpha amt %d: %lu drawn, %lu first", amt[j],
				(unsigned long)ds.brushes, (unsigned long)ds.cutouts);
		}
	}
	kill_all();
	finish("X1", "render mode table + recorder; brush TransAlpha in the translucent pass");
}

/* ---- X2 ordering, full additive blend, blend 0 ---------------------------------- */

static void test_x2(void)
{
	uint8_t kind[HL64_FX_TRANS_MAX], bl[HL64_FX_TRANS_MAX];
	uint16_t id[HL64_FX_TRANS_MAX];
	float eg[3], dist[HL64_MAX_ENTITIES];
	fx_draw_stats_t ds;
	int i, n, e, k, maxkept = 0;
	unsigned seed = 7;

	begin();
	for (k = 0; k < 3; k++)
		eg[k] = L->start_org[k] + L->gofs[k];
	eye(eg, 0, 0);
	for (i = 0; i < 80; i++)
	{
		float g[3];

		seed = seed * 1103515245u + 12345u;
		g[0] = eg[0] + 50 + (float)(seed >> 8 & 1023);
		g[1] = eg[1] + (float)((seed >> 20) & 63) - 32;
		g[2] = eg[2];
		e = sprite_ent(g, FX_RM_TRANSADD, 255);
		dist[e] = g[0] - eg[0];
	}
	draw();
	n = trans_last_order(kind, id, bl, HL64_FX_TRANS_MAX);
	fx_draw_stats(&ds);
	CHECK(n == HL64_FX_TRANS_MAX, "kept %d", n);
	CHECK(ds.dropped == 16, "dropped %lu", (unsigned long)ds.dropped);
	for (i = 1; i < n; i++)
		CHECK(dist[id[i - 1]] >= dist[id[i]], "order at %d: %g < %g", i,
			(double)dist[id[i - 1]], (double)dist[id[i]]);
	/* the nearest are kept: every dropped one is farther than every kept */
	for (i = 0; i < n; i++)
		if (dist[id[i]] > dist[id[maxkept]])
			maxkept = i;
	for (e = 2; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e))
		{
			int kept = 0;

			for (i = 0; i < n; i++)
				kept |= id[i] == e;
			if (!kept)
				CHECK(dist[e] >= dist[id[maxkept]], "dropped %d nearer than kept", e);
		}
	for (i = 0; i < n; i++)
		CHECK(kind[i] == TK_SPRITE_ENT && bl[i] == 255, "item %d", i);
	kill_all();

	/* blend 0: not drawn, still solid */
	{
		float g[3] = { eg[0] + 64, eg[1], eg[2] }, a[3], b[3];
		float mn[3] = { -16, -16, -16 }, mx[3] = { 16, 16, 16 };
		col_trace_t tr;

		e = sprite_ent(g, FX_RM_TRANSTEXTURE, 0);
		eng_set_frame_layer(L->slot);
		ent_set_size(e, mn, mx);
		ent_set_solid(e, COL_SOLID_BBOX);
		draw();
		fx_draw_stats(&ds);
		CHECK(ds.items == 0 && ds.invisible == 1, "items %lu invisible %lu",
			(unsigned long)ds.items, (unsigned long)ds.invisible);
		for (k = 0; k < 3; k++)
		{
			a[k] = L->start_org[k];
			b[k] = g[k] - L->gofs[k] + (k == 0 ? 64 : 0);
		}
		col_trace_line(a, b, COL_MOVE_NORMAL, NULL, &tr);
		CHECK(tr.ent == e && tr.frac < 1, "trace hit ent %d frac %g", tr.ent, (double)tr.frac);
		eng_set_frame_layer(-1);
		kill_all();
	}
	finish("X2", "back to front, nearest 64 kept, blend 0 invisible and solid");
}

/* ---- X3 exact rates (fx 0, 5-8, 14-16) and the shaped fx' figures ----------- */

static int amt_after(int fx, int amt0, double secs, double dt)
{
	float g[3] = { 0, 0, 0 };
	int e = make_ent(g), a;
	long t, n = lround(secs / dt);

	ent_rnd(e)->renderfx = (uint8_t)fx;
	ent_rnd(e)->renderamt = (uint8_t)amt0;
	for (t = 0; t < n; t++)
		trans_frame(dt);
	a = ent_rnd(e)->renderamt;
	ent_destroy(e);
	return a;
}

static void test_x3(void)
{
	static const double dts[3] = { 1.0 / 30, 1.0 / 60, 1.0 / 20 };
	int i, k;
	char note[160];

	begin();
	for (i = 0; i < 3; i++)
	{
		CHECK(amt_after(FX_RFX_FADE_SLOW, 255, 1, dts[i]) == 225, "fade slow dt %g", dts[i]);
		CHECK(amt_after(FX_RFX_FADE_FAST, 255, 1, dts[i]) == 135, "fade fast dt %g", dts[i]);
		CHECK(amt_after(FX_RFX_SOLID_SLOW, 0, 2, dts[i]) == 60, "solid slow dt %g", dts[i]);
		CHECK(amt_after(FX_RFX_SOLID_FAST, 128, 1, dts[i]) == 248, "solid fast dt %g", dts[i]);
		CHECK(amt_after(FX_RFX_FADE_FAST, 255, 5, dts[i]) == 0, "fade stays at 0");
		CHECK(amt_after(FX_RFX_SOLID_FAST, 100, 5, dts[i]) == 255, "solid stays at 255");
	}
	for (k = 0; k < 2; k++)
	{
		int amt = k ? 128 : 255;

		CHECK(trans_blend_amt(5, FX_RFX_NONE, amt, 3, 50) == amt, "fx 0");
		CHECK(trans_blend_amt(5, FX_RFX_NO_DISSIPATION, amt, 3, 50) == amt, "fx 14");
	}
	/* pulse amplitude / period, strobe rate, distort / hologram means */
	{
		static const struct { int fx; float amp, hz; } pul[4] = {
			{ FX_RFX_PULSE_SLOW, 16, 1 }, { FX_RFX_PULSE_FAST, 16, 4 },
			{ FX_RFX_PULSE_SLOW_WIDE, 64, 1 }, { FX_RFX_PULSE_FAST_WIDE, 64, 4 } };
		double save = fx_s.now;

		for (i = 0; i < 4; i++)
		{
			int lo = 255, hi = 0, cross = 0, prev = 0, s;

			for (s = 0; s < 5000; s++)
			{
				int b, up;

				fx_s.now = 10.0 + s * 0.001;
				b = trans_blend_amt(5, pul[i].fx, 128, 3, 50);
				lo = b < lo ? b : lo;
				hi = b > hi ? b : hi;
				up = b >= 128;
				if (s && up && !prev)
					cross++;
				prev = up;
			}
			CHECK(fabsf((float)(hi - lo) - 2 * pul[i].amp) <= 0.1f * 2 * pul[i].amp,
				"pulse %d amplitude %d", pul[i].fx, hi - lo);
			CHECK(fabsf((float)cross / 5.0f - pul[i].hz) <= 0.1f * pul[i].hz + 0.2f,
				"pulse %d rate %g", pul[i].fx, cross / 5.0);
		}
		for (i = 0; i < 3; i++)
		{
			static const float rate[3] = { 4, 8, 16 };
			int changes = 0, prev = -1, s;

			for (s = 0; s < 5000; s++)
			{
				int b;

				fx_s.now = 10.0 + s * 0.001;
				b = trans_blend_amt(5, FX_RFX_STROBE_SLOW + i, 200, 3, 50);
				if (prev >= 0 && b != prev)
					changes++;
				prev = b;
			}
			CHECK(fabsf(changes / 5.0f - rate[i]) <= 0.1f * rate[i], "strobe %d %g/s",
				FX_RFX_STROBE_SLOW + i, changes / 5.0);
		}
		{
			double sum[4] = { 0, 0, 0, 0 };
			static const float d[4] = { 50, 200, 500, 50 };

			for (i = 0; i < 4000; i++)
			{
				sum[0] += trans_blend_amt(5, FX_RFX_HOLOGRAM, 255, 3, d[0]);
				sum[1] += trans_blend_amt(5, FX_RFX_HOLOGRAM, 255, 3, d[1]);
				sum[2] += trans_blend_amt(5, FX_RFX_HOLOGRAM, 255, 3, d[2]);
				sum[3] += trans_blend_amt(5, FX_RFX_DISTORT, 255, 3, d[3]);
			}
			for (i = 0; i < 4; i++)
				sum[i] /= 4000;
			snprintf(note, sizeof(note), "holo50=%.1f holo200=%.1f holo500=%.1f distort=%.1f",
				sum[0], sum[1], sum[2], sum[3]);
			CHECK(fabs(sum[0] - 223.5) < 5 && fabs(sum[1] - 223.5 * 2 / 3) < 5 && sum[2] == 0
				&& fabs(sum[3] - 223.5) < 5, "%s", note);
		}
		fx_s.now = save;
	}
	finish("X3", "fade/solid exact 30/120 per s at 20-60 fps; fx 0/14 = renderamt; "
		"pulse/strobe figures; distort/hologram means (shapes own, calibration pending)");
}

/* ---- X4 glow: occlusion, constant screen size ------------------------------------- */

static float glow_size(float d)
{
	gpu_fx_vert_t v[64];
	float far_g[3] = { 20000, 20000, 20000 }, g[3], w, z;
	int e, n, m;

	eye(far_g, 0, 0);
	g[0] = far_g[0] + d;
	g[1] = far_g[1];
	g[2] = far_g[2];
	e = sprite_ent(g, FX_RM_GLOW, 255);
	draw();
	n = gpu_fx_host_last(v, 64, &m);
	ent_destroy(e);
	if (n != 4 || m != GPU_FX_GLOW)
		return -1;
	/* view: x forward, -y right; width across / depth */
	w = fabsf((float)(v[1].pos[1] - v[0].pos[1]));
	z = (float)v[0].pos[0];
	return w / z;
}

static void test_x4(void)
{
	float s0, s, eg[3], dir[3] = { 1, 0, 0 }, end[3];
	float ds[5] = { 100, 300, 700, 1200, 2000 };
	col_trace_t tr;
	fx_draw_stats_t st;
	char note[128];
	int i, k, e;

	begin();
	make_sprite(FX_SPR_PARALLEL);
	/* render-fx.md 3: group frames pick by time in the group's intervals */
	{
		const fx_sprite_t *sp = sprite_get(test_spr);
		/* the cycle runs on the clock: 6.0 s = 10 cycles of 0.6 s */
		static const double at[6] = { 6.05, 6.15, 6.35, 6.59, 6.61, 7.35 };
		static const int want[6] = { 1, 2, 3, 3, 1, 2 };

		CHECK(sp && sp->nframes == 2 && sprite_image(sp, 0, 10.2) == 0, "single frame");
		for (i = 0; sp && i < 6; i++)
			CHECK(sprite_image(sp, 1, at[i]) == want[i], "group at %g: %d", at[i],
				sprite_image(sp, 1, at[i]));
	}
	s0 = glow_size(ds[0]);
	CHECK(s0 > 0, "glow not drawn");
	for (i = 1; i < 5; i++)
	{
		s = glow_size(ds[i]);
		CHECK(fabsf(s - s0) <= 0.05f * s0, "size at %g: %g vs %g", (double)ds[i], (double)s,
			(double)s0);
	}
	/* occluded: a sprite behind the first wall ahead of the start */
	for (k = 0; k < 3; k++)
	{
		eg[k] = L->start_org[k] + L->gofs[k];
		end[k] = eg[k] + 4096 * dir[k];
	}
	eye(eg, 0, 0);
	fx_trace(eg, end, COL_MOVE_NOMONSTERS, &tr);
	CHECK(tr.frac < 1, "no wall ahead");
	for (k = 0; k < 3; k++)
		end[k] = tr.end[k] + 32 * dir[k];
	e = sprite_ent(end, FX_RM_GLOW, 255);
	draw();
	fx_draw_stats(&st);
	CHECK(st.glow_occluded == 1 && st.sprites == 0, "occluded %lu drawn %lu",
		(unsigned long)st.glow_occluded, (unsigned long)st.sprites);
	ent_destroy(e);
	snprintf(note, sizeof(note), "screen size %.4f constant 100-2000 u, occluded glow hidden",
		(double)s0);
	finish("X4", note);
}

/* ---- X5 decals ------------------------------------------------------------------- */

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Trace from a (global) along dir up to len: hit point, normal; 0 = none. */
static int ray(const float a[3], const float dir[3], float len, float hit[3], float n[3])
{
	float b[3];
	col_trace_t tr;
	int k;

	for (k = 0; k < 3; k++)
		b[k] = a[k] + dir[k] * len;
	fx_trace(a, b, COL_MOVE_NOMONSTERS, &tr);
	if (tr.frac >= 1 || tr.startsolid)
		return 0;
	memcpy(hit, tr.end, sizeof(tr.end));
	memcpy(n, tr.normal, sizeof(tr.normal));
	return 1;
}

static int corner_test(char *note, int len)
{
	float eg[3], pa[3], na[3], pb[3], nb[3], t[3], up[3] = { 0, 0, 1 }, s[3], p[3];
	float v[HL64_FX_DECAL_VERTS][3];
	int yaw, sgn, k, i, owner, nv, found = 0, onA, onB;

	for (k = 0; k < 3; k++)
		eg[k] = L->start_org[k] + L->gofs[k];
	for (yaw = 0; yaw < 360 && !found; yaw += 15)
	{
		float d[3] = { cosf(yaw * 0.0174533f), sinf(yaw * 0.0174533f), 0 };

		if (!ray(eg, d, 2048, pa, na) || fabsf(na[2]) > 0.1f)
			continue;
		t[0] = na[1] * up[2] - na[2] * up[1];
		t[1] = na[2] * up[0] - na[0] * up[2];
		t[2] = na[0] * up[1] - na[1] * up[0];
		for (sgn = -1; sgn <= 1 && !found; sgn += 2)
		{
			float dir[3] = { t[0] * sgn, t[1] * sgn, t[2] * sgn };

			for (k = 0; k < 3; k++)
				s[k] = pa[k] + na[k] * 8;
			if (!ray(s, dir, 512, pb, nb) || dot3(nb, na) > 0.2f || fabsf(nb[2]) > 0.1f)
				continue;
			/* 2 u from wall B, on wall A: both planes within 4 u */
			for (k = 0; k < 3; k++)
				p[k] = pb[k] - na[k] * 8 - dir[k] * 2;
			found = 1;
		}
	}
	if (!found)
	{
		snprintf(note, (size_t)len, "no inside corner found");
		return -1;
	}
	{
		int before = decal_count(), placed = decal_place_now(p, na, -1, 11 /* {scorch1 */);

		CHECK(placed >= 1, "corner decal placed %d", placed);
		CHECK(decal_count() > before, "count");
	}
	onA = onB = 0;
	for (i = 0; i < HL64_FX_DECALS; i++)
	{
		int np = decal_info(i, &owner, &nv, v, HL64_FX_DECAL_VERTS);

		if (np <= 0)
			continue;
		for (k = 0; k < nv; k++)
		{
			float da[3], db[3], r;
			int j;

			for (j = 0; j < 3; j++)
			{
				da[j] = v[k][j] - pa[j];
				db[j] = v[k][j] - pb[j];
			}
			if (fabsf(dot3(da, na)) < 0.6f)
				onA++;
			else if (fabsf(dot3(db, nb)) < 0.6f)
				onB++;
			/* clipped: never behind the other wall, inside the rectangle */
			CHECK(dot3(db, nb) > -0.6f && dot3(da, na) > -0.6f, "vertex %d behind a wall", k);
			for (j = 0; j < 3; j++)
				da[j] = v[k][j] - p[j];
			r = sqrtf(dot3(da, da));
			CHECK(r <= 48 * 1.4143f + 8.5f, "vertex %d %g u from the centre", k, (double)r);
		}
	}
	/* vertices on the corner line count for A; B must hold its own */
	CHECK(onA >= 3 && onB >= 1, "split across both faces: %d on A, %d on B", onA, onB);
	snprintf(note, (size_t)len, "corner split A=%d B=%d verts", onA, onB);
	return 0;
}

static int door_test(void)
{
	int i, k, e = -1, owner, nv, np;
	float v0[HL64_FX_DECAL_VERTS][3], v1[HL64_FX_DECAL_VERTS][3];

	for (i = 0; i < L->nmovers && e < 0; i++)
	{
		const wld_mover_t *mv = &L->movers[i];
		const w64_sub_t *sb = &L->subs[mv->sub];
		static const float dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 },
			{ 0, 0, 1 }, { 0, 0, -1 } };
		float c[3], hx[3];
		int d;

		if (sb->kind != W64_SUB_MOVR)
			continue;
		e = ent_create(ENT_ALLOC_GAME);
		ent_idc(e)->layer = (int8_t)L->slot;
		eng_set_frame_layer(L->slot);
		ent_set_model(e, RES_SUBMODEL(L->slot, mv->sub));
		ent_set_movetype(e, MOVETYPE_PUSH);
		ent_set_solid(e, COL_SOLID_BSP);
		ent_set_origin(e, mv->origin);
		for (k = 0; k < 3; k++)
		{
			c[k] = 0.5f * (sb->bbox[k] + sb->bbox[k + 3]) + mv->origin[k];
			hx[k] = 0.5f * (sb->bbox[k + 3] - sb->bbox[k]) + 16;
		}
		for (d = 0; d < 6; d++)
		{
			float a[3], b[3], z[3] = { 0, 0, 0 }, g[3];
			col_trace_t tr;

			for (k = 0; k < 3; k++)
			{
				a[k] = c[k] + dirs[d][k] * hx[k];
				b[k] = c[k];
			}
			col_clip(ent_cv(e), a, z, z, b, &tr);
			if (tr.frac >= 1 || tr.startsolid)
				continue;
			for (k = 0; k < 3; k++)
				g[k] = tr.end[k] + L->gofs[k];
			eng_set_frame_layer(-1);
			if (decal_place_now(g, tr.normal, e, 0) > 0)
				goto placed;
			eng_set_frame_layer(L->slot);
		}
		eng_set_frame_layer(-1);
		ent_destroy(e);
		e = -1;
	}
	CHECK(e > 0, "no mover face took a decal");
	return -1;
placed:
	np = -1;
	for (i = 0; i < HL64_FX_DECALS; i++)
		if (decal_info(i, &owner, &nv, v0, HL64_FX_DECAL_VERTS) > 0 && owner == e)
		{
			float o[3];

			memcpy(o, ent_cv(e)->origin, sizeof(o));
			o[2] += 32;
			eng_set_frame_layer(L->slot);
			ent_set_origin(e, o);
			eng_set_frame_layer(-1);
			np = decal_info(i, &owner, &nv, v1, HL64_FX_DECAL_VERTS);
			for (k = 0; k < nv; k++)
				CHECK(fabsf(v1[k][2] - v0[k][2] - 32) < 0.01f && fabsf(v1[k][0] - v0[k][0]) < 0.01f,
					"door decal vertex %d did not follow", k);
			break;
		}
	CHECK(np > 0, "door decal not found");
	ent_destroy(e);
	/* the mover is gone: its decal goes on the next draw */
	draw();
	for (i = 0; i < HL64_FX_DECALS; i++)
		if (decal_info(i, &owner, &nv, v0, 1) > 0)
			CHECK(owner != e, "decal of a freed mover drawn");
	return 0;
}

static uint32_t	wld_fr = 1;	/* wld_frame counter of the test's read pumps */

/* The render chunks of every sector meeting box (local, grown by pad)
 * resident: request, pump the reads. 0 when they landed. */
static int make_resident(const int16_t box[6], float pad)
{
	int it, s, k, missing = 1;

	for (it = 0; it < 400 && missing; it++)
	{
		missing = 0;
		io_frame_begin(33333);
		for (s = 0; s < L->dir->nsect; s++)
		{
			const int16_t *b = L->sects[s].bbox;

			for (k = 0; k < 3; k++)
				if (b[k] > box[k + 3] + pad || b[k + 3] < box[k] - pad)
					break;
			if (k == 3)
				missing |= !wld_chunk_use(L, L->sects[s].rend_chunk);
		}
		io_pump();
		wld_frame(wld_fr++);
	}
	return missing;
}

/* Oracle, independent of decal.c: a drawn world triangle (resident sector
 * chunk) lies in the hit plane (1 u) along the hit normal, and the hit
 * point falls inside it (0.5 u). Hits without one (clip-only hull,
 * faces the cook dropped) cannot take a decal. */
static int drawn_face_at(const float hit_g[3], const float n[3])
{
	unsigned stride = L->hdr.idx_stride ? L->hdr.idx_stride : 1, base = L->hdr.idx_base;
	float q[3];
	int s, i, j, k;

	for (k = 0; k < 3; k++)
		q[k] = hit_g[k] - L->gofs[k];
	for (s = 0; s < L->dir->nsect; s++)
	{
		const w64_sector_t *sc = &L->sects[s];
		const uint8_t *ch;
		const w64_rend_t *r;
		const w64_meshlet_t *m;

		for (k = 0; k < 3; k++)
			if (q[k] + 2 < sc->bbox[k] || q[k] - 2 > sc->bbox[k + 3])
				break;
		if (k < 3 || !(ch = wld_chunk_use(L, sc->rend_chunk)))
			continue;
		r = (const w64_rend_t *)ch;
		m = (const w64_meshlet_t *)(ch + sizeof(w64_rend_t));
		for (i = 0; i < r->nmeshlets; i++, m++)
		{
			const w64_vert_t *vp = (const w64_vert_t *)(ch + W64_MESHLET_VOFS(m));
			const int16_t *idx = (const int16_t *)(ch + W64_MESHLET_IOFS(m));
			int ni = W64_MESHLET_NI(m), nv = W64_MESHLET_NV(m), s0 = 0;

			for (j = 0; j < ni; j++)
			{
				int t[3], e, inside = 1;
				float p[3][3], e1[3], e2[3], fn[3], l;

				if ((uint16_t)idx[j] & W64_IDX_RESTART)
					s0 = j;
				if (j - s0 < 2)
					continue;
				for (e = 0; e < 3; e++)
				{
					t[e] = (int)(((uint16_t)idx[j - 2 + e] & 0x3FFF) - base) / (int)stride;
					if (t[e] < 0 || t[e] >= nv)
						break;
					for (k = 0; k < 3; k++)
						p[e][k] = (t[e] & 1) ? vp[t[e] >> 1].posB[k] : vp[t[e] >> 1].posA[k];
				}
				if (e < 3)
					continue;
				for (k = 0; k < 3; k++)
				{
					e1[k] = p[1][k] - p[0][k];
					e2[k] = p[2][k] - p[0][k];
				}
				fn[0] = e1[1] * e2[2] - e1[2] * e2[1];
				fn[1] = e1[2] * e2[0] - e1[0] * e2[2];
				fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
				if ((l = sqrtf(dot3(fn, fn))) < 1e-3f)
					continue;
				for (k = 0; k < 3; k++)
					fn[k] /= l;
				if (fabsf(dot3(fn, n)) < 0.9f)
					continue;
				for (k = 0; k < 3; k++)
					e1[k] = q[k] - p[0][k];
				if (fabsf(dot3(e1, fn)) > 1)
					continue;
				for (e = 0; e < 3 && inside; e++)
				{
					const float *a = p[e], *b = p[(e + 1) % 3], *c = p[(e + 2) % 3];
					float ed[3], m2[3], dq, dc;

					for (k = 0; k < 3; k++)
						ed[k] = b[k] - a[k];
					m2[0] = fn[1] * ed[2] - fn[2] * ed[1];
					m2[1] = fn[2] * ed[0] - fn[0] * ed[2];
					m2[2] = fn[0] * ed[1] - fn[1] * ed[0];
					if ((l = sqrtf(dot3(m2, m2))) < 1e-6f)
						break;
					dq = (dot3(m2, q) - dot3(m2, a)) / l;
					dc = (dot3(m2, c) - dot3(m2, a)) / l;
					if (dc < 0)
						dq = -dq;
					inside = dq > -0.5f;
				}
				if (inside && e == 3)
					return 1;
			}
		}
	}
	return 0;
}

/* Every traced hit on a drawn face takes a decal, whichever of the
 * point's chunks holds the face (regression: the winding vote asked only
 * the first chunk, so a face in another one rejected every triangle as
 * facing away: 1010 of 3199 hits on hldemo1; ares: a glock at a corridor
 * wall left no hole). Rays from the centre of every sector of the map
 * (its neighbourhood made resident first): 24 yaws x 3 pitches, 256 u. */
static int hits_test(char *note, int len)
{
	float hit[3], n[3];
	int s, origins = 0, yaw, pitch, k, hits = 0, faceless = 0, waiting = 0, bad = 0;

	for (s = 0; s < L->dir->nsect; s++)
	{
		const int16_t *b = L->sects[s].bbox;
		float eg[3];

		if (make_resident(b, 256 + 64))
			continue;
		origins++;
		for (k = 0; k < 3; k++)
			eg[k] = L->gofs[k] + 0.5f * (b[k] + b[k + 3]);
		for (pitch = -30; pitch <= 30; pitch += 30)
			for (yaw = 0; yaw < 360; yaw += 15)
			{
				float cp = cosf(pitch * 0.0174533f);
				float d[3] = { cp * cosf(yaw * 0.0174533f), cp * sinf(yaw * 0.0174533f),
					sinf(pitch * 0.0174533f) };
				int r;

				if (!ray(eg, d, 256, hit, n))
					continue;
				r = decal_place_now(hit, n, -1, 0);
				if (r < 0)
				{
					waiting++;	/* a chunk not resident in this setup */
					continue;
				}
				if (!drawn_face_at(hit, n))
				{
					faceless++;
					continue;
				}
				hits++;
				if (r == 0 && bad++ < 4)
					CHECK(0, "sector %d yaw %d pitch %d: hit %g %g %g n %g %g %g took no decal",
						s, yaw, pitch, (double)hit[0], (double)hit[1], (double)hit[2],
						(double)n[0], (double)n[1], (double)n[2]);
			}
	}
	CHECK(hits >= 100, "only %d rays hit drawn faces", hits);
	CHECK(!bad, "%d of %d hits on drawn faces took no decal", bad, hits);
	snprintf(note, (size_t)len, "%d hits on drawn faces from %d sectors all placed (%d faceless)",
		hits, origins, faceless);
	return bad ? -1 : 0;
}

static void test_x5(void)
{
	char note[288] = "", hits[96] = "";
	decal_stats_t ds;
	float eg[3], d[3] = { 1, 0, 0 }, p[3], n[3];
	int i, k;

	begin();
	fx_init();
	corner_test(note, sizeof(note));
	door_test();
	fx_init();
	hits_test(hits, sizeof(hits));
	fx_init();
	/* pool: 100 placements on one wall keep 96, oldest replaced */
	for (k = 0; k < 3; k++)
		eg[k] = L->start_org[k] + L->gofs[k];
	CHECK(ray(eg, d, 4096, p, n), "no wall ahead");
	decal_stats(&ds, 1);
	for (i = 0; i < 100; i++)
	{
		float q[3] = { p[0], p[1], p[2] + (float)(i % 5) - 2 };

		decal_place_now(q, n, -1, i % 5);
	}
	decal_stats(&ds, 0);
	CHECK(decal_count() == HL64_FX_DECALS, "pool %d", decal_count());
	CHECK(ds.replaced >= 4, "replaced %d", ds.replaced);
	/* the queue: 3 ms cap and 16 pending, drops counted */
	fx_init();
	for (i = 0; i < 40; i++)
	{
		col_trace_t tr;

		memset(&tr, 0, sizeof(tr));
		tr.frac = 0.5f;
		for (k = 0; k < 3; k++)
		{
			tr.end[k] = p[k] - L->gofs[k];
			tr.normal[k] = n[k];
		}
		eng_set_frame_layer(L->slot);
		fx_decal_trace(&tr, 0);
		eng_set_frame_layer(-1);
	}
	decal_stats(&ds, 0);
	CHECK(ds.dropped == 40 - HL64_FX_DECAL_QUEUE, "queue drops %d", ds.dropped);
	for (i = 0; i < 60; i++)
	{
		step(1.0 / 30);
		decal_stats(&ds, 0);
		if (!ds.pending && decal_count() >= HL64_FX_DECAL_QUEUE)
			break;
	}
	CHECK(decal_count() >= HL64_FX_DECAL_QUEUE && ds.pending == 0, "queued placed %d pending %d",
		decal_count(), ds.pending);
	draw();
	{
		gpu_fx_stats_t gs;

		gpu_fx_stats(&gs);
		CHECK(gs.calls[GPU_FX_DECAL] >= 1, "decals drawn %lu", (unsigned long)gs.calls[GPU_FX_DECAL]);
	}
	/* regression (hldemo3 SIGSEGV): a chunk whose block is being refilled
	 * (evicted, LOADING again) is never walked or drawn from; placement
	 * waits, the decals are kept and come back when it lands */
	{
		static uint8_t saved[1024];
		gpu_fx_stats_t gs;
		int c, kept = decal_count(), r;

		for (c = 0; c < (int)L->hdr.nchunks && c < 1024; c++)
		{
			saved[c] = L->ch[c].state;
			if (L->chunks[c].fourcc == W64_CHUNK_REND && L->ch[c].state == WLD_CH_RESIDENT)
				L->ch[c].state = WLD_CH_LOADING;
		}
		r = decal_place_now(p, n, -1, 0);
		CHECK(r < 0, "placement on a loading chunk returned %d (must wait)", r);
		draw();
		gpu_fx_stats(&gs);
		CHECK(gs.calls[GPU_FX_DECAL] == 0, "decals drawn from a loading chunk");
		CHECK(decal_count() == kept, "decals dropped while their chunk reloads");
		for (c = 0; c < (int)L->hdr.nchunks && c < 1024; c++)
			L->ch[c].state = saved[c];
		r = decal_place_now(p, n, -1, 0);
		CHECK(r > 0, "placement after the chunk landed %d", r);
		draw();
		gpu_fx_stats(&gs);
		CHECK(gs.calls[GPU_FX_DECAL] >= 1, "decals redrawn after landing");
	}
	/* drawn in the surface's frame: every drawn vertex is its stored
	 * position, whatever the eye's fraction (the cart's decal Z compare
	 * failed when camera-relative quarter units at eye x .96875 put the
	 * decals behind the wall) */
	{
		float g[3], w[HL64_FX_DECAL_VERTS][3], err = 0;
		gpu_fx_vert_t v[64];
		ren_mat_t mt;
		int j, m, nv = 0, nd = 0, owner, framed;

		fx_init();
		CHECK(decal_place_now(p, n, -1, 0) > 0, "frame: placement");
		for (i = 0; i < HL64_FX_DECALS && decal_info(i, &owner, &nd, w, HL64_FX_DECAL_VERTS) <= 0; i++)
			;
		for (k = 0; k < 3; k++)
			g[k] = eg[k] + (k == 0 ? 0.96875f : k == 1 ? 0.40625f : 0.15625f);
		eye(g, 0, 0);
		draw();
		nv = gpu_fx_host_last(v, 64, &m);
		framed = gpu_fx_host_last_frame(&mt);
		CHECK(i < HL64_FX_DECALS && m == GPU_FX_DECAL && framed && nv == nd,
			"frame: decal %d mode %d framed %d verts %d/%d", i, m, framed, nv, nd);
		for (j = 0; j < nv && j < nd; j++)
			for (k = 0; k < 3; k++)
			{
				float q = mt.m[3][k] + g[k] + v[j].pos[0] * mt.m[0][k]
					+ v[j].pos[1] * mt.m[1][k] + v[j].pos[2] * mt.m[2][k];

				if (fabsf(q - w[j][k]) > err)
					err = fabsf(q - w[j][k]);
			}
		CHECK(err <= 1.0f / 256, "frame: drawn vertex %.4f u off its stored position", err);
		{
			size_t l = strlen(note);

			snprintf(note + l, sizeof(note) - l, "; drawn in frame, max %.4f u off", err);
		}
	}
	/* the vertex ring at its worst: all HL64_FX_DECALS in view, each
	 * HL64_FX_DECAL_VERTS vertices and its frame matrix, none dropped */
	{
		gpu_fx_stats_t gs;
		float w1[1][3];
		int owner, nd, i2 = -1, need = HL64_FX_DECALS * ((HL64_FX_DECAL_VERTS + 1) / 2
			+ GPU_FX_MAT_PAIRS);

		fx_init();
		CHECK(decal_place_now(p, n, -1, 0) > 0, "worst: placement");
		for (i = 0; i < HL64_FX_DECALS && decal_info(i, &owner, &nd, w1, 1) <= 0; i++)
			;
		if (i < HL64_FX_DECALS)
			i2 = decal_test_fill_worst(i);
		eye(eg, 0, 0);
		draw();
		gpu_fx_stats(&gs);
		CHECK(i2 == HL64_FX_DECALS && gs.calls[GPU_FX_DECAL] == HL64_FX_DECALS && !gs.drops
			&& gs.pairs >= (uint32_t)need, "worst: %d decals filled, %lu drawn, %lu dropped, "
			"%lu pairs (need %d)", i2, (unsigned long)gs.calls[GPU_FX_DECAL],
			(unsigned long)gs.drops, (unsigned long)gs.pairs, need);
		{
			size_t l = strlen(note);

			snprintf(note + l, sizeof(note) - l, "; %d worst-case decals: %lu of %d ring pairs, "
				"%lu dropped", HL64_FX_DECALS, (unsigned long)gs.pairs, GPU_FX_RING_PAIRS,
				(unsigned long)gs.drops);
		}
		fx_init();
	}
	{
		size_t l = strlen(note);

		snprintf(note + l, sizeof(note) - l, "; %s", hits);
	}
	finish("X5", note);
}

/* ---- X6 effect catalogue under bursts --------------------------------------------- */

static res_t find_res(int kind, const char *name)
{
	int i, n = res_count(kind);

	for (i = 0; i < n; i++)
		if (!strcmp(res_name(RES_MAKE(kind, i)), name))
			return RES_MAKE(kind, i);
	return RES_NONE;
}

static void test_x6(void)
{
	res_t spr = find_res(RES_K_SPRITE, "sprites/zerogxplode.spr");
	res_t gib = find_res(RES_K_MODEL, "models/hgibs.mdl");
	float o[3], d[3] = { 0, 0, 1 }, sz[3] = { 64, 64, 64 }, v[3] = { 0, 0, 100 }, b[3];
	int i, k, beams = 0, id;
	char note[200];
	uint32_t td, pd, bd;

	begin();
	fx_init();
	for (k = 0; k < 3; k++)
	{
		o[k] = L->start_org[k];
		b[k] = o[k] + (k == 0 ? 200 : 0);
	}
	eng_set_frame_layer(L->slot);
	for (i = 0; i < 60; i++)
	{
		fx_sparks(o);
		fx_ricochet(o, 1);
		fx_gunshot(o);
		fx_explosion(o, spr, 20, 15, 0);
		fx_smoke(o, spr, 20, 12);
		fx_tracer(o, b);
		fx_sprite(o, spr, 10, 200);
		fx_glow_sprite(o, spr, 10, 10, 200);
		fx_blood_sprite(o, spr, spr, 247, 10);
		fx_blood_stream(o, d, 247, 200);
		fx_break_model(o, sz, v, 100, gib, 0, 25, 0x08);
		fx_temp_model(o, v, 90, gib, 1, 25);
		fx_dlight(o, 20, 255, 255, 255, 5, 10);
		fx_elight(1, o, 100, 255, 255, 255, 5, 100);
		fx_particles(o, d, 247, 20);
		fx_implosion(o, 100, 20, 5);
		fx_beam_cylinder(o, 200, spr, 5, 16, 0, 255, 255, 255, 200);
		id = fx_beam(-1, o, -1, b, spr, 20, 10, 255, 255, 255, 200, 10, 0);
		beams += id >= 0;
		fx_shake(o, 4, 2, 1, 0);
		fx_fade(0, 0, 0, 255, 1, 1, 1);
		fx_text(0);
		fx_lightstyle(2, 0);
		CHECK(tent_live() <= HL64_FX_TEMPS && part_live() <= HL64_FX_PARTICLES
			&& beam_live() <= HL64_FX_BEAMS, "caps exceeded");
	}
	eng_set_frame_layer(-1);
	td = tent_dropped(0);
	pd = part_dropped(0);
	bd = beam_dropped(0);
	CHECK(tent_live() == HL64_FX_TEMPS && part_live() == HL64_FX_PARTICLES
		&& beam_live() == HL64_FX_BEAMS, "pools full: temps %d parts %d beams %d", tent_live(),
		part_live(), beam_live());
	CHECK(td > 0 && pd > 0 && bd > 0, "drops counted %lu %lu %lu", (unsigned long)td,
		(unsigned long)pd, (unsigned long)bd);
	/* drawing a full scene does not crash; then everything with a life ends */
	{
		float eg[3];

		for (k = 0; k < 3; k++)
			eg[k] = L->start_org[k] + L->gofs[k] - 100;
		eye(eg, 0, 0);
	}
	draw();
	for (i = 0; i < 30 * 10; i++)
	{
		step(1.0 / 30);
		if (i % 10 == 0)
			draw();
	}
	/* infinite-life beams stay until killed */
	CHECK(tent_live() == 0 && part_live() == 0, "after 10 s: temps %d parts %d", tent_live(),
		part_live());
	for (i = 0; i < HL64_FX_BEAMS; i++)
		fx_beam_kill(i);
	CHECK(beam_live() == 0, "beams after kill %d", beam_live());
	snprintf(note, sizeof(note), "60x burst of every effect: drops temps=%lu parts=%lu beams=%lu, "
		"all freed after 10 s", (unsigned long)td, (unsigned long)pd, (unsigned long)bd);
	finish("X6", note);
}

/* ---- screen state ---------------------------------------------------------------------- */

static void test_screen(void)
{
	float org[3], ang[3], a;
	uint8_t rgba[4];
	int k;

	begin();
	fx_init();
	step(0);		/* the effects clock on the test clock */
	fx_fade(255, 0, 0, 200, 1.0f, 0.5f, 1 /* FFADE_OUT */);
	step(0.5);
	fx_view(org, ang, rgba);
	CHECK(rgba[0] == 255 && abs(rgba[3] - 100) <= 1, "fade out half %d", rgba[3]);
	step(0.75);
	fx_view(org, ang, rgba);
	CHECK(rgba[3] == 200, "fade hold %d", rgba[3]);
	step(1.0);
	fx_view(org, ang, rgba);
	CHECK(rgba[3] == 0, "fade over %d", rgba[3]);
	fx_fade(0, 0, 0, 255, 1.0f, 0, 0 /* FFADE_IN */);
	step(0.25);
	fx_view(org, ang, rgba);
	CHECK(abs(rgba[3] - 191) <= 2, "fade in quarter %d", rgba[3]);
	/* shake: the player on the ground, inside the radius */
	if (ent_is_live(1))
	{
		ent_cv(1)->flags |= FL_ONGROUND;
		eng_set_frame_layer(ent_idc(1)->layer);
		memcpy(org, ent_cv(1)->origin, sizeof(org));
		fx_shake(org, 8, 10, 2, 100);
		eng_set_frame_layer(-1);
		step(0.1);
		fx_view(org, ang, rgba);
		a = 0;
		for (k = 0; k < 3; k++)
			a += fabsf(org[k]);
		CHECK(a > 0, "no shake offset");
		step(2.0);
		fx_view(org, ang, rgba);
		CHECK(org[0] == 0 && org[1] == 0 && org[2] == 0, "shake did not end");
	}
	/* titles.txt: fade in + hold + fade out, the fade in per character
	 * under effect 2 (write out) */
	{
		const res_title_t *t = res_title(0);
		float end = !t ? 2.0f : (t->effect == 2 ? t->fadein * t->text_len : t->fadein)
			+ t->holdtime + t->fadeout;

		fx_text(0);
		step(0.01);
		CHECK(fx_text_current(&a) == 0 && fabsf(a - 0.01f) < 0.005f, "title 0 showing %g", a);
		step(end - 0.06);
		CHECK(fx_text_current(&a) == 0 && fabsf(a - (end - 0.05f)) < 0.005f,
			"title 0 (effect %d) still showing at %g of %g s", t ? t->effect : -1, a, end);
		step(0.1);
		CHECK(fx_text_current(&a) == 0xFFFF, "title over");
	}
	finish("SCREEN", ent_is_live(1) ? "fade in/out/hold, shake inside radius and decay, title timing"
		: "fade in/out/hold, title timing (shake needs the player: not run)");
}

/* ---- dlights (D1, D2, D4, model term) -------------------------------------------------- */

static int live_count(void)
{
	int i, n = 0;

	for (i = 0; i < HL64_DLIGHTS; i++)
		n += ren_dlight_live(i);
	return n;
}

static void test_dlights(void)
{
	ren_dlight_t *a, *b, *c;
	double t = 100.0;
	int i;
	float bright, end[3], eg[3], ang[3] = { 0, 0, 0 };

	begin();
	/* D1: key reuse, oldest-dead reuse, decay to death, one-frame lights */
	for (i = 0; i < HL64_DLIGHTS; i++)
		ren_dlight_alloc(0, t)->die = t - 1;
	ren_dlight_frame(t, 0.0);
	CHECK(live_count() == 0, "dead lights cleared %d", live_count());
	a = ren_dlight_alloc(7, t);
	a->die = t + 5;
	a->radius = 100;
	b = ren_dlight_alloc(7, t);
	CHECK(a == b, "key reuse");
	a->die = t + 5;		/* the reuse cleared it */
	a->radius = 100;
	c = ren_dlight_alloc(0, t);
	CHECK(c != a, "a free light, not the keyed one");
	c->die = t + 5;
	c->radius = 50;
	c->decay = 100;
	ren_dlight_frame(t + 0.25, 0.25);
	CHECK(fabsf(c->radius - 25) < 1e-3f && live_count() == 2, "decay %g", (double)c->radius);
	ren_dlight_frame(t + 0.5, 0.25);
	CHECK(live_count() == 1, "decayed to 0 dies (%d live)", live_count());
	c = ren_dlight_alloc(0, t + 0.5);
	c->die = t + 0.5;	/* one frame */
	c->radius = 80;
	ren_dlight_frame(t + 0.5, 0.0);
	CHECK(live_count() == 2, "one-frame light live this frame");
	ren_dlight_frame(t + 0.6, 0.1);
	CHECK(live_count() == 1, "one-frame light gone next frame");
	/* oldest dead reused before light 0 when full */
	for (i = 0; i < HL64_DLIGHTS; i++)
	{
		ren_dlight_t *l = ren_dlight_alloc(0, t + 0.6);

		l->die = t + 10;
		l->radius = 10;
	}
	ren_dlight(5)->die = t;
	{
		ren_dlight_t *x = ren_dlight_alloc(0, t + 1);

		CHECK(x == ren_dlight(5), "first dead light reused");
		x->die = t + 10;
		CHECK(ren_dlight_alloc(0, t + 1) == ren_dlight(0), "full: light 0");
	}
	for (i = 0; i < HL64_DLIGHTS; i++)
		ren_dlight(i)->die = 0;
	ren_dlight_frame(t + 2, 0);
	/* D2: falloff grid */
	{
		static const float ps[4] = { 0, 10, -30, 90 }, qs[4] = { 0, 20, 50, 200 };
		int pi, qi;

		for (pi = 0; pi < 4; pi++)
			for (qi = 0; qi < 4; qi++)
			{
				float want = 100 - fabsf(ps[pi]) < 20 ? 0
					: (100 - fabsf(ps[pi]) - qs[qi] > 0 ? 100 - fabsf(ps[pi]) - qs[qi] : 0);

				CHECK(fabsf(ren_dlight_falloff(100, 20, ps[pi], qs[qi]) - want) < 1e-4f,
					"falloff p %g q %g", (double)ps[pi], (double)qs[qi]);
			}
		CHECK(ren_dlight_falloff(100, 20, 85, 0) == 0, "minlight cutoff");
	}
	/* D4: brightness at 100 / 500 / 1000 / 2000 u, end point of the trace */
	CHECK(ren_flashlight_bright(100) == 1 && ren_flashlight_bright(500) == 1, "near 1");
	CHECK(fabsf(ren_flashlight_bright(1000) - 64.0f / 255) < 1e-6f, "1000 u %g",
		(double)ren_flashlight_bright(1000));
	CHECK(fabsf(ren_flashlight_bright(2000) - 16.0f / 255) < 1e-6f, "2000 u %g",
		(double)ren_flashlight_bright(2000));
	{
		float far_g[3] = { 20000, 20000, 20000 };

		ren_flashlight_place(far_g, ang, -1, end, &bright);
		CHECK(fabsf(end[0] - 22000) < 0.1f && fabsf(bright - 16.0f / 255) < 1e-6f,
			"open 2000 u end %g bright %g", (double)end[0], (double)bright);
	}
	for (i = 0; i < 3; i++)
		eg[i] = L->start_org[i] + L->gofs[i];
	{
		float dir[3] = { 1, 0, 0 }, hit[3], n[3], d;

		if (!ray(eg, dir, 2000, hit, n))
			for (i = 0; i < 3; i++)
				hit[i] = eg[i] + 2000 * dir[i];
		ren_flashlight_place(eg, ang, -1, end, &bright);
		d = hit[0] - eg[0];
		CHECK(fabsf(end[0] - hit[0]) < 0.1f && fabsf(bright - ren_flashlight_bright(d)) < 1e-5f,
			"wall end %g vs %g", (double)end[0], (double)hit[0]);
	}
	/* dlights.md 4: model term, elights on their own entity only */
	{
		float o[3] = { 0, 0, 0 };
		ren_dlight_t *l = ren_dlight_alloc(0, t + 3);

		l->die = t + 9;
		l->radius = 100;
		l->origin[0] = 60;
		CHECK(ren_model_light(5, o, 50, -1, 0) == 90, "dlight term %d",
			ren_model_light(5, o, 50, -1, 0));
		l = ren_dlight_alloc(9, t + 3);
		l->die = t + 9;
		l->radius = 30;
		l->world = 0;
		l->entity = 9;
		CHECK(ren_model_light(9, o, 50, -1, 0) == 120, "elight on its entity");
		CHECK(ren_model_light(5, o, 50, -1, 0) == 90, "elight not on others");
		CHECK(ren_model_light(5, o, 0, -1, 0) >= 32 && ren_model_light(5, o, 0, -1, 0) == 40,
			"floor/sum %d", ren_model_light(5, o, 0, -1, 0));
		CHECK(ren_model_light(5, o, 250, -1, 0) == 255, "clamp");
		CHECK(ren_model_light(4, o, -200, -1, 0) == 32, "floor 32");
		for (i = 0; i < HL64_DLIGHTS; i++)
			ren_dlight(i)->die = 0;
		ren_dlight_frame(t + 20, 0);
	}
	/* flashlight pool (dlights.md 3): a flashlight 16 u off the wall ahead
	 * of the start draws pool pieces and lights no vertex; the same light
	 * as a plain dlight draws none */
	{
		float g[3], hit[3], n[3], d[3] = { 1, 0, 0 };
		ren_stats_t rs;
		uint32_t tris_flash, dlit_flash;
		int k, pass;

		for (k = 0; k < 3; k++)
			g[k] = L->start_org[k] + L->gofs[k];
		CHECK(ray(g, d, 4096, hit, n), "no wall ahead");
		eye(g, 0, 0);
		for (pass = 0; pass < 2; pass++)
		{
			ren_dlight_t *l = ren_dlight_alloc(1, t);

			for (k = 0; k < 3; k++)
				l->origin[k] = hit[k] + 16 * n[k];
			l->radius = 80;
			l->die = t + 1;
			l->color[0] = l->color[1] = l->color[2] = 255;
			l->flash = (uint8_t)!pass;
			ren_frame(t);
			ren_stats(&rs);
			if (!pass)
			{
				tris_flash = rs.pool_tris;
				dlit_flash = rs.dlit;
				CHECK(rs.pool_tris > 0 && rs.pool_verts >= 3 * 8,
					"flashlight pool tris %lu verts %lu", (unsigned long)rs.pool_tris,
					(unsigned long)rs.pool_verts);
			}
			else
				CHECK(rs.pool_tris == 0 && rs.dlit > dlit_flash,
					"plain dlight: pool %lu dlit %lu (flash dlit %lu)",
					(unsigned long)rs.pool_tris, (unsigned long)rs.dlit,
					(unsigned long)dlit_flash);
			l->die = 0;
			ren_dlight_frame(t + 1, 0);
		}
		finish("DLIGHT", "D1 pool, D2 falloff grid + minlight, D4 brightness + trace end, model term "
			"(dlights.md 4), flashlight pool pieces drawn, not vertex-lit");
		(void)tris_flash;
	}
}

/* ---- lightstyles (L1, L2, fx_lightstyle, model term) --------------------------------------- */

static void test_styles(void)
{
	static uint8_t all[26], abc[3] = { 0, 1, 2 };
	static const double ts[5] = { 1.0, 1.05, 1.15, 1.25, 1.35 };
	static const int want[5] = { 1, 1, 2, 0, 1 };
	const uint8_t *pat;
	int i, len;
	float o[3] = { 0, 0, 0 };

	begin();
	wld_style_reset();
	CHECK(wld_style_value(3) == 256, "unset 256");
	for (i = 0; i < 26; i++)
	{
		all[i] = (uint8_t)i;
		wld_style_set(4, all + i, 1);
		CHECK(wld_style_value(4) == i * 22, "letter %c = %d", 'a' + i, wld_style_value(4));
	}
	wld_style_set(4, NULL, 0);
	CHECK(wld_style_value(4) == 256, "empty 256");
	wld_style_set(5, abc, 3);
	for (i = 0; i < 5; i++)
	{
		wld_style_tick(ts[i]);
		CHECK(wld_style_value(5) == want[i] * 22, "abc at %g: %d", ts[i], wld_style_value(5));
	}
	/* pfnLightStyle path: cooked pattern ids */
	wld_style_tick(2.0);
	for (i = 0; i < res_patterns() && i < 14; i++)
	{
		pat = res_pattern((uint16_t)i, &len);
		fx_lightstyle(6, (uint16_t)i);
		CHECK(pat && len > 0 && wld_style_value(6) == pat[20 % len] * 22,
			"pattern %d via fx_lightstyle", i);
	}
	/* lightstyles.md 4: base + amount x value / 264 */
	wld_style_set(7, all + 12, 1);	/* m = 264 */
	CHECK(ren_model_light(3, o, 40, 7, 100) == 140, "style term at m");
	wld_style_set(7, all + 0, 1);	/* a = 0 */
	CHECK(ren_model_light(3, o, 40, 7, 100) == 40, "style term off");
	wld_style_reset();
	finish("LSTYLE", "L1 letters + unset, L2 abc timing, fx_lightstyle -> pattern table, model term "
		"(lightstyles.md 4)");
}

/* ---- ADD: additive without the RDP adder's wrap (render-fx.md 2a) ---------------- */

static const char *root;

/* The RDP's 1-cycle integer path (angrylion's model, which ares'
 * parallel-RDP reproduces): combiner (a - b) c + d, rounded, clamped. */
static int rdp_cc(int a, int b, int c, int d)
{
	int v = ((a - b) * c + (d << 8) + 0x80) >> 8;

	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Blender of an effects mode, one channel: P (combined), a (combined
 * alpha), M (memory, 16-bit framebuffer: 5 bits << 3). The 8-bit sum the
 * RDP keeps (u8: a sum past 255 wraps); -1 for a blender this model does
 * not know. */
static int rdp_blend(int bl, int p, int a, int m)
{
	int q = a >> 3;

	if (bl == GPU_FX_BL_NONE)
		return p;
	if (bl == GPU_FX_BL_ALPHA)
		return (p * q + m * (32 - q)) >> 5;
	return -1;
}

static int c8(unsigned v)
{
	return (int)((v << 3) | (v >> 2));
}

/* A cooked sprite file into buf (bytes read; 0 when missing). */
static size_t load_sc4(const char *name, uint8_t *buf, size_t max)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/sprites/%s.sc4", root, name);
	f = fopen(path, "rb");
	n = f ? fread(buf, 1, max, f) : 0;
	if (f)
		fclose(f);
	return n;
}

static unsigned rd16(const uint8_t *p)
{
	return (unsigned)(p[0] << 8 | p[1]);
}

/* The ramp each cooked sprite carries (sc4-format.md): no wrap through the
 * RDP model for any framebuffer colour, tint and blend; its colours near
 * the TLUT's. */
static void add_cooked(unsigned long *cases, unsigned long *oldwrap, double *dev, double *devold)
{
	static const char *spr[] = { "glow01", "glow02", "glow03", "flare1", "zerogxplode", "fexplo",
		"muzzleflash1", "muzzleflash3", "xffloor", "laserbeam" };
	static const uint8_t tints[3][3] = { { 255, 255, 255 }, { 238, 30, 46 }, { 254, 248, 203 } };
	static uint8_t buf[1 << 16];
	int s, k, i, j;

	for (s = 0; s < (int)(sizeof(spr) / sizeof(spr[0])); s++)
	{
		size_t n = load_sc4(spr[s], buf, sizeof(buf));
		uint16_t tl[16];
		add_ramp_t r;
		double use[16] = { 0 }, err = 0, wsum = 0;
		unsigned ni, im;

		CHECK(n > 88 && rd16(buf + 4) == 3, "%s: no v3 sprite", spr[s]);
		if (n <= 88 || rd16(buf + 4) != 3)
			continue;
		for (i = 0; i < 16; i++)
		{
			tl[i] = (uint16_t)rd16(buf + 16 + 2 * i);
			r.ia[i] = (uint16_t)rd16(buf + 48 + 2 * i);
		}
		memcpy(r.lo, buf + 80, 3);
		memcpy(r.hi, buf + 83, 3);
		/* texel use of every entry over every image (the error's weights) */
		ni = rd16(buf + 10);
		for (im = 0; im < ni; im++)
		{
			const uint8_t *fr = buf + 88 + 16 * im;
			uint32_t ofs = (uint32_t)fr[12] << 24 | (uint32_t)fr[13] << 16 | fr[14] << 8 | fr[15];
			uint32_t b, nb = (uint32_t)fr[8] * fr[9] / 2;

			for (b = 0; b < nb && ofs + b < n; b++)
			{
				use[buf[ofs + b] >> 4]++;
				use[buf[ofs + b] & 15]++;
			}
		}
		for (i = 0; i < 16; i++)
		{
			int c[3] = { c8((tl[i] >> 11) & 31), c8((tl[i] >> 6) & 31), c8((tl[i] >> 1) & 31) };
			int A = r.ia[i] & 255, I = r.ia[i] >> 8;

			if (!(tl[i] & 1))
				CHECK(A == 0, "%s entry %d: transparent, A %d", spr[s], i, A);
			for (k = 0; k < 3; k++)
			{
				/* the ramp's term at full blend and tint, against the colour */
				double v = ((double)r.lo[k] + (r.hi[k] - r.lo[k]) * I / 255.0) * A / 255.0;

				err += use[i] * (v - c[k]) * (v - c[k]);
			}
			wsum += 3 * use[i];
			/* the RDP over every framebuffer colour, tint and blend */
			for (j = 0; j < 3; j++)
			{
				uint8_t prim[4], env[4];
				int bl;

				for (bl = 32; bl <= 256; bl += 32)
				{
					int blend = bl > 255 ? 255 : bl, old = blend > 128 ? 128 : blend, a, m;

					add_ramp_ends(&r, tints[j], (uint8_t)blend, prim, env);
					a = rdp_cc(A, 0, prim[3], 0);
					for (k = 0; k < 3; k++)
					{
						int p = rdp_cc(prim[k], env[k], I, env[k]);
						int ideal = c[k] * tints[j][k] / 255 * blend / 255;
						int cold = c[k] * tints[j][k] / 255;

						for (m = 0; m < 256; m += 8)
						{
							int o = rdp_blend(gpu_fx_rdp[GPU_FX_ADD].blender, p, a, m);
							int sum = ideal + m > 255 ? 255 : ideal + m;
							/* the old path: the adder at blend capped to 128, u8 */
							int ob = (cold * (old >> 3) + m * 32) >> 5;

							CHECK(o >= 0 && o <= 255, "%s entry %d: out %d", spr[s], i, o);
							*oldwrap += ob > 255;
							*dev += fabs((double)o - sum);
							*devold += fabs((double)(ob & 255) - sum);
							(*cases)++;
						}
					}
				}
			}
		}
		err = wsum > 0 ? sqrt(err / wsum) : 0;
		printf("add %s ramp lo %d,%d,%d hi %d,%d,%d rms %.1f\n", spr[s], r.lo[0], r.lo[1],
			r.lo[2], r.hi[0], r.hi[1], r.hi[2], err);
		/* greys exact (8-bit rounding), the rest within two RGBA5551 steps */
		CHECK(err <= (s < 3 ? 2.0 : 16.0), "%s ramp rms %.1f", spr[s], err);
	}
}

/* One sprite entity of res r drawn alone: its mode and ramp ends. */
static int add_draw_one(res_t r, int rm, int amt, const uint8_t col[3], uint8_t prim[4],
	uint8_t env[4], const uint16_t **tlut)
{
	float g[3];
	gpu_fx_vert_t v[4];
	int e, k, mode = -1;

	kill_all();
	for (k = 0; k < 3; k++)
		g[k] = L->start_org[k] + L->gofs[k] + (k == 0 ? 60 : 0);
	e = sprite_ent(g, rm, amt);
	ent_rnd(e)->model = r;
	memcpy(ent_rnd(e)->rendercolor, col, 3);
	for (k = 0; k < 3; k++)
		g[k] = L->start_org[k] + L->gofs[k];
	eye(g, 0, 0);
	draw();
	gpu_fx_host_last(v, 4, &mode);
	*tlut = gpu_fx_host_last_ramp(prim, env);
	kill_all();
	return mode;
}

static void test_add(void)
{
	static const int modes[4] = { GPU_FX_ADD, GPU_FX_GLOW, GPU_FX_BRUSH_ADD, GPU_FX_IALPHA };
	static uint8_t glow[1 << 12] __attribute__((aligned(16)));
	static uint8_t steam[1 << 14] __attribute__((aligned(16)));
	static const uint8_t red[3] = { 238, 30, 46 }, none[3] = { 0, 0, 0 }, grey[3] = { 100, 100, 100 };
	unsigned long cases = 0, oldwrap = 0;
	double dev = 0, devold = 0;
	int k, i;

	begin();
	/* every additive effects mode (and the index-alpha blend): alpha
	 * blender, ramp combiner + IA16 TLUT; streaks: alpha blender (unit
	 * shade colour) */
	for (k = 0; k < 4; k++)
		CHECK(gpu_fx_rdp[modes[k]].blender == GPU_FX_BL_ALPHA
			&& gpu_fx_rdp[modes[k]].combiner == GPU_FX_CC_RAMP
			&& gpu_fx_rdp[modes[k]].tlut == GPU_FX_TLUT_IA16, "mode %s", gpu_fx_mode_name[modes[k]]);
	CHECK(gpu_fx_rdp[GPU_FX_STREAK].blender == GPU_FX_BL_ALPHA, "streak blender");
	add_cooked(&cases, &oldwrap, &dev, &devold);
	printf("add rdp model: %lu cases, 0 wrapped (old capped adder: %lu wrapped, %.1f%%), mean "
		"|out - saturating add| %.1f (old %.1f)\n", cases, oldwrap, cases ? 100.0
		* (double)oldwrap / (double)cases : 0.0, cases ? dev / (double)cases : 0.0,
		cases ? devold / (double)cases : 0.0);
	CHECK(oldwrap > 0, "the model's old adder never wrapped: the model is blind");
	/* untextured / I4 colours: unit colour x brightness, the same term */
	for (i = 0; i < 64; i++)
	{
		uint8_t c[3] = { (uint8_t)(i * 37), (uint8_t)(255 - i * 3), (uint8_t)(i * 11) }, o[4];
		int a = (i * 53) & 255;

		add_split(c, (uint8_t)a, o);
		CHECK(o[0] == 255 || o[1] == 255 || o[2] == 255, "split %d: no unit colour", i);
		for (k = 0; k < 3; k++)
			CHECK(fabs(o[k] * o[3] / 255.0 - c[k] * a / 255.0) <= 2.0, "split %d ch %d", i, k);
	}
	/* the draws bind what the cook stored (render-fx.md 2a, 3): the
	 * rendercolor tint of a glow (hldemo1's red glow02), untinted at
	 * 0 0 0; an index-alpha sprite (steam1: alpha = palette index, colour
	 * = its last palette colour) through its level TLUT */
	{
		size_t ng = load_sc4("glow02", glow, sizeof(glow)), ns = load_sc4("steam1", steam, sizeof(steam));
		res_t rg = RES_MAKE(RES_K_SPRITE, 8001), rs = RES_MAKE(RES_K_SPRITE, 8002);
		const fx_sprite_t *sg, *ss;
		const uint16_t *tl;
		uint8_t prim[4], env[4];
		int m;

		CHECK(ng > 88 && !sprite_test_inject(rg, glow, (uint32_t)ng), "glow02 inject");
		CHECK(ns > 88 && !sprite_test_inject(rs, steam, (uint32_t)ns), "steam1 inject");
		sg = sprite_get(rg);
		ss = sprite_get(rs);
		CHECK(sg && ss && ss->texformat == FX_SPR_TEX_INDEXALPHA, "cooked sprites");
		if (sg && ss)
		{
			m = add_draw_one(rg, FX_RM_GLOW, 255, red, prim, env, &tl);
			printf("add glow02 red: mode %s prim %d,%d,%d,%d\n", m >= 0 ? gpu_fx_mode_name[m] : "-",
				prim[0], prim[1], prim[2], prim[3]);
			CHECK(m == GPU_FX_GLOW && tl == sg->ramp->ia, "glow02 red: mode %d", m);
			for (k = 0; k < 3; k++)
				CHECK(prim[k] == (sg->ramp->hi[k] * red[k] + 127) / 255
					&& env[k] == (sg->ramp->lo[k] * red[k] + 127) / 255, "glow02 red ch %d", k);
			m = add_draw_one(rg, FX_RM_GLOW, 255, none, prim, env, &tl);
			CHECK(m == GPU_FX_GLOW && prim[0] == sg->ramp->hi[0] && prim[1] == sg->ramp->hi[1]
				&& prim[2] == sg->ramp->hi[2], "glow02 at 0 0 0 not untinted: %d,%d,%d", prim[0],
				prim[1], prim[2]);
			for (k = 0; k < 16; k++)
				CHECK(ss->ramp->ia[k] == (0xFF00 | 17 * k), "steam1 level %d: %04x", k,
					ss->ramp->ia[k]);
			/* smoke (fx_smoke: TransTexture, grey): alpha-blended through
			 * the level TLUT, colour = last palette colour x tint */
			m = add_draw_one(rs, FX_RM_TRANSTEXTURE, 200, grey, prim, env, &tl);
			printf("add steam1 smoke: mode %s prim %d,%d,%d,%d\n", m >= 0 ? gpu_fx_mode_name[m] : "-",
				prim[0], prim[1], prim[2], prim[3]);
			CHECK(m == GPU_FX_IALPHA && tl == ss->ramp->ia && prim[3] == 200, "steam1 smoke: mode %d",
				m);
			for (k = 0; k < 3; k++)
				CHECK(prim[k] == (ss->ramp->hi[k] * grey[k] + 127) / 255 && env[k] == prim[k],
					"steam1 smoke ch %d: %d", k, prim[k]);
			/* TransColor sprite (render-fx.md 2): as Normal, untinted,
			 * renderamt only a visibility switch */
			m = add_draw_one(rs, FX_RM_TRANSCOLOR, 60, red, prim, env, &tl);
			printf("add steam1 transcolor: mode %s prim %d,%d,%d,%d\n",
				m >= 0 ? gpu_fx_mode_name[m] : "-", prim[0], prim[1], prim[2], prim[3]);
			CHECK(m == GPU_FX_IALPHA && tl == ss->ramp->ia && prim[3] == 255
				&& prim[0] == ss->ramp->hi[0] && prim[1] == ss->ramp->hi[1]
				&& prim[2] == ss->ramp->hi[2], "steam1 TransColor not untinted / full: mode %d", m);
			{
				gpu_fx_stats_t gs;

				add_draw_one(rs, FX_RM_TRANSCOLOR, 0, red, prim, env, &tl);
				gpu_fx_stats(&gs);
				CHECK(!gs.calls[GPU_FX_IALPHA] && !gs.calls[GPU_FX_CUTOUT],
					"TransColor at amount 0 drawn");
			}
			/* additive: unit colour, brightness x blend (2a) */
			m = add_draw_one(rs, FX_RM_TRANSADD, 200, none, prim, env, &tl);
			CHECK(m == GPU_FX_ADD && tl == ss->ramp->ia, "steam1 add: mode %d", m);
			{
				uint8_t o[4];

				add_split(ss->ramp->hi, 200, o);
				CHECK(!memcmp(prim, o, 4), "steam1 add ends %d,%d,%d,%d", prim[0], prim[1], prim[2],
					prim[3]);
			}
		}
	}
	finish("ADD", "cooked additive ramps, RDP blender model: no wrap, split; glow rendercolor tint, "
		"index-alpha levels (render-fx.md 2a, 3)");
}

/* ---- PLACED: infodecal and env_glow of one map (w64-format.md "DECL") ---- */

/* A New Game on map (logic records and those near the start spawn as in a
 * route), every other infodecal / env_glow record woken, the engine run:
 * each targetless infodecal sprays its decal (map-sheet ids from the
 * map's DECL slot) and is removed, each env_glow lives. */
static int test_placed(const char *name)
{
	int map = wld_map_find(name), slot, lump, f;
	int decals = 0, statics = 0, local = 0, triggered = 0, glows = 0, live = 0, left = 0;
	decal_stats_t ds;
	fx_stats_t fs;
	uint32_t base;
	char note[320];

	begin();
	if (map < 0)
	{
		printf("PLACED FAIL %s: no such map\n", name);
		return 1;
	}
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	game_new_campaign();
	rec_new_campaign(map);
	slot = wld_open(map)->slot;
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		cls_t c;

		if (rec_kind(gid) < 0)
			continue;
		c = rec_class(gid);
		if (c != RC_INFODECAL && c != RC_ENV_GLOW)
			continue;
		if (c == RC_INFODECAL)
		{
			int d = rec_key_decal(gid, RK_TEXTURE);

			decals++;
			if (d < 0)
				continue;	/* not in decals.wad: refused */
			local += d >= 42;
			if (rec_targetname(gid) == NAME_NONE)
				statics++;
			else
				triggered++;
		}
		else
			glows++;
		if (rec_entity(gid) < 0 && (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY))
			rec_wake(gid);
	}
	decal_stats(&ds, 1);
	base = ds.region_peak;
	fx_stats(&fs, 1);
	for (f = 0; f < 900; f++)
	{
		eng_frame();
		decal_stats(&ds, 0);
		if (ds.statics >= statics)
			break;
	}
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		int e;

		if (rec_kind(gid) < 0)
			continue;
		e = rec_entity(gid);
		if (rec_class(gid) == RC_ENV_GLOW)
			live += e >= 0 && RES_KIND(ent_rnd(e)->model) == RES_K_SPRITE;
		else if (rec_class(gid) == RC_INFODECAL && rec_targetname(gid) == NAME_NONE && e >= 0)
			left++;
	}
	decal_stats(&ds, 0);
	CHECK(ds.statics == statics, "%s: %d of %d targetless infodecals placed (%d frames, nosheet %d)",
		name, ds.statics, statics, f, ds.nosheet);
	CHECK(!left, "%s: %d infodecals not removed after spraying", name, left);
	CHECK(live == glows, "%s: %d of %d env_glow live with a sprite", name, live, glows);
	CHECK(decal_count() >= statics, "%s: %d decals live", name, decal_count());
	CHECK(!local || ds.region_peak > base, "%s: decal region peak %lu B misses the map sheet "
		"(boot %lu B)", name, (unsigned long)ds.region_peak, (unsigned long)base);
	/* the targeted ones spray when used (TriggerDecal), gone 0.1 s on */
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		int e;

		if (rec_kind(gid) < 0 || rec_class(gid) != RC_INFODECAL || rec_targetname(gid) == NAME_NONE
			|| (e = rec_entity(gid)) < 0)
			continue;
		eng_set_frame_layer(slot);
		game_use(e, e, e, 3 /* USE_TOGGLE */, 0);
		eng_set_frame_layer(-1);
	}
	for (lump = 0; lump < 60; lump++)	/* 2 s */
		eng_frame();
	left = 0;
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);

		if (rec_kind(gid) >= 0 && rec_class(gid) == RC_INFODECAL && rec_entity(gid) >= 0)
			left++;
	}
	decal_stats(&ds, 0);
	CHECK(ds.statics == statics + triggered, "%s: %d of %d triggered infodecals placed", name,
		ds.statics - statics, triggered);
	CHECK(!left, "%s: %d infodecals left after use", name, left);
	/* the effects time roll-up: max is the worst frame of what avg sums */
	fx_stats(&fs, 0);
	CHECK(fs.frames && (uint64_t)fs.us_max * fs.frames >= fs.us
		&& fs.us == fs.sim_us + fs.part_us + fs.trans_us + fs.decal_us,
		"%s: fx_us sum %lu over %lu frames, max %lu (sim %lu part %lu trans %lu decal %lu)", name,
		(unsigned long)fs.us, (unsigned long)fs.frames, (unsigned long)fs.us_max,
		(unsigned long)fs.sim_us, (unsigned long)fs.part_us, (unsigned long)fs.trans_us,
		(unsigned long)fs.decal_us);
	snprintf(note, sizeof(note), "%s: infodecal %d (%d targetless placed in %d frames, %d map-sheet, "
		"%d of %d triggered placed on use), env_glow %d/%d live, decals live %d, region peak %lu B "
		"(boot %lu), fx_us avg %lu max %lu", name, decals, statics, f, local,
		ds.statics - statics, triggered, live, glows, decal_count(), (unsigned long)ds.region_peak,
		(unsigned long)base, (unsigned long)(fs.us / (fs.frames ? fs.frames : 1)),
		(unsigned long)fs.us_max);
	finish("PLACED", note);
	return fails != 0;
}

int main(int argc, char **argv)
{
	int map;

	if (argc == 5 && !strcmp(argv[1], "--root") && !strcmp(argv[3], "--placed"))
	{
		host_io_init(argv[2]);
		eng_boot("host");
		return test_placed(argv[4]);
	}
	if (argc != 3 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-fx --root CONTENT [--placed MAP]\n");
		return 2;
	}
	host_io_init(argv[2]);
	root = argv[2];
	mem_boot_carve();
	settings_reset();
	io_init();
	ent_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-fx: no content in %s\n", argv[2]);
		return 2;
	}
	res_init();
	col_init();
	ren_init();
	studio_init();
	fx_init();
	map = wld_start_map();
	L = wld_open(map);
	eng_set_frame_layer(-1);
	/* the sectors the start leaf sees and the movers resident (the decal
	 * tests need their chunks): request, pump the reads */
	{
		const uint16_t *list;
		int cnt, i, it, missing = 1;

		wld_want_open(L, wld_point_leaf(L, L->start_org));
		list = wld_leaf_sectors(L, wld_point_leaf(L, L->start_org), &cnt);
		for (it = 0; it < 4000 && missing; it++)
		{
			missing = 0;
			io_frame_begin(33333);
			for (i = 0; i < cnt; i++)
				missing |= !wld_chunk_use(L, L->sects[list[i]].rend_chunk);
			for (i = 0; i < L->nmovers && i < 8; i++)
				if (L->subs[L->movers[i].sub].kind == W64_SUB_MOVR)
					missing |= !wld_chunk_use(L, L->subs[L->movers[i].sub].chunk);
			io_pump();
			wld_frame(wld_fr++);
		}
		if (missing)
			printf("setup: start sectors not resident after %d frames\n", it);
	}
	printf("fx tests on %s (layer %d)\n", wld_map_name(map), L->slot);
	test_x1();
	test_x2();
	test_x3();
	test_x4();
	test_x5();
	test_x6();
	test_screen();
	test_dlights();
	test_styles();
	test_add();
	printf(fails ? "fx FAIL (%d)\n" : "fx ok\n", fails);
	return fails ? 1 : 0;
}
