// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System Fusion in the codeplug (core/ysf/ysf_codeplug.c, docs/spec/ysf-codeplug.md) on a simulated NOR flash: the
// shared vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), a channel's DG-IDs from
// its tag, the radio-wide record (the callsign; never a radio ID), and a channel written from the radio
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "ysf/ysf_codeplug.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, counted;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

// The board's air interface (core/radio/radio_ysf.c in the firmware): whether the radio runs kind 103 here
static bool fits = true;
bool radio_ysf_fits(void) { return fits; }

#define FLASH_SIZE	0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 900000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static int unhex(const char *s, uint8_t *out, int max)
{
	int n = 0;

	if (strcmp(s, "-") == 0)
		return 0;
	while (s[0] != '\0' && s[1] != '\0' && n < max) {
		char b[3] = { s[0], s[1], 0 };
		out[n++] = (uint8_t)strtoul(b, NULL, 16);
		s += 2;
	}
	return n;
}

static uint32_t h(const char *s) { return (uint32_t)strtoul(s, NULL, 16); }

static void test_vectors(void)
{
	FILE *f = fopen("../vectors/channel-modes.txt", "r");
	char line[256];

	CHECK(f != NULL);
	if (f == NULL)
		return;
	while (fgets(line, sizeof line, f) != NULL) {
		char *w[8] = { 0 };
		char *rest = NULL;
		uint8_t b[64];
		int n = 0;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		for (char *p = strtok_r(line, " ", &rest); p != NULL && n < 8; p = strtok_r(NULL, " ", &rest))
			w[n++] = p;
		if (strcmp(w[0], "ysfchan") == 0) {
			ysf_chan_t c;
			ysf_chan_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(c.tx_dgid == h(w[2]) && c.rx_dgid == h(w[3]));
		} else if (strcmp(w[0], "ysfenc") == 0) {
			ysf_chan_t c = { (uint8_t)h(w[1]), (uint8_t)h(w[2]) };
			uint8_t e[YSF_CP_CHAN_LEN];
			ysf_chan_encode(&c, e);
			CHECK(unhex(w[3], b, sizeof b) == YSF_CP_CHAN_LEN && memcmp(e, b, YSF_CP_CHAN_LEN) == 0);
		} else if (strcmp(w[0], "ysfcall") == 0) {
			ysf_radio_t r;
			int len = unhex(w[1], b, sizeof b);
			ysf_radio_decode(b, len == 0 ? -1 : len, &r);
			CHECK(strcmp(r.callsign, strcmp(w[2], "-") == 0 ? "" : w[2]) == 0);
		} else {
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(counted >= 20);
}

static channel_t make(const char *name, uint32_t rx, uint32_t tx)
{
	channel_t c;

	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = tx;
	c.mode = RADIO_MODE_DMR;
	c.rx_only = true;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

static void test_load(void)
{
	ysf_chan_t yc = { .tx_dgid = 12, .rx_dgid = 34 }, back;
	uint8_t blk[YSF_CP_CHAN_LEN];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	channel_t c;

	fresh();
	c = make("Fusion", 438900000, 431300000);
	CHECK(codeplug_set_channel(10, &c));
	ysf_chan_encode(&yc, blk);
	CHECK(codeplug_tag_write(10, "Fusion", &t, blk, sizeof blk));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_YSF && !c.rx_only);
	fits = false;
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_YSF) && strcmp(codeplug_kind_name(c.kind), "YSF") == 0);
	fits = true;
	CHECK(codeplug_kind_built(CODEPLUG_KIND_YSF));
	CHECK(ysf_chan_load(10, &c, &back) && back.tx_dgid == 12 && back.rx_dgid == 34);
	// No block: DG-ID 0 both ways; receive only with the tag's flag clear
	c = make("Zeroed", 438925000, 438925000);
	CHECK(codeplug_set_channel(11, &c));
	t.flags = 0;
	CHECK(codeplug_tag_write(11, "Zeroed", &t, NULL, 0));
	CHECK(codeplug_channel(11, &c) && c.kind == CODEPLUG_KIND_YSF && c.rx_only);
	CHECK(ysf_chan_load(11, &c, &back) && back.tx_dgid == 0 && back.rx_dgid == 0);
	// A plain channel: no block
	channel_t plain = make("Plain", 438950000, 438950000);
	CHECK(codeplug_set_channel(12, &plain) && codeplug_channel(12, &plain) && !ysf_chan_load(12, &plain, &back));
}

