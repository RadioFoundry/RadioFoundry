// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 host port's wait for a quiet line before a write (platform/hrc7000/uart_idle.h): on a radio whose cable
// line can't take both directions at once, sending while the host still sends latched the line low until a power
// cycle (docs/spec/gotchas.md 16.5).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../platform/hrc7000/uart_idle.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define IDLE	2u
#define MAX	50u

// The ms a write starts at: the host's characters land at the times in rx[] (n of them), the write is asked for at
// ask, and the writer looks again every ms, as uart.c does (vTaskDelay(1))
static uint32_t start_at(uint32_t ask, const uint32_t *rx, unsigned n)
{
	uint32_t since = ask;
	for (uint32_t now = ask;; now++) {
		uint32_t last = 0;
		bool seen = false;
		for (unsigned i = 0; i < n && rx[i] <= now; i++) {
			last = rx[i];
			seen = true;
		}
		if (!hrc_uart_tx_wait(now, last, seen, since, IDLE, MAX)) {
			return now;
		}
	}
}

int main(void)
{
	// Nothing received yet: send at once
	CHECK(!hrc_uart_tx_wait(100, 0, false, 100, IDLE, MAX));
	// A request ended long ago: at once
	{
		const uint32_t rx[] = { 10, 11 };
		CHECK(start_at(100, rx, 2) == 100);
	}
	// The answer to a request that just ended: more than IDLE ms after its last character's tick
	{
		const uint32_t rx[] = { 98, 99, 100 };
		CHECK(start_at(100, rx, 3) == 100 + IDLE + 1u);
	}
	// The host goes on sending (PPP: the next frame right behind): wait for its end too
	{
		const uint32_t rx[] = { 100, 101, 102, 103, 104 };
		CHECK(start_at(100, rx, 5) == 104 + IDLE + 1u);
	}
	// A host that never pauses can't hold the write up for good: MAX ms at most
	{
		static uint32_t rx[400];
		for (unsigned i = 0; i < 400u; i++) {
			rx[i] = 100u + i;
		}
		CHECK(start_at(100, rx, 400) == 100 + MAX);
	}
	// Across the clock's wrap: a character at 0xFFFFFFFF holds the write until 2, the limit counts from 0xFFFFFFF0
	CHECK(hrc_uart_tx_wait(1, 0xFFFFFFFFu, true, 0xFFFFFFFFu, IDLE, MAX));
	CHECK(!hrc_uart_tx_wait(2, 0xFFFFFFFFu, true, 0xFFFFFFFFu, IDLE, MAX));
	CHECK(!hrc_uart_tx_wait(MAX - 0x10u, MAX - 0x11u, true, 0xFFFFFFF0u, IDLE, MAX));
	printf("hrc7000UartIdleTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
