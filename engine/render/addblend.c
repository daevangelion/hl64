/* SPDX-License-Identifier: MIT */
/*
 * addblend.c -- additive draws without the RDP adder's wrap
 * (docs/specs/render-fx.md 2a; the interface note is addblend.h).
 *
 * Why: the RDP blender sums in x a + mem x b in 8 bits per channel and
 * keeps the low 8 bits (no clamp). An additive item over a surface already
 * brighter than 255 - c turns black or a false colour (measured on ares,
 * whose parallel-RDP blender is the angrylion integer model: (P a0 +
 * M (b0 + 1)) >> 5, truncated to u8). Capping the blend cannot help: the
 * framebuffer can already be at 255.
 *
 * What: out = n x a + mem x (1 - a) through the alpha blender, n a unit
 * colour, a its brightness; (n a0 + M (32 - a0)) >> 5 <= 255 for any a0,
 * n, M, so no wrap is possible. The source term n x a is GoldSrc's c.
 *
 * Ramp fit: the unit colours of the lit TLUT entries are the candidate
 * ends; a pair (lo, hi) (one colour when lo = hi) scores the sum over lit
 * entries of brightness x the squared residual of the best non-negative
 * x lo + y hi ~ c (two unknowns, normal equations; on the edge of the
 * cone, one end alone). Search: the best single end, then alternately the
 * best hi for lo held and the best lo for hi held until neither moves;
 * each score stops once past the best so far. On the 27 cooked sprites it
 * picks the same pair as trying every pair, and that pair is within 0.5
 * (8-bit units, usage-weighted rms) of the best usage-weighted pair except
 * muzzleflash3 (12.3 against 7.1); greys and single hues are exact, the
 * fire sprites 6-11 (one RGBA5551 step is 8). The ramp's A = x + y
 * (scaled down to 1 when above), I = y / (x + y).
 */
#include <string.h>

#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "engine/render/addblend.h"
#include "engine/render/gpu.h"

#define FENCE	2	/* frames the GPU may still read a TLUT after its use */

typedef struct {
	add_ramp_t	r;	/* first: 8-aligned */
	uint16_t	key[16];	/* the page's RGBA5551 TLUT */
	uint32_t	used;	/* frame of the last use; 0 = free */
} page_t;

static page_t		pages[HL64_FX_ADD_PAGES] __attribute__((aligned(8)));
static uint32_t		frame = 1;
static add_stats_t	st;

/* 5-bit channel to 8 bits as the RDP expands an RGBA5551 texel */
static int c8(unsigned v)
{
	return (int)((v << 3) | (v >> 2));
}

static int byte(float v)
{
	return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f);
}

typedef struct {
	float	c[16][3], w[16], cc[16];	/* lit entries: colour (0..1), brightness, |c|^2 */
	float	cn[16][16];		/* lit entry . candidate */
	float	nn[16][16], inv[16];	/* candidate . candidate, 1 / |n|^2 */
	uint8_t	q[16][3];		/* candidate unit colours, 8-bit */
	int	nl, nc, ent[16];	/* lit entry -> TLUT index */
} fit_t;

/* 1 / det of the pair's normal equations; 0 when the ends are parallel */
static float idet(const fit_t *f, int a, int b)
{
	float det = f->nn[a][a] * f->nn[b][b] - f->nn[a][b] * f->nn[a][b];

	return det > 1e-4f * f->nn[a][a] * f->nn[b][b] ? 1.0f / det : 0;
}

/* Best non-negative x n_a + y n_b ~ c_j; returns the squared residual. */
static float fit2(const fit_t *f, int j, int a, int b, float id, float *x, float *y)
{
	float cu = f->cn[j][a], cv = f->cn[j][b], uv = f->nn[a][b], s, e, e2;

	if (id > 0)
	{
		float p = (cu * f->nn[b][b] - cv * uv) * id, q = (cv * f->nn[a][a] - cu * uv) * id;

		if (p >= 0 && q >= 0)
		{
			*x = p;
			*y = q;
			return f->cc[j] - p * cu - q * cv;
		}
	}
	s = cu > 0 ? cu * f->inv[a] : 0;
	e = f->cc[j] - s * cu;
	*x = s;
	*y = 0;
	s = cv > 0 ? cv * f->inv[b] : 0;
	e2 = f->cc[j] - s * cv;
	if (e2 < e)
	{
		*x = 0;
		*y = s;
		e = e2;
	}
	return e;
}

/* The pair's brightness-weighted error, abandoned once past lim. */
static float score(const fit_t *f, int a, int b, float lim)
{
	float id = idet(f, a, b), e = 0, x, y;
	int j;

	for (j = 0; j < f->nl && e < lim; j++)
		e += f->w[j] * fit2(f, j, a, b, id, &x, &y);
	return e;
}

