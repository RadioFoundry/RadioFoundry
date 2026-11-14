// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x flash stub's portable parts (stub/gd32f30x/gd32stub_board.c, gd32stub_header.h; docs/spec/stub-gd32f30x.md)
// with the shared protocol engine (stub/common/stubeng.c) on a simulated flash: the 4 KB sector map and its 1 MB cut,
// the board record, the image header RadioFoundry checks before the copy, and the engine's rules at a bootloader end that
// is not on an STM32F4 sector (0x08006000).
#include <stdio.h>
#include <string.h>
#include "gd32stub.h"
#include "gd32stub_header.h"
#include "f4stub_header.h"
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

#define FLASH_SIZE	GD32_STUB_FLASH_MAX
#define APP		0x08006000u	// a bootloader of 24 KB: twelve 2 KB pages, six engine sectors

static uint8_t flash[FLASH_SIZE];
static unsigned erased[GD32_STUB_SECTORS];
static uint8_t sent[16 * 1024];
static uint32_t sent_len;
static stubeng_port_t port;
static stubeng_t eng;

static uint8_t *at(uint32_t addr)
{
	return flash + (addr - GD32_FLASH_START);
}

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
	if (addr >= GD32_FLASH_START && addr - GD32_FLASH_START + n <= FLASH_SIZE) {
		memcpy(dst, at(addr), n);
	} else {
		memset(dst, 0, n);
	}
}

// As the target's port: the engine's sector index to its 4 KB at the map's address
static bool sim_erase(void *ctx, unsigned sector)
{
	(void)ctx;
	uint32_t a = stubeng_sector_addr(&port.flash, sector);
	memset(at(a), 0xFF, stubeng_sector_size(&port.flash, sector));
	erased[sector]++;
	return true;
}

static bool sim_program(void *ctx, uint32_t addr, const uint32_t *words, uint32_t n)
{
	(void)ctx;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t w;
		memcpy(&w, at(addr + 4u * i), 4);
		w &= words[i];
		memcpy(at(addr + 4u * i), &w, 4);
	}
	return true;
}

static void sim_reset(void *ctx)
{
	(void)ctx;
}

