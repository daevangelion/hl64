/* SPDX-License-Identifier: MIT */
/*
 * rng.c -- the engine random stream (docs/architecture.md 7.1 "time,
 * rng"): one xorshift32 stream, seeded at New Game, for every game and
 * engine random draw (HLSDK RandomLong / RandomFloat ranges).
 */
#include "engine/core/rng.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "hl64/pmove.h"

static uint32_t rng_state = 0x6c078965u;
#ifdef HL64_TEST
static uint32_t rng_draws;

uint32_t hl64_rand_state(uint32_t *draws)
{
	*draws = rng_draws;
	return rng_state;
}
#endif

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
#ifdef HL64_TEST
	rng_draws++;
#endif
	return x;
}

#ifdef HL64_TEST
/* `trace` window: each draw with its caller's address (addr2line on the
 * build's ELF names the call site) */
#define TRACE_DRAW()	do { if (dbg_trace()) log_printf("trace %lu rand %lu at %lx\n", \
	(unsigned long)dbg_trace_frame(), (unsigned long)rng_draws, \
	(unsigned long)(uintptr_t)__builtin_return_address(0)); } while (0)
#else
#define TRACE_DRAW()	((void)0)
#endif

/* [lo, hi] inclusive; lo when the range is empty */
int32_t hl64_rand_long(int32_t lo, int32_t hi)
{
	uint32_t span;

	if (hi <= lo)
		return lo;
	span = (uint32_t)hi - (uint32_t)lo + 1u;
	if (!span)
	{
		TRACE_DRAW();
		return (int32_t)next();
	}
	TRACE_DRAW();
	return (int32_t)((uint32_t)lo + next() % span);
}

/* [lo, hi] */
float hl64_rand_float(float lo, float hi)
{
	TRACE_DRAW();
	return lo + (hi - lo) * (float)(next() >> 8) * (1.0f / 16777215.0f);
}
