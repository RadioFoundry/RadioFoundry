// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone sharing's codeplug side (core/codeplug/zone_import.c) on a simulated flash: band limits, receive only, contacts
// matched by ID with the user's names kept and the record's duplicates folded, an update replacing the earlier import,
// name collisions, the virtual zone kept, the undo when the codeplug runs out of room, and the export's fit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_import.h"
#include "codeplug/zone_edit.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#if CONFIG_DMR_TRUNK
#include "codeplug/codeplug_kv.h"
#include "radio/radio_trunk.h"
#endif

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x200000u
#define EE		0x000000u
#define FL		0x020000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = EE, .openfw_flash = FL, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

// VHF and UHF; 300 MHz and 900 MHz are out of band
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 5, .flash = &sim_flash };

static settings_zs_import_t imports[SETTINGS_ZS_IMPORTS];
static uint8_t recbuf[ZS_RECORD_MAX];

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();	// the zone write layer's power-cut journal is in the settings
	codeplug_init();
	memset(imports, 0, sizeof(imports));
}

static channel_t make(const char *name, uint32_t rx, uint32_t tx, bool dmr)
{
	channel_t c;

	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = rx;
	c.tx_hz = tx;
	c.mode = dmr ? RADIO_MODE_DMR : RADIO_MODE_FM;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}

static contact_t contact(const char *name, uint32_t id, uint8_t type)
{
	contact_t c;

	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.id = id;
	c.type = type;
	return c;
}

// A codeplug with zones: zone 0 "Home" holding channel 1
static void with_zones(void)
{
	channel_t ch = make("Home", 146520000, 146520000, false);
	zone_t z;

	CHECK(codeplug_set_channel(1, &ch));
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Home");
	z.count = 1;
	z.channels[0] = 1;
	CHECK(codeplug_set_zone(0, &z));
	CHECK(codeplug_zones_stored());
}

static int used_channels(void)
{
	channel_t ch;
	int n = 0;

	for (uint16_t i = 1; i <= CODEPLUG_CHANNELS_MAX; i++) {
		n += codeplug_channel(i, &ch);
	}
	return n;
}

static int used_contacts(void)
{
	contact_t c;
	int n = 0;

	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		n += codeplug_contact(i, &c);
	}
	return n;
}

static zsChannel_t zch(const char *name, uint32_t rx, int32_t off, bool dmr, uint8_t contactRef)
{
	zsChannel_t z;

	memset(&z, 0, sizeof(z));
	strncpy(z.name, name, ZS_NAME_MAX);
	z.rxHz = rx;
	z.txOffsetHz = off;
	z.dmr = dmr;
	z.colorCode = 3;
	z.timeslot = 2;
	z.contact = contactRef;
	z.power = ZS_POWER_HIGH;
	return z;
}

static zsContact_t zct(const char *name, uint32_t id, uint8_t type)
{
	zsContact_t c;

	memset(&c, 0, sizeof(c));
	strncpy(c.name, name, ZS_NAME_MAX);
	c.id = id;
	c.type = type;
	return c;
}

