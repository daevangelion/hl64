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
 * items.c -- HLSDK dlls/items.cpp (CItem, world_items, item_suit,
 * item_battery, item_longjump), healthkit.cpp (item_healthkit) and
 * player.cpp CStripWeapons (player_weaponstrip), single-player rules (no
 * respawn). item_antidote / item_security are not in classes.def (no
 * Uplink / Hazard map places them); world_items types 42 / 43 log it.
 */
#include "game/hl/player.h"

#define SF_SUIT_SHORTLOGON	0x0001

typedef struct {
	CBASEENTITY_FIELDS
	uint8_t		kind;		/* ITEMK_* */
	uint8_t		pad[3];
} CItem;

enum { ITEMK_SUIT, ITEMK_HEALTHKIT, ITEMK_BATTERY, ITEMK_LONGJUMP };

/* MyTouch of each item */
static int MyTouch(int e, CItem *it)
{
	CBasePlayer *p = Player();
	CBaseMonster *m = PlayerMon();

	switch (it->kind)
	{
	case ITEMK_SUIT:
		if (p->weapons & (1u << WEAPON_SUIT))
			return 0;
		if (it->spawnflags & SF_SUIT_SHORTLOGON)
			Player_SuitSound(RESID(SEN_HEV_A0));	/* short version of suit logon */
		else
			Player_SuitSound(RESID(SEN_HEV_AAX));	/* long version of suit logon */
		p->weapons |= 1u << WEAPON_SUIT;
		hud_pickup(HIST_ITEM, ITEMK_SUIT, 1);
		return 1;
	case ITEMK_HEALTHKIT:
		if (m->deadflag != DEAD_NO)
			return 0;
		if (Player_TakeHealth(gSkillData.healthkitCapacity, DMG_GENERIC))
		{
			EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(I_SMALLMEDKIT1), 1, ATTN_NORM);
			hud_pickup(HIST_ITEM, ITEMK_HEALTHKIT, 1);
			return 1;
		}
		return 0;
	case ITEMK_BATTERY:
		if (m->deadflag != DEAD_NO)
			return 0;
		if (p->armorvalue < MAX_NORMAL_BATTERY && (p->weapons & (1u << WEAPON_SUIT)))
		{
			int pct;

			p->armorvalue += gSkillData.batteryCapacity;
			if (p->armorvalue > MAX_NORMAL_BATTERY)
				p->armorvalue = MAX_NORMAL_BATTERY;
			EMIT_SOUND(ENT_PLAYER, CHAN_ITEM, RESID(I_GUNPICKUP2), 1, ATTN_NORM);
			hud_pickup(HIST_ITEM, ITEMK_BATTERY, 1);
			/* suit reports new power level: round to 5% */
			pct = (int)((float)(p->armorvalue * 100.0f) * (1.0f / MAX_NORMAL_BATTERY) + 0.5f);
			pct = pct / 5;
			if (pct > 0)
				pct--;
			if (pct > 19)
				pct = 19;
			Player_SetSuitUpdate(res_def(R_SEN_HEV_0P + pct), SUIT_NEXT_IN_30SEC);
			return 1;
		}
		return 0;
	case ITEMK_LONGJUMP:
		if (p->m_fLongJump)
			return 0;
		if (p->weapons & (1u << WEAPON_SUIT))
		{
			/* player now has longjump module ("slj" physics key: the
			 * player block has no physinfo yet; see player.h) */
			p->m_fLongJump = 1;
			Player_SuitSound(RESID(SEN_HEV_A1));
			hud_pickup(HIST_ITEM, ITEMK_LONGJUMP, 1);
			return 1;
		}
		return 0;
	}
	(void)e;
	return 0;
}

void CItem_ItemTouch(int e, int other)
{
	CItem *it = PRIV(CItem, e);

	/* if it's not a player, ignore */
	if (!IsPlayer(other) || !PlayerMon())
		return;
	if (MyTouch(e, it))
	{
		log_printf("item: %s picked up\n", game_class_name(ent_idc(e)->cls));
		SUB_UseTargets(e, other, USE_TOGGLE, 0);
		SetTouch(e, FN_NONE);
		UTIL_Remove(e);
	}
}

