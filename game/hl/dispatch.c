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
 * dispatch.c -- the game exports the engine calls (include/hl64/game.h,
 * architecture.md 7.2): HLSDK cbase.cpp DispatchSpawn / DispatchKeyValue /
 * DispatchThink / DispatchTouch / DispatchUse / DispatchBlocked over the
 * class table (classes.def -> gc_<IDENT>, weak: a class registers by
 * defining its entry) and the function table (funcs.def ids), logic
 * records, masters, and the game's lifetime hooks.
 *
 * Other game slices hook lifetime events without editing this file by
 * defining the weak functions below (game_mon_*, game_items_*).
 */
#include <string.h>

#include "engine/core/sys.h"
#include "game/hl/util.h"

/* ---- profile (prof game_us roll-up) ---------------------------------------- */

/* Time inside the game's dispatch per frame (outermost calls only: a use
 * fired from a think counts once; avg / max over the roll-up window's
 * frames), and the window's totals of thinks, uses (entity + logic) and
 * fires (rec_fire / rec_fire_at the game issues, game_fires). */
static struct {
	uint64_t	sum, max, frame, t0;
	uint32_t	frames, thinks, uses;
	int		depth;
} gprof;
uint32_t	game_fires;

static void prof_enter(void)
{
	if (!gprof.depth++)
		gprof.t0 = sys_time_us();
}

static void prof_leave(void)
{
	if (!--gprof.depth)
		gprof.frame += sys_time_us() - gprof.t0;
}

void game_prof_line(void)
{
	unsigned long n = gprof.frames ? gprof.frames : 1;

	log_printf("prof game_us avg=%lu max=%lu thinks=%lu uses=%lu fires=%lu\n",
		(unsigned long)(gprof.sum / n), (unsigned long)gprof.max,
		(unsigned long)gprof.thinks, (unsigned long)gprof.uses,
		(unsigned long)game_fires);
	gprof.sum = gprof.max = 0;
	gprof.frames = gprof.thinks = gprof.uses = 0;
	game_fires = 0;
}

/* ---- function table (funcs.def; fn_t = index + 1) ------------------------ */

#define FK_THINK	0
#define FK_TOUCH	1
#define FK_USE		2
#define FK_BLOCKED	3

typedef struct {
	uint8_t		kind;
	union {
		void	(*think)(int e);
		void	(*touch)(int e, int other);
		void	(*use)(int e, int activator, int caller, int use_type, float value);
	} fn;
#ifdef HL64_HOST
	const char	*name;
#endif
} game_func_t;

#ifdef HL64_HOST
#define FNAME(f)	, #f
#else
#define FNAME(f)
#endif

static const game_func_t game_funcs[] = {
#define THINK(f)	{ FK_THINK, { .think = f } FNAME(f) },
#define TOUCH(f)	{ FK_TOUCH, { .touch = f } FNAME(f) },
#define USE(f)		{ FK_USE, { .use = f } FNAME(f) },
#define BLOCKED(f)	{ FK_BLOCKED, { .touch = f } FNAME(f) },
#include "game/defs/funcs.def"
#undef THINK
#undef TOUCH
#undef USE
#undef BLOCKED
};

static const game_func_t *func_of(fn_t f, int kind, int e)
{
	const game_func_t *gf;

	if (f == FN_NONE)
		return 0;
	if (f >= FN__COUNT)
	{
		log_printf("game: entity %d: bad function id %u\n", e, (unsigned)f);
		return 0;
	}
	gf = &game_funcs[f - 1];
	if (gf->kind != kind)
	{
		log_printf("game: entity %d: function id %u has kind %u, called as %u\n", e,
			(unsigned)f, (unsigned)gf->kind, (unsigned)kind);
		return 0;
	}
	return gf;
}

void game_call_think(int e, fn_t f)
{
	const game_func_t *gf = func_of(f, FK_THINK, e);

	if (gf)
		gf->fn.think(e);
}

/* ---- class table (classes.def) --------------------------------------------- */

