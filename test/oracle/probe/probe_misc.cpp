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
// W-water: contents of a func_water (spawned by the game DLL; none of the
// Uplink / Hazard Course maps place one) and of probe brush entities with
// contents skins: pfnPointContents, the pmove point contents, the water
// level of toss / step entities and of the fake client inside it, and the
// contents after the func_water moved (it is a door).
// L-illum: pfnGetEntityIllum, the only server-side observable of light
// styles: sample points every frame, with light style 0 changed, next to an
// EF_BRIGHTLIGHT entity, and for the fake client.

#include "probe_int.h"
#include <math.h>
#include <string.h>

#define F g_scenFloor
#define R g_scenRadius

// ---------------------------------------------------------------- W-water

static EHANDLE s_water;
static Vector s_wpts[300];
static int s_nwpts;

static void WaterPm( playermove_t *pm )
{
	for ( int i = 0; i < s_nwpts; i++ )
	{
		Vector p = s_wpts[i];
		int tc = 0;
		int pc = pm->PM_PointContents( p, &tc );
		int tpc = pm->PM_TruePointContents( p );
		fprintf( g_scenFile, "{\"sc\":%d,\"p\":" VF ",\"c\":%d,\"pmc\":%d,\"pmtc\":%d,\"tpc\":%d}\n", g_scenId, VA( p ), POINT_CONTENTS( p ), pc, tc, tpc );
	}
	s_nwpts = -1;
}

// Point samples over the water entity's bounds (8 u margin): 6x6x6 lattice
// plus the centre column; pmove contents from the fake client parked just
// outside the +x face (its physents include the water entity).
static void SampleWater( CBaseEntity *w, bool pm )
{
	Vector lo = w->pev->absmin - Vector( 8, 8, 8 ), hi = w->pev->absmax + Vector( 8, 8, 8 );
	s_nwpts = 0;
	for ( int a = 0; a < 6; a++ ) for ( int b = 0; b < 6; b++ ) for ( int c = 0; c < 6; c++ )
		s_wpts[s_nwpts++] = Vector( floorf( lo.x + ( hi.x - lo.x ) * ( a + 0.5f ) / 6 ), floorf( lo.y + ( hi.y - lo.y ) * ( b + 0.5f ) / 6 ),
			floorf( lo.z + ( hi.z - lo.z ) * ( c + 0.5f ) / 6 ) );
	Vector c = ( w->pev->absmin + w->pev->absmax ) * 0.5f;
	for ( float z = lo.z; z <= hi.z && s_nwpts < 300; z += 4 )
		s_wpts[s_nwpts++] = Vector( floorf( c.x ), floorf( c.y ), z );
	edict_t *bot = pm ? Probe_Bot() : NULL;
	if ( bot )
	{
		Probe_BotReset( Vector( w->pev->absmax.x + 18, c.y, F.z + 37 ), SOLID_SLIDEBOX );
		g_probePmPre = WaterPm;
		Probe_BotMove( g_vecZero, 0, 0, 0, 0, 10 );
		g_probePmPre = NULL;
		Probe_BotPark();
	}
	if ( s_nwpts >= 0 )	// no pmove: engine contents only
		for ( int i = 0; i < s_nwpts; i++ )
			fprintf( g_scenFile, "{\"sc\":%d,\"p\":" VF ",\"c\":%d}\n", g_scenId, VA( s_wpts[i] ), POINT_CONTENTS( s_wpts[i] ) );
}

static CBaseEntity *FuncWater( void )
{
	Vector mn, mx;
	const char *model = Probe_PickModel( 64, 2 * R - 72, 48, 200, false, mn, mx );
	if ( !model ) return NULL;
	edict_t *e = CREATE_NAMED_ENTITY( MAKE_STRING( "func_water" ) );
	if ( FNullEnt( e ) ) return NULL;
	Vector c = ( mn + mx ) * 0.5f;
	Vector org( F.x - c.x, F.y - c.y, F.z - ( mn.z + 1 ) );	// bottom face on the floor
	char o[96];
	snprintf( o, sizeof( o ), "%.9g %.9g %.9g", (double)org.x, (double)org.y, (double)org.z );
	const char *kv[] = { "model", model, "origin", o, "skin", "-3", "speed", "100", "lip", "0", "WaveHeight", "0" };
	for ( int i = 0; i < 12; i += 2 )
	{
		KeyValueData kvd;
		kvd.szClassName = (char *)"func_water";
		kvd.szKeyName = (char *)kv[i];
		kvd.szValue = (char *)STRING( ALLOC_STRING( kv[i + 1] ) );
		kvd.fHandled = 0;
		DispatchKeyValue( e, &kvd );
	}
	DispatchSpawn( e );
	CBaseEntity *w = CBaseEntity::Instance( e );
	if ( !w ) return NULL;
	Scen_Own( w );
	Scen_Track( e );
	s_water = w;
	fprintf( g_scenFile, "{\"sc\":%d,\"func_water\":", g_scenId );
	Probe_PrEnt( g_scenFile, e );
	fprintf( g_scenFile, "}\n" );
	return w;
}

