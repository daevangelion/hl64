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
 * cbase.h -- the game's entity base (HLSDK dlls/cbase.h, util.h, const.h
 * constants) on the hl64 components (include/hl64/ent.h; architecture.md
 * 6.2, 7.3).
 *
 * An entity is a slot int. The entvars the engine interprets are
 * components (origin / angles / box / solid / movetype / flags / owner in
 * col_ent_t, velocity in ent_mot_t, nextthink / think / touch / use /
 * blocked in ent_tm_t, model / frame / render in ent_rnd_t, targetname /
 * class / record gid in ent_id_t). Everything else HLSDK keeps in entvars
 * or in class members lives in the entity's game private block
 * (ent_priv_alloc, one per entity), laid out as HLSDK's class chain with
 * HLSDK member names: CBaseEntity -> CBaseDelay -> CBaseAnimating ->
 * CBaseToggle -> (game/hl/monster.h CBaseMonster, player / item structs).
 * Each level is a flat member list macro, so a derived block starts with
 * its bases' members in the same order and a pointer to it may be used as
 * a pointer to any base (game/hl builds with -fno-strict-aliasing).
 *
 * Virtual functions are a per-class table of function pointers
 * (game_vt_t; NULL = the CBaseEntity default); think / touch / use /
 * blocked are funcs.def ids stored in ent_tm_t (game/defs/funcs.def,
 * dispatch.c). Classes register a game_class_t named gc_<IDENT> for their
 * classes.def line in their own source file (dispatch.c builds the table
 * from weak references: a class without one refuses to spawn, logged).
 *
 * References kept across frames are ent_h (enemy, goal, activator):
 * resolve them with ent_of() (-1 when freed). Names are name_t, models and
 * sounds res_t, times f32 seconds of the game clock (gtime()).
 */
#ifndef HL64_GAME_CBASE_H
#define HL64_GAME_CBASE_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/rec.h"
#include "hl64/types.h"
#include "game/studio/anim.h"

/* ---- HLSDK constants (common/const.h, dlls/cbase.h, util.h) --------------- */

#define SOLID_NOT		COL_SOLID_NOT
#define SOLID_TRIGGER		COL_SOLID_TRIGGER
#define SOLID_BBOX		COL_SOLID_BBOX
#define SOLID_SLIDEBOX		COL_SOLID_SLIDEBOX
#define SOLID_BSP		COL_SOLID_BSP

#define CONTENTS_EMPTY		COL_EMPTY
#define CONTENTS_SOLID		COL_SOLID
#define CONTENTS_WATER		COL_WATER
#define CONTENTS_SLIME		COL_SLIME
#define CONTENTS_LAVA		COL_LAVA
#define CONTENTS_SKY		COL_SKY
#define CONTENTS_LADDER		COL_LADDER

#define DEAD_NO			0
#define DEAD_DYING		1
#define DEAD_DEAD		2
#define DEAD_RESPAWNABLE	3
#define DEAD_DISCARDBODY	4

#define DAMAGE_NO		0
#define DAMAGE_YES		1
#define DAMAGE_AIM		2

#define kRenderNormal		0
#define kRenderTransColor	1
#define kRenderTransTexture	2
#define kRenderGlow		3
#define kRenderTransAlpha	4
#define kRenderTransAdd		5

#define kRenderFxNone		0
#define kRenderFxPulseSlow	1
#define kRenderFxPulseFast	2
#define kRenderFxPulseSlowWide	3
#define kRenderFxPulseFastWide	4
#define kRenderFxFadeSlow	5
#define kRenderFxFadeFast	6
#define kRenderFxSolidSlow	7
#define kRenderFxSolidFast	8
#define kRenderFxStrobeSlow	9
#define kRenderFxStrobeFast	10
#define kRenderFxStrobeFaster	11
#define kRenderFxFlickerSlow	12
#define kRenderFxFlickerFast	13
#define kRenderFxNoDissipation	14
#define kRenderFxDistort	15
#define kRenderFxHologram	16
#define kRenderFxDeadPlayer	17
#define kRenderFxExplode	18
#define kRenderFxGlowShell	19
#define kRenderFxClampMinScale	20

#define FCAP_CUSTOMSAVE		0x00000001
#define FCAP_ACROSS_TRANSITION	0x00000002
#define FCAP_MUST_SPAWN		0x00000004
#define FCAP_DONT_SAVE		0x80000000
#define FCAP_IMPULSE_USE	0x00000008
#define FCAP_CONTINUOUS_USE	0x00000010
#define FCAP_ONOFF_USE		0x00000020
#define FCAP_DIRECTIONAL_USE	0x00000040
#define FCAP_MASTER		0x00000080
#define FCAP_FORCE_TRANSITION	0x00000080

