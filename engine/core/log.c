/* SPDX-License-Identifier: MIT */
/*
 * log.c -- the log ring (log.h). Text is formatted once into the ring;
 * head and flushed are byte counts since boot, so the unflushed span is
 * head - flushed. When unflushed text would be overwritten the oldest is
 * dropped and the next flush reports how much.
 */
#include <stdio.h>

#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"

#define RING_MASK	(HL64_LOG_RING_BYTES - 1u)

typedef char ring_size_is_pow2[(HL64_LOG_RING_BYTES & RING_MASK) == 0 ? 1 : -1];

static char	ring[HL64_LOG_RING_BYTES];
static uint32_t	head, flushed, dropped;

void log_vprintf(const char *fmt, va_list ap)
{
	char line[HL64_LOG_LINE_MAX];
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	int i;

	if (n < 0)
		return;
	if (n >= (int)sizeof(line))
		n = (int)sizeof(line) - 1;
	for (i = 0; i < n; i++)
		ring[head++ & RING_MASK] = line[i];
	if (head - flushed > HL64_LOG_RING_BYTES)
	{
		dropped += head - flushed - HL64_LOG_RING_BYTES;
		flushed = head - HL64_LOG_RING_BYTES;
	}
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
}

void log_flush(void)
{
	if (dropped)
	{
		char msg[48];
		int n = snprintf(msg, sizeof(msg), "log: %lu bytes dropped\n",
			(unsigned long)dropped);

		dropped = 0;
		sys_log_write(msg, (uint32_t)n);
	}
	while (flushed != head)
	{
		uint32_t at = flushed & RING_MASK;
		uint32_t n = head - flushed;

		if (n > HL64_LOG_RING_BYTES - at)
			n = HL64_LOG_RING_BYTES - at;
		sys_log_write(ring + at, n);
		flushed += n;
	}
}
