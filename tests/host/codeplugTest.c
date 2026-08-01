// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// core/codeplug against synthetic flash images: a legacy-layout codeplug, an erased flash, no flash at all
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "board.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define IMAGE_SIZE	0x0B0000u
#define EE		0x000000u
#define FL		0x020000u

static uint8_t *image;

static bool fake_init(void) { return true; }
static bool fake_read(uint32_t addr, void *buf, uint32_t len)
{
	if (addr + len > IMAGE_SIZE) {
		return false;
	}
	memcpy(buf, &image[addr], len);
	return true;
}
static bool fake_write(uint32_t addr, const void *buf, uint32_t len) { (void)addr; (void)buf; (void)len; return false; }
static bool fake_erase(uint32_t addr) { (void)addr; return false; }

static hal_flash_t flash = { .init = fake_init, .size = IMAGE_SIZE, .erase_size = 4096,
			     .read = fake_read, .write = fake_write, .erase = fake_erase };

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &flash };

static void put_name(uint8_t *p, const char *s)
{
	memset(p, 0xFF, 16);
	memcpy(p, s, strlen(s));
}

// 10 Hz units as packed BCD in a LE word
static void put_freq(uint8_t *p, uint32_t hz)
{
	uint32_t v = hz / 10, w = 0;
	for (int i = 0; i < 8; i++) {
		w |= (v % 10) << (4 * i);
		v /= 10;
	}
	p[0] = (uint8_t)w;
	p[1] = (uint8_t)(w >> 8);
	p[2] = (uint8_t)(w >> 16);
	p[3] = (uint8_t)(w >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static uint8_t *channel(uint16_t n)
{
	if (n <= 128) {
		return &image[EE + 0x3790 + 56u * (n - 1u)];
	}
	return &image[FL + 0x7B1C0 + 16u * ((n - 129u) / 128u) + 56u * (n - 129u)];
}

static void mark(uint16_t n)
{
	uint32_t bitmap;
	unsigned k;
	if (n <= 128) {
		bitmap = EE + 0x3780;
		k = n - 1u;
	} else {
		bitmap = FL + 0x7B1B0 + 0x1C10u * ((n - 129u) / 128u);
		k = (n - 129u) % 128u;
	}
	image[bitmap + k / 8] |= (uint8_t)(1u << (k % 8));
}

static void erase_bitmaps(void)
{
	memset(&image[EE + 0x3780], 0, 16);
	for (unsigned b = 0; b < 7; b++) {
		memset(&image[FL + 0x7B1B0 + 0x1C10u * b], 0, 16);
	}
}

static void build_legacy(void)
{
	memset(image, 0xFF, IMAGE_SIZE);
	erase_bitmaps();

	uint8_t *c = channel(1);
	memset(c, 0, 56);
	put_name(c, "Calling");
	put_freq(c + 0x10, 146520000);
	put_freq(c + 0x14, 146520000);
	c[0x18] = 0;
	c[0x19] = 0;
	put16(c + 0x20, 0xFFFF);
	put16(c + 0x22, 0x0885);	// 88.5 Hz
	c[0x33] = 0x02;			// 25 kHz
	mark(1);

	c = channel(2);
	memset(c, 0, 56);
	put_name(c, "Repeater DMR 16ch");	// longer than 16: truncated
	put_freq(c + 0x10, 438500000);
	put_freq(c + 0x14, 430900000);
	c[0x18] = 1;
	c[0x19] = 3;			// level 2
	put16(c + 0x20, 0xC023);	// D023I
	put16(c + 0x22, 0x8754);	// D754N
	c[0x2B] = 4;
	c[0x2C] = 7;
	put16(c + 0x2E, 321);
	c[0x31] = 0x40;			// TS2
	c[0x33] = 0x04;			// RX only, 12.5 kHz
	mark(2);

	mark(3);			// marked in use but the record is erased: must be dropped

	c = channel(200);
	memset(c, 0, 56);
	put_name(c, "Bank1");
	put_freq(c + 0x10, 145500000);
	put_freq(c + 0x14, 145500000);
	c[0x19] = 11;			// beyond this board's levels: clamped
	mark(200);

	// Zones: slot 0 unused, slot 2 "Zone A" with 1, 200, an empty slot 5, then the end; slot 5 blank name
	memset(&image[EE + 0x8010], 0, 0x20);
	image[EE + 0x8010] = (1u << 2) | (1u << 5);
	for (unsigned s = 0; s < 68; s++) {
		memset(&image[EE + 0x8030 + 0xB0u * s + 16], 0, 160);
	}
	uint8_t *z = &image[EE + 0x8030 + 0xB0u * 2];
	put_name(z, "Zone A");
	put16(z + 16, 1);
	put16(z + 18, 200);
	put16(z + 20, 5);
	put16(z + 22, 0);
	put16(z + 24, 2);		// after the end marker: ignored
}

static void test_legacy(void)
{
	build_legacy();
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
	CHECK(codeplug_source() == CODEPLUG_LEGACY);
	CHECK(codeplug_channel_count() == 200);

	channel_t ch;
	CHECK(codeplug_channel(1, &ch));
	CHECK(strcmp(ch.name, "Calling") == 0);
	CHECK(ch.rx_hz == 146520000 && ch.tx_hz == 146520000);
	CHECK(ch.mode == RADIO_MODE_FM && ch.bw == RADIO_BW_25K);
	CHECK(ch.power == CODEPLUG_POWER_DEFAULT);
	CHECK(ch.rx_tone.type == TONE_NONE);
	CHECK(ch.tx_tone.type == TONE_CTCSS && ch.tx_tone.value == 885);
	CHECK(!ch.rx_only);

	CHECK(codeplug_channel(2, &ch));
	CHECK(strcmp(ch.name, "Repeater DMR 16c") == 0);
	CHECK(ch.rx_hz == 438500000 && ch.tx_hz == 430900000);
	CHECK(ch.mode == RADIO_MODE_DMR && ch.bw == RADIO_BW_12K5);
	CHECK(ch.power == 2);
	CHECK(ch.rx_tone.type == TONE_DCS_INV && ch.rx_tone.value == 023);
	CHECK(ch.tx_tone.type == TONE_DCS && ch.tx_tone.value == 0754);
	CHECK(ch.listen_list == 4 && ch.color_code == 7 && ch.contact == 321 && ch.timeslot == 2);
	CHECK(ch.rx_only);

	CHECK(!codeplug_channel(3, &ch));
	CHECK(!codeplug_channel(4, &ch));
	CHECK(!codeplug_channel(0, &ch));
	CHECK(!codeplug_channel(1025, &ch));

	CHECK(codeplug_channel(200, &ch));
	CHECK(strcmp(ch.name, "Bank1") == 0 && ch.rx_hz == 145500000);
	CHECK(ch.power == 8);
	CHECK(ch.timeslot == 1);

	CHECK(codeplug_zone_count() == 1);
	zone_t z;
	CHECK(codeplug_zone(0, &z));
	CHECK(strcmp(z.name, "Zone A") == 0);
	CHECK(z.count == 2 && z.channels[0] == 1 && z.channels[1] == 200);
	CHECK(!codeplug_zone(1, &z));

	// No zones stored: one virtual zone with every used channel
	memset(&image[EE + 0x8010], 0, 0x20);
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
	CHECK(codeplug_zone_count() == 1);
	CHECK(codeplug_zone(0, &z));
	CHECK(strcmp(z.name, "All channels") == 0);
	CHECK(z.count == 3 && z.channels[0] == 1 && z.channels[1] == 2 && z.channels[2] == 200);
}

static void test_erased(void)
{
	memset(image, 0xFF, IMAGE_SIZE);	// bitmaps say "all in use", every record fails the BCD check
	CHECK(codeplug_init() == CODEPLUG_BUILTIN);
	CHECK(codeplug_channel_count() == 6);
	channel_t ch;
	CHECK(codeplug_channel(2, &ch));
	CHECK(ch.rx_hz == 146520000 && ch.tx_hz == ch.rx_hz && ch.mode == RADIO_MODE_FM && !ch.rx_only);
	CHECK(ch.power == CODEPLUG_POWER_DEFAULT);
	CHECK(!codeplug_channel(7, &ch));
	zone_t z;
	CHECK(codeplug_zone_count() == 1);
	CHECK(codeplug_zone(0, &z) && z.count == 6 && z.channels[5] == 6);
}

static void test_no_flash(void)
{
	band_t saved[2] = { bands[0], bands[1] };
	flash.size = 0;
	bands[0] = bands[1] = (band_t){ 400000000, 470000000, false };	// UHF receive only
	CHECK(codeplug_init() == CODEPLUG_BUILTIN);
	CHECK(codeplug_channel_count() == 3);
	channel_t ch;
	CHECK(codeplug_channel(1, &ch) && ch.rx_hz == 433500000 && ch.bw == RADIO_BW_12K5 && ch.rx_only);
	bands[0] = saved[0];
	bands[1] = saved[1];
	flash.size = IMAGE_SIZE;
}

int main(void)
{
	image = malloc(IMAGE_SIZE);
	if (image == NULL) {
		return 1;
	}
	test_legacy();
	test_erased();
	test_no_flash();
	free(image);
	printf(failures ? "# codeplugTest FAILED (%d)\n" : "# codeplugTest PASSED\n", failures);
	return failures ? 1 : 0;
}
