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
// Clock and think captures for docs/specs/think-and-time.md (K1-K8):
// think timing of probe entities at frame times 0.01, 1/30 and 0.1 s
// (host_framerate), thinks set by other entities, the nextthink boundary,
// multi_manager / trigger_relay delays (game DLL entities firing probe
// targets), pusher local time, edict allocation and reuse, and the map
// start (K-settle: what probe.cpp recorded from map load on).

#include "probe_int.h"
#include <math.h>
#include <string.h>

#define F g_scenFloor
#define R g_scenRadius

static const float DT30 = 1.0f / 30.0f;

static CProbeEnt *Thinker( const Vector &o, int mode, float period, int count )
{
	CProbeEnt *e = Scen_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), o );
	e->m_thinkMode = mode;
	e->m_period = period;
	e->m_thinkCount = count;
	return e;
}

static void SetNT( CBaseEntity *e, float nt, const char *what )
{
	e->pev->nextthink = nt;
	fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"schedule\",\"i\":%d,\"what\":\"%s\",\"t\":%.9g,\"ft\":%.9g,\"nt\":%.9g,\"ltime\":%.9g}\n",
		g_scenId, e->entindex(), what, (double)gpGlobals->time, (double)gpGlobals->frametime, (double)nt, (double)e->pev->ltime );
}

static void SetFramerate( float fr )
{
	CVAR_SET_FLOAT( "host_framerate", fr );
	fprintf( g_scenFile, "{\"sc\":%d,\"host_framerate\":%.9g,\"t\":%.9g}\n", g_scenId, (double)fr, (double)gpGlobals->time );
}

// ---------------------------------------------------------------- K-think

static EHANDLE s_k[8];
static int s_nk;

static void KeyValue( edict_t *e, const char *cls, const char *k, const char *v )
{
	KeyValueData kvd;
	kvd.szClassName = (char *)cls;
	kvd.szKeyName = (char *)k;
	kvd.szValue = (char *)v;
	kvd.fHandled = 0;
	DispatchKeyValue( e, &kvd );
}

static CBaseEntity *GameEnt( const char *cls, const char *const *kv, int n )
{
	edict_t *e = CREATE_NAMED_ENTITY( MAKE_STRING( cls ) );
	if ( FNullEnt( e ) ) return NULL;
	for ( int i = 0; i + 1 < n; i += 2 ) KeyValue( e, cls, kv[i], kv[i + 1] );
	DispatchSpawn( e );
	CBaseEntity *b = CBaseEntity::Instance( e );
	Scen_Own( b );
	return b;
}

static bool MultiManager( void )
{
	static const char *names[] = { "hl64_t1", "hl64_t2", "hl64_t3", "hl64_t4", "hl64_t5" };
	for ( int i = 0; i < 5; i++ )
	{
		CProbeEnt *t = Scen_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), F + Vector( 0, 0, 32 ) );
		t->pev->targetname = MAKE_STRING( names[i] );
	}
	static const char *relay[] = { "targetname", "hl64_relay", "target", "hl64_t5", "delay", "0.15", "triggerstate", "2" };
	CBaseEntity *r = GameEnt( "trigger_relay", relay, 8 );
	static const char *mm[] = { "targetname", "hl64_mm", "hl64_t1", "0", "hl64_t2", "0.05", "hl64_t3", "0.1", "hl64_t4", "0.1",
		"hl64_relay", "0", "hl64_t1#2", "0.33" };
	CBaseEntity *m = GameEnt( "multi_manager", mm, 14 );
	if ( !r || !m ) return Scen_Skip( "cannot create multi_manager / trigger_relay" );
	s_k[0] = m;
	fprintf( g_scenFile, "{\"sc\":%d,\"multi_manager\":%d,\"relay\":%d,\"targets\":{\"hl64_t1\":[0,0.33],\"hl64_t2\":0.05,\"hl64_t3\":0.1,\"hl64_t4\":0.1,\"hl64_relay\":0,\"hl64_t5\":\"relay+0.15\"}}\n",
		g_scenId, m->entindex(), r->entindex() );
	return true;
}

static void FireMM( void )
{
	CBaseEntity *m = s_k[0];
	if ( !m ) return;
	fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"fire\",\"i\":%d,\"t\":%.9g,\"ft\":%.9g}\n", g_scenId, m->entindex(), (double)gpGlobals->time, (double)gpGlobals->frametime );
	m->Use( m, m, USE_TOGGLE, 0 );
}

// six entities around the `nextthink <= time + frametime` boundary
static void Boundary( void )
{
	float t = gpGlobals->time, ft = gpGlobals->frametime;
	const float nts[6] = { t + ft, t + ft + 0.0001f, t + ft - 0.0001f, t + 2 * ft, 0, t - 0.5f };
	static const char *what[6] = { "t+ft", "t+ft+1e-4", "t+ft-1e-4", "t+2ft", "unset", "t-0.5" };
	for ( int i = 0; i < 6; i++ )
	{
		CProbeEnt *e = Thinker( F + Vector( 0, 0, 32 ), TH_LOG, 0, 0 );
		SetNT( e, nts[i], what[i] );
	}
}

