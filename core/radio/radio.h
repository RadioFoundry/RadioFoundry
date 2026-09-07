// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core: owns the channel / VFO state, squelch, PTT and the TX timer, and drives board.radio.
// It runs in its own task. The UI reads a snapshot and posts requests; it never touches the hardware.
#ifndef RADIO_CORE_H
#define RADIO_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "config.h"

typedef enum {
	RS_IDLE,	// receiving, squelch closed
	RS_RX,		// receiving, squelch open (carrier and tone)
	RS_TX,
	RS_TX_DENIED,	// PTT held but transmit refused (RX-only, out of band, timed out); shown until PTT release
} radio_state_t;

// TXDENY_MODE: the channel's mode is not built in (a DMR channel in an FM-only build, or the reverse).
// DMR: NO_CODEC no vocoder merged (receive only), NO_DMR the modem didn't start, BUSY a call on our slot (polite
// access), NO_REPEATER the repeater didn't wake, REJECTED the modem refused, HOTSPOT the radio is a hotspot now,
// NO_ID no DMR ID in the codeplug. BUSY on FM: busy channel lockout. INHIBIT: transmit is switched off in the
// settings. BATTERY: the battery is critical
typedef enum {
	TXDENY_NONE, TXDENY_RX_ONLY, TXDENY_BAND, TXDENY_TIMEOUT, TXDENY_NO_FREQ, TXDENY_MODE,
	TXDENY_NO_CODEC, TXDENY_NO_DMR, TXDENY_BUSY, TXDENY_NO_REPEATER, TXDENY_REJECTED, TXDENY_HOTSPOT, TXDENY_NO_ID,
	TXDENY_INHIBIT, TXDENY_BATTERY,
} tx_deny_t;

// Busy channel lockout (FM): CARRIER refuses PTT while anything is heard on the channel; WRONG_TONE only while a
// carrier without our RX tone is heard (the channel is used by another group)
typedef enum { RADIO_BCL_OFF, RADIO_BCL_CARRIER, RADIO_BCL_WRONG_TONE } radio_bcl_t;
// VOX: off, on channels whose codeplug entry allows it, on every FM channel
typedef enum { RADIO_VOX_OFF, RADIO_VOX_CHANNEL, RADIO_VOX_ALL } radio_vox_t;

// The radio core's settings (docs/spec/radio-features.md). The radio core loads them from the settings store at
// start-up (docs/spec/settings-store.md 3) and saves every change made with radio_set_settings
typedef struct {
	uint16_t tot_s;		// the radio's TX time-out, for channels whose own is 0; 0 = none
	uint8_t tot_warn_s;	// warning beep this long before the time-out, 0 = none
	uint8_t bcl;		// radio_bcl_t
	bool roger_beep;	// FM: a short tone pair at the end of each transmission (CONFIG_FM_TX_TONES)
	bool tx_inhibit;	// refuse every transmission
	// Scan (CONFIG_SCAN)
	uint8_t scan_resume;	// scan_resume_t (radio/scan.h): 0 time, 1 carrier, 2 stop
	uint8_t scan_hold_s;	// time mode: move on after this long on a signal
	uint16_t scan_delay_ms;	// after the signal ends, move on after this long
	uint16_t scan_dwell_ms;	// listen time per channel with nothing heard
	bool scan_priority;	// zone scan: revisit the start channel between the others
	bool scan_beep;		// a short beep when the scan stops on a signal
	// Beeps (CONFIG_BEEP)
	uint8_t beep_level;	// 0 = silent .. 100, relative to the volume knob
	bool key_beep;		// radio_req_beep(BEEP_KEY) sounds
	bool talk_permit;	// DMR: the talk permit tone when the voice goes on air (channels without no_beep)
	// VOX (CONFIG_VOX)
	uint8_t vox;		// radio_vox_t
	uint8_t vox_level;	// sensitivity 1 (needs a loud voice) .. 10
	uint16_t vox_tail_ms;	// stays keyed this long after the voice stops
	// Power
	uint8_t rx_save;	// receiver battery saving (CONFIG_BATTERY_SAVE): 0 off, 1..3 = sleep 1, 2, 4 x the listen time
	uint16_t rx_save_idle_s;	// starts after this long without activity
	uint16_t battery_off_s;	// switch off after this long with the battery critical, 0 = never
	uint16_t backlight_dim_s;	// for batt_backlight_pct (radio/battery.h): dim after, 0 = never
	uint16_t backlight_off_s;	// off after, 0 = never
	// DMR (CONFIG_DMR)
	uint8_t reply_s;	// reply window after a call, 0 = off .. 30 s, RADIO_REPLY_CODEPLUG = the codeplug's hang times
	// DMR repeater roaming (CONFIG_DMR_ROAMING; docs/spec/dmr-roaming.md). Which zones roam is kept apart (RREQ_ROAM)
	int8_t roam_rssi_dbm;	// threshold: the site counts as weak below it, a candidate must reach it (-125 .. -60)
	uint8_t roam_hyst_db;	// a candidate must beat the current site by this much (1 .. 30)
	uint8_t roam_probe;	// 0 passive, 1 wake probe (a BS_Dwn_Act on each silent site visited: transmits)
	uint16_t roam_check_s;	// periodic search: look at the other sites this often, 0 = off (30 .. 3600)
	uint16_t roam_beacon_s;	// the repeaters' idle beacon interval, 0 = none (10 .. 600); must match the repeaters
	uint8_t roam_beacon_len_s;	// their beacon duration (1 .. 10)
	uint8_t roam_survey;	// 0 off, 1 the sites beacon together (one window), 2 each on its own schedule
	bool rpt_beacon;	// our repeater roles (SFR) key an idle beacon on that interval and duration
} radio_settings_t;