// Contacts: 1 TG 91 "World", 2 TG 91 again "Dup", 3 private 9997001 "Bob" (only an out-of-band channel uses it),
// 4 TG 3100 "USA". Channels: FM with tones and a repeater offset; DMR on 1; DMR on 2; 900 MHz DMR on 3; DMR on 4;
// 300 MHz FM
static int build_record(uint32_t zoneId, const char *name, uint8_t *out)
{
	zsWriter_t w;
	zsContact_t c;
	zsChannel_t z;

	zsWriterBegin(&w, out, ZS_RECORD_MAX, zoneId, 0, name);
	c = zct("World", 91, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	c = zct("Dup", 91, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	c = zct("Bob", 9997001, ZS_CONTACT_PRIVATE);
	zsWriterContact(&w, &c);
	c = zct("USA", 3100, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	z = zch("Rpt FM", 146940000, -600000, false, 0);
	z.rxToneType = ZS_TONE_CTCSS;
	z.rxTone = 1000;
	z.txToneType = ZS_TONE_DCS;
	z.txTone = 023;
	z.wide = true;
	z.rxOnly = false;
	zsWriterChannel(&w, &z);
	z = zch("DMR World", 441000000, 5000000, true, 1);
	zsWriterChannel(&w, &z);
	z = zch("DMR Dup", 442000000, 5000000, true, 2);
	zsWriterChannel(&w, &z);
	z = zch("Far", 902000000, 0, true, 3);
	zsWriterChannel(&w, &z);
	z = zch("DMR USA", 443000000, 5000000, true, 4);
	zsWriterChannel(&w, &z);
	z = zch("Low", 300000000, 0, false, 0);
	zsWriterChannel(&w, &z);
	int n = zsWriterEnd(&w);

	CHECK(n > 0);
	return n;
}

static void test_import(void)
{
	zone_import_result_t r;
	zone_t z;
	channel_t ch;
	contact_t c;

	fresh();
	with_zones();
	contact_t mine = contact("Worldwide", 91, CONTACT_GROUP);
	CHECK(codeplug_set_contact(5, &mine));
	int len = build_record(0x11223344, "Club", recbuf);

	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK);
	CHECK(r.channels == 4 && r.out_of_band == 2 && !r.updated);
	CHECK(r.zone == 1 && strcmp(r.name, "Club") == 0);
	// TG 91 (twice in the record) is the user's contact; USA is new; Bob, only used out of band, isn't added
	CHECK(r.contacts_added == 1);
	CHECK(used_contacts() == 2);
	CHECK(codeplug_contact(5, &c) && strcmp(c.name, "Worldwide") == 0);
	CHECK(codeplug_zone_count() == 2 && codeplug_zone(1, &z) && z.count == 4 && strcmp(z.name, "Club") == 0);
	uint16_t usa = 0;

	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (codeplug_contact(i, &c) && c.id == 3100) {
			usa = i;
			CHECK(strcmp(c.name, "USA") == 0 && c.type == CONTACT_GROUP);
		}
	}
	CHECK(usa != 0);
	// Every channel receive only; values carried
	for (uint16_t k = 0; k < z.count; k++) {
		CHECK(codeplug_channel(z.channels[k], &ch) && ch.rx_only);
		CHECK(board_in_band(ch.rx_hz, false));
	}
	CHECK(codeplug_channel(z.channels[0], &ch));
	CHECK(strcmp(ch.name, "Rpt FM") == 0 && ch.rx_hz == 146940000 && ch.tx_hz == 146340000);
	CHECK(ch.mode == RADIO_MODE_FM && ch.bw == RADIO_BW_25K && ch.power == 4);
	CHECK(ch.rx_tone.type == TONE_CTCSS && ch.rx_tone.value == 1000 && ch.tx_tone.type == TONE_DCS && ch.tx_tone.value == 023);
	CHECK(codeplug_channel(z.channels[1], &ch));
	CHECK(ch.mode == RADIO_MODE_DMR && ch.color_code == 3 && ch.timeslot == 2 && ch.contact == 5 && ch.tx_hz == 446000000);
	CHECK(codeplug_channel(z.channels[2], &ch) && ch.contact == 5);// the duplicate folded into the same contact
	CHECK(codeplug_channel(z.channels[3], &ch) && ch.contact == usa);
	CHECK(imports[0].key == zone_import_key(9996543, 0x11223344) && memcmp(imports[0].zone, "Club", 5) == 0);
	// Home untouched
	CHECK(codeplug_zone(0, &z) && z.count == 1 && z.channels[0] == 1 && strcmp(z.name, "Home") == 0);
}

// A contact block that can't be read fails the import, with nothing written: the match it hid would be added twice
static void test_contacts_unread(void)
{
	zone_import_result_t r;

	fresh();
	with_zones();
	contact_t mine = contact("Worldwide", 91, CONTACT_GROUP);
	CHECK(codeplug_set_contact(300, &mine));
	int len = build_record(0x11223344, "Club", recbuf);
	int channels = used_channels();

	// Contact 300: a later block of the scan, and another flash sector than the free slots the import writes
	sim_flash_bad_read(FL + 0x87620u + 24u * 299u, 24u);
	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_FLASH);
	sim_flash_bad_read(0, 0);
	CHECK(codeplug_zone_count() == 1 && used_channels() == channels && used_contacts() == 1);
	CHECK(imports[0].key == 0);
	// Readable again: TG 91 is the user's contact 300, only USA is added
	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_OK);
	CHECK(r.contacts_added == 1 && used_contacts() == 2);
}

static void test_update(void)
{
	zone_import_result_t r;
	zone_t z, before;
	zsWriter_t w;
	zsChannel_t zc;

	fresh();
	with_zones();
	int len = build_record(0x11223344, "Club", recbuf);

	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK);
	CHECK(codeplug_zone(1, &before) && before.count == 4);
	int channels = used_channels(), contacts = used_contacts();

	// The user also put one of the imported channels in Home: an update keeps it
	CHECK(codeplug_zone(0, &z));
	z.channels[z.count++] = before.channels[3];
	CHECK(codeplug_set_zone(0, &z));
	// and added a channel of their own to the import's zone, at the end: the update keeps it, after the new ones
	channel_t ch = make("Mine", 146550000, 146550000, false);
	uint16_t mine = codeplug_free_channel();

	ch.rx_only = true;// shaped like an imported one: still the user's
	CHECK(mine != 0 && codeplug_set_channel(mine, &ch));
	CHECK(codeplug_zone(1, &z));
	z.channels[z.count++] = mine;
	CHECK(codeplug_set_zone(1, &z));
	channels = used_channels();
	// Another zone in between, so the import's zone isn't the last
	zone_t other;

	memset(&other, 0, sizeof(other));
	strcpy(other.name, "Other");
	other.count = 1;
	other.channels[0] = 1;
	CHECK(codeplug_set_zone(2, &other));
	// The same zone from the same sender, now two channels
	zsWriterBegin(&w, recbuf, sizeof(recbuf), 0x11223344, 0, "Club v2");
	zc = zch("New A", 145500000, 0, false, 0);
	zsWriterChannel(&w, &zc);
	zc = zch("New B", 435000000, 0, false, 0);
	zsWriterChannel(&w, &zc);
	len = zsWriterEnd(&w);
	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK);
	CHECK(r.updated && r.zone == 1 && r.channels == 2);
	CHECK(strcmp(r.name, "Club") == 0);// the name it got here stays
	CHECK(codeplug_zone_count() == 3);
	CHECK(codeplug_zone(1, &z) && z.count == 3 && strcmp(z.name, "Club") == 0);
	CHECK(z.channels[2] == mine && codeplug_channel(mine, &ch) && strcmp(ch.name, "Mine") == 0);
	// The import's four go except the one Home holds; the new two added
	CHECK(used_channels() == channels - 4 + 1 + 2);

	CHECK(codeplug_channel(before.channels[3], &ch) && strcmp(ch.name, "DMR USA") == 0);
	CHECK(!codeplug_channel(before.channels[0], &ch) || strcmp(ch.name, "Rpt FM") != 0);
	CHECK(used_contacts() == contacts);// contacts are never deleted by an update
	CHECK(imports[0].key == zone_import_key(9996543, 0x11223344) && imports[1].key == 0);
	CHECK(imports[0].count == 2);
	// Sent once more: the two it wrote lead the zone untouched, so they are replaced and Mine stays again
	channels = used_channels();
	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK && r.updated);
	CHECK(codeplug_zone(1, &z) && z.count == 3 && z.channels[2] == mine && codeplug_channel(mine, &ch));
	CHECK(used_channels() == channels);
	// The same zone ID from another sender is another zone; its name is taken, so it gets a suffix
	CHECK(zone_import_zone(recbuf, len, 9991277, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 3 && strcmp(r.name, "Club v2") == 0);
	CHECK(zone_import_zone(recbuf, len, 9991278, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 4 && strcmp(r.name, "Club v2 2") == 0);
	CHECK(imports[0].key == zone_import_key(9991278, 0x11223344));
	CHECK(imports[2].key == zone_import_key(9996543, 0x11223344));
	// The user renamed the import: sent again, it comes in as a new zone
	CHECK(codeplug_zone(1, &z));
	strcpy(z.name, "Mine");
	CHECK(codeplug_set_zone(1, &z));
	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 5);
}

