// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC A broadcast FM receiver: the RDA5807-class tuner at 7-bit 0x11 on the bit-banged I2C pair PTA7 / PTA8
// (docs/spec/hardware-gd88.md 9) as the board's hal_bcast_t. The chip, its register shadows and the HAL are shared
// (drivers/bcast/rda5807m); this file is the wiring only. The tuner has no enable line; its audio goes through the
// chain's source bit 6 and the amplifier (paths_gd88.c)
#include "gd88.h"
#include "config.h"

#if CONFIG_BCAST_FM	// the whole file: broadcast FM only
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "bcast/rda5807m/rda5807m_hal.h"

// IO_DIPLEX0 bits 5 and 6: the pads of PTA7 / PTA8 (bit = 1 is a GPIO)
// TODO(gd88): inferred from the bits the vendor code changes around the tuner; check the bus answers on a radio
#define DIPLEX0_I2C_GPIO	((1u << 5) | (1u << 6))

static void scl(bool high)
{
	hrc_pin_out(PIN_I2C_SCL, high);
}

// SDA is open drain: released (an input, the bus pull-up takes it high) or driven low
static void sda(bool high)
{
	if (high) {
		hrc_pin_in(PIN_I2C_SDA);
	} else {
		hrc_pin_out(PIN_I2C_SDA, false);
	}
}

static bool sda_in(void)
{
	return hrc_pin_read(PIN_I2C_SDA);
}

static const i2c_gpio_t bus = {
	.scl = scl, .sda = sda, .sda_in = sda_in, .delay_us = hal_delay_us, .half_us = 5,
};

static const rda5807m_hal_cfg_t cfg = {
	.bus = &bus,
	.delay_ms = hal_delay_ms,
	.audio_path = gd88_audio_bcast,
	// 32.768 kHz crystal clocking (the vendor setup leaves CLK_MODE at 0), 75 us de-emphasis (US broadcast), the
	// datasheet's default seek threshold
	.cfg = { .clk_mode = 0, .rclk_direct = false, .de_50us = false, .seek_th = 8 },
	.start_khz = 100000,
};

void gd88_bcast_init(void)
{
	IO_DIPLEX0 |= DIPLEX0_I2C_GPIO;
	hrc_pin_out(PIN_I2C_SCL, true);
	hrc_pin_in(PIN_I2C_SDA);
	rda5807m_hal_init(&cfg);
}

#endif	// CONFIG_BCAST_FM
