/* SPDX-License-Identifier: MIT */
/*
 * eng.h -- engine boot and the frame loop (docs/architecture.md 3.1, 3.2).
 * The platform's main() initialises the hardware (or its host model),
 * calls eng_boot() once and eng_frame() forever; the loop leaves through
 * sys_exit (DBG_QUIT) or sys_error.
 */
#ifndef HL64_ENG_H
#define HL64_ENG_H

#include <stdint.h>

/* Boot steps after the platform's: carve, settings, io, debug channel,
 * entities, world, input, player, and a compiled route when one is
 * present (it spawns the player at the campaign start). Logs the carve
 * table and "boot ok". */
void		eng_boot(const char *platform);
void		eng_frame(void);

/* The route eng_boot loads: "route.rbc" at the cart root unless the
 * platform names another before eng_boot (host --route). */
extern const char *eng_route_path;

#endif
