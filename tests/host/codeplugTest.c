// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// core/codeplug against synthetic flash images: an open-firmware codeplug, an erased flash, no flash at all, another
// firmware's codeplug in the open-firmware area (foreign: shown as none)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "codeplug/codeplug.h"
#include "codeplug/dmr_iddb.h"
#include "board.h"

static int failures;

void storage_lock(void) {}
void storage_unlock(void) {}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define IMAGE_SIZE	0x100000u	// to the DMR ID database's area 2
#define EE		0x000000u
#define FL		0x020000u

static uint8_t *image;

static unsigned reads;		// flash reads, for the scan's cost
static uint32_t fail_at = ~0u;	// a read covering this address fails

static bool fake_init(void) { return true; }
static bool fake_read(uint32_t addr, void *buf, uint32_t len)
{
	reads++;
	if (addr + len > IMAGE_SIZE || (fail_at >= addr && fail_at < addr + len)) {
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

static void build_openfw(void)
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

static void test_openfw(void)
{
	build_openfw();
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_source() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel_count() == 200);
	// Evidence: channels 1, 2, 200 and zone A good; zone slot 5 (in use, not erased, no name) bad
	uint16_t good, bad;
	codeplug_evidence(&good, &bad);
	CHECK(!codeplug_foreign() && good == 4 && bad == 1);

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
	// A used channel whose record doesn't decode is counted; an unused one isn't
	uint32_t errors = codeplug_channel_errors();
	uint8_t keep[4];
	memcpy(keep, channel(200) + 0x10, sizeof(keep));	// the RX frequency
	memset(channel(200) + 0x10, 0xFF, sizeof(keep));
	CHECK(!codeplug_channel(200, &ch) && codeplug_channel_errors() == errors + 1);
	CHECK(!codeplug_channel(3, &ch) && codeplug_channel_errors() == errors + 1);
	memcpy(channel(200) + 0x10, keep, sizeof(keep));
	CHECK(codeplug_channel(200, &ch) && codeplug_channel_errors() == errors + 1);
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
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_zone_count() == 1);
	CHECK(codeplug_zone(0, &z));
	CHECK(strcmp(z.name, "All channels") == 0);
	CHECK(z.count == 3 && z.channels[0] == 1 && z.channels[1] == 2 && z.channels[2] == 200);
}