// The user changed one imported channel and put one of their own first: the earlier copy can't be told apart by its
// signature, so only the channels identical to one the new copy brings are replaced; the rest stay
static void test_update_user_changes(void)
{
	zone_import_result_t r;
	zone_t z, before;
	channel_t ch;

	fresh();
	with_zones();
	int len = build_record(0x5555, "Club", recbuf);

	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK);
	CHECK(codeplug_zone(1, &before) && before.count == 4);
	// Transmit on for the second
	CHECK(codeplug_channel(before.channels[1], &ch) && ch.rx_only);
	ch.rx_only = false;
	CHECK(codeplug_set_channel(before.channels[1], &ch));
	// A channel of the user's, first in the zone
	ch = make("Mine", 146550000, 146550000, false);
	ch.rx_only = true;
	uint16_t mine = codeplug_free_channel();

	CHECK(mine != 0 && codeplug_set_channel(mine, &ch));
	z = before;
	memmove(&z.channels[1], &z.channels[0], z.count * sizeof(z.channels[0]));
	z.channels[0] = mine;
	z.count++;
	CHECK(codeplug_set_zone(1, &z));
	int channels = used_channels();

	// The same record again: three of the earlier channels are identical to new ones and go; the changed one and Mine
	// stay, after the new four, in their order
	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK && r.updated && r.channels == 4);
	CHECK(codeplug_zone(1, &z) && z.count == 6);
	CHECK(z.channels[4] == mine && z.channels[5] == before.channels[1]);
	CHECK(codeplug_channel(mine, &ch) && strcmp(ch.name, "Mine") == 0);
	CHECK(codeplug_channel(before.channels[1], &ch) && !ch.rx_only);
	CHECK(used_channels() == channels - 3 + 4);
	for (int k = 0; k < 4; k++) {
		CHECK(codeplug_channel(z.channels[k], &ch) && ch.rx_only);
	}
	// From here the new four lead the zone untouched: a different record replaces exactly them
	zsWriter_t w;
	zsChannel_t zc = zch("New A", 145500000, 0, false, 0);

	zsWriterBegin(&w, recbuf, sizeof(recbuf), 0x5555, 0, "Club");
	zsWriterChannel(&w, &zc);
	len = zsWriterEnd(&w);
	channels = used_channels();
	CHECK(zone_import_zone(recbuf, len, 9996543, imports, &r) == ZI_OK && r.updated);
	CHECK(codeplug_zone(1, &z) && z.count == 3 && z.channels[1] == mine && z.channels[2] == before.channels[1]);
	CHECK(used_channels() == channels - 4 + 1);
}

// A channel or contact the record gives no name gets one: "Channel" and its place in the record (a channel left out
// still counts), "TG" or "ID" and the number
static void test_default_names(void)
{
	zone_import_result_t r;
	zsWriter_t w;
	zsChannel_t zc;
	zsContact_t c;
	zone_t z;
	channel_t ch;
	contact_t ct;

	fresh();
	with_zones();
	zsWriterBegin(&w, recbuf, sizeof(recbuf), 11, 0, "Bare");
	c = zct("", 3100, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	c = zct("", 9997001, ZS_CONTACT_PRIVATE);
	zsWriterContact(&w, &c);
	zc = zch("", 441000000, 0, true, 1);
	zsWriterChannel(&w, &zc);
	zc = zch("", 300000000, 0, false, 0);	// out of band
	zsWriterChannel(&w, &zc);
	zc = zch("", 442000000, 0, true, 2);
	zsWriterChannel(&w, &zc);
	int len = zsWriterEnd(&w);

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_OK);
	CHECK(r.channels == 2 && r.out_of_band == 1 && r.contacts_added == 2);
	CHECK(codeplug_zone(r.zone, &z) && z.count == 2);
	CHECK(codeplug_channel(z.channels[0], &ch) && strcmp(ch.name, "Channel 1") == 0);
	CHECK(codeplug_contact(ch.contact, &ct) && strcmp(ct.name, "TG 3100") == 0);
	CHECK(codeplug_channel(z.channels[1], &ch) && strcmp(ch.name, "Channel 3") == 0);
	CHECK(codeplug_contact(ch.contact, &ct) && strcmp(ct.name, "ID 9997001") == 0);
}

