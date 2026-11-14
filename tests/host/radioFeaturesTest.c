// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's everyday features (docs/spec/radio-features.md): the scan, beep and battery modules on their own,
// then scan, TX time-out warning, busy channel lockout, roger beep, 1750 Hz burst, VOX, battery monitor, receiver
// battery saving and the DMR beeps through radio_sm.c, against a fake board whose receiver hears signals per frequency
#include <stdio.h>
#include <math.h>
#include <string.h>
// The caps change at run time here (a radio without a battery): defined writable, the core sees it const
#define board board_as_declared
#include "board.h"
#undef board
#include "radio/radio_sm.h"
#include "radio/scan.h"
#include "radio/beep.h"
#include "radio/vibe.h"
#include "usbaudio/audio_mux.h"
#include "radio/battery.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "settings/settings.h"
#include "radio/tx_limit.h"
#include "codeplug/codeplug_kv.h"	// the settings area fakes
#include "radio/radio_keys.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake board: signals on frequencies ----
#define MAX_SIG 4
typedef struct {
	uint32_t hz;
	uint8_t noise;
	subtone_t tone;		// the sub-audio the transmitter sends
	bool tdma;		// DMR direct mode: on air every other 30 ms slot (the noise reading drops between bursts)
} sig_t;

static uint32_t now;

static struct {
	uint32_t rx_hz;
	uint32_t rx_ms;		// when rx() last tuned
	subtone_t rx_tone;
	int rx_calls;
	int tx_calls;
	uint32_t tx_hz;
	int tx_off_calls;
	bool audio;
	bool led[3];
	uint16_t tone_hz;	// tx_tone
	int tone_calls;
	uint16_t tone_log[16];
	bool asleep;
	int sleeps, wakes;
	int status_asleep;	// status() calls while the receiver sleeps (must stay 0)
	sig_t sig[MAX_SIG];
	uint16_t battery_mv;
	uint8_t mic;
	bool has_tx_tone;
	bool has_tail;		// tx_tail starts one (the board's TX tone is DCS)
	bool tail_on;
	int tail_starts;
	int vibes;		// vibrate() calls
	uint16_t vibe_ms[8];	// their lengths
	uint32_t vibe_at[8];	// and when
	char dtmf;		// tx_dtmf: the digit sounding
	int dtmf_calls;
	char dtmf_log[32];	// the digits started, in order
	uint32_t dtmf_on[32];	// when each started
	uint32_t dtmf_off[32];	// and ended (tx_dtmf(0))
	int dtmf_n;
	int mixed;		// a DTMF digit and the sub-audio tail on the air together
} hw;

static bool same_tone(subtone_t a, subtone_t b)
{
	return a.type == b.type && (a.type == TONE_NONE || a.value == b.value);
}

static void r_init(void) {}
static void r_set_mode(radio_mode_t m) { (void)m; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	(void)bw;
	hw.rx_hz = hz;
	hw.rx_ms = now;
	hw.rx_tone = *t;
	hw.rx_calls++;
	hw.asleep = false;
	return true;
}
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	(void)bw;
	(void)p;
	(void)t;
	hw.tx_calls++;
	hw.tx_hz = hz;
	return true;
}
static void r_tx_off(void) { hw.tx_off_calls++; hw.tone_hz = 0; hw.tail_on = false; }
static bool r_tx_tail(bool on)
{
	if (on && !hw.has_tail) {
		return false;
	}
	hw.tail_starts += on;
	hw.tail_on = on;
	hw.mixed += on && hw.dtmf != 0;
	return true;
}
static void r_status(radio_status_t *st)
{
	if (hw.asleep) {
		hw.status_asleep++;
	}
	st->noise = 120;
	st->rssi_dbm = -125;
	st->carrier = false;
	st->tone_ok = hw.rx_tone.type == TONE_NONE;
	for (int i = 0; i < MAX_SIG; i++) {
		if (hw.sig[i].hz != 0 && hw.sig[i].hz == hw.rx_hz && !(hw.sig[i].tdma && (now / 30u) % 2u != 0)) {
			st->noise = hw.sig[i].noise;
			st->rssi_dbm = -80;
			st->tone_ok = hw.rx_tone.type == TONE_NONE || same_tone(hw.rx_tone, hw.sig[i].tone);
		}
	}
}
static void r_audio(bool on) { hw.audio = on; }
static bool r_tx_tone(uint16_t hz)
{
	if (!hw.has_tx_tone) {
		return false;
	}
	hw.tone_hz = hz;
	if (hw.tone_calls < 16) {
		hw.tone_log[hw.tone_calls] = hz;
	}
	hw.tone_calls++;
	return true;
}
static bool r_tx_dtmf(char digit)
{
	hw.dtmf_calls++;
	hw.mixed += digit != 0 && hw.tail_on;
	if (digit == 0) {
		if (hw.dtmf != 0 && hw.dtmf_n > 0) {
			hw.dtmf_off[hw.dtmf_n - 1] = now;
		}
	} else if (hw.dtmf_n < 32) {
		hw.dtmf_log[hw.dtmf_n] = digit;
		hw.dtmf_on[hw.dtmf_n++] = now;
	}
	hw.dtmf = digit;
	return true;
}
static void r_rx_sleep(bool sleep)
{
	hw.asleep = sleep;
	if (sleep) {
		hw.sleeps++;
	} else {
		hw.wakes++;
	}
}
static const hal_radio_t fake_radio = {
	.init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off, .status = r_status,
	.audio = r_audio, .tx_tone = r_tx_tone, .tx_dtmf = r_tx_dtmf, .rx_sleep = r_rx_sleep, .tx_tail = r_tx_tail,
};
static const hal_radio_t fake_radio_no_dtmf = {
	.init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off, .status = r_status,
	.audio = r_audio, .tx_tone = r_tx_tone, .rx_sleep = r_rx_sleep, .tx_tail = r_tx_tail,
};

static void p_led(uint8_t which, bool on) { hw.led[which] = on; }
static uint16_t p_battery_mv(void) { return hw.battery_mv; }
static uint8_t p_battery_pct(void) { return hw.battery_mv >= 7400 ? 50 : 5; }
static uint8_t p_mic(void) { return hw.mic; }
static void p_vibrate(uint16_t ms)
{
	if (hw.vibes < 8) {
		hw.vibe_ms[hw.vibes] = ms;
		hw.vibe_at[hw.vibes] = now;
	}
	hw.vibes++;
}
static const hal_power_t fake_power = {
	.led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct, .mic_level = p_mic,
	.vibrate = p_vibrate,
};
// A mobile that measures its DC input (supply_mv) and has no battery; the battery hooks stay to show they aren't read
static uint16_t p_supply_mv(void) { return hw.battery_mv; }
static const hal_power_t fake_supply_power = {
	.led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct, .mic_level = p_mic,
	.vibrate = p_vibrate, .supply_mv = p_supply_mv,
};

static struct {
	int starts, stops;
	bool streaming;
	bool pcm;
	audio_block_cb_t cb;
} aud;
static bool a_start_fails;	// the codec clock check fails after the callback already ran once
static bool a_start(audio_block_cb_t cb, void *ctx)
{
	aud.starts++;
	if (a_start_fails) {
		int16_t m[AUDIO_BLOCK_SAMPLES] = { 0 }, s[AUDIO_BLOCK_SAMPLES];
		cb(m, s, ctx);
		return false;
	}
	aud.cb = cb;
	aud.streaming = true;
	return true;
}
static void a_stop(void) { aud.streaming = false; aud.cb = NULL; aud.stops++; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm };

// The amplifier hang hook (hal_audio_t.speaker_warm)
static int warm_calls;
static bool warm_state;
static void a_warm(bool on)
{
	warm_calls++;
	warm_state = on;
}

// FM transmit audio from the MCU (hal_audio_tap_t.fm_tx_inject): what the radio core asked for
static struct {
	bool ok;		// the board accepts
	int calls;
	audio_tap_pull_t pull;	// the source it was given (NULL: the microphone)
} tap_hw;
static bool t_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
	(void)mode;
	tap_hw.calls++;
	if (!tap_hw.ok) {
		return false;
	}
	tap_hw.pull = pull;
	return true;
}
static const hal_audio_tap_t fake_tap = { .caps = AUDIO_TAP_FM_RX | AUDIO_TAP_FM_TX, .fm_tx_inject = t_inject };

static bool v_present(void) { return true; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode, NULL, NULL, NULL, NULL };

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};
static const char *const power_names[] = { "L", "M", "H" };

board_t board = {
	.name = "fake",
	.bands = bands,
	.band_count = 2,
	.power_count = 3,
	.power_names = power_names,
	.power_default = 1,
	.radio = &fake_radio,
	.power = &fake_power,
	.dmr = &sim_modem,
	.audio = &fake_audio,
	.audio_tap = &fake_tap,
	.vocoder = &fake_vocoder,
	.battery_low_mv = 6400,
	.battery_critical_mv = 6000,
};

// ---- fake codeplug: one zone of 7 channels ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zone;

codeplug_source_t codeplug_source(void) { return CODEPLUG_OPENFW; }
uint32_t codeplug_serial(void) { return 1; }
// The settings store runs from RAM here (no flash map); its locks are no-ops
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint16_t codeplug_channel_count(void) { return NCH; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n < 1 || n > NCH || !used[n]) {
		return false;
	}
	*out = chans[n];
	return true;
}
static zone_t zone2;		// dual watch: a second zone when nzones is 2
static uint16_t nzones;
uint16_t codeplug_zone_count(void) { return nzones; }
bool codeplug_zones_stored(void) { return true; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i >= nzones) {
		return false;
	}
	*out = i == 0 ? zone : zone2;
	return true;
}
static const contact_t contacts[] = { { "Local", 9, CONTACT_GROUP, 0 } };
uint32_t codeplug_dmr_id(void) { return 9996543; }
// The settings area holds only the Settings > Sound tone keys here: one-byte bools, -1 absent (kv_tone_set)
static int16_t kv_tone[CODEPLUG_KEY_TX_DENY_TONE - CODEPLUG_KEY_CALL_END_TONE + 1] = { -1, -1, -1, -1 };
static uint8_t kv_serial;
int codeplug_kv_get(uint16_t key, void *buf, uint16_t max)
{
	if (key < CODEPLUG_KEY_CALL_END_TONE || key > CODEPLUG_KEY_TX_DENY_TONE || kv_tone[key - CODEPLUG_KEY_CALL_END_TONE] < 0) {
		return -1;
	}
	if (max > 0) {
		*(uint8_t *)buf = (uint8_t)kv_tone[key - CODEPLUG_KEY_CALL_END_TONE];
	}
	return 1;
}
bool codeplug_kv_write(const codeplug_kv_op_t *ops, uint8_t n) { (void)ops; (void)n; return false; }
static void kv_tone_set(uint16_t key, int16_t v)
{
	kv_tone[key - CODEPLUG_KEY_CALL_END_TONE] = v;
	kv_serial++;	// as codeplug_kv_write: the radio core reads the keys again
}
void codeplug_kv_begin(codeplug_kv_cursor_t *c, uint16_t lo, uint16_t hi)
{
	c->at = lo < CODEPLUG_KEY_CALL_END_TONE ? CODEPLUG_KEY_CALL_END_TONE : lo;
	c->hi = hi > CODEPLUG_KEY_TX_DENY_TONE ? CODEPLUG_KEY_TX_DENY_TONE : hi;
}
int codeplug_kv_next(codeplug_kv_cursor_t *c, uint16_t *key, void *buf, uint16_t max)
{
	for (; c->at <= c->hi; c->at++) {
		int16_t v = kv_tone[c->at - CODEPLUG_KEY_CALL_END_TONE];
		if (v >= 0) {
			*key = c->at++;
			if (max > 0) {
				*(uint8_t *)buf = (uint8_t)v;
			}
			return 1;
		}
	}
	return -1;
}
uint8_t codeplug_kv_serial(void) { return kv_serial; }
bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (index != 1) {
		return false;
	}
	*out = contacts[0];
	return true;
}
bool codeplug_listen_list(uint16_t index, listen_list_t *out) { (void)index; (void)out; return false; }
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	if (group && id == 9) {
		*out = contacts[0];
		return true;
	}
	return false;
}
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	*out = (codeplug_dmr_settings_t){ "Q0TEST", 0, 0 };
	return true;
}
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len) { (void)id; (void)out; (void)len; return false; }

static void add(uint16_t n, const char *name, uint32_t hz, uint8_t mode)
{
	memset(&chans[n], 0, sizeof(chans[n]));
	strcpy(chans[n].name, name);
	chans[n].rx_hz = chans[n].tx_hz = hz;
	chans[n].mode = mode;
	chans[n].power = CODEPLUG_POWER_DEFAULT;
	chans[n].color_code = 1;
	chans[n].timeslot = 1;
	used[n] = true;
}

// Zone positions: 0 ch1 Home 146.520, 1 ch2 Tone 146.940 (CTCSS 100.0 both ways), 2 ch3 Skip 147.000 (scan skip),
// 3 (empty slot 4), 4 ch5 Other 145.500, 5 ch6 DMR 438.500 (CC 1, TS 1, TG 9), 6 ch7 Last 146.580 (TOT 30 s)
static void setup_codeplug(void)
{
	memset(used, 0, sizeof(used));
	add(1, "Home", 146520000, RADIO_MODE_FM);
	add(2, "Tone", 146940000, RADIO_MODE_FM);
	chans[2].rx_tone = chans[2].tx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	add(3, "Skip", 147000000, RADIO_MODE_FM);
	chans[3].scan_skip = true;
	add(5, "Other", 145500000, RADIO_MODE_FM);
	add(6, "DMR", 438500000, RADIO_MODE_DMR);
	chans[6].contact = 1;
	add(7, "Last", 146580000, RADIO_MODE_FM);
	chans[7].tot_s = 30;
	nzones = 1;
	memset(&zone, 0, sizeof(zone));
	strcpy(zone.name, "Z");
	zone.count = 7;
	for (int i = 0; i < 7; i++) {
		zone.channels[i] = (uint16_t)(i + 1);
	}
}

static radio_sm_t sm;

static void req2(uint8_t type, int32_t arg, uint32_t arg2)
{
	radio_req_t r = { type, arg, arg2 };
	radio_sm_request(&sm, &r, now);
}
static void req(uint8_t type, int32_t arg) { req2(type, arg, 0); }

// 5 ms steps: modem slots every 30 ms, the stack tick every 10 ms, the status poll every 25 ms; and the audio
// callback every 20 ms while a stream runs (so beeps play out and the own stream stops)
static int16_t spk[AUDIO_BLOCK_SAMPLES];
static int16_t mic[AUDIO_BLOCK_SAMPLES];
static void air_burst(void);
static void kept_timing(void);
static struct {
	bool on;		// a DMR voice call in progress (no header: it started before the radio came by)
	uint32_t hz;
	uint32_t t0;		// burst A of a superframe was on air then (bursts every 60 ms)
	uint32_t dst;		// the talkgroup
	uint8_t lc_at;		// the modem hands over the embedded LC at the F of this superframe after it joined (1:
				// the first complete one; 2: the next, as with a sticky LC equal to this call's)
	bool joined;
	uint32_t join_rx_ms;	// the tune it joined on
	uint8_t sf;		// superframes delivered since it joined
} air;

static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (now % 30 == 0) {
			sim_slot();
		}
		kept_timing();
		if (air.on && now >= air.t0 && (now - air.t0) % 60u == 0) {
			air_burst();
		}
		if (now % RADIO_TICK_MS == 0) {
			radio_sm_tick(&sm, now);
		}
		if (now % RADIO_POLL_MS == 0) {
			radio_sm_poll(&sm, now);
		}
		if (now % 20 == 0 && aud.streaming && aud.cb != NULL) {
			aud.cb(mic, spk, NULL);
		}
	}
}

static void signal_on(int i, uint32_t hz, uint8_t noise, subtone_t tone)
{
	hw.sig[i].hz = hz;
	hw.sig[i].noise = noise;
	hw.sig[i].tone = tone;
}

static const subtone_t no_tone = { TONE_NONE, 0 };

static void start(void)
{
	memset(&air, 0, sizeof(air));
	ambeVoiceStop();
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	hw.battery_mv = 8000;
	hw.has_tx_tone = true;
	beep_reset_stats();
	setup_codeplug();
	now = 1000;
	settings_init();	// no flash: the defaults again (the last test's settings were "saved" in RAM)
	radio_sm_init(&sm, now);
	run(100);
}

static void settings(void (*edit)(radio_settings_t *))
{
	radio_settings_t s = sm.set;
	edit(&s);
	radio_sm_set_settings(&sm, &s, now);
}

static void set_resume_time(radio_settings_t *s);
static void set_resume_stop(radio_settings_t *s);
static void set_resume_carrier(radio_settings_t *s);
static void set_priority(radio_settings_t *s);
static void set_tot(radio_settings_t *s);

// ---- the scan engine on its own ----

static bool all_usable(uint16_t pos, void *ctx) { (void)ctx; return pos != 3; }

