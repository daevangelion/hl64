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
// PM-bot runs for docs/specs/pmove-host.md: the fake client is driven
// through pfnRunPlayerMove (the engine's player-move path: SV_RunCmd,
// physents, PM_Move of this DLL, links and touches) with scripted command
// streams from start points found in the map (flat floor, stairs, ladder,
// water, slope). Every frame logs the command and the resulting player
// state; the pmove block after PM_Move adds hull, physents and touches.

#include "probe_int.h"
#include "in_buttons.h"
#include <math.h>
#include <string.h>

static ProbeRng s_rng;
static FILE *s_f;

enum { K_STAND, K_WALK, K_RUN, K_STRAFE, K_JUMP, K_SJUMP, K_DUCK, K_DUCKJUMP, K_STAIRS, K_LADDER, K_WATER, K_SLOPE, K_LADDER_LEVEL, K_STEP18, K_STEP19 };
static const char *s_kindName[] = { "stand", "walk", "run", "strafe", "jump", "standjump", "duck", "duckjump",
	"stairs", "ladder", "water", "slope", "ladder_level", "step18", "step19" };

struct Run { int kind; int msec; Vector start; float yaw; char where[160]; };
static Run s_runs[64];
static int s_nruns;
static const int RUN_FRAMES = 300;

// ---------------------------------------------------------------- finders

static Vector YawDir( float yaw )
{
	float r = yaw * ( M_PI / 180.0 );
	return Vector( cosf( r ), sinf( r ), 0 );
}

static float Clear( const Vector &o, float yaw, float dist )
{
	TraceResult tr;
	TRACE_HULL( o, o + YawDir( yaw ) * dist, ignore_monsters, 1, NULL, &tr );
	return tr.fStartSolid ? 0 : tr.flFraction * dist;
}

// Standing origins on floors: random open points dropped onto the floor below.
static int FloorCandidates( Vector *out, Vector *normals, int max )
{
	int n = 0;
	edict_t *st = FIND_ENTITY_BY_CLASSNAME( NULL, "info_player_start" );
	if ( st && Probe_FloorBelow( st->v.origin + Vector( 0, 0, 1 ), 1, 256, out[n], normals[n] ) ) n++;
	for ( int t = 0; t < 400000 && n < max; t++ )
	{
		Vector p( s_rng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), s_rng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
			s_rng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
		if ( POINT_CONTENTS( p ) != CONTENTS_EMPTY ) continue;
		TraceResult tr;
		TRACE_HULL( p, p, ignore_monsters, 1, NULL, &tr );
		if ( tr.fStartSolid ) continue;
		Vector o, nrm;
		if ( !Probe_FloorBelow( p, 1, 1024, o, nrm ) ) continue;
		if ( POINT_CONTENTS( o ) != CONTENTS_EMPTY ) continue;
		out[n] = o;
		normals[n++] = nrm;
	}
	return n;
}

static void AddRun( int kind, int msec, const Vector &start, float yaw, const char *where )
{
	if ( s_nruns >= 64 ) return;
	Run &r = s_runs[s_nruns++];
	r.kind = kind;
	r.msec = msec;
	r.start = start;
	r.yaw = yaw;
	strncpy( r.where, where, sizeof( r.where ) - 1 );
}

// Distance along yaw that is clear for the standing hull and over level floor.
static float LevelDist( const Vector &o, float yaw )
{
	float d = Clear( o, yaw, 1200 ), flat = 0;
	Vector dir = YawDir( yaw );
	for ( float s = 32; s <= d; s += 32 )
	{
		Vector f, n;
		if ( !Probe_FloorBelow( o + dir * s + Vector( 0, 0, 1 ), 1, 3, f, n ) || n.z < 0.999f || fabsf( f.z - o.z ) > 0.5f ) break;
		flat = s;
	}
	return flat;
}

static bool FindFlat( Vector *c, Vector *nrm, int n, Vector &start, float &yaw )
{
	float best = 0;
	for ( int i = 0; i < n; i++ )
	{
		if ( nrm[i].z < 0.999f ) continue;
		for ( int y = 0; y < 16; y++ )
		{
			float flat = LevelDist( c[i], y * 22.5f );
			if ( flat > best ) { best = flat; start = c[i]; yaw = y * 22.5f; }
		}
	}
	return best >= 128;
}

// Step test: a box brush model sunk into level floor so its top is h above
// the floor, 64 u ahead of the start along an axis.
static CProbeEnt *s_stepEnt;

