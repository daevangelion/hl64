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
#include "engine/save/sv.h"
#include "engine/save/svmod.h"

#define FORWARD_SPEED	400.0f	/* cl_forwardspeed, cl_backspeed */
#define SIDE_SPEED	400.0f	/* cl_sidespeed */
#define WALK_SCALE	0.3f	/* cl_movespeedkey (IN_BTN_RUN, "+speed") */
#define STICK_FULL	80.0f
#define PITCH_LIMIT	89.0f	/* cl_pitchup / cl_pitchdown */

/* Impulses the pad sends (game/hl/player.c ImpulseCommands): HLSDK 100
 * flashlight; hl64's 6 / 7 / 8 = the HLSDK client's invnext / invprev /
 * lastinv. */
#define IMP_INVNEXT	6
#define IMP_INVPREV	7
#define IMP_LASTINV	8
#define IMP_FLASHLIGHT	100

/* The pad binding (docs/controls.md). A button with a command button
 * holds it while down; one with an impulse sends it once per press (the
 * frame it goes down). C-up / C-down pitch and the stick are in_frame's. */
static const struct {
	uint16_t	pad;
	uint16_t	btn;		/* IN_BTN_* held */
	uint8_t		impulse;	/* sent on the press edge */
} bind[] = {
	{ SYS_PAD_A, IN_BTN_JUMP, 0 },
	{ SYS_PAD_B, IN_BTN_USE, 0 },
	{ SYS_PAD_Z, IN_BTN_ATTACK, 0 },
	{ SYS_PAD_R, IN_BTN_ATTACK2, 0 },
	{ SYS_PAD_L, IN_BTN_DUCK, 0 },
	{ SYS_PAD_C_LEFT, IN_BTN_MOVELEFT, 0 },
	{ SYS_PAD_C_RIGHT, IN_BTN_MOVERIGHT, 0 },
	{ SYS_PAD_D_DOWN, IN_BTN_RELOAD, 0 },
	{ SYS_PAD_D_UP, 0, IMP_FLASHLIGHT },
	{ SYS_PAD_D_LEFT, 0, IMP_INVPREV },
	{ SYS_PAD_D_RIGHT, 0, IMP_INVNEXT },
	{ SYS_PAD_START, 0, IMP_LASTINV },
};
#define NBIND	(sizeof(bind) / sizeof(bind[0]))
_Static_assert(NBIND <= 16, "pending presses are a 16-bit mask over bind[]");

static struct {
	float		ang[3];
	uint16_t	held;		/* route / debug buttons */
	uint8_t		impulse;	/* route / debug impulse */
	uint16_t	pad_last;	/* pad buttons of the last frame */
	uint16_t	pend;		/* bind[] impulse presses not yet sent */
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
	for (i = 0; i < NBIND; i++)
	{
		if (!(p.buttons & bind[i].pad))
			continue;
		b |= bind[i].btn;
		if (bind[i].impulse && !(in.pad_last & bind[i].pad))
			in.pend |= (uint16_t)(1u << i);
	}
	in.pad_last = p.buttons;
	/* one impulse per command: a route / debug impulse first, then the
	 * pad's presses in bind[] order, each in its own command */
	if (!in.impulse && in.pend)
	{
		for (i = 0; !(in.pend & (1u << i)); i++)
			;
		in.pend &= (uint16_t)~(1u << i);
		in.impulse = bind[i].impulse;
	}
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

/* ---- saved games (architecture.md 14) ------------------------------------------- */

void in_save(void)
{
	sv_tag(SV_TAG('I', 'N', 'P', '1'));
	sv_put(in.ang, sizeof(in.ang));
	sv_u16(in.held);
	sv_u8(in.impulse);
	sv_u16(in.pend);
}

/* The pad's last buttons are the pad's: a held button reads as held, not
 * as a new press, on the first frame after the load. */
int in_load(void)
{
	if (sv_expect(SV_TAG('I', 'N', 'P', '1')))
		return -1;
	sv_get(in.ang, sizeof(in.ang));
	in.held = (uint16_t)sv_get_u16();
	in.impulse = (uint8_t)sv_get_u8();
	in.pend = (uint16_t)sv_get_u16();
	return sv_failed() ? -1 : 0;
}
