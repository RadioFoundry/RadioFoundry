// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio's own settings, kept across power cycles (docs/spec/settings-store.md): where the radio was (zone,
// channel, VFO), the VFO's settings and the user's preferences. Stored as versioned records appended to a ring of
// flash sectors (board.flash->map settings area): a record is valid only once its commit word is programmed, so a
// power cut during a save leaves the previous record in force; sectors are erased in turn (wear spread over the
// ring). Changes are saved after SETTINGS_SAVE_DELAY_MS without a further change (settings_poll), or at once with
// settings_flush (power off, the CPS's "save").
//
// Using it: s = settings_begin(); change fields; settings_end(now). No flash operation happens inside, so any task
// may do this (the radio task included). Without a flash map everything works from RAM.
#ifndef RFOUNDRY_SETTINGS_H
#define RFOUNDRY_SETTINGS_H

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
// A zone share import: which sender's zone it was, and the zone it became
#define SETTINGS_ZS_IMPORTS	4
typedef struct {
	uint32_t key;			// hash of the sender's ID and the record's zone ID, 0 = unused
	char zone[16];			// the zone's name here (not terminated when 16 long)
	uint32_t sig;			// XOR of the imported channels' hashes (number and content), as written
	uint8_t count;			// how many channels the import wrote (the first count of the zone)
	uint8_t reserved[3];
} settings_zs_import_t;

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
	uint32_t vfo_step_hz;		// 0 = auto, the width's (radio_req_vfo_step_size)
	uint8_t vfo_mode;		// radio_mode_t in bits 0-3; bits 4-7: the VFO's talk target ID bits 8-11 (see below)
	uint8_t vfo_bw;			// radio_bw_t in bits 0-3; bits 4-7: the talk target ID bits 12-15 (see below)
	uint8_t vfo_power;		// board power level, SETTINGS_UNSET8 = default
	uint8_t vfo_cc;			// DMR color code in bits 0-3; bits 4-7: the VFO's talk target ID bits 16-19 (see below)
	uint8_t vfo_ts;			// DMR timeslot 1 / 2 in bits 0-1; the rest holds the talk target (see below)
	uint8_t vfo_rx_tone_type;	// tone_type_t
	uint8_t vfo_tx_tone_type;
	uint8_t net_flags;		// SETTINGS_NET_* bits (was reserved0: 0 = as before)
	uint16_t vfo_rx_tone;		// subtone value
	uint16_t vfo_tx_tone;
	// Preferences (independent of the volume knob)
	uint8_t brightness;		// backlight percent, 10-100
	uint8_t backlight_s;		// backlight time-out in s, 0 = always on, SETTINGS_UNSET8 = the UI's default
	uint8_t beeps;			// key and status beeps: 0 off, 1 on
	uint8_t hotspot_allowed;	// the host may make the radio an MMDVM hotspot
	uint8_t master_power;		// power for channels set to "default", SETTINGS_UNSET8 = the board's default
	uint8_t usb_mode;		// the USB personality at power-on: 0 serial, 1 network (docs/spec/usb-network.md 1)
	uint8_t accent;			// color displays (docs/spec/ui.md 8): SETTINGS_ACCENT_* (was reserved1's first byte;
					// 0 = the defaults: the theme's highlight, the strip filled)
	uint8_t vfo_talk_lo;		// the DMR VFO's talk target ID, bits 0-7 (was reserved1's second byte; see below)
	uint32_t unused_dmr_id;		// was an own DMR ID override: ignored (the ID is the codeplug's), kept for the layout
	char radio_name[SETTINGS_NAME_LEN];	// NUL padded, "" = the codeplug's callsign
	// DMR basic privacy keys (the key store dmr-voice's privacy uses; docs/spec/settings-store.md 3). A channel's
	// choice of key will be a slot number in the native codeplug
	uint32_t privacy_key[SETTINGS_PRIVACY_KEYS];	// key value per slot (0 = empty)
	uint8_t privacy_type[SETTINGS_PRIVACY_KEYS];	// dmrPrivacyType_t per slot
	uint8_t privacy_active;		// slot in use radio-wide, 1..SETTINGS_PRIVACY_KEYS, 0 = privacy off
	// SNMP (was reserved2; snmp.md 6): the DMR ID notifications go to over RF, little endian, 0 = none
	uint8_t snmp_rf_manager[3];
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
	uint16_t aprsis_range_km;	// APRS-IS: the server filter "m/<km>" around us, 0 = no filter (was reserved3)
	// The DMR data service's settings (dmrDataSettings_t, docs/spec/dmr-data.md); ds_stored 0 = never set
	uint32_t ds_ars_server;		// ARS server ID, 0 = ARS off
	uint8_t ds_flags;		// SETTINGS_DS_* bits
	uint8_t ds_stored;		// 1 = the ds_ fields hold the data service's settings
	uint8_t ds_ip_cnf;		// confirmed IP packets: bits 0-3 tries (0 = the default), bits 4-5 SETTINGS_DS_IPC_*
	uint8_t snmp_flags;		// SNMP: SNMP_CFG_* bits (was reserved4; 0 = the defaults)
	// Appended for the UI's settings (docs/spec/ui.md 4). The radio core's DMR reply window belongs with the rs_
	// fields (taken only when rs_stored is 1); the others are the UI's
	uint8_t rs_reply_s;		// radio_settings_t.reply_s: 0-30 s, SETTINGS_UNSET8 = the codeplug's hang times
	uint8_t mic_gain;		// 0-10 (5 = as the board sets it); kept only: no board takes it yet
	uint16_t auto_lock_s;		// lock the keys after this long without a key press, 0 = never
	uint16_t long_press_ms;		// long press threshold for core/input, 0 = INPUT_LONG_MS
	uint16_t repeat_ms;		// key repeat period, 0 = INPUT_REPEAT_MS
	uint8_t gps;			// GPS on: Settings > Location > GPS powers the module (board_gps_enable) and lets APRS use its fix
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
	uint16_t aprsis_port;		// APRS-IS: the server's port, 0 = 14580 (was reserved5)
	// Roaming beacons and the survey (docs/spec/dmr-roaming.md 5.1; appended for roam2). Taken with the other roam_
	// fields when roam_stored is 1; a record without them keeps these defaults (60 s, 4 s, synchronized survey)
	uint16_t roam_beacon_s;		// the repeaters' beacon interval, 0 = no beacons, else 10..600
	uint8_t roam_beacon_len_s;	// their beacon duration, 1..10
	uint8_t roam_survey;		// 0 off, 1 the sites beacon together, 2 each site on its own schedule
	uint8_t rpt_beacon;		// SETTINGS_RPT_* bits: our repeater roles key an idle beacon (roam_beacon_s / _len_s);
					// bits 1-2 what an SFR user channel hears (radio_settings_t.sfr_rx; 0 in older records),
					// bits 3-5 the SFR repeater's CW ID interval (radio_settings_t.sfr_cwid; 0 off)
	uint8_t aprs_power;		// APRS RF power: 0 = the radio's current level, n = level n - 1 (was reserved6[0]; taken
					// with the aprs_ fields below)
	uint8_t aprs_cp_path;		// the codeplug APRS record's path last taken into aprs_path, aprs_path_t + 1 (0 = none
					// yet; was reserved6[1]). A different path in the codeplug replaces aprs_path (aprs.md 11)
	uint8_t volume;			// speaker volume on radios without a knob: 0 = the default, else 0x80 | (step + 1)
					// (step 0..RADIO_VOLUME_MAX, 0 = mute); 1..11 = a step 0..10 of the first version
					// (was reserved6, radio.h RADIO_VOLUME_*, radio_volume_step)
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
	uint16_t snmp_rf_gap_s;		// SNMP: at most one RF notification per this many seconds, 0 = 60 (was reserved7)
	// The data service's confirmed IP rules (dmrDataSettings_t.ipConfirmedRule, docs/spec/dmr-data.md 12; appended
	// for sap3), taken with the other ds_ fields: 0 unused, a radio ID, or 0x80000000 | a UDP port
	uint32_t ds_ip_rule[2];
	// The radio's own APRS-IS client on the USB network link (CONFIG_NET_APRSIS, docs/spec/tcp-services.md 7; appended
	// for tcp; its port and range are in reserved3 / reserved5 above)
	uint32_t aprsis_ip;		// server IPv4 address, 0 = off
	// Control station mode on the USB network link (docs/spec/usb-network.md 5; appended for dispatch): the radio's
	// address on a fixed link (the PC's is the next one), 0 = 192.168.10.1; on with net_flags SETTINGS_NET_FIXED_LINK
	uint32_t net_link_ip;
	// The SNMP agent's communities (core/snmp/snmpMib.h snmpConfig_t, docs/spec/snmp.md 6; appended for snmp; its other
	// settings are in snmp_rf_manager, snmp_flags and snmp_rf_gap_s above). 0 = the defaults
	uint32_t snmp_read_hash;	// read community (FNV-1a of the text), 0 = "public"
	uint32_t snmp_write_hash;	// write community, 0 = none (SET refused)

	// --- Page 1 (bytes 236 on): a save writes one record per SETTINGS_PAGE_BYTES (settings-store.md 2.1) ---
	// Dual watch (docs/ux/dual-watch.md): the last B, or while the watch runs the channel that isn't selected
	uint16_t dual_b;		// 0 none, 1..1024 a codeplug channel number, SETTINGS_DUAL_VFO + n VFO n
	uint8_t dual_state;		// SETTINGS_DUAL_* (0: off at power off)
	uint8_t vfo_index;		// the VFO in use, 0 or 1 (the second is dual watch's other VFO)
	// The second VFO (dual watch: each channel has its own), as the first; vfo2_rx_hz 0 = never set
	uint32_t vfo2_rx_hz;
	uint32_t vfo2_tx_hz;
	uint32_t vfo2_talk;		// the DMR talk target: ID in bits 0-23, bit 31 private call, 0 = none
	uint16_t vfo2_rx_tone;
	uint16_t vfo2_tx_tone;
	uint8_t vfo2_mode;		// radio_mode_t
	uint8_t vfo2_bw;		// radio_bw_t
	uint8_t vfo2_power;		// board power level, SETTINGS_UNSET8 = default
	uint8_t vfo2_cc;
	uint8_t vfo2_ts;
	uint8_t vfo2_rx_tone_type;	// tone_type_t
	uint8_t vfo2_tx_tone_type;
	uint8_t rs_dual_delay;		// radio_settings_t.dual_delay_ms: n - 1 steps of 100 ms, 0 = the default
	uint8_t rs_bcast_step;		// radio_settings_t.bcast_step + 1, 0 = the default (200 kHz on the odd tenths)
	uint8_t zs_offers;		// zone share offers: 0 Off (the default), 1 Ask (docs/spec/zone-share.md 5)
	int16_t ref_trim_hz;		// reference oscillator trim, Hz at the carrier, -10000..+10000 (radio/ref_trim.h; was
					// reserved8, so older records read 0 = none)
	// The IP gateway's off-net route (dmrDataSettings_t.ipGatewayId, docs/spec/usb-network.md 3.1; appended for
	// ip-addr), taken with the other ds_ fields: the gateway radio's DMR ID, 0 = off-net packets dropped
	uint32_t ds_ip_gateway;
	// A zone add or delete under way (core/codeplug/zone_settle.c): written before the codeplug's zone write, cleared
	// with zone_index and roam_zones moved to match. Found set at power on, it is finished from the zone list
	uint16_t zone_jrn_index;	// the index the zone was added at or deleted from
	uint16_t zone_jrn_count;	// the stored zone count before it (0: only the built-in zone)
	uint8_t zone_jrn_kind;		// SETTINGS_ZONE_JRN_*
	uint8_t reserved8b[3];
	// Zone share: the zones imported last, so the same zone sent again updates its import instead of adding a copy
	// (docs/spec/zone-share.md 5.4); key 0 = unused, the oldest replaced first
	settings_zs_import_t zs_import[SETTINGS_ZS_IMPORTS];
} settings_t;