#define CLASS_NONE		0
#define CLASS_MACHINE		1
#define CLASS_PLAYER		2
#define CLASS_HUMAN_PASSIVE	3
#define CLASS_HUMAN_MILITARY	4
#define CLASS_ALIEN_MILITARY	5
#define CLASS_ALIEN_PASSIVE	6
#define CLASS_ALIEN_MONSTER	7
#define CLASS_ALIEN_PREY	8
#define CLASS_ALIEN_PREDATOR	9
#define CLASS_INSECT		10
#define CLASS_PLAYER_ALLY	11
#define CLASS_PLAYER_BIOWEAPON	12
#define CLASS_ALIEN_BIOWEAPON	13
#define CLASS_VEHICLE		14
#define CLASS_BARNACLE		99

#define DONT_BLEED		(-1)
#define BLOOD_COLOR_RED		247
#define BLOOD_COLOR_YELLOW	195
#define BLOOD_COLOR_GREEN	BLOOD_COLOR_YELLOW

#define SF_NORESPAWN		(1 << 30)
#define SF_ITEM_USE_ONLY	256

#define SF_TRIGGER_ALLOWMONSTERS 1
#define SF_TRIGGER_NOCLIENTS	2
#define SF_TRIGGER_PUSHABLES	4

#define GIB_HEALTH_VALUE	(-30)
#define GIB_NORMAL		0
#define GIB_NEVER		1
#define GIB_ALWAYS		2

#define DMG_GENERIC		0
#define DMG_CRUSH		(1 << 0)
#define DMG_BULLET		(1 << 1)
#define DMG_SLASH		(1 << 2)
#define DMG_BURN		(1 << 3)
#define DMG_FREEZE		(1 << 4)
#define DMG_FALL		(1 << 5)
#define DMG_BLAST		(1 << 6)
#define DMG_CLUB		(1 << 7)
#define DMG_SHOCK		(1 << 8)
#define DMG_SONIC		(1 << 9)
#define DMG_ENERGYBEAM		(1 << 10)
#define DMG_NEVERGIB		(1 << 12)
#define DMG_ALWAYSGIB		(1 << 13)
#define DMG_DROWN		(1 << 14)
#define DMG_TIMEBASED		(~(0x3fff))
#define DMG_PARALYZE		(1 << 15)
#define DMG_NERVEGAS		(1 << 16)
#define DMG_POISON		(1 << 17)
#define DMG_RADIATION		(1 << 18)
#define DMG_DROWNRECOVER	(1 << 19)
#define DMG_ACID		(1 << 20)
#define DMG_SLOWBURN		(1 << 21)
#define DMG_SLOWFREEZE		(1 << 22)
#define DMG_MORTAR		(1 << 23)
#define DMG_GIB_CORPSE		(DMG_CRUSH | DMG_FALL | DMG_BLAST | DMG_SONIC | DMG_CLUB)
#define DMG_SHOWNHUD		(DMG_POISON | DMG_ACID | DMG_FREEZE | DMG_SLOWFREEZE | DMG_DROWN | \
				 DMG_BURN | DMG_SLOWBURN | DMG_NERVEGAS | DMG_RADIATION | DMG_SHOCK)

/* TOGGLE_STATE */
#define TS_AT_TOP		0
#define TS_AT_BOTTOM		1
#define TS_GOING_UP		2
#define TS_GOING_DOWN		3

/* ---- private blocks (HLSDK class members) ------------------------------- */

/* CBaseEntity: the entvars the engine does not interpret, and
 * CBaseEntity's own members. */
#define CBASEENTITY_FIELDS \
	uint32_t	spawnflags;	\
	float		health;		\
	float		max_health;	\
	float		dmg;		/* doors, triggers, explosions */ \
	float		dmg_take;	\
	float		speed;		\
	float		dmgtime;	\
	float		view_ofs[3];	\
	float		movedir[3];	\
	ent_h		enemy;		\
	ent_h		dmg_inflictor;	\
	ent_h		m_pGoalEnt;	/* path corner we are heading towards */ \
	ent_h		m_pLink;	\
	name_t		target;		\
	name_t		netname;	\
	uint16_t	message;	/* name_t / title / sentence / res by class */ \
	res_t		model;		/* pev->model as cooked (MODEL or SUBMODEL) */ \
	res_t		noise, noise1, noise2, noise3; \
	uint8_t		takedamage;	/* DAMAGE_* */ \
	uint8_t		deadflag;	/* DEAD_* */ \
	uint8_t		m_fireState;	\
	uint8_t		cbe_pad;

/* CBaseDelay */
#define CBASEDELAY_FIELDS \
	float		m_flDelay;	\
	name_t		m_iszKillTarget; \
	uint16_t	cbd_pad;

/* CBaseAnimating (Studio's gs_animating_t: m_flFrameRate, m_flGroundSpeed,
 * m_flLastEventCheck, m_fSequenceFinished, m_fSequenceLoops) */
#define CBASEANIMATING_FIELDS \
	gs_animating_t	anim;

