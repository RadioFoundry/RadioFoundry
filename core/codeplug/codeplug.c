// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug, read only: a legacy-layout codeplug in the board's data flash (docs/spec/legacy-cps-interop.md 7,
// docs/spec/hardware-dm1701.md 13.2), or a small built-in set of simplex FM channels when there is none.
// Records are read from flash on demand; RAM holds only an in-use bitmap of the channels and the used zone slots.
#include <string.h>
#include "codeplug.h"
#include "board.h"

// Where the two legacy images sit in the data flash. The STM32 family puts the 64 KB EEPROM image at 0 and the
// flash image at +128 KB. TODO: these belong in the board descriptor once a radio with other offsets is added
#ifndef CODEPLUG_LEGACY_EEPROM_BASE
#define CODEPLUG_LEGACY_EEPROM_BASE	0x000000u
#endif
#ifndef CODEPLUG_LEGACY_FLASH_BASE
#define CODEPLUG_LEGACY_FLASH_BASE	0x020000u
#endif

#define EE(a)		(CODEPLUG_LEGACY_EEPROM_BASE + (a))
#define FL(a)		(CODEPLUG_LEGACY_FLASH_BASE + (a))

// Channels (7.3.3): bank 0 = channels 1-128 in the EEPROM image, banks 1-7 = 129-1024 in the flash image.
// Every bank is a 16-byte in-use bitmap followed by 128 records of 56 bytes
#define CH_MAX		1024u
#define CH_PER_BANK	128u
#define CH_BANKS	(CH_MAX / CH_PER_BANK)
#define CH_SIZE		56u
#define BANK0_BITMAP	EE(0x3780u)
#define BANK1_BITMAP	FL(0x7B1B0u)
#define BANK_STRIDE	(16u + CH_PER_BANK * CH_SIZE)

// Channel record offsets
#define CH_NAME		0x00u
#define CH_RX		0x10u
#define CH_TX		0x14u
#define CH_MODE		0x18u
#define CH_POWER	0x19u
#define CH_TOT		0x1Bu	// 15 s units, 0 = off
#define CH_RX_TONE	0x20u
#define CH_TX_TONE	0x22u
#define CH_LFLAG1	0x26u
#define CH_DMR_ID	0x27u
#define CH_RX_GROUP	0x2Bu
#define CH_CC		0x2Cu
#define CH_CONTACT	0x2Eu
#define CH_FLAG1	0x30u	// bits 0-1 talker alias on TS1, 2-3 on TS2: 0 off, 1 APRS, 2 text, 3 both
#define CH_FLAG2	0x31u
#define CH_FLAG4	0x33u
#define FLAG2_TS2	0x40u
#define FLAG4_RX_ONLY	0x04u
#define FLAG4_25K	0x02u
#define LFLAG1_ID	0x80u	// per-channel DMR ID present
#define LFLAG1_DMO	0x04u	// force direct mode

// General settings (7.3.2): the radio's DMR ID, 8 BCD digits big endian
#define GEN_BASE	EE(0x00E0u)
#define GEN_DMR_ID	EE(0x00E8u)
#define GEN_GROUP_HANG	0x18u	// offsets in the block
#define GEN_PRIV_HANG	0x19u
#define GEN_HANG_UNIT_MS 500u	// TODO: the unit is not in the interop spec; 500 ms steps as in the vendor CPS range

// Contacts (7.3.5) and RX group lists (7.3.6), flash image
#define CONTACT_BASE	FL(0x87620u)
#define CONTACT_SIZE	24u
#define LIST_COUNTS	FL(0x8D620u)
#define LIST_BASE	FL(0x8D6A0u)
#define LIST_SIZE	80u

// Zones (7.2, 7.3.4)
#define ZONE_SLOTS	68u
#define ZONE_BITMAP	EE(0x8010u)
#define ZONE_DATA	EE(0x8030u)
#define ZONE_LAYOUT	EE(0x806Fu)	// 0x00-0x04: 80 channels per zone, else the old 16-channel layout
#define ZONE_NAME_LEN	16u

static codeplug_source_t source = CODEPLUG_NONE;

