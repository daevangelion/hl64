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
 * player.h -- the player (HLSDK dlls/player.h, weapons.h constants and
 * CBasePlayer / CBasePlayerWeapon members) on the hl64 player: entity
 * slot 1, the engine's player block (ent_player_t: origin, velocity,
 * view_ofs, punchangle, v_angle, flags, movetype, waterlevel) and the
 * game state below.
 *
 * Slot 1's private block is the CBaseMonster chain (CBasePlayer derives
 * from it; GameMon's code reads health, takedamage, deadflag, view_ofs,
 * m_bitsDamageType through it). The CBasePlayer members proper -- weapons,
 * ammo, suit queue, flashlight, time-based damage bookkeeping -- are one
 * static block (single player; the private slab's 1024 B size class
 * cannot hold both): Player().
 *
 * Weapons are not entities while carried: HLSDK attaches the picked-up
 * weapon entity to the player (MOVETYPE_FOLLOW, EF_NODRAW); here its
 * CBasePlayerWeapon members live in the player block by weapon id and the
 * world entity is removed. Timers follow HLSDK's CLIENT_WEAPONS build
 * (UTIL_WeaponTimeBase() = 0, the weapon and m_flNextAttack timers
 * decrement in PostThink).
 */
#ifndef HL64_GAME_PLAYER_H
#define HL64_GAME_PLAYER_H

#include "engine/core/sys.h"
#include "engine/phys/player.h"
#include "game/hl/monster.h"

/* ---- weapons.h ------------------------------------------------------------- */

#define ITEM_HEALTHKIT		1
#define ITEM_ANTIDOTE		2
#define ITEM_SECURITY		3
#define ITEM_BATTERY		4
#define MAX_ITEMS		5

#define WEAPON_NONE		0
#define WEAPON_CROWBAR		1
#define WEAPON_GLOCK		2
#define WEAPON_PYTHON		3
#define WEAPON_MP5		4
#define WEAPON_SHOTGUN		7
#define WEAPON_HANDGRENADE	12
#define WEAPON_TRIPMINE		13
#define WEAPON_SUIT		31
#define MAX_WEAPONS		32
#define MAX_NORMAL_BATTERY	100

#define CROWBAR_WEIGHT		0
#define GLOCK_WEIGHT		10
#define PYTHON_WEIGHT		15
#define MP5_WEIGHT		15
#define SHOTGUN_WEIGHT		15
#define HANDGRENADE_WEIGHT	5
#define TRIPMINE_WEIGHT		(-10)

#define _9MM_MAX_CARRY		250
#define _357_MAX_CARRY		36
#define BUCKSHOT_MAX_CARRY	125
#define HANDGRENADE_MAX_CARRY	10
#define M203_GRENADE_MAX_CARRY	10
#define TRIPMINE_MAX_CARRY	5

#define WEAPON_NOCLIP		(-1)
#define GLOCK_MAX_CLIP		17
#define PYTHON_MAX_CLIP		6
#define MP5_MAX_CLIP		50
#define SHOTGUN_MAX_CLIP	8

#define GLOCK_DEFAULT_GIVE	17
#define PYTHON_DEFAULT_GIVE	6
#define MP5_DEFAULT_GIVE	25
#define MP5_M203_DEFAULT_GIVE	0
#define SHOTGUN_DEFAULT_GIVE	12
#define HANDGRENADE_DEFAULT_GIVE 5
#define TRIPMINE_DEFAULT_GIVE	1

#define AMMO_GLOCKCLIP_GIVE	GLOCK_MAX_CLIP
#define AMMO_357BOX_GIVE	PYTHON_MAX_CLIP
#define AMMO_MP5CLIP_GIVE	MP5_MAX_CLIP
#define AMMO_CHAINBOX_GIVE	200
#define AMMO_M203BOX_GIVE	2
#define AMMO_BUCKSHOTBOX_GIVE	12

