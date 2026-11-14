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
 * weapons.c -- HLSDK dlls/weapons.cpp (CBasePlayerItem, CBasePlayerWeapon,
 * CBasePlayerAmmo, CWeaponBox, DecalGunshot, EjectBrass), the player's
 * weapon methods of player.cpp (FireBulletsPlayer of combat.cpp,
 * GetAutoaimVector / AutoaimDeflection, ItemPreFrame / ItemPostFrame),
 * util.cpp UTIL_SharedRandom*, the client weapon events' effects of
 * cl_dll/ev_hldm.cpp / ev_common.cpp (shells, muzzle flash, punch,
 * tracers, impact decals and texture sounds: one process, so they run
 * where the server fires), the skill.cfg player values and the weapon /
 * ammo info tables (weapons.h, each weapon's GetItemInfo). Weapon state
 * lives in the player block (game/hl/player.h); world weapons are
 * CBasePlayerItem entities until picked up.
 */
#include <string.h>

#include "game/hl/player.h"

/* ---- skill.cfg ------------------------------------------------------------- */

skilldata_t gSkillData;

/* the HL / Uplink skill.cfg values (sk_<name>1..3) */
static const float sk_plr_crowbar[3] = { 10, 10, 10 };
static const float sk_plr_9mm_bullet[3] = { 8, 8, 8 };
static const float sk_plr_357_bullet[3] = { 40, 40, 40 };
static const float sk_plr_9mmAR_bullet[3] = { 5, 5, 5 };
static const float sk_plr_9mmAR_grenade[3] = { 100, 100, 100 };
static const float sk_plr_buckshot[3] = { 5, 5, 5 };
static const float sk_plr_hand_grenade[3] = { 100, 100, 100 };
static const float sk_plr_tripmine[3] = { 150, 150, 150 };
static const float sk_healthkit[3] = { 15, 15, 10 };
static const float sk_battery[3] = { 15, 15, 10 };
static const float sk_healthcharger[3] = { 50, 40, 25 };
static const float sk_suitcharger[3] = { 75, 50, 35 };
static const float sk_player_head[3] = { 3, 3, 3 };
static const float sk_player_chest[3] = { 1, 1, 1 };
static const float sk_player_stomach[3] = { 1, 1, 1 };
static const float sk_player_arm[3] = { 1, 1, 1 };
static const float sk_player_leg[3] = { 1, 1, 1 };

void SkillDataInit(void)
{
	gSkillData.plrDmgCrowbar = mon_skill(sk_plr_crowbar);
	gSkillData.plrDmg9MM = mon_skill(sk_plr_9mm_bullet);
	gSkillData.plrDmg357 = mon_skill(sk_plr_357_bullet);
	gSkillData.plrDmgMP5 = mon_skill(sk_plr_9mmAR_bullet);
	gSkillData.plrDmgM203Grenade = mon_skill(sk_plr_9mmAR_grenade);
	gSkillData.plrDmgBuckshot = mon_skill(sk_plr_buckshot);
	gSkillData.plrDmgHandGrenade = mon_skill(sk_plr_hand_grenade);
	gSkillData.plrDmgTripmine = mon_skill(sk_plr_tripmine);
	gSkillData.healthkitCapacity = mon_skill(sk_healthkit);
	gSkillData.batteryCapacity = mon_skill(sk_battery);
	gSkillData.healthchargerCapacity = mon_skill(sk_healthcharger);
	gSkillData.suitchargerCapacity = mon_skill(sk_suitcharger);
	gSkillData.plrHead = mon_skill(sk_player_head);
	gSkillData.plrChest = mon_skill(sk_player_chest);
	gSkillData.plrStomach = mon_skill(sk_player_stomach);
	gSkillData.plrArm = mon_skill(sk_player_arm);
	gSkillData.plrLeg = mon_skill(sk_player_leg);
}

/* ---- item info (GetItemInfo of each weapon) ------------------------------------ */

static ItemInfo item_info[WPN_SLOTS];

const ItemInfo *ItemInfoOf(int id)
{
	return id > 0 && id < WPN_SLOTS && item_info[id].cls ? &item_info[id] : NULL;
}

static void info(int id, int slot, int pos, int a1, int m1, int a2, int m2, int clip, int flags,
	int weight, int defammo, cls_t cls, res_t v, res_t p, res_t w)
{
	ItemInfo *ii = &item_info[id];

	ii->iSlot = (int8_t)slot;
	ii->iPosition = (int8_t)pos;
	ii->iAmmo1 = (int8_t)a1;
	ii->iMaxAmmo1 = (int16_t)m1;
	ii->iAmmo2 = (int8_t)a2;
	ii->iMaxAmmo2 = (int16_t)m2;
	ii->iMaxClip = (int16_t)clip;
	ii->iFlags = (int8_t)flags;
	ii->iWeight = (int8_t)weight;
	ii->iDefaultAmmo = (int16_t)defammo;
	ii->cls = cls;
	ii->vmodel = v;
	ii->pmodel = p;
	ii->wmodel = w;
}

void W_Precache(void)
{
	memset(item_info, 0, sizeof(item_info));
	info(WEAPON_CROWBAR, 0, 0, AMMO_NONE, -1, AMMO_NONE, -1, WEAPON_NOCLIP, 0, CROWBAR_WEIGHT,
		0, CLS_WEAPON_CROWBAR, RESID(W_V_CROWBAR), RESID(W_P_CROWBAR), RESID(W_W_CROWBAR));
	info(WEAPON_GLOCK, 1, 0, AMMO_9MM, _9MM_MAX_CARRY, AMMO_NONE, -1, GLOCK_MAX_CLIP, 0,
		GLOCK_WEIGHT, GLOCK_DEFAULT_GIVE, CLS_WEAPON_9MMHANDGUN, RESID(W_V_9MMHANDGUN),
		RESID(W_P_9MMHANDGUN), RESID(W_W_9MMHANDGUN));
	info(WEAPON_PYTHON, 1, 1, AMMO_357, _357_MAX_CARRY, AMMO_NONE, -1, PYTHON_MAX_CLIP, 0,
		PYTHON_WEIGHT, PYTHON_DEFAULT_GIVE, CLS_WEAPON_357, RESID(W_V_357), RESID(W_P_357),
		RESID(W_W_357));
	info(WEAPON_MP5, 2, 0, AMMO_9MM, _9MM_MAX_CARRY, AMMO_ARGRENADES, M203_GRENADE_MAX_CARRY,
		MP5_MAX_CLIP, 0, MP5_WEIGHT, MP5_DEFAULT_GIVE, CLS_WEAPON_9MMAR, RESID(W_V_9MMAR),
		RESID(W_P_9MMAR), RESID(W_W_9MMAR));
	info(WEAPON_SHOTGUN, 2, 1, AMMO_BUCKSHOT, BUCKSHOT_MAX_CARRY, AMMO_NONE, -1,
		SHOTGUN_MAX_CLIP, 0, SHOTGUN_WEIGHT, SHOTGUN_DEFAULT_GIVE, CLS_WEAPON_SHOTGUN,
		RESID(W_V_SHOTGUN), RESID(W_P_SHOTGUN), RESID(W_W_SHOTGUN));
	info(WEAPON_HANDGRENADE, 4, 0, AMMO_HANDGRENADE, HANDGRENADE_MAX_CARRY, AMMO_NONE, -1,
		WEAPON_NOCLIP, ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE, HANDGRENADE_WEIGHT,
		HANDGRENADE_DEFAULT_GIVE, CLS_WEAPON_HANDGRENADE, RESID(W_V_GRENADE),
		RESID(W_P_GRENADE), RESID(W_W_GRENADE));
	/* the world tripmine is v_tripmine's TRIPMINE_GROUND sequence */
	info(WEAPON_TRIPMINE, 4, 2, AMMO_TRIPMINE, TRIPMINE_MAX_CARRY, AMMO_NONE, -1,
		WEAPON_NOCLIP, ITEM_FLAG_LIMITINWORLD | ITEM_FLAG_EXHAUSTIBLE, TRIPMINE_WEIGHT,
		TRIPMINE_DEFAULT_GIVE, CLS_WEAPON_TRIPMINE, RESID(W_V_TRIPMINE),
		RESID(W_P_TRIPMINE), RESID(W_V_TRIPMINE));
}

int MaxAmmoCarry(int ammo)
{
	int i;

	if (ammo <= AMMO_NONE)
		return -1;
	for (i = 1; i < WPN_SLOTS; i++)
	{
		const ItemInfo *ii = &item_info[i];

		if (!ii->cls)
			continue;
		if (ii->iAmmo1 == ammo)
			return ii->iMaxAmmo1;
		if (ii->iAmmo2 == ammo)
			return ii->iMaxAmmo2;
	}
	return -1;
}

static const weapon_vt_t *wvt(int id)
{
	switch (id)
	{
	case WEAPON_CROWBAR: return &wvt_crowbar;
	case WEAPON_GLOCK: return &wvt_glock;
	case WEAPON_PYTHON: return &wvt_python;
	case WEAPON_MP5: return &wvt_mp5;
	case WEAPON_SHOTGUN: return &wvt_shotgun;
	case WEAPON_HANDGRENADE: return &wvt_handgrenade;
	case WEAPON_TRIPMINE: return &wvt_tripmine;
	default: return NULL;
	}
}

/* ---- util.cpp shared random ------------------------------------------------------ */

static unsigned int glSeed;

static const unsigned int seed_table[256] = {
	28985, 27138, 26457, 9451, 17764, 10909, 28790, 8716, 6361, 4853, 17798, 21977, 19643, 20662, 10834, 20103,
	27067, 28634, 18623, 25849, 8576, 26234, 23887, 18228, 32587, 4836, 3306, 1811, 3035, 24559, 18399, 315,
	26766, 907, 24102, 12370, 9674, 2972, 10472, 16492, 22683, 11529, 27968, 30406, 13213, 2319, 23620, 16823,
	10013, 23772, 21567, 1251, 19579, 20313, 18241, 30130, 8402, 20807, 27354, 7169, 21211, 17293, 5410, 19223,
	10255, 22480, 27388, 9946, 15628, 24389, 17308, 2370, 9530, 31683, 25927, 23567, 11694, 26397, 32602, 15031,
	18255, 17582, 1422, 28835, 23607, 12597, 20602, 10138, 5212, 1252, 10074, 23166, 19823, 31667, 5902, 24630,
	18948, 14330, 14950, 8939, 23540, 21311, 22428, 22391, 3583, 29004, 30498, 18714, 4278, 2437, 22430, 3439,
	28313, 23161, 25396, 13471, 19324, 15287, 2563, 18901, 13103, 16867, 9714, 14322, 15197, 26889, 19372, 26241,
	31925, 14640, 11497, 8941, 10056, 6451, 28656, 10737, 13874, 17356, 8281, 25937, 1661, 4850, 7448, 12744,
	21826, 5477, 10167, 16705, 26897, 8839, 30947, 27978, 27283, 24685, 32298, 3525, 12398, 28726, 9475, 10208,
	617, 13467, 22287, 2376, 6097, 26312, 2974, 9114, 21787, 28010, 4725, 15387, 3274, 10762, 31695, 17320,
	18324, 12441, 16801, 27376, 22464, 7500, 5666, 18144, 15314, 31914, 31627, 6495, 5226, 31203, 2331, 4668,
	12650, 18275, 351, 7268, 31319, 30119, 7600, 2905, 13826, 11343, 13053, 15583, 30055, 31093, 5067, 761,
	9685, 11070, 21369, 27155, 3663, 26542, 20169, 12161, 15411, 30401, 7580, 31784, 8985, 29367, 20989, 14203,
	29694, 21167, 10337, 1706, 28578, 887, 3373, 19477, 14382, 675, 7033, 15111, 26138, 12252, 30996, 21409,
	25678, 18555, 13256, 23316, 22407, 16727, 991, 9236, 5373, 29402, 6117, 15241, 27715, 19291, 19888, 19847
};

static unsigned int U_Random(void)
{
	glSeed *= 69069;
	glSeed += seed_table[glSeed & 0xff];
	return (++glSeed & 0x0fffffff);
}

static void U_Srand(unsigned int seed)
{
	glSeed = seed_table[seed & 0xff];
}

int UTIL_SharedRandomLong(unsigned int seed, int low, int high)
{
	unsigned int range;

	U_Srand((int)seed + low + high);
	range = (unsigned int)(high - low + 1);
	if (!(range - 1))
		return low;
	return low + (int)(U_Random() % range);
}

float UTIL_SharedRandomFloat(unsigned int seed, float low, float high)
{
	union { float f; int i; } l = { low }, h = { high };
	float range;

	U_Srand((int)seed + l.i + h.i);
	U_Random();
	U_Random();
	range = high - low;
	if (!range)
		return low;
	return low + (float)(U_Random() & 65535) / 65536.0f * range;
}

/* ---- effects (weapons.cpp, ev_common.cpp) ----------------------------------------- */

void DecalGunshot(const col_trace_t *tr, int iBulletType)
{
	int hit = tr->ent >= 0 ? tr->ent : 0;
	const col_ent_t *c;

	/* UTIL_IsValidEntity: the world (slot 0, SOLID_BSP in HLSDK; no live
	 * entity in hl64) or a live entity */
	if (hit != ENT_WORLD)
	{
		if (!ent_is_live(hit))
			return;
		c = ent_cv(hit);
		if (c->solid != SOLID_BSP && c->movetype != MOVETYPE_PUSHSTEP)
			return;
	}
	switch (iBulletType)
	{
	case BULLET_PLAYER_CROWBAR:
		/* wall decal */
		UTIL_DecalTrace(tr, DamageDecal(hit, DMG_CLUB));
		break;
	default:
		/* UTIL_GunshotDecalTrace: TE_GUNSHOTDECAL (the decal and the
		 * client's gunshot effect) */
		if (tr->frac == 1.0f)
			break;
		UTIL_DecalTrace(tr, DamageDecal(hit, DMG_BULLET));
		fx_gunshot(tr->end);
		break;
	}
}

void EjectBrass(const float org[3], const float vel[3], float rotation, res_t model, int soundtype)
{
	/* TE_MODEL with a 2.5 s life */
	fx_temp_model(org, vel, rotation, model, soundtype, 25);
}

/* EV_GetDefaultShellInfo + EV_EjectBrass for the local player */
void W_EjectShell(res_t model, float fwd, float up, float right, int soundtype)
{
	const ent_player_t *pl = phy_player();
	float f[3], r[3], u[3], org[3], vel[3], fR, fU;
	int i;

	AngleVectors(pl->view_angles, f, r, u);
	fR = hl64_rand_float(50, 70);
	fU = hl64_rand_float(100, 150);
	for (i = 0; i < 3; i++)
	{
		vel[i] = pl->velocity[i] + r[i] * fR + u[i] * fU + f[i] * 25;
		org[i] = pl->origin[i] + pl->view_ofs[i] + u[i] * up + f[i] * fwd + r[i] * right;
	}
	EjectBrass(org, vel, pl->view_angles[1], model, soundtype);
}

/* V_PunchAxis: the client's punch (the view adds punchangle) */
void W_PunchAxis(int axis, float punch)
{
	phy_player()->punchangle[axis] = punch;
}

/* ---- autoaim (player.cpp) ------------------------------------------------------------ */

static float	m_vecAutoAim[3];
static int	m_fOnTarget;

static void AutoaimDeflection(const float src[3], float flDist, float flDelta, float out[3])
{
	const ent_player_t *pl = phy_player();
	col_trace_t tr;
	float a[3], bestdir[3], end[3], bestdot = flDelta;
	int e = -1, bestent = -1;

	/* sv_aim 1 */
	VecAdd(pl->view_angles, pl->punchangle, a);
	VecAdd(a, m_vecAutoAim, a);
	UTIL_MakeVectors(a);
	VecCopy(gpGlobals.v_forward, bestdir);
	m_fOnTarget = 0;
	VecMA(src, flDist, bestdir, end);
	UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tr);
	if (tr.ent > 0 && CBase(tr.ent) && CBase(tr.ent)->takedamage != DAMAGE_NO)
	{
		/* don't look through water */
		int wl = ent_phys(tr.ent)->waterlevel;

		if (!((pl->waterlevel != 3 && wl == 3) || (pl->waterlevel == 3 && wl == 0)))
		{
			if (CBase(tr.ent)->takedamage == DAMAGE_AIM)
				m_fOnTarget = 1;
			VecCopy(m_vecAutoAim, out);
			return;
		}
	}
	while ((e = ent_next_in_sphere(e, src, flDist)) >= 0)
	{
		float center[3], dir[3], dot, d[3];
		int wl;

		if (e == ENT_PLAYER || !CBase(e) || CBase(e)->takedamage != DAMAGE_AIM)
			continue;
		if (!IsAlive(e))
			continue;
		wl = ent_phys(e)->waterlevel;
		if ((pl->waterlevel != 3 && wl == 3) || (pl->waterlevel == 3 && wl == 0))
			continue;
		BodyTarget(e, src, center);
		if (ent_frame_delta(e, d))
			VecAdd(center, d, center);
		VecSub(center, src, dir);
		VecNorm(dir, dir);
		/* only target if in front of the player */
		if (VecDot(dir, gpGlobals.v_forward) < 0)
			continue;
		dot = fabsf(VecDot(dir, gpGlobals.v_right)) + fabsf(VecDot(dir, gpGlobals.v_up)) * 0.5f;
		/* tweak for distance */
		VecSub(center, src, d);
		dot *= 1.0f + 0.2f * (VecLen(d) / flDist);
		if (dot > bestdot)
			continue;	/* too far to turn */
		UTIL_TraceLine(src, center, dont_ignore_monsters, ENT_PLAYER, &tr);
		if (tr.frac != 1.0f && tr.ent != e)
			continue;
		/* don't shoot at friends */
		if (CBaseMonster_IRelationship(ENT_PLAYER, e) < 0)
			continue;
		bestdot = dot;
		bestent = e;
		VecCopy(dir, bestdir);
	}
	if (bestent >= 0)
	{
		UTIL_VecToAngles(bestdir, out);
		out[0] = -out[0];
		VecSub(out, pl->view_angles, out);
		VecSub(out, pl->punchangle, out);
		if (CBase(bestent)->takedamage == DAMAGE_AIM)
			m_fOnTarget = 1;
		return;
	}
	VecSet(out, 0, 0, 0);
}

