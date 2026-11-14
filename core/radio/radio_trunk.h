// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A DMR Tier III trunked system on the radio (docs/spec/dmr-trunking.md 11, 12). The system is staged in RAM
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
#define RADIO_T3_SYSTEMS	8	// systems read from the codeplug (keys CODEPLUG_KEY_T3_SYSTEM + 0..7)
#define RADIO_T3_STAGED	0xFFu	// radio_t3_setup_t.sys of a system the debug link set up

// Fallback (docs/spec/dmr-trunking.md 12.5)
#define RADIO_T3_FALLBACK_HOLD_S	30	// hunting this long with no TSCC: to the fallback channel
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
} radio_t3_setup_t;

enum { RADIO_T3_DIV_CLEAR, RADIO_T3_DIV_SET, RADIO_T3_DIV_CLEAR_IN };

// A trunked channel's tag (docs/spec/dmr-trunking.md 12.4)
typedef struct {
	uint8_t sys;		// system n
	uint8_t flags;		// RADIO_T3_TAG_*
	uint16_t fb_chan;	// this channel's fallback, 0 = the system's, and its name check
	uint16_t fb_check;
} radio_t3_tag_t;
#define RADIO_T3_TAG_NO_FALLBACK	0x01u

// The setup the debug link writes to (host link task); RADIO_T3_START copies it into the radio core
radio_t3_setup_t *radio_t3_staged(void);
void radio_t3_reset(radio_t3_setup_t *s);
// Point cfg's internal pointers into s (after a copy)
void radio_t3_fix(radio_t3_setup_t *s);
// The name check of a channel name (12.4): FNV-1a over it NUL padded to 16 octets, the halves XORed
uint16_t radio_t3_name_check(const char *name);
// Channel c's tag; false when it has none, or its name check doesn't match name (the channel's)
bool radio_t3_tag(uint16_t c, const char *name, radio_t3_tag_t *tag);
// System n of the codeplug (12.1-12.3) into s, with no talkgroups: false (s reset) when it is absent, too short or not
// usable (no control channel, or it authenticates and has no key)
bool radio_t3_load(uint8_t n, radio_t3_setup_t *s);
// The channel's talk target and the talkgroups of its RX group list into s (replacing any)
void radio_t3_talk(const channel_t *ch, radio_t3_setup_t *s);
// Whether system n is usable
bool radio_t3_usable(uint8_t n);
// System n's name (CODEPLUG_NAME_LEN + 1 bytes); false when it is absent
bool radio_t3_name(uint8_t n, char *name);
// The fallback for a trunked channel with this tag on system s (12.5) into out: its channel number, 0 when none (none
// set, the channel gone, renamed or itself trunked)
uint16_t radio_t3_fallback(const radio_t3_tag_t *tag, const radio_t3_setup_t *s, channel_t *out);

#if CONFIG_ZONE_SHARE
// Zone sharing (docs/spec/zone-share.md 4.2): systems in and out of a share record. Codeplug writes: the storage job
// that imports, never the radio task
#include "dmr/zoneShare.h"
// System n as its sharing setting allows (12.6), its channels into chan (ZS_SYS_CHANS_MAX): false when it is absent or
// not shared. *fb_chan: its fallback channel (0: none), for the caller to point at in the record. Never the radio's
// own ID
bool radio_t3_share_out(uint8_t n, zsSystem_t *s, zsSysChan_t *chan, uint16_t *fb_chan);
// The system here with this system code (model, NET, category), -1: none
int radio_t3_find(uint8_t model, uint16_t net, uint8_t category);
// System `index` of a record written to the lowest free n (its channels and key with it, sharing off): n, -1 no room
// or a flash error (anything written is removed). s: that system, as zsSystemAt read it
int radio_t3_share_in(const zsRecord_t *rec, int index, const zsSystem_t *s);
// System n's fallback channel set to channel c (the record's, once imported) and its name check
bool radio_t3_set_fallback(uint8_t n, uint16_t c, const char *name);
// System n, its channels and key removed (an import undone)
void radio_t3_remove(uint8_t n);
// Channel c tagged with system n (its name for the check); the tag removed
bool radio_t3_tag_set(uint16_t c, uint8_t n, const char *name);
void radio_t3_tag_remove(uint16_t c);
#endif
#endif

#endif
