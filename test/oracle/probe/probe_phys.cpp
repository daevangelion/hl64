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
// Physics captures for docs/specs/physics-movetypes.md (P1-P12): probe
// entities of each movetype on a level floor spot, probe brush entities
// (copies of map brush models) as platforms, pushers, blockers and
// conveyors; the engine's physics runs them and every frame's state is
// logged (scenario runner, probe_scen.cpp). P-walkmove calls pfnWalkMove,
// pfnDropToFloor and pfnMoveToOrigin directly.

#include "probe_int.h"
#include <math.h>
#include <string.h>

#define F g_scenFloor
#define R g_scenRadius

static const Vector CUBE8_MIN( -8, -8, -8 ), CUBE8_MAX( 8, 8, 8 );
static const Vector CRATE_MIN( -12, -12, 0 ), CRATE_MAX( 12, 12, 24 );
static const Vector MON_MIN( -16, -16, 0 ), MON_MAX( 16, 16, 72 );

static CProbeEnt *Monster( int movetype, const Vector &org )
{
	CProbeEnt *m = Scen_Ent( SOLID_SLIDEBOX, movetype, MON_MIN, MON_MAX, org );
	m->pev->flags |= FL_MONSTER;
	return m;
}

static void OnGround( CBaseEntity *e, edict_t *ground )
{
	e->pev->flags |= FL_ONGROUND;
	e->pev->groundentity = ground;
}

static CProbeEnt *PlatformAt( float minXY, float maxXY, float minZ, float x, float y, float topZ, int solid, int movetype, float *top )
{
	Vector mn, mx;
	const char *model = Probe_PickModel( minXY, maxXY, minZ, 512, false, mn, mx );
	if ( !model ) return NULL;
	CProbeEnt *p = Probe_Platform( model, mn, mx, x, y, topZ, solid, movetype, top );
	if ( p )
	{
		Scen_Own( p );
		Scen_Track( p->edict() );
		fprintf( g_scenFile, "{\"sc\":%d,\"platform\":%d,\"model\":\"%s\",\"top\":%.9g}\n", g_scenId, p->entindex(), model, (double)( top ? *top : topZ ) );
	}
	return p;
}

static void Pusher( CBaseEntity *p, const Vector &vel, const Vector &avel, float after )
{
	CProbeEnt *pe = (CProbeEnt *)p;
	p->pev->velocity = vel;
	p->pev->avelocity = avel;
	p->pev->nextthink = p->pev->ltime + after;
	pe->m_thinkMode = TH_STOP;
	fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"pusher_start\",\"i\":%d,\"v\":" VF ",\"av\":" VF ",\"ltime\":%.9g,\"nt\":%.9g,\"t\":%.9g}\n",
		g_scenId, p->entindex(), VA( vel ), VA( avel ), (double)p->pev->ltime, (double)p->pev->nextthink, (double)gpGlobals->time );
}

// ---------------------------------------------------------------- P-toss

static const ScenDef s_toss[] =
{
	{ "drop16", 60, NULL, NULL },
	{ "drop64_spin", 80, NULL, NULL },
	{ "drop200", 100, NULL, NULL },
	{ "arc", 100, NULL, NULL },
	{ "onto_monster", 80, NULL, NULL },
	{ "conveyor", 150, NULL, NULL },
	{ "basevelocity", 60, NULL, NULL },
	{ "fly_down", 40, NULL, NULL },
};

static bool TossSetup( void )
{
	CProbeEnt *e;
	switch ( g_scenId )
	{
	case 0: Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( 0, 0, 8 + 16 ) ); return true;
	case 1:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( 0, 0, 8 + 64 ) );
		e->pev->avelocity = Vector( 0, 90, 0 );
		return true;
	case 2: Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( 0, 0, 8 + 200 ) ); return true;
	case 3:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( -R + 16, 0, 8 ) );
		e->pev->velocity = Vector( 150, 0, 250 );
		return true;
	case 4:
	{
		CProbeEnt *m = Monster( MOVETYPE_NONE, F );
		OnGround( m, INDEXENT( 0 ) );
		Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( 0, 0, 72 + 8 + 32 ) );
		return true;
	}
	case 5:
	{
		float top;
		CProbeEnt *c = PlatformAt( 64, 2 * R - 16, 8, F.x, F.y, F.z + 8, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !c ) return Scen_Skip( "no platform model" );
		c->pev->flags |= FL_CONVEYOR;
		c->pev->speed = 100;
		c->pev->movedir = Vector( 1, 0, 0 );
		float hx = ( c->pev->maxs.x - c->pev->mins.x ) * 0.5f;
		Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, Vector( F.x - hx * 0.5f, F.y, top + 8 + 8 ) );
		return true;
	}
	case 6:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, F + Vector( -R + 16, 0, 8 + 100 ) );
		e->pev->basevelocity = Vector( 100, 0, 0 );
		e->pev->flags |= FL_BASEVELOCITY;
		return true;
	case 7:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_FLY, CUBE8_MIN, CUBE8_MAX, F + Vector( 0, 0, 8 + 32 ) );
		e->pev->velocity = Vector( 0, 0, -200 );
		return true;
	}
	return false;
}

