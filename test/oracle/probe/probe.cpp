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
// Probe core: the `probe_run` server command, the per-frame capture
// scheduler, map preparation (monsters removed, map triggers disabled so
// bots cannot change level), JSONL output, the probe entity class, the
// fake client and the PM_Move wrapper through which captures query the
// engine's pmove services.

#include "probe_int.h"
#include "probe.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#define PROBE_VERSION "hl64-probe 1"

extern "C" void PM_Move( struct playermove_s *ppmove, int server );

FILE *g_probeTouchLog;
const char *g_probePhase = "";
int g_probeCase;
Vector g_probeWorldMins, g_probeWorldMaxs;
int g_probeFrame;
float g_probeScale = 1.0f;
ProbePmFn g_probePmPre, g_probePmPost;

static char s_outDir[512] = "/scratch/hl64/fixtures";

struct ProbeCapture { const char *name; const char *spec; ProbeStepFn fn; };
static const ProbeCapture s_caps[] =
{
	{ "T-points",   "trace-and-hulls",      Cap_TPoints },
	{ "T-segments", "trace-and-hulls",      Cap_TSegments },
	{ "T-allsolid", "trace-and-hulls",      Cap_TAllsolid },
	{ "T-entities", "trace-and-hulls",      Cap_TEntities },
	{ "T-rotated",  "trace-and-hulls",      Cap_TRotated },
	{ "T-hitbox",   "trace-and-hulls",      Cap_THitbox },
	{ "A-touch",    "area-links-and-touch", Cap_ATouch },
	{ "A-shape",    "area-links-and-touch", Cap_AShape },
	{ "PM-bot",     "pmove-host",           Cap_PMBot },
};
#define NUM_CAPS ( (int)( sizeof( s_caps ) / sizeof( s_caps[0] ) ) )

static int s_queue[32], s_numQueued, s_cur = -1, s_step;
static bool s_prepared, s_quitWhenDone;
static float s_startAt;
static edict_t *s_bot;
static Vector s_botPark;
static char s_removed[4096], s_disabled[4096];

// ---------------------------------------------------------------- rng

void ProbeRng::Seed( const char *a, const char *b )
{
	unsigned h = 2166136261u;
	for ( const char *p = a; *p; p++ ) h = ( h ^ (unsigned char)*p ) * 16777619u;
	h = ( h ^ '/' ) * 16777619u;
	for ( const char *p = b; *p; p++ ) h = ( h ^ (unsigned char)*p ) * 16777619u;
	s = h ? h : 1;
}

unsigned ProbeRng::Next( void )
{
	s ^= s << 13; s ^= s >> 17; s ^= s << 5;
	return s;
}

float ProbeRng::Uni( float lo, float hi )
{
	return lo + ( hi - lo ) * (float)( ( Next() >> 8 ) * ( 1.0 / 16777216.0 ) );
}

int ProbeRng::Range( int lo, int hi )
{
	return lo + (int)( Next() % (unsigned)( hi - lo + 1 ) );
}

Vector ProbeRng::Dir( void )
{
	for ( ;; )
	{
		Vector v( Uni( -1, 1 ), Uni( -1, 1 ), Uni( -1, 1 ) );
		float l = v.Length();
		if ( l > 0.05f && l <= 1.0f )
			return v * ( 1.0f / l );
	}
}

// ---------------------------------------------------------------- output

const char *Probe_Map( void )
{
	return STRING( gpGlobals->mapname );
}

static void MkdirP( char *path )
{
	for ( char *p = path + 1; *p; p++ )
	{
		if ( *p != '/' ) continue;
		*p = 0; mkdir( path, 0755 ); *p = '/';
	}
	mkdir( path, 0755 );
}

void Probe_Log( const char *fmt, ... )
{
	char buf[512];
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	g_engfuncs.pfnServerPrint( buf );
}

int Probe_Idx( edict_t *e )
{
	return e ? ENTINDEX( e ) : -1;
}