/* GetAutoaimVector(flDelta): the aim into gpGlobals->v_forward */
static void GetAutoaimVector(float flDelta, float out[3])
{
	const ent_player_t *pl = phy_player();
	float src[3], angles[3], a[3];

	if (settings.skill == 3)
	{
		Player_MakeAimVectors();
		VecCopy(gpGlobals.v_forward, out);
		return;
	}
	Player_GunPosition(src);
	VecSet(m_vecAutoAim, 0, 0, 0);
	AutoaimDeflection(src, 8192, flDelta, angles);
	/* AllowAutoTargetCrosshair: easy skill only */
	if (settings.skill != 1)
		m_fOnTarget = 0;
	if (angles[0] > 180) angles[0] -= 360;
	if (angles[0] < -180) angles[0] += 360;
	if (angles[1] > 180) angles[1] -= 360;
	if (angles[1] < -180) angles[1] += 360;
	if (angles[0] > 25) angles[0] = 25;
	if (angles[0] < -25) angles[0] = -25;
	if (angles[1] > 12) angles[1] = 12;
	if (angles[1] < -12) angles[1] = -12;
	/* always use non-sticky autoaim */
	if (settings.skill == 1)
	{
		VecScale(m_vecAutoAim, 0.67f, m_vecAutoAim);
		VecMA(m_vecAutoAim, 0.33f, angles, m_vecAutoAim);
	}
	else
		VecScale(angles, 0.9f, m_vecAutoAim);
	VecAdd(pl->view_angles, pl->punchangle, a);
	VecAdd(a, m_vecAutoAim, a);
	UTIL_MakeVectors(a);
	VecCopy(gpGlobals.v_forward, out);
}

