// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fast codeplug protocol (core/cps/cps_fast.c, docs/spec/cps-fast.md) on a simulated flash with the DM-1701's
// layout: the LZ4 encoder and streaming decoder against a reference decoder and a second encoder, blank blocks, CRC
// sums and DIFF, READ windows, the range refusals, direct and staged writes, the session time-out, power cuts all
// through a staged commit and a direct write, and the staging area shared with the TFTP server.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cps/cps_fast.h"
#include "cps/cps_lz4.h"
#include "cps/cps_openfw.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "net/tftp_codeplug.h"
#include "debuglink/debuglink_proto.h"
#include "sim/sim_flash.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x400000u
#define BLOCK		4096u
#define STAGING		0x200000u

// The DM-1701's areas, the image shortened to fit 4 MB
static const hal_flash_map_t dm1701_map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x1C0000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x1C0000u, .native_size = 0x40000u,
	.staging_addr = STAGING, .staging_size = 0x40000u,
};

// An HR_C7000-style layout: the image at +0x200000, no staging area, a calibration window inside the flash image
static const hal_flash_map_t offset_map = {
	.settings_addr = 0x211000u, .settings_size = 0x8000u, .journal_addr = 0x219000u,
	.openfw_eeprom = 0x200000u, .openfw_flash = 0x220000u, .openfw_end = 0x3C0000u,
	.calibration_addr = 0x230000u, .calibration_size = 0x200u,
	.native_addr = 0x3C0000u, .native_size = 0x40000u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static uint8_t rep[DL_REPLY_MAX];
static size_t rep_len;
static uint32_t now = 1000;
static uint32_t rng = 12345;

static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void p16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void p32(uint8_t *p, uint32_t v)
{
	p16(p, v);
	p16(p + 2, v >> 16);
}

static uint32_t g16(const uint8_t *p)
{
	return (uint32_t)(p[0] | (p[1] << 8));
}

static uint32_t g32(const uint8_t *p)
{
	return g16(p) | (g16(p + 2) << 16);
}

static void boot(const hal_flash_map_t *m, bool fresh)
{
	if (fresh) {
		sim_flash_free();
		sim_flash_create(FLASH_SIZE, m);
	}
	storage_buffer_release();	// RAM starts over
	storage_init();
	cps_fast_resume();
	settings_init();
	codeplug_init();
}

// --- requests ---

static uint8_t call(const uint8_t *req, size_t len)
{
	memset(rep, 0xEE, sizeof(rep));
	rep_len = cps_fast_cmd(req, (uint16_t)len, rep, sizeof(rep), now);
	now += 1;
	return rep[0];
}

static uint8_t op(uint8_t o)
{
	return call(&o, 1);
}

static uint8_t sums(uint32_t addr, uint8_t count)
{
	uint8_t r[6] = { CPSF_OP_SUMS };
	p32(&r[1], addr);
	r[5] = count;
	return call(r, sizeof(r));
}

static uint8_t begin(uint8_t mode)
{
	uint8_t r[2] = { CPSF_OP_BEGIN, mode };
	return call(r, sizeof(r));
}

static uint8_t block_req(uint32_t addr, uint8_t format, uint32_t len, uint32_t crc)
{
	uint8_t r[12] = { CPSF_OP_BLOCK };
	p32(&r[1], addr);
	r[5] = format;
	p16(&r[6], len);
	p32(&r[8], crc);
	return call(r, sizeof(r));
}

static uint8_t state(void)
{
	return op(CPSF_OP_STATUS) == DL_ST_OK ? rep[1] : 0xFF;
}

static bool refused(uint8_t why)
{
	return rep[0] == DL_ST_REFUSED && rep[1] == why;
}

// A reference LZ4 block decoder, written apart from the radio's: also checks the format's end rules (the last 5 bytes
// literals, no match starting in the last 12)
static bool ref_decode(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t size)
{
	uint32_t i = 0, o = 0;
	for (;;) {
		if (i >= n) {
			return false;
		}
		uint8_t tok = src[i++];
		uint32_t lit = tok >> 4;
		if (lit == 15) {
			uint8_t b;
			do {
				if (i >= n) {
					return false;
				}
				b = src[i++];
				lit += b;
			} while (b == 255);
		}
		if (i + lit > n || o + lit > size) {
			return false;
		}
		memcpy(dst + o, src + i, lit);
		i += lit;
		o += lit;
		if (i == n) {
			return o == size && (size < 5 || lit >= 5 || size < 13);
		}
		if (i + 2 > n) {
			return false;
		}
		uint32_t off = src[i] | (src[i + 1] << 8), ml = (tok & 15u);
		i += 2;
		if (ml == 15) {
			uint8_t b;
			do {
				if (i >= n) {
					return false;
				}
				b = src[i++];
				ml += b;
			} while (b == 255);
		}
		ml += 4;
		if (off == 0 || off > o || o + ml > size || o + 12 > size || o + ml + 5 > size) {
			return false;
		}
		for (uint32_t k = 0; k < ml; k++, o++) {
			dst[o] = dst[o - off];
		}
	}
}

// A second encoder (longest match over the whole block, any offset), so the decoder sees streams the radio's doesn't
// make
static uint32_t slow_encode(const uint8_t *src, uint32_t n, uint8_t *out)
{
	uint32_t ip = 0, anchor = 0, k = 0;
	while (n >= 13 && ip + 12 <= n) {
		uint32_t best = 0, boff = 0;
		for (uint32_t ref = 0; ref < ip; ref++) {
			uint32_t ml = 0;
			while (ip + ml < n - 5 && src[ref + ml] == src[ip + ml]) {
				ml++;
			}
			if (ml >= 4 && ml > best) {
				best = ml;
				boff = ip - ref;
			}
		}
		if (best == 0) {
			ip++;
			continue;
		}
		uint32_t lit = ip - anchor;
		out[k++] = (uint8_t)(((lit < 15 ? lit : 15) << 4) | (best - 4 < 15 ? best - 4 : 15));
		if (lit >= 15) {
			uint32_t v = lit - 15;
			for (; v >= 255; v -= 255) {
				out[k++] = 255;
			}
			out[k++] = (uint8_t)v;
		}
		memcpy(out + k, src + anchor, lit);
		k += lit;
		out[k++] = (uint8_t)boff;
		out[k++] = (uint8_t)(boff >> 8);
		if (best - 4 >= 15) {
			uint32_t v = best - 19;
			for (; v >= 255; v -= 255) {
				out[k++] = 255;
			}
			out[k++] = (uint8_t)v;
		}
		ip += best;
		anchor = ip;
	}
	uint32_t lit = n - anchor;
	out[k++] = (uint8_t)((lit < 15 ? lit : 15) << 4);
	if (lit >= 15) {
		uint32_t v = lit - 15;
		for (; v >= 255; v -= 255) {
			out[k++] = 255;
		}
		out[k++] = (uint8_t)v;
	}
	memcpy(out + k, src + anchor, lit);
	return k + lit;
}

// Blocks like a codeplug's: 56-byte records, names padded with 0xFF, a few fields, empty records erased
static void codeplug_block(uint8_t *b, unsigned used)
{
	memset(b, 0xFF, BLOCK);
	for (unsigned r = 0; r < used && (r + 1) * 56 <= BLOCK; r++) {
		uint8_t *p = b + r * 56;
		int n = snprintf((char *)p, 17, "Ch %u", r + rnd() % 900);
		memset(p + n, 0xFF, 16 - (unsigned)n);
		p32(p + 16, 0x44600000u + (rnd() % 100) * 125);
		p32(p + 20, 0x44600000u + (rnd() % 100) * 125);
		memset(p + 24, 0, 32);
		p[24] = (uint8_t)(rnd() & 1);
		p[40] = 0x16;
		p[44] = (uint8_t)(rnd() % 16);
	}
}

static void random_block(uint8_t *b)
{
	for (unsigned i = 0; i < BLOCK; i++) {
		b[i] = (uint8_t)rnd();
	}
}

// --- the codec ---

static bool stream_decodes(const uint8_t *stream, uint32_t n, const uint8_t *want, uint32_t size, uint32_t piece)
{
	static uint8_t out[BLOCK];
	cps_lz4_dec_t d;
	cps_lz4_dec_init(&d, out, size);
	for (uint32_t off = 0; off < n; off += piece) {
		uint32_t k = n - off < piece ? n - off : piece;
		if (!cps_lz4_dec_feed(&d, stream + off, k)) {
			return false;
		}
	}
	return cps_lz4_dec_done(&d) && memcmp(out, want, size) == 0;
}

static void test_lz4(void)
{
	static uint8_t src[BLOCK], stream[2 * BLOCK], win[2 * BLOCK], dec[BLOCK];
	static const uint32_t pieces[] = { 1, 2, 3, 7, 61, 1000, 2 * BLOCK };
	unsigned kinds = 0;

	for (unsigned kind = 0; kind < 8; kind++) {
		switch (kind) {
		case 0: memset(src, 0xFF, BLOCK); break;
		case 1: memset(src, 0, BLOCK); break;
		case 2: random_block(src); break;
		case 3: codeplug_block(src, 3); break;
		case 4: codeplug_block(src, 73); break;
		case 5: for (unsigned i = 0; i < BLOCK; i++) src[i] = (uint8_t)(i / 300); break;	// long runs
		case 6: for (unsigned i = 0; i < BLOCK; i++) src[i] = (uint8_t)(i % 7); break;	// a short period
		default: random_block(src); memset(src + 1000, 0xFF, 2000); break;
		}
		uint32_t n = cps_lz4_encode(src, BLOCK, stream, 0, sizeof(stream));
		CHECK(n == cps_lz4_encode(src, BLOCK, NULL, 0, 0));	// measuring gives the same length
		CHECK(ref_decode(stream, n, dec, BLOCK) && memcmp(dec, src, BLOCK) == 0);
		for (unsigned p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
			CHECK(stream_decodes(stream, n, src, BLOCK, pieces[p]));
		}
		// Windows of the stream, as READ sends it, put back together
		uint32_t got = 0;
		while (got < n) {
			uint32_t t = cps_lz4_encode(src, BLOCK, win + got, got, CPSF_READ_MAX);
			CHECK(t == n);
			got += n - got < CPSF_READ_MAX ? n - got : CPSF_READ_MAX;
		}
		CHECK(memcmp(win, stream, n) == 0);
		// The other encoder's stream
		uint32_t m = slow_encode(src, BLOCK, stream);
		CHECK(ref_decode(stream, m, dec, BLOCK) && memcmp(dec, src, BLOCK) == 0);
		CHECK(stream_decodes(stream, m, src, BLOCK, 61));
		kinds++;
		if (kind == 4) {
			printf("# a 73-record codeplug block: %u bytes LZ4, %u with the slow encoder\n", (unsigned)n, (unsigned)m);
			CHECK(n < BLOCK / 2);
		}
		if (kind == 0) {
			CHECK(n < 40);
		}
	}
	// Short blocks: all literals below 13 bytes
	for (uint32_t len = 0; len < 40; len++) {
		memset(src, 0x55, len);
		uint32_t n = cps_lz4_encode(src, len, stream, 0, sizeof(stream));
		CHECK(ref_decode(stream, n, dec, len) && memcmp(dec, src, len) == 0);
		CHECK(stream_decodes(stream, n, src, len, 1));
	}

	// Corrupt streams
	codeplug_block(src, 20);
	uint32_t n = cps_lz4_encode(src, BLOCK, stream, 0, sizeof(stream));
	cps_lz4_dec_t d;
	cps_lz4_dec_init(&d, dec, BLOCK);
	CHECK(cps_lz4_dec_feed(&d, stream, n - 1) && !cps_lz4_dec_done(&d));	// truncated: never done
	cps_lz4_dec_init(&d, dec, BLOCK);
	uint8_t extra[1] = { 0 };
	CHECK(cps_lz4_dec_feed(&d, stream, n) && cps_lz4_dec_done(&d));
	CHECK(!cps_lz4_dec_feed(&d, extra, 1) || !cps_lz4_dec_done(&d));	// a byte after the end
	static const uint8_t zero_off[] = { 0x14, 'a', 0x00, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a' };	// offset 0
	cps_lz4_dec_init(&d, dec, 13);
	CHECK(!cps_lz4_dec_feed(&d, zero_off, sizeof(zero_off)));
	static const uint8_t far_off[] = { 0x14, 'a', 0x05, 0x00, 0x50, 'a', 'a', 'a', 'a', 'a' };	// before the start
	cps_lz4_dec_init(&d, dec, 13);
	CHECK(!cps_lz4_dec_feed(&d, far_off, sizeof(far_off)));
	static const uint8_t too_long[] = { 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x10 };	// literals longer than the block
	cps_lz4_dec_init(&d, dec, BLOCK);
	CHECK(!cps_lz4_dec_feed(&d, too_long, sizeof(too_long)));
	static const uint8_t long_match[] = { 0x1F, 'a', 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };	// a match past the end
	cps_lz4_dec_init(&d, dec, BLOCK);
	CHECK(!cps_lz4_dec_feed(&d, long_match, sizeof(long_match)));
	static const uint8_t short_out[] = { 0x50, 'a', 'b', 'c', 'd', 'e' };	// 5 bytes for a 6-byte block
	cps_lz4_dec_init(&d, dec, 6);
	CHECK(cps_lz4_dec_feed(&d, short_out, sizeof(short_out)) && !cps_lz4_dec_done(&d));
	printf("# LZ4: %u kinds of block, every piece size, windows, two encoders, corrupt streams refused\n", kinds);
}

// --- reading ---

// A block through READ windows, decoded; the format in *format. False on any error or a CRC that doesn't match
static bool read_block(uint32_t addr, uint8_t *dst, uint8_t *format, unsigned *requests)
{
	static uint8_t stream[2 * BLOCK];
	uint32_t total = 0, got = 0, crc = 0;
	*requests = 0;
	do {
		uint8_t r[7] = { CPSF_OP_READ };
		p32(&r[1], addr);
		p16(&r[5], got);
		if (call(r, sizeof(r)) != DL_ST_OK || g32(&rep[1]) != addr || g16(&rep[12]) != got) {
			return false;
		}
		(*requests)++;
		*format = rep[5];
		total = g16(&rep[6]);
		crc = g32(&rep[8]);
		uint32_t n = (uint32_t)rep_len - 15u;
		if ((n == 0 && got < total) || n > CPSF_READ_MAX) {
			return false;
		}
		memcpy(stream + got, rep + 15, n);
		got += n;
	} while (got < total);
	switch (*format) {
	case CPSF_FMT_BLANK:
		memset(dst, 0xFF, BLOCK);
		break;
	case CPSF_FMT_LZ4:
		if (!ref_decode(stream, total, dst, BLOCK)) {
			return false;
		}
		break;
	default:
		return false;
	}
	return storage_crc32(0, dst, BLOCK) == crc;
}

static void test_info_and_reads(void)
{
	static uint8_t b[BLOCK], got[BLOCK];
	uint8_t fmt;
	unsigned reqs;

	boot(&dm1701_map, true);
	CHECK(op(CPSF_OP_INFO) == DL_ST_OK && rep_len == 27);
	CHECK(rep[1] == CPSF_VERSION && rep[2] == 3 && g16(&rep[3]) == BLOCK && rep[5] == CPSF_SUMS_MAX);
	CHECK(rep[6] == CPSF_DATA_MAX && g16(&rep[7]) == CPSF_READ_MAX);
	CHECK(g32(&rep[9]) == 0x10000u && g32(&rep[13]) == 0x20000u && g32(&rep[17]) == 0x1C0000u && g32(&rep[21]) == 0);
	CHECK(g16(&rep[25]) == 63);	// 256 KB of staging: the record and 63 slots

	// Blank detection: an erased flash is all blank, every CRC the CRC of 4 KB of 0xFF
	memset(b, 0xFF, BLOCK);
	uint32_t blank_crc = storage_crc32(0, b, BLOCK);
	CHECK(sums(0, 16) == DL_ST_OK && rep_len == 8 + 64 && g16(&rep[6]) == 0xFFFF && rep[5] == 16);
	CHECK(g32(&rep[8]) == blank_crc && g32(&rep[8 + 60]) == blank_crc);

	// Some blocks with data: 0x3000 a codeplug block, 0x5000 random, 0x9C000 a byte in the last position
	codeplug_block(b, 40);
	memcpy(&sim_flash_data[0x3000], b, BLOCK);
	random_block(&sim_flash_data[0x5000]);
	sim_flash_data[0x9CFFF] = 0x00;
	CHECK(sums(0, 16) == DL_ST_OK && g16(&rep[6]) == (0xFFFF & ~((1u << 3) | (1u << 5))));
	CHECK(g32(&rep[8 + 12]) == storage_crc32(0, b, BLOCK));
	CHECK(g32(&rep[8 + 20]) == storage_crc32(0, &sim_flash_data[0x5000], BLOCK));
	CHECK(sums(0x9C000, 1) == DL_ST_OK && g16(&rep[6]) == 0);

	// The host's delta: a cached image whose 0x3000 matches the radio's and whose 0x5000 doesn't
	uint32_t cached[16];
	for (unsigned i = 0; i < 16; i++) {
		cached[i] = blank_crc;
	}
	cached[3] = storage_crc32(0, b, BLOCK);
	CHECK(sums(0, 16) == DL_ST_OK);
	unsigned changed = 0;
	for (unsigned i = 0; i < 16; i++) {
		changed |= g32(&rep[8 + 4 * i]) != cached[i] ? 1u << i : 0u;
	}
	CHECK(changed == (1u << 5));	// only 0x5000 moves
	CHECK(sums(0, 17) == DL_ST_BAD_ARG && sums(0, 0) == DL_ST_BAD_ARG && sums(0x1BF000, 2) == DL_ST_REFUSED);

	// READ: LZ4 (one window for the codeplug block, five for the random one), nothing for a blank one
	CHECK(read_block(0x3000, got, &fmt, &reqs) && fmt == CPSF_FMT_LZ4 && memcmp(got, b, BLOCK) == 0 && reqs == 1);
	CHECK(read_block(0x5000, got, &fmt, &reqs) && fmt == CPSF_FMT_LZ4 && reqs == 5);
	CHECK(memcmp(got, &sim_flash_data[0x5000], BLOCK) == 0);
	CHECK(read_block(0x4000, got, &fmt, &reqs) && fmt == CPSF_FMT_BLANK && reqs == 1 && rep_len == 15);
	CHECK(read_block(0x9C000, got, &fmt, &reqs) && fmt == CPSF_FMT_LZ4 && got[BLOCK - 1] == 0 && got[0] == 0xFF);
	uint8_t r[7] = { CPSF_OP_READ };
	p32(&r[1], 0x3000);
	p16(&r[5], 4000);
	CHECK(call(r, 7) == DL_ST_BAD_ARG);	// past the stream's end

	// Range refusals (cps-fast.md 6): every op that names a block
	static const uint32_t bad[] = { 0x10000, 0x11000, 0x19000, 0x1F000, 0x1C0000, 0x1FF000, STAGING, 0x3FF000, 0x100,
					0xFFFFF000u };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(sums(bad[i], 1) == DL_ST_REFUSED && refused(CPSF_R_RANGE));
		p32(&r[1], bad[i]);
		p16(&r[5], 0);
		CHECK(call(r, 7) == DL_ST_REFUSED && refused(CPSF_R_RANGE));
	}
	CHECK(sums(0xF000, 2) == DL_ST_REFUSED && refused(CPSF_R_RANGE));	// the second block is the calibration's
	CHECK(sums(0xF000, 1) == DL_ST_OK && sums(0x20000, 1) == DL_ST_OK && sums(0x1BF000, 1) == DL_ST_OK);
	CHECK(call((const uint8_t[]){ 0x7F }, 1) == DL_ST_BAD_ARG && call(r, 0) == DL_ST_BAD_ARG);

	// The live VFO shows in VFO A (EEPROM 0x7590), in the sums and the reads alike, as the open-firmware CPS shows it
	settings_t *st = settings_begin();
	st->vfo_rx_hz = st->vfo_tx_hz = 446006250u;
	settings_end(now);
	CHECK(read_block(0x7000, got, &fmt, &reqs) && fmt == CPSF_FMT_LZ4);
	CHECK(got[0x590 + 0x10] == 0x25 && got[0x590 + 0x13] == 0x44);	// 446.00625 MHz: BCD 44600625, little endian
	CHECK(sums(0x7000, 1) == DL_ST_OK && (g16(&rep[6]) & 1) == 0 && g32(&rep[8]) == storage_crc32(0, got, BLOCK));
	printf("# INFO, SUMS and the host's delta, READ (LZ4, blank), %u refused ranges, the VFO overlay\n",
	       (unsigned)(sizeof(bad) / sizeof(bad[0])));
}

// --- writing ---

static uint8_t last_format;

// Send a block as a host would: blank, or LZ4 (from the test's own encoder); DATA in pieces of piece bytes. Returns
// END's status (or the first one that wasn't OK)
static uint8_t write_block(uint32_t addr, const uint8_t *data, unsigned piece)
{
	static uint8_t stream[2 * BLOCK];
	uint8_t r[64];
	uint32_t n = 0;
	bool blank = true;
	for (unsigned i = 0; i < BLOCK && blank; i++) {
		blank = data[i] == 0xFF;
	}
	last_format = CPSF_FMT_BLANK;
	if (!blank) {
		n = slow_encode(data, BLOCK, stream);
		last_format = CPSF_FMT_LZ4;
	}
	r[0] = CPSF_OP_BLOCK;
	p32(&r[1], addr);
	r[5] = last_format;
	p16(&r[6], n);
	p32(&r[8], storage_crc32(0, data, BLOCK));
	if (call(r, 12) != DL_ST_OK) {
		return rep[0];
	}
	for (uint32_t off = 0; off < n; off += piece) {
		uint32_t k = n - off < piece ? n - off : piece;
		r[0] = CPSF_OP_DATA;
		p16(&r[1], off);
		memcpy(&r[3], stream + off, k);
		if (call(r, 3 + k) != DL_ST_OK) {
			return rep[0];
		}
		if (g16(&rep[1]) != off + k) {
			return 0xFE;
		}
	}
	return op(CPSF_OP_END);
}

static void test_direct_write(void)
{
	static uint8_t b[BLOCK], c[BLOCK], got[BLOCK];
	uint8_t fmt;
	unsigned reqs;
	uint8_t r[64];

	boot(&dm1701_map, true);
	codeplug_block(b, 30);
	CHECK(write_block(0x3000, b, 61) == DL_ST_REFUSED && refused(CPSF_R_STATE));	// no session
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_OK && g16(&rep[1]) == 0 && state() == CPSF_ST_OPEN);
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_REFUSED && refused(CPSF_R_STATE));	// one at a time
	uint32_t serial = codeplug_serial();
	CHECK(write_block(0x3000, b, 61) == DL_ST_OK && last_format == CPSF_FMT_LZ4);
	CHECK(memcmp(&sim_flash_data[0x3000], b, BLOCK) == 0);	// direct: in place at END
	random_block(c);
	CHECK(write_block(0x9B000, c, 61) == DL_ST_OK && last_format == CPSF_FMT_LZ4);	// incompressible
	CHECK(memcmp(&sim_flash_data[0x9B000], c, BLOCK) == 0);
	memset(c, 0xFF, BLOCK);
	CHECK(write_block(0x5000, c, 61) == DL_ST_OK && last_format == CPSF_FMT_BLANK);
	CHECK(write_block(0x3000, b, 7) == DL_ST_OK);	// unchanged: the journal skips it
	CHECK(state() == CPSF_ST_OPEN && g16(&rep[3]) == 4);

	// Refusals: the radio's own areas, a CRC that doesn't match, a corrupt stream, a gap
	static const uint32_t bad[] = { 0x10000, 0x11000, 0x19000, 0x1C0000, STAGING, 0x123 };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(write_block(bad[i], b, 61) == DL_ST_REFUSED && refused(CPSF_R_RANGE));
	}
	static uint8_t cs[2 * BLOCK];
	random_block(c);
	uint32_t cn = cps_lz4_encode(c, BLOCK, cs, 0, sizeof(cs));
	r[0] = CPSF_OP_BLOCK;
	p32(&r[1], 0x6000);
	r[5] = CPSF_FMT_LZ4;
	p16(&r[6], cn);
	p32(&r[8], storage_crc32(0, c, BLOCK) ^ 1u);	// the wrong CRC
	CHECK(call(r, 12) == DL_ST_OK && state() == CPSF_ST_BLOCK);
	for (uint32_t off = 0; off < cn; off += 61) {
		uint32_t k = cn - off < 61 ? cn - off : 61;
		r[0] = CPSF_OP_DATA;
		p16(&r[1], off);
		memcpy(&r[3], cs + off, k);
		CHECK(call(r, 3 + k) == DL_ST_OK);
		if (off == 61) {
			CHECK(call(r, 3 + k) == DL_ST_OK && g16(&rep[1]) == 2 * 61);	// a repeat: answered, not taken twice
			p16(&r[1], 0);
			CHECK(call(r, 3 + 10) == DL_ST_OK && g16(&rep[1]) == 2 * 61);	// an old piece
			p16(&r[1], off);
		}
	}
	CHECK(op(CPSF_OP_END) == DL_ST_REFUSED && refused(CPSF_R_CRC));
	CHECK(sim_flash_data[0x6000] == 0xFF && state() == CPSF_ST_OPEN);	// nothing written
	CHECK(block_req(0x6000, CPSF_FMT_LZ4, 100, 0) == DL_ST_OK);
	r[0] = CPSF_OP_DATA;
	p16(&r[1], 10);
	CHECK(call(r, 3 + 5) == DL_ST_REFUSED && refused(CPSF_R_STREAM));	// a gap
	p16(&r[1], 0);
	static const uint8_t bad_stream[] = { 0x14, 'a', 0x00, 0x00, 0x50 };	// offset 0
	memcpy(&r[3], bad_stream, sizeof(bad_stream));
	CHECK(call(r, 3 + sizeof(bad_stream)) == DL_ST_REFUSED && refused(CPSF_R_STREAM));
	CHECK(state() == CPSF_ST_OPEN);	// the block was dropped
	CHECK(block_req(0x6000, CPSF_FMT_LZ4, 4, 0) == DL_ST_OK);
	r[0] = CPSF_OP_DATA;
	p16(&r[1], 0);
	memcpy(&r[3], "\x30\x01\x02\x03", 4);	// whole, but 3 bytes, not 4 KB
	CHECK(call(r, 3 + 4) == DL_ST_OK && op(CPSF_OP_END) == DL_ST_REFUSED && refused(CPSF_R_STREAM));
	CHECK(block_req(0x6000, 0, BLOCK, 0) == DL_ST_BAD_ARG);	// no raw format
	CHECK(block_req(0x6000, CPSF_FMT_BLANK, 1, 0) == DL_ST_BAD_ARG);	// a blank block has no stream
	CHECK(block_req(0x6000, CPSF_FMT_LZ4, 0, 0) == DL_ST_BAD_ARG);
	CHECK(block_req(0x6000, CPSF_FMT_LZ4, 5000, 0) == DL_ST_BAD_ARG);

	// While a block is being written, READ waits for the shared buffer
	CHECK(block_req(0x6000, CPSF_FMT_BLANK, 0, 0) == DL_ST_OK && storage_buffer_acquire() == NULL);
	uint8_t rr[7] = { CPSF_OP_READ };
	p32(&rr[1], 0x3000);
	CHECK(call(rr, 7) == DL_ST_BUSY);
	CHECK(op(CPSF_OP_ABORT) == DL_ST_OK && state() == CPSF_ST_IDLE);
	uint8_t *buf = storage_buffer_acquire();
	CHECK(buf != NULL);	// released
	storage_buffer_release();
	CHECK(codeplug_serial() != serial);	// the session wrote: the codeplug was re-read at its end

	// COMMIT ends a direct session; the blocks read back
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_OK);
	codeplug_block(c, 50);
	CHECK(write_block(0x8000, c, 61) == DL_ST_OK);
	serial = codeplug_serial();
	CHECK(op(CPSF_OP_COMMIT) == DL_ST_OK && state() == CPSF_ST_DONE && codeplug_serial() != serial);
	CHECK(read_block(0x8000, got, &fmt, &reqs) && memcmp(got, c, BLOCK) == 0);
	CHECK(read_block(0x3000, got, &fmt, &reqs) && memcmp(got, b, BLOCK) == 0);
	CHECK(op(CPSF_OP_COMMIT) == DL_ST_REFUSED && refused(CPSF_R_STATE));

	// The session time-out lets go of the buffer
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_OK);
	r[0] = CPSF_OP_BLOCK;
	p32(&r[1], 0x3000);
	r[5] = CPSF_FMT_BLANK;
	p16(&r[6], 0);
	CHECK(call(r, 12) == DL_ST_OK);
	cps_fast_tick(now + CPSF_SESSION_MS / 2);
	CHECK(state() == CPSF_ST_BLOCK);
	cps_fast_tick(now + CPSF_SESSION_MS + 1);
	CHECK(state() == CPSF_ST_IDLE && memcmp(&sim_flash_data[0x3000], b, BLOCK) == 0);
	buf = storage_buffer_acquire();
	CHECK(buf != NULL);
	storage_buffer_release();

	// The counters the debug link's storage view shows
	cps_fast_stats_t fs;
	cps_fast_get_stats(&fs);
	CHECK(fs.blocks_written == 5 && fs.refused > 0 && fs.commits >= 2);
	printf("# direct writes: LZ4, incompressible, blank, repeats, refusals, abort, commit, time-out\n");
}

