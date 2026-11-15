/* SPDX-License-Identifier: MIT */
/*
 * in.c -- pad -> usercmd (docs/architecture.md 11; docs/specs/pmove-host.md
 * 1 step 1). One command per frame from the pad, the held route / debug
 * buttons and the view angles kept here (the HLSDK client's view angles);
 * and the menu's keys (engine/ui/menu.c): Start, A, B and the D-pad /
 * stick directions with auto-repeat.
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
 * flashlight; hl64's 6 / 7 = the HLSDK client's invnext / invprev. Start
 * is the pause menu's (in_keys), not a binding. */
#define IMP_INVNEXT	6
#define IMP_INVPREV	7
#define IMP_FLASHLIGHT	100

/* Menu keys: a direction repeats after REPEAT_FIRST us held, then every
 * REPEAT_NEXT us; the stick counts as a direction past STICK_KEY. */
#define REPEAT_FIRST	400000u
#define REPEAT_NEXT	120000u
#define STICK_KEY	40

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
};
#define NBIND	(sizeof(bind) / sizeof(bind[0]))
_Static_assert(NBIND <= 16, "pending presses are a 16-bit mask over bind[]");

static struct {
	float		ang[3];
	uint16_t	held;		/* route / debug buttons */
	uint8_t		impulse;	/* route / debug impulse */
	uint16_t	pad_last;	/* pad buttons of the last frame */
	uint16_t	pend;		/* bind[] impulse presses not yet sent */
	uint16_t	mask;		/* held at in_hold: ignored until released */
	uint16_t	keys_last;	/* in_keys: keys held last call */
	uint32_t	rep_us;		/* in_keys: time to the next repeat */
#ifdef HL64_TEST
	sys_pad_t	dbg;		/* DBG_PAD: the pad as a debug command holds it */
#endif
} in;

/* Port 1 (test builds: with the DBG_PAD pad over it). */
static void pad_read(sys_pad_t *p)
{
	sys_pad_read(p);
#ifdef HL64_TEST
	p->buttons |= in.dbg.buttons;
	if (in.dbg.stick_x)
		p->stick_x = in.dbg.stick_x;
	if (in.dbg.stick_y)
		p->stick_y = in.dbg.stick_y;
#endif
}

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
	float dt = (float)dt_us * 1e-6f, sx, sy, fm, sm, spd, look;
	uint32_t ms = (dt_us + 500u) / 1000u;
	uint16_t b = in.held;
	unsigned i;

	pad_read(&p);
	/* buttons held when the menu closed stay ignored until released */
	in.mask &= p.buttons;
	p.buttons &= (uint16_t)~in.mask;
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

	/* view: stick x and the turn buttons yaw, C-up / C-down pitch, at the
	 * menu's look sensitivity (percent) */
	look = (float)settings.look_sens / 100.0f;
	in.ang[1] -= sx * settings.yawspeed * look * dt;
	if (b & IN_BTN_LEFT)
		in.ang[1] += settings.yawspeed * look * dt;
	if (b & IN_BTN_RIGHT)
		in.ang[1] -= settings.yawspeed * look * dt;
	if (p.buttons & (SYS_PAD_C_UP | SYS_PAD_C_DOWN))
	{
		float d = settings.pitchspeed * look * dt *
			((p.buttons & SYS_PAD_C_UP) ? -1.0f : 1.0f);

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

uint16_t in_keys(uint32_t dt_us)
{
	static const uint16_t dir[4][2] = {
		{ IN_KEY_UP, SYS_PAD_D_UP }, { IN_KEY_DOWN, SYS_PAD_D_DOWN },
		{ IN_KEY_LEFT, SYS_PAD_D_LEFT }, { IN_KEY_RIGHT, SYS_PAD_D_RIGHT },
	};
	sys_pad_t p;
	uint16_t held = 0, down, out;
	int k;

	pad_read(&p);
	for (k = 0; k < 4; k++)
		if (p.buttons & dir[k][1])
			held |= dir[k][0];
	if (p.stick_y >= STICK_KEY)
		held |= IN_KEY_UP;
	else if (p.stick_y <= -STICK_KEY)
		held |= IN_KEY_DOWN;
	if (p.stick_x >= STICK_KEY)
		held |= IN_KEY_RIGHT;
	else if (p.stick_x <= -STICK_KEY)
		held |= IN_KEY_LEFT;
	if (p.buttons & SYS_PAD_A)
		held |= IN_KEY_A;
	if (p.buttons & SYS_PAD_B)
		held |= IN_KEY_B;
	if (p.buttons & SYS_PAD_START)
		held |= IN_KEY_START;
	down = (uint16_t)(held & ~in.keys_last);
	out = down;
	/* directions: a new one starts the repeat clock; a held one repeats */
	if (down & IN_KEY_DIRS)
		in.rep_us = REPEAT_FIRST;
	else if (held & IN_KEY_DIRS)
	{
		if (in.rep_us > dt_us)
			in.rep_us -= dt_us;
		else
		{
			in.rep_us = REPEAT_NEXT;
			out |= (uint16_t)(held & IN_KEY_DIRS);
		}
	}
	in.keys_last = held;
	return out;
}

void in_hold(void)
{
	sys_pad_t p;

	pad_read(&p);
	in.mask = p.buttons;
	in.pad_last = 0;
	in.pend = 0;
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

#ifdef HL64_TEST
/* pad buttons stick_x stick_y: the pad held as given (SYS_PAD_* bits, stick
 * -128..127) until the next pad command */
static int cmd_pad(const dbg_cmd_t *c)
{
	float x = c->arg[1].f, y = c->arg[2].f;

	if (c->arg[0].u > 0xFFFFu || !(x >= -128 && x <= 127) || !(y >= -128 && y <= 127))
		return -1;
	in.dbg.buttons = (uint16_t)c->arg[0].u;
	in.dbg.stick_x = (int8_t)x;
	in.dbg.stick_y = (int8_t)y;
	return 0;
}
#endif

void in_init(void)
{
	memset(&in, 0, sizeof(in));
	dbg_handler_set(DBG_BUTTONS, cmd_buttons);
	dbg_handler_set(DBG_IMPULSE, cmd_impulse);
#ifdef HL64_TEST
	dbg_handler_set(DBG_PAD, cmd_pad);
#endif
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
