/* SPDX-License-Identifier: MIT */
/*
 * game.c -- host spawn-all driver for test/m3/game.py (slice GameCore).
 *
 * hl64-game --root CONTENT MAP: boots the engine as hl64-host does (the
 * game's tables and hooks included), starts a New Game on MAP (logic
 * records and the records near the start spawn as in a route), then wakes
 * every remaining physical record of the map in lump order. One line per
 * record, between the engine's own log lines (spawn refusals carry their
 * reason there):
 *
 *   rec LUMP CLASSNAME logic|point
 *   rec LUMP CLASSNAME live SLOT solid=S movetype=M
 *   rec LUMP CLASSNAME refused
 *   rec LUMP CLASSNAME killed|dry|pending
 *
 * then `done N` (records). Exit 0; 2 on a usage or content error.
 */
#include <stdio.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/rec.h"
#include "engine/core/eng.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static const char *const names[] = {
#define CLASS(id, name)	name,
#include "game/defs/classes.def"
#undef CLASS
};

static const char *cname(cls_t c)
{
	return c < sizeof(names) / sizeof(names[0]) ? names[c] : "?";
}

int main(int argc, char **argv)
{
	int map, slot, lump, n = 0;

	if (argc != 4 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-game --root CONTENT MAP\n");
		return 2;
	}
	host_io_init(argv[2]);
	eng_boot("host");
	map = wld_map_find(argv[3]);
	if (map < 0)
	{
		fprintf(stderr, "hl64-game: no map %s\n", argv[3]);
		return 2;
	}
	/* boot opened the start map for the free camera: one map per run */
	if (map != wld_start_map())
		wld_close(wld_open(wld_start_map()));
	game_new_campaign();
	rec_new_campaign(map);
	slot = wld_open(map)->slot;
	for (lump = 0; lump < 1024; lump++)
	{
		gid_t16 gid = GID_MAKE(slot, lump);
		int kind = rec_kind(gid), e;
		const char *cn;

		if (kind < 0)
			continue;	/* a lump the cook dropped */
		n++;
		cn = cname(rec_class(gid));
		if (kind == REC_KIND_LOGIC || kind == REC_KIND_POINT)
		{
			log_printf("rec %d %s %s\n", lump, cn, kind == REC_KIND_LOGIC ? "logic" : "point");
			continue;
		}
		e = rec_entity(gid);
		if (e < 0 && (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY))
		{
			e = rec_wake(gid);
			if (e < 0)
			{
				log_printf("rec %d %s refused\n", lump, cn);
				continue;
			}
		}
		if (e >= 0)
			log_printf("rec %d %s live %d solid=%d movetype=%d\n", lump, cn, e,
				ent_cv(e)->solid, ent_cv(e)->movetype);
		else
			log_printf("rec %d %s %s\n", lump, cn,
				rec_state(gid) == REC_KILLED ? "killed" : "pending");
		log_flush();
	}
	log_printf("done %d\n", n);
	log_flush();
	return 0;
}
