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
 * anim.c -- game/studio/anim.h: HLSDK dlls/animation.cpp (LookupActivity,
 * LookupActivityHeaviest, LookupSequence, GetSequenceInfo,
 * GetSequenceFlags, GetAnimationEvent, SetController, SetBlending,
 * FindTransition, SetBodygroup, GetBodygroup, GetEyePosition, ExtractBbox)
 * and the CBaseAnimating members of dlls/animating.cpp
 * (StudioFrameAdvance, ResetSequenceInfo, DispatchAnimEvents' window,
 * SetSequenceBox, GetAttachment / GetBonePosition) over the cooked info
 * block (docs/formats/studio-format.md). gpGlobals->time is the f32 of
 * eng_time(); RANDOM_LONG is hl64_rand_long.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/pmove.h"
#include "game/studio/anim.h"
#include "game/studio/pose.h"

#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

#define STUDIO_XR	0x0008
#define STUDIO_YR	0x0010
#define STUDIO_ZR	0x0020

static const sfmt_seq_t *seq_of(const sfmt_info_t *h, int seq)
{
	return h && seq >= 0 && seq < h->numseq ? &SFMT_SEQS(h)[seq] : NULL;
}

static float gtime(void)
{
	return (float)eng_time();
}

static int ieq(const char *a, const char *b)
{
	for (;; a++, b++)
	{
		int x = (unsigned char)*a, y = (unsigned char)*b;

		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
			return 0;
		if (!x)
			return 1;
	}
}

int gs_lookup_sequence(res_t model, const char *label)
{
	const sfmt_info_t *h = gs_info(model);
	int i;

	if (!h)
		return 0;
	for (i = 0; i < h->numseq; i++)
		if (ieq(SFMT_STR(h, SFMT_SEQS(h)[i].label), label))
			return i;
	return -1;
}

int gs_lookup_activity(res_t model, int activity)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *pseqdesc;
	int i, weighttotal = 0, seq = ACTIVITY_NOT_AVAILABLE;

	if (!h)
		return 0;
	pseqdesc = SFMT_SEQS(h);
	for (i = 0; i < h->numseq; i++)
	{
		if (pseqdesc[i].activity == activity)
		{
			weighttotal += pseqdesc[i].actweight;
			if (!weighttotal || hl64_rand_long(0, weighttotal - 1) < pseqdesc[i].actweight)
				seq = i;
		}
	}
	return seq;
}

int gs_lookup_activity_heaviest(res_t model, int activity)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *pseqdesc;
	int i, weight = 0, seq = ACTIVITY_NOT_AVAILABLE;

	if (!h)
		return 0;
	pseqdesc = SFMT_SEQS(h);
	for (i = 0; i < h->numseq; i++)
	{
		if (pseqdesc[i].activity == activity && pseqdesc[i].actweight > weight)
		{
			weight = pseqdesc[i].actweight;
			seq = i;
		}
	}
	return seq;
}

void gs_eye_position(res_t model, float out[3])
{
	const sfmt_info_t *h = gs_info(model);

	if (!h)
		return;
	memcpy(out, h->eyeposition, sizeof(h->eyeposition));
}

void gs_sequence_info(res_t model, int seq, float *framerate, float *groundspeed)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *s;

	if (!h)
		return;
	s = seq_of(h, seq);
	if (!s)
	{
		*framerate = 0.0f;
		*groundspeed = 0.0f;
		return;
	}
	if (s->numframes > 1)
	{
		*framerate = 256 * s->fps / (s->numframes - 1);
		*groundspeed = (float)sqrt(s->linearmovement[0] * s->linearmovement[0]
			+ s->linearmovement[1] * s->linearmovement[1]
			+ s->linearmovement[2] * s->linearmovement[2]);
		*groundspeed = *groundspeed * s->fps / (s->numframes - 1);
	}
	else
	{
		*framerate = 256.0f;
		*groundspeed = 0.0f;
	}
}

int gs_sequence_flags(res_t model, int seq)
{
	const sfmt_seq_t *s = seq_of(gs_info(model), seq);

	return s ? (int)s->flags : 0;
}

int gs_sequence_frames(res_t model, int seq)
{
	const sfmt_seq_t *s = seq_of(gs_info(model), seq);

	return s ? s->numframes : 0;
}

int gs_sequence_count(res_t model)
{
	const sfmt_info_t *h = gs_info(model);

	return h ? h->numseq : 0;
}

