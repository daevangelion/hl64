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
 * trains.c -- HLSDK dlls/pathcorner.cpp CPathTrack (path_track) and
 * plats.cpp CFuncTrackTrain (func_tracktrain). The player's train controls
 * are player.cpp's (player.c); the train's pitch-adjusted engine sound is
 * the client's train event (cl_dll ev_hldm.cpp EV_TrainPitchAdjust) played
 * here.
 *
 * hl64: the CPathTrack pointers (m_pnext, m_pprevious, m_paltpath, the
 * train's m_ppath) are the targetnames of those path_tracks, resolved
 * when used (CPathTrack::Instance of the first entity of that name, a
 * pending record woken): records hydrate near the player and come back
 * as new entities, so no entity reference outlives a release. A path is
 * named by its first holder (FIND_ENTITY_BY_TARGETNAME); maps give each
 * path_track its own name. Link wakes nothing: a path_track that
 * hydrates takes its m_pprevious from the live path_tracks linking to it
 * (the GoldSrc map-start order: the one latest in the map wins), and the
 * path_tracks of a map stay while one of its track trains moves
 * (CPathTrack_quiet). CFuncTrackTrain::NearestPath / OverrideReset
 * (GoldSrc level transitions), SetTrack / SetControls (func_trackchange,
 * func_traincontrols) are not ported: no Uplink / Hazard Course map has
 * those classes.
 */
#include "game/hl/util.h"
#include "game/hl/trains.h"

/* Spawnflag for CPathTrack */
#define SF_PATH_DISABLED	0x00000001
#define SF_PATH_FIREONCE	0x00000002
#define SF_PATH_ALTREVERSE	0x00000004
#define SF_PATH_DISABLE_TRAIN	0x00000008
#define SF_PATH_ALTERNATE	0x00008000

#define TRAIN_STARTPITCH	60
#define TRAIN_MAXPITCH		200
#define TRAIN_MAXSPEED		1000	/* approx max speed for sound pitch calculation */

typedef struct {
	CBASEENTITY_FIELDS
	float		m_length;
	name_t		m_altName;
	name_t		m_pnext, m_pprevious, m_paltpath;	/* targetnames (above) */
	uint16_t	pad;
} CPathTrack;

typedef struct {
	CBASEENTITY_FIELDS
	name_t		m_ppath;		/* targetname (above) */
	uint16_t	m_sentSound;		/* hl64: the last train event's pitch |
						   volume << 6 bits (UpdateSound; not saved) */
	float		m_length;
	float		m_height;
	float		m_speed;
	float		m_dir;
	float		m_startSpeed;
	float		m_controlMins[3];
	float		m_controlMaxs[3];
	int32_t		m_soundPlaying;
	int32_t		m_sounds;
	float		m_flVolume;
	float		m_flBank;
	float		m_oldSpeed;
	int32_t		impulse;		/* pev->impulse */
} CFuncTrackTrain;

static CPathTrack *Path(int e) { return PRIV(CPathTrack, e); }
static CFuncTrackTrain *Train(int e) { return PRIV(CFuncTrackTrain, e); }

/* Vector::Normalize (a zero vector gives (0, 0, 1)) */
static void Normalize(const float v[3], float o[3])
{
	float l = VecLen(v);

	if (l == 0)
	{
		VecSet(o, 0, 0, 1);
		return;
	}
	VecScale(v, 1 / l, o);
}

/* ---- path_track (pathcorner.cpp CPathTrack) ------------------------------- */

/* CPathTrack::Instance(FIND_ENTITY_BY_TARGETNAME(NULL, n)): -1 when the
 * first entity of the name is no path_track */
static int PathInstance(name_t n)
{
	int e = UTIL_FindByTargetname1(n);

	return e >= 0 && ent_idc(e)->cls == CLS_PATH_TRACK ? e : -1;
}

/* the same test on the records of the name, waking none (Link) */
static int NamesPath(name_t n)
{
	int e = -1, it = 0;

	if (n == NAME_NONE || !rec_name_count(n))
		return 0;
	if (rec_entity(rec_name_gid(n, 0)) < 0)
		return rec_class(rec_name_gid(n, 0)) == CLS_PATH_TRACK;
	e = UTIL_FindEntityByTargetname(n, &it);
	return e >= 0 && ent_idc(e)->cls == CLS_PATH_TRACK;
}

/* lump order of the first record of name n (GoldSrc's edict order) */
static int NameLump(name_t n)
{
	return n != NAME_NONE && rec_name_count(n) ? (int)GID_LUMP(rec_name_gid(n, 0)) : -1;
}

void CPathTrack_Use(int e, int activator, int caller, int use_type, float value)
{
	CPathTrack *p = Path(e);
	int on;

	(void)activator;
	(void)caller;
	(void)value;
	/* use toggles between two paths */
	if (p->m_paltpath != NAME_NONE)
	{
		on = !(p->spawnflags & SF_PATH_ALTERNATE);
		if (ShouldToggle(use_type, on))
		{
			if (on)
				p->spawnflags |= SF_PATH_ALTERNATE;
			else
				p->spawnflags &= ~SF_PATH_ALTERNATE;
		}
	}
	else	/* use toggles between enabled/disabled */
	{
		on = !(p->spawnflags & SF_PATH_DISABLED);
		if (ShouldToggle(use_type, on))
		{
			if (on)
				p->spawnflags |= SF_PATH_DISABLED;
			else
				p->spawnflags &= ~SF_PATH_DISABLED;
		}
	}
}

/* CPathTrack::SetPrevious: only set previous if this isn't my alternate
 * path; of several, the one latest in the map (GoldSrc links them in
 * edict order at map start) */
static void SetPrevious(int e, name_t prev)
{
	CPathTrack *p = Path(e);

	if (prev != NAME_NONE && prev != p->m_altName
		&& (p->m_pprevious == NAME_NONE || NameLump(prev) >= NameLump(p->m_pprevious)))
		p->m_pprevious = prev;
}

static void CPathTrack_Link(int e)
{
	CPathTrack *p = Path(e);
	name_t self = ent_idc(e)->targetname;
	int t;

	if (p->target != NAME_NONE)
	{
		if (rec_name_count(p->target) || UTIL_FindByTargetname1(p->target) >= 0)
		{
			p->m_pnext = NamesPath(p->target) ? p->target : NAME_NONE;
			/* if no next pointer, this is the end of a path */
			if (p->m_pnext != NAME_NONE)
			{
				int it = 0;

				t = rec_entity(rec_name_gid(p->target, 0)) >= 0
					? UTIL_FindEntityByTargetname(p->target, &it) : -1;
				if (t >= 0)
					SetPrevious(t, self);
			}
		}
		else
			log_printf("game: path_track %d: dead end link\n", e);
	}
	/* find "alternate" path */
	if (p->m_altName != NAME_NONE && NamesPath(p->m_altName))
	{
		int it = 0;

		p->m_paltpath = p->m_altName;
		t = rec_entity(rec_name_gid(p->m_altName, 0)) >= 0
			? UTIL_FindEntityByTargetname(p->m_altName, &it) : -1;
		if (t >= 0)
			SetPrevious(t, self);
	}
}

/* CPathTrack::Activate: link to next, and back-link; then the back links
 * the live path_tracks made while this one was a pending record */
static void CPathTrack_Activate(int e)
{
	name_t self = ent_idc(e)->targetname;
	int o = -1;

	if (self == NAME_NONE)
		return;
	CPathTrack_Link(e);
	while ((o = ent_next_class(o, CLS_PATH_TRACK, ent_idc(e)->layer)) >= 0)
	{
		const CPathTrack *q = Path(o);

		if (o != e && ent_idc(o)->targetname != NAME_NONE
			&& (q->m_pnext == self || q->m_paltpath == self))
			SetPrevious(e, ent_idc(o)->targetname);
	}
}

static int CPathTrack_Spawn(int e, gid_t16 gid)
{
	CPathTrack *p = Path(e);
	static const float mn[3] = { -8, -8, -8 }, mx[3] = { 8, 8, 8 };

	p->m_altName = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_ALTPATH);
	/* message: the name a passing train fires */
	p->message = gid == GID_NONE ? NAME_NONE : rec_key_name(gid, K_MESSAGE);
	p->m_length = 0;
	ent_set_solid(e, SOLID_TRIGGER);
	UTIL_SetSize(e, mn, mx);
	p->m_pnext = NAME_NONE;
	p->m_pprevious = NAME_NONE;
	p->m_paltpath = NAME_NONE;
	SetUse(e, FN_CPathTrack_Use);
	return 0;
}

