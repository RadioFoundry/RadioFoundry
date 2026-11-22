// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// X25519 (RFC 7748 5): the Diffie-Hellman function on Curve25519, written from the RFC. Constant time in the scalar
// (the ladder swaps with a mask, never a branch). Slow but small: 16-bit limbs, no tables. The working field elements
// live in a workspace the caller owns (its heap or static memory), so the stack holds one product at a time (about
// 300 bytes): a radio's UI task can run it
#ifndef RFOUNDRY_X25519_H
#define RFOUNDRY_X25519_H

#include <stdbool.h>
#include <stdint.h>

#define X25519_LEN	32u

typedef uint32_t x25519_fe[16];	// a field element mod 2^255 - 19: 16 limbs of 16 bits, little endian
typedef struct {
	x25519_fe v[10];
} x25519_ws_t;

// out = X25519(scalar, u): scalar clamped and u's top bit masked as 5 says. False when out is all zeros (u of low
// order: 6.1 says to refuse it, as a peer who sent one forces a known secret). The workspace is wiped before return
bool x25519(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN], const uint8_t u[X25519_LEN],
	    x25519_ws_t *ws);
// A public key: X25519(scalar, 9)
void x25519_public(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN], x25519_ws_t *ws);

#endif