static void test_erased(void)
{
	memset(image, 0xFF, IMAGE_SIZE);	// bitmaps say "all in use", every record fails the BCD check
	CHECK(codeplug_init() == CODEPLUG_BUILTIN);
	uint16_t good, bad;
	codeplug_evidence(&good, &bad);
	CHECK(!codeplug_foreign() && good == 0 && bad == 0);	// erased records don't count: blank
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

// 8 BCD digits, bytes in reading order (DMR IDs and contact numbers)
static void put_bcd_be(uint8_t *p, uint32_t v)
{
	for (int i = 3; i >= 0; i--) {
		p[i] = (uint8_t)((v % 10) | ((v / 10 % 10) << 4));
		v /= 100;
	}
}

static void put_contact(uint16_t n, const char *name, uint32_t id, uint8_t type, uint8_t ts_byte)
{
	uint8_t *c = &image[FL + 0x87620 + 24u * (n - 1u)];
	put_name(c, name);
	put_bcd_be(c + 0x10, id);
	c[0x14] = type;
	c[0x15] = 0;
	c[0x16] = 0;
	c[0x17] = ts_byte;
}

// The pseudo-random bytes site/tests/stock.test.mjs classifies, from the same generator in JavaScript's arithmetic (a
// double product, then ToUint32): the web editor counts 0 good and 624 bad open-firmware records in them, the radio
// the same less the general settings and the contacts it doesn't count. The product is rounded on its own (volatile):
// a compiler that fuses the multiply and add (clang on arm64 does) gives other bytes
static void fill_noise(void)
{
	uint32_t x = 12345;
	for (uint32_t a = 0; a < IMAGE_SIZE; a++) {
		volatile double product = (double)x * 1103515245.0;
		x = (uint32_t)fmod(product + 12345.0, 4294967296.0);
		image[a] = (uint8_t)(x >> 24);
	}
}

// A UTF-16LE name, as the stock layout stores them
static void put_utf16(uint8_t *p, const char *s)
{
	memset(p, 0, 16);
	for (unsigned i = 0; s[i] != '\0' && i < 8; i++) {
		p[2 * i] = (uint8_t)s[i];
	}
}

static void check_empty(void)
{
	CHECK(codeplug_source() == CODEPLUG_BUILTIN && codeplug_channel_count() == 6);
	channel_t ch;
	CHECK(codeplug_channel(1, &ch) && strcmp(ch.name, "Simplex 145.500") == 0);
	zone_t z;
	CHECK(codeplug_zone_count() == 1 && !codeplug_zones_stored() && codeplug_zone(0, &z) && z.count == 6);
	char name[CODEPLUG_NAME_LEN + 1];
	codeplug_radio_name(name);
	CHECK(name[0] == '\0');
#if CONFIG_DMR
	contact_t ct;
	listen_list_t ll;
	CHECK(codeplug_dmr_id() == 0 && !codeplug_contact(1, &ct) && !codeplug_listen_list(1, &ll));
	CHECK(!codeplug_contact_find(201, true, &ct));
#endif
}

// Another firmware's codeplug where the open-firmware one goes (codeplug-stock-tyt.md 2): its in-use bits and BCD
// frequencies pass by chance, its names and the rest don't. The radio shows no codeplug and keeps the bytes
static void test_foreign(void)
{
	uint16_t good, bad;

	memset(image, 0xFF, IMAGE_SIZE);
	fill_noise();
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign());
	codeplug_evidence(&good, &bad);
	CHECK(good == 0 && bad == 620);	// the web editor: 624, with the general settings and the contacts
	check_empty();

	// Stock-style: records with UTF-16 names and valid frequencies, and the one seen on a radio (channel 183, a name
	// of random bytes, 0.08020 / 2.20022 MHz)
	memset(image, 0xFF, IMAGE_SIZE);
	erase_bitmaps();
	for (uint16_t n = 1; n <= 16; n++) {
		uint8_t *c = channel(n);
		memset(c, 0, 56);
		put_utf16(c, "Channel");
		put_freq(c + 0x10, 438500000);
		put_freq(c + 0x14, 438500000);
		mark(n);
	}
	uint8_t *c = channel(183);
	static const uint8_t junk[16] = { 'r', 0x85, 0xC2, '@', 0xF9, ' ', 0x85, 0xF7, 0xC0, ' ', '@', '*', 0x85, '1', 0, 1 };
	memcpy(c, junk, 16);
	put_freq(c + 0x10, 80200);
	put_freq(c + 0x14, 2200220);
	mark(183);
	put_utf16(&image[EE + 0xE0], "Q0TEST");		// UTF-16 in the callsign field: the radio doesn't count it
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign());
	codeplug_evidence(&good, &bad);
	CHECK(good == 0 && bad == 17);
	check_empty();

	// Mixed: a RadioFoundry codeplug over stock remnants is ours while at most one record in five is bad
	build_openfw();		// 4 good, 1 bad
	for (uint16_t n = 4; n < 12; n++) {
		c = channel(n);
		memset(c, 0, 56);
		put_name(c, "Simplex");
		put_freq(c + 0x10, 146520000);
		put_freq(c + 0x14, 146520000);
		mark(n);
	}
	for (uint16_t n = 300; n < 302; n++) {
		c = channel(n);
		memset(c, 0, 56);
		put_utf16(c, "Old");
		put_freq(c + 0x10, 146520000);
		put_freq(c + 0x14, 146520000);
		mark(n);
	}
	// The other firmware's contacts and callsign aren't counted: left beside a codeplug edited on the radio, they
	// mustn't hide it
	put_utf16(&image[EE + 0xE0], "Q0TEST");
	for (uint16_t n = 1; n <= 8; n++) {
		put_utf16(&image[FL + 0x87620 + 24u * (n - 1u)], "TG");
	}
	CHECK(codeplug_init() == CODEPLUG_OPENFW && !codeplug_foreign());
	codeplug_evidence(&good, &bad);
	CHECK(good == 12 && bad == 3);
	CHECK(codeplug_channel_count() == 301);	// the remnants have valid frequencies: shown, as the editor shows them
	// One bad record more than the rule allows: foreign
	c = channel(302);
	memset(c, 0, 56);
	put_utf16(c, "Old");
	put_freq(c + 0x10, 146520000);
	put_freq(c + 0x14, 146520000);
	mark(302);
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign());
	codeplug_evidence(&good, &bad);
	CHECK(good == 12 && bad == 4);
	check_empty();
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

