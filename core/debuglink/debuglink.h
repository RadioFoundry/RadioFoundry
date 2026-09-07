// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The USB debug link (docs/spec/debuglink.md): framed commands on the host port, beside the MMDVM hotspot and the CPS
// protocol (core/hostlink), for bench automation: version, diagnostics and counters, the radio view as key=value
// text, screenshots, key and PTT injection, radio requests, a log ring. CONFIG_DEBUGLINK.
#ifndef DEBUGLINK_H
#define DEBUGLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "debuglink/dl_fmt.h"
#include "radio/radio.h"

// Register with the host link as its side protocol (hostlink_start calls it)
void debuglink_start(void);

// --- The radio view as key=value text (debuglink_view.c), in sections so each fits one reply ---

enum {
	DL_VIEW_CORE,		// state, channel position, squelch, RSSI, power, TX timer
	DL_VIEW_CHANNEL,	// the channel / VFO settings in effect
	DL_VIEW_DMR,		// the DMR call view (debuglink_view_dmr.c)
	DL_VIEW_DMR_CALL,	// the current and the last DMR call (debuglink_view_dmr.c)
	DL_VIEW_DMR_STATS,	// the DMR voice path counters: call stack, AMBE voice (debuglink_view_dmr.c)
	DL_VIEW_HOTSPOT,	// the host link mode and the hotspot engine
	DL_VIEW_LINK,		// input and debug link state
	DL_VIEW_FEATURES,	// scan, FM extras, battery, beeps, the radio settings (debuglink_view_radio.c)
	DL_VIEW_DMR_DATA,	// the DMR data service: state, settings, counters (debuglink_dmr_data.c)
	DL_VIEW_STORAGE,	// settings store, flash journal, codeplug and CPS session counters
	DL_VIEW_DMR_EXT,	// DMR emergency, ETSI short data, the hotspot CW ID (debuglink_view_ext.c)
	DL_VIEW_DMR_DATA_STATS,	// the DMR data service counters (debuglink_dmr_data.c; split from dmr_data for the reply size)
	DL_VIEW_ROAM,		// DMR repeater roaming: state, sites, counters, settings (debuglink_view_roam.c)
	DL_VIEW_COUNT
};
#define DL_VIEW_VERSION		1	// bumped when a key changes meaning or goes away (new keys don't bump it)

// Render one section of v into t. Every section starts with "section=<name>"
void debuglink_view_section(uint8_t section, const radio_view_t *v, dl_text_t *t);

// The DMR sections (part 0: DL_VIEW_DMR, 1: DL_VIEW_DMR_CALL, 2: DL_VIEW_DMR_STATS), in their own file so the DMR
// integration can change them without touching the rest; built without DMR they say "dmr.built=0"
void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t);
// The DMR voice path counters as diagnostics lines (call stack, AMBE voice): line i < DL_DMR_DIAG_LINES
#define DL_DMR_DIAG_LINES	4
bool debuglink_dmr_diag_line(uint8_t i, char *buf, uint8_t len);
// Zero the call stack's and the AMBE voice counters
void debuglink_dmr_reset(void);

// The radio features (debuglink_view_radio.c): the "features" section, and the RADIO operations for scan, beeps, the
// tone burst and one setting (DL_OP_*; returns a DL_ST_* status)
void debuglink_view_radio(const radio_view_t *v, dl_text_t *t);
enum { DL_OP_SCAN = 20, DL_OP_SCAN_VFO = 21, DL_OP_BEEP = 22, DL_OP_TONE_BURST = 23, DL_OP_SETTING = 24, DL_OP_ROAM = 26 };
uint8_t debuglink_radio_feature_op(uint8_t op, const uint8_t *args, uint16_t len);
// The DMR data service (debuglink_dmr_data.c): the dmr_data view section, the DMR_DATA command (docs/spec/debuglink.md
// 3.12) and its counter reset. Built without CONFIG_DMR_DATA_CORE: "data.built=0", the command answers unavailable
void debuglink_view_dmr_data(uint8_t part, dl_text_t *t);	// part 0: state and settings, 1: counters
size_t debuglink_cmd_dmr_data(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_dmr_data_reset(void);
// The audio probe (debuglink_audio_probe.c, docs/spec/debuglink.md 3.14): built without CONFIG_AUDIO_PROBE it answers
// unavailable
size_t debuglink_cmd_audio_probe(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// The dmr_ext section (debuglink_view_ext.c): emergency state, alert and settings, the ETSI data counters, CW ID
void debuglink_view_dmr_ext(dl_text_t *t);
// The roam section (debuglink_view_roam.c); built without CONFIG_DMR_ROAMING it says "roam.built=0"
void debuglink_view_roam(const radio_view_t *v, dl_text_t *t);

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
// The split duplex command SPLIT and its view text (debuglink_split.c, docs/spec/debuglink.md 3.15)
size_t debuglink_cmd_split(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_view_split(dl_text_t *t);
#endif

// The debug link's own counters: frames answered, bad CRC, too long, timed out (debuglink.c)
#define DL_LINK_COUNTERS	4
void debuglink_link_counters(uint32_t out[DL_LINK_COUNTERS]);

#endif