static void ThinkTick( int f )
{
	switch ( g_scenId )
	{
	case 0: if ( f == 0 ) SetNT( s_k[0], gpGlobals->time, "now" ); break;
	case 1: if ( f == 1 ) SetNT( s_k[0], gpGlobals->time, "now" ); break;
	case 2: case 3: if ( f == 1 ) SetNT( s_k[0], gpGlobals->time + 0.03f, "setter" ); break;
	case 4: if ( f == 0 ) SetNT( s_k[0], gpGlobals->time, "now" ); break;
	case 5: if ( f == 0 ) Boundary(); break;
	case 6: if ( f == 1 ) Boundary(); break;
	case 7:
		if ( f == 1 )
		{
			const float t = gpGlobals->time;
			static const float d[4] = { 0.05f, 0.1f, 0.15f, -0.05f };
			static const char *w[4] = { "t+0.05", "t+0.1", "t+0.15", "t-0.05" };
			for ( int i = 0; i < 4; i++ ) SetNT( Thinker( F + Vector( 0, 0, 32 ), TH_LOG, 0, 0 ), t + d[i], w[i] );
			CProbeEnt *r = Thinker( F + Vector( 0, 0, 32 ), TH_RESCHED, 0.12f, 0 );
			SetNT( r, t, "now_resched_0.12" );
		}
		break;
	case 8: if ( f == 5 ) FireMM(); break;
	case 9: if ( f == 2 ) FireMM(); break;
	}
}

static const ScenDef s_think[] =
{
	{ "resched012_dt001", 150, NULL, ThinkTick },
	{ "resched012_dt30", 70, NULL, ThinkTick },
	{ "lower_sets_higher", 10, NULL, ThinkTick },
	{ "higher_sets_lower", 10, NULL, ThinkTick },
	{ "self_now", 10, NULL, ThinkTick },
	{ "boundary_dt001", 6, NULL, ThinkTick },
	{ "boundary_dt30", 8, NULL, ThinkTick },
	{ "dt01", 15, NULL, ThinkTick },
	{ "multi_manager_dt001", 60, NULL, ThinkTick },
	{ "multi_manager_dt30", 30, NULL, ThinkTick },
};

static bool ThinkSetup( void )
{
	s_nk = 0;
	for ( int i = 0; i < 8; i++ ) s_k[i] = NULL;
	Vector o = F + Vector( 0, 0, 32 );
	switch ( g_scenId )
	{
	case 0: s_k[0] = Thinker( o, TH_RESCHED, 0.12f, 0 ); return true;
	case 1: SetFramerate( DT30 ); s_k[0] = Thinker( o, TH_RESCHED, 0.12f, 0 ); return true;
	case 2: case 3:
	{
		CProbeEnt *a = Thinker( o, TH_LOG, 0, 0 ), *b = Thinker( o, TH_LOG, 0, 0 );
		if ( a->entindex() > b->entindex() ) { CProbeEnt *t = a; a = b; b = t; }
		CProbeEnt *setter = g_scenId == 2 ? a : b, *other = g_scenId == 2 ? b : a;
		setter->m_thinkMode = TH_SET_OTHER;
		setter->m_other = other;
		s_k[0] = setter;
		fprintf( g_scenFile, "{\"sc\":%d,\"setter\":%d,\"other\":%d}\n", g_scenId, setter->entindex(), other->entindex() );
		return true;
	}
	case 4: s_k[0] = Thinker( o, TH_RESCHED, 0, 5 ); return true;
	case 5: return true;
	case 6: SetFramerate( DT30 ); return true;
	case 7: SetFramerate( 0.1f ); return true;
	case 8: return MultiManager();
	case 9: SetFramerate( DT30 ); return MultiManager();
	}
	return false;
}