// The offset layout (the HR_C7000 radios): addresses stay image offsets, no staging, a calibration window inside the
// flash image refused
static void test_offset_layout(void)
{
	static uint8_t b[BLOCK], got[BLOCK];
	uint8_t fmt;
	unsigned reqs;

	boot(&offset_map, true);
	CHECK(op(CPSF_OP_INFO) == DL_ST_OK && rep[2] == 2 && g32(&rep[21]) == 0x200000u && g16(&rep[25]) == 0);
	CHECK(g32(&rep[13]) == 0x20000u && g32(&rep[17]) == 0x1C0000u);
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_REFUSED && refused(CPSF_R_NO_STAGING));
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_OK);
	codeplug_block(b, 12);
	CHECK(write_block(0x3000, b, 61) == DL_ST_OK);
	CHECK(memcmp(&sim_flash_data[0x203000], b, BLOCK) == 0);	// at openfw_eeprom + 0x3000
	CHECK(write_block(0x30000, b, 61) == DL_ST_REFUSED && refused(CPSF_R_RANGE));	// the calibration window
	CHECK(write_block(0x2F000, b, 61) == DL_ST_OK);
	CHECK(op(CPSF_OP_COMMIT) == DL_ST_OK);
	CHECK(read_block(0x3000, got, &fmt, &reqs) && memcmp(got, b, BLOCK) == 0);
	CHECK(sums(0x30000, 1) == DL_ST_REFUSED);
	printf("# image at +0x200000: offsets on the wire, direct writes only, the calibration block refused\n");
}