FILE *Probe_Open( const char *spec, const char *capture, const ProbeRng *rng )
{
	char dir[768], path[1024];
	snprintf( dir, sizeof( dir ), "%s/goldsrc/%s/%s", s_outDir, spec, capture );
	MkdirP( dir );
	snprintf( path, sizeof( path ), "%s/%s.jsonl", dir, Probe_Map() );
	FILE *f = fopen( path, "w" );
	if ( !f )
	{
		Probe_Log( "probe: cannot open %s: %s\n", path, strerror( errno ) );
		return NULL;
	}
	fprintf( f, "{\"meta\":{\"capture\":\"%s\",\"spec\":\"%s\",\"map\":\"%s\",\"probe\":\"%s\","
		"\"seed\":%u,\"time\":%.9g,\"frametime\":%.9g,\"host_framerate\":%.9g,"
		"\"sv_gravity\":%.9g,\"sv_maxspeed\":%.9g,\"sv_stepsize\":%.9g,\"maxclients\":%d,"
		"\"world_mins\":" VF ",\"world_maxs\":" VF ","
		"\"removed_monsters\":[%s],\"disabled_triggers\":[%s]}}\n",
		capture, spec, Probe_Map(), PROBE_VERSION, rng ? rng->s : 0u,
		(double)gpGlobals->time, (double)gpGlobals->frametime, (double)CVAR_GET_FLOAT( "host_framerate" ),
		(double)CVAR_GET_FLOAT( "sv_gravity" ), (double)CVAR_GET_FLOAT( "sv_maxspeed" ),
		(double)CVAR_GET_FLOAT( "sv_stepsize" ), gpGlobals->maxClients,
		VA( g_probeWorldMins ), VA( g_probeWorldMaxs ), s_removed, s_disabled );
	Probe_PrBrushEnts( f );
	return f;
}

void Probe_Close( FILE *f )
{
	if ( f ) fclose( f );
}

void Probe_PrTrace( FILE *f, const TraceResult &tr )
{
	fprintf( f, "\"as\":%d,\"ss\":%d,\"io\":%d,\"iw\":%d,\"fr\":%.9g,\"end\":" VF ",\"n\":" VF ",\"d\":%.9g,\"hit\":%d,\"hg\":%d",
		tr.fAllSolid, tr.fStartSolid, tr.fInOpen, tr.fInWater, (double)tr.flFraction,
		VA( tr.vecEndPos ), VA( tr.vecPlaneNormal ), (double)tr.flPlaneDist, Probe_Idx( tr.pHit ), tr.iHitgroup );
}

void Probe_PrPmTrace( FILE *f, const pmtrace_t &tr )
{
	fprintf( f, "\"as\":%d,\"ss\":%d,\"io\":%d,\"iw\":%d,\"fr\":%.9g,\"end\":" VF ",\"n\":" VF ",\"d\":%.9g,\"pe\":%d,\"hg\":%d",
		tr.allsolid, tr.startsolid, tr.inopen, tr.inwater, (double)tr.fraction,
		VA( tr.endpos ), VA( tr.plane.normal ), (double)tr.plane.dist, tr.ent, tr.hitgroup );
}

void Probe_PrEnt( FILE *f, edict_t *e )
{
	entvars_t *v = &e->v;
	fprintf( f, "{\"i\":%d,\"class\":\"%s\",\"model\":\"%s\",\"o\":" VF ",\"a\":" VF ",\"mins\":" VF ",\"maxs\":" VF
		",\"absmin\":" VF ",\"absmax\":" VF ",\"solid\":%d,\"movetype\":%d,\"flags\":%d,\"skin\":%d,\"rendermode\":%d,\"owner\":%d}",
		ENTINDEX( e ), STRING( v->classname ), v->model ? STRING( v->model ) : "",
		VA( v->origin ), VA( v->angles ), VA( v->mins ), VA( v->maxs ), VA( v->absmin ), VA( v->absmax ),
		v->solid, v->movetype, v->flags, v->skin, v->rendermode, Probe_Idx( v->owner ) );
}