bool Cap_KThink( int step )
{
	static ScenDef d[sizeof( s_think ) / sizeof( s_think[0] )];
	const int n = sizeof( s_think ) / sizeof( s_think[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_think[i]; d[i].setup = ThinkSetup; }
	return Scen_Capture( step, "think-and-time", "K-think", d, n, 64, 128 );
}

// ---------------------------------------------------------------- K-pusher

static EHANDLE s_p;

static void PusherTick( int f )
{
	CBaseEntity *p = s_p;
	if ( !p ) return;
	CProbeEnt *pe = (CProbeEnt *)p;
	switch ( g_scenId )
	{
	case 0: case 1:
		if ( f == ( g_scenId == 0 ? 5 : 2 ) )
		{
			p->pev->velocity = Vector( 0, 0, 64 );
			pe->m_thinkMode = TH_STOP;
			SetNT( p, p->pev->ltime + 50.0f / 64.0f, "ltime+50/64" );
		}
		break;
	case 2:
		if ( f == 5 )
		{
			pe->m_thinkMode = TH_RESCHED;
			pe->m_period = 0.25f;
			pe->m_thinkCount = 4;
			SetNT( p, p->pev->ltime + 0.25f, "ltime+0.25" );
		}
		break;
	case 3:
		if ( f == 10 )
		{
			p->pev->velocity = Vector( 100, 0, 0 );
			pe->m_thinkMode = TH_STOP;
			SetNT( p, p->pev->ltime + 1.0f, "ltime+1.0" );
		}
		break;
	case 4:
		if ( f == 5 )
		{
			p->pev->velocity = Vector( 0, 0, 64 );
			pe->m_thinkMode = TH_STOP;
			SetNT( p, p->pev->ltime + 0.005f, "ltime+0.005" );
		}
		break;
	}
}

static const ScenDef s_pusher[] =
{
	{ "door_dt001", 120, NULL, PusherTick },
	{ "door_dt30", 50, NULL, PusherTick },
	{ "think_zero_velocity", 120, NULL, PusherTick },
	{ "blocked", 160, NULL, PusherTick },
	{ "subframe", 20, NULL, PusherTick },
	{ "idle", 30, NULL, PusherTick },
};

static bool PusherSetup( void )
{
	s_p = NULL;
	Vector mn, mx;
	if ( g_scenId == 3 )
	{
		// a door pushing a crate jammed against a static brush
		const char *model = Probe_PickModel( 48, 128, 16, 256, false, mn, mx );
		if ( !model ) return Scen_Skip( "no pusher model" );
		float doorTop = F.z - 4 + ( mx.z - mn.z - 2 );	// bottom 4 u under the floor
		CProbeEnt *p = Probe_Platform( model, mn, mx, F.x - R * 0.5f, F.y, doorTop, SOLID_BSP, MOVETYPE_PUSH, NULL );
		if ( !p ) return Scen_Skip( "pusher not placed" );
		Scen_Own( p );
		Scen_Track( p->edict() );
		s_p = p;
		float face = p->pev->absmax.x - 1;
		CProbeEnt *c = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, Vector( -12, -12, 0 ), Vector( 12, 12, 24 ), Vector( face + 16, F.y, F.z ) );
		c->pev->flags |= FL_ONGROUND;
		c->pev->groundentity = INDEXENT( 0 );
		float hx = ( mx.x - mn.x ) * 0.5f - 1;
		CProbeEnt *b = Probe_Platform( model, mn, mx, face + 28 + 8 + hx, F.y, doorTop, SOLID_BSP, MOVETYPE_PUSH, NULL );
		if ( !b ) return Scen_Skip( "blocker not placed" );
		Scen_Own( b );
		Scen_Track( b->edict() );
		return true;
	}
	if ( g_scenId == 1 ) SetFramerate( DT30 );
	const char *model = Probe_PickModel( 32, 192, 8, 96, false, mn, mx );
	if ( !model ) return Scen_Skip( "no pusher model" );
	CProbeEnt *p = Probe_Platform( model, mn, mx, F.x, F.y, F.z + 128, SOLID_BSP, MOVETYPE_PUSH, NULL );
	if ( !p ) return Scen_Skip( "pusher not placed" );
	Scen_Own( p );
	Scen_Track( p->edict() );
	s_p = p;
	return true;
}

bool Cap_KPusher( int step )
{
	static ScenDef d[sizeof( s_pusher ) / sizeof( s_pusher[0] )];
	const int n = sizeof( s_pusher ) / sizeof( s_pusher[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_pusher[i]; d[i].setup = PusherSetup; }
	return Scen_Capture( step, "think-and-time", "K-pusher", d, n, 192, 256 );
}

// ---------------------------------------------------------------- K-alloc

#define MAX_ALLOC 1024
static edict_t *s_alloc[MAX_ALLOC];
static int s_nalloc, s_hi;
static edict_t *s_E[6];

// INDEXENT hides free edicts and edicts without game data: read the edict
// array directly. A slot is in use when not free and it has game data or a
// classname (raw allocations below get the classname "hl64_raw"); never
// used slots are zero.
static edict_t *Edict( int i )
{
	return INDEXENT( 0 ) + i;
}

static bool Used( edict_t *e )
{
	return !e->free && ( e->pvPrivateData || e->v.classname );
}

static int HighestUsed( void )
{
	int hi = 0;
	for ( int i = 0; i < gpGlobals->maxEntities; i++ )
		if ( Used( Edict( i ) ) ) hi = i;
	return hi;
}

// every freed slot: [index, freetime, serial]
static void Snapshot( const char *label )
{
	int hi = HighestUsed();
	fprintf( g_scenFile, "{\"sc\":%d,\"snapshot\":\"%s\",\"t\":%.9g,\"highest_used\":%d,\"free\":[", g_scenId, label, (double)gpGlobals->time, hi );
	bool first = true;
	for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )	// every freed slot (never-used slots are zero)
	{
		edict_t *e = Edict( i );
		if ( !e->free ) continue;
		fprintf( g_scenFile, "%s[%d,%.9g,%d]", first ? "" : ",", i, (double)e->freetime, e->serialnumber );
		first = false;
	}
	fprintf( g_scenFile, "]}\n" );
}

