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
// hl64 GoldSrc probe (Half Life 1 SDK LICENSE, test/oracle/LICENSE-HLSDK).
//
// P-tracktrain: the map's func_tracktrain entities and their path_tracks
// stay live (not frozen). Scenario 0 `spawn` is recorded from the first
// StartFrame after map spawn (frames 0..60, Probe_TrainFrame); `drive` (the
// fake client on the first train takes control with +use and steps the
// speed up with +forward) and `toggle` (the trains fired by targetname with
// USE_TOGGLE, no client) run when the capture starts. Thinks are seen by
// wrapping each train's m_pfnThink (the wrapper calls the original and
// re-wraps whatever the think set); sounds by wrapping the DLL's copies of
// pfnEmitSound and pfnPlaybackEvent.

#include "probe_int.h"
#include "trains.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TT_MAX 8
#define TT_EARLY_FRAMES 61		// spawn: state lines 0..60
#define TT_TAIL 100				// frames recorded after every train came to rest (1 s)

static edict_t *s_tt[TT_MAX];
static int s_ntt;
static BASEPTR s_orig[TT_MAX];	// the think each wrapped train really has
static FILE *s_sink;			// events go here: the early memstream, then the capture file
static int s_evFrame;			// state line index that first shows an event
static bool s_earlyStarted;
static FILE *s_early;
static char *s_earlyBuf;
static size_t s_earlyLen;
static int s_earlyN;

static void ( *s_emitSound )( edict_t *, int, const char *, float, float, int, int );
static void ( *s_playbackEvent )( int, const edict_t *, unsigned short, float, float *, float *, float, float, int, int, int, int );

class CProbeThinkWrap : public CBaseEntity
{
public:
	void WrapThink( void );
};
static const BASEPTR s_wrap = static_cast<BASEPTR>( &CProbeThinkWrap::WrapThink );

static int TT_Slot( const edict_t *e )
{
	for ( int k = 0; k < s_ntt; k++ )
		if ( s_tt[k] == e ) return k;
	return -1;
}

static int TT_Case( void )
{
	return s_sink == s_early ? 0 : g_scenId;
}

static CFuncTrackTrain *TT_Train( int k )
{
	return (CFuncTrackTrain *)CBaseEntity::Instance( s_tt[k] );
}

static BASEPTR TT_Think( int k )
{
	CBaseEntity *e = TT_Train( k );
	return e->m_pfnThink == s_wrap ? s_orig[k] : e->m_pfnThink;
}

static const char *TT_FnName( BASEPTR p )
{
	if ( !p ) return "none";
	if ( p == static_cast<BASEPTR>( &CFuncTrackTrain::Next ) ) return "Next";
	if ( p == static_cast<BASEPTR>( &CFuncTrackTrain::Find ) ) return "Find";
	if ( p == static_cast<BASEPTR>( &CFuncTrackTrain::DeadEnd ) ) return "DeadEnd";
	if ( p == static_cast<BASEPTR>( &CFuncTrackTrain::NearestPath ) ) return "NearestPath";
	return "other";
}

static const char *TT_PathName( CFuncTrackTrain *t )
{
	return t->m_ppath ? STRING( t->m_ppath->pev->targetname ) : "";
}

// Wrap while events are recorded, unwrap otherwise.
static void TT_Wrap( void )
{
	for ( int k = 0; k < s_ntt; k++ )
	{
		CBaseEntity *e = TT_Train( k );
		if ( !e ) continue;
		if ( s_sink && e->m_pfnThink && e->m_pfnThink != s_wrap )
		{
			s_orig[k] = e->m_pfnThink;
			e->m_pfnThink = s_wrap;
		}
		else if ( !s_sink && e->m_pfnThink == s_wrap )
			e->m_pfnThink = s_orig[k];
	}
}

