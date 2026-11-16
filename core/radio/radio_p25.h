// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional on a channel, the radio core's side (docs/spec/p25.md 7): the engine (core/p25/p25_engine.h) and
// the voice path (core/p25/p25Voice.h) on the memory they run in, the channel's P25 parameters, and a compact view
// for the radio view. radio_sm.c decides when P25 runs and keys the transmitter; nothing here keys it.
//
// The parameters come from radio_p25_params alone. Its only source for now is a RAM setting the debug link changes
// (radio_req_p25_settings); a codeplug reader takes its place once the codeplug carries P25 channels.
#ifndef RADIO_P25_H
#define RADIO_P25_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "hal_audio.h"

// The P25 settings (RAM only, reset at start-up)
#define RADIO_P25_TA_AUTO	0	// talkaround: on a simplex channel (TX = RX), else off (a repeater)
#define RADIO_P25_TA_ON		1
#define RADIO_P25_TA_OFF	2
typedef struct {
	uint16_t rx_nac;	// P25_NAC_ANY (0xF7E): every NAC
	uint16_t tx_nac;
	uint16_t rx_tgid;	// 0: every call of the NAC (the normal squelch); else only this talk group and calls to us
	uint8_t talkaround;	// RADIO_P25_TA_*
	bool unit_call;		// PTT calls a unit (tx_dst), else the talk group tx_dst
	uint32_t tx_dst;	// talk group (16 bits) or unit (24 bits) PTT calls
	uint32_t unit_id;	// ours (24 bits); 0: the radio's DMR ID
} radio_p25_settings_t;

// view flags
#define RADIO_P25_ON		0x01u	// the engine runs on this channel
#define RADIO_P25_RX		0x02u	// a call is being received
#define RADIO_P25_PLAYED	0x04u	// ... and passes the squelch (its voice plays)
#define RADIO_P25_ENC		0x08u	// ... and is encrypted (not played)
#define RADIO_P25_UNIT		0x10u	// ... is a unit to unit call (dst is a unit, else a talk group)
#define RADIO_P25_LATE		0x20u	// ... was entered without its header

// The radio view's part (radio_view_t.p25): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_P25_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint16_t nac;
	uint32_t src;		// the caller's unit ID, 0 not known yet
	uint32_t dst;		// the talk group or the unit called
} radio_p25_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while P25 runs)
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t units, voice, voice_bad, voice_bits, lc_fail, es_fail;	// the call layer's (p25_call_stats_t)
	uint32_t rx_chunks, tx_underruns, tx_syms;			// the session's
	uint32_t begins, begin_fails;	// engine starts, and the ones the provider refused (no provider, no fit)
	uint32_t mem_lent;		// starts that ran in memory lent by the vocoder
} radio_p25_stats_t;

#if CONFIG_P25
#include "p25/p25_engine.h"

void radio_p25_init(const hal_audio_t *audio);	// the PCM stream the voice plays on
// The board's air interface can receive P25 (any task): what the UI offers P25 on
bool radio_p25_fits(void);
void radio_p25_settings(radio_p25_settings_t *out);	// any task
void radio_p25_set_settings(const radio_p25_settings_t *s);	// radio task (RREQ_P25_SET)
// The engine's configuration for a channel: the one place P25 parameters come from
void radio_p25_params(const channel_t *ch, uint32_t own_id, p25_engine_cfg_t *cfg);

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped. False: no memory, no provider or
// no fit (P25 then receives nothing and refuses PTT)
bool radio_p25_begin(const channel_t *ch, uint32_t own_id);
void radio_p25_end(void);
bool radio_p25_running(void);
void radio_p25_retuned(void);
// The settings or the channel's parameters changed: the engine takes them (not while transmitting)
void radio_p25_apply(const channel_t *ch, uint32_t own_id);
// Every tick: the voice stream, the view and the counters. True when the view changed
bool radio_p25_poll(radio_p25_view_t *v);

// Transmit, as p25_engine.h
bool radio_p25_tx_prepare(void);
bool radio_p25_tx_go(void);
void radio_p25_tx_stop(void);
void radio_p25_tx_abort(void);
uint8_t radio_p25_tx_state(void);	// air_tx_state_t
void radio_p25_tx_clear(void);

void radio_p25_stats(radio_p25_stats_t *out);
void radio_p25_reset_stats(void);
#endif

#endif
