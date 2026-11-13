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
/* The PI completion interrupt's handler (cart: called in interrupt
 * context after every PI DMA, the chain's or not; host: no interrupt,
 * never called). Interrupts off / on around state a handler shares with
 * the main thread (nest; host: nothing). */
void		sys_pi_set_isr(void (*isr)(void));
void		sys_irq_off(void);
void		sys_irq_on(void);

#endif