/* weapons.h gun volumes / flashes */
#define LOUD_GUN_VOLUME		1000
#define NORMAL_GUN_VOLUME	600
#define QUIET_GUN_VOLUME	200
#define BRIGHT_GUN_FLASH	512
#define NORMAL_GUN_FLASH	256
#define DIM_GUN_FLASH		128

/* CBasePlayer::m_iHideHUD (cdll_dll.h HIDEHUD_*) */
#define HIDEHUD_WEAPONS		(1 << 0)

/* BULLET_*: game/hl/monster.h (weapons.h Bullet) */

#define ITEM_FLAG_SELECTONEMPTY		1
#define ITEM_FLAG_NOAUTORELOAD		2
#define ITEM_FLAG_NOAUTOSWITCHEMPTY	4
#define ITEM_FLAG_LIMITINWORLD		8
#define ITEM_FLAG_EXHAUSTIBLE		16
#define ITEM_FLAG_NOAUTOSWITCHTO	32

/* Ammo types (HLSDK AmmoInfoArray index; 0 = none). */
#define AMMO_NONE		0
#define AMMO_9MM		1	/* "9mm" */
#define AMMO_357		2	/* "357" */
#define AMMO_BUCKSHOT		3	/* "buckshot" */
#define AMMO_ARGRENADES		4	/* "ARgrenades" */
#define AMMO_HANDGRENADE	5	/* "Hand Grenade" */
#define AMMO_TRIPMINE		6	/* "Trip Mine" */
#define MAX_AMMO_TYPES		7

/* CBasePlayerItem::ItemInfo (GetItemInfo) of a weapon id. */
typedef struct {
	int8_t		iSlot, iPosition;	/* HUD bucket, position */
	int8_t		iAmmo1, iAmmo2;		/* AMMO_* */
	int16_t		iMaxAmmo1, iMaxAmmo2;
	int16_t		iMaxClip;
	int8_t		iFlags;
	int8_t		iWeight;
	int16_t		iDefaultAmmo;		/* m_iDefaultAmmo of a fresh pickup */
	cls_t		cls;			/* the weapon_* class */
	res_t		vmodel, pmodel, wmodel;	/* view / player / world model */
} ItemInfo;
/* By weapon id; iId 0 rows unused. */
const ItemInfo	*ItemInfoOf(int id);
/* CBasePlayerItem::MaxAmmoCarry: the carry limit the registered weapons
 * give ammo type ammo (their iMaxAmmo1 / iMaxAmmo2), -1 when no weapon of
 * the game uses it. */
int		MaxAmmoCarry(int ammo);

/* ---- player.h --------------------------------------------------------------- */

#define PLAYER_FATAL_FALL_SPEED		1024
#define PLAYER_MAX_SAFE_FALL_SPEED	580
#define DAMAGE_FOR_FALL_SPEED		((float)100 / (PLAYER_FATAL_FALL_SPEED - PLAYER_MAX_SAFE_FALL_SPEED))
#define PLAYER_MIN_BOUNCE_SPEED		200
#define PLAYER_FALL_PUNCH_THRESHHOLD	(float)350

#define PFLAG_ONLADDER		(1 << 0)
#define PFLAG_ONTRAIN		(1 << 1)
#define PFLAG_ONBARNACLE	(1 << 2)
#define PFLAG_DUCKING		(1 << 3)
#define PFLAG_USING		(1 << 4)

#define CSUITPLAYLIST		4
#define CSUITNOREPEAT		32
#define SUIT_NEXT_IN_30SEC	30
#define SUIT_NEXT_IN_1MIN	60
#define SUIT_NEXT_IN_5MIN	300
#define SUIT_NEXT_IN_10MIN	600
#define SUIT_NEXT_IN_30MIN	1800
#define SUIT_NEXT_IN_1HOUR	3600

/* cbase.h time-based damage indices */
#define itbd_Paralyze		0
#define itbd_NerveGas		1
#define itbd_Poison		2
#define itbd_Radiation		3
#define itbd_DrownRecover	4
#define itbd_Acid		5
#define itbd_SlowBurn		6
#define itbd_SlowFreeze		7