#define RADIO_REPLY_CODEPLUG	0xFFu	// radio_settings_t.reply_s: the codeplug's hang times (else the stack's 10 s)

typedef enum { RADIO_BATT_OK, RADIO_BATT_LOW, RADIO_BATT_CRITICAL } radio_batt_t;	// = batt_level_t

#if CONFIG_DMR
#define RADIO_ALIAS_MAX		31	// talker alias characters (DMR_TA_TEXT_MAX)

// What the DMR side is doing, for the screens
typedef enum {
	RDMR_IDLE,		// listening
	RDMR_RX,		// playing a call that passed the filters
	RDMR_RX_OTHER,		// a call on our slot we don't play (other talkgroup, encrypted)
	RDMR_TX_WAIT,		// PTT held: waiting for the channel or waking the repeater
	RDMR_TX,		// on air
	RDMR_TX_ENDING,		// PTT released: finishing the superframe
	RDMR_TX_DATA,		// sending data / a CSBK for the data service (SMS, ACK, call alert, ...)
} radio_dmr_state_t;

typedef struct {
	uint32_t src, dst;
	bool group;		// group call, else private
	bool to_us;		// private call to our ID
	bool encrypted;		// privacy service option, PI or a PI header
	bool emergency;		// the emergency service option
	bool pi_header;		// a PI header was heard (an MI-based scheme: we can't decrypt it)
	char src_name[CODEPLUG_NAME_LEN + 1];	// codeplug contact, else the DMR ID database's text cut short, "" if unknown
	char dst_name[CODEPLUG_NAME_LEN + 1];	// talkgroup name, "" if unknown
} radio_dmr_call_t;

typedef struct {
	bool active;		// the call stack runs (a DMR channel, not hotspot mode)
	bool codec;		// the vocoder is merged: voice plays and PTT works
	uint8_t state;		// radio_dmr_state_t
	uint32_t own_id;
	uint8_t cc, ts;
	bool rmo;		// through a repeater
	bool repeater_up;
	// Where PTT goes now (the reply target during the reply window)
	uint32_t target_id;
	bool target_private;
	char target_name[CODEPLUG_NAME_LEN + 1];
	bool reply_active;
	uint8_t reply_s;	// reply window seconds left
	bool target_manual;	// a talk target chosen for now (contact, typed ID, call back), until the channel changes
	bool privacy;		// a privacy key is loaded: encrypted calls play, our calls go out encrypted
	bool alias_tx;		// our talker alias goes out on this channel
	bool hear_all;		// every talkgroup on our slot: MONITOR held, or the hear all latch
	bool hear_all_latch;	// hear all latched (radio_req_dmr_hear_all), until the channel changes
	uint32_t focus_id;	// the focused row's target (radio_req_dmr_focus), 0 = none: PTT talks to it once
	bool focus_private;
	bool one_shot;		// PTT went to the focused target: it stays the talk target for the reply window
	// The current call (RX or TX); valid while state != RDMR_IDLE
	radio_dmr_call_t call;
	bool late_entry;
	char alias[RADIO_ALIAS_MAX + 1];	// talker alias so far (CONFIG_DMR_TALKER_ALIAS), "" if none
	int16_t rssi_dbm;	// last burst of the call, 0 = unknown
	uint32_t call_ms;	// duration of the current call
	// The last call heard
	radio_dmr_call_t last;
	uint32_t last_ms;	// hal_millis() of its last burst, 0 = none yet
	uint32_t underruns;	// TX voice frames sent as silence (diagnostics)
} radio_dmr_view_t;
#endif

