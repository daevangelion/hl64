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
// Trace captures for docs/specs/trace-and-hulls.md: T-points (point
// contents, engine and pmove), T-segments and T-allsolid (world and move
// traces per hull, pmove player traces), T-entities (box targets, filters,
// missile size, owner skips, glass, hull selection by box size), T-rotated
// (rotated brush entities) and T-hitbox (line traces against posed studio
// monsters). All run synchronously in one frame.

#include "probe_int.h"
#include <stdlib.h>
#include <math.h>

extern int DispatchSpawn( edict_t *pent );

static ProbeRng s_rng;
static FILE *s_f;

static int OpenFor( int hull, const Vector &p )
{
	if ( hull == 0 ) return POINT_CONTENTS( p ) != CONTENTS_SOLID;
	TraceResult tr;
	TRACE_HULL( p, p, ignore_monsters, hull, NULL, &tr );
	return !tr.fStartSolid && !tr.fAllSolid;
}

static Vector RandPoint( void )
{
	return Vector( s_rng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), s_rng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
		s_rng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
}

static int UseHullFor( int hull )
{
	static const int map[4] = { 2, 0, 3, 1 };	// engine hull -> pmove usehull
	return map[hull];
}

// ---------------------------------------------------------------- T-points

struct TPt { Vector p; char src; };
static TPt *s_pts;
static int s_npts;

static void TPoints_Pm( playermove_t *pm )
{
	for ( int i = 0; i < s_npts; i++ )
	{
		Vector p = s_pts[i].p;
		int tc = 0;
		int pc = pm->PM_PointContents( p, &tc );
		int tpc = pm->PM_TruePointContents( p );
		fprintf( s_f, "{\"p\":" VF ",\"src\":\"%c\",\"c\":%d,\"pmc\":%d,\"pmtc\":%d,\"tpc\":%d}\n",
			VA( p ), s_pts[i].src, POINT_CONTENTS( p ), pc, tc, tpc );
	}
	s_npts = -1;	// written
}

bool Cap_TPoints( int step )
{
	s_rng.Seed( Probe_Map(), "T-points" );
	s_f = Probe_Open( "trace-and-hulls", "T-points", &s_rng );
	if ( !s_f ) return true;
	int n = Probe_Count( 20000 );
	s_pts = (TPt *)calloc( n + 4096, sizeof( TPt ) );
	s_npts = 0;
	for ( int i = 0; i < n; i++ )
	{
		Vector p = RandPoint();
		char src = 'u';
		if ( i % 4 == 0 )
		{
			p = Vector( floorf( p.x * 16 + 0.5f ) / 16, floorf( p.y * 16 + 0.5f ) / 16, floorf( p.z * 16 + 0.5f ) / 16 );
			src = 'l';
		}
		s_pts[s_npts].p = p;
		s_pts[s_npts++].src = src;
	}
	// points inside brush entities that carry contents (func_water & co.)
	for ( int i = 1; i < gpGlobals->maxEntities && s_npts < n + 4000; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' || e->v.skin >= -1 ) continue;
		for ( int k = 0; k < 64; k++ )
		{
			s_pts[s_npts].p = Vector( s_rng.Uni( e->v.absmin.x, e->v.absmax.x ), s_rng.Uni( e->v.absmin.y, e->v.absmax.y ),
				s_rng.Uni( e->v.absmin.z, e->v.absmax.z ) );
			s_pts[s_npts++].src = 'b';
		}
	}
	edict_t *bot = Probe_Bot();
	if ( bot )
	{
		g_probePmPre = TPoints_Pm;
		Probe_BotMove( g_vecZero, 0, 0, 0, 0, 10 );
	}
	if ( s_npts >= 0 )	// pmove hook did not run
	{
		for ( int i = 0; i < s_npts; i++ )
			fprintf( s_f, "{\"p\":" VF ",\"src\":\"%c\",\"c\":%d}\n", VA( s_pts[i].p ), s_pts[i].src, POINT_CONTENTS( s_pts[i].p ) );
		Probe_Log( "probe: T-points without pmove contents\n" );
	}
	g_probePmPre = NULL;
	free( s_pts );
	s_pts = NULL;
	Probe_Close( s_f );
	return true;
}

