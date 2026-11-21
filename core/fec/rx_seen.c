// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "fec/rx_seen.h"

// The hash is h x 31 + octet: 31 is odd, so two messages that differ in one octet never hash the same. One that hashes
// to 0 matches an empty entry, as likely as any other collision
bool rx_seen(rx_seen_t *s, const uint8_t *d, uint8_t n)
{
	uint16_t h = 1;
	while (n--) {
		h = (uint16_t)(h * 31u + *d++);
	}
	for (uint8_t i = 0; i < RX_SEEN_N; i++) {
		if (s->h[i] == h) {
			return true;
		}
	}
	s->h[s->at++ & (RX_SEEN_N - 1u)] = h;
	return false;
}
