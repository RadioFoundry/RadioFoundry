// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// NMEA 0183 line framing for a GPS module on a UART: bytes in, checksum-valid sentences out (docs/spec/hardware-md380.md
// 13, gotchas 13.7). Generic and pure: no UART, no RTOS. A sentence is "$" ... "*hh" (up to 82 characters by the
// standard), ended by CR and / or LF. Sentences without a valid checksum are dropped; so are overlong ones. Any talker
// (GP, GN, GL, GA, BD) is accepted: parsing the fields is the position source's job (core/aprs/aprs_nmea.h).
#ifndef NMEA_LINE_H
#define NMEA_LINE_H

#include <stdint.h>
#include <stdbool.h>

#define NMEA_LINE_LEN	88	// 82 by the standard plus slack, as core/aprs's NMEA_LINE_MAX

typedef struct {
	char buf[NMEA_LINE_LEN + 1];
	uint8_t len;
	bool in;		// inside a sentence ("$" seen)
	uint32_t good;		// sentences with a valid checksum
	uint32_t bad;		// checksum errors, overlong lines, sentences without "*hh"
} nmea_line_t;

void nmea_line_init(nmea_line_t *l);
// One received byte. Returns true when it completed a valid sentence: then l->buf holds it, NUL terminated, from "$"
// through the checksum (no CR / LF), and l->len its length, valid until the next call
bool nmea_line_feed(nmea_line_t *l, uint8_t c);
// Pure: does s (n characters, "$...*hh") carry a matching checksum
bool nmea_line_checksum_ok(const char *s, uint32_t n);

#endif
