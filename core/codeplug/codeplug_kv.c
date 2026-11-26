// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug settings area (docs/spec/codeplug-settings.md). Reads walk the flash a few bytes at a time (no sector
// in RAM, and nothing of the area kept between calls), the first sector and then the continuation it names (2.4); a
// write applies a batch of sets and removes to a sector in the storage's shared buffer and rewrites it through the
// journal in one step, so a power cut leaves the old sector or the new one
#include <string.h>
#include "codeplug.h"
#include "codeplug_kv.h"
#include "board.h"
#include "storage/storage.h"

#define ENTRY_HDR	4u	// key, length
#define IMAGE_SIZE	0x10000u	// the EEPROM image: a continuation's offset is inside it (2.4)

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// A checked sector of the area: where its entries start, their length, count and CRC, and the continuation its header
// names (bytes 12-15, 0 = none)
typedef struct {
	uint32_t data;
	uint32_t crc;
	uint32_t next;
	uint16_t len, count;
} sector_t;

static bool in_cont(uint16_t key)
{
	return key >= CODEPLUG_KV_CONT_LO && key <= CODEPLUG_KV_CONT_HI;
}

// The flash address of the sector at off in the EEPROM image. None while the open-firmware area holds another
// firmware's codeplug (codeplug_foreign): that is shown as no codeplug, so its bytes at 0xE000 aren't read as our keys
// nor written over before the user has answered
static bool sector_addr(uint32_t off, uint32_t *addr)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || codeplug_foreign() || board.flash->size == 0 || off % STORAGE_SECTOR != 0 || off + STORAGE_SECTOR > IMAGE_SIZE ||
	    m->openfw_eeprom + off + STORAGE_SECTOR > board.flash->size) {
		return false;
	}
	*addr = m->openfw_eeprom + off;
	return true;
}

