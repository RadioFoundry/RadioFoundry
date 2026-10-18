// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DMR ID database lookup (see dmr_iddb.h). Layout from docs/spec/open-firmware-cps.md 7.5: a 12-byte header
// ("Id", format, record size + 0x4A, version, LE record count) at the start of area 1, records sorted by ID, area 1
// holding as many whole records as fit in 256 KB with the header, the rest from the start of area 2.
#include <string.h>
#include "codeplug/dmr_iddb.h"
#include "codeplug/codeplug.h"
#include "board.h"

#if CONFIG_DMR

// Flash image addresses (7.5) and where the flash image sits in the data flash (the STM32 family's +128 KB, as in
// codeplug.c). TODO: per board, with the codeplug's offsets
#ifndef CODEPLUG_OPENFW_FLASH_BASE
#define CODEPLUG_OPENFW_FLASH_BASE	0x020000u
#endif
#define IDDB_AREA1		(CODEPLUG_OPENFW_FLASH_BASE + 0x30000u)
#define IDDB_AREA2		(CODEPLUG_OPENFW_FLASH_BASE + 0xB8000u)
#define IDDB_AREA2_VP		(CODEPLUG_OPENFW_FLASH_BASE + 0x8F400u)	// format 'n': in the voice prompt area
#define IDDB_AREA1_SIZE		0x40000u
#define IDDB_HEADER		12u
#define IDDB_RECORD_MAX		DMR_IDDB_RECORD_MAX

static struct {
	bool checked;
	uint32_t serial;	// codeplug_serial() when checked: a codeplug write (CPS, TFTP) re-reads the header
	bool valid;
	bool packed;		// formats 'N' / 'n': 3-byte LE IDs, 6-bit text
	uint8_t rec;		// record size
	uint32_t count;
	uint32_t per_area1;	// records in area 1
	uint32_t area2;
} db;

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash != NULL && board.flash->size != 0 && board.flash->read(addr, buf, len);
}

bool dmr_iddb_init(void)
{
	uint8_t h[IDDB_HEADER];

	memset(&db, 0, sizeof(db));
	db.checked = true;
	db.serial = codeplug_serial();
	if (!rd(IDDB_AREA1, h, sizeof(h)) || h[0] != 'I' || h[1] != 'd' || h[3] < 0x4Au) {
		return false;
	}
	db.packed = h[2] == 'N' || h[2] == 'n';
	db.rec = (uint8_t)(h[3] - 0x4Au);
	db.count = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
	uint8_t id_len = db.packed ? 3u : 4u;
	if (db.rec <= id_len || db.rec > IDDB_RECORD_MAX || db.count == 0 || db.count > 0x400000u) {
		db.count = 0;
		return false;
	}
	db.per_area1 = (IDDB_AREA1_SIZE - IDDB_HEADER) / db.rec;
	db.area2 = h[2] == 'n' ? IDDB_AREA2_VP : IDDB_AREA2;
	db.valid = true;
	return true;
}

uint32_t dmr_iddb_count(void)
{
	if (!db.checked || db.serial != codeplug_serial()) {
		dmr_iddb_init();
	}
	return db.valid ? db.count : 0;
}

static uint32_t rec_addr(uint32_t i)
{
	return i < db.per_area1 ? IDDB_AREA1 + IDDB_HEADER + i * db.rec : db.area2 + (i - db.per_area1) * db.rec;
}

// The ID of a record: 3 bytes LE binary, or 8 BCD digits (stored as a LE word, like the other LE fields of the
// database; the interop spec doesn't say). 0 if malformed
static uint32_t rec_id(const uint8_t *r)
{
	if (db.packed) {
		return (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16);
	}
	uint32_t w = (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
	uint32_t v = 0;
	for (int i = 7; i >= 0; i--) {
		uint32_t d = (w >> (4 * i)) & 0xFu;
		if (d > 9u) {
			return 0;
		}
		v = v * 10u + d;
	}
	return v;
}

// 6-bit packed text: 4 characters in 3 bytes, most significant first, alphabet space 0-9 A-Z a-z .
static char unpack6(uint8_t c)
{
	if (c == 0) {
		return ' ';
	}
	if (c <= 10) {
		return (char)('0' + c - 1);
	}
	if (c <= 36) {
		return (char)('A' + c - 11);
	}
	if (c <= 62) {
		return (char)('a' + c - 37);
	}
	return '.';
}

static void rec_text(const uint8_t *r, char *out, size_t len)
{
	size_t n = 0;

	if (db.packed) {
		const uint8_t *t = r + 3;
		unsigned chars = (db.rec - 3u) * 4u / 3u;
		for (unsigned j = 0; j < chars && n + 1 < len; j++) {
			unsigned bit = 6u * j;
			uint16_t w = (uint16_t)((t[bit / 8u] << 8) | (bit / 8u + 1u < db.rec - 3u ? t[bit / 8u + 1u] : 0u));
			out[n++] = unpack6((uint8_t)((w >> (10u - bit % 8u)) & 0x3Fu));
		}
	} else {
		for (unsigned i = 4; i < db.rec && n + 1 < len; i++) {
			uint8_t c = r[i];
			if (c == 0 || c == 0xFF) {
				break;
			}
			out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
		}
	}
	while (n > 0 && out[n - 1] == ' ') {
		n--;
	}
	out[n] = '\0';
}

bool dmr_iddb_lookup(uint32_t id, char *out, size_t len)
{
	uint8_t r[IDDB_RECORD_MAX];
	uint32_t lo = 0, hi;

	if (len == 0 || id == 0 || dmr_iddb_count() == 0) {
		return false;
	}
	hi = db.count;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (!rd(rec_addr(mid), r, db.rec)) {
			return false;
		}
		uint32_t rid = rec_id(r);
		if (rid == id) {
			rec_text(r, out, len);
			return true;
		}
		if (rid < id) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}
	return false;
}

#endif
