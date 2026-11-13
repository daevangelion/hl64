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
 * weapon_python.c -- CPython, the .357 (HLSDK dlls/python.cpp) with its
 * client event EV_FirePython (cl_dll/ev_hldm.cpp). Single player: no
 * zoom (SecondaryAttack returns), body 0.
 */
#include "game/hl/player.h"

enum {
	PYTHON_IDLE1 = 0, PYTHON_FIDGET, PYTHON_FIRE1, PYTHON_RELOAD, PYTHON_HOLSTER, PYTHON_DRAW,
	PYTHON_IDLE2, PYTHON_IDLE3
};

static int Deploy(int id)
{
	Player()->viewbody = 0;
	return W_DefaultDeploy(id, PYTHON_DRAW);
}

static void Holster(int id)
{
	CBasePlayer *p = Player();

	p->wpn[id].m_fInReload = 0;
	p->m_flNextAttack = 1.0f;
	p->wpn[id].m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
	W_SendWeaponAnim(PYTHON_HOLSTER);
}

static void PrimaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	static const float VECTOR_CONE_1DEG[3] = { 0.00873f, 0.00873f, 0.00873f };
	float src[3], aim[3], dir[2];

	/* don't fire underwater */
	if (phy_player()->waterlevel == 3)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = 0.15f;
		return;
	}
	if (w->m_iClip <= 0)
	{
		if (!w->m_fFireOnEmpty)
			wvt_python.Reload(id);
		else
		{
			W_PlayEmptySound(id);
			w->m_flNextPrimaryAttack = 0.15f;
		}
		return;
	}
	p->m_iWeaponVolume = LOUD_GUN_VOLUME;
	p->m_iWeaponFlash = BRIGHT_GUN_FLASH;
	w->m_iClip--;
	ent_rnd(ENT_PLAYER)->effects |= EF_MUZZLEFLASH;
	Player_SetAnimationAttack();
	Player_MakeAimVectors();
	Player_GunPosition(src);
	W_AutoaimVector(1, aim);
	W_FireBulletsPlayer(1, src, aim, VECTOR_CONE_1DEG, 8192, BULLET_PLAYER_357, 0, 0, dir);
	/* EV_FirePython */
	fx_muzzle_flash(ENT_PLAYER, 0, 0);
	W_SendWeaponAnim(PYTHON_FIRE1);
	W_PunchAxis(0, -10.0f);
	{
		/* draws in source order (argument order is unspecified, K10) */
		res_t s = RANDOM_LONG(0, 1) ? RESID(W_357_SHOT2) : RESID(W_357_SHOT1);

		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, s, RANDOM_FLOAT(0.8f, 0.9f), ATTN_NORM, 0,
			PITCH_NORM);
	}
	if (!w->m_iClip && p->m_rgAmmo[AMMO_357] <= 0)
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
	w->m_flNextPrimaryAttack = 0.75f;
	w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

static void Reload(int id)
{
	if (Player()->m_rgAmmo[AMMO_357] <= 0)
		return;
	W_DefaultReload(id, 6, PYTHON_RELOAD, 2.0f);
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float aim[3], flRand;
	int anim;

	W_ResetEmptySound(id);
	W_AutoaimVector(1, aim);
	if (w->m_flTimeWeaponIdle > 0)
		return;
	flRand = UTIL_SharedRandomFloat(p->random_seed, 0, 1);
	if (flRand <= 0.5f)
	{
		anim = PYTHON_IDLE1;
		w->m_flTimeWeaponIdle = 70.0f / 30.0f;
	}
	else if (flRand <= 0.7f)
	{
		anim = PYTHON_IDLE2;
		w->m_flTimeWeaponIdle = 60.0f / 30.0f;
	}
	else if (flRand <= 0.9f)
	{
		anim = PYTHON_IDLE3;
		w->m_flTimeWeaponIdle = 88.0f / 30.0f;
	}
	else
	{
		anim = PYTHON_FIDGET;
		w->m_flTimeWeaponIdle = 170.0f / 30.0f;
	}
	W_SendWeaponAnim(anim);
}

const weapon_vt_t wvt_python = {
	.Deploy = Deploy,
	.Holster = Holster,
	.PrimaryAttack = PrimaryAttack,
	.Reload = Reload,
	.WeaponIdle = WeaponIdle,
};