// One line per think, written after it ran (events the think caused come first).
void CProbeThinkWrap::WrapThink( void )
{
	int k = TT_Slot( edict() );
	BASEPTR orig = k >= 0 ? s_orig[k] : NULL;
	m_pfnThink = orig;
	char in[512];
	snprintf( in, sizeof( in ), "{\"ev\":\"think\",\"case\":%d,\"f\":%d,\"t\":%.9g,\"ft\":%.9g,\"i\":%d,\"fn\":\"%s\",\"lt\":%.9g,\"nt_in\":%.9g,\"fl_in\":%d,\"o_in\":" VF ",",
		s_sink ? TT_Case() : -1, s_evFrame, (double)gpGlobals->time, (double)gpGlobals->frametime, entindex(), TT_FnName( orig ),
		(double)pev->ltime, (double)pev->nextthink, pev->flags, VA( pev->origin ) );
	if ( orig ) ( this->*orig )();
	if ( s_sink )
	{
		CFuncTrackTrain *t = (CFuncTrackTrain *)this;
		fprintf( s_sink, "%s\"nt\":%.9g,\"fl\":%d,\"next_fn\":\"%s\",\"speed\":%.9g,\"path\":\"%s\",\"sf\":%d,\"o\":" VF ",\"a\":" VF ",\"v\":" VF ",\"av\":" VF "}\n",
			in, (double)pev->nextthink, pev->flags, TT_FnName( m_pfnThink ), (double)pev->speed, TT_PathName( t ), pev->spawnflags,
			VA( pev->origin ), VA( pev->angles ), VA( pev->velocity ), VA( pev->avelocity ) );
	}
	if ( k >= 0 && s_sink && m_pfnThink )
	{
		s_orig[k] = m_pfnThink;
		m_pfnThink = s_wrap;
	}
}

static void TT_EmitSound( edict_t *e, int ch, const char *sample, float vol, float attn, int fl, int pitch )
{
	if ( s_sink && e && ( TT_Slot( e ) >= 0 || ( e->v.flags & FL_CLIENT ) ) )
		fprintf( s_sink, "{\"ev\":\"sound\",\"case\":%d,\"f\":%d,\"t\":%.9g,\"i\":%d,\"ch\":%d,\"sample\":\"%s\",\"vol\":%.9g,\"attn\":%.9g,\"fl\":%d,\"pitch\":%d}\n",
			TT_Case(), s_evFrame, (double)gpGlobals->time, ENTINDEX( e ), ch, sample ? sample : "", (double)vol, (double)attn, fl, pitch );
	s_emitSound( e, ch, sample, vol, attn, fl, pitch );
}

// CFuncTrackTrain::UpdateSound / StopSound: iparam1 = sound (3 bits) << 12 |
// pitch / 10 (6 bits) << 6 | volume * 40 (6 bits); bparam1 1 = stop.
static void TT_PlaybackEvent( int flags, const edict_t *inv, unsigned short idx, float delay, float *org, float *ang,
	float fp1, float fp2, int ip1, int ip2, int bp1, int bp2 )
{
	if ( s_sink && inv && TT_Slot( inv ) >= 0 )
		fprintf( s_sink, "{\"ev\":\"event\",\"case\":%d,\"f\":%d,\"t\":%.9g,\"i\":%d,\"flags\":%d,\"index\":%d,\"ip1\":%d,\"bp1\":%d,"
			"\"sound\":%d,\"pitch\":%d,\"vol\":%.9g}\n",
			TT_Case(), s_evFrame, (double)gpGlobals->time, ENTINDEX( (edict_t *)inv ), flags, idx, ip1, bp1,
			( ip1 >> 12 ) & 7, ( ( ip1 >> 6 ) & 63 ) * 10, ( ip1 & 63 ) / 40.0 );
	s_playbackEvent( flags, inv, idx, delay, org, ang, fp1, fp2, ip1, ip2, bp1, bp2 );
}

void Probe_TrainInit( void )
{
	s_emitSound = g_engfuncs.pfnEmitSound;
	g_engfuncs.pfnEmitSound = TT_EmitSound;
	s_playbackEvent = g_engfuncs.pfnPlaybackEvent;
	g_engfuncs.pfnPlaybackEvent = TT_PlaybackEvent;
}

void Probe_TrainPrecache( void )
{
	if ( s_early ) fclose( s_early );
	free( s_earlyBuf );
	s_early = NULL;
	s_earlyBuf = NULL;
	s_earlyLen = 0;
	s_earlyN = 0;
	s_earlyStarted = false;
	s_sink = NULL;
	s_ntt = 0;
}

