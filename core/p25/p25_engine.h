// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional voice on a channel (docs/spec/p25.md 6): the call layer (p25.h) on an air interface session
// (core/air/air_session.h). The radio core begins it on a P25 channel and ends it on leaving; in between it
// receives calls, hands their voice frames to a sink, and transmits when the radio core says so. No RTOS call:
// the radio core's task and the provider's task use it as air_session.h describes, voice frames cross to and from
// the audio side through the caller's hooks.
#ifndef RFOUNDRY_P25_ENGINE_H
#define RFOUNDRY_P25_ENGINE_H

#include "p25.h"
#include "../air/air_session.h"

typedef struct {
	// Receive, from the provider's task. start: a call we take began (its fields may still fill in: update)
	void (*rx_start)(const p25_call_t *c, void *ctx);
	void (*rx_update)(const p25_call_t *c, void *ctx);
	void (*rx_voice)(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx);
	void (*rx_end)(void *ctx);
	// Transmit, from the provider's task: the next encoded voice frame, false when none is ready. NULL: every
	// frame is `silence`
	bool (*tx_voice)(uint8_t imbe[P25_IMBE_BYTES], void *ctx);
#if CONFIG_P25_TRUNK
	// A trunked channel (cfg.trunk), from the provider's task: every unit taken, trunking signaling included, and
	// every call's fields as they come in, played or not (a trunking follower, p25_follow.h, takes both)
	void (*rx_unit)(const p25_rx_unit_t *u, void *ctx);
	void (*rx_call)(const p25_call_t *c, void *ctx);
#endif
} p25_engine_hooks_t;

typedef struct {
	uint16_t rx_nac;	// P25_NAC_ANY: every system's calls
	uint16_t tx_nac;
	// Which calls of our NAC are played: 0 every one (the normal squelch of a conventional channel), or only calls
	// to this talk group, to everyone (P25_TGID_EVERYONE) and unit to unit calls to unit_id (selective squelch)
	uint16_t rx_tgid;
	uint32_t unit_id;	// ours: the source of what we send
	p25_lc_t tx_lc;		// what we call: a talk group or a unit (src is set from unit_id)
	bool talkaround;	// status symbols say direct (P25_SS_TALKAROUND); else unknown, as to a repeater
	const uint8_t *silence;	// a voice frame of silence (P25_IMBE_BYTES), from the vocoder
#if CONFIG_P25_TRUNK
	bool trunk;		// trunking signaling is received too (a control channel, or a voice channel it granted)
#endif
} p25_engine_cfg_t;

typedef struct {
	bool rx_active;		// a call is being received (whether or not it is played)
	bool rx_played;
	p25_call_t rx;		// its fields; kept after it ends (the last call heard)
	uint8_t tx;		// air_tx_state_t
	uint32_t rx_calls, tx_calls;
	uint32_t tx_filler;	// silence frames sent because the encoder had none ready
} p25_engine_view_t;

typedef struct {
	air_session_t session;
	p25_call_rx_t rx;
	p25_call_tx_t tx;
	p25_engine_cfg_t cfg;
	p25_engine_hooks_t hooks;
	void *ctx;
	volatile bool tx_stop;		// the radio core let go: end at the next unit boundary
	bool tx_ended;			// ... and the call layer knows
	bool played;			// the call in hand passes the squelch
	volatile bool recheck;		// p25_engine_set changed what passes it
	volatile uint8_t seq;		// odd while the provider's task writes view
	p25_engine_view_t view;
	uint8_t frame[P25_UNIT_BYTES_MAX];	// the framer's unit buffer
} p25_engine_t;

// What the board's provider gives P25: AIR_FIT_xxx, 0 = this board can't
uint32_t p25_engine_fit(const hal_air_t *air);
// Start on the tuned channel (the board in its digital mode, its DMR stack stopped). False: no provider, no fit
bool p25_engine_begin(p25_engine_t *e, const hal_air_t *air, const p25_engine_cfg_t *cfg, const p25_engine_hooks_t *hooks,
		      void *ctx);
void p25_engine_end(p25_engine_t *e);
// The receiver is on a new frequency: what it held is dropped (on a trunked channel, the call in hand ends)
void p25_engine_retuned(p25_engine_t *e);
// Change what is received and sent while running (a talk group or NAC change that needs no retune). Not while
// transmitting
void p25_engine_set(p25_engine_t *e, const p25_engine_cfg_t *cfg);

// Transmit (the radio core's task). prepare: the header is queued; then hal_radio_t.tx(), then go. stop: the
// operator let go: the LDU on air is finished and a terminator sent. state: AIR_TX_DONE = switch the transmitter
// off (hal_radio_t.tx_off), then clear
bool p25_engine_tx_prepare(p25_engine_t *e);
bool p25_engine_tx_go(p25_engine_t *e);
void p25_engine_tx_stop(p25_engine_t *e);
void p25_engine_tx_abort(p25_engine_t *e);
air_tx_state_t p25_engine_tx_state(const p25_engine_t *e);
void p25_engine_tx_clear(p25_engine_t *e);
#if CONFIG_P25_TRUNK
// An inbound trunking block (p25_tsbk.h, sealed) as a one-block TSDU on the tx NAC, in place of a call: prepare as
// p25_engine_tx_prepare, then the same hal_radio_t.tx(), go, state and clear
bool p25_engine_tx_burst(p25_engine_t *e, const uint8_t blk[12]);
#endif

// A copy of the state for the radio core's task
void p25_engine_view(const p25_engine_t *e, p25_engine_view_t *out);

#endif
