// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Codeplug editing on a simulated NOR flash with the DM-1701 map: the open-firmware record encoder against the decoder,
// bytes the model doesn't own kept, the built-in set turned into a stored codeplug by the first edit, zones,
// contacts and RX group lists, and the round trip read -> write -> read (model and image), with power cuts. Another
// firmware's codeplug in the open-firmware area: left alone, the first edit starts a clean codeplug.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static bool same_channel(const channel_t *a, const channel_t *b)
{
	return strcmp(a->name, b->name) == 0 && a->rx_hz == b->rx_hz && a->tx_hz == b->tx_hz && a->mode == b->mode &&
	       a->bw == b->bw && a->rx_tone.type == b->rx_tone.type && a->rx_tone.value == b->rx_tone.value &&
	       a->tx_tone.type == b->tx_tone.type && a->tx_tone.value == b->tx_tone.value && a->power == b->power &&
	       a->rx_only == b->rx_only && a->color_code == b->color_code && a->timeslot == b->timeslot &&
	       a->contact == b->contact && a->listen_list == b->listen_list && a->dmr_id == b->dmr_id && a->dmo == b->dmo &&
	       a->vox == b->vox && a->scan_skip == b->scan_skip && a->no_beep == b->no_beep && a->no_eco == b->no_eco;
}

static channel_t make(const char *name, uint32_t rx, uint32_t tx)
{
	channel_t c;
	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = tx;
	c.mode = RADIO_MODE_FM;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

static void test_encode(void)
{
	channel_t v[6];
	v[0] = make("FM wide", 146520000, 146520000);
	v[0].bw = RADIO_BW_25K;
	v[0].tx_tone = (subtone_t){ TONE_CTCSS, 885 };
	v[0].rx_tone = (subtone_t){ TONE_CTCSS, 1318 };
	v[1] = make("DCS", 446006250, 446006250);
	v[1].rx_tone = (subtone_t){ TONE_DCS_INV, 023 };
	v[1].tx_tone = (subtone_t){ TONE_DCS, 0754 };
	v[1].power = 0;
	v[2] = make("DMR rpt 16 chars", 438500000, 430900000);
	v[2].mode = RADIO_MODE_DMR;
	v[2].color_code = 15;
	v[2].timeslot = 2;
	v[2].contact = 1024;
	v[2].listen_list = 76;
	v[2].power = 8;
	v[3] = make("ID+DMO", 145500000, 145100000);
	v[3].mode = RADIO_MODE_DMR;
	v[3].dmr_id = 9996543;
	v[3].dmo = true;
	v[3].rx_only = true;
	v[3].no_beep = true;	// the channel editor's flags (#321: the encoder kept the bits as read)
	v[3].no_eco = true;
	v[3].vox = true;
	v[3].scan_skip = true;
	v[4] = make("", 136000000, 174000000);
	v[5] = make("Lo", 400012500, 479987500);
	for (unsigned i = 0; i < 6; i++) {
		uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
		channel_t back;
		memset(r, 0xFF, sizeof(r));
		codeplug_openfw_encode(&v[i], r);
		CHECK(codeplug_openfw_decode(r, &back));
		CHECK(same_channel(&v[i], &back));
	}
	// Known encodings (open-firmware-cps.md 7.3.3)
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0xFF, sizeof(r));
	channel_t c = make("x", 438500000, 438500000);
	c.rx_tone = (subtone_t){ TONE_DCS_INV, 023 };
	c.tx_tone = (subtone_t){ TONE_CTCSS, 885 };
	codeplug_openfw_encode(&c, r);
	CHECK(r[0x10] == 0x00 && r[0x11] == 0x00 && r[0x12] == 0x85 && r[0x13] == 0x43);
	CHECK(r[0x20] == 0x23 && r[0x21] == 0xC0 && r[0x22] == 0x85 && r[0x23] == 0x08);
	CHECK(r[0x19] == 0 && r[0x27] == 0x00 && r[0x28] == 0x16 && r[0x29] == 0x00);
	CHECK(r[1] == 0xFF && r[0] == 'x');

	// Bytes the model doesn't own are kept: TOT, location, SFR bytes, vendor flags, VFO step, squelch
	uint8_t old[CODEPLUG_OPENFW_CHANNEL_SIZE];
	memset(r, 0x00, sizeof(r));
	codeplug_openfw_encode(&v[2], r);
	r[0x1A] = 0x11; r[0x1B] = 0x08; r[0x1C] = 0x22; r[0x24] = 0x33; r[0x25] = 0x0B; r[0x2A] = 0x85;
	r[0x2D] = 2; r[0x30] = 0x05; r[0x31] |= 0x01; r[0x32] = 0xC0; r[0x33] |= 0x40 | 0x20; r[0x26] |= 0x42;
	r[0x34] = 0x12; r[0x35] = 0x34; r[0x36] = 0x50; r[0x37] = 12;
	memcpy(old, r, sizeof(r));
	channel_t d;
	CHECK(codeplug_openfw_decode(r, &d));
	codeplug_openfw_encode(&d, r);
	CHECK(memcmp(old, r, sizeof(r)) == 0);	// decode then encode: byte for byte the same
	d.timeslot = 1;
	d.bw = RADIO_BW_25K;
	codeplug_openfw_encode(&d, r);
	CHECK(r[0x31] == ((old[0x31] & ~0x40) | 0x00) && (r[0x33] & 0x02) && (r[0x33] & 0x60) == 0x60 && r[0x26] == old[0x26]);
	CHECK(r[0x1B] == 0x08 && r[0x25] == 0x0B && r[0x2A] == 0x85 && r[0x37] == 12 && r[0x36] == 0x50);
	// The editor's flags reach the record: VOX and zone scan skip (0x33 bits 6, 5), no beep (0x26 bit 6), no eco
	// (bit 5). The SFR bit (0x26 bit 1) is still kept
	d.vox = d.scan_skip = d.no_beep = false;
	d.no_eco = true;
	codeplug_openfw_encode(&d, r);
	CHECK((r[0x33] & 0x60) == 0 && (r[0x26] & 0x62) == 0x22);
	CHECK(codeplug_openfw_decode(r, &d) && !d.vox && !d.scan_skip && !d.no_beep && d.no_eco);
}