#if CONFIG_DMR_ROAMING
#define RADIO_ROAM_SITES	16	// = ROAM_SITES_MAX (radio/roam.h)

// Why roaming can't start a search now (docs/spec/dmr-roaming.md 4.3)
typedef enum {
	RROAM_BLOCK_NONE, RROAM_BLOCK_MODE, RROAM_BLOCK_HOTSPOT, RROAM_BLOCK_SCAN, RROAM_BLOCK_TX, RROAM_BLOCK_CALL,
	RROAM_BLOCK_REPLY, RROAM_BLOCK_QUIET, RROAM_BLOCK_OVERRIDE, RROAM_BLOCK_EMERGENCY,
} radio_roam_block_t;

// radio_roam_view_t.state (= roam_state_t in radio/roam.h)
enum { RROAM_STATE_OFF, RROAM_STATE_WATCH, RROAM_STATE_SEARCH, RROAM_STATE_LOCKED, RROAM_STATE_SURVEY };

// Roaming for the screens and the debug link
typedef struct {
	bool zone_on;		// the current zone is a roaming zone
	uint8_t state;		// roam_state_t: 0 off (not active here), 1 watch, 2 search, 3 locked
	uint8_t why;		// roam_why_t of the running or last search (0 none, 1 weak, 2 tx_fail, 3 manual, 4 check, 5 stale)
	uint8_t pending;	// a latched trigger waiting (roam_why_t)
	uint8_t blocked;	// radio_roam_block_t now
	uint8_t sites;		// sites in the zone (0 when roaming is off here)
	uint8_t site;		// the site the radio is on (index)
	uint8_t home;		// the site the user picked
	int16_t rssi_dbm;	// the current site's average, 0 = unknown
	bool weak;		// that average is below the threshold
	uint32_t searches, roams, stays, aborts, probes, triggers;
	uint16_t site_number[RADIO_ROAM_SITES];	// codeplug channel numbers
	int16_t site_rssi[RADIO_ROAM_SITES];	// last measurement (the current site: its average), 0 = none
	// The survey (docs/spec/dmr-roaming.md 4.8 - 4.10)
	uint8_t visit;		// the site a survey visit or search is on, 0xFF = none
	uint8_t win_conf;	// the shared beacon window: beacons that matched it (2+ = known)
	uint32_t win_next_ms;	// ms until the next window, 0 = not known
	uint8_t confirm;	// a confirm visit planned to this site + 1, 0 = none
	uint32_t surveys, visits, cuts, beacons, confirms;
	int16_t site_surv[RADIO_ROAM_SITES];	// the survey's average, 0 = none / not heard
	uint16_t site_age_s[RADIO_ROAM_SITES];	// s since the survey measured it, 0xFFFF = never
	uint32_t site_phase_ms[RADIO_ROAM_SITES];	// its last beacon's start in the interval (ms), UINT32_MAX = unknown
	uint16_t site_fresh16;	// bit i: site i's survey data is fresh
} radio_roam_view_t;
#endif