#define AUTOAIM_5DEGREES	0.08715574274766f
#define AUTOAIM_10DEGREES	0.1736481776669f

void W_AutoaimVector(int degrees10, float out[3])
{
	GetAutoaimVector(degrees10 ? AUTOAIM_10DEGREES : AUTOAIM_5DEGREES, out);
}

int W_OnTarget(void)
{
	return m_fOnTarget;
}

/* ---- FireBulletsPlayer (combat.cpp) with EV_HLDM_FireBullets ------------------------ */

static int tracerCount;

void W_FireBulletsPlayer(int cShots, const float src[3], const float dirShooting[3],
	const float spread[3], float dist, int bullet, int tracerfreq, int damage, float out[2])
{
	CBasePlayer *p = Player();
	col_trace_t tr;
	float right[3], up[3], x = 0, y = 0;
	int shot;

	VecCopy(gpGlobals.v_right, right);
	VecCopy(gpGlobals.v_up, up);
	ClearMultiDamage();
	for (shot = 1; shot <= cShots; shot++)
	{
		float dir[3], end[3];
		int hit, tracer = 0;

		x = UTIL_SharedRandomFloat(p->random_seed + shot, -0.5f, 0.5f) +
			UTIL_SharedRandomFloat(p->random_seed + (1 + shot), -0.5f, 0.5f);
		y = UTIL_SharedRandomFloat(p->random_seed + (2 + shot), -0.5f, 0.5f) +
			UTIL_SharedRandomFloat(p->random_seed + (3 + shot), -0.5f, 0.5f);
		VecMA(dirShooting, x * spread[0], right, dir);
		VecMA(dir, y * spread[1], up, dir);
		VecMA(src, dist, dir, end);
		UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tr);
		/* EV_HLDM_CheckTracer */
		if (tracerfreq != 0 && (tracerCount++ % tracerfreq) == 0)
		{
			float ts[3];
			int i;

			for (i = 0; i < 3; i++)
				ts[i] = src[i] + right[i] * 2 + dirShooting[i] * 16;
			ts[2] -= 4;
			if (tracerfreq != 1)
				tracer = 1;
			fx_tracer(ts, tr.end);
		}
		if (tr.frac == 1.0f)
			continue;
		hit = tr.ent >= 0 ? tr.ent : 0;
		if (damage)
		{
			TraceAttack(hit, ENT_PLAYER, (float)damage, dir, &tr,
				DMG_BULLET | (damage > 16 ? DMG_ALWAYSGIB : DMG_NEVERGIB));
			TEXTURETYPE_PlaySound(&tr, src, end, bullet);
			DecalGunshot(&tr, bullet);
			continue;
		}
		switch (bullet)
		{
		default:
		case BULLET_PLAYER_9MM:
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmg9MM, dir, &tr, DMG_BULLET);
			break;
		case BULLET_PLAYER_MP5:
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmgMP5, dir, &tr, DMG_BULLET);
			break;
		case BULLET_PLAYER_BUCKSHOT:
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmgBuckshot, dir, &tr, DMG_BULLET);
			break;
		case BULLET_PLAYER_357:
			TraceAttack(hit, ENT_PLAYER, gSkillData.plrDmg357, dir, &tr, DMG_BULLET);
			break;
		}
		/* the client event's impact (EV_HLDM_FireBullets) */
		switch (bullet)
		{
		default:
		case BULLET_PLAYER_9MM:
		case BULLET_PLAYER_357:
			TEXTURETYPE_PlaySound(&tr, src, end, bullet);
			DecalGunshot(&tr, bullet);
			break;
		case BULLET_PLAYER_MP5:
			if (!tracer)
			{
				TEXTURETYPE_PlaySound(&tr, src, end, bullet);
				DecalGunshot(&tr, bullet);
			}
			break;
		case BULLET_PLAYER_BUCKSHOT:
			DecalGunshot(&tr, bullet);
			break;
		}
	}
	ApplyMultiDamage(ENT_PLAYER, ENT_PLAYER);
	if (out)
	{
		out[0] = x * spread[0];
		out[1] = y * spread[1];
	}
}

