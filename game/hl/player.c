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
 * player.c -- CBasePlayer (HLSDK dlls/player.cpp, singleplay_gamerules.cpp
 * single-player rules) on the hl64 player: Spawn, PreThink (WaterMove,
 * UpdateClientData, CheckTimeBasedDamage, CheckSuitUpdate, the death
 * think), PostThink (ItemPostFrame -> ImpulseCommands -> PlayerUse,
 * fall damage, the CLIENT_WEAPONS timer decrement), TakeDamage / Killed /
 * TraceAttack, the suit, the flashlight, the inventory (GiveAmmo,
 * AddPlayerItem, SelectItem, RemoveAllItems, GiveNamedItem) and the
 * barnacle hooks. The engine runs the move (pm_shared) between
 * game_player_prethink and game_player_postthink (pmove-host.md 1); every
 * call runs in the player's home frame.
 *
 * The player block (ent_player_t) is what pm_shared moves; game code
 * elsewhere writes the components (trigger_push velocity / basevelocity,
 * cleared FL_ONGROUND, damage push): those writes are applied to the
 * player block at the next command as deltas against what the last
 * PostThink copied out.
 *
 * Single-player death: HLSDK respawn() reloads the level ("reload"); hl64
 * has no reload yet, the player stays dead (logged).
 */
#include <string.h>

#include "game/hl/player.h"
#include "game/hl/gib.h"
#include "game/hl/savegame.h"

#define ARMOR_RATIO		0.2f	/* armor takes 80% of the damage */
#define ARMOR_BONUS		0.5f	/* each point of armor is worth 1/x points of health */
#define AIRTIME			12
#define FLASH_DRAIN_TIME	1.2f	/* 100 units / 3 minutes */
#define FLASH_CHARGE_TIME	0.2f	/* 100 units / 20 seconds (seconds per unit) */
#define PLAYER_SEARCH_RADIUS	64.0f
#define VIEW_FIELD_NARROW	0.7f
#define SUITUPDATETIME		3.5f
#define SUITFIRSTUPDATETIME	0.1f
#define SUIT_VOLUME		0.25f	/* the suitvolume default */
#define GEIGERDELAY		0.25f

#define PARALYZE_DURATION	2
#define NERVEGAS_DURATION	2
#define POISON_DURATION		5
#define POISON_DAMAGE		2.0f
#define RADIATION_DURATION	2
#define ACID_DURATION		2
#define SLOWBURN_DURATION	2
#define SLOWFREEZE_DURATION	2

/* hl64 weapon selection impulses (the HLSDK client's slot1-5, invnext,
 * invprev and lastinv commands, which hl64 has no console for) */
#define IMPULSE_SLOT1		1	/* .. 5 */
#define IMPULSE_INVNEXT		6
#define IMPULSE_INVPREV		7
#define IMPULSE_LASTINV		8

static CBasePlayer	pl_state;
static float		geiger_range = 1000, geiger_delay;
static int		geiger_range_prev = 1000;

/* CBasePlayer and the geiger counter (the player entity's CBaseMonster
 * block is a private block: the engine saves it) */
uint32_t Player_SaveState(int op)
{
	CBasePlayer *p = &pl_state;
	uint32_t n = 0;

	n += sg_io(op, p, sizeof(*p));
	n += sg_io(op, &geiger_range, sizeof(geiger_range));
	n += sg_io(op, &geiger_delay, sizeof(geiger_delay));
	n += sg_io(op, &geiger_range_prev, sizeof(geiger_range_prev));
	if (op == SG_GET && (p->m_pActiveItem >= WPN_SLOTS || p->m_pLastItem >= WPN_SLOTS ||
		p->m_iSuitPlayNext < 0 || p->m_iSuitPlayNext >= CSUITPLAYLIST))
	{
		sv_fail("game: player weapon or suit index out of range");
		memset(p, 0, sizeof(*p));
	}
	return n;
}

CBasePlayer *Player(void)
{
	return &pl_state;
}

CBaseMonster *PlayerMon(void)
{
	return ent_is_live(ENT_PLAYER) && ent_idc(ENT_PLAYER)->cls == CLS_PLAYER
		? (CBaseMonster *)ent_priv(ENT_PLAYER) : NULL;
}

/* CBasePlayer::IsAlive */
static int CBasePlayer_IsAlive(int e)
{
	CBaseMonster *m = MON(e);

	return m && m->deadflag == DEAD_NO && m->health > 0;
}

int Player_IsAlive(void)
{
	return PlayerMon() && CBasePlayer_IsAlive(ENT_PLAYER);
}

/* gpGlobals->v_forward etc. of pev->v_angle */
static void make_vectors(void)
{
	UTIL_MakeVectors(phy_player()->view_angles);
}

void Player_GunPosition(float out[3])
{
	const ent_player_t *pl = phy_player();

	VecAdd(pl->origin, pl->view_ofs, out);
}

void Player_MakeAimVectors(void)
{
	const ent_player_t *pl = phy_player();
	float a[3];

	VecAdd(pl->view_angles, pl->punchangle, a);
	UTIL_MakeVectors(a);
}

/* ---- suit ---------------------------------------------------------------- */

void Player_SuitSound(res_t sentence)
{
	int pitch = PITCH_NORM;

	if (sentence == RES_NONE)
		return;
	if (RANDOM_LONG(0, 1))
		pitch = RANDOM_LONG(0, 6) + 98;
	EMIT_SENTENCE(ENT_PLAYER, CHAN_STATIC, sentence, SUIT_VOLUME, ATTN_NORM, 0, pitch);
}

void Player_SetSuitUpdate(res_t s, int iNoRepeatTime)
{
	CBasePlayer *p = &pl_state;
	int i, iempty = -1;

	if (!(p->weapons & (1u << WEAPON_SUIT)))
		return;
	if (s == RES_NONE)
	{
		for (i = 0; i < CSUITPLAYLIST; i++)
			p->m_rgSuitPlayList[i] = RES_NONE;
		return;
	}
	for (i = 0; i < CSUITNOREPEAT; i++)
	{
		if (s == p->m_rgiSuitNoRepeat[i])
		{
			/* this sentence or group is already in the norepeat list */
			if (p->m_rgflSuitNoRepeatTime[i] < gtime())
			{
				/* norepeat time has expired, clear it out */
				p->m_rgiSuitNoRepeat[i] = RES_NONE;
				p->m_rgflSuitNoRepeatTime[i] = 0;
				iempty = i;
				break;
			}
			return;	/* don't play, still marked as norepeat */
		}
		if (p->m_rgiSuitNoRepeat[i] == RES_NONE)
			iempty = i;
	}
	if (iNoRepeatTime)
	{
		if (iempty < 0)
			iempty = RANDOM_LONG(0, CSUITNOREPEAT - 1);
		p->m_rgiSuitNoRepeat[iempty] = s;
		p->m_rgflSuitNoRepeatTime[iempty] = iNoRepeatTime + gtime();
	}
	p->m_rgSuitPlayList[p->m_iSuitPlayNext++] = s;
	if (p->m_iSuitPlayNext == CSUITPLAYLIST)
		p->m_iSuitPlayNext = 0;
	if (p->m_flSuitUpdate <= gtime())
	{
		if (p->m_flSuitUpdate == 0)
			p->m_flSuitUpdate = gtime() + SUITFIRSTUPDATETIME;
		else
			p->m_flSuitUpdate = gtime() + SUITUPDATETIME;
	}
}

void game_player_geiger_range(float range)
{
	if (geiger_range >= range)
		geiger_range = range;
}

/* UpdateGeigerCounter: the range goes to the HUD's geiger (hud_geiger) */

