/* SPDX-License-Identifier: MIT */
/*
 * studio.c -- host driver for the studio tests (test/m3/studio_test.py):
 * the engine's studio residency (engine/render/studio.c) and the game's
 * animation API (game/studio/anim.h) on cooked models bound with
 * studio_test_bind, one command per line on stdin, one result line
 * ("R ...") per query on stdout.
 *
 *   B idx s64 a64            bind MODEL index idx to cooked files
 *   L idx label              gs_lookup_sequence
 *   H idx act                gs_lookup_activity_heaviest
 *   Y idx act n              n draws of gs_lookup_activity
 *   I idx seq                gs_sequence_info, flags, frames
 *   V idx seq start end      every gs_anim_event in the window (+ its res)
 *   N idx seq                new entity of the model: R N slot
 *   T time                   eng_set_clock(time, 0.1)
 *   R e                      gs_reset_sequence_info
 *   A e interval             gs_frame_advance
 *   W e                      gs_event_window
 *   G e group value          gs_set_bodygroup / gs_get_bodygroup
 *   K idx i value            gs_set_controller into a scratch ctl[4]
 *   F                        frame boundary (studio_frame)
 *   S                        studio stats
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/game.h"
#include "hl64/pmove.h"
#include "hl64/studio.h"
#include "game/studio/anim.h"
#include "engine/core/log.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/phys.h"
#include "plat/host/host.h"

static gs_animating_t	anim[HL64_MAX_ENTITIES];
static char		paths[64][2][256];

static res_t mdl(int idx)
{
	return RES_MAKE(RES_K_MODEL, idx);
}

int main(int argc, char **argv)
{
	static char line[1024];

	if (argc != 2)
	{
		fprintf(stderr, "usage: hl64-studiotest CONTENT_DIR < commands\n");
		return 2;
	}
	host_io_init(argv[1]);
	mem_boot_carve();
	io_init();
	ent_init();
	studio_init();
	eng_clock_new_game();
	log_flush();
	while (fgets(line, sizeof(line), stdin))
	{
		char a[256], b[256];
		int i, j, k, e;
		float f, g;

		switch (line[0])
		{
		case 'B':
			if (sscanf(line + 1, "%d %255s %255s", &i, a, b) != 3 || i < 0 || i >= 64)
				goto bad;
			strcpy(paths[i][0], a);
			strcpy(paths[i][1], b);
			studio_test_bind(i, paths[i][0], paths[i][1]);
			break;
		case 'L':
			if (sscanf(line + 1, "%d %255s", &i, a) != 2)
				goto bad;
			printf("R L %d\n", gs_lookup_sequence(mdl(i), a));
			break;
		case 'H':
			if (sscanf(line + 1, "%d %d", &i, &j) != 2)
				goto bad;
			printf("R H %d\n", gs_lookup_activity_heaviest(mdl(i), j));
			break;
		case 'Y':
			if (sscanf(line + 1, "%d %d %d", &i, &j, &k) != 3)
				goto bad;
			printf("R Y");
			while (k-- > 0)
				printf(" %d", gs_lookup_activity(mdl(i), j));
			printf("\n");
			break;
		case 'I':
			if (sscanf(line + 1, "%d %d", &i, &j) != 2)
				goto bad;
			f = g = -1;
			gs_sequence_info(mdl(i), j, &f, &g);
			printf("R I %.9g %.9g %d %d %d\n", f, g, gs_sequence_flags(mdl(i), j),
				gs_sequence_frames(mdl(i), j), gs_sequence_count(mdl(i)));
			break;
		case 'V':
		{
			int ev, idx = 0;
			const char *opt;

			if (sscanf(line + 1, "%d %d %f %f", &i, &j, &f, &g) != 4)
				goto bad;
			printf("R V");
			while ((idx = gs_anim_event(mdl(i), j, f, g, idx, &ev, &opt)) != 0)
				printf(" %d:%u:%s", ev, (unsigned)gs_anim_event_res(mdl(i), j, idx), opt);
			printf("\n");
			break;
		}
		case 'N':
			if (sscanf(line + 1, "%d %d", &i, &j) != 2)
				goto bad;
			e = ent_create(ENT_ALLOC_GAME);
			ent_rnd(e)->model = mdl(i);
			ent_rnd(e)->sequence = (uint16_t)j;
			ent_rnd(e)->frame = 0;
			memset(&anim[e], 0, sizeof(anim[e]));
			printf("R N %d\n", e);
			break;
		case 'T':
		{
			double t;

			if (sscanf(line + 1, "%lf", &t) != 1)
				goto bad;
			eng_set_clock(t, 0.1);
			break;
		}
		case 'R':
			if (sscanf(line + 1, "%d", &e) != 1 || !ent_is_live(e))
				goto bad;
			gs_reset_sequence_info(e, &anim[e]);
			printf("R R %.9g %.9g %d %.9g %.9g\n", anim[e].m_flFrameRate,
				anim[e].m_flGroundSpeed, anim[e].m_fSequenceLoops, ent_rnd(e)->animtime,
				ent_rnd(e)->framerate);
			break;
		case 'A':
			if (sscanf(line + 1, "%d %f", &e, &f) != 2 || !ent_is_live(e))
				goto bad;
			g = gs_frame_advance(e, &anim[e], f);
			printf("R A %.9g %.9g %d %.9g\n", g, ent_rnd(e)->frame,
				anim[e].m_fSequenceFinished, ent_rnd(e)->animtime);
			break;
		case 'W':
			if (sscanf(line + 1, "%d", &e) != 1 || !ent_is_live(e))
				goto bad;
			gs_event_window(e, &anim[e], &f, &g);
			printf("R W %.9g %.9g %d %.9g\n", f, g, anim[e].m_fSequenceFinished,
				anim[e].m_flLastEventCheck);
			break;
		case 'G':
			if (sscanf(line + 1, "%d %d %d", &e, &j, &k) != 3 || !ent_is_live(e))
				goto bad;
			gs_set_bodygroup(ent_rnd(e)->model, &ent_rnd(e)->body, j, k);
			printf("R G %d %d\n", ent_rnd(e)->body,
				gs_get_bodygroup(ent_rnd(e)->model, ent_rnd(e)->body, j));
			break;
		case 'K':
		{
			uint8_t ctl[4] = { 0, 0, 0, 0 };

			if (sscanf(line + 1, "%d %d %f", &i, &j, &f) != 3)
				goto bad;
			g = gs_set_controller(mdl(i), ctl, j, f);
			printf("R K %.9g %d %d %d %d\n", g, ctl[0], ctl[1], ctl[2], ctl[3]);
			break;
		}
		case 'F':
			studio_frame();
			io_pump();
			break;
		case 'S':
		{
			studio_stats_t s;

			studio_stats(&s, 0);
			printf("R S models=%u anims=%u pool=%u peak=%u sync=%u\n", (unsigned)s.models,
				(unsigned)s.anims, (unsigned)s.pool_used, (unsigned)s.pool_peak,
				(unsigned)s.sync_loads);
			break;
		}
		case '\n':
		case '#':
			break;
		default:
			goto bad;
		}
		log_flush();
		continue;
bad:
		fprintf(stderr, "studiotest: bad command: %s", line);
		return 2;
	}
	return 0;
}
