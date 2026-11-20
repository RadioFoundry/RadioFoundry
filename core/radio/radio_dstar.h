// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a channel, the radio core's side (docs/spec/dstar.md 6): the engine (core/dstar/dstar_engine.h) and the
// voice path (core/dstar/dstarVoice.h) on the memory they run in, the header we send, and a compact view for the
// radio view. radio_sm.c decides when D-STAR runs and keys the transmitter (radio_air.h); nothing here keys it.
//
// What we send comes from the D-STAR settings (RAM; the debug link and the UI change them): our callsign (by default
// the callsign in the radio name), its suffix, the companion's ("CQCQCQ" by default) and the repeaters' ("DIRECT"
// both by default, simplex), and a message for the slow data.
#ifndef RADIO_DSTAR_H
#define RADIO_DSTAR_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"

#define RADIO_DSTAR_CALL	8
#define RADIO_DSTAR_SUFFIX	4
#define RADIO_DSTAR_MSG		20

// Text fields, NUL terminated
typedef struct {
	char my[RADIO_DSTAR_CALL + 1];		// "" = the callsign in the radio name
	char sfx[RADIO_DSTAR_SUFFIX + 1];
	char ur[RADIO_DSTAR_CALL + 1];		// "" = CQCQCQ
	char rpt1[RADIO_DSTAR_CALL + 1];	// the repeater we transmit to, with its module letter in the 8th place
						// ("" = DIRECT: simplex)
	char rpt2[RADIO_DSTAR_CALL + 1];	// its gateway ("" = rpt1 with G in the 8th place; DIRECT when simplex)
	char msg[RADIO_DSTAR_MSG + 1];		// the message in the slow data, "" none
} radio_dstar_settings_t;

// view flags
#define RADIO_DSTAR_ON		0x01u	// the engine runs on this channel
#define RADIO_DSTAR_RX		0x02u	// a call is being received
#define RADIO_DSTAR_HDR		0x04u	// ... and its header is known (callsigns below)
#define RADIO_DSTAR_LATE	0x08u	// ... joined after the header
#define RADIO_DSTAR_HAS_MSG	0x10u	// ... has sent a message
#define RADIO_DSTAR_RPT		0x20u	// ... came through a repeater
#define RADIO_DSTAR_EMERG	0x40u	// ... has the emergency flag

// The radio view's part (radio_view_t.dstar): the call heard now, or the last one. Callsigns NUL terminated, trailing
// spaces dropped
typedef struct {
	uint8_t flags;		// RADIO_DSTAR_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint16_t fit;		// what the board's provider gives D-STAR (AIR_FIT_*), 0: nothing
	char my[RADIO_DSTAR_CALL + 1];
	char sfx[RADIO_DSTAR_SUFFIX + 1];
	char ur[RADIO_DSTAR_CALL + 1];
	char rpt1[RADIO_DSTAR_CALL + 1];
	char msg[RADIO_DSTAR_MSG + 1];
	// What PTT sends: the companion (UR) and the repeater (RPT1, "" simplex)
	char talk[RADIO_DSTAR_CALL + 1];
	char talk_rpt[RADIO_DSTAR_CALL + 1];
} radio_dstar_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while D-STAR runs)
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t headers, header_fail, late, frames, voice, syncs, ends, lost, slow_msgs, slow_headers, slow_text;	// the call layer's
	uint32_t rx_chunks, tx_underruns, tx_syms;	// the session's
	uint32_t begins, begin_fails;	// engine starts, and the ones refused (no provider, no fit, no memory)
} radio_dstar_stats_t;

#if CONFIG_DSTAR
#include "radio/radio_air.h"
#include "hal_audio.h"

extern const radio_air_ops_t radio_dstar_air;

void radio_dstar_init(const hal_audio_t *audio);
// The board's provider gives D-STAR something (receive or transmit; any task): what the UI and the mode request offer
// D-STAR on
bool radio_dstar_fits(void);
uint32_t radio_dstar_fit(void);
void radio_dstar_settings(radio_dstar_settings_t *out);		// any task
void radio_dstar_set_settings(const radio_dstar_settings_t *s);	// radio task (RREQ_DSTAR_SET)

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped. callsign: the radio's own (from its
// name; "" none), what MY is when the settings leave it empty. False: no memory, no provider or no fit (D-STAR then
// receives nothing and refuses PTT)
bool radio_dstar_begin(const channel_t *ch, const char *callsign);
void radio_dstar_end(void);
bool radio_dstar_running(void);
void radio_dstar_retuned(void);
// The settings changed: the engine takes them (not while transmitting)
void radio_dstar_apply(const char *callsign);
// Every tick: the view and the counters. True when the view changed
bool radio_dstar_poll(radio_dstar_view_t *v);

void radio_dstar_stats(radio_dstar_stats_t *out);
void radio_dstar_reset_stats(void);
// What we send now, as on air (the header's fields): for the debug link and the UI. False when D-STAR isn't running
bool radio_dstar_tx_header(char my[RADIO_DSTAR_CALL + 1], char ur[RADIO_DSTAR_CALL + 1], char rpt1[RADIO_DSTAR_CALL + 1],
			   char rpt2[RADIO_DSTAR_CALL + 1]);
#endif

#endif
