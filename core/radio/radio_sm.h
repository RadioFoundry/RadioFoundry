// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's state machine, without the RTOS: it takes requests and the time, polls board.radio and keeps
// the view. radio_task.c runs it in a task; the host tests run it against a fake board.
#ifndef RADIO_SM_H
#define RADIO_SM_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio.h"
#include "radio/radio_squelch.h"
#include "radio/scan.h"
#include "radio/battery.h"
#include "radio/morse.h"
#include "radio/radio_cwid.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif
#if CONFIG_DMR_ROAMING
#include "radio/roam.h"
#include "settings/settings.h"	// SETTINGS_ROAM_ZONE_BYTES
#endif
#if CONFIG_DMR_TRUNK
#include "radio/radio_trunk.h"	// not under roaming: trunking builds without it
#endif

#define RADIO_SM_CW		(CONFIG_HOTSPOT || CONFIG_SFR_CWID)	// the CW keyer (radio_sm.c)
#define RADIO_POLL_MS		25	// status poll and squelch period
#define RADIO_RETUNE_SETTLE_MS	400	// RREQ_RETUNE: tune once the reference trim has held this long
#define RADIO_TICK_MS		10	// DMR call stack poll (radio_sm_tick)
// The LEDs on a repeater wake-up and a TX deny (docs/spec/radio-features.md 5.7): while the radio waits for a
// repeater to wake, red blinks once per wake-up burst sent, not solid (a burst is 30 ms: stretched to be seen); a
// deny flashes red, green, red, green, red, green, RADIO_LED_DENY_STEP_MS each
#define RADIO_LED_WAKE_MS	100
#define RADIO_LED_STUCK_STEP_MS	125	// a stuck PTT line: red and green in turn, to ask for a power cycle
#define RADIO_LED_DENY_STEP_MS	100
#define RADIO_LED_DENY_STEPS	6
#define RADIO_TOT_DEFAULT_S	180	// TX time-out
#define RADIO_VFO_STEP_HZ	12500

// The squelch levels (RADIO_SQUELCH_*, RADIO_SQL_*): radio/radio_squelch.h
// A carrier that goes: the noise jumps far over the line at once, while a fade crosses it by a little. A reading this
// far above the close line (open threshold + RADIO_SQL_HYST_DB) closes after one poll, which halves the squelch tail
// (radio-features.md 3.5)
#define RADIO_SQL_FAST_CLOSE_DB	12
// DMR scan: a stop also hears a carrier by its signal strength, as the squelch levels do on noise: level 1 above
// RADIO_DMR_SQL_L1_DBM, RADIO_SQL_DB_PER_STEP more a level (5, the default, above -100 dBm). Some receivers' noise
// reading doesn't follow a DMR signal (gotchas 17.3)
#define RADIO_DMR_SQL_L1_DBM	(-112)
// ... and level 1 at least this far above the floor the scan measured on the stops before (scan_floor): an uncalibrated
// strength reading can sit above the scale with no signal at all (gotchas 17.4)
#define RADIO_DMR_SQL_L1_FLOOR_DB	6
// While APRS receive listens (aprs.md 5.3): a packet's tones at full deviation push the noise reading over the line
// for a poll or two, and on a radio whose modem gates it, a closed squelch zeroes the receive audio, so a packet came apart (on air,
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
#define RADIO_SCAN_AIR_PROBE_MS	700	// P25, NXDN: a carrier, waiting for a call of the channel's NAC or RAN (the engine
					// starts at the retune; a P25 voice frame and its NID come every 180 ms)
#define RADIO_SCAN_AIR_SYNC_MS	1500	// P25, NXDN: such a call heard, waiting for the field its squelch needs (a P25
					// late entry's link control comes every 360 ms, an NXDN VCALL in each SACCH superframe)
// Dual watch with a second receiver (board.rx2): how often its settings go again (the link drops repeats), how long
// it may take to say it is on them, and how long after a failure to try it again (meanwhile the main receiver alternates)
#define RADIO_RX2_REFRESH_MS	2000
#define RADIO_RX2_TUNE_MS	1500
#define RADIO_RX2_RETRY_MS	10000
#define RADIO_SCAN_HOLD_S	5
#define RADIO_SCAN_DELAY_MS	2000

// Battery monitor: one reading a second, none while transmitting and for a while after (the load sags the voltage)
#define RADIO_BATT_PERIOD_MS	1000
#define RADIO_BATT_TX_QUIET_MS	3000