static void StepPlace( const Run &r, int h )
{
	s_stepEnt = NULL;
	TraceResult tr;
	TRACE_LINE( r.start, r.start - Vector( 0, 0, 64 ), ignore_monsters, NULL, &tr );
	float floorZ = tr.vecEndPos.z - 0.03125f;	// a vertical trace stops 1/32 above the plane
	Vector dir = YawDir( r.yaw );
	// any unrotated map brush model of a usable size; keep the first whose
	// top is measured at floor + h on the path
	for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' ) continue;
		if ( ( e->v.flags & FL_KILLME ) || FClassnameIs( e, "probe_ent" ) || e->v.angles != g_vecZero ) continue;
		Vector mn = e->v.mins, mx = e->v.maxs, sz = mx - mn;
		if ( sz.x < 40 || sz.y < 40 || sz.z < 24 || sz.x > 512 || sz.y > 512 || sz.z > 512 ) continue;
		Vector c = ( mn + mx ) * 0.5f;
		float depth = fabsf( dir.x ) > 0.5f ? sz.x * 0.5f : sz.y * 0.5f;
		Vector at = r.start + dir * ( 64 + depth );
		Vector org( at.x - c.x, at.y - c.y, floorZ + h - ( mx.z - 1 ) );	// model bounds are padded by 1
		CProbeEnt *b = Probe_BrushEnt( SOLID_BSP, STRING( e->v.model ), org, g_vecZero );
		TRACE_LINE( at + Vector( 0, 0, h + 64 ), at - Vector( 0, 0, 64 ), dont_ignore_monsters, NULL, &tr );
		float topZ = tr.vecEndPos.z - 0.03125f;
		bool ok = tr.pHit == b->edict() && tr.vecPlaneNormal.z > 0.999f && fabsf( topZ - ( floorZ + h ) ) <= 0.002f;
		if ( ok )
		{
			// room for the standing hull on top of the step and at step height over the start
			TraceResult t2;
			TRACE_HULL( Vector( at.x, at.y, floorZ + h + 37 ), Vector( at.x, at.y, floorZ + h + 37 ), ignore_monsters, 1, NULL, &t2 );
			ok = !t2.fStartSolid;
			TRACE_HULL( r.start + Vector( 0, 0, h + 1 ), at + Vector( 0, 0, 0 ) + Vector( 0, 0, floorZ + h + 37 - at.z ), ignore_monsters, 1, NULL, &t2 );
			ok = ok && !t2.fStartSolid && t2.flFraction == 1.0f;
		}
		if ( !ok )
		{
			Probe_Free( b );
			continue;
		}
		s_stepEnt = b;
		fprintf( s_f, "{\"step\":%d,\"floor_z\":%.9g,\"top_z\":%.9g,\"top_plane_dist\":%.9g,\"ent\":", h, (double)floorZ, (double)topZ, (double)tr.flPlaneDist );
		Probe_PrEnt( s_f, b->edict() );
		fprintf( s_f, "}\n" );
		return;
	}
	fprintf( s_f, "{\"step\":%d,\"placed\":false}\n", h );
}

// Repeated step-ups along a yaw: count of rises of 0.5-18 u within 12 x 32 u.
static int StairScore( const Vector &o, float yaw, float &rise )
{
	Vector pos = o, dir = YawDir( yaw );
	int steps = 0;
	rise = 0;
	for ( int k = 0; k < 12; k++ )
	{
		TraceResult tr, up, fw;
		TRACE_HULL( pos, pos + dir * 32, ignore_monsters, 1, NULL, &tr );
		if ( tr.fStartSolid ) break;
		Vector f, n;
		if ( tr.flFraction == 1.0f )
		{
			if ( !Probe_FloorBelow( tr.vecEndPos + Vector( 0, 0, 1 ), 1, 20, f, n ) || n.z < 0.7f ) break;
			if ( f.z > pos.z + 0.5f ) { steps++; rise += f.z - pos.z; }
			pos = f;
			continue;
		}
		if ( tr.vecPlaneNormal.z > 0.7f ) break;
		TRACE_HULL( pos, pos + Vector( 0, 0, 18 ), ignore_monsters, 1, NULL, &up );
		TRACE_HULL( up.vecEndPos, up.vecEndPos + dir * 32, ignore_monsters, 1, NULL, &fw );
		if ( fw.flFraction <= tr.flFraction + 0.05f ) break;
		if ( !Probe_FloorBelow( fw.vecEndPos, 1, 40, f, n ) || n.z < 0.7f ) break;
		if ( f.z <= pos.z + 0.5f || f.z > pos.z + 18.01f ) break;
		steps++;
		rise += f.z - pos.z;
		pos = f;
	}
	return steps;
}

