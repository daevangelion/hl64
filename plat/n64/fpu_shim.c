/* SPDX-License-Identifier: MIT */
/*
 * fpu_shim.c -- VR4300 FPU trap emulation, imported from the old tree's
 * n64/port/fpu_shim.c (audit entry CLEAN; our code, relicensed MIT).
 *
 * The VR4300 raises an unmaskable "unimplemented operation" exception for
 * float->int conversions that overflow, and for operations on NaN or
 * denormal operands. Desktop IEEE hardware produces saturated / NaN
 * results silently instead, and float code written against that (HLSDK
 * game logic, pm_shared) expects it. The shim catches the trap, emulates
 * the instruction with IEEE semantics, and resumes.
 */
#include <libdragon.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

static exception_handler_t chain_handler;

#define OPC(i) ((i) >> 26)
#define FMT(i) (((i) >> 21) & 0x1f)
#define RS(i) (((i) >> 21) & 0x1f)
#define RT(i) (((i) >> 16) & 0x1f)
#define FT(i) (((i) >> 16) & 0x1f)
#define FS(i) (((i) >> 11) & 0x1f)
#define FD(i) (((i) >> 6) & 0x1f)
#define FUNC(i) ((i) & 0x3f)
#define SIMM(i) ((int16_t)((i) & 0xffff))

#define FMT_S 16
#define FMT_D 17
#define FMT_W 20
#define FMT_BC 8

#define FCR31_CONDITION (1u << 23)

static double read_fpr(reg_block_t *r, int reg, int fmt)
{
	if (fmt == FMT_D)
	{
		double d;
		memcpy(&d, &r->fpr[reg], 8);
		return d;
	}
	else
	{
		float f;
		uint32_t v = (uint32_t)r->fpr[reg];
		memcpy(&f, &v, 4);
		return (double)f;
	}
}

static void write_fpr_s(reg_block_t *r, int reg, float f)
{
	uint32_t v;
	memcpy(&v, &f, 4);
	r->fpr[reg] = (r->fpr[reg] & 0xffffffff00000000ull) | v;
}

static void write_fpr_d(reg_block_t *r, int reg, double d)
{
	memcpy(&r->fpr[reg], &d, 8);
}

static void write_fpr_w(reg_block_t *r, int reg, int32_t w)
{
	r->fpr[reg] = (r->fpr[reg] & 0xffffffff00000000ull) | (uint32_t)w;
}

// The hardware would trap on denormal operands before doing any math;
// mirror the FS-bit behavior and flush them to zero instead.
static double sanitize(double v, int fmt)
{
	double lim = (fmt == FMT_D) ? 2.2250738585072014e-308 : 1.17549435e-38;

	if (v != 0.0 && !isnan(v) && fabs(v) < lim)
		return 0.0;
	return v;
}

static int32_t to_int32(double v, int rounding)
{
	if (isnan(v))
		return 0;
	if (v >= 2147483648.0)
		return INT32_MAX;
	if (v <= -2147483649.0)
		return INT32_MIN;
	switch (rounding)
	{
	case 0x0c: return (int32_t)lround(v);  // round.w
	case 0x0e: return (int32_t)ceil(v);    // ceil.w
	case 0x0f: return (int32_t)floor(v);   // floor.w
	default:   return (int32_t)trunc(v);   // trunc.w / cvt.w (RN≈trunc for our uses)
	}
}

// Emulate the COP1 arithmetic instruction `insn`. Returns 0 if the
// instruction is not one we understand.
static int emulate_fpu_op(reg_block_t *r, uint32_t insn)
{
	int fmt = FMT(insn);
	int func = FUNC(insn);
	double a, b, res;

	if (OPC(insn) != 0x11) // COP1
		return 0;
	if (fmt != FMT_S && fmt != FMT_D && fmt != FMT_W)
		return 0;

	// cvt.s.w / cvt.d.w never trap and W has no other arith; don't touch
	if (fmt == FMT_W)
	{
		int32_t w = (int32_t)(uint32_t)r->fpr[FS(insn)];
		if (func == 0x20) { write_fpr_s(r, FD(insn), (float)w); return 1; }
		if (func == 0x21) { write_fpr_d(r, FD(insn), (double)w); return 1; }
		return 0;
	}

	a = sanitize(read_fpr(r, FS(insn), fmt), fmt);

	switch (func)
	{
	case 0x00: case 0x01: case 0x02: case 0x03: // add/sub/mul/div
		b = sanitize(read_fpr(r, FT(insn), fmt), fmt);
		switch (func)
		{
		case 0x00: res = a + b; break;
		case 0x01: res = a - b; break;
		case 0x02: res = a * b; break;
		default:   res = a / b; break;
		}
		break;
	case 0x04: res = sqrt(a); break;
	case 0x05: res = fabs(a); break;
	case 0x06: res = a; break; // mov
	case 0x07: res = -a; break;

	case 0x0c: case 0x0d: case 0x0e: case 0x0f: // round/trunc/ceil/floor.w
		write_fpr_w(r, FD(insn), to_int32(a, func));
		return 1;

	case 0x20: // cvt.s (from d)
		if (fmt != FMT_D) return 0;
		write_fpr_s(r, FD(insn), (float)a);
		return 1;
	case 0x21: // cvt.d (from s)
		if (fmt != FMT_S) return 0;
		write_fpr_d(r, FD(insn), a);
		return 1;
	case 0x24: // cvt.w
		write_fpr_w(r, FD(insn), to_int32(a, func));
		return 1;

	default:
		if (func >= 0x30) // c.cond compares
		{
			int less, equal, unordered;
			int cond = func & 0xf;
			int result;

			b = sanitize(read_fpr(r, FT(insn), fmt), fmt);
			if (isnan(a) || isnan(b))
			{
				less = 0; equal = 0; unordered = 1;
			}
			else
			{
				less = a < b; equal = a == b; unordered = 0;
			}
			result = ((cond & 4) && less) || ((cond & 2) && equal)
				|| ((cond & 1) && unordered);
			if (result)
				r->fc31 |= FCR31_CONDITION;
			else
				r->fc31 &= ~FCR31_CONDITION;
			return 1;
		}
		return 0;
	}

	// flush denormal results like the FS bit would
	res = sanitize(res, fmt);
	if (fmt == FMT_D)
		write_fpr_d(r, FD(insn), res);
	else
		write_fpr_s(r, FD(insn), (float)res);
	return 1;
}

