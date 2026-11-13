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
 * material.c -- HLSDK dlls/sound.cpp TEXTURETYPE_PlaySound. TRACE_TEXTURE
 * + TEXTURETYPE_Find become one cooked lookup: engine/coll col_material
 * (trace-and-hulls.md 4.7) returns the texture-type character of the
 * surface the segment crosses ('C' for an unlisted texture), through
 * util.c UTIL_TraceTexture in the caller's frame. Replaces the old tree's
 * hl_material.c (audit entry: HLSDK + CONTAINS-QUAKE + own N64 path; its
 * hull walk and rotate-into-model blocks are not used: the cooked material
 * walk lives in engine/coll).
 */
#include "game/hl/material.h"

int TEXTURETYPE_Get(const col_trace_t *tr, const float src[3], const float end[3])
{
	int hit = tr->ent, cls;

	if (hit > 0 && CBase(hit))
	{
		cls = Classify(hit);
		if (cls != CLASS_NONE && cls != CLASS_MACHINE)
			return CHAR_TEX_FLESH;
	}
	return UTIL_TraceTexture(hit > 0 ? hit : 0, src, end);
}

float TEXTURETYPE_PlaySound(const col_trace_t *tr, const float src[3], const float end[3],
	int bullet)
{
	res_t s[4];
	float fvol, fvolbar, fattn = ATTN_NORM;
	int cnt, type = TEXTURETYPE_Get(tr, src, end);

	switch (type)
	{
	default:
	case CHAR_TEX_CONCRETE:
		fvol = 0.9f; fvolbar = 0.6f;
		s[0] = RESID(PL_STEP1); s[1] = RESID(PL_STEP2);
		cnt = 2;
		break;
	case CHAR_TEX_METAL:
		fvol = 0.9f; fvolbar = 0.3f;
		s[0] = RESID(PL_METAL1); s[1] = RESID(PL_METAL2);
		cnt = 2;
		break;
	case CHAR_TEX_DIRT:
		fvol = 0.9f; fvolbar = 0.1f;
		s[0] = RESID(PL_DIRT1); s[1] = RESID(PL_DIRT2); s[2] = RESID(PL_DIRT3);
		cnt = 3;
		break;
	case CHAR_TEX_VENT:
		fvol = 0.5f; fvolbar = 0.3f;
		s[0] = RESID(PL_DUCT1); s[1] = RESID(PL_DUCT1);
		cnt = 2;
		break;
	case CHAR_TEX_GRATE:
		fvol = 0.9f; fvolbar = 0.5f;
		s[0] = RESID(PL_GRATE1); s[1] = RESID(PL_GRATE4);
		cnt = 2;
		break;
	case CHAR_TEX_TILE:
		fvol = 0.8f; fvolbar = 0.2f;
		s[0] = RESID(PL_TILE1); s[1] = RESID(PL_TILE3);
		s[2] = RESID(PL_TILE2); s[3] = RESID(PL_TILE4);
		cnt = 4;
		break;
	case CHAR_TEX_SLOSH:
		fvol = 0.9f; fvolbar = 0.0f;
		s[0] = RESID(PL_SLOSH1); s[1] = RESID(PL_SLOSH3);
		s[2] = RESID(PL_SLOSH2); s[3] = RESID(PL_SLOSH4);
		cnt = 4;
		break;
	case CHAR_TEX_WOOD:
		fvol = 0.9f; fvolbar = 0.2f;
		s[0] = RESID(DEBRIS_WOOD1); s[1] = RESID(DEBRIS_WOOD2); s[2] = RESID(DEBRIS_WOOD3);
		cnt = 3;
		break;
	case CHAR_TEX_GLASS:
	case CHAR_TEX_COMPUTER:
		fvol = 0.8f; fvolbar = 0.2f;
		s[0] = RESID(DEBRIS_GLASS1); s[1] = RESID(DEBRIS_GLASS2); s[2] = RESID(DEBRIS_GLASS3);
		cnt = 3;
		break;
	case CHAR_TEX_FLESH:
		if (bullet == BULLET_PLAYER_CROWBAR)
			return 0.0f;
		fvol = 1.0f; fvolbar = 0.2f;
		s[0] = RESID(BULLET_HIT1); s[1] = RESID(BULLET_HIT2);
		fattn = 1.0f;
		cnt = 2;
		break;
	}
	if (tr->ent > 0 && ent_idc(tr->ent)->cls == CLS_FUNC_BREAKABLE)
	{
		fvol /= 1.5f;
		fvolbar /= 2.0f;
	}
	else if (type == CHAR_TEX_COMPUTER)
	{
		if (tr->frac != 1.0f && RANDOM_LONG(0, 1))
		{
			float v;

			UTIL_Sparks(tr->end);
			v = RANDOM_FLOAT(0.7f, 1.0f);
			snd_ambient(tr->end, RANDOM_LONG(0, 1) ? RESID(SPARK6) : RESID(SPARK5), v,
				ATTN_NORM, 0, 100);
		}
	}
	{
		/* the sample before the pitch: argument order is unspecified (K10) */
		res_t snd = s[RANDOM_LONG(0, cnt - 1)];

		snd_ambient(tr->end, snd, fvol, fattn, 0, 96 + RANDOM_LONG(0, 0xf));
	}
	return fvolbar;
}