/* ---- CBasePlayerWeapon ------------------------------------------------------------ */

static CBasePlayerWeapon *W(int id)
{
	return &Player()->wpn[id];
}

void W_SendWeaponAnim(int anim)
{
	CBasePlayer *p = Player();

	p->weaponanim = (uint8_t)anim;
	p->weaponanim_time = gtime();
}

int W_CanDeploy(int id)
{
	const ItemInfo *ii = ItemInfoOf(id);
	CBasePlayer *p = Player();
	int has = 0;

	if (!ii)
		return 0;
	if (!ii->iAmmo1)
		return 1;	/* this weapon doesn't use ammo, can always deploy */
	has |= p->m_rgAmmo[(int)ii->iAmmo1] != 0;
	if (ii->iAmmo2)
		has |= p->m_rgAmmo[(int)ii->iAmmo2] != 0;
	if (W(id)->m_iClip > 0)
		has |= 1;
	return has;
}

int W_IsUseable(int id)
{
	const ItemInfo *ii = ItemInfoOf(id);

	if (W(id)->m_iClip <= 0)
	{
		if (Player()->m_rgAmmo[(int)ii->iAmmo1] <= 0 && ii->iMaxAmmo1 != -1)
			return W_CanDeploy(id);
	}
	return 1;
}

int W_CanHolster(int id)
{
	const weapon_vt_t *vt = wvt(id);

	return vt && vt->CanHolster ? vt->CanHolster(id) : 1;
}

int W_DefaultDeploy(int id, int anim)
{
	const ItemInfo *ii = ItemInfoOf(id);
	CBasePlayer *p = Player();

	if (!W_CanDeploy(id))
		return 0;
	p->viewmodel = ii->vmodel;
	W_SendWeaponAnim(anim);
	p->m_flNextAttack = 0.5f;
	W(id)->m_flTimeWeaponIdle = 1.0f;
	W(id)->m_flLastFireTime = 0;
	return 1;
}

int W_Deploy(int id)
{
	const weapon_vt_t *vt = wvt(id);
	int r = vt && vt->Deploy ? vt->Deploy(id) : 0;

	hud_weapon_select(id);
	log_printf("weapon: deploy %s\n", game_class_name(ItemInfoOf(id)->cls));
	return r;
}

