// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's everyday features (docs/spec/radio-features.md): the scan, beep and battery modules on their own,
// then scan, TX time-out warning, busy channel lockout, roger beep, 1750 Hz burst, VOX, battery monitor, receiver
// battery saving and the DMR beeps through radio_sm.c, against a fake board whose receiver hears signals per frequency
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/scan.h"
#include "radio/beep.h"
#include "radio/battery.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "settings/settings.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake board: signals on frequencies ----
#define MAX_SIG 4
typedef struct {
	uint32_t hz;
	uint8_t noise;
	subtone_t tone;		// the sub-audio the transmitter sends
} sig_t;

static struct {
	uint32_t rx_hz;
	subtone_t rx_tone;
	int rx_calls;
	int tx_calls;
	uint32_t tx_hz;
	int tx_off_calls;
	bool audio;
	bool led[2];
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
static void r_tx_off(void) { hw.tx_off_calls++; hw.tone_hz = 0; }
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
		if (hw.sig[i].hz != 0 && hw.sig[i].hz == hw.rx_hz) {
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
	.audio = r_audio, .tx_tone = r_tx_tone, .rx_sleep = r_rx_sleep,
};

static void p_led(uint8_t which, bool on) { hw.led[which] = on; }
static uint16_t p_battery_mv(void) { return hw.battery_mv; }
static uint8_t p_battery_pct(void) { return hw.battery_mv >= 7400 ? 50 : 5; }
static uint8_t p_mic(void) { return hw.mic; }
static const hal_power_t fake_power = {
	.led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct, .mic_level = p_mic,
};

static struct {
	int starts, stops;
	bool streaming;
	bool pcm;
	audio_block_cb_t cb;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; aud.cb = cb; aud.streaming = true; aud.starts++; return true; }
static void a_stop(void) { aud.streaming = false; aud.cb = NULL; aud.stops++; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };

static bool v_present(void) { return true; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode };

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};
static const char *const power_names[] = { "L", "M", "H" };

const board_t board = {
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
	.vocoder = &fake_vocoder,
	.battery_low_mv = 6400,
	.battery_critical_mv = 6000,
};

// ---- fake codeplug: one zone of 7 channels ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zone;

codeplug_source_t codeplug_source(void) { return CODEPLUG_LEGACY; }
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
uint16_t codeplug_zone_count(void) { return 1; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i != 0) {
		return false;
	}
	*out = zone;
	return true;
}
static const contact_t contacts[] = { { "Local", 9, CONTACT_GROUP, 0 } };
uint32_t codeplug_dmr_id(void) { return 3101276; }
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
	*out = (codeplug_dmr_settings_t){ "N0CALL", 0, 0 };
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
	memset(&zone, 0, sizeof(zone));
	strcpy(zone.name, "Z");
	zone.count = 7;
	for (int i = 0; i < 7; i++) {
		zone.channels[i] = (uint16_t)(i + 1);
	}
}

