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
 * weapon_mp5.c -- CMP5, the 9mmAR with the M203 (HLSDK dlls/mp5.cpp) with
 * its client events EV_FireMP5 / EV_FireMP52 (cl_dll/ev_hldm.cpp).
 */
#include "game/hl/player.h"
#include "game/hl/grenade.h"

enum {
	MP5_LONGIDLE = 0, MP5_IDLE1, MP5_LAUNCH, MP5_RELOAD, MP5_DEPLOY, MP5_FIRE1, MP5_FIRE2,
	MP5_FIRE3
};

static int Deploy(int id)
{
	return W_DefaultDeploy(id, MP5_DEPLOY);
}

static void PrimaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	static const float VECTOR_CONE_3DEG[3] = { 0.02618f, 0.02618f, 0.02618f };
	float src[3], aim[3], dir[2];
	res_t s;

	/* don't fire underwater */
	if (phy_player()->waterlevel == 3)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = 0.15f;
		return;
	}
	if (w->m_iClip <= 0)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = 0.15f;
		return;
	}
	p->m_iWeaponVolume = NORMAL_GUN_VOLUME;
	p->m_iWeaponFlash = NORMAL_GUN_FLASH;
	w->m_iClip--;
	ent_rnd(ENT_PLAYER)->effects |= EF_MUZZLEFLASH;
	Player_SetAnimationAttack();
	Player_GunPosition(src);
	W_AutoaimVector(0, aim);
	W_FireBulletsPlayer(1, src, aim, VECTOR_CONE_3DEG, 8192, BULLET_PLAYER_MP5, 2, 0, dir);
	/* EV_FireMP5 */
	fx_muzzle_flash(ENT_PLAYER, 0, 0);
	W_SendWeaponAnim(MP5_FIRE1 + RANDOM_LONG(0, 2));
	W_PunchAxis(0, RANDOM_FLOAT(-2, 2));
	W_EjectShell(RESID(W_SHELL), 20, -12, 4, 1);
	/* draws in source order (argument order is unspecified, K10) */
	s = RANDOM_LONG(0, 1) ? RESID(W_HKS2) : RESID(W_HKS1);
	EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, s, 1, ATTN_NORM, 0, 94 + RANDOM_LONG(0, 0xf));
	if (!w->m_iClip && p->m_rgAmmo[AMMO_9MM] <= 0)
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
	w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 0.1f);
	if (w->m_flNextPrimaryAttack < 0)
		w->m_flNextPrimaryAttack = 0.1f;
	w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

static void SecondaryAttack(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	const ent_player_t *pl = phy_player();
	float src[3], vel[3];
	res_t s;

	/* don't fire underwater */
	if (pl->waterlevel == 3)
	{
		W_PlayEmptySound(id);
		w->m_flNextPrimaryAttack = 0.15f;
		return;
	}
	if (p->m_rgAmmo[AMMO_ARGRENADES] == 0)
	{
		W_PlayEmptySound(id);
		return;
	}
	p->m_iWeaponVolume = NORMAL_GUN_VOLUME;
	p->m_iWeaponFlash = BRIGHT_GUN_FLASH;
	p->m_iExtraSoundTypes = bits_SOUND_DANGER;
	p->m_flStopExtraSoundTime = gtime() + 0.2f;
	p->m_rgAmmo[AMMO_ARGRENADES]--;
	Player_SetAnimationAttack();
	Player_MakeAimVectors();
	/* we don't add in player velocity anymore */
	Player_GunPosition(src);
	VecMA(src, 16, gpGlobals.v_forward, src);
	VecScale(gpGlobals.v_forward, 800, vel);
	CGrenade_ShootContact(ENT_PLAYER, src, vel);
	/* EV_FireMP52 */
	W_SendWeaponAnim(MP5_LAUNCH);
	W_PunchAxis(0, -10);
	s = RANDOM_LONG(0, 1) ? RESID(W_GLAUNCHER2) : RESID(W_GLAUNCHER);	/* source order (K10) */
	EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, s, 1, ATTN_NORM, 0, 94 + RANDOM_LONG(0, 0xf));
	w->m_flNextPrimaryAttack = W_GetNextAttackDelay(id, 1);
	w->m_flNextSecondaryAttack = 1;
	w->m_flTimeWeaponIdle = 5;	/* idle pretty soon after shooting */
	if (!p->m_rgAmmo[AMMO_ARGRENADES])
		Player_SetSuitUpdate(RESID(SEN_HEV_AMO0), 0);
}

static void Reload(int id)
{
	if (Player()->m_rgAmmo[AMMO_9MM] <= 0)
		return;
	W_DefaultReload(id, MP5_MAX_CLIP, MP5_RELOAD, 1.5f);
}

static void WeaponIdle(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = &p->wpn[id];
	float aim[3];

	W_ResetEmptySound(id);
	W_AutoaimVector(0, aim);
	if (w->m_flTimeWeaponIdle > 0)
		return;
	W_SendWeaponAnim(RANDOM_LONG(0, 1) ? MP5_IDLE1 : MP5_LONGIDLE);
	w->m_flTimeWeaponIdle = UTIL_SharedRandomFloat(p->random_seed, 10, 15);
}

const weapon_vt_t wvt_mp5 = {
	.Deploy = Deploy,
	.PrimaryAttack = PrimaryAttack,
	.SecondaryAttack = SecondaryAttack,
	.Reload = Reload,
	.WeaponIdle = WeaponIdle,
};
