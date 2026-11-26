// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 in the codeplug (core/p25/p25_codeplug.c, docs/spec/p25-codeplug.md) on a simulated NOR flash: the shared vectors
// (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), defaults, zeroed and short blocks, a
// conventional channel's set-up from its tag, the radio-wide record and its record; with CONFIG_P25_TRUNK a trunked
// system with several sites, its control channels in hunt order, the identifier plan and the fallback; a channel whose
// contact and RX group list are P25 contact slots (the flag bits) against the air mode contact table
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "p25/p25_codeplug.h"
#include "codeplug/codeplug_air_contacts.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

// The board's air interface (core/radio/radio_p25.c in the firmware): whether the radio runs kind 2 here
static bool fits;
bool radio_p25_fits(void) { return fits; }

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
			p25_conv_t c = { (uint16_t)h(w[1]), (uint16_t)h(w[2]), (uint8_t)h(w[3]), false };
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
	p25_conv_t pc = { 0x34, 0x293, P25_SQUELCH_SELECTIVE, false };
	uint8_t blk[P25_CP_CONV_LEN];
	p25_conv_encode(&pc, blk);
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(10, "P25 Ops", &t, blk, sizeof blk));
	CHECK(codeplug_channel(10, &c) && c.kind == CODEPLUG_KIND_P25 && !c.rx_only);
	// Run where the board's air interface carries P25, parked elsewhere, whatever the codec says
	fits = false;
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_P25) && strcmp(codeplug_kind_name(c.kind), "P25") == 0);
	fits = true;
	CHECK(codeplug_kind_built(CODEPLUG_KIND_P25));
	p25_conv_t only;
	CHECK(p25_conv_params(10, &c, &only) && only.rx_nac == 0x34 && only.squelch == P25_SQUELCH_SELECTIVE);

	p25_conv_cfg_t cfg;
	CHECK(p25_conv_load(10, &c, &cfg));
	CHECK(cfg.conv.rx_nac == 0x34 && cfg.conv.tx_nac == 0x293 && cfg.conv.squelch == P25_SQUELCH_SELECTIVE);
	CHECK(cfg.unit_id == 1234567 && cfg.target == P25_CP_TG_DEFAULT && !cfg.target_unit && !cfg.talkaround);
	// The radio-wide unit ID, then the channel's own
	uint8_t u[4] = { 0x40, 0xE2, 0x01, 0x00 };	// 123456
	CHECK(p25_radio_unit_id(3101276) == 3101276);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_P25), u, 4));
	CHECK(p25_conv_load(10, &c, &cfg) && cfg.unit_id == 123456);
	CHECK(p25_radio_unit_id(3101276) == 123456);
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
	CHECK(!p25_conv_params(12, &plain, &only));
}

