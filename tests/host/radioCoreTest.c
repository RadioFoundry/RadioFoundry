// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine (core/radio/radio_sm.c) against a fake board and a fake codeplug
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"

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
static const hal_radio_t fake_radio = { r_init, r_set_mode, r_rx, r_tx, r_tx_off, r_status, r_audio, NULL };

static void p_led(uint8_t which, bool on) { hw.led[which] = on; }
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
};

// ---- fake codeplug ----
#define NCH 8
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zones[3];
static uint16_t nzones;
static codeplug_source_t source = CODEPLUG_LEGACY;

codeplug_source_t codeplug_source(void) { return source; }
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
	radio_req_t r = { type, arg };
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

static void start(void)
{
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

	// A DMR channel shows the carrier but keeps the FM speaker shut
	req(RREQ_ZONE_POS, 3);
	signal(10, true);
	poll_for(RADIO_POLL_MS);
	CHECK(sm.view.state == RS_RX && !hw.audio);
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

	// DMR channel: no FM transmit
	req(RREQ_CH_STEP, 1);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_MODE);
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

int main(void)
{
	test_startup();
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
	printf("# radioCoreTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
