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
 * tanks.c -- mounted guns and mortar strikes, ported from HLSDK
 * dlls/func_tank.cpp (CFuncTank, CFuncTankGun as func_tank,
 * CFuncTankRocket, CFuncTankControls), dlls/mortar.cpp (CFuncMortarField,
 * CMortar as monster_mortar) and dlls/rpg.cpp (CRpgRocket as rpg_rocket,
 * the rocket a func_tankrocket fires; hl64 has no RPG weapon, so no
 * launcher and no laser spot ever guides it).
 *
 * Positions are map-local in the tank's frame: the player (its target or
 * its controller) is read through OriginIn / EyePositionIn /
 * BodyTargetIn. HLSDK's gpGlobals->time is gtime(); a tank is a pusher
 * and thinks on its own ltime (ThinkInLtime). func_tanklaser and
 * func_tankmortar are not in the Uplink / Hazard Course maps and are not
 * ported.
 */
#include <math.h>

#include "hl64/fx.h"
#include "engine/phys/player.h"
#include "game/hl/breakable.h"
#include "game/hl/grenade.h"
#include "game/hl/monster.h"
#include "game/hl/player.h"
#include "game/hl/tanks.h"

#define SF_TANK_ACTIVE		0x0001
#define SF_TANK_PLAYER		0x0002
#define SF_TANK_HUMANS		0x0004
#define SF_TANK_ALIENS		0x0008
#define SF_TANK_LINEOFSIGHT	0x0010
#define SF_TANK_CANCONTROL	0x0020
#define SF_TANK_SOUNDON		0x8000

enum { TANK_BULLET_NONE = 0, TANK_BULLET_9MM = 1, TANK_BULLET_MP5 = 2, TANK_BULLET_12MM = 3 };

static const float gTankSpread[][3] = {
	{ 0, 0, 0 },			/* perfect */
	{ 0.025f, 0.025f, 0.025f },	/* small cone */
	{ 0.05f, 0.05f, 0.05f },	/* medium cone */
	{ 0.1f, 0.1f, 0.1f },		/* large cone */
	{ 0.25f, 0.25f, 0.25f },	/* extra-large cone */
};
#define MAX_FIRING_SPREADS	((int)(sizeof(gTankSpread) / sizeof(gTankSpread[0])))

/* skill.cfg sk_plr_rpg (rpg.cpp: pev->dmg = gSkillData.plrDmgRPG) */
static const float sk_plr_rpg[3] = { 100, 100, 100 };

/* ---- func_tank, func_tankrocket (CFuncTank) ------------------------------------ */

typedef struct {
	CBASEENTITY_FIELDS
	ent_h		m_pController;		/* the player, EH_NONE */
	float		m_flNextAttack;
	float		m_vecControllerUsePos[3];
	float		m_yawCenter;		/* "Center" yaw */
	float		m_yawRate;		/* max turn rate to track targets */
	float		m_yawRange;		/* range of turning motion (one-sided) */
	float		m_yawTolerance;		/* tolerance angle */
	float		m_pitchCenter;
	float		m_pitchRate;
	float		m_pitchRange;
	float		m_pitchTolerance;
	float		m_fireLast;		/* last time I fired */
	float		m_fireRate;		/* rounds per second */
	float		m_lastSightTime;	/* last time I saw the target */
	float		m_persist;		/* how long I shoot when I can't see */
	float		m_minRange;		/* range to aim / track */
	float		m_maxRange;
	float		m_barrelPos[3];		/* length of the barrel */
	float		m_spriteScale;		/* scale of the sprites we shoot */
	res_t		m_iszSpriteSmoke;
	res_t		m_iszSpriteFlash;
	int32_t		m_bulletType;		/* TANK_BULLET_* */
	int32_t		m_iBulletDamage;	/* 0: the bullet type's default */
	float		m_sightOrigin[3];	/* last sight of the target */
	int32_t		m_spread;		/* firing spread */
	name_t		m_iszMaster;		/* master entity */
	uint16_t	tank_pad;
} CFuncTank;

#define T(e)	PRIV(CFuncTank, e)

static int IsActive(int e)
{
	return (T(e)->spawnflags & SF_TANK_ACTIVE) != 0;
}

static int CanFire(int e)
{
	return (gtime() - T(e)->m_lastSightTime) < T(e)->m_persist;
}

static void BarrelPosition(int e, float out[3])
{
	const CFuncTank *p = T(e);
	float f[3], r[3], u[3];

	AngleVectors(ent_cv(e)->angles, f, r, u);
	VecMA(ent_cv(e)->origin, p->m_barrelPos[0], f, out);
	VecMA(out, p->m_barrelPos[1], r, out);
	VecMA(out, p->m_barrelPos[2], u, out);
}

static void StartRotSound(int e)
{
	CFuncTank *p = T(e);

	if (p->noise == RES_NONE || (p->spawnflags & SF_TANK_SOUNDON))
		return;
	p->spawnflags |= SF_TANK_SOUNDON;
	EMIT_SOUND(e, CHAN_STATIC, p->noise, 0.85f, ATTN_NORM);
}

static void StopRotSound(int e)
{
	CFuncTank *p = T(e);

	if (p->spawnflags & SF_TANK_SOUNDON)
		STOP_SOUND(e, CHAN_STATIC, p->noise);
	p->spawnflags &= ~SF_TANK_SOUNDON;
}

