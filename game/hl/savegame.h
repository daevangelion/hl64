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
 * savegame.h -- the game's saved-game sections (game/hl/savegame.c,
 * architecture.md 14). Every module whose file-scope state the simulation
 * (or the first frames' picture) reads moves it through one function:
 *   SG_PUT   write it (engine/save/sv.h writer);
 *   SG_GET   read it back into the module, checking what would index out
 *            of range (sv_fail with the reason);
 *   SG_SIZE  touch nothing: the bytes of the types it moves.
 * Each returns those bytes (game_save_layout hashes them); variable-length
 * state counts one element of each kind. Times stay absolute (the clock is
 * restored exactly); ent_h, name_t, gid_t16, res_t and entity slots stay
 * valid as they are.
 */
#ifndef HL64_GAME_SAVEGAME_H
#define HL64_GAME_SAVEGAME_H

#include <stdint.h>

#include "engine/save/sv.h"

enum { SG_PUT, SG_GET, SG_SIZE };

/* n bytes at p through the stream for SG_PUT / SG_GET (from 16 bytes a
 * delta row against zeros: mostly-zero state costs its mask); returns n */
static inline uint32_t sg_io(int op, void *p, uint32_t n)
{
	if (op == SG_PUT)
	{
		if (n >= 16)
			sv_put_delta(p, NULL, n);
		else
			sv_put(p, n);
	}
	else if (op == SG_GET)
	{
		if (n >= 16)
			sv_get_delta(p, NULL, n, NULL);
		else
			sv_get(p, n);
	}
	return n;
}

/* game/hl */
uint32_t	SoundEnt_SaveState(int op);	/* aisound.c: the world's sound list */
uint32_t	FireBullets_SaveState(int op);	/* aicombat.c: the monsters' tracer count */
uint32_t	Grunt_SaveState(int op);	/* grunt.c: g_fGruntQuestion */
uint32_t	Sv_SpawnSumState(int op);	/* levelstate.c: pristine forms of live slots */
uint32_t	MM_SaveState(int op);		/* logic.c: multi_manager thread clones */
uint32_t	Graph_SaveState(int op);	/* nodes.c: CGraph caches, lastCoverSearch */
uint32_t	Player_SaveState(int op);	/* player.c: CBasePlayer, geiger */
uint32_t	SENTENCEG_SaveState(int op);	/* sentences.c: sentence group LRUs */
uint32_t	Tlk_SaveState(int op);		/* talkmonster.c: g_talkWaitTime, friends */
uint32_t	EndSection_SaveState(int op);	/* triggers.c: trigger_endsection fires */
uint32_t	Util_SaveState(int op);		/* util.c: gpGlobals, multidamage, globals */
uint32_t	W_SaveState(int op);		/* weapons.c: autoaim, tracers, skill data */
uint32_t	W_CrowbarSaveState(int op);	/* weapon_crowbar.c: the swing counter */
uint32_t	World_SaveState(int op);	/* world.c: the pending chapter title */
/* game/hud */
uint32_t	View_SaveState(int op);		/* view.c: bob and step smoothing */
/* game/pm */
uint32_t	Pm_HostSaveState(int op);	/* pm_host.c: playermove fields that persist */
uint32_t	Pm_SharedSaveState(int op);	/* pm_shared.c: stuck offsets, step skip */
/* game/studio */
uint32_t	Gs_PoseSaveState(int op);	/* pose.c: the server pose memo */
/* pose.c, at the end of a load: the client pose latch of every live
 * entity as if its current state had been drawn (no interpolation from a
 * state before the load, no client events replayed) */
void		Gs_LatchLoadDone(void);

/* aithink.c: game_save_settle, game_save_pending */
void		Ai_Settle(void);
int		Ai_Pending(void);

/* the fx beams a private block names, made again after a load (the engine
 * starts the effects empty): garg.c, islave.c, tripmine.c, env.c (env_beam,
 * env_laser and its end sprite) */
void		CGargantua_LoadDone(int e);
void		CISlave_LoadDone(int e);
void		CTripmineGrenade_LoadDone(int e);
void		CLightning_LoadDone(int e);
void		CLaser_LoadDone(int e);

#endif
