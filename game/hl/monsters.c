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
/*
 * hl64 game/, Half Life 1 SDK licence (game/LICENSE-HLSDK).
 *
 * monsters.c -- CBaseMonster (HLSDK dlls/monsters.cpp, monsterstate.cpp and
 * the CBaseAnimating calls it makes) on the hl64 API: components for the
 * entvars, ent_h for EHANDLEs, phy_walk_move / phy_move_to_origin /
 * phy_drop_to_floor for the engine's WALK_MOVE / UTIL_MoveToOrigin /
 * DROP_TO_FLOOR, the node graph of the monster's HLE slot (nodes.c),
 * game/studio/anim.h for the model. Written from the HLSDK, not from the
 * old tree's QuakeC-pattern thinkers.
 */
#include <math.h>
#include <string.h>

#include "engine/core/sys.h"
#include "game/hl/monster.h"

#define MONSTER_CUT_CORNER_DIST	8
#define LOCAL_STEP_SIZE		16
#define DIST_TO_CHECK		200

/* ---- vtable and dispatch ------------------------------------------------ */

static const mon_vt_t	mon_vt_none;

const mon_vt_t *mon_vt(int e)
{
	const game_class_t *c;

	if (e < 0 || !ent_is_live(e))
		return &mon_vt_none;
	c = game_class(ent_idc(e)->cls);
	return (c && c->ext) ? (const mon_vt_t *)c->ext : &mon_vt_none;
}

int mon_is_monster(int e)
{
	const game_class_t *c;

	if (e < 0 || !ent_is_live(e))
		return 0;
	if (e == ENT_PLAYER)
		return 1;	/* CBasePlayer : CBaseMonster */
	c = game_class(ent_idc(e)->cls);
	return c && c->ext;
}

#define VT(e)	mon_vt(e)
#define DISPATCH(f, base, ...)	return VT(e)->f ? VT(e)->f(__VA_ARGS__) : base(__VA_ARGS__)
#define DISPATCHV(f, base, ...)	do { if (VT(e)->f) VT(e)->f(__VA_ARGS__); else base(__VA_ARGS__); } while (0)

static void	nop_e(int e) { (void)e; }
static int	zero_e(int e) { (void)e; return 0; }

void Mon_Look(int e, int d) { DISPATCHV(Look, CBaseMonster_Look, e, d); }
void Mon_RunAI(int e) { DISPATCHV(RunAI, CBaseMonster_RunAI, e); }
void Mon_MonsterInit(int e) { DISPATCHV(MonsterInit, CBaseMonster_MonsterInit, e); }
void Mon_MonsterInitDead(int e) { DISPATCHV(MonsterInitDead, CBaseMonster_MonsterInitDead, e); }
void Mon_BecomeDead(int e) { DISPATCHV(BecomeDead, CBaseMonster_BecomeDead, e); }
void Mon_StartMonster(int e) { DISPATCHV(StartMonster, CBaseMonster_StartMonster, e); }
int Mon_BestVisibleEnemy(int e) { DISPATCH(BestVisibleEnemy, CBaseMonster_BestVisibleEnemy, e); }
static int base_FInViewCone(int e, int other);
int Mon_FInViewCone(int e, int other) { DISPATCH(FInViewCone, base_FInViewCone, e, other); }
void Mon_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{ DISPATCHV(HandleAnimEvent, CBaseMonster_HandleAnimEvent, e, ev); }
int Mon_CheckLocalMove(int e, const float s[3], const float t[3], int target, float *d)
{ DISPATCH(CheckLocalMove, CBaseMonster_CheckLocalMove, e, s, t, target, d); }
void Mon_Move(int e, float i) { DISPATCHV(Move, CBaseMonster_Move, e, i); }
void Mon_MoveExecute(int e, int target, const float dir[3], float i)
{ DISPATCHV(MoveExecute, CBaseMonster_MoveExecute, e, target, dir, i); }
static int base_ShouldAdvanceRoute(int e, float d) { (void)e; return d <= MONSTER_CUT_CORNER_DIST; }
int Mon_ShouldAdvanceRoute(int e, float d) { DISPATCH(ShouldAdvanceRoute, base_ShouldAdvanceRoute, e, d); }
static int base_GetStoppedActivity(int e) { (void)e; return ACT_IDLE; }
int Mon_GetStoppedActivity(int e) { DISPATCH(GetStoppedActivity, base_GetStoppedActivity, e); }
static void base_Stop(int e) { MON(e)->m_IdealActivity = Mon_GetStoppedActivity(e); }
void Mon_Stop(int e) { DISPATCHV(Stop, base_Stop, e); }
int Mon_CheckRangeAttack1(int e, float a, float b) { DISPATCH(CheckRangeAttack1, CBaseMonster_CheckRangeAttack1, e, a, b); }
int Mon_CheckRangeAttack2(int e, float a, float b) { DISPATCH(CheckRangeAttack2, CBaseMonster_CheckRangeAttack2, e, a, b); }
int Mon_CheckMeleeAttack1(int e, float a, float b) { DISPATCH(CheckMeleeAttack1, CBaseMonster_CheckMeleeAttack1, e, a, b); }
int Mon_CheckMeleeAttack2(int e, float a, float b) { DISPATCH(CheckMeleeAttack2, CBaseMonster_CheckMeleeAttack2, e, a, b); }
void Mon_StartTask(int e, const Task_t *t) { DISPATCHV(StartTask, CBaseMonster_StartTask, e, t); }
void Mon_RunTask(int e, const Task_t *t) { DISPATCHV(RunTask, CBaseMonster_RunTask, e, t); }
const Schedule_t *Mon_GetScheduleOfType(int e, int type)
{ DISPATCH(GetScheduleOfType, CBaseMonster_GetScheduleOfType, e, type); }
const Schedule_t *Mon_GetSchedule(int e) { DISPATCH(GetSchedule, CBaseMonster_GetSchedule, e); }
void Mon_ScheduleChange(int e) { DISPATCHV(ScheduleChange, nop_e, e); }
int Mon_CanPlaySequence(int e, int f, int l) { DISPATCH(CanPlaySequence, CBaseMonster_CanPlaySequence, e, f, l); }
static int base_CanPlaySentence(int e, int f) { (void)f; return Mon_IsAllowedToSpeak(e); }
int Mon_CanPlaySentence(int e, int f) { DISPATCH(CanPlaySentence, base_CanPlaySentence, e, f); }
/* CBaseToggle::PlaySentence */
static void base_PlaySentence(int e, res_t s, float dur, float vol, float attn)
{
	(void)dur;
	if (RES_KIND(s) == RES_K_SENTENCE)
		snd_sentence(e, CHAN_VOICE, sentence_id(s), vol, attn, 0, PITCH_NORM);
	else if (RES_KIND(s) == RES_K_GROUP)
		SENTENCEG_PlayRndSz(e, s, vol, attn, 0, PITCH_NORM);
}
void Mon_PlaySentence(int e, res_t s, float d, float v, float a)
{ DISPATCHV(PlaySentence, base_PlaySentence, e, s, d, v, a); }
static void base_PlayScriptedSentence(int e, res_t s, float d, float v, float a, int c, int l)
{
	(void)c; (void)l;
	Mon_PlaySentence(e, s, d, v, a);
}
void Mon_PlayScriptedSentence(int e, res_t s, float d, float v, float a, int c, int l)
{ DISPATCHV(PlayScriptedSentence, base_PlayScriptedSentence, e, s, d, v, a, c, l); }
int Mon_IsAllowedToSpeak(int e) { DISPATCH(IsAllowedToSpeak, IsAlive, e); }
int Mon_GetIdealState(int e) { DISPATCH(GetIdealState, CBaseMonster_GetIdealState, e); }
void Mon_SetActivity(int e, int a) { DISPATCHV(SetActivity, CBaseMonster_SetActivity, e, a); }
int Mon_CheckEnemy(int e, int enemy) { DISPATCH(CheckEnemy, CBaseMonster_CheckEnemy, e, enemy); }
int Mon_FTriangulate(int e, const float s[3], const float t[3], float d, int target, float apex[3])
{ DISPATCH(FTriangulate, CBaseMonster_FTriangulate, e, s, t, d, target, apex); }
void Mon_SetYawSpeed(int e) { DISPATCHV(SetYawSpeed, nop_e, e); }
int Mon_BuildNearestRoute(int e, const float t[3], const float v[3], float a, float b)
{ DISPATCH(BuildNearestRoute, CBaseMonster_BuildNearestRoute, e, t, v, a, b); }
int Mon_FindCover(int e, const float t[3], const float v[3], float a, float b)
{ DISPATCH(FindCover, CBaseMonster_FindCover, e, t, v, a, b); }
static int base_FValidateCover(int e, const float c[3]) { (void)e; (void)c; return 1; }
int Mon_FValidateCover(int e, const float c[3]) { DISPATCH(FValidateCover, base_FValidateCover, e, c); }
static float base_CoverRadius(int e) { (void)e; return 784; }
float Mon_CoverRadius(int e) { DISPATCH(CoverRadius, base_CoverRadius, e); }
static int base_FCanCheckAttacks(int e)
{
	CBaseMonster *m = MON(e);
	return Mon_HasConditions(m, bits_COND_SEE_ENEMY) && !Mon_HasConditions(m, bits_COND_ENEMY_TOOFAR);
}
int Mon_FCanCheckAttacks(int e) { DISPATCH(FCanCheckAttacks, base_FCanCheckAttacks, e); }
void Mon_CheckAmmo(int e) { DISPATCHV(CheckAmmo, nop_e, e); }
int Mon_IgnoreConditions(int e) { DISPATCH(IgnoreConditions, CBaseMonster_IgnoreConditions, e); }
static int base_FValidateHintType(int e, int h) { (void)e; (void)h; return 0; }
int Mon_FValidateHintType(int e, int h) { DISPATCH(FValidateHintType, base_FValidateHintType, e, h); }
int Mon_FCanActiveIdle(int e) { DISPATCH(FCanActiveIdle, zero_e, e); }
static int base_ISoundMask(int e)
{
	(void)e;
	return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_PLAYER;
}
int Mon_ISoundMask(int e) { DISPATCH(ISoundMask, base_ISoundMask, e); }
static CSound *base_PBestSound(int e);
static CSound *base_PBestScent(int e);
CSound *Mon_PBestSound(int e) { DISPATCH(PBestSound, base_PBestSound, e); }
CSound *Mon_PBestScent(int e) { DISPATCH(PBestScent, base_PBestScent, e); }
static float base_HearingSensitivity(int e) { (void)e; return 1.0f; }
float Mon_HearingSensitivity(int e) { DISPATCH(HearingSensitivity, base_HearingSensitivity, e); }
void Mon_BarnacleVictimBitten(int e, int b) { DISPATCHV(BarnacleVictimBitten, CBaseMonster_BarnacleVictimBitten, e, b); }
void Mon_BarnacleVictimReleased(int e) { DISPATCHV(BarnacleVictimReleased, CBaseMonster_BarnacleVictimReleased, e); }
void Mon_PrescheduleThink(int e) { DISPATCHV(PrescheduleThink, nop_e, e); }
int Mon_GetDeathActivity(int e) { DISPATCH(GetDeathActivity, CBaseMonster_GetDeathActivity, e); }
void Mon_GibMonster(int e) { DISPATCHV(GibMonster, CBaseMonster_GibMonster, e); }
static int base_HasHumanGibs(int e)
{
	int c = Classify(e);
	return c == CLASS_HUMAN_MILITARY || c == CLASS_PLAYER_ALLY || c == CLASS_HUMAN_PASSIVE ||
		c == CLASS_PLAYER;
}
static int base_HasAlienGibs(int e)
{
	int c = Classify(e);
	return c == CLASS_ALIEN_MILITARY || c == CLASS_ALIEN_MONSTER || c == CLASS_ALIEN_PASSIVE ||
		c == CLASS_INSECT || c == CLASS_ALIEN_PREDATOR || c == CLASS_ALIEN_PREY;
}
int Mon_HasHumanGibs(int e) { DISPATCH(HasHumanGibs, base_HasHumanGibs, e); }
int Mon_HasAlienGibs(int e) { DISPATCH(HasAlienGibs, base_HasAlienGibs, e); }
void Mon_FadeMonster(int e) { DISPATCHV(FadeMonster, CBaseMonster_FadeMonster, e); }
static int base_ShouldFadeOnDeath(int e)
{
	/* pev->owner: an owner entity, or the monstermaker of a child */
	return (CBase(e)->spawnflags & SF_MONSTER_FADECORPSE) || ent_cv(e)->owner != COL_NONE ||
		MON(e)->m_makerGid != GID_NONE;
}
int Mon_ShouldFadeOnDeath(int e) { DISPATCH(ShouldFadeOnDeath, base_ShouldFadeOnDeath, e); }
static void base_BodyTarget(int e, const float src[3], float out[3])
{
	float c[3], eye[3];

	(void)src;
	Center(e, c);
	EyePosition(e, eye);
	out[0] = c[0] * 0.75f + eye[0] * 0.25f;
	out[1] = c[1] * 0.75f + eye[1] * 0.25f;
	out[2] = c[2] * 0.75f + eye[2] * 0.25f;
}
void Mon_BodyTarget(int e, const float s[3], float o[3]) { DISPATCHV(BodyTarget, base_BodyTarget, e, s, o); }
static void base_GetGunPosition(int e, float out[3])
{
	CBaseMonster *m = MON(e);
	const float *o = ent_cv(e)->origin;

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecMA(o, m->m_HackedGunPos[1], gpGlobals.v_forward, out);
	VecMA(out, m->m_HackedGunPos[0], gpGlobals.v_right, out);
	VecMA(out, m->m_HackedGunPos[2], gpGlobals.v_up, out);
}
void Mon_GetGunPosition(int e, float o[3]) { DISPATCHV(GetGunPosition, base_GetGunPosition, e, o); }
void Mon_DeathSound(int e) { DISPATCHV(DeathSound, nop_e, e); }
void Mon_AlertSound(int e) { DISPATCHV(AlertSound, nop_e, e); }
void Mon_IdleSound(int e) { DISPATCHV(IdleSound, nop_e, e); }
void Mon_PainSound(int e) { DISPATCHV(PainSound, nop_e, e); }
static void base_StopFollowing(int e, int c) { (void)e; (void)c; }
void Mon_StopFollowing(int e, int c) { DISPATCHV(StopFollowing, base_StopFollowing, e, c); }
float Mon_ChangeYaw(int e, int s) { DISPATCH(ChangeYaw, CBaseMonster_ChangeYaw, e, s); }

