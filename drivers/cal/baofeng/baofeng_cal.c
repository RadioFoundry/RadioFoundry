// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Baofeng HR_C7000 factory calibration: TX I/Q center, modulation balance and PA drive (docs/spec/hardware-dm32.md 11).
// The DM-32UV and the DM-1801 Pro keep it in the same page with the same field offsets (hardware-dm1801pro.md 6.4).
//
// The factory data is the 4 KB page of the stock codeplug area (0x1000..0xC8000) whose last byte is 2. Three pieces
// of it make a 0x175-byte working image: 0x108 bytes from its start, 0x15 from +0x200 and 0x50 (four tables of five BCD
// frequencies) from +0x300. It's only read: this firmware never writes below 0x700000. Without a factory page, the
// decoded copy OpenDM32 keeps at the board's copy address (0x710000 on the DM-32UV) is used when its header is right.
#include <string.h>
#include "baofeng_cal.h"

#define CAL_PAGE_FIRST		0x1000u
#define CAL_PAGE_END		0xC9000u
#define CAL_PAGE_SIZE		0x1000u
#define CAL_PAGE_MARKER		2u		// logical block ID in the last byte of the page

// Offsets in the working image (11.2)
#define OFF_HEADER		0x000u
#define OFF_TX_CENTER		0x016u
#define OFF_UHF_MOD		0x017u
#define OFF_UHF_POWER		0x040u		// low, mid, high: 5 points each
#define OFF_VHF_MOD		0x0A6u
#define OFF_VHF_POWER		0x0D0u
#define OFF_FREQ_TABLES		0x11Du		// 4 tables x 5 x u32 LE (10 Hz units once decoded)
#define OFF_K_VHF		0x16Du
#define OFF_K_UHF		0x171u

#define FALLBACK_DAC		1408u
#define MODCAL_NONE		0x4D4Du

