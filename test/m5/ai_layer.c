/* SPDX-License-Identifier: MIT */
/*
 * ai_layer.c -- host AI regression driver (test/run/m5.sh suite ai-layer):
 * the two AI faults behind the uplink route's endless grunt fire.
 *
 * hl64-ailayer --root CONTENT --stuck MAP
 *   New Game on MAP (player god, noclip), every record of it woken and
 *   kept live (rec_test_class_sweep), the player in a grunt's clear view
 *   as its enemy, the grunt set to take cover with the think budget at 0:
 *   its think (game/hl/aithink.c) pauses inside the StartTask of
 *   TASK_FIND_COVER_FROM_ENEMY, a task only StartTask ends. Then
 *     refused  game_dehydrate, refused (a monster in combat; the record
 *              layer retries the release every 0.5 s): the paused think
 *              goes on;
 *     drop     Ai_Drop (a map open / close, a twin copy, a dehydrate):
 *              the still-running task is new again at once;
 *   and after each, at the default budget, the task ends within 3 s
 *   (left running, the grunt keeps its fire animation looping for ever).
 *
 * hl64-ailayer --root CONTENT --layer A B
 *   New Game on A (player god, noclip), B's layer and HLE slot opened
 *   beside it, B's records woken, a grunt of B given the player (home A)
 *   as enemy, the player placed at the A-local numbers that, read in B's
 *   frame, stand in the grunt's clear view 200 u ahead (the uplink case:
 *   the player back in hldemo1, a grunt of hldemo2 aiming 950 u away at a
 *   player 4400 u away). For 10 s: FVisible(grunt, player) = 0, a sound
 *   at the player is not on the grunt's audible list, the grunt's enemy
 *   LKP never becomes the player's A-local origin, and it fires nothing
 *   (no round leaves its magazine). Then (RunAI moved the enemy of another
 *   layer into its old-enemy memory): no enemy, the player remembered,
 *   out of combat, PopEnemy keeps the player there, game_dehydrate accepts
 *   the grunt; the player re-based home into B at the grunt's view: the
 *   grunt takes the player back as its enemy within 2 s.
 *
 * Lines `ailayer ...`; failures `ailayer FAIL ...`. Exit 0 when nothing
 * failed, 1 otherwise, 2 on usage / content errors.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/rec.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "game/hl/monster.h"
#include "plat/host/host.h"

static int fails;

#define FAIL(...)	do { fails++; log_printf("ailayer FAIL " __VA_ARGS__); log_flush(); } while (0)

static void wake_slot(int slot)
{
	int lump;

	rec_test_class_sweep(slot);
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);

		if (rec_kind(gid) != REC_KIND_ENTITY || rec_entity(gid) >= 0)
			continue;
		if (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY)
			rec_wake(gid);
	}
}

/* New Game on MAP, the player god and noclip; returns MAP's slot */
static int boot(const char *root, const char *mapname)
{
	dbg_cmd_t dt;
	int map;

	host_io_init(root);
	eng_boot("host");
	map = wld_map_find(mapname);
	if (map < 0)
	{
		fprintf(stderr, "hl64-ailayer: no map %s\n", mapname);
		return -1;
	}
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	memset(&dt, 0, sizeof(dt));
	dt.op = DBG_FIXED_DT;
	dt.nargs = 1;
	dt.arg[0].u = 33333;
	if (dbg_push(&dt))
		return -1;
	game_new_campaign();
	rec_new_campaign(map);
	if (phy_player_spawn_start(map))
	{
		fprintf(stderr, "hl64-ailayer: no player start on %s\n", mapname);
		return -1;
	}
	phy_player()->movetype = PL_MOVE_NOCLIP;
	phy_player()->flags |= PL_FL_GODMODE;
	settings.ai_spread = 1;
	return wld_open(map)->slot;
}

/* a live grunt of slot, not in a script, its nearest-to-clear view first */
static int find_grunt(int slot, float q[3])
{
	static const float dist[] = { 200, 160, 240 };
	int e, k, j;

	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		const col_ent_t *c = ent_cv(e);
		float eye[3];

		if (ent_idc(e)->cls != CLS_MONSTER_HUMAN_GRUNT || ent_idc(e)->layer != slot ||
			!ent_priv(e) || !IsAlive(e) || EHGet(MON(e)->m_pCine) >= 0 ||
			MON(e)->m_MonsterState == MONSTERSTATE_SCRIPT)
			continue;
		eng_set_frame_layer(slot);
		VecAdd(c->origin, MON(e)->view_ofs, eye);
		/* straight ahead first, then 15-degree steps either side */
		for (k = 0; k < 13; k++)
			for (j = 0; j < 3; j++)
			{
				float a = (c->angles[1] + (k & 1 ? 1 : -1) * ((k + 1) / 2) * 15.0f) *
					(float)M_PI / 180.0f;
				col_trace_t tr;

				q[0] = eye[0] + dist[j] * cosf(a);
				q[1] = eye[1] + dist[j] * sinf(a);
				q[2] = eye[2];
				UTIL_TraceLine(eye, q, ignore_monsters, e, &tr);
				if (tr.frac == 1.0f && !tr.startsolid &&
					UTIL_PointContents(q) == CONTENTS_EMPTY)
					return e;
			}
	}
	return -1;
}

