// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The receive-only bands in the radio core (docs/spec/airband-and-broadcast.md): air band AM (3, CONFIG_AM_RX) and
// broadcast FM (4, CONFIG_BCAST_FM). Which channels are received in AM or on the broadcast receiver, both are receive
// only (PTT, split TX, DMR and the hotspot refused there), the squelch on the signal strength with no sub-audio, the
// VFO steps and the board's RX-only bands; against a fake board with CAP_AM_RX and CAP_BCAST_FM whose receivers hear
// signals per frequency
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/radio_am.h"
#include "radio/radio_bcast.h"
#include "dmr/ambe/ambeVoice.h"
#include "sim/dmr_sim_modem.h"
#include "settings/settings.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t now;

static struct {
	uint32_t rx_hz;
	int rx_calls, am_calls, tx_calls;
	bool am;		// the receiver is in AM (rx_am since the last rx / set_mode)
	radio_bw_t am_bw;
	bool audio;
	uint32_t sig_hz;	// a signal on this frequency ...
	int16_t sig_rssi;	// ... at this strength
	bool am_refuse;		// rx_am returns false
} hw;

// The broadcast receiver: powered, tuned, its audio and volume, and a station at bc_sig_khz with bc_rssi
static struct {
	bool on, audio, dead;
	uint32_t khz, sig_khz;
	uint8_t rssi, volume;
	int power_calls, tune_calls;
} bc;

static bool b_power(bool on)
{
	bc.power_calls++;
	if (on && bc.dead) {
		return false;
	}
	bc.on = on;
	if (!on) {
		bc.audio = false;
	}
	return true;
}
static bool b_tune(uint32_t khz)
{
	bc.tune_calls++;
	bc.khz = khz;
	return bc.on;
}
static bool b_seek(bool up) { (void)up; return false; }
static void b_volume(uint8_t level) { bc.volume = level; }
static bool b_status(bcast_status_t *st)
{
	if (!bc.on) {
		return false;
	}
	memset(st, 0, sizeof(*st));
	st->khz = bc.khz;
	st->rssi = bc.sig_khz != 0 && bc.sig_khz == bc.khz ? bc.rssi : 5;
	return true;
}
static void b_audio(bool on) { bc.audio = on; }
static const hal_bcast_t fake_bcast = {
	.lo_khz = 65000, .hi_khz = 108000, .grid_khz = 50, .power = b_power, .tune = b_tune, .seek = b_seek,
	.volume = b_volume, .status = b_status, .audio = b_audio,
};

static void r_init(void) {}
static void r_set_mode(radio_mode_t m) { (void)m; hw.am = false; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	(void)bw;
	(void)t;
	hw.rx_hz = hz;
	hw.rx_calls++;
	hw.am = false;
	return true;
}
static bool r_rx_am(uint32_t hz, radio_bw_t bw)
{
	if (hw.am_refuse) {
		return false;
	}
	hw.rx_hz = hz;
	hw.am_calls++;
	hw.am = true;
	hw.am_bw = bw;
	return true;
}
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	(void)hz;
	(void)bw;
	(void)p;
	(void)t;
	hw.tx_calls++;
	return true;
}
static void r_tx_off(void) {}
static void r_status(radio_status_t *st)
{
	st->noise = 120;	// the FM squelch never opens here: AM must use the RSSI
	st->rssi_dbm = -130;
	st->carrier = false;
	st->tone_ok = false;	// a board in AM may report anything: the core ignores sub-audio there
	if (hw.sig_hz != 0 && hw.sig_hz == hw.rx_hz) {
		st->rssi_dbm = hw.sig_rssi;
		if (!hw.am) {
			st->noise = 20;	// an FM receiver hears it too
			st->tone_ok = true;
		}
	}
}
static void r_audio(bool on) { hw.audio = on; }
static const hal_radio_t fake_radio = {
	.init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off, .status = r_status,
	.audio = r_audio, .rx_am = r_rx_am,
};

static void p_led(uint8_t which, bool on) { (void)which; (void)on; }
static uint16_t p_battery_mv(void) { return 8000; }
static uint8_t p_battery_pct(void) { return 80; }
static const hal_power_t fake_power = { .led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct };

