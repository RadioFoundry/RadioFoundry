// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HDLC framing for 1200 baud packet (docs/spec/aprs.md 4): flags, bit stuffing, the FCS, NRZI. The encoder turns
// frames into the tone sequence a board's AFSK modulator plays (one bit per symbol, 1 = mark 1200 Hz, 0 = space
// 2200 Hz); the decoder takes the demodulator's tone decisions and hands back frames whose FCS checks. Bits go out
// least significant first (AX.25 2.2 3.4, 3.6, 3.8; ISO 3309).
#ifndef ODR_HDLC_H
#define ODR_HDLC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/ax25.h"

#define HDLC_FLAG		0x7Eu
#define HDLC_FRAME_MIN		(2u * AX25_ADDR_BYTES + 2u + 2u)	// two addresses, control, PID, FCS
#define HDLC_BUF_MAX		(AX25_FRAME_MAX + 2u)			// a frame and its FCS

// Tone sequence bits: bit i of the buffer is byte i / 8, bit i % 8 (LSB first)
static inline bool hdlc_tone_bit(const uint8_t *tones, uint32_t i)
{
	return ((tones[i >> 3] >> (i & 7u)) & 1u) != 0;
}

typedef struct {
	uint8_t *buf;
	uint32_t max_bits;
	uint32_t nbits;
	uint8_t level;		// the tone now: 1 mark, 0 space (NRZI state)
	uint8_t ones;		// ones in a row since the last 0 (stuffing)
} hdlc_enc_t;

void hdlc_enc_init(hdlc_enc_t *e, uint8_t *buf, size_t bytes);
// n flags (no stuffing). False when the buffer is full
bool hdlc_enc_flags(hdlc_enc_t *e, uint16_t n);
// The frame bytes and their FCS, stuffed. False when the buffer is full
bool hdlc_enc_frame(hdlc_enc_t *e, const uint8_t *frame, size_t n);
// Flags for ms milliseconds at baud (the TXDELAY preamble or the tail); at least min flags
uint16_t hdlc_flags_for_ms(uint16_t ms, uint16_t baud, uint16_t min);

typedef struct {
	uint8_t last;		// the previous tone (NRZI)
	uint8_t ones;
	bool in_frame;		// a flag was seen: bits are collected
	uint32_t nbits;
	uint8_t buf[HDLC_BUF_MAX];
	uint32_t frames, fcs_errors, aborts;	// counters
} hdlc_dec_t;

void hdlc_dec_init(hdlc_dec_t *d);
// One tone decision. Returns the length of a frame (without its FCS, in d->buf) when a closing flag ends one whose FCS
// checks, else 0. The frame is valid until the next call
size_t hdlc_dec_bit(hdlc_dec_t *d, bool mark);

#endif
