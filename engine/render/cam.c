/* SPDX-License-Identifier: MIT */
/*
 * cam.c -- the M1 free camera (docs/architecture.md 17 M1): the debug pad
 * flies the eye; the setpos / look debug commands place it (setpos opens
 * the named map's layer). M2's view (game_calc_view) replaces it as the
 * camera source; the commands then move the player instead.
 *
 * Pad: stick up/down = forward/back along the view, stick left/right =
 * turn; C-up/down = pitch, C-left/right = strafe; Z = down, R = up;
 * L held = 4x speed.
 */
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"

#define FLY_SPEED	320.0f	/* units per second */
#define TURN_SPEED	120.0f	/* degrees per second, full stick */
#define STICK_DEAD	8
#define STICK_FULL	80.0f

ren_cam_t ren_cam = { 0, { 0, 0, 0 }, { 0, 0, 0 } };

static float stick(int v)
{
	if (v > -STICK_DEAD && v < STICK_DEAD)
		return 0;
	if (v > STICK_FULL)
		return 1;
	if (v < -STICK_FULL)
		return -1;
	return (float)v / STICK_FULL;
}

void ren_cam_update(uint32_t dt_us)
{
	sys_pad_t p;
	float dt = (float)dt_us * 1e-6f, f[3], r[3], u[3], fwd, side, up, speed;
	int k;

	sys_pad_read(&p);
	if (!p.stick_x && !p.stick_y && !p.buttons)
		return;
	speed = FLY_SPEED * dt * ((p.buttons & SYS_PAD_L) ? 4.0f : 1.0f);
	ren_cam.ang[1] -= stick(p.stick_x) * TURN_SPEED * dt;
	if (p.buttons & SYS_PAD_C_UP)
		ren_cam.ang[0] -= TURN_SPEED * 0.5f * dt;
	if (p.buttons & SYS_PAD_C_DOWN)
		ren_cam.ang[0] += TURN_SPEED * 0.5f * dt;
	if (ren_cam.ang[0] > 89)
		ren_cam.ang[0] = 89;
	if (ren_cam.ang[0] < -89)
		ren_cam.ang[0] = -89;
	while (ren_cam.ang[1] >= 360)
		ren_cam.ang[1] -= 360;
	while (ren_cam.ang[1] < 0)
		ren_cam.ang[1] += 360;
	ren_angle_axes(ren_cam.ang, f, r, u);
	fwd = stick(p.stick_y) * speed;
	side = ((p.buttons & SYS_PAD_C_RIGHT) ? speed : 0) - ((p.buttons & SYS_PAD_C_LEFT) ? speed : 0);
	up = ((p.buttons & SYS_PAD_R) ? speed : 0) - ((p.buttons & SYS_PAD_Z) ? speed : 0);
	for (k = 0; k < 3; k++)
		ren_cam.org[k] += f[k] * fwd + r[k] * side;
	ren_cam.org[2] += up;
}

/* setpos map x y z pitch yaw roll: the eye, map-local */
static int cmd_setpos(const dbg_cmd_t *c)
{
	int map = (int)c->arg[0].u, k;
	const float *g;

	if (map < 0 || map >= wld_map_count())
		return -1;
	wld_open(map);
	g = wld_map_origin(map);
	ren_cam.map = map;
	for (k = 0; k < 3; k++)
	{
		ren_cam.org[k] = c->arg[1 + k].f + g[k];
		ren_cam.ang[k] = c->arg[4 + k].f;
	}
	log_printf("cam: %s %.9g %.9g %.9g ang %.9g %.9g %.9g\n", wld_map_name(map),
		(double)c->arg[1].f, (double)c->arg[2].f, (double)c->arg[3].f,
		(double)ren_cam.ang[0], (double)ren_cam.ang[1], (double)ren_cam.ang[2]);
	return 0;
}

static int cmd_look(const dbg_cmd_t *c)
{
	ren_cam.ang[0] = c->arg[0].f;
	ren_cam.ang[1] = c->arg[1].f;
	ren_cam.ang[2] = 0;
	return 0;
}

/* Start: the campaign's start map at its player start (eye 28 u above the
 * origin, GoldSrc's standing view height), else the world model's centre. */
void ren_cam_init(void)
{
	int map = wld_start_map(), k;
	wld_layer_t *L = wld_open(map);
	const float *g = wld_map_origin(map);

	ren_cam.map = map;
	for (k = 0; k < 3; k++)
	{
		ren_cam.org[k] = g[k] + (L->has_start ? L->start_org[k]
			: 0.5f * (L->models[0].mins[k] + L->models[0].maxs[k]));
		ren_cam.ang[k] = L->has_start ? L->start_ang[k] : 0;
	}
	if (L->has_start)
		ren_cam.org[2] += 28.0f;
	dbg_handler_set(DBG_SETPOS, cmd_setpos);
	dbg_handler_set(DBG_LOOK, cmd_look);
}
