// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine, without the RTOS: it takes requests and the time, polls board.radio and keeps
// the view. radio_task.c runs it in a task; the host tests run it against a fake board.
#ifndef RADIO_SM_H
#define RADIO_SM_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"
#include "radio/scan.h"
#include "radio/battery.h"
#include "radio/morse.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif
#if CONFIG_DMR_ROAMING
#include "radio/roam.h"
#include "settings/settings.h"	// SETTINGS_ROAM_ZONE_BYTES
#endif

#define RADIO_POLL_MS		25	// status poll and squelch period
#define RADIO_TICK_MS		10	// DMR call stack poll (radio_sm_tick)
#define RADIO_TOT_DEFAULT_S	180	// TX time-out
#define RADIO_SQUELCH_DEFAULT	5
#define RADIO_SQUELCH_MAX	9
#define RADIO_VFO_STEP_HZ	12500

// Software squelch on the chip's noise reading (1 dB units, larger = more noise). Level 0 is always open.
// Level 1..9 opens when noise < RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP, so 1 = 64 (loose) .. 9 = 40 (tight),
// and closes once noise rises RADIO_SQL_HYST_DB above that for RADIO_SQL_CLOSE_POLLS polls in a row.
#define RADIO_SQL_OPEN_L1	64
#define RADIO_SQL_DB_PER_STEP	3
#define RADIO_SQL_HYST_DB	3
#define RADIO_SQL_CLOSE_POLLS	2
// While APRS receive listens (aprs.md 5.3): a packet's tones at full deviation push the noise reading over the line
// for a poll or two, and on the DM-1701 a closed squelch zeroes the receive audio, so a packet came apart (on air,
// 2026-10-13). On the APRS frequency the squelch holds this many polls (300 ms) instead
#define RADIO_SQL_CLOSE_POLLS_APRS	12

// RX tone debounce: the AT1846S tone flag can drop out for a poll or two on a weak or fluttering signal. The tone
// counts as heard after RADIO_TONE_ON_POLLS matching polls in a row and as gone after RADIO_TONE_OFF_POLLS missing
// polls in a row (the chip's own detector already takes some 100-250 ms to report a match)
#define RADIO_TONE_ON_POLLS	2
#define RADIO_TONE_OFF_POLLS	8

// RSSI moves smaller than this don't bump the view serial (the snapshot still carries the latest value)
#define RADIO_RSSI_DEADBAND_DB	2

// DMR is TDMA: a call puts RF on one 30 ms slot of each 60 ms frame, so an instantaneous reading lands in the empty
// slot about half the time and shows the noise floor. While the DMR path is running the published RSSI is the peak of
// the last RADIO_RSSI_HOLD_MS: two full frames, so one lost sample or a late poll still leaves a burst in the window.
// RADIO_POLL_MS (25) is shorter than a slot (30), so every slot gets at least one sample.
#define RADIO_RSSI_HOLD_MS	120
#define RADIO_RSSI_HOLD_POLLS	((RADIO_RSSI_HOLD_MS + RADIO_POLL_MS - 1) / RADIO_POLL_MS)

// Scan defaults and per-stop timing (docs/spec/radio-features.md 2)
#define RADIO_SCAN_DWELL_MS	150	// listen per channel (from the retune) with nothing heard
#define RADIO_SCAN_FM_PROBE_MS	350	// FM: a carrier without the RX tone yet (CTCSS detection takes 100-250 ms)
#define RADIO_SCAN_DMR_PROBE_MS	700	// DMR: a carrier, waiting for bursts with the channel's color code (they come
					// within a superframe of the modem's lock)
#define RADIO_SCAN_DMR_SYNC_MS	1500	// DMR: such bursts heard, waiting for the call to play (a late entry takes its
					// LC from the second complete superframe at worst: about 1.1 s from the retune)
#define RADIO_SCAN_DMR_CARRIER_MS 150	// DMR: a carrier this recent still counts (direct mode: 30 ms on air in 60)
#define RADIO_SCAN_DMR_SYNC_AGE_MS 400	// DMR: bursts this recent are sync (a superframe and a slot)
#define RADIO_SCAN_HOLD_S	5
#define RADIO_SCAN_DELAY_MS	2000

