/* SPDX-License-Identifier: MIT */
/*
 * main.c -- N64 entry point (docs/architecture.md 3.1 step 1): libdragon
 * bring-up, then the engine's boot and frame loop. Retargeted from the
 * boot of the old tree's n64/port/quakegeneric_n64.c (audit entry
 * QUAKE-INTERFACE, D9): its libdragon / banked-arena / FPU / stack setup
 * is kept; the quakegeneric QG_* platform API, console scripts, key
 * codes and the template file header are not.
 *
 * Single-threaded on the main stack; libdragon's preview kernel is not
 * used (kthreads start with a fresh FPU state that would need its own
 * care).
 */
#include <libdragon.h>
#include <rba_ld.h>
#include <rdram_bank.h>
#include <t3d/t3d.h>

#include "hl64/limits.h"
#include "engine/core/dbg.h"
#include "engine/core/eng.h"
#include "engine/core/sys.h"
#include "engine/core/log.h"
#include "plat/n64/n64.h"

#define AUDIO_HZ	22050
#define AUDIO_BUFFERS	4
#define FB_BYTES	(320 * 240 * 2)

int main(void)
{
	debug_init_isviewer();
	debug_init_usblog();
	fpu_shim_install();
	sys_stack_paint();

	/* All FPU exceptions masked (as retail games run) and denormals
	 * flushed to zero (the VR4300 cannot represent them); libdragon's
	 * debug default enables the traps, which would turn a transient
	 * 0/0 or overflow into a crash. */
	C1_WRITE_FCR31((C1_FCR31() | C1_FCR31_FS) & ~C1_ENABLE_MASK);

	/* Bank arenas (libdragon-banked): framebuffers low, Z and stream at
	 * the top of RDRAM, so the RDP's colour and Z traffic open rows in
	 * different banks. Sized to what is allocated: each page-aligned
	 * buffer costs a page for its block header. */
	rba_ld_init(&(rba_ld_config_t){
		.fb_arena_bytes = 2 * (FB_BYTES + RBA_PAGE_SIZE) + RBA_PAGE_SIZE,
		.z_arena_bytes = FB_BYTES + 2 * RBA_PAGE_SIZE,
		.stream_arena_bytes = 128 * 1024,
	});
	/* VI_CRT_MARGIN shrinks the picture so CRTs don't clip the HUD */
	display_init((resolution_t){
		.width = 320,
		.height = 240,
		.interlaced = INTERLACE_OFF,
		.aspect_ratio = 0,
		.overscan_margin = VI_CRT_MARGIN,
	}, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
	rdpq_init();
	joypad_init();
	timer_init();
	audio_init(AUDIO_HZ, AUDIO_BUFFERS);
	t3d_init((T3DInitParams){});
	vid_init();

	if (get_memory_size() < HL64_RDRAM_BYTES)
		sys_error("hl64 needs the Expansion Pak\n(8 MB RDRAM)");
	if (dfs_init(DFS_DEFAULT_LOCATION) != DFS_ESUCCESS)
		log_printf("io: no DFS in this ROM\n");

	eng_boot("n64");
#ifdef HL64_TEST
	/* no route VM yet: a test ROM idles, then quits 0 through the harness */
	{
		dbg_cmd_t quit = { .frame = HL64_TEST_IDLE_FRAMES, .op = DBG_QUIT, .nargs = 1 };

		quit.arg[0].u = 0;
		dbg_push(&quit);
	}
#endif
	for (;;)
		eng_frame();
}