static void test_dmr_records(void)
{
	build_openfw();
	put_bcd_be(&image[EE + 0xE8], 9996543);
	put_contact(1, "Main Ops", 201, 0, 0xFF);
	put_contact(2, "Joe", 9991234, 1, 0x02);	// TS2 override
	put_contact(3, "All", 16777215, 2, 0x01);	// all call, no override
	put_contact(5, "Bad BCD", 0, 0, 0xFF);
	image[FL + 0x87620 + 24u * 4u + 0x10] = 0xAB;
	// List 2: TG 123 and contact 3; list 1 unused
	image[FL + 0x8D620 + 0] = 0;
	image[FL + 0x8D620 + 1] = 3;
	uint8_t *l = &image[FL + 0x8D6A0 + 80u];
	put_name(l, "Local TGs");
	memset(l + 16, 0, 64);
	put16(l + 16, 1);
	put16(l + 18, 3);
	// Channel 2 gets a DMR ID override and force DMO
	uint8_t *c = channel(2);
	c[0x26] = 0x84;
	c[0x27] = 0x98;
	c[0x28] = 0x88;
	c[0x29] = 0xFF;
	CHECK(codeplug_init() == CODEPLUG_OPENFW);

	CHECK(codeplug_dmr_id() == 9996543);
	contact_t ct;
	CHECK(codeplug_contact(1, &ct) && ct.id == 201 && ct.type == CONTACT_GROUP && ct.timeslot == 0 &&
		strcmp(ct.name, "Main Ops") == 0);
	CHECK(codeplug_contact(2, &ct) && ct.id == 9991234 && ct.type == CONTACT_PRIVATE && ct.timeslot == 2);
	CHECK(codeplug_contact(3, &ct) && ct.id == 0xFFFFFF && ct.type == CONTACT_ALL_CALL && ct.timeslot == 0);
	CHECK(!codeplug_contact(4, &ct));	// erased
	CHECK(!codeplug_contact(5, &ct));	// malformed number
	CHECK(!codeplug_contact(0, &ct) && !codeplug_contact(1025, &ct));

	listen_list_t ll;
	CHECK(!codeplug_listen_list(1, &ll));
	CHECK(codeplug_listen_list(2, &ll) && ll.count == 2 && ll.contacts[0] == 1 && ll.contacts[1] == 3 &&
		strcmp(ll.name, "Local TGs") == 0);
	CHECK(!codeplug_listen_list(77, &ll));

	channel_t ch;
	CHECK(codeplug_channel(2, &ch) && ch.dmr_id == 0x9888FF && ch.dmo);
	CHECK(codeplug_channel(1, &ch) && ch.dmr_id == 0 && !ch.dmo);

	// Lookup by ID: group and private are separate name spaces
	CHECK(codeplug_contact_find(201, true, &ct) && strcmp(ct.name, "Main Ops") == 0);
	CHECK(!codeplug_contact_find(201, false, &ct));
	CHECK(codeplug_contact_find(9991234, false, &ct) && strcmp(ct.name, "Joe") == 0);
	CHECK(!codeplug_contact_find(9991234, true, &ct));
	CHECK(codeplug_contact_find(0xFFFFFF, true, &ct) && ct.type == CONTACT_ALL_CALL);
	put_contact(1024, "Last", 777, 1, 0xFF);	// the last record
	CHECK(codeplug_contact_find(777, false, &ct) && strcmp(ct.name, "Last") == 0);
	CHECK(!codeplug_contact_find(778, false, &ct) && !codeplug_contact_find(0, true, &ct));

	// Channel TOT and talker alias TX flags
	c = channel(2);
	c[0x1B] = 4;		// 60 s
	c[0x30] = 0x0E;		// TS1 text (2), TS2 both (3)
	CHECK(codeplug_channel(2, &ch) && ch.tot_s == 60 &&
		ch.alias_tx == (CODEPLUG_ALIAS_TS1_TEXT | CODEPLUG_ALIAS_TS2_TEXT));
	c[0x30] = 0x01;		// TS1 APRS only: no text
	CHECK(codeplug_channel(2, &ch) && ch.alias_tx == 0);

	// General settings: radio name (0xFF padded) and hang times
	codeplug_dmr_settings_t g;
	memcpy(&image[EE + 0xE0], "Q0TEST\xFF\xFF", 8);
	image[EE + 0xF8] = 6;
	image[EE + 0xF9] = 0xFF;
	CHECK(codeplug_dmr_settings(&g) && strcmp(g.radio_name, "Q0TEST") == 0 && g.group_hang_ms == 3000 &&
		g.private_hang_ms == 0);

	// No ID stored
	memset(&image[EE + 0xE8], 0xFF, 4);
	CHECK(codeplug_dmr_id() == 0);
}

// DMR ID database records (open-firmware-cps.md 7.5)
static int pack6(char c)
{
	if (c == ' ') return 0;
	if (c >= '0' && c <= '9') return 1 + c - '0';
	if (c >= 'A' && c <= 'Z') return 11 + c - 'A';
	if (c >= 'a' && c <= 'z') return 37 + c - 'a';
	return 63;
}

static void put_iddb_header(char format, uint8_t rec, uint32_t count)
{
	uint8_t *h = &image[FL + 0x30000];
	h[0] = 'I';
	h[1] = 'd';
	h[2] = (uint8_t)format;
	h[3] = (uint8_t)(rec + 0x4A);
	memcpy(h + 4, "001", 4);
	h[8] = (uint8_t)count;
	h[9] = (uint8_t)(count >> 8);
	h[10] = (uint8_t)(count >> 16);
	h[11] = (uint8_t)(count >> 24);
}

static uint8_t *iddb_rec(uint32_t i, uint8_t rec, uint32_t area2)
{
	uint32_t per1 = (0x40000u - 12u) / rec;
	return i < per1 ? &image[FL + 0x30000 + 12 + i * rec] : &image[area2 + (i - per1) * rec];
}