// A P25 channel written from the radio (2.3): the safe stand-in first, then a new tag; an edit after lets it follow
static void test_conv_save(void)
{
	fresh();
	channel_t c = make("Save", 453100000, 458100000);
	c.mode = RADIO_MODE_P25;	// as the radio core runs it
	c.rx_only = false;
	c.contact = 0;
	p25_conv_t pc = { 0x2A0, 0x2A1, P25_SQUELCH_SELECTIVE, false };
	// Over a plain channel with a stale tag of another kind at the number: the tag is replaced
	channel_t old = make("Old", 453100000, 453100000);
	CHECK(codeplug_set_channel(30, &old));
	codeplug_tag_t t = { .kind = 0x77, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(30, "Old", &t, NULL, 0));
	CHECK(p25_conv_save(30, &c, &pc));
	channel_t r;
	CHECK(codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_P25 && !r.rx_only && r.mode == RADIO_MODE_DMR);
	codeplug_tag_t rt;
	CHECK(codeplug_tag_read(30, &rt) && rt.kind == CODEPLUG_KIND_P25 && rt.system == CODEPLUG_SYSTEM_NONE &&
	      (rt.flags & CODEPLUG_TAG_TX) != 0 && (rt.flags & CODEPLUG_TAG_BASE_OWN) == 0);
	p25_conv_t back;
	CHECK(p25_conv_params(30, &r, &back) && back.rx_nac == 0x2A0 && back.tx_nac == 0x2A1 &&
	      back.squelch == P25_SQUELCH_SELECTIVE);
	// The record underneath is the stand-in: a reader that knows no tags sees DMR, receive only
	codeplug_tag_remove(30);
	CHECK(codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_NONE && r.mode == RADIO_MODE_DMR && r.rx_only);
	CHECK(p25_conv_save(30, &c, &pc) && codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_P25);
	// Renamed from the radio: the tag follows, its block kept
	strcpy(r.name, "Saved");
	r.rx_only = true;
	CHECK(codeplug_set_channel(30, &r) && codeplug_channel(30, &r) && r.kind == CODEPLUG_KIND_P25 && r.rx_only);
	CHECK(p25_conv_params(30, &r, &back) && back.rx_nac == 0x2A0);
	// Receive only from the radio: the tag says so
	c.rx_only = true;
	CHECK(p25_conv_save(31, &c, &pc) && codeplug_channel(31, &r) && r.kind == CODEPLUG_KIND_P25 && r.rx_only);
	// The block edited on a channel that is P25 already (the channel editor's NAC): the tag keeps its system, its
	// fallback, its other flags and what its block holds after this firmware's; transmit allowed from the channel
	uint8_t longer[P25_CP_CONV_LEN + 3];
	uint8_t raw[sizeof longer + CODEPLUG_TAG_HEAD_MAX];
	p25_conv_encode(&pc, longer);
	memcpy(&longer[P25_CP_CONV_LEN], "\xA1\xA2\xA3", 3);
	channel_t base = make("Keep", 453100000, 458100000);
	CHECK(codeplug_set_channel(33, &base));
	codeplug_tag_t kt = { .kind = CODEPLUG_KIND_P25, .system = 2, .fb_chan = 30, .fb_check = 0x1234,
			      .flags = CODEPLUG_TAG_FALLBACK | 0x40u };
	CHECK(codeplug_tag_write(33, "Keep", &kt, longer, sizeof longer));
	CHECK(codeplug_channel(33, &r) && r.kind == CODEPLUG_KIND_P25 && r.system == 2 && r.rx_only);
	strcpy(r.name, "Kept");
	r.rx_only = false;
	r.mode = RADIO_MODE_P25;
	p25_conv_t ed = { 0x123, 0x124, P25_SQUELCH_NORMAL, false };
	CHECK(p25_conv_save(33, &r, &ed));
	CHECK(codeplug_channel(33, &r) && r.kind == CODEPLUG_KIND_P25 && r.system == 2 && !r.rx_only &&
	      r.mode == RADIO_MODE_DMR && strcmp(r.name, "Kept") == 0);
	CHECK(codeplug_tag_read(33, &rt) && rt.flags == (CODEPLUG_TAG_FALLBACK | CODEPLUG_TAG_TX | 0x40u) &&
	      rt.fb_chan == 30 && rt.fb_check == 0x1234 && rt.len == CODEPLUG_TAG_HEAD_MAX + sizeof longer);
	CHECK(p25_conv_params(33, &r, &back) && back.rx_nac == 0x123 && back.tx_nac == 0x124 &&
	      back.squelch == P25_SQUELCH_NORMAL);
	CHECK(codeplug_tag_params(33, &r, raw, sizeof longer) == (int)sizeof longer &&
	      memcmp(&raw[P25_CP_CONV_LEN], "\xA1\xA2\xA3", 3) == 0);
	// A mixed channel: its record is an FM channel of its own (wide, transmitting), its tag the P25 side (receive
	// only). The NAC edited on the radio, which holds the channel as it runs (P25, narrow, the tag's receive only):
	// the FM side stays as stored, and so does the tag's flag that says the record is its own
	channel_t fm = make("Mixed", 453200000, 458200000);
	fm.mode = RADIO_MODE_FM;
	fm.bw = RADIO_BW_25K;
	fm.rx_only = false;
	CHECK(codeplug_set_channel(34, &fm));
	codeplug_tag_t mt = { .kind = CODEPLUG_KIND_P25, .flags = CODEPLUG_TAG_BASE_OWN };
	p25_conv_encode(&pc, longer);
	CHECK(codeplug_tag_write(34, "Mixed", &mt, longer, P25_CP_CONV_LEN));
	CHECK(codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_P25 && r.rx_only && r.mode == RADIO_MODE_FM &&
	      r.bw == RADIO_BW_25K);
	r.mode = RADIO_MODE_P25;
	r.bw = RADIO_BW_12K5;
	CHECK(p25_conv_save(34, &r, &ed));
	CHECK(codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_P25 && r.rx_only && r.mode == RADIO_MODE_FM &&
	      r.bw == RADIO_BW_25K && r.tx_hz == 458200000);
	CHECK(codeplug_tag_read(34, &rt) && rt.flags == CODEPLUG_TAG_BASE_OWN);
	CHECK(p25_conv_params(34, &r, &back) && back.rx_nac == 0x123 && back.tx_nac == 0x124);
	CHECK(codeplug_tag_remove(34) && codeplug_channel(34, &r) && r.kind == CODEPLUG_KIND_NONE && !r.rx_only &&
	      r.mode == RADIO_MODE_FM && r.bw == RADIO_BW_25K);
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
	// Independent extended Site records, including unknown suffixes. The generic group is not RFSS or Site ID.
	static const uint8_t site0[] = { 1, 2, 0x23, 0x81, 0xDE, 0xAD };
	static const uint8_t site1[] = { 3, 4, 0xBC, 0x8A, 0xBE, 0xEF };
	sh[2] = 0;
	put(CODEPLUG_KEY_SYS_CHAN, sh, 4, site0, sizeof site0);
	sh[2] = 1;
	put(CODEPLUG_KEY_SYS_CHAN + 1, sh, 4, site1, sizeof site1);
	CHECK(p25_trunk_load(21, &c, &cfg));
	CHECK(cfg.ctl_identity[0].nac_set && cfg.ctl_identity[0].nac == 0x123 &&
	      cfg.ctl_identity[1].nac == 0x123 && cfg.ctl_identity[2].nac == 0xABC);
	CHECK(cfg.ctl_site[2] == 1 && cfg.ctl_identity[2].rfss == 3 && cfg.ctl_identity[2].site == 4);
	// Legacy Site bodies retain System inheritance; explicit Auto overrides a fixed System default.
	sh[2] = 0;
	put(CODEPLUG_KEY_SYS_CHAN, sh, 4, site0, 2);
	static const uint8_t automatic[] = { 3, 4, 0, 0 };
	sh[2] = 1;
	put(CODEPLUG_KEY_SYS_CHAN + 1, sh, 4, automatic, sizeof automatic);
	CHECK(p25_trunk_load(21, &c, &cfg) && !cfg.ctl_identity[0].nac_set &&
	      cfg.ctl_identity[2].nac_set && cfg.ctl_identity[2].nac == P25_NAC_ANY);

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
	// More than the previous 16-channel ceiling: retain all 22 candidates and site groups.
	for (uint8_t n = 0; n < 19; n++)
		put_ctl((uint8_t)(30 + n), (uint8_t)(n % 6), 453000000u + n * 25000u);
	const uint8_t expected = P25_CP_CTL_MAX < 22 ? P25_CP_CTL_MAX : 22;
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.ctl_count == expected);
	CHECK(cfg.ctl[expected - 1].rx_hz == 453000000u + (expected - 4u) * 25000u &&
	      cfg.ctl_site[expected - 1] == (expected - 4u) % 6u);
	// No system: no set-up
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_SYSTEM + 2));
	CHECK(!p25_trunk_load(21, &c, &cfg));
}

