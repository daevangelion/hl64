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
// A-order (docs/specs/area-links-and-touch.md 3.1, A9): the order in which
// the engine touches overlapping map triggers. Runs live (map triggers left
// linked as the map left them; their touches are recorded and swallowed, so
// no map logic runs). Phases:
//  1. static: every pair of SOLID_TRIGGER entities whose linked boxes are
//     within 64 u gets, per mover hull (0, 1, 3, 2), up to two origins where
//     a mover would touch both (box overlap, origin inside each brush
//     trigger's clip hull for that mover, trigger angles ignored); a FLY
//     mover of that size is set there by set_origin and the touches of the
//     physics link that follows are recorded in order; the fake client's
//     1 ms player move at the hull-1 origin likewise. Map triggers are
//     never relinked in this phase.
//  2. runtime: probe trigger B created, then A in a lower slot (a filler
//     freed 0.6 s earlier), both after the map and inside a map trigger.
//  3. start-off: SOLID_NOT trigger_hurt / trigger_push / trigger_monsterjump
//     turned on as HLSDK CBaseTrigger::ToggleUse does, in index order, and
//     their pairs measured.
//  4. toggle: per pair measured in phase 1, the trigger touched first is
//     turned off and on again (ToggleUse twice) and the pair re-measured at
//     the same origin; toggles accumulate in pair order.

#include "probe_int.h"
#include <math.h>
#include <string.h>

#define MAX_TRIG 256
#define MAX_PLACE 16384
#define MAX_PAIRS 4096

static ProbeRng s_rng;
static FILE *s_f;

struct OHull { const char *name; int hull; Vector mins, maxs; };
static const OHull s_hulls[4] =
{
	{ "point", 0, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ) },
	{ "human", 1, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ) },
	{ "head", 3, Vector( -16, -16, -18 ), Vector( 16, 16, 18 ) },
	{ "large", 2, Vector( -32, -32, -32 ), Vector( 32, 32, 32 ) },
};

struct Place { int a, b; int h; Vector p; int bot; int phase; int toggle; int tog; };
static Place *s_place;
static int s_nplace, s_iplace;
static edict_t *s_trig[MAX_TRIG];
static int s_ntrig;
static CProbeEnt *s_M;
static int s_touched[64], s_ntouched;
static edict_t *s_mover;
static int s_phase;		// phase tag of placements queued now
// first order measured per pair in phase 1 (fly mover): the toggle phase input
static int s_pairA[MAX_PAIRS], s_pairB[MAX_PAIRS], s_pairFirst[MAX_PAIRS], s_pairPlace[MAX_PAIRS], s_npairs;

static void OnTouch( edict_t *self, edict_t *other )
{
	if ( other == s_mover && s_ntouched < 64 )
		s_touched[s_ntouched++] = ENTINDEX( self );
}

static bool IsBrush( edict_t *e )
{
	return e->v.model && STRING( e->v.model )[0] == '*';
}

static bool StartOffClass( edict_t *e )
{
	return FClassnameIs( e, "trigger_hurt" ) || FClassnameIs( e, "trigger_push" ) || FClassnameIs( e, "trigger_monsterjump" );
}

// A mover of hull h at origin p touches trigger t: real boxes overlap (the
// linked boxes' 1 u widening is margin), and for a brush trigger p is inside
// its clip hull h in the trigger's unrotated frame.
static bool Inside( edict_t *t, int h, const Vector &p )
{
	const OHull &H = s_hulls[h];
	for ( int k = 0; k < 3; k++ )
		if ( p[k] + H.maxs[k] < t->v.absmin[k] || p[k] + H.mins[k] > t->v.absmax[k] ) return false;
	if ( !IsBrush( t ) ) return true;
	// field changes only, no relink: the trigger keeps its place in the engine's lists
	int solid = t->v.solid;
	Vector ang = t->v.angles;
	t->v.solid = SOLID_BSP;
	t->v.angles = g_vecZero;
	TraceResult tr;
	TRACE_MODEL( p, p, H.hull, t, &tr );
	t->v.solid = solid;
	t->v.angles = ang;
	return tr.fStartSolid != 0;
}