/* ---- profiling ------------------------------------------------------------ */

static mon_stats_t	mstat;
static uint32_t		mstat_frame_us;

void mon_stats(mon_stats_t *s, int reset)
{
	*s = mstat;
	if (reset)
	{
		uint32_t mons = mstat.monsters, scr = mstat.scripts;

		memset(&mstat, 0, sizeof(mstat));
		mstat.monsters = mons;
		mstat.scripts = scr;
	}
}

void mon_stats_frame(void)
{
	mstat.us += mstat_frame_us;
	if (mstat_frame_us > mstat.us_max)
		mstat.us_max = mstat_frame_us;
	mstat_frame_us = 0;
}

void mon_stats_route(void) { mstat.routes++; }

/* ---- animation helpers (CBaseAnimating, animating.cpp) -------------------- */

int Mon_LookupActivity(int e, int act) { return gs_lookup_activity(ent_rnd(e)->model, act); }
int Mon_LookupActivityHeaviest(int e, int act) { return gs_lookup_activity_heaviest(ent_rnd(e)->model, act); }
int Mon_LookupSequence(int e, const char *name) { return gs_lookup_sequence(ent_rnd(e)->model, name); }
void Mon_ResetSequenceInfo(int e) { gs_reset_sequence_info(e, &MON(e)->anim); }
float Mon_StudioFrameAdvance(int e, float interval) { return gs_frame_advance(e, &MON(e)->anim, interval); }

void Mon_DispatchAnimEvents(int e, float interval)
{
	float		start, end;
	int		index = 0, next;
	MonsterEvent_t	ev;
	ent_rnd_t	*r = ent_rnd(e);
	int		seq;

	(void)interval;
	if (!r->model)
		return;
	gs_event_window(e, &MON(e)->anim, &start, &end);
	seq = r->sequence;
	while ((next = gs_anim_event(r->model, seq, start, end, index, &ev.event, &ev.options)) != 0)
	{
		ev.res = gs_anim_event_res(r->model, seq, next);
		index = next;
		Mon_HandleAnimEvent(e, &ev);
		if (!ent_is_live(e) || r->sequence != seq)
			break;
	}
}

void Mon_SetBoneController(int e, int i, float value)
{
	gs_set_controller(ent_rnd(e)->model, ent_rnd(e)->controller, i, value);
}

void Mon_InitBoneControllers(int e)
{
	int i;

	for (i = 0; i < 4; i++)
		Mon_SetBoneController(e, i, 0.0f);
}

void Mon_SetBodygroup(int e, int group, int value)
{
	gs_set_bodygroup(ent_rnd(e)->model, &ent_rnd(e)->body, group, value);
}

int Mon_GetBodygroup(int e, int group) { return gs_get_bodygroup(ent_rnd(e)->model, ent_rnd(e)->body, group); }

/* ---- small members -------------------------------------------------------- */

void Mon_Eat(int e, float d) { MON(e)->m_flHungryTime = gtime() + d; }
int Mon_FShouldEat(int e) { return !(MON(e)->m_flHungryTime > gtime()); }

void CBaseMonster_BarnacleVictimBitten(int e, int barnacle)
{
	const Schedule_t *s = Mon_GetScheduleOfType(e, SCHED_BARNACLE_VICTIM_CHOMP);

	(void)barnacle;
	if (s)
		Mon_ChangeSchedule(e, s);
}

void CBaseMonster_BarnacleVictimReleased(int e)
{
	MON(e)->m_IdealMonsterState = MONSTERSTATE_IDLE;
	ent_set_velocity(e, g_vecZero);
	ent_set_movetype(e, MOVETYPE_STEP);
}

/* Listen: the active sound list (aisound.c). */
void Mon_Listen(int e)
{
	CBaseMonster	*m = MON(e);
	int		iSound, iMySounds;
	float		hearing, ear[3], d[3];
	CSound		*s;

	m->m_iAudibleList = SOUNDLIST_EMPTY;
	Mon_ClearConditions(m, bits_COND_HEAR_SOUND | bits_COND_SMELL | bits_COND_SMELL_FOOD);
	m->m_afSoundTypes = 0;
	iMySounds = Mon_ISoundMask(e);
	if (m->m_pSchedule)
		iMySounds &= m->m_pSchedule->iSoundMask;
	iSound = CSoundEnt_ActiveList();
	hearing = Mon_HearingSensitivity(e);
	EyePosition(e, ear);	/* EarPosition = origin + view_ofs */
	while (iSound != SOUNDLIST_EMPTY)
	{
		s = CSoundEnt_SoundPointerForIndex(iSound);
		if (!s)
			break;
		VecSub(s->m_vecOrigin, ear, d);
		if ((s->m_iType & iMySounds) && VecLen(d) <= s->m_iVolume * hearing)
		{
			s->m_iNextAudible = m->m_iAudibleList;
			if (CSound_FIsSound(s))
				Mon_SetConditions(m, bits_COND_HEAR_SOUND);
			else if (s->m_iType & (bits_SOUND_MEAT | bits_SOUND_CARCASS))
				Mon_SetConditions(m, bits_COND_SMELL_FOOD | bits_COND_SMELL);
			else
				Mon_SetConditions(m, bits_COND_SMELL);
			m->m_afSoundTypes |= s->m_iType;
			m->m_iAudibleList = iSound;
		}
		iSound = s->m_iNext;
	}
}

float Mon_FLSoundVolume(int e, const CSound *s)
{
	return s->m_iVolume - mv_dist(s->m_vecOrigin, ent_cv(e)->origin);
}

/* ---- sight ---------------------------------------------------------------- */

/* m_pLink chain of the last Look (HLSDK keeps it in every entity's
 * m_pLink; one looker at a time uses it, as here). */
#define SIGHT_MAX	100
static uint16_t	sight_list[SIGHT_MAX];
static int	sight_count, sight_owner = -1;

static int base_FInViewCone(int e, int other)
{
	float los[3], fwd[3], fov;

	if (e == ENT_PLAYER)
		fov = 0.5f;	/* CBasePlayer::Spawn m_flFieldOfView */
	else
		fov = MON(e)->m_flFieldOfView;
	UTIL_MakeVectors(ent_cv(e)->angles);
	VecSub(ent_cv(other)->origin, ent_cv(e)->origin, los);
	mv_norm2d(los, los);
	mv_norm2d(fwd, gpGlobals.v_forward);
	return los[0] * fwd[0] + los[1] * fwd[1] > fov;
}

int Mon_FInViewConeAt(int e, const float org[3])
{
	float los[3], fwd[3];

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecSub(org, ent_cv(e)->origin, los);
	mv_norm2d(los, los);
	mv_norm2d(fwd, gpGlobals.v_forward);
	return los[0] * fwd[0] + los[1] * fwd[1] > MON(e)->m_flFieldOfView;
}

static float ent_health(int e)
{
	CBaseEntity *b = ent_priv(e) ? CBase(e) : 0;
	return b ? b->health : 0;
}

