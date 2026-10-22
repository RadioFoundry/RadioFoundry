// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The STM32F4 flash stub's protocol engine (stub/stm32f4/f4stub_proto.c, docs/spec/stub-stm32f4.md) against a
// simulated 1 MB flash and a 1 MB SPI flash: framing and resync, every op's checks, the bootloader guard, the sector
// 3 interlock, blank checks and read-back, a whole update in the order the host tool uses, and the SPI flash's reads,
// erases and programs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "f4stub.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                                            \
	do {                                                                                                        \
		checks++;                                                                                           \
		if (!(cond)) {                                                                                      \
			failures++;                                                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                 \
			printf(__VA_ARGS__);                                                                        \
			printf("\n");                                                                               \
		}                                                                                                   \
	} while (0)

#define FLASH_SIZE	0x100000u
#define SRAM_SIZE	0x20000u

static uint8_t flash[FLASH_SIZE];
static uint8_t sram[SRAM_SIZE];
static int erases[12];
static bool fail_program;	// the controller "programs" one bit wrong
static bool fail_erase;
static int resets;
static int feeds;		// the watchdog feeds the engine asked for
static int feeds_at_erase;	// feeds counted when the last erase / program started
static int feeds_at_program;

#define SPI_SIZE	0x100000u
static uint8_t spi[SPI_SIZE];
static bool spi_dead;		// the chip stops answering
static bool spi_flip;		// a programmed byte reads back wrong
static int spi_erases;

// What the engine sent: the frames, concatenated
static uint8_t sent[64 * 1024];
static uint32_t sent_len;

static void sim_write(void *ctx, const uint8_t *buf, uint32_t n)
{
	(void)ctx;
	if (sent_len + n <= sizeof(sent)) {
		memcpy(sent + sent_len, buf, n);
		sent_len += n;
	}
}

static void sim_read(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	if (addr >= F4_FLASH_BASE && addr - F4_FLASH_BASE + n <= FLASH_SIZE) {
		memcpy(dst, flash + (addr - F4_FLASH_BASE), n);
	} else if (addr >= 0x20000000u && addr - 0x20000000u + n <= SRAM_SIZE) {
		memcpy(dst, sram + (addr - 0x20000000u), n);
	} else {
		memset(dst, 0xA5, n);	// system memory, option bytes, CCM: something recognizable
	}
}

static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	if (fail_erase) {
		return false;
	}
	erases[sector]++;
	feeds_at_erase = feeds;
	memset(flash + (f4_sector_addr(sector) - F4_FLASH_BASE), 0xFF, f4_sector_size(sector));
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	feeds_at_program = feeds;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t w = fail_program && i == n / 2 ? words[i] & (words[i] - 1u) : words[i];	// lowest set bit lost
		uint8_t *p = flash + (addr + 4u * i - F4_FLASH_BASE);
		uint32_t old;
		memcpy(&old, p, 4);
		old &= w;	// programming only clears bits
		memcpy(p, &old, 4);
	}
	return true;
}

static void sim_feed(void *ctx)
{
	(void)ctx;
	feeds++;
}

static void sim_reset(void *ctx)
{
	(void)ctx;
	resets++;
}

static bool sim_xread(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	if (spi_dead || addr + n > SPI_SIZE) {
		return false;
	}
	memcpy(dst, spi + addr, n);
	return true;
}

static bool sim_xerase(void *ctx, uint32_t addr)
{
	(void)ctx;
	spi_erases++;
	memset(spi + addr, 0xFF, F4STUB_BLOCK);
	return !spi_dead;
}

static bool sim_xprogram(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		spi[addr + i] &= data[i];
	}
	if (spi_flip) {
		spi[addr] ^= 0x01;
	}
	return !spi_dead;
}