static bool Near( edict_t *a, edict_t *b, float d )
{
	for ( int k = 0; k < 3; k++ )
		if ( a->v.absmin[k] > b->v.absmax[k] + d || b->v.absmin[k] > a->v.absmax[k] + d ) return false;
	return true;
}

static void AddPlace( int a, int b, int h, const Vector &p, int bot, int toggle, int tog )
{
	if ( s_nplace >= MAX_PLACE ) return;
	Place &q = s_place[s_nplace++];
	q.a = a; q.b = b; q.h = h; q.p = p; q.bot = bot; q.phase = s_phase; q.toggle = toggle; q.tog = tog;
}

// Up to two origins (first found, farthest from it) per hull where a mover
// touches both triggers; a line per pair and hull; placements queued.
static int PairPlaces( edict_t *A, edict_t *B )
{
	int found = 0;
	for ( int h = 0; h < 4; h++ )
	{
		const OHull &H = s_hulls[h];
		Vector lo, hi;
		bool ok = true;
		for ( int k = 0; k < 3; k++ )
		{
			lo[k] = fmaxf( A->v.absmin[k], B->v.absmin[k] ) - H.maxs[k];
			hi[k] = fminf( A->v.absmax[k], B->v.absmax[k] ) - H.mins[k];
			ok = ok && lo[k] <= hi[k];
		}
		Vector first, far;
		float farD = -1;
		int n = 0;
		const int N = 7;
		for ( int i = 0; ok && i < N; i++ ) for ( int j = 0; j < N; j++ ) for ( int k = 0; k < N; k++ )
		{
			Vector p( lo.x + ( hi.x - lo.x ) * ( i + 0.5f ) / N, lo.y + ( hi.y - lo.y ) * ( j + 0.5f ) / N, lo.z + ( hi.z - lo.z ) * ( k + 0.5f ) / N );
			p = Vector( floorf( p.x * 8 ) / 8, floorf( p.y * 8 ) / 8, floorf( p.z * 8 ) / 8 );
			if ( !Inside( A, h, p ) || !Inside( B, h, p ) ) continue;
			if ( n++ == 0 ) { first = p; continue; }
			float d = ( p - first ).Length();
			if ( d > farD ) { farD = d; far = p; }
		}
		fprintf( s_f, "{\"pair\":[%d,%d],\"phase\":%d,\"hull\":%d,\"mover\":\"%s\",\"found\":%s,\"candidates\":%d", ENTINDEX( A ), ENTINDEX( B ),
			s_phase, H.hull, H.name, n ? "true" : "false", n );
		if ( n ) fprintf( s_f, ",\"p\":" VF, VA( first ) );
		fprintf( s_f, "}\n" );
		if ( !n ) continue;
		found++;
		AddPlace( ENTINDEX( A ), ENTINDEX( B ), h, first, 0, 0, -1 );
		if ( farD >= 4 ) AddPlace( ENTINDEX( A ), ENTINDEX( B ), h, far, 0, 0, -1 );
		if ( H.hull == 1 ) AddPlace( ENTINDEX( A ), ENTINDEX( B ), h, first, 1, 0, -1 );
	}
	return found;
}

static void PrTrigger( edict_t *e, bool startOff )
{
	char tn[64];
	int lump = Probe_LumpIndex( e, tn, sizeof( tn ) );
	fprintf( s_f, "{\"trigger\":{\"i\":%d,\"class\":\"%s\",\"model\":\"%s\",\"targetname\":\"%s\",\"lump\":%d,\"start_off\":%s,"
		"\"o\":" VF ",\"a\":" VF ",\"mins\":" VF ",\"maxs\":" VF ",\"absmin\":" VF ",\"absmax\":" VF ",\"solid\":%d,\"spawnflags\":%d}}\n",
		ENTINDEX( e ), STRING( e->v.classname ), e->v.model ? STRING( e->v.model ) : "", tn, lump, startOff ? "true" : "false",
		VA( e->v.origin ), VA( e->v.angles ), VA( e->v.mins ), VA( e->v.maxs ), VA( e->v.absmin ), VA( e->v.absmax ),
		e->v.solid, e->v.spawnflags );
}