// Pages: settings_t is stored as records of SETTINGS_PAGE_BYTES each (settings-store.md 2.1). Only the pages its size
// needs are written, each with its own length, so settings_t grows by appending fields; the record's page field
// allows 16 pages (3776 bytes)
#define SETTINGS_PAGE_BYTES	236u
#define SETTINGS_PAGES_MAX	16u
#define SETTINGS_PAGES		((sizeof(settings_t) + SETTINGS_PAGE_BYTES - 1u) / SETTINGS_PAGE_BYTES)
#define SETTINGS_PAGE_SHIFT	12	// a record's layout field: the page in bits 12-15, the layout in bits 0-11
#define SETTINGS_ZONE_JRN_NONE		0u	// zone_jrn_kind
#define SETTINGS_ZONE_JRN_ADD		1u
#define SETTINGS_ZONE_JRN_DELETE	2u
#define SETTINGS_DUAL_VFO	1025u	// dual_b: 1025 the first VFO, 1026 the second
#define SETTINGS_DUAL_ON	0x01u	// dual_state: it ran at power off, and comes back at power on
#define SETTINGS_DUAL_B_SEL	0x02u	// ... with B selected: dual_b is then A's (the radio powers up on B)

// The DMR VFO's talk target (docs/spec/settings-store.md 3): a 24-bit ID and a private call flag, packed into bytes
// that were reserved or held a small value, so the record stays 236 bytes and an older record reads as "none" (0).
// ID bits 0-7 vfo_talk_lo, 8-11 vfo_mode bits 4-7, 12-15 vfo_bw bits 4-7, 16-19 vfo_cc bits 4-7, 20-23 vfo_ts bits
// 2-5; vfo_ts bit 7 = private call. Use these accessors: vfo_mode, vfo_bw, vfo_cc and vfo_ts are not plain numbers
// any more
static inline uint8_t settings_vfo_mode(const settings_t *s) { return s->vfo_mode & 0x0Fu; }
static inline uint8_t settings_vfo_bw(const settings_t *s) { return s->vfo_bw & 0x0Fu; }
static inline uint8_t settings_vfo_cc(const settings_t *s) { return s->vfo_cc & 0x0Fu; }
static inline uint8_t settings_vfo_ts(const settings_t *s) { return (s->vfo_ts & 3u) == 2u ? 2u : 1u; }
static inline uint32_t settings_vfo_talk_id(const settings_t *s)
{
	return (uint32_t)s->vfo_talk_lo | ((uint32_t)(s->vfo_mode >> 4) << 8) | ((uint32_t)(s->vfo_bw >> 4) << 12) |
	       ((uint32_t)(s->vfo_cc >> 4) << 16) | ((uint32_t)((s->vfo_ts >> 2) & 0x0Fu) << 20);
}
static inline bool settings_vfo_talk_private(const settings_t *s) { return (s->vfo_ts & 0x80u) != 0; }
// Mode and bandwidth, color code and timeslot of the VFO, leaving the talk target; and the talk target, leaving those
static inline void settings_set_vfo_mode_bw(settings_t *s, uint8_t mode, uint8_t bw)
{
	s->vfo_mode = (uint8_t)((s->vfo_mode & 0xF0u) | (mode & 0x0Fu));
	s->vfo_bw = (uint8_t)((s->vfo_bw & 0xF0u) | (bw & 0x0Fu));
}
static inline void settings_set_vfo_cc_ts(settings_t *s, uint8_t cc, uint8_t ts)
{
	s->vfo_cc = (uint8_t)((s->vfo_cc & 0xF0u) | (cc & 0x0Fu));
	s->vfo_ts = (uint8_t)((s->vfo_ts & 0xFCu) | (ts == 2 ? 2u : 1u));
}
static inline void settings_set_vfo_talk(settings_t *s, uint32_t id, bool private_call)
{
	s->vfo_talk_lo = (uint8_t)id;
	s->vfo_mode = (uint8_t)((s->vfo_mode & 0x0Fu) | (((id >> 8) & 0x0Fu) << 4));
	s->vfo_bw = (uint8_t)((s->vfo_bw & 0x0Fu) | (((id >> 12) & 0x0Fu) << 4));
	s->vfo_cc = (uint8_t)((s->vfo_cc & 0x0Fu) | (((id >> 16) & 0x0Fu) << 4));
	s->vfo_ts = (uint8_t)((s->vfo_ts & 0x03u) | (((id >> 20) & 0x0Fu) << 2) | (private_call ? 0x80u : 0));
}

