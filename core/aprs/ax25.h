// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AX.25 2.2 UI frames as APRS uses them (docs/spec/aprs.md 3): addresses, frame build and parse, the FCS, and the
// TNC2 text form ("N0CALL-7>APZRDF,WIDE1-1*:payload") the APRS-IS servers and KISS monitors use. No RTOS, no board:
// the host tests run it as is.
#ifndef RFOUNDRY_AX25_H
#define RFOUNDRY_AX25_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AX25_CALL_LEN		6	// characters of a callsign without the SSID
#define AX25_ADDR_BYTES		7
#define AX25_MAX_DIGIS		8
#define AX25_INFO_MAX		256
#define AX25_CONTROL_UI		0x03
#define AX25_PID_NONE		0xF0	// no layer 3
// Header (destination, source, 8 digipeaters), control, PID, information field; without the FCS
#define AX25_FRAME_MAX		(AX25_ADDR_BYTES * (2 + AX25_MAX_DIGIS) + 2 + AX25_INFO_MAX)
#define AX25_TNC2_MAX		(10 * (2 + AX25_MAX_DIGIS + 2) + AX25_INFO_MAX + 2)

typedef struct {
	char call[AX25_CALL_LEN + 1];	// upper case letters and digits, NUL terminated
	uint8_t ssid;			// 0..15
	bool h;				// digipeater: has been repeated (the H bit); destination / source: the C bit
} ax25_addr_t;

typedef struct {
	ax25_addr_t dst, src;
	ax25_addr_t digi[AX25_MAX_DIGIS];
	uint8_t ndigi;
	uint8_t control, pid;
	const uint8_t *info;		// points into the parsed buffer, or the caller's
	uint16_t info_len;
} ax25_frame_t;

// CRC-16 of HDLC / AX.25 (ISO 3309, X.25: reflected 0x1021, init 0xFFFF, final complement). Appended low byte first
uint16_t ax25_fcs(const uint8_t *p, size_t n);

// "N0CALL-7" -> address. Accepts 1..6 letters / digits (lower case is folded), an optional -0..-15. False otherwise
bool ax25_addr_parse(const char *s, size_t n, ax25_addr_t *a);
// "N0CALL-7" ("N0CALL" for SSID 0), with a '*' when star and the H bit is set. Returns the length written
size_t ax25_addr_format(const ax25_addr_t *a, bool star, char *out);
bool ax25_addr_equal(const ax25_addr_t *a, const ax25_addr_t *b);	// call and SSID

// A UI frame to bytes (no FCS). Returns the length, 0 if it doesn't fit max
size_t ax25_encode(const ax25_frame_t *f, uint8_t *out, size_t max);
// Bytes (no FCS) to a frame; info points into buf. False for anything but a well formed frame with 2..10 addresses
bool ax25_decode(const uint8_t *buf, size_t n, ax25_frame_t *f);

// TNC2 monitor text: "SRC>DST,DIGI1,DIGI2*:info" (the last repeated digipeater carries the '*'). NUL terminated;
// returns the length, 0 if it doesn't fit
size_t ax25_to_tnc2(const ax25_frame_t *f, char *out, size_t max);
// The reverse, for lines from APRS-IS or a test vector. info points into line. Source and destination must be AX.25
// addresses; path entries that aren't (server names longer than 6 characters) are skipped, at most AX25_MAX_DIGIS kept
bool ax25_from_tnc2(const char *line, size_t n, ax25_frame_t *f);

#endif