bool Cap_PToss( int step )
{
	static ScenDef d[sizeof( s_toss ) / sizeof( s_toss[0] )];
	const int n = sizeof( s_toss ) / sizeof( s_toss[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_toss[i]; d[i].setup = TossSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-toss", d, n, 160, 256 );
}

// ---------------------------------------------------------------- P-bounce

static const ScenDef s_bounce[] =
{
	{ "bounce_f0", 300, NULL, NULL },
	{ "bounce_f055", 400, NULL, NULL },
	{ "bounce_f08", 400, NULL, NULL },
	{ "bounce_f1", 300, NULL, NULL },
	{ "bounce_vert_f08", 300, NULL, NULL },
	{ "grenade_g05_f08", 400, NULL, NULL },
	{ "bouncemissile_down", 100, NULL, NULL },
	{ "bounce_box_f08", 300, NULL, NULL },
};

static bool BounceSetup( void )
{
	static const float fr[] = { 0, 0.55f, 0.8f, 1.0f };
	CProbeEnt *e;
	switch ( g_scenId )
	{
	case 0: case 1: case 2: case 3:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCE, g_vecZero, g_vecZero, F + Vector( -R + 8, 0, 64 ) );
		e->pev->velocity = Vector( 60, 0, 0 );
		e->pev->friction = fr[g_scenId];
		return true;
	case 4:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCE, g_vecZero, g_vecZero, F + Vector( 0, 0, 100 ) );
		e->pev->friction = 0.8f;
		return true;
	case 5:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCE, g_vecZero, g_vecZero, F + Vector( -R + 8, 0, 32 ) );
		e->pev->velocity = Vector( 60, 0, 150 );
		e->pev->gravity = 0.5f;
		e->pev->friction = 0.8f;
		return true;
	case 6:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCEMISSILE, g_vecZero, g_vecZero, F + Vector( 0, 0, 64 ) );
		e->pev->velocity = Vector( 0, 0, -300 );
		return true;
	case 7:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCE, Vector( -4, -4, -4 ), Vector( 4, 4, 4 ), F + Vector( -R + 8, 0, 64 ) );
		e->pev->velocity = Vector( 60, 0, 0 );
		e->pev->friction = 0.8f;
		return true;
	}
	return false;
}

bool Cap_PBounce( int step )
{
	static ScenDef d[sizeof( s_bounce ) / sizeof( s_bounce[0] )];
	const int n = sizeof( s_bounce ) / sizeof( s_bounce[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_bounce[i]; d[i].setup = BounceSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-bounce", d, n, 160, 256 );
}

// ---------------------------------------------------------------- P-step

static const ScenDef s_step[] =
{
	{ "fall", 100, NULL, NULL },
	{ "pushed", 120, NULL, NULL },
	{ "ledge48", 150, NULL, NULL },
	{ "fly", 60, NULL, NULL },
	{ "water_step", 150, NULL, NULL },
	{ "water_toss", 150, NULL, NULL },
	{ "water_bounce", 150, NULL, NULL },
};

static bool StepWater( Vector &w )
{
	float floorZ;
	if ( !Probe_WaterSpot( g_scenRng, w, floorZ ) ) return false;
	fprintf( g_scenFile, "{\"sc\":%d,\"water\":" VF ",\"water_floor\":%.9g}\n", g_scenId, VA( w ), (double)floorZ );
	return true;
}

static bool StepSetup( void )
{
	CProbeEnt *e;
	Vector w;
	switch ( g_scenId )
	{
	case 0: Monster( MOVETYPE_STEP, F + Vector( 0, 0, 64 ) ); return true;
	case 1:
		e = Monster( MOVETYPE_STEP, F + Vector( -R + 24, 0, 0 ) );
		OnGround( e, INDEXENT( 0 ) );
		e->pev->velocity = Vector( 300, 0, 0 );
		return true;
	case 2:
	{
		float top;
		CProbeEnt *p = PlatformAt( 64, 2 * R - 64, 8, F.x, F.y, F.z + 48, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "no platform model" );
		float edge = p->pev->absmax.x - 1;
		e = Monster( MOVETYPE_STEP, Vector( edge - 20, F.y, top ) );
		OnGround( e, p->edict() );
		e->pev->velocity = Vector( 200, 0, 0 );
		return true;
	}
	case 3:
		e = Monster( MOVETYPE_STEP, F + Vector( 0, 0, 64 ) );
		e->pev->flags |= FL_FLY;
		e->pev->velocity = Vector( 50, 0, 0 );
		return true;
	case 4:
		if ( !StepWater( w ) ) return Scen_Skip( "no water" );
		Monster( MOVETYPE_STEP, w + Vector( 0, 0, 40 ) );
		return true;
	case 5:
		if ( !StepWater( w ) ) return Scen_Skip( "no water" );
		Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CUBE8_MIN, CUBE8_MAX, w + Vector( 0, 0, 40 ) );
		return true;
	case 6:
		if ( !StepWater( w ) ) return Scen_Skip( "no water" );
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_BOUNCE, g_vecZero, g_vecZero, w + Vector( 0, 0, 40 ) );
		e->pev->friction = 0.8f;
		return true;
	}
	return false;
}