static f4stub_port_t port = {
	.write = sim_write,
	.read = sim_read,
	.erase = sim_erase,
	.program = sim_program,
	.reset = sim_reset,
	.flash_kb = 1024,
	.rdp = 0x55,
	.flags = F4_HELLO_USB_ADOPTED,
	.xread = sim_xread,
	.xerase = sim_xerase,
	.xprogram = sim_xprogram,
	.feed = sim_feed,
	.xsize = SPI_SIZE,
	.xid = { 0xEF, 0x40, 0x14 },
};
static f4stub_t eng;

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t frame(uint8_t *out, uint8_t op, const uint8_t *payload, uint16_t n)
{
	uint8_t sum = 0;
	for (uint16_t i = 0; i < n; i++) {
		sum = (uint8_t)(sum + payload[i]);
	}
	out[0] = 0x02;
	out[1] = op;
	out[2] = (uint8_t)(n >> 8);
	out[3] = (uint8_t)n;
	out[4] = sum;
	out[5] = 0x03;
	if (n > 0) {
		memcpy(out + 6, payload, n);
	}
	return 6u + n;
}

// Send one request; returns the first reply's status, its payload in *body (pointing into sent) and length in *blen
static uint8_t request(uint8_t op, const uint8_t *payload, uint16_t n, const uint8_t **body, uint32_t *blen)
{
	static uint8_t buf[F4STUB_FRAME_HDR + F4STUB_MAX_PAYLOAD + 16];
	uint32_t len = frame(buf, op, payload, n);
	sent_len = 0;
	f4stub_feed(&eng, buf, len);
	if (sent_len < 6) {
		return 0xFF;
	}
	uint32_t bl = ((uint32_t)sent[2] << 8) | sent[3];
	if (body != NULL) {
		*body = sent + 6;
	}
	if (blen != NULL) {
		*blen = bl;
	}
	return sent[1];
}

static uint8_t simple(uint8_t op, const uint8_t *payload, uint16_t n)
{
	return request(op, payload, n, NULL, NULL);
}

static uint8_t erase(unsigned sector, uint32_t magic)
{
	uint8_t p[5] = { (uint8_t)sector };
	put32(p + 1, magic);
	return simple(F4OP_ERASE, p, 5);
}

static uint8_t program(uint32_t addr, const uint8_t *data, uint32_t n)
{
	static uint8_t p[4 + F4STUB_BLOCK + 8];
	put32(p, addr);
	memcpy(p + 4, data, n);
	return simple(F4OP_PROGRAM, p, (uint16_t)(4u + n));
}

static uint8_t *at(uint32_t addr)
{
	return flash + (addr - F4_FLASH_BASE);
}

static void reset_flash(void)
{
	memset(flash, 0xFF, sizeof(flash));
	for (uint32_t i = 0; i < 0xC000u; i++) {
		flash[i] = (uint8_t)(i * 7u + 1u);	// "the bootloader"
	}
	for (uint32_t i = 0x0C000u; i < 0x60000u; i++) {
		flash[i] = (uint8_t)(i ^ (i >> 8));	// an installed application
	}
	uint32_t sp = 0x2001FFFCu;
	memcpy(at(F4_APP_BASE), &sp, 4);
	memset(erases, 0, sizeof(erases));
	fail_program = fail_erase = false;
}