static void test_materialize(void)
{
	fresh();
	CHECK(codeplug_source() == CODEPLUG_BUILTIN && codeplug_channel_count() == 6);
	uint32_t s0 = codeplug_serial();
	channel_t c = make("Added", 145550000, 145550000), back;
	CHECK(codeplug_free_channel() == 7);
	CHECK(codeplug_set_channel(10, &c));
	CHECK(codeplug_serial() != s0);
	CHECK(codeplug_source() == CODEPLUG_OPENFW && codeplug_channel_count() == 10);
	CHECK(codeplug_channel(2, &back) && strcmp(back.name, "Simplex 146.520") == 0);
	CHECK(codeplug_channel(10, &back) && same_channel(&c, &back));
	CHECK(!codeplug_channel(7, &back));
	CHECK(codeplug_free_channel() == 7);
	// The same after a reboot
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_channel_count() == 10 && codeplug_channel(10, &back) && same_channel(&c, &back));
	CHECK(codeplug_zone_count() == 1 && !codeplug_zones_stored());
	// Delete the top one: the count follows
	CHECK(codeplug_delete_channel(10));
	CHECK(codeplug_channel_count() == 6 && !codeplug_channel(10, &back));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel_count() == 6);
	// Channel 129 and 1024 (flash image banks)
	c = make("Bank 1", 433000000, 433000000);
	CHECK(codeplug_set_channel(129, &c) && codeplug_set_channel(1024, &c));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel_count() == 1024);
	CHECK(codeplug_channel(129, &back) && same_channel(&c, &back));
	CHECK(sim_flash_data[FL + 0x7B1B0] == 0x01);	// bank 1 bitmap: its channel 0
	// Refused
	c.rx_hz = 0;
	CHECK(!codeplug_set_channel(5, &c));
	CHECK(!codeplug_set_channel(0, &c) && !codeplug_set_channel(1025, &c));
	CHECK(sim_flash_violations() == 0);
}

