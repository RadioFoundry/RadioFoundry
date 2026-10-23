// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A host stand-in for platform/hrc7000/platform.h, enough to build a GD-88 board.c in a host test: the registers it
// writes are plain variables and the pin calls are recorded by the test
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_system.h"

extern uint32_t host_io_diplex0, host_io_diplex2, host_dac_data_b, host_dac_pd_ctrl;
#define IO_DIPLEX0		host_io_diplex0
#define IO_DIPLEX2		host_io_diplex2
#define IO_DIPLEX2_DAC_A	(1u << 27)
#define IO_DIPLEX2_DAC_B	(1u << 28)
#define IO_DIPLEX2_DAC_C	(1u << 29)
#define DAC_DATA_B		host_dac_data_b
#define DAC_PD_CTRL		host_dac_pd_ctrl
#define DAC_B_OFF		(1u << 1)
#define HRC_RAMFUNC

void hrc_pin_out(uint8_t pin, bool level);
void hrc_pin_in(uint8_t pin);
void hrc_pin_write(uint8_t pin, bool level);
void hrc_pin_force(uint8_t pin, bool level);

extern const hal_system_t hrc_system;

#endif