static bool a_start(audio_block_cb_t cb, void *ctx) { (void)cb; (void)ctx; return true; }
static void a_stop(void) {}
static void a_pcm(bool on) { (void)on; }
static const hal_audio_t fake_audio = { a_start, a_stop, a_pcm };
static bool v_present(void) { return true; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; memset(bits, 0, VOCODER_BITS); }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; memset(pcm, 0, VOCODER_FRAME_SAMPLES * 2); }
static const hal_vocoder_t fake_vocoder = { v_present, v_reset, v_encode, v_decode, NULL, NULL, NULL, NULL };

// A band table with the air band and the broadcast band: RX-only bands below the VHF transmit band
static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
	{ 108000000, 136000000, false },
	{ 65000000, 108000000, false },
};
static const char *const power_names[] = { "L", "M", "H" };

const board_t board = {
	.name = "fake-am",
	.caps = CAP_AM_RX | CAP_BCAST_FM | CAP_DMR,
	.bands = bands,
	.band_count = 4,
	.bcast = &fake_bcast,
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
static zone_t zone;

codeplug_source_t codeplug_source(void) { return CODEPLUG_OPENFW; }
uint32_t codeplug_serial(void) { return 1; }
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint16_t codeplug_channel_count(void) { return NCH; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n < 1 || n > NCH) {
		return false;
	}
	*out = chans[n];
	return true;
}
uint16_t codeplug_zone_count(void) { return 1; }
bool codeplug_zones_stored(void) { return true; }
bool codeplug_zone(uint16_t i, zone_t *out)
{
	if (i != 0) {
		return false;
	}
	*out = zone;
	return true;
}
uint32_t codeplug_dmr_id(void) { return 3101276; }
bool codeplug_contact(uint16_t index, contact_t *out) { (void)index; (void)out; return false; }
bool codeplug_listen_list(uint16_t index, listen_list_t *out) { (void)index; (void)out; return false; }
bool codeplug_contact_find(uint32_t id, bool group, contact_t *out) { (void)id; (void)group; (void)out; return false; }
bool codeplug_dmr_settings(codeplug_dmr_settings_t *out)
{
	*out = (codeplug_dmr_settings_t){ "N0CALL", 0, 0 };
	return true;
}
bool dmr_iddb_lookup(uint32_t id, char *out, size_t len) { (void)id; (void)out; (void)len; return false; }

static void add(uint16_t n, const char *name, uint32_t rx, uint32_t tx, uint8_t mode)
{
	memset(&chans[n], 0, sizeof(chans[n]));
	strcpy(chans[n].name, name);
	chans[n].rx_hz = rx;
	chans[n].tx_hz = tx;
	chans[n].mode = mode;
	chans[n].bw = RADIO_BW_25K;
	chans[n].power = CODEPLUG_POWER_DEFAULT;
	chans[n].color_code = 1;
	chans[n].timeslot = 1;
}

// Positions: 0 Tower 118.100 (an FM channel in the standard codeplug, TX allowed by the codeplug), 1 ATIS 127.850
// with an RX tone set, 2 Simplex 146.520, 3 AirDMR 120.000 stored as DMR, 4 Split RX 146.520 TX 136.500, 5 Edge
// 136.975, 6 WXYZ 101.100 (broadcast, with an RX tone), 7 BcDMR 99.900 stored as DMR
static void setup_codeplug(void)
{
	add(1, "Tower", 118100000, 118100000, RADIO_MODE_FM);
	add(2, "ATIS", 127850000, 127850000, RADIO_MODE_FM);
	chans[2].rx_tone = chans[2].tx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	add(3, "Simplex", 146520000, 146520000, RADIO_MODE_FM);
	add(4, "AirDMR", 120000000, 120000000, RADIO_MODE_DMR);
	add(5, "Split", 146520000, 136500000, RADIO_MODE_FM);
	add(6, "Edge", 136975000, 136975000, RADIO_MODE_FM);
	add(7, "WXYZ", 101100000, 101100000, RADIO_MODE_FM);	// a broadcast station, TX allowed by the codeplug
	chans[7].rx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	add(8, "BcDMR", 99900000, 99900000, RADIO_MODE_DMR);
	memset(&zone, 0, sizeof(zone));
	strcpy(zone.name, "Air");
	zone.count = NCH;
	for (int i = 0; i < NCH; i++) {
		zone.channels[i] = (uint16_t)(i + 1);
	}
}

static radio_sm_t sm;

static void req(uint8_t type, int32_t arg)
{
	radio_req_t r = { type, arg, 0 };
	radio_sm_request(&sm, &r, now);
}

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

