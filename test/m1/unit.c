/* SPDX-License-Identifier: MIT */
/*
 * unit.c -- host unit tests of the M1 runtime rules, self-contained
 * fixtures from the specs:
 *   W4  texture animation walk (docs/specs/world-surfaces.md 5)
 *   L1  letter values, unset 256; L2 pattern phase; L3 no change between
 *       ticks / for unchanged values (docs/specs/lightstyles.md 5)
 *   D1  dlight pool reuse and decay; D2 world falloff with the minlight
 *       cutoff (docs/specs/dlights.md 5)
 * Exit 0 when every check passes; each failure prints one line.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/core/sys.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* W4: a 3-frame sequence (textures 0, 1, 2) with a 2-frame alternate
 * sequence (3, 4); world-surfaces.md 4 cooking rules. */
static void test_w4(void)
{
	w64_tex_t t[5];
	static const double tm[5] = { 1.0, 1.15, 1.2, 1.45, 1.6 };
	static const int want[5] = { 2, 2, 0, 1, 2 };
	int i, s;

	memset(t, 0, sizeof(t));
	for (i = 0; i < 3; i++)
	{
		t[i].flags = W64_TEXF_ANIM;
		t[i].anim_total = 6;
		t[i].anim_min = (uint8_t)(2 * i);
		t[i].anim_max = (uint8_t)(2 * i + 2);
		t[i].anim_next = (uint16_t)((i + 1) % 3);
		t[i].alternate = 3;
	}
	for (i = 0; i < 2; i++)
	{
		t[3 + i].flags = W64_TEXF_ANIM;
		t[3 + i].anim_total = 4;
		t[3 + i].anim_min = (uint8_t)(2 * i);
		t[3 + i].anim_max = (uint8_t)(2 * i + 2);
		t[3 + i].anim_next = (uint16_t)(3 + (i + 1) % 2);
		t[3 + i].alternate = 0;
	}
	for (i = 0; i < 5; i++)
		for (s = 0; s < 3; s++)
		{
			uint32_t r10 = (uint32_t)(tm[i] * 10.0);
			int got = ren_tex_anim(t, 5, s, 0, r10);

			CHECK(got == want[i], "W4 t=%.2f from frame %d: frame %d, want %d", tm[i], s,
				got, want[i]);
		}
	/* frame != 0: the alternate sequence, same clock (r = 12 mod 4 = 0) */
	CHECK(ren_tex_anim(t, 5, 1, 1, 12) == 3, "W4 alternate at r10=12");
	CHECK(ren_tex_anim(t, 5, 1, 1, 14) == 4, "W4 alternate at r10=14");
	/* a texture without animation flags is itself */
	t[2].flags = 0;
	CHECK(ren_tex_anim(t, 5, 2, 1, 7) == 2, "W4 plain texture");
}

static void test_styles(void)
{
	static const uint8_t az[26] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		16, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
	static const uint8_t abc[3] = { 0, 1, 2 };
	static const double tm[5] = { 1.0, 1.05, 1.15, 1.25, 1.35 };
	static const int want[5] = { 1, 1, 2, 0, 1 };
	uint32_t e;
	int i;

	wld_style_reset();
	CHECK(wld_style_value(5) == 256, "L1 unset value %u", wld_style_value(5));
	/* L1: letter k at index k is k x 22 */
	wld_style_set(1, az, 26);
	for (i = 0; i < 26; i++)
	{
		wld_style_tick(i * 0.1 + 0.01);
		CHECK(wld_style_value(1) == i * 22, "L1 letter %c = %u", 'a' + i, wld_style_value(1));
	}
	wld_style_set(1, az, 0);
	CHECK(wld_style_value(1) == 256, "L1 unset after set(0)");

	/* L2: "abc" at 1.0, 1.05, 1.15, 1.25, 1.35 -> b, b, c, a, b */
	wld_style_reset();
	wld_style_set(2, abc, 3);
	for (i = 0; i < 5; i++)
	{
		wld_style_tick(tm[i]);
		CHECK(wld_style_value(2) == want[i] * 22, "L2 t=%.2f value %u want %d", tm[i],
			wld_style_value(2), want[i] * 22);
	}

	/* L3: no change between ticks or for an unchanged value */
	wld_style_reset();
	wld_style_set(3, abc, 1);	/* "a": constant */
	wld_style_tick(2.0);
	e = wld_style_epoch();
	wld_style_tick(2.05);
	wld_style_tick(2.3);
	CHECK(wld_style_epoch() == e, "L3 epoch moved without a value change");
	wld_style_set(4, abc, 3);
	e = wld_style_epoch();
	wld_style_tick(2.31);	/* same tick */
	CHECK(wld_style_epoch() == e, "L3 epoch moved inside a tick");
	wld_style_tick(2.4);
	CHECK(wld_style_epoch() == e + 1 && wld_style_changed(4) == e + 1
		&& wld_style_changed(3) < e + 1, "L3 change stamp");
}