// ---------------------------------------------------------------- T-segments / T-allsolid

struct TSeg { int hull; char kind; Vector s, e; int havePm; pmtrace_t pm; };
static TSeg *s_segs;
static int s_nsegs, s_pmPerHull;

static void TSeg_Pm( playermove_t *pm )
{
	int saved = pm->usehull, done[4] = { 0, 0, 0, 0 };
	for ( int i = 0; i < s_nsegs; i++ )
	{
		TSeg &g = s_segs[i];
		if ( done[g.hull] >= s_pmPerHull ) continue;
		done[g.hull]++;
		pm->usehull = UseHullFor( g.hull );
		g.pm = pm->PM_PlayerTrace( g.s, g.e, PM_WORLD_ONLY, -1 );
		g.havePm = 1;
	}
	pm->usehull = saved;
}

static void TSeg_Write( void )
{
	for ( int i = 0; i < s_nsegs; i++ )
	{
		TSeg &g = s_segs[i];
		TraceResult tw, tm;
		TRACE_MODEL( g.s, g.e, g.hull, INDEXENT( 0 ), &tw );
		TRACE_HULL( g.s, g.e, ignore_monsters, g.hull, NULL, &tm );
		fprintf( s_f, "{\"hull\":%d,\"kind\":\"%c\",\"s\":" VF ",\"e\":" VF ",\"world\":{", g.hull, g.kind, VA( g.s ), VA( g.e ) );
		Probe_PrTrace( s_f, tw );
		fprintf( s_f, "},\"move\":{" );
		Probe_PrTrace( s_f, tm );
		fprintf( s_f, "}" );
		if ( g.havePm )
		{
			fprintf( s_f, ",\"pm\":{\"usehull\":%d,", UseHullFor( g.hull ) );
			Probe_PrPmTrace( s_f, g.pm );
			fprintf( s_f, "}" );
		}
		fprintf( s_f, "}\n" );
	}
}

static void TSeg_Run( void )
{
	if ( Probe_Bot() )
	{
		g_probePmPre = TSeg_Pm;
		Probe_BotMove( g_vecZero, 0, 0, 0, 0, 10 );
		g_probePmPre = NULL;
	}
	TSeg_Write();
	free( s_segs );
	s_segs = NULL;
}

static Vector SegDir( void )
{
	int k = s_rng.Range( 0, 99 );
	if ( k < 15 )
	{
		static const Vector axes[6] = { Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ),
			Vector( 0, -1, 0 ), Vector( 0, 0, 1 ), Vector( 0, 0, -1 ) };
		return axes[s_rng.Range( 0, 5 )];
	}
	if ( k < 30 )
	{
		float a = s_rng.Uni( 0, 2 * M_PI );
		return Vector( cosf( a ), sinf( a ), 0 );
	}
	return s_rng.Dir();
}

bool Cap_TSegments( int step )
{
	s_rng.Seed( Probe_Map(), "T-segments" );
	s_f = Probe_Open( "trace-and-hulls", "T-segments", &s_rng );
	if ( !s_f ) return true;
	int nShort = Probe_Count( 2000 ), nLong = Probe_Count( 200 );
	s_segs = (TSeg *)calloc( 4 * ( nShort + nLong ), sizeof( TSeg ) );
	s_nsegs = 0;
	s_pmPerHull = Probe_Count( 500 );
	for ( int hull = 0; hull < 4; hull++ )
	{
		for ( int i = 0; i < nShort + nLong; i++ )
		{
			bool lng = i >= nShort;
			Vector s;
			bool wantOpen = s_rng.Range( 0, 99 ) < 80;
			for ( int t = 0; t < 10000; t++ )
			{
				s = RandPoint();
				if ( !wantOpen || OpenFor( hull, s ) ) break;
			}
			float len = lng ? s_rng.Uni( 192, 4096 ) : s_rng.Uni( 0.5f, 192 );
			TSeg &g = s_segs[s_nsegs++];
			g.hull = hull;
			g.kind = lng ? 'L' : ( wantOpen ? 'o' : 'u' );
			g.s = s;
			g.e = s + SegDir() * len;
		}
	}
	TSeg_Run();
	Probe_Close( s_f );
	return true;
}