static void WaterTick( int f )
{
	CBaseEntity *w = s_water;
	switch ( g_scenId )
	{
	case 3:
	{
		edict_t *bot = Probe_Bot();
		if ( bot ) Probe_BotMove( g_vecZero, 0, 0, 0, 0, 10 );
		break;
	}
	case 4:
		if ( w && f == 1 )
		{
			fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"use\",\"t\":%.9g}\n", g_scenId, (double)gpGlobals->time );
			w->Use( w, w, USE_TOGGLE, 0 );
		}
		if ( w && f == 199 )
			SampleWater( w, false );
		break;
	}
}

static const ScenDef s_wdefs[] =
{
	{ "func_water_points", 1, NULL, NULL },
	{ "func_water_toss", 120, NULL, NULL },
	{ "func_water_step", 120, NULL, NULL },
	{ "func_water_bot", 60, NULL, WaterTick },
	{ "func_water_moved", 200, NULL, WaterTick },
	{ "skins", 1, NULL, NULL },
};

static bool WaterSetup( void )
{
	s_water = NULL;
	if ( g_scenId == 5 )
	{
		Vector mn, mx;
		const char *model = Probe_PickModel( 64, 2 * R - 72, 48, 200, false, mn, mx );
		if ( !model ) return Scen_Skip( "no model" );
		static const int skins[] = { CONTENTS_WATER, CONTENTS_SLIME, CONTENTS_LAVA, -9 /* CONTENTS_CURRENT_0 */, CONTENTS_LADDER, CONTENTS_SOLID };
		static const int solids[] = { SOLID_NOT, SOLID_TRIGGER, SOLID_BSP };
		for ( int s = 0; s < 6; s++ ) for ( int k = 0; k < 3; k++ )
		{
			CProbeEnt *b = Probe_Platform( model, mn, mx, F.x, F.y, F.z + ( mx.z - mn.z - 2 ), solids[k], MOVETYPE_PUSH, NULL );
			if ( !b ) continue;
			b->pev->skin = skins[s];
			UTIL_SetOrigin( b->pev, b->pev->origin );
			Vector c = ( b->pev->absmin + b->pev->absmax ) * 0.5f;
			TraceResult tr;
			TRACE_HULL( c + Vector( -200, 0, 0 ), c + Vector( 200, 0, 0 ), ignore_monsters, 0, NULL, &tr );
			fprintf( g_scenFile, "{\"sc\":%d,\"skin\":%d,\"solid\":%d,\"i\":%d,\"c_centre\":%d,\"c_top_in\":%d,\"c_above\":%d,\"line\":{",
				g_scenId, skins[s], solids[k], b->entindex(), POINT_CONTENTS( c ), POINT_CONTENTS( Vector( c.x, c.y, b->pev->absmax.z - 2 ) ),
				POINT_CONTENTS( Vector( c.x, c.y, b->pev->absmax.z + 2 ) ) );
			Probe_PrTrace( g_scenFile, tr );
			fprintf( g_scenFile, "}}\n" );
			b->pev->skin = 0;	// a removed entity stays linked until the frame ends: no contents meanwhile
			Probe_Free( b );
		}
		return true;
	}
	CBaseEntity *w = FuncWater();
	if ( !w ) return Scen_Skip( "cannot place func_water" );
	Vector c = ( w->pev->absmin + w->pev->absmax ) * 0.5f;
	switch ( g_scenId )
	{
	case 0: SampleWater( w, true ); return true;
	case 1: Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, Vector( -8, -8, -8 ), Vector( 8, 8, 8 ), Vector( c.x, c.y, w->pev->absmax.z + 40 ) ); return true;
	case 2:
	{
		CProbeEnt *m = Scen_Ent( SOLID_SLIDEBOX, MOVETYPE_STEP, Vector( -16, -16, 0 ), Vector( 16, 16, 72 ), Vector( c.x, c.y, w->pev->absmax.z + 40 ) );
		m->pev->flags |= FL_MONSTER;
		return true;
	}
	case 3:
	{
		edict_t *bot = Probe_Bot();
		if ( !bot ) return Scen_Skip( "no fake client" );
		Probe_BotReset( Vector( c.x, c.y, F.z + 37 ), SOLID_SLIDEBOX );
		Scen_Track( bot );
		return true;
	}
	case 4: SampleWater( w, false ); return true;
	}
	return false;
}

