// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 in the codeplug (core/p25/p25_codeplug.c, docs/spec/p25-codeplug.md) on a simulated NOR flash: the shared vectors
// (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), defaults, zeroed and short blocks, a
// conventional channel's set-up from its tag, the radio-wide record and its record; with CONFIG_P25_TRUNK a trunked
// system with several sites, its control channels in hunt order, the identifier plan and the fallback
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "p25/p25_codeplug.h"
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

static uint32_t h(const char *s) { return (uint32_t)strtoul(s, NULL, 16); }

// The shared vectors, each kind against the codec
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
		char *w[10] = { 0 };
		int n = 0;
		char *rest = NULL;
		for (char *p = strtok_r(line, " ", &rest); p != NULL && n < 10; p = strtok_r(NULL, " ", &rest)) {
			w[n++] = p;
		}
		uint8_t b[64];
		if (strcmp(w[0], "check") == 0) {
			// the name is the rest of the line, rebuilt from its words
			char name[CODEPLUG_NAME_LEN + 1] = "";
			for (int i = 2; i < n; i++) {
				if (i > 2) {
					strcat(name, " ");
				}
				strcat(name, w[i]);
			}
			CHECK(codeplug_name_check(name) == h(w[1]));
		} else if (strcmp(w[0], "p25conv") == 0) {
			p25_conv_t c;
			p25_conv_decode(b, unhex(w[1], b, sizeof b), &c);
			CHECK(c.rx_nac == h(w[2]) && c.tx_nac == h(w[3]) && c.squelch == h(w[4]));
		} else if (strcmp(w[0], "p25enc") == 0) {
			p25_conv_t c = { (uint16_t)h(w[1]), (uint16_t)h(w[2]), (uint8_t)h(w[3]) };
			uint8_t e[P25_CP_CONV_LEN];
			p25_conv_encode(&c, e);
			CHECK(unhex(w[4], b, sizeof b) == P25_CP_CONV_LEN && memcmp(e, b, P25_CP_CONV_LEN) == 0);
		} else if (strcmp(w[0], "p25unit") == 0) {
			int len = unhex(w[1], b, sizeof b);
			CHECK(p25_unit_id_decode(b, len == 0 ? -1 : len, h(w[2])) == h(w[3]));
#if CONFIG_P25_TRUNK
		} else if (strcmp(w[0], "p25tsys") == 0) {
			p25_tsys_t s;
			p25_tsys_decode(b, unhex(w[1], b, sizeof b), &s);
			CHECK(s.wacn == h(w[2]) && s.sysid == h(w[3]) && s.cc_nac == h(w[4]) && s.options == h(w[5]) &&
			      s.hold_s == h(w[6]) && (uint8_t)s.roam_dbm == h(w[7]));
		} else if (strcmp(w[0], "p25iden") == 0) {
			p25_iden_t i;
			int len = unhex(w[1], b, sizeof b);
			p25_iden_decode(b, len, &i);
			CHECK(i.base_hz == h(w[2]) && i.spacing_hz == h(w[3]) && (uint32_t)i.tx_offset_hz == h(w[4]) &&
			      i.bw_hz == h(w[5]) && i.type == h(w[6]) && i.slots == h(w[7]));
			if (len == (int)P25_CP_IDEN_LEN && b[17] != 0) {
				uint8_t e[P25_CP_IDEN_LEN];
				p25_iden_encode(&i, e);
				CHECK(memcmp(e, b, P25_CP_IDEN_LEN) == 0);
			}
#endif
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

static void test_conv_load(void)
{
	fresh();
	CHECK(codeplug_set_dmr_id(1234567));
	channel_t c = make("P25 Ops", 453100000, 458100000);
	CHECK(codeplug_set_channel(10, &c));
	p25_conv_t pc = { 0x34, 0x293, P25_SQUELCH_SELECTIVE };
	uint8_t blk[P25_CP_CONV_LEN];
	p25_conv_encode(&pc, blk);
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, "P25 Ops", &t, blk, sizeof blk));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_P25 && !c.rx_only);
	// Not built on this tree: the radio parks it, whatever the codec says
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_P25) && strcmp(codeplug_kind_name(c.kind), "P25") == 0);

	p25_conv_cfg_t cfg;
	CHECK(p25_conv_load(10, &c, &cfg));
	CHECK(cfg.conv.rx_nac == 0x34 && cfg.conv.tx_nac == 0x293 && cfg.conv.squelch == P25_SQUELCH_SELECTIVE);
	CHECK(cfg.unit_id == 1234567 && cfg.target == P25_CP_TG_DEFAULT && !cfg.target_unit && !cfg.talkaround);
	// The radio-wide unit ID, then the channel's own
	uint8_t u[4] = { 0x40, 0xE2, 0x01, 0x00 };	// 123456
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25), u, 4));
	CHECK(p25_conv_load(10, &c, &cfg) && cfg.unit_id == 123456);
	c.dmr_id = 7654321;
	c.dmo = true;
	CHECK(p25_conv_load(10, &c, &cfg) && cfg.unit_id == 7654321 && cfg.talkaround);

	// A tag without a block: the defaults, receive only
	c = make("Zeroed", 453200000, 453200000);
	CHECK(codeplug_set_channel(11, &c));
	t.flags = 0;
	CHECK(codeplug_tag_write(11, "Zeroed", &t, NULL, 0));
	CHECK(codeplug_channel(11, &c) && c.kind == CODEPLUG_KIND_P25 && c.rx_only);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.conv.rx_nac == P25_NAC_DEFAULT && cfg.conv.tx_nac == P25_NAC_DEFAULT &&
	      cfg.conv.squelch == P25_SQUELCH_NORMAL && cfg.talkaround);
	// Another kind's channel, or a plain one: no set-up
	channel_t plain = make("Plain", 453300000, 453300000);
	CHECK(codeplug_set_channel(12, &plain) && codeplug_channel(12, &plain) && !p25_conv_load(12, &plain, &cfg));
}

