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
// Link and touch captures for docs/specs/area-links-and-touch.md.
// A-touch: linked boxes read back (A1), trigger touches against a box
// trigger at controlled gaps for movers of each kind, set_origin versus
// physics links (A3/A4), touch order of overlapping triggers, impact touch
// order and the SOLID_NOT rule (A7). A-shape: a brush trigger whose brush
// does not fill its bounds, probed by the fake client's player move and by
// a physics mover at sample points (A5). Multi-frame: the engine runs
// physics (and the touches it causes) between two StartFrame calls.

#include "probe_int.h"
#include <math.h>

static ProbeRng s_rng;
static FILE *s_f;
static Vector s_S, s_park;

static void PrState( const char *ev, CBaseEntity *e )
{
	fprintf( s_f, "{\"ev\":\"%s\",\"case\":%d,\"frame\":%d,\"t\":%.9g,\"ent\":", ev, g_probeCase, g_probeFrame, (double)gpGlobals->time );
	Probe_PrEnt( s_f, e->edict() );
	fprintf( s_f, ",\"v\":" VF ",\"ground\":%d}\n", VA( e->pev->velocity ), Probe_Idx( e->pev->groundentity ) );
}

// ---------------------------------------------------------------- A-touch

enum { MV_FLY, MV_ITEM, MV_NOT, MV_TRIG, MV_NONE, MV_TOSS, MV_BOT };
struct ACase { char name[40]; int kind; int axis; float gap; int extra; };
static ACase s_cases[128];
static int s_ncases;
static CProbeEnt *s_T1, *s_T2, *s_T3, *s_M;

static void AddCase( int kind, int axis, float gap, int extra = 0 )
{
	static const char *kn[] = { "fly", "item", "not", "trigger", "none", "toss", "bot" };
	ACase &c = s_cases[s_ncases++];
	snprintf( c.name, sizeof( c.name ), "%s_%c_%g%s", kn[kind], "xyz"[axis], gap, extra ? "_order" : "" );
	c.kind = kind;
	c.axis = axis;
	c.gap = gap;
	c.extra = extra;
}

// Mover origin whose box face (+axis side of the trigger) is `gap` from the trigger's face.
static Vector CasePos( const ACase &c, const Vector &mmins )
{
	Vector p = s_S;
	p[c.axis] = s_S[c.axis] + 32 + c.gap - mmins[c.axis];
	return p;
}

static void LinkReadback( void )
{
	struct LB { const char *name; int solid; int flags; int brush; Vector ang; };
	static const LB lbs[] =
	{
		{ "box_bbox", SOLID_BBOX, 0, 0, Vector( 0, 0, 0 ) },
		{ "box_slidebox", SOLID_SLIDEBOX, 0, 0, Vector( 0, 0, 0 ) },
		{ "box_trigger", SOLID_TRIGGER, 0, 0, Vector( 0, 0, 0 ) },
		{ "box_not", SOLID_NOT, 0, 0, Vector( 0, 0, 0 ) },
		{ "box_item", SOLID_TRIGGER, PROBE_FL_ITEM, 0, Vector( 0, 0, 0 ) },
		{ "box_item_bbox", SOLID_BBOX, PROBE_FL_ITEM, 0, Vector( 0, 0, 0 ) },
		{ "box_yaw30", SOLID_BBOX, 0, 0, Vector( 0, 30, 0 ) },
		{ "brush_bsp", SOLID_BSP, 0, 1, Vector( 0, 0, 0 ) },
		{ "brush_bsp_yaw30", SOLID_BSP, 0, 1, Vector( 0, 30, 0 ) },
		{ "brush_bsp_pitch20", SOLID_BSP, 0, 1, Vector( 20, 0, 0 ) },
		{ "brush_trigger_yaw30", SOLID_TRIGGER, 0, 1, Vector( 0, 30, 0 ) },
	};
	Vector mn, mx;
	const char *model = Probe_PickBoxModel( 16, 256, &mn, &mx );
	for ( unsigned i = 0; i < sizeof( lbs ) / sizeof( lbs[0] ); i++ )
	{
		const LB &l = lbs[i];
		CProbeEnt *e;
		if ( l.brush )
		{
			if ( !model ) continue;
			e = Probe_BrushEnt( l.solid, model, s_S, l.ang );
		}
		else
		{
			e = Probe_Ent( l.solid, MOVETYPE_NONE, Vector( -16, -12, -36 ), Vector( 20, 16, 30 ), s_S );
			e->pev->flags = l.flags;
			e->pev->angles = l.ang;
			UTIL_SetOrigin( e->pev, s_S + Vector( 0.25f, 0.5f, 0.125f ) );
		}
		fprintf( s_f, "{\"linkbox\":\"%s\",\"ent\":", l.name );
		Probe_PrEnt( s_f, e->edict() );
		fprintf( s_f, "}\n" );
		Probe_Free( e );
	}
}

