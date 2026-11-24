// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (scan_list.h)
#include <string.h>
#include "codeplug/scan_list.h"
#include "config.h"
#include "storage/storage.h"

#if CONFIG_SCAN_LIST

#define SLOTS_PER_SECTOR	(STORAGE_SECTOR / SCAN_LIST_RECORD)
#define CRC_AT			(SCAN_LIST_RECORD - 4u)

_Static_assert(STORAGE_SECTOR % SCAN_LIST_RECORD == 0, "a sector holds whole records");
_Static_assert(CODEPLUG_NAME_LEN == 16, "the record's name field is 16 bytes");

// Record offsets (docs/spec/scan-list.md 3)
enum {
	OFF_MAGIC = 0, OFF_LAYOUT = 4, OFF_FLAGS = 5, OFF_COUNT = 6, OFF_NAME = 8, OFF_CHANNELS = 24,
};

static volatile uint32_t generation;

uint32_t scan_list_generation(void)
{
	return generation;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (uint32_t)get16(p + 2) << 16;
}

// A list the store accepts: a name, at most SCAN_LIST_MAX channels, each a channel number (1 and up) once
static bool valid(const scan_list_t *l)
{
	if (l->name[0] == '\0' || l->count > SCAN_LIST_MAX)
		return false;
	for (uint16_t i = 0; i < l->count; i++) {
		if (l->channels[i] == 0)
			return false;
		for (uint16_t j = 0; j < i; j++)
			if (l->channels[j] == l->channels[i])
				return false;
	}
	return true;
}

bool scan_list_encode(const scan_list_t *l, uint8_t out[SCAN_LIST_RECORD])
{
	if (!valid(l))
		return false;
	memset(out, 0, SCAN_LIST_RECORD);
	put32(out + OFF_MAGIC, SCAN_LIST_MAGIC);
	out[OFF_LAYOUT] = SCAN_LIST_LAYOUT;
	put16(out + OFF_COUNT, l->count);
	memcpy(out + OFF_NAME, l->name, CODEPLUG_NAME_LEN);
	for (uint16_t i = 0; i < l->count; i++)
		put16(out + OFF_CHANNELS + 2u * i, l->channels[i]);
	put32(out + CRC_AT, storage_crc32(0, out, CRC_AT));
	return true;
}

bool scan_list_decode(const uint8_t in[SCAN_LIST_RECORD], scan_list_t *out)
{
	if (get32(in + OFF_MAGIC) != SCAN_LIST_MAGIC || in[OFF_LAYOUT] != SCAN_LIST_LAYOUT ||
	    get32(in + CRC_AT) != storage_crc32(0, in, CRC_AT) || get16(in + OFF_COUNT) > SCAN_LIST_MAX)
		return false;
	scan_list_t l;
	memset(&l, 0, sizeof l);
	memcpy(l.name, in + OFF_NAME, CODEPLUG_NAME_LEN);
	l.count = get16(in + OFF_COUNT);
	for (uint16_t i = 0; i < l.count; i++)
		l.channels[i] = get16(in + OFF_CHANNELS + 2u * i);
	if (!valid(&l))
		return false;
	*out = l;
	return true;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// The area when the board has a usable one: aligned whole sectors on the flash, clear of every other area, and the
// storage region ours
static bool area(uint32_t *addr, uint32_t *size)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || !storage_ready() || m->scan_size == 0 || m->scan_addr % STORAGE_SECTOR != 0 ||
	    m->scan_size % STORAGE_SECTOR != 0 || m->scan_addr > board.flash->size ||
	    m->scan_size > board.flash->size - m->scan_addr)
		return false;
	uint32_t a = m->scan_addr, n = m->scan_size;
	if (overlaps(a, n, m->settings_addr, m->settings_size) || overlaps(a, n, m->journal_addr, 2u * STORAGE_SECTOR) ||
	    overlaps(a, n, m->openfw_eeprom, 0x10000u) || overlaps(a, n, m->openfw_flash, m->openfw_end - m->openfw_flash) ||
	    overlaps(a, n, m->native_addr, m->native_size) || overlaps(a, n, m->calibration_addr, m->calibration_size) ||
	    overlaps(a, n, m->staging_addr, m->staging_size) || overlaps(a, n, m->satellite_addr, m->satellite_size))
		return false;
	*addr = a;
	*size = n;
	return true;
}

uint16_t scan_list_slots(void)
{
	uint32_t a, n;
	return area(&a, &n) ? (uint16_t)(n / SCAN_LIST_RECORD) : 0u;
}

typedef enum { SLOT_FREE, SLOT_VALID, SLOT_OTHER, SLOT_UNREADABLE } slot_t;

#define PIECE	32u

