/* SPDX-License-Identifier: MIT */
/*
 * ent.h -- entity components, active sets, think heap, entity lifetime and
 * the engine's entity services for game/ (docs/architecture.md 6.2, 6.3,
 * 7.1; docs/specs/think-and-time.md, physics-movetypes.md).
 *
 * Storage: structure of arrays indexed by slot (0 world, 1 player, game
 * entities from 2; HL64_MAX_ENTITIES), in the boot carve's components
 * region. The collision view col_ent_t (include/hl64/coll.h) is the
 * component holding origin / angles (xf), mins / maxs / absmin / absmax
 * (box) and the fields traces filter on (solid, movetype, flags, owner,
 * brush model, studio model, skin, rendermode); the other components
 * below hold the rest of 6.2. Every array is valid for the slot's life
 * (pointers into them stay valid while the slot is live).
 *
 * Frames (architecture.md 8 "Frames"): every position in a component is
 * map-local to the entity's layer (ent_id_t.layer); the spatial grid and
 * linking add g_M(layer) themselves. Game code only ever sees map-local
 * values.
 *
 * Writes: game code may write any component field directly EXCEPT the
 * ones whose change the engine must observe; those go through the setters
 * below (velocity / avelocity / basevelocity / flags / movetype / solid /
 * groundentity / aiment / nextthink / origin / size / model), or the
 * caller calls ent_motion_changed(e) after writing them directly. The K9
 * debug mode (step every live entity every frame) checks that nothing
 * was missed.
 */
#ifndef HL64_ENTCOMP_H
#define HL64_ENTCOMP_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/types.h"

/* ---- HLSDK constants the engine interprets (common/const.h values) ---- */

#define MOVETYPE_NONE		0
#define MOVETYPE_WALK		3
#define MOVETYPE_STEP		4
#define MOVETYPE_FLY		5
#define MOVETYPE_TOSS		6
#define MOVETYPE_PUSH		7
#define MOVETYPE_NOCLIP		8
#define MOVETYPE_FLYMISSILE	9
#define MOVETYPE_BOUNCE		10
#define MOVETYPE_BOUNCEMISSILE	11
#define MOVETYPE_FOLLOW		12
#define MOVETYPE_PUSHSTEP	13

#define FL_FLY			(1u << 0)
#define FL_SWIM			(1u << 1)
#define FL_CONVEYOR		(1u << 2)
#define FL_CLIENT		(1u << 3)
#define FL_INWATER		(1u << 4)
#define FL_MONSTER		(1u << 5)
#define FL_GODMODE		(1u << 6)
#define FL_NOTARGET		(1u << 7)
#define FL_SKIPLOCALHOST	(1u << 8)
#define FL_ONGROUND		(1u << 9)
#define FL_PARTIALGROUND	(1u << 10)
#define FL_WATERJUMP		(1u << 11)
#define FL_FROZEN		(1u << 12)
#define FL_FAKECLIENT		(1u << 13)
#define FL_DUCKING		(1u << 14)
#define FL_FLOAT		(1u << 15)
#define FL_GRAPHED		(1u << 16)
#define FL_IMMUNE_WATER		(1u << 17)
#define FL_IMMUNE_SLIME		(1u << 18)
#define FL_IMMUNE_LAVA		(1u << 19)
#define FL_PROXY		(1u << 20)
#define FL_ALWAYSTHINK		(1u << 21)
#define FL_BASEVELOCITY		(1u << 22)
#define FL_MONSTERCLIP		(1u << 23)
#define FL_ONTRAIN		(1u << 24)
#define FL_WORLDBRUSH		(1u << 25)
#define FL_SPECTATOR		(1u << 26)
#define FL_CUSTOMENTITY		(1u << 29)
#define FL_KILLME		(1u << 30)
#define FL_DORMANT		(1u << 31)

#define EF_BRIGHTFIELD		1
#define EF_MUZZLEFLASH		2
#define EF_BRIGHTLIGHT		4
#define EF_DIMLIGHT		8
#define EF_INVLIGHT		16
#define EF_NOINTERP		32
#define EF_LIGHT		64
#define EF_NODRAW		128

/* ---- components (architecture.md 6.2) ---------------------------------- */

typedef struct {
	float		velocity[3];
	float		avelocity[3];
	float		basevelocity[3];
} ent_mot_t;			/* 36 */

typedef struct {
	float		gravity;	/* scale, 0 = 1 */
	float		friction;	/* 0 = 1 (HLSDK pev->friction default 1) */
	float		ideal_yaw;	/* walk_move / move_to_origin / change_yaw */
	float		yaw_speed;
	uint16_t	groundentity;	/* slot, ENT_NONE */
	uint16_t	aiment;		/* MOVETYPE_FOLLOW target, ENT_NONE */
	int8_t		waterlevel;	/* 0..3 */
	int8_t		watertype;	/* COL_* contents */
	uint8_t		pad[2];
} ent_phys_t;			/* 24 */