int gs_extract_bbox(res_t model, int seq, float mins[3], float maxs[3])
{
	const sfmt_seq_t *s = seq_of(gs_info(model), seq);

	if (!s)
		return 0;
	memcpy(mins, s->bbmin, sizeof(s->bbmin));
	memcpy(maxs, s->bbmax, sizeof(s->bbmax));
	return 1;
}

int gs_anim_event(res_t model, int seq, float flStart, float flEnd, int index, int *event,
	const char **options)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *pseqdesc = seq_of(h, seq);
	const sfmt_event_t *pevent;

	if (!pseqdesc || !event || !options)
		return 0;
	pevent = SFMT_EVENTS(h) + pseqdesc->event_first;
	if (pseqdesc->numevents == 0 || index > pseqdesc->numevents)
		return 0;

	if (pseqdesc->numframes > 1)
	{
		flStart *= (pseqdesc->numframes - 1) / 256.0f;
		flEnd *= (pseqdesc->numframes - 1) / 256.0f;
	}
	else
	{
		flStart = 0;
		flEnd = 1.0f;
	}

	for (; index < pseqdesc->numevents; index++)
	{
		// Don't send client-side events to the server AI
		if (pevent[index].event >= EVENT_CLIENT)
			continue;

		if ((pevent[index].frame >= flStart && pevent[index].frame < flEnd)
			|| ((pseqdesc->flags & STUDIO_LOOPING) && flEnd >= pseqdesc->numframes - 1
			&& pevent[index].frame < flEnd - pseqdesc->numframes + 1))
		{
			*event = pevent[index].event;
			*options = SFMT_STR(h, pevent[index].options);
			return index + 1;
		}
	}
	return 0;
}

res_t gs_anim_event_res(res_t model, int seq, int index)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *s = seq_of(h, seq);

	if (!s || index < 1 || index > s->numevents)
		return RES_NONE;
	return SFMT_EVENTS(h)[s->event_first + index - 1].res;
}

/* the rotational wrap SetController / SetBlending share */
static float wrap_rot(float flValue, float start, float end, int full)
{
	// ugly hack, invert value if end < start
	if (end < start)
		flValue = -flValue;

	// does the controller not wrap?
	if (start + 359.0f >= end)
	{
		if (flValue > ((start + end) / 2.0f) + 180)
			flValue = flValue - 360;
		if (flValue < ((start + end) / 2.0f) - 180)
			flValue = flValue + 360;
	}
	else if (full)
	{
		if (flValue > 360)
			flValue = flValue - (int)(flValue / 360.0f) * 360.0f;
		else if (flValue < 0)
			flValue = flValue + (int)((flValue / -360.0f) + 1) * 360.0f;
	}
	return flValue;
}

float gs_set_controller(res_t model, uint8_t ctl[4], int iController, float flValue)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_ctl_t *pbonecontroller;
	int i, setting;

	if (!h)
		return flValue;
	pbonecontroller = SFMT_CTLS(h);
	// find first controller that matches the index
	for (i = 0; i < h->numctl; i++, pbonecontroller++)
		if (pbonecontroller->index == iController)
			break;
	if (i >= h->numctl)
		return flValue;

	// wrap 0..360 if it's a rotational controller
	if (pbonecontroller->type & (STUDIO_XR | STUDIO_YR | STUDIO_ZR))
		flValue = wrap_rot(flValue, pbonecontroller->start, pbonecontroller->end, 1);

	setting = (int)(255 * (flValue - pbonecontroller->start)
		/ (pbonecontroller->end - pbonecontroller->start));
	if (setting < 0)
		setting = 0;
	if (setting > 255)
		setting = 255;
	if (iController >= 0 && iController < 4)
		ctl[iController] = (uint8_t)setting;

	return setting * (1.0f / 255.0f) * (pbonecontroller->end - pbonecontroller->start)
		+ pbonecontroller->start;
}

float gs_set_blending(res_t model, int seq, uint8_t blend[2], int iBlender, float flValue)
{
	const sfmt_seq_t *s = seq_of(gs_info(model), seq);
	int setting;

	if (!s || iBlender < 0 || iBlender > 1)
		return flValue;
	if (s->blendtype[iBlender] == 0)
		return flValue;

	if (s->blendtype[iBlender] & (STUDIO_XR | STUDIO_YR | STUDIO_ZR))
		flValue = wrap_rot(flValue, s->blendstart[iBlender], s->blendend[iBlender], 0);

	setting = (int)(255 * (flValue - s->blendstart[iBlender])
		/ (s->blendend[iBlender] - s->blendstart[iBlender]));
	if (setting < 0)
		setting = 0;
	if (setting > 255)
		setting = 255;
	blend[iBlender] = (uint8_t)setting;

	return setting * (1.0f / 255.0f) * (s->blendend[iBlender] - s->blendstart[iBlender])
		+ s->blendstart[iBlender];
}