bool Cap_PStep( int step )
{
	static ScenDef d[sizeof( s_step ) / sizeof( s_step[0] )];
	const int n = sizeof( s_step ) / sizeof( s_step[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_step[i]; d[i].setup = StepSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-step", d, n, 160, 256 );
}

// ---------------------------------------------------------------- P-push

static EHANDLE s_pusher;

static void PushTick( int f )
{
	CBaseEntity *p = s_pusher;
	if ( !p ) return;
	switch ( g_scenId )
	{
	case 0: if ( f == 40 ) Pusher( p, Vector( 0, 0, 100 ), g_vecZero, 0.5f ); break;
	case 1: if ( f == 10 ) Pusher( p, Vector( 100, 0, 0 ), g_vecZero, 1.0f ); break;
	case 2: if ( f == 30 ) Pusher( p, Vector( 100, 0, 0 ), g_vecZero, 1.0f ); break;
	case 3: case 4: if ( f == 10 ) Pusher( p, Vector( 100, 0, 0 ), g_vecZero, 1.0f ); break;
	case 5: if ( f == 10 ) Pusher( p, Vector( 0, 0, -100 ), g_vecZero, 0.5f ); break;
	}
}

static const ScenDef s_push[] =
{
	{ "lift", 140, NULL, PushTick },
	{ "door_side", 140, NULL, PushTick },
	{ "door_block_rider", 140, NULL, PushTick },
	{ "corpse", 120, NULL, PushTick },
	{ "monster_block", 120, NULL, PushTick },
	{ "down_onto_crate", 80, NULL, PushTick },
};

static bool PushSetup( void )
{
	s_pusher = NULL;
	float top;
	switch ( g_scenId )
	{
	case 0:
	{
		CProbeEnt *p = PlatformAt( 48, 128, 16, F.x, F.y, F.z + 8, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "no pusher model" );
		s_pusher = p;
		Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CRATE_MIN, CRATE_MAX, Vector( F.x, F.y, top + 2 ) );
		return true;
	}
	case 1: case 2: case 3: case 4:
	{
		// bottom 4 u under the floor: the crate in front is inside its swept box
		Vector pmn, pmx;
		const char *model = Probe_PickModel( 48, 128, 16, 256, false, pmn, pmx );
		if ( !model ) return Scen_Skip( "no pusher model" );
		float doorTop = F.z - 4 + ( pmx.z - pmn.z - 2 );
		CProbeEnt *p = Probe_Platform( model, pmn, pmx, F.x - R * 0.5f, F.y, doorTop, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "pusher not placed" );
		Scen_Own( p );
		Scen_Track( p->edict() );
		fprintf( g_scenFile, "{\"sc\":%d,\"pusher\":%d,\"model\":\"%s\",\"top\":%.9g}\n", g_scenId, p->entindex(), model, (double)top );
		s_pusher = p;
		float face = p->pev->absmax.x - 1;
		CProbeEnt *c;
		float cmax;
		if ( g_scenId == 4 )
		{
			c = Monster( MOVETYPE_STEP, Vector( face + 4 + 16, F.y, F.z ) );
			OnGround( c, INDEXENT( 0 ) );
			cmax = c->pev->origin.x + 16;
		}
		else
		{
			c = Scen_Ent( g_scenId == 3 ? SOLID_TRIGGER : SOLID_BBOX, MOVETYPE_TOSS, CRATE_MIN, CRATE_MAX, Vector( face + 4 + 12, F.y, F.z ) );
			OnGround( c, INDEXENT( 0 ) );
			cmax = c->pev->origin.x + 12;
		}
		if ( g_scenId >= 2 )
		{
			Vector mn = p->pev->mins, mx = p->pev->maxs;
			float hx = ( mx.x - mn.x ) * 0.5f - 1;
			CProbeEnt *b = Probe_Platform( STRING( p->pev->model ), mn, mx, cmax + 8 + hx, F.y, doorTop, SOLID_BSP, MOVETYPE_PUSH, NULL );
			if ( !b ) return Scen_Skip( "blocker not placed" );
			Scen_Own( b );
			Scen_Track( b->edict() );
		}
		if ( g_scenId == 2 )
			Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CRATE_MIN, CRATE_MAX, Vector( p->pev->absmin.x + 1 + ( p->pev->absmax.x - p->pev->absmin.x ) * 0.5f - 1, F.y, top + 2 ) );
		return true;
	}
	case 5:
	{
		CProbeEnt *c = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, CRATE_MIN, CRATE_MAX, F );
		OnGround( c, INDEXENT( 0 ) );
		Vector mn, mx;
		const char *model = Probe_PickModel( 48, 128, 16, 128, false, mn, mx );
		if ( !model ) return Scen_Skip( "no pusher model" );
		CProbeEnt *p = Probe_Platform( model, mn, mx, F.x, F.y, F.z + 32 + ( mx.z - mn.z - 2 ), SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "pusher not placed" );
		Scen_Own( p );
		Scen_Track( p->edict() );
		s_pusher = p;
		return true;
	}
	}
	return false;
}