typedef struct {
	float		nextthink;	/* f32 s; <= 0 none (think-and-time.md 2) */
	float		ltime;		/* pushers' local clock */
	fn_t		think, touch, use, blocked;	/* funcs.def ids, 0 none */
} ent_tm_t;			/* 16 */

typedef struct {
	res_t		model;		/* MODEL / SPRITE / SUBMODEL res_t, 0 none */
	uint16_t	sequence;
	float		frame;
	float		animtime;	/* f32 s */
	float		framerate;
	float		scale;		/* 0 = 1 */
	uint16_t	effects;	/* EF_* */
	uint8_t		body;
	uint8_t		renderfx;
	uint8_t		controller[4];
	uint8_t		blending[2];
	uint8_t		renderamt;
	uint8_t		rendercolor[3];
	uint16_t	sectors[4];	/* renderer: overlapped sectors (ENT_NONE) */
} ent_rnd_t;			/* 40 */
/* rendermode and skin live in col_ent_t (traces filter on them). */

#define ENT_ST_FREE		0
#define ENT_ST_LIVE		1

typedef struct {
	uint16_t	serial;
	uint8_t		state;		/* ENT_ST_* */
	int8_t		layer;		/* HLE / world slot of its frame, -1 none */
	cls_t		cls;		/* classes.def id */
	uint16_t	priv;		/* game private block (ent_priv) */
	gid_t16		gid;		/* map record, GID_NONE for runtime ones */
	uint16_t	rtid;		/* runtime id (persistence), 0 none */
	name_t		targetname;	/* NAME_NONE */
	uint16_t	spawnflags_hi;	/* reserved; 0 */
} ent_id_t;			/* 16 */

/* Component arrays (valid after ent_init). */
typedef struct {
	col_ent_t	*cv;		/* xf + box + collision fields */
	ent_mot_t	*mot;
	ent_phys_t	*phys;
	ent_tm_t	*tm;
	ent_rnd_t	*rnd;
	ent_id_t	*id;
} ent_comp_t;
extern ent_comp_t	ent_c;

static inline col_ent_t  *ent_cv(int e)   { return &ent_c.cv[e]; }
static inline ent_mot_t  *ent_mot(int e)  { return &ent_c.mot[e]; }
static inline ent_phys_t *ent_phys(int e) { return &ent_c.phys[e]; }
static inline ent_tm_t   *ent_tm(int e)   { return &ent_c.tm[e]; }
static inline ent_rnd_t  *ent_rnd(int e)  { return &ent_c.rnd[e]; }
static inline ent_id_t   *ent_idc(int e)  { return &ent_c.id[e]; }
static inline int	ent_is_live(int e)
{
	return e >= 0 && e < HL64_MAX_ENTITIES && ent_c.id[e].state == ENT_ST_LIVE;
}

/* ---- lifetime (think-and-time.md 6) -------------------------------------- */

#define ENT_ALLOC_GAME		0	/* may use every slot */
#define ENT_ALLOC_OPTIONAL	1	/* hydration: only while >= 48 slots free */
#define ENT_ALLOC_TRANSIENT	2	/* gibs, debris, shells: only while >= 32 free */

/* Lowest free slot >= 2 freed before time 2.0 or at least 0.5 s ago, else
 * the next never-used slot; components zeroed (owner / groundentity /
 * aiment / model refs ENT_NONE / COL_NONE, gravity / friction / scale 0),
 * layer = the caller's frame layer (eng_frame_layer), serial and creation
 * rank assigned. Returns the slot, or -1 (reserve rule; ENT_ALLOC_GAME
 * exhaustion is sys_error with a class census). */
int		ent_create(int reserve);
/* Immediate free (pfnRemoveEntity): unlink, leave the heap and the active
 * sets, free the private block, zero components (serial += 1). */
void		ent_destroy(int e);
/* HLSDK UTIL_Remove: FL_KILLME; the engine frees it after its movetype
 * step this frame (or at the end of the frame's entity pass). */
void		ent_remove(int e);
ent_h		ent_handle_of(int e);
/* Slot of a handle, -1 when freed since. */
int		ent_of(ent_h h);

/* Game private block (architecture.md 6.2): zeroed, from size-class slabs
 * (32..1024 B) in the game private region; NULL when the class is full
 * (optional spawns refuse). Freed with the entity. */
void		*ent_priv_alloc(int e, uint32_t size);
void		*ent_priv(int e);

/* ---- setters that keep links and active sets current (6.3) ------------- */

