// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 factory calibration reader (docs/spec/hardware-md380.md section 11).
// The radio is single band: the table in SPI flash security registers 1-2 (512 bytes) holds one band. The row layout
// is assumed to be the MD-UV380 family's (the same factory tool) and is not confirmed on an MD-380, so the reader is
// read only, checks every row before use, and the radio refuses to transmit when the table is not accepted.
#include <string.h>
#include "md380.h"

#define CAL_SIZE	512u
#define POINTS		9u

// Row offsets (hardware-md380.md 11.2, all ?)
#define REF_TUNE	0x009u	// reference oscillator tune byte
#define ROW_HIGH	0x010u	// TX power, high (5 W)
#define ROW_LOW		0x020u	// TX power, low (1 W)
#define ROW_RX_TUNE	0x030u	// receiver front-end tuning voltage
#define ROW_DMR_I	0x130u
#define ROW_DMR_Q	0x140u
#define ROW_FM_I	0x170u
#define ROW_FM_Q	0x180u
#define FREQ_LIST	0x1B0u	// 9 calibration frequencies, 4-byte BCD, least significant byte first, 10 Hz units

// Without a table: a low APC reference (a guess, and TX is refused anyway), gains at mid scale, the receiver tuned
// to mid band, no reference correction
#define SAFE_DAC	384u
#define SAFE_RX_DAC	2048u
#define SAFE_GAIN	0x80u
#define SAFE_REF	0x80u

// Used when the frequency list is unreadable: the band split in eight equal steps
#if MD380_VHF
#define GRID_FIRST	13600000u	// 10 Hz units
#define GRID_STEP	475000u
#else
#define GRID_FIRST	40000000u
#define GRID_STEP	1000000u
#endif

typedef enum { CAL_NONE, CAL_FACTORY } cal_source_t;

static uint8_t table[CAL_SIZE];
static uint32_t freq10[POINTS];	// calibration points, 10 Hz units
static bool freq_from_table;
static cal_source_t source;
static const char *why = "not read";

static bool all_same(const uint8_t *p, uint32_t n, uint8_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] != v) {
			return false;
		}
	}
	return true;
}

// 4 BCD bytes, least significant first; 0 if a nibble is not a digit
static uint32_t bcd32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 3; i >= 0; i--) {
		uint8_t hi = p[i] >> 4, lo = p[i] & 0xFu;
		if (hi > 9 || lo > 9) {
			return 0;
		}
		v = v * 100u + hi * 10u + lo;
	}
	return v;
}

static void load_frequencies(void)
{
	uint32_t prev = 0;
	freq_from_table = true;
	for (uint32_t i = 0; i < POINTS; i++) {
		uint32_t f = bcd32(&table[FREQ_LIST + i * 4u]);
#if MD380_VHF
		bool in_band = f >= 13000000u && f <= 18000000u;
#else
		bool in_band = f >= 39000000u && f <= 49000000u;
#endif
		if (!in_band || f <= prev) {
			freq_from_table = false;
			break;
		}
		freq10[i] = prev = f;
	}
	if (!freq_from_table) {
		for (uint32_t i = 0; i < POINTS; i++) {
			freq10[i] = GRID_FIRST + i * GRID_STEP;
		}
	}
}

// Usable when the power rows are neither erased nor zero and low < high at every point, and no gain or tuning row is
// erased (a gain row may be all zero: the DM-1701's factory table has one)
static bool table_sane(void)
{
	const uint8_t *low = &table[ROW_LOW], *high = &table[ROW_HIGH];
	if (all_same(low, POINTS, 0xFF) || all_same(high, POINTS, 0xFF)) {
		why = "power erased";
		return false;
	}
	for (uint32_t i = 0; i < POINTS; i++) {
		if (low[i] == 0 || high[i] == 0) {
			why = "power zero";
			return false;
		}
		if (low[i] >= high[i]) {
			why = "power order";
			return false;
		}
	}
	static const uint16_t rows[] = { ROW_RX_TUNE, ROW_DMR_I, ROW_DMR_Q, ROW_FM_I, ROW_FM_Q };
	for (uint32_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
		if (all_same(&table[rows[r]], POINTS, 0xFF)) {
			why = "row erased";
			return false;
		}
	}
	why = "ok";
	return true;
}

bool md380_cal_load(void)
{
	source = CAL_NONE;
	if (!md380_flash_read_security(1, 0, &table[0], 256) || !md380_flash_read_security(2, 0, &table[256], 256)) {
		why = "read failed";
		memset(table, 0xFF, sizeof(table));
		load_frequencies();
		return false;
	}
	load_frequencies();
	if (table_sane()) {
		source = CAL_FACTORY;
	}
	return source != CAL_NONE;
}

// Linear interpolation over the points: clamped below the first and above the last
static int32_t interpolate(uint16_t row, uint32_t f10, int32_t mul)
{
	const uint8_t *p = &table[row];
	if (f10 <= freq10[0]) {
		return p[0] * mul;
	}
	if (f10 >= freq10[POINTS - 1]) {
		return p[POINTS - 1] * mul;
	}
	uint32_t i = 0;
	while (i < POINTS - 2 && f10 >= freq10[i + 1]) {
		i++;
	}
	int32_t a = p[i] * mul, b = p[i + 1] * mul;
	int32_t span = (int32_t)(freq10[i + 1] - freq10[i]);
	int32_t d = (int32_t)(f10 - freq10[i]);
	return a + (int32_t)((int64_t)(b - a) * d / span);
}

static uint16_t dac_at(uint16_t row, uint32_t f10)
{
	int32_t v = interpolate(row, f10, 16);	// 8-bit table value to the 12-bit DAC (?)
	return (uint16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
}

static uint8_t byte_at(uint16_t row, uint32_t f10)
{
	int32_t v = interpolate(row, f10, 1);
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

bool md380_cal_at(uint32_t hz, md380_cal_point_t *out)
{
	uint32_t f10 = hz / 10u;
	if (source == CAL_NONE) {
		*out = (md380_cal_point_t){
			.valid = false,
			.power_dac = { SAFE_DAC, SAFE_DAC },
			.rx_tune_dac = SAFE_RX_DAC,
			.fm_i = SAFE_GAIN, .fm_q = SAFE_GAIN, .dmr_i = SAFE_GAIN, .dmr_q = SAFE_GAIN,
			.ref_tune = SAFE_REF,
		};
		return false;
	}
	out->valid = true;
	out->power_dac[0] = dac_at(ROW_LOW, f10);
	out->power_dac[1] = dac_at(ROW_HIGH, f10);
	out->rx_tune_dac = dac_at(ROW_RX_TUNE, f10);
	out->fm_i = byte_at(ROW_FM_I, f10);
	out->fm_q = byte_at(ROW_FM_Q, f10);
	out->dmr_i = byte_at(ROW_DMR_I, f10);
	out->dmr_q = byte_at(ROW_DMR_Q, f10);
	out->ref_tune = table[REF_TUNE];
	return true;
}

const char *md380_cal_source(void)
{
	return source == CAL_FACTORY ? (freq_from_table ? "factory" : "factory/grid") : "none";
}

const char *md380_cal_why(void)
{
	return why;
}

uint8_t md380_cal_byte(uint16_t offset)
{
	return offset < CAL_SIZE ? table[offset] : 0;
}
