/* SPDX-License-Identifier: MIT */
/*
 * in.c -- pad -> usercmd (docs/architecture.md 11; docs/specs/pmove-host.md
 * 1 step 1). One command per frame from the pad, the held route / debug
 * buttons and the view angles kept here (the HLSDK client's view angles).
 */
#include <math.h>
#include <string.h>

#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"

#define FORWARD_SPEED	400.0f	/* cl_forwardspeed, cl_backspeed */
#define SIDE_SPEED	400.0f	/* cl_sidespeed */
#define WALK_SCALE	0.3f	/* cl_movespeedkey (IN_BTN_RUN, "+speed") */
#define STICK_FULL	80.0f
#define PITCH_LIMIT	89.0f	/* cl_pitchup / cl_pitchdown */

/* pad buttons -> command buttons */
static const struct {
	uint16_t	pad;
	uint16_t	btn;
} bind[] = {
	{ SYS_PAD_A, IN_BTN_JUMP },
	{ SYS_PAD_B, IN_BTN_USE },
	{ SYS_PAD_Z, IN_BTN_ATTACK },
	{ SYS_PAD_R, IN_BTN_ATTACK2 },
	{ SYS_PAD_L, IN_BTN_DUCK },
	{ SYS_PAD_C_LEFT, IN_BTN_MOVELEFT },
	{ SYS_PAD_C_RIGHT, IN_BTN_MOVERIGHT },
};

static struct {
	float		ang[3];
	uint16_t	held;		/* route / debug buttons */
	uint8_t		impulse;
} in;

static float stick(int v)
{
	int dead = settings.stick_deadzone;

	if (v > -dead && v < dead)
		return 0;
	if (v > STICK_FULL)
		return 1;
	if (v < -STICK_FULL)
		return -1;
	return (float)v / STICK_FULL;
}

void in_set_angles(const float ang[3])
{
	in.ang[0] = ang[0];
	in.ang[1] = ang[1];
	in.ang[2] = ang[2];
}

const float *in_angles(void)
{
	return in.ang;
}

void in_buttons(uint16_t down, uint16_t up)
{
	in.held = (uint16_t)((in.held | down) & ~up);
}

void in_impulse(uint8_t impulse)
{
	in.impulse = impulse;
}

uint16_t in_held(void)
{
	return in.held;
}

void in_frame(uint32_t dt_us, float maxspeed, in_cmd_t *cmd)
{
	sys_pad_t p;
	float dt = (float)dt_us * 1e-6f, sx, sy, fm, sm, spd;
	uint32_t ms = (dt_us + 500u) / 1000u;
	uint16_t b = in.held;
	unsigned i;

	sys_pad_read(&p);
	for (i = 0; i < sizeof(bind) / sizeof(bind[0]); i++)
		if (p.buttons & bind[i].pad)
			b |= bind[i].btn;
	sx = stick(p.stick_x);
	sy = stick(p.stick_y);

	/* view: stick x and the turn buttons yaw, C-up / C-down pitch */
	in.ang[1] -= sx * settings.yawspeed * dt;
	if (b & IN_BTN_LEFT)
		in.ang[1] += settings.yawspeed * dt;
	if (b & IN_BTN_RIGHT)
		in.ang[1] -= settings.yawspeed * dt;
	if (p.buttons & (SYS_PAD_C_UP | SYS_PAD_C_DOWN))
	{
		float d = settings.pitchspeed * dt * ((p.buttons & SYS_PAD_C_UP) ? -1.0f : 1.0f);

		in.ang[0] += settings.invert_y ? -d : d;
	}
	if (in.ang[0] > PITCH_LIMIT)
		in.ang[0] = PITCH_LIMIT;
	if (in.ang[0] < -PITCH_LIMIT)
		in.ang[0] = -PITCH_LIMIT;
	in.ang[1] = fmodf(in.ang[1], 360.0f);
	if (in.ang[1] < 0)
		in.ang[1] += 360.0f;

	/* moves: stick y, then the movement buttons (HLSDK +forward etc.) */
	fm = sy * FORWARD_SPEED;
	sm = 0;
	if (b & IN_BTN_FORWARD)
		fm += FORWARD_SPEED;
	if (b & IN_BTN_BACK)
		fm -= FORWARD_SPEED;
	if (b & IN_BTN_MOVERIGHT)
		sm += SIDE_SPEED;
	if (b & IN_BTN_MOVELEFT)
		sm -= SIDE_SPEED;
	if (sy > 0)
		b |= IN_BTN_FORWARD;
	else if (sy < 0)
		b |= IN_BTN_BACK;
	if (b & IN_BTN_RUN)
	{
		fm *= WALK_SCALE;
		sm *= WALK_SCALE;
	}
	/* clip to the client max speed */
	spd = sqrtf(fm * fm + sm * sm);
	if (maxspeed > 0 && spd > maxspeed)
	{
		fm *= maxspeed / spd;
		sm *= maxspeed / spd;
	}

	memset(cmd, 0, sizeof(*cmd));
	cmd->msec = (uint8_t)(ms < 1 ? 1 : ms > 100 ? 100 : ms);
	memcpy(cmd->viewangles, in.ang, sizeof(cmd->viewangles));
	cmd->forwardmove = fm;
	cmd->sidemove = sm;
	cmd->upmove = 0;
	cmd->buttons = b;
	cmd->impulse = in.impulse;
	in.impulse = 0;
}

/* buttons down up */
static int cmd_buttons(const dbg_cmd_t *c)
{
	if ((c->arg[0].u | c->arg[1].u) > 0xFFFFu || (c->arg[0].u & c->arg[1].u))
		return -1;
	in_buttons((uint16_t)c->arg[0].u, (uint16_t)c->arg[1].u);
	return 0;
}

static int cmd_impulse(const dbg_cmd_t *c)
{
	if (c->arg[0].u > 255)
		return -1;
	in_impulse((uint8_t)c->arg[0].u);
	return 0;
}

void in_init(void)
{
	memset(&in, 0, sizeof(in));
	dbg_handler_set(DBG_BUTTONS, cmd_buttons);
	dbg_handler_set(DBG_IMPULSE, cmd_impulse);
}