static bool area_addr(uint32_t *addr)
{
	return sector_addr(CODEPLUG_KV_ADDR, addr);
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
		// No version 0 was ever written: a damaged byte, not a later firmware's area that must be left alone
		return h[4] > CODEPLUG_KV_VERSION ? CODEPLUG_KV_NEWER : CODEPLUG_KV_BAD;
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

// The sector at flash address a, checked (magic, version, bounds, CRC, every entry inside the data). On
// CODEPLUG_KV_VALID s describes it. Under storage_lock
static codeplug_kv_state_t check_flash(uint32_t a, sector_t *s)
{
	uint8_t b[CODEPLUG_KV_HDR];	// small chunks: no RAM of its own, little stack (the debug link's view runs deep)
	uint16_t hdr;
	s->count = 0;
	s->len = 0;
	if (!board.flash->read(a, b, CODEPLUG_KV_HDR)) {
		return CODEPLUG_KV_EMPTY;
	}
	codeplug_kv_state_t st = header(b, &hdr, &s->len, &s->crc);
	if (st != CODEPLUG_KV_VALID) {
		return st;
	}
	s->next = get32(&b[12]);
	s->data = a + hdr;
	uint32_t c = 0, at = 0, entry = 0;
	while (at < s->len) {
		uint32_t n = s->len - at < sizeof(b) ? s->len - at : sizeof(b);
		if (!board.flash->read(s->data + at, b, n)) {
			return CODEPLUG_KV_BAD;
		}
		c = storage_crc32(c, b, n);
		at += n;
	}
	if (c != s->crc) {
		return CODEPLUG_KV_BAD;
	}
	while (entry < s->len) {	// the entries' framing, a header at a time
		if (s->len - entry < ENTRY_HDR || !board.flash->read(s->data + entry, b, ENTRY_HDR) ||
		    get16(&b[2]) > s->len - entry - ENTRY_HDR) {
			return CODEPLUG_KV_BAD;
		}
		entry += ENTRY_HDR + get16(&b[2]);
		s->count++;
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

// The walk takes sector s (at flash address a): its header as checked, and the sector it names. A pointer past the
// image can't be followed
static void enter(codeplug_kv_cursor_t *c, uint32_t a, const sector_t *s)
{
	c->crc = s->crc;
	c->len = s->len;
	c->hdr = (uint8_t)(s->data - a);
	c->next = s->next < IMAGE_SIZE ? (uint16_t)s->next : 0;
	c->at = 0;
	c->first = CODEPLUG_KV_NONE;
}

// Where the current sector's data starts: the first sector checked whole at the first call, its header compared at each
// later one (a rewrite gives it a new CRC or length), so a walk never reads entries at offsets of a sector that is
// gone. Under storage_lock
static bool walk_data(codeplug_kv_cursor_t *c, uint32_t *data)
{
	uint32_t a, crc;
	uint16_t len, hdr;
	if (c->hdr == 0) {
		sector_t s;
		if (!area_addr(&a) || check_flash(a, &s) != CODEPLUG_KV_VALID) {
			return false;
		}
		c->sector = CODEPLUG_KV_ADDR;
		c->visited = (uint16_t)(1u << (CODEPLUG_KV_ADDR / STORAGE_SECTOR));
		enter(c, a, &s);
		*data = s.data;
		return true;
	}
	uint8_t h[CODEPLUG_KV_HDR];
	if (!sector_addr(c->sector, &a) || !board.flash->read(a, h, sizeof(h)) || header(h, &hdr, &len, &crc) != CODEPLUG_KV_VALID ||
	    hdr != c->hdr || len != c->len || crc != c->crc) {
		return false;
	}
	*data = a + hdr;
	return true;
}

// Move on to the sector the current one names (2.4): a multiple of the sector size inside the image, not reached
// already, a valid area, and at most CODEPLUG_KV_CHAIN_MAX in all. False ends the walk: the chain stops at the first
// sector that is missing, erased, bad or newer. Under storage_lock
static bool advance(codeplug_kv_cursor_t *c, uint32_t *data)
{
	uint32_t a;
	sector_t s;
	unsigned reached = 0;
	for (unsigned v = c->visited; v != 0; v >>= 1) {
		reached += v & 1u;
	}
	uint16_t bit = (uint16_t)(1u << (c->next / STORAGE_SECTOR));
	if (c->next == 0 || (c->visited & bit) != 0 || reached >= CODEPLUG_KV_CHAIN_MAX ||
	    !sector_addr(c->next, &a) || check_flash(a, &s) != CODEPLUG_KV_VALID) {
		return false;
	}
	if (c->first != CODEPLUG_KV_NONE) {
		c->earlier = 1;
	}
	c->visited |= bit;
	c->sector = c->next;
	enter(c, a, &s);
	*data = s.data;
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

// Whether a sector before the walk's current one has an entry with key k: the key is read from the earlier (2.4)
static bool seen_earlier(const codeplug_kv_cursor_t *c, uint16_t k)
{
	uint32_t off = CODEPLUG_KV_ADDR;
	for (unsigned i = 0; i < CODEPLUG_KV_CHAIN_MAX && off != c->sector; i++) {
		uint32_t a, crc;
		uint16_t hdr, len;
		uint8_t h[CODEPLUG_KV_HDR];
		if (!sector_addr(off, &a) || !board.flash->read(a, h, sizeof(h)) || header(h, &hdr, &len, &crc) != CODEPLUG_KV_VALID ||
		    seen_flash(a + hdr, 0, len, k)) {
			return true;	// gone under the walk: skip, as a failed read does
		}
		off = get32(&h[12]);
	}
	return false;
}

// The walk's next entry in range: its key, the flash address of its value and its length; -1 at the end. Under
// storage_lock
static int next_entry(codeplug_kv_cursor_t *c, uint16_t *key, uint32_t *value)
{
	uint32_t data;
	if (c->done || !walk_data(c, &data)) {
		return -1;
	}
	uint8_t e[ENTRY_HDR];
	for (;;) {
		if (c->at >= c->len) {
			if (!advance(c, &data)) {
				return -1;
			}
			continue;
		}
		if (!board.flash->read(data + c->at, e, ENTRY_HDR)) {
			return -1;
		}
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
		if (c->earlier != 0 && seen_earlier(c, k)) {
			continue;	// the key is in an earlier sector, which wins (2.4)
		}
		*key = k;
		*value = data + at + ENTRY_HDR;
		return n;
	}
}

int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max)
{
	uint32_t at;
	storage_lock();
	int found = next_entry(c, key, &at);
	if (found >= 0 && max != 0 && !board.flash->read(at, buf, found < max ? (uint16_t)found : max)) {
		found = -1;
	}
	c->done = found < 0;
	storage_unlock();
	return found;
}

static uint8_t kv_serial;	// codeplug_kv_serial(): one byte, only compared for a change

uint8_t codeplug_kv_serial(void)
{
	return (uint8_t)(kv_serial + codeplug_serial());	// both only go up: the sum moves when either does
}

int codeplug_kv_read(uint16_t key, uint16_t off, void *buf, uint16_t max)
{
	codeplug_kv_cursor_t c;
	uint16_t k;
	uint32_t at;
	codeplug_kv_begin(&c, key, key);
	storage_lock();
	int n = next_entry(&c, &k, &at);
	if (n >= 0 && max != 0 && off < n) {
		uint16_t cnt = (uint16_t)(n - off) < max ? (uint16_t)(n - off) : max;
		if (!board.flash->read(at + off, buf, cnt)) {
			n = -1;
		}
	}
	storage_unlock();
	return n;
}

int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	return codeplug_kv_read(key, 0, buf, max);
}

bool codeplug_kv_get_id(uint16_t key, uint32_t *id)
{
	uint8_t v[CODEPLUG_KV_SCALAR];
	return codeplug_kv_id(v, codeplug_kv_get(key, v, sizeof(v)), id);
}

codeplug_kv_state_t codeplug_kv_info(uint16_t *length, uint16_t *entries)
{
	uint32_t a;
	sector_t s;
	storage_lock();
	codeplug_kv_state_t st = area_addr(&a) ? check_flash(a, &s) : CODEPLUG_KV_EMPTY;
	storage_unlock();
	*length = st == CODEPLUG_KV_VALID ? s.len : 0;
	*entries = st == CODEPLUG_KV_VALID ? s.count : 0;
	return st;
}

bool codeplug_kv_keep(uint32_t addr, const uint8_t *sector)
{
	uint32_t a, c;
	sector_t s;
	// The area's sector and its continuation's, each judged for itself (docs/spec/codeplug-settings.md 6)
	if (!(area_addr(&a) && addr == a) && !(sector_addr(CODEPLUG_KV_CONT_ADDR, &c) && addr == c)) {
		return false;	// another sector, or a foreign codeplug's bytes: written as they come
	}
	storage_lock();
	codeplug_kv_state_t st = check_flash(addr, &s);
	storage_unlock();
	return st == CODEPLUG_KV_NEWER || (st == CODEPLUG_KV_VALID && codeplug_kv_check(sector) != CODEPLUG_KV_VALID);
}

bool codeplug_kv_commit(uint32_t addr, const uint8_t *sector)
{
	return codeplug_kv_keep(addr, sector) || storage_sector_write(addr, sector);
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

// A byte table op (CODEPLUG_KV_TABLE) on the entries d[0..*len): the bytes it names set, the value grown with 0xFF to
// reach them and cut of its trailing 0xFF after, an empty value removed. False if the growth doesn't fit in room
static bool apply_table(uint8_t *d, uint16_t *len, uint32_t room, const codeplug_kv_op_t *o, bool *changed)
{
	const uint8_t *v = o->value;
	uint32_t l = *len, at = 0, cnt = o->len & ~CODEPLUG_KV_TABLE, off = get16(v), n = 0;
	v += 2;
	while (at < l && get16(&d[at]) != o->key) {
		at += ENTRY_HDR + get16(&d[at + 2]);
	}
	bool have = at < l;
	if (have) {
		n = get16(&d[at + 2]);
	}
	bool diff = false;
	for (uint32_t i = 0; i < cnt; i++) {
		diff = diff || (off + i < n ? d[at + ENTRY_HDR + off + i] : 0xFFu) != v[i];
	}
	if (!diff) {
		return true;
	}
	uint32_t grown = off + cnt > n ? off + cnt : n;
	if (!have) {
		if (l + ENTRY_HDR + grown > room) {
			return false;
		}
		at = l;
		put16(&d[at], o->key);
		put16(&d[at + 2], 0);
		l += ENTRY_HDR;
	} else if (grown > n && l + (grown - n) > room) {
		return false;
	}
	if (grown > n) {
		uint32_t tail = at + ENTRY_HDR + n;
		memmove(&d[at + ENTRY_HDR + grown], &d[tail], l - tail);
		memset(&d[tail], 0xFF, grown - n);
		l += grown - n;
		put16(&d[at + 2], grown);
	}
	memcpy(&d[at + ENTRY_HDR + off], v, cnt);
	uint32_t keep = grown;
	while (keep > 0 && d[at + ENTRY_HDR + keep - 1] == 0xFFu) {
		keep--;
	}
	uint32_t after = at + ENTRY_HDR + grown;
	if (keep == 0) {
		l = cut(d, l, at, after);
	} else if (keep < grown) {
		memmove(&d[at + ENTRY_HDR + keep], &d[after], l - after);
		l -= grown - keep;
		put16(&d[at + 2], keep);
	}
	*len = (uint16_t)l;
	*changed = true;
	return true;
}

// One op on the entries d[0..*len): a set replaces the key's first entry in place or appends one (with
// CODEPLUG_KV_HEAD only its first bytes, the stored value's later ones kept), a remove takes out every entry of the
// key. *changed when a byte changed. False if a set doesn't fit in room
static bool apply(uint8_t *d, uint16_t *len, uint32_t room, const codeplug_kv_op_t *o, bool *changed)
{
	if (o->len != CODEPLUG_KV_REMOVE && (o->len & CODEPLUG_KV_TABLE) != 0) {
		return apply_table(d, len, room, o, changed);
	}
	uint32_t l = *len, at = 0;
	codeplug_kv_op_t set = *o;
	const codeplug_kv_op_t *op = &set;
	bool head = set.len != CODEPLUG_KV_REMOVE && (set.len & CODEPLUG_KV_HEAD) != 0;
	if (head) {
		set.len &= (uint16_t)~CODEPLUG_KV_HEAD;
	}
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
		if (head && n > op->len) {	// the fields this writer knows; the later ones stay
			if (op->len != 0 && memcmp(&d[at + ENTRY_HDR], op->value, op->len) != 0) {
				memcpy(&d[at + ENTRY_HDR], op->value, op->len);
				*changed = true;
			}
		} else if (n != op->len || (n != 0 && memcmp(&d[at + ENTRY_HDR], op->value, n) != 0)) {
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

// Apply the ops of the sector in b (a whole sector; cont: the continuation) with *changed, b is the new sector: later
// entries of any key dropped (no reader sees them), the rest 0xFF, the header and CRC made. A sector that isn't a valid
// area starts a new one. Ops for the other sector aren't applied; a set or remove of a key that belongs to the other
// sector also takes the key out here (in the continuation only when it changes anyway): a key present in two sectors
// is read from the earlier (2.4), so the copy a writer didn't set mustn't survive. link
// (not below 0) is the continuation's offset for header bytes 12-15, set when it differs. False if an op doesn't fit
// or the sector is a newer version
static bool edit(uint8_t *b, const codeplug_kv_op_t *ops, uint8_t n, bool cont, int32_t link, bool *changed)
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
		if (in_cont(ops[i].key) == cont && !apply(d, &len, room, &ops[i], changed)) {
			return false;
		}
	}
	for (uint8_t i = 0; i < n; i++) {
		// A key of the other sector (set or removed): a copy here must not stay. In the first sector it would be read
		// before the continuation's own, so it goes whether or not anything else changed; in the continuation it is only
		// a later copy no reader sees, and goes when the sector is written anyway
		if (in_cont(ops[i].key) != cont && (!cont || *changed)) {
			codeplug_kv_op_t drop = { ops[i].key, CODEPLUG_KV_REMOVE, NULL };
			apply(d, &len, room, &drop, changed);
		}
	}
	if (link >= 0 && get32(&b[12]) != (uint32_t)link) {
		put32(&b[12], (uint32_t)link);
		*changed = true;
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

// Edit the sector at flash address a in the shared buffer: fresh starts it from an erased one (what was there is
// dropped), write puts it through the journal when something changed (false: only whether the ops fit)
static bool sector_edit(uint32_t a, bool cont, const codeplug_kv_op_t *ops, uint8_t n, int32_t link, bool fresh, bool write,
			bool *changed)
{
	uint8_t *b = storage_buffer_acquire();
	bool ok = b != NULL;
	*changed = false;
	if (ok && fresh) {
		memset(b, 0xFF, STORAGE_SECTOR);
	} else if (ok) {
		ok = board.flash->read(a, b, STORAGE_SECTOR);
	}
	ok = ok && edit(b, ops, n, cont, link, changed) && (!write || !*changed || storage_sector_write(a, b));
	if (b != NULL) {
		storage_buffer_release();
	}
	return ok;
}

// The continuation to write (2.4): the one the first sector names when that is a valid area of ours, else the first
// one, 0xF000, started from an erased sector so that what an old unlinked continuation held doesn't come back. False
// when a sector there is of a later version (never written)
static bool continuation(uint32_t main_a, uint32_t *off, bool *fresh)
{
	sector_t s, t;
	uint32_t pa;
	*off = CODEPLUG_KV_CONT_ADDR;
	*fresh = true;
	codeplug_kv_state_t st = check_flash(main_a, &s);
	if (st == CODEPLUG_KV_NEWER) {
		return false;
	}
	if (st == CODEPLUG_KV_VALID && s.next != 0 && s.next != CODEPLUG_KV_ADDR && sector_addr(s.next, &pa)) {
		st = check_flash(pa, &t);
		if (st == CODEPLUG_KV_NEWER) {
			return false;
		}
		if (st == CODEPLUG_KV_VALID) {
			*off = s.next;
			*fresh = false;
			return true;
		}
	}
	return !(sector_addr(CODEPLUG_KV_CONT_ADDR, &pa) && check_flash(pa, &t) == CODEPLUG_KV_NEWER);
}

// The ops of a batch, sector by sector: the continuation first (created and linked on its first write), then the first
// sector. Under storage_lock
static bool write_sectors(uint32_t a, const codeplug_kv_op_t *ops, uint8_t n, bool main, bool cont, bool *changed)
{
	uint32_t off = 0, ca = 0;
	bool fresh = false, c_changed = false, m_changed = false;
	if (cont && (!continuation(a, &off, &fresh) || !sector_addr(off, &ca))) {
		return false;
	}
	// What doesn't fit stops the batch before anything is written
	if (main && cont && !sector_edit(a, false, ops, n, -1, false, false, &m_changed)) {
		return false;
	}
	if (cont && !sector_edit(ca, true, ops, n, -1, fresh, true, &c_changed)) {
		return false;
	}
	// A continuation just written is linked from the first sector, which keeps a link it has already
	bool link = c_changed;
	// The first sector is edited for a continuation batch too: a copy of a continuation key in it would be read first
	if ((main || cont) && !sector_edit(a, false, ops, n, link ? (int32_t)off : -1, false, true, &m_changed)) {
		return false;
	}
	*changed = c_changed || m_changed;
	return true;
}

// Whether an op is well formed: a key, a value when it has bytes, and a table op's offset and bytes inside a sector
static bool op_ok(const codeplug_kv_op_t *o)
{
	uint16_t len = o->len != CODEPLUG_KV_REMOVE ? (uint16_t)(o->len & ~CODEPLUG_KV_HEAD) : 0;
	if (o->key == 0 || o->key == 0xFFFFu || (len != 0 && o->value == NULL)) {
		return false;
	}
	if (o->len != CODEPLUG_KV_REMOVE && (o->len & CODEPLUG_KV_TABLE) != 0) {
		uint16_t cnt = o->len & ~CODEPLUG_KV_TABLE;
		return (o->len & CODEPLUG_KV_HEAD) == 0 && cnt != 0 && o->value != NULL &&
		       (uint32_t)get16(o->value) + cnt <= STORAGE_SECTOR - CODEPLUG_KV_HDR;
	}
	return true;
}

bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n)
{
	uint32_t a;
	bool main = false, cont = false;
	for (uint8_t i = 0; i < n; i++) {
		if (!op_ok(&ops[i])) {
			return false;
		}
		if (in_cont(ops[i].key)) {
			cont = true;
		} else {
			main = true;
		}
	}
	if (!area_addr(&a)) {
		return false;
	}
	storage_lock();
	bool changed = false;
	bool ok = write_sectors(a, ops, n, main, cont, &changed);
	if (ok && changed) {
		kv_serial++;	// the radio core takes the new values when it sees it move: no codeplug reload
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