// Legacy codeplug index
static uint8_t used[CH_MAX / 8];	// bit set = channel record passed the checks
static uint16_t channel_max;		// highest used channel number
static uint8_t zone_slot[ZONE_SLOTS];
static uint8_t zone_count;
static uint8_t zone_channels;		// 80 or 16
static uint16_t zone_record;		// 0xB0 or 0x30

// Built-in channels: common simplex calling and working frequencies, kept if they fall in one of the board's bands
typedef struct {
	const char *name;
	uint32_t hz;
	uint8_t bw;
} builtin_t;

static const builtin_t builtin[] = {
	{ "Simplex 145.500", 145500000u, RADIO_BW_25K },
	{ "Simplex 146.520", 146520000u, RADIO_BW_25K },
	{ "Simplex 146.550", 146550000u, RADIO_BW_25K },
	{ "Simplex 433.500", 433500000u, RADIO_BW_12K5 },
	{ "Simplex 446.000", 446000000u, RADIO_BW_12K5 },
	{ "Simplex 446.500", 446500000u, RADIO_BW_12K5 },
};

#define BUILTIN_COUNT	(sizeof(builtin) / sizeof(builtin[0]))

static uint8_t builtin_map[BUILTIN_COUNT];	// channel number - 1 -> builtin[] index
static uint8_t builtin_count;

static bool flash_read(uint32_t addr, void *buf, uint32_t len)
{
	return board.flash != NULL && board.flash->size != 0 && board.flash->read(addr, buf, len);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Packed BCD, digits most significant first in the word; false if a nibble is not 0-9
static bool bcd(uint32_t word, uint8_t digits, uint32_t *out)
{
	uint32_t v = 0;
	for (int i = digits - 1; i >= 0; i--) {
		uint32_t d = (word >> (4 * i)) & 0xFu;
		if (d > 9u) {
			return false;
		}
		v = v * 10u + d;
	}
	*out = v;
	return true;
}

// Frequency field: BCD of 10 Hz units as a LE word (438.500 MHz = bytes 00 00 85 43)
static bool freq(const uint8_t *p, uint32_t *hz)
{
	uint32_t v;
	if (!bcd(le32(p), 8, &v) || v == 0) {
		return false;
	}
	*hz = v * 10u;
	return true;
}

// Tone field (7.3.3): 0 / 0xFFFF none; bit 15 DCS (bit 14 inverted) with the octal digits written as hex digits;
// otherwise CTCSS as BCD tenths of Hz. Anything malformed reads as no tone
static subtone_t tone(uint16_t v)
{
	subtone_t t = { TONE_NONE, 0 };
	if (v == 0 || v == 0xFFFFu) {
		return t;
	}
	if (v & 0x8000u) {
		uint16_t code = 0;
		for (int i = 2; i >= 0; i--) {
			uint16_t d = (v >> (4 * i)) & 0xFu;
			if (d > 7u) {
				return t;
			}
			code = (uint16_t)(code * 8u + d);
		}
		t.type = (v & 0x4000u) ? TONE_DCS_INV : TONE_DCS;
		t.value = code;
	} else if ((v & 0x4000u) == 0) {
		uint32_t tenths;
		if (bcd(v, 4, &tenths) && tenths >= 300u && tenths <= 3000u) {
			t.type = TONE_CTCSS;
			t.value = (uint16_t)tenths;
		}
	}
	return t;
}

static void copy_name(char *dst, const uint8_t *src)
{
	unsigned n = 0;
	while (n < CODEPLUG_NAME_LEN && src[n] != 0x00 && src[n] != 0xFF) {
		dst[n] = (char)src[n];
		n++;
	}
	dst[n] = '\0';
}

static uint32_t bank_bitmap(unsigned bank)
{
	return bank == 0 ? BANK0_BITMAP : BANK1_BITMAP + (bank - 1u) * BANK_STRIDE;
}

static uint32_t channel_addr(uint16_t number)
{
	uint32_t i = number - 1u;
	return bank_bitmap(i / CH_PER_BANK) + 16u + (i % CH_PER_BANK) * CH_SIZE;
}

static bool is_used(uint16_t number)
{
	return number >= 1 && number <= CH_MAX && (used[(number - 1u) / 8u] & (1u << ((number - 1u) % 8u))) != 0;
}

static void clear_used(uint16_t number)
{
	used[(number - 1u) / 8u] &= (uint8_t)~(1u << ((number - 1u) % 8u));
}

// In-use bitmaps, then the frequencies of every marked record: an erased bank (all 0xFF) fails the BCD check
static void index_channels(void)
{
	channel_max = 0;
	for (unsigned bank = 0; bank < CH_BANKS; bank++) {
		uint8_t *bits = &used[bank * (CH_PER_BANK / 8u)];
		if (!flash_read(bank_bitmap(bank), bits, CH_PER_BANK / 8u)) {
			memset(bits, 0, CH_PER_BANK / 8u);
			continue;
		}
		for (unsigned k = 0; k < CH_PER_BANK; k++) {
			uint16_t number = (uint16_t)(bank * CH_PER_BANK + k + 1u);
			if (!is_used(number)) {
				continue;
			}
			uint8_t f[8];
			uint32_t rx, tx;
			if (!flash_read(channel_addr(number) + CH_RX, f, sizeof(f)) || !freq(&f[0], &rx) || !freq(&f[4], &tx)) {
				clear_used(number);
				continue;
			}
			channel_max = number;
		}
	}
}

static void index_zones(void)
{
	uint8_t layout, bits[(ZONE_SLOTS + 7u) / 8u];
	zone_count = 0;
	if (!flash_read(ZONE_LAYOUT, &layout, 1) || !flash_read(ZONE_BITMAP, bits, sizeof(bits))) {
		return;
	}
	zone_channels = layout <= 0x04 ? 80 : 16;
	zone_record = (uint16_t)(ZONE_NAME_LEN + 2u * zone_channels);
	for (unsigned slot = 0; slot < ZONE_SLOTS; slot++) {
		if ((bits[slot / 8u] & (1u << (slot % 8u))) == 0) {
			continue;
		}
		uint8_t first;
		if (flash_read(ZONE_DATA + slot * zone_record, &first, 1) && first != 0x00 && first != 0xFF) {
			zone_slot[zone_count++] = (uint8_t)slot;
		}
	}
}

static bool in_bands(uint32_t hz, bool need_tx)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (hz >= b->lo_hz && hz <= b->hi_hz && (b->tx || !need_tx)) {
			return true;
		}
	}
	return false;
}