// Battery monitor: one reading a second, none while transmitting and for a while after (the load sags the voltage)
#define RADIO_BATT_PERIOD_MS	1000
#define RADIO_BATT_TX_QUIET_MS	3000

// FM transmit tones
#define RADIO_BURST_HZ		1750
#define RADIO_ROGER_HZ1		1200
#define RADIO_ROGER_HZ2		900
#define RADIO_ROGER_STEP_MS	100	// each of the two roger tones
// The DCS turn-off tail after PTT release (hal_radio_t.tx_tail; radio-features.md)
#define RADIO_SUBTONE_TAIL_MS	300

// VOX: the mic level counts as voice at or above 100 - 8 x sensitivity; keyed after RADIO_VOX_ATTACK_POLLS polls of
// voice, never while received audio plays or for RADIO_VOX_RX_GUARD_MS after (the speaker would key it)
#define RADIO_VOX_ATTACK_POLLS	2
#define RADIO_VOX_RX_GUARD_MS	500

// Receiver battery saving: awake RADIO_SAVE_WAKE_MS (the first RADIO_SAVE_SETTLE_MS of it ignored), asleep
// RADIO_SAVE_SLEEP_MS x 1, 2 or 4
#define RADIO_SAVE_WAKE_MS	100
#define RADIO_SAVE_SETTLE_MS	50
#define RADIO_SAVE_SLEEP_MS	200

typedef enum {
	RREQ_CH_STEP,		// arg: direction
	RREQ_ZONE_POS,		// arg: position in the zone
	RREQ_ZONE,		// arg: zone index
	RREQ_VFO,		// arg: 1 on, 0 off
	RREQ_VFO_STEP,		// arg: direction
	RREQ_VFO_SET,		// arg: Hz
	RREQ_PTT,		// arg: 1 down, 2 down with a host audio session's audio (radio_req_ptt_host), 0 up
	RREQ_MONITOR,		// arg: 1 down, 0 up
	RREQ_POWER,		// arg: direction
	RREQ_SQUELCH,		// arg: direction
	RREQ_CLEAR_TEMP,
	RREQ_BANDWIDTH,		// arg: radio_bw_t
	RREQ_TONE,		// arg: radio_tone_arg()
	RREQ_REPLY_CANCEL,	// DMR: end the reply window
	RREQ_DMR_TARGET,	// DMR: arg = ID (bits 23:0) | private (bit 24)
	RREQ_DMR_CONTACT,	// DMR: arg = contact index
	RREQ_DMR_CALL_BACK,	// DMR: talk to the last call heard
	RREQ_DMR_TARGET_CLEAR,	// DMR: back to the channel's talk target
	RREQ_DMR_PRIVACY,	// DMR: arg = type << 16 | key
	RREQ_HS_ENTER,		// hotspot: arg = RX Hz (0 = stay)
	RREQ_HS_FREQ,		// hotspot: arg = RX Hz, arg2 = TX Hz
	RREQ_HS_POWER,		// hotspot: arg = MMDVM RF level 0-255
	RREQ_HS_LEAVE,		// hotspot: back to the channel
	RREQ_SCAN,		// arg: radio_scan_cmd_t, arg2: dual watch position
	RREQ_SCAN_VFO,		// arg: low Hz, arg2: high Hz (both 0: the VFO's band)
	RREQ_BEEP,		// arg: beep_t
	RREQ_TONE_BURST,	// arg: 1 on, 0 off
	RREQ_SETTINGS,		// radio_task.c: the pending settings were copied with radio_sm_set_settings
	RREQ_POWER_OFF,		// switching off: ARS deregistration (radio_power_off_busy until done); arg 1 = canceled
	RREQ_RELOAD,		// the settings' preferences changed: read them again (codeplug changes are noticed alone)
	RREQ_DMR_TS,		// DMR: arg = timeslot 1 / 2 (temporary)
	RREQ_DMR_CC,		// DMR: arg = color code 0..15 (temporary)
	RREQ_MODE,		// arg = radio_mode_t (temporary FM <-> DMR on a channel)
	RREQ_DMR_HEAR_ALL,	// DMR: arg = 1 latch hear all, 0 off
	RREQ_DMR_FOCUS,		// DMR: arg = ID (bits 23:0) | private (bit 24) of the focused row, 0 = none
	RREQ_HS_CWID,		// hotspot: key the CW ID left by radio_hs_ops.cw_id (radio_hotspot.h)
	RREQ_ROAM,		// DMR roaming: arg = radio_roam_cmd_t
	RREQ_VFO_STEP_SIZE,	// arg: the VFO step in Hz
	RREQ_VIBRATE,		// arg: vibe_t (radio/vibe.h)
	RREQ_VOLUME,		// arg: the volume step 0..RADIO_VOLUME_MAX (radios without a knob)
	RREQ_VOLUME_OVERRIDE,	// arg: the step over the knob, -1 = back to the knob
	RREQ_VFO_COPY,		// the channel's settings into the VFO, and open it (CONFIG_VFO); no-op in the VFO
} radio_req_type_t;

