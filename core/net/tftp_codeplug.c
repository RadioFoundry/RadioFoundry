// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// codeplug.bin over TFTP (tftp_codeplug.h, docs/spec/tftp.md 3-4).
//
// The staging area (hal_flash_map_t.staging_*): sector 0 is the commit record, written only once the whole image is
// in and checked; the staged bytes (the regions below, back to back) follow from sector 1. A commit copies them into
// the stored codeplug one 4 KB sector per tick, each through the sector journal (storage_sector_write), then erases
// the record. A power cut before the record leaves the old codeplug, after it the next boot finishes the copy
// (tftp_codeplug_resume), so a cut transfer or a cut commit never leaves half of each.
#include <string.h>
#include "net/tftp_codeplug.h"
#include "board.h"
#include "storage/storage.h"
#include "codeplug/codeplug.h"

#define SECTOR		STORAGE_SECTOR
#define EE_IMAGE	0x10000u	// file offsets below this are the EEPROM image, from 0x20000 the flash image
#define FL_IMAGE	0x20000u
#define QUICK_KEYS	0x7524u		// 10 x 2 bytes the radio owns (cps-protocol.md 2.3): never overwritten
#define QUICK_END	0x7538u
#define GEN_DMR_ID	0x00E8u		// general settings: the DMR ID, 8 BCD digits
#define BANK0_BITMAP	0x3780u		// channels 1-128: 16-byte bitmap, then 128 records of 56 bytes
#define BANK1_BITMAP	0x9B1B0u	// channels 129-1024 in banks of the same shape (flash image 0x7B1B0)
#define BANK_STRIDE	(16u + 128u * CODEPLUG_OPENFW_CHANNEL_SIZE)
#define BANKS		8u
#define VERSION		1u
#define HEADER_LEN	20u

// What qdmr's open-firmware driver and tools/cps_client.py transfer (open-firmware-cps.md 7.1), file offsets
static const struct {
	uint32_t start, end;
} regions[] = {
	{ 0x00080u, 0x06060u },		// EEPROM image part 1: general settings, channels 1-128, ...
	{ 0x07500u, 0x0B000u },		// EEPROM image part 2: boot text, VFOs, zones
	{ 0x20000u, 0x211A0u },		// custom data
	{ 0x9B000u, 0xAEE60u },		// flash codeplug: channels 129-1024, contacts, RX group lists
};
#define REGIONS		(sizeof(regions) / sizeof(regions[0]))

static bool allowed = true;
static struct {
	bool open;			// a write is under way
	bool committing;
	bool recorded;			// the commit record is written
	uint32_t erased;		// staged bytes whose staging sectors are erased
	uint32_t crc;			// over the staged bytes so far
	uint32_t staged;		// staged bytes so far
	uint32_t sector;		// committing: the file offset of the next sector to copy
	uint8_t *buf;			// committing: the shared sector buffer
} w;

static const hal_flash_map_t *map(void)
{
	return storage_map();
}

static uint32_t staged_total(void)
{
	uint32_t n = 0;
	for (unsigned i = 0; i < REGIONS; i++) {
		n += regions[i].end - regions[i].start;
	}
	return n;
}

// Where a file offset is in the data flash
static uint32_t phys(uint32_t off)
{
	return off < EE_IMAGE ? map()->openfw_eeprom + off : map()->openfw_flash + (off - FL_IMAGE);
}

// Where a file offset (inside a region) is among the staged bytes, -1 if in no region
static int32_t staged_at(uint32_t off)
{
	uint32_t base = 0;
	for (unsigned i = 0; i < REGIONS; i++) {
		if (off >= regions[i].start && off < regions[i].end) {
			return (int32_t)(base + off - regions[i].start);
		}
		base += regions[i].end - regions[i].start;
	}
	return -1;
}

