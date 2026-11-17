// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Whether a host port write waits for the host to stop sending (uart.c, HRC_HOST_TX_IDLE_MS). A board whose cable
// line can't take both directions at once (docs/spec/gotchas.md 16.5) sends only after no character has arrived for
// idle_ms, or after max_ms of waiting, so a host that never pauses can't hold the link up for good. Pure, for the
// host test.
#ifndef PLATFORM_HRC7000_UART_IDLE_H
#define PLATFORM_HRC7000_UART_IDLE_H

#include <stdbool.h>
#include <stdint.h>

// now, last_rx and since in ms (hal_millis): last_rx the last received character, since the start of this wait.
// With 1 ms ticks, a character at the end of tick last_rx is idle_ms old at the earliest when now - last_rx exceeds
// idle_ms
static inline bool hrc_uart_tx_wait(uint32_t now, uint32_t last_rx, bool rx_seen, uint32_t since, uint32_t idle_ms,
	uint32_t max_ms)
{
	if (!rx_seen || now - since >= max_ms) {
		return false;
	}
	return now - last_rx <= idle_ms;
}

#endif
