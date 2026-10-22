// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Codeplug: an open-firmware codeplug in the board's data flash (docs/spec/open-firmware-cps.md 7,
// docs/spec/hardware-dm1701.md 13.2), or a small built-in set of simplex FM channels when there is none.
// Records are read from flash on demand; RAM holds only an in-use bitmap of the channels and the used zone slots.
// Edits rewrite records through the sector journal (core/storage) and keep every byte the model doesn't own.
#include <string.h>
#include "codeplug.h"
#include "board.h"
#include "storage/storage.h"

// Where the two open-firmware images sit in the data flash when the board gives no map: the STM32 family puts the 64 KB
// EEPROM image at 0 and the flash image at +128 KB
#define DEFAULT_EEPROM_BASE	0x000000u
#define DEFAULT_FLASH_BASE	0x020000u

static uint32_t ee_base = DEFAULT_EEPROM_BASE;
static uint32_t fl_base = DEFAULT_FLASH_BASE;

#define EE(a)		(ee_base + (a))
#define FL(a)		(fl_base + (a))

// Channels (7.3.3): bank 0 = channels 1-128 in the EEPROM image, banks 1-7 = 129-1024 in the flash image.
// Every bank is a 16-byte in-use bitmap followed by 128 records of 56 bytes
#define CH_MAX		CODEPLUG_CHANNELS_MAX
#define CH_PER_BANK	128u
#define CH_BANKS	(CH_MAX / CH_PER_BANK)
#define CH_SIZE		CODEPLUG_OPENFW_CHANNEL_SIZE
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
#define FLAG4_VOX	0x40u
#define FLAG4_ZONE_SKIP	0x20u	// zone scan skip
#define FLAG4_RX_ONLY	0x04u
#define FLAG4_25K	0x02u
#define LFLAG1_ID	0x80u	// per-channel DMR ID present
#define LFLAG1_NO_BEEP	0x40u
#define LFLAG1_NO_ECO	0x20u	// no power saving on this channel
#define LFLAG1_DMO	0x04u	// force direct mode
#define LFLAG1_ROAM	0x01u	// roaming (read only: the encoder keeps the bit as read)

// General settings (7.3.2): the callsign (radio name) and the radio's DMR ID, 8 BCD digits big endian
#define GEN_CALLSIGN	EE(0x00E0u)
#define GEN_NAME_LEN	8u
#define QUICK_KEYS	EE(0x7524u)	// CODEPLUG_QUICK_KEYS x 2 bytes, LE (open-firmware-cps.md 7.2)
#define BOOT_TEXT	EE(0x7540u)	// two 16 byte lines (open-firmware-cps.md 7, CPS section 2)
#define GEN_BASE	GEN_CALLSIGN	// the general settings block
#define GEN_DMR_ID	EE(0x00E8u)
#define GEN_GROUP_HANG	0x18u	// offsets in the block
#define GEN_PRIV_HANG	0x19u
#define GEN_HANG_UNIT_MS 500u	// assumed, not confirmed: no unit in the interop spec, the ref notes or the vendor
				// guide (docs/spec/dmr-voice.md 8); check with the vendor CPS on the bench

// Contacts (7.3.5) and RX group lists (7.3.6), flash image
#define CONTACT_BASE	FL(0x87620u)
#define CONTACT_SIZE	24u
#define LIST_COUNTS	FL(0x8D620u)
#define LIST_BASE	FL(0x8D6A0u)
#define LIST_SIZE	80u

// Zones (7.2, 7.3.4)
#define ZONE_SLOTS	CODEPLUG_ZONES_MAX
#define ZONE_BITMAP	EE(0x8010u)
#define ZONE_DATA	EE(0x8030u)
#define ZONE_LAYOUT	EE(0x806Fu)	// 0x00-0x04: 80 channels per zone, else the old 16-channel layout
#define ZONE_NAME_LEN	16u
#define ZONE_RECORD_80	(ZONE_NAME_LEN + 2u * 80u)

static codeplug_source_t source = CODEPLUG_NONE;
static uint32_t serial;

// Open-firmware codeplug index
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

static bool have_flash(void)
{
	return board.flash != NULL && board.flash->size != 0;
}

static bool flash_read(uint32_t addr, void *buf, uint32_t len)
{
	return have_flash() && board.flash->read(addr, buf, len);
}

static bool flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	return storage_write(addr, buf, len);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
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