static void UpdateGeigerCounter(void)
{
	int range;

	if (gtime() < geiger_delay)
		return;
	geiger_delay = gtime() + GEIGERDELAY;
	range = (int)(geiger_range / 4) & 255;
	if (range != geiger_range_prev)
	{
		geiger_range_prev = range;
		hud_geiger(range << 2);
	}
	if (!RANDOM_LONG(0, 3))
		geiger_range = 1000;
}

static void CheckSuitUpdate(void)
{
	CBasePlayer *p = &pl_state;
	int i, isearch = p->m_iSuitPlayNext;
	res_t s = RES_NONE;

	if (!(p->weapons & (1u << WEAPON_SUIT)))
		return;
	UpdateGeigerCounter();
	if (gtime() >= p->m_flSuitUpdate && p->m_flSuitUpdate > 0)
	{
		for (i = 0; i < CSUITPLAYLIST; i++)
		{
			if ((s = p->m_rgSuitPlayList[isearch]) != RES_NONE)
				break;
			if (++isearch == CSUITPLAYLIST)
				isearch = 0;
		}
		if (s != RES_NONE)
		{
			p->m_rgSuitPlayList[isearch] = RES_NONE;
			Player_SuitSound(s);
			p->m_flSuitUpdate = gtime() + SUITUPDATETIME;
		}
		else
			p->m_flSuitUpdate = 0;
	}
}

/* ---- flashlight -------------------------------------------------------------- */

static int FlashlightIsOn(void)
{
	return (ent_rnd(ENT_PLAYER)->effects & EF_DIMLIGHT) != 0;
}

static void FlashlightTurnOn(void)
{
	CBasePlayer *p = &pl_state;

	/* FAllowFlashlight is TRUE in single player */
	if (p->weapons & (1u << WEAPON_SUIT))
	{
		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, RESID(PL_FLASHLIGHT), 1.0f, ATTN_NORM, 0,
			PITCH_NORM);
		ent_rnd(ENT_PLAYER)->effects |= EF_DIMLIGHT;
		hud_flashlight(1, p->m_iFlashBattery);
		p->m_flFlashLightTime = FLASH_DRAIN_TIME + gtime();
		log_printf("flashlight: on, battery %d, t=%.9g\n", p->m_iFlashBattery,
			(double)gtime());
	}
}

static void FlashlightTurnOff(void)
{
	CBasePlayer *p = &pl_state;

	EMIT_SOUND_DYN(ENT_PLAYER, CHAN_WEAPON, RESID(PL_FLASHLIGHT), 1.0f, ATTN_NORM, 0, PITCH_NORM);
	ent_rnd(ENT_PLAYER)->effects &= ~EF_DIMLIGHT;
	hud_flashlight(0, p->m_iFlashBattery);
	p->m_flFlashLightTime = FLASH_CHARGE_TIME + gtime();
	log_printf("flashlight: off, battery %d, t=%.9g\n", p->m_iFlashBattery, (double)gtime());
}

/* ---- damage --------------------------------------------------------------- */

static void Pain(void)
{
	float r = RANDOM_FLOAT(0, 1);

	if (r <= 0.33f)
		EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN5), 1, ATTN_NORM);
	else if (r <= 0.66f)
		EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN6), 1, ATTN_NORM);
	else
		EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN7), 1, ATTN_NORM);
}

static void DeathSound(void)
{
	switch (RANDOM_LONG(1, 5))
	{
	case 1: EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN5), 1, ATTN_NORM); break;
	case 2: EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN6), 1, ATTN_NORM); break;
	case 3: EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_PAIN7), 1, ATTN_NORM); break;
	}
	Player_SuitSound(RESID(GRP_HEV_DEAD));
}

int Player_TakeHealth(float health, int bits)
{
	return CBaseMonster_TakeHealth(ENT_PLAYER, health, bits);
}

static int CBasePlayer_TakeHealth(int e, float health, int bits)
{
	(void)e;
	return Player_TakeHealth(health, bits);
}

static void CBasePlayer_TraceAttack(int e, int attacker, float flDamage, const float dir[3],
	const col_trace_t *ptr, int bits)
{
	CBaseMonster *m = MON(e);

	if (!m->takedamage)
		return;
	m->m_LastHitGroup = ptr->hitgroup;
	switch (ptr->hitgroup)
	{
	case HITGROUP_GENERIC: break;
	case HITGROUP_HEAD: flDamage *= gSkillData.plrHead; break;
	case HITGROUP_CHEST: flDamage *= gSkillData.plrChest; break;
	case HITGROUP_STOMACH: flDamage *= gSkillData.plrStomach; break;
	case HITGROUP_LEFTARM:
	case HITGROUP_RIGHTARM: flDamage *= gSkillData.plrArm; break;
	case HITGROUP_LEFTLEG:
	case HITGROUP_RIGHTLEG: flDamage *= gSkillData.plrLeg; break;
	default: break;
	}
	SpawnBlood(ptr->end, BloodColor(e), flDamage);	/* a little surface blood */
	TraceBleed(e, flDamage, dir, ptr, bits);
	AddMultiDamage(attacker, e, flDamage, bits);
}

static void CBasePlayer_Killed(int e, int attacker, int iGib);

/* CBaseMonster::TakeDamage, the player's path (combat.cpp): the damage
 * total for the HUD, god mode, the push, health, death. */
static int monster_take_damage(int inflictor, int attacker, float flDamage, int bits)
{
	CBaseMonster *m = PlayerMon();
	CBasePlayer *p = &pl_state;
	ent_player_t *pl = phy_player();
	float flTake = flDamage, dir[3] = { 0, 0, 0 }, c0[3], c1[3];

	if (!m->takedamage)
		return 0;
	if (!CBasePlayer_IsAlive(ENT_PLAYER))
		return CBaseMonster_DeadTakeDamage(ENT_PLAYER, inflictor, attacker, flDamage, bits);
	if (m->deadflag == DEAD_NO)
		Pain();
	m->m_bitsDamageType |= bits;
	if (inflictor >= 0 && ent_is_live(inflictor))
	{
		float d[3];

		Center(inflictor, c0);
		if (ent_frame_delta(inflictor, d))
			VecAdd(c0, d, c0);
		Center(ENT_PLAYER, c1);
		c0[2] -= 10;
		VecSub(c0, c1, dir);
		VecNorm(dir, dir);
		VecCopy(dir, g_vecAttackDir);
	}
	if (inflictor >= 0)
		p->dmg_inflictor = EHANDLE(inflictor);
	p->dmg_take += flTake;
	/* check for godmode or invincibility */
	if (pl->flags & FL_GODMODE)
		return 0;
	/* if this is a player, move him around! */
	if (inflictor >= 0 && pl->movetype == PL_MOVE_WALK &&
		(attacker < 0 || ent_cv(attacker)->solid != SOLID_TRIGGER))
		VecMA(pl->velocity, -Mon_DamageForce(ENT_PLAYER, flDamage), dir, pl->velocity);
	m->health -= flTake;
	if (m->health <= 0)
	{
		if (bits & DMG_ALWAYSGIB)
			CBasePlayer_Killed(ENT_PLAYER, attacker, GIB_ALWAYS);
		else if (bits & DMG_NEVERGIB)
			CBasePlayer_Killed(ENT_PLAYER, attacker, GIB_NEVER);
		else
			CBasePlayer_Killed(ENT_PLAYER, attacker, GIB_NORMAL);
		return 0;
	}
	return 1;
}

