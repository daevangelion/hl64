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
 * entities, world, input, player, and New Game (records, the player at the
 * start map's info_player_start) when a compiled route is present or
 * eng_play is set. Logs the carve table and "boot ok". */
void		eng_boot(const char *platform);
void		eng_frame(void);
/* Frames since boot (the frame being run; dbg commands and stats records
 * carry the same counter). */
uint32_t	eng_frame_count(void);
/* K10 triage (test builds, `trace FIRST LAST`): 1 while the frame being run
 * is inside the trace window; modules then log `trace F <what> ...` lines
 * of their decisions, the frame loop one `trace F frame` line per frame. */
#ifdef HL64_TEST
int		eng_trace(void);
#else
#define eng_trace()	0
#endif

/* The route eng_boot loads: "route.rbc" at the cart root unless the
 * platform names another before eng_boot (host --route). */
extern const char *eng_route_path;
/* Without a route: New Game at the campaign start and play on pad input.
 * Playable builds 1; HL64_TEST builds 0 (their routeless boots fly the
 * free camera) unless the platform sets it before eng_boot (host --play). */
extern int eng_play;

#endif