static void test_names_and_list(void)
{
	zone_import_result_t r;
	zsWriter_t w;
	zsChannel_t zc = zch("A", 145500000, 0, false, 0);

	fresh();
	with_zones();
	// A 16-character name taken: the suffix replaces its end
	for (int i = 0; i < 3; i++) {
		zsWriterBegin(&w, recbuf, sizeof(recbuf), 7, 0, "Sixteen chars ok");
		zsWriterChannel(&w, &zc);
		int len = zsWriterEnd(&w);

		CHECK(zone_import_zone(recbuf, len, 100 + i, imports, &r) == ZI_OK);
	}
	zone_t z;

	CHECK(codeplug_zone(1, &z) && strcmp(z.name, "Sixteen chars ok") == 0);
	CHECK(codeplug_zone(2, &z) && strcmp(z.name, "Sixteen chars 2") == 0);
	CHECK(codeplug_zone(3, &z) && strcmp(z.name, "Sixteen chars 3") == 0);
	// The list keeps the newest SETTINGS_ZS_IMPORTS
	for (int i = 3; i < 6; i++) {
		zsWriterBegin(&w, recbuf, sizeof(recbuf), 7, 0, "Z");
		zsWriterChannel(&w, &zc);
		int len = zsWriterEnd(&w);

		CHECK(zone_import_zone(recbuf, len, 100 + i, imports, &r) == ZI_OK);
	}
	for (int i = 0; i < SETTINGS_ZS_IMPORTS; i++) {
		CHECK(imports[i].key == zone_import_key(105 - i, 7));
	}
	// A zone record's channel flag, a bad record
	CHECK(zone_import_zone(recbuf, 5, 1, imports, &r) == ZI_BAD_RECORD);
	zsWriterBegin(&w, recbuf, sizeof(recbuf), 7, ZS_FLAG_CHANNEL, "C");
	zsWriterChannel(&w, &zc);
	int len = zsWriterEnd(&w);

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_BAD_RECORD);
}

static void test_band_and_room(void)
{
	zone_import_result_t r;
	zsWriter_t w;
	zsChannel_t zc;
	zsContact_t c;

	fresh();
	with_zones();
	int channels = used_channels();

	// Nothing the radio can receive: refused, nothing written
	zsWriterBegin(&w, recbuf, sizeof(recbuf), 9, 0, "Far");
	c = zct("TG", 9, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	zc = zch("900", 902000000, 0, true, 1);
	zsWriterChannel(&w, &zc);
	zc = zch("300", 300000000, 0, false, 0);
	zsWriterChannel(&w, &zc);
	int len = zsWriterEnd(&w);

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_NONE_IN_BAND);
	CHECK(r.out_of_band == 2);
	CHECK(used_channels() == channels && used_contacts() == 0 && codeplug_zone_count() == 1 && imports[0].key == 0);

	// Room for one channel only: the first is written, then undone with the contact it added
	channel_t fill = make("Fill", 146000000, 146000000, false);

	for (uint16_t n = 2; n < CODEPLUG_CHANNELS_MAX; n++) {
		CHECK(codeplug_set_channel(n, &fill));
	}
	CHECK(codeplug_free_channel() == CODEPLUG_CHANNELS_MAX);
	zsWriterBegin(&w, recbuf, sizeof(recbuf), 10, 0, "Two");
	c = zct("TG", 9, ZS_CONTACT_GROUP);
	zsWriterContact(&w, &c);
	zc = zch("One", 441000000, 0, true, 1);
	zsWriterChannel(&w, &zc);
	zc = zch("Two", 442000000, 0, true, 1);
	zsWriterChannel(&w, &zc);
	len = zsWriterEnd(&w);
	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_NO_CHANNELS);
	CHECK(codeplug_free_channel() == CODEPLUG_CHANNELS_MAX && used_contacts() == 0 && codeplug_zone_count() == 1);
	CHECK(imports[0].key == 0);

	// No free contact: refused before any channel is written
	fresh();
	with_zones();
	channels = used_channels();
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		contact_t k = contact("K", 1000 + i, CONTACT_GROUP);

		CHECK(codeplug_set_contact(i, &k));
	}
	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_NO_CONTACTS);
	CHECK(used_channels() == channels && codeplug_zone_count() == 1);
	// ... but a contact that is there by ID needs no room
	contact_t k = contact("Mine", 9, CONTACT_GROUP);

	CHECK(codeplug_set_contact(7, &k));
	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_OK);
	CHECK(r.contacts_matched == 1 && r.contacts_added == 0);
}

static void test_virtual_zone(void)
{
	zone_import_result_t r;
	zone_t z;
	channel_t ch = make("Only", 146520000, 146520000, false);

	fresh();
	CHECK(codeplug_set_channel(1, &ch));// the first edit stores the built-in channels too
	CHECK(!codeplug_zones_stored() && codeplug_zone_count() == 1);
	zone_t all;

	CHECK(codeplug_zone(0, &all) && all.count > 1);
	int len = build_record(0x55, "Club", recbuf);

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_OK);
	CHECK(codeplug_zones_stored() && codeplug_zone_count() == 2);
	// "All channels" as the user saw it, then ours
	CHECK(codeplug_zone(0, &z) && strcmp(z.name, "All channels") == 0 && z.count == all.count);
	CHECK(memcmp(z.channels, all.channels, sizeof(z.channels[0]) * all.count) == 0);
	CHECK(codeplug_zone(1, &z) && z.count == 4 && r.zone == 1);
}

