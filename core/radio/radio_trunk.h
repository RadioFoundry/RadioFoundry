// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A DMR Tier III trunked system on the radio (docs/spec/dmr-trunking.md 11). The system is set up in RAM (the debug
// link stages it: radio_t3_staged) until the codeplug has keys for it; RADIO_T3_START hands it to the radio core, which runs
// the engine (dmr/trunk/t3_ms.h) on the channel the radio is on.
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
} radio_t3_cmd_t;

#if CONFIG_DMR_TRUNK
#include "dmr/trunk/t3_ms.h"

#define RADIO_T3_CHANS	8	// programmed channels (the plan's table; control channels are among them)

typedef struct {
	t3_cfg_t cfg;		// cfg.auth.sites and cfg.plan.table point into this struct once taken (radio_t3_fix)
	t3_chan_t chans[RADIO_T3_CHANS];
	uint8_t nchans;
	uint8_t cc;		// the system's color code, for channels programmed with T3_CC_ANY
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
} radio_t3_setup_t;

enum { RADIO_T3_DIV_CLEAR, RADIO_T3_DIV_SET, RADIO_T3_DIV_CLEAR_IN };

// The setup the debug link writes to (host link task); RADIO_T3_START copies it into the radio core
radio_t3_setup_t *radio_t3_staged(void);
void radio_t3_reset(radio_t3_setup_t *s);
// Point cfg's internal pointers into s (after a copy)
void radio_t3_fix(radio_t3_setup_t *s);
#endif

#endif
