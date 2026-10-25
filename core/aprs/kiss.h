// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// KISS framing (Chepponis and Karn, "The KISS TNC", 1987; docs/spec/aprs.md 8): FEND 0xC0 frames, FESC escapes, a
// type byte (port in the high nibble, command in the low) and the data. Data frames (command 0) carry an AX.25 frame
// without its FCS. Our own frames use command 6 (SetHardware, "specific to each TNC") with a tag byte: 'G' an
// APRS-IS line for the iGate helper (both ways), 'N' an NMEA sentence from the host.
#ifndef RFOUNDRY_KISS_H
#define RFOUNDRY_KISS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/ax25.h"

#define KISS_FEND		0xC0u
#define KISS_FESC		0xDBu
#define KISS_TFEND		0xDCu
#define KISS_TFESC		0xDDu

#define KISS_CMD_DATA		0x00u
#define KISS_CMD_TXDELAY	0x01u	// 10 ms units
#define KISS_CMD_PERSIST	0x02u	// p = (value + 1) / 256
#define KISS_CMD_SLOTTIME	0x03u	// 10 ms units
#define KISS_CMD_TXTAIL		0x04u
#define KISS_CMD_FULLDUPLEX	0x05u
#define KISS_CMD_SETHW		0x06u
#define KISS_CMD_RETURN		0xFFu	// the whole type byte

#define KISS_TAG_IS_LINE	'G'	// SetHardware: an APRS-IS (TNC2) line
#define KISS_TAG_NMEA		'N'	// SetHardware: an NMEA sentence

#define KISS_FRAME_MAX		(1u + AX25_TNC2_MAX)	// the type byte and the largest payload we take

// Encode type + data into out; returns the length (0 when it doesn't fit max). Worst case 2 * (n + 1) + 2 bytes
size_t kiss_encode(uint8_t type, const uint8_t *data, size_t n, uint8_t *out, size_t max);

typedef struct {
	uint8_t buf[KISS_FRAME_MAX];
	uint16_t len;
	bool in_frame;
	bool esc;
	bool overflow;
	uint32_t frames, overflows, bad_escapes;
} kiss_dec_t;

void kiss_dec_init(kiss_dec_t *k);
// One byte from the host. Returns the length of a complete frame (type byte first, in k->buf), else 0. Empty frames
// (FEND FEND) are skipped
size_t kiss_dec_byte(kiss_dec_t *k, uint8_t b);
// Between frames: the next byte would start a new one (the host link may give the port to another protocol here)
static inline bool kiss_dec_idle(const kiss_dec_t *k)
{
	return !k->in_frame || k->len == 0;
}

#endif
