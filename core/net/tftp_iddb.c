// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR ID database over TFTP (tftp_iddb.h, docs/spec/tftp.md 5).
//
// A database is too big for the staging area (megabytes against 256 KB), and doesn't need it: a broken one only
// costs names on the receive screen. So it is written in place, in an order that never shows a broken one: the
// sector holding the header is erased first (the radio then has no database), the records are programmed as they
// arrive, each read back, and the header is programmed last into its still erased bytes. Nothing here goes through
// the sector journal: the areas hold nothing but the database.
#include <string.h>
#include "net/tftp_iddb.h"
#include "net/tftp_codeplug.h"
#include "board.h"
#include "storage/storage.h"
#include "codeplug/codeplug.h"
#include "codeplug/dmr_iddb.h"

#define SECTOR		STORAGE_SECTOR
#define FL_IMAGE	0x20000u	// image address of the flash image's start
#define AREA2_VP	(FL_IMAGE + 0x8F400u)	// format 'n': area 2 in the voice prompt area (read only here)
#define COUNT_MAX	0x400000u	// as dmr_iddb.c

static struct {
	bool on;			// a database is being written
	uint8_t h[TFTP_IDDB_HEADER];
	uint32_t rec, count, per1;	// record size, records, records in area 1
	uint32_t done;			// record bytes written
	uint32_t erased;		// physical address up to which the sectors being written are erased
} d;

// iddb.bin's own transfer: the header as it comes in, the size announced
static struct {
	bool open;
	uint8_t h[TFTP_IDDB_HEADER];
	uint32_t tsize;
} f;

static const hal_flash_map_t *map(void)
{
	return storage_map();
}

static uint32_t phys(uint32_t img)
{
	return map()->openfw_flash + (img - FL_IMAGE);
}

uint32_t tftp_iddb_image_end(void)
{
	const hal_flash_map_t *m = map();
	return m == NULL ? 0 : m->openfw_end - m->openfw_flash + FL_IMAGE;
}

// Image address of record byte s (the records back to back, as in iddb.bin after the header)
static uint32_t rec_image(uint32_t s, uint32_t rec, uint32_t per1, uint32_t area2)
{
	return s < per1 * rec ? TFTP_IDDB_AREA1 + TFTP_IDDB_HEADER + s : area2 + (s - per1 * rec);
}

static bool fl_read(uint32_t addr, void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->read(addr, buf, len);
	storage_unlock();
	return ok;
}

static bool fl_erase(uint32_t addr)
{
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

// Program and read back
static bool fl_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->write(addr, buf, len);
	storage_unlock();
	uint8_t b[64];
	for (uint32_t off = 0; ok && off < len; off += sizeof(b)) {
		uint32_t n = len - off < sizeof(b) ? len - off : sizeof(b);
		ok = fl_read(addr + off, b, n) && memcmp(b, buf + off, n) == 0;
	}
	return ok;
}

// The header's record size and count, if the radio reads it (dmr_iddb.c's rules); format 'n' only when reading
static bool parse(const uint8_t *h, bool allow_n, uint32_t *rec, uint32_t *count)
{
	if (h[0] != 'I' || h[1] != 'd' || h[3] < 0x4Au || (h[2] == 'n' && !allow_n)) {
		return false;
	}
	bool packed = h[2] == 'N' || h[2] == 'n';
	*rec = h[3] - 0x4Au;
	*count = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
	return *rec > (packed ? 3u : 4u) && *rec <= DMR_IDDB_RECORD_MAX && *count <= COUNT_MAX;
}

bool tftp_iddb_header_ok(const uint8_t *h, uint32_t *end, const char **msg)
{
	uint32_t rec, count;
	if (!parse(h, false, &rec, &count)) {
		*msg = h[2] == 'n' ? "format n (area 2 in the voice prompts) isn't taken" : "not a DMR ID database header";
		return false;
	}
	uint32_t per1 = (TFTP_IDDB_AREA1_SIZE - TFTP_IDDB_HEADER) / rec;
	*end = count <= per1 ? TFTP_IDDB_AREA1 + TFTP_IDDB_HEADER + count * rec : TFTP_IDDB_AREA2 + (count - per1) * rec;
	if (count > per1 && (tftp_iddb_image_end() <= TFTP_IDDB_AREA2 || *end > tftp_iddb_image_end())) {
		*msg = "the ID database doesn't fit in this radio";
		return false;
	}
	return true;
}