// RREQ_TONE's argument: which (RADIO_TONE_*) in bits 25:24, the tone type in 23:16, the value in 15:0
static inline int32_t radio_tone_arg(uint8_t which, subtone_t t)
{
	return (int32_t)(((uint32_t)(which & 3u) << 24) | ((uint32_t)t.type << 16) | t.value);
}

typedef struct {
	uint8_t type;		// radio_req_type_t
	int32_t arg;
	uint32_t arg2;
} radio_req_t;

#if CONFIG_DMR
#define RADIO_NAMES_MAX		(1 + CODEPLUG_LIST_LEN)	// talk target and the listen list's contacts
#define RADIO_LOOKUPS		4	// IDs looked up in all contacts / the DMR ID database, most recent kept

// A contact name the screens show for a talkgroup or an ID
typedef struct {
	uint32_t id;
	bool group;
	char name[CODEPLUG_NAME_LEN + 1];
} radio_name_t;
#endif

typedef struct {
	radio_view_t view;
	uint32_t serial;		// bumped on every visible change

	zone_t zone;			// the current zone's channel list
	channel_t vfo_ch;		// the VFO's settings, kept while on a channel
	bool vfo_seeded;		// vfo_ch holds a frequency
	uint32_t vfo_talk_id;		// the DMR VFO's talk target (CONFIG_VFO_DMR), 0 = none chosen: talkgroup 9
	bool vfo_talk_private;
	bool have_channels;		// some zone holds a readable channel

	uint8_t base_squelch;		// squelch without the temporary change
	bool temp_power;
	bool temp_squelch;
	uint8_t base_bw;		// the channel's own bandwidth and tones, for undoing temporary changes
	subtone_t base_rx_tone;
	subtone_t base_tx_tone;
	uint8_t base_cc;		// the channel's own color code, timeslot and mode (temporary changes)
	uint8_t base_ts;
	uint8_t base_mode;
	uint8_t temp_ts;		// a timeslot chosen for now (RREQ_DMR_TS), 0 = none
	uint8_t board_mode;		// radio_mode_t the board was last put in
	uint8_t vol_step;		// the volume step of a radio without a knob
	int8_t vol_override;		// the debug link's step over the knob, -1 = none
	int16_t volume_sent;		// last level given to board.radio->volume, -1 = none yet

	bool ptt;			// PTT held
	bool host_audio;		// this PTT is a host audio session's (USRP): its audio, not the microphone
	uint32_t host_pushes;		// the host's audio pushes last seen, and when they last moved (the watchdog)
	uint32_t host_push_ms;
	bool retune_pending;		// receive settings changed during TX: retune on PTT release
	uint32_t tx_start_ms;
	uint8_t close_count;		// polls in a row above the close threshold
	uint8_t tone_count;		// polls in a row disagreeing with view.tone_ok (debounce)
	int16_t rssi_published;
#if CONFIG_DMR
	int16_t rssi_ring[RADIO_RSSI_HOLD_POLLS];	// the last polls' readings while the DMR path runs (peak hold)
	uint8_t rssi_ring_n;		// readings in the ring (0 outside DMR)
	uint8_t rssi_ring_pos;
#endif
	int8_t audio_out;		// last speaker / LED settings sent to the board, -1 = not yet
	int8_t green_out;
	int8_t red_out;
	bool hotspot;			// hotspot mode: the host link owns the modem

	radio_settings_t set;		// the settings in effect
	uint32_t activity_ms;		// last request, reception or transmission (battery save)
	bool tot_warned;		// the time-out warning sounded for this transmission
#if CONFIG_SCAN
	scan_t scan;
#endif
	batt_t batt;
	uint32_t batt_ms;		// last battery reading
	uint32_t tx_end_ms;		// end of the last transmission (battery readings wait)
#if CONFIG_FM_TX_TONES
	bool burst_keyed;		// the 1750 Hz burst keyed the transmitter (not PTT)
	uint32_t roger_ms;		// roger beep start, 0 = none
#endif
#if CONFIG_SUBTONE
	uint32_t tail_ms;		// sub-audio tail start (hal_radio_t.tx_tail), 0 = none
#endif
#if CONFIG_VOX
	bool vox_keyed;
	uint8_t vox_count;		// polls in a row above (or below, while keyed) the threshold
	uint32_t vox_quiet_ms;		// keyed: when the voice stopped, 0 = still talking
#endif
#if CONFIG_BATTERY_SAVE
	bool save_on;			// duty cycling now
	bool save_asleep;
	uint32_t save_ms;		// start of the current sleep / wake
#endif
	uint32_t codeplug_serial;	// codeplug_serial() when the zone and channel were last read
	bool reload_pending;		// RREQ_RELOAD came while busy
	bool power_off_req;		// RREQ_POWER_OFF seen
	bool power_off_busy;		// still sending what has to go out before the supply drops
#if CONFIG_DMR
	uint8_t dmr_wake_admits;	// the stack's wake_admits at the last tick (the permit tone for a short press)
	uint8_t dmr_prev_state;		// radio_dmr_state_t at the last tick (beeps on changes)
	bool codec;			// the vocoder is present
	bool dmr_running;		// the call stack has the modem
	bool dmr_keyed;			// board.radio->tx() armed the per-burst keying (until tx_off)
	uint32_t own_id;		// the radio's DMR ID (codeplug, else RADIO_DMR_ID_DEFAULT)
	uint32_t dmr_serial;		// call stack view serial last copied
	uint32_t talk_id;		// the channel's talk target
	bool talk_private;
	uint8_t talk_ts;		// the contact's timeslot override, 0 = none
	uint32_t listen[CODEPLUG_LIST_LEN];	// the channel's listen list (talkgroup IDs)
	uint16_t listen_count;
	radio_name_t names[RADIO_NAMES_MAX];
	uint8_t name_count;
	radio_name_t lookups[RADIO_LOOKUPS];	// cache of codeplug_contact_find / dmr_iddb_lookup answers ("" = none)
	uint8_t lookup_next;
	// A talk target chosen for now (RREQ_DMR_TARGET / _CONTACT / _CALL_BACK), on channel manual_ch
	bool manual;
	uint32_t manual_id;
	bool manual_private;
	uint8_t manual_ts;
	uint16_t manual_ch;
	bool manual_vfo;
	// Hear all latched (RREQ_DMR_HEAR_ALL) on channel latch_ch
	bool hear_all_latch;
	uint16_t latch_ch;
	bool latch_vfo;
	// The focused row's target (RREQ_DMR_FOCUS) and the one-shot target PTT took from it: the talk target until
	// one_shot_until (0 = while the call goes on), on channel one_shot_ch
	uint32_t focus_id;
	bool focus_private;
	bool one_shot;
	uint32_t one_shot_id;
	bool one_shot_private;
	uint16_t one_shot_ch;
	uint32_t one_shot_until;
	uint8_t win_group_s, win_private_s;	// reply windows in effect (dmr_configure)
	uint8_t privacy_type;		// dmrPrivacyType_t, 0 = off
	uint16_t privacy_key;
	char radio_name[CODEPLUG_RADIO_NAME_LEN + 1];	// our talker alias text
	uint16_t group_hang_ms, private_hang_ms;	// codeplug hang times, 0 = the stack's default reply window
	uint32_t hs_rx_hz, hs_tx_hz;	// hotspot frequencies
	uint8_t hs_power;		// hotspot board power level
	bool hs_carrier;		// hotspot: the receiver hears a carrier
#if CONFIG_HOTSPOT
	uint8_t cw_phase;		// hotspot CW ID: CW_* in radio_sm.c, 0 = not keying
	morse_t cw;
	uint32_t cw_ms;			// start of the current phase
	uint32_t cw_start_ms;		// carrier up
	uint16_t cw_tone_hz;
	bool cw_key;			// the tone keyed now
#endif
#endif
#if CONFIG_DMR_ROAMING
	roam_t roam;
	uint8_t roam_zones[SETTINGS_ROAM_ZONE_BYTES];	// the roaming zones (settings_t.roam_zones)
	bool roam_dirty;		// the site list needs building again
	uint16_t roam_zone;		// zone and channel the site list was built for
	uint16_t roam_channel;
	bool roam_probing;		// our wake probe is on the air (dmr_call_data_tx)
	uint8_t roam_probe_err;		// its result, 0xFF = still running
	uint32_t roam_quiet_ms;		// last reception or transmission seen (ROAM_QUIET_MS)
	uint8_t roam_prev_why;		// to notice a finished search (beep, toast)
	bool roam_shown;		// a survey round is away: the screens keep showing the site it left
	channel_t roam_shown_ch;	// ... that site's channel
	uint16_t roam_shown_pos, roam_shown_number;
#endif
#if CONFIG_DMR_EMERGENCY
	bool em_keyed;			// the emergency hot mic keyed the transmitter (not PTT)
	uint32_t em_cfg_serial;		// dmr_emergency_config_serial() the call stack was configured with
#endif
#if CONFIG_APRS
	// APRS transmissions (core/aprs, docs/spec/aprs.md 6): the radio leaves the channel for the APRS frequency
	uint8_t aprs_phase;		// APRS_RF_* in radio_sm.c, 0 = not on the APRS frequency
	uint32_t aprs_ms;		// start of the current phase (LISTEN: of the current slot)
	uint32_t aprs_listen_ms;	// start of the wait for a clear channel
	uint32_t aprs_rand;		// p-persistence
	bool aprs_was_tx;		// our own transmission (not APRS) at the last poll: the "after PTT" trigger
	aprs_rf_job_t aprs_job;
#endif
} radio_sm_t;

