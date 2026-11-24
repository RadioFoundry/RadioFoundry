// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 channels and systems in the codeplug (docs/spec/p25-codeplug.md): the codec from the stored bytes to plain structs
// a P25 engine is configured from. Bytes in, values out: no engine here. The conventional part is built with
// CONFIG_P25, the trunked part with CONFIG_P25_TRUNK
#ifndef RFOUNDRY_P25_CODEPLUG_H
#define RFOUNDRY_P25_CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

#ifndef P25_NAC_DEFAULT
#define P25_NAC_DEFAULT		0x293
#endif
#ifndef P25_NAC_ANY
#define P25_NAC_ANY		0xF7E	// a receiver set to it takes every NAC
#endif
#ifndef P25_NAC_REPEAT_ANY
#define P25_NAC_REPEAT_ANY	0xF7F	// a repeater's value: never a radio's
#endif
#define P25_CP_NAC_SET		0x8000u	// a stored NAC field holds a value (clear: the default)
#define P25_CP_TG_DEFAULT	1u	// the talk group of a channel with no contact
#define P25_CP_TG_ALL		0xFFFFu	// the all-call contact's talk group
#define P25_CP_UNIT_MAX		0xFFFFFEu

// A conventional channel's parameter block (2)
#define P25_CP_CONV_LEN		5u
enum { P25_SQUELCH_NORMAL, P25_SQUELCH_SELECTIVE };
typedef struct {
	uint16_t rx_nac;	// P25_NAC_ANY: every NAC
	uint16_t tx_nac;
	uint8_t squelch;	// P25_SQUELCH_*
} p25_conv_t;

// Decode len bytes of a block (a short one, or none, takes the defaults); never fails: a value out of range is read as
// its default
void p25_conv_decode(const uint8_t *b, int len, p25_conv_t *out);
// The block for c (P25_CP_CONV_LEN bytes): a NAC equal to its default is stored as "default"
void p25_conv_encode(const p25_conv_t *c, uint8_t out[P25_CP_CONV_LEN]);
// The radio-wide unit ID from the kind's record (len bytes, -1 = none): the DMR ID when unset or out of range
uint32_t p25_unit_id_decode(const uint8_t *b, int len, uint32_t dmr_id);
// The radio-wide unit ID as stored (1): the kind's record, else dmr_id
uint32_t p25_radio_unit_id(uint32_t dmr_id);

// What a conventional call is set up from: the channel's NACs and squelch, our unit ID and the talk target
typedef struct {
	p25_conv_t conv;
	uint32_t unit_id;	// 0: none (PTT is refused)
	uint32_t target;	// talk group or unit
	bool target_unit;	// target is a unit (a private contact)
	bool talkaround;
} p25_conv_cfg_t;
// Channel number's set-up from its tag, the radio-wide record and its record ch (read by codeplug_channel); false when
// it isn't a live P25 conventional channel
bool p25_conv_load(uint16_t number, const channel_t *ch, p25_conv_cfg_t *out);
// The parameter block alone, without the contact and the unit ID (a list row, channel info): false as p25_conv_load
bool p25_conv_params(uint16_t number, const channel_t *ch, p25_conv_t *out);
// Write ch at number as a P25 conventional channel (channel-modes.md 2.4): the base record in its safe stand-in shape,
// then the tag with conv's block, transmit allowed unless ch->rx_only. A channel that is P25 already keeps its tag's
// system, fallback, other flags and what its block holds after conv's (codeplug_tag_rewrite), and under a base-own
// tag its record's own mode; else the tag is new.
// Where a P25 channel is made from the radio's own settings (the VFO, a mode changed for now) or its block edited
bool p25_conv_save(uint16_t number, const channel_t *ch, const p25_conv_t *conv);

#if CONFIG_P25_TRUNK
// A trunked system's block (3), after the generic header
#define P25_CP_TSYS_LEN		10u
#define P25_CP_WACN_ANY		0xFFFFFFu
#define P25_CP_SYSID_ANY	0xFFFFu
#define P25_TSYS_SKIP_ENC	0x01u	// options: skip encrypted calls
#define P25_TSYS_PLAN_ONLY	0x02u	// options: use only the programmed identifier plan
#define P25_TSYS_NO_REG		0x04u	// options: ask for voice channels without registering or affiliating
typedef struct {
	uint32_t wacn;		// P25_CP_WACN_ANY: any
	uint16_t sysid;		// P25_CP_SYSID_ANY: any
	uint16_t cc_nac;	// P25_NAC_ANY: any
	uint8_t options;
	uint8_t hold_s;		// 0 = no hold
	int8_t roam_dbm;	// 0 = the radio's
} p25_tsys_t;
void p25_tsys_decode(const uint8_t *b, int len, p25_tsys_t *out);
void p25_tsys_encode(const p25_tsys_t *s, uint8_t out[P25_CP_TSYS_LEN]);

// A site entry's body (3.1), a control channel's (3.2) and an identifier plan row's (3.3)
#define P25_CP_UNKNOWN		0xFFu
typedef struct {
	uint8_t rfss, site;	// P25_CP_UNKNOWN: not known
} p25_site_t;
typedef struct {
	uint16_t chan;		// identifier << 12 | channel, 0 = by frequency only
	uint32_t rx_hz, tx_hz;
} p25_ctl_t;
#define P25_CP_IDEN_LEN		18u
enum { P25_IDEN_FDMA, P25_IDEN_TDMA };
typedef struct {
	uint32_t base_hz;
	uint32_t spacing_hz;
	int32_t tx_offset_hz;
	uint32_t bw_hz;
	uint8_t type;		// P25_IDEN_*
	uint8_t slots;		// TDMA: slots per carrier
} p25_iden_t;
void p25_site_decode(const uint8_t *b, int len, p25_site_t *out);
void p25_ctl_decode(const uint8_t *b, int len, p25_ctl_t *out);
void p25_iden_decode(const uint8_t *b, int len, p25_iden_t *out);
void p25_iden_encode(const p25_iden_t *i, uint8_t out[P25_CP_IDEN_LEN]);

// A trunked channel's parameter block (4)
#define P25_TCH_FOLLOW_ALL	0x01u
#define P25_TAG_NO_FALLBACK	0x10u	// tag flag (a kind bit): no fallback on this channel

// What following a system needs: the system, its sites' control channels in hunt order, the plan; the channel's
// talk group, whether it follows every group, the fallback
#define P25_CP_CTL_MAX		16u
#define P25_CP_IDEN_MAX		16u
typedef struct {
	p25_tsys_t sys;
	uint32_t unit_id;
	bool listen_only;		// the system's transmit side off (channel-modes.md 7.1)
	uint8_t ctl_count;
	uint8_t ctl_site[P25_CP_CTL_MAX];	// each control channel's site group
	p25_ctl_t ctl[P25_CP_CTL_MAX];
	uint16_t iden_set;		// bit n: identifier n programmed
	p25_iden_t iden[P25_CP_IDEN_MAX];
	uint16_t talk_group;
	bool follow_all;
	uint16_t fallback;		// channel number, 0 = none
} p25_trunk_cfg_t;
// Channel number's set-up from its tag, its system and its record ch; false when it isn't a live P25 trunked channel
// or its system is missing
bool p25_trunk_load(uint16_t number, const channel_t *ch, p25_trunk_cfg_t *out);
#endif

#endif