static radio_sm_t sm;
static uint32_t now;

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
static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (now % 30 == 0) {
			sim_slot();
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
	CHECK(scan_poll(&s, 1025, true, true) == SCAN_ACT_NONE && s.phase == SCAN_PH_LISTEN);	// settling
	CHECK(scan_poll(&s, 1100, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 1150, false, false) == SCAN_ACT_NEXT);
	// A carrier at the end of the dwell: probe, then give up after probe_ms
	scan_tuned(&s, 2000);
	CHECK(scan_poll(&s, 2150, false, true) == SCAN_ACT_NONE && s.phase == SCAN_PH_PROBE);
	CHECK(scan_poll(&s, 2300, false, true) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 2450, false, true) == SCAN_ACT_NEXT);
	// ... or the carrier goes away
	scan_tuned(&s, 3000);
	scan_poll(&s, 3150, false, true);
	CHECK(scan_poll(&s, 3175, false, false) == SCAN_ACT_NEXT);
	// Hold on a signal, resume delay after it ends, back to hold if it returns
	scan_tuned(&s, 4000);
	CHECK(scan_poll(&s, 4075, true, true) == SCAN_ACT_NONE && s.phase == SCAN_PH_HOLD && s.holds == 1);
	CHECK(scan_poll(&s, 20000, true, true) == SCAN_ACT_NONE);	// carrier mode: no hold limit
	CHECK(scan_poll(&s, 20025, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_RESUME);
	CHECK(scan_poll(&s, 21000, true, true) == SCAN_ACT_NONE && s.phase == SCAN_PH_HOLD);
	scan_poll(&s, 21025, false, false);
	CHECK(scan_poll(&s, 23000, false, false) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 23025, false, false) == SCAN_ACT_NEXT);
	// Time mode: moves on while the signal lasts
	s.cfg.resume = SCAN_RESUME_TIME;
	scan_tuned(&s, 30000);
	scan_poll(&s, 30075, true, true);
	CHECK(scan_poll(&s, 35000, true, true) == SCAN_ACT_NONE);
	CHECK(scan_poll(&s, 35075, true, true) == SCAN_ACT_NEXT);
	// Stop mode: the first signal ends the scan
	s.cfg.resume = SCAN_RESUME_STOP;
	scan_tuned(&s, 40000);
	CHECK(scan_poll(&s, 40075, true, true) == SCAN_ACT_STOP);
	// Paused: nothing moves; resuming listens afresh
	s.cfg.resume = SCAN_RESUME_CARRIER;
	scan_tuned(&s, 50000);
	scan_pause(&s, true, 50050);
	CHECK(scan_poll(&s, 60000, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_PAUSED);
	scan_pause(&s, false, 60000);
	CHECK(scan_poll(&s, 60100, false, false) == SCAN_ACT_NONE && s.phase == SCAN_PH_LISTEN);
	CHECK(scan_poll(&s, 60150, false, false) == SCAN_ACT_NEXT);
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

	// Dual watch: alternates, falls back to the usable one
	memset(&s, 0, sizeof(s));
	cfg.priority = false;
	s.origin_pos = 0;
	s.dual_pos = 4;
	scan_begin(&s, SCAN_DUAL, &cfg, 0);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 4);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 0);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 4);
	scan_nuisance_pos(&s, 4);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 0);
	CHECK(scan_next_pos(&s, 6, all_usable, NULL) == 0);

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
		CHECK(n >= 1 && n <= BEEP_SEGS_MAX && segs[0].hz != 0);
	}

	// Level 0 or BEEP_NONE: nothing
	beep_play(BEEP_KEY, 0, false);
	beep_play(BEEP_NONE, 50, false);
	CHECK(aud.starts == 0 && !beep_busy());

	// Own stream: started, the pattern plays for its length (25 ms = 200 samples: two blocks), the speaker route
	// follows, the stream stops after the idle blocks
	beep_play(BEEP_KEY, 100, false);
	CHECK(aud.starts == 1 && aud.streaming && beep_busy());
	int16_t blk[AUDIO_BLOCK_SAMPLES];
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
	beep_service(false);
	CHECK(aud.streaming);			// not idle long enough yet
	beep_audio_block(mic, blk, NULL);
	CHECK(!aud.pcm && !beep_busy());
	beep_audio_block(mic, blk, NULL);
	beep_service(false);
	CHECK(!aud.streaming && aud.stops == 1);

	// Shared (the voice stream runs): no stream started; beep_mix adds into the block
	beep_play(BEEP_TX_PERMIT, 50, true);
	CHECK(aud.starts == 1);
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
	CHECK(st.no_stream == 1 && st.played == 3);
	beep_init(&fake_audio);
}

// ---- through the radio core ----

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

	// Dual watch: Home and Last, nothing else
	req(RREQ_ZONE_POS, 0);
	req2(RREQ_SCAN, RSCAN_DUAL, 6);
	CHECK(sm.view.scan == SCAN_DUAL && sm.view.zone_pos == 6);
	bool other = false;
	int home = 0;
	for (int i = 0; i < 120; i++) {
		run(25);
		other |= sm.view.zone_pos != 0 && sm.view.zone_pos != 6;
		home += sm.view.zone_pos == 0;
	}
	CHECK(!other && home > 20);
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

static void mk_lc(uint8_t lc[9], uint32_t dst, uint32_t src)
{
	dmr_lc_t l = { .flco = DMR_FLCO_GROUP_VOICE, .dst = dst, .src = src };
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
	mk_lc(lc, dst, 3105555);
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
}

static void set_tot(radio_settings_t *s) { s->tot_s = 60; s->tot_warn_s = 0; s->key_beep = false; }

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

static void set_roger(radio_settings_t *s) { s->roger_beep = true; }

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
	// On a DMR channel: nothing
	req(RREQ_ZONE_POS, 5);
	run(50);
	req(RREQ_TONE_BURST, 1);
	CHECK(sm.view.state != RS_TX && !sm.view.tone_burst);
	req(RREQ_TONE_BURST, 0);
}

static void set_vox_all(radio_settings_t *s) { s->vox = RADIO_VOX_ALL; s->vox_level = 5; s->vox_tail_ms = 1000; }
static void set_vox_channel(radio_settings_t *s) { s->vox = RADIO_VOX_CHANNEL; }
static void set_vox_off(radio_settings_t *s) { s->vox = RADIO_VOX_OFF; }

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
	// Cycles: awake 100 ms, asleep 400 ms
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

int main(void)
{
	test_scan_engine();
	test_battery_module();
	test_beep_module();
	test_zone_scan();
	test_scan_ptt_and_modes();
	test_vfo_scan();
	test_mixed_scan();
	test_tot_and_beeps();
	test_bcl_inhibit();
	test_roger_burst();
	test_vox();
	test_battery();
	test_battery_save();
	test_dmr_beeps();
	printf("# radioFeaturesTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
