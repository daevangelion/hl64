/* SPDX-License-Identifier: MIT */
/*
 * io.h -- cart read scheduler (docs/architecture.md 5.2), imported from
 * the old tree's q64_io.h (audit entry source/q64_io.h CLEAN).
 *
 * Every streaming cart read goes through one queue of caller-owned
 * requests. The scheduler plans pieces from the queue (highest priority
 * first, never more than the frame's byte budget: 64 KB; 128 KB while a
 * seam pin is outstanding) into a plan array of PI DMAs of at most
 * IO_PIECE bytes, and the PI completion interrupt chains them: the
 * interrupt only launches the plan's next piece and marks the landed one
 * (no queue or list work, no callbacks, no allocation); the main thread
 * touches the plan and the in-flight state with interrupts off. The host
 * build has no interrupt: every io call runs the same chain step, and a
 * host piece lands when it is launched. io_pump() (the frame loop, after
 * several frame steps) publishes landed pieces (playable builds: and
 * plans more) and resumes a chain paused by a sync / audio read.
 *
 * Synchronous reads (layer open, a collision chunk missing in the middle
 * of a trace) bypass the budget and are counted as blocked I/O, except
 * during boot (until the first io_frame_begin) and inside io_set_load
 * (boot / New Game / setpos map loads), which count as load I/O. A frame
 * blocked more than 1 ms logs `io: frame N blocked U us: S sync reads B B,
 * largest L B at R, A audio reads, drain D us`; inside a test teleport
 * window (io_tele_window) it counts as blk1ms_tele. Drain is the wait for
 * the chain's piece in flight when a sync / audio read starts: the read
 * pauses the chain until the next io_pump, so the reads of one frame step
 * wait for one IO_PIECE (0.4 ms) at most.
 * Test builds also log each sync read in play: `io: sync <what> N B at R`.
 *
 * Shared by the cart and the host build: the backend (sys.h sys_pi_*) is
 * a PI DMA (cart) or a pread with a modelled PI cost per KB (host), so
 * the host reports the same per-frame traffic, budget and sync-read time.
 *
 * HL64_TEST builds (host and cart, K10, architecture.md 15) complete I/O
 * in frames: io_frame_begin plans the frame's pieces from the queue alone
 * (priority order, budget, at most IO_PLAN_UNITS requests and 16 KB
 * units), the chain moves them during the frame, and their completions
 * (done, state, callbacks) publish only at the next io_frame_begin,
 * before anything else of that frame. A planned request reads IO_ACTIVE
 * all frame. A plan the frame did not move is finished there
 * synchronously: late (`io: frame N late: ...`, stats `late=`, blocked
 * I/O). io_finish, io_cancel and sync / audio reads act at once.
 * Playable builds publish a request as soon as its last piece lands (at
 * the next io call) and plan again whenever the plan has landed; their
 * chain runs across frame boundaries.
 */
#ifndef HL64_IO_H
#define HL64_IO_H

#include <stdint.h>

#define IO_PIECE		(2 * 1024)	/* one PI DMA ≈ 0.4 ms at 5.25 MB/s: the
					   longest wait of a frame step's sync /
					   audio reads */
#define IO_BUDGET		(64 * 1024)	/* bytes per frame, normal */
#define IO_BUDGET_SEAM	(128 * 1024)	/* while a seam pin is outstanding */
#define IO_PLAN_UNIT	(16 * 1024)
#define IO_PLAN_UNITS	8		/* a plan: at most 8 requests and 8 units
					   (each request's bytes rounded up to
					   units): the seam budget's */
#define IO_KB_BINS		66		/* per-frame async KB histogram: 0..64, more */

/* queue priorities, strongest first; NEAR: engine/world's near class
 * (thinking monsters' collision boxes) */
enum { IO_PRIO_OPEN, IO_PRIO_SEAM, IO_PRIO_NEAR, IO_PRIO_PREFETCH, IO_NPRIO };
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
	/* queued ahead of its class's other requests (after earlier front
	 * ones): PVS rows, whose absence makes the game's PVS test read
	 * synchronously. Not COLL chunks: after a teleport their burst kept
	 * the visible set's render chunks queued past the teleport window
	 * (test/m4/stream_open.route: popin 3, 0 without) */
	uint8_t			front;
	void			(*cb)(struct io_req_s *r);	/* on completion, main thread:
							   the io call that publishes it
							   (test builds io_frame_begin),
							   io_finish */
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
	uint32_t	promoted;		/* io_promote moves */
	/* since boot (io_stats never resets these): the stats record's
	 * blkmax / over / blk1ms, comparable host = cart whatever the
	 * roll-up resets */
	uint32_t	life_frames;
	uint32_t	life_blocked_max_us;
	uint32_t	life_frames_blocked_1ms;
	uint32_t	life_frames_budget_over;
	/* frames > 1 ms blocked inside io_tele_window (blk1ms_tele): not in
	 * life_frames_blocked_1ms / life_blocked_max_us, counted apart */
	uint32_t	life_frames_blocked_1ms_tele;
	/* HL64_TEST: frames whose plan was finished late at the boundary,
	 * and its pieces (0 on the gate routes) */
	uint32_t	life_late_frames;
	uint32_t	life_late_pieces;
} io_stats_t;

void	io_init(void);
/* New frame: frame_us = this frame's simulated duration (host PI model). */
void	io_frame_begin(uint32_t frame_us);
void	io_set_seam(int on);
void	io_submit(io_req_t *r);
/* A stronger class needs a queued (or in-flight) request: move it to the
 * tail of class prio's queue (architecture 5.2 promotion); no-op when r
 * is idle / done or already at least that strong. */
void	io_promote(io_req_t *r, int prio);
/* Remove a request; an active one is waited for (counted as blocked). */
void	io_cancel(io_req_t *r);
/* Block until r is DONE (counted as blocked); r must be queued or active;
 * `what` as io_read_sync's. */
void	io_finish(io_req_t *r, const char *what);
/* Publish landed pieces (playable: plan more once the plan has landed);
 * resume a paused chain. */
void	io_pump(void);
/* While held (nests), the chain launches no new piece: cart file lookups
 * and reads outside the queue (plat/n64/cart.c), which would otherwise
 * wait for a piece per DMA. */
void	io_hold(int on);
/* Blocking read outside the queue (waits for the piece in flight first);
 * `what` names the caller in the test builds' in-play `io: sync` line. */
void	io_read_sync(void *dst, uint32_t rom, uint32_t len, const char *what);
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
/* Map load (boot / New Game / setpos): sync reads count as load I/O, not
 * as blocked frame time. Nests: io_set_load(1) ... io_set_load(0). */
void	io_set_load(int on);
/* A test-only teleport (route SETPOS, HL64_TEST): this frame and the next
 * `frames` - 1 (the destination hydrates) are a teleport window. Their sync
 * reads are still blocked frame time; a frame > 1 ms in the window counts
 * as life_frames_blocked_1ms_tele (stats `blk1ms_tele=`) instead of
 * blk1ms / blkmax, and its `io: frame` line says "(teleport window)". */
void	io_tele_window(uint32_t frames);
/* 1 inside io_set_load / boot (sync reads are load I/O). */
int	io_loading(void);
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
/* Blocked us of the frame so far. */
uint32_t io_frame_blocked_us(void);

#endif