static void start(void)
{
	ambeVoiceStop();
	sim_reset();
	memset(&hw, 0, sizeof(hw));
	memset(&bc, 0, sizeof(bc));
	setup_codeplug();
	now = 1000;
	settings_init();
	radio_sm_init(&sm, now);
	run(100);
}

static void ptt(bool down)
{
	req(RREQ_PTT, down);
	run(100);
}

// ---- the pure helpers ----

static void test_helpers(void)
{
	CHECK(radio_am_band(108000000) && radio_am_band(118100000) && radio_am_band(136990000));
	CHECK(!radio_am_band(107999999) && !radio_am_band(137000000) && !radio_am_band(146520000));
	CHECK(radio_am_capable());
	CHECK(radio_am_tx_refused(136500000) && !radio_am_tx_refused(146520000));
	channel_t ch = { .rx_hz = 118100000, .mode = RADIO_MODE_FM };
	CHECK(radio_am_channel(&ch));
	ch.mode = RADIO_MODE_DMR;
	CHECK(!radio_am_channel(&ch));
	// The squelch scale: -18 dBm and stronger is 0, every dB weaker adds one
	CHECK(radio_am_noise(-10) == 0 && radio_am_noise(-18) == 0 && radio_am_noise(-70) == 52);
	CHECK(radio_am_noise(-400) == 255);
	// Level 5 opens above -70 dBm: noise 52 < 52 is false, 51 (-69 dBm) opens
	CHECK(radio_am_noise(-69) < radio_sm_squelch_open_below(5) && !(radio_am_noise(-70) < radio_sm_squelch_open_below(5)));
	// 8.33 kHz: the exact 25/3 kHz grid, snapping first; other steps add
	CHECK(radio_am_step(118000000, 1, RADIO_AM_STEP_833) == 118008333);
	CHECK(radio_am_step(118008333, 1, RADIO_AM_STEP_833) == 118016667);
	CHECK(radio_am_step(118016667, 1, RADIO_AM_STEP_833) == 118025000);
	CHECK(radio_am_step(118025000, -1, RADIO_AM_STEP_833) == 118016667);
	CHECK(radio_am_step(118010000, 1, RADIO_AM_STEP_833) == 118016667);	// 118.010 snaps to 118.00833 first
	CHECK(radio_am_step(118100000, 1, RADIO_AM_STEP_25K) == 118125000);
	CHECK(radio_am_step(146520000, -1, 12500) == 146507500);
	// Many 8.33 steps don't drift: 30 up lands on 118.250 exactly
	uint32_t hz = 118000000;
	for (int i = 0; i < 30; i++) {
		hz = radio_am_step(hz, 1, RADIO_AM_STEP_833);
	}
	CHECK(hz == 118250000);

	// Broadcast: the receiver's range, FM channels only, TX refused there, its grid, the squelch scale and volume
	CHECK(radio_bcast_capable());
	CHECK(radio_bcast_band(65000000) && radio_bcast_band(101100000) && radio_bcast_band(108000000));
	CHECK(!radio_bcast_band(64990000) && !radio_bcast_band(108050000) && !radio_bcast_band(146520000));
	ch = (channel_t){ .rx_hz = 101100000, .mode = RADIO_MODE_FM };
	CHECK(radio_bcast_channel(&ch));
	ch.mode = RADIO_MODE_DMR;
	CHECK(!radio_bcast_channel(&ch));
	CHECK(radio_bcast_tx_refused(88500000) && !radio_bcast_tx_refused(146520000));
	CHECK(radio_bcast_khz(101100000) == 101100 && radio_bcast_khz(101124999) == 101100 &&
		radio_bcast_khz(101125000) == 101150);
	CHECK(radio_bcast_noise(97) == 0 && radio_bcast_noise(120) == 0 && radio_bcast_noise(45) == 52);
	// Level 5 opens above 45 (the receiver's units): 46 opens, 45 doesn't
	CHECK(radio_bcast_noise(46) < radio_sm_squelch_open_below(5) &&
		!(radio_bcast_noise(45) < radio_sm_squelch_open_below(5)));
	CHECK(radio_bcast_volume(-1) == RADIO_BCAST_VOLUME_FIXED);
	CHECK(radio_bcast_volume(0) == 0 && radio_bcast_volume(100) == BCAST_VOLUME_MAX &&
		radio_bcast_volume(50) == 8 && radio_bcast_volume(150) == BCAST_VOLUME_MAX);
}

// ---- through the radio core ----

