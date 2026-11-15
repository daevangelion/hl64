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
 * savegame.c -- the game's side of a saved game (include/hl64/game.h
 * "saved games", architecture.md 14).
 *
 * The engine restores every entity into its slot with its serial, every
 * layer into its slot, the components and private blocks byte for byte,
 * the clock and the random stream; so ent_h, name_t, gid_t16, res_t and
 * slot ints in game state stay valid. What is left to the game:
 *
 *   - settle (game_save_settle): monster thinks paused on a coroutine
 *     stack or queued (aithink.c) cannot be saved; they end now, in the
 *     order the next frame start would run them, with no budget
 *     (Ai_Settle). The saving game goes on from the settled state, the
 *     loading one starts from it.
 *   - private blocks: the one pointer a block holds is CBaseMonster
 *     m_pSchedule (rodata of the monster files), saved as a registry id
 *     (monster.h sched_list_t): (list + 1) << 8 | index in the pointer's
 *     place, 0 = NULL. Lists are append-only, so ids stay. A schedule
 *     missing from the registry fails the save (logged); an id this build
 *     lacks fails the load. A block must be the size class's of its class
 *     (priv.c slabs: 32 << k bytes).
 *   - file-scope state (game_save_globals): one section per module
 *     (savegame.h); the version and every section's sizes make
 *     game_save_layout.
 *   - game_load_done: what the picture needs: the fx beams private blocks
 *     name (the engine starts the effects empty), the client pose latches.
 */
#include <stddef.h>
#include <string.h>

#include "game/hl/monster.h"
#include "game/hl/player.h"
#include "game/hl/savegame.h"
#include "game/hud/hud.h"

/* GAME section version: a change of what or how the sections write */
#define GAME_SAVE_VERSION	1

/* ---- schedule registry ------------------------------------------------------- */

/* append-only: an id's list is its position here */
static const sched_list_t *const sched_lists[] = {
	&sched_defaultai, &sched_talkmonster, &sched_barney, &sched_garg, &sched_grunt,
	&sched_headcrab, &sched_houndeye, &sched_islave, &sched_scientist,
};
#define SCHED_LISTS	((int)(sizeof(sched_lists) / sizeof(sched_lists[0])))
#define SCHED_UNKNOWN	0xFFFFFFFFu

static uint32_t sched_id(const Schedule_t *s)
{
	int l, i;

	if (!s)
		return 0;
	for (l = 0; l < SCHED_LISTS; l++)
		for (i = 0; i < sched_lists[l]->n; i++)
			if (sched_lists[l]->s[i] == s)
				return (uint32_t)(l + 1) << 8 | (uint32_t)i;
	return SCHED_UNKNOWN;
}

/* the schedule of id (NULL for 0); -1 when this build has none */
static int sched_of(uint32_t id, const Schedule_t **out)
{
	uint32_t l = (id >> 8) - 1, i = id & 0xFF;

	*out = NULL;
	if (!id)
		return 0;
	if (id >> 16 || l >= (uint32_t)SCHED_LISTS || i >= (uint32_t)sched_lists[l]->n ||
		!sched_lists[l]->s[i])
		return -1;
	*out = sched_lists[l]->s[i];
	return 0;
}

/* ---- private blocks -------------------------------------------------------------- */

/* The slab block of cls's private block (dispatch.c spawn: the class
 * size, at least a CBaseEntity; priv.c: the size class 32 << k holding
 * it); 0 when the class makes no entity. */
static uint32_t priv_bytes(cls_t cls)
{
	const game_class_t *c = game_class(cls);
	uint32_t size, b;

	if (!c)
		return 0;
	size = c->size > sizeof(CBaseEntity) ? c->size : sizeof(CBaseEntity);
	for (b = 32; b < size; b <<= 1)
		;
	return b;
}

/* a CBaseMonster block: mon_is_monster's classes (the player's too) */
static int priv_check(int e, uint32_t size, int *monster)
{
	cls_t cls = ent_idc(e)->cls;
	const game_class_t *c = game_class(cls);

	if (!c || size != priv_bytes(cls))
	{
		log_printf("savegame: %s (%d): private block of %lu B, the class's is %lu B\n",
			game_class_name(cls), e, (unsigned long)size, (unsigned long)priv_bytes(cls));
		return -1;
	}
	*monster = c->ext != NULL;
	return 0;
}