bool Cap_TAllsolid( int step )
{
	s_rng.Seed( Probe_Map(), "T-allsolid" );
	s_f = Probe_Open( "trace-and-hulls", "T-allsolid", &s_rng );
	if ( !s_f ) return true;
	int n = Probe_Count( 500 );
	s_segs = (TSeg *)calloc( 4 * 2 * n, sizeof( TSeg ) );
	s_nsegs = 0;
	s_pmPerHull = 2 * n;
	for ( int hull = 0; hull < 4; hull++ )
	{
		// 's': start in solid, short random segment; 'x': start in solid, end in the open
		for ( int kind = 0; kind < 2; kind++ )
		{
			for ( int i = 0; i < n; i++ )
			{
				for ( int t = 0; t < 100000; t++ )
				{
					Vector s = RandPoint();
					if ( OpenFor( hull, s ) ) continue;
					Vector e = s + s_rng.Dir() * s_rng.Uni( 1, kind ? 128 : 64 );
					if ( kind == 1 && !OpenFor( hull, e ) ) continue;
					TSeg &g = s_segs[s_nsegs++];
					g.hull = hull;
					g.kind = kind ? 'x' : 's';
					g.s = s;
					g.e = e;
					break;
				}
			}
		}
	}
	TSeg_Run();
	Probe_Close( s_f );
	return true;
}

// ---------------------------------------------------------------- T-entities

static void EntTrace( const char *cfg, const char *fn, int mode, int hull, edict_t *pass, edict_t *mover,
	const Vector &a, const Vector &b )
{
	TraceResult tr;
	if ( !strcmp( fn, "line" ) ) TRACE_LINE( a, b, mode, pass, &tr );
	else if ( !strcmp( fn, "hull" ) ) TRACE_HULL( a, b, mode, hull, pass, &tr );
	else TRACE_MONSTER_HULL( mover, a, b, mode, pass, &tr );
	fprintf( s_f, "{\"cfg\":\"%s\",\"fn\":\"%s\",\"mode\":%d,\"hull\":%d,\"pass\":%d,\"mover\":%d,\"s\":" VF ",\"e\":" VF ",",
		cfg, fn, mode, hull, Probe_Idx( pass ), Probe_Idx( mover ), VA( a ), VA( b ) );
	if ( mover )
		fprintf( s_f, "\"mover_mins\":" VF ",\"mover_maxs\":" VF ",", VA( mover->v.mins ), VA( mover->v.maxs ) );
	Probe_PrTrace( s_f, tr );
	fprintf( s_f, "}\n" );
}

static void EntSetup( const char *cfg, CBaseEntity **ents, int n )
{
	fprintf( s_f, "{\"setup\":\"%s\",\"ents\":[", cfg );
	for ( int i = 0; i < n; i++ )
	{
		if ( i ) fputc( ',', s_f );
		Probe_PrEnt( s_f, ents[i]->edict() );
	}
	fprintf( s_f, "]}\n" );
}

