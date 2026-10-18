// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The sector journal (storage.h, docs/spec/settings-store.md 4). Two erase units at map->journal_addr:
//   J0  header: magic, target address, CRC-32 of the copy, header CRC, then a "done" byte (0xFF = pending)
//   J1  the new content of the target sector
// A rewrite: J1 erased and programmed, J0 erased and the header programmed, the target erased and programmed, then
// the done byte programmed to 0. A cut before the header is complete leaves the target as it was; a cut after it is
// finished at the next boot from J1.
// A write of up to P_MAX bytes (storage_write: a codeplug record, which may cross a sector end) is one transaction
// over the one or two sectors it touches, so a cut never leaves a record half old, half new:
//   J0  patch header: magic "OJN2", address, length, CRC-32 of bytes 0-11 and the data; marks (staged and done per
//       sector, then all done; 0xFF = not yet); the data at P_DATA
//   J1  the new content of the sector being rewritten
// J0 programmed is the commit point. Then for each sector: J1 = the sector merged with the data, "staged", the
// sector erased and programmed from J1, "done". Replay: a sector not staged is still intact, so it is merged again.
#include <string.h>
#include "storage/storage.h"

#define J_MAGIC		0x4C4E4A4Fu	// "OJNL"
#define J_HDR_LEN	16u		// magic, target, data CRC, header CRC
#define J_DONE		J_HDR_LEN	// the done byte's offset
#define P_MAGIC		0x324E4A4Fu	// "OJN2"
#define P_MARK		16u		// staged, done for the first sector, the same for the second, then all done
#define P_ALL		(P_MARK + 4u)
#define P_DATA		24u
#define P_MAX		(STORAGE_SECTOR - P_DATA)
#define CHUNK		256u

static bool ready;			// usable flash (the region may still be foreign)
static storage_region_t region;
static bool claim_pending;
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

