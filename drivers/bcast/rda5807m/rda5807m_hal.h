// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The RDA5807M as the board's hal_bcast_t: chip driver (rda5807m.h) and a bit-banged I2C bus on two GPIO pins
// (drivers/bus/i2c_gpio) behind the interface the core's broadcast receiver uses (hal_bcast.h). A board supplies its
// pins and, if it has them, the tuner's enable line and the audio path switch; one tuner per radio, so the state is
// static. All calls come from one task.
#ifndef RDA5807M_HAL_H
#define RDA5807M_HAL_H

#include "hal_bcast.h"
#include "rda5807m.h"
#include "bus/i2c_gpio/i2c_gpio.h"

typedef struct {
	const i2c_gpio_t *bus;		// the chip's two pins; half_us 5 is 100 kHz
	void (*delay_ms)(uint32_t ms);	// the chip's reset time; may sleep
	void (*enable)(bool on);	// optional: the tuner's own enable / reset line, called before power-up and after power-down
	void (*audio_path)(bool on);	// optional: the board's switch that sends the tuner's audio to the speaker
	rda5807m_cfg_t cfg;		// reference clock and de-emphasis
	uint32_t start_khz;		// the frequency the first power-up tunes to
} rda5807m_hal_cfg_t;

// Select the board's wiring. cfg must stay valid. Call once, before the core uses rda5807m_hal
void rda5807m_hal_init(const rda5807m_hal_cfg_t *cfg);

extern const hal_bcast_t rda5807m_hal;

// Register access over the I2C master, for hosts that want the chip without the HAL (and for tests)
extern const rda5807m_bus_t rda5807m_i2c_bus;

#endif