static void test_crc_and_geometry(void)
{
	CHECK(f4stub_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value");
	CHECK(f4_sector_count(1024) == 12, "1 MB: 12 sectors");
	CHECK(f4_sector_count(512) == 8, "512 KB: 8 sectors");
	CHECK(f4_sector_count(128) == 5, "128 KB: 5 sectors");
	CHECK(f4_sector_count(64) == 4 && f4_sector_count(32) == 2, "small parts");
	CHECK(f4_sector_addr(3) == 0x0800C000u && f4_sector_size(3) == 0x4000u, "sector 3");
	CHECK(f4_sector_addr(4) == 0x08010000u && f4_sector_size(4) == 0x10000u, "sector 4");
	CHECK(f4_sector_addr(9) == 0x080A0000u && f4_sector_size(9) == 0x20000u, "sector 9");
	CHECK(f4_sector_of(0x0800BFFFu, 1024) == 2 && f4_sector_of(0x0800C000u, 1024) == 3, "the boundary");
	CHECK(f4_sector_of(0x080FFFFFu, 1024) == 11 && f4_sector_of(0x08100000u, 1024) == -1, "the end");
	CHECK(f4_sector_of(0x08080000u, 512) == -1, "past a 512 KB part");
}

static void test_hello_and_framing(void)
{
	const uint8_t *b;
	uint32_t n;
	CHECK(request(F4OP_HELLO, NULL, 0, &b, &n) == F4ST_OK, "hello");
	CHECK(n == 24 && memcmp(b, "F4STUB", 6) == 0, "hello magic (%u bytes)", (unsigned)n);
	CHECK(b[6] == F4STUB_VERSION && ((b[7] << 8) | b[8]) == F4STUB_MAX_PAYLOAD, "version, max payload");
	CHECK(((b[9] << 8) | b[10]) == 1024 && b[11] == 0x55 && b[12] == F4_HELLO_USB_ADOPTED, "flash KB, RDP, flags");
	CHECK(get32(b + 13) == F4_APP_BASE, "app base");
	CHECK(get32(b + 17) == SPI_SIZE && b[21] == 0xEF && b[22] == 0x40 && b[23] == 0x14, "SPI flash size and ID");
	uint8_t s = 0;
	for (uint32_t i = 0; i < n; i++) {
		s = (uint8_t)(s + b[i]);
	}
	CHECK(sent[0] == 0x02 && sent[5] == 0x03 && sent[4] == s, "reply frame");

	// Garbage, then a frame split into single bytes: one reply
	uint8_t buf[32];
	uint32_t len = frame(buf, F4OP_HELLO, NULL, 0);
	static const uint8_t junk[] = { 0x00, 0x7E, 0x02, 0x55, 0x02, 0x01, 0x00 };
	sent_len = 0;
	f4stub_feed(&eng, junk, sizeof(junk));
	f4stub_idle(&eng);
	for (uint32_t i = 0; i < len; i++) {
		f4stub_feed(&eng, buf + i, 1);
	}
	CHECK(sent_len == 6 + 24 && sent[1] == F4ST_OK, "resync after junk (%u bytes sent)", (unsigned)sent_len);

	// A bad header byte 5 inside what looked like a frame start: resynchronizes on the next 0x02
	uint8_t two[64];
	two[0] = 0x02;
	two[1] = 0x11;
	uint32_t l2 = frame(two + 2, F4OP_HELLO, NULL, 0);
	sent_len = 0;
	f4stub_feed(&eng, two, 2 + l2);
	CHECK(sent_len == 6 + 24 && sent[1] == F4ST_OK, "resync on a false start (%u)", (unsigned)sent_len);

	// Bad checksum
	uint8_t p[6] = { 0x08, 0x00, 0xC0, 0x00, 0x00, 0x10 };
	len = frame(buf, F4OP_PEEK, p, 6);
	buf[4] ^= 1;
	sent_len = 0;
	f4stub_feed(&eng, buf, len);
	CHECK(sent_len == 6 && sent[1] == F4ST_BADSUM, "bad checksum");

	// Too long
	uint8_t hdr[6] = { 0x02, F4OP_PROGRAM, 0x20, 0x00, 0x00, 0x03 };
	sent_len = 0;
	f4stub_feed(&eng, hdr, 6);
	CHECK(sent_len == 6 && sent[1] == F4ST_TOOLONG, "too long");

	// A partial frame dropped by idle: the next frame is answered alone
	sent_len = 0;
	f4stub_feed(&eng, buf, 4);
	f4stub_idle(&eng);
	len = frame(buf, F4OP_HELLO, NULL, 0);
	f4stub_feed(&eng, buf, len);
	CHECK(sent_len == 6 + 24 && sent[1] == F4ST_OK, "idle drops a partial frame");

	CHECK(simple(0x77, NULL, 0) == F4ST_BADOP, "unknown op");
}

static void test_peek(void)
{
	const uint8_t *b;
	uint32_t n;
	uint8_t p[6];
	put32(p, 0x08000010u);
	p[4] = 0;
	p[5] = 16;
	CHECK(request(F4OP_PEEK, p, 6, &b, &n) == F4ST_OK && n == 16 && memcmp(b, flash + 0x10, 16) == 0,
	      "peek the bootloader (CPU reads work at RDP 1)");
	put32(p, 0x1FFF7A10u);
	p[5] = 12;
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_OK, "peek the UID");
	put32(p, 0x1FFFC000u);
	p[5] = 16;
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_OK, "peek the option bytes");
	put32(p, 0x2001FFF0u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_OK, "peek SRAM");
	put32(p, 0x2001FFF8u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_REFUSED, "peek past SRAM");
	put32(p, 0x40023C00u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_REFUSED, "peek a peripheral");
	put32(p, 0x50000000u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_REFUSED, "peek the USB core");
	put32(p, 0x080FFFF8u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_REFUSED, "peek past flash");
	p[4] = 0;
	p[5] = 0;
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_BADARG, "peek 0 bytes");
	CHECK(simple(F4OP_PEEK, p, 5) == F4ST_BADARG, "peek short request");
	put32(p, 0x08000000u);
	p[4] = (uint8_t)((F4STUB_MAX_PAYLOAD + 1u) >> 8);
	p[5] = (uint8_t)(F4STUB_MAX_PAYLOAD + 1u);
	CHECK(simple(F4OP_PEEK, p, 6) == F4ST_BADARG, "peek over the max payload");
}

