// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 boards' broadcast FM receiver: an RDA5807-class tuner on two GPIO pins, bit-banged I2C (drivers/bus/i2c_gpio)
// and the shared chip HAL (drivers/bcast/rda5807m). The board gives its pins, the tuner's enable line if it has one
// and its audio path; board.bcast is then &rda5807m_hal. One tuner per radio.
#ifndef HRC_BCAST_H
#define HRC_BCAST_H

#include <stdint.h>
#include <stdbool.h>

#define HRC_BCAST_NO_PIN	0xFFu

typedef struct {
	uint8_t sda_pin, scl_pin;	// the I2C pins; SDA is driven open drain (the bus pull-up takes it high)
	uint8_t enable_pin;		// the tuner's enable line, HRC_BCAST_NO_PIN = none
	bool enable_level;		// the level on enable_pin that runs the tuner
	void (*audio_path)(bool on);	// the tuner's audio to the speaker (its amplifier, a source switch); may be NULL
} hrc_bcast_cfg_t;

// Pins idle, the tuner held off, the HAL set up. cfg must stay valid. Before the core starts
void hrc_bcast_init(const hrc_bcast_cfg_t *cfg);

#endif
