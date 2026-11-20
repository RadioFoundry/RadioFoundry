// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion in the codeplug (core/ysf/ysf_codeplug.c, docs/spec/ysf-codeplug.md) on a simulated NOR flash:
// the shared vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), zeroed, short and
// erased blocks, the field filter, a channel's fields from its tag (live, then stale after a rename), the radio-wide
// record, the radio editor's writer keeping what it doesn't know. Built with CONFIG_YSF off as well: the registry then
// says the kind isn't built, and a Fusion channel is still read as one (parked)
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

// The registry, whichever way the switch is: the row is there, built follows CONFIG_YSF
static void test_registry(void)
{
	CHECK(CODEPLUG_KIND_YSF == 6);
	CHECK(strcmp(codeplug_kind_name(CODEPLUG_KIND_YSF), "YSF") == 0);
	CHECK(codeplug_kind_built(CODEPLUG_KIND_YSF) == (CONFIG_YSF != 0));
	CHECK(!codeplug_systype_built(CODEPLUG_KIND_YSF) && strcmp(codeplug_systype_name(CODEPLUG_KIND_YSF), "?") == 0);

	// A Fusion channel is one whatever the build: a build without the switch parks it, never takes it as DMR
	fresh();
	channel_t c = make("C4FM Park", 438625000, 438625000);
	CHECK(codeplug_set_channel(3, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(3, "C4FM Park", &t, NULL, 0));
	CHECK(codeplug_channel(3, &c) && c.kind == CODEPLUG_KIND_YSF && !c.rx_only);
}

#if CONFIG_YSF

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
		if (strcmp(w[0], "ysfchan") == 0 && n == 6) {
			ysf_chan_t c;
			ysf_chan_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(field_is(c.dest, YSF_CALLSIGN_LEN, w[2]) && c.tx_dgid == atoi(w[3]) && c.rx_dgid == atoi(w[4]) &&
			      c.flags == strtoul(w[5], NULL, 16));
			// what decodes, encodes to the same fields
			uint8_t e[YSF_CP_CHAN_LEN];
			ysf_chan_t back;
			ysf_chan_encode(&c, e);
			ysf_chan_decode(e, sizeof e, &back);
			CHECK(memcmp(&back, &c, sizeof c) == 0);
		} else if (strcmp(w[0], "ysfenc") == 0 && n == 6) {
			char s[32];
			ysf_chan_t c;
			text(s, sizeof s, w[1]);
			ysf_field_set(c.dest, YSF_CALLSIGN_LEN, s);
			c.tx_dgid = (uint8_t)atoi(w[2]);
			c.rx_dgid = (uint8_t)atoi(w[3]);
			c.flags = (uint8_t)strtoul(w[4], NULL, 16);
			uint8_t e[YSF_CP_CHAN_LEN];
			ysf_chan_encode(&c, e);
			CHECK(unhex(w[5], b, sizeof b) == (int)YSF_CP_CHAN_LEN && memcmp(e, b, YSF_CP_CHAN_LEN) == 0);
		} else if (strcmp(w[0], "ysfstation") == 0 && n == 4) {
			ysf_station_t s;
			int len = unhex(w[1], b, sizeof b);
			ysf_station_decode(b, len == 0 ? -1 : len, &s);
			CHECK(field_is(s.call, YSF_CALLSIGN_LEN, w[2]) && field_is(s.radio_id, YSF_RADIO_ID_LEN, w[3]));
			uint8_t e[YSF_CP_STATION_LEN];
			ysf_station_t back;
			ysf_station_encode(&s, e);
			ysf_station_decode(e, sizeof e, &back);
			CHECK(memcmp(&back, &s, sizeof s) == 0);
		} else {
			continue;
		}
		counted++;
	}
	fclose(f);
	CHECK(counted >= 20);
}

