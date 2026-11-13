/* SPDX-License-Identifier: MIT */
/*
 * hlmath.h -- the engine's transcendental functions (docs/architecture.md
 * 15, "Test-build determinism"). Host libm (glibc) and cart libm (newlib)
 * round sin / cos / atan2 ... differently, so a route that calls them
 * takes different paths on host and cart. These are built only from IEEE
 * basic operations (+ - * /, sqrt, and the exact fmod), which round the
 * same on both, compiled without -ffast-math: every engine/ and game/ call
 * goes through them (test/m4/hlmath.py fails on a direct libm call).
 *
 * Accuracy (test/m4/hlmath.c, max error vs a long-double reference): sin
 * and cos within 1 ulp, atan / atan2 1.5, asin / acos 2, tan 2.5; the
 * float functions are the double ones rounded once, within 1 ulp of the
 * correctly rounded value (measured: correctly rounded).
 * Domain: |x| < 2^19 reduces exactly for sin / cos / tan; larger |x| are
 * first taken modulo the double nearest 2 pi (deterministic, less
 * accurate). NaN / out-of-range input gives NaN, infinities follow C99
 * Annex F for atan / atan2. Arguments whose atan2 ratio would underflow
 * a double (|y / x| < 2^-1022) are outside the contract: the cart
 * flushes denormals.
 */
#ifndef HL64_HLMATH_H
#define HL64_HLMATH_H

double	hl_sin(double x);
double	hl_cos(double x);
double	hl_tan(double x);
double	hl_atan(double x);
double	hl_atan2(double y, double x);
double	hl_asin(double x);
double	hl_acos(double x);

float	hl_sinf(float x);
float	hl_cosf(float x);
float	hl_tanf(float x);
float	hl_atanf(float x);
float	hl_atan2f(float y, float x);
float	hl_asinf(float x);
float	hl_acosf(float x);

#endif