// A new zone goes through the zone write layer: it takes a deleted zone's free slot, ahead of the zones after it, the
// result says where it landed, and the move is logged for whoever keeps a zone index. A channel goes to the end of
// the picked zone the same way
static void test_lands_ahead(void)
{
	zone_import_result_t r;
	zone_t z;
	uint16_t out;

	fresh();
	with_zones();// 0 Home
	memset(&z, 0, sizeof(z));
	z.count = 1;
	z.channels[0] = 1;
	strcpy(z.name, "One");
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);// 1 One
	strcpy(z.name, "Two");
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);// 2 Two
	CHECK(zone_edit_delete(1) == ZONE_EDIT_OK);// Two moves to 1, slot 1 free
	uint32_t s0 = zone_edit_shifts();
	int len = build_record(0x77, "Club", recbuf);

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_OK);
	CHECK(r.zone == 1 && codeplug_zone(1, &z) && strcmp(z.name, "Club") == 0 && z.count == 4);
	CHECK(zone_edit_shifts() == s0 + 1u);
	CHECK(zone_edit_map(s0, zone_edit_shifts(), 1, &out) == ZONE_MAP_KEPT && out == 2);// Two follows
	CHECK(codeplug_zone(2, &z) && strcmp(z.name, "Two") == 0);
	// An update of it rewrites the zone in place: nothing moves
	uint32_t s1 = zone_edit_shifts();

	CHECK(zone_import_zone(recbuf, len, 1, imports, &r) == ZI_OK && r.updated && r.zone == 1);
	CHECK(zone_edit_shifts() == s1);
	// One channel (Home's, sent back as a copy), to the end of Two
	len = zone_export_channel(1, recbuf, sizeof(recbuf));
	CHECK(len > 0 && zone_import_channel(recbuf, len, 2, &r) == ZI_OK && r.zone == 2);
	CHECK(codeplug_zone(2, &z) && z.count == 2 && z.channels[0] == 1 && z.channels[1] != 1);
	CHECK(zone_edit_shifts() == s1);
}

static void test_export(void)
{
	zone_t z;
	zsRecord_t rec;
	zsChannel_t zc;
	zsContact_t c;
	uint8_t included = 0;

	fresh();
	contact_t tg = contact("Local", 3100, CONTACT_GROUP);

	CHECK(codeplug_set_contact(1, &tg));
	// 80 channels with long names: they don't all fit; every DMR one talks to the same contact (one in the record)
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Big");
	for (uint16_t n = 1; n <= CODEPLUG_ZONE_MAX; n++) {
		char name[17];

		snprintf(name, sizeof(name), "Channel name %03u", n);
		channel_t ch = make(name, 440000000 + n * 12500, 445000000 + n * 12500, (n & 1) != 0);

		if (n & 1) {
			ch.contact = 1;
			ch.color_code = 7;
			ch.timeslot = 2;
		} else {
			ch.rx_tone = (subtone_t){ TONE_CTCSS, 885 };
			ch.power = 0;
		}
		CHECK(codeplug_set_channel(n, &ch));
		z.channels[z.count++] = n;
	}
	CHECK(codeplug_set_zone(0, &z));
	int len = zone_export_zone(0, recbuf, sizeof(recbuf), &included);

	CHECK(len > 0 && len <= ZS_RECORD_MAX);
	CHECK(included > 20 && included < CODEPLUG_ZONE_MAX);
	CHECK(zsParse(recbuf, len, &rec) && rec.channels == included && rec.contacts == 1 && !(rec.flags & ZS_FLAG_CHANNEL));
	CHECK(strcmp(rec.name, "Big") == 0);
	CHECK(zsContactAt(&rec, 0, &c) && c.id == 3100 && strcmp(c.name, "Local") == 0);
	CHECK(zsChannelAt(&rec, 0, &zc) && zc.dmr && zc.contact == 1 && zc.colorCode == 7 && zc.timeslot == 2);
	CHECK(zc.rxHz == 440012500 && zc.txOffsetHz == 5000000 && strcmp(zc.name, "Channel name 001") == 0);
	CHECK(zsChannelAt(&rec, 1, &zc) && !zc.dmr && zc.rxToneType == ZS_TONE_CTCSS && zc.rxTone == 885 && zc.power == ZS_POWER_LOW);
	// The next channel (with its 16-character name, 28 octets at least) wouldn't have fit
	CHECK(len + 28 > ZS_RECORD_MAX);
	// The zone ID follows the name, so sending again updates the receiver's copy
	uint32_t id = rec.zoneId;

	CHECK(zone_export_zone(0, recbuf, sizeof(recbuf), NULL) == len && zsParse(recbuf, len, &rec) && rec.zoneId == id);
	// A channel record, and imported into another zone on a radio with zones
	len = zone_export_channel(1, recbuf, sizeof(recbuf));
	CHECK(len > 0 && zsParse(recbuf, len, &rec) && (rec.flags & ZS_FLAG_CHANNEL) && rec.channels == 1 && rec.contacts == 1);
	CHECK(zone_export_channel(999, recbuf, sizeof(recbuf)) == 0);
	CHECK(zone_export_zone(9, recbuf, sizeof(recbuf), &included) == 0 && included == 0);

	uint8_t copy[ZS_RECORD_MAX];

	memcpy(copy, recbuf, (size_t)len);
	fresh();
	with_zones();
	zone_import_result_t r;

	CHECK(zone_import_channel(copy, len, 0, &r) == ZI_OK);
	CHECK(r.channels == 1 && r.zone == 0 && r.contacts_added == 1);
	CHECK(codeplug_zone(0, &z) && z.count == 2);
	channel_t ch;

	CHECK(codeplug_channel(z.channels[1], &ch) && ch.rx_only && ch.mode == RADIO_MODE_DMR && ch.contact != 0);
	CHECK(zone_import_channel(copy, len, 1, &r) == ZI_NO_ZONE);
	// A full zone
	for (uint16_t n = z.count; n < CODEPLUG_ZONE_MAX; n++) {
		z.channels[z.count++] = 1;
	}
	CHECK(codeplug_set_zone(0, &z));
	CHECK(zone_import_channel(copy, len, 0, &r) == ZI_ZONE_FULL);
	// A zone record is not a channel record
	len = build_record(1, "Z", copy);
	CHECK(zone_import_channel(copy, len, 0, &r) == ZI_BAD_RECORD);
}

