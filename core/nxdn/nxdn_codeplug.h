// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional channels in the codeplug (docs/spec/nxdn-codeplug.md): the codec from the stored bytes to a plain
// struct the radio core sets the NXDN engine up from. Bytes in, values out: no engine here. Built with CONFIG_NXDN
#ifndef RFOUNDRY_NXDN_CODEPLUG_H
#define RFOUNDRY_NXDN_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

#define NXDN_CP_RAN_MAX		63u
#define NXDN_CP_TX_RAN_SET	0x80u	// a stored TX RAN holds a value (clear: the default)
#define NXDN_CP_TX_RAN_DEFAULT	1u	// the TX RAN where the RX RAN is "every RAN"
#define NXDN_CP_GROUP_DEFAULT	1u	// the group of a channel with no contact
#define NXDN_CP_ID_ALL		0xFFFFu	// the all-call contact's group (every group, every unit)
#define NXDN_CP_UNIT_MAX	65519u	// the IDs above are special (TS 1-B)

// A conventional channel's parameter block (2)
#define NXDN_CP_CONV_LEN	3u
#define NXDN_CP_4800		0x04u	// byte 0: 4800 bit/s, else 9600
enum { NXDN_SQUELCH_RAN, NXDN_SQUELCH_SELECTIVE };
typedef struct {
	bool rate9600;
	uint8_t rx_ran;		// 0: every RAN
	uint8_t tx_ran;
	uint8_t squelch;	// NXDN_SQUELCH_*
} nxdn_conv_t;

// Decode len bytes of a block (a short one, or none, takes the defaults); never fails: a value out of range is read as
// its default
void nxdn_conv_decode(const uint8_t *b, int len, nxdn_conv_t *out);
// The block for c (NXDN_CP_CONV_LEN bytes): a TX RAN equal to its default is stored as "default"
void nxdn_conv_encode(const nxdn_conv_t *c, uint8_t out[NXDN_CP_CONV_LEN]);
// The radio-wide unit ID from the kind's record (len bytes, -1 = none): when unset or out of range the DMR ID where it
// fits, else 1
uint16_t nxdn_unit_id_decode(const uint8_t *b, int len, uint32_t dmr_id);

// What a conventional call is set up from: the channel's block, our unit ID and the talk target
typedef struct {
	nxdn_conv_t conv;
	uint16_t unit_id;
	uint16_t target;	// group or unit
	bool target_unit;	// target is a unit (a private contact)
} nxdn_conv_cfg_t;
// Channel number's set-up from its tag, the radio-wide record and its record ch (read by codeplug_channel); false when
// it isn't a live NXDN channel
bool nxdn_conv_load(uint16_t number, const channel_t *ch, nxdn_conv_cfg_t *out);
// Write ch at number as an NXDN channel (channel-modes.md 2.4): the base record in its safe stand-in shape,
// then the tag with conv's block, transmit allowed unless ch->rx_only. A channel that is NXDN already keeps its tag's
// system, fallback, other flags and what its block holds after conv's (codeplug_tag_rewrite), and under a base-own
// tag its record's own mode; else the tag is new.
// Where an NXDN channel is made from the radio's own settings (the VFO, a mode changed for now) or its block edited
bool nxdn_conv_save(uint16_t number, const channel_t *ch, const nxdn_conv_t *conv);

#endif
