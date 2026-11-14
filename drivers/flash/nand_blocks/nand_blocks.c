// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A large linear device over NAND blocks (nand_blocks.h, docs/spec/nand-blocks.md).
//
// Each logical block has at most one current copy, a physical block. A write never changes a copy: it goes into the
// open copy, a fresh block filled in page order. Pages the writes skip are moved from the old copy inside the chip
// (or left erased where the old copy has nothing), and the copy takes over (the map moves) once its last page is
// programmed. A write to another block, or to a page below the open copy's next page, closes the open copy first.
// Every programmed page carries TAG_LEN bytes in its spare area at TAG_COL:
//   0  u32 sequence number (one per copy; it only grows)
//   4  u16 logical block     6  u8 page in the block     7  u8 kind (KIND_DATA)
//   8  u32 CRC-32 of the page's 2048 data bytes
//   12 u32 CRC-32 of TAG_MAGIC and bytes 0-11: a page whose spare area doesn't check isn't ours
// The last page is always programmed, last: a block whose last page carries a valid tag with a matching data CRC is a
// complete copy, and of the complete copies of a logical block the one with the highest sequence number is current.
#include <string.h>
#include "nand_blocks.h"

#define P		NAND_BLOCKS_PAGE
#define TAG_SPARE	8u		// spare bytes 0-7 stay erased (the factory bad-block mark is byte 0), as nand_sectors
#define TAG_COL		(P + TAG_SPARE)
#define TAG_LEN		16u
#define TAG_MAGIC	0x314C424Eu	// "NBL1"
#define KIND_DATA	0xB1u
#define NONE		NAND_BLOCKS_NONE
#define CHUNK		128u
#define ATTEMPTS	3u

// Physical block flags
#define F_BAD		0x01u	// bad (factory mark or retired): never touched
#define F_FOREIGN	0x02u	// holds data the layer didn't write: never touched
#define F_OURS		0x04u	// holds pages of ours (the foreign check isn't needed before erasing it)

typedef struct {
	uint32_t seq;
	uint16_t logical;
	uint8_t page;
	uint32_t dcrc;
} tag_t;

typedef enum { TAG_BLANK, TAG_VALID, TAG_INVALID } tag_state_t;

// CRC-32 (IEEE 802.3, reflected), four bits at a time (as nand_sectors)
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

static uint32_t tag_crc(const uint8_t *b)
{
	uint8_t magic[4];
	put32(magic, TAG_MAGIC);
	return ~crc_raw(crc_raw(~0u, magic, 4), b, 12);
}

static void tag_encode(const tag_t *t, uint8_t b[TAG_LEN])
{
	put32(&b[0], t->seq);
	b[4] = (uint8_t)t->logical;
	b[5] = (uint8_t)(t->logical >> 8);
	b[6] = t->page;
	b[7] = KIND_DATA;
	put32(&b[8], t->dcrc);
	put32(&b[12], tag_crc(b));
}

static uint16_t ppb(const nand_blocks_t *nb)
{
	return nb->dev->pages_per_block;
}

static uint32_t block_bytes(const nand_blocks_t *nb)
{
	return (uint32_t)ppb(nb) * P;
}

static uint32_t page_of(const nand_blocks_t *nb, uint16_t b, uint16_t p)
{
	return (uint32_t)b * ppb(nb) + p;
}

static void busy(const nand_blocks_t *nb)
{
	if (nb->dev->busy != NULL) {
		nb->dev->busy(nb->dev->ctx);
	}
}

static tag_state_t read_tag(nand_blocks_t *nb, uint32_t page, tag_t *t)
{
	uint8_t b[TAG_LEN];
	if (!nb->dev->read(nb->dev->ctx, page, TAG_COL, b, sizeof(b))) {
		return TAG_INVALID;		// ECC couldn't correct it: torn or worn
	}
	bool blank = true;
	for (unsigned i = 0; i < sizeof(b) && blank; i++) {
		blank = b[i] == 0xFF;
	}
	if (blank) {
		return TAG_BLANK;
	}
	if (get32(&b[12]) != tag_crc(b) || b[7] != KIND_DATA || b[6] != page % ppb(nb)) {
		return TAG_INVALID;
	}
	t->seq = get32(&b[0]);
	t->logical = (uint16_t)(b[4] | (b[5] << 8));
	t->page = b[6];
	t->dcrc = get32(&b[8]);
	return TAG_VALID;
}

