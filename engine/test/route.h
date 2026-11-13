/* SPDX-License-Identifier: MIT */
/*
 * route.h -- the compiled-route VM (engine/test/route.c,
 * docs/formats/route-bytecode.md).
 */
#ifndef HL64_ROUTE_H
#define HL64_ROUTE_H

#include <stdint.h>

/* Load and validate a route (cart path or host file): 1 loaded, 0 none,
 * -1 rejected (logged). */
int	rt_load(const char *path);
int	rt_active(void);
/* Step 13: run ops until a wait is not yet satisfied; PASS / FAIL exit. */
void	rt_frame(uint64_t now_us);

#endif
