// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-1801 Pro board files
#ifndef DM1801PRO_H
#define DM1801PRO_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "dmr_glue.h"
#include "audio_pcm.h"
#include "vocoder.h"

// Ops each board file provides
extern const hal_display_t dm1801pro_display;
extern const hal_input_t dm1801pro_input;
extern const hal_radio_t dm1801pro_radio;
extern const hal_power_t dm1801pro_power;

// Physical key bits returned by dm1801pro_input.scan(); the matrix positions and the four direct pins are in
// input_dm1801pro.c (hardware-dm1801pro.md 9.1)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_MENU, KEY_BACK, KEY_VFO_MR, KEY_AB,
	KEY_SK1, KEY_SK2, KEY_SOS, KEY_PTT,
	KEY_COUNT
};

// Raw key state for the diagnostics page: matrix keys as a bit per strobe * 4 + input, the direct pins in bits 20-23
uint32_t dm1801pro_key_raw(void);

uint16_t dm1801pro_battery_raw(void);	// the last ADC channel C reading

// --- Radio (radio_dm1801pro.c) ---
typedef struct {
	uint16_t rssi_raw, noise_raw;	// FD6818 0x67 / 0x65 from the last status poll
	uint16_t fm_noise;		// HR_C7000 FM_SQL
	uint32_t rf_writes;
} dm1801pro_radio_diag_t;
void dm1801pro_radio_diag(dm1801pro_radio_diag_t *d);
// DMR PCM to the speaker instead of the FM path (the speaker_pcm hook of hrc_audio_pcm)
void dm1801pro_speaker_pcm(bool on);
// The speaker amplifier (PTA16) is on while any user wants it: the channel audio, the broadcast tuner, speaker PCM
#define DM1801PRO_AMP_RADIO	0x01u
#define DM1801PRO_AMP_BCAST	0x02u
#define DM1801PRO_AMP_PCM	0x04u
void dm1801pro_amp(uint8_t who, bool on);

// --- Broadcast FM receiver (bcast_dm1801pro.c, CONFIG_BCAST_FM; the HAL is rda5807m_hal) ---
void dm1801pro_bcast_init(void);

// --- Accelerometer (accel_dm1801pro.c, CONFIG_DIAG): milli-g on x, y, z; false when the chip doesn't answer ---
bool dm1801pro_accel_read(int16_t mg[3]);
int16_t dm1801pro_accel_id(void);	// WHO_AM_I, -1 = no acknowledge

// --- Diagnostics (diag_dm1801pro.c) ---
bool dm1801pro_diag_line(uint8_t i, char *buf, uint8_t len);
void dm1801pro_diag_reset(void);

#endif