static void TankActivate(int e)
{
	CFuncTank *p = T(e);

	p->spawnflags |= SF_TANK_ACTIVE;
	ThinkInLtime(e, 0.1f);
	p->m_fireLast = 0;
}

static void TankDeactivate(int e)
{
	CFuncTank *p = T(e);

	p->spawnflags &= ~SF_TANK_ACTIVE;
	p->m_fireLast = 0;
	StopRotSound(e);
}

static int InRange(int e, float range)
{
	const CFuncTank *p = T(e);

	if (range < p->m_minRange)
		return 0;
	if (p->m_maxRange > 0 && range > p->m_maxRange)
		return 0;
	return 1;
}

/* HLSDK computes (gpGlobals->time - m_fireLast) * m_fireRate in the x87's
 * extended precision; double keeps the truncation to whole rounds off f32
 * rounding */
static int BulletCount(const CFuncTank *p)
{
	return (int)(((double)gtime() - (double)p->m_fireLast) * (double)p->m_fireRate);
}

/* CFuncTank::Fire: fire targets and spawn sprites */
static void CFuncTank_FireBase(int e, const float barrelEnd[3])
{
	CFuncTank *p = T(e);
	ent_rnd_t *r = ent_rnd(e);
	int s;

	if (p->m_fireLast != 0)
	{
		if (p->m_iszSpriteSmoke != RES_NONE &&
			(s = CSprite_SpriteCreate(p->m_iszSpriteSmoke, barrelEnd, 1)) >= 0)
		{
			float v[3];

			CSprite_AnimateAndDie(s, RANDOM_FLOAT(15.0f, 20.0f));
			CSprite_SetTransparency(s, kRenderTransAlpha, r->rendercolor[0],
				r->rendercolor[1], r->rendercolor[2], 255, kRenderFxNone);
			VecSet(v, 0, 0, RANDOM_FLOAT(40, 80));
			ent_set_velocity(s, v);
			ent_rnd(s)->scale = p->m_spriteScale;
		}
		if (p->m_iszSpriteFlash != RES_NONE &&
			(s = CSprite_SpriteCreate(p->m_iszSpriteFlash, barrelEnd, 1)) >= 0)
		{
			CSprite_AnimateAndDie(s, 60);
			CSprite_SetTransparency(s, kRenderTransAdd, 255, 255, 255, 255,
				kRenderFxNoDissipation);
			ent_rnd(s)->scale = p->m_spriteScale;
			/* Hack Hack, make it stick around for at least 100 ms. */
			ent_think_at(s, ent_tm(s)->nextthink + 0.1f);
		}
		CBaseEntity_SUB_UseTargets(e, e, USE_TOGGLE, 0);
	}
	p->m_fireLast = gtime();
}

/* CFuncTankGun::Fire / CFuncTankRocket::Fire */
static void CFuncTank_Fire(int e, const float barrelEnd[3], const float forward[3], int attacker)
{
	CFuncTank *p = T(e);
	int i, n;

	if (p->m_fireLast == 0)
	{
		CFuncTank_FireBase(e, barrelEnd);
		return;
	}
	if (ent_idc(e)->cls == CLS_FUNC_TANKROCKET)
	{
		n = BulletCount(p);
		if (n > 0)
		{
			for (i = 0; i < n; i++)
				CBaseEntity_Create(CLS_RPG_ROCKET, barrelEnd, ent_cv(e)->angles, e);
			CFuncTank_FireBase(e, barrelEnd);
		}
		return;
	}
	/* FireBullets needs gpGlobals->v_up, etc. */
	UTIL_MakeAimVectors(ent_cv(e)->angles);
	n = BulletCount(p);
	if (n <= 0)
		return;
	for (i = 0; i < n; i++)
	{
		int type;

		switch (p->m_bulletType)
		{
		case TANK_BULLET_9MM: type = BULLET_MONSTER_9MM; break;
		case TANK_BULLET_MP5: type = BULLET_MONSTER_MP5; break;
		case TANK_BULLET_12MM: type = BULLET_MONSTER_12MM; break;
		default: type = BULLET_NONE; break;
		}
		if (type != BULLET_NONE)
			FireBullets(e, 1, barrelEnd, forward, gTankSpread[p->m_spread], 4096, type, 1,
				p->m_iBulletDamage, attacker);
	}
	CFuncTank_FireBase(e, barrelEnd);
}

/* ---- tank controlling ---- */

int CFuncTank_OnControls(int e)
{
	const CFuncTank *p = T(e);
	float o[3], d[3];

	if (!(p->spawnflags & SF_TANK_CANCONTROL))
		return 0;
	OriginIn(ENT_PLAYER, e, o);
	VecSub(p->m_vecControllerUsePos, o, d);
	return VecLen(d) < 30;
}

static int StartControl(int e)
{
	CFuncTank *p = T(e);
	CBasePlayer *pl = Player();

	if (EHGet(p->m_pController) >= 0)
		return 0;
	/* Team only or disabled? */
	if (p->m_iszMaster != NAME_NONE && !UTIL_IsMasterTriggered(p->m_iszMaster, ENT_PLAYER))
		return 0;
	log_printf("tank: %d using TANK!\n", e);
	p->m_pController = EHANDLE(ENT_PLAYER);
	if (pl->m_pActiveItem)
		W_Holster(pl->m_pActiveItem);	/* viewmodel and weaponmodel 0 */
	pl->m_iHideHUD |= HIDEHUD_WEAPONS;
	OriginIn(ENT_PLAYER, e, p->m_vecControllerUsePos);
	ThinkInLtime(e, 0.1f);
	return 1;
}

