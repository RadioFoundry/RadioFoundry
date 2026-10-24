// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's state machine (core/radio/radio_sm.c) against a fake board and a fake codeplug
#include "input/keyassign.h"
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "radio/radio_hotspot.h"
#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "settings/settings.h"
#include "dmr/dmr_data_api.h"
#include "dmr/dmr_emergency.h"
#include "ui/ui_svc.h"
#include "storage/storage.h"
#include "dmr/dmrPrivacy.h"
#include "dmr/dmrData.h"
#include "sim/sim_flash.h"
#include "radio/vibe.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake radio ----
static struct {
	int set_mode_calls;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	subtone_t rx_tone;
	int rx_calls;
	radio_bw_t tx_bw;
	subtone_t tx_tone;
	int tx_calls;
	uint32_t tx_hz;
	uint8_t tx_power;
	int tx_off_calls;
	bool audio;
	bool led[2];
	radio_status_t st;
} hw;

static void r_init(void) {}
static radio_mode_t hw_mode;
// As on the DM-1701: the modem starts only with the board in DMR mode (dm1701_dmr_enter)
static void r_set_mode(radio_mode_t m) { hw_mode = m; hw.set_mode_calls++; sim.refuse_start = m != RADIO_MODE_DMR; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	hw.rx_hz = hz;
	hw.rx_bw = bw;
	hw.rx_tone = *t;
	hw.rx_calls++;
	return true;
}
static bool tx_fail;			// the board refuses to key (synthesizer won't lock, PA fault)
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	if (tx_fail) {
		return false;
	}
	hw.tx_calls++;
	hw.tx_hz = hz;
	hw.tx_bw = bw;
	hw.tx_tone = *t;
	hw.tx_power = p;
	return true;
}
static void r_tx_off(void) { hw.tx_off_calls++; }
static void r_status(radio_status_t *st) { *st = hw.st; }
static void r_audio(bool on) { hw.audio = on; }
// FM TX tone and its Morse keying (the hotspot's CW ID): every change logged with the time
static struct {
	uint16_t hz;		// tx_tone now, 0 = the microphone
	bool down;		// tx_tone_key
	int changes;		// key edges
	uint32_t first_down_ms, last_up_ms;
	uint32_t down_ms;	// total key-down time
	uint32_t edge_ms;
} tone;
static uint32_t now;
static bool r_tx_tone(uint16_t hz) { tone.hz = hz; if (hz == 0) tone.down = false; return true; }
static bool r_tx_tone_key(bool down)
{
	if (tone.hz == 0 || down == tone.down) {
		return tone.hz != 0;
	}
	if (down) {
		if (tone.first_down_ms == 0) {
			tone.first_down_ms = now;
		}
	} else {
		tone.down_ms += now - tone.edge_ms;
		tone.last_up_ms = now;
	}
	tone.edge_ms = now;
	tone.down = down;
	tone.changes++;
	return true;
}
static char dtmf_on;		// tx_dtmf: the digit sounding
static bool r_tx_dtmf(char digit) { dtmf_on = digit; return true; }
static hal_radio_t fake_radio = { .init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off,
					.status = r_status, .audio = r_audio, .tx_tone = r_tx_tone,
					.tx_tone_key = r_tx_tone_key, .tx_dtmf = r_tx_dtmf };

// (fake_radio.volume is set by test_soft_volume only)
static void p_led(uint8_t which, bool on) { hw.led[which] = on; }

// ---- fake PCM audio and vocoder (core/dmr/ambe runs on them) ----
static struct {
	bool started;
	int starts, stops;
	bool pcm;
	bool feed;		// run() hands the callback a 20 ms microphone block, as a driver does (off: no blocks)
	audio_block_cb_t cb;
	void *ctx;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { aud.cb = cb; aud.ctx = ctx; aud.started = true; aud.starts++; return true; }
static void a_stop(void) { aud.started = false; aud.stops++; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };

static bool codec_present;
static bool v_present(void) { return codec_present; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static uint32_t v_options(void) { return VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC; }
static void v_set_options(uint32_t opts) { (void)opts; }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode, NULL, v_options, v_set_options,
					    NULL };
static const char *codec_profile;
static const char *v_profile(void) { return codec_profile; }
static uint8_t p_mic(void) { return 0; }
static hal_power_t fake_power = { .led = p_led };	// mic_level set by the VOX check, vibrate by the vibrate check
static int vibrations;
static void p_vibrate(uint16_t ms) { (void)ms; vibrations++; }

static const band_t bands[] = {
	{ 108000000, 135999999, false },	// receive only
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};
static const char *const power_names[] = { "L", "M", "H" };
// Keys: only what the side key glue (ui_svc_storage.c) needs, two side keys
static const keymap_entry_t keymap[] = {
	{ 0, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ 1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ 2, ACT_TALK_TARGET, ACT_USER_2, false, false, 0 },
};
static const uint8_t side_keys[] = { 1, 2 };

static int gps_enable_calls;
static bool gps_enabled;
static void fake_gps_enable(bool on)
{
	gps_enable_calls++;
	gps_enabled = on;
}

const board_t board = {
	.name = "fake",
	.gps_enable = fake_gps_enable,
	.bands = bands,
	.band_count = 3,
	.power_count = 3,
	.power_names = power_names,
	.power_default = 1,
	.keymap = keymap,
	.keymap_len = 3,
	.side_keys = side_keys,
	.side_key_count = 2,
	.radio = &fake_radio,
	.power = &fake_power,
	.dmr = &sim_modem,
	.audio = &fake_audio,
	.vocoder = &fake_vocoder,
	.flash = &sim_flash,		// no map until test_factory_reset: RAM only, as on a radio without one
};

// ---- fake codeplug ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zones[3];
static uint16_t nzones;
static codeplug_source_t source = CODEPLUG_OPENFW;

static uint32_t cp_serial;
codeplug_source_t codeplug_source(void) { return source; }
static uint16_t cp_quick[CODEPLUG_QUICK_KEYS];	// the quick-key slots: the side key assignment
static bool cp_quick_fail;
bool codeplug_quick_keys(uint16_t out[CODEPLUG_QUICK_KEYS]) { memcpy(out, cp_quick, sizeof(cp_quick)); return true; }
bool codeplug_set_quick_keys(const uint16_t in[CODEPLUG_QUICK_KEYS])
{
	if (cp_quick_fail) {
		return false;
	}
	memcpy(cp_quick, in, sizeof(cp_quick));
	cp_serial++;
	return true;
}
uint32_t codeplug_serial(void) { return cp_serial; }
uint16_t codeplug_channel_count(void) { return NCH; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n < 1 || n > NCH || !used[n]) {
		return false;
	}
	*out = chans[n];
	return true;
}
uint16_t codeplug_zone_count(void) { return nzones; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i >= nzones) {
		return false;
	}
	*out = zones[i];
	return true;
}

// DMR records: the radio's ID, contacts, RX group lists
static uint32_t own_dmr_id = 3101276;
static const contact_t contacts[] = {
	{ "Main Ops", 201, CONTACT_GROUP, 0 },
	{ "Joe", 3101234, CONTACT_PRIVATE, 0 },
	{ "Local", 9, CONTACT_GROUP, 0 },
};
uint32_t codeplug_dmr_id(void) { return own_dmr_id; }
void codeplug_boot_text(char line1[CODEPLUG_NAME_LEN + 1], char line2[CODEPLUG_NAME_LEN + 1]) { line1[0] = line2[0] = 0; }
bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (index < 1 || index > sizeof(contacts) / sizeof(contacts[0])) {
		return false;
	}
	*out = contacts[index - 1];
	return true;
}
static uint32_t contact_finds, iddb_lookups;
static contact_t added_contact;	// a contact written after the radio started (id 0 = none)
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	contact_finds++;
	if (added_contact.id != 0 && added_contact.id == id && (added_contact.type != CONTACT_PRIVATE) == group) {
		*out = added_contact;
		return true;
	}
	for (unsigned i = 0; i < sizeof(contacts) / sizeof(contacts[0]); i++) {
		if (contacts[i].id == id && (contacts[i].type != CONTACT_PRIVATE) == group) {
			*out = contacts[i];
			return true;
		}
	}
	if (group && id == 3100) {
		*out = (contact_t){ "TG USA", 3100, CONTACT_GROUP, 0 };
		return true;
	}
	return false;
}
static codeplug_dmr_settings_t dmr_settings = { "N0CALL", 0, 0 };	// no hang times: the stack's 10 s
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	*out = dmr_settings;
	return true;
}
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len)
{
	iddb_lookups++;
	if (id != 3105555) {
		return false;
	}
	snprintf(out, len, "KD9XYZ Robert Longname Springfield");
	return true;
}
bool codeplug_listen_list(uint16_t index, listen_list_t *out)
{
	if (index != 1 && index != 2) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (index == 2) {		// a private contact first: no talk target, the default skips it
		strcpy(out->name, "Joe first");
		out->count = 2;
		out->contacts[0] = 2;
		out->contacts[1] = 1;
		return true;
	}
	strcpy(out->name, "Local TGs");
	out->count = 3;
	out->contacts[0] = 1;
	out->contacts[1] = 2;	// private: not a listen entry, but its name is known
	out->contacts[2] = 3;
	return true;
}

static void add(uint16_t n, const char *name, uint32_t hz, uint8_t mode, bool rx_only, uint8_t power)
{
	memset(&chans[n], 0, sizeof(chans[n]));
	strcpy(chans[n].name, name);
	chans[n].rx_hz = hz;
	chans[n].tx_hz = hz;
	chans[n].mode = mode;
	chans[n].rx_only = rx_only;
	chans[n].power = power;
	used[n] = true;
}

static void setup_codeplug(void)
{
	memset(used, 0, sizeof(used));
	add(1, "Simplex", 146520000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	add(2, "Weather", 162550000, RADIO_MODE_FM, true, CODEPLUG_POWER_DEFAULT);
	// 3 is empty
	add(4, "DMR rptr", 438500000, RADIO_MODE_DMR, false, 2);
	chans[4].color_code = 2;
	chans[4].timeslot = 1;
	chans[4].contact = 1;
	chans[4].listen_list = 1;
	add(5, "Air", 121500000, RADIO_MODE_FM, false, 0);	// receive-only band
	add(6, "Other", 145500000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	// zone "Tones": a wide repeater with TX tone only, a narrow one with DCS 023 inverted both ways and different RX
	add(7, "Rptr wide", 146940000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	chans[7].tx_hz = 146340000;
	chans[7].bw = RADIO_BW_25K;
	chans[7].tx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	add(8, "Rptr DCS", 446100000, RADIO_MODE_FM, false, CODEPLUG_POWER_DEFAULT);
	chans[8].tx_tone = (subtone_t){ TONE_DCS_INV, 023 };
	chans[8].rx_tone = (subtone_t){ TONE_DCS, 0754 };
	memset(zones, 0, sizeof(zones));
	strcpy(zones[0].name, "Local");
	zones[0].count = 5;
	zones[0].channels[0] = 1;
	zones[0].channels[1] = 2;
	zones[0].channels[2] = 3;
	zones[0].channels[3] = 4;
	zones[0].channels[4] = 5;
	strcpy(zones[1].name, "Two");
	zones[1].count = 1;
	zones[1].channels[0] = 6;
	strcpy(zones[2].name, "Tones");
	zones[2].count = 2;
	zones[2].channels[0] = 7;
	zones[2].channels[1] = 8;
	nzones = 3;
}

static radio_sm_t sm;

static void req(uint8_t type, int32_t arg)
{
	radio_req_t r = { type, arg, 0 };
	radio_sm_request(&sm, &r, now);
}

static void poll_for(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
		now += RADIO_POLL_MS;
		radio_sm_poll(&sm, now);
	}
}

static void signal(uint8_t noise, bool tone_ok)
{
	hw.st.noise = noise;
	hw.st.tone_ok = tone_ok;
	hw.st.rssi_dbm = noise < 50 ? -80 : -125;
}

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

// A boot with the settings as they are (RAM only here: the fake board has no flash)
static void boot(void)
{
	ambeVoiceStop();
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	signal(120, true);
	radio_sm_init(&sm, now);
}

static void start(void)
{
	settings_init();	// nothing saved: the defaults
	ambeVoiceStop();	// a stream left from the last test
	memset(&aud, 0, sizeof(aud));
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	signal(120, true);
	now = 1000;
	radio_sm_init(&sm, now);
}

static void test_startup(void)
{
	setup_codeplug();
	start();
	CHECK(!sm.view.vfo);
	CHECK(sm.view.zone_index == 0);
	CHECK(sm.view.zone_pos == 0);
	CHECK(sm.view.zone_size == 5);
	CHECK(strcmp(sm.view.zone_name, "Local") == 0);
	CHECK(sm.view.channel_number == 1);
	CHECK(strcmp(sm.view.ch.name, "Simplex") == 0);
	CHECK(hw.set_mode_calls == 1);
	CHECK(hw.rx_hz == 146520000);
	CHECK(sm.view.power == 1);		// CODEPLUG_POWER_DEFAULT -> board.power_default
	CHECK(sm.view.squelch == RADIO_SQUELCH_DEFAULT);
	CHECK(sm.view.tot_s == 180);
	CHECK(sm.view.vfo_step_hz == 12500);
	CHECK(sm.view.source == CODEPLUG_OPENFW);
	CHECK(sm.view.state == RS_IDLE);
	CHECK(!hw.audio && !hw.led[LED_GREEN] && !hw.led[LED_RED]);
}

// The VFO's step: auto (0, the default) follows the width; a step set stays whatever the width; kept in the settings
static void test_vfo_step_auto(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.vfo_step_auto && sm.view.vfo_step_hz == 12500);
	uint32_t hz = sm.view.ch.rx_hz;
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == hz + 12500);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	CHECK(sm.view.vfo_step_auto && sm.view.vfo_step_hz == 25000);
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == hz + 37500);
	req(RREQ_VFO_STEP_SIZE, 5000);
	CHECK(!sm.view.vfo_step_auto && sm.view.vfo_step_hz == 5000);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	CHECK(sm.view.vfo_step_hz == 5000);
	settings_t st;
	settings_read(&st);
	CHECK(st.vfo_step_hz == 5000);
	req(RREQ_VFO_STEP_SIZE, 0);
	CHECK(sm.view.vfo_step_auto && sm.view.vfo_step_hz == 12500);
	settings_read(&st);
	CHECK(st.vfo_step_hz == 0);
}

static void test_channel_step(void)
{
	setup_codeplug();
	start();
	uint32_t s = sm.serial;
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 2 && hw.rx_hz == 162550000);
	CHECK(sm.serial != s);
	req(RREQ_CH_STEP, 1);			// skips the empty slot 3
	CHECK(sm.view.channel_number == 4 && sm.view.zone_pos == 3);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 5);
	req(RREQ_CH_STEP, 1);			// wraps
	CHECK(sm.view.channel_number == 1 && sm.view.zone_pos == 0);
	req(RREQ_CH_STEP, -1);			// wraps backwards
	CHECK(sm.view.channel_number == 5);
	req(RREQ_CH_STEP, -2);
	CHECK(sm.view.channel_number == 2);
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	req(RREQ_ZONE_POS, 2);			// empty: no change
	CHECK(sm.view.channel_number == 4);
	req(RREQ_ZONE, 1);
	CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6 && strcmp(sm.view.zone_name, "Two") == 0);
	req(RREQ_CH_STEP, 1);			// one channel: stays
	CHECK(sm.view.channel_number == 6);
	req(RREQ_ZONE, 7);			// no such zone
	CHECK(sm.view.zone_index == 1);
}

static void test_squelch(void)
{
	setup_codeplug();
	start();
	CHECK(radio_sm_squelch_open_below(1) == 64);
	CHECK(radio_sm_squelch_open_below(9) == 40);
	uint8_t thr = radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT);

	poll_for(100);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
	signal(thr, true);			// at the threshold: still closed
	poll_for(50);
	CHECK(sm.view.state == RS_IDLE);
	signal(thr - 10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX && hw.audio && hw.led[LED_GREEN]);
	CHECK(sm.view.rssi_dbm == -80);
	signal(thr + RADIO_SQL_HYST_DB - 1, true);	// inside the hysteresis: stays open
	poll_for(200);
	CHECK(sm.view.state == RS_RX);
	CHECK(sm.view.last_rx_ms == 0);
	signal(thr + RADIO_SQL_HYST_DB, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);		// one poll above is not enough
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !hw.led[LED_GREEN]);
	CHECK(sm.view.last_rx_ms == now);

	signal(10, false);			// no RX tone on this channel: the board's tone flag doesn't matter
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	signal(120, true);
	poll_for(100);
	CHECK(sm.view.state == RS_IDLE);

	req(RREQ_SQUELCH, -9);			// to 0: always open
	CHECK(sm.view.squelch == 0 && sm.view.temporary);
	signal(200, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX && hw.audio);
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.squelch == RADIO_SQUELCH_DEFAULT && !sm.view.temporary);
	poll_for(100);
	CHECK(sm.view.state == RS_IDLE);
	req(RREQ_SQUELCH, 20);
	CHECK(sm.view.squelch == RADIO_SQUELCH_MAX);

	// Monitor opens the speaker without changing the state
	req(RREQ_MONITOR, 1);
	CHECK(sm.view.monitor && hw.audio && sm.view.state == RS_IDLE);
	req(RREQ_MONITOR, 0);
	CHECK(!sm.view.monitor && !hw.audio);

	// A DMR channel: the state follows the call stack, not the carrier, and the FM speaker stays shut
	req(RREQ_ZONE_POS, 3);
	signal(10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
}

static void test_tx(void)
{
	setup_codeplug();
	start();
	signal(10, true);
	poll_for(50);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.tx_hz == 146520000 && hw.tx_power == 1);
	CHECK(!hw.audio && hw.led[LED_RED] && !hw.led[LED_GREEN]);
	CHECK(sm.view.last_rx_ms != 0);		// the reception ended when TX started
	req(RREQ_CH_STEP, 1);			// no retuning while transmitting
	CHECK(sm.view.channel_number == 1);
	poll_for(2000);
	CHECK(sm.view.tx_elapsed_ms == 2000);
	req(RREQ_PTT, 1);			// repeated down: nothing
	CHECK(hw.tx_calls == 1);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !hw.led[LED_RED]);
	req(RREQ_PTT, 0);
	CHECK(hw.tx_off_calls == 1);

	// RX-only channel
	req(RREQ_CH_STEP, 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == 1);
	CHECK(!hw.led[LED_RED]);
	poll_for(100);
	CHECK(sm.view.state == RS_TX_DENIED);	// until release
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && sm.view.tx_deny == TXDENY_NONE && hw.tx_off_calls == 1);

	// DMR channel without the codec: receive only
	req(RREQ_CH_STEP, 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_CODEC && hw.tx_calls == 1);
	req(RREQ_PTT, 0);

	// Receive-only band
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 5 && sm.view.power == 0);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && hw.tx_calls == 1);
	req(RREQ_PTT, 0);
}

static void test_timeout(void)
{
	setup_codeplug();
	start();
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	poll_for(179000);
	CHECK(sm.view.state == RS_TX);
	poll_for(1000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT);
	CHECK(hw.tx_off_calls == 1 && !hw.led[LED_RED]);
	poll_for(5000);
	CHECK(sm.view.state == RS_TX_DENIED);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1);
	req(RREQ_PTT, 1);			// a fresh press transmits again
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 2);
	req(RREQ_PTT, 0);
}

static void test_power(void)
{
	setup_codeplug();
	start();
	req(RREQ_POWER, 1);
	CHECK(sm.view.power == 2 && sm.view.temporary);
	req(RREQ_POWER, 1);			// wraps
	CHECK(sm.view.power == 0 && sm.view.temporary);
	req(RREQ_POWER, 1);			// back to the channel's level: not temporary
	CHECK(sm.view.power == 1 && !sm.view.temporary);
	req(RREQ_POWER, -1);
	CHECK(sm.view.power == 0 && sm.view.temporary);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_power == 0);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);			// a channel change drops the temporary change
	CHECK(sm.view.power == 1 && !sm.view.temporary);
	req(RREQ_POWER, 1);
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(sm.view.power == 1 && !sm.view.temporary);
}

static void test_vfo(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	CHECK(sm.view.vfo && sm.view.channel_number == 0 && strcmp(sm.view.ch.name, "VFO") == 0);
	CHECK(sm.view.ch.rx_hz == 146520000 && hw.rx_hz == 146520000);	// from the channel
	req(RREQ_VFO_STEP, 1);
	CHECK(sm.view.ch.rx_hz == 146532500 && sm.view.ch.tx_hz == 146532500 && hw.rx_hz == 146532500);
	req(RREQ_CH_STEP, -1);			// channel step in the VFO steps the frequency
	CHECK(sm.view.ch.rx_hz == 146520000);
	req(RREQ_VFO_SET, 174000000);
	CHECK(sm.view.ch.rx_hz == 174000000);
	req(RREQ_VFO_STEP, 1);			// past the top of the band: the next band
	CHECK(sm.view.ch.rx_hz == 400000000);
	req(RREQ_VFO_STEP, -1);
	CHECK(sm.view.ch.rx_hz == 174000000);
	req(RREQ_VFO_SET, 480000000);
	req(RREQ_VFO_STEP, 1);			// past the top band: wraps to the bottom
	CHECK(sm.view.ch.rx_hz == 108000000);
	req(RREQ_VFO_SET, 520000000);		// out of every band: refused
	CHECK(sm.view.ch.rx_hz == 108000000);
	req(RREQ_PTT, 1);			// receive-only band
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 446000000);
	req(RREQ_PTT, 0);

	req(RREQ_VFO, 0);			// back to the channel
	CHECK(!sm.view.vfo && sm.view.channel_number == 1 && hw.rx_hz == 146520000);
	req(RREQ_VFO, 1);			// the VFO remembers its frequency
	CHECK(sm.view.ch.rx_hz == 446000000);
	req(RREQ_VFO, 0);
	req(RREQ_VFO_SET, 440000000);		// typing a frequency on a channel goes to the VFO
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 440000000);
	req(RREQ_ZONE_POS, 1);			// picking a channel leaves it
	CHECK(!sm.view.vfo && sm.view.channel_number == 2);
}