// A read that fails at the index (and works again after it) is no free slot (codeplug-native.md 2). Bank 0: in-use
// bitmap EE+0x3780, records from EE+0x3790; bank 1: bitmap FL+0x7B1B0. Records are 56 bytes
static void test_unread(void)
{
	channel_t c = make("Kept", 433000000, 433000000), back;
	fresh();
	CHECK(codeplug_set_channel(2, &c) && codeplug_set_channel(129, &c));	// with the built-in 1-6
	// Record 2 unreadable: hidden, its bank unknown. No slot there is offered or taken and nothing deleted, a channel
	// shown is still saved, and no bitmap is written: it is back at the next index
	sim_flash_bad_read(EE + 0x3790u + 56u, 56u);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	sim_flash_bad_read(0, 0);
	CHECK(!codeplug_channel(2, &back));
	CHECK(codeplug_free_channel() == 130);
	c = make("Other", 433100000, 433100000);
	CHECK(!codeplug_set_channel(7, &c) && !codeplug_delete_channel(3));
	CHECK(codeplug_set_channel(3, &c));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(2, &back) && strcmp(back.name, "Kept") == 0);
	CHECK(codeplug_channel(3, &back) && strcmp(back.name, "Other") == 0);
	// Bank 1's bitmap unreadable: its channels unknown, nothing written there
	uint8_t bits[16];
	memcpy(bits, &sim_flash_data[FL + 0x7B1B0u], sizeof bits);
	sim_flash_bad_read(FL + 0x7B1B0u, 16u);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	sim_flash_bad_read(0, 0);
	CHECK(!codeplug_set_channel(130, &c));
	CHECK(!codeplug_delete_channel(129));
	CHECK(memcmp(bits, &sim_flash_data[FL + 0x7B1B0u], sizeof bits) == 0);
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(129, &back) && strcmp(back.name, "Kept") == 0);
	// A zone read without bank 1's channels isn't written back without them
	zone_t z, zb;
	memset(&z, 0, sizeof z);
	strcpy(z.name, "Both");
	z.count = 2;
	z.channels[0] = 2;
	z.channels[1] = 129;
	sim_flash_bad_read(FL + 0x7B1B0u, 16u);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	sim_flash_bad_read(0, 0);
	CHECK(codeplug_zone(0, &zb) && zb.count >= 2);		// the virtual zone, read without 129
	CHECK(!codeplug_set_zone(0, &zb));			// storing it would drop 129
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_set_zone(0, &z));
	sim_flash_bad_read(FL + 0x7B1B0u, 16u);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	sim_flash_bad_read(0, 0);
	CHECK(codeplug_zone(0, &zb) && zb.count == 1);
	CHECK(!codeplug_set_zone(0, &zb));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_zone(0, &zb) && zb.count == 2 && zb.channels[1] == 129);
	CHECK(codeplug_delete_zone(0));
	// The only bank in use unreadable looks blank: the first edit doesn't materialize over it
	for (uint16_t n = 1; n <= 7; n++) {
		CHECK(codeplug_delete_channel(n));
	}
	sim_flash_bad_read(FL + 0x7B1B0u, 16u);
	CHECK(codeplug_init() == CODEPLUG_BUILTIN);
	sim_flash_bad_read(0, 0);
	CHECK(!codeplug_set_channel(1, &c));
	CHECK(codeplug_init() == CODEPLUG_OPENFW && codeplug_channel(129, &back) && strcmp(back.name, "Kept") == 0);
	CHECK(sim_flash_violations() == 0);
	// Blank flash (every bitmap bit set, every record erased): a record that can't be read makes no codeplug of it
	fresh();
	sim_flash_bad_read(EE + 0x3790u, 56u);
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_channel_count() == 6);
	sim_flash_bad_read(0, 0);
	CHECK(!codeplug_set_channel(10, &c));
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_set_channel(10, &c));
	CHECK(sim_flash_violations() == 0);
}

static void test_zones(void)
{
	zone_t z, back;
	fresh();
	channel_t c = make("A", 145500000, 145500000);
	CHECK(codeplug_set_channel(20, &c));
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Local");
	z.count = 3;
	z.channels[0] = 20;
	z.channels[1] = 1;
	z.channels[2] = 3;
	CHECK(codeplug_set_zone(0, &z));
	CHECK(codeplug_zones_stored() && codeplug_zone_count() == 1);
	CHECK(sim_flash_data[EE + 0x806F] == 0x00);	// the 80-channel layout marker
	CHECK(codeplug_zone(0, &back) && strcmp(back.name, "Local") == 0 && back.count == 3 && back.channels[0] == 20);
	strcpy(z.name, "Second");
	z.count = 80;
	for (unsigned i = 0; i < 80; i++) {
		z.channels[i] = (uint16_t)(1 + i % 6);
	}
	CHECK(!codeplug_set_zone(5, &z));		// not the next index
	CHECK(codeplug_set_zone(1, &z));
	CHECK(codeplug_zone_count() == 2 && codeplug_zone(1, &back) && back.count == 80);
	z.channels[3] = 99;				// not stored
	CHECK(!codeplug_set_zone(1, &z));
	z.channels[3] = 20;
	strcpy(z.name, "Renamed");
	z.count = 2;
	CHECK(codeplug_set_zone(0, &z));
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	CHECK(codeplug_zone_count() == 2 && codeplug_zone(0, &back) && strcmp(back.name, "Renamed") == 0 && back.count == 2);
	CHECK(codeplug_delete_zone(0));
	CHECK(codeplug_zone_count() == 1 && codeplug_zone(0, &back) && strcmp(back.name, "Second") == 0);
	// A new zone takes the free slot 0 again
	strcpy(z.name, "Third");
	CHECK(codeplug_set_zone(1, &z));
	CHECK(sim_flash_data[EE + 0x8010] == 0x03);
	CHECK(codeplug_delete_zone(0) && codeplug_delete_zone(0));
	CHECK(codeplug_zone_count() == 1 && !codeplug_zones_stored());
	CHECK(sim_flash_violations() == 0);
}

