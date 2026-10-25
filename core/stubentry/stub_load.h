// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The runtime RAM-monitor loader's portable core (docs/spec/stub-entry.md, docs/spec/debuglink.md 3.18): the STUB
// command's state machine (INFO, BEGIN, DATA, ENTER) with no RTOS or board, so the host tests run it. The firmware glue
// is stub_entry.c; the platform supplies the RAM span and the quiesce-and-jump (hal_system_t.stub_region / enter_stub).
#ifndef STUB_LOAD_H
#define STUB_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "debuglink/dl_frame.h"

// STUB operations (first payload byte of command DL_CMD_STUB). A guarded way to hand the SoC to the host's RAM monitor
// at run time, no power cycle: INFO, then BEGIN, DATA chunks, ENTER. All u32 are little endian
enum {
	STUB_OP_INFO = 0,	// -> magic u32, load_addr u32, region_size u32, max_chunk u16, armed u8, received u32
	STUB_OP_BEGIN,		// magic u32, total_len u32, total_crc32 u32, entry u32 -> received u32 (0): arm a transfer
	STUB_OP_DATA,		// offset u32, bytes -> received u32: write a chunk (frame CRC-16 guards it)
	STUB_OP_ENTER,		// magic u32 -> status: verify total_len and CRC-32, then quiesce and jump (no reply after)
};
// The guard key, little endian in the payload: bytes 'C' '7' 'S' 'B'. Required on BEGIN and ENTER so a stray frame
// can never arm or launch the monitor; entry also needs the exact length and a matching CRC-32 of the whole image
#define STUB_MAGIC		0x42533743u
// ENTER refused (DL_ST_REFUSED) with a reason byte after the status: the platform's watchdog runs and can't be stopped
// (the STM32 IWDG), and the image header doesn't say the stub feeds it (docs/spec/stub-stm32f4.md 3.1). A bad
// magic is refused with no reason byte
#define STUB_REFUSE_WATCHDOG	1u
// The biggest DATA chunk that fits a DL_REQ_MAX request (op byte + offset u32 + bytes)
#define STUB_CHUNK_MAX		(DL_REQ_MAX - 1u - 4u)
// The longest reply: INFO's
#define STUB_REPLY_MAX		20u

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
	// Optional (NULL: any image): the platform's last check of a verified image before ENTER fires. False refuses
	// ENTER with reason STUB_REFUSE_WATCHDOG: the image doesn't feed a watchdog that can't be stopped
	bool (*image_ok)(const uint8_t *img, uint32_t len);
} stub_load_t;

void stub_load_init(stub_load_t *s, uint8_t *region, uint32_t region_size, uint32_t load_addr);

// Handle one STUB request. Writes the reply payload (out[0] = status, at most STUB_REPLY_MAX bytes) and returns its
// length. On a verified ENTER it sets *enter = true and *entry to the jump address; the caller sends the reply, then
// jumps. *enter is left untouched otherwise, so the caller clears it first
size_t stub_load_request(stub_load_t *s, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, bool *enter,
			 uint32_t *entry);

#endif
