/* SPDX-License-Identifier: MIT */
/*
 * sys.h -- the platform services the engine calls (docs/architecture.md 3).
 * plat/n64 implements them on libdragon, plat/host on POSIX with the
 * cart's limits; nothing else in engine/ touches the hardware.
 */
#ifndef HL64_SYS_H
#define HL64_SYS_H

#include <stddef.h>
#include <stdint.h>

/* ---- fatal paths ----------------------------------------------------- */

/* Log, flush, show the error screen (cart) and stop; test runs exit via
 * the harness with SYS_EXIT_ERROR. */
void		sys_error(const char *fmt, ...)
			__attribute__((noreturn, format(printf, 1, 2)));
/* Flush the log and leave: harness exit code (cart), exit status (host). */
void		sys_exit(int code) __attribute__((noreturn));
#define SYS_EXIT_ERROR	3

/* ---- clock, log sink -------------------------------------------------- */

/* Monotonic microseconds since boot (profiling; game time is frame dt). */
uint64_t	sys_time_us(void);
/* Raw bytes to the debug channel (ISViewer / stdout). */
void		sys_log_write(const char *s, uint32_t n);
/* PROFILE=1 builds: close a frame's platform I/O counters; log them per
 * nframes (once a second). */
void		sys_prof_frame(void);
void		sys_prof_dump(unsigned nframes);
/* FPU traps emulated since boot (cart fpu_shim; 0 on the host). */
uint32_t	sys_fpu_traps(void);

/* ---- video: VI clock, frame bracket, paced present (3.2, 3.3) -------- */

uint32_t	sys_vi_count(void);		/* vblanks since boot */
uint32_t	sys_vi_period_us(void);
/* Take a free framebuffer and start the frame; 0 while none is free (the
 * caller pumps I/O and retries). */
int		sys_frame_try_begin(void);
/* Submit the frame; it is shown by the pacing gate. */
void		sys_frame_end(void);
/* Shown-frame interval histogram since the last call, "pace ..." text. */
int		sys_pace_stats(char *buf, int size);
/* Dump the last shown frame (harness); -1 where there is none. */
int		sys_shot(void);

/* ---- memory ---------------------------------------------------------- */

/* The boot carve's one block, 16-byte aligned; NULL if it does not fit. */
void		*sys_carve_alloc(uint32_t bytes);
/* Image (code + data + bss) bytes; 0 where not meaningful (host). */
uint32_t	sys_image_bytes(void);
/* Deepest stack use in bytes, -1 where not tracked. */
int		sys_stack_peak(void);
/* Write CPU-cached bytes back to RDRAM before the RSP/RDP reads them
 * (no-op on the host). */
void		sys_cache_writeback(const void *p, uint32_t n);

/* ---- cooperative contexts (engine/core/co.c) --------------------------- */

/* A context runs entry() on its own stack [stack, stack + bytes) from its
 * first sys_co_resume until it calls sys_co_yield, which returns to the
 * sys_co_resume; the next sys_co_resume continues after that yield. entry
 * never returns (re-init the context instead). One level only: a context
 * never resumes another. The register state saved is the ABI's
 * callee-saved set (cart: libdragon ucontext, s0-s8 / ra / sp, f20-f31,
 * FCR31 -- a new context starts with the main stack's FCR31; host: glibc
 * ucontext). The storage is opaque. */
typedef struct {
	uint64_t	opaque[64];
} sys_co_t;
void		sys_co_init(sys_co_t *co, void *stack, uint32_t bytes, void (*entry)(void));
void		sys_co_resume(sys_co_t *co);
void		sys_co_yield(sys_co_t *co);

/* ---- controller (11; M1: the free camera's debug pad) ------------------ */

typedef struct {
	int8_t		stick_x, stick_y;	/* -128..127, right / up positive */
	uint16_t	buttons;		/* SYS_PAD_* held */
} sys_pad_t;

#define SYS_PAD_A	0x0001
#define SYS_PAD_B	0x0002
#define SYS_PAD_Z	0x0004
#define SYS_PAD_START	0x0008
#define SYS_PAD_L	0x0010
#define SYS_PAD_R	0x0020
#define SYS_PAD_C_UP	0x0040
#define SYS_PAD_C_DOWN	0x0080
#define SYS_PAD_C_LEFT	0x0100
#define SYS_PAD_C_RIGHT	0x0200
#define SYS_PAD_D_UP	0x0400
#define SYS_PAD_D_DOWN	0x0800
#define SYS_PAD_D_LEFT	0x1000
#define SYS_PAD_D_RIGHT	0x2000

/* Port 1 now (the host's scripted pad reads as idle). */
void		sys_pad_read(sys_pad_t *p);

/* ---- cart files and the PI (5.1, 5.2) -------------------------------- */

