// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The sector journal (storage.h, docs/spec/settings-store.md 4). Two erase units at map->journal_addr:
//   J0  header: magic, target address, CRC-32 of the copy, header CRC, then a "done" byte (0xFF = pending)
//   J1  the new content of the target sector
// A rewrite: J1 erased and programmed, J0 erased and the header programmed, the target erased and programmed, then
// the done byte programmed to 0. A cut before the header is complete leaves the target as it was; a cut after it is
// finished at the next boot from J1.
#include <string.h>
#include "storage/storage.h"

#define J_MAGIC		0x4C4E4A4Fu	// "OJNL"
#define J_HDR_LEN	16u		// magic, target, data CRC, header CRC
#define J_DONE		J_HDR_LEN	// the done byte's offset
#define CHUNK		256u

static bool ready;
static uint8_t buffer[STORAGE_SECTOR];
static bool buffer_taken;
static storage_stats_t stats;

uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len-- > 0) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const hal_flash_map_t *storage_map(void)
{
	return board.flash != NULL ? board.flash->map : NULL;
}

static bool fail(void)
{
	stats.errors++;
	return false;
}

static bool rd(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash->read(addr, buf, len) || fail();
}

static bool wr(uint32_t addr, const void *buf, uint32_t len)
{
	return board.flash->write(addr, buf, len) || fail();
}

static bool er(uint32_t addr)
{
	return board.flash->erase(addr) || fail();
}

// CRC of a flash range, read in chunks
static bool flash_crc(uint32_t addr, uint32_t len, uint32_t *out)
{
	uint8_t c[CHUNK];
	uint32_t crc = 0;

	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		crc = storage_crc32(crc, c, n);
	}
	*out = crc;
	return true;
}

// Whether the flash holds exactly data
static bool same(uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint8_t c[CHUNK];

	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n) || memcmp(c, data + off, n) != 0) {
			return false;
		}
	}
	return true;
}

// Erase dst and copy the sector at src into it, a chunk at a time
static bool copy_sector(uint32_t dst, uint32_t src)
{
	uint8_t c[CHUNK];

	if (!er(dst)) {
		return false;
	}
	for (uint32_t off = 0; off < STORAGE_SECTOR; off += CHUNK) {
		if (!rd(src + off, c, CHUNK) || !wr(dst + off, c, CHUNK)) {
			return false;
		}
	}
	return true;
}

static bool target_ok(const hal_flash_map_t *m, uint32_t target)
{
	uint32_t j = m->journal_addr;

	return (target % STORAGE_SECTOR) == 0 && target < board.flash->size && target + STORAGE_SECTOR > target &&
	       !(target + STORAGE_SECTOR > j && target < j + 2u * STORAGE_SECTOR);
}

// A pending journal entry: finish it
static void replay(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t h[J_HDR_LEN + 1];
	uint32_t crc;

	if (!rd(m->journal_addr, h, sizeof(h)) || get32(&h[0]) != J_MAGIC || h[J_DONE] != 0xFF ||
	    get32(&h[12]) != storage_crc32(0, h, 12) || !target_ok(m, get32(&h[4]))) {
		return;
	}
	if (flash_crc(m->journal_addr + STORAGE_SECTOR, STORAGE_SECTOR, &crc) && crc == get32(&h[8])) {
		uint32_t target = get32(&h[4]);
		uint32_t now;
		if (!flash_crc(target, STORAGE_SECTOR, &now) || now != crc) {
			if (!copy_sector(target, m->journal_addr + STORAGE_SECTOR)) {
				return;		// try again at the next boot
			}
		}
		stats.replays++;
	}
	uint8_t done = 0;
	wr(m->journal_addr + J_DONE, &done, 1);
}

bool storage_init(void)
{
	const hal_flash_map_t *m = storage_map();

	ready = false;
	if (m == NULL || board.flash->size == 0 || board.flash->erase_size != STORAGE_SECTOR ||
	    board.flash->write == NULL || board.flash->erase == NULL) {
		return false;
	}
	ready = true;
	storage_lock();
	replay();
	storage_unlock();
	return true;
}

bool storage_ready(void)
{
	return ready;
}

bool storage_sector_write(uint32_t addr, const uint8_t *data)
{
	const hal_flash_map_t *m = storage_map();
	bool ok = false;

	if (!ready || !target_ok(m, addr)) {
		return false;
	}
	storage_lock();
	if (same(addr, data, STORAGE_SECTOR)) {
		stats.sector_skips++;
		storage_unlock();
		return true;
	}
	uint32_t crc = storage_crc32(0, data, STORAGE_SECTOR);
	uint32_t j1 = m->journal_addr + STORAGE_SECTOR;
	uint8_t h[J_HDR_LEN];
	put32(&h[0], J_MAGIC);
	put32(&h[4], addr);
	put32(&h[8], crc);
	put32(&h[12], storage_crc32(0, h, 12));
	uint32_t check;
	uint8_t done = 0;
	if (er(j1) && wr(j1, data, STORAGE_SECTOR) && flash_crc(j1, STORAGE_SECTOR, &check) && check == crc &&
	    er(m->journal_addr) && wr(m->journal_addr, h, sizeof(h)) &&
	    er(addr) && wr(addr, data, STORAGE_SECTOR) && same(addr, data, STORAGE_SECTOR) &&
	    wr(m->journal_addr + J_DONE, &done, 1)) {
		stats.sector_writes++;
		ok = true;
	}
	storage_unlock();
	return ok;
}

uint8_t *storage_buffer_acquire(void)
{
	uint8_t *b = NULL;

	storage_lock();
	if (!buffer_taken) {
		buffer_taken = true;
		b = buffer;
	}
	storage_unlock();
	return b;
}

void storage_buffer_release(void)
{
	storage_lock();
	buffer_taken = false;
	storage_unlock();
}

bool storage_write(uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	if (!ready || len == 0 || addr + len < addr || addr + len > board.flash->size) {
		return ready && len == 0;
	}
	storage_lock();
	uint8_t *b = storage_buffer_acquire();
	bool ok = b != NULL;
	while (ok && len > 0) {
		uint32_t base = addr & ~(STORAGE_SECTOR - 1u);
		uint32_t off = addr - base;
		uint32_t n = STORAGE_SECTOR - off < len ? STORAGE_SECTOR - off : len;
		ok = rd(base, b, STORAGE_SECTOR);
		if (ok) {
			memcpy(b + off, p, n);
			ok = storage_sector_write(base, b);
		}
		addr += n;
		p += n;
		len -= n;
	}
	if (b != NULL) {
		storage_buffer_release();
	}
	storage_unlock();
	return ok;
}

void storage_get_stats(storage_stats_t *s)
{
	*s = stats;
}