static void StopControl(int e)
{
	CFuncTank *p = T(e);
	CBasePlayer *pl = Player();

	if (EHGet(p->m_pController) < 0)
		return;
	if (pl->m_pActiveItem)
		W_Deploy(pl->m_pActiveItem);
	log_printf("tank: %d stopped using TANK\n", e);
	pl->m_iHideHUD &= (uint8_t)~HIDEHUD_WEAPONS;
	ent_think_at(e, 0);
	p->m_pController = EH_NONE;
	if (IsActive(e))
		ThinkInLtime(e, 1.0f);
}

/* Called each frame by the player's PostThink (Use USE_SET 2) */
static void ControllerPostFrame(int e)
{
	CFuncTank *p = T(e);
	float forward[3], barrel[3];

	if (gtime() < p->m_flNextAttack)
		return;
	if (Player()->button & IN_BTN_ATTACK)
	{
		AngleVectors(ent_cv(e)->angles, forward, NULL, NULL);
		/* to make sure the gun doesn't fire too many bullets */
		p->m_fireLast = gtime() - (1 / p->m_fireRate) - 0.01f;
		BarrelPosition(e, barrel);
		CFuncTank_Fire(e, barrel, forward, ENT_PLAYER);
		/* HACKHACK -- make some noise (that the AI can hear) */
		Player()->m_iWeaponVolume = LOUD_GUN_VOLUME;
		p->m_flNextAttack = gtime() + (1 / p->m_fireRate);
	}
}

void CFuncTank_Use(int e, int activator, int caller, int use_type, float value)
{
	CFuncTank *p = T(e);

	(void)caller;
	if (p->spawnflags & SF_TANK_CANCONTROL)
	{
		/* player controlled turret */
		if (activator < 0 || Classify(activator) != CLASS_PLAYER)
			return;
		if (value == 2 && use_type == USE_SET)
			ControllerPostFrame(e);
		else if (EHGet(p->m_pController) < 0 && use_type != USE_OFF)
		{
			Player()->m_pTank = EHANDLE(e);
			StartControl(e);
		}
		else
			StopControl(e);
	}
	else
	{
		if (!ShouldToggle(use_type, IsActive(e)))
			return;
		if (IsActive(e))
			TankDeactivate(e);
		else
			TankActivate(e);
	}
}

/* If barrel is offset, add in additional rotation */
static void AdjustAnglesForBarrel(int e, float angles[3], float distance)
{
	const CFuncTank *p = T(e);
	float r2, d2;

	if (p->m_barrelPos[1] != 0 || p->m_barrelPos[2] != 0)
	{
		distance -= p->m_barrelPos[2];
		d2 = distance * distance;
		if (p->m_barrelPos[1] != 0)
		{
			r2 = p->m_barrelPos[1] * p->m_barrelPos[1];
			angles[1] += (float)((180.0 / M_PI) * atan2(p->m_barrelPos[1], sqrt(d2 - r2)));
		}
		if (p->m_barrelPos[2] != 0)
		{
			r2 = p->m_barrelPos[2] * p->m_barrelPos[2];
			angles[0] += (float)((180.0 / M_PI) * atan2(-p->m_barrelPos[2], sqrt(d2 - r2)));
		}
	}
}

