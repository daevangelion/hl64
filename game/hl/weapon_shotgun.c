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
 * weapon_shotgun.c -- CShotgun (HLSDK dlls/shotgun.cpp) with its client
 * events EV_FireShotGunSingle / EV_FireShotGunDouble (cl_dll/ev_hldm.cpp;
 * single player: 6 / 12 pellets in a 10 degree cone).
 */
#include "game/hl/player.h"

enum {
	SHOTGUN_IDLE = 0, SHOTGUN_FIRE, SHOTGUN_FIRE2, SHOTGUN_RELOAD, SHOTGUN_PUMP,
	SHOTGUN_START_RELOAD, SHOTGUN_DRAW, SHOTGUN_HOLSTER, SHOTGUN_IDLE4, SHOTGUN_IDLE_DEEP
};

static const float VECTOR_CONE_10DEG[3] = { 0.08716f, 0.08716f, 0.08716f };

static int Deploy(int id)
{
	return W_DefaultDeploy(id, SHOTGUN_DRAW);
}

static void Reload(int id);

static void fire(int id, int pellets, int shells, int anim, float punch, res_t snd, float vlo,
	int pitch)
{
	float src[3], aim[3], dir[2];
	int i;

	ent_rnd(ENT_PLAYER)->effects |= EF_MUZZLEFLASH;
	Player_GunPosition(src);
	W_AutoaimVector(0, aim);
	W_FireBulletsPlayer(pellets, src, aim, VECTOR_CONE_10DEG, 2048, BULLET_PLAYER_BUCKSHOT, 0, 0,
		dir);
	/* EV_FireShotGunSingle / Double */
	fx_muzzle_flash(ENT_PLAYER, 0, 0);
	W_SendWeaponAnim(anim);
	W_PunchAxis(0, punch);
	for (i = 0; i < shells; i++)
		W_EjectShell(RESID(W_SHOTGUNSHELL), 32, -12, 6, 2);
	EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, snd, RANDOM_FLOAT(vlo, 1.0f), ATTN_NORM, 0, pitch);
	(void)id;
}

static void PrimaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];

	/* don't fire underwater */
	if (phy_player()->waterlevel == 3)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.15f);
		return;
	}
	if (w->m_iClip <= 0)
	{
		Reload(id);
		if (w->m_iClip == 0)
			W_PlayEmptySound(id);
		return;
	}
	p->m_iWeaponVolume = LOUD_GUN_VOLUME;
	p->m_iWeaponFlash = NORMAL_GUN_FLASH;
	w->m_iClip--;
	fire(id, 6, 1, SHOTGUN_FIRE, -5.0f, RESID(W_SBARREL1), 0.95f, 93 + RANDOM_LONG(0, 0x1f));
	if (!w->m_iClip && p->m_rgAmmo[AMMO_BUCKSHOT] <= 0)
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
	w->m_flPumpTime = gtime() + 0.5f;
	w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.75f);
	w->m_flNextSecondaryAttack = 0.75f;
	w->m_flTimeWeaponIdle = w->m_iClip != 0 ? 5.0f : 0.75f;
	w->m_fInSpecialReload = 0;
}

static void SecondaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];

	/* don't fire underwater */
	if (phy_player()->waterlevel == 3)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.15f);
		return;
	}
	if (w->m_iClip <= 1)
	{
		Reload(id);
		W_PlayEmptySound(id);
		return;
	}
	p->m_iWeaponVolume = LOUD_GUN_VOLUME;
	p->m_iWeaponFlash = NORMAL_GUN_FLASH;
	w->m_iClip -= 2;
	Player_SetAnimationAttack();
	fire(id, 12, 2, SHOTGUN_FIRE2, -10.0f, RESID(W_DBARREL1), 0.98f,
		85 + RANDOM_LONG(0, 0x1f));
	if (!w->m_iClip && p->m_rgAmmo[AMMO_BUCKSHOT] <= 0)
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
	w->m_flPumpTime = gtime() + 0.95f;
	w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 1.5f);
	w->m_flNextSecondaryAttack = 1.5f;
	w->m_flTimeWeaponIdle = w->m_iClip != 0 ? 6.0f : 1.5f;
	w->m_fInSpecialReload = 0;
}