static bool tick_until_done(int max)
{
	for (int i = 0; i < max; i++) {
		cps_fast_tick(now++);
		uint8_t st = state();
		if (st == CPSF_ST_DONE || st == CPSF_ST_FAILED) {
			return st == CPSF_ST_DONE;
		}
	}
	return false;
}

static void test_staged_write(void)
{
	static uint8_t b[4][BLOCK], got[BLOCK], old[BLOCK];
	static const uint32_t at[4] = { 0x3000, 0x7000, 0x20000, 0x9C000 };
	uint8_t fmt;
	unsigned reqs;

	boot(&dm1701_map, true);
	codeplug_block(old, 5);
	for (unsigned i = 0; i < 4; i++) {
		memcpy(&sim_flash_data[at[i]], old, BLOCK);
		codeplug_block(b[i], 10 + 20 * i);
	}
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK && g16(&rep[1]) == 63);
	CHECK(cps_fast_staging_busy());
	const char *msg = NULL;
	CHECK(tftp_codeplug_file.open_write(0, &msg) != TFTP_OK && msg != NULL);	// TFTP waits
	for (unsigned i = 0; i < 4; i++) {
		CHECK(write_block(at[i], b[i], 61) == DL_ST_OK);
	}
	CHECK(write_block(at[1], b[1], 61) == DL_ST_OK);	// again: the same slot
	for (unsigned i = 0; i < 4; i++) {
		CHECK(memcmp(&sim_flash_data[at[i]], old, BLOCK) == 0);	// nothing in place yet
		CHECK(read_block(at[i], got, &fmt, &reqs) && memcmp(got, old, BLOCK) == 0);
	}
	CHECK(state() == CPSF_ST_OPEN && g16(&rep[3]) == 5);
	uint32_t serial = codeplug_serial();
	CHECK(op(CPSF_OP_COMMIT) == DL_ST_OK && state() == CPSF_ST_COMMITTING);
	CHECK(g32(&sim_flash_data[STAGING]) == STORAGE_STAGE_MAGIC_CPSF && g32(&sim_flash_data[STAGING + 8]) == 4);
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_BUSY && op(CPSF_OP_ABORT) == DL_ST_BUSY);
	CHECK(tick_until_done(20) && g16(&rep[5]) == 4);
	for (unsigned i = 0; i < 4; i++) {
		CHECK(memcmp(&sim_flash_data[at[i]], b[i], BLOCK) == 0);
	}
	CHECK(g32(&sim_flash_data[STAGING]) == 0xFFFFFFFFu && codeplug_serial() != serial && !cps_fast_staging_busy());

	// An abandoned staged session changes nothing
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK);
	CHECK(write_block(at[0], old, 61) == DL_ST_OK);
	CHECK(op(CPSF_OP_ABORT) == DL_ST_OK && memcmp(&sim_flash_data[at[0]], b[0], BLOCK) == 0);

	// More blocks than the staging area holds
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK);
	unsigned taken = 0;
	for (uint32_t a = 0x20000; a < 0x20000 + 64 * BLOCK; a += BLOCK) {
		if (write_block(a, b[0], 61) == DL_ST_OK) {
			taken++;
		}
	}
	CHECK(taken == 63 && refused(CPSF_R_STAGE_FULL));
	CHECK(write_block(0x20000, b[1], 61) == DL_ST_OK);	// a block already staged still takes a new version
	CHECK(op(CPSF_OP_ABORT) == DL_ST_OK);

	// A TFTP write holds the staging area: a staged session waits, a direct one doesn't
	CHECK(tftp_codeplug_file.open_write(0, &msg) == TFTP_OK && tftp_codeplug_writing());
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_REFUSED && refused(CPSF_R_STAGE_BUSY));
	CHECK(begin(CPSF_MODE_DIRECT) == DL_ST_OK && op(CPSF_OP_ABORT) == DL_ST_OK);
	tftp_codeplug_file.abort();
	CHECK(!tftp_codeplug_writing());

	// A TFTP record left for the next boot: ours leaves it alone (and refuses to stage over it)
	p32(&sim_flash_data[STAGING], STORAGE_STAGE_MAGIC_TFTP);
	CHECK(!cps_fast_resume() && g32(&sim_flash_data[STAGING]) == STORAGE_STAGE_MAGIC_TFTP);
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_REFUSED && refused(CPSF_R_STAGE_BUSY));
	sim_flash.erase(STAGING);
	// An erased record sector is left as it is at BEGIN (REC_BLANK_CRC: the CRC-32 of 148 bytes of 0xFF)...
	uint8_t ff[16 + 2 * CPSF_STAGE_MAX + 4];
	memset(ff, 0xFF, sizeof(ff));
	CHECK(storage_crc32(0, ff, sizeof(ff)) == 0x54BDF9D4u);
	uint32_t erases = sim_flash_erases(STAGING);
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK && sim_flash_erases(STAGING) == erases);
	CHECK(op(CPSF_OP_ABORT) == DL_ST_OK);
	// ... a torn one (an erased first word, the rest not) is erased
	sim_flash_data[STAGING + 20] = 0x12;
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK && sim_flash_data[STAGING + 20] == 0xFF);
	CHECK(sim_flash_erases(STAGING) == erases + 1);
	CHECK(op(CPSF_OP_ABORT) == DL_ST_OK);
	printf("# staged writes: nothing in place before COMMIT, the copy by ticks, slots reused, 63 at most, TFTP shares the area\n");
}

