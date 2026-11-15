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
// Scenario runner and placement helpers for the M3 captures (P-*, K-*,
// W-water, L-illum): see probe_int.h.

#include "probe_int.h"
#include <math.h>
#include <string.h>

FILE *g_scenFile;
int g_scenId;
bool g_scenStop;

#define SCEN_MAX 32
static EHANDLE s_own[SCEN_MAX];
static int s_nown;
static edict_t *s_track[SCEN_MAX];
static int s_ntrack;
static bool s_skipped;

void Scen_Track( edict_t *e )
{
	if ( e && s_ntrack < SCEN_MAX ) s_track[s_ntrack++] = e;
}

CBaseEntity *Scen_Own( CBaseEntity *e )
{
	if ( e && s_nown < SCEN_MAX ) s_own[s_nown++] = e;
	return e;
}

CProbeEnt *Scen_Ent( int solid, int movetype, const Vector &mins, const Vector &maxs, const Vector &org )
{
	CProbeEnt *e = Probe_Ent( solid, movetype, mins, maxs, org );
	Scen_Own( e );
	Scen_Track( e->edict() );
	return e;
}

bool Scen_Skip( const char *reason )
{
	fprintf( g_scenFile, "{\"scenario\":%d,\"skipped\":\"%s\"}\n", g_scenId, reason );
	s_skipped = true;
	return false;
}

static void Scen_End( ScenRun &r )
{
	for ( int i = 0; i < s_nown; i++ )
		if ( (CBaseEntity *)s_own[i] )
		{
			s_own[i]->pev->skin = 0;	// stays linked until the frame ends: no contents meanwhile
			Probe_Free( s_own[i] );
		}
	s_nown = s_ntrack = 0;
	if ( CVAR_GET_FLOAT( "host_framerate" ) != r.hostFramerate )
		CVAR_SET_FLOAT( "host_framerate", r.hostFramerate );
	Probe_BotPark();
}

void Scen_Begin( ScenRun &r, const ScenDef *defs, int n, FILE *f )
{
	r.defs = defs;
	r.n = n;
	r.cur = -1;
	r.frame = 0;
	r.f = f;
	r.hostFramerate = CVAR_GET_FLOAT( "host_framerate" );
	g_scenFile = f;
	g_probeTouchLog = f;
	g_probePhase = "physics";
	s_nown = s_ntrack = 0;
}

void Scen_BeginAt( ScenRun &r, const ScenDef *defs, int n, FILE *f, int first )
{
	Scen_Begin( r, defs, n, f );
	if ( first <= 0 ) return;
	r.cur = first - 1;	// the caller wrote the scenarios before `first`
	s_skipped = true;
}

static void PrState( ScenRun &r )
{
	fprintf( r.f, "{\"sc\":%d,\"f\":%d,\"t\":%.9g,\"ft\":%.9g,\"e\":[", r.cur, r.frame, (double)gpGlobals->time, (double)gpGlobals->frametime );
	for ( int i = 0; i < s_ntrack; i++ )
	{
		if ( i ) fputc( ',', r.f );
		edict_t *e = s_track[i];
		if ( e->free ) fprintf( r.f, "{\"i\":%d,\"free\":1}", ENTINDEX( e ) );
		else Probe_PrDyn( r.f, e );
	}
	fprintf( r.f, "]}\n" );
}

bool Scen_Step( ScenRun &r )
{
	if ( r.cur >= 0 && !s_skipped )
	{
		PrState( r );
		const ScenDef &d = r.defs[r.cur];
		if ( r.frame < d.frames && !g_scenStop )
		{
			if ( d.tick ) d.tick( r.frame );
			r.frame++;
			return false;
		}
	}
	if ( r.cur >= 0 ) Scen_End( r );
	if ( ++r.cur >= r.n )
	{
		g_probeTouchLog = NULL;
		g_scenFile = NULL;
		return true;
	}
	const ScenDef &d = r.defs[r.cur];
	g_scenId = g_probeCase = r.cur;
	s_skipped = false;
	g_scenStop = false;
	r.frame = 0;
	fprintf( r.f, "{\"scenario\":%d,\"name\":\"%s\",\"frames\":%d,\"t\":%.9g}\n", r.cur, d.name, d.frames, (double)gpGlobals->time );
	if ( !d.setup() )
	{
		if ( !s_skipped ) Scen_Skip( "setup failed" );
		return false;	// Scen_End on the next step
	}
	fprintf( r.f, "{\"sc\":%d,\"ents\":[", r.cur );
	for ( int i = 0; i < s_ntrack; i++ )
	{
		if ( i ) fputc( ',', r.f );
		Probe_PrEnt( r.f, s_track[i] );
	}
	fprintf( r.f, "]}\n" );
	PrState( r );
	if ( d.tick ) d.tick( 0 );
	r.frame = 1;
	return false;
}

// ---------------------------------------------------------------- placement helpers