/* the player's eyes at q (numbers in the player's home frame) */
static void player_eyes_at(const float q[3])
{
	ent_player_t *pl = phy_player();
	float o[3];
	int layer = eng_frame_layer();

	VecSub(q, pl->view_ofs, o);
	VecCopy(o, pl->origin);
	VecSet(pl->velocity, 0, 0, 0);
	eng_set_frame_layer(ent_idc(ENT_PLAYER)->layer);
	ent_set_origin(ENT_PLAYER, o);
	eng_set_frame_layer(layer);
}

static void make_enemy(int g)
{
	CBaseMonster *m = MON(g);

	m->m_hEnemy = EHANDLE(ENT_PLAYER);
	VecCopy(ent_cv(g)->origin, m->m_vecEnemyLKP);
	m->m_IdealMonsterState = MONSTERSTATE_COMBAT;
	Mon_SetConditions(m, bits_COND_NEW_ENEMY);
}

/* ---- --stuck ------------------------------------------------------------------------------- */

static const Schedule_t *forced;

static int in_cover_task(int g)
{
	const CBaseMonster *m = MON(g);
	const Task_t *t = m->m_pSchedule ? Mon_GetTask(g) : NULL;

	return t && t->iTask == TASK_FIND_COVER_FROM_ENEMY;
}

/* zero budget: run until g's think is paused inside the StartTask of
 * TASK_FIND_COVER_FROM_ENEMY (the task running between frames: only
 * StartTask ends it) */
static int pause_in_start(int g, const float q[3])
{
	CBaseMonster *m = MON(g);
	int f;

	settings.ai_budget = 0;
	for (f = 0; f < 600; f++)
	{
		if (!ent_is_live(g) || !IsAlive(g))
			return 0;
		if (in_cover_task(g) && m->m_iTaskStatus == TASKSTATUS_RUNNING)
			return 1;
		if (m->m_pSchedule != forced)
		{
			/* (again) the cover schedule, the player as its enemy */
			make_enemy(g);
			Mon_SetState(g, MONSTERSTATE_COMBAT);
			forced = Mon_GetScheduleOfType(g, SCHED_TAKE_COVER_FROM_ENEMY);
			Mon_ChangeSchedule(g, forced);
		}
		player_eyes_at(q);
		eng_frame();
		log_flush();
	}
	return 0;
}

/* default budget: the cover task must end (complete, fail, or the
 * schedule change) within 3 s */
static int task_ends(int g, float budget, const char *what)
{
	CBaseMonster *m = MON(g);
	const Schedule_t *s = m->m_pSchedule;
	int i = m->m_iScheduleIndex, f;

	settings.ai_budget = budget;
	for (f = 0; f < 90; f++)
	{
		eng_frame();
		log_flush();
		if (!ent_is_live(g) || m->m_pSchedule != s || m->m_iScheduleIndex != i ||
			m->m_iTaskStatus != TASKSTATUS_RUNNING)
		{
			log_printf("ailayer stuck %s: the cover task ended after %d frames\n", what, f + 1);
			return 1;
		}
	}
	FAIL("stuck %s: grunt %d still in TASK_FIND_COVER_FROM_ENEMY (running) after 3 s\n", what, g);
	return 0;
}