void W_Holster(int id)
{
	const weapon_vt_t *vt = wvt(id);

	W(id)->m_fInReload = 0;	/* cancel any reload in progress */
	if (vt && vt->Holster)
		vt->Holster(id);
	Player()->viewmodel = RES_NONE;
}

int W_DefaultReload(int id, int clipsize, int anim, float delay)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = W(id);
	int ammo = ItemInfoOf(id)->iAmmo1, j;

	if (p->m_rgAmmo[ammo] <= 0)
		return 0;
	j = clipsize - w->m_iClip;
	if (p->m_rgAmmo[ammo] < j)
		j = p->m_rgAmmo[ammo];
	if (j == 0)
		return 0;
	p->m_flNextAttack = delay;
	W_SendWeaponAnim(anim);
	w->m_fInReload = 1;
	log_printf("weapon: reload %s, clip %d + %d\n", game_class_name(ItemInfoOf(id)->cls),
		w->m_iClip, j);
	w->m_flTimeWeaponIdle = 3;
	return 1;
}

void W_PlayEmptySound(int id)
{
	if (W(id)->m_iPlayEmptySound)
	{
		EMIT_SOUND(ENT_PLAYER, CHAN_WEAPON, RESID(W_357_COCK1), 0.8f, ATTN_NORM);
		W(id)->m_iPlayEmptySound = 0;
	}
}

void W_ResetEmptySound(int id)
{
	W(id)->m_iPlayEmptySound = 1;
}

float W_GetNextAttackDelay(int id, float delay)
{
	CBasePlayerWeapon *w = W(id);
	float between, creep = 0, next;

	if (w->m_flLastFireTime == 0 || w->m_flNextPrimaryAttack == -1)
	{
		/* at this point, we are assuming that the client has stopped
		 * firing and we are going to reset our book keeping variables */
		w->m_flLastFireTime = gtime();
		w->m_flPrevPrimaryAttack = delay;
	}
	/* calculate the time between this shot and the previous */
	between = gtime() - w->m_flLastFireTime;
	if (between > 0)
		creep = between - w->m_flPrevPrimaryAttack;
	w->m_flLastFireTime = gtime();
	next = delay - creep;
	w->m_flPrevPrimaryAttack = next;
	return next;
}

/* RetireWeapon + CHalfLifeRules::GetNextBestWeapon (exhaustible weapons:
 * HLGetNextBestWeapon) */
void W_RetireWeapon(int id)
{
	CBasePlayer *p = Player();
	const ItemInfo *cur = ItemInfoOf(id);
	int i, best = 0, bestw = -1;

	p->viewmodel = RES_NONE;
	if (!cur || !(cur->iFlags & ITEM_FLAG_EXHAUSTIBLE) || !W_CanHolster(id))
		return;
	for (i = 1; i < WPN_SLOTS; i++)
	{
		const ItemInfo *ii = ItemInfoOf(i);

		if (!ii || i == id || !Player_HasWeapon(i) || (ii->iFlags & ITEM_FLAG_NOAUTOSWITCHTO))
			continue;
		if (ii->iWeight > -1 && ii->iWeight == cur->iWeight && W_CanDeploy(i))
		{
			best = i;	/* same weight: switch to it at once */
			bestw = 1000;
			break;
		}
		if (ii->iWeight > bestw && W_CanDeploy(i))
		{
			bestw = ii->iWeight;
			best = i;
		}
	}
	if (best)
	{
		W_Holster(id);
		p->m_pLastItem = (uint8_t)id;
		p->m_pActiveItem = (uint8_t)best;
		W_Deploy(best);
	}
}

void W_ItemPreFrame(void)
{
	/* CBasePlayer::ItemPreFrame: no weapon here has an ItemPreFrame */
}

void W_BaseItemPostFrame(int id)
{
	CBasePlayer *p = Player();
	CBasePlayerWeapon *w = W(id);
	const ItemInfo *ii = ItemInfoOf(id);
	const weapon_vt_t *vt = wvt(id);

	if (w->m_fInReload && p->m_flNextAttack <= 0)
	{
		/* complete the reload */
		int j = ii->iMaxClip - w->m_iClip;

		if (p->m_rgAmmo[(int)ii->iAmmo1] < j)
			j = p->m_rgAmmo[(int)ii->iAmmo1];
		w->m_iClip = (int16_t)(w->m_iClip + j);
		p->m_rgAmmo[(int)ii->iAmmo1] = (int16_t)(p->m_rgAmmo[(int)ii->iAmmo1] - j);
		w->m_fInReload = 0;
	}
	if (!(p->button & IN_BTN_ATTACK))
		w->m_flLastFireTime = 0;
	if ((p->button & IN_BTN_ATTACK2) && w->m_flNextSecondaryAttack <= 0)
	{
		if (ii->iAmmo2 && !p->m_rgAmmo[(int)ii->iAmmo2])
			w->m_fFireOnEmpty = 1;
		if (vt->SecondaryAttack)
			vt->SecondaryAttack(id);
		p->button &= ~IN_BTN_ATTACK2;
	}
	else if ((p->button & IN_BTN_ATTACK) && w->m_flNextPrimaryAttack <= 0)
	{
		if ((w->m_iClip == 0 && ii->iAmmo1) ||
			(ii->iMaxClip == -1 && !p->m_rgAmmo[(int)ii->iAmmo1]))
			w->m_fFireOnEmpty = 1;
		if (vt->PrimaryAttack)
			vt->PrimaryAttack(id);
	}
	else if ((p->button & IN_BTN_RELOAD) && ii->iMaxClip != WEAPON_NOCLIP && !w->m_fInReload)
	{
		/* reload when reload is pressed, or if no buttons are down and
		 * weapon is empty */
		if (vt->Reload)
			vt->Reload(id);
	}
	else if (!(p->button & (IN_BTN_ATTACK | IN_BTN_ATTACK2)))
	{
		/* no fire buttons down */
		w->m_fFireOnEmpty = 0;
		if (!W_IsUseable(id) && w->m_flNextPrimaryAttack < 0)
		{
			/* weapon isn't useable, switch (exhaustible weapons only) */
			if (!(ii->iFlags & ITEM_FLAG_NOAUTOSWITCHEMPTY) &&
				(ii->iFlags & ITEM_FLAG_EXHAUSTIBLE))
			{
				int before = p->m_pActiveItem;

				W_RetireWeapon(id);
				if (p->m_pActiveItem != before)
				{
					w->m_flNextPrimaryAttack = 0.3f;
					return;
				}
			}
		}
		else
		{
			/* weapon is useable. Reload if empty and weapon has waited
			 * as long as it has to after firing */
			if (w->m_iClip == 0 && !(ii->iFlags & ITEM_FLAG_NOAUTORELOAD) &&
				w->m_flNextPrimaryAttack < 0)
			{
				if (vt->Reload)
					vt->Reload(id);
				return;
			}
		}
		if (vt->WeaponIdle)
			vt->WeaponIdle(id);
		return;
	}
}

