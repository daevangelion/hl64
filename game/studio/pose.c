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
 * pose.c -- studio poses: HLSDK cl_dll/StudioModelRenderer.cpp
 * (StudioCalcBoneAdj, StudioCalcBoneQuaterion, StudioCalcBonePosition,
 * StudioSlerpBones, StudioCalcRotations, StudioEstimateFrame,
 * StudioEstimateInterpolant, StudioSetupBones) and cl_dll/studio_util.cpp
 * (AngleMatrix, ConcatTransforms, AngleQuaternion, QuaternionSlerp,
 * QuaternionMatrix) over the cooked info block and per-sequence animation
 * chunks (docs/formats/studio-format.md) instead of a studiohdr_t.
 *
 * Two poses (docs/specs/trace-and-hulls.md 4.5, architecture.md 9.4):
 *  - client (game_studio_setup_bones, the draw): the frame estimated from
 *    animtime and framerate, controllers / blends interpolated over the
 *    0.1 s after a change, the previous sequence blended out over 0.2 s
 *    (the client's latched state, kept here per entity slot);
 *  - server (game_studio_pose: hitboxes, attachments, bone positions):
 *    pev->frame as is, no interpolation, memoised by its inputs.
 * A sequence whose chunk is not resident (studio_anim NULL: requested)
 * holds the entity's last resident sequence and frame, else the bones'
 * default values. The pose is computed in float (hl_sinf / hl_cosf).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine/core/hlmath.h"

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/studio.h"
#include "game/studio/pose.h"

#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

#define STUDIO_X	0x0001
#define STUDIO_Y	0x0002
#define STUDIO_Z	0x0004
#define STUDIO_XR	0x0008
#define STUDIO_YR	0x0010
#define STUDIO_ZR	0x0020
#define STUDIO_TYPES	0x7FFF
#define STUDIO_RLOOP	0x8000
#define STUDIO_LOOPING	0x0001
#define PITCH		0
#define YAW		1
#define ROLL		2

typedef float	vec4_t[4];

const sfmt_info_t *gs_info(res_t model)
{
	const sfmt_info_t *h;

	if (RES_KIND(model) != RES_K_MODEL)
		return NULL;
	h = studio_info(model, NULL);
	return h && h->magic == SFMT_INFO_MAGIC ? h : NULL;
}

/* ---- studio_util.cpp ------------------------------------------------------ */

static void AngleMatrix(const float *angles, float (*matrix)[4])
{
	float angle;
	float sr, sp, sy, cr, cp, cy;

	angle = angles[YAW] * (float)(M_PI * 2 / 360);
	sy = hl_sinf(angle);
	cy = hl_cosf(angle);
	angle = angles[PITCH] * (float)(M_PI * 2 / 360);
	sp = hl_sinf(angle);
	cp = hl_cosf(angle);
	angle = angles[ROLL] * (float)(M_PI * 2 / 360);
	sr = hl_sinf(angle);
	cr = hl_cosf(angle);

	// matrix = (YAW * PITCH) * ROLL
	matrix[0][0] = cp * cy;
	matrix[1][0] = cp * sy;
	matrix[2][0] = -sp;
	matrix[0][1] = sr * sp * cy + cr * -sy;
	matrix[1][1] = sr * sp * sy + cr * cy;
	matrix[2][1] = sr * cp;
	matrix[0][2] = (cr * sp * cy + -sr * -sy);
	matrix[1][2] = (cr * sp * sy + -sr * cy);
	matrix[2][2] = cr * cp;
	matrix[0][3] = 0.0f;
	matrix[1][3] = 0.0f;
	matrix[2][3] = 0.0f;
}

static int VectorCompare(const float *v1, const float *v2)
{
	int i;

	for (i = 0; i < 3; i++)
		if (v1[i] != v2[i])
			return 0;
	return 1;
}

static void ConcatTransforms(const float in1[3][4], const float in2[3][4], float out[3][4])
{
	out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
	out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
	out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
	out[0][3] = in1[0][0] * in2[0][3] + in1[0][1] * in2[1][3] + in1[0][2] * in2[2][3]
		+ in1[0][3];
	out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
	out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
	out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
	out[1][3] = in1[1][0] * in2[0][3] + in1[1][1] * in2[1][3] + in1[1][2] * in2[2][3]
		+ in1[1][3];
	out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
	out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
	out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
	out[2][3] = in1[2][0] * in2[0][3] + in1[2][1] * in2[1][3] + in1[2][2] * in2[2][3]
		+ in1[2][3];
}

// angles index are not the same as ROLL, PITCH, YAW
static void AngleQuaternion(const float *angles, vec4_t quaternion)
{
	float angle;
	float sr, sp, sy, cr, cp, cy;

	// FIXME: rescale the inputs to 1/2 angle
	angle = angles[2] * 0.5f;
	sy = hl_sinf(angle);
	cy = hl_cosf(angle);
	angle = angles[1] * 0.5f;
	sp = hl_sinf(angle);
	cp = hl_cosf(angle);
	angle = angles[0] * 0.5f;
	sr = hl_sinf(angle);
	cr = hl_cosf(angle);

	quaternion[0] = sr * cp * cy - cr * sp * sy; // X
	quaternion[1] = cr * sp * cy + sr * cp * sy; // Y
	quaternion[2] = cr * cp * sy - sr * sp * cy; // Z
	quaternion[3] = cr * cp * cy + sr * sp * sy; // W
}

static void QuaternionSlerp(const vec4_t p, vec4_t q, float t, vec4_t qt)
{
	int i;
	float omega, cosom, sinom, sclp, sclq;

	// decide if one of the quaternions is backwards
	float a = 0;
	float b = 0;

	for (i = 0; i < 4; i++)
	{
		a += (p[i] - q[i]) * (p[i] - q[i]);
		b += (p[i] + q[i]) * (p[i] + q[i]);
	}
	if (a > b)
	{
		for (i = 0; i < 4; i++)
			q[i] = -q[i];
	}

	cosom = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];

	if ((1.0f + cosom) > 0.000001f)
	{
		if ((1.0f - cosom) > 0.000001f)
		{
			omega = hl_acosf(cosom);
			sinom = hl_sinf(omega);
			sclp = hl_sinf((1.0f - t) * omega) / sinom;
			sclq = hl_sinf(t * omega) / sinom;
		}
		else
		{
			sclp = 1.0f - t;
			sclq = t;
		}
		for (i = 0; i < 4; i++)
			qt[i] = sclp * p[i] + sclq * q[i];
	}
	else
	{
		qt[0] = -q[1];
		qt[1] = q[0];
		qt[2] = -q[3];
		qt[3] = q[2];
		sclp = hl_sinf((1.0f - t) * (0.5f * (float)M_PI));
		sclq = hl_sinf(t * (0.5f * (float)M_PI));
		for (i = 0; i < 3; i++)
			qt[i] = sclp * p[i] + sclq * qt[i];
	}
}