static void test_no_codeplug(void)
{
	memset(used, 0, sizeof(used));
	nzones = 0;
	source = CODEPLUG_NONE;
	start();
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146520000 && hw.rx_hz == 146520000);
	CHECK(sm.view.source == CODEPLUG_NONE);
	req(RREQ_VFO, 0);			// nothing to go back to
	CHECK(sm.view.vfo);
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.ch.rx_hz == 146532500);
	source = CODEPLUG_OPENFW;

	// Zones that exist but have no readable channel
	setup_codeplug();
	memset(used, 0, sizeof(used));
	start();
	CHECK(sm.view.vfo);
}

static void test_codeplug_tones(void)
{
	setup_codeplug();
	start();
	CHECK(hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);
	req(RREQ_ZONE, 2);
	CHECK(sm.view.channel_number == 7 && hw.rx_hz == 146940000);
	CHECK(hw.rx_bw == RADIO_BW_25K && hw.rx_tone.type == TONE_NONE);	// a TX-only tone: no tone squelch
	CHECK(sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_hz == 146340000 && hw.tx_bw == RADIO_BW_25K && hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 1000);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	CHECK(hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_DCS && hw.rx_tone.value == 0754);
	CHECK(!sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_tone.type == TONE_DCS_INV && hw.tx_tone.value == 023 && hw.tx_bw == RADIO_BW_12K5);
	req(RREQ_PTT, 0);
}

static void test_tone_squelch(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE, 2);
	req(RREQ_CH_STEP, 1);			// RX DCS 754
	uint8_t thr = radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT);

	signal(thr - 20, false);		// carrier, no code: closed
	poll_for(500);
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !sm.view.tone_ok);
	signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);		// one poll of a match is not enough
	CHECK(sm.view.state == RS_IDLE);
	poll_for(RADIO_POLL_MS * (RADIO_TONE_ON_POLLS - 1));
	CHECK(sm.view.state == RS_RX && hw.audio && sm.view.tone_ok);

	// The flag chatters: short drop-outs keep the squelch open
	for (int i = 0; i < 20; i++) {
		signal(thr - 20, i % 3 != 0);
		poll_for(RADIO_POLL_MS * (1 + i % 4 % 3));
		CHECK(sm.view.state == RS_RX);
	}
	signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);
	// The tone goes away for good, carrier stays: closes after the debounce and the close count
	signal(thr - 20, false);
	poll_for(RADIO_POLL_MS * (RADIO_TONE_OFF_POLLS - 1));
	CHECK(sm.view.state == RS_RX);
	poll_for(RADIO_POLL_MS * (1 + RADIO_SQL_CLOSE_POLLS));
	CHECK(sm.view.state == RS_IDLE && !hw.audio && !sm.view.tone_ok);

	// Tone without a carrier (noise above the threshold): closed
	signal(thr + 20, true);
	poll_for(500);
	CHECK(sm.view.state == RS_IDLE && sm.view.tone_ok);
	// Carrier back with the tone: opens at once (the tone is already heard)
	signal(thr - 20, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
	// Carrier gone: closes on the noise alone
	signal(thr + 20, true);
	poll_for(RADIO_POLL_MS * RADIO_SQL_CLOSE_POLLS);
	CHECK(sm.view.state == RS_IDLE);

	// Monitor overrides tone squelch
	signal(thr - 20, false);
	poll_for(500);
	req(RREQ_MONITOR, 1);
	CHECK(hw.audio && sm.view.state == RS_IDLE);
	req(RREQ_MONITOR, 0);
	CHECK(!hw.audio);

	// Squelch level 0 with an RX tone: the tone alone decides
	req(RREQ_SQUELCH, -9);
	signal(200, false);
	poll_for(200);
	CHECK(sm.view.state == RS_IDLE);
	signal(200, true);
	poll_for(RADIO_POLL_MS * RADIO_TONE_ON_POLLS);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_CLEAR_TEMP, 0);

	// A channel without an RX tone ignores the board's tone flag
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 7 && sm.view.tone_ok);
	signal(thr - 20, false);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX);
}

static void test_temp_bw_tone(void)
{
	setup_codeplug();
	start();
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.temp == 0);
	int calls = hw.rx_calls;
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	CHECK(sm.view.ch.bw == RADIO_BW_25K && hw.rx_bw == RADIO_BW_25K && hw.rx_calls == calls + 1);
	CHECK(sm.view.temporary && sm.view.temp == RADIO_TEMP_BW);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);	// no change: no retune
	CHECK(hw.rx_calls == calls + 1);
	req(RREQ_BANDWIDTH, 7);			// not a bandwidth
	CHECK(sm.view.ch.bw == RADIO_BW_25K);

	// One tone for both directions
	subtone_t t100 = { TONE_CTCSS, 1000 };
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.rx_tone.value == 1000);
	CHECK(hw.rx_tone.type == TONE_CTCSS && hw.rx_tone.value == 1000);
	CHECK(sm.view.temp == (RADIO_TEMP_BW | RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE));
	CHECK(!sm.view.tone_ok);
	req(RREQ_PTT, 1);
	CHECK(hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 1000 && hw.tx_bw == RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_NONE, 0 }));	// not while transmitting
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	CHECK(sm.view.ch.bw == RADIO_BW_25K);
	req(RREQ_PTT, 0);

	// TX only: no retune
	calls = hw.rx_calls;
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_DCS, 023 }));
	CHECK(sm.view.ch.tx_tone.type == TONE_DCS && sm.view.ch.rx_tone.type == TONE_CTCSS && hw.rx_calls == calls);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_DCS_INV, 023 }));
	CHECK(hw.rx_tone.type == TONE_DCS_INV && hw.rx_tone.value == 023 && hw.rx_calls == calls + 1);
	// Invalid tones are refused
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_CTCSS, 50 }));
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ 9, 1000 }));
	CHECK(sm.view.ch.tx_tone.type == TONE_DCS && sm.view.ch.rx_tone.type == TONE_DCS_INV);
	// Setting the channel's own value again clears that flag
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, (subtone_t){ TONE_NONE, 0 }));
	CHECK(sm.view.temp == RADIO_TEMP_BW);

	// BACK on Home undoes everything and retunes
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	calls = hw.rx_calls;
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(!sm.view.temporary && sm.view.temp == 0 && sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.rx_tone.type == TONE_NONE);
	CHECK(hw.rx_calls == calls + 1 && hw.rx_bw == RADIO_BW_12K5 && hw.rx_tone.type == TONE_NONE);

	// Undo during TX: the receiver is retuned when PTT is released
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_PTT, 1);
	calls = hw.rx_calls;
	req(RREQ_CLEAR_TEMP, 0);
	CHECK(hw.rx_calls == calls && sm.view.ch.bw == RADIO_BW_12K5);
	req(RREQ_PTT, 0);
	CHECK(hw.rx_calls == calls + 1 && hw.rx_bw == RADIO_BW_12K5);

	// A channel change drops them
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	CHECK(sm.view.temp == 0 && sm.view.ch.tx_tone.type == TONE_NONE && hw.rx_tone.type == TONE_NONE);

	// DMR channel: refused
	req(RREQ_ZONE_POS, 3);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_BOTH, t100));
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.tx_tone.type == TONE_NONE && sm.view.temp == 0);

	// VFO: its own settings, not temporary, remembered
	req(RREQ_ZONE_POS, 0);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);	// temporary on the channel
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.temp == 0);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 885 }));
	CHECK(sm.view.ch.bw == RADIO_BW_25K && sm.view.ch.tx_tone.value == 885 && !sm.view.temporary);
	CHECK(hw.rx_bw == RADIO_BW_25K && hw.rx_tone.type == TONE_NONE);
	req(RREQ_VFO, 0);
	CHECK(sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.tx_tone.type == TONE_NONE && hw.rx_bw == RADIO_BW_12K5);
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.bw == RADIO_BW_25K && sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 885);
	CHECK(hw.rx_bw == RADIO_BW_25K);
}

static void test_tone_lists(void)
{
	// Spot checks and order
	CHECK(tones_ctcss[0] == 670 && tones_ctcss[12] == 1000 && tones_ctcss[TONES_CTCSS_COUNT - 1] == 2541);
	CHECK(tones_dcs[0] == 023 && tones_dcs[TONES_DCS_COUNT - 1] == 0754);
	for (int i = 1; i < TONES_CTCSS_COUNT; i++)
		CHECK(tones_ctcss[i] > tones_ctcss[i - 1]);
	for (int i = 1; i < TONES_DCS_COUNT; i++)
		CHECK(tones_dcs[i] > tones_dcs[i - 1]);

	// Stepping visits every picker entry once and wraps
	subtone_t t = { TONE_NONE, 0 };
	for (int i = 1; i <= TONES_PICKER_COUNT; i++) {
		t = tones_step(&t, 1);
		subtone_t want = tones_picker((uint16_t)(i % TONES_PICKER_COUNT));
		CHECK(t.type == want.type && t.value == want.value);
		CHECK(tones_standard(&t));
	}
	t = tones_step(&t, -1);
	CHECK(t.type == TONE_DCS_INV && t.value == 0754);
	t = tones_step(&(subtone_t){ TONE_CTCSS, 670 }, -1);
	CHECK(t.type == TONE_NONE);
	t = tones_step(&(subtone_t){ TONE_CTCSS, 2541 }, 1);
	CHECK(t.type == TONE_DCS && t.value == 023);
	t = tones_step(&(subtone_t){ TONE_DCS, 0754 }, 1);
	CHECK(t.type == TONE_DCS_INV && t.value == 023);

	// A non-standard tone steps to its neighbors
	subtone_t odd = { TONE_CTCSS, 1500 };
	CHECK(!tones_standard(&odd) && tones_valid(&odd));
	t = tones_step(&odd, 1);
	CHECK(t.type == TONE_CTCSS && t.value == 1514);
	t = tones_step(&odd, -1);
	CHECK(t.type == TONE_CTCSS && t.value == 1462);
	CHECK(!tones_valid(&(subtone_t){ TONE_CTCSS, 2700 }));
	CHECK(!tones_valid(&(subtone_t){ TONE_DCS, 01000 }));
}

// ---- DMR: the call stack on a simulated modem, driven like the radio task drives it ----

// Advance in 5 ms steps: a modem slot every 30 ms, the stack tick every RADIO_TICK_MS, the status poll every
// RADIO_POLL_MS
static int16_t burst_rssi = -85;	// what the simulated modem reports per burst (0 = it doesn't)
static bool rssi_alt;			// the fake radio's reading alternates strong / floor every 30 ms (TDMA)

static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		if (rssi_alt) {
			hw.st.rssi_dbm = (now / 30) % 2 ? -125 : -70;
		}
		if (now % 30 == 0) {
			sim_slot();
		}
		if (aud.feed && aud.started && aud.cb != NULL && now % 20 == 0) {
			static const int16_t mic[AUDIO_BLOCK_SAMPLES];
			int16_t spk[AUDIO_BLOCK_SAMPLES];
			aud.cb(mic, spk, aud.ctx);
		}
		if (now % RADIO_TICK_MS == 0) {
			radio_sm_tick(&sm, now);
		}
		if (now % RADIO_POLL_MS == 0) {
			radio_sm_poll(&sm, now);
		}
	}
}

static void mk_lc(uint8_t lc[9], bool group, uint32_t dst, uint32_t src)
{
	dmr_lc_t l = { .flco = group ? DMR_FLCO_GROUP_VOICE : DMR_FLCO_UNIT_VOICE, .dst = dst, .src = src };
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
	b.rssi_dbm = burst_rssi;
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

// A call heard: three headers, superframes of voice, a terminator if end
static void hear_call(bool group, uint32_t dst, uint32_t src, uint8_t cc, int superframes, bool end)
{
	uint8_t lc[9];
	mk_lc(lc, group, dst, src);
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

static void dmr_start(bool codec)
{
	setup_codeplug();
	codec_present = codec;
	start();
	req(RREQ_ZONE_POS, 3);		// channel 4: DMR, CC 2, TS 1, talk target TG 201, listen list 1
	run(50);
}

static void test_dmr_rx(void)
{
	dmr_start(false);
	CHECK(sim.started && sim.cc_filter == 2 && sim.cfg.own_id == 3101276 && !sim.cfg.repeater);
	const radio_dmr_view_t *d = &sm.view.dmr;
	CHECK(d->active && !d->codec && d->own_id == 3101276);
	CHECK(d->target_id == 201 && !d->target_private && strcmp(d->target_name, "Main Ops") == 0);
	CHECK(d->cc == 2 && d->ts == 1 && !d->rmo && d->state == RDMR_IDLE && d->last_ms == 0);

	// A group call on the talk target: received, shown, played (silence without the codec)
	hear_call(true, 201, 3101234, 2, 1, false);
	CHECK(sm.view.state == RS_RX && d->state == RDMR_RX);
	CHECK(d->call.src == 3101234 && d->call.dst == 201 && d->call.group && strcmp(d->call.dst_name, "Main Ops") == 0);
	CHECK(strcmp(d->call.src_name, "Joe") == 0);	// a private contact in the listen list
	CHECK(d->rssi_dbm == -85);
	CHECK(hw.led[LED_GREEN] && !hw.audio);		// FM speaker path stays off: DMR voice is PCM
	CHECK(aud.started);
	ambe_voice_stats_t vs;
	ambeVoiceGetStats(&vs);
	CHECK(vs.rx_frames == 0 && vs.rx_lost == 0);	// the fake audio callback never runs; frames were queued
	burst_lc((uint8_t[9]){ 0 }, 2, true);
	uint8_t lc[9];
	mk_lc(lc, true, 201, 3101234);
	burst_lc(lc, 2, true);
	run(100);
	CHECK(sm.view.state == RS_IDLE && d->state == RDMR_IDLE && !hw.led[LED_GREEN]);
	CHECK(d->last_ms != 0 && d->last.src == 3101234 && d->last.dst == 201 && sm.view.last_rx_ms != 0);
	CHECK(!d->reply_active);	// the talk target's own talkgroup: no reply window

	// TG 9 (in the listen list) opens the reply window to TG 9; BACK (RREQ_REPLY_CANCEL) ends it
	hear_call(true, 9, 3105555, 2, 1, true);
	CHECK(d->reply_active && d->target_id == 9 && d->reply_s == 10 && strcmp(d->target_name, "Local") == 0);
	run(2000);
	CHECK(d->reply_active && d->reply_s == 8);
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	CHECK(!d->reply_active && d->target_id == 201);

	// A talkgroup outside the list: shown as busy, not played
	hear_call(true, 3100, 3107777, 2, 1, false);
	CHECK(sm.view.state == RS_RX && d->state == RDMR_RX_OTHER && d->call.dst == 3100 && !hw.led[LED_GREEN]);
	run(500);			// fades out
	CHECK(sm.view.state == RS_IDLE);
	// MONITOR on DMR: a short press toggles digital monitor (the hear all latch) on its release (#19)
	req(RREQ_MONITOR, 1);
	CHECK(!d->hear_all_latch && !sm.view.monitor);
	req(RREQ_MONITOR, 0);
	CHECK(d->hear_all && d->hear_all_latch && !sm.view.monitor && !hw.audio);
	hear_call(true, 3100, 3107777, 2, 1, true);
	CHECK(d->last.dst == 3100);
	// Released after its hold function ran: no toggle
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 2);
	CHECK(d->hear_all_latch);
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(!d->hear_all && !d->hear_all_latch);
	// Another color code: ignored
	hear_call(true, 201, 3101234, 5, 1, false);
	CHECK(sm.view.state == RS_IDLE);
	run(500);

	// Without the codec PTT is refused before anything is keyed
	int tx = hw.tx_calls;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_CODEC && hw.tx_calls == tx);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);

	// Leaving DMR stops the stack and the PCM stream
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started && !d->active && aud.stops > 0 && !aud.started);
	req(RREQ_CH_STEP, -1);
	CHECK(sim.started && d->active);
}

static void test_dmr_tx(void)
{
	dmr_start(true);
	CHECK(sm.view.dmr.codec);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.tx_hz == 438500000 && hw.tx_power == 2);
	CHECK(sim.tx_begins == 1 && sim.tx_active);
	run(600);
	CHECK(sm.view.dmr.state == RDMR_TX && hw.led[LED_RED] && sim.air_count >= 6);
	CHECK(sim.air[0].kind == DMR_BURST_VOICE_LC_HEADER && sim.air[3].kind == DMR_BURST_VOICE);
	CHECK(sm.view.dmr.call.dst == 201 && sm.view.dmr.call.src == 3101276);
	req(RREQ_CH_STEP, 1);		// no retuning on air
	CHECK(sm.view.channel_number == 4);

	// Release: on air until the superframe ends and the modem reports TX done, then tx_off once
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_TX && hw.tx_off_calls == 0);
	bool early = false;
	for (int i = 0; i < 100 && sm.view.state == RS_TX; i++) {
		run(10);
		if (hw.tx_off_calls != 0 && sim.tx_active) {
			early = true;
		}
	}
	CHECK(!early && sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !sim.tx_active && !hw.led[LED_RED]);
	CHECK(sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);
	run(100);
	CHECK(hw.tx_off_calls == 1);

	// Pressed again while the last one is ending: one arming, keeps going
	req(RREQ_PTT, 1);
	run(400);
	req(RREQ_PTT, 0);
	run(10);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 2);
	run(600);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 2);
}

// A modem with no slot timing takes the transmission and never sends (MD-380, HR_C5000 not answering): the radio must
// not stay keyed. Released: back to receive with the arming dropped; held: refused until release
static void test_dmr_stalled(void)
{
	dmr_start(true);
	aud.feed = true;	// the encoder fills voice bursts, so the stack gets past the headers as on the radio
	sim.stalled = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && sim.tx_begins == 1);
	run(400);
	req(RREQ_PTT, 0);
	run(DMR_CALL_TX_STALL_MS);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 1 && !sim.tx_active && !hw.led[LED_RED]);
	CHECK(sim.tx_aborts == 1 && sim.air_count == 0);

	req(RREQ_PTT, 1);
	run(DMR_CALL_TX_STALL_MS + 100);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REJECTED && hw.tx_off_calls == 2);
	req(RREQ_PTT, 0);
	run(20);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == 2);
	sim.stalled = false;
	aud.feed = false;
}

// A channel with no TX contact talks to the first talkgroup of its RX group list (issue #4: qdmr's open-firmware
// encoder writes a channel's contact or its list, never both); talkgroup 9 only with neither; a contact always wins
static void test_dmr_list_talk_target(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);
	CHECK(d->target_id == 201);	// channel 4's contact, Main Ops
	chans[4].contact = 0;		// no contact, list 1: Main Ops first
	chans[4].listen_list = 1;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 201 && !d->target_private && strcmp(d->target_name, "Main Ops") == 0);

	chans[4].listen_list = 2;	// Joe (private) first: skipped
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 201 && !d->target_private);

	chans[4].listen_list = 0;	// neither: talkgroup 9
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 9 && !d->target_private);

	chans[4].contact = 3;		// a contact wins over the list
	chans[4].listen_list = 1;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(d->target_id == 9 && strcmp(d->target_name, "Local") == 0);

	// What PTT sends: the list's talkgroup, as a group call
	chans[4].contact = 0;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && sm.view.dmr.call.dst == 201 && sm.view.dmr.call.group);
	req(RREQ_PTT, 0);
	run(1000);
	setup_codeplug();
}