void W_ItemPostFrame(void)
{
	CBasePlayer *p = Player();
	int id = p->m_pActiveItem;
	const weapon_vt_t *vt = wvt(id);

	if (!id || !vt)
		return;
	if (vt->ItemPostFrame)
		vt->ItemPostFrame(id);
	else
		W_BaseItemPostFrame(id);
}

/* ---- world weapons (CBasePlayerItem) ------------------------------------------------ */

#define SF_NORESPAWN_ITEM	SF_NORESPAWN

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	int16_t		m_iId;
	int16_t		m_iDefaultAmmo;
	int16_t		m_iClip;
	uint16_t	pad;
} CBasePlayerItem;

/* HLSDK CBasePlayerItem / CBasePlayerWeapon m_SaveData of a weapon lying in
 * the world (m_iId, its default ammo and clip) */
static const sv_field_t f_CBasePlayerItem[] = {
	SVF(CBasePlayerItem, m_iId, SV_I16, SVK),
	SVF(CBasePlayerItem, m_iDefaultAmmo, SV_I16, 0),
	SVF(CBasePlayerItem, m_iClip, SV_I16, 0),
};
static const sv_table_t sv_CBasePlayerItem = SV_TABLE(&sv_CBaseAnimating, f_CBasePlayerItem);

#define ITEM(e)		((CBasePlayerItem *)ent_priv(e))

void CBasePlayerItem_FallThink(int e)
{
	CBasePlayerItem *it = ITEM(e);

	ThinkIn(e, 0.1f);
	if (ent_cv(e)->flags & FL_ONGROUND)
	{
		/* clatter if we have an owner (i.e., dropped by someone) */
		if (ent_cv(e)->owner != COL_NONE)
			EMIT_SOUND_DYN(e, CHAN_VOICE, RESID(W_WEAPONDROP1), 1, ATTN_NORM, 0,
				95 + RANDOM_LONG(0, 29));
		/* lie flat */
		ent_cv(e)->angles[0] = 0;
		ent_cv(e)->angles[2] = 0;
		/* Materialize */
		if (ent_rnd(e)->effects & EF_NODRAW)
		{
			EMIT_SOUND_DYN(e, CHAN_WEAPON, RESID(I_SUITCHARGEOK1), 1, ATTN_NORM, 0, 150);
			ent_rnd(e)->effects &= ~EF_NODRAW;
			ent_rnd(e)->effects |= EF_MUZZLEFLASH;
		}
		ent_set_solid(e, SOLID_TRIGGER);
		UTIL_SetOrigin(e, ent_cv(e)->origin);	/* link into world */
		SetTouch(e, FN_CBasePlayerItem_DefaultTouch);
		SetThink(e, FN_NONE);
		ent_think_at(e, 0);
		log_printf("item: %s %d landed at %.1f %.1f %.1f\n",
			game_class_name(ent_idc(e)->cls), e, (double)ent_cv(e)->origin[0],
			(double)ent_cv(e)->origin[1], (double)ent_cv(e)->origin[2]);
	}
	(void)it;
}

void CBasePlayerItem_DefaultTouch(int e, int other)
{
	CBasePlayerItem *it = ITEM(e);
	int r;

	/* if it's not a player, ignore */
	if (!IsPlayer(other))
		return;
	r = Player_AddPlayerItem(it->m_iId, it->m_iDefaultAmmo, it->m_iClip);
	if (r < 0)
		return;
	if (r > 0)
		EMIT_SOUND(other, CHAN_ITEM, RESID(I_GUNPICKUP2), 1, ATTN_NORM);
	log_printf("item: %s picked up (%s)\n", game_class_name(ent_idc(e)->cls),
		r > 0 ? "new" : "ammo");
	SUB_UseTargets(e, other, USE_TOGGLE, 0);
	SetTouch(e, FN_NONE);
	UTIL_Remove(e);
}

/* CBasePlayerItem spawn: SET_MODEL + FallInit */
static int weapon_spawn(int e, gid_t16 gid, int id)
{
	CBasePlayerItem *it = ITEM(e);
	const ItemInfo *ii = ItemInfoOf(id);
	static const float zero[3] = { 0, 0, 0 };

	/* CBasePlayerItem : CBaseAnimating : CBaseDelay: delay / killtarget
	 * keys (NAME_NONE when absent or runtime-created) */
	CBaseDelay_KeyValue(e, gid);
	if (!ii)
		return SpawnRefuse("CBasePlayerItem: weapon not registered (no GetItemInfo)");
	it->m_iId = (int16_t)id;
	it->m_iDefaultAmmo = ii->iDefaultAmmo;
	it->m_iClip = ii->iMaxClip < 1 ? WEAPON_NOCLIP : 0;
	PRECACHE(ii->vmodel);
	PRECACHE(ii->wmodel);
	ent_set_model(e, ii->wmodel);
	/* FallInit */
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_set_solid(e, SOLID_BBOX);
	UTIL_SetOrigin(e, ent_cv(e)->origin);
	UTIL_SetSize(e, zero, zero);	/* pointsize until it lands on the ground */
	SetTouch(e, FN_CBasePlayerItem_DefaultTouch);
	SetThink(e, FN_CBasePlayerItem_FallThink);
	ThinkIn(e, 0.1f);
	return 0;
}

#define WEAPON_CLASS(IDENT, ID) \
	static int IDENT##_Spawn(int e, gid_t16 gid) { return weapon_spawn(e, gid, ID); } \
	const game_class_t gc_##IDENT = { .size = sizeof(CBasePlayerItem), .Spawn = IDENT##_Spawn, \
		.save = &sv_CBasePlayerItem };

WEAPON_CLASS(WEAPON_CROWBAR, WEAPON_CROWBAR)
WEAPON_CLASS(WEAPON_9MMHANDGUN, WEAPON_GLOCK)
WEAPON_CLASS(WEAPON_GLOCK, WEAPON_GLOCK)
WEAPON_CLASS(WEAPON_357, WEAPON_PYTHON)
WEAPON_CLASS(WEAPON_SHOTGUN, WEAPON_SHOTGUN)
WEAPON_CLASS(WEAPON_9MMAR, WEAPON_MP5)
WEAPON_CLASS(WEAPON_MP5, WEAPON_MP5)
WEAPON_CLASS(WEAPON_HANDGRENADE, WEAPON_HANDGRENADE)