#if CONFIG_DMR
static void test_dmr(void)
{
	contact_t c, back;
	listen_list_t l, lb;
	char name[CODEPLUG_NAME_LEN + 1];

	fresh();
	memset(&c, 0, sizeof(c));
	strcpy(c.name, "TG 91");
	c.id = 91;
	c.type = CONTACT_GROUP;
	CHECK(codeplug_set_contact(1, &c));
	strcpy(c.name, "Joe");
	c.id = 9991234;
	c.type = CONTACT_PRIVATE;
	c.timeslot = 2;
	CHECK(codeplug_set_contact(1024, &c));
	strcpy(c.name, "All");
	c.type = CONTACT_ALL_CALL;
	c.timeslot = 1;
	c.id = 0;
	CHECK(codeplug_set_contact(3, &c));
	CHECK(codeplug_contact(1, &back) && back.id == 91 && back.type == CONTACT_GROUP && back.timeslot == 0);
	CHECK(codeplug_contact(1024, &back) && back.id == 9991234 && back.timeslot == 2 && strcmp(back.name, "Joe") == 0);
	CHECK(codeplug_contact(3, &back) && back.id == 0xFFFFFF && back.timeslot == 1);
	CHECK(codeplug_delete_contact(3) && !codeplug_contact(3, &back));
	c.id = 0;
	c.type = CONTACT_GROUP;
	CHECK(!codeplug_set_contact(4, &c));

	memset(&l, 0, sizeof(l));
	strcpy(l.name, "Local");
	l.count = 2;
	l.contacts[0] = 1;
	l.contacts[1] = 1024;
	CHECK(codeplug_set_listen_list(76, &l));
	CHECK(codeplug_listen_list(76, &lb) && lb.count == 2 && lb.contacts[1] == 1024 && strcmp(lb.name, "Local") == 0);
	l.count = 32;
	for (unsigned i = 0; i < 32; i++) {
		l.contacts[i] = (uint16_t)(i + 1);
	}
	CHECK(codeplug_set_listen_list(1, &l) && codeplug_listen_list(1, &lb) && lb.count == 32);
	CHECK(codeplug_delete_listen_list(1) && !codeplug_listen_list(1, &lb));
	CHECK(!codeplug_listen_list(2, &lb));		// cleared by the materialize

	CHECK(codeplug_set_dmr_id(9996543) && codeplug_dmr_id() == 9996543);
	CHECK(sim_flash_data[EE + 0xE8] == 0x09 && sim_flash_data[EE + 0xEB] == 0x43);
	CHECK(codeplug_set_dmr_id(0) && codeplug_dmr_id() == 0);
	CHECK(codeplug_set_radio_name("Q0TEST-LONGER"));
	codeplug_radio_name(name);
	CHECK(strcmp(name, "Q0TEST-L") == 0);
	char l1[CODEPLUG_NAME_LEN + 1], l2[CODEPLUG_NAME_LEN + 1];
	CHECK(codeplug_set_boot_text("Charlie Harper", "Q0TEST and more text"));
	codeplug_boot_text(l1, l2);
	CHECK(strcmp(l1, "Charlie Harper") == 0 && strcmp(l2, "Q0TEST and more ") == 0);
	CHECK(codeplug_set_boot_text("", "Line two"));
	codeplug_boot_text(l1, l2);
	CHECK(l1[0] == 0 && strcmp(l2, "Line two") == 0);
	CHECK(sim_flash_violations() == 0);
}
#endif

// A fully populated codeplug: read every object, write each one back into the same image (no byte may change, no
// sector rewritten), then into a fresh flash, and read that back
typedef struct {
	channel_t ch[CODEPLUG_CHANNELS_MAX + 1];
	bool ch_used[CODEPLUG_CHANNELS_MAX + 1];
	zone_t zone[CODEPLUG_ZONES_MAX];
	uint16_t zones;
	contact_t ct[CODEPLUG_CONTACTS_MAX + 1];
	bool ct_used[CODEPLUG_CONTACTS_MAX + 1];
	listen_list_t ll[CODEPLUG_LISTS_MAX + 1];
	bool ll_used[CODEPLUG_LISTS_MAX + 1];
	uint32_t dmr_id;
} model_t;

static void read_model(model_t *m)
{
	memset(m, 0, sizeof(*m));
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++) {
		m->ch_used[n] = codeplug_channel(n, &m->ch[n]);
	}
	m->zones = codeplug_zones_stored() ? codeplug_zone_count() : 0;
	for (uint16_t i = 0; i < m->zones; i++) {
		CHECK(codeplug_zone(i, &m->zone[i]));
	}
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		m->ct_used[i] = codeplug_contact(i, &m->ct[i]);
	}
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
		m->ll_used[i] = codeplug_listen_list(i, &m->ll[i]);
	}
	m->dmr_id = codeplug_dmr_id();
}