static int CBasePlayer_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	CBaseMonster *m = MON(e);
	CBasePlayer *p = &pl_state;
	int bitsDamage = bits, ffound = 1, fmajor, fcritical, fTookDamage, ftrivial, i;
	float flRatio = ARMOR_RATIO, flBonus = ARMOR_BONUS, flHealthPrev = m->health;

	/* already dead */
	if (!CBasePlayer_IsAlive(e))
		return 0;
	/* keep track of amount of damage last sustained */
	m->m_lastDamageAmount = (int)flDamage;
	/* armor (doesn't protect against fall or drown damage) */
	if (p->armorvalue && !(bits & (DMG_FALL | DMG_DROWN)))
	{
		float flNew = flDamage * flRatio, flArmor = (flDamage - flNew) * flBonus;

		if (flArmor > p->armorvalue)
		{
			flArmor = p->armorvalue;
			flArmor *= (1 / flBonus);
			flNew = flDamage - flArmor;
			p->armorvalue = 0;
		}
		else
			p->armorvalue -= flArmor;
		flDamage = flNew;
	}
	/* this cast to INT is critical!!! */
	fTookDamage = monster_take_damage(inflictor, attacker, (float)(int)flDamage, bits);
	/* reset damage time countdown for each type of time based damage */
	for (i = 0; i < CDMG_TIMEBASED; i++)
		if (bits & (DMG_PARALYZE << i))
			m->m_rgbTimeBasedDamage[i] = 0;
	/* how bad is it, doc? */
	ftrivial = (m->health > 75 || m->m_lastDamageAmount < 5);
	fmajor = (m->m_lastDamageAmount > 25);
	fcritical = (m->health < 30);
	m->m_bitsDamageType |= bitsDamage;
	p->m_bitsHUDDamage = -1;	/* make sure the damage bits get resent */
	while (fTookDamage && (!ftrivial || (bitsDamage & DMG_TIMEBASED)) && ffound && bitsDamage)
	{
		ffound = 0;
		if (bitsDamage & DMG_CLUB)
		{
			if (fmajor)
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG4), SUIT_NEXT_IN_30SEC);
			bitsDamage &= ~DMG_CLUB;
			ffound = 1;
		}
		if (bitsDamage & (DMG_FALL | DMG_CRUSH))
		{
			if (fmajor)
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG5), SUIT_NEXT_IN_30SEC);
			else
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG4), SUIT_NEXT_IN_30SEC);
			bitsDamage &= ~(DMG_FALL | DMG_CRUSH);
			ffound = 1;
		}
		if (bitsDamage & DMG_BULLET)
		{
			if (m->m_lastDamageAmount > 5)
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG6), SUIT_NEXT_IN_30SEC);
			bitsDamage &= ~DMG_BULLET;
			ffound = 1;
		}
		if (bitsDamage & DMG_SLASH)
		{
			if (fmajor)
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG1), SUIT_NEXT_IN_30SEC);
			else
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG0), SUIT_NEXT_IN_30SEC);
			bitsDamage &= ~DMG_SLASH;
			ffound = 1;
		}
		if (bitsDamage & DMG_SONIC)
		{
			if (fmajor)
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG2), SUIT_NEXT_IN_1MIN);
			bitsDamage &= ~DMG_SONIC;
			ffound = 1;
		}
		if (bitsDamage & (DMG_POISON | DMG_PARALYZE))
		{
			Player_SetSuitUpdate(RESID(SEN_HEV_DMG3), SUIT_NEXT_IN_1MIN);
			bitsDamage &= ~(DMG_POISON | DMG_PARALYZE);
			ffound = 1;
		}
		if (bitsDamage & DMG_ACID)
		{
			Player_SetSuitUpdate(RESID(SEN_HEV_DET1), SUIT_NEXT_IN_1MIN);
			bitsDamage &= ~DMG_ACID;
			ffound = 1;
		}
		if (bitsDamage & DMG_NERVEGAS)
		{
			Player_SetSuitUpdate(RESID(SEN_HEV_DET0), SUIT_NEXT_IN_1MIN);
			bitsDamage &= ~DMG_NERVEGAS;
			ffound = 1;
		}
		if (bitsDamage & DMG_RADIATION)
		{
			Player_SetSuitUpdate(RESID(SEN_HEV_DET2), SUIT_NEXT_IN_1MIN);
			bitsDamage &= ~DMG_RADIATION;
			ffound = 1;
		}
		if (bitsDamage & DMG_SHOCK)
		{
			bitsDamage &= ~DMG_SHOCK;
			ffound = 1;
		}
	}
	phy_player()->punchangle[0] = -2;
	if (fTookDamage && !ftrivial && fmajor && flHealthPrev >= 75)
	{
		/* first time we take major damage: automedic on, morphine shot */
		Player_SetSuitUpdate(RESID(SEN_HEV_MED1), SUIT_NEXT_IN_30MIN);
		Player_SetSuitUpdate(RESID(SEN_HEV_HEAL7), SUIT_NEXT_IN_30MIN);
	}
	if (fTookDamage && !ftrivial && fcritical && flHealthPrev < 75)
	{
		/* already took major damage, now it's critical */
		if (m->health < 6)
			Player_SetSuitUpdate(RESID(SEN_HEV_HLTH3), SUIT_NEXT_IN_10MIN);
		else if (m->health < 20)
			Player_SetSuitUpdate(RESID(SEN_HEV_HLTH2), SUIT_NEXT_IN_10MIN);
		if (!RANDOM_LONG(0, 3) && flHealthPrev < 50)
			Player_SetSuitUpdate(RESID(SEN_HEV_DMG7), SUIT_NEXT_IN_5MIN);
	}
	/* if we're taking time based damage, warn about its continuing effects */
	if (fTookDamage && (bits & DMG_TIMEBASED) && flHealthPrev < 75)
	{
		if (flHealthPrev < 50)
		{
			if (!RANDOM_LONG(0, 3))
				Player_SetSuitUpdate(RESID(SEN_HEV_DMG7), SUIT_NEXT_IN_5MIN);
		}
		else
			Player_SetSuitUpdate(RESID(SEN_HEV_HLTH1), SUIT_NEXT_IN_10MIN);
	}
	return fTookDamage;
}

static void CBasePlayer_Killed(int e, int attacker, int iGib)
{
	CBaseMonster *m = MON(e);
	CBasePlayer *p = &pl_state;
	ent_player_t *pl = phy_player();

	(void)attacker;
	/* holster weapon immediately, to allow it to cleanup */
	if (p->m_pActiveItem)
		W_Holster(p->m_pActiveItem);
	p->m_flRespawnTimer = 0;
	m->deadflag = DEAD_DYING;
	pl->dead = 1;
	pl->flags &= ~FL_ONGROUND;
	if (pl->velocity[2] < 10)
		pl->velocity[2] += RANDOM_FLOAT(0, 300);
	/* clear out the suit message cache so we don't keep chattering */
	Player_SetSuitUpdate(RES_NONE, 0);
	if ((m->health < -40 && iGib != GIB_NEVER) || iGib == GIB_ALWAYS)
	{
		/* GibMonster: the player's body is not removed */
		CGib_SpawnHeadGib(e);
		CGib_SpawnRandomGibs(e, 4, 1);
		ent_rnd(e)->effects |= EF_NODRAW;
		log_printf("player: killed (gibbed)\n");
		return;
	}
	DeathSound();
	log_printf("player: killed\n");
}

/* ---- the barnacle (barnacle.cpp victims) ------------------------------------- */

int Player_FBecomeProne(int e)
{
	(void)e;
	pl_state.m_afPhysicsFlags |= PFLAG_ONBARNACLE;
	return 1;
}

void Player_BarnacleVictimBitten(int e, int barnacle)
{
	(void)e;
	CBaseMonster *m = PlayerMon();

	if (m)
		TakeDamage(ENT_PLAYER, barnacle, barnacle, m->health + pl_state.armorvalue,
			DMG_SLASH | DMG_ALWAYSGIB);
}