// Format N: 3-byte LE ID, 16 characters packed in 12 bytes
static void put_iddb_n(uint32_t i, uint32_t id, const char *text, uint32_t area2)
{
	uint8_t *r = iddb_rec(i, 15, area2);
	char t[16];
	memset(t, ' ', sizeof(t));
	memcpy(t, text, strlen(text) < 16 ? strlen(text) : 16);
	r[0] = (uint8_t)id;
	r[1] = (uint8_t)(id >> 8);
	r[2] = (uint8_t)(id >> 16);
	for (int k = 0; k < 4; k++) {
		uint32_t w = 0;
		for (int j = 0; j < 4; j++) {
			w = (w << 6) | (uint32_t)pack6(t[4 * k + j]);
		}
		r[3 + 3 * k] = (uint8_t)(w >> 16);
		r[4 + 3 * k] = (uint8_t)(w >> 8);
		r[5 + 3 * k] = (uint8_t)w;
	}
}

static void test_iddb(void)
{
	char text[DMR_IDDB_TEXT_MAX + 1];
	uint32_t area2 = FL + 0xB8000;

	build_openfw();
	CHECK(!dmr_iddb_init() && dmr_iddb_count() == 0 && !dmr_iddb_lookup(9991234, text, sizeof(text)));

	// 17500 records with IDs 1000000 + 7 i: 17475 fit in area 1, the rest are in area 2
	uint32_t n = 17500;
	put_iddb_header('N', 15, n);
	for (uint32_t i = 0; i < n; i++) {
		char t[17];
		snprintf(t, sizeof(t), "K%05u Op%u", (unsigned)i, (unsigned)(i % 10));
		put_iddb_n(i, 1000000u + 7u * i, t, area2);
	}
	CHECK(dmr_iddb_init() && dmr_iddb_count() == n);
	CHECK(dmr_iddb_lookup(1000000, text, sizeof(text)) && strcmp(text, "K00000 Op0") == 0);
	CHECK(dmr_iddb_lookup(1000000 + 7 * 1234, text, sizeof(text)) && strcmp(text, "K01234 Op4") == 0);
	CHECK(dmr_iddb_lookup(1000000 + 7 * 17475, text, sizeof(text)) && strcmp(text, "K17475 Op5") == 0);
	CHECK(dmr_iddb_lookup(1000000 + 7 * 17476, text, sizeof(text)) && strcmp(text, "K17476 Op6") == 0);
	CHECK(dmr_iddb_lookup(1000000 + 7 * 17499, text, sizeof(text)) && strcmp(text, "K17499 Op9") == 0);
	CHECK(!dmr_iddb_lookup(1000001, text, sizeof(text)) && !dmr_iddb_lookup(999999, text, sizeof(text)));
	char small[7];
	CHECK(dmr_iddb_lookup(1000000 + 7 * 42, small, sizeof(small)) && strcmp(small, "K00042") == 0);

	// A header whose records run past the database's room is no database, so a lookup never reads what follows it:
	// the chip's end here (area 2 runs from 0xD8000 to 0x100000: 28397 records of 15 bytes in all)...
	put_iddb_header('N', 15, 28397);
	CHECK(dmr_iddb_init() && dmr_iddb_count() == 28397);
	put_iddb_header('N', 15, 28398);
	CHECK(!dmr_iddb_init() && dmr_iddb_count() == 0 && !dmr_iddb_lookup(1000000, text, sizeof(text)));
	put_iddb_header('N', 15, 0x3FFFFF);
	CHECK(!dmr_iddb_init());
	// ...and the open-firmware area's end on a board with a map, whatever follows it (the native codeplug)
	hal_flash_map_t m = { .openfw_flash = FL, .openfw_end = area2 + 25u * 15u };
	flash.map = &m;
	put_iddb_header('N', 15, n);
	CHECK(dmr_iddb_init() && dmr_iddb_lookup(1000000 + 7 * 17499, text, sizeof(text)) && strcmp(text, "K17499 Op9") == 0);
	m.openfw_end--;
	CHECK(!dmr_iddb_init());
	m.openfw_end = area2;		// no area 2: area 1's records only
	CHECK(!dmr_iddb_init());
	put_iddb_header('N', 15, 17475);
	CHECK(dmr_iddb_init() && dmr_iddb_count() == 17475);
	m.openfw_end = FL + 0x30000 + 12 + 100u * 15u;	// the area ends inside area 1
	put_iddb_header('N', 15, 100);
	CHECK(dmr_iddb_init());
	put_iddb_header('N', 15, 101);
	CHECK(!dmr_iddb_init());
	m.openfw_end = FL + 0x30000 + 11;		// not even the header: it isn't read
	unsigned before = reads;
	CHECK(!dmr_iddb_init() && reads == before);
	flash.map = NULL;

	// Plain format: 4-byte BCD IDs (LE word) and ASCII text
	put_iddb_header('-', 4 + 20, 2);
	uint8_t *r = &image[FL + 0x30000 + 12];
	memset(r, 0, 48);
	r[0] = 0x34; r[1] = 0x12; r[2] = 0x99; r[3] = 0x09;	// 09991234
	memcpy(r + 4, "Q0TEST Charlie", 14);
	r[24] = 0x20; r[25] = 0x70; r[26] = 0x99; r[27] = 0x09;	// 09997020
	memcpy(r + 28, "W1AW", 4);
	CHECK(dmr_iddb_init() && dmr_iddb_count() == 2);
	CHECK(dmr_iddb_lookup(9991234, text, sizeof(text)) && strcmp(text, "Q0TEST Charlie") == 0);
	CHECK(dmr_iddb_lookup(9997020, text, sizeof(text)) && strcmp(text, "W1AW") == 0);

	// A database written by a CPS takes effect at the codeplug reload that ends the session, without a restart
	put_iddb_header('-', 4 + 20, 1);
	CHECK(dmr_iddb_count() == 2);	// no reload yet: the header read before is kept
	codeplug_reload();
	CHECK(dmr_iddb_count() == 1);

	// The old "ID" tag is rejected
	image[FL + 0x30001] = 'D';
	CHECK(!dmr_iddb_init());
}

