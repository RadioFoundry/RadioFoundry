// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 factory calibration and hardware option block (cal_md9600.h, docs/spec/hardware-md9600.md 11). Both blocks
// are kept whole as read (drivers/cal/calblock), so the bytes nobody has decoded yet are there for the diagnostics and
// for a later layout; only the rows the spec lists are read, each only when it isn't blank. Transmit stays refused
// (radio_md9600.c) until the PA rows are checked on a radio, whatever they hold.
#include <string.h>
#include "md9600.h"
#include "cal_md9600.h"
#include "cal/calblock/calblock.h"

#define SECURITY_REG(n)		((uint32_t)(n) << 12)
#define UHF_FIRST_HZ		400000000u
#define UHF_STEP_HZ		10000000u
#define UHF_POINTS		9u
#define VHF_FIRST_HZ		136000000u
#define VHF_STEP_HZ		9500000u
#define VHF_POINTS		5u
#define SAFE_RX_DAC		2048u
#define CURVE			(CAL_CURVE_ROUND | CAL_CURVE_EXTEND)

// Fixed offsets (11.2)
#define OFF_RSSI_M120		0x04u
#define OFF_RSSI_M70		0x05u
#define OFF_REF_UHF		0x09u
#define OFF_REF_VHF		0x0Cu
#define OPT_EDGES		0x14u	// four halfwords
#define OPT_FLAGS		0x1Du
#define OPT_BACKLIGHT_SWITCHED	0x10u

// First byte of each row, UHF and VHF (11.2)
static const uint16_t row_offset[MD9600_CAL_ROW_COUNT][2] = {
	[MD9600_CAL_RX_TUNE] = { 0x030, 0x039 },
	[MD9600_CAL_POWER_LOW] = { 0x020, 0x029 },
	[MD9600_CAL_POWER_MID_LOW] = { 0x1A0, 0x1A9 },
	[MD9600_CAL_POWER_MID] = { 0x190, 0x199 },
	[MD9600_CAL_POWER_HIGH] = { 0x010, 0x019 },
	[MD9600_CAL_SQ9_OPEN] = { 0x040, 0x0E0 },
	[MD9600_CAL_SQ9_CLOSE] = { 0x050, 0x0E5 },
	[MD9600_CAL_SQ1_OPEN] = { 0x060, 0x0EA },
	[MD9600_CAL_SQ1_CLOSE] = { 0x070, 0x0EF },
	[MD9600_CAL_CTCSS_67] = { 0x090, 0x09B },
	[MD9600_CAL_CTCSS_151] = { 0x0A0, 0x0AB },
	[MD9600_CAL_CTCSS_254] = { 0x0B0, 0x0BB },
	[MD9600_CAL_DCS] = { 0x0D0, 0x0DB },
};

static uint8_t cal_bytes[MD9600_CAL_SIZE];
static uint8_t opt_bytes[MD9600_OPT_SIZE];
static calblock_t cal = { .data = cal_bytes, .size = MD9600_CAL_SIZE };
static calblock_t opt = { .data = opt_bytes, .size = MD9600_OPT_SIZE };

static bool read_security(uint32_t addr, void *buf, uint32_t len)
{
	return md9600_flash_read_security((uint8_t)(addr >> 12), addr & 0xFFFu, buf, len);
}

void md9600_cal_load(void)
{
	static const calblock_part_t cal_parts[] = {
		{ SECURITY_REG(1), 256 },
		{ SECURITY_REG(2), 256 },
	};
	static const calblock_part_t opt_parts[] = { { SECURITY_REG(3), MD9600_OPT_SIZE } };
	calblock_load(&cal, read_security, cal_parts, 2);
	calblock_load(&opt, read_security, opt_parts, 1);
}

void md9600_cal_set(const uint8_t *c, const uint8_t *o)
{
	cal.state = opt.state = CALBLOCK_READ_FAILED;
	if (c != NULL) {
		memcpy(cal_bytes, c, sizeof cal_bytes);
		calblock_check(&cal);
	}
	if (o != NULL) {
		memcpy(opt_bytes, o, sizeof opt_bytes);
		calblock_check(&opt);
	}
}