void Player_BarnacleVictimReleased(int e)
{
	(void)e;
	pl_state.m_afPhysicsFlags &= ~PFLAG_ONBARNACLE;
}

static void mvt_BarnacleVictimBitten(int e, int barnacle)
{
	Player_BarnacleVictimBitten(e, barnacle);
}

static void mvt_BarnacleVictimReleased(int e)
{
	Player_BarnacleVictimReleased(e);
}

static void mvt_GetGunPosition(int e, float out[3])
{
	(void)e;
	Player_GunPosition(out);
}

/* ---- class --------------------------------------------------------------------- */

static int CBasePlayer_Classify(int e) { (void)e; return CLASS_PLAYER; }
static int CBasePlayer_BloodColor(int e) { (void)e; return BLOOD_COLOR_RED; }
static int CBasePlayer_ObjectCaps(int e) { (void)e; return 0; }

static void CBasePlayer_EyePosition(int e, float out[3])
{
	(void)e;
	Player_GunPosition(out);
}

static const game_vt_t player_vt = {
	.ObjectCaps = CBasePlayer_ObjectCaps,
	.Classify = CBasePlayer_Classify,
	.BloodColor = CBasePlayer_BloodColor,
	.TraceAttack = CBasePlayer_TraceAttack,
	.TakeDamage = CBasePlayer_TakeDamage,
	.TakeHealth = CBasePlayer_TakeHealth,
	.Killed = CBasePlayer_Killed,
	.IsAlive = CBasePlayer_IsAlive,
	.IsMoving = CBaseMonster_IsMoving,
	.BodyTarget = Mon_BodyTarget,
	.EyePosition = CBasePlayer_EyePosition,
};

static const mon_vt_t player_mvt = {
	.BarnacleVictimBitten = mvt_BarnacleVictimBitten,
	.BarnacleVictimReleased = mvt_BarnacleVictimReleased,
	.GetGunPosition = mvt_GetGunPosition,
};

/* "player" is never spawned from a record: the engine creates slot 1 and
 * calls game_player_spawn (GoldSrc's ClientPutInServer) */
static int CBasePlayer_SpawnRecord(int e, gid_t16 gid)
{
	(void)e; (void)gid;
	return SpawnRefuse("player: the engine creates the client entity (ClientPutInServer), "
		"a map's player entity is not spawned");
}

/* the player never dehydrates; its CBaseMonster level is what
 * game_cut_refs clears */
const game_class_t gc_PLAYER = {
	.size = sizeof(CBaseMonster),
	.Spawn = CBasePlayer_SpawnRecord,
	.vt = &player_vt,
	.ext = &player_mvt,
	.save = &sv_CBaseMonster,
};

/* ---- component sync ------------------------------------------------------------ */

/* After the move: the components follow the player block. */
static void sync_out(const ent_player_t *pl)
{
	CBaseMonster *m = PlayerMon();
	col_ent_t *cv = ent_cv(ENT_PLAYER);
	uint32_t f = pl->flags | FL_CLIENT;

	cv->flags = f;
	cv->movetype = pl->dead ? MOVETYPE_TOSS : pl->movetype;
	VecCopy(pl->velocity, ent_mot(ENT_PLAYER)->velocity);
	ent_phys(ENT_PLAYER)->waterlevel = pl->waterlevel;
	ent_phys(ENT_PLAYER)->watertype = pl->watertype;
	ent_phys(ENT_PLAYER)->groundentity = pl->groundentity < 0 ? ENT_NONE
		: (uint16_t)pl->groundentity;
	if (m)
		VecCopy(pl->view_ofs, m->view_ofs);
	pl_state.sync_flags = f;
	VecCopy(pl->velocity, pl_state.sync_velocity);
}

/* Before the move: game writes to the components since sync_out. */
static void sync_in(ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	ent_mot_t *mot = ent_mot(ENT_PLAYER);
	uint32_t d = (ent_cv(ENT_PLAYER)->flags ^ p->sync_flags) & ~FL_CLIENT;
	int k;

	pl->flags ^= d;
	for (k = 0; k < 3; k++)
	{
		pl->velocity[k] += mot->velocity[k] - p->sync_velocity[k];
		/* trigger_push / conveyors set the base velocity in their touch */
		pl->basevelocity[k] += mot->basevelocity[k];
		mot->basevelocity[k] = 0;
	}
	if (ent_cv(ENT_PLAYER)->flags & FL_BASEVELOCITY)
		pl->flags |= FL_BASEVELOCITY;
}

/* ---- spawn ------------------------------------------------------------------------ */

void game_player_spawn(int e)
{
	CBasePlayer *p = &pl_state;
	ent_player_t *pl = phy_player();
	CBaseMonster *m;
	int i;
	name_t n;

	if (e != ENT_PLAYER || !pl)
		return;
	ent_idc(e)->cls = CLS_PLAYER;
	m = (CBaseMonster *)ent_priv(e);
	if (!m)
		m = (CBaseMonster *)ent_priv_alloc(e, sizeof(CBaseMonster));
	if (!m)
		sys_error("player: no private block");
	memset(m, 0, sizeof(*m));
	memset(p, 0, sizeof(*p));
	m->target = m->netname = m->message = NAME_NONE;
	m->enemy = m->dmg_inflictor = m->m_pGoalEnt = m->m_pLink = EH_NONE;
	m->m_hEnemy = m->m_hTargetEnt = m->m_pCine = m->m_hActivator = EH_NONE;
	for (i = 0; i < MAX_OLD_ENEMIES; i++)
		m->m_hOldEnemy[i] = EH_NONE;
	m->m_makerGid = GID_NONE;
	m->m_iszTriggerTarget = NAME_NONE;
	m->m_sMaster = NAME_NONE;
	p->dmg_inflictor = EH_NONE;
	/* CBasePlayer::Spawn */
	m->health = 100;
	p->armorvalue = 0;
	m->takedamage = DAMAGE_AIM;
	m->max_health = m->health;
	p->air_finished = gtime() + 12;
	p->dmg = 2;	/* initial water damage */
	ent_rnd(e)->effects = 0;
	m->deadflag = DEAD_NO;
	p->m_bitsHUDDamage = -1;
	m->m_bitsDamageType = 0;
	p->m_afPhysicsFlags = 0;
	p->m_fLongJump = 0;	/* no longjump module */
	phy_player_set_physinfo("slj", "0");
	phy_player_set_physinfo("hl", "1");
	m->m_flFieldOfView = 0.5f;
	m->m_bloodColor = BLOOD_COLOR_RED;
	p->m_flNextAttack = 0;	/* UTIL_WeaponTimeBase() */
	p->m_iFlashBattery = 99;
	p->m_flFlashLightTime = 1;	/* force first message */
	pl->fall_velocity = 0;
	pl->dead = 0;
	ent_rnd(e)->model = RESID(MON_PLAYER_MDL);
	ent_cv(e)->studio = COL_NONE;
	VecCopy(pl->view_ofs, m->view_ofs);
	m->m_HackedGunPos[1] = 32;
	p->m_fInitHUD = 1;
	geiger_range = 1000;
	geiger_range_prev = 1000;
	geiger_delay = gtime() + 2;
	sync_out(pl);
	hud_reset();
	/* UpdateClientData's first frame fires game_playerspawn */
	n = rec_name_find(eng_frame_layer(), "game_playerspawn");
	if (n != NAME_NONE)
		FireTargets(n, e, e, USE_TOGGLE, 0);
	log_printf("player: spawn health 100\n");
}

