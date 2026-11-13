/* SPDX-License-Identifier: MIT */
/*
 * sys.c -- cart system services (engine/core/sys.h): the fatal-error
 * screen, exit through the test harness, the ISViewer log sink, the
 * microsecond clock, the carve block, image and stack measurement, and a
 * non-allocating RSP crash report. Written for hl64 from the description
 * in docs/architecture.md 16.1 (the old sys_n64.c is not used).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <libdragon.h>
#include <system.h>	/* stdio_t, hook_stdio_calls */

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/n64/harness.h"
#include "plat/n64/n64.h"

/* ---- error screen and exit ---------------------------------------------- */

/* Paint a title and a message (split at '\n' and at the screen width) on a
 * black screen with libdragon's built-in 8x8 font, then show it. */
static void paint_screen(const char *title, const char *msg)
{
	enum { X0 = 24, Y0 = 24, COLS = (320 - 2 * X0) / 8, ROWS = (240 - 2 * Y0) / 10 };
	char line[COLS + 1];
	surface_t *d;
	int y = Y0 + 16, row = 0;

	vid_abort_frame();
	d = display_get();
	graphics_fill_screen(d, 0);
	graphics_set_color(0xFFFFFFFF, 0x00000000);
	graphics_draw_text(d, X0, Y0, title);
	while (*msg && row < ROWS)
	{
		int n = 0;

		while (msg[n] && msg[n] != '\n' && n < COLS)
			n++;
		memcpy(line, msg, (size_t)n);
		line[n] = 0;
		graphics_draw_text(d, X0, y, line);
		msg += n;
		if (*msg == '\n')
			msg++;
		y += 10;
		row++;
	}
	display_show(d);
}

void sys_error(const char *fmt, ...)
{
	static int nested;
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (nested++)
	{
		/* failed while failing: raw text only */
		fprintf(stderr, "error (nested): %s\n", msg);
		for (;;)
			;
	}
	log_printf("error: %s\n", msg);
	log_flush();
	paint_screen("hl64 error", msg);
	th_exit(SYS_EXIT_ERROR);
	for (;;)
		;
}

void sys_exit(int code)
{
	char msg[32];

	log_printf("exit %d\n", code);
	log_flush();
	th_exit(code);
	/* hardware: nothing to return to */
	snprintf(msg, sizeof(msg), "exit %d", code);
	paint_screen("hl64 halted", msg);
	for (;;)
		;
}

/* ---- clock, log sink ------------------------------------------------------ */

uint64_t sys_time_us(void)
{
	return get_ticks_us();
}

/* stderr is libdragon's debug channel (ISViewer, flashcart USB), unbuffered */
void sys_log_write(const char *s, uint32_t n)
{
	fwrite(s, 1, n, stderr);
}

uint32_t sys_fpu_traps(void)
{
	return (uint32_t)fpu_shim_trap_count();
}

/* ---- memory --------------------------------------------------------------- */

/* malloc + 15 and align by hand, not memalign: memalign's lead / tail
 * give-back depends on where the heap top happens to sit, which moved
 * the heap left after the carve by up to 24 B between builds; this block
 * always costs bytes + 24 B (dlmalloc chunk), so the host's mirror of
 * the heap left (limits.h HL64_CART_BOOT_BYTES) is exact. */
void *sys_carve_alloc(uint32_t bytes)
{
	char *p = malloc((size_t)bytes + 15u);

	return p ? (void *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u) : NULL;
}

uint32_t sys_image_bytes(void)
{
	extern char __text_start[], __bss_end[];

	return (uint32_t)(__bss_end - __text_start);
}

void sys_cache_writeback(const void *p, uint32_t n)
{
	data_cache_hit_writeback(p, n);
}

/*
 * Stack: libdragon's 64 KiB at the top of RDRAM; the banked Z arena sits
 * directly below it, so an overflow silently corrupts depth. Boot paints
 * the unused stack once and sys_stack_peak() reports the deepest byte
 * ever written.
 */
#define STACK_BYTES	0x10000
#define STACK_PAINT	0x5a17c0deu

static uint32_t *stack_lo, *stack_hi;

void __attribute__((noinline)) sys_stack_paint(void)
{
	volatile uint32_t here;
	uint32_t *p, *end;

	stack_hi = (uint32_t *)(0x80000000u + (uint32_t)get_memory_size());
	stack_lo = stack_hi - STACK_BYTES / 4;
	end = (uint32_t *)((uintptr_t)&here & ~(uintptr_t)3) - 256;	/* 1 KiB below this frame */
	for (p = stack_lo; p < end; p++)
		*p = STACK_PAINT;
}

int sys_stack_peak(void)
{
	const uint32_t *p = stack_lo;

	if (!p)
		return -1;
	while (p < stack_hi && *p == STACK_PAINT)
		p++;
	return (int)((stack_hi - p) * 4);
}

/* ---- RSP crash report without allocation ---------------------------------- */

/*
 * libdragon's RSP crash handler snapshots the RSP and then calls
 * console_init(), which allocates a framebuffer pair; on a full heap that
 * fails before any register is printed. The Makefile wraps console_init
 * and console_render (only the crash handler calls them in hl64): stdout
 * goes unbuffered to the debug channel, the start of the report is kept in
 * a static buffer, and console_render -- called just before abort --
 * raises an assertion carrying that text so libdragon's exception screen,
 * which needs no heap, shows it.
 */
static char	rsp_crash_text[1024];
static size_t	rsp_crash_len;

static int rsp_crash_write(char *data, unsigned int len)
{
	size_t room = sizeof(rsp_crash_text) - 1 - rsp_crash_len;
	size_t n = len < room ? len : room;

	memcpy(rsp_crash_text + rsp_crash_len, data, n);
	rsp_crash_len += n;
	fwrite(data, 1, len, stderr);
	return (int)len;
}

void __wrap_console_init(void);
void __wrap_console_render(void);

void __wrap_console_init(void)
{
	static stdio_t calls = { NULL, rsp_crash_write, NULL };

	setvbuf(stdout, NULL, _IONBF, 0);
	hook_stdio_calls(&calls);
}

void __wrap_console_render(void)
{
	rsp_crash_text[rsp_crash_len] = 0;
	debug_assert_func_f(__FILE__, __LINE__, __func__, "RSP crash (dump above)",
		"%s", rsp_crash_text);
}

/* ---- controller ------------------------------------------------------------ */

void sys_pad_read(sys_pad_t *p)
{
	joypad_inputs_t in;

	joypad_poll();
	in = joypad_get_inputs(JOYPAD_PORT_1);
	p->stick_x = in.stick_x;
	p->stick_y = in.stick_y;
	p->buttons = (uint16_t)((in.btn.a ? SYS_PAD_A : 0) | (in.btn.b ? SYS_PAD_B : 0)
		| (in.btn.z ? SYS_PAD_Z : 0) | (in.btn.start ? SYS_PAD_START : 0)
		| (in.btn.l ? SYS_PAD_L : 0) | (in.btn.r ? SYS_PAD_R : 0)
		| (in.btn.c_up ? SYS_PAD_C_UP : 0) | (in.btn.c_down ? SYS_PAD_C_DOWN : 0)
		| (in.btn.c_left ? SYS_PAD_C_LEFT : 0) | (in.btn.c_right ? SYS_PAD_C_RIGHT : 0));
}