#if !CONFIG_VFO_DMR
// Without the DMR VFO (CONFIG_VFO_DMR=0) the VFO is analog and every DMR request in it is refused
static void test_vfo_no_dmr(void)
{
	dmr_start(true);
	req(RREQ_VFO_SET, 446000000);
	req(RREQ_MODE, RADIO_MODE_DMR);
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, 3100);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && hw_mode == RADIO_MODE_FM);
	CHECK(sm.vfo_ch.color_code == 1 && sm.vfo_ch.timeslot == 1 && sm.vfo_talk_id == 0 && !sm.view.temporary);
	CHECK(!ui_svc_save_talk_target(3100, false));
	boot();				// a record that says DMR (another build wrote it): this one starts analog
	settings_t *st = settings_begin();
	st->vfo_mode = RADIO_MODE_DMR;
	settings_set_vfo_talk(st, 3100, false);
	settings_end(now);
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started);
	req(RREQ_VFO_STEP, 1);		// and saves leave the talk target as the other build wrote it
	st = settings_begin();
	CHECK(settings_vfo_talk_id(st) == 3100);
	settings_end(now);
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#else
// The DMR VFO (CONFIG_VFO_DMR): mode, color code, slot and talk target are the VFO's own settings, kept across
// restarts, and receive and transmit follow them like a DMR channel's
static void test_vfo_dmr(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);			// channel 4: DMR, CC 2, TS 1, TG 201
	req(RREQ_VFO_SET, 446000000);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && hw_mode == RADIO_MODE_FM);
	// FM -> DMR on the same frequency: the call stack starts, the board's mode follows, no temporary dot
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 446000000);
	CHECK(!sm.view.temporary && sm.view.temp == 0 && d->active && d->own_id == 3101276);
	CHECK(d->cc == 1 && d->ts == 1 && d->target_id == 9 && !d->target_private && !d->target_manual);
	CHECK(hw.rx_tone.type == TONE_NONE);
	// Receive filtering as a DMR channel with no RX group list: the talk target's group (9) and nothing else
	hear_call(true, 9, 3105555, 1, 1, true);
	CHECK(d->last_ms != 0 && d->last.dst == 9 && d->last.src == 3105555);
	hear_call(true, 3100, 3107777, 1, 1, false);
	CHECK(d->state == RDMR_RX_OTHER);
	run(500);
	hear_call(true, 9, 3105555, 5, 1, false);	// another color code: not heard
	CHECK(sm.view.state == RS_IDLE);
	run(500);

	// Color code, slot and a typed talk target
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, 3100);
	run(50);
	CHECK(d->cc == 7 && d->ts == 2 && sm.view.ch.color_code == 7 && sm.view.ch.timeslot == 2);
	CHECK(!sm.view.temporary && sm.vfo_ch.color_code == 7 && sm.vfo_ch.timeslot == 2);
	CHECK(d->target_id == 3100 && !d->target_private && !d->target_manual && sim.cc_filter == 7);
	hear_call(true, 3100, 3107777, 7, 1, true);
	CHECK(d->last.dst == 3100 && d->last.src == 3107777);
	// The debug link's one request for mode, color code and slot (RADIO op 31), 0xFF / 0 = keep
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 9, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(d->cc == 9 && d->ts == 2 && sm.vfo_ch.color_code == 9);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF, 1 }, 3) == DL_ST_OK);
	run(20);
	CHECK(d->cc == 9 && d->ts == 1);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0, 0xFF, 0 }, 3) == DL_ST_OK);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 1, 7, 2 }, 3) == DL_ST_OK);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->ts == 2);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 2, 0xFF, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 16, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF, 3 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DMR_FIELDS, (const uint8_t[]){ 0xFF, 0xFF }, 2) == DL_ST_BAD_ARG);
	CHECK(debuglink_is_feature_op(DL_OP_DMR_FIELDS));
	// PTT sends the typed target from our own ID
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 446000000 && d->call.dst == 3100 && d->call.group &&
	      d->call.src == 3101276);
	req(RREQ_PTT, 0);
	run(1000);
	// A private target
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 3107777u));
	run(50);
	CHECK(d->target_id == 3107777 && d->target_private && !d->target_manual);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && d->call.dst == 3107777 && !d->call.group);
	req(RREQ_PTT, 0);
	run(1000);
	// A call back is for now: the VFO's target is still there after it is cleared
	req(RREQ_DMR_TARGET, 3100);
	hear_call(false, 3101276, 3105555, 7, 1, true);
	req(RREQ_DMR_CALL_BACK, 0);
	CHECK(d->target_manual && d->target_id == 3105555 && d->target_private);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	run(20);
	CHECK(!d->target_manual && d->target_id == 3100 && !d->target_private);

	// Back to FM and again: the stack stops and starts, the settings stay
	req(RREQ_MODE, RADIO_MODE_FM);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sim.started && !d->active && hw_mode == RADIO_MODE_FM);
	req(RREQ_DMR_CC, 3);			// color code only in DMR
	CHECK(sm.vfo_ch.color_code == 7);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sim.started && d->cc == 7 && d->ts == 2 && d->target_id == 3100);

	// A restart: the VFO comes back in DMR with all of it
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 3107777u));
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw.rx_hz == 446000000);
	CHECK(d->cc == 7 && d->ts == 2 && d->target_id == 3107777 && d->target_private);
	// The channel is not touched by any of it
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.channel_number == 4 && d->cc == 2 && d->ts == 1 && d->target_id == 201 &&
	      !sm.view.temporary);
	req(RREQ_ZONE_POS, 0);			// an FM channel
	run(20);
	CHECK(!sim.started && sm.view.ch.mode == RADIO_MODE_FM);
	req(RREQ_VFO, 1);			// the VFO is still a DMR one
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->target_id == 3107777);
	req(RREQ_VFO_STEP, 1);			// stepping the frequency retunes the DMR VFO
	run(50);
	CHECK(sm.view.ch.rx_hz == 446012500 && hw.rx_hz == 446012500 && sim.started && d->cc == 7);

	// A record from before the DMR VFO (mode FM, the old color code and slot bytes, no target) reads as FM
	settings_t *st = settings_begin();
	st->vfo_mode = RADIO_MODE_FM;
	st->vfo_cc = 1;
	st->vfo_ts = 1;
	st->vfo_bw &= 0x0Fu;			// the old bandwidth byte: a plain radio_bw_t
	st->vfo_talk_lo = 0;
	settings_end(now);
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && !sim.started && sm.view.ch.color_code == 1);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sim.started && d->cc == 1 && d->ts == 1 && d->target_id == 9);
	// An ID out of range in a damaged record is dropped
	st = settings_begin();
	st->vfo_cc = 0xF1;
	st->vfo_ts = 0x3D;			// bits 2-5: 0xF; the ID is 0xFF0000 here, a valid 24 bit number
	st->vfo_talk_lo = 0;
	settings_end(now);
	boot();
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(d->cc == 1 && d->ts == 1 && d->target_id == 0xFF0000u && !d->target_private);
	// Leave the VFO the way the other tests expect it
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#endif

static void reload_channel(void);

// Copy to VFO (RREQ_VFO_COPY, docs/spec/dmr-voice.md 2.2): what is in effect on the channel becomes the VFO's own
// settings, the VFO opens, the channel stays as it was
static void test_vfo_copy(void)
{
	setup_codeplug();
	start();
	// An FM channel with an offset and a TX tone (channel 7, zone "Tones"); temporary bandwidth, RX tone and power
	req(RREQ_ZONE, 2);
	run(20);
	CHECK(sm.view.channel_number == 7 && !sm.view.vfo);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_CTCSS, 885 }));
	req(RREQ_POWER, 1);			// the channel's default is 1: this is the high step
	CHECK(sm.view.temporary && sm.view.power == 2);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.channel_number == 0 && !sm.view.temporary && sm.view.temp == 0);
	CHECK(sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_hz == 146340000 && hw.rx_hz == 146940000);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.ch.bw == RADIO_BW_12K5);
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 1000);
	CHECK(sm.view.ch.rx_tone.type == TONE_CTCSS && sm.view.ch.rx_tone.value == 885 && hw.rx_tone.value == 885);
	CHECK(sm.view.power == 2 && sm.vfo_ch.power == 2);
	CHECK(strcmp(sm.view.ch.name, "VFO") == 0);
	req(RREQ_PTT, 1);			// the split: TX on the channel's TX frequency
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 146340000 && hw.tx_power == 2);
	req(RREQ_PTT, 0);
	// The VFO's values are its own settings, kept across a restart; the channel was not touched
	boot();
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_hz == 146340000 &&
	      sm.view.ch.bw == RADIO_BW_12K5 && sm.view.ch.rx_tone.value == 885 && sm.view.ch.tx_tone.value == 1000 &&
	      sm.view.power == 2);
	req(RREQ_VFO, 0);
	run(20);
	CHECK(!sm.view.vfo && sm.view.channel_number == 7 && sm.view.ch.bw == RADIO_BW_25K && !sm.view.temporary &&
	      sm.view.ch.rx_tone.type == TONE_NONE && chans[7].bw == RADIO_BW_25K && sm.view.power == 1);
	// In the VFO the request does nothing, and neither does it while transmitting
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 440000000);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 440000000);
	req(RREQ_VFO, 0);
	req(RREQ_PTT, 1);
	req(RREQ_VFO_COPY, 0);
	CHECK(!sm.view.vfo && sm.view.state == RS_TX);
	req(RREQ_PTT, 0);
	run(20);
	// A frequency outside the board's bands: refused, nothing changes
	chans[7].rx_hz = 520000000;
	chans[7].tx_hz = 520000000;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 0);
	run(20);
	CHECK(radio_vfo_copy_check(&sm.view.ch, false) == VFO_COPY_OUT_OF_BAND);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_REFUSED);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(!sm.view.vfo && sm.view.ch.rx_hz == 520000000 && sm.vfo_ch.rx_hz == 440000000);
	chans[7].rx_hz = 146940000;
	chans[7].tx_hz = 146340000;
	chans[7].tx_tone.type = TONE_NONE;
	req(RREQ_ZONE_POS, 1);
	req(RREQ_ZONE_POS, 0);
	run(20);
	// The debug link op: the same request; nothing to do in the VFO
	CHECK(debuglink_is_feature_op(DL_OP_VFO_COPY));
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_OK);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 146940000 && sm.view.ch.tx_tone.type == TONE_NONE);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_UNAVAILABLE);
	CHECK(radio_vfo_copy_check(&sm.view.ch, true) == VFO_COPY_ALREADY);
	// The DTMF op (radio-features.md 3.7): a key's edges, refused unless a key would send now
	CHECK(debuglink_is_feature_op(DL_OP_DTMF));
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ '5', 1 }, 2) == DL_ST_REFUSED);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ '5' }, 1) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'x', 1 }, 2) == DL_ST_BAD_ARG);
	req(RREQ_PTT, 1);
	run(60);
	CHECK(sm.view.state == RS_TX && sm.view.dtmf_ready);
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'D', 1 }, 2) == DL_ST_OK);
	run(20);
	CHECK(dtmf_on == 'D' && sm.view.dtmf == 'D');
	{
		static char buf[2048];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		CHECK(strstr(buf, "dtmf=D\n") != NULL && strstr(buf, "dtmf_ready=1") != NULL &&
		      strstr(buf, "dtmf_digits=D\n") != NULL && strstr(buf, "dtmf_sent=1\n") != NULL);
	}
	CHECK(debuglink_radio_feature_op(DL_OP_DTMF, (const uint8_t[]){ 'D', 0 }, 2) == DL_ST_OK);
	run(200);
	CHECK(dtmf_on == 0 && sm.view.dtmf == 0);
	req(RREQ_PTT, 0);
	run(20);
	// The UI's service call: the code of a refusal, or the request
	req(RREQ_VFO, 0);
	run(20);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_OK && sm.view.vfo);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_ALREADY);
	req(RREQ_VFO, 0);
	// An FM channel leaves the VFO's DMR values alone
#if CONFIG_VFO_DMR
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 440000000);
	req(RREQ_MODE, RADIO_MODE_DMR);
	req(RREQ_DMR_CC, 9);
	req(RREQ_DMR_TARGET, 3100);
	req(RREQ_VFO, 0);
	req(RREQ_VFO_COPY, 0);
	run(20);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_FM && sm.vfo_ch.color_code == 9 && sm.vfo_talk_id == 3100);
	req(RREQ_VFO, 0);
#endif
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}

#if CONFIG_VFO_DMR
// A DMR channel: color code, timeslot and the talk target (a contact is its ID and the group / private flag)
static void test_vfo_copy_dmr(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;

	dmr_start(true);			// channel 4: DMR, CC 2, TS 1, contact 1 = group 201
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && hw_mode == RADIO_MODE_DMR);
	CHECK(sm.view.ch.rx_hz == 438500000 && d->cc == 2 && d->ts == 1 && d->target_id == 201 && !d->target_private);
	CHECK(sm.vfo_talk_id == 201 && !sm.vfo_talk_private && !d->target_manual && sim.cc_filter == 2);
	// The VFO has its own settings now: they stay when the channel is left, and the channel is as before
	req(RREQ_DMR_CC, 5);
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && d->cc == 2 && d->target_id == 201 && sm.vfo_ch.color_code == 5 && sm.view.channel_number == 4);
	// A private contact
	chans[4].contact = 2;
	reload_channel();
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 3101234 && d->target_private && sm.vfo_talk_private && d->cc == 2);
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX && d->call.dst == 3101234 && !d->call.group && hw.tx_hz == 438500000);
	req(RREQ_PTT, 0);
	run(1000);
	req(RREQ_VFO, 0);
	// No contact: what the channel itself would send, the RX group list's first talkgroup (201), else talkgroup 9
	chans[4].contact = 0;
	reload_channel();
	CHECK(d->target_id == 201);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 201 && !d->target_private && sm.vfo_talk_id == 201);
	req(RREQ_VFO, 0);
	chans[4].listen_list = 0;
	reload_channel();
	CHECK(d->target_id == 9);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && d->target_id == 9 && !d->target_private && sm.vfo_talk_id == 9);
	req(RREQ_VFO, 0);
	chans[4].listen_list = 1;
	// Temporary changes in effect are copied: color code, slot, a talk target chosen for now
	chans[4].contact = 1;
	reload_channel();
	req(RREQ_DMR_CC, 7);
	req(RREQ_DMR_TS, 2);
	req(RREQ_DMR_TARGET, (int32_t)((1u << 24) | 3107777u));
	run(50);
	CHECK(sm.view.temporary && d->target_manual && d->target_id == 3107777);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(sm.view.vfo && !sm.view.temporary && d->cc == 7 && d->ts == 2 && d->target_id == 3107777 &&
	      d->target_private && !d->target_manual && sm.vfo_ch.color_code == 7 && sm.vfo_ch.timeslot == 2);
	// All of it survives a restart
	boot();
	run(50);
	CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && d->cc == 7 && d->ts == 2 &&
	      d->target_id == 3107777 && d->target_private && hw.rx_hz == 438500000);
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && d->cc == 2 && d->ts == 1 && d->target_id == 201 && !sm.view.temporary);
	req(RREQ_VFO, 0);
	settings_defaults(settings_begin());
	settings_end(now);
	setup_codeplug();
}
#else
static void test_vfo_copy_dmr(void)
{
	// Without the DMR VFO a DMR channel is refused, nothing changes
	dmr_start(true);
	CHECK(radio_vfo_copy_check(&sm.view.ch, false) == VFO_COPY_NO_MODE);
	CHECK(ui_svc_vfo_copy(&sm.view) == VFO_COPY_NO_MODE);
	CHECK(debuglink_radio_feature_op(DL_OP_VFO_COPY, NULL, 0) == DL_ST_REFUSED);
	req(RREQ_VFO_COPY, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.ch.mode == RADIO_MODE_DMR && sim.started && sm.view.channel_number == 4);
	setup_codeplug();
}
#endif

static void test_dmr_denied(void)
{
	// Polite access: a call on our slot and color code refuses PTT, and the arming is released
	dmr_start(true);
	hear_call(true, 3100, 3107777, 2, 1, false);
	CHECK(sm.view.state == RS_RX);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BUSY);
	CHECK(hw.tx_calls == 1 && hw.tx_off_calls == 1 && sim.tx_begins == 0);
	run(100);
	CHECK(sm.view.state == RS_TX_DENIED);
	req(RREQ_PTT, 0);
	run(20);
	CHECK(sm.view.state != RS_TX_DENIED);
	run(600);

	// Through a repeater that never answers: three wake-ups, then refused
	chans[4].tx_hz = 433500000;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(sm.view.dmr.rmo && sim.cfg.repeater && sim.locked_ts == 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433500000);
	run(500);
	CHECK(sm.view.dmr.state == RDMR_TX_WAIT);
	run(4000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_REPEATER && hw.tx_off_calls == 2);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);

	// No DMR ID anywhere
	own_dmr_id = 0;
	dmr_start(true);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_ID && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	own_dmr_id = 3101276;

	// RX-only channel and out-of-band checks come first on DMR channels too
	dmr_start(true);
	chans[4].rx_only = true;
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	req(RREQ_PTT, 1);
	CHECK(sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	chans[4].rx_only = false;
}

static void test_hotspot(void)
{
	CHECK(radio_sm_hotspot_freq_ok(438800000, 438800000));
	CHECK(!radio_sm_hotspot_freq_ok(121500000, 121500000));	// TX not allowed there
	CHECK(!radio_sm_hotspot_freq_ok(438800000, 0));
	CHECK(radio_sm_hotspot_power(255) == 2 && radio_sm_hotspot_power(0) == 0 && radio_sm_hotspot_power(128) == 1);

	dmr_start(true);
	// Refused while PTT is held
	req(RREQ_PTT, 1);
	req(RREQ_HS_ENTER, 0);
	CHECK(!sm.hotspot);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);

	int modes = hw.set_mode_calls;
	int tx0 = hw.tx_calls, off0 = hw.tx_off_calls;
	req(RREQ_HS_ENTER, 0);
	CHECK(sm.hotspot && sm.view.hotspot && !sim.started && sim.cb == NULL && !sm.view.dmr.active);
	CHECK(hw.set_mode_calls == modes);	// already in DMR
	radio_req_t r = { RREQ_HS_FREQ, 438800000, 438800000 };
	radio_sm_request(&sm, &r, now);
	CHECK(hw.rx_hz == 438800000 && hw.tx_hz == 438800000 && hw.tx_calls == tx0 + 1 &&
		hw.tx_power == 1);
	req(RREQ_HS_POWER, 255);
	CHECK(hw.tx_calls == tx0 + 2 && hw.tx_power == 2);
	r.arg = 121500000;	// refused: the radio stays where it was
	r.arg2 = 121500000;
	radio_sm_request(&sm, &r, now);
	CHECK(hw.rx_hz == 438800000);

	// Channel keys, monitor and PTT do nothing; the carrier comes from the noise reading
	req(RREQ_CH_STEP, 1);
	CHECK(sm.view.channel_number == 4);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_HOTSPOT);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE && hw.tx_off_calls == off0);
	signal(10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.hs_carrier && sm.view.state == RS_IDLE);
	signal(120, true);
	poll_for(RADIO_POLL_MS);
	CHECK(!sm.hs_carrier);

	// Leave: disarmed, the channel back, the call stack has the modem again
	req(RREQ_HS_LEAVE, 0);
	CHECK(!sm.hotspot && !sm.view.hotspot && hw.tx_off_calls == off0 + 1);
	CHECK(hw.rx_hz == 438500000 && sim.started && sim.cb != NULL && sm.view.dmr.active);
	hear_call(true, 201, 3101234, 2, 1, false);
	CHECK(sm.view.state == RS_RX);
	run(500);

	// From an FM channel: into DMR mode and back
	req(RREQ_CH_STEP, 1);
	modes = hw.set_mode_calls;
	req(RREQ_HS_ENTER, 446000000);
	CHECK(sm.hotspot && hw.set_mode_calls == modes + 1 && hw.rx_hz == 446000000);
	req(RREQ_HS_LEAVE, 0);
	CHECK(!sm.hotspot && hw.set_mode_calls == modes + 2 && hw.rx_hz == 121500000 && !sim.started);
	CHECK(hw.tx_off_calls == off0 + 1);	// never armed: no TX frequency was set
}

// ---- The hotspot session end to end: host link frames -> mmdvm.c -> hotspot.c -> radio_hotspot.c ops -> the radio
// state machine -> the modem, with the radio task modeled: requests are queued and handled either at once (the
// radio task is above the host link task and waiting on its queue) or later (it was busy elsewhere) ----

#define HS_Q	16
static radio_req_t hs_q[HS_Q];
static int hs_qn;
static bool radio_preempts = true;	// a post runs the radio task at once
static int mm_replies, mm_acks, mm_naks;
static uint8_t mm_last[64];

static void radio_pass(void)
{
	for (int i = 0; i < hs_qn; i++) {
		radio_sm_request(&sm, &hs_q[i], now);
	}
	hs_qn = 0;
	radio_hs_watch(&sm, now);
	radio_hs_publish(&sm);
}

static void hs_post(const radio_req_t *r, bool must)
{
	(void)must;
	if (hs_qn < HS_Q) {
		hs_q[hs_qn++] = *r;
	}
	if (radio_preempts) {
		radio_pass();
	}
}

static void mm_write(const uint8_t *buf, size_t len)
{
	mm_replies++;
	if (len >= 3 && buf[2] == MMDVM_ACK) {
		mm_acks++;
	}
	if (len >= 3 && buf[2] == MMDVM_NAK) {
		mm_naks++;
	}
	memcpy(mm_last, buf, len < sizeof(mm_last) ? len : sizeof(mm_last));
}

static const mmdvm_io_t mm_io = { .write = mm_write, .description = "RadioFoundry fake" };

static void mm_cmd(uint8_t cmd, const uint8_t *p, uint8_t n)
{
	uint8_t f[64];
	f[0] = MMDVM_FRAME_START;
	f[1] = (uint8_t)(n + 3);
	f[2] = cmd;
	if (n > 0) memcpy(f + 3, p, n);	// p may be NULL for a command without a payload
	mmdvm_frame(f, (uint16_t)(n + 3), now);
}

// The host link task's loop for ms: mmdvm_poll every 5 ms; the radio task's tick and poll on their periods
static void hs_run(uint32_t ms)
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
			radio_pass();
		}
		mmdvm_poll(now);
	}
}

static void hs_session_start(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	chans[4].rx_hz = chans[4].tx_hz = 467375000;	// the bench's DMR channel
	req(RREQ_ZONE_POS, 3);
	run(50);
	hs_qn = 0;
	radio_hs_init(hs_post);
	radio_hs_publish(&sm);
	hotspot_set_radio(&radio_hs_ops);
	hotspot_init(&sim_modem, mmdvm_send);
	mmdvm_init(&mm_io);
	mm_replies = mm_acks = mm_naks = 0;
}

static void test_hotspot_session(void)
{
	hotspot_stats_t st;
	hotspot_view_t v;

	// The radio task answers the enter at once: the first frame takes the modem
	hs_session_start();
	CHECK(sim.started && sim.cb != NULL && sm.view.dmr.active);
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	CHECK(mm_replies == 1 && mm_last[2] == MMDVM_GET_VERSION);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(sm.hotspot && v.running && st.enter_attempts == 1 && hotspot_running());
	CHECK(sim.started && sim.cb == hotspot_modem_event);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && mm_last[8] > 0);	// DMR slot 2 space: the hotspot runs
	hs_run(4000);
	hotspot_get_view(&v);
	CHECK(v.running && sm.hotspot);	// the orphan watch leaves it alone
	mmdvm_end();
	radio_pass();
	CHECK(!sm.hotspot && sim.cb != NULL && sim.cb != hotspot_modem_event && sm.view.dmr.active);

	// The radio task busy when the first frame comes (the enter is handled after the post returned): taken on the
	// next attempt, HOTSPOT_ENTER_RETRY_MS later
	hs_session_start();
	radio_preempts = false;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hotspot_get_stats(&st);
	CHECK(st.enter_attempts == 1 && !hotspot_running());
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && mm_last[8] == 0);	// no space until the hotspot runs
	hs_run(HOTSPOT_ENTER_RETRY_MS + 50);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(st.enter_attempts == 2 && v.running && sm.hotspot && sim.cb == hotspot_modem_event);
	radio_preempts = true;
	mmdvm_end();
	radio_pass();
	CHECK(!sm.hotspot && sm.view.dmr.active);
	// A second session on the same boot (the first ended with the port closed, after a full configuration): the
	// next host frame takes the modem again
	hs_session_start();
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	{
		uint8_t fr[13] = { 0 };
		uint32_t hz = 467375000;
		for (int i = 0; i < 4; i++) {
			fr[1 + i] = (uint8_t)(hz >> (8 * i));
			fr[5 + i] = (uint8_t)(hz >> (8 * i));
		}
		fr[9] = 255;
		mm_cmd(MMDVM_SET_FREQ, fr, 10);
		uint8_t cfg[21] = { MMDVM_CONFIG_SIMPLEX, MMDVM_MODE_BIT_DMR, 10, 0, 128, 128, 1 };
		mm_cmd(MMDVM_SET_CONFIG, cfg, sizeof(cfg));
		uint8_t m = MMDVM_MODE_DMR;
		mm_cmd(MMDVM_SET_MODE, &m, 1);
	}
	CHECK(mm_naks == 0 && hotspot_running() && sm.hs_tx_hz == 467375000 && sm.dmr_keyed);
	hs_run(500);
	mmdvm_end();
	hs_run(100);
	CHECK(!sm.hotspot && !sm.dmr_keyed && sm.view.dmr.active);
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hs_run(100);
	hotspot_get_stats(&st);
	hotspot_get_view(&v);
	CHECK(st.enter_attempts == 2 && v.running && sm.hotspot && sim.cb == hotspot_modem_event);
	CHECK(st.starts == 2 && st.stops == 1);	// counted across sessions: enter_attempts 2 is two sessions here
	mmdvm_end();
	radio_pass();
	hotspot_get_stats(&st);
	CHECK(st.stops == 2 && !hotspot_running());
}