static void QuaternionMatrix(const vec4_t quaternion, float (*matrix)[4])
{
	matrix[0][0] = 1.0f - 2.0f * quaternion[1] * quaternion[1] - 2.0f * quaternion[2] * quaternion[2];
	matrix[1][0] = 2.0f * quaternion[0] * quaternion[1] + 2.0f * quaternion[3] * quaternion[2];
	matrix[2][0] = 2.0f * quaternion[0] * quaternion[2] - 2.0f * quaternion[3] * quaternion[1];

	matrix[0][1] = 2.0f * quaternion[0] * quaternion[1] - 2.0f * quaternion[3] * quaternion[2];
	matrix[1][1] = 1.0f - 2.0f * quaternion[0] * quaternion[0] - 2.0f * quaternion[2] * quaternion[2];
	matrix[2][1] = 2.0f * quaternion[1] * quaternion[2] + 2.0f * quaternion[3] * quaternion[0];

	matrix[0][2] = 2.0f * quaternion[0] * quaternion[2] + 2.0f * quaternion[3] * quaternion[1];
	matrix[1][2] = 2.0f * quaternion[1] * quaternion[2] - 2.0f * quaternion[3] * quaternion[0];
	matrix[2][2] = 1.0f - 2.0f * quaternion[0] * quaternion[0] - 2.0f * quaternion[1] * quaternion[1];
}

/* ---- StudioModelRenderer.cpp ---------------------------------------------- */

/* panim->offset[j] of bone `bone`, blend `blend`: the channel's value words,
 * NULL for offset 0 (or no chunk: every bone at its default). */
static const int16_t *channel(const uint16_t *chunk, int numbones, int blend, int bone, int j)
{
	uint16_t w;

	if (!chunk)
		return NULL;
	w = chunk[(blend * numbones + bone) * 6 + j];
	return w ? (const int16_t *)chunk + w : NULL;
}

