// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D RF chips, whichever the radio has (docs/spec/hardware-db25d.md 4.2, 4.8). One image serves every batch: the
// main transceiver is a BK4819 (PRO) or an FD6818 (older batches), the LO a BK4819, an FD6818, the 0x7813 part or an
// AT1141. rfchip_db25d.c reads register 0 of each chip at start-up and drives the pair it finds; radio_db25d.c and
// dmr_db25d.c only call the functions below.
#ifndef RFCHIP_DB25D_H
#define RFCHIP_DB25D_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"

typedef enum { DB25D_AF_MUTE, DB25D_AF_FM, DB25D_AF_DMR } db25d_af_t;

// The bus pins to idle (enables high, clock low) and both chips' buses: first in the radio init, before the RF paths
void db25d_rf_bus_init(void);
// Read both chips' IDs and set them up. False: a chip RadioFoundry doesn't drive (nothing else is written then)
bool db25d_rf_init(void);
uint16_t db25d_rf_main_id(void);
uint16_t db25d_rf_lo_id(void);
// The batch code the stock firmware shows for this pair ("DBB", "D64", "D66", "D6T"; "D??" for another): out holds 4
void db25d_rf_variant(char out[4]);
uint32_t db25d_rf_writes(void);

// ---- Main transceiver ----

void db25d_main_idle(void);
void db25d_main_rx_state(void);
// The main chip's band flag (the TX band) for hz (hardware-db25d.md 4.8)
void db25d_main_band(uint32_t hz);
// Idle, then the 51.55 MHz IF (receive)
void db25d_main_to_if(void);
void db25d_main_bandwidth(radio_bw_t bw);
// Receive sub-audio, TONE_* of radio.h (value: Hz x 10 or the 9-bit DCS code)
void db25d_main_rx_tone(uint8_t type, uint16_t value);
bool db25d_main_tone_heard(uint16_t r0c, uint8_t type);
void db25d_main_af(db25d_af_t af);
void db25d_main_af_gain(uint8_t gain);	// 0..15
// Status registers: RSSI (0x67), noise (0x65), sub-audio flags (0x0C)
void db25d_main_status(uint16_t *r67, uint16_t *r65, uint16_t *r0c);

// FM transmit: words written while still receiving, then the tune into TX, the end back to RX on the TX frequency
void db25d_main_tx_prepare(uint8_t type, uint16_t value, uint32_t hz, radio_bw_t bw);
void db25d_main_tx_tune(uint32_t hz);
void db25d_main_tx_end(void);
bool db25d_main_tail(bool on);		// DCS turn-off tail on (false: the transmission sends no DCS) or off (true)
void db25d_main_tone(uint16_t hz, uint8_t gain);	// hz 0 = stop
bool db25d_main_tone_on(void);
void db25d_main_tone_key(bool down);
void db25d_main_dtmf(uint16_t low_hz, uint16_t high_hz);

// DMR: the digital set (af_gain = the AF DAC gain the modem hears), its end, the TX words, the per-burst tune
void db25d_main_dmr_enter(uint8_t af_gain);
void db25d_main_dmr_leave(void);
void db25d_main_dmr_tx_prepare(uint32_t hz);
void db25d_main_burst(uint32_t hz, bool tx);

// ---- LO ----

// Run the synthesizer at hz (receive), off for a transmission, back on after it
void db25d_lo_tune(uint32_t hz);
void db25d_lo_off(void);
void db25d_lo_resume(void);
// The LO's output that follows ADC 7 (hardware-db25d.md 3.3); the chip writes it only when it changes
void db25d_lo_aux(bool high);

#endif
