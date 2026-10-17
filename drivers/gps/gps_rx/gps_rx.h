// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A GPS module's NMEA output on a receive-only UART, the part every board shares: the interrupt puts bytes in a ring,
// the board's task drains it through nmea_line and learns whether a module is there at all (hardware-md380.md 13,
// hardware-dm32.md 12). Generic and pure: no UART, no RTOS. One writer each way: the interrupt calls gps_rx_byte and
// gps_rx_error, one task calls gps_rx_poll.
#ifndef GPS_RX_H
#define GPS_RX_H

#include <stdint.h>
#include <stdbool.h>
#include "gps/nmea_line/nmea_line.h"

#define GPS_RX_RING	256u	// 9600 baud is under 1 byte/ms; a 20 ms poll needs 20

typedef enum {
	GPS_RX_LISTENING,	// nothing valid heard yet, still within the listen time
	GPS_RX_FOUND,		// the first valid sentence came in this poll: the board settles the module's power
	GPS_RX_PRESENT,		// heard before
	GPS_RX_ABSENT,		// the listen time passed with nothing valid: the board turns the UART off
} gps_rx_state_t;

// A checksum-valid sentence ("$...*hh", no CR / LF), valid only during the call
typedef void (*gps_rx_sentence_fn)(const char *s, uint8_t len, uint32_t now_ms);

typedef struct {
	volatile uint8_t ring[GPS_RX_RING];
	volatile uint16_t head, tail;	// the interrupt moves head, the task tail
	volatile uint32_t bytes;	// every byte received, kept or not
	volatile uint32_t dropped;	// the ring was full
	volatile uint32_t uart_errors;	// characters the UART flagged (overrun, framing, parity)
	nmea_line_t line;
	bool present;
	uint32_t start_ms, listen_ms;
} gps_rx_t;

void gps_rx_init(gps_rx_t *g, uint32_t now_ms, uint32_t listen_ms);
// From the UART interrupt: one received character, or one the UART flagged (dropped)
void gps_rx_byte(gps_rx_t *g, uint8_t c);
void gps_rx_error(gps_rx_t *g);
// From the task: everything received so far through the line framing, each valid sentence to fn (may be NULL)
gps_rx_state_t gps_rx_poll(gps_rx_t *g, uint32_t now_ms, gps_rx_sentence_fn fn);
// For diagnostics: valid sentences, bad ones (checksum, length, UART errors, ring overflow), bytes received
void gps_rx_counts(const gps_rx_t *g, uint32_t *good, uint32_t *bad, uint32_t *bytes);

#endif