static void index_builtin(void)
{
	builtin_count = 0;
	for (unsigned i = 0; i < BUILTIN_COUNT; i++) {
		if (board.band_count == 0 || in_bands(builtin[i].hz, false)) {
			builtin_map[builtin_count++] = (uint8_t)i;
		}
	}
}

codeplug_source_t codeplug_init(void)
{
	memset(used, 0, sizeof(used));
	channel_max = 0;
	zone_count = 0;
	index_builtin();
	if (board.flash != NULL && board.flash->size != 0) {
		index_channels();
	}
	if (channel_max > 0) {
		index_zones();
		source = CODEPLUG_LEGACY;
	} else {
		source = CODEPLUG_BUILTIN;
	}
	return source;
}

codeplug_source_t codeplug_source(void)
{
	return source;
}

uint16_t codeplug_channel_count(void)
{
	switch (source) {
	case CODEPLUG_LEGACY:
		return channel_max;
	case CODEPLUG_BUILTIN:
		return builtin_count;
	default:
		return 0;
	}
}

static bool builtin_channel(uint16_t number, channel_t *out)
{
	if (number < 1 || number > builtin_count) {
		return false;
	}
	const builtin_t *b = &builtin[builtin_map[number - 1u]];
	memset(out, 0, sizeof(*out));
	strncpy(out->name, b->name, CODEPLUG_NAME_LEN);
	out->rx_hz = out->tx_hz = b->hz;
	out->mode = RADIO_MODE_FM;
	out->bw = b->bw;
	out->power = CODEPLUG_POWER_DEFAULT;
	out->rx_only = board.band_count != 0 && !in_bands(b->hz, true);
	out->color_code = 1;
	out->timeslot = 1;
	return true;
}