int gs_find_transition(res_t model, int iEndingAnim, int iGoalAnim, int *piDir)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_seq_t *pseqdesc;
	const uint8_t *pTransition;
	int iEndNode, iInternNode, i, nt;

	if (!h || !seq_of(h, iEndingAnim) || !seq_of(h, iGoalAnim))
		return iGoalAnim;
	pseqdesc = SFMT_SEQS(h);

	// bail if we're going to or from a node 0
	if (pseqdesc[iEndingAnim].entrynode == 0 || pseqdesc[iGoalAnim].entrynode == 0)
		return iGoalAnim;

	if (*piDir > 0)
		iEndNode = pseqdesc[iEndingAnim].exitnode;
	else
		iEndNode = pseqdesc[iEndingAnim].entrynode;

	if (iEndNode == pseqdesc[iGoalAnim].entrynode)
	{
		*piDir = 1;
		return iGoalAnim;
	}

	nt = h->numtransitions;
	if (iEndNode < 1 || iEndNode > nt || pseqdesc[iGoalAnim].entrynode > nt)
		return iGoalAnim;
	pTransition = SFMT_TRANS(h);
	iInternNode = pTransition[(iEndNode - 1) * nt + (pseqdesc[iGoalAnim].entrynode - 1)];

	if (iInternNode == 0)
		return iGoalAnim;

	// look for someone going
	for (i = 0; i < h->numseq; i++)
	{
		if (pseqdesc[i].entrynode == iEndNode && pseqdesc[i].exitnode == iInternNode)
		{
			*piDir = 1;
			return i;
		}
		if (pseqdesc[i].nodeflags)
		{
			if (pseqdesc[i].exitnode == iEndNode && pseqdesc[i].entrynode == iInternNode)
			{
				*piDir = -1;
				return i;
			}
		}
	}
	return iGoalAnim;
}

void gs_set_bodygroup(res_t model, uint8_t *body, int iGroup, int iValue)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_bodypart_t *pbodypart;
	int iCurrent;

	if (!h || iGroup < 0 || iGroup >= h->numbodyparts)
		return;
	pbodypart = SFMT_BODYPARTS(h) + iGroup;
	if (iValue >= pbodypart->nummodels)
		return;
	iCurrent = (*body / pbodypart->base) % pbodypart->nummodels;
	*body = (uint8_t)(*body - (iCurrent * pbodypart->base) + (iValue * pbodypart->base));
}

int gs_get_bodygroup(res_t model, int body, int iGroup)
{
	const sfmt_info_t *h = gs_info(model);
	const sfmt_bodypart_t *pbodypart;

	if (!h || iGroup < 0 || iGroup >= h->numbodyparts)
		return 0;
	pbodypart = SFMT_BODYPARTS(h) + iGroup;
	if (pbodypart->nummodels <= 1)
		return 0;
	return (body / pbodypart->base) % pbodypart->nummodels;
}

/* ---- pfnGetAttachment / pfnGetBonePosition ------------------------------------ */

int gs_attachment(int e, int i, float out[3])
{
	const float (*bones)[3][4];
	const sfmt_info_t *h;
	const sfmt_attach_t *a;
	int n, k;

	if (!ent_is_live(e))
		return 0;
	memcpy(out, ent_cv(e)->origin, 3 * sizeof(float));
	h = gs_info(ent_rnd(e)->model);
	if (!h || i < 0 || i >= h->numattach)
		return 0;
	n = gs_entity_pose(e, &bones);
	a = SFMT_ATTACHES(h) + i;
	if (a->bone >= n)
		return 0;
	for (k = 0; k < 3; k++)
		out[k] = a->org[0] * bones[a->bone][k][0] + a->org[1] * bones[a->bone][k][1]
			+ a->org[2] * bones[a->bone][k][2] + bones[a->bone][k][3];
	return 1;
}

int gs_bone_position(int e, int bone, float out[3])
{
	const float (*bones)[3][4];
	int n, k;

	if (!ent_is_live(e))
		return 0;
	n = gs_entity_pose(e, &bones);
	if (bone < 0 || bone >= n)
		return 0;
	for (k = 0; k < 3; k++)
		out[k] = bones[bone][k][3];
	return 1;
}

