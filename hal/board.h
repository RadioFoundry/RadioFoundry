// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The board descriptor: everything the core may know about the radio it runs on.
// Core code reads this at run time; it never tests a radio define.
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_display.h"
#include "hal_input.h"
#include "hal_radio.h"
#include "hal_power.h"
#include "hal_flash.h"
#include "hal_dmr_modem.h"
#include "hal_audio.h"
#include "hal_vocoder.h"

typedef struct {
	uint32_t lo_hz;
	uint32_t hi_hz;
	bool tx;		// transmit allowed in this range
} band_t;

enum {
	CAP_KEYPAD	= 1u << 0,
	CAP_ENCODER	= 1u << 1,
	CAP_GPS		= 1u << 2,
	CAP_DMR		= 1u << 3,
	CAP_VOLUME_POT	= 1u << 4,
};

typedef struct {
	const char *name;
	uint32_t caps;
	const band_t *bands;
	uint8_t band_count;
	uint8_t power_count;
	const char *const *power_names;	// short labels for the status line, power_count entries
	uint8_t power_default;
	const hal_display_t *display;
	const hal_input_t *input;
	const keymap_entry_t *keymap;
	uint8_t keymap_len;
	const hal_radio_t *radio;
	const hal_power_t *power;
	const hal_flash_t *flash;
	const hal_dmr_modem_t *dmr;	// NULL on FM-only radios
	const hal_audio_t *audio;
	const hal_vocoder_t *vocoder;
	// Optional: line i of a board diagnostics page (raw readings for bench checks); false past the last line
	bool (*diag_line)(uint8_t i, char *buf, uint8_t len);
} board_t;

extern const board_t board;

// Clocks, pins, anything that must happen before the RTOS starts. Called from the platform's main()
void board_early_init(void);
// Start-up after the scheduler runs (drivers that sleep), from the first task
void board_init(void);

#endif
