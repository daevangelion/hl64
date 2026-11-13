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
 * ambient.c -- HLSDK dlls/sound.cpp CAmbientGeneric (ambient_generic: the
 * dynamic pitch / volume envelope and LFO) and CSpeaker (speaker). The
 * sound is the cooked res_t of the `message` key (SOUND, or SENTENCE for
 * "!NAME"); EMIT_AMBIENT_SOUND plays on the entity's CHAN_STATIC at its
 * fixed origin (snd_emit / snd_sentence), so stop and change requests
 * reach the same sound. Retargets the old tree's hl_ambient.c (audit entry
 * source/hl/hl_ambient.c: HLSDK + QUAKE-INTERFACE) by rewriting it from
 * sound.cpp: no static-sound shortcut, no Quake channel.
 */
#include <stdlib.h>

#include "game/hl/util.h"

#define AMBIENT_SOUND_STATIC		0
#define AMBIENT_SOUND_EVERYWHERE	1
#define AMBIENT_SOUND_SMALLRADIUS	2
#define AMBIENT_SOUND_MEDIUMRADIUS	4
#define AMBIENT_SOUND_LARGERADIUS	8
#define AMBIENT_SOUND_START_SILENT	16
#define AMBIENT_SOUND_NOT_LOOPING	32
#define SPEAKER_START_SILENT		1

#define LFO_SQUARE			1
#define LFO_TRIANGLE			2
#define LFO_RANDOM			3

typedef struct {
	int	preset;
	int	pitchrun, pitchstart, spinup, spindown, volrun, volstart, fadein, fadeout;
	int	lfotype, lforate, lfomodpitch, lfomodvol, cspinup;
	int	cspincount, pitch, spinupsav, spindownsav, pitchfrac, vol, fadeinsav, fadeoutsav;
	int	volfrac, lfofrac, lfomult;
} dynpitchvol_t;