void Probe_PrBrushEnts( FILE *f )
{
	fprintf( f, "{\"brush_ents\":[" );
	bool first = true;
	for ( int i = 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model ) continue;
		if ( STRING( e->v.model )[0] != '*' ) continue;
		if ( !first ) fputc( ',', f );
		first = false;
		Probe_PrEnt( f, e );
	}
	fprintf( f, "]}\n" );
}

int Probe_Count( int n )
{
	int c = (int)( n * g_probeScale + 0.5f );
	return c < 1 ? 1 : c;
}

// ---------------------------------------------------------------- entities

LINK_ENTITY_TO_CLASS( probe_ent, CProbeEnt );

void CProbeEnt::Spawn( void )
{
}

void CProbeEnt::Touch( CBaseEntity *pOther )
{
	if ( g_probeTouchLog )
		fprintf( g_probeTouchLog, "{\"ev\":\"touch\",\"case\":%d,\"phase\":\"%s\",\"frame\":%d,\"t\":%.9g,\"self\":%d,\"other\":%d,"
			"\"self_solid\":%d,\"other_solid\":%d,\"other_o\":" VF "}\n",
			g_probeCase, g_probePhase, g_probeFrame, (double)gpGlobals->time, entindex(), pOther ? pOther->entindex() : -1,
			pev->solid, pOther ? pOther->pev->solid : -1, VA( pOther ? pOther->pev->origin : g_vecZero ) );
	switch ( m_touchMode )
	{
	case PT_SELF_NOT:
		pev->solid = SOLID_NOT;
		UTIL_SetOrigin( pev, pev->origin );
		break;
	case PT_REMOVE_SELF:	// as game code does: flagged, freed at frame end
		UTIL_Remove( this );
		break;
	case PT_REMOVE_OTHER:
		if ( pOther && !pOther->IsPlayer() ) UTIL_Remove( pOther );
		break;
	}
}

CProbeEnt *Probe_Ent( int solid, int movetype, const Vector &mins, const Vector &maxs, const Vector &org )
{
	CProbeEnt *e = GetClassPtr( (CProbeEnt *)NULL );
	e->pev->classname = MAKE_STRING( "probe_ent" );
	e->pev->solid = solid;
	e->pev->movetype = movetype;
	e->m_touchMode = PT_LOG;
	UTIL_SetSize( e->pev, mins, maxs );
	UTIL_SetOrigin( e->pev, org );
	return e;
}

CProbeEnt *Probe_BrushEnt( int solid, const char *model, const Vector &org, const Vector &ang )
{
	CProbeEnt *e = GetClassPtr( (CProbeEnt *)NULL );
	e->pev->classname = MAKE_STRING( "probe_ent" );
	e->pev->solid = solid;
	e->pev->movetype = MOVETYPE_PUSH;
	e->pev->angles = ang;
	e->m_touchMode = PT_LOG;
	SET_MODEL( e->edict(), STRING( ALLOC_STRING( model ) ) );	// the engine keeps the pointer
	UTIL_SetOrigin( e->pev, org );
	return e;
}

void Probe_Free( CBaseEntity *e )
{
	if ( !e ) return;
	e->pev->solid = SOLID_NOT;
	e->pev->movetype = MOVETYPE_NONE;
	e->pev->velocity = g_vecZero;
	UTIL_SetOrigin( e->pev, e->pev->origin );
	UTIL_Remove( e );
}

// ---------------------------------------------------------------- world helpers

