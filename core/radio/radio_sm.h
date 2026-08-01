// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine, without the RTOS: it takes requests and the time, polls board.radio and keeps
// the view. radio_task.c runs it in a task; the host tests run it against a fake board.
#ifndef RADIO_SM_H
#define RADIO_SM_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"

#define RADIO_POLL_MS		25	// status poll and squelch period
#define RADIO_TOT_DEFAULT_S	180	// TX time-out
#define RADIO_SQUELCH_DEFAULT	5
#define RADIO_SQUELCH_MAX	9
#define RADIO_VFO_STEP_HZ	12500

// Software squelch on the chip's noise reading (1 dB units, larger = more noise). Level 0 is always open.
// Level 1..9 opens when noise < RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP, so 1 = 64 (loose) .. 9 = 40 (tight),
// and closes once noise rises RADIO_SQL_HYST_DB above that for RADIO_SQL_CLOSE_POLLS polls in a row.
#define RADIO_SQL_OPEN_L1	64
#define RADIO_SQL_DB_PER_STEP	3
#define RADIO_SQL_HYST_DB	3
#define RADIO_SQL_CLOSE_POLLS	2

// RSSI moves smaller than this don't bump the view serial (the snapshot still carries the latest value)
#define RADIO_RSSI_DEADBAND_DB	2

typedef enum {
	RREQ_CH_STEP,		// arg: direction
	RREQ_ZONE_POS,		// arg: position in the zone
	RREQ_ZONE,		// arg: zone index
	RREQ_VFO,		// arg: 1 on, 0 off
	RREQ_VFO_STEP,		// arg: direction
	RREQ_VFO_SET,		// arg: Hz
	RREQ_PTT,		// arg: 1 down, 0 up
	RREQ_MONITOR,		// arg: 1 down, 0 up
	RREQ_POWER,		// arg: direction
	RREQ_SQUELCH,		// arg: direction
	RREQ_CLEAR_TEMP,
} radio_req_type_t;

typedef struct {
	uint8_t type;		// radio_req_type_t
	int32_t arg;
} radio_req_t;

typedef struct {
	radio_view_t view;
	uint32_t serial;		// bumped on every visible change

	zone_t zone;			// the current zone's channel list
	channel_t vfo_ch;		// the VFO's settings, kept while on a channel
	bool vfo_seeded;		// vfo_ch holds a frequency
	bool have_channels;		// some zone holds a readable channel

	uint8_t base_squelch;		// squelch without the temporary change
	bool temp_power;
	bool temp_squelch;

	bool ptt;			// PTT held
	uint32_t tx_start_ms;
	uint8_t close_count;		// polls in a row above the close threshold
	int16_t rssi_published;
	int8_t audio_out;		// last speaker / LED settings sent to the board, -1 = not yet
	int8_t green_out;
	int8_t red_out;
} radio_sm_t;

// Start-up: codeplug, zone 0, the first channel (or the VFO), FM receive
void radio_sm_init(radio_sm_t *sm, uint32_t now_ms);
void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms);
// Every RADIO_POLL_MS: status, squelch, audio, LEDs, TX time-out
void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms);

// Squelch thresholds for a level (exposed for the tests)
uint8_t radio_sm_squelch_open_below(uint8_t level);

#endif
