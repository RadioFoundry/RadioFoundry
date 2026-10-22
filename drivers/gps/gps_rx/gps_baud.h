// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Finding a GPS module's baud rate: listen at each rate of a list in turn until a checksum-valid sentence comes
// (gps_rx.h), for boards whose module rate isn't settled (docs/spec/hardware-md9600.md 13). A line that stays silent
// for a whole window means no module at all: there is nothing to try other rates on, so the hunt ends there and a
// radio without a module stops listening after one window. A line with characters but no valid sentence is a module at
// another rate: the next one is tried. Generic and pure: no UART, no RTOS.
#ifndef GPS_BAUD_H
#define GPS_BAUD_H

#include <stdint.h>
#include <stdbool.h>
#include "gps/gps_rx/gps_rx.h"

typedef enum {
	GPS_BAUD_LISTEN,	// keep listening at the current rate
	GPS_BAUD_SWITCH,	// set the UART to gps_baud_rate() now
	GPS_BAUD_FOUND,		// a module was heard: keep the rate
	GPS_BAUD_ABSENT,	// no module: the board turns the UART off
} gps_baud_step_t;

typedef struct {
	const uint32_t *rates;	// tried in order
	uint8_t n, idx;
	uint32_t window_ms;	// listening time per rate
	uint32_t t0;		// when the current rate started
	uint32_t activity0;	// characters and UART errors seen until then
	bool done;		// FOUND or ABSENT given
	gps_baud_step_t last;
} gps_baud_t;

// Start at rates[0]; activity: the receive path's characters plus flagged characters so far
void gps_baud_init(gps_baud_t *b, const uint32_t *rates, uint8_t n, uint32_t window_ms, uint32_t now_ms,
		   uint32_t activity);
uint32_t gps_baud_rate(const gps_baud_t *b);
// After each gps_rx_poll: its result, the time, the activity count. After FOUND or ABSENT it keeps returning that
gps_baud_step_t gps_baud_step(gps_baud_t *b, gps_rx_state_t rx, uint32_t now_ms, uint32_t activity);

#endif
