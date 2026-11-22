// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A randomness pool for keys (docs/spec/key-fill.md 6). The radios have no hardware random number generator: events
// whose timing a person or the air decides are stirred in with SHA-256, and only some are credited. One task stirs and
// takes (the UI task): no lock. Only entropy_wipe comes from another task
#ifndef RFOUNDRY_ENTROPY_H
#define RFOUNDRY_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

#define ENTROPY_FULL		128u	// credited bits a key may be taken at
#define ENTROPY_KEY_BITS	4u	// credited for a key press (6)

// Mixes n octets and the microsecond clock into the pool, crediting bits (capped at 256)
void entropy_stir(const void *data, size_t n, unsigned bits);
// Credited bits now (0 to 256)
unsigned entropy_bits(void);
// n octets from the pool, which then moves on so they can't be found from it later; the credit restarts
void entropy_take(uint8_t *out, size_t n);
// The pool, its count and its credit zeroed, as at power on: the emergency wipe (zeroize_ram_hook), from the
// housekeeping task with the scheduler suspended so the UI task can't stir in the middle of it
void entropy_wipe(void);

#endif
