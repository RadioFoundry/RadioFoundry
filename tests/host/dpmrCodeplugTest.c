// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR in the codeplug (core/dpmr/dpmr_codeplug.c, docs/spec/dpmr.md 6) on a simulated NOR flash: the shared vectors
// (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), the channel code a channel sends, a
// channel's block from its tag, the radio-wide record, and a channel written from the radio keeping what it doesn't
// know. Built with CONFIG_DPMR off as well: the registry then says the kind isn't built, and a dPMR channel is still
// read as one (parked), never as the DMR stand-in under it
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "codeplug/codeplug_modes.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#if CONFIG_DPMR
#include "dpmr/dpmr_codeplug.h"
#include "dpmr/dpmr.h"
#endif

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

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 900000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
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

// The id is the registry's, whatever the build; not run until the radio core takes the kind
static void test_registry(void)
{
	codeplug_tag_t t = { .kind = 7, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	channel_t c;

	CHECK(CODEPLUG_KIND_DPMR == 7);
	CHECK(strcmp(codeplug_kind_name(CODEPLUG_KIND_DPMR), "dPMR") == 0);
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_DPMR));
	// A dPMR channel is one whatever the build: a build without it parks it, never takes it as the DMR stand-in
	fresh();
	c = make("PMR Park", 446109375, 446109375);
	CHECK(codeplug_set_channel(3, &c));
	CHECK(codeplug_tag_write(3, "PMR Park", &t, NULL, 0));
	CHECK(codeplug_channel(3, &c) && c.kind == CODEPLUG_KIND_DPMR && !c.rx_only);
}

#if CONFIG_DPMR
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