// settings_t.accent: bits 0-2 the highlight color (0 the theme's own, 1 red, 2 orange, 3 yellow, 4 teal, 5 light blue,
// 6 purple, 7 pink), bit 3 set: the status strip on the screen's background, not filled (0, the default: filled),
// bits 4-5 the crash report's mode (below), bits 6-7 the vocoder's mic options (SETTINGS_AUDIO_* below)
#define SETTINGS_ACCENT_COLOR	0x07u
#define SETTINGS_ACCENT_PLAIN	0x08u

// theme and updown_home hold 0..2 in bits 0-1; their upper bits hold the crash report's target (below)
#define SETTINGS_SMALL_MASK	0x03u
static inline uint8_t settings_theme(const settings_t *s) { return (s->theme & 3u) <= 2u ? s->theme & 3u : 0; }
static inline uint8_t settings_updown_home(const settings_t *s)
{
	return (s->updown_home & 3u) <= 2u ? s->updown_home & 3u : 0;
}

// The crash report (docs/spec/crash-log.md 5.2), packed into spare bits so the record stays 236 bytes and an older
// record reads as off with no target: the mode (0 off, 1 ask, 2 send at once) in accent bits 4-5; the target's
// codeplug contact number (0 none, else 1..CODEPLUG_CONTACTS_MAX, 11 bits) bits 0-5 in theme bits 2-7 and bits 6-10
// in updown_home bits 2-6. Firmware from before reads theme and updown_home with these bits as out of range: dark
// and Up/Down = channel
static inline uint8_t settings_crash_mode(const settings_t *s) { return (s->accent >> 4) & 3u; }
static inline uint16_t settings_crash_contact(const settings_t *s)
{
	return (uint16_t)((s->theme >> 2) | (((s->updown_home >> 2) & 0x1Fu) << 6));
}
static inline void settings_set_crash_mode(settings_t *s, uint8_t mode)
{
	s->accent = (uint8_t)((s->accent & 0xCFu) | ((mode & 3u) << 4));
}
static inline void settings_set_crash_contact(settings_t *s, uint16_t num)
{
	s->theme = (uint8_t)((s->theme & 3u) | ((num & 0x3Fu) << 2));
	s->updown_home = (uint8_t)((s->updown_home & 0x83u) | (((num >> 6) & 0x1Fu) << 2));
}