/* ---- PreThink ------------------------------------------------------------------- */

static void WaterMove(ent_player_t *pl)
{
	CBaseMonster *m = PlayerMon();
	CBasePlayer *p = &pl_state;
	int air;

	if (pl->movetype == PL_MOVE_NOCLIP)
		return;
	if (m->health < 0)
		return;
	if (pl->waterlevel != 3)
	{
		/* not underwater: play 'up for air' sound */
		if (p->air_finished < gtime())
			EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_WADE1), 1, ATTN_NORM);
		else if (p->air_finished < gtime() + 9)
			EMIT_SOUND(ENT_PLAYER, CHAN_VOICE, RESID(PL_WADE2), 1, ATTN_NORM);
		p->air_finished = gtime() + AIRTIME;
		p->dmg = 2;
		/* if we took drowning damage, give it back slowly */
		if (p->m_idrowndmg > p->m_idrownrestored)
		{
			m->m_bitsDamageType |= DMG_DROWNRECOVER;
			m->m_bitsDamageType &= ~DMG_DROWN;
			m->m_rgbTimeBasedDamage[itbd_DrownRecover] = 0;
		}
	}
	else
	{
		/* fully under water: stop restoring damage while underwater */
		m->m_bitsDamageType &= ~DMG_DROWNRECOVER;
		m->m_rgbTimeBasedDamage[itbd_DrownRecover] = 0;
		if (p->air_finished < gtime())	/* drown! */
		{
			if (p->pain_finished < gtime())
			{
				p->dmg += 1;
				if (p->dmg > 5)
					p->dmg = 5;
				TakeDamage(ENT_PLAYER, -1, -1, p->dmg, DMG_DROWN);
				p->pain_finished = gtime() + 1;
				p->m_idrowndmg += (int)p->dmg;
			}
		}
		else
			m->m_bitsDamageType &= ~DMG_DROWN;
	}
	if (!pl->waterlevel)
	{
		pl->flags &= ~PL_FL_INWATER;
		return;
	}
	/* make bubbles */
	air = (int)(p->air_finished - gtime());
	if (!RANDOM_LONG(0, 0x1f) && RANDOM_LONG(0, AIRTIME - 1) >= air)
	{
		switch (RANDOM_LONG(0, 3))
		{
		case 0: EMIT_SOUND(ENT_PLAYER, CHAN_BODY, RESID(PL_SWIM1), 0.8f, ATTN_NORM); break;
		case 1: EMIT_SOUND(ENT_PLAYER, CHAN_BODY, RESID(PL_SWIM2), 0.8f, ATTN_NORM); break;
		case 2: EMIT_SOUND(ENT_PLAYER, CHAN_BODY, RESID(PL_SWIM3), 0.8f, ATTN_NORM); break;
		case 3: EMIT_SOUND(ENT_PLAYER, CHAN_BODY, RESID(PL_SWIM4), 0.8f, ATTN_NORM); break;
		}
	}
	if (pl->watertype == CONTENTS_LAVA)
	{
		if (p->dmgtime < gtime())
			TakeDamage(ENT_PLAYER, -1, -1, 10.0f * pl->waterlevel, DMG_BURN);
	}
	else if (pl->watertype == CONTENTS_SLIME)
	{
		p->dmgtime = gtime() + 1;
		TakeDamage(ENT_PLAYER, -1, -1, 4.0f * pl->waterlevel, DMG_ACID);
	}
	if (!(pl->flags & PL_FL_INWATER))
	{
		pl->flags |= PL_FL_INWATER;
		p->dmgtime = 0;
	}
}

/* UpdateClientData: the HUD messages that are state changes (damage,
 * flashlight battery); the HUD reads health, armour and ammo directly. */
static void UpdateClientData(void)
{
	CBaseMonster *m = PlayerMon();
	CBasePlayer *p = &pl_state;

	if (p->m_fInitHUD)
	{
		p->m_fInitHUD = 0;
		hud_flashlight(FlashlightIsOn(), p->m_iFlashBattery);
	}
	if (p->dmg_take || p->dmg_save || p->m_bitsHUDDamage != m->m_bitsDamageType)
	{
		float org[3];
		int other = EHGet(p->dmg_inflictor);

		VecCopy(phy_player()->origin, org);
		if (other >= 0)
		{
			float d[3];

			Center(other, org);
			if (ent_frame_delta(other, d))
				VecAdd(org, d, org);
		}
		hud_damage((int)p->dmg_save, (int)p->dmg_take, m->m_bitsDamageType & DMG_SHOWNHUD,
			org);
		p->dmg_take = 0;
		p->dmg_save = 0;
		p->dmg_inflictor = EH_NONE;
		p->m_bitsHUDDamage = m->m_bitsDamageType;
		/* clear off non-time-based damage indicators */
		m->m_bitsDamageType &= DMG_TIMEBASED;
	}
	/* update flashlight */
	if (p->m_flFlashLightTime && p->m_flFlashLightTime <= gtime())
	{
		if (FlashlightIsOn())
		{
			if (p->m_iFlashBattery)
			{
				p->m_flFlashLightTime = FLASH_DRAIN_TIME + gtime();
				p->m_iFlashBattery--;
				if (!p->m_iFlashBattery)
					FlashlightTurnOff();
			}
		}
		else
		{
			if (p->m_iFlashBattery < 100)
			{
				p->m_flFlashLightTime = FLASH_CHARGE_TIME + gtime();
				p->m_iFlashBattery++;
			}
			else
			{
				p->m_flFlashLightTime = 0;
				log_printf("flashlight: charged, battery %d, t=%.9g\n",
					p->m_iFlashBattery, (double)gtime());
			}
		}
		hud_flashlight(FlashlightIsOn(), p->m_iFlashBattery);
	}
}

static void CheckTimeBasedDamage(void)
{
	CBaseMonster *m = PlayerMon();
	CBasePlayer *p = &pl_state;
	int i, bDuration = 0;

	if (!(m->m_bitsDamageType & DMG_TIMEBASED))
		return;
	/* only check for time based damage approx. every 2 seconds */
	if (fabsf(gtime() - p->m_tbdPrev) < 2.0f)
		return;
	p->m_tbdPrev = gtime();
	for (i = 0; i < CDMG_TIMEBASED; i++)
	{
		/* make sure bit is set for damage type */
		if (!(m->m_bitsDamageType & (DMG_PARALYZE << i)))
			continue;
		switch (i)
		{
		case itbd_Paralyze: bDuration = PARALYZE_DURATION; break;
		case itbd_NerveGas: bDuration = NERVEGAS_DURATION; break;
		case itbd_Poison:
			TakeDamage(ENT_PLAYER, ENT_PLAYER, ENT_PLAYER, POISON_DAMAGE, DMG_GENERIC);
			bDuration = POISON_DURATION;
			break;
		case itbd_Radiation: bDuration = RADIATION_DURATION; break;
		case itbd_DrownRecover:
			/* NOTE: this hack is actually used to RESTORE health after the
			 * player has been drowning and finally takes a breath */
			if (p->m_idrowndmg > p->m_idrownrestored)
			{
				int idif = p->m_idrowndmg - p->m_idrownrestored;

				if (idif > 10)
					idif = 10;
				Player_TakeHealth((float)idif, DMG_GENERIC);
				p->m_idrownrestored += idif;
			}
			bDuration = 4;	/* get up to 5*10 = 50 points back */
			break;
		case itbd_Acid: bDuration = ACID_DURATION; break;
		case itbd_SlowBurn: bDuration = SLOWBURN_DURATION; break;
		case itbd_SlowFreeze: bDuration = SLOWFREEZE_DURATION; break;
		default: bDuration = 0;
		}
		if (m->m_rgbTimeBasedDamage[i])
		{
			/* use up an antitoxin on poison or nervegas after a few seconds of damage */
			if ((i == itbd_NerveGas && m->m_rgbTimeBasedDamage[i] < NERVEGAS_DURATION) ||
				(i == itbd_Poison && m->m_rgbTimeBasedDamage[i] < POISON_DURATION))
			{
				if (p->m_rgItems[ITEM_ANTIDOTE])
				{
					m->m_rgbTimeBasedDamage[i] = 0;
					p->m_rgItems[ITEM_ANTIDOTE]--;
					Player_SetSuitUpdate(RESID(SEN_HEV_HEAL4), 0);
				}
			}
			/* decrement damage duration, detect when done */
			if (!m->m_rgbTimeBasedDamage[i] || --m->m_rgbTimeBasedDamage[i] == 0)
			{
				m->m_rgbTimeBasedDamage[i] = 0;
				m->m_bitsDamageType &= ~(DMG_PARALYZE << i);
			}
		}
		else
			/* first time taking this damage type - init damage duration */
			m->m_rgbTimeBasedDamage[i] = (uint8_t)bDuration;
	}
}

