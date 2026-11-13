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
 * const.h -- the subset of HLSDK common/const.h that pm_shared uses
 * (flags, move types, solid types, contents, sound channel constants,
 * render modes and the basic types). Values unchanged.
 */
#ifndef CONST_H
#define CONST_H

#include "in_buttons.h"

#define	FL_FLY					(1<<0)
#define	FL_SWIM					(1<<1)
#define	FL_CONVEYOR				(1<<2)
#define	FL_CLIENT				(1<<3)
#define	FL_INWATER				(1<<4)
#define	FL_MONSTER				(1<<5)
#define	FL_GODMODE				(1<<6)
#define	FL_NOTARGET				(1<<7)
#define	FL_SKIPLOCALHOST		(1<<8)
#define	FL_ONGROUND				(1<<9)
#define	FL_PARTIALGROUND		(1<<10)
#define	FL_WATERJUMP			(1<<11)
#define FL_FROZEN				(1<<12)
#define FL_FAKECLIENT			(1<<13)
#define FL_DUCKING				(1<<14)
#define FL_FLOAT				(1<<15)
#define FL_GRAPHED				(1<<16)
#define FL_IMMUNE_WATER			(1<<17)
#define	FL_IMMUNE_SLIME			(1<<18)
#define FL_IMMUNE_LAVA			(1<<19)
#define FL_PROXY				(1<<20)
#define FL_ALWAYSTHINK			(1<<21)
#define FL_BASEVELOCITY			(1<<22)
#define FL_MONSTERCLIP			(1<<23)
#define FL_ONTRAIN				(1<<24)
#define FL_WORLDBRUSH			(1<<25)
#define FL_SPECTATOR            (1<<26)

#define	MOVETYPE_NONE			0
#define	MOVETYPE_WALK			3
#define	MOVETYPE_STEP			4
#define	MOVETYPE_FLY			5
#define	MOVETYPE_TOSS			6
#define	MOVETYPE_PUSH			7
#define	MOVETYPE_NOCLIP			8
#define	MOVETYPE_FLYMISSILE		9
#define	MOVETYPE_BOUNCE			10
#define MOVETYPE_BOUNCEMISSILE	11
#define MOVETYPE_FOLLOW			12
#define	MOVETYPE_PUSHSTEP		13

#define	SOLID_NOT				0
#define	SOLID_TRIGGER			1
#define	SOLID_BBOX				2
#define	SOLID_SLIDEBOX			3
#define	SOLID_BSP				4

#define	CONTENTS_EMPTY		-1
#define	CONTENTS_SOLID		-2
#define	CONTENTS_WATER		-3
#define	CONTENTS_SLIME		-4
#define	CONTENTS_LAVA		-5
#define	CONTENTS_SKY		-6
#define	CONTENTS_LADDER		-16

#define CHAN_AUTO			0
#define CHAN_WEAPON			1
#define	CHAN_VOICE			2
#define CHAN_ITEM			3
#define	CHAN_BODY			4

#define ATTN_NONE		0
#define	ATTN_NORM		(float)0.8

#define	PITCH_NORM		100

#define	DEAD_NO				0
#define	DEAD_DYING			1
#define	DEAD_DEAD			2
#define DEAD_RESPAWNABLE	3
#define DEAD_DISCARDBODY	4

enum
{
	kRenderNormal,
	kRenderTransColor,
	kRenderTransTexture,
	kRenderGlow,
	kRenderTransAlpha,
	kRenderTransAdd
};

typedef unsigned char 		byte;

#undef true
#undef false

typedef enum {false, true}	qboolean;

#endif
