/* SPDX-License-Identifier: MIT */
/*
 * rng.h -- the engine random stream; the draws are declared in
 * include/hl64/pmove.h (hl64_rand_long / hl64_rand_float) for game/.
 */
#ifndef HL64_RNG_H
#define HL64_RNG_H

#include <stdint.h>

/* New Game (and tests): restart the stream. */
void	hl64_rand_seed(uint32_t seed);
#ifdef HL64_TEST
/* K10 triage: the stream's state and its draws since boot. */
uint32_t	hl64_rand_state(uint32_t *draws);
#endif

#endif