// The shared vectors of kind 7 against the codec
static void test_vectors(void)
{
	FILE *f = fopen("../vectors/channel-modes.txt", "r");
	char line[256];
	int counted = 0;

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
		if (strcmp(w[0], "dpmrchan") == 0) {
			dpmr_chan_t c;
			dpmr_chan_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(c.called == h(w[2]) && c.cc == h(w[3]) && ((c.flags & DPMR_CP_RX_MINE) != 0) == (h(w[4]) != 0));
		} else if (strcmp(w[0], "dpmrenc") == 0) {
			dpmr_chan_t c = { h(w[1]), (uint8_t)h(w[2]), h(w[3]) != 0 ? DPMR_CP_RX_MINE : 0u };
			uint8_t e[DPMR_CP_CHAN_LEN];
			dpmr_chan_encode(&c, e);
			CHECK(unhex(w[4], b, sizeof b) == DPMR_CP_CHAN_LEN && memcmp(e, b, DPMR_CP_CHAN_LEN) == 0);
		} else if (strcmp(w[0], "dpmrown") == 0) {
			int len = unhex(w[1], b, sizeof b);
			uint32_t want = strcmp(w[2], "-") == 0 ? DPMR_CP_ID_NONE : h(w[2]);
			CHECK(dpmr_own_decode(b, len == 0 ? -1 : len) == want);
		} else {
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(counted >= 19);
}

// The code a channel sends: its own from table 6.1, or the one the table ties to its frequency (A1 off the band)
static void test_cc(void)
{
	dpmr_chan_t c = { DPMR_ID_ALL, DPMR_CP_CC_AUTO, 0 };

	CHECK(dpmr_chan_cc(&c, 446109375u) == dpmr_cc_for_hz(446109375u));
	CHECK(dpmr_cc_index(dpmr_chan_cc(&c, 446109375u)) == 2);	// the second dPMR446 channel: A2
	CHECK(dpmr_chan_cc(&c, 433450000u) == dpmr_cc_group(false, 0));
	c.cc = 16;
	CHECK(dpmr_chan_cc(&c, 446109375u) == dpmr_cc_group(false, 15) && dpmr_cc_index(dpmr_chan_cc(&c, 0)) == 16);
	c.cc = 17;
	CHECK(dpmr_chan_cc(&c, 446109375u) == dpmr_cc_group(true, 0) && dpmr_cc_index(dpmr_chan_cc(&c, 0)) == 17);
	c.cc = 32;
	CHECK(dpmr_chan_cc(&c, 446109375u) == dpmr_cc_group(true, 15) && dpmr_cc_index(dpmr_chan_cc(&c, 0)) == 32);
	c.cc = 33;		// past the table (a block this firmware didn't write): automatic
	CHECK(dpmr_chan_cc(&c, 446109375u) == dpmr_cc_for_hz(446109375u));
}

static void test_load_save(void)
{
	dpmr_chan_t want = { DPMR_ID_COMMON(87), 2, DPMR_CP_RX_MINE }, got;
	codeplug_tag_t rt;
	channel_t c, r;

	fresh();
	c = make("PMR 2", 446109375, 446109375);
	c.kind = CODEPLUG_KIND_DPMR;
	c.mode = RADIO_MODE_FM;		// whatever the caller holds: the record is written as the stand-in
	CHECK(dpmr_chan_save(5, &c, &want));
	CHECK(codeplug_channel(5, &r) && r.kind == CODEPLUG_KIND_DPMR && r.rx_only && r.mode == RADIO_MODE_DMR);
	CHECK(dpmr_chan_load(5, &r, &got) && got.called == want.called && got.cc == 2 && got.flags == DPMR_CP_RX_MINE);
	// The record underneath is the stand-in: a reader that knows no tags sees DMR, receive only
	CHECK(codeplug_tag_read(5, &rt) && (rt.flags & CODEPLUG_TAG_TX) == 0);
	// Made able to transmit: the tag says so, the record stays the stand-in
	c.rx_only = false;
	CHECK(dpmr_chan_save(5, &c, &want));
	CHECK(codeplug_channel(5, &r) && r.kind == CODEPLUG_KIND_DPMR && !r.rx_only);
	CHECK(codeplug_tag_read(5, &rt) && (rt.flags & CODEPLUG_TAG_TX) != 0);
	codeplug_tag_remove(5);
	CHECK(codeplug_channel(5, &r) && r.kind == CODEPLUG_KIND_NONE && r.mode == RADIO_MODE_DMR && r.rx_only);
	// Another kind's channel is not a dPMR channel
	{
		codeplug_tag_t nt = { .kind = CODEPLUG_KIND_NXDN, .system = CODEPLUG_SYSTEM_NONE, .flags = 0 };
		channel_t n = make("NXDN", 453400000, 453400000);
		CHECK(codeplug_set_channel(8, &n) && codeplug_tag_write(8, "NXDN", &nt, NULL, 0));
		CHECK(codeplug_channel(8, &n) && n.kind == CODEPLUG_KIND_NXDN && !dpmr_chan_load(8, &n, &got));
	}

	// A tag a later firmware wrote (a flag of its own, a fallback, a reserved block flag and two bytes after the
	// block) is edited from the radio: everything this firmware doesn't know stays
	{
		uint8_t blk[DPMR_CP_CHAN_LEN + 2] = { 0xFF, 0xFF, 0xFF, 0, 0x40, 0x12, 0x34 };
		uint8_t raw[sizeof blk + CODEPLUG_TAG_HEAD_MAX];
		codeplug_tag_t kt = { .kind = CODEPLUG_KIND_DPMR, .system = CODEPLUG_SYSTEM_NONE, .fb_chan = 5,
				      .fb_check = 0x1234, .flags = CODEPLUG_TAG_FALLBACK | 0x80u };
		channel_t k = make("PMR Keep", 446103125, 446103125);

		CHECK(codeplug_set_channel(9, &k) && codeplug_tag_write(9, "PMR Keep", &kt, blk, sizeof blk));
		CHECK(codeplug_channel(9, &r) && r.kind == CODEPLUG_KIND_DPMR && dpmr_chan_load(9, &r, &got));
		CHECK(got.flags == 0x40u);
		got.called = DPMR_ID_COMMON(87);
		got.flags |= DPMR_CP_RX_MINE;
		r.rx_only = false;
		CHECK(dpmr_chan_save(9, &r, &got));
		CHECK(codeplug_tag_read(9, &rt) && rt.flags == (CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_TX | 0x80u) &&
		      rt.fb_chan == 5 && rt.fb_check == 0x1234);
		CHECK(codeplug_channel(9, &r) && codeplug_tag_params(9, &r, raw, sizeof blk) == (int)sizeof blk);
		CHECK(raw[2] == 0x57 && raw[4] == (0x40u | DPMR_CP_RX_MINE) && raw[5] == 0x12 && raw[6] == 0x34);
	}

	// The radio-wide record
	{
		static const uint8_t own[DPMR_CP_STATION_LEN] = { 0xDE, 0xBC, 0x0A };
		CHECK(dpmr_own_load() == DPMR_CP_ID_NONE);
		CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_DPMR), own, sizeof own));
		CHECK(dpmr_own_load() == 0x0ABCDEu);
	}
}
#endif

int main(void)
{
	test_registry();
#if CONFIG_DPMR
	test_vectors();
	test_cc();
	test_load_save();
#endif
	sim_flash_free();
	printf("%s dpmrCodeplugTest%s: %d failure(s)\n", failures ? "FAIL" : "ok", CONFIG_DPMR ? "" : " (CONFIG_DPMR=0)",
	       failures);
	return failures ? 1 : 0;
}
