// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The codeplug as the core sees it: channels and zones, whatever format is stored underneath.
// Today: read-only from a legacy-layout codeplug already in the radio's flash (docs/spec/legacy-cps-interop.md),
// or a small built-in set when there is none. The native format comes later.
#ifndef CODEPLUG_H
#define CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_radio.h"
#include "config.h"

#define CODEPLUG_NAME_LEN	16
#define CODEPLUG_ZONE_MAX	80	// channels per zone

typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint32_t rx_hz;
	uint32_t tx_hz;
	uint8_t mode;		// radio_mode_t
	uint8_t bw;		// radio_bw_t
	subtone_t rx_tone;
	subtone_t tx_tone;
	uint8_t power;		// board power level index, or CODEPLUG_POWER_DEFAULT
	bool rx_only;
	// DMR (unused by FM)
	uint8_t color_code;
	uint8_t timeslot;	// 1 or 2
	uint16_t contact;	// talk target contact index, 0 = none
	uint16_t listen_list;	// RX group list index, 0 = none
	uint32_t dmr_id;	// own DMR ID for this channel, 0 = the radio's
	bool dmo;		// direct mode even when RX and TX differ (legacy "force DMO")
	uint16_t tot_s;		// TX time-out, 0 = the radio's
	uint8_t alias_tx;	// talker alias TX: CODEPLUG_ALIAS_TS1_* / _TS2_* bits
} channel_t;

// channel_t.alias_tx: what the channel sends as talker alias on each timeslot (text = our radio name)
#define CODEPLUG_ALIAS_TS1_TEXT	0x01u
#define CODEPLUG_ALIAS_TS2_TEXT	0x02u

#define CODEPLUG_POWER_DEFAULT	0xFF

typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint16_t count;
	uint16_t channels[CODEPLUG_ZONE_MAX];	// codeplug channel numbers, 1-based
} zone_t;

typedef enum { CODEPLUG_NONE, CODEPLUG_BUILTIN, CODEPLUG_LEGACY } codeplug_source_t;

// Find and check the stored codeplug. Call once the board's flash works
codeplug_source_t codeplug_init(void);
codeplug_source_t codeplug_source(void);

// Highest channel number that may be in use (channels are 1..count, some may be empty)
uint16_t codeplug_channel_count(void);
// false if the slot is empty or unreadable
bool codeplug_channel(uint16_t number, channel_t *out);

// Zones 0..count-1. With no zones stored there is one zone "All channels" holding every used channel
uint16_t codeplug_zone_count(void);
bool codeplug_zone(uint16_t index, zone_t *out);

#if CONFIG_DMR
#define CODEPLUG_CONTACTS_MAX	1024
#define CODEPLUG_LISTS_MAX	76	// RX group lists (listen lists)
#define CODEPLUG_LIST_LEN	32	// contacts per list

typedef enum { CONTACT_GROUP, CONTACT_PRIVATE, CONTACT_ALL_CALL } contact_type_t;

typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint32_t id;		// talkgroup or DMR ID (24 bits)
	uint8_t type;		// contact_type_t
	uint8_t timeslot;	// 1 or 2 overrides the channel's, 0 = the channel's
} contact_t;

typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint8_t count;
	uint16_t contacts[CODEPLUG_LIST_LEN];	// contact indices, 1-based
} listen_list_t;

// The radio's own DMR ID from the general settings, 0 if none is stored
uint32_t codeplug_dmr_id(void);
// Contacts 1..CODEPLUG_CONTACTS_MAX; false if empty or unreadable
bool codeplug_contact(uint16_t index, contact_t *out);
// RX group lists 1..CODEPLUG_LISTS_MAX; false if unused or unreadable
bool codeplug_listen_list(uint16_t index, listen_list_t *out);
// The first contact with this ID: group true finds a talkgroup (or all call for 0xFFFFFF), false a private contact.
// Reads every contact record (24 KB of flash) in the worst case: cache the answer
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out);

#define CODEPLUG_RADIO_NAME_LEN	8

// General DMR settings (legacy general settings, docs/spec/legacy-cps-interop.md 7.3.2)
typedef struct {
	char radio_name[CODEPLUG_RADIO_NAME_LEN + 1];	// callsign / radio name, "" if none
	uint16_t group_hang_ms;		// group call hang time, 0 = none stored
	uint16_t private_hang_ms;	// private call hang time, 0 = none stored
} codeplug_dmr_settings_t;

// false (and out zeroed) without a legacy codeplug
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out);
#endif

#endif