// The position, the VFO and the preferences come back after a reboot (core/settings), temporary changes don't, and
// a codeplug change underneath moves the radio to a channel that still exists
static void test_persist(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE, 1);
	CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6);
	boot();
	CHECK(!sm.view.vfo && sm.view.zone_index == 1 && sm.view.channel_number == 6);
	req(RREQ_ZONE, 0);
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	boot();
	CHECK(sm.view.zone_index == 0 && sm.view.zone_pos == 3 && sm.view.channel_number == 4);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sm.board_mode == RADIO_MODE_DMR);

	// The VFO and its settings
	req(RREQ_VFO_SET, 145600000);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 1000 }));
	CHECK(sm.view.vfo);
	boot();
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 145600000 && sm.view.ch.bw == RADIO_BW_25K);
	CHECK(sm.view.ch.tx_tone.type == TONE_CTCSS && sm.view.ch.tx_tone.value == 1000 && hw.rx_hz == 145600000);
	req(RREQ_VFO, 0);
	boot();
	CHECK(!sm.view.vfo && sm.view.channel_number == 4);
	req(RREQ_VFO, 1);
	CHECK(sm.view.ch.rx_hz == 145600000);	// the VFO kept its frequency while on the channel
	req(RREQ_VFO, 0);

	// Temporary changes are not saved; the base squelch, master power and DMR ID override are preferences
	req(RREQ_SQUELCH, 2);
	CHECK(sm.view.temporary);
	boot();
	CHECK(!sm.view.temporary && sm.view.squelch == RADIO_SQUELCH_DEFAULT);
	settings_t *st = settings_begin();
	st->squelch = 2;
	st->master_power = 0;
	st->dmr_id = 1234567;
	settings_end(now);
	req(RREQ_RELOAD, 0);
	CHECK(sm.view.squelch == 2 && !sm.view.temporary);
	CHECK(sm.view.dmr.own_id == 1234567);
	req(RREQ_ZONE_POS, 0);			// "Simplex": default power = the master power now
	CHECK(sm.view.power == 0);
	boot();
	CHECK(sm.view.squelch == 2 && sm.view.power == 0 && sm.view.dmr.own_id == 1234567);

	// The codeplug changes underneath: the channel is deleted, the radio moves on to the next one in the zone
	req(RREQ_ZONE_POS, 3);
	CHECK(sm.view.channel_number == 4);
	used[4] = false;
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.channel_number == 5 && sm.view.zone_pos == 4);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.board_mode == RADIO_MODE_FM);
	boot();
	CHECK(sm.view.channel_number == 5);
	// Renamed in place: the new name shows
	strcpy(chans[5].name, "Air 2");
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.channel_number == 5 && strcmp(sm.view.ch.name, "Air 2") == 0);
	// A saved zone that no longer exists: the first zone
	req(RREQ_ZONE, 2);
	nzones = 2;
	boot();
	CHECK(sm.view.zone_index == 0);

	// The radio core's settings (radio_set_settings) are saved and in effect after a reboot
	radio_settings_t rs;
	radio_settings_default(&rs);
	CHECK(sm.set.tot_s == rs.tot_s && sm.set.key_beep);
	rs.tot_s = 45;
	rs.key_beep = false;
	rs.roger_beep = true;
	rs.vox_level = 99;			// clamped to 10
	radio_sm_set_settings(&sm, &rs, now);
	settings_t cur;
	settings_read(&cur);
	CHECK(cur.rs_stored == 1 && cur.rs_tot_s == 45 && cur.rs_vox_level == 10);
	boot();
	CHECK(sm.set.tot_s == 45 && !sm.set.key_beep && sm.set.roger_beep && sm.set.vox_level == 10);
	settings_begin()->rs_stored = 0;	// never set: the defaults
	settings_end(now);
	boot();
	CHECK(sm.set.tot_s == RADIO_TOT_DEFAULT_S && sm.set.key_beep && !sm.set.roger_beep);

	// The DMR data service's settings: saved when they change, back after a reboot
	dmrDataSettings_t ds;
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0 && !ds.radioCheckReply && ds.callAlertAck && ds.smsFormat == DMR_SMS_FORMAT_ETSI_IP);
	ds.arsServer = 3100999;
	ds.radioCheckReply = true;
	ds.callAlertAck = false;
	dmr_data_set_settings(&ds);
	run(50);
	settings_read(&cur);
	CHECK(cur.ds_stored == 1 && cur.ds_ars_server == 3100999 && cur.ds_flags == (SETTINGS_DS_RADIO_CHECK_REPLY |
											SETTINGS_DS_LRRP_REPLY | SETTINGS_DS_SMS_ETSI_IP));
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 3100999 && ds.radioCheckReply && !ds.callAlertAck && ds.lrrpReply);
	// A record from before ETSI IP text (no format bit) keeps TMS: only the default changed
	settings_begin()->ds_flags = SETTINGS_DS_CALL_ALERT_ACK | SETTINGS_DS_LRRP_REPLY;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.smsFormat == DMR_SMS_FORMAT_TMS && ds.callAlertAck);
	settings_begin()->ds_stored = 0;
	settings_end(now);
	boot();
	dmrDataServiceGetSettings(&ds);
	CHECK(ds.arsServer == 0 && !ds.radioCheckReply && ds.callAlertAck && ds.smsFormat == DMR_SMS_FORMAT_ETSI_IP);

	// Power off: nothing to deregister, so the radio core is done at once
	req(RREQ_POWER_OFF, 0);
	run(50);
	CHECK(sm.power_off_req && !sm.power_off_busy);
	// An auto power off that didn't cut the power (the knob still on): canceled, the radio runs on
	req(RREQ_POWER_OFF, 1);
	run(50);
	CHECK(!sm.power_off_req && !sm.power_off_busy);
	setup_codeplug();
	start();
}

// The air header's LC of the last transmission that started at air[first]
static void air_lc(unsigned first, dmr_lc_t *l)
{
	dmr_lc_unpack(sim.air[first].payload, l);
}

static void reload_channel(void)
{
	req(RREQ_ZONE_POS, 0);
	req(RREQ_ZONE_POS, 3);
	run(50);
}

static void tx_once(void)
{
	req(RREQ_PTT, 1);
	run(600);
	req(RREQ_PTT, 0);
	run(1000);
}

static void test_dmr_voice_features(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_lc_t l;

	// A typed private ID: PTT calls it, its name comes from the DMR ID database (cut to the name field)
	dmr_start(true);
	CHECK(!d->target_manual && !d->alias_tx && !d->privacy);
	req(RREQ_DMR_TARGET, (int32_t)(3105555u | (1u << 24)));
	run(20);
	CHECK(d->target_manual && d->target_id == 3105555 && d->target_private);
	CHECK(strcmp(d->target_name, "KD9XYZ Robert Lo") == 0);
	unsigned first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(sim.air[first].kind == DMR_BURST_VOICE_LC_HEADER && l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 3105555 &&
	      l.src == 3101276);
	// Looked up once, then from the cache
	uint32_t finds = contact_finds, looks = iddb_lookups;
	run(2000);
	hear_call(true, 201, 3105555, 2, 1, true);
	CHECK(contact_finds == finds && iddb_lookups == looks);
	// Back to the channel's target
	req(RREQ_DMR_TARGET_CLEAR, 0);
	run(20);
	CHECK(!d->target_manual && d->target_id == 201 && !d->target_private);

	// A codeplug contact as the target; a channel change ends it
	req(RREQ_DMR_CONTACT, 2);
	run(20);
	CHECK(d->target_manual && d->target_id == 3101234 && d->target_private && strcmp(d->target_name, "Joe") == 0);
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	run(50);
	CHECK(!d->target_manual && d->target_id == 201);
	req(RREQ_DMR_CONTACT, 99);	// no such contact: nothing changes
	run(20);
	CHECK(!d->target_manual);

	// A private call to us: shown with its name, emergency flag passed on; then call back to the caller
	uint8_t lc[9];
	mk_lc(lc, false, 3101276, 3105555);
	lc[2] = DMR_SVC_EMERGENCY;
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	CHECK(d->state == RDMR_RX && d->call.to_us && !d->call.group && d->call.emergency);
	CHECK(strcmp(d->call.src_name, "KD9XYZ Robert Lo") == 0);
	burst_lc(lc, 2, true);
	run(100);
	CHECK(d->reply_active && d->target_private && d->target_id == 3105555);	// the reply window
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	req(RREQ_DMR_CALL_BACK, 0);	// still possible after the window: from the last call heard
	run(20);
	CHECK(d->target_manual && d->target_private && d->target_id == 3105555);
	// A group call heard last: call back goes to its talkgroup, named from all the contacts
	hear_call(true, 3100, 3107777, 2, 1, true);
	req(RREQ_DMR_TARGET_CLEAR, 0);
	req(RREQ_DMR_HEAR_ALL, 1);
	hear_call(true, 3100, 3107777, 2, 1, true);
	req(RREQ_DMR_HEAR_ALL, 0);
	req(RREQ_DMR_CALL_BACK, 0);
	run(20);
	CHECK(d->target_manual && !d->target_private && d->target_id == 3100 && strcmp(d->target_name, "TG USA") == 0);
	req(RREQ_DMR_TARGET_CLEAR, 0);

	// Codeplug hang times become the reply windows (group 3 s, private 6 s)
	dmr_settings.group_hang_ms = 3000;
	dmr_settings.private_hang_ms = 6000;
	dmr_start(true);
	hear_call(true, 9, 3105555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 3);
	run(3100);
	CHECK(!d->reply_active);
	hear_call(false, 3101276, 3105555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 6);
	dmr_settings.group_hang_ms = dmr_settings.private_hang_ms = 0;

	// Talker alias TX when the channel asks for it on its timeslot: the radio name in alternate superframes
	dmr_start(true);
	chans[4].alias_tx = CODEPLUG_ALIAS_TS2_TEXT;	// TS1 channel: not on this one
	reload_channel();
	CHECK(!d->alias_tx);
	chans[4].alias_tx = CODEPLUG_ALIAS_TS1_TEXT;
	reload_channel();
	CHECK(d->alias_tx);
	dmr_call_stats_t cs0, cs1;
	dmr_call_get_stats(&cs0);
	req(RREQ_PTT, 1);
	run(360 * 4);
	req(RREQ_PTT, 0);
	run(1000);
	dmr_call_get_stats(&cs1);
	CHECK(cs1.tx_alias_lcs - cs0.tx_alias_lcs >= 1);	// "N0CALL" in every other superframe (details: dmrCallTest)
	chans[4].alias_tx = 0;

	// Channel TX time-out from the codeplug
	dmr_start(true);
	chans[4].tot_s = 2;
	reload_channel();
	req(RREQ_PTT, 1);
	run(3000);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_TIMEOUT);
	req(RREQ_PTT, 0);
	run(500);
	chans[4].tot_s = 0;

	// Basic privacy key: our calls go out with the privacy FID and service option; turned off again
	dmr_start(true);
	req(RREQ_DMR_PRIVACY, (int32_t)((1u << 16) | 0u));	// Motorola key 0 is not valid: ignored
	CHECK(!d->privacy);
	req(RREQ_DMR_PRIVACY, (int32_t)((1u << 16) | 7u));
	CHECK(d->privacy);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.fid == 0x10 && l.svc == DMR_SVC_PRIVACY);
	req(RREQ_DMR_PRIVACY, 0);
	CHECK(!d->privacy);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.fid == 0 && l.svc == 0);
}

// ---- DMR emergency in the radio core (docs/spec/dmr-emergency.md): the hot mic keys a call with the emergency service
// option to the target, PTT takes it over, cancel ends the option; a heard emergency call raises one alert ----

static void hear_emergency_call(uint32_t dst, uint32_t src)
{
	uint8_t lc[9];
	dmr_lc_t l = { .flco = DMR_FLCO_GROUP_VOICE, .svc = DMR_SVC_EMERGENCY, .dst = dst, .src = src };
	dmr_lc_pack(&l, lc);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int i = 0; i < 6; i++) {
		burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
		run(60);
	}
	burst_lc(lc, 2, true);
	run(100);
}

static ui_event_t last_ev;	// the UI glue fakes below (ui_notify)
static int n_ev;

// A private contact added (an edit, a CPS write) after its ID was already called: the name shows from the next
// codeplug read on, not the cached "no name"
static void test_dmr_contact_added(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_start(true);
	req(RREQ_DMR_TARGET, (int32_t)(3106001u | (1u << 24)));
	run(20);
	CHECK(d->target_manual && d->target_id == 3106001 && d->target_private && d->target_name[0] == '\0');
	added_contact = (contact_t){ "Spencer Fowler", 3106001, CONTACT_PRIVATE, 0 };
	cp_serial++;
	poll_for(RADIO_POLL_MS);
	run(20);
	CHECK(d->target_id == 3106001 && d->target_private && strcmp(d->target_name, "Spencer Fowler") == 0);
	memset(&added_contact, 0, sizeof(added_contact));
}

static void test_dmr_emergency(void)
{
	dmr_lc_t l;
	dmr_start(true);
	dmr_emergency_settings_t es;
	dmr_emergency_settings_default(&es);
	es.mode = DMR_EMERGENCY_MODE_CALL;	// the alarm CSBKs: dmrDataChainTest
	es.hot_mic_s = 2;
	dmr_emergency_set_settings(&es);
	run(20);
	settings_t st;
	settings_read(&st);
	CHECK(st.em_stored == 1 && st.em_mode == DMR_EMERGENCY_MODE_CALL && st.em_hot_mic_s == 2);

	// The key: the hot mic keys the channel's talk target (TG 201) with the emergency option, impolite
	unsigned first = sim.air_count;
	int tx = hw.tx_calls;
	CHECK(dmr_emergency_start());
	run(50);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.em_keyed && sm.view.state == RS_TX && hw.tx_calls == tx + 1);
	run(600);
	air_lc(first, &l);
	CHECK(sim.air[first].kind == DMR_BURST_VOICE_LC_HEADER && (l.svc & DMR_SVC_EMERGENCY) && l.dst == 201 &&
	      l.flco == DMR_FLCO_GROUP_VOICE);
	run(1600);	// the 2 s run out: released
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_ACTIVE && !sm.em_keyed);
	run(1000);
	CHECK(sm.view.state == RS_IDLE && sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);

	// Still in emergency: the user's own call carries the option
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK((l.svc & DMR_SVC_EMERGENCY) && l.dst == 201);
	// Canceled: the next call doesn't
	dmr_emergency_cancel();
	run(20);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(!(l.svc & DMR_SVC_EMERGENCY) && l.dst == 201);

	// PTT pressed during the hot mic takes the call over: it stays keyed past the hot mic, ends on release
	es.hot_mic_s = 1;
	es.dst = 9;		// a set emergency target
	dmr_emergency_set_settings(&es);
	run(20);
	first = sim.air_count;
	CHECK(dmr_emergency_start());
	run(300);
	CHECK(sm.view.state == RS_TX && sm.em_keyed);
	req(RREQ_PTT, 1);
	CHECK(!sm.em_keyed);
	run(1500);
	CHECK(sm.view.state == RS_TX && dmr_emergency_state() == DMR_EMERGENCY_ACTIVE);
	air_lc(first, &l);
	CHECK((l.svc & DMR_SVC_EMERGENCY) && l.dst == 9);
	req(RREQ_PTT, 0);
	run(1000);
	CHECK(sm.view.state == RS_IDLE);
	dmr_emergency_cancel();
	run(20);

	// Heard: an emergency call on a group we listen to raises the alert and its tone once
	dmr_emergency_stats_t s0, s1;
	dmr_emergency_get_stats(&s0);
	hear_emergency_call(201, 3101234);
	dmr_emergency_alert_t a;
	CHECK(dmr_emergency_alert(&a) && a.kind == DMR_EMERGENCY_ALERT_CALL && a.src == 3101234 && a.dst == 201 && a.group);
	run(500);
	hear_emergency_call(201, 3101234);	// the next over: the same alert
	dmr_emergency_get_stats(&s1);
	CHECK(s1.alerts == s0.alerts + 1 && s1.alerts_call == s0.alerts_call + 1 && s1.tones == s0.tones + 1);
	n_ev = 0;
	ui_svc_poll();		// the UI's pass: the alert as UI_EV_EMERGENCY, once
	ui_svc_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_EMERGENCY && last_ev.id == 3101234 && !last_ev.ok &&
	      strcmp(last_ev.name, "Joe") == 0);
	dmr_emergency_dismiss();
	run(20);
	CHECK(!dmr_emergency_alert(&a));

	// The UI's emergency key (ui_svc_emergency, core/ui/ui_svc_emergency.c)
	CHECK(ui_svc_feat_ext() & UI_FEAT_EMERGENCY);
	CHECK(ui_svc_emergency(true));
	run(50);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.view.state == RS_TX);
	CHECK(ui_svc_emergency(true));		// already on
	CHECK(ui_svc_emergency(false));
	run(20);
	CHECK(dmr_emergency_state() == DMR_EMERGENCY_IDLE && !sm.em_keyed);
	run(1500);
	CHECK(sm.view.state == RS_IDLE);
}

// ---- The hotspot's CW ID (SEND_CWID) on a board with tx_tone_key: FM carrier on the hotspot TX frequency, the tone
// keyed in Morse at 20 wpm after a 150 ms lead, then back to DMR on the RX frequency with the keying armed ----

static void hs_configure(void)
{
	uint8_t fr[13] = { 0 };
	uint32_t hz = 467375000;
	for (int i = 0; i < 4; i++) {
		fr[1 + i] = (uint8_t)(hz >> (8 * i));
		fr[5 + i] = (uint8_t)(hz >> (8 * i));
	}
	fr[9] = 255;
	mm_cmd(MMDVM_SET_FREQ, fr, 10);
	uint8_t cfg[21] = { MMDVM_CONFIG_SIMPLEX, MMDVM_MODE_BIT_DMR, 10, 0, 128, 128, 1 };
	mm_cmd(MMDVM_SET_CONFIG, cfg, sizeof(cfg));
	uint8_t m = MMDVM_MODE_IDLE;
	mm_cmd(MMDVM_SET_MODE, &m, 1);
}

static void test_hotspot_cwid(void)
{
	hs_session_start();
	radio_preempts = true;
	mm_cmd(MMDVM_GET_VERSION, NULL, 0);
	hs_configure();
	CHECK(mm_naks == 0 && hotspot_running() && sm.dmr_keyed && hw_mode == RADIO_MODE_DMR);
	memset(&tone, 0, sizeof(tone));
	radio_hs_cw_stats_t cs0, cs;
	radio_hs_cw_get_stats(&cs0);
	int modes = hw.set_mode_calls;
	uint32_t t0 = now;
	// "AB": A .- (5 units), the character gap (3), B -... (9): 17 units, 1020 ms; key down 10 units, 600 ms
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"AB", 2);
	CHECK(mm_last[2] == MMDVM_ACK && mm_naks == 0);
	CHECK(hw_mode == RADIO_MODE_FM && hw.set_mode_calls == modes + 1 && hw.tx_hz == 467375000 && tone.hz == 800);
	CHECK(!sm.dmr_keyed && sm.cw_phase != 0 && hw.led[LED_RED]);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(mm_last[2] == MMDVM_GET_STATUS && (mm_last[5] & MMDVM_STATUS_TX));	// the TX flag while it keys
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"AB", 2);
	CHECK(mm_last[2] == MMDVM_NAK);		// not idle meanwhile
	hs_run(1500);
	CHECK(tone.changes == 12);		// six elements
	CHECK(tone.first_down_ms >= t0 + 150 && tone.first_down_ms <= t0 + 160);
	CHECK(tone.last_up_ms - tone.first_down_ms >= 1010 && tone.last_up_ms - tone.first_down_ms <= 1030);
	CHECK(tone.down_ms >= 590 && tone.down_ms <= 610);
	radio_hs_cw_get_stats(&cs);
	CHECK(cs.sent == cs0.sent + 1 && cs.wpm == 20 && !cs.active && cs.last_ms >= 1300 && cs.last_ms <= 1340);
	// Back: DMR mode (the modem set up again), the RX frequency, the keying armed, the TX flag clear
	CHECK(hw_mode == RADIO_MODE_DMR && hw.set_mode_calls == modes + 2 && hw.rx_hz == 467375000 && sm.dmr_keyed);
	CHECK(sm.cw_phase == 0 && tone.hz == 0 && !hw.led[LED_RED]);
	mm_cmd(MMDVM_GET_STATUS, NULL, 0);
	CHECK(!(mm_last[5] & MMDVM_STATUS_TX));

	// Hotspot mode left in the middle of an ID: cut, counted as not sent
	mm_cmd(MMDVM_SEND_CWID, (const uint8_t *)"TEST", 4);
	hs_run(300);
	CHECK(sm.cw_phase != 0 && tone.hz == 800);
	mmdvm_end();
	hs_run(100);
	radio_hs_cw_get_stats(&cs);
	CHECK(cs.refused == cs0.refused + 1 && !cs.active && sm.cw_phase == 0 && tone.hz == 0 && !sm.hotspot);
	CHECK(hw_mode == RADIO_MODE_DMR && sm.view.dmr.active);
}