static bool FindStairs( Vector *c, Vector *nrm, int n, Vector &start, float &yaw, char *where )
{
	int best = 1;
	float bestRise = 0;
	for ( int i = 0; i < n; i++ )
	{
		if ( nrm[i].z < 0.99f ) continue;
		for ( int y = 0; y < 8; y++ )
		{
			float rise;
			int s = StairScore( c[i], y * 45.0f, rise );
			if ( s > best || ( s == best && rise > bestRise ) )
			{
				best = s; bestRise = rise; start = c[i]; yaw = y * 45.0f;
			}
		}
	}
	snprintf( where, 160, "{\"steps\":%d,\"rise\":%.4g}", best, (double)bestRise );
	return best >= 2;
}

static bool FindLadder( Vector &start, float &yaw, char *where )
{
	for ( edict_t *e = FIND_ENTITY_BY_CLASSNAME( NULL, "func_ladder" ); !FNullEnt( e ); e = FIND_ENTITY_BY_CLASSNAME( e, "func_ladder" ) )
	{
		Vector mn = e->v.absmin, mx = e->v.absmax, c = ( mn + mx ) * 0.5f;
		int thin = ( mx.x - mn.x ) < ( mx.y - mn.y ) ? 0 : 1;
		float half = ( mx[thin] - mn[thin] ) * 0.5f;
		for ( int side = -1; side <= 1; side += 2 )
		{
			Vector q = c;
			q[thin] = c[thin] + side * ( half + 20 );
			q.z = mn.z + 40;
			Vector o, n;
			if ( !Probe_FloorBelow( q, 1, 96, o, n ) || n.z < 0.7f ) continue;
			if ( o.z > mn.z + 64 || o.z < mn.z - 64 ) continue;
			Vector to = c - o;
			to.z = 0;
			yaw = atan2f( to.y, to.x ) * ( 180.0 / M_PI );
			start = o;
			snprintf( where, 160, "{\"ladder\":%d,\"absmin\":" VF ",\"absmax\":" VF "}", ENTINDEX( e ), VA( mn ), VA( mx ) );
			return true;
		}
	}
	return false;
}

static bool FindWater( Vector &start, float &yaw, char *where )
{
	for ( int t = 0; t < 400000; t++ )
	{
		Vector p( s_rng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), s_rng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
			s_rng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
		if ( POINT_CONTENTS( p ) != CONTENTS_WATER ) continue;
		float lo = p.z, hi = p.z;
		while ( hi < g_probeWorldMaxs.z && POINT_CONTENTS( Vector( p.x, p.y, hi ) ) == CONTENTS_WATER ) hi += 8;
		if ( POINT_CONTENTS( Vector( p.x, p.y, hi ) ) != CONTENTS_EMPTY ) continue;
		lo = hi - 8;
		for ( int k = 0; k < 6; k++ )
		{
			float m = ( lo + hi ) * 0.5f;
			if ( POINT_CONTENTS( Vector( p.x, p.y, m ) ) == CONTENTS_WATER ) lo = m; else hi = m;
		}
		Vector s( p.x, p.y, floorf( hi ) + 40 );
		if ( POINT_CONTENTS( Vector( p.x, p.y, hi - 72 ) ) != CONTENTS_WATER ) continue;	// deep enough to swim
		TraceResult tr;
		TRACE_HULL( s, s, ignore_monsters, 1, NULL, &tr );
		if ( tr.fStartSolid ) continue;
		float best = -1;
		Vector under( p.x, p.y, hi - 40 );
		for ( int y = 0; y < 8; y++ )
		{
			float d = Clear( under, y * 45.0f, 512 );
			if ( d > best ) { best = d; yaw = y * 45.0f; }
		}
		start = s;
		snprintf( where, 160, "{\"surface_z\":%.6g,\"clear\":%.6g}", (double)hi, (double)best );
		return true;
	}
	return false;
}

