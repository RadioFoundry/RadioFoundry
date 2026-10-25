// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fast codeplug protocol engine (cps_fast.h, docs/spec/cps-fast.md).
//
// Addresses on the wire are image offsets: 0 is the open-firmware EEPROM image (hal_flash_map_t.openfw_eeprom), the
// flash image follows at openfw_flash - openfw_eeprom, as in the files tools/cps_client.py and the web editor keep.
// Blocks are 4 KB, the flash's erase unit, so a block write is one sector rewrite.
//
// Staged writes (cps-fast.md 7): each block goes to its own slot of the staging area (slot k at staging_addr +
// (k + 1) x 4 KB), is read back and checked there, and the stored codeplug is not touched. COMMIT writes the record
// (sector 0 of the staging area: magic, version, count, the CRC-32 of the staged bytes, the slots' block numbers, the
// record's own CRC-32), reads it back, then the tick copies one slot a pass through the sector journal and erases the
// record at the end. A power cut before the record leaves the old codeplug; after it the next boot finishes the copy
// (cps_fast_resume). Direct writes (no staging area) go through the sector journal block by block, as the
// open-firmware CPS's do.
#include <string.h>
#include "config.h"
#include "board.h"
#include "cps/cps_fast.h"
#include "cps/cps_lz4.h"
#include "cps/cps_openfw.h"
#include "codeplug/codeplug.h"
#include "storage/storage.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_TFTP
#include "net/tftp_codeplug.h"
#endif

#define REC_VERSION	1u
#define REC_HDR		16u	// magic, version, count, CRC-32 of the staged bytes
#define REC_MAX		(REC_HDR + 2u * CPSF_STAGE_MAX + 4u)
#define CHUNK		128u	// flash bytes hashed at a time (on the host link task's stack)
#define LZ4_MAX		(CPSF_BLOCK + CPSF_BLOCK / 255u + 16u)	// the longest stream a 4 KB block can need
#define READ_HDR	15u	// READ reply: status, addr, format, length, CRC-32, offset
#define REC_BLANK_CRC	0x54BDF9D4u	// CRC-32 of REC_MAX bytes of 0xFF: an erased record sector (cpsFastTest checks it)

static struct {
	uint8_t state;		// CPSF_ST_*
	uint8_t mode;		// CPSF_MODE_*
	uint32_t last_ms;	// the session's last request
	bool written;		// direct: a block went in this session (the codeplug is re-read at its end)
	uint16_t blocks;	// blocks ended this session
	// The block in progress
	uint32_t addr;
	uint32_t crc;
	uint16_t length;
	uint16_t received;
	uint8_t format;
	uint8_t *buf;		// the shared sector buffer: held from BLOCK to END, and for each sector a commit copies
	cps_lz4_dec_t dec;
	// Staged blocks: slot k holds image block block[k]
	uint16_t slots;
	uint16_t copied;	// committing: slots copied so far
	uint16_t block[CPSF_STAGE_MAX];
} s;
static cps_fast_stats_t stats;

// --- little endian helpers ---

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

// --- the flash, under the storage lock ---

static const hal_flash_map_t *map(void)
{
	return board.flash != NULL ? storage_map() : NULL;
}

static bool fread_(uint32_t addr, void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->read(addr, buf, len);
	storage_unlock();
	return ok;
}

static bool ferase(uint32_t addr)
{
	storage_lock();
	bool ok = board.flash->erase(addr);
	storage_unlock();
	return ok;
}

static bool fprog(uint32_t addr, const void *buf, uint32_t len)
{
	storage_lock();
	bool ok = board.flash->write(addr, buf, len);
	storage_unlock();
	return ok;
}

// CRC-32 of len bytes of flash at addr
static bool fcrc(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint8_t c[CHUNK];
	uint32_t v = 0;
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!fread_(addr + off, c, n)) {
			return false;
		}
		v = storage_crc32(v, c, n);
	}
	*crc = v;
	return true;
}

// --- the image and the radio's own areas ---

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

static uint32_t phys(uint32_t off)
{
	return map()->openfw_eeprom + off;
}

