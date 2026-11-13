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
 * hud.c -- the HUD's message side (HLSDK cl_dll user messages, received
 * here as direct calls from the player): pickup history (ammohistory.cpp
 * AddToHistory), damage (health.cpp MsgFunc_Damage), flashlight
 * (flashlight.cpp MsgFunc_Flashlight / FlashBat), geiger (geiger.cpp
 * MsgFunc_Geiger), weapon selection.
 */
#include <string.h>

#include "game/hl/player.h"
#include "game/hud/hud.h"

hud_state_t hud;

void hud_reset(void)
{
	memset(&hud, 0, sizeof(hud));
	hud.flash_bat = 100;
}

/* HistoryResource::AddToHistory: newest at the bottom; a full list
 * drops the oldest */
void hud_pickup(int type, int id, int count)
{
	hud_hist_t *h;
	int i;

	if (type == HIST_AMMO && !count)
		return;
	if (hud.nhist == HUD_HISTORY)
	{
		for (i = 1; i < HUD_HISTORY; i++)
			hud.hist[i - 1] = hud.hist[i];
		hud.nhist--;
	}
	h = &hud.hist[hud.nhist++];
	h->type = (uint8_t)type;
	h->id = (uint8_t)id;
	h->count = (int16_t)count;
	h->time = (float)hud.time + HISTORY_DRAW_TIME;
}

void hud_damage(int armor, int damage, int bits, const float from[3])
{
	hud_msg_damage(armor, damage, bits, from);
}

void hud_flashlight(int on, int battery)
{
	hud.flash_on = (uint8_t)on;
	hud.flash_bat = (uint8_t)battery;
}

void hud_weapon_select(int id)
{
	hud.select_id = (uint8_t)id;
	hud.select_time = (float)hud.time + HUD_SELECT_TIME;
}

void hud_geiger(int range)
{
	hud.geiger_range = range;
}

/* ---- CHudHealth messages (health.cpp) ------------------------------------------- */

/* giDmgFlags: the damage icon order of 320hud4 (dmg_poison ... dmg_shock) */
const int giDmgFlags[NUM_DMG_TYPES] = {
	DMG_POISON, DMG_ACID, DMG_FREEZE | DMG_SLOWFREEZE, DMG_DROWN, DMG_BURN | DMG_SLOWBURN,
	DMG_NERVEGAS, DMG_RADIATION, DMG_SHOCK, (int)(1u << 30) /* DMG_CALTROP */,
	DMG_MORTAR /* DMG_TRANQ */, DMG_SONIC /* DMG_CONCUSS */, (int)(1u << 31) /* DMG_HALLUC */
};

static int dmg_width = 32, dmg_height = 32;	/* giDmgWidth / Height (dmg_bio rect) */

void hud_dmg_tile_size(int w, int h)
{
	dmg_width = w;
	dmg_height = h;
}

/* UpdateTiles */
static void UpdateTiles(float flTime, int bitsDamage)
{
	int bitsOn = ~hud.bitsDamage & bitsDamage, i, j;

	for (i = 0; i < NUM_DMG_TYPES; i++)
	{
		hud_dmg_t *pdmg = &hud.dmg[i];

		/* is this one already on? */
		if (hud.bitsDamage & giDmgFlags[i])
		{
			pdmg->fExpire = flTime + DMG_IMAGE_LIFE;	/* extend the duration */
			if (!pdmg->fBaseline)
				pdmg->fBaseline = flTime;
		}
		/* are we just turning it on? */
		if (bitsOn & giDmgFlags[i])
		{
			/* put this one at the bottom */
			pdmg->x = dmg_width / 8;
			pdmg->y = 240 - dmg_height * 2;
			pdmg->fExpire = flTime + DMG_IMAGE_LIFE;
			/* move everyone else up */
			for (j = 0; j < NUM_DMG_TYPES; j++)
			{
				if (j == i)
					continue;
				if (hud.dmg[j].y)
					hud.dmg[j].y -= dmg_height;
			}
		}
	}
	/* damage bits are only turned on here; they are turned off when the
	 * draw time has expired (in DrawDamage) */
	hud.bitsDamage |= bitsDamage;
}

/* CalcDamageDirection */
static void CalcDamageDirection(const float from[3])
{
	const ent_player_t *pl = phy_player();
	float v[3], forward[3], right[3], up[3], side, front, dist;

	if (!from[0] && !from[1] && !from[2])
	{
		hud.fAttackFront = hud.fAttackRear = hud.fAttackRight = hud.fAttackLeft = 0;
		return;
	}
	VecSub(from, pl->origin, v);
	dist = VecLen(v);
	VecNorm(v, v);
	AngleVectors(pl->view_angles, forward, right, up);
	front = VecDot(v, right);
	side = VecDot(v, forward);
	if (dist <= 50)
	{
		hud.fAttackFront = hud.fAttackRear = hud.fAttackRight = hud.fAttackLeft = 1;
		return;
	}
	if (side > 0)
	{
		if (side > 0.3f)
			hud.fAttackFront = fmaxf(hud.fAttackFront, side);
	}
	else if (fabsf(side) > 0.3f)
		hud.fAttackRear = fmaxf(hud.fAttackRear, fabsf(side));
	if (front > 0)
	{
		if (front > 0.3f)
			hud.fAttackRight = fmaxf(hud.fAttackRight, front);
	}
	else if (fabsf(front) > 0.3f)
		hud.fAttackLeft = fmaxf(hud.fAttackLeft, fabsf(front));
}

void hud_msg_damage(int armor, int damage, int bits, const float from[3])
{
	UpdateTiles((float)hud.time, bits);
	/* actually took damage? */
	if (damage > 0 || armor > 0)
		CalcDamageDirection(from);
}
