// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR in the codeplug (core/dstar/dstar_codeplug.c, docs/spec/dstar-codeplug.md) on a simulated NOR flash: the
// shared vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), zeroed, short and erased
// blocks, the field filter, a channel's fields from its tag (live, then stale after a rename), the radio-wide record,
// the radio editor's writer keeping what it doesn't know. Built with CONFIG_DSTAR off as well: the registry then says
// the kind isn't built, and a D-STAR channel is still read as one (parked)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "dstar/dstar_codeplug.h"
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
	c.mode = RADIO_MODE_DMR;	// the safe stand-in: DMR, receive only
	c.rx_only = true;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	c.system = CODEPLUG_SYSTEM_NONE;
	return c;
}

// The registry, whichever way the switch is: the row is there, built follows CONFIG_DSTAR
static void test_registry(void)
{
	CHECK(CODEPLUG_KIND_DSTAR == 5);
	CHECK(strcmp(codeplug_kind_name(CODEPLUG_KIND_DSTAR), "DV") == 0);
	CHECK(codeplug_kind_built(CODEPLUG_KIND_DSTAR) == (CONFIG_DSTAR != 0));
	CHECK(!codeplug_systype_built(CODEPLUG_KIND_DSTAR) && strcmp(codeplug_systype_name(CODEPLUG_KIND_DSTAR), "?") == 0);

	// A D-STAR channel is one whatever the build: a build without the switch parks it, never takes it as DMR
	fresh();
	channel_t c = make("DV Park", 438625000, 438625000);
	CHECK(codeplug_set_channel(3, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_DSTAR, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(3, "DV Park", &t, NULL, 0));
	CHECK(codeplug_channel(3, &c) && c.kind == CODEPLUG_KIND_DSTAR && !c.rx_only);
}

#if CONFIG_DSTAR

static int unhex(const char *s, uint8_t *out, int max)
{
	if (strcmp(s, "-") == 0) {
		return 0;
	}
	int n = 0;
	while (s[0] != '\0' && s[1] != '\0' && n < max) {
		char b[3] = { s[0], s[1], 0 };
		out[n++] = (uint8_t)strtoul(b, NULL, 16);
		s += 2;
	}
	return n;
}

// A vector's text field: "_" a space, "-" empty
static void text(char *out, size_t cap, const char *v)
{
	size_t i = 0;
	if (strcmp(v, "-") != 0) {
		for (; v[i] != '\0' && i + 1 < cap; i++) {
			out[i] = v[i] == '_' ? ' ' : v[i];
		}
	}
	out[i] = '\0';
}

// A decoded field against a vector's full-length field
static bool field_is(const char *f, size_t len, const char *v)
{
	char want[32];
	text(want, sizeof want, v);
	return strlen(want) == len && memcmp(f, want, len) == 0;
}

static void test_vectors(void)
{
	FILE *f = fopen("../vectors/channel-modes.txt", "r");
	CHECK(f != NULL);
	if (f == NULL) {
		return;
	}
	char line[256];
	int counted = 0;
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0') {
			continue;
		}
		char *w[8] = { 0 };
		int n = 0;
		char *rest = NULL;
		for (char *p = strtok_r(line, " ", &rest); p != NULL && n < 8; p = strtok_r(NULL, " ", &rest)) {
			w[n++] = p;
		}
		uint8_t b[64];
		if (strcmp(w[0], "dstarchan") == 0 && n == 6) {
			dstar_chan_t c;
			dstar_chan_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(field_is(c.ur, DSTAR_CALL_LEN, w[2]) && field_is(c.rpt1, DSTAR_CALL_LEN, w[3]) &&
			      field_is(c.rpt2, DSTAR_CALL_LEN, w[4]) && c.flags == strtoul(w[5], NULL, 16));
			// what decodes, encodes to the same fields
			uint8_t e[DSTAR_CP_CHAN_LEN];
			dstar_chan_t back;
			dstar_chan_encode(&c, e);
			dstar_chan_decode(e, sizeof e, &back);
			CHECK(memcmp(&back, &c, sizeof c) == 0);
		} else if (strcmp(w[0], "dstarenc") == 0 && n == 6) {
			char s[32];
			dstar_chan_t c;
			text(s, sizeof s, w[1]);
			dstar_field_set(c.ur, DSTAR_CALL_LEN, s);
			text(s, sizeof s, w[2]);
			dstar_field_set(c.rpt1, DSTAR_CALL_LEN, s);
			text(s, sizeof s, w[3]);
			dstar_field_set(c.rpt2, DSTAR_CALL_LEN, s);
			c.flags = (uint8_t)strtoul(w[4], NULL, 16);
			uint8_t e[DSTAR_CP_CHAN_LEN];
			dstar_chan_encode(&c, e);
			CHECK(unhex(w[5], b, sizeof b) == (int)DSTAR_CP_CHAN_LEN && memcmp(e, b, DSTAR_CP_CHAN_LEN) == 0);
		} else if (strcmp(w[0], "dstarstation") == 0 && n == 5) {
			dstar_station_t s;
			int len = unhex(w[1], b, sizeof b);
			dstar_station_decode(b, len == 0 ? -1 : len, &s);
			CHECK(field_is(s.my, DSTAR_CALL_LEN, w[2]) && field_is(s.suffix, DSTAR_SUFFIX_LEN, w[3]) &&
			      field_is(s.msg, DSTAR_MSG_LEN, w[4]));
			uint8_t e[DSTAR_CP_STATION_LEN];
			dstar_station_t back;
			dstar_station_encode(&s, e);
			dstar_station_decode(e, sizeof e, &back);
			CHECK(memcmp(&back, &s, sizeof s) == 0);
		} else {
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(counted >= 20);
}

// Filtering beyond the vectors: every byte value reads as a character the field allows
static void test_filter(void)
{
	uint8_t b[DSTAR_CP_CHAN_LEN];
	for (int v = 0; v < 256; v++) {
		memset(b, v, sizeof b);
		dstar_chan_t c;
		dstar_chan_decode(b, sizeof b, &c);
		for (unsigned i = 0; i < DSTAR_CALL_LEN; i++) {
			char x = c.rpt1[i];
			CHECK((x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == ' ' || x == '/');
		}
		CHECK(c.flags == (v == 0xFF ? 0u : (uint8_t)(v & 1)));
		uint8_t r[DSTAR_CP_STATION_LEN];
		memset(r, v, sizeof r);
		dstar_station_t s;
		dstar_station_decode(r, sizeof r, &s);
		CHECK(s.msg[0] == (v >= 0x20 && v <= 0x7E ? (char)v : ' '));
	}
	// A C string: cut, padded, upper-cased; NULL is blank
	char f[DSTAR_CALL_LEN];
	dstar_field_set(f, sizeof f, "n0call/p extra");
	CHECK(memcmp(f, "N0CALL/P", 8) == 0);
	dstar_field_set(f, sizeof f, "ab");
	CHECK(memcmp(f, "AB      ", 8) == 0);
	dstar_field_set(f, sizeof f, NULL);
	CHECK(memcmp(f, "        ", 8) == 0);
	char m[DSTAR_MSG_LEN];
	dstar_msg_set(m, "Hi,\tall!");
	CHECK(memcmp(m, "Hi, all!            ", DSTAR_MSG_LEN) == 0);
	// Encoding cleans too: a struct filled carelessly never stores a byte a decode wouldn't give
	dstar_chan_t c;
	memset(&c, 0, sizeof c);
	c.flags = 0xFF;
	uint8_t e[DSTAR_CP_CHAN_LEN];
	dstar_chan_encode(&c, e);
	for (unsigned i = 0; i < 3 * DSTAR_CALL_LEN; i++) {
		CHECK(e[i] == ' ');
	}
	CHECK(e[DSTAR_CP_CHAN_LEN - 1u] == DSTAR_CP_VIA_RPT);
}

static void test_load(void)
{
	fresh();
	channel_t c = make("DV Rpt", 439412500, 431812500);
	CHECK(codeplug_set_channel(10, &c));
	dstar_chan_t dc;
	dstar_field_set(dc.ur, DSTAR_CALL_LEN, "CQCQCQ");
	dstar_field_set(dc.rpt1, DSTAR_CALL_LEN, "N0CALL B");
	dstar_field_set(dc.rpt2, DSTAR_CALL_LEN, "N0CALL G");
	dc.flags = DSTAR_CP_VIA_RPT;
	CHECK(dstar_chan_write(10, "DV Rpt", &dc, true));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_DSTAR && c.system == CODEPLUG_SYSTEM_NONE && !c.rx_only);
	dstar_chan_t got;
	CHECK(dstar_chan_load(10, &c, &got) && memcmp(&got, &dc, sizeof dc) == 0);
	// The stored tag: kind 5, no system, transmit allowed, the 25-byte block as the spec lays it out
	uint8_t v[64];
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 10 - 1), v, sizeof v);
	CHECK(n == (int)(CODEPLUG_TAG_HDR + DSTAR_CP_CHAN_LEN) && v[0] == CODEPLUG_KIND_DSTAR && v[1] == 0xFF && v[4] == CODEPLUG_TAG_TX);
	CHECK(memcmp(&v[CODEPLUG_TAG_HDR], "CQCQCQ  N0CALL BN0CALL G\x01", DSTAR_CP_CHAN_LEN) == 0);

	// Receive only: the tag says so, the fields are the same
	CHECK(dstar_chan_write(10, "DV Rpt", &dc, false));
	CHECK(codeplug_channel(10, &c) && c.rx_only && dstar_chan_load(10, &c, &got));

	// A tag without a block: direct, calling CQ
	c = make("DV Simplex", 438625000, 438625000);
	CHECK(codeplug_set_channel(11, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_DSTAR, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(11, "DV Simplex", &t, NULL, 0));
	CHECK(codeplug_channel(11, &c) && dstar_chan_load(11, &c, &got));
	CHECK(memcmp(got.ur, DSTAR_CP_UR_CQ, 8) == 0 && memcmp(got.rpt1, "        ", 8) == 0 && got.flags == 0);

	// A tag under another name than the record's (a tool that knows no tags renamed it): stale, kept as found, never
	// applied
	channel_t st = make("DV Stale", 438650000, 438650000);
	CHECK(codeplug_set_channel(14, &st));
	CHECK(codeplug_tag_write(14, "Other name", &t, NULL, 0));
	CHECK(codeplug_channel(14, &st) && st.kind == CODEPLUG_KIND_NONE && !dstar_chan_load(14, &st, &got));
	codeplug_tag_t back;
	CHECK(codeplug_tag_read(14, &back) && back.kind == CODEPLUG_KIND_DSTAR);
	st.kind = CODEPLUG_KIND_DSTAR;	// even asked as one: the name check doesn't match
	CHECK(!dstar_chan_load(14, &st, &got));

	// Renamed through the codeplug: the tag goes with the old name, the channel is plain
	channel_t ren = make("Renamed", 438625000, 438625000);
	CHECK(codeplug_set_channel(11, &ren) && codeplug_channel(11, &ren) && ren.kind == CODEPLUG_KIND_NONE);
	CHECK(!dstar_chan_load(11, &ren, &got));
	// A channel read before the rename, asked after it: the tag no longer matches its name
	c.kind = CODEPLUG_KIND_DSTAR;
	strcpy(c.name, "Renamed");
	CHECK(!dstar_chan_load(11, &c, &got));

	// Plain and other-kind channels: no set-up
	channel_t plain = make("Plain", 438500000, 438500000);
	plain.mode = RADIO_MODE_FM;
	plain.rx_only = false;
	CHECK(codeplug_set_channel(12, &plain) && codeplug_channel(12, &plain) && !dstar_chan_load(12, &plain, &got));
	channel_t p25 = make("P25 Ops", 453100000, 458100000);
	CHECK(codeplug_set_channel(13, &p25));
	t.kind = CODEPLUG_KIND_P25;
	CHECK(codeplug_tag_write(13, "P25 Ops", &t, NULL, 0));
	CHECK(codeplug_channel(13, &p25) && p25.kind == CODEPLUG_KIND_P25 && !dstar_chan_load(13, &p25, &got));
}

