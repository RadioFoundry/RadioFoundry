// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 factory calibration: the data flash's last sector (docs/spec/hardware-rt10.md 4). Byte tables of 15 entries,
// one per 10 MHz step from a base the sector names (90: 900-909 MHz is step 0). The stock firmware reads them at every
// channel change; here the entries are copied into RAM once, at start-up, and nothing is ever written back.
#include <string.h>
#include "rt10.h"

#define CAL_SECTOR		0x1FF000u
#define OFF_TX_CENTER		0x1A9u		// TCXO center byte (the modem's SIG_CENTER)
#define OFF_BASE		0x470u		// the first step's frequency / 10 MHz
#define OFF_DAC_HIGH		0x51Du		// PA control level / 16, the stock's two power sets
#define OFF_DAC_LOW		0x52Cu
#define OFF_MOD			0x559u		// (u8, u8) pairs, the first byte: the modulation balance
#define OFF_PA_HIGH		0x5B5u		// FD6818 PA gain; the stock caps the high set's at 7
#define OFF_PA_LOW		0x5C4u
#define OFF_ON_HIGH		0x5D3u		// key-up delay / 10 us, RX supply off to TX state
#define OFF_ON_LOW		0x5F1u
#define OFF_OFF			0x600u		// key-down delay / 10 us, before the ramp down

#define PA_GAIN_MAX		7u

enum { T_DAC_HIGH, T_DAC_LOW, T_PA_HIGH, T_PA_LOW, T_ON_HIGH, T_ON_LOW, T_OFF, T_COUNT };
static const uint16_t table_off[T_COUNT] = {
	OFF_DAC_HIGH, OFF_DAC_LOW, OFF_PA_HIGH, OFF_PA_LOW, OFF_ON_HIGH, OFF_ON_LOW, OFF_OFF,
};

static uint8_t tables[T_COUNT][RT10_CAL_STEPS];
static uint8_t mod[RT10_CAL_STEPS];
static uint8_t base, tx_center;
static bool valid;

bool rt10_cal_load(rt10_cal_read_fn read)
{
	valid = false;
	if (!read(CAL_SECTOR + OFF_BASE, &base, 1) || !read(CAL_SECTOR + OFF_TX_CENTER, &tx_center, 1)) {
		return false;
	}
	for (unsigned t = 0; t < T_COUNT; t++) {
		if (!read(CAL_SECTOR + table_off[t], tables[t], RT10_CAL_STEPS)) {
			return false;
		}
	}
	for (unsigned i = 0; i < RT10_CAL_STEPS; i++) {
		if (!read(CAL_SECTOR + OFF_MOD + 2u * i, &mod[i], 1)) {
			return false;
		}
	}
	// An erased sector (or another radio's layout) has no base in the band
	valid = base != 0xFFu && base >= RT10_CAL_BASE_MIN && base <= RT10_CAL_BASE_MAX;
	return valid;
}

bool rt10_cal_valid(void)
{
	return valid;
}

uint8_t rt10_cal_step(uint32_t hz)
{
	uint32_t s = hz / 10000000u;
	if (s < base) {
		return 0;
	}
	s -= base;
	return (uint8_t)(s > RT10_CAL_STEPS - 1u ? RT10_CAL_STEPS - 1u : s);
}

bool rt10_cal_tx_plan(uint32_t hz, bool high, rt10_tx_plan_t *p)
{
	if (!valid) {
		return false;
	}
	unsigned s = rt10_cal_step(hz);
	uint8_t dac = tables[high ? T_DAC_HIGH : T_DAC_LOW][s];
	uint8_t pa = tables[high ? T_PA_HIGH : T_PA_LOW][s];
	if (dac == 0xFFu || pa == 0xFFu) {
		return false;	// a step the factory didn't fill
	}
	p->dac = (uint16_t)(dac << 4);
	p->pa_gain = high && pa > PA_GAIN_MAX ? (uint8_t)PA_GAIN_MAX : (uint8_t)(pa & PA_GAIN_MAX);
	uint8_t on = tables[high ? T_ON_HIGH : T_ON_LOW][s], off = tables[T_OFF][s];
	p->on_us = on == 0xFFu ? 0 : (uint16_t)(on * 10u);
	p->off_us = off == 0xFFu ? 0 : (uint16_t)(off * 10u);
	return true;
}

uint8_t rt10_cal_tx_center(void)
{
	return valid && tx_center != 0xFFu ? tx_center : RT10_TX_CENTER_DEFAULT;
}

uint8_t rt10_cal_mod(uint32_t hz)
{
	uint8_t m = valid ? mod[rt10_cal_step(hz)] : 0xFFu;
	return m == 0xFFu ? 0 : m;
}
