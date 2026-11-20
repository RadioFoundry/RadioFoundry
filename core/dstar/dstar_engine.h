// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a channel (docs/spec/dstar.md 6): the call layer (dstar.h) on an air interface session
// (core/air/air_session.h). The radio core begins it on a D-STAR channel and ends it on leaving; in between it receives
// calls, hands their voice frames to a sink, and transmits when the radio core says so. No RTOS call: the radio core's
// task and the provider's task use it as air_session.h describes; voice frames cross to and from the audio side through
// the caller's hooks.
#ifndef RFOUNDRY_DSTAR_ENGINE_H
#define RFOUNDRY_DSTAR_ENGINE_H

#include "dstar.h"
#include "../air/air_session.h"

typedef struct {
	// Receive, from the provider's task: a call began (its fields may still fill in: update), a voice frame as on air,
	// the call ended
	void (*rx_start)(const dstar_call_t *c, void *ctx);
	void (*rx_update)(const dstar_call_t *c, void *ctx);
	void (*rx_voice)(const uint8_t voice[DSTAR_VOICE_BYTES], void *ctx);
	void (*rx_end)(void *ctx);
	// Transmit, from the provider's task: the next voice frame as on air, false when none is ready. NULL: every frame
	// is silence
	bool (*tx_voice)(uint8_t voice[DSTAR_VOICE_BYTES], void *ctx);
} dstar_engine_hooks_t;

typedef struct {
	dstar_header_t tx_hdr;		// what we send: our callsigns, the companion's and the repeaters'
	char tx_msg[DSTAR_MSG_LEN + 1];	// the message in the slow data, "" none
	bool tx_inhibit;
	const uint8_t *silence;		// a voice frame of silence (DSTAR_VOICE_BYTES)
} dstar_engine_cfg_t;

typedef struct {
	bool rx_active;			// a call is being received
	dstar_call_t rx;		// its fields; kept after it ends (the last call heard)
	uint8_t tx;			// air_tx_state_t
	uint32_t rx_calls, tx_calls;
	uint32_t tx_filler;		// silence frames sent because the encoder had none ready
} dstar_engine_view_t;

typedef struct {
	air_session_t session;
	dstar_call_rx_t rx;		// the framer's context
	dstar_call_tx_t tx;
	dstar_engine_cfg_t cfg;
	dstar_engine_hooks_t hooks;
	void *ctx;
	volatile bool tx_stop;		// the radio core let go: end at the next frame boundary
	bool tx_ended;			// ... and the call layer knows
	volatile uint8_t seq;		// odd while the provider's task writes view
	dstar_engine_view_t view;
	uint8_t frame[(24 + DSTAR_HDR_SYMS + 3) / 4];	// the framer's buffer: the longest unit, the header's ...
	int8_t soft[24 + DSTAR_HDR_SYMS];		// ... and its soft values, for the header's Viterbi decoder
} dstar_engine_t;

// What the board's provider gives D-STAR: AIR_FIT_xxx, 0 = this board can't
uint32_t dstar_engine_fit(const hal_air_t *air);
// Start on the tuned channel (the board in its digital mode, its DMR stack stopped). False: no provider, no fit
bool dstar_engine_begin(dstar_engine_t *e, const hal_air_t *air, const dstar_engine_cfg_t *cfg,
			const dstar_engine_hooks_t *hooks, void *ctx);
void dstar_engine_end(dstar_engine_t *e);
void dstar_engine_retuned(dstar_engine_t *e);
// What we send changed (not while transmitting)
void dstar_engine_set(dstar_engine_t *e, const dstar_engine_cfg_t *cfg);

// Transmit (the radio core's task), as nxdn_engine.h: prepare (the preamble and header queued), hal_radio_t.tx(), go;
// stop when the operator lets go; state AIR_TX_DONE: tx_off, then clear
bool dstar_engine_tx_prepare(dstar_engine_t *e);
bool dstar_engine_tx_go(dstar_engine_t *e);
void dstar_engine_tx_stop(dstar_engine_t *e);
void dstar_engine_tx_abort(dstar_engine_t *e);
air_tx_state_t dstar_engine_tx_state(const dstar_engine_t *e);
void dstar_engine_tx_clear(dstar_engine_t *e);

// A copy of the state for the radio core's task
void dstar_engine_view(const dstar_engine_t *e, dstar_engine_view_t *out);

#endif
