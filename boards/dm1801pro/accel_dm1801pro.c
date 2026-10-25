// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-1801 Pro accelerometer: a LIS3DH-class sensor (address 0x19) on a bit-banged I2C bus on PTB2 / PTB23
// (hardware-dm1801pro.md 8). Nothing in the radio uses motion yet; the diagnostics page shows the reading so the chip
// and its orientation can be checked on the bench, so the code is built with CONFIG_DIAG only.
#include "dm1801pro.h"
#include "config.h"

#if CONFIG_DIAG	// the whole file: the diagnostics page only
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "bus/i2c_gpio/i2c_gpio.h"
#include "sensor/lis3dh/lis3dh.h"

#define ACCEL_ADDR	0x19u	// 7-bit; SA0 high

static void scl(bool high)
{
	hrc_pin_out(PIN_ACCEL_SCL, high);
}

// SDA is open drain: released (an input, the bus pull-up takes it high) or driven low
static void sda(bool high)
{
	if (high) {
		hrc_pin_in(PIN_ACCEL_SDA);
	} else {
		hrc_pin_out(PIN_ACCEL_SDA, false);
	}
}

static bool sda_in(void)
{
	return hrc_pin_read(PIN_ACCEL_SDA);
}

static const i2c_gpio_t bus = {
	.scl = scl, .sda = sda, .sda_in = sda_in, .delay_us = hal_delay_us, .half_us = 5,
};

static bool reg_write(void *ctx, uint8_t reg, uint8_t val)
{
	(void)ctx;
	const uint8_t w[2] = { reg, val };
	return i2c_gpio_write(&bus, ACCEL_ADDR, w, sizeof(w));
}

static bool reg_read(void *ctx, uint8_t reg, uint8_t *buf, size_t n)
{
	(void)ctx;
	return i2c_gpio_write_read(&bus, ACCEL_ADDR, &reg, 1, buf, n);
}

static const lis3dh_bus_t chip = { reg_write, reg_read, NULL };

// The WHO_AM_I byte, or -1 when nothing acknowledges the address: the diagnostics line shows it when the probe fails
int16_t dm1801pro_accel_id(void)
{
	uint8_t id = 0;
	return reg_read(NULL, LIS3DH_REG_WHO_AM_I, &id, 1) ? id : -1;
}

// 0 = not tried, 1 = running, -1 = no answer (tried again on the next call: the diagnostics page is the only caller)
static int8_t state;

bool dm1801pro_accel_read(int16_t mg[3])
{
	if (state != 1) {
		hrc_pin_out(PIN_ACCEL_SCL, true);
		hrc_pin_in(PIN_ACCEL_SDA);
		i2c_gpio_recover(&bus);
		state = lis3dh_init(&chip) ? 1 : -1;
		if (state != 1) {
			return false;
		}
	}
	lis3dh_sample_t s;
	if (!lis3dh_read(&chip, &s, NULL)) {
		state = -1;
		return false;
	}
	mg[0] = s.x;
	mg[1] = s.y;
	mg[2] = s.z;
	return true;
}

#endif	// CONFIG_DIAG
