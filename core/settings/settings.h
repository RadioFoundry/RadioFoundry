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
	// The radio core's settings (radio_settings_t, docs/spec/radio-features.md), field by field so the layout stays
	// fixed; rs_stored 0 = never set (the radio core's defaults apply)
	uint16_t rs_tot_s;
	uint8_t rs_tot_warn_s;
	uint8_t rs_bcl;
	uint8_t rs_flags;		// SETTINGS_RS_* bits
	uint8_t rs_scan_resume;
	uint8_t rs_scan_hold_s;
	uint8_t rs_beep_level;
	uint16_t rs_scan_delay_ms;
	uint16_t rs_scan_dwell_ms;
	uint8_t rs_vox;
	uint8_t rs_vox_level;
	uint16_t rs_vox_tail_ms;
	uint8_t rs_rx_save;
	uint8_t rs_stored;		// 1 = the rs_ fields hold the radio core's settings
	uint16_t rs_rx_save_idle_s;
	uint16_t rs_battery_off_s;
	uint16_t rs_backlight_dim_s;
	uint16_t rs_backlight_off_s;
	uint16_t reserved3;
	// The DMR data service's settings (dmrDataSettings_t, docs/spec/dmr-data.md); ds_stored 0 = never set
	uint32_t ds_ars_server;		// ARS server ID, 0 = ARS off
	uint8_t ds_flags;		// SETTINGS_DS_* bits
	uint8_t ds_stored;		// 1 = the ds_ fields hold the data service's settings
	uint16_t reserved4;
	// Appended for the UI's settings (docs/spec/ui.md 4). The radio core's DMR reply window belongs with the rs_
	// fields (taken only when rs_stored is 1); the others are the UI's
	uint8_t rs_reply_s;		// radio_settings_t.reply_s: 0-30 s, SETTINGS_UNSET8 = the codeplug's hang times
	uint8_t mic_gain;		// 0-10 (5 = as the board sets it); kept only: no board takes it yet
	uint16_t auto_lock_s;		// lock the keys after this long without a key press, 0 = never
	uint16_t long_press_ms;		// long press threshold for core/input, 0 = INPUT_LONG_MS
	uint16_t repeat_ms;		// key repeat period, 0 = INPUT_REPEAT_MS
	uint8_t gps;			// GPS on (kept only: no GPS driver yet)
	uint8_t pos_format;		// position format shown (UI list index)
	uint16_t pos_send_s;		// send our position every so many seconds, 0 = never (kept only)
} settings_t;

// settings_t.rs_flags
#define SETTINGS_RS_ROGER_BEEP		0x01u
#define SETTINGS_RS_TX_INHIBIT		0x02u
#define SETTINGS_RS_SCAN_PRIORITY	0x04u
#define SETTINGS_RS_SCAN_BEEP		0x08u
#define SETTINGS_RS_KEY_BEEP		0x10u

// settings_t.ds_flags
#define SETTINGS_DS_RADIO_CHECK_REPLY	0x01u
#define SETTINGS_DS_CALL_ALERT_ACK	0x02u
#define SETTINGS_DS_LRRP_REPLY		0x04u

typedef enum {
	SETTINGS_RAM,		// no flash: defaults, nothing saved
	SETTINGS_DEFAULTS,	// flash usable but no valid record (new radio, or an incompatible layout)
	SETTINGS_LOADED,	// the newest record, same size
	SETTINGS_UPGRADED,	// the newest record, written by a firmware with fewer or more fields
	SETTINGS_FOREIGN,	// the settings area holds another firmware's data: defaults, nothing saved until claimed
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
