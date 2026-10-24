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
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

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

// VHF and UHF; 300 MHz and 900 MHz are out of band
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 5, .flash = &sim_flash };

static settings_zs_import_t imports[SETTINGS_ZS_IMPORTS];
static uint8_t recbuf[ZS_RECORD_MAX];

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
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

// Contacts: 1 TG 91 "World", 2 TG 91 again "Dup", 3 private 1234567 "Bob" (only an out-of-band channel uses it),
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
	c = zct("Bob", 1234567, ZS_CONTACT_PRIVATE);
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

	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_OK);
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
	CHECK(imports[0].key == zone_import_key(3101276, 0x11223344) && memcmp(imports[0].zone, "Club", 5) == 0);
	// Home untouched
	CHECK(codeplug_zone(0, &z) && z.count == 1 && z.channels[0] == 1 && strcmp(z.name, "Home") == 0);
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

	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_OK);
	CHECK(codeplug_zone(1, &before) && before.count == 4);
	int channels = used_channels(), contacts = used_contacts();

	// The user also put one of the imported channels in Home: an update keeps it
	CHECK(codeplug_zone(0, &z));
	z.channels[z.count++] = before.channels[3];
	CHECK(codeplug_set_zone(0, &z));
	// and turned transmit on for another: that one is the user's now, and the update keeps it in the zone
	channel_t ch;

	CHECK(codeplug_channel(before.channels[1], &ch) && ch.rx_only);
	ch.rx_only = false;
	CHECK(codeplug_set_channel(before.channels[1], &ch));
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
	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_OK);
	CHECK(r.updated && r.zone == 1 && r.channels == 2);
	CHECK(strcmp(r.name, "Club") == 0);// the name it got here stays
	CHECK(codeplug_zone_count() == 3);
	CHECK(codeplug_zone(1, &z) && z.count == 3 && strcmp(z.name, "Club") == 0);
	CHECK(z.channels[2] == before.channels[1]);// the transmit channel kept, after the new ones
	CHECK(codeplug_channel(before.channels[1], &ch) && !ch.rx_only);
	// Old channels gone except the one Home holds and the one kept; the new two added
	CHECK(used_channels() == channels - 4 + 1 + 1 + 2);

	CHECK(codeplug_channel(before.channels[3], &ch) && strcmp(ch.name, "DMR USA") == 0);
	CHECK(!codeplug_channel(before.channels[0], &ch) || strcmp(ch.name, "Rpt FM") != 0);
	CHECK(used_contacts() == contacts);// contacts are never deleted by an update
	CHECK(imports[0].key == zone_import_key(3101276, 0x11223344) && imports[1].key == 0);
	// The same zone ID from another sender is another zone; its name is taken, so it gets a suffix
	CHECK(zone_import_zone(recbuf, len, 3101277, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 3 && strcmp(r.name, "Club v2") == 0);
	CHECK(zone_import_zone(recbuf, len, 3101278, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 4 && strcmp(r.name, "Club v2 2") == 0);
	CHECK(imports[0].key == zone_import_key(3101278, 0x11223344));
	CHECK(imports[2].key == zone_import_key(3101276, 0x11223344));
	// The user renamed the import: sent again, it comes in as a new zone
	CHECK(codeplug_zone(1, &z));
	strcpy(z.name, "Mine");
	CHECK(codeplug_set_zone(1, &z));
	CHECK(zone_import_zone(recbuf, len, 3101276, imports, &r) == ZI_OK);
	CHECK(!r.updated && r.zone == 5);
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

int main(void)
{
	test_import();
	test_update();
	test_names_and_list();
	test_band_and_room();
	test_virtual_zone();
	test_export();
	printf("zoneImportTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