static void test_scan_engine(void)
{
	scan_t s;
	memset(&s, 0, sizeof(s));
	scan_cfg_t cfg = { .dwell_ms = 150, .probe_ms = 300, .hold_ms = 5000, .delay_ms = 2000,
			   .resume = SCAN_RESUME_CARRIER };
	s.origin_pos = 1;
	scan_begin(&s, SCAN_ZONE, &cfg, 0);
	CHECK(scan_on(&s) && s.phase == SCAN_PH_LISTEN && s.sweep_pos == 1);

	// Positions: 3 is unusable; the sweep wraps
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 2);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 4);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 5);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 0);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 1);
	CHECK(scan_nuisance_pos(&s, 2) && !scan_nuisance_pos(&s, 2) && s.skipped == 1);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 4);

	// Timing: settle, dwell, nothing heard -> next
	scan_tuned(&s, 1000);
	CHECK(scan_poll(&s, 1025, true, true, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_LISTEN);	// settling
	CHECK(scan_poll(&s, 1100, false, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 1150, false, false, false) == SCAN_ACT_NEXT);
	// A carrier at the end of the dwell: probe, then give up after probe_ms
	scan_tuned(&s, 2000);
	CHECK(scan_poll(&s, 2150, false, true, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_PROBE);
	CHECK(scan_poll(&s, 2300, false, true, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 2450, false, true, false) == SCAN_ACT_NEXT);
	// ... or the carrier goes away
	scan_tuned(&s, 3000);
	scan_poll(&s, 3150, false, true, false);
	CHECK(scan_poll(&s, 3175, false, false, false) == SCAN_ACT_NEXT);
	// Hold on a signal, resume delay after it ends, back to hold if it returns
	scan_tuned(&s, 4000);
	CHECK(scan_poll(&s, 4075, true, true, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_HOLD && s.holds == 1);
	CHECK(scan_poll(&s, 20000, true, true, false) == SCAN_ACT_NONE);	// carrier mode: no hold limit
	CHECK(scan_poll(&s, 20025, false, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_RESUME);
	CHECK(scan_poll(&s, 21000, true, true, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_HOLD);
	scan_poll(&s, 21025, false, false, false);
	CHECK(scan_poll(&s, 23000, false, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 23025, false, false, false) == SCAN_ACT_NEXT);
	// Time mode: moves on while the signal lasts
	s.cfg.resume = SCAN_RESUME_TIME;
	scan_tuned(&s, 30000);
	scan_poll(&s, 30075, true, true, false);
	CHECK(scan_poll(&s, 35000, true, true, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 35075, true, true, false) == SCAN_ACT_NEXT);
	// Stop mode: the first signal ends the scan
	s.cfg.resume = SCAN_RESUME_STOP;
	scan_tuned(&s, 40000);
	CHECK(scan_poll(&s, 40075, true, true, false) == SCAN_ACT_STOP);
	// Paused: nothing moves; resuming listens afresh
	s.cfg.resume = SCAN_RESUME_CARRIER;
	scan_tuned(&s, 50000);
	scan_pause(&s, true, 50050);
	CHECK(scan_poll(&s, 60000, false, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_PAUSED);
	scan_pause(&s, false, 60000);
	CHECK(scan_poll(&s, 60100, false, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_LISTEN);
	CHECK(scan_poll(&s, 60150, false, false, false) == SCAN_ACT_NEXT);
	// DMR: a carrier seen in the last carrier_hold_ms still counts (direct mode is on air every other slot) ...
	s.cfg.probe_ms = 700;
	s.cfg.sync_ms = 1500;
	s.cfg.carrier_hold_ms = 150;
	uint32_t probes = s.probes;
	scan_tuned(&s, 70000);
	CHECK(scan_poll(&s, 70075, false, true, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 70150, false, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_PROBE);
	CHECK(s.probes == probes + 1);
	CHECK(scan_poll(&s, 70200, false, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 70250, false, false, false) == SCAN_ACT_NEXT);	// nothing for 175 ms
	// ... a flickering carrier keeps the probe for probe_ms
	scan_tuned(&s, 80000);
	scan_act_t a = SCAN_ACT_NONE;
	uint32_t t = 80050;
	for (; t <= 82000 && a == SCAN_ACT_NONE; t += 25) {
		a = scan_poll(&s, t, false, (t / 25) % 2 == 0, false);
	}
	CHECK(a == SCAN_ACT_NEXT && t - 25 == 80150 + 700);
	// ... and sync (bursts with the channel's color code) extends it to sync_ms
	scan_tuned(&s, 90000);
	a = SCAN_ACT_NONE;
	for (t = 90050; t <= 93000 && a == SCAN_ACT_NONE; t += 25) {
		a = scan_poll(&s, t, false, (t / 25) % 2 == 0, t >= 90500);
	}
	CHECK(a == SCAN_ACT_NEXT && t - 25 == 90150 + 1500 && s.stats_synced == 1);
	s.cfg.sync_ms = s.cfg.carrier_hold_ms = 0;
	scan_end(&s);
	CHECK(!scan_on(&s) && s.skipped == 0 && !scan_pos_skipped(&s, 2));

	// Priority: the start channel after every SCAN_PRIORITY_EVERY others, not in the sweep itself
	memset(&s, 0, sizeof(s));
	cfg.priority = true;
	s.origin_pos = 0;
	scan_begin(&s, SCAN_ZONE, &cfg, 0);
	int seq[8];
	for (int i = 0; i < 8; i++) {
		seq[i] = scan_next_pos(&s, 6, all_usable, NULL);
	}
	CHECK(seq[0] == 1 && seq[1] == 2 && seq[2] == 0 && seq[3] == 4 && seq[4] == 5 && seq[5] == 0);
	CHECK(seq[6] == 1 && seq[7] == 2);

	// Our own transmission (dual watch goes on through PTT): marked every poll while it lasts, then the stop waits
	// the delay as after a signal, and a reply holds it
	memset(&s, 0, sizeof(s));
	cfg.priority = false;
	scan_begin(&s, SCAN_DUAL, &cfg, 0);
	scan_talked(&s, 1000);
	CHECK(s.phase == SCAN_PH_RESUME);
	scan_talked(&s, 5000);
	CHECK(scan_poll(&s, 5025, false, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 6975, false, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 7000, false, false, false) == SCAN_ACT_NEXT);
	scan_talked(&s, 8000);
	CHECK(scan_poll(&s, 8100, true, true, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_HOLD);

	// Nothing usable
	memset(&s, 0, sizeof(s));
	scan_begin(&s, SCAN_ZONE, &cfg, 0);
	for (uint16_t p = 0; p < 6; p++) {
		scan_nuisance_pos(&s, p);
	}
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == -1);

	// VFO: steps, wraps, skips nuisance frequencies
	memset(&s, 0, sizeof(s));
	s.lo_hz = 146000000;
	s.hi_hz = 146100000;
	scan_begin(&s, SCAN_VFO, &cfg, 0);
	CHECK(scan_next_hz(&s, 146000000, 25000) == 146025000);
	CHECK(scan_next_hz(&s, 146100000, 25000) == 146000000);
	CHECK(scan_next_hz(&s, 146090000, 25000) == 146000000);	// no room for a step: wrap
	CHECK(scan_next_hz(&s, 145000000, 25000) == 146000000);	// outside: the low end
	CHECK(scan_nuisance_hz(&s, 146050000) && !scan_nuisance_hz(&s, 146050000));
	CHECK(scan_next_hz(&s, 146025000, 25000) == 146075000);
	for (uint32_t hz = 146000000; hz <= 146100000; hz += 25000) {
		scan_nuisance_hz(&s, hz);
	}
	CHECK(scan_next_hz(&s, 146000000, 25000) == 0);
}

// ---- the battery monitor on its own ----

static void test_battery_module(void)
{
	batt_t b;
	batt_init(&b);
	CHECK(batt_update(&b, 7800, 6400, 6000, 1000) == BATT_EV_NONE && b.level == BATT_OK);
	CHECK(batt_update(&b, 0, 6400, 6000, 2000) == BATT_EV_NONE && b.mv == 7800);	// no reading
	// One sag is not a flat battery; three in a row are
	CHECK(batt_update(&b, 6300, 6400, 6000, 3000) == BATT_EV_NONE);
	CHECK(batt_update(&b, 6500, 6400, 6000, 4000) == BATT_EV_NONE && b.level == BATT_OK);
	batt_update(&b, 6300, 6400, 6000, 5000);
	batt_update(&b, 6300, 6400, 6000, 6000);
	CHECK(batt_update(&b, 6300, 6400, 6000, 7000) == BATT_EV_LOW && b.level == BATT_LOW);
	CHECK(batt_update(&b, 6300, 6400, 6000, 8000) == BATT_EV_NONE);
	CHECK(batt_update(&b, 6300, 6400, 6000, 7000 + BATT_LOW_REPEAT_MS) == BATT_EV_LOW);	// repeats
	// Hysteresis: 6500 isn't enough to leave low, 6600 is
	CHECK(batt_update(&b, 6500, 6400, 6000, 200000) == BATT_EV_NONE && b.level == BATT_LOW);
	batt_update(&b, 6600, 6400, 6000, 201000);
	CHECK(b.level == BATT_OK);
	// Straight to critical
	for (int i = 0; i < 3; i++) {
		batt_update(&b, 5900, 6400, 6000, 300000 + (uint32_t)i * 1000);
	}
	CHECK(b.level == BATT_CRITICAL && batt_critical_for(&b, 312000) == 10000);
	CHECK(batt_update(&b, 5900, 6400, 6000, 302000 + BATT_CRIT_REPEAT_MS) == BATT_EV_CRITICAL);
	// A charger: critical -> low (still below low + hysteresis) -> ok
	batt_update(&b, 6300, 6400, 6000, 400000);
	CHECK(b.level == BATT_LOW && batt_critical_for(&b, 400000) == 0);
	batt_update(&b, 7000, 6400, 6000, 401000);
	CHECK(b.level == BATT_OK);
	// No thresholds: never warns
	batt_init(&b);
	for (int i = 0; i < 5; i++) {
		CHECK(batt_update(&b, 5000, 0, 0, (uint32_t)i * 1000) == BATT_EV_NONE);
	}

	// Backlight
	CHECK(batt_backlight_pct(80, 30, 120, 0, BATT_OK) == 80);
	CHECK(batt_backlight_pct(80, 30, 120, 30000, BATT_OK) == 16);
	CHECK(batt_backlight_pct(10, 30, 120, 30000, BATT_OK) == 5);
	CHECK(batt_backlight_pct(80, 30, 120, 120000, BATT_OK) == 0);
	CHECK(batt_backlight_pct(80, 0, 0, 999999, BATT_OK) == 80);
	CHECK(batt_backlight_pct(80, 30, 120, 0, BATT_LOW) == BATT_DIM_PCT);
}

// ---- the beep engine on its own ----

// Tones without PCM audio (beep_init_tone): each segment's tone for its whole polls, then off
static uint16_t tone_hz[16];
static uint8_t tone_lvl[16];
static int tone_calls;
static void fake_tone(uint16_t hz, uint8_t level)
{
	if (tone_calls < 16) {
		tone_hz[tone_calls] = hz;
		tone_lvl[tone_calls] = level;
	}
	tone_calls++;
}

static void test_beep_tone(void)
{
	beep_init(NULL);
	beep_init_tone(fake_tone, 0);
	CHECK(beep_tone_route() && !beep_has_hang());	// the view's beep.tone 1, no beep.warm
	beep_reset_stats();
	tone_calls = 0;
	beep_play(BEEP_KEY, 0, false);
	CHECK(tone_calls == 0 && !beep_busy());

	// The key beep, 25 ms, timed to the millisecond from the radio task's next pass (beep_tone_run)
	beep_play(BEEP_KEY, 70, false);
	CHECK(tone_calls == 0 && beep_busy() && beep_tone_wait(1000) == 0);
	beep_tone_run(1000);
	CHECK(tone_calls == 1 && tone_hz[0] == 1000 && tone_lvl[0] == 70);
	CHECK(beep_tone_wait(1010) == 15);
	beep_tone_run(1024);
	CHECK(tone_calls == 1 && beep_busy());
	beep_tone_run(1025);
	CHECK(tone_calls == 2 && tone_hz[1] == 0 && !beep_busy() && beep_tone_wait(1025) == -1);
	beep_tone_run(1100);
	CHECK(tone_calls == 2);

	// Call end: 1200 Hz 50 ms, a 30 ms gap, 900 Hz 70 ms
	tone_calls = 0;
	beep_play(BEEP_CALL_END, 100, false);
	beep_tone_run(2000);
	CHECK(tone_calls == 1 && tone_hz[0] == 1200);
	beep_tone_run(2050);			// the gap, silent
	CHECK(tone_calls == 2 && tone_hz[1] == 0 && tone_lvl[1] == 0 && beep_busy());
	beep_tone_run(2080);			// the 900 Hz tone, to 2150
	CHECK(tone_calls == 3 && tone_hz[2] == 900 && tone_lvl[2] == 100);
	beep_tone_run(2149);
	CHECK(tone_calls == 3 && beep_busy());
	beep_service(false, 2150);		// beep_service runs the tones too
	CHECK(tone_calls == 4 && tone_hz[3] == 0 && !beep_busy());

	// A late pass makes every change due, in order
	tone_calls = 0;
	beep_play(BEEP_CALL_END, 100, false);
	beep_tone_run(3000);
	beep_tone_run(3200);
	CHECK(tone_calls == 4 && tone_hz[1] == 0 && tone_hz[2] == 900 && tone_hz[3] == 0 && !beep_busy());

	// A lead: each tone runs silent (level 0) that long first; a gap before a tone runs that tone silent
	beep_init_tone(fake_tone, 40);
	tone_calls = 0;
	beep_play(BEEP_KEY, 70, false);
	beep_tone_run(4000);
	CHECK(tone_calls == 1 && tone_hz[0] == 1000 && tone_lvl[0] == 0 && beep_tone_wait(4000) == 40);
	beep_tone_run(4040);
	CHECK(tone_calls == 2 && tone_hz[1] == 1000 && tone_lvl[1] == 70);
	beep_tone_run(4064);
	CHECK(tone_calls == 2);
	beep_tone_run(4065);
	CHECK(tone_calls == 3 && tone_hz[2] == 0 && !beep_busy());
	tone_calls = 0;
	beep_play(BEEP_CALL_END, 100, false);
	beep_tone_run(5000);			// 1200 Hz silent to 5040, sounding to 5090
	beep_tone_run(5040);
	CHECK(tone_calls == 2 && tone_hz[1] == 1200 && tone_lvl[1] == 100);
	beep_tone_run(5090);			// the gap: 900 Hz silent to 5120
	CHECK(tone_calls == 3 && tone_hz[2] == 900 && tone_lvl[2] == 0);
	beep_tone_run(5120);
	CHECK(tone_calls == 4 && tone_hz[3] == 900 && tone_lvl[3] == 100);
	beep_tone_run(5190);
	CHECK(tone_calls == 5 && tone_hz[4] == 0 && !beep_busy());

	// A new pattern replaces one still sounding
	beep_init_tone(fake_tone, 0);
	tone_calls = 0;
	beep_play(BEEP_TX_DENY, 50, false);
	beep_tone_run(6000);
	beep_play(BEEP_KEY, 50, false);
	beep_tone_run(6010);
	CHECK(tone_calls == 2 && tone_hz[1] == 1000);
	beep_tone_run(6035);
	CHECK(tone_calls == 3 && tone_hz[2] == 0 && !beep_busy());
	beep_stats_t bs;
	beep_get_stats(&bs);
	CHECK(bs.played == 7 && bs.no_stream == 0);

	// No hook: dropped, as before
	beep_init_tone(NULL, 0);
	CHECK(!beep_tone_route());
	beep_play(BEEP_KEY, 50, false);
	beep_get_stats(&bs);
	CHECK(bs.no_stream == 1 && !beep_busy());

	// The sweep: a tone hook plays fixed pitches, so it is dropped (counted in no_stream), never a 1 Hz "tone"
	beep_init_tone(fake_tone, 0);
	tone_calls = 0;
	beep_play(BEEP_SWEEP, 70, false);
	beep_tone_run(7000);
	beep_get_stats(&bs);
	CHECK(tone_calls == 0 && !beep_busy() && bs.no_stream == 2);
}

// The speaker test's sweep (BEEP_SWEEP): silence with the route up, then a rising tone with no jump between samples, from
// BEEP_SWEEP_LO_HZ to BEEP_SWEEP_HI_HZ, then silence again
static void test_beep_sweep(void)
{
	memset(&aud, 0, sizeof(aud));
	beep_init(&fake_audio);
	const beep_seg_t *segs;
	CHECK(beep_pattern(BEEP_SWEEP, &segs) == 3 && segs[0].hz == 0 && segs[1].hz == BEEP_HZ_SWEEP && segs[2].hz == 0);
	uint32_t total = 0;
	for (int i = 0; i < 3; i++) {
		total += segs[i].ms;
	}
	beep_play(BEEP_SWEEP, 100, false);
	static int16_t out[(4600 / 20 + 20) * AUDIO_BLOCK_SAMPLES];
	int n = 0;
	int16_t blk[AUDIO_BLOCK_SAMPLES];
	while (beep_busy() && n + AUDIO_BLOCK_SAMPLES <= (int)(sizeof out / sizeof out[0])) {
		beep_audio_block(mic, blk, NULL);
		memcpy(&out[n], blk, sizeof blk);
		n += AUDIO_BLOCK_SAMPLES;
	}
	CHECK(!beep_busy() && aud.pcm);
	CHECK(n >= (int)(total * 8) && n <= (int)(total * 8) + 20 * AUDIO_BLOCK_SAMPLES);	// its length and the lead-in
	// The first sound: after the lead-in and the 400 ms of silence
	int first = 0;
	while (first < n && out[first] == 0) {
		first++;
	}
	CHECK(first >= 400 * 8 && first < 400 * 8 + 11 * AUDIO_BLOCK_SAMPLES);
	// Zero crossings in 20 ms near each end: about 2 x 20 ms x the frequency there
	int lo = 0, hi = 0;
	int a = first + 400, b = first + 4000 * 8 - 600;
	for (int i = a; i < a + 160; i++) {
		lo += (out[i] < 0) != (out[i + 1] < 0);
	}
	for (int i = b; i < b + 160; i++) {
		hi += (out[i] < 0) != (out[i + 1] < 0);
	}
	CHECK(lo >= 5 && lo <= 9);		// near 160 Hz
	CHECK(hi >= 145 && hi <= 155);		// near 3780 Hz
	// Phase continuous: each sample follows from the two before it as a sine at the sweep's frequency there would
	// (x[n+1] = 2 cos(w) x[n] - x[n-1]), within the sine table's
	// interpolation error
	double worst = 0;
	for (int i = first + 16 + 1; i < first + 4000 * 8 - 16; i++) {
		double f = BEEP_SWEEP_LO_HZ + (double)(BEEP_SWEEP_HI_HZ - BEEP_SWEEP_LO_HZ) * (i - first) / (4000 * 8);
		double pred = 2 * cos(2 * M_PI * f / 8000) * out[i] - out[i - 1];
		double e = fabs(out[i + 1] - pred);
		worst = e > worst ? e : worst;
	}
	CHECK(worst < 1500);	// about 450 at the top of the sweep; a jump misses by up to twice the amplitude
}

static void test_beep_module(void)
{
	memset(&aud, 0, sizeof(aud));
	beep_init(&fake_audio);
	beep_reset_stats();
	const beep_seg_t *segs;
	CHECK(beep_pattern(BEEP_KEY, &segs) == 1 && segs[0].hz == 1000 && segs[0].ms == 25);
	CHECK(beep_pattern(BEEP_NONE, &segs) == 0 && beep_pattern(BEEP_COUNT, &segs) == 0);
	for (int b = BEEP_KEY; b < BEEP_COUNT; b++) {
		uint8_t n = beep_pattern((beep_t)b, &segs);
		CHECK(n >= 1 && n <= BEEP_SEGS_MAX && (segs[0].hz != 0 || b == BEEP_SWEEP));	// the sweep starts silent
	}

	// Level 0 or BEEP_NONE: nothing
	beep_play(BEEP_KEY, 0, false);
	beep_play(BEEP_NONE, 50, false);
	CHECK(aud.starts == 0 && !beep_busy());

	// Own stream: started, one silent block with the speaker routed (the route settles), the pattern for its length
	// (25 ms = 200 samples: two blocks), the route held through four silent blocks, then the stream stops
	beep_play(BEEP_KEY, 100, false);
	CHECK(aud.starts == 1 && aud.streaming && beep_busy());
	int16_t blk[AUDIO_BLOCK_SAMPLES];
	beep_audio_block(mic, blk, NULL);
	CHECK(aud.pcm && blk[0] == 0 && blk[80] == 0 && blk[159] == 0 && beep_busy());
	beep_audio_block(mic, blk, NULL);
	int peak = 0;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int v = blk[i] < 0 ? -blk[i] : blk[i];
		peak = v > peak ? v : peak;
	}
	CHECK(aud.pcm && peak > 14000 && peak <= 16100 && blk[0] == 0);	// level 100: about half scale, faded in
	// A 1 kHz tone at 8 kHz: 8 samples per period -> sign changes every 4 samples
	int crossings = 0;
	for (int i = 40; i < 120; i++) {
		if ((blk[i] < 0) != (blk[i + 1] < 0)) {
			crossings++;
		}
	}
	CHECK(crossings >= 18 && crossings <= 22);
	beep_audio_block(mic, blk, NULL);	// samples 160..199, then silence
	CHECK(blk[100] == 0 && blk[159] == 0 && aud.pcm);
	for (int i = 0; i < 3; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
		CHECK(aud.pcm && aud.streaming && !beep_busy() && blk[0] == 0);	// held: the queued tail plays out
	}
	beep_audio_block(mic, blk, NULL);
	CHECK(!aud.pcm);
	beep_service(false, 0);
	CHECK(!aud.streaming && aud.stops == 1);

	// The stream's clock stops after one block (a codec that goes quiet): the beep gives the stream and the speaker
	// route up after STALL_POLLS polls instead of holding the PCM route on until a reboot
	beep_play(BEEP_KEY, 100, false);
	CHECK(aud.starts == 2 && aud.streaming);
	beep_audio_block(mic, blk, NULL);
	CHECK(aud.pcm && beep_busy());
	beep_service(false, 0);			// sees the block
	for (int i = 0; i < 7; i++) {
		beep_service(false, 0);
	}
	CHECK(aud.streaming && aud.pcm);	// 7 polls (175 ms) without a block: still waiting
	beep_service(false, 0);
	CHECK(!aud.streaming && !aud.pcm && !beep_busy() && aud.stops == 2);
	beep_stats_t bs;
	beep_get_stats(&bs);
	CHECK(bs.stalls == 1);
	// A healthy stream is never given up: a block between polls resets the count. The beep after a give-up still
	// gets its silent lead-in block (the stall left the lead counter at its end)
	beep_play(BEEP_ERROR, 100, false);
	beep_audio_block(mic, blk, NULL);
	bool lead_silent = true;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		lead_silent &= blk[i] == 0;
	}
	CHECK(lead_silent && aud.pcm && beep_busy());
	beep_service(false, 0);
	for (int i = 0; i < 12; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
	}
	beep_get_stats(&bs);
	CHECK(bs.stalls == 1);
	while (aud.streaming) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
	}
	CHECK(!aud.pcm && aud.stops == 3);

	// A start that fails after one block already played part of the beep: stopped, route back, not busy
	a_start_fails = true;
	beep_play(BEEP_KEY, 100, false);
	a_start_fails = false;
	CHECK(aud.starts == 4 && aud.stops == 4 && !aud.pcm && !beep_busy());

	// Shared (the voice stream runs): no stream started; beep_mix adds into the block
	beep_play(BEEP_TX_PERMIT, 50, true);
	CHECK(aud.starts == 4);	// no stream of its own
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		blk[i] = 1000;
	}
	CHECK(beep_mix(blk));
	CHECK(blk[0] == 1000 && blk[20] != 1000);
	// 50+40+50 ms = 1120 samples = 7 blocks; the eighth is quiet
	for (int i = 0; i < 6; i++) {
		CHECK(beep_mix(blk));
	}
	CHECK(!beep_mix(blk));
	// Saturation
	beep_play(BEEP_ERROR, 100, true);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		blk[i] = 30000;
	}
	beep_mix(blk);
	beep_mix(blk);
	bool clipped = false;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		clipped |= blk[i] == 32767;
	}
	CHECK(clipped);
	// No audio: counted as dropped
	beep_init(NULL);
	beep_play(BEEP_KEY, 50, false);
	beep_stats_t st;
	beep_get_stats(&st);
	CHECK(st.no_stream == 2 && st.played == 5);
	beep_init(&fake_audio);

	// A beep inside the hold: no second lead-in, the tone starts in the first block
	beep_play(BEEP_KEY, 100, false);
	beep_audio_block(mic, blk, NULL);	// lead-in
	beep_audio_block(mic, blk, NULL);
	beep_audio_block(mic, blk, NULL);	// the tone ends
	beep_audio_block(mic, blk, NULL);	// held
	beep_play(BEEP_KEY, 100, false);	// held, not idle long enough to stop
	CHECK(aud.pcm);
	beep_audio_block(mic, blk, NULL);
	CHECK(blk[0] == 0 && blk[42] != 0);
	for (int i = 0; i < 8; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
	}
	CHECK(!aud.pcm && !aud.streaming);

	// A board whose amplifier unmutes slowly (speaker_lead_ms 50): three silent blocks with the speaker routed, then
	// the whole tone
	static const hal_audio_t slow_amp = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm, .speaker_lead_ms = 50 };
	beep_init(&slow_amp);
	beep_play(BEEP_KEY, 100, false);
	for (int b = 0; b < 3; b++) {
		beep_audio_block(mic, blk, NULL);
		bool silent = true;
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			silent &= blk[i] == 0;
		}
		CHECK(silent && aud.pcm && beep_busy());
	}
	beep_audio_block(mic, blk, NULL);
	CHECK(blk[0] == 0 && blk[42] != 0);	// the tone from the block's start, faded in
	for (int i = 0; i < 10; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
	}
	CHECK(!aud.pcm && !aud.streaming);
	// The voice stream takes a beep over during its hold (DMR transmit right after a beep): the next beep on the
	// beep stream still gets the whole lead-in, not the count the last one left
	beep_play(BEEP_KEY, 100, false);
	for (int b = 0; b < 4; b++) {
		beep_audio_block(mic, blk, NULL);
	}
	a_stop();
	for (int b = 0; b < 8; b++) {	// the voice stream runs on past the hold
		beep_mix(blk);
	}
	CHECK(!beep_busy());
	beep_play(BEEP_KEY, 100, false);
	for (int b = 0; b < 3; b++) {
		beep_audio_block(mic, blk, NULL);
		bool silent = true;
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			silent &= blk[i] == 0;
		}
		CHECK(silent && aud.pcm);
	}
	beep_audio_block(mic, blk, NULL);
	CHECK(blk[42] != 0);
	for (int i = 0; i < 10; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 0);
	}
	CHECK(!aud.pcm && !aud.streaming);

	// The amplifier hang (speaker_warm, 2 s): a key at 1000 ms powers it, a beep at once still takes the full lead
	// (the amplifier only just came up), one after the lead takes one block, and the hang ends 2 s after the last event
	warm_calls = 0;
	warm_state = false;
	static const hal_audio_t hang_amp = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm, .speaker_lead_ms = 50,
					      .speaker_warm = a_warm, .speaker_hang_s = 2 };
	beep_init(&hang_amp);
	CHECK(beep_has_hang() && !beep_tone_route());
	beep_service(false, 1000);
	CHECK(!beep_warm_on() && !warm_state);
	beep_warm(1000);
	CHECK(beep_warm_on() && warm_state && warm_calls == 1);
	beep_warm(1010);	// again: held, not powered twice
	CHECK(warm_calls == 1);
	beep_reset_stats();
	beep_play(BEEP_KEY, 100, false);
	for (int i = 0; i < 12; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 1010);
	}
	beep_stats_t ws;
	beep_get_stats(&ws);
	CHECK(ws.leads == 3);
	beep_service(false, 1100);	// 100 ms warm: past the 50 ms lead
	beep_play(BEEP_KEY, 100, false);
	beep_audio_block(mic, blk, NULL);
	beep_audio_block(mic, blk, NULL);
	beep_get_stats(&ws);
	CHECK(ws.leads == 4 && blk[42] != 0);	// one silent block, then the tone
	for (int i = 0; i < 10; i++) {
		beep_audio_block(mic, blk, NULL);
		beep_service(false, 1200);
	}
	CHECK(beep_warm_on());		// the beep at 1100 holds it until 3100
	beep_service(false, 3099);
	CHECK(beep_warm_on() && warm_state);
	beep_service(false, 3100);
	CHECK(!beep_warm_on() && !warm_state && warm_calls == 2);
	// Shared (a DMR call left the voice stream running) with the amplifier cold: the same lead-in, mixed in as silence
	// with the speaker routed, then the tone; warm past the lead, the tone in the first block
	beep_reset_stats();
	beep_play(BEEP_KEY, 100, true);		// powers the amplifier at 3100: the full lead
	for (int b = 0; b < 4; b++) {
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			blk[i] = 1000;
		}
		bool sounds = beep_mix(blk);
		CHECK(sounds && beep_busy() && (b < 3 ? blk[42] == 1000 : blk[42] != 1000));
	}
	while (beep_mix(blk)) {
	}
	beep_get_stats(&ws);
	CHECK(!beep_busy() && ws.leads == 3);
	beep_service(true, 3200);		// 100 ms warm
	beep_play(BEEP_KEY, 100, true);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		blk[i] = 1000;
	}
	CHECK(beep_mix(blk) && blk[42] != 1000);
	while (beep_mix(blk)) {
	}
	beep_get_stats(&ws);
	CHECK(ws.leads == 3);
	beep_service(true, 5200);
	CHECK(!beep_warm_on() && !warm_state);
	// A board without the hook or with no hang: never warm
	beep_init(&fake_audio);
	CHECK(!beep_has_hang());
	beep_warm(4000);
	CHECK(!beep_warm_on());
}

