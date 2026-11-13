/* SPDX-License-Identifier: MIT */
/*
 * io_host.c -- cart reads for the host build, imported from the old
 * tree's n64/port/host/q64_io_host.c (audit entry CLEAN; its Sys_Error
 * calls are now sys_error, its Quake string helpers snprintf, and it
 * serves sys.h's sys_cart_* / sys_pi_* instead of the old R64_Cart*).
 *
 * On the cart, plat/n64/cart.c maps a loose DFS file to its PI address
 * and DMAs from it; here each loose file under the cart root (the DFS
 * image source, HL64_CONTENT) gets a range in a virtual PI address space
 * and reads are pread()s from it. Same contract as the cart: 0 for
 * anything that is not a loose file, 0 on out-of-range or odd-aligned
 * transfers.
 *
 * Every transfer is charged the modelled PI cost (architecture 3.4: 195
 * us/KB = 5.25 MB/s; HL64_HOST_PI_US_PER_KB overrides), which engine/io
 * counts against the frame like the cart's DMA time.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "plat/host/host.h"

/* Cart files the host models (the cart's DFS has no such table; cooked
 * Uplink + Hazard content holds ~950 files with the studio cook). Files
 * are opened on demand, one at a time, so the host never holds more than
 * one descriptor whatever the count. */
#define IO_MAX_FILES	4096
#define IO_PATH_MAX	256
#define IO_ROM_BASE	0x10000000u	/* cart ROM domain, like the PI bus */
#define IO_PI_US_PER_KB	195u

typedef struct {
	char		path[IO_PATH_MAX];
	int		fd;
	uint32_t	rom;
	uint32_t	size;
} io_file_t;

static io_file_t	io_files[IO_MAX_FILES];
static int		io_nfiles;
static uint32_t		io_next_rom = IO_ROM_BASE;
static char		io_root[IO_PATH_MAX] = ".";
static uint32_t		io_us_per_kb = IO_PI_US_PER_KB;

static struct {
	uint64_t	calls;		/* transfers */
	uint64_t	bytes;
	uint64_t	frame_peak;	/* most bytes in one frame */
	uint64_t	pi_us;		/* modelled PI time */
	uint64_t	frame_bytes;
} io_st;

/* Regular files under dir, recursively, but *.ent: what the Makefile
 * packs into the ROM's DFS (find . -type f ! -name '*.ent'). */
static unsigned io_count_files(const char *dir)
{
	char path[1024];
	DIR *d = opendir(dir);
	struct dirent *e;
	struct stat st;
	unsigned n = 0;
	size_t len;

	if (!d)
		sys_error("io: cannot list %s", dir);
	while ((e = readdir(d)))
	{
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			sys_error("io: path too long under %s", dir);
		if (lstat(path, &st))
			sys_error("io: cannot stat %s", path);
		len = strlen(e->d_name);
		if (S_ISDIR(st.st_mode))
			n += io_count_files(path);
		else if (S_ISREG(st.st_mode) && !(len >= 4 && !strcmp(e->d_name + len - 4, ".ent")))
			n++;
	}
	closedir(d);
	return n;
}

/* the cart's DFS mount blocks, held for the whole run as on the cart
 * (volatile: GCC deletes a malloc whose block is never read) */
static void *volatile io_dfs_lookup, *volatile io_dfs_prefix;

void host_io_init_files(const char *root, unsigned extra_files)
{
	const char *e = getenv("HL64_HOST_PI_US_PER_KB");
	unsigned n;

	if (e)
		io_us_per_kb = (uint32_t)strtoul(e, NULL, 10);
	if (!root)
		return;
	snprintf(io_root, sizeof(io_root), "%s", root);
	/* libdragon dfs_init: malloc of the ROM's lookup table (mkdfs: 8 B +
	 * 16 B per file), then attach_filesystem's copy of "rom:/", which
	 * costs a 24 B chunk on the cart (limits.h HL64_CART_*, measured) */
	n = io_count_files(io_root) + extra_files;
	io_dfs_lookup = malloc(8u + 16u * n);
	io_dfs_prefix = malloc(20u);
	if (!io_dfs_lookup || !io_dfs_prefix)
		sys_error("io: DFS mount of %u files does not fit the heap", n);
	log_printf("io: DFS %u files\n", n);
}

void host_io_init(const char *root)
{
	host_io_init_files(root, 0);
}

/* "rom:/x" and DFS-relative "x" are cart-root relative. 0 if too long. */
static int io_path(char *out, size_t outsz, const char *path)
{
	int n;

	if (!strncmp(path, "rom:/", 5))
		n = snprintf(out, outsz, "%s/%s", io_root, path + 5);
	else if (path[0] == '/' || !strncmp(path, io_root, strlen(io_root)))
		n = snprintf(out, outsz, "%s", path);
	else
		n = snprintf(out, outsz, "%s/%s", io_root, path);
	return n >= 0 && (size_t)n < outsz;
}

