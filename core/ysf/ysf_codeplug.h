// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion channels in the codeplug (docs/spec/ysf-codeplug.md): the codec from the stored bytes to what a
// transmission is built from (ysf_frame.h) and back. Text stays as it goes on air: ASCII, space padded, not terminated.
// Every decode cleans what it reads (NUL, 0xFF and characters a field doesn't take become spaces, lower case is made
// upper), so no stored byte reaches the air uncleaned. No engine here. Built with CONFIG_YSF
#ifndef RFOUNDRY_YSF_CODEPLUG_H
#define RFOUNDRY_YSF_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "codeplug/codeplug.h"
#include "ysf/ysf_frame.h"

// A channel's parameter block (2.1): the destination, the DG-IDs each way, flags
#define YSF_CP_CHAN_LEN		(YSF_CALLSIGN_LEN + 3u)
#define YSF_CP_NARROW		0x01u		// flags: narrow deviation (FICH Dev = 1); clear: wide
#define YSF_CP_VW		0x02u		// flags: PTT sends Voice FR (VW); clear: V/D mode 2 (DN)
#define YSF_CP_FLAGS		(YSF_CP_NARROW | YSF_CP_VW)	// the flags this firmware knows
#define YSF_CP_DEST_ALL		"ALL       "	// a blank destination: a group / CQ call
#define YSF_CP_DGID_MAX		99u
typedef struct {
	char dest[YSF_CALLSIGN_LEN];	// as on air, space padded
	uint8_t tx_dgid;	// 0-99, sent in the FICH's squelch code
	uint8_t rx_dgid;	// 0-99; 0 hears every DG-ID
	uint8_t flags;		// YSF_CP_NARROW, YSF_CP_VW; the reserved bits read as 0
} ysf_chan_t;

// The kind's radio-wide record (3): our callsign and radio ID
#define YSF_CP_STATION_LEN	(YSF_CALLSIGN_LEN + YSF_RADIO_ID_LEN)
typedef struct {
	char call[YSF_CALLSIGN_LEN];	// all spaces: the radio core sends the radio name
	char radio_id[YSF_RADIO_ID_LEN];	// all spaces: none (spaces on air)
} ysf_station_t;

// Decode len bytes of a block (a short one, or none, reads the rest as zero: a group call, DG-ID 0, wide). Never fails
void ysf_chan_decode(const uint8_t *b, int len, ysf_chan_t *out);
// The block for c, its fields cleaned as a decode would; the reserved flag bits written 0
void ysf_chan_encode(const ysf_chan_t *c, uint8_t out[YSF_CP_CHAN_LEN]);
// The radio-wide record from len bytes (-1 or a short one: the rest blank). Never fails
void ysf_station_decode(const uint8_t *b, int len, ysf_station_t *out);
void ysf_station_encode(const ysf_station_t *s, uint8_t out[YSF_CP_STATION_LEN]);

// Channel number's fields from its live tag (codeplug_tag_params), ch read by codeplug_channel; false when it isn't a
// live Fusion channel. Transmit allowed is ch->rx_only, which the tag already set
bool ysf_chan_load(uint16_t number, const channel_t *ch, ysf_chan_t *out);
// The radio-wide record; false when it is absent (out holds blanks)
bool ysf_station_load(ysf_station_t *out);

// A C string (NULL: none) into an on-air field of len characters: upper-cased, characters a callsign field doesn't take
// made spaces, cut or padded with spaces
void ysf_field_set(char *dst, size_t len, const char *src);

// The radio's own channel editor: write channel number's tag as a Fusion channel named name, transmit allowed when tx,
// with c's fields. The caller writes the base record first as the safe stand-in (DMR, receive only, the channel's
// frequencies; docs/spec/channel-modes.md 2.3). A live tag there keeps its reserved bits and any bytes past the block
// this firmware knows. False when the settings area can't be written
bool ysf_chan_write(uint16_t number, const char *name, const ysf_chan_t *c, bool tx);

#endif
