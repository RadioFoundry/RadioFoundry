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
	DL_VIEW_STORAGE,	// settings store, flash journal, codeplug and CPS session counters
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

// The debug link's own counters: frames answered, bad CRC, too long, timed out (debuglink.c)
#define DL_LINK_COUNTERS	4
void debuglink_link_counters(uint32_t out[DL_LINK_COUNTERS]);

#endif
