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
// Hooks the patched HLSDK game DLL calls (test/oracle/hlsdk-hooks.patch).
// The probe observes retail GoldSrc (HLDS) through the engine interface
// only: it calls engine functions on probe entities and fake clients and
// logs inputs and results as JSONL fixtures. Nothing here is engine code.

#ifndef HL64_PROBE_H
#define HL64_PROBE_H

void Probe_Init( void );		// GameDLLInit: register the probe commands
void Probe_Precache( void );	// CWorld::Precache: models the captures spawn
void Probe_StartFrame( void );	// StartFrame: drive the scheduled captures
extern "C" void Probe_PM_Move( struct playermove_s *ppmove, int server );	// wraps PM_Move

#endif
