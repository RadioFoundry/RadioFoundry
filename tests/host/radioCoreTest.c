// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's state machine (core/radio/radio_sm.c) against a fake board and a fake codeplug
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- fake radio ----
static struct {
	int set_mode_calls;
	uint32_t rx_hz;
	int rx_calls;
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
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t) { (void)bw; (void)t; hw.rx_hz = hz; hw.rx_calls++; return true; }
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t) { (void)bw; (void)t; hw.tx_calls++; hw.tx_hz = hz; hw.tx_power = p; return true; }
static void r_tx_off(void) { hw.tx_off_calls++; }
static void r_status(radio_status_t *st) { *st = hw.st; }
static void r_audio(bool on) { hw.audio = on; }
static const hal_radio_t fake_radio = { r_init, r_set_mode, r_rx, r_tx, r_tx_off, r_status, r_audio };

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
#define NCH 6
static channel_t chans[NCH + 1];
static bool used[NCH + 1];
static zone_t zones[2];
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
	nzones = 2;
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

	signal(10, false);			// strong carrier, wrong tone
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
	printf("# radioCoreTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