static void TrackTarget(int e)
{
	CFuncTank *p = T(e);
	col_trace_t tr;
	const float *cur = ent_cv(e)->angles;
	int controller = EHGet(p->m_pController), updateTime = 0, target = -1;
	float angles[3], direction[3] = { 0, 0, 0 }, targetPosition[3], barrelEnd[3] = { 0, 0, 0 };
	float av[3], distX, distY, range;

	/* Get a position to aim for */
	if (controller >= 0)
	{
		/* Tanks attempt to mirror the player's angles */
		VecCopy(phy_player()->view_angles, angles);
		angles[0] = 0 - angles[0];
		ThinkInLtime(e, 0.05f);
	}
	else
	{
		if (IsActive(e))
			ThinkInLtime(e, 0.1f);
		else
			return;
		/* FIND_CLIENT_IN_PVS */
		if (!ent_client_in_pvs(e))
		{
			if (IsActive(e))
				ThinkInLtime(e, 2);	/* Wait 2 secs */
			return;
		}
		target = ENT_PLAYER;	/* FindTarget */
		/* Calculate angle needed to aim at target */
		BarrelPosition(e, barrelEnd);
		EyePositionIn(target, e, targetPosition);
		VecSub(targetPosition, barrelEnd, direction);
		range = VecLen(direction);
		if (!InRange(e, range))
			return;
		UTIL_TraceLine(barrelEnd, targetPosition, dont_ignore_monsters, e, &tr);
		/* No line of sight, don't track */
		if (tr.frac == 1.0f || tr.ent == target)
		{
			if (InRange(e, range) && IsAlive(target))
			{
				updateTime = 1;
				/* UpdateTargetPosition: pTarget->BodyTarget(pev->origin) */
				BodyTargetIn(target, e, ent_cv(e)->origin, p->m_sightOrigin);
			}
		}
		/* Track sight origin */
		VecSub(p->m_sightOrigin, ent_cv(e)->origin, direction);
		UTIL_VecToAngles(direction, angles);
		/* Calculate the additional rotation to point the end of the
		 * barrel at the target (not the gun's center) */
		AdjustAnglesForBarrel(e, angles, VecLen(direction));
	}
	angles[0] = -angles[0];
	/* Force the angles to be relative to the center position */
	angles[1] = p->m_yawCenter + UTIL_AngleDistance(angles[1], p->m_yawCenter);
	angles[0] = p->m_pitchCenter + UTIL_AngleDistance(angles[0], p->m_pitchCenter);
	/* Limit against range in y (don't update if you saw the player, but
	 * out of range) */
	if (angles[1] > p->m_yawCenter + p->m_yawRange)
	{
		angles[1] = p->m_yawCenter + p->m_yawRange;
		updateTime = 0;
	}
	else if (angles[1] < (p->m_yawCenter - p->m_yawRange))
	{
		angles[1] = (p->m_yawCenter - p->m_yawRange);
		updateTime = 0;
	}
	if (updateTime)
		p->m_lastSightTime = gtime();
	VecCopy(ent_mot(e)->avelocity, av);
	/* Move toward target at rate or less */
	distY = UTIL_AngleDistance(angles[1], cur[1]);
	av[1] = distY * 10;
	if (av[1] > p->m_yawRate)
		av[1] = p->m_yawRate;
	else if (av[1] < -p->m_yawRate)
		av[1] = -p->m_yawRate;
	/* Limit against range in x */
	if (angles[0] > p->m_pitchCenter + p->m_pitchRange)
		angles[0] = p->m_pitchCenter + p->m_pitchRange;
	else if (angles[0] < p->m_pitchCenter - p->m_pitchRange)
		angles[0] = p->m_pitchCenter - p->m_pitchRange;
	/* Move toward target at rate or less */
	distX = UTIL_AngleDistance(angles[0], cur[0]);
	av[0] = distX * 10;
	if (av[0] > p->m_pitchRate)
		av[0] = p->m_pitchRate;
	else if (av[0] < -p->m_pitchRate)
		av[0] = -p->m_pitchRate;
	ent_set_avelocity(e, av);
	if (controller >= 0)
		return;
	if (CanFire(e) && ((fabsf(distX) < p->m_pitchTolerance && fabsf(distY) < p->m_yawTolerance) ||
		(p->spawnflags & SF_TANK_LINEOFSIGHT)))
	{
		int fire = 0;
		float forward[3];

		AngleVectors(cur, forward, NULL, NULL);
		if (p->spawnflags & SF_TANK_LINEOFSIGHT)
		{
			float end[3];

			VecMA(barrelEnd, VecLen(direction), forward, end);
			UTIL_TraceLine(barrelEnd, end, dont_ignore_monsters, e, &tr);
			if (tr.ent == target)
				fire = 1;
		}
		else
			fire = 1;
		if (fire)
			CFuncTank_Fire(e, barrelEnd, forward, e);
		else
			p->m_fireLast = 0;
	}
	else
		p->m_fireLast = 0;
}

void CFuncTank_Think(int e)
{
	const float *av;

	ent_set_avelocity(e, g_vecZero);
	TrackTarget(e);
	av = ent_mot(e)->avelocity;
	if (fabsf(av[0]) > 1 || fabsf(av[1]) > 1)
		StartRotSound(e);
	else
		StopRotSound(e);
}

static int CFuncTank_Spawn(int e, gid_t16 gid)
{
	CFuncTank *p = T(e);

	/* KeyValue */
	p->m_pController = EH_NONE;
	p->m_iszMaster = NAME_NONE;
	if (gid != GID_NONE)
	{
		p->m_yawRate = KeyF(gid, K_YAWRATE, 0);
		p->m_yawRange = KeyF(gid, K_YAWRANGE, 0);
		p->m_yawTolerance = KeyF(gid, K_YAWTOLERANCE, 0);
		p->m_pitchRange = KeyF(gid, K_PITCHRANGE, 0);
		p->m_pitchRate = KeyF(gid, K_PITCHRATE, 0);
		p->m_pitchTolerance = KeyF(gid, K_PITCHTOLERANCE, 0);
		p->m_fireRate = KeyF(gid, K_FIRERATE, 0);
		p->m_barrelPos[0] = KeyF(gid, K_BARREL, 0);
		p->m_barrelPos[1] = KeyF(gid, K_BARRELY, 0);
		p->m_barrelPos[2] = KeyF(gid, K_BARRELZ, 0);
		p->m_spriteScale = KeyF(gid, K_SPRITESCALE, 0);
		p->m_iszSpriteSmoke = rec_key_res(gid, K_SPRITESMOKE);
		p->m_iszSpriteFlash = rec_key_res(gid, K_SPRITEFLASH);
		p->noise = rec_key_res(gid, K_ROTATESOUND);
		p->m_persist = KeyF(gid, K_PERSISTENCE, 0);
		p->m_bulletType = (int32_t)KeyF(gid, K_BULLET, 0);
		p->m_iBulletDamage = (int32_t)KeyF(gid, K_BULLET_DAMAGE, 0);
		p->m_spread = (int32_t)KeyF(gid, K_FIRESPREAD, 0);
		p->m_minRange = KeyF(gid, K_TANK_MINRANGE, 0);
		p->m_maxRange = KeyF(gid, K_TANK_MAXRANGE, 0);
		p->m_iszMaster = rec_key_name(gid, K_MASTER);
	}
	/* Precache */
	PRECACHE(p->m_iszSpriteSmoke);
	PRECACHE(p->m_iszSpriteFlash);
	PRECACHE(p->noise);
	if (ent_idc(e)->cls == CLS_FUNC_TANKROCKET)
	{
		/* UTIL_PrecacheOther("rpg_rocket") */
		PRECACHE(RESID(RPGROCKET_MDL));
		PRECACHE(RESID(SPR_SMOKE_TRAIL));
		PRECACHE(RESID(W_ROCKET1));
	}
	ent_set_movetype(e, MOVETYPE_PUSH);	/* so it doesn't get pushed by anything */
	ent_set_solid(e, SOLID_BSP);
	ent_set_origin(e, ent_cv(e)->origin);
	ent_set_model(e, p->model);
	p->m_yawCenter = ent_cv(e)->angles[1];
	p->m_pitchCenter = ent_cv(e)->angles[0];
	SetThink(e, FN_CFuncTank_Think);
	SetUse(e, FN_CFuncTank_Use);
	if (IsActive(e))
		ThinkInLtime(e, 1.0f);
	BarrelPosition(e, p->m_sightOrigin);	/* point at the end of the barrel */
	if (p->m_fireRate <= 0)
		p->m_fireRate = 1;
	/* HLSDK: m_spread > MAX_FIRING_SPREADS (lets 5 index past the table) */
	if (p->m_spread < 0 || p->m_spread >= MAX_FIRING_SPREADS)
		p->m_spread = 0;
	return 0;
}

