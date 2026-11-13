/* SPDX-License-Identifier: MIT */
/*
 * items.c -- host driver for test/m3/items.py (slice Items): the player,
 * weapons and items on the real engine frame (eng_frame at a fixed 1/30 s),
 * driven by a line script on stdin.
 *
 *   hl64-items --root CONTENT MAP < script
 *
 * New Game on MAP with the player at its start. Positions are MAP-local.
 * Script lines:
 *   give CLASSNAME          Player_GiveNamedItem
 *   setpos X Y Z PITCH YAW  the player there (view angles too)
 *   buttons DOWN UP         held buttons (IN_BTN_* masks): (held|DOWN)&~UP
 *   frames N                N engine frames
 *   wake CLASSNAME          wake every record of that class:
 *                           "wake LUMP ENT"
 *   find CLASSNAME          "ent E X Y Z solid=S movetype=M onground=G"
 *                           per live entity of the class, then "find N"
 *   tp E|CLASSNAME DZ       the player at entity E's (or the first live
 *                           CLASSNAME's) origin + DZ (z)
 *   trace PITCH YAW DIST [DZ]  from the eye (+ DZ in z):
 *                           "trace FRAC X Y Z NX NY NZ ENT"
 *   pl                      "pl X Y Z VX VY VZ PUNCHX FLAGS HEALTH LJ
 *                           PHYSINFO" (LJ: m_fLongJump; physinfo "-" empty)
 *   inv                     "inv weapons=0xW active=A ammo=A1,A2,..."
 *   live E                  "live E 0|1 CLASSNAME"
 *   rec LUMP                "rec LUMP state=S": MAP's record LUMP (REC_*)
 * Engine and game log lines come in between. Exit 0; 2 on a usage or
 * content error.
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
#include "engine/in/in.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "game/hl/player.h"
#include "plat/host/host.h"

static const char *const names[] = {
#define CLASS(id, name)	name,
#include "game/defs/classes.def"
#undef CLASS
};
#define NCLASS	(sizeof(names) / sizeof(names[0]))

static int map;

static cls_t cls_of(const char *n)
{
	cls_t c;

	for (c = 0; c < NCLASS; c++)
		if (!strcmp(names[c], n))
			return c;
	return CLS_NONE;
}

static const char *cname(cls_t c)
{
	return c < NCLASS ? names[c] : "?";
}

static int setpos(const float org[3], float pitch, float yaw)
{
	float ang[3] = { pitch, yaw, 0 };

	return phy_player_spawn(map, org, ang);
}

static int line(char *s)
{
	char cmd[32], arg[64];
	int n, i, slot = wld_open(map)->slot;

	if (sscanf(s, "%31s%n", cmd, &n) != 1)
		return 0;
	s += n;
	if (!strcmp(cmd, "give") && sscanf(s, "%63s", arg) == 1)
	{
		if (cls_of(arg) == CLS_NONE)
			return -1;
		eng_set_frame_layer(slot);
		Player_GiveNamedItem(cls_of(arg));
		eng_set_frame_layer(-1);
	}
	else if (!strcmp(cmd, "setpos"))
	{
		float o[3], p, y;

		if (sscanf(s, "%f %f %f %f %f", &o[0], &o[1], &o[2], &p, &y) != 5)
			return -1;
		return setpos(o, p, y);
	}
	else if (!strcmp(cmd, "buttons"))
	{
		unsigned d, u;

		if (sscanf(s, "%i %i", &d, &u) != 2)
			return -1;
		in_buttons((uint16_t)d, (uint16_t)u);
	}
	else if (!strcmp(cmd, "frames"))
	{
		if (sscanf(s, "%d", &n) != 1)
			return -1;
		while (n-- > 0)
			eng_frame();
	}
	else if (!strcmp(cmd, "wake") && sscanf(s, "%63s", arg) == 1)
	{
		for (i = 0; i < 1024; i++)
		{
			gid_t16 gid = GID_MAKE(slot, i);
			int e;

			if (rec_kind(gid) != REC_KIND_ENTITY || strcmp(cname(rec_class(gid)), arg))
				continue;
			e = rec_entity(gid);
			if (e < 0 && (rec_state(gid) == REC_PENDING || rec_state(gid) == REC_DRY))
				e = rec_wake(gid);
			log_printf("wake %d %d\n", i, e);
		}
	}
	else if (!strcmp(cmd, "find") && sscanf(s, "%63s", arg) == 1)
	{
		int k = 0;

		for (i = 0; i < HL64_MAX_ENTITIES; i++)
		{
			const col_ent_t *c;

			if (!ent_is_live(i) || strcmp(cname(ent_idc(i)->cls), arg))
				continue;
			c = ent_cv(i);
			log_printf("ent %d %.3f %.3f %.3f solid=%d movetype=%d onground=%d\n", i,
				(double)c->origin[0], (double)c->origin[1], (double)c->origin[2],
				c->solid, c->movetype, (c->flags & FL_ONGROUND) ? 1 : 0);
			k++;
		}
		log_printf("find %d\n", k);
	}
	else if (!strcmp(cmd, "tp"))
	{
		float dz, o[3];
		const float *a = in_angles();

		if (sscanf(s, "%63s %f", arg, &dz) != 2)
			return -1;
		if (cls_of(arg) != CLS_NONE)
		{
			/* the first live entity of the class */
			for (i = 0; i < HL64_MAX_ENTITIES; i++)
				if (ent_is_live(i) && ent_idc(i)->cls == cls_of(arg))
					break;
		}
		else
			i = atoi(arg);
		if (i < 0 || i >= HL64_MAX_ENTITIES || !ent_is_live(i))
			return -1;
		memcpy(o, ent_cv(i)->origin, sizeof(o));
		o[2] += dz;
		return setpos(o, a[0], a[1]);
	}
	else if (!strcmp(cmd, "trace"))
	{
		float a[3] = { 0, 0, 0 }, d, dz = 0, src[3], end[3];
		col_trace_t tr;

		if (sscanf(s, "%f %f %f %f", &a[0], &a[1], &d, &dz) < 3)
			return -1;
		eng_set_frame_layer(slot);
		Player_GunPosition(src);
		src[2] += dz;
		UTIL_MakeVectors(a);
		VecMA(src, d, gpGlobals.v_forward, end);
		UTIL_TraceLine(src, end, dont_ignore_monsters, ENT_PLAYER, &tr);
		eng_set_frame_layer(-1);
		log_printf("trace %.6f %.3f %.3f %.3f %.4f %.4f %.4f %d\n", (double)tr.frac,
			(double)tr.end[0], (double)tr.end[1], (double)tr.end[2], (double)tr.normal[0],
			(double)tr.normal[1], (double)tr.normal[2], tr.ent);
	}
	else if (!strcmp(cmd, "pl"))
	{
		const ent_player_t *pl = phy_player();

		log_printf("pl %.4f %.4f %.4f %.4f %.4f %.4f %.4f %lu %.1f %d %s\n",
			(double)pl->origin[0], (double)pl->origin[1], (double)pl->origin[2],
			(double)pl->velocity[0], (double)pl->velocity[1], (double)pl->velocity[2],
			(double)pl->punchangle[0], (unsigned long)pl->flags,
			(double)(PlayerMon() ? PlayerMon()->health : 0), Player()->m_fLongJump,
			pl->physinfo[0] ? pl->physinfo : "-");
	}
	else if (!strcmp(cmd, "inv"))
	{
		CBasePlayer *p = Player();
		char buf[96];
		int k = 0;

		for (i = 0; i < MAX_AMMO_TYPES; i++)
			k += snprintf(buf + k, sizeof(buf) - (size_t)k, "%s%d", i ? "," : "",
				p->m_rgAmmo[i]);
		log_printf("inv weapons=0x%lx active=%d ammo=%s\n", (unsigned long)p->weapons,
			p->m_pActiveItem, buf);
	}
	else if (!strcmp(cmd, "live"))
	{
		if (sscanf(s, "%d", &i) != 1)
			return -1;
		log_printf("live %d %d %s\n", i, ent_is_live(i),
			ent_is_live(i) ? cname(ent_idc(i)->cls) : "-");
	}
	else if (!strcmp(cmd, "rec"))
	{
		if (sscanf(s, "%d", &i) != 1)
			return -1;
		log_printf("rec %d state=%d\n", i, rec_state(GID_MAKE(slot, i)));
	}
	else
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	char buf[256];
	dbg_cmd_t dt;
	int ln = 0;

	if (argc != 4 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-items --root CONTENT MAP < script\n");
		return 2;
	}
	host_io_init(argv[2]);
	eng_boot("host");
	map = wld_map_find(argv[3]);
	if (map < 0)
	{
		fprintf(stderr, "hl64-items: no map %s\n", argv[3]);
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
		fprintf(stderr, "hl64-items: no player start on %s\n", argv[3]);
		return 2;
	}
	eng_frame();	/* the fixed frame time takes effect at the frame boundary */
	while (fgets(buf, sizeof(buf), stdin))
	{
		ln++;
		if (line(buf))
		{
			log_flush();
			fprintf(stderr, "hl64-items: script line %d: %s", ln, buf);
			return 2;
		}
		log_flush();
	}
	log_printf("items done\n");
	log_flush();
	return 0;
}
