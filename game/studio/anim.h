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
 * anim.h -- the game's studio animation API: HLSDK dlls/animation.cpp and
 * the CBaseAnimating members of dlls/animating.cpp over the cooked studio
 * info block (include/hl64/studio.h studio_info; docs/formats/studio-format.md)
 * instead of a studiohdr_t. Models are res_t (MODEL kind); entities are
 * slots whose ent_rnd_t holds pev->sequence / frame / animtime / framerate /
 * controller / blending / body. A res_t that is not a resident-able studio
 * model behaves as HLSDK's NULL model pointer (the documented fallbacks).
 *
 * Positions are map-local in the entity's frame (architecture.md 8).
 */
#ifndef HL64_GAME_STUDIO_ANIM_H
#define HL64_GAME_STUDIO_ANIM_H

#include <stdint.h>

#include "hl64/types.h"

/* HLSDK animation.h / engine/studio.h / monsterevent.h values. */
#define ACTIVITY_NOT_AVAILABLE	(-1)
#define STUDIO_LOOPING		0x0001
#define EVENT_SPECIFIC		0
#define EVENT_SCRIPTED		1000
#define EVENT_SHARED		2000
#define EVENT_CLIENT		5000

/* CBaseAnimating members (HLSDK names). The game's CBaseAnimating layout
 * (game/hl/cbase.h) holds exactly this struct; gs_reset_sequence_info and
 * gs_frame_advance read and write it as the HLSDK member functions do. */
typedef struct {
	float		m_flFrameRate;		/* frame units (0..256) per second */
	float		m_flGroundSpeed;
	float		m_flLastEventCheck;	/* f32 s */
	uint8_t		m_fSequenceFinished;
	uint8_t		m_fSequenceLoops;
	uint8_t		pad[2];
} gs_animating_t;			/* 16 */

/* ---- sequences (animation.cpp) ------------------------------------------- */

/* LookupSequence: case-insensitive label match; -1 when absent; 0 for a
 * non-studio model. Spawn time only (string compare). */
int	gs_lookup_sequence(res_t model, const char *name);
/* LookupActivity: weighted random pick among the sequences of activity
 * act (hl64_rand_long, the one game stream); ACTIVITY_NOT_AVAILABLE when
 * none; 0 for a non-studio model. */
int	gs_lookup_activity(res_t model, int act);
/* LookupActivityHeaviest: the sequence of act with the largest weight. */
int	gs_lookup_activity_heaviest(res_t model, int act);
/* Residency only (no game state, no random draw): request the animation
 * chunks of every sequence whose activity is one of acts[0..n) ahead of
 * the think that switches to it (studio_want). */
void	gs_want_activities(res_t model, const int *acts, int n);
/* GetSequenceInfo: frame units per second (256 * fps / (frames - 1), or
 * 256 for one frame) and ground speed (u/s); both 0 for a bad sequence. */
void	gs_sequence_info(res_t model, int seq, float *framerate, float *groundspeed);
/* GetSequenceFlags (STUDIO_LOOPING ...), 0 for a bad sequence. */
int	gs_sequence_flags(res_t model, int seq);
/* Frame count of a sequence (mstudioseqdesc_t numframes), 0 when bad. */
int	gs_sequence_frames(res_t model, int seq);
/* Number of sequences, 0 for a non-studio model. */
int	gs_sequence_count(res_t model);
/* ExtractBbox: the sequence's bounding box; 0 when absent. */
int	gs_extract_bbox(res_t model, int seq, float mins[3], float maxs[3]);

/* GetAnimationEvent: start / end in frame units (0..256 = pev->frame);
 * client events (>= EVENT_CLIENT) are skipped; returns the next index
 * (pass it back to continue) after filling *event / *options, 0 at the
 * end. options: the event's option text (cooked string, resident with
 * the model). */
int	gs_anim_event(res_t model, int seq, float start, float end, int index, int *event,
		const char **options);
/* The resource the option text of the event returned last as index - 1
 * names, cooked by tools/restab: SOUND for "*.wav", SENTENCE for "!NAME";
 * RES_NONE otherwise (SequencePrecache's sound list is these ids). */
res_t	gs_anim_event_res(res_t model, int seq, int index);

/* SetController: controller (bone controller index i, 0..3 and 4 =
 * mouth) setting into ctl[i] (pev->controller); returns the value the
 * setting decodes to. ctl has 4 entries: i == 4 is ignored (the mouth is
 * client side) except for its return value. */
float	gs_set_controller(res_t model, uint8_t ctl[4], int i, float value);
/* SetBlending of sequence seq into blend[i] (pev->blending). */
float	gs_set_blending(res_t model, int seq, uint8_t blend[2], int i, float value);
/* FindTransition (entry / exit node graph); *dir in/out as HLSDK. */
int	gs_find_transition(res_t model, int ending, int goal, int *dir);
/* SetBodygroup / GetBodygroup on pev->body. */
void	gs_set_bodygroup(res_t model, uint8_t *body, int group, int value);
int	gs_get_bodygroup(res_t model, int body, int group);
/* GetEyePosition: the model's eye position (model frame). */
void	gs_eye_position(res_t model, float out[3]);

/* ---- entity pose (pfnGetAttachment / pfnGetBonePosition) ------------------ */

/* Attachment i of entity e at its current pose (map-local, e's frame);
 * returns 0 when e has no such attachment (out = origin). The pose is
 * computed on demand and cached for the frame (shared with the draw and
 * the hitbox source). */
int	gs_attachment(int e, int i, float out[3]);
/* Bone origin of entity e (map-local); 0 when bad. */
int	gs_bone_position(int e, int bone, float out[3]);

/* ---- CBaseAnimating ------------------------------------------------------- */

/* ResetSequenceInfo: frame rate / ground speed of rnd.sequence, loops flag,
 * animtime = time, framerate = 1, finished = 0, last event check = time. */
void	gs_reset_sequence_info(int e, gs_animating_t *a);
/* StudioFrameAdvance(interval): interval 0 = since animtime (<= 0.001 s:
 * animtime := time, returns 0); frame += interval * m_flFrameRate *
 * framerate; wrap (loops) or clamp 0 / 255 outside [0, 256) and set
 * m_fSequenceFinished. Returns the interval used (HLSDK return value). */
float	gs_frame_advance(int e, gs_animating_t *a, float interval);
/* DispatchAnimEvents' window (interval forced to 0.1 as HLSDK): the
 * [start, end) frame window to pass to gs_anim_event, updates
 * m_flLastEventCheck and m_fSequenceFinished. */
void	gs_event_window(int e, gs_animating_t *a, float *start, float *end);
/* SetSequenceBox: size from the sequence bbox rotated by yaw (z 0..1). */
void	gs_set_sequence_box(int e);

/* ---- view model (HUD / weapons) ----------------------------------------------- */

/* The client's frame estimate (StudioEstimateFrame) for a view model
 * playing seq since animtime at framerate from frame (pev units), in pev
 * units: game_pose_t.frame of the studio_view_t the HUD hands the engine. */
float	gs_view_frame(res_t model, int seq, float frame, float animtime, float framerate);

#endif