static void StudioCalcBoneAdj(const sfmt_info_t *hdr, float dadt, float *adj,
	const uint8_t *pcontroller1, const uint8_t *pcontroller2, uint8_t mouthopen)
{
	int i, j;
	float value = 0;
	const sfmt_ctl_t *pbonecontroller = SFMT_CTLS(hdr);

	for (j = 0; j < hdr->numctl; j++)
	{
		i = pbonecontroller[j].index;
		if (i <= 3)
		{
			// check for 360% wrapping
			if (pbonecontroller[j].type & STUDIO_RLOOP)
			{
				if (abs(pcontroller1[i] - pcontroller2[i]) > 128)
				{
					int a, b;
					/* HLSDK indexes these two with j (the controller
					 * table index), not i: kept */
					a = (pcontroller1[j & 3] + 128) % 256;
					b = (pcontroller2[j & 3] + 128) % 256;
					value = ((a * dadt) + (b * (1 - dadt)) - 128) * (360.0f / 256.0f)
						+ pbonecontroller[j].start;
				}
				else
				{
					value = ((pcontroller1[i] * dadt + (pcontroller2[i]) * (1.0f - dadt)))
						* (360.0f / 256.0f) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = (pcontroller1[i] * dadt + pcontroller2[i] * (1.0f - dadt)) / 255.0f;
				if (value < 0)
					value = 0;
				if (value > 1.0f)
					value = 1.0f;
				value = (1.0f - value) * pbonecontroller[j].start
					+ value * pbonecontroller[j].end;
			}
		}
		else
		{
			value = mouthopen / 64.0f;
			if (value > 1.0f)
				value = 1.0f;
			value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		switch (pbonecontroller[j].type & STUDIO_TYPES)
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = value * (float)(M_PI / 180.0);
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

static void StudioCalcBoneQuaterion(int frame, float s, const sfmt_bone_t *pbone,
	const uint16_t *chunk, int numbones, int blend, int bone, const float *adj, float *q)
{
	int j, k;
	vec4_t q1, q2;
	float angle1[3], angle2[3];
	const int16_t *panimvalue;

	for (j = 0; j < 3; j++)
	{
		panimvalue = channel(chunk, numbones, blend, bone, j + 3);
		if (!panimvalue)
		{
			angle2[j] = angle1[j] = pbone->value[j + 3]; // default;
		}
		else
		{
			k = frame;
			// DEBUG
			if (SFMT_TOTAL(panimvalue[0]) < SFMT_VALID(panimvalue[0]))
				k = 0;
			while (SFMT_TOTAL(panimvalue[0]) <= k)
			{
				k -= SFMT_TOTAL(panimvalue[0]);
				panimvalue += SFMT_VALID(panimvalue[0]) + 1;
				// DEBUG
				if (SFMT_TOTAL(panimvalue[0]) < SFMT_VALID(panimvalue[0]))
					k = 0;
			}
			// Bah, missing blend!
			if (SFMT_VALID(panimvalue[0]) > k)
			{
				angle1[j] = panimvalue[k + 1];

				if (SFMT_VALID(panimvalue[0]) > k + 1)
				{
					angle2[j] = panimvalue[k + 2];
				}
				else
				{
					if (SFMT_TOTAL(panimvalue[0]) > k + 1)
						angle2[j] = angle1[j];
					else
						angle2[j] = panimvalue[SFMT_VALID(panimvalue[0]) + 2];
				}
			}
			else
			{
				angle1[j] = panimvalue[SFMT_VALID(panimvalue[0])];
				if (SFMT_TOTAL(panimvalue[0]) > k + 1)
					angle2[j] = angle1[j];
				else
					angle2[j] = panimvalue[SFMT_VALID(panimvalue[0]) + 2];
			}
			angle1[j] = pbone->value[j + 3] + angle1[j] * pbone->scale[j + 3];
			angle2[j] = pbone->value[j + 3] + angle2[j] * pbone->scale[j + 3];
		}

		if (pbone->ctl[j + 3] != -1)
		{
			angle1[j] += adj[pbone->ctl[j + 3]];
			angle2[j] += adj[pbone->ctl[j + 3]];
		}
	}

	if (!VectorCompare(angle1, angle2))
	{
		AngleQuaternion(angle1, q1);
		AngleQuaternion(angle2, q2);
		QuaternionSlerp(q1, q2, s, q);
	}
	else
	{
		AngleQuaternion(angle1, q);
	}
}

static void StudioCalcBonePosition(int frame, float s, const sfmt_bone_t *pbone,
	const uint16_t *chunk, int numbones, int blend, int bone, const float *adj, float *pos)
{
	int j, k;
	const int16_t *panimvalue;

	for (j = 0; j < 3; j++)
	{
		pos[j] = pbone->value[j]; // default;
		panimvalue = channel(chunk, numbones, blend, bone, j);
		if (panimvalue)
		{
			k = frame;
			// DEBUG
			if (SFMT_TOTAL(panimvalue[0]) < SFMT_VALID(panimvalue[0]))
				k = 0;
			// find span of values that includes the frame we want
			while (SFMT_TOTAL(panimvalue[0]) <= k)
			{
				k -= SFMT_TOTAL(panimvalue[0]);
				panimvalue += SFMT_VALID(panimvalue[0]) + 1;
				// DEBUG
				if (SFMT_TOTAL(panimvalue[0]) < SFMT_VALID(panimvalue[0]))
					k = 0;
			}
			// if we're inside the span
			if (SFMT_VALID(panimvalue[0]) > k)
			{
				// and there's more data in the span
				if (SFMT_VALID(panimvalue[0]) > k + 1)
					pos[j] += (panimvalue[k + 1] * (1.0f - s) + s * panimvalue[k + 2])
						* pbone->scale[j];
				else
					pos[j] += panimvalue[k + 1] * pbone->scale[j];
			}
			else
			{
				// are we at the end of the repeating values section and there's another section with data?
				if (SFMT_TOTAL(panimvalue[0]) <= k + 1)
					pos[j] += (panimvalue[SFMT_VALID(panimvalue[0])] * (1.0f - s)
						+ s * panimvalue[SFMT_VALID(panimvalue[0]) + 2]) * pbone->scale[j];
				else
					pos[j] += panimvalue[SFMT_VALID(panimvalue[0])] * pbone->scale[j];
			}
		}
		if (pbone->ctl[j] != -1 && adj)
			pos[j] += adj[pbone->ctl[j]];
	}
}

static void StudioSlerpBones(int numbones, vec4_t q1[], float pos1[][3], vec4_t q2[],
	float pos2[][3], float s)
{
	int i;
	vec4_t q3;
	float s1;

	if (s < 0)
		s = 0;
	else if (s > 1.0f)
		s = 1.0f;

	s1 = 1.0f - s;

	for (i = 0; i < numbones; i++)
	{
		QuaternionSlerp(q1[i], q2[i], s, q3);
		q1[i][0] = q3[0];
		q1[i][1] = q3[1];
		q1[i][2] = q3[2];
		q1[i][3] = q3[3];
		pos1[i][0] = pos1[i][0] * s1 + pos2[i][0] * s;
		pos1[i][1] = pos1[i][1] * s1 + pos2[i][1] * s;
		pos1[i][2] = pos1[i][2] * s1 + pos2[i][2] * s;
	}
}

static void StudioCalcRotations(const sfmt_info_t *hdr, float pos[][3], vec4_t *q,
	const sfmt_seq_t *pseqdesc, const uint16_t *chunk, int blend, float f, const float *adj)
{
	int i, frame;
	const sfmt_bone_t *pbone;
	float s;

	if (f > pseqdesc->numframes - 1)
	{
		f = 0;	// bah, fix this bug with changing sequences too fast
	}
	// BUG ( somewhere else ) but this code should validate this data.
	// This could cause a crash if the frame # is negative, so we'll go ahead
	//  and clamp it here
	else if (f < -0.01f)
	{
		f = -0.01f;
	}

	frame = (int)f;
	s = (f - frame);

	pbone = SFMT_BONES(hdr);
	for (i = 0; i < hdr->numbones; i++, pbone++)
	{
		StudioCalcBoneQuaterion(frame, s, pbone, chunk, hdr->numbones, blend, i, adj, q[i]);
		StudioCalcBonePosition(frame, s, pbone, chunk, hdr->numbones, blend, i, adj, pos[i]);
	}

	if (pseqdesc->motiontype & STUDIO_X)
		pos[pseqdesc->motionbone][0] = 0.0f;
	if (pseqdesc->motiontype & STUDIO_Y)
		pos[pseqdesc->motionbone][1] = 0.0f;
	if (pseqdesc->motiontype & STUDIO_Z)
		pos[pseqdesc->motionbone][2] = 0.0f;
	/* the STUDIO_LX/LY/LZ terms are scaled by 0 in the HLSDK: dropped */
}

/* StudioEstimateFrame, as the HLSDK writes it, over the cl_entity_t
 * fields it reads (m_fDoInterp: client pose; 0: server pose). */
float gs_estimate_frame(const sfmt_seq_t *pseqdesc, const gs_clent_t *m_pCurrentEntity,
	double m_clTime, int m_fDoInterp)
{
	double				dfdt, f;

	if ( m_fDoInterp )
	{
		if ( m_clTime < m_pCurrentEntity->curstate.animtime )
		{
			dfdt = 0;
		}
		else
		{
			dfdt = (m_clTime - m_pCurrentEntity->curstate.animtime) * m_pCurrentEntity->curstate.framerate * pseqdesc->fps;

		}
	}
	else
	{
		dfdt = 0;
	}

	if (pseqdesc->numframes <= 1)
	{
		f = 0;
	}
	else
	{
		f = (m_pCurrentEntity->curstate.frame * (pseqdesc->numframes - 1)) / 256.0;
	}
 	
	f += dfdt;

	if (pseqdesc->flags & STUDIO_LOOPING) 
	{
		if (pseqdesc->numframes > 1)
		{
			f -= (int)(f / (pseqdesc->numframes - 1)) *  (pseqdesc->numframes - 1);
		}
		if (f < 0) 
		{
			f += (pseqdesc->numframes - 1);
		}
	}
	else 
	{
		if (f >= pseqdesc->numframes - 1.001) 
		{
			f = pseqdesc->numframes - 1.001;
		}
		if (f < 0.0) 
		{
			f = 0.0;
		}
	}
	return (float)f;
}

/* ---- one sequence's pose, its blends slerped (StudioSetupBones) ------------- */

static float	pos1[GS_MAXBONES][3], pos2[GS_MAXBONES][3], pos3[GS_MAXBONES][3];
static float	pos4[GS_MAXBONES][3], pos1b[GS_MAXBONES][3];
static vec4_t	q1[GS_MAXBONES], q2[GS_MAXBONES], q3[GS_MAXBONES], q4[GS_MAXBONES];
static vec4_t	q1b[GS_MAXBONES];

static void seq_pose(const sfmt_info_t *hdr, const sfmt_seq_t *pseqdesc, const uint16_t *chunk,
	float f, const float *adj, const float s[2], float (*pos)[3], vec4_t *q)
{
	int nb = hdr->numbones;

	StudioCalcRotations(hdr, pos, q, pseqdesc, chunk, 0, f, adj);
	if (pseqdesc->numblends > 1)
	{
		StudioCalcRotations(hdr, pos2, q2, pseqdesc, chunk, 1, f, adj);
		StudioSlerpBones(nb, q, pos, q2, pos2, s[0]);
		if (pseqdesc->numblends == 4)
		{
			StudioCalcRotations(hdr, pos3, q3, pseqdesc, chunk, 2, f, adj);
			StudioCalcRotations(hdr, pos4, q4, pseqdesc, chunk, 3, f, adj);
			StudioSlerpBones(nb, q3, pos3, q4, pos4, s[0]);
			StudioSlerpBones(nb, q, pos, q3, pos3, s[1]);
		}
	}
}

/* Bone matrices from (pos, q): root bones under `root` (NULL: model space). */
static void build_matrices(const sfmt_info_t *hdr, const float (*root)[4], float (*out)[3][4])
{
	const sfmt_bone_t *pbones = SFMT_BONES(hdr);
	float bonematrix[3][4];
	int i;

	for (i = 0; i < hdr->numbones; i++)
	{
		QuaternionMatrix(q1[i], bonematrix);
		bonematrix[0][3] = pos1[i][0];
		bonematrix[1][3] = pos1[i][1];
		bonematrix[2][3] = pos1[i][2];
		if (pbones[i].parent == -1)
		{
			if (root)
				ConcatTransforms(root, (const float (*)[4])bonematrix, out[i]);
			else
				memcpy(out[i], bonematrix, sizeof(bonematrix));
		}
		else
			ConcatTransforms((const float (*)[4])out[pbones[i].parent],
				(const float (*)[4])bonematrix, out[i]);
	}
}

/* ---- server pose (memoised) -------------------------------------------------- */

/* 4: an islave zap's two beams pose the slave (attachments) and the
 * entities its traces pass by, a shotgun's pellets the same few
 * entities; 2 entries thrashed between three poses */
#define SV_CACHE	4

typedef struct {
	game_pose_t	in;
	int		n;
	int		valid;
	float		bones[GS_MAXBONES][3][4];
} pose_memo_t;

/* the game's memo: the renderer reads it but never fills it */
static pose_memo_t	sv_cache[SV_CACHE];
static int		sv_next;
uint32_t		game_studio_pose_bones;

static int memo_get(const pose_memo_t *m, const game_pose_t *p, float (*out)[3][4])
{
	if (!m->valid || memcmp(&m->in, p, sizeof(*p)))
		return 0;
	memcpy(out, m->bones, sizeof(out[0]) * (size_t)m->n);
	return m->n;
}

static void memo_put(pose_memo_t *m, const game_pose_t *p, int n, float (*out)[3][4])
{
	m->in = *p;
	m->n = n;
	m->valid = 1;
	memcpy(m->bones, out, sizeof(out[0]) * (size_t)n);
}

/* the pose of p (hdr its info); 0 when its chunk is missing (a bad file) */
static int pose_compute(const sfmt_info_t *hdr, const game_pose_t *p, float (*out)[3][4])
{
	const sfmt_seq_t *pseqdesc;
	const uint16_t *chunk;
	float adj[8], s[2], f, angles[3], rot[3][4];
	gs_clent_t cl;
	int seq, i;

	seq = p->sequence < hdr->numseq ? p->sequence : 0;
	pseqdesc = &SFMT_SEQS(hdr)[seq];
	chunk = studio_anim_now(p->model, seq, NULL);
	cl.curstate.frame = p->frame;
	cl.curstate.animtime = 0;
	cl.curstate.framerate = 0;
	f = gs_estimate_frame(pseqdesc, &cl, 0, 0);
	StudioCalcBoneAdj(hdr, 1.0f, adj, p->controller, p->controller, 0);
	s[0] = p->blending[0] / 255.0f;
	s[1] = p->blending[1] / 255.0f;
	seq_pose(hdr, pseqdesc, chunk, f, adj, s, pos1, q1);

	angles[PITCH] = -p->angles[PITCH];
	angles[YAW] = p->angles[YAW];
	angles[ROLL] = p->angles[ROLL];
	AngleMatrix(angles, rot);
	for (i = 0; i < 3; i++)
		rot[i][3] = p->origin[i];
	build_matrices(hdr, (const float (*)[4])rot, out);
	return chunk != NULL;
}

static const sfmt_info_t *pose_info(const game_pose_t *p, int max)
{
	const sfmt_info_t *hdr = gs_info(p->model);

	if (!hdr || !hdr->numbones || hdr->numbones > max || !hdr->numseq)
		return NULL;
	return hdr;
}

/* The game's poses (hitboxes, attachments, bone positions), memoised:
 * only game code uses the memo, so its hits and the bones computed
 * (game_studio_pose_bones, the AI work units) are the same on the host
 * and the cart. */
int game_studio_pose(const game_pose_t *p, float (*out)[3][4], int max)
{
	const sfmt_info_t *hdr = pose_info(p, max);
	int c, n;

	if (!hdr)
		return 0;
	for (c = 0; c < SV_CACHE; c++)
		if ((n = memo_get(&sv_cache[c], p, out)) != 0)
			return n;
	game_studio_pose_bones += (uint32_t)hdr->numbones;
	/* a pose missing its chunk (a bad file) is not memoised */
	if (pose_compute(hdr, p, out))
	{
		memo_put(&sv_cache[sv_next], p, hdr->numbones, out);
		sv_next = (sv_next + 1) % SV_CACHE;
	}
	return hdr->numbones;
}

int game_studio_pose_draw(const game_pose_t *p, float (*out)[3][4], int max)
{
	const sfmt_info_t *hdr = pose_info(p, max);
	int c, n;

	if (!hdr)
		return 0;
	for (c = 0; c < SV_CACHE; c++)
		if ((n = memo_get(&sv_cache[c], p, out)) != 0)
			return n;
	pose_compute(hdr, p, out);
	return hdr->numbones;
}

/* ---- hitbox reach (the hitbox source's line cull) ----------------------------- */

/* How far game_studio_pose can put a hitbox point from the pose origin:
 * every bone matrix is a rotation (a slerped or nearly-equal-lerped unit
 * quaternion: norm <= 1) with the bone's translation, so a bone's origin
 * lies within the sum of its chain's translation lengths and a hitbox
 * point within its corner's length of that. A translation is the bone's
 * default plus a stored animation value times its scale (frame
 * interpolation and blends mix stored values convexly; the -0.01 frame
 * clamp extrapolates by 1 %) plus a controller's adjustment (at most
 * max(|start|, |end|), + 360 for a wrapping one). The result carries 3 %
 * + 2 units for the float rounding of the pose and the hitbox walk's
 * plane epsilon. */
#define REACH_CACHE	32

static struct {
	res_t		model;
	uint16_t	seq;
	float		reach;
} reach_c[REACH_CACHE];
static int	reach_n, reach_next;

/* The largest |stored value| of the channel whose run list starts at word
 * w (the runs covering frames 0 .. numframes - 1; a lookup's read into the
 * next run past them has weight 0); -1 on a run list the walk can't bound
 * (a run without values, more values than frames, past the chunk). */
static int channel_max(const uint16_t *chunk, uint32_t words, uint32_t w, int numframes)
{
	const int16_t *p = (const int16_t *)chunk + w, *end = (const int16_t *)chunk + words;
	int cum = 0, k, valid, total, m = 0, v;

	while (cum < numframes)
	{
		if (p >= end)
			return -1;
		valid = SFMT_VALID(p[0]);
		total = SFMT_TOTAL(p[0]);
		if (!valid || total < valid || p + valid >= end)
			return -1;
		for (k = 1; k <= valid; k++)
		{
			v = p[k] < 0 ? -p[k] : p[k];
			if (v > m)
				m = v;
		}
		cum += total;
		p += valid + 1;
	}
	return m;
}

static float hitbox_reach(const sfmt_info_t *hdr, int seq, const uint16_t *chunk, uint32_t size)
{
	const sfmt_seq_t *pseqdesc = &SFMT_SEQS(hdr)[seq];
	const sfmt_bone_t *pbone = SFMT_BONES(hdr);
	const sfmt_ctl_t *pctl = SFMT_CTLS(hdr);
	const sfmt_hitbox_t *x = SFMT_HITBOXES(hdr);
	float chain[GS_MAXBONES], reach = 0;
	uint32_t words = size / 2;
	int nb = hdr->numbones, blends = pseqdesc->numblends, i, j, b, c, m;

	if (blends < 1)
		blends = 1;
	if (blends > 4)
		blends = 4;
	if (chunk && (uint32_t)blends * (uint32_t)nb * 6u > words)
		return -1;
	for (i = 0; i < nb; i++, pbone++)
	{
		float len2 = 0;

		if (pbone->parent >= i)
			return -1;
		for (j = 0; j < 3; j++)
		{
			float mj = 0, a;

			for (b = 0; b < blends; b++)
			{
				uint16_t w = chunk ? chunk[(b * nb + i) * 6 + j] : 0;

				a = fabsf(pbone->value[j]);
				if (w)
				{
					if ((m = channel_max(chunk, words, w, pseqdesc->numframes)) < 0)
						return -1;
					a += (float)m * fabsf(pbone->scale[j]) * 1.02f;
				}
				if (a > mj)
					mj = a;
			}
			if ((c = pbone->ctl[j]) != -1)
			{
				int t;

				if (c < 0 || c >= hdr->numctl || c >= 8)
					return -1;
				t = pctl[c].type & STUDIO_TYPES;
				if (t != STUDIO_X && t != STUDIO_Y && t != STUDIO_Z && t != STUDIO_XR
					&& t != STUDIO_YR && t != STUDIO_ZR)
					return -1;
				a = fabsf(pctl[c].start) > fabsf(pctl[c].end) ? fabsf(pctl[c].start)
					: fabsf(pctl[c].end);
				mj += a + ((pctl[c].type & STUDIO_RLOOP) ? 360.0f : 0.0f);
			}
			len2 += mj * mj;
		}
		chain[i] = sqrtf(len2) + (pbone->parent >= 0 ? chain[pbone->parent] : 0);
	}
	for (i = 0; i < hdr->numhitboxes; i++)
	{
		float c2 = 0, r;

		if (x[i].bone >= nb)
			return -1;
		for (j = 0; j < 3; j++)
		{
			float a = fabsf(x[i].bbmin[j]) > fabsf(x[i].bbmax[j]) ? fabsf(x[i].bbmin[j])
				: fabsf(x[i].bbmax[j]);

			c2 += a * a;
		}
		r = chain[x[i].bone] + sqrtf(c2);
		if (r > reach)
			reach = r;
	}
	return reach * 1.03f + 2.0f;
}

float game_studio_hitbox_reach(res_t model, int sequence)
{
	const sfmt_info_t *hdr = gs_info(model);
	const uint16_t *chunk;
	uint32_t size = 0;
	float r;
	int seq, i;

	if (!hdr || !hdr->numbones || hdr->numbones > GS_MAXBONES || !hdr->numseq)
		return -1;
	seq = sequence < hdr->numseq ? sequence : 0;
	for (i = 0; i < reach_n; i++)
		if (reach_c[i].model == model && reach_c[i].seq == seq)
			return reach_c[i].reach;
	chunk = studio_anim_now(model, seq, &size);
	if (!chunk && SFMT_SEQS(hdr)[seq].anim_size)
		return -1;	/* not resident: no bound (not memoised) */
	r = hitbox_reach(hdr, seq, chunk, chunk ? size : 0);
	i = reach_n < REACH_CACHE ? reach_n++ : reach_next++ % REACH_CACHE;
	reach_c[i].model = model;
	reach_c[i].seq = (uint16_t)seq;
	reach_c[i].reach = r;
	return r;
}

static float	ent_bones[GS_MAXBONES][3][4];

int gs_entity_pose(int e, const float (**bones)[3][4])
{
	const ent_rnd_t *r;
	const col_ent_t *cv;
	game_pose_t p;
	int n;

	*bones = (const float (*)[3][4])ent_bones;
	if (!ent_is_live(e))
		return 0;
	r = ent_rnd(e);
	cv = ent_cv(e);
	memset(&p, 0, sizeof(p));
	p.model = r->model;
	p.sequence = r->sequence;
	p.frame = r->frame;
	memcpy(p.origin, cv->origin, sizeof(p.origin));
	memcpy(p.angles, cv->angles, sizeof(p.angles));
	memcpy(p.controller, r->controller, sizeof(p.controller));
	memcpy(p.blending, r->blending, sizeof(p.blending));
	n = game_studio_pose(&p, ent_bones, GS_MAXBONES);
	return n;
}

/* ---- client pose (latched per entity) ------------------------------------------ */

/* cl_entity_t's latched state, per slot. */
typedef struct {
	uint16_t	serial;
	res_t		model;
	uint16_t	seq, prevseq;
	uint16_t	heldseq;	/* last sequence whose chunk was resident */
	uint8_t		valid, held_ok;
	float		animtime, prevanimtime;
	float		sequencetime;
	float		prevframe, heldframe;
	uint8_t		ctl[4], prevctl[4];
	uint8_t		blend[2], prevblend[2], prevseqblend[2];
	uint8_t		ev_ok, pad;
	uint16_t	evseq;		/* client events: sequence and frame */
	float		evframe;	/* of the last draw */
} latch_t;

static latch_t	latch[HL64_MAX_ENTITIES];

static void latch_update(latch_t *L, int e, const ent_rnd_t *r, double time)
{
	uint16_t serial = ent_idc(e)->serial;

	if (!L->valid || L->serial != serial || L->model != r->model)
	{
		memset(L, 0, sizeof(*L));
		L->valid = 1;
		L->serial = serial;
		L->model = r->model;
		L->seq = L->prevseq = r->sequence;
		L->animtime = L->prevanimtime = r->animtime;
		memcpy(L->ctl, r->controller, 4);
		memcpy(L->prevctl, r->controller, 4);
		memcpy(L->blend, r->blending, 2);
		memcpy(L->prevblend, r->blending, 2);
		return;
	}
	/* a new state of the entity (the client latches the old one) */
	if (r->animtime != L->animtime)
	{
		L->prevanimtime = L->animtime;
		memcpy(L->prevctl, L->ctl, 4);
		memcpy(L->prevblend, L->blend, 2);
		L->animtime = r->animtime;
	}
	memcpy(L->ctl, r->controller, 4);
	memcpy(L->blend, r->blending, 2);
	if (r->sequence != L->seq)
	{
		L->prevseq = L->seq;
		L->sequencetime = r->animtime > 0 ? r->animtime : (float)time;
		memcpy(L->prevseqblend, L->blend, 2);
		L->seq = r->sequence;
	}
}

/* StudioEstimateInterpolant */
static float estimate_interpolant(const latch_t *L, double time)
{
	float dadt = 1.0f;

	if (L->animtime >= L->prevanimtime + 0.01f)
	{
		dadt = (float)((time - L->animtime) / 0.1);
		if (dadt > 2.0f)
			dadt = 2.0f;
	}
	return dadt;
}

/* Client events (event >= 5000) of the drawn sequence whose frame the pose
 * passed since the last draw: (last, f], a new sequence from -0.01, a
 * looping wrap as (last, end] + [0, f]. */
static void client_events(int e, const sfmt_info_t *hdr, int seq, latch_t *L, float f)
{
	const sfmt_seq_t *s = &SFMT_SEQS(hdr)[seq];
	const sfmt_event_t *ev = SFMT_EVENTS(hdr) + s->event_first;
	float start;
	int i;

	if (!L->ev_ok || L->evseq != seq)
		start = -0.01f;
	else
		start = L->evframe;
	L->ev_ok = 1;
	L->evseq = (uint16_t)seq;
	L->evframe = f;
	if (f == start)
		return;
	for (i = 0; i < s->numevents; i++)
	{
		float fr = (float)ev[i].frame;

		if (ev[i].event < 5000)
			continue;
		if (f > start ? (fr > start && fr <= f) : (fr > start || fr <= f))
			game_studio_event(e, ev[i].event, SFMT_STR(hdr, ev[i].options));
	}
}

int game_studio_setup_bones(int e, float (*out)[3][4], int max)
{
	const ent_rnd_t *r;
	const sfmt_info_t *hdr;
	const sfmt_seq_t *seqs, *pseqdesc;
	const uint16_t *chunk;
	latch_t *L;
	double time = eng_time();
	float adj[8], s[2], f, dadt;
	int seq;

	if (!ent_is_live(e))
		return 0;
	r = ent_rnd(e);
	hdr = gs_info(r->model);
	if (!hdr || !hdr->numbones || hdr->numbones > max || !hdr->numseq)
		return 0;
	seqs = SFMT_SEQS(hdr);
	L = &latch[e];
	latch_update(L, e, r, time);

	seq = r->sequence < hdr->numseq ? r->sequence : 0;
	pseqdesc = &seqs[seq];
	chunk = studio_anim(r->model, seq, NULL);
	dadt = estimate_interpolant(L, time);
	StudioCalcBoneAdj(hdr, dadt, adj, L->ctl, L->prevctl, 0);
	if (chunk)
	{
		gs_clent_t cl;

		cl.curstate.frame = r->frame;
		cl.curstate.animtime = r->animtime;
		cl.curstate.framerate = r->framerate;
		f = gs_estimate_frame(pseqdesc, &cl, time, 1);
		s[0] = (L->blend[0] * dadt + L->prevblend[0] * (1.0f - dadt)) / 255.0f;
		s[1] = (L->blend[1] * dadt + L->prevblend[1] * (1.0f - dadt)) / 255.0f;
		seq_pose(hdr, pseqdesc, chunk, f, adj, s, pos1, q1);
		L->heldseq = (uint16_t)seq;
		L->heldframe = f;
		L->held_ok = 1;
		client_events(e, hdr, seq, L, f);
	}
	else
	{
		/* not resident (requested): hold the last pose shown */
		const uint16_t *hc = NULL;

		if (L->held_ok && L->heldseq < hdr->numseq)
			hc = studio_anim(r->model, L->heldseq, NULL);
		s[0] = L->blend[0] / 255.0f;
		s[1] = L->blend[1] / 255.0f;
		pseqdesc = &seqs[hc ? L->heldseq : seq];
		seq_pose(hdr, pseqdesc, hc, hc ? L->heldframe : 0, adj, s, pos1, q1);
		build_matrices(hdr, NULL, out);
		return hdr->numbones;
	}

	if (L->sequencetime != 0 && L->sequencetime + 0.2 > time && L->prevseq < hdr->numseq)
	{
		/* blend from last sequence */
		const sfmt_seq_t *pprev = &seqs[L->prevseq];
		const uint16_t *pc = studio_anim(r->model, L->prevseq, NULL);
		float sb[2], sw;

		if (pc)
		{
			sb[0] = L->prevseqblend[0] / 255.0f;
			sb[1] = L->prevseqblend[1] / 255.0f;
			seq_pose(hdr, pprev, pc, L->prevframe, adj, sb, pos1b, q1b);
			sw = 1.0f - (float)((time - L->sequencetime) / 0.2);
			StudioSlerpBones(hdr->numbones, q1, pos1, q1b, pos1b, sw);
		}
	}
	else
		L->prevframe = f;
	build_matrices(hdr, NULL, out);
	return hdr->numbones;
}

/* ---- chrome (HLSDK utils/mdlviewer/studio_render.cpp StudioModel::Chrome) ---- */

static void VectorNormalize(float *v)
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

static void CrossProduct(const float *v1, const float *v2, float *cross)
{
	cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
	cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
	cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

static void VectorIRotate(const float *in1, const float in2[3][4], float *out)
{
	out[0] = in1[0] * in2[0][0] + in1[1] * in2[1][0] + in1[2] * in2[2][0];
	out[1] = in1[0] * in2[0][1] + in1[1] * in2[1][1] + in1[2] * in2[2][1];
	out[2] = in1[0] * in2[0][2] + in1[1] * in2[1][2] + in1[2] * in2[2][2];
}

void game_studio_chrome_axes(const float (*bones)[3][4], int nbones, const float vright[3],
	float (*up)[3], float (*right)[3])
{
	int bone;

	for (bone = 0; bone < nbones; bone++)
	{
		// calculate vectors from the viewer to the bone. This roughly adjusts for position
		float chromeupvec[3];		// g_chrome t vector in world reference frame
		float chromerightvec[3];	// g_chrome s vector in world reference frame
		float tmp[3];			// vector pointing at bone in world reference frame

		/* the viewer is at the origin of the bones' frame */
		tmp[0] = bones[bone][0][3];
		tmp[1] = bones[bone][1][3];
		tmp[2] = bones[bone][2][3];
		VectorNormalize(tmp);
		CrossProduct(tmp, vright, chromeupvec);
		VectorNormalize(chromeupvec);
		CrossProduct(tmp, chromeupvec, chromerightvec);
		VectorNormalize(chromerightvec);

		VectorIRotate(chromeupvec, bones[bone], up[bone]);
		VectorIRotate(chromerightvec, bones[bone], right[bone]);
	}
}

void game_studio_chrome_st(const float up[3], const float right[3], const float normal[3],
	float st[2])
{
	float n;

	// calc s coord
	n = normal[0] * right[0] + normal[1] * right[1] + normal[2] * right[2];
	st[0] = (n + 1.0f) * 32;

	// calc t coord
	n = normal[0] * up[0] + normal[1] * up[1] + normal[2] * up[2];
	st[1] = (n + 1.0f) * 32;
}