/* Bmodels don't go across transitions */
static int NoTransition_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static const game_vt_t CFuncTank_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

/* a tank someone is driving stays */
static int CFuncTank_quiet(int e)
{
	return EHGet(T(e)->m_pController) < 0;
}

/* HLSDK CFuncTank::m_SaveData */
static const sv_field_t f_CFuncTank[] = {
	SVF(CFuncTank, m_yawCenter, SV_F32, SVK),
	SVF(CFuncTank, m_yawRate, SV_F32, SVK),
	SVF(CFuncTank, m_yawRange, SV_F32, SVK),
	SVF(CFuncTank, m_yawTolerance, SV_F32, SVK),
	SVF(CFuncTank, m_pitchCenter, SV_F32, SVK),
	SVF(CFuncTank, m_pitchRate, SV_F32, SVK),
	SVF(CFuncTank, m_pitchRange, SV_F32, SVK),
	SVF(CFuncTank, m_pitchTolerance, SV_F32, SVK),
	SVF(CFuncTank, m_fireLast, SV_TIME, 0),
	SVF(CFuncTank, m_fireRate, SV_F32, SVK),
	SVF(CFuncTank, m_lastSightTime, SV_TIME, 0),
	SVF(CFuncTank, m_persist, SV_F32, SVK),
	SVF(CFuncTank, m_minRange, SV_F32, SVK),
	SVF(CFuncTank, m_maxRange, SV_F32, SVK),
	SVF(CFuncTank, m_barrelPos, SV_VEC, SVK),
	SVF(CFuncTank, m_spriteScale, SV_F32, SVK),
	SVF(CFuncTank, m_iszSpriteSmoke, SV_RES, SVK),
	SVF(CFuncTank, m_iszSpriteFlash, SV_RES, SVK),
	SVF(CFuncTank, m_bulletType, SV_I32, SVK),
	SVF(CFuncTank, m_sightOrigin, SV_POS, 0),
	SVF(CFuncTank, m_spread, SV_I32, SVK),
	SVF(CFuncTank, m_pController, SV_EH, 0),
	SVF(CFuncTank, m_vecControllerUsePos, SV_POS, 0),
	SVF(CFuncTank, m_flNextAttack, SV_TIME, 0),
	SVF(CFuncTank, m_iBulletDamage, SV_I32, SVK),
	SVF(CFuncTank, m_iszMaster, SV_NAME, SVK),
};
static const sv_table_t sv_CFuncTank = SV_TABLE(&sv_CBaseEntity, f_CFuncTank,
	.quiet = CFuncTank_quiet);

const game_class_t gc_FUNC_TANK = {
	.size = sizeof(CFuncTank), .Spawn = CFuncTank_Spawn, .vt = &CFuncTank_vt,
	.save = &sv_CFuncTank,
};

const game_class_t gc_FUNC_TANKROCKET = {
	.size = sizeof(CFuncTank), .Spawn = CFuncTank_Spawn, .vt = &CFuncTank_vt,
	.save = &sv_CFuncTank,
};

/* ---- func_tankcontrols (CFuncTankControls) ------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	ent_h		m_pTank;
} CFuncTankControls;

/* The first func_tank* named by target (waking its record), -1 none. */
static int find_tank(int e)
{
	name_t n = CBase(e)->target;
	int t, it = 0;

	while ((t = UTIL_FindEntityByTargetname(n, &it)) >= 0)
	{
		cls_t c = ent_idc(t)->cls;

		if (c == CLS_FUNC_TANK || c == CLS_FUNC_TANKROCKET)
			return t;
	}
	return -1;
}

