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
// PM-fall: the fake client dropped from rest onto level world floor
// (Probe_FloorSpot) from a list of heights, at command msec 33 and 10,
// through pfnRunPlayerMove (the engine's SV_RunCmd: PlayerPreThink, PM_Move,
// PlayerPostThink). The client takes damage here (godmode off, DAMAGE_AIM,
// health 100 before every drop), so the single-player rules' fall damage
// (CHalfLifeRules::FlPlayerFallDamage) shows in pev->health. Every frame
// logs pev->flFallVelocity (pmove's), CBasePlayer::m_flFallVelocity (the
// game's, set in PreThink) and the health; each drop ends 10 frames after
// it landed.

#include "probe_int.h"
#include <math.h>
#include <string.h>

static ProbeRng s_rng;
static FILE *s_f;

static const float s_heights[] = { 64, 128, 200, 256, 300, 350, 400, 444, 500, 560, 620 };
static const int s_msecs[] = { 33, 10 };
#define NH ( (int)( sizeof( s_heights ) / sizeof( s_heights[0] ) ) )
#define NM ( (int)( sizeof( s_msecs ) / sizeof( s_msecs[0] ) ) )
static const int MAX_FRAMES = 400;
static const int AFTER_LAND = 10;
static Vector s_floor;

static CBasePlayer *BotPlayer( edict_t *b )
{
	return (CBasePlayer *)CBaseEntity::Instance( b );
}

static void PrState( edict_t *b )
{
	entvars_t *v = &b->v;
	CBasePlayer *p = BotPlayer( b );
	fprintf( s_f, "\"t\":%.9g,\"o\":" VF ",\"v\":" VF ",\"fl\":%d,\"fv\":%.9g,\"pfv\":%.9g,\"hp\":%.9g,\"pa\":" VF
		",\"dmg\":%.9g,\"dt\":%d",
		(double)gpGlobals->time, VA( v->origin ), VA( v->velocity ), v->flags, (double)v->flFallVelocity,
		p ? (double)p->m_flFallVelocity : -1.0, (double)v->health, VA( v->punchangle ),
		(double)v->dmg_take, p ? p->m_bitsDamageType : 0 );
}

bool Cap_PMFall( int step )
{
	static int drop, frame, landed;
	if ( step == 0 )
	{
		s_rng.Seed( Probe_Map(), "PM-fall" );
		s_f = Probe_Open( "pmove-host", "PM-fall", &s_rng );
		if ( !s_f ) return true;
		if ( !Probe_Bot() )
		{
			fprintf( s_f, "{\"skipped\":\"no fake client\"}\n" );
			Probe_Close( s_f );
			return true;
		}
		// the hull-2 clearance covers the highest drop's start box
		if ( !Probe_FloorSpot( s_rng, 48, s_heights[NH - 1] + 120, s_floor ) )
		{
			fprintf( s_f, "{\"skipped\":\"no level floor with %.6g u clear above\"}\n", (double)( s_heights[NH - 1] + 120 ) );
			Probe_Close( s_f );
			return true;
		}
		fprintf( s_f, "{\"floor\":" VF ",\"drops\":%d,\"max_frames\":%d,\"after_land\":%d}\n",
			VA( s_floor ), NH * NM, MAX_FRAMES, AFTER_LAND );
		drop = 0;
		frame = -1;
		return false;
	}
	if ( drop >= NH * NM )
	{
		Probe_BotPark();
		Probe_Close( s_f );
		return true;
	}
	edict_t *bot = Probe_Bot();
	float h = s_heights[drop % NH];
	int msec = s_msecs[drop / NH];
	if ( frame < 0 )
	{
		// standing origin on the floor plane is floor + 36 (hull 1 mins)
		Vector start = s_floor + Vector( 0, 0, 36 + h );
		Probe_BotReset( start, SOLID_SLIDEBOX );
		bot->v.flags &= ~FL_GODMODE;
		bot->v.takedamage = DAMAGE_AIM;
		bot->v.health = 100;
		bot->v.dmg_take = 0;
		bot->v.deadflag = DEAD_NO;
		CBasePlayer *p = BotPlayer( bot );
		if ( p )
		{
			p->m_flFallVelocity = 0;
			p->m_bitsDamageType = 0;
			p->m_lastDamageAmount = 0;
		}
		fprintf( s_f, "{\"drop\":%d,\"h\":%.9g,\"msec\":%d,\"start\":" VF ",\"reset\":{", drop, (double)h, msec, VA( start ) );
		PrState( bot );
		fprintf( s_f, "}}\n" );
		frame = 0;
		landed = -1;
	}
	Probe_BotMove( Vector( 0, 0, 0 ), 0, 0, 0, 0, msec );
	fprintf( s_f, "{\"drop\":%d,\"f\":%d,", drop, frame );
	PrState( bot );
	fprintf( s_f, "}\n" );
	if ( landed < 0 && ( bot->v.flags & FL_ONGROUND ) )
		landed = frame;
	if ( ++frame >= MAX_FRAMES || ( landed >= 0 && frame - landed > AFTER_LAND ) )
	{
		fprintf( s_f, "{\"drop\":%d,\"end\":true,\"landed\":%d,\"hp\":%.9g}\n", drop, landed, (double)bot->v.health );
		frame = -1;
		drop++;
	}
	return false;
}