static float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void add_ramp_fit(add_ramp_t *r, const uint16_t tlut[16])
{
	static fit_t f;
	float n[16][3], best = 1e30f, e, id;
	int i, j, k, a, lo = 0, hi = 0, moved;

	memset(r, 0, sizeof(*r));
	f.nl = f.nc = 0;
	for (i = 0; i < 16; i++)
	{
		unsigned t = tlut[i];
		int c[3] = { c8((t >> 11) & 31), c8((t >> 6) & 31), c8((t >> 1) & 31) };
		int m = c[0] > c[1] ? c[0] : c[1];
		uint8_t q[3];

		m = m > c[2] ? m : c[2];
		if (!(t & 1) || !m)
			continue;
		j = f.nl++;
		f.ent[j] = i;
		f.w[j] = (float)m / 255.0f;
		for (k = 0; k < 3; k++)
		{
			f.c[j][k] = (float)c[k] / 255.0f;
			q[k] = (uint8_t)((c[k] * 255 + m / 2) / m);
		}
		f.cc[j] = dot(f.c[j], f.c[j]);
		/* a candidate end unless an earlier one has the same unit colour */
		for (a = 0; a < f.nc && memcmp(f.q[a], q, 3); a++)
			;
		if (a == f.nc)
		{
			memcpy(f.q[a], q, 3);
			for (k = 0; k < 3; k++)
				n[a][k] = (float)c[k] / (float)m;
			f.nc++;
		}
	}
	if (!f.nl)
		return;
	for (a = 0; a < f.nc; a++)
	{
		for (k = 0; k < f.nc; k++)
			f.nn[a][k] = dot(n[a], n[k]);
		f.inv[a] = 1.0f / f.nn[a][a];
		for (j = 0; j < f.nl; j++)
			f.cn[j][a] = dot(f.c[j], n[a]);
	}
	for (a = 0; a < f.nc; a++)
		if ((e = score(&f, a, a, best)) < best)
		{
			best = e;
			lo = hi = a;
		}
	do
	{
		moved = 0;
		for (a = 0; a < f.nc; a++)
			if ((e = score(&f, lo, a, best)) < best)
			{
				best = e;
				hi = a;
				moved = 1;
			}
		for (a = 0; a < f.nc; a++)
			if ((e = score(&f, a, hi, best)) < best)
			{
				best = e;
				lo = a;
				moved = 1;
			}
	} while (moved);
	memcpy(r->lo, f.q[lo], 3);
	memcpy(r->hi, f.q[hi], 3);
	id = idet(&f, lo, hi);
	for (j = 0; j < f.nl; j++)
	{
		float x, y, s;

		fit2(&f, j, lo, hi, id, &x, &y);
		s = x + y;
		r->ia[f.ent[j]] = (uint16_t)(byte(s > 0 ? y / s : 0) << 8 | byte(s));
	}
}

void add_ramp_ends(const add_ramp_t *r, const uint8_t tint[3], uint8_t blend, uint8_t prim[4],
	uint8_t env[4])
{
	int k;

	for (k = 0; k < 3; k++)
	{
		prim[k] = (uint8_t)((r->hi[k] * tint[k] + 127) / 255);
		env[k] = (uint8_t)((r->lo[k] * tint[k] + 127) / 255);
	}
	prim[3] = blend;
	env[3] = 255;
}

void add_split(const uint8_t rgb[3], uint8_t a, uint8_t out[4])
{
	int m = rgb[0] > rgb[1] ? rgb[0] : rgb[1], k;

	m = m > rgb[2] ? m : rgb[2];
	if (!m)
	{
		memset(out, 0, 4);
		return;
	}
	for (k = 0; k < 3; k++)
		out[k] = (uint8_t)((rgb[k] * 255 + m / 2) / m);
	out[3] = (uint8_t)((m * a + 127) / 255);
}

/* ---- page ramps -------------------------------------------------------------- */

void add_ramp_make(add_ramp_t *r, const uint16_t tlut[16])
{
	uint64_t t0 = sys_time_us();
	uint32_t us;

	add_ramp_fit(r, tlut);
	gpu_writeback(r->ia, sizeof(r->ia));
	us = (uint32_t)(sys_time_us() - t0);
	st.fits++;
	st.fit_us_sum += us;
	if (us > st.fit_us_max)
		st.fit_us_max = us;
}

const add_ramp_t *add_ramp_page(const uint16_t tlut[16])
{
	page_t *p, *old = NULL;
	int i;

	for (i = 0; i < HL64_FX_ADD_PAGES; i++)
	{
		p = &pages[i];
		if (p->used && !memcmp(p->key, tlut, sizeof(p->key)))
		{
			p->used = frame;
			st.hits++;
			return &p->r;
		}
		if ((!p->used || p->used + FENCE < frame) && (!old || p->used < old->used))
			old = p;
	}
	if (!old)
	{
		st.full++;
		return NULL;
	}
	memcpy(old->key, tlut, sizeof(old->key));
	add_ramp_make(&old->r, tlut);
	old->used = frame;
	return &old->r;
}

void add_frame(void)
{
	frame++;
}

void add_stats(add_stats_t *s)
{
	*s = st;
}