void CBaseMonster_Look(int e, int iDistance)
{
	CBaseMonster	*m = MON(e);
	int		iSighted = 0, i, count, s;
	uint16_t	list[SIGHT_MAX];
	float		mn[3], mx[3];
	const float	*o = ent_cv(e)->origin;
	int		enemy = EHGet(m->m_hEnemy);

	Mon_ClearConditions(m, bits_COND_SEE_HATE | bits_COND_SEE_DISLIKE | bits_COND_SEE_ENEMY |
		bits_COND_SEE_FEAR | bits_COND_SEE_NEMESIS | bits_COND_SEE_CLIENT);
	sight_owner = e;
	sight_count = 0;
	if (m->spawnflags & SF_MONSTER_PRISONER)
		return;
	for (i = 0; i < 3; i++)
	{
		mn[i] = o[i] - iDistance;
		mx[i] = o[i] + iDistance;
	}
	count = UTIL_EntitiesInBox(list, SIGHT_MAX, mn, mx, FL_CLIENT | FL_MONSTER);
	for (i = 0; i < count; i++)
	{
		s = list[i];
		if (s == e || !ent_priv(s) || (CBase(s)->spawnflags & SF_MONSTER_PRISONER) ||
			ent_health(s) <= 0)
			continue;
		if (Mon_IRelationshipOf(e, s) != R_NO && Mon_FInViewCone(e, s) &&
			!(ent_cv(s)->flags & FL_NOTARGET) && FVisible(e, s))
		{
			if (IsPlayer(s))
			{
				if (m->spawnflags & SF_MONSTER_WAIT_TILL_SEEN)
				{
					if (!Mon_FInViewCone(s, e))
						continue;
					m->spawnflags &= ~SF_MONSTER_WAIT_TILL_SEEN;
				}
				iSighted |= bits_COND_SEE_CLIENT;
			}
			/* HLSDK pushes onto the front of m_pLink */
			if (sight_count < SIGHT_MAX)
			{
				memmove(sight_list + 1, sight_list, sight_count * sizeof(sight_list[0]));
				sight_list[0] = (uint16_t)s;
				sight_count++;
			}
			if (s == enemy)
				iSighted |= bits_COND_SEE_ENEMY;
			switch (Mon_IRelationshipOf(e, s))
			{
			case R_NM: iSighted |= bits_COND_SEE_NEMESIS; break;
			case R_HT: iSighted |= bits_COND_SEE_HATE; break;
			case R_DL: iSighted |= bits_COND_SEE_DISLIKE; break;
			case R_FR: iSighted |= bits_COND_SEE_FEAR; break;
			default: break;
			}
		}
	}
	Mon_SetConditions(m, iSighted);
}

int CBaseMonster_BestVisibleEnemy(int e)
{
	int	i, n, best = -1, bestrel = R_NO, rel;
	float	nearest = 8192, d;

	if (sight_owner != e)
		return -1;
	for (i = 0; i < sight_count; i++)
	{
		n = sight_list[i];
		if (!ent_is_live(n) || !IsAlive(n))
			continue;
		rel = Mon_IRelationshipOf(e, n);
		if (rel > bestrel)
		{
			bestrel = rel;
			nearest = (float)(int)mv_dist(ent_cv(n)->origin, ent_cv(e)->origin);
			best = n;
		}
		else if (rel == bestrel)
		{
			d = (float)(int)mv_dist(ent_cv(n)->origin, ent_cv(e)->origin);
			if (d <= nearest)
			{
				nearest = d;
				best = n;
			}
		}
	}
	return best;
}

/* The sight list of e's last Look (talk monsters walk it), count. */
int Mon_SightList(int e, const uint16_t **list)
{
	*list = sight_list;
	return sight_owner == e ? sight_count : 0;
}

static CSound *base_PBestSound(int e)
{
	CBaseMonster	*m = MON(e);
	int		i = m->m_iAudibleList, best = -1;
	float		bestd = 8192, d, ear[3];
	CSound		*s;

	if (i == SOUNDLIST_EMPTY)
		return 0;
	EyePosition(e, ear);
	while (i != SOUNDLIST_EMPTY)
	{
		s = CSoundEnt_SoundPointerForIndex(i);
		if (!s)
			break;
		if (CSound_FIsSound(s))
		{
			d = mv_dist(s->m_vecOrigin, ear);
			if (d < bestd)
			{
				best = i;
				bestd = d;
			}
		}
		i = s->m_iNextAudible;
	}
	return best >= 0 ? CSoundEnt_SoundPointerForIndex(best) : 0;
}

static CSound *base_PBestScent(int e)
{
	CBaseMonster	*m = MON(e);
	int		i = m->m_iAudibleList, best = -1;
	float		bestd = 8192, d;
	CSound		*s;

	if (i == SOUNDLIST_EMPTY)
		return 0;
	while (i != SOUNDLIST_EMPTY)
	{
		s = CSoundEnt_SoundPointerForIndex(i);
		if (!s)
			break;
		if (CSound_FIsScent(s))
		{
			d = mv_dist(s->m_vecOrigin, ent_cv(e)->origin);
			if (d < bestd)
			{
				best = i;
				bestd = d;
			}
		}
		i = s->m_iNextAudible;
	}
	return best >= 0 ? CSoundEnt_SoundPointerForIndex(best) : 0;
}

/* ---- think ------------------------------------------------------------------ */

void CBaseMonster_MonsterThink(int e)
{
	CBaseMonster	*m = MON(e);
	uint64_t	t0 = sys_time_us();
	float		flInterval;
	int		seq;
	uint32_t	us;

	ThinkIn(e, 0.1f);
	Mon_RunAI(e);
	if (!ent_is_live(e) || MON(e) != m)
		goto out;
	flInterval = Mon_StudioFrameAdvance(e, 0);
	if (m->m_MonsterState != MONSTERSTATE_SCRIPT && m->m_MonsterState != MONSTERSTATE_DEAD &&
		m->m_Activity == ACT_IDLE && m->anim.m_fSequenceFinished)
	{
		if (m->anim.m_fSequenceLoops)
			seq = Mon_LookupActivity(e, m->m_Activity);
		else
			seq = Mon_LookupActivityHeaviest(e, m->m_Activity);
		if (seq != ACTIVITY_NOT_AVAILABLE)
		{
			ent_rnd(e)->sequence = (uint16_t)seq;
			Mon_ResetSequenceInfo(e);
		}
	}
	Mon_DispatchAnimEvents(e, flInterval);
	if (!ent_is_live(e) || MON(e) != m)
		goto out;
	if (!Mon_MovementIsComplete(m))
		Mon_Move(e, flInterval);
out:
	us = (uint32_t)(sys_time_us() - t0);
	mstat.thinks++;
	mstat_frame_us += us;
}

void CBaseMonster_CallMonsterThink(int e) { CBaseMonster_MonsterThink(e); }

void CBaseMonster_MonsterUse(int e, int activator, int caller, int useType, float value)
{
	(void)activator; (void)caller; (void)useType; (void)value;
	MON(e)->m_IdealMonsterState = MONSTERSTATE_ALERT;
}

int CBaseMonster_IgnoreConditions(int e)
{
	CBaseMonster	*m = MON(e);
	int		ign = 0, cine;

	if (!Mon_FShouldEat(e))
		ign |= bits_COND_SMELL_FOOD;
	cine = EHGet(m->m_pCine);
	if (m->m_MonsterState == MONSTERSTATE_SCRIPT && cine >= 0)
		ign |= Script_IgnoreConditions(cine);
	return ign;
}

/* ---- routes ------------------------------------------------------------------ */

void Mon_RouteClear(int e)
{
	CBaseMonster *m = MON(e);

	Mon_RouteNew(e);
	m->m_movementGoal = MOVEGOAL_NONE;
	m->m_movementActivity = ACT_IDLE;
	Mon_Forget(m, bits_MEMORY_MOVE_FAILED);
}

void Mon_RouteNew(int e)
{
	CBaseMonster *m = MON(e);

	m->m_Route[0].iType = 0;
	m->m_iRouteIndex = 0;
}

int Mon_FRouteClear(int e)
{
	CBaseMonster *m = MON(e);

	return m->m_Route[m->m_iRouteIndex].iType == 0 || m->m_movementGoal == MOVEGOAL_NONE;
}

int Mon_FRefreshRoute(int e)
{
	CBaseMonster	*m = MON(e);
	int		pc, i, ret = 0, t;

	Mon_RouteNew(e);
	switch (m->m_movementGoal)
	{
	case MOVEGOAL_PATHCORNER:
		pc = EHGet(m->m_pGoalEnt);
		i = 0;
		while (pc >= 0 && i < ROUTE_SIZE)
		{
			m->m_Route[i].iType = bits_MF_TO_PATHCORNER;
			VecCopy(ent_cv(pc)->origin, m->m_Route[i].vecLocation);
			pc = GetNextTarget(pc);
			if (pc < 0)
				m->m_Route[i].iType |= bits_MF_IS_GOAL;
			i++;
		}
		ret = 1;
		break;
	case MOVEGOAL_ENEMY:
		ret = Mon_BuildRoute(e, m->m_vecEnemyLKP, bits_MF_TO_ENEMY, EHGet(m->m_hEnemy));
		break;
	case MOVEGOAL_LOCATION:
		ret = Mon_BuildRoute(e, m->m_vecMoveGoal, bits_MF_TO_LOCATION, -1);
		break;
	case MOVEGOAL_TARGETENT:
		t = EHGet(m->m_hTargetEnt);
		if (t >= 0)
			ret = Mon_BuildRoute(e, ent_cv(t)->origin, bits_MF_TO_TARGETENT, t);
		break;
	case MOVEGOAL_NODE:
		ret = Mon_FGetNodeRoute(e, m->m_vecMoveGoal);
		break;
	}
	return ret;
}

int Mon_MoveToEnemy(int e, int act, float wait)
{
	CBaseMonster *m = MON(e);

	m->m_movementActivity = act;
	m->m_moveWaitTime = wait;
	m->m_movementGoal = MOVEGOAL_ENEMY;
	return Mon_FRefreshRoute(e);
}

int Mon_MoveToLocation(int e, int act, float wait, const float goal[3])
{
	CBaseMonster *m = MON(e);

	m->m_movementActivity = act;
	m->m_moveWaitTime = wait;
	m->m_movementGoal = MOVEGOAL_LOCATION;
	VecCopy(goal, m->m_vecMoveGoal);
	return Mon_FRefreshRoute(e);
}

int Mon_MoveToTarget(int e, int act, float wait)
{
	CBaseMonster *m = MON(e);

	m->m_movementActivity = act;
	m->m_moveWaitTime = wait;
	m->m_movementGoal = MOVEGOAL_TARGETENT;
	return Mon_FRefreshRoute(e);
}

int Mon_MoveToNode(int e, int act, float wait, const float goal[3])
{
	CBaseMonster *m = MON(e);

	m->m_movementActivity = act;
	m->m_moveWaitTime = wait;
	m->m_movementGoal = MOVEGOAL_NODE;
	VecCopy(goal, m->m_vecMoveGoal);
	return Mon_FRefreshRoute(e);
}

static int ShouldSimplify(int routeType)
{
	routeType &= ~bits_MF_IS_GOAL;
	if (routeType == bits_MF_TO_PATHCORNER || (routeType & bits_MF_DONT_SIMPLIFY))
		return 0;
	return 1;
}