// ---- through the radio core ----

// The vibration pattern player alone, with a recording motor and without one
static void test_vibe_module(void)
{
	vibe_reset();
	memset(&hw, 0, sizeof(hw));
	const vibe_pulse_t *p;
	CHECK(vibe_pattern(VIBE_NONE, &p) == 0 && p == NULL && vibe_pattern(VIBE_COUNT, &p) == 0);
	for (int v = VIBE_MESSAGE; v < VIBE_COUNT; v++) {
		uint8_t n = vibe_pattern((vibe_t)v, &p);
		CHECK(n >= 1 && n <= VIBE_PULSES_MAX && p[0].on_ms >= 100 && p[n - 1].off_ms == 0);
	}
	CHECK(vibe_pattern(VIBE_CALL, &p) == 1 && p[0].on_ms == 500);

	// Nothing asked: the motor stays still
	vibe_service(0, p_vibrate);
	CHECK(hw.vibes == 0 && !vibe_busy());
	vibe_play(VIBE_NONE);
	vibe_play(VIBE_COUNT);
	vibe_service(0, p_vibrate);
	CHECK(hw.vibes == 0 && !vibe_busy() && vibe_played() == 0);

	// A message: two 150 ms pulses, the second 300 ms after the first started
	vibe_play(VIBE_MESSAGE);
	CHECK(vibe_busy());
	vibe_service(1000, p_vibrate);
	CHECK(hw.vibes == 1 && hw.vibe_ms[0] == 150 && vibe_busy() && vibe_played() == 1);
	vibe_service(1275, p_vibrate);
	CHECK(hw.vibes == 1);
	vibe_service(1300, p_vibrate);
	CHECK(hw.vibes == 2 && hw.vibe_ms[1] == 150 && !vibe_busy());
	vibe_service(2000, p_vibrate);
	CHECK(hw.vibes == 2 && vibe_played() == 1);

	// A new alert replaces the one still running, from its first pulse
	vibe_play(VIBE_EMERGENCY);
	vibe_service(3000, p_vibrate);
	CHECK(hw.vibes == 3 && hw.vibe_ms[2] == 400 && vibe_busy());
	vibe_play(VIBE_CALL);
	vibe_service(3100, p_vibrate);
	CHECK(hw.vibes == 4 && hw.vibe_ms[3] == 500 && !vibe_busy() && vibe_played() == 3);

	// The clock wrapping around between pulses
	vibe_play(VIBE_MESSAGE);
	vibe_service(0xFFFFFF00u, p_vibrate);
	vibe_service(0x00000010u, p_vibrate);
	CHECK(hw.vibes == 5 && vibe_busy());
	vibe_service(0x0000002Cu, p_vibrate);
	CHECK(hw.vibes == 6 && !vibe_busy());

	// No motor on this radio: the pattern is dropped, nothing counted
	vibe_play(VIBE_EMERGENCY);
	vibe_service(5000, NULL);
	CHECK(!vibe_busy() && vibe_played() == 4);
	vibe_reset();
	CHECK(vibe_played() == 0);
}

// Through the radio core: RREQ_VIBRATE plays on board.power->vibrate from the poll, and isn't user activity
static void test_vibrate(void)
{
	start();
	vibe_reset();
	run(1000);
	uint32_t act = sm.activity_ms;
	uint32_t t0 = now;
	req(RREQ_VIBRATE, VIBE_EMERGENCY);
	CHECK(sm.activity_ms == act);
	run(3000);
	CHECK(hw.vibes == 4 && vibe_played() == 1 && !vibe_busy());
	for (int i = 0; i < 4 && i < hw.vibes; i++) {
		CHECK(hw.vibe_ms[i] == 400);
		// The first pulse at the next poll, the rest 600 ms apart (within one poll)
		uint32_t want = t0 + (uint32_t)i * 600u;
		CHECK(hw.vibe_at[i] >= want && hw.vibe_at[i] <= want + 2u * RADIO_POLL_MS);
	}
	req(RREQ_VIBRATE, 99);		// out of range: nothing
	req(RREQ_VIBRATE, VIBE_MESSAGE);
	run(500);
	CHECK(hw.vibes == 6 && hw.vibe_ms[5] == 150);
}

static void test_zone_scan(void)
{
	start();
	CHECK(sm.view.zone_pos == 0 && sm.view.scan == SCAN_OFF);
	// Nothing heard: every usable channel in turn, skipping the scan-skip one and the empty slot
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_ZONE && sm.view.zone_pos == 1);
	uint16_t seen[12];
	int n = 0;
	seen[n++] = sm.view.zone_pos;
	for (int i = 0; i < 400 && n < 12; i++) {
		run(25);
		if (sm.view.zone_pos != seen[n - 1]) {
			seen[n++] = sm.view.zone_pos;
		}
	}
	CHECK(n == 12);
	CHECK(seen[0] == 1 && seen[1] == 4 && seen[2] == 5 && seen[3] == 6 && seen[4] == 0 && seen[5] == 1);
	CHECK(sm.scan.steps >= 11);

	// A signal on "Other": the scan holds there with the squelch open
	signal_on(0, 145500000, 30, no_tone);
	for (int i = 0; i < 200 && !(sm.view.zone_pos == 4 && sm.view.scan_phase == SCAN_PH_HOLD); i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 4 && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.state == RS_RX && hw.audio);
	run(10000);
	CHECK(sm.view.zone_pos == 4 && sm.view.scan_phase == SCAN_PH_HOLD);	// carrier mode: stays while it lasts
	// The signal ends: the resume delay, then on
	hw.sig[0].hz = 0;
	run(1500);
	CHECK(sm.view.zone_pos == 4 && sm.view.scan_phase == SCAN_PH_RESUME);
	run(700);
	CHECK(sm.view.zone_pos != 4);

	// Nuisance delete: back on Other, delete it, it is never visited again
	hw.sig[0].hz = 145500000;
	for (int i = 0; i < 200 && sm.view.scan_phase != SCAN_PH_HOLD; i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 4);
	req(RREQ_SCAN, RSCAN_NUISANCE);
	CHECK(sm.view.zone_pos == 5 && sm.view.scan_skipped == 1 && sm.view.scan_phase == SCAN_PH_LISTEN);
	bool visited = false;
	for (int i = 0; i < 200; i++) {
		run(25);
		visited |= sm.view.zone_pos == 4;
	}
	CHECK(!visited);
	hw.sig[0].hz = 0;

	// Monitor pauses the stepping
	req(RREQ_MONITOR, 1);
	uint16_t at = sm.view.zone_pos;
	run(2000);
	CHECK(sm.view.zone_pos == at && sm.view.scan_phase == SCAN_PH_PAUSED);
	req(RREQ_MONITOR, 0);
	run(500);
	CHECK(sm.view.zone_pos != at);

	// Cancel: back to where it started (Home); the nuisance list is gone with the scan
	req(RREQ_SCAN, RSCAN_CANCEL);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == 0 && sm.view.scan_skipped == 0);
	// Stop: stays where it is
	req(RREQ_SCAN, RSCAN_ZONE);
	run(400);
	at = sm.view.zone_pos;
	req(RREQ_SCAN, RSCAN_STOP);
	run(1000);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == at && at != 0);
	// Toggle starts and stops
	req(RREQ_SCAN, RSCAN_TOGGLE);
	CHECK(sm.view.scan == SCAN_ZONE);
	req(RREQ_SCAN, RSCAN_TOGGLE);
	CHECK(sm.view.scan == SCAN_OFF);
	// A channel change by the user stops it
	req(RREQ_SCAN, RSCAN_ZONE);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.scan == SCAN_OFF);
}

static void test_scan_ptt_and_modes(void)
{
	start();
	// PTT while still searching: back to the start channel, transmit there
	req(RREQ_SCAN, RSCAN_ZONE);
	run(300);
	CHECK(sm.view.zone_pos != 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == 0 && sm.view.state == RS_TX && hw.tx_hz == 146520000);
	req(RREQ_PTT, 0);

	// PTT while held on a signal: talk back on it
	signal_on(0, 145500000, 30, no_tone);
	req(RREQ_SCAN, RSCAN_ZONE);
	for (int i = 0; i < 200 && sm.view.scan_phase != SCAN_PH_HOLD; i++) {
		run(25);
	}
	hw.sig[0].hz = 0;
	run(100);			// the signal ended: resume delay
	CHECK(sm.view.scan_phase == SCAN_PH_RESUME);
	req(RREQ_PTT, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == 4 && hw.tx_hz == 145500000);
	req(RREQ_PTT, 0);

	// Tone channel: a carrier without our tone is probed, then left; with the tone it holds
	req(RREQ_ZONE_POS, 0);
	signal_on(0, 146940000, 30, (subtone_t){ TONE_CTCSS, 1230 });
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.zone_pos == 1);
	run(250);
	CHECK(sm.view.zone_pos == 1 && sm.view.scan_phase == SCAN_PH_PROBE);
	run(400);
	CHECK(sm.view.zone_pos != 1);
	hw.sig[0].tone = (subtone_t){ TONE_CTCSS, 1000 };
	for (int i = 0; i < 400 && !(sm.view.zone_pos == 1 && sm.view.scan_phase == SCAN_PH_HOLD); i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 1 && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.tone_ok);
	req(RREQ_SCAN, RSCAN_STOP);
	hw.sig[0].hz = 0;

	// Time mode: moves on after the hold time while the signal lasts
	settings(set_resume_time);
	signal_on(0, 145500000, 30, no_tone);
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	for (int i = 0; i < 200 && sm.view.scan_phase != SCAN_PH_HOLD; i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 4);
	run(1900);
	CHECK(sm.view.zone_pos == 4);
	run(200);
	CHECK(sm.view.zone_pos != 4);
	req(RREQ_SCAN, RSCAN_STOP);

	// Stop mode: the scan ends on the first signal
	settings(set_resume_stop);
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	for (int i = 0; i < 200 && sm.view.scan != SCAN_OFF; i++) {
		run(25);
	}
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == 4 && sm.view.state == RS_RX);
	hw.sig[0].hz = 0;

	// Squelch fully open: an empty channel isn't a signal (stops need a carrier)
	settings(set_resume_carrier);
	req(RREQ_SQUELCH, -9);
	CHECK(sm.view.squelch == 0);
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	run(2000);
	CHECK(sm.view.scan == SCAN_ZONE && sm.scan.holds == 0 && sm.scan.steps > 5);
	req(RREQ_SCAN, RSCAN_CANCEL);
	req(RREQ_SQUELCH, 5);

	// Priority: Home between every two others
	settings(set_priority);
	req(RREQ_ZONE_POS, 0);
	req(RREQ_SCAN, RSCAN_ZONE);
	uint16_t seq[7];
	int n = 0;
	seq[n++] = sm.view.zone_pos;
	for (int i = 0; i < 400 && n < 7; i++) {
		run(25);
		if (sm.view.zone_pos != seq[n - 1] || sm.scan.steps != (uint32_t)n) {
			if (sm.scan.steps == (uint32_t)n + 1) {
				seq[n++] = sm.view.zone_pos;
			}
		}
	}
	CHECK(n == 7 && seq[0] == 1 && seq[1] == 4 && seq[2] == 0 && seq[3] == 5 && seq[4] == 6 && seq[5] == 0);
	req(RREQ_SCAN, RSCAN_CANCEL);

	// Dual watch (the debug link's request): Home and Last, nothing else; it starts listening where it is
	req(RREQ_ZONE_POS, 0);
	req2(RREQ_SCAN, RSCAN_DUAL, 6);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.zone_pos == 0);
	bool other = false;
	int home = 0, last = 0;
	for (int i = 0; i < 120; i++) {
		run(25);
		other |= sm.view.zone_pos != 0 && sm.view.zone_pos != 6;
		home += sm.view.zone_pos == 0;
		last += sm.view.zone_pos == 6;
	}
	CHECK(!other && home > 20 && last > 20);
	req(RREQ_SCAN, RSCAN_STOP);
	// Dual watch with itself, or in the VFO: refused
	req(RREQ_ZONE_POS, 0);
	req2(RREQ_SCAN, RSCAN_DUAL, 0);
	CHECK(sm.view.scan == SCAN_OFF);
}

static void set_resume_time(radio_settings_t *s) { s->scan_resume = SCAN_RESUME_TIME; s->scan_hold_s = 2; }
static void set_resume_stop(radio_settings_t *s) { s->scan_resume = SCAN_RESUME_STOP; }
static void set_resume_carrier(radio_settings_t *s) { s->scan_resume = SCAN_RESUME_CARRIER; s->scan_priority = false; }
static void set_priority(radio_settings_t *s) { s->scan_priority = true; }

static void test_vfo_scan(void)
{
	start();
	req(RREQ_VFO_SET, 146000000);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146000000 && sm.view.vfo_step_hz == 12500);
	req2(RREQ_SCAN_VFO, 146000000, 146100000);
	CHECK(sm.view.scan == SCAN_VFO && sm.view.ch.rx_hz == 146012500);
	signal_on(0, 146050000, 30, no_tone);
	for (int i = 0; i < 200 && sm.view.scan_phase != SCAN_PH_HOLD; i++) {
		run(25);
	}
	CHECK(sm.view.ch.rx_hz == 146050000 && sm.view.state == RS_RX);
	req(RREQ_SCAN, RSCAN_NUISANCE);
	CHECK(sm.view.ch.rx_hz == 146062500);
	// Wraps at the top, never stops on the deleted frequency
	bool wrapped = false, hit = false;
	for (int i = 0; i < 200; i++) {
		run(25);
		wrapped |= sm.view.ch.rx_hz == 146000000;
		hit |= sm.view.ch.rx_hz == 146050000;
		CHECK(sm.view.ch.rx_hz >= 146000000 && sm.view.ch.rx_hz <= 146100000);
	}
	CHECK(wrapped && !hit);
	req(RREQ_SCAN, RSCAN_CANCEL);
	CHECK(sm.view.ch.rx_hz == 146000000 && sm.view.scan == SCAN_OFF);
	// The whole band by default; a range across bands or out of band is refused; a zone scan in the VFO is refused
	req(RREQ_SCAN, RSCAN_VFO);
	CHECK(sm.view.scan == SCAN_VFO && sm.scan.lo_hz == 136000000 && sm.scan.hi_hz == 174000000);
	req(RREQ_SCAN, RSCAN_STOP);
	req2(RREQ_SCAN_VFO, 170000000, 410000000);
	CHECK(sm.view.scan == SCAN_OFF);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.scan == SCAN_OFF);
	req(RREQ_SCAN, RSCAN_TOGGLE);
	CHECK(sm.view.scan == SCAN_VFO);
	req(RREQ_SCAN, RSCAN_STOP);
	hw.sig[0].hz = 0;
}

// ---- DMR on the simulated modem (as in radioCoreTest.c) ----

static bool lc_private;		// hear_call: a private call to dst rather than a group call
static void mk_lc(uint8_t lc[9], uint32_t dst, uint32_t src)
{
	dmr_lc_t l = { .flco = lc_private ? DMR_FLCO_UNIT_VOICE : DMR_FLCO_GROUP_VOICE, .dst = dst, .src = src };
	dmr_lc_pack(&l, lc);
}

static void burst_lc(const uint8_t lc[9], uint8_t cc, bool terminator)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = terminator ? DMR_BURST_TERMINATOR_LC : DMR_BURST_VOICE_LC_HEADER;
	b.sync = DMR_SYNC_MS;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.len = 12;
	dmr_lc_encode_full(lc, terminator ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER, b.payload);
	sim_rx(&b);
}

static void burst_voice(uint8_t index, uint8_t cc, const uint8_t *lc)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = index == 0 ? DMR_SYNC_MS : DMR_SYNC_NONE;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.len = 27;
	for (int f = 0; f < 3; f++) {
		memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
	}
	if (lc != NULL) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	sim_rx(&b);
}

static void hear_call(uint32_t dst, uint8_t cc, int superframes, bool end)
{
	uint8_t lc[9];
	mk_lc(lc, dst, 9995555);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, cc, false);
		run(60);
	}
	for (int sf = 0; sf < superframes; sf++) {
		for (int i = 0; i < 6; i++) {
			burst_voice((uint8_t)i, cc, i == 5 ? lc : NULL);
			run(60);
		}
	}
	if (end) {
		burst_lc(lc, cc, true);
		run(100);
	}
}

static void test_mixed_scan(void)
{
	start();
	// A carrier on the DMR channel but no call: probed for the DMR probe time, then left
	signal_on(0, 438500000, 30, no_tone);
	req(RREQ_ZONE_POS, 4);
	req(RREQ_SCAN, RSCAN_ZONE);
	CHECK(sm.view.zone_pos == 5 && sm.view.ch.mode == RADIO_MODE_DMR && sim.started);
	run(300);
	CHECK(sm.view.zone_pos == 5 && sm.view.scan_phase == SCAN_PH_PROBE);
	run(700);
	CHECK(sm.view.zone_pos != 5 && !sim.started);	// back on FM: the stack let go of the modem
	// Next time round a call is on: the scan holds on it
	for (int i = 0; i < 200 && sm.view.zone_pos != 5; i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 5);
	hear_call(9, 1, 1, false);
	CHECK(sm.view.dmr.state == RDMR_RX && sm.view.scan_phase == SCAN_PH_HOLD);
	hear_call(9, 1, 2, true);
	CHECK(sm.view.zone_pos == 5);
	run(1000);
	CHECK(sm.view.scan_phase == SCAN_PH_RESUME);
	run(1500);
	CHECK(sm.view.zone_pos != 5);
	req(RREQ_SCAN, RSCAN_STOP);
	hw.sig[0].hz = 0;
}