#if CONFIG_DMR_TRUNK
// Trunked channels in shares (zone-share.md 4.2, dmr-trunking.md 12.6)
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

#define OWN_ID		3100777u

// System n: its code (model, NET, category), options, sharing, the radio's own ID on it, a fallback channel
static void t3_system(uint8_t n, const char *name, uint8_t model, uint16_t net, uint8_t cat, uint16_t opts,
		      uint8_t sharing, uint16_t fb, const char *fb_name)
{
	uint8_t v[64];

	memset(v, 0, sizeof(v));
	strncpy((char *)v, name, 16);
	v[16] = model;
	put16(&v[17], net);
	v[19] = cat;
	v[20] = 2;
	put16(&v[21], opts);
	put32(&v[23], OWN_ID);
	v[27] = sharing;
	v[28] = 40;
	put16(&v[29], fb);
	put32(&v[31], 450000000);
	put16(&v[35], 1);
	v[37] = 2;
	put32(&v[39], 5000000);
	v[45] = 1;
	put16(&v[46], 7);
	put16(&v[62], fb != 0 ? radio_t3_name_check(fb_name) : 0);
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + n), v, sizeof(v)));
}

static void t3_chan(uint16_t key, uint8_t n, bool ctrl, uint16_t lcn, uint32_t rx, uint32_t tx)
{
	uint8_t r[13] = { n, ctrl ? 1 : 0 };

	put16(&r[2], lcn);
	put32(&r[4], rx);
	put32(&r[8], tx);
	r[12] = 0xFF;
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_CHAN + key), r, sizeof(r)));
}

static void t3_key(uint8_t n)
{
	uint8_t k[19];

	for (int i = 0; i < 19; i++) {
		k[i] = (uint8_t)(0x40 + i);
	}
	CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_T3_KEY + n), k, sizeof(k)));
}

static uint16_t find_channel(const char *name)
{
	channel_t ch;

	for (uint16_t i = 1; i <= CODEPLUG_CHANNELS_MAX; i++) {
		if (codeplug_channel(i, &ch) && strcmp(ch.name, name) == 0) {
			return i;
		}
	}
	return 0;
}

// The sender: "County" shared whole (it authenticates: its key goes too), "Metro" by identity, "Private" not shared.
// Zone "Trunk": a channel of each, and the conventional channel County falls back to
static int sender_record(uint8_t *out, uint8_t *included)
{
	zone_t z;
	contact_t tg = contact("Fire", 1, CONTACT_GROUP);

	fresh();
	CHECK(codeplug_set_contact(1, &tg));
	channel_t c1 = make("County Fire", 451000000, 456000000, true);
	channel_t c2 = make("Metro Ops", 452000000, 457000000, true);
	channel_t c3 = make("Talkaround", 453000000, 453000000, false);
	channel_t c4 = make("Private", 454000000, 459000000, true);

	c1.contact = c2.contact = c4.contact = 1;
	CHECK(codeplug_set_channel(1, &c1) && codeplug_set_channel(2, &c2) && codeplug_set_channel(3, &c3) &&
	      codeplug_set_channel(4, &c4));
	t3_system(0, "County", 1, 5, 1, 0x0840, 2, 3, "Talkaround");
	t3_chan(0, 0, true, 1, 451000000, 456000000);
	t3_chan(1, 0, false, 2, 451100000, 456100000);
	t3_key(0);
	t3_system(1, "Metro", 2, 300, 2, 0x0040, 1, 0, "");
	t3_chan(2, 1, true, 1, 452000000, 457000000);
	t3_system(2, "Private", 1, 9, 1, 0, 0, 0, "");
	t3_chan(3, 2, true, 1, 454000000, 459000000);
	CHECK(radio_t3_tag_set(1, 0, "County Fire") && radio_t3_tag_set(2, 1, "Metro Ops") &&
	      radio_t3_tag_set(4, 2, "Private"));
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Trunk");
	z.count = 4;
	for (int i = 0; i < 4; i++) {
		z.channels[i] = (uint16_t)(i + 1);
	}
	CHECK(codeplug_set_zone(0, &z));
	return zone_export_zone(0, out, ZS_RECORD_MAX, included);
}