bool Cap_TEntities( int step )
{
	s_rng.Seed( Probe_Map(), "T-entities" );
	s_f = Probe_Open( "trace-and-hulls", "T-entities", &s_rng );
	if ( !s_f ) return true;
	Vector S;
	if ( !Probe_OpenSpot( s_rng, 192, S ) && !Probe_OpenSpot( s_rng, 160, S ) && !Probe_OpenSpot( s_rng, 128, S ) && !Probe_OpenSpot( s_rng, 96, S ) && !Probe_OpenSpot( s_rng, 64, S ) )
	{
		fprintf( s_f, "{\"skipped\":\"no open spot\"}\n" );
		Probe_Close( s_f );
		return true;
	}
	fprintf( s_f, "{\"spot\":" VF "}\n", VA( S ) );

	CProbeEnt *B = Probe_Ent( SOLID_BBOX, MOVETYPE_NONE, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ), S );
	CProbeEnt *P = Probe_Ent( SOLID_BBOX, MOVETYPE_NONE, Vector( -8, -8, -8 ), Vector( 8, 8, 8 ), S + Vector( -100, 0, 0 ) );
	CBaseEntity *both[2] = { B, P };

	struct Cfg { const char *name; int solid; int owner; int glass; int monster; int zero; };
	static const Cfg cfgs[] =
	{
		{ "bbox", SOLID_BBOX, 0, 0, 0, 0 },
		{ "slidebox", SOLID_SLIDEBOX, 0, 0, 0, 0 },
		{ "not", SOLID_NOT, 0, 0, 0, 0 },
		{ "trigger", SOLID_TRIGGER, 0, 0, 0, 0 },
		{ "owned_by_pass", SOLID_BBOX, 1, 0, 0, 0 },
		{ "owner_of_pass", SOLID_BBOX, 2, 0, 0, 0 },
		{ "glass", SOLID_BBOX, 0, 1, 0, 0 },
		{ "monster", SOLID_SLIDEBOX, 0, 0, 1, 0 },
		{ "zero_size", SOLID_BBOX, 0, 0, 0, 1 },
	};
	static const float lineOff[][2] = { { 0, 0 }, { 0, 30 }, { 15.999f, 0 }, { 16, 0 }, { 16.001f, 0 }, { 20, 0 },
		{ 30, 0 }, { 31, 0 }, { 31.5f, 0 }, { 0, 36 }, { 0, 50 }, { 16, 36 } };
	static const float hullOff[] = { 0, 15.999f, 16, 16.001f, 31.999f, 32, 32.001f, 47.9f, 48, 48.1f };
	static const int lineModes[] = { 0, 1, 2, 0x100, 0x101 };

	for ( unsigned c = 0; c < sizeof( cfgs ) / sizeof( cfgs[0] ); c++ )
	{
		const Cfg &k = cfgs[c];
		B->pev->solid = k.solid;
		B->pev->owner = k.owner == 1 ? P->edict() : NULL;
		P->pev->owner = k.owner == 2 ? B->edict() : NULL;
		B->pev->rendermode = k.glass ? kRenderTransTexture : kRenderNormal;
		B->pev->renderamt = k.glass ? 128 : 0;
		B->pev->flags = k.monster ? FL_MONSTER : 0;
		if ( k.zero ) UTIL_SetSize( B->pev, g_vecZero, g_vecZero );
		else UTIL_SetSize( B->pev, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ) );
		UTIL_SetOrigin( B->pev, S );
		P->pev->solid = SOLID_BBOX;
		UTIL_SetOrigin( P->pev, S + Vector( -100, 0, 0 ) );
		EntSetup( k.name, both, 2 );

		edict_t *passes[3] = { NULL, P->edict(), B->edict() };
		for ( int pi = 0; pi < 3; pi++ )
		{
			for ( unsigned m = 0; m < sizeof( lineModes ) / sizeof( lineModes[0] ); m++ )
				for ( unsigned o = 0; o < sizeof( lineOff ) / sizeof( lineOff[0] ); o++ )
				{
					Vector off( 0, lineOff[o][0], lineOff[o][1] );
					EntTrace( k.name, "line", lineModes[m], 0, passes[pi], NULL, S + Vector( -90, 0, 0 ) + off, S + Vector( 90, 0, 0 ) + off );
				}
			for ( int hull = 1; hull < 4; hull++ )
				for ( int m = 0; m < 2; m++ )
					for ( unsigned o = 0; o < sizeof( hullOff ) / sizeof( hullOff[0] ); o++ )
					{
						Vector off( 0, hullOff[o], 0 );
						EntTrace( k.name, "hull", m, hull, passes[pi], NULL, S + Vector( -90, 0, 0 ) + off, S + Vector( 90, 0, 0 ) + off );
						Vector up( hullOff[o], 0, 0 );
						EntTrace( k.name, "hull", m, hull, passes[pi], NULL, S + Vector( 0, 0, 120 ) + up, S + Vector( 0, 0, -120 ) + up );
					}
		}
	}

	// T6: hull selection by moving box size, against a brush entity and the world.
	B->pev->solid = SOLID_NOT;
	UTIL_SetOrigin( B->pev, S );
	P->pev->solid = SOLID_NOT;
	UTIL_SetOrigin( P->pev, P->pev->origin );
	Vector dmn, dmx;
	const char *dm = Probe_PickBoxModel( 16, 128, &dmn, &dmx );
	CProbeEnt *D = NULL;
	if ( dm )
	{
		Vector org( S.x + 100 - dmn.x, S.y - ( dmn.y + dmx.y ) * 0.5f, S.z - ( dmn.z + dmx.z ) * 0.5f );
		D = Probe_BrushEnt( SOLID_BSP, dm, org, g_vecZero );
		CBaseEntity *d1[1] = { D };
		EntSetup( "hullsel_door", d1, 1 );
	}
	static const float sx[] = { 0, 8, 8.01f, 32, 36, 36.01f, 64 };
	static const float sz[] = { 18, 36, 36.01f, 72 };
	static const Vector dirs[6] = { Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ), Vector( 0, -1, 0 ),
		Vector( 0, 0, 1 ), Vector( 0, 0, -1 ) };
	CProbeEnt *M = Probe_Ent( SOLID_NOT, MOVETYPE_NONE, g_vecZero, g_vecZero, S );
	for ( unsigned a = 0; a < sizeof( sx ) / sizeof( sx[0] ); a++ )
		for ( unsigned b = 0; b < sizeof( sz ) / sizeof( sz[0] ); b++ )
		{
			char cfg[48];
			snprintf( cfg, sizeof( cfg ), "hullsel_%g_%g", sx[a], sz[b] );
			UTIL_SetSize( M->pev, Vector( -sx[a] / 2, -sx[a] / 2, -sz[b] / 2 ), Vector( sx[a] / 2, sx[a] / 2, sz[b] / 2 ) );
			UTIL_SetOrigin( M->pev, S );
			for ( int d = 0; d < 6; d++ )
				EntTrace( cfg, "monsterhull", 0, -1, M->edict(), M->edict(), S, S + dirs[d] * ( d == 0 && D ? 300 : 4000 ) );
			// feet-origin variant of the same size
			UTIL_SetSize( M->pev, Vector( -sx[a] / 2, -sx[a] / 2, 0 ), Vector( sx[a] / 2, sx[a] / 2, sz[b] ) );
			snprintf( cfg, sizeof( cfg ), "hullsel_feet_%g_%g", sx[a], sz[b] );
			for ( int d = 0; d < 6; d++ )
				EntTrace( cfg, "monsterhull", 0, -1, M->edict(), M->edict(), S, S + dirs[d] * ( d == 0 && D ? 300 : 4000 ) );
		}
	for ( int hull = 0; hull < 4; hull++ )
		for ( int d = 0; d < 6; d++ )
			EntTrace( "hullsel_ref", "hull", 0, hull, NULL, NULL, S, S + dirs[d] * ( d == 0 && D ? 300 : 4000 ) );

	Probe_Free( B );
	Probe_Free( P );
	Probe_Free( M );
	if ( D ) Probe_Free( D );
	Probe_Close( s_f );
	return true;
}