/* returns ppath if enabled, -1 otherwise */
static int ValidPath(int ppath, int testFlag)
{
	if (ppath < 0)
		return -1;
	if (testFlag && (Path(ppath)->spawnflags & SF_PATH_DISABLED))
		return -1;
	return ppath;
}

static void Project(int pstart, int pend, float origin[3], float dist)
{
	float dir[3];

	if (pstart >= 0 && pend >= 0)
	{
		VecSub(ent_cv(pend)->origin, ent_cv(pstart)->origin, dir);
		Normalize(dir, dir);
		VecMA(ent_cv(pend)->origin, dist, dir, origin);
	}
}

static int GetNext(int e)
{
	const CPathTrack *p = Path(e);

	if (p->m_paltpath != NAME_NONE && (p->spawnflags & SF_PATH_ALTERNATE)
		&& !(p->spawnflags & SF_PATH_ALTREVERSE))
		return PathInstance(p->m_paltpath);
	return PathInstance(p->m_pnext);
}

static int GetPrevious(int e)
{
	const CPathTrack *p = Path(e);

	if (p->m_paltpath != NAME_NONE && (p->spawnflags & SF_PATH_ALTERNATE)
		&& (p->spawnflags & SF_PATH_ALTREVERSE))
		return PathInstance(p->m_paltpath);
	return PathInstance(p->m_pprevious);
}