// FM transmit tones
#define RADIO_BURST_HZ		1750
#define RADIO_BURST_MIN_MS	1000
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
	RREQ_BEEP,		// arg: beep_t, arg2: delay in ms (0 = now)
	RREQ_TONE_BURST,	// arg: 1 on, 0 off; arg2: the burst's longest time in ms (0 = until off or the TX time-out)
	RREQ_TONE_BURST_KEY,	// arg: 1 down, 0 up; at least RADIO_BURST_MIN_MS
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
	RREQ_DTMF,		// arg: the digit (ASCII) in bits 7:0, bit 8 = key down (0 = up)
	RREQ_DUAL,		// dual watch (CONFIG_DUAL_WATCH): arg channel A, arg2 channel B, radio_dual_arg()
	RREQ_RETUNE,		// tune the current frequency again (the reference trim changed: radio/ref_trim.h)
	RREQ_VFO_TX,		// arg: the VFO's TX Hz, 0 = simplex (its RX frequency)
	RREQ_WAKE,		// radio_task.c: tick now (a DMR transmission ended: dmr_call_set_wake); never radio_sm_request
	RREQ_VOLUME_HOLD,	// arg: 1 hold the level and ignore the knob (keys locked), 0 follow it again
	RREQ_VFO_SFR,		// the VFO's SFR role (CONFIG_SFR): arg = byte A, as a channel's (sfr.h); VFO only
	RREQ_T3,		// a trunked system (CONFIG_DMR_TRUNK): arg = radio_t3_cmd_t
	RREQ_T3_POLL,		// poll MS arg2 for its short data, as the system's poll setting says (CONFIG_DMR_TRUNK_TX)
	RREQ_T3_DIAL,		// the digits a call through a gateway sends (CONFIG_DMR_TRUNK_GW): radio_req_t3_dial()
	RREQ_P25_SET,		// radio_task.c: the pending P25 settings were copied (radio_p25_set_settings); applied here
	RREQ_AIR_BENCH,		// the air interface bench (CONFIG_AIR_RAW): arg 1 take the modem, 0 give it back
	RREQ_AIR_TX,		// ... arg 1 key (arg2: the longest time in ms), 0 unkey
	RREQ_NXDN_SET,		// radio_task.c: the pending NXDN settings were copied (radio_nxdn_set_settings); applied here
} radio_req_type_t;

// RREQ_TONE's argument: which (RADIO_TONE_*) in bits 25:24, the tone type in 23:16, the value in 15:0
static inline int32_t radio_tone_arg(uint8_t which, subtone_t t)
{
	return (int32_t)(((uint32_t)(which & 3u) << 24) | ((uint32_t)t.type << 16) | t.value);
}

