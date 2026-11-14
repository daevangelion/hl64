/* SPDX-License-Identifier: MIT */
/*
 * co.h -- cooperative contexts for game work spread over frames (monster
 * thinks that pause when the frame's AI work budget is spent:
 * game/hl/aithink.c). HL64_AI_CONTEXTS contexts, each with its own stack
 * and game scratch in the boot carve's aithink region (limits.h).
 * Switching is cooperative and explicit: the same work units pause the
 * same code at the same point on the host and the cart.
 */
#ifndef HL64_CO_H
#define HL64_CO_H

#include <stdint.h>

/* The context's game scratch (HL64_AI_SCRATCH_BYTES, 16-aligned). */
void		*co_scratch(int i);
/* (Re)arm context i: its next co_resume starts entry() on a fresh stack.
 * entry must never return: it ends with co_yield() forever. */
void		co_start(int i, void (*entry)(void));
/* Main stack only: run context i until it yields. A stack overflow into
 * the context's guard words is fatal (sys_error). */
void		co_resume(int i);
/* The running context only: back to its co_resume. */
void		co_yield(void);
/* The running context, -1 on the main stack. */
int		co_running(void);
/* Deepest stack use of context i since boot, bytes (painted stacks). */
uint32_t	co_stack_peak(int i);

#endif