// The radio's own areas: no block that touches one is read or written
static bool protected_area(const hal_flash_map_t *m, uint32_t a, uint32_t n)
{
	return overlaps(a, n, m->settings_addr, m->settings_size) || overlaps(a, n, m->journal_addr, 2u * STORAGE_SECTOR) ||
	       overlaps(a, n, m->native_addr, m->native_size) || overlaps(a, n, m->staging_addr, m->staging_size) ||
	       overlaps(a, n, m->calibration_addr, m->calibration_size);
}

// An image block the protocol may read and write: inside the EEPROM image or the flash image, on the flash, clear of
// the radio's own areas
static bool block_ok(uint32_t off)
{
	const hal_flash_map_t *m = map();
	if (m == NULL || off % CPSF_BLOCK != 0) {
		return false;
	}
	uint32_t fl = m->openfw_flash - m->openfw_eeprom, end = m->openfw_end - m->openfw_eeprom;
	if (!(off < CPSF_EE_SIZE || (off >= fl && off < end && end - off >= CPSF_BLOCK))) {
		return false;
	}
	uint32_t p = m->openfw_eeprom + off;
	return p <= board.flash->size - CPSF_BLOCK && !protected_area(m, p, CPSF_BLOCK);
}

// A block's CRC-32 and whether it is all 0xFF, as the image shows it (the live VFO in VFO A)
static bool block_hash(uint32_t off, uint32_t *crc, bool *blank)
{
	uint8_t c[CHUNK];
	uint32_t v = 0;
	bool b = true;
	for (uint32_t o = 0; o < CPSF_BLOCK; o += CHUNK) {
		if (!fread_(phys(off + o), c, CHUNK)) {
			return false;
		}
		cps_openfw_vfo_overlay(phys(off + o), c, CHUNK);
		v = storage_crc32(v, c, CHUNK);
		for (unsigned i = 0; i < CHUNK && b; i++) {
			b = c[i] == 0xFF;
		}
	}
	*crc = v;
	*blank = b;
	return true;
}

// --- the staging area ---

// Slots the staging area holds after its record sector (0: no staging area)
static uint32_t stage_cap(void)
{
	uint32_t n = storage_staging_size() / CPSF_BLOCK;
	return n == 0 ? 0 : (n - 1u < CPSF_STAGE_MAX ? n - 1u : CPSF_STAGE_MAX);
}

static uint32_t slot_addr(uint32_t k)
{
	return map()->staging_addr + (k + 1u) * CPSF_BLOCK;
}

static uint32_t record_word(void)
{
	uint8_t w[4];
	return fread_(map()->staging_addr, w, sizeof(w)) ? get32(w) : 0;
}

// The record for the staged slots: its length
static uint32_t make_record(uint8_t *r, uint32_t data_crc)
{
	uint32_t n = REC_HDR + 2u * s.slots;
	put32(&r[0], STORAGE_STAGE_MAGIC_CPSF);
	put32(&r[4], REC_VERSION);
	put32(&r[8], s.slots);
	put32(&r[12], data_crc);
	for (unsigned k = 0; k < s.slots; k++) {
		put16(&r[REC_HDR + 2u * k], s.block[k]);
	}
	put32(&r[n], storage_crc32(0, r, n));
	return n + 4u;
}

// CRC-32 of the staged slots, in slot order
static bool staged_crc(uint32_t count, uint32_t *crc)
{
	uint8_t c[CHUNK];
	uint32_t v = 0;
	for (uint32_t k = 0; k < count; k++) {
		for (uint32_t o = 0; o < CPSF_BLOCK; o += CHUNK) {
			if (!fread_(slot_addr(k) + o, c, CHUNK)) {
				return false;
			}
			v = storage_crc32(v, c, CHUNK);
		}
	}
	*crc = v;
	return true;
}

// Copy slot k into the stored codeplug at image block blk through the journal; with now_ms (not at boot, where the
// settings aren't up yet) a changed VFO A record becomes the VFO's settings, as a CPS write does
static bool copy_slot(uint32_t k, uint32_t blk, uint8_t *buf, bool vfo, uint32_t now_ms)
{
	uint32_t p = phys(blk * CPSF_BLOCK);
	if (!fread_(slot_addr(k), buf, CPSF_BLOCK)) {
		return false;
	}
	if (vfo) {
		cps_openfw_vfo_written(p, buf, now_ms);
	}
	return storage_sector_write(p, buf);
}

