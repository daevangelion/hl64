/* SPDX-License-Identifier: MIT */
/*
 * vid.c -- VI clock, frame bracket and paced present (architecture 3.2,
 * 3.3). The pacing gate, its histogram and the frame bracket are imported
 * from the old tree's n64/port/r64_vid.c, split per its audit entry: only
 * the CLEAN ranges (59-150 pacing, 214-251 frame bracket); VID_Init
 * (162-172) and the D_*DirectRect stubs (252-258) are not used, and the
 * Quake viddef/palette bindings are gone.
 *
 * A frame is shown at most every HL64_PRESENT_VBL vblanks (2: 30 fps
 * NTSC, 25 PAL) and otherwise on the first vblank after the RDP finishes
 * it, so a 40 ms frame shows after 50 ms rather than 67. (libdragon's
 * display_set_fps_limit instead skips every other vblank, which turns any
 * frame over 33 ms into a 67 ms one.) With two framebuffers the next
 * frame's simulation runs while a finished frame waits here; the frame
 * after that waits in sys_frame_try_begin until the flip frees a buffer.
 *
 * The RDP-done callback shows the frame if the gate is open and parks it
 * otherwise; the vblank handler shows a parked frame once the gate opens.
 * display_show at vblank count v flips at v + 1: libdragon's handler runs
 * before ours (installed later) in the same interrupt.
 */
#include <libdragon.h>

#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "plat/n64/harness.h"
#include "plat/n64/n64.h"

#define PACE_BINS 8

static volatile uint32_t pace_vbl;	/* vblanks since vid_init */
static volatile uint32_t pace_open;	/* first vblank count a show may happen */
static volatile uint32_t pace_last;	/* vblank count of the last show */
static surface_t *volatile pace_parked;
/* shown-frame intervals in vblanks (bin PACE_BINS-1 = that or longer) */
static volatile uint32_t pace_hist[PACE_BINS];
static volatile uint32_t pace_vbl_mark;	/* pace_vbl at the last sys_pace_stats */

static uint32_t		vi_period_us;
static surface_t	*disp;		/* frame being drawn */
static surface_t	*last;		/* last submitted frame */

static void pace_show(surface_t *s)
{
	uint32_t iv = pace_vbl - pace_last;

	display_show(s);
	if (pace_last)
		pace_hist[iv < PACE_BINS ? iv : PACE_BINS - 1]++;
	pace_last = pace_vbl ? pace_vbl : 1;
	pace_open = pace_vbl + HL64_PRESENT_VBL;
}

static void pace_rdp_done(void *arg)
{
	if ((int32_t)(pace_vbl - pace_open) >= 0)
		pace_show(arg);
	else
		pace_parked = arg;
}

static void pace_vblank(void *arg)
{
	surface_t *s = pace_parked;

	(void)arg;
	pace_vbl++;
	if (s && (int32_t)(pace_vbl - pace_open) >= 0)
	{
		pace_parked = NULL;
		pace_show(s);
	}
}

void vid_init(void)
{
	vi_period_us = (uint32_t)(1000000.0f / display_get_refresh_rate() + 0.5f);
	/* after display_init's handler, so it runs after libdragon's flip */
	vi_install_vblank_handler(pace_vblank, NULL);
}

void vid_abort_frame(void)
{
	if (!disp)
		return;
	rdpq_detach_wait();
	display_show(disp);
	disp = NULL;
}

uint32_t sys_vi_count(void)
{
	return pace_vbl;
}

uint32_t sys_vi_period_us(void)
{
	return vi_period_us;
}

int sys_frame_try_begin(void)
{
	/* NULL until a framebuffer is free: the RDP is still drawing the one
	 * we would reuse, or (paced) the parked frame has not flipped yet */
	disp = display_try_get();
	if (!disp)
		return 0;
	rdpq_attach_clear(disp, display_get_zbuf());
	return 1;
}

void sys_frame_end(void)
{
	last = disp;
	disp = NULL;
	rdpq_detach_cb(pace_rdp_done, last);
}

/*
 * Shown frames since the last call by interval: "pace vbl=N fps=F
 * v1..v7+=..." (one vblank = 16.7 ms NTSC / 20 ms PAL). Resets the counts.
 */
int sys_pace_stats(char *buf, int size)
{
	uint32_t h[PACE_BINS], vbl, n = 0;
	float hz = display_get_refresh_rate();
	int i, len;

	disable_interrupts();
	for (i = 0; i < PACE_BINS; i++)
	{
		h[i] = pace_hist[i];
		pace_hist[i] = 0;
		n += h[i];
	}
	vbl = pace_vbl - pace_vbl_mark;
	pace_vbl_mark = pace_vbl;
	enable_interrupts();
	len = snprintf(buf, (size_t)size, "pace vbl=%lu fps=%.1f v1..v%d+=%lu/%lu/%lu/%lu/%lu/%lu/%lu",
		(unsigned long)vbl, vbl ? (double)(n * hz / vbl) : 0.0, PACE_BINS - 1,
		(unsigned long)h[1], (unsigned long)h[2], (unsigned long)h[3],
		(unsigned long)h[4], (unsigned long)h[5], (unsigned long)h[6],
		(unsigned long)h[7]);
	return len < size ? len : size - 1;
}

int sys_shot(void)
{
	if (!last || !th_present())
		return -1;
	rspq_wait();
	th_screenshot(last->buffer, last->width, last->height, 2);
#ifdef HL64_PSAMP
	psamp_dump();
#endif
	return 0;
}