/* CBasePlayerWeapon members of one carried weapon (by weapon id). */
typedef struct {
	int16_t		m_iClip;
	uint8_t		m_fInReload;
	uint8_t		m_fInSpecialReload;	/* shotgun */
	uint8_t		m_iPlayEmptySound;
	uint8_t		m_fFireOnEmpty;
	uint8_t		m_iSwing;		/* crowbar */
	uint8_t		pad;
	float		m_flNextPrimaryAttack;	/* decrementing (CLIENT_WEAPONS) */
	float		m_flNextSecondaryAttack;
	float		m_flTimeWeaponIdle;
	float		m_flLastFireTime;	/* gpGlobals->time */
	float		m_flPrevPrimaryAttack;
	float		m_flPumpTime;		/* shotgun: gpGlobals->time */
	float		m_flNextReload;		/* shotgun */
	float		m_flStartThrow;		/* hand grenade: gpGlobals->time */
	float		m_flReleaseThrow;
	/* the weapon entity's own think (crowbar Smack / SwingAgain):
	 * gpGlobals->time it runs at, 0 none */
	float		m_flThinkTime;
	uint8_t		m_iThink;
	uint8_t		pad2[3];
	col_trace_t	m_trHit;		/* crowbar */
} CBasePlayerWeapon;

/* Weapon ids carried in the game (index into Player()->wpn). */
#define WPN_SLOTS		14	/* ids 0..WEAPON_TRIPMINE */

typedef struct {
	/* buttons (pev->button, debounced) and the command */
	uint16_t	button;
	uint16_t	m_afButtonLast, m_afButtonPressed, m_afButtonReleased;
	uint8_t		impulse;
	uint8_t		m_afPhysicsFlags;	/* PFLAG_* */
	uint8_t		m_fLongJump;
	uint8_t		m_fInitHUD;
	uint32_t	weapons;		/* pev->weapons: 1 << WEAPON_* */
	uint32_t	random_seed;
	float		armorvalue;
	float		air_finished, pain_finished, dmg;	/* WaterMove */
	float		dmgtime;
	int		m_idrowndmg, m_idrownrestored;
	float		m_tbdPrev;
	int		m_bitsHUDDamage;
	float		dmg_take, dmg_save;
	ent_h		dmg_inflictor;
	float		m_flRespawnTimer, m_fDeadTime;
	/* -velocity.z while in the air, set in PreThink (pmove's own
	 * flFallVelocity is zeroed when it lands, before PostThink) */
	float		m_flFallVelocity;
	/* suit */
	res_t		m_rgSuitPlayList[CSUITPLAYLIST];
	res_t		m_rgiSuitNoRepeat[CSUITNOREPEAT];
	float		m_rgflSuitNoRepeatTime[CSUITNOREPEAT];
	int		m_iSuitPlayNext;
	float		m_flSuitUpdate;
	uint8_t		m_rgItems[MAX_ITEMS];
	/* flashlight */
	uint8_t		m_iFlashBattery;
	uint8_t		pad[2];
	float		m_flFlashLightTime;
	/* weapons */
	float		m_flNextAttack;		/* decrementing (CLIENT_WEAPONS) */
	uint8_t		m_pActiveItem, m_pLastItem;	/* weapon ids, 0 none */
	uint8_t		m_iHideHUD;		/* HIDEHUD_* */
	uint8_t		pad3;
	/* the tank the player is controlling (game/hl/tanks.c), EH_NONE */
	ent_h		m_pTank;
	/* UpdatePlayerSound */
	int16_t		m_iWeaponVolume, m_iWeaponFlash, m_iTargetVolume, m_iExtraSoundTypes;
	float		m_flStopExtraSoundTime;
	int16_t		m_rgAmmo[MAX_AMMO_TYPES];
	CBasePlayerWeapon wpn[WPN_SLOTS];
	/* view model (pev->viewmodel / weaponanim; client weapon animation) */
	res_t		viewmodel;
	uint8_t		weaponanim, viewbody;
	float		weaponanim_time;	/* gpGlobals->time the sequence started */
	/* the player block's game-written fields as last copied to the
	 * components (flags / velocity deltas the game made in between are
	 * applied to the player block at the next command) */
	uint32_t	sync_flags;
	float		sync_velocity[3];
} CBasePlayer;