// The UI's on-the-fly DMR changes (temporary timeslot, color code and mode), hear all latched, the one-shot talk
// target of a focused row and the reply window setting (docs/spec/ui.md 4)
static void test_ui_requests(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	dmr_lc_t l;

	// Timeslot 2 for now; asking for the channel's slot again clears it
	dmr_start(true);
	req(RREQ_DMR_TS, 2);
	run(20);
	CHECK((sm.view.temp & RADIO_TEMP_TS) && sm.view.ch.timeslot == 2 && d->ts == 2);
	req(RREQ_DMR_TS, 1);
	run(20);
	CHECK(!sm.view.temporary && sm.view.ch.timeslot == 1 && d->ts == 1);
	req(RREQ_DMR_TS, 3);
	CHECK(!sm.view.temporary);
	// Color code 7 for now: the old color code isn't heard any more, the new one is
	req(RREQ_DMR_CC, 7);
	run(20);
	CHECK((sm.view.temp & RADIO_TEMP_CC) && d->cc == 7 && sm.view.ch.color_code == 7);
	hear_call(true, 201, 3101234, 2, 1, false);
	CHECK(sm.view.state == RS_IDLE);
	run(500);
	hear_call(true, 201, 3101234, 7, 1, true);
	CHECK(d->last_ms != 0 && d->last.src == 3101234);
	// Undo (BACK on Home): the channel's color code and timeslot again
	req(RREQ_DMR_TS, 2);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
	CHECK(!sm.view.temporary && d->cc == 2 && d->ts == 1 && sm.view.ch.color_code == 2 && sm.view.ch.timeslot == 1);
	// FM for now stops the call stack (color code refused on FM); undo starts it again
	req(RREQ_MODE, RADIO_MODE_FM);
	run(20);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && (sm.view.temp & RADIO_TEMP_MODE) && !d->active && !sim.started);
	req(RREQ_DMR_CC, 3);
	CHECK(sm.view.ch.color_code == 2 && !(sm.view.temp & RADIO_TEMP_CC));
	req(RREQ_CLEAR_TEMP, 0);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && d->active && sim.started && !sm.view.temporary);
	// An FM channel as DMR for now; a channel change drops it
	req(RREQ_ZONE_POS, 0);
	run(20);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && d->active && (sm.view.temp & RADIO_TEMP_MODE) && d->ts == 1);
	req(RREQ_ZONE_POS, 3);
	run(50);
	CHECK(!sm.view.temporary && d->active && d->cc == 2);
	// Nothing changes while transmitting
	req(RREQ_PTT, 1);
	run(100);
	req(RREQ_DMR_CC, 5);
	req(RREQ_MODE, RADIO_MODE_FM);
	CHECK(sm.view.ch.color_code == 2 && sm.view.ch.mode == RADIO_MODE_DMR);
	req(RREQ_PTT, 0);
	run(1000);

	// Hear all latched: other talkgroups play until it is turned off, or the channel changes
	req(RREQ_DMR_HEAR_ALL, 1);
	CHECK(d->hear_all && d->hear_all_latch);
	hear_call(true, 3100, 3107777, 2, 1, false);
	CHECK(d->state == RDMR_RX);
	run(500);
	req(RREQ_DMR_HEAR_ALL, 0);
	CHECK(!d->hear_all && !d->hear_all_latch);
	req(RREQ_REPLY_CANCEL, 0);		// TG 3100's reply window would let it through
	hear_call(true, 3100, 3107777, 2, 1, false);
	CHECK(d->state == RDMR_RX_OTHER);
	run(500);
	req(RREQ_DMR_HEAR_ALL, 1);
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(!d->hear_all_latch);		// the monitor key's press turned it off again
	req(RREQ_MONITOR, 1);
	req(RREQ_MONITOR, 0);
	CHECK(d->hear_all && d->hear_all_latch);	// and on
	req(RREQ_CH_STEP, 1);
	req(RREQ_CH_STEP, -1);
	run(50);
	CHECK(!d->hear_all_latch && !d->hear_all);

	// One-shot: PTT with Joe focused calls Joe once and keeps him for the reply window, then TG 201 again
	req(RREQ_DMR_FOCUS, (int32_t)(3101234u | (1u << 24)));
	CHECK(d->focus_id == 3101234 && d->focus_private);
	unsigned first = sim.air_count;
	req(RREQ_PTT, 1);
	run(600);
	CHECK(d->one_shot && d->target_id == 3101234 && d->target_private);
	req(RREQ_PTT, 0);
	run(1000);
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 3101234);
	CHECK(d->one_shot && d->reply_active && d->reply_s >= 8 && d->target_id == 3101234);
	req(RREQ_DMR_FOCUS, 0);		// back on Home: PTT inside the window still answers Joe
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_UNIT_VOICE && l.dst == 3101234 && d->one_shot);
	run(8000);
	CHECK(d->one_shot && d->reply_active && d->reply_s <= 2);
	run(1500);
	CHECK(!d->one_shot && !d->reply_active && d->target_id == 201 && !d->target_private);
	first = sim.air_count;
	tx_once();
	air_lc(first, &l);
	CHECK(l.flco == DMR_FLCO_GROUP_VOICE && l.dst == 201);
	// BACK on Home (reply cancel) ends it at once
	req(RREQ_DMR_FOCUS, 9);
	tx_once();
	CHECK(d->one_shot && d->target_id == 9 && !d->target_private);
	req(RREQ_REPLY_CANCEL, 0);
	run(20);
	CHECK(!d->one_shot && d->target_id == 201);
	// The talk target itself focused: an ordinary transmission
	req(RREQ_DMR_FOCUS, 201);
	tx_once();
	CHECK(!d->one_shot);
	req(RREQ_DMR_FOCUS, 0);
	CHECK(d->focus_id == 0);

	// The reply window setting replaces the codeplug's hang time (and the stack's 10 s)
	radio_settings_t rs = sm.set;
	rs.reply_s = 3;
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 3105555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 3);
	run(3100);
	CHECK(!d->reply_active);
	rs.reply_s = 0;			// off
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 3105555, 2, 1, true);
	CHECK(!d->reply_active);
	settings_t st;
	settings_read(&st);
	CHECK(st.rs_reply_s == 0 && st.rs_stored == 1);
	rs.reply_s = RADIO_REPLY_CODEPLUG;
	radio_sm_set_settings(&sm, &rs, now);
	hear_call(true, 9, 3105555, 2, 1, true);
	CHECK(d->reply_active && d->reply_s == 10);
	run(11000);
}

// ---- The UI's service glue (core/ui/ui_svc_radio.c, ui_svc_storage.c, ui_svc_data.c) on the radio core: the
// requests go straight into the state machine here instead of through radio_task.c's queue ----

uint32_t hal_millis(void) { return now; }
void radio_get_view(radio_view_t *v) { *v = sm.view; }
uint32_t radio_view_serial(void) { return sm.serial; }
void radio_get_settings(radio_settings_t *s) { *s = sm.set; }
void radio_set_settings(const radio_settings_t *s) { radio_sm_set_settings(&sm, s, now); }
void radio_req_monitor(bool down) { req(RREQ_MONITOR, down); }
void radio_req_reload(void) { req(RREQ_RELOAD, 0); }
void radio_req_volume(uint8_t step) { req(RREQ_VOLUME, step); }
void radio_req_volume_override(int8_t step) { req(RREQ_VOLUME_OVERRIDE, step); }
void radio_req_clear_temporary(void) { req(RREQ_CLEAR_TEMP, 0); }
// Not under test here: debuglink_view_radio.c, linked for the volume operation, refers to them
void radio_req_scan_dual(uint16_t pos) { (void)pos; }
void radio_req_zone_pos(uint16_t pos) { req(RREQ_ZONE_POS, pos); }
void radio_req_zone(uint16_t index) { req(RREQ_ZONE, index); }
void radio_req_scan_vfo(uint32_t lo, uint32_t hi) { (void)lo; (void)hi; }
void radio_req_tone_burst(bool on) { (void)on; }
void radio_req_dtmf(char digit, bool down) { req(RREQ_DTMF, (uint8_t)digit | (down ? 0x100 : 0)); }
bool radio_dtmf_ready(void) { return sm.view.dtmf_ready; }
void radio_req_scan(uint8_t cmd) { req(RREQ_SCAN, cmd); }
void radio_req_vfo_step_size(uint32_t hz) { req(RREQ_VFO_STEP_SIZE, (int32_t)hz); }
void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel)
{
	radio_req_t r = { RREQ_DUAL, 0, 0 };
	if (ref != NULL) {
		r.arg = (int32_t)radio_dual_arg(&ref[0], sel == 0);
		r.arg2 = radio_dual_arg(&ref[1], sel != 0);
	}
	radio_sm_request(&sm, &r, now);
}
bool radio_get_vfo(uint8_t index, channel_t *out)
{
	if (index == sm.vfo_idx) {
		*out = sm.vfo_ch;
		return sm.vfo_seeded;
	}
	*out = sm.vfo_other.ch;
	return sm.vfo_other.seeded;
}
bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at)
{
	ref[0] = sm.dual[0];
	ref[1] = sm.dual[1];
	*sel = sm.dual_sel;
	*at = sm.dual_at;
	return sm.scan.kind == SCAN_DUAL;
}
void radio_req_roam(uint8_t cmd) { req(RREQ_ROAM, cmd); }
void radio_req_beep(uint8_t b) { req(RREQ_BEEP, b); }
void radio_req_vibrate(uint8_t v) { req(RREQ_VIBRATE, v); }
void radio_req_dmr_target(uint32_t id, bool p) { req(RREQ_DMR_TARGET, (int32_t)((id & 0xFFFFFFu) | (p ? 1u << 24 : 0))); }
void radio_req_dmr_target_clear(void) { req(RREQ_DMR_TARGET_CLEAR, 0); }
void radio_req_dmr_timeslot(uint8_t ts) { req(RREQ_DMR_TS, ts); }
void radio_req_dmr_color_code(uint8_t cc) { req(RREQ_DMR_CC, cc); }
void radio_req_mode(uint8_t mode) { req(RREQ_MODE, mode); }
void radio_req_vfo_copy(void) { req(RREQ_VFO_COPY, 0); }
radio_vfo_copy_t radio_req_vfo_copy_checked(void)
{
	radio_vfo_copy_t r = radio_vfo_copy_check(&sm.view.ch, sm.view.vfo);
	if (r == VFO_COPY_OK) {
		radio_req_vfo_copy();
	}
	return r;
}
void radio_req_dmr_hear_all(bool on) { req(RREQ_DMR_HEAR_ALL, on); }
static int focus_posts;
void radio_req_dmr_focus(uint32_t id, bool p)
{
	focus_posts++;
	req(RREQ_DMR_FOCUS, (int32_t)((id & 0xFFFFFFu) | (p ? 1u << 24 : 0)));
}
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { req(RREQ_DMR_PRIVACY, (int32_t)(((uint32_t)type << 16) | key)); }
static uint16_t input_long, input_repeat;
void input_set_timing(uint16_t l, uint16_t r) { input_long = l; input_repeat = r; }
static bool ptt_blocked;
void input_set_ptt_block(bool b) { ptt_blocked = b; }
static bool dtmf_blocked;
void input_set_dtmf_block(bool b) { dtmf_blocked = b; }
static keymap_entry_t got_keymap[3];
static int keymap_sets;
void input_set_keymap(const keymap_entry_t *map) { memcpy(got_keymap, map, sizeof(got_keymap)); keymap_sets++; }
void hostlink_set_hotspot_allowed(bool on) { (void)on; }
void hostlink_settings_changed(void) { }
static int32_t ram[UI_SET_COUNT];
int32_t ui_svc_ram_get(uint8_t id) { return id < UI_SET_COUNT ? ram[id] : 0; }
bool ui_svc_ram_set(uint8_t id, int32_t v) { if (id >= UI_SET_COUNT) return false; ram[id] = v; return true; }
static ui_event_t last_ev;
static int n_ev;
bool ui_notify(const ui_event_t *ev) { last_ev = *ev; n_ev++; return true; }
static int cp_writes;
static bool cp_write_fails;
bool codeplug_set_channel(uint16_t n, const channel_t *ch)
{
	if (cp_write_fails || n < 1 || n > NCH) {
		return false;
	}
	chans[n] = *ch;
	used[n] = true;
	cp_writes++;
	cp_serial++;
	return true;
}

bool codeplug_delete_channel(uint16_t n)
{
	if (n < 1 || n > NCH) {
		return false;
	}
	used[n] = false;
	cp_serial++;
	return true;
}

// Zone writes (the zone editor's job, zone_edit.c on these fakes)
static int zone_writes;
static uint16_t zone_written;
bool codeplug_zones_stored(void) { return nzones > 0; }
uint16_t codeplug_zone_capacity(void) { return CODEPLUG_ZONE_MAX; }
uint16_t codeplug_zone_written(void) { return zone_written; }
bool codeplug_set_zone(uint16_t i, const zone_t *z)
{
	if (i > nzones || i >= 3) {
		return false;
	}
	zones[i] = *z;
	if (i == nzones) {
		nzones++;
	}
	zone_written = i;
	zone_writes++;
	cp_serial++;
	return true;
}
bool codeplug_delete_zone(uint16_t i)
{
	if (i >= nzones) {
		return false;
	}
	memmove(&zones[i], &zones[i + 1u], (size_t)(nzones - i - 1u) * sizeof(zones[0]));
	nzones--;
	zone_writes++;
	cp_serial++;
	return true;
}

static int job_runs;
static void test_job(void *ctx) { (void)ctx; job_runs++; }

#if CONFIG_DUAL_WATCH
// Dual watch through the UI's service (ui_svc_dual on radio_req_dual): any zone or the VFO; what can't work is refused
// at once, the rest goes to the radio core (radioFeaturesTest covers the watch itself)
static void test_ui_dual(void)
{
	setup_codeplug();
	start();
	req(RREQ_ZONE_POS, 0);
	const ui_dual_ref_t a = { false, 0, 0 }, tones = { false, 2, 1 }, vfo = { true, 0, 0 };
	const ui_dual_ref_t same[2] = { a, a }, empty[2] = { a, { false, 0, 2 } }, past[2] = { a, { false, 1, 1 } };
	const ui_dual_ref_t nozone[2] = { a, { false, 5, 0 } };
	CHECK(!ui_svc_dual(same, 0) && !ui_svc_dual(empty, 0) && !ui_svc_dual(past, 1) && !ui_svc_dual(nozone, 0));
	CHECK(sm.view.scan == SCAN_OFF);
	// Another zone's channel as B, selected: the watch runs, then stops on B
	const ui_dual_ref_t pair[2] = { a, tones };
	CHECK(ui_svc_dual(pair, 1) && sm.view.scan == SCAN_DUAL && sm.dual_sel == 1);
	CHECK(ui_svc_dual(pair, 1) && sm.view.scan == SCAN_DUAL);	// again: nothing changes
	{
		// The debug link's features section: the pair, the selected one, where the receiver is; it still fits a reply
		static char buf[DL_REPLY_MAX - 6 - 16];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		char at[24];
		snprintf(at, sizeof at, "dual.sel=b\ndual.at=%c\n", 'a' + sm.dual_at);
		CHECK(!t.full && strstr(buf, "dual.a=0:0\ndual.b=2:1\n") != NULL && strstr(buf, at) != NULL);
	}
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF && sm.view.zone_index == 2 && sm.view.zone_pos == 1);
	// The VFO's settings while on a channel, and the VFO as A
	req(RREQ_VFO, 1);
	req(RREQ_VFO, 0);
	channel_t v;
	CHECK(!sm.view.vfo && ui_svc_vfo_get(sm.vfo_idx, &v) && v.rx_hz == sm.vfo_ch.rx_hz && v.rx_hz != 0);
	const ui_dual_ref_t withvfo[2] = { vfo, a };
	CHECK(ui_svc_dual(withvfo, 0) && sm.view.scan == SCAN_DUAL);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.scan == SCAN_OFF && sm.view.vfo);
	// Saved while the watch is on B: powers up on the selected A all the same
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	CHECK(ui_svc_dual(pair, 0) && sm.dual_sel == 0);
	for (int i = 0; i < 1000 && sm.dual_at != 1; i++) {
		poll_for(RADIO_POLL_MS);
	}
	CHECK(sm.view.scan == SCAN_DUAL && sm.dual_at == 1 && sm.view.zone_index == 2);
	CHECK(ui_svc_dual(pair, 0));		// a request saves the position
	boot();
	CHECK(!sm.view.vfo && sm.view.zone_index == 0 && sm.view.zone_pos == 0);
}

// Two VFOs: dual watch gives each channel its own; each keeps its frequency, both come back at power on, and Copy to
// VFO goes to the one the other channel isn't on
static void test_dual_two_vfos(void)
{
	setup_codeplug();
	start();
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 146550000);
	req(RREQ_VFO, 0);
	const ui_dual_ref_t v0 = { true, 0, 0 }, v1 = { true, 0, 1 }, a = { false, 0, 0 };
	const ui_dual_ref_t vfos[2] = { v0, v1 }, same[2] = { v1, v1 };
	CHECK(!ui_svc_dual(same, 0));
	CHECK(ui_svc_dual(vfos, 1) && sm.view.scan == SCAN_DUAL);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo && sm.view.vfo_index == 1);	// stopped on B: the second VFO
	req(RREQ_VFO_SET, 446006250);
	channel_t c0, c1;
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000 && ui_svc_vfo_get(1, &c1) && c1.rx_hz == 446006250);
	// The watch hops between them: each hop tunes that VFO's own frequency
	CHECK(ui_svc_dual(vfos, 0));
	bool heard0 = false, heard1 = false;
	for (int i = 0; i < 400 && !(heard0 && heard1); i++) {
		poll_for(RADIO_POLL_MS);
		if (sm.view.vfo && sm.vfo_idx == 0 && hw.rx_hz == 146550000)
			heard0 = true;
		if (sm.view.vfo && sm.vfo_idx == 1 && hw.rx_hz == 446006250)
			heard1 = true;
	}
	CHECK(heard0 && heard1);
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo && sm.view.vfo_index == 0 && sm.view.ch.rx_hz == 146550000);
	// Both saved, and the one in use
	CHECK(ui_svc_dual(vfos, 1));
	CHECK(ui_svc_dual(NULL, 0) && sm.view.vfo_index == 1);
	boot();
	CHECK(sm.view.vfo && sm.view.vfo_index == 1 && sm.view.ch.rx_hz == 446006250);
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000);
	// Copy to VFO with B on the first VFO: A's channel goes to the second, the first is untouched
	req(RREQ_VFO, 0);
	req(RREQ_ZONE_POS, 0);
	const ui_dual_ref_t chan_vfo[2] = { a, v0 };
	CHECK(ui_svc_dual(chan_vfo, 0) && sm.dual_sel == 0);
	uint32_t ch_hz = sm.view.ch.rx_hz;
	req(RREQ_VFO_COPY, 0);
	CHECK(sm.view.vfo && sm.view.vfo_index == 1 && sm.view.ch.rx_hz == ch_hz);
	CHECK(ui_svc_vfo_get(0, &c0) && c0.rx_hz == 146550000);
}
#endif

