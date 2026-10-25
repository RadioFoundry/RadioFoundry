// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 broadcast FM receiver wiring (hrc_bcast.h; docs/spec/airband-and-broadcast.md 4)
#include "config.h"

#if CONFIG_BCAST_FM	// the whole file
#include "hrc_bcast.h"
#include "platform.h"
#include "hal_time.h"
#include "bcast/rda5807m/rda5807m_hal.h"

static const hrc_bcast_cfg_t *wiring;

static void scl(bool high)
{
	hrc_pin_out(wiring->scl_pin, high);
}

// Open drain: released (an input) or driven low
static void sda(bool high)
{
	if (high) {
		hrc_pin_in(wiring->sda_pin);
	} else {
		hrc_pin_out(wiring->sda_pin, false);
	}
}

static bool sda_in(void)
{
	return hrc_pin_read(wiring->sda_pin);
}

static const i2c_gpio_t bus = {
	.scl = scl, .sda = sda, .sda_in = sda_in, .delay_us = hal_delay_us, .half_us = 5,	// 100 kHz
};

static void enable(bool on)
{
	if (wiring->enable_pin != HRC_BCAST_NO_PIN) {
		hrc_pin_write(wiring->enable_pin, on ? wiring->enable_level : !wiring->enable_level);
	}
}

static void audio_path(bool on)
{
	if (wiring->audio_path != NULL) {
		wiring->audio_path(on);
	}
}

static const rda5807m_hal_cfg_t hal_cfg = {
	.bus = &bus,
	.delay_ms = hal_delay_ms,
	.enable = enable,
	.audio_path = audio_path,
	// 32.768 kHz crystal clocking, 75 us de-emphasis (the Americas), the datasheet's seek threshold
	.cfg = { .clk_mode = 0, .rclk_direct = false, .de_50us = false, .seek_th = 8 },
	.start_khz = 100000,
};

void hrc_bcast_init(const hrc_bcast_cfg_t *cfg)
{
	wiring = cfg;
	hrc_pin_out(cfg->scl_pin, true);
	hrc_pin_in(cfg->sda_pin);
	if (cfg->enable_pin != HRC_BCAST_NO_PIN) {
		hrc_pin_out(cfg->enable_pin, !cfg->enable_level);
	}
	rda5807m_hal_init(&hal_cfg);
}

#endif	// CONFIG_BCAST_FM
