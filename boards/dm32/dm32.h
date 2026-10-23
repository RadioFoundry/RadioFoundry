// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the DM-32UV board files
#ifndef DM32_H
#define DM32_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "cal/baofeng/baofeng_cal.h"
#include "hal_flash.h"
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "vocoder.h"

// The data flash is the shared SPI0 one (platform/hrc7000/flash_spi0.c, configured in board.c)
extern hal_flash_t hrc_flash_spi0;
#define dm32_flash hrc_flash_spi0

// Ops each board file provides
extern const hal_display_t dm32_display;
extern const hal_input_t dm32_input;
// Key scans with the PTT pin low that were ignored as host serial data (hardware-dm32.md 6.2)
uint32_t dm32_input_ptt_ignored(void);
extern const hal_radio_t dm32_radio;
extern const hal_power_t dm32_power;

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

// --- Radio <-> DMR glue: the shared modem glue is platform/hrc7000/dmr_glue.h, audio_pcm.h ---

#include "config.h"
#include "modem/hrc7000/hrc7000.h"

// DMR PCM to the speaker instead of the FM path (radio_dm32.c; the hal_audio_t speaker_pcm hook of hrc_audio_pcm)
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

#endif
