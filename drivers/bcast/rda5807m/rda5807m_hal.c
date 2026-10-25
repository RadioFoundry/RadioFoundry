// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RDA5807M as hal_bcast_t (rda5807m_hal.h)
#include "rda5807m_hal.h"

#define SETTLE_POLLS		40	// at most 40 x 5 ms for the power-up tune to finish
#define SETTLE_POLL_MS		5

static const rda5807m_hal_cfg_t *wiring;
static rda5807m_t dev;
static uint32_t last_khz;
static bool tune_pending;	// the tune that came with the power-up may still be running

// The chip's random-access mode: a write is register, high byte, low byte; a read sends the register and then reads two
static bool reg_write(void *ctx, uint8_t reg, uint16_t val)
{
	(void)ctx;
	const uint8_t w[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
	return i2c_gpio_write(wiring->bus, RDA5807M_I2C_ADDR, w, sizeof(w));
}

static bool reg_read(void *ctx, uint8_t reg, uint16_t *val)
{
	(void)ctx;
	uint8_t r[2];
	if (!i2c_gpio_write_read(wiring->bus, RDA5807M_I2C_ADDR, &reg, 1, r, sizeof(r))) {
		return false;
	}
	*val = (uint16_t)(((uint16_t)r[0] << 8) | r[1]);
	return true;
}

static void reg_delay(uint32_t ms)
{
	if (wiring->delay_ms != NULL) {
		wiring->delay_ms(ms);
	}
}

const rda5807m_bus_t rda5807m_i2c_bus = { reg_write, reg_read, reg_delay, NULL };

void rda5807m_hal_init(const rda5807m_hal_cfg_t *cfg)
{
	wiring = cfg;
	rda5807m_attach(&dev, &rda5807m_i2c_bus, &cfg->cfg);
	last_khz = cfg->start_khz;
}

static bool hal_power(bool on)
{
	if (on) {
		if (wiring->enable != NULL) {
			wiring->enable(true);
		}
		// A tuner left holding SDA by a reset in the middle of a transfer would never answer
		i2c_gpio_recover(wiring->bus);
		if (rda5807m_probe(&dev) && rda5807m_power(&dev, true, last_khz)) {
			tune_pending = true;
			return true;
		}
		if (wiring->enable != NULL) {
			wiring->enable(false);
		}
		return false;
	}
	bool ok = rda5807m_power(&dev, false, 0);
	if (wiring->audio_path != NULL) {
		wiring->audio_path(false);
	}
	if (wiring->enable != NULL) {
		wiring->enable(false);
	}
	return ok;
}

// A tune written while the power-up tune is still running leaves the chip on the first frequency: wait for its STC
static void settle_tune(void)
{
	rda5807m_status_t s;
	for (int i = 0; tune_pending && i < SETTLE_POLLS; i++) {
		if (!rda5807m_status(&dev, &s) || s.done) {
			break;
		}
		reg_delay(SETTLE_POLL_MS);
	}
	tune_pending = false;
}

static bool hal_tune(uint32_t khz)
{
	settle_tune();
	if (!rda5807m_tune(&dev, khz)) {
		return false;
	}
	last_khz = khz;
	return true;
}

static bool hal_seek(bool up)
{
	return rda5807m_seek(&dev, up);
}

static void hal_volume(uint8_t level)
{
	(void)rda5807m_volume(&dev, level);
}

static bool hal_status(bcast_status_t *st)
{
	rda5807m_status_t s;
	if (!dev.on || !rda5807m_status(&dev, &s)) {
		return false;
	}
	st->khz = s.khz;
	st->tuning = !s.done;
	st->seek_failed = s.seek_failed;
	st->stereo = s.stereo;
	st->station = s.station;
	st->rssi = s.rssi;
	if (s.done) {
		last_khz = s.khz;	// a seek ends on a new frequency; power-up comes back to it
	}
	return true;
}

static void hal_audio(bool on)
{
	if (!dev.on) {
		on = false;
	}
	(void)rda5807m_mute(&dev, !on);
	if (wiring->audio_path != NULL) {
		wiring->audio_path(on);
	}
}

const hal_bcast_t rda5807m_hal = {
	.lo_khz = RDA5807M_LO_KHZ,
	.hi_khz = RDA5807M_HI_KHZ,
	.grid_khz = RDA5807M_GRID_KHZ,
	.power = hal_power,
	.tune = hal_tune,
	.seek = hal_seek,
	.volume = hal_volume,
	.status = hal_status,
	.audio = hal_audio,
};
