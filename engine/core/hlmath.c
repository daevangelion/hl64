/* SPDX-License-Identifier: MIT */
/*
 * hlmath.c -- deterministic transcendentals (hlmath.h). Written from the
 * math: Cody-Waite reduction by pi/2 in three parts, the Taylor series of
 * sin / cos on |r| <= pi/4 (truncation < 2^-60 relative), atan from the
 * series below 1/8 and from a table of atan(k/8) plus the series on
 * |t| <= 1/16 above, asin / acos through atan2. The constants are the
 * doubles nearest the exact values (Taylor coefficients (-1)^k / n!,
 * (-1)^k / (2k+1); pi / 2 split into two 33-bit heads and a tail;
 * atan(k/8) as head + tail). Built without -ffast-math and without
 * contraction on both host and cart.
 */
#include "engine/core/hlmath.h"

#include <math.h>	/* fabs, fmod, sqrt: exact or correctly rounded */
#include <stdint.h>
#include <string.h>

/* pi / 2 = P1 + P2 + P3: P1, P2 33 significant bits, so n * P1 and
 * n * P2 are exact for |n| < 2^20 */
#define PIO2_1		0x1.921fb544p+0
#define PIO2_2		0x1.0b4611a6p-34
#define PIO2_3		0x1.3198a2e037073p-69
#define INV_PIO2	0x1.45f306dc9c883p-1	/* 2 / pi */
#define TWO_PI		0x1.921fb54442d18p+2	/* double nearest 2 pi */
#define PIO2_HI		1.5707963267948966
#define PIO2_LO		6.123233995736766e-17
#define PI_HI		3.141592653589793
#define PI_LO		1.2246467991473532e-16
#define ROUND_MAGIC	0x1.8p52		/* x + M - M: x to the nearest integer */
#define REDUCE_MAX	0x1p19
#define TINY		0x1p-27			/* sin x = x, cos x = 1 below */

/* (-1)^k / (2k+1)!, k = 1..8 */
#define S1 -0.16666666666666666
#define S2 0.008333333333333333
#define S3 -0.0001984126984126984
#define S4 2.7557319223985893e-06
#define S5 -2.505210838544172e-08
#define S6 1.6059043836821613e-10
#define S7 -7.647163731819816e-13
#define S8 2.8114572543455206e-15
/* (-1)^k / (2k)!, k = 2..8 */
#define C2 0.041666666666666664
#define C3 -0.001388888888888889
#define C4 2.48015873015873e-05
#define C5 -2.755731922398589e-07
#define C6 2.08767569878681e-09
#define C7 -1.1470745597729725e-11
#define C8 4.779477332387385e-14
/* (-1)^k / (2k+1), k = 1..8 */
#define A1 -0.3333333333333333
#define A2 0.2
#define A3 -0.14285714285714285
#define A4 0.1111111111111111
#define A5 -0.09090909090909091
#define A6 0.07692307692307693
#define A7 -0.06666666666666667
#define A8 0.058823529411764705

/* atan(k / 8) = hi + lo, k = 0..8 */
static const double atan_hi[9] = {
	0.0, 0.12435499454676144, 0.24497866312686414, 0.35877067027057225,
	0.4636476090008061, 0.5585993153435624, 0.6435011087932844,
	0.7188299996216245, 0.7853981633974483,
};
static const double atan_lo[9] = {
	0.0, -3.1253241424539383e-18, 1.0698755618734451e-17, -2.4623815582638635e-17,
	2.2698777452961687e-17, -5.4556305485916264e-18, 1.5834785051444286e-17,
	-2.1478388444456983e-17, 3.061616997868383e-17,
};

/* A quiet NaN made without a floating-point operation (the cart traps
 * invalid operations). */
static double nan_d(void)
{
	const uint64_t bits = 0x7ff8000000000000ull;
	double d;

	memcpy(&d, &bits, sizeof d);
	return d;
}

static int sign_bit(double x)
{
	uint64_t bits;

	memcpy(&bits, &x, sizeof bits);
	return (int)(bits >> 63);
}

static int finite_d(double x)
{
	return fabs(x) <= 0x1.fffffffffffffp+1023;
}

/* x = n pi / 2 + r + c with |r + c| <= ~pi / 4, |c| <= ulp(r) / 2;
 * returns n mod 4. x finite. */
static int reduce(double x, double *r, double *c)
{
	double fn, t, w, y, e;

	if (!(fabs(x) < REDUCE_MAX))
		x = fmod(x, TWO_PI);
	fn = x * INV_PIO2 + ROUND_MAGIC;
	fn -= ROUND_MAGIC;
	t = x - fn * PIO2_1;	/* exact (Sterbenz) */
	w = fn * PIO2_2;	/* exact */
	y = t - w;
	e = ((t - y) - w) - fn * PIO2_3;
	*r = y + e;
	*c = (y - *r) + e;
	return (int)fn & 3;
}

/* sin(r + c), |r| <= ~pi / 4 */
static double ksin(double r, double c)
{
	double z = r * r;
	double p = S2 + z * (S3 + z * (S4 + z * (S5 + z * (S6 + z * (S7 + z * S8)))));

	return r + (c + r * z * (S1 + z * p));
}