static void test_write_keeps(void)
{
	fresh();
	channel_t c = make("DV Ref", 439412500, 431812500);
	CHECK(codeplug_set_channel(20, &c));
	// A tag a later firmware wrote: reserved tag bit 3, a kind bit, reserved block bits, two bytes appended
	uint8_t blk[DSTAR_CP_CHAN_LEN + 2];
	memset(blk, ' ', sizeof blk);
	blk[DSTAR_CP_CHAN_LEN - 1u] = 0x80;
	blk[DSTAR_CP_CHAN_LEN] = 0x12;
	blk[DSTAR_CP_CHAN_LEN + 1u] = 0x34;
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_DSTAR, .system = CODEPLUG_SYSTEM_NONE, .flags = 0x08 | 0x20 };
	CHECK(codeplug_tag_write(20, "DV Ref", &t, blk, sizeof blk));
	dstar_chan_t dc;
	dstar_field_set(dc.ur, DSTAR_CALL_LEN, "REF030CL");
	dstar_field_set(dc.rpt1, DSTAR_CALL_LEN, "N0CALL B");
	dstar_field_set(dc.rpt2, DSTAR_CALL_LEN, "N0CALL G");
	dc.flags = DSTAR_CP_VIA_RPT;
	CHECK(dstar_chan_write(20, "DV Ref", &dc, true));
	uint8_t v[64];
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 20 - 1), v, sizeof v);
	CHECK(n == (int)(CODEPLUG_TAG_HDR + sizeof blk) && v[4] == (0x08 | 0x20 | CODEPLUG_TAG_TX));
	CHECK(v[CODEPLUG_TAG_HDR + DSTAR_CP_CHAN_LEN - 1u] == (0x80 | DSTAR_CP_VIA_RPT));
	CHECK(v[n - 2] == 0x12 && v[n - 1] == 0x34);
	dstar_chan_t got;
	CHECK(codeplug_channel(20, &c) && dstar_chan_load(20, &c, &got) && memcmp(got.ur, "REF030CL", 8) == 0);

	// A stale tag under another name: replaced whole, nothing of it kept
	c = make("DV New", 439412500, 431812500);
	CHECK(codeplug_set_channel(21, &c));
	t.flags = 0xF8;
	CHECK(codeplug_tag_write(21, "Old name", &t, blk, sizeof blk));
	CHECK(dstar_chan_write(21, "DV New", &dc, false));
	n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 21 - 1), v, sizeof v);
	CHECK(n == (int)(CODEPLUG_TAG_HDR + DSTAR_CP_CHAN_LEN) && v[4] == 0 && v[CODEPLUG_TAG_HDR + 24] == DSTAR_CP_VIA_RPT);
	CHECK(codeplug_channel(21, &c) && c.kind == CODEPLUG_KIND_DSTAR && c.rx_only);
}