// Train members not in entvars: {"i","model","speed","impulse","path",...}
static void PrTT( FILE *f, int k )
{
	CFuncTrackTrain *t = TT_Train( k );
	entvars_t *v = t->pev;
	fprintf( f, "{\"i\":%d,\"model\":\"%s\",\"speed\":%.9g,\"impulse\":%d,\"path\":\"%s\",\"m_speed\":%.9g,\"m_dir\":%.9g,"
		"\"m_height\":%.9g,\"m_length\":%.9g,\"m_oldSpeed\":%.9g,\"snd\":%d,\"sf\":%d,\"think\":\"%s\",\"always\":%d}",
		ENTINDEX( s_tt[k] ), v->model ? STRING( v->model ) : "", (double)v->speed, v->impulse, TT_PathName( t ),
		(double)t->m_speed, (double)t->m_dir, (double)t->m_height, (double)t->m_length, (double)t->m_oldSpeed,
		t->m_soundPlaying, v->spawnflags, TT_FnName( TT_Think( k ) ), ( v->flags & FL_ALWAYSTHINK ) ? 1 : 0 );
}

// {"sc","f","t","ft","e":[dyn…]} and {"sc","f","tt":[train members…]}
static void PrTrainState( FILE *f, int sc, int frame )
{
	fprintf( f, "{\"sc\":%d,\"f\":%d,\"t\":%.9g,\"ft\":%.9g,\"e\":[", sc, frame, (double)gpGlobals->time, (double)gpGlobals->frametime );
	for ( int k = 0; k < s_ntt; k++ )
	{
		if ( k ) fputc( ',', f );
		Probe_PrDyn( f, s_tt[k] );
	}
	fprintf( f, "]}\n{\"sc\":%d,\"f\":%d,\"tt\":[", sc, frame );
	for ( int k = 0; k < s_ntt; k++ )
	{
		if ( k ) fputc( ',', f );
		PrTT( f, k );
	}
	fprintf( f, "]}\n" );
}

// Every StartFrame: spawn recording (the first TT_EARLY_FRAMES frames from
// map load), then the think wrappers follow the event sink.
void Probe_TrainFrame( void )
{
	if ( !s_earlyStarted )
	{
		s_earlyStarted = true;
		for ( edict_t *e = FIND_ENTITY_BY_CLASSNAME( NULL, "func_tracktrain" ); !FNullEnt( e ) && s_ntt < TT_MAX;
			e = FIND_ENTITY_BY_CLASSNAME( e, "func_tracktrain" ) )
			if ( CBaseEntity::Instance( e ) ) s_tt[s_ntt++] = e;
		if ( s_ntt ) s_early = open_memstream( &s_earlyBuf, &s_earlyLen );
		if ( s_early )
		{
			s_sink = s_early;
			fprintf( s_early, "{\"scenario\":0,\"name\":\"spawn\",\"frames\":%d,\"t\":%.9g}\n{\"sc\":0,\"ents\":[",
				TT_EARLY_FRAMES - 1, (double)gpGlobals->time );
			for ( int k = 0; k < s_ntt; k++ )
			{
				if ( k ) fputc( ',', s_early );
				Probe_PrEnt( s_early, s_tt[k] );
			}
			fprintf( s_early, "]}\n" );
		}
	}
	if ( s_early && s_sink == s_early && s_earlyN < TT_EARLY_FRAMES )
	{
		PrTrainState( s_early, 0, s_earlyN );
		s_evFrame = ++s_earlyN;
		if ( s_earlyN == TT_EARLY_FRAMES ) s_sink = NULL;	// physics of frame 60 is not recorded
	}
	TT_Wrap();
}

// ---------------------------------------------------------------- capture

enum { TT_SPAWN, TT_DRIVE, TT_TOGGLE };

// The trains' state when the capture starts (they rest at their first
// path_track since spawn); `toggle` starts from it again.
struct TTSnap
{
	Vector o, a, v, av;
	float speed, ltime, nextthink, oldSpeed, dir;
	int flags, spawnflags, sound;
	CPathTrack *path;
	BASEPTR think;
};
static TTSnap s_snap[TT_MAX];
static ScenRun s_run;
static FILE *s_f;
static int s_restAt;			// frame at which every train came to rest after it was started (-1)
static int s_startFrame;		// frame of the first start (drive: first +forward, toggle: the fire)
static float s_prevSpeed[TT_MAX];
static int s_prevPflags, s_prevITrain;
static char s_prevPath[TT_MAX][32];

#define DRIVE_USE 20
#define DRIVE_FWD1 50
static const int s_driveFwd[] = { DRIVE_FWD1, 150, 170, 190 };	// 4 notches of 0.25: 50, 100, 150, 200
#define TOGGLE_FIRE 10