// CRC-32 of a page's data as the chip reads it now, with len bytes of data at column col in its place (len 0: none)
static bool page_crc(nand_blocks_t *nb, uint32_t page, uint16_t col, const uint8_t *data, uint32_t len, uint32_t *out)
{
	uint8_t c[CHUNK];
	uint32_t crc = ~0u;
	for (uint32_t at = 0; at < P; at += CHUNK) {
		if (page == NAND_SECTORS_NO_PAGE) {
			memset(c, 0xFF, CHUNK);
		} else if (!nb->dev->read(nb->dev->ctx, page, (uint16_t)at, c, CHUNK)) {
			return false;
		}
		for (uint32_t i = 0; i < CHUNK; i++) {
			if (at + i >= col && at + i < (uint32_t)col + len) {
				c[i] = data[at + i - col];
			}
		}
		crc = crc_raw(crc, c, CHUNK);
	}
	*out = ~crc;
	return true;
}

// --- Physical blocks ---

static bool mapped(const nand_blocks_t *nb, uint16_t b)
{
	for (uint16_t l = 0; l < nb->blocks; l++) {
		if (nb->map[l] == b) {
			return true;
		}
	}
	return false;
}

static bool is_free(const nand_blocks_t *nb, uint16_t b)
{
	return (nb->flags[b] & (F_BAD | F_FOREIGN)) == 0 && !(nb->open_l != NONE && nb->open_p == b) && !mapped(nb, b);
}

static void retire(nand_blocks_t *nb, uint16_t b)
{
	nb->dev->mark_bad(nb->dev->ctx, b);
	nb->flags[b] = F_BAD;
	nb->stats.retired++;
}

// --- Foreign data (the rules of nand_sectors, docs/spec/nand-sectors.md 4) ---

typedef enum { PAGE_BLANK, PAGE_OURS, PAGE_UNREADABLE, PAGE_OTHER } page_kind_t;

static page_kind_t page_kind(nand_blocks_t *nb, uint32_t page)
{
	tag_t t;
	tag_state_t st = read_tag(nb, page, &t);
	if (st == TAG_VALID) {
		return PAGE_OURS;
	}
	const uint8_t *mark = nb->dev->factory_mark;
	uint32_t mark_len = mark != NULL ? nb->dev->factory_mark_len : 0u;
	uint8_t c[CHUNK];
	uint32_t end = P + nb->dev->spare_size;
	for (uint32_t col = 0; col < end; col += CHUNK) {
		uint32_t n = end - col < CHUNK ? end - col : CHUNK;
		if (!nb->dev->read(nb->dev->ctx, page, (uint16_t)col, c, n)) {
			return PAGE_UNREADABLE;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint32_t at = col + i;
			if (c[i] != 0xFFu && !(at < mark_len && c[i] == mark[at])) {
				return PAGE_OTHER;
			}
		}
	}
	return st == TAG_INVALID ? PAGE_UNREADABLE : PAGE_BLANK;
}

// A block's first and last pages: true if either is readable, programmed and not ours
static bool foreign(nand_blocks_t *nb, uint16_t b)
{
	page_kind_t k0 = page_kind(nb, page_of(nb, b, 0));
	if (k0 == PAGE_OURS) {
		return false;
	}
	return k0 == PAGE_OTHER || page_kind(nb, page_of(nb, b, (uint16_t)(ppb(nb) - 1u))) == PAGE_OTHER;
}

static void found_foreign(nand_blocks_t *nb, uint16_t b)
{
	if (nb->stats.foreign == 0) {
		nb->stats.foreign_block = b;
	}
	nb->stats.foreign++;
	nb->flags[b] |= F_FOREIGN;
}