static bool legacy_channel(uint16_t number, channel_t *out)
{
	uint8_t r[CH_SIZE];
	if (!is_used(number) || !flash_read(channel_addr(number), r, sizeof(r))) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!freq(&r[CH_RX], &out->rx_hz) || !freq(&r[CH_TX], &out->tx_hz)) {
		return false;
	}
	copy_name(out->name, &r[CH_NAME]);
	out->mode = r[CH_MODE] == 1 ? RADIO_MODE_DMR : RADIO_MODE_FM;
	out->bw = (r[CH_FLAG4] & FLAG4_25K) ? RADIO_BW_25K : RADIO_BW_12K5;
	out->rx_tone = tone(le16(&r[CH_RX_TONE]));
	out->tx_tone = tone(le16(&r[CH_TX_TONE]));
	// 0 = the radio's master power, n = level n - 1 (levels are per radio; clamp to this board's top level)
	if (r[CH_POWER] == 0) {
		out->power = CODEPLUG_POWER_DEFAULT;
	} else {
		uint8_t level = (uint8_t)(r[CH_POWER] - 1u);
		out->power = (board.power_count != 0 && level >= board.power_count) ? (uint8_t)(board.power_count - 1u) : level;
	}
	out->rx_only = (r[CH_FLAG4] & FLAG4_RX_ONLY) != 0;
	out->color_code = r[CH_CC] & 0x0Fu;
	out->timeslot = (r[CH_FLAG2] & FLAG2_TS2) ? 2 : 1;
	out->contact = le16(&r[CH_CONTACT]);
	out->listen_list = r[CH_RX_GROUP];
	if (r[CH_LFLAG1] & LFLAG1_ID) {
		out->dmr_id = ((uint32_t)r[CH_DMR_ID] << 16) | ((uint32_t)r[CH_DMR_ID + 1] << 8) | r[CH_DMR_ID + 2];
	}
	out->dmo = (r[CH_LFLAG1] & LFLAG1_DMO) != 0;
	out->tot_s = (uint16_t)(r[CH_TOT] * 15u);
	out->alias_tx = (uint8_t)(((r[CH_FLAG1] & 0x02u) ? CODEPLUG_ALIAS_TS1_TEXT : 0u) |
				  ((r[CH_FLAG1] & 0x08u) ? CODEPLUG_ALIAS_TS2_TEXT : 0u));
	return true;
}

bool codeplug_channel(uint16_t number, channel_t *out)
{
	switch (source) {
	case CODEPLUG_LEGACY:
		return legacy_channel(number, out);
	case CODEPLUG_BUILTIN:
		return builtin_channel(number, out);
	default:
		return false;
	}
}

uint16_t codeplug_zone_count(void)
{
	if (source == CODEPLUG_NONE) {
		return 0;
	}
	return zone_count > 0 ? zone_count : 1;
}

// The virtual zone: every used channel, as many as a zone_t holds
static void all_channels(zone_t *out)
{
	strncpy(out->name, "All channels", CODEPLUG_NAME_LEN);
	uint16_t top = codeplug_channel_count();
	for (uint16_t n = 1; n <= top && out->count < CODEPLUG_ZONE_MAX; n++) {
		if (source == CODEPLUG_BUILTIN || is_used(n)) {
			out->channels[out->count++] = n;
		}
	}
}

bool codeplug_zone(uint16_t index, zone_t *out)
{
	if (index >= codeplug_zone_count()) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (zone_count == 0) {
		all_channels(out);
		return true;
	}
	uint8_t r[ZONE_NAME_LEN + 2u * 80u];
	if (!flash_read(ZONE_DATA + zone_slot[index] * zone_record, r, zone_record)) {
		return false;
	}
	copy_name(out->name, r);
	for (unsigned i = 0; i < zone_channels && out->count < CODEPLUG_ZONE_MAX; i++) {
		uint16_t n = le16(&r[ZONE_NAME_LEN + 2u * i]);
		if (n == 0) {
			break;
		}
		if (is_used(n)) {
			out->channels[out->count++] = n;	// skip references to empty or broken records
		}
	}
	return true;
}

#if CONFIG_DMR
// DMR ID or talkgroup: 8 BCD digits, bytes in reading order; 0 if malformed or out of the 24-bit range
static uint32_t bcd_id(const uint8_t *p)
{
	uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	uint32_t v;
	if (!bcd(word, 8, &v) || v > 0xFFFFFFu) {
		return 0;
	}
	return v;
}

