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
// Internal interface shared by the probe core (probe.cpp) and the capture
// modules (probe_trace.cpp, probe_area.cpp, probe_pmove.cpp). A capture is
// a step function called once per server frame from StartFrame until it
// reports done; it writes one JSONL file per map under
// $HL64_PROBE_OUT/goldsrc/<spec>/<capture>/<map>.jsonl (format:
// test/oracle/README.md).

#ifndef HL64_PROBE_INT_H
#define HL64_PROBE_INT_H

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "monsters.h"
#include "client.h"
#include "usercmd.h"
#include "pm_defs.h"
#include <stdio.h>

// HLSDK has no FL_ITEM; Quake's FL_ITEM bit (256) is FL_SKIPLOCALHOST in
// HLSDK const.h. A-touch probes whether the engine still widens it.
#define PROBE_FL_ITEM ( 1 << 8 )
#define TRACE_MODEL ( *g_engfuncs.pfnTraceModel )
#define CREATE_FAKE_CLIENT ( *g_engfuncs.pfnCreateFakeClient )

#define VF "[%.9g,%.9g,%.9g]"
#define VA(v) (double)(v)[0], (double)(v)[1], (double)(v)[2]

// Deterministic generator: inputs are reproducible from (map, capture).
struct ProbeRng
{
	unsigned s;
	void Seed( const char *a, const char *b );
	unsigned Next( void );
	float Uni( float lo, float hi );	// [lo, hi)
	int Range( int lo, int hi );		// [lo, hi]
	Vector Dir( void );					// uniform unit vector
};

// Output
void Probe_Log( const char *fmt, ... );					// server console
FILE *Probe_Open( const char *spec, const char *capture, const ProbeRng *rng );
void Probe_Close( FILE *f );
void Probe_PrTrace( FILE *f, const TraceResult &tr );	// object body, no braces
void Probe_PrPmTrace( FILE *f, const pmtrace_t &tr );
void Probe_PrEnt( FILE *f, edict_t *e );				// {"i":..,...}
void Probe_PrBrushEnts( FILE *f );						// {"brush_ents":[...]} line
int Probe_Idx( edict_t *e );							// -1 for NULL

// Probe entity: logs and optionally reacts to touches.
enum { PT_LOG = 0, PT_SELF_NOT = 1, PT_REMOVE_SELF = 2, PT_REMOVE_OTHER = 3 };
class CProbeEnt : public CBaseEntity
{
public:
	void Spawn( void );
	void Touch( CBaseEntity *pOther );
	int ObjectCaps( void ) { return FCAP_DONT_SAVE; }
	int m_touchMode;
	int m_tag;
};
CProbeEnt *Probe_Ent( int solid, int movetype, const Vector &mins, const Vector &maxs, const Vector &org );
CProbeEnt *Probe_BrushEnt( int solid, const char *model, const Vector &org, const Vector &ang );
void Probe_Free( CBaseEntity *e );

extern FILE *g_probeTouchLog;		// touches are logged here when non-NULL
extern const char *g_probePhase;	// phase label written with each touch
extern int g_probeCase;				// case id written with each touch

// World
extern Vector g_probeWorldMins, g_probeWorldMaxs;
extern int g_probeFrame;
extern float g_probeScale;			// sample-count multiplier (HL64_PROBE_SCALE)
const char *Probe_Map( void );
bool Probe_OpenSpot( ProbeRng &rng, float clear, Vector &out );
bool Probe_FloorBelow( const Vector &p, int hull, float maxDrop, Vector &origin, Vector &normal );
const char *Probe_PickBoxModel( float minSize, float maxSize, Vector *mins, Vector *maxs );
int Probe_Count( int n );			// n scaled by g_probeScale, at least 1

// Fake client
edict_t *Probe_Bot( void );
void Probe_BotPark( void );
void Probe_BotReset( const Vector &origin, int solid );
void Probe_BotMove( const Vector &angles, float fwd, float side, float up, unsigned short buttons, int msec );

// Called once inside the next PM_Move of the probe bot (before PM_Move) / after it.
typedef void ( *ProbePmFn )( playermove_t *pm );
extern ProbePmFn g_probePmPre;
extern ProbePmFn g_probePmPost;

// Captures
typedef bool ( *ProbeStepFn )( int step );
bool Cap_TPoints( int step );
bool Cap_TSegments( int step );
bool Cap_TAllsolid( int step );
bool Cap_TEntities( int step );
bool Cap_TRotated( int step );
bool Cap_THitbox( int step );
bool Cap_ATouch( int step );
bool Cap_AShape( int step );
bool Cap_PMBot( int step );

#endif