bool Probe_OpenSpot( ProbeRng &rng, float clear, Vector &out )
{
	static const Vector axes[6] = { Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ),
		Vector( 0, -1, 0 ), Vector( 0, 0, 1 ), Vector( 0, 0, -1 ) };
	for ( int tries = 0; tries < 200000; tries++ )
	{
		Vector p( rng.Uni( g_probeWorldMins.x, g_probeWorldMaxs.x ), rng.Uni( g_probeWorldMins.y, g_probeWorldMaxs.y ),
			rng.Uni( g_probeWorldMins.z, g_probeWorldMaxs.z ) );
		p = Vector( floorf( p.x ), floorf( p.y ), floorf( p.z ) );
		if ( POINT_CONTENTS( p ) != CONTENTS_EMPTY ) continue;
		TraceResult tr;
		TRACE_HULL( p, p, ignore_monsters, 2, NULL, &tr );
		if ( tr.fStartSolid || tr.fAllSolid ) continue;
		bool ok = true;
		for ( int a = 0; a < 6 && ok; a++ )
		{
			TRACE_HULL( p, p + axes[a] * clear, ignore_monsters, 2, NULL, &tr );
			ok = tr.flFraction == 1.0f && !tr.fStartSolid;
		}
		if ( !ok ) continue;
		// no solid or trigger entity box near the spot either
		Vector lo = p - Vector( clear + 32, clear + 32, clear + 32 ), hi = p + Vector( clear + 32, clear + 32, clear + 32 );
		for ( int i = 1; i < gpGlobals->maxEntities && ok; i++ )
		{
			edict_t *e = INDEXENT( i );
			if ( !e || e->free || e->v.solid == SOLID_NOT ) continue;
			ok = e->v.absmin.x > hi.x || e->v.absmin.y > hi.y || e->v.absmin.z > hi.z ||
				e->v.absmax.x < lo.x || e->v.absmax.y < lo.y || e->v.absmax.z < lo.z;
		}
		if ( !ok ) continue;
		out = p;
		return true;
	}
	return false;
}

bool Probe_FloorBelow( const Vector &p, int hull, float maxDrop, Vector &origin, Vector &normal )
{
	TraceResult tr;
	TRACE_HULL( p, p - Vector( 0, 0, maxDrop ), ignore_monsters, hull, NULL, &tr );
	if ( tr.fStartSolid || tr.fAllSolid || tr.flFraction == 1.0f ) return false;
	origin = tr.vecEndPos;
	normal = tr.vecPlaneNormal;
	return true;
}

// The brush model (of any map entity) whose size is in range and which
// best fills its bounds; ties go to the model whose bounds centre is nearest
// its origin (an origin brush), so rotations stay local.
const char *Probe_PickBoxModel( float minSize, float maxSize, Vector *pmins, Vector *pmaxs )
{
	static char best[16];
	float bestScore = -1;
	best[0] = 0;
	for ( int i = 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' ) continue;
		if ( ( e->v.flags & FL_KILLME ) || FClassnameIs( e, "probe_ent" ) ) continue;
		Vector mn = e->v.mins, mx = e->v.maxs, sz = mx - mn;
		if ( sz.x < minSize || sz.y < minSize || sz.z < minSize ) continue;
		if ( sz.x > maxSize || sz.y > maxSize || sz.z > maxSize ) continue;
		if ( e->v.angles != g_vecZero ) continue;
		int inside = 0;
		for ( int a = 1; a <= 3; a++ ) for ( int b = 1; b <= 3; b++ ) for ( int c = 1; c <= 3; c++ )
		{
			Vector p = e->v.origin + mn + Vector( sz.x * a / 4, sz.y * b / 4, sz.z * c / 4 );
			TraceResult tr;
			TRACE_MODEL( p, p, 0, e, &tr );
			inside += tr.fStartSolid ? 1 : 0;
		}
		Vector c = ( mn + mx ) * 0.5f;
		float score = inside * 1000.0f - c.Length() / ( sz.Length() + 1 );
		if ( score > bestScore )
		{
			bestScore = score;
			strncpy( best, STRING( e->v.model ), sizeof( best ) - 1 );
			if ( pmins ) *pmins = mn;
			if ( pmaxs ) *pmaxs = mx;
		}
	}
	return best[0] ? best : NULL;
}

static void AppendIdx( char *buf, size_t size, int idx, const char *cls )
{
	size_t l = strlen( buf );
	if ( l + 48 >= size ) return;
	snprintf( buf + l, size - l, "%s[%d,\"%s\"]", l ? "," : "", idx, cls );
}