// Format C (iddb-compact.md): written by hand, so the radio's reading is checked apart from the editor's encoder
static uint8_t *c_at(uint32_t off)
{
	return off < 0x40000u ? &image[FL + 0x30000u + off] : &image[FL + 0xB8000u + off - 0x40000u];
}

static void put_le(uint8_t *p, uint32_t v, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		p[i] = (uint8_t)(v >> (8u * i));
	}
}

// The characters of t packed at 6 bits, most significant first; the bytes written
static unsigned c_pack(uint8_t *p, const char *t)
{
	unsigned n = (unsigned)strlen(t), bits = 0, out = 0;
	uint32_t acc = 0;
	for (unsigned i = 0; i < n; i++) {
		acc = (acc << 6) | (uint32_t)pack6(t[i]);
		bits += 6u;
		while (bits >= 8u) {
			bits -= 8u;
			p[out++] = (uint8_t)(acc >> bits);
		}
	}
	if (bits > 0u) {
		p[out++] = (uint8_t)(acc << (8u - bits));
	}
	return out;
}

static unsigned c_literal(uint8_t *p, const char *t)
{
	p[0] = (uint8_t)(0xC0u + strlen(t));
	return 1u + c_pack(p + 1, t);
}

static unsigned c_leb(uint8_t *p, uint32_t v)
{
	unsigned n = 0;
	do {
		p[n] = (uint8_t)(v & 0x7Fu);
		v >>= 7;
		if (v != 0u) {
			p[n] |= 0x80u;
		}
		n++;
	} while (v != 0u);
	return n;
}

