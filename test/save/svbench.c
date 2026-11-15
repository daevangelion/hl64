/* SPDX-License-Identifier: MIT */
/*
 * svbench.c -- host bench of the save image coder (engine/save/sv.c):
 * per raw state dump (the coder's exact input, test/save/svdump.py) the
 * image size, the encode and decode times (thread CPU time, the best of
 * -r runs, one unbounded step each) and the steps at a 0 us budget (a
 * step per chunk); it checks the round trip, that the 0 us steps give
 * the same image and state, and that a short image does not decode.
 * -w writes each image to <dir>/<dump name>.img. Built standalone:
 *
 *   cc -m32 -std=c99 -O2 -Wall -Wextra -Werror -I. -Iinclude \
 *	test/save/svbench.c engine/save/sv.c -o svbench
 *   ./svbench [-r runs] [-w image-dir] raw-*.bin
 */
#define _POSIX_C_SOURCE 199309L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/save/sv.h"
#include "engine/save/sv_int.h"

#define RAW_CAP		(160u * 1024u)
#define OUT_CAP		(128u * 1024u)

uint64_t sys_time_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* the bench's clock: this thread's CPU time (a loaded machine) */
static uint64_t cpu_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static uint8_t raw[RAW_CAP], back[RAW_CAP], img[OUT_CAP], img0[OUT_CAP];
static uint8_t work[SV_WORK_BYTES] __attribute__((aligned(16)));

/* The image of raw[0, n) with steps of budget us: its size (0 failed),
 * *steps the sv_code_step calls. */
static uint32_t code(uint32_t n, uint8_t *out, uint32_t budget, uint32_t *steps)
{
	static uint8_t stage[RAW_CAP];

	sv_write_begin(stage, RAW_CAP, work);
	sv_put(raw, n);
	if (sv_write_end() != n)
		return 0;
	sv_code_begin(out, OUT_CAP, work);
	*steps = 1;
	while (sv_code_step(budget))
		++*steps;
	return sv_code_end();
}

int main(int argc, char **argv)
{
	int runs = 5, a = 1, r;
	const char *wdir = NULL;
	uint32_t traw = 0, timg = 0;
	uint64_t tenc = 0, tdec = 0;

	for (; a + 1 < argc && argv[a][0] == '-'; a += 2)
		if (!strcmp(argv[a], "-r"))
			runs = atoi(argv[a + 1]);
		else if (!strcmp(argv[a], "-w"))
			wdir = argv[a + 1];
		else
		{
			fprintf(stderr, "usage: svbench [-r runs] [-w image-dir] raw.bin...\n");
			return 2;
		}
	printf("%-22s %7s %7s %6s %9s %9s %6s %6s\n", "dump", "raw", "image", "%", "enc_us",
		"dec_us", "esteps", "dsteps");
	for (; a < argc; a++)
	{
		FILE *f = fopen(argv[a], "rb");
		uint32_t n, m = 0, m0, steps, steps0, dn = 0, dsteps = 1;
		uint64_t be = ~0ull, bd = ~0ull;
		const char *name = strrchr(argv[a], '/');

		name = name ? name + 1 : argv[a];
		if (!f)
		{
			fprintf(stderr, "%s: cannot open\n", argv[a]);
			return 1;
		}
		n = (uint32_t)fread(raw, 1, RAW_CAP, f);
		fclose(f);
		for (r = 0; r < runs; r++)
		{
			uint64_t t0 = cpu_us(), t1;

			m = code(n, img, ~0u, &steps);
			t1 = cpu_us() - t0;
			be = t1 < be ? t1 : be;
		}
		if (!m)
		{
			fprintf(stderr, "%s: the coder failed\n", argv[a]);
			return 1;
		}
		m0 = code(n, img0, 0, &steps0);
		if (m0 != m || memcmp(img, img0, m))
		{
			fprintf(stderr, "%s: a 0 us budget gives another image\n", argv[a]);
			return 1;
		}
		for (r = 0; r < runs; r++)
		{
			uint64_t t0 = cpu_us(), t1;
			int s;

			memset(back, 0xA5, n);
			sv_decode_begin(img, m, back, RAW_CAP);
			while ((s = sv_decode_step(~0u)) > 0)
				;
			dn = sv_decode_end();
			t1 = cpu_us() - t0;
			bd = t1 < bd ? t1 : bd;
			if (s < 0 || dn != n || memcmp(back, raw, n))
			{
				fprintf(stderr, "%s: round trip fails (%d, %u of %u B)\n", argv[a], s,
					(unsigned)dn, (unsigned)n);
				return 1;
			}
		}
		/* a budgeted decode (a step per chunk): the same bytes */
		memset(back, 0xA5, n);
		sv_decode_begin(img, m, back, RAW_CAP);
		while (sv_decode_step(0) > 0)
			dsteps++;
		if (sv_decode_end() != n || memcmp(back, raw, n))
		{
			fprintf(stderr, "%s: budgeted decode fails\n", argv[a]);
			return 1;
		}
		/* a short image does not decode */
		sv_decode_begin(img, m - 1, back, RAW_CAP);
		while ((r = sv_decode_step(~0u)) > 0)
			;
		if (r >= 0 || sv_decode_end())
		{
			fprintf(stderr, "%s: a short image decodes\n", argv[a]);
			return 1;
		}
		if (wdir)
		{
			char path[512];

			snprintf(path, sizeof(path), "%s/%s.img", wdir, name);
			if (!(f = fopen(path, "wb")) || fwrite(img, 1, m, f) != m || fclose(f))
			{
				fprintf(stderr, "%s: cannot write\n", path);
				return 1;
			}
		}
		printf("%-22s %7u %7u %6.2f %9llu %9llu %6u %6u\n", name, (unsigned)n, (unsigned)m,
			100.0 * m / n, (unsigned long long)be, (unsigned long long)bd, (unsigned)steps0,
			(unsigned)dsteps);
		traw += n;
		timg += m;
		tenc += be;
		tdec += bd;
	}
	printf("%-22s %7u %7u %6.2f %9llu %9llu\n", "total", (unsigned)traw, (unsigned)timg,
		traw ? 100.0 * timg / traw : 0.0, (unsigned long long)tenc,
		(unsigned long long)tdec);
	return 0;
}