// The modem on the call's frequency (as the HR_C6000 slot engine does it): nothing for AIR_LOCK_MS after a retune
// (the receiver restarts and searches), then voice from the first burst A on, with the embedded LC at the lc_at-th F
#define AIR_LOCK_MS	90

// A modem that keeps the slot timing it took (the HR_C7000, modem-hrc7000.md 5.3): on a frequency with a DMR signal
// the chip takes that transmitter's timing, and it keeps it over a retune until the receiver is restarted
// (hal_dmr_modem_t.rx_restart). Meanwhile another transmitter's bursts don't decode. The engine drops a timing it takes
// nothing from after 600 ms (RX_IDLE_TICKS); kept.hz's signal is in a color code no channel has, so nothing is taken
static struct {
	bool on;
	uint32_t hz;		// the DMR signal in the other color code
	bool held;		// the chip has its slot timing
	uint32_t since;		// taken then
	unsigned restarts;	// sim.rx_restarts when last looked at
} kept;

static void kept_timing(void)
{
	if (!kept.on) {
		return;
	}
	if (sim.rx_restarts != kept.restarts) {
		kept.restarts = sim.rx_restarts;
		kept.held = false;
	}
	if (kept.held && now - kept.since >= 600) {
		kept.held = false;
	}
	bool dmr = sm.view.ch.mode == RADIO_MODE_DMR && sim.started;
	if (!kept.held && dmr && hw.rx_hz == kept.hz && now >= hw.rx_ms + AIR_LOCK_MS) {
		kept.held = true;
		kept.since = now;
	}
}

static void air_burst(void)
{
	uint8_t idx = (uint8_t)(((now - air.t0) / 60u) % 6u);
	bool tuned = hw.rx_hz == air.hz && sm.view.ch.mode == RADIO_MODE_DMR && sim.started;
	if (!tuned || now < hw.rx_ms + AIR_LOCK_MS || (kept.held && kept.hz != air.hz)) {
		air.joined = false;
		return;
	}
	if (!air.joined || air.join_rx_ms != hw.rx_ms) {
		if (idx != 0) {
			air.joined = false;
			return;
		}
		air.joined = true;
		air.join_rx_ms = hw.rx_ms;
		air.sf = 0;
	}
	if (idx == 0) {
		air.sf++;
	}
	uint8_t lc[9];
	mk_lc(lc, air.dst, 9995555);
	burst_voice(idx, 1, idx == 5 && air.sf >= air.lc_at ? lc : NULL);
}

// Every channel but the scan-skip one a DMR channel (CC 1, TS 1, TG 9), as the HIL zone (all DMR): the retunes keep
// the stack running
static void all_dmr(void)
{
	static const uint16_t n[] = { 1, 2, 5, 7 };
	for (unsigned i = 0; i < sizeof(n) / sizeof(n[0]); i++) {
		chans[n[i]].mode = RADIO_MODE_DMR;
		chans[n[i]].rx_tone = chans[n[i]].tx_tone = no_tone;
		chans[n[i]].contact = 1;
	}
}

// HIL 2026-10-10 (test_scan_stops_on_dmr_call): the zone scan visited the bench's DMR channel every 2.5 s during a
// 20-superframe call (voice only by then: sync in burst A every 360 ms) and never held. A direct mode signal is on
// air 30 ms in every 60, so the noise reading at the end of the dwell, or any poll in the probe, often missed it and
// the probe ended at once; and the probe (700 ms) was shorter than a late entry that takes its LC from the second
// superframe. Now: a carrier seen in the last 150 ms counts, and bursts with the channel's color code extend the
// probe to 1.5 s. The call must hold the scan on the first visit, whatever the superframe phase
static void test_scan_dmr_mid_call(void)
{
	for (uint8_t lc_at = 1; lc_at <= 2; lc_at++) {
		for (uint32_t phase = 0; phase < 360; phase += 60) {
			start();
			all_dmr();
			signal_on(0, 438500000, 30, no_tone);
			hw.sig[0].tdma = true;
			air.on = true;
			air.hz = 438500000;
			air.t0 = now + 300 + phase;
			air.dst = 9;
			air.lc_at = lc_at;
			req(RREQ_ZONE_POS, 6);
			req(RREQ_SCAN, RSCAN_ZONE);
			uint32_t arrived = 0, held_ms = 0;
			bool left = false;
			for (int i = 0; i < 240 && held_ms == 0; i++) {
				run(25);
				if (sm.view.zone_pos == 5 && arrived == 0) {
					arrived = now;
				}
				left |= arrived != 0 && sm.view.zone_pos != 5;
				if (sm.view.zone_pos == 5 && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.dmr.state == RDMR_RX) {
					held_ms = now - arrived;
				}
			}
			if (held_ms == 0 || left) {
				printf("  mid-call DMR scan: LC at F %u, phase %u ms: held %u ms after arriving, left %d\n",
				       lc_at, (unsigned)phase, (unsigned)held_ms, left);
			}
			CHECK(arrived != 0 && held_ms != 0 && !left && held_ms <= 1650);
			req(RREQ_SCAN, RSCAN_STOP);
		}
	}

	// Nothing on the air: the scan still moves on at the dwell time
	start();
	all_dmr();
	req(RREQ_ZONE_POS, 6);
	req(RREQ_SCAN, RSCAN_ZONE);
	uint32_t steps = sm.scan.steps;
	run(1500);
	CHECK(sm.scan.steps - steps >= 9);
	req(RREQ_SCAN, RSCAN_STOP);

	// A call to a talkgroup we don't listen to: no hold, and the scan goes on soon after it is known
	start();
	all_dmr();
	signal_on(0, 438500000, 30, no_tone);
	hw.sig[0].tdma = true;
	air.on = true;
	air.hz = 438500000;
	air.t0 = now + 300;
	air.dst = 99;
	air.lc_at = 1;
	req(RREQ_ZONE_POS, 6);
	req(RREQ_SCAN, RSCAN_ZONE);
	bool held = false;
	uint32_t on5 = 0, max5 = 0;
	for (int i = 0; i < 200; i++) {
		run(25);
		held |= sm.view.scan_phase == SCAN_PH_HOLD;
		on5 = sm.view.zone_pos == 5 ? on5 + 25 : 0;
		max5 = on5 > max5 ? on5 : max5;
	}
	CHECK(!held && max5 > 0 && max5 <= 1300);
	req(RREQ_SCAN, RSCAN_STOP);
	hw.sig[0].hz = 0;
}

// Field report on an HR_C7000 radio (build b0b69d2): "the scan does not stop on DMR channels, even with a
// transmission". The chip keeps the slot timing of the last DMR signal it heard over a retune (kept_timing), and a
// retune to another DMR frequency did not restart the receiver: the HR_C6000 boards restart it in their own rx(), the
// HR_C7000 ones did not. Here the stop before the call carries DMR in a color code no channel has: the chip takes its
// timing there and, on the call's channel, decodes nothing until its own stale restart, up to 600 ms of the stop.
// The core now restarts DMR reception at every new frequency: the modem searches from the retune, and the call holds
// the scan by the LC's superframe after the first burst A (gotchas 17.2). With the noise reading showing the carrier
// the probe outlasted the stale timing anyway, only later; a noise reading blind to the DMR carrier left the stop at
// the dwell on every visit
// The status LED on a board that asks for it (radio-features.md 5.7): green flashes while the scan runs, solid or cyan
// once a signal holds it; the other boards keep a dark LED, and dual watch never flashes
static void test_scan_led(void)
{
	start();
	req(RREQ_SCAN, RSCAN_ZONE);
	bool lit = false;
	for (int i = 0; i < 40; i++) {
		run(25);
		lit |= hw.led[LED_GREEN];
	}
	CHECK(!lit);		// no flag: nothing changes
	req(RREQ_SCAN, RSCAN_STOP);

	board.caps |= CAP_LED_SCAN_FLASH | CAP_LED_RGB;
	start();
	req(RREQ_SCAN, RSCAN_ZONE);
	int ons = 0, offs = 0, flips = 0;
	bool was = hw.led[LED_GREEN];
	for (int i = 0; i < 80; i++) {		// 2 s: four periods
		run(25);
		bool g = hw.led[LED_GREEN];
		CHECK(g == ((sm.view.leds & RADIO_LED_GREEN) != 0) && !hw.led[LED_RED] && !hw.led[LED_BLUE]);
		ons += g;
		offs += !g;
		flips += g != was;
		was = g;
	}
	CHECK(flips >= 6 && flips <= 9 && ons >= 30 && offs >= 30);

	// Stopped: dark, and no timed output left running
	req(RREQ_SCAN, RSCAN_STOP);
	run(600);
	CHECK(sm.view.scan == SCAN_OFF && !hw.led[LED_GREEN] && !sm.led_timed);

	// A digital call that holds the scan: cyan, steady
	start();
	all_dmr();
	signal_on(0, 438500000, 30, no_tone);
	hw.sig[0].tdma = true;
	air.on = true;
	air.hz = 438500000;
	air.t0 = now + 300;
	air.dst = 9;
	air.lc_at = 1;
	req(RREQ_ZONE_POS, 6);
	req(RREQ_SCAN, RSCAN_ZONE);
	bool cyan = false;
	for (int i = 0; i < 240 && !cyan; i++) {
		run(25);
		cyan = sm.view.dmr.state == RDMR_RX && sm.view.state == RS_RX && sm.view.scan_phase == SCAN_PH_HOLD;
	}
	CHECK(cyan);
	CHECK(hw.led[LED_GREEN] && hw.led[LED_BLUE] && !hw.led[LED_RED]);
	CHECK((sm.view.leds & (RADIO_LED_GREEN | RADIO_LED_BLUE)) == (RADIO_LED_GREEN | RADIO_LED_BLUE));
	air.on = false;
	req(RREQ_SCAN, RSCAN_STOP);
	run(600);
	CHECK(!hw.led[LED_BLUE]);
	board.caps &= ~(CAP_LED_SCAN_FLASH | CAP_LED_RGB);
}

static void test_scan_dmr_kept_timing(void)
{
	// Every stop on a DMR frequency restarts the receiver; the same frequency again doesn't (a repeater's timing stays)
	start();
	all_dmr();
	req(RREQ_ZONE_POS, 0);
	unsigned r0 = sim.rx_restarts;
	req(RREQ_ZONE_POS, 1);
	CHECK(sim.rx_restarts == r0 + 1);
	req(RREQ_ZONE_POS, 1);
	CHECK(sim.rx_restarts == r0 + 1);

	for (uint8_t lc_at = 1; lc_at <= 2; lc_at++) {
		// The restart, the lock, burst A at most a superframe later, then lc_at superframes to the LC, and a poll
		uint32_t bound = AIR_LOCK_MS + 360u * (1u + lc_at) + 100u;
		for (uint32_t phase = 0; phase < 360; phase += 30) {
			start();
			all_dmr();
			signal_on(0, 438500000, 30, no_tone);
			hw.sig[0].tdma = true;
			signal_on(1, 145500000, 30, no_tone);	// the stop before: DMR in another color code
			hw.sig[1].tdma = true;
			memset(&kept, 0, sizeof(kept));
			kept.on = true;
			kept.hz = 145500000;
			air.on = true;
			air.hz = 438500000;
			air.t0 = now + 300 + phase;
			air.dst = 9;
			air.lc_at = lc_at;
			req(RREQ_ZONE_POS, 6);
			req(RREQ_SCAN, RSCAN_ZONE);
			uint32_t arrived = 0, held_ms = 0;
			bool left = false;
			for (int i = 0; i < 400 && held_ms == 0; i++) {
				run(25);
				if (sm.view.zone_pos == 5 && arrived == 0) {
					arrived = now;
				}
				left |= arrived != 0 && sm.view.zone_pos != 5;
				if (sm.view.zone_pos == 5 && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.dmr.state == RDMR_RX) {
					held_ms = now - arrived;
				}
			}
			if (held_ms == 0 || left || held_ms > bound) {
				printf("  kept-timing DMR scan: LC at F %u, phase %u ms: held %u ms after arriving, left %d\n",
				       lc_at, (unsigned)phase, (unsigned)held_ms, left);
			}
			CHECK(arrived != 0 && held_ms != 0 && !left && held_ms <= bound);
			req(RREQ_SCAN, RSCAN_STOP);
			kept.on = false;
			hw.sig[1].hz = 0;
		}
	}
	hw.sig[0].hz = 0;
}

static void test_scan_dmr_noise_deaf(void)
{
	// A receiver whose noise reading stays at its idle level on a DMR signal (gotchas 17.3): the signal strength
	// alone makes the stop probe, and the call holds the scan
	uint32_t bound = AIR_LOCK_MS + 360u * 2u + 100u;
	for (uint32_t phase = 0; phase < 360; phase += 30) {
		start();
		all_dmr();
		signal_on(0, 438500000, 120, no_tone);	// 120: the idle reading, as on an empty channel
		hw.sig[0].tdma = true;
		air.on = true;
		air.hz = 438500000;
		air.t0 = now + 300 + phase;
		air.dst = 9;
		air.lc_at = 1;
		req(RREQ_ZONE_POS, 6);
		req(RREQ_SCAN, RSCAN_ZONE);
		uint32_t arrived = 0, held_ms = 0;
		bool left = false;
		for (int i = 0; i < 400 && held_ms == 0; i++) {
			run(25);
			if (sm.view.zone_pos == 5 && arrived == 0) {
				arrived = now;
			}
			left |= arrived != 0 && sm.view.zone_pos != 5;
			if (sm.view.zone_pos == 5 && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.dmr.state == RDMR_RX) {
				held_ms = now - arrived;
			}
		}
		if (held_ms == 0 || left || held_ms > bound) {
			printf("  noise-deaf DMR scan: phase %u ms: held %u ms after arriving, left %d\n", (unsigned)phase,
			       (unsigned)held_ms, left);
		}
		CHECK(arrived != 0 && held_ms != 0 && !left && held_ms <= bound);
		req(RREQ_SCAN, RSCAN_STOP);
	}
	hw.sig[0].hz = 0;
}

// ---- Dual watch (CONFIG_DUAL_WATCH): two channels of any zone or the VFO, PTT on the selected one ----
#if CONFIG_DUAL_WATCH
static void dual(radio_dual_ref_t a, radio_dual_ref_t b, uint8_t sel)
{
	req2(RREQ_DUAL, (int32_t)radio_dual_arg(&a, sel == 0), radio_dual_arg(&b, sel != 0));
}
static void dual_stop(void) { req2(RREQ_DUAL, 0, 0); }

static bool on_ref(radio_dual_ref_t r)
{
	return r.vfo ? sm.view.vfo : !sm.view.vfo && sm.view.zone_index == r.zone && sm.view.zone_pos == r.pos;
}

// Run until the receiver is on r (at most ms); false if it never got there
static bool wait_on(radio_dual_ref_t r, uint32_t ms)
{
	for (uint32_t t = 0; t < ms && !on_ref(r); t += 25) {
		run(25);
	}
	return on_ref(r);
}

