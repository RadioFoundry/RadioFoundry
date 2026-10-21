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

// Factory calibration and the hardware option block: cal_md9600.h (docs/spec/hardware-md9600.md 11)
#include "cal_md9600.h"
// What the RF code takes from it (rfcal_md9600.c): PA drive per power level and the squelch thresholds, each used
// only when its rows look plausible (md9600_cal_rf_rows)
#define MD9600_POWER_LEVELS	4u		// 5 W, 10 W, 25 W, 40 W (labels, unverified)
enum { MD9600_CALOK_RX = 1u << 0, MD9600_CALOK_PA = 1u << 1, MD9600_CALOK_SQL = 1u << 2 };
bool md9600_cal_apc(uint32_t hz, uint8_t power, uint16_t *dac);	// false: no plausible PA rows
bool md9600_cal_squelch(uint32_t hz, uint16_t *l1_open, uint16_t *l9_open);	// 12-bit noise readings
uint8_t md9600_cal_rf_rows(uint8_t band);	// MD9600_CALOK_* bits for a band (md9600_band_t)

// Board revision in use and the RF state (radio_md9600.c, rfseq_md9600.c)
md9600_rev_state_t md9600_rev_state(void);
typedef struct {
	uint32_t lo_hz;		// the synthesizer's last frequency
	uint8_t band;
	uint8_t state;		// md9600_rf_state_t
	uint32_t retunes, refused, errors;
	uint8_t last_err;	// md9600_rf_err_t
	bool tx_armed;
	int16_t probe;		// receiver probe: -1 not run, else the md9600_rev_t it picked (0 = no verdict)
} md9600_radio_diag_t;
void md9600_radio_diag(md9600_radio_diag_t *d);
void md9600_radio_early(void);	// board_init: the revision and the power-on synthesizer set-up
// Transmit arm for this power cycle (hwopt_md9600.c); false when refused (rfseq_md9600.h)
bool md9600_radio_tx_arm(bool on);
// Hardware options (hwopt_md9600.c, hal_hwopt.h): the board revision, and the transmit arm with CONFIG_MD9600_TX
#define MD9600_HWOPT_COUNT	(CONFIG_MD9600_TX ? 2u : 1u)	// needs config.h
extern const hal_hwopt_t md9600_hwopts[];
// The other analog inputs: the sub-audio input on ADC3 (PA2), 12-bit, one conversion
uint16_t md9600_adc3_qt(void);

// The debug link's board view keys (view_md9600.c)
bool md9600_view_line(uint8_t i, char *buf, uint8_t len);

// Diagnostics page (diag_md9600.c)
bool md9600_diag_line(uint8_t i, char *buf, uint8_t len);
void md9600_diag_reset(void);

// A DMA stream's memory address must be in main SRAM (0x2000_0000): the DMA controllers can't reach the CCM
#define DMA_RAM_OK(p)	configASSERT(((uintptr_t)(p) >> 24) == 0x20u)

#endif