static void Snap( int k )
{
	CFuncTrackTrain *t = TT_Train( k );
	entvars_t *v = t->pev;
	TTSnap &s = s_snap[k];
	s.o = v->origin; s.a = v->angles; s.v = v->velocity; s.av = v->avelocity;
	s.speed = v->speed; s.ltime = v->ltime; s.nextthink = v->nextthink; s.flags = v->flags; s.spawnflags = v->spawnflags;
	s.oldSpeed = t->m_oldSpeed; s.dir = t->m_dir; s.sound = t->m_soundPlaying; s.path = t->m_ppath; s.think = TT_Think( k );
}

static void Restore( int k )
{
	CFuncTrackTrain *t = TT_Train( k );
	entvars_t *v = t->pev;
	const TTSnap &s = s_snap[k];
	v->angles = s.a; v->velocity = s.v; v->avelocity = s.av;
	v->speed = s.speed; v->ltime = s.ltime; v->nextthink = s.nextthink; v->flags = s.flags; v->spawnflags = s.spawnflags;
	t->m_oldSpeed = s.oldSpeed; t->m_dir = s.dir; t->m_soundPlaying = s.sound; t->m_ppath = s.path; t->m_pfnThink = s.think;
	UTIL_SetOrigin( v, s.o );
}

static bool TrainsAtRest( void )
{
	for ( int k = 0; k < s_ntt; k++ )
	{
		const entvars_t *v = &s_tt[k]->v;
		if ( v->speed != 0 || v->velocity != g_vecZero || v->avelocity != g_vecZero || v->nextthink > 0 ) return false;
	}
	return true;
}

static void Changes( int f, CBasePlayer *pl )
{
	for ( int k = 0; k < s_ntt; k++ )
	{
		CFuncTrackTrain *t = TT_Train( k );
		const char *path = TT_PathName( t );
		if ( t->pev->speed != s_prevSpeed[k] || strcmp( path, s_prevPath[k] ) )
			fprintf( s_f, "{\"sc\":%d,\"ev\":\"train\",\"f\":%d,\"i\":%d,\"speed\":%.9g,\"was\":%.9g,\"path\":\"%s\",\"was_path\":\"%s\",\"sf\":%d}\n",
				g_scenId, f, ENTINDEX( s_tt[k] ), (double)t->pev->speed, (double)s_prevSpeed[k], path, s_prevPath[k], t->pev->spawnflags );
		s_prevSpeed[k] = t->pev->speed;
		strncpy( s_prevPath[k], path, sizeof( s_prevPath[k] ) - 1 );
	}
	if ( pl && ( (int)pl->m_afPhysicsFlags != s_prevPflags || pl->m_iTrain != s_prevITrain ) )
	{
		fprintf( s_f, "{\"sc\":%d,\"ev\":\"player\",\"f\":%d,\"pflags\":%d,\"was\":%d,\"ontrain\":%d,\"itrain\":%d,\"was_itrain\":%d}\n",
			g_scenId, f, pl->m_afPhysicsFlags, s_prevPflags, ( pl->m_afPhysicsFlags & PFLAG_ONTRAIN ) ? 1 : 0, pl->m_iTrain, s_prevITrain );
		s_prevPflags = pl->m_afPhysicsFlags;
		s_prevITrain = pl->m_iTrain;
	}
}

static void ResetChanges( CBasePlayer *pl )
{
	for ( int k = 0; k < s_ntt; k++ )
	{
		CFuncTrackTrain *t = TT_Train( k );
		s_prevSpeed[k] = t->pev->speed;
		strncpy( s_prevPath[k], TT_PathName( t ), sizeof( s_prevPath[k] ) - 1 );
	}
	s_prevPflags = pl ? (int)pl->m_afPhysicsFlags : 0;
	s_prevITrain = pl ? pl->m_iTrain : 0;
	s_restAt = -1;
}

// After every train that was started came to rest: TT_TAIL more frames.
static void CheckEnd( int f )
{
	if ( f <= s_startFrame + 1 ) return;
	if ( s_restAt < 0 && TrainsAtRest() )
	{
		s_restAt = f;
		fprintf( s_f, "{\"sc\":%d,\"ev\":\"rest\",\"f\":%d,\"t\":%.9g}\n", g_scenId, f, (double)gpGlobals->time );
	}
	if ( s_restAt >= 0 && f >= s_restAt + TT_TAIL )
	{
		fprintf( s_f, "{\"sc\":%d,\"ev\":\"end\",\"f\":%d}\n", g_scenId, f );
		g_scenStop = true;
	}
}