void Mon_RouteSimplify(int e, int target)
{
	CBaseMonster	*m = MON(e);
	int		i, count = 0, outCount = 0, ri = m->m_iRouteIndex, iType;
	float		start[3], test[3], split[3];
	WayPoint_t	out[ROUTE_SIZE * 2 + 1];

	for (i = ri; i < ROUTE_SIZE; i++)
	{
		if (!m->m_Route[i].iType)
			break;
		count++;
		if (m->m_Route[i].iType & bits_MF_IS_GOAL)
			break;
	}
	if (count < 2)
		return;
	VecCopy(ent_cv(e)->origin, start);
	for (i = 0; i < count - 1; i++)
	{
		WayPoint_t *w = &m->m_Route[ri + i], *wn = &m->m_Route[ri + i + 1];

		if (!ShouldSimplify(w->iType))
		{
			out[outCount] = *w;
		}
		else if (Mon_CheckLocalMove(e, start, wn->vecLocation, target, 0) == LOCALMOVE_VALID)
		{
			continue;
		}
		else
		{
			VecAdd(wn->vecLocation, w->vecLocation, test);
			VecScale(test, 0.5f, test);
			VecAdd(w->vecLocation, start, split);
			VecScale(split, 0.5f, split);
			iType = (w->iType | bits_MF_TO_DETOUR) & ~bits_MF_NOT_TO_MASK;
			if (Mon_CheckLocalMove(e, start, test, target, 0) == LOCALMOVE_VALID)
			{
				out[outCount].iType = iType;
				VecCopy(test, out[outCount].vecLocation);
			}
			else if (Mon_CheckLocalMove(e, split, test, target, 0) == LOCALMOVE_VALID)
			{
				out[outCount].iType = iType;
				VecCopy(split, out[outCount].vecLocation);
				out[outCount + 1].iType = iType;
				VecCopy(test, out[outCount + 1].vecLocation);
				outCount++;
			}
			else
			{
				out[outCount] = *w;
			}
		}
		VecCopy(out[outCount].vecLocation, start);
		outCount++;
	}
	out[outCount] = m->m_Route[ri + i];
	outCount++;
	out[outCount].iType = 0;
	m->m_iRouteIndex = 0;
	for (i = 0; i < ROUTE_SIZE && i < outCount; i++)
		m->m_Route[i] = out[i];
	if (i < ROUTE_SIZE)
		m->m_Route[i].iType = 0;
}

int Mon_FBecomeProne(int e)
{
	if (ent_cv(e)->flags & FL_ONGROUND)
		EFlagsClear(e, FL_ONGROUND);
	MON(e)->m_IdealMonsterState = MONSTERSTATE_PRONE;
	return 1;
}

int CBaseMonster_CheckRangeAttack1(int e, float dot, float dist)
{
	(void)e;
	return dist > 64 && dist <= 784 && dot >= 0.5f;
}

int CBaseMonster_CheckRangeAttack2(int e, float dot, float dist)
{
	(void)e;
	return dist > 64 && dist <= 512 && dot >= 0.5f;
}

int CBaseMonster_CheckMeleeAttack1(int e, float dot, float dist)
{
	int en = EHGet(MON(e)->m_hEnemy);

	return dist <= 64 && dot >= 0.7f && en >= 0 && (ent_cv(en)->flags & FL_ONGROUND);
}

int CBaseMonster_CheckMeleeAttack2(int e, float dot, float dist)
{
	(void)e;
	return dist <= 64 && dot >= 0.7f;
}

void Mon_CheckAttacks(int e, int target, float dist)
{
	CBaseMonster	*m = MON(e);
	float		los[3], fwd[3], dot;

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecSub(ent_cv(target)->origin, ent_cv(e)->origin, los);
	mv_norm2d(los, los);
	mv_norm2d(fwd, gpGlobals.v_forward);
	dot = los[0] * fwd[0] + los[1] * fwd[1];
	Mon_ClearConditions(m, bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 |
		bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2);
	if ((m->m_afCapability & bits_CAP_RANGE_ATTACK1) && Mon_CheckRangeAttack1(e, dot, dist))
		Mon_SetConditions(m, bits_COND_CAN_RANGE_ATTACK1);
	if ((m->m_afCapability & bits_CAP_RANGE_ATTACK2) && Mon_CheckRangeAttack2(e, dot, dist))
		Mon_SetConditions(m, bits_COND_CAN_RANGE_ATTACK2);
	if ((m->m_afCapability & bits_CAP_MELEE_ATTACK1) && Mon_CheckMeleeAttack1(e, dot, dist))
		Mon_SetConditions(m, bits_COND_CAN_MELEE_ATTACK1);
	if ((m->m_afCapability & bits_CAP_MELEE_ATTACK2) && Mon_CheckMeleeAttack2(e, dot, dist))
		Mon_SetConditions(m, bits_COND_CAN_MELEE_ATTACK2);
}

int CBaseMonster_CheckEnemy(int e, int enemy)
{
	CBaseMonster	*m = MON(e);
	int		updated = 0, i;
	float		pos[3], d, d2, sz;
	const float	*o = ent_cv(e)->origin;
	const col_ent_t	*ec = ent_cv(enemy);

	Mon_ClearConditions(m, bits_COND_ENEMY_FACING_ME);
	if (!FVisible(e, enemy))
		Mon_SetConditions(m, bits_COND_ENEMY_OCCLUDED);
	else
		Mon_ClearConditions(m, bits_COND_ENEMY_OCCLUDED);
	if (!IsAlive(enemy))
	{
		Mon_SetConditions(m, bits_COND_ENEMY_DEAD);
		Mon_ClearConditions(m, bits_COND_SEE_ENEMY | bits_COND_ENEMY_OCCLUDED);
		return 0;
	}
	VecCopy(ec->origin, pos);
	d = mv_dist(pos, o);
	sz = ec->maxs[2] - ec->mins[2];
	pos[2] += sz * 0.5f;
	d2 = mv_dist(pos, o);
	if (d2 < d)
		d = d2;
	else
	{
		pos[2] -= sz;
		d2 = mv_dist(pos, o);
		if (d2 < d)
			d = d2;
	}
	if (Mon_HasConditions(m, bits_COND_SEE_ENEMY))
	{
		updated = 1;
		VecCopy(ec->origin, m->m_vecEnemyLKP);
		if (mon_is_monster(enemy))
		{
			if (Mon_FInViewCone(enemy, e))
				Mon_SetConditions(m, bits_COND_ENEMY_FACING_ME);
			else
				Mon_ClearConditions(m, bits_COND_ENEMY_FACING_ME);
		}
		if (!VecIsZero(ent_mot(enemy)->velocity))
			VecMA(m->m_vecEnemyLKP, -RANDOM_FLOAT(-0.05f, 0), ent_mot(enemy)->velocity,
				m->m_vecEnemyLKP);
	}
	else if (!Mon_HasConditions(m, bits_COND_ENEMY_OCCLUDED | bits_COND_SEE_ENEMY) && d <= 256)
	{
		updated = 1;
		VecCopy(ec->origin, m->m_vecEnemyLKP);
	}
	if (d >= m->m_flDistTooFar)
		Mon_SetConditions(m, bits_COND_ENEMY_TOOFAR);
	else
		Mon_ClearConditions(m, bits_COND_ENEMY_TOOFAR);
	if (Mon_FCanCheckAttacks(e))
		Mon_CheckAttacks(e, EHGet(m->m_hEnemy) >= 0 ? EHGet(m->m_hEnemy) : enemy, d);
	if (m->m_movementGoal == MOVEGOAL_ENEMY)
	{
		for (i = m->m_iRouteIndex; i < ROUTE_SIZE; i++)
		{
			if (m->m_Route[i].iType == (bits_MF_IS_GOAL | bits_MF_TO_ENEMY) &&
				mv_dist(m->m_Route[i].vecLocation, m->m_vecEnemyLKP) > 80)
			{
				Mon_FRefreshRoute(e);
				return updated;
			}
		}
	}
	return updated;
}

void Mon_PushEnemy(int e, int enemy, const float lkp[3])
{
	CBaseMonster	*m = MON(e);
	int		i;
	ent_h		h;

	if (enemy < 0)
		return;
	h = EHANDLE(enemy);
	for (i = 0; i < MAX_OLD_ENEMIES; i++)
	{
		if (m->m_hOldEnemy[i] == h && EHGet(h) >= 0)
			return;
		if (EHGet(m->m_hOldEnemy[i]) < 0)
			break;
	}
	if (i >= MAX_OLD_ENEMIES)
		return;
	m->m_hOldEnemy[i] = h;
	VecCopy(lkp, m->m_vecOldEnemy[i]);
}

int Mon_PopEnemy(int e)
{
	CBaseMonster	*m = MON(e);
	int		i, o;

	for (i = MAX_OLD_ENEMIES - 1; i >= 0; i--)
	{
		o = EHGet(m->m_hOldEnemy[i]);
		if (o >= 0)
		{
			if (IsAlive(o))
			{
				m->m_hEnemy = m->m_hOldEnemy[i];
				VecCopy(m->m_vecOldEnemy[i], m->m_vecEnemyLKP);
				return 1;
			}
			m->m_hOldEnemy[i] = EH_NONE;
		}
	}
	return 0;
}

void CBaseMonster_SetActivity(int e, int act)
{
	CBaseMonster	*m = MON(e);
	ent_rnd_t	*r = ent_rnd(e);
	int		seq = Mon_LookupActivity(e, act);

	if (seq > ACTIVITY_NOT_AVAILABLE)
	{
		if (r->sequence != seq || !m->anim.m_fSequenceLoops)
		{
			if (!(m->m_Activity == ACT_WALK || m->m_Activity == ACT_RUN) ||
				!(act == ACT_WALK || act == ACT_RUN))
				r->frame = 0;
		}
		r->sequence = (uint16_t)seq;
		Mon_ResetSequenceInfo(e);
		Mon_SetYawSpeed(e);
	}
	else
	{
		r->sequence = 0;
	}
	m->m_Activity = act;
	m->m_IdealActivity = m->m_Activity;
}

void Mon_SetSequenceByName(int e, const char *name)
{
	ent_rnd_t	*r = ent_rnd(e);
	int		seq = Mon_LookupSequence(e, name);

	if (seq > ACTIVITY_NOT_AVAILABLE)
	{
		if (r->sequence != seq || !MON(e)->anim.m_fSequenceLoops)
			r->frame = 0;
		r->sequence = (uint16_t)seq;
		Mon_ResetSequenceInfo(e);
		Mon_SetYawSpeed(e);
	}
	else
	{
		log_printf("mon %d: no sequence %s\n", e, name);
		r->sequence = 0;
	}
}

/* Set e's sequence index (scripted sequences: the name resolved once). */
void Mon_SetSequenceIndex(int e, int seq)
{
	ent_rnd_t *r = ent_rnd(e);

	if (seq >= 0)
	{
		if (r->sequence != seq || !MON(e)->anim.m_fSequenceLoops)
			r->frame = 0;
		r->sequence = (uint16_t)seq;
		Mon_ResetSequenceInfo(e);
		Mon_SetYawSpeed(e);
	}
	else
		r->sequence = 0;
}

