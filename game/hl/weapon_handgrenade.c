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
 * weapon_handgrenade.c -- CHandGrenade (HLSDK dlls/handgrenade.cpp).
 */
#include "game/hl/player.h"
#include "game/hl/grenade.h"

enum {
	HANDGRENADE_IDLE = 0, HANDGRENADE_FIDGET, HANDGRENADE_PINPULL, HANDGRENADE_THROW1,
	HANDGRENADE_THROW2, HANDGRENADE_THROW3, HANDGRENADE_HOLSTER, HANDGRENADE_DRAW
};

static int Deploy(int id)
{
	Player()->wpn[id].m_flReleaseThrow = -1;
	return W_DefaultDeploy(id, HANDGRENADE_DRAW);
}

static int CanHolster(int id)
{
	/* can only holster hand grenades when not primed! */
	return Player()->wpn[id].m_flStartThrow == 0;
}

static void Holster(int id)
{
	CBasePlayer *p = Player();

	p->m_flNextAttack = 0.5f;
	if (p->m_rgAmmo[AMMO_HANDGRENADE])
		W_SendWeaponAnim(HANDGRENADE_HOLSTER);
	else
		/* no more grenades! (DestroyItem) */
		p->weapons &= ~(1u << id);
	EMIT_SOUND(ENT_PLAYER, CHAN_WEAPON, RESID(SND_NULL), 1.0f, ATTN_NORM);
}

static void PrimaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];

	if (!w->m_flStartThrow && p->m_rgAmmo[AMMO_HANDGRENADE] > 0)
	{
		w->m_flStartThrow = gtime();
		w->m_flReleaseThrow = 0;
		W_SendWeaponAnim(HANDGRENADE_PINPULL);
		w->m_flTimeWeaponIdle = 0.5f;
	}
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	const ent_player_t *pl = phy_player();

	if (w->m_flReleaseThrow == 0 && w->m_flStartThrow)
		w->m_flReleaseThrow = gtime();
	if (w->m_flTimeWeaponIdle > 0)
		return;
	if (w->m_flStartThrow)
	{
		float ang[3], src[3], vel[3], flVel, time;

		VecAdd(pl->view_angles, pl->punchangle, ang);
		if (ang[0] < 0)
			ang[0] = -10 + ang[0] * ((90 - 10) / 90.0f);
		else
			ang[0] = -10 + ang[0] * ((90 + 10) / 90.0f);
		flVel = (90 - ang[0]) * 6.5f;
		if (flVel > 1000)
			flVel = 1000;
		UTIL_MakeVectors(ang);
		VecAdd(pl->origin, pl->view_ofs, src);
		VecMA(src, 16, gpGlobals.v_forward, src);
		VecMA(pl->velocity, flVel, gpGlobals.v_forward, vel);
		/* always explode 3 seconds after the pin was pulled */
		time = w->m_flStartThrow - gtime() + 3.0f;
		if (time < 0)
			time = 0;
		CGrenade_ShootTimed(ENT_PLAYER, src, vel, time);
		if (flVel < 500)
			W_SendWeaponAnim(HANDGRENADE_THROW1);
		else if (flVel < 1000)
			W_SendWeaponAnim(HANDGRENADE_THROW2);
		else
			W_SendWeaponAnim(HANDGRENADE_THROW3);
		Player_SetAnimationAttack();
		w->m_flReleaseThrow = 0;
		w->m_flStartThrow = 0;
		w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.5f);
		w->m_flTimeWeaponIdle = 0.5f;
		p->m_rgAmmo[AMMO_HANDGRENADE]--;
		if (!p->m_rgAmmo[AMMO_HANDGRENADE])
			/* just threw last grenade: ensure that the animation can
			 * finish playing */
			w->m_flTimeWeaponIdle = w->m_flNextSecondaryAttack = w->m_flNextPrimaryAttack =
				W_GetNextAttackDelay(id, 0.5f);
		return;
	}
	else if (w->m_flReleaseThrow > 0)
	{
		/* we've finished the throw, restart */
		w->m_flStartThrow = 0;
		if (p->m_rgAmmo[AMMO_HANDGRENADE])
			W_SendWeaponAnim(HANDGRENADE_DRAW);
		else
		{
			W_RetireWeapon(id);
			return;
		}
		w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
		w->m_flReleaseThrow = -1;
		return;
	}
	if (p->m_rgAmmo[AMMO_HANDGRENADE])
	{
		float flRand = UTIL_SharedRandomFloat(p->random_seed, 0, 1);

		if (flRand <= 0.75f)
		{
			W_SendWeaponAnim(HANDGRENADE_IDLE);
			w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
		}
		else
		{
			W_SendWeaponAnim(HANDGRENADE_FIDGET);
			w->m_flTimeWeaponIdle = 75.0f / 30.0f;
		}
	}
}

const weapon_vt_t wvt_handgrenade = {
	.Deploy = Deploy,
	.Holster = Holster,
	.PrimaryAttack = PrimaryAttack,
	.WeaponIdle = WeaponIdle,
	.CanHolster = CanHolster,
};
