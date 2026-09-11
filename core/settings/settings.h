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
#define SETTINGS_ROAM_ZONE_BYTES	10	// roaming zone bits for zones 0..79

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
	// DMR emergency (dmr_emergency_settings_t, docs/spec/dmr-emergency.md); em_stored 0 = never set (the defaults)
	uint32_t em_dst;		// emergency target, 0 = the channel's talk target
	uint8_t em_mode;		// dmr_emergency_mode_t
	uint8_t em_flags;		// SETTINGS_EM_* bits
	uint8_t em_alarm_tries;
	uint8_t em_hot_mic_s;
	uint8_t em_stored;		// 1 = the em_ fields hold the emergency settings
	// The hotspot's CW ID (docs/spec/mmdvm-protocol.md 2.11): 0 = the default
	uint8_t cw_wpm;			// Morse speed, words per minute (PARIS)
	uint16_t cw_tone_hz;		// keyer tone
	// DMR repeater roaming (docs/spec/dmr-roaming.md 5.1): the radio core's roam_* settings, taken only when
	// roam_stored is 1, and the roaming zones (written by the radio core when the user turns roaming on for a zone)
	int8_t roam_rssi_dbm;		// threshold
	uint8_t roam_hyst_db;		// hysteresis
	uint8_t roam_probe;		// 0 passive, 1 wake probe
	uint8_t roam_stored;		// 1 = the roam_ values are set
	uint16_t roam_check_s;		// beacon check interval, 0 = off
	uint8_t roam_zones[SETTINGS_ROAM_ZONE_BYTES];	// bit z % 8 of byte z / 8: zone z is a roaming zone
	// More of the UI's settings (docs/spec/ui.md 4; appended for uifix)
	uint8_t theme;			// 0 dark, 1 light, 2 high contrast (color displays)
	uint8_t ui_flags;		// SETTINGS_UI_* bits
	uint8_t updown_home;		// what Up / Down change on Home: 0 channel, 1 zone, 2 talk target
	uint8_t low_batt_pct;		// warn at this battery percent (5-30), 0 = only the board's low voltage
	uint16_t auto_off_min;		// switch off after this long without use, 0 = never
	uint16_t reserved5;
	// Roaming beacons and the survey (docs/spec/dmr-roaming.md 5.1; appended for roam2). Taken with the other roam_
	// fields when roam_stored is 1; a record without them keeps these defaults (60 s, 4 s, synchronized survey)
	uint16_t roam_beacon_s;		// the repeaters' beacon interval, 0 = no beacons, else 10..600
	uint8_t roam_beacon_len_s;	// their beacon duration, 1..10
	uint8_t roam_survey;		// 0 off, 1 the sites beacon together, 2 each site on its own schedule
	uint8_t rpt_beacon;		// our repeater roles key an idle beacon (roam_beacon_s / _len_s): 0 off, 1 on
	uint8_t reserved6[3];
	// APRS (core/aprs/aprs.h aprs_cfg_t, docs/spec/aprs.md 11); taken only when aprs_stored is 1, else the defaults
	// (automatic transmit off)
	uint8_t aprs_flags;		// SETTINGS_APRS_* bits
	uint8_t aprs_flags2;		// SETTINGS_APRS2_* bits
	uint8_t aprs_region;		// aprs_region_t: the APRS frequency
	uint8_t aprs_ssid;		// our SSID, 0-15
	uint32_t aprs_freq_hz;		// the custom region's frequency
	int32_t aprs_lat;		// the fixed position, 1e-7 degree (both 0 = not set)
	int32_t aprs_lon;
	uint32_t aprs_dmr_dst;		// digital APRS: the destination ID, 0 = the default
	int16_t aprs_alt_m;		// the fixed position's altitude, INT16_MIN = none
	uint16_t aprs_interval_s;	// fixed beacon interval, 0 = none
	uint16_t aprs_distance_m;	// beacon after moving this far, 0 = off
	char aprs_symbol[2];		// symbol table and code
	uint8_t aprs_path;		// aprs_path_t
	uint8_t aprs_pos_src;		// aprs_pos_src_t
	uint8_t aprs_level;		// AFSK level, 0-100
	uint8_t aprs_txdelay;		// preamble in 10 ms units
	uint8_t aprs_misc;		// bits 0-1 aprs_via_t, 2-3 digipeater mode, 4-6 position ambiguity
	uint8_t aprs_stored;		// 1 = the aprs_ fields are set
	uint16_t reserved7;
} settings_t;

