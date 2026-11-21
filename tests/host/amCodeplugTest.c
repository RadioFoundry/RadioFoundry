// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AM kind in the codeplug (core/radio/radio_am_cp.c, docs/spec/airband-and-broadcast.md 3.4) on a simulated NOR
// flash: a channel set to AM written from the radio and read back, its stand-in record for readers that know no tags,
// a rename and a copy, a later firmware's bytes kept, and that no write ever makes it transmit
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_tag.h"
#include "radio/radio_am.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

// A radio that receives AM wherever it receives (radio_am_channel reads only the capability and rx_am)
static bool r_rx_am(uint32_t hz, radio_bw_t bw) { (void)hz; (void)bw; return true; }
static const hal_radio_t radio = { .rx_am = r_rx_am };
static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true },
				{ 108000000, 136000000, false } };
static const band_t am_bands[] = { { 108000000, 480000000, false } };

const board_t board = {
	.name = "test", .caps = CAP_AM_RX, .bands = bands, .band_count = 3, .am_bands = am_bands, .am_band_count = 1,
	.power_count = 3, .flash = &sim_flash, .radio = &radio,
};

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static channel_t make(const char *name, uint32_t hz)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	strcpy(c.name, name);
	c.rx_hz = c.tx_hz = hz;
	c.mode = RADIO_MODE_FM;
	c.bw = RADIO_BW_25K;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.timeslot = 1;
	c.kind = CODEPLUG_KIND_NONE;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

static void test_save_and_read(void)
{
	codeplug_tag_t t;
	channel_t c, r;

	fresh();
	// The channel editor's AM channel at 145 MHz, with the RX only row left off: written receive only all the same
	c = make("AM 2m", 145000000);
	radio_am_set(&c, true);
	CHECK(radio_am_kind(&c) && c.mode == RADIO_MODE_FM && !c.rx_only);
	CHECK(radio_am_save(20, &c));
	CHECK(codeplug_channel(20, &r) && radio_am_kind(&r) && r.rx_only && r.rx_hz == 145000000 &&
	      r.system == CODEPLUG_SYSTEM_NONE && strcmp(r.name, "AM 2m") == 0);
	CHECK(r.mode == RADIO_MODE_DMR);	// the stand-in, as read
	radio_am_take(&r);
	CHECK(r.mode == RADIO_MODE_FM && radio_am_channel(&r) && radio_am_tx_never(&r));
	// The tag: kind 5, no system, receive only, the record not its own, no block
	CHECK(codeplug_tag_read(20, &t) && t.kind == CODEPLUG_KIND_AM && t.system == CODEPLUG_SYSTEM_NONE &&
	      t.flags == 0u && t.len == CODEPLUG_TAG_HDR);
	// The record underneath, for a reader that knows no tags: DMR, receive only, on the channel's frequencies
	CHECK(codeplug_tag_remove(20));
	CHECK(codeplug_channel(20, &r) && r.kind == CODEPLUG_KIND_NONE && r.mode == RADIO_MODE_DMR && r.rx_only &&
	      r.rx_hz == 145000000 && r.tx_hz == 145000000);

	// 70 cm, and a tag that some other tool wrote with transmit allowed: saving from the radio clears it
	c = make("AM 70cm", 433000000);
	radio_am_set(&c, true);
	CHECK(radio_am_save(21, &c));
	codeplug_tag_t tx = { .kind = CODEPLUG_KIND_AM, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(21, "AM 70cm", &tx, NULL, 0));
	CHECK(codeplug_channel(21, &r) && radio_am_kind(&r) && !r.rx_only);
	radio_am_take(&r);
	CHECK(radio_am_tx_never(&r));	// the radio core refuses it whatever the tag says
	CHECK(radio_am_save(21, &r) && codeplug_channel(21, &r) && r.rx_only);
	CHECK(codeplug_tag_read(21, &t) && (t.flags & CODEPLUG_TAG_TX) == 0);
}

// The tag follows a rename from the radio, a copy takes it, a later firmware's bytes after the header stay, a plain
// channel written over it takes it off, and a stale tag of another kind is replaced
static void test_edits(void)
{
	codeplug_tag_t t;
	channel_t c, r;

	fresh();
	c = make("Listen", 146000000);
	radio_am_set(&c, true);
	CHECK(radio_am_save(30, &c));
	CHECK(codeplug_channel(30, &r));
	radio_am_take(&r);
	strcpy(r.name, "Listen 2");
	CHECK(radio_am_save(30, &r) && codeplug_channel(30, &r) && radio_am_kind(&r) && strcmp(r.name, "Listen 2") == 0);
	// A copy at another number
	CHECK(radio_am_save(31, &r) && codeplug_channel(31, &r) && radio_am_kind(&r) && r.rx_only);
	// A block a later firmware stored after the header: kept by a save from this one
	const uint8_t later[3] = { 0xA1, 0xA2, 0xA3 };
	codeplug_tag_t lt = { .kind = CODEPLUG_KIND_AM, .system = CODEPLUG_SYSTEM_NONE, .flags = 0x40u };
	CHECK(codeplug_tag_write(31, r.name, &lt, later, sizeof later));
	radio_am_take(&r);
	r.rx_hz = r.tx_hz = 147000000;
	CHECK(radio_am_save(31, &r));
	uint8_t raw[16 + CODEPLUG_TAG_HEAD_MAX];
	CHECK(codeplug_channel(31, &r) && r.rx_hz == 147000000 && radio_am_kind(&r));
	CHECK(codeplug_tag_read(31, &t) && t.flags == 0x40u && codeplug_tag_params(31, &r, raw, 16) == 3 &&
	      memcmp(raw, later, 3) == 0);
	// Made plain FM on the radio (the editor's Mode back to FM): the tag goes, an FM channel stays
	c = make("Listen 2", 146000000);
	CHECK(codeplug_set_channel(30, &c) && codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NONE &&
	      r.mode == RADIO_MODE_FM && !r.rx_only && !codeplug_tag_read(30, &t));
	// Over a stale tag of another kind at the number: replaced
	codeplug_tag_t other = { .kind = 0x77, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	c = make("Old", 145500000);
	CHECK(codeplug_set_channel(32, &c) && codeplug_tag_write(32, "Old", &other, NULL, 0));
	radio_am_set(&c, true);
	CHECK(radio_am_save(32, &c) && codeplug_channel(32, &r) && radio_am_kind(&r) && r.rx_only);
	CHECK(codeplug_tag_read(32, &t) && t.kind == CODEPLUG_KIND_AM && t.flags == 0u);
}

int main(void)
{
	test_save_and_read();
	test_edits();
	sim_flash_free();
	if (failures) {
		printf("amCodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("amCodeplugTest: all passed\n");
	return 0;
}