/* CPathTrack::LookAhead (assumes this is ALWAYS enabled): -1 = NULL */
static int LookAhead(int self, float origin[3], float dist, int move)
{
	float originalDist = dist, currentPos[3], dir[3], length;
	int pcurrent = self, next;

	VecCopy(origin, currentPos);
	if (dist < 0)	/* travelling backwards through path */
	{
		dist = -dist;
		while (dist > 0)
		{
			VecSub(ent_cv(pcurrent)->origin, currentPos, dir);
			length = VecLen(dir);
			if (!length)
			{
				/* no previous node, or it's disabled: return now */
				if (ValidPath(GetPrevious(pcurrent), move) < 0)
				{
					if (!move)
						Project(GetNext(pcurrent), pcurrent, origin, dist);
					return -1;
				}
				pcurrent = GetPrevious(pcurrent);
			}
			else if (length > dist)	/* enough left in this path to move */
			{
				VecMA(currentPos, dist / length, dir, origin);
				return pcurrent;
			}
			else
			{
				dist -= length;
				VecCopy(ent_cv(pcurrent)->origin, currentPos);
				VecCopy(currentPos, origin);
				if (ValidPath(GetPrevious(pcurrent), move) < 0)
					return -1;
				pcurrent = GetPrevious(pcurrent);
			}
		}
		VecCopy(currentPos, origin);
		return pcurrent;
	}
	while (dist > 0)
	{
		next = GetNext(pcurrent);
		/* no next node, or it's disabled: return now */
		if (ValidPath(next, move) < 0)
		{
			if (!move)
				Project(GetPrevious(pcurrent), pcurrent, origin, dist);
			return -1;
		}
		VecSub(ent_cv(next)->origin, currentPos, dir);
		length = VecLen(dir);
		if (!length && ValidPath(GetNext(next), move) < 0)
		{
			if (dist == originalDist)	/* HACK -- up against a dead end */
				return -1;
			return pcurrent;
		}
		if (length > dist)	/* enough left in this path to move */
		{
			VecMA(currentPos, dist / length, dir, origin);
			return pcurrent;
		}
		dist -= length;
		VecCopy(ent_cv(next)->origin, currentPos);
		pcurrent = next;
		VecCopy(currentPos, origin);
	}
	VecCopy(currentPos, origin);
	return pcurrent;
}