int game_priv_save(int e, void *copy, uint32_t size)
{
	uint8_t *p = (uint8_t *)copy + offsetof(CBaseMonster, m_pSchedule);
	const Schedule_t *s;
	uintptr_t v;
	uint32_t id;
	int monster;

	if (priv_check(e, size, &monster) < 0)
		return -1;
	if (!monster)
		return 0;
	memcpy(&s, p, sizeof(s));
	id = sched_id(s);
	if (id == SCHED_UNKNOWN)
	{
		log_printf("savegame: %s (%d): schedule \"%s\" is not in the saved-game registry\n",
			game_class_name(ent_idc(e)->cls), e, s->pName ? s->pName : "?");
		return -1;
	}
	v = id;
	memcpy(p, &v, sizeof(v));
	return 0;
}

int game_priv_load(int e, void *blk, uint32_t size)
{
	uint8_t *p = (uint8_t *)blk + offsetof(CBaseMonster, m_pSchedule);
	const Schedule_t *s;
	uintptr_t v;
	int monster;

	if (priv_check(e, size, &monster) < 0)
		return -1;
	if (!monster)
		return 0;
	memcpy(&v, p, sizeof(v));
	if (v > 0xFFFFu || sched_of((uint32_t)v, &s) < 0)
	{
		log_printf("savegame: %s (%d): schedule id %lu not in this build\n",
			game_class_name(ent_idc(e)->cls), e, (unsigned long)v);
		return -1;
	}
	memcpy(p, &s, sizeof(s));
	return 0;
}

/* ---- settle --------------------------------------------------------------------- */

void game_save_settle(void)
{
	Ai_Settle();
}

/* ---- file-scope state ------------------------------------------------------------ */

/* game/hud: the HUD the saving game showed (fades, pickup history,
 * damage tiles, its rand()); client-side only */
static uint32_t Hud_SaveState(int op)
{
	return sg_io(op, &hud, sizeof(hud));
}

/* monsters.c: the last Look's sight list (HLSDK's m_pLink chain, read by
 * later thinks of its looker) through Mon_SharedSave / Load; at a frame
 * boundary no CheckLocalMove scope is open */
static uint32_t Sight_SaveState(int op)
{
	mon_shared_t sh;

	if (op == SG_SIZE)
		return sizeof(sh);
	if (op == SG_PUT)
	{
		memset(&sh, 0, sizeof(sh));
		Mon_SharedSave(&sh);
		sv_put(&sh, sizeof(sh));
		return 0;
	}
	sv_get(&sh, sizeof(sh));
	if (sh.lm_depth != 0 || sh.sight_count < 0 || sh.sight_count > SIGHT_MAX ||
		sh.sight_owner < -1 || sh.sight_owner >= HL64_MAX_ENTITIES)
	{
		sv_fail("game: sight list out of range");
		return 0;
	}
	Mon_SharedLoad(&sh, 1);
	return 0;
}

typedef struct {
	uint32_t	tag;
	uint32_t	(*io)(int op);
} section_t;

