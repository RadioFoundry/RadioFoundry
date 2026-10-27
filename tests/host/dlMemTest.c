// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's peek and poke (core/debuglink/dl_mem.c, docs/spec/debuglink.md 3.23) on a 64-byte buffer: widths,
// alignment, the arming key and its time-out, set and clear, the pin action
#include <stdio.h>
#include <string.h>
#include "debuglink/dl_mem.h"
#include "debuglink/debuglink_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define BASE	0x20000000u
static uint8_t mem[64];

static uint32_t rd(uint32_t a, uint8_t w)
{
	uint32_t v = 0;
	for (uint8_t i = 0; i < w; i++) {
		v |= (uint32_t)mem[a - BASE + i] << (8u * i);
	}
	return v;
}

static void wr(uint32_t a, uint8_t w, uint32_t v)
{
	for (uint8_t i = 0; i < w; i++) {
		mem[a - BASE + i] = (uint8_t)(v >> (8u * i));
	}
}

static uint8_t pins[4];	// bit 0 output, bit 2 latch; the level follows the latch when driven
static bool pin(uint8_t p, uint8_t action, uint8_t *state)
{
	if (p >= 4) {
		return false;
	}
	if (action == DL_MEM_PIN_INPUT) {
		pins[p] &= (uint8_t)~DL_MEM_PS_OUTPUT;
	} else if (action != DL_MEM_PIN_READ) {
		pins[p] = (uint8_t)(DL_MEM_PS_OUTPUT | (action == DL_MEM_PIN_HIGH ? DL_MEM_PS_LATCH : 0));
	}
	*state = (uint8_t)(pins[p] | ((pins[p] & DL_MEM_PS_LATCH) ? DL_MEM_PS_LEVEL : 0));
	return true;
}

static const dl_mem_io_t io = { rd, wr, pin };
static const dl_mem_io_t io_no_pin = { rd, wr, NULL };
static uint8_t out[300];

static void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static size_t req_read(uint8_t w, uint32_t a, uint8_t n)
{
	uint8_t r[7] = { DL_MEM_OP_READ, w };
	put32(r + 2, a);
	r[6] = n;
	return dl_mem_cmd(&io, r, 7, out, sizeof(out), 0);
}

static size_t req_write(uint8_t op, uint8_t w, uint32_t a, uint32_t v, uint32_t now)
{
	uint8_t r[10] = { op, w };
	put32(r + 2, a);
	put32(r + 6, v);
	return dl_mem_cmd(&io, r, 10, out, sizeof(out), now);
}

static size_t req_arm(uint32_t key, uint32_t now)
{
	uint8_t r[5] = { DL_MEM_OP_ARM };
	put32(r + 1, key);
	return dl_mem_cmd(&io, r, 5, out, sizeof(out), now);
}