static const game_class_t *const game_classes[CLS__COUNT] = {
#define CLASS(id, name)	&gc_##id,
#include "game/defs/classes.def"
#undef CLASS
};

static const char *const game_class_names[CLS__COUNT] = {
#define CLASS(id, name)	name,
#include "game/defs/classes.def"
#undef CLASS
};

const game_class_t *game_class(cls_t cls)
{
	return cls < CLS__COUNT ? game_classes[cls] : 0;
}

const char *game_class_name(cls_t cls)
{
	return cls < CLS__COUNT ? game_class_names[cls] : "?";
}

/* ---- slice hooks (weak) ------------------------------------------------------ */

void	game_mon_init(void) __attribute__((weak));
void	game_items_init(void) __attribute__((weak));
void	game_mon_new_campaign(void) __attribute__((weak));
void	game_items_new_campaign(void) __attribute__((weak));
void	game_mon_map_opened(int slot) __attribute__((weak));
void	game_mon_map_closing(int slot) __attribute__((weak));
void	game_mon_start_frame(void) __attribute__((weak));
void	game_items_start_frame(void) __attribute__((weak));

void	world_lightstyles(void);

/* ---- lifetime ------------------------------------------------------------------ */

static const phy_game_hooks_t game_hooks = { game_think, game_touch, game_blocked };

/* The client effects' sounds and sprites (fx.h FX_A_* sets): ricochets
 * weapons/ric1-5 (HLSDK world.cpp W_Precache), the func_break.cpp
 * material debris sounds, and the shipped valve files named for the other
 * roles (weapons/explode3-5, player/pl_shell1-3, weapons/sshell1-3,
 * sprites/richo1.spr, sprites/muzzleflash1-3 for flash type % 10 0-2). */
static void fx_asset_set(int set, const int *defs, int n)
{
	res_t ids[FX_ASSET_MAX];
	int i;

	for (i = 0; i < n; i++)
		ids[i] = res_def(defs[i]);
	fx_assets(set, ids, n);
}

static void game_fx_assets(void)
{
	static const int ric[] = { R_WEAPONS_RIC1, R_WEAPONS_RIC2, R_WEAPONS_RIC3,
		R_WEAPONS_RIC4, R_WEAPONS_RIC5 };
	static const int expl[] = { R_WEAPONS_EXPLODE3, R_WEAPONS_EXPLODE4, R_WEAPONS_EXPLODE5 };
	static const int shell[] = { R_PLAYER_PL_SHELL1, R_PLAYER_PL_SHELL2, R_PLAYER_PL_SHELL3 };
	static const int sshell[] = { R_WEAPONS_SSHELL1, R_WEAPONS_SSHELL2, R_WEAPONS_SSHELL3 };
	static const int glass[] = { R_DEBRIS_GLASS1, R_DEBRIS_GLASS2, R_DEBRIS_GLASS3 };
	static const int metal[] = { R_DEBRIS_METAL1, R_DEBRIS_METAL2, R_DEBRIS_METAL3 };
	static const int flesh[] = { R_DEBRIS_FLESH1, R_DEBRIS_FLESH2, R_DEBRIS_FLESH3,
		R_DEBRIS_FLESH5, R_DEBRIS_FLESH6, R_DEBRIS_FLESH7 };
	static const int wood[] = { R_DEBRIS_WOOD1, R_DEBRIS_WOOD2, R_DEBRIS_WOOD3 };
	static const int concrete[] = { R_DEBRIS_CONCRETE1, R_DEBRIS_CONCRETE2,
		R_DEBRIS_CONCRETE3 };
	static const int richo[] = { R_SPR_RICHO1 };
	static const int muzzle[] = { R_SPR_MUZZLEFLASH1, R_SPR_MUZZLEFLASH2,
		R_SPR_MUZZLEFLASH3 };
#define SET(s, a)	fx_asset_set(s, a, (int)(sizeof(a) / sizeof(a[0])))
	SET(FX_A_RICOCHET_SND, ric);
	SET(FX_A_EXPLODE_SND, expl);
	SET(FX_A_SHELL_SND, shell);
	SET(FX_A_SHOTSHELL_SND, sshell);
	SET(FX_A_GLASS_SND, glass);
	SET(FX_A_METAL_SND, metal);
	SET(FX_A_FLESH_SND, flesh);
	SET(FX_A_WOOD_SND, wood);
	SET(FX_A_CONCRETE_SND, concrete);
	SET(FX_A_RICOCHET_SPR, richo);
	SET(FX_A_MUZZLE_SPR, muzzle);
#undef SET
}

