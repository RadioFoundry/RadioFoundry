// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The codeplug as the core sees it: channels, zones, contacts and RX group lists, whatever format is stored
// underneath. This is the editing model the UI and the CPS share (docs/spec/codeplug-native.md 2).
// Today it is stored in the open-firmware layout (docs/spec/open-firmware-cps.md 7), read on demand and written through
// the sector journal (core/storage); with nothing stored, a small built-in set of channels, which the first edit
// turns into a stored codeplug. The native format comes later behind the same calls.
// All calls may come from any task (they take the storage lock); writes can take some 100 ms (sector rewrites).
#ifndef CODEPLUG_H
#define CODEPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_radio.h"
#include "config.h"

#define CODEPLUG_NAME_LEN	16
// A name field's text valid: Latin-1 and UTF-8 mixed, no truncated or stray UTF-8 lead. The rule the CPS and the radio
// use, also exposed for tests (core/codeplug/codeplug.c)
bool name_field_ok(const uint8_t *p, unsigned len, bool *empty);
#define CODEPLUG_ZONE_MAX	80	// channels per zone
#define CODEPLUG_DMR_ID_MAX	16776415u	// a radio's own ID; the IDs above it are the system's (gateways, all call)

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
	bool dmo;		// direct mode even when RX and TX differ (open-firmware "force DMO")
	uint16_t tot_s;		// TX time-out, 0 = the radio's
	uint8_t alias_tx;	// talker alias TX: CODEPLUG_ALIAS_TS1_* / _TS2_* bits
	bool scan_skip;		// left out of zone scans
	bool vox;		// VOX allowed on this channel (FM)
	bool no_beep;		// no DMR talk permit / call end beeps on this channel
	bool no_eco;		// no receiver battery saving on this channel
	bool roam;		// a roaming site (the open-firmware Roaming flag; docs/spec/dmr-roaming.md 5.2), read only
	bool tone_burst;	// FM: the 1750 Hz access burst at each PTT key-up (repeaters that need it)
	// SFR (docs/spec/sfr.md 4): flag 1 bit 1 and the two SFR bytes as stored, for sfrChannelDecode(); read only
	// (the encoder keeps them as read), all 0 when the channel has none
	uint8_t sfr_flag1;
	uint8_t sfr_a;
	uint8_t sfr_b;
} channel_t;

// channel_t.alias_tx: what the channel sends as talker alias on each timeslot (text = our radio name)
#define CODEPLUG_ALIAS_TS1_TEXT	0x01u
#define CODEPLUG_ALIAS_TS2_TEXT	0x02u
// ... and whether its calls on that timeslot carry our position (GPS Info, docs/spec/dmr-voice.md 3.1), whatever the
// radio-wide "Send in DMR calls" setting says. The codeplug's own name for the bit is APRS
#define CODEPLUG_ALIAS_TS1_GPS	0x04u
#define CODEPLUG_ALIAS_TS2_GPS	0x08u

#define CODEPLUG_POWER_DEFAULT	0xFF

typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint16_t count;
	uint16_t channels[CODEPLUG_ZONE_MAX];	// codeplug channel numbers, 1-based
} zone_t;

typedef enum { CODEPLUG_NONE, CODEPLUG_BUILTIN, CODEPLUG_OPENFW } codeplug_source_t;

// Find and check the stored codeplug. Call once the board's flash works
codeplug_source_t codeplug_init(void);
codeplug_source_t codeplug_source(void);
// The open-firmware area holds records in use that aren't an open-firmware codeplug: another firmware's own
// codeplug, left there when RadioFoundry was installed over it. Decided at every index (start-up, codeplug_reload) by
// the rule the web editor uses (docs/spec/codeplug-stock-tyt.md 2, step 1), unless the owner accepted it (below). The
// radio then shows no codeplug (the built-in set, source CODEPLUG_BUILTIN) and writes nothing there: every edit fails
// until codeplug_start_new or codeplug_accept (the UI asks which first)
bool codeplug_foreign(void);
// The evidence counts behind it: the good and bad open-firmware records found
void codeplug_evidence(uint16_t *good, uint16_t *bad);
// "Start new": clear what the radio reads of the other firmware's codeplug (general settings up to the hang times,
// boot text, quick keys, contacts) and start a stored one from the built-in set. true when the codeplug is ours after
bool codeplug_start_new(void);
// "Use anyway": the codeplug is the radio's own (on) or judged again (off), erasing nothing; codeplug_reload shows it.
// The settings store keeps it (settings_t.cp_flags, app.c). codeplug_written drops it
void codeplug_accept(bool on);
typedef enum { CODEPLUG_ACCEPT_NO, CODEPLUG_ACCEPT_YES, CODEPLUG_ACCEPT_DROPPED } codeplug_accept_t;
// DROPPED: a codeplug was written since start-up or the last codeplug_accept, so the stored acceptance goes
codeplug_accept_t codeplug_acceptance(void);
// The housekeeping task's: true once after a change of the acceptance, with the state to store (YES or NO; DROPPED
// becomes NO here). No flag of its own in app.c: RAM is short
bool codeplug_accept_take(codeplug_accept_t *store);