static void test_dual_watch(void)
{
	start();
	// Zone 1 "Far": position 0 ch8 Far 445.900 FM, position 1 ch5 Other
	add(8, "Far", 445900000, RADIO_MODE_FM);
	memset(&zone2, 0, sizeof(zone2));
	strcpy(zone2.name, "Far zone");
	zone2.count = 2;
	zone2.channels[0] = 8;
	zone2.channels[1] = 5;
	nzones = 2;
	const radio_dual_ref_t home = { false, 0, 0 }, far = { false, 1, 0 }, other = { false, 0, 4 };
	const radio_dual_ref_t vfo = { true, 0, 0 }, bad = { false, 1, 5 };

	// Refused: the same channel twice, a channel that isn't there, a zone that isn't there
	req(RREQ_ZONE_POS, 0);
	dual(home, home, 0);
	CHECK(sm.view.scan == SCAN_OFF);
	dual(home, bad, 0);
	CHECK(sm.view.scan == SCAN_OFF);
	dual(home, (radio_dual_ref_t){ false, 7, 0 }, 1);
	CHECK(sm.view.scan == SCAN_OFF && on_ref(home));

	// Across zones: the view reports the tuned channel's zone and position, nothing else is visited
	dual(home, far, 0);
	CHECK(sm.view.scan == SCAN_DUAL && on_ref(home));
	bool stray = false;
	int at_home = 0, at_far = 0;
	for (int i = 0; i < 120; i++) {
		run(25);
		stray |= !on_ref(home) && !on_ref(far);
		at_home += on_ref(home);
		at_far += on_ref(far);
		if (on_ref(far)) {
			CHECK(hw.rx_hz == 445900000 && strcmp(sm.view.zone_name, "Far zone") == 0);
			CHECK(sm.view.zone_size == 2);
		}
	}
	CHECK(!stray && at_home > 20 && at_far > 20 && sm.view.scan == SCAN_DUAL);

	// Asked again: the same pair changes nothing but the selection (no retune)
	CHECK(wait_on(far, 500));
	int rx = hw.rx_calls;
	dual(home, far, 0);
	dual(far, home, 1);
	CHECK(hw.rx_calls == rx && on_ref(far) && sm.dual_sel == 1);
	CHECK(!sm.dual[1].vfo && sm.dual[1].zone == 0 && sm.dual[1].pos == 0);
	dual(home, far, 0);

	// PTT with the receiver on the other channel: back to the selected one, transmit there, the watch goes on
	CHECK(wait_on(far, 500));
	req(RREQ_PTT, 1);
	CHECK(on_ref(home) && sm.view.state == RS_TX && hw.tx_hz == 146520000 && sm.view.scan == SCAN_DUAL);
	run(3000);
	CHECK(on_ref(home) && sm.view.scan == SCAN_DUAL);
	req(RREQ_PTT, 0);
	// After it: the wait on the selected channel (a reply is heard there), then the watch again
	run(850);
	CHECK(on_ref(home) && sm.view.scan == SCAN_DUAL && sm.view.scan_phase == SCAN_PH_RESUME);
	CHECK(wait_on(far, 300) && sm.view.scan == SCAN_DUAL);

	// B selected, the receiver on A: PTT retunes to B, in the other zone
	dual(home, far, 1);
	CHECK(wait_on(home, 500));
	req(RREQ_PTT, 1);
	CHECK(on_ref(far) && hw.tx_hz == 445900000 && sm.view.scan == SCAN_DUAL);
	req(RREQ_PTT, 0);
	CHECK(wait_on(home, 1300) && sm.view.scan == SCAN_DUAL);

	// A call holds the receiver on its channel, the watch goes on after the delay
	signal_on(0, 445900000, 30, no_tone);
	CHECK(wait_on(far, 500));
	run(3000);
	CHECK(on_ref(far) && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.state == RS_RX);
	hw.sig[0].hz = 0;
	run(850);
	CHECK(on_ref(far) && sm.view.scan_phase == SCAN_PH_RESUME);
	CHECK(wait_on(home, 500));

	// Stop: on the selected channel (B here), wherever the receiver was
	CHECK(wait_on(home, 500));
	dual_stop();
	CHECK(sm.view.scan == SCAN_OFF && on_ref(far));

	// A new pair keeps the receiver where it is when that channel is still one of the two
	dual(far, home, 0);
	CHECK(on_ref(far));
	dual(other, far, 0);
	CHECK(sm.view.scan == SCAN_DUAL && on_ref(far) && sm.dual_at == 1);
	// ... and starts on the selected one when it isn't
	dual(home, other, 0);
	CHECK(sm.view.scan == SCAN_DUAL && on_ref(home));

	// A channel change by the user ends the watch, from the selected channel
	dual(home, far, 0);
	CHECK(wait_on(far, 500));
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_index == 0 && sm.view.zone_pos == 1);

	// The VFO as A, with its own settings: the receiver alternates with Other
	req(RREQ_VFO_SET, 146000000);
	CHECK(sm.view.vfo);
	req(RREQ_VFO, 0);
	CHECK(!sm.view.vfo);
	dual(vfo, other, 0);
	CHECK(sm.view.scan == SCAN_DUAL && wait_on(vfo, 500) && sm.view.ch.rx_hz == 146000000 && hw.rx_hz == 146000000);
	CHECK(wait_on(other, 500) && hw.rx_hz == 145500000);
	req(RREQ_PTT, 1);
	CHECK(sm.view.vfo && hw.tx_hz == 146000000 && sm.view.scan == SCAN_DUAL);
	req(RREQ_PTT, 0);
	// The VFO as B, in the other zone's company
	dual(far, vfo, 1);
	CHECK(wait_on(far, 2500));
	// A frequency request while A is tuned changes the selected B VFO, then the watch can resume with that VFO.
	req(RREQ_VFO_SET, 146550000);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.vfo && sm.view.ch.rx_hz == 146550000 && hw.rx_hz == 146550000);
	dual(far, vfo, 1);
	CHECK(sm.view.scan == SCAN_DUAL && wait_on(far, 2500) && wait_on(vfo, 2500));
	CHECK(sm.view.ch.rx_hz == 146550000 && hw.rx_hz == 146550000);
	req(RREQ_PTT, 1);
	CHECK(sm.view.vfo && hw.tx_hz == 146550000 && sm.view.scan == SCAN_DUAL);
	req(RREQ_PTT, 0);
	CHECK(wait_on(far, 2500) && sm.view.scan == SCAN_DUAL);
	dual_stop();
	CHECK(sm.view.scan == SCAN_OFF && sm.view.vfo);

	// The stop resume mode doesn't end dual watch on a signal; it holds as carrier
	settings(set_resume_stop);
	req(RREQ_VFO, 0);
	req(RREQ_ZONE, 0);
	dual(home, far, 0);
	signal_on(0, 445900000, 30, no_tone);
	CHECK(wait_on(far, 500));
	run(1000);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.scan_phase == SCAN_PH_HOLD && on_ref(far));
	hw.sig[0].hz = 0;
	dual_stop();
	settings(set_resume_carrier);

	// DMR: the reply window after a call (a private one here) holds the selected channel; on the other one the
	// watch goes on
	const radio_dual_ref_t dmr = { false, 0, 5 };
	signal_on(0, 438500000, 30, no_tone);
	dual(dmr, home, 0);
	CHECK(wait_on(dmr, 500));
	lc_private = true;
	hear_call(9996543, 1, 1, false);
	CHECK(sm.view.dmr.state == RDMR_RX && sm.view.scan_phase == SCAN_PH_HOLD);
	hear_call(9996543, 1, 2, true);
	lc_private = false;
	hw.sig[0].hz = 0;
	CHECK(sm.view.dmr.reply_active);
	run(4000);
	CHECK(on_ref(dmr) && sm.view.dmr.reply_active);
	dual(dmr, home, 1);		// Home selected: the reply window no longer holds the DMR channel
	CHECK(wait_on(home, 2500));
	dual_stop();

	// Monitor with B (FM) selected and the receiver on A (DMR): the squelch opens on B, not digital monitor on A, and
	// the watch waits there while the key is held, then goes on
	dual(dmr, far, 1);
	CHECK(wait_on(dmr, 2500));
	bool latch = sm.hear_all_latch;
	req(RREQ_MONITOR, 1);
	CHECK(on_ref(far) && sm.view.monitor && sm.view.scan == SCAN_DUAL);
	run(3000);
	CHECK(on_ref(far) && sm.view.monitor && sm.view.scan_phase == SCAN_PH_PAUSED && hw.audio);
	req(RREQ_MONITOR, 0);
	CHECK(!sm.view.monitor && sm.hear_all_latch == latch);
	CHECK(wait_on(dmr, 2500) && sm.view.scan == SCAN_DUAL);
	// Already on the selected channel: it stays there
	CHECK(wait_on(far, 2500));
	req(RREQ_MONITOR, 1);
	run(2000);
	CHECK(on_ref(far) && sm.view.monitor);
	req(RREQ_MONITOR, 0);
	dual_stop();

	// Changes for now stay with their channel while the receiver hops: B's bandwidth (a retuning change: the UI starts
	// the watch again), and power and squelch asked for with the receiver on the other channel go to the selected one
	req(RREQ_ZONE, 0);
	dual(home, far, 1);
	CHECK(wait_on(far, 2500));
	uint8_t bw = sm.view.ch.bw == RADIO_BW_25K ? RADIO_BW_12K5 : RADIO_BW_25K;
	req(RREQ_BANDWIDTH, bw);
	CHECK(on_ref(far) && sm.view.ch.bw == bw && (sm.view.temp & RADIO_TEMP_BW));
	dual(home, far, 1);
	CHECK(sm.view.scan == SCAN_DUAL);
	CHECK(wait_on(home, 2500));
	uint8_t pw = sm.view.power;
	req(RREQ_POWER, 1);
	CHECK(on_ref(far) && sm.view.ch.bw == bw && sm.view.power != pw && sm.view.scan == SCAN_DUAL);
	uint8_t far_pw = sm.view.power;
	CHECK(wait_on(home, 2500) && sm.view.power == pw && sm.view.temp == 0);
	dual(home, far, 0);
	CHECK(wait_on(far, 2500));
	uint8_t sq = sm.view.squelch;
	req(RREQ_SQUELCH, sq > 0 ? -1 : 1);
	CHECK(on_ref(home) && sm.view.squelch != sq && sm.view.temp == RADIO_TEMP_SQUELCH);
	uint8_t home_sq = sm.view.squelch;
	for (int i = 0; i < 3; i++) {
		CHECK(wait_on(far, 2500) && sm.view.ch.bw == bw && sm.view.power == far_pw);
		CHECK(sm.view.temp == (RADIO_TEMP_BW | RADIO_TEMP_POWER) && sm.view.squelch == sq);
		CHECK(wait_on(home, 2500) && sm.view.squelch == home_sq && sm.view.temp == RADIO_TEMP_SQUELCH);
	}
	// Stopped by the user: the selected channel keeps its changes for now, the other's are gone
	dual_stop();
	CHECK(on_ref(home) && sm.view.temp == RADIO_TEMP_SQUELCH);
	dual(home, far, 0);
	CHECK(wait_on(far, 2500) && sm.view.temp == 0 && sm.view.ch.bw != bw);
	// The same for a stop over the debug link (the UI asks for nothing after it)
	dual(home, far, 1);
	req(RREQ_POWER, 1);
	CHECK(on_ref(far) && (sm.view.temp & RADIO_TEMP_POWER));
	CHECK(wait_on(home, 2500));
	req(RREQ_SCAN, RSCAN_STOP);
	CHECK(sm.view.scan != SCAN_DUAL);
	dual(home, far, 0);
	CHECK(wait_on(far, 2500) && sm.view.temp == 0);
	// A new channel in a pane doesn't inherit the old one's
	dual(home, far, 1);
	req(RREQ_POWER, 1);
	CHECK(on_ref(far) && (sm.view.temp & RADIO_TEMP_POWER));
	dual(home, other, 1);
	dual(home, far, 1);
	CHECK(wait_on(far, 2500) && sm.view.temp == 0);
	// A talk target for now on the DMR channel, chosen while the receiver is on the other: set there, kept over hops
	dual(dmr, home, 0);
	CHECK(wait_on(home, 2500));
	req(RREQ_DMR_TARGET, 2345);
	CHECK(on_ref(dmr) && sm.view.dmr.target_manual && sm.view.dmr.target_id == 2345);
	CHECK(wait_on(home, 2500) && wait_on(dmr, 2500));
	CHECK(sm.view.dmr.target_manual && sm.view.dmr.target_id == 2345);
	dual_stop();
}

// Dual watch on one receiver, the wait after a signal (dual_delay_ms): the receiver stays for the reply and looks at the
// other channel in between. A signal there is shown (view.dual_busy), never played or held, until the wait ends
static void set_dual_delay_3s(radio_settings_t *s) { s->dual_delay_ms = 3000; }
static void set_dual_delay_0(radio_settings_t *s) { s->dual_delay_ms = 0; }

// The wait after a call on far ends: how often the receiver looked at home, and whether it played or held there
static void dual_wait_watch(radio_dual_ref_t home, uint32_t ms, int *looks, bool *played, bool *held)
{
	bool was = false;
	*looks = 0;
	*played = *held = false;
	for (uint32_t t = 0; t < ms; t += 25) {
		run(25);
		bool here = on_ref(home);
		*looks += here && !was;
		*played |= here && (hw.audio || sm.view.state == RS_RX);
		*held |= here && sm.view.scan_phase == SCAN_PH_HOLD;
		was = here;
	}
}

// A call on far (445.900): held and played there, then it ends
static bool far_call(radio_dual_ref_t far)
{
	signal_on(0, 445900000, 30, no_tone);
	for (int t = 0; t < 160 && !(on_ref(far) && sm.view.scan_phase == SCAN_PH_HOLD); t++) {
		run(25);
	}
	bool ok = on_ref(far) && sm.view.scan_phase == SCAN_PH_HOLD && hw.audio;
	run(100);
	hw.sig[0].hz = 0;
	return ok;
}

static void test_dual_wait(void)
{
	start();
	add(8, "Far", 445900000, RADIO_MODE_FM);
	memset(&zone2, 0, sizeof(zone2));
	strcpy(zone2.name, "Far zone");
	zone2.count = 1;
	zone2.channels[0] = 8;
	nzones = 2;
	const radio_dual_ref_t home = { false, 0, 0 }, far = { false, 1, 0 };
	int looks;
	bool played, held;
	CHECK(sm.set.dual_delay_ms == RADIO_DUAL_DELAY_MS && sm.set.dual_delay_ms == 1000);

	// A call on far (B) ends while home (A, selected) has a signal: the looks find it, show it, don't play it
	dual(home, far, 0);
	signal_on(0, 445900000, 30, no_tone);
	CHECK(wait_on(far, 500));
	run(300);
	CHECK(on_ref(far) && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.dual_busy == 0);
	signal_on(1, 146520000, 30, no_tone);
	hw.sig[0].hz = 0;
	dual_wait_watch(home, 900, &looks, &played, &held);
	CHECK(looks >= 2 && !played && !held && sm.view.dual_busy == 1);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.scan_phase == SCAN_PH_RESUME && on_ref(far));
	// The wait ends: the watch goes to home, which holds and plays; the mark goes
	run(300);
	CHECK(on_ref(home) && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.state == RS_RX && hw.audio);
	CHECK(sm.view.dual_busy == 0);
	hw.sig[1].hz = 0;

	// Home quiet: the looks find nothing, and no mark
	CHECK(far_call(far));
	dual_wait_watch(home, 900, &looks, &played, &held);
	CHECK(looks >= 2 && sm.view.dual_busy == 0 && on_ref(far));

	// A mark from one look goes at the next look that hears nothing
	CHECK(far_call(far));
	signal_on(1, 146520000, 30, no_tone);
	for (int t = 0; t < 40 && sm.view.dual_busy == 0; t++) {
		run(25);
	}
	CHECK(sm.view.dual_busy == 1);
	hw.sig[1].hz = 0;
	for (int t = 0; t < 40 && sm.view.dual_busy != 0 && sm.view.scan_phase == SCAN_PH_RESUME; t++) {
		run(25);
	}
	CHECK(sm.view.dual_busy == 0);

	// The reply on far during the wait: back there within a look, held and played
	CHECK(far_call(far));
	for (int t = 0; t < 40 && on_ref(far); t++) {
		run(25);	// to the first look
	}
	CHECK(on_ref(home) && !hw.audio);
	signal_on(0, 445900000, 30, no_tone);
	run(300);
	CHECK(on_ref(far) && sm.view.scan_phase == SCAN_PH_HOLD && sm.view.state == RS_RX && hw.audio);
	hw.sig[0].hz = 0;

	// PTT during a look: transmits on the selected channel (home here), the look is over
	CHECK(far_call(far));
	for (int t = 0; t < 40 && on_ref(far); t++) {
		run(25);
	}
	CHECK(on_ref(home));
	req(RREQ_PTT, 1);
	CHECK(on_ref(home) && sm.view.state == RS_TX && hw.tx_hz == 146520000);
	run(500);
	req(RREQ_PTT, 0);
	// The wait after it is home's: the looks are at far
	dual_wait_watch(far, 800, &looks, &played, &held);
	CHECK(looks >= 1 && !held && on_ref(home) && sm.view.scan_phase == SCAN_PH_RESUME);

	// The setting: 3 s waits longer, with more looks
	settings(set_dual_delay_3s);
	CHECK(far_call(far));
	dual_wait_watch(home, 2700, &looks, &played, &held);
	CHECK(looks >= 7 && !played && !held && sm.view.scan_phase == SCAN_PH_RESUME);	// still waiting (maybe in a look)
	// 0: no wait, no looks: the watch goes on at once
	settings(set_dual_delay_0);
	CHECK(far_call(far));
	CHECK(wait_on(home, 200) && sm.view.scan_phase == SCAN_PH_LISTEN);
	dual_stop();
	CHECK(sm.view.scan == SCAN_OFF && sm.view.dual_busy == 0);

	// Stored with the settings (page 1), and read back: 0 is kept as 0, the default for a store without it
	settings_t st;
	settings_read(&st);
	CHECK(st.rs_dual_delay == 1);
	settings(set_dual_delay_3s);
	settings_read(&st);
	CHECK(st.rs_dual_delay == 31);
	radio_sm_init(&sm, now);
	CHECK(sm.set.dual_delay_ms == 3000);
	st.rs_dual_delay = 0;
	settings_t *w = settings_begin();
	w->rs_dual_delay = 0;
	settings_end(now);
	radio_sm_init(&sm, now);
	CHECK(sm.set.dual_delay_ms == RADIO_DUAL_DELAY_MS);
}

// What the debug link sends during the watch (radio-features.md 2.1): a position, a zone or a frequency acts on the
// selected channel; a scan stop ends the watch and says so (view.dual_stops), so the UI doesn't start it again
static void test_dual_requests(void)
{
	start();
	add(8, "Far", 445900000, RADIO_MODE_FM);
	memset(&zone2, 0, sizeof(zone2));
	strcpy(zone2.name, "Far zone");
	zone2.count = 2;
	zone2.channels[0] = 8;
	zone2.channels[1] = 5;
	nzones = 2;
	const radio_dual_ref_t home = { false, 0, 0 }, far = { false, 1, 0 };

	// B selected in the other zone, the receiver on A: a position is B's zone's
	req(RREQ_ZONE_POS, 0);
	dual(home, far, 1);
	CHECK(wait_on(home, 1500));
	req(RREQ_ZONE_POS, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_index == 1 && sm.view.zone_pos == 1);

	// A zone: from the selected channel, to that zone's first channel
	req(RREQ_ZONE, 0);
	dual(home, far, 1);
	CHECK(wait_on(home, 1500));
	req(RREQ_ZONE, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_index == 1 && sm.view.zone_pos == 0);

	// A frequency with the other channel on a VFO: the selected one gets the VFO the other isn't on
	req(RREQ_VFO_SET, 146000000);
	uint8_t other_vfo = sm.view.vfo_index;
	req(RREQ_VFO, 0);
	req(RREQ_ZONE, 0);
	const radio_dual_ref_t v = { true, 0, other_vfo };
	dual(home, v, 0);
	CHECK(sm.view.scan == SCAN_DUAL);
	req(RREQ_VFO_SET, 145000000);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.vfo && sm.view.vfo_index != other_vfo && sm.view.ch.rx_hz == 145000000);
	CHECK(sm.vfo_other.ch.rx_hz == 146000000);	// the other channel's VFO kept its frequency
	req(RREQ_VFO, 0);
	req(RREQ_ZONE, 0);

	// A scan stop ends the watch on the selected channel and counts it
	uint8_t stops = sm.view.dual_stops;
	dual(home, far, 1);
	CHECK(wait_on(home, 1500));
	req(RREQ_SCAN, RSCAN_STOP);
	CHECK(sm.view.scan == SCAN_OFF && on_ref(far) && sm.view.dual_stops == (uint8_t)(stops + 1u));
	// ... the UI's own stop (RREQ_DUAL) and a channel change don't count
	dual(home, far, 0);
	dual_stop();
	dual(home, far, 0);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.scan == SCAN_OFF && sm.view.dual_stops == (uint8_t)(stops + 1u));
	// ... nor does a stop with no watch running
	req(RREQ_SCAN, RSCAN_STOP);
	CHECK(sm.view.dual_stops == (uint8_t)(stops + 1u));
	// Cancel ends it too (on the selected channel)
	dual(home, far, 1);
	CHECK(wait_on(home, 1500));
	req(RREQ_SCAN, RSCAN_CANCEL);
	CHECK(sm.view.scan == SCAN_OFF && on_ref(far) && sm.view.dual_stops == (uint8_t)(stops + 2u));
}

// A second receiver (board.rx2): the main one stays on the selected channel, the second has the other; it falls back to
// alternating while the second one is down or won't take its channel
static struct {
	bool up, refuse, deaf, on, audio;	// deaf: takes settings but never says it's on them
	bool active;
	bool sense;				// it only senses its channel (rx2_status_t.sense)
	rx2_tune_t t;
	int tunes, stops;
} r2;
static bool r2_tune(const rx2_tune_t *t)
{
	if (!r2.up || r2.refuse) {
		return false;
	}
	if (!r2.on || memcmp(&r2.t, t, sizeof(*t)) != 0) {
		r2.tunes++;
	}
	r2.t = *t;
	r2.on = true;
	return true;
}
static void r2_stop(void) { r2.on = false; r2.audio = false; r2.stops++; }
static void r2_status(rx2_status_t *st)
{
	memset(st, 0, sizeof(*st));
	st->up = r2.up;
	st->tuned = r2.up && r2.on && !r2.deaf;
	st->active = st->tuned && r2.active;
	st->heard = st->active && r2.audio && !r2.sense;
	st->sense = r2.sense;
	st->rssi_dbm = -80;
}
static void r2_audio(bool on) { r2.audio = on; }
static const hal_rx2_t fake_rx2 = { .tune = r2_tune, .stop = r2_stop, .status = r2_status, .audio = r2_audio };

static void test_dual_rx2(void)
{
	start();
	memset(&r2, 0, sizeof(r2));
	r2.up = true;
	board.rx2 = &fake_rx2;
	const radio_dual_ref_t home = { false, 0, 0 }, tone = { false, 0, 1 }, other = { false, 0, 4 };
	const radio_dual_ref_t dmr = { false, 0, 5 };

	// Start: the main receiver on the selected channel, the second on the other, and they stay there
	dual(home, tone, 0);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.rx2 && on_ref(home) && r2.on && r2.tunes == 1);
	CHECK(r2.t.rx_hz == 146940000 && r2.t.mode == RADIO_MODE_FM && r2.t.rx_tone.type == TONE_CTCSS &&
	      r2.t.rx_tone.value == 1000 && r2.t.squelch == sm.view.squelch);
	int rx = hw.rx_calls;
	bool moved = false;
	for (int i = 0; i < 200; i++) {
		run(25);
		moved |= !on_ref(home);
	}
	CHECK(!moved && hw.rx_calls == rx && sm.view.scan == SCAN_DUAL);
	CHECK(r2.tunes == 1);		// sent again every 2 s, but nothing changed

	// Audio: the second receiver has the speaker while the selected channel is quiet; a signal there takes it
	r2.active = true;
	run(50);
	CHECK(r2.audio);
	signal_on(0, 146520000, 30, no_tone);
	run(200);
	CHECK(sm.view.state == RS_RX && hw.audio && !r2.audio && on_ref(home));
	hw.sig[0].hz = 0;
	run(300);
	CHECK(r2.audio && !hw.audio);
	// ... and PTT mutes it; the transmission is on the selected channel, with no retune
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 146520000 && !r2.audio && sm.view.scan == SCAN_DUAL && r2.on);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(r2.audio && on_ref(home) && sm.view.rx2);
	r2.active = false;

	// A new selection swaps the receivers' channels
	dual(home, tone, 1);
	CHECK(on_ref(tone) && r2.t.rx_hz == 146520000 && sm.view.rx2 && sm.dual_at == 1);
	// A new pair
	dual(other, tone, 1);
	CHECK(on_ref(tone) && r2.t.rx_hz == 145500000);

	// DMR on the second receiver: color code, timeslot, our ID, the talk group
	dual(dmr, home, 1);
	CHECK(on_ref(home) && r2.t.mode == RADIO_MODE_DMR && r2.t.rx_hz == 438500000 && r2.t.color_code == 1 &&
	      r2.t.timeslot == 1 && r2.t.own_id == 9996543 && r2.t.talk_group == 9);

	// It goes down: the main receiver alternates; back up, it is given the channel again within the retry time
	dual(home, tone, 0);
	r2.up = false;
	r2.on = false;
	run(100);
	CHECK(!sm.view.rx2 && sm.view.scan == SCAN_DUAL);
	CHECK(wait_on(tone, 1000) && wait_on(home, 1000));
	r2.up = true;
	int tunes = r2.tunes;
	run(RADIO_RX2_RETRY_MS + 200);
	CHECK(sm.view.rx2 && r2.on && r2.tunes == tunes + 1 && on_ref(home));

	// It refuses the channel: alternating at once
	r2.refuse = true;
	dual(home, other, 0);
	CHECK(!sm.view.rx2 && sm.view.scan == SCAN_DUAL);
	CHECK(wait_on(other, 1000));
	r2.refuse = false;
	run(RADIO_RX2_RETRY_MS + 200);
	CHECK(sm.view.rx2 && on_ref(home));

	// It takes the settings but never says it's on them: alternating after RADIO_RX2_TUNE_MS
	r2.deaf = true;
	dual(home, tone, 0);
	CHECK(sm.view.rx2);
	run(RADIO_RX2_TUNE_MS - 200);
	CHECK(sm.view.rx2 && on_ref(home));
	run(400);
	CHECK(!sm.view.rx2 && sm.view.scan == SCAN_DUAL);
	CHECK(wait_on(tone, 1000));
	r2.deaf = false;

	// The watch ends: the second receiver stops, audio off
	int stops = r2.stops;
	run(RADIO_RX2_RETRY_MS + 200);
	CHECK(sm.view.rx2);
	r2.active = true;
	run(100);
	CHECK(r2.audio);
	dual_stop();
	CHECK(sm.view.scan == SCAN_OFF && !sm.view.rx2 && r2.stops == stops + 1 && !r2.on && !r2.audio);

	// A user channel change ends it too
	dual(home, tone, 0);
	CHECK(sm.view.rx2);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.scan == SCAN_OFF && !sm.view.rx2 && !r2.on);
	r2.active = false;
	board.rx2 = NULL;
}