// Every byte value reads as a character the field allows, and a DG-ID as 0-99
static void test_filter(void)
{
	uint8_t b[YSF_CP_CHAN_LEN];
	for (int v = 0; v < 256; v++) {
		memset(b, v, sizeof b);
		ysf_chan_t c;
		ysf_chan_decode(b, sizeof b, &c);
		for (unsigned i = 0; i < YSF_CALLSIGN_LEN; i++) {
			char x = c.dest[i];
			CHECK((x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == ' ' || x == '/' || x == '-');
		}
		CHECK(c.tx_dgid == (v <= 99 ? v : 0) && c.rx_dgid == c.tx_dgid);
		CHECK(c.flags == (v == 0xFF ? 0u : (uint8_t)(v & YSF_CP_FLAGS)));
	}
	char f[YSF_CALLSIGN_LEN];
	ysf_field_set(f, sizeof f, "g4xyz/p-1 extra");
	CHECK(memcmp(f, "G4XYZ/P-1 ", 10) == 0);
	ysf_field_set(f, sizeof f, NULL);
	CHECK(memcmp(f, "          ", 10) == 0);
	// Encoding cleans too: a struct filled carelessly never stores what a decode wouldn't give
	ysf_chan_t c;
	memset(&c, 0, sizeof c);
	c.tx_dgid = 200;
	c.flags = 0xFF;
	uint8_t e[YSF_CP_CHAN_LEN];
	ysf_chan_encode(&c, e);
	CHECK(e[0] == ' ' && e[9] == ' ' && e[10] == 0 && e[12] == YSF_CP_FLAGS);
}

static void test_load(void)
{
	fresh();
	channel_t c = make("C4FM Rpt", 439412500, 431812500);
	CHECK(codeplug_set_channel(10, &c));
	ysf_chan_t yc;
	ysf_field_set(yc.dest, YSF_CALLSIGN_LEN, "ALL");
	yc.tx_dgid = 10;
	yc.rx_dgid = 0;
	yc.flags = 0;
	CHECK(ysf_chan_write(10, "C4FM Rpt", &yc, true));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_YSF && c.system == CODEPLUG_SYSTEM_NONE && !c.rx_only);
	ysf_chan_t got;
	CHECK(ysf_chan_load(10, &c, &got) && memcmp(&got, &yc, sizeof yc) == 0);
	// The stored tag: kind 6, no system, transmit allowed, the 13-byte block as the spec lays it out
	uint8_t v[64];
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 10 - 1), v, sizeof v);
	CHECK(n == (int)(CODEPLUG_TAG_HDR + YSF_CP_CHAN_LEN) && v[0] == CODEPLUG_KIND_YSF && v[1] == 0xFF && v[4] == CODEPLUG_TAG_TX);
	CHECK(memcmp(&v[CODEPLUG_TAG_HDR], "ALL       \x0a\x00\x00", YSF_CP_CHAN_LEN) == 0);
	CHECK(ysf_chan_write(10, "C4FM Rpt", &yc, false));
	CHECK(codeplug_channel(10, &c) && c.rx_only && ysf_chan_load(10, &c, &got));

	// A tag without a block: a group call, DG-ID 0, wide
	c = make("C4FM Simplex", 438625000, 438625000);
	CHECK(codeplug_set_channel(11, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(11, "C4FM Simplex", &t, NULL, 0));
	CHECK(codeplug_channel(11, &c) && ysf_chan_load(11, &c, &got));
	CHECK(memcmp(got.dest, YSF_CP_DEST_ALL, 10) == 0 && got.tx_dgid == 0 && got.rx_dgid == 0 && got.flags == 0);

	// Renamed through the codeplug: the tag goes with the old name, the channel is plain
	channel_t ren = make("Renamed", 438625000, 438625000);
	CHECK(codeplug_set_channel(11, &ren) && codeplug_channel(11, &ren) && ren.kind == CODEPLUG_KIND_NONE);
	CHECK(!ysf_chan_load(11, &ren, &got));
	// Another kind: no set-up
	channel_t dv = make("DV", 438500000, 438500000);
	CHECK(codeplug_set_channel(12, &dv));
	t.kind = CODEPLUG_KIND_DSTAR;
	CHECK(codeplug_tag_write(12, "DV", &t, NULL, 0));
	CHECK(codeplug_channel(12, &dv) && dv.kind == CODEPLUG_KIND_DSTAR && !ysf_chan_load(12, &dv, &got));
}

