// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV channels in the codeplug (docs/spec/dstar-codeplug.md): the codec from the stored bytes to the on-air
// fields the radio header is built from (dstar_frame.h), and back. Text stays as it goes on air: ASCII, space padded,
// not terminated, so the header is filled by copying. Every decode cleans what it reads (NUL, 0xFF and characters a
// field doesn't take become spaces, callsign fields upper-cased), so no stored byte reaches the air uncleaned. No engine
// here. Built with CONFIG_DSTAR
#ifndef RFOUNDRY_DSTAR_CODEPLUG_H
#define RFOUNDRY_DSTAR_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "codeplug/codeplug.h"
#include "dstar/dstar_frame.h"

// A channel's parameter block (2.1): UR, RPT1, RPT2, flags
#define DSTAR_CP_CHAN_LEN	(3u * DSTAR_CALL_LEN + 1u)
#define DSTAR_CP_VIA_RPT	0x01u		// flags: via a repeater (header flag 1 b6); clear: direct
#define DSTAR_CP_UR_CQ		"CQCQCQ  "	// a blank UR
typedef struct {
	char ur[DSTAR_CALL_LEN], rpt1[DSTAR_CALL_LEN], rpt2[DSTAR_CALL_LEN];	// as on air, space padded
	uint8_t flags;		// DSTAR_CP_VIA_RPT; the reserved bits read as 0
} dstar_chan_t;

// The kind's radio-wide record (3): MY, its suffix, the slow data message
#define DSTAR_CP_STATION_LEN	(DSTAR_CALL_LEN + DSTAR_SUFFIX_LEN + DSTAR_MSG_LEN)
typedef struct {
	char my[DSTAR_CALL_LEN];	// all spaces: the radio core sends the DMR callsign
	char suffix[DSTAR_SUFFIX_LEN];
	char msg[DSTAR_MSG_LEN];	// all spaces: no message
} dstar_station_t;

// Decode len bytes of a block (a short one, or none, reads the rest as zero: UR CQ, direct). Never fails
void dstar_chan_decode(const uint8_t *b, int len, dstar_chan_t *out);
// The block for c, its fields cleaned as a decode would; the reserved flag bits written 0
void dstar_chan_encode(const dstar_chan_t *c, uint8_t out[DSTAR_CP_CHAN_LEN]);
// The radio-wide record from len bytes (-1 or a short one: the rest blank). Never fails
void dstar_station_decode(const uint8_t *b, int len, dstar_station_t *out);
void dstar_station_encode(const dstar_station_t *s, uint8_t out[DSTAR_CP_STATION_LEN]);

// Channel number's fields from its live tag (codeplug_tag_params), ch read by codeplug_channel; false when it isn't a
// live D-STAR channel. Transmit allowed is ch->rx_only, which the tag already set
bool dstar_chan_load(uint16_t number, const channel_t *ch, dstar_chan_t *out);
// The radio-wide record; false when it is absent (out holds blanks)
bool dstar_station_load(dstar_station_t *out);

// A C string (NULL: none) into an on-air callsign field of len characters: upper-cased, characters a callsign field
// doesn't take made spaces, cut or padded with spaces
void dstar_field_set(char *dst, size_t len, const char *src);
// The same for the message field: printable ASCII kept as typed, anything else a space
void dstar_msg_set(char dst[DSTAR_MSG_LEN], const char *src);

// The radio's own channel editor: write channel number's tag as a D-STAR channel named name, transmit allowed when tx,
// with c's fields. The base record rule (docs/spec/channel-modes.md 2.3) is the caller's: write the record first, as
// the safe stand-in (DMR, receive only, the channel's frequencies) under the same name, then call this; a cut between
// the two leaves a harmless receive-only channel. A tag already live there for that name keeps its reserved flag bits
// and any bytes past the block this firmware knows. False when the settings area can't be written
bool dstar_chan_write(uint16_t number, const char *name, const dstar_chan_t *c, bool tx);

#endif
