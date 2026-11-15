/* SPDX-License-Identifier: MIT */
/*
 * sv.h -- the save stream (docs/architecture.md 14): one writer or one
 * reader at a time, LZ-compressed into / out of the slot image. Sections
 * follow each other in a fixed order, each opened by a 4-byte tag; a
 * reader that meets another tag, or runs past the end, marks the save
 * corrupt (sv_failed) and every later get reads zeros. Values are in the
 * machine's own byte order (a save never moves between cart and host).
 */
#ifndef HL64_SV_H
#define HL64_SV_H

#include <stdint.h>

#define SV_TAG(a, b, c, d) \
	((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- writer ------------------------------------------------------------- */

void		sv_tag(uint32_t tag);
void		sv_put(const void *p, uint32_t n);
void		sv_u8(uint32_t v);
void		sv_u16(uint32_t v);
void		sv_u32(uint32_t v);
void		sv_f32(float v);
void		sv_f64(double v);
/* n bytes against ref (NULL: zeros): per 8 32-bit words a mask byte of
 * the words that differ, then those words; the n % 4 tail as it is. The
 * cost of a row near its reference is about n / 32 bytes. */
void		sv_put_delta(const void *p, const void *ref, uint32_t n);
/* Test builds: a part of the stream starts here (name: a static string)
 * for the per-part size log; the stream's bytes are unchanged. */
#ifdef HL64_TEST
void		sv_part(const char *name);
/* on 1 / off 0: a test-only check runs between them; its time is left out
 * of the snapshot's and the frame's figures (engine/save/save.c) */
void		sv_check_time(int on);
#else
#define sv_part(name)	((void)(name))
#endif

/* Memory a section may use while the stream is written or read (the
 * coder's work area, idle then), 16-aligned; *bytes its size. Nothing in
 * it lasts from one section to the next. */
void		*sv_work_area(uint32_t *bytes);

/* ---- reader ------------------------------------------------------------- */

/* The next section's tag must be tag: 0, else -1 (corrupt). */
int		sv_expect(uint32_t tag);
void		sv_get(void *p, uint32_t n);
uint32_t	sv_get_u8(void);
uint32_t	sv_get_u16(void);
uint32_t	sv_get_u32(void);
float		sv_get_f32(void);
double		sv_get_f64(void);
/* A sv_put_delta row: p = ref (NULL: zeros; p may be ref) with the saved
 * words; mask (NULL: not wanted) gets the (n / 4 + 7) / 8 mask bytes. */
void		sv_get_delta(void *p, const void *ref, uint32_t n, uint8_t *mask);
/* Mark the save being read corrupt (a value out of range), with a reason
 * for the log; the first reason is kept. */
void		sv_fail(const char *why);
int		sv_failed(void);

#endif