static bool sim_configure(void *ctx, const uint8_t *rec, uint32_t n, stubeng_config_t *out)
{
	(void)ctx;
	gd32_board_t b;
	if (!gd32_board_parse(rec, n, &b)) {
		return false;
	}
	out->app_base = b.app_base;
	return true;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint8_t request(uint8_t op, const uint8_t *payload, uint16_t n)
{
	static uint8_t buf[STUBENG_FRAME_HDR + STUBENG_MAX_PAYLOAD];
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
	if (n != 0) {
		memcpy(buf + 6, payload, n);
	}
	sent_len = 0;
	stubeng_feed(&eng, buf, 6u + n);
	return sent_len >= 6 ? sent[1] : 0xFF;
}

static uint8_t config(uint8_t board, uint32_t app_base)
{
	uint8_t rec[GD32_REC_LEN] = { board, GD32_REC_LAYOUT, 0, 0 };
	put32(rec + GD32_REC_APP_BASE, app_base);
	return request(STUB_OP_CONFIG, rec, sizeof(rec));
}

static uint8_t erase(unsigned sector)
{
	uint8_t p[5] = { (uint8_t)sector };
	put32(p + 1, GD32STUB_MAGIC);
	return request(STUB_OP_ERASE, p, 5);
}

// The two-byte form, for a sector past 255
static uint8_t erase16(unsigned sector)
{
	uint8_t p[6] = { (uint8_t)(sector >> 8), (uint8_t)sector };
	put32(p + 2, GD32STUB_MAGIC);
	return request(STUB_OP_ERASE, p, 6);
}

static uint8_t program_word(uint32_t addr, uint32_t w)
{
	uint8_t p[8];
	put32(p, addr);
	memcpy(p + 4, &w, 4);
	return request(STUB_OP_PROGRAM, p, 8);
}

static void test_geometry(void)
{
	stubeng_flash_t big = gd32_flash(3072), half = gd32_flash(512), small = gd32_flash(256);
	CHECK(big.size == 3072u * 1024u && stubeng_sector_count(&big) == 768, "a 3 MB part: 768 sectors");
	CHECK(stubeng_sector_count(&half) == 128 && stubeng_sector_count(&small) == 64, "512 KB and 256 KB parts");
	CHECK(stubeng_sector_of(&big, 0x080FF000u) == 255, "the last 4 KB of the first 1 MB is sector 255 (%d)",
	      stubeng_sector_of(&big, 0x080FF000u));
	CHECK(stubeng_sector_of(&big, APP) == 6 && stubeng_sector_addr(&big, 6) == APP, "0x08006000 starts sector 6");
	CHECK(stubeng_sector_of(&big, 0x08100000u) == 256 && stubeng_sector_of(&big, 0x082FF000u) == 767,
	      "past 1 MB: sectors 256 to 767");
	CHECK(stubeng_sector_of(&big, 0x08300000u) == -1, "nothing past 3 MB");
	CHECK(stubeng_sector_addr(&big, 128) == GD32_BANK0_END, "sector 128 starts bank 1");
}

static void test_record(void)
{
	uint8_t rec[GD32_REC_LEN] = { 7, GD32_REC_LAYOUT, 0, 0, 0x08, 0x00, 0x60, 0x00 };
	gd32_board_t b;
	CHECK(gd32_board_parse(rec, sizeof(rec), &b) && b.board == 7 && b.app_base == APP, "a record decodes");
	CHECK(!gd32_board_parse(rec, sizeof(rec) - 1u, &b), "a short record is refused");
	rec[0] = 0;
	CHECK(!gd32_board_parse(rec, sizeof(rec), &b), "board id 0 is refused");
	rec[0] = 7;
	rec[1] = 2;
	CHECK(!gd32_board_parse(rec, sizeof(rec), &b), "another layout is refused");
	rec[1] = GD32_REC_LAYOUT;
	rec[3] = 1;
	CHECK(!gd32_board_parse(rec, sizeof(rec), &b), "the reserved bytes must be 0");

	// Layout 2: an SPI NAND on PB8 (CS), PB15 (CLK), PB14 (IO0), PA15 (IO1), dual, 256 MB
	uint8_t n2[GD32_REC_LEN_NAND] = { 7, GD32_REC_LAYOUT_NAND, 0, 0, 0x08, 0x00, 0x60, 0x00,
					  0x18, 0x1F, 0x1E, 0x0F, GD32_NAND_DUAL, 0, 0x01, 0x00 };
	CHECK(gd32_board_parse(n2, sizeof(n2), &b) && b.nand && b.app_base == APP && b.nand_pin[0] == 0x18 &&
	      b.nand_pin[3] == 0x0F && b.nand_flags == GD32_NAND_DUAL && b.nand_mb == 256, "a NAND record decodes");
	uint8_t r1[GD32_REC_LEN] = { 7, GD32_REC_LAYOUT, 0, 0, 0x08, 0x00, 0x60, 0x00 };
	CHECK(gd32_board_parse(r1, sizeof(r1), &b) && !b.nand, "layout 1 has no NAND");
	CHECK(!gd32_board_parse(n2, GD32_REC_LEN, &b), "layout 2 cut to 8 bytes is refused");
	n2[1] = GD32_REC_LAYOUT;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "16 bytes of layout 1 are refused");
	n2[1] = GD32_REC_LAYOUT_NAND;
	n2[9] = 0x18;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "two roles on one pin are refused");
	n2[9] = 0x5F;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "a port past E is refused");
	n2[9] = 0x1F;
	n2[12] = 0x02;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "an unknown flag is refused");
	n2[12] = GD32_NAND_DUAL;
	n2[14] = 0;
	n2[15] = 0;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "a NAND of 0 MB is refused");
	n2[14] = 0x08;
	n2[15] = 0x01;
	CHECK(!gd32_board_parse(n2, sizeof(n2), &b), "one over 2048 MB is refused");
}

static void make_image(uint8_t *img, uint32_t len, uint32_t magic, uint32_t version, uint32_t flags)
{
	memset(img, 0, len);
	gd32stub_header_t h = { magic, GD32STUB_HDR_LAYOUT, GD32STUB_HDR_SIZE, version, flags };
	memcpy(img + GD32STUB_HDR_OFFSET, &h, sizeof(h));	// little endian on the host, as on the target
}

static void test_header(void)
{
	static uint8_t img[GD32STUB_IMAGE_MAX + 4u];
	make_image(img, 7000, GD32STUB_HDR_MAGIC, GD32STUB_VERSION, GD32STUB_FLAG_FEEDS_FWDGT);
	CHECK(gd32stub_image_ok(img, 7000, true) && gd32stub_image_ok(img, 7000, false), "a stub that feeds runs");
	CHECK(!gd32stub_image_ok(img, GD32STUB_IMAGE_MAX + 4u, false), "an image over 8 KB is refused");
	CHECK(!gd32stub_image_ok(img, GD32STUB_HDR_OFFSET + 8u, false), "a cut header is refused");
	make_image(img, 7000, GD32STUB_HDR_MAGIC, GD32STUB_VERSION, 0);
	CHECK(!gd32stub_image_ok(img, 7000, true) && gd32stub_image_ok(img, 7000, false),
	      "without the feed flag only while the watchdog is off");
	make_image(img, 7000, F4STUB_HDR_MAGIC, 5, F4STUB_FLAG_FEEDS_IWDG);
	CHECK(!gd32stub_image_ok(img, 7000, false), "the STM32F4 stub's image is refused, watchdog or not");
	memset(img, 0, 7000);
	CHECK(!gd32stub_image_ok(img, 7000, false), "an image with no header is refused");
}