void CFuncTankControls_Think(int e)
{
	int t = find_tank(e);

	if (t < 0)
	{
		log_printf("tank: controls %d: no tank\n", e);
		return;
	}
	PRIV(CFuncTankControls, e)->m_pTank = EHANDLE(t);
}

/* pass the Use command onto the controls */
void CFuncTankControls_Use(int e, int activator, int caller, int use_type, float value)
{
	CFuncTankControls *p = PRIV(CFuncTankControls, e);
	int t = EHGet(p->m_pTank);

	/* a tank released since (record streaming) is found again by name */
	if (t < 0 && p->m_pTank != EH_NONE)
	{
		t = find_tank(e);
		p->m_pTank = EHANDLE(t);
	}
	if (t >= 0)
		UseEntity(t, activator, caller, use_type, value);
}

static int CFuncTankControls_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_solid(e, SOLID_TRIGGER);
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects |= EF_NODRAW;
	ent_set_model(e, CBase(e)->model);
	PRIV(CFuncTankControls, e)->m_pTank = EH_NONE;
	SetThink(e, FN_CFuncTankControls_Think);
	SetUse(e, FN_CFuncTankControls_Use);
	/* After all the func_tank's have spawned */
	ThinkIn(e, 0.3f);
	return 0;
}

static int CFuncTankControls_ObjectCaps(int e)
{
	(void)e;
	return FCAP_IMPULSE_USE;
}

static const game_vt_t CFuncTankControls_vt = {
	.ObjectCaps = CFuncTankControls_ObjectCaps,
};

/* HLSDK CFuncTankControls::m_SaveData */
static const sv_field_t f_CFuncTankControls[] = {
	SVF(CFuncTankControls, m_pTank, SV_EH, 0),
};
static const sv_table_t sv_CFuncTankControls = SV_TABLE(&sv_CBaseEntity, f_CFuncTankControls);

const game_class_t gc_FUNC_TANKCONTROLS = {
	.size = sizeof(CFuncTankControls), .Spawn = CFuncTankControls_Spawn,
	.vt = &CFuncTankControls_vt, .save = &sv_CFuncTankControls,
};

/* ---- func_mortar_field (CFuncMortarField) -------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	name_t		m_iszXController;
	name_t		m_iszYController;
	float		m_flSpread;
	int32_t		m_iCount;
	int32_t		m_fControl;
} CFuncMortarField;

/* If connected to a table, then use the table controllers, else hit where
 * the trigger is. */
void CFuncMortarField_FieldUse(int e, int activator, int caller, int use_type, float value)
{
	CFuncMortarField *p = PRIV(CFuncMortarField, e);
	const col_ent_t *cv = ent_cv(e);
	col_trace_t tr;
	float vecStart[3], t = 2.5f;
	int i, pitch, c;

	(void)caller;
	(void)use_type;
	(void)value;
	vecStart[0] = RANDOM_FLOAT(cv->mins[0], cv->maxs[0]);
	vecStart[1] = RANDOM_FLOAT(cv->mins[1], cv->maxs[1]);
	vecStart[2] = cv->maxs[2];
	switch (p->m_fControl)
	{
	case 0:	/* random */
		break;
	case 1:	/* Trigger Activator */
		if (activator >= 0)
		{
			float o[3];

			OriginIn(activator, e, o);
			vecStart[0] = o[0];
			vecStart[1] = o[1];
		}
		break;
	case 2:	/* table */
		if (p->m_iszXController != NAME_NONE &&
			(c = UTIL_FindByTargetname1(p->m_iszXController)) >= 0)
			vecStart[0] = cv->mins[0] + ent_phys(c)->ideal_yaw * (cv->maxs[0] - cv->mins[0]);
		if (p->m_iszYController != NAME_NONE &&
			(c = UTIL_FindByTargetname1(p->m_iszYController)) >= 0)
			vecStart[1] = cv->mins[1] + ent_phys(c)->ideal_yaw * (cv->maxs[1] - cv->mins[1]);
		break;
	}
	pitch = RANDOM_LONG(95, 124);
	EMIT_SOUND_DYN(e, CHAN_VOICE, RESID(W_MORTAR), 1.0f, ATTN_NONE, 0, pitch);
	for (i = 0; i < p->m_iCount; i++)
	{
		float spot[3], end[3];
		int m;

		VecCopy(vecStart, spot);
		spot[0] += RANDOM_FLOAT(-p->m_flSpread, p->m_flSpread);
		spot[1] += RANDOM_FLOAT(-p->m_flSpread, p->m_flSpread);
		VecCopy(spot, end);
		end[2] -= 4096;
		UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
		m = CBaseEntity_Create(CLS_MONSTER_MORTAR, tr.end, g_vecZero, activator);
		if (m >= 0)
			ent_think_at(m, gtime() + t);
		t += RANDOM_FLOAT(0.2f, 0.5f);
		if (i == 0)
			CSoundEnt_InsertSound(bits_SOUND_DANGER, tr.end, 400, 0.3f);
	}
	log_printf("mortar: field %d strikes %d at %.9g %.9g\n", e, (int)p->m_iCount,
		(double)vecStart[0], (double)vecStart[1]);
}