static int stuck_mode(const char *root, const char *mapname)
{
	static uint8_t buf[4096];
	float q[3], budget;
	int slot, g, f;
	uint32_t n;

	if ((slot = boot(root, mapname)) < 0)
		return 2;
	wake_slot(slot);
	budget = settings.ai_budget;
	g = find_grunt(slot, q);
	if (g < 0)
	{
		fprintf(stderr, "hl64-ailayer: no grunt with a clear view on %s\n", mapname);
		return 2;
	}
	log_printf("ailayer stuck: grunt %d at %.0f %.0f %.0f, player eyes at %.0f %.0f %.0f\n", g,
		(double)ent_cv(g)->origin[0], (double)ent_cv(g)->origin[1],
		(double)ent_cv(g)->origin[2], (double)q[0], (double)q[1], (double)q[2]);
	/* past the boot window where thinks run whole (aithink.c AI_MIN_START) */
	for (f = 0; f < 90; f++)
	{
		player_eyes_at(q);
		eng_frame();
	}
	log_flush();

	/* a refused release leaves the paused think alone */
	if (!pause_in_start(g, q))
	{
		FAIL("stuck refused: grunt %d never paused inside the cover StartTask\n", g);
		return 1;
	}
	n = game_dehydrate(g, buf, sizeof buf);
	if (n)
		FAIL("stuck refused: grunt %d in combat dehydrated (%u B)\n", g, (unsigned)n);
	task_ends(g, budget, "refused");

	/* a drop restarts the task it abandoned in StartTask */
	if (!pause_in_start(g, q))
	{
		FAIL("stuck drop: grunt %d never paused inside the cover StartTask\n", g);
		return 1;
	}
	Ai_Drop(g);
	if (MON(g)->m_iTaskStatus != TASKSTATUS_NEW)
		FAIL("stuck drop: grunt %d's cover task status %d after the drop (want new)\n", g,
			MON(g)->m_iTaskStatus);
	task_ends(g, budget, "drop");
	log_printf("ailayer done stuck %s fails=%d\n", mapname, fails);
	log_flush();
	return fails ? 1 : 0;
}

/* ---- --layer -------------------------------------------------------------------------------- */

static int on_audible_list(int g, int snd)
{
	int i, n = 0;

	for (i = MON(g)->m_iAudibleList; i != SOUNDLIST_EMPTY && n < MAX_WORLD_SOUNDS; n++)
	{
		if (i == snd)
			return 1;
		i = CSoundEnt_SoundPointerForIndex(i)->m_iNextAudible;
	}
	return 0;
}

static int remembers_player(int g)
{
	int i;

	for (i = 0; i < MAX_OLD_ENEMIES; i++)
		if (EHGet(MON(g)->m_hOldEnemy[i]) == ENT_PLAYER)
			return 1;
	return 0;
}

/* After the 10 s with the player of another layer as its enemy: the grunt
 * holds the player in its old-enemy memory, not as its enemy, is out of
 * combat and releasable (game_dehydrate accepts it); Mon_PopEnemy keeps
 * the player there while it is in another layer; with the player re-based
 * home into the grunt's map (T 0) at its clear view (B's own numbers), the
 * grunt's RunAI takes the player back as its enemy within 2 s. */
static void left_checks(int g, int sa, int sb, int mapb, const float q[3], const char *mb)
{
	static uint8_t buf[4096];
	static const float T0[3];
	CBaseMonster *m = MON(g);
	uint32_t n;
	int f;

	eng_set_frame_layer(sb);
	log_printf("ailayer left: grunt %d enemy=%d remembered=%d state=%d ideal=%d\n", g,
		EHGet(m->m_hEnemy), remembers_player(g), m->m_MonsterState, m->m_IdealMonsterState);
	if (EHGet(m->m_hEnemy) >= 0)
		FAIL("left: grunt %d of %s still has enemy %d\n", g, mb, EHGet(m->m_hEnemy));
	if (!remembers_player(g))
		FAIL("left: grunt %d of %s forgot the player\n", g, mb);
	if (m->m_MonsterState == MONSTERSTATE_COMBAT || m->m_IdealMonsterState == MONSTERSTATE_COMBAT)
		FAIL("left: grunt %d of %s still in combat (state %d ideal %d)\n", g, mb,
			m->m_MonsterState, m->m_IdealMonsterState);
	if (Mon_PopEnemy(g) || EHGet(m->m_hEnemy) >= 0 || !remembers_player(g))
		FAIL("left: grunt %d's PopEnemy took or dropped the player of another layer\n", g);
	n = game_dehydrate(g, buf, sizeof buf);
	log_printf("ailayer left: grunt %d dehydrates to %u B\n", g, (unsigned)n);
	if (!n)
		FAIL("left: grunt %d of %s refused its release\n", g, mb);

	eng_set_frame_layer(sa);
	if (phy_player_rebase(mapb, T0, -1))
	{
		FAIL("back: player re-base into %s failed\n", mb);
		return;
	}
	for (f = 0; f < 60 && EHGet(m->m_hEnemy) != ENT_PLAYER; f++)
	{
		player_eyes_at(q);
		eng_frame();
		log_flush();
	}
	log_printf("ailayer back: grunt %d enemy=%d after %d frames (player layer %d)\n", g,
		EHGet(m->m_hEnemy), f, ent_idc(ENT_PLAYER)->layer);
	if (EHGet(m->m_hEnemy) != ENT_PLAYER)
		FAIL("back: grunt %d of %s never took the player back in 2 s\n", g, mb);
}

