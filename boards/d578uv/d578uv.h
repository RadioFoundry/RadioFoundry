// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV board internals shared between its files (docs/spec/hardware-d578uv.md)
#ifndef D578UV_H
#define D578UV_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"

extern const hal_display_t d578uv_display;
extern const hal_input_t d578uv_input;
extern const hal_power_t d578uv_power;
extern const hal_radio_t d578uv_radio;
#if CONFIG_DUAL_WATCH
extern const hal_rx2_t d578uv_rx2;	// side B (radio_d578uv.c, hardware-d578uv.md 5.2)
#endif
#if CONFIG_BCAST_FM
extern const hal_bcast_t d578uv_bcast;	// the KTMicro receiver's broadcast FM (radio_d578uv.c, hardware-d578uv.md 5.4)
#endif

// Key map bits (input_d578uv.c), named for what the stock firmware does with them (hardware-d578uv.md 3.2). Ladder 0
// from the lowest voltage up gives the stock key codes 0x14, 0x15, 0x16, 0x0E, ladder 1 0x17, 0x18, 0x19, 0x0F; PA1 is
// 0x1E. In menus the stock firmware takes 0x14 and 0x16 as up and down, 0x0E confirms and 0x0F exits. Which printed key
// each is isn't traced, so the others keep their stock code. PTT and the microphone's Up and Down come from the plain
// hand microphone's lines (front_io.h) or the serial microphone's frames, which add its keypad (the KEY_MIC_ digits,
// '*', '#', A/B and the programmable PA..PD; drivers/input/anytone_mic). The serial microphone's Menu and Exit are the
// front panel's
enum {
	KEY_UP, KEY_C15, KEY_DOWN, KEY_MENU,
	KEY_C17, KEY_C18, KEY_C19, KEY_EXIT,
	KEY_C1E,
	KEY_PTT, KEY_MIC_UP, KEY_MIC_DOWN,
	KEY_MIC_0, KEY_MIC_1, KEY_MIC_2, KEY_MIC_3, KEY_MIC_4, KEY_MIC_5, KEY_MIC_6, KEY_MIC_7, KEY_MIC_8, KEY_MIC_9,
	KEY_MIC_STAR, KEY_MIC_HASH, KEY_MIC_AB, KEY_MIC_PA, KEY_MIC_PB, KEY_MIC_PC, KEY_MIC_PD,
	KEY_COUNT
};
_Static_assert(KEY_COUNT <= 32, "the key scan is a 32-bit mask");

// The serial hand microphone (input_d578uv.c): heard, its accepted frames, bytes dropped and the key it holds
void d578uv_hand_mic(bool *present, uint32_t *frames, uint32_t *errors, uint8_t *key);

// LCD backlight, 0..100 %: register 8 of the PA5 device (radio_d578uv.c, front_io.h). Any task
void d578uv_backlight(uint8_t percent);
// The RF bus lines and every enable on it deselected, and every RF chip off, before anything else uses the bus
// (board_init: the backlight is set before the radio starts)
void d578uv_bus_init(void);

// --- Radio (radio_d578uv.c, hardware-d578uv.md 5) ---
// board_rf_safe's part: PE6 low, the PA DAC at 0, the shift register's TX bit clear. Plain register writes only
void d578uv_rf_safe(void);
#if CONFIG_DMR
// DMR transmit bursts the PA gate switched on, and key-ups refused with the burst line already high (pa_gate.h)
void d578uv_pa_counts(uint32_t *bursts, uint32_t *refused);
void d578uv_pa_counts_reset(void);
#endif
#if CONFIG_APRS_RX
// Side A is receiving FM (tuned, not transmitting, not in AM): what PC0 carries is worth demodulating
// (afsk_d578uv.c). Radio task
bool d578uv_fm_rx_a(void);
#endif
#if CONFIG_APRS
// APRS: the tones sent on PB9 (tone_d578uv.c), received on PC0 with CONFIG_APRS_RX (afsk_d578uv.c,
// hardware-d578uv.md 3.4)
extern const hal_afsk_t d578uv_afsk;
// The RF chip's audio input for PB9 with the microphone muted (on: only while keyed in FM), or the microphone again
// (radio_d578uv.c)
bool d578uv_line_in(bool on);
// hal_afsk.h's transmit calls (tone_d578uv.c)
bool d578uv_afsk_tx_start(const uint8_t *tones, uint16_t nbits, uint16_t baud, uint16_t mark_hz, uint16_t space_hz,
			  uint8_t level);