/* a path_track stays while a track train of its map moves: the train
 * holds it by name, and its Next (every frame) would wake it again */
static int CPathTrack_quiet(int e)
{
	int t = -1;

	while ((t = ent_next_class(t, CLS_FUNC_TRACKTRAIN, ent_idc(e)->layer)) >= 0)
		if (Train(t)->speed != 0)
			return 0;
	return 1;
}

static int CPathTrack_ObjectCaps(int e)
{
	(void)e;
	return 0;	/* CPointEntity: CBaseEntity::ObjectCaps() & ~FCAP_ACROSS_TRANSITION */
}

static const game_vt_t CPathTrack_vt = {
	.ObjectCaps = CPathTrack_ObjectCaps,
	.Activate = CPathTrack_Activate,
};

/* HLSDK CPathTrack::m_SaveData (the pointers as names) */
static const sv_field_t f_CPathTrack[] = {
	SVF(CPathTrack, m_length, SV_F32, 0),
	SVF(CPathTrack, m_pnext, SV_NAME, 0),
	SVF(CPathTrack, m_paltpath, SV_NAME, 0),
	SVF(CPathTrack, m_pprevious, SV_NAME, 0),
	SVF(CPathTrack, m_altName, SV_NAME, SVK),
};
static const sv_table_t sv_CPathTrack = SV_TABLE(&sv_CBaseEntity, f_CPathTrack,
	.flags = SVT_MSG_NAME, .quiet = CPathTrack_quiet);

const game_class_t gc_PATH_TRACK = {
	.size = sizeof(CPathTrack),
	.Spawn = CPathTrack_Spawn,
	.vt = &CPathTrack_vt,
	.save = &sv_CPathTrack,
};

/* ---- func_tracktrain (plats.cpp CFuncTrackTrain) ---------------------------- */

static void NextThink(int e, float thinkTime, int alwaysThink)
{
	if (alwaysThink)
		EFlagsSet(e, FL_ALWAYSTHINK);
	else
		EFlagsClear(e, FL_ALWAYSTHINK);
	ent_think_at(e, thinkTime);
}

void CFuncTrackTrain_Blocked(int e, int other)
{
	CFuncTrackTrain *p = Train(e);
	float v[3];

	/* blocker is on-ground on the train */
	if ((EFlags(other) & FL_ONGROUND) && ent_phys(other)->groundentity == e)
	{
		float deltaSpeed = fabsf(p->speed);

		if (deltaSpeed > 50)
			deltaSpeed = 50;
		VecCopy(ent_mot(other)->velocity, v);
		if (!v[2])
		{
			v[2] += deltaSpeed;
			ent_set_velocity(other, v);
		}
		return;
	}
	OriginIn(other, e, v);
	VecSub(v, ent_cv(e)->origin, v);
	Normalize(v, v);
	VecScale(v, p->dmg, v);
	ent_set_velocity(other, v);
	if (p->dmg <= 0)
		return;
	/* we can't hurt this thing, so we're not concerned with it */
	TakeDamage(other, e, e, p->dmg, DMG_CRUSH);
}

static void StopSound(int e)
{
	CFuncTrackTrain *p = Train(e);

	/* if sound playing, stop it (the client's train event, stop) */
	if (p->m_soundPlaying && p->noise != RES_NONE)
	{
		STOP_SOUND(e, CHAN_STATIC, p->noise);
		EMIT_SOUND_DYN(e, CHAN_ITEM, RESID(TTRAIN_BRAKE1), p->m_flVolume, ATTN_NORM, 0, 100);
	}
	p->m_soundPlaying = 0;
}

/* update pitch based on speed, start sound if not playing. NOTE: when
 * train goes through transition, m_soundPlaying should go to 0, which
 * will cause the looped sound to restart. */
