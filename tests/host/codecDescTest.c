// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The STM32 codec descriptor (platform/stm32f4/codec_desc.h): a sound one passes, every malformed one is refused, the
// window CRCs are checked against the bytes. Synthetic flash, no codec. With file arguments, also checks the codec
// sectors as tools/codec_extract.py merges them for its synthetic codecs (--test-desc: the DM-1701's sectors 10 and
// 11, then the MD-380's 8-11 with five table windows), so the writer and the firmware agree on the format.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../platform/stm32f4/codec_desc.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define LO		0x080C0000u
#define ROOM		0x40000u	// sectors 10 and 11
#define TAG		0x0206u
#define CODE		0x080E3A74u
#define CODE_LEN	0x1021Cu
#define TABLES		0x080D19F0u
#define TABLES_LEN	0x12082u

static uint8_t flash[ROOM];

static codec_desc_t *desc(void)
{
	return (codec_desc_t *)(flash + (CODEC_DESC_ADDR - LO));
}

static void seal(void)
{
	codec_desc_t *d = desc();
	d->crc = codec_desc_crc32((const uint8_t *)d, offsetof(codec_desc_t, crc), 0);
}

static void build(void)
{
	memset(flash, 0xFF, sizeof flash);
	srand(11);
	for (uint32_t i = 0; i < TABLES_LEN; i++) {
		flash[TABLES - LO + i] = (uint8_t)rand();
	}
	for (uint32_t i = 0; i < CODE_LEN; i++) {
		flash[CODE - LO + i] = (uint8_t)rand();
	}
	codec_desc_t *d = desc();
	memset(d, 0, sizeof *d);
	d->magic = CODEC_DESC_MAGIC;
	d->version = CODEC_DESC_VERSION;
	d->tag = TAG;
	d->code.addr = CODE;
	d->code.len = CODE_LEN;
	d->code.crc = codec_desc_crc32(flash + (CODE - LO), CODE_LEN, 0);
	d->windows = 1;
	d->table[0].addr = TABLES;
	d->table[0].len = TABLES_LEN;
	d->table[0].crc = codec_desc_crc32(flash + (TABLES - LO), TABLES_LEN, 0);
	seal();
}

static int ok(void)
{
	return codec_desc_check(desc(), TAG, LO, CODEC_DESC_ADDR, CODE_LEN);
}

static void test_sound(void)
{
	build();
	CHECK(ok());
	CHECK(codec_desc_check_bytes(desc(), flash, LO));
	flash[CODE - LO + 100] ^= 1;			// a codec byte changed
	CHECK(ok() && !codec_desc_check_bytes(desc(), flash, LO));
	build();
	flash[TABLES - LO + TABLES_LEN - 1] ^= 1;	// a table byte changed
	CHECK(!codec_desc_check_bytes(desc(), flash, LO));
}

static void test_refused(void)
{
	build();
	desc()->crc ^= 1;
	CHECK(!ok());
	build();
	desc()->magic = 0xFFFFFFFFu;			// erased flash
	seal();
	CHECK(!ok());
	build();
	desc()->version = 2;
	seal();
	CHECK(!ok());
	build();
	CHECK(!codec_desc_check(desc(), 0x0203u, LO, CODEC_DESC_ADDR, CODE_LEN));	// another build's codec
	desc()->code.addr = CODE + 2;			// not word aligned
	seal();
	CHECK(!ok());
	build();
	desc()->code.len = CODE_LEN - 4;		// shorter than the entries need
	seal();
	CHECK(!ok());
	build();
	desc()->code.addr = CODEC_DESC_ADDR - 0x1000u;	// runs over the descriptor
	seal();
	CHECK(!ok());
	build();
	desc()->code.addr = 0x080A0000u;		// in the application's flash
	seal();
	CHECK(!ok());
	build();
	desc()->windows = CODEC_DESC_WINDOWS + 1;
	seal();
	CHECK(!ok());
	build();
	desc()->table[0].addr = 0x080BFFF0u;		// starts below the codec sectors
	seal();
	CHECK(!ok());
	build();
	desc()->table[0].len = 0xFFFFFFF0u;		// wraps
	seal();
	CHECK(!ok());
}

// Sectors 10 and 11 as codec_extract.py merges its synthetic DM-1701 codec
static void test_tool(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	size_t n = fread(flash, 1, sizeof flash, f);
	fclose(f);
	CHECK(n == ROOM);
	const codec_desc_t *d = desc();
	CHECK(codec_desc_check(d, TAG, LO, CODEC_DESC_ADDR, CODE_LEN));
	CHECK(codec_desc_check_bytes(d, flash, LO));
	CHECK(d->code.addr == CODE && d->code.len == CODE_LEN && d->windows == 1);
	CHECK(d->table[0].addr == TABLES && d->table[0].len == TABLES_LEN);
}

// Sectors 8-11 as codec_extract.py merges its synthetic MD-380 codec: the code at the start of sector 11, the main
// table window in sector 8 and four small ones (boards/md380/vocoder_md380.c)
static uint8_t md[0x80000];

static void test_tool_md380(const char *path)
{
	static const uint32_t addr[CODEC_DESC_WINDOWS] = { 0x0808FC64u, 0x080D0FC8u, 0x080F9698u, 0x080F9E74u, 0x080FD490u };
	static const uint32_t len[CODEC_DESC_WINDOWS] = { 0xAFF8u, 0x24u, 0x18u, 0x14u, 0x20u };
	const uint32_t lo = 0x08080000u;
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	size_t n = fread(md, 1, sizeof md, f);
	fclose(f);
	CHECK(n == sizeof md);
	const codec_desc_t *d = (const codec_desc_t *)(md + (CODEC_DESC_ADDR - lo));
	CHECK(codec_desc_check(d, 0x2032u, lo, CODEC_DESC_ADDR, 0x10884u));
	CHECK(codec_desc_check_bytes(d, md, lo));
	CHECK(d->code.addr == 0x080E0000u && d->code.len == 0x10884u && d->windows == CODEC_DESC_WINDOWS);
	for (uint32_t i = 0; i < CODEC_DESC_WINDOWS; i++) {
		CHECK(d->table[i].addr == addr[i] && d->table[i].len == len[i]);
	}
}

int main(int argc, char **argv)
{
	test_sound();
	test_refused();
	if (argc > 1) {
		test_tool(argv[1]);
	}
	if (argc > 2) {
		test_tool_md380(argv[2]);
	}
	if (failures) {
		printf("codecDescTest: %d failures\n", failures);
		return 1;
	}
	printf("codecDescTest: ok\n");
	return 0;
}
