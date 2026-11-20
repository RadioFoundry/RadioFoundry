// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// A channel's Accept invalid CRC (channel_t.accept_bad_crc): a message whose CRC fails is taken only when the same
// bytes failed recently, here the last RX_SEEN_N such messages. Some systems send every control message and LC with a
// check the standard rejects, and repeat them constantly; a burst damaged on the air rarely comes out the same twice.
// Each engine keeps its own table; the code is one copy (rx_seen.c, built with DMR or the air interface)

#ifndef FEC_RX_SEEN_H
#define FEC_RX_SEEN_H

#include <stdbool.h>
#include <stdint.h>

#define RX_SEEN_N	4u	// a power of two

typedef struct {
	uint16_t h[RX_SEEN_N];	// hashes, not the bytes: 2 octets each
	uint8_t at;
} rx_seen_t;

// True when the n octets at d failed recently; else they are kept, in place of the oldest
bool rx_seen(rx_seen_t *s, const uint8_t *d, uint8_t n);

#endif
