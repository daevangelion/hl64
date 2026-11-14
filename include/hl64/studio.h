/* SPDX-License-Identifier: MIT */
/*
 * studio.h -- studio models (docs/architecture.md 9.4, D3(b); docs/specs/
 * trace-and-hulls.md 4.5): residency of cooked studio models and their
 * per-sequence animation chunks, the model info the game's animation code
 * reads (sequences, events, attachments, bones, hitboxes, controllers),
 * and the engine draw. The pose maths (HLSDK StudioModelRenderer /
 * animation.cpp) live in game/studio (HLSDK licence) and read the cooked
 * info blocks through this API; the engine never interprets MDL structs.
 * Formats: docs/formats/studio-*.md (cooked by tools/studio).
 */
#ifndef HL64_STUDIO_H
#define HL64_STUDIO_H

#include <stdint.h>

#include "hl64/coll.h"
#include "hl64/types.h"

/* The model's cooked info block (bones, controllers, sequences with
 * their fps / frames / flags / motion / events, attachments, hitboxes,
 * body parts): resident whenever the model is (pinned by live entities);
 * loads synchronously on first use (counted). Layout: the studio format
 * doc. NULL for a non-studio res. */
const void	*studio_info(res_t model, uint32_t *size);
/* Animation chunk of one sequence (D3(b)): resident or NULL; a NULL
 * chunk requests it (prefetch class) and the game holds the last pose. */
const void	*studio_anim(res_t model, int sequence, uint32_t *size);
/* The same chunk now: read synchronously when not resident (counted as a
 * sync load); the server pose (hitboxes, attachments, AI) cannot hold. */
const void	*studio_anim_now(res_t model, int sequence, uint32_t *size);
/* Body combinations of the model (product of body-part model counts;
 * GoldSrc ModelFrames for studio models), 1 when not resident yet. */
int		studio_body_count(res_t model);
/* Interest in a sequence's chunk ahead of use (scripted sequences). */
void		studio_want(res_t model, int sequence);

/* Engine: boot, per-frame residency (live entity models pinned, their
 * current sequences' chunks wanted, STEP interpolation state), the draw
 * of the visible studio entities with bones from game_studio_setup_bones
 * (called by the renderer between the world and the translucent pass),
 * hitbox source for engine/coll (col_set_hitbox_source). */
void		studio_init(void);
void		studio_frame(void);
/* New Game after the entity slots were reset: per-entity STEP and probe
 * state back to boot (the model pools keep their residency). */
void		studio_new_game(void);
/* New Game (load I/O): read the info block of every model with residency
 * interest now; afterwards studio_frame prefetches newly interesting
 * models' infos asynchronously (no mid-game synchronous info read once
 * the interest precedes the use). */
void		studio_preload(void);
/* Models with residency interest whose info is still on its way (the
 * async prefetch has it queued or not yet started); rec holds a newly
 * opened map's hydration until 0 (level-load precache before spawns). */
int		studio_infos_pending(void);
void		studio_draw(double time);
/* The renderer's translucent pass (engine/render/trans.c, back to front):
 * entity e, already found visible by studio_draw, in gpu_mode
 * (engine/render/gpu_studio.h GPU_STUDIO_BLEND / ADD) with alpha. */
void		studio_draw_trans(int e, int gpu_mode, uint8_t alpha);
/* A temporary studio model (gibs, shells, debris: TE_BREAKMODEL /
 * TE_MODEL) at a global placement, sequence 0 frame 0. */
void		studio_draw_temp(res_t model, int body, int skin, const float org_g[3],
			const float ang[3], int gpu_mode, uint8_t alpha);
/* The view model (studio_view_model) after the translucent pass. */
void		studio_draw_view(void);
/* The hitbox source studio_init installs (coll.h col_hitbox_fn): the
 * entity's cooked hitboxes the line start -> end (NULL: none) can meet,
 * on the bones of its server pose (game_studio_hitbox_pose), in its own
 * map-local frame; -1 without posing when the line passes beyond
 * game_studio_hitbox_reach of the entity's origin (or beyond every
 * hitbox's bone chain bound). */
int		studio_hitboxes(const col_ent_t *e, const float start[3], const float end[3],
			const col_hitbox_t **out);

/* View model (HUD / weapons slice; architecture.md 9.4): this frame's
 * first-person model, drawn after the entities in the front of the depth
 * range (gpu_studio_ent_t.view): its chrome runs CPU-skinned, the others
 * from their blocks with the pose's bone matrices. org: the view
 * (game_calc_view) in the player's frame; ang: the model's angles there
 * (an entity's: the pose negates the pitch, so a view model at the view
 * angles has the pitch negated, HLSDK V_CalcGunAngle); the pose is the
 * server pose (game_studio_pose_draw) of (model, sequence, frame,
 * controller, org, ang). Call once per frame before the draw; model
 * RES_NONE = none. */
typedef struct {
	res_t		model;
	uint16_t	sequence;
	float		frame;		/* pev units, gs_view_frame */
	float		org[3], ang[3];
	uint8_t		body, skin;
	uint8_t		controller[4];
	uint8_t		pad[2];
} studio_view_t;
void		studio_view_model(const studio_view_t *v);
#ifdef HL64_TEST
/* Test builds: the last studio_view_model's bone `bone` posed at the
 * camera (ren_cam), in the camera's frame (rows forward, right, up;
 * column 3 the position from the eye). 0, -1 none / not posed. */
int		studio_view_bone_cam(int bone, float out[3][4]);
#endif

typedef struct {
	uint32_t	models, pool_used, pool_peak, anims, sync_loads;
	uint32_t	drawn, bones_us, draw_us, us_max;
	uint32_t	gpu_us, view_us;	/* draw_us parts: the GPU submit of the entities, the view model */
	uint32_t	frames, us_sum, culled, pending, nofit, dropped;
	uint32_t	sync_play;	/* since boot: sync reads outside load I/O */
	uint32_t	want_peak;	/* since boot: pinned + in-use bytes (live entities' and the
					 * view model's info + mesh, animations in use, reads in
					 * flight) at once; pool_peak is what was resident */
	uint32_t	soft;		/* since boot: soft blocks freed after their grace */
} studio_stats_t;
/* Counters since the last reset (pool fields: now / peak; sync_play,
 * want_peak and soft are never reset). */
void		studio_stats(studio_stats_t *s, int reset);

#ifdef HL64_HOST
/* Host tests without a resource table: bind MODEL index idx to cooked
 * files (paths under the content root). */
void		studio_test_bind(int idx, const char *s64, const char *a64);
#endif

#endif