void game_init(void)
{
	phy_set_game_hooks(&game_hooks);
	game_fx_assets();
	if (game_mon_init)
		game_mon_init();
	if (game_items_init)
		game_items_init();
}

void	game_logic_reset(void);

void game_new_campaign(void)
{
	GlobalReset();
	EndSectionReset();
	game_logic_reset();
	ClearMultiDamage();
	memset(&gpGlobals, 0, sizeof gpGlobals);
	world_lightstyles();
	if (game_mon_new_campaign)
		game_mon_new_campaign();
	if (game_items_new_campaign)
		game_items_new_campaign();
}

void game_map_opened(int slot)
{
	if (game_mon_map_opened)
		game_mon_map_opened(slot);
}

void game_map_closing(int slot)
{
	if (game_mon_map_closing)
		game_mon_map_closing(slot);
}

void game_start_frame(void)
{
	/* the previous frame's dispatch time (its active set, touches, uses)
	 * plus this frame's logic timers (step 4, before this call) */
	gprof.sum += gprof.frame;
	if (gprof.frame > gprof.max)
		gprof.max = gprof.frame;
	gprof.frame = 0;
	gprof.frames++;
	prof_enter();
	if (game_mon_start_frame)
		game_mon_start_frame();
	if (game_items_start_frame)
		game_items_start_frame();
	prof_leave();
}

/* ---- spawn (DispatchSpawn) ----------------------------------------------------- */

static void refuse_log(cls_t cls, gid_t16 gid, const char *why)
{
	if (gid == GID_NONE)
		log_printf("spawn: %s (runtime) refused: %s\n", game_class_name(cls), why);
	else
		log_printf("spawn: %s map %u lump %u refused: %s\n", game_class_name(cls),
			(unsigned)GID_SLOT(gid), (unsigned)GID_LUMP(gid), why);
}

static int spawn(cls_t cls, gid_t16 gid, int e)
{
	const game_class_t *c = game_class(cls);
	CBaseEntity *pev;
	uint32_t size;
	int i;

	if (!c || !c->Spawn)
	{
		refuse_log(cls, gid, c ? "logic class has no entity spawn" : "class not implemented");
		return -1;
	}
	ent_idc(e)->cls = cls;
	ent_idc(e)->gid = gid;
	size = c->size > sizeof(CBaseEntity) ? c->size : sizeof(CBaseEntity);
	pev = (CBaseEntity *)ent_priv_alloc(e, size);
	if (!pev)
	{
		refuse_log(cls, gid, "game private slab full");
		return -1;
	}
	pev->target = NAME_NONE;
	pev->netname = NAME_NONE;
	pev->message = NAME_NONE;
	pev->enemy = EH_NONE;
	pev->dmg_inflictor = EH_NONE;
	pev->m_pGoalEnt = EH_NONE;
	pev->m_pLink = EH_NONE;
	/* DispatchSpawn: a box around the origin until the entity links */
	for (i = 0; i < 3; i++)
	{
		ent_cv(e)->absmin[i] = ent_cv(e)->origin[i] - 1;
		ent_cv(e)->absmax[i] = ent_cv(e)->origin[i] + 1;
	}
	EntvarsKeyvalue(e, gid);
	if (c->Spawn(e, gid) < 0)
		return -1;
	if (!ent_is_live(e) || (ent_cv(e)->flags & FL_KILLME))
	{
		refuse_log(cls, gid, "removed itself in spawn");
		return -1;
	}
	if (c->vt && c->vt->Activate)
		c->vt->Activate(e);
	return 0;
}