bool Cap_WWater( int step )
{
	static ScenDef d[sizeof( s_wdefs ) / sizeof( s_wdefs[0] )];
	const int n = sizeof( s_wdefs ) / sizeof( s_wdefs[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_wdefs[i]; d[i].setup = WaterSetup; }
	return Scen_Capture( step, "trace-and-hulls", "W-water", d, n, 160, 256 );
}

// ---------------------------------------------------------------- L-illum

#define NPTS 48
static Vector s_lpts[NPTS];
static EHANDLE s_probe, s_light;

static void Illum( int f )
{
	CBaseEntity *e = s_probe;
	if ( !e ) return;
	fprintf( g_scenFile, "{\"sc\":%d,\"f\":%d,\"t\":%.9g,\"illum\":[", g_scenId, f, (double)gpGlobals->time );
	int n = g_scenId == 2 ? 1 : NPTS;
	for ( int i = 0; i < n; i++ )
	{
		SET_ORIGIN( e->edict(), s_lpts[i] );
		fprintf( g_scenFile, "%s%d", i ? "," : "", GETENTITYILLUM( e->edict() ) );
	}
	fprintf( g_scenFile, "]}\n" );
}

static void IllumTick( int f )
{
	switch ( g_scenId )
	{
	case 1:
		if ( f == 10 ) LIGHT_STYLE( 0, (char *)"a" );
		if ( f == 30 ) LIGHT_STYLE( 0, (char *)"z" );
		if ( f == 50 ) LIGHT_STYLE( 0, (char *)"m" );
		if ( f == 10 || f == 30 || f == 50 ) fprintf( g_scenFile, "{\"sc\":%d,\"f\":%d,\"lightstyle0\":\"%s\"}\n", g_scenId, f, f == 10 ? "a" : f == 30 ? "z" : "m" );
		break;
	case 2:
	{
		CBaseEntity *l = s_light;
		if ( l && f == 10 ) { l->pev->effects = EF_BRIGHTLIGHT; fprintf( g_scenFile, "{\"sc\":%d,\"f\":%d,\"effects\":%d}\n", g_scenId, f, EF_BRIGHTLIGHT ); }
		break;
	}
	case 3:
	{
		edict_t *bot = Probe_Bot();
		if ( bot )
		{
			Probe_BotMove( g_vecZero, 0, 0, 0, 0, 10 );
			fprintf( g_scenFile, "{\"sc\":%d,\"f\":%d,\"bot_illum\":%d,\"light_level\":%d}\n", g_scenId, f, GETENTITYILLUM( bot ), bot->v.light_level );
		}
		return;
	}
	}
	Illum( f );
}

static const ScenDef s_ldefs[] =
{
	{ "sample_points", 200, NULL, IllumTick },
	{ "lightstyle0_set", 60, NULL, IllumTick },
	{ "brightlight_effect", 20, NULL, IllumTick },
	{ "bot", 20, NULL, IllumTick },
};

static bool IllumSetup( void )
{
	if ( g_scenId == 0 )
	{
		// floor points 16 u up, anywhere in the map
		int n = 0;
		s_lpts[n++] = F + Vector( 0, 0, 16 );
		for ( int t = 0; t < 400000 && n < NPTS; t++ )
		{
			Vector p( g_scenRng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), g_scenRng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
				g_scenRng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
			p = Vector( floorf( p.x ), floorf( p.y ), floorf( p.z ) );
			if ( POINT_CONTENTS( p ) != CONTENTS_EMPTY ) continue;
			TraceResult tr;
			TRACE_LINE( p, p - Vector( 0, 0, 1024 ), ignore_monsters, NULL, &tr );
			if ( tr.flFraction == 1.0f || tr.fStartSolid || tr.vecPlaneNormal.z < 0.7f ) continue;
			Vector q = tr.vecEndPos + Vector( 0, 0, 16 );
			if ( POINT_CONTENTS( q ) != CONTENTS_EMPTY ) continue;
			s_lpts[n++] = q;
		}
		while ( n < NPTS ) { s_lpts[n] = s_lpts[0]; n++; }
		fprintf( g_scenFile, "{\"sc\":%d,\"points\":[", g_scenId );
		for ( int i = 0; i < NPTS; i++ ) fprintf( g_scenFile, "%s" VF, i ? "," : "", VA( s_lpts[i] ) );
		fprintf( g_scenFile, "]}\n" );
	}
	CProbeEnt *e = Probe_Ent( SOLID_NOT, MOVETYPE_NONE, g_vecZero, g_vecZero, s_lpts[0] );
	Scen_Own( e );
	s_probe = e;
	s_light = NULL;
	if ( g_scenId == 2 )
	{
		CProbeEnt *l = Probe_Ent( SOLID_NOT, MOVETYPE_NONE, g_vecZero, g_vecZero, s_lpts[0] + Vector( 0, 0, 16 ) );
		Scen_Own( l );
		s_light = l;
	}
	if ( g_scenId == 3 )
	{
		edict_t *bot = Probe_Bot();
		if ( !bot ) return Scen_Skip( "no fake client" );
		Probe_BotReset( s_lpts[0] + Vector( 0, 0, 21 ), SOLID_SLIDEBOX );
	}
	return true;
}

bool Cap_LIllum( int step )
{
	static ScenDef d[sizeof( s_ldefs ) / sizeof( s_ldefs[0] )];
	const int n = sizeof( s_ldefs ) / sizeof( s_ldefs[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_ldefs[i]; d[i].setup = IllumSetup; }
	return Scen_Capture( step, "lightstyles", "L-illum", d, n, 64, 128 );
}