static bool FindSlope( Vector *c, Vector *nrm, int n, Vector &start, float &yaw, char *where )
{
	float best = 0;
	for ( int i = 0; i < n; i++ )
	{
		if ( nrm[i].z < 0.72f || nrm[i].z > 0.985f ) continue;
		float y = atan2f( -nrm[i].y, -nrm[i].x ) * ( 180.0 / M_PI );
		float d = Clear( c[i] + Vector( 0, 0, 2 ), y, 256 );
		if ( d > best )
		{
			best = d; start = c[i]; yaw = y;
			snprintf( where, 160, "{\"normal\":" VF ",\"clear\":%.6g}", VA( nrm[i] ), (double)d );
		}
	}
	return best >= 48;
}

// ---------------------------------------------------------------- commands

struct Cmd { Vector ang; float fwd, side, up; unsigned short buttons; };

static bool In( int t, int a, int b ) { return t >= a && t < b; }

static Cmd MakeCmd( const Run &r, int f )
{
	Cmd c;
	int t = f * r.msec;	// ms since the run began
	c.ang = Vector( 0, r.yaw, 0 );
	c.fwd = c.side = c.up = 0;
	c.buttons = 0;
	switch ( r.kind )
	{
	case K_STAND: break;
	case K_WALK: c.fwd = 150; break;
	case K_RUN: case K_STEP18: case K_STEP19: c.fwd = 400; break;
	case K_STRAFE: c.fwd = 400; c.side = 400; c.ang.y = r.yaw + t * 0.03f; break;
	case K_JUMP: c.fwd = 400; if ( In( t, 400, 450 ) || In( t, 1500, 1550 ) ) c.buttons |= IN_JUMP; break;
	case K_SJUMP: if ( In( t, 200, 250 ) ) c.buttons |= IN_JUMP; break;
	case K_DUCK: c.fwd = 400; if ( In( t, 300, 2000 ) ) c.buttons |= IN_DUCK; break;
	case K_DUCKJUMP: c.fwd = 400; if ( In( t, 400, 450 ) ) c.buttons |= IN_JUMP; if ( In( t, 440, 1500 ) ) c.buttons |= IN_DUCK; break;
	case K_STAIRS: c.fwd = t < 2000 ? 400 : -400; break;
	case K_LADDER:
		c.ang.x = -45;
		if ( t < 2000 ) c.fwd = 400;
		else if ( In( t, 2000, 2050 ) ) c.buttons |= IN_JUMP;
		break;
	case K_LADDER_LEVEL: if ( t < 2000 ) c.fwd = 400; break;
	case K_WATER:
		if ( t < 1500 ) { c.fwd = 400; c.ang.x = 30; }
		else { c.fwd = 400; c.ang.x = -30; c.buttons |= IN_JUMP; }
		break;
	case K_SLOPE: c.fwd = 400; if ( t >= 1500 ) c.ang.y = r.yaw + 180; break;
	}
	if ( c.fwd > 0 ) c.buttons |= IN_FORWARD;
	if ( c.fwd < 0 ) c.buttons |= IN_BACK;
	if ( c.side > 0 ) c.buttons |= IN_MOVERIGHT;
	return c;
}

// ---------------------------------------------------------------- logging

static char s_pmInfo[2048];
static int s_peInfo[MAX_PHYSENTS], s_npe;

static void PMBot_Post( playermove_t *pm )
{
	int l = snprintf( s_pmInfo, sizeof( s_pmInfo ), "\"hull\":%d,\"pmt\":%.9g,\"onground\":%d,\"oldbuttons\":%d,\"pe\":[",
		pm->usehull, (double)pm->time, pm->onground, pm->oldbuttons );
	s_npe = 0;
	for ( int i = 0; i < pm->numphysent && i < MAX_PHYSENTS; i++ )
		s_peInfo[s_npe++] = pm->physents[i].info;
	for ( int i = 0; i < pm->numphysent && l < (int)sizeof( s_pmInfo ) - 64; i++ )
		l += snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "%s[%d,%d,%d]", i ? "," : "", pm->physents[i].info, pm->physents[i].solid, pm->physents[i].skin );
	l += snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "],\"me\":[" );
	for ( int i = 0; i < pm->nummoveent && l < (int)sizeof( s_pmInfo ) - 64; i++ )
		l += snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "%s%d", i ? "," : "", pm->moveents[i].info );
	l += snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "],\"touch\":[" );
	for ( int i = 0; i < pm->numtouch && l < (int)sizeof( s_pmInfo ) - 64; i++ )
	{
		int pe = pm->touchindex[i].ent;
		l += snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "%s%d", i ? "," : "", pe >= 0 && pe < pm->numphysent ? pm->physents[pe].info : -1 );
	}
	snprintf( s_pmInfo + l, sizeof( s_pmInfo ) - l, "]" );
}