// The trapping instruction sat in a branch delay slot: emulate the branch
// at `epc` to find the continuation address. Returns 0 for branches we
// don't understand. The FPU condition bit is taken from the pre-trap fc31
// (a compare in the delay slot cannot affect its own branch).
static int emulate_branch(reg_block_t *r, uint32_t cond_before, uint32_t *next)
{
	uint32_t epc = r->epc;
	uint32_t insn = *(uint32_t *)(uintptr_t)epc;
	uint32_t not_taken = epc + 8;
	uint32_t target = epc + 4 + ((int32_t)SIMM(insn) << 2);
	int64_t rs = (int64_t)r->gpr[RS(insn)];
	int64_t rt = (int64_t)r->gpr[RT(insn)];
	int taken;

	switch (OPC(insn))
	{
	case 0x02: // j
	case 0x03: // jal
		if (OPC(insn) == 0x03)
			r->gpr[31] = not_taken;
		*next = ((epc + 4) & 0xf0000000u) | ((insn & 0x03ffffffu) << 2);
		return 1;
	case 0x00: // special: jr/jalr
		if (FUNC(insn) == 0x08)
		{
			*next = (uint32_t)r->gpr[RS(insn)];
			return 1;
		}
		if (FUNC(insn) == 0x09)
		{
			r->gpr[(insn >> 11) & 0x1f] = not_taken;
			*next = (uint32_t)r->gpr[RS(insn)];
			return 1;
		}
		return 0;
	case 0x04: case 0x14: taken = rs == rt; break;  // beq(l)
	case 0x05: case 0x15: taken = rs != rt; break;  // bne(l)
	case 0x06: case 0x16: taken = rs <= 0; break;   // blez(l)
	case 0x07: case 0x17: taken = rs > 0; break;    // bgtz(l)
	case 0x01: // regimm
		switch (RT(insn))
		{
		case 0x00: case 0x02: taken = rs < 0; break;  // bltz(l)
		case 0x01: case 0x03: taken = rs >= 0; break; // bgez(l)
		case 0x10: case 0x12: r->gpr[31] = not_taken; taken = rs < 0; break;
		case 0x11: case 0x13: r->gpr[31] = not_taken; taken = rs >= 0; break;
		default: return 0;
		}
		break;
	case 0x11: // cop1 bc1f/bc1t(l)
		if (FMT(insn) != FMT_BC)
			return 0;
		taken = (RT(insn) & 1) ? (cond_before & FCR31_CONDITION) != 0
		                       : (cond_before & FCR31_CONDITION) == 0;
		break;
	default:
		return 0;
	}

	*next = taken ? target : not_taken;
	return 1;
}

static unsigned long shim_traps;

unsigned long fpu_shim_trap_count(void)
{
	return shim_traps;
}

static void fpu_shim_handler(exception_t *exc)
{
	reg_block_t *r = exc->regs;
	int bd = (r->cr & 0x80000000u) != 0;
	uint32_t cond_before = r->fc31;
	uint32_t insn;

	if (exc->code != EXCEPTION_CODE_FLOATING_POINT
		|| !(r->fc31 & C1_CAUSE_NOT_IMPLEMENTED))
		goto not_ours;

	shim_traps++;

	insn = *(uint32_t *)(uintptr_t)(r->epc + (bd ? 4 : 0));
	if (!emulate_fpu_op(r, insn))
		goto not_ours;

	// clear the sticky cause bits we just handled
	r->fc31 &= ~(C1_CAUSE_NOT_IMPLEMENTED | C1_CAUSE_INVALID_OP
		| C1_CAUSE_OVERFLOW | C1_CAUSE_UNDERFLOW | C1_CAUSE_INEXACT_OP);

	if (bd)
	{
		uint32_t next;
		if (!emulate_branch(r, cond_before, &next))
			goto not_ours;
		r->epc = next;
	}
	else
	{
		r->epc += 4;
	}
	return;

not_ours:
	if (chain_handler)
		chain_handler(exc);
	else
		exception_default_handler(exc);
}

void fpu_shim_install(void)
{
	chain_handler = register_exception_handler(fpu_shim_handler);
}