#define CDPVPRESETMAX 27
static const dynpitchvol_t rgdpvpreset[CDPVPRESETMAX] = {
	{1, 255, 75, 95, 95, 10, 1, 50, 95, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{2, 255, 85, 70, 88, 10, 1, 20, 88, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{3, 255, 100, 50, 75, 10, 1, 10, 75, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{4, 100, 100, 0, 0, 10, 1, 90, 90, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{5, 100, 100, 0, 0, 10, 1, 80, 80, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{6, 100, 100, 0, 0, 10, 1, 50, 70, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{7, 100, 100, 0, 0, 5, 1, 40, 50, 1, 50, 0, 10, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{8, 100, 100, 0, 0, 5, 1, 40, 50, 1, 150, 0, 10, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{9, 100, 100, 0, 0, 5, 1, 40, 50, 1, 750, 0, 10, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{10, 128, 100, 50, 75, 10, 1, 30, 40, 2, 8, 20, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{11, 128, 100, 50, 75, 10, 1, 30, 40, 2, 25, 20, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{12, 128, 100, 50, 75, 10, 1, 30, 40, 2, 70, 20, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{13, 50, 50, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{14, 70, 70, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{15, 90, 90, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{16, 120, 120, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{17, 180, 180, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{18, 255, 255, 0, 0, 10, 1, 20, 50, 0, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{19, 200, 75, 90, 90, 10, 1, 50, 90, 2, 100, 20, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{20, 255, 75, 97, 90, 10, 1, 50, 90, 1, 40, 50, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{21, 100, 100, 0, 0, 10, 1, 30, 50, 3, 15, 20, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{22, 160, 160, 0, 0, 10, 1, 50, 50, 3, 500, 25, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{23, 255, 75, 88, 0, 10, 1, 40, 0, 0, 0, 0, 0, 5, 0,0,0,0,0,0,0,0,0,0,0},
	{24, 200, 20, 95, 70, 10, 1, 70, 70, 3, 20, 50, 0, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{25, 180, 100, 50, 60, 10, 1, 40, 60, 2, 90, 100, 100, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{26, 60, 60, 0, 0, 10, 1, 40, 70, 3, 80, 20, 50, 0, 0,0,0,0,0,0,0,0,0,0,0},
	{27, 128, 90, 10, 10, 10, 1, 20, 40, 1, 5, 10, 20, 0, 0,0,0,0,0,0,0,0,0,0,0},
};

typedef struct {
	CBASEENTITY_FIELDS
	float		m_flAttenuation;
	dynpitchvol_t	m_dpv;
	uint8_t		m_fActive;
	uint8_t		m_fLooping;
	uint8_t		pad[2];
} CAmbientGeneric;

/* UTIL_EmitAmbientSound(ENT(pev), pev->origin, message, ...) */
static void ambient_emit(int e, float vol, float attn, int flags, int pitch)
{
	res_t s = CBase(e)->message;

	if (RES_KIND(s) == RES_K_SENTENCE)
		snd_sentence(e, CHAN_STATIC, (uint16_t)RES_INDEX(s), vol, attn, flags, pitch);
	else
		snd_emit(e, CHAN_STATIC, s, vol, attn, flags, pitch);
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void InitModulationParms(int e)
{
	CAmbientGeneric *p = PRIV(CAmbientGeneric, e);
	dynpitchvol_t *d = &p->m_dpv;
	int pitchinc;

	d->volrun = (int)(p->health * 10);
	if (d->volrun > 100)
		d->volrun = 100;
	if (d->volrun < 0)
		d->volrun = 0;
	if (d->preset != 0 && d->preset <= CDPVPRESETMAX)
	{
		*d = rgdpvpreset[d->preset - 1];
		if (d->spindown > 0)
			d->spindown = (101 - d->spindown) * 64;
		if (d->spinup > 0)
			d->spinup = (101 - d->spinup) * 64;
		d->volstart *= 10;
		d->volrun *= 10;
		if (d->fadein > 0)
			d->fadein = (101 - d->fadein) * 64;
		if (d->fadeout > 0)
			d->fadeout = (101 - d->fadeout) * 64;
		d->lforate *= 256;
		d->fadeinsav = d->fadein;
		d->fadeoutsav = d->fadeout;
		d->spinupsav = d->spinup;
		d->spindownsav = d->spindown;
	}
	d->fadein = d->fadeinsav;
	d->fadeout = 0;
	d->vol = d->fadein ? d->volstart : d->volrun;
	d->spinup = d->spinupsav;
	d->spindown = 0;
	d->pitch = d->spinup ? d->pitchstart : d->pitchrun;
	if (d->pitch == 0)
		d->pitch = PITCH_NORM;
	d->pitchfrac = d->pitch << 8;
	d->volfrac = d->vol << 8;
	d->lfofrac = 0;
	d->lforate = abs(d->lforate);
	d->cspincount = 1;
	if (d->cspinup)
	{
		pitchinc = (255 - d->pitchstart) / d->cspinup;
		d->pitchrun = d->pitchstart + pitchinc;
		if (d->pitchrun > 255)
			d->pitchrun = 255;
	}
	if ((d->spinupsav || d->spindownsav || (d->lfotype && d->lfomodpitch)) &&
	    d->pitch == PITCH_NORM)
		d->pitch = PITCH_NORM + 1;
}

void CAmbientGeneric_RampThink(int e)
{
	CAmbientGeneric *p = PRIV(CAmbientGeneric, e);
	dynpitchvol_t *d = &p->m_dpv;
	int pitch = d->pitch, vol = d->vol, flags = 0, changed = 0, prev, pos;

	if (!d->spinup && !d->spindown && !d->fadein && !d->fadeout && !d->lfotype)
		return;
	if (d->spinup || d->spindown)
	{
		prev = d->pitchfrac >> 8;
		if (d->spinup > 0)
			d->pitchfrac += d->spinup;
		else if (d->spindown > 0)
			d->pitchfrac -= d->spindown;
		pitch = d->pitchfrac >> 8;
		if (pitch > d->pitchrun)
		{
			pitch = d->pitchrun;
			d->spinup = 0;
		}
		if (pitch < d->pitchstart)
		{
			pitch = d->pitchstart;
			d->spindown = 0;
			ambient_emit(e, 0, 0, SND_STOP, 0);
			return;
		}
		pitch = clampi(pitch, 1, 255);
		d->pitch = pitch;
		changed |= prev != pitch;
		flags |= SND_CHANGE_PITCH;
	}
	if (d->fadein || d->fadeout)
	{
		prev = d->volfrac >> 8;
		if (d->fadein > 0)
			d->volfrac += d->fadein;
		else if (d->fadeout > 0)
			d->volfrac -= d->fadeout;
		vol = d->volfrac >> 8;
		if (vol > d->volrun)
		{
			vol = d->volrun;
			d->fadein = 0;
		}
		if (vol < d->volstart)
		{
			vol = d->volstart;
			d->fadeout = 0;
			ambient_emit(e, 0, 0, SND_STOP, 0);
			return;
		}
		vol = clampi(vol, 1, 100);
		d->vol = vol;
		changed |= prev != vol;
		flags |= SND_CHANGE_VOL;
	}
	if (d->lfotype)
	{
		if (d->lfofrac > 0x6fffffff)
			d->lfofrac = 0;
		d->lfofrac += d->lforate;
		pos = d->lfofrac >> 8;
		if (d->lfofrac < 0)
		{
			d->lfofrac = 0;
			d->lforate = abs(d->lforate);
			pos = 0;
		}
		else if (pos > 255)
		{
			pos = 255;
			d->lfofrac = 255 << 8;
			d->lforate = -abs(d->lforate);
		}
		switch (d->lfotype)
		{
		case LFO_SQUARE:
			d->lfomult = pos < 128 ? 255 : 0;
			break;
		case LFO_RANDOM:
			if (pos == 255)
				d->lfomult = RANDOM_LONG(0, 255);
			break;
		case LFO_TRIANGLE:
		default:
			d->lfomult = pos;
			break;
		}
		if (d->lfomodpitch)
		{
			prev = pitch;
			pitch += ((d->lfomult - 128) * d->lfomodpitch) / 100;
			pitch = clampi(pitch, 1, 255);
			changed |= prev != pitch;
			flags |= SND_CHANGE_PITCH;
		}
		if (d->lfomodvol)
		{
			prev = vol;
			vol += ((d->lfomult - 128) * d->lfomodvol) / 100;
			vol = clampi(vol, 0, 100);
			changed |= prev != vol;
			flags |= SND_CHANGE_VOL;
		}
	}
	if (flags && changed)
	{
		if (pitch == PITCH_NORM)
			pitch = PITCH_NORM + 1;
		ambient_emit(e, (float)(vol * 0.01), p->m_flAttenuation, flags, pitch);
	}
	ThinkIn(e, 0.2f);
}

void CAmbientGeneric_ToggleUse(int e, int activator, int caller, int use_type, float value)
{
	CAmbientGeneric *p = PRIV(CAmbientGeneric, e);
	dynpitchvol_t *d = &p->m_dpv;
	float fraction;
	int pitchinc;

	(void)activator;
	(void)caller;
	if (use_type != USE_TOGGLE)
	{
		if ((p->m_fActive && use_type == USE_ON) || (!p->m_fActive && use_type == USE_OFF))
			return;
	}
	if (use_type == USE_SET && p->m_fActive)
	{
		fraction = value;
		if (fraction > 1.0f)
			fraction = 1.0f;
		if (fraction < 0.0f)
			fraction = 0.01f;
		d->pitch = (int)(fraction * 255);
		ambient_emit(e, 0, 0, SND_CHANGE_PITCH, d->pitch);
		return;
	}
	if (p->m_fActive)
	{
		if (d->cspinup)
		{
			if (d->cspincount <= d->cspinup)
			{
				d->cspincount++;
				pitchinc = (255 - d->pitchstart) / d->cspinup;
				d->spinup = d->spinupsav;
				d->spindown = 0;
				d->pitchrun = d->pitchstart + pitchinc * d->cspincount;
				if (d->pitchrun > 255)
					d->pitchrun = 255;
				ThinkIn(e, 0.1f);
			}
		}
		else
		{
			p->m_fActive = 0;
			p->spawnflags |= AMBIENT_SOUND_START_SILENT;
			if (d->spindownsav || d->fadeoutsav)
			{
				d->spindown = d->spindownsav;
				d->spinup = 0;
				d->fadeout = d->fadeoutsav;
				d->fadein = 0;
				ThinkIn(e, 0.1f);
			}
			else
				ambient_emit(e, 0, 0, SND_STOP, 0);
		}
	}
	else
	{
		if (p->m_fLooping)
			p->m_fActive = 1;
		else
			ambient_emit(e, 0, 0, SND_STOP, 0);
		InitModulationParms(e);
		ambient_emit(e, (float)(d->vol * 0.01), p->m_flAttenuation, 0, d->pitch);
		ThinkIn(e, 0.1f);
	}
}

/* CAmbientGeneric::KeyValue: the dpv keys with their clamps and scales */
static void CAmbientGeneric_KeyValue(int e, gid_t16 gid)
{
	dynpitchvol_t *d = &PRIV(CAmbientGeneric, e)->m_dpv;
	float v;

	if (gid == GID_NONE)
		return;
	if (rec_key_f(gid, K_PRESET, &v))
		d->preset = (int)v;
	if (rec_key_f(gid, K_PITCH, &v))
		d->pitchrun = clampi((int)v, 0, 255);
	if (rec_key_f(gid, K_PITCHSTART, &v))
		d->pitchstart = clampi((int)v, 0, 255);
	if (rec_key_f(gid, K_SPINUP, &v))
	{
		d->spinup = clampi((int)v, 0, 100);
		if (d->spinup > 0)
			d->spinup = (101 - d->spinup) * 64;
		d->spinupsav = d->spinup;
	}
	if (rec_key_f(gid, K_SPINDOWN, &v))
	{
		d->spindown = clampi((int)v, 0, 100);
		if (d->spindown > 0)
			d->spindown = (101 - d->spindown) * 64;
		d->spindownsav = d->spindown;
	}
	if (rec_key_f(gid, K_VOLSTART, &v))
		d->volstart = clampi((int)v, 0, 10) * 10;
	if (rec_key_f(gid, K_FADEIN, &v))
	{
		d->fadein = clampi((int)v, 0, 100);
		if (d->fadein > 0)
			d->fadein = (101 - d->fadein) * 64;
		d->fadeinsav = d->fadein;
	}
	if (rec_key_f(gid, K_FADEOUT, &v))
	{
		d->fadeout = clampi((int)v, 0, 100);
		if (d->fadeout > 0)
			d->fadeout = (101 - d->fadeout) * 64;
		d->fadeoutsav = d->fadeout;
	}
	if (rec_key_f(gid, K_LFOTYPE, &v))
	{
		d->lfotype = (int)v;
		if (d->lfotype > 4)
			d->lfotype = LFO_TRIANGLE;
	}
	if (rec_key_f(gid, K_LFORATE, &v))
		d->lforate = clampi((int)v, 0, 1000) * 256;
	if (rec_key_f(gid, K_LFOMODPITCH, &v))
		d->lfomodpitch = clampi((int)v, 0, 100);
	if (rec_key_f(gid, K_LFOMODVOL, &v))
		d->lfomodvol = clampi((int)v, 0, 100);
	if (rec_key_f(gid, K_CSPINUP, &v))
		d->cspinup = clampi((int)v, 0, 100);
}

static int CAmbientGeneric_Spawn(int e, gid_t16 gid)
{
	CAmbientGeneric *p = PRIV(CAmbientGeneric, e);
	const float *o = ent_cv(e)->origin;

	CAmbientGeneric_KeyValue(e, gid);
	p->message = gid == GID_NONE ? RES_NONE : rec_key_res(gid, K_MESSAGE);
	if (p->spawnflags & AMBIENT_SOUND_EVERYWHERE)
		p->m_flAttenuation = ATTN_NONE;
	else if (p->spawnflags & AMBIENT_SOUND_SMALLRADIUS)
		p->m_flAttenuation = ATTN_IDLE;
	else if (p->spawnflags & AMBIENT_SOUND_MEDIUMRADIUS)
		p->m_flAttenuation = ATTN_STATIC;
	else if (p->spawnflags & AMBIENT_SOUND_LARGERADIUS)
		p->m_flAttenuation = ATTN_NORM;
	else
		p->m_flAttenuation = ATTN_STATIC;
	if (p->message == RES_NONE)
	{
		log_printf("game: EMPTY AMBIENT AT: %f, %f, %f\n", (double)o[0], (double)o[1],
			(double)o[2]);
		ThinkIn(e, 0.1f);
		SetThink(e, FN_SUB_Remove);
		return 0;
	}
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	SetThink(e, FN_CAmbientGeneric_RampThink);
	ent_think_at(e, 0);
	SetUse(e, FN_CAmbientGeneric_ToggleUse);
	p->m_fActive = 0;
	p->m_fLooping = !(p->spawnflags & AMBIENT_SOUND_NOT_LOOPING);
	/* Precache */
	if (RES_KIND(p->message) == RES_K_SOUND)
		PRECACHE(p->message);
	InitModulationParms(e);
	if (!(p->spawnflags & AMBIENT_SOUND_START_SILENT) && p->m_fLooping)
		p->m_fActive = 1;
	if (p->m_fActive)
	{
		ambient_emit(e, (float)(p->m_dpv.vol * 0.01), p->m_flAttenuation, SND_SPAWNING,
			p->m_dpv.pitch);
		ThinkIn(e, 0.1f);
	}
	return 0;
}

static int NoTransition_ObjectCaps(int e)
{
	(void)e;
	return 0;
}

static const game_vt_t CAmbientGeneric_vt = {
	.ObjectCaps = NoTransition_ObjectCaps,
};

const game_class_t gc_AMBIENT_GENERIC = {
	.size = sizeof(CAmbientGeneric), .Spawn = CAmbientGeneric_Spawn,
	.vt = &CAmbientGeneric_vt,
};

/* ---- speaker (CSpeaker) ------------------------------------------------------ */

/* CTalkMonster::g_talkWaitTime (GameMon's talk monsters) */
extern float g_talkWaitTime __attribute__((weak));

#define ANNOUNCE_MINUTES_MIN	0.25
#define ANNOUNCE_MINUTES_MAX	2.25

typedef struct {
	CBASEENTITY_FIELDS
	int32_t		m_preset;
} CSpeaker;

/* The m_preset groups (C1A0_ .. C3A2_) are not cooked: no map of the
 * campaign uses a preset, and each would add its sentences' words to the
 * cart. A preset speaker logs and stays silent. */
static res_t speaker_preset(int n)
{
	log_printf("game: speaker preset %d: group not cooked (res.def)\n", n);
	return RES_NONE;
}

void CSpeaker_SpeakerThink(int e)
{
	CSpeaker *p = PRIV(CSpeaker, e);
	float vol = p->health * 0.1f, attn = 0.3f, t = gtime();
	res_t s;

	if (&g_talkWaitTime && t <= g_talkWaitTime)
	{
		ent_think_at(e, g_talkWaitTime + RANDOM_FLOAT(5, 10));
		return;
	}
	s = p->m_preset ? speaker_preset(p->m_preset) : p->message;
	if (RES_KIND(s) == RES_K_SENTENCE)
	{
		snd_sentence(e, CHAN_STATIC, (uint16_t)RES_INDEX(s), vol, attn, 0, 100);
		ent_think_at(e, 0);
		return;
	}
	if (RES_KIND(s) == RES_K_GROUP)
		EMIT_SENTENCE(e, CHAN_VOICE, s, vol, attn, 0, 100);
	else
		log_printf("game: Level Design Error! SPEAKER has bad sentence group name\n");
	ent_think_at(e, (float)(t + RANDOM_FLOAT((float)(ANNOUNCE_MINUTES_MIN * 60.0),
		(float)(ANNOUNCE_MINUTES_MAX * 60.0))));
	if (&g_talkWaitTime)
		g_talkWaitTime = t + 5;
}

void CSpeaker_ToggleUse(int e, int activator, int caller, int use_type, float value)
{
	int active = ent_tm(e)->nextthink > 0.0f;

	(void)activator;
	(void)caller;
	(void)value;
	if (use_type != USE_TOGGLE)
	{
		if ((active && use_type == USE_ON) || (!active && use_type == USE_OFF))
			return;
	}
	if (use_type == USE_ON)
	{
		ThinkIn(e, 0.1f);
		return;
	}
	if (use_type == USE_OFF)
	{
		ent_think_at(e, 0);
		return;
	}
	if (active)
		ent_think_at(e, 0);
	else
		ThinkIn(e, 0.1f);
}

static int CSpeaker_Spawn(int e, gid_t16 gid)
{
	CSpeaker *p = PRIV(CSpeaker, e);
	const float *o = ent_cv(e)->origin;

	p->m_preset = (int)KeyF(gid, K_PRESET, 0);
	p->message = gid == GID_NONE ? RES_NONE : rec_key_res(gid, K_MESSAGE);
	if (!p->m_preset && p->message == RES_NONE)
	{
		log_printf("game: SPEAKER with no Level/Sentence! at: %f, %f, %f\n", (double)o[0],
			(double)o[1], (double)o[2]);
		ThinkIn(e, 0.1f);
		SetThink(e, FN_SUB_Remove);
		return 0;
	}
	ent_set_solid(e, SOLID_NOT);
	ent_set_movetype(e, MOVETYPE_NONE);
	SetThink(e, FN_CSpeaker_SpeakerThink);
	ent_think_at(e, 0);
	SetUse(e, FN_CSpeaker_ToggleUse);
	if (!(p->spawnflags & SPEAKER_START_SILENT))
		ThinkIn(e, RANDOM_FLOAT(5.0f, 15.0f));
	return 0;
}

const game_class_t gc_SPEAKER = {
	.size = sizeof(CSpeaker), .Spawn = CSpeaker_Spawn, .vt = &CAmbientGeneric_vt,
};