bool d578uv_afsk_tx_busy(void);
void d578uv_afsk_tx_stop(void);
#endif
#if CONFIG_AM_RX || CONFIG_BCAST_FM
bool d578uv_kt_present(void);		// the KTMicro receiver answered at the radio's init (5.4)
#endif
#if CONFIG_VOX
uint8_t d578uv_mic_level(void);	// hal_power_t.mic_level: chip 0's voice amplitude, 0..100 (radio_d578uv.c)
#endif
// Side A's last raw RSSI, noise and signal flag (the radio task's status poll), for the diagnostics
void d578uv_rx_raw(uint16_t *rssi, uint16_t *noise, bool *signal);
// Diagnostics page lines (diag_d578uv.c)
bool d578uv_diag_line(uint8_t i, char *buf, uint8_t len);
void d578uv_diag_reset(void);
#if CONFIG_DTMF
// The DTMF pair on PB9 (tone_d578uv.c, hardware-d578uv.md 5.3): start, or change to, low_hz + high_hz
void d578uv_dtmf_start(uint16_t low_hz, uint16_t high_hz);
#endif
#if CONFIG_DTMF || CONFIG_APRS
// PB9's audio off, DTMF or AFSK; plain register writes (d578uv_rf_safe)
void d578uv_tone_stop(void);
#endif

// --- SPI NAND (nand_d578uv.c, hardware-d578uv.md 6) ---
// RadioFoundry's own block range, the only blocks it programs or erases (hardware-d578uv.md 6.3): the top 32 blocks
// (4 MB) of the 32 MB at 0x0E000000 the stock firmware never addresses. "Initialize radio" doesn't reach it either
#define D578UV_NAND_RFOUNDRY_FIRST	2016u
#define D578UV_NAND_RFOUNDRY_BLOCKS	32u
bool d578uv_nand_init(void);
bool d578uv_nand_ready(void);
uint16_t d578uv_nand_id(void);		// manufacturer << 8 | device, 0 if nothing answered
bool d578uv_nand_read(uint32_t addr, void *buf, uint32_t len);
// The NAND's mutex (recursive): taken by every function here, and by the storage around each whole operation
void d578uv_nand_lock(void);
void d578uv_nand_unlock(void);
#if CONFIG_NAND_STORAGE
#include "flash/nand_sectors/nand_sectors.h"
#include "hal_flash.h"
extern const nand_dev_t d578uv_nand_dev;
extern hal_flash_t d578uv_flash;		// flash_d578uv.c
bool d578uv_flash_init(void);
#endif

// --- GPS (gps_d578uv.c, hardware-d578uv.md 3.6) ---
// Start listening for the module (board_init); the platform's receiver (gps_uart.h) does the rest
void d578uv_gps_init(void);
// The module's supply: RF chip 0's output bit 2 (radio_d578uv.c). Task context; held until the radio has set the chip
// up when asked earlier
void d578uv_gps_power(bool on);

// --- DMR baseband (modem_d578uv.c, hardware-d578uv.md 4) ---
// Boot the SCT3258 from the codec slot (about 1 s) and start the modem task. False: no block or no answer, FM only
bool d578uv_modem_init(void);
uint8_t d578uv_modem_boot_result(void);		// sct_boot_result_t
#endif