static void test_dlights(void)
{
	ren_dlight_t *a, *b, *c;
	int i, n;

	/* D1: key reuse, first dead reuse, decay to death, one-frame lights
	 * (game clock, f64 s) */
	for (i = 0; i < 32; i++)
	{
		a = ren_dlight_alloc(0, 1.0);
		a->radius = 100;
		a->die = 1.5;
	}
	a = ren_dlight_alloc(7, 1.25);	/* all live at 1.25: light 0 */
	a->radius = 200;
	a->die = 10.0;
	CHECK(a == ren_dlight(0), "D1 no dead light: light 0");
	b = ren_dlight_alloc(7, 1.25);
	CHECK(b == a, "D1 key reuse");
	b->radius = 200;
	b->die = 10.0;
	c = ren_dlight_alloc(0, 2.0);	/* 2.0: lights 1.. are past die */
	CHECK(c == ren_dlight(1), "D1 first dead light");
	c->radius = 50;
	c->decay = 100;
	c->die = 6.0;
	ren_dlight_frame(2.0, 0.25);	/* 0.25 s: 50 - 25 */
	CHECK(fabsf(c->radius - 25) < 1e-3f, "D1 decay radius %g", (double)c->radius);
	ren_dlight_frame(2.25, 0.25);
	CHECK(!ren_dlight_live(1), "D1 decayed to death");
	a = ren_dlight_alloc(9, 3.0);
	a->radius = 80;
	a->die = 3.0;	/* this frame only */
	ren_dlight_frame(3.0, 1.0 / 30);
	CHECK(ren_dlight_live((int)(a - ren_dlight(0))), "D1 one-frame light live in its frame");
	ren_dlight_frame(3.0 + 1.0 / 30, 1.0 / 30);
	CHECK(!ren_dlight_live((int)(a - ren_dlight(0))), "D1 one-frame light gone next frame");
	for (i = n = 0; i < 32; i++)
		n += ren_dlight_live(i);
	CHECK(n == 1, "D1 live lights %d (key 7 only)", n);

	/* D2: (radius - |p|) - q above minlight */
	CHECK(fabsf(ren_dlight_falloff(200, 0, 50, 30) - 120) < 1e-4f, "D2 front");
	CHECK(fabsf(ren_dlight_falloff(200, 0, -50, 30) - 120) < 1e-4f, "D2 |p|");
	CHECK(ren_dlight_falloff(200, 0, 50, 160) == 0, "D2 beyond reach");
	CHECK(ren_dlight_falloff(200, 32, 170, 0) == 0, "D2 minlight cutoff");
	CHECK(fabsf(ren_dlight_falloff(200, 32, 168, 0) - 32) < 1e-4f, "D2 at minlight");
}

int main(void)
{
	test_w4();
	test_styles();
	test_dlights();
	printf("unit: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