static void test_am_channel(void)
{
	start();
	// Position 0: Tower 118.100, an FM channel in the codeplug, received in AM
	CHECK(sm.view.am && hw.am && hw.rx_hz == 118100000 && hw.am_bw == RADIO_BW_25K);
	CHECK(hw.am_calls >= 1);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM);	// the channel itself stays as stored

	// Squelch on the signal strength: noise stays 120 (closed for FM), a -50 dBm signal opens
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
	hw.sig_hz = 118100000;
	hw.sig_rssi = -50;
	run(100);
	CHECK(sm.view.state == RS_RX && hw.audio);
	CHECK(sm.view.noise == 32);	// the AM squelch value: -18 - (-50)
	hw.sig_rssi = -76;	// below level 5's -70 dBm (with its hysteresis)
	run(100);
	CHECK(sm.view.state == RS_IDLE && !hw.audio);
	hw.sig_rssi = -63;
	run(100);
	CHECK(sm.view.state == RS_RX);
	hw.sig_hz = 0;
	run(100);
	CHECK(sm.view.state == RS_IDLE);

	// Receive only: PTT refused with the AM reason, nothing keyed, even though the codeplug allows TX
	ptt(true);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_AM && hw.tx_calls == 0);
	ptt(false);
	CHECK(sm.view.state == RS_IDLE);
}

static void test_tone_ignored(void)
{
	start();
	req(RREQ_ZONE_POS, 1);	// ATIS 127.850 with a CTCSS RX tone in the codeplug
	run(50);
	CHECK(sm.view.am && sm.view.tone_ok);
	hw.sig_hz = 127850000;
	hw.sig_rssi = -48;
	run(150);
	CHECK(sm.view.state == RS_RX);	// no sub-audio in AM: the tone doesn't hold the squelch shut
}

static void test_fm_and_dmr(void)
{
	start();
	req(RREQ_ZONE_POS, 2);	// Simplex 146.520: FM as ever
	run(50);
	CHECK(!sm.view.am && !hw.am && hw.rx_hz == 146520000);
	ptt(true);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1);
	ptt(false);

	req(RREQ_ZONE_POS, 3);	// AirDMR 120.000: no DMR in the air band, AM whatever the channel says
	run(100);
	CHECK(sm.view.am && hw.am && sm.view.ch.mode == RADIO_MODE_FM && !sm.view.temporary);
	req(RREQ_MODE, RADIO_MODE_DMR);	// and no switching it to DMR there
	run(30);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.am);
	ptt(true);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_AM);
	ptt(false);

	req(RREQ_ZONE_POS, 4);	// Split: receive 146.520 in FM, transmit 136.500 refused
	run(50);
	CHECK(!sm.view.am);
	int tx = hw.tx_calls;
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_AM && hw.tx_calls == tx);
	ptt(false);

	req(RREQ_ZONE_POS, 5);	// Edge 136.975: still the air band (to 137 MHz exclusive)
	run(50);
	CHECK(sm.view.am);

	// Leaving AM: the next channel goes through rx() again
	int rx = hw.rx_calls;
	req(RREQ_ZONE_POS, 2);
	run(50);
	CHECK(!sm.view.am && !hw.am && hw.rx_calls == rx + 1);

	// The hotspot never transmits in the air band
	CHECK(!radio_sm_hotspot_freq_ok(118100000, 118100000));
	CHECK(!radio_sm_hotspot_freq_ok(438500000, 136500000));
	CHECK(radio_sm_hotspot_freq_ok(438500000, 438500000));
}

static void test_board_refuses(void)
{
	start();
	hw.am_refuse = true;
	req(RREQ_ZONE_POS, 1);	// ATIS: the board can't, so FM as before, and still no transmit
	run(50);
	CHECK(!sm.view.am && hw.rx_hz == 127850000);
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_AM && hw.tx_calls == 0);
	ptt(false);
}

