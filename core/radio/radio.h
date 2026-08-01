// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core: owns the channel / VFO state, squelch, PTT and the TX timer, and drives board.radio.
// It runs in its own task. The UI reads a snapshot and posts requests; it never touches the hardware.
#ifndef RADIO_CORE_H
#define RADIO_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

typedef enum {
	RS_IDLE,	// receiving, squelch closed
	RS_RX,		// receiving, squelch open (carrier and tone)
	RS_TX,
	RS_TX_DENIED,	// PTT held but transmit refused (RX-only, out of band, timed out); shown until PTT release
} radio_state_t;

// TXDENY_MODE: the channel's mode can't transmit yet (a DMR channel while only FM exists)
typedef enum { TXDENY_NONE, TXDENY_RX_ONLY, TXDENY_BAND, TXDENY_TIMEOUT, TXDENY_NO_FREQ, TXDENY_MODE } tx_deny_t;

typedef struct {
	radio_state_t state;
	uint8_t tx_deny;	// tx_deny_t, while RS_TX_DENIED
	bool vfo;		// tuning a frequency by hand rather than a channel
	uint16_t zone_index;
	uint16_t zone_pos;	// position in the zone, 0-based
	uint16_t zone_size;
	uint16_t channel_number;	// codeplug channel number, 0 in VFO
	char zone_name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;		// what the radio is set to now (VFO: the VFO settings, with ch.name "VFO")
	uint8_t power;		// effective board power level index
	bool temporary;		// a temporary change (P7) is in effect on this channel
	uint8_t squelch;	// 0 = open .. 9 = tight
	bool monitor;		// squelch held open by the user
	int16_t rssi_dbm;
	uint8_t noise;
	uint32_t tx_elapsed_ms;	// time in TX, while RS_TX
	uint16_t tot_s;		// TX time-out, 0 = none
	uint32_t last_rx_ms;	// hal_millis() at the end of the last reception, 0 = none yet
	uint32_t vfo_step_hz;
	codeplug_source_t source;
} radio_view_t;

void radio_core_start(void);
// A consistent copy of the current state
void radio_get_view(radio_view_t *v);
// Bumped on every visible change, so the UI can redraw only when needed
uint32_t radio_view_serial(void);

// Requests (queued, handled by the radio task)
void radio_req_channel_step(int8_t dir);	// next / previous channel in the zone, wraps
void radio_req_zone_pos(uint16_t pos);		// jump to a position in the zone
void radio_req_zone(uint16_t index);
void radio_req_vfo(bool on);
void radio_req_vfo_step(int8_t dir);
void radio_req_vfo_set(uint32_t hz);		// rx and tx frequency (simplex)
void radio_req_ptt(bool down);
void radio_req_monitor(bool down);
void radio_req_power(int8_t dir);		// temporary change of power level, wraps
void radio_req_squelch(int8_t dir);
void radio_req_clear_temporary(void);		// BACK on Home: revert temporary changes

#endif
