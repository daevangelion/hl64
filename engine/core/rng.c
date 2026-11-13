/* SPDX-License-Identifier: MIT */
/*
 * rng.c -- the engine random stream (docs/architecture.md 7.1 "time,
 * rng"): one xorshift32 stream, seeded at New Game, for every game and
 * engine random draw (HLSDK RandomLong / RandomFloat ranges).
 */
#include "engine/core/rng.h"
#include "hl64/pmove.h"

static uint32_t rng_state = 0x6c078965u;

void hl64_rand_seed(uint32_t seed)
{
	rng_state = seed ? seed : 0x6c078965u;
}

static uint32_t next(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

/* [lo, hi] inclusive; lo when the range is empty */
int32_t hl64_rand_long(int32_t lo, int32_t hi)
{
	uint32_t span;

	if (hi <= lo)
		return lo;
	span = (uint32_t)hi - (uint32_t)lo + 1u;
	if (!span)
		return (int32_t)next();
	return (int32_t)((uint32_t)lo + next() % span);
}

/* [lo, hi] */
float hl64_rand_float(float lo, float hi)
{
	return lo + (hi - lo) * (float)(next() >> 8) * (1.0f / 16777215.0f);
}