static void write_model(const model_t *m)
{
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++) {
		if (m->ch_used[n]) {
			CHECK(codeplug_set_channel(n, &m->ch[n]));
		}
	}
	for (uint16_t i = 0; i < m->zones; i++) {
		CHECK(codeplug_set_zone(i, &m->zone[i]));
	}
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (m->ct_used[i]) {
			CHECK(codeplug_set_contact(i, &m->ct[i]));
		}
	}
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i++) {
		if (m->ll_used[i]) {
			CHECK(codeplug_set_listen_list(i, &m->ll[i]));
		}
	}
	CHECK(codeplug_set_dmr_id(m->dmr_id));
}

static bool same_model(const model_t *a, const model_t *b)
{
	bool ok = true;
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++) {
		if (a->ch_used[n] != b->ch_used[n] || (a->ch_used[n] && !same_channel(&a->ch[n], &b->ch[n]))) {
			printf("  channel %u differs\n", n);
			ok = false;
		}
	}
	ok = ok && a->zones == b->zones;
	for (uint16_t i = 0; ok && i < a->zones; i++) {
		ok = strcmp(a->zone[i].name, b->zone[i].name) == 0 && a->zone[i].count == b->zone[i].count &&
		     memcmp(a->zone[i].channels, b->zone[i].channels, a->zone[i].count * 2u) == 0;
	}
	for (uint16_t i = 1; ok && i <= CODEPLUG_CONTACTS_MAX; i++) {
		ok = a->ct_used[i] == b->ct_used[i] && (!a->ct_used[i] || (strcmp(a->ct[i].name, b->ct[i].name) == 0 &&
		     a->ct[i].id == b->ct[i].id && a->ct[i].type == b->ct[i].type && a->ct[i].timeslot == b->ct[i].timeslot));
	}
	for (uint16_t i = 1; ok && i <= CODEPLUG_LISTS_MAX; i++) {
		ok = a->ll_used[i] == b->ll_used[i] && (!a->ll_used[i] || (strcmp(a->ll[i].name, b->ll[i].name) == 0 &&
		     a->ll[i].count == b->ll[i].count && memcmp(a->ll[i].contacts, b->ll[i].contacts, a->ll[i].count * 2u) == 0));
	}
	return ok && a->dmr_id == b->dmr_id;
}

static void populate(void)
{
	char name[17];
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n += (n < 140 ? 1 : 37)) {
		snprintf(name, sizeof(name), "Ch %u", n);
		channel_t c = make(name, 430000000u + n * 12500u, 438000000u + n * 12500u);
		c.mode = n % 3 == 0 ? RADIO_MODE_DMR : RADIO_MODE_FM;
		c.bw = n % 2 ? RADIO_BW_25K : RADIO_BW_12K5;
		c.tx_tone = n % 5 == 0 ? (subtone_t){ TONE_CTCSS, 1000 + n } : (subtone_t){ TONE_NONE, 0 };
		c.rx_tone = n % 7 == 0 ? (subtone_t){ TONE_DCS, n % 0777 } : (subtone_t){ TONE_NONE, 0 };
		c.color_code = n % 16;
		c.timeslot = (uint8_t)(1 + n % 2);
		c.contact = (uint16_t)(n % 50);
		c.listen_list = (uint16_t)(n % 77);
		c.power = n % 4 == 0 ? CODEPLUG_POWER_DEFAULT : (uint8_t)(n % 9);
		CHECK(codeplug_set_channel(n, &c));
	}
	for (uint16_t i = 0; i < 20; i++) {
		zone_t z;
		memset(&z, 0, sizeof(z));
		snprintf(z.name, sizeof(z.name), "Zone %u", i);
		z.count = (uint16_t)(1 + i * 4);
		for (uint16_t k = 0; k < z.count; k++) {
			z.channels[k] = (uint16_t)(1 + (k + i) % 130);
		}
		CHECK(codeplug_set_zone(i, &z));
	}
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i += 3) {
		contact_t c;
		memset(&c, 0, sizeof(c));
		snprintf(c.name, sizeof(c.name), "Contact %u", i);
		c.id = 9990000u + i;
		c.type = i % 2 ? CONTACT_GROUP : CONTACT_PRIVATE;
		c.timeslot = (uint8_t)(i % 3);
		CHECK(codeplug_set_contact(i, &c));
	}
	for (uint16_t i = 1; i <= CODEPLUG_LISTS_MAX; i += 2) {
		listen_list_t l;
		memset(&l, 0, sizeof(l));
		snprintf(l.name, sizeof(l.name), "List %u", i);
		l.count = (uint8_t)(i % 33);
		for (uint8_t k = 0; k < l.count; k++) {
			l.contacts[k] = (uint16_t)(1 + 3 * k);
		}
		CHECK(codeplug_set_listen_list(i, &l));
	}
	CHECK(codeplug_set_dmr_id(9997030));
}

