// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NOR-like 4 KB sectors over NAND (nand_sectors.h, docs/spec/nand-sectors.md).
//
// A record is the content of one sector at one moment: two pages (half 0, half 1), or one page that says the sector is
// erased. Every page carries META_LEN bytes in its spare area at META_COL:
//   0  u32 sequence number (one per record, both halves the same; it only grows)
//   4  u16 logical sector     6  u8 half (0, 1)     7  u8 kind (KIND_DATA, KIND_ERASED)
//   8  u32 CRC-32 of the page's 2048 data bytes
//   12 u32 CRC-32 of META_MAGIC and bytes 0-11: a page whose spare area doesn't check isn't ours
// Pages are written in one stream: one open block at a time, filled in page order, every record numbered after the
// last. So blocks ordered by their first sequence number, pages in page order, are records oldest first, and a
// sector's current record is the last complete one seen. Nothing is ever overwritten in place: a rewrite programs a
// new record and only then moves the map, and a block is erased only once nothing current is left in it.
#include <string.h>
#include "nand_sectors.h"

#define P		NAND_SECTORS_PAGE
#define META_SPARE	8u		// offset in the spare area: bytes 0-7 stay erased (the factory bad-block mark is
					// byte 0; a stock firmware keeps its own block marks at bytes 4-5)
#define META_COL	(P + META_SPARE)
#define META_LEN	16u
#define META_MAGIC	0x314C534Eu	// "NSL1"
#define KIND_DATA	0x5Du
#define KIND_ERASED	0xE5u

// map entries: the record's first page in the region, ERASED_BIT for an erase record, NONE for a sector never written
// (or whose erase record isn't needed any more)
#define NONE		0xFFFFu
#define ERASED_BIT	0x8000u
#define PAGE_MASK	0x7FFFu

// Block flags
#define F_BAD		0x01u	// bad: never touched
#define F_RETIRE	0x02u	// a program in it failed: emptied first, then retired
#define F_CLEAN		0x04u	// erased this session and nothing written since
#define F_OURS		0x08u	// the mount found records of ours in it
#define F_DONE		0x10u	// the mount has scanned it
#define F_FOREIGN	0x20u	// holds data the layer didn't write: never touched

#define CHUNK		128u
#define ATTEMPTS	3u
// Static wear leveling: after this many blocks opened, the block holding the oldest data is emptied into the stream,
// so blocks full of data that never changes (a codeplug) take their turn too. Costs at most one block's copy per
// LEVEL_EVERY blocks written
#define LEVEL_EVERY	16u

typedef struct {
	uint32_t seq;
	uint16_t sector;
	uint8_t half;
	uint8_t kind;
	uint32_t dcrc;
} meta_t;

typedef enum { META_BLANK, META_VALID, META_INVALID } meta_state_t;

