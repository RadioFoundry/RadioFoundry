// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A session on the air interface (docs/spec/air-interface.md 7): one mode on one provider (hal_air.h) with the
// framer between them (air_framer.h). It opens and closes the provider, feeds received chunks to the framer, and
// runs a transmission from the first queued symbol to AIR_EV_TX_DONE. It knows no mode and makes no RTOS call.
//
// Two contexts use it. The owner's task (the radio core's) opens, closes, primes and starts a transmission and
// switches the transmitter; the provider's task delivers events, and everything a mode's hooks do on receive and
// tx_fill does once a transmission runs happens there. The hand-over is air_session_tx_go: before it only the
// owner's task touches the transmit side, after it only the provider's. Flags between the two are single bytes.
#ifndef RFOUNDRY_AIR_SESSION_H
#define RFOUNDRY_AIR_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "air_framer.h"

typedef enum {
	AIR_TX_IDLE,
	AIR_TX_PRIMING,		// symbols are being queued, nothing is keyed
	AIR_TX_ON,		// the provider modulates; tx_fill keeps it fed
	AIR_TX_ENDING,		// the last symbol is queued
	AIR_TX_DONE,		// AIR_EV_TX_DONE came: the owner switches the transmitter off, then air_session_tx_clear
} air_tx_state_t;

typedef struct {
	// The provider's queue has room: add symbols with air_session_tx, or end with air_session_tx_finish. Called by
	// air_session_tx_prime (owner's task) and, once the transmission runs, on the provider's events. Not adding
	// any is allowed (nothing ready yet): it is called again when the next chunk has left
	void (*tx_fill)(void *ctx);
	// The transmission has left the air (provider's task). May be NULL: the owner polls air_session_tx_state
	void (*tx_done)(void *ctx);
} air_session_ops_t;

typedef struct {
	uint32_t rx_chunks, rx_syms;
	uint32_t tx_chunks_started;	// transmissions the provider started
	uint32_t tx_underruns;
	uint32_t tx_syms;
	uint32_t tx_started_us;		// the provider's clock at the latest AIR_EV_TX_STARTED
} air_session_stats_t;

typedef struct {
	const hal_air_t *air;		// NULL: closed
	uint32_t fit;
	air_framer_t fr;
	air_session_ops_t ops;
	void *ctx;
	volatile uint8_t tx;		// air_tx_state_t
	volatile bool finish;		// air_session_tx_finish was called ...
	volatile bool finished;		// ... and the last chunk is in the provider's queue (it may have had to wait for room)
	air_session_stats_t stats;
} air_session_t;

// What the provider gives the mode (AIR_FIT_xxx), 0 without a provider or a fit
uint32_t air_session_fit(const hal_air_t *air, const air_mode_t *mode);
// Open: the board is tuned and in its digital mode, its DMR stack stopped. mode_ctx goes to the mode's hooks; hard,
// soft and buf_syms are the framer's frame buffer (air_framer_init). False: no fit, or the provider refused
bool air_session_open(air_session_t *s, const hal_air_t *air, const air_mode_t *mode, void *mode_ctx, uint8_t *hard,
		      int8_t *soft, uint16_t buf_syms, const air_session_ops_t *ops, void *ctx);
// Close (a transmission still running is aborted; the owner switches the transmitter off)
void air_session_close(air_session_t *s);
bool air_session_is_open(const air_session_t *s);
// After a retune: the receiver and the framer start over
void air_session_retuned(air_session_t *s);

// Transmit, owner's task: prime (calls tx_fill; true once a chunk is queued), switch the transmitter on
// (hal_radio_t.tx), go. False from go: the provider refused; the owner switches off again
bool air_session_tx_prime(air_session_t *s);
bool air_session_tx_go(air_session_t *s);
// Stop at once, whatever is queued (owner's task). AIR_TX_DONE follows
void air_session_tx_abort(air_session_t *s);
air_tx_state_t air_session_tx_state(const air_session_t *s);
// After AIR_TX_DONE and hal_radio_t.tx_off: back to AIR_TX_IDLE
void air_session_tx_clear(air_session_t *s);

// Inside tx_fill: symbols the provider's queue takes now; queue n dibits from symbol `first` of syms (false, none
// taken, when they don't fit); the symbols queued but not yet on air (whole chunks); the last symbol is in
uint16_t air_session_tx_space(const air_session_t *s);
bool air_session_tx(air_session_t *s, const uint8_t *syms, uint16_t first, uint16_t n);
uint16_t air_session_tx_queued(const air_session_t *s);
void air_session_tx_finish(air_session_t *s);

#endif
