// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF channels in the codeplug (docs/spec/ysf-codeplug.md): the codec from the stored bytes to a plain
// struct the radio core sets the YSF engine up from. Bytes in, values out: no engine here. Built with CONFIG_YSF
#ifndef RFOUNDRY_YSF_CODEPLUG_H
#define RFOUNDRY_YSF_CODEPLUG_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug/codeplug.h"

#define YSF_CP_DGID_MAX		99u
#define YSF_CP_CALL_LEN		10u	// the radio-wide record's callsign field

// A channel's parameter block (2)
#define YSF_CP_CHAN_LEN		2u
typedef struct {
	uint8_t tx_dgid;	// 0..99
	uint8_t rx_dgid;	// 0..99; 0 hears every DG-ID
} ysf_chan_t;

// Decode len bytes of a block (a short one, or none, takes the defaults: 0 and 0); a value out of range reads as 0
void ysf_chan_decode(const uint8_t *b, int len, ysf_chan_t *out);
void ysf_chan_encode(const ysf_chan_t *c, uint8_t out[YSF_CP_CHAN_LEN]);

// The radio-wide record (1): the callsign as stored, "" where unset or not valid. The radio ID is never stored:
// a codeplug is copied from radio to radio, and every radio needs an ID of its own (ysf_id.h, docs/spec/ysf.md 6)
typedef struct {
	char callsign[YSF_CP_CALL_LEN + 1];
} ysf_radio_t;
void ysf_radio_decode(const uint8_t *b, int len, ysf_radio_t *out);
// The record for r (YSF_CP_CALL_LEN bytes)
void ysf_radio_encode(const ysf_radio_t *r, uint8_t out[YSF_CP_CALL_LEN]);
// The stored record (an empty one when there is none)
void ysf_radio_load(ysf_radio_t *out);
// The callsign a radio name gives when the record has none (ysf.md 5.2): its first word up to a space, '-' or '/',
// upper case, when it reads as a callsign: 3 to 10 letters and digits with at least one of each. "" otherwise
void ysf_callsign_from_name(const char *name, char out[YSF_CP_CALL_LEN + 1]);

// Channel number's block from its tag (ch read by codeplug_channel); false when it isn't a live YSF channel
bool ysf_chan_load(uint16_t number, const channel_t *ch, ysf_chan_t *out);
// Write ch at number as a YSF channel (channel-modes.md 2.4): the base record as the safe stand-in, then the tag
// with c's block, transmit allowed unless ch->rx_only; a channel that is YSF already keeps the rest of its tag
bool ysf_chan_save(uint16_t number, const channel_t *ch, const ysf_chan_t *c);

#endif