// --- replies ---

static size_t status_only(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

static size_t refuse(uint8_t *out, uint8_t why)
{
	stats.refused++;
	stats.reason = why;
	out[0] = DL_ST_REFUSED;
	out[1] = why;
	return 2;
}

static void drop_block(void)
{
	if (s.buf != NULL) {
		storage_buffer_release();
		s.buf = NULL;
	}
	if (s.state == CPSF_ST_BLOCK) {
		s.state = CPSF_ST_OPEN;
	}
}

// A session ends without a commit: staged slots are forgotten (no record, so nothing reads them); a direct session
// that wrote blocks re-reads the codeplug
static void end_session(uint8_t state)
{
	drop_block();
	if (s.written) {
		codeplug_reload();	// the radio core follows codeplug_serial()
		stats.commits++;
	}
	s.written = false;
	s.slots = 0;
	s.state = state;
}

static bool in_session(void)
{
	return s.state == CPSF_ST_OPEN || s.state == CPSF_ST_BLOCK;
}

// --- operations ---

static size_t op_info(uint8_t *out)
{
	const hal_flash_map_t *m = map();
	uint32_t cap = stage_cap();
	out[0] = DL_ST_OK;
	out[1] = CPSF_VERSION;
	out[2] = (uint8_t)((cap != 0 ? 1u : 0u) | (storage_ready() ? 2u : 0u));
	put16(&out[3], CPSF_BLOCK);
	out[5] = CPSF_SUMS_MAX;
	out[6] = CPSF_DATA_MAX;
	put16(&out[7], CPSF_READ_MAX);
	put32(&out[9], CPSF_EE_SIZE);
	put32(&out[13], m->openfw_flash - m->openfw_eeprom);
	put32(&out[17], m->openfw_end - m->openfw_eeprom);
	put32(&out[21], m->openfw_eeprom);
	put16(&out[25], cap);
	return 27;
}

static size_t op_sums(const uint8_t *a, uint16_t len, uint8_t *out)
{
	if (len != 5 || a[4] == 0 || a[4] > CPSF_SUMS_MAX) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t addr = get32(a), count = a[4], mask = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (!block_ok(addr + i * CPSF_BLOCK)) {
			return refuse(out, CPSF_R_RANGE);
		}
	}
	out[0] = DL_ST_OK;
	put32(&out[1], addr);
	out[5] = (uint8_t)count;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t crc;
		bool blank;
		if (!block_hash(addr + i * CPSF_BLOCK, &crc, &blank)) {
			return refuse(out, CPSF_R_FLASH);
		}
		mask |= blank ? 1u << i : 0u;
		put32(&out[8 + 4u * i], crc);
	}
	put16(&out[6], mask);
	return 8u + 4u * count;
}

// One window of a block's stream: the radio encodes the block again for every window (a few hundred microseconds),
// so a READ is stateless and a lost reply is simply asked for again
static size_t op_read(const uint8_t *a, uint16_t len, uint8_t *out, size_t cap)
{
	if (len != 6 || cap < READ_HDR + CPSF_READ_MAX) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t addr = get32(a), offset = get16(&a[4]);
	if (!block_ok(addr)) {
		return refuse(out, CPSF_R_RANGE);
	}
	uint8_t *buf = storage_buffer_acquire();
	if (buf == NULL) {
		return status_only(out, DL_ST_BUSY);	// a block being written, or a CPS sector pending
	}
	bool ok = fread_(phys(addr), buf, CPSF_BLOCK);
	if (ok) {
		cps_openfw_vfo_overlay(phys(addr), buf, CPSF_BLOCK);
	}
	uint32_t crc = storage_crc32(0, buf, CPSF_BLOCK);
	bool blank = true;
	for (unsigned i = 0; i < CPSF_BLOCK && blank; i++) {
		blank = buf[i] == 0xFF;
	}
	uint32_t total = blank ? 0 : cps_lz4_encode(buf, CPSF_BLOCK, out + READ_HDR, offset, CPSF_READ_MAX);
	storage_buffer_release();
	if (!ok) {
		return refuse(out, CPSF_R_FLASH);
	}
	if (offset > total) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t n = total - offset < CPSF_READ_MAX ? total - offset : CPSF_READ_MAX;
	out[0] = DL_ST_OK;
	put32(&out[1], addr);
	out[5] = blank ? CPSF_FMT_BLANK : CPSF_FMT_LZ4;
	put16(&out[6], total);
	put32(&out[8], crc);
	put16(&out[12], offset);
	if (offset + n == total) {
		stats.blocks_read++;
	}
	return READ_HDR + n;
}