/* cos(r + c), |r| <= ~pi / 4: 1 - z / 2 rounded, its rounding error
 * recovered exactly, then the rest */
static double kcos(double r, double c)
{
	double z = r * r, hz = 0.5 * z, w = 1.0 - hz;
	double q = C2 + z * (C3 + z * (C4 + z * (C5 + z * (C6 + z * (C7 + z * C8)))));

	return w + (((1.0 - w) - hz) + (z * z * q - r * c));
}

double hl_sin(double x)
{
	double r, c;

	if (!finite_d(x))
		return nan_d();
	if (fabs(x) < TINY)
		return x;
	switch (reduce(x, &r, &c))
	{
	case 0:
		return ksin(r, c);
	case 1:
		return kcos(r, c);
	case 2:
		return -ksin(r, c);
	default:
		return -kcos(r, c);
	}
}

double hl_cos(double x)
{
	double r, c;

	if (!finite_d(x))
		return nan_d();
	if (fabs(x) < TINY)
		return 1.0;
	switch (reduce(x, &r, &c))
	{
	case 0:
		return kcos(r, c);
	case 1:
		return -ksin(r, c);
	case 2:
		return -kcos(r, c);
	default:
		return ksin(r, c);
	}
}

/* hl_sin and hl_cos of one argument from one reduction: the same
 * operations, so the same results as the two calls */
void hl_sincos(double x, double *s, double *c)
{
	double r, cc, ks, kc;
	int n;

	if (!finite_d(x))
	{
		*s = *c = nan_d();
		return;
	}
	if (fabs(x) < TINY)
	{
		*s = x;
		*c = 1.0;
		return;
	}
	n = reduce(x, &r, &cc);
	ks = ksin(r, cc);
	kc = kcos(r, cc);
	*s = n == 0 ? ks : n == 1 ? kc : n == 2 ? -ks : -kc;
	*c = n == 0 ? kc : n == 1 ? -ks : n == 2 ? -kc : ks;
}

double hl_tan(double x)
{
	double r, c;
	int n;

	if (!finite_d(x))
		return nan_d();
	if (fabs(x) < TINY)
		return x;
	n = reduce(x, &r, &c);
	return (n & 1) ? -kcos(r, c) / ksin(r, c) : ksin(r, c) / kcos(r, c);
}

/* atan(a), 0 <= a <= 1: below 1/8 the series (to a^17); above, k =
 * nearest a * 8 (>= 1), t = (a - k/8) / (1 + a k/8) (|t| <= 1/16;
 * a - k/8 exact) and atan(k/8) + atan(t) */
static double katan01(double a)
{
	int k;
	double t, z;

	if (a < 0.125)
	{
		z = a * a;
		return a + a * z * (A1 + z * (A2 + z * (A3 + z * (A4 + z * (A5 + z * (A6 + z * (A7 + z * A8)))))));
	}
	k = (int)(a * 8.0 + 0.5);
	t = (a - k * 0.125) / (1.0 + a * (k * 0.125));
	z = t * t;
	return atan_hi[k] + (atan_lo[k] + (t + t * z * (A1 + z * (A2 + z * (A3 + z * (A4 + z * (A5 + z * (A6 + z * A7))))))));
}

double hl_atan2(double y, double x)
{
	double ax = fabs(x), ay = fabs(y), r;

	if (x != x || y != y)
		return nan_d();
	if (ay == 0.0)
		r = sign_bit(x) ? PI_HI : 0.0;
	else if (ax == 0.0)
		r = PIO2_HI;
	else
	{
		if (!finite_d(ax) && !finite_d(ay))
			r = katan01(1.0);
		else if (ay <= ax)
			r = katan01(ay / ax);
		else
			r = PIO2_HI - (katan01(ax / ay) - PIO2_LO);
		if (sign_bit(x))
			r = PI_HI - (r - PI_LO);
	}
	return sign_bit(y) ? -r : r;
}

double hl_atan(double x)
{
	return hl_atan2(x, 1.0);
}

/* asin / acos: atan2 of (x, sqrt(1 - x^2)) with 1 - x^2 = (1 - x)(1 + x) */
double hl_asin(double x)
{
	if (!(fabs(x) <= 1.0))
		return nan_d();
	return hl_atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double hl_acos(double x)
{
	if (!(fabs(x) <= 1.0))
		return nan_d();
	return hl_atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

/* float: the double function rounded once */
float hl_sinf(float x)
{
	return (float)hl_sin(x);
}

float hl_cosf(float x)
{
	return (float)hl_cos(x);
}

void hl_sincosf(float x, float *s, float *c)
{
	double sd, cd;

	hl_sincos(x, &sd, &cd);
	*s = (float)sd;
	*c = (float)cd;
}

float hl_tanf(float x)
{
	return (float)hl_tan(x);
}

float hl_atanf(float x)
{
	return (float)hl_atan(x);
}

float hl_atan2f(float y, float x)
{
	return (float)hl_atan2(y, x);
}

float hl_asinf(float x)
{
	return (float)hl_asin(x);
}

float hl_acosf(float x)
{
	return (float)hl_acos(x);
}
