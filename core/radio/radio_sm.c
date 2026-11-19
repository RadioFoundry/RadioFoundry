// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's state machine: channel / zone / VFO selection, software squelch, PTT checks and the TX timer,
// and the everyday features around them (docs/spec/radio-features.md): scan, busy channel lockout, roger beep and
// 1750 Hz burst, VOX, beeps, the battery monitor and receiver battery saving.
// No RTOS here: radio_task.c serializes the calls, and the host tests drive it directly.
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "settings/settings.h"
#include "config.h"
#include "hal_dtmf.h"
#include "codeplug/codeplug_modes.h"	// channel kinds (docs/spec/channel-modes.md): parked channels, in every build
#include "codeplug/codeplug_tag.h"	// CODEPLUG_SYSTEM_NONE
#if CONFIG_ZONE_WRITE
#include "codeplug/zone_edit.h"
#define ZONE_WRITE_BUSY()	zone_edit_busy()
#define ZONES_MOVED(sm)		(zone_edit_shifts() != (sm)->zone_shifts)
#else
#define ZONE_WRITE_BUSY()	false
#define ZONES_MOVED(sm)		false
#endif
#if CONFIG_DMR
#include "dmr/call/dmr_call.h"
#include "dmr/ambe/ambeVoice.h"
#include "codeplug/dmr_iddb.h"
#endif
#if CONFIG_DMR_PRIVACY
#include "dmr/dmrPrivacy.h"
#endif
#include "radio/beep.h"	// beep_t (beep_play only with CONFIG_BEEP)
#include "radio/sigtone.h"
#include "radio/vibe.h"	// vibe_t (vibe_play only with CONFIG_VIBRATE)
#include "radio/tx_limit.h"	// the test TX limit (radio-features.md 3.1)
#if CONFIG_P25 || CONFIG_NXDN
#include "air/air_session.h"	// air_tx_state_t
#endif
#if CONFIG_P25
#include "radio/radio_p25.h"	// P25 conventional (docs/spec/p25.md 7)
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif
#endif
#if CONFIG_NET_USRP || CONFIG_APRS_RX
#include "usbaudio/audio_mux.h"	// the PCM stream shared with a host audio session and APRS
#endif
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"	// dmr_sms_format_t
#include "position/nmea.h"	// DMR APRS positions heard (data_position)
#endif
#include "radio/radio_keys.h"	// the codeplug settings area's keys: tones, data service
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_SFR
#include "radio/radio_sfr.h"		// standalone SFR roles (docs/spec/sfr.md 4)
#endif
#include "dmr/sfr.h"			// the SFR bytes the VFO keeps in every build (vfo_sfr_set)
#if CONFIG_HOTSPOT
#include "radio/radio_hotspot.h"	// the CW ID handover
#endif
#if CONFIG_SFR_CWID
static void cw_cancel(radio_sm_t *sm);
#endif
#if CONFIG_DMR_IP_GATEWAY
#include "hostlink/hostlink_net_air.h"	// the IP gateway's air side (the data service's packet hook)
#include "hostlink/hostlink_net.h"	// control station passthrough
#include "dmr/ipGateway.h"		// the off-net route's gateway ID
#endif
#if CONFIG_DMR_AIR_QUEUE
#include "dmr/dmrAirQueue.h"	// what the gateway and zone sharing send, in order (docs/spec/dmr-data.md 12)
#endif
#if CONFIG_ZONE_SHARE
#include "dmr/zone_share_api.h"
#endif
#if CONFIG_AM_RX
#include "radio/radio_am.h"	// air band AM receive (docs/spec/airband-and-broadcast.md 3)
#endif
#if CONFIG_BCAST_FM
#include "radio/radio_bcast.h"	// broadcast FM receive (docs/spec/airband-and-broadcast.md 4)
#endif
#include "radio/radio_bands.h"	// the listen bands, never DMR
#if CONFIG_APRS_AFSK
#include "aprs/afsk.h"			// AFSK_BAUD, the tones
#endif
#if CONFIG_POSITION
#include "position/position.h"		// our position for DMR GPS Info and LRRP (docs/spec/position.md)
#endif

// The radio's own DMR ID when the codeplug's general settings hold none (build value; 0 = refuse DMR transmit)
#ifndef RADIO_DMR_ID_DEFAULT
#define RADIO_DMR_ID_DEFAULT	0
#endif

// Simplex frequencies tried, in order, for the VFO when there is nothing better to start from
// The VFO is analog (CONFIG_VFO_FM), digital (CONFIG_VFO_DMR) or both, with the mode a setting of the VFO itself
#define VFO_AVAILABLE	(CONFIG_VFO_FM || CONFIG_VFO_DMR)
// The mode a VFO starts in
#define VFO_MODE_DEFAULT	(CONFIG_VFO_FM ? RADIO_MODE_FM : RADIO_MODE_DMR)

// P25 on this board: built, and the air interface can receive it
static inline bool p25_ok(void)
{
#if CONFIG_P25
	return radio_p25_fits();
#else
	return false;
#endif
}

// NXDN on this board: built, and the air interface receives or transmits it at either rate
static inline bool nxdn_ok(void)
{
#if CONFIG_NXDN
	return radio_nxdn_fits();
#else
	return false;
#endif
}

// An air-framed mode this build runs (P25, NXDN): the radio core runs it with the board in its digital mode
static inline bool air_mode(uint8_t mode)
{
	return (CONFIG_P25 && mode == RADIO_MODE_P25) || (CONFIG_NXDN && mode == RADIO_MODE_NXDN);
}
// What a scan stop hears of an air mode's call (air_heard)
enum { AIR_HEARD_NONE, AIR_HEARD_PENDING, AIR_HEARD_PLAYED, AIR_HEARD_OTHER };
#if CONFIG_AIR_RAW
#define BENCH_HELD(sm)		((sm)->air_bench)	// the debug link's air interface bench has the modem
#else
#define BENCH_HELD(sm)		false
#endif

static const uint32_t vfo_start_hz[] = { 146520000, 145500000, 446000000, 433500000 };

// Used ahead of their definitions by the emergency and trunking code
static void ptt_down(radio_sm_t *sm, uint32_t now_ms);
static void ptt_release(radio_sm_t *sm, bool roger, uint32_t now_ms);

#if CONFIG_DMR_TRUNK
// The engine and the system it runs, in plain RAM (radio_sm_t sits in the CPU-only RAM, which is full on some radios)
static t3_ms_t t3m;
static radio_t3_setup_t t3s;	// the system, copied from the staged one at RADIO_T3_START
radio_t3_view_t radio_t3v;	// the whole trunking view (view.t3 is its brief: radio_sm_t3_brief); in RAM, not the
				// CPU-only RAM radio_sm_t may be in
#endif

// The VFO's step in use: auto follows the width (the air band's channels are 25 kHz apart). In the broadcast band
// it is always the broadcast step setting's, whatever the VFO step: a fixed 12.5 kHz step for the other bands
// shouldn't make the stations 8 or 16 knob clicks apart
static void step_update(radio_sm_t *sm)
{
	uint32_t hz = sm->vfo_step_set;
	if (hz == 0)
		hz = sm->view.ch.bw == RADIO_BW_25K || sm->view.am ? 25000u : RADIO_VFO_STEP_HZ;
#if CONFIG_BCAST_FM
	if (sm->view.bcast)
		hz = radio_bcast_step_hz(sm->set.bcast_step);
#endif
	sm->view.vfo_step_hz = hz;
	sm->view.vfo_step_auto = sm->vfo_step_set == 0;
}

static void changed(radio_sm_t *sm)
{
	step_update(sm);	// every change that can move the width comes here
	sm->serial++;
}

void radio_settings_default(radio_settings_t *s)
{
	memset(s, 0, sizeof(*s));
	s->tot_s = RADIO_TOT_DEFAULT_S;
	s->tot_warn_s = 10;
	s->bcl = RADIO_BCL_OFF;
	s->scan_resume = SCAN_RESUME_CARRIER;
	s->scan_hold_s = RADIO_SCAN_HOLD_S;
	s->scan_delay_ms = RADIO_SCAN_DELAY_MS;
	s->scan_dwell_ms = RADIO_SCAN_DWELL_MS;
	s->dual_delay_ms = RADIO_DUAL_DELAY_MS;
	s->beep_level = 50;
	s->key_beep = true;
	s->talk_permit = true;
	s->vox = RADIO_VOX_OFF;
	s->vox_level = 5;
	s->vox_tail_ms = 1500;
	s->rx_save = 0;		// off until the board's receiver sleep is verified on the bench
	s->rx_save_idle_s = 10;
	s->battery_off_s = 0;
	s->backlight_dim_s = 30;
	s->backlight_off_s = 120;
	s->reply_s = RADIO_REPLY_CODEPLUG;
	s->roam_rssi_dbm = -105;	// ROAM_RSSI_DEFAULT
	s->roam_hyst_db = 6;		// ROAM_HYST_DEFAULT
	s->roam_probe = 0;		// passive: no automatic transmission (P7)
	s->roam_check_s = 0;
	s->roam_beacon_s = 60;		// ROAM_BEACON_DEFAULT_S: the common setting of networked repeaters
	s->roam_beacon_len_s = 4;	// ROAM_BEACON_LEN_DEFAULT
	s->roam_survey = 1;		// listening only (P7 allows it): the survey of the synchronized beacon window
	s->rpt_beacon = false;		// transmits: off until the user turns it on (P7)
	s->sfr_rx = 0;			// DMR_SFR_RX_REPEATER
	s->sfr_cwid = 0;		// transmits: off until the user turns it on
}

static void activity(radio_sm_t *sm, uint32_t now_ms)
{
	sm->activity_ms = now_ms;
#if CONFIG_BEEP
	beep_warm(now_ms);	// a sound may follow: the amplifier up now (a board with a speaker hang)
#endif
}

// Whether a voice stream (DMR or P25) owns the PCM audio: beeps are mixed into it
static inline bool voice_streaming(void)
{
	bool on = false;
#if CONFIG_DMR
	on = ambeVoiceStreaming();
#endif
#if CONFIG_P25 && CONFIG_IMBE
	on = on || p25VoiceStreaming();
#endif
	return on;
}

#if CONFIG_BEEP
// The level a beep plays at, and whether it is mixed into the voice stream (running) or has its own stream
static uint8_t beep_level_now(const radio_sm_t *sm)
{
	uint8_t level = sm->set.beep_level;
	if (board.radio != NULL && board.radio->volume != NULL && !board.radio->volume_pcm) {
		level = radio_beep_level(level, sm->volume_sent);
	}
	return level;
}

static bool beep_shared_now(void)
{
	return voice_streaming();
}
#endif

// A beep at the settings' level: mixed into the voice stream when one runs, else on its own stream
static void beep(radio_sm_t *sm, uint8_t b)
{
#if CONFIG_BEEP
	if (radio_tone_off(sm->set.tones_off, b)) {
		return;		// Settings > Sound turned it off (the codeplug's settings area)
	}
	beep_play((beep_t)b, beep_level_now(sm), beep_shared_now());
#else
	(void)sm;
	(void)b;
#endif
}

// --- Signaling tones (sigtone.h, docs/spec/radio-features.md 4.3) ---

_Static_assert(SIG_EV_COUNT == 3, "radio_settings_t.sig_* hold three events");
_Static_assert(SIG_STEPS == 10, "radio_settings_t.sig_hz / sig_ms hold ten steps");

// Where the event's tone goes on this channel's mode (sig_out_t)
static uint8_t sig_out(const radio_sm_t *sm, sig_event_t ev)
{
#if CONFIG_SIG_TONES
	return sm->view.ch.mode == RADIO_MODE_DMR ? sm->set.sig_dmr[ev] : sm->set.sig_fm[ev];
#else
	(void)sm;
	(void)ev;
	return SIG_OUT_DEFAULT;	// built without the chains: the radio's own sounds
#endif
}

// Whether the event's tone sounds on the speaker instead of the radio's own: the setting says so, on a channel that
// allows beeps
static bool sig_local_on(const radio_sm_t *sm, sig_event_t ev)
{
	return CONFIG_BEEP && !sm->view.ch.no_beep && sig_out_local(sig_out(sm, ev));
}

// The event's tone on the speaker
static void sig_local(radio_sm_t *sm, sig_event_t ev)
{
#if CONFIG_BEEP && CONFIG_SIG_TONES
	beep_seg_t seg[SIG_STEPS];
	uint8_t n = sig_chain(sm->set.sig_hz[ev], sm->set.sig_ms[ev], ev, seg);
	beep_play_custom(seg, n, beep_level_now(sm), beep_shared_now());
#else
	(void)sm;
	(void)ev;
#endif
}

#if CONFIG_DMR_DATA_CORE
static void data_kv_load(void);
#endif

// The keys of the codeplug's settings area the radio core holds (radio_keys.h), read at start, on a reload (a codeplug
// written) and whenever codeplug_kv_serial() moves on (a key written by the menu or the debug link). No copy of the
// area in RAM: a read is a few bytes of flash, and only on those events
static uint8_t kv_seen;		// codeplug_kv_serial() at the last load

// The Settings > Sound tones' keys over s->tones_off, the radio's own copy (docs/spec/codeplug-settings.md 5.2): true
// if one changed it
_Static_assert(RADIO_TONES_MASK <= (SETTINGS_TONES_OFF_MASK >> SETTINGS_TONES_OFF_SHIFT), "the tones fit sound_flags");
static bool tones_apply(radio_settings_t *s)
{
	radio_tones_t t;
	radio_tones_unpack(s->tones_off, &t);
	if (!codeplug_keys_apply(radio_tone_rows, RADIO_TONE_ROWS_N, &t)) {
		return false;
	}
	s->tones_off = radio_tones_pack(&t);
	return true;
}

#if CONFIG_SIG_TONES
// The signaling tones' records over s->sig_*, the radio's own copy: one walk over their keys. A record the setting can't
// take reads as absent (sig_rec_decode). True if one changed
static bool sig_apply(radio_settings_t *s)
{
	bool changed = false;
	codeplug_kv_cursor_t c;
	uint16_t key;
	uint8_t v[SIG_REC_LEN];
	int len;
	codeplug_kv_begin(&c, CODEPLUG_KEY_SIG_BEGIN, CODEPLUG_KEY_SIG_BEGIN + SIG_EV_COUNT - 1);
	while ((len = codeplug_kv_next(&c, &key, v, sizeof(v))) >= 0) {
		sig_event_t ev = (sig_event_t)(key - CODEPLUG_KEY_SIG_BEGIN);
		sig_rec_t r, now;
		uint8_t a[SIG_REC_LEN], b[SIG_REC_LEN];
		radio_sig_get(s, ev, &now);
		if (sig_rec_decode(v, len, ev, &r)) {
			sig_rec_encode(&r, a);	// compared as stored: the struct has padding
			sig_rec_encode(&now, b);
			if (memcmp(a, b, sizeof a) != 0) {
				radio_sig_put(s, ev, &r);
				changed = true;
			}
		}
	}
	return changed;
}
#endif

static void radio_to_settings(const radio_settings_t *s, uint32_t now_ms);

static void kv_load(radio_sm_t *sm, uint32_t now_ms)
{
	kv_seen = codeplug_kv_serial();
	bool sig = false;
#if CONFIG_SIG_TONES
	sig = sig_apply(&sm->set);
#endif
	if (tones_apply(&sm->set) || sig) {
		changed(sm);	// published with the settings (radio_get_settings): the menu shows it
		radio_to_settings(&sm->set, now_ms);	// the radio's own copy follows the key
	}
#if CONFIG_DMR_DATA_CORE
	data_kv_load();
#endif
}

// The TX time-out in effect (FM and DMR): the channel's own from the codeplug, or the radio's (settings, 180 s by
// default) when the channel's is 0 (builtin channels and the VFO too)
static void apply_tot(radio_sm_t *sm)
{
	uint16_t t = sm->view.ch.tot_s;
	sm->view.tot_s = t != 0 ? t : sm->set.tot_s;
}

static uint8_t master_power = SETTINGS_UNSET8;	// the settings' power for "default" channels
// The radio task's copy of the settings for the few places that read more than one field (data_init, cw_begin,
// reload). Static: off the radio task's stack; one copy, since none of them calls another (RAM is tight on some radios)
static settings_t st_copy;

static uint8_t default_power(void)
{
	if (board.power_count == 0) {
		return 0;
	}
	if (master_power < board.power_count) {
		return master_power;
	}
	return board.power_default < board.power_count ? board.power_default : board.power_count - 1;
}

static uint8_t channel_power(const channel_t *ch)
{
	if (ch->power == CODEPLUG_POWER_DEFAULT || ch->power >= board.power_count) {
		return default_power();
	}
	return ch->power;
}

uint8_t radio_sm_squelch_open_below(uint8_t level)
{
	if (level == 0) {
		return 255;
	}
	if (level > RADIO_SQUELCH_MAX) {
		level = RADIO_SQUELCH_MAX;
	}
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

bool radio_view_wakes(const radio_view_t *v)
{
	if (v->hotspot) {
		return false;		// the host's traffic
	}
	switch (v->state) {
	case RS_TX:
		return !v->tx_auto;
	case RS_TX_DENIED:
		return true;		// PTT held: the screen says why
	case RS_RX:
		if (v->bcast) {
			return false;	// a broadcast program plays on: not something new to look at
		}
#if CONFIG_DMR
		if (v->ch.mode == RADIO_MODE_DMR) {
			return v->dmr.state == RDMR_RX;	// a call that plays; not data or a call we don't play
		}
#endif
#if CONFIG_P25
		if (v->ch.mode == RADIO_MODE_P25) {
			return (v->p25.flags & RADIO_P25_PLAYED) != 0;
		}
#endif
#if CONFIG_NXDN
		if (v->ch.mode == RADIO_MODE_NXDN) {
			return (v->nxdn.flags & RADIO_NXDN_PLAYED) != 0;
		}
#endif
		return true;		// FM: the squelch opened, something plays
	default:
		return false;
	}
}

static bool same_tone(const subtone_t *a, const subtone_t *b)
{
	return a->type == b->type && (a->type == TONE_NONE || a->value == b->value);
}

// Recompute the temporary flags. Bandwidth and tones count against the channel's own values; in the VFO they are
// the VFO's settings, never temporary
static void set_temporary(radio_sm_t *sm)
{
	uint8_t t = 0;
	if (sm->temp_power) {
		t |= RADIO_TEMP_POWER;
	}
	if (sm->temp_squelch) {
		t |= RADIO_TEMP_SQUELCH;
	}
	if (!sm->view.vfo) {
		if (sm->view.ch.bw != sm->base_bw) {
			t |= RADIO_TEMP_BW;
		}
		if (!same_tone(&sm->view.ch.tx_tone, &sm->base_tx_tone)) {
			t |= RADIO_TEMP_TX_TONE;
		}
		if (!same_tone(&sm->view.ch.rx_tone, &sm->base_rx_tone)) {
			t |= RADIO_TEMP_RX_TONE;
		}
		if (sm->view.ch.mode != sm->base_mode) {
			t |= RADIO_TEMP_MODE;
		}
		if (sm->view.ch.mode == RADIO_MODE_DMR) {
			if (sm->view.ch.color_code != sm->base_cc) {
				t |= RADIO_TEMP_CC;
			}
			if (sm->temp_ts != 0) {
				t |= RADIO_TEMP_TS;
			}
		}
	}
	sm->view.temp = t;
	sm->view.temporary = t != 0;
}

// Back to the channel's own settings. Returns whether the receiver needs retuning (bandwidth, RX tone, mode, color
// code or timeslot changed: tune() also reconfigures the DMR call stack)
static bool clear_temporary(radio_sm_t *sm)
{
	bool retune = (sm->view.temp & (RADIO_TEMP_BW | RADIO_TEMP_RX_TONE | RADIO_TEMP_MODE | RADIO_TEMP_CC |
					RADIO_TEMP_TS)) != 0;
	sm->temp_power = false;
	sm->temp_squelch = false;
	sm->temp_ts = 0;
	sm->ts_pending = 0;
	sm->view.power = channel_power(&sm->view.ch);
	sm->view.squelch = sm->base_squelch;
	if (!sm->view.vfo) {
		sm->view.ch.bw = sm->base_bw;
		sm->view.ch.rx_tone = sm->base_rx_tone;
		sm->view.ch.tx_tone = sm->base_tx_tone;
		sm->view.ch.mode = sm->base_mode;
		sm->view.ch.color_code = sm->base_cc;
		sm->view.ch.timeslot = sm->base_ts;
	}
	set_temporary(sm);
	return retune;
}

// RX tone squelch: whether the tone counts as heard, debounced. Always heard when the channel sets no RX tone
// (DMR channels never use one)
static bool tone_needed(const radio_sm_t *sm)
{
	if (sm->view.am || sm->view.bcast) {
		return false;	// AM and broadcast: sub-audio ignored
	}
	return sm->view.ch.rx_tone.type != TONE_NONE && sm->view.ch.mode == RADIO_MODE_FM;
}

static void follow_tone(radio_sm_t *sm, bool raw)
{
	bool needed = tone_needed(sm);
	if (!needed) {
		raw = true;
	}
	if (raw == sm->view.tone_ok) {
		sm->tone_count = 0;
		return;
	}
	sm->tone_count++;
	if (!needed || sm->tone_count >= (raw ? RADIO_TONE_ON_POLLS : RADIO_TONE_OFF_POLLS)) {
		sm->view.tone_ok = raw;
		sm->tone_count = 0;
		changed(sm);
	}
}

// After a retune, or with a new RX tone: nothing heard yet (or no tone needed)
static void reset_tone(radio_sm_t *sm)
{
	sm->view.tone_ok = !tone_needed(sm);
	sm->tone_count = 0;
}

// The APRS job's phases (aprs_phase)
enum { APRS_RF_OFF = 0, APRS_RF_LISTEN, APRS_RF_KEYUP, APRS_RF_SEND };

// A broadcast station is listened to, not squelched: the state stays RX (so the screen shows the level; the LED stays
// off, outputs()) whatever the signal, and the audio plays. A scan keeps the squelch to stop on stations
static bool bcast_listening(const radio_sm_t *sm)
{
#if CONFIG_BCAST_FM
	bool own_rx = true;	// an APRS job has the channel receiver on its own frequency
#if CONFIG_APRS_AFSK
	own_rx = sm->aprs_phase == APRS_RF_OFF;
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
	return own_rx && sm->view.bcast && !sm->view.bcast_fault;
#else
	(void)sm;
	return false;
#endif
}

static bool ptt_line_stuck(void)
{
	return board.input != NULL && board.input->ptt_line_stuck != NULL && board.input->ptt_line_stuck();
}

// The LEDs' timed signals over what the state shows (RADIO_LED_*_MS). A stuck PTT line flashes red and green fast.
// A deny flashes red, green, ... once: a radio without a screen has no other way to say PTT was refused. While the
// radio waits for a repeater to wake, red shows each wake-up burst going out, not the whole wait, which is mostly
// listening
static void led_signals(radio_sm_t *sm, bool *red, bool *green)
{
	uint32_t now = sm->led_now;
	bool timed = false;
	if (ptt_line_stuck()) {
		*red = ((now / RADIO_LED_STUCK_STEP_MS) & 1u) == 0;	// until the power cycle that releases it
		*green = !*red;
		sm->led_timed = true;
		return;
	}
	if (sm->deny_led) {
		uint32_t step = (now - sm->deny_led_ms) / RADIO_LED_DENY_STEP_MS;
		// A new transmission ends the flash: red then shows the air (or the wake-up bursts), not the refusal before
		if (step < RADIO_LED_DENY_STEPS && sm->view.state != RS_TX) {
			*red = (step & 1u) == 0;
			*green = !*red;
			sm->led_timed = true;
			return;
		}
		sm->deny_led = false;
	}
#if CONFIG_DMR
	bool waking = sm->view.state == RS_TX && sm->view.ch.mode != RADIO_MODE_FM && !sm->hotspot &&
		(sm->view.dmr.state == RDMR_TX_WAIT || sm->view.dmr.state == RDMR_TX_PERMIT);
	if (sm->wake_led && now - sm->wake_led_ms >= RADIO_LED_WAKE_MS) {
		sm->wake_led = false;
	}
	if (waking) {
		*red = sm->wake_led;
		timed = sm->wake_led;
	}
#endif
	sm->led_timed = timed;
}

static void deny_led_start(radio_sm_t *sm)
{
	sm->deny_led = true;
	sm->deny_led_ms = sm->led_now;
}

// A channel of a kind the radio doesn't run (docs/spec/channel-modes.md 6): tuned to its frequency but silent, never
// scanned and never keyed, whatever its record says. A built kind's mode module took its channel when it was read
// (channel_read), which gave it the mode's own: P25 or NXDN. A Tier III channel runs on its record's DMR (the
// trunking engine follows its system)
static bool parked(const channel_t *ch)
{
	if (ch->kind == CODEPLUG_KIND_NONE) {
		return false;
	}
	if (!codeplug_kind_built(ch->kind)) {
		return true;
	}
	return ch->kind != CODEPLUG_KIND_DMR_TRUNK && ch->mode != RADIO_MODE_P25 && ch->mode != RADIO_MODE_NXDN;
}

// Channel number from the codeplug as the radio runs it: the mode module of its kind takes it
static bool channel_read(uint16_t number, channel_t *ch)
{
	if (!codeplug_channel(number, ch)) {
		return false;
	}
#if CONFIG_P25
	radio_p25_take(ch);
#endif
#if CONFIG_NXDN
	radio_nxdn_take(ch);
#endif
	return true;
}

// Speaker and LEDs follow the state; only changes go to the board
// FM: the board's speaker path follows the squelch (or the monitor). DMR voice goes out as PCM from the vocoder
// (core/dmr/ambe routes the speaker itself); the green LED shows a call being played
static void outputs(radio_sm_t *sm)
{
	bool tx = sm->view.state == RS_TX;
	bool fm = sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot && !parked(&sm->view.ch);
	bool open = !tx && fm && (sm->view.monitor || sm->view.state == RS_RX || (sm->view.state == RS_TX_DENIED && sm->deny_rx));
	bool green = open;
#if CONFIG_P25
	if (sm->view.ch.mode == RADIO_MODE_P25 && !tx && sm->view.state == RS_RX &&
	    (sm->view.p25.flags & RADIO_P25_PLAYED) != 0) {
		green = true;
	}
#endif
#if CONFIG_NXDN
	if (sm->view.ch.mode == RADIO_MODE_NXDN && !tx && sm->view.state == RS_RX &&
	    (sm->view.nxdn.flags & RADIO_NXDN_PLAYED) != 0) {
		green = true;
	}
#endif
#if CONFIG_DMR
	if (!fm && !tx && sm->view.state == RS_RX && sm->view.dmr.state == RDMR_RX) {
		green = true;
	}
	if ((sm->hotspot || sm->sfr_rpt) && board.dmr != NULL && board.dmr->is_transmitting()) {
		tx = true;
	}
#endif
#if RADIO_SM_CW
	if (sm->cw_phase != 0) {
		tx = true;	// a CW ID
	}
#endif

	// The broadcast receiver plays instead of the channel receiver while the channel is a broadcast one
	bool bcast = false;
#if CONFIG_BCAST_FM
	bcast = sm->view.bcast;
	bool bcast_play = bcast && !tx && bcast_listening(sm);
	if (sm->bcast_on && sm->bcast_audio_out != (int8_t)((open || bcast_play) && bcast)) {
		sm->bcast_audio_out = (int8_t)((open || bcast_play) && bcast);
		board.bcast->audio((open || bcast_play) && bcast);
	}
#endif
	if (sm->audio_out != (int8_t)(open && !bcast)) {
		sm->audio_out = (int8_t)(open && !bcast);
		board.radio->audio(open && !bcast);
	}
#if CONFIG_DUAL_WATCH
	// The second receiver has the speaker while the selected channel is quiet: that one's calls always win
	if (board.rx2 != NULL) {
		int8_t on = (int8_t)(sm->rx2_on && !tx && !green);
		if (sm->rx2_audio_out != on) {
			sm->rx2_audio_out = on;
			board.rx2->audio(on != 0);
		}
	}
#endif
	bool red = tx;
	if (bcast) {
		green = false;	// a broadcast station playing isn't a call: the LED stays dark (after dual watch's use above)
	}
	led_signals(sm, &red, &green);
	if (sm->green_out != (int8_t)green) {
		sm->green_out = (int8_t)green;
		board.power->led(LED_GREEN, green);
	}
	if (sm->red_out != (int8_t)red) {
		sm->red_out = (int8_t)red;
		board.power->led(LED_RED, red);
	}
	sm->view.leds = (uint8_t)((green ? RADIO_LED_GREEN : 0u) | (red ? RADIO_LED_RED : 0u));
}

static void end_reception(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state == RS_RX) {
		sm->view.state = RS_IDLE;
		sm->view.last_rx_ms = now_ms != 0 ? now_ms : 1;
	}
	sm->close_count = 0;
}

// Receive on what view.ch says now
static void dmr_begin(radio_sm_t *sm);
static void dmr_end(radio_sm_t *sm);
#if CONFIG_AIR
// Not inlined: tune's frame is on the radio task's deepest path
static void air_stop(radio_sm_t *sm) __attribute__((noinline));
static void air_begin(radio_sm_t *sm) __attribute__((noinline));
static bool air_running(void);
#if CONFIG_SCAN
static uint8_t air_heard(const radio_sm_t *sm);
#endif
#endif

#if CONFIG_BCAST_FM
// The broadcast receiver on or off. Off mutes it first
static void bcast_power(radio_sm_t *sm, bool on)
{
	if (on == sm->bcast_on) {
		return;
	}
	if (on) {
		sm->bcast_on = board.bcast->power(true);
		sm->bcast_audio_out = -1;
		sm->bcast_volume_out = -1;
		return;
	}
	board.bcast->audio(false);
	board.bcast->power(false);
	sm->bcast_on = false;
}

// Broadcast or not for what view.ch says now (radio_bcast.h): on a broadcast channel the receiver is switched on and
// tuned; view.bcast_fault says it didn't answer. Never in hotspot mode, where the host link owns the radio
static void bcast_tune(radio_sm_t *sm)
{
	sm->view.bcast = !sm->view.am && !sm->hotspot && radio_bcast_channel(&sm->view.ch);
	if (!sm->view.bcast) {
		bcast_power(sm, false);
		sm->view.bcast_fault = false;
		return;
	}
	bcast_power(sm, true);
	sm->view.bcast_fault = !sm->bcast_on || !board.bcast->tune(radio_bcast_khz(sm->view.ch.rx_hz));
}

// The squelch and the meter on the broadcast receiver's signal strength; its volume follows the radio's
static void bcast_status(radio_sm_t *sm, radio_status_t *st)
{
	bcast_status_t bs;
	bool got = sm->bcast_on && board.bcast->status(&bs);
	uint8_t rssi = got ? bs.rssi : 0;
	sm->view.bcast_khz = got ? bs.khz : 0;
	st->rssi_dbm = (int16_t)(rssi + RADIO_BCAST_DBUV_DBM);
	st->noise = radio_bcast_noise(rssi);
	st->carrier = false;
	st->tone_ok = true;
	uint8_t vol = radio_bcast_volume(board.radio->volume != NULL ? sm->volume_sent : -1);
	if (sm->bcast_on && sm->bcast_volume_out != (int8_t)vol) {
		sm->bcast_volume_out = (int8_t)vol;
		board.bcast->volume(vol);
	}
}
#endif

static void tune(radio_sm_t *sm, uint32_t now_ms)
{
	sm->retune_pending = sm->retune_due = false;	// this tune is the one they wait for
#if CONFIG_SFR_CWID
	cw_cancel(sm);		// the repeater's ID: the board is set up for the channel below
#endif
	end_reception(sm, now_ms);
	if (radio_listen_band(sm->view.ch.rx_hz)) {
		// AM or broadcast there, whatever the channel says; the VFO stays FM when it leaves the band. The channel's
		// own mode follows, so FM there is not a temporary change (dual watch brings a pane back already FM)
		sm->view.ch.mode = RADIO_MODE_FM;
		if (sm->view.vfo)
			sm->vfo_ch.mode = RADIO_MODE_FM;
		else
			sm->base_mode = RADIO_MODE_FM;
		set_temporary(sm);
	}
#if CONFIG_AM_RX
	sm->view.am = radio_am_channel(&sm->view.ch);	// an FM channel in the air band: AM, no sub-audio
#endif
	reset_tone(sm);
#if CONFIG_BATTERY_SAVE
	sm->save_on = sm->save_asleep = false;	// rx() below wakes the receiver
	sm->view.rx_sleeping = false;
#endif
	radio_mode_t mode = (CONFIG_DMR && sm->view.ch.mode == RADIO_MODE_DMR && !parked(&sm->view.ch)) ?
		RADIO_MODE_DMR : RADIO_MODE_FM;
	// P25 and NXDN run with the board in its digital mode, the DMR stack stopped; a retune starts the engine over
	bool air = air_mode(sm->view.ch.mode);
	if (air) {
		mode = (radio_mode_t)sm->view.ch.mode;
	}
	// No sub-audio squelch in DMR, P25 or NXDN, whatever tone the channel or the VFO still holds from its FM settings
	static const subtone_t no_tone = { TONE_NONE, 0 };
	if (sm->board_mode == RADIO_MODE_DMR && mode != RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack (or an air mode) lets go of the modem before the chip leaves DMR
	}
#if CONFIG_AIR
	if (!air && air_running()) {
		air_stop(sm);		// to DMR: the engine closes and the vocoder's memory is back before DMR starts
	}
#endif
	radio_mode_t board_mode = air ? RADIO_MODE_DMR : mode;
	if (board_mode != sm->board_mode) {
		sm->board_mode = board_mode;
		board.radio->set_mode(board_mode);
	}
#if CONFIG_AM_RX
	if (sm->view.am && !board.radio->rx_am(sm->view.ch.rx_hz, (radio_bw_t)sm->view.ch.bw)) {
		sm->view.am = false;	// the board refused: FM as before (TX stays refused in the air band)
	}
#endif
#if CONFIG_BCAST_FM
	bcast_tune(sm);		// the channel receiver keeps its last frequency, muted, while the broadcast one plays
#endif
	if (!sm->view.am && !sm->view.bcast)
		board.radio->rx(sm->view.ch.rx_hz, air ? RADIO_BW_12K5 : (radio_bw_t)sm->view.ch.bw,
				mode != RADIO_MODE_FM ? &no_tone : &sm->view.ch.rx_tone);
	if (mode == RADIO_MODE_DMR) {
		dmr_begin(sm);		// after set_mode(DMR): the modem only starts in DMR mode
	}
#if CONFIG_AIR
	if (air) {
		air_begin(sm);
	}
#endif
	outputs(sm);
	changed(sm);
}

// From position start in z, the first readable channel going in dir (wraps once round the zone)
static bool find_channel(const zone_t *z, int start, int dir, uint16_t *pos, channel_t *ch)
{
	int n = z->count;
	for (int i = 0; i < n; i++) {
		int p = ((start + dir * i) % n + n) % n;
		if (channel_read(z->channels[p], ch)) {
			*pos = (uint16_t)p;
			return true;
		}
	}
	return false;
}

static void set_zone(radio_sm_t *sm, uint16_t index, const zone_t *z)
{
	if (z != &sm->zone) {
		sm->zone = *z;
	}
	sm->zone.name[CODEPLUG_NAME_LEN] = '\0';
	sm->view.zone_index = index;
	sm->view.zone_size = sm->zone.count;
	memcpy(sm->view.zone_name, sm->zone.name, sizeof(sm->view.zone_name));
}

#if CONFIG_DMR_TRUNK
static bool t3_channel(radio_sm_t *sm, uint16_t number, const channel_t *ch);
static void t3_off(radio_sm_t *sm);
#endif

// A channel's own settings into the view, temporary changes dropped (P7)
static void load_channel(radio_sm_t *sm, const channel_t *ch)
{
	sm->view.ch = *ch;
	sm->view.ch.name[CODEPLUG_NAME_LEN] = '\0';
#if !CONFIG_SUBTONE
	sm->view.ch.rx_tone = sm->view.ch.tx_tone = (subtone_t){ TONE_NONE, 0 };	// built without CTCSS / DCS
#endif
	sm->base_bw = sm->view.ch.bw;
	sm->base_rx_tone = sm->view.ch.rx_tone;
	sm->base_tx_tone = sm->view.ch.tx_tone;
	sm->base_mode = sm->view.ch.mode;
	sm->base_cc = sm->view.ch.color_code;
	sm->base_ts = sm->view.ch.timeslot;
	clear_temporary(sm);
	apply_tot(sm);
}

// A channel change
static void set_channel(radio_sm_t *sm, uint16_t pos, const channel_t *ch)
{
#if CONFIG_DMR_TRUNK
	// A trunked channel starts its system; any other leaves it. Another channel of the system running keeps the
	// engine where it is (a payload channel, another TSCC), not on the record's frequencies
	bool kept = t3_channel(sm, sm->zone.channels[pos], ch);
	uint32_t rx_hz = sm->view.ch.rx_hz, tx_hz = sm->view.ch.tx_hz;
	uint8_t cc = sm->view.ch.color_code, ts = sm->view.ch.timeslot;
#endif
	sm->view.vfo = false;
	sm->view.zone_pos = pos;
	sm->view.channel_number = sm->zone.channels[pos];
	load_channel(sm, ch);
#if CONFIG_DMR_TRUNK
	if (kept) {
		sm->view.ch.rx_hz = rx_hz;	// as t3_retune left them
		sm->view.ch.tx_hz = tx_hz;
		sm->view.ch.mode = RADIO_MODE_DMR;
		sm->view.ch.dmo = false;
		sm->view.ch.color_code = cc;
		sm->view.ch.timeslot = ts;
	}
	if (sm->t3_tagged && radio_t3v.unavail) {
		sm->view.ch.rx_only = true;	// a system that can't be used: the tag's transmit bit keys nothing on the
						// record's frequencies
	}
#endif
}

static bool read_zone(uint16_t index, zone_t *z)
{
	if (!codeplug_zone(index, z)) {
		return false;
	}
	if (z->count > CODEPLUG_ZONE_MAX) {
		z->count = CODEPLUG_ZONE_MAX;
	}
	return z->count > 0;
}

static uint32_t vfo_default_hz(void)
{
	for (unsigned i = 0; i < sizeof(vfo_start_hz) / sizeof(vfo_start_hz[0]); i++) {
		if (board_in_band(vfo_start_hz[i], true)) {
			return vfo_start_hz[i];
		}
	}
	for (uint8_t i = 0; i < board_band_count(); i++) {
		if (board.bands[i].tx) {
			uint32_t mid = board.bands[i].lo_hz + (board.bands[i].hi_hz - board.bands[i].lo_hz) / 2;
			return mid - mid % RADIO_VFO_STEP_HZ;
		}
	}
	return board_band_count() > 0 ? board.bands[0].lo_hz : 0;
}

// A new RX frequency. A split VFO keeps its offset within one transmit band (board_offset_carries); otherwise it is
// simplex
static void vfo_set_hz(radio_sm_t *sm, uint32_t hz)
{
	uint32_t tx = sm->vfo_ch.tx_hz - sm->vfo_ch.rx_hz + hz;
	if (!sm->vfo_seeded || !board_offset_carries(sm->vfo_ch.rx_hz, hz, tx))
		tx = hz;
	sm->vfo_ch.rx_hz = hz;
	sm->vfo_ch.tx_hz = tx;
	sm->vfo_seeded = true;
	if (sm->view.vfo) {
		sm->view.ch.rx_hz = hz;
		sm->view.ch.tx_hz = tx;
	}
}

static void enter_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->vfo_seeded) {
		bool keep = !sm->view.vfo && board_in_band(sm->view.ch.rx_hz, false);
		uint32_t hz = keep ? sm->view.ch.rx_hz : vfo_default_hz();
		vfo_set_hz(sm, hz);
	}
#if CONFIG_DMR_TRUNK
	t3_off(sm);
#endif
	sm->view.ch = sm->vfo_ch;
	sm->view.vfo = true;
	sm->view.channel_number = 0;
	clear_temporary(sm);
	apply_tot(sm);
	tune(sm, now_ms);
}

#if CONFIG_DUAL_WATCH
// VFO idx (0 or 1) becomes the one in use: vfo_ch and its talk target swap with vfo_other. The caller retunes when
// the radio is in the VFO
static void vfo_use(radio_sm_t *sm, uint8_t idx)
{
	idx &= 1u;
	if (idx == sm->vfo_idx) {
		return;
	}
	radio_vfo_t t = sm->vfo_other;
	sm->vfo_other.ch = sm->vfo_ch;
	sm->vfo_other.seeded = sm->vfo_seeded;
	sm->vfo_other.talk_id = sm->vfo_talk_id;
	sm->vfo_other.talk_private = sm->vfo_talk_private;
	sm->vfo_ch = t.ch;
	sm->vfo_seeded = t.seeded;
	sm->vfo_talk_id = t.talk_id;
	sm->vfo_talk_private = t.talk_private;
	sm->vfo_idx = idx;
	sm->view.vfo_index = idx;
}
#endif

radio_vfo_copy_t radio_vfo_copy_check(const channel_t *ch, bool in_vfo)
{
	if (!VFO_AVAILABLE) {
		return VFO_COPY_NO_VFO;
	}
	if (in_vfo) {
		return VFO_COPY_ALREADY;
	}
	if (ch->kind != CODEPLUG_KIND_NONE || (ch->mode == RADIO_MODE_DMR ? !CONFIG_VFO_DMR : !CONFIG_VFO_FM)) {
		return VFO_COPY_NO_MODE;	// the VFO holds plain FM and DMR only
	}
	if (!board_in_band(ch->rx_hz, false) || (ch->tx_hz != 0 && !board_in_band(ch->tx_hz, false))) {
		return VFO_COPY_OUT_OF_BAND;
	}
	return VFO_COPY_OK;
}

// The VFO's SFR role from byte A (sfr.h), as a channel's: role bits 0 = none, the flag clear. No byte B: the outbound
// color code is the VFO's own
static void vfo_sfr_set(channel_t *c, uint8_t a)
{
	bool on = (a & 0x07u) != 0;
	c->sfr_flag1 = on ? SFR_CHANNEL_FLAG1_ENABLED : 0;
	c->sfr_a = on ? a : 0;
	c->sfr_b = 0;
}

// Copy to VFO: the channel as it is in effect now (view.ch holds the temporary bandwidth, tones, mode, color code and
// timeslot; the temporary power is view.power; a talk target chosen for now counts, a one-shot or reply target does
// not) becomes the VFO's own settings, which are saved like any VFO change. Not copied, the VFO has nothing to hold
// them: the RX group list, the privacy key, roaming, data and emergency settings, the squelch level
static void vfo_copy(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *c = &sm->view.ch;
	if (radio_vfo_copy_check(c, sm->view.vfo) != VFO_COPY_OK) {
		return;
	}
	channel_t *v = &sm->vfo_ch;
	v->rx_hz = c->rx_hz;
	v->tx_hz = c->tx_hz != 0 && !c->dmo ? c->tx_hz : c->rx_hz;
	v->mode = c->mode;
	v->bw = c->bw;
	v->power = sm->temp_power ? sm->view.power : c->power;
	if (c->mode == RADIO_MODE_DMR) {
#if CONFIG_VFO_DMR
		uint8_t over = sm->one_shot ? 0 : sm->manual ? sm->manual_ts : sm->talk_ts;
		if (sm->temp_ts != 0) {
			over = sm->temp_ts;
		}
		v->color_code = c->color_code & 0x0Fu;
		v->timeslot = over != 0 ? over : (c->timeslot == 2 ? 2 : 1);
		sm->vfo_talk_id = sm->manual ? sm->manual_id : sm->talk_id;
		sm->vfo_talk_private = sm->manual ? sm->manual_private : sm->talk_private;
#endif
	} else {
		v->rx_tone = c->rx_tone;	// a DMR channel has no tones: the VFO keeps its own for its FM mode
		v->tx_tone = c->tx_tone;
	}
	// The channel's SFR role too (it acts on a simplex VFO only, as on a channel), with the VFO's color code; an FM
	// channel has none, so the VFO's old role doesn't come back at the next switch to DMR
	vfo_sfr_set(v, (c->sfr_flag1 & SFR_CHANNEL_FLAG1_ENABLED) ? c->sfr_a : 0);
	sm->vfo_seeded = true;
	enter_vfo(sm, now_ms);
}

static void leave_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	if (!sm->have_channels || !find_channel(&sm->zone, sm->view.zone_pos, 1, &pos, &ch)) {
		return;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Next band edge past hz in dir, wrapping round the board's bands
static uint32_t band_wrap(uint32_t hz, int dir)
{
	bool found = false;
	uint32_t best = 0;
	for (uint8_t i = 0; i < board_band_count(); i++) {
		const band_t *b = &board.bands[i];
		if (dir > 0 && b->lo_hz > hz && (!found || b->lo_hz < best)) {
			best = b->lo_hz;
			found = true;
		} else if (dir < 0 && b->hi_hz < hz && (!found || b->hi_hz > best)) {
			best = b->hi_hz;
			found = true;
		}
	}
	if (found) {
		return best;
	}
	for (uint8_t i = 0; i < board_band_count(); i++) {
		const band_t *b = &board.bands[i];
		uint32_t edge = dir > 0 ? b->lo_hz : b->hi_hz;
		if (!found || (dir > 0 ? edge < best : edge > best)) {
			best = edge;
			found = true;
		}
	}
	return best;
}

static void vfo_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	step_update(sm);
#if CONFIG_AM_RX
	int64_t hz = radio_am_step(sm->view.ch.rx_hz, dir, sm->view.vfo_step_hz);	// the 8.33 kHz grid kept exact
#else
	int64_t hz = (int64_t)sm->view.ch.rx_hz + (int64_t)dir * sm->view.vfo_step_hz;
#endif
#if CONFIG_BCAST_FM
	if (sm->view.bcast) {
		hz = radio_bcast_step(sm->view.ch.rx_hz, dir, sm->set.bcast_step);	// 200 kHz lands on the odd tenths
	}
#endif
	uint32_t next;
	if (hz > 0 && hz <= (int64_t)UINT32_MAX && board_in_band((uint32_t)hz, false)) {
		next = (uint32_t)hz;
	} else {
		next = band_wrap(sm->view.ch.rx_hz, dir);
	}
	vfo_set_hz(sm, next);
	tune(sm, now_ms);
}

// A trunked channel whose system isn't set up (dmr-trunking-codeplug.md 4): stepping passes it by
static bool channel_locked(const channel_t *ch)
{
#if CONFIG_DMR_TRUNK
	return ch->kind == CODEPLUG_KIND_DMR_TRUNK && !radio_t3_usable(ch->system);
#else
	(void)ch;
	return false;
#endif
}

static void channel_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	int start = sm->view.zone_pos + dir;
	for (int i = 0;; i++) {
		if (i >= sm->zone.count || !find_channel(&sm->zone, start, dir, &pos, &ch)) {
			return;		// nothing else in the zone the radio can use
		}
		if (!channel_locked(&ch)) {
			break;
		}
		start = pos + dir;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Something on the channel by the noise reading, at the squelch level (the default level when the squelch is open)
static bool carrier_heard(const radio_sm_t *sm, uint8_t noise)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	return noise < radio_sm_squelch_open_below(sq);
}

// Busy channel lockout (FM): refuse while the channel is in use (by anyone, or by a group with another tone)
static bool bcl_busy(const radio_sm_t *sm)
{
	if (sm->view.ch.mode != RADIO_MODE_FM || sm->set.bcl == RADIO_BCL_OFF) {
		return false;
	}
	bool carrier = sm->view.state == RS_RX || carrier_heard(sm, sm->view.noise);
	if (sm->set.bcl == RADIO_BCL_CARRIER) {
		return carrier;
	}
	return carrier && tone_needed(sm) && !sm->view.tone_ok;
}

#if CONFIG_SFR
// Asked by the board as it keys each DMR burst, from the RF TX interrupt (hal_radio_t.dmr_direct_query): an SFR repeats
// right now, our repeater role or the hotspot's SFR mode (not a split role: hotspot_sfr_repeating). It reads the state
// itself, single flags, so the answer is never a stale copy
static radio_sm_t *direct_sm;

static bool sfr_repeats_now(void)
{
	const volatile radio_sm_t *sm = direct_sm;
	if (sm == NULL) {
		return false;
	}
	bool on = sm->sfr_rpt;
#if CONFIG_HOTSPOT
	on = on || (sm->hotspot && hotspot_sfr_repeating());
#endif
	return on;
}
#endif

// Every keying of the transmitter goes through here: the test TX limit (tx_limit.h) refuses what it doesn't allow, as
// a board that can't key there would
static bool key_tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	if (!tx_limit_allows(hz)) {
		tx_limit_refused(hz);
		return false;
	}
	return board.radio->tx(hz, bw, power, tone);
}

static uint8_t tx_check(const radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	// A trunked channel's frequencies are the system's: checked when it keys (p25t_tick, p25t_burst)
	bool trunk = ch->kind == CODEPLUG_KIND_P25_TRUNK;
	if (sm->set.tx_inhibit || (!trunk && !tx_limit_allows(ch->tx_hz))) {
		return TXDENY_INHIBIT;		// the test TX limit denies PTT as the inhibit does
	}
#if CONFIG_AM_RX
	if (sm->view.am || radio_am_tx_refused(ch->tx_hz)) {
		return TXDENY_AM;	// the air band is receive only, whatever the codeplug says
	}
#endif
#if CONFIG_BCAST_FM
	if (sm->view.bcast || radio_bcast_tx_refused(ch->tx_hz)) {
		return TXDENY_BCAST;	// so is the broadcast band
	}
#endif
	if (parked(ch)) {
		return TXDENY_MODE;	// before receive only: the record underneath says so, the mode is the reason
	}
	if (trunk) {
		// Its tag's receive only, the system's listen only, or no follower with a transmit side (p25-trunking.md 7)
#if CONFIG_P25_TRUNK
		if (ch->rx_only || !radio_p25_trunk_tx()) {
			return TXDENY_RX_ONLY;
		}
#else
		return TXDENY_RX_ONLY;
#endif
	} else if (ch->tx_hz == 0) {
		return TXDENY_NO_FREQ;
	} else if (ch->rx_only) {
		return TXDENY_RX_ONLY;
	}
	if (ch->mode == RADIO_MODE_DMR ? !CONFIG_DMR : ch->mode == RADIO_MODE_P25 ? !CONFIG_P25 :
	    ch->mode == RADIO_MODE_NXDN ? !CONFIG_NXDN : !CONFIG_FM) {
		return TXDENY_MODE;
	}
	if (!trunk && !board_in_band(ch->tx_hz, true)) {
		return TXDENY_BAND;
	}
	if (board_hwopt_pending() != NULL) {
		// Unset, or stored but not applied until a restart: the transmitter may be set up for the wrong hardware
		return TXDENY_HWOPT;
	}
	if (sm->batt.level == BATT_CRITICAL) {
		return TXDENY_BATTERY;
	}
	if (bcl_busy(sm)) {
		return TXDENY_BUSY;
	}
	return TXDENY_NONE;
}

// The board didn't key a frequency tx_check passed: a hardware option it still needs, or its own refusal (no
// calibration there, for one), never the band
static uint8_t board_refusal(void)
{
	return board_hwopt_pending() != NULL ? TXDENY_HWOPT : TXDENY_REFUSED;
}

// A busy refusal over a call the user hears is quiet: the screen says "Channel busy", and no tone covers the call. On
// FM that call plays on while PTT is held (deny_rx, set by ptt_down). With nothing playing (a wrong-tone carrier
// behind a closed squelch, a DMR call not ours) the tone is the only sign, so it plays
static void deny(radio_sm_t *sm, uint8_t reason)
{
	bool audible = sm->deny_rx;
#if CONFIG_DMR
	audible = audible || (sm->view.ch.mode == RADIO_MODE_DMR && sm->view.dmr.state == RDMR_RX);
#endif
	if (reason != TXDENY_BUSY) {
		sm->deny_rx = false;
	}
	sm->view.state = RS_TX_DENIED;
	sm->view.tx_deny = reason;
	sm->view.tot_warning = false;
	deny_led_start(sm);
	outputs(sm);
	changed(sm);
	if (reason != TXDENY_BUSY || !audible) {
		beep(sm, BEEP_TX_DENY);
	}
}

// The TX timer in the view, and the warning tot_warn_s before the time-out (FM and DMR)
static void tx_timer(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t elapsed = now_ms - sm->tx_start_ms;
	if (elapsed / 1000 != sm->view.tx_elapsed_ms / 1000) {
		changed(sm);
	}
	sm->view.tx_elapsed_ms = elapsed;
	uint16_t tot = sm->view.tot_s, warn = sm->set.tot_warn_s;
	if (tot != 0 && warn != 0 && tot > warn && !sm->tot_warned && elapsed >= (uint32_t)(tot - warn) * 1000u) {
		sm->tot_warned = true;
		sm->view.tot_warning = true;
		changed(sm);
		beep(sm, BEEP_TOT_WARN);
	}
}

// --- DMR: the call stack on DMR channels (core/dmr/call), voice through core/dmr/ambe ---
#if CONFIG_DMR
static void add_name(radio_sm_t *sm, const contact_t *c)
{
	if (sm->name_count >= RADIO_NAMES_MAX || c->name[0] == '\0') {
		return;
	}
	radio_name_t *n = &sm->names[sm->name_count++];
	n->id = c->id;
	n->group = c->type != CONTACT_PRIVATE;
	memcpy(n->name, c->name, sizeof(n->name));
}

// A name for an ID: the channel's contacts (talk target, listen list), then the lookup cache, then every codeplug
// contact and, for a private ID, the DMR ID database (both read from flash once per ID, the answer cached, "" too)
static void find_name(radio_sm_t *sm, uint32_t id, bool group, char *out)
{
	out[0] = '\0';
	if (id == 0) {
		return;
	}
	for (uint8_t i = 0; i < sm->name_count; i++) {
		if (sm->names[i].id == id && sm->names[i].group == group) {
			memcpy(out, sm->names[i].name, CODEPLUG_NAME_LEN + 1);
			return;
		}
	}
	for (uint8_t i = 0; i < RADIO_LOOKUPS; i++) {
		if (sm->lookups[i].id == id && sm->lookups[i].group == group) {
			memcpy(out, sm->lookups[i].name, CODEPLUG_NAME_LEN + 1);
			return;
		}
	}
	radio_name_t *n = &sm->lookups[sm->lookup_next];
	sm->lookup_next = (uint8_t)((sm->lookup_next + 1u) % RADIO_LOOKUPS);
	n->id = id;
	n->group = group;
	n->name[0] = '\0';
	contact_t c;
	if (codeplug_contact_find(id, group, &c)) {
		memcpy(n->name, c.name, sizeof(n->name));
	} else if (!group) {
		char text[DMR_IDDB_TEXT_MAX + 1];
		if (dmr_iddb_lookup(id, text, sizeof(text))) {
			memcpy(n->name, text, CODEPLUG_NAME_LEN);
			n->name[CODEPLUG_NAME_LEN] = '\0';
		}
	}
	memcpy(out, n->name, CODEPLUG_NAME_LEN + 1);
}

// The channel's talk target and listen list from the codeplug contacts, with their names for the screens
static void dmr_load_channel(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	contact_t c;
	sm->name_count = 0;
	sm->listen_count = 0;
	sm->talk_id = 9;		// no contact: local talkgroup 9, the usual default
	sm->talk_private = false;
	sm->talk_ts = 0;
	if (sm->manual && (sm->manual_vfo != sm->view.vfo || sm->manual_ch != sm->view.channel_number)) {
		sm->manual = false;	// a chosen target lasts as long as its channel (temporary state, docs/ux)
	}
	if (sm->hear_all_latch && (sm->latch_vfo != sm->view.vfo || sm->latch_ch != sm->view.channel_number)) {
		sm->hear_all_latch = false;	// so does the hear all latch
	}
	if (sm->one_shot && (sm->view.vfo || sm->one_shot_ch != sm->view.channel_number)) {
		sm->one_shot = false;		// and a one-shot target
	}
	if (sm->view.vfo) {
		// The VFO's own target, kept with its other settings. It has no RX group list, so it hears its talkgroup,
		// all call and private calls to us, plus everything with digital monitor (docs/spec/dmr-voice.md 2)
		if (sm->vfo_talk_id != 0) {
			sm->talk_id = sm->vfo_talk_id;
			sm->talk_private = sm->vfo_talk_private;
		}
		return;
	}
	uint16_t talk = codeplug_channel_talk_contact(ch);
	if (talk != 0 && codeplug_contact(talk, &c)) {
		sm->talk_id = c.id;
		sm->talk_private = c.type == CONTACT_PRIVATE;
		sm->talk_ts = c.timeslot;
		add_name(sm, &c);
	}
	static listen_list_t list;	// only the radio task uses it
	if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &list)) {
		for (uint8_t i = 0; i < list.count; i++) {
			if (!codeplug_contact(list.contacts[i], &c)) {
				continue;
			}
			if (c.type != CONTACT_PRIVATE && sm->listen_count < CODEPLUG_LIST_LEN) {
				sm->listen[sm->listen_count++] = c.id;
			}
			add_name(sm, &c);
		}
	}
}

#if CONFIG_DMR_PRIVACY
static dmrPrivacyKey_t privacy_key;	// the call stack's privacy hook context (radio task only)
#endif

// Hang time (ms) as the call stack's reply window (whole seconds, rounded up); 0 keeps the stack's default
static uint8_t hang_s(uint16_t ms, uint8_t dflt)
{
	if (ms == 0) {
		return dflt;
	}
	uint32_t s = (ms + 999u) / 1000u;
	return (uint8_t)(s > 30u ? 30u : s);
}

// The talk permit before keying (CAP_BEEP_NOT_ON_AIR): the hold's minimum covers the beep's start (beep_busy is true
// from beep_play on); the hold lasts while the beep plays or its audio is still in the board's buffers, so none of it
// is left to reach the modulator
#define PERMIT_HOLD_MIN_MS	40u
// Longest our DMR end tone waits for the audio path to open after the transmission (the engine's last burst)
#define DMR_END_WAIT_MS		400u

static bool permit_busy(void)
{
#if CONFIG_BEEP
	const hal_audio_t *a = board.audio;
	return beep_busy() || (a != NULL && a->spk_pending != NULL && a->spk_pending());
#else
	return false;
#endif
}

// Whether the DMR talk permit sounds on the speaker: the radio's own tone when the setting is the default (and the
// talk permit is on), else the start of transmission tone's sequence when it is set to sound there
static bool permit_local(const radio_sm_t *sm)
{
	return sig_out(sm, SIG_EV_BEGIN) == SIG_OUT_DEFAULT ? sm->set.talk_permit : sig_local_on(sm, SIG_EV_BEGIN);
}

static void permit_play(radio_sm_t *sm, uint8_t b)
{
	if (sm->view.ch.no_beep || !permit_local(sm)) {
		return;
	}
	if (sig_out(sm, SIG_EV_BEGIN) == SIG_OUT_DEFAULT) {
		beep(sm, b);
	} else {
		sig_local(sm, SIG_EV_BEGIN);
	}
}

// Whether the talk permit sounds before keying on this channel: the board can't play it on air, and it is on
static bool permit_before_key(const radio_sm_t *sm)
{
	return CONFIG_BEEP && (board.caps & CAP_BEEP_NOT_ON_AIR) != 0 && permit_local(sm) && !sm->view.ch.no_beep;
}

// Digital monitor on now: latched on this channel, or carried by a zone or VFO scan started with it on, so every
// stop hears every talkgroup and a call on any of them holds the scan (#595)
static bool hear_all_on(const radio_sm_t *sm)
{
#if CONFIG_SCAN
	if (sm->scan_hear_all && scan_on(&sm->scan) && sm->scan.kind != SCAN_DUAL) {
		return true;
	}
#endif
	return sm->hear_all_latch;
}

static void dmr_configure(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = ch->dmr_id != 0 ? ch->dmr_id : sm->own_id;
	cfg.cc = ch->color_code & 0x0Fu;
	// The talk target: a one-shot target (the focused row PTT went to), else one chosen for now, else the channel's.
	// A timeslot chosen for now beats a contact's timeslot override
	uint8_t ts_override = sm->one_shot ? 0 : sm->manual ? sm->manual_ts : sm->talk_ts;
	if (sm->temp_ts != 0) {
		ts_override = sm->temp_ts;
	}
	cfg.ts = ts_override != 0 ? ts_override : (ch->timeslot == 2 ? 2 : 1);
	cfg.rmo = ch->tx_hz != 0 && ch->rx_hz != ch->tx_hz && !ch->dmo;
#if CONFIG_SFR
	// A user of a single frequency repeater: the inbound slot and its sync, the copies heard (sfr.md 4)
	sfrConfig_t sc;
	sm->view.dmr.sfr_role = radio_sfr_channel(ch, &sc);
	sm->view.dmr.sfr_inbound = sc.inboundSlot;
	if (sfrRoleIsUser(sm->view.dmr.sfr_role) && !cfg.rmo) {
		cfg.sfr_user = true;
		radio_sfr_user(&sc, &cfg.ts, &cfg.tx_sync, &cfg.rx_sync, &cfg.cc2);
		cfg.sfr_rx = sm->set.sfr_rx;
	}
#endif
	cfg.talk_target.id = sm->one_shot ? sm->one_shot_id : sm->manual ? sm->manual_id : sm->talk_id;
	cfg.talk_target.private_call = sm->one_shot ? sm->one_shot_private : sm->manual ? sm->manual_private :
		sm->talk_private;
	cfg.listen.hear_all = sm->view.monitor || hear_all_on(sm);
	// Digital monitor hears everything on the frequency: any color code, either timeslot (dmr-voice.md 1)
	cfg.listen.any_cc = cfg.listen.both_ts = cfg.listen.hear_all;
	cfg.listen.groups = sm->listen;
	cfg.listen.group_count = sm->listen_count;
	// The channel's TX time-out when it has one, else the radio's
	cfg.tot_s = sm->view.tot_s;	// apply_tot: the channel's, else the radio's
	if (sm->set.reply_s <= 30u) {
		cfg.reply_window_s = cfg.reply_window_private_s = sm->set.reply_s;	// the radio's setting
	} else {
		cfg.reply_window_s = hang_s(sm->group_hang_ms, cfg.reply_window_s);
		cfg.reply_window_private_s = hang_s(sm->private_hang_ms, cfg.reply_window_s);
	}
	sm->win_group_s = cfg.reply_window_s;
	sm->win_private_s = cfg.reply_window_private_s != 0 ? cfg.reply_window_private_s : cfg.reply_window_s;
#if CONFIG_DMR_GPS
	// The channel's own position setting for its timeslot (the codeplug): GPS Info in its calls with the radio-wide
	// setting off too
	sm->view.dmr.gps_tx_channel = !sm->view.vfo &&
		(ch->alias_tx & (cfg.ts == 2 ? CODEPLUG_ALIAS_TS2_GPS : CODEPLUG_ALIAS_TS1_GPS)) != 0;
#endif
	bool alias = false;
#if CONFIG_DMR_TALKER_ALIAS
	alias = sm->radio_name[0] != '\0' && !sm->view.vfo &&
		(ch->alias_tx & (cfg.ts == 2 ? CODEPLUG_ALIAS_TS2_TEXT : CODEPLUG_ALIAS_TS1_TEXT)) != 0;
	if (alias) {
		memcpy(cfg.tx_alias, sm->radio_name, sizeof(sm->radio_name));
		cfg.tx_alias_format = DMR_TA_ISO8;
	}
#endif
	bool privacy = false;
#if CONFIG_DMR_PRIVACY
	privacy = sm->privacy_type != DMR_PRIVACY_OFF;
	if (privacy) {
		privacy_key.type = (dmrPrivacyType_t)sm->privacy_type;
		privacy_key.key = sm->privacy_key;
		cfg.privacy = dmrPrivacyCallHook;
		cfg.privacy_ctx = &privacy_key;
		cfg.tx_fid = dmrPrivacyFID(privacy_key.type, cfg.tx_fid);
		cfg.tx_svc = dmrPrivacyServiceOptions(privacy_key.type, cfg.tx_svc);
	}
#endif
#if CONFIG_DMR_EMERGENCY
	// In emergency: every call carries the emergency service option, goes to the emergency target, and doesn't wait
	// for a busy channel (docs/spec/dmr-emergency.md 3)
	if (dmr_emergency_in_progress()) {
		uint32_t id;
		bool priv;
		dmr_emergency_target(&id, &priv);
		if (id != 0) {
			cfg.talk_target.id = id;
			cfg.talk_target.private_call = priv;
		}
		cfg.tx_svc |= DMR_SVC_EMERGENCY;
		cfg.access = DMR_ACCESS_IMPOLITE;
	}
	sm->em_cfg_serial = dmr_emergency_config_serial();
#endif
	if (permit_before_key(sm)) {
		cfg.permit_hold_ms = PERMIT_HOLD_MIN_MS;
		cfg.permit_busy = permit_busy;
	}
#if CONFIG_DMR_TRUNK
	// A trunked system: both slots on the control channel; on a payload channel the engine chose the call, so it
	// plays whatever its talkgroup, and PTT talks to it
	if (sm->t3_on) {
		cfg.own_id = t3s.cfg.own_id;	// the system's ID: calls to it, and the source of ours
		cfg.both_slots = sm->t3_both;
		if (t3m.state == T3_FOLLOW) {
			const t3_call_t *c = &t3m.call;
			cfg.listen.hear_all = true;
			cfg.talk_target.private_call = !c->group;
			cfg.talk_target.id = c->group || c->src == cfg.own_id ? c->dst : c->src;
			// 361-2 5.3.2.1, 5.3.3.1: a broadcast or all-call says so in its LC, whatever grant came back (a
			// TV_GRANT to an all-unit ID is one too, table 7.11)
			if (c->ours && (c->op == T3_OP_BTV_GRANT || (c->group && c->dst >= T3_ID_ALLMSIDL))) {
				cfg.tx_svc |= DMR_SVC_BROADCAST;
			}
		}
		cfg.reply_window_s = cfg.reply_window_private_s = 0;
	}
#endif
	dmr_call_configure(&cfg);
#if CONFIG_SIG_TONES
	sm->dmr_end_wait = false;	// a pending end tone belongs to the channel it was sent on
#endif
	sm->view.dmr.own_id = cfg.own_id;
	// The channel's color code, timeslot and repeater mode in the same update as the channel: dmr_publish copies the
	// stack's own only at the next tick, and a reader between the two (dual watch's snapshot at each hop) would take
	// the old channel's timeslot for the new one
	sm->view.dmr.cc = cfg.cc;
	sm->view.dmr.ts = cfg.ts;
	sm->view.dmr.rmo = cfg.rmo;
	sm->view.dmr.hear_all = cfg.listen.hear_all;
	sm->view.dmr.hear_all_latch = hear_all_on(sm);
	sm->view.dmr.one_shot = sm->one_shot;
	sm->view.dmr.target_manual = sm->manual;
	// The target with its flags in one update (dmr_publish copies the stack's own at the next tick): a reader never
	// sees the new manual / one-shot flag beside the old target
	sm->view.dmr.target_id = cfg.talk_target.id;
	sm->view.dmr.target_private = cfg.talk_target.private_call;
	find_name(sm, cfg.talk_target.id, !cfg.talk_target.private_call, sm->view.dmr.target_name);
	sm->view.dmr.alias_tx = alias;
	sm->view.dmr.privacy = privacy;
}

// The VFO's talk target (typed, a contact): a setting of the VFO, kept with it, and in force from now on
static void vfo_set_talk(radio_sm_t *sm, uint32_t id, bool private_call)
{
	sm->vfo_talk_id = id;
	sm->vfo_talk_private = private_call;
	sm->talk_id = id;
	sm->talk_private = private_call;
	sm->one_shot = false;		// an explicit choice replaces a one-shot or call back target
	sm->manual = false;
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;	// publish the new target at the next tick
	}
	changed(sm);
}

// A talk target (typed, a contact, a call back): reconfigures the stack, which ends the reply window. On a channel
// it is for now; in the VFO a typed or picked one (keep) is the VFO's own target, a call back is for now
static void dmr_set_manual(radio_sm_t *sm, uint32_t id, bool private_call, uint8_t ts, bool keep)
{
	if (id == 0 || id > DMR_ID_MAX) {
		return;
	}
	if (sm->view.vfo && keep) {
		if (CONFIG_VFO_DMR) {
			vfo_set_talk(sm, id, private_call);
		}
		return;
	}
	if (sm->view.ch.mode != RADIO_MODE_DMR) {
		return;
	}
	sm->one_shot = false;		// an explicit choice replaces a one-shot target
	sm->manual = true;
	sm->manual_id = id;
	sm->manual_private = private_call;
	sm->manual_ts = ts;
	sm->manual_ch = sm->view.channel_number;
	sm->manual_vfo = sm->view.vfo;
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;	// publish the new target at the next tick
	}
	changed(sm);
}

// The stack follows a change of the channel's DMR settings (temporary color code, timeslot, hear all)
static void dmr_reconfigure(radio_sm_t *sm)
{
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;	// publish at the next tick
	}
	changed(sm);
}

// Digital monitor (the hear all latch: every talkgroup on the slot and color code) on or off, for this channel, and
// during a scan for the rest of the scan (RREQ_DMR_HEAR_ALL, and the monitor key's press on DMR)
static void set_hear_all_latch(radio_sm_t *sm, bool on)
{
	if (on == hear_all_on(sm)) {
		return;
	}
	sm->hear_all_latch = on;
	sm->scan_hear_all = on;
	sm->latch_ch = sm->view.channel_number;
	sm->latch_vfo = sm->view.vfo;
	sm->view.dmr.hear_all_latch = on;
	dmr_reconfigure(sm);
}

// A timeslot for now (RREQ_DMR_TS). The slot the radio would use without it (a contact's override, else the
// channel's) clears it again
static void dmr_set_timeslot(radio_sm_t *sm, int32_t ts, bool busy)
{
	if (sm->view.ch.mode != RADIO_MODE_DMR || (ts != 1 && ts != 2)) {
		return;
	}
	if (busy) {
		// A data burst list (ARS after a slot change, a position) is not the user's transmission: the change waits
		// for its end instead of being lost (a TS2 call went out on TS1, 2026-10-27)
		if (sm->view.state == RS_TX && sm->tx_no_ptt && !sm->hotspot) {
			sm->ts_pending = (uint8_t)ts;
		}
		return;
	}
	sm->ts_pending = 0;
	if (sm->view.vfo) {
		// The VFO's own slot, not a temporary change
		if (ts != sm->view.ch.timeslot) {
			sm->view.ch.timeslot = (uint8_t)ts;
			sm->vfo_ch.timeslot = (uint8_t)ts;
			dmr_reconfigure(sm);
		}
		return;
	}
	uint8_t over = sm->one_shot ? 0 : sm->manual ? sm->manual_ts : sm->talk_ts;
	uint8_t natural = over != 0 ? over : (sm->base_ts == 2 ? 2 : 1);
	uint8_t want = ts == natural ? 0 : (uint8_t)ts;
	if (want == sm->temp_ts) {
		return;
	}
	sm->temp_ts = want;
	sm->view.ch.timeslot = want != 0 ? want : sm->base_ts;
	set_temporary(sm);
	dmr_reconfigure(sm);
}

#if CONFIG_SFR

// The repeater role lets go of the modem and the keying
static void sfr_rpt_end(radio_sm_t *sm)
{
	if (!sm->sfr_rpt) {
		return;
	}
	radio_sfr_stop();
	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->sfr_rpt = false;
	sm->view.dmr.sfr_rpt = false;
	sm->view.dmr.sfr_keyed = false;
	sm->view.dmr.sfr_busy = false;
}

// The repeater role waits while the radio may not transmit (TX inhibit, the battery, the band) and while a scan or dual
// watch runs: they visit the channel to listen, and their activity check reads the call stack
static bool sfr_rpt_wait(radio_sm_t *sm)
{
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return true;
	}
#endif
	return tx_check(sm) != TXDENY_NONE;
}

#if CONFIG_SFR_CWID
#define SFR_RPT_RESUMING(sm)	((sm)->cw_sfr)	// back from its CW ID: the same role, its counters carry on
#else
#define SFR_RPT_RESUMING(sm)	false
#endif

// A channel whose SFR role is a repeater: the runner takes the modem instead of the call stack, and the keying is armed
// on the channel's frequency for the modem's per-burst keying, as in hotspot mode. Not while it must wait (sfr_rpt_wait),
// or when the board won't key: then the channel is an ordinary one. False: the call stack's channel
static bool sfr_rpt_begin(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	const channel_t *ch = &sm->view.ch;
	sfrConfig_t sc;

	sm->view.dmr.sfr_role = radio_sfr_channel(ch, &sc);
	sm->view.dmr.sfr_inbound = sc.inboundSlot;
	sfr_rpt_end(sm);
	sm->sfr_denied = false;
	if (!sfrRoleIsRepeater(sm->view.dmr.sfr_role) || board.dmr == NULL) {
		return false;
	}
	if (sfr_rpt_wait(sm)) {
		sm->sfr_denied = true;	// radio_sm_tick starts the role once it may
		return false;
	}
	if (sm->dmr_running) {
		dmr_call_ptt(false);
		dmr_call_stop();
		ambeVoiceStop();
		sm->dmr_running = false;
	}
	if (!radio_sfr_start(board.dmr, &sc, sm->set.rpt_beacon ? sm->set.roam_beacon_s : 0, sm->set.roam_beacon_len_s,
			     SFR_RPT_RESUMING(sm), 0)) {	// the beacon schedule starts at the first poll
		return false;
	}
	sm->dmr_keyed = key_tx(ch->tx_hz, RADIO_BW_12K5, sm->view.power, &none);
	if (!sm->dmr_keyed) {
		radio_sfr_stop();	// the board refused the keying: no repeater that can't send
		return false;
	}
#if CONFIG_SFR_CWID
	if (!sm->cw_sfr) {
		memset(&sm->cwid, 0, sizeof(sm->cwid));	// a new role (not back from its ID): the first ID after its first
	}						// transmission
#endif
	sm->sfr_rpt = true;
	sm->view.dmr.sfr_rpt = true;
	sm->view.dmr.active = false;
	return true;
}
#endif

// On a DMR channel, after set_mode(DMR) and rx(): configure, and start the stack if it isn't running
static void dmr_begin(radio_sm_t *sm)
{
	if (sm->dmr_killed) {
		sm->view.dmr.active = false;	// killed (6.4.10.0): no DMR at all, conventional or trunked, PTT denied
		return;
	}
#if CONFIG_SFR
	if (sfr_rpt_begin(sm)) {
		return;
	}
#endif
	// A new frequency with the stack running: the receiver drops the slot timing it took on the last one and searches
	// again. Without it a modem may keep that timing and decode nothing from another transmitter until its own stale
	// restart, 390-600 ms later: longer than a scan's dwell (gotchas 17.2). A start (set_mode) searches anyway
	if (sm->dmr_running && sm->dmr_rx_hz != sm->view.ch.rx_hz) {
		board.dmr->rx_restart();
	}
	sm->dmr_rx_hz = sm->view.ch.rx_hz;
	dmr_load_channel(sm);
	dmr_configure(sm);
	if (!sm->dmr_running && board.dmr != NULL) {
		sm->dmr_running = dmr_call_start();
	}
	sm->view.dmr.active = sm->dmr_running;
	sm->dmr_serial = dmr_call_view()->serial - 1u;	// copy the stack's view at the next tick
}

// A split or color code change in the VFO, whose SFR role reads both (radio_sfr_channel): the role is looked at again,
// so a repeater stops on a split and starts on one frequency, and takes a new color code
static void dmr_reconfigure_sfr(radio_sm_t *sm)
{
#if CONFIG_SFR
	if (sm->view.vfo && sm->view.ch.mode == RADIO_MODE_DMR && (sm->sfr_rpt || sm->view.ch.sfr_flag1 != 0)) {
		dmr_begin(sm);
		changed(sm);
		return;
	}
#endif
	dmr_reconfigure(sm);
}

// Leaving DMR (or into hotspot mode): drop any transmission, stop the stack and the PCM stream
static void dmr_end(radio_sm_t *sm)
{
#if CONFIG_AIR
	air_stop(sm);
#endif
#if CONFIG_SFR
	sfr_rpt_end(sm);
#endif
	dmr_call_ptt(false);
	dmr_call_stop();
	ambeVoiceStop();
	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->dmr_running = false;
#if CONFIG_SFR
	sm->view.dmr.sfr_role = SFR_ROLE_OFF;	// no role in force outside DMR (the VFO keeps its stored one in FM)
	sm->view.dmr.sfr_inbound = 0;
#endif
	sm->manual = false;		// a chosen talk target ends with the channel
	sm->view.dmr.target_manual = false;
	sm->one_shot = false;
	sm->view.dmr.one_shot = false;
	sm->hear_all_latch = false;	// and so does the hear all latch
	sm->view.dmr.hear_all_latch = false;
	sm->view.dmr.hear_all = sm->view.monitor;
	memset(&sm->view.dmr.call, 0, sizeof(sm->view.dmr.call));
	sm->view.dmr.active = false;
	sm->view.dmr.state = RDMR_IDLE;
	sm->view.dmr.reply_active = false;
	sm->view.dmr.alias[0] = '\0';
}

static uint8_t dmr_deny_reason(uint8_t err)
{
	switch (err) {
	case DMR_TX_ERR_BUSY: return TXDENY_BUSY;
	case DMR_TX_ERR_NO_REPEATER: return TXDENY_NO_REPEATER;
	case DMR_TX_ERR_REJECTED:
	case DMR_TX_ERR_STALLED: return TXDENY_REJECTED;	// stalled: the modem never sent a burst
	case DMR_TX_ERR_TIMEOUT: return TXDENY_TIMEOUT;
	case DMR_TX_ERR_CEASED: return TXDENY_CEASED;
	default: return TXDENY_NO_DMR;
	}
}

static bool dmr_tx_phase(uint8_t phase)
{
	return phase == DMR_CALL_PHASE_TX_ACCESS || phase == DMR_CALL_PHASE_TX_WAKEUP ||
		phase == DMR_CALL_PHASE_TX_VOICE || phase == DMR_CALL_PHASE_TX_ENDING || phase == DMR_CALL_PHASE_TX_DATA ||
		phase == DMR_CALL_PHASE_TX_PERMIT;
}

#if CONFIG_DMR_GPS
// Our position for DMR GPS Info (docs/spec/dmr-voice.md): the position service's (GPS, host, fixed). send: the
// "send position in DMR calls" setting
static bool own_position(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *send, uint32_t now_ms)
{
#if CONFIG_POSITION
	return position_now(lat, lon, err_m, send, now_ms);
#else
	(void)lat;
	(void)lon;
	(void)err_m;
	(void)now_ms;
	*send = false;
	return false;	// built without a position source
#endif
}

// At PTT: our GPS Info for this transmission, or none. It goes out when the radio-wide setting or the channel's own
// (the codeplug) asks for it
static void dmr_gps_tx(const radio_sm_t *sm, uint32_t now_ms)
{
	int32_t lat, lon;
	uint32_t err_m;
	bool send;
	if (own_position(&lat, &lon, &err_m, &send, now_ms) && (send || sm->view.dmr.gps_tx_channel)) {
		dmr_gps_t g = { .err = dmr_gps_err_code(err_m), .lat = lat, .lon = lon };
		dmr_call_set_tx_gps(&g);
	} else {
		dmr_call_set_tx_gps(NULL);
	}
}
#endif

static void copy_call(radio_sm_t *sm, const dmr_caller_t *c, radio_dmr_call_t *out, uint32_t now_ms)
{
	out->src = c->src;
	out->dst = c->dst;
	out->group = c->group;
	out->to_us = c->to_us;
	out->encrypted = c->encrypted;
	out->emergency = c->emergency;
	out->pi_header = c->pi_header;
	find_name(sm, c->src, false, out->src_name);
	find_name(sm, c->dst, c->group, out->dst_name);
	out->gps_valid = false;
	out->gps_range = false;
#if !CONFIG_DMR_GPS
	(void)now_ms;
#else
	if (c->gps_valid) {
		int32_t lat, lon;
		uint32_t err_m;
		bool send;
		out->gps_valid = true;
		out->gps_err = c->gps.err;
		out->gps_lat = c->gps.lat;
		out->gps_lon = c->gps.lon;
		if (own_position(&lat, &lon, &err_m, &send, now_ms)) {
			out->gps_range = true;
			dmr_gps_range(lat, lon, c->gps.lat, c->gps.lon, &out->gps_dist_m, &out->gps_bearing);
		}
	}
#endif
}

// The stack's view into radio_view_t.dmr
static void dmr_publish(radio_sm_t *sm, uint32_t now_ms)
{
	const dmr_call_view_t *cv = dmr_call_view();
	radio_dmr_view_t *d = &sm->view.dmr;
	if (cv->serial == sm->dmr_serial) {
		return;
	}
	sm->dmr_serial = cv->serial;
	switch (cv->phase) {
	case DMR_CALL_PHASE_RX_VOICE: d->state = RDMR_RX; break;
	case DMR_CALL_PHASE_RX_BUSY: d->state = RDMR_RX_OTHER; break;
	case DMR_CALL_PHASE_TX_ACCESS:
	case DMR_CALL_PHASE_TX_WAKEUP: d->state = RDMR_TX_WAIT; break;
	case DMR_CALL_PHASE_TX_VOICE: d->state = RDMR_TX; break;
	case DMR_CALL_PHASE_TX_ENDING: d->state = RDMR_TX_ENDING; break;
	case DMR_CALL_PHASE_TX_DATA: d->state = RDMR_TX_DATA; break;
	case DMR_CALL_PHASE_TX_PERMIT: d->state = RDMR_TX_PERMIT; break;
	default: d->state = RDMR_IDLE; break;
	}
	d->codec = sm->codec;
	d->cc = cv->cc;
	d->ts = cv->ts;
	d->rx_cc = cv->rx_cc;
	d->rx_ts = cv->rx_ts;
	d->rmo = cv->rmo;
	d->repeater_up = cv->repeater_up;
	d->target_id = cv->tx_target.id;
	d->target_private = cv->tx_target.private_call;
	find_name(sm, d->target_id, !d->target_private, d->target_name);
	d->reply_active = cv->reply_active;
	d->reply_s = cv->reply_s;
	d->one_shot = sm->one_shot;
#if CONFIG_DMR_EMERGENCY
	bool emergency = cv->caller.emergency;
#if CONFIG_DMR_TRUNK
	// Trunked: the grant's Emergency flag too (table 7.11), should a site's voice LC lose its option bit
	emergency |= sm->t3_on && t3m.state == T3_FOLLOW && t3m.call.emergency && !t3m.call.ours;
#endif
	if (d->state == RDMR_RX && emergency) {
		dmr_emergency_heard(DMR_EMERGENCY_ALERT_CALL, cv->caller.src, cv->caller.dst, cv->caller.group, cv->call_start_ms);
	}
#endif
	if (d->state != RDMR_IDLE) {
		copy_call(sm, &cv->caller, &d->call, now_ms);
		d->late_entry = cv->caller.late_entry;
#if CONFIG_DMR_TALKER_ALIAS
		memcpy(d->alias, cv->caller.alias, sizeof(d->alias));
		d->alias[RADIO_ALIAS_MAX] = '\0';
#else
		d->alias[0] = '\0';
#endif
	}
	if (cv->rssi_dbm != 0 || d->state == RDMR_RX || d->state == RDMR_RX_OTHER) {
		d->rssi_dbm = cv->rssi_dbm;	// an RX call without a modem level gets the held one at the next poll
	}
	d->call_ms = cv->call_ms;
	if (cv->last_heard_ms != 0) {
		copy_call(sm, &cv->last_heard, &d->last, now_ms);
		d->last_ms = cv->last_heard_ms;
	}
	d->underruns = cv->tx_underruns;
	changed(sm);
}

// Follow the stack: release the board's TX arming once the stack is off the air (after the modem's TX done), PTT
// refusals, TX while the stack sends without PTT, the RX state from the calls and data heard
static void dmr_follow(radio_sm_t *sm, uint32_t now_ms)
{
	const dmr_call_view_t *cv = dmr_call_view();
	bool tx = dmr_tx_phase(cv->phase);

	if (cv->wake_sent != sm->wake_seen) {
		sm->wake_seen = cv->wake_sent;
		sm->wake_led = true;	// a wake-up burst just went out: one blink
		sm->wake_led_ms = now_ms;
	}

	if (sm->dmr_keyed && !tx) {
		sm->dmr_keyed = false;
#if CONFIG_DMR_TRUNK
		if (sm->t3_on) {
			sm->t3_tx_end_ms = now_ms != 0 ? now_ms : 1;	// for the turnaround (t3_tap)
		}
#endif
		// The stack is off the air: the receiver stays as the last burst left it when it can (a reply in our own
		// timeslot comes 30 ms after our terminator; a receive set-up then misses it)
		if (board.radio->dmr_tx_end == NULL || !board.radio->dmr_tx_end()) {
			board.radio->tx_off();
		}
	}
	if (sm->ptt) {
		sm->tx_no_ptt = false;	// the user keyed: that transmission counts
		if (sm->view.tx_auto) {
			sm->view.tx_auto = false;
			changed(sm);
		}
		if (cv->phase == DMR_CALL_PHASE_TX_DENIED && sm->view.state != RS_TX_DENIED) {
			deny(sm, dmr_deny_reason(cv->tx_error));
#if CONFIG_DMR_ROAMING
			if (cv->tx_error == DMR_TX_ERR_NO_REPEATER) {
				roam_trigger(&sm->roam, ROAM_WHY_TX_FAIL);	// the site didn't wake: look for another
			}
#endif
		}
	} else if (tx && !sm->hotspot && sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED) {
		// On the air without PTT: a data burst list (ARS, SMS, a position, the IP gateway) or a roaming probe. The
		// view says TX like a call does, so the strip, the LEDs and everything that asks whether the radio is
		// transmitting see it
		sm->view.state = RS_TX;
		sm->tx_no_ptt = true;
		sm->view.tx_auto = true;
		sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.tx_elapsed_ms = 0;
		sm->tx_start_ms = now_ms;
		sm->tot_warned = false;
		sm->view.tot_warning = false;
		changed(sm);
	} else if (sm->view.state == RS_TX && !tx) {
		bool data = sm->view.dmr.state == RDMR_TX_DATA;
		sm->tx_no_ptt = false;
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		changed(sm);
		if (sm->ts_pending != 0) {
			uint8_t ts = sm->ts_pending;
			sm->ts_pending = 0;	// taken or refused now, never left for a later transmission
			dmr_set_timeslot(sm, ts, sm->hotspot);
		}
		if (cv->tx_error == DMR_TX_ERR_NO_REPEATER && !data) {	// a data sender hears about its own failure
			// A short press goes on waking the repeater after the release: its failure still gets the deny tone and
			// the deny flash
			beep(sm, BEEP_TX_DENY);
			deny_led_start(sm);
#if CONFIG_DMR_ROAMING
			roam_trigger(&sm->roam, ROAM_WHY_TX_FAIL);
#endif
		}
	}
#if CONFIG_DMR_GPS && CONFIG_APRS_RX
	// A caller's position (ETSI GPS Info) into the APRS station list, once per change of the call view
	if (cv->serial != sm->dmr_serial && cv->phase <= DMR_CALL_PHASE_RX_BUSY && cv->caller.gps_valid) {
		aprs_dmr_heard(cv->caller.src, cv->caller.alias, cv->caller.gps.lat, cv->caller.gps.lon, now_ms);
	}
#endif
	dmr_publish(sm, now_ms);
	if (sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED) {
		bool rx = cv->state == DMR_CALL_STATE_RX;
		if (rx && sm->view.state != RS_RX) {
			sm->view.state = RS_RX;
			changed(sm);
		} else if (!rx && sm->view.state == RS_RX) {
			end_reception(sm, now_ms);
			changed(sm);
		}
	}
	if (sm->view.state == RS_TX) {
		tx_timer(sm, now_ms);
	}
	outputs(sm);

	// Talk permit when the voice goes on air (unless turned off in the settings), call end when a call we played
	// ends (neither on no-beep channels). A board whose beeps can't sound on air plays a short permit before keying
	// instead, in the stack's hold (permit_seq)
	bool early = (board.caps & CAP_BEEP_NOT_ON_AIR) != 0;
	uint8_t permit = early ? BEEP_TX_PERMIT_SHORT : BEEP_TX_PERMIT;
	if (cv->wake_admits != sm->dmr_wake_admits) {
		sm->dmr_wake_admits = cv->wake_admits;
		permit_play(sm, permit);	// a short press: the repeater woke after PTT was let go
	}
	if (cv->permit_seq != sm->dmr_permit_seq) {
		sm->dmr_permit_seq = cv->permit_seq;
		if (permit_before_key(sm)) {
			permit_play(sm, BEEP_TX_PERMIT_SHORT);	// the stack holds the key-up until it has played
		}
	}
	uint8_t st = sm->view.dmr.state;
	if (st != sm->dmr_prev_state) {
		if (!sm->view.ch.no_beep) {
			if (st == RDMR_TX && !early) {
				permit_play(sm, BEEP_TX_PERMIT);
			} else if (sm->dmr_prev_state == RDMR_RX && (st == RDMR_IDLE || st == RDMR_RX_OTHER) &&
				   sm->view.state != RS_TX_DENIED) {	// a refusal's idle: the call plays on
				// The call heard ended: the radio's call end tone, unless the channel free tone is set
				if (sig_out(sm, SIG_EV_IDLE) == SIG_OUT_DEFAULT) {
					beep(sm, BEEP_CALL_END);
				} else if (sig_local_on(sm, SIG_EV_IDLE)) {
					sig_local(sm, SIG_EV_IDLE);
				}
#if CONFIG_SIG_TONES
			} else if (sm->dmr_prev_state == RDMR_TX && st != RDMR_TX && !sm->ptt &&
				   sig_local_on(sm, SIG_EV_END)) {
				// Our transmission is over (PTT let go: not a time-out or a denial, as in FM), but the engine may still be sending its last burst: a board whose
				// audio path is gated drops what is queued meanwhile, and the tone was heard half. Wait for it
				sm->dmr_end_wait = true;
				sm->dmr_end_ms = now_ms;
#endif
			}
		}
		sm->dmr_prev_state = st;
	}
#if CONFIG_SIG_TONES
	if (sm->dmr_end_wait && sm->view.ch.mode != RADIO_MODE_DMR) {
		sm->dmr_end_wait = false;	// the channel is not DMR any more
	}
	if (sm->dmr_end_wait) {
		const hal_audio_t *a = board.audio;
		bool blocked = a != NULL && a->speaker_blocked != NULL && a->speaker_blocked();
		if (st == RDMR_TX || (!blocked && !sm->dmr_keyed) || now_ms - sm->dmr_end_ms >= DMR_END_WAIT_MS) {
			sm->dmr_end_wait = false;
			if (st != RDMR_TX) {
				sig_local(sm, SIG_EV_END);
			}
		}
	}
#endif
}

// --- One-shot talk target: PTT while a list row is focused talks to that row's target once, and keeps it as the
// talk target for the reply window after the call (information-architecture.md 1). The stack sees it as the talk
// target, so replies on that talkgroup are heard; radio_sm_tick ends it ---

static bool same_dmr_target(uint32_t id_a, bool priv_a, uint32_t id_b, bool priv_b)
{
	return id_a == id_b && priv_a == priv_b;
}

// At PTT down: the focused target, if it isn't where PTT goes already
static void one_shot_start(radio_sm_t *sm)
{
	if (sm->focus_id == 0 || !sm->dmr_running) {
		if (sm->one_shot) {
			sm->one_shot_until = 0;	// PTT again inside the window: it runs again after this call
		}
		return;
	}
	const dmr_call_view_t *cv = dmr_call_view();
	if (same_dmr_target(cv->tx_target.id, cv->tx_target.private_call, sm->focus_id, sm->focus_private)) {
		if (sm->one_shot) {
			sm->one_shot_until = 0;
		}
		return;
	}
	sm->one_shot = true;
	sm->one_shot_id = sm->focus_id;
	sm->one_shot_private = sm->focus_private;
	sm->one_shot_ch = sm->view.channel_number;
	sm->one_shot_until = 0;
	dmr_configure(sm);
	sm->dmr_serial = cv->serial - 1u;	// publish the new target at the next tick
}

static void one_shot_end(radio_sm_t *sm)
{
	if (!sm->one_shot) {
		return;
	}
	sm->one_shot = false;
	sm->view.dmr.one_shot = false;
	if (sm->dmr_running) {
		dmr_configure(sm);
		sm->dmr_serial = dmr_call_view()->serial - 1u;
	}
	changed(sm);
}

// Every tick: the window runs from the end of our call (or of the target's answer), then the target goes back
static void one_shot_follow(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->one_shot) {
		return;
	}
	radio_dmr_view_t *d = &sm->view.dmr;
	const radio_dmr_call_t *c = &d->call;
	bool theirs = sm->view.state == RS_RX && d->state == RDMR_RX &&
		(sm->one_shot_private ? c->src == sm->one_shot_id : (c->group && c->dst == sm->one_shot_id));
	if (sm->ptt || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || theirs) {
		sm->one_shot_until = 0;
		return;
	}
	const dmr_call_view_t *cv = dmr_call_view();
	if (sm->one_shot_until == 0) {
		uint8_t w = sm->one_shot_private ? sm->win_private_s : sm->win_group_s;
		sm->one_shot_until = now_ms + (uint32_t)w * 1000u;
		if (sm->one_shot_until == 0) {
			sm->one_shot_until = 1;
		}
	}
	int32_t left = (int32_t)(sm->one_shot_until - now_ms);
	if (left <= 0) {
		if (!cv->reply_active) {	// someone else's reply window runs: the target comes back after it
			one_shot_end(sm);
		}
		return;
	}
	if (!cv->reply_active) {
		uint8_t s = (uint8_t)((left + 999) / 1000);
		if (!d->reply_active || d->reply_s != s) {
			d->reply_active = true;
			d->reply_s = s;
			changed(sm);
		}
	}
}

// PTT on a DMR channel (band, RX-only and frequency checks passed): arm the board's per-burst keying, then the stack
// decides when the bursts go out
// The event's tone on air in DMR (sigtone.h): sent as voice in place of the mic, from the first voice of the
// transmission (start) or after the last of it (end, which holds the over open for it). Not on a no-beep channel, and
// not while the host sends its own audio
static void sig_air_dmr(radio_sm_t *sm, sig_event_t ev)
{
#if CONFIG_SIG_TONES
	if (sm->host_audio || sm->view.ch.no_beep || !sig_out_air(sig_out(sm, ev), ev)) {
		return;
	}
	beep_seg_t seg[SIG_AIR_STEPS];
	uint8_t n = sig_chain(sm->set.sig_hz[ev], sm->set.sig_ms[ev], ev, seg);
	// A guard of silence on the outer side: before a start tone (the transmission's first frames), after an end tone
	// (its last), where a lossy link such as a hotspot has been seen to clip
	const beep_seg_t guard = { 0, ev == SIG_EV_END ? SIG_AIR_END_GUARD_MS : SIG_AIR_GUARD_MS };
	if (ev == SIG_EV_END) {
		seg[n++] = guard;
	} else {
		memmove(&seg[1], &seg[0], n * sizeof seg[0]);
		seg[0] = guard;
		n++;
	}
	ambeVoiceTxTone(seg, n, ev == SIG_EV_END);
#else
	(void)sm;
	(void)ev;
#endif
}

static void dmr_ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *ch = &sm->view.ch;
	if (!sm->codec) {
		deny(sm, TXDENY_NO_CODEC);
		return;
	}
	if (!sm->dmr_running) {
		deny(sm, TXDENY_NO_DMR);
		return;
	}
	if ((ch->dmr_id != 0 ? ch->dmr_id : sm->own_id) == 0) {
		deny(sm, TXDENY_NO_ID);
		return;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		if (!key_tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			deny(sm, board_refusal());
			return;
		}
		sm->dmr_keyed = true;
	}
	one_shot_start(sm);
	sm->view.state = RS_TX;
	sm->view.tx_auto = false;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	changed(sm);
#if CONFIG_DMR_GPS
	dmr_gps_tx(sm, now_ms);
#endif
	sig_air_dmr(sm, SIG_EV_BEGIN);
	dmr_call_ptt(true);
	dmr_call_poll(now_ms);
	dmr_follow(sm, now_ms);
}

#if CONFIG_DMR_DATA_CORE
// --- DMR data: the data service (core/dmr/dmrDataService.c) on the call stack. The service decides what to send and
// when (listen before talk, retries); this side checks the channel allows it, arms the board's per-burst keying as
// PTT does, and lends the list to the stack. dmr_follow releases the keying once the stack is off the air ---

#if CONFIG_DMR_TRUNK_TX && CONFIG_DMR_EMERGENCY
// Emergency on a Tier III system (docs/spec/dmr-trunking.md 7.8): the alarm acknowledged, so its cancel is due when
// the emergency ends, on the system that took the alarm; the last status heard that was looked at
static struct {
	bool alarm_sent;
	bool alarm_group;
	uint8_t alarm_sys;	// radio_t3_setup_t.sys: another system's IDs are another network's
	uint32_t alarm_dst;
	uint32_t status_seen;
} t3_em;
#endif

static struct {
	const dmrBurst_t *list;		// the service's buffer, valid until dmrDataServiceTxDone
	int count, next;
#if CONFIG_DMR_TRUNK_TX
	// A Tier III system: list waits while the engine gets a packet data channel for it (data_t3_tick)
	bool t3_wait;
	bool t3_asked;
	bool t3_group;
	uint32_t t3_dst;
	uint32_t t3_ip;		// to IPI: the IPv4 destination, asked for as a multi-part call (dmr-trunking.md 7.7)
	uint8_t t3_status;	// not 0: the list is the emergency alarm or a radio check, sent as this status instead
	bool t3_sent;		// sent on the packet data channel: its outcome ends the call (t3_ms_data_done)
	uint32_t t3_failures;	// dmrDataServiceIpFailures when it was sent
#endif
} data_tx;

static bool data_next(dmr_tx_burst_t *out, void *ctx)
{
	(void)ctx;
	if (data_tx.list == NULL || data_tx.next >= data_tx.count) {
		return false;
	}
	const dmrBurst_t *d = &data_tx.list[data_tx.next++];
	memset(out, 0, sizeof(*out));
	out->kind = d->dataType;
	out->host_crc = (d->flags & DMR_BURST_FLAG_MCU_CRC) != 0;	// CRC_MCU_Control (gotcha 5.1)
	out->sync_override = DMR_SYNC_NONE;
	out->len = d->length <= DMR_PAYLOAD_MAX ? d->length : DMR_PAYLOAD_MAX;
	memcpy(out->payload, d->payload, out->len);
	return true;
}

static void data_done(uint8_t err, void *ctx)
{
#if CONFIG_DMR_ROAMING
	radio_sm_t *sm = ctx;
	if (err == DMR_TX_ERR_NO_REPEATER && sm != NULL) {
		roam_trigger(&sm->roam, ROAM_WHY_TX_FAIL);	// the site didn't wake for our data either
	}
#else
	(void)ctx;
#endif
	data_tx.list = NULL;
	dmrDataServiceTxDone(err);
}

// Every data / control burst the stack heard on our slot
static void data_rx(const dmr_burst_t *b, uint32_t now_ms, void *ctx)
{
	(void)ctx;
	dmrBurst_t d;
	d.dataType = b->kind;
	d.length = b->len <= DMR_BURST_PAYLOAD_MAX ? b->len : DMR_BURST_PAYLOAD_MAX;
	d.flags = b->crc_ok ? 0 : DMR_BURST_FLAG_RX_CRC_ERROR;
	memcpy(d.payload, b->payload, d.length);
#if CONFIG_DMR_TRUNK_TX
	if (b->kind != DMR_BURST_CSBK) {
		t3_ms_data_heard(&t3m, now_ms);	// a reply on the radio's data channel holds it a while longer
	}
#endif
	dmrDataServiceRxBurst(&d, now_ms);
}

// A DMR VFO sends data as a DMR channel does: it has its own color code, slot and talk target, and tx_check covers its
// frequency. An FM VFO is out by the mode
static bool data_can_send(const radio_sm_t *sm)
{
	bool roaming = false;
#if CONFIG_DMR_ROAMING
	roaming = roam_searching(&sm->roam);	// never on a site being looked at
#endif
#if CONFIG_DMR_TRUNK_TX
	// A trunked system: data asks for a channel on the TSCC, or goes on the packet data call the radio is in. Never
	// while hunting or in a voice call
	if (sm->t3_on && !(t3m.state == T3_CAMPED || (t3m.state == T3_FOLLOW && t3m.call.data))) {
		return false;
	}
#endif
	return sm->dmr_running && !sm->hotspot && sm->view.ch.mode == RADIO_MODE_DMR &&
		tx_check(sm) == TXDENY_NONE && sm->view.dmr.own_id != 0 && !roaming;
}

static bool data_send(radio_sm_t *sm, const dmrBurst_t *bursts, int count)
{
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		const channel_t *ch = &sm->view.ch;
		if (!key_tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			return false;
		}
		sm->dmr_keyed = true;
	}
	data_tx.list = bursts;
	data_tx.count = count;
	data_tx.next = 0;
	dmr_data_source_t src = { .next = data_next, .done = data_done, .ctx = sm,
				  .count = (uint8_t)(count > 255 ? 255 : count), .tag = dmrDataServiceTxStartingName() };
	if (!dmr_call_data_tx(&src)) {
		data_tx.list = NULL;
		return false;	// the keying is released by dmr_follow (the stack isn't transmitting)
	}
	return true;
}

#if CONFIG_DMR_TRUNK_TX
// Packet data on a Tier III system (docs/spec/dmr-trunking.md 7.7): it goes on a payload channel the TSCC grants for
// it (6.6.3). The list waits here while the engine asks; the caller sends first (6.6.3.3), so it goes as soon as the
// radio is there. On a packet data call already (a response, a retry, the next packet to the same party) it goes at once
static bool data_t3_start(radio_sm_t *sm, const dmrBurst_t *bursts, int count)
{
	// Preambles wake a radio on a conventional channel; on the payload channel the called party is already there
	int i = 0;
	while (i < count && bursts[i].dataType != DMR_BURST_DATA_HEADER) {
		i++;
	}
	if (data_tx.t3_wait) {
		return false;
	}
#if CONFIG_DMR_EMERGENCY
	// The emergency alarm CSBK: on a trunked system it is the status service, status 126 (361-4 6.6.6.3.1), sent once
	// (the TSCC acknowledges it; the module's repeats are for a channel nobody acknowledges on)
	if (strcmp(dmrDataServiceTxStartingName(), "emergency") == 0) {
		uint32_t id;
		bool priv;
		dmr_emergency_target(&id, &priv);
		if (id == 0 || t3m.state != T3_CAMPED || t3m.ax.state != T3_AX_IDLE) {
			return false;
		}
		data_tx.list = NULL;
		data_tx.t3_wait = true;
		data_tx.t3_asked = false;
		data_tx.t3_group = !priv;
		data_tx.t3_dst = id;
		data_tx.t3_status = T3_STATUS_EMERGENCY;
		return true;
	}
#endif
#if CONFIG_DMR_SIGNALING
	// A radio check: on a trunked system the status polling service (6.6.6.2), whose answer is the called MS's own
	// status. Its answer finishes the check (dmrDataServiceSignalResult); there is no conventional answer CSBK
	if (strcmp(dmrDataServiceTxStartingName(), "signal") == 0 && count >= 1 && bursts[0].length >= 10 &&
	    (bursts[0].payload[0] & 0x3Fu) == DMR_CSBKO_RADIO_CHECK) {
		const uint8_t *c = bursts[0].payload;
		uint32_t id = ((uint32_t)c[4] << 16) | ((uint32_t)c[5] << 8) | c[6];
		if (id == 0 || t3m.state != T3_CAMPED || t3m.ax.state != T3_AX_IDLE) {
			return false;
		}
		data_tx.list = NULL;
		data_tx.t3_wait = true;
		data_tx.t3_asked = false;
		data_tx.t3_group = false;
		data_tx.t3_dst = id;
		data_tx.t3_status = T3_STATUS_POLL;
		return true;
	}
#endif
	if (i == count || bursts[i].length < 8) {
		return false;
	}
	// 361-1 9.2.6: G/I in octet 0, the destination in octets 2-4
	const uint8_t *h = bursts[i].payload;
	bool group = (h[0] & 0x80) != 0;
	uint32_t dst = ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
	if (t3m.state == T3_FOLLOW && t3m.call.data) {
		const t3_call_t *c = &t3m.call;
		bool same = group == c->group && (dst == c->dst || (!group && dst == c->src));
		if (same && t3_ms_data_ready(&t3m, sm->t3_now) && data_send(sm, bursts + i, count - i)) {
			data_tx.t3_sent = c->ours;
			data_tx.t3_failures = dmrDataServiceIpFailures();
			return true;
		}
		return false;
	}
	if (t3m.state != T3_CAMPED || t3m.ax.state != T3_AX_IDLE) {
		return false;
	}
	// Off-net IP on a system with an IP gateway: the IP gateway's ID is the destination (data_tick routes it there),
	// the packet's own destination the call's extended address
	uint32_t ip = !group && dst == T3_ID_IPI ? dmrDataServiceTxIpDst() : 0;
	if (!group && dst == T3_ID_IPI && ip == 0) {
		return false;
	}
	data_tx.list = bursts + i;
	data_tx.count = count - i;
	data_tx.next = 0;
	data_tx.t3_wait = true;
	data_tx.t3_asked = false;
	data_tx.t3_group = group;
	data_tx.t3_dst = dst;
	data_tx.t3_ip = ip;
	data_tx.t3_status = 0;
	return true;
}

static void data_t3_fail(uint8_t err)
{
	data_tx.t3_wait = false;
	data_tx.list = NULL;
	dmrDataServiceTxDone(err);
}

// From t3_tick: ask for the channel, send once on it, or give the list back failed
static void data_t3_tick(radio_sm_t *sm, uint32_t now_ms)
{
	if (data_tx.t3_sent && !dmrDataServiceIsBusy() && !dmrDataServiceIpBusy()) {
		// The single item (and any confirmed retries) is over: so is the radio's data call (dmr-trunking.md 7.7)
		data_tx.t3_sent = false;
		t3_ms_data_done(&t3m, now_ms, dmrDataServiceIpFailures() == data_tx.t3_failures);
	}
	if (!data_tx.t3_wait) {
		return;
	}
	if (!data_tx.t3_asked) {
		bool ok = data_tx.t3_status != 0 ?
			t3_ms_status(&t3m, now_ms, data_tx.t3_dst, data_tx.t3_group, data_tx.t3_status) :
			data_tx.t3_ip != 0 ? t3_ms_pdata_ip(&t3m, now_ms, data_tx.t3_ip) :
			t3_ms_pdata(&t3m, now_ms, data_tx.t3_dst, data_tx.t3_group);
		if (!ok) {
			data_t3_fail(DMR_TX_ERR_REJECTED);
			return;
		}
		data_tx.t3_asked = true;
		return;
	}
	const t3_ms_t *m = &t3m;
	if (data_tx.t3_status != 0) {
		if (m->ax.svc == T3_SVC_STATUS && (m->ax.state != T3_AX_IDLE || m->ax.result == T3_RES_PENDING)) {
			return;
		}
		bool ok = m->ax.svc == T3_SVC_STATUS && m->ax.result == T3_RES_DONE;
		uint8_t status = data_tx.t3_status;
		data_tx.t3_wait = false;
		data_tx.t3_status = 0;
		if (status == T3_STATUS_POLL) {
#if CONFIG_DMR_SIGNALING
			dmrDataServiceSignalResult(data_tx.t3_dst, ok);	// answered: the MS is there
#endif
			dmrDataServiceTxDone(ok ? DMR_TX_OK : DMR_TX_ERR_NO_REPEATER);
			return;
		}
#if CONFIG_DMR_EMERGENCY
		if (ok) {
			t3_em.alarm_dst = data_tx.t3_dst;
			t3_em.alarm_group = data_tx.t3_group;
			t3_em.alarm_sys = t3s.sys;
			t3_em.alarm_sent = true;
		}
		dmrDataServiceTxDone(ok ? DMR_TX_OK : DMR_TX_ERR_NO_REPEATER);
		if (ok) {
			dmrDataServiceEmergencyAlarmCancel();	// acknowledged: no repeats; the module goes on to the call
		}
#endif
		return;
	}
	if (t3m.state == T3_FOLLOW && t3m.call.data && t3m.call.ours) {
		if (t3_ms_data_ready(&t3m, now_ms) && !sm->ptt) {
			data_tx.t3_wait = false;
			if (!data_send(sm, data_tx.list, data_tx.count)) {
				dmrDataServiceTxDone(DMR_TX_ERR_REJECTED);
			} else {
				data_tx.t3_sent = true;
				data_tx.t3_failures = dmrDataServiceIpFailures();
			}
		}
		return;
	}
	if (m->ax.svc != T3_SVC_PDATA || (m->ax.state == T3_AX_IDLE && m->ax.result != T3_RES_PENDING)) {
		// Refused, no answer, timed out, or the call came and went without the radio sending
		data_t3_fail(m->ax.result == T3_RES_REFUSED ? DMR_TX_ERR_REJECTED : DMR_TX_ERR_NO_REPEATER);
	}
}
#endif

static bool data_tx_start(const dmrBurst_t *bursts, int count, void *ctx)
{
	radio_sm_t *sm = ctx;
	if (!data_can_send(sm) || sm->ptt || count <= 0) {
		return false;
	}
#if CONFIG_DMR_TRUNK_TX
	if (sm->t3_on) {
		return data_t3_start(sm, bursts, count);
	}
#endif
	return data_send(sm, bursts, count);
}

static bool data_channel_clear(uint32_t now_ms, void *ctx)
{
#if CONFIG_DMR_TRUNK
	// A trunked system gives the slot: random access on the TSCC, a grant on the payload channel
	const radio_sm_t *sm = ctx;
	if (sm->t3_on) {
		return true;
	}
#else
	(void)ctx;
#endif
	return dmr_call_channel_clear(now_ms);
}

#if CONFIG_APRS_RX
// A DMR APRS position (an NMEA sentence on UDP 10110, docs/spec/aprs.md 8.2) into the station list, as a GPS Info is
static void data_position(uint32_t src, const uint8_t *nmea, int length, uint32_t now_ms, void *ctx)
{
	nmea_fix_t f;
	(void)ctx;
	if (nmea_parse_unchecked(nmea, (size_t)length, &f) && f.fix) {
		// No talker alias in a data call: the ID database's "callsign name ..." in its place (aprs_dmr_heard takes
		// the first word when it is a callsign, else the DMR ID)
		char text[DMR_IDDB_TEXT_MAX + 1] = "";
		if (!dmr_iddb_lookup(src, text, sizeof(text))) {
			text[0] = '\0';
		}
		aprs_dmr_heard(src, text, f.lat, f.lon, now_ms);
	}
}
#endif

static bool data_in_rx_group(uint32_t tg, void *ctx)
{
	const radio_sm_t *sm = ctx;
#if CONFIG_DMR_TRUNK
	if (sm->t3_on) {
		return t3_ms_wants_group(&t3m, tg, false);
	}
#endif
	for (uint16_t i = 0; i < sm->listen_count; i++) {
		if (sm->listen[i] == tg) {
			return true;
		}
	}
	return false;
}

// The data service's settings as last saved: a change (dmr_data_set_settings, applied by the service's tick) is
// noted in the settings store from data_tick
static dmrDataSettings_t data_saved;

// The ARS server and the IP gateway as the codeplug's settings area gives them (docs/spec/codeplug-settings.md 5),
// read at start, whenever codeplug_serial() (a codeplug written) or codeplug_kv_serial() (a key written) moves on and
// whenever the settings change. A key that is present holds its
// setting: a change made another way is put back at the next tick, before it is saved. An absent key leaves the
// setting as the radio has it (the settings store's copy), so a codeplug from a tool that doesn't carry the area
// changes nothing. No copy of the keys in RAM: a read is a few bytes of flash, and only on those events. The keys are the
// table in radio_keys.h (codeplug_keys_apply)
static void data_kv_load(void)
{
	dmrDataSettings_t set;
	dmrDataServiceGetSettings(&set);
	if (codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &set)) {
		dmrDataServiceSetSettings(&set);
	}
}

static void data_save_settings(uint32_t now_ms)
{
	dmrDataSettings_t cur;
	dmrDataServiceGetSettings(&cur);
	if (cur.arsServer == data_saved.arsServer && cur.radioCheckReply == data_saved.radioCheckReply &&
	    cur.callAlertAck == data_saved.callAlertAck && cur.lrrpReply == data_saved.lrrpReply &&
	    cur.smsFormat == data_saved.smsFormat && cur.radioCheckEtsi == data_saved.radioCheckEtsi &&
	    cur.udpCompress == data_saved.udpCompress && cur.ipConfirmed == data_saved.ipConfirmed &&
	    cur.ipConfirmedTries == data_saved.ipConfirmedTries && cur.ipGatewayId == data_saved.ipGatewayId &&
	    memcmp(cur.ipConfirmedRule, data_saved.ipConfirmedRule, sizeof(cur.ipConfirmedRule)) == 0) {
		return;
	}
	if (codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &cur)) {
		dmrDataServiceSetSettings(&cur);	// the codeplug's key holds against the change
	}
	data_saved = cur;
	settings_t *st = settings_begin();
	st->ds_ars_server = cur.arsServer;
	st->ds_flags = (uint8_t)((cur.radioCheckReply ? SETTINGS_DS_RADIO_CHECK_REPLY : 0) |
				 (cur.callAlertAck ? SETTINGS_DS_CALL_ALERT_ACK : 0) |
				 (cur.lrrpReply ? SETTINGS_DS_LRRP_REPLY : 0) |
				 (cur.smsFormat == DMR_SMS_FORMAT_ETSI ? SETTINGS_DS_SMS_ETSI : 0) |
				 (cur.smsFormat == DMR_SMS_FORMAT_ETSI_IP ? SETTINGS_DS_SMS_ETSI_IP : 0) |
				 (cur.radioCheckEtsi ? SETTINGS_DS_RADIO_CHECK_ETSI : 0) |
				 (cur.udpCompress == DMR_UDPC_OFF ? SETTINGS_DS_UDPC_OFF : 0) |
				 (cur.udpCompress == DMR_UDPC_ALL ? SETTINGS_DS_UDPC_ALL : 0));
	st->ds_ip_cnf = (uint8_t)((cur.ipConfirmedTries & 0x0F) | SETTINGS_DS_CHECK_SET |
				  ((cur.ipConfirmed == DMR_IPC_OFF ? SETTINGS_DS_IPC_OFF :
				    cur.ipConfirmed == DMR_IPC_ALL ? SETTINGS_DS_IPC_ALL : 0u) << SETTINGS_DS_IPC_SHIFT));
	memcpy(st->ds_ip_rule, cur.ipConfirmedRule, sizeof(st->ds_ip_rule));
	st->ds_ip_gateway = cur.ipGatewayId;
	st->ds_stored = 1;
	settings_end(now_ms);
}

#if CONFIG_DMR_EMERGENCY
// --- DMR emergency (core/dmr/dmr_emergency.c): the module decides; this keys the hot mic as the 1750 Hz burst keys
// the transmitter (PTT taken over by a real press), reconfigures the stack when the emergency starts or ends, sounds
// the alert tone and keeps the settings in the store (settings_t em_*) ---

static uint32_t em_saved_serial;

static void emergency_init(const settings_t *st)
{
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	if (st->em_stored == 1) {
		es.mode = st->em_mode;
		es.dst = st->em_dst;
		es.dst_private = (st->em_flags & SETTINGS_EM_DST_PRIVATE) != 0;
		es.etsi_fid = (st->em_flags & SETTINGS_EM_ETSI_FID) != 0;
		es.rx_tone = (st->em_flags & SETTINGS_EM_NO_TONE) == 0;
		es.alarm_tries = st->em_alarm_tries;
		es.hot_mic_s = st->em_hot_mic_s;
	}
	dmr_emergency_init(&es);
	em_saved_serial = dmr_emergency_settings_serial();
}

static void emergency_save(uint32_t now_ms)
{
	if (dmr_emergency_settings_serial() == em_saved_serial) {
		return;
	}
	em_saved_serial = dmr_emergency_settings_serial();
	dmr_emergency_settings_t es;
	dmr_emergency_get_settings_now(&es);
	settings_t *st = settings_begin();
	st->em_mode = es.mode;
	st->em_dst = es.dst;
	st->em_flags = (uint8_t)((es.dst_private ? SETTINGS_EM_DST_PRIVATE : 0) | (es.etsi_fid ? SETTINGS_EM_ETSI_FID : 0) |
				 (es.rx_tone ? 0 : SETTINGS_EM_NO_TONE));
	st->em_alarm_tries = es.alarm_tries;
	st->em_hot_mic_s = es.hot_mic_s;
	st->em_stored = 1;
	settings_end(now_ms);
}

#if CONFIG_DMR_TRUNK_TX
static uint32_t t3_base_talk_id(bool *group);
#endif

static void emergency_tick(radio_sm_t *sm, uint32_t now_ms)
{
	dmr_emergency_channel_t c = {
		.can_transmit = data_can_send(sm),
		.talk_id = sm->manual ? sm->manual_id : sm->talk_id,
		.talk_private = sm->manual ? sm->manual_private : sm->talk_private,
	};
#if CONFIG_DMR_TRUNK_TX
	if (sm->t3_on) {
		bool group;
		c.talk_id = t3_base_talk_id(&group);
		c.talk_private = !group;
		c.can_transmit = t3m.state == T3_CAMPED || t3_ms_can_talk(&t3m);	// the hot mic lasts into the granted call
	}
#endif
	dmr_emergency_tick(now_ms, &c);
	if (sm->dmr_running && dmr_emergency_config_serial() != sm->em_cfg_serial) {
		dmr_configure(sm);	// the emergency option and target on or off; takes effect at the next call
	}
	bool want = dmr_emergency_hot_mic();
	if (want && !sm->em_keyed && !sm->ptt) {
		ptt_down(sm, now_ms);
		sm->em_keyed = sm->ptt;		// held by the hot mic, even when refused (the deny shows until it ends)
	} else if (!want && sm->em_keyed) {
		sm->em_keyed = false;
		ptt_release(sm, false, now_ms);
	}
	if (dmr_emergency_take_tone()) {
		beep(sm, BEEP_EMERGENCY);
	}
	emergency_save(now_ms);
}
#endif

#if CONFIG_DMR_LRRP
// LRRP's position (dmrDataEnv_t.ownPosition): the position service's, with the ambiguity setting applied as for DMR
// GPS Info
static bool data_own_position(lrrpPosition_t *position, uint32_t now_ms, void *ctx)
{
	(void)ctx;
	position_fix_t f;
	uint32_t err_m;
	if (position_report(&f, &err_m, now_ms) == POSITION_SRC_NONE) {
		return false;
	}
	uint16_t kn = f.speed_kmh == 0xFFFFu ? 0xFFFFu : (uint16_t)(((uint32_t)f.speed_kmh * 1000u + 926u) / 1852u);
	lrrpPositionFromE7(f.lat, f.lon, kn, f.course, f.utc_hms, f.date, position);
	return true;
}
#endif

static void data_init(radio_sm_t *sm)
{
	dmrDataEnv_t env = { .txStart = data_tx_start, .channelClear = data_channel_clear,
			     .inRxGroup = data_in_rx_group, .ctx = sm,
			     .noRate34 = board.dmr != NULL && board.dmr->tx_rate34_bad };
#if CONFIG_DMR_AIR_QUEUE
	airqInit();
#endif
#if CONFIG_APRS_RX
	env.position = data_position;
#endif
#if CONFIG_DMR_LRRP
	env.ownPosition = data_own_position;
#endif
#if CONFIG_DMR_IP_GATEWAY
	env.packet = hostlink_net_air_packet;
	hostlink_net_air_start();
#endif
	// No ARS server until the user sets one (docs/ux/principles.md). Radio checks are answered and call alerts
	// acknowledged (that is what makes them work for the radio asking); LRRP answers "no fix" without a position
	// source
	// IP packets: compressed headers where the ports allow, confirmed only for the rules the user sets
	dmrDataSettings_t set = { .arsServer = 0, .radioCheckReply = true, .callAlertAck = true, .lrrpReply = true,
				  .hasGps = false, .smsFormat = DMR_SMS_FORMAT_ETSI_IP, .udpCompress = DMR_UDPC_AUTO,
				  .ipConfirmed = DMR_IPC_RULES, .ipConfirmedTries = DMR_IPC_TRIES_DEFAULT };
#if CONFIG_DMR_LRRP
	// The position service answers (data_own_position). A source can come later (a GPS found after start-up, a fixed
	// position typed in): no fix until then
	set.hasGps = true;
#endif
	settings_t *st = &st_copy;
	settings_read(st);
	if (st->ds_stored == 1) {	// as saved (docs/spec/settings-store.md 3)
		set.arsServer = st->ds_ars_server <= 0xFFFFFFu ? st->ds_ars_server : 0;
		// A record from before the default became On answers too (as zone shares, docs/spec/dmr-data.md 8)
		set.radioCheckReply = !(st->ds_ip_cnf & SETTINGS_DS_CHECK_SET) ||
				      (st->ds_flags & SETTINGS_DS_RADIO_CHECK_REPLY) != 0;
		set.callAlertAck = (st->ds_flags & SETTINGS_DS_CALL_ALERT_ACK) != 0;
		set.lrrpReply = (st->ds_flags & SETTINGS_DS_LRRP_REPLY) != 0;
		// A record from before ETSI IP text has neither format bit: TMS, as that firmware sent (only the default
		// changed, docs/spec/dmr-data.md 8)
		set.smsFormat = (st->ds_flags & SETTINGS_DS_SMS_ETSI_IP) ? DMR_SMS_FORMAT_ETSI_IP :
				(st->ds_flags & SETTINGS_DS_SMS_ETSI) ? DMR_SMS_FORMAT_ETSI : DMR_SMS_FORMAT_TMS;
		set.radioCheckEtsi = (st->ds_flags & SETTINGS_DS_RADIO_CHECK_ETSI) != 0;
		set.udpCompress = (st->ds_flags & SETTINGS_DS_UDPC_OFF) ? DMR_UDPC_OFF :
				  (st->ds_flags & SETTINGS_DS_UDPC_ALL) ? DMR_UDPC_ALL : DMR_UDPC_AUTO;
		uint8_t ipc = (uint8_t)(st->ds_ip_cnf >> SETTINGS_DS_IPC_SHIFT) & 3u;
		set.ipConfirmed = ipc == SETTINGS_DS_IPC_OFF ? DMR_IPC_OFF : ipc == SETTINGS_DS_IPC_ALL ? DMR_IPC_ALL :
				  DMR_IPC_RULES;
		set.ipConfirmedTries = (st->ds_ip_cnf & 0x0F) ? (st->ds_ip_cnf & 0x0F) : DMR_IPC_TRIES_DEFAULT;
		memcpy(set.ipConfirmedRule, st->ds_ip_rule, sizeof(set.ipConfirmedRule));
		set.ipGatewayId = st->ds_ip_gateway & 0xFFFFFFu;	// 0 in a record from before it (off)
	}
	// The settings store's copy as saved, then the keys over it: a key it doesn't hold yet (a factory reset leaves the
	// codeplug, a power cut before the copy was saved) differs from data_saved, so the first tick saves the copy
	// (docs/spec/codeplug-settings.md 5.2: the radio's own copy follows the key)
	data_saved = set;
	codeplug_keys_apply(radio_keys, RADIO_KEYS_N, &set);
	data_tx.list = NULL;
	dmrDataServiceInit(&env);
	dmrDataServiceSetSettings(&set);
#if CONFIG_ZONE_SHARE
	zone_share_init();
	// On unless the user turned it off (docs/spec/zone-share.md 5)
	zone_share_set_offers(settings_zs_accept(st) ? ZS_OFFERS_ASK : ZS_OFFERS_OFF);
#endif
	dmr_call_set_data_rx(data_rx, NULL);
#if CONFIG_DMR_EMERGENCY
	emergency_init(st);
#endif
}

static void data_tick(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *ch = &sm->view.ch;
	dmrDataChannel_t c = {
		.ownId = sm->view.dmr.own_id,
		.talkGroup = sm->talk_private ? 0 : sm->talk_id,
		.rmo = sm->view.dmr.rmo,
		.canTransmit = data_can_send(sm),
	};
	uint32_t h = 2166136261u;	// FNV-1a of what a registration is for
	uint32_t vals[] = { ch->rx_hz, ch->tx_hz, sm->view.dmr.ts, sm->view.dmr.cc };
#if CONFIG_DMR_TRUNK
	if (sm->t3_on) {
		// A trunked system: the call's talkgroup; one registration for the system, whichever channel it is on
		c.talkGroup = t3m.state == T3_FOLLOW && t3m.call.group ? t3m.call.dst : 0;
		vals[0] = t3s.cfg.own_id;
		vals[1] = vals[2] = vals[3] = 0;
	}
#endif
	for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		h = (h ^ vals[i]) * 16777619u;
	}
	c.channelKey = h;
#if CONFIG_DMR_PRIVACY
	// The voice key is the data key: data is encrypted or held back as the scheme says
	static dmrPrivacyKey_t data_key;	// the data layer's crypt context points here (radio task only)
	data_key.type = (dmrPrivacyType_t)sm->privacy_type;
	data_key.key = sm->privacy_key;
	dmrPrivacyDataConfig(&data_key, &c.privacy);
#endif
#if CONFIG_DMR_IP_GATEWAY
	// Control station passthrough while the PC can take the packets (docs/spec/usb-network.md 5.2)
	dmrDataServiceSetPassthrough(hostlink_net_passthrough_active());
#endif
	dmrDataServiceTick(now_ms, &c);
#if CONFIG_DMR_IP_GATEWAY
	hostlink_net_air_tick(sm->view.dmr.own_id, now_ms);
#endif
#if CONFIG_DMR_AIR_QUEUE
	airqTick(sm->view.dmr.own_id, now_ms);	// the next queued packet, when the service is free
#endif
#if CONFIG_ZONE_SHARE
	zone_share_tick(now_ms);
#endif
	data_save_settings(now_ms);
	if (sm->power_off_req && sm->power_off_busy) {
		sm->power_off_busy = dmrDataServicePowerOff();	// ARS deregistration, until it has gone out
	}
#if CONFIG_DMR_IP_GATEWAY
	// the setting in force (data_save_settings above follows it); on a system with an IP gateway, that gateway
	uint32_t off_net = data_saved.ipGatewayId;
#if CONFIG_DMR_TRUNK_TX
	if (sm->t3_on && radio_t3v.ip_gw) {
		off_net = T3_ID_IPI;
	}
#endif
	ipGatewaySetOffNetId(off_net);
#endif
}
#endif

#if CONFIG_DMR_TRUNK
// --- A Tier III trunked system (docs/spec/dmr-trunking.md 11). The engine (dmr/trunk/t3_ms.c) decides; this side tunes
// where it says, hands it the control bursts the stack hears (the RX tap) and one tick per slot, sends its CSBKs through
// the stack's data path, and turns PTT on the control channel into a call request ---

static bool t3_send_next(dmr_tx_burst_t *out, void *ctx)
{
	radio_sm_t *sm = ctx;
	if (sm->t3_send_left == 0) {
		return false;
	}
	uint8_t i = (uint8_t)(sm->t3_send_n - sm->t3_send_left);
	sm->t3_send_left--;
	memset(out, 0, sizeof(*out));
	// A short data upload is a UDT header, then its rate 1/2 blocks in the next frames
	out->kind = !sm->t3_send_udt ? DMR_BURST_CSBK : (i == 0 ? DMR_BURST_DATA_HEADER : DMR_BURST_RATE_12_DATA);
	out->host_crc = true;
	out->sync_override = DMR_SYNC_NONE;
	out->len = T3_CSBK_BYTES;
	memcpy(out->payload, sm->t3_send_burst[i], T3_CSBK_BYTES);
	return true;
}

static void t3_send_done(uint8_t err, void *ctx)
{
	radio_sm_t *sm = ctx;
	sm->t3_sending = false;
	if (err != DMR_TX_OK) {
		radio_t3v.send_fail++;
	}
	sm->t3_sent_report = err == DMR_TX_OK ? 1u : 2u;	// to the engine at the next step (t3_ms_sent)
}

// The color code a channel uses: its own, else the control channel's, else the system's
static uint8_t t3_chan_cc(const radio_sm_t *sm, const t3_chan_t *c)
{
	if (c->cc != T3_CC_ANY) {
		return c->cc & 0x0Fu;
	}
	if (t3m.confirmed && t3m.tscc.cc != T3_CC_ANY) {
		return t3m.tscc.cc & 0x0Fu;
	}
	return t3s.cc & 0x0Fu;
}

// Power save (dmr-trunking.md 7.13): the receiver off while the engine sleeps, on boards whose rx_sleep works on DMR.
// Awake again, the modem searches the TSCC's slot timing afresh: the old timing went with the signal
static void t3_rx_sleep(radio_sm_t *sm, bool off, uint32_t now_ms)
{
	if (off == sm->t3_rx_off) {
		return;
	}
	sm->t3_rx_off = off;
	radio_t3v.rx_off = off;
	board.radio->rx_sleep(off);
	if (off) {
		sm->t3_rx_off_at = now_ms;
		radio_t3v.rx_offs++;
		return;
	}
	radio_t3v.rx_off_ms += now_ms - sm->t3_rx_off_at;
	if (sm->dmr_running) {
		board.dmr->rx_restart();
	}
}

static void t3_retune(radio_sm_t *sm, const t3_chan_t *c, uint8_t slot, uint32_t now_ms)
{
	sm->t3_tune_held = false;
	sm->t3_tx_end_ms = 0;	// the turnaround counts on the channel transmitted on only
	t3_rx_sleep(sm, false, now_ms);
	channel_t *ch = &sm->view.ch;
	uint8_t ts = slot == 2 ? 2 : 1;
	bool rf = ch->rx_hz != c->rx_hz || sm->board_mode != RADIO_MODE_DMR || !sm->dmr_running;
	ch->rx_hz = c->rx_hz;
	ch->tx_hz = c->tx_hz;
	ch->mode = RADIO_MODE_DMR;
	ch->dmo = false;
	ch->color_code = t3_chan_cc(sm, c);
	ch->timeslot = ts;	// a control channel: its first slot carries the TSCC (dedicated)
	sm->t3_cc = ch->color_code;
	sm->t3_both = slot == 0;
	sm->temp_ts = 0;
	if (rf) {
		tune(sm, now_ms);
	} else {
		dmr_reconfigure(sm);
	}
}

// Where the engine says. A burst list still with the stack (an answer before leaving a payload channel, 6.6.2.3.2.7)
// goes out first: a new slot or channel would drop it before its slot came
static void t3_tune(radio_sm_t *sm, const t3_act_t *a, uint32_t now_ms)
{
	if (sm->t3_sending) {
		sm->t3_tune_held = true;
		sm->t3_tune_chan = a->chan;
		sm->t3_tune_slot = a->slot;
		sm->t3_tune_ms = now_ms;
		return;
	}
	t3_retune(sm, &a->chan, a->slot, now_ms);
}

static void t3_send(radio_sm_t *sm, const t3_act_t *a, uint32_t now_ms)
{
	// PTT held while waiting for our grant is the request itself, not talking. A held retune: the engine is
	// elsewhere already
	if (sm->t3_tune_held || sm->view.ch.timeslot != (a->slot == 2 ? 2 : 1) || (sm->ptt && !sm->t3_ptt_wait) ||
	    sm->view.state == RS_TX) {
		radio_t3v.send_fail++;	// not on the slot asked for, or talking: the engine retries what needs it
		t3_ms_sent(&t3m, now_ms, false);
		return;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		if (!key_tx(sm->view.ch.tx_hz, (radio_bw_t)sm->view.ch.bw, sm->view.power, &none)) {
			radio_t3v.send_fail++;
			t3_ms_sent(&t3m, now_ms, false);
			return;
		}
		sm->dmr_keyed = true;
	}
	uint8_t nb = a->udt && a->blocks != NULL ? (a->nblocks > T3_UDT_BLOCKS_MAX ? T3_UDT_BLOCKS_MAX : a->nblocks) : 0u;
	memcpy(sm->t3_send_burst[0], a->burst, T3_CSBK_BYTES);
	for (uint8_t i = 0; i < nb; i++) {
		memcpy(sm->t3_send_burst[1 + i], a->blocks[i], T3_CSBK_BYTES);
	}
	sm->t3_send_udt = a->udt;
	sm->t3_send_n = (uint8_t)(1u + nb);
	sm->t3_send_left = sm->t3_send_n;
	dmr_data_source_t src = { .next = t3_send_next, .done = t3_send_done, .ctx = sm, .count = sm->t3_send_n,
				  .tag = "t3" };
	if (!dmr_call_data_tx(&src)) {
		sm->t3_send_left = 0;
		radio_t3v.send_fail++;	// the keying is released by dmr_follow
		t3_ms_sent(&t3m, now_ms, false);
		return;
	}
	sm->t3_sending = true;
	radio_t3v.last_tx_op = (a->udt ? a->burst[9] : a->burst[0]) & 0x3Fu;
	memcpy(radio_t3v.last_tx, a->burst, T3_CSBK_BYTES);
	radio_t3v.last_tx_ms = now_ms != 0 ? now_ms : 1;
}

// The RX tap: control bursts to the engine, activity on the followed slot. Voice and the rest go on to the stack
static bool t3_tap(const dmr_burst_t *b, uint32_t now_ms, void *ctx)
{
	radio_sm_t *sm = ctx;
	if (!sm->t3_on) {
		return false;
	}
	uint8_t slot = (b->ts == 1 || b->ts == 2) ? (uint8_t)b->ts : 0;
	// TV_Inactive measures signal quality (6.6.2.3.2.9 a): every burst with a sync counts, a hang's idles included
	if (slot != 0) {
		t3_ms_activity(&t3m, now_ms, slot);
		if (b->kind == DMR_BURST_VOICE || b->kind == DMR_BURST_VOICE_LC_HEADER || b->kind == DMR_BURST_TERMINATOR_LC) {
			t3_ms_call_burst(&t3m, now_ms, slot, b->cc);
		}
		if (sm->t3_tx_end_ms != 0) {
			uint32_t gap = now_ms - sm->t3_tx_end_ms;
			sm->t3_tx_end_ms = 0;
			radio_t3v.turns++;
			radio_t3v.turn_ms = gap > 0xFFFFu ? 0xFFFFu : (uint16_t)gap;
			if (radio_t3v.turn_ms > radio_t3v.turn_max_ms) {
				radio_t3v.turn_max_ms = radio_t3v.turn_ms;
			}
			if (gap > 90u) {
				radio_t3v.turn_slow++;
			}
		}
	}
	if (b->kind != DMR_BURST_CSBK && b->kind != DMR_BURST_MBC_HEADER && b->kind != DMR_BURST_MBC_CONT &&
	    b->kind != DMR_BURST_DATA_HEADER && b->kind != DMR_BURST_RATE_12_DATA) {
		return false;
	}
	if (t3m.state == T3_FOLLOW && t3m.call.data && b->kind != DMR_BURST_CSBK && b->kind != DMR_BURST_MBC_HEADER &&
	    b->kind != DMR_BURST_MBC_CONT) {
		return false;	// a packet data call: its data is the data service's (UDT is a control channel's)
	}
	if (slot == 0) {
		slot = sm->view.ch.timeslot;	// not known: the slot we listen to
	}
	if (b->len >= T3_CSBK_BYTES) {
		// Whether the data service can take packet data to the radio now (6.6.3.2.6)
#if CONFIG_DMR_DATA_CORE
		t3m.data_nack = dmr_call_data_busy() ? T3_RC_EQUIP_BUSY : 0u;
#else
		t3m.data_nack = T3_RC_MS_NOT_SUPPORTED;
#endif
		t3_ms_rx(&t3m, now_ms, slot, b->kind, b->cc, b->payload);
		if (b->kind != DMR_BURST_RATE_12_DATA) {
			// A UDT header's opcode is in octet 9
			radio_t3v.last_rx_op = b->payload[b->kind == DMR_BURST_DATA_HEADER ? 9 : 0] & 0x3Fu;
			radio_t3v.last_rx_slot = slot;
			radio_t3v.last_rx_ms = now_ms != 0 ? now_ms : 1;
		}
	}
	return true;
}

#if CONFIG_DMR_TRUNK_TX
// The system's talk target, as the emergency module's channel target too
static uint32_t t3_base_talk_id(bool *group)
{
	const radio_t3_setup_t *s = &t3s;
	*group = !s->talk_private;
	if (s->talk_id != 0) {
		return s->talk_id;
	}
	*group = true;
	if (t3m.dgna_onekey != 0) {
		return t3m.dgna_onekey;	// DGNA's One-key talkgroup (6.6.8.1)
	}
	for (uint8_t i = 0; i < s->cfg.ntg; i++) {
		if (s->cfg.tg[i].flags & T3_TG_TALK) {
			return s->cfg.tg[i].id;
		}
	}
	return s->cfg.ntg != 0 ? s->cfg.tg[0].id : 0;
}

// PTT's target: in emergency, the emergency target; else the focused row's (one call), the one chosen for now, the
// system's own, as on a conventional channel
static uint32_t t3_talk_id(const radio_sm_t *sm, bool *group)
{
#if CONFIG_DMR_EMERGENCY
	if (dmr_emergency_in_progress()) {
		uint32_t id;
		bool priv;
		dmr_emergency_target(&id, &priv);
		if (id != 0) {
			*group = !priv;
			return id;
		}
	}
#endif
	if (sm->focus_id != 0) {
		*group = !sm->focus_private;
		return sm->focus_id;
	}
	if (sm->manual) {
		*group = !sm->manual_private;
		return sm->manual_id;
	}
	return t3_base_talk_id(group);
}
#endif

static void t3_publish(radio_sm_t *sm)
{
	radio_t3_view_t *v = &radio_t3v;
	const t3_ms_t *m = &t3m;
	v->on = sm->t3_on;
	v->state = (uint8_t)m->state;
	v->reg = (uint8_t)m->reg;
	v->confirmed = m->confirmed;
	v->syscode = m->syscode;
	v->tscc_lcn = m->tscc.lcn;
	v->rx_hz = sm->view.ch.rx_hz;
	v->tx_hz = sm->view.ch.tx_hz;
	v->slot = sm->t3_both ? 0 : sm->view.ch.timeslot;
	v->cc = sm->t3_cc;
	v->aloha_mask = m->aloha.mask;
	v->aloha_sf = m->aloha.service_fn;
	v->aloha_nrand = m->aloha.nrand_wait;
	v->aloha_backoff = m->aloha.backoff;
	v->aloha_reg = m->aloha.reg;
	v->call_dst = m->call.dst;
	v->call_src = m->call.src;
	v->call_group = m->call.group;
	v->call_ours = m->call.ours;
	v->ax_svc = (uint8_t)m->ax.svc;
	v->ax_result = (uint8_t)m->ax.result;
	v->ax_reason = m->ax.reason;
	v->ax_state = (uint8_t)m->ax.state;
	v->ax_info = m->ax.info;
	v->own_status = m->own_status;
	v->reg_tgs = m->reg_tgs;
	_Static_assert(T3_RA_LOG == sizeof(v->ra) / sizeof(v->ra[0]), "the view holds the whole log");
	v->nra = (uint8_t)(m->st.ra_tries < T3_RA_LOG ? m->st.ra_tries : T3_RA_LOG);
	for (uint8_t i = 0; i < v->nra; i++) {
		uint8_t k = (uint8_t)((m->st.ra_tries - v->nra + i) % T3_RA_LOG);
		v->ra[i].tx_ms = m->st.ra_log[k].tx_ms;
		v->ra[i].rx_ms = m->st.ra_log[k].rx_ms;
		v->ra[i].tx_slot = m->st.ra_log[k].tx_slot;
		v->ra[i].frames = m->st.ra_log[k].frames;
		v->ra[i].rx_slot = m->st.ra_log[k].rx_slot;
		v->ra[i].op = m->st.ra_log[k].op;
		v->ra[i].air_ms = m->st.ra_log[k].air_ms;
	}
	v->reg_tg_ok = m->reg_tg_ok;
	v->call_data = m->call.data;
	v->stunned = m->stunned;
	v->stuns = m->stuns;
	v->revives = m->revives;
	v->auth_key = m->cfg != NULL && m->cfg->auth_key_set;
	v->auths = m->auths;
	v->massregs = m->massregs;
	v->moves = m->moves;
	v->ps_rq = m->ps_rq;
	v->ps_offset = m->ps_offset;
	v->asleep = m->asleep;
	v->sleeps = m->sleeps;
	v->csc_valid = m->csc_valid;
	v->csc = m->csc;
	v->ip_advised = m->ip_advised;
	v->ahoys = m->st.ahoys;
	v->ahoy_src = m->st.ahoy_src;
	v->ahoy_ms = m->st.ahoy_ms;
	v->ahoy_kind = m->st.ahoy_kind;
	_Static_assert(RADIO_T3_DGNA == T3_DGNA_MAX, "the view holds the DGNA list");
	memcpy(v->dgna, m->dgna, sizeof(v->dgna));
	v->ndgna = m->ndgna;
	v->dgna_onekey = m->dgna_onekey;
	v->dgna_alias_tg = m->dgna_alias_tg;
	v->dgnas = m->dgnas;
	v->divert_to = m->ax.divert_to;
	v->divert_group = m->ax.divert_group;
	v->als_calls = m->als_calls;
	v->killed = m->killed;
	v->kills = m->kills;
	v->tscc_auth_fails = m->tscc_auth_fails;
	v->call_als = m->call.als;
#if CONFIG_DMR_DATA_CORE && CONFIG_DMR_TRUNK_TX
	v->pd_wait = data_tx.t3_wait;
#endif
	v->st_count = m->status_rx.count;
	v->st_src = m->status_rx.src;
	v->st_dst = m->status_rx.dst;
	v->st_group = m->status_rx.group;
	v->st_value = m->status_rx.value;
	v->st_ms = m->status_rx.ms;
	v->sd_count = m->sd_rx.count;
	v->sd_src = m->sd_rx.src;
	v->sd_dst = m->sd_rx.dst;
	v->sd_group = m->sd_rx.group;
	v->sd_format = m->sd_rx.format;
	v->sd_len = m->sd_rx.len;
	memcpy(v->sd_data, m->sd_rx.data, sizeof(v->sd_data));
	v->sd_ms = m->sd_rx.ms;
#if CONFIG_DMR_TRUNK_GW
	v->cli_count = m->cli.count;
	v->cli_gw = m->cli.gw;
	v->call_cli = m->call.cli;
	v->cli_dst = m->cli.dst;
	v->cli_group = m->cli.group;
	v->cli_n = m->cli.n;
	memcpy(v->cli_bcd, m->cli.bcd, sizeof(v->cli_bcd));
	v->cli_ms = m->cli.ms;
#endif
#if CONFIG_DMR_TRUNK_SUPL
	v->supl_count = m->supl_rx.count;
	v->supl_src = m->supl_rx.src;
	v->supl_dst = m->supl_rx.dst;
	v->supl_group = m->supl_rx.group;
	v->supl_format = m->supl_rx.format;
	v->supl_len = m->supl_rx.len;
	memcpy(v->supl_data, m->supl_rx.data, sizeof(v->supl_data));
	v->supl_ms = m->supl_rx.ms;
#endif
	v->ptt_wait = sm->t3_ptt_wait;
	v->can_talk = t3_ms_can_talk(m);
	v->ticks = sm->t3_ticks;
	v->csbk = m->st.csbk;
	v->crc_bad = m->st.crc_bad;
	v->grants = m->st.grants;
	v->followed = m->st.followed;
	v->unmapped = m->st.unmapped;
	v->lent = m->st.lent;
	v->lent_calls = m->st.lent_calls;
	v->last_unmapped = m->st.last_unmapped;
	v->returns = m->st.returns;
	v->hunts = m->st.hunts;
	v->foreign = m->st.foreign;
	v->learned = m->st.learned;
	v->sent = m->st.sent;
	v->ra_tries = m->st.ra_tries;
	v->votes = m->st.votes;
	v->vote_moves = m->st.vote_moves;
	v->weak_leaves = m->st.weak_leaves;
	v->adj_refused = m->st.adj_refused;
	v->level_dbm = m->level_dbm;
	v->leave_dbm = t3s.cfg.leave_dbm;
	v->reg_area = m->reg_area;
	v->dmrla = t3s.cfg.dmrla;
	v->vote_on = m->vote.on;
	v->hunt_strict = m->hunt_strict;
	_Static_assert(T3_ADJ_MAX == sizeof v->adj / sizeof v->adj[0], "radio_t3_view_t.adj");
	v->nadj = m->nadj;
	for (uint8_t i = 0; i < m->nadj; i++) {
		v->adj[i].rx_hz = m->adj[i].chan.rx_hz;
		v->adj[i].sys14 = m->adj[i].sys14;
		v->adj[i].prio = (uint8_t)(m->adj[i].conf_prio << 4 | m->adj[i].adj_prio);
		v->adj[i].dbm = m->adj[i].dbm;
		v->adj[i].sampled_ms = m->adj[i].sampled_ms;
	}
	v->own_id = t3s.cfg.own_id;
}

// The settings' t3_stun for a stun of codeplug system sys (n + 1; 0 = a staged system, never kept)
static uint8_t t3_stun_word(uint8_t sys, uint8_t model, uint16_t net)
{
	if (sys == 0 || sys > RADIO_T3_SYSTEMS) {
		return 0;
	}
	uint8_t h = (uint8_t)(model ^ net ^ (net >> 4) ^ (net >> 8));
	return (uint8_t)(SETTINGS_T3_STUN | ((sys - 1u) << 4) | (h & 0x0Fu));
}

#if CONFIG_DMR_TRUNK_GW
// RREQ_T3_DIAL: the 16 BCD nibbles radio_req_t3_dial packs, into the system in use (the engine reads cfg.dial when
// the gateway asks); a new start copies the staged system's digits again
static void t3_dial(int32_t a, uint32_t b)
{
	uint32_t w[2] = { (uint32_t)a, b };
	uint8_t n = 0;
	memset(t3s.dial, 0xFF, sizeof t3s.dial);
	while (n < 16u) {
		uint8_t d = (uint8_t)(w[n / 8u] >> (28u - 4u * (n % 8u)) & 0xFu);
		if (d > 0xBu) {
			break;
		}
		t3s.dial[n / 2u] = (uint8_t)(n & 1u ? (t3s.dial[n / 2u] & 0xF0u) | d : (d << 4) | 0x0Fu);
		n++;
	}
	t3s.cfg.dial_n = n;
}
#endif

// A short data poll from the menus (6.6.5): the system's prearranged format and size; the answer comes as short data
// (into the inbox, t3_sd_inbox). A poll that can't start, is refused or goes unanswered counts in poll_fails
static void t3_poll(radio_sm_t *sm, uint32_t dst, uint32_t now_ms)
{
	radio_t3v.polls++;
#if CONFIG_DMR_TRUNK_TX
	uint8_t fmt = t3s.cfg.poll_len != 0 || t3s.cfg.poll_format != 0 ? t3s.cfg.poll_format : T3_UDTF_ISO8;
	uint8_t blocks = t3s.poll_blocks != 0 ? t3s.poll_blocks : T3_UDT_BLOCKS_MAX;
	if (sm->t3_on && !sm->t3_polling && t3_ms_sdata_poll(&t3m, now_ms, dst, fmt, blocks)) {
		sm->t3_polling = true;
		return;
	}
#else
	(void)dst;
	(void)now_ms;
#endif
	(void)sm;
	radio_t3v.poll_fails++;
}

static void t3_poll_tick(radio_sm_t *sm)
{
	const t3_ms_t *m = &t3m;
	if (!sm->t3_polling || (m->ax.svc == T3_SVC_SDPOLL && (m->ax.state != T3_AX_IDLE || m->ax.result == T3_RES_PENDING))) {
		return;
	}
	sm->t3_polling = false;
	if (m->ax.svc != T3_SVC_SDPOLL || m->ax.result != T3_RES_DONE) {
		radio_t3v.poll_fails++;
	}
}

// Short data text to the radio (6.6.4; a reply to its own poll too) goes into the message inbox as a received message.
// Other formats (binary, BCD, addresses, NMEA, IP) stay for the debug link only
static void t3_sd_inbox(radio_sm_t *sm)
{
	if (t3m.sd_rx.count == sm->t3_sd_seen) {
		return;
	}
	sm->t3_sd_seen = t3m.sd_rx.count;
#if CONFIG_DMR_DATA_CORE
	char text[(T3_UDT_MAX * 8) / 7 + 1];	// the most a UDT holds: ISO 7 bit, 52 characters (on the radio task's stack)
	if (!dmrDataDecodeUdtText(t3m.sd_rx.format, t3m.sd_rx.data, t3m.sd_rx.len * 8, text, sizeof text)) {
		return;
	}
	size_t n = strlen(text);
	while (n > 0 && text[n - 1] == 0x7F) {
		text[--n] = '\0';	// ISO 7 bit: the pad bits of the last octet, all ones, read as one more character
	}
	dmrDataServiceInboxAdd(t3m.sd_rx.src, t3m.sd_rx.dst, t3m.sd_rx.group, text);
#endif
}

// A stun or revive taken on a codeplug system: kept in the settings (written once the changes rest)
static void t3_keep_stun(radio_sm_t *sm, uint32_t now_ms)
{
	uint8_t want = t3m.stunned ? t3_stun_word(radio_t3v.sys, t3s.cfg.auth.model, t3s.cfg.auth.net) : 0u;
	if (want == sm->t3_stun_kept) {
		return;
	}
	settings_t *st = settings_begin();
	if (want != 0) {
		st->t3_stun = want;
	} else if (st->t3_stun == sm->t3_stun_kept) {
		st->t3_stun = 0;	// revived: only this system's stun goes
	}
	settings_end(now_ms);
	sm->t3_stun_kept = want;
}

// A kill taken (6.4.10.0: the radio "shall lose all DMR functionality"): kept in the settings, and the DMR stack stops
// now and never starts again until a factory reset clears it. FM stays
static void t3_keep_kill(radio_sm_t *sm, uint32_t now_ms)
{
	if (!t3m.killed || sm->dmr_killed) {
		return;
	}
	settings_t *st = settings_begin();
	st->t3_kill |= SETTINGS_T3_KILLED;
	settings_end(now_ms);
	sm->dmr_killed = true;
	sm->view.dmr.killed = true;
	if (sm->dmr_running) {
		dmr_end(sm);
		sm->view.dmr.active = false;
	}
	changed(sm);
}

// Start the engine on t3s
static void t3_begin(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->t3_on) {
		t3_ms_stop(&t3m);
		sm->t3_on = false;
	}
	if (t3s.cfg.own_id == 0) {
		t3s.cfg.own_id = sm->own_id;
	}
	if (t3s.cfg.nctrl == 0 || t3s.cfg.own_id == 0) {
		radio_t3v.unavail = true;	// nothing to hunt on, or no ID: shown, not silently idle
		t3_publish(sm);
		return;
	}
	if (t3s.cfg.leave_dbm == 0) {
		t3s.cfg.leave_dbm = sm->set.roam_rssi_dbm;	// the roaming threshold: weak below it (6.3.2.3)
	}
#if CONFIG_DMR_DATA_CORE
	// IP through the system: its gateway learns the radio's address at registration (6.4.11), the CAI address of
	// the ID the radio registers with
	t3s.cfg.ip_addr = t3s.ip_gw ? dmrDataIdAddress(false, t3s.cfg.reg_id != 0 ? t3s.cfg.reg_id : t3s.cfg.own_id) : 0u;
#endif
	// The view's counters start over with the engine; the channel's system and the fallback are the radio core's
	uint8_t sys = radio_t3v.sys, fallback = radio_t3v.fallback;		// not a whole copy: the view is big
	uint32_t fallbacks = radio_t3v.fallbacks, backs = radio_t3v.backs;
	uint16_t polls = radio_t3v.polls, poll_fails = radio_t3v.poll_fails;
	memset(&radio_t3v, 0, sizeof(radio_t3v));
	radio_t3v.polls = polls;
	radio_t3v.poll_fails = poll_fails;
	radio_t3v.sys = sys;
	radio_t3v.fallback = fallback;
	radio_t3v.fallbacks = fallbacks;
	radio_t3v.backs = backs;
	radio_t3v.ip_gw = t3s.ip_gw;
	radio_t3v.last_tx_op = 0xFF;
	sm->t3_tx_end_ms = 0;
	sm->t3_sent_report = 0;
	sm->t3_on = true;
	sm->t3_sending = false;
	sm->t3_tune_held = false;
	sm->t3_ptt_wait = false;
	sm->t3_als_keyed = false;
	sm->t3_als_quiet_ms = 0;
	sm->t3_talking = false;
	sm->t3_ticks = dmr_call_ticks(NULL);
	sm->t3_slot = 0;
	sm->t3_hunt_ms = now_ms | 1u;	// hunting from now (the fallback's clock)
	memset(sm->t3_at, 0, sizeof(sm->t3_at));
	sm->t3_cc = t3s.cc;
	dmr_call_set_rx_tap(t3_tap, sm);
	// A stun outlasts leaving the system and coming back (6.4.9): it is the network's, so another network's
	// doesn't carry over
	static int32_t last_net = -1;
	int32_t net = (int32_t)(((uint32_t)t3s.cfg.auth.model << 16) | t3s.cfg.auth.net);
	bool stunned = t3m.stunned && net == last_net;
	// ... and a restart, for a codeplug system: kept in the settings with the system's slot and network
	sm->t3_stun_kept = t3_stun_word(radio_t3v.sys, t3s.cfg.auth.model, t3s.cfg.auth.net);
	settings_t *st = settings_begin();
	bool kept = sm->t3_stun_kept != 0 && st->t3_stun == sm->t3_stun_kept;
	settings_end(now_ms);
	stunned = stunned || kept;
	if (!kept) {
		sm->t3_stun_kept = 0;
	}
	bool killed = t3m.killed || sm->dmr_killed;	// any network's
	last_net = net;
	t3_ms_start(&t3m, &t3s.cfg, now_ms);
	sm->t3_sd_seen = t3m.sd_rx.count;
	t3_ms_set_stunned(&t3m, stunned);
	t3_ms_set_killed(&t3m, killed);
	t3_publish(sm);
	changed(sm);
}

void radio_sm_t3_brief(radio_sm_t *sm)
{
	const radio_t3_view_t *x = &radio_t3v;
	radio_t3_brief_t *b = &sm->view.t3;
	b->on = x->on;
	b->sys = x->sys;
	b->unavail = x->unavail;
	b->fallback = x->fallback;
	b->fallbacks = x->fallbacks;
	b->backs = x->backs;
	b->state = x->state;
	b->reg = x->reg;
	b->polls = x->polls;
	b->poll_fails = x->poll_fails;
}

// RADIO_T3_START: the debug link's staged system, on whatever channel the radio is on
static void t3_start(radio_sm_t *sm, uint32_t now_ms)
{
	t3_off(sm);
	t3s = *radio_t3_staged();
	radio_t3_fix(&t3s);
	t3_begin(sm, now_ms);
}

static void t3_leave(radio_sm_t *sm)
{
	if (!sm->t3_on) {
		return;
	}
	t3_rx_sleep(sm, false, sm->t3_now);
	t3_ms_stop(&t3m);
	sm->t3_on = false;
#if CONFIG_DMR_DATA_CORE && CONFIG_DMR_TRUNK_TX
	if (data_tx.t3_wait) {
		data_t3_fail(DMR_TX_ERR_REJECTED);	// the system went before its channel came
	}
	data_tx.t3_sent = false;	// its data call went with the engine: not the next engine's to end
#endif
	sm->t3_both = false;
	sm->t3_ptt_wait = false;
	if (sm->t3_polling) {
		sm->t3_polling = false;
		radio_t3v.poll_fails++;		// left before the answer
	}
	dmr_call_set_rx_tap(NULL, NULL);
	t3_publish(sm);
}

// The system goes, and with it the fallback and the channel's tag (a conventional channel, the VFO, the debug link)
static void t3_off(radio_sm_t *sm)
{
	t3_leave(sm);
	sm->t3_pending = false;
	sm->t3_tagged = false;
	sm->t3_fb = 0;
	sm->t3_fb_number = 0;
	radio_t3v.sys = 0;
	radio_t3v.unavail = false;
	radio_t3v.fallback = 0;
}

// A channel selected (docs/spec/dmr-trunking-codeplug.md 4): a trunked one starts its system, or on the system running
// takes the channel's talk target and talkgroups; an unavailable one leaves any; a conventional one leaves it. True:
// the system running kept its engine and channel
static bool t3_channel(radio_sm_t *sm, uint16_t number, const channel_t *ch)
{
	if (number == 0 || ch->kind != CODEPLUG_KIND_DMR_TRUNK) {
		t3_off(sm);
		return false;
	}
	sm->t3_fb = 0;
	radio_t3v.fallback = 0;
	radio_t3v.unavail = false;
	sm->t3_tagged = true;
	if (sm->t3_on && t3s.sys == ch->system && sm->t3_ch_rx == ch->rx_only && sm->t3_kv == codeplug_kv_serial()) {
		// Another channel of the system running: the same TSCC, the new channel's talkgroups
		uint8_t ntg = t3s.cfg.ntg;
		t3_tg_t tg[T3_TG_MAX];
		memcpy(tg, t3s.cfg.tg, sizeof(tg));
		radio_t3_talk(ch, &t3s);
		sm->t3_fb_number = radio_t3_fallback(number, &t3s, &sm->t3_fb_ch);
		if (ntg != t3s.cfg.ntg || memcmp(tg, t3s.cfg.tg, ntg * sizeof(tg[0])) != 0) {
			t3_ms_groups_changed(&t3m, sm->t3_now);
		}
		return true;
	}
	t3_leave(sm);
	sm->t3_pending = false;
	// (a channel that names no system still shows as a trunked one)
	radio_t3v.sys = (uint8_t)(ch->system < CODEPLUG_SYSTEM_NONE ? ch->system + 1u : CODEPLUG_SYSTEM_NONE);
	if (!radio_t3_load(ch->system, &t3s)) {
		radio_t3v.unavail = true;	// no such system, no control channel, or no key where the system needs one
		sm->t3_fb_number = 0;
		return false;
	}
	if (t3s.cfg.own_id == 0 && sm->own_id == 0) {
		radio_t3v.unavail = true;	// no ID to register with: the system's record nor the radio's
		sm->t3_fb_number = 0;
		return false;
	}
	sm->t3_kv = codeplug_kv_serial();
	sm->t3_ch_rx = ch->rx_only;
	if (ch->rx_only) {
		// A receive-only channel never transmits (channel-modes.md 2.1): no registration from it either
		t3s.cfg.opts |= T3_OPT_LISTEN_ONLY;
	}
	radio_t3_talk(ch, &t3s);
	sm->t3_fb_number = radio_t3_fallback(number, &t3s, &sm->t3_fb_ch);
	sm->t3_pending = true;		// started by t3_tick, which has the clock
	return false;
}

// To the fallback channel (12.5): the engine stops, the radio works the channel as any conventional one
static void t3_to_fallback(radio_sm_t *sm, uint32_t now_ms)
{
	t3_leave(sm);
	if (sm->t3_fb == 0) {
		radio_t3v.fallbacks++;
	}
	sm->t3_fb = 1;
	radio_t3v.fallback = 1;
	load_channel(sm, &sm->t3_fb_ch);
	tune(sm, now_ms);
	sm->t3_probe_ms = now_ms + RADIO_T3_PROBE_MS;
	changed(sm);
}

// On the fallback channel: look for the TSCC every RADIO_T3_PROBE_MS while the channel is quiet
static void t3_fallback_tick(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->t3_fb != 1 || (int32_t)(now_ms - sm->t3_probe_ms) < 0) {
		return;
	}
	bool quiet = sm->view.state == RS_IDLE && !sm->ptt &&
		     (sm->view.last_rx_ms == 0 || now_ms - sm->view.last_rx_ms >= RADIO_T3_PROBE_QUIET_MS) &&
		     (sm->view.last_tx_ms == 0 || now_ms - sm->view.last_tx_ms >= RADIO_T3_PROBE_QUIET_MS);
	if (!quiet) {
		return;		// tried again at the next tick: the probe waits for the channel to go quiet
	}
	sm->t3_fb = 2;
	radio_t3v.fallback = 2;
	t3_begin(sm, now_ms);
	sm->t3_probe_ms = now_ms + RADIO_T3_PROBE_HUNT_MS;
}

static void t3_stop(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->t3_on) {
		return;
	}
	if (sm->t3_als_keyed) {
		sm->t3_als_keyed = false;
		ptt_release(sm, false, now_ms);
	}
	t3_off(sm);
	// Back to the channel or the VFO the user had
	channel_t ch;
	if (sm->view.vfo) {
		sm->view.ch = sm->vfo_ch;
	} else if (sm->have_channels && codeplug_channel(sm->view.channel_number, &ch)) {
		sm->view.ch = ch;
	}
	tune(sm, now_ms);
}

static void t3_request(radio_sm_t *sm, uint8_t cmd, uint32_t now_ms)
{
	switch (cmd) {
	case RADIO_T3_START:
		t3_start(sm, now_ms);
		break;
	case RADIO_T3_STOP:
		t3_stop(sm, now_ms);
		break;
#if CONFIG_DMR_TRUNK_TX
	case RADIO_T3_DEREG:
		if (sm->t3_on) {
			(void)t3_ms_deregister(&t3m, now_ms);
		}
		break;
	case RADIO_T3_END:
		if (sm->t3_on) {
			t3_ms_end_call(&t3m, now_ms);
		}
		break;
	case RADIO_T3_STATUS:
		if (sm->t3_on) {
			const radio_t3_setup_t *st = radio_t3_staged();
			(void)t3_ms_status(&t3m, now_ms, st->sd_dst, st->sd_group, st->sd_status);
		}
		break;
	case RADIO_T3_SDATA:
		if (sm->t3_on) {
			const radio_t3_setup_t *st = radio_t3_staged();
			(void)t3_ms_sdata(&t3m, now_ms, st->sd_dst, st->sd_group, st->sd_format, st->sd_data, st->sd_len);
		}
		break;
	case RADIO_T3_ALS:
		if (sm->t3_on) {
			const radio_t3_setup_t *st = radio_t3_staged();
			(void)t3_ms_request_als(&t3m, now_ms, st->sd_dst, st->sd_group);
		}
		break;
	case RADIO_T3_SDPOLL:
		if (sm->t3_on) {
			const radio_t3_setup_t *st = radio_t3_staged();
			(void)t3_ms_sdata_poll(&t3m, now_ms, st->sd_dst, st->sd_format, st->sd_len);
		}
		break;
	case RADIO_T3_INTERRUPT:
	case RADIO_T3_ALS_END:
		if (sm->t3_on && !sm->ptt) {
			(void)t3_ms_interrupt(&t3m, now_ms, cmd == RADIO_T3_ALS_END);
		}
		break;
	case RADIO_T3_DIVERT:
		if (sm->t3_on) {
			const radio_t3_setup_t *st = radio_t3_staged();
			if (st->div_mode == RADIO_T3_DIV_SET) {
				(void)t3_ms_divert(&t3m, now_ms, st->div_svcs, st->sd_dst, st->sd_group);
			} else {
				(void)t3_ms_divert_clear(&t3m, now_ms, st->div_svcs, st->div_mode == RADIO_T3_DIV_CLEAR_IN);
			}
		}
		break;
#endif
	default:
		break;
	}
}

// PTT on a trunked system. True: handled here (a request on the control channel; PTT stays held while it runs)
static bool t3_ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->t3_on) {
		return false;
	}
	if (t3_ms_can_talk(&t3m)) {
		sm->t3_talking = true;
		return false;	// on the payload channel: the stack sends the voice
	}
#if CONFIG_DMR_TRUNK_TX
	bool group;
	uint32_t dst = t3_talk_id(sm, &group);
	bool emergency = false;
#if CONFIG_DMR_EMERGENCY
	emergency = dmr_emergency_in_progress();
#endif
	if (t3m.state == T3_CAMPED && dst != 0 && t3_ms_request(&t3m, now_ms, dst, group, emergency)) {
		sm->t3_ptt_wait = true;
		sm->ptt = true;
		changed(sm);
		return true;
	}
#endif
	deny(sm, TXDENY_NO_DMR);
	return true;
}

static void t3_ptt_up(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->t3_on) {
		return;
	}
#if CONFIG_DMR_TRUNK_TX
	if (sm->t3_ptt_wait) {
		sm->t3_ptt_wait = false;
		if (t3m.ax.state != T3_AX_IDLE) {
			t3_ms_cancel(&t3m, now_ms);
		}
	}
#endif
	sm->t3_talking = false;
	(void)now_ms;
}

#if CONFIG_DMR_TRUNK_TX
// An Ambient Listening call to the radio (6.6.2.6.3): it transmits with no one at the PTT until T_ALS, then ends the
// call (terminator, then P_MAINT). A user's PTT meanwhile is the user's
static void t3_als_tick(radio_sm_t *sm, uint32_t now_ms)
{
	bool want = t3_ms_als_tx(&t3m, now_ms);
	if (want && !sm->t3_als_keyed && !sm->ptt) {
		ptt_down(sm, now_ms);
		sm->t3_als_keyed = sm->ptt;
		sm->t3_als_quiet_ms = 0;
	} else if (!want && sm->t3_als_keyed) {
		sm->t3_als_keyed = false;
		ptt_release(sm, false, now_ms);
		sm->t3_als_quiet_ms = now_ms | 1u;
	}
	if (sm->t3_als_quiet_ms != 0 && (int32_t)(now_ms - sm->t3_als_quiet_ms) >= 360) {	// quiet may be 1 ms ahead
		sm->t3_als_quiet_ms = 0;
		if (t3_ms_als_over(&t3m, now_ms)) {
			t3_ms_end_call(&t3m, now_ms);
		}
	}
}
#endif

#if CONFIG_DMR_TRUNK_TX && CONFIG_DMR_EMERGENCY
// An emergency alarm heard (status 126) raises the alert; the radio's own emergency over, its alarm is canceled with
// status 124 to the same target (361-4 6.6.6.3.2)
static void t3_emergency_tick(uint32_t now_ms)
{
	const t3_ms_t *m = &t3m;
	if (m->status_rx.count != t3_em.status_seen) {
		t3_em.status_seen = m->status_rx.count;
		if (m->status_rx.value == T3_STATUS_EMERGENCY) {
			dmr_emergency_heard(DMR_EMERGENCY_ALERT_ALARM, m->status_rx.src, m->status_rx.dst, m->status_rx.group,
					    now_ms);
		}
	}
	if (t3_em.alarm_sent && !dmr_emergency_in_progress() && t3s.sys == t3_em.alarm_sys && m->state == T3_CAMPED &&
	    m->ax.state == T3_AX_IDLE && !data_tx.t3_wait &&
	    t3_ms_status(&t3m, now_ms, t3_em.alarm_dst, t3_em.alarm_group, T3_STATUS_EMERGENCY_CANCEL)) {
		t3_em.alarm_sent = false;
	}
}
#endif

// Each radio tick while a system is selected: the slots since the last tick, then the engine's actions
static void t3_tick(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->t3_pending) {
		sm->t3_pending = false;
		t3_begin(sm, now_ms);
	}
	t3_fallback_tick(sm, now_ms);
	if (!sm->t3_on) {
		return;
	}
	uint32_t tc = dmr_call_ticks(NULL);
	uint32_t n = tc - sm->t3_ticks;
	if (n > 8u) {
		n = 8u;		// a stall: the engine only needs the frames to count, not every one
	}
	for (uint32_t i = 0; i < n; i++) {
		// The tick's slot when the modem knows it, else alternating; its CACH's AT bit when the modem reads it. A
		// modem that never reports AT gets every slot as free, so random access still runs (docs 7)
		int8_t kts;
		uint8_t fl = dmr_call_tick_at(tc - n + 1u + i, &kts);
		uint8_t slot = (kts == 1 || kts == 2) ? (uint8_t)kts : (sm->t3_slot == 1 ? 2u : 1u);
		sm->t3_slot = slot;
		// fl is the AT of the CACH naming this slot; the one for this slot's inbound may be the other's (t3_ms_at_tc)
		sm->t3_at[slot - 1u] = fl;
		fl = sm->t3_at[t3_ms_at_tc(&t3m, slot) - 1u];
		bool known = (fl & DMR_TICK_AT_KNOWN) != 0;
		if (known) {
			radio_t3v.at_known++;
			if (fl & DMR_TICK_AT_BUSY) {
				radio_t3v.at_busy++;
			}
		}
		t3_ms_slot(&t3m, now_ms, slot, true, known && (fl & DMR_TICK_AT_BUSY) != 0);
	}
	sm->t3_ticks = tc;
	sm->t3_now = now_ms;
	// The stack's report on our last burst list, after the frames before it were counted: a random access burst's
	// answer window runs from here
	if (sm->t3_sent_report != 0) {
		t3_ms_sent(&t3m, now_ms, sm->t3_sent_report == 1u);
		sm->t3_sent_report = 0;
	}
	// The CACH short LC (C_SYS_Parms): the system's identity check and the Common_Slot_Counter power save counts by
	uint32_t slc, slc_bad;
	uint32_t slc_n = dmr_call_short_lc(&slc, &slc_bad);
	if (slc_n != sm->t3_slc_n) {
		sm->t3_slc_n = slc_n;
		t3_ms_slc(&t3m, now_ms, slc);
	}
	radio_t3v.slc_ok = slc_n;
	radio_t3v.slc_bad = slc_bad;
	radio_t3v.ticks += n;
	// Talking (or sending data) on the payload channel: the radio doesn't hear its own slot, so its transmission is the
	// call's activity (TV_Inactive would otherwise pull it back to the control channel mid-sentence)
	if (n != 0 && t3m.state == T3_FOLLOW && (sm->view.state == RS_TX || sm->dmr_keyed)) {
		t3_ms_activity(&t3m, now_ms, sm->view.ch.timeslot);
	}

	if (n != 0 && t3m.state != T3_FOLLOW) {
		t3_ms_level(&t3m, now_ms, sm->view.rssi_dbm);	// once a frame: the TSCC's or the one hunted or sampled
	}

	if (sm->t3_tune_held && (!sm->t3_sending || now_ms - sm->t3_tune_ms >= RADIO_T3_TUNE_HOLD_MS)) {
		t3_retune(sm, &sm->t3_tune_chan, sm->t3_tune_slot, now_ms);
	}
	t3_act_t a;
	while (t3_ms_poll(&t3m, now_ms, &a)) {
		if (a.type == T3_ACT_TUNE) {
			t3_tune(sm, &a, now_ms);
		} else if (a.type == T3_ACT_SEND) {
			t3_send(sm, &a, now_ms);
		}
	}
	t3_rx_sleep(sm, t3_ms_asleep(&t3m) && board.radio->rx_sleep_dmr && board.radio->rx_sleep != NULL &&
		    sm->board_mode == RADIO_MODE_DMR && sm->view.state == RS_IDLE && !sm->ptt, now_ms);
#if CONFIG_DMR_DATA_CORE && CONFIG_DMR_TRUNK_TX
	data_t3_tick(sm, now_ms);
#endif
#if CONFIG_DMR_TRUNK_TX && CONFIG_DMR_EMERGENCY
	t3_emergency_tick(now_ms);
#endif
	// Fallback (12.5): hunting with no TSCC for the hold time goes to it; a probe from it that finds the TSCC stays
	if (t3m.state != T3_HUNT) {
		sm->t3_hunt_ms = 0;
	} else if (sm->t3_hunt_ms == 0) {
		sm->t3_hunt_ms = now_ms | 1u;
	}
	if (sm->t3_fb == 2 && t3m.state == T3_CAMPED && t3m.confirmed) {
		// Found from the fallback channel: the trunked channel again (the engine tunes the radio)
		channel_t ch;
		if (codeplug_channel(sm->view.channel_number, &ch)) {
			load_channel(sm, &ch);
		}
		sm->t3_fb = 0;
		radio_t3v.fallback = 0;
		radio_t3v.backs++;
		beep(sm, BEEP_SYSTEM_BACK);
		changed(sm);
	} else if (sm->t3_fb == 2 && (int32_t)(now_ms - sm->t3_probe_ms) >= 0) {
		t3_to_fallback(sm, now_ms);	// not found: back to the fallback channel
		return;
	} else if (sm->t3_fb == 0 && sm->t3_fb_number != 0 && sm->t3_hunt_ms != 0 &&
		   (int32_t)(now_ms - sm->t3_hunt_ms) >=
			   (int32_t)(t3s.fb_hold_s != 0 ? t3s.fb_hold_s : RADIO_T3_FALLBACK_HOLD_S) * 1000) {
		t3_to_fallback(sm, now_ms);
		return;
	}
#if CONFIG_DMR_TRUNK_TX
	t3_als_tick(sm, now_ms);
	// Our grant came while PTT is held: talk now
	if (sm->t3_ptt_wait) {
		if (t3_ms_can_talk(&t3m) && t3m.state == T3_FOLLOW) {
			sm->t3_ptt_wait = false;
			sm->t3_talking = true;
			sm->ptt = false;	// ptt_down runs again on the payload channel
			ptt_down(sm, now_ms);
		} else if (t3m.ax.state == T3_AX_IDLE && t3m.ax.result != T3_RES_PENDING &&
			   t3m.ax.result != T3_RES_GRANTED) {
			sm->t3_ptt_wait = false;	// refused, no answer, timed out
			deny(sm, TXDENY_NO_DMR);
		}
	}
#endif
	t3_keep_stun(sm, now_ms);
	t3_keep_kill(sm, now_ms);
	t3_sd_inbox(sm);
	t3_poll_tick(sm);
	t3_publish(sm);
}
#endif

#else
static void dmr_begin(radio_sm_t *sm) { (void)sm; }
#if CONFIG_AIR
static void dmr_end(radio_sm_t *sm) { air_stop(sm); }
#else
static void dmr_end(radio_sm_t *sm) { (void)sm; }
#endif
#endif

// --- Scan (radio/scan.c decides; this tunes) ---
#if CONFIG_SCAN
// A channel scan and dual watch can listen to (number: where it is stored, 0 the VFO): not parked, and one a stop can
// judge. A P25 trunked channel is left out: it follows its system's control channel, nothing a stop of a few hundred
// milliseconds decides, and so is a Tier III one. An NXDN channel at a rate the board's air interface can't receive hears nothing
static bool scan_listens(const channel_t *ch, uint16_t number)
{
	(void)number;
	if (parked(ch) || ch->kind == CODEPLUG_KIND_P25_TRUNK || ch->kind == CODEPLUG_KIND_DMR_TRUNK) {
		return false;
	}
#if CONFIG_NXDN
	if (ch->mode == RADIO_MODE_NXDN && !radio_nxdn_receives(ch, number)) {
		return false;
	}
#endif
	return true;
}

static bool scan_usable(uint16_t pos, void *ctx)
{
	const radio_sm_t *sm = ctx;
	channel_t ch;
	return pos < sm->zone.count && channel_read(sm->zone.channels[pos], &ch) && !ch.scan_skip &&
	       scan_listens(&ch, sm->zone.channels[pos]);
}

// Dual watch (SCAN_DUAL): the receiver alternates between two channels, dual[0] and dual[1], skip flags ignored. With
// CONFIG_DUAL_WATCH each is a channel of any zone or the VFO, and PTT talks on the selected one without ending the
// watch; without it, two channels of the current zone (the debug link's) and PTT ends it as any scan

static bool dual_running(const radio_sm_t *sm)
{
	return sm->scan.kind == SCAN_DUAL;
}

static void dual_busy_set(radio_sm_t *sm, uint8_t busy)
{
	if (sm->view.dual_busy != busy) {
		sm->view.dual_busy = busy;
		changed(sm);
	}
}

// The receiver is on the other channel for a look (dual_wait_service): its squelch stays shut
static bool dual_peeking(const radio_sm_t *sm)
{
#if CONFIG_DUAL_WATCH
	return sm->dual_peek && dual_running(sm);
#else
	(void)sm;
	return false;
#endif
}

static bool dual_same(const radio_dual_ref_t *a, const radio_dual_ref_t *b)
{
	return a->vfo == b->vfo && a->pos == b->pos && (a->vfo || a->zone == b->zone);
}

// The receiver is on channel i now
static bool dual_here(const radio_sm_t *sm, uint8_t i)
{
	const radio_dual_ref_t *r = &sm->dual[i];
	return r->vfo ? sm->view.vfo && sm->vfo_idx == r->pos :
			!sm->view.vfo && r->zone == sm->view.zone_index && r->pos == sm->view.zone_pos;
}

// A channel's zone (the current one, or read into buf) and its settings; NULL when it can't be watched. buf may be
// NULL without CONFIG_DUAL_WATCH: only the current zone then
static const zone_t *dual_read(const radio_sm_t *sm, const radio_dual_ref_t *r, zone_t *buf, channel_t *ch)
{
	const zone_t *z = NULL;
	if (r->vfo) {
		return NULL;
	}
	if (sm->have_channels && r->zone == sm->view.zone_index) {
		z = &sm->zone;
	}
#if CONFIG_DUAL_WATCH
	else if (read_zone(r->zone, buf)) {
		z = buf;
	}
#else
	(void)buf;
#endif
	return z != NULL && r->pos < z->count && channel_read(z->channels[r->pos], ch) &&
	       scan_listens(ch, z->channels[r->pos]) ? z : NULL;
}

static bool dual_ok(const radio_sm_t *sm, const radio_dual_ref_t *r)
{
	if (r->vfo) {
		return CONFIG_DUAL_WATCH && VFO_AVAILABLE;
	}
	channel_t ch;
#if CONFIG_DUAL_WATCH
	zone_t z;
	return dual_read(sm, r, &z, &ch) != NULL;
#else
	return dual_read(sm, r, NULL, &ch) != NULL;
#endif
}

#if CONFIG_DUAL_WATCH
// Channel i's changes for now, as they are while the receiver is on it
static void dual_keep_save(radio_sm_t *sm, uint8_t i)
{
	radio_dual_keep_t *k = &sm->dual_keep[i];
	const channel_t *ch = &sm->view.ch;
	k->valid = true;
	k->temp_power = sm->temp_power;
	k->power = sm->view.power;
	k->temp_squelch = sm->temp_squelch;
	k->squelch = sm->view.squelch;
	k->temp_ts = sm->temp_ts;
	k->bw = ch->bw;
	k->mode = ch->mode;
	k->color_code = ch->color_code;
	k->timeslot = ch->timeslot;
	k->rx_tone = ch->rx_tone;
	k->tx_tone = ch->tx_tone;
#if CONFIG_DMR
	k->manual = sm->manual && sm->manual_ch == sm->view.channel_number && sm->manual_vfo == sm->view.vfo;
	k->manual_id = sm->manual_id;
	k->manual_private = sm->manual_private;
	k->manual_ts = sm->manual_ts;
	k->hear_all_latch = sm->hear_all_latch && sm->latch_ch == sm->view.channel_number &&
			    sm->latch_vfo == sm->view.vfo;
#endif
}

// ... and back on it (the channel or the VFO just loaded, not tuned yet)
static void dual_keep_load(radio_sm_t *sm, uint8_t i)
{
	const radio_dual_keep_t *k = &sm->dual_keep[i];
	if (!k->valid) {
		return;
	}
	if (k->temp_power) {
		sm->temp_power = true;
		sm->view.power = k->power;
	}
	if (k->temp_squelch) {
		sm->temp_squelch = true;
		sm->view.squelch = k->squelch;
	}
	if (!sm->view.vfo) {	// the VFO keeps its own settings
		channel_t *ch = &sm->view.ch;
		ch->bw = k->bw;
		ch->mode = k->mode;
		ch->color_code = k->color_code;
		ch->timeslot = k->timeslot;
		ch->rx_tone = k->rx_tone;
		ch->tx_tone = k->tx_tone;
		sm->temp_ts = k->temp_ts;
	}
	set_temporary(sm);
#if CONFIG_DMR
	sm->manual = k->manual;
	sm->manual_id = k->manual_id;
	sm->manual_private = k->manual_private;
	sm->manual_ts = k->manual_ts;
	sm->manual_ch = sm->view.channel_number;
	sm->manual_vfo = sm->view.vfo;
	sm->hear_all_latch = k->hear_all_latch;
	sm->latch_ch = sm->view.channel_number;
	sm->latch_vfo = sm->view.vfo;
#endif
}
#endif

// Tune channel i (the VFO with its own settings), with its changes for now. False when it can't be read now (the
// codeplug changed)
static bool dual_tune(radio_sm_t *sm, uint8_t i, uint32_t now_ms)
{
	const radio_dual_ref_t *r = &sm->dual[i];
	channel_t ch;
#if CONFIG_DUAL_WATCH
	sm->dual_peek = false;	// a look at the other channel ends with any tune (dual_wait_service starts one after)
	if (sm->dual_at != i && dual_here(sm, sm->dual_at)) {
		dual_keep_save(sm, sm->dual_at);
	}
	if (r->vfo) {
		if (!VFO_AVAILABLE) {
			return false;
		}
		vfo_use(sm, (uint8_t)r->pos);	// each channel its own VFO
		enter_vfo(sm, now_ms);
		dual_keep_load(sm, i);
#if CONFIG_DMR
		if (sm->dmr_running) {
			dmr_configure(sm);	// a talk target or the hear all latch kept
		}
#endif
		sm->dual_at = i;
		return true;
	}
	zone_t buf;
	const zone_t *z = dual_read(sm, r, &buf, &ch);
#else
	const zone_t *z = dual_read(sm, r, NULL, &ch);
#endif
	if (z == NULL) {
		return false;
	}
	if (z != &sm->zone) {
		set_zone(sm, r->zone, z);	// the view reports the tuned channel's zone, as it does its position
	}
	set_channel(sm, r->pos, &ch);
#if CONFIG_DUAL_WATCH
	dual_keep_load(sm, i);
#endif
	tune(sm, now_ms);
	sm->dual_at = i;
	return true;
}

#if CONFIG_DUAL_WATCH
// Two receivers (board.rx2, hal_rx2.h): the main receiver stays on the selected channel and the second one gets the
// other, so dual watch hears both at once. While the second one is down or refuses the channel, the main receiver
// alternates as on any radio, and the second is tried again after RADIO_RX2_RETRY_MS

// Channel i's settings for the second receiver. False: it can't be read now
static bool rx2_settings(radio_sm_t *sm, uint8_t i, rx2_tune_t *t)
{
	static zone_t buf;		// only the radio task uses it
	const radio_dual_ref_t *r = &sm->dual[i];
	channel_t ch;
	memset(t, 0, sizeof(*t));
	if (r->vfo) {
		if (!VFO_AVAILABLE) {
			return false;
		}
		ch = r->pos == sm->vfo_idx ? sm->vfo_ch : sm->vfo_other.ch;
	} else if (dual_read(sm, r, &buf, &ch) == NULL) {
		return false;
	}
	if (air_mode(ch.mode)) {
		return false;	// the second receiver takes FM and DMR: the main one alternates onto a P25 or NXDN channel
	}
	t->rx_hz = ch.rx_hz;
	t->mode = ch.mode;
	t->bw = ch.bw;
	t->squelch = sm->view.squelch;
	t->rx_tone = ch.rx_tone;
#if CONFIG_DMR
	if (ch.mode == RADIO_MODE_DMR) {
		static listen_list_t list;	// only the radio task uses it
		contact_t c;
		t->color_code = ch.color_code & 0x0Fu;
		t->timeslot = ch.timeslot == 2 ? 2 : 1;
		t->own_id = ch.dmr_id != 0 ? ch.dmr_id : sm->own_id;
		t->talk_group = 9;
		if (r->vfo) {
			bool cur = r->pos == sm->vfo_idx;
			uint32_t id = cur ? sm->vfo_talk_id : sm->vfo_other.talk_id;
			if (id != 0) {
				t->talk_group = (cur ? sm->vfo_talk_private : sm->vfo_other.talk_private) ? 0 : id;
			}
		} else {
			uint16_t talk = codeplug_channel_talk_contact(&ch);
			if (talk != 0 && codeplug_contact(talk, &c)) {
				t->talk_group = c.type == CONTACT_PRIVATE ? 0 : c.id;
			}
			if (ch.listen_list != 0 && codeplug_listen_list(ch.listen_list, &list)) {
				for (uint8_t k = 0; k < list.count && t->group_count < RX2_GROUPS; k++) {
					if (codeplug_contact(list.contacts[k], &c) && c.type != CONTACT_PRIVATE) {
						t->groups[t->group_count++] = c.id;
					}
				}
			}
		}
	}
#endif
	return true;
}

static void rx2_publish(radio_sm_t *sm)
{
	if (sm->view.rx2 != sm->rx2_on) {
		sm->view.rx2 = sm->rx2_on;
		changed(sm);
	}
}

static void rx2_release(radio_sm_t *sm)
{
	if (sm->rx2_on) {
		sm->rx2_on = false;
		board.rx2->stop();
		outputs(sm);
		rx2_publish(sm);
	}
}

// Give the second receiver the channel not selected, and the main receiver the selected one. False: it can't now
static bool rx2_engage(radio_sm_t *sm, uint32_t now_ms)
{
	rx2_tune_t t;
	sm->rx2_try_ms = sm->rx2_refresh_ms = now_ms;
	if (board.rx2 == NULL || !rx2_settings(sm, (uint8_t)!sm->dual_sel, &t) || !board.rx2->tune(&t)) {
		rx2_release(sm);
		return false;
	}
	if (!dual_here(sm, sm->dual_sel)) {
		if (!dual_tune(sm, sm->dual_sel, now_ms)) {
			rx2_release(sm);
			return false;
		}
		scan_tuned(&sm->scan, now_ms);
	}
	sm->dual_at = sm->dual_sel;
	sm->rx2_on = true;
	rx2_publish(sm);
	return true;
}

// Every poll in dual watch on a radio with a second receiver. True: it has the other channel (nothing to alternate)
static bool rx2_service(radio_sm_t *sm, uint32_t now_ms)
{
	rx2_status_t st;
	board.rx2->status(&st);
	if (!sm->rx2_on) {
		if (st.up && now_ms - sm->rx2_try_ms >= RADIO_RX2_RETRY_MS) {
			return rx2_engage(sm, now_ms);
		}
		return false;
	}
	if (!st.up || (!st.tuned && now_ms - sm->rx2_try_ms >= RADIO_RX2_TUNE_MS)) {
		rx2_release(sm);	// gone, or it won't take the channel: alternate until it's back
		sm->rx2_try_ms = now_ms;
		return false;
	}
	if (st.tuned) {
		sm->rx2_try_ms = now_ms;	// the deadline runs from the last time it was on its settings
	}
	if (now_ms - sm->rx2_refresh_ms >= RADIO_RX2_REFRESH_MS) {
		rx2_tune_t t;
		sm->rx2_refresh_ms = now_ms;
		if (rx2_settings(sm, (uint8_t)!sm->dual_sel, &t)) {
			board.rx2->tune(&t);	// a codeplug edit or a squelch change; the same settings again send nothing
		}
	}
	if (!dual_here(sm, sm->dual_sel) && dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_tuned(&sm->scan, now_ms);
	}
	return true;
}
#else
static void rx2_release(radio_sm_t *sm) { (void)sm; }
#endif

static void scan_publish(radio_sm_t *sm)
{
	const scan_t *sc = &sm->scan;
	if (sm->view.scan != sc->kind || sm->view.scan_phase != sc->phase || sm->view.scan_skipped != sc->skipped) {
		sm->view.scan = sc->kind;
		sm->view.scan_phase = sc->phase;
		sm->view.scan_skipped = sc->skipped;
		sm->view.scan_probes = (uint16_t)sc->probes;	// with the phase changes, not a change of their own
		sm->view.scan_synced = (uint16_t)sc->stats_synced;
		changed(sm);
	}
}

// End the scan: stay where it is, or (cancel) go back to where it started
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc)) {
		return;
	}
	uint8_t kind = sc->kind;
	if (kind == SCAN_DUAL) {
		rx2_release(sm);
		dual_busy_set(sm, 0);
	}
#if CONFIG_DMR
	bool hear_all = hear_all_on(sm);	// never in dual watch (hear_all_on)
#endif
	scan_end(sc);
	if (cancel) {
		if (kind == SCAN_DUAL) {
			if (!dual_here(sm, sm->dual_sel)) {
				dual_tune(sm, sm->dual_sel, now_ms);	// dual watch: back on the selected channel
			}
		} else if (kind == SCAN_VFO) {
			if (sm->view.ch.rx_hz != sc->origin_hz) {
				vfo_set_hz(sm, sc->origin_hz);
				tune(sm, now_ms);
			}
		} else if (sm->view.zone_pos != sc->origin_pos) {
			channel_t ch;
			if (channel_read(sm->zone.channels[sc->origin_pos], &ch)) {
				set_channel(sm, sc->origin_pos, &ch);
				tune(sm, now_ms);
			}
		}
	}
#if CONFIG_DMR
	// A scan with digital monitor leaves it on, latched on the channel it ends on (the stack still hears all)
	sm->scan_hear_all = false;
	if (hear_all && sm->view.ch.mode == RADIO_MODE_DMR) {
		set_hear_all_latch(sm, true);
	}
#endif
	scan_publish(sm);
}

#define SCAN_NO_POS	0xFFFFu

// Tune zone position pos as a scan stop, outside the sweep's order (the sweep goes on from where it was)
static bool scan_tune_pos(radio_sm_t *sm, uint16_t pos, uint32_t now_ms)
{
	channel_t ch;
	if (pos >= sm->zone.count || !channel_read(sm->zone.channels[pos], &ch)) {
		return false;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
	scan_tuned(&sm->scan, now_ms);
	return true;
}

// The first scannable digital channel (DMR, P25, NXDN) of the zone from position from on, on the receive frequency of
// the FM stop at position fm, -1 if none
static int scan_twin_pos(const radio_sm_t *sm, uint16_t fm, uint16_t from)
{
	for (uint16_t pos = from; pos < sm->zone.count; pos++) {
		channel_t ch;
		if (pos == fm || scan_pos_skipped(&sm->scan, pos) || !channel_read(sm->zone.channels[pos], &ch) ||
		    ch.scan_skip || ch.rx_hz != sm->view.ch.rx_hz || !scan_listens(&ch, sm->zone.channels[pos])) {
			continue;
		}
		if ((CONFIG_DMR && ch.mode == RADIO_MODE_DMR) || air_mode(ch.mode)) {
			return pos;
		}
	}
	return -1;
}

// An FM channel and a digital one on the same frequency (a mixed zone): a digital signal opens the FM squelch, and a
// scan that held the FM stop would sit through the call there, never reaching the channel that plays it. So before an
// FM stop holds, the scan checks the zone's digital channels of that frequency in turn (DMR, P25 and NXDN may share
// it), each as a stop with the usual probe: a call there holds it, and a stop that heard its mode's signal (a call not
// for us included) goes on with the sweep. When none heard its mode it was an FM signal: the scan goes back to the FM
// stop, which holds this time. True when it retuned (poll's action is spent). heard: this stop heard its own digital
// mode
static bool scan_mixed(radio_sm_t *sm, uint8_t was, scan_act_t act, bool heard, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sc->kind != SCAN_ZONE) {
		return false;
	}
	if (sm->scan_twin_fm != SCAN_NO_POS) {
		// On the digital channel for the check: a hold or the probe stays here; scan_step clears the check
		sm->scan_twin_digital |= heard;
		if (act != SCAN_ACT_NEXT || sm->scan_twin_digital) {
			return false;
		}
		uint16_t fm = sm->scan_twin_fm;
		int next = scan_twin_pos(sm, fm, (uint16_t)(sm->view.zone_pos + 1u));
		if (next >= 0 && scan_tune_pos(sm, (uint16_t)next, now_ms)) {
			return true;	// the frequency's next digital channel
		}
		sm->scan_twin_fm = SCAN_NO_POS;
		if (!scan_tune_pos(sm, fm, now_ms)) {
			return false;
		}
		sm->scan_fm_heard = fm;
		return true;
	}
	bool holds = sc->phase == SCAN_PH_HOLD && was != SCAN_PH_HOLD && was != SCAN_PH_RESUME;
	if (!holds || sm->view.ch.mode != RADIO_MODE_FM || sm->view.zone_pos == sm->scan_fm_heard) {
		return false;
	}
	int twin = scan_twin_pos(sm, sm->view.zone_pos, 0);
	uint16_t fm = sm->view.zone_pos;
	if (twin < 0 || !scan_tune_pos(sm, (uint16_t)twin, now_ms)) {
		return false;
	}
	sc->holds--;		// not a hold after all
	sm->scan_twin_fm = fm;
	sm->scan_twin_digital = false;
	return true;
}

// Move to the next stop. False (and the scan ends) when there is none
static bool scan_step(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	sm->scan_twin_fm = sm->scan_fm_heard = SCAN_NO_POS;	// a mixed frequency's check ends with its stop (scan_mixed)
	if (sc->kind == SCAN_DUAL) {
		if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << sm->dual_at)));	// the watch hears it itself now
	} else if (sc->kind == SCAN_VFO) {
		uint32_t hz = scan_next_hz(sc, sm->view.ch.rx_hz, sm->view.vfo_step_hz);
		if (hz == 0 || !board_in_band(hz, false)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		vfo_set_hz(sm, hz);
		tune(sm, now_ms);
	} else {
		int pos = scan_next_pos(sc, sm->zone.count, scan_usable, sm);
		channel_t ch;
		if (pos < 0 || !channel_read(sm->zone.channels[pos], &ch)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		set_channel(sm, (uint16_t)pos, &ch);
		tune(sm, now_ms);
	}
	scan_tuned(sc, now_ms);
	scan_publish(sm);
	return true;
}

static void scan_config(const radio_sm_t *sm, uint8_t kind, scan_cfg_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->dwell_ms = sm->set.scan_dwell_ms < SCAN_SETTLE_MS ? SCAN_SETTLE_MS : sm->set.scan_dwell_ms;
	cfg->hold_ms = (uint16_t)(sm->set.scan_hold_s * 1000u);
	cfg->delay_ms = sm->set.scan_delay_ms;
	cfg->resume = sm->set.scan_resume;
	cfg->priority = sm->set.scan_priority;
	if (kind == SCAN_DUAL && cfg->resume == SCAN_RESUME_STOP) {
		cfg->resume = SCAN_RESUME_CARRIER;	// a signal holds dual watch; it doesn't end it
	}
	if (kind == SCAN_DUAL) {
		cfg->delay_ms = sm->set.dual_delay_ms;
	}
}

static void scan_start(radio_sm_t *sm, uint8_t kind, uint32_t lo_hz, uint32_t hi_hz, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (scan_on(sc)) {
		scan_stop(sm, false, now_ms);
	}
	if (kind == SCAN_VFO) {
		if (!sm->view.vfo) {
			return;
		}
		if (lo_hz == 0 && hi_hz == 0) {
			// The band the VFO is in
			for (uint8_t i = 0; i < board_band_count(); i++) {
				if (sm->view.ch.rx_hz >= board.bands[i].lo_hz && sm->view.ch.rx_hz <= board.bands[i].hi_hz) {
					lo_hz = board.bands[i].lo_hz;
					hi_hz = board.bands[i].hi_hz;
				}
			}
		}
		// Both ends in one band, and room for two stops
		bool same_band = false;
		for (uint8_t i = 0; i < board_band_count(); i++) {
			const band_t *b = &board.bands[i];
			if (lo_hz >= b->lo_hz && hi_hz <= b->hi_hz) {
				same_band = true;
			}
		}
		if (!same_band || hi_hz < lo_hz + sm->view.vfo_step_hz || !scan_listens(&sm->view.ch, 0)) {
			return;
		}
	} else if (sm->view.vfo || !sm->have_channels) {
		return;
	}
	scan_cfg_t cfg;
	scan_config(sm, kind, &cfg);
#if CONFIG_DMR
	// Digital monitor on here (latched on this channel, not one left behind): on at every stop (#595)
	sm->scan_hear_all = sm->hear_all_latch && sm->latch_ch == sm->view.channel_number &&
			    sm->latch_vfo == sm->view.vfo;
#endif
	sm->scan_twin_fm = sm->scan_fm_heard = SCAN_NO_POS;
	sc->origin_vfo = sm->view.vfo;
	sc->origin_pos = sm->view.zone_pos;
	sc->origin_hz = sm->view.ch.rx_hz;
	sc->lo_hz = lo_hz;
	sc->hi_hz = hi_hz;
	scan_begin(sc, kind, &cfg, now_ms);
	if (kind != SCAN_VFO) {
		// Nothing to scan when no other channel qualifies
		scan_t probe = *sc;
		int next = scan_next_pos(&probe, sm->zone.count, scan_usable, sm);
		if (next < 0 || next == sm->view.zone_pos) {
			scan_end(sc);
			return;
		}
	}
	activity(sm, now_ms);
	scan_step(sm, now_ms);
}

// Dual watch on ref[0] and ref[1], ref[sel] selected. Asked again with the same pair: only the selection changes. A
// new pair keeps the receiver where it is (a call it holds too) if that channel is still one of the two, else it
// starts on the selected one
static void dual_start(radio_sm_t *sm, const radio_dual_ref_t ref[2], uint8_t sel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sel > 1 || dual_same(&ref[0], &ref[1]) || !dual_ok(sm, &ref[0]) || !dual_ok(sm, &ref[1])) {
		return;
	}
	bool running = dual_running(sm);
	if (scan_on(sc) && !running) {
		scan_stop(sm, false, now_ms);
	}
#if CONFIG_DUAL_WATCH
	for (uint8_t i = 0; i < 2; i++) {
		if (!dual_same(&sm->dual[i], &ref[i])) {
			sm->dual_keep[i].valid = false;	// another channel: its changes for now went with the old one
			dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << i)));
		}
	}
#endif
	sm->dual[0] = ref[0];
	sm->dual[1] = ref[1];
	sm->dual_sel = sel;
	sm->view.dual_sel = sel;
	bool here = false;
	for (uint8_t i = 0; i < 2; i++) {
		if (dual_here(sm, i)) {
			sm->dual_at = i;
			here = true;
		}
	}
	if (running && here) {
#if CONFIG_DUAL_WATCH
		if (board.rx2 != NULL) {
			rx2_engage(sm, now_ms);	// the selection may have changed: the receivers swap channels
			scan_publish(sm);
		}
#endif
		return;
	}
	if (!running) {
		scan_cfg_t cfg;
		scan_config(sm, SCAN_DUAL, &cfg);
		scan_begin(sc, SCAN_DUAL, &cfg, now_ms);
		activity(sm, now_ms);
	}
#if CONFIG_DUAL_WATCH
	if (board.rx2 != NULL && rx2_engage(sm, now_ms)) {
		scan_publish(sm);
		return;
	}
#endif
	if (!here) {
		if (!dual_tune(sm, sel, now_ms)) {
			scan_end(sc);
			scan_publish(sm);
			return;
		}
		scan_tuned(sc, now_ms);
	}
	scan_publish(sm);
}

#if CONFIG_DUAL_WATCH
// PTT in dual watch: on the selected channel (tuned first if the receiver is on the other), and the watch goes on
// after the transmission (scan_talked every poll while it lasts)
static void dual_talk(radio_sm_t *sm, uint32_t now_ms)
{
	sm->dual_peek = false;
	if (!dual_here(sm, sm->dual_sel) && !dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_stop(sm, false, now_ms);
		return;
	}
	scan_talked(&sm->scan, now_ms);
	scan_publish(sm);
}

// A request for the selected channel (the monitor, a change for now, a talk target) while the receiver is on the
// other: it tunes the selected one first, as PTT does, and the watch goes on from there. Monitor held pauses it there
// (scan_service)
static void dual_to_selected(radio_sm_t *sm, uint32_t now_ms)
{
	sm->dual_peek = false;
	if (dual_here(sm, sm->dual_sel)) {
		return;
	}
	if (!dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_stop(sm, false, now_ms);
		return;
	}
	scan_tuned(&sm->scan, now_ms);
	scan_publish(sm);
}

// Requests that act on the channel the receiver is on: in dual watch that must be the selected one
static bool for_selected(uint8_t type)
{
	switch (type) {
	case RREQ_POWER: case RREQ_SQUELCH: case RREQ_CLEAR_TEMP: case RREQ_DMR_TARGET: case RREQ_DMR_CONTACT:
	case RREQ_DMR_CALL_BACK: case RREQ_DMR_TARGET_CLEAR: case RREQ_DMR_HEAR_ALL:
		return true;
	default:
		return false;
	}
}

static radio_dual_ref_t dual_ref(uint32_t a)
{
	radio_dual_ref_t r = { false, 0, 0 };
	if ((a & RADIO_DUAL_VFO) != 0) {
		r.vfo = true;
		r.pos = (uint16_t)(a & 1u);
	} else {
		r.zone = (uint16_t)((a >> 16) & 0x3FFFu);
		r.pos = (uint16_t)a;
	}
	return r;
}

static void dual_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	uint32_t a = (uint32_t)req->arg, b = req->arg2;
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	if (((a | b) & RADIO_DUAL_SEL) == 0) {
		if (dual_running(sm)) {
			scan_stop(sm, true, now_ms);	// on the selected channel
		}
		sm->dual_keep[0].valid = sm->dual_keep[1].valid = false;	// the user stopped it: the other's go
		return;
	}
	radio_dual_ref_t ref[2] = { dual_ref(a), dual_ref(b) };
	dual_start(sm, ref, (b & RADIO_DUAL_SEL) != 0 ? 1 : 0, now_ms);
}
#endif

#if CONFIG_DUAL_WATCH
// Dual watch on one receiver, in the wait after a signal (SCAN_PH_RESUME): the receiver stays for the reply, but after
// each dwell there it looks at the other channel for one. A signal there is shown (view.dual_busy), not played or
// held: the receiver goes back and the wait goes on, and the watch goes there when the wait ends. A hold (a signal,
// the DMR reply window) has no looks. True while a look runs: this poll's readings are the other channel's
static bool dual_wait_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	uint32_t dwell = sc->cfg.dwell_ms;
	sc->cfg.delay_ms = sm->set.dual_delay_ms;	// a new setting applies at once
	if (sm->dual_peek) {
		uint32_t since = now_ms - sc->tuned_ms;
		if (since >= SCAN_SETTLE_MS && carrier_heard(sm, st->noise)) {
			sm->dual_peek_heard = true;
		}
		if (since < dwell) {
			return true;
		}
		uint8_t bit = (uint8_t)(1u << sm->dual_at);
		dual_busy_set(sm, sm->dual_peek_heard ? (uint8_t)(sm->view.dual_busy | bit) : (uint8_t)(sm->view.dual_busy & ~bit));
		if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
			scan_stop(sm, false, now_ms);
			return true;
		}
		scan_tuned(sc, now_ms);
		sc->phase = SCAN_PH_RESUME;	// the wait goes on where it was
		sc->phase_ms = sm->dual_wait_ms;
		return true;
	}
	if (sc->phase != SCAN_PH_RESUME || sm->view.monitor || now_ms - sc->tuned_ms < dwell ||
	    now_ms - sc->phase_ms < dwell) {
		return false;
	}
	if (now_ms - sc->phase_ms + 2u * dwell > sc->cfg.delay_ms) {
		return false;	// no time for a look and a dwell back: the wait ends here
	}
	sm->dual_wait_ms = sc->phase_ms;
	uint8_t busy = sm->view.dual_busy;
	if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
		scan_stop(sm, false, now_ms);
		return true;
	}
	scan_tuned(sc, now_ms);
	sc->phase = SCAN_PH_RESUME;
	sc->phase_ms = sm->dual_wait_ms;
	sm->dual_peek = true;
	sm->dual_peek_heard = false;
	sm->view.dual_busy = busy;	// the mark stays until this look says
	return true;
}
#endif

#if CONFIG_DMR || CONFIG_AIR
// A DMR, P25 or NXDN carrier by its signal strength at the squelch level (the default level when the squelch is open):
// the board's digital mode, whose noise reading may not follow the signal (gotchas 17.3)
static bool dmr_strength_heard(const radio_sm_t *sm, int16_t rssi_dbm)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	if (sq > RADIO_SQUELCH_MAX) {
		sq = RADIO_SQUELCH_MAX;
	}
	int16_t above = (int16_t)((sq - 1) * RADIO_SQL_DB_PER_STEP), floor_dbm;
	if (scan_floor(&sm->scan, &floor_dbm) && rssi_dbm <= floor_dbm + RADIO_DMR_SQL_L1_FLOOR_DB + above) {
		return false;
	}
	return rssi_dbm > RADIO_DMR_SQL_L1_DBM + above;
}
#endif

// Every poll while scanning: pause on the monitor, what the receiver hears, move on
static void scan_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
#if CONFIG_DUAL_WATCH
	if (sc->kind == SCAN_DUAL && board.rx2 != NULL && rx2_service(sm, now_ms)) {
		scan_publish(sm);
		return;
	}
	if (sc->kind == SCAN_DUAL && dual_wait_service(sm, st, now_ms)) {
		scan_publish(sm);
		return;
	}
#endif
	scan_pause(sc, sm->view.monitor, now_ms);
	bool dmr = sm->view.ch.mode == RADIO_MODE_DMR;
	bool air = air_mode(sm->view.ch.mode);
	bool carrier = carrier_heard(sm, st->noise);
	bool active, sync = false, digital = false;	// digital: the stop's own digital mode heard (scan_mixed)
#if CONFIG_DMR
	if (dmr) {
		active = sm->view.dmr.state == RDMR_RX;
		// Some receivers' noise reading stays put on a DMR signal while its strength rises: either one is a
		// carrier, the strength once the settle has passed (a reading just after the retune is the last stop's)
		// The strength scale also sits above the floor read on the stops before: some receivers' strength
		// reading isn't calibrated, and reads far up the scale on an empty channel (gotchas 17.4)
		scan_level(sc, now_ms, st->rssi_dbm);
		carrier = carrier || (now_ms - sc->tuned_ms >= SCAN_SETTLE_MS && dmr_strength_heard(sm, st->rssi_dbm));
		// Sync: bursts with the channel's color code since this stop's settle, a call or a late entry still
		// waiting for its LC. A call known not to be for us (another group, a private call) holds nothing
		uint32_t heard_ms;
		sync = dmr_call_last_activity(&heard_ms) && (int32_t)(heard_ms - sc->tuned_ms) >= SCAN_SETTLE_MS &&
		       now_ms - heard_ms <= RADIO_SCAN_DMR_SYNC_AGE_MS;
		digital = sync || active || sm->view.dmr.state == RDMR_RX_OTHER;
		if (sm->view.dmr.state == RDMR_RX_OTHER) {
			carrier = sync = false;
		}
#if CONFIG_DUAL_WATCH
		// Dual watch: the reply window holds the selected channel (PTT talks there)
		active = active || (dual_running(sm) && sm->dual_at == sm->dual_sel && sm->view.dmr.reply_active);
#endif
	} else
#endif
#if CONFIG_AIR
	if (air) {
		// P25, NXDN: a carrier by either reading, as on DMR. Sync: a call of the channel's NAC or RAN whose
		// squelch isn't decided yet (a late entry before its talk group or group is read). Active: a call that
		// plays. A call decided against (another talk group, encrypted) holds nothing
		uint8_t h = air_heard(sm);
		scan_level(sc, now_ms, st->rssi_dbm);
		carrier = carrier || (now_ms - sc->tuned_ms >= SCAN_SETTLE_MS && dmr_strength_heard(sm, st->rssi_dbm));
		active = h == AIR_HEARD_PLAYED;
		sync = h == AIR_HEARD_PENDING;
		digital = h != AIR_HEARD_NONE;
		if (h == AIR_HEARD_OTHER) {
			carrier = false;
		}
	} else
#endif
	{
		active = sm->view.state == RS_RX && (sm->view.squelch != 0 || carrier);
	}
	sc->cfg.probe_ms = dmr ? RADIO_SCAN_DMR_PROBE_MS : air ? RADIO_SCAN_AIR_PROBE_MS :
			   tone_needed(sm) ? RADIO_SCAN_FM_PROBE_MS : 0;
	sc->cfg.sync_ms = dmr ? RADIO_SCAN_DMR_SYNC_MS : air ? RADIO_SCAN_AIR_SYNC_MS : 0;
	sc->cfg.carrier_hold_ms = dmr || air ? RADIO_SCAN_DMR_CARRIER_MS : 0;
	uint8_t was = sc->phase;
	scan_act_t act = scan_poll(sc, now_ms, active, carrier, sync);
	if (scan_mixed(sm, was, act, digital, now_ms)) {
		scan_publish(sm);
		return;
	}
	switch (act) {
	case SCAN_ACT_NEXT:
		scan_step(sm, now_ms);
		return;
	case SCAN_ACT_STOP:
		scan_stop(sm, false, now_ms);
		break;
	default:
		break;
	}
	if (sc->phase == SCAN_PH_HOLD && was != SCAN_PH_HOLD && was != SCAN_PH_RESUME && sm->set.scan_beep) {
		beep(sm, BEEP_SCAN_HOLD);
	}
	scan_publish(sm);
}

// A scan stop asked for (the debug link): dual watch stops on the selected channel, as the UI's stop does, and the
// view counts it so the UI knows it was asked for and doesn't start the watch again
static void scan_stop_asked(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	if (dual_running(sm)) {
		cancel = true;
		sm->view.dual_stops++;
#if CONFIG_DUAL_WATCH
		// The user's stop, as dual_request's: the UI asks for nothing after it, so the other's go here
		sm->dual_keep[0].valid = sm->dual_keep[1].valid = false;
#endif
	}
	scan_stop(sm, cancel, now_ms);
}

static void scan_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	switch (req->arg) {
	case RSCAN_STOP:
		scan_stop_asked(sm, false, now_ms);
		break;
	case RSCAN_CANCEL:
		scan_stop_asked(sm, true, now_ms);
		break;
	case RSCAN_ZONE:
		scan_start(sm, SCAN_ZONE, 0, 0, now_ms);
		break;
	case RSCAN_VFO:
		scan_start(sm, SCAN_VFO, 0, 0, now_ms);
		break;
	case RSCAN_DUAL: {
		// The current channel (selected) and position arg2 of the current zone
		radio_dual_ref_t ref[2] = {
			{ sm->view.vfo, sm->view.vfo ? 0 : sm->view.zone_index, sm->view.vfo ? 0 : sm->view.zone_pos },
			{ false, sm->view.zone_index, (uint16_t)req->arg2 },
		};
		dual_start(sm, ref, 0, now_ms);
		break;
	}
	case RSCAN_TOGGLE:
		if (scan_on(&sm->scan)) {
			scan_stop_asked(sm, false, now_ms);
		} else {
			scan_start(sm, sm->view.vfo ? SCAN_VFO : SCAN_ZONE, 0, 0, now_ms);
		}
		break;
	case RSCAN_NUISANCE:
		if (scan_on(&sm->scan) && !dual_running(sm)) {	// dual watch: both channels were picked
			bool ok = sm->scan.kind == SCAN_VFO ? scan_nuisance_hz(&sm->scan, sm->view.ch.rx_hz)
				: scan_nuisance_pos(&sm->scan, sm->view.zone_pos);
			if (ok) {
				scan_step(sm, now_ms);
			}
		}
		break;
	default:
		break;
	}
}
#else
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms) { (void)sm; (void)cancel; (void)now_ms; }
static bool dual_running(const radio_sm_t *sm) { (void)sm; return false; }
static bool dual_peeking(const radio_sm_t *sm) { (void)sm; return false; }
#endif

static void persist(radio_sm_t *sm, uint32_t now_ms);

// --- DMR repeater roaming (radio/roam.c decides; this builds the site list, measures, tunes and keeps the block
// list; docs/spec/dmr-roaming.md) ---
#if CONFIG_DMR_ROAMING
static bool roam_zone_flag(const radio_sm_t *sm, uint16_t zi)
{
	return zi < SETTINGS_ROAM_ZONE_BYTES * 8u && (sm->roam_zones[zi / 8u] & (1u << (zi % 8u))) != 0;
}

static void roam_cfg_of(const radio_settings_t *s, roam_cfg_t *c)
{
	c->rssi_dbm = s->roam_rssi_dbm;
	c->hyst_db = s->roam_hyst_db;
	c->probe = s->roam_probe;
	c->check_s = s->roam_check_s;
	c->beacon_s = s->roam_beacon_s;
	c->beacon_len_s = s->roam_beacon_len_s;
	c->survey = s->roam_survey;
}

// Where a channel talks: its default contact's ID and call type, else talkgroup 9 (as dmr_load_channel)
static void roam_talk_of(const channel_t *ch, uint32_t *id, bool *priv)
{
	contact_t c;
	uint16_t talk = codeplug_channel_talk_contact(ch);
	*id = 9;
	*priv = false;
	if (talk != 0 && codeplug_contact(talk, &c)) {
		*id = c.id;
		*priv = c.type == CONTACT_PRIVATE;
	}
}

// A DMR repeater channel the radio could roam to (docs/spec/dmr-roaming.md 2)
static bool roam_repeater(const channel_t *ch)
{
	return ch->mode == RADIO_MODE_DMR && !parked(ch) && !ch->rx_only && ch->tx_hz != 0 && ch->tx_hz != ch->rx_hz && !ch->dmo &&
		board_in_band(ch->rx_hz, false);
}

static void roam_publish(radio_sm_t *sm, uint32_t now_ms)
{
	const roam_t *r = &sm->roam;
	radio_roam_view_t *v = &sm->view.roam;
	bool on = r->state != ROAM_OFF;
	int16_t avg = on ? roam_signal(r, now_ms) : 0;
	bool zone_on = !sm->view.vfo && sm->have_channels && roam_zone_flag(sm, sm->view.zone_index);
	bool weak = avg != 0 && avg < sm->set.roam_rssi_dbm;
	uint8_t sites = on ? r->count : 0;
	uint8_t visit = roam_searching(r) ? r->visit : 0xFF;
	if (v->zone_on != zone_on || v->state != r->state || v->why != r->why || v->pending != r->pending ||
	    v->sites != sites || v->site != r->cur || v->home != r->home || v->weak != weak ||
	    v->searches != r->searches || v->roams != r->roams || v->aborts != r->aborts || v->visit != visit ||
	    v->surveys != r->surveys || v->beacons != r->beacons || v->win_conf != r->win_conf) {
		changed(sm);
	}
	uint32_t next = 0;
	v->visit = visit;
	v->win_conf = r->win_conf;
	v->win_next_ms = on && roam_window(r, now_ms, &next) ? next - now_ms : 0;
	v->confirm = r->confirm;
	v->surveys = r->surveys;
	v->visits = r->visits;
	v->cuts = r->cuts;
	v->beacons = r->beacons;
	v->confirms = r->confirms;
	v->site_fresh16 = 0;
	uint32_t period = (uint32_t)r->cfg.beacon_s * 1000u;
	for (uint8_t i = 0; i < RADIO_ROAM_SITES; i++) {
		const roam_site_t *s = &r->site[i];
		bool here = i < sites;
		uint32_t age = here && s->surv_ms != 0 ? (now_ms - s->surv_ms) / 1000u : 0xFFFFu;
		v->site_surv[i] = here ? s->surv_dbm : 0;
		v->site_age_s[i] = (uint16_t)(age > 0xFFFEu && age != 0xFFFFu ? 0xFFFEu : age);
		v->site_phase_ms[i] = here && s->phase_ms != 0 && period != 0 ? s->phase_ms % period : UINT32_MAX;
		if (here && roam_site_fresh(r, i, now_ms)) {
			v->site_fresh16 |= (uint16_t)(1u << i);
		}
	}
	v->zone_on = zone_on;
	v->state = r->state;
	v->why = r->why;
	v->pending = r->pending;
	v->sites = sites;
	v->site = r->cur;
	v->home = r->home;
	v->rssi_dbm = avg;
	v->weak = weak;
	v->searches = r->searches;
	v->roams = r->roams;
	v->stays = r->stays;
	v->aborts = r->aborts;
	v->probes = r->probes;
	v->triggers = r->triggers;
	for (uint8_t i = 0; i < RADIO_ROAM_SITES; i++) {
		v->site_number[i] = i < sites ? r->site[i].number : 0;
		v->site_rssi[i] = i < sites ? r->site[i].rssi_dbm : 0;
	}
}

// The site list from the current zone (docs/spec/dmr-roaming.md 2). user: the user picked this channel (it is home)
static void roam_rebuild(radio_sm_t *sm, uint32_t now_ms, bool user)
{
	roam_site_t sites[ROAM_SITES_MAX];
	bool flagged[ROAM_SITES_MAX];
	uint8_t n = 0;
	int cur = -1;
	channel_t here, ch;
	sm->roam_dirty = false;
	sm->roam_zone = sm->view.zone_index;
	sm->roam_channel = sm->view.vfo ? 0 : sm->view.channel_number;
	if (sm->view.vfo || !sm->have_channels || !roam_zone_flag(sm, sm->view.zone_index) ||
	    !channel_read(sm->view.channel_number, &here) || !roam_repeater(&here)) {
		roam_set_sites(&sm->roam, sites, 0, -1, -1, now_ms);
		roam_publish(sm, now_ms);
		return;
	}
	uint32_t talk_id;
	bool talk_priv, any_flag = false;
	roam_talk_of(&here, &talk_id, &talk_priv);
	for (uint16_t p = 0; p < sm->zone.count; p++) {
		uint32_t id;
		bool priv;
		if (!channel_read(sm->zone.channels[p], &ch)) {
			continue;
		}
		any_flag |= ch.roam;
		if (n >= ROAM_SITES_MAX || !roam_repeater(&ch)) {
			continue;
		}
		roam_talk_of(&ch, &id, &priv);
		if (id != talk_id || priv != talk_priv) {
			continue;
		}
		memset(&sites[n], 0, sizeof(sites[n]));
		sites[n].pos = p;
		sites[n].number = sm->zone.channels[p];
		flagged[n] = ch.roam;
		n++;
	}
	if (any_flag) {
		// The open-firmware Roaming flag narrows the zone to the flagged channels
		uint8_t k = 0;
		for (uint8_t i = 0; i < n; i++) {
			if (flagged[i]) {
				sites[k++] = sites[i];
			}
		}
		n = k;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (sites[i].pos == sm->view.zone_pos) {
			cur = i;
		}
	}
	roam_set_sites(&sm->roam, sites, n, cur, user ? cur : -1, now_ms);
	roam_publish(sm, now_ms);
}

// Why a search can't start now (docs/spec/dmr-roaming.md 4.3)
static uint8_t roam_blocked(const radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot) {
		return RROAM_BLOCK_HOTSPOT;
	}
	if (sm->view.vfo || sm->view.ch.mode != RADIO_MODE_DMR || !sm->dmr_running) {
		return RROAM_BLOCK_MODE;
	}
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return RROAM_BLOCK_SCAN;
	}
#endif
	if (sm->ptt || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || sm->dmr_keyed || dmr_call_data_busy()) {
		return RROAM_BLOCK_TX;
	}
	if (sm->view.state == RS_RX || sm->view.dmr.state != RDMR_IDLE) {
		return RROAM_BLOCK_CALL;
	}
	if (sm->view.dmr.reply_active || sm->one_shot) {
		return RROAM_BLOCK_REPLY;
	}
	if (now_ms - sm->roam_quiet_ms < ROAM_QUIET_MS) {
		return RROAM_BLOCK_QUIET;
	}
	if (sm->view.temporary || sm->manual || sm->hear_all_latch || sm->view.monitor) {
		return RROAM_BLOCK_OVERRIDE;
	}
#if CONFIG_DMR_EMERGENCY
	if (dmr_emergency_in_progress()) {
		return RROAM_BLOCK_EMERGENCY;
	}
#endif
	return RROAM_BLOCK_NONE;
}

// Tune site idx (a roaming move, not the user's: the site list stays). force: retune even when already there
static bool roam_tune(radio_sm_t *sm, uint8_t idx, bool force, uint32_t now_ms)
{
	const roam_site_t *s = &sm->roam.site[idx];
	channel_t ch;
	if (idx >= sm->roam.count || s->pos >= sm->zone.count || sm->zone.channels[s->pos] != s->number ||
	    !channel_read(s->number, &ch)) {
		return false;
	}
	sm->roam_channel = s->number;
	if (!force && !sm->view.vfo && sm->view.zone_pos == s->pos) {
		return true;
	}
	set_channel(sm, s->pos, &ch);
	tune(sm, now_ms);
	return true;
}

void radio_sm_shown_view(const radio_sm_t *sm, radio_view_t *v)
{
	if (!sm->roam_shown || sm->roam.state != ROAM_SURVEY) {
		return;
	}
	v->ch = sm->roam_shown_ch;
	v->zone_pos = sm->roam_shown_pos;
	v->channel_number = sm->roam_shown_number;
}

// The wake probe: one BS_Dwn_Act CSBK through the stack's data path, which wakes the repeater first (its own
// activation bursts and retries) when it isn't up (docs/spec/dmr-roaming.md 4.5)
static uint8_t roam_probe_left;

static bool roam_probe_next(dmr_tx_burst_t *out, void *ctx)
{
	const radio_sm_t *sm = ctx;
	if (roam_probe_left == 0) {
		return false;
	}
	roam_probe_left--;
	memset(out, 0, sizeof(*out));
	out->kind = DMR_BURST_CSBK;
	out->host_crc = true;
	out->sync_override = DMR_SYNC_NONE;
	out->len = 12;
	dmr_csbk_bs_activation(sm->view.dmr.own_id, out->payload);
	return true;
}

static void roam_probe_done(uint8_t err, void *ctx)
{
	radio_sm_t *sm = ctx;
	sm->roam_probing = false;
	sm->roam_probe_err = err;
}

static bool roam_probe(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	if (!sm->dmr_running || sm->ptt || sm->view.dmr.own_id == 0 || tx_check(sm) != TXDENY_NONE ||
	    dmr_call_data_busy()) {
		return false;
	}
	if (!sm->dmr_keyed) {
		static const subtone_t none = { TONE_NONE, 0 };
		if (!key_tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &none)) {
			return false;
		}
		sm->dmr_keyed = true;
	}
	roam_probe_left = 1;
	dmr_data_source_t src = { .next = roam_probe_next, .done = roam_probe_done, .ctx = sm, .count = 1,
				  .tag = "roam" };
	if (!dmr_call_data_tx(&src)) {
		return false;	// dmr_follow releases the keying
	}
	sm->roam_probing = true;
	sm->roam_probe_err = 0xFF;
	return true;
}

// End a search now and go back to where it started (the user did something)
static void roam_cancel(radio_sm_t *sm, uint32_t now_ms)
{
	if (!roam_searching(&sm->roam)) {
		return;
	}
	roam_abort(&sm->roam, now_ms);
	bool restart = sm->roam_probing || dmr_call_data_busy() || sm->dmr_keyed;
	if (restart) {
		dmr_end(sm);		// our probe is on the air: stop it with the stack (tune() starts it again)
		sm->roam_probing = false;
	}
	if (!roam_tune(sm, sm->roam.cur, restart, now_ms)) {
		sm->roam_dirty = true;
	}
	roam_publish(sm, now_ms);
}

// Every poll (DMR builds, not in hotspot mode): the site list, the measurement, the search
static void roam_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	roam_t *r = &sm->roam;
	if (sm->view.state != RS_IDLE || sm->view.dmr.state != RDMR_IDLE) {
		sm->roam_quiet_ms = now_ms;
	}
	if (!roam_searching(r)) {
		bool moved = sm->view.zone_index != sm->roam_zone ||
			     sm->view.channel_number != sm->roam_channel || (sm->view.vfo && sm->roam_channel != 0);
		bool scanning = false;
#if CONFIG_SCAN
		scanning = scan_on(&sm->scan);
#endif
		if ((moved || sm->roam_dirty) && !scanning) {
			if (moved) {
				roam_lock(r, false, now_ms);	// a lock ends with the user's channel change
			}
			roam_rebuild(sm, now_ms, moved);
		}
	}
	if (r->state == ROAM_OFF) {
		roam_publish(sm, now_ms);
		return;
	}
	bool heard = sm->dmr_running && dmr_call_view()->repeater_up;
	roam_sample(r, now_ms, heard, sm->view.rssi_dbm, sm->view.state == RS_RX || sm->view.dmr.state != RDMR_IDLE);
	uint8_t blocked = roam_blocked(sm, now_ms);
	sm->view.roam.blocked = blocked;
	if (roam_searching(r) && (sm->roam_probing || sm->dmr_keyed)) {
		roam_publish(sm, now_ms);	// the probe is on the air: no retune until it is done
		return;
	}
	uint32_t roams = r->roams;
	bool was_away = roam_searching(r);
	switch (roam_poll(r, now_ms, blocked, sm->view.dmr.state == RDMR_RX)) {
	case ROAM_ACT_TUNE:
		if (roam_surveying(r) && !was_away) {
			// A survey round leaves: the screens keep the site it returns to (radio_sm_shown_view)
			sm->roam_shown = true;
			sm->roam_shown_ch = sm->view.ch;
			sm->roam_shown_pos = sm->view.zone_pos;
			sm->roam_shown_number = sm->view.channel_number;
		}
		if (roam_tune(sm, r->visit, false, now_ms)) {
			roam_tuned(r, now_ms);
		} else {
			roam_cancel(sm, now_ms);
			sm->roam_dirty = true;
		}
		break;
	case ROAM_ACT_PROBE:
		roam_probe_sent(r, roam_probe(sm));
		break;
	case ROAM_ACT_DONE:
		if (!roam_tune(sm, r->cur, false, now_ms)) {
			sm->roam_dirty = true;
		}
		if (r->roams != roams) {
			persist(sm, now_ms);	// powers up on this site
			if (sm->set.key_beep && !sm->view.ch.no_beep) {
				beep(sm, BEEP_SCAN_HOLD);
			}
		}
		break;
	default:
		break;
	}
	roam_publish(sm, now_ms);
}

static void roam_request(radio_sm_t *sm, uint8_t cmd, uint32_t now_ms)
{
	roam_t *r = &sm->roam;
	switch (cmd) {
	case RROAM_ZONE_OFF:
	case RROAM_ZONE_ON:
	case RROAM_ZONE_TOGGLE: {
		uint16_t zi = sm->view.zone_index;
		if (sm->view.vfo || !sm->have_channels || zi >= SETTINGS_ROAM_ZONE_BYTES * 8u) {
			break;
		}
		bool on = cmd == RROAM_ZONE_ON || (cmd == RROAM_ZONE_TOGGLE && !roam_zone_flag(sm, zi));
		if (!on) {
			roam_cancel(sm, now_ms);
		}
		uint8_t bit = (uint8_t)(1u << (zi % 8u));
		sm->roam_zones[zi / 8u] = (uint8_t)(on ? sm->roam_zones[zi / 8u] | bit : sm->roam_zones[zi / 8u] & ~bit);
		settings_t *s = settings_begin();
		// Only this zone's flag: the others are the zone writes' to move (zone_settle.c), and a reload not done yet
		// leaves this zone's index the old one
		uint16_t saved = zi;
#if CONFIG_ZONE_WRITE
		if (zone_edit_map(sm->zone_shifts, zone_edit_shifts(), zi, &saved) != ZONE_MAP_KEPT) {
			saved = SETTINGS_ROAM_ZONE_BYTES * 8u;	// gone or lost: the reload reads the saved flags again
		}
#endif
		if (saved < SETTINGS_ROAM_ZONE_BYTES * 8u) {
			uint8_t sb = (uint8_t)(1u << (saved % 8u));
			s->roam_zones[saved / 8u] = (uint8_t)(on ? s->roam_zones[saved / 8u] | sb : s->roam_zones[saved / 8u] & ~sb);
		}
		settings_end(now_ms);
		roam_rebuild(sm, now_ms, true);
		break;
	}
	case RROAM_NOW:
		if (sm->roam_dirty) {
			roam_rebuild(sm, now_ms, false);
		}
		roam_trigger(r, ROAM_WHY_MANUAL);
		break;
	case RROAM_LOCK:
	case RROAM_UNLOCK:
	case RROAM_LOCK_TOGGLE:
		roam_cancel(sm, now_ms);
		roam_lock(r, cmd == RROAM_LOCK || (cmd == RROAM_LOCK_TOGGLE && r->state != ROAM_LOCKED), now_ms);
		break;
	default:
		break;
	}
	roam_publish(sm, now_ms);
}
#else
static inline void roam_cancel(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
void radio_sm_shown_view(const radio_sm_t *sm, radio_view_t *v) { (void)sm; (void)v; }
#endif

#if CONFIG_APRS_AFSK
// --- APRS transmissions (core/aprs, docs/spec/aprs.md 6) ---
// The engine (aprs_tick) decides what to send; here the radio leaves the user's channel for the APRS frequency (FM,
// no subtone), waits for a clear channel (p-persistent CSMA on the noise reading), keys up, lets board.afsk send the
// tones, and tunes back. Only from idle: no PTT, no reception, no scan, no hotspot, no DMR call or data, TX allowed.
// Any user action (PTT, a request) cuts it short and the engine tries again later.

#define APRS_SETTLE_MS		50	// the receiver settles on the APRS frequency before the first carrier check
#define APRS_SLOT_MS		100	// CSMA slot time
#define APRS_PERSIST		63	// p = (63 + 1) / 256 per clear slot
#define APRS_KEYUP_MS		25	// carrier up before the tones (the preamble flags follow)
#define APRS_SEND_MAX_MS	6000u	// longer than any tone buffer (4096 symbols = 3.4 s)

static bool aprs_can_start(const radio_sm_t *sm)
{
	if (sm->aprs_phase != APRS_RF_OFF || sm->ptt || sm->hotspot || (sm->view.state != RS_IDLE && !(sm->view.state == RS_RX && bcast_listening(sm))) ||
	    sm->view.monitor || sm->power_off_req || sm->set.tx_inhibit || sm->batt.level == BATT_CRITICAL || board.afsk == NULL ||
	    board.afsk->tx_start == NULL) {
		return false;
	}
#if CONFIG_SCAN
	if (scan_on(&sm->scan) && !dual_running(sm)) {
		return false;
	}
#if CONFIG_DUAL_WATCH
	if (dual_peeking(sm)) {
		return false;
	}
	if (sm->rx2_on) {
		rx2_status_t st;
		board.rx2->status(&st);
		if (st.active) {
			return false;
		}
	}
#endif
#endif
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 || sm->burst_keyed) {
		return false;
	}
#endif
#if CONFIG_VOX
	if (sm->vox_keyed) {
		return false;
	}
#endif
#if CONFIG_DMR
	if ((sm->dmr_running && sm->view.dmr.state != RDMR_IDLE) || sm->dmr_keyed) {
		return false;
	}
#if RADIO_SM_CW
	if (sm->cw_phase != 0) {
		return false;
	}
#endif
#endif
#if CONFIG_DMR_DATA_CORE
	if (dmrDataServiceIsBusy()) {
		return false;
	}
#endif
#if CONFIG_DMR_ROAMING
	if (sm->roam_probing) {
		return false;
	}
#endif
#if CONFIG_DMR_EMERGENCY
	if (sm->em_keyed) {
		return false;
	}
#endif
	return true;
}

// Back to the user's channel; how it ended for the engine
static void aprs_rf_end(radio_sm_t *sm, aprs_rf_result_t r, uint32_t now_ms)
{
	uint8_t phase = sm->aprs_phase;
	if (phase == APRS_RF_OFF) {
		return;
	}
	if (phase == APRS_RF_SEND) {
		board.afsk->tx_stop();
	}
	if (phase >= APRS_RF_KEYUP) {
		board.radio->tx_off();
		sm->tx_end_ms = now_ms;
	}
	sm->aprs_phase = APRS_RF_OFF;
	sm->view.state = RS_IDLE;
	sm->view.aprs_tx = false;
	aprs_rf_done(r, now_ms);
	tune(sm, now_ms);	// the channel again (DMR set up again if it was DMR)
#if CONFIG_SCAN
	if (dual_running(sm)) {
		scan_tuned(&sm->scan, now_ms);
		scan_publish(sm);
#if CONFIG_DUAL_WATCH
		if (board.rx2 != NULL) {
			rx2_engage(sm, now_ms);
		}
#endif
	}
#endif
}

static void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms)
{
	aprs_rf_end(sm, APRS_RF_ABORTED, now_ms);
}

static void aprs_rf_start(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!aprs_rf_take(&sm->aprs_job, now_ms)) {
		return;
	}
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	if (dual_running(sm)) {
		rx2_release(sm);	// the other receiver must be quiet while APRS has the transmitter
	}
#endif
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack lets go of the modem before the chip leaves DMR
	}
	if (sm->board_mode != RADIO_MODE_FM) {
		sm->board_mode = RADIO_MODE_FM;
		board.radio->set_mode(RADIO_MODE_FM);
	}
	sm->aprs_phase = APRS_RF_LISTEN;
	sm->aprs_ms = now_ms;
	sm->aprs_listen_ms = now_ms;
	sm->aprs_rand ^= now_ms;
	board.radio->rx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K, &none);
	outputs(sm);
	changed(sm);
}

// Every poll while on the APRS frequency
static void aprs_rf_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	switch (sm->aprs_phase) {
	case APRS_RF_LISTEN:
		if (now_ms - sm->aprs_listen_ms >= APRS_CSMA_GIVE_UP_MS) {
			aprs_rf_end(sm, APRS_RF_BUSY, now_ms);
			return;
		}
		if (now_ms - sm->aprs_ms < APRS_SETTLE_MS + APRS_SLOT_MS) {
			return;
		}
		sm->aprs_ms = now_ms - APRS_SETTLE_MS;	// the next check a slot later
		if (st->noise < radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT)) {
			return;		// a carrier: wait
		}
		sm->aprs_rand = sm->aprs_rand * 1103515245u + 12345u;
		if ((sm->aprs_rand >> 24) > APRS_PERSIST) {
			return;		// clear, but not our turn this slot
		}
		sm->aprs_phase = APRS_RF_KEYUP;		// before tx(): aprs_rf_end unkeys from here on
		sm->aprs_ms = now_ms;
		sm->view.state = RS_TX;			// speaker off, red LED on, before keying
		sm->view.tx_auto = true;
		sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.aprs_tx = true;
		outputs(sm);
		changed(sm);
		// APRS's own power level when it has one, else the radio's current one (a temporary change included), as for
		// PTT. The settings' default (the board's default power) ignored a lowered level
		if (!key_tx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K,
				     aprs_tx_power(sm->view.power), &none)) {
			aprs_rf_end(sm, APRS_RF_NO_KEY, now_ms);
		}
		break;
	case APRS_RF_KEYUP:
		if (now_ms - sm->aprs_ms < APRS_KEYUP_MS) {
			return;
		}
		if (!board.afsk->tx_start(sm->aprs_job.tones, sm->aprs_job.nbits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ,
					  sm->aprs_job.level)) {
			aprs_rf_end(sm, APRS_RF_NO_AFSK, now_ms);
			return;
		}
		sm->aprs_phase = APRS_RF_SEND;
		sm->aprs_ms = now_ms;
		break;
	case APRS_RF_SEND:
		if (!board.afsk->tx_busy()) {
			aprs_rf_end(sm, APRS_RF_SENT, now_ms);
		} else if (now_ms - sm->aprs_ms > APRS_SEND_MAX_MS) {
			aprs_rf_end(sm, APRS_RF_TIMEOUT, now_ms);
		}
		break;
	default:
		break;
	}
}

#else
static inline void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

#if CONFIG_APRS
// Every poll, before anything else: the engine, the "after PTT" trigger, a transmission to start
static void aprs_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx_now = sm->view.state == RS_TX && !sm->tx_no_ptt;
#if CONFIG_APRS_AFSK
	tx_now = tx_now && sm->aprs_phase == APRS_RF_OFF;
#endif
	if (sm->aprs_was_tx && !tx_now) {
		aprs_ptt_end(now_ms);
	}
	sm->aprs_was_tx = tx_now;
	aprs_radio_t r;
	memset(&r, 0, sizeof(r));
#if CONFIG_APRS_AFSK
	r.can_rf = aprs_can_start(sm);
	r.fm_rx = sm->aprs_phase == APRS_RF_OFF && sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot &&
		  sm->view.state != RS_TX;
	r.rx_hz = sm->view.ch.rx_hz;
#endif
#if CONFIG_DMR
	r.on_dmr = sm->dmr_running && !sm->hotspot;
	r.own_dmr_id = sm->own_id;
#endif
	aprs_tick(&r, now_ms);
#if CONFIG_APRS_AFSK
	if (r.can_rf && aprs_rf_pending()) {
		aprs_rf_start(sm, now_ms);
	}
#endif
}
#endif

// --- DTMF (CONFIG_DTMF; radio-features.md 3.7): a digit key held during an FM transmission sends its pair ---

#if CONFIG_DTMF
// A digit key would send now: FM TX that PTT holds, with nothing else on the air
static bool dtmf_allowed(const radio_sm_t *sm)
{
	if (board.radio->tx_dtmf == NULL || sm->view.state != RS_TX || sm->view.ch.mode != RADIO_MODE_FM || !sm->ptt ||
			sm->hotspot || sm->host_audio) {
		return false;
	}
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 || sm->view.tone_burst || sm->burst_keyed) {
		return false;
	}
#endif
	return true;
}

static void dtmf_start(radio_sm_t *sm, char digit, bool held, uint32_t now_ms)
{
	if (!board.radio->tx_dtmf(digit)) {
		return;
	}
	sm->view.dtmf = digit;
	sm->dtmf_held = held;
	sm->dtmf_on_ms = now_ms;
	sm->view.dtmf_sent++;
	size_t n = strlen(sm->view.dtmf_digits);
	if (n >= RADIO_DTMF_SHOWN) {
		memmove(sm->view.dtmf_digits, sm->view.dtmf_digits + 1, RADIO_DTMF_SHOWN - 1);	// keep the newest
		n = RADIO_DTMF_SHOWN - 1;
	}
	sm->view.dtmf_digits[n] = digit;
	sm->view.dtmf_digits[n + 1] = 0;
	changed(sm);
}

// The digit sounding off, the microphone back
static void dtmf_end(radio_sm_t *sm, uint32_t now_ms)
{
	board.radio->tx_dtmf(0);
	sm->view.dtmf = 0;
	sm->dtmf_held = false;
	sm->dtmf_off_ms = now_ms != 0 ? now_ms : 1;
	changed(sm);
}

// Everything off at once: the transmission ends, or something else takes the air
static void dtmf_cancel(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.dtmf != 0) {
		dtmf_end(sm, now_ms);
	}
	sm->dtmf_next = 0;
	if (sm->view.dtmf_ready) {
		sm->view.dtmf_ready = false;
		changed(sm);
	}
}

// Every poll while transmitting: the tone sounding ends once its key is up and it has had its shortest length; a
// waiting digit starts after the gap
static void dtmf_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!dtmf_allowed(sm)) {
		dtmf_cancel(sm, now_ms);
		return;
	}
	if (!sm->view.dtmf_ready) {
		sm->view.dtmf_ready = true;
		changed(sm);
	}
	if (sm->view.dtmf != 0 && !sm->dtmf_held && now_ms - sm->dtmf_on_ms >= RADIO_DTMF_MIN_MS) {
		dtmf_end(sm, now_ms);
	}
	if (sm->view.dtmf == 0 && sm->dtmf_next != 0 &&
			(sm->dtmf_off_ms == 0 || now_ms - sm->dtmf_off_ms >= RADIO_DTMF_GAP_MS)) {
		char d = sm->dtmf_next;
		sm->dtmf_next = 0;
		dtmf_start(sm, d, sm->dtmf_next_held, now_ms);
	}
}

// RREQ_DTMF: a key down or up. Up always counts (a key held from before still lets go); down only while allowed
static void dtmf_key(radio_sm_t *sm, char digit, bool down, uint32_t now_ms)
{
	if (!down) {
		if (sm->view.dtmf == digit) {
			sm->dtmf_held = false;
		}
		if (sm->dtmf_next == digit) {
			sm->dtmf_next_held = false;
		}
		if (sm->view.state == RS_TX) {
			dtmf_poll(sm, now_ms);
		}
		return;
	}
	if (!dtmf_valid(digit) || !dtmf_allowed(sm)) {
		return;
	}
	activity(sm, now_ms);
	if (sm->view.dtmf == digit && sm->dtmf_held) {
		return;		// the same key again (a repeated edge)
	}
	if (sm->dtmf_next != 0) {
		return;		// one digit waits already: a third key in the rollover is dropped
	}
	// Another key down (rollover) ends the digit sounding once it has had its shortest length; this one waits for
	// that and the gap
	sm->dtmf_held = false;
	sm->dtmf_next = digit;
	sm->dtmf_next_held = true;
	dtmf_poll(sm, now_ms);
}
#else
static inline void dtmf_cancel(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

// --- FM transmit tones and the end of an FM transmission ---

// Back to receive after an FM transmission (PTT released, roger beep done, burst released)
// keyed: DMR per-burst keying is still armed (dmr_follow releases it), so no tx_off here
static void fm_tx_end(radio_sm_t *sm, bool keyed, uint32_t now_ms)
{
#if CONFIG_FM_TX_TONES
	sm->burst_timed = sm->burst_held = sm->burst_key_owned = false;
	sm->burst_ms = sm->burst_end_ms = 0;
#endif
	dtmf_cancel(sm, now_ms);
#if CONFIG_DTMF
	sm->view.dtmf_digits[0] = 0;
#endif
	if (sm->view.state == RS_TX && !keyed) {
		board.radio->tx_off();	// ends a sub-audio tail too
	}
#if CONFIG_SUBTONE
	sm->tail_ms = 0;
#endif
	if (sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		bool sent = sm->view.state == RS_TX && sm->view.tx_deny == TXDENY_NONE;
		// A busy refusal's reception goes on as it was, its tone already heard; anything else starts from idle
		bool rx_on = sm->view.state == RS_TX_DENIED && sm->deny_rx;
		sm->deny_rx = false;
		sm->view.state = rx_on ? RS_RX : RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.tot_warning = false;
		sm->view.tone_burst = false;
		sm->view.roger = false;
		sm->close_count = 0;
		if (!rx_on) {
			reset_tone(sm);
		}
		outputs(sm);
		changed(sm);
		// The end of transmission tone on the speaker, once the transmitter is off (so a board that can't beep on
		// air plays it too). Not after a time-out or a denial: they have their own beeps
		if (sent && sm->view.ch.mode == RADIO_MODE_FM && sig_local_on(sm, SIG_EV_END)) {
			sig_local(sm, SIG_EV_END);
		}
	}
	sm->tx_end_ms = now_ms;
	if (sm->retune_pending) {
		sm->retune_pending = false;
		tune(sm, now_ms);	// a temporary bandwidth or RX tone was undone during TX
	}
}

#if CONFIG_SUBTONE
// The sub-audio tail at the end of an FM transmission (the DCS turn-off): the board starts it, the core holds the
// carrier RADIO_SUBTONE_TAIL_MS, then unkeys. True when it runs (the poll unkeys)
static bool tail_start(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state != RS_TX || sm->view.ch.mode != RADIO_MODE_FM || board.radio->tx_tail == NULL ||
			!board.radio->tx_tail(true)) {
		return false;
	}
	sm->tail_ms = now_ms != 0 ? now_ms : 1;
	return true;
}

static void tail_cancel(radio_sm_t *sm)
{
	if (sm->tail_ms != 0) {
		board.radio->tx_tail(false);
		sm->tail_ms = 0;
	}
}

static void tail_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (now_ms - sm->tail_ms >= RADIO_SUBTONE_TAIL_MS) {
		tail_cancel(sm);
		fm_tx_end(sm, false, now_ms);
	}
}
#endif

// The end of an FM transmission the user let go of: the sub-audio tail first, if the board sends one
static void fm_unkey(radio_sm_t *sm, bool keyed, uint32_t now_ms)
{
#if CONFIG_SUBTONE
	if (!keyed && tail_start(sm, now_ms)) {
		return;		// tail_poll unkeys
	}
#endif
	fm_tx_end(sm, keyed, now_ms);
}

#if CONFIG_FM_TX_TONES
#if CONFIG_SIG_TONES
// The start tone's sidetone stays on through the silent steps of the chain; it ends with the chain
static void tone_side_off(void)
{
	if (board.radio->tx_tone_side != NULL) {
		board.radio->tx_tone_side(false);
	}
}
#endif

static bool tx_tone(uint16_t hz)
{
	return board.radio->tx_tone != NULL && board.radio->tx_tone(hz);
}

#if CONFIG_SIG_TONES

// Step i of an on-air FM chain. A pause is a silent carrier (tx_tone_key), not tx_tone(0): that gives the microphone
// back on some chips (HR_C7000), so every gap would send live audio. A chain that opens with a pause first starts its
// first tone, then keys it off. A board without tx_tone_key can't key a silent carrier: the old behavior (tx_tone(0)
// between steps, the microphone live before a leading pause)
static bool chain_tone(const beep_seg_t *g, uint8_t n, uint8_t i)
{
	if (g[i].hz != 0) {
		return board.radio->tx_tone(g[i].hz);
	}
	if (board.radio->tx_tone_key == NULL) {
		return i == 0 || board.radio->tx_tone(0);
	}
	if (i == 0) {
		uint8_t k = 0;
		while (k < n && g[k].hz == 0) {
			k++;
		}
		if (k == n || !board.radio->tx_tone(g[k].hz)) {
			return false;
		}
	}
	return board.radio->tx_tone_key(false) || board.radio->tx_tone(0);
}

// The tone sent before unkeying on an FM channel when PTT is let go: the roger beep (the end tone's default, if it is
// on) or the end tone's sequence when it is set to go on air. Starts its first step; true when it runs (roger_poll
// goes through the rest and unkeys)
static bool roger_start(radio_sm_t *sm, uint32_t now_ms)
{
	uint8_t out = sig_out(sm, SIG_EV_END);
	beep_seg_t *g = sm->roger_seg;
	uint8_t n;
	if (out == SIG_OUT_DEFAULT) {
		if (!sm->set.roger_beep) {
			return false;
		}
		g[0] = (beep_seg_t){ RADIO_ROGER_HZ1, RADIO_ROGER_STEP_MS };
		g[1] = (beep_seg_t){ RADIO_ROGER_HZ2, RADIO_ROGER_STEP_MS };
		n = 2;
#if CONFIG_SIG_TONES
	} else if (sig_out_air(out, SIG_EV_END)) {
		n = sig_chain(sm->set.sig_hz[SIG_EV_END], sm->set.sig_ms[SIG_EV_END], SIG_EV_END, g);
#endif
	} else {
		return false;
	}
	if (board.radio->tx_tone == NULL || !chain_tone(g, n, 0)) {
		return false;
	}
	sm->roger_n = n;
	sm->roger_i = 0;
	sm->roger_lead = false;
	sm->roger_ms = now_ms != 0 ? now_ms : 1;
	return true;
}

// The next step of the tone, then its end (every poll while it runs)
static void roger_poll(radio_sm_t *sm, uint32_t now_ms)
{
	while (sm->roger_ms != 0 && now_ms - sm->roger_ms >= sm->roger_seg[sm->roger_i].ms) {
		sm->roger_ms += sm->roger_seg[sm->roger_i].ms;	// from the step's own start: no drift over the steps
		if (++sm->roger_i >= sm->roger_n) {
			tx_tone(0);
			tone_side_off();
			sm->roger_ms = 0;
			if (sm->roger_lead) {
				sm->roger_lead = false;	// the start tone is over: the transmission goes on, the microphone live
				return;
			}
			sm->view.roger = false;
			fm_unkey(sm, false, now_ms);
			return;
		}
		chain_tone(sm->roger_seg, sm->roger_n, sm->roger_i);
	}
}

static void roger_cancel(radio_sm_t *sm)
{
	if (sm->roger_ms != 0) {
		tx_tone(0);
		tone_side_off();
		sm->roger_ms = 0;
		sm->roger_lead = false;
		sm->view.roger = false;
		changed(sm);
	}
}
#else
// Without the chains: the roger beep alone, two fixed steps timed from roger_ms (no steps to keep in RAM)
static bool roger_start(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->set.roger_beep || !tx_tone(RADIO_ROGER_HZ1)) {
		return false;
	}
	sm->roger_ms = now_ms != 0 ? now_ms : 1;
	return true;
}

// The roger beep's second tone, then its end (every poll while it runs)
static void roger_poll(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t t = now_ms - sm->roger_ms;
	if (t >= 2u * RADIO_ROGER_STEP_MS) {
		tx_tone(0);
		sm->roger_ms = 0;
		sm->view.roger = false;
		fm_unkey(sm, false, now_ms);
	} else if (t >= RADIO_ROGER_STEP_MS && t < RADIO_ROGER_STEP_MS + RADIO_POLL_MS) {
		tx_tone(RADIO_ROGER_HZ2);
	}
}

static void roger_cancel(radio_sm_t *sm)
{
	if (sm->roger_ms != 0) {
		tx_tone(0);
		sm->roger_ms = 0;
		sm->view.roger = false;
		changed(sm);
	}
}
#endif
#endif

#if CONFIG_SIG_TONES && CONFIG_FM_TX_TONES
// The start tone goes to the speaker with the board's sidetone, on air too
static bool lead_side(const radio_sm_t *sm)
{
	return board.radio->tx_tone_side != NULL && sig_local_on(sm, SIG_EV_BEGIN) &&
	       sig_out_air(sig_out(sm, SIG_EV_BEGIN), SIG_EV_BEGIN);
}

// The start tone on air in FM (sigtone.h): right after keying, the chain goes out as tone on the carrier with
// hal_radio_t.tx_tone, step by step as the roger beep does (roger_poll), and the microphone opens when it ends. Not on
// a no-beep channel, with the host's audio, or on a board with no tx_tone
static void fm_lead_start(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->host_audio || sm->view.ch.no_beep || board.radio->tx_tone == NULL ||
	    !sig_out_air(sig_out(sm, SIG_EV_BEGIN), SIG_EV_BEGIN)) {
		return;
	}
	beep_seg_t *g = sm->roger_seg;
	uint8_t n = sig_chain(sm->set.sig_hz[SIG_EV_BEGIN], sm->set.sig_ms[SIG_EV_BEGIN], SIG_EV_BEGIN, g);
	if (!chain_tone(g, n, 0)) {
		return;
	}
	sm->roger_n = n;
	sm->roger_i = 0;
	sm->roger_ms = now_ms != 0 ? now_ms : 1;
	sm->roger_lead = true;
	// On the speaker too: the modem's sidetone plays what goes on air (the speaker copy of sig_local isn't played then)
	if (lead_side(sm)) {
		board.radio->tx_tone_side(true);
	}
}
#endif

#if CONFIG_P25
// --- P25 conventional: radio_p25.c runs the engine and the voice; the transmitter is the air modes' (below) ---

static uint32_t p25_own_id(const radio_sm_t *sm)
{
#if CONFIG_DMR
	return sm->own_id;
#else
	(void)sm;
	return codeplug_dmr_id();
#endif
}

#endif

#if CONFIG_AIR
// The stored channel an air mode runs on, 0 in the VFO: its tag gives the parameters (radio_p25_params, radio_nxdn.c)
static uint16_t air_number(const radio_sm_t *sm)
{
	return sm->view.vfo ? 0u : sm->view.channel_number;
}
#endif

#if CONFIG_AIR
// --- Air-framed modes (P25, NXDN): their modules (radio_p25.c, radio_nxdn.c) run the engine and the voice; here, when
// it runs and the transmitter, the same for each (radio_air.h). One runs at a time ---

#if CONFIG_NXDN
// NXDN is the mode whose engine runs, or the channel's when none runs
static bool nxdn_now(const radio_sm_t *sm)
{
#if CONFIG_P25
	if (radio_p25_running()) {
		return false;
	}
#endif
	return radio_nxdn_running() || sm->view.ch.mode == RADIO_MODE_NXDN;
}
#endif

static const radio_air_ops_t *air_ops(const radio_sm_t *sm)
{
#if CONFIG_NXDN && CONFIG_P25
	return nxdn_now(sm) ? &radio_nxdn_air : &radio_p25_air;
#elif CONFIG_NXDN
	(void)sm;
	return &radio_nxdn_air;
#else
	(void)sm;
	return &radio_p25_air;
#endif
}

static bool air_running(void)
{
	bool on = false;
#if CONFIG_P25
	on = radio_p25_running();
#endif
#if CONFIG_NXDN
	on = on || radio_nxdn_running();
#endif
	return on;
}

// The engine's view into the radio view. True when it changed
static bool air_poll(radio_sm_t *sm)
{
#if CONFIG_NXDN
	if (nxdn_now(sm)) {
		return radio_nxdn_poll(&sm->view.nxdn);
	}
#endif
#if CONFIG_P25
	return radio_p25_poll(&sm->view.p25);
#else
	return false;
#endif
}

// A call on the channel (our NAC or RAN): it shows as a reception, whether or not it plays
static bool air_rx(const radio_sm_t *sm)
{
#if CONFIG_NXDN
	if (nxdn_now(sm)) {
		return (sm->view.nxdn.flags & RADIO_NXDN_RX) != 0;
	}
#endif
#if CONFIG_P25
	return (sm->view.p25.flags & RADIO_P25_RX) != 0;
#else
	return false;
#endif
}

#if CONFIG_SCAN
// What a scan stop hears of the engine's call (AIR_HEARD_*): a call of the channel's NAC or RAN is pending while its
// squelch waits for a field, a late entry's talk group (P25) or a call's VCALL (NXDN); played; or decided against
static uint8_t air_heard(const radio_sm_t *sm)
{
	bool rx = false, played = false, pending = false;
#if CONFIG_NXDN
	if (nxdn_now(sm)) {
		const radio_nxdn_view_t *v = &sm->view.nxdn;
		rx = (v->flags & RADIO_NXDN_RX) != 0;
		played = (v->flags & RADIO_NXDN_PLAYED) != 0;
		pending = (v->flags & RADIO_NXDN_ENC) == 0 && v->src == 0;	// no VCALL yet: nothing decided
	} else
#endif
	{
#if CONFIG_P25
		const radio_p25_view_t *v = &sm->view.p25;
		rx = (v->flags & RADIO_P25_RX) != 0;
		played = (v->flags & RADIO_P25_PLAYED) != 0;
		// Only a late entry waits (the header gives the talk group): its link control isn't in while src is 0
		pending = (v->flags & (RADIO_P25_ENC | RADIO_P25_LATE)) == RADIO_P25_LATE && v->src == 0;
#endif
	}
	return !rx ? AIR_HEARD_NONE : played ? AIR_HEARD_PLAYED : pending ? AIR_HEARD_PENDING : AIR_HEARD_OTHER;
}
#endif

// On an air mode's channel, the board in its digital mode and tuned: the engine starts (dmr_end stopped any before)
static void air_begin(radio_sm_t *sm)
{
#if CONFIG_NXDN
	if (sm->view.ch.mode == RADIO_MODE_NXDN) {
		radio_nxdn_begin(&sm->view.ch, air_number(sm));
		radio_nxdn_poll(&sm->view.nxdn);
		return;
	}
#endif
#if CONFIG_P25
	radio_p25_begin(&sm->view.ch, air_number(sm), p25_own_id(sm));
	radio_p25_poll(&sm->view.p25);
#endif
}

// The air modes stop (leaving them, or something else takes the modem): the engine closes, which cuts a
// transmission, and the transmitter goes off. Called from dmr_end, so every path that takes the modem stops them too
static void air_stop(radio_sm_t *sm)
{
#if CONFIG_P25
	radio_p25_end();
#endif
#if CONFIG_NXDN
	radio_nxdn_end();
#endif
	if (sm->air_keyed) {
		sm->air_keyed = false;
		board.radio->tx_off();
	}
	sm->air_tx = false;
#if CONFIG_P25_TRUNK
	sm->p25_req = sm->p25_talk = false;
#endif
#if CONFIG_P25
	sm->view.p25.flags = 0;
	sm->view.p25.tx = AIR_TX_IDLE;
	sm->view.p25.trunk = RADIO_P25_TRUNK_OFF;
#endif
#if CONFIG_NXDN
	sm->view.nxdn.flags &= (uint8_t)~(RADIO_NXDN_ON | RADIO_NXDN_RX | RADIO_NXDN_PLAYED);
	sm->view.nxdn.tx = AIR_TX_IDLE;
#endif
}

// PTT on an air mode's channel, tx_check passed: the header queued, the transmitter keyed at the channel's power, the
// provider started (hal_air.h: it keys only inside that). A refusal after the engine began ends at its AIR_TX_DONE
// in air_tick, which switches off
static void air_key(radio_sm_t *sm, uint32_t now_ms, uint32_t hz)
{
	static const subtone_t none = { TONE_NONE, 0 };
	const radio_air_ops_t *o = air_ops(sm);

	if (!o->running()) {
		deny(sm, TXDENY_REFUSED);	// no engine on this board (no provider, no fit, no memory)
		return;
	}
	if (o->no_tx != NULL && o->no_tx()) {
		deny(sm, TXDENY_AIR_NO_TX);	// the provider receives this profile only
		return;
	}
	bool primed = o->tx_prepare();
	sm->air_tx = o->tx_state() != AIR_TX_IDLE;
	if (!primed) {
		o->tx_abort();
		deny(sm, TXDENY_REFUSED);
		return;
	}
	sm->view.state = RS_TX;		// speaker off, red LED on, before keying
	sm->view.tx_auto = false;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	outputs(sm);
	if (!key_tx(hz, RADIO_BW_12K5, sm->view.power, &none)) {
		o->tx_abort();
		deny(sm, board_refusal());
		return;
	}
	sm->air_keyed = true;
	if (!o->tx_go()) {
		deny(sm, TXDENY_REFUSED);	// AIR_TX_DONE already: the tick switches off
		return;
	}
	air_poll(sm);			// the engine's transmit state in the view now, not at the next tick
	changed(sm);
}

// The settings sent while it transmitted (RREQ_P25_SET, RREQ_NXDN_SET): the engine takes them now
static void air_apply(radio_sm_t *sm)
{
#if CONFIG_NXDN
	if (nxdn_now(sm)) {
		radio_nxdn_apply(&sm->view.ch, air_number(sm));
		return;
	}
#endif
#if CONFIG_P25
	radio_p25_apply(&sm->view.ch, air_number(sm), p25_own_id(sm));
#endif
}
#endif

#if CONFIG_P25_TRUNK
// --- P25 trunked (docs/spec/p25-trunking.md 7): PTT asks the follower for a voice channel; it keys there on the grant.
// Between calls the follower's inbound bursts (registration, affiliation, requests) go out on the control channel's
// inbound frequency through the same engine and checks ---

// PTT on a trunked channel, tx_check passed
static void p25t_ptt(radio_sm_t *sm)
{
	uint8_t why = radio_p25_trunk_ptt(true);
	if (why != RADIO_P25T_WHY_NONE) {
		sm->view.p25.twhy = why;	// (the next poll keeps it)
		deny(sm, TXDENY_TRUNK);
		return;
	}
	sm->p25_req = true;
	changed(sm);
}

// A frequency the follower gives us to transmit on: the board's transmit bands, and no listen-only band
static bool p25t_tx_ok(uint32_t hz)
{
	return hz != 0 && board_in_band(hz, true) && !radio_listen_band(hz);
}

// One inbound block: the transmit checks a channel's PTT has (the inhibit, the battery, a hardware option, the band),
// then keyed for the burst alone. The view stays idle (no red LED, no TX timer); air_tick switches off at its end
static void p25t_burst(radio_sm_t *sm, uint32_t hz)
{
	static const subtone_t none = { TONE_NONE, 0 };

	if (sm->set.tx_inhibit || !p25t_tx_ok(hz) || board_hwopt_pending() != NULL || sm->batt.level == BATT_CRITICAL ||
	    sm->view.ch.rx_only) {
		radio_p25_trunk_burst_drop();
		return;
	}
	if (!radio_p25_trunk_burst_start()) {
		radio_p25_tx_abort();
		sm->air_tx = radio_p25_tx_state() != AIR_TX_IDLE;
		return;
	}
	sm->air_tx = true;
	if (!key_tx(hz, RADIO_BW_12K5, sm->view.power, &none)) {
		radio_p25_tx_abort();		// AIR_TX_DONE follows: air_tick clears it
		return;
	}
	sm->air_keyed = true;
	radio_p25_tx_go();			// (false: AIR_TX_DONE already)
}

// Every tick on a trunked channel: a request's grant keys the transmitter, its failure denies; bursts between calls.
// noinline: inlined, its locals (the keying's) land in the tick's frame and, with link time optimization, in the radio
// task's own, which is under the task's deepest path
static void __attribute__((noinline)) p25t_tick(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->p25_req && !sm->air_tx) {
		uint32_t hz = 0;
		uint8_t r = radio_p25_trunk_talk(&hz);
		if (r == RADIO_P25T_GO) {
			sm->p25_req = false;
			if (!sm->ptt) {
				radio_p25_trunk_talk_end();	// released meanwhile
			} else if (!p25t_tx_ok(hz)) {
				radio_p25_trunk_talk_end();
				deny(sm, TXDENY_BAND);
			} else {
				sm->p25_talk = true;
				air_key(sm, now_ms, hz);
				if (!sm->air_tx) {
					sm->p25_talk = false;	// refused before the engine began: no AIR_TX_DONE to end it
					radio_p25_trunk_talk_end();
				}
			}
			changed(sm);
		} else if (r == RADIO_P25T_FAIL) {
			sm->p25_req = false;
			radio_p25_trunk_ptt(false);
			radio_p25_poll(&sm->view.p25);	// its twhy now
			if (sm->ptt) {
				deny(sm, TXDENY_TRUNK);
			}
			changed(sm);
		}
	}
	if (!sm->air_tx && sm->view.state != RS_TX) {
		uint32_t hz = radio_p25_trunk_burst_hz();
		if (hz != 0) {
			p25t_burst(sm, hz);
		}
	}
}
#endif

#if CONFIG_AIR
// Every tick while an air mode's engine runs: its view, the end of a transmission, the receive state
static void air_tick(radio_sm_t *sm, uint32_t now_ms)
{
	const radio_air_ops_t *o = air_ops(sm);

	if (air_poll(sm)) {
		changed(sm);
	}
	if (sm->air_tx && o->tx_state() == AIR_TX_DONE) {
		if (sm->air_keyed) {
			sm->air_keyed = false;
			board.radio->tx_off();
		}
		o->tx_clear();
		sm->air_tx = false;
#if CONFIG_P25_TRUNK
		if (sm->p25_talk) {
			sm->p25_talk = false;
			radio_p25_trunk_talk_end();	// back to the control channel
		}
#endif
		air_apply(sm);
		if (sm->view.state == RS_TX) {
			fm_tx_end(sm, true, now_ms);	// (the transmitter is off already)
			if (sm->ptt) {
				sm->ptt = false;	// pressed again while the last one ended: a new transmission
				ptt_down(sm, now_ms);
			}
		}
		changed(sm);
	}
#if CONFIG_P25_TRUNK
	if (radio_p25_trunked()) {
		p25t_tick(sm, now_ms);
	}
#endif
	if (sm->air_tx || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		return;
	}
	// A call on our NAC or RAN shows as a reception; its voice plays only if it passes the squelch (PLAYED)
	bool rx = air_rx(sm);
	if (rx && sm->view.state == RS_IDLE) {
		sm->view.state = RS_RX;
		activity(sm, now_ms);
		changed(sm);
	} else if (!rx && sm->view.state == RS_RX) {
		end_reception(sm, now_ms);
		changed(sm);
	}
	outputs(sm);
}

// The TX timer and the time-out while an air mode's transmission runs (radio_sm_poll)
static void air_poll_tx(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state != RS_TX || !sm->air_tx) {
		return;
	}
	tx_timer(sm, now_ms);
	if (sm->view.tot_s != 0 && sm->view.tx_elapsed_ms >= (uint32_t)sm->view.tot_s * 1000) {
		const radio_air_ops_t *o = air_ops(sm);	// a typed pointer: the stack check follows its calls
		o->tx_abort();	// cut at once; AIR_TX_DONE follows and air_tick switches off
		if (sm->ptt) {
			deny(sm, TXDENY_TIMEOUT);	// until PTT is released
		}
	}
}
#endif

static void ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->ptt) {
		return;
	}
#if CONFIG_DMR_TRUNK
	if (sm->t3_fb == 2) {
		t3_to_fallback(sm, now_ms);	// looking for the TSCC from the fallback channel: PTT talks on the fallback
	}
#endif
	aprs_rf_abort(sm, now_ms);	// back on the channel first: PTT talks there
	activity(sm, now_ms);
	roam_cancel(sm, now_ms);	// PTT transmits on the site the user saw before a roaming search
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 && sm->view.state == RS_TX) {
		roger_cancel(sm);	// pressed again during the roger beep: the transmission goes on
		sm->ptt = true;
		return;
	}
#endif
#if CONFIG_SUBTONE
	if (sm->tail_ms != 0 && sm->view.state == RS_TX) {
		tail_cancel(sm);	// pressed again during the tail: the transmission goes on
		sm->ptt = true;
		return;
	}
	sm->tail_ms = 0;	// a transmission that ended some other way (tx_off() ended its tail)
#endif
#if CONFIG_SCAN
#if CONFIG_DUAL_WATCH
	if (dual_running(sm)) {
		dual_talk(sm, now_ms);
	} else
#endif
	if (scan_on(&sm->scan)) {
		// On a signal: talk back on it. Still searching: back to the start channel
		bool on_signal = sm->scan.phase == SCAN_PH_HOLD || sm->scan.phase == SCAN_PH_RESUME;
		scan_stop(sm, !on_signal, now_ms);
	}
#endif
	sm->ptt = true;
	sm->tot_warned = false;
	sm->view.tot_warning = false;
	if (sm->hotspot) {
		deny(sm, TXDENY_HOTSPOT);	// the host link transmits for the hotspot; PTT does nothing until it ends
		return;
	}
#if CONFIG_AIR_RAW
	if (sm->air_bench) {
		deny(sm, TXDENY_BENCH);	// the air interface bench transmits; PTT does nothing until it ends
		return;
	}
#endif
#if CONFIG_DMR && CONFIG_SFR
	if (sm->sfr_rpt) {
		deny(sm, TXDENY_SFR_RPT);	// the repeater keys for the calls it repeats
		return;
	}
#endif
	bool was_rx = sm->view.state == RS_RX;
	end_reception(sm, now_ms);

	uint8_t reason = tx_check(sm);
	if (reason != TXDENY_NONE) {
		if (reason == TXDENY_INHIBIT && !sm->set.tx_inhibit) {
			tx_limit_refused(sm->view.ch.tx_hz);	// counted here: tx_check is also a plain test elsewhere
		}
		sm->deny_rx = reason == TXDENY_BUSY && was_rx && sm->view.ch.mode == RADIO_MODE_FM;
		deny(sm, reason);
		return;
	}
#if CONFIG_DMR
#if CONFIG_DMR_TRUNK
	if (t3_ptt_down(sm, now_ms)) {
		return;
	}
#endif
	if (sm->view.ch.mode == RADIO_MODE_DMR) {
		dmr_ptt_down(sm, now_ms);
		return;
	}
#endif
#if CONFIG_NXDN
	if (sm->view.ch.mode == RADIO_MODE_NXDN && !sm->codec) {
		deny(sm, TXDENY_NO_CODEC);	// its voice is the DMR vocoder's
		return;
	}
#endif
#if CONFIG_AIR
	if (air_mode(sm->view.ch.mode)) {
#if CONFIG_P25_TRUNK
		if (radio_p25_trunked()) {
			p25t_ptt(sm);		// the grant keys it (p25t_tick)
			return;
		}
#endif
		if (!sm->air_tx) {
			air_key(sm, now_ms, sm->view.ch.tx_hz);
		}		// else the last transmission is still ending: air_tick keys again once it is done
		return;
	}
#endif
#if CONFIG_NET_USRP
	// The host's audio replaces the microphone: never key without a way to send it
	const hal_audio_tap_t *tap = board.audio_tap;
	if (sm->host_audio && (tap == NULL || tap->fm_tx_inject == NULL)) {
		deny(sm, TXDENY_MODE);
		return;
	}
#endif
	sm->view.state = RS_TX;		// speaker off, red LED on, before keying
	sm->view.tx_auto = false;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	outputs(sm);
	const channel_t *ch = &sm->view.ch;
	if (!key_tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &ch->tx_tone)) {
		deny(sm, board_refusal());
		return;
	}
#if CONFIG_NET_USRP
	// The board ends the injection itself at tx_off() / rx()
	if (sm->host_audio && !tap->fm_tx_inject(audio_mux_fm_pull, AUDIO_TAP_VOICE)) {
		board.radio->tx_off();
		deny(sm, TXDENY_MODE);
		return;
	}
#endif
	// The start of transmission tone on the speaker. CAP_BEEP_NOT_ON_AIR is about the DMR engine closing the audio path
	// while it transmits; the FM transmitter doesn't, so the speaker copy plays on every board
	bool side = false;
#if CONFIG_FM_TX_TONES && CONFIG_SIG_TONES
	side = !sm->host_audio && board.radio->tx_tone != NULL && lead_side(sm);	// the sidetone plays it
#endif
	if (!side && sig_local_on(sm, SIG_EV_BEGIN)) {
		sig_local(sm, SIG_EV_BEGIN);
	}
#if CONFIG_FM_TX_TONES && CONFIG_SIG_TONES
	fm_lead_start(sm, now_ms);	// and on air
#endif
	changed(sm);
}

// roger: send the roger beep (if set) before unkeying
static void ptt_release(radio_sm_t *sm, bool roger, uint32_t now_ms)
{
	if (!sm->ptt) {
		return;
	}
	sm->ptt = false;
	activity(sm, now_ms);
	if (sm->host_audio) {
		roger = false;		// the host sends its own audio, tones included
	}
#if CONFIG_P25_TRUNK
	if (radio_p25_trunked()) {
		radio_p25_trunk_ptt(false);	// a request not granted yet is dropped
		sm->p25_req = false;
	}
#endif
#if CONFIG_AIR
	if (sm->air_tx && sm->view.state == RS_TX) {
		const radio_air_ops_t *o = air_ops(sm);
		o->tx_stop();	// the frame on air is finished and the end sent; air_tick ends it at done
		return;
	}
#endif
#if CONFIG_DMR
	if (sm->dmr_running) {
		if (sm->view.dmr.state == RDMR_TX) {
			sig_air_dmr(sm, SIG_EV_END);	// before the stack hears PTT is up: it holds the over open for it
		} else {
#if CONFIG_SIG_TONES
			ambeVoiceTxToneCancel();	// a start tone for a transmission that never went on air
#endif
		}
		dmr_call_ptt(false);
		dmr_call_poll(now_ms);
		if (sm->view.state == RS_TX) {
			dmr_follow(sm, now_ms);	// on air until the superframe ends and the modem reports TX done
			return;
		}
	}
	bool keyed = sm->dmr_keyed;	// the DMR keying stays armed until the stack lets go (dmr_follow)
#else
	bool keyed = false;
#endif
	dtmf_cancel(sm, now_ms);	// before the roger beep: the board never mixes the two
#if CONFIG_FM_TX_TONES
	roger_cancel(sm);	// let go during the start tone on air: it ends here (the end tone is next)
	sm->burst_timed = sm->burst_held = sm->burst_key_owned = false;
	sm->burst_ms = sm->burst_end_ms = 0;
	if (sm->view.tone_burst) {
		roger = false;	// releasing PTT during a burst ends the transmission
		tx_tone(0);
		sm->view.tone_burst = false;
	}
	sm->burst_keyed = false;
	if (roger && !keyed && sm->view.state == RS_TX && sm->view.ch.mode == RADIO_MODE_FM && roger_start(sm, now_ms)) {
		sm->view.roger = true;
		changed(sm);
		return;		// roger_poll unkeys
	}
#else
	(void)roger;
#endif
	fm_unkey(sm, keyed, now_ms);
}

static void ptt_up(radio_sm_t *sm, uint32_t now_ms)
{
#if CONFIG_DMR_TRUNK
	t3_ptt_up(sm, now_ms);
#endif
	ptt_release(sm, true, now_ms);
}

// --- Hotspot mode (core/hotspot drives the modem; hotspot_radio_ops_t in radio_task.c posts these) ---

bool radio_sm_hotspot_freq_ok(uint32_t rx_hz, uint32_t tx_hz)
{
#if CONFIG_AM_RX
	if (radio_am_tx_refused(tx_hz)) {
		return false;	// never a transmitter in the air band
	}
#endif
#if CONFIG_BCAST_FM
	if (radio_bcast_tx_refused(tx_hz)) {
		return false;	// nor in the broadcast band
	}
#endif
	return rx_hz != 0 && tx_hz != 0 && board_in_band(rx_hz, false) && board_in_band(tx_hz, true);
}

uint8_t radio_sm_hotspot_power(uint8_t rf_level)
{
	if (board.power_count == 0) {
		return 0;
	}
	return (uint8_t)(((uint32_t)rf_level * (board.power_count - 1u) + 127u) / 255u);
}

#if CONFIG_DMR
// Per-burst keying stays armed on the hotspot's TX frequency: the modem keys each burst the host sends
static void hotspot_arm(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!sm->hotspot || sm->hs_tx_hz == 0) {
		return;
	}
#if RADIO_SM_CW
	if (sm->cw_phase != 0) {
		return;		// armed again when the CW ID ends
	}
#endif
	sm->dmr_keyed = key_tx(sm->hs_tx_hz, RADIO_BW_12K5, sm->hs_power, &none);
}

#if RADIO_SM_CW
// --- CW IDs: the hotspot's (SEND_CWID, docs/spec/mmdvm-protocol.md 2.11; the hotspot is idle, mmdvm.c accepts it only
// then) and the standalone SFR repeater's (CONFIG_SFR_CWID, docs/spec/sfr.md 4, CW ID). The DMR keying is released, the
// board goes to FM and keys a carrier on the TX frequency, the board's TX tone is keyed in Morse (core/radio/morse.c)
// at the settings' speed, then FM is left: set_mode(DMR) sets up the modem again (gotcha 8.13), the receiver goes
// back to the RX frequency and the hotspot's keying is armed again, or the repeater role starts again ---

#define CW_LEAD_MS		150	// carrier up before the first element
#define CW_TAIL_MS		150	// and after the last
#define CW_TONE_MIN_HZ		300
#define CW_TONE_MAX_HZ		3000
enum { CW_OFF = 0, CW_KEYING, CW_TAIL };

static void cw_key(radio_sm_t *sm, bool down)
{
	sm->cw_key = down;
	if (board.radio->tx_tone_key != NULL && board.radio->tx_tone_key(down)) {
		return;
	}
	board.radio->tx_tone(down ? sm->cw_tone_hz : 0);
}

static void cw_ended(radio_sm_t *sm, bool sent, uint32_t ms)
{
	(void)ms;
#if CONFIG_SFR_CWID
	if (sm->cw_sfr) {
		if (sent) {
			sm->view.dmr.sfr_cwid_sent++;
		} else {
			sm->view.dmr.sfr_cwid_cut++;
		}
		sm->cw_sfr = false;
		sm->view.dmr.sfr_cw = false;
		changed(sm);
		return;
	}
#endif
#if CONFIG_HOTSPOT
	radio_hs_cw_ended(sent, ms);
#endif
}

// FM off again, the hotspot's or the repeater's DMR set-up back
static void cw_restore(radio_sm_t *sm)
{
	static const subtone_t none = { TONE_NONE, 0 };
	board.radio->tx_tone(0);
	board.radio->tx_off();
	sm->cw_phase = CW_OFF;
	sm->cw_key = false;
	sm->board_mode = RADIO_MODE_DMR;
	board.radio->set_mode(RADIO_MODE_DMR);
#if CONFIG_SFR_CWID
	if (sm->cw_sfr) {
		board.radio->rx(sm->view.ch.rx_hz, RADIO_BW_12K5, &none);
		dmr_begin(sm);		// the role again (sfr_rpt still set; cw_sfr keeps the ID schedule), or the call
					// stack if the radio may not transmit now
		outputs(sm);
		return;
	}
#endif
	if (sm->hs_rx_hz != 0) {
		board.radio->rx(sm->hs_rx_hz, RADIO_BW_12K5, &none);
	}
	hotspot_arm(sm);
	outputs(sm);
}

// The carrier up in FM on hz and the text queued after the lead-in: the keying speed, 0 if the board refused
static uint8_t cw_begin(radio_sm_t *sm, uint32_t hz, uint8_t power, const char *text, uint8_t n, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	settings_t *st = &st_copy;
	settings_read(st);
	uint8_t wpm = st->cw_wpm != 0 ? st->cw_wpm : MORSE_WPM_DEFAULT;
	if (wpm < MORSE_WPM_MIN || wpm > MORSE_WPM_MAX) {
		wpm = MORSE_WPM_DEFAULT;
	}
	uint16_t tone_hz = st->cw_tone_hz;
	sm->cw_tone_hz = (tone_hz >= CW_TONE_MIN_HZ && tone_hz <= CW_TONE_MAX_HZ) ? tone_hz : MORSE_TONE_HZ_DEFAULT;

	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);
	sm->cw_phase = CW_KEYING;	// before tx(): cw_restore and outputs see it
	if (!key_tx(hz, RADIO_BW_12K5, power, &none) || !board.radio->tx_tone(sm->cw_tone_hz)) {
		cw_restore(sm);
		cw_ended(sm, false, 0);
		return 0;
	}
	cw_key(sm, false);		// a silent carrier for the lead-in
	morse_start(&sm->cw, text, n, wpm, now_ms + CW_LEAD_MS);
	sm->cw_start_ms = now_ms;
	outputs(sm);
	changed(sm);
	return wpm;
}

#if CONFIG_HOTSPOT
// Leaving hotspot mode or retuning in the middle: the ID is cut
static void cw_abort(radio_sm_t *sm)
{
	if (sm->cw_phase != CW_OFF) {
		cw_restore(sm);
		cw_ended(sm, false, 0);
	}
}

static void cw_start(radio_sm_t *sm, uint32_t now_ms)
{
	char text[MORSE_TEXT_MAX];
	uint8_t n = radio_hs_cw_take(text, sizeof(text));
	if (n == 0) {
		return;
	}
	if (sm->cw_phase != CW_OFF || !sm->hotspot || sm->hs_tx_hz == 0 || board.radio->tx_tone == NULL ||
	    (board.dmr != NULL && board.dmr->is_transmitting())) {
		radio_hs_cw_ended(false, 0);
		return;
	}
	uint8_t wpm = cw_begin(sm, sm->hs_tx_hz, sm->hs_power, text, n, now_ms);
	if (wpm != 0) {
		radio_hs_cw_started(wpm);
	}
}
#endif

#if CONFIG_SFR_CWID
// The SFR repeater's ID, idle and due (sfr_rpt_tick): the runner lets go of the modem, sfr_rpt stays set (PTT stays
// refused, the poll leaves the state alone) and the callsign is keyed on the channel's frequency
static void cw_start_sfr(radio_sm_t *sm, uint32_t now_ms)
{
	char text[MORSE_TEXT_MAX];
	uint8_t n = radio_cwid_callsign(sm->radio_name, text, sizeof(text));
	radio_cwid_sent(&sm->cwid, now_ms);
	if (n == 0 || board.radio->tx_tone == NULL) {
		sm->view.dmr.sfr_cwid_cut++;	// nothing to send: no radio name
		return;
	}
	radio_sfr_stop();
	sm->view.dmr.sfr_keyed = false;
	sm->view.dmr.sfr_busy = false;
	sm->cw_sfr = true;
	sm->view.dmr.sfr_cw = true;
	(void)cw_begin(sm, sm->view.ch.tx_hz, sm->view.power, text, n, now_ms);
}

// A retune or hotspot mode in the middle of the repeater's ID: cut; the caller sets the board up for what follows
static void cw_cancel(radio_sm_t *sm)
{
	if (sm->cw_phase == CW_OFF || !sm->cw_sfr) {
		return;
	}
	board.radio->tx_tone(0);
	board.radio->tx_off();
	sm->cw_phase = CW_OFF;
	sm->cw_key = false;
	cw_ended(sm, false, 0);
	sfr_rpt_end(sm);
}
#endif

// Every RADIO_TICK_MS
static void cw_service(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->cw_phase == CW_KEYING) {
		bool key = morse_poll(&sm->cw, now_ms);
		if (key != sm->cw_key) {
			cw_key(sm, key);
		}
		if (sm->cw.done) {
			sm->cw_phase = CW_TAIL;
			sm->cw_ms = now_ms;
		}
	} else if (sm->cw_phase == CW_TAIL && (now_ms - sm->cw_ms) >= CW_TAIL_MS) {
		uint32_t ms = now_ms - sm->cw_start_ms;
		cw_restore(sm);
		cw_ended(sm, true, ms);
		changed(sm);
	}
}
#endif

static void hotspot_enter(radio_sm_t *sm, uint32_t rx_hz, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	bool keyed = sm->dmr_keyed;
#if CONFIG_SFR
	keyed = keyed && !sm->sfr_rpt;	// the repeater role's keying stays armed while it runs: dmr_end ends it
#endif
	if (sm->hotspot || board.dmr == NULL || sm->ptt || sm->view.state == RS_TX || keyed || sm->dmr_killed) {
		return;		// refused for now: the hotspot asks again
	}
#if CONFIG_AIR_RAW
	if (sm->air_bench) {
		return;		// the air interface bench has the modem
	}
#endif
#if CONFIG_SFR_CWID
	cw_cancel(sm);
#endif
	scan_stop(sm, false, now_ms);
	end_reception(sm, now_ms);
#if CONFIG_BCAST_FM
	bcast_power(sm, false);
	sm->view.bcast = sm->view.bcast_fault = false;
#endif
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);	// the modem is free for the hotspot
	}
	if (sm->board_mode != RADIO_MODE_DMR) {
		sm->board_mode = RADIO_MODE_DMR;
		board.radio->set_mode(RADIO_MODE_DMR);
	}
	uint32_t hz = (rx_hz != 0 && board_in_band(rx_hz, false)) ? rx_hz : sm->view.ch.rx_hz;
	if (hz != 0 && board_in_band(hz, false)) {
		board.radio->rx(hz, RADIO_BW_12K5, &none);
	}
	sm->hs_rx_hz = hz;
	sm->hs_tx_hz = 0;
	sm->hs_carrier = false;
	sm->hotspot = true;
	sm->view.hotspot = true;
	sm->view.state = RS_IDLE;
	outputs(sm);
	changed(sm);
}

static void hotspot_freq(radio_sm_t *sm, uint32_t rx_hz, uint32_t tx_hz)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!sm->hotspot || !radio_sm_hotspot_freq_ok(rx_hz, tx_hz)) {
		return;
	}
#if CONFIG_HOTSPOT
	cw_abort(sm);
#endif
	board.radio->rx(rx_hz, RADIO_BW_12K5, &none);	// disarms; armed again below
	sm->dmr_keyed = false;
	sm->hs_rx_hz = rx_hz;
	sm->hs_tx_hz = tx_hz;
	hotspot_arm(sm);
	changed(sm);
}

static void hotspot_leave(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->hotspot) {
		return;
	}
#if CONFIG_HOTSPOT
	cw_abort(sm);
#endif
	if (sm->dmr_keyed) {
		sm->dmr_keyed = false;
		board.radio->tx_off();
	}
	sm->hotspot = false;
	sm->view.hotspot = false;
	sm->hs_carrier = false;
	tune(sm, now_ms);	// the channel again; on a DMR channel the call stack takes the modem back
}
#endif

#if CONFIG_AIR_RAW
// --- The debug link's air interface bench (debuglink_air.h): it takes the modem as a hotspot does, and keys only
// through here ---

static void bench_unkey(radio_sm_t *sm)
{
	if (!sm->bench_keyed) {
		return;
	}
	sm->bench_keyed = false;
	board.radio->tx_off();
	sm->view.state = RS_IDLE;
	outputs(sm);
	changed(sm);
}

// The DMR stack and P25 stopped, the board receiving in its digital mode on the channel's frequency. Refused while
// transmitting or a hotspot (the bench asks again)
static void bench_enter(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	bool keyed = false;
#if CONFIG_DMR
	keyed = sm->dmr_keyed;
#endif
	if (sm->air_bench || sm->hotspot || sm->ptt || sm->view.state == RS_TX || keyed) {
		return;
	}
#if CONFIG_SFR_CWID
	cw_cancel(sm);
#endif
	scan_stop(sm, false, now_ms);
	end_reception(sm, now_ms);
#if CONFIG_BCAST_FM
	bcast_power(sm, false);
	sm->view.bcast = sm->view.bcast_fault = false;
#endif
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);	// the call stack and P25 let go of the modem
	}
	if (sm->board_mode != RADIO_MODE_DMR) {
		sm->board_mode = RADIO_MODE_DMR;
		board.radio->set_mode(RADIO_MODE_DMR);
	}
	if (sm->view.ch.rx_hz != 0 && board_in_band(sm->view.ch.rx_hz, false)) {
		board.radio->rx(sm->view.ch.rx_hz, RADIO_BW_12K5, &none);
	}
	sm->air_bench = true;
	sm->view.state = RS_IDLE;
	outputs(sm);
	changed(sm);
}

static void bench_leave(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->air_bench) {
		return;
	}
	bench_unkey(sm);
	sm->air_bench = false;
	tune(sm, now_ms);	// the channel again: DMR or P25 take the modem back
}

// The core's transmit checks, the transmitter on the channel's TX frequency at the lowest power, then the provider's
// tx_begin (the bench queued its first chunk). Off again after max_ms at most, or at the channel's time-out
static void bench_key(radio_sm_t *sm, uint16_t max_ms, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!sm->air_bench || sm->bench_keyed || board.air == NULL || max_ms == 0 || tx_check(sm) != TXDENY_NONE) {
		return;
	}
	if (!key_tx(sm->view.ch.tx_hz, RADIO_BW_12K5, 0, &none)) {	// level 0: the board's lowest (bench signals)
		return;
	}
	if (!board.air->tx_begin()) {
		board.radio->tx_off();
		return;
	}
	sm->bench_keyed = true;
	sm->bench_max_ms = max_ms;
	sm->bench_key_ms = now_ms;
	sm->view.state = RS_TX;
	sm->view.tx_auto = true;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	outputs(sm);
	changed(sm);
}

// The hard cap: the provider stops at once (its AIR_EV_TX_DONE still reaches the bench) and the transmitter goes off
static void bench_tick(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t cap = sm->bench_max_ms;
	if (sm->view.tot_s != 0 && (uint32_t)sm->view.tot_s * 1000u < cap) {
		cap = (uint32_t)sm->view.tot_s * 1000u;
	}
	sm->view.tx_elapsed_ms = now_ms - sm->bench_key_ms;
	if (sm->view.tx_elapsed_ms >= cap) {
		board.air->tx_abort();
		bench_unkey(sm);
	}
}
#endif

// --- Settings: what the radio was set to, back at power-on (core/settings), and saved when it changes ---

// The preferences the radio core uses: base squelch, master power, VFO step. The own DMR ID is the codeplug's
static void apply_prefs(radio_sm_t *sm, const settings_t *st)
{
	master_power = st->master_power;
	sm->vol_step = radio_volume_step(st->volume);
	if (st->squelch <= RADIO_SQUELCH_MAX && sm->base_squelch != st->squelch) {
		sm->base_squelch = st->squelch;
		if (!sm->temp_squelch) {
			sm->view.squelch = st->squelch;
		}
	}
	if (!sm->temp_power) {
		sm->view.power = channel_power(&sm->view.ch);
	}
	sm->vfo_step_set = st->vfo_step_hz >= 1000u && st->vfo_step_hz <= 1000000u ? st->vfo_step_hz : 0;
	step_update(sm);
#if CONFIG_DMR
	sm->own_id = codeplug_dmr_id();	// set on the radio in Settings > My radio, which writes the codeplug
	if (sm->own_id == 0) {
		sm->own_id = RADIO_DMR_ID_DEFAULT;
	}
	sm->view.dmr.own_id = sm->own_id;
	// The radio name (talker alias text): the settings' override, else the codeplug's; hang times from the codeplug
	codeplug_dmr_settings_t gs;
	codeplug_dmr_settings(&gs);
	if (st->radio_name[0] != '\0') {
		memset(gs.radio_name, 0, sizeof(gs.radio_name));
		memcpy(gs.radio_name, st->radio_name, CODEPLUG_RADIO_NAME_LEN);
	}
	memcpy(sm->radio_name, gs.radio_name, sizeof(sm->radio_name));
	sm->group_hang_ms = gs.group_hang_ms;
	sm->private_hang_ms = gs.private_hang_ms;
#if CONFIG_DMR_PRIVACY
	// The privacy key in use, kept in the settings (slot 1 for now; per-channel choice: codeplug-native.md)
	uint8_t slot = st->privacy_active;
	if (slot >= 1 && slot <= SETTINGS_PRIVACY_KEYS && st->privacy_type[slot - 1] < DMR_PRIVACY_NUM_TYPES &&
	    st->privacy_type[slot - 1] != DMR_PRIVACY_OFF &&
	    dmrPrivacyKeyIsValid(st->privacy_type[slot - 1], (uint16_t)st->privacy_key[slot - 1])) {
		sm->privacy_type = st->privacy_type[slot - 1];
		sm->privacy_key = (uint16_t)st->privacy_key[slot - 1];
	} else {
		sm->privacy_type = DMR_PRIVACY_OFF;
		sm->privacy_key = 0;
	}
#endif
#endif
	set_temporary(sm);
}

// The VFO as saved, if its frequency still fits the board
static void vfo_defaults(channel_t *c)
{
	memset(c, 0, sizeof(*c));
	strcpy(c->name, "VFO");
	c->mode = VFO_MODE_DEFAULT;
	c->bw = RADIO_BW_12K5;
	c->power = CODEPLUG_POWER_DEFAULT;
	c->timeslot = 1;
	c->color_code = 1;	// the settings' default too: the usual repeater color code
}

// One saved VFO, whichever settings fields hold it: VFO 0 the first ones (packed), VFO 1 page 1's
typedef struct {
	uint32_t rx_hz, tx_hz;
	uint32_t talk;		// ID in bits 0-23, bit 31 private call
	uint16_t rx_tone, tx_tone;
	uint8_t mode, bw, power, cc, ts, rx_tone_type, tx_tone_type;
	uint8_t sfr_a;		// its SFR role: a channel's SFR byte A, role 0 none
} vfo_saved_t;

static void vfo_saved_get(const settings_t *st, uint8_t idx, vfo_saved_t *o)
{
#if CONFIG_DUAL_WATCH
	if (idx != 0) {
		*o = (vfo_saved_t){ st->vfo2_rx_hz, st->vfo2_tx_hz, st->vfo2_talk, st->vfo2_rx_tone, st->vfo2_tx_tone,
				    st->vfo2_mode, st->vfo2_bw, st->vfo2_power, st->vfo2_cc, st->vfo2_ts,
				    st->vfo2_rx_tone_type, st->vfo2_tx_tone_type, st->vfo2_sfr };
		return;
	}
#endif
	(void)idx;
	*o = (vfo_saved_t){ st->vfo_rx_hz, st->vfo_tx_hz,
			    settings_vfo_talk_id(st) | (settings_vfo_talk_private(st) ? 0x80000000u : 0),
			    st->vfo_rx_tone, st->vfo_tx_tone, settings_vfo_mode(st), settings_vfo_bw(st), st->vfo_power,
			    settings_vfo_cc(st), settings_vfo_ts(st), st->vfo_rx_tone_type, st->vfo_tx_tone_type,
			    st->vfo_sfr };
}

static void restore_vfo(radio_sm_t *sm, const settings_t *st, uint8_t idx)
{
	vfo_saved_t v;
	vfo_saved_get(st, idx, &v);
#if CONFIG_VFO_DMR
	uint32_t id = v.talk & 0xFFFFFFu;
	sm->vfo_talk_id = id <= DMR_ID_MAX ? id : 0;
	sm->vfo_talk_private = sm->vfo_talk_id != 0 && (v.talk & 0x80000000u) != 0;
#endif	// a build without the DMR VFO leaves the saved target alone (persist() doesn't write it)
	if (v.rx_hz == 0 || !board_in_band(v.rx_hz, false)) {
		return;
	}
	sm->vfo_ch.rx_hz = v.rx_hz;
	sm->vfo_ch.tx_hz = v.tx_hz != 0 ? v.tx_hz : v.rx_hz;
	// The saved mode if this build has that VFO (P25: and the board runs it); an older record holds FM
	sm->vfo_ch.mode = v.mode == RADIO_MODE_DMR && CONFIG_VFO_DMR ? RADIO_MODE_DMR :
		v.mode == RADIO_MODE_FM && CONFIG_VFO_FM ? RADIO_MODE_FM :
		v.mode == RADIO_MODE_P25 && p25_ok() ? RADIO_MODE_P25 :
		v.mode == RADIO_MODE_NXDN && nxdn_ok() ? RADIO_MODE_NXDN : VFO_MODE_DEFAULT;
	if (sm->vfo_ch.mode != v.mode) {
		sm->view.vfo_drop = (uint8_t)(v.mode + 1u);	// the debug link's vfo_drop: why the VFO came back changed
	}
	sm->vfo_ch.bw = v.bw == RADIO_BW_25K ? RADIO_BW_25K : RADIO_BW_12K5;
	sm->vfo_ch.power = v.power < board.power_count ? v.power : CODEPLUG_POWER_DEFAULT;
	sm->vfo_ch.color_code = v.cc <= 15 ? v.cc : 1;
	sm->vfo_ch.timeslot = v.ts == 2 ? 2 : 1;
	subtone_t rx = { v.rx_tone_type, v.rx_tone }, tx = { v.tx_tone_type, v.tx_tone };
	sm->vfo_ch.rx_tone = tones_valid(&rx) ? rx : (subtone_t){ TONE_NONE, 0 };
	sm->vfo_ch.tx_tone = tones_valid(&tx) ? tx : (subtone_t){ TONE_NONE, 0 };
	vfo_sfr_set(&sm->vfo_ch, v.sfr_a);
	sm->vfo_seeded = true;
}

// The VFO in use (vfo_ch and its target) into VFO idx's settings fields
static void persist_vfo(const radio_sm_t *sm, settings_t *st, uint8_t idx)
{
	const channel_t *v = &sm->vfo_ch;
	uint8_t power = v->power == CODEPLUG_POWER_DEFAULT ? SETTINGS_UNSET8 : v->power;
#if CONFIG_DUAL_WATCH
	if (idx != 0) {
		if (sm->vfo_seeded) {
			st->vfo2_rx_hz = v->rx_hz;
			st->vfo2_tx_hz = v->tx_hz;
			st->vfo2_mode = v->mode;	// P25 and NXDN too: a build without them reads its default
			st->vfo2_bw = v->bw;
			st->vfo2_power = power;
			st->vfo2_cc = v->color_code;
			st->vfo2_ts = v->timeslot;
			st->vfo2_rx_tone_type = v->rx_tone.type;
			st->vfo2_rx_tone = v->rx_tone.value;
			st->vfo2_tx_tone_type = v->tx_tone.type;
			st->vfo2_tx_tone = v->tx_tone.value;
			st->vfo2_sfr = v->sfr_a;
		}
#if CONFIG_VFO_DMR
		st->vfo2_talk = sm->vfo_talk_id | (sm->vfo_talk_private ? 0x80000000u : 0);
#endif
		return;
	}
#endif
	(void)idx;
	if (sm->vfo_seeded) {
		st->vfo_rx_hz = v->rx_hz;
		st->vfo_tx_hz = v->tx_hz;
		settings_set_vfo_mode_bw(st, v->mode, v->bw);
		st->vfo_power = power;
		settings_set_vfo_cc_ts(st, v->color_code, v->timeslot);
		st->vfo_rx_tone_type = v->rx_tone.type;
		st->vfo_rx_tone = v->rx_tone.value;
		st->vfo_tx_tone_type = v->tx_tone.type;
		st->vfo_tx_tone = v->tx_tone.value;
		st->vfo_sfr = v->sfr_a;
	}
#if CONFIG_VFO_DMR
	settings_set_vfo_talk(st, sm->vfo_talk_id, sm->vfo_talk_private);
#endif
}

// The position of channel number in z, or -1
static int zone_find(const zone_t *z, uint16_t number)
{
	for (uint16_t i = 0; number != 0 && i < z->count; i++) {
		if (z->channels[i] == number) {
			return i;
		}
	}
	return -1;
}

// Zone zi, on channel number (or else near pos); false if the zone has no readable channel
static bool select_in_zone(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	zone_t z;
	uint16_t p;
	channel_t ch;
	if (!read_zone(zi, &z)) {
		return false;
	}
	int at = zone_find(&z, number);
	if (at >= 0 && channel_read(number, &ch)) {
		p = (uint16_t)at;
	} else if (!find_channel(&z, pos < z.count ? pos : 0, 1, &p, &ch)) {
		return false;
	}
	set_zone(sm, zi, &z);
	set_channel(sm, p, &ch);
	sm->have_channels = true;
	return true;
}

// Zone and channel: the saved ones, else zone 0's first channel, else the first zone that has one
static void select_position(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	uint16_t zones = codeplug_zone_count();
	sm->have_channels = false;
	if (zi < zones && select_in_zone(sm, zi, number, pos)) {
		return;
	}
	for (uint16_t i = 0; i < zones; i++) {
		if (select_in_zone(sm, i, 0, 0)) {
			return;
		}
	}
	memset(&sm->zone, 0, sizeof(sm->zone));
}

// Note the position and the VFO in the settings (saved later, when they rest; settings_end only marks changes)
static void persist(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot) {
		return;		// the hotspot's frequencies are the host's, not the radio's
	}
	settings_t *st = settings_begin();
	bool vfo = sm->view.vfo;
	uint8_t vfo_index = sm->vfo_idx;
	uint16_t zone = sm->view.zone_index, pos = sm->view.zone_pos;
	const zone_t *z = sm->have_channels ? &sm->zone : NULL;
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	zone_t buf;
	if (dual_running(sm) && !dual_here(sm, sm->dual_sel)) {
		// Dual watch on the other channel now: power up on the selected one
		const radio_dual_ref_t *r = &sm->dual[sm->dual_sel];
		vfo = r->vfo;
		if (r->vfo) {
			vfo_index = (uint8_t)r->pos;	// power up in the selected channel's VFO
		} else {
			if (r->zone != zone) {
				z = read_zone(r->zone, &buf) ? &buf : NULL;
			}
			zone = r->zone;
			pos = r->pos;
		}
	}
#endif
	st->vfo = vfo;
	// Zones moved and not followed yet: the indexes here are the old ones, and the zone write saved the new ones
	// (zone_settle.c). Checked under the settings lock, so a write can't come between
	if (z != NULL && !ZONES_MOVED(sm)) {
		st->zone_index = zone;
		st->zone_pos = pos;
		st->channel = pos < z->count ? z->channels[pos] : 0;
	}
	persist_vfo(sm, st, sm->vfo_idx);
	if (sm->squelch_save) {
		st->squelch = sm->base_squelch;
		sm->squelch_save = false;
	}
#if CONFIG_DUAL_WATCH
	uint8_t in_use = sm->vfo_idx;
	vfo_use(sm, (uint8_t)!in_use);		// the other VFO into its own fields: swap, write, swap back
	persist_vfo(sm, st, sm->vfo_idx);
	vfo_use(sm, in_use);
	st->vfo_index = vfo_index;
#else
	(void)vfo_index;
#endif
#if CONFIG_DMR_PRIVACY
	if (sm->privacy_type != DMR_PRIVACY_OFF) {
		st->privacy_active = 1;
		st->privacy_type[0] = sm->privacy_type;
		st->privacy_key[0] = sm->privacy_key;
	} else {
		st->privacy_active = 0;		// the key stays in its slot
	}
#endif
	settings_end(now_ms);
}

static void clamp_settings(radio_settings_t *s)
{
#if CONFIG_SIG_TONES
	for (int i = 0; i < SIG_EV_COUNT; i++) {
		s->sig_dmr[i] = sig_out_clean(s->sig_dmr[i], (sig_event_t)i);
		s->sig_fm[i] = sig_out_clean(s->sig_fm[i], (sig_event_t)i);
		for (unsigned k = 0; k < SIG_STEPS; k++) {
			if (s->sig_hz[i][k] > SIG_HZ_MAX) {
				s->sig_hz[i][k] = SIG_HZ_MAX;
			}
			s->sig_hz[i][k] = (uint16_t)(s->sig_hz[i][k] / SIG_HZ_UNIT * SIG_HZ_UNIT);
			if (s->sig_ms[i][k] > SIG_MAX_MS / SIG_MS_UNIT) {
				s->sig_ms[i][k] = SIG_MAX_MS / SIG_MS_UNIT;
			}
		}
	}
#endif
	if (s->beep_level > 100) {
		s->beep_level = 100;
	}
	if (s->vox_level > 10) {
		s->vox_level = 10;
	}
	if (s->rx_save > 3) {
		s->rx_save = 3;
	}
	if (s->reply_s > 30 && s->reply_s != RADIO_REPLY_CODEPLUG) {
		s->reply_s = 30;
	}
	if (s->dual_delay_ms > RADIO_DUAL_DELAY_MAX_MS) {
		s->dual_delay_ms = RADIO_DUAL_DELAY_MAX_MS;
	}
	if (s->bcast_step > RADIO_BCAST_STEP_100) {
		s->bcast_step = RADIO_BCAST_STEP_200_ODD;
	}
	if (s->roam_rssi_dbm < -125) {
		s->roam_rssi_dbm = -125;
	} else if (s->roam_rssi_dbm > -60) {
		s->roam_rssi_dbm = -60;
	}
	if (s->roam_hyst_db < 1) {
		s->roam_hyst_db = 1;
	} else if (s->roam_hyst_db > 30) {
		s->roam_hyst_db = 30;
	}
	if (s->roam_probe > 1) {
		s->roam_probe = 1;
	}
	if (s->roam_check_s != 0 && s->roam_check_s < 30) {
		s->roam_check_s = 30;
	} else if (s->roam_check_s > 3600) {
		s->roam_check_s = 3600;
	}
	if (s->roam_beacon_s != 0 && s->roam_beacon_s < 10) {
		s->roam_beacon_s = 10;
	} else if (s->roam_beacon_s > 600) {
		s->roam_beacon_s = 600;
	}
	if (s->roam_beacon_len_s < 1) {
		s->roam_beacon_len_s = 1;
	} else if (s->roam_beacon_len_s > 10) {
		s->roam_beacon_len_s = 10;
	}
	if (s->roam_survey > 2) {
		s->roam_survey = 1;
	}
	// 0 repeat only, 1 both (first arrival), 2 both (dual-path RX, CONFIG_SFR_DUALPATH_RX; else first arrival)
	if (s->sfr_rx > 2) {
		s->sfr_rx = 0;
	} else if (s->sfr_rx == 2 && !CONFIG_SFR_DUALPATH_RX) {
		s->sfr_rx = 1;
	}
	if (s->sfr_cwid > 4 || !CONFIG_SFR_CWID) {
		s->sfr_cwid = 0;
	}
}

// The radio core's settings as kept in the settings store (docs/spec/settings-store.md 3); the defaults if never set
static void settings_to_radio(const settings_t *st, radio_settings_t *s)
{
	radio_settings_default(s);
	if (st->roam_stored == 1) {
		s->roam_rssi_dbm = st->roam_rssi_dbm;
		s->roam_hyst_db = st->roam_hyst_db;
		s->roam_probe = st->roam_probe;
		s->roam_check_s = st->roam_check_s;
		s->roam_beacon_s = st->roam_beacon_s;
		s->roam_beacon_len_s = st->roam_beacon_len_s;
		s->roam_survey = st->roam_survey;
		s->rpt_beacon = (st->rpt_beacon & SETTINGS_RPT_BEACON) != 0;
		s->sfr_rx = (uint8_t)((st->rpt_beacon & SETTINGS_RPT_SFR_RX) >> SETTINGS_RPT_SFR_RX_SHIFT);
		s->sfr_cwid = (uint8_t)((st->rpt_beacon & SETTINGS_RPT_SFR_CWID) >> SETTINGS_RPT_SFR_CWID_SHIFT);
		clamp_settings(s);
	}
	// Page 1: 0 (never set, or a page 0 alone) keeps the default
	if (st->rs_dual_delay != 0) {
		s->dual_delay_ms = (uint16_t)((st->rs_dual_delay - 1u) * 100u);
		clamp_settings(s);
	}
	if (st->rs_bcast_step != 0) {
		s->bcast_step = (uint8_t)(st->rs_bcast_step - 1u);
		clamp_settings(s);
	}
	s->tones_off = settings_tones_off(st) & RADIO_TONES_MASK;	// 0, all on, in a record from before it
#if CONFIG_SIG_TONES
	for (int i = 0; i < SIG_EV_COUNT; i++) {
		s->sig_dmr[i] = st->sig_dmr[i];	// 0 = default, in a record from before them; clamp_settings checks
		s->sig_fm[i] = st->sig_fm[i];
	}
#endif
	if (st->rs_stored != 1) {
		return;
	}
	s->tot_s = st->rs_tot_s;
	s->tot_warn_s = st->rs_tot_warn_s;
	s->bcl = st->rs_bcl;
	s->roger_beep = (st->rs_flags & SETTINGS_RS_ROGER_BEEP) != 0;
	s->tx_inhibit = (st->rs_flags & SETTINGS_RS_TX_INHIBIT) != 0;
	s->scan_priority = (st->rs_flags & SETTINGS_RS_SCAN_PRIORITY) != 0;
	s->scan_beep = (st->rs_flags & SETTINGS_RS_SCAN_BEEP) != 0;
	s->key_beep = (st->rs_flags & SETTINGS_RS_KEY_BEEP) != 0;
	s->talk_permit = (st->rs_flags & SETTINGS_RS_NO_TALK_PERMIT) == 0;
	s->scan_resume = st->rs_scan_resume;
	s->scan_hold_s = st->rs_scan_hold_s;
	s->scan_delay_ms = st->rs_scan_delay_ms;
	s->scan_dwell_ms = st->rs_scan_dwell_ms;
	s->beep_level = st->rs_beep_level;
	s->vox = st->rs_vox;
	s->vox_level = st->rs_vox_level;
	s->vox_tail_ms = st->rs_vox_tail_ms;
	s->rx_save = st->rs_rx_save;
	s->rx_save_idle_s = st->rs_rx_save_idle_s;
	s->battery_off_s = st->rs_battery_off_s;
	s->backlight_dim_s = st->rs_backlight_dim_s;
	s->backlight_off_s = st->rs_backlight_off_s;
	s->reply_s = st->rs_reply_s;
	clamp_settings(s);
}

static void radio_to_settings(const radio_settings_t *s, uint32_t now_ms)
{
	settings_t *st = settings_begin();
	st->rs_tot_s = s->tot_s;
	st->rs_tot_warn_s = s->tot_warn_s;
	st->rs_bcl = s->bcl;
	st->rs_flags = (uint8_t)((s->roger_beep ? SETTINGS_RS_ROGER_BEEP : 0) | (s->tx_inhibit ? SETTINGS_RS_TX_INHIBIT : 0) |
				 (s->scan_priority ? SETTINGS_RS_SCAN_PRIORITY : 0) |
				 (s->scan_beep ? SETTINGS_RS_SCAN_BEEP : 0) | (s->key_beep ? SETTINGS_RS_KEY_BEEP : 0) |
				 (s->talk_permit ? 0 : SETTINGS_RS_NO_TALK_PERMIT));
	st->rs_scan_resume = s->scan_resume;
	st->rs_scan_hold_s = s->scan_hold_s;
	st->rs_scan_delay_ms = s->scan_delay_ms;
	st->rs_scan_dwell_ms = s->scan_dwell_ms;
	st->rs_beep_level = s->beep_level;
	st->rs_vox = s->vox;
	st->rs_vox_level = s->vox_level;
	st->rs_vox_tail_ms = s->vox_tail_ms;
	st->rs_rx_save = s->rx_save;
	st->rs_rx_save_idle_s = s->rx_save_idle_s;
	st->rs_battery_off_s = s->battery_off_s;
	st->rs_backlight_dim_s = s->backlight_dim_s;
	st->rs_backlight_off_s = s->backlight_off_s;
	st->rs_reply_s = s->reply_s;
	st->rs_dual_delay = (uint8_t)(s->dual_delay_ms / 100u + 1u);
	st->rs_bcast_step = (uint8_t)(s->bcast_step + 1u);
	settings_set_tones_off(st, s->tones_off);
#if CONFIG_SIG_TONES
	for (int i = 0; i < SIG_EV_COUNT; i++) {
		st->sig_dmr[i] = s->sig_dmr[i];
		st->sig_fm[i] = s->sig_fm[i];
	}
#endif
	st->rs_stored = 1;
	st->roam_rssi_dbm = s->roam_rssi_dbm;
	st->roam_hyst_db = s->roam_hyst_db;
	st->roam_probe = s->roam_probe;
	st->roam_check_s = s->roam_check_s;
	st->roam_beacon_s = s->roam_beacon_s;
	st->roam_beacon_len_s = s->roam_beacon_len_s;
	st->roam_survey = s->roam_survey;
	st->rpt_beacon = (uint8_t)((s->rpt_beacon ? SETTINGS_RPT_BEACON : 0) |
				   ((s->sfr_rx << SETTINGS_RPT_SFR_RX_SHIFT) & SETTINGS_RPT_SFR_RX) |
				   ((s->sfr_cwid << SETTINGS_RPT_SFR_CWID_SHIFT) & SETTINGS_RPT_SFR_CWID));
	st->roam_stored = 1;
	settings_end(now_ms);
}

static void use_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
#if CONFIG_DMR
	bool reply_changed = s->reply_s != sm->set.reply_s || s->sfr_rx != sm->set.sfr_rx ||
		s->talk_permit != sm->set.talk_permit	// the permit hold (permit_before_key)
#if CONFIG_SIG_TONES
		|| s->sig_dmr[SIG_EV_BEGIN] != sm->set.sig_dmr[SIG_EV_BEGIN]
#endif
		;
#endif
	sm->set = *s;
	clamp_settings(&sm->set);
	apply_tot(sm);
#if CONFIG_DMR_ROAMING
	roam_cfg_t rc;
	roam_cfg_of(&sm->set, &rc);
	roam_set_cfg(&sm->roam, &rc);
#endif
	activity(sm, now_ms);
#if CONFIG_DMR
	if (reply_changed && sm->dmr_running) {
		dmr_configure(sm);	// the reply window, an SFR user's copies, the talk permit
	}
#endif
#if CONFIG_VOX
	if (sm->vox_keyed && sm->set.vox == RADIO_VOX_OFF) {
		sm->vox_keyed = false;
		sm->view.vox_keyed = false;
		ptt_release(sm, false, now_ms);
	}
#endif
	changed(sm);
}

// From radio_set_settings (the settings screen, the debug link): in effect at once, saved by the settings store
void radio_sm_set_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
	use_settings(sm, s, now_ms);
	tones_apply(&sm->set);	// a tone's key that is present holds against the change (codeplug-settings.md 5.2)
	radio_to_settings(&sm->set, now_ms);
}

// The codeplug changed underneath (an edit, a CPS write) or the preferences did: read them again, keep the zone and
// the channel if they still exist
#if CONFIG_ZONE_WRITE
// A zone added ahead of others or a zone deleted moves the indexes after it (zone_edit.h). Followed here: the zone in
// use (in the VFO too: the zone it goes back to) and dual watch's two channels. The saved roaming zones were moved by
// the zone write itself (zone_settle.c; a deleted one loses its flag) and the reload reads them. Returns the zone in
// use's index now; *gone: it was deleted, and the index is the zone that took its place
static uint16_t follow_zones(radio_sm_t *sm, bool *gone, uint32_t now_ms)
{
	uint32_t from = sm->zone_shifts, to = zone_edit_shifts();
	uint16_t zi = sm->view.zone_index, out;
	sm->zone_shifts = to;
	*gone = false;
	if (from == to) {
		return zi;
	}
	zone_map_t m = zone_edit_map(from, to, zi, &out);
	if (m != ZONE_MAP_LOST) {
		zi = out;
		*gone = m == ZONE_MAP_GONE;
	}
#if CONFIG_SCAN
	for (uint8_t i = 0; i < 2; i++) {
		if (!sm->dual[i].vfo && zone_edit_map(from, to, sm->dual[i].zone, &out) != ZONE_MAP_LOST) {
			sm->dual[i].zone = out;		// a deleted one: the UI finds its channel again or ends the watch
		}
	}
#endif
	(void)now_ms;
	return zi;
}

#if CONFIG_SCAN
// A checksum of the zone in use and its channel list: an edit of it ends a zone scan (its skips are by position)
static uint32_t zone_sum(const radio_sm_t *sm)
{
	uint32_t h = sm->have_channels ? 2166136261u ^ sm->zone.count : 0;
	for (uint16_t i = 0; sm->have_channels && i < sm->zone.count; i++) {
		h = (h ^ sm->zone.channels[i]) * 16777619u;
	}
	return h ^ sm->view.zone_index;
}
#endif
#endif

static void reload(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t *st = &st_copy;
	roam_cancel(sm, now_ms);	// back to the site the search started from first
	settings_read(st);
#if CONFIG_DMR_ROAMING
	memcpy(sm->roam_zones, st->roam_zones, sizeof(sm->roam_zones));
	sm->roam_dirty = true;
#endif
	sm->reload_pending = false;
	sm->codeplug_serial = codeplug_serial();
	sm->view.source = codeplug_source();
#if CONFIG_DMR
	// The cached names may be gone or new now ("" is cached too): a contact added after its ID was heard or called
	// would otherwise stay a bare ID on the screens until it fell out of the cache
	memset(sm->lookups, 0, sizeof(sm->lookups));
	sm->lookup_next = 0;
#endif
	uint16_t number = sm->have_channels && sm->view.zone_pos < sm->zone.count ? sm->zone.channels[sm->view.zone_pos] : 0;
	bool vfo = sm->view.vfo;
	uint16_t zi = sm->view.zone_index, pos = sm->view.zone_pos;
#if CONFIG_ZONE_WRITE && CONFIG_SCAN
	uint32_t sum = zone_sum(sm);	// read by the zone scan check below
#endif
#if CONFIG_ZONE_WRITE
	bool gone;
	zi = follow_zones(sm, &gone, now_ms);
	if (gone) {
		// The zone in use was deleted: the zone that took its place (the last one when it was the last), on the
		// same channel when that zone holds it, else its first channel
		uint16_t n = codeplug_zone_count();
		zi = n > 0 && zi >= n ? (uint16_t)(n - 1u) : zi;
		pos = 0;
	}
	sm->view.zone_index = zi;
#endif
	select_position(sm, zi, number, pos);
#if CONFIG_ZONE_WRITE && CONFIG_SCAN
	if (sm->scan.kind == SCAN_ZONE && zone_sum(sm) != sum) {
		scan_stop(sm, false, now_ms);
	}
#endif
	apply_prefs(sm, st);
	radio_settings_t rs;
	settings_to_radio(st, &rs);
	use_settings(sm, &rs, now_ms);
	kv_load(sm, now_ms);	// after the settings: the keys hold over the radio's own copies
#if CONFIG_RX_EXT
	// A VFO left in the receive range past the table, now off, goes back to a band the next time it is used
	if (sm->vfo_seeded && !board_in_band(sm->vfo_ch.rx_hz, false)) {
		sm->vfo_seeded = false;
	}
#if CONFIG_DUAL_WATCH
	if (sm->vfo_other.seeded && !board_in_band(sm->vfo_other.ch.rx_hz, false)) {
		sm->vfo_other.seeded = false;
	}
#endif
#endif
	if (vfo || !sm->have_channels) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");
			changed(sm);
		}
	} else {
		tune(sm, now_ms);
	}
}

#if CONFIG_NET_USRP
// A host transmission whose audio stopped coming ends here too: the network service's own watchdog doesn't run while
// the USB link isn't in network mode (a personality switch mid-transmission)
static void host_audio_watch(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->ptt || !sm->host_audio) {
		return;
	}
	uint32_t p = audio_mux_host_pushes();
	if (p != sm->host_pushes) {
		sm->host_pushes = p;
		sm->host_push_ms = now_ms;
	} else if ((now_ms - sm->host_push_ms) >= AUDIO_MUX_HOST_STALE_MS) {
		ptt_release(sm, false, now_ms);
	}
}

#endif
#if CONFIG_BEEP || CONFIG_DMR || CONFIG_P25   // its only users; without them, it would be unused (a warning)
// The PCM stream the radio's own users (beeps, DMR voice) take: the board's, shared with a host audio session
static const hal_audio_t *pcm_audio(void)
{
#if CONFIG_NET_USRP || CONFIG_APRS_RX
	return audio_mux_wrap(board.audio);
#else
	return board.audio;
#endif
}
#endif

void radio_sm_init(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t st;
	settings_read(&st);	// a zone add or delete a power cut interrupted is settled first (radio_core_start)
#if CONFIG_POSITION
	position_init();
	position_load(&st);
#endif

	memset(sm, 0, sizeof(*sm));
#if CONFIG_SFR
	direct_sm = sm;
	if (board.radio->dmr_direct_query != NULL) {
		board.radio->dmr_direct_query(sfr_repeats_now);
	}
#endif
	sm->audio_out = -1;
	sm->green_out = -1;
#if CONFIG_BCAST_FM
	sm->bcast_audio_out = -1;
	sm->bcast_volume_out = -1;
#endif
	sm->red_out = -1;
#if CONFIG_DUAL_WATCH
	sm->rx2_audio_out = -1;
#endif
	sm->volume_sent = -1;
	sm->vol_step = RADIO_VOLUME_DEFAULT;
	sm->vol_override = -1;
	sm->knob_seen = -1;
	sm->vol_hold = false;
	sm->view.volume = -1;
	sm->view.volume_knob = -1;
	sm->view.volume_override = -1;
	sm->view.volume_step = RADIO_VOLUME_DEFAULT;
	sm->base_squelch = RADIO_SQUELCH_DEFAULT;
	sm->view.squelch = RADIO_SQUELCH_DEFAULT;
	settings_to_radio(&st, &sm->set);	// as saved (docs/spec/settings-store.md 3)
#if CONFIG_DMR
	sm->dmr_killed = (st.t3_kill & SETTINGS_T3_KILLED) != 0;	// a kill holds in a build without trunking too
	sm->view.dmr.killed = sm->dmr_killed;
#endif
#if CONFIG_DMR_TRUNK
	t3m.killed = false;	// a start: the kill is the settings' (dmr_killed) from here on
#endif
#if CONFIG_DMR_ROAMING
	roam_cfg_t rc;
	roam_cfg_of(&sm->set, &rc);
	roam_init(&sm->roam, &rc);
	memcpy(sm->roam_zones, st.roam_zones, sizeof(sm->roam_zones));
	sm->roam_dirty = true;
#endif
	sm->view.tot_s = sm->set.tot_s;
	sm->activity_ms = now_ms;
	batt_init(&sm->batt);
#if CONFIG_BEEP
	beep_init(pcm_audio());
#endif
	step_update(sm);
	sm->view.source = codeplug_source();
	sm->codeplug_serial = codeplug_serial();
#if CONFIG_ZONE_WRITE
	sm->zone_shifts = zone_edit_shifts();
#endif

#if CONFIG_DUAL_WATCH
	vfo_defaults(&sm->vfo_ch);	// the second VFO first, then moved to vfo_other
	restore_vfo(sm, &st, 1);
	sm->vfo_other = (radio_vfo_t){ sm->vfo_ch, sm->vfo_seeded, sm->vfo_talk_id, sm->vfo_talk_private };
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
#endif
	vfo_defaults(&sm->vfo_ch);
	restore_vfo(sm, &st, 0);
#if CONFIG_DUAL_WATCH
	vfo_use(sm, st.vfo_index);
#endif

	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);

#if CONFIG_DMR
	sm->codec = board.vocoder != NULL && board.vocoder->present != NULL && board.vocoder->present();
	sm->view.dmr.codec = sm->codec;
	sm->hs_power = default_power();
	dmr_call_init(board.dmr);	// board.dmr NULL: DMR channels receive nothing and refuse PTT
	ambeVoiceInit(pcm_audio(), board.vocoder);
	dmr_call_set_voice(ambeVoiceSink(), ambeVoiceSource());
#if CONFIG_BEEP
	ambeVoiceSetMixer(beep_mix);	// beeps while the voice stream owns the PCM audio
#endif
#if CONFIG_DMR_DATA_CORE
	data_init(sm);
#endif
#endif
#if CONFIG_NXDN
	radio_nxdn_init();
#endif
#if CONFIG_P25
	radio_p25_init(pcm_audio());
#if CONFIG_IMBE && CONFIG_BEEP
	p25VoiceSetMixer(beep_mix);
#endif
#endif

	kv_load(sm, now_ms);

	// The saved zone and channel; else zone 0 and its first channel; if zone 0 has none, the first zone that has one
	select_position(sm, st.zone_index, st.channel, st.zone_pos);
	apply_prefs(sm, &st);
#if CONFIG_DMR
	sm->hs_power = default_power();
#endif
	if (!sm->have_channels || (st.vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");	// built without a VFO and nothing programmed: nothing to tune
		}
	} else {
		tune(sm, now_ms);
	}
	sm->rssi_published = sm->view.rssi_dbm;
#if CONFIG_APRS
	aprs_init(now_ms);
#endif
}


#if CONFIG_FM_TX_TONES
// The 1750 Hz burst: on keys up (FM channel, the usual TX checks) if not transmitting yet
static void tone_burst(radio_sm_t *sm, bool on, uint32_t max_ms, uint32_t now_ms)
{
	if (on) {
		if (sm->view.tone_burst) {
			return;
		}
		sm->burst_timed = sm->burst_held = sm->burst_key_owned = false;
		sm->burst_ms = sm->burst_end_ms = 0;
		if (sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot || board.radio->tx_tone == NULL) {
			beep(sm, BEEP_ERROR);
			return;
		}
		if (sm->view.state != RS_TX || !sm->ptt) {
			ptt_down(sm, now_ms);
			sm->burst_keyed = sm->ptt;	// held by the burst, even when refused (the deny shows until release)
			if (sm->view.state != RS_TX) {
				return;
			}
		}
		roger_cancel(sm);
		dtmf_cancel(sm, now_ms);
		if (tx_tone(RADIO_BURST_HZ)) {
			sm->view.tone_burst = true;
			sm->burst_end_ms = max_ms != 0 ? (now_ms + max_ms) | 1u : 0;	// | 1: never 0, "no limit"
			changed(sm);
		} else {
			beep(sm, BEEP_ERROR);
			if (sm->burst_keyed) {
				ptt_release(sm, false, now_ms);
			}
		}
		return;
	}
	sm->burst_timed = sm->burst_held = sm->burst_key_owned = false;
	sm->burst_ms = sm->burst_end_ms = 0;
	if (sm->view.tone_burst) {
		tx_tone(0);
		sm->view.tone_burst = false;
		changed(sm);
	}
	if (sm->burst_keyed) {
		sm->burst_keyed = false;
		ptt_release(sm, false, now_ms);
	}
}
#endif

#if CONFIG_FM_TX_TONES
// A channel with its tone burst on (channel_t.tone_burst): each PTT key-up opens with one timed burst, as the Tone
// burst key pressed during PTT does; the microphone follows. Nothing on a channel or board that can't send it
static void channel_burst(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->view.ch.tone_burst || sm->view.vfo || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot || sm->host_audio ||
	    board.radio->tx_tone == NULL || sm->view.state != RS_TX || sm->view.tone_burst) {
		return;
	}
	tone_burst(sm, true, 0, now_ms);
	sm->burst_timed = sm->view.tone_burst;
	sm->burst_held = false;
	sm->burst_ms = sm->burst_timed ? now_ms : 0;
}
#endif

// A request that changes what the receiver is tuned to ends a scan first (it stays where it is)
static bool retunes(uint8_t type)
{
	switch (type) {
	case RREQ_CH_STEP: case RREQ_ZONE_POS: case RREQ_ZONE: case RREQ_VFO: case RREQ_VFO_STEP: case RREQ_VFO_SET:
	case RREQ_VFO_TX: case RREQ_BANDWIDTH: case RREQ_TONE: case RREQ_MODE: case RREQ_DMR_TS: case RREQ_DMR_CC: case RREQ_VFO_COPY:
	case RREQ_VFO_SFR:
		return true;
	default:
		return false;
	}
}

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	sm->led_now = now_ms;
	// A beep or a vibration is an alert, not the user doing something; the volume changes nothing on air or on the
	// channel, so it neither gives way to nor cancels anything either
	bool alert = req->type == RREQ_BEEP || req->type == RREQ_VIBRATE || req->type == RREQ_VOLUME ||
		     req->type == RREQ_VOLUME_OVERRIDE || req->type == RREQ_VOLUME_HOLD;
	if (!alert && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE) {
		aprs_rf_abort(sm, now_ms);	// the user does something: an APRS transmission gives way
	}
	bool busy = sm->view.state == RS_TX || sm->hotspot;	// no retuning while transmitting or a hotspot
#if CONFIG_AIR_RAW
	busy = busy || sm->air_bench;				// ... or while the air interface bench has the modem
#endif
	int dir = req->arg > 0 ? 1 : -1;
	// (RREQ_DUAL packs a channel into arg, RADIO_DUAL_VFO in bit 31: INT32_MIN can't be negated, and isn't a step)
	int steps = req->arg > 0 ? req->arg : req->arg != INT32_MIN ? -req->arg : INT32_MAX;

	if (!alert) {
		activity(sm, now_ms);
	}
	if (!alert && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS && req->type != RREQ_ROAM) {
		roam_cancel(sm, now_ms);	// the user does something: back to the site a roaming search started from
	}
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	// The selected channel goes into a VFO the other channel isn't on: the one last used, else the other one. Chosen
	// while the pair is known, used once the stop has the receiver back on the selected channel
	bool to_vfo = req->type == RREQ_VFO_COPY || req->type == RREQ_VFO_SET || (req->type == RREQ_VFO && req->arg != 0);
	int vfo_pick = -1;
	if (to_vfo && dual_running(sm) && !sm->dual[sm->dual_sel].vfo) {
		const radio_dual_ref_t *o = &sm->dual[!sm->dual_sel];
		vfo_pick = o->vfo && o->pos == sm->vfo_idx ? !sm->vfo_idx : sm->vfo_idx;
	}
#endif
	if (retunes(req->type)) {
		scan_stop(sm, dual_running(sm), now_ms);	// dual watch: the change starts from the selected one
	}
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	if (vfo_pick >= 0) {
		vfo_use(sm, (uint8_t)vfo_pick);
	}
#endif
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	if (for_selected(req->type) && dual_running(sm)) {
		dual_to_selected(sm, now_ms);
	}
#endif
	switch (req->type) {
	case RREQ_CH_STEP:
		if (busy || steps == 0) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			if (sm->view.vfo) {
				vfo_step(sm, dir, now_ms);
			} else {
				channel_step(sm, dir, now_ms);
			}
		}
		break;
	case RREQ_VFO_STEP:
		if (busy || !sm->view.vfo) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			vfo_step(sm, dir, now_ms);
		}
		break;
	case RREQ_ZONE_POS: {
		uint16_t pos;
		channel_t ch;
		if (busy || !sm->have_channels || req->arg < 0 || req->arg >= sm->zone.count) {
			break;
		}
		pos = (uint16_t)req->arg;
		if (channel_read(sm->zone.channels[pos], &ch)) {
			set_channel(sm, pos, &ch);
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_ZONE: {
		zone_t z;
		uint16_t pos;
		channel_t ch;
		if (busy || req->arg < 0 || req->arg >= codeplug_zone_count()) {
			break;
		}
		if (read_zone((uint16_t)req->arg, &z) && find_channel(&z, 0, 1, &pos, &ch)) {
			set_zone(sm, (uint16_t)req->arg, &z);
			set_channel(sm, pos, &ch);
			sm->have_channels = true;
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_VFO:
		if (busy || !VFO_AVAILABLE || (req->arg != 0) == sm->view.vfo) {
			break;
		}
		if (req->arg != 0) {
			enter_vfo(sm, now_ms);
		} else {
			leave_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_COPY:
		if (!busy) {
			vfo_copy(sm, now_ms);
		}
		break;
	case RREQ_VFO_SET:
		if (busy || !VFO_AVAILABLE || req->arg <= 0 || !board_in_band((uint32_t)req->arg, false)) {
			break;
		}
		vfo_set_hz(sm, (uint32_t)req->arg);
		if (sm->view.vfo) {
			tune(sm, now_ms);
		} else {
			enter_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_TX: {
		// The receiver stays as it is: TX is set up at the next key-up. DMR works out repeater or direct from the
		// split, so the call stack is set up again (it publishes the view)
		uint32_t tx = req->arg > 0 ? (uint32_t)req->arg : sm->vfo_ch.rx_hz;
		if (busy || !sm->view.vfo || req->arg < 0 || !board_in_band(tx, true)) {
			break;
		}
		sm->vfo_ch.tx_hz = tx;
		sm->view.ch.tx_hz = tx;
#if CONFIG_DMR
		dmr_reconfigure_sfr(sm);
#else
		changed(sm);
#endif
		break;
	}
#if CONFIG_AM_RX
	case RREQ_VFO_STEP_SIZE:
		if ((req->arg == 0 || (req->arg >= 1000 && req->arg <= 1000000)) && (uint32_t)req->arg != sm->vfo_step_set) {
			sm->vfo_step_set = (uint32_t)req->arg;
			settings_t *st = settings_begin();
			st->vfo_step_hz = sm->vfo_step_set;
			settings_end(now_ms);
			changed(sm);
		}
		break;
#endif
	case RREQ_PTT:
#if CONFIG_FM_TX_TONES
		if (sm->burst_keyed) {
			sm->burst_keyed = req->arg == 0;	// PTT pressed during a burst takes the transmission over
			break;
		}
#endif
#if CONFIG_DMR_EMERGENCY
		if (sm->em_keyed) {
			sm->em_keyed = req->arg == 0;	// PTT pressed during the hot mic takes the transmission over
			break;
		}
#endif
#if CONFIG_VOX
		if (sm->vox_keyed) {
			if (req->arg != 0) {
				sm->vox_keyed = false;		// PTT takes a VOX transmission over
				sm->view.vox_keyed = false;
				changed(sm);
			}
			break;
		}
#endif
		if (req->arg != 0) {
			if (!sm->ptt) {
				sm->host_audio = req->arg == 2;	// a transmission keeps the source it started with
#if CONFIG_NET_USRP
				sm->host_pushes = audio_mux_host_pushes();
				sm->host_push_ms = now_ms;
#endif
			}
#if CONFIG_FM_TX_TONES
			bool was_tx = sm->view.state == RS_TX && !sm->view.tx_auto;	// the radio's own transmission is not the user's key-up
#endif
			ptt_down(sm, now_ms);
#if CONFIG_FM_TX_TONES
			if (!was_tx) {
				channel_burst(sm, now_ms);
			}
#endif
		} else {
			ptt_up(sm, now_ms);
		}
		break;
	case RREQ_MONITOR:
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
		// Before the DMR test: the receiver may be on the other channel, of the other mode
		if (req->arg == 1 && dual_running(sm)) {
			dual_to_selected(sm, now_ms);
		}
#endif
#if CONFIG_DMR
		// arg 1 down, 0 up, 2 up after the key's hold function ran. DMR: a short press toggles digital monitor (the
		// hear all latch) on its release, so a hold can do something else (#19); FM: the squelch is open while
		// the key is held. A key held across a change to DMR is still released here
		if (sm->view.ch.mode == RADIO_MODE_DMR && !sm->view.monitor) {
			if (req->arg == 0) {
				set_hear_all_latch(sm, !hear_all_on(sm));
			}
			break;
		}
#endif
		if (sm->view.monitor != (req->arg == 1)) {
			sm->view.monitor = req->arg == 1;
#if CONFIG_DMR
			if (sm->dmr_running) {
				dmr_configure(sm);	// DMR: hear all while held
			}
#endif
			outputs(sm);
			changed(sm);
		}
		break;
	case RREQ_POWER: {
		int n = board.power_count;
		if (n == 0 || steps == 0) {
			break;
		}
		int p = ((sm->view.power + req->arg) % n + n) % n;
		sm->view.power = (uint8_t)p;
		if (sm->view.vfo) {	// the VFO's own power, kept like its other settings (#486)
			sm->view.ch.power = sm->vfo_ch.power = (uint8_t)p;
		}
		sm->temp_power = sm->view.power != channel_power(&sm->view.ch);
		set_temporary(sm);
		changed(sm);
		break;
	}
	case RREQ_VOLUME:
		sm->vol_step = (uint8_t)(req->arg < 0 ? 0 : req->arg > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : req->arg);
		break;
	case RREQ_VOLUME_OVERRIDE:
		sm->vol_override = (int8_t)(req->arg < 0 ? -1 : req->arg > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : req->arg);
		break;
	case RREQ_VOLUME_HOLD:
		sm->vol_hold = req->arg != 0;
		break;
	case RREQ_SQUELCH: {
		int s = sm->view.squelch + req->arg;
		if (s < 0) {
			s = 0;
		} else if (s > RADIO_SQUELCH_MAX) {
			s = RADIO_SQUELCH_MAX;
		}
		if (s != sm->view.squelch) {
			sm->view.squelch = (uint8_t)s;
			if (sm->view.vfo) {	// squelch is radio-wide: in the VFO it becomes the default, as a channel save does
				sm->base_squelch = sm->view.squelch;
				sm->squelch_save = true;
			}
			sm->temp_squelch = sm->view.squelch != sm->base_squelch;
			set_temporary(sm);
			changed(sm);
		}
		break;
	}
	case RREQ_CLEAR_TEMP:
		if (sm->view.temporary) {
			if (clear_temporary(sm)) {
				if (busy) {
					sm->retune_pending = true;
				} else {
					tune(sm, now_ms);
				}
			}
			changed(sm);
		}
		break;
	case RREQ_RETUNE:	// a board that applies its reference trim when it tunes (hal_radio_t.ref_trim)
		sm->retune_due = true;	// radio_sm_poll: once the trim stops changing
		sm->retune_due_ms = now_ms + RADIO_RETUNE_SETTLE_MS;
		break;
	case RREQ_BANDWIDTH:
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || (req->arg != RADIO_BW_12K5 && req->arg != RADIO_BW_25K) ||
				req->arg == sm->view.ch.bw) {
			break;
		}
		sm->view.ch.bw = (uint8_t)req->arg;
		if (sm->view.vfo) {
			sm->vfo_ch.bw = sm->view.ch.bw;
		}
		set_temporary(sm);
		tune(sm, now_ms);
		break;
	case RREQ_TONE: {
		uint8_t which = (uint8_t)(((uint32_t)req->arg >> 24) & 3u);
		subtone_t t = { (uint8_t)(((uint32_t)req->arg >> 16) & 0xFFu), (uint16_t)req->arg };
		if (t.type == TONE_NONE) {
			t.value = 0;
		}
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || which == 0 || !tones_valid(&t)) {
			break;
		}
		bool rx_changed = (which & RADIO_TONE_RX) && !same_tone(&t, &sm->view.ch.rx_tone);
		bool tx_changed = (which & RADIO_TONE_TX) && !same_tone(&t, &sm->view.ch.tx_tone);
		if (which & RADIO_TONE_TX) {
			sm->view.ch.tx_tone = t;
		}
		if (which & RADIO_TONE_RX) {
			sm->view.ch.rx_tone = t;
		}
		if (sm->view.vfo) {
			sm->vfo_ch.tx_tone = sm->view.ch.tx_tone;
			sm->vfo_ch.rx_tone = sm->view.ch.rx_tone;
		}
		set_temporary(sm);
		if (rx_changed) {
			tune(sm, now_ms);	// the receiver's tone detector
		} else if (tx_changed) {
			changed(sm);		// used on the next PTT
		}
		break;
	}
#if CONFIG_DMR
	case RREQ_REPLY_CANCEL:
		dmr_call_reply_cancel();
		one_shot_end(sm);
		break;
	case RREQ_DMR_TARGET:
		dmr_set_manual(sm, (uint32_t)req->arg & DMR_ID_MAX, ((uint32_t)req->arg >> 24) & 1u, 0, true);
		break;
	case RREQ_DMR_CONTACT: {
		contact_t c;
		if (req->arg > 0 && req->arg <= 0xFFFF && codeplug_contact((uint16_t)req->arg, &c)) {
			dmr_set_manual(sm, c.id, c.type == CONTACT_PRIVATE, c.timeslot, true);
		}
		break;
	}
	case RREQ_DMR_CALL_BACK: {
		const dmr_call_view_t *cv = dmr_call_view();
		if (sm->dmr_running && cv->last_heard_ms != 0) {
			const dmr_caller_t *l = &cv->last_heard;
			dmr_set_manual(sm, l->group ? l->dst : l->src, !l->group, 0, false);
		}
		break;
	}
	case RREQ_DMR_TARGET_CLEAR:
		if (sm->manual || sm->one_shot) {
			sm->manual = false;
			sm->one_shot = false;
			if (sm->dmr_running) {
				dmr_configure(sm);
				sm->dmr_serial = dmr_call_view()->serial - 1u;
			}
			changed(sm);
		}
		break;
	case RREQ_DMR_TS:
		dmr_set_timeslot(sm, req->arg, busy);
		break;
	case RREQ_DMR_CC:
		if (busy || sm->view.ch.mode != RADIO_MODE_DMR || req->arg < 0 || req->arg > 15 ||
				req->arg == sm->view.ch.color_code) {
			break;
		}
		sm->view.ch.color_code = (uint8_t)req->arg;
		if (sm->view.vfo) {
			sm->vfo_ch.color_code = sm->view.ch.color_code;	// the VFO's own, not a temporary change
		}
		set_temporary(sm);
		dmr_reconfigure_sfr(sm);
		break;
#if CONFIG_SFR
	case RREQ_VFO_SFR: {
		// The VFO's SFR role (sfr.md 4): byte A as a channel's, taken as a channel's at a tune (a repeater role takes
		// the modem, a user role the inbound slot). Kept in DMR and FM alike: it acts only in DMR
		uint8_t a = (uint8_t)req->arg;
		if (busy || !sm->view.vfo || req->arg < 0 || req->arg > 0xFF || (a & 0x07u) >= SFR_ROLE_NUM_STORED) {
			break;
		}
		if (((a & 0x07u) != 0 ? a : 0) == sm->vfo_ch.sfr_a) {
			break;		// the same role again: a retune would only interrupt a running repeater
		}
		vfo_sfr_set(&sm->vfo_ch, a);
		vfo_sfr_set(&sm->view.ch, a);
		tune(sm, now_ms);
		break;
	}
#endif
	case RREQ_MODE:
		// Not on a channel of a kind: its mode is the kind's (channel-modes.md 6)
		if (busy || sm->view.ch.kind != CODEPLUG_KIND_NONE ||
				(req->arg != RADIO_MODE_FM && req->arg != RADIO_MODE_DMR && req->arg != RADIO_MODE_P25 &&
				 req->arg != RADIO_MODE_NXDN) ||
				req->arg == sm->view.ch.mode ||
				(req->arg == RADIO_MODE_FM && !(sm->view.vfo ? CONFIG_VFO_FM : CONFIG_FM)) ||
				(req->arg == RADIO_MODE_DMR && (radio_listen_band(sm->view.ch.rx_hz) ||
								(sm->view.vfo && !CONFIG_VFO_DMR))) ||
				(req->arg == RADIO_MODE_P25 && (!p25_ok() || radio_listen_band(sm->view.ch.rx_hz))) ||
				(req->arg == RADIO_MODE_NXDN && (!nxdn_ok() || radio_listen_band(sm->view.ch.rx_hz)))) {
			break;
		}
		sm->view.ch.mode = (uint8_t)req->arg;
		if (sm->view.ch.mode == RADIO_MODE_DMR && sm->view.ch.timeslot != 2) {
			sm->view.ch.timeslot = 1;
		}
		if (sm->view.vfo) {
			sm->vfo_ch.mode = sm->view.ch.mode;	// the VFO's own mode, not a temporary change
			sm->vfo_ch.timeslot = sm->view.ch.timeslot;
		}
		set_temporary(sm);
		tune(sm, now_ms);	// the call stack starts or stops with the mode
		break;
	case RREQ_DMR_HEAR_ALL:
		set_hear_all_latch(sm, req->arg != 0);
		break;
	case RREQ_DMR_FOCUS: {
		uint32_t id = (uint32_t)req->arg & DMR_ID_MAX;
		bool priv = (((uint32_t)req->arg >> 24) & 1u) != 0;
		if (id != sm->focus_id || priv != sm->focus_private) {
			sm->focus_id = id;
			sm->focus_private = id != 0 && priv;
			sm->view.dmr.focus_id = id;
			sm->view.dmr.focus_private = sm->focus_private;
			changed(sm);
		}
		break;
	}
#if CONFIG_DMR_PRIVACY
	case RREQ_DMR_PRIVACY: {
		uint8_t type = (uint8_t)(((uint32_t)req->arg >> 16) & 0xFFu);
		uint16_t key = (uint16_t)req->arg;
		if (type >= DMR_PRIVACY_NUM_TYPES || (type != DMR_PRIVACY_OFF && !dmrPrivacyKeyIsValid(type, key))) {
			break;
		}
		sm->privacy_type = type;
		sm->privacy_key = type != DMR_PRIVACY_OFF ? key : 0;
		if (sm->dmr_running) {
			dmr_configure(sm);
		}
		changed(sm);
		break;
	}
#endif
	case RREQ_HS_ENTER:
		hotspot_enter(sm, (uint32_t)req->arg, now_ms);
		break;
	case RREQ_HS_FREQ:
		hotspot_freq(sm, (uint32_t)req->arg, req->arg2);
		break;
	case RREQ_HS_POWER:
		sm->hs_power = radio_sm_hotspot_power((uint8_t)req->arg);
		hotspot_arm(sm);
		break;
	case RREQ_HS_LEAVE:
		hotspot_leave(sm, now_ms);
		break;
#if CONFIG_HOTSPOT
	case RREQ_HS_CWID:
		cw_start(sm, now_ms);
		break;
#endif
#endif
#if CONFIG_DMR_ROAMING
	case RREQ_ROAM:
		roam_request(sm, (uint8_t)req->arg, now_ms);
		break;
#endif
#if CONFIG_DMR_TRUNK
	case RREQ_T3:
		t3_request(sm, (uint8_t)req->arg, now_ms);
		break;
	case RREQ_T3_POLL:
		t3_poll(sm, req->arg2, now_ms);
		break;
	case RREQ_T3_DIAL:
#if CONFIG_DMR_TRUNK_GW
		t3_dial(req->arg, req->arg2);
#endif
		break;
#endif
	case RREQ_POWER_OFF:
		if (req->arg != 0) {
			// Still running after all (auto power off with the knob on): back to normal, registration kept
			sm->power_off_req = false;
			sm->power_off_busy = false;
#if CONFIG_DMR_DATA_CORE
			dmrDataServicePowerOffCanceled();
#endif
			break;
		}
#if CONFIG_FM_TX_TONES
		if (sm->view.tone_burst || sm->burst_keyed) {
#if CONFIG_VOX
			sm->vox_keyed = sm->view.vox_keyed = false;
#endif
			ptt_release(sm, false, now_ms);
			fm_tx_end(sm, false, now_ms);	// shutdown does not wait for a sub-audio tail
		}
#endif
		sm->power_off_req = true;
		sm->power_off_busy = CONFIG_DMR_DATA_CORE;	// data_tick clears it once the deregistration is out
		break;
	case RREQ_RELOAD:
#if CONFIG_POSITION
		settings_read(&st_copy);	// the task's scratch copy: a settings_t is too big for this frame
		position_load(&st_copy);	// the settings may have been replaced
#endif
#if CONFIG_APRS
		aprs_reload();
#endif
		if (busy || sm->ptt || ZONE_WRITE_BUSY()) {
			sm->reload_pending = true;	// after the transmission or the hotspot (radio_sm_poll)
		} else {
			reload(sm, now_ms);
		}
		break;
#if CONFIG_SCAN
	case RREQ_SCAN:
		scan_request(sm, req, now_ms);
		break;
#if CONFIG_DUAL_WATCH
	case RREQ_DUAL:
		dual_request(sm, req, now_ms);
		break;
#endif
	case RREQ_SCAN_VFO:
		if (!busy && !sm->ptt) {
			scan_start(sm, SCAN_VFO, (uint32_t)req->arg, req->arg2, now_ms);
		}
		break;
#endif
	case RREQ_BEEP:
		if (req->arg == BEEP_KEY && !sm->set.key_beep) {
			break;
		}
#if CONFIG_BEEP
		if (req->arg2 != 0) {	// later (a speaker test on a radio whose cable mutes the speaker): one at a time
			sm->beep_due = (uint8_t)req->arg;
			sm->beep_due_ms = now_ms + req->arg2;
			break;
		}
		sm->beep_due = BEEP_NONE;
#endif
		beep(sm, (uint8_t)req->arg);
		break;
#if CONFIG_VIBRATE
	case RREQ_VIBRATE:
		vibe_play((vibe_t)req->arg);	// dropped at the next poll on a radio without a motor
		break;
#endif
#if CONFIG_FM_TX_TONES
	case RREQ_TONE_BURST_KEY:
		if (req->arg != 0) {
			if (!sm->view.tone_burst) {
				tone_burst(sm, true, 0, now_ms);
				sm->burst_timed = sm->view.tone_burst;
				sm->burst_key_owned = sm->burst_timed || sm->burst_keyed;
				sm->burst_ms = sm->burst_timed ? now_ms : 0;
			}
			sm->burst_held = sm->burst_timed;
		} else if (sm->burst_key_owned) {
			sm->burst_held = false;
			if (!sm->burst_timed || now_ms - sm->burst_ms >= RADIO_BURST_MIN_MS) {
				tone_burst(sm, false, 0, now_ms);
			}
		}
		break;
	case RREQ_TONE_BURST:
		tone_burst(sm, req->arg != 0, req->arg2, now_ms);
		break;
#endif
#if CONFIG_DTMF
	case RREQ_DTMF:
		dtmf_key(sm, (char)(req->arg & 0xFF), (req->arg & 0x100) != 0, now_ms);
		break;
#endif
#if CONFIG_P25
	case RREQ_P25_SET:
		// radio_task.c copied the settings in: the engine takes them now, or when its transmission is done (air_tick)
		if (radio_p25_running()) {
			radio_p25_apply(&sm->view.ch, air_number(sm), p25_own_id(sm));
		}
		changed(sm);
		break;
#endif
#if CONFIG_NXDN
	case RREQ_NXDN_SET:
		// The same for NXDN; a new rate starts the engine over (radio_nxdn_apply)
		if (radio_nxdn_running()) {
			radio_nxdn_apply(&sm->view.ch, air_number(sm));
		} else if (sm->view.ch.mode == RADIO_MODE_NXDN && sm->board_mode == RADIO_MODE_DMR && !BENCH_HELD(sm) &&
			   radio_nxdn_fits()) {
			air_begin(sm);		// not running at the old rate: perhaps at the new one
		}
		radio_nxdn_poll(&sm->view.nxdn);	// the settings in the view now, running or not
		changed(sm);
		break;
#endif
#if CONFIG_AIR_RAW
	case RREQ_AIR_BENCH:
		if ((req->arg & 0xFF) != 0) {
			bench_enter(sm, now_ms);
		} else {
			bench_leave(sm, now_ms);
		}
		if ((req->arg >> 8) != 0) {
			sm->bench_seq = (uint8_t)(req->arg >> 8);
		}
		break;
	case RREQ_AIR_TX:
		if ((req->arg & 0xFF) != 0) {
			bench_key(sm, (uint16_t)(req->arg2 > 0xFFFFu ? 0xFFFFu : req->arg2), now_ms);
		} else {
			bench_unkey(sm);
		}
		if ((req->arg >> 8) != 0) {
			sm->bench_seq = (uint8_t)(req->arg >> 8);
		}
		break;
#endif
	default:
		break;
	}
	persist(sm, now_ms);
}

bool radio_volume_soft(void)
{
	return board.radio != NULL && board.radio->volume != NULL && board.power != NULL &&
	       board.power->volume != NULL && board.power->volume() < 0;
}

bool radio_volume_knob(void)
{
	return board.radio != NULL && board.radio->volume != NULL && board.power != NULL &&
	       board.power->volume != NULL && board.power->volume() >= 0;
}

bool radio_volume_settable(void)
{
	return board.radio != NULL && board.radio->volume != NULL;
}

uint8_t radio_volume_percent(uint8_t step)
{
	return (uint8_t)((step * 100u + RADIO_VOLUME_MAX / 2u) / RADIO_VOLUME_MAX);
}

// The stored byte: 0 = the default; bit 7 set = step + 1 on this scale; 1..11 = a step 0..10 stored by the first
// version, scaled to this range (docs/spec/settings-store.md 3)
uint8_t radio_volume_step(uint8_t stored)
{
	if (stored == 0) {
		return RADIO_VOLUME_DEFAULT;
	}
	if (stored & 0x80u) {
		unsigned n = stored & 0x7Fu;
		return (uint8_t)(n == 0 || n - 1u > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : n - 1u);
	}
	unsigned old = stored > 11 ? 10u : stored - 1u;
	return (uint8_t)((old * RADIO_VOLUME_MAX + 5u) / 10u);
}

uint8_t radio_beep_level(uint8_t cap, int16_t volume_percent)
{
	if (volume_percent < 0 || volume_percent >= cap) {
		return cap;
	}
	return (uint8_t)volume_percent;
}

uint8_t radio_volume_stored(uint8_t step)
{
	return (uint8_t)(0x80u | ((step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step) + 1u));
}

// Follow a volume knob the MCU reads (soft volume): pass changes of 2 % or more, and the ends exactly. A radio with
// no knob (volume() = -1) gets the stored step instead, and a knob radio the debug link's override
// (radio-features.md 4.2)
static void follow_volume(radio_sm_t *sm)
{
	if (board.radio->volume == NULL || board.power == NULL || board.power->volume == NULL) {
		return;
	}
	int v = board.power->volume();
	uint8_t src = RVOL_KNOB;
	if (v >= 0) {
		// The knob's own position, with the same 2 % filter as the level below, in force or not: the UI shows the
		// unlock hint when a held knob turns
		int dk = v - sm->knob_seen;
		if (sm->knob_seen < 0 || dk >= 2 || dk <= -2 || ((v == 0 || v == 100) && v != sm->knob_seen)) {
			sm->knob_seen = (int16_t)v;
			sm->view.volume_knob = (int8_t)v;
			changed(sm);
		}
	}
	if (v < 0) {
		v = radio_volume_percent(sm->vol_step);
		src = RVOL_SOFT;
	} else if (sm->vol_override >= 0) {
		v = radio_volume_percent((uint8_t)sm->vol_override);
		src = RVOL_OVERRIDE;
	}
	else if (sm->vol_hold && sm->volume_sent >= 0) {
		// Keys locked with the knob lock on: the level in force stays. Let go, the knob's level goes out at once
		v = sm->volume_sent;
		src = RVOL_HOLD;
	}
	if (src != sm->view.volume_src || sm->vol_step != sm->view.volume_step ||
	    sm->vol_override != sm->view.volume_override) {
		sm->view.volume_src = src;
		sm->view.volume_step = sm->vol_step;
		sm->view.volume_override = sm->vol_override;
		changed(sm);
	}
	int d = v - sm->volume_sent;
	if (sm->volume_sent < 0 || d >= 2 || d <= -2 || ((v == 0 || v == 100) && v != sm->volume_sent)) {
		sm->volume_sent = (int16_t)v;
		sm->view.volume = (int8_t)v;
		changed(sm);
		board.radio->volume((uint8_t)v);
	}
}

// Battery monitor: a reading a second, none during or just after a transmission. A radio that measures its supply
// instead gets the reading a second for display, and nothing acts on it: no thresholds are defined for a supply
static void battery_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx = sm->view.state == RS_TX;
#if CONFIG_DMR
	tx = tx || sm->dmr_keyed;
#endif
	if (tx) {
		sm->tx_end_ms = now_ms;
		return;
	}
	if (board_volts() == BOARD_VOLTS_SUPPLY) {
		if (now_ms - sm->batt_ms >= RADIO_BATT_PERIOD_MS) {
			sm->batt_ms = now_ms;
			sm->view.supply_mv = board.power->supply_mv();
		}
		return;
	}
	if (!board_has_battery() || board.power == NULL || board.power->battery_mv == NULL || now_ms - sm->batt_ms < RADIO_BATT_PERIOD_MS ||
			(sm->tx_end_ms != 0 && now_ms - sm->tx_end_ms < RADIO_BATT_TX_QUIET_MS)) {
		return;
	}
	sm->batt_ms = now_ms;
	uint16_t mv = board.power->battery_mv();
	uint8_t pct = board.power->battery_pct != NULL ? board.power->battery_pct() : 0;
	batt_event_t ev = batt_update(&sm->batt, mv, board.battery_low_mv, board.battery_critical_mv, now_ms);
	sm->view.battery_mv = mv;
	if (sm->view.battery != sm->batt.level || sm->view.battery_pct != pct) {
		sm->view.battery = sm->batt.level;
		sm->view.battery_pct = pct;
		changed(sm);
	}
	if (ev == BATT_EV_LOW) {
		beep(sm, BEEP_BATTERY_LOW);
	} else if (ev == BATT_EV_CRITICAL) {
		beep(sm, BEEP_BATTERY_CRIT);
	}
	bool off = sm->set.battery_off_s != 0 &&
		batt_critical_for(&sm->batt, now_ms) >= (uint32_t)sm->set.battery_off_s * 1000u;
	if (off != sm->view.power_off) {
		sm->view.power_off = off;
		changed(sm);
	}
}

#if CONFIG_BATTERY_SAVE
static bool save_allowed(const radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->set.rx_save == 0 || board.radio->rx_sleep == NULL || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
			sm->view.state != RS_IDLE || sm->view.monitor || sm->ptt || sm->view.ch.no_eco || sm->view.bcast) {
		return false;
	}
#if CONFIG_APRS_AFSK
	if (sm->aprs_phase != APRS_RF_OFF || aprs_rf_pending() || aprs_rx_listening()) {
		return false;	// a sleeping receiver misses packets
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
#if CONFIG_VOX
	if (sm->set.vox != RADIO_VOX_OFF) {
		return false;	// VOX listens to the microphone; keep the radio fully awake
	}
#endif
	return now_ms - sm->activity_ms >= (uint32_t)sm->set.rx_save_idle_s * 1000u;
}

// Receiver duty cycling while idle on an FM channel. True: skip this poll's status and squelch (asleep, or the
// readings right after waking)
static bool save_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!save_allowed(sm, now_ms)) {
		if (sm->save_on) {
			if (sm->save_asleep) {
				board.radio->rx_sleep(false);
			}
			sm->save_on = sm->save_asleep = false;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return false;
	}
	if (!sm->save_on) {
		sm->save_on = true;
		sm->save_asleep = false;
		sm->save_ms = now_ms;
	}
	uint32_t t = now_ms - sm->save_ms;
	if (sm->save_asleep) {
		uint32_t sleep_ms = (uint32_t)RADIO_SAVE_SLEEP_MS << (sm->set.rx_save - 1u);	// x1, x2, x4
		if (t >= sleep_ms) {
			board.radio->rx_sleep(false);
			sm->save_asleep = false;
			sm->save_ms = now_ms;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return true;
	}
	if (t < RADIO_SAVE_SETTLE_MS) {
		return true;
	}
	if (t >= RADIO_SAVE_WAKE_MS) {
		board.radio->rx_sleep(true);
		sm->save_asleep = true;
		sm->save_ms = now_ms;
		sm->view.rx_sleeping = true;
		changed(sm);
		return true;
	}
	return false;
}
#endif

// Polls of noise above the line before the squelch closes
static uint8_t close_polls(void)
{
#if CONFIG_APRS_RX
	if (aprs_rx_listening()) {
		return RADIO_SQL_CLOSE_POLLS_APRS;
	}
#endif
	return RADIO_SQL_CLOSE_POLLS;
}

#if CONFIG_VOX
static bool vox_enabled(const radio_sm_t *sm)
{
	if (sm->set.vox == RADIO_VOX_OFF || sm->set.vox_level == 0 || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
			board.power == NULL || board.power->mic_level == NULL) {
		return false;
	}
	return sm->set.vox == RADIO_VOX_ALL || sm->view.ch.vox;
}

// Key on voice, unkey vox_tail_ms after it stops. Never keyed by received audio (squelch open, monitor, just after)

static void vox_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->vox_keyed) {
		if (sm->ptt || !vox_enabled(sm)) {
			sm->vox_count = 0;
			return;
		}
		bool rx_audio = sm->view.state == RS_RX || sm->view.monitor ||
			(sm->view.last_rx_ms != 0 && now_ms - sm->view.last_rx_ms < RADIO_VOX_RX_GUARD_MS);
		uint8_t level = board.power->mic_level();
		if (rx_audio || level < 100u - 8u * sm->set.vox_level) {
			sm->vox_count = 0;
			return;
		}
		if (++sm->vox_count < RADIO_VOX_ATTACK_POLLS) {
			return;
		}
		sm->vox_count = 0;
		sm->vox_quiet_ms = 0;
		ptt_down(sm, now_ms);
		sm->vox_keyed = sm->ptt;
		sm->view.vox_keyed = sm->vox_keyed;
		changed(sm);
		return;
	}
#if CONFIG_FM_TX_TONES
	if (sm->view.tone_burst) {
		sm->vox_quiet_ms = 0;	// the quiet tail starts after the burst restores the microphone
		return;
	}
#endif
	uint8_t level = vox_enabled(sm) ? board.power->mic_level() : 0;
	if (level >= 100u - 8u * sm->set.vox_level) {
		sm->vox_quiet_ms = 0;
		return;
	}
	if (sm->vox_quiet_ms == 0) {
		sm->vox_quiet_ms = now_ms != 0 ? now_ms : 1;
	} else if (now_ms - sm->vox_quiet_ms >= sm->set.vox_tail_ms) {
		sm->vox_keyed = false;
		sm->view.vox_keyed = false;
		ptt_release(sm, true, now_ms);
		changed(sm);
	}
}
#endif

#if CONFIG_DMR
// The RSSI to publish for this poll's reading: the peak of the last RADIO_RSSI_HOLD_MS while the DMR path runs (the
// call stack, or an SFR repeater role: a reading in an empty slot, or one of the repeater's own, is the floor), the
// reading itself otherwise (FM, idle, hotspot). The squelch and scan keep using the raw reading.
static int16_t rssi_held(radio_sm_t *sm, int16_t rssi_dbm)
{
	if (!sm->dmr_running && !sm->sfr_rpt) {
		sm->rssi_ring_n = 0;
		return rssi_dbm;
	}
	sm->rssi_ring[sm->rssi_ring_pos] = rssi_dbm;
	sm->rssi_ring_pos = (uint8_t)((sm->rssi_ring_pos + 1u) % RADIO_RSSI_HOLD_POLLS);
	if (sm->rssi_ring_n < RADIO_RSSI_HOLD_POLLS) {
		sm->rssi_ring_n++;
	}
	int16_t peak = sm->rssi_ring[0];
	for (uint8_t i = 1; i < sm->rssi_ring_n; i++) {
		if (sm->rssi_ring[i] > peak) {
			peak = sm->rssi_ring[i];
		}
	}
	return sm->rssi_ring_n > 0 ? peak : rssi_dbm;
}
#else
static int16_t rssi_held(radio_sm_t *sm, int16_t rssi_dbm)
{
	(void)sm;
	return rssi_dbm;
}
#endif

void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)
{
	radio_status_t st;
	sm->led_now = now_ms;
	if (sm->led_timed || sm->deny_led || ptt_line_stuck()) {
		outputs(sm);	// as in radio_sm_tick: some paths below return before their outputs()
	}
	// codeplug_serial() before zone_edit_busy(): a zone write that moved indexes has its log entry by then
	if ((sm->reload_pending || codeplug_serial() != sm->codeplug_serial || ZONES_MOVED(sm)) && !ZONE_WRITE_BUSY() &&
	    !sm->hotspot && !BENCH_HELD(sm) && !sm->ptt && sm->view.state != RS_TX) {
		reload(sm, now_ms);
		persist(sm, now_ms);
	}
	follow_volume(sm);
	battery_poll(sm, now_ms);
#if CONFIG_BEEP
	bool shared = voice_streaming();
	if (sm->beep_due != BEEP_NONE && (int32_t)(now_ms - sm->beep_due_ms) >= 0) {
		uint8_t b = sm->beep_due;
		sm->beep_due = BEEP_NONE;
		beep(sm, b);
	}
	beep_service(shared, now_ms);
#endif
#if CONFIG_VIBRATE
	vibe_service(now_ms, board.power != NULL ? board.power->vibrate : NULL);
#endif
#if CONFIG_NET_USRP || CONFIG_APRS_RX
	audio_mux_service(now_ms);	// the stream for a host audio session or APRS (after the beep: it may have stopped it)
#endif
#if CONFIG_NET_USRP
	host_audio_watch(sm, now_ms);
#endif
#if CONFIG_APRS
	aprs_poll(sm, now_ms);	// before battery saving: beacons are due while the receiver sleeps too
#endif
	// The reference trim changed (RREQ_RETUNE): one tune once it has held still, so stepping it drops a reception once,
	// not at every step. It waits for an APRS job; while transmitting or a hotspot, for the end of it
	if (sm->retune_due && (int32_t)(now_ms - sm->retune_due_ms) >= 0) {
		bool aprs = false;
#if CONFIG_APRS_AFSK
		aprs = sm->aprs_phase != APRS_RF_OFF;
#endif
		if (sm->view.state == RS_TX || sm->hotspot || BENCH_HELD(sm)) {
			sm->retune_due = false;
			sm->retune_pending = true;
		} else if (!aprs) {
			tune(sm, now_ms);
		}
	}
#if CONFIG_BATTERY_SAVE
	if (save_poll(sm, now_ms)) {
		outputs(sm);
		return;
	}
#endif
	// An APRS job has the channel receiver on its own frequency in FM: its carrier sense reads that receiver as is
	bool own_rx = true;
#if CONFIG_APRS_AFSK
	own_rx = sm->aprs_phase == APRS_RF_OFF;
#endif
	(void)own_rx;
#if CONFIG_AIR_RAW
	if (sm->air_bench) {
		outputs(sm);
		return;		// the bench has the modem: no squelch, scan or time-out here (bench_tick caps a keying)
	}
#endif
#if CONFIG_BCAST_FM
	if (sm->view.bcast && own_rx)
		bcast_status(sm, &st);
	else
#endif
	board.radio->status(&st);
#if CONFIG_AM_RX
	if (sm->view.am && own_rx) {
		st.tone_ok = true;	// no sub-audio in AM; the board gives the AM squelch value (hal_radio.h rx_am)
	}
#endif
	sm->view.rssi_dbm = rssi_held(sm, st.rssi_dbm);
	sm->view.noise = st.noise;
#if CONFIG_DMR
	// The modem may not report a level per burst: then the call's level is the held one while a call is heard. A
	// finished call keeps its last value (dmr_publish leaves it alone once the stack's own is 0 and the state is idle).
	if (sm->view.dmr.state == RDMR_RX || sm->view.dmr.state == RDMR_RX_OTHER) {
		const dmr_call_view_t *cv = dmr_call_view();
		int16_t level = cv->rssi_dbm != 0 ? cv->rssi_dbm : sm->view.rssi_dbm;
		int move = level - sm->view.dmr.rssi_dbm;
		if (sm->view.dmr.rssi_dbm == 0 || move >= RADIO_RSSI_DEADBAND_DB || move <= -RADIO_RSSI_DEADBAND_DB) {
			sm->view.dmr.rssi_dbm = level;
			changed(sm);
		}
	}
#endif
	int drift = sm->view.rssi_dbm - sm->rssi_published;
	if (drift >= RADIO_RSSI_DEADBAND_DB || drift <= -RADIO_RSSI_DEADBAND_DB) {
		sm->rssi_published = sm->view.rssi_dbm;
		changed(sm);
	}
#if CONFIG_APRS_AFSK
	if (sm->aprs_phase != APRS_RF_OFF) {
		aprs_rf_service(sm, &st, now_ms);	// on the APRS frequency: no squelch, scan or time-out here
		outputs(sm);
		return;
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan) && !sm->ptt && sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED && !sm->hotspot) {
		uint32_t steps = sm->scan.steps;
		scan_service(sm, &st, now_ms);
		if (sm->scan.steps != steps) {
			outputs(sm);
			return;		// retuned: this poll's readings belong to the last stop
		}
	}
#if CONFIG_DUAL_WATCH
	else if (dual_running(sm) && !sm->hotspot) {
		scan_talked(&sm->scan, now_ms);	// transmitting: the wait after it starts when it ends
	}
#endif
#endif
#if CONFIG_VOX
	vox_poll(sm, now_ms);
	if (vox_enabled(sm) != sm->view.vox_on) {
		sm->view.vox_on = !sm->view.vox_on;
		changed(sm);
	}
#endif
#if CONFIG_DMR
	if (sm->hotspot) {
		// Carrier for the host's listen before talk: the noise reading against the squelch level's threshold
		uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
		sm->hs_carrier = st.noise < radio_sm_squelch_open_below(sq);
		outputs(sm);
		return;
	}
#if CONFIG_DMR_ROAMING
	roam_service(sm, &st, now_ms);
#endif
	if (sm->dmr_running) {
		return;		// the state follows the call stack (radio_sm_tick), and the stack keeps its own TX time-out
	}
#if CONFIG_SFR
	if (sm->sfr_rpt) {
		return;		// the state follows the repeater (sfr_rpt_tick)
	}
#endif
#endif
#if CONFIG_AIR
	if (air_running() || air_mode(sm->view.ch.mode)) {
		air_poll_tx(sm, now_ms);
		outputs(sm);
		return;		// the state follows the engine (air_tick)
	}
#endif

	if (sm->view.state == RS_TX) {
#if CONFIG_FM_TX_TONES
		if (sm->burst_end_ms != 0 && sm->view.tone_burst && (int32_t)(now_ms - sm->burst_end_ms) >= 0) {
			tone_burst(sm, false, 0, now_ms);	// its "off" never came: unkey as the off would
			if (sm->view.state != RS_TX) {
				return;
			}
		}
		if (sm->roger_ms != 0) {
			roger_poll(sm, now_ms);
			if (sm->view.state != RS_TX) {
				return;
			}
		}
#endif
#if CONFIG_SUBTONE
		if (sm->tail_ms != 0) {
			tail_poll(sm, now_ms);
			if (sm->view.state != RS_TX) {
				return;
			}
		}
#endif
#if CONFIG_DTMF
		dtmf_poll(sm, now_ms);	// PTT is up during the tail: no digit sounds over it
#endif
		tx_timer(sm, now_ms);
		if (sm->view.tot_s != 0 && sm->view.tx_elapsed_ms >= (uint32_t)sm->view.tot_s * 1000) {
			dtmf_cancel(sm, now_ms);
#if CONFIG_FM_TX_TONES
			// Keep side-key ownership so its release clears the time-out denial.
			sm->burst_timed = sm->burst_held = false;
			sm->burst_ms = sm->burst_end_ms = 0;
			if (sm->roger_ms != 0 || sm->view.tone_burst) {
				tx_tone(0);
				sm->roger_ms = 0;
#if CONFIG_SIG_TONES
				sm->roger_lead = false;
#endif
				sm->view.roger = false;
				sm->view.tone_burst = false;
			}
#endif
#if CONFIG_SUBTONE
			tail_cancel(sm);
#endif
			board.radio->tx_off();
			deny(sm, TXDENY_TIMEOUT);	// until PTT is released
			if (!sm->ptt) {
				fm_tx_end(sm, true, now_ms);	// the roger beep ran into the time-out: nothing holds the deny
			}
		}
		return;
	}
	if (sm->view.state == RS_TX_DENIED && !sm->deny_rx) {
		return;
	}

	// Open on carrier (noise below the level's threshold) and the RX tone, both; the monitor overrides (outputs())
	follow_tone(sm, st.tone_ok);
	uint8_t sq = sm->view.squelch;
	uint8_t open_below = radio_sm_squelch_open_below(sq);
	bool listening = bcast_listening(sm);
	bool opens = (listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below))) && !dual_peeking(sm) &&
		     !parked(&sm->view.ch);
	bool holds = listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB));

	if (sm->view.state == RS_IDLE) {
		if (opens) {
			sm->view.state = RS_RX;
			sm->close_count = 0;
			changed(sm);
		}
	} else if (holds) {
		activity(sm, now_ms);
		sm->close_count = 0;
	} else if (++sm->close_count >= close_polls() || (sq != 0 && close_polls() == RADIO_SQL_CLOSE_POLLS &&
			   st.noise >= open_below + RADIO_SQL_HYST_DB + RADIO_SQL_FAST_CLOSE_DB)) {
		bool was_rx = sm->view.state == RS_RX;
		if (sm->view.state == RS_TX_DENIED) {
			// A busy refusal over a reception (deny_rx): its audio stops where the squelch closes; the refusal
			// stays until PTT is released
			sm->deny_rx = false;
			sm->close_count = 0;
		} else {
			end_reception(sm, now_ms);
		}
		changed(sm);
		// The channel is free again: the idle tone (a scan landing or a broadcast listen has none)
		// (not within a second of our own transmission: the end tone just played and the squelch tail is not a call)
		if (was_rx && !listening && sm->view.ch.mode == RADIO_MODE_FM && now_ms - sm->tx_end_ms > 1000u &&
		    sig_local_on(sm, SIG_EV_IDLE)) {
			sig_local(sm, SIG_EV_IDLE);
		}
	}
	outputs(sm);
}

#if CONFIG_SFR
// The repeater role: its runner, the state shown (receiving while it repeats a call), and back to an ordinary channel
// once the radio may not transmit any more (TX inhibit set, the battery critical)
static void sfr_rpt_tick(radio_sm_t *sm, uint32_t now_ms)
{
	radio_sfr_view_t v;

#if CONFIG_SFR_CWID
	if (sm->cw_phase != 0) {
		return;		// the ID is being keyed (cw_service); the role starts again after it
	}
#endif
	if (sfr_rpt_wait(sm)) {
		sfr_rpt_end(sm);
		dmr_begin(sm);
		changed(sm);
		return;
	}
	radio_sfr_poll(now_ms);
	radio_sfr_get_view(&v);
#if CONFIG_SFR_CWID
	if (radio_cwid_due(&sm->cwid, radio_cwid_interval_ms(sm->set.sfr_cwid), v.keyed || v.inbound_busy, now_ms)) {
		cw_start_sfr(sm, now_ms);
		return;
	}
#endif
	uint8_t state = v.inbound_busy ? RS_RX : RS_IDLE;
	if (v.keyed != sm->view.dmr.sfr_keyed || v.inbound_busy != sm->view.dmr.sfr_busy || state != sm->view.state) {
		sm->view.dmr.sfr_keyed = v.keyed;
		sm->view.dmr.sfr_busy = v.inbound_busy;
		if (state == RS_IDLE && sm->view.state == RS_RX) {
			end_reception(sm, now_ms);
		}
		sm->view.state = state;
		changed(sm);
	}
	outputs(sm);
}
#endif

void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)
{
	sm->led_now = now_ms;
	if (codeplug_kv_serial() != kv_seen) {
		kv_load(sm, now_ms);	// a key written (menu, debug link): its value only, without a codeplug reload
	}
#if CONFIG_FM_TX_TONES
	if (sm->burst_timed && !sm->burst_held && now_ms - sm->burst_ms >= RADIO_BURST_MIN_MS) {
		tone_burst(sm, false, 0, now_ms);
	}
#endif
#if RADIO_SM_CW
	cw_service(sm, now_ms);
#endif
#if CONFIG_DMR
	if (sm->dmr_running) {
		dmr_call_poll(now_ms);
		dmr_follow(sm, now_ms);
		one_shot_follow(sm, now_ms);
	}
#if CONFIG_SFR
	if (sm->sfr_rpt) {
		sfr_rpt_tick(sm, now_ms);
	} else if (sm->sfr_denied && sm->dmr_running && !sm->ptt && sm->view.state == RS_IDLE && !sfr_rpt_wait(sm)) {
		dmr_begin(sm);		// the repeater role back, between calls
		changed(sm);
	}
#endif
#if CONFIG_DMR_DATA_CORE
	data_tick(sm, now_ms);	// also off DMR: requests fail at once, ARS keeps its state. A list it starts is queued
				// to the modem at once (dmr_call_data_tx)
#endif
#if CONFIG_DMR_EMERGENCY
	emergency_tick(sm, now_ms);
#endif
#if CONFIG_DMR_TRUNK
	t3_tick(sm, now_ms);
#endif
#endif
#if CONFIG_AIR
	if (air_running()) {
		air_tick(sm, now_ms);
	}
#endif
#if CONFIG_AIR_RAW
	if (sm->bench_keyed) {
		bench_tick(sm, now_ms);
	}
#endif
	if (sm->led_timed || sm->deny_led || ptt_line_stuck()) {
		outputs(sm);	// a blink or the deny flash runs on time, not only when the state changes
	}
}