static size_t op_begin(const uint8_t *a, uint16_t len, uint8_t *out, uint32_t now_ms)
{
	if (len != 1 || a[0] > CPSF_MODE_STAGED) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (s.state == CPSF_ST_COMMITTING) {
		return status_only(out, DL_ST_BUSY);
	}
	if (in_session()) {
		return refuse(out, CPSF_R_STATE);
	}
	if (!storage_ready()) {
		return refuse(out, CPSF_R_STORAGE);
	}
	uint32_t cap = 0;
	if (a[0] == CPSF_MODE_STAGED) {
		cap = stage_cap();
		if (cap == 0) {
			return refuse(out, CPSF_R_NO_STAGING);
		}
#if CONFIG_TFTP
		if (tftp_codeplug_writing()) {
			return refuse(out, CPSF_R_STAGE_BUSY);
		}
#endif
		// The record sector must be erased: a record there belongs to a commit the next boot finishes. One with an
		// erased first word is no one's record (a torn erase): erased again
		uint32_t crc;
		if (record_word() != 0xFFFFFFFFu) {
			return refuse(out, CPSF_R_STAGE_BUSY);
		}
		if ((!fcrc(map()->staging_addr, REC_MAX, &crc) || crc != REC_BLANK_CRC) && !ferase(map()->staging_addr)) {
			return refuse(out, CPSF_R_FLASH);
		}
	}
	s.state = CPSF_ST_OPEN;
	s.mode = a[0];
	s.written = false;
	s.blocks = 0;
	s.slots = 0;
	s.copied = 0;
	s.last_ms = now_ms;
	stats.reason = CPSF_R_NONE;
	out[0] = DL_ST_OK;
	put16(&out[1], cap);
	return 3;
}

static size_t op_block(const uint8_t *a, uint16_t len, uint8_t *out)
{
	if (len != 11) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	uint32_t addr = get32(a), length = get16(&a[5]);
	uint8_t format = a[4];
	if ((format == CPSF_FMT_LZ4) ? (length == 0 || length > LZ4_MAX) : (format != CPSF_FMT_BLANK || length != 0)) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (!in_session()) {
		return refuse(out, CPSF_R_STATE);
	}
	if (!block_ok(addr)) {
		return refuse(out, CPSF_R_RANGE);
	}
	if (s.mode == CPSF_MODE_STAGED && s.slots >= stage_cap()) {
		bool again = false;
		for (unsigned k = 0; k < s.slots && !again; k++) {
			again = s.block[k] == addr / CPSF_BLOCK;
		}
		if (!again) {
			return refuse(out, CPSF_R_STAGE_FULL);
		}
	}
	if (s.buf == NULL && (s.buf = storage_buffer_acquire()) == NULL) {
		return status_only(out, DL_ST_BUSY);
	}
	s.addr = addr;
	s.format = format;
	s.length = (uint16_t)length;
	s.crc = get32(&a[7]);
	s.received = 0;
	if (format == CPSF_FMT_BLANK) {
		memset(s.buf, 0xFF, CPSF_BLOCK);
	} else {
		cps_lz4_dec_init(&s.dec, s.buf, CPSF_BLOCK);
	}
	s.state = CPSF_ST_BLOCK;
	return status_only(out, DL_ST_OK);
}