static int CItem_Spawn(int e, res_t model, int kind)
{
	CItem *it = PRIV(CItem, e);
	static const float mins[3] = { -16, -16, 0 }, maxs[3] = { 16, 16, 16 };
	const float *o;

	it->kind = (uint8_t)kind;
	PRECACHE(model);
	ent_set_model(e, model);
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetOrigin(e, ent_cv(e)->origin);
	UTIL_SetSize(e, mins, maxs);
	SetTouch(e, FN_CItem_ItemTouch);
	if (phy_drop_to_floor(e) == 0)
	{
		o = ent_cv(e)->origin;
		log_printf("game: Item %s fell out of level at %f,%f,%f\n",
			game_class_name(ent_idc(e)->cls), (double)o[0], (double)o[1], (double)o[2]);
		UTIL_Remove(e);
		return -1;
	}
	return 0;
}

#define ITEM_CLASS(IDENT, MODEL, KIND) \
	static int IDENT##_Spawn(int e, gid_t16 gid) \
	{ (void)gid; return CItem_Spawn(e, RESID(MODEL), KIND); } \
	const game_class_t gc_##IDENT = { .size = sizeof(CItem), .Spawn = IDENT##_Spawn };

ITEM_CLASS(ITEM_SUIT, I_W_SUIT, ITEMK_SUIT)
ITEM_CLASS(ITEM_HEALTHKIT, I_W_MEDKIT, ITEMK_HEALTHKIT)
ITEM_CLASS(ITEM_BATTERY, I_W_BATTERY, ITEMK_BATTERY)
ITEM_CLASS(ITEM_LONGJUMP, I_W_LONGJUMP, ITEMK_LONGJUMP)

/* ---- world_items (CWorldItem) ----------------------------------------------- */

static int CWorldItem_Spawn(int e, gid_t16 gid)
{
	CBaseEntity *pev = CBase(e);
	int type = (int)KeyF(gid, K_TYPE, 0), n;
	cls_t cls = CLS_NONE;

	switch (type)
	{
	case 44: cls = CLS_ITEM_BATTERY; break;	/* ITEM_BATTERY */
	case 45: cls = CLS_ITEM_SUIT; break;	/* ITEM_SUIT */
	case 42:	/* ITEM_ANTIDOTE */
	case 43:	/* ITEM_SECURITY */
	default: break;
	}
	n = cls == CLS_NONE ? -1 : CBaseEntity_Create(cls, ent_cv(e)->origin, ent_cv(e)->angles, -1);
	if (n < 0)
		log_printf("game: unable to create world_item %d\n", type);
	else
	{
		CBase(n)->target = pev->target;
		ent_idc(n)->targetname = ent_idc(e)->targetname;
		if (ent_idc(n)->targetname != NAME_NONE)
			rec_name_hold(n, ent_idc(n)->targetname);
		CBase(n)->spawnflags = pev->spawnflags;
	}
	/* REMOVE_ENTITY: the record is done (the created item lives on) */
	return -1;
}

const game_class_t gc_WORLD_ITEMS = {
	.size = sizeof(CBaseEntity),
	.Spawn = CWorldItem_Spawn,
};

/* ---- player_weaponstrip (CStripWeapons: a logic record) ------------------------- */

static void CStripWeapons_LogicSpawn(gid_t16 gid)
{
	(void)gid;
}

static void CStripWeapons_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	(void)gid; (void)activator; (void)caller; (void)use_type; (void)value;
	/* single player: the player, whoever activated */
	if (PlayerMon())
		Player_RemoveAllItems(0);
}

const game_class_t gc_PLAYER_WEAPONSTRIP = {
	.LogicSpawn = CStripWeapons_LogicSpawn,
	.LogicUse = CStripWeapons_LogicUse,
};