static void ConfigMover( const ACase &c )
{
	entvars_t *v = s_M->pev;
	v->solid = c.kind == MV_NOT ? SOLID_NOT : c.kind == MV_TRIG ? SOLID_TRIGGER : SOLID_BBOX;
	v->movetype = c.kind == MV_NONE ? MOVETYPE_NONE : c.kind == MV_TOSS ? MOVETYPE_TOSS : MOVETYPE_FLY;
	v->flags = c.kind == MV_ITEM ? PROBE_FL_ITEM : 0;
	v->velocity = v->avelocity = g_vecZero;
	v->groundentity = NULL;
}

bool Cap_ATouch( int step )
{
	static int phase, caseIdx, sub;
	static CProbeEnt *A, *B;
	if ( step == 0 )
	{
		s_rng.Seed( Probe_Map(), "A-touch" );
		s_f = Probe_Open( "area-links-and-touch", "A-touch", &s_rng );
		if ( !s_f ) return true;
		if ( !Probe_OpenSpot( s_rng, 192, s_S ) && !Probe_OpenSpot( s_rng, 160, s_S ) && !Probe_OpenSpot( s_rng, 128, s_S ) && !Probe_OpenSpot( s_rng, 96, s_S ) && !Probe_OpenSpot( s_rng, 64, s_S ) )
		{
			fprintf( s_f, "{\"skipped\":\"no open spot\"}\n" );
			Probe_Close( s_f );
			return true;
		}
		s_park = s_S + Vector( 0, -120, 0 );
		fprintf( s_f, "{\"spot\":" VF ",\"park\":" VF "}\n", VA( s_S ), VA( s_park ) );
		LinkReadback();
		s_ncases = 0;
		static const float gaps[] = { -8, 0, 0.5f, 1, 1.5f, 1.99f, 2, 2.01f, 2.5f, 3 };
		for ( int axis = 0; axis < 3; axis++ )
			for ( unsigned g = 0; g < sizeof( gaps ) / sizeof( gaps[0] ); g++ )
				AddCase( MV_FLY, axis, gaps[g] );
		static const float igaps[] = { 0, 2, 14, 15.99f, 16, 16.01f, 17, 17.01f, 18 };
		for ( unsigned g = 0; g < sizeof( igaps ) / sizeof( igaps[0] ); g++ )
			AddCase( MV_ITEM, 0, igaps[g] );
		AddCase( MV_ITEM, 2, 1.99f );
		AddCase( MV_ITEM, 2, 2.01f );
		AddCase( MV_NOT, 0, -8 );
		AddCase( MV_TRIG, 0, -8 );
		AddCase( MV_NONE, 0, -8 );
		AddCase( MV_TOSS, 0, -8 );
		static const float bgaps[] = { -8, 0, 1.99f, 2, 2.01f };
		for ( unsigned g = 0; g < sizeof( bgaps ) / sizeof( bgaps[0] ); g++ )
			AddCase( MV_BOT, 0, bgaps[g] );
		AddCase( MV_FLY, 0, -8, 1 );	// three overlapping triggers
		AddCase( MV_BOT, 0, -8, 1 );
		s_T1 = Probe_Ent( SOLID_TRIGGER, MOVETYPE_NONE, Vector( -32, -32, -32 ), Vector( 32, 32, 32 ), s_S );
		s_M = Probe_Ent( SOLID_BBOX, MOVETYPE_FLY, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ), s_park );
		s_T2 = s_T3 = NULL;
		CBaseEntity *ents[2] = { s_T1, s_M };
		fprintf( s_f, "{\"setup\":\"touch\",\"trigger\":" );
		Probe_PrEnt( s_f, ents[0]->edict() );
		fprintf( s_f, ",\"mover\":" );
		Probe_PrEnt( s_f, ents[1]->edict() );
		fprintf( s_f, "}\n" );
		g_probeTouchLog = s_f;
		phase = 0;
		caseIdx = 0;
		sub = 0;
		Probe_Bot();
		return false;
	}
	if ( phase == 0 )
	{
		// gap cases: even sub = place, odd sub = read back and park
		if ( caseIdx >= s_ncases )
		{
			phase = 1;
			caseIdx = 0;
			sub = 0;
			s_T1->pev->solid = SOLID_NOT;
			UTIL_SetOrigin( s_T1->pev, s_S );
			if ( s_T2 ) Probe_Free( s_T2 );
			if ( s_T3 ) Probe_Free( s_T3 );
			s_M->pev->solid = SOLID_NOT;
			s_M->pev->movetype = MOVETYPE_NONE;
			UTIL_SetOrigin( s_M->pev, s_park );
			Probe_BotPark();
			return false;
		}
		const ACase &c = s_cases[caseIdx];
		g_probeCase = caseIdx;
		if ( sub == 0 )
		{
			if ( c.extra && !s_T2 )
			{
				s_T2 = Probe_Ent( SOLID_TRIGGER, MOVETYPE_NONE, Vector( -24, -40, -24 ), Vector( 40, 24, 24 ), s_S );
				s_T3 = Probe_Ent( SOLID_TRIGGER, MOVETYPE_NONE, Vector( -8, -8, -8 ), Vector( 8, 8, 8 ), s_S + Vector( 4, 0, 0 ) );
				fprintf( s_f, "{\"setup\":\"order\",\"triggers\":[%d,%d,%d]}\n", s_T1->entindex(), s_T2->entindex(), s_T3->entindex() );
			}
			fprintf( s_f, "{\"case\":%d,\"name\":\"%s\",\"kind\":%d,\"axis\":%d,\"gap\":%.9g}\n", caseIdx, c.name, c.kind, c.axis, (double)c.gap );
			if ( c.kind == MV_BOT )
			{
				edict_t *bot = Probe_Bot();
				Vector pos = CasePos( c, bot->v.mins );
				g_probePhase = "set_origin";
				Probe_BotReset( pos, SOLID_SLIDEBOX );
				g_probePhase = "pmove";
				Probe_BotMove( g_vecZero, 0, 0, 0, 0, 1 );
				fprintf( s_f, "{\"ev\":\"bot\",\"case\":%d,\"pos\":" VF ",\"after\":" VF ",\"absmin\":" VF ",\"absmax\":" VF "}\n",
					caseIdx, VA( pos ), VA( bot->v.origin ), VA( bot->v.absmin ), VA( bot->v.absmax ) );
			}
			else
			{
				ConfigMover( c );
				g_probePhase = "set_origin";
				UTIL_SetOrigin( s_M->pev, CasePos( c, s_M->pev->mins ) );
				PrState( "placed", s_M );
			}
			g_probePhase = "physics";
			sub = 1;
			return false;
		}
		if ( c.kind == MV_BOT )
			Probe_BotPark();
		else
		{
			PrState( "after", s_M );
			g_probePhase = "park";
			s_M->pev->velocity = g_vecZero;
			UTIL_SetOrigin( s_M->pev, s_park );
		}
		g_probePhase = "physics_parked";
		sub = 0;
		caseIdx++;
		return false;
	}

	// phase 1: impact scenarios, 40 frames each
	struct Imp { const char *name; int movetype; int solid; int aMode; int bMode; Vector from; Vector vel; };
	static const Imp imps[] =
	{
		{ "fly_into_box", MOVETYPE_FLY, SOLID_BBOX, PT_LOG, PT_LOG, Vector( -70, 0, 0 ), Vector( 300, 0, 0 ) },
		{ "fly_self_not", MOVETYPE_FLY, SOLID_BBOX, PT_SELF_NOT, PT_LOG, Vector( -70, 0, 0 ), Vector( 300, 0, 0 ) },
		{ "fly_solid_not", MOVETYPE_FLY, SOLID_NOT, PT_LOG, PT_LOG, Vector( -70, 0, 0 ), Vector( 300, 0, 0 ) },
		{ "toss_onto_box", MOVETYPE_TOSS, SOLID_BBOX, PT_LOG, PT_LOG, Vector( 0, 0, 100 ), Vector( 0, 0, 0 ) },
		{ "bounce_into_box", MOVETYPE_BOUNCE, SOLID_BBOX, PT_LOG, PT_LOG, Vector( -70, 0, 20 ), Vector( 300, 0, 50 ) },
		{ "target_removes_mover", MOVETYPE_FLY, SOLID_BBOX, PT_LOG, PT_REMOVE_OTHER, Vector( -70, 0, 0 ), Vector( 300, 0, 0 ) },
		{ "mover_removes_self", MOVETYPE_FLY, SOLID_BBOX, PT_REMOVE_SELF, PT_LOG, Vector( -70, 0, 0 ), Vector( 300, 0, 0 ) },
	};
	const int nimps = sizeof( imps ) / sizeof( imps[0] );
	// EHANDLEs: a touch may remove A or B; the engine frees them at frame end.
	static EHANDLE hA, hB;
	if ( caseIdx >= nimps )
	{
		if ( (CBaseEntity *)hA ) Probe_Free( hA );
		if ( (CBaseEntity *)hB ) Probe_Free( hB );
		Probe_Free( s_T1 );
		Probe_Free( s_M );
		g_probeTouchLog = NULL;
		Probe_Close( s_f );
		return true;
	}
	const Imp &im = imps[caseIdx];
	g_probeCase = 1000 + caseIdx;
	if ( sub == 0 )
	{
		if ( (CBaseEntity *)hA ) Probe_Free( hA );
		if ( (CBaseEntity *)hB ) Probe_Free( hB );
		B = Probe_Ent( SOLID_BBOX, MOVETYPE_NONE, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ), s_S );
		A = Probe_Ent( im.solid, im.movetype, Vector( -8, -8, -8 ), Vector( 8, 8, 8 ), s_S + im.from );
		A->m_touchMode = im.aMode;
		B->m_touchMode = im.bMode;
		A->pev->velocity = im.vel;
		A->pev->gravity = 1;
		A->pev->friction = 0.5f;
		hA = A;
		hB = B;
		fprintf( s_f, "{\"case\":%d,\"name\":\"%s\",\"a\":%d,\"b\":%d}\n", g_probeCase, im.name, A->entindex(), B->entindex() );
		PrState( "a", A );
		PrState( "b", B );
	}
	else
	{
		CBaseEntity *a = hA;
		if ( a && !( a->pev->flags & FL_KILLME ) ) PrState( "a", a );
		else fprintf( s_f, "{\"ev\":\"a_removed\",\"case\":%d,\"frame\":%d}\n", g_probeCase, g_probeFrame );
	}
	g_probePhase = "physics";
	if ( ++sub >= 40 )
	{
		sub = 0;
		caseIdx++;
	}
	return false;
}