static size_t op_data(const uint8_t *a, uint16_t len, uint8_t *out)
{
	if (len < 3 || len - 2u > CPSF_DATA_MAX) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (s.state != CPSF_ST_BLOCK) {
		return refuse(out, CPSF_R_STATE);
	}
	uint32_t off = get16(a), n = len - 2u;
	if (off + n > s.received) {
		// New bytes: they must follow on from what came (a repeat of bytes already taken is only answered)
		if (off != s.received || off + n > s.length) {
			return refuse(out, CPSF_R_STREAM);
		}
		if (!cps_lz4_dec_feed(&s.dec, a + 2, n)) {
			drop_block();
			return refuse(out, CPSF_R_STREAM);
		}
		s.received = (uint16_t)(off + n);
	}
	out[0] = DL_ST_OK;
	put16(&out[1], s.received);
	return 3;
}

// Stage the block in the buffer: its slot (the one it had, or the next) erased, programmed and read back
static bool stage_block(uint32_t blk)
{
	uint32_t k = 0, crc;
	while (k < s.slots && s.block[k] != blk) {
		k++;
	}
	uint32_t at = slot_addr(k);
	if (!ferase(at) || !fprog(at, s.buf, CPSF_BLOCK) || !fcrc(at, CPSF_BLOCK, &crc) || crc != s.crc) {
		return false;
	}
	s.block[k] = (uint16_t)blk;
	if (k == s.slots) {
		s.slots++;
	}
	return true;
}

static size_t op_end(uint16_t len, uint8_t *out, uint32_t now_ms)
{
	if (len != 0) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (s.state != CPSF_ST_BLOCK) {
		return refuse(out, CPSF_R_STATE);
	}
	if (s.received != s.length || (s.format == CPSF_FMT_LZ4 && !cps_lz4_dec_done(&s.dec))) {
		drop_block();
		return refuse(out, CPSF_R_STREAM);
	}
	if (storage_crc32(0, s.buf, CPSF_BLOCK) != s.crc) {
		drop_block();
		return refuse(out, CPSF_R_CRC);
	}
	bool ok;
	if (s.mode == CPSF_MODE_STAGED) {
		ok = stage_block(s.addr / CPSF_BLOCK);
	} else {
		cps_openfw_vfo_written(phys(s.addr), s.buf, now_ms);
		ok = storage_sector_write(phys(s.addr), s.buf);
		s.written = s.written || ok;
	}
	drop_block();
	if (!ok) {
		return refuse(out, CPSF_R_FLASH);
	}
	s.blocks++;
	stats.blocks_written++;
	return status_only(out, DL_ST_OK);
}

static size_t op_commit(uint16_t len, uint8_t *out)
{
	if (len != 0) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (s.state == CPSF_ST_COMMITTING) {
		return status_only(out, DL_ST_BUSY);
	}
	if (s.state != CPSF_ST_OPEN) {
		return refuse(out, CPSF_R_STATE);
	}
	if (s.mode == CPSF_MODE_DIRECT || s.slots == 0) {
		end_session(CPSF_ST_DONE);
		return status_only(out, DL_ST_OK);
	}
	uint8_t r[REC_MAX];
	uint32_t data_crc, back;
	if (!staged_crc(s.slots, &data_crc)) {
		return refuse(out, CPSF_R_FLASH);
	}
	uint32_t n = make_record(r, data_crc);
	uint32_t want = storage_crc32(0, r, n);
	if (!fprog(map()->staging_addr, r, n) || !fcrc(map()->staging_addr, n, &back) || back != want) {
		ferase(map()->staging_addr);	// a record that didn't take: the staged blocks stay, COMMIT may be sent again
		return refuse(out, CPSF_R_FLASH);
	}
	s.copied = 0;
	s.state = CPSF_ST_COMMITTING;
	return status_only(out, DL_ST_OK);
}

static size_t op_status(uint8_t *out)
{
	out[0] = DL_ST_OK;
	out[1] = s.state;
	out[2] = s.mode;
	put16(&out[3], s.blocks);
	put16(&out[5], s.copied);
	out[7] = stats.reason;
	return 8;
}