static void test_trunked_export(void)
{
	static uint8_t out[ZS_RECORD_MAX];
	zsRecord_t r;
	zsSystem_t s;
	zsSysChan_t c;
	zsChannel_t ch;
	uint8_t included = 0;
	int n = sender_record(out, &included);

	CHECK(n > 0 && included == 3);	// "Private" isn't shared: left out
	CHECK(zsParse(out, n, &r) && (r.flags & ZS_FLAG_SYSTEMS) && r.systems == 2 && r.channels == 3);
	CHECK(zsSystemAt(&r, 0, &s) && s.level == ZS_SYS_FULL && strcmp(s.name, "County") == 0 && s.net == 5 &&
	      s.opts == 0x0840 && s.chans == 2 && s.hasKey && s.hasPsn && s.key[0] == 0x40 && s.psn == 0x505152 &&
	      s.holdS == 40 && s.planBaseHz == 450000000 && s.sites == 1 && s.site[0] == 7);
	CHECK(s.fallback == 3);	// Talkaround, the record's third channel
	CHECK(zsSysChanAt(&r, 0, 0, &c) && c.ctrl && c.rxHz == 451000000 && zsSysChanAt(&r, 0, 1, &c) && !c.ctrl);
	CHECK(zsSystemAt(&r, 1, &s) && s.level == ZS_SYS_IDENTITY && strcmp(s.name, "Metro") == 0 && s.net == 300 &&
	      s.category == 2 && s.chans == 0 && !s.hasKey);
	CHECK(zsChannelAt(&r, 0, &ch) && ch.system == 1 && zsChannelAt(&r, 1, &ch) && ch.system == 2 &&
	      zsChannelAt(&r, 2, &ch) && ch.system == 0);
	// The radio's own ID on a system is never sent
	uint8_t id[3] = { (uint8_t)(OWN_ID >> 16), (uint8_t)(OWN_ID >> 8), (uint8_t)OWN_ID };
	bool found = false;
	for (int i = 0; i + 3 <= n; i++) {
		found = found || memcmp(&out[i], id, 3) == 0;
	}
	CHECK(!found);

	// A zone with no trunked channel stays a record without systems, and keeps its zone ID either way
	zone_t z;
	CHECK(codeplug_zone(0, &z));
	z.count = 1;
	z.channels[0] = 3;
	CHECK(codeplug_set_zone(0, &z));
	uint32_t id_with = r.zoneId;
	n = zone_export_zone(0, out, ZS_RECORD_MAX, &included);
	CHECK(zsParse(out, n, &r) && r.flags == 0 && r.systems == 0 && r.zoneId == id_with);
	// Only an unshared system's channel: nothing to send
	z.channels[0] = 4;
	CHECK(codeplug_set_zone(0, &z));
	CHECK(zone_export_zone(0, out, ZS_RECORD_MAX, &included) == 0 && included == 0);
}

static void test_trunked_import(void)
{
	static uint8_t rec_buf[ZS_RECORD_MAX];
	zone_import_result_t res;
	radio_t3_tag_t tag;
	uint8_t v[64];
	uint8_t included;
	int n = sender_record(rec_buf, &included);

	// A radio without either system: County comes in whole and usable, Metro by identity and locked out
	fresh();
	with_zones();
	CHECK(zone_import_zone(rec_buf, n, 3100001, imports, &res) == ZI_OK);
	CHECK(res.channels == 3 && res.systems_added == 2 && res.t3_unavailable == 1);
	int county = radio_t3_find(1, 5, 1), metro = radio_t3_find(2, 300, 2);
	CHECK(county >= 0 && metro >= 0 && county != metro);
	CHECK(radio_t3_usable((uint8_t)county) && !radio_t3_usable((uint8_t)metro));
	uint16_t fire = find_channel("County Fire"), ops = find_channel("Metro Ops"), talk = find_channel("Talkaround");
	CHECK(fire != 0 && ops != 0 && talk != 0);
	CHECK(radio_t3_tag(fire, "County Fire", &tag) && tag.sys == county);
	CHECK(radio_t3_tag(ops, "Metro Ops", &tag) && tag.sys == metro);
	CHECK(!radio_t3_tag(talk, "Talkaround", &tag));
	CHECK(codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_SYSTEM + county), v, sizeof(v)) == 64);
	CHECK(v[23] == 0 && v[24] == 0 && v[25] == 0 && v[26] == 0);	// own ID: the receiver's
	CHECK(v[27] == 0);	// sharing off: the receiver chooses
	CHECK(v[29] == (uint8_t)talk && v[30] == (uint8_t)(talk >> 8));	// its fallback, here
	CHECK(v[62] == (uint8_t)radio_t3_name_check("Talkaround"));
	uint8_t k[19];
	CHECK(codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_KEY + county), k, sizeof(k)) == 19 && k[0] == 0x40);
	CHECK(codeplug_kv_get((uint16_t)(CODEPLUG_KEY_T3_KEY + metro), k, sizeof(k)) < 0);
	radio_t3_setup_t *su = radio_t3_staged();
	CHECK(radio_t3_load((uint8_t)county, su) && su->nchans == 2 && su->cfg.nctrl == 1 && su->fb_chan == talk);

	// Sent again: an update, no system added twice
	CHECK(zone_import_zone(rec_buf, n, 3100001, imports, &res) == ZI_OK && res.updated && res.systems_added == 0 &&
	      res.t3_unavailable == 1);

	// A radio with County already (its own set-up, by another name): the channel joins it, nothing of it is written
	fresh();
	with_zones();
	t3_system(0, "Our county", 1, 5, 1, 0x0040, 0, 0, "");
	t3_chan(0, 0, true, 1, 451000000, 456000000);
	CHECK(zone_import_zone(rec_buf, n, 3100001, imports, &res) == ZI_OK && res.systems_added == 1 &&
	      res.t3_unavailable == 1);
	CHECK(radio_t3_find(1, 5, 1) == 0 && radio_t3_name(0, (char *)v) && strcmp((char *)v, "Our county") == 0);
	fire = find_channel("County Fire");
	CHECK(radio_t3_tag(fire, "County Fire", &tag) && tag.sys == 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_T3_KEY, k, sizeof(k)) < 0);	// its key isn't taken over

	// No free system slot: the trunked channels are left out and counted, the rest come in
	fresh();
	with_zones();
	for (uint8_t i = 0; i < 8; i++) {
		t3_system(i, "Other", 0, (uint16_t)(100 + i), 1, 0, 0, 0, "");
	}
	int before = used_channels();
	CHECK(zone_import_zone(rec_buf, n, 3100001, imports, &res) == ZI_OK);
	CHECK(res.channels == 1 && res.t3_left_out == 2 && res.systems_added == 0 && used_channels() == before + 1);
	CHECK(find_channel("Talkaround") != 0 && find_channel("County Fire") == 0 && radio_t3_find(1, 5, 1) < 0);
	CHECK(used_contacts() == 0);	// the left-out channels' talkgroup isn't added

	// Only trunked channels, and no room for their system: nothing to import
	uint8_t one[ZS_RECORD_MAX];
	fresh();
	t3_system(0, "County", 1, 5, 1, 0x0040, 2, 0, "");
	t3_chan(0, 0, true, 1, 451000000, 456000000);
	channel_t solo = make("County Fire", 451000000, 456000000, true);
	CHECK(codeplug_set_channel(1, &solo) && radio_t3_tag_set(1, 0, "County Fire"));
	int m = zone_export_channel(1, one, sizeof(one));
	CHECK(m > 0);
	fresh();
	with_zones();
	for (uint8_t i = 0; i < 8; i++) {
		t3_system(i, "Other", 0, (uint16_t)(100 + i), 1, 0, 0, 0, "");
	}
	before = used_channels();
	CHECK(zone_import_channel(one, m, 0, &res) == ZI_NO_SYSTEMS && used_channels() == before);

	// A failure after the systems were added takes them out again: no channel numbers left
	fresh();
	with_zones();
	channel_t filler = make("Filler", 146000000, 146000000, false);
	for (uint16_t i = 2; i <= CODEPLUG_CHANNELS_MAX; i++) {
		codeplug_set_channel(i, &filler);
	}
	CHECK(zone_import_zone(rec_buf, n, 3100001, imports, &res) == ZI_NO_CHANNELS);
	CHECK(radio_t3_find(1, 5, 1) < 0 && radio_t3_find(2, 300, 2) < 0);
	CHECK(codeplug_kv_get(CODEPLUG_KEY_T3_KEY, k, sizeof(k)) < 0);
	uint8_t probe[13];
	codeplug_kv_cursor_t cur;
	uint16_t key;
	codeplug_kv_begin(&cur, CODEPLUG_KEY_T3_CHAN, CODEPLUG_KEY_T3_CHAN + 0xFFu);
	CHECK(codeplug_kv_next(&cur, &key, probe, sizeof(probe)) < 0);	// none of its channels left behind
}