static void UpdateSound(int e)
{
	CFuncTrackTrain *p = Train(e);
	float flpitch;

	if (p->noise == RES_NONE)
		return;
	flpitch = TRAIN_STARTPITCH + (fabsf(p->speed) * (TRAIN_MAXPITCH - TRAIN_STARTPITCH)
		/ TRAIN_MAXSPEED);
	if (!p->m_soundPlaying)
	{
		/* play startup sound for train */
		EMIT_SOUND_DYN(e, CHAN_ITEM, RESID(TTRAIN_START1), p->m_flVolume, ATTN_NORM, 0, 100);
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise, p->m_flVolume, ATTN_NORM, 0, (int)flpitch);
		p->m_soundPlaying = 1;
		p->m_sentSound = 0xFFFF;
	}
	else
	{
		/* the client's train event: pitch in 6 bits of tens, volume in
		 * 6 bits of 1/40. GoldSrc sends it every Next (every frame while
		 * moving); hl64 changes the loop only when the bits differ from
		 * the last event's (the same bits set the same pitch / volume) */
		int pbits = (int)(flpitch / 10.0f) & 0x3f;
		int vbits = (int)(p->m_flVolume * 40.0f) & 0x3f;
		uint16_t sent = (uint16_t)(pbits | vbits << 6);

		if (sent == p->m_sentSound)
			return;
		p->m_sentSound = sent;
		EMIT_SOUND_DYN(e, CHAN_STATIC, p->noise, (float)vbits / 40.0f, ATTN_NORM,
			SND_CHANGE_PITCH, 10 * pbits);
	}
}

void CFuncTrackTrain_Use(int e, int activator, int caller, int use_type, float value)
{
	CFuncTrackTrain *p = Train(e);

	(void)activator;
	(void)caller;
	if (use_type != USE_SET)
	{
		if (!ShouldToggle(use_type, p->speed != 0))
			return;
		if (p->speed == 0)
		{
			p->speed = p->m_speed * p->m_dir;
			CFuncTrackTrain_Next(e);
		}
		else
		{
			p->speed = 0;
			ent_set_velocity(e, g_vecZero);
			ent_set_avelocity(e, g_vecZero);
			StopSound(e);
			SetThink(e, FN_NONE);
		}
	}
	else
	{
		float delta = value;

		delta = ((int)(p->speed * 4) / (int)p->m_speed) * 0.25f + 0.25f * delta;
		if (delta > 1)
			delta = 1;
		else if (delta < -1)
			delta = -1;
		if ((p->spawnflags & SF_TRACKTRAIN_FORWARDONLY) && delta < 0)
			delta = 0;
		p->speed = p->m_speed * delta;
		CFuncTrackTrain_Next(e);
		log_printf("game: train %d speed %.2f lt=%.2f\n", e, (double)p->speed,
			(double)ent_tm(e)->ltime);
	}
}

static float Fix(float angle)
{
	while (angle < 0)
		angle += 360;
	while (angle > 360)
		angle -= 360;
	return angle;
}

static void FixupAngles(float v[3])
{
	v[0] = Fix(v[0]);
	v[1] = Fix(v[1]);
	v[2] = Fix(v[2]);
}