static void test_station(void)
{
	fresh();
	dstar_station_t s;
	memset(&s, 'x', sizeof s);
	CHECK(!dstar_station_load(&s));
	CHECK(memcmp(s.my, "        ", 8) == 0 && memcmp(s.suffix, "    ", 4) == 0 && s.msg[0] == ' ' && s.msg[19] == ' ');
	dstar_field_set(s.my, DSTAR_CALL_LEN, "n0call");
	dstar_field_set(s.suffix, DSTAR_SUFFIX_LEN, "mobl");
	dstar_msg_set(s.msg, "Hello from the bench");
	uint8_t r[DSTAR_CP_STATION_LEN];
	dstar_station_encode(&s, r);
	CHECK(memcmp(r, "N0CALL  MOBLHello from the bench", DSTAR_CP_STATION_LEN) == 0);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_DSTAR), r, sizeof r));
	dstar_station_t got;
	CHECK(dstar_station_load(&got) && memcmp(&got, &s, sizeof s) == 0);
	// A short record: MY only, the rest blank
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_DSTAR), (const uint8_t *)"N0CALL", 6));
	CHECK(dstar_station_load(&got) && memcmp(got.my, "N0CALL  ", 8) == 0 && memcmp(got.suffix, "    ", 4) == 0);
	// A longer one (fields a later firmware appended): read as far as this one knows
	uint8_t longer[DSTAR_CP_STATION_LEN + 4];
	memcpy(longer, r, sizeof r);
	memset(&longer[DSTAR_CP_STATION_LEN], 0xAA, 4);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_DSTAR), longer, sizeof longer));
	CHECK(dstar_station_load(&got) && memcmp(&got, &s, sizeof s) == 0);
}
#endif

int main(void)
{
	test_registry();
#if CONFIG_DSTAR
	test_vectors();
	test_filter();
	test_load();
	test_write_keeps();
	test_station();
#endif
	sim_flash_free();
	if (failures) {
		printf("dstarCodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("dstarCodeplugTest%s: all passed\n", CONFIG_DSTAR ? "" : " (CONFIG_DSTAR=0)");
	return 0;
}
