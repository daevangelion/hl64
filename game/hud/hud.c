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
#include <math.h>
#include <string.h>

#include "engine/core/hlmath.h"
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
static const int giDmgFlags[NUM_DMG_TYPES] = {
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

/* ---- drawing (hud_redraw.cpp, health.cpp, battery.cpp, ammo.cpp,
 * ammohistory.cpp, flashlight.cpp, geiger.cpp, message.cpp) ----------------------- */

#include "hl64/draw2d.h"
#include "hl64/fx.h"
#include "hl64/rec.h"
#include "game/hud/hud_rects.h"

#define MIN_ALPHA	100
#define FADE_TIME	100
#define DHN_DRAWZERO	1
#define DHN_2DIGITS	2
#define DHN_3DIGITS	4
#define SCREEN_W	320
#define SCREEN_H	240

static const uint8_t RGB_YELLOWISH[3] = { 255, 160, 0 };
static const uint8_t RGB_REDISH[3] = { 255, 16, 16 };

/* weapon_*.txt 320 rectangles (the player's data; sheet, x, y, w, h):
 * inactive / selected weapon, ammo, ammo2, crosshair */
typedef struct { int8_t s; uint8_t x, y, w, h; } rc_t;
typedef struct { rc_t weapon, weapon_s, ammo, ammo2, cross; } wrects_t;
static const wrects_t wrects[WPN_SLOTS] = {
	[WEAPON_CROWBAR] = { { 0, 0, 0, 80, 20 }, { 0, 0, 20, 80, 20 }, { -1 }, { -1 }, { -1 } },
	[WEAPON_GLOCK] = { { 0, 0, 40, 80, 20 }, { 0, 0, 60, 80, 20 }, { 1, 0, 16, 18, 18 }, { -1 },
		{ 2, 24, 0, 24, 24 } },
	[WEAPON_PYTHON] = { { 0, 0, 80, 80, 20 }, { 0, 0, 100, 80, 20 }, { 1, 18, 16, 18, 18 }, { -1 },
		{ 2, 48, 0, 24, 24 } },
	[WEAPON_MP5] = { { 0, 0, 120, 80, 20 }, { 0, 0, 140, 80, 20 }, { 1, 0, 16, 18, 18 },
		{ 1, 36, 16, 18, 18 }, { 2, 0, 48, 24, 24 } },
	[WEAPON_SHOTGUN] = { { 0, 0, 160, 80, 20 }, { 0, 0, 180, 80, 20 }, { 1, 54, 16, 18, 18 }, { -1 },
		{ 2, 48, 24, 24, 24 } },
	[WEAPON_HANDGRENADE] = { { 0, 160, 0, 80, 20 }, { 0, 160, 20, 80, 20 },
		{ 1, 36, 34, 18, 18 }, { -1 }, { -1 } },
	[WEAPON_TRIPMINE] = { { 0, 160, 80, 80, 20 }, { 0, 160, 100, 80, 20 },
		{ 1, 72, 34, 18, 18 }, { -1 }, { -1 } },
};
/* ammo type -> the weapon whose ammo icon it is (GetAmmoPicFromWeapon) */
static const uint8_t ammo_weapon[MAX_AMMO_TYPES] = {
	0, WEAPON_GLOCK, WEAPON_PYTHON, WEAPON_SHOTGUN, 0, WEAPON_HANDGRENADE, WEAPON_TRIPMINE
};

static void scale(const uint8_t c[3], int a, uint8_t out[3])
{
	int i;

	for (i = 0; i < 3; i++)
		out[i] = (uint8_t)(c[i] * a / 255);
}

static void spr(int hr, int x, int y, const uint8_t c[3])
{
	const hl_rect_t *r = &hl_rects[hr];

	d2_pic(r->sheet, r->x, r->y, r->w, r->h, x, y, c[0], c[1], c[2], 255, D2_ADD);
}

static void sprc(const rc_t *r, int x, int y, const uint8_t c[3])
{
	if (r->s >= 0 && r->w)
		d2_pic(r->s, r->x, r->y, r->w, r->h, x, y, c[0], c[1], c[2], 255, D2_ADD);
}

/* CHud::DrawHudNumber */
static int DrawHudNumber(int x, int y, int iFlags, int iNumber, const uint8_t c[3])
{
	int iWidth = hl_rects[HR_number_0].w, k;

	if (iNumber > 0)
	{
		if (iNumber >= 100)
		{
			k = iNumber / 100;
			spr(HR_number_0 + (k > 9 ? 9 : k), x, y, c);
			x += iWidth;
		}
		else if (iFlags & DHN_3DIGITS)
			x += iWidth;
		if (iNumber >= 10)
		{
			spr(HR_number_0 + (iNumber % 100) / 10, x, y, c);
			x += iWidth;
		}
		else if (iFlags & (DHN_3DIGITS | DHN_2DIGITS))
			x += iWidth;
		spr(HR_number_0 + iNumber % 10, x, y, c);
		x += iWidth;
	}
	else if (iFlags & DHN_DRAWZERO)
	{
		if (iFlags & DHN_3DIGITS)
			x += iWidth;
		if (iFlags & (DHN_3DIGITS | DHN_2DIGITS))
			x += iWidth;
		spr(HR_number_0, x, y, c);
		x += iWidth;
	}
	return x;
}

/* DrawHudNumberString: right-aligned digits ending at xpos */
static void DrawHudNumberString(int xpos, int y, int num, const uint8_t c[3])
{
	char s[8];
	int n = 0, i;

	do
		s[n++] = (char)('0' + num % 10);
	while ((num /= 10) && n < 7);
	for (i = 0; i < n; i++)
	{
		xpos -= hl_rects[HR_number_0].w;
		spr(HR_number_0 + s[i] - '0', xpos, y, c);
	}
}

static int font_h(void) { return hl_rects[HR_number_0].h; }

static void draw_health(const CBasePlayer *p)
{
	uint8_t c[3];
	int a, x, y, hw = hl_rects[HR_number_0].w, cw = hl_rects[HR_cross].w;

	if (hud.health_fade)
	{
		hud.health_fade -= (float)(hud.frametime * 20);
		if (hud.health_fade <= 0)
			hud.health_fade = 0;
		a = MIN_ALPHA + (int)(hud.health_fade / FADE_TIME * 128);
	}
	else
		a = MIN_ALPHA;
	if (hud.health <= 15)
		a = 255;
	/* GetPainColor */
	scale(hud.health > 25 ? RGB_YELLOWISH : (const uint8_t[3]){ 250, 0, 0 }, a, c);
	if (p->weapons & (1u << WEAPON_SUIT))
	{
		y = SCREEN_H - font_h() - font_h() / 2;
		x = cw / 2;
		spr(HR_cross, x, y, c);
		x = cw + hw / 2;
		y += (int)(font_h() * 0.2f);
		x = DrawHudNumber(x, y, DHN_3DIGITS | DHN_DRAWZERO, hud.health, c);
		x += hw / 2;
		d2_fill(x, y, hw / 10 > 0 ? hw / 10 : 1, font_h(), 255, 160, 0, (uint8_t)a, D2_ADD);
	}
}

static void draw_damage(void)
{
	uint8_t c[3];
	int a, i, j;

	if (!hud.bitsDamage)
		return;
	a = (int)(fabs(hl_sin(hud.time * 2)) * 256.0);
	if (a > 255)
		a = 255;
	scale(RGB_YELLOWISH, a, c);
	for (i = 0; i < NUM_DMG_TYPES; i++)
		if ((hud.bitsDamage & giDmgFlags[i]) && HR_dmg_bio + 1 + i < HR_number_0)
			spr(HR_dmg_bio + 1 + i, hud.dmg[i].x, hud.dmg[i].y, c);
	/* check for bits that should be expired */
	for (i = 0; i < NUM_DMG_TYPES; i++)
	{
		hud_dmg_t *d = &hud.dmg[i];

		if (!(hud.bitsDamage & giDmgFlags[i]))
			continue;
		d->fExpire = fminf((float)hud.time + DMG_IMAGE_LIFE, d->fExpire);
		if (d->fExpire <= hud.time && a < 40)
		{
			int y = d->y;

			d->fExpire = 0;
			d->x = d->y = 0;
			/* move everyone above down */
			for (j = 0; j < NUM_DMG_TYPES; j++)
				if (hud.dmg[j].y && hud.dmg[j].y < y)
					hud.dmg[j].y += hl_rects[HR_dmg_bio].w;
			hud.bitsDamage &= ~giDmgFlags[i];
		}
	}
}

static void pain_one(float *f, int hr, int x, int y, float fade)
{
	uint8_t c[3];

	if (*f > 0.4f)
	{
		scale(hud.health > 25 ? RGB_YELLOWISH : (const uint8_t[3]){ 250, 0, 0 },
			(int)(255 * fmaxf(*f, 0.5f)), c);
		spr(hr, x, y, c);
		*f = fmaxf(0, *f - fade);
	}
	else
		*f = 0;
}

static void draw_pain(void)
{
	float fade = (float)hud.frametime * 2;
	const hl_rect_t *u = &hl_rects[HR_pain_up], *r = &hl_rects[HR_pain_right],
		*d = &hl_rects[HR_pain_down], *l = &hl_rects[HR_pain_left];

	pain_one(&hud.fAttackFront, HR_pain_up, SCREEN_W / 2 - u->w / 2, SCREEN_H / 2 - u->h * 3, fade);
	pain_one(&hud.fAttackRight, HR_pain_right, SCREEN_W / 2 + r->w * 2, SCREEN_H / 2 - r->h / 2,
		fade);
	pain_one(&hud.fAttackRear, HR_pain_down, SCREEN_W / 2 - d->w / 2, SCREEN_H / 2 + d->h * 2, fade);
	pain_one(&hud.fAttackLeft, HR_pain_left, SCREEN_W / 2 - l->w * 3, SCREEN_H / 2 - l->h / 2, fade);
}

static void draw_battery(const CBasePlayer *p)
{
	const hl_rect_t *e = &hl_rects[HR_suit_empty], *f = &hl_rects[HR_suit_full];
	uint8_t c[3];
	int a, x, y, off = e->h / 6, cut;

	if (!(p->weapons & (1u << WEAPON_SUIT)))
		return;
	if (hud.battery_fade)
	{
		if (hud.battery_fade > FADE_TIME)
			hud.battery_fade = FADE_TIME;
		hud.battery_fade -= (float)(hud.frametime * 20);
		if (hud.battery_fade <= 0)
			hud.battery_fade = 0;
		a = MIN_ALPHA + (int)(hud.battery_fade / FADE_TIME * 128);
	}
	else
		a = MIN_ALPHA;
	scale(RGB_YELLOWISH, a, c);
	y = SCREEN_H - font_h() - font_h() / 2;
	x = SCREEN_W / 5;
	spr(HR_suit_empty, x, y - off, c);
	cut = (int)(f->h * ((100 - (hud.battery < 100 ? hud.battery : 100)) * 0.01f));
	if (f->h - cut > 0)
		d2_pic(f->sheet, f->x, f->y + cut, f->w, f->h - cut, x, y - off + cut, c[0], c[1], c[2],
			255, D2_ADD);
	x += e->w;
	DrawHudNumber(x, y, DHN_3DIGITS | DHN_DRAWZERO, hud.battery, c);
}

static void draw_ammo(const CBasePlayer *p)
{
	const ItemInfo *ii = ItemInfoOf(p->m_pActiveItem);
	const wrects_t *wr = &wrects[p->m_pActiveItem];
	uint8_t c[3];
	int a, x, y, aw = hl_rects[HR_number_0].w, iw, off;

	/* CHudAmmo::Draw: HIDEHUD_WEAPONS (a tank) hides it */
	if (!ii || !Player_IsAlive() || (p->m_iHideHUD & HIDEHUD_WEAPONS))
		return;
	a = (int)fmaxf(MIN_ALPHA, hud.ammo_fade);
	if (hud.ammo_fade > 0)
		hud.ammo_fade -= (float)(hud.frametime * 20);
	scale(RGB_YELLOWISH, a, c);
	y = SCREEN_H - font_h() - font_h() / 2;
	if (ii->iAmmo1 > 0)
	{
		iw = wr->ammo.w;
		if (p->wpn[p->m_pActiveItem].m_iClip >= 0)
		{
			/* room for the number and the '|' and the current ammo */
			x = SCREEN_W - 8 * aw - iw;
			x = DrawHudNumber(x, y, DHN_DRAWZERO | DHN_3DIGITS,
				p->wpn[p->m_pActiveItem].m_iClip, c);
			x += aw / 2;
			d2_fill(x, y, aw / 10 > 0 ? aw / 10 : 1, font_h(), 255, 160, 0, (uint8_t)a, D2_ADD);
			x += (aw / 10) + aw / 2;
			x = DrawHudNumber(x, y, DHN_DRAWZERO | DHN_3DIGITS, p->m_rgAmmo[(int)ii->iAmmo1], c);
		}
		else
		{
			/* a bullets only line */
			x = SCREEN_W - 4 * aw - iw;
			x = DrawHudNumber(x, y, DHN_DRAWZERO | DHN_3DIGITS, p->m_rgAmmo[(int)ii->iAmmo1], c);
		}
		off = wr->ammo.h / 8;
		sprc(&wr->ammo, x, y - off, c);
	}
	/* does weapon have secondary ammo? */
	if (ii->iAmmo2 > 0 && p->m_rgAmmo[(int)ii->iAmmo2] > 0)
	{
		iw = wr->ammo2.w;
		y -= font_h() + font_h() / 4;
		x = SCREEN_W - 4 * aw - iw;
		x = DrawHudNumber(x, y, DHN_DRAWZERO | DHN_3DIGITS, p->m_rgAmmo[(int)ii->iAmmo2], c);
		off = wr->ammo2.h / 8;
		sprc(&wr->ammo2, x, y - off, c);
	}
}

static void draw_crosshair(const CBasePlayer *p)
{
	const rc_t *r = &wrects[p->m_pActiveItem].cross;
	uint8_t c[3];

	/* MsgFunc_HideWeapon: HIDEHUD_WEAPONS (a tank) clears the crosshair */
	if (!p->m_pActiveItem || r->s < 0 || !r->w || !Player_IsAlive() ||
		(p->m_iHideHUD & HIDEHUD_WEAPONS))
		return;
	/* the engine's crosshair, drawn in the HUD colour */
	scale(RGB_YELLOWISH, 255, c);
	sprc(r, SCREEN_W / 2 - r->w / 2, SCREEN_H / 2 - r->h / 2, c);
}

static void draw_flashlight(const CBasePlayer *p)
{
	const hl_rect_t *e = &hl_rects[HR_flash_empty], *f = &hl_rects[HR_flash_full];
	uint8_t c[3];
	int a, x, y, w = f->w, off;
	float bat = hud.flash_bat / 100.0f;

	if (!(p->weapons & (1u << WEAPON_SUIT)))
		return;
	a = hud.flash_on ? 225 : MIN_ALPHA;
	scale(bat < 0.20f ? RGB_REDISH : RGB_YELLOWISH, a, c);
	y = (e->h) / 2;
	x = SCREEN_W - w - w / 2;
	spr(HR_flash_empty, x, y, c);
	if (hud.flash_on)
		spr(HR_flash_beam, SCREEN_W - w / 2, y, c);
	off = (int)(w * (1.0f - bat));
	if (off < w)
		d2_pic(f->sheet, f->x + off, f->y, f->w - off, f->h, x + off, y, c[0], c[1], c[2], 255,
			D2_ADD);
}

static void draw_history(const CBasePlayer *p)
{
	int i, gap = 20 + 5, pick = 32 + 20 * 2;

	for (i = 0; i < hud.nhist; i++)
	{
		hud_hist_t *h = &hud.hist[i];
		uint8_t c[3];
		int ypos = SCREEN_H - (pick + gap * i) / 2, sc;

		h->time = fminf(h->time, (float)hud.time + HISTORY_DRAW_TIME);
		if (h->time <= hud.time)
		{
			memmove(&hud.hist[i], &hud.hist[i + 1], (size_t)(hud.nhist - i - 1) * sizeof(*h));
			hud.nhist--;
			i--;
			continue;
		}
		sc = (int)((h->time - hud.time) * 80);
		if (sc > 255)
			sc = 255;
		if (h->type == HIST_AMMO && h->id < MAX_AMMO_TYPES && ammo_weapon[h->id])
		{
			const rc_t *r = &wrects[ammo_weapon[h->id]].ammo;
			int xpos = SCREEN_W - 24;

			scale(RGB_YELLOWISH, sc, c);
			sprc(r, xpos, ypos, c);
			DrawHudNumberString(xpos - 10, ypos, h->count, c);
		}
		else if (h->type == HIST_WEAP && h->id < WPN_SLOTS)
		{
			const rc_t *r = &wrects[h->id].weapon;

			scale(W_CanDeploy(h->id) ? RGB_YELLOWISH : RGB_REDISH, sc, c);
			sprc(r, SCREEN_W - r->w, ypos, c);
		}
		else if (h->type == HIST_ITEM)
		{
			static const int item_rect[4] = { -1, HR_item_healthkit, HR_item_battery,
				HR_item_longjump };
			int hr = h->id < 4 ? item_rect[h->id] : -1;

			if (hr < 0)
				continue;
			scale(RGB_YELLOWISH, sc, c);
			spr(hr, SCREEN_W - hl_rects[hr].w - 10, ypos, c);
		}
	}
	(void)p;
}

/* the weapon list after a switch (ammo.cpp DrawWList, fast switch) */
static void draw_selection(const CBasePlayer *p)
{
	const ItemInfo *act = ItemInfoOf(p->m_pActiveItem);
	uint8_t c[3];
	int slot, x = 10, y, id;

	/* CHudAmmo::DrawWList: HIDEHUD_WEAPONS (a tank) hides it */
	if (!act || hud.time > hud.select_time || (p->m_iHideHUD & HIDEHUD_WEAPONS))
		return;
	scale(RGB_YELLOWISH, 255, c);
	for (slot = 0; slot < 5; slot++)
	{
		int has = 0, bw = hl_rects[HR_bucket1].w;

		for (id = 1; id < WPN_SLOTS; id++)
			if (ItemInfoOf(id) && ItemInfoOf(id)->iSlot == slot && Player_HasWeapon(id))
				has = 1;
		if (!has)
			continue;
		y = 10;
		spr(slot == 0 ? HR_bucket1 : HR_bucket1 + slot, x, y, c);
		if (slot == act->iSlot)
		{
			y += hl_rects[HR_bucket1].h + 2;
			for (id = 1; id < WPN_SLOTS; id++)
			{
				const ItemInfo *ii = ItemInfoOf(id);

				if (!ii || ii->iSlot != slot || !Player_HasWeapon(id))
					continue;
				if (id == p->m_pActiveItem)
				{
					sprc(&wrects[id].weapon_s, x, y, c);
					spr(HR_selection, x, y, c);
				}
				else
					sprc(&wrects[id].weapon, x, y, c);
				y += wrects[id].weapon.h + 2;
			}
			bw = wrects[p->m_pActiveItem].weapon.w;
		}
		x += bw + 5;
	}
}

/* CHudMessage::XPosition (cl_dll/message.cpp): -1 centres the line,
 * other negatives align the block's right edge, then clamp on screen */
static int title_x(float x, int width, int total)
{
	int pos;

	if (x == -1)
		pos = (SCREEN_W - width) / 2;
	else if (x < 0)
		pos = (int)((1.0f + x) * SCREEN_W - total);
	else
		pos = (int)(x * SCREEN_W);
	if (pos + width > SCREEN_W)
		pos = SCREEN_W - width;
	else if (pos < 0)
		pos = 0;
	return pos;
}

/* CHudMessage::YPosition: -1 centres, other negatives align the bottom */
static int title_y(float y, int height)
{
	int pos;

	if (y == -1)
		pos = (int)((SCREEN_H - height) * 0.5f);
	else if (y < 0)
		pos = (int)((1.0f + y) * SCREEN_H - height);
	else
		pos = (int)(y * SCREEN_H);
	if (pos + height > SCREEN_H)
		pos = SCREEN_H - height;
	else if (pos < 0)
		pos = 0;
	return pos;
}

/* gHUD.m_scrinfo.charWidths[c] of the text font */
static int title_char_w(unsigned char c)
{
	char s[2] = { (char)c, 0 };

	return d2_text(-1000, -1000, s, 0, 0, 0, 0);	/* off screen: measured, not drawn */
}

/* client text messages: CHudMessage::MessageDrawScan with MessageScanStart
 * and MessageScanNextChar for the current title (titles.txt). Effect 1
 * fades like effect 0: its flicker redraws a character in color2, black
 * in titles.txt, which adds nothing. */
static void draw_title(void)
{
	float time, fade_time = 0, char_time = 0, dt;
	uint16_t id = fx_text_current(&time);
	const res_title_t *t;
	const char *s, *p;
	unsigned char line[80];
	uint8_t cw[80];
	int lines = 1, length = 0, width = 0, total_width = 0, lh = d2_text_height();
	int fade_blend = 0, blend, x, y, i, j, n, next, k, src[3], dst[3], c[3];

	if (id == 0xFFFF || !(t = res_title(id)) || !(s = res_title_text(id)) || !lh)
		return;
	for (p = s; *p; p++, length++)
	{
		if (*p == '\n')
		{
			lines++;
			if (width > total_width)
				total_width = width;
			width = 0;
		}
		else
			width += title_char_w((unsigned char)*p);
	}
	y = title_y(t->y, lines * lh);
	/* MessageScanStart */
	switch (t->effect)
	{
	case 0:
	case 1:
		fade_time = t->fadein + t->holdtime;
		if (time < t->fadein)
			fade_blend = (int)((t->fadein - time) * (1.0f / t->fadein) * 255);
		else if (time > fade_time)
			fade_blend = t->fadeout > 0 ? (int)((time - fade_time) / t->fadeout * 255) : 255;
		break;
	case 2:
		fade_time = t->fadein * length + t->holdtime;
		if (time > fade_time && t->fadeout > 0)
			fade_blend = (int)((time - fade_time) / t->fadeout * 255);
		break;
	}
	for (p = s, i = 0; i < lines; i++)
	{
		for (n = 0, width = 0; *p && *p != '\n' && n < (int)sizeof(line) - 1; n++, p++)
		{
			line[n] = (unsigned char)*p;
			cw[n] = (uint8_t)title_char_w(line[n]);
			width += cw[n];
		}
		if (*p)
			p++;	/* the LF */
		x = title_x(t->x, width, total_width);
		for (j = 0; j < n; j++)
		{
			next = x + cw[j];
			/* MessageScanNextChar */
			for (k = 0; k < 3; k++)
			{
				src[k] = t->color1[k];
				dst[k] = 0;
			}
			blend = 0;
			switch (t->effect)
			{
			case 0:
			case 1:
				blend = fade_blend;
				break;
			case 2:
				char_time += t->fadein;
				if (char_time > time)
				{
					src[0] = src[1] = src[2] = 0;
					break;
				}
				dt = time - char_time;
				if (time > fade_time)
					blend = fade_blend;
				else if (dt > t->fxtime || t->fxtime <= 0)
					blend = 0;
				else
				{
					for (k = 0; k < 3; k++)
						dst[k] = t->color2[k];
					blend = 255 - (int)(dt * (1.0f / t->fxtime) * 255.0f + 0.5f);
				}
				break;
			}
			blend = blend > 255 ? 255 : blend < 0 ? 0 : blend;
			for (k = 0; k < 3; k++)
				c[k] = (src[k] * (255 - blend) + dst[k] * blend) >> 8;
			/* additive: a black character adds nothing */
			if (x >= 0 && y >= 0 && next <= SCREEN_W && (c[0] | c[1] | c[2]))
			{
				char ch[2] = { (char)line[j], 0 };

				d2_text(x, y, ch, (uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], 255);
			}
			x = next;
		}
		y += lh;
	}
}

/* CHudGeiger::Draw: the clicks */
static unsigned hud_rand(void)
{
	hud.rand = hud.rand * 1103515245u + 12345u;
	return (hud.rand >> 16) & 0x7fff;
}

static void geiger(void)
{
	int r = hud.geiger_range, pct = 0, i = 2;
	float vol = 0;

	if (r >= 1000 || r <= 0)
		return;
	if (r > 800) pct = 0;
	else if (r > 600) { pct = 2; vol = 0.4f; }
	else if (r > 500) { pct = 4; vol = 0.5f; }
	else if (r > 400) { pct = 8; vol = 0.6f; i = 3; }
	else if (r > 300) { pct = 8; vol = 0.7f; i = 3; }
	else if (r > 200) { pct = 28; vol = 0.78f; i = 3; }
	else if (r > 150) { pct = 40; vol = 0.80f; i = 3; }
	else if (r > 100) { pct = 60; vol = 0.85f; i = 3; }
	else if (r > 75) { pct = 80; vol = 0.9f; i = 3; }
	else if (r > 50) { pct = 90; vol = 0.95f; }
	else { pct = 95; vol = 1.0f; }
	vol = (vol * (hud_rand() & 127)) / 255 + 0.25f;
	if ((int)(hud_rand() & 127) < pct || (int)(hud_rand() & 127) < pct)
	{
		int j = hud_rand() & 1;

		if (i > 2)
			j += hud_rand() & 1;
		EMIT_SOUND_DYN(ENT_PLAYER, CHAN_AUTO, res_def(R_PL_GEIGER1 + j), vol, ATTN_NONE, 0,
			PITCH_NORM);
	}
}

void game_hud_frame(double time, double dt)
{
	const CBaseMonster *m = PlayerMon();
	const CBasePlayer *p = Player();
	int h, b, id, clip;

	hud.time = time;
	hud.frametime = dt;
	if (!m)
		return;
	/* the state messages (Health, Battery, CurWeapon) */
	h = m->health > 0 && m->health <= 1 ? 1 : (int)(m->health < 0 ? 0 : m->health > 255 ? 255
		: m->health);
	if (h != hud.health)
	{
		hud.health_fade = FADE_TIME;
		hud.health = h;
	}
	b = (int)p->armorvalue;
	if (b != hud.battery)
	{
		hud.battery_fade = FADE_TIME;
		hud.battery = b;
	}
	id = p->m_pActiveItem;
	clip = id ? p->wpn[id].m_iClip : 0;
	if (id != hud.ammo_cur_id || clip != hud.ammo_cur_clip)
	{
		hud.ammo_fade = 200.0f;
		hud.ammo_cur_id = id;
		hud.ammo_cur_clip = clip;
	}
	geiger();
}

void game_hud_draw(void)
{
	const CBasePlayer *p = Player();
	float so[3], sa[3];
	uint8_t fade[4];

	/* the screen fade under the HUD (env_fade) */
	fx_view(so, sa, fade);
	if (fade[3])
		d2_fill(0, 0, SCREEN_W, SCREEN_H, fade[0], fade[1], fade[2], fade[3], D2_ALPHA);
	if (!PlayerMon())
		return;
	draw_health(p);
	draw_damage();
	draw_pain();
	draw_battery(p);
	draw_ammo(p);
	draw_crosshair(p);
	draw_flashlight(p);
	draw_history(p);
	draw_selection(p);
	draw_title();
}