// As HLSDK CBaseTrigger::ToggleUse.
static void ToggleTrigger( edict_t *e )
{
	if ( e->v.solid == SOLID_NOT )
	{
		e->v.solid = SOLID_TRIGGER;
		gpGlobals->force_retouch++;
	}
	else
		e->v.solid = SOLID_NOT;
	SET_ORIGIN( e, e->v.origin );
}

static void WritePlace( const Place &q, const Vector &after )
{
	fprintf( s_f, "{\"place\":%d,\"phase\":%d,\"pair\":[%d,%d],\"hull\":%d,\"mover\":\"%s\",\"p\":" VF ",\"after\":" VF,
		s_iplace, q.phase, q.a, q.b, s_hulls[q.h].hull, q.bot ? "bot" : "fly", VA( q.p ), VA( after ) );
	if ( q.toggle ) fprintf( s_f, ",\"after_toggle\":%d", q.toggle );
	fprintf( s_f, ",\"touched\":[" );
	int ia = -1, ib = -1;
	for ( int i = 0; i < s_ntouched; i++ )
	{
		fprintf( s_f, "%s%d", i ? "," : "", s_touched[i] );
		if ( s_touched[i] == q.a && ia < 0 ) ia = i;
		if ( s_touched[i] == q.b && ib < 0 ) ib = i;
	}
	fprintf( s_f, "]" );
	if ( ia >= 0 && ib >= 0 )
	{
		int first = ia < ib ? q.a : q.b;
		fprintf( s_f, ",\"order\":[%d,%d]", first, ia < ib ? q.b : q.a );
		if ( q.phase == 1 && !q.bot )
		{
			bool seen = false;
			for ( int k = 0; k < s_npairs && !seen; k++ ) seen = s_pairA[k] == q.a && s_pairB[k] == q.b;
			if ( !seen && s_npairs < MAX_PAIRS )
			{
				s_pairA[s_npairs] = q.a;
				s_pairB[s_npairs] = q.b;
				s_pairFirst[s_npairs] = first;
				s_pairPlace[s_npairs] = s_iplace;
				s_npairs++;
			}
		}
	}
	fprintf( s_f, "}\n" );
}