/* ---- CBaseAnimating -------------------------------------------------------------- */

void gs_reset_sequence_info(int e, gs_animating_t *a)
{
	ent_rnd_t *r = ent_rnd(e);

	gs_sequence_info(r->model, r->sequence, &a->m_flFrameRate, &a->m_flGroundSpeed);
	a->m_fSequenceLoops = (gs_sequence_flags(r->model, r->sequence) & STUDIO_LOOPING) != 0;
	r->animtime = gtime();
	r->framerate = 1.0f;
	a->m_fSequenceFinished = 0;
	a->m_flLastEventCheck = gtime();
}

float gs_frame_advance(int e, gs_animating_t *a, float flInterval)
{
	ent_rnd_t *r = ent_rnd(e);
	float time = gtime();

	if (flInterval == 0.0f)
	{
		flInterval = time - r->animtime;
		if (flInterval <= 0.001f)
		{
			r->animtime = time;
			return 0.0f;
		}
	}
	if (!r->animtime)
		flInterval = 0.0f;

	r->frame += flInterval * a->m_flFrameRate * r->framerate;
	r->animtime = time;

	if (r->frame < 0.0f || r->frame >= 256.0f)
	{
		if (a->m_fSequenceLoops)
			r->frame -= (int)(r->frame / 256.0f) * 256.0f;
		else
			r->frame = (r->frame < 0.0f) ? 0 : 255;
		a->m_fSequenceFinished = 1;	// just in case it wasn't caught in GetEvents
	}
	return flInterval;
}

void gs_event_window(int e, gs_animating_t *a, float *flStart, float *flEnd)
{
	const ent_rnd_t *r = ent_rnd(e);
	// FIXME: I have to do this or some events get missed, and this is probably causing the problem below
	float flInterval = 0.1f;

	// FIX: this still sometimes hits events twice
	*flStart = r->frame + (a->m_flLastEventCheck - r->animtime) * a->m_flFrameRate * r->framerate;
	*flEnd = r->frame + flInterval * a->m_flFrameRate * r->framerate;
	a->m_flLastEventCheck = r->animtime + flInterval;

	a->m_fSequenceFinished = 0;
	if (*flEnd >= 256 || *flEnd <= 0.0f)
		a->m_fSequenceFinished = 1;
}

void gs_set_sequence_box(int e)
{
	const ent_rnd_t *r = ent_rnd(e);
	float mins[3], maxs[3], rmin[3], rmax[3], c, sn, ax, bx, ay, by;

	// Get sequence bbox
	if (!gs_extract_bbox(r->model, r->sequence, mins, maxs))
		return;
	/* the bbox's corners rotated by yaw: per axis the smallest and the
	 * largest of (cos, -sin) . (x, y) resp. (sin, cos) . (x, y), each term
	 * taken at the end of its interval that minimises / maximises it */
	c = cosf(ent_cv(e)->angles[1] * (float)(M_PI / 180.0));
	sn = sinf(ent_cv(e)->angles[1] * (float)(M_PI / 180.0));
	ax = c * mins[0];
	bx = c * maxs[0];
	ay = -sn * mins[1];
	by = -sn * maxs[1];
	rmin[0] = (ax < bx ? ax : bx) + (ay < by ? ay : by);
	rmax[0] = (ax > bx ? ax : bx) + (ay > by ? ay : by);
	ax = sn * mins[0];
	bx = sn * maxs[0];
	ay = c * mins[1];
	by = c * maxs[1];
	rmin[1] = (ax < bx ? ax : bx) + (ay < by ? ay : by);
	rmax[1] = (ax > bx ? ax : bx) + (ay > by ? ay : by);
	rmin[2] = 0;
	rmax[2] = rmin[2] + 1;
	ent_set_size(e, rmin, rmax);
}

float gs_view_frame(res_t model, int seq, float frame, float animtime, float framerate)
{
	const sfmt_seq_t *s = seq_of(gs_info(model), seq);
	gs_clent_t cl;
	float f;

	if (!s || s->numframes <= 1)
		return 0;
	cl.curstate.frame = frame;
	cl.curstate.animtime = animtime;
	cl.curstate.framerate = framerate;
	f = gs_estimate_frame(s, &cl, eng_time(), 1);
	return f * 256.0f / (s->numframes - 1);
}