/* the order of the GAME sections in a save (a change: GAME_SAVE_VERSION) */
static const section_t sections[] = {
	{ SV_TAG('G', 'u', 't', 'l'), Util_SaveState },
	{ SV_TAG('G', 's', 'n', 'd'), SoundEnt_SaveState },
	{ SV_TAG('G', 's', 'g', 't'), Sight_SaveState },
	{ SV_TAG('G', 'g', 'r', 'f'), Graph_SaveState },
	{ SV_TAG('G', 's', 'n', 't'), SENTENCEG_SaveState },
	{ SV_TAG('G', 't', 'l', 'k'), Tlk_SaveState },
	{ SV_TAG('G', 'g', 'r', 'u'), Grunt_SaveState },
	{ SV_TAG('G', 'f', 'b', 'l'), FireBullets_SaveState },
	{ SV_TAG('G', 'm', 'm', 'c'), MM_SaveState },
	{ SV_TAG('G', 'e', 'n', 'd'), EndSection_SaveState },
	{ SV_TAG('G', 'w', 'l', 'd'), World_SaveState },
	{ SV_TAG('G', 's', 'u', 'm'), Sv_SpawnSumState },
	{ SV_TAG('G', 'p', 'l', 'r'), Player_SaveState },
	{ SV_TAG('G', 'w', 'p', 'n'), W_SaveState },
	{ SV_TAG('G', 'c', 'b', 'r'), W_CrowbarSaveState },
	{ SV_TAG('G', 'p', 'm', 'h'), Pm_HostSaveState },
	{ SV_TAG('G', 'p', 'm', 's'), Pm_SharedSaveState },
	{ SV_TAG('G', 'p', 's', 'e'), Gs_PoseSaveState },
	{ SV_TAG('G', 'h', 'u', 'd'), Hud_SaveState },
	{ SV_TAG('G', 'v', 'i', 'w'), View_SaveState },
};
#define NSECTIONS	((int)(sizeof(sections) / sizeof(sections[0])))

void game_save_globals(void)
{
	int i;

	sv_tag(SV_TAG('G', 'A', 'M', 'E'));
	sv_u32(GAME_SAVE_VERSION);
	for (i = 0; i < NSECTIONS; i++)
	{
		sv_tag(sections[i].tag);
		sections[i].io(SG_PUT);
	}
}

int game_load_globals(void)
{
	int i;

	if (sv_expect(SV_TAG('G', 'A', 'M', 'E')) < 0)
		return -1;
	if (sv_get_u32() != GAME_SAVE_VERSION)
	{
		sv_fail("game: save version");
		return -1;
	}
	for (i = 0; i < NSECTIONS; i++)
	{
		if (sv_expect(sections[i].tag) < 0)
			return -1;
		sections[i].io(SG_GET);
		if (sv_failed())
			return -1;
	}
	return 0;
}

/* ---- layout id ------------------------------------------------------------------- */

static uint32_t fnv_u32(uint32_t h, uint32_t v)
{
	int i;

	for (i = 0; i < 4; i++, v >>= 8)
		h = (h ^ (v & 0xFF)) * 16777619u;
	return h;
}

uint32_t game_save_layout(void)
{
	uint32_t h = 2166136261u;
	int i;

	h = fnv_u32(h, GAME_SAVE_VERSION);
	h = fnv_u32(h, (uint32_t)sizeof(void *));
	h = fnv_u32(h, CLS__COUNT);
	for (i = 0; i < CLS__COUNT; i++)
	{
		const game_class_t *c = game_class((cls_t)i);

		h = fnv_u32(h, c ? c->size : 0);
		h = fnv_u32(h, priv_bytes((cls_t)i));
	}
	h = fnv_u32(h, (uint32_t)sizeof(CBaseMonster));
	h = fnv_u32(h, (uint32_t)offsetof(CBaseMonster, m_pSchedule));
	for (i = 0; i < NSECTIONS; i++)
	{
		h = fnv_u32(h, sections[i].tag);
		h = fnv_u32(h, sections[i].io(SG_SIZE));
	}
	return h;
}

/* ---- after a load ---------------------------------------------------------------- */

void game_load_done(void)
{
	int e, layer = eng_frame_layer();

	for (e = 0; e < HL64_MAX_ENTITIES; e++)
	{
		void (*fix)(int e);

		if (!ent_is_live(e) || !ent_priv(e))
			continue;
		switch (ent_idc(e)->cls)
		{
		case CLS_MONSTER_GARGANTUA: fix = CGargantua_LoadDone; break;
		case CLS_MONSTER_ALIEN_SLAVE: fix = CISlave_LoadDone; break;
		case CLS_MONSTER_TRIPMINE: fix = CTripmineGrenade_LoadDone; break;
		default: continue;
		}
		/* beam points are map-local in the entity's frame */
		eng_set_frame_layer(ent_idc(e)->layer);
		fix(e);
	}
	eng_set_frame_layer(layer);
	Gs_LatchLoadDone();
}