// A second receiver that only senses its DMR channel (rx2_status_t.sense): on its carrier, with the selected channel
// quiet, the main receiver goes there to decode, holds the call, and comes back after the wait
static void test_dual_rx2_sense(void)
{
	start();
	memset(&r2, 0, sizeof(r2));
	r2.up = true;
	r2.sense = true;
	board.rx2 = &fake_rx2;
	const radio_dual_ref_t home = { false, 0, 0 }, dmr = { false, 0, 5 };
	dual(dmr, home, 1);
	CHECK(sm.view.rx2 && on_ref(home) && r2.on && r2.t.mode == RADIO_MODE_DMR);
	bool moved = false;
	for (int i = 0; i < 80; i++) {
		run(25);
		moved |= !on_ref(home);
	}
	CHECK(!moved);

	// A call there: the main receiver goes, plays it and holds it
	signal_on(0, 438500000, 30, no_tone);
	hw.sig[0].tdma = true;
	memset(&air, 0, sizeof(air));
	air.on = true;
	air.hz = 438500000;
	air.t0 = now + 100;
	air.dst = 9;
	air.lc_at = 1;
	r2.active = true;
	uint32_t went = 0, played = 0, t0 = now;
	for (int i = 0; i < 160; i++) {
		run(25);
		if (went == 0 && on_ref(dmr)) {
			went = now - t0;
		}
		if (played == 0 && on_ref(dmr) && sm.view.dmr.state == RDMR_RX) {
			played = now - t0;
		}
	}
	CHECK(went != 0 && went <= 100 && played != 0 && on_ref(dmr) && sm.view.rx2 &&
	      sm.view.scan_phase == SCAN_PH_HOLD && sm.view.dmr.state == RDMR_RX);
	// It ends: back on the selected channel after the wait
	air.on = false;
	hw.sig[0].hz = 0;
	r2.active = false;
	CHECK(wait_on(home, RADIO_DUAL_DELAY_MS + 3000));
	CHECK(sm.view.rx2 && sm.view.scan == SCAN_DUAL);

	// The selected channel has a signal: it keeps the main receiver
	signal_on(0, 146520000, 30, no_tone);
	hw.sig[0].tdma = false;
	run(300);
	CHECK(sm.view.state == RS_RX && on_ref(home));
	r2.active = true;
	moved = false;
	for (int i = 0; i < 40; i++) {
		run(25);
		moved |= !on_ref(home);
	}
	CHECK(!moved);
	hw.sig[0].hz = 0;

	// A carrier with no call for us: one look, and the next only after RADIO_RX2_RELOOK_MS
	int looks = 0;
	bool was = false;
	for (uint32_t t = 0; t < RADIO_RX2_RELOOK_MS; t += 25) {
		run(25);
		looks += on_ref(dmr) && !was;
		was = on_ref(dmr);
	}
	CHECK(looks == 1 && on_ref(home));
	// ... or once its carrier dropped and came back
	r2.active = false;
	run(100);
	r2.active = true;
	CHECK(wait_on(dmr, 200));
	CHECK(wait_on(home, 4000));

	// A look that played a call: the next goes at once, though the carrier stayed up (a repeater's hang, the reply)
	r2.active = false;
	run(100);
	signal_on(0, 438500000, 30, no_tone);
	hw.sig[0].tdma = true;
	memset(&air, 0, sizeof(air));
	air.on = true;
	air.hz = 438500000;
	air.t0 = now + 100;
	air.dst = 9;
	air.lc_at = 1;
	r2.active = true;
	CHECK(wait_on(dmr, 200));
	bool call = false;
	for (int i = 0; i < 80; i++) {
		run(25);
		call |= on_ref(dmr) && sm.view.dmr.state == RDMR_RX;
	}
	CHECK(call);
	air.on = false;
	hw.sig[0].hz = 0;
	CHECK(wait_on(home, RADIO_DUAL_DELAY_MS + 3000));
	CHECK(wait_on(dmr, 200));
	CHECK(wait_on(home, 4000));

	dual_stop();
	r2.active = false;
	board.rx2 = NULL;
}
#else
// Without it: the debug link's dual watch in the current zone, which PTT ends as any scan; radio_req_dual does nothing
static void test_dual_watch(void)
{
	start();
	req(RREQ_ZONE_POS, 0);
	radio_dual_ref_t a = { false, 0, 0 }, b = { false, 0, 4 };
	req2(RREQ_DUAL, (int32_t)radio_dual_arg(&a, true), radio_dual_arg(&b, false));
	CHECK(sm.view.scan == SCAN_OFF);
	req2(RREQ_SCAN, RSCAN_DUAL, 4);
	CHECK(sm.view.scan == SCAN_DUAL);
	for (int i = 0; i < 40 && sm.view.zone_pos != 4; i++) {
		run(25);
	}
	CHECK(sm.view.zone_pos == 4);
	req(RREQ_PTT, 1);		// still searching: back to the start channel, the watch ends
	CHECK(sm.view.scan == SCAN_OFF && sm.view.zone_pos == 0 && hw.tx_hz == 146520000);
	req(RREQ_PTT, 0);
}
#endif

static void test_tot_and_beeps(void)
{
	start();
	// The codeplug's TOT (30 s on Last), the warning 10 s before with a beep on its own stream
	req(RREQ_ZONE_POS, 6);
	CHECK(sm.view.tot_s == 30);
	req(RREQ_ZONE_POS, 0);
	CHECK(sm.view.tot_s == 180);
	req(RREQ_ZONE_POS, 6);
	beep_stats_t b0, b1;
	beep_get_stats(&b0);
	req(RREQ_PTT, 1);
	run(19900);
	CHECK(sm.view.state == RS_TX && !sm.view.tot_warning);
	run(200);
	beep_get_stats(&b1);
	CHECK(sm.view.tot_warning && b1.played == b0.played + 1 && aud.starts >= 1);
	run(10000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT && !sm.view.tot_warning);
	beep_get_stats(&b0);
	CHECK(b0.played == b1.played + 1);	// the deny beep
	req(RREQ_PTT, 0);
	run(500);
	CHECK(!aud.streaming);			// the own stream stopped after the beeps
	// Settings: the default TOT for "default" channels follows, the warning can be off
	settings(set_tot);
	req(RREQ_ZONE_POS, 0);
	CHECK(sm.view.tot_s == 60);
	// Key beeps only when enabled
	beep_get_stats(&b0);
	req(RREQ_BEEP, BEEP_KEY);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played);
	req(RREQ_BEEP, BEEP_ERROR);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);
	// A beep for later (the debug link's delay, a speaker test): nothing now, once when its time comes, and a second
	// request before then replaces the first
	run(500);
	beep_get_stats(&b0);
	req2(RREQ_BEEP, BEEP_TX_DENY, 3000);
	req2(RREQ_BEEP, BEEP_ERROR, 2000);
	run(1900);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played);
	run(200);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);
	run(3000);
	beep_get_stats(&b0);
	CHECK(b0.played == b1.played);
}

static void set_tot(radio_settings_t *s) { s->tot_s = 60; s->tot_warn_s = 0; s->key_beep = false; }

// A beep's peak sample: the error beep (400 Hz, 150 ms) played out on its own stream
static int beep_peak(void)
{
	req(RREQ_BEEP, BEEP_ERROR);
	int peak = 0;
	for (int i = 0; i < 12 && aud.cb != NULL; i++) {
		aud.cb(mic, spk, NULL);
		for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++) {
			int v = spk[k] < 0 ? -spk[k] : spk[k];
			peak = v > peak ? v : peak;
		}
	}
	run(500);	// the own stream stops
	return peak;
}

static uint8_t soft_vol = 255;
static void r_volume(uint8_t percent) { soft_vol = percent; }
static int8_t p_no_knob(void) { return -1; }
static void set_beep_80(radio_settings_t *s) { s->beep_level = 80; }

// Beeps follow the soft volume up to beep_level (radio_beep_level), unless the board's volume already scales the PCM
// path (volume_pcm), and radios without the hook (an analog knob) keep beep_level as it is
static void test_beep_follows_volume(void)
{
	CHECK(radio_beep_level(50, -1) == 50 && radio_beep_level(50, 100) == 50 && radio_beep_level(50, 50) == 50);
	CHECK(radio_beep_level(50, 25) == 25 && radio_beep_level(50, 0) == 0 && radio_beep_level(0, 80) == 0);

	audio_mux_reset();	// an earlier test's stream may still count as running
	start();
	req(RREQ_ZONE_POS, 6);	// FM: the beep starts its own stream
	int analog = beep_peak();	// no volume hook: beep_level 50 whatever the volume
	CHECK(analog > 7000 && analog <= 8100);

	hal_radio_t soft_radio = fake_radio;
	hal_power_t soft_power = fake_power;
	soft_radio.volume = r_volume;
	soft_power.volume = p_no_knob;
	board.radio = &soft_radio;
	board.power = &soft_power;
	audio_mux_reset();
	start();
	req(RREQ_ZONE_POS, 6);
	settings(set_beep_80);
	req(RREQ_VOLUME, RADIO_VOLUME_MAX / 4);	// 25 %
	run(100);
	CHECK(soft_vol == 25);
	int quiet = beep_peak();
	CHECK(quiet > 3500 && quiet <= 4100);	// level 25
	req(RREQ_VOLUME, RADIO_VOLUME_MAX);
	run(100);
	int loud = beep_peak();
	CHECK(soft_vol == 100 && loud > 11500 && loud <= 12900);	// capped at beep_level 80
	beep_stats_t b0, b1;
	req(RREQ_VOLUME, 0);
	run(100);
	beep_get_stats(&b0);
	req(RREQ_BEEP, BEEP_ERROR);
	beep_get_stats(&b1);
	CHECK(soft_vol == 0 && b1.played == b0.played);	// muted: no beep
	run(500);

	soft_radio.volume_pcm = true;	// the board's volume reaches the beeps itself
	req(RREQ_VOLUME, RADIO_VOLUME_MAX / 4);
	run(100);
	int pcm = beep_peak();
	CHECK(pcm > 11500 && pcm <= 12900);

	board.radio = &fake_radio;
	board.power = &fake_power;
}

static void set_bcl_carrier(radio_settings_t *s) { s->bcl = RADIO_BCL_CARRIER; }
static void set_bcl_tone(radio_settings_t *s) { s->bcl = RADIO_BCL_WRONG_TONE; }
static void set_inhibit(radio_settings_t *s) { s->tx_inhibit = true; }

static void test_bcl_inhibit(void)
{
	start();
	settings(set_bcl_carrier);
	signal_on(0, 146520000, 30, no_tone);
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BUSY && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	hw.sig[0].hz = 0;
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	// Wrong tone: our group (CTCSS 100.0) talking is fine to talk over... rather, only another group locks out
	settings(set_bcl_tone);
	req(RREQ_ZONE_POS, 1);
	signal_on(0, 146940000, 30, (subtone_t){ TONE_CTCSS, 1230 });
	run(400);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BUSY);
	req(RREQ_PTT, 0);
	hw.sig[0].tone = (subtone_t){ TONE_CTCSS, 1000 };
	run(400);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	hw.sig[0].hz = 0;
	// Carrier mode on a channel without tone and wrong-tone mode on it: no tone, no lockout
	req(RREQ_ZONE_POS, 0);
	signal_on(0, 146520000, 30, no_tone);
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	hw.sig[0].hz = 0;
	// TX inhibit
	settings(set_inhibit);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_INHIBIT);
	req(RREQ_PTT, 0);
}

// The test TX limit (tx_limit.h): block refuses PTT as the inhibit does, a list lets only its frequencies key, off is
// no limit; the inhibit setting is never touched
static void test_tx_limit(void)
{
#if CONFIG_DEBUGLINK
	static const uint32_t pos0[] = { 146520000 }, many[TX_LIMIT_MAX + 1] = { 0 };
	tx_limit_info_t l;
	start();
	CHECK(tx_limit_set(true, NULL, 0));
	tx_limit_get(&l, NULL, 0);
	uint32_t refused = l.refused;
	int calls = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_INHIBIT && hw.tx_calls == calls);
	tx_limit_get(&l, NULL, 0);
	CHECK(l.refused == refused + 1 && l.last_hz == 146520000);	// a refused PTT counts
	req(RREQ_PTT, 0);
	CHECK(!sm.set.tx_inhibit);
	CHECK(tx_limit_set(false, pos0, 1));
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 146520000);
	req(RREQ_PTT, 0);
	req(RREQ_ZONE_POS, 1);		// 146.940: not on the list
	calls = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_INHIBIT && hw.tx_calls == calls);
	req(RREQ_PTT, 0);
	CHECK(!tx_limit_set(false, many, TX_LIMIT_MAX + 1));	// too many: refused, the list stays
	tx_limit_get(&l, NULL, 0);
	CHECK(l.on && !l.block && l.n == 1);
	CHECK(tx_limit_set(false, NULL, 0));	// off
	tx_limit_get(&l, NULL, 0);
	CHECK(!l.on);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 146940000);
	req(RREQ_PTT, 0);
	req(RREQ_ZONE_POS, 0);
#endif
}

static void set_roger(radio_settings_t *s) { s->roger_beep = true; }

// The DCS turn-off tail (hal_radio_t.tx_tail): after PTT release (and after the roger beep), the carrier stays up
// RADIO_SUBTONE_TAIL_MS; PTT again cancels it and the transmission goes on; the time-out ends it
static void set_tot5(radio_settings_t *s) { s->tot_s = 5; s->tot_warn_s = 0; }

static void test_subtone_tail(void)
{
	start();
	// No tail (no DCS on the board's side): plain unkey
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && hw.tail_starts == 0);
	hw.has_tail = true;
	req(RREQ_PTT, 1);
	run(500);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.tail_on && hw.tail_starts == 1 && hw.tx_off_calls == 1);
	radio_view_t shown = sm.view;
	radio_view_for_screen(&shown);
	CHECK(shown.state == RS_IDLE);	// the screen follows the key
	run(RADIO_SUBTONE_TAIL_MS - 50);
	CHECK(sm.view.state == RS_TX && hw.tail_on);
	run(100);
	CHECK(sm.view.state == RS_IDLE && !hw.tail_on && hw.tx_off_calls == 2);
	// Pressed again during the tail: the tail stops, the transmission goes on, and the next release tails again
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	run(100);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && !hw.tail_on && hw.tx_off_calls == 2);
	run(1000);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	CHECK(hw.tail_on && hw.tail_starts == 3);
	run(RADIO_SUBTONE_TAIL_MS + 50);
	CHECK(sm.view.state == RS_IDLE && !hw.tail_on && hw.tx_off_calls == 3);
	// After the roger beep
	settings(set_roger);
	req(RREQ_PTT, 1);
	run(200);
	req(RREQ_PTT, 0);
	CHECK(sm.view.roger && !hw.tail_on);
	run(2 * RADIO_ROGER_STEP_MS + 20);
	CHECK(sm.view.state == RS_TX && !sm.view.roger && hw.tail_on && hw.tone_hz == 0);
	run(RADIO_SUBTONE_TAIL_MS + 50);
	CHECK(sm.view.state == RS_IDLE && !hw.tail_on && hw.tx_off_calls == 4);
	// The time-out during the tail (after the roger beep, still on) ends it with the transmission
	settings(set_tot5);
	req(RREQ_PTT, 1);
	run(4600);
	req(RREQ_PTT, 0);
	run(2 * RADIO_ROGER_STEP_MS + 20);
	CHECK(sm.view.state == RS_TX && hw.tail_on);
	run(250);
	CHECK(sm.view.state != RS_TX && !hw.tail_on && hw.tx_off_calls == 5);
	run(1000);
	CHECK(sm.view.state != RS_TX && hw.tx_off_calls == 5);
}

static void test_roger_burst(void)
{
	start();
	// No roger beep by default
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	CHECK(hw.tx_off_calls == 1 && hw.tone_calls == 0);
	settings(set_roger);
	req(RREQ_PTT, 1);
	run(500);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && sm.view.roger && hw.tone_hz == RADIO_ROGER_HZ1 && hw.tx_off_calls == 1);
	run(110);
	CHECK(hw.tone_hz == RADIO_ROGER_HZ2 && sm.view.state == RS_TX);
	run(100);
	CHECK(sm.view.state == RS_IDLE && !sm.view.roger && hw.tx_off_calls == 2 && hw.tone_hz == 0);
	// Pressed again during the roger beep: the transmission continues
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	run(50);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && !sm.view.roger && hw.tone_hz == 0);
	run(500);
	CHECK(sm.view.state == RS_TX && hw.tx_off_calls == 2);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 3);
	// A board without tx_tone: plain unkey
	hw.has_tx_tone = false;
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 4);
	hw.has_tx_tone = true;

	// 1750 Hz burst from receive: keys up, sends the tone, unkeys on release with no roger beep
	int tx = hw.tx_calls;
	req(RREQ_TONE_BURST, 1);
	CHECK(sm.view.state == RS_TX && sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ && hw.tx_calls == tx + 1);
	run(1000);
	req(RREQ_TONE_BURST, 0);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tx_off_calls == 5 && hw.tone_hz == 0);
	// During PTT: the tone, then the microphone again, still on air
	req(RREQ_PTT, 1);
	req(RREQ_TONE_BURST, 1);
	CHECK(hw.tone_hz == RADIO_BURST_HZ);
	req(RREQ_TONE_BURST, 0);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(300);
	// PTT pressed during a burst takes over: releasing the burst keeps transmitting
	req(RREQ_TONE_BURST, 1);
	req(RREQ_PTT, 1);
	req(RREQ_TONE_BURST, 0);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);
	// A burst with a time limit and no "off" (the host went away): it unkeys by itself when the time is up
	int off = hw.tx_off_calls;
	req2(RREQ_TONE_BURST, 1, 2000);
	run(1800);
	CHECK(sm.view.state == RS_TX && sm.view.tone_burst);
	run(400);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tone_hz == 0 && hw.tx_off_calls == off + 1);
	req(RREQ_TONE_BURST, 0);	// the late "off": nothing more
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == off + 1);
	// The same during PTT: the tone ends, PTT keeps the transmission
	req(RREQ_PTT, 1);
	req2(RREQ_TONE_BURST, 1, 500);
	run(700);
	CHECK(sm.view.state == RS_TX && !sm.view.tone_burst && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);
	// The explicit debug-link request takes over the roger tail until burst off.
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && sm.view.roger && !sm.ptt);
	req(RREQ_TONE_BURST, 1);
	CHECK(sm.ptt && sm.burst_keyed && !sm.view.roger && sm.view.tone_burst);
	run(300);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == RADIO_BURST_HZ);
	req(RREQ_TONE_BURST, 0);
	CHECK(sm.view.state == RS_IDLE && !sm.ptt && !sm.view.tone_burst && hw.tone_hz == 0);

	// Refused explicit bursts beep without keying: DMR, hotspot, no tone hook.
	hal_radio_t radio_no_tone = fake_radio;
	radio_no_tone.tx_tone = NULL;
	for (unsigned refusal = 0; refusal < 3; refusal++) {
		start();
		if (refusal == 0) {
			req(RREQ_ZONE_POS, 5);
			run(50);
		} else if (refusal == 1) {
			sm.hotspot = true;
		} else {
			board.radio = &radio_no_tone;
		}
		beep_reset_stats();
		req(RREQ_TONE_BURST, 1);
		beep_stats_t stats;
		beep_get_stats(&stats);
		CHECK(stats.played == 1 && hw.tx_calls == 0);
		CHECK(sm.view.state != RS_TX && !sm.view.tone_burst);
		CHECK(!sm.burst_timed && !sm.burst_held && sm.burst_ms == 0 && sm.burst_end_ms == 0);
		req(RREQ_TONE_BURST, 0);
		board.radio = &fake_radio;
	}
}

// A channel with its tone burst on: each PTT key-up opens with one timed burst, then the microphone (channel_t.tone_burst)
static void test_channel_burst(void)
{
	start();
	// Off (the default): PTT keys with no tone
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && !sm.view.tone_burst && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);

	chans[1].tone_burst = true;
	req(RREQ_ZONE_POS, 0);
	run(50);
	CHECK(sm.view.ch.tone_burst);
	int tx = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && sm.ptt && sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ && hw.tx_calls == tx + 1);
	CHECK(!sm.burst_keyed);	// the PTT owns the carrier, not the burst
	run(RADIO_BURST_MIN_MS - 20);
	CHECK(sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
	run(40);
	CHECK(sm.view.state == RS_TX && sm.ptt && !sm.view.tone_burst && hw.tone_hz == 0);	// the microphone follows
	run(2000);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == 0);	// once per key-up
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);
	// Released during the burst: unkeys at once, the tone with it
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.tone_burst);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tone_hz == 0 && !sm.burst_timed);
	run(300);
	// The next key-up has it again
	req(RREQ_PTT, 1);
	CHECK(sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
	req(RREQ_PTT, 0);
	run(300);

	// The tone refused: the error beep warns that the repeater may not open, and the PTT's transmission goes on
	hw.has_tx_tone = false;
	beep_stats_t b0, b1;
	beep_get_stats(&b0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && sm.ptt && !sm.view.tone_burst);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);
	req(RREQ_PTT, 0);
	run(300);
	hw.has_tx_tone = true;

	// Host audio (USRP): the host sends its own tones, so no burst over them
	tap_hw = (typeof(tap_hw)){ .ok = true };
	int16_t pcm[AUDIO_BLOCK_SAMPLES] = { 0 };
	req(RREQ_PTT, 2);
	CHECK(sm.view.state == RS_TX && sm.host_audio && !sm.view.tone_burst && hw.tone_hz == 0);
	for (int i = 0; i < 10; i++) {
		audio_mux_host_push(pcm, AUDIO_BLOCK_SAMPLES);
		run(20);
	}
	CHECK(sm.view.state == RS_TX && !sm.view.tone_burst && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);

	// Never on DMR, whatever the record holds
	chans[6].tone_burst = true;
	req(RREQ_ZONE_POS, 5);
	run(50);
	req(RREQ_PTT, 1);
	run(50);
	CHECK(!sm.view.tone_burst && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(1000);
	chans[1].tone_burst = chans[6].tone_burst = false;
}