/* CTripmine::Spawn: the ground pose of v_tripmine (body 3, TRIPMINE_GROUND,
 * still), FallInit, then single player's pick-up box; Precache takes
 * monster_tripmine's (UTIL_PrecacheOther) */
#define TRIPMINE_GROUND		8

static int WEAPON_TRIPMINE_Spawn(int e, gid_t16 gid)
{
	static const float mins[3] = { -16, -16, 0 }, maxs[3] = { 16, 16, 28 };

	if (weapon_spawn(e, gid, WEAPON_TRIPMINE) < 0)
		return -1;
	PRECACHE(RESID(W_MINE_DEPLOY));
	PRECACHE(RESID(W_MINE_ACTIVATE));
	PRECACHE(RESID(W_MINE_CHARGE));
	ent_rnd(e)->frame = 0;
	ent_rnd(e)->body = 3;
	ent_rnd(e)->sequence = TRIPMINE_GROUND;
	ent_rnd(e)->framerate = 0;
	UTIL_SetSize(e, mins, maxs);
	return 0;
}

const game_class_t gc_WEAPON_TRIPMINE = {
	.size = sizeof(CBasePlayerItem),
	.Spawn = WEAPON_TRIPMINE_Spawn,
	.save = &sv_CBasePlayerItem,
};

/* ---- ammo (CBasePlayerAmmo) ----------------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	int16_t		ammo, give, max;
	uint16_t	pad;
} CBasePlayerAmmo;

/* CBasePlayerAmmo of our layout (HLSDK keeps them in the class's code) */
static const sv_field_t f_CBasePlayerAmmo[] = {
	SVF(CBasePlayerAmmo, ammo, SV_I16, SVK),
	SVF(CBasePlayerAmmo, give, SV_I16, 0),
	SVF(CBasePlayerAmmo, max, SV_I16, SVK),
};
static const sv_table_t sv_CBasePlayerAmmo = SV_TABLE(&sv_CBaseEntity, f_CBasePlayerAmmo);

void CBasePlayerAmmo_DefaultTouch(int e, int other)
{
	CBasePlayerAmmo *a = PRIV(CBasePlayerAmmo, e);

	if (!IsPlayer(other))
		return;
	/* AddAmmo */
	if (Player_GiveAmmo(a->give, a->ammo, a->max) != -1)
	{
		EMIT_SOUND(e, CHAN_ITEM, RESID(W_CLIP1), 1, ATTN_NORM);
		log_printf("item: %s picked up\n", game_class_name(ent_idc(e)->cls));
		SetTouch(e, FN_NONE);
		SetThink(e, FN_SUB_Remove);
		ThinkIn(e, 0.1f);
	}
}

static int ammo_spawn(int e, res_t model, int ammo, int give, int max)
{
	CBasePlayerAmmo *a = PRIV(CBasePlayerAmmo, e);
	static const float mins[3] = { -16, -16, 0 }, maxs[3] = { 16, 16, 16 };

	a->ammo = (int16_t)ammo;
	a->give = (int16_t)give;
	a->max = (int16_t)max;
	PRECACHE(model);
	ent_set_model(e, model);
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetSize(e, mins, maxs);
	UTIL_SetOrigin(e, ent_cv(e)->origin);
	SetTouch(e, FN_CBasePlayerAmmo_DefaultTouch);
	return 0;
}

#define AMMO_CLASS(IDENT, MODEL, AMMO, GIVE, MAX) \
	static int IDENT##_Spawn(int e, gid_t16 gid) \
	{ (void)gid; return ammo_spawn(e, RESID(MODEL), AMMO, GIVE, MAX); } \
	const game_class_t gc_##IDENT = { .size = sizeof(CBasePlayerAmmo), .Spawn = IDENT##_Spawn, \
		.save = &sv_CBasePlayerAmmo };

AMMO_CLASS(AMMO_9MMCLIP, W_W_9MMCLIP, AMMO_9MM, AMMO_GLOCKCLIP_GIVE, _9MM_MAX_CARRY)
AMMO_CLASS(AMMO_9MMAR, W_W_9MMARCLIP, AMMO_9MM, AMMO_MP5CLIP_GIVE, _9MM_MAX_CARRY)
AMMO_CLASS(AMMO_MP5CLIP, W_W_9MMARCLIP, AMMO_9MM, AMMO_MP5CLIP_GIVE, _9MM_MAX_CARRY)
AMMO_CLASS(AMMO_9MMBOX, W_W_CHAINAMMO, AMMO_9MM, AMMO_CHAINBOX_GIVE, _9MM_MAX_CARRY)
AMMO_CLASS(AMMO_ARGRENADES, W_W_ARGRENADE, AMMO_ARGRENADES, AMMO_M203BOX_GIVE,
	M203_GRENADE_MAX_CARRY)
AMMO_CLASS(AMMO_MP5GRENADES, W_W_ARGRENADE, AMMO_ARGRENADES, AMMO_M203BOX_GIVE,
	M203_GRENADE_MAX_CARRY)
AMMO_CLASS(AMMO_BUCKSHOT, W_W_SHOTBOX, AMMO_BUCKSHOT, AMMO_BUCKSHOTBOX_GIVE, BUCKSHOT_MAX_CARRY)
AMMO_CLASS(AMMO_357, W_W_357AMMOBOX, AMMO_357, AMMO_357BOX_GIVE, _357_MAX_CARRY)

/* ---- weaponbox (CWeaponBox) ------------------------------------------------------------ */

/* m_rgiszAmmo / m_rgAmmo: named ammo slots in pack order (slot 0 unused,
 * as HLSDK's GiveAmmo); a slot holds an AMMO_* type, 0 = free. */
#define WEAPONBOX_AMMO_SLOTS	MAX_AMMO_TYPES

typedef struct {
	CBASEENTITY_FIELDS
	uint16_t	weapons;		/* weapon id bits packed */
	int16_t		clip[WPN_SLOTS];
	uint8_t		m_rgiszAmmo[WEAPONBOX_AMMO_SLOTS];
	int16_t		m_rgAmmo[WEAPONBOX_AMMO_SLOTS];
} CWeaponBox;

/* HLSDK CWeaponBox::m_SaveData (weapons as id bits + clips, ammo slots) */
static const sv_field_t f_CWeaponBox[] = {
	SVF(CWeaponBox, weapons, SV_U16, 0),
	SVF(CWeaponBox, clip, SV_I16, 0),
	SVF(CWeaponBox, m_rgiszAmmo, SV_U8, 0),
	SVF(CWeaponBox, m_rgAmmo, SV_I16, 0),
};
static const sv_table_t sv_CWeaponBox = SV_TABLE(&sv_CBaseEntity, f_CWeaponBox);