static void invalidate(void)
{
#if CONFIG_DMR
	dmr_iddb_init();
#endif
}

int tftp_iddb_begin(const uint8_t *h, const char **msg)
{
	uint32_t end;
	memset(&d, 0, sizeof(d));
	if (map() == NULL || board.flash == NULL || !tftp_iddb_header_ok(h, &end, msg)) {
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	memcpy(d.h, h, sizeof(d.h));
	parse(h, false, &d.rec, &d.count);
	d.per1 = (TFTP_IDDB_AREA1_SIZE - TFTP_IDDB_HEADER) / d.rec;
	if (!fl_erase(phys(TFTP_IDDB_AREA1))) {
		*msg = "flash erase failed";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	invalidate();		// no database from here until the header is back
	d.erased = phys(TFTP_IDDB_AREA1) + SECTOR;
	d.on = true;
	return TFTP_OK;
}

// Record bytes at image address img (inside one area), in order
static int put_at(uint32_t img, const uint8_t *buf, uint32_t len, const char **msg)
{
	uint32_t p = phys(img);
	while (d.erased < p + len) {	// erase the sectors as the data reaches them; the jump to area 2 skips the gap
		if (d.erased < (p & ~(SECTOR - 1u))) {
			d.erased = p & ~(SECTOR - 1u);
		}
		if (!fl_erase(d.erased)) {
			*msg = "flash erase failed";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		d.erased += SECTOR;
	}
	if (!fl_program(p, buf, len)) {
		*msg = "flash write failed";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	d.done += len;
	return TFTP_OK;
}

// Record bytes s.. of the stream (the records back to back), split where area 1 ends
static int put_stream(uint32_t s, const uint8_t *buf, uint32_t len, const char **msg)
{
	uint32_t a1 = d.per1 * d.rec;
	if (s != d.done || s + len > d.count * d.rec) {
		*msg = "longer than its header says";
		return TFTP_FAIL(TFTP_ERR_FULL);
	}
	while (len > 0) {
		uint32_t n = s < a1 && s + len > a1 ? a1 - s : len;
		int r = put_at(rec_image(s, d.rec, d.per1, TFTP_IDDB_AREA2), buf, n, msg);
		if (r != TFTP_OK) {
			return r;
		}
		s += n;
		buf += n;
		len -= n;
	}
	return TFTP_OK;
}

int tftp_iddb_image(uint32_t off, const uint8_t *buf, uint32_t len, const char **msg)
{
	if (!d.on) {
		return TFTP_OK;
	}
	// The two ranges the records take in the image, and where each starts in the stream
	const uint32_t total = d.count * d.rec, a1 = d.per1 * d.rec < total ? d.per1 * d.rec : total;
	const struct {
		uint32_t img, s, n;
	} part[2] = {
		{ TFTP_IDDB_AREA1 + TFTP_IDDB_HEADER, 0, a1 },
		{ TFTP_IDDB_AREA2, a1, total - a1 },
	};
	for (unsigned i = 0; i < 2; i++) {
		uint32_t lo = off > part[i].img ? off : part[i].img;
		uint32_t hi = off + len < part[i].img + part[i].n ? off + len : part[i].img + part[i].n;
		if (lo < hi) {
			int r = put_stream(part[i].s + (lo - part[i].img), buf + (lo - off), hi - lo, msg);
			if (r != TFTP_OK) {
				return r;
			}
		}
	}
	return TFTP_OK;
}

int tftp_iddb_finish(const char **msg)
{
	if (!d.on) {
		return TFTP_OK;
	}
	d.on = false;
	if (d.done != d.count * d.rec) {
		*msg = "the ID database ends early";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	if (!fl_program(phys(TFTP_IDDB_AREA1), d.h, sizeof(d.h))) {
		*msg = "flash write failed";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	invalidate();
	codeplug_reload();	// its serial makes the radio drop the names it cached
	return TFTP_OK;
}

void tftp_iddb_cancel(void)
{
	d.on = false;
}

// ---- iddb.bin ----

// The stored database: record size, count and where area 2 is; false if there is none
static bool stored(uint32_t *rec, uint32_t *count, uint32_t *area2)
{
	uint8_t h[TFTP_IDDB_HEADER];
	if (map() == NULL || board.flash == NULL || !fl_read(phys(TFTP_IDDB_AREA1), h, sizeof(h)) ||
	    !parse(h, true, rec, count) || *count == 0) {
		return false;
	}
	*area2 = h[2] == 'n' ? AREA2_VP : TFTP_IDDB_AREA2;
	uint32_t per1 = (TFTP_IDDB_AREA1_SIZE - TFTP_IDDB_HEADER) / *rec;
	return *count <= per1 || rec_image(*count * *rec - 1u, *rec, per1, *area2) < tftp_iddb_image_end();
}

static bool open_read(uint32_t *size)
{
	uint32_t rec, count, area2;
	if (!stored(&rec, &count, &area2)) {
		return false;
	}
	*size = TFTP_IDDB_HEADER + count * rec;
	return true;
}

static bool read_file(uint32_t off, uint8_t *buf, uint16_t len)
{
	uint32_t rec, count, area2;
	if (!stored(&rec, &count, &area2)) {
		return false;
	}
	uint32_t per1 = (TFTP_IDDB_AREA1_SIZE - TFTP_IDDB_HEADER) / rec, a1 = per1 * rec;
	while (len > 0) {
		uint32_t n, img;
		if (off < TFTP_IDDB_HEADER) {
			img = TFTP_IDDB_AREA1 + off;
			n = TFTP_IDDB_HEADER - off;
		} else {
			uint32_t s = off - TFTP_IDDB_HEADER;
			img = rec_image(s, rec, per1, area2);
			n = s < a1 ? a1 - s : len;
		}
		n = n < len ? n : len;
		if (!fl_read(phys(img), buf, n)) {
			return false;
		}
		off += n;
		buf += n;
		len = (uint16_t)(len - n);
	}
	return true;
}

static int open_write(uint32_t tsize, const char **msg)
{
	if (!tftp_codeplug_write_allowed()) {
		*msg = "codeplug writes are off (Settings > Connections)";
		return TFTP_FAIL(TFTP_ERR_ACCESS);
	}
	if (!storage_ready() || map() == NULL) {
		*msg = "the radio's flash isn't writable (foreign settings region?)";
		return TFTP_FAIL(TFTP_ERR_ACCESS);
	}
	if (tsize != 0 && tsize < TFTP_IDDB_HEADER) {
		*msg = "iddb.bin starts with a 12-byte header";
		return TFTP_FAIL(TFTP_ERR_FULL);
	}
	memset(&f, 0, sizeof(f));
	d.on = false;
	f.tsize = tsize;
	f.open = true;
	return TFTP_OK;
}

static int write_file(uint32_t off, const uint8_t *buf, uint16_t len, const char **msg)
{
	if (!f.open) {
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	if (off < TFTP_IDDB_HEADER) {	// the header, which may span blocks at the smallest blksize
		uint32_t n = TFTP_IDDB_HEADER - off < len ? TFTP_IDDB_HEADER - off : len;
		memcpy(f.h + off, buf, n);
		off += n;
		buf += n;
		len = (uint16_t)(len - n);
		if (off == TFTP_IDDB_HEADER) {
			uint32_t end;
			if (!tftp_iddb_header_ok(f.h, &end, msg)) {
				return TFTP_FAIL(TFTP_ERR_OTHER);
			}
			uint32_t rec, count;
			parse(f.h, false, &rec, &count);
			if (f.tsize != 0 && f.tsize != TFTP_IDDB_HEADER + count * rec) {
				*msg = "the size isn't what the header's record count gives";
				return TFTP_FAIL(TFTP_ERR_FULL);
			}
			int r = tftp_iddb_begin(f.h, msg);
			if (r != TFTP_OK) {
				return r;
			}
		}
	}
	return len == 0 ? TFTP_OK : put_stream(off - TFTP_IDDB_HEADER, buf, len, msg);
}

static int commit(uint32_t size, const char **msg)
{
	f.open = false;
	if (size < TFTP_IDDB_HEADER || !d.on || size != TFTP_IDDB_HEADER + d.count * d.rec) {
		d.on = false;
		*msg = "iddb.bin is shorter than its header says";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	return tftp_iddb_finish(msg);
}

static void abort_write(void)
{
	f.open = false;
	d.on = false;		// the header stays erased: no database
}

const tftp_file_t tftp_iddb_file = {
	.name = "iddb.bin",
	.open_read = open_read,
	.read = read_file,
	.open_write = open_write,
	.write = write_file,
	.commit = commit,
	.abort = abort_write,
};