// ---------------------------------------------------------------- A-shape

struct ASample { Vector p; int in0, in1, in1u, box; };
static ASample s_samples[512];
static int s_nsamples;

// Fraction of interior lattice points of a brush model that are inside it.
static float FillFraction( edict_t *e )
{
	int inside = 0, total = 0;
	int saved = e->v.solid;
	e->v.solid = SOLID_BSP;
	Vector mn = e->v.mins, sz = e->v.maxs - e->v.mins;
	for ( int a = 1; a <= 5; a++ ) for ( int b = 1; b <= 5; b++ ) for ( int c = 1; c <= 5; c++ )
	{
		Vector p = e->v.origin + mn + Vector( sz.x * a / 6, sz.y * b / 6, sz.z * c / 6 );
		TraceResult tr;
		TRACE_MODEL( p, p, 0, e, &tr );
		inside += tr.fStartSolid ? 1 : 0;
		total++;
	}
	e->v.solid = saved;
	return (float)inside / total;
}

static void ShapeSamples( CProbeEnt *T )
{
	int saved = T->pev->solid;
	T->pev->solid = SOLID_BSP;
	s_nsamples = 0;
	Vector lo = T->pev->absmin - Vector( 16, 16, 36 ), hi = T->pev->absmax + Vector( 16, 16, 36 );
	for ( int a = 0; a < 8; a++ ) for ( int b = 0; b < 8; b++ ) for ( int c = 0; c < 4; c++ )
	{
		Vector p( lo.x + ( hi.x - lo.x ) * ( a + 0.5f ) / 8, lo.y + ( hi.y - lo.y ) * ( b + 0.5f ) / 8, lo.z + ( hi.z - lo.z ) * ( c + 0.5f ) / 4 );
		s_samples[s_nsamples++].p = p;
	}
	while ( s_nsamples < 320 )
		s_samples[s_nsamples++].p = Vector( s_rng.Uni( lo.x, hi.x ), s_rng.Uni( lo.y, hi.y ), s_rng.Uni( lo.z, hi.z ) );
	for ( int i = 0; i < s_nsamples; i++ )
	{
		ASample &s = s_samples[i];
		TraceResult tr;
		TRACE_MODEL( s.p, s.p, 0, T->edict(), &tr );
		s.in0 = tr.fStartSolid;
		TRACE_MODEL( s.p, s.p, 1, T->edict(), &tr );
		s.in1 = tr.fStartSolid;
		Vector ang = T->pev->angles;	// the same point against the unrotated brush
		T->pev->angles = g_vecZero;
		TRACE_MODEL( s.p, s.p, 1, T->edict(), &tr );
		s.in1u = tr.fStartSolid;
		T->pev->angles = ang;
		Vector amn = s.p + Vector( -16, -16, -36 ), amx = s.p + Vector( 16, 16, 36 );
		s.box = !( amn.x > T->pev->absmax.x || amn.y > T->pev->absmax.y || amn.z > T->pev->absmax.z ||
			amx.x < T->pev->absmin.x || amx.y < T->pev->absmin.y || amx.z < T->pev->absmin.z );
	}
	T->pev->solid = saved;
	UTIL_SetOrigin( T->pev, T->pev->origin );
}

