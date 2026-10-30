// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR ID database lookup (see dmr_iddb.h). Layout from docs/spec/open-firmware-cps.md 7.5: a 12-byte header
// ("Id", format, record size + 0x4A, version, LE record count) at the start of area 1, records sorted by ID, area 1
// holding as many whole records as fit in 256 KB with the header, the rest from the start of area 2. With
// CONFIG_IDDB_COMPACT also format C (docs/spec/iddb-compact.md): pages of records found through an index, names coded
// through a dictionary.
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
#if CONFIG_IDDB_COMPACT
	bool compact;		// format C: the fields below
	uint16_t pages;		// index entries
	uint16_t dict;		// dictionary entries
	uint32_t size;		// database offsets: where it ends, the index, the dictionary's table and strings
	uint32_t index;
	uint32_t table;
	uint32_t strings;
#endif
} db;

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash != NULL && board.flash->size != 0 && board.flash->read(addr, buf, len);
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

#if CONFIG_IDDB_COMPACT
// --- Format C (iddb-compact.md) ---
#define C_HEADER	32u
#define C_PAGE		4096u
#define C_DICT_MAX	16512u
#define C_TEXT_MAX	48u
#define C_EMPTY		0xFFFFu

static uint32_t le(const uint8_t *b, unsigned n)
{
	uint32_t v = 0;
	while (n-- > 0u) {
		v = (v << 8) | b[n];
	}
	return v;
}

// A database offset's flash address: area 1, then area 2
static uint32_t c_addr(uint32_t off)
{
	return off < IDDB_AREA1_SIZE ? IDDB_AREA1 + off : IDDB_AREA2 + (off - IDDB_AREA1_SIZE);
}

static bool c_read(uint32_t off, uint8_t *buf, uint32_t len)
{
	while (len > 0u) {
		uint32_t n = off < IDDB_AREA1_SIZE && off + len > IDDB_AREA1_SIZE ? IDDB_AREA1_SIZE - off : len;
		if (!rd(c_addr(off), buf, n)) {
			return false;
		}
		off += n;
		buf += n;
		len -= n;
	}
	return true;
}

static bool compact_init(void)
{
	uint8_t h[C_HEADER];
	if (!rd(IDDB_AREA1, h, sizeof(h)) || h[4] != 1u) {
		return false;
	}
	uint32_t count = le(h + 8, 4), size = le(h + 12, 4), index = le(h + 16, 4), table = le(h + 24, 4);
	uint32_t strings = le(h + 28, 4), pages = le(h + 20, 2), dict = le(h + 22, 2);
	uint32_t space = board.flash->size > IDDB_AREA2 ? IDDB_AREA1_SIZE + (board.flash->size - IDDB_AREA2) : IDDB_AREA1_SIZE;
	// each section's length against the room after its offset, so no sum can wrap
	if (count == 0u || count > 0x400000u || pages == 0u || dict > C_DICT_MAX || size > space || size % C_PAGE != 0u ||
		index < C_PAGE || index > size || 5u * pages > size - index || table < C_PAGE || table > size ||
		3u * dict > size - table || strings < C_PAGE || strings > size) {
		return false;
	}
	db.compact = true;
	db.count = count;
	db.size = size;
	db.index = index;
	db.pages = (uint16_t)pages;
	db.table = table;
	db.dict = (uint16_t)dict;
	db.strings = strings;
	db.valid = true;
	return true;
}

// Bytes of the database read in order through a small window: the only buffer a lookup has (the radio task's stack
// is tight)
typedef struct {
	uint32_t off;		// offset of buf[0]
	uint32_t end;		// nothing at or past this is read
	uint8_t pos, n;
	uint8_t buf[32];
} reader_t;

static void seek(reader_t *r, uint32_t off, uint32_t end)
{
	r->off = off;
	r->end = end;
	r->pos = r->n = 0;
}

static int next(reader_t *r)
{
	if (r->pos == r->n) {
		r->off += r->n;
		uint32_t n = r->end > r->off ? r->end - r->off : 0u;
		n = n < sizeof(r->buf) ? n : sizeof(r->buf);
		if (n == 0u || !c_read(r->off, r->buf, n)) {
			return -1;
		}
		r->n = (uint8_t)n;
		r->pos = 0;
	}
	return r->buf[r->pos++];
}

// A token (3.4): a dictionary code, or a literal's length and offset (its bytes are skipped)
typedef struct {
	uint32_t at;
	uint16_t code;
	uint8_t chars;
} token_t;

