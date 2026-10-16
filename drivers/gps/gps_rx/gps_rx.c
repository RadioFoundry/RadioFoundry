// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GPS NMEA receive ring and module detection (gps_rx.h)
#include "gps_rx.h"
#include <stddef.h>

void gps_rx_init(gps_rx_t *g, uint32_t now_ms, uint32_t listen_ms)
{
	nmea_line_init(&g->line);
	g->head = g->tail = 0;
	g->bytes = g->dropped = g->uart_errors = 0;
	g->present = false;
	g->start_ms = now_ms;
	g->listen_ms = listen_ms;
}

void gps_rx_byte(gps_rx_t *g, uint8_t c)
{
	uint16_t h = g->head, n = (uint16_t)((h + 1u) % GPS_RX_RING);
	if (n != g->tail) {
		g->ring[h] = c;
		g->head = n;
	} else {
		g->dropped++;
	}
	g->bytes++;
}

void gps_rx_error(gps_rx_t *g)
{
	g->uart_errors++;
}

gps_rx_state_t gps_rx_poll(gps_rx_t *g, uint32_t now_ms, gps_rx_sentence_fn fn)
{
	bool found = false;
	while (g->tail != g->head) {
		uint8_t c = g->ring[g->tail];
		g->tail = (uint16_t)((g->tail + 1u) % GPS_RX_RING);
		if (!nmea_line_feed(&g->line, c)) {
			continue;
		}
		if (!g->present) {
			g->present = true;
			found = true;
		}
		if (fn != NULL) {
			fn(g->line.buf, g->line.len, now_ms);
		}
	}
	if (found) {
		return GPS_RX_FOUND;
	}
	if (g->present) {
		return GPS_RX_PRESENT;
	}
	return (uint32_t)(now_ms - g->start_ms) >= g->listen_ms ? GPS_RX_ABSENT : GPS_RX_LISTENING;
}

void gps_rx_counts(const gps_rx_t *g, uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
	*good = g->line.good;
	*bad = g->line.bad + g->uart_errors + g->dropped;
	*bytes = g->bytes;
}
