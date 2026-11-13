/* SPDX-License-Identifier: MIT */
/*
 * io.h -- cart read scheduler (docs/architecture.md 5.2), imported from
 * the old tree's q64_io.h (audit entry source/q64_io.h CLEAN).
 *
 * Every streaming cart read goes through one queue of caller-owned
 * requests. io_pump(), called a few times per frame from the frame loop
 * (never from an interrupt), retires the PI piece in flight and launches
 * the next one: at most IO_PIECE bytes per PI DMA, highest priority first,
 * and never more than the frame's byte budget (64 KB; 128 KB while a seam
 * pin is outstanding).
 *
 * Synchronous reads (layer open, a collision chunk missing in the middle
 * of a trace, PVS rows) bypass the budget and are counted as blocked I/O.
 *
 * Shared by the cart and the host build: the backend (sys.h sys_pi_*) is
 * a PI DMA (cart) or a pread with a modelled PI cost per KB (host), so
 * the host reports the same per-frame traffic, budget and blocked time.
 */
#ifndef HL64_IO_H
#define HL64_IO_H

#include <stdint.h>

#define IO_PIECE		(16 * 1024)	/* one PI DMA ≈ 3.1 ms at 5.25 MB/s */
#define IO_BUDGET		(64 * 1024)	/* bytes per frame, normal */
#define IO_BUDGET_SEAM	(128 * 1024)	/* while a seam pin is outstanding */
#define IO_KB_BINS		66		/* per-frame async KB histogram: 0..64, more */

enum { IO_PRIO_OPEN, IO_PRIO_SEAM, IO_PRIO_PREFETCH, IO_NPRIO };
enum { IO_IDLE, IO_QUEUED, IO_ACTIVE, IO_DONE };

typedef struct io_req_s
{
	struct io_req_s	*next;
	uint32_t		rom;	/* PI address of the first byte (even) */
	uint8_t			*dst;	/* 16-aligned */
	uint32_t		len;	/* bytes (even) */
	uint32_t		done;	/* bytes landed */
	uint8_t			prio;
	volatile uint8_t	state;
	void			(*cb)(struct io_req_s *r);	/* on completion, from io_pump */
	int			user;
} io_req_t;

typedef struct
{
	uint32_t	frames;
	uint64_t	async_bytes;		/* scheduled pieces */
	uint32_t	async_pieces;
	uint32_t	frame_async_max;	/* most async bytes in one frame */
	uint32_t	sync_calls;
	uint64_t	sync_bytes;
	uint64_t	blocked_us;		/* sync reads + waits (host: modelled) */
	uint32_t	frame_blocked_max_us;
	uint32_t	frames_blocked_1ms;	/* frames with > 1000 us blocked */
	uint32_t	frames_budget_over;	/* async bytes > budget (must stay 0) */
	uint32_t	frames_pi_over;		/* host: modelled PI time > frame time */
	uint32_t	queue_max;		/* deepest queue seen */
	uint64_t	load_bytes;		/* sync reads during map load */
	uint64_t	load_us;
	uint32_t	hist[6];		/* frames by async bytes: 0, <=8K, <=16K, <=32K, <=64K, more */
	uint32_t	kb_hist[IO_KB_BINS];	/* frames by async KB (rounded up), last bin = more */
	uint32_t	audio_calls;		/* AUDIO class reads (mixer wave data) */
	uint64_t	audio_bytes;
	uint64_t	audio_us;		/* their PI time (host: modelled) */
} io_stats_t;

void	io_init(void);
/* New frame: frame_us = this frame's simulated duration (host PI model). */
void	io_frame_begin(uint32_t frame_us);
void	io_set_seam(int on);
void	io_submit(io_req_t *r);
/* Remove a request; an active one is waited for (counted as blocked). */
void	io_cancel(io_req_t *r);
/* Block until r is DONE (counted as blocked); r must be queued or active. */
void	io_finish(io_req_t *r);
void	io_pump(void);
/* Blocking read outside the queue (waits for the piece in flight first). */
void	io_read_sync(void *dst, uint32_t rom, uint32_t len);
/*
 * AUDIO class (architecture 5.2: AUDIO > OPEN > SEAM > PREFETCH): the
 * mixer's wave reads, synchronous and ahead of every queued request (they
 * wait only for the piece in flight). Counted apart from blocked I/O and
 * the async budget (snd's own CPU time includes them). dst 16-aligned,
 * rom and len even.
 */
void	io_read_audio(void *dst, uint32_t rom, uint32_t len);
int	io_busy(void);
/* A piece is still in flight (a finished one is retired first): a sync read
 * now would wait for it. */
int	io_in_flight(void);
/* Map load: sync reads count as load I/O, not as blocked frame time. */
void	io_set_load(int on);
/* Bytes queued or in flight. */
uint32_t io_queued(void);
/* Bytes still allowed this frame; the frame's whole budget. */
uint32_t io_budget_left(void);
uint32_t io_budget(void);
void	io_stats(io_stats_t *out, int reset);
/* Percentile (0..100) of per-frame async bytes from the stats' KB histogram, in KB. */
uint32_t io_frame_kb_percentile(const io_stats_t *s, int pct);
/* Last completed frame (route stats line). */
void	io_last_frame(uint32_t *async_bytes, uint32_t *blocked_us, uint32_t *queued);

#endif