/* CBaseToggle */
#define CBASETOGGLE_FIELDS \
	float		m_flActivateFinished; \
	float		m_flMoveDistance; \
	float		m_flWait;	\
	float		m_flLip;	\
	float		m_flTWidth;	\
	float		m_flTLength;	\
	float		m_vecPosition1[3]; \
	float		m_vecPosition2[3]; \
	float		m_vecAngle1[3];	\
	float		m_vecAngle2[3];	\
	float		m_flHeight;	\
	float		m_vecFinalDest[3]; \
	float		m_vecFinalAngle[3]; \
	ent_h		m_hActivator;	\
	int32_t		m_cTriggersLeft; \
	int32_t		m_bitsDamageInflict; \
	fn_t		m_pfnCallWhenMoveDone; /* think-kind funcs.def id */ \
	name_t		m_sMaster;	\
	uint8_t		m_toggle_state;	\
	uint8_t		cbt_pad[3];

typedef struct {
	CBASEENTITY_FIELDS
} CBaseEntity;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
} CBaseDelay;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
} CBaseAnimating;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
} CBaseToggle;

/* The private block of entity e as class T (NULL when e has none). */
#define PRIV(T, e)		((T *)ent_priv(e))

/* EHANDLE: an ent_h that may be "none" (zeroed handles would name the
 * world). Private blocks start with every handle EH_NONE. */
#define EH_NONE			0xFFFFFFFFu
static inline ent_h	EHANDLE(int e) { return e < 0 ? EH_NONE : ent_handle_of(e); }
static inline int	EHGet(ent_h h) { return h == EH_NONE ? -1 : ent_of(h); }

/* ---- virtual functions and the class table --------------------------------- */

/* HLSDK CBaseEntity virtuals that other classes call. NULL = the
 * CBaseEntity default (util.c). Positions are map-local in e's frame. */
typedef struct game_vt {
	int	(*ObjectCaps)(int e);
	int	(*Classify)(int e);
	int	(*BloodColor)(int e);
	void	(*TraceAttack)(int e, int attacker, float damage, const float dir[3],
			const col_trace_t *tr, int bits);
	int	(*TakeDamage)(int e, int inflictor, int attacker, float damage, int bits);
	int	(*TakeHealth)(int e, float health, int bits);
	void	(*Killed)(int e, int attacker, int gib);
	int	(*IsTriggered)(int e, int activator);
	int	(*GetToggleState)(int e);
	void	(*SetToggleState)(int e, int state);
	float	(*GetDelay)(int e);
	int	(*IsMoving)(int e);
	int	(*IsAlive)(int e);
	int	(*IRelationship)(int e, int other);
	int	(*DamageDecal)(int e, int bits);
	void	(*Activate)(int e);	/* after the map's records spawned */
	void	(*OverrideReset)(int e);
	void	(*DeathNotice)(int e, int child);
	void	(*BodyTarget)(int e, const float src[3], float out[3]);
	void	(*EyePosition)(int e, float out[3]);
} game_vt_t;

/* One class (classes.def line IDENT): gc_<IDENT>. */
typedef struct game_class {
	uint16_t	size;		/* private block bytes; 0 = none */
	uint16_t	flags;		/* GC_* */
	/* Entity spawn (KeyValue + Spawn + Precache): gid = the record (keys
	 * via rec_key_*), GID_NONE for runtime creation. 0, or -1 when the
	 * class refuses: `return SpawnRefuse("reason")` (dispatch logs it;
	 * the engine frees e). */
	int	(*Spawn)(int e, gid_t16 gid);
	const game_vt_t	*vt;
	/* Logic records (HLE_CF_LOGIC classes). */
	void	(*LogicSpawn)(gid_t16 gid);
	void	(*LogicUse)(gid_t16 gid, int activator, int caller, int use_type, float value);
	void	(*LogicTimer)(gid_t16 gid, uint16_t arg, int activator);
	int	(*LogicIsTriggered)(gid_t16 gid, int activator);	/* masters */
	/* slice extension: GameMon's monster table, Items' item info, ... */
	const void	*ext;
} game_class_t;

#define GC_MASTER		1	/* FCAP_MASTER logic class (multisource) */

/* classes.def ids: CLS_<IDENT> */
enum {
#define CLASS(id, name)	CLS_##id,
#include "game/defs/classes.def"
#undef CLASS
	CLS__COUNT
};

/* class entries: weak, so a class registers by defining gc_<IDENT> */
#define CLASS(id, name)	extern const game_class_t gc_##id __attribute__((weak));
#include "game/defs/classes.def"
#undef CLASS

/* A spawn refusal: Spawn returns SpawnRefuse(why) (-1), HLSDK's own reason
 * where its Spawn removes the entity (missing target / model / key, ...;
 * why: a static string). Dispatch logs every refusal, entity and logic, as
 * `spawn: CLASS map S lump L refused: WHY` (record gid) or
 * `spawn: CLASS (runtime entity E) refused: WHY` (gid GID_NONE): logic
 * classes removing their record at spawn call game_refuse_log. */
int			SpawnRefuse(const char *why);
void			game_refuse_log(cls_t cls, gid_t16 gid, int e, const char *why);

/* The class of entity e and its entry (NULL when unregistered). */
const game_class_t	*game_class(cls_t cls);
static inline const game_vt_t *ent_vt(int e)
{
	const game_class_t *c = game_class(ent_idc(e)->cls);
	return c ? c->vt : 0;
}

#endif
