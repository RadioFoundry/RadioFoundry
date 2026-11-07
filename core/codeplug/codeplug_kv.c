// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md). Reads walk the flash a few bytes at a time (no sector
// in RAM, and nothing of the area kept between calls); a write applies a batch of sets and removes to the sector in
// the storage's shared buffer and rewrites it through the journal in one step, so a power cut leaves the old area or
// the new one
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

// None while the open-firmware area holds another firmware's codeplug (codeplug_foreign): that is shown as no
// codeplug, so its bytes at 0xE000 aren't read as our keys nor written over before the user has answered
static bool area_addr(uint32_t *addr)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || codeplug_foreign() || board.flash->size == 0 || m->openfw_eeprom + CODEPLUG_KV_ADDR + STORAGE_SECTOR > board.flash->size) {
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
// the data starts, its length and its CRC. Under storage_lock
static codeplug_kv_state_t check_flash(uint32_t *data, uint16_t *len, uint16_t *count, uint32_t *crc)
{
	uint8_t b[CODEPLUG_KV_HDR];	// small chunks: no RAM of its own, little stack (the debug link's view runs deep)
	uint32_t a;
	uint16_t hdr;
	*count = 0;
	if (!area_addr(&a) || !board.flash->read(a, b, CODEPLUG_KV_HDR)) {
		return CODEPLUG_KV_EMPTY;
	}
	codeplug_kv_state_t s = header(b, &hdr, len, crc);
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
	if (c != *crc) {
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

codeplug_kv_state_t codeplug_kv_check(const uint8_t *sector)
{
	uint16_t hdr, len, count;
	uint32_t crc;
	codeplug_kv_state_t s = header(sector, &hdr, &len, &crc);
	if (s != CODEPLUG_KV_VALID) {
		return s;
	}
	return storage_crc32(0, &sector[hdr], len) == crc && walk_ok(&sector[hdr], len, &count) ? CODEPLUG_KV_VALID :
												 CODEPLUG_KV_BAD;
}

void codeplug_kv_begin(codeplug_kv_cursor_t *c, uint16_t lo, uint16_t hi)
{
	memset(c, 0, sizeof(*c));
	c->lo = lo != 0 ? lo : 1u;		// 0x0000 and 0xFFFF are no keys
	c->hi = hi != 0xFFFFu ? hi : 0xFFFEu;
	c->first = CODEPLUG_KV_NONE;
	c->done = c->lo > c->hi;
}

// Where the walk's data starts: the area checked whole at the first call, its header compared at each later one (a
// rewrite gives it a new CRC or length), so a walk never reads entries at offsets of an area that is gone. Under
// storage_lock
static bool walk_data(codeplug_kv_cursor_t *c, uint32_t *data)
{
	uint32_t a, crc;
	uint16_t len, count, hdr;
	if (c->hdr == 0) {
		if (check_flash(data, &len, &count, &crc) != CODEPLUG_KV_VALID || !area_addr(&a)) {
			return false;
		}
		c->crc = crc;
		c->len = len;
		c->hdr = (uint8_t)(*data - a);
		return true;
	}
	uint8_t h[CODEPLUG_KV_HDR];
	if (!area_addr(&a) || !board.flash->read(a, h, sizeof(h)) || header(h, &hdr, &len, &crc) != CODEPLUG_KV_VALID ||
	    hdr != c->hdr || len != c->len || crc != c->crc) {
		return false;
	}
	*data = a + hdr;
	return true;
}

// Whether an entry from offset from up to the one at end has key k (a walk's duplicate check, in flash)
static bool seen_flash(uint32_t data, uint32_t from, uint32_t end, uint16_t k)
{
	uint8_t e[ENTRY_HDR];
	for (uint32_t at = from; at < end; at += ENTRY_HDR + get16(&e[2])) {
		if (!board.flash->read(data + at, e, ENTRY_HDR) || get16(e) == k) {
			return true;	// a read that fails: skip the entry rather than give a value that may not count
		}
	}
	return false;
}

int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max)
{
	uint32_t data;
	int found = -1;
	storage_lock();
	if (!c->done && walk_data(c, &data)) {
		uint8_t e[ENTRY_HDR];
		while (found < 0 && c->at < c->len && board.flash->read(data + c->at, e, ENTRY_HDR)) {
			uint16_t k = get16(e), n = get16(&e[2]);
			uint32_t at = c->at;
			c->at = (uint16_t)(at + ENTRY_HDR + n);
			if (k < c->lo || k > c->hi) {
				continue;
			}
			if (c->first == CODEPLUG_KV_NONE) {
				c->first = (uint16_t)at;
			} else if (seen_flash(data, c->first, at, k)) {
				continue;	// a later entry of a key: no reader takes it (docs/spec/codeplug-settings.md 4)
			}
			if (!board.flash->read(data + at + ENTRY_HDR, buf, n < max ? n : max)) {
				break;
			}
			*key = k;
			found = n;
		}
	}
	c->done = found < 0;
	storage_unlock();
	return found;
}

int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	codeplug_kv_cursor_t c;
	uint16_t k;
	codeplug_kv_begin(&c, key, key);
	return codeplug_kv_next(&c, &k, buf, max);
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
	uint32_t data, crc;
	uint16_t len = 0, count = 0;
	storage_lock();
	codeplug_kv_state_t s = check_flash(&data, &len, &count, &crc);
	storage_unlock();
	*length = s == CODEPLUG_KV_VALID ? len : 0;
	*entries = s == CODEPLUG_KV_VALID ? count : 0;
	return s;
}

bool codeplug_kv_keep(uint32_t addr, const uint8_t *sector)
{
	uint32_t a, data, crc;
	uint16_t len, count;
	if (!area_addr(&a) || addr != a) {
		return false;	// another sector, or a foreign codeplug's bytes: written as they come
	}
	storage_lock();
	codeplug_kv_state_t s = check_flash(&data, &len, &count, &crc);
	storage_unlock();
	return s == CODEPLUG_KV_NEWER || (s == CODEPLUG_KV_VALID && codeplug_kv_check(sector) != CODEPLUG_KV_VALID);
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

// Take out the entry at at, which ends at next: the entries after it close the gap
static uint32_t cut(uint8_t *d, uint32_t len, uint32_t at, uint32_t next)
{
	memmove(&d[at], &d[next], len - next);
	return len - (next - at);
}

// One op on the entries d[0..*len): a set replaces the key's first entry in place or appends one, a remove takes out
// every entry of the key. *changed when a byte changed. False if a set doesn't fit in room
static bool apply(uint8_t *d, uint16_t *len, uint32_t room, const codeplug_kv_op_t *op, bool *changed)
{
	uint32_t l = *len, at = 0;
	while (at < l && get16(&d[at]) != op->key) {
		at += ENTRY_HDR + get16(&d[at + 2]);
	}
	if (op->len == CODEPLUG_KV_REMOVE) {
		while (at < l) {
			uint32_t next = at + ENTRY_HDR + get16(&d[at + 2]);
			if (get16(&d[at]) == op->key) {
				l = cut(d, l, at, next);
				*changed = true;
			} else {
				at = next;
			}
		}
	} else if (at < l) {
		uint16_t n = get16(&d[at + 2]);
		uint32_t next = at + ENTRY_HDR + n;
		if (n != op->len || (n != 0 && memcmp(&d[at + ENTRY_HDR], op->value, n) != 0)) {
			if (l - n + op->len > room) {
				return false;
			}
			memmove(&d[at + ENTRY_HDR + op->len], &d[next], l - next);
			put16(&d[at + 2], op->len);
			if (op->len != 0) {
				memcpy(&d[at + ENTRY_HDR], op->value, op->len);
			}
			l = l - n + op->len;
			*changed = true;
		}
	} else {
		if (l + ENTRY_HDR + op->len > room) {
			return false;
		}
		put16(&d[l], op->key);
		put16(&d[l + 2], op->len);
		if (op->len != 0) {
			memcpy(&d[l + ENTRY_HDR], op->value, op->len);
		}
		l += ENTRY_HDR + op->len;
		*changed = true;
	}
	*len = (uint16_t)l;
	return true;
}

// Apply the ops to the area in b (a whole sector); with *changed, b is the new area: later entries of any key dropped
// (no reader sees them), the rest 0xFF, the header and CRC made. A sector that isn't a valid area starts a new one.
// False if an op doesn't fit or the area is a newer version
static bool edit(uint8_t *b, const codeplug_kv_op_t *ops, uint8_t n, bool *changed)
{
	uint16_t hdr = CODEPLUG_KV_HDR, len = 0;
	uint32_t crc;
	codeplug_kv_state_t s = codeplug_kv_check(b);
	if (s == CODEPLUG_KV_NEWER) {
		return false;	// a later firmware's area: never ours to rewrite
	}
	if (s == CODEPLUG_KV_VALID) {
		header(b, &hdr, &len, &crc);
	} else {
		memset(b, 0, CODEPLUG_KV_HDR);	// nothing in it could be read: start over
	}
	uint8_t *d = &b[hdr];
	uint32_t room = STORAGE_SECTOR - hdr;
	*changed = false;
	for (uint8_t i = 0; i < n; i++) {
		if (!apply(d, &len, room, &ops[i], changed)) {
			return false;
		}
	}
	if (!*changed) {
		return true;
	}
	for (uint32_t at = 0; at < len; ) {
		uint32_t next = at + ENTRY_HDR + get16(&d[at + 2]);
		if (seen(d, at, get16(&d[at]))) {
			len = (uint16_t)cut(d, len, at, next);
		} else {
			at = next;
		}
	}
	memset(&d[len], 0xFF, room - len);	// erased, as the sector after the data
	put32(&b[0], CODEPLUG_KV_MAGIC);
	b[4] = CODEPLUG_KV_VERSION;
	b[5] = (uint8_t)hdr;
	put16(&b[6], len);
	put32(&b[8], storage_crc32(0, d, len));
	// bytes 12 up to hdr stay as found (spec 2.4: the continuation, a later version's header fields); a new area's are 0
	return true;
}

bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n)
{
	uint32_t a;
	for (uint8_t i = 0; i < n; i++) {
		if (ops[i].key == 0 || ops[i].key == 0xFFFFu ||
		    (ops[i].len != CODEPLUG_KV_REMOVE && ops[i].len != 0 && ops[i].value == NULL)) {
			return false;
		}
	}
	if (!area_addr(&a)) {
		return false;
	}
	storage_lock();
	uint8_t *b = storage_buffer_acquire();
	bool changed = false;
	bool ok = b != NULL && board.flash->read(a, b, STORAGE_SECTOR) && edit(b, ops, n, &changed) &&
		  (!changed || storage_sector_write(a, b));
	if (b != NULL) {
		storage_buffer_release();
	}
	if (ok && changed) {
		codeplug_note_change();	// the radio core takes the new value when it sees the serial move
	}
	storage_unlock();
	return ok;
}

bool codeplug_kv_set(uint16_t key, const void *value, uint16_t len)
{
	codeplug_kv_op_t op = { key, len, value };
	return len != CODEPLUG_KV_REMOVE && codeplug_kv_write(&op, 1);
}

bool codeplug_kv_set_u32(uint16_t key, uint32_t value)
{
	uint8_t v[4];
	put32(v, value);
	return codeplug_kv_set(key, v, sizeof(v));
}

bool codeplug_kv_remove(uint16_t key)
{
	codeplug_kv_op_t op = { key, CODEPLUG_KV_REMOVE, NULL };
	return codeplug_kv_write(&op, 1);
}
