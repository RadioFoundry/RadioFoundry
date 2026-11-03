// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md). Reads walk the flash a few bytes at a time (no sector
// in RAM); a write edits the sector in the storage's shared buffer and rewrites it through the journal in one step,
// so a power cut leaves the old area or the new one
#include <string.h>
#include "codeplug.h"
#include "codeplug_kv.h"
#include "board.h"
#include "storage/storage.h"

#define ENTRY_HDR	4u	// key, length

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static bool area_addr(uint32_t *addr)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || board.flash->size == 0 || m->openfw_eeprom + CODEPLUG_KV_ADDR + STORAGE_SECTOR > board.flash->size) {
		return false;
	}
	*addr = m->openfw_eeprom + CODEPLUG_KV_ADDR;
	return true;
}

// The header's state; on CODEPLUG_KV_VALID the header length, data length and CRC it names (the entries unchecked)
static codeplug_kv_state_t header(const uint8_t h[CODEPLUG_KV_HDR], uint16_t *hdr, uint16_t *len, uint32_t *crc)
{
	if (get32(h) != CODEPLUG_KV_MAGIC) {
		for (unsigned i = 0; i < CODEPLUG_KV_HDR; i++) {
			if (h[i] != 0xFF) {
				return CODEPLUG_KV_BAD;
			}
		}
		return CODEPLUG_KV_EMPTY;
	}
	if (h[4] != CODEPLUG_KV_VERSION) {
		return CODEPLUG_KV_NEWER;
	}
	*hdr = h[5];
	*len = get16(&h[6]);
	*crc = get32(&h[8]);
	return *hdr >= CODEPLUG_KV_HDR && (uint32_t)*hdr + *len <= STORAGE_SECTOR ? CODEPLUG_KV_VALID : CODEPLUG_KV_BAD;
}

// Every entry inside the data: true when the walk ends exactly at its end; *count entries
static bool walk_ok(const uint8_t *data, uint16_t len, uint16_t *count)
{
	uint32_t at = 0;
	*count = 0;
	while (at < len) {
		if (len - at < ENTRY_HDR || get16(&data[at + 2]) > len - at - ENTRY_HDR) {
			return false;
		}
		at += ENTRY_HDR + get16(&data[at + 2]);
		(*count)++;
	}
	return true;
}

// The area in flash, checked (magic, version, bounds, CRC, every entry inside the data). On CODEPLUG_KV_VALID: where
// the data starts and its length. Under storage_lock
static codeplug_kv_state_t check_flash(uint32_t *data, uint16_t *len, uint16_t *count)
{
	static uint8_t b[64];	// static: the lock serializes callers, and the debug link's view runs on a full stack
	uint32_t a, crc;
	uint16_t hdr;
	*count = 0;
	if (!area_addr(&a) || !board.flash->read(a, b, CODEPLUG_KV_HDR)) {
		return CODEPLUG_KV_EMPTY;
	}
	codeplug_kv_state_t s = header(b, &hdr, len, &crc);
	if (s != CODEPLUG_KV_VALID) {
		return s;
	}
	*data = a + hdr;
	uint32_t c = 0, at = 0, entry = 0;
	while (at < *len) {
		uint32_t n = *len - at < sizeof(b) ? *len - at : sizeof(b);
		if (!board.flash->read(*data + at, b, n)) {
			return CODEPLUG_KV_BAD;
		}
		c = storage_crc32(c, b, n);
		at += n;
	}
	if (c != crc) {
		return CODEPLUG_KV_BAD;
	}
	while (entry < *len) {	// the entries' framing, a header at a time
		if (*len - entry < ENTRY_HDR || !board.flash->read(*data + entry, b, ENTRY_HDR) ||
		    get16(&b[2]) > *len - entry - ENTRY_HDR) {
			return CODEPLUG_KV_BAD;
		}
		entry += ENTRY_HDR + get16(&b[2]);
		(*count)++;
	}
	return CODEPLUG_KV_VALID;
}

int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	uint32_t data;
	uint16_t len, count;
	int found = -1;
	storage_lock();
	if (key != 0 && key != 0xFFFFu && check_flash(&data, &len, &count) == CODEPLUG_KV_VALID) {
		uint8_t e[ENTRY_HDR];
		for (uint32_t at = 0; at < len && found < 0; ) {
			if (!board.flash->read(data + at, e, ENTRY_HDR)) {
				break;
			}
			uint16_t n = get16(&e[2]);
			if (get16(e) == key) {	// the first one counts (docs/spec/codeplug-settings.md 4)
				if (board.flash->read(data + at + ENTRY_HDR, buf, n < max ? n : max)) {
					found = n;
				}
				break;
			}
			at += ENTRY_HDR + n;
		}
	}
	storage_unlock();
	return found;
}

