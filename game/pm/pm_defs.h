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
// pm_defs.h
/*
 * hl64 game/, Half Life 1 SDK licence (game/LICENSE-HLSDK).
 *
 * HLSDK pm_shared/pm_defs.h with the hl64 changes of decision D2
 * (docs/specs/pmove-host.md 2, 3):
 * - caps: physents 64 (was 600), move-ents 8 (was 64), touch list 32
 *   (was 600, MAX_PHYSENTS), physics info string 32 (was 256,
 *   MAX_PHYSINFO_STRING of pm_info.h), no visents (pm_shared's movement
 *   never reads them);
 * - physent_t keeps HLSDK's leading fields verbatim (the unused studio
 *   model pointer included) and drops the name, animation and mod user
 *   fields;
 * - playermove_t drops the visent list, the texture name buffer and the
 *   file / console / particle / event services pm_shared no longer
 *   calls; PM_TraceTexture returns the
 *   surface's texture-type character (the engine's cooked material
 *   table) instead of a texture name;
 * - trace_t (engine type, PM_TraceModel) is defined here; hull_t is an
 *   opaque engine handle instead of the clipnode / plane view.
 */
#if !defined( PM_DEFSH )
#define PM_DEFSH

#define	MAX_PHYSENTS 64
#define MAX_MOVEENTS 8
#define MAX_PMTOUCH 32
#define MAX_PHYSINFO_STRING 32
#define	MAX_CLIP_PLANES	5

#define PM_NORMAL			0x00000000
#define PM_STUDIO_IGNORE	0x00000001		// Skip studio models
#define PM_STUDIO_BOX		0x00000002		// Use boxes for non-complex studio models (even in traceline)
#define PM_GLASS_IGNORE		0x00000004		// Ignore entities with non-normal rendermode
#define PM_WORLD_ONLY		0x00000008		// Only trace against the world

// PM_PlayerTrace results.
#include "pmtrace.h"

#if !defined ( USERCMD_H )
#include "usercmd.h"
#endif

// physent_t
typedef struct physent_s
{
	int				player;
	vec3_t			origin;               // Model's origin in world coordinates.
	struct model_s	*model;		          // only for bsp models
	struct model_s	*studiomodel;         // SOLID_BBOX, but studio clip intersections.
	vec3_t			mins, maxs;	          // only for non-bsp models
	int				info;		          // For client or server to use to identify (index into edicts or cl_entities)
	vec3_t			angles;               // rotated entities need this info for hull testing to work.

	int				solid;				  // Triggers and func_door type WATER brushes are SOLID_NOT
	int				skin;                 // BSP Contents for such things like fun_door water brushes.
	int				rendermode;			  // So we can ignore glass
} physent_t;

typedef struct
{
	vec3_t	normal;
	float	dist;
} plane_t;

typedef struct
{
	qboolean	allsolid;	// if true, plane is not valid
	qboolean	startsolid;	// if true, the initial point was in a solid area
	qboolean	inopen, inwater;
	float	fraction;		// time completed, 1.0 = didn't hit anything
	vec3_t	endpos;			// final position
	plane_t	plane;			// surface normal at impact
	int		ent;			// physent index the surface is on (hl64: no edict pointer)
	int		hitgroup;		// 0 == generic, non zero is specific body part
} trace_t;

struct model_s;

// hl64: hull handle (PM_HullForBsp). pm_shared reads firstclipnode (the
// walk root); the adapter keeps the engine's hull reference in ref.
typedef struct hull_s
{
	int			firstclipnode;
	int			ref;
} hull_t;