static void test_site_nac(void)
{
	static const struct { const char *body; bool set; uint16_t nac; } cases[] = {
		{ "0102", false, P25_NAC_ANY }, { "0102AA", false, P25_NAC_ANY },
		{ "01020000", true, P25_NAC_ANY }, { "01020080", true, 0 },
		{ "01022381", true, 0x123 }, { "0102BC8A", true, 0xABC },
		{ "0102FFFF", false, P25_NAC_ANY }, { "01020010", false, P25_NAC_ANY },
		{ "01020100", false, P25_NAC_ANY }, { "01022381DEAD", true, 0x123 },
		{ "01027E8F", true, P25_NAC_ANY }, { "01027F8F", false, P25_NAC_ANY },
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		uint8_t b[8];
		p25_site_t s;
		int len = unhex(cases[i].body, b, sizeof b);
		p25_site_decode(b, len, &s);
		CHECK(s.rfss == 1 && s.site == 2 && s.nac_set == cases[i].set && s.nac == cases[i].nac);
	}
}
#endif

// Slot n of the air mode contact table as the Codeplug Editor writes it (air-contacts.md 3); mode 0xFF is an empty slot
static void air_put(uint16_t n, uint8_t mode, uint8_t type, uint32_t id, const char *name)
{
	uint8_t *r = &sim_flash_data[CODEPLUG_AIR_CONTACTS_ADDR + (n - 1u) * CODEPLUG_AIR_CONTACT_SIZE];
	memset(r, 0xFF, CODEPLUG_AIR_CONTACT_SIZE);
	r[0] = mode;
	r[1] = type;
	r[2] = (uint8_t)id;
	r[3] = (uint8_t)(id >> 8);
	r[4] = (uint8_t)(id >> 16);
	memcpy(&r[8], name, strlen(name));
}

