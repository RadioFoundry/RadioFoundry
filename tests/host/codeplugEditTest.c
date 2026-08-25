// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug editing on a simulated NOR flash with the DM-1701 map: the legacy record encoder against the decoder,
// bytes the model doesn't own kept, the built-in set turned into a stored codeplug by the first edit, zones,
// contacts and RX group lists, and the round trip read -> write -> read (model and image), with power cuts.
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
	.legacy_eeprom = EE, .legacy_flash = FL, .legacy_end = FLASH_SIZE,
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
	       a->contact == b->contact && a->listen_list == b->listen_list && a->dmr_id == b->dmr_id && a->dmo == b->dmo;
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
	v[3].dmr_id = 3101276;
	v[3].dmo = true;
	v[3].rx_only = true;
	v[4] = make("", 136000000, 174000000);
	v[5] = make("Lo", 400012500, 479987500);
	for (unsigned i = 0; i < 6; i++) {
		uint8_t r[CODEPLUG_LEGACY_CHANNEL_SIZE];
		channel_t back;
		memset(r, 0xFF, sizeof(r));
		codeplug_legacy_encode(&v[i], r);
		CHECK(codeplug_legacy_decode(r, &back));
		CHECK(same_channel(&v[i], &back));
	}
	// Known encodings (legacy-cps-interop.md 7.3.3)
	uint8_t r[CODEPLUG_LEGACY_CHANNEL_SIZE];
	memset(r, 0xFF, sizeof(r));
	channel_t c = make("x", 438500000, 438500000);
	c.rx_tone = (subtone_t){ TONE_DCS_INV, 023 };
	c.tx_tone = (subtone_t){ TONE_CTCSS, 885 };
	codeplug_legacy_encode(&c, r);
	CHECK(r[0x10] == 0x00 && r[0x11] == 0x00 && r[0x12] == 0x85 && r[0x13] == 0x43);
	CHECK(r[0x20] == 0x23 && r[0x21] == 0xC0 && r[0x22] == 0x85 && r[0x23] == 0x08);
	CHECK(r[0x19] == 0 && r[0x27] == 0x00 && r[0x28] == 0x16 && r[0x29] == 0x00);
	CHECK(r[1] == 0xFF && r[0] == 'x');

	// Bytes the model doesn't own are kept: TOT, location, SFR bytes, vendor flags, VFO step, squelch
	uint8_t old[CODEPLUG_LEGACY_CHANNEL_SIZE];
	memset(r, 0x00, sizeof(r));
	codeplug_legacy_encode(&v[2], r);
	r[0x1A] = 0x11; r[0x1B] = 0x08; r[0x1C] = 0x22; r[0x24] = 0x33; r[0x25] = 0x0B; r[0x2A] = 0x85;
	r[0x2D] = 2; r[0x30] = 0x05; r[0x31] |= 0x01; r[0x32] = 0xC0; r[0x33] |= 0x40 | 0x20; r[0x26] |= 0x42;
	r[0x34] = 0x12; r[0x35] = 0x34; r[0x36] = 0x50; r[0x37] = 12;
	memcpy(old, r, sizeof(r));
	channel_t d;
	CHECK(codeplug_legacy_decode(r, &d));
	codeplug_legacy_encode(&d, r);
	CHECK(memcmp(old, r, sizeof(r)) == 0);	// decode then encode: byte for byte the same
	d.timeslot = 1;
	d.bw = RADIO_BW_25K;
	codeplug_legacy_encode(&d, r);
	CHECK(r[0x31] == ((old[0x31] & ~0x40) | 0x00) && (r[0x33] & 0x02) && (r[0x33] & 0x60) == 0x60 && r[0x26] == old[0x26]);
	CHECK(r[0x1B] == 0x08 && r[0x25] == 0x0B && r[0x2A] == 0x85 && r[0x37] == 12 && r[0x36] == 0x50);
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
	CHECK(codeplug_source() == CODEPLUG_LEGACY && codeplug_channel_count() == 10);
	CHECK(codeplug_channel(2, &back) && strcmp(back.name, "Simplex 146.520") == 0);
	CHECK(codeplug_channel(10, &back) && same_channel(&c, &back));
	CHECK(!codeplug_channel(7, &back));
	CHECK(codeplug_free_channel() == 7);
	// The same after a reboot
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
	CHECK(codeplug_channel_count() == 10 && codeplug_channel(10, &back) && same_channel(&c, &back));
	CHECK(codeplug_zone_count() == 1 && !codeplug_zones_stored());
	// Delete the top one: the count follows
	CHECK(codeplug_delete_channel(10));
	CHECK(codeplug_channel_count() == 6 && !codeplug_channel(10, &back));
	CHECK(codeplug_init() == CODEPLUG_LEGACY && codeplug_channel_count() == 6);
	// Channel 129 and 1024 (flash image banks)
	c = make("Bank 1", 433000000, 433000000);
	CHECK(codeplug_set_channel(129, &c) && codeplug_set_channel(1024, &c));
	CHECK(codeplug_init() == CODEPLUG_LEGACY && codeplug_channel_count() == 1024);
	CHECK(codeplug_channel(129, &back) && same_channel(&c, &back));
	CHECK(sim_flash_data[FL + 0x7B1B0] == 0x01);	// bank 1 bitmap: its channel 0
	// Refused
	c.rx_hz = 0;
	CHECK(!codeplug_set_channel(5, &c));
	CHECK(!codeplug_set_channel(0, &c) && !codeplug_set_channel(1025, &c));
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
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
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
	c.id = 3101234;
	c.type = CONTACT_PRIVATE;
	c.timeslot = 2;
	CHECK(codeplug_set_contact(1024, &c));
	strcpy(c.name, "All");
	c.type = CONTACT_ALL_CALL;
	c.timeslot = 1;
	c.id = 0;
	CHECK(codeplug_set_contact(3, &c));
	CHECK(codeplug_contact(1, &back) && back.id == 91 && back.type == CONTACT_GROUP && back.timeslot == 0);
	CHECK(codeplug_contact(1024, &back) && back.id == 3101234 && back.timeslot == 2 && strcmp(back.name, "Joe") == 0);
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

	CHECK(codeplug_set_dmr_id(3101276) && codeplug_dmr_id() == 3101276);
	CHECK(sim_flash_data[EE + 0xE8] == 0x03 && sim_flash_data[EE + 0xEB] == 0x76);
	CHECK(codeplug_set_dmr_id(0) && codeplug_dmr_id() == 0);
	CHECK(codeplug_set_radio_name("N0CALL-LONGER"));
	codeplug_radio_name(name);
	CHECK(strcmp(name, "N0CALL-L") == 0);
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
		c.id = 1000000u + i;
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
	CHECK(codeplug_set_dmr_id(2345678));
}

