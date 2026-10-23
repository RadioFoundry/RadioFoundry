// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-1801 Pro broadcast FM receiver: the RDA5807-class tuner on a bit-banged I2C bus on PTA25 / PTA26
// (hardware-dm1801pro.md 7) as the board's hal_bcast_t. The chip, its register shadows and the HAL are shared
// (drivers/bcast/rda5807m); this file is the wiring only.
#include "dm1801pro.h"
#include "config.h"

#if CONFIG_BCAST_FM	// the whole file: broadcast FM only
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "bcast/rda5807m/rda5807m_hal.h"

static void scl(bool high)
{
	hrc_pin_out(PIN_BCAST_SCL, high);
}

// SDA is open drain: released (an input, the bus pull-up takes it high) or driven low
static void sda(bool high)
{
	if (high) {
		hrc_pin_in(PIN_BCAST_SDA);
	} else {
		hrc_pin_out(PIN_BCAST_SDA, false);
	}
}

static bool sda_in(void)
{
	return hrc_pin_read(PIN_BCAST_SDA);
}

static const i2c_gpio_t bus = {
	.scl = scl, .sda = sda, .sda_in = sda_in, .delay_us = hal_delay_us, .half_us = 5,
};

// The tuner's enable line is low while it runs (it sits high at start-up, hardware-dm1801pro.md 3; polarity from the
// order of the start and stop code, not measured)
static void enable(bool on)
{
	hrc_pin_write(PIN_BCAST_EN, !on);
}

// The tuner's audio needs the speaker amplifier, which the channel audio shares
static void audio_path(bool on)
{
	dm1801pro_amp(DM1801PRO_AMP_BCAST, on);
}

static const rda5807m_hal_cfg_t cfg = {
	.bus = &bus,
	.delay_ms = hal_delay_ms,
	.enable = enable,
	.audio_path = audio_path,
	// 32.768 kHz crystal clocking, 75 us de-emphasis (US broadcast), the datasheet's default seek threshold
	.cfg = { .clk_mode = 0, .rclk_direct = false, .de_50us = false, .seek_th = 8 },
	.start_khz = 100000,
};

void dm1801pro_bcast_init(void)
{
	hrc_pin_out(PIN_BCAST_SCL, true);
	hrc_pin_in(PIN_BCAST_SDA);
	rda5807m_hal_init(&cfg);
}

#endif	// CONFIG_BCAST_FM