int game_spawn_record(gid_t16 gid, int e)
{
	return spawn(rec_class(gid), gid, e);
}

int game_spawn_class(cls_t cls, int e)
{
	if (spawn(cls, GID_NONE, e) < 0)
	{
		if (ent_is_live(e))
			ent_destroy(e);
		return -1;
	}
	return 0;
}

void game_spawn_logic(gid_t16 gid)
{
	cls_t cls = rec_class(gid);
	const game_class_t *c = game_class(cls);

	if (!c || !c->LogicSpawn)
	{
		refuse_log(cls, gid, "no logic class");
		return;
	}
	c->LogicSpawn(gid);
}

/* ---- dispatch ------------------------------------------------------------------- */

void game_think(int e)
{
	const game_func_t *gf;

	if (ent_cv(e)->flags & FL_DORMANT)
		log_printf("game: dormant entity %s (%d) is thinking\n",
			game_class_name(ent_idc(e)->cls), e);
	gf = func_of(ent_tm(e)->think, FK_THINK, e);
	if (gf)
	{
		gprof.thinks++;
		prof_enter();
		gf->fn.think(e);
		prof_leave();
	}
}

void game_touch(int e, int other)
{
	const game_func_t *gf;

	if ((ent_cv(e)->flags | ent_cv(other)->flags) & FL_KILLME)
		return;
	gf = func_of(ent_tm(e)->touch, FK_TOUCH, e);
	if (gf)
	{
		prof_enter();
		gf->fn.touch(e, other);
		prof_leave();
	}
}

void game_blocked(int e, int other)
{
	const game_func_t *gf = func_of(ent_tm(e)->blocked, FK_BLOCKED, e);

	if (gf)
	{
		prof_enter();
		gf->fn.touch(e, other);
		prof_leave();
	}
}

void game_use(int e, int activator, int caller, int use_type, float value)
{
	const game_func_t *gf;

	if (!ent_is_live(e) || (ent_cv(e)->flags & FL_KILLME))
		return;
	gf = func_of(ent_tm(e)->use, FK_USE, e);
	if (gf)
	{
		gprof.uses++;
		prof_enter();
		gf->fn.use(e, activator, caller, use_type, value);
		prof_leave();
	}
}

void game_logic_use(gid_t16 gid, int activator, int caller, int use_type, float value)
{
	const game_class_t *c = game_class(rec_class(gid));

	if (c && c->LogicUse)
	{
		gprof.uses++;
		prof_enter();
		c->LogicUse(gid, activator, caller, use_type, value);
		prof_leave();
	}
}

void game_logic_timer(gid_t16 gid, uint16_t arg, int activator)
{
	const game_class_t *c = game_class(rec_class(gid));

	if (c && c->LogicTimer)
	{
		gprof.thinks++;
		prof_enter();
		c->LogicTimer(gid, arg, activator);
		prof_leave();
	}
}

/* UTIL_IsMasterTriggered's lookup: the first record named master (a
 * logic master or an entity with FCAP_MASTER). */
int game_master_state(name_t master, int activator)
{
	const game_class_t *c;
	gid_t16 g;
	int e;

	if (master == NAME_NONE || rec_name_count(master) < 1)
		return -1;
	g = rec_name_gid(master, 0);
	c = game_class(rec_class(g));
	if (rec_kind(g) == REC_KIND_LOGIC)
	{
		if (c && (c->flags & GC_MASTER) && c->LogicIsTriggered)
			return c->LogicIsTriggered(g, activator) ? 1 : 0;
		return -1;
	}
	e = rec_entity(g);
	if (e < 0)
		e = rec_wake(g);
	if (e < 0 || !(ObjectCaps(e) & FCAP_MASTER))
		return -1;
	return IsTriggered(e, activator) ? 1 : 0;
}