typedef struct {
	radio_state_t state;
	uint8_t tx_deny;	// tx_deny_t, while RS_TX_DENIED
	bool vfo;		// tuning a frequency by hand rather than a channel
	uint16_t zone_index;
	uint16_t zone_pos;	// position in the zone, 0-based
	uint16_t zone_size;
	uint16_t channel_number;	// codeplug channel number, 0 in VFO
	char zone_name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;		// what the radio is set to now (VFO: the VFO settings, with ch.name "VFO")
	uint8_t power;		// effective board power level index
	bool temporary;		// a temporary change (P7) is in effect on this channel
	uint8_t temp;		// which settings are temporary changes: RADIO_TEMP_* bits (temporary == (temp != 0))
	uint8_t squelch;	// 0 = open .. 9 = tight
	bool monitor;		// squelch held open by the user
	bool tone_ok;		// the RX tone or code is heard (debounced; true when the channel has no RX tone)
	int16_t rssi_dbm;
	uint8_t noise;
	uint32_t tx_elapsed_ms;	// time in TX, while RS_TX
	uint16_t tot_s;		// TX time-out, 0 = none
	uint32_t last_rx_ms;	// hal_millis() at the end of the last reception, 0 = none yet
	uint32_t vfo_step_hz;
	codeplug_source_t source;
	bool hotspot;		// the radio is an MMDVM hotspot: the host link drives the modem, channel keys are off
	// Scan (CONFIG_SCAN; all 0 otherwise)
	uint8_t scan;		// scan_kind_t: 0 off, 1 zone, 2 VFO, 3 dual watch
	uint8_t scan_phase;	// scan_phase_t: listen, probe, hold, resume, paused
	uint8_t scan_skipped;	// nuisance-deleted stops
	uint16_t scan_probes;	// this scan: stops probed (a carrier at the end of the dwell)
	uint16_t scan_synced;	// ... stops where sync was heard (DMR: bursts with the channel's color code)
	// FM extras
	bool tot_warning;	// TX: the time-out is near (tot_warn_s)
	bool tone_burst;	// TX: the 1750 Hz burst is on
	bool roger;		// TX: the roger beep is being sent after the PTT release
	bool vox_keyed;		// TX keyed by VOX
	bool vox_on;		// VOX listens on this channel (the settings' vox, the channel, an FM channel)
	bool rx_sleeping;	// battery save: the receiver is off for now
	// Battery
	uint8_t battery;	// radio_batt_t
	uint8_t battery_pct;	// board.power->battery_pct(), 0 if unknown
	uint16_t battery_mv;
	bool power_off;		// the radio core asks to be switched off (critical battery, battery_off_s)
#if CONFIG_DMR
	radio_dmr_view_t dmr;
#endif
#if CONFIG_DMR_ROAMING
	radio_roam_view_t roam;
#endif
} radio_view_t;

// view.temp bits
#define RADIO_TEMP_POWER	0x01u
#define RADIO_TEMP_SQUELCH	0x02u
#define RADIO_TEMP_BW		0x04u
#define RADIO_TEMP_TX_TONE	0x08u
#define RADIO_TEMP_RX_TONE	0x10u
#define RADIO_TEMP_TS		0x20u	// DMR timeslot (radio_req_dmr_timeslot)
#define RADIO_TEMP_CC		0x40u	// DMR color code (radio_req_dmr_color_code)
#define RADIO_TEMP_MODE		0x80u	// FM <-> DMR (radio_req_mode)

// Which tone radio_req_tone sets
#define RADIO_TONE_TX		0x01u
#define RADIO_TONE_RX		0x02u
#define RADIO_TONE_BOTH		(RADIO_TONE_TX | RADIO_TONE_RX)

void radio_core_start(void);
// A consistent copy of the current state
void radio_get_view(radio_view_t *v);
// Bumped on every visible change, so the UI can redraw only when needed
uint32_t radio_view_serial(void);