static void test_round_trip(void)
{
	static model_t a, b;
	storage_stats_t st0, st1;

	fresh();
	populate();
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	read_model(&a);

	// Write every object back into the same image: nothing changes
	uint8_t *before = malloc(FLASH_SIZE);
	memcpy(before, sim_flash_data, FLASH_SIZE);
	storage_get_stats(&st0);
	write_model(&a);
	storage_get_stats(&st1);
	CHECK(memcmp(before, sim_flash_data, FLASH_SIZE) == 0);
	CHECK(st1.sector_writes == st0.sector_writes);
	free(before);

	// Into a fresh flash, then read back
	sim_flash_create(FLASH_SIZE, &map);
	storage_init();
	codeplug_init();
	write_model(&a);
	CHECK(codeplug_init() == CODEPLUG_OPENFW);
	read_model(&b);
	// The fresh codeplug also holds the built-in channels the first edit stored, where a has none
	for (uint16_t n = 1; n <= 6; n++) {
		if (!a.ch_used[n]) {
			b.ch_used[n] = false;
		}
	}
	CHECK(same_model(&a, &b));
	printf("round trip: channels up to %u, %u zones written back unchanged and into a fresh flash\n",
	       codeplug_channel_count(), a.zones);
}

// Power cut at every unit of a channel edit: after the boot the channel reads as before or as after
static void edit_cuts(uint16_t ch)
{
	channel_t old = make("Old", 145500000, 145500000), neu = make("New", 438500000, 430900000), back;
	neu.mode = RADIO_MODE_DMR;
	neu.color_code = 7;		// fields in the record's second half differ too
	neu.timeslot = 2;
	neu.contact = 3;
	neu.rx_only = true;
	fresh();
	CHECK(codeplug_set_channel(ch, &old));
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	uint32_t u0 = sim_flash_units();
	CHECK(codeplug_set_channel(ch, &neu));
	uint32_t total = sim_flash_units() - u0;
	int o = 0, n = 0;
	for (uint32_t cut = 1; cut <= total; cut += cut < 300 ? 1 : 61) {
		memcpy(sim_flash_data, snap, FLASH_SIZE);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		sim_flash_cut_after(cut);
		codeplug_set_channel(ch, &neu);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		bool ok = codeplug_channel(ch, &back);
		if (ok && same_channel(&back, &neu)) {
			n++;
		} else if (ok && same_channel(&back, &old)) {
			o++;
		} else {
			printf("FAIL edit of channel %u: cut after %u of %u units\n", ch, cut, total);
			failures++;
		}
		CHECK(codeplug_channel(1, &back) && strcmp(back.name, "Simplex 145.500") == 0);
	}
	CHECK(o > 0 && n > 0);
	free(snap);
}

static void test_edit_cuts(void)
{
	edit_cuts(100);
	edit_cuts(39);		// its record crosses a sector end (0x3FE0..0x4017)
}

#if CONFIG_APRS
// The APRS path written back into a record a CPS made (aprs.md 11): the callsigns space padded, the SSID after each,
// the rest of the record kept; no record is made where there is none
static void test_aprs_path(void)
{
	codeplug_aprs_t a;
	fresh();
	CHECK(codeplug_set_radio_name("Q0TEST"));	// a stored codeplug
	uint8_t r[0x40];
	memset(r, 0, sizeof(r));
	memcpy(r, "APRS1", 5);
	r[0x08] = 9;
	memcpy(&r[0x0F], "WIDE1 \x01WIDE2 \x01", 14);
	memset(&r[0x1F], 0xFF, 24);
	memcpy(&r[0x1F], "Mobile", 6);
	r[0x3E] = 0x53;
	r[0x3F] = 0x41;
	CHECK(storage_write(EE + 0x1588u, r, sizeof(r)));
	codeplug_reload();
	uint32_t ser = codeplug_serial();
	CHECK(codeplug_set_aprs_path(1, "WIDE2-2", ""));
	CHECK(codeplug_serial() == ser + 1u);
	CHECK(memcmp(&sim_flash_data[EE + 0x1588u + 0x0F], "WIDE2 \x02      \x00", 14) == 0);
	CHECK(codeplug_aprs(1, &a) && strcmp(a.path[0], "WIDE2-2") == 0 && a.path[1][0] == '\0' && a.ssid == 9 &&
	      strcmp(a.comment, "Mobile") == 0);
	CHECK(codeplug_set_aprs_path(1, "WIDE1-1", "WIDE2-1") && codeplug_aprs(1, &a) &&
	      strcmp(a.path[0], "WIDE1-1") == 0 && strcmp(a.path[1], "WIDE2-1") == 0);
	CHECK(!codeplug_set_aprs_path(2, "WIDE1-1", "") && !codeplug_set_aprs_path(0, "", ""));	// no record: none made
	CHECK(sim_flash_data[EE + 0x1588u + 0x40u] == 0xFF);
}
#endif

