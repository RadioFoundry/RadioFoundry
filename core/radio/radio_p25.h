// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional on a channel, the radio core's side (docs/spec/p25.md 7): the engine (core/p25/p25_engine.h) and
// the voice path (core/p25/p25Voice.h) on the memory they run in, the channel's P25 parameters, and a compact view
// for the radio view. radio_sm.c decides when P25 runs and keys the transmitter; nothing here keys it.
//
// The parameters come from radio_p25_params alone: a stored P25 channel's own (core/p25/p25_codeplug.h), else the RAM
// settings the debug link changes (radio_req_p25_settings), which the VFO and a mode changed for now run on.
#ifndef RADIO_P25_H
#define RADIO_P25_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#include "hal_audio.h"

// The P25 settings (RAM only, reset at start-up). On a stored P25 channel only unit_id counts (a session override)
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
#define RADIO_P25_TALK_UNIT	0x40u	// PTT calls unit talk, else talk group talk (the settings' unit_call)
#define RADIO_P25_TRUNK_TX	0x80u	// a trunked channel whose transmit side is on (registration, PTT)

// On a trunked channel (CONFIG_P25_TRUNK, docs/spec/p25-trunking.md 7): what the follower does
#define RADIO_P25_TRUNK_OFF	0	// not a trunked channel, or not running
#define RADIO_P25_TRUNK_HUNT	1	// looking for the system's control channel
#define RADIO_P25_TRUNK_CONTROL	2	// on the control channel, waiting for a call it follows
#define RADIO_P25_TRUNK_VOICE	3	// on a voice channel, following a call (or talking on the one granted to us)

// Why PTT on a trunked channel, P25 or DMR Tier III, did not get a voice channel (TXDENY_TRUNK; the reason is in
// radio_p25_view_t.twhy or radio_t3_brief_t.why)
enum {
	RADIO_TRUNK_WHY_NONE,
	RADIO_TRUNK_WHY_OFF,		// the transmit side is off (listen only, receive only, no unit ID or talk group)
	RADIO_TRUNK_WHY_NO_CC,		// no control channel held
	RADIO_TRUNK_WHY_NOT_REG,	// not registered (yet, or denied); P25: and affiliated
	RADIO_TRUNK_WHY_DENIED,		// the system denied the request
	RADIO_TRUNK_WHY_NO_ANSWER,	// no grant came
	RADIO_TRUNK_WHY_NO_CHAN,	// granted on a channel this radio can't transmit on
	RADIO_TRUNK_WHY_OTHER_SYS,	// no control channel held; another system's heard (Tier III: model, network or CC)
	RADIO_TRUNK_WHY_REG_REFUSED,	// the system refused the radio's registration (Tier III Reg_Refused: ID not known)
	RADIO_TRUNK_WHY_UNMAPPED,	// granted on a channel the codeplug doesn't list (Tier III: radio_t3_brief_t.unmapped)
};

// The radio view's part (radio_view_t.p25): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_P25_*
	uint8_t tx;		// air_tx_state_t of the engine
	uint16_t nac;
	uint16_t rx_nac;	// the NAC received (the settings' rx_nac; 0xF7E: any)
	uint8_t trunk;		// RADIO_P25_TRUNK_*
	uint8_t treg, treq;	// a trunked channel's registration and voice request (P25_REG_*, P25_REQ_* of p25_follow.h)
	uint8_t twhy;		// RADIO_TRUNK_WHY_* of the last PTT refused or request failed
	uint32_t src;		// the caller's unit ID, 0 not known yet
	uint32_t dst;		// the talk group or the unit called
	uint32_t talk;		// what PTT calls (the settings' tx_dst): a talk group, or a unit with RADIO_P25_TALK_UNIT
} radio_p25_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while P25 runs)
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t units, voice, voice_bad, voice_bits, lc_fail, es_fail;	// the call layer's (p25_call_stats_t)
	uint32_t rx_chunks, tx_underruns, tx_syms;			// the session's
	uint32_t begins, begin_fails;	// engine starts, and the ones the provider refused (no provider, no fit)
	uint32_t mem_lent;		// starts that ran in memory lent by the vocoder
	// A trunked channel's: retunes the follower asked, the ones to a frequency the radio can't receive, units and
	// calls the follower missed because the radio task was inside it
	uint32_t trunk_retunes, trunk_untuned, trunk_skips;
	// Inbound bursts the follower asked: sent, and refused by the radio's transmit checks
	uint32_t trunk_bursts, trunk_burst_refused;
} radio_p25_stats_t;