static bool token(reader_t *r, token_t *t)
{
	int b = next(r);
	t->code = C_EMPTY;
	t->chars = 0;
	if (b < 0 || b > 0xF0) {
		return false;
	}
	if (b < 0x80) {
		t->code = (uint16_t)b;
	} else if (b < 0xC0) {
		int c = next(r);
		if (c < 0) {
			return false;
		}
		t->code = (uint16_t)(128u + (((unsigned)b - 0x80u) << 8) + (unsigned)c);
	} else if (b > 0xC0) {
		t->chars = (uint8_t)(b - 0xC0);
		t->at = r->off + r->pos;
		for (unsigned n = (6u * t->chars + 7u) / 8u; n > 0u; n--) {
			if (next(r) < 0) {
				return false;
			}
		}
	}
	return true;
}

// chars 6-bit characters from the reader onto out (n so far, cut at len - 1), a space before them unless first
static bool unpack(reader_t *r, unsigned chars, char *out, size_t len, size_t *n)
{
	uint32_t acc = 0;
	unsigned bits = 0;
	if (*n > 0u && *n + 1u < len) {
		out[(*n)++] = ' ';
	}
	for (unsigned j = 0; j < chars; j++) {
		while (bits < 6u) {
			int b = next(r);
			if (b < 0) {
				return false;
			}
			acc = ((acc << 8) | (uint32_t)b) & 0xFFFFu;
			bits += 8u;
		}
		bits -= 6u;
		if (*n + 1u < len) {
			out[(*n)++] = unpack6((uint8_t)((acc >> bits) & 0x3Fu));
		}
	}
	return true;
}

static bool compact_text(reader_t *r, const token_t *t, char *out, size_t len)
{
	size_t n = 0;
	for (unsigned k = 0; k < 3u; k++) {
		unsigned chars = t[k].chars;
		if (chars != 0u) {
			seek(r, t[k].at, db.size);
		} else if (t[k].code != C_EMPTY) {
			uint8_t e[3];
			if (t[k].code >= db.dict || !c_read(db.table + 3u * t[k].code, e, sizeof(e))) {
				return false;
			}
			seek(r, db.strings + le(e, 3), db.size);
			int c = next(r);
			if (c <= 0 || c > (int)C_TEXT_MAX) {
				return false;
			}
			chars = (unsigned)c;
		}
		if (chars != 0u && !unpack(r, chars, out, len, &n)) {
			return false;
		}
	}
	while (n > 0u && out[n - 1u] == ' ') {
		n--;
	}
	out[n] = '\0';
	return true;
}

static bool compact_lookup(uint32_t id, char *out, size_t len)
{
	reader_t r;
	uint8_t e[5];
	uint32_t lo = 0, hi = db.pages;

	// the last page whose first ID is at most id
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2u;
		if (!c_read(db.index + 5u * mid, e, sizeof(e))) {
			return false;
		}
		if (le(e, 3) <= id) {
			lo = mid + 1u;
		} else {
			hi = mid;
		}
	}
	if (lo == 0u || !c_read(db.index + 5u * (lo - 1u), e, sizeof(e))) {
		return false;
	}
	uint32_t at = le(e + 3, 2) * C_PAGE;
	if (at < C_PAGE || at + C_PAGE > db.size) {
		return false;
	}
	seek(&r, at, at + C_PAGE);
	int c0 = next(&r), c1 = next(&r);
	if (c0 < 0 || c1 < 0) {
		return false;
	}
	uint32_t count = (uint32_t)c0 | ((uint32_t)c1 << 8), rid = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t v = 0;
		int b;
		unsigned shift = 0;
		do {
			b = next(&r);
			if (b < 0 || shift > 21u) {
				return false;
			}
			v |= (uint32_t)(b & 0x7F) << shift;
			shift += 7u;
		} while (b & 0x80);
		rid = i == 0u ? v : rid + v;
		token_t t[3];
		for (unsigned k = 0; k < 3u; k++) {
			if (!token(&r, &t[k])) {
				return false;
			}
		}
		if (rid == id) {
			return compact_text(&r, t, out, len);
		}
		if (rid > id) {
			return false;
		}
	}
	return false;
}
#endif

bool dmr_iddb_init(void)
{
	uint8_t h[IDDB_HEADER];

	memset(&db, 0, sizeof(db));
	db.checked = true;
	db.serial = codeplug_serial();
	if (!rd(IDDB_AREA1, h, sizeof(h)) || h[0] != 'I' || h[1] != 'd') {
		return false;
	}
#if CONFIG_IDDB_COMPACT
	if (h[2] == 'C' && h[3] == 0u) {
		return compact_init();
	}
#endif
	if (h[3] < 0x4Au) {	// format C to a build without it: no database (iddb-compact.md 3.1)
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
#if CONFIG_IDDB_COMPACT
	if (db.compact) {
		return compact_lookup(id, out, len);
	}
#endif
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