// CRC of a flash range, read in chunks, continuing from crc (0 to start)
static bool flash_crc(uint32_t addr, uint32_t len, uint32_t crc, uint32_t *out)
{
	uint8_t c[CHUNK];

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

// A patch header in J0 (h: its first P_DATA bytes): the bytes of J0 it uses, 0 if it isn't one or is torn
static uint32_t patch_len(const hal_flash_map_t *m, const uint8_t *h)
{
	uint32_t len = get32(&h[8]), crc;
	if (get32(&h[0]) != P_MAGIC || len == 0 || len > P_MAX ||
	    !flash_crc(m->journal_addr + P_DATA, len, storage_crc32(0, h, 12), &crc) || crc != get32(&h[12])) {
		return 0;
	}
	return P_DATA + len;
}

// Sector k (0, 1) of the patch transaction in J0, at its address and length: staged into J1 unless it was (the
// sector is still intact then), then rewritten from J1. b: a sector buffer
static bool patch_sector(const hal_flash_map_t *m, uint32_t addr, uint32_t len, unsigned k, uint8_t *b)
{
	static const uint8_t zero = 0;
	uint32_t j0 = m->journal_addr, j1 = j0 + STORAGE_SECTOR;
	uint32_t base = (addr & ~(STORAGE_SECTOR - 1u)) + k * STORAGE_SECTOR;
	uint32_t lo = addr > base ? addr : base;
	uint32_t hi = addr + len < base + STORAGE_SECTOR ? addr + len : base + STORAGE_SECTOR;
	uint8_t mk[2];

	if (!target_ok(m, base) || !rd(j0 + P_MARK + 2u * k, mk, 2)) {
		return false;
	}
	if (mk[1] == 0) {
		return true;
	}
	if (mk[0] != 0) {
		if (!rd(base, b, STORAGE_SECTOR) || !rd(j0 + P_DATA + (lo - addr), b + (lo - base), hi - lo) ||
		    !er(j1) || !wr(j1, b, STORAGE_SECTOR) || !same(j1, b, STORAGE_SECTOR) ||
		    !wr(j0 + P_MARK + 2u * k, &zero, 1)) {
			return false;
		}
	} else if (!rd(j1, b, STORAGE_SECTOR)) {
		return false;
	}
	if (!same(base, b, STORAGE_SECTOR) && !(er(base) && wr(base, b, STORAGE_SECTOR) && same(base, b, STORAGE_SECTOR))) {
		return false;
	}
	stats.sector_writes++;
	return wr(j0 + P_MARK + 2u * k + 1u, &zero, 1);
}

// Every sector of the patch transaction in J0, then the all done mark
static bool patch_finish(const hal_flash_map_t *m, uint32_t addr, uint32_t len, uint8_t *b)
{
	static const uint8_t zero = 0;
	bool two = (addr & (STORAGE_SECTOR - 1u)) + len > STORAGE_SECTOR;
	return patch_sector(m, addr, len, 0, b) && (!two || patch_sector(m, addr, len, 1, b)) &&
	       wr(m->journal_addr + P_ALL, &zero, 1);
}

// A pending journal entry: finish it
static void replay(void)
{
	const hal_flash_map_t *m = storage_map();
	uint8_t h[P_DATA];
	uint32_t crc;

	if (!rd(m->journal_addr, h, sizeof(h))) {
		return;
	}
	if (get32(&h[0]) == P_MAGIC) {
		if (h[P_ALL] == 0xFF && patch_len(m, h) != 0 && patch_finish(m, get32(&h[4]), get32(&h[8]), buffer)) {
			stats.replays++;
		}
		return;
	}
	if (get32(&h[0]) != J_MAGIC || h[J_DONE] != 0xFF ||
	    get32(&h[12]) != storage_crc32(0, h, 12) || !target_ok(m, get32(&h[4]))) {
		return;
	}
	if (flash_crc(m->journal_addr + STORAGE_SECTOR, STORAGE_SECTOR, 0, &crc) && crc == get32(&h[8])) {
		uint32_t target = get32(&h[4]);
		uint32_t now;
		if (!flash_crc(target, STORAGE_SECTOR, 0, &now) || now != crc) {
			if (!copy_sector(target, m->journal_addr + STORAGE_SECTOR)) {
				return;		// try again at the next boot
			}
		}
		stats.replays++;
	}
	uint8_t done = 0;
	wr(m->journal_addr + J_DONE, &done, 1);
}

storage_slot_t storage_slot_check(const uint8_t r[STORAGE_SLOT_SIZE])
{
	bool erased = true;
	for (unsigned i = 0; i < STORAGE_SLOT_SIZE && erased; i++) {
		erased = r[i] == 0xFF;
	}
	if (erased) {
		return STORAGE_SLOT_FREE;
	}
	if (get32(&r[0]) != STORAGE_SLOT_MAGIC) {
		// A save cut inside the magic: the bytes programmed so far are on their way to it (programming only
		// clears bits) and everything after them is still erased
		static const uint8_t magic[4] = { 'O', 'S', 'E', 'T' };
		unsigned i = 0;
		while (i < 4 && r[i] == magic[i]) {
			i++;
		}
		if ((r[i] & magic[i]) != magic[i]) {
			return STORAGE_SLOT_OTHER;
		}
		for (unsigned k = i + 1; k < STORAGE_SLOT_SIZE; k++) {
			if (r[k] != 0xFF) {
				return STORAGE_SLOT_OTHER;
			}
		}
		return STORAGE_SLOT_TORN;
	}
	uint16_t n = (uint16_t)(r[10] | (r[11] << 8));
	if (get32(&r[STORAGE_SLOT_COMMIT]) != 0 || n > STORAGE_SLOT_PAYLOAD_MAX ||
	    get32(&r[STORAGE_SLOT_HDR + n]) != storage_crc32(0, r, STORAGE_SLOT_HDR + n)) {
		return STORAGE_SLOT_TORN;
	}
	return STORAGE_SLOT_VALID;
}

// Whether len bytes at addr are all erased; false also when unreadable
static bool blank(uint32_t addr, uint32_t len)
{
	uint8_t c[CHUNK];
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!rd(addr + off, c, n)) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (c[i] != 0xFF) {
				return false;
			}
		}
	}
	return true;
}

// An ownership mark: a valid settings slot of layout 0 (which no settings layout uses, so the settings load their
// defaults past it) in the first free slot of the ring. Written while the region holds no valid record of ours, so
// from then on one always exists (saves never erase the sector holding the newest record) and a cut in the middle
// of a journal rewrite (J0 erased, J1 full) can't make the region look foreign at the next boot
static void mark(const hal_flash_map_t *m)
{
	static uint8_t r[STORAGE_SLOT_SIZE];
	for (uint32_t a = m->settings_addr; a + STORAGE_SLOT_SIZE <= m->settings_addr + m->settings_size;
	     a += STORAGE_SLOT_SIZE) {
		if (!rd(a, r, sizeof(r)) || storage_slot_check(r) != STORAGE_SLOT_FREE) {
			continue;
		}
		memset(r, 0xFF, sizeof(r));
		put32(&r[0], STORAGE_SLOT_MAGIC);
		put32(&r[4], 0);		// sequence 0: any real record is newer
		r[8] = r[9] = 0;		// layout 0
		r[10] = r[11] = 0;		// no payload
		put32(&r[STORAGE_SLOT_HDR], storage_crc32(0, r, STORAGE_SLOT_HDR));
		static const uint8_t commit[4] = { 0, 0, 0, 0 };
		if (wr(a, r, STORAGE_SLOT_HDR + 4u)) {
			wr(a + STORAGE_SLOT_COMMIT, commit, sizeof(commit));
		}
		return;
	}
}

