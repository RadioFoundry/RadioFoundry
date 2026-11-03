// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md): keyed values in one sector of the open-firmware EEPROM
// image, for settings the open-firmware layout has no field for. Every call takes storage_lock, so any task may call
#ifndef RFOUNDRY_CODEPLUG_KV_H
#define RFOUNDRY_CODEPLUG_KV_H

#include <stdint.h>
#include <stdbool.h>

#define CODEPLUG_KV_ADDR	0xE000u		// in the EEPROM image: one STORAGE_SECTOR
#define CODEPLUG_KV_MAGIC	0x564B4652u	// bytes 52 46 4B 56, "RFKV"
#define CODEPLUG_KV_VERSION	1u
#define CODEPLUG_KV_HDR		16u		// the header length this version writes

// Keys (docs/spec/codeplug-settings.md 3): a key's meaning never changes, a new meaning gets a new key
#define CODEPLUG_KEY_ARS_SERVER	0x0001u		// u32: the ARS presence server's DMR ID, 0 = ARS off
#define CODEPLUG_KEY_IP_GATEWAY	0x0002u		// u32: the off-net IP gateway radio's DMR ID, 0 = none

typedef enum {
	CODEPLUG_KV_EMPTY,	// no area (erased, or no flash map): no key is present
	CODEPLUG_KV_VALID,
	CODEPLUG_KV_BAD,	// something there that isn't a valid area: read as empty, a write starts a new one
	CODEPLUG_KV_NEWER,	// our magic with a version we don't know: read as empty and never written
} codeplug_kv_state_t;

// The value's length (at most max bytes are copied to buf), or -1 when the key is absent or the area isn't valid
int codeplug_kv_get(uint16_t key, void *buf, uint16_t max);
// A u32 value that is a DMR ID: false when absent, shorter than 4 bytes or above CODEPLUG_DMR_ID_MAX (as if absent)
bool codeplug_kv_get_id(uint16_t key, uint32_t *id);
// Write one key (one journaled sector rewrite, through the shared sector buffer) and bump codeplug_serial(). False
// while a CPS session holds the buffer, when the storage region isn't ours, the area is CODEPLUG_KV_NEWER or full
bool codeplug_kv_set(uint16_t key, const void *value, uint16_t len);
bool codeplug_kv_set_u32(uint16_t key, uint32_t value);
// The state of an area held whole in RAM (a STORAGE_SECTOR), as a reader would find it in flash: for a write that
// carries the whole sector (TFTP codeplug.bin), which takes it only when it is CODEPLUG_KV_VALID. No lock
codeplug_kv_state_t codeplug_kv_check(const uint8_t *sector);
// For the debug link: the area's state, its data length and how many entries it holds (0 unless valid)
codeplug_kv_state_t codeplug_kv_info(uint16_t *length, uint16_t *entries);

#endif