/* Drop bombs from above */
static int CFuncMortarField_Spawn(int e, gid_t16 gid)
{
	CFuncMortarField *p = PRIV(CFuncMortarField, e);

	p->m_iszXController = NAME_NONE;
	p->m_iszYController = NAME_NONE;
	if (gid != GID_NONE)
	{
		p->m_iszXController = rec_key_name(gid, K_M_ISZXCONTROLLER);
		p->m_iszYController = rec_key_name(gid, K_M_ISZYCONTROLLER);
		p->m_flSpread = KeyF(gid, K_M_FLSPREAD, 0);
		p->m_fControl = (int32_t)KeyF(gid, K_M_FCONTROL, 0);
		p->m_iCount = (int32_t)KeyF(gid, K_M_ICOUNT, 0);
	}
	ent_set_solid(e, SOLID_NOT);
	ent_set_model(e, p->model);	/* set size and link into world */
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_rnd(e)->effects |= EF_NODRAW;
	SetUse(e, FN_CFuncMortarField_FieldUse);
	/* Precache */
	PRECACHE(RESID(W_MORTAR));
	PRECACHE(RESID(SLV_LGTNING_SPR));
	return 0;
}

static const game_vt_t CFuncMortarField_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

/* HLSDK CFuncMortarField::m_SaveData */
static const sv_field_t f_CFuncMortarField[] = {
	SVF(CFuncMortarField, m_iszXController, SV_NAME, SVK),
	SVF(CFuncMortarField, m_iszYController, SV_NAME, SVK),
	SVF(CFuncMortarField, m_flSpread, SV_F32, SVK),
	SVF(CFuncMortarField, m_iCount, SV_I32, SVK),
	SVF(CFuncMortarField, m_fControl, SV_I32, SVK),
};
static const sv_table_t sv_CFuncMortarField = SV_TABLE(&sv_CBaseEntity, f_CFuncMortarField);

const game_class_t gc_FUNC_MORTAR_FIELD = {
	.size = sizeof(CFuncMortarField), .Spawn = CFuncMortarField_Spawn,
	.vt = &CFuncMortarField_vt, .save = &sv_CFuncMortarField,
};

/* ---- monster_mortar (CMortar) --------------------------------------------------- */

void CMortar_MortarExplode(int e)
{
	const float *o = ent_cv(e)->origin;
	col_trace_t tr;
	float top[3], bottom[3];

	/* mortar beam (TE_BEAMPOINTS: life 0.1 s, width 40, colour 255 160 100,
	 * brightness 128) */
	VecCopy(o, top);
	top[2] += 1024;
	fx_beam(-1, -1, o, -1, top, RESID(SLV_LGTNING_SPR), 40, 0, 255, 160, 100, 128, 0, 1);
	VecCopy(o, bottom);
	bottom[2] -= 1024;
	UTIL_TraceLine(top, bottom, dont_ignore_monsters, e, &tr);
	log_printf("mortar: %d explodes at %.9g %.9g %.9g\n", e, (double)tr.end[0],
		(double)tr.end[1], (double)tr.end[2]);
	CGrenade_Explode(e, &tr, DMG_BLAST | DMG_MORTAR);
	UTIL_ScreenShake(tr.end, 25.0f, 150.0f, 1.0f, 750);
}

static int CMortar_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	ent_set_movetype(e, MOVETYPE_NONE);
	ent_set_solid(e, SOLID_NOT);
	CBase(e)->dmg = 200;
	SetThink(e, FN_CMortar_MortarExplode);
	ent_think_at(e, 0);
	PRECACHE(RESID(SLV_LGTNING_SPR));
	return 0;
}

static int Projectile_BloodColor(int e)
{
	(void)e;
	return DONT_BLEED;
}

static const game_vt_t CMortar_vt = {
	.BloodColor = Projectile_BloodColor,
	.Killed = CGrenade_KilledVt,
};

/* a rocket in flight does not persist (as thrown grenades) */
static int projectile_quiet(int e)
{
	(void)e;
	return -1;
}

static const sv_table_t sv_Projectile = { .base = &sv_CGrenade, .quiet = projectile_quiet };

/* a mortar waiting to strike (at most 2.5 s plus the field's stagger)
 * stays until it has struck, also out past the release radius: GoldSrc
 * strikes wherever the player is; then a transient */
static int mortar_quiet(int e)
{
	return ent_tm(e)->think == FN_CMortar_MortarExplode ? 0 : -1;
}

static const sv_table_t sv_Mortar = { .base = &sv_CGrenade, .quiet = mortar_quiet };

const game_class_t gc_MONSTER_MORTAR = {
	.size = sizeof(CGrenade), .Spawn = CMortar_Spawn, .vt = &CMortar_vt,
	.save = &sv_Mortar,
};

/* ---- rpg_rocket (CRpgRocket) ---------------------------------------------------- */

typedef struct {
	CGrenade	g;
	float		m_flIgniteTime;
} CRpgRocket;

#define R(e)	PRIV(CRpgRocket, e)

/* CRpgRocket::Explode */
static void RocketExplode(int e, const col_trace_t *tr, int bits)
{
	STOP_SOUND(e, CHAN_VOICE, RESID(W_ROCKET1));
	log_printf("rocket: %d explodes at %.9g %.9g %.9g\n", e, (double)ent_cv(e)->origin[0],
		(double)ent_cv(e)->origin[1], (double)ent_cv(e)->origin[2]);
	CGrenade_Explode(e, tr, bits);
}