static uint32_t to_bcd(uint32_t v, uint8_t digits)
{
	uint32_t w = 0;
	for (uint8_t i = 0; i < digits; i++) {
		w |= (v % 10u) << (4u * i);
		v /= 10u;
	}
	return w;
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

static void put_freq(uint8_t *p, uint32_t hz)
{
	uint32_t w = to_bcd(hz / 10u, 8);
	p[0] = (uint8_t)w;
	p[1] = (uint8_t)(w >> 8);
	p[2] = (uint8_t)(w >> 16);
	p[3] = (uint8_t)(w >> 24);
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

static uint16_t tone_word(const subtone_t *t)
{
	switch (t->type) {
	case TONE_CTCSS:
		return (uint16_t)to_bcd(t->value, 4);
	case TONE_DCS:
	case TONE_DCS_INV: {
		// Octal digits written as hex digits
		uint16_t v = (uint16_t)(((t->value >> 6) & 7u) << 8 | ((t->value >> 3) & 7u) << 4 | (t->value & 7u));
		return (uint16_t)(v | 0x8000u | (t->type == TONE_DCS_INV ? 0x4000u : 0));
	}
	default:
		return 0xFFFFu;
	}
}

static void copy_name(char *dst, const uint8_t *src, unsigned len)
{
	unsigned n = 0;
	while (n < len && src[n] != 0x00 && src[n] != 0xFF) {
		dst[n] = (char)src[n];
		n++;
	}
	dst[n] = '\0';
}

// A name field: the text, then 0xFF padding
static void put_name(uint8_t *dst, const char *src, unsigned len)
{
	unsigned n = 0;
	memset(dst, 0xFF, len);
	while (n < len && src[n] != '\0') {
		dst[n] = (uint8_t)src[n];
		n++;
	}
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

static void set_used(uint16_t number, bool on)
{
	uint8_t bit = (uint8_t)(1u << ((number - 1u) % 8u));
	if (on) {
		used[(number - 1u) / 8u] |= bit;
	} else {
		used[(number - 1u) / 8u] &= (uint8_t)~bit;
	}
}

static void find_max(void)
{
	channel_max = 0;
	for (uint16_t n = CH_MAX; n >= 1; n--) {
		if (is_used(n)) {
			channel_max = n;
			break;
		}
	}
}

// In-use bitmaps, then the frequencies of every marked record: an erased bank (all 0xFF) fails the BCD check
static void index_channels(void)
{
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
				set_used(number, false);
			}
		}
	}
	find_max();
}

