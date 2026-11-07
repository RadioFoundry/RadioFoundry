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
// '*', '#', the A/B button above PTT and the A..D keys; drivers/input/anytone_mic). The microphone's EXIT and MENU are
// its C and D
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
void d578uv_hand_mic(hal_hand_mic_stats_t *st);

// LCD backlight, 0..100 %: register 8 of the PA5 device (radio_d578uv.c, front_io.h). Any task
void d578uv_backlight(uint8_t percent);
// The RF bus lines and every enable on it deselected, and every RF chip off, before anything else uses the bus
// (board_init: the backlight is set before the radio starts)
void d578uv_bus_init(void);

// --- Radio (radio_d578uv.c, hardware-d578uv.md 5) ---
// board_rf_safe's part: PE6 low, the PA DAC at 0, the shift register's TX bit clear. Plain register writes only
void d578uv_rf_safe(void);
// board.chip_reg: the QA4818s (chips 0-3) and the PA5 trims device (chip 4, write-only)
bool d578uv_chip_reg(uint8_t chip, uint8_t page, uint8_t reg, uint32_t *val, bool write);
void d578uv_outputs_off(void);		// the shift register all off, for the standby (power_d578uv.c)
bool d578uv_fan_on(void);		// the fan runs (chip 0's GPIO bit 1: transmitting or the PA hot)
extern const hal_temp_t d578uv_temp;
uint8_t d578uv_pot_percent(uint32_t raw);	// a volume pot's ADC reading as 0..100 (power_d578uv.c)
#if CONFIG_DMR
// DMR transmit bursts the PA gate switched on, and key-ups refused with the burst line already high (pa_gate.h)
void d578uv_pa_counts(uint32_t *bursts, uint32_t *refused);
void d578uv_pa_counts_reset(void);
// From the last DMR burst's line fall to tx_off() and to the receiver set up again, in us (0 before any)
void d578uv_rx_back(uint32_t *off_us, uint32_t *rx_us);
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
#if CONFIG_BCAST_FM
void d578uv_bc_raw(uint16_t r[3]);	// the RDA5807's 0x02, 0x0A, 0x0B as last polled (0 without one)
#endif
const char *d578uv_bc_chip(void);	// the AM/FM receiver that answered at init: "kt", "rda" (FM only) or "no" (5.4)
#endif
#if CONFIG_VOX
uint8_t d578uv_mic_level(void);	// hal_power_t.mic_level: chip 0's voice amplitude, 0..100 (radio_d578uv.c)
#endif
// Side A's last raw RSSI, noise and signal flag (the radio task's status poll), for the diagnostics
void d578uv_rx_raw(uint16_t *rssi, uint16_t *noise, bool *signal);
// Diagnostics page lines (diag_d578uv.c)
bool d578uv_diag_line(uint8_t i, char *buf, uint8_t len);
// The band split: the UHF path from here up (hardware-d578uv.md 5)
#define D578UV_UHF_FROM_HZ	300000000u
// The frequency the RF chips were last keyed on, FM or DMR (radio_d578uv.c)
uint32_t d578uv_tx_hz(void);
// The calibration copy in use: 'A', 'B' or 0 for none; *fqca the UHF block's byte 0 (radio_d578uv.c)
char d578uv_cal_copy(uint8_t *fqca);
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
// The same with a page whose ECC failed passed on (spi_nand_read_any): the stock calibration's first page reads so
bool d578uv_nand_read_any(uint32_t addr, void *buf, uint32_t len, uint32_t *ecc_failed);
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

// --- Bluetooth PTT and keys (bt_d578uv.c, hardware-d578uv.md 3.7), with CONFIG_BT_PTT. Input task ---
#if CONFIG_BT_PTT
void d578uv_bt_init(void);
uint32_t d578uv_bt_scan(void);		// key bits from the module, the probe while nothing has answered
void d578uv_bt_stats(hal_bt_stats_t *st);
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
// Side A's carrier in DMR, each status poll: tells the baseband when one comes up and when it goes (CARRIER_READY)
void d578uv_modem_carrier(bool carrier);
// The burst line's level changes seen while keyed (sct_dmr_port_t.burst_edges)
uint32_t d578uv_burst_edges(void);
#endif