/* CGrenade::Detonate on the rocket's Explode */
static void RocketDetonate(int e)
{
	col_trace_t tr;
	float spot[3], end[3];

	VecCopy(ent_cv(e)->origin, spot);
	spot[2] += 8;
	VecCopy(spot, end);
	end[2] -= 40;
	UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
	RocketExplode(e, &tr, DMG_BLAST);
}

/* CGrenade::ExplodeTouch on the rocket's Explode */
void CRpgRocket_ExplodeTouch(int e, int other)
{
	col_trace_t tr;
	float dir[3], spot[3], end[3];

	R(e)->g.enemy = EHANDLE(other);
	VecNorm(ent_mot(e)->velocity, dir);
	VecMA(ent_cv(e)->origin, -32, dir, spot);
	VecMA(spot, 64, dir, end);
	UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
	RocketExplode(e, &tr, DMG_BLAST);
}

void CRpgRocket_IgniteThink(int e)
{
	ent_set_movetype(e, MOVETYPE_FLY);
	ent_rnd(e)->effects |= EF_LIGHT;
	/* make rocket sound */
	EMIT_SOUND(e, CHAN_VOICE, RESID(W_ROCKET1), 1, 0.5f);
	/* rocket trail (TE_BEAMFOLLOW sprites/smoke.spr, life 4 s, width 5,
	 * colour 224 224 255, brightness 255): the effects API has no
	 * entity-following trail beam */
	R(e)->m_flIgniteTime = gtime();
	/* set to follow laser spot */
	SetThink(e, FN_CRpgRocket_FollowThink);
	ThinkIn(e, 0.1f);
}

void CRpgRocket_FollowThink(int e)
{
	CRpgRocket *p = R(e);
	float vecTarget[3], v[3], flSpeed;

	UTIL_MakeAimVectors(ent_cv(e)->angles);
	VecCopy(gpGlobals.v_forward, vecTarget);
	/* HLSDK examines every laser_spot here; hl64 has no RPG weapon and so
	 * none: the rocket flies straight on */
	UTIL_VecToAngles(vecTarget, ent_cv(e)->angles);
	/* this acceleration and turning math is totally wrong, but it seems to
	 * respond well so don't change it. */
	VecCopy(ent_mot(e)->velocity, v);
	flSpeed = VecLen(v);
	if (gtime() - p->m_flIgniteTime < 1.0f)
	{
		VecScale(v, 0.2f, v);
		VecMA(v, flSpeed * 0.8f + 400, vecTarget, v);
		if (ent_phys(e)->waterlevel == 3)
		{
			/* go slow underwater (UTIL_BubbleTrail: no bubble effect in
			 * the effects API) */
			if (VecLen(v) > 300)
			{
				VecNorm(v, v);
				VecScale(v, 300, v);
			}
		}
		else if (VecLen(v) > 2000)
		{
			VecNorm(v, v);
			VecScale(v, 2000, v);
		}
		ent_set_velocity(e, v);
	}
	else
	{
		if (ent_rnd(e)->effects & EF_LIGHT)
		{
			ent_rnd(e)->effects = 0;
			STOP_SOUND(e, CHAN_VOICE, RESID(W_ROCKET1));
		}
		VecScale(v, 0.2f, v);
		VecMA(v, flSpeed * 0.798f, vecTarget, v);
		ent_set_velocity(e, v);
		if (ent_phys(e)->waterlevel == 0 && VecLen(v) < 1500)
			RocketDetonate(e);
	}
	if (UTIL_PointContents(ent_cv(e)->origin) == CONTENTS_SKY)
		RocketDetonate(e);
	ThinkIn(e, 0.1f);
}

static int CRpgRocket_Spawn(int e, gid_t16 gid)
{
	CRpgRocket *p = R(e);
	float *ang = ent_cv(e)->angles, v[3];

	(void)gid;
	PRECACHE(RESID(RPGROCKET_MDL));
	PRECACHE(RESID(SPR_SMOKE_TRAIL));
	PRECACHE(RESID(W_ROCKET1));
	/* motor */
	ent_set_movetype(e, MOVETYPE_BOUNCE);
	ent_set_solid(e, SOLID_BBOX);
	ent_set_model(e, RESID(RPGROCKET_MDL));
	UTIL_SetSize(e, g_vecZero, g_vecZero);
	UTIL_SetOrigin(e, ent_cv(e)->origin);
	SetThink(e, FN_CRpgRocket_IgniteThink);
	SetTouch(e, FN_CRpgRocket_ExplodeTouch);
	ang[0] -= 30;
	UTIL_MakeVectors(ang);
	ang[0] = -(ang[0] + 30);
	VecScale(gpGlobals.v_forward, 250, v);
	ent_set_velocity(e, v);
	ent_phys(e)->gravity = 0.5f;
	ThinkIn(e, 0.4f);
	p->g.dmg = mon_skill(sk_plr_rpg);
	return 0;
}

static const game_vt_t CRpgRocket_vt = {
	.BloodColor = Projectile_BloodColor,
	.Killed = CGrenade_KilledVt,
};

const game_class_t gc_RPG_ROCKET = {
	.size = sizeof(CRpgRocket), .Spawn = CRpgRocket_Spawn, .vt = &CRpgRocket_vt,
	.save = &sv_Projectile,
};