static void test_ui_glue(void)
{
	const radio_dmr_view_t *d = &sm.view.dmr;
	settings_t *st;

	dmr_start(true);
	uint32_t f = ui_svc_feat_voice();
	CHECK((f & UI_FEAT_TIMESLOT) && (f & UI_FEAT_COLOR_CODE) && (f & UI_FEAT_HEAR_ALL) && (f & UI_FEAT_MODE) &&
	      (f & UI_FEAT_PRIVACY) && (f & UI_FEAT_MONITOR_LATCH) && (f & UI_FEAT_TALK_TARGET));
	CHECK(ui_svc_feat_storage() & UI_FEAT_FACTORY_RESET);
	// The focus goes to the radio only when it changes
	int posts = focus_posts;
	ui_svc_focus_target(3101234, true);
	ui_svc_focus_target(3101234, true);
	CHECK(focus_posts == posts + 1 && d->focus_id == 3101234 && d->focus_private);
	ui_svc_focus_target(0, false);
	CHECK(d->focus_id == 0);
	// Temporary channel changes and latches
	CHECK(ui_svc_dmr_timeslot(2) && !ui_svc_dmr_timeslot(3));
	CHECK(ui_svc_dmr_color_code(9) && !ui_svc_dmr_color_code(16));
	run(20);
	CHECK(d->ts == 2 && d->cc == 9);
	CHECK(ui_svc_hear_all(true) && d->hear_all_latch);
	CHECK(ui_svc_hear_all(false) && !d->hear_all_latch);
	CHECK(ui_svc_monitor(true) && !sm.view.monitor && !d->hear_all_latch);	// DMR: a press, toggled on release
	CHECK(ui_svc_monitor(false) && d->hear_all_latch);
	CHECK(ui_svc_monitor(true) && ui_svc_monitor(false) && !d->hear_all_latch);
	// Save to channel: nothing written by the call itself, the housekeeping job writes timeslot and color code
	int w = cp_writes;
	CHECK(ui_svc_save_channel());
	CHECK(cp_writes == w && storage_job_busy() && !ui_svc_save_channel());
	storage_job_poll();
	CHECK(cp_writes == w + 1 && chans[4].timeslot == 2 && chans[4].color_code == 9 && !storage_job_busy());
	run(100);			// the radio core re-reads the channel (codeplug serial)
	CHECK(!sm.view.temporary && d->ts == 2 && d->cc == 9);
	chans[4].timeslot = 1;
	chans[4].color_code = 2;
	reload_channel();
	// The channel editor's whole channel (ui_svc_write_channel): numbers out of range are refused, a write in
	// progress refuses the next, the channel in use drops its temporary changes. So does a write of another channel:
	// any codeplug write reloads the channel in use (ui.md 2.1)
	channel_t ed = chans[4];
	CHECK(!ui_svc_write_channel(0, &ed) && !ui_svc_write_channel(CODEPLUG_CHANNELS_MAX + 1, &ed));
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	CHECK(sm.view.temporary);
	ed.timeslot = 2;
	ed.color_code = 5;
	w = cp_writes;
	CHECK(ui_svc_write_channel(4, &ed) && cp_writes == w && !ui_svc_write_channel(6, &ed));
	storage_job_poll();
	CHECK(cp_writes == w + 1 && chans[4].timeslot == 2 && chans[4].color_code == 5);
	run(100);
	CHECK(!sm.view.temporary && d->ts == 2 && d->cc == 5);
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	ed = chans[6];
	ed.color_code = 7;
	CHECK(ui_svc_write_channel(6, &ed));
	storage_job_poll();
	run(100);
	CHECK(chans[6].color_code == 7 && !sm.view.temporary && d->cc == 5);
	// A failed write: the event, and nothing reloaded
	CHECK(ui_svc_dmr_color_code(9));
	run(20);
	cp_write_fails = true;
	n_ev = 0;
	CHECK(ui_svc_write_channel(6, &ed));
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && sm.view.temporary);
	cp_write_fails = false;
	req(RREQ_CLEAR_TEMP, 0);
	chans[4].timeslot = 1;
	chans[4].color_code = 2;
	reload_channel();
	// A failed write comes back as an event
	CHECK(ui_svc_dmr_color_code(4));
	cp_write_fails = true;
	n_ev = 0;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(last_ev.text, "not saved") != NULL);
	CHECK(sm.view.temporary);
	cp_write_fails = false;
	req(RREQ_CLEAR_TEMP, 0);
	// The permanent talk target: at once for now, then the channel's contact once the job ran
	CHECK(ui_svc_talk_to(9, false, UI_TALK_PERMANENT));
	run(20);
	CHECK(d->target_id == 9);
	storage_job_poll();
	CHECK(chans[4].contact == 3);
	run(100);
	CHECK(d->target_id == 9 && !d->target_manual);
	n_ev = 0;
	CHECK(ui_svc_talk_to(3109999, true, UI_TALK_PERMANENT));	// not a stored contact: stays for now
	storage_job_poll();
	run(20);
	CHECK(n_ev == 1 && strstr(last_ev.text, "for now only") != NULL && d->target_manual && d->target_id == 3109999);
	chans[4].contact = 1;
	reload_channel();
	// Mode
	CHECK(ui_svc_mode(RADIO_MODE_FM) && sm.view.ch.mode == RADIO_MODE_FM && !ui_svc_mode(7));
	req(RREQ_CLEAR_TEMP, 0);
	run(50);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR);
#if CONFIG_ZONE_EDIT
	{
		// The zone editor's queue (ui_svc_zone_edit, ui.md 2.2): nothing is written by the call; what is queued runs in
		// order in one job; a refusal drops what was queued after it; a full queue refuses
		zone_t keep[3];
		uint16_t nkeep = nzones;
		memcpy(keep, zones, sizeof keep);
		zone_op_t op, q[4];
		memset(q, 0, sizeof q);
		uint16_t num = sm.view.channel_number, pos = sm.view.zone_pos;
		CHECK(sm.view.zone_index == 0 && pos > 0);
		q[0].kind = ZONE_OP_MOVE;	// up
		q[0].pos = pos;
		q[0].dir = -1;
		q[1] = q[0];			// and back down
		q[1].pos = (uint16_t)(pos - 1u);
		q[1].dir = 1;
		q[2].kind = ZONE_OP_INSERT;	// refused: it is in the zone
		q[2].channel = num;
		q[3] = q[0];			// dropped
		int zw = zone_writes;
		n_ev = 0;
		for (int i = 0; i < 4; i++) {
			CHECK(ui_svc_zone_edit(&q[i]));
		}
		CHECK(!ui_svc_zone_edit(&q[0]) && zone_writes == zw && ui_svc_zone_pending());
		storage_job_poll();
		run(100);
		CHECK(zone_writes == zw + 2 && zones[0].channels[pos] == num && !ui_svc_zone_pending());
		CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(last_ev.text, "Already") != NULL);
		uint32_t applied;
		uint8_t last;
		zone_edit_stats(&applied, &last);
		CHECK(applied >= 2 && last == ZONE_EDIT_IN_ZONE);
		// A move in the zone in use keeps the radio on its channel at its new position
		CHECK(ui_svc_zone_edit(&q[0]));
		storage_job_poll();
		run(100);
		CHECK(zones[0].channels[pos - 1u] == num);
		CHECK(sm.view.channel_number == num && sm.view.zone_pos == pos - 1u && sm.view.zone_index == 0);
		// Removing the channel in use: the radio goes to the one that took its place (the next usable one)
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_REMOVE;
		op.pos = (uint16_t)(pos - 1u);
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number != num && sm.view.zone_pos >= pos - 1u &&
		      zones[0].channels[sm.view.zone_pos] == sm.view.channel_number);
		memcpy(zones, keep, sizeof keep);
		cp_serial++;
		run(100);

		// Deleting a zone ahead of the zone in use: the radio follows its zone to the index it moved to, and so do dual
		// watch's channels and the roaming zones; a deleted roaming zone loses its flag
		req(RREQ_ZONE, 2);
		run(50);
		CHECK(sm.view.zone_index == 2 && sm.view.channel_number == 7);
		req(RREQ_ZONE_POS, 1);
		run(50);
#if CONFIG_SCAN
		radio_dual_ref_t dual_keep[2] = { sm.dual[0], sm.dual[1] };
		sm.dual[0] = (radio_dual_ref_t){ .vfo = false, .zone = 2, .pos = 0 };
		sm.dual[1] = (radio_dual_ref_t){ .vfo = false, .zone = 0, .pos = 1 };
#endif
#if CONFIG_DMR_ROAMING
		st = settings_begin();
		st->roam_zones[0] = 0x05;	// zones 0 and 2
		settings_end(0);
#endif
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_DELETE;
		op.zone = 0;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(nzones == 2 && sm.view.zone_index == 1 && sm.view.channel_number == 8 && sm.view.zone_pos == 1);
#if CONFIG_SCAN
		CHECK(sm.dual[0].zone == 1 && sm.dual[1].zone == 0);	// the deleted one's index: the zone that took it
		sm.dual[0] = dual_keep[0];
		sm.dual[1] = dual_keep[1];
#endif
#if CONFIG_DMR_ROAMING
		settings_t saved;
		settings_read(&saved);
		CHECK(sm.roam_zones[0] == 0x02 && saved.roam_zones[0] == 0x02);	// zone 2 is zone 1; zone 0's flag is gone
		st = settings_begin();
		st->roam_zones[0] = 0;
		settings_end(0);
#endif
		// Deleting the zone in use, the last one: the radio goes to the zone before it, on its first channel (it
		// doesn't hold channel 8)
		op.zone = 1;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(nzones == 1 && sm.view.zone_index == 0 && sm.view.channel_number == 6 && sm.view.zone_pos == 0);
		// An outside writer's entries (an import), seen only when its write is done: zone_edit_busy holds the reload
		memcpy(zones, keep, sizeof keep);
		nzones = nkeep;
		zone_shift_busy(true);
		zone_shift_note(false, 0);	// as if zone 0 (Local) had come in ahead of Two and Tones
		cp_serial++;
		run(100);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number == 6);	// held
		zone_shift_busy(false);
		run(100);
		CHECK(sm.view.zone_index == 1 && sm.view.channel_number == 6);	// Two is zone 1
		CHECK(sm.view.zone_index == 1);
#if CONFIG_SCAN
		// A zone scan ends when the zone in use's list changes under it (its skips are by position); a rename doesn't
		req(RREQ_ZONE, 0);
		run(50);
		req(RREQ_SCAN, RSCAN_ZONE);
		run(200);
		CHECK(sm.scan.kind == SCAN_ZONE);
		memset(&op, 0, sizeof op);
		op.kind = ZONE_OP_RENAME;
		strcpy(op.name, "Renamed");
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.scan.kind == SCAN_ZONE && strcmp(zones[0].name, "Renamed") == 0);
		op.kind = ZONE_OP_MOVE;
		op.pos = 0;
		op.dir = 1;
		CHECK(ui_svc_zone_edit(&op));
		storage_job_poll();
		run(100);
		CHECK(sm.scan.kind == SCAN_OFF);
#endif
		memcpy(zones, keep, sizeof keep);
		nzones = nkeep;
		cp_serial++;
		run(100);
		req(RREQ_ZONE, 0);
		run(50);
		req(RREQ_ZONE_POS, pos);
		run(50);
		CHECK(sm.view.zone_index == 0 && sm.view.channel_number == num);
	}
#endif

	// Privacy: no key stored, nothing to turn on; then the first valid slot is used
	CHECK(!ui_svc_privacy(true) && !d->privacy);
	st = settings_begin();
	st->privacy_type[1] = 1;	// Motorola basic privacy, key 7, slot 2 (none active)
	st->privacy_key[1] = 7;
	settings_end(now);
	CHECK(ui_svc_privacy(true) && d->privacy);
	CHECK(ui_svc_privacy(false) && !d->privacy);

	// Settings with a store field now: kept across "power cycles" (settings_t), the reply window in the radio core
	CHECK(ui_svc_setting_set(UI_SET_AUTO_LOCK_S, 60) && ui_svc_setting_get(UI_SET_AUTO_LOCK_S) == 60);
	CHECK(ui_svc_setting_set(UI_SET_MIC_GAIN, 8) && ui_svc_setting_get(UI_SET_MIC_GAIN) == 8);
	CHECK(ui_svc_setting_set(UI_SET_LONG_PRESS_MS, 900) && ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 900);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 1) && ui_svc_setting_get(UI_SET_GPS) == 1);
	CHECK(ui_svc_setting_set(UI_SET_POS_SEND_S, 300) && ui_svc_setting_get(UI_SET_POS_SEND_S) == 300);
	settings_t sv;
	settings_read(&sv);
	CHECK(sv.auto_lock_s == 60 && sv.mic_gain == 8 && sv.long_press_ms == 900 && sv.gps == 1 && sv.pos_send_s == 300);
#if CONFIG_DUAL_WATCH
	// Dual watch's B, and whether the watch ran and which channel was selected (it comes back at power on)
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 37) && ui_svc_setting_get(UI_SET_DUAL_B) == 37);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, UI_DUAL_B_VFO | UI_DUAL_ON));
	CHECK(ui_svc_setting_get(UI_SET_DUAL_B) == (UI_DUAL_B_VFO | UI_DUAL_ON));
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 1024 | UI_DUAL_ON | UI_DUAL_B_SEL));
	CHECK(ui_svc_setting_get(UI_SET_DUAL_B) == (1024 | UI_DUAL_ON | UI_DUAL_B_SEL));
	settings_read(&sv);
	CHECK(sv.dual_b == 1024 && sv.dual_state == (SETTINGS_DUAL_ON | SETTINGS_DUAL_B_SEL) && sv.mic_gain == 8);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, UI_DUAL_B_VFO2) && ui_svc_setting_get(UI_SET_DUAL_B) == UI_DUAL_B_VFO2);
	CHECK(ui_svc_setting_set(UI_SET_DUAL_B, 0) && ui_svc_setting_get(UI_SET_DUAL_B) == 0);
#endif
	// Mic noise reduction: on by default, kept in accent bits 6-7, handed to the vocoder's encode options (tone frames
	// stay on)
	CHECK(ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 1 && (sv.accent & SETTINGS_AUDIO_NO_NOISE_REDUCTION) == 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 0) && ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 0);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_TONE_FRAMES);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == SETTINGS_AUDIO_NO_NOISE_REDUCTION);
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 7) && ui_svc_setting_get(UI_SET_NOISE_REDUCTION) == 1);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 1);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	// Mic AGC: off by default, its own bit, added to the options in force
	CHECK(ui_svc_setting_get(UI_SET_MIC_AGC) == 0);
	CHECK(ui_svc_setting_set(UI_SET_MIC_AGC, 1) && ui_svc_setting_get(UI_SET_MIC_AGC) == 1);
	ui_svc_apply(UI_SET_MIC_AGC, 1);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == SETTINGS_AUDIO_AGC);
	// ... and kept when noise reduction changes
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 0));
	ui_svc_apply(UI_SET_NOISE_REDUCTION, 0);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == (SETTINGS_AUDIO_AGC | SETTINGS_AUDIO_NO_NOISE_REDUCTION));
	CHECK(ui_svc_setting_set(UI_SET_NOISE_REDUCTION, 1) && ui_svc_setting_set(UI_SET_MIC_AGC, 0));
	ui_svc_apply(UI_SET_MIC_AGC, 0);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	// The debug link's SETTING 31 sets the options in RAM, any combination (0 = the codec's plain 0x800 word); the
	// store is not touched, and the next settings change brings theirs back
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 0, 0 }, 3) == DL_ST_OK);
	CHECK(ambeVoiceEncodeOptions() == 0);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 7, 0 }, 3) == DL_ST_OK);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 8, 0 }, 3) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_SETTING, (const uint8_t[]){ 31, 0, 1 }, 3) == DL_ST_BAD_ARG);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC));
	settings_read(&sv);
	CHECK((sv.accent & SETTINGS_AUDIO_MASK) == 0);
	ui_svc_apply(UI_SET_NOISE_REDUCTION, ui_svc_setting_get(UI_SET_NOISE_REDUCTION));
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT);
	// Settings > Location > GPS reaches the board: the stored value at start-up (ui_app applies every setting), then
	// each change (the setting store keeps it either way)
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 1 && gps_enabled);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 0));
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 2 && !gps_enabled);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 1));
	ui_svc_apply(UI_SET_GPS, ui_svc_setting_get(UI_SET_GPS));
	CHECK(gps_enable_calls == 3 && gps_enabled);
	ui_svc_apply(UI_SET_LONG_PRESS_MS, 900);
	ui_svc_apply(UI_SET_REPEAT_MS, 200);
	CHECK(input_long == 900 && input_repeat == 200);
	// The UI's own settings (settings_t.ui_flags and the fields after it)
	CHECK(ui_svc_setting_get(UI_SET_HINTS) == 1 && ui_svc_setting_get(UI_SET_LARGE_TEXT) == 0);
	CHECK(ui_svc_setting_get(UI_SET_PTT_LOCKED) == 1 && ui_svc_setting_get(UI_SET_LOW_BATTERY) == 15);
	CHECK(ui_svc_setting_set(UI_SET_LARGE_TEXT, 1) && ui_svc_setting_set(UI_SET_HINTS, 0));
	CHECK(ui_svc_setting_set(UI_SET_PTT_LOCKED, 0) && ui_svc_setting_set(UI_SET_HEAR_ALL_DEFAULT, 1));
	CHECK(ui_svc_setting_set(UI_SET_THEME, 2) && ui_svc_setting_set(UI_SET_UPDOWN_HOME, 1));
	CHECK(ui_svc_setting_set(UI_SET_AUTO_OFF_MIN, 30) && ui_svc_setting_set(UI_SET_LOW_BATTERY, 20));
	settings_read(&sv);
	CHECK(sv.ui_flags == (SETTINGS_UI_LARGE_TEXT | SETTINGS_UI_MESSAGE_TONE | SETTINGS_UI_ALIAS | SETTINGS_UI_HEAR_ALL));
	CHECK(sv.theme == 2 && sv.updown_home == 1 && sv.auto_off_min == 30 && sv.low_batt_pct == 20);
	// Vibrate: on by default, kept as an "off" bit so records saved before it keep it on
	CHECK(ui_svc_setting_get(UI_SET_VIBRATE) == 1);
	CHECK(ui_svc_setting_set(UI_SET_VIBRATE, 0) && ui_svc_setting_get(UI_SET_VIBRATE) == 0);
	settings_read(&sv);
	CHECK(sv.ui_flags == (SETTINGS_UI_LARGE_TEXT | SETTINGS_UI_MESSAGE_TONE | SETTINGS_UI_ALIAS |
			      SETTINGS_UI_HEAR_ALL | SETTINGS_UI_NO_VIBRATE));
	CHECK(ui_svc_setting_set(UI_SET_VIBRATE, 1) && ui_svc_setting_get(UI_SET_VIBRATE) == 1);
	settings_read(&sv);
	CHECK(!(sv.ui_flags & SETTINGS_UI_NO_VIBRATE));
	// The setting is usable only on a board with a motor; ui_svc_vibrate reaches it through the radio core
	CHECK(!(ui_svc_feat_radio() & UI_FEAT_VIBRATE));
	vibrations = 0;
	ui_svc_vibrate(UI_VIBE_CALL);
	run(100);
	CHECK(vibrations == 0 && !vibe_busy());
	fake_power.vibrate = p_vibrate;
	CHECK(ui_svc_feat_radio() & UI_FEAT_VIBRATE);
	ui_svc_vibrate(UI_VIBE_MESSAGE);
	ui_svc_vibrate(200);		// not a pattern: nothing
	run(1000);
	CHECK(vibrations == 2 && !vibe_busy());
	ui_svc_vibrate(UI_VIBE_EMERGENCY);
	run(3000);
	CHECK(vibrations == 6);
	fake_power.vibrate = NULL;
	ui_svc_ptt_block(true);
	CHECK(ptt_blocked);
	ui_svc_ptt_block(false);
	CHECK(!ptt_blocked);
	ui_svc_dtmf_block(true);
	CHECK(dtmf_blocked);
	ui_svc_dtmf_block(false);
	CHECK(!dtmf_blocked);
	// Talk permit tone: the radio core's, saved with its settings
	CHECK(ui_svc_setting_get(UI_SET_TALK_PERMIT) == 1);
	ui_svc_apply(UI_SET_TALK_PERMIT, 0);
	run(20);
	CHECK(!sm.set.talk_permit && ui_svc_setting_get(UI_SET_TALK_PERMIT) == 0);
	settings_read(&sv);
	CHECK(sv.rs_flags & SETTINGS_RS_NO_TALK_PERMIT);
	ui_svc_apply(UI_SET_TALK_PERMIT, 1);
	run(20);
	CHECK(sm.set.talk_permit);
	// Reply window: the value shown at start-up for "the codeplug's" keeps it; another value is the radio's own
	CHECK(ui_svc_setting_get(UI_SET_REPLY_S) == 10);
	ui_svc_apply(UI_SET_REPLY_S, 10);
	CHECK(sm.set.reply_s == RADIO_REPLY_CODEPLUG);
	ui_svc_apply(UI_SET_REPLY_S, 4);
	CHECK(sm.set.reply_s == 4 && ui_svc_setting_get(UI_SET_REPLY_S) == 4);
	ui_svc_apply(UI_SET_REPLY_S, 10);
	CHECK(sm.set.reply_s == 10);
	// VOX: refused on a radio that can't measure the mic (the UI says so instead of "VOX on"); with a mic level, on
	// every FM channel, off again (the radio's settings)
	CHECK(!ui_svc_vox(true));
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_OFF);
	fake_power.mic_level = p_mic;
	CHECK(ui_svc_vox(true));
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_ALL);
	ui_svc_vox(false);
	run(5);
	CHECK(sm.set.vox == RADIO_VOX_OFF);
	fake_power.mic_level = NULL;

	// Messages: delete through the data service
	CHECK(!ui_svc_msg_delete(0));

	// Factory reset: a job; the settings are the defaults afterwards (no flash here: RAM only, no restart)
	CHECK(ui_svc_factory_reset() && !ui_svc_factory_reset());
	storage_job_poll();
	settings_read(&sv);
	CHECK(sv.auto_lock_s == 0 && sv.mic_gain == 5 && sv.privacy_type[1] == 0 && sv.rs_stored == 0);
	run(100);
	CHECK(sm.set.reply_s == RADIO_REPLY_CODEPLUG && sm.set.vox == RADIO_VOX_OFF);
}

