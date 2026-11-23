// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash stubs' shared protocol engine (stub/common/stubeng.c) with the K22F target's facts and board record check
// (stub/k22f/k22stub_board.c, docs/spec/stub-k22f.md) against a simulated 512 KB flash and a 2 MB SPI flash: the
// test profile's record and the records refused, the 2 KB map and 8-byte units, the loader below 0x4000 and the last sector
// never erased or programmed, the commit sector interlock on a 2 KB sector, an update in the host tool's order, the SPI
// flash's write floor, and PEEK's ranges.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "k22stub.h"
#include "stubeng.h"

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

#define FLASH_SIZE	0x80000u
#define APP_BASE	0x4000u
#define APP_END		0x7F800u
#define XFLOOR		0x100000u
#define SPI_SIZE	0x200000u

static uint8_t flash[FLASH_SIZE];
static uint8_t spi[SPI_SIZE];
static int erases[256];
static int spi_erases;
static k22_board_t last_board;
static stubeng_port_t port;
static stubeng_t eng;
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
	if (addr + n <= FLASH_SIZE) {
		memcpy(dst, flash + addr, n);
	} else {
		memset(dst, 0xA5, n);	// RAM, the SIM, the FTFA registers: something recognizable
	}
}

static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	erases[sector]++;
	memset(flash + stubeng_sector_addr(&port.flash, sector), 0xFF, stubeng_sector_size(&port.flash, sector));
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t old;
		memcpy(&old, flash + addr + 4u * i, 4);
		old &= words[i];
		memcpy(flash + addr + 4u * i, &old, 4);
	}
	return true;
}

static void sim_reset(void *ctx)
{
	(void)ctx;
}

static bool sim_xread(void *ctx, uint32_t addr, void *dst, uint32_t n)
{
	(void)ctx;
	memcpy(dst, spi + addr, n);
	return true;
}

static bool sim_xerase(void *ctx, uint32_t addr)
{
	(void)ctx;
	spi_erases++;
	memset(spi + addr, 0xFF, STUBENG_BLOCK);
	return true;
}

static bool sim_xprogram(void *ctx, uint32_t addr, const uint8_t *data, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		spi[addr + i] &= data[i];
	}
	return true;
}

static bool sim_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	if (!k22_board_parse(rec, n, &last_board)) {
		return false;
	}
	out->app_base = last_board.app_base;
	out->app_end = last_board.app_end;
	out->xfloor = last_board.xfloor;
	if (last_board.xflash != 0) {
		out->xsize = SPI_SIZE;
		out->xid[0] = 0xEF;
		out->xid[1] = 0x40;
		out->xid[2] = 0x15;
	}
	return true;
}

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

// One request; the first reply's status, its payload in *body
static uint8_t request(uint8_t op, const uint8_t *payload, uint16_t n, const uint8_t **body, uint32_t *blen)
{
	static uint8_t buf[STUBENG_FRAME_HDR + STUBENG_MAX_PAYLOAD + 16];
	uint8_t sum = 0;
	for (uint16_t i = 0; i < n; i++) {
		sum = (uint8_t)(sum + payload[i]);
	}
	buf[0] = 0x02;
	buf[1] = op;
	buf[2] = (uint8_t)(n >> 8);
	buf[3] = (uint8_t)n;
	buf[4] = sum;
	buf[5] = 0x03;
	if (n > 0) {
		memcpy(buf + 6, payload, n);
	}
	sent_len = 0;
	stubeng_feed(&eng, buf, 6u + n);
	if (sent_len < 6) {
		return 0xFF;
	}
	if (body != NULL) {
		*body = sent + 6;
	}
	if (blen != NULL) {
		*blen = ((uint32_t)sent[2] << 8) | sent[3];
	}
	return sent[1];
}

static uint8_t simple(uint8_t op, const uint8_t *payload, uint16_t n)
{
	return request(op, payload, n, NULL, NULL);
}

static uint8_t erase(unsigned sector)
{
	uint8_t p[5] = { (uint8_t)sector };
	put32(p + 1, K22STUB_MAGIC);
	return simple(STUB_OP_ERASE, p, 5);
}

static uint8_t program(uint32_t addr, const uint8_t *data, uint32_t n)
{
	static uint8_t p[4 + STUBENG_BLOCK];
	put32(p, addr);
	memcpy(p + 4, data, n);
	return simple(STUB_OP_PROGRAM, p, (uint16_t)(4u + n));
}

