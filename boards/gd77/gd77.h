// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the GD-77 board files (docs/spec/hardware-gd77.md)
#ifndef GD77_H
#define GD77_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "keys.h"
#include "platform.h"
#include "bus/i2c_bus.h"
#include "bus/spi_bus.h"

// Transmit in this build (boards/gd77/config.mk, off by default): the bands allow it and tx() may key, still only with
// a valid calibration point
#ifndef CONFIG_GD77_TX
#define CONFIG_GD77_TX		0
#endif
// The HR_C6000 voice port on SPI1 (PTB10, PTB11, PTB16, PTB17); 0: page 3 over the configuration port instead
#ifndef GD77_VSPI_ON_SPI1
#define GD77_VSPI_ON_SPI1	1
#endif

// Ops each board file provides
extern const hal_display_t gd77_display;
extern const hal_input_t gd77_input;
extern const hal_radio_t gd77_radio;
extern const hal_power_t gd77_power;
extern hal_flash_t gd77_flash;	// size and map set by its init (storage_gd77.c)
extern const hal_audio_t gd77_audio;		// audio_gd77.c, with CONFIG_BEEP or CONFIG_DMR
extern const hal_dmr_modem_t gd77_dmr;		// dmr_gd77.c, with CONFIG_DMR
extern const hal_vocoder_t gd77_vocoder;	// vocoder_gd77.c, with CONFIG_DMR

// --- Audio (audio_gd77.c, radio_gd77.c) ---
// PCM to the speaker (beeps, DMR voice) on top of the FM squelch request: radio_gd77.c combines the two. Callable
// from the audio and radio tasks
void gd77_speaker_pcm(bool on);
typedef struct {
	bool running;
	uint32_t blocks;	// 20 ms blocks handed to the callback
	uint32_t dropped;	// microphone blocks overwritten before the task took them
	uint32_t underruns;	// transmit halves sent as silence
	uint32_t i2s_errors;	// I2S FIFO and frame sync errors
} gd77_audio_stats_t;
void gd77_audio_stats(gd77_audio_stats_t *out);

// --- Buses (bus_gd77.c; software or hardware per bus_cfg.h) ---
void gd77_bus_init(void);	// the software SPI buses; before anything uses them
k22f_status_t gd77_bus_status(uint8_t id);	// per gd77_bus_id_t: K22F_ERR_NOT_IMPLEMENTED for a choice not written yet
extern const spi_bus_t gd77_nor_bus;	// data flash
extern const spi_bus_t gd77_lcd_bus;	// display, write only (A0 stays with the display code)
const i2c_bus_t *gd77_i2c_b(void);	// bus B, NULL before gd77_i2c_init
// I2C bus A, shared by the AT1846S and the EEPROM: each call is one whole transaction under the bus lock
void gd77_i2c_init(void);	// the lock; after the scheduler starts
bool gd77_i2c_write(uint8_t addr7, const uint8_t *data, uint32_t n);
bool gd77_i2c_write_read(uint8_t addr7, const uint8_t *w, uint32_t wn, uint8_t *r, uint32_t rn);
// From the modem interrupts only: one write, false at once when a task-level transaction is on the wire
bool gd77_i2c_write_isr(uint8_t addr7, const uint8_t *data, uint32_t n);
// The HR_C6000 configuration port on SPI0: CS low, header, data, CS high
void gd77_c6k_bus_init(void);
bool gd77_c6k_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len);

// --- Keys (input_gd77.c): raw state for the diagnostics page: matrix bit = column * 5 + row, direct keys 20-23 ---
uint32_t gd77_key_raw(void);

// --- Power (power_gd77.c) ---
uint16_t gd77_battery_raw(void);	// the last ADC reading of the battery candidate channel
uint16_t gd77_adc_raw(uint8_t ch);	// the last reading of ADC_CH_BATTERY, _DP2 or _DP3; 0xFFFF = none yet
// --- Display (display_gd77.c) ---
bool gd77_backlight_pwm(void);		// the backlight runs on the FTM channel (GD77_BACKLIGHT_PWM)

// --- Data flash (flash_gd77.c) ---
uint32_t gd77_flash_jedec(void);	// manufacturer << 16 | device, 0 when nothing answered
uint32_t gd77_flash_refusals(void);	// erases and programs refused outside ODR's areas
// Any part of the chip, the first megabyte included (read only: diagnostics, the logical space below)
bool gd77_flash_raw_read(uint32_t addr, void *buf, uint32_t len);

// --- EEPROM and calibration (eeprom_gd77.c, cal_blocks_gd77.h) ---
void gd77_eeprom_init(void);		// probe, then load the calibration; after gd77_i2c_init
bool gd77_eeprom_present(void);
bool gd77_eeprom_read(uint32_t addr, void *buf, uint32_t len);
// The radio's own logical codeplug space across both chips (storage_gd77.h), read only
bool gd77_logical_read(uint32_t logical, void *buf, uint32_t len);

// --- Radio (radio_gd77.c) ---
typedef struct {
	bool rf_ok;		// the AT1846S answered its init
	bool c6k_ok;		// the HR_C6000 took its init
	uint16_t rf_id;		// AT1846S reg 0x00 (chip id)
	uint32_t rf_errors;	// failed register transfers
	uint8_t rssi, noise;	// reg 0x1B from the last status poll
	uint8_t drive;		// DAC0 now (8-bit drive unit)
	bool tx_build;		// built with CONFIG_GD77_TX
	uint32_t tx_refused;	// tx() calls refused (not built, band, no calibration)
} gd77_radio_diag_t;
void gd77_radio_diag(gd77_radio_diag_t *d);

// --- Diagnostics (diag_gd77.c) ---
bool gd77_diag_line(uint8_t i, char *buf, uint8_t len);
// The debug link's board view section (board.view_line): read-only key=value lines for the bring-up HIL tests
bool gd77_view_line(uint8_t i, char *buf, uint8_t len);

#endif