void CFuncTrackTrain_Next(int e)
{
	CFuncTrackTrain *p = Train(e);
	col_ent_t *cv = ent_cv(e);
	float time = 0.5f, nextPos[3], nextFront[3], delta[3], angles[3], v[3], av[3];
	float vx, vy;
	int path, pnext;

	if (!p->speed)
	{
		StopSound(e);
		return;
	}
	path = PathInstance(p->m_ppath);
	if (path < 0)
	{
		log_printf("game: train %d: lost path\n", e);
		StopSound(e);
		return;
	}
	UpdateSound(e);
	VecCopy(cv->origin, nextPos);
	nextPos[2] -= p->m_height;
	pnext = LookAhead(path, nextPos, p->speed * 0.1f, 1);
	nextPos[2] += p->m_height;
	VecSub(nextPos, cv->origin, v);
	VecScale(v, 10, v);
	ent_set_velocity(e, v);
	VecCopy(cv->origin, nextFront);
	nextFront[2] -= p->m_height;
	LookAhead(path, nextFront, p->m_length > 0 ? p->m_length : 100, 0);
	nextFront[2] += p->m_height;
	VecSub(nextFront, cv->origin, delta);
	UTIL_VecToAngles(delta, angles);
	/* the train actually points west */
	angles[1] += 180;
	/* !!! All of this crap has to be done to make the angles not wrap
	 * around, revisit this. */
	FixupAngles(angles);
	FixupAngles(cv->angles);
	if (pnext < 0 || (delta[0] == 0 && delta[1] == 0))
		VecCopy(cv->angles, angles);
	vx = (p->spawnflags & SF_TRACKTRAIN_NOPITCH) ? 0 : UTIL_AngleDistance(angles[0], cv->angles[0]);
	vy = UTIL_AngleDistance(angles[1], cv->angles[1]);
	VecCopy(ent_mot(e)->avelocity, av);
	av[1] = vy * 10;
	av[0] = vx * 10;
	if (p->m_flBank != 0)
	{
		if (av[1] < -5)
			av[2] = UTIL_AngleDistance(UTIL_ApproachAngle(-p->m_flBank, cv->angles[2],
				p->m_flBank * 2), cv->angles[2]);
		else if (av[1] > 5)
			av[2] = UTIL_AngleDistance(UTIL_ApproachAngle(p->m_flBank, cv->angles[2],
				p->m_flBank * 2), cv->angles[2]);
		else
			av[2] = UTIL_AngleDistance(UTIL_ApproachAngle(0, cv->angles[2],
				p->m_flBank * 4), cv->angles[2]) * 4;
	}
	ent_set_avelocity(e, av);
	if (pnext >= 0)
	{
		if (pnext != path)
		{
			int pFire = p->speed >= 0 ? pnext : path;
			CPathTrack *f = Path(pFire);

			p->m_ppath = ent_idc(pnext)->targetname;
			/* fire the pass target if there is one */
			if (f->message != NAME_NONE)
			{
				FireTargets(f->message, e, e, USE_TOGGLE, 0);
				if (f->spawnflags & SF_PATH_FIREONCE)
					f->message = NAME_NONE;
			}
			if (f->spawnflags & SF_PATH_DISABLE_TRAIN)
				p->spawnflags |= SF_TRACKTRAIN_NOCONTROL;
			/* don't override speed if under user control */
			if ((p->spawnflags & SF_TRACKTRAIN_NOCONTROL) && f->speed != 0)
				p->speed = f->speed;	/* don't copy speed from target if it is 0 */
		}
		SetThink(e, FN_CFuncTrackTrain_Next);
		NextThink(e, ent_tm(e)->ltime + time, 1);
	}
	else	/* end of path, stop */
	{
		float distance;

		StopSound(e);
		VecSub(nextPos, cv->origin, v);
		ent_set_avelocity(e, g_vecZero);
		distance = VecLen(v);
		p->m_oldSpeed = p->speed;
		p->speed = 0;
		/* move to the dead end; are we there yet? */
		if (distance > 0)
		{
			/* no, how long to get there? */
			time = distance / p->m_oldSpeed;
			VecScale(v, p->m_oldSpeed / distance, v);
			ent_set_velocity(e, v);
			SetThink(e, FN_CFuncTrackTrain_DeadEnd);
			NextThink(e, ent_tm(e)->ltime + time, 0);
		}
		else
		{
			ent_set_velocity(e, v);
			CFuncTrackTrain_DeadEnd(e);
		}
	}
}