static uint32_t staging_data(void)
{
	return map()->staging_addr + SECTOR;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// The board has a staging area that holds the image and touches nothing else
static bool staging_ok(void)
{
	const hal_flash_map_t *m = map();
	if (m == NULL || board.flash == NULL || m->staging_size < SECTOR + staged_total() ||
	    m->staging_addr % SECTOR != 0 || m->staging_addr + m->staging_size > board.flash->size) {
		return false;
	}
	uint32_t a = m->staging_addr, n = m->staging_size;
	return !overlaps(a, n, m->settings_addr, m->settings_size) && !overlaps(a, n, m->journal_addr, 2u * SECTOR) &&
	       !overlaps(a, n, m->openfw_eeprom, EE_IMAGE) && !overlaps(a, n, m->openfw_flash, m->openfw_end - m->openfw_flash) &&
	       !overlaps(a, n, m->native_addr, m->native_size) && !overlaps(a, n, m->calibration_addr, m->calibration_size);
}

static bool flash_read(uint32_t addr, void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->read(addr, buf, len);
	storage_unlock();
	return ok;
}

static bool flash_erase(uint32_t addr)
{
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

static bool flash_program(uint32_t addr, const void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->write(addr, buf, len);
	storage_unlock();
	return ok;
}

static void put32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// CRC-32 of the staged bytes as they are in the staging area
static bool staged_crc(uint32_t *crc)
{
	uint8_t b[256];
	uint32_t c = 0, total = staged_total();
	for (uint32_t off = 0; off < total; off += sizeof(b)) {
		uint32_t n = total - off < sizeof(b) ? total - off : sizeof(b);
		if (!flash_read(staging_data() + off, b, n)) {
			return false;
		}
		c = storage_crc32(c, b, n);
	}
	*crc = c;
	return true;
}

// The staged bytes at a file offset (inside one region)
static bool staged_read(uint32_t off, void *buf, uint32_t len)
{
	int32_t s = staged_at(off);
	return s >= 0 && staged_at(off + len - 1u) == s + (int32_t)len - 1 && flash_read(staging_data() + (uint32_t)s, buf, len);
}

// ---- reading ----

static bool open_read(uint32_t *size)
{
	if (map() == NULL || board.flash == NULL) {
		return false;
	}
	*size = TFTP_CODEPLUG_SIZE;
	return true;
}

static bool read_file(uint32_t off, uint8_t *buf, uint16_t len)
{
	memset(buf, 0xFF, len);
	for (unsigned i = 0; i < REGIONS; i++) {
		uint32_t lo = off > regions[i].start ? off : regions[i].start;
		uint32_t hi = off + len < regions[i].end ? off + len : regions[i].end;
		if (lo < hi && !flash_read(phys(lo), buf + (lo - off), hi - lo)) {
			return false;
		}
	}
	return true;
}

// ---- writing ----

static void reset_write(void)
{
	if (w.buf != NULL) {
		storage_buffer_release();
	}
	memset(&w, 0, sizeof(w));
}

static int open_write(uint32_t tsize, const char **msg)
{
	if (!allowed) {
		*msg = "codeplug writes are off (Settings > Connections)";
		return TFTP_FAIL(TFTP_ERR_ACCESS);
	}
	if (!storage_ready()) {
		*msg = "the radio's flash isn't writable (foreign settings region?)";
		return TFTP_FAIL(TFTP_ERR_ACCESS);
	}
	if (!staging_ok()) {
		*msg = "no staging area on this radio";
		return TFTP_FAIL(TFTP_ERR_ACCESS);
	}
	if (tsize != 0 && tsize != TFTP_CODEPLUG_SIZE && tsize != TFTP_CODEPLUG_FULL) {
		*msg = "codeplug.bin is 716384 bytes (or a 1179648 byte cps_client image)";
		return TFTP_FAIL(TFTP_ERR_FULL);
	}
	reset_write();
	// A commit record left by a failed commit: that image is finished first (else it would be taken for this one's
	// staged bytes at the next boot)
	uint8_t h[4];
	if (!flash_read(map()->staging_addr, h, sizeof(h))) {
		*msg = "flash error";
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	if ((h[0] & h[1] & h[2] & h[3]) != 0xFF && tftp_codeplug_resume()) {
		codeplug_reload();
	}
	w.open = true;
	return TFTP_OK;
}

static int write_file(uint32_t off, const uint8_t *buf, uint16_t len, const char **msg)
{
	if (!w.open) {
		return TFTP_FAIL(TFTP_ERR_OTHER);
	}
	if (off + len > TFTP_CODEPLUG_FULL) {
		*msg = "codeplug.bin is 716384 bytes";
		return TFTP_FAIL(TFTP_ERR_FULL);
	}
	for (unsigned i = 0; i < REGIONS; i++) {
		uint32_t lo = off > regions[i].start ? off : regions[i].start;
		uint32_t hi = off + len < regions[i].end ? off + len : regions[i].end;
		if (lo >= hi) {
			continue;
		}
		uint32_t s = (uint32_t)staged_at(lo), n = hi - lo;
		while (w.erased < s + n) {	// sequential: erase the staging sectors as the data reaches them
			if (!flash_erase(staging_data() + w.erased)) {
				*msg = "flash erase failed";
				return TFTP_FAIL(TFTP_ERR_OTHER);
			}
			w.erased += SECTOR;
		}
		if (!flash_program(staging_data() + s, buf + (lo - off), n)) {
			*msg = "flash write failed";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		w.crc = storage_crc32(w.crc, buf + (lo - off), n);
		w.staged += n;
	}
	return TFTP_OK;
}

static bool bcd_ok(const uint8_t *b, int n)
{
	for (int i = 0; i < n; i++) {
		if ((b[i] >> 4) > 9 || (b[i] & 0x0F) > 9) {
			return false;
		}
	}
	return true;
}

// Is the staged image an open-firmware codeplug? The format has no header or version: the general settings' DMR ID must
// be BCD, and at least one channel marked used must decode
static const char *check_image(void)
{
	uint8_t b[CODEPLUG_OPENFW_CHANNEL_SIZE];
	if (!staged_read(GEN_DMR_ID, b, 4) || !bcd_ok(b, 4)) {
		return "not a codeplug image (the DMR ID field isn't BCD)";
	}
	for (unsigned bank = 0; bank < BANKS; bank++) {
		uint32_t bitmap = bank == 0 ? BANK0_BITMAP : BANK1_BITMAP + (bank - 1u) * BANK_STRIDE;
		uint8_t bits[16];
		if (!staged_read(bitmap, bits, sizeof(bits))) {
			return "staging read failed";
		}
		for (unsigned k = 0; k < 128u; k++) {
			channel_t ch;
			if ((bits[k / 8u] & (1u << (k % 8u))) != 0 &&
			    staged_read(bitmap + 16u + k * CODEPLUG_OPENFW_CHANNEL_SIZE, b, sizeof(b)) && codeplug_openfw_decode(b, &ch)) {
				return NULL;
			}
		}
	}
	return "not a codeplug image (no valid channel)";
}

static bool write_record(uint32_t crc)
{
	uint8_t h[HEADER_LEN];
	put32le(&h[0], TFTP_STAGING_MAGIC);
	put32le(&h[4], VERSION);
	put32le(&h[8], staged_total());
	put32le(&h[12], crc);
	put32le(&h[16], storage_crc32(0, h, 16));
	return flash_program(map()->staging_addr, h, sizeof(h));
}

// The next sector to copy, from file offset `from` (sector aligned) on: its file offset, UINT32_MAX when done
static uint32_t next_sector(uint32_t from)
{
	for (unsigned i = 0; i < REGIONS; i++) {
		uint32_t last = (regions[i].end - 1u) & ~(SECTOR - 1u);
		if (from <= last) {
			uint32_t first = regions[i].start & ~(SECTOR - 1u);
			return from > first ? from : first;
		}
	}
	return UINT32_MAX;
}

// Copy one sector of the staged image into the stored codeplug: the stored sector, the staged regions' bytes over
// it (not the quick keys), written through the journal (skipped when nothing changed)
static bool copy_sector(uint32_t s, uint8_t *buf)
{
	if (!flash_read(phys(s), buf, SECTOR)) {
		return false;
	}
	for (unsigned i = 0; i < REGIONS; i++) {
		uint32_t lo = s > regions[i].start ? s : regions[i].start;
		uint32_t hi = s + SECTOR < regions[i].end ? s + SECTOR : regions[i].end;
		for (uint32_t a = lo; a < hi;) {
			uint32_t b = hi;
			if (a < QUICK_END && b > QUICK_KEYS) {
				if (a >= QUICK_KEYS) {
					a = QUICK_END;	// the radio's quick keys stay
					continue;
				}
				b = QUICK_KEYS;
			}
			if (!staged_read(a, buf + (a - s), b - a)) {
				return false;
			}
			a = b;
		}
	}
	return storage_sector_write(phys(s), buf);
}

static int commit(uint32_t size, const char **msg)
{
	if (!w.committing) {
		uint32_t crc;
		const char *bad;
		if (size != TFTP_CODEPLUG_SIZE && size != TFTP_CODEPLUG_FULL) {
			reset_write();
			*msg = "codeplug.bin must be 716384 bytes (or a 1179648 byte cps_client image)";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		if (w.staged != staged_total() || !staged_crc(&crc) || crc != w.crc) {
			reset_write();
			*msg = "staging verify failed";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		if ((bad = check_image()) != NULL) {
			reset_write();
			*msg = bad;
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		w.committing = true;
		w.sector = next_sector(0);
	}
	if (w.buf == NULL) {
		w.buf = storage_buffer_acquire();
		if (w.buf == NULL) {
			return TFTP_PENDING;	// a CPS sector is pending: wait for it
		}
	}
	if (!w.recorded) {
		if (!write_record(w.crc)) {
			reset_write();
			*msg = "flash write failed";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		w.recorded = true;
		return TFTP_PENDING;
	}
	if (w.sector != UINT32_MAX) {
		if (!copy_sector(w.sector, w.buf)) {
			reset_write();	// the record stays: the next boot tries again
			*msg = "flash write failed (finished at the next power on)";
			return TFTP_FAIL(TFTP_ERR_OTHER);
		}
		w.sector = next_sector(w.sector + SECTOR);
		return TFTP_PENDING;
	}
	flash_erase(map()->staging_addr);
	reset_write();
	codeplug_reload();		// the radio core follows codeplug_serial()
	return TFTP_OK;
}

static void abort_write(void)
{
	if (!w.recorded) {
		reset_write();		// what was staged is left: nothing reads it without a record
	}
}

const tftp_file_t tftp_codeplug_file = {
	.name = "codeplug.bin",
	.open_read = open_read,
	.read = read_file,
	.open_write = open_write,
	.write = write_file,
	.commit = commit,
	.abort = abort_write,
};

void tftp_codeplug_allow_write(bool allow)
{
	allowed = allow;
}

bool tftp_codeplug_write_allowed(void)
{
	return allowed;
}

bool tftp_codeplug_resume(void)
{
	uint8_t h[HEADER_LEN];
	uint32_t crc;

	if (!staging_ok() || !storage_ready() || !flash_read(map()->staging_addr, h, sizeof(h))) {
		return false;
	}
	bool blank = true;
	for (unsigned i = 0; i < sizeof(h); i++) {
		blank = blank && h[i] == 0xFF;
	}
	if (blank) {
		return false;
	}
	bool valid = get32le(&h[0]) == TFTP_STAGING_MAGIC && get32le(&h[4]) == VERSION &&
		     get32le(&h[8]) == staged_total() && get32le(&h[16]) == storage_crc32(0, h, 16) &&
		     staged_crc(&crc) && crc == get32le(&h[12]);
	uint8_t *buf = valid ? storage_buffer_acquire() : NULL;
	bool done = buf != NULL;
	for (uint32_t s = next_sector(0); done && s != UINT32_MAX; s = next_sector(s + SECTOR)) {
		done = copy_sector(s, buf);
	}
	if (buf != NULL) {
		storage_buffer_release();
	}
	if (done || !valid) {
		flash_erase(map()->staging_addr);	// finished, or a torn record of a commit that never started
	}
	return done;
}
