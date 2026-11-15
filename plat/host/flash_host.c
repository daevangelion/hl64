/* SPDX-License-Identifier: MIT */
/*
 * flash_host.c -- the host's FlashRAM: sys.h's sys_flash_* over a 128 KB
 * image file (the cart's chip: 8 sectors of 16 KB, 128-byte pages).
 *
 * The image is hl64-host's `--save FILE`; without it, hl64.fla in the
 * directory of the hl64-host binary (the build directory's host/), so a
 * second run of the same binary sees what the first saved. A missing or
 * empty file is created filled with 0xFF (an erased chip). Tests that
 * need a clean chip pass a fresh --save file.
 *
 * Erase writes 0xFF over the sector; program ANDs the page into the array,
 * like NOR flash: programming a page that was not erased shows. Both take
 * effect when started; sys_flash_poll then reports the operation's end.
 * The cart's chip is busy for a long time (an erase hundreds of ms, a page
 * ~ms), so engine/save/flq.c's multi-frame path only runs there: to run it
 * here, HL64_HOST_FLASH_BUSY=E,P makes the chip report busy for the first
 * E polls of an erase and P polls of a program (counted, not timed, so a
 * run stays deterministic). Default 0,0: every poll reports the end.
 *
 * Power-cut tests (test/save/flashcut.sh): HL64_HOST_FLASH_CUT=N applies
 * the Nth erase or program since boot only halfway (the first half of the
 * sector or page) and exits at once with status 3, as a power cut in the
 * middle of that operation.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/host/host.h"

#define FL_PAGE		128u
#define FL_SECTOR	(128u * FL_PAGE)
#define FL_SECTORS	8u
#define FL_BYTES	(FL_SECTORS * FL_SECTOR)

static char	fl_path[1024];
static int	fl_fd = -1;
static int	fl_op;			/* operation in flight (its polls left in fl_busy) */
static unsigned	fl_busy;
static unsigned	fl_busy_erase, fl_busy_program;

void host_flash_set_path(const char *path)
{
	snprintf(fl_path, sizeof(fl_path), "%s", path);
}

/* The default image: hl64.fla beside the running binary. */
static int fl_default_path(void)
{
	char exe[1000];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char *slash;

	if (n <= 0)
		return -1;
	exe[n] = 0;
	slash = strrchr(exe, '/');
	if (!slash)
		return -1;
	slash[1] = 0;
	return snprintf(fl_path, sizeof(fl_path), "%shl64.fla", exe) >= (int)sizeof(fl_path) ? -1 : 0;
}

int sys_flash_init(void)
{
	static uint8_t blank[FL_SECTOR];
	const char *busy = getenv("HL64_HOST_FLASH_BUSY");
	off_t size;
	unsigned s;

	if (fl_fd >= 0)
		return 1;
	if (!fl_path[0] && fl_default_path())
	{
		log_printf("flash: no image path (hl64-host --save FILE)\n");
		return 0;
	}
	if (busy && sscanf(busy, "%u,%u", &fl_busy_erase, &fl_busy_program) != 2)
		sys_error("flash: HL64_HOST_FLASH_BUSY wants E,P (polls), got \"%s\"", busy);
	fl_fd = open(fl_path, O_RDWR | O_CREAT, 0644);
	if (fl_fd < 0)
	{
		log_printf("flash: cannot open %s\n", fl_path);
		return 0;
	}
	size = lseek(fl_fd, 0, SEEK_END);
	if (size == 0)
	{
		memset(blank, 0xFF, sizeof(blank));
		for (s = 0; s < FL_SECTORS; s++)
			if (pwrite(fl_fd, blank, sizeof(blank), (off_t)s * FL_SECTOR) != (ssize_t)sizeof(blank))
			{
				log_printf("flash: cannot write %s\n", fl_path);
				close(fl_fd);
				fl_fd = -1;
				return 0;
			}
		log_printf("flash: host image %s (new, %u KB)\n", fl_path, FL_BYTES / 1024);
	}
	else if (size != (off_t)FL_BYTES)
	{
		log_printf("flash: %s is %lld bytes, not %u\n", fl_path, (long long)size, FL_BYTES);
		close(fl_fd);
		fl_fd = -1;
		return 0;
	}
	else
		log_printf("flash: host image %s (%u KB)\n", fl_path, FL_BYTES / 1024);
	return 1;
}

int sys_flash_read(void *dst, uint32_t off, uint32_t len)
{
	if (fl_fd < 0 || fl_op)
		return -1;
	if ((off | len) & 1u || off > FL_BYTES || len > FL_BYTES - off || ((uintptr_t)dst & 15u))
		return -1;
	return pread(fl_fd, dst, len, off) == (ssize_t)len ? 0 : -1;
}

static unsigned	fl_ops;

/* Operation HL64_HOST_FLASH_CUT: half of it lands (n bytes at off), then
 * the power goes. */
static void power_cut(const void *src, uint32_t n, uint32_t off)
{
	static long cut = -1;

	if (cut < 0)
	{
		const char *c = getenv("HL64_HOST_FLASH_CUT");

		cut = c ? strtol(c, NULL, 10) : 0;
	}
	if (cut <= 0 || ++fl_ops < (unsigned long)cut)
		return;
	if (pwrite(fl_fd, src, n / 2, (off_t)off) != (ssize_t)(n / 2))
		log_printf("flash: power cut: half write failed\n");
	log_printf("flash: power cut in operation %u\n", fl_ops);
	log_flush();
	_exit(3);
}

int sys_flash_erase(uint32_t sector)
{
	static uint8_t blank[FL_SECTOR];

	if (fl_fd < 0 || fl_op || sector >= FL_SECTORS)
		return -1;
	memset(blank, 0xFF, sizeof(blank));
	power_cut(blank, sizeof(blank), sector * FL_SECTOR);
	if (pwrite(fl_fd, blank, sizeof(blank), (off_t)sector * FL_SECTOR) != (ssize_t)sizeof(blank))
		return -1;
	fl_op = 1;
	fl_busy = fl_busy_erase;
	return 0;
}

int sys_flash_program(uint32_t page, const void *src)
{
	uint8_t cur[FL_PAGE];
	unsigned i;

	if (fl_fd < 0 || fl_op || page >= FL_BYTES / FL_PAGE || ((uintptr_t)src & 15u))
		return -1;
	if (pread(fl_fd, cur, sizeof(cur), (off_t)page * FL_PAGE) != (ssize_t)sizeof(cur))
		return -1;
	for (i = 0; i < FL_PAGE; i++)
		cur[i] &= ((const uint8_t *)src)[i];
	power_cut(cur, sizeof(cur), page * FL_PAGE);
	if (pwrite(fl_fd, cur, sizeof(cur), (off_t)page * FL_PAGE) != (ssize_t)sizeof(cur))
		return -1;
	fl_op = 1;
	fl_busy = fl_busy_program;
	return 0;
}

int sys_flash_poll(void)
{
	if (!fl_op)
		return 0;
	if (fl_busy)
	{
		fl_busy--;
		return 1;
	}
	fl_op = 0;
	return 0;
}
