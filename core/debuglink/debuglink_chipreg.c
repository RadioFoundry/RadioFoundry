// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_CHIP_REG (docs/spec/debuglink.md 3.24): one register read, or written and then read back, for bench work
// without a new build. The board's RF chips through board.chip_reg (the board's chip and page numbers, 8-bit register
// addresses); the SoC's own 32-bit registers through board.system->reg (chip DL_CHIP_SOC, a 32-bit address inside the
// platform's register windows); and peek, poke and pins at any address once armed with a key (chip DL_CHIP_MEM,
// dl_mem.c, CONFIG_DEBUG_MEM). No driver knows about a write: its next set-up of that register overwrites it.
#include "board.h"
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#include "debuglink/dl_mem.h"

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t answer(uint8_t *out, uint8_t status, uint32_t v)
{
	out[0] = status;
	if (status != DL_ST_OK) {
		return 1;
	}
	out[1] = (uint8_t)v;
	out[2] = (uint8_t)(v >> 8);
	out[3] = (uint8_t)(v >> 16);
	out[4] = (uint8_t)(v >> 24);
	return 5;
}

// chip DL_CHIP_SOC, address u32 [, value u32]
static size_t soc_reg(const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (board.system == NULL || board.system->reg == NULL) {
		return answer(out, DL_ST_UNAVAILABLE, 0);
	}
	if (len != 5 && len != 9) {
		return answer(out, DL_ST_BAD_ARG, 0);
	}
	bool write = len == 9;
	uint32_t addr = get32(req + 1);
	uint32_t v = write ? get32(req + 5) : 0u;
	// Not while transmitting: a timer or an interrupt mask written then could leave the transmitter keyed
	if (write && debuglink_transmitting()) {
		return answer(out, DL_ST_REFUSED, 0);
	}
	if (!board.system->reg(addr, &v, write)) {
		return answer(out, DL_ST_REFUSED, 0);	// outside the windows, as READ_MEM answers
	}
	if (write) {
		dlog("debuglink: soc 0x%08lX = 0x%08lX", (unsigned long)addr, (unsigned long)v);
	}
	return answer(out, DL_ST_OK, v);
}

#if CONFIG_DEBUG_MEM
// chip DL_CHIP_MEM, operation u8, its arguments (DL_MEM_OP_*): the guard and the log as for the SoC's registers, the rest
// in dl_mem.c
static size_t mem(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	uint8_t op = len >= 2u ? req[1] : 0xFFu;
	bool write = op == DL_MEM_OP_WRITE || op == DL_MEM_OP_SET || op == DL_MEM_OP_CLEAR;
	bool pin_change = op == DL_MEM_OP_PIN && len >= 4u && req[3] != DL_MEM_PIN_READ;
	if ((write || pin_change) && debuglink_transmitting()) {
		return answer(out, DL_ST_REFUSED, 0);
	}
	size_t n = debuglink_cmd_mem(req + 1, (uint16_t)(len - 1u), out, cap);
	if (out[0] == DL_ST_OK && write && n == 5u) {
		dlog("debuglink: mem %u 0x%08lX w%u = 0x%08lX", op, (unsigned long)(req[3] | (uint32_t)req[4] << 8 |
		     (uint32_t)req[5] << 16 | (uint32_t)req[6] << 24), req[2], (unsigned long)(out[1] | (uint32_t)out[2] << 8 |
		     (uint32_t)out[3] << 16 | (uint32_t)out[4] << 24));
	} else if (out[0] == DL_ST_OK && pin_change) {
		dlog("debuglink: pin %u action %u", req[2], req[3]);
	}
	return n;
}
#endif

size_t debuglink_cmd_chip_reg(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (cap < 5u) {
		return answer(out, DL_ST_BAD_ARG, 0);
	}
	if (len >= 1u && req[0] == DL_CHIP_SOC) {
		return soc_reg(req, len, out);
	}
	if (len >= 1u && req[0] == DL_CHIP_MEM) {
#if CONFIG_DEBUG_MEM
		return mem(req, len, out, cap);
#else
		return answer(out, DL_ST_UNAVAILABLE, 0);
#endif
	}
	if (board.chip_reg == NULL) {
		return answer(out, DL_ST_UNAVAILABLE, 0);
	}
	if (len != 3 && len != 7) {
		return answer(out, DL_ST_BAD_ARG, 0);
	}
	bool write = len == 7;
	uint32_t v = write ? get32(req + 3) : 0u;
	if (!board.chip_reg(req[0], req[1], req[2], &v, write)) {
		return answer(out, DL_ST_BAD_ARG, 0);
	}
	if (write) {
		dlog("debuglink: chip %u page %u reg 0x%02X = 0x%lX", req[0], req[1], req[2], (unsigned long)v);
	}
	return answer(out, DL_ST_OK, v);
}
