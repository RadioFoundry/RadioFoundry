// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 factory calibration reader (docs/spec/hardware-md380.md section 11).
// The radio is single band: the table in SPI flash security registers 1-2 (512 bytes) holds one band. The reader is
// read only, checks every row before use, and the radio refuses to transmit when the table is not accepted.
#include <string.h>
#include "md380.h"
#include "radio/cal_curve.h"

#define CAL_SIZE	512u
#define POINTS		9u

// Row offsets (hardware-md380.md 11.2)
#define REF_TUNE	0x009u	// reference oscillator tune byte
#define ROW_HIGH	0x010u	// TX power, high (5 W)
#define ROW_LOW		0x020u	// TX power, low (1 W)
#define ROW_RX_TUNE	0x030u	// receiver front-end tuning voltage
#define ROW_DMR_I	0x130u
#define ROW_DMR_Q	0x140u
#define ROW_FM_I	0x170u
#define ROW_FM_Q	0x180u
#define ROW_FIRST	0x010u	// rows whose odd points are filled in: 0x010-0x180 but 0x0E0
#define ROW_LAST	0x180u
#define ROW_NONE	0x0E0u
#define BAND_LO		0x1B2u	// band ends, 4 BCD digits in 100 kHz units, least significant byte first
#define BAND_HI		0x1F2u

// Without a table: a low APC reference (a guess, and TX is refused anyway), gains at mid scale, the receiver tuned
// to mid band, no reference correction
#define SAFE_DAC	384u
#define SAFE_RX_DAC	2048u
#define SAFE_GAIN	0x80u
#define SAFE_REF	0x80u

// Band ends (100 kHz units) used when the table's are unreadable, and the range they must fall in
#if RFOUNDRY_BAND_VHF
#define DEFAULT_LO	1360u
#define DEFAULT_HI	1740u
#define BAND_MIN	1300u
#define BAND_MAX	1800u
#else
#define DEFAULT_LO	4000u
#define DEFAULT_HI	4700u
#define BAND_MIN	3900u
#define BAND_MAX	4900u
#endif

typedef enum { CAL_NONE, CAL_FACTORY } cal_source_t;

static uint8_t table[CAL_SIZE];
static uint32_t points[POINTS];	// calibration points, 100 kHz units
static bool points_from_table;
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

// 2 BCD bytes, least significant first; 0 if a nibble is not a digit
static uint32_t bcd16(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 1; i >= 0; i--) {
		uint8_t hi = p[i] >> 4, lo = p[i] & 0xFu;
		if (hi > 9 || lo > 9) {
			return 0;
		}
		v = v * 100u + hi * 10u + lo;
	}
	return v;
}

// The points split the band by halving: the ends, the middle, then the middles between, so they are only as even as
// integer halves allow. The frequency list's other entries are not used
static void load_points(void)
{
	uint32_t lo = bcd16(&table[BAND_LO]), hi = bcd16(&table[BAND_HI]);
	points_from_table = lo >= BAND_MIN && hi <= BAND_MAX && lo < hi;
	if (!points_from_table) {
		lo = DEFAULT_LO;
		hi = DEFAULT_HI;
	}
	points[0] = lo;
	points[POINTS - 1] = hi;
	for (uint32_t step = (POINTS - 1) / 2; step > 0; step /= 2) {
		for (uint32_t i = step; i < POINTS - 1; i += 2 * step) {
			points[i] = (points[i - step] + points[i + step]) / 2u;
		}
	}
}

// Only the even points of a row are used: each odd one is the mean of its neighbors, rounded down, whatever the flash
// holds there
static void fill_odd_points(void)
{
	for (uint32_t row = ROW_FIRST; row <= ROW_LAST; row += 0x10u) {
		if (row == ROW_NONE) {
			continue;
		}
		for (uint32_t i = 1; i < POINTS; i += 2) {
			table[row + i] = (uint8_t)((table[row + i - 1] + table[row + i + 1]) / 2u);
		}
	}
}

// Usable when the power rows are neither erased nor zero and low < high at every point, and no gain or tuning row is
// erased (a gain row may be all zero: one radio's factory table has one)
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
	if (!stm32_spi_flash_read_security(1, 0, &table[0], 256) || !stm32_spi_flash_read_security(2, 0, &table[256], 256)) {
		why = "read failed";
		memset(table, 0xFF, sizeof(table));
		load_points();
		return false;
	}
	fill_odd_points();
	load_points();
	if (table_sane()) {
		source = CAL_FACTORY;
	}
	return source != CAL_NONE;
}

// Linear over the points (radio/cal_curve.h): clamped below the first and above the last, truncated toward the
// lower point's value. f in 100 kHz units, truncated
static uint8_t byte_at(uint16_t row, uint32_t f)
{
	int32_t v[POINTS];
	for (uint32_t i = 0; i < POINTS; i++) {
		v[i] = table[row + i];
	}
	return (uint8_t)cal_curve_at(points, v, POINTS, f, 0);	// between two bytes, so a byte
}

// The byte x 255 in the 12-bit DAC's left-aligned register: (b x 255) >> 4 counts
static uint16_t dac_at(uint16_t row, uint32_t f)
{
	return (uint16_t)((byte_at(row, f) * 255u) >> 4);
}

bool md380_cal_at(uint32_t hz, md380_cal_point_t *out)
{
	uint32_t f = hz / 100000u;
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
	out->power_dac[0] = dac_at(ROW_LOW, f);
	out->power_dac[1] = dac_at(ROW_HIGH, f);
	out->rx_tune_dac = dac_at(ROW_RX_TUNE, f);
	out->fm_i = byte_at(ROW_FM_I, f);
	out->fm_q = byte_at(ROW_FM_Q, f);
	out->dmr_i = byte_at(ROW_DMR_I, f);
	out->dmr_q = byte_at(ROW_DMR_Q, f);
	out->ref_tune = table[REF_TUNE];
	return true;
}

const char *md380_cal_source(void)
{
	return source == CAL_FACTORY ? (points_from_table ? "factory" : "factory/grid") : "none";
}

const char *md380_cal_why(void)
{
	return why;
}

uint8_t md380_cal_byte(uint16_t offset)
{
	return offset < CAL_SIZE ? table[offset] : 0;
}