bool codeplug_kv_get_id(uint16_t key, uint32_t *id)
{
	uint8_t v[4];
	if (codeplug_kv_get(key, v, sizeof(v)) < (int)sizeof(v) || get32(v) > CODEPLUG_DMR_ID_MAX) {
		return false;	// a value that reads as absent: the radio keeps its own
	}
	*id = get32(v);
	return true;
}

codeplug_kv_state_t codeplug_kv_info(uint16_t *length, uint16_t *entries)
{
	uint32_t data;
	uint16_t len = 0, count = 0;
	storage_lock();
	codeplug_kv_state_t s = check_flash(&data, &len, &count);
	storage_unlock();
	*length = s == CODEPLUG_KV_VALID ? len : 0;
	*entries = s == CODEPLUG_KV_VALID ? count : 0;
	return s;
}

// Whether an entry before the one at end has key k
static bool seen(const uint8_t *d, uint32_t end, uint16_t k)
{
	for (uint32_t at = 0; at < end; at += ENTRY_HDR + get16(&d[at + 2])) {
		if (get16(&d[at]) == k) {
			return true;
		}
	}
	return false;
}

// Put key = value into the area in b (a whole sector): replaced in place, or appended; later entries of the same key
// dropped. A sector that isn't a valid area starts a new one. False if it doesn't fit or the area is a newer version
static bool edit(uint8_t *b, uint16_t key, const uint8_t *value, uint16_t vlen)
{
	uint16_t hdr = CODEPLUG_KV_HDR, len = 0, count;
	uint32_t crc;
	codeplug_kv_state_t s = header(b, &hdr, &len, &crc);
	if (s == CODEPLUG_KV_NEWER) {
		return false;	// a later firmware's area: never ours to rewrite
	}
	if (s != CODEPLUG_KV_VALID || storage_crc32(0, &b[hdr], len) != crc || !walk_ok(&b[hdr], len, &count)) {
		hdr = CODEPLUG_KV_HDR;	// nothing in it could be read: start over
		len = 0;
		memset(b, 0, CODEPLUG_KV_HDR);
	}
	uint8_t *d = &b[hdr];
	uint32_t room = STORAGE_SECTOR - hdr, at = 0;
	bool done = false;
	while (at < len) {
		uint16_t k = get16(&d[at]), n = get16(&d[at + 2]);
		uint32_t next = at + ENTRY_HDR + n;
		if (k == key ? done : seen(d, at, k)) {	// a later entry of a key: no reader sees it, dropped
			memmove(&d[at], &d[next], len - next);
			len = (uint16_t)(len - (next - at));
		} else if (k != key) {
			at = next;
		} else {
			if ((uint32_t)len - n + vlen > room) {
				return false;
			}
			memmove(&d[at + ENTRY_HDR + vlen], &d[next], len - next);
			put16(&d[at + 2], vlen);
			memcpy(&d[at + ENTRY_HDR], value, vlen);
			len = (uint16_t)(len - n + vlen);
			at += ENTRY_HDR + vlen;
			done = true;
		}
	}
	if (!done) {
		if ((uint32_t)len + ENTRY_HDR + vlen > room) {
			return false;
		}
		put16(&d[len], key);
		put16(&d[len + 2], vlen);
		memcpy(&d[len + ENTRY_HDR], value, vlen);
		len = (uint16_t)(len + ENTRY_HDR + vlen);
	}
	memset(&d[len], 0xFF, room - len);	// erased, as the sector after the data
	put32(&b[0], CODEPLUG_KV_MAGIC);
	b[4] = CODEPLUG_KV_VERSION;
	b[5] = (uint8_t)hdr;
	put16(&b[6], len);
	put32(&b[8], storage_crc32(0, d, len));
	memset(&b[12], 0, 4);
	return true;
}

bool codeplug_kv_set(uint16_t key, const void *value, uint16_t len)
{
	uint32_t a;
	if (key == 0 || key == 0xFFFFu || !area_addr(&a)) {
		return false;
	}
	storage_lock();
	uint8_t *b = storage_buffer_acquire();
	bool ok = b != NULL && board.flash->read(a, b, STORAGE_SECTOR) && edit(b, key, value, len) &&
		  storage_sector_write(a, b);
	if (b != NULL) {
		storage_buffer_release();
	}
	if (ok) {
		codeplug_note_change();	// the radio core takes the new value when it sees the serial move
	}
	storage_unlock();
	return ok;
}

bool codeplug_kv_set_u32(uint16_t key, uint32_t value)
{
	uint8_t v[4];
	put32(v, value);
	return codeplug_kv_set(key, v, sizeof(v));
}