// The next free block after the last one taken (wear goes round the region), erased. NONE when none is left
static uint16_t take_block(nand_blocks_t *nb)
{
	uint16_t blocks = nb->dev->blocks;
	for (uint16_t i = 1; i <= blocks; i++) {
		uint16_t b = (uint16_t)((nb->rotor + i) % blocks);
		if (!is_free(nb, b)) {
			continue;
		}
		busy(nb);
		// Checked just before the erase, so a block can't be lost to a read error at the mount
		if (!(nb->flags[b] & F_OURS) && foreign(nb, b)) {
			found_foreign(nb, b);
			continue;
		}
		nand_result_t r = nb->dev->erase(nb->dev->ctx, b);
		nb->stats.erases++;
		if (r == NAND_FAILED) {
			retire(nb, b);
			continue;
		}
		if (r != NAND_OK) {
			nb->stats.errors++;
			continue;
		}
		nb->flags[b] = F_OURS;
		nb->rotor = b;
		return b;
	}
	return NONE;
}

// --- The open copy ---

// The physical page that holds logical page p of logical block l now (NAND_SECTORS_NO_PAGE: it reads 0xFF)
static uint32_t current(const nand_blocks_t *nb, uint16_t l, uint16_t p)
{
	if (nb->open_l == l && p < nb->next) {
		return page_of(nb, nb->open_p, p);
	}
	uint16_t b = nb->map[l];
	return b == NONE ? NAND_SECTORS_NO_PAGE : page_of(nb, b, p);
}

// The open copy's block failed at page k: a fresh block takes pages 0..k-1 from it (they carry the right tags
// already), then the failed one is retired
static bool relocate(nand_blocks_t *nb, uint16_t k)
{
	uint16_t old = nb->open_p;
	for (unsigned attempt = 0; attempt < ATTEMPTS; attempt++) {
		uint16_t b = take_block(nb);
		if (b == NONE) {
			return false;
		}
		bool ok = true;
		for (uint16_t q = 0; q < k && ok; q++) {
			tag_t t;
			tag_state_t st = read_tag(nb, page_of(nb, old, q), &t);
			if (st == TAG_BLANK) {
				continue;		// left erased: reads 0xFF in both
			}
			if (st != TAG_VALID) {
				return false;		// a page of the copy is lost: the write fails, the old copy stays
			}
			nand_result_t r = nb->dev->program(nb->dev->ctx, page_of(nb, b, q), page_of(nb, old, q), NULL, 0);
			nb->stats.moves++;
			if (r == NAND_FAILED) {
				retire(nb, b);
				ok = false;
			} else if (r != NAND_OK) {
				nb->stats.errors++;
				nb->flags[b] = F_OURS;	// erased again when next taken
				ok = false;
			}
		}
		if (ok) {
			nb->open_p = b;
			retire(nb, old);
			return true;
		}
	}
	return false;
}

// Program page p of the open copy: from src's data (NAND_SECTORS_NO_PAGE: all 0xFF), then len bytes at col
static bool program(nand_blocks_t *nb, uint16_t p, uint32_t src, uint16_t col, const uint8_t *data, uint32_t len,
		    uint32_t dcrc, bool move)
{
	tag_t t = { .seq = nb->open_seq, .logical = nb->open_l, .page = (uint8_t)p, .dcrc = dcrc };
	uint8_t tb[TAG_LEN];
	tag_encode(&t, tb);
	nand_chunk_t c[2];
	unsigned n = 0;
	if (len != 0) {
		c[n++] = (nand_chunk_t){ .column = col, .len = (uint16_t)len, .data = data };
	}
	c[n++] = (nand_chunk_t){ .column = TAG_COL, .len = TAG_LEN, .data = tb };
	for (unsigned attempt = 0; attempt < ATTEMPTS; attempt++) {
		nand_result_t r = nb->dev->program(nb->dev->ctx, page_of(nb, nb->open_p, p), src, c, n);
		if (move) {
			nb->stats.moves++;
		} else {
			nb->stats.programs++;
		}
		if (r == NAND_OK) {
			return true;
		}
		nb->stats.errors++;
		// The page may be half programmed: it is never programmed again. The pages before it go to a new block
		if (!relocate(nb, p)) {
			return false;
		}
	}
	return false;
}

