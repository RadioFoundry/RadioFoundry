// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The runtime RAM-monitor loader's portable core (docs/spec/debuglink.md 3.18, hardware-dm32.md 17.2): the DL_CMD_STUB
// state machine with no RTOS or board, so the host tests run it. The firmware glue is debuglink_stub.c; the platform
// supplies the RAM span and the quiesce-and-jump (hal_system_t.stub_region / enter_stub).
#ifndef DL_STUB_H
#define DL_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *region;	// writable RAM the image is loaded into (its base is where the image runs)
	uint32_t region_size;	// capacity in bytes
	uint32_t load_addr;	// the target address region[0] maps to
	bool armed;		// a transfer has begun and not yet entered
	uint32_t total_len;	// image length a BEGIN announced
	uint32_t total_crc;	// its CRC-32 (zlib / IEEE), checked before the jump
	uint32_t entry;		// where to jump, inside [load_addr, load_addr + total_len)
	uint32_t received;	// highest offset + length written, for progress and the exact-fill check
	bool transmitting;	// set by the caller before each request: ENTER answers BUSY while the radio is on air,
				// since the monitor would leave the transmitter keyed for the whole session
} dl_stub_t;

// The biggest DATA chunk that fits a DL_REQ_MAX request (op byte + offset u32 + bytes)
#define DL_STUB_CHUNK_MAX	(64u - 1u - 4u)

void dl_stub_init(dl_stub_t *s, uint8_t *region, uint32_t region_size, uint32_t load_addr);

// Handle one DL_CMD_STUB request. Writes the reply payload (out[0] = status) and returns its length. On a verified
// ENTER it sets *enter = true and *entry to the jump address; the caller sends the reply, then jumps. *enter is left
// untouched otherwise, so the caller clears it first
size_t dl_stub_request(dl_stub_t *s, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, bool *enter,
		       uint32_t *entry);

#endif