static void TT_Tracks( void )
{
	for ( int k = 0; k < s_ntt; k++ ) Scen_Track( s_tt[k] );
}

static void PrPlayer( int f, edict_t *bot, unsigned short buttons, float fwd )
{
	CBasePlayer *pl = (CBasePlayer *)CBaseEntity::Instance( bot );
	CFuncTrackTrain *t = TT_Train( 0 );
	Vector rel = bot->v.origin - t->pev->origin, fw, rt, up;
	g_engfuncs.pfnAngleVectors( t->pev->angles, fw, rt, up );
	Vector local( DotProduct( rel, fw ), -DotProduct( rel, rt ), DotProduct( rel, up ) );
	fprintf( s_f, "{\"sc\":%d,\"f\":%d,\"cmd\":{\"b\":%d,\"fm\":%.9g},\"pl\":{\"o\":" VF ",\"v\":" VF ",\"bv\":" VF ",\"fl\":%d,"
		"\"onground\":%d,\"ontrain\":%d,\"ge\":%d,\"itrain\":%d,\"pflags\":%d,\"rel\":" VF ",\"local\":" VF ",\"oncontrols\":%d}}\n",
		g_scenId, f, buttons, (double)fwd, VA( bot->v.origin ), VA( bot->v.velocity ), VA( bot->v.basevelocity ), bot->v.flags,
		( bot->v.flags & FL_ONGROUND ) ? 1 : 0, ( bot->v.flags & FL_ONTRAIN ) ? 1 : 0, Probe_Idx( bot->v.groundentity ),
		pl->m_iTrain, pl->m_afPhysicsFlags, VA( rel ), VA( local ),
		!( t->pev->spawnflags & SF_TRACKTRAIN_NOCONTROL ) &&
		local.x >= t->m_controlMins.x && local.y >= t->m_controlMins.y && local.z >= t->m_controlMins.z &&
		local.x <= t->m_controlMaxs.x && local.y <= t->m_controlMaxs.y && local.z <= t->m_controlMaxs.z );
}

static void TrainLines( int f )
{
	fprintf( s_f, "{\"sc\":%d,\"f\":%d,\"tt\":[", g_scenId, f );
	for ( int k = 0; k < s_ntt; k++ )
	{
		if ( k ) fputc( ',', s_f );
		PrTT( s_f, k );
	}
	fprintf( s_f, "]}\n" );
}

// drive: the fake client standing on the first train (a hull trace down
// from above its origin), +use at frame 20, +forward edges at frames 50,
// 150, 170, 190 (one frame each, forwardmove 400 as cl_forwardspeed).
static bool DriveSetup( void )
{
	s_evFrame = 0;
	edict_t *bot = Probe_Bot();
	if ( !bot ) return Scen_Skip( "no fake client" );
	CFuncTrackTrain *t = TT_Train( 0 );
	entvars_t *v = t->pev;
	Vector top( v->origin.x, v->origin.y, v->origin.z + v->maxs.z + 80 );
	Vector bottom( v->origin.x, v->origin.y, v->origin.z + v->mins.z - 80 );
	// line profile above the origin: every train surface on the way down
	fprintf( s_f, "{\"sc\":%d,\"profile\":[", g_scenId );
	Vector p = top;
	for ( int n = 0; n < 8; n++ )
	{
		TraceResult tr;
		TRACE_LINE( p, bottom, ignore_monsters, NULL, &tr );
		fprintf( s_f, "%s{", n ? "," : "" );
		Probe_PrTrace( s_f, tr );
		fprintf( s_f, "}" );
		if ( tr.flFraction == 1.0f || tr.fAllSolid ) break;
		// continue from the first point under the hit that a short line trace starts out of solid
		p = tr.vecEndPos;
		do
		{
			p.z -= 1;
			TRACE_LINE( p, p - Vector( 0, 0, 0.5f ), ignore_monsters, NULL, &tr );
		} while ( p.z > bottom.z && tr.fStartSolid );
		if ( p.z <= bottom.z ) break;
	}
	fprintf( s_f, "]}\n" );
	TraceResult tr;
	TRACE_HULL( top, bottom, ignore_monsters, human_hull, NULL, &tr );
	if ( tr.fStartSolid || tr.flFraction == 1.0f || tr.pHit != s_tt[0] || tr.vecPlaneNormal.z < 0.7f )
	{
		fprintf( s_f, "{\"sc\":%d,\"floor_trace\":{", g_scenId );
		Probe_PrTrace( s_f, tr );
		fprintf( s_f, "}}\n" );
		return Scen_Skip( "no floor on the train above its origin" );
	}
	Probe_BotReset( tr.vecEndPos, SOLID_SLIDEBOX );
	CBasePlayer *pl = (CBasePlayer *)CBaseEntity::Instance( bot );
	pl->m_afPhysicsFlags &= ~PFLAG_ONTRAIN;
	fprintf( s_f, "{\"sc\":%d,\"train\":%d,\"model\":\"%s\",\"floor_trace\":{", g_scenId, ENTINDEX( s_tt[0] ), STRING( v->model ) );
	Probe_PrTrace( s_f, tr );
	fprintf( s_f, "},\"bot\":" VF ",\"use\":%d,\"forward\":[%d,%d,%d,%d],\"yaw\":270,\"msec\":10,\"fake_client\":%d}\n",
		VA( bot->v.origin ), DRIVE_USE, s_driveFwd[0], s_driveFwd[1], s_driveFwd[2], s_driveFwd[3], ENTINDEX( bot ) );
	TT_Tracks();
	Scen_Track( bot );
	ResetChanges( pl );
	s_startFrame = DRIVE_FWD1;
	return true;
}