// The DMR contacts and the P25 slots of the tests below: contact n and slot n are different things on purpose
static void air_fill(void)
{
	contact_t c = { .id = 91, .type = CONTACT_GROUP };
	strcpy(c.name, "DMR 91");
	CHECK(codeplug_set_contact(1, &c));
	c.id = 5000;
	strcpy(c.name, "DMR 5000");
	CHECK(codeplug_set_contact(2, &c));
	c.id = 77;
	c.type = CONTACT_PRIVATE;
	strcpy(c.name, "DMR 77");
	CHECK(codeplug_set_contact(3, &c));
	air_put(1, CODEPLUG_KIND_P25, 1, 301, "G301");
	air_put(2, CODEPLUG_KIND_P25, 0, 4242, "U4242");
	air_put(3, CODEPLUG_KIND_P25, 2, 0, "All");
	// slot 4 empty, slot 5 another mode, slot 6 a call type out of range, slot 7 a group out of range
	air_put(4, 0xFF, 0xFF, 0xFFFFFF, "");
	air_put(5, CODEPLUG_KIND_NXDN, 1, 302, "Nx");
	air_put(6, CODEPLUG_KIND_P25, 3, 303, "Bad");
	air_put(7, CODEPLUG_KIND_P25, 1, 0, "Zero");
	air_put(8, CODEPLUG_KIND_P25, 1, 305, "G305");
}

static void air_list(uint16_t index, uint8_t count, const uint16_t *idx)
{
	listen_list_t l;
	memset(&l, 0, sizeof l);
	strcpy(l.name, "List");
	l.count = count;
	memcpy(l.contacts, idx, count * sizeof idx[0]);
	CHECK(codeplug_set_listen_list(index, &l));
}

// A P25 channel's flag bit, with its block, written as the Codeplug Editor does
static channel_t air_channel(uint16_t number, uint16_t contact, uint16_t list, bool flag, bool selective)
{
	channel_t c = make("Air", 453100000, 458100000);
	c.contact = contact;
	c.listen_list = list;
	CHECK(codeplug_set_channel(number, &c));
	uint8_t blk[P25_CP_CONV_LEN] = { (uint8_t)((flag ? P25_CONV_AIR_CONTACTS : 0u) | (selective ? 1u : 0u)), 0, 0, 0, 0 };
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	CHECK(codeplug_tag_write(number, "Air", &t, blk, sizeof blk));
	CHECK(codeplug_channel(number, &c));
	return c;
}