static void test_write_keeps(void)
{
	fresh();
	channel_t c = make("C4FM Ref", 439412500, 431812500);
	CHECK(codeplug_set_channel(20, &c));
	// A tag a later firmware wrote: reserved tag bit 3, a kind bit, reserved block bits, two bytes appended
	uint8_t blk[YSF_CP_CHAN_LEN + 2];
	memset(blk, ' ', sizeof blk);
	blk[YSF_CP_CHAN_LEN - 1u] = 0x80;
	blk[YSF_CP_CHAN_LEN] = 0x12;
	blk[YSF_CP_CHAN_LEN + 1u] = 0x34;
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_YSF, .system = CODEPLUG_SYSTEM_NONE, .flags = 0x08 | 0x20 };
	CHECK(codeplug_tag_write(20, "C4FM Ref", &t, blk, sizeof blk));
	ysf_chan_t yc;
	ysf_field_set(yc.dest, YSF_CALLSIGN_LEN, "G4XYZ");
	yc.tx_dgid = 20;
	yc.rx_dgid = 20;
	yc.flags = YSF_CP_NARROW;
	CHECK(ysf_chan_write(20, "C4FM Ref", &yc, true));
	uint8_t v[64];
	int n = codeplug_kv_get((uint16_t)(CODEPLUG_KEY_CH_TAG + 20 - 1), v, sizeof v);
	CHECK(n == (int)(CODEPLUG_TAG_HDR + sizeof blk) && v[4] == (0x08 | 0x20 | CODEPLUG_TAG_TX));
	CHECK(v[CODEPLUG_TAG_HDR + YSF_CP_CHAN_LEN - 1u] == (0x80 | YSF_CP_NARROW));
	CHECK(v[n - 2] == 0x12 && v[n - 1] == 0x34);
	ysf_chan_t got;
	CHECK(codeplug_channel(20, &c) && ysf_chan_load(20, &c, &got) && memcmp(got.dest, "G4XYZ     ", 10) == 0);
	CHECK(got.tx_dgid == 20 && got.flags == YSF_CP_NARROW);
}

static void test_station(void)
{
	fresh();
	ysf_station_t s;
	memset(&s, 'x', sizeof s);
	CHECK(!ysf_station_load(&s));
	CHECK(memcmp(s.call, "          ", 10) == 0 && memcmp(s.radio_id, "     ", 5) == 0);
	ysf_field_set(s.call, YSF_CALLSIGN_LEN, "g4xyz");
	ysf_field_set(s.radio_id, YSF_RADIO_ID_LEN, "e0123");
	uint8_t r[YSF_CP_STATION_LEN];
	ysf_station_encode(&s, r);
	CHECK(memcmp(r, "G4XYZ     E0123", YSF_CP_STATION_LEN) == 0);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_YSF), r, sizeof r));
	ysf_station_t got;
	CHECK(ysf_station_load(&got) && memcmp(&got, &s, sizeof s) == 0);
	// The D-STAR record beside it is another key: neither reads the other's
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_DSTAR), (const uint8_t *)"N0CALL", 6));
	CHECK(ysf_station_load(&got) && memcmp(got.call, "G4XYZ     ", 10) == 0);
}
#endif

int main(void)
{
	test_registry();
#if CONFIG_YSF
	test_vectors();
	test_filter();
	test_load();
	test_write_keeps();
	test_station();
#endif
	sim_flash_free();
	if (failures) {
		printf("ysfCodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("ysfCodeplugTest%s: all passed\n", CONFIG_YSF ? "" : " (CONFIG_YSF=0)");
	return 0;
}
