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
 * hud.h -- the client HUD state (HLSDK cl_dll CHud and its elements:
 * health / pain / damage tiles, battery, ammo and pickup history,
 * flashlight, geiger, weapon selection) shared by game/hud's files.
 */
#ifndef HL64_GAME_HUD_H
#define HL64_GAME_HUD_H

#include <stdint.h>

#define HUD_HISTORY		8	/* MAX_HISTORY 12; 320x240 shows fewer rows */
#define HISTORY_DRAW_TIME	5.0f	/* hud_drawhistory_time default */
#define HUD_SELECT_TIME		1.5f	/* weapon selection display after a switch */
#define NUM_DMG_TYPES		12
#define DMG_IMAGE_LIFE		2.0f

typedef struct {
	uint8_t		type, id;	/* HIST_*, ammo / weapon / item id */
	int16_t		count;
	float		time;		/* DisplayTime */
} hud_hist_t;

typedef struct {
	float		fExpire, fBaseline;
	int		x, y;
} hud_dmg_t;

typedef struct {
	double		time, frametime;	/* m_flTime, m_flTimeDelta */
	/* health (CHudHealth) */
	int		health;
	float		health_fade;
	float		fAttackFront, fAttackRear, fAttackLeft, fAttackRight;
	int		bitsDamage;
	hud_dmg_t	dmg[NUM_DMG_TYPES];
	/* battery (CHudBattery) */
	int		battery;
	float		battery_fade;
	/* ammo (CHudAmmo) */
	int		ammo_cur_id, ammo_cur_clip;
	float		ammo_fade;
	/* pickup history */
	hud_hist_t	hist[HUD_HISTORY];
	int		nhist;
	/* flashlight */
	uint8_t		flash_on, flash_bat;
	uint8_t		select_id, pad;
	float		select_time;
	/* geiger */
	int		geiger_range;
	uint32_t	rand;			/* the client's rand() */
} hud_state_t;
extern hud_state_t	hud;

/* CHudHealth::MsgFunc_Damage */
void	hud_msg_damage(int armor, int damage, int bits, const float from[3]);

/* The view model (V_CalcNormalRefdef's view entity) for this frame. */
void	hud_view_model(const float org[3], const float ang[3]);

#endif