int CBaseMonster_CheckLocalMove(int e, const float start[3], const float end[3], int target,
	float *pflDist)
{
	col_ent_t	*cv = ent_cv(e);
	float		startPos[3], dir[3], yaw, dist, step, stepSize;
	int		ret = LOCALMOVE_VALID;

	mstat.local_moves++;
	VecCopy(cv->origin, startPos);
	VecSub(end, start, dir);
	yaw = UTIL_VecToYaw(dir);
	dist = VecLen2D(dir);
	ent_set_origin(e, start);
	if (!(cv->flags & (FL_FLY | FL_SWIM)))
		phy_drop_to_floor(e);
	for (step = 0; step < dist; step += LOCAL_STEP_SIZE)
	{
		stepSize = LOCAL_STEP_SIZE;
		if (step + LOCAL_STEP_SIZE >= dist - 1)
			stepSize = (dist - step) - 1;
		if (!phy_walk_move(e, yaw, stepSize, WALKMOVE_CHECKONLY))
		{
			if (pflDist)
				*pflDist = step;
			if (target >= 0 && target == phy_trace_ent())
				ret = LOCALMOVE_VALID;
			else
				ret = LOCALMOVE_INVALID;
			break;
		}
	}
	if (ret == LOCALMOVE_VALID && !(cv->flags & (FL_FLY | FL_SWIM)) &&
		(target < 0 || (ent_cv(target)->flags & FL_ONGROUND)))
	{
		if (fabsf(end[2] - cv->origin[2]) > 64)
			ret = LOCALMOVE_INVALID_DONT_TRIANGULATE;
	}
	ent_set_origin(e, startPos);
	return ret;
}

float Mon_OpenDoorAndWait(int e, int door)
{
	float	travel = 0;
	name_t	n;
	int	it = 0, d;

	if (door >= 0 && !IsLockedByMaster(door))
	{
		UseEntity(door, e, e, USE_ON, 0.0f);
		travel = ent_tm(door)->nextthink - ent_tm(door)->ltime;
		n = ent_idc(door)->targetname;
		if (n != NAME_NONE)
		{
			while ((d = UTIL_FindEntityByTargetname(n, &it)) >= 0)
			{
				if (d != door && ent_idc(d)->cls == ent_idc(door)->cls)
					UseEntity(d, e, e, USE_ON, 0.0f);
			}
		}
	}
	return gtime() + travel;
}

void Mon_AdvanceRoute(int e, float distance)
{
	CBaseMonster	*m = MON(e);
	int		ri = m->m_iRouteIndex, door;

	if (ri == ROUTE_SIZE - 1)
	{
		Mon_FRefreshRoute(e);
		return;
	}
	if (!(m->m_Route[ri].iType & bits_MF_IS_GOAL))
	{
		if ((m->m_Route[ri].iType & ~bits_MF_NOT_TO_MASK) == bits_MF_TO_PATHCORNER)
		{
			int pc = EHGet(m->m_pGoalEnt);
			m->m_pGoalEnt = EHANDLE(pc >= 0 ? GetNextTarget(pc) : -1);
		}
		if ((m->m_Route[ri].iType & bits_MF_TO_NODE) == bits_MF_TO_NODE &&
			(m->m_Route[ri + 1].iType & bits_MF_TO_NODE) == bits_MF_TO_NODE)
		{
			door = Graph_RouteDoor(e, m->m_Route[ri].vecLocation, m->m_Route[ri + 1].vecLocation);
			if (door >= 0)
				m->m_flMoveWaitFinished = Mon_OpenDoorAndWait(e, door);
		}
		m->m_iRouteIndex++;
	}
	else if (distance < m->anim.m_flGroundSpeed * 0.2f)
	{
		Mon_MovementComplete(e);
	}
}

int Mon_RouteClassify(int e, int f)
{
	(void)e;
	if (f & bits_MF_TO_TARGETENT)
		return MOVEGOAL_TARGETENT;
	if (f & bits_MF_TO_ENEMY)
		return MOVEGOAL_ENEMY;
	if (f & bits_MF_TO_PATHCORNER)
		return MOVEGOAL_PATHCORNER;
	if (f & bits_MF_TO_NODE)
		return MOVEGOAL_NODE;
	if (f & bits_MF_TO_LOCATION)
		return MOVEGOAL_LOCATION;
	return MOVEGOAL_NONE;
}

int Mon_BuildRoute(int e, const float goal[3], int flag, int target)
{
	CBaseMonster	*m = MON(e);
	float		dist = 0, apex[3], g[3];
	int		lm;

	VecCopy(goal, g);	/* goal may point into m_Route / a component */
	mstat.routes++;
	Mon_RouteNew(e);
	m->m_movementGoal = Mon_RouteClassify(e, flag);
	VecCopy(g, m->m_Route[0].vecLocation);
	m->m_Route[0].iType = flag | bits_MF_IS_GOAL;
	lm = Mon_CheckLocalMove(e, ent_cv(e)->origin, g, target, &dist);
	if (lm == LOCALMOVE_VALID)
		return 1;
	if (lm != LOCALMOVE_INVALID_DONT_TRIANGULATE &&
		Mon_FTriangulate(e, ent_cv(e)->origin, g, dist, target, apex))
	{
		VecCopy(apex, m->m_Route[0].vecLocation);
		m->m_Route[0].iType = flag | bits_MF_TO_DETOUR;
		VecCopy(g, m->m_Route[1].vecLocation);
		m->m_Route[1].iType = flag | bits_MF_IS_GOAL;
		Mon_RouteSimplify(e, target);
		return 1;
	}
	if (Mon_FGetNodeRoute(e, g))
	{
		VecCopy(g, m->m_vecMoveGoal);
		Mon_RouteSimplify(e, target);
		return 1;
	}
	return 0;
}

void Mon_InsertWaypoint(int e, const float loc[3], int flags)
{
	CBaseMonster	*m = MON(e);
	int		i, type;

	type = flags | (m->m_Route[m->m_iRouteIndex].iType & ~bits_MF_NOT_TO_MASK);
	for (i = ROUTE_SIZE - 1; i > 0; i--)
		m->m_Route[i] = m->m_Route[i - 1];
	VecCopy(loc, m->m_Route[m->m_iRouteIndex].vecLocation);
	m->m_Route[m->m_iRouteIndex].iType = type;
}

int CBaseMonster_FTriangulate(int e, const float start[3], const float end[3], float dist,
	int target, float apex[3])
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	float		dir[3], fwd[3], up[3] = { 0, 0, 1 }, left[3], right[3], top[3], bottom[3];
	float		far[3], d[3], sizeX, sizeZ;
	int		i, fly = cv->movetype == MOVETYPE_FLY;

	sizeX = cv->maxs[0] - cv->mins[0];
	if (sizeX < 24.0f)
		sizeX = 24.0f;
	else if (sizeX > 48.0f)
		sizeX = 48.0f;
	sizeZ = cv->maxs[2] - cv->mins[2];
	VecSub(end, start, d);
	VecNorm(d, fwd);
	VecCross(fwd, up, dir);
	VecMA(cv->origin, dist + sizeX, fwd, left);
	VecMA(left, -(sizeX * 3), dir, left);
	VecMA(cv->origin, dist + sizeX, fwd, right);
	VecMA(right, sizeX * 3, dir, right);
	if (fly)
	{
		VecMA(cv->origin, dist, fwd, top);
		VecMA(top, sizeZ * 3, up, top);
		VecMA(cv->origin, dist, fwd, bottom);
		VecMA(bottom, -(sizeZ * 3), up, bottom);
	}
	VecCopy(m->m_Route[m->m_iRouteIndex].vecLocation, far);
	VecScale(dir, sizeX * 2, dir);
	if (fly)
		VecScale(up, sizeZ * 2, up);
	for (i = 0; i < 8; i++)
	{
		if (Mon_CheckLocalMove(e, cv->origin, right, target, 0) == LOCALMOVE_VALID &&
			Mon_CheckLocalMove(e, right, far, target, 0) == LOCALMOVE_VALID)
		{
			if (apex)
				VecCopy(right, apex);
			return 1;
		}
		if (Mon_CheckLocalMove(e, cv->origin, left, target, 0) == LOCALMOVE_VALID &&
			Mon_CheckLocalMove(e, left, far, target, 0) == LOCALMOVE_VALID)
		{
			if (apex)
				VecCopy(left, apex);
			return 1;
		}
		if (fly)
		{
			if (Mon_CheckLocalMove(e, cv->origin, top, target, 0) == LOCALMOVE_VALID &&
				Mon_CheckLocalMove(e, top, far, target, 0) == LOCALMOVE_VALID)
			{
				if (apex)
					VecCopy(top, apex);
				return 1;
			}
			if (Mon_CheckLocalMove(e, cv->origin, bottom, target, 0) == LOCALMOVE_VALID &&
				Mon_CheckLocalMove(e, bottom, far, target, 0) == LOCALMOVE_VALID)
			{
				if (apex)
					VecCopy(bottom, apex);
				return 1;
			}
		}
		VecAdd(right, dir, right);
		VecSub(left, dir, left);
		if (fly)
		{
			VecAdd(top, up, top);
			VecSub(bottom, up, bottom);
		}
	}
	return 0;
}

void CBaseMonster_Move(int e, float flInterval)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	float		wpDist, checkDist, dist, dir[3], d[3], apex[3], to[3];
	int		target = -1, blocker;
	WayPoint_t	*w;

	if (Mon_FRouteClear(e))
	{
		if (m->m_movementGoal == MOVEGOAL_NONE || !Mon_FRefreshRoute(e))
		{
			Mon_TaskFail(m);
			return;
		}
	}
	if (m->m_flMoveWaitFinished > gtime())
		return;
	w = &m->m_Route[m->m_iRouteIndex];
	VecSub(w->vecLocation, cv->origin, d);
	VecNorm(d, dir);
	wpDist = VecLen2D(d);
	Mon_MakeIdealYaw(e, w->vecLocation);
	Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
	checkDist = wpDist < DIST_TO_CHECK ? wpDist : DIST_TO_CHECK;
	if ((w->iType & ~bits_MF_NOT_TO_MASK) == bits_MF_TO_ENEMY)
		target = EHGet(m->m_hEnemy);
	else if ((w->iType & ~bits_MF_NOT_TO_MASK) == bits_MF_TO_TARGETENT)
		target = EHGet(m->m_hTargetEnt);
	dist = 0;
	VecMA(cv->origin, checkDist, dir, to);
	if (Mon_CheckLocalMove(e, cv->origin, to, target, &dist) != LOCALMOVE_VALID)
	{
		Mon_Stop(e);
		blocker = phy_trace_ent();
		if (blocker > 0 && ent_is_live(blocker))
			game_blocked(e, blocker);
		if (!ent_is_live(e))
			return;
		if (blocker > 0 && ent_is_live(blocker) && m->m_moveWaitTime > 0 && IsMoving(blocker) &&
			!IsPlayer(blocker) && (gtime() - m->m_flMoveWaitFinished) > 3.0f)
		{
			if (dist < m->anim.m_flGroundSpeed)
			{
				m->m_flMoveWaitFinished = gtime() + m->m_moveWaitTime;
				return;
			}
		}
		else
		{
			if (Mon_FTriangulate(e, cv->origin, w->vecLocation, dist, target, apex))
			{
				Mon_InsertWaypoint(e, apex, bits_MF_TO_DETOUR);
				Mon_RouteSimplify(e, target);
			}
			else
			{
				Mon_Stop(e);
				if (m->m_moveWaitTime > 0 && !Mon_HasMemory(m, bits_MEMORY_MOVE_FAILED))
				{
					Mon_FRefreshRoute(e);
					if (Mon_FRouteClear(e))
						Mon_TaskFail(m);
					else
					{
						if (gtime() - m->m_flMoveWaitFinished < 0.2f)
							Mon_Remember(m, bits_MEMORY_MOVE_FAILED);
						m->m_flMoveWaitFinished = gtime() + 0.1f;
					}
				}
				else
				{
					Mon_TaskFail(m);
				}
				return;
			}
		}
	}
	if (Mon_ShouldAdvanceRoute(e, wpDist))
		Mon_AdvanceRoute(e, wpDist);
	if (m->m_flMoveWaitFinished > gtime())
	{
		Mon_Stop(e);
		return;
	}
	if (checkDist < m->anim.m_flGroundSpeed * flInterval)
		flInterval = checkDist / m->anim.m_flGroundSpeed;
	Mon_MoveExecute(e, target, dir, flInterval);
	if (Mon_MovementIsComplete(m))
	{
		Mon_Stop(e);
		Mon_RouteClear(e);
	}
}

