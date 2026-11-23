// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The part every air-framed voice engine shares (NXDN, YSF): the session, the call in hand and whether it plays, the
// view the radio core reads, and transmit: voice frames from the encoder into the protocol's call layer, silence when
// the encoder falls behind, its symbols into the session. The protocol keeps what differs: its call layer, which calls
// play (its squelch), what a transmission sends, and its own typed hooks, which it calls on what air_voice returns.
// Threads as air_session.h: receive and tx_fill in the provider's task, the rest in the radio core's.
#ifndef RFOUNDRY_AIR_VOICE_H
#define RFOUNDRY_AIR_VOICE_H

#include <stdbool.h>
#include <stdint.h>
#include "air_session.h"

#define AIR_VOICE_FRAME_MAX	9	// the largest voice frame a protocol hands on (DMR's 72-bit AMBE+2 frame)

// The protocol's transmit call layer, as tx_fill drives it (tx: its state)
typedef struct {
	uint16_t (*ready)(void *tx, uint16_t *first);	// symbols ready to send, from *first
	const uint8_t *(*syms)(void *tx);		// its symbol buffer
	void (*sent)(void *tx, uint16_t n);		// n of them went to the session
	bool (*done)(void *tx);				// the transmission is complete
	bool (*voice)(void *tx, const uint8_t *v);	// queue the next voice frame; false: no room
	void (*end)(void *tx, const uint8_t *silence);	// the operator let go: finish and end
} air_voice_tx_ops_t;

// What the radio core reads; the protocol's call fields go with it (air_voice_view)
typedef struct {
	bool rx_active;		// a call is being received (whether or not it is played)
	bool rx_played;
	uint8_t tx;		// air_tx_state_t
	uint32_t rx_calls, tx_calls;
	uint32_t tx_filler;	// silence frames sent because the encoder had none ready
} air_voice_view_t;

typedef struct {
	air_session_t session;
	const air_mode_t *mode;
	const air_voice_tx_ops_t *txo;
	void *tx;				// the protocol's transmit state
	bool (*tx_voice)(uint8_t *v, void *ctx);	// the encoder's next frame, false: none ready; NULL: silence
	void *ctx;
	const uint8_t *silence;			// a voice frame of silence, from the vocoder
	void *view_call;			// the protocol's copy of the call heard, published with view ...
	uint16_t call_size;			// ... and its size
	volatile bool tx_stop;			// the radio core let go: end at the next frame boundary
	bool tx_ended;				// ... and the call layer knows
	bool played;				// the call in hand passes the protocol's squelch
	volatile bool recheck;			// the protocol's settings changed what passes it
	volatile uint8_t seq;			// odd while the provider's task writes view
	air_voice_view_t view;
} air_voice_t;

// What a receive step means for the protocol's hooks
typedef enum { AIR_VOICE_NONE, AIR_VOICE_START, AIR_VOICE_UPDATE, AIR_VOICE_END } air_voice_event_t;

// Open the session on the tuned channel (air_session_open: mode_ctx the call layer's receiver, with tx_fill); the
// rest as the struct says
bool air_voice_open(air_voice_t *v, const hal_air_t *air, const air_mode_t *mode, void *mode_ctx,
		    uint8_t *frame, int8_t *soft, uint16_t frame_syms, const air_voice_tx_ops_t *txo, void *tx,
		    void *view_call, uint16_t call_size);
void air_voice_close(air_voice_t *v);
void air_voice_retuned(air_voice_t *v);

// Receive (the provider's task). fields: the call layer started (start) or updated call; play: it passes the
// protocol's squelch. The call's fields are published; the return says which hook the protocol calls. voice: true
// when its frame goes to the protocol's voice hook. end: the call ended
air_voice_event_t air_voice_rx_fields(air_voice_t *v, const void *call, bool start, bool play);
bool air_voice_rx_voice(const air_voice_t *v);
air_voice_event_t air_voice_rx_end(air_voice_t *v);
// The protocol's settings changed (not while transmitting): its squelch is applied again at the next voice frame
void air_voice_recheck(air_voice_t *v);
// At a voice frame: true once after air_voice_recheck (the protocol calls fields again with the call it holds)
bool air_voice_recheck_due(air_voice_t *v);

// Transmit (the radio core's task). can: open, the provider transmits, a silence frame (need_silence) given. begin:
// true when the session is idle, so the protocol queues its new transmission now; then prime (air_session_tx_prime),
// hal_radio_t.tx(), go. stop, abort, state, clear as air_session.h
bool air_voice_tx_can(const air_voice_t *v, bool need_silence);
bool air_voice_tx_begin(air_voice_t *v);
bool air_voice_tx_prime(air_voice_t *v);
bool air_voice_tx_go(air_voice_t *v);
void air_voice_tx_stop(air_voice_t *v);
void air_voice_tx_abort(air_voice_t *v);
air_tx_state_t air_voice_tx_state(const air_voice_t *v);
void air_voice_tx_clear(air_voice_t *v);

// A copy of the view and the call heard (call_out: call_size bytes) for the radio core's task
void air_voice_view(const air_voice_t *v, air_voice_view_t *out, void *call_out);

#endif
