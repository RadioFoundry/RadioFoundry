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

// The FD6818's register bus (radio_rt10.c), its ID as read at start-up and the frames it didn't acknowledge
#include "rf/rfbus/rfbus.h"
rfbus_t *rt10_rf_bus(void);
void rt10_rf_info(uint16_t *id, uint32_t *nacks);
// The debug link's CHIP_REG for the FD6818 (board.chip_reg)
bool rt10_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write);
bool rt10_diag_line(uint8_t i, char *buf, uint8_t len);

// --- Factory calibration (calibration_rt10.c, hardware-rt10.md 4) ---
#define RT10_CAL_STEPS		15u	// 10 MHz steps from the sector's base
#define RT10_CAL_BASE_MIN	80u	// the base's plausible range, / 10 MHz
#define RT10_CAL_BASE_MAX	95u
#define RT10_TX_CENTER_DEFAULT	128u	// the modem's TX center without a calibration
typedef bool (*rt10_cal_read_fn)(uint32_t addr, void *buf, uint32_t len);
// Copies the tables from the data flash; false (and every plan refused) without a calibration
bool rt10_cal_load(rt10_cal_read_fn read);
bool rt10_cal_valid(void);
uint8_t rt10_cal_step(uint32_t hz);
// One transmission's settings: PA control level (DAC B, 12 bit), FD6818 PA gain 0..7, the delay between the RX
// supply going off and the TX state, and the one before the ramp down
typedef struct {
	uint16_t dac;
	uint8_t pa_gain;
	uint16_t on_us, off_us;
} rt10_tx_plan_t;
// high: the stock's high power set, else its low one
bool rt10_cal_tx_plan(uint32_t hz, bool high, rt10_tx_plan_t *p);
uint8_t rt10_cal_tx_center(void);
uint8_t rt10_cal_mod(uint32_t hz);

#endif