#if CONFIG_P25
#include "p25/p25_engine.h"
#include "radio/radio_air.h"
#if CONFIG_P25_TRUNK
#include "p25/p25_follow.h"

// The follower's view as the radio task last copied it (any task reads it)
typedef struct {
	uint8_t seq;		// odd while it is written
	bool on;		// a trunked channel runs
	bool tx;		// ... with the transmit side on
	uint32_t retunes, untuned, skips, bursts, burst_refused;	// radio_p25_stats_t's trunk_*
	p25_follow_view_t v;
} radio_p25_trunk_view_t;
void radio_p25_trunk_view(radio_p25_trunk_view_t *out);

// Transmitting on a trunked channel (radio task; docs/spec/p25-trunking.md 7). The follower's transmit side is on
bool radio_p25_trunk_tx(void);
// PTT pressed or released: pressed, RADIO_TRUNK_WHY_NONE when a voice request goes out, else why not
uint8_t radio_p25_trunk_ptt(bool down);
// After PTT: RADIO_P25T_WAIT while the request waits, GO when the grant is in and the receiver on the voice channel
// (*hz: where to transmit), FAIL when it won't come (the view's twhy says why)
enum { RADIO_P25T_WAIT, RADIO_P25T_GO, RADIO_P25T_FAIL };
uint8_t radio_p25_trunk_talk(uint32_t *hz);
// Our transmission on the granted channel is over (or never started): back to the control channel
void radio_p25_trunk_talk_end(void);
// An inbound burst the follower asked: its frequency, 0 none. start: the engine primes it (then key, go, as for a
// call); drop: the radio's transmit checks refused it
uint32_t radio_p25_trunk_burst_hz(void);
bool radio_p25_trunk_burst_start(void);
void radio_p25_trunk_burst_drop(void);
#endif

void radio_p25_init(const hal_audio_t *audio);	// the PCM stream the voice plays on
// The board's air interface can receive P25 (any task): what the UI offers P25 on
bool radio_p25_fits(void);
// A channel read from the codeplug, as the radio runs it: a live P25 conventional channel (kind 2), and with
// CONFIG_P25_TRUNK a trunked one (kind 3), becomes RADIO_MODE_P25 where the board runs P25 and its frequency allows a
// digital mode. False: left as read (parked)
bool radio_p25_take(channel_t *ch);
void radio_p25_settings(radio_p25_settings_t *out);	// any task
void radio_p25_set_settings(const radio_p25_settings_t *s);	// radio task (RREQ_P25_SET)
// The engine's configuration for channel ch, stored at number (0: the VFO, or none): the one place P25 parameters come
// from. A live P25 channel's tag and record give them, else the settings. own_id: the radio's DMR ID, the last resort
// for our unit ID (after the settings' and the channel's, then the radio-wide P25 one)
void radio_p25_params(const channel_t *ch, uint16_t number, uint32_t own_id, p25_engine_cfg_t *cfg);
// The parameters in force, as settings (talkaround on or off): false when the engine isn't running. Any task: a read
// that crosses a change of channel may mix the two (diagnostics only)
bool radio_p25_in_force(radio_p25_settings_t *out);

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped. False: no memory, no provider or
// no fit (P25 then receives nothing and refuses PTT)
bool radio_p25_begin(const channel_t *ch, uint16_t number, uint32_t own_id);
void radio_p25_end(void);
bool radio_p25_running(void);
// The engine runs a trunked channel (P25T, CONFIG_P25_TRUNK): the follower tunes
bool radio_p25_trunked(void);
void radio_p25_retuned(void);
// The settings or the channel's parameters changed: the engine takes them (not while transmitting)
void radio_p25_apply(const channel_t *ch, uint16_t number, uint32_t own_id);
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

// The transmit side above as radio_sm.c drives every air-framed mode
extern const radio_air_ops_t radio_p25_air;
#ifndef HAL_VOCODER_LEND_BYTES
// The memory P25 runs in while it doesn't (another air mode runs in it, one at a time): NULL while P25 runs
void *radio_p25_idle_mem(size_t *len);
#endif
#endif

#endif