static uint8_t xop(uint8_t op, uint32_t addr, const uint8_t *data, uint32_t n)
{
	static uint8_t p[8 + STUBENG_BLOCK];
	put32(p, addr);
	if (op == STUB_OP_XERASE) {
		put32(p + 4, K22STUB_MAGIC);
		return simple(op, p, 8);
	}
	memcpy(p + 4, data, n);
	return simple(op, p, (uint16_t)(4u + n));
}

// The test profile's record (stub/k22stub.py test_board(), id 0xFE)
static void test_record(uint8_t r[K22_REC_LEN])
{
	memset(r, 0, K22_REC_LEN);
	r[K22_REC_BOARD] = 0xFEu;
	r[K22_REC_FORMAT] = K22_REC_LAYOUT;
	r[K22_REC_XFLASH] = 1;
	r[K22_REC_CS_MODE] = 1;
	put32(r + K22_REC_APP_BASE, APP_BASE);
	put32(r + K22_REC_APP_END, APP_END);
	put32(r + K22_REC_XFLOOR, XFLOOR);
	r[K22_REC_SCK] = 4u * 32u + 5u;		// PTE5
	r[K22_REC_MOSI] = 4u * 32u + 4u;	// PTE4
	r[K22_REC_MISO] = 4u * 32u + 6u;	// PTE6
	r[K22_REC_CS] = 0u * 32u + 19u;		// PTA19
}

static void reset_all(void)
{
	for (uint32_t i = 0; i < FLASH_SIZE; i++) {
		flash[i] = (uint8_t)(i * 7u + (i >> 11));
	}
	uint32_t sp = 0x2000FEF0u;
	memcpy(flash + APP_BASE, &sp, 4);
	for (uint32_t i = 0; i < SPI_SIZE; i++) {
		spi[i] = (uint8_t)(i * 13u + (i >> 12));
	}
	memset(erases, 0, sizeof(erases));
	spi_erases = 0;
	stubeng_init(&eng, &port);
}

static void configure_test(void)
{
	uint8_t r[K22_REC_LEN];
	test_record(r);
	CHECK(simple(STUB_OP_CONFIG, r, sizeof(r)) == STUB_ST_OK, "the test record is taken");
}

static void test_records(void)
{
	reset_all();
	uint8_t r[K22_REC_LEN];
	test_record(r);
	k22_board_t b;
	CHECK(k22_board_parse(r, sizeof(r), &b) && b.board == 0xFEu && b.xflash == 1 && b.cs_released && b.app_base == APP_BASE &&
	      b.app_end == APP_END && b.xfloor == XFLOOR && b.sck == 0x85 && b.mosi == 0x84 && b.miso == 0x86 && b.cs == 0x13,
	      "the test record decodes");
	struct {
		int at;
		uint8_t val;
		const char *what;
	} bad[] = {
		{ K22_REC_BOARD, 0, "board id 0" },		{ K22_REC_FORMAT, 2, "another layout" },
		{ K22_REC_XFLASH, 2, "an unknown flash kind" },	{ K22_REC_CS_MODE, 2, "an unknown chip select mode" },
		{ K22_REC_ZERO, 1, "reserved bytes set" },	{ K22_REC_CS, 0x85, "the chip select on SCK" },
		{ K22_REC_MISO, 0xA0, "a pin past port E" },	{ K22_REC_XFLOOR + 3, 0x10, "a floor off a 4 KB block" },
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		uint8_t x[K22_REC_LEN];
		memcpy(x, r, sizeof(x));
		x[bad[i].at] = bad[i].val;
		CHECK(!k22_board_parse(x, sizeof(x), &b), "refused: %s", bad[i].what);
		CHECK(simple(STUB_OP_CONFIG, x, sizeof(x)) == STUB_ST_BADARG, "the engine answers BADARG: %s", bad[i].what);
	}
	CHECK(!k22_board_parse(r, sizeof(r) - 1u, &b), "refused: short record");
	// What the engine checks itself: the range against the 2 KB map
	struct {
		uint32_t base, end;
		const char *what;
	} ranges[] = {
		{ 0x0000u, APP_END, "sector 0 (the vector table, the flash configuration field)" },
		{ 0x4400u, APP_END, "a base off a sector start" },
		{ APP_BASE, 0x7F400u, "an end off a sector start" },
		{ APP_BASE, APP_BASE, "an empty range" },
		{ APP_BASE, 0x80800u, "an end past the flash" },
	};
	for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		uint8_t x[K22_REC_LEN];
		memcpy(x, r, sizeof(x));
		put32(x + K22_REC_APP_BASE, ranges[i].base);
		put32(x + K22_REC_APP_END, ranges[i].end);
		CHECK(simple(STUB_OP_CONFIG, x, sizeof(x)) == STUB_ST_BADARG, "the engine refuses %s", ranges[i].what);
	}
	// No SPI flash: the pins are not looked at
	uint8_t none[K22_REC_LEN];
	memcpy(none, r, sizeof(none));
	none[K22_REC_XFLASH] = 0;
	none[K22_REC_CS] = none[K22_REC_SCK];
	CHECK(k22_board_parse(none, sizeof(none), &b) && b.xflash == 0, "no SPI flash: the pins are ignored");

	// Accepted: HELLO echoes it, with the chip's facts
	configure_test();
	const uint8_t *body;
	uint32_t blen;
	CHECK(request(STUB_OP_HELLO, NULL, 0, &body, &blen) == STUB_ST_OK && blen == STUBENG_HELLO_LEN + K22_REC_LEN,
	      "HELLO with the echo (%u bytes)", (unsigned)blen);
	CHECK(memcmp(body, K22STUB_NAME, 6) == 0 && body[6] == K22STUB_VERSION, "HELLO name and version");
	CHECK(((body[9] << 8) | body[10]) == 512 && (body[12] & STUB_HELLO_CONFIGURED), "HELLO: 512 KB, configured");
	CHECK(get32(body + 13) == APP_BASE && get32(body + 17) == SPI_SIZE && body[24] == 0xFEu, "HELLO: base, SPI size, id");
	CHECK(memcmp(body + STUBENG_HELLO_LEN, r, K22_REC_LEN) == 0, "HELLO echoes the record");
}

