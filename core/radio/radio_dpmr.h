// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR on a channel, the radio core's side (docs/spec/dpmr.md 7): the engine (core/dpmr/dpmr_engine.h) on the memory it
// runs in, its voice frames to and from the vocoder the board hosts for DMR (core/dmr/ambe), the channel's dPMR
// parameters, and a compact view for the radio view. radio_sm.c decides when dPMR runs and keys the transmitter
// (radio_air.h); nothing here keys it, and nothing here decides where it may: the radio core's checks are the same for
// every mode.
//
// The parameters of a stored dPMR channel are its own (dpmr.md 6); the VFO and a channel whose mode was changed to
// dPMR for now take the RAM settings the debug link changes (radio_req_dpmr_settings), as NXDN's do.
#ifndef RADIO_DPMR_H
#define RADIO_DPMR_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"

// The dPMR settings (RAM only, reset at start-up; dpmr.md 7.2): the VFO's, and the shape of what is in force
typedef struct {
	uint32_t called;	// who PTT calls (24 bits): a common ID (n << 16 | 0xFFFF), the all call, or an address
	uint32_t own;		// who we are (24 bits); 0: the called ID, as an initial-services radio sends
	uint8_t cc;		// 0: the code table 6.1 ties to the transmit frequency; 1-16 group A, 17-32 group B
	bool rx_mine;		// play only calls to `called`, to all and to us; else every call
} radio_dpmr_settings_t;

#define RADIO_DPMR_ID_MAX	0xFFFFFFu
#define RADIO_DPMR_CC_MAX	32u

// view flags
#define RADIO_DPMR_ON		0x01u	// the engine runs on this channel
#define RADIO_DPMR_RX		0x02u	// a call is being received
#define RADIO_DPMR_PLAYED	0x04u	// ... and passes the filter (its voice plays)
#define RADIO_DPMR_LATE		0x08u	// ... was entered without its header
#define RADIO_DPMR_ENDED	0x10u	// the last call heard ended with its end frame (else the signal went)
#define RADIO_DPMR_MINE		0x20u	// the filter in force plays only calls to us (rx_mine)
#define RADIO_DPMR_KNOWN	0x40u	// the call's called ID is in (a late entry's comes in the CCH)

// The radio view's part (radio_view_t.dpmr): the call heard now, or the last one, and what PTT sends
typedef struct {
	uint8_t flags;		// RADIO_DPMR_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint8_t have;		// DPMR_HAVE_* of the call heard: which of its fields below are known
	uint16_t fit;		// what the board's provider gives dPMR (AIR_FIT_*), 0: nothing
	uint16_t cc;		// the call's channel code (12 bits)
	uint16_t tx_cc;		// the channel code PTT sends; 0 while not running on the frequency's automatic one
	uint32_t called, own;	// the call's IDs (24 bits)
	uint32_t talk;		// who PTT calls
	uint32_t tx_own;	// the own ID PTT sends
} radio_dpmr_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while dPMR runs)
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t headers, frames, ends, hi_fail, cch_fail, end_fail, voice, voice_held, ham_fixed, ham_bad;	// the call layer's
	uint32_t rejects;	// payload syncs the receiver turned down (a channel code read as FS2)
	uint32_t rx_chunks, tx_underruns, tx_syms;	// the session's
	uint32_t begins, begin_fails;	// engine starts, and the ones the provider refused (no provider, no fit, no memory)
} radio_dpmr_stats_t;

#if CONFIG_DPMR
#include "radio/radio_air.h"

extern const radio_air_ops_t radio_dpmr_air;

void radio_dpmr_init(void);
// The board's provider gives dPMR something, receive or transmit (any task): what the UI and the mode request offer it on
bool radio_dpmr_fits(void);
// What the provider gives dPMR (AIR_FIT_*; any task)
uint32_t radio_dpmr_fit(void);
// A channel read from the codeplug, as the radio runs it: a live dPMR channel (kind 7) becomes RADIO_MODE_DPMR where the
// board carries dPMR and its frequency allows a digital mode. False: left as read (parked)
bool radio_dpmr_take(channel_t *ch);
// The board's provider receives dPMR: what scan and dual watch visit
bool radio_dpmr_receives(void);
void radio_dpmr_settings(radio_dpmr_settings_t *out);		// any task
void radio_dpmr_set_settings(const radio_dpmr_settings_t *s);	// radio task (RREQ_DPMR_SET)

// The parameters in force, as settings (own and cc as sent, never 0): false when the engine isn't running. Any task: a
// read that crosses a change of channel may mix the two (diagnostics only)
bool radio_dpmr_in_force(radio_dpmr_settings_t *out);

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped; the audio stream starts. number: the
// stored channel ch is (0: the VFO, or none): a live dPMR channel's tag gives the parameters, else the settings. False:
// no memory, no provider or no fit (dPMR then receives nothing and refuses PTT)
bool radio_dpmr_begin(const channel_t *ch, uint16_t number);
void radio_dpmr_end(void);
bool radio_dpmr_running(void);
void radio_dpmr_retuned(void);
// The settings changed: the engine takes them (not while transmitting)
void radio_dpmr_apply(const channel_t *ch, uint16_t number);
// Every tick: the view and the counters. True when the view changed
bool radio_dpmr_poll(radio_dpmr_view_t *v);

void radio_dpmr_stats(radio_dpmr_stats_t *out);
void radio_dpmr_reset_stats(void);
#endif

#endif