// Classify the settings ring and the journal (storage.h "Foreign region guard")
static storage_region_t scan_region(const hal_flash_map_t *m)
{
	static uint8_t r[STORAGE_SLOT_SIZE];	// under the storage lock (off the start-up task's stack)
	bool ours = false;			// one valid record of ours somewhere
	uint16_t other = 0;

	for (uint32_t a = m->settings_addr; a + STORAGE_SLOT_SIZE <= m->settings_addr + m->settings_size;
	     a += STORAGE_SLOT_SIZE) {
		storage_slot_t s = rd(a, r, sizeof(r)) ? storage_slot_check(r) : STORAGE_SLOT_OTHER;
		ours = ours || s == STORAGE_SLOT_VALID;
		other = (uint16_t)(other + (s == STORAGE_SLOT_OTHER));
	}
	// J0: erased, or our header (a torn one counts as ours-looking), and nothing after the done byte
	uint8_t h[P_DATA];
	bool j0_blank = blank(m->journal_addr, STORAGE_SECTOR);
	if (!j0_blank) {
		bool hdr = rd(m->journal_addr, h, sizeof(h)) && get32(&h[0]) == J_MAGIC;
		bool valid = hdr && get32(&h[12]) == storage_crc32(0, h, 12);
		uint32_t used = J_HDR_LEN + 1u;
		if (!hdr && get32(&h[0]) == P_MAGIC) {
			// A patch header: valid when its CRC matches; torn (a cut while it was programmed): ours-looking,
			// but then where its data ends isn't known, so the rest isn't checked
			used = patch_len(m, h);
			hdr = true;
			valid = used != 0;
			used = valid ? used : STORAGE_SECTOR;
		}
		ours = ours || valid;
		if (!hdr || !blank(m->journal_addr + used, STORAGE_SECTOR - used)) {
			other++;
		}
	}
	// J1: a sector copy, any content, but only after our J0 header has been written at least once
	if (j0_blank && !blank(m->journal_addr + STORAGE_SECTOR, STORAGE_SECTOR)) {
		other++;
	}
	stats.foreign_slots = other;
	if (other != 0 && !ours) {
		return STORAGE_REGION_FOREIGN;
	}
	if (!ours) {
		mark(m);
	}
	return STORAGE_REGION_OURS;
}

bool storage_init(void)
{
	const hal_flash_map_t *m = storage_map();

	ready = false;
	region = STORAGE_REGION_NONE;
	claim_pending = false;
	if (m == NULL || board.flash->size == 0 || board.flash->erase_size != STORAGE_SECTOR ||
	    board.flash->write == NULL || board.flash->erase == NULL) {
		return false;
	}
	ready = true;
	storage_lock();
	region = scan_region(m);
	if (region == STORAGE_REGION_OURS) {
		replay();
	}
	storage_unlock();
	return region == STORAGE_REGION_OURS;
}

bool storage_ready(void)
{
	return ready && region == STORAGE_REGION_OURS;
}

storage_region_t storage_region(void)
{
	return region;
}

bool storage_claim_request(void)
{
	if (region != STORAGE_REGION_FOREIGN) {
		return false;
	}
	claim_pending = true;
	return true;
}

bool storage_claim_pending(void)
{
	return claim_pending;
}

bool storage_claim(void)
{
	const hal_flash_map_t *m = storage_map();
	bool ok = true;

	storage_lock();
	claim_pending = false;
	if (region != STORAGE_REGION_FOREIGN) {
		storage_unlock();
		return false;
	}
	for (uint32_t a = m->settings_addr; ok && a < m->settings_addr + m->settings_size; a += STORAGE_SECTOR) {
		ok = er(a) && blank(a, STORAGE_SECTOR);
	}
	for (uint32_t a = m->journal_addr; ok && a < m->journal_addr + 2u * STORAGE_SECTOR; a += STORAGE_SECTOR) {
		ok = er(a) && blank(a, STORAGE_SECTOR);
	}
	if (ok) {
		region = STORAGE_REGION_OURS;
		stats.foreign_slots = 0;
		mark(m);
	}
	storage_unlock();
	return ok;
}

static bool inside(uint32_t addr, uint32_t len, uint32_t base, uint32_t size)
{
	return size != 0 && addr >= base && len <= size && addr - base <= size - len;
}