int main(void)
{
	for (int i = 0; i < 64; i++) {
		mem[i] = (uint8_t)i;
	}
	// Reads at each width, little endian, count values
	CHECK(req_read(1, BASE + 3, 2) == 3 && out[0] == DL_ST_OK && out[1] == 3 && out[2] == 4);
	CHECK(req_read(2, BASE + 4, 1) == 3 && out[1] == 4 && out[2] == 5);
	CHECK(req_read(4, BASE + 8, 2) == 9 && out[1] == 8 && out[8] == 15);
	// Unaligned, a bad width, no count, too many: refused as bad arguments
	CHECK(req_read(4, BASE + 2, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(req_read(2, BASE + 1, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(req_read(3, BASE, 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(req_read(1, BASE, 0) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(req_read(1, BASE, DL_MEM_PEEK_MAX + 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(req_read(4, 0xFFFFFFFCu, 2) == 1 && out[0] == DL_ST_BAD_ARG);	// wraps
	// Writes refused until armed; a wrong key doesn't arm
	CHECK(req_write(DL_MEM_OP_WRITE, 4, BASE, 0xDEADBEEFu, 0) == 1 && out[0] == DL_ST_REFUSED && mem[0] == 0);
	CHECK(req_arm(0x12345678u, 0) == 1 && out[0] == DL_ST_REFUSED);
	CHECK(req_write(DL_MEM_OP_WRITE, 4, BASE, 0xDEADBEEFu, 0) == 1 && out[0] == DL_ST_REFUSED);
	CHECK(req_arm(DL_MEM_KEY, 1000) == 1 && out[0] == DL_ST_OK);
	CHECK(req_write(DL_MEM_OP_WRITE, 4, BASE, 0xDEADBEEFu, 1000) == 5 && out[0] == DL_ST_OK);
	CHECK(mem[0] == 0xEF && mem[3] == 0xDE && out[1] == 0xEF && out[4] == 0xDE);
	CHECK(req_write(DL_MEM_OP_WRITE, 1, BASE + 5, 0x1FFu, 1000) == 5 && mem[5] == 0xFF && mem[6] == 6);
	// Set and clear: read, modify, write; the value read back
	CHECK(req_write(DL_MEM_OP_SET, 2, BASE + 8, 0x8001u, 2000) == 5 && rd(BASE + 8, 2) == 0x8909u);
	CHECK(req_write(DL_MEM_OP_CLEAR, 2, BASE + 8, 0x0009u, 2000) == 5 && rd(BASE + 8, 2) == 0x8900u);
	CHECK(out[1] == 0x00 && out[2] == 0x89);
	// The arming runs out DL_MEM_ARM_MS after the key (the clock wraps)
	CHECK(req_write(DL_MEM_OP_WRITE, 1, BASE, 1, 1000 + DL_MEM_ARM_MS - 1) == 5);
	CHECK(req_write(DL_MEM_OP_WRITE, 1, BASE, 2, 1000 + DL_MEM_ARM_MS) == 1 && out[0] == DL_ST_REFUSED && mem[0] == 1);
	CHECK(req_arm(DL_MEM_KEY, 0xFFFFFF00u) == 1 && req_write(DL_MEM_OP_WRITE, 1, BASE, 3, 0x100u) == 5 && mem[0] == 3);
	// Disarm; a wrong key also disarms
	uint8_t d = DL_MEM_OP_DISARM;
	CHECK(dl_mem_cmd(&io, &d, 1, out, sizeof(out), 0x100u) == 1 && out[0] == DL_ST_OK);
	CHECK(req_write(DL_MEM_OP_WRITE, 1, BASE, 4, 0x100u) == 1 && out[0] == DL_ST_REFUSED);
	req_arm(DL_MEM_KEY, 0);
	req_arm(1, 0);
	CHECK(req_write(DL_MEM_OP_WRITE, 1, BASE, 4, 0) == 1 && out[0] == DL_ST_REFUSED && mem[0] == 3);
	// Pins: a read needs no key; an action does; no such pin; no hook
	uint8_t p[3] = { DL_MEM_OP_PIN, 1, DL_MEM_PIN_READ };
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 2 && out[0] == DL_ST_OK && out[1] == 0);
	p[2] = DL_MEM_PIN_HIGH;
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 1 && out[0] == DL_ST_REFUSED && pins[1] == 0);
	req_arm(DL_MEM_KEY, 0);
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 2 &&
	      out[1] == (DL_MEM_PS_OUTPUT | DL_MEM_PS_LEVEL | DL_MEM_PS_LATCH));
	p[2] = DL_MEM_PIN_INPUT;
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 2 && (out[1] & DL_MEM_PS_OUTPUT) == 0);
	p[1] = 9;
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 1 && out[0] == DL_ST_BAD_ARG);
	p[2] = 7;
	CHECK(dl_mem_cmd(&io, p, 3, out, sizeof(out), 0) == 1 && out[0] == DL_ST_BAD_ARG);
	p[1] = 1;
	p[2] = DL_MEM_PIN_READ;
	CHECK(dl_mem_cmd(&io_no_pin, p, 3, out, sizeof(out), 0) == 1 && out[0] == DL_ST_UNAVAILABLE);
	// Malformed
	CHECK(dl_mem_cmd(&io, p, 0, out, sizeof(out), 0) == 1 && out[0] == DL_ST_BAD_ARG);
	uint8_t bad = 0x40;
	CHECK(dl_mem_cmd(&io, &bad, 1, out, sizeof(out), 0) == 1 && out[0] == DL_ST_BAD_ARG);
	printf("dlMemTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures != 0;
}
