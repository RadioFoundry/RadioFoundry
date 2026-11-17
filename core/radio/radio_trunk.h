// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A DMR Tier III trunked system on the radio (docs/spec/dmr-trunking.md 11, dmr-trunking-codeplug.md). The system is staged in RAM
// (radio_t3_staged), by the debug link or from the codeplug (radio_t3_load); RADIO_T3_START hands it to the radio core,
// which runs the engine (dmr/trunk/t3_ms.h) on the channel the radio is on.
#ifndef RFOUNDRY_RADIO_TRUNK_H
#define RFOUNDRY_RADIO_TRUNK_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

typedef enum {
	RADIO_T3_START = 1,		// select the staged system: hunt for its TSCC
	RADIO_T3_STOP,			// leave it: back to the channel or VFO
	RADIO_T3_DEREG,			// de-register (when registered), staying on the system
	RADIO_T3_END,			// end the call being followed (P_MAINT when it is ours or private)
	RADIO_T3_STATUS,		// send the staged status (radio_t3_setup_t.sd_*)
	RADIO_T3_SDATA,			// send the staged short data (radio_t3_setup_t.sd_*)
	RADIO_T3_DIVERT,		// set or clear a call diversion (radio_t3_setup_t.div_*, sd_dst, sd_group)
	RADIO_T3_ALS,			// listen to sd_dst's surroundings (ALS); sd_group: an emergency one
	RADIO_T3_INTERRUPT,		// ask the TS to stop the talker in the call followed (status 125)
	RADIO_T3_ALS_END,		// end the ALS call the radio listens to (status 123)
	RADIO_T3_SDPOLL,		// poll sd_dst for its short data: format sd_format, sd_len blocks
} radio_t3_cmd_t;

#if CONFIG_DMR_TRUNK
#include "dmr/trunk/t3_ms.h"
#include "codeplug/codeplug.h"

#define RADIO_T3_CHANS	8	// programmed channels (the plan's table; control channels are among them)
#define RADIO_T3_SYSTEMS	8	// systems the radio runs: the codeplug's system records 0-7 (a stun names one in 3 bits)
#define RADIO_T3_STAGED	0xFFu	// radio_t3_setup_t.sys of a system the debug link set up

// Fallback (docs/spec/dmr-trunking.md 12)
#define RADIO_T3_FALLBACK_HOLD_S	30	// hunting this long with no TSCC: to the fallback channel
#define RADIO_T3_TUNE_HOLD_MS		500	// a retune waits at most this long for a burst list to go out
#define RADIO_T3_PROBE_MS		10000	// on the fallback channel: how often the radio looks for the TSCC
#define RADIO_T3_PROBE_HUNT_MS		2000	// for how long
#define RADIO_T3_PROBE_QUIET_MS		2000	// only when the fallback channel has been quiet this long

typedef struct {
	t3_cfg_t cfg;		// cfg.auth.sites and cfg.plan.table point into this struct once taken (radio_t3_fix)
	t3_chan_t chans[RADIO_T3_CHANS];
	uint8_t nchans;
	uint8_t cc;		// the system's color code, for channels programmed with T3_CC_ANY
	uint8_t sys;		// codeplug system n, or RADIO_T3_STAGED
	uint8_t fb_hold_s;	// fallback hold time, 0 = RADIO_T3_FALLBACK_HOLD_S
	uint16_t fb_chan;	// the system's fallback channel, 0 = none, and its name check
	uint16_t fb_check;
	uint32_t talk_id;	// PTT on the TSCC asks for a call to this (0: the first talkgroup with T3_TG_TALK)
	bool talk_private;
	uint32_t sd_dst;	// RADIO_T3_STATUS, RADIO_T3_SDATA: to this MS or talkgroup
	bool sd_group;
	uint8_t sd_status;	// 0-126, or T3_STATUS_POLL
	uint8_t sd_format;	// RADIO_T3_SDATA: T3_UDTF_*, sd_len octets of sd_data
	uint8_t sd_len;
	uint8_t sd_data[T3_UDT_MAX];
	uint8_t div_mode;	// RADIO_T3_DIVERT: RADIO_T3_DIV_*
	uint8_t div_svcs;	// T3_DIV_*
	uint8_t poll_reply[T3_UDT_MAX];	// the answer to a short data poll: cfg.poll_len octets, cfg.poll_format
#if CONFIG_DMR_TRUNK_GW
	uint8_t dial[T3_DIAL_MAX / 2];	// the digits a call through a dialing gateway sends: cfg.dial_n of them, BCD
#endif
#if CONFIG_DMR_TRUNK_SUPL
	uint8_t supl[T3_UDT_MAX];	// supplementary user data offered with each call: cfg.supl_len octets, cfg.supl_format
#endif
	uint8_t poll_blocks;	// polling an MS from the menus: the prearranged size, 1-4 blocks (0: 4)
	bool ip_gw;		// off-net IP goes through the system's IP gateway (IPI, dmr-trunking.md 7.7)
	bool allcall;		// the system's record follows all-calls (a channel's RX list may add it, radio_t3_talk)
} radio_t3_setup_t;

enum { RADIO_T3_DIV_CLEAR, RADIO_T3_DIV_SET, RADIO_T3_DIV_CLEAR_IN };

// A trunked channel's own tag flag (dmr-trunking-codeplug.md 4; the common ones are CODEPLUG_TAG_*)
#define RADIO_T3_TAG_NO_FALLBACK	0x10u

// The setup the debug link writes to (host link task); RADIO_T3_START copies it into the radio core
radio_t3_setup_t *radio_t3_staged(void);
void radio_t3_reset(radio_t3_setup_t *s);
// Point cfg's internal pointers into s (after a copy)
void radio_t3_fix(radio_t3_setup_t *s);
#if CONFIG_DMR_TRUNK_GW
// RREQ_T3_DIAL's two arguments for digits ("0-9*#", the first 16; another character ends them): 16 BCD nibbles,
// the first in w[0]'s top one, 0xF after the last (B.3.3)
void radio_t3_dial_args(const char *digits, uint32_t w[2]);
#endif
// System n of the codeplug (dmr-trunking-codeplug.md 1-3, 5) into s, with no talkgroups: false (s reset) when it is
// absent, not a Tier III one, too short or not usable (no control channel, or it needs a key and has none). A system
// without the transmit side is listen only (T3_OPT_LISTEN_ONLY)
bool radio_t3_load(uint8_t n, radio_t3_setup_t *s);
// The channel's talk target and the talkgroups of its RX group list into s (replacing any)
void radio_t3_talk(const channel_t *ch, radio_t3_setup_t *s);
// Whether system n is usable
bool radio_t3_usable(uint8_t n);
// System n's name (CODEPLUG_NAME_LEN + 1 bytes); false when it is absent or not a Tier III one
bool radio_t3_name(uint8_t n, char *name);
// The fallback for trunked channel `number` on system s into out: its channel number, 0 when none (none set, the
// channel gone, renamed or not a conventional one). The channel's own (its tag's) goes before the system's
uint16_t radio_t3_fallback(uint16_t number, const radio_t3_setup_t *s, channel_t *out);
#endif

#endif