bool storage_debug_read_allowed(uint32_t addr, uint32_t len)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || len == 0 || addr + len < addr) {
		return false;
	}
	return inside(addr, len, m->settings_addr, m->settings_size) ||
	       inside(addr, len, m->journal_addr, 2u * STORAGE_SECTOR) ||
	       inside(addr, len, m->calibration_addr, m->calibration_size);
}

// A write refused because the region is foreign (counted), or because there is no flash
static bool refused(void)
{
	if (ready && region == STORAGE_REGION_FOREIGN) {
		stats.foreign_refusals++;
	}
	return false;
}

bool storage_sector_write(uint32_t addr, const uint8_t *data)
{
	const hal_flash_map_t *m = storage_map();
	bool ok = false;

	if (!storage_ready() || !target_ok(m, addr)) {
		return refused();
	}
	storage_lock();
	if (same(addr, data, STORAGE_SECTOR)) {
		stats.sector_skips++;
		storage_unlock();
		return true;
	}
	if (board.flash->replace != NULL) {
		// Copy on write underneath: the new sector replaces the old in one step, so the journal's copy, header and
		// the erase would only wear the flash (on NAND, 11 page programs against 2)
		ok = board.flash->replace(addr, data) && same(addr, data, STORAGE_SECTOR);
		stats.sector_writes += ok;
		if (!ok) {
			stats.errors++;
		}
		storage_unlock();
		return ok;
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
	if (er(j1) && wr(j1, data, STORAGE_SECTOR) && flash_crc(j1, STORAGE_SECTOR, 0, &check) && check == crc &&
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
	const hal_flash_map_t *m = storage_map();
	const uint8_t *p = buf;

	if (!storage_ready()) {
		return refused();
	}
	if (len == 0 || addr + len < addr || addr + len > board.flash->size) {
		return len == 0;
	}
	storage_lock();
	uint8_t *b = storage_buffer_acquire();
	bool ok = b != NULL;
	if (ok && len <= P_MAX) {
		// One transaction for the one or two sectors it touches (above)
		if (same(addr, p, len)) {
			stats.sector_skips++;
		} else {
			memset(b, 0xFF, P_DATA);
			put32(&b[0], P_MAGIC);
			put32(&b[4], addr);
			put32(&b[8], len);
			put32(&b[12], storage_crc32(storage_crc32(0, b, 12), p, len));
			memcpy(b + P_DATA, p, len);
			ok = er(m->journal_addr) && wr(m->journal_addr, b, P_DATA + len) &&
			     same(m->journal_addr, b, P_DATA + len) && patch_finish(m, addr, len, b);
		}
		len = 0;
	}
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

storage_native_t storage_native_erase(void)
{
	const hal_flash_map_t *m = storage_map();
	if (!storage_ready() || m == NULL || m->native_size == 0 || (m->native_addr % STORAGE_SECTOR) != 0 ||
	    (m->native_size % STORAGE_SECTOR) != 0 || m->native_addr + m->native_size > board.flash->size) {
		return STORAGE_NATIVE_FAILED;
	}
	storage_native_t r = STORAGE_NATIVE_FAILED;
	uint8_t h[4];
	storage_lock();
	if (!rd(m->native_addr, h, sizeof(h))) {
		goto out;
	}
	if (get32(h) != STORAGE_NATIVE_MAGIC) {
		r = blank(m->native_addr, m->native_size) ? STORAGE_NATIVE_BLANK : STORAGE_NATIVE_FOREIGN;
		goto out;
	}
	for (uint32_t a = m->native_addr + m->native_size; a > m->native_addr; ) {
		a -= STORAGE_SECTOR;		// from the end: the header sector last
		if (!blank(a, STORAGE_SECTOR) && !er(a)) {
			goto out;
		}
	}
	r = blank(m->native_addr, STORAGE_SECTOR) ? STORAGE_NATIVE_ERASED : STORAGE_NATIVE_FAILED;
out:
	storage_unlock();
	return r;
}

static storage_job_fn volatile job_fn;	// set last by the poster, cleared by the housekeeping task when done
static void *volatile job_ctx;

bool storage_job_post(storage_job_fn fn, void *ctx)
{
	if (fn == NULL || job_fn != NULL) {
		return false;
	}
	job_ctx = ctx;
	job_fn = fn;
	return true;
}

bool storage_job_busy(void)
{
	return job_fn != NULL;
}

void storage_job_poll(void)
{
	storage_job_fn fn = job_fn;
	if (fn != NULL) {
		fn(job_ctx);
		job_fn = NULL;
	}
}

void storage_get_stats(storage_stats_t *s)
{
	*s = stats;
	s->claim_pending = claim_pending;
}
