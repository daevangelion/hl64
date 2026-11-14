/* SPDX-License-Identifier: MIT */
/*
 * game.h -- exports of game/ the engine calls (docs/architecture.md 7.2).
 * Entities are slot indices (include/hl64/ent.h); records are gids
 * (include/hl64/rec.h). Every call runs with the engine's frame layer set
 * to the entity's (or record's) layer, so all positions the game sees are
 * map-local.
 */
#ifndef HL64_GAME_H
#define HL64_GAME_H

#include "hl64/pmove.h"
#include "hl64/types.h"

/* ---- lifetime ---------------------------------------------------------- */

/* Boot, once: the game's tables (dispatch, class table, precache lists). */
void	game_init(void);
/* PROFILE roll-up line `prof game_us avg= max= thinks= uses= fires=`
 * (the game's dispatch time per frame over the window; window totals),
 * then resets the window. */
void	game_prof_line(void);
/* The game's dispatch CPU (outermost calls) and the monster thinks' share
 * of it, us since boot (u32, wraps): the seam-window profiler's game /
 * mon zones (engine/test/seamprof.c). */
void	game_prof_us(uint32_t *dispatch_us, uint32_t *mon_us);
/* New Game: global state, skill, the player's inventory reset. */
void	game_new_campaign(void);
/* An HLE slot opened / is closing (its records' game state). The open
 * declares residency interest for every model the map's records will
 * need (game/hl/precache.c, GoldSrc's level-load precache). */
void	game_map_opened(int slot);
void	game_map_closing(int slot);
/* Residency interest for class cls's precache list (a runtime spawn the
 * map's records do not name: the test route's DROP). */
void	game_precache_class(cls_t cls);

/* Spawn a physical record into entity e (allocated by the engine, layer
 * and gid set, origin / angles from the record): read its keys, run the
 * class's spawn. Returns 0, or -1 when the class refuses (e is freed). */
int	game_spawn_record(gid_t16 gid, int e);
/* A logic record goes live (its rec_logic block zeroed). */
void	game_spawn_logic(gid_t16 gid);
/* A live logic record of a reopened map (its frozen state and timers
 * thawed): the class's Precache on restore (HLSDK DispatchRestore calls
 * Precache after Restore: trigger_auto thinks again). */
void	game_logic_restored(gid_t16 gid);
/* Runtime creation by class (HLSDK CBaseEntity::Create / route DROP):
 * spawn class cls into the fresh entity e at its origin / angles. */
int	game_spawn_class(cls_t cls, int e);

/* ---- dispatch by function id (funcs.def) -------------------------------- */

void	game_think(int e);		/* ent_tm(e)->think */
void	game_touch(int e, int other);	/* ent_tm(e)->touch */
void	game_blocked(int e, int other);	/* ent_tm(e)->blocked */
/* HLSDK Use on entity e (ent_tm(e)->use). */
void	game_use(int e, int activator, int caller, int use_type, float value);
/* Use / timer on a logic record. */
void	game_logic_use(gid_t16 gid, int activator, int caller, int use_type, float value);
void	game_logic_timer(gid_t16 gid, uint16_t arg, int activator);
/* Multisource state for a master name: 1 open, 0 closed, -1 missing. */
int	game_master_state(name_t master, int activator);
/* HLSDK gGlobalState.EntityGetState of a global id (rec_key_global):
 * 0 OFF / 1 ON / 2 DEAD, OFF when unset (route WAIT_GLOBAL). */
int	game_global_state(int id);
/* trigger_endsection fires since New Game (route WAIT_END). */
int	game_end_section(void);
/* A spawner logic record (monstermaker) will spawn nothing more: HLSDK
 * CMonsterMaker::MakeMonster clears its think and use when its count runs
 * out (route WAIT_GONE of a spawned name). 0 for any other record. */
int	game_spawner_done(gid_t16 gid);
/* A spawner logic record whose think will create an entity (a
 * monstermaker with its think set): 1 and the map-local box the child
 * spawns, falls and first moves in, else 0 (any other record). Pure (no
 * game state read but the record's): rec names the box's collision as a
 * prefetch spawn window while the think is pending. */
int	game_spawner_box(gid_t16 gid, float box[6]);

/* ---- frame (architecture.md 3.2) ------------------------------------------- */

void	game_start_frame(void);		/* step 4, after rec_run_timers */
/* GameMon: the `prof mon_us` roll-up line (eng.c rollup, weak). */
void	game_mon_prof(void);
/* GameMon: the `stats N ai:` record (eng.c cmd_stats): AI work since boot. */
void	game_ai_stats(uint32_t id);

/* ---- player ---------------------------------------------------------------- */

/* The player entity (slot 1) was created at the spawn point. */
void	game_player_spawn(int e);
/* Usercmd side effects before the move (impulse, use, weapons). */
void	game_player_command(const in_cmd_t *cmd);
void	game_player_postthink(ent_player_t *pl);

/* ---- level state (6.4) ------------------------------------------------------- */

/* Per-class delta of a released entity into buf (<= max); bytes written,
 * 0 = refused (not quiescent: retried after 0.5 s). */
uint32_t game_dehydrate(int e, void *buf, uint32_t max);
/* Restore it into a freshly hydrated e (after game_spawn_record, or
 * game_spawn_class for a runtime entity). */
void	game_rehydrate(int e, const void *buf, uint32_t n);
/* Seam twin handoff (architecture.md 5.4 req. 3): the engine has copied
 * the components of `from` (map A) into its twin `to` (map B, origin in
 * B's frame); copy the game state (mover phase, toggle state, think,
 * targets as names of B's map). `from` then retires silently. */
