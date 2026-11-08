// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md): keyed values in one sector of the open-firmware EEPROM
// image, for settings the open-firmware layout has no field for. Every call takes storage_lock, so any task may call
#ifndef RFOUNDRY_CODEPLUG_KV_H
#define RFOUNDRY_CODEPLUG_KV_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"	// CODEPLUG_DMR_ID_MAX

#define CODEPLUG_KV_ADDR	0xE000u		// in the EEPROM image: one STORAGE_SECTOR
#define CODEPLUG_KV_MAGIC	0x564B4652u	// bytes 52 46 4B 56, "RFKV"
#define CODEPLUG_KV_VERSION	1u
#define CODEPLUG_KV_HDR		16u		// the header length this version writes

// Keys (docs/spec/codeplug-settings.md 3): a key's meaning never changes, a new meaning gets a new key
#define CODEPLUG_KEY_ARS_SERVER	0x0001u		// u32: the ARS presence server's DMR ID, 0 = ARS off
#define CODEPLUG_KEY_IP_GATEWAY	0x0002u		// u32: the off-net IP gateway radio's DMR ID, 0 = none

// How a key's value is encoded (docs/spec/codeplug-settings.md 3.1): the encoding, never a width. A type is never
// redefined, only grown by appending bytes
enum {
	CODEPLUG_T_UINT,	// little endian, 1-8 bytes, a shorter value zero-extends
	CODEPLUG_T_INT,		// the same, sign-extending from the stored length
	CODEPLUG_T_BOOL,
	CODEPLUG_T_ENUM,	// a small uint, values only appended
	CODEPLUG_T_ID,		// a uint DMR ID, above CODEPLUG_DMR_ID_MAX reads as absent
	CODEPLUG_T_STRING,	// the bytes of the value, no terminator
	CODEPLUG_T_LIST,	// a key family: one key per record (3)
};

typedef enum {
	CODEPLUG_KV_EMPTY,	// no area (erased, or no flash map): no key is present
	CODEPLUG_KV_VALID,
	CODEPLUG_KV_BAD,	// something there that isn't a valid area: read as empty, a write starts a new one
	CODEPLUG_KV_NEWER,	// our magic with a version we don't know: read as empty and never written
} codeplug_kv_state_t;

// The value's length (at most max bytes are copied to buf), or -1 when the key is absent or the area isn't valid. Each
// call checks the whole area: to read several keys, walk them with codeplug_kv_next
int codeplug_kv_get(uint16_t key, void *buf, uint16_t max);
// A value that is a DMR ID, 1-4 bytes zero-extended: false when absent, empty or above CODEPLUG_DMR_ID_MAX (as if
// absent)
bool codeplug_kv_get_id(uint16_t key, uint32_t *id);

// The same from a value already read: v holds its first min(n, 4) bytes, n its length (from codeplug_kv_next). A longer
// value is a later version's: its first 4 bytes are the ID
static inline bool codeplug_kv_id(const uint8_t *v, int n, uint32_t *id)
{
	uint32_t x = 0;
	for (int i = 0; i < n && i < 4; i++) {
		x |= (uint32_t)v[i] << (8 * i);	// a shorter value zero-extends (docs/spec/codeplug-settings.md 3.1)
	}
	if (n < 1 || x > CODEPLUG_DMR_ID_MAX) {
		return false;
	}
	*id = x;
	return true;
}

// A walk over the entries with keys lo..hi, in the area's order, a later entry of a key skipped (the first counts). The
// area is checked whole once, at the first codeplug_kv_next; each later call reads only its header again, and the walk
// ends if the area changed under it. On the caller's stack, set up by codeplug_kv_begin
typedef struct {
	uint32_t crc;		// the header as checked: data CRC, data length, header length (0 until checked)
	uint16_t len;
	uint8_t hdr;
	uint8_t done;
	uint16_t at;		// the next entry, from the data's start
	uint16_t first;		// the first entry in range (CODEPLUG_KV_NONE: none yet), where a duplicate check starts
	uint16_t lo, hi;
} codeplug_kv_cursor_t;
#define CODEPLUG_KV_NONE	0xFFFFu
void codeplug_kv_begin(codeplug_kv_cursor_t *c, uint16_t lo, uint16_t hi);
// The next entry in range: its key in *key, its value's length (at most max bytes copied to buf); -1 at the end, when
// the area isn't valid or changed since the walk began
int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max);

// One change of a batch (codeplug_kv_write): set key to len bytes at value, or with len CODEPLUG_KV_REMOVE remove it
#define CODEPLUG_KV_REMOVE	0xFFFFu		// longer than any value can be
typedef struct {
	uint16_t key;
	uint16_t len;
	const void *value;
} codeplug_kv_op_t;
// Apply n sets and removes in order, then write the area in one journaled sector rewrite (through the shared sector
// buffer) and bump codeplug_kv_serial(). All or nothing: false, and nothing written, if any op doesn't fit, names key 0 or
// 0xFFFF, while a CPS session holds the buffer, when the storage region isn't ours or the area is CODEPLUG_KV_NEWER.
// A batch that changes nothing (a key set to the value it has, an absent key removed) is true with no write
bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n);
bool codeplug_kv_set(uint16_t key, const void *value, uint16_t len);
bool codeplug_kv_set_u32(uint16_t key, uint32_t value);
bool codeplug_kv_remove(uint16_t key);
// The state of an area held whole in RAM (a STORAGE_SECTOR), as a reader would find it in flash. No lock
codeplug_kv_state_t codeplug_kv_check(const uint8_t *sector);
// Whether a raw write of sector (STORAGE_SECTOR bytes) at flash address addr must leave the radio's sector as it is
// (docs/spec/codeplug-settings.md 6): addr is the area's sector and the radio's area is CODEPLUG_KV_NEWER, or
// CODEPLUG_KV_VALID while sector isn't. For every path that writes the EEPROM image's sectors (CPS shim, fast CPS,
// TFTP), so a file without the area never erases the radio's keys
bool codeplug_kv_keep(uint32_t addr, const uint8_t *sector);
// Moves on with every write of the area through codeplug_kv_write, so the radio core takes the new values without the
// whole codeplug's reload that codeplug_serial() brings. A raw write of the sector (CPS, TFTP) moves codeplug_serial()
// instead, whose reload reads the keys too
uint8_t codeplug_kv_serial(void);

// For the debug link: the area's state, its data length and how many entries it holds (0 unless valid)
codeplug_kv_state_t codeplug_kv_info(uint16_t *length, uint16_t *entries);

#endif