static void PlayerDeathThink(ent_player_t *pl)
{
	CBaseMonster *m = PlayerMon();
	CBasePlayer *p = &pl_state;

	if (pl->flags & FL_ONGROUND)
	{
		float flForward = VecLen(pl->velocity) - 20;

		if (flForward <= 0)
			VecSet(pl->velocity, 0, 0, 0);
		else
		{
			float v[3];

			VecNorm(pl->velocity, v);
			VecScale(v, flForward, pl->velocity);
		}
	}
	if (m->deadflag == DEAD_DYING)
	{
		/* the player model's death animation is not drawn in first person:
		 * the HLSDK 4 s cap stands for it */
		p->m_flRespawnTimer += gframetime();
		if (p->m_flRespawnTimer < 4.0f)
			return;
		m->deadflag = DEAD_DEAD;
	}
	/* wait for all buttons released */
	if (m->deadflag == DEAD_DEAD)
	{
		if (p->button)
			return;
		p->m_fDeadTime = gtime();
		m->deadflag = DEAD_RESPAWNABLE;
		log_printf("player: dead; respawn (reload) is not available\n");
	}
}

void game_player_command(const in_cmd_t *cmd)
{
	CBasePlayer *p = &pl_state;
	ent_player_t *pl = phy_player();

	if (!PlayerMon() || !pl)
		return;
	p->button = cmd->buttons;
	if (cmd->impulse)
		p->impulse = cmd->impulse;
	/* the client's per-command seed (UTIL_SharedRandom*) */
	p->random_seed = (uint32_t)RANDOM_LONG(0, 0x7fffffff);
	sync_in(pl);
}

void game_player_prethink(ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	CBaseMonster *m = PlayerMon();
	int changed;

	if (!m)
	{
		/* no game player (engine tests): pmove-host.md 1 step 2 only */
		if (pl->movetype == PL_MOVE_NOCLIP || pl->dead)
			return;
		if (!pl->waterlevel)
			pl->flags &= ~PL_FL_INWATER;
		else
			pl->flags |= PL_FL_INWATER;
		return;
	}
	ent_rnd(ENT_PLAYER)->effects &= ~EF_MUZZLEFLASH;
	changed = p->m_afButtonLast ^ p->button;
	p->m_afButtonPressed = (uint16_t)(changed & p->button);
	p->m_afButtonReleased = (uint16_t)(changed & ~p->button);
	make_vectors();
	W_ItemPreFrame();
	WaterMove(pl);
	UpdateClientData();
	CheckTimeBasedDamage();
	CheckSuitUpdate();
	if (m->deadflag >= DEAD_DYING)
	{
		PlayerDeathThink(pl);
		return;
	}
	if (p->m_afPhysicsFlags & PFLAG_ONBARNACLE)
		VecSet(pl->velocity, 0, 0, 0);
}

/* ---- PostThink ----------------------------------------------------------------- */

/* UpdatePlayerSound: the client's reserved sound (monsters hear the
 * player's movement and weapons) */
static void UpdatePlayerSound(const ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	CSound *snd = CSoundEnt_SoundPointerForIndex(CSoundEnt_ClientSoundIndex(ENT_PLAYER));
	int iBodyVolume, iVolume;
	float ft = gframetime();

	if (!snd)
		return;
	snd->m_iType = bits_SOUND_NONE;
	/* now calculate the best target volume for the sound */
	if (pl->flags & FL_ONGROUND)
	{
		iBodyVolume = (int)VecLen(pl->velocity);
		if (iBodyVolume > 512)
			iBodyVolume = 512;
	}
	else
		iBodyVolume = 0;
	if (p->button & IN_BTN_JUMP)
		iBodyVolume += 100;
	/* convert player move speed and actions into sound audible by monsters */
	if (p->m_iWeaponVolume > iBodyVolume)
	{
		p->m_iTargetVolume = p->m_iWeaponVolume;
		snd->m_iType |= bits_SOUND_COMBAT;
	}
	else
		p->m_iTargetVolume = (int16_t)iBodyVolume;
	/* decay weapon volume over time so bits_SOUND_COMBAT stays set */
	p->m_iWeaponVolume = (int16_t)(p->m_iWeaponVolume - 250 * ft);
	iVolume = snd->m_iVolume;
	if (p->m_iTargetVolume > iVolume)
		iVolume = p->m_iTargetVolume;
	else if (iVolume > p->m_iTargetVolume)
	{
		iVolume -= (int)(250 * ft);
		if (iVolume < p->m_iTargetVolume)
			iVolume = 0;
	}
	if (gtime() > p->m_flStopExtraSoundTime)
		p->m_iExtraSoundTypes = 0;
	VecCopy(pl->origin, snd->m_vecOrigin);
	snd->m_iType |= bits_SOUND_PLAYER | p->m_iExtraSoundTypes;
	snd->m_iVolume = iVolume;
	/* keep track of virtual muzzle flash */
	p->m_iWeaponFlash = (int16_t)(p->m_iWeaponFlash - 256 * ft);
	if (p->m_iWeaponFlash < 0)
		p->m_iWeaponFlash = 0;
}

