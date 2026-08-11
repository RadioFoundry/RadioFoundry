// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio's own settings, kept across power cycles (docs/spec/settings-store.md): where the radio was (zone,
// channel, VFO), the VFO's settings and the user's preferences. Stored as versioned records appended to a ring of
// flash sectors (board.flash->map settings area): a record is valid only once its commit word is programmed, so a
// power cut during a save leaves the previous record in force; sectors are erased in turn (wear spread over the
// ring). Changes are saved after SETTINGS_SAVE_DELAY_MS without a further change (settings_poll), or at once with
// settings_flush (power off, the CPS's "save").
//
// Using it: s = settings_begin(); change fields; settings_end(now). No flash operation happens inside, so any task
// may do this (the radio task included). Without a flash map everything works from RAM.
#ifndef ODR_SETTINGS_H
#define ODR_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_LAYOUT		1	// bumped only when a field changes meaning or moves (old records then reset)
#define SETTINGS_SAVE_DELAY_MS	2000
#define SETTINGS_NAME_LEN	16
#define SETTINGS_PRIVACY_KEYS	4
#define SETTINGS_UNSET8		0xFFu	// "not set: the radio's default" in an 8-bit field

// Fields are only ever appended (docs/spec/settings-store.md 3); a record written by an older firmware fills the
// fields it knows, the rest keep their defaults. Fixed-width members, ordered so there is no padding
typedef struct {
	// Where the radio is
	uint8_t vfo;			// 1 = tuning the VFO, 0 = on a channel
	uint8_t squelch;		// 0-9, SETTINGS_UNSET8 = the radio's default
	uint16_t zone_index;		// zone 0..n-1
	uint16_t channel;		// codeplug channel number in that zone, 0 = none yet
	uint16_t zone_pos;		// its position in the zone (used when the channel number is no longer there)
	// The VFO (vfo_rx_hz 0 = never set)
	uint32_t vfo_rx_hz;
	uint32_t vfo_tx_hz;
	uint32_t vfo_step_hz;		// 0 = the radio's default
	uint8_t vfo_mode;		// radio_mode_t
	uint8_t vfo_bw;			// radio_bw_t
	uint8_t vfo_power;		// board power level, SETTINGS_UNSET8 = default
	uint8_t vfo_cc;			// DMR color code
	uint8_t vfo_ts;			// DMR timeslot 1 / 2
	uint8_t vfo_rx_tone_type;	// tone_type_t
	uint8_t vfo_tx_tone_type;
	uint8_t reserved0;
	uint16_t vfo_rx_tone;		// subtone value
	uint16_t vfo_tx_tone;
	// Preferences (independent of the volume knob)
	uint8_t brightness;		// backlight percent, 10-100
	uint8_t backlight_s;		// backlight time-out in s, 0 = always on, SETTINGS_UNSET8 = the UI's default
	uint8_t beeps;			// key and status beeps: 0 off, 1 on
	uint8_t hotspot_allowed;	// the host may make the radio an MMDVM hotspot
	uint8_t master_power;		// power for channels set to "default", SETTINGS_UNSET8 = the board's default
	uint8_t usb_mode;		// usb_mode_t (serial / network)
	uint16_t reserved1;
	uint32_t dmr_id;		// own DMR ID override, 0 = the codeplug's
	char radio_name[SETTINGS_NAME_LEN];	// NUL padded, "" = the codeplug's callsign
	// DMR basic privacy keys (the key store dmr-voice's privacy uses; docs/spec/settings-store.md 3). A channel's
	// choice of key will be a slot number in the native codeplug
	uint32_t privacy_key[SETTINGS_PRIVACY_KEYS];	// key value per slot (0 = empty)
	uint8_t privacy_type[SETTINGS_PRIVACY_KEYS];	// dmrPrivacyType_t per slot
	uint8_t privacy_active;		// slot in use radio-wide, 1..SETTINGS_PRIVACY_KEYS, 0 = privacy off
	uint8_t reserved2[3];
} settings_t;

typedef enum {
	SETTINGS_RAM,		// no flash: defaults, nothing saved
	SETTINGS_DEFAULTS,	// flash usable but no valid record (new radio, or an incompatible layout)
	SETTINGS_LOADED,	// the newest record, same size
	SETTINGS_UPGRADED,	// the newest record, written by a firmware with fewer or more fields
} settings_load_t;

typedef struct {
	uint8_t load;		// settings_load_t
	bool dirty;		// changes not saved yet
	uint32_t seq;		// sequence number of the newest record (0 = none)
	uint32_t saves;		// records written since boot
	uint32_t erases;	// sectors erased since boot
	uint32_t failures;	// saves that failed
	uint16_t sector;	// ring sector holding the newest record
	uint16_t slot;		// its slot in the sector
	uint16_t skipped;	// unusable slots seen at load (torn writes)
} settings_stats_t;

void settings_defaults(settings_t *s);

// Load the newest valid record (after storage_init). Call once, before anything reads the settings
settings_load_t settings_init(void);

// A consistent copy
void settings_read(settings_t *out);
// Change: begin returns the live copy (under a short lock), end releases it and notes the time if anything changed
settings_t *settings_begin(void);
void settings_end(uint32_t now_ms);

// Save once the changes have rested SETTINGS_SAVE_DELAY_MS (call it every 100 ms or so)
void settings_poll(uint32_t now_ms);
// Save now if anything changed; true when nothing is left unsaved
bool settings_flush(void);

void settings_get_stats(settings_stats_t *s);

#endif
