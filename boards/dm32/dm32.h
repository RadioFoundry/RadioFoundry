// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-32UV board files
#ifndef DM32_H
#define DM32_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "cal/baofeng/baofeng_cal.h"
#include "hal_flash.h"

// The data flash is the shared SPI0 one (platform/hrc7000/flash_spi0.c, configured in board.c)
extern hal_flash_t hrc_flash_spi0;
#define dm32_flash hrc_flash_spi0

// Ops each board file provides
extern const hal_display_t dm32_display;
extern const hal_input_t dm32_input;
extern const hal_radio_t dm32_radio;
extern const hal_power_t dm32_power;
extern const hal_dmr_modem_t dm32_dmr;
extern const hal_audio_t dm32_audio;
extern const hal_vocoder_t dm32_vocoder;

// Physical key bits returned by dm32_input.scan() (matrix in docs/spec/hardware-dm32.md 6.1, 6.2)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_GREEN, KEY_RED,
	KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};

// The LCD data lines DB0-DB4 are also the keypad rows: the display and the key scan take turns (board.c)
void dm32_lcdbus_lock(void);
void dm32_lcdbus_unlock(void);

// --- Radio <-> DMR glue (radio_dm32.c, dmr_dm32.c) ---

#include "config.h"
#include "modem/hrc7000/hrc7000.h"
#if CONFIG_DMR
void dm32_dmr_board_init(hrc7000_t *modem);	// once, from the radio init: interrupts, PA timer, task
void dm32_dmr_enter(void);			// the modem has just entered DMR: engine on, interrupts on
void dm32_dmr_leave(void);
void dm32_dmr_rx_restart(void);
bool dm32_dmr_tx_active(void);
void dm32_dmr_tx_drop(void);
#else
static inline void dm32_dmr_board_init(hrc7000_t *modem) { (void)modem; }
static inline void dm32_dmr_enter(void) {}
static inline void dm32_dmr_leave(void) {}
static inline void dm32_dmr_rx_restart(void) {}
static inline bool dm32_dmr_tx_active(void) { return false; }
static inline void dm32_dmr_tx_drop(void) {}
#endif

// From the modem interrupts (ISR-safe, radio_dm32.c): per-burst RF switching and the PA one-shot expiry
void dm32_rf_dmr_switch(bool tx);
void dm32_rf_dmr_pa_on(void);
// The "DMR PA" delay in microseconds (1..35 ms, default 28 ms; hardware-dm32.md 8.6)
uint32_t dm32_dmr_pa_delay_us(void);

// The modem driver instance (radio_dm32.c owns it; audio and DMR use it)
hrc7000_t *dm32_modem(void);
// DMR PCM to the speaker instead of the FM path (radio_dm32.c; hal_audio_t.speaker_pcm)
void dm32_speaker_pcm(bool on);

// --- Diagnostics (diag_dm32.c) ---
typedef struct {
	uint16_t rssi_raw, noise_raw;	// FD6818 0x67 / 0x65 from the last status poll
	uint16_t fm_noise;		// HR_C7000 FM_SQL
	uint32_t fd_writes;
} dm32_radio_diag_t;
void dm32_radio_diag(dm32_radio_diag_t *d);
bool dm32_diag_line(uint8_t i, char *buf, uint8_t len);
void dm32_diag_reset(void);
#define DM32_DMR_DIAG_LINES	9
bool dm32_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
void dm32_dmr_stats_reset(void);
void dm32_audio_stats(uint32_t *blocks, uint32_t *overruns, uint32_t *underruns);
void dm32_audio_reset_stats(void);
uint8_t dm32_vocoder_tag(void);	// 49 or 46: the stock codec in the slot, 0 none

#endif