static void test_iddb_compact(void)
{
	char text[DMR_IDDB_TEXT_MAX + 1];
	char word[16];
	const char *long48 = "QH7Y Frederic Pine Grove CA abcdefghijklmnopqrst";

	build_openfw();
	memset(c_at(0), 0xFF, 0x40000);
	memset(c_at(0x40000), 0xFF, 0x2000);

	// Dictionary: 130 entries (page 2 the table, page 3 the strings), so entry 129 takes a 2-byte code
	uint32_t so = 0;
	for (uint32_t k = 0; k < 130u; k++) {
		snprintf(word, sizeof(word), "Name%u", (unsigned)k);
		const char *w = k == 0u ? "Charlie" : k == 129u ? "Pine Grove CA" : word;
		put_le(c_at(0x2000u + 3u * k), so, 3);
		uint8_t *sp = c_at(0x3000u + so);
		sp[0] = (uint8_t)strlen(w);
		so += 1u + c_pack(sp + 1, w);
	}

	// Page 4: three records. Page 64 (the first in area 2): two, the second with a token no format has
	uint8_t *p = c_at(0x4000);
	unsigned o = 2;
	put_le(p, 3, 2);
	o += c_leb(p + o, 9991234);
	o += c_literal(p + o, "Q0TEST");
	p[o++] = 0x00;
	p[o++] = 0x80;
	p[o++] = 0x01;			// 128 + 1
	o += c_leb(p + o, 6);		// 9991240
	o += c_literal(p + o, "W1AW");
	p[o++] = 0xC0;
	p[o++] = 0xC0;
	o += c_leb(p + o, 260);		// 9991500: two LEB128 bytes
	p[o++] = 0xC0;
	p[o++] = 0x00;
	p[o++] = 0xC0;
	p = c_at(0x40000);
	o = 2;
	put_le(p, 2, 2);
	o += c_leb(p + o, 9997020);
	o += c_literal(p + o, long48);
	p[o++] = 0xC0;
	p[o++] = 0x05;
	o += c_leb(p + o, 1);
	p[o++] = 0xF5;
	p[o++] = 0xC0;
	p[o++] = 0xC0;

	uint8_t *ix = c_at(0x1000);
	put_le(ix, 9991234, 3);
	put_le(ix + 3, 4, 2);
	put_le(ix + 5, 9997020, 3);
	put_le(ix + 8, 64, 2);

	uint8_t *h = c_at(0);
	h[0] = 'I';
	h[1] = 'd';
	h[2] = 'C';
	h[3] = 0;
	h[4] = 1;
	h[5] = 48;
	put_le(h + 6, 4, 2);
	put_le(h + 8, 5, 4);
	put_le(h + 12, 0x41000, 4);
	put_le(h + 16, 0x1000, 4);
	put_le(h + 20, 2, 2);
	put_le(h + 22, 130, 2);
	put_le(h + 24, 0x2000, 4);
	put_le(h + 28, 0x3000, 4);

#if CONFIG_IDDB_COMPACT
	CHECK(dmr_iddb_init() && dmr_iddb_count() == 5);
	CHECK(dmr_iddb_lookup(9991234, text, sizeof(text)) && strcmp(text, "Q0TEST Charlie Pine Grove CA") == 0);
	CHECK(dmr_iddb_lookup(9991240, text, sizeof(text)) && strcmp(text, "W1AW") == 0);
	CHECK(dmr_iddb_lookup(9991500, text, sizeof(text)) && strcmp(text, "Charlie") == 0);
	CHECK(dmr_iddb_lookup(9997020, text, sizeof(text)) && strcmp(text, long48) == 0);
	CHECK(!dmr_iddb_lookup(9991235, text, sizeof(text)));		// between two records
	CHECK(!dmr_iddb_lookup(9991233, text, sizeof(text)));		// before the first page
	CHECK(!dmr_iddb_lookup(9991600, text, sizeof(text)));		// past a page's last record
	CHECK(!dmr_iddb_lookup(9997021, text, sizeof(text)));		// an unused token: the page stops there
	char small[10];
	CHECK(dmr_iddb_lookup(9991234, small, sizeof(small)) && strcmp(small, "Q0TEST Ch") == 0);

	// A header that doesn't hold together is no database
	h[4] = 2;
	CHECK(!dmr_iddb_init());
	h[4] = 1;
	put_le(h + 12, 0x41001, 4);		// not whole pages
	CHECK(!dmr_iddb_init());
	put_le(h + 12, 0x2000, 4);		// the index past the end
	CHECK(!dmr_iddb_init());
	put_le(h + 12, 0x41000, 4);
	put_le(h + 22, 16513, 2);		// more dictionary entries than codes
	CHECK(!dmr_iddb_init());
	put_le(h + 22, 130, 2);
	put_le(h + 16, 0xFFFFFFFEu, 4);		// an offset whose sum with its section's length wraps
	CHECK(!dmr_iddb_init());
	put_le(h + 16, 0x1000, 4);
	put_le(h + 24, 0xFFFFF000u, 4);
	CHECK(!dmr_iddb_init());
	put_le(h + 24, 0x2000, 4);
	CHECK(dmr_iddb_init());
	// a size past the open-firmware area's end: the board's map, not the chip, bounds the database
	hal_flash_map_t m = { .openfw_flash = FL, .openfw_end = FL + 0xB8000 + 0x1000 };
	flash.map = &m;
	CHECK(dmr_iddb_init());
	m.openfw_end--;
	CHECK(!dmr_iddb_init());
	m.openfw_end = FL + 0x30000 + 31;	// room for the plain header, not format C's: only the plain one is read
	unsigned before = reads;
	CHECK(!dmr_iddb_init() && reads == before + 1);
	flash.map = NULL;
	CHECK(dmr_iddb_init());
#else
	// A build without the format sees no database, never a wrong one (iddb-compact.md 3.1)
	CHECK(!dmr_iddb_init() && dmr_iddb_count() == 0 && !dmr_iddb_lookup(9991234, text, sizeof(text)));
#endif
	memset(c_at(0), 0xFF, 32);
}

#if CONFIG_APRS
// The APRS configuration records a CPS writes (open-firmware-cps.md 7.3.8, 0x40 bytes each at EEPROM 0x1588):
// the two magic numbers (the older one keeps the ambiguity one bit lower), and whatever is out of range in a
// record from another tool comes back as a safe value; an empty or foreign record is no configuration
static uint8_t *aprs_rec(unsigned n) { return &image[EE + 0x1588u + 0x40u * (n - 1u)]; }

static void put_aprs(unsigned n, uint16_t magic)
{
	uint8_t *r = aprs_rec(n);
	memset(r, 0, 0x40);
	memcpy(r, "APRS1", 5);			// the record's name: only its first byte matters (not 0x00 / 0xFF)
	r[0x08] = 9;				// our SSID
	memcpy(&r[0x0F], "WIDE1 ", 6);
	r[0x15] = 1;
	memcpy(&r[0x16], "WIDE2 ", 6);
	r[0x1C] = 12;				// two digit SSID
	r[0x1D] = '/';
	r[0x1E] = '>';
	memset(&r[0x1F], 0xFF, 23);
	memcpy(&r[0x1F], "Mobile", 6);
	r[0x3D] = (uint8_t)(magic == 0x4153 ? 3u << 5 : 3u << 4);	// ambiguity 3
	put16(&r[0x3E], magic);
}

