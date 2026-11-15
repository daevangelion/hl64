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
 * precache.c -- level-load model precache (docs/architecture.md 6.4,
 * precache-and-resources.md 2). GoldSrc runs every entity's Precache when
 * the level loads, before the first frame; hl64 hydrates records near the
 * player during play, so a class's Precache (its PRECACHE calls in Spawn)
 * would declare a model's residency interest only at hydration and the
 * Spawn's model info read would block that frame (scientist 17 KB, about
 * 3 ms of cart PI). game_map_opened runs game_precache_map at HLE map open:
 * residency interest for every model and sprite the map's records will
 * need -- each record class's precache list below, the classes it creates
 * (monstermaker monstertype, func_breakable spawnobject, world_items type,
 * the drops and projectiles of the class code), and the per-record model
 * keys a class reads (env_shooter shootmodel, func_breakable material gibs
 * and gibmodel). The record's own `model` key is declared by rec at the
 * open (engine/ent/rec.c). The lists mirror the classes' PRECACHE /
 * ent_set_model calls of the game/hl classes; sounds are not listed (the AUDIO
 * class never blocks a frame).
 */
#include <string.h>

#include "game/hl/breakable.h"
#include "game/hl/cbase.h"
#include "game/hl/util.h"

#define NO_RES		0xFFFF

typedef struct {
	uint16_t	cls;	/* CLS_ */
	uint16_t	res;	/* R_ (res.def line) or NO_RES */
	uint16_t	dep;	/* CLS_ the class creates, or CLS_NONE */
} pre_t;

#define M(c, r)		{ CLS_##c, R_##r, CLS_NONE }
#define D(c, d)		{ CLS_##c, NO_RES, CLS_##d }

static const pre_t pre[] = {
	/* every map (HLSDK CWorld::Precache / W_Precache): gibs, blood */
	M(WORLDSPAWN, MON_HGIBS), M(WORLDSPAWN, MON_AGIBS), M(WORLDSPAWN, SPR_BLOOD),
	M(WORLDSPAWN, SPR_BLOODSPRAY),
	/* monsters */
	M(MONSTER_COCKROACH, MON_ROACH_MDL),
	M(MONSTER_SCIENTIST, MON_SCIENTIST_MDL),
	M(MONSTER_SCIENTIST_DEAD, MON_SCIENTIST_MDL),
	M(MONSTER_HEADCRAB, MON_HEADCRAB_MDL),
	M(MONSTER_ZOMBIE, MON_ZOMBIE_MDL),
	M(MONSTER_HOUNDEYE, MON_HOUNDEYE_MDL), M(MONSTER_HOUNDEYE, MON_SHOCKWAVE_SPR),
	M(MONSTER_BARNEY, MON_BARNEY_MDL), D(MONSTER_BARNEY, WEAPON_9MMHANDGUN),
	M(MONSTER_GMAN, MON_GMAN_MDL),
	M(MONSTER_GARGANTUA, MON_GARG_MDL), M(MONSTER_GARGANTUA, GA_BEAM_SPR),
	M(MONSTER_GARGANTUA, GA_EYE_SPR), M(MONSTER_GARGANTUA, MDL_METALPLATEGIBS),
	M(MONSTER_GARGANTUA, SPR_SMOKE), D(MONSTER_GARGANTUA, ENV_EXPLOSION),
	M(MONSTER_BARNACLE, MON_BARNACLE_MDL),
	M(MONSTER_HUMAN_GRUNT, MON_HGRUNT_MDL), M(MONSTER_HUMAN_GRUNT, W_SHELL),
	M(MONSTER_HUMAN_GRUNT, W_SHOTGUNSHELL), D(MONSTER_HUMAN_GRUNT, WEAPON_9MMAR),
	D(MONSTER_HUMAN_GRUNT, WEAPON_SHOTGUN), D(MONSTER_HUMAN_GRUNT, AMMO_ARGRENADES),
	D(MONSTER_HUMAN_GRUNT, GRENADE),
	M(MONSTER_HGRUNT_DEAD, MON_HGRUNT_MDL),
	M(MONSTER_GRUNT_REPEL, MON_ROPE_SPR), D(MONSTER_GRUNT_REPEL, MONSTER_HUMAN_GRUNT),
	M(MONSTER_TRIPMINE, W_V_TRIPMINE), M(MONSTER_TRIPMINE, SPR_LASERBEAM),
	M(MONSTER_ALIEN_SLAVE, MON_ISLAVE_MDL), M(MONSTER_ALIEN_SLAVE, SLV_LGTNING_SPR),
	M(MONSTER_RAT, MON_BIGRAT_MDL),
	M(MONSTER_MINITURRET, MON_MINITURRET_MDL), M(MONSTER_MINITURRET, SPR_SMOKE),
	/* items (items.c ITEM_CLASS), ammo (weapons.c AMMO_CLASS) */
	M(ITEM_SUIT, I_W_SUIT), M(ITEM_HEALTHKIT, I_W_MEDKIT), M(ITEM_BATTERY, I_W_BATTERY),
	M(ITEM_LONGJUMP, I_W_LONGJUMP),
	M(AMMO_9MMCLIP, W_W_9MMCLIP), M(AMMO_9MMAR, W_W_9MMARCLIP), M(AMMO_MP5CLIP, W_W_9MMARCLIP),
	M(AMMO_9MMBOX, W_W_CHAINAMMO), M(AMMO_ARGRENADES, W_W_ARGRENADE),
	M(AMMO_MP5GRENADES, W_W_ARGRENADE), M(AMMO_BUCKSHOT, W_W_SHOTBOX),
	M(AMMO_357, W_W_357AMMOBOX),
	/* weapons (weapons.c W_Precache item_info v / p / w, their shells) */
	M(WEAPON_CROWBAR, W_V_CROWBAR), M(WEAPON_CROWBAR, W_P_CROWBAR),
	M(WEAPON_CROWBAR, W_W_CROWBAR),
	M(WEAPON_9MMHANDGUN, W_V_9MMHANDGUN), M(WEAPON_9MMHANDGUN, W_P_9MMHANDGUN),
	M(WEAPON_9MMHANDGUN, W_W_9MMHANDGUN), M(WEAPON_9MMHANDGUN, W_SHELL),
	D(WEAPON_GLOCK, WEAPON_9MMHANDGUN),
	M(WEAPON_357, W_V_357), M(WEAPON_357, W_P_357), M(WEAPON_357, W_W_357),
	M(WEAPON_SHOTGUN, W_V_SHOTGUN), M(WEAPON_SHOTGUN, W_P_SHOTGUN),
	M(WEAPON_SHOTGUN, W_W_SHOTGUN), M(WEAPON_SHOTGUN, W_SHOTGUNSHELL),
	M(WEAPON_9MMAR, W_V_9MMAR), M(WEAPON_9MMAR, W_P_9MMAR), M(WEAPON_9MMAR, W_W_9MMAR),
	M(WEAPON_9MMAR, W_SHELL), D(WEAPON_9MMAR, GRENADE),
	D(WEAPON_MP5, WEAPON_9MMAR),
	M(WEAPON_HANDGRENADE, W_V_GRENADE), M(WEAPON_HANDGRENADE, W_P_GRENADE),
	M(WEAPON_HANDGRENADE, W_W_GRENADE), D(WEAPON_HANDGRENADE, GRENADE),
	M(WEAPON_TRIPMINE, W_V_TRIPMINE), M(WEAPON_TRIPMINE, W_P_TRIPMINE),
	D(WEAPON_TRIPMINE, MONSTER_TRIPMINE),
	M(WEAPONBOX, W_W_WEAPONBOX),
	/* projectiles, effects, spawners */
	M(GRENADE, MDL_GRENADE), M(GRENADE, SPR_FIREBALL), M(GRENADE, SPR_SMOKE),
	M(GRENADE, SPR_WXPLO1), D(GRENADE, SPARK_SHOWER),
	M(ENV_EXPLOSION, SPR_FIREBALL), M(ENV_EXPLOSION, SPR_SMOKE),
	D(ENV_EXPLOSION, SPARK_SHOWER),
	/* spark_shower (env.c CShower_Spawn): a never-drawn grenade model */
	M(SPARK_SHOWER, MDL_GRENADE),
	D(ENV_BEVERAGE, ITEM_SODACAN), M(ITEM_SODACAN, MDL_CAN),
	M(GIBSHOOTER, MON_HGIBS),
	/* env_bubbles (CBubbling::Precache) */
	M(ENV_BUBBLES, SPR_BUBBLE),
};