/* PI address and size of a loose cart file, 0 if none. */
uint32_t	sys_cart_addr(const char *path, uint32_t *size);
/* Synchronous DMA of [off, off + len) of a file at rom into dst (len
 * rounded up to even must fit dst_cap); 0 on a precondition failure. */
int		sys_cart_read(void *dst, size_t dst_cap, uint32_t rom,
			uint32_t rom_size, uint32_t off, uint32_t len);
/* engine/io backend: start one piece, poll, wait (returns blocked us),
 * synchronous read (returns blocked us), modelled PI us (host; 0 cart).
 * sys_pi_wait's audio: the waiter is the mixer's AUDIO read. A host
 * piece lands when it is launched (sys_pi_busy is always 0 there). */
void		sys_pi_launch(void *dst, uint32_t rom, uint32_t len);
int		sys_pi_busy(void);
uint32_t	sys_pi_wait(int audio);
uint32_t	sys_pi_read(void *dst, uint32_t rom, uint32_t len);
uint32_t	sys_pi_cost_us(uint32_t len);
/* The PI completion handler (cart: called after every PI DMA, the chain's
 * or not, with interrupts off: from the interrupt, or nested in a libdragon
 * DMA call (sys_pi_*, the mixer's, DFS) whose queue poll took the
 * completion; host: no interrupt, never called). Interrupts off / on
 * around state a handler shares with the main thread (nest; host: nothing). */
void		sys_pi_set_isr(void (*isr)(void));
void		sys_irq_off(void);
void		sys_irq_on(void);

/* ---- FlashRAM save chip (D4, docs/architecture.md 14) --------------------
 * 128 KB: 8 sectors of 16 KB, pages of 128 B (erase sets 0xFF). The cart
 * drives the chip's command register at PI 0x08000000 (DOM2) with the io
 * chain held around each register access / DMA, so streaming pieces never
 * wait more than one such access; the host keeps the image in a file
 * (hl64-host --save FILE). Erase and program only start the operation:
 * sys_flash_poll reports when it ends. One operation at a time. */
int		sys_flash_init(void);		/* boot: 1 chip present, 0 none */
/* Blocking read of [off, off + len) into dst: off and len even, dst
 * 16-aligned; 0 ok, -1 no chip / bad arguments. */
int		sys_flash_read(void *dst, uint32_t off, uint32_t len);
int		sys_flash_erase(uint32_t sector);	/* 0 started, -1 no chip / busy */
/* Program page (128 B) from src (16-aligned); 0 started, -1 no chip / busy. */
int		sys_flash_program(uint32_t page, const void *src);
/* 1 the operation runs, 0 it ended ok (or none), -1 it failed. */
int		sys_flash_poll(void);

/* ---- Controller Pak in controller 1 (D4, docs/architecture.md 14) --------
 * This game's notes on the Pak (game code "HL64", publisher code "HL",
 * file names "HL64SAVE.<id>", id 0..15; plat/pakfs.h, shared by the cart
 * and the host); other games' notes and the Pak's
 * file system stay intact whatever happens to ours. The host keeps a
 * 32 KB Pak image in a file (hl64-host --pak FILE; no --pak: nothing in
 * the controller). Reads block; a write is open / steps / close, each step
 * bounded by its byte count, so the caller spreads it over frames. */
enum { SYS_PAK_ABSENT, SYS_PAK_CPAK, SYS_PAK_RUMBLE, SYS_PAK_OTHER };
/* What is in controller 1 now (cheap: every frame). */
int		sys_pak_accessory(void);
/* Mount the Pak's file system after SYS_PAK_CPAK appears: 0 ok, -1
 * unformatted / damaged (nothing mounted). Unmount when it goes. */
int		sys_pak_mount(void);
void		sys_pak_unmount(void);
/* Free bytes (whole pages) and free note entries; -1 not mounted. */
int		sys_pak_stat(uint32_t *free_bytes, int *free_notes);
/* Our notes: their ids (ascending) and sizes; count, -1 not mounted. */
int		sys_pak_notes(uint8_t *ids, uint32_t *bytes, int max);
/* Blocking read of [off, off + len) of note id: 0 ok, -1 failed. */
int		sys_pak_read(int id, void *dst, uint32_t off, uint32_t len);
/* Create note id (it must not exist) of len bytes, all pages allocated:
 * 0 ok, -1 no room / failed. Then sys_pak_write any [off, off + len)
 * inside it (save.c writes its header last: a note whose header does not
 * check out is a corrupt save, never another game's damage) and
 * sys_pak_close. */
int		sys_pak_create(int id, uint32_t len);
int		sys_pak_write(const void *src, uint32_t off, uint32_t len);	/* 0 ok, -1 failed */
int		sys_pak_close(void);		/* 0 ok, -1 failed */
int		sys_pak_delete(int id);		/* 0 ok, -1 failed */

#endif