bool Cap_PPush( int step )
{
	static ScenDef d[sizeof( s_push ) / sizeof( s_push[0] )];
	const int n = sizeof( s_push ) / sizeof( s_push[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_push[i]; d[i].setup = PushSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-push", d, n, 192, 256 );
}

// ---------------------------------------------------------------- P-rotate

static Vector s_botAng;

static void RotTick( int f )
{
	CBaseEntity *p = s_pusher;
	if ( p && f == 30 ) Pusher( p, g_vecZero, Vector( 0, 90, 0 ), 2.0f );	// half a turn
	edict_t *bot = Probe_Bot();
	if ( bot && bot->v.solid != SOLID_NOT )
	{
		Probe_BotMove( s_botAng, 0, 0, 0, 0, 10 );
		fprintf( g_scenFile, "{\"sc\":%d,\"f\":%d,\"bot\":{\"o\":" VF ",\"a\":" VF ",\"v_angle\":" VF ",\"fl\":%d,\"ge\":%d}}\n",
			g_scenId, f, VA( bot->v.origin ), VA( bot->v.angles ), VA( bot->v.v_angle ), bot->v.flags, Probe_Idx( bot->v.groundentity ) );
	}
}

static const ScenDef s_rotate[] =
{
	{ "rotate_riders", 260, NULL, RotTick },
};

// The rotating pusher turns about its origin (a door's hinge): a copy of the
// map brush model whose bounds stay nearest its origin, origin on the floor
// spot, top 8 u above the floor; a crate and the fake client ride on top, a
// crate on the floor stands where the far corner sweeps.
static bool RotSetup( void )
{
	s_pusher = NULL;
	edict_t *cand[64];
	float cr[64];
	int nc = 0;
	for ( int i = 1; i < gpGlobals->maxEntities; i++ )
	{
		edict_t *e = INDEXENT( i );
		if ( !e || e->free || !e->v.model || STRING( e->v.model )[0] != '*' || FClassnameIs( e, "probe_ent" ) ) continue;
		Vector mn = e->v.mins, mx = e->v.maxs, sz = mx - mn, c = ( mn + mx ) * 0.5f;
		if ( sz.x < 34 || sz.y < 34 || sz.z < 10 ) continue;
		float r = sqrtf( c.x * c.x + c.y * c.y ) + sqrtf( sz.x * sz.x + sz.y * sz.y ) * 0.5f;
		if ( r > R - 8 ) continue;
		int k = nc < 64 ? nc++ : 63;
		while ( k > 0 && cr[k - 1] > r ) { cand[k] = cand[k - 1]; cr[k] = cr[k - 1]; k--; }
		cand[k] = e;
		cr[k] = r;
	}
	CProbeEnt *p = NULL;
	float top = 0;
	Vector mn, mx, c;
	for ( int i = 0; i < nc && !p; i++ )
	{
		mn = cand[i]->v.mins;
		mx = cand[i]->v.maxs;
		c = ( mn + mx ) * 0.5f;
		p = Probe_Platform( STRING( cand[i]->v.model ), mn, mx, F.x + c.x, F.y + c.y, F.z + 8, SOLID_BSP, MOVETYPE_PUSH, &top );
	}
	if ( !p ) return Scen_Skip( "no brush model whose sweep fits the floor spot" );
	Scen_Own( p );
	Scen_Track( p->edict() );
	s_pusher = p;
	Vector o = p->pev->origin, ctr( o.x + c.x, o.y + c.y, top );
	float rmax = 0;
	for ( int k = 0; k < 4; k++ )
	{
		Vector q( k & 1 ? mx.x - 1 : mn.x + 1, k & 2 ? mx.y - 1 : mn.y + 1, 0 );
		rmax = fmaxf( rmax, sqrtf( q.x * q.x + q.y * q.y ) );
	}
	fprintf( g_scenFile, "{\"sc\":%d,\"pusher\":%d,\"model\":\"%s\",\"hinge\":" VF ",\"centre\":" VF ",\"rmax\":%.9g,\"top\":%.9g}\n",
		g_scenId, p->entindex(), STRING( p->pev->model ), VA( o ), VA( ctr ), (double)rmax, (double)top );
	// riders along the model's long axis, either side of its centre
	Vector along = ( mx.x - mn.x ) >= ( mx.y - mn.y ) ? Vector( ( mx.x - mn.x ) * 0.25f, 0, 0 ) : Vector( 0, ( mx.y - mn.y ) * 0.25f, 0 );
	Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, Vector( -8, -8, 0 ), Vector( 8, 8, 16 ), ctr + along + Vector( 0, 0, 1 ) );
	// on the far corner's circle, outside the model's resting footprint
	Vector sp = Vector( o.x + rmax - 6, o.y, F.z );
	for ( int a = 0; a < 24; a++ )
	{
		float ang = a * ( M_PI / 12 );
		Vector q( cosf( ang ) * ( rmax - 6 ), sinf( ang ) * ( rmax - 6 ), 0 );
		bool clear = q.x - 8 > mx.x - 1 || q.x + 8 < mn.x + 1 || q.y - 8 > mx.y - 1 || q.y + 8 < mn.y + 1;
		if ( clear ) { sp = Vector( o.x + q.x, o.y + q.y, F.z ); break; }
	}
	CProbeEnt *sweep = Scen_Ent( SOLID_BBOX, MOVETYPE_TOSS, Vector( -8, -8, 0 ), Vector( 8, 8, 16 ), sp );
	OnGround( sweep, INDEXENT( 0 ) );
	edict_t *bot = Probe_Bot();
	if ( bot )
	{
		s_botAng = g_vecZero;
		Probe_BotReset( ctr - along + Vector( 0, 0, 37 ), SOLID_SLIDEBOX );
		Scen_Track( bot );
	}
	return true;
}

bool Cap_PRotate( int step )
{
	static ScenDef d[1];
	d[0] = s_rotate[0];
	d[0].setup = RotSetup;
	return Scen_Capture( step, "physics-movetypes", "P-rotate", d, 1, 192, 256 );
}

// ---------------------------------------------------------------- P-pushable

static EHANDLE s_box;
static Vector s_boxPush;

static void PushableTick( int f )
{
	CBaseEntity *b = s_box;
	if ( b && f == 10 )
	{
		b->pev->velocity = s_boxPush;
		fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"push\",\"i\":%d,\"v\":" VF ",\"fl\":%d}\n", g_scenId, b->entindex(), VA( s_boxPush ), b->pev->flags );
	}
}