static void test_aprs_records(void)
{
	codeplug_aprs_t a;

	build_openfw();
	put_aprs(1, 0x4153);
	put_aprs(2, 0x4152);			// the older layout
	put_aprs(3, 0x4153);
	aprs_rec(3)[0x08] = 22;			// SSID out of range
	aprs_rec(3)[0x1E] = 0x07;		// not a printable symbol
	aprs_rec(3)[0x3D] = 7u << 5;		// ambiguity 7
	memcpy(&aprs_rec(3)[0x0F], "      ", 6);	// no first path
	put_aprs(4, 0x1234);			// unknown magic
	put_aprs(5, 0x4153);
	aprs_rec(5)[0] = 0x00;			// unnamed: empty
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_aprs(1, &a) && a.ssid == 9 && strcmp(a.path[0], "WIDE1-1") == 0 &&
	      strcmp(a.path[1], "WIDE2-12") == 0 && a.sym_table == '/' && a.sym_code == '>' &&
	      strcmp(a.comment, "Mobile") == 0 && a.ambiguity == 3);
	CHECK(codeplug_aprs(2, &a) && a.ambiguity == 3);
	CHECK(codeplug_aprs(3, &a) && a.ssid == 0 && a.sym_table == 0 && a.sym_code == 0 && a.ambiguity == 4 &&
	      a.path[0][0] == '\0');
	CHECK(!codeplug_aprs(4, &a) && !codeplug_aprs(5, &a) && !codeplug_aprs(6, &a));	// 6: erased
	CHECK(!codeplug_aprs(0, &a) && !codeplug_aprs(CODEPLUG_APRS_CONFIGS + 1, &a));
	// The boot text (two 16-byte lines at EEPROM 0x7540), padding dropped
	put_name(&image[EE + 0x7540], "Welcome");
	put_name(&image[EE + 0x7550], "Q0TEST");
	char l1[CODEPLUG_NAME_LEN + 1], l2[CODEPLUG_NAME_LEN + 1];
	codeplug_boot_text(l1, l2);
	CHECK(strcmp(l1, "Welcome") == 0 && strcmp(l2, "Q0TEST") == 0);
	// Without an open-firmware codeplug: nothing
	memset(image, 0xFF, IMAGE_SIZE);
	codeplug_init();
	codeplug_boot_text(l1, l2);
	CHECK(l1[0] == 0 && l2[0] == 0 && !codeplug_aprs(1, &a));
}
#endif

// codeplug_contacts_each: every slot once, in order, empty ones as NULL; a few large reads, not one per slot; the
// callback stops it; a scan from inside a callback still sees every slot
typedef struct {
	uint16_t next, seen, stop_at;
	uint32_t ids;
	bool nested_ok;
} each_t;

static bool each_fn(uint16_t index, const contact_t *c, void *ctx)
{
	each_t *e = ctx;
	if (index != e->next) {
		e->next = 0xFFFF;	// out of order
	} else {
		e->next++;
	}
	if (c != NULL) {
		e->seen++;
		e->ids += c->id;
	}
	if (index == 2) {
		contact_t ct;
		e->nested_ok = codeplug_contact_find(777, false, &ct) && strcmp(ct.name, "Last") == 0;
	}
	return index != e->stop_at;
}

static void test_contacts_each(void)
{
	build_openfw();
	put_contact(1, "First", 201, 0, 0xFF);
	put_contact(32, "End of block", 202, 0, 0xFF);
	put_contact(33, "Next block", 203, 1, 0xFF);
	put_contact(40, "Bad BCD", 0, 0, 0xFF);
	put_contact(1024, "Last", 777, 1, 0xFF);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);

	each_t e = { .next = 1 };
	reads = 0;
	CHECK(codeplug_contacts_each(each_fn, &e));
	CHECK(e.next == CODEPLUG_CONTACTS_MAX + 1);
	CHECK(e.seen == 4 && e.ids == 201u + 202u + 203u + 777u);
	CHECK(e.nested_ok);
	// 32 block reads, plus the nested scan's slot by slot reads (it found the last slot)
	CHECK(reads == 32 + CODEPLUG_CONTACTS_MAX);

	e = (each_t){ .next = 1, .stop_at = 33 };
	reads = 0;
	CHECK(codeplug_contacts_each(each_fn, &e));
	CHECK(e.next == 34 && e.seen == 3 && reads == 2 + CODEPLUG_CONTACTS_MAX);

	// The nested scan is done: the next one reads in blocks again
	contact_t ct;
	reads = 0;
	CHECK(codeplug_contact_find(777, false, &ct) && reads == 32);
	reads = 0;
	CHECK(codeplug_contact_find(203, false, &ct) && strcmp(ct.name, "Next block") == 0 && reads == 2);

	// A failed read ends the scan
	fail_at = FL + 0x87620 + 24u * 100u;
	e = (each_t){ .next = 1 };
	CHECK(!codeplug_contacts_each(each_fn, &e));
	CHECK(e.next == 97);	// blocks 1-96 seen, the fourth failed
	CHECK(!codeplug_contact_find(777, false, &ct));
	fail_at = ~0u;
}

