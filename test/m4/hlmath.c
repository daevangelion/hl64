/* SPDX-License-Identifier: MIT */
/*
 * hlmath.c -- accuracy driver of engine/core/hlmath.c (run by
 * test/m4/hlmath.py). Every function against the host's long-double libm
 * (x87 80-bit: sinl, atan2l ...) as the reference, over the angles the
 * engine feeds them (every 1/16 degree of +-4 turns, in double and as the
 * float the call sites form), random arguments of the reduced and the
 * large range, near multiples of pi / 2, tiny arguments, and the C99
 * special cases. Prints one `max <fn> <ulp> at <x>` line per function
 * and `hlmath PASS` when every max is within its bound.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine/core/hlmath.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* bounds (ulp of the reference rounded to the function's type): the
 * double functions as hlmath.h states them; the float ones are the
 * double ones rounded once (measured 0.5: correctly rounded on every
 * argument here) */
#define BOUND_D		1.0	/* sin, cos */
#define BOUND_DA	1.5	/* atan, atan2: the quotient's rounding */
#define BOUND_DS	2.0	/* asin, acos: + sqrt((1 - x)(1 + x)) */
#define BOUND_DT	2.5	/* tan: sin / cos */
#define BOUND_F		1.0

typedef struct
{
	const char	*name;
	double		max;
	double		at_x, at_y;
	long		n;
} acc_t;

