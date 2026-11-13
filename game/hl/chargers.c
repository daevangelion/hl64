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
 * chargers.c -- the wall chargers: CWallHealth (func_healthcharger, HLSDK
 * dlls/healthkit.cpp) and CRecharge (func_recharge, h_battery.cpp).
 * Single player: FlHealthChargerRechargeTime / FlHEVChargerRechargeTime
 * are 0, so an empty charger stays empty (the dmdelay key only matters in
 * deathmatch and is not cooked).
 */
#include "game/hl/player.h"

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	float		m_flNextCharge;
	int		m_iReactivate;	/* DeathMatch delay until reactivated */
	int		m_iJuice;
	int		m_iOn;		/* 0 = off, 1 = startup, 2 = going */
	float		m_flSoundTime;
} CWallCharger;

#define WC(e)	PRIV(CWallCharger, e)

static int charger_caps(int e)
{
	(void)e;
	/* (CBaseToggle::ObjectCaps() | FCAP_CONTINUOUS_USE) & ~FCAP_ACROSS_TRANSITION */
	return FCAP_CONTINUOUS_USE;
}

static const game_vt_t charger_vt = { .ObjectCaps = charger_caps };

static int charger_spawn(int e, gid_t16 gid, float juice)
{
	CWallCharger *c = WC(e);

	CBaseToggle_KeyValue(e, gid);
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, SOLID_BSP);
	UTIL_SetOrigin(e, ent_cv(e)->origin);	/* set size and link into world */
	ent_set_model(e, c->model);
	c->m_iJuice = (int)juice;
	ent_rnd(e)->frame = 0;
	return 0;
}

/* ---- func_healthcharger (CWallHealth) --------------------------------------------- */

void CWallHealth_Off(int e)
{
	CWallCharger *c = WC(e);

	/* stop looping sound */
	if (c->m_iOn > 1)
		STOP_SOUND(e, CHAN_STATIC, RESID(I_MEDCHARGE4));
	c->m_iOn = 0;
	SetThink(e, FN_SUB_DoNothing);
}

void CWallHealth_Recharge(int e)
{
	CWallCharger *c = WC(e);

	EMIT_SOUND(e, CHAN_ITEM, RESID(I_MEDSHOT4), 1.0f, ATTN_NORM);
	c->m_iJuice = (int)gSkillData.healthchargerCapacity;
	ent_rnd(e)->frame = 0;
	SetThink(e, FN_SUB_DoNothing);
}

void CWallHealth_Use(int e, int activator, int caller, int use_type, float value)
{
	CWallCharger *c = WC(e);

	(void)caller; (void)use_type; (void)value;
	/* make sure that we have a caller, and that it's a player */
	if (activator < 0 || !IsPlayer(activator) || !PlayerMon())
		return;
	/* if there is no juice left, turn it off */
	if (c->m_iJuice <= 0)
	{
		ent_rnd(e)->frame = 1;
		CWallHealth_Off(e);
	}
	/* if the activator doesn't have a suit, or there is no juice left,
	 * make the deny noise */
	if (c->m_iJuice <= 0 || !(Player()->weapons & (1u << WEAPON_SUIT)))
	{
		if (c->m_flSoundTime <= gtime())
		{
			c->m_flSoundTime = gtime() + 0.62f;
			EMIT_SOUND(e, CHAN_ITEM, RESID(I_MEDSHOTNO1), 1.0f, ATTN_NORM);
		}
		return;
	}
	ThinkInLtime(e, 0.25f);
	SetThink(e, FN_CWallHealth_Off);
	/* time to recharge yet? */
	if (c->m_flNextCharge >= gtime())
		return;
	/* play the on sound or the looping charging sound */
	if (!c->m_iOn)
	{
		c->m_iOn++;
		EMIT_SOUND(e, CHAN_ITEM, RESID(I_MEDSHOT4), 1.0f, ATTN_NORM);
		c->m_flSoundTime = 0.56f + gtime();
	}
	if (c->m_iOn == 1 && c->m_flSoundTime <= gtime())
	{
		c->m_iOn++;
		EMIT_SOUND(e, CHAN_STATIC, RESID(I_MEDCHARGE4), 1.0f, ATTN_NORM);
	}
	/* charge the player */
	if (TakeHealth(activator, 1, DMG_GENERIC))
		c->m_iJuice--;
	/* govern the rate of charge */
	c->m_flNextCharge = gtime() + 0.1f;
}