// CRC-32 (IEEE 802.3, reflected), four bits at a time: a 64-byte table instead of 1 KB, still fast enough for a
// page per millisecond
static const uint32_t crc_nibble[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
	0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static uint32_t crc_raw(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	while (len-- > 0) {
		crc ^= *p++;
		crc = (crc >> 4) ^ crc_nibble[crc & 15u];
		crc = (crc >> 4) ^ crc_nibble[crc & 15u];
	}
	return crc;
}

static uint32_t crc_ff(uint32_t crc, uint32_t len)
{
	static const uint8_t ff[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	while (len > 0) {
		uint32_t n = len < sizeof(ff) ? len : sizeof(ff);
		crc = crc_raw(crc, ff, n);
		len -= n;
	}
	return crc;
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

static uint32_t meta_crc(const uint8_t *b)
{
	uint8_t magic[4];
	put32(magic, META_MAGIC);
	return ~crc_raw(crc_raw(~0u, magic, 4), b, 12);
}

static void meta_encode(const meta_t *m, uint8_t b[META_LEN])
{
	put32(&b[0], m->seq);
	b[4] = (uint8_t)m->sector;
	b[5] = (uint8_t)(m->sector >> 8);
	b[6] = m->half;
	b[7] = m->kind;
	put32(&b[8], m->dcrc);
	put32(&b[12], meta_crc(b));
}

static uint32_t ppb(const nand_sectors_t *ns)
{
	return ns->dev->pages_per_block;
}

static bool excluded(const nand_sectors_t *ns, uint32_t page)
{
	for (unsigned i = 0; i < ns->excluded_count; i++) {
		if (ns->excluded[i] == page) {
			return true;
		}
	}
	return false;
}

static void busy(const nand_sectors_t *ns)
{
	if (ns->dev->busy != NULL) {
		ns->dev->busy(ns->dev->ctx);
	}
}

static meta_state_t read_meta(nand_sectors_t *ns, uint32_t page, meta_t *m)
{
	uint8_t b[META_LEN];
	if (!ns->dev->read(ns->dev->ctx, page, META_COL, b, sizeof(b))) {
		return META_INVALID;		// ECC couldn't correct it: torn or worn
	}
	bool blank = true;
	for (unsigned i = 0; i < sizeof(b) && blank; i++) {
		blank = b[i] == 0xFF;
	}
	if (blank) {
		return META_BLANK;
	}
	if (get32(&b[12]) != meta_crc(b) || (b[7] != KIND_DATA && b[7] != KIND_ERASED) || b[6] > 1) {
		return META_INVALID;
	}
	m->seq = get32(&b[0]);
	m->sector = (uint16_t)(b[4] | (b[5] << 8));
	m->half = b[6];
	m->kind = b[7];
	m->dcrc = get32(&b[8]);
	return META_VALID;
}

// CRC-32 of a page's data as the chip reads it now
static bool page_crc(nand_sectors_t *ns, uint32_t page, uint32_t *out)
{
	uint8_t c[CHUNK];
	uint32_t crc = ~0u;
	for (uint32_t col = 0; col < P; col += CHUNK) {
		if (!ns->dev->read(ns->dev->ctx, page, (uint16_t)col, c, CHUNK)) {
			return false;
		}
		crc = crc_raw(crc, c, CHUNK);
	}
	*out = ~crc;
	return true;
}

static bool is_data(uint16_t e)
{
	return e != NONE && (e & ERASED_BIT) == 0;
}

static uint32_t entry_page(uint16_t e)
{
	return e & PAGE_MASK;
}

static uint8_t entry_pages(uint16_t e)
{
	return e == NONE ? 0 : is_data(e) ? 2 : 1;
}

// --- Blocks ---

static bool usable(const nand_sectors_t *ns, uint16_t b)
{
	return (ns->flags[b] & (F_BAD | F_RETIRE | F_FOREIGN)) == 0;
}

static bool is_free(const nand_sectors_t *ns, uint16_t b)
{
	return usable(ns, b) && ns->live[b] == 0 && b != ns->open;
}

static uint16_t free_blocks(const nand_sectors_t *ns)
{
	uint16_t n = 0;
	for (uint16_t b = 0; b < ns->dev->blocks; b++) {
		n = (uint16_t)(n + is_free(ns, b));
	}
	return n;
}

static void retire(nand_sectors_t *ns, uint16_t b)
{
	ns->dev->mark_bad(ns->dev->ctx, b);
	ns->flags[b] = F_BAD;
	ns->stats.retired++;
}

// --- Foreign data ---
//
// The region may hold data someone else wrote (a firmware that used these blocks). The layer never erases it: before
// a block that isn't known to be ours is erased, its first and last pages are looked at. Stock firmware writes with
// the chip's ECC on, so its pages read back clean; a page of ours cut by a power cut or a torn erase reads back
// uncorrectable, blank or as a valid record.

typedef enum { BLOCK_BLANK, BLOCK_OURS, BLOCK_TORN, BLOCK_FOREIGN } block_kind_t;
typedef enum { PAGE_BLANK, PAGE_OURS, PAGE_UNREADABLE, PAGE_OTHER } page_kind_t;

static page_kind_t page_kind(nand_sectors_t *ns, uint32_t page)
{
	meta_t m;
	meta_state_t st = read_meta(ns, page, &m);
	if (st == META_VALID) {
		return PAGE_OURS;
	}
	// The whole page, data and spare area, must be erased to count as blank
	uint8_t c[CHUNK];
	uint32_t end = P + ns->dev->spare_size;
	for (uint32_t col = 0; col < end; col += CHUNK) {
		uint32_t n = end - col < CHUNK ? end - col : CHUNK;
		if (!ns->dev->read(ns->dev->ctx, page, (uint16_t)col, c, n)) {
			return PAGE_UNREADABLE;
		}
		for (uint32_t i = 0; i < n; i++) {
			if (c[i] != 0xFFu) {
				return PAGE_OTHER;
			}
		}
	}
	return st == META_INVALID ? PAGE_UNREADABLE : PAGE_BLANK;
}

// The layer erases every block itself before it programs the first page, so a valid record on page 0 or page 1
// means the block is ours (page 0 may be one of ours that read back wrong). Otherwise a readable, programmed first or
// last page without our spare area is someone else's. An unreadable page is what a power cut leaves of ours
static block_kind_t block_kind(nand_sectors_t *ns, uint16_t b)
{
	uint32_t first = (uint32_t)b * ppb(ns);
	page_kind_t k0 = page_kind(ns, first);
	meta_t m;
	if (k0 == PAGE_OURS || read_meta(ns, first + 1u, &m) == META_VALID) {
		return BLOCK_OURS;
	}
	page_kind_t kl = page_kind(ns, first + ppb(ns) - 1u);
	if (k0 == PAGE_OTHER || kl == PAGE_OTHER) {
		return BLOCK_FOREIGN;
	}
	if (kl == PAGE_OURS) {
		return BLOCK_OURS;
	}
	return k0 == PAGE_BLANK && kl == PAGE_BLANK ? BLOCK_BLANK : BLOCK_TORN;
}

static void found_foreign(nand_sectors_t *ns, uint16_t b)
{
	if (ns->stats.foreign == 0) {
		ns->stats.foreign_block = b;
	}
	ns->stats.foreign++;
	ns->flags[b] |= F_FOREIGN;
}

// Erase a block that holds nothing current
static void release(nand_sectors_t *ns, uint16_t b)
{
	if (ns->flags[b] & F_RETIRE) {
		retire(ns, b);
		return;
	}
	nand_result_t r = ns->dev->erase(ns->dev->ctx, b);
	ns->stats.erases++;
	if (r == NAND_OK) {
		ns->flags[b] = F_CLEAN;
	} else if (r == NAND_FAILED) {
		retire(ns, b);
	} else {
		ns->stats.errors++;
		ns->flags[b] &= (uint8_t)~F_CLEAN;	// erased again when it's next used
	}
}

// Open the next free block after the last one (wear goes round the region), erased unless it is already
static bool open_block(nand_sectors_t *ns)
{
	uint16_t blocks = ns->dev->blocks;
	for (uint16_t i = 1; i <= blocks; i++) {
		uint16_t b = (uint16_t)((ns->rotor + i) % blocks);
		if (!is_free(ns, b)) {
			continue;
		}
		if (!(ns->flags[b] & F_CLEAN)) {
			// Checked again just before the erase, so a block can't be lost to a read error at the mount
			if (!(ns->flags[b] & F_OURS) && block_kind(ns, b) == BLOCK_FOREIGN) {
				found_foreign(ns, b);
				continue;
			}
			release(ns, b);
			if (!(ns->flags[b] & F_CLEAN)) {
				continue;
			}
		}
		ns->flags[b] &= (uint8_t)~F_CLEAN;
		ns->rotor = b;
		ns->open = b;
		ns->next = 0;
		ns->order[b] = ns->seq;
		ns->opened++;
		return true;
	}
	return false;
}

static void close_block(nand_sectors_t *ns)
{
	ns->open = NONE;
}

// The old record of sector s gives way to the new one: map and live counts
static void commit(nand_sectors_t *ns, uint16_t s, uint16_t entry)
{
	uint16_t old = ns->map[s];
	if (old != NONE) {
		ns->live[entry_page(old) / ppb(ns)] = (uint8_t)(ns->live[entry_page(old) / ppb(ns)] - entry_pages(old));
	}
	ns->map[s] = entry;
	ns->live[entry_page(entry) / ppb(ns)] = (uint8_t)(ns->live[entry_page(entry) / ppb(ns)] + entry_pages(entry));
}

// Program one page of a record and read it back: the spare area exactly, the data by its CRC (unless it's an erase
// record). NAND_FAILED from the chip retires the block; a page that reads back wrong is only skipped
static nand_result_t put_page(nand_sectors_t *ns, uint32_t page, uint32_t src, const nand_chunk_t *data,
			      const meta_t *m)
{
	uint8_t b[META_LEN], back[META_LEN];
	nand_chunk_t c[2];
	unsigned n = 0;
	meta_encode(m, b);
	if (data != NULL) {
		c[n++] = *data;
	}
	c[n].column = META_COL;
	c[n].len = META_LEN;
	c[n].data = b;
	n++;
	nand_result_t r = ns->dev->program(ns->dev->ctx, page, src, c, n);
	if (r != NAND_OK) {
		ns->stats.errors++;
		return r;
	}
	uint32_t crc;
	if (!ns->dev->read(ns->dev->ctx, page, META_COL, back, sizeof(back)) || memcmp(b, back, sizeof(b)) != 0 ||
	    (m->kind == KIND_DATA && (!page_crc(ns, page, &crc) || crc != m->dcrc))) {
		ns->stats.verify_fails++;
		ns->stats.errors++;
		return NAND_ERROR;
	}
	return NAND_OK;
}

static bool room(nand_sectors_t *ns, uint16_t pages, bool collect);
static bool put_erased(nand_sectors_t *ns, uint16_t s, bool may_collect);

// A program in the open block went wrong. The block takes no more pages: the page may have been left looking blank,
// and the mount stops reading a block at its first blank page. If the chip said the program failed, the block is
// emptied and retired
static void failed(nand_sectors_t *ns, nand_result_t r)
{
	uint16_t b = ns->open;
	if (r == NAND_FAILED) {
		ns->flags[b] |= F_RETIRE;
	}
	close_block(ns);
	// A first page that went wrong would make the block look foreign to the next mount (programmed, maybe readable,
	// without a valid spare area). Nothing else is in the block yet: erase it (or retire it) now
	if (ns->next == 0) {
		release(ns, b);
	}
}

// Copy a current record (first page src, pages 1 or 2) to the open block under a new sequence number, the data moved
// inside the chip
static bool move_record(nand_sectors_t *ns, uint16_t s, uint32_t src, uint8_t pages)
{
	meta_t m[2];
	for (uint8_t i = 0; i < pages; i++) {
		if (read_meta(ns, src + i, &m[i]) != META_VALID) {
			ns->stats.errors++;
			return false;
		}
	}
	for (unsigned attempt = 0; attempt < ATTEMPTS; attempt++) {
		if (!room(ns, pages, false)) {
			return false;
		}
		uint32_t dst = ns->open * ppb(ns) + ns->next;
		uint32_t seq = ns->seq++;
		nand_result_t r = NAND_OK;
		for (uint8_t i = 0; i < pages && r == NAND_OK; i++) {
			m[i].seq = seq;
			r = put_page(ns, dst + i, src + i, NULL, &m[i]);
		}
		if (r == NAND_OK) {
			ns->next = (uint16_t)(ns->next + pages);
			commit(ns, s, (uint16_t)(dst | (pages == 1 ? ERASED_BIT : 0)));
			ns->stats.gc_copies++;
			return true;
		}
		failed(ns, r);
	}
	return false;
}

// Move every current record out of block v, then erase it
static bool evacuate(nand_sectors_t *ns, uint16_t v)
{
	uint32_t first = v * ppb(ns);
	for (uint32_t p = 0; p < ppb(ns) && ns->live[v] > 0; p++) {
		busy(ns);
		meta_t m;
		meta_state_t st = read_meta(ns, first + p, &m);
		if (st == META_BLANK) {
			break;
		}
		if (st != META_VALID || m.sector >= ns->sectors || m.half != 0) {
			continue;
		}
		uint16_t e = ns->map[m.sector];
		if (e == NONE || entry_page(e) != first + p) {
			continue;		// superseded
		}
		if (!move_record(ns, m.sector, first + p, entry_pages(e))) {
			return false;
		}
	}
	if (ns->live[v] != 0) {
		return false;
	}
	release(ns, v);
	ns->stats.gc_blocks++;
	return true;
}

// Garbage collection: while too few blocks are free, empty the block with the fewest current pages (a block marked
// for retirement first)
static void collect(nand_sectors_t *ns)
{
	while (free_blocks(ns) <= NAND_SECTORS_RESERVE) {
		busy(ns);
		uint16_t v = NONE;
		for (uint16_t b = 0; b < ns->dev->blocks; b++) {
			if (b == ns->open || (ns->flags[b] & F_BAD)) {
				continue;
			}
			bool retiring = (ns->flags[b] & F_RETIRE) != 0;
			if (ns->live[b] == 0) {
				if (retiring) {
					retire(ns, b);
				}
				continue;
			}
			if (v != NONE && (ns->flags[v] & F_RETIRE)) {
				continue;		// a block to retire goes first
			}
			if (v == NONE || retiring || ns->live[b] < ns->live[v]) {
				v = b;
			}
		}
		if (v == NONE || (!(ns->flags[v] & F_RETIRE) && ns->live[v] >= ppb(ns)) || !evacuate(ns, v)) {
			return;			// nothing to gain, or the copy failed: the writes use what is left
		}
	}
}

// Static wear leveling (LEVEL_EVERY)
static void level(nand_sectors_t *ns)
{
	if ((uint16_t)(ns->opened - ns->leveled) < LEVEL_EVERY) {
		return;
	}
	ns->leveled = ns->opened;
	uint16_t v = NONE;
	for (uint16_t b = 0; b < ns->dev->blocks; b++) {
		if (b != ns->open && usable(ns, b) && ns->live[b] > 0 && (v == NONE || ns->order[b] < ns->order[v])) {
			v = b;
		}
	}
	if (v != NONE && evacuate(ns, v)) {
		ns->stats.leveled++;
	}
}

// pages free pages in the open block: a new block when it hasn't, after garbage collection if allowed
static bool room(nand_sectors_t *ns, uint16_t pages, bool may_collect)
{
	if (ns->open != NONE && ns->next + pages <= ppb(ns)) {
		return true;
	}
	close_block(ns);
	if (may_collect) {
		collect(ns);
		level(ns);
		if (ns->open != NONE && ns->next + pages <= ppb(ns)) {
			return true;		// the block the collection's copies went to has room left
		}
		close_block(ns);
	}
	return open_block(ns);
}

// --- Mount ---

static void mount_scan(nand_sectors_t *ns)
{
	const nand_dev_t *d = ns->dev;
	uint32_t max_seq = 0;
	bool any = false;

	for (uint16_t s = 0; s < ns->sectors; s++) {
		ns->map[s] = NONE;
	}
	// Each block's first sequence number (bad blocks were found before)
	for (uint16_t b = 0; b < d->blocks; b++) {
		ns->live[b] = 0;
		ns->flags[b] &= (uint8_t)~(F_OURS | F_DONE);
		ns->order[b] = 0xFFFFFFFFu;
		if (ns->flags[b] & F_BAD) {
			continue;
		}
		busy(ns);
		for (uint32_t p = 0; p < ppb(ns); p++) {
			meta_t m;
			meta_state_t st = read_meta(ns, b * ppb(ns) + p, &m);
			ns->stats.mount_reads++;
			if (st == META_BLANK) {
				break;
			}
			if (st == META_VALID) {
				ns->order[b] = m.seq;
				ns->flags[b] |= F_OURS;
				break;
			}
		}
	}
	// The blocks oldest first, each page in order: a complete record replaces what the map had for its sector
	for (;;) {
		uint16_t b = NONE;
		for (uint16_t i = 0; i < d->blocks; i++) {
			if ((ns->flags[i] & (F_OURS | F_DONE)) == F_OURS && (b == NONE || ns->order[i] < ns->order[b])) {
				b = i;
			}
		}
		if (b == NONE) {
			break;
		}
		ns->flags[b] |= F_DONE;
		busy(ns);
		ns->rotor = b;		// the newest block: allocation goes on after it
		uint32_t first = b * ppb(ns);
		for (uint32_t p = 0; p < ppb(ns); p++) {
			meta_t m, m1;
			meta_state_t st = read_meta(ns, first + p, &m);
			ns->stats.mount_reads++;
			if (st == META_BLANK) {
				break;
			}
			if (st != META_VALID) {
				continue;
			}
			if (!any || (int32_t)(m.seq - max_seq) > 0) {
				max_seq = m.seq;
				any = true;
			}
			if (excluded(ns, first + p) || m.sector >= ns->sectors || m.half != 0) {
				continue;
			}
			if (m.kind == KIND_ERASED) {
				// Needed only while an older record of the sector is somewhere in the region
				ns->map[m.sector] = ns->map[m.sector] == NONE ? NONE : (uint16_t)((first + p) | ERASED_BIT);
				continue;
			}
			if (p + 1 >= ppb(ns) || excluded(ns, first + p + 1)) {
				continue;
			}
			meta_state_t st1 = read_meta(ns, first + p + 1, &m1);
			ns->stats.mount_reads++;
			if (st1 == META_VALID && m1.seq == m.seq && m1.sector == m.sector && m1.half == 1 &&
			    m1.kind == KIND_DATA) {
				ns->map[m.sector] = (uint16_t)(first + p);
				p++;
				if ((int32_t)(m1.seq - max_seq) > 0) {
					max_seq = m1.seq;
				}
			}
		}
	}
	ns->seq = any ? max_seq + 1u : 1u;
	for (uint16_t s = 0; s < ns->sectors; s++) {
		uint16_t e = ns->map[s];
		if (e != NONE) {
			ns->live[entry_page(e) / ppb(ns)] = (uint8_t)(ns->live[entry_page(e) / ppb(ns)] + entry_pages(e));
		}
	}
}

// Every current data record read whole: a page whose data doesn't match its CRC was torn by a power cut while it
// was programmed (or has decayed). It is set aside and the scan runs again, so the record before it counts. True if
// one was found
static bool mount_verify(nand_sectors_t *ns)
{
	for (uint16_t s = 0; s < ns->sectors; s++) {
		uint16_t e = ns->map[s];
		if (!is_data(e)) {
			continue;
		}
		busy(ns);
		for (uint32_t h = 0; h < 2; h++) {
			uint32_t page = entry_page(e) + h, crc;
			meta_t m;
			if (read_meta(ns, page, &m) != META_VALID || !page_crc(ns, page, &crc) || crc != m.dcrc) {
				if (ns->excluded_count >= NAND_SECTORS_EXCLUDE) {
					return false;	// more than a power cut leaves: take the map as it is
				}
				ns->excluded[ns->excluded_count++] = (uint16_t)page;
				ns->stats.torn++;
				return true;
			}
		}
	}
	return false;
}

// A torn record stays its sector's newest until something newer is written: the content the mount fell back to is
// written again (or an erase record if there was none), so the next boots don't trip over the torn one
static void mount_fixup(nand_sectors_t *ns)
{
	for (unsigned i = 0; i < ns->excluded_count; i++) {
		meta_t m;
		if (read_meta(ns, ns->excluded[i], &m) != META_VALID || m.sector >= ns->sectors) {
			continue;
		}
		bool done = false;	// both pages of one record torn: one fix-up
		for (unsigned k = 0; k < i && !done; k++) {
			meta_t mk;
			done = read_meta(ns, ns->excluded[k], &mk) == META_VALID && mk.sector == m.sector;
		}
		if (done) {
			continue;
		}
		// Room first, with garbage collection like any write: the mount left the newest block closed, and the
		// region may have no block free otherwise. The record may move in the collection, so the map is read after
		uint16_t e = ns->map[m.sector];
		if (!room(ns, is_data(e) ? 2 : 1, true)) {
			continue;
		}
		e = ns->map[m.sector];
		if (is_data(e)) {
			move_record(ns, m.sector, entry_page(e), 2);
		} else {
			put_erased(ns, m.sector, false);
		}
	}
}

bool nand_sectors_mount(nand_sectors_t *ns)
{
	const nand_dev_t *d = ns->dev;
	ns->mounted = false;
	memset(&ns->stats, 0, sizeof(ns->stats));
	ns->stats.status = NAND_SECTORS_BAD_SETUP;
	if (d == NULL || d->pages_per_block < 2 || d->pages_per_block > 255 || d->blocks == 0 ||
	    (uint32_t)d->blocks * d->pages_per_block > PAGE_MASK || d->spare_size < META_SPARE + META_LEN ||
	    ns->sectors == 0 || ns->map == NULL || ns->live == NULL || ns->flags == NULL || ns->order == NULL) {
		return false;
	}
	ns->excluded_count = 0;
	ns->open = NONE;
	ns->next = 0;
	ns->opened = 0;
	ns->leveled = 0;
	ns->rotor = (uint16_t)(d->blocks - 1u);
	// Foreign data anywhere in the region: no mount, nothing programmed or erased. The owner decides what to do
	for (uint16_t b = 0; b < d->blocks; b++) {
		busy(ns);
		bool bad = false;
		if (!d->block_bad(d->ctx, b, &bad)) {
			bad = true;		// unreadable: left alone
		}
		ns->flags[b] = bad ? F_BAD : 0;
		if (!bad && block_kind(ns, b) == BLOCK_FOREIGN) {
			found_foreign(ns, b);
		}
	}
	if (ns->stats.foreign != 0) {
		ns->stats.status = NAND_SECTORS_FOREIGN;
		return false;
	}
	do {
		mount_scan(ns);
	} while (mount_verify(ns));
	// Blocks that hold records of ours but nothing current: erased now, so the next boot doesn't read them again.
	// Blocks with nothing of ours in them are erased when they are next used
	for (uint16_t b = 0; b < d->blocks; b++) {
		if ((ns->flags[b] & F_OURS) && ns->live[b] == 0 && usable(ns, b)) {
			release(ns, b);
		}
	}
	mount_fixup(ns);
	// Writes start in a fresh block: the last page of the newest one may have been cut part way, looking blank, and
	// a page must not be programmed twice
	ns->mounted = true;
	ns->stats.status = NAND_SECTORS_MOUNTED;
	return true;
}

uint32_t nand_sectors_size(const nand_sectors_t *ns)
{
	return (uint32_t)ns->sectors * NAND_SECTORS_SECTOR;
}

// --- Read, write, erase ---

static bool range_ok(const nand_sectors_t *ns, uint32_t addr, uint32_t len)
{
	uint32_t size = nand_sectors_size(ns);
	return ns->mounted && addr <= size && len <= size - addr;
}

bool nand_sectors_read(nand_sectors_t *ns, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *out = buf;
	if (!range_ok(ns, addr, len)) {
		return false;
	}
	while (len > 0) {
		uint16_t e = ns->map[addr / NAND_SECTORS_SECTOR];
		uint32_t off = addr % P;
		uint32_t n = P - off < len ? P - off : len;
		if (!is_data(e)) {
			memset(out, 0xFF, n);
		} else {
			uint32_t page = entry_page(e) + (addr % NAND_SECTORS_SECTOR) / P;
			if (!ns->dev->read(ns->dev->ctx, page, (uint16_t)off, out, n)) {
				ns->stats.errors++;
				return false;
			}
		}
		addr += n;
		out += n;
		len -= n;
	}
	return true;
}

// The new content of half h of a sector: its CRC, whether it differs from the old, and whether the write is legal
// (programs only clear bits; not checked for a replace, which may set any bit). Bytes [lo, hi) of the half (offsets in
// it) come from data
static bool half_crc(nand_sectors_t *ns, uint16_t e, uint32_t h, uint32_t lo, uint32_t hi, const uint8_t *data,
		     uint32_t *crc_out, bool *changed, bool replace)
{
	if (is_data(e) && lo >= hi) {
		meta_t m;
		if (read_meta(ns, entry_page(e) + h, &m) != META_VALID) {
			ns->stats.errors++;
			return false;
		}
		*crc_out = m.dcrc;
		return true;
	}
	uint8_t c[CHUNK];
	uint32_t crc = ~0u;
	for (uint32_t col = 0; col < P; col += CHUNK) {
		if (is_data(e)) {
			if (!ns->dev->read(ns->dev->ctx, entry_page(e) + h, (uint16_t)col, c, CHUNK)) {
				ns->stats.errors++;
				if (!replace) {
					return false;
				}
				memset(c, 0x00, CHUNK);	// a replace doesn't depend on the old page: it counts as changed
			}
		} else {
			memset(c, 0xFF, CHUNK);
		}
		for (uint32_t i = col < lo ? lo : col; i < hi && i < col + CHUNK; i++) {
			uint8_t o = c[i - col], w = data[i - lo];
			if (!replace && (o & w) != w) {
				return false;		// a 0 bit back to 1: needs an erase
			}
			*changed = *changed || o != w;
			c[i - col] = w;
		}
		crc = crc_raw(crc, c, CHUNK);
	}
	*crc_out = ~crc;
	return true;
}

// Bytes [off, off + n) of sector s
static bool write_sector(nand_sectors_t *ns, uint16_t s, uint32_t off, const uint8_t *data, uint32_t n, bool replace)
{
	uint32_t crc[2], lo[2], hi[2];
	bool changed = false;
	for (uint32_t h = 0; h < 2; h++) {
		uint32_t a = h * P;
		uint32_t a0 = off > a ? off : a, a1 = off + n < a + P ? off + n : a + P;
		lo[h] = a1 > a0 ? a0 - a : 0;
		hi[h] = a1 > a0 ? a1 - a : 0;
		if (!half_crc(ns, ns->map[s], h, lo[h], hi[h], data + (a1 > a0 ? a0 - off : 0), &crc[h], &changed, replace)) {
			return false;
		}
	}
	if (!changed) {
		return true;
	}
	for (unsigned attempt = 0; attempt < ATTEMPTS; attempt++) {
		if (!room(ns, 2, true)) {
			return false;
		}
		uint16_t e = ns->map[s];	// garbage collection may have moved it (same content)
		uint32_t dst = ns->open * ppb(ns) + ns->next;
		meta_t m = { .seq = ns->seq++, .sector = s, .kind = KIND_DATA };
		nand_result_t r = NAND_OK;
		for (uint32_t h = 0; h < 2 && r == NAND_OK; h++) {
			nand_chunk_t c = { (uint16_t)lo[h], (uint16_t)(hi[h] - lo[h]),
					   data + (hi[h] > lo[h] ? h * P + lo[h] - off : 0) };
			// From the old page, unless there is none or the write covers the whole half
			uint32_t src = is_data(e) && !(lo[h] == 0 && hi[h] == P) ? entry_page(e) + h : NAND_SECTORS_NO_PAGE;
			m.half = (uint8_t)h;
			m.dcrc = crc[h];
			r = put_page(ns, dst + h, src, hi[h] > lo[h] ? &c : NULL, &m);
		}
		if (r == NAND_OK) {
			ns->next = (uint16_t)(ns->next + 2u);
			commit(ns, s, (uint16_t)dst);
			ns->stats.writes++;
			return true;
		}
		failed(ns, r);
	}
	return false;
}

bool nand_sectors_write(nand_sectors_t *ns, uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;
	if (!range_ok(ns, addr, len)) {
		return false;
	}
	while (len > 0) {
		uint32_t off = addr % NAND_SECTORS_SECTOR;
		uint32_t n = NAND_SECTORS_SECTOR - off < len ? NAND_SECTORS_SECTOR - off : len;
		busy(ns);	// a run of sector writes (a codeplug import) busy-waits as long as a collection
		if (!write_sector(ns, (uint16_t)(addr / NAND_SECTORS_SECTOR), off, p, n, false)) {
			return false;
		}
		addr += n;
		p += n;
		len -= n;
	}
	return true;
}

bool nand_sectors_replace(nand_sectors_t *ns, uint32_t addr, const void *buf)
{
	if (!range_ok(ns, addr, NAND_SECTORS_SECTOR) || addr % NAND_SECTORS_SECTOR != 0) {
		return false;
	}
	busy(ns);
	return write_sector(ns, (uint16_t)(addr / NAND_SECTORS_SECTOR), 0, buf, NAND_SECTORS_SECTOR, true);
}

bool nand_sectors_erase(nand_sectors_t *ns, uint32_t addr)
{
	if (!range_ok(ns, addr, NAND_SECTORS_SECTOR) || addr % NAND_SECTORS_SECTOR != 0) {
		return false;
	}
	uint16_t s = (uint16_t)(addr / NAND_SECTORS_SECTOR);
	if (!is_data(ns->map[s])) {
		return true;			// erased already
	}
	busy(ns);
	return put_erased(ns, s, true);
}

// An erase record for sector s (one page)
static bool put_erased(nand_sectors_t *ns, uint16_t s, bool may_collect)
{
	for (unsigned attempt = 0; attempt < ATTEMPTS; attempt++) {
		if (!room(ns, 1, may_collect)) {
			return false;
		}
		uint32_t dst = ns->open * ppb(ns) + ns->next;
		meta_t m = { .seq = ns->seq++, .sector = s, .half = 0, .kind = KIND_ERASED, .dcrc = ~crc_ff(~0u, P) };
		nand_result_t r = put_page(ns, dst, NAND_SECTORS_NO_PAGE, NULL, &m);
		if (r == NAND_OK) {
			ns->next++;
			commit(ns, s, (uint16_t)(dst | ERASED_BIT));
			ns->stats.writes++;
			return true;
		}
		failed(ns, r);
	}
	return false;
}

void nand_sectors_get_stats(const nand_sectors_t *ns, nand_sectors_stats_t *out)
{
	*out = ns->stats;
	out->seq = ns->seq;
	out->blocks_free = 0;
	out->blocks_used = 0;
	out->blocks_bad = 0;
	out->sectors_used = 0;
	if (!ns->mounted) {
		return;
	}
	for (uint16_t b = 0; b < ns->dev->blocks; b++) {
		if (!usable(ns, b)) {
			out->blocks_bad++;
		} else if (ns->live[b] == 0 && b != ns->open) {
			out->blocks_free++;
		} else {
			out->blocks_used++;
		}
	}
	for (uint16_t s = 0; s < ns->sectors; s++) {
		out->sectors_used = (uint16_t)(out->sectors_used + is_data(ns->map[s]));
	}
}
