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
 * event.c -- client-side studio events (HLSDK cl_dll/entity.cpp
 * HUD_StudioEvent): muzzle flashes on attachments 0-3, the spark effect,
 * client sounds. Fired by game_studio_setup_bones (pose.c) for the frames
 * the drawn pose passed since the entity was last drawn.
 */
#include <stdlib.h>

#include "hl64/ent.h"
#include "hl64/fx.h"
#include "hl64/snd.h"
#include "game/studio/anim.h"
#include "game/studio/pose.h"

/* The cooked resource of the option text `options` (a pointer into the
 * model's string pool, as gs_anim_event / the client event walk hand it
 * out): pfnPlaySoundByNameAtLocation's sound without a name lookup. */
static res_t option_res(res_t model, const char *options)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_event_t *ev;
	int i;

	if (!h)
		return RES_NONE;
	ev = SFMT_EVENTS(h);
	for (i = 0; i < h->numevents; i++)
		if (SFMT_STR(h, ev[i].options) == options)
			return ev[i].res;
	return RES_NONE;
}

void game_studio_event(int e, int event, const char *options)
{
	float org[3];
	res_t r;

	switch (event)
	{
	case 5001:
		fx_muzzle_flash(e, 0, atoi(options));
		break;
	case 5011:
		fx_muzzle_flash(e, 1, atoi(options));
		break;
	case 5021:
		fx_muzzle_flash(e, 2, atoi(options));
		break;
	case 5031:
		fx_muzzle_flash(e, 3, atoi(options));
		break;
	case 5002:
		/* R_SparkEffect on attachment 0 (count = options, velocity
		 * -100..100): the effects API's spark burst */
		gs_attachment(e, 0, org);
		fx_sparks(org);
		break;
	// Client side sound
	case 5004:
		r = option_res(ent_rnd(e)->model, options);
		if (r != RES_NONE)
		{
			gs_attachment(e, 0, org);
			snd_ambient(org, r, 1.0f, ATTN_NORM, 0, 100);
		}
		break;
	default:
		break;
	}
}