// Where page q of the open copy's old copy is, to be moved (*src, its data CRC *dcrc). False: nothing to move, the
// page stays erased: no old copy, a page it never programmed, or one that can't be read (lost either way, and left
// erased so the block can still be written; the host's checksums show it and send it again)
static bool old_page(nand_blocks_t *nb, uint16_t q, uint32_t *src, uint32_t *dcrc)
{
	uint16_t old = nb->map[nb->open_l];
	if (old == NONE) {
		return false;
	}
	*src = page_of(nb, old, q);
	tag_t t;
	tag_state_t st = read_tag(nb, *src, &t);
	if (st == TAG_BLANK) {
		return false;
	}
	if (st == TAG_INVALID && !page_crc(nb, *src, 0, NULL, 0, &t.dcrc)) {
		nb->stats.lost++;
		return false;
	}
	*dcrc = t.dcrc;
	return true;
}

// Move pages next..to-1 from the old copy into the open one
static bool move_to(nand_blocks_t *nb, uint16_t to)
{
	while (nb->next < to) {
		uint16_t q = nb->next;
		uint32_t src, dcrc;
		if (old_page(nb, q, &src, &dcrc) && !program(nb, q, src, 0, NULL, 0, dcrc, true)) {
			return false;
		}
		nb->next = (uint16_t)(q + 1u);
	}
	return true;
}

static uint32_t ff_crc(void)
{
	static uint32_t v;
	if (v == 0) {
		uint8_t ff[CHUNK];
		memset(ff, 0xFF, sizeof(ff));
		uint32_t crc = ~0u;
		for (uint32_t i = 0; i < P; i += CHUNK) {
			crc = crc_raw(crc, ff, CHUNK);
		}
		v = ~crc;
	}
	return v;
}

static bool close_open(nand_blocks_t *nb)
{
	if (nb->open_l == NONE) {
		return true;
	}
	uint16_t last = (uint16_t)(ppb(nb) - 1u);
	if (!move_to(nb, last)) {
		return false;
	}
	if (nb->next == last) {
		// The last page: moved from the old copy, or programmed erased, so the commit is always there
		uint32_t src = NAND_SECTORS_NO_PAGE, dcrc = ff_crc();
		if (!old_page(nb, last, &src, &dcrc)) {
			src = NAND_SECTORS_NO_PAGE;
			dcrc = ff_crc();
		}
		if (!program(nb, last, src, 0, NULL, 0, dcrc, true)) {
			return false;
		}
		nb->next = ppb(nb);
	} else {
		// The writes passed the last page leaving it erased: it is programmed erased now, with the commit
		tag_t t;
		tag_state_t st = read_tag(nb, page_of(nb, nb->open_p, last), &t);
		if (st == TAG_BLANK && !program(nb, last, NAND_SECTORS_NO_PAGE, 0, NULL, 0, ff_crc(), true)) {
			return false;
		}
	}
	nb->map[nb->open_l] = nb->open_p;
	nb->open_l = NONE;
	nb->stats.commits++;
	return true;
}

// Make logical page (l, p) the open copy's next page to program
static bool open_at(nand_blocks_t *nb, uint16_t l, uint16_t p)
{
	if (nb->open_l != NONE && (nb->open_l != l || p < nb->next)) {
		if (!close_open(nb)) {
			nb->open_l = NONE;	// the old copy stays current; the block taken is free again
			return false;
		}
	}
	if (nb->open_l == NONE) {
		uint16_t b = take_block(nb);
		if (b == NONE) {
			return false;
		}
		nb->open_l = l;
		nb->open_p = b;
		nb->next = 0;
		nb->open_seq = nb->stats.seq++;
	}
	if (!move_to(nb, p)) {
		nb->open_l = NONE;
		return false;
	}
	return true;
}

// Logical page (l, p) gets len bytes at col over its current content (len P: the whole page). A page that comes out
// all 0xFF is left erased
static bool put_page(nand_blocks_t *nb, uint16_t l, uint16_t p, uint16_t col, const uint8_t *data, uint32_t len)
{
	if (!open_at(nb, l, p)) {
		return false;
	}
	uint32_t src = len == P ? NAND_SECTORS_NO_PAGE : current(nb, l, p);
	uint32_t dcrc;
	if (!page_crc(nb, src, col, data, len, &dcrc)) {
		return false;		// the page's old bytes can't be read: this write fails, the open copy goes on
	}
	bool ok = dcrc == ff_crc() || program(nb, p, src, col, data, len, dcrc, false);
	if (!ok) {
		nb->open_l = NONE;
		return false;
	}
	nb->next = (uint16_t)(p + 1u);
	return p + 1u < ppb(nb) || close_open(nb);
}

