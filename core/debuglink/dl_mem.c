// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Peek and poke (dl_mem.h, docs/spec/debuglink.md 3.24). A read can still have an effect (a status flag cleared, a FIFO
// popped): the person asking picks the address. Writes need the arming key first and stop DL_MEM_ARM_MS later.
#include "debuglink/dl_mem.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_frame.h"

static bool armed;
static uint32_t armed_ms;

void dl_mem_disarm(void)
{
	armed = false;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put(uint8_t *p, uint32_t v, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		p[i] = (uint8_t)(v >> (8u * i));
	}
}

static size_t status(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

static bool width_ok(uint8_t w, uint32_t addr)
{
	return (w == 1u || w == 2u || w == 4u) && addr % w == 0;
}

static bool is_armed(uint32_t now_ms)
{
	if (armed && now_ms - armed_ms >= DL_MEM_ARM_MS) {
		armed = false;
	}
	return armed;
}

size_t dl_mem_cmd(const dl_mem_io_t *io, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, uint32_t now_ms)
{
	if (len < 1) {
		return status(out, DL_ST_BAD_ARG);
	}
	switch (req[0]) {
	case DL_MEM_OP_READ: {
		if (len != 7) {
			return status(out, DL_ST_BAD_ARG);
		}
		uint8_t w = req[1], n = req[6];
		uint32_t addr = get32(req + 2);
		if (!width_ok(w, addr) || n == 0 || n > DL_MEM_PEEK_MAX || 1u + (size_t)n * w > cap ||
		    addr + (uint32_t)n * w - 1u < addr) {
			return status(out, DL_ST_BAD_ARG);
		}
		for (uint8_t i = 0; i < n; i++) {
			put(out + 1 + (size_t)i * w, io->rd(addr + (uint32_t)i * w, w), w);
		}
		out[0] = DL_ST_OK;
		return 1u + (size_t)n * w;
	}
	case DL_MEM_OP_WRITE:
	case DL_MEM_OP_SET:
	case DL_MEM_OP_CLEAR: {
		if (len != 10) {
			return status(out, DL_ST_BAD_ARG);
		}
		uint8_t w = req[1];
		uint32_t addr = get32(req + 2), v = get32(req + 6);
		if (!width_ok(w, addr) || cap < 5) {
			return status(out, DL_ST_BAD_ARG);
		}
		if (!is_armed(now_ms)) {
			return status(out, DL_ST_REFUSED);
		}
		if (req[0] != DL_MEM_OP_WRITE) {
			uint32_t old = io->rd(addr, w);
			v = req[0] == DL_MEM_OP_SET ? old | v : old & ~v;
		}
		io->wr(addr, w, v);
		out[0] = DL_ST_OK;
		put(out + 1, io->rd(addr, w), 4);
		return 5;
	}
	case DL_MEM_OP_ARM:
		if (len != 5 || get32(req + 1) != DL_MEM_KEY) {
			armed = false;
			return status(out, DL_ST_REFUSED);
		}
		armed = true;
		armed_ms = now_ms;
		return status(out, DL_ST_OK);
	case DL_MEM_OP_DISARM:
		armed = false;
		return status(out, DL_ST_OK);
	case DL_MEM_OP_PIN: {
		if (len != 3 || req[2] > DL_MEM_PIN_HIGH || cap < 2) {
			return status(out, DL_ST_BAD_ARG);
		}
		if (io->pin == NULL) {
			return status(out, DL_ST_UNAVAILABLE);
		}
		if (req[2] != DL_MEM_PIN_READ && !is_armed(now_ms)) {
			return status(out, DL_ST_REFUSED);
		}
		uint8_t st = 0;
		if (!io->pin(req[1], req[2], &st)) {
			return status(out, DL_ST_BAD_ARG);
		}
		out[0] = DL_ST_OK;
		out[1] = st;
		return 2;
	}
	default:
		return status(out, DL_ST_BAD_ARG);
	}
}
