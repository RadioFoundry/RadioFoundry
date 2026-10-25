// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 codec slot (platform/hrc7000/codec_slot.h): the code moves to the window's start and the listed words
// are rewritten; a version 1 slot (from before the move) and every malformed slot are refused. Synthetic slots, no codec.
// With a file argument, also checks the slot tools/hrc7000_codec.py writes for its synthetic codec (--test-slot), so
// the writer and the firmware agree on the format.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../platform/hrc7000/codec_slot.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define ROOM		0x2000u
#define CODE_LEN	0x400u
#define TAB_LEN		0x100u
#define STOCK_RUN	0x1CEB0u
#define TAB_RAM		0x180014ACu

static const codec_windows_t WIN = { 0x45000u, 0x45000u + 0x800u, 0x18001400u, 0x18004400u };
static uint8_t slot[ROOM];
static const uint32_t rels[3] = { 0x40, 0x84, 0x3FC };

static void put(uint8_t *p, uint32_t w)
{
	memcpy(p, &w, 4);
}

static uint32_t get(const uint8_t *p)
{
	uint32_t w;
	memcpy(&w, p, 4);
	return w;
}

static void seal(void)
{
	codec_slot_t *s = (codec_slot_t *)slot;
	uint32_t c = codec_slot_crc32(slot + s->tab_off, s->tab_len, codec_slot_crc32(slot + s->code_off, s->code_len, 0));
	s->crc = codec_slot_crc32(slot + s->rel_off, s->rel_count * 4u, c);
}

// A slot like hrc7000_codec.py writes: header, code, tables, the list
static void build(uint32_t code_ram)
{
	memset(slot, 0xFF, sizeof slot);
	codec_slot_t s = { 0 };
	s.magic = CODEC_SLOT_MAGIC;
	s.version = CODEC_SLOT_V2;
	s.code_off = 128;
	s.code_len = CODE_LEN;
	s.code_ram = code_ram;
	s.tab_off = 128 + CODE_LEN;
	s.tab_len = TAB_LEN;
	s.tab_ram = TAB_RAM;
	s.decode = 0;
	s.encode = 0x260;
	s.dec_init = 0xB4;
	s.dec_set = 0x84;
	s.enc_init = 0x314;
	s.enc_set = 0x2E4;
	s.fec_init = 0x1DC;
	s.stock_tag = 2501;
	srand(7);
	for (uint32_t i = 0; i < CODE_LEN + TAB_LEN; i++) {
		slot[128 + i] = (uint8_t)rand();
	}
	put(slot + 128, 0x152514D8u);
	put(slot + 132, 0x74C776CDu);
	for (int i = 0; i < 3; i++) {
		put(slot + 128 + rels[i], code_ram + 0x10u * (uint32_t)i);	// addresses inside the code
	}
	s.rel_off = 128 + CODE_LEN + TAB_LEN;
	s.rel_count = 3;
	for (int i = 0; i < 3; i++) {
		put(slot + s.rel_off + 4u * (uint32_t)i, rels[i]);
	}
	memcpy(slot, &s, sizeof s);
	seal();
}

static codec_slot_t *hdr(void)
{
	return (codec_slot_t *)slot;
}

static int ok(void)
{
	uint32_t run = 0;
	return codec_slot_check(slot, ROOM, &WIN, &run);
}

// A version 1 slot (the code at its stock address, no list): refused, even where that address is inside the window
static void test_v1_refused(void)
{
	build(0x45100u);
	hdr()->rel_off = 0;
	hdr()->rel_count = 0;
	seal();
	CHECK(ok());	// as version 2 with an empty list: accepted
	hdr()->version = 1;
	CHECK(!ok());
}

static void test_v2_moves(void)
{
	uint32_t run = 0;
	build(STOCK_RUN);
	CHECK(codec_slot_check(slot, ROOM, &WIN, &run) && run == WIN.run_lo);
	uint8_t ram[CODE_LEN];
	memcpy(ram, slot + 128, CODE_LEN);
	codec_slot_relocate(slot, ram, run);
	for (uint32_t i = 0; i < CODE_LEN; i += 4) {
		int listed = i == rels[0] || i == rels[1] || i == rels[2];
		CHECK(listed || get(ram + i) == get(slot + 128 + i));
	}
	CHECK(get(ram + 0x40) == WIN.run_lo);
	CHECK(get(ram + 0x84) == WIN.run_lo + 0x10u);
	CHECK(get(ram + 0x3FC) == WIN.run_lo + 0x20u);
	CHECK(get(slot + 128 + 0x40) == STOCK_RUN);	// the slot keeps the stock bytes
}

static void test_refused(void)
{
	build(STOCK_RUN);
	CHECK(ok());
	hdr()->crc ^= 1;
	CHECK(!ok());
	build(STOCK_RUN);
	put(slot + hdr()->rel_off, 0x41);	// unaligned
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	put(slot + hdr()->rel_off, 0x4);	// the signature word
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	put(slot + hdr()->rel_off, CODE_LEN);	// past the code
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	put(slot + hdr()->rel_off, 0x100);	// a word that holds no address in the code: another build's list
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->rel_count = CODEC_SLOT_REL_MAX + 1;
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->rel_off = ROOM - 4;	// list runs out of the slot
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->code_len = 0x900;	// bigger than the window
	hdr()->tab_off = 128 + 0x900;
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->tab_ram = 0x18004400u - 0x80u;	// tables past their window
	seal();
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->encode = 0x261;	// odd entry
	seal();
	CHECK(!ok());
	build(STOCK_RUN + 2);	// a move by other than whole words
	CHECK(!ok());
	build(STOCK_RUN);
	hdr()->version = 3;
	CHECK(!ok());
	build(STOCK_RUN);
	CHECK(!codec_slot_check(slot, 100, &WIN, &(uint32_t){ 0 }));	// room smaller than the header
}

// The slot hrc7000_codec.py writes for its synthetic DB25-D codec; the windows it was written for come with it
static void test_tool_slot(const char *path)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	static uint8_t buf[0x18000];
	uint32_t n = (uint32_t)fread(buf, 1, sizeof buf, f);
	fclose(f);
	CHECK(n > 128 + 16);
	if (n <= 128 + 16) {
		return;
	}
	// the tool appends the board's windows and the expected moved code's CRC after the slot: 5 words
	n -= 20;
	codec_windows_t w = { get(buf + n), get(buf + n + 4), get(buf + n + 8), get(buf + n + 12) };
	uint32_t want = get(buf + n + 16);
	uint32_t run = 0;
	CHECK(codec_slot_check(buf, n, &w, &run) && run == w.run_lo);
	const codec_slot_t *s = (const codec_slot_t *)buf;
	CHECK(s->version == CODEC_SLOT_V2 && s->rel_count > 0);
	static uint8_t ram[0x18000];
	memcpy(ram, buf + s->code_off, s->code_len);
	codec_slot_relocate(buf, ram, run);
	CHECK(codec_slot_crc32(ram, s->code_len, 0) == want);
}

int main(int argc, char **argv)
{
	test_v1_refused();
	test_v2_moves();
	test_refused();
	if (argc > 1) {
		test_tool_slot(argv[1]);
	}
	if (failures) {
		printf("hrc7000CodecSlotTest: %d failures\n", failures);
		return 1;
	}
	printf("hrc7000CodecSlotTest: ok\n");
	return 0;
}
