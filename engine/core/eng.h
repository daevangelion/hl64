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

/* The route eng_boot loads: "route.rbc" at the cart root unless the
 * platform names another before eng_boot (host --route). */
extern const char *eng_route_path;
/* Without a route: New Game at the campaign start and play on pad input.
 * Playable builds 1; HL64_TEST builds 0 (their routeless boots fly the
 * free camera) unless the platform sets it before eng_boot (host --play). */
extern int eng_play;
/* Without a route: boot to the title menu (engine/ui/menu.c) instead,
 * which starts the game. Playable builds 1; HL64_TEST builds 0 unless the
 * platform sets it (host --title). */
extern int eng_title;

/* The front end's session control (engine/ui/menu.c), at step 2: */
/* 1 when New Game may start now (no session; the last one's layers have
 * retired their fenced frees). */
int		eng_can_start(void);
/* New Game on CWG map (eng_can_start() must hold). */
void		eng_start_game(int map);
/* End the session (Quit to title): sounds, effects, entities, player,
 * records and layers dropped as at an end of section; the regions return
 * to their boot state (checked: `mem: title ...` line). */
void		eng_to_title(void);
/* The game's "reload" (HLSDK player_loadsaved, any time in a frame): the
 * newest saved game loads at the next frame boundary; without one, New
 * Game on the player's map (logged `eng: reload: ...`). */
void		eng_reload(void);

#endif
