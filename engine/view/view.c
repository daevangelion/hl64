/* SPDX-License-Identifier: MIT */
/*
 * view.c -- the camera source once the player exists (docs/architecture.md
 * 3.2 step 11, 9.1): the game's first-person view (game_calc_view) of the
 * player block places the renderer's camera in the player's home map.
 * Before the player spawns the M1 free camera (engine/render/cam.c) flies.
 */
#include "hl64/game.h"
#include "engine/phys/player.h"
#include "engine/render/ren.h"
#include "engine/view/view.h"
#include "engine/world/wld.h"

void ren_view_frame(double time, double dt)
{
	game_view_t v;
	const float *g;
	int k;

	if (!phy_player_active())
		return;
	game_calc_view(phy_player(), (float)time, (float)dt, &v);
	ren_cam.map = phy_player_map();
	g = wld_map_origin(ren_cam.map);
	/* the view is home-local; the camera global */
	for (k = 0; k < 3; k++)
	{
		ren_cam.org[k] = v.org[k] + g[k];
		ren_cam.ang[k] = v.ang[k];
	}
}
