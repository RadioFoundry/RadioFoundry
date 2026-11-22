// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional on a channel, the radio core's side (docs/spec/nxdn.md 6): the engine (core/nxdn/nxdn_engine.h)
// on the memory it runs in, its voice frames to and from the vocoder the board hosts for DMR (core/dmr/ambe), the
// channel's NXDN parameters, and a compact view for the radio view. radio_sm.c decides when NXDN runs and keys the
// transmitter (radio_air.h); nothing here keys it.
//
// The parameters of a stored NXDN channel are its own (docs/spec/nxdn-codeplug.md); the VFO and a channel whose mode
// was changed to NXDN for now take the RAM settings the debug link changes (radio_req_nxdn_settings).
#ifndef RADIO_NXDN_H
#define RADIO_NXDN_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#if CONFIG_NXDN_OTA_ALIAS
#include "nxdn/nxdn.h"
#endif

// The NXDN settings (RAM only, reset at start-up; nxdn.md 6.2): the VFO's, and the shape of what is in force
typedef struct {
	bool rate9600;		// 9600 bit/s (4800 symbols/s), else 4800 bit/s (2400 symbols/s)
	uint8_t rx_ran;		// 0 (every RAN) to 63
	uint8_t tx_ran;		// 1 to 63 (0 sends RAN 0)
	uint16_t rx_gid;	// 0: every call of the RAN (the RAN squelch); else group calls to it, to all and to us
	uint16_t unit_id;	// ours, the source of what we send: 1 to 65519
	bool unit_call;		// PTT calls the unit tx_dst (individual), else the group tx_dst
	uint16_t tx_dst;
	bool emergency;		// what we send has the emergency bit
} radio_nxdn_settings_t;

#define RADIO_NXDN_RAN_MAX	63
#define RADIO_NXDN_UNIT_MAX	65519u	// the IDs above are special (TS 1-B)

// view flags
#define RADIO_NXDN_ON		0x01u	// the engine runs on this channel
#define RADIO_NXDN_RX		0x02u	// a call is being received
#define RADIO_NXDN_PLAYED	0x04u	// ... and passes the squelch (its voice plays)
#define RADIO_NXDN_ENC		0x08u	// ... and is encrypted (not played)
#define RADIO_NXDN_UNIT		0x10u	// ... is an individual call (dst is a unit, else a group)
#define RADIO_NXDN_LATE		0x20u	// ... was entered without its header
#define RADIO_NXDN_TALK_UNIT	0x40u	// PTT calls a unit, else a group (the settings' unit_call)
#define RADIO_NXDN_9600		0x80u	// the rate in force is 9600 bit/s, else 4800

// The radio view's part (radio_view_t.nxdn): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_NXDN_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint8_t ran;		// the RAN of the call heard
	uint8_t rx_ran;		// the settings' (0: any)
	uint8_t tx_ran;
	bool emergency;		// the call heard has the emergency bit
	uint16_t fit;		// what the board's provider gives this rate (AIR_FIT_*), 0: nothing
	uint16_t src;		// the caller's unit ID, 0 not known yet
	uint16_t dst;		// the group or unit called
	uint16_t talk;		// what PTT calls (the settings' tx_dst): a group, or a unit with RADIO_NXDN_TALK_UNIT
#if CONFIG_NXDN_OTA_ALIAS
	char alias[NXDN_ALIAS_LEN + 1];
#endif
} radio_nxdn_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while NXDN runs)
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t frames, lich_fail, sacch_fail, facch_fail, voice, voice_held, fec_bits, ran_rejects;	// the call layer's
	uint32_t rx_chunks, tx_underruns, tx_syms;	// the session's
	uint32_t begins, begin_fails;	// engine starts, and the ones the provider refused (no provider, no fit, no memory)
} radio_nxdn_stats_t;

#if CONFIG_NXDN
#include "radio/radio_air.h"

extern const radio_air_ops_t radio_nxdn_air;

void radio_nxdn_init(void);
// The board's provider gives NXDN something (receive or transmit) at either rate (any task): what the UI and the
// mode request offer NXDN on
bool radio_nxdn_fits(void);
// What the provider gives the rate (AIR_FIT_*; any task)
uint32_t radio_nxdn_fit(bool rate9600);
// A channel read from the codeplug, as the radio runs it: a live NXDN channel (kind 4) becomes RADIO_MODE_NXDN where
// the board carries NXDN and its frequency allows a digital mode. False: left as read (parked)
bool radio_nxdn_take(channel_t *ch);
// The board's provider receives channel ch at number (0: the VFO) at its rate (a stored channel's own, else the
// settings'): what scan and dual watch visit
bool radio_nxdn_receives(const channel_t *ch, uint16_t number);
void radio_nxdn_settings(radio_nxdn_settings_t *out);		// any task
void radio_nxdn_set_settings(const radio_nxdn_settings_t *s);	// radio task (RREQ_NXDN_SET)

// The parameters in force, as settings: false when the engine isn't running. Any task: a read that crosses a change
// of channel may mix the two (diagnostics only)
bool radio_nxdn_in_force(radio_nxdn_settings_t *out);

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped; the audio stream starts. number: the
// stored channel ch is (0: the VFO, or none): a live NXDN channel's tag gives the parameters, else the settings. False:
// no memory, no provider or no fit at the rate (NXDN then receives nothing and refuses PTT)
bool radio_nxdn_begin(const channel_t *ch, uint16_t number);
void radio_nxdn_end(void);
bool radio_nxdn_running(void);
void radio_nxdn_retuned(void);
// The settings changed: the engine takes them (not while transmitting). A new rate starts the engine over
void radio_nxdn_apply(const channel_t *ch, uint16_t number);
// Every tick: the view and the counters. True when the view changed
bool radio_nxdn_poll(radio_nxdn_view_t *v);
#if CONFIG_NXDN_OTA_ALIAS
// Stage the name for the next PTT. An empty string disables transmit alias.
void radio_nxdn_set_tx_alias(const char *alias);
#endif

void radio_nxdn_stats(radio_nxdn_stats_t *out);
void radio_nxdn_reset_stats(void);
#endif

#endif