static void test_flash_rules(void)
{
	reset_all();
	uint8_t data[STUBENG_BLOCK];
	memset(data, 0x5A, sizeof(data));
	CHECK(erase(20) == STUB_ST_NOCONFIG && program(0x10000u, data, 8) == STUB_ST_NOCONFIG, "nothing before CONFIG");
	configure_test();
	CHECK(erase(7) == STUB_ST_REFUSED && erase(0) == STUB_ST_REFUSED, "the loader's sectors refused");
	CHECK(erase(255) == STUB_ST_REFUSED, "the last sector (0x7F800, radio data) refused");
	CHECK(erase(20) == STUB_ST_REFUSED && erases[20] == 0, "interlock: the application is committed");
	CHECK(program(0x3FF8u, data, 8) == STUB_ST_REFUSED, "program into the loader refused");
	CHECK(erase(8) == STUB_ST_OK && erases[8] == 1, "the commit sector (8, 0x4000) erases");
	CHECK(erase(254) == STUB_ST_OK, "the sector below the last one erases");
	CHECK(program(0x7F7F8u, data, 8) == STUB_ST_OK, "program the last unit below 0x7F800");
	CHECK(program(0x7F7F8u + 8u, data, 8) == STUB_ST_REFUSED, "program at 0x7F800 refused");
	memset(flash + 0x7F000u, 0xFF, 0x800);
	CHECK(program(0x7F7F0u, data, 24) == STUB_ST_REFUSED && flash[0x7F7F0] == 0xFF, "a program crossing 0x7F800 refused");
	CHECK(program(0x4004u, data, 8) == STUB_ST_BADARG, "a program off an 8-byte unit");
	CHECK(program(0x4008u, data, 12) == STUB_ST_BADARG, "a program of part of a unit");
	CHECK(program(0x4008u, data, 2040) == STUB_ST_OK, "the commit sector except its first unit");
	CHECK(erase(9) == STUB_ST_OK, "uncommitted: the next sector erases");
	CHECK(program(0x4800u, data, 2048) == STUB_ST_OK, "uncommitted: programs outside the commit sector");
	CHECK(program(0x4000u, data, 8) == STUB_ST_OK, "the commit unit");
	CHECK(erase(9) == STUB_ST_REFUSED, "committed again: the interlock is back");
	CHECK(memcmp(flash + 0x4000u, data, 0x1000) == 0, "both sectors hold what was sent");
	unsigned total = 0;
	for (unsigned i = 0; i < 8; i++) {
		total += (unsigned)erases[i];
	}
	CHECK(total == 0 && erases[255] == 0, "the loader and the last sector never erased");
}