uint32_t sys_cart_addr(const char *path, uint32_t *out_size)
{
	char full[IO_PATH_MAX];
	struct stat st;
	io_file_t *f;
	int i, fd;

	if (out_size)
		*out_size = 0;
	if (!path || !path[0])
		return 0;
	if (!io_path(full, sizeof(full), path))
		return 0;
	for (i = 0; i < io_nfiles; i++)
		if (!strcmp(io_files[i].path, full))
		{
			if (out_size)
				*out_size = io_files[i].size;
			return io_files[i].rom;
		}
	fd = open(full, O_RDONLY);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
	{
		close(fd);
		return 0;
	}
	close(fd);
	if (io_nfiles >= IO_MAX_FILES)
		sys_error("io_host: more than %d cart files", IO_MAX_FILES);
	f = &io_files[io_nfiles++];
	snprintf(f->path, sizeof(f->path), "%s", full);
	f->fd = -1;
	f->size = (uint32_t)st.st_size;
	f->rom = io_next_rom;
	/* next file on a 2-byte boundary, like DFS's halfword-aligned files */
	io_next_rom += (f->size + 1u) & ~1u;
	if (out_size)
		*out_size = f->size;
	return f->rom;
}

/* The file holding PI address rom, or sys_error. */
static io_file_t *io_file_at(uint32_t rom)
{
	int i;

	for (i = 0; i < io_nfiles; i++)
		if (rom >= io_files[i].rom && rom < io_files[i].rom + io_files[i].size)
			return &io_files[i];
	sys_error("io_host: unknown rom address 0x%08lx", (unsigned long)rom);
}

/* The file's descriptor: the last file read keeps its descriptor open. */
static int io_fd(io_file_t *f)
{
	static io_file_t *open_f;

	if (open_f == f)
		return f->fd;
	if (open_f)
	{
		close(open_f->fd);
		open_f->fd = -1;
		open_f = NULL;
	}
	f->fd = open(f->path, O_RDONLY);
	if (f->fd < 0)
		sys_error("io_host: cannot reopen %s", f->path);
	open_f = f;
	return f->fd;
}

static void io_count(uint32_t n)
{
	io_st.calls++;
	io_st.bytes += n;
	io_st.pi_us += sys_pi_cost_us(n);
	io_st.frame_bytes += n;
}

int sys_cart_read(void *dst, size_t dst_cap, uint32_t rom, uint32_t rom_size,
	uint32_t off, uint32_t len)
{
	io_file_t *f;
	uint32_t dma_len, src;

	if (!dst || !rom || len == 0)
		return 0;
	if ((uint64_t)off + (uint64_t)len > (uint64_t)rom_size)
		return 0;
	dma_len = (len + 1u) & ~1u;
	if ((size_t)dma_len > dst_cap)
		return 0;
	src = rom + off;
	if (((uintptr_t)dst | (uintptr_t)src | (uintptr_t)dma_len) & 1u)
		return 0;
	f = io_file_at(rom);
	/* the PI transfers the rounded halfword past a file's odd tail too */
	if (off + dma_len > f->size)
		dma_len = len;
	if (pread(io_fd(f), dst, dma_len, (off_t)off) != (ssize_t)dma_len)
		sys_error("io_host: short read %s @%lu", f->path, (unsigned long)off);
	io_count(dma_len);
	return 1;
}

/* ---- engine/io backend: a piece lands when launched ----------------------- */

static uint32_t io_pread(void *dst, uint32_t rom, uint32_t len)
{
	io_file_t *f = io_file_at(rom);
	uint32_t off = rom - f->rom, n = len;

	if (off + n > f->size)
		n = f->size - off;
	if (pread(io_fd(f), dst, n, (off_t)off) != (ssize_t)n)
		sys_error("io_host: short read %s @%lu", f->path, (unsigned long)off);
	io_count(n);
	return n;
}

/* The host has no PI interrupt: engine/io runs its chain step at every io
 * call, and a launched piece has landed (sys_pi_busy 0), so a frame's
 * plan moves at once and a sync read never waits for a piece in flight.
 * The pieces' modelled PI time is still summed per frame (io.c Account:
 * frames_pi_over). */
void sys_pi_launch(void *dst, uint32_t rom, uint32_t len)
{
	io_pread(dst, rom, len);
}

int sys_pi_busy(void)
{
	return 0;
}

uint32_t sys_pi_wait(int audio)
{
	(void)audio;
	return 0;
}

void sys_pi_set_isr(void (*isr)(void))
{
	(void)isr;
}

void sys_irq_off(void)
{
}

void sys_irq_on(void)
{
}

uint32_t sys_pi_read(void *dst, uint32_t rom, uint32_t len)
{
	return sys_pi_cost_us(io_pread(dst, rom, len));
}

uint32_t sys_pi_cost_us(uint32_t len)
{
	return (uint32_t)(((uint64_t)len * io_us_per_kb + 512u) / 1024u);
}

/* ---- PROFILE=1 counters -------------------------------------------------- */

void sys_prof_frame(void)
{
	if (io_st.frame_bytes > io_st.frame_peak)
		io_st.frame_peak = io_st.frame_bytes;
	io_st.frame_bytes = 0;
}

void sys_prof_dump(unsigned nframes)
{
	unsigned nf = nframes ? nframes : 1;

	log_printf("io host calls=%lu/f %luB/f pi=%luus/f frame_peak=%luB\n",
		(unsigned long)(io_st.calls / nf), (unsigned long)(io_st.bytes / nf),
		(unsigned long)(io_st.pi_us / nf), (unsigned long)io_st.frame_peak);
	memset(&io_st, 0, sizeof(io_st));
}