// settings_t.ui_flags (defaults: hints, message tone, talker alias, PTT when locked and vibrate on)
#define SETTINGS_UI_LARGE_TEXT		0x01u
#define SETTINGS_UI_HINTS		0x02u
#define SETTINGS_UI_MESSAGE_TONE	0x04u
#define SETTINGS_UI_ALIAS		0x08u	// show received talker aliases
#define SETTINGS_UI_PTT_LOCKED		0x10u	// PTT works while the keys are locked
#define SETTINGS_UI_HEAR_ALL		0x20u	// hear all on whenever a DMR channel is selected
#define SETTINGS_UI_NO_VIBRATE		0x40u	// vibrate for alerts off (0 = on: older records keep it on)
#define SETTINGS_UI_DUAL_TALK_BACK	0x80u	// dual watch: after a call on the other channel, PTT talks there a while

// The vocoder's mic options in settings_t.accent bits 6-7: no byte was left for them, and a record from before reads
// as 0, noise reduction on and AGC off
#define SETTINGS_AUDIO_NO_NOISE_REDUCTION	0x40u	// the vocoder's mic noise reduction off (DMR TX)
#define SETTINGS_AUDIO_AGC			0x80u	// the vocoder's mic AGC on (DMR TX; mic gain then step 5)
#define SETTINGS_AUDIO_MASK			0xC0u
#define SETTINGS_AUDIO_SHIFT			6

