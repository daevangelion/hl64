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
 * weapon_tripmine.c -- CTripmine (HLSDK dlls/tripmine.cpp), the player's
 * tripmine weapon, with its client event EV_TripmineFire
 * (cl_dll/ev_hldm.cpp: the draw animation when the aim line hits
 * something within 128 units). The mine it places is monster_tripmine
 * (game/hl/tripmine.c); the world weapon is weapons.c's weapon_tripmine.
 */
#include "game/hl/player.h"

enum {
	TRIPMINE_IDLE1 = 0, TRIPMINE_IDLE2, TRIPMINE_ARM1, TRIPMINE_ARM2, TRIPMINE_FIDGET,
	TRIPMINE_HOLSTER, TRIPMINE_DRAW, TRIPMINE_WORLD, TRIPMINE_GROUND
};

static int Deploy(int id)
{
	Player()->viewbody = 0;
	return W_DefaultDeploy(id, TRIPMINE_DRAW);
}

static void Holster(int id)
{
	CBasePlayer *p = Player();

	p->m_flNextAttack = 0.5f;
	if (!p->m_rgAmmo[AMMO_TRIPMINE])
		/* out of mines (DestroyItem) */
		p->weapons &= ~(1u << id);
	W_SendWeaponAnim(TRIPMINE_HOLSTER);
	EMIT_SOUND(ENT_PLAYER, CHAN_WEAPON, RESID(SND_NULL), 1.0f, ATTN_NORM);
}

static void PrimaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	col_trace_t tr;
	float src[3], end[3];

	if (p->m_rgAmmo[AMMO_TRIPMINE] <= 0)
		return;
	Player_MakeAimVectors();
	Player_GunPosition(src);
	VecMA(src, 128, gpGlobals.v_forward, end);
	UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tr);
	if (tr.frac < 1.0f)
	{
		int hit = tr.ent >= 0 ? tr.ent : 0;

		/* EV_TripmineFire: the local player's view model draws again
		 * when the line hit something solid */
		W_SendWeaponAnim(TRIPMINE_DRAW);
		/* CBaseEntity::Instance: the world (slot 0) or a live entity */
		if (hit == 0 || (ent_is_live(hit) && !(ent_cv(hit)->flags & FL_CONVEYOR)))
		{
			float org[3], angles[3];
			int mine;

			UTIL_VecToAngles(tr.normal, angles);
			VecMA(tr.end, 8, tr.normal, org);
			mine = CBaseEntity_Create(CLS_MONSTER_TRIPMINE, org, angles, ENT_PLAYER);
			p->m_rgAmmo[AMMO_TRIPMINE]--;
			log_printf("tripmine: placed %d at %.1f %.1f %.1f on %d at %.3f, %d left\n",
				mine, (double)org[0], (double)org[1], (double)org[2], hit,
				(double)gtime(), p->m_rgAmmo[AMMO_TRIPMINE]);
			/* player "shoot" animation */
			Player_SetAnimationAttack();
			if (p->m_rgAmmo[AMMO_TRIPMINE] <= 0)
			{
				/* no more mines! */
				W_RetireWeapon(id);
				return;
			}
		}
	}
	w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.3f);
	w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float flRand;
	int iAnim;

	if (w->m_flTimeWeaponIdle > 0)
		return;
	if (p->m_rgAmmo[AMMO_TRIPMINE] > 0)
		W_SendWeaponAnim(TRIPMINE_DRAW);
	else
	{
		W_RetireWeapon(id);
		return;
	}
	flRand = UTIL_SharedRandomFloat(p->random_seed, 0, 1);
	if (flRand <= 0.25f)
	{
		iAnim = TRIPMINE_IDLE1;
		w->m_flTimeWeaponIdle = 90.0f / 30.0f;
	}
	else if (flRand <= 0.75f)
	{
		iAnim = TRIPMINE_IDLE2;
		w->m_flTimeWeaponIdle = 60.0f / 30.0f;
	}
	else
	{
		iAnim = TRIPMINE_FIDGET;
		w->m_flTimeWeaponIdle = 100.0f / 30.0f;
	}
	W_SendWeaponAnim(iAnim);
}

const weapon_vt_t wvt_tripmine = {
	.Deploy = Deploy,
	.Holster = Holster,
	.PrimaryAttack = PrimaryAttack,
	.WeaponIdle = WeaponIdle,
};