size_t cps_fast_cmd(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap, uint32_t now_ms)
{
	if (len < 1) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (map() == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	if (in_session()) {
		s.last_ms = now_ms;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);
	switch (req[0]) {
	case CPSF_OP_INFO:
		return op_info(out);
	case CPSF_OP_SUMS:
		return op_sums(a, n, out);
	case CPSF_OP_READ:
		return op_read(a, n, out, cap);
	case CPSF_OP_BEGIN:
		return op_begin(a, n, out, now_ms);
	case CPSF_OP_BLOCK:
		return op_block(a, n, out);
	case CPSF_OP_DATA:
		return op_data(a, n, out);
	case CPSF_OP_END:
		return op_end(n, out, now_ms);
	case CPSF_OP_COMMIT:
		return op_commit(n, out);
	case CPSF_OP_STATUS:
		return op_status(out);
	case CPSF_OP_ABORT:
		if (s.state == CPSF_ST_COMMITTING) {
			return status_only(out, DL_ST_BUSY);	// a commit always runs to its end
		}
		if (in_session()) {
			end_session(CPSF_ST_IDLE);
		}
		return status_only(out, DL_ST_OK);
	default:
		return status_only(out, DL_ST_BAD_ARG);
	}
}

void cps_fast_tick(uint32_t now_ms)
{
	if (in_session() && now_ms - s.last_ms > CPSF_SESSION_MS) {
		end_session(CPSF_ST_IDLE);	// the host went away
		return;
	}
	if (s.state != CPSF_ST_COMMITTING) {
		return;
	}
	// One sector a pass: the host link task answers STATUS between them
	if (s.copied < s.slots) {
		uint8_t *buf = storage_buffer_acquire();
		if (buf == NULL) {
			return;		// a CPS sector is pending: the next pass
		}
		bool ok = copy_slot(s.copied, s.block[s.copied], buf, true, now_ms);
		storage_buffer_release();
		if (!ok) {
			stats.reason = CPSF_R_FLASH;
			s.state = CPSF_ST_FAILED;	// the record stays: the next boot copies again
			return;
		}
		s.copied++;
		return;
	}
	ferase(map()->staging_addr);
	s.slots = 0;
	s.state = CPSF_ST_DONE;
	stats.commits++;
	codeplug_reload();
}

bool cps_fast_staging_busy(void)
{
	if ((in_session() && s.mode == CPSF_MODE_STAGED) || s.state == CPSF_ST_COMMITTING) {
		return true;
	}
	return stage_cap() != 0 && record_word() == STORAGE_STAGE_MAGIC_CPSF;
}

bool cps_fast_resume(void)
{
	uint8_t r[REC_MAX];
	uint32_t cap = stage_cap(), crc;

	memset(&s, 0, sizeof(s));
	if (cap == 0 || !storage_ready() || !fread_(map()->staging_addr, r, sizeof(r))) {
		return false;
	}
	uint32_t magic = get32(r), count = get32(&r[8]);
	if (magic == 0xFFFFFFFFu || magic == STORAGE_STAGE_MAGIC_TFTP) {
		return false;		// nothing, or the TFTP server's record (tftp_codeplug_resume)
	}
	bool valid = magic == STORAGE_STAGE_MAGIC_CPSF && get32(&r[4]) == REC_VERSION && count != 0 && count <= cap &&
		     get32(&r[REC_HDR + 2u * count]) == storage_crc32(0, r, REC_HDR + 2u * count) &&
		     staged_crc(count, &crc) && crc == get32(&r[12]);
	for (uint32_t k = 0; valid && k < count; k++) {
		valid = block_ok(get16(&r[REC_HDR + 2u * k]) * CPSF_BLOCK);
	}
	uint8_t *buf = valid ? storage_buffer_acquire() : NULL;
	bool done = buf != NULL;
	for (uint32_t k = 0; done && k < count; k++) {
		done = copy_slot(k, get16(&r[REC_HDR + 2u * k]), buf, false, 0);
	}
	if (buf != NULL) {
		storage_buffer_release();
	}
	if (done || !valid) {
		ferase(map()->staging_addr);	// finished, or a torn record of a commit that never started
	}
	return done;
}

void cps_fast_get_stats(cps_fast_stats_t *out)
{
	*out = stats;
	out->state = s.state;
}