#if CONFIG_P25_TRUNK
static void put(uint16_t key, const uint8_t *head, unsigned hlen, const uint8_t *body, unsigned blen)
{
	uint8_t v[96];
	memcpy(v, head, hlen);
	memcpy(&v[hlen], body, blen);
	CHECK(codeplug_kv_set(key, v, (uint16_t)(hlen + blen)));
}

static void put_ctl(uint8_t n, uint8_t site, uint32_t rx)
{
	uint8_t hd[4] = { 2, CODEPLUG_ENTRY_CHANNEL, site, CODEPLUG_ENTRY_CONTROL };
	uint8_t b[10] = { 0 };
	for (int i = 0; i < 4; i++) {
		b[2 + i] = (uint8_t)(rx >> (8 * i));
	}
	put((uint16_t)(CODEPLUG_KEY_SYS_CHAN + n), hd, 4, b, 10);
}

static void test_trunk_load(void)
{
	fresh();
	CHECK(codeplug_set_dmr_id(1234567));
	// System 2: listen only (flag bit 1 clear), its own unit ID, a fallback
	uint8_t hd[CODEPLUG_SYS_HDR];
	memset(hd, 0, sizeof hd);
	hd[0] = CODEPLUG_SYSTYPE_P25_TRUNK;
	hd[3] = 5;
	hd[4] = 0x15;	// unit 0x000115
	hd[5] = 0x01;
	hd[8] = 20;
	uint16_t fbc = codeplug_name_check("Fallback");
	hd[10] = (uint8_t)fbc;
	hd[11] = (uint8_t)(fbc >> 8);
	memset(&hd[12], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&hd[12], "County", 6);
	p25_tsys_t s = { 0xBEE00, 0x3A1, P25_NAC_ANY, P25_TSYS_SKIP_ENC, 3, -95 };
	uint8_t blk[P25_CP_TSYS_LEN];
	p25_tsys_encode(&s, blk);
	put(CODEPLUG_KEY_SYSTEM + 2, hd, sizeof hd, blk, sizeof blk);
	// Two sites, three control channels in hunt order (stored out of order), two plan rows
	uint8_t sh[4] = { 2, CODEPLUG_ENTRY_SITE, 0, 0 }, sb[2] = { 1, 4 };
	put(CODEPLUG_KEY_SYS_CHAN + 0, sh, 4, sb, 2);
	sh[2] = 1;
	sb[1] = 7;
	put(CODEPLUG_KEY_SYS_CHAN + 1, sh, 4, sb, 2);
	put_ctl(9, 1, 851500000);
	put_ctl(3, 0, 851012500);
	put_ctl(4, 0, 851025000);
	p25_iden_t id = { 851006250, 6250, -45000000, 12500, P25_IDEN_FDMA, 1 };
	uint8_t ib[P25_CP_IDEN_LEN];
	p25_iden_encode(&id, ib);
	uint8_t ph[4] = { 2, CODEPLUG_ENTRY_PLAN, 0, 0 };
	put(CODEPLUG_KEY_SYS_CHAN + 20, ph, 4, ib, sizeof ib);
	id.type = P25_IDEN_TDMA;
	id.slots = 2;
	id.spacing_hz = 12500;
	p25_iden_encode(&id, ib);
	ph[2] = 3;
	put(CODEPLUG_KEY_SYS_CHAN + 21, ph, 4, ib, sizeof ib);

	channel_t fb = make("Fallback", 453500000, 453500000);
	fb.mode = RADIO_MODE_FM;
	fb.rx_only = false;
	CHECK(codeplug_set_channel(20, &fb));
	channel_t c = make("County TG", 851012500, 806012500);
	CHECK(codeplug_set_channel(21, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25_TRUNK, .system = 2 };
	uint8_t p = P25_TCH_FOLLOW_ALL;
	CHECK(codeplug_tag_write(21, "County TG", &t, &p, 1));
	CHECK(codeplug_channel(21, &c) && c.kind == CODEPLUG_KIND_P25_TRUNK && c.system == 2 && c.rx_only);

	static p25_trunk_cfg_t cfg;
	CHECK(p25_trunk_load(21, &c, &cfg));
	CHECK(cfg.sys.wacn == 0xBEE00 && cfg.sys.sysid == 0x3A1 && cfg.sys.cc_nac == P25_NAC_ANY && cfg.sys.hold_s == 3);
	CHECK(cfg.sys.options == P25_TSYS_SKIP_ENC && cfg.sys.roam_dbm == -95 && cfg.listen_only && cfg.unit_id == 0x115);
	CHECK(cfg.ctl_count == 3 && cfg.ctl[0].rx_hz == 851012500 && cfg.ctl[1].rx_hz == 851025000 &&
	      cfg.ctl[2].rx_hz == 851500000 && cfg.ctl_site[2] == 1);
	CHECK(cfg.iden_set == 0x0009 && cfg.iden[0].tx_offset_hz == -45000000 && cfg.iden[3].type == P25_IDEN_TDMA &&
	      cfg.iden[3].slots == 2);
	CHECK(cfg.follow_all && cfg.talk_group == P25_CP_TG_DEFAULT && cfg.fallback == 20);

	// The channel's own "no fallback"; a fallback renamed under the system: none
	t.flags = P25_TAG_NO_FALLBACK;
	CHECK(codeplug_tag_write(21, "County TG", &t, NULL, 0));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.fallback == 0 && !cfg.follow_all);
	t.flags = 0;
	CHECK(codeplug_tag_write(21, "County TG", &t, NULL, 0));
	strcpy(fb.name, "Renamed");
	CHECK(codeplug_set_channel(20, &fb));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.fallback == 0);

	// A zeroed system block: any identity, any NAC, the radio's defaults; the system's transmit side allowed
	memset(blk, 0, sizeof blk);
	hd[1] = CODEPLUG_SYS_TX;
	hd[4] = hd[5] = 0;
	put(CODEPLUG_KEY_SYSTEM + 2, hd, sizeof hd, blk, 0);
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.sys.wacn == 0 && cfg.sys.cc_nac == P25_NAC_ANY && !cfg.listen_only);
	CHECK(cfg.unit_id == 1234567);
	// No system: no set-up
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYSTEM + 2));
	CHECK(!p25_trunk_load(21, &c, &cfg));
}
#endif

int main(void)
{
	test_vectors();
	test_conv_load();
#if CONFIG_P25_TRUNK
	test_trunk_load();
#endif
	sim_flash_free();
	if (failures) {
		printf("p25CodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("p25CodeplugTest: all passed\n");
	return 0;
}