// The flag bit (2), the talk target from a P25 contact slot, and the same slot number read as a DMR contact without it
static void test_conv_air(void)
{
	fresh();
	fits = true;
	CHECK(codeplug_set_dmr_id(1234567));
	air_fill();

	// The bit in the block: bit 2 of byte 0, beside the squelch; a short block and the others' bits read clear
	uint8_t b[P25_CP_CONV_LEN] = { 0x04, 0, 0, 0, 0 };
	p25_conv_t pc;
	p25_conv_decode(b, sizeof b, &pc);
	CHECK(pc.air_contacts && pc.squelch == P25_SQUELCH_NORMAL);
	b[0] = 0x05;
	p25_conv_decode(b, sizeof b, &pc);
	CHECK(pc.air_contacts && pc.squelch == P25_SQUELCH_SELECTIVE);
	b[0] = 0xFB;
	p25_conv_decode(b, sizeof b, &pc);
	CHECK(!pc.air_contacts);
	p25_conv_decode(b, 0, &pc);
	CHECK(!pc.air_contacts);
	pc = (p25_conv_t){ 0x293, 0x293, P25_SQUELCH_SELECTIVE, true };
	p25_conv_encode(&pc, b);
	CHECK(b[0] == 0x05);
	pc.air_contacts = false;
	p25_conv_encode(&pc, b);
	CHECK(b[0] == 0x01);

	// Flag clear: contact 1 is the DMR contact 91, as ever
	p25_conv_cfg_t cfg;
	p25_talk_t tk;
	channel_t c = air_channel(10, 1, 0, false, false);
	CHECK(p25_conv_load(10, &c, &cfg) && !cfg.conv.air_contacts && cfg.target == 91 && !cfg.target_unit);
	// p25_channel_talk: the DMR contact's target and name, the flag clear
	CHECK(p25_channel_talk(10, &c, true, &tk) && !tk.air && tk.set && tk.id == 91 && !tk.unit &&
	      strcmp(tk.name, "DMR 91") == 0);
	CHECK(!p25_channel_talk(10, &(channel_t){ .kind = CODEPLUG_KIND_NONE }, true, &tk));	// not a P25 channel
	// A DMR private contact and the DMR contact 2 (5000) without the flag
	c = air_channel(10, 3, 0, false, false);
	CHECK(p25_conv_load(10, &c, &cfg) && cfg.target == 77 && cfg.target_unit);
	// Flag set: contact 1 is slot 1, the P25 group 301; the DMR contact 91 is not used
	c = air_channel(11, 1, 0, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.conv.air_contacts && cfg.target == 301 && !cfg.target_unit);
	CHECK(p25_channel_talk(11, &c, true, &tk) && tk.air && tk.set && tk.id == 301 && !tk.unit &&
	      strcmp(tk.name, "G301") == 0);
	// A unit slot calls a unit, the all call slot talk group 65535
	c = air_channel(11, 2, 0, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == 4242 && cfg.target_unit);
	CHECK(p25_channel_talk(11, &c, false, &tk) && tk.air && tk.id == 4242 && tk.unit && strcmp(tk.name, "U4242") == 0);
	c = air_channel(11, 3, 0, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == P25_CP_TG_ALL && !cfg.target_unit);
	CHECK(p25_channel_talk(11, &c, false, &tk) && tk.air && tk.id == P25_CP_TG_ALL && !tk.unit && strcmp(tk.name, "All") == 0);
	// An explicit empty, wrong-mode, invalid or dangling slot has no target, whatever the DMR contact is.
	static const uint16_t none[] = { 4, 5, 6, 7, 9, 341, 342, 1000 };
	for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) {
		c = air_channel(11, none[i], 0, true, false);
		CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == 0 && !cfg.target_unit);
		CHECK(p25_channel_talk(11, &c, true, &tk) && tk.air && !tk.set && tk.id == 0 && tk.name[0] == '\0');
	}
	// No contact: the RX list's first group slot, as a DMR channel's first talk group; its unit slots and empty
	// slots are passed over. A nonempty list without a usable group has no target.
	static const uint16_t l1[] = { 4, 2, 5, 8, 1 };
	air_list(1, 5, l1);
	c = air_channel(11, 0, 1, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == 305 && !cfg.target_unit);
	CHECK(p25_channel_talk(11, &c, true, &tk) && tk.set && tk.id == 305 && strcmp(tk.name, "G305") == 0);
	CHECK(p25_channel_talk(11, &c, false, &tk) && !tk.set && tk.id == P25_CP_TG_DEFAULT);	// a list row reads no list
	static const uint16_t l2[] = { 4, 2, 6 };
	air_list(2, 3, l2);
	c = air_channel(11, 0, 2, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == 0);
	// The contact wins over the list
	c = air_channel(11, 1, 1, true, false);
	CHECK(p25_conv_load(11, &c, &cfg) && cfg.target == 301);
	// The same list on an unflagged channel is a DMR list: indices 4, 2, 5, 8, 1 are DMR contacts (none of 4, 5, 8
	// stored, 2 is 5000)
	c = air_channel(12, 0, 1, false, false);
	CHECK(p25_conv_load(12, &c, &cfg) && cfg.target == 5000);
	// Selective squelch on a flagged channel plays the slot's talk group
	c = air_channel(13, 1, 0, true, true);
	CHECK(p25_conv_load(13, &c, &cfg) && cfg.conv.squelch == P25_SQUELCH_SELECTIVE && cfg.target == 301);

	// An edit from the radio keeps the flag: the block read, written back (2.3)
	c = air_channel(14, 1, 0, true, false);
	CHECK(p25_conv_params(14, &c, &pc) && pc.air_contacts);
	pc.rx_nac = 0x123;
	CHECK(p25_conv_save(14, &c, &pc));
	CHECK(codeplug_channel(14, &c) && p25_conv_load(14, &c, &cfg) && cfg.conv.rx_nac == 0x123 &&
	      cfg.conv.air_contacts && cfg.target == 301);
	// A channel the radio makes P25 has no flag: contact 1 is the DMR contact
	pc = (p25_conv_t){ 0x293, 0x293, P25_SQUELCH_NORMAL, false };
	c = make("Made", 453100000, 458100000);
	c.contact = 1;
	CHECK(p25_conv_save(15, &c, &pc) && codeplug_channel(15, &c) && p25_conv_load(15, &c, &cfg) &&
	      !cfg.conv.air_contacts && cfg.target == 91);
}