/* pfnSetOrigin: origin := org, link without touches. */
void		ent_set_origin(int e, const float org[3]);
/* pfnSetSize: mins / maxs, link. */
void		ent_set_size(int e, const float mins[3], const float maxs[3]);
/* pfnSetModel: rnd.model := m; a SUBMODEL sets the brush model (col model)
 * and the size to its bounds; a MODEL sets the studio model (hitboxes) and
 * the size as GoldSrc does for studio models; links. */
void		ent_set_model(int e, res_t m);
void		ent_set_velocity(int e, const float v[3]);
void		ent_set_avelocity(int e, const float v[3]);
void		ent_set_basevelocity(int e, const float v[3]);
void		ent_set_flags(int e, uint32_t flags);	/* whole word */
/* fatal (sys_error naming the class) for SOLID_BSP without MOVETYPE_PUSH */
void		ent_set_movetype(int e, int movetype);
void		ent_set_solid(int e, int solid);	/* relinks */
void		ent_set_ground(int e, int ground);	/* ENT_NONE / -1 clears */
void		ent_set_aiment(int e, int aiment);
/* nextthink := t (f32 s, <= 0 clears); updates the think heap. Pushers:
 * t is in ltime units (physics-movetypes.md 4.1). */
void		ent_think_at(int e, float t);
/* After direct writes to mot / cv.flags / cv.movetype / phys.groundentity:
 * re-evaluate active-set membership. */
void		ent_motion_changed(int e);
/* Link with or without trigger touches (area-links-and-touch.md 3). */
void		ent_relink(int e, int touch);

/* ---- frame (architecture.md 3.2 step 6; engine side) --------------------- */

/* Active set pass: every due or moving entity in ascending index, its
 * movetype step (engine/phys). time = frame start (f64 s), dt (f64 s). */
void		phy_entities_frame(double time, double dt);
/* K9: step every live entity every frame (test builds). */
void		phy_set_step_all(int on);

typedef struct {
	uint32_t	stepped, thinks, movers_max, heap_max;
	uint32_t	by_type[14];	/* steps per movetype */
	uint32_t	us;		/* step 6 CPU in the window */
} phy_ent_stats_t;
void		phy_ent_stats(phy_ent_stats_t *s, int reset);

/* ---- engine movement services (physics-movetypes.md 6) ------------------- */

#define WALKMOVE_NORMAL		0
#define WALKMOVE_WORLDONLY	1
#define WALKMOVE_CHECKONLY	2	/* moves origin, no link / touches / ground */
int		phy_walk_move(int e, float yaw, float dist, int mode);
/* Blocking entity of the last walk_move / move_to_origin / step trace
 * (HLSDK gpGlobals->trace_ent), -1 none. */
int		phy_trace_ent(void);
int		phy_move_to_origin(int e, const float goal[3], float dist, int mode);
/* -1 all-solid, 0 nothing below, 1 dropped */
int		phy_drop_to_floor(int e);
void		phy_change_yaw(int e);	/* toward phys.ideal_yaw by yaw_speed */
int		phy_check_bottom(int e);

/* ---- searches (7.1) -------------------------------------------------------- */

/* Next live entity after `after` (-1 starts) whose linked box comes within
 * r of org (HLSDK FindEntityInSphere: box distance), ascending index;
 * -1 when none. Map-local to the caller's frame. */
int		ent_next_in_sphere(int after, const float org[3], float r);
/* Live entities whose linked box meets [mins, maxs] (map-local, caller's
 * frame), any solid type incl. SOLID_NOT when linked, ascending index; at
 * most max; returns the count (UTIL_EntitiesInBox). */
int		ent_in_box(const float mins[3], const float maxs[3], uint32_t flagmask,
			uint16_t *out, int max);
/* Next live entity of class cls in HLE slot `slot` (-1: any) after `after`. */
int		ent_next_class(int after, cls_t cls, int slot);
/* The player if its PVS leaf set holds e's origin (pfnFindClientInPVS),
 * else 0 (the world). */
int		ent_client_in_pvs(int e);
/* pfnCheckVisibility: point (map-local, e's frame) in the PVS of the
 * viewer's eye leaf. */
int		ent_point_in_pvs(int viewer, const float p[3]);

/* ---- frames (architecture.md 8) ------------------------------------------ */

/* The layer whose frame engine calls from game code use: set by the
 * engine around every dispatch (the dispatched entity's layer; the
 * player's home layer for the player). */
int		eng_frame_layer(void);
void		eng_set_frame_layer(int layer);
/* g_M of a layer (global = local + g_M). */
const float	*eng_layer_origin(int layer);

/* ---- clock and random (think-and-time.md 1) ------------------------------- */

double		eng_time(void);		/* f64 s; during a think: its due time */
double		eng_frametime(void);	/* f64 s */
/* hl64_rand_long / hl64_rand_float: include/hl64/pmove.h */

#endif