// Every setting ui_svc_storage.c answers: where it is read from, the clamps on the way in, the privacy key slot,
// and the codeplug jobs' refusals (no channel, a job still pending, the channel unreadable)
static void test_ui_settings(void)
{
	settings_t sv;
	radio_settings_t rs;

	dmr_start(true);
	// The radio core's own settings, read back through radio_get_settings
	radio_get_settings(&rs);
	rs.key_beep = true;
	rs.beep_level = 65;		// shown in tenths, rounded: 7
	rs.tot_s = 90;
	rs.backlight_dim_s = 20;
	rs.rx_save = 2;
	rs.roam_survey = 2;
	rs.roam_beacon_s = 120;
	rs.roam_beacon_len_s = 6;
	rs.roam_probe = 1;
	radio_set_settings(&rs);
	CHECK(ui_svc_setting_get(UI_SET_KEY_TONES) == 1 && ui_svc_setting_get(UI_SET_TONE_VOLUME) == 7);
	CHECK(ui_svc_setting_get(UI_SET_TOT_S) == 90 && ui_svc_setting_get(UI_SET_BACKLIGHT_S) == 20);
	CHECK(ui_svc_setting_get(UI_SET_BATTERY_SAVER) == 1);
	CHECK(ui_svc_setting_get(UI_SET_ROAM_SURVEY) == 2 && ui_svc_setting_get(UI_SET_ROAM_BEACON_S) == 120 &&
	      ui_svc_setting_get(UI_SET_ROAM_BEACON_LEN) == 6 && ui_svc_setting_get(UI_SET_ROAM_PROBE) == 1);
	// ... which the store leaves to ui_svc_apply (nothing written here)
	settings_read(&sv);
	CHECK(ui_svc_setting_set(UI_SET_KEY_TONES, 0) && ui_svc_setting_set(UI_SET_ROAM_PROBE, 0));
	settings_t sv2;
	settings_read(&sv2);
	CHECK(memcmp(&sv, &sv2, sizeof(sv)) == 0);

	// Preferences in settings_t, with their clamps and the fallbacks for out of range stored values
	CHECK(ui_svc_setting_set(UI_SET_BRIGHTNESS, 3) && ui_svc_setting_get(UI_SET_BRIGHTNESS) == 10);
	CHECK(ui_svc_setting_set(UI_SET_BRIGHTNESS, 250) && ui_svc_setting_get(UI_SET_BRIGHTNESS) == 100);
	CHECK(ui_svc_setting_set(UI_SET_SQUELCH, -4) && ui_svc_setting_get(UI_SET_SQUELCH) == 0);
	CHECK(ui_svc_setting_set(UI_SET_SQUELCH, 12) && ui_svc_setting_get(UI_SET_SQUELCH) == 9);
	CHECK(ui_svc_setting_set(UI_SET_HOTSPOT_AUTO, 0) && ui_svc_setting_get(UI_SET_HOTSPOT_AUTO) == 0);
	CHECK(ui_svc_setting_set(UI_SET_HOTSPOT_AUTO, 5) && ui_svc_setting_get(UI_SET_HOTSPOT_AUTO) == 1);
	CHECK(ui_svc_setting_set(UI_SET_MIC_GAIN, 40) && ui_svc_setting_get(UI_SET_MIC_GAIN) == 10);
	CHECK(ui_svc_setting_set(UI_SET_AUTO_LOCK_S, 99999) && ui_svc_setting_get(UI_SET_AUTO_LOCK_S) == 3600);
	CHECK(ui_svc_setting_set(UI_SET_LONG_PRESS_MS, 10) && ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 200);
	CHECK(ui_svc_setting_set(UI_SET_REPEAT_MS, 5000) && ui_svc_setting_get(UI_SET_REPEAT_MS) == 1000);
	CHECK(ui_svc_setting_set(UI_SET_POS_FORMAT, 9) && ui_svc_setting_get(UI_SET_POS_FORMAT) == 2);
	CHECK(ui_svc_setting_set(UI_SET_POS_SEND_S, -1) && ui_svc_setting_get(UI_SET_POS_SEND_S) == 0);
	CHECK(ui_svc_setting_set(UI_SET_GPS, 0) && ui_svc_setting_get(UI_SET_GPS) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, -3) && ui_svc_setting_get(UI_SET_THEME) == 0);
	// The highlight color and the filled strip share settings_t.accent: setting one keeps the other. A zero byte (new
	// or older records) is the defaults: the theme's highlight, the strip filled
	CHECK(ui_svc_setting_get(UI_SET_ACCENT) == 0 && ui_svc_setting_get(UI_SET_STRIP) == 1);
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 2) && ui_svc_setting_set(UI_SET_STRIP, 0));
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 9) && ui_svc_setting_get(UI_SET_ACCENT) == 7 &&
	      ui_svc_setting_get(UI_SET_STRIP) == 0);
	settings_read(&sv);
	CHECK(sv.accent == (7 | SETTINGS_ACCENT_PLAIN));
	CHECK(ui_svc_setting_set(UI_SET_STRIP, 1) && ui_svc_setting_get(UI_SET_ACCENT) == 7);
	CHECK(ui_svc_setting_set(UI_SET_ACCENT, 0) && ui_svc_setting_get(UI_SET_STRIP) == 1);
	settings_read(&sv);
	CHECK(sv.accent == 0);
	CHECK(ui_svc_setting_set(UI_SET_UPDOWN_HOME, 7) && ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 2);
#if CONFIG_CRASH_REPORT
	// The crash report's mode and target share theme, updown_home and accent (settings.h): each keeps the others
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, 1) && ui_svc_setting_set(UI_SET_ACCENT, 3) && ui_svc_setting_set(UI_SET_STRIP, 0));
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, 9) && ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_AUTO);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, -1) && ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, UI_CRASH_SEND_ASK));
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, CODEPLUG_CONTACTS_MAX + 5) &&
	      ui_svc_setting_get(UI_SET_CRASH_CONTACT) == CODEPLUG_CONTACTS_MAX);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, -3) && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 0);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_CONTACT, 1000));
	CHECK(ui_svc_setting_get(UI_SET_THEME) == 1 && ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 2 &&
	      ui_svc_setting_get(UI_SET_ACCENT) == 3 && ui_svc_setting_get(UI_SET_STRIP) == 0);
	CHECK(ui_svc_setting_set(UI_SET_THEME, 2) && ui_svc_setting_set(UI_SET_UPDOWN_HOME, 1) &&
	      ui_svc_setting_set(UI_SET_ACCENT, 5) && ui_svc_setting_set(UI_SET_STRIP, 1));
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_ASK && ui_svc_setting_get(UI_SET_CRASH_CONTACT) == 1000);
	// A stored mode of 3 (no such mode): off
	settings_t *cs = settings_begin();
	settings_set_crash_mode(cs, 3);
	settings_end(now);
	CHECK(ui_svc_setting_get(UI_SET_CRASH_MODE) == UI_CRASH_SEND_OFF);
	CHECK(ui_svc_setting_set(UI_SET_CRASH_MODE, UI_CRASH_SEND_OFF) && ui_svc_setting_set(UI_SET_CRASH_CONTACT, 0) &&
	      ui_svc_setting_set(UI_SET_THEME, 0) && ui_svc_setting_set(UI_SET_ACCENT, 0) &&
	      ui_svc_setting_set(UI_SET_UPDOWN_HOME, 2));
#endif
	CHECK(ui_svc_setting_set(UI_SET_LOW_BATTERY, 80) && ui_svc_setting_get(UI_SET_LOW_BATTERY) == 50);
	CHECK(ui_svc_setting_set(UI_SET_AUTO_OFF_MIN, 100000) && ui_svc_setting_get(UI_SET_AUTO_OFF_MIN) == 1440);
	CHECK(ui_svc_setting_set(UI_SET_MESSAGE_TONE, 0) && ui_svc_setting_get(UI_SET_MESSAGE_TONE) == 0);
	CHECK(ui_svc_setting_set(UI_SET_ALIAS_SHOW, 0) && ui_svc_setting_get(UI_SET_ALIAS_SHOW) == 0);
	CHECK(ui_svc_setting_set(UI_SET_TFTP_WRITE, 0) && ui_svc_setting_get(UI_SET_TFTP_WRITE) == 0);
	settings_read(&sv);
	CHECK(sv.net_flags & SETTINGS_NET_TFTP_NO_WRITE);
	CHECK(ui_svc_setting_set(UI_SET_TFTP_WRITE, 1) && ui_svc_setting_get(UI_SET_TFTP_WRITE) == 1);
	// Stored values another firmware (or a damaged record) left out of range: the defaults shown
	settings_t *st = settings_begin();
	st->brightness = 3;
	st->squelch = 0xEE;
	st->mic_gain = 77;
	st->long_press_ms = 0;
	st->repeat_ms = 0;
	st->pos_format = 9;
	st->theme = 7;		// bits 0-1 out of range (the bits above hold the crash report's target)
	st->updown_home = 7;
	st->privacy_type[0] = 99;
	settings_end(now);
	ram[UI_SET_SQUELCH] = 3;
	ram[UI_SET_LONG_PRESS_MS] = 777;
	ram[UI_SET_REPEAT_MS] = 111;
	CHECK(ui_svc_setting_get(UI_SET_BRIGHTNESS) == 100 && ui_svc_setting_get(UI_SET_SQUELCH) == 3 &&
	      ui_svc_setting_get(UI_SET_MIC_GAIN) == 5);
	CHECK(ui_svc_setting_get(UI_SET_LONG_PRESS_MS) == 777 && ui_svc_setting_get(UI_SET_REPEAT_MS) == 111);
	CHECK(ui_svc_setting_get(UI_SET_POS_FORMAT) == 0 && ui_svc_setting_get(UI_SET_THEME) == 0 &&
	      ui_svc_setting_get(UI_SET_UPDOWN_HOME) == 0);
	CHECK(ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == DMR_PRIVACY_OFF);
	// Settings nobody stores: the RAM table
	CHECK(ui_svc_setting_set(UI_SET_VOICE_PROMPTS, 1) && ram[UI_SET_VOICE_PROMPTS] == 1 &&
	      ui_svc_setting_get(UI_SET_VOICE_PROMPTS) == 1);

	// The data service's answers and message format
	CHECK(ui_svc_setting_set(UI_SET_CHECK_REPLY, 0) && ui_svc_setting_get(UI_SET_CHECK_REPLY) == 0);
	CHECK(ui_svc_setting_set(UI_SET_ALERT_ANSWER, 0) && ui_svc_setting_get(UI_SET_ALERT_ANSWER) == 0);
	CHECK(ui_svc_setting_set(UI_SET_POS_REPLY, 1) && ui_svc_setting_get(UI_SET_POS_REPLY) == 1);
	CHECK(ui_svc_setting_set(UI_SET_MSG_FORMAT, DMR_SMS_FORMAT_ETSI) &&
	      ui_svc_setting_get(UI_SET_MSG_FORMAT) == DMR_SMS_FORMAT_ETSI);
	CHECK(!ui_svc_setting_set(UI_SET_MSG_FORMAT, 9) && ui_svc_setting_get(UI_SET_MSG_FORMAT) == DMR_SMS_FORMAT_ETSI);
	CHECK(ui_svc_setting_set(UI_SET_MSG_FORMAT, DMR_SMS_FORMAT_ETSI_IP));

	// USB personality and the control station switches (no control station in this build: those stay off)
	CHECK(ui_svc_setting_set(UI_SET_USB_BOOT_NET, 1) && ui_svc_setting_get(UI_SET_USB_BOOT_NET) == 1);
	CHECK(ui_svc_setting_set(UI_SET_PC_ENDPOINT, 1) && ui_svc_setting_set(UI_SET_USB_FIXED_LINK, 1));
	CHECK(ui_svc_setting_get(UI_SET_PC_ENDPOINT) == CONFIG_DMR_CONTROL_STATION &&
	      ui_svc_setting_get(UI_SET_USB_FIXED_LINK) == CONFIG_DMR_CONTROL_STATION);
	CHECK(ui_svc_setting_set(UI_SET_USB_BOOT_NET, 0) && ui_svc_setting_get(UI_SET_USB_BOOT_NET) == 0);

	// Privacy key slot 1: the type and the key, kept in range; off clears the key and turns privacy off when
	// slot 1 was the active one; with privacy on, the radio core gets the new key at once
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_MOTOROLA_BP));
	settings_read(&sv);
	CHECK(sv.privacy_type[0] == DMR_PRIVACY_MOTOROLA_BP && sv.privacy_key[0] == 1);	// key 0: the lowest valid
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 70000) && ui_svc_setting_get(UI_SET_PRIVACY_KEY) == 255);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 42) && ui_svc_setting_get(UI_SET_PRIVACY_KEY) == 42);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, 50) &&
	      ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == DMR_PRIVACY_NUM_TYPES - 1);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_MOTOROLA_BP));
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 42));
	st = settings_begin();
	st->privacy_active = 1;
	settings_end(now);
	CHECK(ui_svc_privacy(true));
	run(20);
	CHECK(sm.view.dmr.privacy);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_KEY, 43));
	run(20);
	settings_read(&sv);
	CHECK(sv.privacy_key[0] == 43 && sm.view.dmr.privacy);
	CHECK(ui_svc_setting_set(UI_SET_PRIVACY_TYPE, DMR_PRIVACY_OFF));
	run(20);
	settings_read(&sv);
	CHECK(sv.privacy_type[0] == DMR_PRIVACY_OFF && sv.privacy_key[0] == 0 && sv.privacy_active == 0);
	CHECK(!sm.view.dmr.privacy);

	// Codeplug jobs: refused off a stored channel and while one is pending; a channel that can't be read is
	// reported, nothing written
	int w = cp_writes;
	req(RREQ_VFO, 1);
	run(50);
	CHECK(sm.view.vfo && !ui_svc_save_channel());
#if CONFIG_VFO_DMR
	CHECK(ui_svc_save_talk_target(3100, false) && cp_writes == w);	// the VFO's own target: nothing to write
	run(20);
	CHECK(sm.vfo_talk_id == 3100 && !sm.vfo_talk_private);
#else
	CHECK(!ui_svc_save_talk_target(3100, false));
#endif
	sm.vfo_talk_id = 0;
	req(RREQ_VFO, 0);
	run(50);
	CHECK(!sm.view.vfo && sm.view.channel_number == 4);
	CHECK(!ui_svc_save_talk_target(0, false));
	CHECK(storage_job_post(test_job, NULL));
	CHECK(!ui_svc_save_channel() && !ui_svc_save_talk_target(9, false) && !ui_svc_factory_reset());
	storage_job_poll();
	used[4] = false;			// the channel is gone from the codeplug before the job runs
	n_ev = 0;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(last_ev.text, "read the channel") != NULL);
	n_ev = 0;
	CHECK(ui_svc_save_talk_target(9, false));
	storage_job_poll();
	CHECK(n_ev == 1 && strstr(last_ev.text, "for now only") != NULL);
	used[4] = true;
	cp_write_fails = true;			// the contact found, the write fails
	n_ev = 0;
	CHECK(ui_svc_save_talk_target(9, false));
	storage_job_poll();
	CHECK(n_ev == 1 && strstr(last_ev.text, "write the codeplug") != NULL);
	cp_write_fails = false;
	CHECK(cp_writes == w);
	// The squelch saved with the channel goes to the default squelch preference
	ui_svc_setting_set(UI_SET_SQUELCH, 2);
	req(RREQ_SQUELCH, 1);
	run(20);
	CHECK(sm.view.temp & RADIO_TEMP_SQUELCH);
	uint8_t sq = sm.view.squelch;
	CHECK(sq != 2 && ui_svc_save_channel());
	storage_job_poll();
	settings_read(&sv);
	CHECK(sv.squelch == sq && cp_writes == w + 1);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
	// Every temporary change goes into the channel: power, bandwidth, both tones, the mode
	req(RREQ_ZONE_POS, 0);			// channel 1, FM simplex
	run(50);
	req(RREQ_POWER, 1);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 1000 }));
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_CTCSS, 1230 }));
	run(20);
	uint8_t pw = sm.view.power;
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(chans[1].power == pw && chans[1].bw == RADIO_BW_12K5 && chans[1].tx_tone.type == TONE_CTCSS &&
	      chans[1].tx_tone.value == 1000 && chans[1].rx_tone.value == 1230);
	run(100);				// the radio core re-reads the channel (codeplug serial)
	CHECK(ui_svc_mode(RADIO_MODE_DMR) && (sm.view.temp & RADIO_TEMP_MODE));
	run(20);
	CHECK(ui_svc_save_channel());
	storage_job_poll();
	CHECK(chans[1].mode == RADIO_MODE_DMR);
	setup_codeplug();
	char l1[17], l2[17];
	ui_svc_boot_text(l1, l2);
	CHECK(l1[0] == 0 && l2[0] == 0);
	req(RREQ_CLEAR_TEMP, 0);
	run(20);
}

// ---- The DMR data service on the radio core (radio_sm.c's data glue): what it lets out, keyed like PTT, and the
// answers it hands back ----

static void csbk_rx(uint8_t opcode, uint32_t target, uint32_t from, uint8_t cc)
{
	uint8_t c[10] = { (uint8_t)(0x80 | opcode), DMR_FID_ETSI, 0, 0, (uint8_t)(target >> 16), (uint8_t)(target >> 8),
			  (uint8_t)target, (uint8_t)(from >> 16), (uint8_t)(from >> 8), (uint8_t)from };
	dmrBurst_t d;
	CHECK(dmrDataBuildCSBK(c, &d) == 1);
	dmr_burst_t b;
	memset(&b, 0, sizeof(b));
	b.kind = DMR_BURST_CSBK;
	b.sync = DMR_SYNC_MS;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.len = (uint8_t)d.length;
	memcpy(b.payload, d.payload, d.length);
	sim_rx(&b);
}

// Bursts on air since n0 of this kind, with this CSBK opcode (0xFF: any)
static int air_count(unsigned n0, uint8_t kind, uint8_t opcode)
{
	int n = 0;
	for (unsigned i = n0; i < sim.air_count; i++) {
		if (sim.air[i].kind == kind && (opcode == 0xFF || (sim.air[i].payload[0] & 0x3F) == opcode)) {
			n++;
		}
	}
	return n;
}

// The side keys (ui_svc_storage.c over core/input/keyassign.c): the codeplug's slots give the key map; a change in
// Settings takes effect at once and goes to the codeplug through a storage job; a CPS write is taken
// The speaker volume of a radio without a knob (radio-features.md 4.2): the stored step reaches hal_radio_t.volume() as a
// percent at start-up and at each change; a knob radio, and a radio without the hook, are left alone
static int vol_knob = -1;
static int vol_calls, vol_last = -1;
static void r_volume(uint8_t percent) { vol_calls++; vol_last = percent; }
static int8_t p_volume(void) { return (int8_t)vol_knob; }

static void test_soft_volume(void)
{
	settings_t st;
	fake_radio.volume = NULL;
	// A power HAL that doesn't report volume at all: not soft, nothing sent
	start();
	run(100);
	CHECK(!radio_volume_soft() && vol_calls == 0 && !(ui_svc_feat_radio() & UI_FEAT_VOLUME));
	fake_radio.volume = r_volume;
	// No knob (-1): the default step goes out at start-up as 50 %
	fake_power.volume = p_volume;
	vol_knob = -1;
	start();
	run(100);
	CHECK(radio_volume_soft() && (ui_svc_feat_radio() & UI_FEAT_VOLUME));
	CHECK(vol_calls == 1 && vol_last == 50 && sm.view.volume == 50 && sm.view.volume_src == RVOL_SOFT);
	CHECK(ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_DEFAULT);
	// A change reaches the hook at once as round(step * 100 / RADIO_VOLUME_MAX) and is stored with bit 7 and step + 1
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 13));
	run(100);
	CHECK(vol_calls == 2 && vol_last == 81 && sm.view.volume == 81 && ui_svc_setting_get(UI_SET_VOLUME) == 13);
	settings_read(&st);
	CHECK(st.volume == 0x8E);
	// The ends: mute (0) is stored as 1, the maximum is 100 %; out-of-range values are clamped
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 0));
	run(100);
	CHECK(vol_last == 0 && ui_svc_setting_get(UI_SET_VOLUME) == 0);
	settings_read(&st);
	CHECK(st.volume == 0x81);
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 99));
	run(100);
	CHECK(vol_last == 100 && ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_MAX);
	// The same step again sends nothing
	int calls = vol_calls;
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, RADIO_VOLUME_MAX));
	run(100);
	CHECK(vol_calls == calls);
	// A boot with the step stored: it goes out at once, the settings' value rather than the default
	CHECK(ui_svc_setting_set(UI_SET_VOLUME, 4));
	run(100);
	vol_calls = 0;
	boot();
	run(100);
	CHECK(vol_calls == 1 && vol_last == 25);
	// A byte stored by the 0..10 version (1..11) is scaled to the new range: step s -> round(s * 16 / 10)
	settings_begin()->volume = 6;		// old step 5
	settings_end(now);
	CHECK(ui_svc_setting_get(UI_SET_VOLUME) == 8);
	CHECK(radio_volume_step(1) == 0 && radio_volume_step(2) == 2 && radio_volume_step(4) == 5 &&
	      radio_volume_step(11) == RADIO_VOLUME_MAX && radio_volume_step(0x80) == RADIO_VOLUME_MAX &&
	      radio_volume_step(radio_volume_stored(7)) == 7);
	// A knob that reads (0..100) wins: the stored step is not used
	vol_knob = 64;
	boot();
	run(100);
	CHECK(!radio_volume_soft() && vol_last == 64 && !(ui_svc_feat_radio() & UI_FEAT_VOLUME));
	CHECK(sm.view.volume_src == RVOL_KNOB && radio_volume_settable());
	CHECK(radio_volume_step(0) == RADIO_VOLUME_DEFAULT && radio_volume_step(200) == RADIO_VOLUME_MAX);
	// The debug link's override (RADIO op 30) takes the knob's place until it is given back; RAM only
	vol_calls = 0;
	CHECK(debuglink_is_feature_op(DL_OP_VOLUME));	// the dispatcher routes it here
	// VERSION's codec=: a codec stopped for a stub upload (present() false) still reports its profile from flash
	{
		const hal_vocoder_t pv = { v_present, v_reset, v_encode, v_decode, v_profile, NULL, NULL, NULL };
		bool was = codec_present;
		CHECK(strcmp(debuglink_codec_id(NULL), "none") == 0);
		codec_present = false;
		codec_profile = "db25d-2501";
		CHECK(strcmp(debuglink_codec_id(&pv), "db25d-2501") == 0);
		codec_profile = NULL;
		CHECK(strcmp(debuglink_codec_id(&pv), "absent") == 0);
		CHECK(strcmp(debuglink_codec_id(&fake_vocoder), "absent") == 0);
		codec_present = true;
		CHECK(strcmp(debuglink_codec_id(&fake_vocoder), "present") == 0);
		codec_present = was;
	}
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 4 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_calls == 1 && vol_last == 25 && sm.view.volume_src == RVOL_OVERRIDE && sm.view.volume_override == 4);
	vol_knob = 90;		// the knob moves: no effect
	run(100);
	CHECK(vol_last == 25 && vol_calls == 1);
	{
		static char buf[2048];
		dl_text_t t;
		dl_text_init(&t, buf, sizeof buf);
		debuglink_view_radio(&sm.view, &t);
		CHECK(strstr(buf, "volume.src=override") != NULL && strstr(buf, "volume.override=4") != NULL &&
		      strstr(buf, "volume.max=16") != NULL);
	}
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 17 }, 1) == DL_ST_BAD_ARG);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 90 && sm.view.volume_src == RVOL_KNOB && sm.view.volume_override == -1);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 0 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 0);
	debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1);
	run(100);
	// No knob: the op stores the step like a UI change; 0xFF gives the default back
	vol_knob = -1;
	boot();
	run(100);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 3 }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 19 && sm.view.volume_src == RVOL_SOFT && ui_svc_setting_get(UI_SET_VOLUME) == 3);
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ RADIO_VOLUME_KNOB }, 1) == DL_ST_OK);
	run(100);
	CHECK(vol_last == 50 && ui_svc_setting_get(UI_SET_VOLUME) == RADIO_VOLUME_DEFAULT);
	// No hook at all (the volume works in analog hardware): not supported
	fake_radio.volume = NULL;
	CHECK(debuglink_radio_feature_op(DL_OP_VOLUME, (const uint8_t[]){ 3 }, 1) == DL_ST_UNAVAILABLE);
	fake_radio.volume = r_volume;
	vol_knob = 64;
	fake_power.volume = NULL;
	fake_radio.volume = NULL;
}