// settings_t.ui_flags (defaults: hints, message tone, talker alias and PTT when locked on)
#define SETTINGS_UI_LARGE_TEXT		0x01u
#define SETTINGS_UI_HINTS		0x02u
#define SETTINGS_UI_MESSAGE_TONE	0x04u
#define SETTINGS_UI_ALIAS		0x08u	// show received talker aliases
#define SETTINGS_UI_PTT_LOCKED		0x10u	// PTT works while the keys are locked
#define SETTINGS_UI_HEAR_ALL		0x20u	// hear all on whenever a DMR channel is selected

// settings_t.rs_flags
#define SETTINGS_RS_ROGER_BEEP		0x01u
#define SETTINGS_RS_TX_INHIBIT		0x02u
#define SETTINGS_RS_SCAN_PRIORITY	0x04u
#define SETTINGS_RS_SCAN_BEEP		0x08u
#define SETTINGS_RS_KEY_BEEP		0x10u
#define SETTINGS_RS_NO_TALK_PERMIT	0x20u	// the talk permit tone off (0 = on, as before)

// settings_t.ds_flags
#define SETTINGS_DS_RADIO_CHECK_REPLY	0x01u
#define SETTINGS_DS_CALL_ALERT_ACK	0x02u
#define SETTINGS_DS_LRRP_REPLY		0x04u
#define SETTINGS_DS_SMS_ETSI		0x08u	// sms_send's default format is ETSI, else TMS
#define SETTINGS_DS_RADIO_CHECK_ETSI	0x10u	// our radio checks in the ETSI form (FID 0x00)

// settings_t.em_flags
#define SETTINGS_EM_DST_PRIVATE		0x01u
#define SETTINGS_EM_ETSI_FID		0x02u
#define SETTINGS_EM_NO_TONE		0x04u

// settings_t.aprs_flags / aprs_flags2
#define SETTINGS_APRS_TX		0x01u	// automatic beacons
#define SETTINGS_APRS_SMART		0x02u	// SmartBeaconing
#define SETTINGS_APRS_COMPRESSED	0x04u
#define SETTINGS_APRS_AFTER_PTT		0x08u
#define SETTINGS_APRS_BOOT		0x10u	// beacon on the first position after power-up
#define SETTINGS_APRS_KISS		0x20u	// KISS TNC on the host port
#define SETTINGS_APRS_NARROW		0x40u	// 12.5 kHz on the APRS frequency
#define SETTINGS_APRS_RX		0x80u	// receive on the APRS frequency (boards that can)
#define SETTINGS_APRS2_IGATE		0x01u	// RF -> APRS-IS
#define SETTINGS_APRS2_IGATE_TX		0x02u	// APRS-IS -> RF (messages to local stations)
#define SETTINGS_APRS2_MESSAGING	0x04u	// we take messages ('=' / '@' positions, acks)
#define SETTINGS_APRS2_DMR_UDT		0x08u	// digital APRS as an ETSI UDT location (else Motorola LRRP)
#define SETTINGS_APRS2_TO_IS		0x10u	// our own beacons to APRS-IS through the host helper
#define SETTINGS_APRS2_DMR_GPS		0x20u	// our position in DMR voice calls (ETSI GPS Info LC, CONFIG_DMR_GPS)

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