static void test_sums_and_read(void)
{
	const uint8_t *b;
	uint32_t n;
	uint8_t p[6];
	put32(p, 0x0805E000u);	// two app blocks, then blank sector 7 blocks
	p[4] = 0;
	p[5] = 4;
	CHECK(request(F4OP_SUMS, p, 6, &b, &n) == F4ST_OK && n == 20, "sums");
	CHECK(get32(b) == f4stub_crc32(0, at(0x0805E000u), F4STUB_BLOCK) && b[4] == 0, "sum of an app block");
	CHECK(b[14] == 1 && b[19] == 1, "blank flags in sector 7");
	put32(p, 0x0805E001u);
	CHECK(simple(F4OP_SUMS, p, 6) == F4ST_BADARG, "sums misaligned");
	put32(p, 0x080FF000u);
	p[5] = 2;
	CHECK(simple(F4OP_SUMS, p, 6) == F4ST_BADARG, "sums past the end");
	p[5] = 0;
	CHECK(simple(F4OP_SUMS, p, 6) == F4ST_BADARG, "sums of none");

	// READ: one frame per block, raw or blank
	uint8_t r[6];
	put32(r, 0x0805F000u);
	r[4] = 2;
	r[5] = 1;
	sent_len = 0;
	{
		uint8_t buf[16];
		uint32_t len = frame(buf, F4OP_READ, r, 6);
		f4stub_feed(&eng, buf, len);
	}
	uint32_t l1 = ((uint32_t)sent[2] << 8) | sent[3];
	CHECK(sent[1] == F4ST_OK && l1 == 7 + F4STUB_BLOCK, "read: a raw record");
	CHECK(get32(sent + 6) == f4stub_crc32(0, at(0x0805F000u), F4STUB_BLOCK) && sent[10] == F4_METHOD_RAW,
	      "read: its CRC and method");
	CHECK(memcmp(sent + 13, at(0x0805F000u), F4STUB_BLOCK) == 0, "read: the bytes");
	const uint8_t *f2 = sent + 6 + l1;
	CHECK(f2[1] == F4ST_OK && f2[3] == 7 && f2[10] == F4_METHOD_BLANK && f2[11] == 0 && f2[12] == 0,
	      "read: a blank record");
	CHECK(sent_len == 6 + l1 + 6 + 7, "read: exactly two frames");
	r[4] = 0;
	CHECK(simple(F4OP_READ, r, 6) == F4ST_BADARG, "read none");
}