// The quick-key slots (EEPROM 0x7524, 10 x 2 bytes LE), where the side key assignment lives
// (core/input/keyassign.h): none without a stored codeplug; the first write stores the built-in set
static void test_quick_keys(void)
{
	uint16_t k[CODEPLUG_QUICK_KEYS], back[CODEPLUG_QUICK_KEYS];
	fresh();
	CHECK(!codeplug_quick_keys(back) && back[0] == 0 && back[9] == 0);
	for (unsigned i = 0; i < CODEPLUG_QUICK_KEYS; i++) {
		k[i] = (uint16_t)(0x4B01u + i * 0x0101u);
	}
	uint32_t ser = codeplug_serial();
	CHECK(codeplug_set_quick_keys(k));
	CHECK(codeplug_source() == CODEPLUG_OPENFW && codeplug_serial() != ser);
	CHECK(codeplug_quick_keys(back) && memcmp(back, k, sizeof(k)) == 0);
	CHECK(sim_flash_data[EE + 0x7524] == 0x01 && sim_flash_data[EE + 0x7525] == 0x4B);	// little endian
	CHECK(sim_flash_data[EE + 0x7523] == 0xFF && sim_flash_data[EE + 0x7538] == 0xFF);	// nothing around them
}

// Another firmware's codeplug where the open-firmware images go (codeplug-stock-tyt.md 2): pseudo-random bytes over
// both images, clear of the settings, the journal and the calibration. Its first 16 channels are stock-style: in use,
// valid BCD frequencies (438.5 MHz), UTF-16 names, which the radio reads as channels once the owner accepts them
static void foreign_image(void)
{
	uint32_t x = 99;
	for (uint32_t a = 0; a < 0xB0000u; a++) {
		x = x * 1103515245u + 12345u;
		if (a < 0x10000u || a >= FL) {
			sim_flash_data[a] = (uint8_t)(x >> 24);
		}
	}
	sim_flash_data[EE + 0x3780] = sim_flash_data[EE + 0x3781] = 0xFF;	// channels 1..16 in use
	for (unsigned n = 0; n < 16; n++) {
		uint8_t *r = &sim_flash_data[EE + 0x3790 + 56u * n];
		memset(r, 0, 56);
		memcpy(r, "C\0h\0a\0n\0", 8);
		static const uint8_t f[4] = { 0x00, 0x00, 0x85, 0x43 };	// 438.5 MHz: BCD in 10 Hz, little endian
		memcpy(r + 0x10, f, 4);
		memcpy(r + 0x14, f, 4);
		r[0x18] = 1;
	}
}

// Both open-firmware images as they were (the settings ring, the journal and the calibration aren't compared)
static bool images_same(const uint8_t *before)
{
	return memcmp(before, sim_flash_data, 0x10000u) == 0 && memcmp(before + FL, sim_flash_data + FL, 0x90000u) == 0;
}