// The offer's preview (zone_import_t3_locked): which trunked channels of a share would come in unavailable on this
// radio, before anything is written. Channels in the share: County Fire (County, whole, with its key), Metro Ops
// (Metro, identity only), Talkaround (conventional)
static void test_trunked_locked(void)
{
	static uint8_t rec_buf[ZS_RECORD_MAX];
	zsRecord_t r;
	uint8_t included;
	int n = sender_record(rec_buf, &included);

	// A radio with neither system: County comes in usable, Metro by identity does not
	fresh();
	CHECK(zsParse(rec_buf, n, &r) && r.channels == 3);
	CHECK(!zone_import_t3_locked(&r, 0) && zone_import_t3_locked(&r, 1) && !zone_import_t3_locked(&r, 2));
	CHECK(!zone_import_t3_locked(&r, 3));	// past the last channel

	// The radio's own set-up decides: County that authenticates with no key here is locked, whatever the share
	// carries; Metro set up here with a control channel is not
	fresh();
	t3_system(0, "Our county", 1, 5, 1, 0x0840, 0, 0, "");
	t3_chan(0, 0, true, 1, 451000000, 456000000);
	t3_system(1, "Our metro", 2, 300, 2, 0x0040, 0, 0, "");
	t3_chan(1, 1, true, 1, 452000000, 457000000);
	CHECK(zone_import_t3_locked(&r, 0) && !zone_import_t3_locked(&r, 1));
	t3_key(0);
	CHECK(!zone_import_t3_locked(&r, 0));

	// The whole system, but it authenticates and its key didn't come: locked
	n = sender_record(rec_buf, &included);
	CHECK(codeplug_kv_remove(CODEPLUG_KEY_T3_KEY));
	n = zone_export_zone(0, rec_buf, ZS_RECORD_MAX, &included);
	fresh();
	CHECK(n > 0 && zsParse(rec_buf, n, &r) && zone_import_t3_locked(&r, 0));

	// The whole system with no control channel among its channels: nothing to find it on
	n = sender_record(rec_buf, &included);
	t3_chan(0, 0, false, 1, 451000000, 456000000);	// its one control channel, now a traffic channel
	n = zone_export_zone(0, rec_buf, ZS_RECORD_MAX, &included);
	fresh();
	CHECK(n > 0 && zsParse(rec_buf, n, &r) && zone_import_t3_locked(&r, 0));
}

// A deleted channel takes its tag with it: a channel written at its number later isn't trunked
static void test_trunked_delete(void)
{
	radio_t3_tag_t tag;
	channel_t ch = make("County Fire", 451000000, 456000000, true);

	fresh();
	CHECK(codeplug_set_channel(5, &ch));
	CHECK(radio_t3_tag_set(5, 0, "County Fire") && radio_t3_tag(5, "County Fire", &tag));
	CHECK(codeplug_delete_channel(5));
	CHECK(codeplug_set_channel(5, &ch));
	CHECK(!radio_t3_tag(5, "County Fire", &tag));
}
#endif

int main(void)
{
	test_import();
	test_contacts_unread();
	test_update();
	test_update_user_changes();
	test_default_names();
	test_names_and_list();
	test_band_and_room();
	test_virtual_zone();
	test_lands_ahead();
	test_export();
#if CONFIG_DMR_TRUNK
	test_trunked_export();
	test_trunked_import();
	test_trunked_locked();
	test_trunked_delete();
#endif
	printf("zoneImportTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