uint32_t codeplug_dmr_id(void)
{
	uint8_t b[4];
	if (source != CODEPLUG_LEGACY || !flash_read(GEN_DMR_ID, b, sizeof(b))) {
		return 0;
	}
	return bcd_id(b);
}

bool codeplug_contact(uint16_t index, contact_t *out)
{
	uint8_t r[CONTACT_SIZE];
	if (source != CODEPLUG_LEGACY || index < 1 || index > CODEPLUG_CONTACTS_MAX ||
			!flash_read(CONTACT_BASE + (index - 1u) * CONTACT_SIZE, r, sizeof(r))) {
		return false;
	}
	if (r[0] == 0xFF || r[0x14] > CONTACT_ALL_CALL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	copy_name(out->name, r);
	out->type = r[0x14];
	out->id = out->type == CONTACT_ALL_CALL ? 0xFFFFFFu : bcd_id(&r[0x10]);
	if (out->id == 0) {
		return false;
	}
	if (r[0x17] != 0xFF && (r[0x17] & 0x01u) == 0) {
		out->timeslot = (r[0x17] & 0x02u) ? 2 : 1;
	}
	return true;
}

bool codeplug_listen_list(uint16_t index, listen_list_t *out)
{
	uint8_t n;
	uint8_t r[LIST_SIZE];
	if (source != CODEPLUG_LEGACY || index < 1 || index > CODEPLUG_LISTS_MAX ||
			!flash_read(LIST_COUNTS + (index - 1u), &n, 1) || n == 0 || n == 0xFF ||
			!flash_read(LIST_BASE + (index - 1u) * LIST_SIZE, r, sizeof(r))) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	copy_name(out->name, r);
	unsigned members = n - 1u;
	if (members > CODEPLUG_LIST_LEN) {
		members = CODEPLUG_LIST_LEN;
	}
	for (unsigned i = 0; i < members; i++) {
		uint16_t c = le16(&r[ZONE_NAME_LEN + 2u * i]);
		if (c == 0 || c > CODEPLUG_CONTACTS_MAX) {
			break;
		}
		out->contacts[out->count++] = c;
	}
	return true;
}
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	enum { PER_READ = 32 };	// records per flash read (768 bytes on the stack)
	uint8_t r[PER_READ * CONTACT_SIZE];

	if (source != CODEPLUG_LEGACY || id == 0 || id > 0xFFFFFFu) {
		return false;
	}
	for (unsigned first = 0; first < CODEPLUG_CONTACTS_MAX; first += PER_READ) {
		if (!flash_read(CONTACT_BASE + first * CONTACT_SIZE, r, sizeof(r))) {
			return false;
		}
		for (unsigned i = 0; i < PER_READ; i++) {
			const uint8_t *c = &r[i * CONTACT_SIZE];
			uint8_t type = c[0x14];
			if (c[0] == 0xFF || type > CONTACT_ALL_CALL || (type == CONTACT_PRIVATE) == group) {
				continue;
			}
			uint32_t cid = type == CONTACT_ALL_CALL ? 0xFFFFFFu : bcd_id(&c[0x10]);
			if (cid == id) {
				return codeplug_contact((uint16_t)(first + i + 1u), out);
			}
		}
	}
	return false;
}

bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	uint8_t g[0x1A];

	memset(out, 0, sizeof(*out));
	if (source != CODEPLUG_LEGACY || !flash_read(GEN_BASE, g, sizeof(g))) {
		return false;
	}
	unsigned n = 0;
	while (n < CODEPLUG_RADIO_NAME_LEN && g[n] >= 0x20 && g[n] < 0x7F) {
		out->radio_name[n] = (char)g[n];
		n++;
	}
	while (n > 0 && out->radio_name[n - 1] == ' ') {
		n--;
	}
	out->radio_name[n] = '\0';
	if (g[GEN_GROUP_HANG] != 0xFF) {
		out->group_hang_ms = (uint16_t)(g[GEN_GROUP_HANG] * GEN_HANG_UNIT_MS);
	}
	if (g[GEN_PRIV_HANG] != 0xFF) {
		out->private_hang_ms = (uint16_t)(g[GEN_PRIV_HANG] * GEN_HANG_UNIT_MS);
	}
	return true;
}
#endif
