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
 * pose.h -- game/studio internals shared by pose.c (HLSDK cl_dll
 * StudioModelRenderer bone maths) and anim.c (dlls/animation.cpp).
 */
#ifndef HL64_GAME_STUDIO_POSE_H
#define HL64_GAME_STUDIO_POSE_H

#include "hl64/game.h"
#include "hl64/studio_fmt.h"
#include "hl64/types.h"

#define GS_MAXBONES	128	/* MAXSTUDIOBONES */

/* The cl_entity_t fields StudioEstimateFrame reads. */
typedef struct {
	struct {
		float	frame, animtime, framerate;
	} curstate;
} gs_clent_t;
/* StudioEstimateFrame: frame index (0..numframes-1) of the pose. */
float			gs_estimate_frame(const sfmt_seq_t *pseqdesc, const gs_clent_t *ent,
				double cltime, int interp);

/* The model's resident info block (loaded on first use), NULL for a
 * res_t that is not a studio model. */
const sfmt_info_t	*gs_info(res_t model);
/* Bone `bone` of entity e's server pose (its components, map-local): only
 * the bone's chain is posed (memoised); NULL when e has no studio pose or
 * no such bone. Valid until the next server pose. */
const float		(*gs_entity_bone(int e, int bone))[4];

#endif