static void PlayerUse(const ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	float flMaxDot = VIEW_FIELD_NARROW, eye[3], los[3], half[3], d[3];
	int e = -1, closest = -1, caps;

	if (!((p->button | p->m_afButtonPressed | p->m_afButtonReleased) & IN_BTN_USE))
		return;
	make_vectors();
	VecAdd(pl->origin, pl->view_ofs, eye);
	while ((e = UTIL_FindEntityInSphere(e, pl->origin, PLAYER_SEARCH_RADIUS)) >= 0)
	{
		const col_ent_t *c;
		float flDot;

		if (e == ENT_PLAYER || !(ObjectCaps(e) & (FCAP_IMPULSE_USE | FCAP_CONTINUOUS_USE |
			FCAP_ONOFF_USE)))
			continue;
		c = ent_cv(e);
		VecBModelOrigin(e, los);
		if (ent_frame_delta(e, d))
			VecAdd(los, d, los);
		VecSub(los, eye, los);
		/* the corner nearest the player: the hull in the view cone */
		half[0] = (c->maxs[0] - c->mins[0]) * 0.5f;
		half[1] = (c->maxs[1] - c->mins[1]) * 0.5f;
		half[2] = (c->maxs[2] - c->mins[2]) * 0.5f;
		UTIL_ClampVectorToBox(los, half, los);
		flDot = VecDot(los, gpGlobals.v_forward);
		if (flDot > flMaxDot)
		{
			closest = e;
			flMaxDot = flDot;
		}
	}
	if (closest >= 0)
	{
		caps = ObjectCaps(closest);
		if (p->m_afButtonPressed & IN_BTN_USE)
			EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(PL_WPN_SELECT), 0.4f, ATTN_NORM);
		if (((p->button & IN_BTN_USE) && (caps & FCAP_CONTINUOUS_USE)) ||
			((p->m_afButtonPressed & IN_BTN_USE) && (caps & (FCAP_IMPULSE_USE | FCAP_ONOFF_USE))))
		{
			if (caps & FCAP_CONTINUOUS_USE)
				p->m_afPhysicsFlags |= PFLAG_USING;
			if (p->m_afButtonPressed & IN_BTN_USE)
				log_printf("player: use %d (%s)\n", closest,
					game_class_name(ent_idc(closest)->cls));
			UseEntity(closest, ENT_PLAYER, ENT_PLAYER, USE_SET, 1);
		}
		else if ((p->m_afButtonReleased & IN_BTN_USE) && (caps & FCAP_ONOFF_USE))
			UseEntity(closest, ENT_PLAYER, ENT_PLAYER, USE_SET, 0);
	}
	else if (p->m_afButtonPressed & IN_BTN_USE)
		EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(PL_WPN_DENYSELECT), 0.4f, ATTN_NORM);
}

/* CheatImpulseCommands 101 (test builds; HLSDK needs sv_cheats) */
static void Impulse101(void)
{
#ifdef HL64_TEST
	static const cls_t give[] = {
		CLS_ITEM_SUIT, CLS_ITEM_BATTERY, CLS_WEAPON_CROWBAR, CLS_WEAPON_9MMHANDGUN,
		CLS_AMMO_9MMCLIP, CLS_WEAPON_SHOTGUN, CLS_AMMO_BUCKSHOT, CLS_WEAPON_9MMAR,
		CLS_AMMO_9MMAR, CLS_AMMO_ARGRENADES, CLS_WEAPON_HANDGRENADE, CLS_WEAPON_TRIPMINE,
		CLS_WEAPON_357, CLS_AMMO_357,
	};
	unsigned i;

	for (i = 0; i < sizeof(give) / sizeof(give[0]); i++)
		Player_GiveNamedItem(give[i]);
	log_printf("player: impulse 101\n");
#endif
}

static void ImpulseCommands(const ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	int iImpulse = p->impulse;

	PlayerUse(pl);
	switch (iImpulse)
	{
	case 0:
		break;
	case 100:
		/* temporary flashlight for level designers */
		if (FlashlightIsOn())
			FlashlightTurnOff();
		else
			FlashlightTurnOn();
		break;
	case 101:
		Impulse101();
		break;
	case IMPULSE_INVNEXT:
		Player_SelectNext(1);
		break;
	case IMPULSE_INVPREV:
		Player_SelectNext(-1);
		break;
	case IMPULSE_LASTINV:
		Player_SelectLastItem();
		break;
	default:
		if (iImpulse >= IMPULSE_SLOT1 && iImpulse < IMPULSE_SLOT1 + 5)
			Player_SelectSlot(iImpulse - IMPULSE_SLOT1);
		break;
	}
	p->impulse = 0;
}

void game_player_postthink(ent_player_t *pl)
{
	CBasePlayer *p = &pl_state;
	CBaseMonster *m = PlayerMon();
	float ft = gframetime();
	int i;

	if (!m)
		return;
	if (!CBasePlayer_IsAlive(ENT_PLAYER))
		goto pt_end;
	/* do weapon stuff: ItemPostFrame (impulses and use first, unless the
	 * weapon's attack delay holds them) */
	if (p->m_flNextAttack <= 0)
	{
		ImpulseCommands(pl);
		W_ItemPostFrame();
	}
	/* check to see if player landed hard enough to make a sound */
	if ((pl->flags & FL_ONGROUND) && m->health > 0 &&
		pl->fall_velocity >= PLAYER_FALL_PUNCH_THRESHHOLD)
	{
		if (pl->watertype == CONTENTS_WATER)
			;	/* HLSDK: no wade sound (commented out) */
		else if (pl->fall_velocity > PLAYER_MAX_SAFE_FALL_SPEED)
		{
			/* CHalfLifeRules::FlPlayerFallDamage */
			float flFallDamage;

			pl->fall_velocity -= PLAYER_MAX_SAFE_FALL_SPEED;
			flFallDamage = pl->fall_velocity * DAMAGE_FOR_FALL_SPEED;
			if (flFallDamage > m->health)
				EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(MON_BODYSPLAT), 1, ATTN_NORM);
			if (flFallDamage > 0)
			{
				TakeDamage(ENT_PLAYER, -1, -1, flFallDamage, DMG_FALL);
				pl->punchangle[0] = 0;
			}
		}
	}
	if (pl->flags & FL_ONGROUND)
	{
		if (pl->fall_velocity > 64)
			CSoundEnt_InsertSound(bits_SOUND_PLAYER, pl->origin, (int)pl->fall_velocity, 0.2f);
		pl->fall_velocity = 0;
	}
	/* the weapon entities' thinks (crowbar Smack / SwingAgain) */
	for (i = 1; i < WPN_SLOTS; i++)
	{
		CBasePlayerWeapon *w = &p->wpn[i];

		if (w->m_flThinkTime > 0 && w->m_flThinkTime <= gtime() && (p->weapons & (1u << i)))
		{
			int which = w->m_iThink;
			const weapon_vt_t *vt;

			w->m_flThinkTime = 0;
			w->m_iThink = 0;
			vt = i == WEAPON_CROWBAR ? &wvt_crowbar : NULL;
			if (vt && vt->Think)
				vt->Think(i, which);
		}
	}
	UpdatePlayerSound(pl);
pt_end:
	/* decay timers on weapons (CLIENT_WEAPONS) */
	for (i = 1; i < WPN_SLOTS; i++)
	{
		CBasePlayerWeapon *w = &p->wpn[i];

		if (!(p->weapons & (1u << i)))
			continue;
		w->m_flNextPrimaryAttack = fmaxf(w->m_flNextPrimaryAttack - ft, -1.0f);
		w->m_flNextSecondaryAttack = fmaxf(w->m_flNextSecondaryAttack - ft, -0.001f);
		if (w->m_flTimeWeaponIdle != 1000)
			w->m_flTimeWeaponIdle = fmaxf(w->m_flTimeWeaponIdle - ft, -0.001f);
	}
	p->m_flNextAttack -= ft;
	if (p->m_flNextAttack < -0.001f)
		p->m_flNextAttack = -0.001f;
	/* track button info so we can detect 'pressed' and 'released' buttons */
	p->m_afButtonLast = p->button;
	sync_out(pl);
}

/* ---- inventory ---------------------------------------------------------------------- */

int Player_GiveAmmo(int count, int ammo, int max)
{
	CBasePlayer *p = &pl_state;
	int add;

	if (ammo <= AMMO_NONE || ammo >= MAX_AMMO_TYPES)
		return -1;
	/* CGameRules::CanHaveAmmo */
	if (p->m_rgAmmo[ammo] >= max)
		return -1;
	add = max - p->m_rgAmmo[ammo];
	if (count < add)
		add = count;
	if (add < 1)
		return ammo;
	p->m_rgAmmo[ammo] = (int16_t)(p->m_rgAmmo[ammo] + add);
	hud_pickup(HIST_AMMO, ammo, add);
	return ammo;
}

int Player_HasWeapon(int id)
{
	return id > 0 && id < WPN_SLOTS && (pl_state.weapons & (1u << id));
}