void CFuncTrackTrain_DeadEnd(int e)
{
	CFuncTrackTrain *p = Train(e);
	int pTrack = PathInstance(p->m_ppath), pNext;

	/* find the dead end path node. HACKHACK -- This is bugly, but the
	 * train can actually stop moving at a different node depending on
	 * its speed so we have to traverse the list to its end. */
	if (pTrack >= 0)
	{
		if (p->m_oldSpeed < 0)
		{
			while ((pNext = ValidPath(GetPrevious(pTrack), 1)) >= 0)
				pTrack = pNext;
		}
		else
		{
			while ((pNext = ValidPath(GetNext(pTrack), 1)) >= 0)
				pTrack = pNext;
		}
	}
	ent_set_velocity(e, g_vecZero);
	ent_set_avelocity(e, g_vecZero);
	log_printf("game: train %d dead end lt=%.2f\n", e, (double)ent_tm(e)->ltime);
	if (pTrack >= 0 && CBase(pTrack)->netname != NAME_NONE)
		FireTargets(CBase(pTrack)->netname, e, e, USE_TOGGLE, 0);
}

int TrackTrain_OnControls(int train, const float pos[3])
{
	const CFuncTrackTrain *p;
	float offset[3], local[3];

	if (train < 0 || ent_idc(train)->cls != CLS_FUNC_TRACKTRAIN)
		return 0;
	p = Train(train);
	if (p->spawnflags & SF_TRACKTRAIN_NOCONTROL)
		return 0;
	VecSub(pos, ent_cv(train)->origin, offset);
	/* transform offset into local coordinates */
	UTIL_MakeVectors(ent_cv(train)->angles);
	local[0] = VecDot(offset, gpGlobals.v_forward);
	local[1] = -VecDot(offset, gpGlobals.v_right);
	local[2] = VecDot(offset, gpGlobals.v_up);
	return local[0] >= p->m_controlMins[0] && local[1] >= p->m_controlMins[1]
		&& local[2] >= p->m_controlMins[2] && local[0] <= p->m_controlMaxs[0]
		&& local[1] <= p->m_controlMaxs[1] && local[2] <= p->m_controlMaxs[2];
}

int TrackTrain_Impulse(int e)
{
	return e >= 0 && ent_idc(e)->cls == CLS_FUNC_TRACKTRAIN ? Train(e)->impulse : 0;
}

void CFuncTrackTrain_Find(int e)
{
	CFuncTrackTrain *p = Train(e);
	float nextPos[3], look[3], ang[3];
	int path = PathInstance(p->target);

	p->m_ppath = path >= 0 ? p->target : NAME_NONE;
	if (path < 0)
		return;
	VecCopy(ent_cv(path)->origin, nextPos);
	nextPos[2] += p->m_height;
	VecCopy(nextPos, look);
	look[2] -= p->m_height;
	LookAhead(path, look, p->m_length, 0);
	look[2] += p->m_height;
	VecSub(look, nextPos, look);
	UTIL_VecToAngles(look, ang);
	/* the train actually points west */
	ang[1] += 180;
	if (p->spawnflags & SF_TRACKTRAIN_NOPITCH)
		ang[0] = 0;
	VecCopy(ang, ent_cv(e)->angles);
	UTIL_SetOrigin(e, nextPos);
	log_printf("game: train %d on path %d: %.0f %.0f %.0f yaw %.0f\n", e, path,
		(double)nextPos[0], (double)nextPos[1], (double)nextPos[2], (double)ang[1]);
	NextThink(e, ent_tm(e)->ltime + 0.1f, 0);
	SetThink(e, FN_CFuncTrackTrain_Next);
	p->speed = p->m_startSpeed;
	UpdateSound(e);
}

static void CFuncTrackTrain_Precache(int e)
{
	CFuncTrackTrain *p = Train(e);

	if (p->m_flVolume == 0)
		p->m_flVolume = 1;
	switch (p->m_sounds)
	{
	default: p->noise = RES_NONE; break;	/* no sound */
	case 1: p->noise = RESID(TTRAIN1); break;
	case 2: p->noise = RESID(TTRAIN2); break;
	case 3: p->noise = RESID(TTRAIN3); break;
	case 4: p->noise = RESID(TTRAIN4); break;
	case 5: p->noise = RESID(TTRAIN6); break;
	case 6: p->noise = RESID(TTRAIN7); break;
	}
	PRECACHE(p->noise);
	PRECACHE(RESID(TTRAIN_BRAKE1));
	PRECACHE(RESID(TTRAIN_START1));
}