// Highest channel number that may be in use (channels are 1..count, some may be empty)
uint16_t codeplug_channel_count(void);
// false if the slot is empty or unreadable
bool codeplug_channel(uint16_t number, channel_t *out);
// Reads of a used channel that failed (flash read or record decode), since start-up
uint32_t codeplug_channel_errors(void);

// Zones 0..count-1. With no zones stored there is one zone "All channels" holding every used channel
uint16_t codeplug_zone_count(void);
bool codeplug_zone(uint16_t index, zone_t *out);
// The name alone, false as codeplug_channel / codeplug_zone: a search by name reads 16 bytes a record, not all of it
bool codeplug_channel_name(uint16_t number, char out[CODEPLUG_NAME_LEN + 1]);
bool codeplug_zone_name(uint16_t index, char out[CODEPLUG_NAME_LEN + 1]);
// Whether a channel number holds a channel, from the in-use bitmap: no flash read
bool codeplug_channel_used(uint16_t number);
// Every channel 1..codeplug_channel_count() in order with its name, NULL for an empty number, a few records per flash
// read (as codeplug_contacts_each, whose rules fn follows). names false: only which numbers are used, from the in-use
// bitmap without a flash read; the name is "" then. False when the codeplug can't be read
typedef bool (*codeplug_name_fn)(uint16_t number, const char *name, void *ctx);
bool codeplug_channel_names_each(codeplug_name_fn fn, void *ctx, bool names);
// true when the zones are stored ones (false: the virtual "All channels" zone)
bool codeplug_zones_stored(void);

// --- Editing. Each returns false if nothing is stored and can't be (no flash), the argument is invalid, or the
// flash failed. A change bumps codeplug_serial(), so the radio core knows to re-read its channel ---
#define CODEPLUG_CHANNELS_MAX	1024
#define CODEPLUG_ZONES_MAX	68

// Bumped on every change (edits, and codeplug_reload after the CPS wrote the image)
uint32_t codeplug_serial(void);
// Store a channel at number 1..CODEPLUG_CHANNELS_MAX (new or replaced; fields the model doesn't know are kept)
bool codeplug_set_channel(uint16_t number, const channel_t *ch);
bool codeplug_delete_channel(uint16_t number);
// The lowest unused channel number, 0 if all are used
uint16_t codeplug_free_channel(void);
// Replace zone index (0..count-1), or add one with index == codeplug_zone_count() (when only the virtual zone
// exists, index 0 replaces it with a stored zone). Channels that aren't stored are refused
bool codeplug_set_zone(uint16_t index, const zone_t *z);
bool codeplug_delete_zone(uint16_t index);
// The index of the zone the last successful codeplug_set_zone wrote. An added zone takes the lowest free record, so
// it can land before zones that were there (core/codeplug/zone_edit.c follows it)
uint16_t codeplug_zone_written(void);
// The index a zone added now (codeplug_set_zone with index == count) would take
uint16_t codeplug_zone_add_index(void);
// Channels one zone can hold: CODEPLUG_ZONE_MAX, or 16 in a codeplug with the older 16-channel zone records
uint16_t codeplug_zone_capacity(void);
// The radio name (the open-firmware callsign field, 8 characters), "" if none
#define CODEPLUG_RADIO_NAME_LEN	8
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1]);
bool codeplug_set_radio_name(const char *name);
// The two boot screen text lines the CPS writes (empty when unset or not in this codeplug)
void codeplug_boot_text(char line1[CODEPLUG_NAME_LEN + 1], char line2[CODEPLUG_NAME_LEN + 1]);
bool codeplug_set_boot_text(const char *line1, const char *line2);
// The quick-key slots (EEPROM 0x7524, 10 x 2 bytes LE): RadioFoundry keeps the side key assignment there
// (core/input/keyassign.h). Read: all 0 and false without an open-firmware codeplug. Write: the first edit stores the
// built-in set, as other edits do
#define CODEPLUG_QUICK_KEYS	10
bool codeplug_quick_keys(uint16_t out[CODEPLUG_QUICK_KEYS]);
bool codeplug_set_quick_keys(const uint16_t in[CODEPLUG_QUICK_KEYS]);