static void test_radio_record(void)
{
	ysf_radio_t r;
	uint8_t rec[YSF_CP_CALL_LEN];

	fresh();
	ysf_radio_load(&r);
	CHECK(r.callsign[0] == '\0');		// none: the radio name's
	strcpy(r.callsign, "SQ5FOX");
	ysf_radio_encode(&r, rec);
	CHECK(memcmp(rec, "SQ5FOX\0\0\0\0", YSF_CP_CALL_LEN) == 0);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_YSF), rec, sizeof rec));
	memset(&r, 0, sizeof r);
	ysf_radio_load(&r);
	CHECK(strcmp(r.callsign, "SQ5FOX") == 0);
}

// The callsign from the radio name, when the record has none (ysf.md 5.2)
static void test_callsign_from_name(void)
{
	static const struct { const char *name, *call; } v[] = {
		{ "SQ5FOX", "SQ5FOX" }, { "sq5fox Adrian", "SQ5FOX" }, { "SP5WLF-7", "SP5WLF" }, { "SP5WLF/P", "SP5WLF" },
		{ "K1A", "K1A" }, { "Base", "" }, { "12345", "" }, { "", "" }, { "N0CALL12345", "" }, { "SQ5_FOX", "" },
	};
	char out[YSF_CP_CALL_LEN + 1];

	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
		ysf_callsign_from_name(v[i].name, out);
		CHECK(strcmp(out, v[i].call) == 0);
	}
	ysf_callsign_from_name(NULL, out);
	CHECK(out[0] == '\0');
}

static void test_save(void)
{
	ysf_chan_t yc = { .tx_dgid = 5, .rx_dgid = 0 }, back;
	codeplug_tag_t rt;
	channel_t c, r;

	fresh();
	c = make("Saved", 438900000, 431300000);
	c.mode = RADIO_MODE_YSF;	// as the radio core runs it
	c.rx_only = false;
	CHECK(ysf_chan_save(20, &c, &yc));
	CHECK(codeplug_channel(20, &r) && r.kind == CODEPLUG_KIND_YSF && !r.rx_only && r.mode == RADIO_MODE_DMR);
	CHECK(codeplug_tag_read(20, &rt) && rt.kind == CODEPLUG_KIND_YSF && (rt.flags & CODEPLUG_TAG_TX) != 0);
	CHECK(ysf_chan_load(20, &r, &back) && back.tx_dgid == 5 && back.rx_dgid == 0);
	// The record underneath is the safe stand-in
	codeplug_tag_remove(20);
	CHECK(codeplug_channel(20, &r) && r.kind == CODEPLUG_KIND_NONE && r.mode == RADIO_MODE_DMR && r.rx_only);
	// Saved again over a YSF channel: the block changes, the tag stays
	CHECK(ysf_chan_save(20, &c, &yc) && codeplug_channel(20, &r));
	yc.tx_dgid = yc.rx_dgid = 42;
	CHECK(ysf_chan_save(20, &r, &yc) && codeplug_channel(20, &r) && ysf_chan_load(20, &r, &back) && back.tx_dgid == 42 &&
	      back.rx_dgid == 42);
}

int main(void)
{
	test_vectors();
	test_load();
	test_radio_record();
	test_callsign_from_name();
	test_save();
	printf("# ysfCodeplugTest %s (%d vectors, %d failures)\n", failures ? "FAILED" : "PASSED", counted, failures);
	return failures != 0;
}