static int layer_mode(const char *root, const char *ma, const char *mb)
{
	float q[3], raw[3];
	int sa, sb, mapb, g, f, seen = 0, heard = 0, lkp = 0, shots = 0, ammo;
	wld_layer_t *L;

	if ((sa = boot(root, ma)) < 0)
		return 2;
	mapb = wld_map_find(mb);
	if (mapb < 0)
		return 2;
	L = wld_open(mapb);
	sb = L->slot;
	rec_map_open(sb);
	for (f = 0; f < 600 && rec_kind(GID_MAKE(sb, 0)) < 0; f++)
		eng_frame();
	wake_slot(sb);
	/* B's monsters started, past the boot window where thinks run whole */
	for (f = 0; f < 90; f++)
		eng_frame();
	log_flush();
	g = find_grunt(sb, q);
	if (g < 0)
	{
		fprintf(stderr, "hl64-ailayer: no grunt with a clear view on %s\n", mb);
		return 2;
	}
	if (ent_idc(ENT_PLAYER)->layer != sa)
	{
		fprintf(stderr, "hl64-ailayer: the player is not home in %s\n", ma);
		return 2;
	}
	/* the player (home A) at the A-local numbers of B's clear view */
	eng_set_frame_layer(sa);
	player_eyes_at(q);
	VecCopy(phy_player()->origin, raw);
	log_printf("ailayer layer: grunt %d of %s at %.0f %.0f %.0f, player in %s at %.0f %.0f %.0f "
		"(B-local view point %.0f %.0f %.0f)\n", g, mb, (double)ent_cv(g)->origin[0],
		(double)ent_cv(g)->origin[1], (double)ent_cv(g)->origin[2], ma, (double)raw[0],
		(double)raw[1], (double)raw[2], (double)q[0], (double)q[1], (double)q[2]);
	/* 1 s: the player's own sound (combat, 1000 u) and the grunt's ears
	 * (its class's sound mask: no schedule's, which may hear nothing) */
	for (f = 0; f < 30; f++)
	{
		const Schedule_t *s;
		int snd;

		player_eyes_at(q);
		eng_frame();
		log_flush();
		eng_set_frame_layer(sa);
		CSoundEnt_InsertSound(bits_SOUND_COMBAT, raw, 1000, 0.3f);
		snd = CSoundEnt_ActiveList();
		eng_set_frame_layer(sb);
		s = MON(g)->m_pSchedule;
		MON(g)->m_pSchedule = NULL;
		Mon_Listen(g);
		MON(g)->m_pSchedule = s;
		heard += on_audible_list(g, snd);
	}
	/* 10 s with the player as its enemy */
	make_enemy(g);
	ammo = MON(g)->m_cAmmoLoaded;
	for (f = 0; f < 300; f++)
	{
		player_eyes_at(q);
		eng_frame();
		log_flush();
		if (!ent_is_live(g) || !IsAlive(g))
		{
			FAIL("layer: grunt %d gone after %d frames\n", g, f);
			break;
		}
		eng_set_frame_layer(sb);
		seen += FVisible(g, ENT_PLAYER);
		lkp += fabsf(MON(g)->m_vecEnemyLKP[0] - raw[0]) < 1 &&
			fabsf(MON(g)->m_vecEnemyLKP[1] - raw[1]) < 1 &&
			fabsf(MON(g)->m_vecEnemyLKP[2] - raw[2]) < 1;
		if (MON(g)->m_cAmmoLoaded < ammo)
			shots += ammo - MON(g)->m_cAmmoLoaded;
		ammo = MON(g)->m_cAmmoLoaded;
	}
	eng_set_frame_layer(sa);
	log_printf("ailayer layer: frames=%d seen=%d heard=%d lkp_raw=%d shots=%d enemy=%d\n", f, seen,
		heard, lkp, shots, EHGet(MON(g)->m_hEnemy));
	if (seen)
		FAIL("layer: grunt %d of %s saw the player of %s in %d frames\n", g, mb, ma, seen);
	if (heard)
		FAIL("layer: grunt %d of %s heard a sound of %s in %d frames\n", g, mb, ma, heard);
	if (lkp)
		FAIL("layer: grunt %d's enemy LKP was the player's %s-local origin in %d frames\n", g, ma,
			lkp);
	if (shots)
		FAIL("layer: grunt %d of %s fired %d rounds at the player of %s\n", g, mb, shots, ma);
	left_checks(g, sa, sb, mapb, q, mb);
	log_printf("ailayer done layer %s %s fails=%d\n", ma, mb, fails);
	log_flush();
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "--root") && !strcmp(argv[3], "--stuck"))
		return stuck_mode(argv[2], argv[4]);
	if (argc == 6 && !strcmp(argv[1], "--root") && !strcmp(argv[3], "--layer"))
		return layer_mode(argv[2], argv[4], argv[5]);
	fprintf(stderr, "usage: hl64-ailayer --root CONTENT --stuck MAP\n"
		"       hl64-ailayer --root CONTENT --layer A B\n");
	return 2;
}