void CBaseMonster_MoveExecute(int e, int target, const float dir[3], float flInterval)
{
	CBaseMonster	*m = MON(e);
	float		total, step, fr = ent_rnd(e)->framerate;

	(void)target; (void)dir;
	if (m->m_IdealActivity != m->m_movementActivity)
		m->m_IdealActivity = m->m_movementActivity;
	total = m->anim.m_flGroundSpeed * fr * flInterval;
	while (total > 0.001f)
	{
		step = total < 16.0f ? total : 16.0f;
		phy_move_to_origin(e, m->m_Route[m->m_iRouteIndex].vecLocation, step, MOVE_NORMAL);
		total -= step;
	}
}

/* ---- init -------------------------------------------------------------------- */

void CBaseMonster_MonsterInit(int e)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);

	ent_rnd(e)->effects = 0;
	m->takedamage = DAMAGE_AIM;
	ent_phys(e)->ideal_yaw = cv->angles[1];
	m->max_health = m->health;
	m->deadflag = DEAD_NO;
	m->m_IdealMonsterState = MONSTERSTATE_IDLE;
	m->m_IdealActivity = ACT_IDLE;
	EFlagsSet(e, FL_MONSTER);
	if (m->spawnflags & SF_MONSTER_HITMONSTERCLIP)
		EFlagsSet(e, FL_MONSTERCLIP);
	Mon_ClearSchedule(e);
	Mon_RouteClear(e);
	Mon_InitBoneControllers(e);
	m->m_iHintNode = NO_NODE;
	m->m_afMemory = MEMORY_CLEAR;
	m->m_hEnemy = EH_NONE;
	m->m_hTargetEnt = EH_NONE;
	m->m_pCine = EH_NONE;
	m->m_pGoalEnt = EH_NONE;
	for (int i = 0; i < MAX_OLD_ENEMIES; i++)
		m->m_hOldEnemy[i] = EH_NONE;
	m->m_flDistTooFar = 1024.0f;
	m->m_flDistLook = 2048.0f;
	m->m_iAudibleList = SOUNDLIST_EMPTY;
	Mon_SetEyePosition(e);
	SetThink(e, FN_CBaseMonster_MonsterInitThink);
	ThinkIn(e, 0.1f);
	SetUse(e, FN_CBaseMonster_MonsterUse);
	mstat.monsters++;
}

void CBaseMonster_MonsterInitThink(int e) { Mon_StartMonster(e); }

void CBaseMonster_StartMonster(int e)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	float		org[3];
	int		g;

	if (Mon_LookupActivity(e, ACT_RANGE_ATTACK1) != ACTIVITY_NOT_AVAILABLE)
		m->m_afCapability |= bits_CAP_RANGE_ATTACK1;
	if (Mon_LookupActivity(e, ACT_RANGE_ATTACK2) != ACTIVITY_NOT_AVAILABLE)
		m->m_afCapability |= bits_CAP_RANGE_ATTACK2;
	if (Mon_LookupActivity(e, ACT_MELEE_ATTACK1) != ACTIVITY_NOT_AVAILABLE)
		m->m_afCapability |= bits_CAP_MELEE_ATTACK1;
	if (Mon_LookupActivity(e, ACT_MELEE_ATTACK2) != ACTIVITY_NOT_AVAILABLE)
		m->m_afCapability |= bits_CAP_MELEE_ATTACK2;
	if (cv->movetype != MOVETYPE_FLY && !(m->spawnflags & SF_MONSTER_FALL_TO_GROUND))
	{
		VecCopy(cv->origin, org);
		org[2] += 1;
		ent_set_origin(e, org);
		phy_drop_to_floor(e);
		if (!phy_walk_move(e, 0, 0, WALKMOVE_NORMAL))
		{
			log_printf("Monster %d (class %u) stuck in wall--level design error\n", e,
				(unsigned)ent_idc(e)->cls);
			ent_rnd(e)->effects = EF_BRIGHTFIELD;
		}
	}
	else
	{
		EFlagsClear(e, FL_ONGROUND);
	}
	if (m->target != NAME_NONE)
	{
		g = UTIL_FindByTargetname1(m->target);
		m->m_pGoalEnt = EHANDLE(g);
		if (g >= 0)
		{
			Mon_MakeIdealYaw(e, ent_cv(g)->origin);
			m->m_movementGoal = MOVEGOAL_PATHCORNER;
			m->m_movementActivity = cv->movetype == MOVETYPE_FLY ? ACT_FLY : ACT_WALK;
			Mon_FRefreshRoute(e);
			Mon_SetState(e, MONSTERSTATE_IDLE);
			Mon_ChangeSchedule(e, Mon_GetScheduleOfType(e, SCHED_IDLE_WALK));
		}
		else
			log_printf("ReadyMonster()--%d couldn't find its target\n", e);
	}
	SetThink(e, FN_CBaseMonster_CallMonsterThink);
	ent_think_at(e, ent_tm(e)->nextthink + RANDOM_FLOAT(0.1f, 0.4f));
	if (ent_idc(e)->targetname != NAME_NONE)
	{
		Mon_SetState(e, MONSTERSTATE_IDLE);
		Mon_SetActivity(e, ACT_IDLE);
		Mon_ChangeSchedule(e, Mon_GetScheduleOfType(e, SCHED_WAIT_TRIGGER));
	}
}

void Mon_MovementComplete(int e)
{
	CBaseMonster *m = MON(e);

	switch (m->m_iTaskStatus)
	{
	case TASKSTATUS_NEW:
	case TASKSTATUS_RUNNING:
		m->m_iTaskStatus = TASKSTATUS_RUNNING_TASK;
		break;
	case TASKSTATUS_RUNNING_MOVEMENT:
		Mon_TaskComplete(m);
		break;
	default:
		break;
	}
	m->m_movementGoal = MOVEGOAL_NONE;
}

int Mon_TaskIsRunning(int e)
{
	CBaseMonster *m = MON(e);

	return m->m_iTaskStatus != TASKSTATUS_COMPLETE && m->m_iTaskStatus != TASKSTATUS_RUNNING_MOVEMENT;
}

/* monsters.cpp IRelationship iEnemy table */
static const int8_t iEnemy[14][14] = {
	/*        NONE  MACH  PLYR  HPASS HMIL  AMIL  APASS AMONS APREY APRED INSEC PLRAL PBWPN ABWPN */
	/*NONE*/ { R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO },
	/*MACH*/ { R_NO, R_NO, R_DL, R_DL, R_NO, R_DL, R_DL, R_DL, R_DL, R_DL, R_NO, R_DL, R_DL, R_DL },
	/*PLYR*/ { R_NO, R_DL, R_NO, R_NO, R_DL, R_DL, R_DL, R_DL, R_DL, R_DL, R_NO, R_NO, R_DL, R_DL },
	/*HPAS*/ { R_NO, R_NO, R_AL, R_AL, R_HT, R_FR, R_NO, R_HT, R_DL, R_FR, R_NO, R_AL, R_NO, R_NO },
	/*HMIL*/ { R_NO, R_NO, R_HT, R_DL, R_NO, R_HT, R_DL, R_DL, R_DL, R_DL, R_NO, R_HT, R_NO, R_NO },
	/*AMIL*/ { R_NO, R_DL, R_HT, R_DL, R_HT, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_DL, R_NO, R_NO },
	/*APAS*/ { R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO },
	/*AMON*/ { R_NO, R_DL, R_DL, R_DL, R_DL, R_NO, R_NO, R_NO, R_NO, R_NO, R_NO, R_DL, R_NO, R_NO },
	/*APRY*/ { R_NO, R_NO, R_DL, R_DL, R_DL, R_NO, R_NO, R_NO, R_NO, R_FR, R_NO, R_DL, R_NO, R_NO },
	/*APRD*/ { R_NO, R_NO, R_DL, R_DL, R_DL, R_NO, R_NO, R_NO, R_HT, R_DL, R_NO, R_DL, R_NO, R_NO },
	/*INSC*/ { R_FR, R_FR, R_FR, R_FR, R_FR, R_NO, R_FR, R_FR, R_FR, R_FR, R_NO, R_FR, R_NO, R_NO },
	/*PALY*/ { R_NO, R_DL, R_AL, R_AL, R_DL, R_DL, R_DL, R_DL, R_DL, R_DL, R_NO, R_NO, R_NO, R_NO },
	/*PBWP*/ { R_NO, R_NO, R_DL, R_DL, R_DL, R_DL, R_DL, R_DL, R_DL, R_DL, R_NO, R_DL, R_NO, R_DL },
	/*ABWP*/ { R_NO, R_NO, R_DL, R_DL, R_DL, R_AL, R_NO, R_DL, R_DL, R_NO, R_NO, R_DL, R_DL, R_NO },
};

int CBaseMonster_IRelationship(int e, int other)
{
	int a = Classify(e), b = Classify(other);

	/* CLASS_BARNACLE (99) and out-of-table classes: no relationship (the
	 * HLSDK table read past its end for them; barnacles are never looked
	 * at because they are SF_MONSTER_PRISONER-like FL_NOTARGET?); R_NO. */
	if (a < 0 || a >= 14 || b < 0 || b >= 14)
		return R_NO;
	return iEnemy[a][b];
}

/* The looker's virtual IRelationship (game_vt_t slot, else the table). */
int Mon_IRelationshipOf(int e, int other)
{
	const game_vt_t *vt = ent_vt(e);

	if (vt && vt->IRelationship)
		return vt->IRelationship(e, other);
	return CBaseMonster_IRelationship(e, other);
}

/* ---- cover and routes through the graph ------------------------------------- */

int CBaseMonster_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx)
{
	float spot[3];

	if (!mx)
		mx = 784;
	if (mn > 0.5f * mx)
		mn = 0.5f * mx;
	return Graph_FindCover(e, threat, viewofs, mn, mx, spot);
}

int CBaseMonster_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn,
	float mx)
{
	if (!mx)
		mx = 784;
	if (mn > 0.5f * mx)
		mn = 0.5f * mx;
	return Graph_BuildNearestRoute(e, threat, viewofs, mn, mx);
}