// The side key owns a minimum duration; the debug link still has explicit on/off control.
static void test_burst_key(void)
{
	start();
	req(RREQ_TONE_BURST_KEY, 1);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == 1750);
	run(100);
	req(RREQ_TONE_BURST_KEY, 0);
	run(RADIO_BURST_MIN_MS - 110);
	CHECK(sm.view.state == RS_TX && sm.view.tone_burst);
	run(10);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tone_hz == 0);
	CHECK(hw.tx_calls == 1 && hw.tx_off_calls == 1);

	req(RREQ_TONE_BURST_KEY, 1);
	run(3000);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == 1750);
	req(RREQ_TONE_BURST_KEY, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tone_hz == 0 && hw.tx_off_calls == 2);

	req(RREQ_PTT, 1);
	req(RREQ_TONE_BURST_KEY, 1);
	run(100);
	req(RREQ_TONE_BURST_KEY, 0);
	run(900);
	CHECK(sm.view.state == RS_TX && sm.ptt && hw.tone_hz == 0 && !sm.view.tone_burst);
	// Even with roger beep enabled, releasing PTT cuts a burst short and unkeys.
	settings(set_roger);
	req(RREQ_TONE_BURST_KEY, 1);
	run(100);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tone_hz == 0 && !sm.burst_timed);
	req(RREQ_TONE_BURST_KEY, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);

	// Taking over a burst with PTT retains the mic after the minimum duration.
	req(RREQ_TONE_BURST_KEY, 1);
	req(RREQ_PTT, 1);
	req(RREQ_TONE_BURST_KEY, 0);
	run(1000);
	CHECK(sm.view.state == RS_TX && sm.ptt && hw.tone_hz == 0);
	req(RREQ_PTT, 0);
	run(300);

	// A burst during the roger beep takes ownership of the carrier and still unkeys.
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.roger && !sm.ptt);
	req(RREQ_TONE_BURST_KEY, 1);
	CHECK(sm.burst_keyed && !sm.view.roger && hw.tone_hz == 1750);
	req(RREQ_TONE_BURST_KEY, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE && hw.tone_hz == 0);

	// A board that refuses the tone must not leave a carrier keyed.
	hw.has_tx_tone = false;
	req(RREQ_TONE_BURST_KEY, 1);
	CHECK(sm.view.state == RS_IDLE && !sm.ptt && hw.tone_hz == 0);
	req(RREQ_TONE_BURST_KEY, 0);
	hw.has_tx_tone = true;

	req(RREQ_ZONE_POS, 5);
	run(50);
	beep_reset_stats();
	int tx = hw.tx_calls;
	req(RREQ_TONE_BURST_KEY, 1);
	beep_stats_t stats;
	beep_get_stats(&stats);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && hw.tx_calls == tx && !sm.view.tone_burst);
	CHECK(stats.played == 1);
	CHECK(!sm.burst_timed && !sm.burst_held && sm.burst_ms == 0 && sm.burst_end_ms == 0);
	req(RREQ_TONE_BURST_KEY, 0);
}

static void test_burst_deadlines(void)
{
	// Every exit clears both clocks before a burst from the other source starts.
	for (unsigned side_key = 0; side_key <= 1; side_key++) {
		for (unsigned end = 0; end < 5; end++) {
			start();
			settings(set_tot5);
			uint8_t source = side_key ? RREQ_TONE_BURST_KEY : RREQ_TONE_BURST;
			req2(source, 1, end == 3 ? 10000 : 2000);
			run(100);
			CHECK(sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
			CHECK(side_key ? sm.burst_timed && sm.burst_end_ms == 0 :
				!sm.burst_timed && !sm.burst_held && sm.burst_ms == 0 && sm.burst_end_ms != 0);
			switch (end) {
			case 0: req(RREQ_TONE_BURST, 0); break;
			case 1:
				req(RREQ_PTT, 1);	// take ownership before releasing PTT
				req(RREQ_PTT, 0);
				break;
			case 2: req(RREQ_POWER_OFF, 0); break;
			case 3: run(5000); break;
			case 4:
				if (side_key) {
					req(source, 0);
				}
				run(2000);
				break;
			}
			CHECK(!sm.view.tone_burst && hw.tone_hz == 0 && hw.tx_off_calls == 1);
			CHECK(!sm.burst_timed && !sm.burst_held && sm.burst_ms == 0 && sm.burst_end_ms == 0);
			CHECK(sm.view.state == (end == 3 ? RS_TX_DENIED : RS_IDLE));
			req(source, 0);	// the release clears a time-out denial
			CHECK(sm.view.state == RS_IDLE && !sm.burst_keyed);
			if (end == 2) {
				req(RREQ_POWER_OFF, 1);
			}
			if (side_key) {
				// A host burst outlasts the side key's old minimum, then obeys max_ms.
				req2(RREQ_TONE_BURST, 1, 2000);
				run(1500);
				CHECK(sm.view.tone_burst && !sm.burst_timed && sm.burst_ms == 0);
				run(600);
			} else {
				// A held side key outlasts the old host deadline; a release after 1 s ends it.
				req(RREQ_TONE_BURST_KEY, 1);
				run(3000);
				CHECK(sm.view.tone_burst && sm.burst_timed && sm.burst_held && sm.burst_end_ms == 0);
				req(RREQ_TONE_BURST_KEY, 0);
			}
			CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tone_hz == 0);
			CHECK(!sm.burst_timed && !sm.burst_held && sm.burst_ms == 0 && sm.burst_end_ms == 0);
		}
	}

	// A second source's down/on leaves an active burst's original timing intact.
	start();
	req2(RREQ_TONE_BURST, 1, 500);
	run(100);
	uint32_t deadline = sm.burst_end_ms;
	req(RREQ_TONE_BURST_KEY, 1);
	CHECK(!sm.burst_timed && !sm.burst_held && sm.burst_end_ms == deadline);
	run(500);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst);
	req(RREQ_TONE_BURST_KEY, 0);
	req(RREQ_TONE_BURST_KEY, 1);
	run(100);
	req(RREQ_TONE_BURST_KEY, 0);
	req2(RREQ_TONE_BURST, 1, 200);
	CHECK(sm.burst_timed && !sm.burst_held && sm.burst_end_ms == 0);
	run(800);
	CHECK(sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
	run(100);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst);
}

static void test_burst_key_host_overlap(void)
{
	start();
	req2(RREQ_TONE_BURST, 1, 500);
	run(100);
	uint32_t deadline = sm.burst_end_ms;
	req(RREQ_TONE_BURST_KEY, 1);
	run(100);
	req(RREQ_TONE_BURST_KEY, 0);
	CHECK(sm.view.state == RS_TX && sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
	CHECK(sm.ptt && sm.burst_keyed && sm.burst_end_ms == deadline && hw.tx_off_calls == 0);
	run(275);
	CHECK(sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
	run(50);
	CHECK(sm.view.state == RS_IDLE && !sm.view.tone_burst && hw.tone_hz == 0);
	CHECK(!sm.ptt && hw.tx_off_calls == 1);
}

static void test_burst_key_denied(void)
{
	start();
	settings(set_inhibit);
	req(RREQ_TONE_BURST_KEY, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_INHIBIT);
	CHECK(sm.ptt && sm.burst_keyed && !sm.view.tone_burst && !sm.burst_timed);
	CHECK(hw.tx_calls == 0 && hw.tone_hz == 0);
	req(RREQ_TONE_BURST_KEY, 0);
	CHECK(sm.view.state == RS_IDLE && sm.view.tx_deny == TXDENY_NONE);
	CHECK(!sm.ptt && !sm.burst_keyed && hw.tx_calls == 0);
}

static void test_burst_power_off(void)
{
	for (unsigned held = 0; held <= 1; held++) {
		start();
		settings(set_roger);
		hw.has_tail = true;
		req(RREQ_TONE_BURST_KEY, 1);
		run(100);
		if (!held) {
			req(RREQ_TONE_BURST_KEY, 0);
		}
		CHECK(sm.view.tone_burst && sm.burst_timed);
		req(RREQ_PTT, 0);
		req(RREQ_POWER_OFF, 0);
		CHECK(sm.view.state == RS_IDLE && !sm.ptt && hw.tx_off_calls == 1);
		CHECK(!sm.view.tone_burst && !sm.view.roger && hw.tone_hz == 0 && !hw.tail_on);
		CHECK(!sm.burst_timed && !sm.burst_held && !sm.burst_keyed);
		req(RREQ_TONE_BURST_KEY, 0);
		run(3000);
		CHECK(sm.view.state == RS_IDLE && hw.tx_calls == 1 && hw.tone_hz == 0);
	}
}

static void test_burst_stray_up(void)
{
	start();
	req(RREQ_TONE_BURST_KEY, 0);
	CHECK(sm.view.state == RS_IDLE && !sm.ptt && hw.tx_calls == 0 && hw.tone_calls == 0);
	req(RREQ_PTT, 1);
	req(RREQ_TONE_BURST_KEY, 0);
	CHECK(sm.view.state == RS_TX && sm.ptt && hw.tx_off_calls == 0 && hw.tone_calls == 0);
	req(RREQ_PTT, 0);
}

static void set_vox_all(radio_settings_t *s) { s->vox = RADIO_VOX_ALL; s->vox_level = 5; s->vox_tail_ms = 1000; }
static void set_vox_channel(radio_settings_t *s) { s->vox = RADIO_VOX_CHANNEL; }
static void set_vox_off(radio_settings_t *s) { s->vox = RADIO_VOX_OFF; }

static void test_vox_burst(void)
{
	for (unsigned held = 0; held <= 1; held++) {
		start();
		settings(set_vox_all);
		hw.mic = 90;
		run(100);
		CHECK(sm.view.state == RS_TX && sm.view.vox_keyed);
		hw.mic = 0;
		run(900);	// Most of the VOX tail elapsed before the burst starts.
		req(RREQ_TONE_BURST_KEY, 1);
		run(100);
		if (!held) {
			req(RREQ_TONE_BURST_KEY, 0);
		}
		run(RADIO_BURST_MIN_MS - 110);
		CHECK(sm.view.state == RS_TX && sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
		run(10);
		if (held) {
			run(2000);
			CHECK(sm.view.state == RS_TX && sm.view.tone_burst && hw.tone_hz == RADIO_BURST_HZ);
			req(RREQ_TONE_BURST_KEY, 0);
		}
		CHECK(sm.view.state == RS_TX && sm.view.vox_keyed && !sm.view.tone_burst && hw.tone_hz == 0);
		run(sm.set.vox_tail_ms - 10);
		CHECK(sm.view.state == RS_TX && sm.view.vox_keyed);
		run(10 + RADIO_POLL_MS);
		CHECK(sm.view.state == RS_IDLE && !sm.view.vox_keyed && hw.tx_off_calls == 1);
	}
}

static void test_vox(void)
{
	start();
	hw.mic = 90;
	run(200);
	CHECK(sm.view.state == RS_IDLE);	// off by default
	settings(set_vox_all);
	hw.mic = 30;
	run(200);
	CHECK(sm.view.state == RS_IDLE);
	hw.mic = 70;				// threshold 100 - 8 x 5 = 60
	run(100);
	CHECK(sm.view.state == RS_TX && sm.view.vox_keyed);
	hw.mic = 20;
	run(900);
	CHECK(sm.view.state == RS_TX);
	hw.mic = 70;				// talking again within the tail
	run(100);
	hw.mic = 20;
	run(900);
	CHECK(sm.view.state == RS_TX);
	run(200);
	CHECK(sm.view.state == RS_IDLE && !sm.view.vox_keyed);
	// Not keyed by received audio, nor just after it
	signal_on(0, 146520000, 30, no_tone);
	hw.mic = 90;
	run(500);
	CHECK(sm.view.state == RS_RX);
	hw.sig[0].hz = 0;
	run(300);
	CHECK(sm.view.state == RS_IDLE);
	run(500);
	CHECK(sm.view.state == RS_TX);
	// PTT takes a VOX transmission over: VOX no longer unkeys it
	req(RREQ_PTT, 1);
	hw.mic = 0;
	run(3000);
	CHECK(sm.view.state == RS_TX && !sm.view.vox_keyed);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(sm.view.state == RS_IDLE);
	// Per channel: the codeplug flag decides
	settings(set_vox_channel);
	hw.mic = 90;
	run(300);
	CHECK(sm.view.state == RS_IDLE);
	hw.mic = 0;
	chans[7].vox = true;
	req(RREQ_ZONE_POS, 6);
	hw.mic = 90;
	run(300);
	CHECK(sm.view.state == RS_TX && sm.view.vox_keyed);
	// Turned off while keyed: unkeys
	settings(set_vox_off);
	CHECK(sm.view.state == RS_IDLE && !sm.view.vox_keyed);
	hw.mic = 0;
	chans[7].vox = false;
}

static void set_batt_off(radio_settings_t *s) { s->battery_off_s = 60; }

static void test_battery(void)
{
	start();
	run(1500);
	CHECK(sm.view.battery == RADIO_BATT_OK && sm.view.battery_mv == 8000 && sm.view.battery_pct == 50);
	beep_stats_t b0, b1;
	beep_get_stats(&b0);
	hw.battery_mv = 6300;
	run(3500);
	beep_get_stats(&b1);
	CHECK(sm.view.battery == RADIO_BATT_LOW && b1.played == b0.played + 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);		// low: still transmits
	// No readings while transmitting and for a while after
	hw.battery_mv = 5800;
	run(5000);
	CHECK(sm.view.battery == RADIO_BATT_LOW && sm.view.battery_mv == 6300);
	req(RREQ_PTT, 0);
	run(2500);
	CHECK(sm.view.battery_mv == 6300);
	run(4000);
	CHECK(sm.view.battery == RADIO_BATT_CRITICAL && sm.view.battery_mv == 5800);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BATTERY);
	req(RREQ_PTT, 0);
	// No switch-off by default; with battery_off_s, after that long critical
	run(70000);
	CHECK(!sm.view.power_off);
	settings(set_batt_off);
	run(2000);
	CHECK(sm.view.power_off);
	hw.battery_mv = 7800;
	run(2000);
	CHECK(!sm.view.power_off && sm.view.battery == RADIO_BATT_OK);
}

// CAP_NO_BATTERY: the monitor never reads the board, never warns, never refuses a transmission and never asks for
// power off, whatever the (here absurdly low) reading says
static void test_no_battery(void)
{
	board.caps |= CAP_NO_BATTERY;
	start();
	hw.battery_mv = 100;
	settings(set_batt_off);
	beep_stats_t b0, b1;
	beep_get_stats(&b0);
	run(80000);
	beep_get_stats(&b1);
	CHECK(sm.view.battery == RADIO_BATT_OK && sm.view.battery_mv == 0 && sm.view.battery_pct == 0);
	CHECK(b1.played == b0.played && !sm.view.power_off && sm.view.supply_mv == 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	board.caps &= ~CAP_NO_BATTERY;
}

// A supply reading (CAP_NO_BATTERY with hal_power_t.supply_mv): the view carries the reading, and nothing
// battery-only acts on it: no level, percentage, warning beep, TX refusal or power off, however low it reads
// (test_no_battery: without supply_mv the radio measures nothing)
static void test_supply_voltage(void)
{
	board.caps |= CAP_NO_BATTERY;
	board.power = &fake_supply_power;
	start();
	hw.battery_mv = 13800;
	run(3000);
	CHECK(sm.view.supply_mv == 13800 && sm.view.battery_mv == 0 && sm.view.battery_pct == 0);
	hw.battery_mv = 100;
	settings(set_batt_off);
	beep_stats_t b0, b1;
	beep_get_stats(&b0);
	run(80000);
	beep_get_stats(&b1);
	CHECK(sm.view.supply_mv == 100 && sm.view.battery == RADIO_BATT_OK && b1.played == b0.played &&
	      !sm.view.power_off);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	board.power = &fake_power;
	board.caps &= ~CAP_NO_BATTERY;
}

static void set_save(radio_settings_t *s) { s->rx_save = 2; s->rx_save_idle_s = 5; }

static void test_battery_save(void)
{
	start();
	run(20000);
	CHECK(hw.sleeps == 0);			// off by default
	settings(set_save);
	run(4000);
	CHECK(hw.sleeps == 0);			// the settings change counts as activity
	run(1200);
	CHECK(hw.sleeps >= 1);
	// Cycles: awake 125 ms, asleep 400 ms
	int s0 = hw.sleeps;
	run(5000);
	CHECK(hw.sleeps - s0 >= 9 && hw.sleeps - s0 <= 11 && hw.status_asleep == 0);
	// A signal is found at the next wake and stops the saving
	signal_on(0, 146520000, 30, no_tone);
	run(600);
	CHECK(sm.view.state == RS_RX && !hw.asleep && !sm.view.rx_sleeping);
	int s1 = hw.sleeps;
	run(3000);
	CHECK(hw.sleeps == s1);
	hw.sig[0].hz = 0;
	// A request wakes the receiver at once (the PTT tunes the transmitter anyway)
	run(8000);
	CHECK(hw.asleep || sm.view.rx_sleeping || hw.sleeps > s1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	run(50);
	CHECK(!sm.view.rx_sleeping);
	run(3000);
	int s2 = hw.sleeps;
	// No saving on a no-eco channel, nor on DMR
	chans[1].no_eco = true;
	req(RREQ_ZONE_POS, 0);
	run(10000);
	CHECK(hw.sleeps == s2);
	chans[1].no_eco = false;
	req(RREQ_ZONE_POS, 5);
	run(10000);
	CHECK(hw.sleeps == s2);
}

// A squelch open for a moment (a transmitter keying up) counts as activity: the receiver stays awake the idle time
// after it closes, so the call that follows isn't missed while it sleeps
static void test_battery_save_brief_open(void)
{
	start();
	settings(set_save);
	run(8000);
	CHECK(hw.sleeps >= 1);
	signal_on(0, 146520000, 30, no_tone);
	for (int i = 0; i < 200 && sm.view.state != RS_RX; i++) {
		run(5);
	}
	CHECK(sm.view.state == RS_RX);
	hw.sig[0].hz = 0;
	for (int i = 0; i < 200 && sm.view.state != RS_IDLE; i++) {
		run(5);
	}
	CHECK(sm.view.state == RS_IDLE);
	int s0 = hw.sleeps;
	run(4000);				// under rx_save_idle_s (5 s)
	CHECK(hw.sleeps == s0 && !sm.view.rx_sleeping);
	run(2000);
	CHECK(hw.sleeps > s0);
}

// A carrier on only one TDMA slot of two (a DMR portable: 30 ms on, 30 ms off) wakes a saving receiver within a
// cycle whatever its phase: a wake whose readings all fell in the gaps would miss it, and with a cycle a multiple of
// 60 ms every later wake too
static void set_save_short(radio_settings_t *s) { s->rx_save = 1; s->rx_save_idle_s = 3; }

static void test_battery_save_slotted(void)
{
	for (uint32_t phase = 0; phase < 60; phase += 5) {
		start();
		settings(set_save_short);
		run(8000);
		CHECK(hw.sleeps >= 1);
		uint32_t t0 = now + phase;
		uint32_t ms = 0;
		while (ms < 1200 && sm.view.state != RS_RX) {
			hw.sig[0].hz = now >= t0 && (now - t0) % 60u < 30u ? 146520000 : 0;
			if (hw.sig[0].hz != 0) {
				hw.sig[0].noise = 30;
				hw.sig[0].tone = no_tone;
			}
			run(5);
			ms += 5;
		}
		CHECK(sm.view.state == RS_RX);
		if (sm.view.state != RS_RX) {
			printf("# slotted carrier at phase %u ms: not heard in 1.2 s\n", (unsigned)phase);
		}
		hw.sig[0].hz = 0;
	}
}

static void test_dmr_beeps(void)
{
	start();
	req(RREQ_ZONE_POS, 5);
	run(50);
	beep_stats_t b0, b1;
	// A call heard and ended: the call end beep, mixed into the voice stream (no own stream started)
	hear_call(9, 1, 1, true);
	run(200);
	CHECK(ambeVoiceStreaming());
	beep_get_stats(&b0);
	CHECK(b0.played >= 1);
	int starts = aud.starts;
	// PTT: talk permit once on air
	req(RREQ_PTT, 1);
	run(600);
	beep_get_stats(&b1);
	CHECK(sm.view.dmr.state == RDMR_TX && b1.played == b0.played + 1 && aud.starts == starts);
	req(RREQ_PTT, 0);
	run(1000);
	// No-beep channel
	chans[6].no_beep = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 5);
	run(50);
	beep_get_stats(&b0);
	hear_call(9, 1, 1, true);
	run(200);
	req(RREQ_PTT, 1);
	run(600);
	req(RREQ_PTT, 0);
	run(1000);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played);
	chans[6].no_beep = false;
}

// Settings > Sound: a tone key of the codeplug's settings area at 0 silences its beeps, at 1 or absent they play
// (codeplug-settings.md 3), and the radio core takes a key written without a codeplug reload
static void set_tones_off(radio_settings_t *s) { s->tones_off = RADIO_TONES_MASK; }

static void test_tone_keys(void)
{
	beep_stats_t b0, b1;
	settings_t st;
	static const struct { uint16_t key; uint8_t beep; } rows[] = {
		{ CODEPLUG_KEY_TOT_WARN_TONE, BEEP_TOT_WARN }, { CODEPLUG_KEY_BATTERY_TONE, BEEP_BATTERY_LOW },
		{ CODEPLUG_KEY_BATTERY_TONE, BEEP_BATTERY_CRIT }, { CODEPLUG_KEY_TX_DENY_TONE, BEEP_TX_DENY },
	};

	start();
	req(RREQ_ZONE_POS, 5);
	run(50);
	// The call end tone, from a call heard
	kv_tone_set(CODEPLUG_KEY_CALL_END_TONE, 0);
	run(50);
	beep_get_stats(&b0);
	hear_call(9, 1, 1, true);
	run(200);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played);
	kv_tone_set(CODEPLUG_KEY_CALL_END_TONE, 1);
	run(3000);
	beep_get_stats(&b0);
	hear_call(9, 1, 1, true);
	run(200);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);
	kv_tone_set(CODEPLUG_KEY_CALL_END_TONE, -1);
	run(3000);
	// The others, as the radio core plays them; each key silences only its own
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		kv_tone_set(rows[i].key, 0);
		run(50);
		beep_get_stats(&b0);
		req(RREQ_BEEP, rows[i].beep);
		run(1000);
		beep_get_stats(&b1);
		CHECK(b1.played == b0.played);
		req(RREQ_BEEP, BEEP_ERROR);
		run(1000);
		beep_get_stats(&b1);
		CHECK(b1.played == b0.played + 1);
		kv_tone_set(rows[i].key, 1);
		run(50);
		beep_get_stats(&b0);
		req(RREQ_BEEP, rows[i].beep);
		run(1000);
		beep_get_stats(&b1);
		CHECK(b1.played == b0.played + 1);
		kv_tone_set(rows[i].key, -1);
	}
	// The radio's own copy (docs/spec/codeplug-settings.md 5.2) follows a key and is saved: with the key removed and
	// after a reboot the tone stays off
	kv_tone_set(CODEPLUG_KEY_TX_DENY_TONE, 0);
	run(50);
	kv_tone_set(CODEPLUG_KEY_TX_DENY_TONE, -1);
	run(50);
	settings_read(&st);
	CHECK(sm.set.tones_off == 0x08u && settings_tones_off(&st) == 0x08u);
	radio_sm_init(&sm, now);
	run(100);
	beep_get_stats(&b0);
	req(RREQ_BEEP, BEEP_TX_DENY);
	run(1000);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played && sm.set.tones_off == 0x08u);
	// A key that is present holds against a settings change made another way; an absent one lets it through
	kv_tone_set(CODEPLUG_KEY_TX_DENY_TONE, 0);
	kv_tone_set(CODEPLUG_KEY_CALL_END_TONE, 1);
	run(50);
	settings(set_tones_off);
	settings_read(&st);
	CHECK(sm.set.tones_off == (RADIO_TONES_MASK & ~0x01u) && settings_tones_off(&st) == sm.set.tones_off);
	kv_tone_set(CODEPLUG_KEY_TX_DENY_TONE, -1);
	kv_tone_set(CODEPLUG_KEY_CALL_END_TONE, -1);
}

