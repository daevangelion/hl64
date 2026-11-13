/* SPDX-License-Identifier: MIT */
/*
 * in.h -- input: pad -> usercmd (docs/architecture.md 11,
 * docs/specs/pmove-host.md 1 step 1).
 *
 * The pad (stick move / turn, C-buttons strafe / look, A jump, B use,
 * Z attack, R attack2, L duck) and the held route / debug buttons
 * (IN_BTN_FORWARD... as the HLSDK client's +forward etc.) build one
 * in_cmd_t per frame: msec = round(dt) in [1, 100], the view angles,
 * forward / side / up moves at cl_forwardspeed 400, cl_sidespeed 400,
 * cl_upspeed 320 (IN_BTN_RUN walks at 0.3), clipped to the player's
 * client max speed. Absolute stick turning scales by dt.
 */
#ifndef HL64_IN_H
#define HL64_IN_H

#include <stdint.h>

#include "hl64/pmove.h"

/* Boot: the buttons / impulse debug command handlers. */
void		in_init(void);
/* Step 2: this frame's command. maxspeed = the player's client max speed
 * (0: no clip). */
void		in_frame(uint32_t dt_us, float maxspeed, in_cmd_t *cmd);
/* View angles (setpos / look / route LOOK). */
void		in_set_angles(const float ang[3]);
const float	*in_angles(void);
/* Route buttons: held = (held | down) & ~up; impulse for the next command. */
void		in_buttons(uint16_t down, uint16_t up);
void		in_impulse(uint8_t impulse);
uint16_t	in_held(void);

#endif