static const uint8_t header[8] = { 0x00, 0x40, 0x00, 0x47, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t image[BF_CAL_LEN];
static bool valid;
static const char *source = "none";

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// Eight BCD digits, most significant nibble first
static uint32_t bcd8(uint32_t v)
{
	uint32_t r = 0;
	for (int i = 0; i < 8; i++) {
		r = r * 10u + (v >> 28);
		v <<= 4;
	}
	return r;
}

bool bf_cal_parse(const uint8_t *sector, uint32_t sector_len, uint8_t out[BF_CAL_LEN])
{
	if (sector == NULL || sector_len < 0x300u + 0x50u) {
		return false;
	}
	memcpy(out + 0x000, sector + 0x000, 0x108);
	memcpy(out + 0x108, sector + 0x200, 0x015);
	memcpy(out + 0x11D, sector + 0x300, 0x050);
	for (int i = 0; i < 20; i++) {
		uint8_t *p = out + OFF_FREQ_TABLES + 4 * i;
		wr32(p, bcd8(rd32(p)));
	}
	// K = mean over the five points of freq_MHz x modulation point: table 2 with the VHF points, table 0 with the UHF
	uint32_t kv = 0, ku = 0;
	for (int i = 0; i < 5; i++) {
		kv += (rd32(out + OFF_FREQ_TABLES + 2 * 20 + 4 * i) / 100000u) * out[OFF_VHF_MOD + i];
		ku += (rd32(out + OFF_FREQ_TABLES + 0 * 20 + 4 * i) / 100000u) * out[OFF_UHF_MOD + i];
	}
	wr32(out + OFF_K_VHF, kv / 5u);
	wr32(out + OFF_K_UHF, ku / 5u);
	return true;
}

void bf_cal_set_image(const uint8_t in[BF_CAL_LEN], bool ok)
{
	memcpy(image, in, BF_CAL_LEN);
	valid = ok;
}

// The three pieces of the page into a buffer laid out as the page (only the first 0x350 bytes), then decoded
static bool read_factory(const hal_flash_t *f, uint32_t page)
{
	static uint8_t sector[0x350];
	if (!f->read(page + 0x000, sector + 0x000, 0x108) || !f->read(page + 0x200, sector + 0x200, 0x015) ||
		!f->read(page + 0x300, sector + 0x300, 0x050)) {
		return false;
	}
	return bf_cal_parse(sector, sizeof sector, image);
}

bool bf_cal_load(const hal_flash_t *flash, uint32_t copy_addr)
{
	valid = false;
	source = "none";
	if (flash == NULL || flash->size == 0) {
		return false;
	}
	for (uint32_t a = CAL_PAGE_FIRST; a < CAL_PAGE_END; a += CAL_PAGE_SIZE) {
		uint8_t id = 0;
		if (flash->read(a + CAL_PAGE_SIZE - 1u, &id, 1) && id == CAL_PAGE_MARKER) {
			if (read_factory(flash, a)) {
				valid = true;
				source = "factory";
			}
			break;
		}
	}
	if (!valid && copy_addr != 0 && flash->read(copy_addr, image, BF_CAL_LEN) &&
		memcmp(image + OFF_HEADER, header, sizeof header) == 0) {
		valid = true;
		source = "copy";
	}
	return valid;
}

bool bf_cal_valid(void)
{
	return valid;
}

const char *bf_cal_source(void)
{
	return source;
}

uint8_t bf_cal_tx_center(void)
{
	return valid ? image[OFF_TX_CENTER] : 0x80u;
}

uint8_t bf_cal_byte(uint16_t offset)
{
	return offset < BF_CAL_LEN ? image[offset] : 0;
}

uint16_t bf_cal_modcal(uint32_t hz)
{
	uint32_t f = hz / 1000000u;
	if (!valid || f == 0) {
		return MODCAL_NONE;
	}
	uint32_t k = rd32(image + (hz > 299999990u ? OFF_K_UHF : OFF_K_VHF));
	if (k == 0) {
		return MODCAL_NONE;
	}
	uint32_t b = (2u * k + f) / (2u * f);	// K / f rounded
	if (b > 0xFFu) {
		b = 0xFFu;
	}
	return (uint16_t)(b << 8 | b);
}

// Point idx of a five-point row, linearly towards the next (at the last point: 2 v4 - v3), clamped 0..4096
static uint16_t interp(const uint8_t *t, uint32_t idx, uint32_t rem, uint32_t step)
{
	int32_t lo = (int32_t)t[idx] << 4;
	int32_t hi = idx < 4 ? (int32_t)t[idx + 1] << 4 : 2 * lo - ((int32_t)t[idx - 1] << 4);
	int32_t v = lo + (hi - lo) * (int32_t)rem / (int32_t)step;
	return (uint16_t)(v < 0 ? 0 : v > 4096 ? 4096 : v);
}

bool bf_cal_power(uint32_t hz, bf_cal_power_points_t *p)
{
	if (!valid) {
		return false;
	}
	uint32_t f = hz / 10u;	// 10 Hz units, as the tables
	if (f < 13600000u) {
		p->low = (uint16_t)(image[OFF_VHF_POWER + 0] << 4);
		p->mid = (uint16_t)(image[OFF_VHF_POWER + 5] << 4);
		p->high = (uint16_t)(image[OFF_VHF_POWER + 10] << 4);
	} else {
		bool uhf = f > 28000000u;
		uint32_t base = uhf ? 40000000u : 13600000u;
		uint32_t step = uhf ? 1750000u : 950000u;
		const uint8_t *t = image + (uhf ? OFF_UHF_POWER : OFF_VHF_POWER);
		// Below the UHF base the first point is used (OpenDM32's unsigned subtraction picked the last: 11.3)
		uint32_t d = f > base ? f - base : 0;
		uint32_t idx = d / step;
		uint32_t rem = d % step;
		if (idx > 4) {
			idx = 4;
			rem = d - 4u * step;
			if (rem > step) {
				rem = step;
			}
		}
		p->low = interp(t, idx, rem, step);
		p->mid = interp(t + 5, idx, rem, step);
		p->high = interp(t + 10, idx, rem, step);
	}
	p->very_low = (uint16_t)(p->low * 9u / 10u);
	return true;
}

// 11.3 fractions f1..f5 in thousandths (f0 = 0.90 is very_low), per band
static const uint16_t frac[3][5] = {
	{ 410, 760, 340, 600, 810 },	// VHF
	{ 420, 750, 310, 550, 780 },	// 220 MHz
	{ 430, 740, 270, 510, 750 },	// UHF
};

uint16_t bf_cal_power_level(uint32_t hz, uint8_t level)
{
	bf_cal_power_points_t p;
	if (!bf_cal_power(hz, &p)) {
		return FALLBACK_DAC;
	}
	const uint16_t *fr = frac[hz < 200000000u ? 0 : hz < 300000000u ? 1 : 2];
	int32_t l = p.low, m = p.mid, h = p.high, v;
	switch (level) {
	case 0: v = p.very_low; break;
	case 1: v = l; break;
	case 2: v = l + (m - l) * fr[0] / 1000; break;
	case 3: v = l + (m - l) * fr[1] / 1000; break;
	case 4: v = m; break;
	case 5: v = m + (h - m) * fr[2] / 1000; break;
	case 6: v = m + (h - m) * fr[3] / 1000; break;
	case 7: v = m + (h - m) * fr[4] / 1000; break;
	default: v = h; break;
	}
	return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}