// --- The device ---

static bool in_range(const nand_blocks_t *nb, uint32_t addr, uint32_t len)
{
	uint32_t size = nand_blocks_size(nb);
	return nb->mounted && addr <= size && len <= size - addr;
}

bool nand_blocks_mount(nand_blocks_t *nb)
{
	const nand_dev_t *d = nb->dev;
	memset(&nb->stats, 0, sizeof(nb->stats));
	nb->mounted = false;
	nb->open_l = NONE;
	if (d == NULL || nb->map == NULL || nb->flags == NULL || d->pages_per_block < 2 || d->pages_per_block > 255 ||
	    d->spare_size < TAG_SPARE + TAG_LEN || nb->blocks == 0 || nb->blocks >= d->blocks || d->blocks >= NONE ||
	    (uint64_t)nb->blocks * d->pages_per_block * P > 0xFFFFFFFFull) {
		nb->stats.status = NAND_BLOCKS_BAD_SETUP;
		return false;
	}
	for (uint16_t l = 0; l < nb->blocks; l++) {
		nb->map[l] = NONE;
	}
	uint32_t seq = 0;
	uint16_t last = (uint16_t)(ppb(nb) - 1u);
	for (uint16_t b = 0; b < d->blocks; b++) {
		busy(nb);
		nb->flags[b] = 0;
		bool bad = false;
		if (!d->block_bad(d->ctx, b, &bad) || bad) {
			nb->flags[b] = F_BAD;
			continue;
		}
		tag_t t, t0;
		tag_state_t st = read_tag(nb, page_of(nb, b, last), &t);
		tag_state_t st0 = read_tag(nb, page_of(nb, b, 0), &t0);
		if (st0 == TAG_VALID) {
			nb->flags[b] |= F_OURS;
			seq = t0.seq + 1u > seq ? t0.seq + 1u : seq;
		}
		if (st == TAG_VALID) {
			nb->flags[b] |= F_OURS;
			seq = t.seq + 1u > seq ? t.seq + 1u : seq;
			uint32_t dcrc;
			if (t.logical >= nb->blocks || !page_crc(nb, page_of(nb, b, last), 0, NULL, 0, &dcrc) || dcrc != t.dcrc) {
				nb->stats.torn++;	// cut while its last page was programmed: the copy never took over
				continue;
			}
			uint16_t cur = nb->map[t.logical];
			tag_t ct;
			if (cur == NONE || read_tag(nb, page_of(nb, cur, last), &ct) != TAG_VALID || t.seq > ct.seq) {
				nb->map[t.logical] = b;
			}
			continue;
		}
		if (st0 != TAG_VALID && foreign(nb, b)) {
			found_foreign(nb, b);
		}
	}
	if (nb->stats.foreign != 0) {
		nb->stats.status = NAND_BLOCKS_FOREIGN;
		return false;
	}
	nb->stats.seq = seq;
	nb->rotor = (uint16_t)(d->blocks - 1u);
	nb->mounted = true;
	nb->stats.status = NAND_BLOCKS_MOUNTED;
	return true;
}

uint32_t nand_blocks_size(const nand_blocks_t *nb)
{
	return nb->dev == NULL ? 0 : (uint32_t)nb->blocks * block_bytes(nb);
}

bool nand_blocks_read(nand_blocks_t *nb, uint32_t addr, void *buf, uint32_t len)
{
	uint8_t *out = buf;
	if (!in_range(nb, addr, len)) {
		return false;
	}
	while (len > 0) {
		uint16_t l = (uint16_t)(addr / block_bytes(nb));
		uint16_t p = (uint16_t)(addr % block_bytes(nb) / P);
		uint16_t col = (uint16_t)(addr % P);
		uint32_t n = P - col < len ? P - col : len;
		uint32_t page = current(nb, l, p);
		if (page == NAND_SECTORS_NO_PAGE) {
			memset(out, 0xFF, n);
		} else if (!nb->dev->read(nb->dev->ctx, page, col, out, n)) {
			nb->stats.errors++;
			return false;
		}
		out += n;
		addr += n;
		len -= n;
	}
	return true;
}