static int fails;

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t next_u64(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

/* uniform in [lo, hi) */
static double uni(double lo, double hi)
{
	return lo + (hi - lo) * ((double)(next_u64() >> 11) * 0x1p-53);
}

/* ulp of v rounded to a type with `mant` mantissa bits (53 / 24),
 * the subnormal floor at `emin` */
static long double ulp_of(long double v, int mant, int emin)
{
	int e;

	if (v == 0)
		return ldexpl(1.0L, emin - mant + 1);
	frexpl(v, &e);
	if (e - 1 < emin)
		e = emin + 1;
	return ldexpl(1.0L, e - mant);
}

static void acc(acc_t *a, long double got, long double ref, double x, double y, int is_float)
{
	long double u = is_float ? ulp_of((float)ref, 24, -126) : ulp_of((double)ref, 53, -1022);
	double err;

	a->n++;
	if (isnan(ref) || isnan(got))
	{
		if (isnan(ref) != isnan(got))
		{
			printf("FAIL %s(%a, %a): got %La, want %La\n", a->name, x, y, got, ref);
			fails++;
		}
		return;
	}
	if (isinf(ref) || isinf(got))
	{
		if (got != ref)
		{
			printf("FAIL %s(%a, %a): got %La, want %La\n", a->name, x, y, got, ref);
			fails++;
		}
		return;
	}
	err = (double)(fabsl(got - ref) / u);
	if (err > a->max)
	{
		a->max = err;
		a->at_x = x;
		a->at_y = y;
	}
}

static void report(acc_t *a, double bound)
{
	printf("max %-8s %.3f ulp at (%a, %a) over %ld args\n", a->name, a->max, a->at_x, a->at_y, a->n);
	if (!(a->max <= bound))
	{
		printf("FAIL %s: %.3f ulp > %.1f\n", a->name, a->max, bound);
		fails++;
	}
}

static acc_t a_sin = { "sin", 0, 0, 0, 0 }, a_cos = { "cos", 0, 0, 0, 0 },
	a_tan = { "tan", 0, 0, 0, 0 }, a_atan = { "atan", 0, 0, 0, 0 },
	a_atan2 = { "atan2", 0, 0, 0, 0 }, a_asin = { "asin", 0, 0, 0, 0 },
	a_acos = { "acos", 0, 0, 0, 0 };
static acc_t f_sin = { "sinf", 0, 0, 0, 0 }, f_cos = { "cosf", 0, 0, 0, 0 },
	f_tan = { "tanf", 0, 0, 0, 0 }, f_atan = { "atanf", 0, 0, 0, 0 },
	f_atan2 = { "atan2f", 0, 0, 0, 0 }, f_asin = { "asinf", 0, 0, 0, 0 },
	f_acos = { "acosf", 0, 0, 0, 0 };

/* sin / cos at x (both types; tan away from its poles, where the
 * reference itself is ill-conditioned); hl_sincos / hl_sincosf give the
 * bits of hl_sin, hl_cos / hl_sinf, hl_cosf */
static void trig(double x)
{
	float xf = (float)x, sf, cf;
	long double cl = cosl(x), cfl = cosl(xf);
	double s, c, ws = hl_sin(x), wc = hl_cos(x);
	float wsf = hl_sinf(xf), wcf = hl_cosf(xf);

	acc(&a_sin, ws, sinl(x), x, 0, 0);
	acc(&a_cos, wc, cl, x, 0, 0);
	acc(&f_sin, wsf, sinl(xf), xf, 0, 1);
	acc(&f_cos, wcf, cfl, xf, 0, 1);
	hl_sincos(x, &s, &c);
	hl_sincosf(xf, &sf, &cf);
	if (memcmp(&s, &ws, sizeof(s)) || memcmp(&c, &wc, sizeof(c))
		|| memcmp(&sf, &wsf, sizeof(sf)) || memcmp(&cf, &wcf, sizeof(cf)))
	{
		if (fails < 20)
			printf("FAIL sincos(%a) != sin, cos\n", x);
		fails++;
	}
	if (fabsl(cl) > 1e-3L)
		acc(&a_tan, hl_tan(x), tanl(x), x, 0, 0);
	if (fabsl(cfl) > 1e-3L)
		acc(&f_tan, hl_tanf(xf), tanl(xf), xf, 0, 1);
}

static void at2(double y, double x)
{
	float yf = (float)y, xf = (float)x;

	acc(&a_atan2, hl_atan2(y, x), atan2l(y, x), y, x, 0);
	acc(&f_atan2, hl_atan2f(yf, xf), atan2l(yf, xf), yf, xf, 1);
}

static void at1(double x)
{
	float xf = (float)x;

	acc(&a_atan, hl_atan(x), atanl(x), x, 0, 0);
	acc(&f_atan, hl_atanf(xf), atanl(xf), xf, 0, 1);
}

static void asc(double x)
{
	float xf = (float)x;

	acc(&a_asin, hl_asin(x), asinl(x), x, 0, 0);
	acc(&a_acos, hl_acos(x), acosl(x), x, 0, 0);
	acc(&f_asin, hl_asinf(xf), asinl(xf), xf, 0, 1);
	acc(&f_acos, hl_acosf(xf), acosl(xf), xf, 0, 1);
}

/* exact results the C99 annex F names (sign of zero included) */
static void special(const char *what, double got, double want)
{
	if (memcmp(&got, &want, sizeof got) != 0 && !(isnan(got) && isnan(want)))
	{
		printf("FAIL special %s: got %a, want %a\n", what, got, want);
		fails++;
	}
}

int main(void)
{
	const double inf = HUGE_VAL;
	double big_err = 0, big_x = 0;
	long i;
	int k;

	/* the call sites' angles: degrees x pi / 180 (double) and
	 * degrees x (float)(pi / 180) (float), every 1/16 degree */
	for (i = -1440 * 16; i <= 1440 * 16; i++)
	{
		double deg = i / 16.0;

		trig(deg * (M_PI / 180.0));
		trig((double)((float)deg * (float)(M_PI / 180.0)));
		trig(deg * (M_PI * 2 / 360) * 0.5);
	}
	for (i = 0; i < 400000; i++)
		trig(uni(-4 * M_PI, 4 * M_PI));
	for (i = 0; i < 100000; i++)
		trig(uni(-0x1p19, 0x1p19));
	/* beyond 2^19 (taken modulo the double 2 pi): absolute error only */
	for (i = 0; i < 20000; i++)
	{
		double x = (i & 1) ? uni(0x1p19, 1e7) : -uni(0x1p19, 1e7);
		double e = fabs(hl_sin(x) - (double)sinl(x)) + fabs(hl_cos(x) - (double)cosl(x));

		if (e > big_err)
			big_err = e, big_x = x;
	}
	/* near multiples of pi / 2 */
	for (k = -2000; k <= 2000; k++)
	{
		double m = k * (M_PI / 2);
		int d;

		for (d = -4; d <= 4; d++)
			trig(m + d * 0x1p-52 * (fabs(m) + 1));
		trig(nextafter(m, inf));
		trig(nextafter(m, -inf));
	}
	for (i = 0; i < 2000; i++)
		trig(ldexp(uni(-1, 1), -(int)(i % 60)));
	/* atan2: random directions, mixed magnitudes, both signs */
	for (i = 0; i < 400000; i++)
	{
		double y = uni(-1, 1) * ldexp(1.0, (int)(next_u64() % 40) - 20);
		double x = uni(-1, 1) * ldexp(1.0, (int)(next_u64() % 40) - 20);

		at2(y, x);
		at1(y / (x != 0 ? x : 1));
	}
	for (i = -4096; i <= 4096; i++)
	{
		double a = i * (M_PI / 2048);

		at2(sin(a), cos(a));
		at2(4096.0 * sin(a), 4096.0 * cos(a));
	}
	for (i = 0; i <= 8192; i++)
		at1(i / 1024.0 - 4.0);
	/* asin / acos over [-1, 1] and next to +-1 */
	for (i = 0; i < 400000; i++)
		asc(uni(-1, 1));
	for (i = 0; i <= 20000; i++)
		asc(1.0 - i * 0x1p-30), asc(-1.0 + i * 0x1p-30);
	asc(1.0);
	asc(-1.0);
	asc(0.0);

	special("sin(-0)", hl_sin(-0.0), -0.0);
	special("sin(+0)", hl_sin(0.0), 0.0);
	special("cos(-0)", hl_cos(-0.0), 1.0);
	special("sin(inf)", hl_sin(inf), NAN);
	special("cos(nan)", hl_cos(NAN), NAN);
	special("atan2(+0,+0)", hl_atan2(0.0, 0.0), 0.0);
	special("atan2(-0,+0)", hl_atan2(-0.0, 0.0), -0.0);
	special("atan2(+0,-0)", hl_atan2(0.0, -0.0), M_PI);
	special("atan2(-0,-0)", hl_atan2(-0.0, -0.0), -M_PI);
	special("atan2(+0,-1)", hl_atan2(0.0, -1.0), M_PI);
	special("atan2(1,0)", hl_atan2(1.0, 0.0), M_PI / 2);
	special("atan2(-1,-0)", hl_atan2(-1.0, -0.0), -M_PI / 2);
	special("atan2(inf,inf)", hl_atan2(inf, inf), M_PI / 4);
	special("atan2(inf,-inf)", hl_atan2(inf, -inf), 3 * M_PI / 4);
	special("atan2(-inf,1)", hl_atan2(-inf, 1.0), -M_PI / 2);
	special("atan2(1,inf)", hl_atan2(1.0, inf), 0.0);
	special("atan2(-1,-inf)", hl_atan2(-1.0, -inf), -M_PI);
	special("atan(inf)", hl_atan(inf), M_PI / 2);
	special("atan(-0)", hl_atan(-0.0), -0.0);
	special("acos(1)", hl_acos(1.0), 0.0);
	special("acos(-1)", hl_acos(-1.0), M_PI);
	special("asin(-1)", hl_asin(-1.0), -M_PI / 2);
	special("asin(1.5)", hl_asin(1.5), NAN);
	special("acos(nan)", hl_acos(NAN), NAN);

	report(&a_sin, BOUND_D);
	report(&a_cos, BOUND_D);
	report(&a_tan, BOUND_DT);
	report(&a_atan, BOUND_DA);
	report(&a_atan2, BOUND_DA);
	report(&a_asin, BOUND_DS);
	report(&a_acos, BOUND_DS);
	report(&f_sin, BOUND_F);
	report(&f_cos, BOUND_F);
	report(&f_tan, BOUND_F);
	report(&f_atan, BOUND_F);
	report(&f_atan2, BOUND_F);
	report(&f_asin, BOUND_F);
	report(&f_acos, BOUND_F);
	printf("max sin/cos abs err %.3g at %a (|x| in [2^19, 1e7])\n", big_err, big_x);
	if (!(big_err < 1e-6))
	{
		printf("FAIL sin/cos beyond 2^19: abs err %.3g\n", big_err);
		fails++;
	}
	if (fails)
	{
		printf("hlmath FAIL (%d)\n", fails);
		return 1;
	}
	printf("hlmath PASS\n");
	return 0;
}