static int CFuncTrackTrain_Spawn(int e, gid_t16 gid)
{
	CFuncTrackTrain *p = Train(e);
	col_ent_t *cv = ent_cv(e);

	/* CFuncTrackTrain::KeyValue */
	p->m_length = KeyF(gid, K_WHEELS, 0);
	p->m_height = KeyF(gid, K_HEIGHT, 0);
	p->m_startSpeed = KeyF(gid, K_STARTSPEED, 0);
	p->m_sounds = (int32_t)KeyF(gid, K_SOUNDS, 0);
	p->m_flVolume = (float)(int)KeyF(gid, K_VOLUME, 0) * 0.1f;
	p->m_flBank = KeyF(gid, K_BANK, 0);
	p->m_ppath = NAME_NONE;
	p->m_speed = p->speed == 0 ? 100 : p->speed;
	p->speed = 0;
	ent_set_velocity(e, g_vecZero);
	ent_set_avelocity(e, g_vecZero);
	p->impulse = (int32_t)p->m_speed;
	p->m_dir = 1;
	if (p->target == NAME_NONE)
		log_printf("game: FuncTrain with no target\n");
	ent_set_movetype(e, MOVETYPE_PUSH);
	ent_set_solid(e, (p->spawnflags & SF_TRACKTRAIN_PASSABLE) ? SOLID_NOT : SOLID_BSP);
	ent_set_model(e, p->model);
	UTIL_SetOrigin(e, cv->origin);
	VecCopy(cv->mins, p->m_controlMins);
	VecCopy(cv->maxs, p->m_controlMaxs);
	p->m_controlMaxs[2] += 72;
	/* start trains on the next frame, to make sure their targets have
	 * had a chance to spawn/activate */
	NextThink(e, ent_tm(e)->ltime + 0.1f, 0);
	SetThink(e, FN_CFuncTrackTrain_Find);
	CFuncTrackTrain_Precache(e);
	SetUse(e, FN_CFuncTrackTrain_Use);
	SetBlocked(e, FN_CFuncTrackTrain_Blocked);
	return 0;
}

static int CFuncTrackTrain_ObjectCaps(int e)
{
	(void)e;
	return FCAP_DIRECTIONAL_USE;	/* (CBaseEntity & ~FCAP_ACROSS_TRANSITION) | ... */
}

/* m_soundPlaying is not saved: a restored train restarts its sound */
static void CFuncTrackTrain_restored(int e)
{
	Train(e)->m_soundPlaying = 0;
}

static const game_vt_t CFuncTrackTrain_vt = {
	.ObjectCaps = CFuncTrackTrain_ObjectCaps,
};

/* HLSDK CFuncTrackTrain::m_SaveData (+ pev->impulse) */
static const sv_field_t f_CFuncTrackTrain[] = {
	SVF(CFuncTrackTrain, m_ppath, SV_NAME, 0),
	SVF(CFuncTrackTrain, m_length, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_height, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_speed, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_dir, SV_F32, 0),
	SVF(CFuncTrackTrain, m_startSpeed, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_controlMins, SV_VEC, SVK),
	SVF(CFuncTrackTrain, m_controlMaxs, SV_VEC, SVK),
	SVF(CFuncTrackTrain, m_sounds, SV_I32, SVK),
	SVF(CFuncTrackTrain, m_flVolume, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_flBank, SV_F32, SVK),
	SVF(CFuncTrackTrain, m_oldSpeed, SV_F32, 0),
	SVF(CFuncTrackTrain, impulse, SV_I32, SVK),
};
static const sv_table_t sv_CFuncTrackTrain = SV_TABLE(&sv_CBaseEntity, f_CFuncTrackTrain,
	.restored = CFuncTrackTrain_restored);

const game_class_t gc_FUNC_TRACKTRAIN = {
	.size = sizeof(CFuncTrackTrain),
	.Spawn = CFuncTrackTrain_Spawn,
	.vt = &CFuncTrackTrain_vt,
	.save = &sv_CFuncTrackTrain,
};