static void PrPlayer( edict_t *b )
{
	entvars_t *v = &b->v;
	fprintf( s_f, "\"t\":%.9g,\"o\":" VF ",\"v\":" VF ",\"bv\":" VF ",\"fl\":%d,\"vo\":" VF ",\"wl\":%d,\"wt\":%d,\"ge\":%d,"
		"\"ind\":%d,\"dt\":%d,\"fv\":%.9g,\"pa\":" VF ",\"mt\":%d,\"mins\":" VF ",\"maxs\":" VF,
		(double)gpGlobals->time, VA( v->origin ), VA( v->velocity ), VA( v->basevelocity ), v->flags, VA( v->view_ofs ),
		v->waterlevel, v->watertype, Probe_Idx( v->groundentity ), v->bInDuck, v->flDuckTime, (double)v->flFallVelocity,
		VA( v->punchangle ), v->movetype, VA( v->mins ), VA( v->maxs ) );
}

// Non-brush physents (studio / box entities) are not on the brush_ents line:
// print each the first time it is a physent in a run and again whenever its
// state changed, before the frame that uses it.
static bool s_entSeen[2048];
static Vector s_entO[2048], s_entMins[2048], s_entMaxs[2048];
static int s_entSolid[2048];

static void PrPhysentEnts( int run )
{
	bool any = false;
	for ( int i = 0; i < s_npe; i++ )
	{
		int idx = s_peInfo[i];
		if ( idx <= 0 || idx >= 2048 ) continue;
		edict_t *e = INDEXENT( idx );
		if ( !e || e->free || ( e->v.model && STRING( e->v.model )[0] == '*' ) ) continue;
		if ( s_entSeen[idx] && s_entO[idx] == e->v.origin && s_entMins[idx] == e->v.mins && s_entMaxs[idx] == e->v.maxs && s_entSolid[idx] == e->v.solid )
			continue;
		s_entSeen[idx] = true;
		s_entO[idx] = e->v.origin;
		s_entMins[idx] = e->v.mins;
		s_entMaxs[idx] = e->v.maxs;
		s_entSolid[idx] = e->v.solid;
		fprintf( s_f, any ? "," : "{\"run\":%d,\"ents\":[", run );
		Probe_PrEnt( s_f, e );
		any = true;
	}
	if ( any ) fprintf( s_f, "]}\n" );
}

// ---------------------------------------------------------------- capture