static void DriveTick( int f )
{
	s_evFrame = f + 1;
	edict_t *bot = Probe_Bot();
	CBasePlayer *pl = (CBasePlayer *)CBaseEntity::Instance( bot );
	TrainLines( f );
	Changes( f, pl );
	unsigned short b = 0;
	float fwd = 0;
	if ( f == DRIVE_USE ) b = IN_USE;
	for ( unsigned i = 0; i < sizeof( s_driveFwd ) / sizeof( s_driveFwd[0] ); i++ )
		if ( f == s_driveFwd[i] ) { b = IN_FORWARD; fwd = 400; }
	Probe_BotMove( Vector( 0, 270, 0 ), fwd, 0, 0, b, 10 );
	PrPlayer( f, bot, b, fwd );
	CheckEnd( f );
	TT_Wrap();
}

// toggle: no client; every train back to the capture-start snapshot, then
// FireTargets( train targetname, USE_TOGGLE ) at frame 10 as a trigger does.
static bool ToggleSetup( void )
{
	s_evFrame = 0;
	edict_t *bot = Probe_Bot();
	if ( bot )
	{
		CBasePlayer *pl = (CBasePlayer *)CBaseEntity::Instance( bot );
		pl->m_afPhysicsFlags &= ~PFLAG_ONTRAIN;
		pl->m_iTrain = 0;
	}
	for ( int k = 0; k < s_ntt; k++ ) Restore( k );
	fprintf( s_f, "{\"sc\":%d,\"restored\":\"capture-start snapshot\",\"fire\":%d,\"target\":\"%s\",\"use_type\":%d}\n",
		g_scenId, TOGGLE_FIRE, STRING( s_tt[0]->v.targetname ), (int)USE_TOGGLE );
	TT_Tracks();
	ResetChanges( NULL );
	s_startFrame = TOGGLE_FIRE;
	TT_Wrap();
	return true;
}

static void ToggleTick( int f )
{
	s_evFrame = f + 1;
	TrainLines( f );
	Changes( f, NULL );
	if ( f == TOGGLE_FIRE )
	{
		fprintf( s_f, "{\"sc\":%d,\"ev\":\"fire\",\"f\":%d,\"t\":%.9g,\"target\":\"%s\"}\n", g_scenId, f, (double)gpGlobals->time,
			STRING( s_tt[0]->v.targetname ) );
		FireTargets( STRING( s_tt[0]->v.targetname ), NULL, NULL, USE_TOGGLE, 0 );
	}
	CheckEnd( f );
	TT_Wrap();
}

static const ScenDef s_ttdefs[] =
{
	{ "spawn", TT_EARLY_FRAMES - 1, NULL, NULL },	// written from the spawn recording
	{ "drive", 3000, DriveSetup, DriveTick },
	{ "toggle", 2000, ToggleSetup, ToggleTick },
};

