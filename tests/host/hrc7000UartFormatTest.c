// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 UART set-up (platform/hrc7000/uart_format.h) against a model of a UART that ignores line control and
// divisor writes while it is busy (docs/spec/gotchas.md 2.8): receiving a character, or holding received ones. The
// host port is set up at every boot, with the host maybe still sending; plain writes left the link deaf.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// The model. Every register access is a tick of time: a character on the line keeps the UART busy for line_ticks
// accesses, then lands in the RX FIFO. With stream_every set, a new character starts that often (a host sending)
static struct {
	uint8_t dll, dlh, lcr, ier, fcr;
	uint32_t rx_count;	// characters in the RX FIFO
	uint32_t line_left;	// accesses until the character being received lands
	uint32_t line_ticks;
	uint32_t stream_every;
	uint32_t ticks;
	uint32_t thr_writes;	// stray THR writes: bytes sent to the host
} u;

static void model_reset(uint8_t dll, uint8_t lcr)
{
	u = (__typeof__(u)){ .dll = dll, .lcr = lcr };
}

static void tick(void)
{
	u.ticks++;
	if (u.line_left != 0 && --u.line_left == 0) {
		u.rx_count++;
	}
	if (u.stream_every != 0 && u.ticks % u.stream_every == 0 && u.line_left == 0) {
		u.line_left = u.line_ticks;
	}
}

static bool busy(void)
{
	return u.line_left != 0 || u.rx_count != 0;
}

static uint32_t model_rd(uint32_t off)
{
	tick();
	bool dlab = (u.lcr & 0x80u) != 0;
	switch (off) {
	case 0x00:
		if (dlab) {
			return u.dll;
		}
		if (u.rx_count != 0) {
			u.rx_count--;
		}
		return 0x55;
	case 0x04: return dlab ? u.dlh : u.ier;
	case 0x0C: return u.lcr;
	case 0x14: return u.rx_count != 0 ? 0x61u : 0x60u;
	default: return 0;
	}
}

static void model_wr(uint32_t off, uint32_t v)
{
	tick();
	bool dlab = (u.lcr & 0x80u) != 0;
	switch (off) {
	case 0x00:
		if (!dlab) {
			u.thr_writes++;
		} else if (!busy()) {
			u.dll = (uint8_t)v;
		}
		break;
	case 0x04:
		if (!dlab) {
			u.ier = (uint8_t)v;
		} else if (!busy()) {
			u.dlh = (uint8_t)v;
		}
		break;
	case 0x08: u.fcr = (uint8_t)v; break;
	case 0x0C:
		if (!busy()) {
			u.lcr = (uint8_t)v;
		}
		break;
	default: break;
	}
}

#define UART_FMT_RD(base, off)		((void)(base), model_rd(off))
#define UART_FMT_WR(base, off, v)	((void)(base), model_wr((off), (v)))
#include "../../platform/hrc7000/uart_format.h"

#define DIV	46u	// 84 MHz, 115200
#define LOADER	0x29u	// what a loader left: some other divisor, 8N1

static void check_set(void)
{
	CHECK(u.dll == DIV && u.dlh == 0 && u.lcr == 0x03 && u.fcr == 0x67 && u.ier == 0);
}

// The sequence uart.c had: plain writes, no read-back
static void plain(uint32_t div)
{
	model_wr(0x04, 0);
	model_wr(0x0C, 0x80);
	model_wr(0x00, div & 0xFFu);
	model_wr(0x04, (div >> 8) & 0xFFu);
	model_wr(0x0C, 0x03);
	model_wr(0x08, 0x67);
}

int main(void)
{
	// An idle line: the first try takes
	model_reset(LOADER, 0x03);
	CHECK(hrc_uart_format(0, DIV) == 1);
	check_set();
	CHECK(u.thr_writes == 0);

	// Characters waiting in the RX FIFO keep it busy: drained, then the first try takes
	model_reset(LOADER, 0x03);
	u.rx_count = 5;
	CHECK(hrc_uart_format(0, DIV) == 1);
	check_set();

	// A character on the line at the start: the plain sequence keeps the loader's divisor (the deaf link), and sends
	// the low divisor byte to the host as data
	model_reset(LOADER, 0x03);
	u.line_ticks = u.line_left = 30;
	plain(DIV);
	CHECK(u.dll == LOADER);
	CHECK(u.thr_writes == 1);
	// The retried set-up waits it out
	model_reset(LOADER, 0x03);
	u.line_ticks = u.line_left = 30;
	uint32_t n = hrc_uart_format(0, DIV);
	CHECK(n > 1);
	check_set();
	CHECK(u.thr_writes == 0);

	// A host sending all the time, with gaps between its frames a little longer than one try (about a dozen accesses;
	// on the radio one character is 87 us, thousands of accesses): the set-up lands in a gap
	model_reset(LOADER, 0x03);
	u.line_ticks = 40;
	u.stream_every = 60;
	u.line_left = 40;
	CHECK(hrc_uart_format(0, DIV) != 0);
	check_set();

	// Never idle: gives up after UART_FMT_TRIES, the FIFOs reset all the same
	model_reset(LOADER, 0x03);
	u.line_ticks = 1000000000u;
	u.line_left = u.line_ticks;
	CHECK(hrc_uart_format(0, DIV) == 0);
	CHECK(u.fcr == 0x67);

	// A divisor above 255 (9600 baud: 547) needs DLH too
	model_reset(LOADER, 0x03);
	CHECK(hrc_uart_format(0, 547u) == 1);
	CHECK(u.dll == (547u & 0xFFu) && u.dlh == 2 && u.lcr == 0x03);

	printf("# hrc7000UartFormatTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