static const ScenDef s_pushable[] =
{
	{ "pushstep_f0", 150, NULL, PushableTick },
	{ "pushstep_f1", 150, NULL, PushableTick },
	{ "pushstep_ledge", 150, NULL, PushableTick },
	{ "map_pushable_0", 150, NULL, PushableTick },
	{ "map_pushable_1", 150, NULL, PushableTick },
	{ "map_pushable_2", 150, NULL, PushableTick },
};

static bool PushableSetup( void )
{
	s_box = NULL;
	CProbeEnt *e;
	switch ( g_scenId )
	{
	case 0: case 1:
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_PUSHSTEP, Vector( -16, -16, 0 ), Vector( 16, 16, 32 ), F + Vector( -R + 20, 0, 1 ) );
		e->pev->friction = g_scenId == 0 ? 0 : 1;
		s_box = e;
		s_boxPush = Vector( 200, 0, 0 );
		return true;
	case 2:
	{
		float top;
		CProbeEnt *p = PlatformAt( 64, 2 * R - 64, 8, F.x, F.y, F.z + 48, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "no platform model" );
		e = Scen_Ent( SOLID_BBOX, MOVETYPE_PUSHSTEP, Vector( -16, -16, 0 ), Vector( 16, 16, 32 ), Vector( p->pev->absmax.x - 1 - 20, F.y, top + 1 ) );
		s_box = e;
		s_boxPush = Vector( 150, 0, 0 );
		return true;
	}
	default:
	{
		int want = g_scenId - 3, k = 0;
		edict_t *pe = NULL;
		for ( edict_t *c = FIND_ENTITY_BY_CLASSNAME( NULL, "func_pushable" ); !FNullEnt( c ); c = FIND_ENTITY_BY_CLASSNAME( c, "func_pushable" ) )
			if ( k++ == want ) { pe = c; break; }
		if ( !pe ) return Scen_Skip( "no such func_pushable" );
		// push toward the side with the longest clear run for its own box
		static const Vector dirs[4] = { Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ), Vector( 0, -1, 0 ) };
		float best = -1;
		for ( int d = 0; d < 4; d++ )
		{
			TraceResult tr;
			TRACE_MONSTER_HULL( pe, pe->v.origin + Vector( 0, 0, 1 ), pe->v.origin + Vector( 0, 0, 1 ) + dirs[d] * 256, ignore_monsters, pe, &tr );
			if ( tr.flFraction > best ) { best = tr.flFraction; s_boxPush = dirs[d] * 120; }
		}
		CBaseEntity *b = CBaseEntity::Instance( pe );
		s_box = b;
		Scen_Track( pe );
		fprintf( g_scenFile, "{\"sc\":%d,\"map_pushable\":%d,\"friction\":%.9g,\"clear\":%.9g}\n", g_scenId, ENTINDEX( pe ), (double)pe->v.friction, (double)best * 256 );
		return true;
	}
	}
}