static void reset_flash(void)
{
	memset(flash, 0xFF, sizeof(flash));
	for (uint32_t i = 0; i < APP - GD32_FLASH_START; i++) {
		flash[i] = (uint8_t)(i * 7u + 1u);	// "the bootloader"
	}
	for (uint32_t i = APP - GD32_FLASH_START; i < 0x40000u; i++) {
		flash[i] = (uint8_t)(i ^ (i >> 8));	// an installed application
	}
	memset(erased, 0, sizeof(erased));
}

static void test_engine(void)
{
	port = (stubeng_port_t){
		.name = GD32STUB_NAME,
		.version = GD32STUB_VERSION,
		.magic = GD32STUB_MAGIC,
		.flash = gd32_flash(3072),
		.peek = gd32_peek_ranges,
		.npeek = 3,
		.write = sim_write,
		.read = sim_read,
		.erase = sim_erase,
		.program = sim_program,
		.reset = sim_reset,
		.configure = sim_configure,
	};
	stubeng_init(&eng, &port);
	reset_flash();

	CHECK(request(STUB_OP_HELLO, NULL, 0) == STUB_ST_OK && memcmp(sent + 6, "GDSTUB", 6) == 0, "HELLO names the target");
	CHECK(((sent[6 + 9] << 8) | sent[6 + 10]) == 3072, "HELLO: all 3072 KB of a 3 MB part");
	CHECK(erase(6) == STUB_ST_NOCONFIG, "no write before CONFIG");
	CHECK(config(1, GD32_FLASH_START) == STUB_ST_BADARG, "sector 0 as the application base is refused");
	CHECK(config(1, APP + 0x800u) == STUB_ST_BADARG, "a base off a 4 KB sector is refused");
	CHECK(config(1, APP) == STUB_ST_OK, "the 24 KB bootloader's end is accepted");

	CHECK(erase(5) == STUB_ST_REFUSED && erased[5] == 0, "the bootloader's last sector is never erased");
	CHECK(program_word(APP - 4u, 0) == STUB_ST_REFUSED, "nor programmed");
	CHECK(erase(7) == STUB_ST_REFUSED && erased[7] == 0, "the interlock: sector 7 waits for the commit sector");
	CHECK(erase(6) == STUB_ST_OK && erased[6] == 1, "the commit sector first");
	CHECK(erase(7) == STUB_ST_OK && erase(255) == STUB_ST_OK, "then any application sector, up to 0x080FF000");
	CHECK(erase(0) == STUB_ST_REFUSED && erased[0] == 0, "sector 0 (the vector table) is never erased");
	CHECK(erase16(600) == STUB_ST_OK && erased[600] == 1, "a sector past 255 in the two-byte form (0x08258000)");
	CHECK(erase16(7) == STUB_ST_OK && erased[7] == 2, "the two-byte form below 256 too");
	CHECK(erase16(768) == STUB_ST_BADARG && erase16(0xFFFF) == STUB_ST_BADARG, "nothing past the map");
	CHECK(erase16(5) == STUB_ST_REFUSED && erased[5] == 0, "nor the bootloader in the two-byte form");
	uint8_t bad[7] = { 0 };
	CHECK(request(STUB_OP_ERASE, bad, 7) == STUB_ST_BADARG && request(STUB_OP_ERASE, bad, 4) == STUB_ST_BADARG,
	      "other lengths are refused");
	CHECK(program_word(0x080FF000u, 0x12345678u) == STUB_ST_OK, "a program in the last sector");
	CHECK(program_word(APP, 0x20018000u) == STUB_ST_OK, "the commit word last");
	CHECK(program_word(0x080FF004u, 0) == STUB_ST_REFUSED, "and no program outside the commit sector after it");
	for (uint32_t i = 0; i < APP - GD32_FLASH_START; i++) {
		if (flash[i] != (uint8_t)(i * 7u + 1u)) {
			CHECK(false, "the bootloader changed at 0x%08X", (unsigned)(GD32_FLASH_START + i));
			break;
		}
	}
}

int main(void)
{
	test_geometry();
	test_record();
	test_header();
	test_engine();
	printf("gd32stubTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