const char *Probe_PickModel( float minXY, float maxXY, float minZ, float maxZ, bool centred, Vector &pmins, Vector &pmaxs )
{
	static char best[16];
	float bestScore = -1;
	best[0] = 0;
	for ( int i = 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' ) continue;
		if ( ( e->v.flags & FL_KILLME ) || FClassnameIs( e, "probe_ent" ) || e->v.angles != g_vecZero ) continue;
		Vector mn = e->v.mins, mx = e->v.maxs, sz = mx - mn, c = ( mn + mx ) * 0.5f;
		if ( sz.x < minXY || sz.y < minXY || sz.x > maxXY || sz.y > maxXY || sz.z < minZ || sz.z > maxZ ) continue;
		if ( centred && ( fabsf( c.x ) > 4 || fabsf( c.y ) > 4 ) ) continue;
		int saved = e->v.solid, inside = 0;
		e->v.solid = SOLID_BSP;
		for ( int a = 1; a <= 3; a++ ) for ( int b = 1; b <= 3; b++ ) for ( int k = 1; k <= 3; k++ )
		{
			Vector p = e->v.origin + mn + Vector( sz.x * a / 4, sz.y * b / 4, sz.z * k / 4 );
			TraceResult tr;
			TRACE_MODEL( p, p, 0, e, &tr );
			inside += tr.fStartSolid ? 1 : 0;
		}
		e->v.solid = saved;
		if ( inside < 27 ) continue;
		// the smallest that fits the range: platforms stay inside the floor spot
		float score = 1e6f - sz.x * sz.y;
		if ( score > bestScore )
		{
			bestScore = score;
			strncpy( best, STRING( e->v.model ), sizeof( best ) - 1 );
			pmins = mn;
			pmaxs = mx;
		}
	}
	return best[0] ? best : NULL;
}

CProbeEnt *Probe_Platform( const char *model, const Vector &mn, const Vector &mx, float x, float y, float topZ,
	int solid, int movetype, float *top )
{
	Vector c = ( mn + mx ) * 0.5f;
	Vector org( x - c.x, y - c.y, topZ - ( mx.z - 1 ) );
	CProbeEnt *b = Probe_BrushEnt( solid == SOLID_BSP ? SOLID_BSP : solid, model, org, g_vecZero );
	b->pev->movetype = movetype;
	UTIL_SetOrigin( b->pev, org );
	int saved = b->pev->solid;
	b->pev->solid = SOLID_BSP;
	TraceResult tr;
	TRACE_MODEL( Vector( x, y, topZ + 64 ), Vector( x, y, topZ - 64 ), 0, b->edict(), &tr );
	b->pev->solid = saved;
	float t = tr.vecEndPos.z - 0.03125f;
	if ( tr.flFraction == 1.0f || tr.vecPlaneNormal.z < 0.999f || fabsf( t - topZ ) > 0.01f )
	{
		Probe_Free( b );
		return NULL;
	}
	if ( top ) *top = t;
	return b;
}

bool Probe_WaterSpot( ProbeRng &rng, Vector &out, float &floorZ )
{
	for ( int t = 0; t < 400000; t++ )
	{
		Vector p( rng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), rng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
			rng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
		p = Vector( floorf( p.x ), floorf( p.y ), floorf( p.z ) );
		if ( POINT_CONTENTS( p ) != CONTENTS_WATER ) continue;
		float s = p.z;
		while ( s < p.z + 512 && POINT_CONTENTS( Vector( p.x, p.y, s ) ) == CONTENTS_WATER ) s += 1;
		if ( POINT_CONTENTS( Vector( p.x, p.y, s ) ) != CONTENTS_EMPTY || POINT_CONTENTS( Vector( p.x, p.y, s + 72 ) ) != CONTENTS_EMPTY ) continue;
		TraceResult tr;
		TRACE_HULL( Vector( p.x, p.y, s + 37 ), Vector( p.x, p.y, s + 37 ), ignore_monsters, 1, NULL, &tr );
		if ( tr.fStartSolid ) continue;
		TRACE_LINE( Vector( p.x, p.y, s - 1 ), Vector( p.x, p.y, s - 257 ), ignore_monsters, NULL, &tr );
		if ( tr.flFraction == 1.0f || tr.fStartSolid || Probe_Idx( tr.pHit ) != 0 ) continue;
		if ( s - 1 - tr.vecEndPos.z < 48 ) continue;	// deep enough to swim
		out = Vector( p.x, p.y, s - 1 );
		floorZ = tr.vecEndPos.z - 0.03125f;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- capture driver

Vector g_scenFloor;
float g_scenRadius;
ProbeRng g_scenRng;
static ScenRun s_run;

bool Scen_Capture( int step, const char *spec, const char *capture, const ScenDef *defs, int n, float radius, float height )
{
	if ( step == 0 )
	{
		g_scenRng.Seed( Probe_Map(), capture );
		FILE *f = Probe_Open( spec, capture, &g_scenRng );
		if ( !f ) return true;
		bool ok = false;
		for ( g_scenRadius = radius; g_scenRadius >= 64 && !ok; g_scenRadius -= 32 )
			ok = Probe_FloorSpot( g_scenRng, g_scenRadius, height, g_scenFloor );
		if ( !ok )
		{
			fprintf( f, "{\"skipped\":\"no level floor spot\"}\n" );
			Probe_Close( f );
			return true;
		}
		g_scenRadius += 32;
		fprintf( f, "{\"floor\":" VF ",\"radius\":%.9g,\"height\":%.9g}\n", VA( g_scenFloor ), (double)g_scenRadius, (double)height );
		Probe_Bot();
		Scen_Begin( s_run, defs, n, f );
		return false;
	}
	if ( !Scen_Step( s_run ) ) return false;
	Probe_Close( s_run.f );
	return true;
}
