/* SPDX-License-Identifier: MIT */
/*
 * view.h -- the player's view as the renderer's camera (engine/view/view.c).
 */
#ifndef HL64_VIEW_H
#define HL64_VIEW_H

/* Step 11, before the world draw: ren_cam from game_calc_view when the
 * player exists (game clock and frame time, f64 s); otherwise the free
 * camera stands. */
void	ren_view_frame(double time, double dt);

#endif