#if CONFIG_P25_TRUNK
// The trunked flag bit (3, byte 0 bit 1): the talk group from a slot, the RX list's group slots followed
static void test_trunk_air(void)
{
	fresh();
	CHECK(codeplug_set_dmr_id(1234567));
	air_fill();
	uint8_t hd[CODEPLUG_SYS_HDR];
	memset(hd, 0, sizeof hd);
	hd[0] = CODEPLUG_SYSTYPE_P25_TRUNK;
	hd[1] = CODEPLUG_SYS_TX;
	hd[3] = 5;
	memset(&hd[12], 0xFF, CODEPLUG_NAME_LEN);
	memcpy(&hd[12], "County", 6);
	uint8_t blk[P25_CP_TSYS_LEN] = { 0 };
	put(CODEPLUG_KEY_SYSTEM + 2, hd, sizeof hd, blk, sizeof blk);

	static const uint16_t l1[] = { 4, 2, 8, 1, 3, 5, 1, 6 };
	air_list(1, 8, l1);
	channel_t c = make("County TG", 851012500, 806012500);
	c.contact = 1;
	c.listen_list = 1;
	CHECK(codeplug_set_channel(21, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25_TRUNK, .system = 2 };
	uint8_t p = 0;
	static p25_trunk_cfg_t cfg;
	CHECK(codeplug_tag_write(21, "County TG", &t, &p, 1));
	CHECK(codeplug_channel(21, &c));
	// Flag clear: the DMR contact 1 (91), no list followed
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 91 && cfg.group_count == 0 && !cfg.follow_all);
	// Flag set: slot 1 (301); the list's group slots 8, 1, 1 in order (a unit, an all call, an empty, another mode's
	// and an invalid slot are skipped)
	p = P25_TCH_AIR_CONTACTS;
	CHECK(codeplug_tag_write(21, "County TG", &t, &p, 1));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 301 && !cfg.follow_all);
	p25_talk_t tk;
	CHECK(p25_channel_talk(21, &c, true, &tk) && tk.air && tk.id == 301 && strcmp(tk.name, "G301") == 0);
	CHECK(cfg.group_count == 3 && cfg.groups[0] == 305 && cfg.groups[1] == 301 && cfg.groups[2] == 301);
	// With follow all besides; a unit slot is no supported trunked group target.
	p = P25_TCH_AIR_CONTACTS | P25_TCH_FOLLOW_ALL;
	c.contact = 2;
	CHECK(codeplug_set_channel(21, &c));
	CHECK(codeplug_tag_write(21, "County TG", &t, &p, 1));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.follow_all && cfg.group_count == 3);
	// All call slot: talk group 65535; an empty slot and no list: talk group 1, nothing followed
	c.contact = 3;
	c.listen_list = 0;
	CHECK(codeplug_set_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == P25_CP_TG_ALL && cfg.group_count == 0);
	// No contact: the RX list's first group slot (305), read once; the contact wins when there is one
	c.contact = 0;
	c.listen_list = 1;
	CHECK(codeplug_set_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 305 && cfg.group_count == 3);
	c.contact = 4;
	c.listen_list = 0;
	CHECK(codeplug_set_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.group_count == 0);
	// The block's other bits stay: bit 0 alone is follow all on DMR contacts (index 2 = 5000)
	p = P25_TCH_FOLLOW_ALL;
	CHECK(codeplug_tag_write(21, "County TG", &t, &p, 1));
	c.contact = 2;
	CHECK(codeplug_set_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 5000 && cfg.follow_all && cfg.group_count == 0);
	CHECK(p25_channel_talk(21, &c, false, &tk) && !tk.air && tk.id == 5000 && strcmp(tk.name, "DMR 5000") == 0);
}