const char *md9600_cal_source(void)
{
	return calblock_state_name(&cal);
}

const char *md9600_opt_source(void)
{
	return calblock_state_name(&opt);
}

bool md9600_cal_valid(void)
{
	return calblock_ok(&cal);
}

bool md9600_cal_row(md9600_cal_row_t row, uint32_t hz, int32_t *value)
{
	if ((unsigned)row >= MD9600_CAL_ROW_COUNT) {
		return false;
	}
	bool vhf = hz < MD9600_BAND_SPLIT_HZ;
	calblock_row_t r = {
		.offset = row_offset[row][vhf ? 1 : 0],
		.points = vhf ? VHF_POINTS : UHF_POINTS,
		.flags = CURVE,
		.first_hz = vhf ? VHF_FIRST_HZ : UHF_FIRST_HZ,
		.step_hz = vhf ? VHF_STEP_HZ : UHF_STEP_HZ,
	};
	return calblock_row_at(&cal, &r, hz, value);
}

uint16_t md9600_cal_rx_tune(uint32_t hz)
{
	int32_t v;
	if (!md9600_cal_row(MD9600_CAL_RX_TUNE, hz, &v)) {
		return SAFE_RX_DAC;
	}
	v *= 16;	// DAC value = byte x 16
	return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}

bool md9600_cal_ref_tune(bool uhf, int8_t *out)
{
	uint8_t b = calblock_byte(&cal, uhf ? OFF_REF_UHF : OFF_REF_VHF);
	if (!calblock_ok(&cal) || b == 0xFFu || b == 0x00u) {
		return false;
	}
	*out = (int8_t)((int)b - 128);
	return true;
}

bool md9600_cal_rssi(uint8_t *at_m120, uint8_t *at_m70)
{
	uint8_t lo = calblock_byte(&cal, OFF_RSSI_M120), hi = calblock_byte(&cal, OFF_RSSI_M70);
	if (!calblock_ok(&cal) || lo == 0xFFu || hi == 0xFFu || lo == hi) {
		return false;
	}
	*at_m120 = lo;
	*at_m70 = hi;
	return true;
}

uint8_t md9600_cal_byte(uint16_t offset)
{
	return calblock_byte(&cal, offset);
}

uint8_t md9600_opt_byte(uint16_t offset)
{
	return calblock_byte(&opt, offset);
}

bool md9600_opt_band_edges(md9600_band_edges_t *e)
{
	if (!calblock_ok(&opt)) {
		return false;
	}
	uint32_t vhf[2], uhf[2];
	uint8_t nv = 0, nu = 0;
	for (uint8_t i = 0; i < 4; i++) {
		uint32_t mhz10;
		if (!calblock_bcd(calblock_u16le(&opt, (uint16_t)(OPT_EDGES + 2u * i)), 4, &mhz10) || mhz10 == 0) {
			return false;
		}
		uint32_t hz = mhz10 * 100000u;
		if (hz < MD9600_BAND_SPLIT_HZ) {
			if (nv == 2) {
				return false;
			}
			vhf[nv++] = hz;
		} else {
			if (nu == 2) {
				return false;
			}
			uhf[nu++] = hz;
		}
	}
	if (vhf[0] == vhf[1] || uhf[0] == uhf[1]) {
		return false;
	}
	e->vhf_lo_hz = vhf[0] < vhf[1] ? vhf[0] : vhf[1];
	e->vhf_hi_hz = vhf[0] < vhf[1] ? vhf[1] : vhf[0];
	e->uhf_lo_hz = uhf[0] < uhf[1] ? uhf[0] : uhf[1];
	e->uhf_hi_hz = uhf[0] < uhf[1] ? uhf[1] : uhf[0];
	return true;
}

bool md9600_opt_backlight_switched(void)
{
	return calblock_ok(&opt) && (calblock_byte(&opt, OPT_FLAGS) & OPT_BACKLIGHT_SWITCHED) != 0;
}
