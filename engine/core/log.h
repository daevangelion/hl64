/* SPDX-License-Identifier: MIT */
/*
 * log.h -- the 8 KB log ring (docs/architecture.md 12). Lines go into the
 * ring; log_flush() sends what is new to the debug channel (ISViewer on
 * the cart, stdout on the host) once per frame, before an error screen
 * and at exit. The ring keeps the last HL64_LOG_RING_BYTES of text for
 * the debug overlay. No console, no command buffer, no notify lines.
 */
#ifndef HL64_LOG_H
#define HL64_LOG_H

#include <stdarg.h>

void	log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void	log_vprintf(const char *fmt, va_list ap);
void	log_flush(void);

#endif