// Remove map monsters and disable map triggers: captures must be
// reproducible and bots must not change level. World bounds come from the
// map file (model 0 of the BSP models lump) since the world edict has none.
static void Probe_Prepare( void )
{
	g_probeWorldMins = Vector( -4096, -4096, -4096 );
	g_probeWorldMaxs = Vector( 4096, 4096, 4096 );
	char path[128];
	snprintf( path, sizeof( path ), "maps/%s.bsp", Probe_Map() );
	int len = 0;
	byte *bsp = LOAD_FILE_FOR_ME( path, &len );
	if ( bsp && len >= 4 + 15 * 8 )
	{
		const int *hdr = (const int *)bsp;
		int ofs = hdr[1 + 14 * 2], size = hdr[2 + 14 * 2];	// LUMP_MODELS
		if ( ofs > 0 && size >= 24 && ofs + 24 <= len )
		{
			const float *m = (const float *)( bsp + ofs );
			g_probeWorldMins = Vector( m[0], m[1], m[2] );
			g_probeWorldMaxs = Vector( m[3], m[4], m[5] );
		}
	}
	if ( bsp ) FREE_FILE( bsp );
	s_removed[0] = s_disabled[0] = 0;
	for ( int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free ) continue;
		CBaseEntity *ent = CBaseEntity::Instance( e );
		if ( e->v.flags & FL_MONSTER )
		{
			AppendIdx( s_removed, sizeof( s_removed ), i, STRING( e->v.classname ) );
			if ( ent ) Probe_Free( ent );
			else REMOVE_ENTITY( e );
		}
		else if ( e->v.solid == SOLID_TRIGGER )
		{
			AppendIdx( s_disabled, sizeof( s_disabled ), i, STRING( e->v.classname ) );
			e->v.solid = SOLID_NOT;
			SET_ORIGIN( e, e->v.origin );
		}
	}
}

// ---------------------------------------------------------------- fake client

edict_t *Probe_Bot( void )
{
	if ( s_bot ) return s_bot;
	edict_t *e = CREATE_FAKE_CLIENT( "probe" );
	if ( !e )
	{
		Probe_Log( "probe: cannot create fake client\n" );
		return NULL;
	}
	if ( e->pvPrivateData ) FREE_PRIVATE( e );
	e->pvPrivateData = NULL;
	e->v.frags = 0;
	char reject[128];
	ClientConnect( e, "probe", "127.0.0.1", reject );
	ClientPutInServer( e );
	e->v.flags |= FL_FAKECLIENT | FL_GODMODE;
	e->v.takedamage = DAMAGE_NO;
	s_bot = e;
	s_botPark = e->v.origin;
	Probe_BotPark();
	return e;
}

void Probe_BotPark( void )
{
	if ( !s_bot ) return;
	Probe_BotReset( s_botPark, SOLID_NOT );
}

void Probe_BotReset( const Vector &origin, int solid )
{
	entvars_t *v = &s_bot->v;
	v->solid = solid;
	v->movetype = MOVETYPE_WALK;
	v->velocity = v->basevelocity = v->avelocity = g_vecZero;
	v->punchangle = g_vecZero;
	v->flags &= ~( FL_ONGROUND | FL_DUCKING | FL_WATERJUMP | FL_INWATER | FL_FROZEN | FL_ONTRAIN );
	v->flags |= FL_GODMODE;
	v->takedamage = DAMAGE_NO;
	v->health = 100;
	v->bInDuck = 0;
	v->flDuckTime = 0;
	v->flTimeStepSound = 0;
	v->flSwimTime = 0;
	v->iStepLeft = 0;
	v->flFallVelocity = 0;
	v->waterlevel = 0;
	v->watertype = CONTENTS_EMPTY;
	v->teleport_time = 0;
	v->groundentity = NULL;
	v->view_ofs = VEC_VIEW;
	v->gravity = 1;
	v->friction = 1;
	v->maxspeed = 0;
	g_engfuncs.pfnSetClientMaxspeed( s_bot, CVAR_GET_FLOAT( "sv_maxspeed" ) );
	UTIL_SetSize( v, VEC_HULL_MIN, VEC_HULL_MAX );
	SET_ORIGIN( s_bot, origin );
}