static edict_t *Alloc( const char *label )
{
	int hi = HighestUsed();
	edict_t *e = CREATE_ENTITY();
	if ( !e ) return NULL;
	if ( s_nalloc < MAX_ALLOC ) s_alloc[s_nalloc++] = e;
	fprintf( g_scenFile, "{\"sc\":%d,\"alloc\":\"%s\",\"t\":%.9g,\"i\":%d,\"serial\":%d,\"freetime\":%.9g,\"highest_used_before\":%d,"
		"\"o\":" VF ",\"fl\":%d,\"classname\":%d,\"solid\":%d}\n",
		g_scenId, label, (double)gpGlobals->time, (int)( e - Edict( 0 ) ), e->serialnumber, (double)e->freetime, hi,
		VA( e->v.origin ), e->v.flags, (int)e->v.classname, e->v.solid );
	e->v.classname = MAKE_STRING( "hl64_raw" );
	return e;
}

static void FreeNow( edict_t *e, const char *label )
{
	e->v.origin = Vector( 1, 2, 3 );	// a reused slot shows whether fields are cleared
	e->v.flags = FL_FLY;
	REMOVE_ENTITY( e );
	fprintf( g_scenFile, "{\"sc\":%d,\"free\":\"%s\",\"t\":%.9g,\"i\":%d,\"serial\":%d,\"freetime\":%.9g}\n",
		g_scenId, label, (double)gpGlobals->time, (int)( e - Edict( 0 ) ), e->serialnumber, (double)e->freetime );
}

static void AllocTick( int f )
{
	switch ( f )
	{
	case 1:
		FreeNow( s_E[1], "E1" );
		FreeNow( s_E[3], "E3" );
		s_E[5]->v.flags |= FL_KILLME;	// freed by the engine at the end of this frame
		fprintf( g_scenFile, "{\"sc\":%d,\"killme\":\"E5\",\"t\":%.9g,\"i\":%d}\n", g_scenId, (double)gpGlobals->time, ENTINDEX( s_E[5] ) );
		break;
	case 2: case 3: case 26: case 50: case 51: case 52: case 53: case 61: case 70:
	{
		char l[16];
		snprintf( l, sizeof( l ), "f%d", f );
		Snapshot( l );
		Alloc( l );
		break;
	}
	case 79:
		for ( int i = 0; i < s_nalloc; i++ )	// a reused slot appears twice; the second sees it free
			if ( !s_alloc[i]->free ) REMOVE_ENTITY( s_alloc[i] );
		s_nalloc = 0;
		Snapshot( "end" );
		break;
	}
}

static bool AllocSetup( void )
{
	s_nalloc = 0;
	Snapshot( "start" );
	s_hi = HighestUsed();
	// fill every free slot below the highest used one
	int fillers = 0;
	for ( int k = 0; k < 600; k++ )
	{
		edict_t *e = Alloc( "filler" );
		if ( !e ) break;
		fillers++;
		if ( e->serialnumber == 0 && e->freetime == 0 ) break;	// a never-used slot: no reusable one left
	}
	fprintf( g_scenFile, "{\"sc\":%d,\"fillers\":%d}\n", g_scenId, fillers );
	for ( int i = 0; i < 6; i++ )
	{
		char l[8];
		snprintf( l, sizeof( l ), "E%d", i );
		s_E[i] = Alloc( l );
	}
	Snapshot( "filled" );
	return true;
}

static const ScenDef s_alloc_s[] = { { "alloc_reuse", 80, NULL, AllocTick } };

bool Cap_KAlloc( int step )
{
	static ScenDef d[1];
	d[0] = s_alloc_s[0];
	d[0].setup = AllocSetup;
	return Scen_Capture( step, "think-and-time", "K-alloc", d, 1, 64, 128 );
}

// ---------------------------------------------------------------- K-settle

bool Cap_KSettle( int step )
{
	ProbeRng rng;
	rng.Seed( Probe_Map(), "K-settle" );
	FILE *f = Probe_Open( "think-and-time", "K-settle", &rng );
	if ( !f ) return true;
	Probe_WriteEarly( f );
	Probe_Close( f );
	return true;
}