/* dispatch.c: the classes.def name of a class id (logs) */
const char	*game_class_name(cls_t cls);

/* The player's state (single player); valid after game_player_spawn. */
CBasePlayer	*Player(void);
/* Slot 1's CBaseMonster block, NULL before the player spawned. */
CBaseMonster	*PlayerMon(void);
int		Player_IsAlive(void);

/* ---- CBasePlayer methods other modules call -------------------------------- */

/* SetSuitUpdate(name, fgroup, iNoRepeatTime): a SENTENCE or GROUP res;
 * RES_NONE clears the queue. */
void	Player_SetSuitUpdate(res_t sentence, int iNoRepeatTime);
/* EMIT_SOUND_SUIT / EMIT_GROUPNAME_SUIT (suitvolume 0.25) */
void	Player_SuitSound(res_t sentence);
/* GiveAmmo(iCount, szName, iMax): the ammo index, -1 none taken */
int	Player_GiveAmmo(int count, int ammo, int max);
/* GiveNamedItem: a fresh class entity at the player, touched at once */
void	Player_GiveNamedItem(cls_t cls);
/* RemoveAllItems(removeSuit) */
void	Player_RemoveAllItems(int removeSuit);
/* SelectItem by weapon id; SelectLastItem; HUD slot keys (slot 0-4,
 * cycling inside the bucket); invnext / invprev */
void	Player_SelectItem(int id);
void	Player_SelectLastItem(void);
void	Player_SelectSlot(int slot);
void	Player_SelectNext(int dir);
int	Player_HasWeapon(int id);
/* Weapon pickup (CBasePlayer::AddPlayerItem + AddToPlayer / AddDuplicate):
 * 1 added, 0 a duplicate gave ammo, -1 nothing taken. defammo is the
 * world item's m_iDefaultAmmo (clip ammo of a second-hand weapon when
 * clip >= 0: ExtractClipAmmo). */
int	Player_AddPlayerItem(int id, int defammo, int clip);
/* TakeHealth / armour */
int	Player_TakeHealth(float health, int bits);
/* GameMon (barnacle.cpp): CBasePlayer::FBecomeProne, BarnacleVictimBitten,
 * BarnacleVictimReleased */
int	Player_FBecomeProne(int e);
void	Player_BarnacleVictimBitten(int e, int barnacle);
void	Player_BarnacleVictimReleased(int e);
/* GetGunPosition (origin + view_ofs) and the aim (v_angle + punchangle
 * vectors into gpGlobals) */
void	Player_GunPosition(float out[3]);
void	Player_MakeAimVectors(void);
/* SetAnimation(PLAYER_ATTACK1): third-person only (no player model drawn
 * in first person); kept as the HLSDK call sites' marker. */
#define Player_SetAnimationAttack()	((void)0)

/* ---- weapons.c (CBasePlayerWeapon behaviour) --------------------------------- */

void	W_Precache(void);
void	W_ItemPostFrame(void);
void	W_ItemPreFrame(void);
/* Holster the active weapon, deploy id (SwitchWeapon) */
int	W_Deploy(int id);
void	W_Holster(int id);
int	W_CanDeploy(int id);
int	W_CanHolster(int id);
int	W_IsUseable(int id);
/* SendWeaponAnim */
void	W_SendWeaponAnim(int anim);
/* DefaultDeploy / DefaultReload / PlayEmptySound / ResetEmptySound /
 * GetNextAttackDelay / RetireWeapon of weapon id */