static void test_spi_floor(void)
{
	reset_all();
	configure_test();
	uint8_t data[256];
	memset(data, 0x33, sizeof(data));
	CHECK(xop(STUB_OP_XERASE, 0x0FF000u, NULL, 0) == STUB_ST_REFUSED && spi_erases == 0,
	      "SPI erase in the first MB refused");
	CHECK(xop(STUB_OP_XERASE, 0, NULL, 0) == STUB_ST_REFUSED, "SPI erase at 0 refused");
	memset(spi + 0x2000u, 0xFF, 256);
	CHECK(xop(STUB_OP_XPROGRAM, 0x2000u, data, 256) == STUB_ST_REFUSED && spi[0x2000] == 0xFF,
	      "SPI program in the first MB refused, even on erased bytes");
	CHECK(xop(STUB_OP_XERASE, XFLOOR, NULL, 0) == STUB_ST_OK && spi_erases == 1, "SPI erase at 1 MB");
	CHECK(xop(STUB_OP_XPROGRAM, XFLOOR + 0x100u, data, 256) == STUB_ST_OK && spi[XFLOOR + 0x100u] == 0x33,
	      "SPI program at 1 MB");
	// Reads are not limited
	uint8_t p[7];
	put32(p, 0);
	p[4] = 0;
	p[5] = 1;
	p[6] = STUB_MEM_SPI;
	const uint8_t *body;
	uint32_t blen;
	CHECK(request(STUB_OP_SUMS, p, 7, &body, &blen) == STUB_ST_OK && blen == 5, "SPI SUMS of the first MB");
}

static void test_peek(void)
{
	reset_all();
	uint8_t p[6];
	struct {
		uint32_t addr;
		uint16_t n;
		uint8_t st;
		const char *what;
	} cases[] = {
		{ 0x00000400u, 16, STUB_ST_OK, "the flash configuration field" },
		{ 0x1FFFFFF8u, 16, STUB_ST_OK, "SRAM across 0x20000000" },
		{ 0x40048054u, 16, STUB_ST_OK, "the unique ID" },
		{ 0x40048024u, 4, STUB_ST_OK, "SDID" },
		{ 0x40020000u, 20, STUB_ST_OK, "the FTFA's registers" },
		{ 0x40072080u, 4, STUB_ST_REFUSED, "the USB controller (ISTAT clears on write, STAT pops on read)" },
		{ 0x40048060u, 8, STUB_ST_REFUSED, "past UIDL" },
		{ 0x20010000u, 4, STUB_ST_REFUSED, "past SRAM_U" },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		put32(p, cases[i].addr);
		p[4] = (uint8_t)(cases[i].n >> 8);
		p[5] = (uint8_t)cases[i].n;
		CHECK(simple(STUB_OP_PEEK, p, 6) == cases[i].st, "PEEK %s", cases[i].what);
	}
}

static void test_geometry(void)
{
	stubeng_flash_t f = k22_flash(512);
	CHECK(stubeng_sector_count(&f) == 256 && stubeng_sector_of(&f, 0x7F800u) == 255 && stubeng_sector_of(&f, 0x4000u) == 8,
	      "512 KB: 256 sectors of 2 KB");
	f = k22_flash(256);
	CHECK(stubeng_sector_count(&f) == 128, "256 KB: 128 sectors");
	CHECK(k22_flash_kb(0xB) == 512 && k22_flash_kb(0x9) == 256 && k22_flash_kb(0x7) == 128 && k22_flash_kb(0xF) == 0,
	      "SIM_FCFG1 PFSIZE codes");
}

int main(void)
{
	port = (stubeng_port_t){
		.write = sim_write,
		.read = sim_read,
		.erase = sim_erase,
		.program = sim_program,
		.reset = sim_reset,
		.name = K22STUB_NAME,
		.version = K22STUB_VERSION,
		.magic = K22STUB_MAGIC,
		.flash = k22_flash(512),
		.peek = k22_peek_ranges,
		.npeek = sizeof(k22_peek_ranges) / sizeof(k22_peek_ranges[0]),
		.security = 0xFE,
		.flags = K22_HELLO_USB_ADOPTED,
		.configure = sim_configure,
		.xread = sim_xread,
		.xerase = sim_xerase,
		.xprogram = sim_xprogram,
	};
	test_geometry();
	test_records();
	test_flash_rules();
	test_spi_floor();
	test_peek();
	printf("k22stubTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