// Power cuts all through a staged write and its commit: after the restart (storage_init, cps_fast_resume) the four
// blocks are all old or all new, never a mix, and the record is gone. Then direct writes: each block old or new
static void test_power_cuts(void)
{
	static uint8_t newb[4][BLOCK], old[4][BLOCK];
	static const uint32_t at[4] = { 0x3000, 0x8000, 0x21000, 0xA0000 };
	int olds = 0, news = 0, cuts = 0, resumed = 0;

	for (unsigned i = 0; i < 4; i++) {
		codeplug_block(old[i], 3 + i);
		codeplug_block(newb[i], 30 + 7 * i);
	}
	for (uint32_t cut = 1; cut < 60000; cut = cut < 64 ? cut * 2 : cut + 1499) {
		boot(&dm1701_map, true);
		for (unsigned i = 0; i < 4; i++) {
			memcpy(&sim_flash_data[at[i]], old[i], BLOCK);
		}
		sim_flash_cut_after(cut);
		if (begin(CPSF_MODE_STAGED) == DL_ST_OK) {
			bool ok = true;
			for (unsigned i = 0; i < 4 && ok; i++) {
				ok = write_block(at[i], newb[i], 61) == DL_ST_OK;
			}
			if (ok && op(CPSF_OP_COMMIT) == DL_ST_OK) {
				tick_until_done(20);
			}
		}
		bool was_cut = sim_flash_is_cut();
		sim_flash_cut_after(0);
		sim_flash_power_on();
		cuts += was_cut;
		// Power on: the journal, then our record
		storage_buffer_release();
		storage_init();
		resumed += cps_fast_resume();
		unsigned o = 0, n = 0;
		for (unsigned i = 0; i < 4; i++) {
			o += memcmp(&sim_flash_data[at[i]], old[i], BLOCK) == 0;
			n += memcmp(&sim_flash_data[at[i]], newb[i], BLOCK) == 0;
		}
		CHECK(o == 4 || n == 4);
		CHECK(g32(&sim_flash_data[STAGING]) == 0xFFFFFFFFu);
		olds += o == 4;
		news += n == 4;
	}
	CHECK(cuts >= 35 && olds > 5 && news > 5 && resumed > 3);
	printf("# %d power cuts through a staged write: %d left the old blocks, %d the new ones (%d finished at boot)\n",
	       cuts, olds, news, resumed);

	// Direct writes: the journal keeps each block whole, old or new
	int direct = 0;
	for (uint32_t cut = 1; cut < 40000; cut = cut < 64 ? cut * 2 : cut + 1999) {
		boot(&dm1701_map, true);
		for (unsigned i = 0; i < 4; i++) {
			memcpy(&sim_flash_data[at[i]], old[i], BLOCK);
		}
		sim_flash_cut_after(cut);
		if (begin(CPSF_MODE_DIRECT) == DL_ST_OK) {
			for (unsigned i = 0; i < 4; i++) {
				if (write_block(at[i], newb[i], 61) != DL_ST_OK) {
					break;
				}
			}
		}
		direct += sim_flash_is_cut();
		sim_flash_cut_after(0);
		sim_flash_power_on();
		storage_buffer_release();
		storage_init();
		cps_fast_resume();
		for (unsigned i = 0; i < 4; i++) {
			CHECK(memcmp(&sim_flash_data[at[i]], old[i], BLOCK) == 0 ||
			      memcmp(&sim_flash_data[at[i]], newb[i], BLOCK) == 0);
		}
	}
	CHECK(direct >= 15);
	printf("# %d power cuts through direct writes: every block old or new\n", direct);
}