#undef M
#undef D

static uint8_t	done[(CLS__COUNT + 7) / 8];

static void precache_class(cls_t cls)
{
	size_t i;

	if (cls >= CLS__COUNT || (done[cls >> 3] >> (cls & 7) & 1))
		return;
	done[cls >> 3] |= (uint8_t)(1u << (cls & 7));
	for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++)
	{
		if (pre[i].cls != cls)
			continue;
		if (pre[i].res != NO_RES)
			PRECACHE(res_def(pre[i].res));
		else
			precache_class(pre[i].dep);
	}
}

/* The model keys a record's class reads (Spawn / KeyValue), and the
 * classes its keys name. */
static void precache_record(gid_t16 gid, cls_t cls)
{
	int type;

	switch (cls)
	{
	case CLS_MONSTERMAKER:
		precache_class(rec_key_cls(gid, K_MONSTERTYPE));
		break;
	case CLS_ENV_SHOOTER:
		PRECACHE(rec_key_res(gid, K_SHOOTMODEL));
		break;
	case CLS_ENV_BEAM:
		PRECACHE(rec_key_res(gid, K_TEXTURE));
		break;
	case CLS_ENV_LASER:
		PRECACHE(rec_key_res(gid, K_TEXTURE));
		PRECACHE(rec_key_res(gid, K_ENDSPRITE));
		break;
	case CLS_FUNC_BREAKABLE:
	case CLS_FUNC_PUSHABLE:
		Breakable_Precache(Breakable_Material(gid), rec_key_res(gid, K_GIBMODEL));
		precache_class(Breakable_SpawnObject((int)KeyF(gid, K_SPAWNOBJECT, 0)));
		/* breakable.c Die: ExplosionCreate when it has a magnitude */
		if ((int)KeyF(gid, K_EXPLODEMAGNITUDE, 0))
			precache_class(CLS_ENV_EXPLOSION);
		break;
	case CLS_WORLD_ITEMS:
		/* CWorldItem::Spawn: 44 battery, 45 suit (42 / 43 not in the game) */
		type = (int)KeyF(gid, K_TYPE, 0);
		precache_class(type == 44 ? CLS_ITEM_BATTERY : type == 45 ? CLS_ITEM_SUIT : CLS_NONE);
		break;
	default:
		break;
	}
}

void game_precache_map(int slot)
{
	int lump;

	memset(done, 0, sizeof(done));
	precache_class(CLS_WORLDSPAWN);
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		cls_t cls;

		if (rec_kind(gid) < 0)
			continue;
		cls = rec_class(gid);
		precache_class(cls);
		precache_record(gid, cls);
	}
}

void game_precache_class(cls_t cls)
{
	memset(done, 0, sizeof(done));
	precache_class(cls);
}
