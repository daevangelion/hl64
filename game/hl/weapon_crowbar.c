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
 * weapon_crowbar.c -- CCrowbar (HLSDK dlls/crowbar.cpp) with its client
 * event EV_Crowbar (cl_dll/ev_hldm.cpp: the swing sound and miss
 * animations).
 */
#include "game/hl/player.h"
#include "game/hl/savegame.h"

enum {
	CROWBAR_IDLE = 0, CROWBAR_DRAW, CROWBAR_HOLSTER, CROWBAR_ATTACK1HIT, CROWBAR_ATTACK1MISS,
	CROWBAR_ATTACK2MISS, CROWBAR_ATTACK2HIT, CROWBAR_ATTACK3MISS, CROWBAR_ATTACK3HIT
};

#define CROWBAR_BODYHIT_VOLUME	128
#define CROWBAR_WALLHIT_VOLUME	512

#define THINK_SMACK		1
#define THINK_SWINGAGAIN	2

static int g_iSwing;	/* the client event's swing counter */

uint32_t W_CrowbarSaveState(int op)
{
	return sg_io(op, &g_iSwing, sizeof(g_iSwing));
}

static int Deploy(int id)
{
	return W_DefaultDeploy(id, CROWBAR_DRAW);
}

static void Holster(int id)
{
	(void)id;
	Player()->m_flNextAttack = 0.5f;
	W_SendWeaponAnim(CROWBAR_HOLSTER);
}

static const float VEC_DUCK_HULL_MIN[3] = { -16, -16, -18 };
static const float VEC_DUCK_HULL_MAX[3] = { 16, 16, 18 };

static void FindHullIntersection(const float src[3], col_trace_t *tr, const float mins[3],
	const float maxs[3])
{
	const float *minmaxs[2] = { mins, maxs };
	col_trace_t tmp;
	float hullend[3], end[3], distance = 1e6f;
	int i, j, k;

	VecSub(tr->end, src, hullend);
	VecMA(src, 2, hullend, hullend);
	UTIL_TraceLine(src, hullend, dont_ignore_monsters, ENT_PLAYER, &tmp);
	if (tmp.frac < 1.0f)
	{
		*tr = tmp;
		return;
	}
	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
			{
				end[0] = hullend[0] + minmaxs[i][0];
				end[1] = hullend[1] + minmaxs[j][1];
				end[2] = hullend[2] + minmaxs[k][2];
				UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tmp);
				if (tmp.frac < 1.0f)
				{
					float d[3], dist;

					VecSub(tmp.end, src, d);
					dist = VecLen(d);
					if (dist < distance)
					{
						*tr = tmp;
						distance = dist;
					}
				}
			}
}