void Probe_BotMove( const Vector &angles, float fwd, float side, float up, unsigned short buttons, int msec )
{
	s_bot->v.v_angle = angles;
	g_engfuncs.pfnRunPlayerMove( s_bot, angles, fwd, side, up, buttons, 0, (byte)msec );
}

extern "C" void Probe_PM_Move( struct playermove_s *ppmove, int server )
{
	bool bot = server && s_bot && ppmove->player_index == ENTINDEX( s_bot ) - 1;
	if ( bot && g_probePmPre )
	{
		ProbePmFn fn = g_probePmPre;
		g_probePmPre = NULL;
		fn( ppmove );
	}
	PM_Move( ppmove, server );
	if ( bot && g_probePmPost )
	{
		ProbePmFn fn = g_probePmPost;
		g_probePmPost = NULL;
		fn( ppmove );
	}
}

// ---------------------------------------------------------------- scheduler

static void Cmd_ProbeRun( void )
{
	s_numQueued = 0;
	for ( int a = 1; a < CMD_ARGC(); a++ )
	{
		const char *n = CMD_ARGV( a );
		for ( int c = 0; c < NUM_CAPS; c++ )
		{
			bool m2 = !strcmp( n, "m2" ) || !strcmp( n, "all" );
			if ( ( m2 || !strcmp( n, s_caps[c].name ) ) && s_numQueued < 32 )
				s_queue[s_numQueued++] = c;
		}
	}
	s_cur = -1;
	s_prepared = false;
	s_startAt = gpGlobals->time + 2.0f;
	Probe_Log( "probe: %d captures queued on %s, start at %.2f\n", s_numQueued, Probe_Map(), s_startAt );
}

static void Cmd_ProbeQuit( void )
{
	s_quitWhenDone = CMD_ARGC() < 2 || atoi( CMD_ARGV( 1 ) ) != 0;
}

void Probe_Init( void )
{
	const char *o = getenv( "HL64_PROBE_OUT" );
	if ( o && *o ) strncpy( s_outDir, o, sizeof( s_outDir ) - 1 );
	const char *sc = getenv( "HL64_PROBE_SCALE" );
	if ( sc && *sc ) g_probeScale = (float)atof( sc );
	g_engfuncs.pfnAddServerCommand( (char *)"probe_run", Cmd_ProbeRun );
	g_engfuncs.pfnAddServerCommand( (char *)"probe_quit", Cmd_ProbeQuit );
}

void Probe_Precache( void )
{
	// T-hitbox spawns these; precaching is only possible during map load.
	UTIL_PrecacheOther( "monster_scientist" );
	UTIL_PrecacheOther( "monster_barney" );
	UTIL_PrecacheOther( "monster_human_grunt" );
	UTIL_PrecacheOther( "monster_zombie" );
	s_bot = NULL;
	s_numQueued = 0;
	s_cur = -1;
	g_probePmPre = g_probePmPost = NULL;
	g_probeTouchLog = NULL;
}

void Probe_StartFrame( void )
{
	g_probeFrame++;
	if ( s_numQueued == 0 || gpGlobals->time < s_startAt ) return;
	if ( !s_prepared )
	{
		Probe_Prepare();
		s_prepared = true;
		return;
	}
	if ( s_cur < 0 )
	{
		s_cur = 0;
		s_step = 0;
		Probe_Log( "probe: %s begins\n", s_caps[s_queue[0]].name );
	}
	if ( s_cur >= s_numQueued ) return;
	const ProbeCapture &c = s_caps[s_queue[s_cur]];
	if ( c.fn( s_step++ ) )
	{
		Probe_Log( "probe: %s done on %s after %d steps\n", c.name, Probe_Map(), s_step );
		g_probeTouchLog = NULL;
		g_probePmPre = g_probePmPost = NULL;
		Probe_BotPark();
		s_cur++;
		s_step = 0;
		if ( s_cur < s_numQueued )
			Probe_Log( "probe: %s begins\n", s_caps[s_queue[s_cur]].name );
		else
		{
			Probe_Log( "probe: all captures done on %s\n", Probe_Map() );
			if ( s_quitWhenDone ) SERVER_COMMAND( (char *)"quit\n" );
		}
	}
}