static void test_side_keys(void)
{
	memset(cp_quick, 0, sizeof(cp_quick));
	keymap_sets = 0;
	ui_svc_keys_poll();
	CHECK(keymap_sets == 1 && got_keymap[1].raw_edges && got_keymap[1].short_action == ACT_MONITOR);
	CHECK(ui_svc_setting_get(UI_SET_KEY_SIDE1) == KEYFN_DEFAULT);
	CHECK(ui_svc_setting_set(UI_SET_KEY_SIDE1, KEYFN_POWER));
	CHECK(keymap_sets == 2 && got_keymap[1].short_action == ACT_POWER && !got_keymap[1].raw_edges);
	CHECK(ui_svc_setting_get(UI_SET_KEY_SIDE1) == KEYFN_POWER);
	CHECK(cp_quick[0] == 0);		// not yet: the job does it
	CHECK(!ui_svc_setting_set(UI_SET_KEY_TOP, KEYFN_SCAN));	// this radio has two side keys
	CHECK(!ui_svc_setting_set(UI_SET_KEY_SIDE2, KEYFN_COUNT));
	uint32_t ser = cp_serial;
	ui_svc_keys_poll();
	storage_job_poll();
	CHECK(cp_quick[0] == KEYASSIGN_MARKER && cp_quick[1] == KEYFN_POWER && cp_serial == ser + 1u);
	ui_svc_keys_poll();			// our own write read back: the same
	CHECK(ui_svc_setting_get(UI_SET_KEY_SIDE1) == KEYFN_POWER && got_keymap[1].short_action == ACT_POWER);
	// A CPS wrote the slots
	cp_quick[1] = KEYFN_SCAN;
	cp_quick[4] = KEYFN_ZONE_UP;
	cp_serial++;
	ui_svc_keys_poll();
	CHECK(ui_svc_setting_get(UI_SET_KEY_SIDE1) == KEYFN_SCAN && got_keymap[1].short_action == ACT_SCAN);
	CHECK(got_keymap[2].short_action == ACT_TALK_TARGET && got_keymap[2].long_action == ACT_ZONE_UP);
	// A write that fails is reported; the key works for now
	cp_quick_fail = true;
	n_ev = 0;
	CHECK(ui_svc_setting_set(UI_SET_KEY_SIDE2_HOLD, KEYFN_ZONE_DOWN));
	ui_svc_keys_poll();
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(last_ev.text, "save the keys") != NULL);
	CHECK(got_keymap[2].long_action == ACT_ZONE_DOWN && cp_quick[4] == KEYFN_ZONE_UP);
	cp_quick_fail = false;
	memset(cp_quick, 0, sizeof(cp_quick));	// leave the board's key map for what follows
	cp_serial++;
	ui_svc_keys_poll();
}

// Switching off with ARS on (gotcha 7.4): the radio core holds the power off until the deregistration is on air, as
// app.c's power_off() waits on radio_power_off_busy() before it drops the latch (the DM-1701 and MD-380 knob)
static void test_power_off_ars(void)
{
	dmrDataSettings_t ds;

	dmr_start(true);
	dmrDataServiceGetSettings(&ds);
	ds.arsServer = 9990100;
	dmr_data_set_settings(&ds);
	unsigned n0 = sim.air_count;
	run(20000);			// the power-on registration, 5-15 s on (no server here to acknowledge it)
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERING);

	// Power off: busy until the preambles, the header and the blocks have all gone out, and nothing after
	n0 = sim.air_count;
	req(RREQ_POWER_OFF, 0);
	run(50);
	CHECK(sm.power_off_req && sm.power_off_busy);
	unsigned ms = 50;
	while (sm.power_off_busy && ms < 3000) {
		run(50);
		ms += 50;
	}
	CHECK(!sm.power_off_busy && ms < 1000);
	unsigned aired = sim.air_count - n0;
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_PREAMBLE) >= 1 && air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);
	CHECK(air_count(n0, DMR_BURST_RATE_12_DATA, 0xFF) >= 1 && !sm.dmr_keyed);
	run(5000);
	CHECK(sim.air_count - n0 == aired);	// no registration while switching off

	// Still on after all (auto power off with the knob on): canceled, and registered again
	req(RREQ_POWER_OFF, 1);
	run(50);
	CHECK(!sm.power_off_req && !sm.power_off_busy);
	n0 = sim.air_count;
	run(2000);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) == 1);

	// ARS off for what follows: the data service keeps its state from test to test
	ds.arsServer = 0;
	dmr_data_set_settings(&ds);
	run(10000);
	CHECK(!dmrDataServiceIsBusy() && dmrDataServiceArsState() == DMR_ARS_STATE_OFF);
}

static void test_dmr_data_glue(void)
{
	uint8_t kind;
	uint32_t dst;

	// A call alert: listen before talk, keyed like PTT, one CSBK with the MCU's CRC; the ack ends it
	dmr_start(true);
	unsigned n0 = sim.air_count;
	int tx0 = hw.tx_calls;
	CHECK(dmr_signal_call_alert(3101234));
	// On air without PTT, the view says TX like a call (#148): the strip's pill, the red LED, Home's activity line
	bool shown_tx = false, red = false, unkeyed = true;
	for (int i = 0; i < 60; i++) {
		run(10);
		if (sm.view.state == RS_TX && sm.view.dmr.state == RDMR_TX_DATA) {
			shown_tx = true;
			red = red || hw.led[LED_RED];
#if CONFIG_APRS
			unkeyed = unkeyed && sm.tx_no_ptt && !sm.aprs_was_tx;	// not the user's PTT: no "beacon after PTT"
#endif
		}
	}
	CHECK(shown_tx && red && unkeyed);
	CHECK(!sm.tx_no_ptt);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) >= 1 && hw.tx_calls > tx0);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_SENDING && dst == 3101234);
	CHECK(sm.view.state != RS_TX && !hw.led[LED_RED]);	// back to receive once the list is out
	for (unsigned i = n0; i < sim.air_count; i++) {
		if (sim.air[i].kind == DMR_BURST_CSBK) {
			CHECK(sim.air[i].host_crc);
		}
	}
	csbk_rx(DMR_CSBKO_CALL_ALERT_ACK, 3101276, 3101234, 2);
	run(50);
	CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_IDLE);	// the ack heard: RX, though no call
	CHECK(!hw.led[LED_GREEN]);	// green is for a call being played
	run(150);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK);
	CHECK(!(sm.view.state == RS_TX));
	run(1000);
	CHECK(sm.view.state == RS_IDLE);	// faded

	// No answer: the retries, then failed; the keying released each time
	n0 = sim.air_count;
	CHECK(dmr_signal_radio_check(3105555));
	run(30000);
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_FAILED);
	CHECK(air_count(n0, DMR_BURST_CSBK, 0xFF) >= 2 && !sm.dmr_keyed);
	CHECK(sm.view.state == RS_IDLE && !sm.tx_no_ptt && !hw.led[LED_RED]);	// a failed data send leaves no TX or deny behind

	// A call alert to us: alerted, and the ack goes out through the same glue
	n0 = sim.air_count;
	csbk_rx(DMR_CSBKO_CALL_ALERT, 3101276, 3109876, 2);
	run(1000);
	CHECK(air_count(n0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT_ACK) == 1);
	// ... not to us: nothing
	n0 = sim.air_count;
	csbk_rx(DMR_CSBKO_CALL_ALERT, 3100001, 3109876, 2);
	run(50);
	CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_IDLE && !hw.led[LED_GREEN]);	// others' data: RX, no call
	run(1000);
	CHECK(sim.air_count == n0);
	CHECK(sm.view.state == RS_IDLE);	// faded
	// ... on another color code: not our channel, no RX
	csbk_rx(DMR_CSBKO_CALL_ALERT, 3100001, 3109876, 5);
	run(50);
	CHECK(sm.view.state == RS_IDLE);
	run(1000);

	// A group SMS: a whole packet on air (preamble, header, blocks), sent once heard clear
	n0 = sim.air_count;
	CHECK(sms_send(9, true, "hello group", false, DMR_SMS_FORMAT_ETSI_IP));
	shown_tx = red = false;
	for (int i = 0; i < 300; i++) {	// the packet shows as a transmission, red LED included, from start to end
		run(10);
		if (sm.view.state == RS_TX && sm.view.dmr.state == RDMR_TX_DATA) {
			shown_tx = true;
			red = red || hw.led[LED_RED];
		}
	}
	CHECK(shown_tx && red);
	CHECK(sms_send_state() == DMR_REQ_OK);
	CHECK(air_count(n0, DMR_BURST_DATA_HEADER, 0xFF) >= 1 && sim.air_count - n0 >= 3);
	CHECK(sm.view.state != RS_TX && !sm.tx_no_ptt);

	// While the user transmits voice, data waits: nothing goes out in the middle of the call
	req(RREQ_PTT, 1);
	run(300);
	CHECK((sm.view.state == RS_TX));
	CHECK(!sm.tx_no_ptt);	// the user's own voice transmission counts for the after-PTT beacon
	unsigned voice0 = sim.air_count;
	CHECK(dmr_signal_call_alert(3101234));
	run(600);
	CHECK(air_count(voice0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) == 0);
	req(RREQ_PTT, 0);
	run(3000);
	CHECK(air_count(voice0, DMR_BURST_CSBK, DMR_CSBKO_CALL_ALERT) >= 1);
	run(30000);

	// An RX only DMR channel and an FM channel: refused, the request fails instead of keying
	chans[4].rx_only = true;
	reload_channel();
	tx0 = hw.tx_calls;
	CHECK(!dmr_signal_call_alert(3101234) || (run(3000), dmr_signal_state(&kind, &dst) == DMR_REQ_FAILED));
	CHECK(hw.tx_calls == tx0);
	chans[4].rx_only = false;
	reload_channel();
	req(RREQ_ZONE_POS, 0);		// channel 1, FM
	run(50);
	tx0 = hw.tx_calls;
	CHECK(!sms_send(3101234, false, "x", true, DMR_SMS_FORMAT_ETSI) || (run(5000), sms_send_state() == DMR_REQ_FAILED));
	CHECK(hw.tx_calls == tx0);
}

// ---- Error paths: the radio refuses to key, a channel without a TX frequency, retuning or scanning while
// transmitting, the stack's own refusals, targets that end with their channel, stored emergency settings ----
static void test_error_paths(void)
{
	// The squelch threshold's ends
	CHECK(radio_sm_squelch_open_below(0) == 255);
	CHECK(radio_sm_squelch_open_below(200) == radio_sm_squelch_open_below(RADIO_SQUELCH_MAX));

	// FM: no TX frequency, and a transmitter that won't key
	setup_codeplug();
	chans[1].tx_hz = 0;
	start();
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_NO_FREQ && hw.tx_calls == 0);
	req(RREQ_PTT, 0);
	CHECK(sm.view.state == RS_IDLE);
	chans[1].tx_hz = 146520000;
	reload_channel();
	req(RREQ_ZONE_POS, 0);
	run(20);
	tx_fail = true;
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && !hw.led[LED_RED]);
	req(RREQ_PTT, 0);
	tx_fail = false;

	// No retuning while transmitting: channel, zone, VFO and scan requests wait for the release
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX);
	uint16_t pos = sm.view.zone_pos;
	uint16_t zone = sm.view.zone_index;
	req(RREQ_CH_STEP, 1);
	req(RREQ_ZONE_POS, 3);
	req(RREQ_ZONE, 1);
	req(RREQ_VFO, 1);
	req(RREQ_SCAN, RSCAN_ZONE);
	run(50);
	CHECK(sm.view.state == RS_TX && sm.view.zone_pos == pos && sm.view.zone_index == zone && !sm.view.vfo);
	CHECK(!(sm.view.scan != 0));
	CHECK(hw.tx_calls == 1 && hw.tx_hz == 146520000);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	run(20);
	CHECK(sm.view.zone_pos != pos);
	// PTT while scanning: the scan stops where it is and the radio transmits there
	req(RREQ_ZONE_POS, 0);
	run(20);
	req(RREQ_SCAN, RSCAN_ZONE);
	run(400);
	CHECK(sm.view.scan != 0);
	req(RREQ_PTT, 1);
	run(20);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == sm.view.ch.tx_hz);
	req(RREQ_PTT, 0);
	run(20);
	req(RREQ_SCAN, RSCAN_STOP);
	run(20);
	CHECK(sm.view.scan == 0);

	// DMR: the transmitter won't key; the stack reports a rejection
	dmr_start(true);
	tx_fail = true;
	req(RREQ_PTT, 1);
	run(100);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BAND && sim.tx_begins == 0);
	req(RREQ_PTT, 0);
	run(100);
	tx_fail = false;
	sim.reject_next = true;
	req(RREQ_PTT, 1);
	run(300);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_REJECTED);
	req(RREQ_PTT, 0);
	run(1000);
	sim.reject_next = false;
	CHECK(sm.view.state != RS_TX && !sm.dmr_keyed);

	// A talk target, the hear all latch and the channel's own target: chosen ones end with their channel
	req(RREQ_DMR_TARGET, 3101234 | (1 << 24));
	req(RREQ_DMR_HEAR_ALL, 1);
	run(20);
	CHECK(sm.view.dmr.target_manual && sm.view.dmr.hear_all_latch);
	req(RREQ_ZONE_POS, 0);		// FM
	run(50);
	req(RREQ_ZONE_POS, 3);		// back on the DMR channel
	run(50);
	CHECK(!sm.view.dmr.target_manual && !sm.view.dmr.hear_all_latch && sm.view.dmr.target_id == 201);
	// Targets out of range or off DMR: ignored
	req(RREQ_DMR_TARGET, 0);
	run(20);
	CHECK(!sm.view.dmr.target_manual);
	req(RREQ_ZONE_POS, 0);
	run(50);
	req(RREQ_DMR_TARGET, 9);
	run(20);
	CHECK(!sm.view.dmr.target_manual);

	// Stored emergency settings are what the emergency engine starts with
	settings_t *st = settings_begin();
	st->em_stored = 1;
	st->em_mode = DMR_EMERGENCY_MODE_ALARM_CALL;
	st->em_dst = 3109999;
	st->em_flags = SETTINGS_EM_DST_PRIVATE | SETTINGS_EM_ETSI_FID | SETTINGS_EM_NO_TONE;
	st->em_alarm_tries = 3;
	st->em_hot_mic_s = 15;
	settings_end(now);
	boot();				// (start() would load the defaults: no flash here)
	run(50);
	dmr_emergency_settings_t es;
	dmr_emergency_get_settings_now(&es);
	CHECK(es.dst == 3109999 && es.dst_private && es.etsi_fid && !es.rx_tone && es.alarm_tries == 3 &&
	      es.hot_mic_s == 15);
	st = settings_begin();
	st->em_stored = 0;
	settings_end(now);
}

// Factory reset on a flash: the defaults saved at once, the native area emptied when ours, a toast when either
// fails (the settings can't be written; the native area won't erase), never the open-firmware codeplug or calibration
static const hal_flash_map_t reset_map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x30000u,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
	.native_addr = 0x30000u, .native_size = 0x4000u,
};

static void test_factory_reset(void)
{
	settings_t sv;
	sim_flash_create(0x40000u, &reset_map);
	CHECK(storage_init());
	settings_init();
	dmr_start(true);
	memset(&sim_flash_data[0x20000u], 0x42, 64);		// an open-firmware codeplug byte or two
	memset(&sim_flash_data[0x10000u], 0x24, 64);		// the calibration copy
	static const uint8_t magic[4] = { 0x4F, 0x44, 0x52, 0x4E };
	CHECK(sim_flash.write(reset_map.native_addr, magic, 4));
	settings_begin()->squelch = 2;
	settings_end(now);
	CHECK(settings_flush());
	n_ev = 0;
	CHECK(ui_svc_factory_reset());
	storage_job_poll();
	settings_read(&sv);
	CHECK(n_ev == 0 && sv.squelch == SETTINGS_UNSET8 && sim_flash_data[reset_map.native_addr] == 0xFF);
	CHECK(sim_flash_data[0x20000u] == 0x42 && sim_flash_data[0x10000u] == 0x24);
	settings_init();				// what the next boot finds
	settings_read(&sv);
	CHECK(sv.squelch == SETTINGS_UNSET8);
	// The settings can't be written (a worn ring): the toast says so
	settings_begin()->squelch = 4;
	settings_end(now);
	CHECK(settings_flush());		// stored: the reset has something to write
	sim_flash_bad_range(reset_map.settings_addr, reset_map.settings_size);
	CHECK(ui_svc_factory_reset());
	storage_job_poll();
	CHECK(n_ev == 1 && last_ev.kind == UI_EV_SAVE_FAILED && strstr(last_ev.text, "settings") != NULL);
	sim_flash_bad_range(0, 0);
	// The native area won't erase (ours, then the flash cut out under it)
	CHECK(sim_flash.write(reset_map.native_addr, magic, 4));
	CHECK(sim_flash.write(reset_map.native_addr + 0x2000u, magic, 4));
	settings_begin()->squelch = 5;
	settings_end(now);
	CHECK(settings_flush());
	n_ev = 0;
	CHECK(ui_svc_factory_reset());
	sim_flash_cut_after(308);	// the settings records (page 1: 48 + 4 bytes, page 0: 252 + 4) go in, the native erase fails
	storage_job_poll();
	sim_flash_power_on();
	CHECK(n_ev == 1 && strstr(last_ev.text, "native") != NULL);
	sim_flash.map = NULL;
	storage_init();			// back to no flash for anything after
	settings_init();
	sim_flash_free();
}

// DMR is TDMA: the radio's reading alternates between the signal and the floor. The published level holds the peak
static void test_dmr_rssi_hold(void)
{
	dmr_start(false);
	const radio_dmr_view_t *d = &sm.view.dmr;
	burst_rssi = 0;				// a modem that reports no level per burst
	rssi_alt = true;
	int16_t lo = 0;
	int16_t hi = -200;
	uint8_t lc[9];
	mk_lc(lc, true, 201, 3101234);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 2, false);
		run(60);
	}
	for (int sf = 0; sf < 3; sf++) {
		for (int i = 0; i < 6; i++) {
			burst_voice((uint8_t)i, 2, i == 5 ? lc : NULL);
			for (int k = 0; k < 12; k++) {	// 60 ms in 5 ms steps, sampling the published value
				run(5);
				if (sf > 0) {
					lo = sm.view.rssi_dbm < lo ? sm.view.rssi_dbm : lo;
					hi = sm.view.rssi_dbm > hi ? sm.view.rssi_dbm : hi;
				}
			}
		}
	}
	CHECK(d->state == RDMR_RX);
	CHECK(lo >= -70 - RADIO_RSSI_DEADBAND_DB && hi <= -70);	// never the floor while the call runs
	CHECK(d->rssi_dbm >= -70 - RADIO_RSSI_DEADBAND_DB && d->rssi_dbm <= -70);	// the call's level is the held one
	burst_lc(lc, 2, true);
	run(60);
	CHECK(d->state == RDMR_IDLE && d->rssi_dbm <= -70 && d->rssi_dbm != 0);	// the last call keeps its level
	run(RADIO_RSSI_HOLD_MS + 2 * RADIO_POLL_MS);
	CHECK(sm.view.rssi_dbm == -70 || sm.view.rssi_dbm == -125);
	rssi_alt = false;
	hw.st.rssi_dbm = -125;			// the signal is gone: the floor shows within the window
	run(RADIO_RSSI_HOLD_MS + RADIO_POLL_MS);
	CHECK(sm.view.rssi_dbm == -125);
	burst_rssi = -85;

	// A modem that reports a level: its value wins for the call
	rssi_alt = true;
	hear_call(true, 201, 3101234, 2, 1, false);
	CHECK(d->state == RDMR_RX && d->rssi_dbm == -85);
	rssi_alt = false;
}

// FM keeps the raw reading: no hold
static void test_fm_rssi_raw(void)
{
	setup_codeplug();
	start();
	for (int i = 0; i < 20; i++) {
		hw.st.rssi_dbm = i % 2 ? -125 : -70;
		poll_for(RADIO_POLL_MS);
		CHECK(sm.view.rssi_dbm == hw.st.rssi_dbm);
	}
}

int main(void)
{
	test_startup();
	test_persist();
	test_channel_step();
	test_vfo_step_auto();
	test_squelch();
	test_tx();
	test_timeout();
	test_power();
	test_vfo();
	test_no_codeplug();
	test_codeplug_tones();
	test_tone_squelch();
	test_temp_bw_tone();
	test_tone_lists();
	test_dmr_rx();
	test_dmr_rssi_hold();
	test_fm_rssi_raw();
	test_dmr_tx();
	test_dmr_stalled();
	test_dmr_list_talk_target();
#if CONFIG_VFO_DMR
	test_vfo_dmr();
#else
	test_vfo_no_dmr();
#endif
	test_vfo_copy();
	test_vfo_copy_dmr();
	test_dmr_denied();
	test_dmr_voice_features();
	test_dmr_contact_added();
	test_ui_requests();
	test_ui_glue();
#if CONFIG_DUAL_WATCH
	test_ui_dual();
	test_dual_two_vfos();
#endif
	test_ui_settings();
	test_soft_volume();
	test_side_keys();
	test_error_paths();
	test_hotspot();
	test_hotspot_session();
	test_hotspot_cwid();
	test_dmr_emergency();
	test_power_off_ars();
	test_dmr_data_glue();	// (last: the data service's state carries over)
	test_factory_reset();
	printf("# radioCoreTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
