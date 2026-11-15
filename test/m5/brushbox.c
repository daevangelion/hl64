/* SPDX-License-Identifier: MIT */
/*
 * brushbox.c -- host brush-entity box driver (test/m5/brushbox.py, a
 * test/run/m5.sh suite): the boxes the game's brush entities get from
 * their models (col_model_bounds, docs/specs/area-links-and-touch.md 2)
 * and the spawn positions HLSDK derives from them (a START_OPEN door's
 * travel: size - 2 - lip), for comparison with GoldSrc's brush_ents.
 *
 * hl64-brushbox --root CONTENT --map MAP [--frames N]
 *   New Game on MAP (player god, noclip), every record of it woken and
 *   kept live (rec_test_class_sweep), at a fixed 0.01 s (the probe's
 *   host_framerate). Before the first frame and after N frames (default
 *   300: pushables dropped to the floor) one line per live brush entity
 *   of MAP's layer:
 *     bb FRAME SUB CLASS o=X,Y,Z mins=... maxs=... absmin=... absmax=...
 *   (SUB its submodel, `*SUB` in the BSP). Exit 0, 2 on usage / content
 *   errors.
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
#include "engine/ent/ent.h"
#include "engine/ent/rec_int.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static void vec(const char *k, const float v[3])
{
	printf(" %s=%.9g,%.9g,%.9g", k, (double)v[0], (double)v[1], (double)v[2]);
}

static void dump(int slot, int frame)
{
	int e;

	for (e = ent_next_live(ENT_PLAYER); e >= 0; e = ent_next_live(e))
	{
		const col_ent_t *cv = ent_cv(e);
		res_t m = ent_rnd(e)->model;

		if (RES_KIND(m) != RES_K_SUBMODEL || RES_SUB_SLOT(m) != slot)
			continue;
		printf("bb %d %d %s", frame, RES_SUB_NUM(m), ent_class_name(e));
		vec("o", cv->origin);
		vec("mins", cv->mins);
		vec("maxs", cv->maxs);
		vec("absmin", cv->absmin);
		vec("absmax", cv->absmax);
		printf("\n");
	}
	fflush(stdout);
}

static int run(const char *root, const char *mapname, int frames)
{
	dbg_cmd_t dt;
	int map, slot, lump, f;

	host_io_init(root);
	eng_boot("host");
	map = wld_map_find(mapname);
	if (map < 0)
	{
		fprintf(stderr, "hl64-brushbox: no map %s\n", mapname);
		return 2;
	}
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	memset(&dt, 0, sizeof(dt));
	dt.op = DBG_FIXED_DT;
	dt.nargs = 1;
	dt.arg[0].u = 10000;
	if (dbg_push(&dt))
		return 2;
	game_new_campaign();
	rec_new_campaign(map);
	if (phy_player_spawn_start(map))
	{
		fprintf(stderr, "hl64-brushbox: no player start on %s\n", mapname);
		return 2;
	}
	phy_player()->movetype = PL_MOVE_NOCLIP;
	phy_player()->flags |= PL_FL_GODMODE;
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
	dump(slot, 0);
	for (f = 0; f < frames; f++)
	{
		eng_frame();
		log_flush();
	}
	dump(slot, frames);
	return 0;
}

int main(int argc, char **argv)
{
	if ((argc == 5 || (argc == 7 && !strcmp(argv[5], "--frames")))
		&& !strcmp(argv[1], "--root") && !strcmp(argv[3], "--map"))
		return run(argv[2], argv[4], argc == 7 ? atoi(argv[6]) : 300);
	fprintf(stderr, "usage: hl64-brushbox --root CONTENT --map MAP [--frames N]\n");
	return 2;
}