// A slot's kind. Read in pieces with the CRC run along, so the scans (state, count, find a free slot) need no record
// on the stack: the UI and the housekeeping task run them with little to spare. A record whose frame is right (magic,
// layout, count, CRC) is VALID; scan_list_read then also checks the list itself
static slot_t slot_kind(uint16_t i)
{
	uint32_t a, n;
	uint8_t b[PIECE];
	if (!area(&a, &n) || i >= n / SCAN_LIST_RECORD)
		return SLOT_UNREADABLE;
	uint32_t at = a + (uint32_t)i * SCAN_LIST_RECORD, crc = 0;
	bool blank = true, head = false;
	storage_lock();
	for (uint32_t off = 0; off < SCAN_LIST_RECORD; off += PIECE) {
		if (!board.flash->read(at + off, b, PIECE)) {
			storage_unlock();
			return SLOT_UNREADABLE;
		}
		if (off == 0)
			head = get32(b + OFF_MAGIC) == SCAN_LIST_MAGIC && b[OFF_LAYOUT] == SCAN_LIST_LAYOUT &&
			       get16(b + OFF_COUNT) <= SCAN_LIST_MAX;
		for (unsigned k = 0; k < PIECE; k++)
			blank = blank && b[k] == 0xFFu;
		bool last = off + PIECE == SCAN_LIST_RECORD;
		crc = storage_crc32(crc, b, last ? PIECE - 4u : PIECE);
		if (last && head && get32(b + PIECE - 4u) == crc) {
			storage_unlock();
			return SLOT_VALID;
		}
	}
	storage_unlock();
	return blank ? SLOT_FREE : SLOT_OTHER;
}

bool scan_list_read(uint16_t slot, scan_list_t *out)
{
	uint32_t a, n;
	uint8_t b[SCAN_LIST_RECORD];
	if (!area(&a, &n) || slot >= n / SCAN_LIST_RECORD)
		return false;
	storage_lock();
	bool ok = board.flash->read(a + (uint32_t)slot * SCAN_LIST_RECORD, b, sizeof b);
	storage_unlock();
	return ok && scan_list_decode(b, out);
}

scan_list_state_t scan_list_state(void)
{
	uint16_t n = scan_list_slots();
	if (n == 0)
		return SCAN_LIST_NONE;
	for (uint16_t i = 0; i < n; i++) {
		slot_t s = slot_kind(i);
		if (s == SLOT_UNREADABLE)
			return SCAN_LIST_NONE;
		if (s == SLOT_OTHER)
			return SCAN_LIST_FOREIGN;
	}
	return SCAN_LIST_OURS;
}

uint16_t scan_list_count(void)
{
	uint16_t n = scan_list_slots(), c = 0;
	for (uint16_t i = 0; i < n; i++)
		c += slot_kind(i) == SLOT_VALID;
	return c;
}

// Rewrite slot i with rec (NULL: erase it to free) through the journal
static bool slot_write(uint16_t i, const uint8_t *rec)
{
	uint32_t a, n;
	if (!area(&a, &n) || i >= n / SCAN_LIST_RECORD)
		return false;
	uint32_t sector = a + (uint32_t)(i / SLOTS_PER_SECTOR) * STORAGE_SECTOR;
	uint32_t off = (uint32_t)(i % SLOTS_PER_SECTOR) * SCAN_LIST_RECORD;
	storage_lock();
	uint8_t *buf = storage_buffer_acquire();
	bool ok = buf != NULL && board.flash->read(sector, buf, STORAGE_SECTOR);
	if (ok) {
		if (rec != NULL)
			memcpy(buf + off, rec, SCAN_LIST_RECORD);
		else
			memset(buf + off, 0xFF, SCAN_LIST_RECORD);
		ok = storage_sector_write(sector, buf);
	}
	generation++;		// even a failed write may have changed the sector
	if (buf != NULL)
		storage_buffer_release();
	storage_unlock();
	return ok;
}

bool scan_list_write(uint16_t slot, const scan_list_t *l)
{
	uint8_t rec[SCAN_LIST_RECORD];
	if (slot >= scan_list_slots() || !scan_list_encode(l, rec) || scan_list_state() != SCAN_LIST_OURS)
		return false;
	return slot_write(slot, rec);
}

int scan_list_add(const scan_list_t *l)
{
	uint8_t rec[SCAN_LIST_RECORD];
	if (!scan_list_encode(l, rec) || scan_list_state() != SCAN_LIST_OURS)
		return -1;
	uint16_t n = scan_list_slots();
	for (uint16_t i = 0; i < n; i++)
		if (slot_kind(i) == SLOT_FREE)
			return slot_write(i, rec) ? (int)i : -1;
	return -1;
}

bool scan_list_delete(uint16_t slot)
{
	return slot_kind(slot) == SLOT_VALID && scan_list_state() == SCAN_LIST_OURS && slot_write(slot, NULL);
}

bool scan_list_clear(void)
{
	uint32_t a, n;
	if (!area(&a, &n))
		return false;
	storage_lock();
	bool ok = storage_write(a, NULL, n);
	generation++;
	storage_unlock();
	return ok;
}

#endif // CONFIG_SCAN_LIST