// settings_t.net_flags (the USB network personality; 0 = as before)
#define SETTINGS_NET_TFTP_NO_WRITE	0x01u	// TFTP refuses codeplug writes (0: allowed, as the CPS over USB; docs/spec/tftp.md)
#define SETTINGS_NET_PASSTHROUGH	0x02u	// the PC is the endpoint of the radio's own IP data (docs/spec/usb-network.md 5.2)
#define SETTINGS_NET_FIXED_LINK		0x04u	// the link is a fixed private pair (net_link_ip), not 12.<ID>/32 (5.3)
// settings_t.rs_flags
#define SETTINGS_RS_ROGER_BEEP		0x01u
#define SETTINGS_RS_TX_INHIBIT		0x02u
#define SETTINGS_RS_SCAN_PRIORITY	0x04u
#define SETTINGS_RS_SCAN_BEEP		0x08u
#define SETTINGS_RS_KEY_BEEP		0x10u
#define SETTINGS_RS_NO_TALK_PERMIT	0x20u	// the talk permit tone off (0 = on, as before)
// settings_t.rpt_beacon (taken with the roam_ fields)
#define SETTINGS_RPT_BEACON		0x01u	// our repeater roles key an idle beacon
#define SETTINGS_RPT_SFR_RX		0x06u	// an SFR user channel hears: 0 the repeat, 1 both (first), 2 both (dual-path RX)
#define SETTINGS_RPT_SFR_RX_SHIFT	1
#define SETTINGS_RPT_SFR_CWID		0x38u	// the SFR repeater's CW ID interval code (radio_cwid_interval_ms), 0 off
#define SETTINGS_RPT_SFR_CWID_SHIFT	3