// RREQ_DUAL's arguments: a dual watch channel in 32 bits, RADIO_DUAL_VFO | which VFO, or zone << 16 | position, with
// RADIO_DUAL_SEL on the selected one. Neither selected: stop on the selected channel
#define RADIO_DUAL_VFO	(1u << 31)
#define RADIO_DUAL_SEL	(1u << 30)
static inline uint32_t radio_dual_arg(const radio_dual_ref_t *r, bool sel)
{
	uint32_t a = r->vfo ? RADIO_DUAL_VFO | (r->pos & 1u) : ((uint32_t)(r->zone & 0x3FFFu) << 16) | r->pos;
	return a | (sel ? RADIO_DUAL_SEL : 0u);
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

#if CONFIG_DUAL_WATCH
// The VFO not in use: dual watch gives each channel its own (radio-features.md 2.1)
typedef struct {
	channel_t ch;
	bool seeded;
	uint32_t talk_id;
	bool talk_private;
} radio_vfo_t;
#endif

#if CONFIG_SCAN && CONFIG_DUAL_WATCH
// A dual watch channel's changes for now (P7), kept while the receiver is on the other channel
typedef struct {
	bool valid;
	bool temp_power, temp_squelch;
	uint8_t power, squelch, temp_ts;
	uint8_t bw, mode, color_code, timeslot;
	subtone_t rx_tone, tx_tone;
	bool manual, manual_private, hear_all_latch;	// DMR: a talk target for now, hear all latched
	uint8_t manual_ts;
	uint32_t manual_id;
} radio_dual_keep_t;
#endif

typedef struct {
	radio_view_t view;
	uint32_t serial;		// bumped on every visible change

	zone_t zone;			// the current zone's channel list
	channel_t vfo_ch;		// the VFO's settings, kept while on a channel
	bool vfo_seeded;		// vfo_ch holds a frequency
	uint32_t vfo_step_set;		// the VFO's step as set, 0 = auto: from the width (view.vfo_step_hz has the one in use)
	uint32_t vfo_talk_id;		// the DMR VFO's talk target (CONFIG_VFO_DMR), 0 = none chosen: talkgroup 9
	bool vfo_talk_private;
	uint8_t vfo_idx;		// which VFO the four above are: 0 or 1 (view.vfo_index)
#if CONFIG_DUAL_WATCH
	radio_vfo_t vfo_other;		// the other VFO, swapped in by vfo_use
#endif
	bool have_channels;		// some zone holds a readable channel

	uint8_t base_squelch;		// squelch without the temporary change
	bool temp_power;
	bool temp_squelch;
	bool squelch_save;		// a squelch set in the VFO: the radio's default, written by persist()
	uint8_t base_bw;		// the channel's own bandwidth and tones, for undoing temporary changes
	subtone_t base_rx_tone;
	subtone_t base_tx_tone;
	uint8_t base_cc;		// the channel's own color code, timeslot and mode (temporary changes)
	uint8_t base_ts;
	uint8_t base_mode;
	uint8_t temp_ts;		// a timeslot chosen for now (RREQ_DMR_TS), 0 = none
	uint8_t ts_pending;		// RREQ_DMR_TS that came during a transmission without PTT: applied at its end
	uint8_t board_mode;		// radio_mode_t the board was last put in
	uint8_t vol_step;		// the volume step of a radio without a knob
	int8_t vol_override;		// the debug link's step over the knob, -1 = none
	int16_t volume_sent;		// last level given to board.radio->volume, -1 = none yet
	int16_t knob_seen;		// the knob's position last put in view.volume_knob, -1 = none yet
	bool vol_hold;			// RREQ_VOLUME_HOLD: the level stays, the knob is ignored

	bool ptt;			// PTT held
	bool host_audio;		// this PTT is a host audio session's (USRP): its audio, not the microphone
	uint32_t host_pushes;		// the host's audio pushes last seen, and when they last moved (the watchdog)
	uint32_t host_push_ms;
	bool retune_pending;		// receive settings changed during TX: retune on PTT release
	bool retune_due;		// the reference trim changed: tune at retune_due_ms (RREQ_RETUNE)
	uint32_t retune_due_ms;
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
#if CONFIG_BCAST_FM
	bool bcast_on;			// the broadcast receiver is powered (radio_bcast.h)
	int8_t bcast_audio_out;		// last board.bcast audio / volume sent, -1 = not yet
	int8_t bcast_volume_out;
#endif
	int8_t green_out;
	int8_t red_out;
	// The LEDs' timed signals (outputs()): led_now is the time of the entry point that runs last (poll, tick,
	// request), since outputs() is called from everywhere without one
	uint32_t led_now;
	uint32_t deny_led_ms;		// a TX deny: red, green, red, green, red, green from here
	bool deny_led;
	bool deny_rx;			// FM: PTT refused as busy over a reception, which plays on while PTT is held
	uint32_t wake_led_ms;		// a repeater wake-up burst went out here: red for RADIO_LED_WAKE_MS
	bool wake_led;
	uint8_t wake_seen;		// dmr_call_view_t.wake_sent last seen
	bool led_timed;			// a timed signal was showing at the last outputs(): the tick keeps calling it
	bool hotspot;			// hotspot mode: the host link owns the modem
#if CONFIG_AIR
	// An air-framed mode (P25, NXDN; radio_air.h)
	bool air_tx;			// a transmission runs in the engine (from prepare until it is done and cleared)
	bool air_keyed;			// board.radio->tx() keyed for it (until tx_off)
#endif
#if CONFIG_P25
#if CONFIG_P25_TRUNK
	bool p25_req;			// PTT on a trunked channel: a voice request waits for its grant
	bool p25_talk;			// the transmission runs on the voice channel granted to us
#endif
#endif
#if CONFIG_AIR_RAW
	bool air_bench;			// the debug link's air interface bench has the modem (RREQ_AIR_BENCH)
	bool bench_keyed;		// ... and the transmitter keyed for it (RREQ_AIR_TX)
	uint16_t bench_max_ms;		// its cap
	uint32_t bench_key_ms;		// when it keyed
	uint8_t bench_seq;		// the latest bench request handled (radio_task.c waits for it)
#endif

	radio_settings_t set;		// the settings in effect
	uint32_t activity_ms;		// last request, reception or transmission (battery save)
	bool tot_warned;		// the time-out warning sounded for this transmission
#if CONFIG_SCAN
	scan_t scan;
	radio_dual_ref_t dual[2];	// SCAN_DUAL: the two channels
	uint8_t dual_sel;		// ... the selected one (PTT)
	uint8_t dual_at;		// ... the one the receiver is on
#if CONFIG_DUAL_WATCH
	bool rx2_on;			// the second receiver (board.rx2) holds the other channel; the main one stays on dual_sel
	// Each channel's changes for now (P7), kept while the receiver is on the other one: a hop reloads the channel
	// from the codeplug, which would drop them. Valid until the user stops the watch or the channel changes
	radio_dual_keep_t dual_keep[2];
	uint32_t rx2_try_ms;		// last time it was given the other channel, or found down
	uint32_t rx2_refresh_ms;	// last time its settings went again
	int8_t rx2_audio_out;		// last board.rx2->audio(), -1 = none yet
	// One receiver, in the wait after a signal: on the other channel for a look (scan.tuned_ms its start), the wait's
	// start to go back to, and whether this look heard a signal
	bool dual_peek;
	bool dual_peek_heard;
	uint32_t dual_wait_ms;
#endif
#endif
	batt_t batt;
	uint32_t batt_ms;		// last battery reading
	uint32_t tx_end_ms;		// end of the last transmission (battery readings wait)
#if CONFIG_BEEP
	uint8_t beep_due;		// a beep asked for later (RREQ_BEEP arg2), BEEP_NONE = none
	uint32_t beep_due_ms;		// when it plays
#endif
#if CONFIG_FM_TX_TONES
	bool burst_timed, burst_held;
	bool burst_key_owned;		// side-key release owns the burst or its TX denial
	uint32_t burst_ms;		// start of the side-key burst
	bool burst_keyed;		// the 1750 Hz burst keyed the transmitter (not PTT)
	uint32_t burst_end_ms;		// the burst ends by itself at this time (RREQ_TONE_BURST arg2), 0 = no limit
	uint32_t roger_ms;		// roger beep start, 0 = none
#endif
#if CONFIG_DTMF
	bool dtmf_held;			// the key of the digit sounding (view.dtmf) is still down
	char dtmf_next;			// a digit waiting for the one sounding (or the gap) to end, 0 = none
	bool dtmf_next_held;		// ... its key is still down
	uint32_t dtmf_on_ms;		// the digit sounding started
	uint32_t dtmf_off_ms;		// the last digit ended
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
#if CONFIG_ZONE_WRITE
	uint32_t zone_shifts;		// zone_edit_shifts() when the zone indexes below were last followed (zone_edit.h)
#endif
	bool reload_pending;		// RREQ_RELOAD came while busy
	bool power_off_req;		// RREQ_POWER_OFF seen
	bool power_off_busy;		// still sending what has to go out before the supply drops
#if CONFIG_DMR
	uint8_t dmr_wake_admits;	// the stack's wake_admits at the last tick (the permit tone for a short press)
	uint8_t dmr_permit_seq;		// the stack's permit_seq at the last tick (the permit tone before keying)
	uint8_t dmr_prev_state;		// radio_dmr_state_t at the last tick (beeps on changes)
	bool codec;			// the vocoder is present
	bool dmr_running;		// the call stack has the modem
	bool dmr_killed;		// settings t3_kill: no DMR at all (dmr-trunking.md 7.9)
	bool dmr_keyed;			// board.radio->tx() armed the per-burst keying (until tx_off)
	bool sfr_rpt;			// the channel's SFR repeater role has the modem (radio_sfr.c), not the call stack
	bool sfr_denied;		// a repeater channel kept ordinary because the radio may not transmit or a scan
					// runs: the role starts once it may (TX inhibit cleared, the scan stopped)
	uint32_t dmr_rx_hz;		// the frequency the call stack's receiver searched for slot timing on
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
	bool scan_hear_all;	// ... and carried by a scan (not dual watch) started with it on: on at every stop
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
#if RADIO_SM_CW
	uint8_t cw_phase;		// a CW ID: CW_* in radio_sm.c, 0 = not keying
	morse_t cw;
	uint32_t cw_ms;			// start of the current phase
	uint32_t cw_start_ms;		// carrier up
	uint16_t cw_tone_hz;
	bool cw_key;			// the tone keyed now
	bool cw_sfr;			// the SFR repeater's ID (CONFIG_SFR_CWID), not the hotspot's
#endif
#if CONFIG_SFR_CWID
	radio_cwid_t cwid;		// the SFR repeater's ID schedule (the counts are in view.dmr)
#endif
#endif
#if CONFIG_DMR_TRUNK
	// A Tier III trunked system (radio_trunk.h): the engine drives the channel the radio is on
	bool t3_on;
	bool t3_pending;		// a trunked channel was selected: start its system (t3s) at the next t3_tick
	bool t3_tagged;			// the channel is a trunked one (channel_t.kind)
	bool t3_ch_rx;			// and receive only (its tag): the system runs listen only from it
	uint8_t t3_kv;			// codeplug_kv_serial() when t3s was loaded from the codeplug
	uint8_t t3_fb;			// fallback (dmr-trunking.md 12): 0 off, 1 on the fallback channel, 2 probing
	uint16_t t3_fb_number;		// the fallback channel, 0: none
	uint32_t t3_hunt_ms;		// since when the engine has been hunting with no TSCC (0: it isn't)
	uint32_t t3_probe_ms;		// on the fallback channel: when to look next; probing: until when
	channel_t t3_fb_ch;
	bool t3_both;			// on a control channel: both slots
	bool t3_ptt_wait;		// PTT held on the TSCC: waiting for our grant
	bool t3_als_keyed;		// keyed by an Ambient Listening call (6.6.2.6), not by the user
	uint32_t t3_als_quiet_ms;	// unkeyed at T_ALS: the call ends once the terminator is out
	uint8_t t3_stun_kept;		// settings t3_stun as last written for the system in use (0: not stunned)
	uint32_t t3_sd_seen;		// the engine's short data count already given to the inbox
	bool t3_polling;		// a poll from the menus is out (RREQ_T3_POLL): its outcome is counted
	bool t3_talking;		// PTT went to the stack for a trunked call
	uint32_t t3_ticks;		// the stack's tick count handed to the engine
	uint32_t t3_slc_n;		// the stack's short LC count handed to the engine
	bool t3_rx_off;			// the receiver is off: the engine sleeps (power save)
	uint32_t t3_rx_off_at;
	uint32_t t3_tx_end_ms;	// when our last transmission on the system ended, 0 = a burst was heard since
	uint8_t t3_sent_report;	// the stack's word on our last burst list: 1 sent, 2 dropped, 0 none pending
	uint8_t t3_slot;		// the slot of the last tick handed to it
	uint8_t t3_at[2];		// the last tick flags (DMR_TICK_AT_*) of the CACH naming slot 1, 2
	uint8_t t3_cc;			// the color code of the channel being listened to
	uint8_t t3_send_left;		// the bursts the data source still has to give
	uint8_t t3_send_n;		// bursts in t3_send_burst: a CSBK, or a UDT header and its blocks
	bool t3_send_udt;
	bool t3_sending;		// a burst list is with the stack: a retune would drop it, so it waits
	bool t3_tune_held;		// a retune waiting for it: t3_tune_chan, t3_tune_slot, since t3_tune_ms
	uint8_t t3_tune_slot;
	t3_chan_t t3_tune_chan;
	uint32_t t3_tune_ms;
	uint8_t t3_send_burst[1 + T3_UDT_BLOCKS_MAX][T3_CSBK_BYTES];
	uint32_t t3_now;		// the clock at the last t3_tick (for the data service's start, which has none)
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
	bool aprs_was_tx;		// our own transmission (not APRS) at the last poll: the "after PTT" trigger
#endif
#if CONFIG_APRS_AFSK
	// APRS transmissions on FM (core/aprs, docs/spec/aprs.md 6): the radio leaves the channel for the APRS frequency
	uint8_t aprs_phase;		// APRS_RF_* in radio_sm.c, 0 = not on the APRS frequency
	uint32_t aprs_ms;		// start of the current phase (LISTEN: of the current slot)
	uint32_t aprs_listen_ms;	// start of the wait for a clear channel
	uint32_t aprs_rand;		// p-persistence
	aprs_rf_job_t aprs_job;
#endif
	bool tx_no_ptt;		// on the air without PTT (a DMR data burst list): not the user's transmission, no "after PTT"
} radio_sm_t;

// Start-up: the codeplug and the settings (core/settings): the saved zone, channel or VFO, else zone 0 and its
// first channel (or the VFO); FM receive. Every request notes the position and the VFO in the settings, and
// radio_sm_poll re-reads the codeplug when codeplug_serial() moves
#if CONFIG_DMR_TRUNK
// The whole trunking view; view.t3 is its brief, made by radio_sm_t3_brief before the view is published
extern radio_t3_view_t radio_t3v;
void radio_sm_t3_brief(radio_sm_t *sm);
#endif
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
