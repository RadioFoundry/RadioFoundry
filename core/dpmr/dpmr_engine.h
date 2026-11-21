// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR voice on a channel (docs/spec/dpmr.md 5): the call layer (dpmr.h) on an air interface session
// (core/air/air_session.h), the way NXDN's engine is (core/nxdn/nxdn_engine.h). The radio core begins it on a dPMR
// channel and ends it on leaving; in between it receives calls, hands their voice frames to a sink, and transmits
// when the radio core says so. No RTOS call: the radio core's task and the provider's task use it as air_session.h
// describes, voice frames cross to and from the audio side through the caller's hooks.
//
// It decides nothing about whether keying is allowed: the radio core's band, inhibit and power checks do, before it
// calls hal_radio_t.tx().
#ifndef RFOUNDRY_DPMR_ENGINE_H
#define RFOUNDRY_DPMR_ENGINE_H

#include "dpmr.h"
#include "../air/air_session.h"

typedef struct {
	// Receive, from the provider's task. start: a call we play began (its IDs may still fill in: update). voice: a
	// 72-bit TCH as on air; its FEC (the same as DMR's voice frames) is the voice path's
	void (*rx_start)(const dpmr_call_t *c, void *ctx);
	void (*rx_update)(const dpmr_call_t *c, void *ctx);
	void (*rx_voice)(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx);
	void (*rx_end)(void *ctx);
	// Transmit, from the provider's task: the next 72-bit voice frame with its FEC, false when none is ready. NULL:
	// every frame is `silence`
	bool (*tx_voice)(uint8_t ambe[DPMR_AMBE_BYTES], void *ctx);
} dpmr_engine_hooks_t;

typedef struct {
	uint32_t called;	// who PTT calls (24 bits): a common ID, DPMR_ID_ALL or an individual address
	uint32_t own;		// who we are: the source of what we send
	uint16_t cc;		// the channel code we send (12 bits)
	// Which calls are played: false every one, true only calls to `called`, to all, and to us (a late entry plays
	// until its called ID is in: a call heard is more use than one cut for not yet knowing whom it is for)
	bool rx_mine;
	bool tx_inhibit;
	uint8_t headers;	// header frames ahead of the voice (0: one)
	const uint8_t *silence;	// a voice frame of silence (DPMR_AMBE_BYTES), from the vocoder
} dpmr_engine_cfg_t;

typedef struct {
	bool rx_active;		// a call is being received (whether or not it is played)
	bool rx_played;
	dpmr_call_t rx;		// its fields; kept after it ends (the last call heard)
	uint8_t tx;		// air_tx_state_t
	uint32_t rx_calls, tx_calls;
	uint32_t tx_filler;	// silence frames sent because the encoder had none ready
} dpmr_engine_view_t;

typedef struct {
	air_session_t session;
	dpmr_call_rx_t rx;
	dpmr_call_tx_t tx;
	dpmr_engine_cfg_t cfg;
	dpmr_engine_hooks_t hooks;
	void *ctx;
	volatile bool tx_stop;		// the radio core let go: finish the superframe, then the end frame
	bool tx_ended;			// ... and the call layer knows
	bool played;			// the call in hand passes the filter
	volatile bool recheck;		// dpmr_engine_set changed what passes it
	volatile uint8_t seq;		// odd while the provider's task writes view
	dpmr_engine_view_t view;
	uint8_t frame[DPMR_FRAME_BYTES];	// the framer's frame buffer: one payload frame, or a header (no soft
						// values: dPMR's block codes take hard bits)
} dpmr_engine_t;

// What the board's provider gives dPMR: AIR_FIT_xxx, 0 = this board can't
uint32_t dpmr_engine_fit(const hal_air_t *air);
// Start on the tuned channel (the board in its digital mode, its DMR stack stopped). False: no provider, no fit
bool dpmr_engine_begin(dpmr_engine_t *e, const hal_air_t *air, const dpmr_engine_cfg_t *cfg,
		       const dpmr_engine_hooks_t *hooks, void *ctx);
void dpmr_engine_end(dpmr_engine_t *e);
// The receiver is on a new frequency: what it held is dropped
void dpmr_engine_retuned(dpmr_engine_t *e);
// Change what is received and sent while running. Not while transmitting
void dpmr_engine_set(dpmr_engine_t *e, const dpmr_engine_cfg_t *cfg);

// Transmit (the radio core's task). prepare: the headers are queued; then hal_radio_t.tx(), then go. stop: the
// operator let go: the superframe on air is finished and the end frame sent. state: AIR_TX_DONE = switch the
// transmitter off (hal_radio_t.tx_off), then clear
bool dpmr_engine_tx_prepare(dpmr_engine_t *e);
bool dpmr_engine_tx_go(dpmr_engine_t *e);
void dpmr_engine_tx_stop(dpmr_engine_t *e);
void dpmr_engine_tx_abort(dpmr_engine_t *e);
air_tx_state_t dpmr_engine_tx_state(const dpmr_engine_t *e);
void dpmr_engine_tx_clear(dpmr_engine_t *e);

// A copy of the state for the radio core's task
void dpmr_engine_view(const dpmr_engine_t *e, dpmr_engine_view_t *out);

#endif