// ---------------------------------------------------------------- T-rotated

bool Cap_TRotated( int step )
{
	s_rng.Seed( Probe_Map(), "T-rotated" );
	s_f = Probe_Open( "trace-and-hulls", "T-rotated", &s_rng );
	if ( !s_f ) return true;
	Vector S, mn, mx;
	const char *model = Probe_PickBoxModel( 16, 160, &mn, &mx );
	if ( !model || ( !Probe_OpenSpot( s_rng, 256, S ) && !Probe_OpenSpot( s_rng, 192, S ) && !Probe_OpenSpot( s_rng, 160, S ) && !Probe_OpenSpot( s_rng, 128, S ) && !Probe_OpenSpot( s_rng, 96, S ) && !Probe_OpenSpot( s_rng, 64, S ) ) )
	{
		fprintf( s_f, "{\"skipped\":\"%s\"}\n", model ? "no open spot" : "no brush model" );
		Probe_Close( s_f );
		return true;
	}
	Vector c = ( mn + mx ) * 0.5f, half = ( mx - mn ) * 0.5f;
	float radius = half.Length();
	fprintf( s_f, "{\"spot\":" VF ",\"model\":\"%s\",\"model_mins\":" VF ",\"model_maxs\":" VF "}\n", VA( S ), model, VA( mn ), VA( mx ) );

	static const Vector angs[] = { Vector( 0, 0, 0 ), Vector( 0, 30, 0 ), Vector( 0, 90, 0 ), Vector( 0, 135, 0 ),
		Vector( 30, 0, 0 ), Vector( 15, 45, 0 ), Vector( 0, 30, 20 ) };
	CProbeEnt *F = Probe_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -16, -16, 0 ), Vector( 16, 16, 72 ), S );
	int n = Probe_Count( 60 );
	for ( unsigned a = 0; a < sizeof( angs ) / sizeof( angs[0] ); a++ )
	{
		// origin so that the rotated model centre lands on the spot
		UTIL_MakeVectors( angs[a] );
		Vector rc = gpGlobals->v_forward * c.x - gpGlobals->v_right * c.y + gpGlobals->v_up * c.z;
		CProbeEnt *R = Probe_BrushEnt( SOLID_BSP, model, S - rc, angs[a] );
		fprintf( s_f, "{\"angles\":" VF ",\"ent\":", VA( angs[a] ) );
		Probe_PrEnt( s_f, R->edict() );
		fprintf( s_f, "}\n" );
		for ( int v = 0; v < 8; v++ )
		{
			for ( int i = 0; i < n; i++ )
			{
				Vector d = s_rng.Dir();
				Vector j( s_rng.Uni( -half.x, half.x ) * 0.8f, s_rng.Uni( -half.y, half.y ) * 0.8f, s_rng.Uni( -half.z, half.z ) * 0.8f );
				Vector a0 = S + d * ( radius + 64 );
				Vector b0 = S + j - d * 24;
				char cfg[32];
				snprintf( cfg, sizeof( cfg ), "rot%u", a );
				switch ( v )
				{
				case 0: EntTrace( cfg, "line", 0, 0, NULL, NULL, a0, b0 ); break;
				case 1: case 2: case 3: EntTrace( cfg, "hull", 0, v, NULL, NULL, a0, b0 ); break;
				case 4: EntTrace( cfg, "monsterhull", 0, -1, F->edict(), F->edict(), a0, b0 ); break;
				default:
				{
					int hull = v == 5 ? 0 : v == 6 ? 1 : 3;
					TraceResult tr;
					TRACE_MODEL( a0, b0, hull, R->edict(), &tr );
					fprintf( s_f, "{\"cfg\":\"%s\",\"fn\":\"model\",\"hull\":%d,\"target\":%d,\"s\":" VF ",\"e\":" VF ",",
						cfg, hull, R->entindex(), VA( a0 ), VA( b0 ) );
					Probe_PrTrace( s_f, tr );
					fprintf( s_f, "}\n" );
				}
				}
			}
		}
		Probe_Free( R );
	}
	Probe_Free( F );
	Probe_Close( s_f );
	return true;
}

