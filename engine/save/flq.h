/* SPDX-License-Identifier: MIT */
/*
 * flq.h -- the flash writer (docs/architecture.md 14): erases and programs
 * a sector-aligned range of the FlashRAM image from a RAM buffer a little
 * per frame, within a time budget, never blocking on the chip (the erase /
 * program run while the frame does other work; their status is polled on
 * the next step). Pages that are all 0xFF are not programmed.
 */
#ifndef HL64_FLQ_H
#define HL64_FLQ_H

#include <stdint.h>

/* Start writing src[0, len) at off (off a multiple of the 16 KB sector;
 * len rounded up to whole pages, the rest of the last page 0xFF; the
 * sectors [off, off + len) rounded up are erased). src stays owned by the
 * caller and must not change until flq_busy() is 0. 0 started, -1 busy /
 * no chip / bad range. */
int	flq_write(uint32_t off, const void *src, uint32_t len);
/* One frame's step: work for at most budget_us, then return. */
void	flq_step(uint32_t budget_us);
int	flq_busy(void);		/* 1 while a write runs */
int	flq_result(void);	/* last write: 0 ok, -1 failed (erase / program error) */

/* Read [off, off + len) of the image (blocking; save.c's directory and
 * load reads): 0 ok, -1 no chip / bad range. dst 16-aligned, off and len
 * even. */
int	flq_read(void *dst, uint32_t off, uint32_t len);
/* The test build's `flash` debug command (save.c registers it):
 * 0 = log the chip probe and the first bytes of every sector;
 * 1 = self-test: write a known pattern over the last sector through
 * flq_write (stepping it to the end inside the command), read it back,
 * log `flash: selftest ok` or the first mismatch. */
int	flq_dbg_flash(uint32_t arg);

#endif