static void set_no_permit(radio_settings_t *s) { s->talk_permit = false; }

// A board whose beeps can't sound on air (CAP_BEEP_NOT_ON_AIR): the short talk permit plays in a hold before keying,
// while the radio still receives, and none plays once on air. With the permit off it keys at once
static void test_dmr_permit_before_key(void)
{
	beep_stats_t b0, b1;
	uint32_t caps = board.caps;

	board.caps |= CAP_BEEP_NOT_ON_AIR;
	start();
	req(RREQ_ZONE_POS, 5);
	run(50);
	beep_get_stats(&b0);
	unsigned begins = sim.tx_begins;
	req(RREQ_PTT, 1);
	run(10);
	beep_get_stats(&b1);
	CHECK(sm.view.dmr.state == RDMR_TX_PERMIT && sim.tx_begins == begins && b1.played == b0.played + 1);
	run(600);
	beep_get_stats(&b1);
	CHECK(sm.view.dmr.state == RDMR_TX && sim.tx_begins == begins + 1 && b1.played == b0.played + 1);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);

	// Talk permit off (the stack is configured again): no hold, no beep
	settings(set_no_permit);
	run(50);
	beep_get_stats(&b0);
	begins = sim.tx_begins;
	req(RREQ_PTT, 1);
	run(10);
	CHECK(sm.view.dmr.state != RDMR_TX_PERMIT && sim.tx_begins == begins + 1);
	run(600);
	beep_get_stats(&b1);
	CHECK(sm.view.dmr.state == RDMR_TX && b1.played == b0.played);
	req(RREQ_PTT, 0);
	run(1000);
	board.caps = caps;
}

// A short PTT press through a repeater: the wake-up goes on after the release, and the user hears the talk permit when
// the repeater answers (the call would have gone through) or the deny tone when none ever does
static void test_dmr_short_press_wake(void)
{
	beep_stats_t b0, b1;

	start();
	chans[6].tx_hz = 443500000;	// split: through a repeater
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 5);
	run(50);
	CHECK(sm.view.dmr.rmo);
	sim.wake_repeater_on_csbk = true;
	beep_get_stats(&b0);
	req(RREQ_PTT, 1);
	run(10);
	req(RREQ_PTT, 0);
	run(2000);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);	// the talk permit
	CHECK(sm.view.state == RS_IDLE && sm.view.dmr.state == RDMR_IDLE);

	start();
	chans[6].tx_hz = 443500000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 5);
	run(50);
	sim.wake_repeater_on_csbk = false;
	beep_get_stats(&b0);
	req(RREQ_PTT, 1);
	run(10);
	req(RREQ_PTT, 0);
	run(4000);
	beep_get_stats(&b1);
	CHECK(b1.played == b0.played + 1);	// the deny tone, once
	CHECK(sm.view.state == RS_IDLE);

	chans[6].tx_hz = 438500000;
}

// DTMF on FM (radio-features.md 3.7): a key's tone lasts at least RADIO_DTMF_MIN_MS and as long as it is held, digits
// are RADIO_DTMF_GAP_MS apart, one digit waits in a rollover, and nothing sends outside an FM transmission PTT holds
static void dtmf(char d, bool down) { req(RREQ_DTMF, (uint8_t)d | (down ? 0x100 : 0)); }

static void test_dtmf(void)
{
	start();
	// Not transmitting: not ready, a key sends nothing
	CHECK(!sm.view.dtmf_ready);
	dtmf('5', true);
	dtmf('5', false);
	run(100);
	CHECK(hw.dtmf_calls == 0 && sm.view.dtmf_sent == 0);
	// PTT held on FM: ready at the next poll
	req(RREQ_PTT, 1);
	run(50);
	CHECK(sm.view.state == RS_TX && sm.view.dtmf_ready && sm.view.dtmf == 0);
	// A tap: the tone at once, held to the shortest length
	dtmf('5', true);
	CHECK(hw.dtmf == '5' && sm.view.dtmf == '5' && strcmp(sm.view.dtmf_digits, "5") == 0 && sm.view.dtmf_sent == 1);
	dtmf('5', false);
	run(RADIO_DTMF_MIN_MS - 30);
	CHECK(hw.dtmf == '5');
	run(30);
	CHECK(hw.dtmf == 0 && sm.view.dtmf == 0 && hw.dtmf_off[0] - hw.dtmf_on[0] >= RADIO_DTMF_MIN_MS &&
	      hw.dtmf_off[0] - hw.dtmf_on[0] < RADIO_DTMF_MIN_MS + RADIO_POLL_MS);
	CHECK(sm.view.state == RS_TX);
	// Held: as long as the key, ending with its release
	run(200);
	dtmf('1', true);
	run(400);
	CHECK(hw.dtmf == '1');
	dtmf('1', false);
	CHECK(hw.dtmf == 0 && hw.dtmf_off[1] - hw.dtmf_on[1] == 400);
	// The same digit again at once: it waits for the gap, so it decodes as two
	dtmf('1', true);
	CHECK(hw.dtmf == 0);
	dtmf('1', false);
	run(RADIO_DTMF_GAP_MS + RADIO_POLL_MS);
	CHECK(hw.dtmf_n == 3 && hw.dtmf_log[2] == '1' && hw.dtmf_on[2] - hw.dtmf_off[1] >= RADIO_DTMF_GAP_MS);
	run(200);
	CHECK(hw.dtmf == 0);
	// Rollover: a second key ends the first (after its shortest length) and follows after the gap; a third is dropped
	dtmf('3', true);
	run(150);
	dtmf('4', true);
	CHECK(hw.dtmf == 0);
	dtmf('6', true);
	run(RADIO_DTMF_GAP_MS + RADIO_POLL_MS);
	CHECK(hw.dtmf == '4');
	dtmf('3', false);
	dtmf('6', false);
	run(200);
	CHECK(hw.dtmf == '4');
	dtmf('4', false);
	CHECK(hw.dtmf == 0 && strcmp(sm.view.dtmf_digits, "51134") == 0 && sm.view.dtmf_sent == 5);
	// Not digits: nothing
	run(100);
	dtmf('E', true);
	dtmf('x', true);
	run(100);
	CHECK(hw.dtmf == 0 && sm.view.dtmf_sent == 5);
	// The view keeps the newest RADIO_DTMF_SHOWN
	const char *seq = "*#ABCD07";
	for (int i = 0; seq[i] != 0; i++) {
		dtmf(seq[i], true);
		dtmf(seq[i], false);
		run(RADIO_DTMF_MIN_MS + RADIO_DTMF_GAP_MS + RADIO_POLL_MS);
	}
	CHECK(strcmp(sm.view.dtmf_digits, "*#ABCD07") == 0 && sm.view.dtmf_sent == 13);
	// PTT released with a key held: the tone stops with the transmission, the digits go
	dtmf('9', true);
	CHECK(hw.dtmf == '9');
	req(RREQ_PTT, 0);
	CHECK(hw.dtmf == 0 && !sm.view.dtmf_ready && sm.view.dtmf_digits[0] == 0);
	dtmf('9', false);
	run(300);
	CHECK(sm.view.state == RS_IDLE && hw.dtmf == 0);

	// During a 1750 Hz burst: not ready; a burst starting while a digit sounds takes the air
	req(RREQ_PTT, 1);
	run(50);
	dtmf('2', true);
	CHECK(hw.dtmf == '2');
	req(RREQ_TONE_BURST, 1);
	CHECK(hw.dtmf == 0 && hw.tone_hz == RADIO_BURST_HZ);
	run(50);
	CHECK(!sm.view.dtmf_ready);
	dtmf('2', false);
	dtmf('3', true);
	run(50);
	CHECK(hw.dtmf == 0 && hw.dtmf_n == 15);
	dtmf('3', false);
	req(RREQ_TONE_BURST, 0);
	run(50);
	CHECK(sm.view.dtmf_ready);
	req(RREQ_PTT, 0);
	run(300);

	// The time-out with a key held: the tone goes with the transmitter
	settings(set_tot5);
	req(RREQ_PTT, 1);
	run(50);
	dtmf('8', true);
	run(5200);
	CHECK(sm.view.state == RS_TX_DENIED && hw.dtmf == 0 && !sm.view.dtmf_ready);
	dtmf('8', false);
	req(RREQ_PTT, 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE);

	// A DMR channel: never ready
	req(RREQ_ZONE_POS, 5);
	run(50);
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state == RS_TX && !sm.view.dtmf_ready);
	int n = hw.dtmf_n;
	dtmf('1', true);
	run(50);
	CHECK(hw.dtmf_n == n);
	dtmf('1', false);
	req(RREQ_PTT, 0);
	run(300);

	// A board without tx_dtmf: never ready
	board.radio = &fake_radio_no_dtmf;
	req(RREQ_ZONE_POS, 0);
	run(50);
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state == RS_TX && !sm.view.dtmf_ready);
	req(RREQ_PTT, 0);
	run(300);
	board.radio = &fake_radio;
}

// DTMF and the DCS turn-off tail: the board's tone generator plays either, never both. A digit held through PTT release
// stops before the tail starts; keys during the tail send nothing; PTT again during the tail brings DTMF back
static void test_dtmf_tail(void)
{
	start();
	hw.has_tail = true;
	req(RREQ_PTT, 1);
	run(50);
	dtmf('7', true);
	CHECK(hw.dtmf == '7' && sm.view.dtmf_ready);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.tail_on && hw.dtmf == 0 && !sm.view.dtmf_ready && hw.mixed == 0);
	// A key during the tail (the held one let go, another pressed): nothing sends, the tail runs out
	dtmf('7', false);
	dtmf('8', true);
	run(RADIO_SUBTONE_TAIL_MS / 2);
	CHECK(sm.view.state == RS_TX && hw.tail_on && hw.dtmf == 0 && hw.dtmf_n == 1 && !sm.view.dtmf_ready);
	dtmf('8', false);
	run(RADIO_SUBTONE_TAIL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.tail_on && hw.dtmf == 0 && hw.dtmf_n == 1 && hw.mixed == 0);
	// PTT again during the tail: the tail stops and digits send again
	req(RREQ_PTT, 1);
	run(50);
	req(RREQ_PTT, 0);
	CHECK(hw.tail_on);
	run(50);
	req(RREQ_PTT, 1);
	run(50);
	CHECK(sm.view.state == RS_TX && !hw.tail_on && sm.view.dtmf_ready);
	dtmf('9', true);
	CHECK(hw.dtmf == '9' && hw.dtmf_n == 2);
	dtmf('9', false);
	run(RADIO_DTMF_MIN_MS + RADIO_POLL_MS);
	req(RREQ_PTT, 0);
	run(RADIO_SUBTONE_TAIL_MS + 50);
	CHECK(sm.view.state == RS_IDLE && !hw.tail_on && hw.mixed == 0);
	// With the roger beep: digit off, then the beep, then the tail
	settings(set_roger);
	req(RREQ_PTT, 1);
	run(50);
	dtmf('1', true);
	req(RREQ_PTT, 0);
	CHECK(hw.dtmf == 0 && sm.view.roger && !hw.tail_on);
	dtmf('1', false);
	run(2 * RADIO_ROGER_STEP_MS + 20);
	CHECK(hw.tail_on && hw.dtmf == 0);
	run(RADIO_SUBTONE_TAIL_MS + 50);
	CHECK(sm.view.state == RS_IDLE && hw.mixed == 0);
}

// The TX time-out while a tone is on the air: a 1750 Hz burst held past it, a roger beep running into it. And
// settings out of range (another firmware's record, a host tool) clamped when they arrive
static void set_tot5_roger(radio_settings_t *s) { s->tot_s = 5; s->tot_warn_s = 0; s->roger_beep = true; }

static void test_tot_tones_and_clamps(void)
{
	start();
	settings(set_tot5_roger);
	// The burst held: cut at the time-out, the tone off with the transmitter, denied until released
	int off = hw.tx_off_calls;
	req(RREQ_TONE_BURST, 1);
	CHECK(sm.view.state == RS_TX && hw.tone_hz == RADIO_BURST_HZ);
	run(5200);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT);
	CHECK(!sm.view.tone_burst && hw.tone_hz == 0 && hw.tx_off_calls == off + 1);
	req(RREQ_TONE_BURST, 0);
	run(100);
	CHECK(sm.view.state == RS_IDLE);
	// PTT released just before the time-out: the roger beep starts and runs into it; nothing holds the deny
	req(RREQ_PTT, 1);
	run(4950);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && sm.view.roger);
	run(300);
	CHECK(sm.view.state != RS_TX && !sm.view.roger && hw.tone_hz == 0);
	run(300);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == off + 2);

	radio_settings_t r = sm.set;
	r.beep_level = 250;
	r.vox_level = 40;
	r.rx_save = 9;
	r.reply_s = 99;
	r.roam_rssi_dbm = -127;
	r.roam_hyst_db = 0;
	r.roam_probe = 7;
	r.roam_check_s = 5;
	r.roam_beacon_s = 3;
	r.roam_beacon_len_s = 0;
	r.roam_survey = 9;
	radio_sm_set_settings(&sm, &r, now);
	CHECK(sm.set.beep_level == 100 && sm.set.vox_level == 10 && sm.set.rx_save == 3 && sm.set.reply_s == 30);
	CHECK(sm.set.roam_rssi_dbm == -125 && sm.set.roam_hyst_db == 1 && sm.set.roam_probe == 1);
	CHECK(sm.set.roam_check_s == 30 && sm.set.roam_beacon_s == 10 && sm.set.roam_beacon_len_s == 1 &&
	      sm.set.roam_survey == 1);
	r.reply_s = RADIO_REPLY_CODEPLUG;
	r.roam_rssi_dbm = -20;
	r.roam_hyst_db = 90;
	r.roam_check_s = 9000;
	r.roam_beacon_s = 9000;
	r.roam_beacon_len_s = 60;
	radio_sm_set_settings(&sm, &r, now);
	CHECK(sm.set.reply_s == RADIO_REPLY_CODEPLUG && sm.set.roam_rssi_dbm == -60 && sm.set.roam_hyst_db == 30);
	CHECK(sm.set.roam_check_s == 3600 && sm.set.roam_beacon_s == 600 && sm.set.roam_beacon_len_s == 10);
}

// A host audio session's PTT (USRP, docs/spec/usb-audio.md): the board's MCU audio path instead of the microphone, no
// roger beep, and an unkey of its own when the host's audio stops coming
static void test_host_audio(void)
{
	start();
	tap_hw = (typeof(tap_hw)){ .ok = true };
	settings(set_roger);
	int16_t pcm[AUDIO_BLOCK_SAMPLES] = { 0 };
	req(RREQ_PTT, 2);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && tap_hw.calls == 1 && tap_hw.pull == audio_mux_fm_pull);
	for (int i = 0; i < 50; i++) {
		audio_mux_host_push(pcm, AUDIO_BLOCK_SAMPLES);
		run(20);
	}
	CHECK(sm.view.state == RS_TX);
	// Local PTT pressed and released meanwhile: the transmission keeps the host's source
	req(RREQ_PTT, 1);
	CHECK(sm.host_audio);
	// Released by the host: no roger beep (the host sends its own audio)
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && hw.tone_calls == 0);

	// The host's audio stops coming (its link switched away mid-transmission): unkeyed after a second
	req(RREQ_PTT, 2);
	CHECK(sm.view.state == RS_TX);
	run(AUDIO_MUX_HOST_STALE_MS - 100);
	CHECK(sm.view.state == RS_TX);
	run(200);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 2 && hw.tone_calls == 0);

	// A board that refuses the MCU audio: not keyed with the microphone instead
	tap_hw.ok = false;
	req(RREQ_PTT, 2);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_MODE && hw.tx_off_calls == 3);
	req(RREQ_PTT, 0);
	tap_hw.ok = true;

	// The local PTT afterwards is the microphone's again
	int calls = tap_hw.calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && !sm.host_audio && tap_hw.calls == calls);
	req(RREQ_PTT, 0);
	run(500);
}

int main(void)
{
	test_scan_engine();
	test_battery_module();
	test_beep_module();
	test_beep_tone();
	test_beep_sweep();
	test_vibe_module();
	test_zone_scan();
	test_scan_ptt_and_modes();
	test_vfo_scan();
	test_mixed_scan();
	test_scan_dmr_mid_call();
	test_scan_led();
	test_scan_dmr_kept_timing();
	test_scan_dmr_noise_deaf();
	test_dual_watch();
#if CONFIG_DUAL_WATCH
	test_dual_wait();
	test_dual_requests();
	test_dual_rx2();
	test_dual_rx2_sense();
#endif
	test_tot_and_beeps();
	test_beep_follows_volume();
	test_bcl_inhibit();
	test_tx_limit();
	test_roger_burst();
	test_burst_key();
	test_channel_burst();
	test_burst_deadlines();
	test_burst_key_host_overlap();
	test_burst_key_denied();
	test_burst_power_off();
	test_burst_stray_up();
	test_dtmf();
	test_subtone_tail();
	test_dtmf_tail();
	test_tot_tones_and_clamps();
	test_vox();
	test_vox_burst();
	test_battery();
	test_no_battery();
	test_supply_voltage();
	test_battery_save();
	test_battery_save_brief_open();
	test_battery_save_slotted();
	test_dmr_beeps();
	test_dmr_permit_before_key();
	test_tone_keys();
	test_dmr_short_press_wake();
	test_vibrate();
	test_host_audio();
	printf("# radioFeaturesTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