static void Reload(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];

	if (p->m_rgAmmo[AMMO_BUCKSHOT] <= 0 || w->m_iClip == SHOTGUN_MAX_CLIP)
		return;
	/* don't reload until recoil is done */
	if (w->m_flNextPrimaryAttack > 0)
		return;
	/* check to see if we're ready to reload */
	if (w->m_fInSpecialReload == 0)
	{
		W_SendWeaponAnim(SHOTGUN_START_RELOAD);
		w->m_fInSpecialReload = 1;
		p->m_flNextAttack = 0.6f;
		w->m_flTimeWeaponIdle = 0.6f;
		w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 1.0f);
		w->m_flNextSecondaryAttack = 1.0f;
		return;
	}
	else if (w->m_fInSpecialReload == 1)
	{
		if (w->m_flTimeWeaponIdle > 0)
			return;
		/* was waiting for gun to move to side */
		w->m_fInSpecialReload = 2;
		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_ITEM, RANDOM_LONG(0, 1) ? RESID(W_RELOAD1)
			: RESID(W_RELOAD3), 1, ATTN_NORM, 0, 85 + RANDOM_LONG(0, 0x1f));
		W_SendWeaponAnim(SHOTGUN_RELOAD);
		w->m_flNextReload = 0.5f;
		w->m_flTimeWeaponIdle = 0.5f;
	}
	else
	{
		/* add them to the clip */
		w->m_iClip += 1;
		p->m_rgAmmo[AMMO_BUCKSHOT] -= 1;
		w->m_fInSpecialReload = 1;
	}
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float aim[3];

	W_ResetEmptySound(id);
	W_AutoaimVector(0, aim);
	if (w->m_flTimeWeaponIdle >= 0)
		return;
	if (w->m_iClip == 0 && w->m_fInSpecialReload == 0 && p->m_rgAmmo[AMMO_BUCKSHOT])
		Reload(id);
	else if (w->m_fInSpecialReload != 0)
	{
		if (w->m_iClip != 8 && p->m_rgAmmo[AMMO_BUCKSHOT])
			Reload(id);
		else
		{
			/* reload debounce has timed out */
			W_SendWeaponAnim(SHOTGUN_PUMP);
			EMIT_SOUND_DYN(ENT_PLAYER, CHAN_ITEM, RESID(W_SCOCK1), 1, ATTN_NORM, 0,
				95 + RANDOM_LONG(0, 0x1f));
			w->m_fInSpecialReload = 0;
			w->m_flTimeWeaponIdle = 1.5f;
		}
	}
	else
	{
		float flRand = UTIL_SharedRandomFloat(p->random_seed, 0, 1);
		int anim;

		if (flRand <= 0.8f)
		{
			anim = SHOTGUN_IDLE_DEEP;
			w->m_flTimeWeaponIdle = 60.0f / 12.0f;
		}
		else if (flRand <= 0.95f)
		{
			anim = SHOTGUN_IDLE;
			w->m_flTimeWeaponIdle = 20.0f / 9.0f;
		}
		else
		{
			anim = SHOTGUN_IDLE4;
			w->m_flTimeWeaponIdle = 20.0f / 9.0f;
		}
		W_SendWeaponAnim(anim);
	}
}

static void ItemPostFrame(int id)
{
	CBasePlayerWeapon *w = &Player()->wpn[id];

	if (w->m_flPumpTime && w->m_flPumpTime < gtime())
	{
		/* play pumping sound */
		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_ITEM, RESID(W_SCOCK1), 1, ATTN_NORM, 0,
			95 + RANDOM_LONG(0, 0x1f));
		w->m_flPumpTime = 0;
	}
	W_BaseItemPostFrame(id);
}

const weapon_vt_t wvt_shotgun = {
	.Deploy = Deploy,
	.PrimaryAttack = PrimaryAttack,
	.SecondaryAttack = SecondaryAttack,
	.Reload = Reload,
	.WeaponIdle = WeaponIdle,
	.ItemPostFrame = ItemPostFrame,
};