static int CWallHealth_Spawn(int e, gid_t16 gid)
{
	SetUse(e, FN_CWallHealth_Use);
	return charger_spawn(e, gid, gSkillData.healthchargerCapacity);
}

const game_class_t gc_FUNC_HEALTHCHARGER = {
	.size = sizeof(CWallCharger),
	.Spawn = CWallHealth_Spawn,
	.vt = &charger_vt,
};

/* ---- func_recharge (CRecharge) ------------------------------------------------------ */

void CRecharge_Off(int e)
{
	CWallCharger *c = WC(e);

	/* stop looping sound */
	if (c->m_iOn > 1)
		STOP_SOUND(e, CHAN_STATIC, RESID(I_SUITCHARGE1));
	c->m_iOn = 0;
	SetThink(e, FN_SUB_DoNothing);
}

void CRecharge_Recharge(int e)
{
	CWallCharger *c = WC(e);

	c->m_iJuice = (int)gSkillData.suitchargerCapacity;
	ent_rnd(e)->frame = 0;
	SetThink(e, FN_SUB_DoNothing);
}

void CRecharge_Use(int e, int activator, int caller, int use_type, float value)
{
	CWallCharger *c = WC(e);
	CBasePlayer *p = Player();

	(void)caller; (void)use_type; (void)value;
	/* if it's not a player, ignore */
	if (activator < 0 || !IsPlayer(activator) || !PlayerMon())
		return;
	/* if there is no juice left, turn it off */
	if (c->m_iJuice <= 0)
	{
		ent_rnd(e)->frame = 1;
		CRecharge_Off(e);
	}
	/* if the activator doesn't have a suit, or there is no juice left,
	 * make the deny noise */
	if (c->m_iJuice <= 0 || !(p->weapons & (1u << WEAPON_SUIT)))
	{
		if (c->m_flSoundTime <= gtime())
		{
			c->m_flSoundTime = gtime() + 0.62f;
			EMIT_SOUND(e, CHAN_ITEM, RESID(I_SUITCHARGENO1), 0.85f, ATTN_NORM);
		}
		return;
	}
	ThinkInLtime(e, 0.25f);
	SetThink(e, FN_CRecharge_Off);
	/* time to recharge yet? */
	if (c->m_flNextCharge >= gtime())
		return;
	c->m_hActivator = EHANDLE(activator);
	/* play the on sound or the looping charging sound */
	if (!c->m_iOn)
	{
		c->m_iOn++;
		EMIT_SOUND(e, CHAN_ITEM, RESID(I_SUITCHARGEOK1), 0.85f, ATTN_NORM);
		c->m_flSoundTime = 0.56f + gtime();
	}
	if (c->m_iOn == 1 && c->m_flSoundTime <= gtime())
	{
		c->m_iOn++;
		EMIT_SOUND(e, CHAN_STATIC, RESID(I_SUITCHARGE1), 0.85f, ATTN_NORM);
	}
	/* charge the player */
	if (p->armorvalue < 100)
	{
		c->m_iJuice--;
		p->armorvalue += 1;
		if (p->armorvalue > 100)
			p->armorvalue = 100;
	}
	/* govern the rate of charge */
	c->m_flNextCharge = gtime() + 0.1f;
}

static int CRecharge_Spawn(int e, gid_t16 gid)
{
	SetUse(e, FN_CRecharge_Use);
	return charger_spawn(e, gid, gSkillData.suitchargerCapacity);
}

const game_class_t gc_FUNC_RECHARGE = {
	.size = sizeof(CWallCharger),
	.Spawn = CRecharge_Spawn,
	.vt = &charger_vt,
};