// The watchdog (docs/spec/watchdog.md 3.1): every request feeds, an erase or a program starts on a fresh feed, and the
// long reads feed once per 4 KB block, so nothing the engine does runs long without one. The target's own busy waits
// (a 128 KB erase, up to 2 s) feed through stub_ms() as well; that part is the hardware's
static void test_feeds(void)
{
	reset_flash();
	uint8_t p[6];
	put32(p, 0x08000000u);		// SUMS of 256 blocks (the whole 1 MB): every block fed
	p[4] = 0x01;
	p[5] = 0x00;
	int f0 = feeds;
	CHECK(simple(F4OP_SUMS, p, 6) == F4ST_OK, "sums of 256 blocks");
	CHECK(feeds - f0 >= 256 + 1, "sums: a feed per block (%d)", feeds - f0);

	put32(p, 0x0805C000u);		// READ of 4 blocks
	p[4] = 4;
	p[5] = 0;
	f0 = feeds;
	CHECK(simple(F4OP_READ, p, 6) == F4ST_OK, "read of 4 blocks");
	CHECK(feeds - f0 >= 4 + 1, "read: a feed per block (%d)", feeds - f0);

	f0 = feeds;
	CHECK(erase(3, F4STUB_MAGIC) == F4ST_OK, "erase sector 3");
	CHECK(feeds_at_erase > f0, "the erase starts on a fresh feed");
	uint8_t w[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	f0 = feeds;
	CHECK(program(0x0800C100u, w, 8) == F4ST_OK, "program 2 words");
	CHECK(feeds_at_program > f0, "the program starts on a fresh feed");

	f0 = feeds;
	CHECK(simple(F4OP_HELLO, NULL, 0) == F4ST_OK, "hello");
	CHECK(feeds - f0 >= 1, "every request feeds");

	port.feed = NULL;		// a port without one: nothing changes
	CHECK(simple(F4OP_HELLO, NULL, 0) == F4ST_OK, "hello without a feed hook");
	port.feed = sim_feed;
}

static void test_erase_rules(void)
{
	reset_flash();
	for (unsigned s = 0; s < 3; s++) {
		CHECK(erase(s, F4STUB_MAGIC) == F4ST_REFUSED, "sector %u is the bootloader", s);
	}
	CHECK(erases[0] + erases[1] + erases[2] == 0, "bootloader never erased");
	CHECK(erase(3, 0x12345678u) == F4ST_REFUSED, "bad magic");
	CHECK(erase(12, F4STUB_MAGIC) == F4ST_BADARG, "no sector 12");
	CHECK(simple(F4OP_ERASE, (const uint8_t *)"\x03", 1) == F4ST_BADARG, "short erase request");
	CHECK(erase(5, F4STUB_MAGIC) == F4ST_REFUSED && erases[5] == 0, "interlock: sector 5 while the app is valid");
	CHECK(erase(3, F4STUB_MAGIC) == F4ST_OK && erases[3] == 1, "sector 3 first");
	CHECK(erase(5, F4STUB_MAGIC) == F4ST_OK && erases[5] == 1, "then sector 5");
	fail_erase = true;
	CHECK(erase(6, F4STUB_MAGIC) == F4ST_FLASHERR, "controller error");
	fail_erase = false;

	// A 512 KB part has 8 sectors
	port.flash_kb = 512;
	CHECK(erase(8, F4STUB_MAGIC) == F4ST_BADARG, "512 KB: no sector 8");
	port.flash_kb = 1024;
}

static void test_program_rules(void)
{
	uint8_t data[F4STUB_BLOCK];
	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i * 3u);
	}
	reset_flash();
	CHECK(program(0x0800B000u, data, 16) == F4ST_REFUSED, "program the bootloader");
	CHECK(program(0x0800BFFCu, data, 8) == F4ST_REFUSED, "program across the bootloader's end");
	CHECK(program(0x08060000u, data, 16) == F4ST_REFUSED, "interlock: blank sector 7 while the app is valid");
	CHECK(program(0x0800C002u, data, 8) == F4ST_BADARG, "misaligned");
	CHECK(program(0x080FFFFCu, data, 8) == F4ST_BADARG, "past the end");
	CHECK(simple(F4OP_PROGRAM, data, 6) == F4ST_BADARG, "length not words");
	CHECK(simple(F4OP_PROGRAM, data, 4) == F4ST_BADARG, "no data");
	CHECK(program(0x0800C100u, data, 16) == F4ST_REFUSED, "not erased: refused, nothing written");

	CHECK(erase(3, F4STUB_MAGIC) == F4ST_OK, "erase 3");
	CHECK(program(0x0800FFF8u, data, 16) == F4ST_REFUSED, "sector 3's tail into sector 4, which is not blank");
	CHECK(program(0x08060000u, data, 16) == F4ST_OK, "blank sector 7 once sector 3 is erased");
	CHECK(memcmp(at(0x08060000u), data, 16) == 0, "written");
	CHECK(program(0x08060000u, data, 16) == F4ST_REFUSED, "twice: not erased any more");
	fail_program = true;
	CHECK(program(0x08061000u, data, 64) == F4ST_FLASHERR, "a wrong bit is found by the read-back");
	fail_program = false;
	CHECK(program(0x08062000u, data, F4STUB_BLOCK) == F4ST_OK, "a whole block");
	CHECK(memcmp(at(0x08062000u), data, F4STUB_BLOCK) == 0, "a whole block written");
}

