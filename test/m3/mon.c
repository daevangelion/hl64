/* SPDX-License-Identifier: MIT */
/*
 * mon.c -- host monster sweep driver for test/m3/mon_sweep.py (slice
 * GameMon).
 *
 * hl64-mon --root CONTENT MAP FRAMES: boots the engine as hl64-host does,
 * starts a New Game on MAP with the player at its start, wakes every
 * physical record of the map in lump order, fires the targetname of every
 * monstermaker and monster_grunt_repel record (the run-time monster
 * creation paths) and of every monster_miniturret (deploy / search), then runs FRAMES frames at a fixed 1/30 s. Every frame
 * it samples the live entities whose class is a monster_* (or a GameMon
 * helper) and counts a think whenever an entity's nextthink changed.
 * The sweep is a class test over the whole map, not a streaming test
 * (rec_test_class_sweep): records woken beyond 1024 u of the player stay
 * live for the run (no distance release, architecture.md 6.4), and a
 * neighbour map a seam approach opens is not hydrated.
 * Output, between the engine's own log lines:
 *
 *   mon fire NAME-ID CLASSNAME
 *   mon class CLASSNAME spawned=N live=N dead=N thinks=N
 *   prof mon_us avg=.. max=.. thinks=.. ... mem=.. mem_peak=..
 *   mon done frames=N monsters=N
 *
 * Exit 0; 2 on a usage or content error (an engine sys_error exits != 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/rec.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/rec_int.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "game/hl/util.h"
#include "plat/host/host.h"

static const char *const names[] = {
#define CLASS(id, name)	name,
#include "game/defs/classes.def"
#undef CLASS
};
#define NCLASS	(sizeof(names) / sizeof(names[0]))

static const char *cname(cls_t c)
{
	return c < NCLASS ? names[c] : "?";
}

static int watched(cls_t c)
{
	const char *n = cname(c);

	return !strncmp(n, "monster_", 8) || !strcmp(n, "garg_stomp") ||
		!strcmp(n, "streak_spiral") || !strcmp(n, "env_smoker");
}

static struct {
	uint32_t	spawned, thinks;
} cs[NCLASS];

static struct {
	uint16_t	serial;		/* the incarnation seen */
	cls_t		cls;
	float		nextthink;
	uint8_t		seen;
} es[HL64_MAX_ENTITIES];

static void sample(void)
{
	int e;

	for (e = 0; e < HL64_MAX_ENTITIES; e++)
	{
		cls_t c;

		if (!ent_is_live(e))
			continue;
		c = ent_idc(e)->cls;
		if (c >= NCLASS || !watched(c))
			continue;
		if (!es[e].seen || es[e].serial != ent_idc(e)->serial || es[e].cls != c)
		{
			/* a new entity in this slot */
			es[e].seen = 1;
			es[e].serial = ent_idc(e)->serial;
			es[e].cls = c;
			es[e].nextthink = ent_tm(e)->nextthink;
			cs[c].spawned++;
			continue;
		}
		if (ent_tm(e)->nextthink != es[e].nextthink)
		{
			es[e].nextthink = ent_tm(e)->nextthink;
			cs[c].thinks++;
		}
	}
}

int main(int argc, char **argv)
{
	int map, slot, lump, frames, f, e, k, nfired = 0, total = 0;
	name_t fired[256];
	uint32_t live[NCLASS], dead[NCLASS];
	dbg_cmd_t dt;
	cls_t c;

	if (argc != 5 || strcmp(argv[1], "--root") || (frames = atoi(argv[4])) <= 0)
	{
		fprintf(stderr, "usage: hl64-mon --root CONTENT MAP FRAMES\n");
		return 2;
	}
	host_io_init(argv[2]);
	eng_boot("host");
	map = wld_map_find(argv[3]);
	if (map < 0)
	{
		fprintf(stderr, "hl64-mon: no map %s\n", argv[3]);
		return 2;
	}
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	memset(&dt, 0, sizeof(dt));
	dt.op = DBG_FIXED_DT;
	dt.nargs = 1;
	dt.arg[0].u = 33333;
	if (dbg_push(&dt))
		return 2;
	game_new_campaign();
	rec_new_campaign(map);
	if (phy_player_spawn_start(map))
	{
		fprintf(stderr, "hl64-mon: no player start on %s\n", argv[3]);
		return 2;
	}
	slot = wld_open(map)->slot;
	rec_test_class_sweep(slot);
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);

		if (rec_kind(gid) != REC_KIND_ENTITY || rec_entity(gid) >= 0)
			continue;
		if (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY)
			rec_wake(gid);
	}
	/* the run-time creation paths (makers, rappelling grunts); turrets on */
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		const char *cn;
		name_t n;

		if (rec_kind(gid) < 0)
			continue;
		cn = cname(rec_class(gid));
		if (strcmp(cn, "monstermaker") && strcmp(cn, "monster_grunt_repel") &&
			strcmp(cn, "monster_miniturret"))
			continue;
		n = rec_targetname(gid);
		if (n == NAME_NONE)
			continue;
		for (k = 0; k < nfired && fired[k] != n; k++)
			;
		if (k < nfired)
			continue;	/* one fire per name (it reaches every holder) */
		if (nfired < (int)(sizeof(fired) / sizeof(fired[0])))
			fired[nfired++] = n;
		log_printf("mon fire %u %s\n", (unsigned)n, cn);
		rec_fire(n, ENT_PLAYER, ENT_PLAYER, GID_NONE, USE_ON, 0);
	}
	log_flush();
	sample();
	for (f = 0; f < frames; f++)
	{
		eng_frame();
		sample();
	}
	memset(live, 0, sizeof(live));
	memset(dead, 0, sizeof(dead));
	for (e = 0; e < HL64_MAX_ENTITIES; e++)
	{
		if (!ent_is_live(e) || (c = ent_idc(e)->cls) >= NCLASS || !watched(c))
			continue;
		if (ent_priv(e) && CBase(e)->deadflag != DEAD_NO)
			dead[c]++;
		else
			live[c]++;
	}
	for (c = 0; c < NCLASS; c++)
	{
		if (!cs[c].spawned)
			continue;
		total += (int)cs[c].spawned;
		log_printf("mon class %s spawned=%lu live=%lu dead=%lu thinks=%lu\n", names[c],
			(unsigned long)cs[c].spawned, (unsigned long)live[c], (unsigned long)dead[c],
			(unsigned long)cs[c].thinks);
	}
	game_mon_prof();	/* the run's prof mon_us line (monster CPU / memory) */
	log_printf("mon done frames=%d monsters=%d\n", frames, total);
	log_flush();
	return 0;
}
