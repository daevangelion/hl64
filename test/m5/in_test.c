/* SPDX-License-Identifier: MIT */
/*
 * in_test.c -- host test of the pad binding (engine/in/in.c,
 * docs/controls.md), run by test/run/m5.sh. The driver is the pad
 * (sys_pad_read --wrap): held buttons hold their command button while
 * down, D-up / D-left / D-right / Start send impulse 100 / 7 / 6 / 8 once
 * per press, presses of the same frame go out one per command in table
 * order, and a route / debug impulse goes before them. Exit 0 and
 * `in_test PASS` when every check passes; each failure prints one line.
 */
#include <stdio.h>
#include <string.h>

#include "hl64/settings.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"

static int fails;
static uint16_t pad;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

void __wrap_sys_pad_read(sys_pad_t *p);
void __wrap_sys_pad_read(sys_pad_t *p)
{
	p->stick_x = 0;
	p->stick_y = 0;
	p->buttons = pad;
}

static in_cmd_t frame(uint16_t buttons)
{
	in_cmd_t c;

	pad = buttons;
	in_frame(33333, 0, &c);
	return c;
}

static void test_held(void)
{
	static const struct {
		uint16_t	pad, btn;
		const char	*name;
	} held[] = {
		{ SYS_PAD_A, IN_BTN_JUMP, "A jump" },
		{ SYS_PAD_B, IN_BTN_USE, "B use" },
		{ SYS_PAD_Z, IN_BTN_ATTACK, "Z attack" },
		{ SYS_PAD_R, IN_BTN_ATTACK2, "R attack2" },
		{ SYS_PAD_L, IN_BTN_DUCK, "L duck" },
		{ SYS_PAD_C_LEFT, IN_BTN_MOVELEFT, "C-left moveleft" },
		{ SYS_PAD_C_RIGHT, IN_BTN_MOVERIGHT, "C-right moveright" },
		{ SYS_PAD_D_DOWN, IN_BTN_RELOAD, "D-down reload" },
	};
	unsigned i;

	in_init();
	for (i = 0; i < sizeof(held) / sizeof(held[0]); i++)
	{
		in_cmd_t c = frame(held[i].pad);

		CHECK(c.buttons == held[i].btn && !c.impulse, "%s: buttons %#x impulse %u",
			held[i].name, c.buttons, c.impulse);
		c = frame(held[i].pad);
		CHECK(c.buttons == held[i].btn && !c.impulse, "%s held: buttons %#x impulse %u",
			held[i].name, c.buttons, c.impulse);
		c = frame(0);
		CHECK(!c.buttons && !c.impulse, "%s released: buttons %#x impulse %u", held[i].name,
			c.buttons, c.impulse);
	}
	/* D-down reloads while firing: both held */
	{
		in_cmd_t c = frame(SYS_PAD_Z | SYS_PAD_D_DOWN);

		CHECK(c.buttons == (IN_BTN_ATTACK | IN_BTN_RELOAD), "Z + D-down: buttons %#x",
			c.buttons);
	}
	/* the turn / pitch buttons hold no command button */
	{
		in_cmd_t c = frame(SYS_PAD_C_UP);

		CHECK(!c.buttons && !c.impulse, "C-up: buttons %#x impulse %u", c.buttons, c.impulse);
	}
}

static void test_edges(void)
{
	static const struct {
		uint16_t	pad;
		uint8_t		impulse;
		const char	*name;
	} imp[] = {
		{ SYS_PAD_D_UP, 100, "D-up flashlight" },
		{ SYS_PAD_D_LEFT, 7, "D-left invprev" },
		{ SYS_PAD_D_RIGHT, 6, "D-right invnext" },
		{ SYS_PAD_START, 8, "Start lastinv" },
	};
	unsigned i, k;

	in_init();
	for (i = 0; i < sizeof(imp) / sizeof(imp[0]); i++)
	{
		in_cmd_t c = frame(imp[i].pad);

		CHECK(c.impulse == imp[i].impulse && !c.buttons, "%s press: impulse %u buttons %#x",
			imp[i].name, c.impulse, c.buttons);
		/* held: no repeat */
		for (k = 0; k < 10; k++)
		{
			c = frame(imp[i].pad);
			CHECK(!c.impulse, "%s held frame %u: impulse %u", imp[i].name, k, c.impulse);
		}
		c = frame(0);
		CHECK(!c.impulse, "%s released: impulse %u", imp[i].name, c.impulse);
		/* a second press: a second impulse */
		c = frame(imp[i].pad);
		CHECK(c.impulse == imp[i].impulse, "%s second press: impulse %u", imp[i].name,
			c.impulse);
		c = frame(0);
	}
}

static void test_queue(void)
{
	in_cmd_t c;

	/* two presses in one frame: one impulse per command, table order
	 * (D-up before D-right), nothing repeated while they stay down */
	in_init();
	c = frame(SYS_PAD_D_UP | SYS_PAD_D_RIGHT);
	CHECK(c.impulse == 100, "D-up + D-right frame 1: impulse %u, want 100", c.impulse);
	c = frame(SYS_PAD_D_UP | SYS_PAD_D_RIGHT);
	CHECK(c.impulse == 6, "D-up + D-right frame 2: impulse %u, want 6", c.impulse);
	c = frame(SYS_PAD_D_UP | SYS_PAD_D_RIGHT);
	CHECK(!c.impulse, "D-up + D-right frame 3: impulse %u, want 0", c.impulse);
	c = frame(0);
	CHECK(!c.impulse, "released: impulse %u", c.impulse);

	/* a route / debug impulse goes first; the pad press next command */
	in_init();
	in_impulse(101);
	c = frame(SYS_PAD_D_UP);
	CHECK(c.impulse == 101, "route impulse + D-up frame 1: impulse %u, want 101", c.impulse);
	c = frame(SYS_PAD_D_UP);
	CHECK(c.impulse == 100, "route impulse + D-up frame 2: impulse %u, want 100", c.impulse);
	c = frame(0);
	CHECK(!c.impulse, "route impulse + D-up frame 3: impulse %u", c.impulse);

	/* route held buttons and pad held buttons combine */
	in_init();
	in_buttons(IN_BTN_FORWARD, 0);
	c = frame(SYS_PAD_D_DOWN);
	CHECK((c.buttons & (IN_BTN_FORWARD | IN_BTN_RELOAD)) == (IN_BTN_FORWARD | IN_BTN_RELOAD),
		"route +forward + D-down: buttons %#x", c.buttons);
}

int main(void)
{
	settings_reset();
	test_held();
	test_edges();
	test_queue();
	printf("in_test %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails != 0;
}
