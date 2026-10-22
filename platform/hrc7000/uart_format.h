// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C7000 UARTs' divisor and 8N1 set-up, written so it survives a busy UART (docs/spec/gotchas.md 2.8): the UART
// ignores a line control or divisor write while it is receiving or holds received characters. The host port is set
// up at every boot, and after a reset the host may still be sending (a test run's next request): plain
// writes were dropped then, the divisor stayed the loader's, and the link stayed deaf until a power cycle.
// The includer defines UART_FMT_RD(base, off) and UART_FMT_WR(base, off, v): uart.c with the registers, the host test
// with a model of a UART that drops writes while busy.
#ifndef PLATFORM_HRC7000_UART_FORMAT_H
#define PLATFORM_HRC7000_UART_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#define UART_FMT_RBR		0x00u	// RBR / THR / DLL
#define UART_FMT_IER		0x04u	// IER / DLH
#define UART_FMT_FCR		0x08u	// IIR (read) / FCR (write)
#define UART_FMT_LCR		0x0Cu
#define UART_FMT_LSR		0x14u
#define UART_FMT_DLAB		0x80u
#define UART_FMT_8N1		0x03u
#define UART_FMT_LSR_DR		0x01u
#define UART_FMT_FIFOS		0x67u	// FIFOs on and reset, RX trigger 1 character
// Tries before giving up: a few register accesses each, so about 100 ms. Host frames leave gaps on the line (each
// request waits for its answer), so a try lands in one long before that
#define UART_FMT_TRIES		100000u

// always_inline: the stub hand-over runs it from RAM and must not branch into XIP flash. The number of the try that
// took (1 on an idle line), 0 if every try was dropped
static inline __attribute__((always_inline)) uint32_t hrc_uart_format(uint32_t base, uint32_t div)
{
	uint32_t lo = div & 0xFFu, hi = (div >> 8) & 0xFFu;
	UART_FMT_WR(base, UART_FMT_IER, 0u);
	for (uint32_t n = 1; n <= UART_FMT_TRIES; n++) {
		// Received characters keep it busy: drop them (bounded: with DLAB stuck on, RBR reads DLL and DR stays set)
		for (uint32_t i = 0; i < 64u && (UART_FMT_RD(base, UART_FMT_LSR) & UART_FMT_LSR_DR) != 0; i++) {
			(void)UART_FMT_RD(base, UART_FMT_RBR);
		}
		UART_FMT_WR(base, UART_FMT_LCR, UART_FMT_DLAB);
		if ((UART_FMT_RD(base, UART_FMT_LCR) & 0xFFu) != UART_FMT_DLAB) {
			continue;	// dropped: DLL and DLH would be THR and IER now
		}
		UART_FMT_WR(base, UART_FMT_RBR, lo);
		UART_FMT_WR(base, UART_FMT_IER, hi);
		bool ok = (UART_FMT_RD(base, UART_FMT_RBR) & 0xFFu) == lo && (UART_FMT_RD(base, UART_FMT_IER) & 0xFFu) == hi;
		UART_FMT_WR(base, UART_FMT_LCR, UART_FMT_8N1);
		if (ok && (UART_FMT_RD(base, UART_FMT_LCR) & 0xFFu) == UART_FMT_8N1) {
			UART_FMT_WR(base, UART_FMT_FCR, UART_FMT_FIFOS);
			return n;
		}
	}
	UART_FMT_WR(base, UART_FMT_FCR, UART_FMT_FIFOS);
	return 0;
}

#endif
