// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR channels in the codeplug (docs/spec/dpmr.md 6): the kind's parameter block and its radio-wide record, from the
// stored bytes to a plain struct the radio core sets the dPMR engine up from, and back. Bytes in, values out: no
// engine here. Built with CONFIG_DPMR.
//
// A channel names who it calls (the called ID: a common ID, or a full 24-bit address) and its channel code; the
// radio-wide record names who we are (the own ID). Both IDs are the standard's 24 bits, stored little endian as the
// layer's other fields are. A zeroed or an erased called ID is an all call (the reserved common ID 0, or common ID 255
// and its sixteen ones, TS 102 490 8.1.1): what a licence-free dPMR446 radio calls out of the box, and what an empty
// channel should do.
#ifndef RFOUNDRY_DPMR_CODEPLUG_H
#define RFOUNDRY_DPMR_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

// The channel block (6.1): called ID (3 bytes), channel code (1), flags (1)
#define DPMR_CP_CHAN_LEN	5u
#define DPMR_CP_CC_AUTO		0u	// the channel code the standard ties to the channel's frequency (dpmr_cc_for_hz)
#define DPMR_CP_CC_MAX		32u	// 1-16: table 6.1 group A's codes; 17-32: group B's
#define DPMR_CP_RX_MINE		0x01u	// flags: play only calls to the called ID, to all and to us; clear: every call
// Bits 1-6 are reserved and kept as found; bit 7 is always written 0, so a written flags byte is never the erased 0xFF
#define DPMR_CP_FLAGS_KEPT	0x7Fu
typedef struct {
	uint32_t called;	// 24 bits
	uint8_t cc;		// DPMR_CP_CC_AUTO or 1-32
	uint8_t flags;		// DPMR_CP_RX_MINE; the reserved bits are a later firmware's, kept as found
} dpmr_chan_t;

// The radio-wide record (6.2): our own ID (3 bytes). Absent, zero or erased: the channel's called ID is sent as our
// own too, as an initial-services radio on a common ID sends (8.1.1: a common ID addresses as both)
#define DPMR_CP_STATION_LEN	3u
#define DPMR_CP_ID_NONE		0xFFFFFFFFu

// Decode len bytes of a block (short or none: the rest read 0). Never fails. A called ID of 0 reads as the all call, a
// code past 32 as automatic, an erased flags byte as none
void dpmr_chan_decode(const uint8_t *b, int len, dpmr_chan_t *out);
void dpmr_chan_encode(const dpmr_chan_t *c, uint8_t out[DPMR_CP_CHAN_LEN]);
// The radio-wide record's own ID (len bytes, -1 = none): DPMR_CP_ID_NONE when absent, 0 or 0xFFFFFF
uint32_t dpmr_own_decode(const uint8_t *b, int len);

// The channel code (12 bits) a channel sends on tx_hz: its own, or the one table 6.1 ties to the frequency
uint16_t dpmr_chan_cc(const dpmr_chan_t *c, uint32_t tx_hz);

// Channel number's block from its live tag (ch read by codeplug_channel); false when it isn't a live dPMR channel
bool dpmr_chan_load(uint16_t number, const channel_t *ch, dpmr_chan_t *out);
// The radio-wide own ID: DPMR_CP_ID_NONE when there is none
uint32_t dpmr_own_load(void);

// Write ch at number as a dPMR channel (channel-modes.md 2.4): the base record in its safe stand-in shape (DMR,
// receive only), then the tag with c's block, transmit allowed unless ch->rx_only. A channel that is dPMR already
// keeps its tag's fallback, its other flags and what its block holds after these bytes (codeplug_tag_rewrite), and
// under a base-own tag its record's own mode; else the tag is new. False when a write fails
bool dpmr_chan_save(uint16_t number, const channel_t *ch, const dpmr_chan_t *c);

#endif