static void test_vfo_steps(void)
{
	start();
	req(RREQ_VFO_SET, 118000000);	// in the RX-only air band of the board's table
	run(50);
	CHECK(sm.view.vfo && sm.view.am && hw.rx_hz == 118000000);
	req(RREQ_VFO_STEP_SIZE, RADIO_AM_STEP_833);
	CHECK(sm.view.vfo_step_hz == RADIO_AM_STEP_833);
	req(RREQ_VFO_STEP, 1);
	run(30);
	CHECK(hw.rx_hz == 118008333 && sm.view.am);
	req(RREQ_VFO_STEP, 2);
	run(30);
	CHECK(sm.view.ch.rx_hz == 118025000);
	req(RREQ_VFO_STEP_SIZE, RADIO_AM_STEP_25K);
	req(RREQ_VFO_STEP, -1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 118000000);
	// The step is kept in the settings
	settings_t st;
	settings_read(&st);
	CHECK(st.vfo_step_hz == RADIO_AM_STEP_25K);
	// Out of range steps are ignored
	req(RREQ_VFO_STEP_SIZE, 500);
	CHECK(sm.view.vfo_step_hz == RADIO_AM_STEP_25K);
	// PTT in the AM VFO: refused
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_AM && hw.tx_calls == 0);
	ptt(false);
	// Up to 2 m: FM, transmit allowed again
	req(RREQ_VFO_SET, 145500000);
	run(50);
	CHECK(!sm.view.am);
	ptt(true);
	CHECK(sm.view.state == RS_TX);
	ptt(false);
}

// ---- broadcast FM through the radio core ----

static void test_bcast_channel(void)
{
	start();
	CHECK(!bc.on && bc.power_calls == 0);	// not a broadcast channel: the receiver stays off
	int rx = hw.rx_calls;
	req(RREQ_ZONE_POS, 6);	// WXYZ 101.100: plays on the broadcast receiver
	run(50);
	CHECK(sm.view.bcast && !sm.view.bcast_fault && !sm.view.am);
	CHECK(bc.on && bc.khz == 101100 && hw.rx_calls == rx);	// the channel receiver isn't retuned there
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.tone_ok);	// the codeplug's RX tone is ignored
	CHECK(bc.volume == RADIO_BCAST_VOLUME_FIXED);	// the fake radio has no volume to follow

	// A station is listened to, not squelched: the state is RX and the audio plays whatever the signal, on the
	// broadcast receiver only
	CHECK(bc.audio && !hw.audio);
	bc.sig_khz = 101100;
	bc.rssi = 65;
	run(100);
	CHECK(sm.view.state == RS_RX && bc.audio && !hw.audio);
	CHECK(sm.view.noise == 32 && sm.view.rssi_dbm == 65 + RADIO_BCAST_DBUV_DBM);
	bc.rssi = 35;	// an empty frequency's level: still RX, still playing
	run(100);
	CHECK(sm.view.state == RS_RX && bc.audio && !hw.audio && sm.view.rssi_dbm == 35 + RADIO_BCAST_DBUV_DBM);

	// Receive only, whatever the codeplug says
	ptt(true);
	CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_BCAST && hw.tx_calls == 0);
	ptt(false);
	CHECK(!hw.audio);

	// A 2 m channel: the receiver is switched off and the channel receiver tuned again
	req(RREQ_ZONE_POS, 2);
	run(50);
	CHECK(!sm.view.bcast && !bc.on && !bc.audio && hw.rx_hz == 146520000 && hw.rx_calls == rx + 1);

	// A DMR channel in the broadcast band plays on the broadcast receiver too, and doesn't transmit there
	req(RREQ_ZONE_POS, 7);
	run(100);
	CHECK(sm.view.bcast && bc.on && bc.khz == 99900 && sm.view.ch.mode == RADIO_MODE_FM);
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_BCAST && hw.tx_calls == 0);
	ptt(false);

	// The hotspot never transmits in the broadcast band
	CHECK(!radio_sm_hotspot_freq_ok(438500000, 101100000));
}

static void test_bcast_dead(void)
{
	start();
	bc.dead = true;		// the receiver doesn't answer: a fault, quiet, still no transmit
	req(RREQ_ZONE_POS, 6);
	run(100);
	CHECK(sm.view.bcast && sm.view.bcast_fault && !bc.on && sm.view.state == RS_IDLE);
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_BCAST && hw.tx_calls == 0);
	ptt(false);
}

// The broadcast step: 200 kHz between odd tenths of a MHz, from on or off that grid, or 100 kHz
static void test_bcast_step(void)
{
	CHECK(radio_bcast_step_hz(RADIO_BCAST_STEP_200_ODD) == 200000 && radio_bcast_step_hz(RADIO_BCAST_STEP_100) == 100000);
	CHECK(radio_bcast_step(90100000, 1, RADIO_BCAST_STEP_200_ODD) == 90300000);
	CHECK(radio_bcast_step(90100000, -1, RADIO_BCAST_STEP_200_ODD) == 89900000);
	CHECK(radio_bcast_step(90200000, 1, RADIO_BCAST_STEP_200_ODD) == 90300000);
	CHECK(radio_bcast_step(90200000, -1, RADIO_BCAST_STEP_200_ODD) == 90100000);
	CHECK(radio_bcast_step(90150000, 1, RADIO_BCAST_STEP_200_ODD) == 90300000);
	CHECK(radio_bcast_step(90150000, -1, RADIO_BCAST_STEP_200_ODD) == 90100000);
	CHECK(radio_bcast_step(107975000, -1, RADIO_BCAST_STEP_200_ODD) == 107900000);
	CHECK(radio_bcast_step(90200000, 1, RADIO_BCAST_STEP_100) == 90300000);
	CHECK(radio_bcast_step(90200000, -1, RADIO_BCAST_STEP_100) == 90100000);
}