// Start-up: the codeplug and the settings (core/settings): the saved zone, channel or VFO, else zone 0 and its
// first channel (or the VFO); FM receive. Every request notes the position and the VFO in the settings, and
// radio_sm_poll re-reads the codeplug when codeplug_serial() moves
void radio_sm_init(radio_sm_t *sm, uint32_t now_ms);
void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms);
// Every RADIO_POLL_MS: status, squelch, audio, LEDs, TX time-out
void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms);

// Every RADIO_TICK_MS: the DMR call stack (poll, PTT keying, TX done, the DMR view). Nothing without CONFIG_DMR
void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms);

// Hotspot frequencies the board can use: RX in a band, TX in a band that allows transmit (the amateur band plan does
// not apply, docs/spec/gotchas.md 8.7)
bool radio_sm_hotspot_freq_ok(uint32_t rx_hz, uint32_t tx_hz);
// MMDVM RF level 0-255 -> board power level
uint8_t radio_sm_hotspot_power(uint8_t rf_level);

// Settings in effect from now on (radio_task.c calls it for RREQ_SETTINGS; the tests call it directly)
void radio_sm_set_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms);

// The view as the screens and the debug link get it: during a roaming survey visit (a few hundred ms on another
// site, docs/spec/dmr-roaming.md 4.9) it still shows the site the radio returns to. radio_task.c applies it when it
// publishes
void radio_sm_shown_view(const radio_sm_t *sm, radio_view_t *v);

// Squelch thresholds for a level (exposed for the tests)
uint8_t radio_sm_squelch_open_below(uint8_t level);

#endif