int Player_AddPlayerItem(int id, int defammo, int clip)
{
	CBasePlayer *p = &pl_state;
	const ItemInfo *ii = ItemInfoOf(id);
	CBasePlayerWeapon *w;

	if (!ii || !PlayerMon() || PlayerMon()->deadflag != DEAD_NO)
		return -1;
	/* CGameRules::CanHavePlayerItem */
	if (Player_HasWeapon(id))
	{
		if (!ii->iAmmo1 || p->m_rgAmmo[(int)ii->iAmmo1] >= ii->iMaxAmmo1)
			return -1;
		/* AddDuplicate: ExtractAmmo (a fresh weapon) or ExtractClipAmmo */
		if (defammo)
		{
			if (Player_GiveAmmo(defammo, ii->iAmmo1, ii->iMaxAmmo1) < 0)
				return -1;
		}
		else if (Player_GiveAmmo(clip == WEAPON_NOCLIP ? 0 : clip, ii->iAmmo1,
			ii->iMaxAmmo1) < 0)
			return -1;
		return 0;
	}
	/* AddToPlayer */
	w = &p->wpn[id];
	memset(w, 0, sizeof(*w));
	w->m_iClip = ii->iMaxClip < 1 ? WEAPON_NOCLIP : 0;
	w->m_flReleaseThrow = -1;
	p->weapons |= 1u << id;
	hud_pickup(HIST_WEAP, id, 1);
	/* AddWeapon -> ExtractAmmo -> AddPrimaryAmmo / AddSecondaryAmmo */
	if (ii->iAmmo1)
	{
		int count = defammo, got;

		if (ii->iMaxClip < 1)
		{
			w->m_iClip = WEAPON_NOCLIP;
			got = Player_GiveAmmo(count, ii->iAmmo1, ii->iMaxAmmo1);
		}
		else
		{
			int i = count < ii->iMaxClip ? count : ii->iMaxClip;

			w->m_iClip = (int16_t)i;
			got = Player_GiveAmmo(count - i, ii->iAmmo1, ii->iMaxAmmo1);
		}
		(void)got;
	}
	if (ii->iAmmo2)
		Player_GiveAmmo(0, ii->iAmmo2, ii->iMaxAmmo2);
	/* FShouldSwitchWeapon (single player): unless the active one cannot
	 * be holstered */
	if (!p->m_pActiveItem || W_CanHolster(p->m_pActiveItem))
	{
		/* SwitchWeapon */
		if (W_CanDeploy(id))
		{
			if (p->m_pActiveItem)
				W_Holster(p->m_pActiveItem);
			p->m_pLastItem = p->m_pActiveItem;
			p->m_pActiveItem = (uint8_t)id;
			W_Deploy(id);
		}
	}
	return 1;
}

void Player_GiveNamedItem(cls_t cls)
{
	const ent_player_t *pl = phy_player();
	int e = CBaseEntity_Create(cls, pl->origin, g_vecZero, -1);

	if (e < 0)
	{
		log_printf("player: GiveNamedItem %s failed\n", game_class_name(cls));
		return;
	}
	game_touch(e, ENT_PLAYER);
	/* gEvilImpulse101: whatever was not taken goes away */
	if (ent_is_live(e) && !(ent_cv(e)->flags & FL_KILLME))
		UTIL_Remove(e);
}

void Player_RemoveAllItems(int removeSuit)
{
	CBasePlayer *p = &pl_state;
	int i;

	if (p->m_pActiveItem)
	{
		W_Holster(p->m_pActiveItem);
		p->m_pActiveItem = 0;
	}
	p->m_pLastItem = 0;
	p->viewmodel = RES_NONE;
	for (i = 0; i < WPN_SLOTS; i++)
		memset(&p->wpn[i], 0, sizeof(p->wpn[i]));
	if (removeSuit)
		p->weapons = 0;
	else
		p->weapons &= 1u << WEAPON_SUIT;
	for (i = 0; i < MAX_AMMO_TYPES; i++)
		p->m_rgAmmo[i] = 0;
	log_printf("player: all items removed%s\n", removeSuit ? " (suit too)" : "");
}

static void select_item(int id)
{
	CBasePlayer *p = &pl_state;

	if (!Player_HasWeapon(id) || id == p->m_pActiveItem)
		return;
	if (p->m_pActiveItem)
		W_Holster(p->m_pActiveItem);
	p->m_pLastItem = p->m_pActiveItem;
	p->m_pActiveItem = (uint8_t)id;
	W_Deploy(id);
}

void Player_SelectItem(int id)
{
	CBasePlayer *p = &pl_state;

	/* the client checks CanHolster before sending the select */
	if (p->m_pActiveItem && !W_CanHolster(p->m_pActiveItem))
		return;
	select_item(id);
}

void Player_SelectLastItem(void)
{
	CBasePlayer *p = &pl_state;
	int last = p->m_pLastItem;

	if (!last || !Player_HasWeapon(last))
		return;
	if (p->m_pActiveItem && !W_CanHolster(p->m_pActiveItem))
		return;
	if (p->m_pActiveItem)
		W_Holster(p->m_pActiveItem);
	p->m_pLastItem = p->m_pActiveItem;
	p->m_pActiveItem = (uint8_t)last;
	W_Deploy(last);
}

/* The HUD's weapon order: bucket, then position (ammo.cpp
 * GetFirstPos / GetNextActivePos), usable weapons only. */
static int weapon_order(int id)
{
	const ItemInfo *ii = ItemInfoOf(id);

	return ii ? ii->iSlot * 16 + ii->iPosition : 1 << 20;
}

void Player_SelectSlot(int slot)
{
	CBasePlayer *p = &pl_state;
	int id, best = 0, first = 0, cur = p->m_pActiveItem;
	const ItemInfo *ci = ItemInfoOf(cur);

	/* slotN: the next weapon in bucket N after the active one, wrapping */
	for (id = 1; id < WPN_SLOTS; id++)
	{
		const ItemInfo *ii = ItemInfoOf(id);

		if (!ii || !Player_HasWeapon(id) || ii->iSlot != slot || !W_CanDeploy(id))
			continue;
		if (!first || weapon_order(id) < weapon_order(first))
			first = id;
		if (ci && ci->iSlot == slot && weapon_order(id) > weapon_order(cur) &&
			(!best || weapon_order(id) < weapon_order(best)))
			best = id;
	}
	if (!best)
		best = first;
	if (best)
		Player_SelectItem(best);
}

void Player_SelectNext(int dir)
{
	CBasePlayer *p = &pl_state;
	int id, best = 0, wrap = 0, cur = p->m_pActiveItem, o = cur ? weapon_order(cur) : -1;

	for (id = 1; id < WPN_SLOTS; id++)
	{
		int w;

		if (!ItemInfoOf(id) || !Player_HasWeapon(id) || id == cur || !W_CanDeploy(id))
			continue;
		w = weapon_order(id);
		if (dir > 0)
		{
			if (w > o && (!best || w < weapon_order(best)))
				best = id;
			if (!wrap || w < weapon_order(wrap))
				wrap = id;
		}
		else
		{
			if (w < o && (!best || w > weapon_order(best)))
				best = id;
			if (!wrap || w > weapon_order(wrap))
				wrap = id;
		}
	}
	if (!best)
		best = wrap;
	if (best)
		Player_SelectItem(best);
}

/* ---- game lifetime hooks (dispatch.c weak) ---------------------------------------- */

void game_items_init(void)
{
	SkillDataInit();
	W_Precache();
}

void game_items_new_campaign(void)
{
	memset(&pl_state, 0, sizeof(pl_state));
	SkillDataInit();
}
