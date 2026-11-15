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

// Probe entity: logs touches, thinks, blocks and uses (to g_probeTouchLog)
// and optionally reacts.
enum { PT_LOG = 0, PT_SELF_NOT = 1, PT_REMOVE_SELF = 2, PT_REMOVE_OTHER = 3 };
enum { TH_LOG = 0, TH_RESCHED = 1, TH_SET_OTHER = 2, TH_STOP = 3 };
class CProbeEnt : public CBaseEntity
{
public:
	void Spawn( void );
	void Touch( CBaseEntity *pOther );
	void Think( void );
	void Blocked( CBaseEntity *pOther );
	void Use( CBaseEntity *pActivator, CBaseEntity *pCaller, USE_TYPE useType, float value );
	int ObjectCaps( void ) { return FCAP_DONT_SAVE; }
	int m_touchMode;
	int m_tag;
	int m_thinkMode;		// TH_*
	float m_period;			// TH_RESCHED: nextthink = time + m_period (pushers: ltime + m_period)
	int m_thinkCount;		// TH_RESCHED stops after this many thinks (0 = never)
	EHANDLE m_other;		// TH_SET_OTHER: other.nextthink = time
};
CProbeEnt *Probe_Ent( int solid, int movetype, const Vector &mins, const Vector &maxs, const Vector &org );
CProbeEnt *Probe_BrushEnt( int solid, const char *model, const Vector &org, const Vector &ang );
void Probe_Free( CBaseEntity *e );

extern FILE *g_probeTouchLog;		// touches are logged here when non-NULL
extern const char *g_probePhase;	// phase label written with each touch
extern int g_probeCase;				// case id written with each touch
extern bool g_probeLive;			// live capture: map triggers stay enabled, map touches are logged, not dispatched
extern bool g_probeFreezeMovers;	// touches of map MOVETYPE_PUSH entities are swallowed (Probe_FreezeMovers)
// Stops every map pusher (velocity, avelocity, nextthink cleared) and
// swallows their touches until the capture ends; writes {"frozen_movers":
// [[index, class, was_moving]…]}.
void Probe_FreezeMovers( FILE *f );
// Called for every touch the probe sees (map touches in live captures and
// touches of probe entities), after logging.
typedef void ( *ProbeTouchFn )( edict_t *self, edict_t *other );
extern ProbeTouchFn g_probeTouchFn;

// World
extern Vector g_probeWorldMins, g_probeWorldMaxs;
extern int g_probeFrame;
extern float g_probeScale;			// sample-count multiplier (HL64_PROBE_SCALE)
const char *Probe_Map( void );
bool Probe_OpenSpot( ProbeRng &rng, float clear, Vector &out );
bool Probe_FloorBelow( const Vector &p, int hull, float maxDrop, Vector &origin, Vector &normal );
const char *Probe_PickBoxModel( float minSize, float maxSize, Vector *mins, Vector *maxs );
int Probe_Count( int n );			// n scaled by g_probeScale, at least 1
// A level floor spot: `out` is on the floor plane (z = plane height); line
// hits within 0.01 u of that height at `radius` around it (8 directions),
// and the large hull is clear from the floor up to `height`.
bool Probe_FloorSpot( ProbeRng &rng, float radius, float height, Vector &out );
// Dynamic state of an entity: {"i",o,a,v,av,bv,fl,ge,lt,nt,wl,wt,solid,mt,mins,maxs,absmin,absmax}
void Probe_PrDyn( FILE *f, edict_t *e );
// The map's entity lump: block index (worldspawn 0) of an entity, matched
// by brush model, else by classname and nearest origin in the plane.
int Probe_LumpIndex( edict_t *e, char *targetname, int tnsize );
// K-settle: state recorded from map load until the captures start.
void Probe_WriteEarly( FILE *f );

