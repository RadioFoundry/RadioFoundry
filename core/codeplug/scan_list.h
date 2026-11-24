// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (docs/spec/scan-list.md): a named list of channel numbers from any zone, in a 256-byte record with
// its own CRC, 16 to a 4 KB sector, in the board's scan list area (hal_flash_map_t.scan_addr / scan_size). A change
// rewrites the record's sector through the journal (core/storage), so a power cut leaves the old sector or the new
// one. The records are read from flash when asked for: no RAM copy. Writes block on the flash; a task that must not
// wait (the UI) hands them to the housekeeping task (storage_job_post).
//
// A list is a zone_t (codeplug.h): the zone helpers in zone_edit.h that change one in RAM serve a scan list too. A list
// is named by its slot, the way a zone is by its index: deleting one leaves a free slot, and the lists after it keep
// their numbers. A channel number the codeplug no longer holds stays in the list, and the scan skips it.
#ifndef SCAN_LIST_H
#define SCAN_LIST_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

#define SCAN_LIST_RECORD	256u
#define SCAN_LIST_MAGIC		0x4C43534Fu	// "OSCL": bytes 4F 53 43 4C
#define SCAN_LIST_LAYOUT	1u
#define SCAN_LIST_MAX		CODEPLUG_ZONE_MAX	// channels in a list

typedef zone_t scan_list_t;

typedef enum {
	SCAN_LIST_NONE,		// no area on this board (or not usable: the flash, a bad map, the storage region foreign)
	SCAN_LIST_OURS,		// every slot free or one of our records
	SCAN_LIST_FOREIGN,	// a slot holds something else: read only, never written
} scan_list_state_t;

// The area's state, from a scan of its slots
scan_list_state_t scan_list_state(void);
// Slots in the area (16 a sector), 0 without one
uint16_t scan_list_slots(void);
// Slot i's list; false when it is free or not a valid record
bool scan_list_read(uint16_t slot, scan_list_t *out);
// Lists in the area
uint16_t scan_list_count(void);
// Store l in slot, replacing what is there. False for a slot outside the area, a list that isn't valid (no name, more
// than SCAN_LIST_MAX channels, channel number 0 or one twice), a foreign area, or when the flash fails
bool scan_list_write(uint16_t slot, const scan_list_t *l);
// Store l in the first free slot: the slot, or -1 when full or when scan_list_write would refuse it
int scan_list_add(const scan_list_t *l);
// Free a slot; false when it holds no list or the write fails
bool scan_list_delete(uint16_t slot);

// Erase the whole area, whatever it holds (the way out of FOREIGN once the flash is backed up). False without an area
// or when the flash fails
bool scan_list_clear(void);

// Counts every change written (write, add, delete, clear): a reader that keeps what it read knows to read again
uint32_t scan_list_generation(void);

// A record's 256 bytes and back (little endian), for the store, the storage claim and the tests. Encode returns false
// for a list that isn't valid (see scan_list_write)
bool scan_list_encode(const scan_list_t *l, uint8_t out[SCAN_LIST_RECORD]);
bool scan_list_decode(const uint8_t in[SCAN_LIST_RECORD], scan_list_t *out);

#endif