static void test_foreign(void)
{
	channel_t c = make("Added", 145550000, 145550000), back;
	uint16_t good, bad;
	zone_t z;

	sim_flash_create(FLASH_SIZE, &map);
	foreign_image();
	uint8_t *before = malloc(FLASH_SIZE);
	memcpy(before, sim_flash_data, FLASH_SIZE);
	CHECK(storage_init());
	codeplug_accept(false);
	CHECK(codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign() && codeplug_channel_count() == 6);
	// Reading writes nothing, and no edit starts on its own (Cancel: nothing changes)
	CHECK(codeplug_channel(1, &back) && codeplug_reload() == CODEPLUG_BUILTIN);
	CHECK(!codeplug_set_channel(10, &c) && !codeplug_set_dmr_id(9996543) && !codeplug_set_radio_name("Q0TEST"));
	CHECK(!codeplug_set_zone(0, &(zone_t){ .name = "Zone", .count = 1, .channels = { 1 } }));
	CHECK(codeplug_source() == CODEPLUG_BUILTIN && codeplug_foreign() && images_same(before));

	// Use anyway: the verdict off, the old records shown, nothing erased; an edit writes only what it changes
	codeplug_accept(true);
	CHECK(codeplug_acceptance() == CODEPLUG_ACCEPT_YES);
	codeplug_reload();
	CHECK(!codeplug_foreign() && images_same(before));
	codeplug_evidence(&good, &bad);
	CHECK(bad > 0 && (uint32_t)bad * 4u > good);	// still counted: the view shows why it was asked
	CHECK(codeplug_source() == CODEPLUG_OPENFW && codeplug_channel_count() >= 16);
	CHECK(codeplug_channel(16, &back) && back.rx_hz == 438500000u);
	uint16_t n = codeplug_free_channel();
	CHECK(n > 16 && codeplug_set_channel(n, &c) && codeplug_channel(n, &back) && same_channel(&c, &back));
	CHECK(codeplug_channel(1, &back) && back.rx_hz == 438500000u);	// the old channels stay
	CHECK(memcmp(before + 0x7524u, sim_flash_data + 0x7524u, 0x7538u - 0x7524u) == 0);	// the quick keys
	CHECK(memcmp(before + FL + 0x87620u, sim_flash_data + FL + 0x87620u, 24u * 1024u) == 0);	// the contacts
	CHECK(memcmp(before + 0xE0u, sim_flash_data + 0xE0u, 0x1Au) == 0);	// the callsign, the DMR ID
	// A codeplug written by a CPS drops the acceptance: judged again
	memcpy(sim_flash_data, before, FLASH_SIZE);
	sim_flash_power_on();
	CHECK(storage_init());
	CHECK(codeplug_written() == CODEPLUG_BUILTIN && codeplug_foreign());
	CHECK(codeplug_acceptance() == CODEPLUG_ACCEPT_DROPPED);
	codeplug_accept(false);

	// Start new: what the radio reads of the old bytes cleared, the built-in set stored; then the edit. The settings
	// area goes too: keys from before the other firmware don't come back in the clean codeplug
	memset(&sim_flash_data[EE + 0xE000], 0x00, 64);
	sim_flash_data[EE + 0xEFFF] = 0x00;
	CHECK(codeplug_start_new() && codeplug_set_channel(10, &c));
	for (uint32_t i = 0; i < 0x1000; i++) {
		if (sim_flash_data[EE + 0xE000 + i] != 0xFF) {
			CHECK(!"the settings area erased by Start new");
			break;
		}
	}
	CHECK(codeplug_source() == CODEPLUG_OPENFW && !codeplug_foreign() && codeplug_channel_count() == 10);
	CHECK(codeplug_init() == CODEPLUG_OPENFW && !codeplug_foreign());
	codeplug_evidence(&good, &bad);
	CHECK(good == 7 && bad == 0);
	CHECK(codeplug_channel(10, &back) && same_channel(&c, &back) && !back.roam && back.sfr_flag1 == 0);
	CHECK(codeplug_zone_count() == 1 && !codeplug_zones_stored() && codeplug_zone(0, &z));
	char name[CODEPLUG_NAME_LEN + 1], l2[CODEPLUG_NAME_LEN + 1];
	codeplug_radio_name(name);
	CHECK(name[0] == '\0');
	codeplug_boot_text(name, l2);
	CHECK(name[0] == '\0' && l2[0] == '\0');
	uint16_t keys[CODEPLUG_QUICK_KEYS];
	CHECK(codeplug_quick_keys(keys) && keys[0] == 0 && keys[9] == 0);
	// Only what the radio reads is cleared: the general settings past the hang times and the bytes between the quick
	// keys and the boot text keep the old bytes
	CHECK(memcmp(&sim_flash_data[EE + 0xE0 + 0x1A], &before[EE + 0xE0 + 0x1A], 0x28 - 0x1A) == 0);
	CHECK(memcmp(&sim_flash_data[EE + 0x7538], &before[EE + 0x7538], 8) == 0);
#if CONFIG_DMR
	contact_t ct;
	listen_list_t ll;
	codeplug_dmr_settings_t ds;
	CHECK(codeplug_dmr_settings(&ds) && ds.group_hang_ms == 0 && ds.private_hang_ms == 0);	// not the old bytes
	CHECK(codeplug_dmr_id() == 0 && !codeplug_contact(1, &ct) && !codeplug_contact(1024, &ct));
	CHECK(!codeplug_listen_list(1, &ll) && !codeplug_listen_list(76, &ll));
#endif
	CHECK(codeplug_start_new());	// ours now: nothing to clear
	CHECK(sim_flash_violations() == 0);

	// A CPS write over the foreign bytes (here, the image of a fresh codeplug put underneath) flips it on reload
	uint8_t *ours = malloc(FLASH_SIZE);
	memcpy(ours, sim_flash_data, FLASH_SIZE);
	memcpy(sim_flash_data, before, FLASH_SIZE);
	sim_flash_power_on();
	CHECK(storage_init() && codeplug_init() == CODEPLUG_BUILTIN && codeplug_foreign());
	uint32_t ser = codeplug_serial();
	memcpy(sim_flash_data, ours, 0x10000u);
	memcpy(sim_flash_data + FL, ours + FL, 0x90000u);
	CHECK(codeplug_written() == CODEPLUG_OPENFW && !codeplug_foreign() && codeplug_serial() != ser);
	CHECK(codeplug_channel(10, &back) && same_channel(&c, &back));
	codeplug_accept(false);
	free(ours);
	free(before);
}

int main(void)
{
	test_encode();
	test_materialize();
	test_unread();
	test_zones();
#if CONFIG_DMR
	test_dmr();
#endif
#if CONFIG_APRS
	test_aprs_path();
#endif
	test_quick_keys();
	test_foreign();
	test_round_trip();
	test_edit_cuts();
	sim_flash_free();
	printf("# codeplugEditTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