// Fake client
edict_t *Probe_Bot( void );
void Probe_BotPark( void );
void Probe_BotReset( const Vector &origin, int solid );
void Probe_BotMove( const Vector &angles, float fwd, float side, float up, unsigned short buttons, int msec );

// Called once inside the next PM_Move of the probe bot (before PM_Move) / after it.
typedef void ( *ProbePmFn )( playermove_t *pm );
extern ProbePmFn g_probePmPre;
extern ProbePmFn g_probePmPost;

// Scenario runner (M3 captures): scenarios run one after another; each
// setup creates entities (Scen_Ent / Scen_Own: freed when the scenario ends,
// Scen_Track: state printed every frame), then `frames` server frames run.
// Every frame: a state line {"sc","f","t","ft","e":[Probe_PrDyn...]} (state
// after f physics frames), then tick(f) may change things before the next
// physics frame. Touches, thinks, blocks and uses go to the same file with
// "case" = scenario id. host_framerate is restored and the bot parked after
// every scenario.
struct ScenDef { const char *name; int frames; bool ( *setup )( void ); void ( *tick )( int f ); };
struct ScenRun { const ScenDef *defs; int n; int cur; int frame; FILE *f; float hostFramerate; };
void Scen_Begin( ScenRun &r, const ScenDef *defs, int n, FILE *f );
bool Scen_Step( ScenRun &r );		// true when every scenario has run
void Scen_Track( edict_t *e );
CBaseEntity *Scen_Own( CBaseEntity *e );
CProbeEnt *Scen_Ent( int solid, int movetype, const Vector &mins, const Vector &maxs, const Vector &org );
bool Scen_Skip( const char *reason );	// prints the skip line, returns false
extern FILE *g_scenFile;			// the running capture's file (setup / tick output)
extern int g_scenId;
// An unrotated map brush model that fills its bounds (27-point test), with
// x and y size in [minXY, maxXY] and z size in [minZ, maxZ]; NULL if none.
// `centred`: model bounds centre within 4 u of the model origin in x and y.
const char *Probe_PickModel( float minXY, float maxXY, float minZ, float maxZ, bool centred, Vector &mins, Vector &maxs );
// A probe brush entity of that model whose bounds centre is at (x, y) and
// whose top face is at topZ (model bounds are padded by 1 u); the measured
// top (line trace) is returned in *top. NULL if the measured top is off.
CProbeEnt *Probe_Platform( const char *model, const Vector &mins, const Vector &maxs, float x, float y, float topZ,
	int solid, int movetype, float *top );
// A water spot: `out` 1 u below a water surface with a world floor at most
// 256 u under it; false if the map has none.
bool Probe_WaterSpot( ProbeRng &rng, Vector &out, float &floorZ );
// World stairs (the PM-bot finder, seeded by `capture`): `start` is a
// standing player origin at the foot, `yaw` the climb direction.
bool Probe_FindStairs( const char *capture, Vector &start, float &yaw, char *where );
// A whole scenario capture: step 0 opens the file, finds a level floor spot
// (g_scenFloor, radius tried from `radius` down to 64, the large hull clear
// `height` above; {"floor"} line) and begins; later steps run scenarios.
bool Scen_Capture( int step, const char *spec, const char *capture, const ScenDef *defs, int n, float radius, float height );
extern Vector g_scenFloor;
extern float g_scenRadius;
extern ProbeRng g_scenRng;

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
bool Cap_AOrder( int step );
bool Cap_PToss( int step );
bool Cap_PBounce( int step );
bool Cap_PStep( int step );
bool Cap_PPush( int step );
bool Cap_PRotate( int step );
bool Cap_PPushable( int step );
bool Cap_PWalkmove( int step );
bool Cap_KThink( int step );
bool Cap_KPusher( int step );
bool Cap_KAlloc( int step );
bool Cap_KSettle( int step );
bool Cap_WWater( int step );
bool Cap_LIllum( int step );
bool Cap_PBotPush( int step );

#endif