// The whole update the host tool runs (stubtool.py F4Radio.write_image): erase 3, the other sectors, program them,
// sector 3 from offset 4, then the first word last
static void test_update_order(void)
{
	static uint8_t img[0x54000];	// sectors 3-6
	for (uint32_t i = 0; i < sizeof(img); i++) {
		img[i] = (uint8_t)(i * 13u + 5u);
	}
	uint32_t sp = 0x2001FFFCu;
	memcpy(img, &sp, 4);
	reset_flash();
	CHECK(erase(3, F4STUB_MAGIC) == F4ST_OK, "update: erase 3");
	for (unsigned s = 4; s <= 6; s++) {
		CHECK(erase(s, F4STUB_MAGIC) == F4ST_OK, "update: erase %u", s);
	}
	for (uint32_t off = F4STUB_BLOCK; off < sizeof(img); off += F4STUB_BLOCK) {
		CHECK(program(F4_APP_BASE + off, img + off, F4STUB_BLOCK) == F4ST_OK, "update: block 0x%x", off);
	}
	CHECK(program(F4_APP_BASE + 4u, img + 4, F4STUB_BLOCK - 4u) == F4ST_OK, "update: first block after the word");
	uint32_t w;
	memcpy(&w, at(F4_APP_BASE), 4);
	CHECK(w == 0xFFFFFFFFu, "update: still uncommitted (the bootloader would stay in DFU)");
	CHECK(erase(9, F4STUB_MAGIC) == F4ST_OK, "update: sector 9 still allowed before the commit");
	CHECK(program(F4_APP_BASE, img, 4) == F4ST_OK, "update: the commit word");
	CHECK(memcmp(at(F4_APP_BASE), img, sizeof(img)) == 0, "update: image in place");
	CHECK(erase(9, F4STUB_MAGIC) == F4ST_REFUSED, "after the commit the interlock is back");
	CHECK(erases[7] + erases[8] + erases[10] + erases[11] == 0, "the codec sectors never erased");
	for (uint32_t i = 0; i < 0xC000u; i++) {
		if (flash[i] != (uint8_t)(i * 7u + 1u)) {
			CHECK(0, "the bootloader changed at 0x%x", i);
			break;
		}
	}
}

static uint8_t xsums(uint32_t addr, uint16_t count, const uint8_t **b, uint32_t *n)
{
	uint8_t p[7];
	put32(p, addr);
	p[4] = (uint8_t)(count >> 8);
	p[5] = (uint8_t)count;
	p[6] = F4_MEM_SPI;
	return request(F4OP_SUMS, p, 7, b, n);
}

static uint8_t xerase(uint32_t addr, uint32_t magic)
{
	uint8_t p[8];
	put32(p, addr);
	put32(p + 4, magic);
	return simple(F4OP_XERASE, p, 8);
}

static uint8_t xprogram(uint32_t addr, const uint8_t *data, uint32_t n)
{
	static uint8_t p[4 + F4STUB_BLOCK];
	put32(p, addr);
	memcpy(p + 4, data, n);
	return simple(F4OP_XPROGRAM, p, (uint16_t)(4u + n));
}