bool Cap_AOrder( int step )
{
	static int stage, sub, waitUntil, runtimeM;
	static EHANDLE hA, hB;
	static Vector park, runtimeP;
	if ( step == 0 )
	{
		s_rng.Seed( Probe_Map(), "A-order" );
		s_f = Probe_Open( "area-links-and-touch", "A-order", &s_rng );
		if ( !s_f ) return true;
		if ( !g_probeLive )
		{
			fprintf( s_f, "{\"skipped\":\"A-order must run live (probe_run live)\"}\n" );
			Probe_Close( s_f );
			return true;
		}
		if ( !s_place ) s_place = (Place *)calloc( MAX_PLACE, sizeof( Place ) );
		s_nplace = s_iplace = s_ntrig = s_npairs = 0;
		int nOff = 0;
		for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
		{
			edict_t *e = INDEXENT( i );
			if ( !e || e->free || ( e->v.flags & FL_KILLME ) || FClassnameIs( e, "probe_ent" ) ) continue;
			bool off = e->v.solid == SOLID_NOT && IsBrush( e ) && StartOffClass( e );
			if ( e->v.solid != SOLID_TRIGGER && !off ) continue;
			PrTrigger( e, off );
			nOff += off;
			if ( !off && s_ntrig < MAX_TRIG ) s_trig[s_ntrig++] = e;
		}
		s_phase = 1;
		int pairs = 0, found = 0;
		for ( int a = 0; a < s_ntrig; a++ )
			for ( int b = a + 1; b < s_ntrig; b++ )
			{
				if ( !Near( s_trig[a], s_trig[b], 64 ) ) continue;
				pairs++;
				found += PairPlaces( s_trig[a], s_trig[b] ) > 0;
			}
		fprintf( s_f, "{\"static\":{\"triggers\":%d,\"start_off\":%d,\"pairs_within_64\":%d,\"pairs_cotouchable\":%d,\"placements\":%d}}\n",
			s_ntrig, nOff, pairs, found, s_nplace );
		if ( !Probe_OpenSpot( s_rng, 128, park ) ) park = g_probeWorldMins + Vector( 8, 8, 8 );
		s_M = Probe_Ent( SOLID_BBOX, MOVETYPE_FLY, s_hulls[1].mins, s_hulls[1].maxs, park );
		s_mover = s_M->edict();
		g_probeTouchLog = NULL;		// placements carry the touches
		g_probeTouchFn = OnTouch;
		g_probePhase = "physics";
		Probe_Bot();
		stage = 1;
		sub = 0;
		return false;
	}

	// the placement the engine's physics just linked
	if ( sub == 1 )
	{
		WritePlace( s_place[s_iplace], s_M->pev->origin );
		s_iplace++;
		sub = 0;
	}
	while ( s_iplace < s_nplace )
	{
		Place &q = s_place[s_iplace];
		if ( q.tog >= 0 )
		{
			edict_t *t = INDEXENT( q.tog );
			ToggleTrigger( t );
			ToggleTrigger( t );
			fprintf( s_f, "{\"toggle\":%d,\"ent\":%d,\"pair\":[%d,%d],\"solid\":%d,\"absmin\":" VF ",\"absmax\":" VF "}\n",
				q.toggle, q.tog, q.a, q.b, t->v.solid, VA( t->v.absmin ), VA( t->v.absmax ) );
			q.tog = -1;
		}
		// a trigger turned on sets force_retouch: the engine relinks every
		// entity with touch for a few frames (and the triggers themselves),
		// so a placement waits until that is over and measures one link
		static int waited;
		if ( gpGlobals->force_retouch > 0 )
		{
			waited++;
			return false;
		}
		if ( waited )
		{
			fprintf( s_f, "{\"force_retouch_frames\":%d,\"before_place\":%d}\n", waited, s_iplace );
			waited = 0;
		}
		s_ntouched = 0;
		if ( q.bot )
		{
			edict_t *bot = Probe_Bot();
			if ( bot )
			{
				s_mover = bot;
				g_probePhase = "set_origin";
				Probe_BotReset( q.p, SOLID_SLIDEBOX );
				g_probePhase = "pmove";
				Probe_BotMove( g_vecZero, 0, 0, 0, 0, 1 );
				Vector after = bot->v.origin;
				Probe_BotPark();
				s_mover = s_M->edict();
				g_probePhase = "physics";
				WritePlace( q, after );
			}
			s_iplace++;
			continue;
		}
		const OHull &H = s_hulls[q.h];
		g_probePhase = "set_origin";
		s_M->pev->velocity = g_vecZero;
		UTIL_SetSize( s_M->pev, H.mins, H.maxs );
		UTIL_SetOrigin( s_M->pev, q.p );
		g_probePhase = "physics";
		sub = 1;
		return false;
	}

	// the queue is empty: next stage
	switch ( stage )
	{
	case 1:
	{
		// runtime triggers inside a brush map trigger (hull-1 interior)
		edict_t *M = NULL;
		for ( int i = 0; i < s_ntrig && !M; i++ )
		{
			edict_t *t = s_trig[i];
			if ( !IsBrush( t ) ) continue;
			for ( int k = 0; k < 64 && !M; k++ )
			{
				Vector q = ( t->v.absmin + t->v.absmax ) * 0.5f;
				if ( k ) q = Vector( s_rng.Uni( t->v.absmin.x, t->v.absmax.x ), s_rng.Uni( t->v.absmin.y, t->v.absmax.y ), s_rng.Uni( t->v.absmin.z, t->v.absmax.z ) );
				q = Vector( floorf( q.x ), floorf( q.y ), floorf( q.z ) );
				if ( Inside( t, 1, q ) && Inside( t, 0, q ) ) { M = t; runtimeP = q; }
			}
		}
		stage = 2;
		if ( !M )
		{
			fprintf( s_f, "{\"runtime\":\"skipped\",\"reason\":\"no brush trigger with a hull-1 interior\"}\n" );
			waitUntil = step;
			hB = NULL;
			runtimeM = -1;
			return false;
		}
		runtimeM = ENTINDEX( M );
		// F takes the lowest reusable slot, B the next; F is freed and, 0.6 s
		// later, A takes F's slot: A is created after B with a lower index
		CProbeEnt *F = Probe_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), park );
		CProbeEnt *B = Probe_Ent( SOLID_TRIGGER, MOVETYPE_NONE, Vector( -20, -20, -20 ), Vector( 20, 20, 20 ), runtimeP );
		hB = B;
		fprintf( s_f, "{\"runtime\":\"B\",\"map_trigger\":%d,\"p\":" VF ",\"filler\":%d,\"B\":%d,\"t\":%.9g}\n",
			runtimeM, VA( runtimeP ), F->entindex(), B->entindex(), (double)gpGlobals->time );
		REMOVE_ENTITY( F->edict() );
		waitUntil = step + 60;
		return false;
	}
	case 2:
	{
		if ( step < waitUntil ) return false;
		stage = 3;
		if ( runtimeM < 0 ) return false;
		CProbeEnt *A = Probe_Ent( SOLID_TRIGGER, MOVETYPE_NONE, Vector( -20, -20, -20 ), Vector( 20, 20, 20 ), runtimeP );
		hA = A;
		CBaseEntity *B = hB;
		int bi = B ? B->entindex() : -1;
		fprintf( s_f, "{\"runtime\":\"A\",\"A\":%d,\"B\":%d,\"t\":%.9g}\n", A->entindex(), bi, (double)gpGlobals->time );
		s_phase = 2;
		for ( int h = 0; h < 2; h++ )
		{
			AddPlace( runtimeM, A->entindex(), h, runtimeP, 0, 0, -1 );
			AddPlace( runtimeM, bi, h, runtimeP, 0, 0, -1 );
			AddPlace( bi, A->entindex(), h, runtimeP, 0, 0, -1 );
		}
		AddPlace( bi, A->entindex(), 1, runtimeP, 1, 0, -1 );
		return false;
	}
	case 3:
	{
		if ( (CBaseEntity *)hA ) Probe_Free( hA );
		if ( (CBaseEntity *)hB ) Probe_Free( hB );
		hA = hB = NULL;
		// start-off triggers on, in index order, then their pairs
		stage = 4;
		s_phase = 3;
		int start = s_ntrig, pairs = 0, found = 0;
		for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities && s_ntrig < MAX_TRIG; i++ )
		{
			edict_t *e = INDEXENT( i );
			if ( !e || e->free || e->v.solid != SOLID_NOT || !IsBrush( e ) || !StartOffClass( e ) || FClassnameIs( e, "probe_ent" ) ) continue;
			ToggleTrigger( e );
			fprintf( s_f, "{\"enabled\":%d,\"class\":\"%s\",\"solid\":%d,\"absmin\":" VF ",\"absmax\":" VF "}\n",
				i, STRING( e->v.classname ), e->v.solid, VA( e->v.absmin ), VA( e->v.absmax ) );
			s_trig[s_ntrig++] = e;
		}
		for ( int a = 0; a < s_ntrig; a++ )
			for ( int b = ( a + 1 > start ? a + 1 : start ); b < s_ntrig; b++ )
			{
				if ( !Near( s_trig[a], s_trig[b], 64 ) ) continue;
				pairs++;
				found += PairPlaces( s_trig[a], s_trig[b] ) > 0;
			}
		fprintf( s_f, "{\"start_off\":{\"enabled\":%d,\"pairs_within_64\":%d,\"pairs_cotouchable\":%d}}\n", s_ntrig - start, pairs, found );
		return false;
	}
	case 4:
	{
		// toggle phase: per static pair, toggle its first-touched trigger and
		// re-measure at the origin of its first static measurement
		stage = 5;
		s_phase = 4;
		for ( int i = 0; i < s_npairs; i++ )
		{
			const Place &q = s_place[s_pairPlace[i]];
			AddPlace( q.a, q.b, q.h, q.p, 0, i + 1, s_pairFirst[i] );
		}
		fprintf( s_f, "{\"toggle_phase\":{\"pairs\":%d}}\n", s_npairs );
		return false;
	}
	}
	Probe_Free( s_M );
	s_M = NULL;
	s_mover = NULL;
	g_probeTouchFn = NULL;
	fprintf( s_f, "{\"done\":{\"placements\":%d,\"pairs_ordered\":%d}}\n", s_iplace, s_npairs );
	Probe_Close( s_f );
	return true;
}