static void index_zones(void)
{
	uint8_t layout, bits[(ZONE_SLOTS + 7u) / 8u];
	zone_count = 0;
	zone_channels = 80;
	zone_record = ZONE_RECORD_80;
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

static codeplug_source_t index_all(void)
{
	const hal_flash_map_t *m = have_flash() ? board.flash->map : NULL;

	ee_base = m != NULL ? m->openfw_eeprom : DEFAULT_EEPROM_BASE;
	fl_base = m != NULL ? m->openfw_flash : DEFAULT_FLASH_BASE;
	memset(used, 0, sizeof(used));
	channel_max = 0;
	zone_count = 0;
	index_builtin();
	if (have_flash()) {
		index_channels();
	}
	if (channel_max > 0) {
		index_zones();
		source = CODEPLUG_OPENFW;
	} else {
		source = CODEPLUG_BUILTIN;
	}
	return source;
}

codeplug_source_t codeplug_init(void)
{
	storage_lock();
	codeplug_source_t s = index_all();
	storage_unlock();
	return s;
}

codeplug_source_t codeplug_reload(void)
{
	storage_lock();
	codeplug_source_t s = index_all();
	serial++;
	storage_unlock();
	return s;
}

codeplug_source_t codeplug_source(void)
{
	return source;
}

uint32_t codeplug_serial(void)
{
	return serial;
}

uint16_t codeplug_channel_count(void)
{
	switch (source) {
	case CODEPLUG_OPENFW:
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

// The power byte is qdmr's 10-step scale (1 min ... 10 max, 7.x). A radio with fewer levels gets the scale spread over
// them, so a 2-level radio runs Min and Low at its low level and Mid and above at its high one. Radios with 9 or more
// levels read it as level n - 1, clamped to the top level
static uint8_t power_from_byte(uint8_t n)
{
	uint8_t count = board.power_count;
	uint8_t step = (uint8_t)(n - 1u);
	if (count == 0 || count >= 9u) {
		return count != 0 && step >= count ? (uint8_t)(count - 1u) : step;
	}
	if (step > 9u) {
		step = 9u;
	}
	return (uint8_t)((step * (count - 1u) + 4u) / 9u);
}

static uint8_t power_to_byte(uint8_t level)
{
	uint8_t count = board.power_count;
	if (count < 2u || count >= 9u) {
		return (uint8_t)(level + 1u);
	}
	return (uint8_t)((level * 9u + (count - 1u) / 2u) / (count - 1u) + 1u);
}

bool codeplug_openfw_decode(const uint8_t *r, channel_t *out)
{
	memset(out, 0, sizeof(*out));
	if (!freq(&r[CH_RX], &out->rx_hz) || !freq(&r[CH_TX], &out->tx_hz)) {
		return false;
	}
	copy_name(out->name, &r[CH_NAME], CODEPLUG_NAME_LEN);
	out->mode = r[CH_MODE] == 1 ? RADIO_MODE_DMR : RADIO_MODE_FM;
	out->bw = (r[CH_FLAG4] & FLAG4_25K) ? RADIO_BW_25K : RADIO_BW_12K5;
	out->rx_tone = tone(le16(&r[CH_RX_TONE]));
	out->tx_tone = tone(le16(&r[CH_TX_TONE]));
	out->power = r[CH_POWER] == 0 ? CODEPLUG_POWER_DEFAULT : power_from_byte(r[CH_POWER]);
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
	out->scan_skip = (r[CH_FLAG4] & FLAG4_ZONE_SKIP) != 0;
	out->vox = (r[CH_FLAG4] & FLAG4_VOX) != 0;
	out->no_beep = (r[CH_LFLAG1] & LFLAG1_NO_BEEP) != 0;
	out->no_eco = (r[CH_LFLAG1] & LFLAG1_NO_ECO) != 0;
	out->roam = (r[CH_LFLAG1] & LFLAG1_ROAM) != 0;
	out->alias_tx = (uint8_t)(((r[CH_FLAG1] & 0x02u) ? CODEPLUG_ALIAS_TS1_TEXT : 0u) |
				  ((r[CH_FLAG1] & 0x08u) ? CODEPLUG_ALIAS_TS2_TEXT : 0u));
	return true;
}

// A fresh record: what a CPS writes for a channel with nothing set (7.3.3)
static void blank_channel(uint8_t *r)
{
	memset(r, 0, CH_SIZE);
	memset(&r[CH_NAME], 0xFF, CODEPLUG_NAME_LEN);
	put16(&r[CH_RX_TONE], 0xFFFFu);
	put16(&r[CH_TX_TONE], 0xFFFFu);
	r[CH_DMR_ID + 1] = 0x16;	// the vendor defaults these bytes replaced
}

void codeplug_openfw_encode(const channel_t *ch, uint8_t *r)
{
	uint32_t rx;
	if (!freq(&r[CH_RX], &rx)) {
		blank_channel(r);	// not a record yet (erased or junk): start from the blank one
	}
	put_name(&r[CH_NAME], ch->name, CODEPLUG_NAME_LEN);
	put_freq(&r[CH_RX], ch->rx_hz);
	put_freq(&r[CH_TX], ch->tx_hz);
	r[CH_MODE] = ch->mode == RADIO_MODE_DMR ? 1 : 0;
	r[CH_POWER] = ch->power == CODEPLUG_POWER_DEFAULT ? 0 : power_to_byte(ch->power);
	put16(&r[CH_RX_TONE], tone_word(&ch->rx_tone));
	put16(&r[CH_TX_TONE], tone_word(&ch->tx_tone));
	r[CH_FLAG4] = (uint8_t)((r[CH_FLAG4] & ~(FLAG4_RX_ONLY | FLAG4_25K)) | (ch->rx_only ? FLAG4_RX_ONLY : 0) |
				(ch->bw == RADIO_BW_25K ? FLAG4_25K : 0));
	r[CH_FLAG2] = (uint8_t)((r[CH_FLAG2] & ~FLAG2_TS2) | (ch->timeslot == 2 ? FLAG2_TS2 : 0));
	r[CH_CC] = ch->color_code & 0x0Fu;
	put16(&r[CH_CONTACT], ch->contact);
	r[CH_RX_GROUP] = (uint8_t)(ch->listen_list <= 0xFFu ? ch->listen_list : 0);
	r[CH_LFLAG1] = (uint8_t)((r[CH_LFLAG1] & ~(LFLAG1_ID | LFLAG1_DMO)) | (ch->dmr_id != 0 ? LFLAG1_ID : 0) |
				 (ch->dmo ? LFLAG1_DMO : 0));
	uint32_t tot = (ch->tot_s + 14u) / 15u;
	r[CH_TOT] = (uint8_t)(tot > 255u ? 255u : tot);
	// The text bit of each timeslot's talker alias field; the APRS bit is kept
	r[CH_FLAG1] = (uint8_t)((r[CH_FLAG1] & ~0x0Au) | ((ch->alias_tx & CODEPLUG_ALIAS_TS1_TEXT) ? 0x02u : 0u) |
				((ch->alias_tx & CODEPLUG_ALIAS_TS2_TEXT) ? 0x08u : 0u));
	if (ch->dmr_id != 0) {
		r[CH_DMR_ID] = (uint8_t)(ch->dmr_id >> 16);
		r[CH_DMR_ID + 1] = (uint8_t)(ch->dmr_id >> 8);
		r[CH_DMR_ID + 2] = (uint8_t)ch->dmr_id;
	} else {
		r[CH_DMR_ID] = 0x00;
		r[CH_DMR_ID + 1] = 0x16;
		r[CH_DMR_ID + 2] = 0x00;
	}
}

static uint32_t ch_errors;	// a used channel that couldn't be read or decoded (codeplug_channel_errors)

static bool openfw_channel(uint16_t number, channel_t *out)
{
	uint8_t r[CH_SIZE];
	if (!is_used(number)) {
		return false;
	}
	if (!flash_read(channel_addr(number), r, sizeof(r)) || !codeplug_openfw_decode(r, out)) {
		ch_errors++;
		return false;
	}
	return true;
}

uint32_t codeplug_channel_errors(void)
{
	return ch_errors;
}

bool codeplug_channel(uint16_t number, channel_t *out)
{
	bool ok;
	storage_lock();
	switch (source) {
	case CODEPLUG_OPENFW:
		ok = openfw_channel(number, out);
		break;
	case CODEPLUG_BUILTIN:
		ok = builtin_channel(number, out);
		break;
	default:
		ok = false;
		break;
	}
	storage_unlock();
	return ok;
}

uint16_t codeplug_zone_count(void)
{
	if (source == CODEPLUG_NONE) {
		return 0;
	}
	return zone_count > 0 ? zone_count : 1;
}

bool codeplug_zones_stored(void)
{
	return source == CODEPLUG_OPENFW && zone_count > 0;
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
	bool ok = true;
	storage_lock();
	if (index >= codeplug_zone_count()) {
		storage_unlock();
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (zone_count == 0) {
		all_channels(out);
	} else {
		uint8_t r[ZONE_RECORD_80];
		ok = flash_read(ZONE_DATA + zone_slot[index] * zone_record, r, zone_record);
		if (ok) {
			copy_name(out->name, r, ZONE_NAME_LEN);
			for (unsigned i = 0; i < zone_channels && out->count < CODEPLUG_ZONE_MAX; i++) {
				uint16_t n = le16(&r[ZONE_NAME_LEN + 2u * i]);
				if (n == 0) {
					break;
				}
				if (is_used(n)) {
					out->channels[out->count++] = n;	// skip references to empty or broken records
				}
			}
		}
	}
	storage_unlock();
	return ok;
}

// --- Editing ---

// A codeplug to edit: the built-in set first becomes a stored one (its channels written, the bitmaps, zones and
// lists that may hold junk from other firmware cleared)
static bool materialize(void)
{
	if (source == CODEPLUG_OPENFW) {
		return true;
	}
	if (source != CODEPLUG_BUILTIN || !storage_ready()) {
		return false;
	}
	static const uint8_t zero[32];
	bool ok = true;
	for (unsigned bank = 0; bank < CH_BANKS && ok; bank++) {
		ok = flash_write(bank_bitmap(bank), zero, CH_PER_BANK / 8u);
	}
	ok = ok && flash_write(ZONE_BITMAP, zero, sizeof(zero));
#if CONFIG_DMR
	for (unsigned off = 0; off < CODEPLUG_LISTS_MAX && ok; off += sizeof(zero)) {
		uint32_t n = CODEPLUG_LISTS_MAX - off < sizeof(zero) ? CODEPLUG_LISTS_MAX - off : sizeof(zero);
		ok = flash_write(LIST_COUNTS + off, zero, n);
	}
#endif
	memset(used, 0, sizeof(used));
	channel_max = 0;
	zone_count = 0;
	zone_channels = 80;
	zone_record = ZONE_RECORD_80;
	source = CODEPLUG_OPENFW;
	for (uint16_t n = 1; n <= builtin_count && ok; n++) {
		channel_t ch;
		builtin_channel(n, &ch);
		ok = codeplug_set_channel(n, &ch);
	}
	if (!ok) {
		index_all();
	}
	return ok;
}

static bool write_bitmap(uint16_t number)
{
	unsigned bank = (number - 1u) / CH_PER_BANK;
	return flash_write(bank_bitmap(bank), &used[bank * (CH_PER_BANK / 8u)], CH_PER_BANK / 8u);
}

uint16_t codeplug_free_channel(void)
{
	uint16_t n;
	storage_lock();
	for (n = 1; n <= CH_MAX && (source == CODEPLUG_BUILTIN ? n <= builtin_count : is_used(n)); n++) {
	}
	storage_unlock();
	return n <= CH_MAX ? n : 0;
}

bool codeplug_set_channel(uint16_t number, const channel_t *ch)
{
	uint8_t r[CH_SIZE];
	if (number < 1 || number > CH_MAX || ch->rx_hz == 0 || ch->tx_hz == 0 || ch->rx_hz >= 1000000000u ||
	    ch->tx_hz >= 1000000000u) {
		return false;
	}
	storage_lock();
	bool ok = materialize() && flash_read(channel_addr(number), r, sizeof(r));
	if (ok) {
		codeplug_openfw_encode(ch, r);
		ok = flash_write(channel_addr(number), r, sizeof(r));
	}
	if (ok && !is_used(number)) {
		set_used(number, true);		// the record first, then its in-use bit
		ok = write_bitmap(number);
		if (!ok) {
			set_used(number, false);
		}
	}
	if (ok) {
		find_max();
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_delete_channel(uint16_t number)
{
	if (number < 1 || number > CH_MAX) {
		return false;
	}
	storage_lock();
	bool ok = materialize();
	if (ok && is_used(number)) {
		set_used(number, false);
		ok = write_bitmap(number);
		if (!ok) {
			set_used(number, true);
		}
	}
	if (ok) {
		find_max();
		serial++;
	}
	storage_unlock();
	return ok;
}

static bool write_zone_bitmap(uint8_t slot, bool on)
{
	uint8_t byte;
	uint32_t a = ZONE_BITMAP + slot / 8u;
	if (!flash_read(a, &byte, 1)) {
		return false;
	}
	byte = on ? (uint8_t)(byte | (1u << (slot % 8u))) : (uint8_t)(byte & ~(1u << (slot % 8u)));
	return flash_write(a, &byte, 1);
}

bool codeplug_set_zone(uint16_t index, const zone_t *z)
{
	uint8_t r[ZONE_RECORD_80];
	if (z->count > CODEPLUG_ZONE_MAX || z->name[0] == '\0') {
		return false;
	}
	storage_lock();
	bool ok = materialize();
	// Every channel must exist
	for (uint16_t i = 0; ok && i < z->count; i++) {
		ok = is_used(z->channels[i]);
	}
	// The old 16-channel layout holds at most 16 per zone; switching layouts would need every zone rewritten
	if (ok && zone_channels != 80 && zone_count > 0 && z->count > zone_channels) {
		ok = false;
	}
	uint8_t slot = 0;
	bool append = zone_count == 0 || index >= zone_count;
	if (ok && zone_count == 0) {
		zone_channels = 80;		// a fresh zone table: the 80-channel layout (slot 0's padding selects it)
		zone_record = ZONE_RECORD_80;
	}
	if (ok && append) {
		if (index != zone_count) {
			ok = false;
		}
		bool taken[ZONE_SLOTS] = { false };
		for (uint8_t i = 0; i < zone_count; i++) {
			taken[zone_slot[i]] = true;
		}
		for (slot = 0; slot < ZONE_SLOTS && taken[slot]; slot++) {
		}
		ok = ok && slot < ZONE_SLOTS;
	} else if (ok) {
		slot = zone_slot[index];
	}
	if (ok) {
		memset(r, 0, sizeof(r));
		put_name(r, z->name, ZONE_NAME_LEN);
		for (uint16_t i = 0; i < z->count && i < zone_channels; i++) {
			put16(&r[ZONE_NAME_LEN + 2u * i], z->channels[i]);
		}
		ok = flash_write(ZONE_DATA + slot * zone_record, r, zone_record) && write_zone_bitmap(slot, true);
	}
	if (ok) {
		index_zones();
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_delete_zone(uint16_t index)
{
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && index < zone_count && write_zone_bitmap(zone_slot[index], false);
	if (ok) {
		index_zones();
		serial++;
	}
	storage_unlock();
	return ok;
}

void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1])
{
	uint8_t b[GEN_NAME_LEN];
	out[0] = '\0';
	storage_lock();
	if (source == CODEPLUG_OPENFW && flash_read(GEN_CALLSIGN, b, sizeof(b))) {
		copy_name(out, b, GEN_NAME_LEN);
	}
	storage_unlock();
}

bool codeplug_quick_keys(uint16_t out[CODEPLUG_QUICK_KEYS])
{
	uint8_t b[CODEPLUG_QUICK_KEYS * 2u];
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && flash_read(QUICK_KEYS, b, sizeof(b));
	storage_unlock();
	for (unsigned i = 0; i < CODEPLUG_QUICK_KEYS; i++) {
		out[i] = ok ? le16(&b[2u * i]) : 0;
	}
	return ok;
}

bool codeplug_set_quick_keys(const uint16_t in[CODEPLUG_QUICK_KEYS])
{
	uint8_t b[CODEPLUG_QUICK_KEYS * 2u];
	for (unsigned i = 0; i < CODEPLUG_QUICK_KEYS; i++) {
		put16(&b[2u * i], in[i]);
	}
	storage_lock();
	bool ok = materialize() && flash_write(QUICK_KEYS, b, sizeof(b));
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

void codeplug_boot_text(char line1[CODEPLUG_NAME_LEN + 1], char line2[CODEPLUG_NAME_LEN + 1])
{
	uint8_t b[2u * CODEPLUG_NAME_LEN];
	line1[0] = line2[0] = '\0';
	storage_lock();
	if (source == CODEPLUG_OPENFW && flash_read(BOOT_TEXT, b, sizeof(b))) {
		copy_name(line1, b, CODEPLUG_NAME_LEN);
		copy_name(line2, b + CODEPLUG_NAME_LEN, CODEPLUG_NAME_LEN);
	}
	storage_unlock();
}

#if CONFIG_APRS
// APRS configuration records (open-firmware-cps.md 7.3.8)
#define APRS_BASE	EE(0x1588u)
#define APRS_SIZE	0x40u
#define APRS_MAGIC	0x4153u	// 0x4152: the older layout, ambiguity one bit lower

static void aprs_path(char *out, const uint8_t *p)
{
	char call[7];
	copy_name(call, p, 6);
	unsigned n = 0;
	while (call[n] != '\0' && call[n] != ' ') {
		n++;
	}
	call[n] = '\0';
	out[0] = '\0';
	if (n == 0) {
		return;
	}
	memcpy(out, call, n);
	if (p[6] >= 1 && p[6] <= 15) {
		out[n++] = '-';
		if (p[6] >= 10) {
			out[n++] = '1';
		}
		out[n++] = (char)('0' + p[6] % 10u);
	}
	out[n] = '\0';
}

bool codeplug_aprs(uint8_t n, codeplug_aprs_t *out)
{
	uint8_t b[APRS_SIZE];
	memset(out, 0, sizeof(*out));
	if (n < 1 || n > CODEPLUG_APRS_CONFIGS) {
		return false;
	}
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && flash_read(APRS_BASE + (uint32_t)(n - 1u) * APRS_SIZE, b, sizeof(b));
	storage_unlock();
	uint16_t magic = le16(&b[0x3E]);
	if (!ok || b[0] == 0xFF || b[0] == 0x00 || (magic != APRS_MAGIC && magic != APRS_MAGIC - 1u)) {
		return false;
	}
	out->ssid = b[0x08] <= 15 ? b[0x08] : 0;
	aprs_path(out->path[0], &b[0x0F]);
	aprs_path(out->path[1], &b[0x16]);
	out->sym_table = (char)b[0x1D];
	out->sym_code = (char)b[0x1E];
	if (out->sym_code < '!' || out->sym_code > '~') {
		out->sym_table = out->sym_code = 0;
	}
	copy_name(out->comment, &b[0x1F], CODEPLUG_APRS_COMMENT);
	uint8_t amb = magic == APRS_MAGIC ? (uint8_t)(b[0x3D] >> 5) : (uint8_t)((b[0x3D] >> 4) & 7u);
	out->ambiguity = amb <= 4 ? amb : 4;
	return true;
}

// One path entry from "CALL-N" (or "" for none): the callsign space padded, then the SSID
static void put_aprs_path(uint8_t *p, const char *text)
{
	unsigned n = 0;
	memset(p, ' ', 6);
	p[6] = 0;
	while (n < 6 && text[n] != '\0' && text[n] != '-') {
		p[n] = (uint8_t)text[n];
		n++;
	}
	if (text[n] == '-') {
		unsigned ssid = 0;
		for (const char *d = &text[n + 1u]; *d >= '0' && *d <= '9'; d++) {
			ssid = ssid * 10u + (unsigned)(*d - '0');
		}
		p[6] = ssid <= 15 ? (uint8_t)ssid : 0;
	}
}

bool codeplug_set_aprs_path(uint8_t n, const char *path1, const char *path2)
{
	uint8_t b[APRS_SIZE];
	if (n < 1 || n > CODEPLUG_APRS_CONFIGS) {
		return false;
	}
	uint32_t addr = APRS_BASE + (uint32_t)(n - 1u) * APRS_SIZE;
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && flash_read(addr, b, sizeof(b));
	uint16_t magic = le16(&b[0x3E]);
	ok = ok && b[0] != 0xFF && b[0] != 0x00 && (magic == APRS_MAGIC || magic == APRS_MAGIC - 1u);
	if (ok) {
		put_aprs_path(&b[0x0F], path1);
		put_aprs_path(&b[0x16], path2);
		ok = flash_write(addr + 0x0Fu, &b[0x0F], 14);
	}
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}
#endif

bool codeplug_set_radio_name(const char *name)
{
	uint8_t b[GEN_NAME_LEN];
	put_name(b, name, GEN_NAME_LEN);
	storage_lock();
	bool ok = materialize() && flash_write(GEN_CALLSIGN, b, sizeof(b));
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
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

static void put_bcd_id(uint8_t *p, uint32_t id)
{
	uint32_t w = to_bcd(id, 8);
	p[0] = (uint8_t)(w >> 24);
	p[1] = (uint8_t)(w >> 16);
	p[2] = (uint8_t)(w >> 8);
	p[3] = (uint8_t)w;
}

uint32_t codeplug_dmr_id(void)
{
	uint8_t b[4];
	uint32_t id = 0;
	storage_lock();
	if (source == CODEPLUG_OPENFW && flash_read(GEN_DMR_ID, b, sizeof(b))) {
		id = bcd_id(b);
	}
	storage_unlock();
	return id;
}

bool codeplug_set_dmr_id(uint32_t id)
{
	uint8_t b[4];
	if (id > 0xFFFFFFu) {
		return false;
	}
	if (id == 0) {
		memset(b, 0xFF, sizeof(b));
	} else {
		put_bcd_id(b, id);
	}
	storage_lock();
	bool ok = materialize() && flash_write(GEN_DMR_ID, b, sizeof(b));
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_contact(uint16_t index, contact_t *out)
{
	uint8_t r[CONTACT_SIZE];
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && index >= 1 && index <= CODEPLUG_CONTACTS_MAX &&
		  flash_read(CONTACT_BASE + (index - 1u) * CONTACT_SIZE, r, sizeof(r));
	storage_unlock();
	if (!ok || r[0] == 0xFF || r[0x14] > CONTACT_ALL_CALL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	copy_name(out->name, r, CODEPLUG_NAME_LEN);
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

bool codeplug_set_contact(uint16_t index, const contact_t *c)
{
	uint8_t r[CONTACT_SIZE];
	if (index < 1 || index > CODEPLUG_CONTACTS_MAX || c->type > CONTACT_ALL_CALL || c->name[0] == '\0' ||
	    (c->type != CONTACT_ALL_CALL && (c->id == 0 || c->id > 0xFFFFFFu)) || c->timeslot > 2) {
		return false;
	}
	put_name(r, c->name, CODEPLUG_NAME_LEN);
	put_bcd_id(&r[0x10], c->type == CONTACT_ALL_CALL ? 0xFFFFFFu : c->id);
	r[0x14] = c->type;
	r[0x15] = 0;
	r[0x16] = 0;
	r[0x17] = c->timeslot == 0 ? 0x01u : (uint8_t)((c->timeslot - 1u) << 1);
	storage_lock();
	bool ok = materialize() && flash_write(CONTACT_BASE + (index - 1u) * CONTACT_SIZE, r, sizeof(r));
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_delete_contact(uint16_t index)
{
	uint8_t r[CONTACT_SIZE];
	if (index < 1 || index > CODEPLUG_CONTACTS_MAX) {
		return false;
	}
	memset(r, 0xFF, sizeof(r));
	storage_lock();
	bool ok = materialize() && flash_write(CONTACT_BASE + (index - 1u) * CONTACT_SIZE, r, sizeof(r));
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_listen_list(uint16_t index, listen_list_t *out)
{
	uint8_t n;
	uint8_t r[LIST_SIZE];
	storage_lock();
	bool ok = source == CODEPLUG_OPENFW && index >= 1 && index <= CODEPLUG_LISTS_MAX &&
		  flash_read(LIST_COUNTS + (index - 1u), &n, 1) && n != 0 && n != 0xFF &&
		  flash_read(LIST_BASE + (index - 1u) * LIST_SIZE, r, sizeof(r));
	storage_unlock();
	if (!ok) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	copy_name(out->name, r, CODEPLUG_NAME_LEN);
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

bool codeplug_set_listen_list(uint16_t index, const listen_list_t *l)
{
	uint8_t r[LIST_SIZE];
	if (index < 1 || index > CODEPLUG_LISTS_MAX || l->count > CODEPLUG_LIST_LEN || l->name[0] == '\0') {
		return false;
	}
	memset(r, 0, sizeof(r));
	put_name(r, l->name, CODEPLUG_NAME_LEN);
	for (unsigned i = 0; i < l->count; i++) {
		if (l->contacts[i] == 0 || l->contacts[i] > CODEPLUG_CONTACTS_MAX) {
			return false;
		}
		put16(&r[ZONE_NAME_LEN + 2u * i], l->contacts[i]);
	}
	uint8_t n = (uint8_t)(l->count + 1u);
	storage_lock();
	// The record first, then the count that makes it visible
	bool ok = materialize() && flash_write(LIST_BASE + (index - 1u) * LIST_SIZE, r, sizeof(r)) &&
		  flash_write(LIST_COUNTS + (index - 1u), &n, 1);
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_delete_listen_list(uint16_t index)
{
	uint8_t n = 0;
	if (index < 1 || index > CODEPLUG_LISTS_MAX) {
		return false;
	}
	storage_lock();
	bool ok = materialize() && flash_write(LIST_COUNTS + (index - 1u), &n, 1);
	if (ok) {
		serial++;
	}
	storage_unlock();
	return ok;
}

bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	enum { PER_READ = 32 };	// records per flash read
	// Static, used only under the storage lock: on the stack it overflowed the radio task, whose DMR set-up calls
	// this through find_name (a DMR VFO whose talk target is a contact, at boot)
	static uint8_t r[PER_READ * CONTACT_SIZE];

	if (source != CODEPLUG_OPENFW || id == 0 || id > 0xFFFFFFu) {
		return false;
	}
	// Locked like every other reader: the radio task and the UI task both search, and an edit may be writing
	bool found = false, done = false;
	storage_lock();
	for (unsigned first = 0; first < CODEPLUG_CONTACTS_MAX && !done; first += PER_READ) {
		if (!flash_read(CONTACT_BASE + first * CONTACT_SIZE, r, sizeof(r))) {
			break;
		}
		for (unsigned i = 0; i < PER_READ; i++) {
			const uint8_t *c = &r[i * CONTACT_SIZE];
			uint8_t type = c[0x14];
			if (c[0] == 0xFF || type > CONTACT_ALL_CALL || (type == CONTACT_PRIVATE) == group) {
				continue;
			}
			uint32_t cid = type == CONTACT_ALL_CALL ? 0xFFFFFFu : bcd_id(&c[0x10]);
			if (cid == id) {
				found = codeplug_contact((uint16_t)(first + i + 1u), out);	// the lock is recursive
				done = true;
				break;
			}
		}
	}
	storage_unlock();
	return found;
}

bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	uint8_t g[0x1A];

	memset(out, 0, sizeof(*out));
	if (source != CODEPLUG_OPENFW || !flash_read(GEN_BASE, g, sizeof(g))) {
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
