// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Internal interfaces between the GD-88 SoC A board files (the shared ones are in common/gd88_rf.h)
#ifndef GD88_H
#define GD88_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "gd88_rf.h"
#include "hal_rx2.h"
#include "platform.h"

extern const hal_display_t gd88_display;
// The panel variant in use (ili9341.h) and whether the panel record in program flash gave it (the debug page)
uint8_t gd88_panel_variant(void);
bool gd88_panel_from_record(void);
extern const hal_input_t gd88_input;
extern const hal_power_t gd88_power;

// Physical key bits returned by gd88_input.scan(), named by the key as the front panel shows it (docs/spec/hardware-gd88.md
// 9.2): the two blank soft keys under the display, green and red, the up/down rocker; SK1 and SK2 the left side keys
// above and below PTT (SK1 the IAP key, the vendor's P2), SOS the orange top key (the vendor's P3). Which matrix code
// is which front key is input_gd88.c's map
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_STAR, KEY_HASH,
	KEY_UP, KEY_DOWN, KEY_GREEN, KEY_RED, KEY_SOFT_LEFT, KEY_SOFT_RIGHT,
	KEY_SK1, KEY_SK2, KEY_SOS,
	KEY_PTT,
	KEY_COUNT
};

// --- The serial output chain (chain_gd88.c): 64 outputs on PTB0 / PTB1 / PTB3 ---
#define CHAIN_BITS		64u
#define CHAIN_NONE		0xFFu	// a function whose chain bit isn't known: never driven
void gd88_chain_init(void);			// all zero, the power-on state
void gd88_chain_set(uint8_t bit, bool on);	// CHAIN_NONE is ignored. Any task
void gd88_chain_zero(void);			// every output to 0: RAM code, interrupts off (board_rf_safe)
uint64_t gd88_chain_shadow(void);

// --- ADC channels (power_gd88.c; hrc_adc_read) ---
#define GD88_ADC_MIC		0u	// VOX / mic level
#define GD88_ADC_VOLUME		1u	// volume knob
#define GD88_ADC_BATTERY	2u

// --- Data flash (flash_gd88.c; CONFIG_NAND_STORAGE) ---
#if CONFIG_NAND_STORAGE
extern hal_flash_t gd88_flash;
bool gd88_flash_init(void);
void gd88_flash_stats(nand_sectors_stats_t *st);
#endif

// --- The second receiver on SoC B (rx2_gd88.c) ---
extern const hal_rx2_t gd88_rx2;
void gd88_rx2_init(void);
void gd88_rx2_led(uint8_t which, bool on);	// hal_power_t.led: LED_RED (TX) is B's LED 1, the other LED 2
// For the diagnostics page: the link's state, its last report and the UART's counters
typedef struct {
	bool up;
	uint8_t version, flags;
	const char *fw;
	uint32_t frames, bad, tunes, resent;
	hrc_link_uart_stats_t uart;
	rx2_status_t st;
} gd88_rx2_diag_t;
void gd88_rx2_diag(gd88_rx2_diag_t *d);
// B's audio to the speaker (true) or not, through A's chain routing; dmr: B is on a DMR channel
void gd88_audio_b(bool on, bool dmr);
// The broadcast FM tuner's audio to the speaker (chain source bit 6), with CONFIG_BCAST_FM
void gd88_audio_bcast(bool on);
void gd88_bcast_init(void);

// --- Diagnostics (diag_gd88.c) ---
uint32_t gd88_input_raw(void);		// the last key scan before debouncing
uint32_t gd88_input_ptt_ignored(void);
bool gd88_diag_line(uint8_t i, char *buf, uint8_t len);
void gd88_diag_reset(void);

#endif