bool Cap_PTracktrain( int step )
{
	if ( step == 0 )
	{
		s_f = Probe_Open( "physics-movetypes", "P-tracktrain", NULL );
		if ( !s_f ) return true;
		if ( !s_ntt )
		{
			fprintf( s_f, "{\"scenario\":0,\"skipped\":\"no func_tracktrain\"}\n" );
			Probe_Close( s_f );
			return true;
		}
		fprintf( s_f, "{\"trains\":[" );
		for ( int k = 0; k < s_ntt; k++ )
		{
			CFuncTrackTrain *t = TT_Train( k );
			edict_t *e = s_tt[k];
			char tn[48];
			int lump = Probe_LumpIndex( e, tn, sizeof( tn ) );
			fprintf( s_f, "%s{\"i\":%d,\"model\":\"%s\",\"lump\":%d,\"targetname\":\"%s\",\"target\":\"%s\",\"oldorigin\":" VF ","
				"\"m_speed\":%.9g,\"m_height\":%.9g,\"m_length\":%.9g,\"m_startSpeed\":%.9g,\"m_flVolume\":%.9g,\"m_flBank\":%.9g,\"m_sounds\":%d,"
				"\"noise\":\"%s\",\"dmg\":%.9g,\"control_mins\":" VF ",\"control_maxs\":" VF "}",
				k ? "," : "", ENTINDEX( e ), STRING( e->v.model ), lump, STRING( e->v.targetname ), STRING( e->v.target ),
				VA( e->v.oldorigin ), (double)t->m_speed, (double)t->m_height, (double)t->m_length, (double)t->m_startSpeed,
				(double)t->m_flVolume, (double)t->m_flBank, t->m_sounds, e->v.noise ? STRING( e->v.noise ) : "", (double)e->v.dmg,
				VA( t->m_controlMins ), VA( t->m_controlMaxs ) );
		}
		fprintf( s_f, "]}\n{\"paths\":[" );
		bool first = true;
		for ( edict_t *e = FIND_ENTITY_BY_CLASSNAME( NULL, "path_track" ); !FNullEnt( e ); e = FIND_ENTITY_BY_CLASSNAME( e, "path_track" ) )
		{
			CPathTrack *p = (CPathTrack *)CBaseEntity::Instance( e );
			if ( !p ) continue;
			fprintf( s_f, "%s{\"i\":%d,\"targetname\":\"%s\",\"o\":" VF ",\"sf\":%d,\"speed\":%.9g,\"next\":\"%s\",\"prev\":\"%s\",\"message\":\"%s\",\"netname\":\"%s\"}",
				first ? "" : ",", ENTINDEX( e ), STRING( e->v.targetname ), VA( e->v.origin ), e->v.spawnflags, (double)e->v.speed,
				p->m_pnext ? STRING( p->m_pnext->pev->targetname ) : "", p->m_pprevious ? STRING( p->m_pprevious->pev->targetname ) : "",
				e->v.message ? STRING( e->v.message ) : "", e->v.netname ? STRING( e->v.netname ) : "" );
			first = false;
		}
		fprintf( s_f, "]}\n" );
		if ( s_early && s_earlyN == TT_EARLY_FRAMES )
		{
			fflush( s_early );
			fwrite( s_earlyBuf, 1, s_earlyLen, s_f );
		}
		else
			fprintf( s_f, "{\"scenario\":0,\"skipped\":\"spawn frames not recorded\"}\n" );
		if ( s_early ) fclose( s_early );
		free( s_earlyBuf );
		s_early = NULL;
		s_earlyBuf = NULL;
		// the trains as the capture finds them (resting since spawn): toggle restarts from here
		fprintf( s_f, "{\"capture_start\":{\"t\":%.9g,\"tt\":[", (double)gpGlobals->time );
		for ( int k = 0; k < s_ntt; k++ )
		{
			Snap( k );
			if ( k ) fputc( ',', s_f );
			Probe_PrDyn( s_f, s_tt[k] );
		}
		fprintf( s_f, "]}}\n" );
		Scen_BeginAt( s_run, s_ttdefs, sizeof( s_ttdefs ) / sizeof( s_ttdefs[0] ), s_f, TT_DRIVE );
		s_sink = s_f;
		TT_Wrap();
		return false;
	}
	if ( !Scen_Step( s_run ) ) return false;
	s_sink = NULL;
	TT_Wrap();
	edict_t *bot = Probe_Bot();
	if ( bot ) ( (CBasePlayer *)CBaseEntity::Instance( bot ) )->m_afPhysicsFlags &= ~PFLAG_ONTRAIN;
	Probe_Close( s_f );
	s_f = NULL;
	return true;
}