static void test_wrong_system_contact(void)
{
	fresh();
	air_fill();
	uint8_t hd[CODEPLUG_SYS_HDR] = { CODEPLUG_SYSTYPE_P25_TRUNK, CODEPLUG_SYS_TX, 0, 5 };
	static const uint8_t sys1[] = { 0x45, 0x23, 1, 0x34, 2 };
	static const uint8_t sys2[] = { 0x56, 0x34, 2, 0x45, 3 };
	put(CODEPLUG_KEY_SYSTEM + 1, hd, sizeof hd, sys1, sizeof sys1);
	put(CODEPLUG_KEY_SYSTEM + 2, hd, sizeof hd, sys2, sizeof sys2);
	air_put(20, CODEPLUG_KIND_P25, 0x05, 1001, "System 1 TG");
	uint8_t *row = &sim_flash_data[CODEPLUG_AIR_CONTACTS_ADDR + 19 * CODEPLUG_AIR_CONTACT_SIZE];
	row[5] = 1;
	row[6] = 0xB6; row[7] = 0x8D;	// independent guard for 03 05 45 23 01 34 02
	static const uint16_t refs[] = { 20, 8 };
	air_list(1, 2, refs);
	channel_t c = make("System 2 TG", 453100000, 458100000);
	c.contact = 20;
	c.listen_list = 1;
	CHECK(codeplug_set_channel(21, &c));
	codeplug_tag_t t = { .kind = CODEPLUG_KIND_P25_TRUNK, .system = 2, .flags = CODEPLUG_TAG_TX };
	uint8_t p = P25_TCH_AIR_CONTACTS;
	CHECK(codeplug_tag_write(21, c.name, &t, &p, 1) && codeplug_channel(21, &c));
	p25_talk_t target;
	p25_trunk_cfg_t cfg;
	CHECK(p25_channel_talk(21, &c, true, &target) && !target.set && target.id == 0 && target.name[0] == '\0');
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.group_count == 1 && cfg.groups[0] == 305);
	// A valid own-System reference resolves first; erasing its row still leaves an explicit dangling slot.
	row[5] = 2; row[6] = 0x8E; row[7] = 0xCD;
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 1001 && cfg.group_count == 2);
	memset(row, 0xFF, CODEPLUG_AIR_CONTACT_SIZE);
	CHECK(p25_channel_talk(21, &c, true, &target) && !target.set && target.id == 0);
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.group_count == 1);
	c.contact = 0xFFFF;
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0);
	// Clearing the scope flag explicitly restores legacy semantics; the old scope bytes stay opaque.
	air_put(20, CODEPLUG_KIND_P25, AIR_CONTACT_GROUP, 1001, "Legacy 1001");
	c.contact = 20;
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 1001);
	// A missing portable RX member is still an explicit selection attempt, even if no scope marker survives.
	static const uint16_t missing[] = { 341 };
	air_list(2, 1, missing);
	// Literal external record bytes bypass the ordinary writer's valid-index checks.
	sim_flash_data[map.openfw_flash + 0x8D6A0u + 80u + CODEPLUG_NAME_LEN] = 0xFF;
	sim_flash_data[map.openfw_flash + 0x8D6A0u + 80u + CODEPLUG_NAME_LEN + 1u] = 0xFF;
	listen_list_t decoded;
	CHECK(codeplug_listen_list(2, &decoded) && decoded.count == 0 && decoded.malformed);
	uint16_t full[CODEPLUG_LIST_LEN];
	for (unsigned i = 0; i < CODEPLUG_LIST_LEN; i++) full[i] = 8;
	air_list(3, CODEPLUG_LIST_LEN, full);
	sim_flash_data[map.openfw_flash + 0x8D620u + 2u] = CODEPLUG_LIST_LEN + 2;
	CHECK(codeplug_listen_list(3, &decoded) && decoded.count == CODEPLUG_LIST_LEN && decoded.malformed);
	c.contact = 0;
	c.listen_list = 2;
	CHECK(codeplug_set_channel(21, &c) && codeplug_channel(21, &c));
	CHECK(p25_channel_talk(21, &c, true, &target) && !target.set && target.id == 0);
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.group_count == 0);
	// The portable typed tombstone stays in the generic list but remains outside the 341-slot air table.
	static const uint16_t portable_missing[] = { 1024 };
	air_list(2, 1, portable_missing);
	CHECK(codeplug_listen_list(2, &decoded) && decoded.count == 1 && !decoded.malformed);
	CHECK(p25_channel_talk(21, &c, true, &target) && target.id == 0 && !target.set);
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0 && cfg.group_count == 0);
	// The list owner reports an I/O failure separately; it cannot invent the legacy default either.
	sim_flash_bad_read(map.openfw_flash + 0x8D6A0u + 80u, 80u);
	CHECK(p25_channel_talk(21, &c, true, &target) && target.id == 0 && !target.set);
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == 0);
	sim_flash_bad_read(0, 0);
	// Truly empty and absent lists still retain the unspecified-target legacy default.
	air_list(4, 0, full);
	c.listen_list = 4;
	CHECK(codeplug_set_channel(21, &c) && codeplug_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == P25_CP_TG_DEFAULT);
	c.listen_list = 0;
	CHECK(codeplug_set_channel(21, &c) && codeplug_channel(21, &c));
	CHECK(p25_trunk_load(21, &c, &cfg) && cfg.talk_group == P25_CP_TG_DEFAULT);
	// A conventional personality cannot select a System-qualified slot, even on the same NAC.
	row[1] |= AIR_CONTACT_SYSTEM; row[5] = 2; row[6] = 0x8E; row[7] = 0xCD;
	c = air_channel(22, 20, 0, true, false);
	p25_conv_cfg_t conv;
	CHECK(p25_conv_load(22, &c, &conv) && conv.target == 0);
}
#endif

int main(void)
{
	test_vectors();
	test_conv_load();
	test_conv_save();
	test_conv_air();
#if CONFIG_P25_TRUNK
	test_trunk_load();
	test_trunk_air();
	test_wrong_system_contact();
	test_site_nac();
#endif
	sim_flash_free();
	if (failures) {
		printf("p25CodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("p25CodeplugTest: all passed\n");
	return 0;
}
