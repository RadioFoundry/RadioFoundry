// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Safe writes to the board's data flash (docs/spec/settings-store.md 4): a sector rewrite goes through a journal (a
// copy of the new sector and a header saying where it goes), so a power cut in the middle of the erase or the
// program is finished at the next boot instead of leaving a half-erased codeplug sector. Shared by the codeplug
// editor and the legacy CPS shim. No RTOS here: storage_lock() / storage_unlock() come from storage_rtos.c in the
// firmware and from the test in host tests.
#ifndef ODR_STORAGE_H
#define ODR_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define STORAGE_SECTOR		4096u	// the erase unit the core works in (boards with another erase size: no writes)

// One recursive lock around every flash transaction sequence of the core (settings, codeplug, CPS)
void storage_lock(void);
void storage_unlock(void);
// A second lock for short RAM-only sections (the settings copy): never held across a flash operation, so the radio
// task never waits for an erase
void storage_quick_lock(void);
void storage_quick_unlock(void);

// After the board's flash works, before the settings and the codeplug are read: finish a journaled rewrite that a
// power cut interrupted. False if there is no usable flash (then every write is refused)
bool storage_init(void);
bool storage_ready(void);

// Rewrite the erase unit at addr (aligned) with data (STORAGE_SECTOR bytes), through the journal. Skipped when the
// flash already holds exactly that
bool storage_sector_write(uint32_t addr, const uint8_t *data);

// Write any range: each sector it touches is read into the shared buffer, merged and rewritten as above. False if
// the shared buffer is taken (a CPS write in progress) or the flash fails
bool storage_write(uint32_t addr, const void *buf, uint32_t len);

// The shared sector buffer (STORAGE_SECTOR bytes): NULL while someone else has it
uint8_t *storage_buffer_acquire(void);
void storage_buffer_release(void);

// The flash map, NULL without one
const hal_flash_map_t *storage_map(void);

typedef struct {
	uint32_t sector_writes;		// journaled rewrites done
	uint32_t sector_skips;		// rewrites skipped (same content)
	uint32_t replays;		// interrupted rewrites finished at boot
	uint32_t errors;		// flash operations that failed
} storage_stats_t;

void storage_get_stats(storage_stats_t *s);

// CRC-32 (IEEE 802.3, reflected, init and final XOR 0xFFFFFFFF); crc = 0 to start
uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