// A record the boot can't trust is erased and nothing is copied; one whose staged data changed after it was written
// too
static void test_bad_records(void)
{
	static uint8_t b[BLOCK], old[BLOCK];

	boot(&dm1701_map, true);
	codeplug_block(old, 4);
	codeplug_block(b, 44);
	memcpy(&sim_flash_data[0x3000], old, BLOCK);
	CHECK(begin(CPSF_MODE_STAGED) == DL_ST_OK && write_block(0x3000, b, 61) == DL_ST_OK);
	CHECK(op(CPSF_OP_COMMIT) == DL_ST_OK);	// the record is written; no tick: the "power cut" comes before the copy
	static uint8_t rec[64], slot[BLOCK];
	memcpy(rec, &sim_flash_data[STAGING], sizeof(rec));
	memcpy(slot, &sim_flash_data[STAGING + BLOCK], BLOCK);

	// A bit flipped in the staged block: the record's data CRC catches it
	sim_flash_data[STAGING + BLOCK + 100] ^= 0x01;
	storage_buffer_release();
	storage_init();
	CHECK(!cps_fast_resume() && g32(&sim_flash_data[STAGING]) == 0xFFFFFFFFu);
	CHECK(memcmp(&sim_flash_data[0x3000], old, BLOCK) == 0);

	// A record whose block list points at the settings ring: refused even with a valid CRC
	memcpy(&sim_flash_data[STAGING + BLOCK], slot, BLOCK);
	memcpy(&sim_flash_data[STAGING], rec, sizeof(rec));
	p16(&sim_flash_data[STAGING + 16], 0x11);	// block 0x11 = 0x11000
	p32(&sim_flash_data[STAGING + 18], storage_crc32(0, &sim_flash_data[STAGING], 18));
	uint8_t settings0 = sim_flash_data[0x11000];
	CHECK(!cps_fast_resume() && g32(&sim_flash_data[STAGING]) == 0xFFFFFFFFu);
	CHECK(sim_flash_data[0x11000] == settings0);

	// The good record: finished at boot
	memcpy(&sim_flash_data[STAGING], rec, sizeof(rec));
	CHECK(cps_fast_resume() && memcmp(&sim_flash_data[0x3000], b, BLOCK) == 0);
	CHECK(g32(&sim_flash_data[STAGING]) == 0xFFFFFFFFu);
	// ... and TFTP's boot resume leaves our record alone
	memcpy(&sim_flash_data[STAGING], rec, sizeof(rec));
	CHECK(!tftp_codeplug_resume() && g32(&sim_flash_data[STAGING]) == STORAGE_STAGE_MAGIC_CPSF);
	CHECK(cps_fast_staging_busy());
	const char *msg = NULL;
	CHECK(tftp_codeplug_file.open_write(0, &msg) != TFTP_OK);
	CHECK(cps_fast_resume() && !cps_fast_staging_busy());
	printf("# boot records: a changed slot, a protected block, the good one, TFTP leaves ours\n");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);	// the lines before a crash
	sim_flash_create(FLASH_SIZE, &dm1701_map);
	test_lz4();
	test_info_and_reads();
	test_direct_write();
	test_offset_layout();
	test_staged_write();
	test_power_cuts();
	test_bad_records();
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
	printf("# cpsFastTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