static void test_bcast_vfo(void)
{
	start();
	req(RREQ_VFO_SET, 99900000);
	run(50);
	CHECK(sm.view.vfo && sm.view.bcast && bc.khz == 99900);
	CHECK(sm.view.vfo_step_hz == 200000);	// auto: the default 200 kHz in the broadcast band
	req(RREQ_VFO_STEP, 1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 100100000 && bc.khz == 100100);
	// A frequency typed in between the odd tenths is heard there; the knob takes it back to the odd ones
	req(RREQ_VFO_SET, 90200000);
	run(30);
	CHECK(sm.view.bcast && bc.khz == 90200);
	req(RREQ_VFO_STEP, 1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 90300000);
	req(RREQ_VFO_SET, 90200000);
	req(RREQ_VFO_STEP, -1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 90100000);
	req(RREQ_VFO_STEP, -1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 89900000);
	// The 100 kHz setting, saved with the radio core's settings
	radio_settings_t s = sm.set;
	s.bcast_step = RADIO_BCAST_STEP_100;
	radio_sm_set_settings(&sm, &s, now);
	CHECK(sm.view.vfo_step_hz == RADIO_BCAST_STEP_HZ);
	req(RREQ_VFO_STEP, 1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 90000000 && bc.khz == 90000);
	settings_t st;
	settings_read(&st);
	CHECK(st.rs_bcast_step == RADIO_BCAST_STEP_100 + 1u);
	s.bcast_step = RADIO_BCAST_STEP_200_ODD;
	radio_sm_set_settings(&sm, &s, now);
	CHECK(sm.view.vfo_step_hz == 200000);
	// An explicit VFO step is used as set, here too
	req(RREQ_VFO_STEP_SIZE, RADIO_AM_STEP_25K);
	req(RREQ_VFO_STEP, 1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 90025000);
	req(RREQ_VFO_STEP_SIZE, 0);
	// Down out of the broadcast band into the air band: AM there, the broadcast receiver off
	req(RREQ_VFO_SET, 108000000);
	run(50);
	CHECK(sm.view.am && !sm.view.bcast && !bc.on);
	req(RREQ_VFO_STEP, -1);
	run(30);
	CHECK(sm.view.ch.rx_hz == 107975000 && sm.view.bcast && !sm.view.am && bc.khz == 108000);	// the 50 kHz grid, rounded
	ptt(true);
	CHECK(sm.view.tx_deny == TXDENY_BCAST && hw.tx_calls == 0);
	ptt(false);

	// The DMR VFO tuned to a station: FM on the broadcast receiver, and the VFO stays FM after it
	req(RREQ_VFO_SET, 146520000);
	run(30);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(30);
	CHECK(sm.view.ch.mode == RADIO_MODE_DMR && !sm.view.bcast);
	req(RREQ_VFO_SET, 99900000);
	run(50);
	CHECK(sm.view.bcast && bc.khz == 99900 && sm.view.ch.mode == RADIO_MODE_FM);
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(30);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.bcast);
	req(RREQ_VFO_SET, 146520000);
	run(30);
	CHECK(!sm.view.bcast && sm.view.ch.mode == RADIO_MODE_FM);
	// ... and into the air band: AM
	req(RREQ_MODE, RADIO_MODE_DMR);
	run(30);
	req(RREQ_VFO_SET, 121500000);
	run(50);
	CHECK(sm.view.am && sm.view.ch.mode == RADIO_MODE_FM);
}

int main(void)
{
	test_helpers();
	test_am_channel();
	test_tone_ignored();
	test_fm_and_dmr();
	test_board_refuses();
	test_vfo_steps();
	test_bcast_channel();
	test_bcast_dead();
	test_bcast_step();
	test_bcast_vfo();
	printf("# radioListenBandsTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