bool Cap_PMBot( int step )
{
	static int run, frame;
	if ( step == 0 )
	{
		s_rng.Seed( Probe_Map(), "PM-bot" );
		s_f = Probe_Open( "pmove-host", "PM-bot", &s_rng );
		if ( !s_f ) return true;
		if ( !Probe_Bot() )
		{
			fprintf( s_f, "{\"skipped\":\"no fake client\"}\n" );
			Probe_Close( s_f );
			return true;
		}
		static Vector cand[600], nrm[600];
		int n = FloorCandidates( cand, nrm, 600 );
		s_nruns = 0;
		Vector st;
		float yaw;
		char where[160];
		if ( FindFlat( cand, nrm, n, st, yaw ) )
		{
			snprintf( where, sizeof( where ), "{\"clear\":%.6g,\"level\":true}", (double)Clear( st, yaw, 1200 ) );
			static const int k10[] = { K_STAND, K_WALK, K_RUN, K_STRAFE, K_JUMP, K_SJUMP, K_DUCK, K_DUCKJUMP };
			for ( unsigned k = 0; k < sizeof( k10 ) / sizeof( k10[0] ); k++ ) AddRun( k10[k], 10, st, yaw, where );
			static const int k33[] = { K_RUN, K_JUMP, K_DUCK, K_DUCKJUMP };
			for ( unsigned k = 0; k < sizeof( k33 ) / sizeof( k33[0] ); k++ ) AddRun( k33[k], 33, st, yaw, where );
			AddRun( K_RUN, 50, st, yaw, where );
			AddRun( K_JUMP, 50, st, yaw, where );
			float best = 0, ay = 0;
			for ( int a = 0; a < 4; a++ )
			{
				float d = LevelDist( st, a * 90.0f );
				if ( d > best ) { best = d; ay = a * 90.0f; }
			}
			if ( best >= 192 )
			{
				snprintf( where, sizeof( where ), "{\"level\":%.6g}", (double)best );
				AddRun( K_STEP18, 10, st, ay, where );
				AddRun( K_STEP19, 10, st, ay, where );
			}
			else fprintf( s_f, "{\"finder\":\"step\",\"found\":false}\n" );
		}
		else fprintf( s_f, "{\"finder\":\"flat\",\"found\":false}\n" );
		if ( FindStairs( cand, nrm, n, st, yaw, where ) ) { AddRun( K_STAIRS, 10, st, yaw, where ); AddRun( K_STAIRS, 33, st, yaw, where ); }
		else fprintf( s_f, "{\"finder\":\"stairs\",\"found\":false,\"best\":%s}\n", where );
		if ( FindLadder( st, yaw, where ) ) { AddRun( K_LADDER, 10, st, yaw, where ); AddRun( K_LADDER, 33, st, yaw, where ); AddRun( K_LADDER_LEVEL, 10, st, yaw, where ); }
		else fprintf( s_f, "{\"finder\":\"ladder\",\"found\":false}\n" );
		if ( FindWater( st, yaw, where ) ) { AddRun( K_WATER, 10, st, yaw, where ); AddRun( K_WATER, 33, st, yaw, where ); }
		else fprintf( s_f, "{\"finder\":\"water\",\"found\":false}\n" );
		where[0] = 0;
		if ( FindSlope( cand, nrm, n, st, yaw, where ) ) { AddRun( K_SLOPE, 10, st, yaw, where ); AddRun( K_SLOPE, 33, st, yaw, where ); }
		else fprintf( s_f, "{\"finder\":\"slope\",\"found\":false}\n" );
		run = 0;
		frame = -1;
		fprintf( s_f, "{\"floor_candidates\":%d,\"runs\":%d,\"frames_per_run\":%d}\n", n, s_nruns, RUN_FRAMES );
		return false;
	}
	if ( run >= s_nruns )
	{
		if ( s_stepEnt ) Probe_Free( s_stepEnt );
		s_stepEnt = NULL;
		Probe_Close( s_f );
		return true;
	}
	const Run &r = s_runs[run];
	edict_t *bot = Probe_Bot();
	if ( frame < 0 )
	{
		if ( s_stepEnt ) Probe_Free( s_stepEnt );
		s_stepEnt = NULL;
		if ( r.kind == K_STEP18 || r.kind == K_STEP19 ) StepPlace( r, r.kind == K_STEP18 ? 18 : 19 );
		Probe_BotReset( r.start, SOLID_SLIDEBOX );
		fprintf( s_f, "{\"run\":%d,\"kind\":\"%s\",\"msec\":%d,\"start\":" VF ",\"yaw\":%.9g,\"where\":%s,\"reset\":{",
			run, s_kindName[r.kind], r.msec, VA( r.start ), (double)r.yaw, r.where[0] ? r.where : "{}" );
		PrPlayer( bot );
		fprintf( s_f, "}}\n" );
		frame = 0;
		memset( s_entSeen, 0, sizeof( s_entSeen ) );
	}
	Cmd c = MakeCmd( r, frame );
	s_pmInfo[0] = 0;
	s_npe = 0;
	g_probePmPost = PMBot_Post;
	Probe_BotMove( c.ang, c.fwd, c.side, c.up, c.buttons, r.msec );
	g_probePmPost = NULL;
	PrPhysentEnts( run );
	fprintf( s_f, "{\"run\":%d,\"f\":%d,\"cmd\":{\"a\":" VF ",\"fm\":%.9g,\"sm\":%.9g,\"um\":%.9g,\"b\":%d,\"ms\":%d},",
		run, frame, VA( c.ang ), (double)c.fwd, (double)c.side, (double)c.up, c.buttons, r.msec );
	PrPlayer( bot );
	fprintf( s_f, ",%s}\n", s_pmInfo[0] ? s_pmInfo : "\"pm\":null" );
	if ( ++frame >= RUN_FRAMES )
	{
		frame = -1;
		run++;
	}
	return false;
}

// World stairs for other captures (P-walkmove): the PM-bot finder on
// candidates drawn from `capture`'s own seed. `start` is a standing player
// origin (hull 1) at the foot of the stairs.
bool Probe_FindStairs( const char *capture, Vector &start, float &yaw, char *where )
{
	static Vector cand[600], nrm[600];
	s_rng.Seed( Probe_Map(), capture );
	int n = FloorCandidates( cand, nrm, 600 );
	return FindStairs( cand, nrm, n, start, yaw, where );
}
