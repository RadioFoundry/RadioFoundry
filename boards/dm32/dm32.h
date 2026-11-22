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
bool dm32_radio_transmitting(void);	// the radio is keyed (the shared PTT line's gate)
extern const hal_radio_t dm32_radio;
// The debug link's CHIP_REG for the FD6818 (board.chip_reg)
bool dm32_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write);
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
void dm32_bcast_audio(bool on);	// the broadcast receiver's audio path (CONFIG_BCAST_FM)
// Who keeps the speaker amplifier on: 0x01 receive audio, 0x02 speaker PCM, 0x04 the broadcast receiver, 0x40 the
// TX tone's sidetone (radio_dm32.c amp())
uint8_t dm32_amp_users(void);

#if CONFIG_FM_FLAT
#include "hal_audio_tap.h"
// --- Flat paths for a data modem (flat_dm32.c, radio_dm32.c; hardware-dm32.md 9.3) ---

// The bench's settings for them, the debug link's knobs (flat_dm32.c). Taken at the next fm_tx_inject, receive tune
// or speaker change, not at once
typedef struct {
	uint16_t rf_tx_2b, rf_rx_2b;	// FD6818 0x2B on the flat transmit and receive paths
	uint8_t tx_bw_clr, rx_bw_clr;	// HR_C7000 FM_BANDWIDTH bits cleared (subsets of HRC7000_FM_FLAT_TX_CLR / _RX_CLR)
	uint32_t dev_coef;		// HR_C7000 FM_DEV_COEF on the flat transmit path, 0 = FM transmit's
	bool fm_play;			// HR_C7000 FM playback (AUDIO_CONTROL b6) on during flat receive
	bool tx_other_half;		// fill the TX RAM half FM_ADDR_SW doesn't name
} dm32_flat_bench_t;
extern dm32_flat_bench_t dm32_flat_bench;
extern const hal_audio_tap_t dm32_audio_tap;
// The route switches (radio_dm32.c), radio task: hal_audio_tap_t fm_tx_inject (NULL = the microphone again) and
// fm_rx_flat_start / stop
bool dm32_flat_tx(audio_tap_pull_t pull);
bool dm32_flat_rx_start(void);
void dm32_flat_rx_stop(void);
bool dm32_flat_rx_on(void);
#endif

// --- Diagnostics (diag_dm32.c) ---
typedef struct {
	uint16_t rssi_raw, noise_raw;	// FD6818 0x67 / 0x65 from the last status poll
	uint16_t fm_noise;		// HR_C7000 FM_SQL
	uint32_t fd_writes;
	char rf_setup;			// the RF section (hardware-dm32.md 8.10): A classic, B newer, - neither locked
} dm32_radio_diag_t;
void dm32_radio_diag(dm32_radio_diag_t *d);
char dm32_rf_setup(void);	// the RF section start-up found, as rf_setup: no bus traffic
bool dm32_diag_line(uint8_t i, char *buf, uint8_t len);
void dm32_diag_reset(void);
bool dm32_view_line(uint8_t i, char *buf, uint8_t len);	// the debug link's board.* keys (view_dm32.c)

#endif
