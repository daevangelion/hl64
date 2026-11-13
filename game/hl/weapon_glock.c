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
 * weapon_glock.c -- CGlock (HLSDK dlls/wpn_shared/hl_wpn_glock.cpp) with
 * its client events EV_FireGlock1 / EV_FireGlock2 (cl_dll/ev_hldm.cpp).
 */
#include "game/hl/player.h"

enum {
	GLOCK_IDLE1 = 0, GLOCK_IDLE2, GLOCK_IDLE3, GLOCK_SHOOT, GLOCK_SHOOT_EMPTY, GLOCK_RELOAD,
	GLOCK_RELOAD_NOT_EMPTY, GLOCK_DRAW, GLOCK_HOLSTER, GLOCK_ADD_SILENCER
};

static int Deploy(int id)
{
	return W_DefaultDeploy(id, GLOCK_DRAW);
}

static void GlockFire(int id, float flSpread, float flCycleTime, int fUseAutoAim)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float src[3], aim[3], spread[3] = { flSpread, flSpread, flSpread }, dir[2];

	if (w->m_iClip <= 0)
	{
		if (w->m_fFireOnEmpty)
		{
			W_PlayEmptySound(id);
			w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.2f);
		}
		return;
	}
	w->m_iClip--;
	p->m_iWeaponVolume = NORMAL_GUN_VOLUME;
	p->m_iWeaponFlash = NORMAL_GUN_FLASH;
	ent_rnd(ENT_PLAYER)->effects |= EF_MUZZLEFLASH;
	Player_SetAnimationAttack();
	Player_GunPosition(src);
	if (fUseAutoAim)
		W_AutoaimVector(1, aim);
	else
		VecCopy(gpGlobals.v_forward, aim);
	W_FireBulletsPlayer(1, src, aim, spread, 8192, BULLET_PLAYER_9MM, 0, 0, dir);
	/* EV_FireGlock1 / 2 */
	fx_muzzle_flash(ENT_PLAYER, 0, 0);
	W_SendWeaponAnim(fUseAutoAim && w->m_iClip == 0 ? GLOCK_SHOOT_EMPTY : GLOCK_SHOOT);
	W_PunchAxis(0, -2.0f);
	W_EjectShell(RESID(W_SHELL), 20, -12, 4, 1);
	EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, RESID(W_PL_GUN3), RANDOM_FLOAT(0.92f, 1.0f), ATTN_NORM,
		0, 98 + RANDOM_LONG(0, 3));
	w->m_flNextPrimaryAttack = w->m_flNextSecondaryAttack = W_GetNextAttackDelay(id, flCycleTime);
	if (!w->m_iClip && p->m_rgAmmo[AMMO_9MM] <= 0)
		/* HEV suit - indicate out of ammo condition */
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
	w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

static void PrimaryAttack(int id)
{
	GlockFire(id, 0.01f, 0.3f, 1);
}

static void SecondaryAttack(int id)
{
	GlockFire(id, 0.1f, 0.2f, 0);
}

static void Reload(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];

	if (p->m_rgAmmo[AMMO_9MM] <= 0)
		return;
	if (W_DefaultReload(id, GLOCK_MAX_CLIP, w->m_iClip > 0 ? GLOCK_RELOAD_NOT_EMPTY
		: GLOCK_RELOAD, 1.5f))
		w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float aim[3];

	W_ResetEmptySound(id);
	W_AutoaimVector(1, aim);
	if (w->m_flTimeWeaponIdle > 0)
		return;
	/* only idle if the slid isn't back */
	if (w->m_iClip != 0)
	{
		float flRand = UTIL_SharedRandomFloat(p->random_seed, 0.0f, 1.0f);
		int anim;

		if (flRand <= 0.3f)
		{
			anim = GLOCK_IDLE3;
			w->m_flTimeWeaponIdle = 49.0f / 16;
		}
		else if (flRand <= 0.6f)
		{
			anim = GLOCK_IDLE1;
			w->m_flTimeWeaponIdle = 60.0f / 16.0f;
		}
		else
		{
			anim = GLOCK_IDLE2;
			w->m_flTimeWeaponIdle = 40.0f / 16.0f;
		}
		W_SendWeaponAnim(anim);
	}
}

const weapon_vt_t wvt_glock = {
	.Deploy = Deploy,
	.PrimaryAttack = PrimaryAttack,
	.SecondaryAttack = SecondaryAttack,
	.Reload = Reload,
	.WeaponIdle = WeaponIdle,
};
