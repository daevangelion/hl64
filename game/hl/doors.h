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
 * doors.h -- HLSDK dlls/doors.h spawnflags, cbase.h locksound_t, the door
 * and button private blocks and the shared lock-sound helpers (doors.c).
 */
#ifndef HL64_GAME_DOORS_H
#define HL64_GAME_DOORS_H

#include "game/hl/util.h"

#define SF_DOOR_ROTATE_Y		0
#define SF_DOOR_START_OPEN		1
#define SF_DOOR_ROTATE_BACKWARDS	2
#define SF_DOOR_PASSABLE		8
#define SF_DOOR_ONEWAY			16
#define SF_DOOR_NO_AUTO_RETURN		32
#define SF_DOOR_ROTATE_Z		64
#define SF_DOOR_ROTATE_X		128
#define SF_DOOR_USE_ONLY		256
#define SF_DOOR_NOMONSTERS		512
#define SF_DOOR_SILENT			0x80000000u

/* sounds that doors and buttons make when locked / unlocked */
typedef struct {
	res_t		sLockedSound;
	res_t		sLockedSentence;	/* GROUP res */
	res_t		sUnlockedSound;
	res_t		sUnlockedSentence;
	int16_t		iLockedSentence;
	int16_t		iUnlockedSentence;
	float		flwaitSound;
	float		flwaitSentence;
	uint8_t		bEOFLocked;
	uint8_t		bEOFUnlocked;
	uint8_t		pad[2];
} locksound_t;

/* CBaseDoor (func_door, func_water, func_door_rotating, momentary_door) */
typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	locksound_t	m_ls;
	uint8_t		m_bHealthValue;
	uint8_t		m_bMoveSnd;
	uint8_t		m_bStopSnd;
	uint8_t		m_bLockedSound;
	uint8_t		m_bLockedSentence;
	uint8_t		m_bUnlockedSound;
	uint8_t		m_bUnlockedSentence;
	uint8_t		pad;
} CBaseDoor;

res_t		ButtonSound(int sound);
void		LockSoundsPrecache(locksound_t *ls, int locked_sound, int unlocked_sound,
			int locked_sentence, int unlocked_sentence);
void		PlayLockSounds(int e, locksound_t *pls, int flocked, int fbutton);
int		SENTENCEG_PlaySequential(int e, res_t group, float vol, float attn, int flags,
			int pitch, int ipick, int freset);
/* m_vecPosition2 of a sliding door / button: origin + movedir * travel */
void		ToggleTravel(int e, float lip, float out[3]);
int		DoorActivate(int e);
/* DoSpark(pev, location): e < 0 plays the sound at the point */
void		DoSpark(int e, const float location[3], const float size[3]);
void		DoSparkPrecache(void);

#endif