bool Cap_AShape( int step )
{
	static CProbeEnt *T, *M;
	static int variant, idx, sub;
	static Vector org;
	static char model[16];
	static float fill;
	if ( step == 0 )
	{
		s_rng.Seed( Probe_Map(), "A-shape" );
		s_f = Probe_Open( "area-links-and-touch", "A-shape", &s_rng );
		if ( !s_f ) return true;
		// the map brush model that least fills its bounds (an angled trigger, a wedge)
		float best = 2;
		model[0] = 0;
		edict_t *src = NULL;
		for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
		{
			edict_t *e = INDEXENT( i );
			if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' ) continue;
			Vector sz = e->v.maxs - e->v.mins;
			if ( sz.x < 48 || sz.y < 48 || sz.z < 48 || sz.x > 768 || sz.y > 768 || sz.z > 512 ) continue;
			if ( e->v.angles != g_vecZero ) continue;
			float f = FillFraction( e );
			fprintf( s_f, "{\"candidate\":%d,\"class\":\"%s\",\"model\":\"%s\",\"fill\":%.4g}\n", i, STRING( e->v.classname ), STRING( e->v.model ), (double)f );
			// prefer triggers (A5 is about brush triggers), then any brush entity
			float score = f + ( strncmp( STRING( e->v.classname ), "trigger_", 8 ) ? 1.0f : 0.0f );
			if ( f > 0.1f && f < 0.9f && score < best )
			{
				best = score;
				src = e;
			}
		}
		if ( !src )
		{
			fprintf( s_f, "{\"skipped\":\"no non-box brush model\",\"best_fill\":%.4g}\n", (double)best );
			Probe_Close( s_f );
			return true;
		}
		strncpy( model, STRING( src->v.model ), sizeof( model ) - 1 );
		org = src->v.origin;
		fill = best >= 1.0f ? best - 1.0f : best;
		fprintf( s_f, "{\"source\":" );
		Probe_PrEnt( s_f, src );
		fprintf( s_f, ",\"fill\":%.4g}\n", (double)fill );
		if ( src->v.solid != SOLID_NOT )
		{
			src->v.solid = SOLID_NOT;
			SET_ORIGIN( src, src->v.origin );
		}
		M = Probe_Ent( SOLID_BBOX, MOVETYPE_FLY, Vector( -16, -16, -36 ), Vector( 16, 16, 36 ), org );
		M->pev->solid = SOLID_NOT;
		M->pev->movetype = MOVETYPE_NONE;
		g_probeTouchLog = s_f;
		variant = -1;
		idx = 0;
		sub = 0;
		Probe_Bot();
	}
	if ( variant < 0 || idx >= s_nsamples )
	{
		static const Vector angs[2] = { Vector( 0, 0, 0 ), Vector( 0, 30, 0 ) };
		if ( T ) Probe_Free( T );
		T = NULL;
		if ( ++variant >= 2 )
		{
			Probe_Free( M );
			g_probeTouchLog = NULL;
			Probe_Close( s_f );
			return true;
		}
		T = Probe_BrushEnt( SOLID_TRIGGER, model, org, angs[variant] );
		T->pev->movetype = MOVETYPE_NONE;
		UTIL_SetOrigin( T->pev, org );
		ShapeSamples( T );
		fprintf( s_f, "{\"variant\":%d,\"trigger\":", variant );
		Probe_PrEnt( s_f, T->edict() );
		fprintf( s_f, "}\n" );
		idx = 0;
		sub = 0;
		return false;
	}
	// per sample: sub 0 = bot player move at the point and place the
	// physics mover; sub 1 = read back the mover after its physics frame.
	ASample &s = s_samples[idx];
	g_probeCase = variant * 1000 + idx;
	if ( sub == 0 )
	{
		fprintf( s_f, "{\"case\":%d,\"variant\":%d,\"p\":" VF ",\"in_hull0\":%d,\"in_hull1\":%d,\"in_hull1_unrotated\":%d,\"box_overlap\":%d}\n",
			g_probeCase, variant, VA( s.p ), s.in0, s.in1, s.in1u, s.box );
		edict_t *bot = Probe_Bot();
		if ( bot )
		{
			g_probePhase = "set_origin";
			Probe_BotReset( s.p, SOLID_SLIDEBOX );
			g_probePhase = "pmove";
			Probe_BotMove( g_vecZero, 0, 0, 0, 0, 1 );
			fprintf( s_f, "{\"ev\":\"bot\",\"case\":%d,\"after\":" VF "}\n", g_probeCase, VA( bot->v.origin ) );
			Probe_BotPark();
		}
		M->pev->solid = SOLID_BBOX;
		M->pev->movetype = MOVETYPE_FLY;
		M->pev->velocity = g_vecZero;
		M->pev->flags = 0;
		g_probePhase = "set_origin";
		UTIL_SetOrigin( M->pev, s.p );
		g_probePhase = "physics";
		sub = 1;
		return false;
	}
	PrState( "after", M );
	M->pev->solid = SOLID_NOT;
	M->pev->movetype = MOVETYPE_NONE;
	g_probePhase = "park";
	UTIL_SetOrigin( M->pev, org );
	sub = 0;
	idx++;
	return false;
}
