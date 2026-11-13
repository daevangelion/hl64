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
/* New Game: global state, skill, the player's inventory reset. */
void	game_new_campaign(void);
/* An HLE slot opened / is closing (its records' game state). */
void	game_map_opened(int slot);
void	game_map_closing(int slot);

/* Spawn a physical record into entity e (allocated by the engine, layer
 * and gid set, origin / angles from the record): read its keys, run the
 * class's spawn. Returns 0, or -1 when the class refuses (e is freed). */
int	game_spawn_record(gid_t16 gid, int e);
/* A logic record goes live (its rec_logic block zeroed). */
void	game_spawn_logic(gid_t16 gid);
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

/* ---- frame (architecture.md 3.2) ------------------------------------------- */

void	game_start_frame(void);		/* step 4, after rec_run_timers */

/* ---- player ---------------------------------------------------------------- */

/* The player entity (slot 1) was created at the spawn point. */
void	game_player_spawn(int e);
/* Usercmd side effects before the move (impulse, use, weapons). */
void	game_player_command(const in_cmd_t *cmd);
void	game_player_postthink(ent_player_t *pl);

/* ---- level state (6.4) ------------------------------------------------------- */

/* Per-class delta of a released entity into buf (<= max); bytes written. */
uint32_t game_dehydrate(int e, void *buf, uint32_t max);
/* Restore it into a freshly hydrated e (after game_spawn_record). */
void	game_rehydrate(int e, const void *buf, uint32_t n);

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