static int Swing(int id, int fFirst)
{
	CBasePlayerWeapon *w = &Player()->wpn[id];
	col_trace_t tr;
	float src[3], end[3];
	int fDidHit = 0, hit;

	UTIL_MakeVectors(phy_player()->view_angles);
	Player_GunPosition(src);
	VecMA(src, 32, gpGlobals.v_forward, end);
	UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tr);
	if (tr.frac >= 1.0f)
	{
		UTIL_TraceHull(src, end, dont_ignore_monsters, head_hull, ENT_PLAYER, &tr);
		if (tr.frac < 1.0f)
		{
			/* calculate the point of intersection of the line (or hull)
			 * and the object we hit */
			hit = tr.ent;
			if (hit <= 0 || IsBSPModel(hit))
				FindHullIntersection(src, &tr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX);
			VecCopy(tr.end, end);	/* the point on the actual surface */
		}
	}
	if (fFirst)
	{
		/* EV_Crowbar */
		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, RESID(W_CBAR_MISS1), 1, ATTN_NORM, 0, PITCH_NORM);
		switch ((g_iSwing++) % 3)
		{
		case 0: W_SendWeaponAnim(CROWBAR_ATTACK1MISS); break;
		case 1: W_SendWeaponAnim(CROWBAR_ATTACK2MISS); break;
		case 2: W_SendWeaponAnim(CROWBAR_ATTACK3MISS); break;
		}
	}
	if (tr.frac >= 1.0f)
	{
		if (fFirst)
		{
			/* miss */
			w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.5f);
			Player_SetAnimationAttack();
		}
	}
	else
	{
		float flVol = 1.0f;
		int fHitWorld = 1;

		switch (((w->m_iSwing++) % 2) + 1)
		{
		case 0: W_SendWeaponAnim(CROWBAR_ATTACK1HIT); break;
		case 1: W_SendWeaponAnim(CROWBAR_ATTACK2HIT); break;
		case 2: W_SendWeaponAnim(CROWBAR_ATTACK3HIT); break;
		}
		Player_SetAnimationAttack();
		fDidHit = 1;
		hit = tr.ent >= 0 ? tr.ent : 0;
		ClearMultiDamage();
		if (w->m_flNextPrimaryAttack + 1.0f <= 0)
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmgCrowbar, gpGlobals.v_forward, &tr,
				DMG_CLUB);
		else
			/* subsequent swings do half */
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmgCrowbar / 2, gpGlobals.v_forward,
				&tr, DMG_CLUB);
		ApplyMultiDamage(ENT_PLAYER, ENT_PLAYER);
		/* play thwack, smack, or dong sound */
		if (hit > 0 && ent_is_live(hit))
		{
			int cls = Classify(hit);

			if (cls != CLASS_NONE && cls != CLASS_MACHINE)
			{
				switch (RANDOM_LONG(0, 2))
				{
				case 0: EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(W_CBAR_HITBOD1), 1, ATTN_NORM); break;
				case 1: EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(W_CBAR_HITBOD2), 1, ATTN_NORM); break;
				case 2: EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(W_CBAR_HITBOD3), 1, ATTN_NORM); break;
				}
				Player()->m_iWeaponVolume = CROWBAR_BODYHIT_VOLUME;
				if (!IsAlive(hit))
					return 1;
				flVol = 0.1f;
				fHitWorld = 0;
			}
		}
		/* play texture hit sound */
		if (fHitWorld)
		{
			float e2[3], fvolbar;

			VecSub(end, src, e2);
			VecMA(src, 2, e2, e2);
			fvolbar = TEXTURETYPE_PlaySound(&tr, src, e2, BULLET_PLAYER_CROWBAR);
			switch (RANDOM_LONG(0, 1))
			{
			case 0:
				EMIT_SOUND_DYN(ENT_PLAYER, CHAN_ITEM, RESID(W_CBAR_HIT1), fvolbar, ATTN_NORM, 0,
					98 + RANDOM_LONG(0, 3));
				break;
			case 1:
				EMIT_SOUND_DYN(ENT_PLAYER, CHAN_ITEM, RESID(W_CBAR_HIT2), fvolbar, ATTN_NORM, 0,
					98 + RANDOM_LONG(0, 3));
				break;
			}
			/* delay the decal a bit */
			w->m_trHit = tr;
		}
		Player()->m_iWeaponVolume = (int16_t)(flVol * CROWBAR_WALLHIT_VOLUME);
		w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.25f);
		w->m_iThink = THINK_SMACK;
		w->m_flThinkTime = gtime() + 0.2f;
	}
	return fDidHit;
}

static void PrimaryAttack(int id)
{
	CBasePlayerWeapon *w = &Player()->wpn[id];

	if (!Swing(id, 1))
	{
		w->m_iThink = THINK_SWINGAGAIN;
		w->m_flThinkTime = gtime() + 0.1f;
	}
}

static void Think(int id, int which)
{
	if (which == THINK_SMACK)
		DecalGunshot(&Player()->wpn[id].m_trHit, BULLET_PLAYER_CROWBAR);
	else if (which == THINK_SWINGAGAIN)
		Swing(id, 0);
}

const weapon_vt_t wvt_crowbar = {
	.Deploy = Deploy,
	.Holster = Holster,
	.PrimaryAttack = PrimaryAttack,
	.Think = Think,
};
