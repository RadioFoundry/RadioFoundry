// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DM-32UV's real-time clock (hardware-dm32.md 3.3): the SoC's RTC on its internal I2C2 (address 0x70), run by a
// 32.768 kHz crystal and kept through power-off by a coin cell (measured: it held the time for about three hours with
// the main battery out, docs/spec/hardware-dm32.md 3.3). It gives hal_rtc_t its UTC Unix time as a day count (days
// since 1970-01-01) and hours, minutes and seconds, all in binary. Nothing else in the firmware uses I2C2, and a caller
// must be the only one at a time (today the debug link task alone). Every wait is bounded, so a missing answer is a
// false return and never a hang.
#include "config.h"

#if CONFIG_RTC
#include <stdbool.h>
#include <stdint.h>
#include "dm32.h"
#include "platform.h"

#define I2C2_BASE	0x14080000u
#define I2C_CON		REG32(I2C2_BASE + 0x00)
#define I2C_TAR		REG32(I2C2_BASE + 0x04)
#define I2C_DATA_CMD	REG32(I2C2_BASE + 0x10)
#define I2C_FS_HCNT	REG32(I2C2_BASE + 0x1C)
#define I2C_FS_LCNT	REG32(I2C2_BASE + 0x20)
#define I2C_INTR_MASK	REG32(I2C2_BASE + 0x30)
#define I2C_RX_TL	REG32(I2C2_BASE + 0x38)
#define I2C_TX_TL	REG32(I2C2_BASE + 0x3C)
#define I2C_CLR_INTR	REG32(I2C2_BASE + 0x40)
#define I2C_ENABLE	REG32(I2C2_BASE + 0x6C)
#define I2C_STATUS	REG32(I2C2_BASE + 0x70)
#define I2C_ENABLE_ST	REG32(I2C2_BASE + 0x9C)
#define I2C_START	REG32(I2C2_BASE + 0xA0)

#define RTC_ADDR	0x70u
#define SPIN_MAX	200000u		// loop turns per wait: a few milliseconds, then give up

// RTC registers
#define R_SEC		0x00u		// current seconds, minutes, hours (6, 6, 5 bits), day count low and high byte
#define R_MIN		0x01u
#define R_HOUR		0x02u
#define R_DAY_L		0x03u
#define R_DAY_H		0x04u
#define R_LOAD_SEC	0x0Cu		// the time to load, the same order
#define R_LOAD_MIN	0x0Du
#define R_LOAD_HOUR	0x0Eu
#define R_LOAD_DAY_L	0x0Fu
#define R_LOAD_DAY_H	0x10u
#define R_CCR		0x11u		// bit 0 interrupt enable, bit 1 run, bit 2 load (clears itself)
#define R_VSTAT		0x12u		// bit 0: the loaded time is valid
#define CCR_RUN		0x02u

static bool wait_status(uint32_t mask, uint32_t want)
{
	for (uint32_t n = 0; n < SPIN_MAX; n++) {
		if ((I2C_STATUS & mask) == want) {
			return true;
		}
	}
	return false;
}

static bool wait_disabled(void)
{
	for (uint32_t n = 0; n < SPIN_MAX; n++) {
		if ((I2C_ENABLE_ST & 1u) == 0) {
			return true;
		}
	}
	return false;
}

// The controller's set-up (hardware-dm32.md 3.3), written each time: it is a few register writes, and nothing else
// owns the controller
static void bus_setup(void)
{
	I2C_ENABLE = 0;
	I2C_CON = 0x65u;
	I2C_INTR_MASK = 0x40u;
	I2C_FS_HCNT = 0x61u;
	I2C_FS_LCNT = 0x68u;
	I2C_TX_TL = 6;
	I2C_RX_TL = 6;
}

static bool reg_read(uint8_t reg, uint8_t *v)
{
	for (unsigned g = 0; g < 64u && (I2C_STATUS & 0x08u); g++) {	// drain the RX FIFO (bounded by its depth)
		(void)I2C_DATA_CMD;
	}
	I2C_ENABLE = 0;
	(void)I2C_CLR_INTR;
	I2C_TAR = RTC_ADDR;
	I2C_ENABLE = 1;
	I2C_DATA_CMD = reg;
	I2C_DATA_CMD = 0x100u;			// read
	I2C_START = 1;
	bool ok = wait_status(0x08u, 0x08u);	// RX FIFO not empty
	if (ok) {
		*v = (uint8_t)(I2C_DATA_CMD & 0xFFu);
	}
	I2C_START = 0;
	I2C_ENABLE = 0;
	return wait_disabled() && ok;
}

static bool reg_write(uint8_t reg, uint8_t v)
{
	I2C_ENABLE = 0;
	(void)I2C_CLR_INTR;
	I2C_TAR = RTC_ADDR;
	I2C_ENABLE = 1;
	I2C_DATA_CMD = reg;
	I2C_DATA_CMD = v;
	I2C_START = 1;
	bool ok = wait_status(0x05u, 0x04u);
	I2C_START = 0;
	I2C_ENABLE = 0;
	return wait_disabled() && ok;
}

// seconds, minutes, hours, day low, day high: read together, and again when the seconds moved between the reads (a
// carry into the minute would otherwise show a time up to a minute off)
static bool read_time(uint8_t t[5])
{
	for (unsigned tries = 0; tries < 3; tries++) {
		uint8_t again;
		for (unsigned i = 0; i < 5; i++) {
			if (!reg_read((uint8_t)(R_SEC + i), &t[i])) {
				return false;
			}
		}
		if (!reg_read(R_SEC, &again)) {
			return false;
		}
		if (again == t[0]) {
			return true;
		}
	}
	return false;
}

static bool rtc_read(int64_t *unix_s)
{
	uint8_t ccr, vstat, t[5];
	bus_setup();
	if (!reg_read(R_VSTAT, &vstat) || !reg_read(R_CCR, &ccr) || !(vstat & 1u) || !(ccr & CCR_RUN)) {
		return false;		// never loaded, or lost: no time
	}
	if (!read_time(t)) {
		return false;
	}
	uint8_t sec = t[0] & 0x3Fu, min = t[1] & 0x3Fu, hour = t[2] & 0x1Fu;
	if (sec > 59 || min > 59 || hour > 23) {
		return false;
	}
	*unix_s = ((int64_t)(t[3] | ((uint16_t)t[4] << 8))) * 86400 + hour * 3600 + min * 60 + sec;
	return true;
}

// The load order of the hardware notes: valid 0, the time into the load registers, CCR (interrupt enable, run, load),
// valid 1. The soft reset the datasheet lists for a first power-up is left out: it was not needed on the radio
static bool rtc_set(int64_t unix_s)
{
	if (unix_s < 0 || unix_s / 86400 > 0xFFFF) {
		return false;
	}
	uint32_t day = (uint32_t)(unix_s / 86400), rem = (uint32_t)(unix_s % 86400);
	bus_setup();
	return reg_write(R_VSTAT, 0) && reg_write(R_LOAD_SEC, (uint8_t)(rem % 60)) &&
	       reg_write(R_LOAD_MIN, (uint8_t)(rem / 60 % 60)) && reg_write(R_LOAD_HOUR, (uint8_t)(rem / 3600)) &&
	       reg_write(R_LOAD_DAY_L, (uint8_t)(day & 0xFFu)) && reg_write(R_LOAD_DAY_H, (uint8_t)(day >> 8)) &&
	       reg_write(R_CCR, 0x07u) && reg_write(R_VSTAT, 1);
}

const hal_rtc_t dm32_rtc = {
	.read = rtc_read,
	.set = rtc_set,
};

#endif