#if CONFIG_APRS_AFSK
// An open-firmware codeplug APRS configuration (open-firmware-cps.md 7.3.8; qdmr writes them): what the APRS engine
// takes from it. Not read: the fixed position and the frequency (their encodings aren't in the interop spec yet)
#define CODEPLUG_APRS_CONFIGS	8
#define CODEPLUG_APRS_COMMENT	23
typedef struct {
	uint8_t ssid;
	char path[2][10];	// "WIDE1-1", "" = none
	char sym_table;
	char sym_code;
	char comment[CODEPLUG_APRS_COMMENT + 1];
	uint8_t ambiguity;	// 0-4
} codeplug_aprs_t;
// Configuration n (1..CODEPLUG_APRS_CONFIGS); false when empty, unknown or no open-firmware codeplug
bool codeplug_aprs(uint8_t n, codeplug_aprs_t *out);
// Rewrite configuration n's two path entries ("WIDE1-1", "" for none), the rest of the record kept; false when the
// record is empty or unknown (a new record isn't made: its other fields' encodings aren't all known)
bool codeplug_set_aprs_path(uint8_t n, const char *path1, const char *path2);
#endif

// Re-read everything after the image was written underneath (the CPS); bumps the serial
codeplug_source_t codeplug_reload(void);
// The same after a new codeplug was written (a CPS session or commit, a TFTP write): the acceptance goes with the old one
codeplug_source_t codeplug_written(void);

// The open-firmware 56-byte channel record (open-firmware-cps.md 7.3.3), for the CPS shim's VFO handling. decode: false
// if the frequencies are malformed. encode: updates rec in place, keeping the bytes the model doesn't own
#define CODEPLUG_OPENFW_CHANNEL_SIZE	56
bool codeplug_openfw_decode(const uint8_t *rec, channel_t *out);
void codeplug_openfw_encode(const channel_t *ch, uint8_t *rec);

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
// Every contact slot in order, many records per flash read: a scan with codeplug_contact (one read per slot) took
// about 0.7 s for 1024 slots on an HR_C7000 radio and kept the idle task out. fn gets the slot (1-based) and the contact,
// or NULL for an empty slot, under the storage lock, and returns false to stop. It may call codeplug functions, a
// nested scan included (that one reads slot by slot). False when the codeplug can't be read
typedef bool (*codeplug_contact_fn)(uint16_t index, const contact_t *c, void *ctx);
bool codeplug_contacts_each(codeplug_contact_fn fn, void *ctx);
// RX group lists 1..CODEPLUG_LISTS_MAX; false if unused or unreadable
bool codeplug_listen_list(uint16_t index, listen_list_t *out);
// The contact a channel talks to by default: its TX contact, else the first talkgroup of its RX group list; 0 when
// it has neither (then talkgroup 9). codeplug_talk.c, docs/spec/dmr-voice.md 2
uint16_t codeplug_channel_talk_contact(const channel_t *ch);

bool codeplug_set_dmr_id(uint32_t id);		// 1..CODEPLUG_DMR_ID_MAX, 0 = none
bool codeplug_set_contact(uint16_t index, const contact_t *c);
bool codeplug_delete_contact(uint16_t index);
bool codeplug_set_listen_list(uint16_t index, const listen_list_t *l);
bool codeplug_delete_listen_list(uint16_t index);
// The first contact with this ID: group true finds a talkgroup (or all call for 0xFFFFFF), false a private contact.
// Reads every contact record (24 KB of flash) in the worst case: cache the answer
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out);
// The same, its index (0: none, or a contact record that couldn't be read)
uint16_t codeplug_contact_find_index(uint32_t id, bool group, contact_t *out);

// General DMR settings (open-firmware general settings, docs/spec/open-firmware-cps.md 7.3.2)
typedef struct {
	char radio_name[CODEPLUG_RADIO_NAME_LEN + 1];	// callsign / radio name, "" if none
	uint16_t group_hang_ms;		// group call hang time, 0 = none stored
	uint16_t private_hang_ms;	// private call hang time, 0 = none stored
} codeplug_dmr_settings_t;

// false (and out zeroed) without an open-firmware codeplug
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out);
#endif

#endif
