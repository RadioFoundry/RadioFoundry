// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN in the codeplug (core/nxdn/nxdn_codeplug.c, docs/spec/nxdn-codeplug.md) on a simulated NOR flash: the shared
// vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), defaults, zeroed and short
// blocks, a channel's set-up from its tag, the radio-wide record and its record, and a channel written from the radio
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "nxdn/nxdn_codeplug.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

// The board's air interface (core/radio/radio_nxdn.c in the firmware): whether the radio runs kind 4 here
static bool fits;
bool radio_nxdn_fits(void) { return fits; }

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

// The shared vectors of kind 4 against the codec
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
		if (strcmp(w[0], "nxdnconv") == 0) {
			nxdn_conv_t c;
			nxdn_conv_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(c.rate9600 == (h(w[2]) != 0) && c.rx_ran == h(w[3]) && c.tx_ran == h(w[4]) && c.squelch == h(w[5]));
		} else if (strcmp(w[0], "nxdnenc") == 0) {
			nxdn_conv_t c = { h(w[1]) != 0, (uint8_t)h(w[2]), (uint8_t)h(w[3]), (uint8_t)h(w[4]) };
			uint8_t e[NXDN_CP_CONV_LEN];
			nxdn_conv_encode(&c, e);
			CHECK(unhex(w[5], b, sizeof b) == NXDN_CP_CONV_LEN && memcmp(e, b, NXDN_CP_CONV_LEN) == 0);
		} else if (strcmp(w[0], "nxdnunit") == 0) {
			int len = unhex(w[1], b, sizeof b);
			CHECK(nxdn_unit_id_decode(b, len == 0 ? -1 : len, h(w[2])) == h(w[3]));
		} else {
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(counted >= 30);
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

static void add_contact(uint16_t index, const char *name, uint32_t id, contact_type_t type)
{
	contact_t ct;

	memset(&ct, 0, sizeof ct);
	strncpy(ct.name, name, sizeof ct.name - 1);
	ct.id = id;
	ct.type = type;
	CHECK(codeplug_set_contact(index, &ct));
}

static void test_conv_load(void)
{
	nxdn_conv_t nc = { .rate9600 = false, .rx_ran = 9, .tx_ran = 5, .squelch = NXDN_SQUELCH_SELECTIVE };
	uint8_t blk[NXDN_CP_CONV_LEN];
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_NXDN, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	nxdn_conv_cfg_t cfg;
	channel_t c;

	fresh();
	CHECK(codeplug_set_dmr_id(1000));
	c = make("NXDN Ops", 453100000, 458100000);
	CHECK(codeplug_set_channel(10, &c));
	nxdn_conv_encode(&nc, blk);
	CHECK(codeplug_tag_write(10, "NXDN Ops", &t, blk, sizeof blk));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_NXDN && !c.rx_only);
	// Run where the board's air interface carries NXDN, parked elsewhere, whatever the codec says
	fits = false;
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_NXDN) && strcmp(codeplug_kind_name(c.kind), "NXDN") == 0);
	fits = true;
	CHECK(codeplug_kind_built(CODEPLUG_KIND_NXDN));

	CHECK(nxdn_conv_load(10, &c, &cfg));
	CHECK(!cfg.conv.rate9600 && cfg.conv.rx_ran == 9 && cfg.conv.tx_ran == 5 &&
	      cfg.conv.squelch == NXDN_SQUELCH_SELECTIVE);
	CHECK(cfg.unit_id == 1000 && cfg.target == NXDN_CP_GROUP_DEFAULT && !cfg.target_unit);
	// A DMR ID too long for NXDN: unit 1; the radio-wide unit ID, then the channel's own where it fits
	CHECK(codeplug_set_dmr_id(3101276));
	CHECK(nxdn_conv_load(10, &c, &cfg) && cfg.unit_id == 1);
	uint8_t u[2] = { 0x39, 0x30 };	// 12345
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_NXDN), u, 2));
	CHECK(nxdn_conv_load(10, &c, &cfg) && cfg.unit_id == 12345);
	c.dmr_id = 7654321;		// out of range: not the channel's NXDN ID
	CHECK(nxdn_conv_load(10, &c, &cfg) && cfg.unit_id == 12345);
	c.dmr_id = 4321;
	CHECK(nxdn_conv_load(10, &c, &cfg) && cfg.unit_id == 4321);

	// The talk target from the contact: a group, a unit, all call; a unit ID out of range calls group 1
	add_contact(1, "Group 20", 20, CONTACT_GROUP);
	add_contact(2, "Unit 2001", 2001, CONTACT_PRIVATE);
	add_contact(3, "All", 16777215, CONTACT_ALL_CALL);
	add_contact(4, "Long unit", 3101276, CONTACT_PRIVATE);
	static const struct { uint16_t contact, target; bool unit; } want[] = {
		{ 1, 20, false }, { 2, 2001, true }, { 3, NXDN_CP_ID_ALL, false }, { 4, NXDN_CP_GROUP_DEFAULT, false },
	};
	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
		c.contact = want[i].contact;
		CHECK(nxdn_conv_load(10, &c, &cfg) && cfg.target == want[i].target && cfg.target_unit == want[i].unit);
	}

	// A tag without a block: the defaults, receive only
	c = make("Zeroed", 453200000, 453200000);
	CHECK(codeplug_set_channel(11, &c));
	t.flags = 0;
	CHECK(codeplug_tag_write(11, "Zeroed", &t, NULL, 0));
	CHECK(codeplug_channel(11, &c) && c.kind == CODEPLUG_KIND_NXDN && c.rx_only);
	CHECK(nxdn_conv_load(11, &c, &cfg) && cfg.conv.rate9600 && cfg.conv.rx_ran == 0 && cfg.conv.tx_ran == 1 &&
	      cfg.conv.squelch == NXDN_SQUELCH_RAN);
	// A longer block from a newer writer: the fields known read as usual
	uint8_t longer[12] = { 0x01, 0x07, 0x00, 0xAA, 0xBB, 0xCC, 1, 2, 3, 4, 5, 6 };
	t.flags = CODEPLUG_TAG_TX;
	CHECK(codeplug_tag_write(11, "Zeroed", &t, longer, sizeof longer));
	CHECK(codeplug_channel(11, &c) && nxdn_conv_load(11, &c, &cfg) && cfg.conv.rx_ran == 7 && cfg.conv.tx_ran == 7 &&
	      cfg.conv.squelch == NXDN_SQUELCH_SELECTIVE);
	// Another kind's channel, or a plain one: no set-up
	channel_t plain = make("Plain", 453300000, 453300000);
	CHECK(codeplug_set_channel(12, &plain) && codeplug_channel(12, &plain) && !nxdn_conv_load(12, &plain, &cfg));
	t.kind = CODEPLUG_KIND_P25;
	plain = make("P25", 453400000, 453400000);
	CHECK(codeplug_set_channel(13, &plain) && codeplug_tag_write(13, "P25", &t, NULL, 0));
	CHECK(codeplug_channel(13, &plain) && plain.kind == CODEPLUG_KIND_P25 && !nxdn_conv_load(13, &plain, &cfg));
}