// ---------------------------------------------------------------- T-hitbox

bool Cap_THitbox( int step )
{
	s_rng.Seed( Probe_Map(), "T-hitbox" );
	s_f = Probe_Open( "trace-and-hulls", "T-hitbox", &s_rng );
	if ( !s_f ) return true;
	Vector S;
	if ( !Probe_OpenSpot( s_rng, 160, S ) && !Probe_OpenSpot( s_rng, 128, S ) && !Probe_OpenSpot( s_rng, 96, S ) && !Probe_OpenSpot( s_rng, 64, S ) )
	{
		fprintf( s_f, "{\"skipped\":\"no open spot\"}\n" );
		Probe_Close( s_f );
		return true;
	}
	S.z -= 36;	// monster origin at its feet; its box is centred on the spot
	fprintf( s_f, "{\"spot\":" VF ",\"sv_clienttrace\":%.9g}\n", VA( S ), (double)CVAR_GET_FLOAT( "sv_clienttrace" ) );
	static const char *mons[] = { "monster_scientist", "monster_barney", "monster_human_grunt", "monster_zombie" };
	for ( unsigned m = 0; m < sizeof( mons ) / sizeof( mons[0] ); m++ )
	{
		edict_t *e = CREATE_NAMED_ENTITY( MAKE_STRING( mons[m] ) );
		if ( !e || !e->pvPrivateData )
		{
			fprintf( s_f, "{\"monster\":\"%s\",\"skipped\":\"create failed\"}\n", mons[m] );
			continue;
		}
		e->v.origin = S;
		DispatchSpawn( e );
		CBaseMonster *mon = (CBaseMonster *)CBaseEntity::Instance( e );
		e->v.nextthink = 0;
		e->v.movetype = MOVETYPE_NONE;
		e->v.takedamage = DAMAGE_NO;
		e->v.flags &= ~FL_ONGROUND;
		struct Pose { int seq; float frame; float yaw; byte ctl[4]; byte blend[2]; };
		Pose poses[4] =
		{
			{ 0, 0, 0, { 127, 127, 127, 127 }, { 127, 127 } },
			{ 0, 128, 90, { 127, 127, 127, 127 }, { 127, 127 } },
			{ mon ? mon->LookupActivity( ACT_WALK ) : 1, 64, 30, { 127, 127, 127, 127 }, { 127, 127 } },
			{ mon ? mon->LookupActivity( ACT_RUN ) : 2, 200, 0, { 255, 0, 127, 127 }, { 255, 0 } },
		};
		for ( int p = 0; p < 4; p++ )
		{
			if ( poses[p].seq < 0 ) poses[p].seq = 0;
			e->v.sequence = poses[p].seq;
			e->v.frame = poses[p].frame;
			e->v.framerate = 0;
			e->v.animtime = gpGlobals->time;
			e->v.angles = Vector( 0, poses[p].yaw, 0 );
			for ( int k = 0; k < 4; k++ ) e->v.controller[k] = poses[p].ctl[k];
			for ( int k = 0; k < 2; k++ ) e->v.blending[k] = poses[p].blend[k];
			UTIL_SetOrigin( &e->v, S );
			fprintf( s_f, "{\"monster\":\"%s\",\"pose\":%d,\"sequence\":%d,\"frame\":%.9g,\"controller\":[%d,%d,%d,%d],\"blending\":[%d,%d],\"body\":%d,\"skin\":%d,\"ent\":",
				mons[m], p, e->v.sequence, (double)e->v.frame, e->v.controller[0], e->v.controller[1], e->v.controller[2],
				e->v.controller[3], e->v.blending[0], e->v.blending[1], e->v.body, e->v.skin );
			Probe_PrEnt( s_f, e );
			fprintf( s_f, "}\n" );
			char cfg[48];
			snprintf( cfg, sizeof( cfg ), "%s_p%d", mons[m] + 8, p );
			for ( int side = 0; side < 4; side++ )
			{
				float yaw = side * 90.0f * ( M_PI / 180.0 );
				Vector dir( cosf( yaw ), sinf( yaw ), 0 ), perp( -sinf( yaw ), cosf( yaw ), 0 );
				for ( int z = -2; z <= 78; z += 4 )
					for ( int l = -20; l <= 20; l += 4 )
					{
						Vector a = S - dir * 64 + perp * (float)l + Vector( 0, 0, (float)z );
						EntTrace( cfg, "line", 0, 0, NULL, NULL, a, a + dir * 128 );
						if ( l == 0 && ( z % 16 ) == 2 )
							EntTrace( cfg, "hull", 0, 3, NULL, NULL, a, a + dir * 128 );
					}
			}
		}
		e->v.solid = SOLID_NOT;
		UTIL_SetOrigin( &e->v, S );
		if ( mon ) UTIL_Remove( mon );
	}
	Probe_Close( s_f );
	return true;
}
