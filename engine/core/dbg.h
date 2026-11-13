/* SPDX-License-Identifier: MIT */
/*
 * dbg.h -- binary debug commands (docs/architecture.md 12).
 *
 * A fixed enum of commands with binary arguments, queued by the route VM,
 * the host CLI (text parsed on the host only) or a flashcart channel, and
 * executed at the frame boundary in queue order once their frame is due.
 * Commands whose module is not linked (entity, player and input commands
 * belong to game/ and phys/) are handled by whoever registers for them;
 * an unhandled command is logged and counted as a failure.
 */
#ifndef HL64_DBG_H
#define HL64_DBG_H

#include <stdint.h>

/* X(id, name, args, test_only); args: f = f32, u = u32 */
#define DBG_COMMANDS(X) \
	X(SETPOS, "setpos", "uffffff", 1)	/* map, pos[3], angles[3] */ \
	X(LOOK, "look", "ff", 0)		/* pitch, yaw */ \
	X(GOD, "god", "u", 1) \
	X(NOCLIP, "noclip", "u", 1) \
	X(NOTARGET, "notarget", "u", 1) \
	X(GIVE, "give", "u", 1)			/* class id */ \
	X(IMPULSE, "impulse", "u", 0) \
	X(USE_NAME, "use", "u", 0)		/* cooked name id */ \
	X(BUTTONS, "buttons", "uu", 0)		/* down, up masks */ \
	X(SET_SETTING, "set", "uf", 0)		/* field id, value */ \
	X(GET_SETTING, "get", "u", 0) \
	X(TIMESCALE, "timescale", "f", 0) \
	X(FIXED_DT, "fixed_dt", "u", 1)		/* us, 0 = VI clock */ \
	X(SHOT, "shot", "u", 0)			/* label id */ \
	X(STATS, "stats", "u", 0)		/* label id */ \
	X(DUMP_ENTITY, "dump", "u", 0)		/* entity index */ \
	X(MEM, "mem", "", 0) \
	X(PROF, "prof", "u", 0)			/* 1 s roll-up on/off */ \
	X(LOG_FLUSH, "flush", "", 0) \
	X(QUIT, "quit", "u", 0)			/* exit code */

enum {
#define DBG_ENUM(id, name, args, test) DBG_##id,
	DBG_COMMANDS(DBG_ENUM)
#undef DBG_ENUM
	DBG_COUNT
};

#define DBG_MAX_ARGS	7

typedef union {
	uint32_t	u;
	float		f;
} dbg_arg_t;

typedef struct {
	uint32_t	frame;		/* run at the first boundary >= frame */
	uint8_t		op;		/* DBG_* */
	uint8_t		nargs;
	dbg_arg_t	arg[DBG_MAX_ARGS];
} dbg_cmd_t;

typedef struct {
	const char	*name;
	const char	*args;
	uint8_t		test_only;
} dbg_desc_t;

extern const dbg_desc_t dbg_desc[DBG_COUNT];

/* Handler: 0 = done, nonzero = failed (logged). */
typedef int (*dbg_handler_t)(const dbg_cmd_t *c);

void	dbg_init(void);
void	dbg_handler_set(int op, dbg_handler_t fn);
/* Queue a command; -1 when the queue is full or the command is invalid. */
int	dbg_push(const dbg_cmd_t *c);
/* Frame boundary: run every due command. Returns failures so far. */
uint32_t dbg_run(uint32_t frame);

#endif