int	W_DefaultDeploy(int id, int anim);
int	W_DefaultReload(int id, int clipsize, int anim, float delay);
void	W_PlayEmptySound(int id);
void	W_ResetEmptySound(int id);
float	W_GetNextAttackDelay(int id, float delay);
void	W_RetireWeapon(int id);
/* CBasePlayer::FireBulletsPlayer: spread of the last shot (x, y) */
void	W_FireBulletsPlayer(int cShots, const float src[3], const float dir[3],
		const float spread[3], float dist, int bullet, int tracerfreq, int damage,
		float out[2]);
/* the client event's effects (ev_hldm.cpp): shell, muzzle flash, punch,
 * tracers and impact decals / texture sounds of each pellet */
void	W_EjectShell(res_t model, float fwd, float up, float right, int soundtype);
void	W_PunchAxis(int axis, float punch);
/* GetAutoaimVector(AUTOAIM_10DEGREES / AUTOAIM_5DEGREES) into out (and
 * gpGlobals' vectors); W_OnTarget: the easy-skill crosshair state */
void	W_AutoaimVector(int degrees10, float out[3]);
int	W_OnTarget(void);
/* UTIL_SharedRandomFloat / Long (util.cpp) */
float	UTIL_SharedRandomFloat(unsigned int seed, float low, float high);
int	UTIL_SharedRandomLong(unsigned int seed, int low, int high);
/* game/hl/material.c (material.h's Bullet enum clashes with monster.h's) */
float	TEXTURETYPE_PlaySound(const col_trace_t *tr, const float src[3], const float end[3],
		int bullet);
/* weapons.cpp DecalGunshot, EjectBrass */
void	DecalGunshot(const col_trace_t *tr, int iBulletType);
void	EjectBrass(const float org[3], const float vel[3], float rotation, res_t model,
		int soundtype);

/* Per-weapon behaviour (weapon_*.c): the CBasePlayerWeapon virtuals. */
typedef struct {
	int	(*Deploy)(int id);
	void	(*Holster)(int id);
	void	(*PrimaryAttack)(int id);
	void	(*SecondaryAttack)(int id);
	void	(*Reload)(int id);
	void	(*WeaponIdle)(int id);
	void	(*ItemPostFrame)(int id);	/* NULL = CBasePlayerWeapon's */
	int	(*CanHolster)(int id);
	void	(*Think)(int id, int which);	/* the weapon entity's think */
} weapon_vt_t;
extern const weapon_vt_t wvt_crowbar, wvt_glock, wvt_python, wvt_mp5, wvt_shotgun,
	wvt_handgrenade, wvt_tripmine;
/* CBasePlayerWeapon::ItemPostFrame (the default) */
void	W_BaseItemPostFrame(int id);

/* ---- skill.cfg (player and item values of the Uplink / HL data) --------------- */

typedef struct {
	float	plrDmgCrowbar, plrDmg9MM, plrDmg357, plrDmgMP5, plrDmgM203Grenade,
		plrDmgBuckshot, plrDmgHandGrenade, plrDmgTripmine;
	float	healthkitCapacity, batteryCapacity, healthchargerCapacity,
		suitchargerCapacity;
	float	plrHead, plrChest, plrStomach, plrArm, plrLeg;
} skilldata_t;
extern skilldata_t	gSkillData;
void	SkillDataInit(void);

/* ---- HUD feed (game/hud: HLSDK user messages) ---------------------------------- */

#define HIST_AMMO	1
#define HIST_WEAP	2
#define HIST_ITEM	3
/* AmmoPickup / WeapPickup / ItemPickup */
void	hud_pickup(int type, int id, int count);
/* Damage message: armour saved, damage taken, the shown damage bits, the
 * source (map-local, the player's frame) */
void	hud_damage(int armor, int damage, int bits, const float from[3]);
/* Flashlight / FlashBat */
void	hud_flashlight(int on, int battery);
/* the active weapon changed (the HUD's selection display) */
void	hud_weapon_select(int id);
/* the geiger range (CHudGeiger) */
void	hud_geiger(int range);
void	hud_reset(void);

#endif