void Mon_MakeIdealYaw(int e, const float target[3])
{
	float	proj[3], d[3];
	int	ma = MON(e)->m_movementActivity;
	const float *o = ent_cv(e)->origin;

	if (ma == ACT_STRAFE_LEFT)
	{
		proj[0] = -target[1];
		proj[1] = target[0];
		proj[2] = 0;
		VecSub(proj, o, d);
	}
	else if (ma == ACT_STRAFE_RIGHT)
	{
		proj[0] = target[1];
		proj[1] = target[0];
		proj[2] = 0;
		VecSub(proj, o, d);
	}
	else
		VecSub(target, o, d);
	ent_phys(e)->ideal_yaw = UTIL_VecToYaw(d);
}

float Mon_FlYawDiff(int e)
{
	float cur = UTIL_AngleMod(ent_cv(e)->angles[1]);

	if (cur == ent_phys(e)->ideal_yaw)
		return 0;
	return UTIL_AngleDiff(ent_phys(e)->ideal_yaw, cur);
}

float CBaseMonster_ChangeYaw(int e, int yawSpeed)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	float		ideal, current, move, speed, delta, yaw;

	current = UTIL_AngleMod(cv->angles[1]);
	ideal = ent_phys(e)->ideal_yaw;
	if (current != ideal)
	{
		if (m->m_flLastYawTime == 0.f)
			m->m_flLastYawTime = gtime() - gframetime();
		delta = gtime() - m->m_flLastYawTime;
		m->m_flLastYawTime = gtime();
		if (delta > 0.25f)
			delta = 0.25f;
		speed = (float)yawSpeed * delta * 2;
		move = ideal - current;
		if (ideal > current)
		{
			if (move >= 180)
				move = move - 360;
		}
		else
		{
			if (move <= -180)
				move = move + 360;
		}
		if (move > 0)
		{
			if (move > speed)
				move = speed;
		}
		else
		{
			if (move < -speed)
				move = -speed;
		}
		cv->angles[1] = UTIL_AngleMod(current + move);
		if (m->m_afCapability & bits_CAP_TURN_HEAD)
		{
			yaw = ideal - cv->angles[1];
			if (yaw > 180)
				yaw -= 360;
			if (yaw < -180)
				yaw += 360;
			Mon_SetBoneController(e, 0, yaw);
		}
	}
	else
		move = 0;
	return move;
}

float Mon_VecToYaw(int e, const float dir[3])
{
	if (dir[0] == 0 && dir[1] == 0 && dir[2] == 0)
		return ent_cv(e)->angles[1];
	return UTIL_VecToYaw(dir);
}

void Mon_SetEyePosition(int e)
{
	gs_eye_position(ent_rnd(e)->model, MON(e)->view_ofs);
}

void CBaseMonster_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	CBaseMonster	*m = MON(e);
	int		cine;

	switch (ev->event)
	{
	case SCRIPT_EVENT_DEAD:
		if (m->m_MonsterState == MONSTERSTATE_SCRIPT)
		{
			m->deadflag = DEAD_DYING;
			m->health = 0;
		}
		break;
	case SCRIPT_EVENT_NOT_DEAD:
		if (m->m_MonsterState == MONSTERSTATE_SCRIPT)
		{
			m->deadflag = DEAD_NO;
			m->health = m->max_health;
		}
		break;
	case SCRIPT_EVENT_SOUND:
		if (ev->res)
			EMIT_SOUND(e, CHAN_BODY, ev->res, 1.0f, ATTN_IDLE);
		break;
	case SCRIPT_EVENT_SOUND_VOICE:
		if (ev->res)
			EMIT_SOUND(e, CHAN_VOICE, ev->res, 1.0f, ATTN_IDLE);
		break;
	case SCRIPT_EVENT_SENTENCE_RND1:
		if (RANDOM_LONG(0, 2) == 0)
			break;
		/* fall through */
	case SCRIPT_EVENT_SENTENCE:
		if (ev->res)
			EMIT_SENTENCE(e, CHAN_VOICE, ev->res, 1.0f, ATTN_IDLE, 0, 100);
		break;
	case SCRIPT_EVENT_FIREEVENT:
		if (ev->options)
		{
			name_t n = rec_name_find(ent_idc(e)->layer, ev->options);
			if (n != NAME_NONE)
				FireTargets(n, e, e, USE_TOGGLE, 0);
		}
		break;
	case SCRIPT_EVENT_NOINTERRUPT:
		cine = EHGet(m->m_pCine);
		if (cine >= 0)
			Script_AllowInterrupt(cine, 0);
		break;
	case SCRIPT_EVENT_CANINTERRUPT:
		cine = EHGet(m->m_pCine);
		if (cine >= 0)
			Script_AllowInterrupt(cine, 1);
		break;
	case MONSTER_EVENT_BODYDROP_HEAVY:
		if (ent_cv(e)->flags & FL_ONGROUND)
			EMIT_SOUND_DYN(e, CHAN_BODY, RANDOM_LONG(0, 1) == 0 ? RESID(MON_BODYDROP3) :
				RESID(MON_BODYDROP4), 1, ATTN_NORM, 0, 90);
		break;
	case MONSTER_EVENT_BODYDROP_LIGHT:
		if (ent_cv(e)->flags & FL_ONGROUND)
			EMIT_SOUND(e, CHAN_BODY, RANDOM_LONG(0, 1) == 0 ? RESID(MON_BODYDROP3) :
				RESID(MON_BODYDROP4), 1, ATTN_NORM);
		break;
	case MONSTER_EVENT_SWISHSOUND:
		EMIT_SOUND(e, CHAN_BODY, RESID(MON_CLAW_MISS2), 1, ATTN_NORM);
		break;
	default:
		break;
	}
}

int Mon_FGetNodeRoute(int e, const float dest[3])
{
	CBaseMonster	*m = MON(e);
	int		path[MAX_PATH_SIZE], src, dst, n, i, copy, slot = ent_idc(e)->layer;

	src = Graph_FindNearestNode(e, ent_cv(e)->origin, -1);
	dst = Graph_FindNearestNode(e, dest, -1);
	if (src < 0 || dst < 0)
		return 0;
	n = Graph_FindShortestPath(slot, path, src, dst, Graph_HullIndex(e), m->m_afCapability);
	if (!n)
		return 0;
	copy = n < ROUTE_SIZE ? n : ROUTE_SIZE;
	for (i = 0; i < copy; i++)
	{
		VecCopy(Graph_NodeOrigin(slot, path[i]), m->m_Route[i].vecLocation);
		m->m_Route[i].iType = bits_MF_TO_NODE;
	}
	if (copy < ROUTE_SIZE)
	{
		VecCopy(dest, m->m_Route[copy].vecLocation);
		m->m_Route[copy].iType |= bits_MF_IS_GOAL;
	}
	return 1;
}

int Mon_FindHintNode(int e)
{
	/* The cooked graph carries no hint nodes (HLN1 has no hint fields; the
	 * Uplink / Hazard maps place none): HLSDK returns NO_NODE when no node
	 * has a hint type. */
	(void)e;
	return NO_NODE;
}

int Tlk_KeyValue(int e, gid_t16 gid);

/* CBaseMonster::KeyValue (TriggerTarget, TriggerCondition) + the toggle
 * chain; called by every monster spawn. */
void CBaseMonster_KeyValue(int e, gid_t16 gid)
{
	CBaseMonster *m = MON(e);

	m->m_makerGid = GID_NONE;
	m->spawnflags |= mon_spawn_extra_flags;
	m->m_hEnemy = m->m_hTargetEnt = m->m_pCine = EH_NONE;
	m->m_iAudibleList = SOUNDLIST_EMPTY;
	if (gid == GID_NONE)
	{
		m->m_iszTriggerTarget = NAME_NONE;
		return;
	}
	CBaseToggle_KeyValue(e, gid);
	m->m_iszTriggerTarget = rec_key_name(gid, K_TRIGGERTARGET);
	m->m_iTriggerCondition = (int)KeyF(gid, K_TRIGGERCONDITION, 0);
}

int Mon_FCheckAITrigger(int e)
{
	CBaseMonster	*m = MON(e);
	int		fire = 0, en;

	if (m->m_iTriggerCondition == AITRIGGER_NONE)
		return 0;
	switch (m->m_iTriggerCondition)
	{
	case AITRIGGER_SEEPLAYER_ANGRY_AT_PLAYER:
		en = EHGet(m->m_hEnemy);
		fire = en >= 0 && IsPlayer(en) && Mon_HasConditions(m, bits_COND_SEE_ENEMY);
		break;
	case AITRIGGER_SEEPLAYER_UNCONDITIONAL:
		fire = Mon_HasConditions(m, bits_COND_SEE_CLIENT);
		break;
	case AITRIGGER_SEEPLAYER_NOT_IN_COMBAT:
		fire = Mon_HasConditions(m, bits_COND_SEE_CLIENT) &&
			m->m_MonsterState != MONSTERSTATE_COMBAT &&
			m->m_MonsterState != MONSTERSTATE_PRONE &&
			m->m_MonsterState != MONSTERSTATE_SCRIPT;
		break;
	case AITRIGGER_TAKEDAMAGE:
		fire = (m->m_afConditions & (bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE)) != 0;
		break;
	case AITRIGGER_DEATH:
		fire = m->deadflag != DEAD_NO;
		break;
	case AITRIGGER_HALFHEALTH:
		fire = IsAlive(e) && m->health <= m->max_health / 2;
		break;
	case AITRIGGER_HEARWORLD:
		fire = (m->m_afConditions & bits_COND_HEAR_SOUND) && (m->m_afSoundTypes & bits_SOUND_WORLD);
		break;
	case AITRIGGER_HEARPLAYER:
		fire = (m->m_afConditions & bits_COND_HEAR_SOUND) && (m->m_afSoundTypes & bits_SOUND_PLAYER);
		break;
	case AITRIGGER_HEARCOMBAT:
		fire = (m->m_afConditions & bits_COND_HEAR_SOUND) && (m->m_afSoundTypes & bits_SOUND_COMBAT);
		break;
	}
	if (fire)
	{
		m->m_iTriggerCondition = AITRIGGER_NONE;
		if (m->m_iszTriggerTarget != NAME_NONE)
			FireTargets(m->m_iszTriggerTarget, e, e, USE_TOGGLE, 0);
		return 1;
	}
	return 0;
}

int CBaseMonster_CanPlaySequence(int e, int fDisregard, int interruptLevel)
{
	CBaseMonster *m = MON(e);

	if (EHGet(m->m_pCine) >= 0 || !IsAlive(e) || m->m_MonsterState == MONSTERSTATE_PRONE)
		return 0;
	if (fDisregard)
		return 1;
	if (m->m_MonsterState == MONSTERSTATE_NONE || m->m_MonsterState == MONSTERSTATE_IDLE ||
		m->m_IdealMonsterState == MONSTERSTATE_IDLE)
		return 1;
	if (m->m_MonsterState == MONSTERSTATE_ALERT && interruptLevel >= SS_INTERRUPT_BY_NAME)
		return 1;
	return 0;
}

#define COVER_CHECKS	5
#define COVER_DELTA	48

