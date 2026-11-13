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

#endif
