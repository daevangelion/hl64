/* SPDX-License-Identifier: MIT */
/*
 * pak_host.c -- the host's controller 1 accessory for plat/pakfs.c (sys.h's
 * sys_pak_*): hl64-host's `--pak FILE` puts a Controller Pak there whose
 * memory is FILE, the Pak's raw 32 KB (the layout plat/pakfs.h gives, so
 * the file is also an ares Controller Pak: test/run/ares.sh ARES_PAK_IN).
 * A missing or empty FILE becomes a freshly formatted Pak. `--pak rumble`
 * puts a Rumble Pak there instead (a file named rumble: --pak ./rumble);
 * without --pak the controller has nothing in it.
 *
 * Every transfer goes straight to the file (pread / pwrite, no buffering),
 * so the file always holds what a real Pak would: whatever sys_pak_create /
 * write / close / delete left, as each call returns, and a run that dies
 * mid-save leaves the Pak a power cut would.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/host/host.h"
#include "plat/pakfs.h"

static char	pk_path[1024];
static int	pk_kind = SYS_PAK_ABSENT;
static int	pk_fd = -1;

void host_pak_set(const char *arg)
{
	if (!strcmp(arg, "rumble"))
	{
		pk_kind = SYS_PAK_RUMBLE;
		return;
	}
	snprintf(pk_path, sizeof(pk_path), "%s", arg);
	pk_kind = SYS_PAK_CPAK;
}

/* Open the image (formatting a missing / empty one); 0 ok. A file that
 * cannot be the Pak takes it out of the controller. */
static int pk_open(void)
{
	static uint8_t img[PAKFS_BYTES];
	off_t size;

	if (pk_fd >= 0)
		return 0;
	pk_fd = open(pk_path, O_RDWR | O_CREAT, 0644);
	if (pk_fd < 0)
	{
		log_printf("pak: cannot open %s; no Pak in the controller\n", pk_path);
		pk_kind = SYS_PAK_ABSENT;
		return -1;
	}
	size = lseek(pk_fd, 0, SEEK_END);
	if (size == 0)
	{
		pakfs_format(img);
		if (pwrite(pk_fd, img, sizeof(img), 0) != (ssize_t)sizeof(img))
			size = -1;
		else
		{
			log_printf("pak: host image %s (new, formatted)\n", pk_path);
			return 0;
		}
	}
	if (size != (off_t)PAKFS_BYTES)
	{
		log_printf("pak: %s is %lld bytes, not %u; no Pak in the controller\n", pk_path,
			(long long)size, PAKFS_BYTES);
		close(pk_fd);
		pk_fd = -1;
		pk_kind = SYS_PAK_ABSENT;
		return -1;
	}
	log_printf("pak: host image %s\n", pk_path);
	return 0;
}

int sys_pak_accessory(void)
{
	if (pk_kind == SYS_PAK_CPAK)
		pk_open();
	return pk_kind;
}

int pakfs_dev_read(uint32_t addr, void *dst, uint32_t len)
{
	if (pk_kind != SYS_PAK_CPAK || pk_open() || addr > PAKFS_BYTES || len > PAKFS_BYTES - addr)
		return -1;
	return pread(pk_fd, dst, len, addr) == (ssize_t)len ? 0 : -1;
}

/* HL64_PAK_FAIL_WRITE=N (tests, test/save/pakfail.sh): the Nth device
 * write of the run fails once, as a transfer error with the Pak still in. */
static int pk_fail_write(void)
{
	static long n = -1;

	if (n < 0)
	{
		const char *e = getenv("HL64_PAK_FAIL_WRITE");

		n = e ? strtol(e, NULL, 10) : 0;
	}
	if (n <= 0 || --n)
		return 0;
	log_printf("pak: injected write failure (HL64_PAK_FAIL_WRITE)\n");
	return 1;
}

int pakfs_dev_write(uint32_t addr, const void *src, uint32_t len)
{
	if (pk_kind != SYS_PAK_CPAK || pk_open() || addr > PAKFS_BYTES || len > PAKFS_BYTES - addr
		|| pk_fail_write())
		return -1;
	return pwrite(pk_fd, src, len, addr) == (ssize_t)len ? 0 : -1;
}