static void test_spi(void)
{
	const uint8_t *b;
	uint32_t n;
	for (uint32_t i = 0; i < SPI_SIZE; i++) {
		spi[i] = (uint8_t)(i * 13u + (i >> 12));	// a codeplug
	}
	memset(spi + 0x80000, 0xFF, F4STUB_BLOCK);

	// SUMS and READ on memory 1, addresses from 0
	CHECK(xsums(0x1000, 2, &b, &n) == F4ST_OK && n == 10, "spi sums");
	CHECK(get32(b) == f4stub_crc32(0, spi + 0x1000, F4STUB_BLOCK) && b[4] == 0, "spi sum of a block");
	CHECK(xsums(0x80000, 1, &b, &n) == F4ST_OK && b[4] == 1, "spi blank flag");
	CHECK(xsums(0x0FF000, 2, NULL, NULL) == F4ST_BADARG, "spi sums past the chip");
	CHECK(xsums(0x1001, 1, NULL, NULL) == F4ST_BADARG, "spi sums misaligned");
	uint8_t p7[7] = { 0x08, 0x00, 0xC0, 0x00, 0, 1, 7 };
	CHECK(simple(F4OP_SUMS, p7, 7) == F4ST_BADARG, "an unknown memory");
	uint8_t r[7];
	put32(r, 0x2000);
	r[4] = 1;
	r[5] = 0;
	r[6] = F4_MEM_SPI;
	sent_len = 0;
	{
		uint8_t buf[16];
		uint32_t len = frame(buf, F4OP_READ, r, 7);
		f4stub_feed(&eng, buf, len);
	}
	CHECK(sent[1] == F4ST_OK && get32(sent + 6) == f4stub_crc32(0, spi + 0x2000, F4STUB_BLOCK) &&
	      memcmp(sent + 13, spi + 0x2000, F4STUB_BLOCK) == 0, "spi read");

	// Erase needs the magic and a sector boundary; program needs erased bytes inside one sector, read back
	CHECK(xerase(0x3000, 0x12345678u) == F4ST_REFUSED && spi_erases == 0, "spi erase without the magic");
	CHECK(xerase(0x3100, F4STUB_MAGIC) == F4ST_BADARG, "spi erase off a sector boundary");
	CHECK(xerase(SPI_SIZE, F4STUB_MAGIC) == F4ST_BADARG, "spi erase past the chip");
	CHECK(xerase(0x3000, F4STUB_MAGIC) == F4ST_OK && spi[0x3000] == 0xFF && spi[0x3FFF] == 0xFF, "spi erase");
	uint8_t data[F4STUB_BLOCK];
	for (uint32_t i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(i * 3u + 1u);
	}
	CHECK(xprogram(0x3000, data, F4STUB_BLOCK) == F4ST_OK && memcmp(spi + 0x3000, data, F4STUB_BLOCK) == 0,
	      "spi program a whole sector");
	CHECK(xprogram(0x3000, data, 16) == F4ST_REFUSED, "spi program over written bytes");
	CHECK(xprogram(0x4FF8, data, 16) == F4ST_BADARG, "spi program across a sector");
	CHECK(xerase(0x5000, F4STUB_MAGIC) == F4ST_OK, "spi erase again");
	spi_flip = true;
	CHECK(xprogram(0x5000, data, 64) == F4ST_FLASHERR, "spi program that reads back wrong");
	spi_flip = false;
	spi_dead = true;
	CHECK(xsums(0x1000, 1, NULL, NULL) == F4ST_FLASHERR, "spi chip gone: sums");
	spi_dead = false;

	// No chip: memory 1 is refused, internal flash still works
	port.xsize = 0;
	CHECK(xsums(0x1000, 1, NULL, NULL) == F4ST_BADARG, "no SPI flash: sums refused");
	CHECK(xerase(0x3000, F4STUB_MAGIC) == F4ST_BADARG, "no SPI flash: erase refused");
	CHECK(request(F4OP_HELLO, NULL, 0, &b, &n) == F4ST_OK && get32(b + 17) == 0, "no SPI flash: hello says 0");
	port.xsize = SPI_SIZE;
}

static void test_reset(void)
{
	uint8_t p[4];
	put32(p, 0x11111111u);
	CHECK(simple(F4OP_RESET, p, 4) == F4ST_REFUSED && resets == 0, "reset needs the magic");
	put32(p, F4STUB_MAGIC);
	CHECK(simple(F4OP_RESET, p, 4) == F4ST_OK && resets == 1, "reset after the reply");
}

int main(void)
{
	memset(sram, 0, sizeof(sram));
	f4stub_init(&eng, &port);
	reset_flash();
	test_crc_and_geometry();
	test_hello_and_framing();
	test_peek();
	test_sums_and_read();
	test_erase_rules();
	test_feeds();
	test_program_rules();
	test_update_order();
	test_spi();
	test_reset();
	printf("f4stubTest: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
