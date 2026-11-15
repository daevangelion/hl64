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
 * aisound.c -- the world's AI sound list (HLSDK dlls/soundent.cpp CSoundEnt):
 * what monsters hear and smell. HLSDK keeps it in a "soundent" entity
 * thinking every 0.3 s; here it is a static pool expired from the game's
 * frame hook on the same 0.3 s schedule. Slot 0 is the client's sound
 * (CBasePlayer::UpdatePlayerSound writes it through
 * CSoundEnt_SoundPointerForIndex(CSoundEnt_ClientSoundIndex(1))).
 */
#include <string.h>

#include "game/hl/monster.h"
#include "game/hl/savegame.h"

static CSound	pool[MAX_WORLD_SOUNDS];
static int	freeSound, activeSound;
static float	nextThink;
static int	ready;

static void sound_clear(CSound *s)
{
	VecSet(s->m_vecOrigin, 0, 0, 0);
	s->m_iType = 0;
	s->m_iVolume = 0;
	s->m_flExpireTime = 0;
	s->m_iNext = SOUNDLIST_EMPTY;
	s->m_iNextAudible = 0;
	s->m_iLayer = -1;
}

static int IAllocSound(void)
{
	int n;

	if (freeSound == SOUNDLIST_EMPTY)
		return SOUNDLIST_EMPTY;
	n = freeSound;
	freeSound = pool[freeSound].m_iNext;
	pool[n].m_iNext = activeSound;
	activeSound = n;
	return n;
}

static void FreeSound(int i, int prev)
{
	if (prev != SOUNDLIST_EMPTY)
		pool[prev].m_iNext = pool[i].m_iNext;
	else
		activeSound = pool[i].m_iNext;
	pool[i].m_iNext = freeSound;
	freeSound = i;
}

void CSoundEnt_Init(void)
{
	int i, s;

	freeSound = 0;
	activeSound = SOUNDLIST_EMPTY;
	for (i = 0; i < MAX_WORLD_SOUNDS; i++)
	{
		sound_clear(&pool[i]);
		pool[i].m_iNext = i + 1;
	}
	pool[MAX_WORLD_SOUNDS - 1].m_iNext = SOUNDLIST_EMPTY;
	/* gpGlobals->maxClients = 1: the client's own slot */
	s = IAllocSound();
	if (s != SOUNDLIST_EMPTY)
		pool[s].m_flExpireTime = SOUND_NEVER_EXPIRE;
	nextThink = gtime() + 1;
	ready = 1;
}

void CSoundEnt_Think(void)
{
	int i, prev, next;

	if (!ready || gtime() < nextThink)
		return;
	nextThink = gtime() + 0.3f;
	prev = SOUNDLIST_EMPTY;
	i = activeSound;
	while (i != SOUNDLIST_EMPTY)
	{
		if (pool[i].m_flExpireTime <= gtime() && pool[i].m_flExpireTime != SOUND_NEVER_EXPIRE)
		{
			next = pool[i].m_iNext;
			FreeSound(i, prev);
			i = next;
		}
		else
		{
			prev = i;
			i = pool[i].m_iNext;
		}
	}
}

void CSoundEnt_InsertSound(int iType, const float org[3], int iVolume, float flDuration)
{
	int i;

	if (!ready)
		return;
	i = IAllocSound();
	if (i == SOUNDLIST_EMPTY)
		return;
	/* org is in the caller's frame (the dispatched entity's layer) */
	VecCopy(org, pool[i].m_vecOrigin);
	pool[i].m_iLayer = eng_frame_layer();
	pool[i].m_iType = iType;
	pool[i].m_iVolume = iVolume;
	pool[i].m_flExpireTime = gtime() + flDuration;
}

int CSoundEnt_ActiveList(void) { return ready ? activeSound : SOUNDLIST_EMPTY; }

CSound *CSoundEnt_SoundPointerForIndex(int i)
{
	if (!ready || i < 0 || i > MAX_WORLD_SOUNDS - 1)
		return 0;
	return &pool[i];
}

int CSoundEnt_ClientSoundIndex(int e) { return e - 1; }

/* ---- saved games (game/hl/savegame.h) -------------------------------------- */

static int sound_index_ok(int i)
{
	return i >= SOUNDLIST_EMPTY && i < MAX_WORLD_SOUNDS;
}

/* The whole pool: a monster's m_iAudibleList may still walk the
 * m_iNextAudible chain of sounds freed since (as in HLSDK). */
uint32_t SoundEnt_SaveState(int op)
{
	uint32_t n = 0;
	int i, bad;

	n += sg_io(op, pool, sizeof(pool));
	n += sg_io(op, &freeSound, sizeof(freeSound));
	n += sg_io(op, &activeSound, sizeof(activeSound));
	n += sg_io(op, &nextThink, sizeof(nextThink));
	n += sg_io(op, &ready, sizeof(ready));
	if (op == SG_GET)
	{
		bad = !sound_index_ok(freeSound) || !sound_index_ok(activeSound) ||
			(ready != 0 && ready != 1);
		for (i = 0; i < MAX_WORLD_SOUNDS; i++)
			bad |= !sound_index_ok(pool[i].m_iNext) ||
				!sound_index_ok(pool[i].m_iNextAudible);
		if (bad)
		{
			sv_fail("game: sound list index out of range");
			CSoundEnt_Init();
		}
	}
	return n;
}