/* the HLSDK ammo names a map's weaponbox keys carry (keys.def) and the
 * game's ammo type of each; AMMO_NONE: no weapon of the game uses it */
static const struct { key_t16 key; uint8_t ammo; const char *name; } ammo_keys[] = {
	{ K_AMMO_9MM, AMMO_9MM, "9mm" },
	{ K_AMMO_357, AMMO_357, "357" },
	{ K_AMMO_BUCKSHOT, AMMO_BUCKSHOT, "buckshot" },
	{ K_AMMO_BOLTS, AMMO_NONE, "bolts" },
	{ K_AMMO_ROCKETS, AMMO_NONE, "rockets" },
	{ K_AMMO_URANIUM, AMMO_NONE, "uranium" },
	{ K_AMMO_HORNETS, AMMO_NONE, "Hornets" },
	{ K_AMMO_HAND_GRENADE, AMMO_HANDGRENADE, "Hand Grenade" },
	{ K_AMMO_SATCHEL_CHARGE, AMMO_NONE, "Satchel Charge" },
	{ K_AMMO_TRIP_MINE, AMMO_TRIPMINE, "Trip Mine" },
	{ K_AMMO_SNARKS, AMMO_NONE, "Snarks" },
	{ K_AMMO_ARGRENADES, AMMO_ARGRENADES, "ARgrenades" },
};

void CWeaponBox_Touch(int e, int other)
{
	CWeaponBox *b = PRIV(CWeaponBox, e);
	int i;

	if (!(ent_cv(e)->flags & FL_ONGROUND))
		return;
	if (!IsPlayer(other) || !IsAlive(other))
		return;
	/* dole out ammo */
	for (i = 0; i < WEAPONBOX_AMMO_SLOTS; i++)
		if (b->m_rgiszAmmo[i])
		{
			/* there's some ammo of this type */
			Player_GiveAmmo(b->m_rgAmmo[i], b->m_rgiszAmmo[i],
				MaxAmmoCarry(b->m_rgiszAmmo[i]));
			/* now empty the ammo from the weaponbox since we just
			 * gave it to the player */
			b->m_rgiszAmmo[i] = 0;
			b->m_rgAmmo[i] = 0;
		}
	/* go through my weapons and try to give the usable ones to the player */
	for (i = 1; i < WPN_SLOTS; i++)
		if (b->weapons & (1u << i))
			Player_AddPlayerItem(i, 0, b->clip[i]);
	EMIT_SOUND(other, CHAN_ITEM, RESID(I_GUNPICKUP2), 1, ATTN_NORM);
	log_printf("item: weaponbox %d picked up\n", e);
	SetTouch(e, FN_NONE);
	UTIL_Remove(e);
}

/* PackWeapon of a runtime weaponbox */
int CWeaponBox_PackWeapon(int e, int id, int clip)
{
	CWeaponBox *b = PRIV(CWeaponBox, e);

	if (id <= 0 || id >= WPN_SLOTS || (b->weapons & (1u << id)))
		return 0;
	b->weapons |= (uint16_t)(1u << id);
	b->clip[id] = (int16_t)clip;
	return 1;
}

/* CWeaponBox::GiveAmmo: the slot of the type (topped up to iMax), else a
 * fresh slot holding iCount as given; -1 nothing added */
static int CWeaponBox_GiveAmmo(CWeaponBox *b, int iCount, int ammo, int iMax)
{
	int i;

	for (i = 1; i < WEAPONBOX_AMMO_SLOTS && b->m_rgiszAmmo[i]; i++)
		if (b->m_rgiszAmmo[i] == ammo)
		{
			int iAdd = iCount < iMax - b->m_rgAmmo[i] ? iCount : iMax - b->m_rgAmmo[i];

			if (iCount == 0 || iAdd > 0)
			{
				b->m_rgAmmo[i] = (int16_t)(b->m_rgAmmo[i] + iAdd);
				return i;
			}
			return -1;
		}
	if (i < WEAPONBOX_AMMO_SLOTS)
	{
		b->m_rgiszAmmo[i] = (uint8_t)ammo;
		b->m_rgAmmo[i] = (int16_t)iCount;
		return i;
	}
	log_printf("game: out of named ammo slots\n");
	return i;
}

/* CWeaponBox::PackAmmo (by ammo type; name for the log) */
static int CWeaponBox_PackAmmo(int e, int ammo, const char *name, int iCount)
{
	int iMaxCarry = MaxAmmoCarry(ammo);

	if (iMaxCarry == -1)
	{
		log_printf("game: MaxAmmoCarry() doesn't recognize '%s'!\n", name);
		return 0;
	}
	if (iCount > 0)
	{
		CWeaponBox_GiveAmmo(PRIV(CWeaponBox, e), iCount, ammo, iMaxCarry);
		log_printf("weaponbox: %d packed %d %s\n", e, iCount, name);
		return 1;
	}
	return 0;
}

/* CWeaponBox::KeyValue: every key of the entity is an ammo name packed
 * with atoi of its value. The cook keeps the HLSDK ammo names
 * (keys.def K_AMMO_*); the entity's other keys name no ammo, which
 * HLSDK's PackAmmo turns away the same way. */
static void CWeaponBox_KeyValues(int e, gid_t16 gid)
{
	int n = rec_key_count(gid), i;
	unsigned j;

	for (i = 0; i < n; i++)
	{
		key_t16 k = rec_key_id(gid, i);
		float v;

		for (j = 0; j < sizeof(ammo_keys) / sizeof(ammo_keys[0]); j++)
			if (ammo_keys[j].key == k && rec_key_f(gid, k, &v))
				CWeaponBox_PackAmmo(e, ammo_keys[j].ammo, ammo_keys[j].name, (int)v);
	}
}

static int CWeaponBox_Spawn(int e, gid_t16 gid)
{
	static const float zero[3] = { 0, 0, 0 };

	if (gid != GID_NONE)
		CWeaponBox_KeyValues(e, gid);
	PRECACHE(RESID(W_W_WEAPONBOX));
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetSize(e, zero, zero);
	ent_set_model(e, RESID(W_W_WEAPONBOX));
	SetTouch(e, FN_CWeaponBox_Touch);
	return 0;
}

const game_class_t gc_WEAPONBOX = {
	.size = sizeof(CWeaponBox),
	.Spawn = CWeaponBox_Spawn,
	.save = &sv_CWeaponBox,
};