bool Cap_PPushable( int step )
{
	static ScenDef d[sizeof( s_pushable ) / sizeof( s_pushable[0] )];
	const int n = sizeof( s_pushable ) / sizeof( s_pushable[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_pushable[i]; d[i].setup = PushableSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-pushable", d, n, 160, 256 );
}

// ---------------------------------------------------------------- P-botpush
//
// A map func_pushable taken off its nearest world ledge. Per pushable k:
// `walk_k`: the fake client runs forward into it (CPushable::Touch);
// `use_k`: the same holding +use (CPushable::Use, continuous use);
// `slide_k`: no bot, the box on the ground with groundentity the world
// gets 400 u/s toward the ledge at frame 10; `slidebsp_k`: the same with
// groundentity a map SOLID_BSP brush entity (which ground the friction
// gate reads past the ledge). Direction: of the four axes, the one whose
// floor ends first (a point 2 u under the box's leading face no longer in
// world solid) on a clear run of its own box; the bot starts behind the
// box, feet on its bottom. Each k's scenarios start from where `walk_k`
// found the box (origin, flags, ground, friction).

enum { BP_WALK, BP_USE, BP_SLIDE, BP_SLIDEBSP, BP_KINDS };
static Vector s_bpDir, s_bpAng;
static unsigned short s_bpButtons;
static EHANDLE s_bpBox;

static void BotPushTick( int f )
{
	if ( g_scenId % BP_KINDS >= BP_SLIDE )
	{
		CBaseEntity *b = s_bpBox;
		if ( b && f == 10 )
		{
			b->pev->velocity = s_bpDir * 400;
			fprintf( g_scenFile, "{\"sc\":%d,\"ev\":\"push\",\"i\":%d,\"v\":" VF ",\"fl\":%d}\n", g_scenId, b->entindex(), VA( b->pev->velocity ), b->pev->flags );
		}
		return;
	}
	edict_t *bot = Probe_Bot();
	if ( !bot || bot->v.solid == SOLID_NOT ) return;
	Probe_BotMove( s_bpAng, f < 500 ? 320 : 0, 0, 0, f < 500 ? s_bpButtons : 0, 10 );
}

#define BP_SCEN( k ) { "walk_" #k, 600, NULL, BotPushTick }, { "use_" #k, 600, NULL, BotPushTick }, \
	{ "slide_" #k, 200, NULL, BotPushTick }, { "slidebsp_" #k, 200, NULL, BotPushTick }
static const ScenDef s_botpush[] = { BP_SCEN( 0 ), BP_SCEN( 1 ), BP_SCEN( 2 ), BP_SCEN( 3 ), BP_SCEN( 4 ) };

static bool BotPushSetup( void )
{
	int want = g_scenId / BP_KINDS, kind = g_scenId % BP_KINDS, k = 0;
	edict_t *pe = NULL;
	s_bpBox = NULL;
	for ( edict_t *c = FIND_ENTITY_BY_CLASSNAME( NULL, "func_pushable" ); !FNullEnt( c ); c = FIND_ENTITY_BY_CLASSNAME( c, "func_pushable" ) )
		if ( k++ == want ) { pe = c; break; }
	if ( !pe ) return Scen_Skip( "no such func_pushable" );
	static Vector s_org;
	static int s_flags;
	static edict_t *s_ground;
	static float s_fric;
	if ( kind != BP_WALK )
	{
		pe->v.velocity = g_vecZero;
		pe->v.flags = s_flags;
		pe->v.groundentity = s_ground;
		pe->v.friction = s_fric;
		UTIL_SetOrigin( &pe->v, s_org );
	}
	else
	{
		s_org = pe->v.origin;
		s_flags = pe->v.flags;
		s_ground = pe->v.groundentity;
		s_fric = pe->v.friction;
	}
	edict_t *bot = Probe_Bot();
	if ( !bot ) return Scen_Skip( "no fake client" );
	static const Vector dirs[4] = { Vector( 1, 0, 0 ), Vector( -1, 0, 0 ), Vector( 0, 1, 0 ), Vector( 0, -1, 0 ) };
	Vector mn = pe->v.origin + pe->v.mins, mx = pe->v.origin + pe->v.maxs, c = ( mn + mx ) * 0.5f;
	float bestS = 1e9f;
	int best = -1;
	for ( int d = 0; d < 4; d++ )
	{
		float half = fabsf( dirs[d].x ) * ( mx.x - mn.x ) * 0.5f + fabsf( dirs[d].y ) * ( mx.y - mn.y ) * 0.5f;
		for ( float s = 0; s <= 384; s += 2 )
		{
			TraceResult tr;
			TRACE_MONSTER_HULL( pe, pe->v.origin + Vector( 0, 0, 1 ), pe->v.origin + Vector( 0, 0, 1 ) + dirs[d] * s, ignore_monsters, pe, &tr );
			if ( tr.flFraction < 1.0f || tr.fStartSolid ) break;
			Vector p = c + dirs[d] * ( half + s );
			if ( POINT_CONTENTS( Vector( p.x, p.y, mn.z - 2 ) ) != CONTENTS_SOLID )
			{
				if ( s < bestS ) { bestS = s; best = d; }
				break;
			}
		}
	}
	if ( best < 0 ) return Scen_Skip( "no ledge" );
	s_bpDir = dirs[best];
	s_bpAng = Vector( 0, best == 0 ? 0 : best == 1 ? 180 : best == 2 ? 90 : 270, 0 );
	s_bpButtons = kind == BP_USE ? IN_USE : 0;
	Scen_Track( pe );
	if ( kind >= BP_SLIDE )
	{
		edict_t *g = INDEXENT( 0 );
		if ( kind == BP_SLIDEBSP )
		{
			g = NULL;
			for ( int i = 1; i < gpGlobals->maxEntities && !g; i++ )
			{
				edict_t *b = INDEXENT( i );
				if ( b && !b->free && b->v.solid == SOLID_BSP && b->v.model && STRING( b->v.model )[0] == '*' )
					g = b;
			}
			if ( !g ) return Scen_Skip( "no SOLID_BSP brush entity" );
		}
		pe->v.flags |= FL_ONGROUND;
		pe->v.groundentity = g;
		s_bpBox = CBaseEntity::Instance( pe );
	}
	else
	{
		float half = fabsf( s_bpDir.x ) * ( mx.x - mn.x ) * 0.5f + fabsf( s_bpDir.y ) * ( mx.y - mn.y ) * 0.5f;
		Vector start = c - s_bpDir * ( half + 16 + 2 );
		start.z = mn.z + 37;
		TraceResult tr;
		TRACE_HULL( start, start, dont_ignore_monsters, human_hull, NULL, &tr );
		if ( tr.fStartSolid ) return Scen_Skip( "bot start blocked" );
		Probe_BotReset( start, SOLID_SLIDEBOX );
		Scen_Track( bot );
	}
	fprintf( g_scenFile, "{\"sc\":%d,\"map_pushable\":%d,\"friction\":%.9g,\"model\":\"%s\",\"dir\":" VF ",\"ledge\":%.9g,\"ground\":%d,\"fake_client\":%s,\"buttons\":%d}\n",
		g_scenId, ENTINDEX( pe ), (double)pe->v.friction, STRING( pe->v.model ), VA( s_bpDir ), (double)bestS,
		Probe_Idx( pe->v.groundentity ), kind < BP_SLIDE ? "true" : "false", s_bpButtons );
	return true;
}

bool Cap_PBotPush( int step )
{
	static ScenDef d[sizeof( s_botpush ) / sizeof( s_botpush[0] )];
	const int n = sizeof( s_botpush ) / sizeof( s_botpush[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_botpush[i]; d[i].setup = BotPushSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-botpush", d, n, 160, 256 );
}

// ---------------------------------------------------------------- P-walkmove

static void WM( CBaseEntity *m, const char *what, float yaw, float dist, int mode )
{
	Vector from = m->pev->origin;
	int fl0 = m->pev->flags;
	int ret = WALK_MOVE( m->edict(), yaw, dist, mode );
	fprintf( g_scenFile, "{\"sc\":%d,\"wm\":\"%s\",\"yaw\":%.9g,\"dist\":%.9g,\"mode\":%d,\"fl_before\":%d,\"from\":" VF ",\"ret\":%d,\"to\":" VF ",\"fl\":%d,\"ge\":%d}\n",
		g_scenId, what, (double)yaw, (double)dist, mode, fl0, VA( from ), ret, VA( m->pev->origin ), m->pev->flags, Probe_Idx( m->pev->groundentity ) );
}

// As a monster spawns: dropped to the floor from 1 u above (rests 1/32 above it).
static void Settle( CBaseEntity *m )
{
	Vector from = m->pev->origin + Vector( 0, 0, 1 );
	UTIL_SetOrigin( m->pev, from );
	int ret = DROP_TO_FLOOR( m->edict() );
	fprintf( g_scenFile, "{\"sc\":%d,\"settle\":" VF ",\"ret\":%d,\"to\":" VF ",\"fl\":%d,\"ge\":%d}\n",
		g_scenId, VA( from ), ret, VA( m->pev->origin ), m->pev->flags, Probe_Idx( m->pev->groundentity ) );
}

static void Reset( CBaseEntity *m, const Vector &o, edict_t *ground, int extraFlags )
{
	m->pev->flags = FL_MONSTER | extraFlags | ( ground ? FL_ONGROUND : 0 );
	m->pev->groundentity = ground;
	m->pev->velocity = g_vecZero;
	UTIL_SetOrigin( m->pev, o );
}

static const ScenDef s_walk[] =
{
	{ "flat", 1, NULL, NULL },
	{ "step18", 1, NULL, NULL },
	{ "step19", 1, NULL, NULL },
	{ "ledge48", 1, NULL, NULL },
	{ "ledge16", 1, NULL, NULL },
	{ "partial48", 1, NULL, NULL },
	{ "drop_to_floor", 1, NULL, NULL },
	{ "move_to_origin", 1, NULL, NULL },
	{ "fly_enemy", 1, NULL, NULL },
	{ "swim", 1, NULL, NULL },
	{ "world_stairs", 1, NULL, NULL },
};

static bool WalkSetup( void )
{
	CProbeEnt *m = Monster( MOVETYPE_STEP, F );
	edict_t *world = INDEXENT( 0 );
	Reset( m, F, world, 0 );
	float top;
	switch ( g_scenId )
	{
	case 0:
	{
		static const float yaws[] = { 0, 45, 90, 180, 270 };
		static const float dists[] = { 4, 16, 32 };
		for ( int y = 0; y < 5; y++ ) for ( int d = 0; d < 3; d++ ) for ( int mode = 0; mode < 3; mode++ )
		{
			Reset( m, F, world, 0 );
			WM( m, "flat", yaws[y], dists[d], mode );
		}
		return true;
	}
	case 1: case 2:
	{
		float h = g_scenId == 1 ? 18 : 19;
		Vector mn, mx;
		const char *model = Probe_PickModel( 64, 2 * R - 64, 24, 512, false, mn, mx );
		if ( !model ) return Scen_Skip( "no step model" );
		float hx = ( mx.x - mn.x ) * 0.5f - 1;
		CProbeEnt *s = Probe_Platform( model, mn, mx, F.x + 24 + hx, F.y, F.z + h, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !s ) return Scen_Skip( "step not placed" );
		Scen_Own( s );
		Scen_Track( s->edict() );
		fprintf( g_scenFile, "{\"sc\":%d,\"step\":%.9g,\"top\":%.9g,\"face_x\":%.9g}\n", g_scenId, (double)h, (double)top, (double)( s->pev->absmin.x + 1 ) );
		float face = s->pev->absmin.x + 1;
		static const float gaps[] = { 30, 7, 1 };
		for ( int g = 0; g < 3; g++ )
		{
			Reset( m, Vector( face - 16 - gaps[g], F.y, F.z ), world, 0 );
			Settle( m );
			for ( int i = 0; i < 8; i++ ) WM( m, "step", 0, 8, 0 );
		}
		static const float dists[] = { 2, 16, 24 };
		for ( int d = 0; d < 3; d++ )
		{
			Reset( m, Vector( face - 16 - 1, F.y, F.z ), world, 0 );
			Settle( m );
			WM( m, "step_dist", 0, dists[d], 0 );
		}
		return true;
	}
	case 3: case 4: case 5:
	{
		float h = g_scenId == 4 ? 16 : 48;
		CProbeEnt *p = PlatformAt( 64, 2 * R - 64, 8, F.x, F.y, F.z + h, SOLID_BSP, MOVETYPE_PUSH, &top );
		if ( !p ) return Scen_Skip( "no platform model" );
		float edge = p->pev->absmax.x - 1;
		if ( g_scenId == 5 )
		{
			Reset( m, Vector( edge, F.y, top ), p->edict(), FL_PARTIALGROUND );
			Settle( m );
			for ( int i = 0; i < 4; i++ ) WM( m, "partial", 0, 8, 0 );
		}
		else
		{
			Reset( m, Vector( edge - 40, F.y, top ), p->edict(), 0 );
			Settle( m );
			for ( int i = 0; i < 8; i++ ) WM( m, "ledge", 0, 8, 0 );
		}
		return true;
	}
	case 6:
	{
		static const float hs[] = { 0, 1, 100, 255, 300, -40 };
		for ( int i = 0; i < 6; i++ )
		{
			Reset( m, F + Vector( 0, 0, hs[i] ), NULL, 0 );
			Vector from = m->pev->origin;
			int c = POINT_CONTENTS( from );
			int ret = DROP_TO_FLOOR( m->edict() );
			fprintf( g_scenFile, "{\"sc\":%d,\"drop\":%.9g,\"from\":" VF ",\"contents\":%d,\"ret\":%d,\"to\":" VF ",\"fl\":%d,\"ge\":%d}\n",
				g_scenId, (double)hs[i], VA( from ), c, ret, VA( m->pev->origin ), m->pev->flags, Probe_Idx( m->pev->groundentity ) );
		}
		return true;
	}
	case 7:
	{
		Vector mn, mx;
		const char *model = Probe_PickModel( 32, 96, 24, 512, false, mn, mx );
		if ( model )
		{
			CProbeEnt *b = Probe_Platform( model, mn, mx, F.x, F.y, F.z + 96, SOLID_BSP, MOVETYPE_PUSH, NULL );
			if ( b ) { Scen_Own( b ); Scen_Track( b->edict() ); }
		}
		Vector goal = F + Vector( R - 24, 0, 0 );
		Reset( m, F + Vector( -R + 24, 0, 0 ), world, 0 );
		m->pev->yaw_speed = 360;
		for ( int i = 0; i < 40; i++ )
		{
			Vector from = m->pev->origin;
			MOVE_TO_ORIGIN( m->edict(), goal, 8, MOVE_NORMAL );
			fprintf( g_scenFile, "{\"sc\":%d,\"mto\":%d,\"goal\":" VF ",\"from\":" VF ",\"to\":" VF ",\"a\":" VF ",\"ideal_yaw\":%.9g,\"fl\":%d}\n",
				g_scenId, i, VA( goal ), VA( from ), VA( m->pev->origin ), VA( m->pev->angles ), (double)m->pev->ideal_yaw, m->pev->flags );
		}
		return true;
	}
	case 8:
	{
		CProbeEnt *enemy = Scen_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), F + Vector( 64, 0, 200 ) );
		Reset( m, F + Vector( 0, 0, 64 ), NULL, FL_FLY );
		m->pev->enemy = enemy->edict();
		for ( int i = 0; i < 6; i++ ) WM( m, "fly_enemy_above", 0, 8, 0 );
		UTIL_SetOrigin( enemy->pev, F + Vector( 64, 0, 8 ) );
		for ( int i = 0; i < 6; i++ ) WM( m, "fly_enemy_below", 0, 8, 0 );
		m->pev->enemy = NULL;
		for ( int i = 0; i < 3; i++ ) WM( m, "fly_no_enemy", 0, 8, 0 );
		return true;
	}
	case 9:
	{
		Vector w;
		float floorZ;
		if ( !Probe_WaterSpot( g_scenRng, w, floorZ ) ) return Scen_Skip( "no water" );
		UTIL_SetSize( m->pev, Vector( -8, -8, 0 ), Vector( 8, 8, 16 ) );
		CProbeEnt *enemy = Scen_Ent( SOLID_NOT, MOVETYPE_NONE, Vector( -1, -1, -1 ), Vector( 1, 1, 1 ), w + Vector( 32, 0, 100 ) );
		Reset( m, w - Vector( 0, 0, 24 ), NULL, FL_SWIM );
		m->pev->enemy = enemy->edict();
		fprintf( g_scenFile, "{\"sc\":%d,\"water\":" VF ",\"water_floor\":%.9g}\n", g_scenId, VA( w ), (double)floorZ );
		for ( int i = 0; i < 8; i++ ) WM( m, "swim_enemy_above", 0, 8, 0 );
		return true;
	}
	case 10:
	{
		Vector st;
		float yaw;
		char where[160];
		if ( !Probe_FindStairs( "P-walkmove", st, yaw, where ) ) return Scen_Skip( "no stairs" );
		fprintf( g_scenFile, "{\"sc\":%d,\"stairs\":" VF ",\"yaw\":%.9g,\"where\":%s}\n", g_scenId, VA( st ), (double)yaw, where );
		Reset( m, st - Vector( 0, 0, 36 ), world, 0 );	// player origin to a feet origin
		Settle( m );
		for ( int i = 0; i < 40; i++ ) WM( m, "stairs", yaw, 8, 0 );
		return true;
	}
	}
	return false;
}

bool Cap_PWalkmove( int step )
{
	static ScenDef d[sizeof( s_walk ) / sizeof( s_walk[0] )];
	const int n = sizeof( s_walk ) / sizeof( s_walk[0] );
	for ( int i = 0; i < n; i++ ) { d[i] = s_walk[i]; d[i].setup = WalkSetup; }
	return Scen_Capture( step, "physics-movetypes", "P-walkmove", d, n, 160, 256 );
}