// codeplug_channel_names_each: every number once, in order, empty ones as NULL; one read a block of records holding a
// used one, none for a block without; names off reads nothing; a nested scan and a failed read as with contacts
typedef struct {
	uint16_t next, used, stop_at;
	bool names_ok, nested_ok;
} names_t;

static bool nested_fn(uint16_t number, const char *name, void *ctx)
{
	if (number == 200) {
		*(bool *)ctx = name != NULL && strcmp(name, "Bank1") == 0;
	}
	return true;
}

static bool names_fn(uint16_t number, const char *name, void *ctx)
{
	names_t *e = ctx;
	e->next = number == e->next ? (uint16_t)(number + 1u) : 0xFFFF;
	if (name != NULL) {
		e->used++;
	}
	if ((number == 1 && (name == NULL || strcmp(name, "Calling") != 0)) ||
	    (number == 2 && (name == NULL || strcmp(name, "Repeater DMR 16c") != 0)) ||
	    (number == 3 && name != NULL) ||
	    (number == 129 && (name == NULL || strcmp(name, "Bank1 first") != 0))) {
		e->names_ok = false;
	}
	if (number == 2) {
		codeplug_channel_names_each(nested_fn, &e->nested_ok, true);
	}
	return number != e->stop_at;
}

static void test_channel_names(void)
{
	build_openfw();
	static const struct { uint16_t n; const char *name; } more[] = {
		{ 13, "Block end" }, { 14, "Next block" }, { 128, "Bank0 end" }, { 129, "Bank1 first" },
	};
	for (unsigned i = 0; i < sizeof more / sizeof more[0]; i++) {
		uint8_t *c = channel(more[i].n);
		memcpy(c, channel(1), 56);
		put_name(c, more[i].name);
		mark(more[i].n);
	}
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel_count() == 200);

	names_t e = { .next = 1, .names_ok = true };
	reads = 0;
	CHECK(codeplug_channel_names_each(names_fn, &e, true));
	CHECK(e.next == 201 && e.used == 7 && e.names_ok && e.nested_ok);
	// 13 records a read: 1-13, 14-26, 118-128, 129-141, 194-200, the nested scan's by name (7 used)
	CHECK(reads == 5 + 7);

	e = (names_t){ .next = 1, .names_ok = true, .stop_at = 14 };
	CHECK(codeplug_channel_names_each(names_fn, &e, true));
	CHECK(e.next == 15 && e.used == 4);

	// Names off: the in-use bitmap alone
	e = (names_t){ .next = 1, .names_ok = true };
	reads = 0;
	codeplug_channel_names_each(names_fn, &e, false);
	CHECK(e.next == 201 && e.used == 7 && !e.names_ok && reads == 7);	// "" for a name; the nested scan's reads

	fail_at = FL + 0x7B1C0 + 20u;	// in channel 129
	e = (names_t){ .next = 1, .names_ok = true };
	CHECK(!codeplug_channel_names_each(names_fn, &e, true));
	CHECK(e.next == 129);
	fail_at = ~0u;

	char name[CODEPLUG_NAME_LEN + 1];
	CHECK(codeplug_channel_name(2, name) && strcmp(name, "Repeater DMR 16c") == 0);
	CHECK(codeplug_channel_name(200, name) && strcmp(name, "Bank1") == 0);
	CHECK(!codeplug_channel_name(3, name) && !codeplug_channel_name(0, name) && !codeplug_channel_name(1025, name));
	CHECK(codeplug_zone_name(0, name) && strcmp(name, "Zone A") == 0);
	CHECK(!codeplug_zone_name(1, name));
	memset(&image[EE + 0x8010], 0, 0x20);	// no zones stored: the virtual one
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_zone_name(0, name) && strcmp(name, "All channels") == 0 && !codeplug_zone_name(1, name));
}

int main(void)
{
	image = malloc(IMAGE_SIZE);
	if (image == NULL) {
		return 1;
	}
	test_openfw();
#if CONFIG_APRS
	test_aprs_records();
#endif
	test_dmr_records();
	test_contacts_each();
	test_channel_names();
	test_iddb();
	test_iddb_compact();
	test_erased();
	test_foreign();
	test_no_flash();
	free(image);
	printf(failures ? "# codeplugTest FAILED (%d)\n" : "# codeplugTest PASSED\n", failures);
	return failures ? 1 : 0;
}