// settings_t.ds_flags
#define SETTINGS_DS_RADIO_CHECK_REPLY	0x01u
#define SETTINGS_DS_CALL_ALERT_ACK	0x02u
#define SETTINGS_DS_LRRP_REPLY		0x04u
#define SETTINGS_DS_SMS_ETSI		0x08u	// sms_send's default format is ETSI defined short data
#define SETTINGS_DS_RADIO_CHECK_ETSI	0x10u	// our radio checks in the ETSI form (FID 0x00)
#define SETTINGS_DS_UDPC_OFF		0x20u	// compressed UDP/IPv4 headers off (neither bit: auto)
#define SETTINGS_DS_UDPC_ALL		0x40u	// ... for every UDP packet, the DMR application ports too
// settings_t.ds_ip_cnf bits 4-5 (0: the confirmed rules, the default)
#define SETTINGS_DS_IPC_SHIFT		4
#define SETTINGS_DS_IPC_OFF		1u
#define SETTINGS_DS_IPC_ALL		2u
#define SETTINGS_DS_SMS_ETSI_IP		0x80u	// sms_send's default format is ETSI IP text (neither ETSI bit: Motorola TMS)

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
	SETTINGS_UPGRADED,	// the newest record, written by a firmware with fewer or more fields or pages
	SETTINGS_FOREIGN,	// the settings area holds another firmware's data: defaults, nothing saved until claimed
} settings_load_t;

typedef struct {
	uint8_t load;		// settings_load_t
	bool dirty;		// changes not saved yet
	uint32_t seq;		// sequence number of the newest page 0 record, the set in force (0 = none)
	uint32_t saves;		// records written since boot
	uint32_t erases;	// sectors erased since boot
	uint32_t failures;	// saves that failed
	uint16_t sector;	// ring sector holding the newest record (of any page: where the next save follows)
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