void	game_twin_copy(int from, int to);
/* 1 when e crosses with the player if it is in the seam's carry box
 * (HLSDK ObjectCaps FCAP_ACROSS_TRANSITION / FCAP_FORCE_TRANSITION). */
int	game_carry(int e);
/* Slot is closing: cut AI / combat references across the map boundary
 * (enemy, target ent, cine links) as if out of range. */
void	game_cut_refs(int slot);

/* ---- residency inputs (architecture.md 5.3) ---------------------------------- */

/* A moving monster's current route waypoint (e's map-local frame) into
 * goal: 1, else 0 (no route, not a monster). The stream prefetches the
 * collision cells between it and e (the route checks trace them). */
int	game_move_goal(int e, float goal[3]);
/* A trigger_teleport the player can take: the origin its touch gives the
 * player (e's map-local frame) into org, 1; else 0. The stream pins the
 * destinations of those in the eye's open set. */
int	game_teleport_dest(int e, float org[3]);

/* ---- client side: view, HUD, studio (D3) ------------------------------------ */

/* Per rendered frame, after the view: HUD logic and 2D draw. */
void	game_hud_frame(double time, double dt);
void	game_hud_draw(void);
/* Studio pose of entity e for this frame: bone-to-model 3x4 matrices
 * (HLSDK StudioSetupBones) into out (<= max bones); returns the bone
 * count, 0 = not a studio entity. */
int	game_studio_setup_bones(int e, float (*out)[3][4], int max);
/* Client-side animation event of e (HLSDK cl_dll StudioEvent: muzzle
 * flashes, client sounds), called by the engine's studio draw. */
void	game_studio_event(int e, int event, const char *options);
/* pfnGetAttachment for the engine's effects (muzzle flashes): attachment
 * i of entity e at its current pose into out (e's map-local frame);
 * returns 0 (out = e's origin) when e has no such attachment. */
int	game_attachment(int e, int i, float out[3]);
/* Server-side studio pose (GoldSrc's server bone setup: pev->frame as is,
 * no client interpolation, no sequence blend-in): bone-to-world 3x4
 * matrices of the pose *p describes into out (<= max); returns the bone
 * count, 0 for a non-studio model. A pure function of *p (memoised): the
 * hitbox source (trace-and-hulls.md 4.5), attachments, bone positions and
 * the view model's pose (frame from gs_view_frame). */
typedef struct {
	res_t		model;
	uint16_t	sequence;
	float		frame;		/* pev->frame, 0..256 */
	float		origin[3];	/* placement in the caller's frame */
	float		angles[3];	/* pev->angles (pitch negated as HLSDK) */
	uint8_t		controller[4];
	uint8_t		blending[2];
	uint8_t		pad[2];
} game_pose_t;
int	game_studio_pose(const game_pose_t *p, float (*out)[3][4], int max);
/* The same pose for the renderer (temp models, the view model): it reads
 * the game's memo but never fills it, so the game's memo hits are the
 * same on every machine whatever it draws. */
int	game_studio_pose_draw(const game_pose_t *p, float (*out)[3][4], int max);
/* Bones of the poses game_studio_pose computed since boot (memo misses):
 * the game's AI work counters take deltas around a think. */
extern uint32_t	game_studio_pose_bones;
/* An upper bound on the distance from the pose origin of any point of
 * model's hitboxes in any pose game_studio_pose gives for sequence
 * `sequence` (any frame, blend, controller; the bone chains' longest
 * translations plus the hitbox corners); -1 when unknown (no animation,
 * a pose that fails). Memoised per (model, sequence). */
float	game_studio_hitbox_reach(res_t model, int sequence);
/* Chrome texture coordinates (HLSDK mdlviewer StudioModel::Chrome): per
 * bone of a pose (bone-to-view-space matrices, viewer at the origin) the
 * chrome up / right vectors in the bone frame for the view's right axis,
 * then s, t (0..64 texels) of a bone-frame normal. */
void	game_studio_chrome_axes(const float (*bones)[3][4], int nbones, const float vright[3],
		float (*up)[3], float (*right)[3]);
void	game_studio_chrome_st(const float up[3], const float right[3], const float normal[3],
		float st[2]);

/* ---- player move (M2) -------------------------------------------------------- */


/* Once at boot: pm_shared's tables. */
void	game_pm_init(void);
/* Run pm_shared on the engine's move record (pmove-host.md 1 step 5):
 * copies pm into HLSDK playermove_t, PM_Move, writes the player block
 * (pm->pl) and the touch list (pm->touch, pm->ntouch) back. */
void	game_pm_move(phy_pm_t *pm);
/* The player's think before the move (pmove-host.md 1 step 2; HLSDK
 * CBasePlayer::PreThink): M2 keeps FL_INWATER with the water level. */
void	game_player_prethink(ent_player_t *pl);

/* The view (HLSDK client V_CalcRefdef for a first-person player): eye
 * origin = origin + view_ofs + bob, stair-step smoothing, water offset
 * (PM_PointContents), in the player's home-local frame; angles = command
 * angles + punch + movement roll. time and frametime in seconds of game
 * clock. */
typedef struct {
	float	org[3];
	float	ang[3];		/* pitch (down +), yaw, roll; degrees */
} game_view_t;
void	game_calc_view(const ent_player_t *pl, float time, float frametime, game_view_t *out);

#endif