static void test_round_trip(void)
{
	static model_t a, b;
	storage_stats_t st0, st1;

	fresh();
	populate();
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
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
	CHECK(codeplug_init() == CODEPLUG_LEGACY);
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
static void test_edit_cuts(void)
{
	channel_t old = make("Old", 145500000, 145500000), neu = make("New", 438500000, 430900000), back;
	neu.mode = RADIO_MODE_DMR;
	fresh();
	CHECK(codeplug_set_channel(100, &old));
	uint8_t *snap = malloc(FLASH_SIZE);
	memcpy(snap, sim_flash_data, FLASH_SIZE);
	uint32_t u0 = sim_flash_units();
	CHECK(codeplug_set_channel(100, &neu));
	uint32_t total = sim_flash_units() - u0;
	int o = 0, n = 0;
	for (uint32_t cut = 1; cut <= total; cut += cut < 300 ? 1 : 61) {
		memcpy(sim_flash_data, snap, FLASH_SIZE);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		sim_flash_cut_after(cut);
		codeplug_set_channel(100, &neu);
		sim_flash_power_on();
		storage_init();
		codeplug_init();
		bool ok = codeplug_channel(100, &back);
		if (ok && same_channel(&back, &neu)) {
			n++;
		} else if (ok && same_channel(&back, &old)) {
			o++;
		} else {
			printf("FAIL edit cut after %u of %u units\n", cut, total);
			failures++;
		}
		CHECK(codeplug_channel(1, &back) && strcmp(back.name, "Simplex 145.500") == 0);
	}
	CHECK(o > 0 && n > 0);
	free(snap);
}

int main(void)
{
	test_encode();
	test_materialize();
	test_zones();
#if CONFIG_DMR
	test_dmr();
#endif
	test_round_trip();
	test_edit_cuts();
	sim_flash_free();
	printf("# codeplugEditTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