// Requests (queued, handled by the radio task)
void radio_req_channel_step(int8_t dir);	// next / previous channel in the zone, wraps
void radio_req_zone_pos(uint16_t pos);		// jump to a position in the zone
void radio_req_zone(uint16_t index);
void radio_req_vfo(bool on);
void radio_req_vfo_step(int8_t dir);
void radio_req_vfo_set(uint32_t hz);		// rx and tx frequency (simplex)
void radio_req_ptt(bool down);
void radio_req_monitor(bool down);
void radio_req_power(int8_t dir);		// temporary change of power level, wraps
void radio_req_squelch(int8_t dir);
void radio_req_clear_temporary(void);		// BACK on Home: revert temporary changes
void radio_req_reload(void);			// the settings' preferences changed (squelch, master power, DMR ID)
// FM bandwidth and sub-audio. On a channel these are temporary changes (P7); in the VFO they change the VFO itself.
// Refused on DMR channels, while transmitting, and for tones tones_valid() rejects
void radio_req_bandwidth(radio_bw_t bw);
void radio_req_tone(uint8_t which, subtone_t tone);	// which: RADIO_TONE_TX, RADIO_TONE_RX or RADIO_TONE_BOTH
#if CONFIG_DMR
void radio_req_reply_cancel(void);		// BACK on Home: end the DMR reply window
// Talk to someone else for now (until the channel changes or radio_req_dmr_target_clear): a typed ID or talkgroup,
// a codeplug contact, or back to whoever was heard last (their ID for a private call, the talkgroup for a group call)
void radio_req_dmr_target(uint32_t id, bool private_call);
void radio_req_dmr_contact(uint16_t index);
void radio_req_dmr_call_back(void);
void radio_req_dmr_target_clear(void);
// On-the-fly channel changes (temporary, P7; undone by radio_req_clear_temporary, kept by a save to the channel).
// Refused while transmitting, in the VFO and in hotspot mode. Timeslot and color code only on a DMR channel; the
// timeslot beats a contact's timeslot override. The mode change retunes (the call stack starts or stops)
void radio_req_dmr_timeslot(uint8_t ts);	// 1 or 2
void radio_req_dmr_color_code(uint8_t cc);	// 0..15
void radio_req_mode(uint8_t mode);		// radio_mode_t: FM or DMR on the current channel
// Hear all latched on (every talkgroup on our slot, as while MONITOR is held) until off or the channel changes
void radio_req_dmr_hear_all(bool on);
// The talk target focused in a list (the UI calls it on every focus change; id 0 = none). A PTT press while one is
// focused transmits to it once (one-shot) and keeps it as the talk target for the reply window, then the channel's
// (or the chosen) target is back. BACK on Home (radio_req_reply_cancel) ends it at once
void radio_req_dmr_focus(uint32_t id, bool private_call);
#if CONFIG_DMR_PRIVACY
// Load a basic privacy key (type dmrPrivacyType_t, 0 = off). Held in RAM until changed or the radio restarts
void radio_req_dmr_privacy(uint8_t type, uint16_t key);
#endif
#endif
// Scan (CONFIG_SCAN; no-ops otherwise). A zone scan starts from the current channel, a VFO scan steps the VFO by its
// step between lo_hz and hi_hz (both 0: the band the VFO is in), dual watch alternates between the current channel
// and zone position pos. Stop stays where the scan is, cancel goes back to where it started. Nuisance leaves the
// current stop out until the scan ends and moves on. Any channel, zone or VFO change, PTT, or hotspot mode stops it
typedef enum { RSCAN_STOP, RSCAN_ZONE, RSCAN_VFO, RSCAN_DUAL, RSCAN_CANCEL, RSCAN_NUISANCE, RSCAN_TOGGLE } radio_scan_cmd_t;
void radio_req_scan(uint8_t cmd);		// radio_scan_cmd_t; TOGGLE: stop if scanning, else zone (or VFO in the VFO)
void radio_req_scan_vfo(uint32_t lo_hz, uint32_t hi_hz);
void radio_req_scan_dual(uint16_t pos);
// DMR repeater roaming (CONFIG_DMR_ROAMING; no-op otherwise; docs/spec/dmr-roaming.md 6.1): the current zone's
// roaming flag (stored), roam now (a search at once, clears a lock), lock to the current site (RAM only)
typedef enum {
	RROAM_ZONE_OFF, RROAM_ZONE_ON, RROAM_ZONE_TOGGLE, RROAM_NOW, RROAM_LOCK, RROAM_UNLOCK, RROAM_LOCK_TOGGLE,
} radio_roam_cmd_t;
void radio_req_roam(uint8_t cmd);
// A beep (radio/beep.h beep_t) at the settings' level: key beeps only when key_beep is set (CONFIG_BEEP)
void radio_req_beep(uint8_t beep);
// FM 1750 Hz tone burst while on (CONFIG_FM_TX_TONES): keys up if not transmitting, stops when off
void radio_req_tone_burst(bool on);
// Settings: radio_settings_default() fills the built-in defaults; set copies them in and applies them
void radio_settings_default(radio_settings_t *s);
void radio_set_settings(const radio_settings_t *s);
void radio_get_settings(radio_settings_t *s);
// view.power_off without copying the view (the housekeeping task polls it)
bool radio_power_off_wanted(void);
// Switching off: the radio core sends what has to go out first (the ARS deregistration). radio_power_off_busy() is
// true until that is done; the caller gives up after a few seconds (gotcha 7.4)
void radio_req_power_off(void);
bool radio_power_off_busy(void);
// The radio is still running after a power off (the knob holds the supply): back to normal operation
void radio_req_power_off_cancel(void);
// The backlight percent for the UI: batt_backlight_pct with the settings and the current battery level
uint8_t radio_backlight_pct(uint8_t brightness, uint32_t idle_ms);
#if CONFIG_HOTSPOT
// Bumped when the hotspot engine reports a change (hotspot_radio_ops_t.changed), for the UI's hotspot screen
uint32_t radio_hotspot_serial(void);
#endif

#endif
