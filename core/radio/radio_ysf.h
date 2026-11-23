// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System Fusion DN on a channel, the radio core's side (docs/spec/ysf.md 5): the engine (core/ysf/ysf_engine.h) on
// the memory it runs in, its voice frames to and from the vocoder the board hosts for DMR (core/dmr/ambe), the
// channel's DG-IDs, who we are on air (callsign and radio ID), and a compact view for the radio view. radio_sm.c
// decides when YSF runs and keys the transmitter (radio_air.h); nothing here keys it.
//
// A stored YSF channel has its own DG-IDs (docs/spec/ysf-codeplug.md); the VFO and a channel whose mode was changed to
// YSF for now take the RAM settings (radio_req_ysf_settings).
//
// The radio ID (RID) is this radio's and no other's: made from the MCU's unique ID (core/ysf/ysf_id.h), never from
// the codeplug, never set by hand. radio_ysf_rid gives it to whatever shows it.
#ifndef RADIO_YSF_H
#define RADIO_YSF_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"

#define RADIO_YSF_CALL_LEN	10
#define RADIO_YSF_RID_LEN	5
#define RADIO_YSF_DGID_MAX	99

// The YSF settings (RAM only, reset at start-up): the VFO's, and the shape of what is in force
typedef struct {
	uint8_t tx_dgid;	// 0..99
	uint8_t rx_dgid;	// 0..99; 0 hears every DG-ID
	// A stored channel's DG-ID for now (quick settings): both ways on channel temp_chan while it is in use; another
	// channel taken drops it. 0: none
	uint16_t temp_chan;
	uint8_t temp_dgid;
} radio_ysf_settings_t;

// view flags
#define RADIO_YSF_ON		0x01u	// the engine runs on this channel
#define RADIO_YSF_RX		0x02u	// a call is being received
#define RADIO_YSF_PLAYED	0x04u	// ... and passes the DG-ID (its voice plays)
#define RADIO_YSF_LATE		0x08u	// ... was entered without its header
#define RADIO_YSF_NOT_DN	0x10u	// ... is not DN voice (VW or data: shown, not played)
#define RADIO_YSF_NO_CALL	0x20u	// we have no callsign: PTT refused
#define RADIO_YSF_RID_WEAK	0x40u	// our RID comes from the fallback, not the MCU's unique ID

// The radio view's part (radio_view_t.ysf): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_YSF_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint8_t dgid;		// of the call heard
	uint8_t rx_dgid, tx_dgid;	// in force
	uint16_t fit;		// what the board's provider gives YSF (AIR_FIT_*), 0: nothing
	char src[RADIO_YSF_CALL_LEN + 1];	// the caller, "" not known yet
	char src_rid[RADIO_YSF_RID_LEN + 1];	// the caller's radio ID, "" not known yet
} radio_ysf_view_t;

// Counters for the debug link
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t frames, fich_fail, csd_fail, dch_fail, voice, voice_held, fec_bits, vch_fixed;	// the call layer's
	uint32_t rx_chunks, tx_underruns, tx_syms;	// the session's
	uint32_t begins, begin_fails;
} radio_ysf_stats_t;

#if CONFIG_YSF
#include "radio/radio_air.h"
#include "ysf/ysf_wx.h"

// Wires-X (docs/spec/ysf.md 9): what the node said last, and what was asked of it
typedef struct {
	uint32_t serial;		// moves at every reply (ysf_wx_t.serial)
	uint32_t asked;			// moves at every command sent
	uint8_t asked_cmd;		// YSF_WX_* of the last one, YSF_WX_NONE before any
	uint8_t kind;			// YSF_WX_* of the last reply
	bool connected;
	char node_name[15], node_call[11], room_id[6], room_name[17];
	bool search;			// the list is a search's result
	uint8_t rooms;			// on the page held
	uint16_t rooms_total, rooms_first;	// rooms the node knows; the page's first entry (1-based)
} radio_ysf_wx_t;

// Radio task: arm a Wires-X command for the next transmission (RREQ_YSF_WX keys for it); false: not one
bool radio_ysf_wx_arm(uint8_t cmd, uint32_t arg, const char *text);
void radio_ysf_wx_disarm(void);
// Any task: the state, and room i of the page held (false past it)
void radio_ysf_wx(radio_ysf_wx_t *out);
bool radio_ysf_wx_room(uint8_t i, ysf_wx_room_t *out);

extern const radio_air_ops_t radio_ysf_air;

void radio_ysf_init(void);
// The board's provider gives YSF something (receive or transmit; any task)
bool radio_ysf_fits(void);
uint32_t radio_ysf_fit(void);
// A channel read from the codeplug, as the radio runs it: a live YSF channel (kind 0x67) becomes RADIO_MODE_YSF where
// the board carries YSF and its frequency allows a digital mode. False: left as read (parked)
bool radio_ysf_take(channel_t *ch);
void radio_ysf_settings(radio_ysf_settings_t *out);		// any task
void radio_ysf_set_settings(const radio_ysf_settings_t *s);	// radio task (RREQ_YSF_SET)
// This radio's RID (any task). weak: it comes from the fallback (no usable MCU unique ID); may be NULL
void radio_ysf_rid(char out[RADIO_YSF_RID_LEN + 1], bool *weak);
// The callsign we send: the codeplug's YSF record, else the radio name's callsign; "" = none (PTT refused)
void radio_ysf_callsign(const char *radio_name, char out[RADIO_YSF_CALL_LEN + 1]);

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped. number: the stored channel ch is
// (0: the VFO). radio_name: the radio name (the callsign's fallback). False: no memory, no provider or no fit
bool radio_ysf_begin(const channel_t *ch, uint16_t number, const char *radio_name);
void radio_ysf_end(void);
bool radio_ysf_running(void);
void radio_ysf_retuned(void);
void radio_ysf_apply(const channel_t *ch, uint16_t number, const char *radio_name);
// Every tick: the view and the counters. True when the view changed
bool radio_ysf_poll(radio_ysf_view_t *v);

void radio_ysf_stats(radio_ysf_stats_t *out);
void radio_ysf_reset_stats(void);
#endif

#endif
