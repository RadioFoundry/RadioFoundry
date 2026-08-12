// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine (core/radio/radio_sm.c) against a fake board and a fake codeplug
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "settings/settings.h"

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
static void r_set_mode(radio_mode_t m) { (void)m; hw.set_mode_calls++; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	hw.rx_hz = hz;
	hw.rx_bw = bw;
	hw.rx_tone = *t;
	hw.rx_calls++;
	return true;
}
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
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
static const hal_radio_t fake_radio = { r_init, r_set_mode, r_rx, r_tx, r_tx_off, r_status, r_audio, NULL, NULL, NULL };

static void p_led(uint8_t which, bool on) { hw.led[which] = on; }

// ---- fake PCM audio and vocoder (core/dmr/ambe runs on them) ----
static struct {
	bool started;
	int starts, stops;
	bool pcm;
} aud;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)cb; (void)ctx; aud.started = true; aud.starts++; return true; }
static void a_stop(void) { aud.started = false; aud.stops++; }
static void a_pcm(bool on) { aud.pcm = on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };

static bool codec_present;
static bool v_present(void) { return codec_present; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode };
static const hal_power_t fake_power = { .led = p_led };

static const band_t bands[] = {
	{ 108000000, 135999999, false },	// receive only
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};
static const char *const power_names[] = { "L", "M", "H" };

const board_t board = {
	.name = "fake",
	.bands = bands,
	.band_count = 3,
	.power_count = 3,
	.power_names = power_names,
	.power_default = 1,
	.radio = &fake_radio,
	.power = &fake_power,
	.dmr = &sim_modem,
	.audio = &fake_audio,
	.vocoder = &fake_vocoder,
};

// ---- fake codeplug ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zones[3];
static uint16_t nzones;
static codeplug_source_t source = CODEPLUG_LEGACY;

static uint32_t cp_serial;
codeplug_source_t codeplug_source(void) { return source; }
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
bool codeplug_contact(uint16_t index, contact_t *out)
{
	if (index < 1 || index > sizeof(contacts) / sizeof(contacts[0])) {
		return false;
	}
	*out = contacts[index - 1];
	return true;
}
static uint32_t contact_finds, iddb_lookups;
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out)
{
	contact_finds++;
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
	if (index != 1) {
		return false;
	}
	memset(out, 0, sizeof(*out));
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
static uint32_t now;

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
	CHECK(sm.view.source == CODEPLUG_LEGACY);
	CHECK(sm.view.state == RS_IDLE);
	CHECK(!hw.audio && !hw.led[LED_GREEN] && !hw.led[LED_RED]);
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
	source = CODEPLUG_LEGACY;

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
	b.rssi_dbm = -85;
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
	// MONITOR held: hear all
	req(RREQ_MONITOR, 1);
	CHECK(d->hear_all && !hw.audio);
	hear_call(true, 3100, 3107777, 2, 1, true);
	CHECK(d->last.dst == 3100);
	req(RREQ_MONITOR, 0);
	CHECK(!d->hear_all);
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
	req(RREQ_MONITOR, 1);
	hear_call(true, 3100, 3107777, 2, 1, true);
	req(RREQ_MONITOR, 0);
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

int main(void)
{
	test_startup();
	test_persist();
	test_channel_step();
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
	test_dmr_tx();
	test_dmr_denied();
	test_dmr_voice_features();
	test_hotspot();
	printf("# radioCoreTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
