// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the MD-9600 board files (docs/spec/hardware-md9600.md)
#ifndef MD9600_H
#define MD9600_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "rfplan_md9600.h"

// Ops each board file provides
extern const hal_display_t md9600_display;
extern const hal_input_t md9600_input;
extern const hal_radio_t md9600_radio;
extern const hal_power_t md9600_power;
extern hal_flash_t md9600_flash;

// SPI2 carries the data flash and the LCD: one lock for both (board.c). Recursive, a no-op before the scheduler
void md9600_spi2_lock(void);
void md9600_spi2_unlock(void);
void md9600_spi2_init(void);

// ADC1 scan ranks (power_md9600.c): raw 12-bit averages
enum { MD9600_ADC_NOISE_U, MD9600_ADC_NOISE_V, MD9600_ADC_RSSI_U, MD9600_ADC_RSSI_V, MD9600_ADC_MIC,
	MD9600_ADC_SUPPLY, MD9600_ADC_MICKEY_ROW, MD9600_ADC_MICKEY_COL, MD9600_ADC_COUNT };
uint16_t md9600_adc_raw(uint8_t ch);
// Supply voltage in mV (unverified scale)
uint16_t md9600_supply_mv(void);

// "Off" is a standby the board keeps itself in (power_md9600.c): the power key's state as the key scan sees it, and
// the start-up check that waits in standby when the radio was switched off
void md9600_power_key(bool down);
void md9600_standby_check(void);

// Backlight on TIM8 CH1 (display_md9600.c), 0..100 %
void md9600_backlight(uint8_t percent);

// Physical key bits returned by md9600_input.scan() (matrix positions in docs/spec/hardware-md9600.md 9.1)
enum {
	KEY_P1, KEY_P2, KEY_P3, KEY_P4, KEY_ALARM, KEY_BACK, KEY_ENTER, KEY_UP, KEY_DOWN, KEY_POWER, KEY_PTT,
	KEY_COUNT
};
// The raw matrix (columns low per row, bits 0-2 = K4-K6), for the diagnostics page
void md9600_input_raw(uint8_t rows[4]);

// Data flash (flash_md9600.c): security registers 1..3, 256 bytes each (calibration in 1-2, options in 3)
bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);
bool md9600_cps_security_read(uint32_t addr, void *buf, uint32_t len);
uint16_t md9600_flash_device(void);	// JEDEC memory type and capacity, 0 = none
// The board's own record sector: the stored board revision
md9600_rev_t md9600_flash_rev_load(void);
bool md9600_flash_rev_store(md9600_rev_t rev);

// Receiver front-end tuning from the factory calibration (cal_md9600.c, docs/spec/hardware-md9600.md 11)
void md9600_cal_load(void);
uint16_t md9600_cal_rx_tune(uint32_t hz);	// 12-bit DAC value
const char *md9600_cal_source(void);		// "factory" or "none"

// Board revision in use and the RF state (radio_md9600.c)
md9600_rev_state_t md9600_rev_state(void);
typedef struct {
	uint32_t lo_hz;		// the synthesizer's last frequency
	uint8_t band;
	uint32_t retunes, refused;
	int16_t probe;		// receiver probe: -1 not run, else the md9600_rev_t it picked
} md9600_radio_diag_t;
void md9600_radio_diag(md9600_radio_diag_t *d);
void md9600_radio_early(void);	// board_init: the revision and the power-on synthesizer set-up

// Diagnostics page (diag_md9600.c)
bool md9600_diag_line(uint8_t i, char *buf, uint8_t len);
void md9600_diag_reset(void);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
