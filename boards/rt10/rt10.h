// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the RT10 board files
#ifndef RT10_H
#define RT10_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "hal_flash.h"

// The data flash is the shared SPI0 one (platform/hrc7000/flash_spi0.c, configured in board.c)
extern hal_flash_t hrc_flash_spi0;

extern const hal_display_t rt10_display;
extern const hal_input_t rt10_input;
extern const hal_radio_t rt10_radio;
extern const hal_power_t rt10_power;

// Physical key bits returned by rt10_input.scan()
enum {
	KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};

// The channel knob's raw 4-bit code (bit 3..0 = PIN_KNOB_B3..B0)
uint8_t rt10_knob_raw(void);

uint32_t rt10_input_ptt_ignored(void);
bool rt10_diag_line(uint8_t i, char *buf, uint8_t len);

#endif
