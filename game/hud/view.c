/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
/*
 * hl64 game/, Half Life 1 SDK licence (game/LICENSE-HLSDK).
 *
 * view.c -- the first-person view: HLSDK cl_dll/view.cpp V_CalcBob,
 * V_CalcRoll and the player part of V_CalcNormalRefdef (eye at origin +
 * view_ofs + bob, the 1/32 nudge, water offset, movement roll, punch
 * angle, stair-step smoothing) with HL's default client settings
 * (cl_bob 0.01, cl_bobcycle 0.8, cl_bobup 0.5, cl_waterdist 4,
 * v_idlescale 0, no pitch drift, no chase camera). The screen shake
 * and the client-side event punch join with M3 effects.
 */
#include <math.h>
#include <stddef.h>

#include "engine/core/hlmath.h"
#include "hl64/fx.h"
#include "hl64/game.h"
#include "hl64/settings.h"
#include "hl64/studio.h"
#include "game/hl/player.h"
#include "game/studio/anim.h"

#define CL_BOB		0.01f
#define CL_BOBCYCLE	0.8f
#define CL_BOBUP	0.5f
#define CL_WATERDIST	4
#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif

#define FL_ONGROUND	(1u << 9)

static void angle_vectors(const float *angles, float *forward, float *right, float *up)
{
	float angle, sr, sp, sy, cr, cp, cy;

	angle = angles[1] * ((float)M_PI * 2 / 360);
	sy = hl_sin(angle);
	cy = hl_cos(angle);
	angle = angles[0] * ((float)M_PI * 2 / 360);
	sp = hl_sin(angle);
	cp = hl_cos(angle);
	angle = angles[2] * ((float)M_PI * 2 / 360);
	sr = hl_sin(angle);
	cr = hl_cos(angle);
	forward[0] = cp * cy;
	forward[1] = cp * sy;
	forward[2] = -sp;
	right[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
	right[1] = (-1 * sr * sp * sy + -1 * cr * cy);
	right[2] = -1 * sr * cp;
	up[0] = (cr * sp * cy + -sr * -sy);
	up[1] = (cr * sp * sy + -sr * cy);
	up[2] = cr * cp;
}

/* V_CalcBob: Quakeworld bob code */
static float V_CalcBob(float time, float frametime, int onground, const float *simvel)
{
	static double bobtime;
	static float bob;
	static float lasttime;
	float cycle;

	if (!onground || time == lasttime)
		return bob;	/* just use old value */
	lasttime = time;

	bobtime += frametime;
	cycle = bobtime - (int)(bobtime / CL_BOBCYCLE) * CL_BOBCYCLE;
	cycle /= CL_BOBCYCLE;
	if (cycle < CL_BOBUP)
		cycle = M_PI * cycle / CL_BOBUP;
	else
		cycle = M_PI + M_PI * (cycle - CL_BOBUP) / (1.0 - CL_BOBUP);

	/* bob is proportional to simulated velocity in the xy plane
	 * (don't count Z, or jumping messes it up) */
	bob = sqrt(simvel[0] * simvel[0] + simvel[1] * simvel[1]) * CL_BOB;
	bob = bob * 0.3 + bob * 0.7 * hl_sin(cycle);
	if (bob > 4.0f)
		bob = 4.0f;
	if (bob < -7.0f)
		bob = -7.0f;
	return bob;
}

/* V_CalcRoll */
static float V_CalcRoll(const float *angles, const float *velocity, float rollangle, float rollspeed)
{
	float sign, side, value, forward[3], right[3], up[3];

	angle_vectors(angles, forward, right, up);
	side = velocity[0] * right[0] + velocity[1] * right[1] + velocity[2] * right[2];
	sign = side < 0 ? -1 : 1;
	side = fabs(side);
	value = rollangle;
	if (side < rollspeed)
		side = side * value / rollspeed;
	else
		side = value;
	return side * sign;
}

/* The view model (V_CalcNormalRefdef's view entity): at the eye with
 * 40% of the bob forward, the bob up and a little tilt, shaken at 90%. */
static void view_model(const ent_player_t *pl, float wofs, float bob,
	const float so[3], const float sa[3])
{
	const CBasePlayer *p = Player();
	studio_view_t vm;
	float fwd[3], right[3], up[3];
	int i;

	if (!p || p->viewmodel == RES_NONE || !PlayerMon() || pl->dead)
	{
		studio_view_model(NULL);
		return;
	}
	vm.model = p->viewmodel;
	vm.sequence = p->weaponanim;
	vm.frame = gs_view_frame(p->viewmodel, p->weaponanim, 0, p->weaponanim_time, 1.0f);
	vm.body = p->viewbody;
	vm.skin = 0;
	for (i = 0; i < 4; i++)
		vm.controller[i] = 0;
	vm.pad[0] = vm.pad[1] = 0;
	angle_vectors(pl->view_angles, fwd, right, up);
	for (i = 0; i < 3; i++)
	{
		vm.ang[i] = pl->view_angles[i] + sa[i] * 0.9f;
		vm.org[i] = pl->origin[i] + pl->view_ofs[i] + so[i] * 0.9f + bob * 0.4f * fwd[i];
	}
	vm.org[2] += wofs + bob;
	/* throw in a little tilt */
	vm.ang[1] -= bob * 0.5f;
	vm.ang[2] -= bob * 1;
	vm.ang[0] -= bob * 0.3f;
	/* pushing the view origin down off of the same X/Y plane as the ent's
	 * origin gives the gun a nice 'shifting' effect when looking up/down */
	vm.org[2] -= 1;
	studio_view_model(&vm);
}

void game_calc_view(const ent_player_t *pl, float time, float frametime, game_view_t *out)
{
	static float oldz, lasttime;
	int onground = (pl->flags & FL_ONGROUND) != 0, i;
	float bob, waterOffset = 0;

	bob = V_CalcBob(time, frametime, onground, pl->velocity);

	/* refresh position */
	for (i = 0; i < 3; i++)
	{
		out->org[i] = pl->origin[i] + pl->view_ofs[i];
		out->ang[i] = pl->view_angles[i];
	}
	out->org[2] += bob;

	/* never let view origin sit exactly on a node line */
	out->org[0] += 1.0 / 32;
	out->org[1] += 1.0 / 32;
	out->org[2] += 1.0 / 32;

	/* check for problems around water, move the viewer if necessary */
	if (pl->waterlevel >= 2)
	{
		float point[3] = { out->org[0], out->org[1], out->org[2] };
		int contents;

		if (pl->waterlevel == 2)
		{
			/* eyes are above water, make sure we're above the waves */
			point[2] -= CL_WATERDIST;
			for (i = 0; i < CL_WATERDIST; i++)
			{
				contents = phy_pm_contents(point, NULL);
				if (contents > COL_WATER)
					break;
				point[2] += 1;
			}
			waterOffset = (point[2] + CL_WATERDIST) - out->org[2];
		}
		else
		{
			/* eyes are under water, make sure we're far enough under */
			point[2] += CL_WATERDIST;
			for (i = 0; i < CL_WATERDIST; i++)
			{
				contents = phy_pm_contents(point, NULL);
				if (contents <= COL_WATER)
					break;
				point[2] -= 1;
			}
			waterOffset = (point[2] - CL_WATERDIST) - out->org[2];
		}
	}
	out->org[2] += waterOffset;

	/* V_CalcViewRoll: roll is induced by movement */
	out->ang[2] += V_CalcRoll(pl->angles, pl->velocity, settings.rollangle, settings.rollspeed);

	/* add in the punchangle, if any */
	for (i = 0; i < 3; i++)
		out->ang[i] += pl->punchangle[i];

	/* smooth out stair step ups */
	if (onground && pl->origin[2] - oldz > 0)
	{
		float steptime = time - lasttime;

		if (steptime < 0)
			steptime = 0;
		oldz += steptime * 150;
		if (oldz > pl->origin[2])
			oldz = pl->origin[2];
		if (pl->origin[2] - oldz > 18)
			oldz = pl->origin[2] - 18;
		out->org[2] += oldz - pl->origin[2];
	}
	else
		oldz = pl->origin[2];

	lasttime = time;

	/* screen shake (the engine's V_CalcShake / V_ApplyShake, fx.h) */
	{
		float so[3], sa[3];
		uint8_t fade[4];

		fx_view(so, sa, fade);
		for (i = 0; i < 3; i++)
		{
			out->org[i] += so[i];
			out->ang[i] += sa[i];
		}
		view_model(pl, waterOffset, bob, so, sa);
	}
}