// An NXDN channel written from the radio (2.2): the safe stand-in first, then a new tag; an edit after lets it follow
static void test_conv_save(void)
{
	nxdn_conv_t nc = { .rate9600 = true, .rx_ran = 12, .tx_ran = 13, .squelch = NXDN_SQUELCH_SELECTIVE };
	codeplug_tag_t t = { .kind = 0x77, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	nxdn_conv_cfg_t back;
	codeplug_tag_t rt;
	channel_t c, r, old;

	fresh();
	c = make("Save", 453100000, 458100000);
	c.mode = RADIO_MODE_NXDN;	// as the radio core runs it
	c.rx_only = false;
	// Over a plain channel with a stale tag of another kind at the number: the tag is replaced
	old = make("Old", 453100000, 453100000);
	CHECK(codeplug_set_channel(30, &old));
	CHECK(codeplug_tag_write(30, "Old", &t, NULL, 0));
	CHECK(nxdn_conv_save(30, &c, &nc));
	CHECK(codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NXDN && !r.rx_only && r.mode == RADIO_MODE_DMR);
	CHECK(codeplug_tag_read(30, &rt) && rt.kind == CODEPLUG_KIND_NXDN && rt.system == CODEPLUG_SYSTEM_NONE &&
	      (rt.flags & CODEPLUG_TAG_TX) != 0 && (rt.flags & CODEPLUG_TAG_BASE_OWN) == 0);
	CHECK(nxdn_conv_load(30, &r, &back) && back.conv.rate9600 && back.conv.rx_ran == 12 && back.conv.tx_ran == 13 &&
	      back.conv.squelch == NXDN_SQUELCH_SELECTIVE);
	// The record underneath is the stand-in: a reader that knows no tags sees DMR, receive only
	codeplug_tag_remove(30);
	CHECK(codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NONE && r.mode == RADIO_MODE_DMR && r.rx_only);
	CHECK(nxdn_conv_save(30, &c, &nc) && codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NXDN);
	// Renamed from the radio: the tag follows, its block kept
	strcpy(r.name, "Saved");
	r.rx_only = true;
	CHECK(codeplug_set_channel(30, &r) && codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NXDN && r.rx_only);
	CHECK(nxdn_conv_load(30, &r, &back) && back.conv.rx_ran == 12);
	// A copy at another number takes the tag
	CHECK(codeplug_set_channel(31, &r) && codeplug_tag_copy(30, 31, &r));
	CHECK(codeplug_channel(31, &r) && r.kind == CODEPLUG_KIND_NXDN && nxdn_conv_load(31, &r, &back) &&
	      back.conv.tx_ran == 13);
	// Receive only from the radio: the tag says so
	c.rx_only = true;
	CHECK(nxdn_conv_save(32, &c, &nc) && codeplug_channel(32, &r) && r.kind == CODEPLUG_KIND_NXDN && r.rx_only);
	// The block edited on a channel that is NXDN already (the channel editor's RAN and rate): the tag keeps its
	// system, its fallback, its other flags and what its block holds after this firmware's
	{
		uint8_t longer[NXDN_CP_CONV_LEN + 3];
		uint8_t raw[sizeof longer + CODEPLUG_TAG_HEAD_MAX];
		codeplug_tag_t kt = { .kind = CODEPLUG_KIND_NXDN, .system = 2, .fb_chan = 30, .fb_check = 0x1234,
				      .flags = CODEPLUG_TAG_FALLBACK | 0x40u };
		nxdn_conv_t ed = { .rate9600 = false, .rx_ran = 21, .tx_ran = 22, .squelch = NXDN_SQUELCH_RAN };
		channel_t base = make("Keep", 453100000, 458100000);

		nxdn_conv_encode(&nc, longer);
		memcpy(&longer[NXDN_CP_CONV_LEN], "\xA1\xA2\xA3", 3);
		CHECK(codeplug_set_channel(33, &base) && codeplug_tag_write(33, "Keep", &kt, longer, sizeof longer));
		CHECK(codeplug_channel(33, &r) && r.kind == CODEPLUG_KIND_NXDN && r.system == 2 && r.rx_only);
		strcpy(r.name, "Kept");
		r.rx_only = false;
		r.mode = RADIO_MODE_NXDN;
		CHECK(nxdn_conv_save(33, &r, &ed));
		CHECK(codeplug_channel(33, &r) && r.kind == CODEPLUG_KIND_NXDN && r.system == 2 && !r.rx_only &&
		      r.mode == RADIO_MODE_DMR && strcmp(r.name, "Kept") == 0);
		CHECK(codeplug_tag_read(33, &rt) && rt.flags == (CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_TX | 0x40u) &&
		      rt.fb_chan == 30 && rt.fb_check == 0x1234 && rt.len == CODEPLUG_TAG_HEAD_MAX + sizeof longer);
		CHECK(nxdn_conv_load(33, &r, &back) && !back.conv.rate9600 && back.conv.rx_ran == 21 &&
		      back.conv.tx_ran == 22 && back.conv.squelch == NXDN_SQUELCH_RAN);
		CHECK(codeplug_tag_params(33, &r, raw, sizeof longer) == (int)sizeof longer &&
		      memcmp(&raw[NXDN_CP_CONV_LEN], "\xA1\xA2\xA3", 3) == 0);
	}
	// A mixed channel: its record is an FM channel of its own (wide, transmitting), its tag the NXDN side (receive
	// only). The RAN edited on the radio, which holds the channel as it runs (NXDN, narrow, the tag's receive only):
	// the FM side stays as stored, and so does the tag's flag that says the record is its own
	{
		uint8_t blk[NXDN_CP_CONV_LEN];
		codeplug_tag_t mt = { .kind = CODEPLUG_KIND_NXDN, .flags = CODEPLUG_TAG_BASE_OWN };
		nxdn_conv_t ed = { .rate9600 = false, .rx_ran = 21, .tx_ran = 22, .squelch = NXDN_SQUELCH_RAN };
		channel_t fm = make("Mixed", 453200000, 458200000);

		fm.mode = RADIO_MODE_FM;
		fm.bw = RADIO_BW_25K;
		fm.rx_only = false;
		nxdn_conv_encode(&nc, blk);
		CHECK(codeplug_set_channel(34, &fm) && codeplug_tag_write(34, "Mixed", &mt, blk, sizeof blk));
		CHECK(codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_NXDN && r.rx_only && r.mode == RADIO_MODE_FM &&
		      r.bw == RADIO_BW_25K);
		r.mode = RADIO_MODE_NXDN;
		r.bw = RADIO_BW_12K5;
		CHECK(nxdn_conv_save(34, &r, &ed));
		CHECK(codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_NXDN && r.rx_only && r.mode == RADIO_MODE_FM &&
		      r.bw == RADIO_BW_25K && r.tx_hz == 458200000);
		CHECK(codeplug_tag_read(34, &rt) && rt.flags == CODEPLUG_TAG_BASE_OWN);
		CHECK(nxdn_conv_load(34, &r, &back) && back.conv.rx_ran == 21 && back.conv.tx_ran == 22);
		CHECK(codeplug_tag_remove(34) && codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_NONE && !r.rx_only &&
		      r.mode == RADIO_MODE_FM && r.bw == RADIO_BW_25K);
	}
}

int main(void)
{
	test_vectors();
	test_conv_load();
	test_conv_save();
	sim_flash_free();
	if (failures) {
		printf("nxdnCodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("nxdnCodeplugTest: all passed\n");
	return 0;
}