bool nand_blocks_write(nand_blocks_t *nb, uint32_t addr, const void *buf, uint32_t len)
{
	const uint8_t *in = buf;
	if (!in_range(nb, addr, len)) {
		return false;
	}
	// NOR rule over the whole range first, so a refused write writes nothing
	bool same = true;
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint8_t c[CHUNK];
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		if (!nand_blocks_read(nb, addr + off, c, n)) {
			return false;
		}
		for (uint32_t i = 0; i < n; i++) {
			if ((c[i] & in[off + i]) != in[off + i]) {
				return false;
			}
			same = same && c[i] == in[off + i];
		}
	}
	while (len > 0 && !same) {
		uint16_t col = (uint16_t)(addr % P);
		uint32_t n = P - col < len ? P - col : len;
		if (!put_page(nb, (uint16_t)(addr / block_bytes(nb)), (uint16_t)(addr % block_bytes(nb) / P), col, in, n)) {
			return false;
		}
		in += n;
		addr += n;
		len -= n;
	}
	return true;
}

bool nand_blocks_replace(nand_blocks_t *nb, uint32_t addr, const void *buf)
{
	const uint8_t *in = buf;
	if (addr % NAND_BLOCKS_SECTOR != 0 || !in_range(nb, addr, NAND_BLOCKS_SECTOR)) {
		return false;
	}
	for (uint32_t h = 0; h < NAND_BLOCKS_SECTOR; h += P) {
		uint32_t a = addr + h;
		if (!put_page(nb, (uint16_t)(a / block_bytes(nb)), (uint16_t)(a % block_bytes(nb) / P), 0, in + h, P)) {
			return false;
		}
	}
	return true;
}

bool nand_blocks_erase(nand_blocks_t *nb, uint32_t addr)
{
	if (addr % NAND_BLOCKS_SECTOR != 0 || !in_range(nb, addr, NAND_BLOCKS_SECTOR)) {
		return false;
	}
	for (uint32_t h = 0; h < NAND_BLOCKS_SECTOR; h += P) {
		uint32_t a = addr + h;
		uint16_t l = (uint16_t)(a / block_bytes(nb)), p = (uint16_t)(a % block_bytes(nb) / P);
		uint8_t c[CHUNK];
		bool blank = true;
		for (uint32_t o = 0; o < P && blank; o += CHUNK) {
			if (!nand_blocks_read(nb, a + o, c, CHUNK)) {
				return false;
			}
			for (unsigned i = 0; i < CHUNK && blank; i++) {
				blank = c[i] == 0xFF;
			}
		}
		if (blank) {
			continue;		// already erased: nothing to write
		}
		// The page goes into the copy erased: left unprogrammed
		if (!open_at(nb, l, p)) {
			return false;
		}
		nb->next = (uint16_t)(p + 1u);
		if (p + 1u == ppb(nb) && !close_open(nb)) {
			nb->open_l = NONE;
			return false;
		}
	}
	return true;
}

bool nand_blocks_sync(nand_blocks_t *nb)
{
	if (!nb->mounted) {
		return false;
	}
	if (!close_open(nb)) {
		nb->open_l = NONE;
		return false;
	}
	return true;
}

void nand_blocks_get_stats(const nand_blocks_t *nb, nand_blocks_stats_t *out)
{
	*out = nb->stats;
	out->blocks_bad = out->blocks_used = out->blocks_free = 0;
	for (uint16_t b = 0; b < (nb->dev != NULL ? nb->dev->blocks : 0u); b++) {
		if (nb->flags[b] & F_BAD) {
			out->blocks_bad++;
		} else if (mapped(nb, b)) {
			out->blocks_used++;
		} else if ((nb->flags[b] & F_FOREIGN) == 0) {
			out->blocks_free++;
		}
	}
}