typedef struct playermove_s
{
	int				player_index;  // So we don't try to run the PM_CheckStuck nudging too quickly.
	qboolean		server;        // For debugging, are we running physics code on server side?

	qboolean		multiplayer;   // 1 == multiplayer server
	float			time;          // realtime on host, for reckoning duck timing
	float			frametime;	   // Duration of this frame

	vec3_t			forward, right, up; // Vectors for angles
	// player state
	vec3_t			origin;        // Movement origin.
	vec3_t			angles;        // Movement view angles.
	vec3_t			oldangles;     // Angles before movement view angles were looked at.
	vec3_t			velocity;      // Current movement direction.
	vec3_t			movedir;       // For waterjumping, a forced forward velocity so we can fly over lip of ledge.
	vec3_t			basevelocity;  // Velocity of the conveyor we are standing, e.g.
	
	// For ducking/dead
	vec3_t			view_ofs;      // Our eye position.
	float			flDuckTime;    // Time we started duck
	qboolean		bInDuck;       // In process of ducking or ducked already?
	
	// For walking/falling
	int				flTimeStepSound;  // Next time we can play a step sound
	int				iStepLeft;

	float			flFallVelocity;
	vec3_t			punchangle;

	float			flSwimTime;

	int				flags;         // FL_ONGROUND, FL_DUCKING, etc.
	int				usehull;       // 0 = regular player hull, 1 = ducked player hull, 2 = point hull
	float			gravity;       // Our current gravity and friction.
	float			friction;
	int				oldbuttons;    // Buttons last usercmd
	float			waterjumptime; // Amount of time left in jumping out of water cycle.
	qboolean		dead;          // Are we a dead player?
	int				deadflag;
	int				spectator;     // Should we use spectator physics model?
	int				movetype;      // Our movement type, NOCLIP, WALK, FLY

	int				onground;
	int				waterlevel;
	int				watertype;
	int				oldwaterlevel;

	char			chtexturetype;

	float			maxspeed;
	float			clientmaxspeed; // Player specific maxspeed

	// For mods
	int				iuser1;
	int				iuser2;
	int				iuser3;
	// world state
	// Number of entities to clip against.
	int				numphysent;    
	physent_t		physents[MAX_PHYSENTS];
	// Number of momvement entities (ladders)
	int				nummoveent;
	// just a list of ladders
	physent_t		moveents[MAX_MOVEENTS];	

	// input to run through physics.
	usercmd_t		cmd;

	// Trace results for objects we collided with.
	int				numtouch;
	pmtrace_t		touchindex[MAX_PMTOUCH];

	char			physinfo[ MAX_PHYSINFO_STRING ]; // Physics info string

	struct movevars_s *movevars;
	vec3_t player_mins[ 4 ];
	vec3_t player_maxs[ 4 ];
	
	// Common functions
	const char		*(*PM_Info_ValueForKey) ( const char *s, const char *key );
	int				(*PM_TestPlayerPosition) (float *pos, pmtrace_t *ptrace );
	void			(*Con_DPrintf)( char *fmt, ... );
	void			(*Con_Printf)( char *fmt, ... );
	double			(*Sys_FloatTime)( void );
	void			(*PM_StuckTouch)( int hitent, pmtrace_t *ptraceresult );
	int				(*PM_PointContents) (float *p, int *truecontents /*filled in if this is non-null*/ );
	int				(*PM_TruePointContents) (float *p);
	int				(*PM_HullPointContents) ( struct hull_s *hull, int num, float *p);   
	pmtrace_t		(*PM_PlayerTrace) (float *start, float *end, int traceFlags, int ignore_pe );
	int				(*RandomLong)( int lLow, int lHigh );
	float			(*RandomFloat)( float flLow, float flHigh );
	int				(*PM_GetModelType)( struct model_s *mod );
	void			(*PM_GetModelBounds)( struct model_s *mod, float *mins, float *maxs );
	void			*(*PM_HullForBsp)( physent_t *pe, float *offset );
	float			(*PM_TraceModel)( physent_t *pEnt, float *start, float *end, trace_t *trace );

	// Functions
	// Run functions for this frame?
	qboolean		runfuncs;      
	void			(*PM_PlaySound) ( int channel, const char *sample, float volume, float attenuation, int fFlags, int pitch );
	char			(*PM_TraceTexture) ( int ground, float *vstart, float *vend );
} playermove_t;

#endif