int Mon_FindLateralCover(int e, const float threat[3], const float viewofs[3])
{
	col_trace_t	tr;
	float		stepR[3], lt[3], rt[3], a[3], b[3];
	const float	*vo = MON(e)->view_ofs, *o = ent_cv(e)->origin;
	int		i;

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecScale(gpGlobals.v_right, COVER_DELTA, stepR);
	stepR[2] = 0;
	VecCopy(o, lt);
	VecCopy(o, rt);
	VecAdd(threat, viewofs, a);
	for (i = 0; i < COVER_CHECKS; i++)
	{
		VecSub(lt, stepR, lt);
		VecAdd(rt, stepR, rt);
		VecAdd(lt, vo, b);
		UTIL_TraceLineGlass(a, b, ignore_monsters, ignore_glass, e, &tr);
		if (tr.frac != 1.0f && Mon_FValidateCover(e, lt) &&
			Mon_CheckLocalMove(e, o, lt, -1, 0) == LOCALMOVE_VALID &&
			Mon_MoveToLocation(e, ACT_RUN, 0, lt))
			return 1;
		VecAdd(rt, vo, b);
		UTIL_TraceLineGlass(a, b, ignore_monsters, ignore_glass, e, &tr);
		if (tr.frac != 1.0f && Mon_FValidateCover(e, rt) &&
			Mon_CheckLocalMove(e, o, rt, -1, 0) == LOCALMOVE_VALID &&
			Mon_MoveToLocation(e, ACT_RUN, 0, rt))
			return 1;
	}
	return 0;
}

void Mon_ShootAtEnemy(int e, const float src[3], float out[3])
{
	CBaseMonster	*m = MON(e);
	int		en = EHGet(m->m_hEnemy);
	float		bt[3], d[3];

	if (en >= 0)
	{
		BodyTarget(en, src, bt);
		VecSub(bt, ent_cv(en)->origin, d);
		VecAdd(d, m->m_vecEnemyLKP, d);
		VecSub(d, src, d);
		VecNorm(d, out);
	}
	else
		VecCopy(gpGlobals.v_forward, out);
}

int Mon_FacingIdeal(int e) { return fabsf(Mon_FlYawDiff(e)) <= 0.006f; }

void CBaseMonster_CorpseFallThink(int e)
{
	if (ent_cv(e)->flags & FL_ONGROUND)
	{
		SetThink(e, FN_NONE);
		gs_set_sequence_box(e);
		ent_set_origin(e, ent_cv(e)->origin);
	}
	else
		ThinkIn(e, 0.1f);
}

void CBaseMonster_MonsterInitDead(int e)
{
	CBaseMonster *m = MON(e);

	Mon_InitBoneControllers(e);
	ent_set_solid(e, SOLID_BBOX);
	ent_set_movetype(e, MOVETYPE_TOSS);
	ent_rnd(e)->frame = 0;
	Mon_ResetSequenceInfo(e);
	ent_rnd(e)->framerate = 0;
	m->max_health = m->health;
	m->deadflag = DEAD_DEAD;
	ent_set_size(e, g_vecZero, g_vecZero);
	ent_set_origin(e, ent_cv(e)->origin);
	Mon_BecomeDead(e);
	SetThink(e, FN_CBaseMonster_CorpseFallThink);
	ThinkIn(e, 0.5f);
}

int Mon_BBoxFlat(int e)
{
	col_trace_t	tr;
	const col_ent_t	*cv = ent_cv(e);
	float		p[3], q[3], fx, fy, l, l2;
	static const int8_t sx[4] = { 1, -1, -1, 1 }, sy[4] = { 1, -1, 1, -1 };
	int		i;

	fx = (cv->maxs[0] - cv->mins[0]) / 2;
	fy = (cv->maxs[1] - cv->mins[1]) / 2;
	l = 0;
	for (i = 0; i < 4; i++)
	{
		p[0] = cv->origin[0] + sx[i] * fx;
		p[1] = cv->origin[1] + sy[i] * fy;
		p[2] = cv->origin[2];
		VecCopy(p, q);
		q[2] -= 100;
		UTIL_TraceLine(p, q, ignore_monsters, e, &tr);
		l2 = mv_dist(p, tr.end);
		if (i > 0 && l2 > l)
			return 0;
		l = l2;
	}
	return 1;
}

int Mon_GetEnemy(int e)
{
	CBaseMonster	*m = MON(e);
	int		ne, owner;

	if (Mon_HasConditions(m, bits_COND_SEE_HATE | bits_COND_SEE_DISLIKE | bits_COND_SEE_NEMESIS))
	{
		ne = Mon_BestVisibleEnemy(e);
		if (ne >= 0 && ne != EHGet(m->m_hEnemy) && m->m_pSchedule)
		{
			if (m->m_pSchedule->iInterruptMask & bits_COND_NEW_ENEMY)
			{
				Mon_PushEnemy(e, EHGet(m->m_hEnemy), m->m_vecEnemyLKP);
				Mon_SetConditions(m, bits_COND_NEW_ENEMY);
				m->m_hEnemy = EHANDLE(ne);
				VecCopy(ent_cv(ne)->origin, m->m_vecEnemyLKP);
			}
			owner = ent_cv(ne)->owner;
			if (owner != COL_NONE && mon_is_monster(owner) && (ent_cv(owner)->flags & FL_MONSTER) &&
				Mon_IRelationshipOf(e, owner) != R_NO)
				Mon_PushEnemy(e, owner, m->m_vecEnemyLKP);
		}
	}
	if (EHGet(m->m_hEnemy) < 0 && Mon_PopEnemy(e))
	{
		if (m->m_pSchedule && (m->m_pSchedule->iInterruptMask & bits_COND_NEW_ENEMY))
			Mon_SetConditions(m, bits_COND_NEW_ENEMY);
	}
	return EHGet(m->m_hEnemy) >= 0;
}

int Mon_DropItem(int e, cls_t cls, const float pos[3], const float ang[3])
{
	int	it;
	float	av[3];

	if (cls == CLS_NONE)
		return -1;
	it = CBaseEntity_Create(cls, pos, ang, e);
	if (it >= 0)
	{
		ent_set_velocity(it, ent_mot(e)->velocity);
		VecSet(av, 0, RANDOM_FLOAT(0, 100), 0);
		ent_set_avelocity(it, av);
	}
	return it;
}

/* ---- monsterstate.cpp -------------------------------------------------------- */

void Mon_SetState(int e, int state)
{
	CBaseMonster *m = MON(e);

	if (state == MONSTERSTATE_IDLE)
		m->m_hEnemy = EH_NONE;
	m->m_MonsterState = state;
	m->m_IdealMonsterState = state;
}

void CBaseMonster_RunAI(int e)
{
	CBaseMonster	*m = MON(e);
	int		en;

	/* HLSDK tests pev->flags & SF_MONSTER_GAG (bit 1 of flags: FL_SWIM) */
	if ((m->m_MonsterState == MONSTERSTATE_IDLE || m->m_MonsterState == MONSTERSTATE_ALERT) &&
		RANDOM_LONG(0, 99) == 0 && !(ent_cv(e)->flags & SF_MONSTER_GAG))
		Mon_IdleSound(e);
	if (m->m_MonsterState != MONSTERSTATE_NONE && m->m_MonsterState != MONSTERSTATE_PRONE &&
		m->m_MonsterState != MONSTERSTATE_DEAD)
	{
		if (ent_client_in_pvs(e) > 0 || m->m_MonsterState == MONSTERSTATE_COMBAT)
		{
			Mon_Look(e, (int)m->m_flDistLook);
			Mon_Listen(e);
			Mon_ClearConditions(m, Mon_IgnoreConditions(e));
			Mon_GetEnemy(e);
		}
		en = EHGet(m->m_hEnemy);
		if (en >= 0)
			Mon_CheckEnemy(e, en);
		else
			m->m_hEnemy = EH_NONE;
		Mon_CheckAmmo(e);
	}
	Mon_FCheckAITrigger(e);
	Mon_PrescheduleThink(e);
	if (!ent_is_live(e))
		return;
	Mon_MaintainSchedule(e);
	if (!ent_is_live(e))
		return;
	m->m_afConditions &= ~(bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE);
}

int CBaseMonster_GetIdealState(int e)
{
	CBaseMonster	*m = MON(e);
	int		c = Mon_IScheduleFlags(e);
	CSound		*s;

	switch (m->m_MonsterState)
	{
	case MONSTERSTATE_IDLE:
		if (c & bits_COND_NEW_ENEMY)
			m->m_IdealMonsterState = MONSTERSTATE_COMBAT;
		else if (c & (bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
		{
			Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
			m->m_IdealMonsterState = MONSTERSTATE_ALERT;
		}
		else if (c & bits_COND_HEAR_SOUND)
		{
			s = Mon_PBestSound(e);
			if (s)
			{
				Mon_MakeIdealYaw(e, s->m_vecOrigin);
				if (s->m_iType & (bits_SOUND_COMBAT | bits_SOUND_DANGER))
					m->m_IdealMonsterState = MONSTERSTATE_ALERT;
			}
		}
		else if (c & (bits_COND_SMELL | bits_COND_SMELL_FOOD))
			m->m_IdealMonsterState = MONSTERSTATE_ALERT;
		break;
	case MONSTERSTATE_ALERT:
		if (c & (bits_COND_NEW_ENEMY | bits_COND_SEE_ENEMY))
			m->m_IdealMonsterState = MONSTERSTATE_COMBAT;
		else if (c & bits_COND_HEAR_SOUND)
		{
			m->m_IdealMonsterState = MONSTERSTATE_ALERT;
			s = Mon_PBestSound(e);
			if (s)
				Mon_MakeIdealYaw(e, s->m_vecOrigin);
		}
		break;
	case MONSTERSTATE_COMBAT:
		if (EHGet(m->m_hEnemy) < 0)
			m->m_IdealMonsterState = MONSTERSTATE_ALERT;
		break;
	case MONSTERSTATE_SCRIPT:
		if (c & (bits_COND_TASK_FAILED | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
			Mon_ExitScriptedSequence(e);
		break;
	case MONSTERSTATE_DEAD:
		m->m_IdealMonsterState = MONSTERSTATE_DEAD;
		break;
	}
	return m->m_IdealMonsterState;
}

/* ---- skill ----------------------------------------------------------------------- */

float mon_skill(const float v[3])
{
	int s = settings.skill;

	if (s < 1)
		s = 1;
	if (s > 3)
		s = 3;
	return v[s - 1];
}

void mv_norm2d(float d[3], const float a[3])
{
	float l = (float)sqrt(a[0] * a[0] + a[1] * a[1]);

	if (l == 0)
	{
		d[0] = d[1] = d[2] = 0;
		return;
	}
	l = 1 / l;
	d[0] = a[0] * l;
	d[1] = a[1] * l;
	d[2] = 0;
}

/* ---- CBaseEntity virtuals of CBaseMonster ------------------------------------- */

const float VEC_HUMAN_HULL_MIN[3] = { -16, -16, 0 };
const float VEC_HUMAN_HULL_MAX[3] = { 16, 16, 72 };

int CBaseMonster_IsAlive(int e) { return MON(e)->deadflag != DEAD_DEAD; }
int CBaseMonster_BloodColor(int e) { return MON(e)->m_bloodColor; }
int CBaseMonster_IsMoving(int e) { return MON(e)->m_movementGoal != MOVEGOAL_NONE; }

CBaseMonster *Mon_SpawnKeys(int e, gid_t16 gid)
{
	CBaseMonster_KeyValue(e, gid);
	return MON(e);
}
