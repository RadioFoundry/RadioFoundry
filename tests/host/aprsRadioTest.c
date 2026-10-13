// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// APRS through the radio core (docs/spec/aprs.md 6, 7, 9): the engine (core/aprs/aprs.c) and radio_sm.c's APRS block
// in an FM-only build, against a fake board with an AFSK transmitter whose tones are decoded back to AX.25 frames.
// Defaults (nothing sent), the amateur and callsign rules, beacon triggers and the hard minimum, CSMA on a busy
// channel, PTT cutting a transmission short, return to the channel, KISS frames from the host, NMEA from the host,
// messages with retries and acks, the station list, the digipeater and the iGate rules.
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "settings/settings.h"
#include "aprs/aprs.h"
#include "aprs/aprs_host.h"
#include "aprs/aprs_beacon.h"
#include "aprs/ax25.h"
#include "aprs/hdlc.h"
#include "aprs/kiss.h"
#include "aprs/afsk.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define CH_HZ		146520000u
#define APRS_HZ		144390000u

// ---- fake board ----
static struct {
	uint32_t rx_hz;
	int tx_calls;
	uint32_t tx_hz;
	uint8_t tx_power;
	int tx_off_calls;
	bool keyed;
	uint8_t noise_on_aprs;	// noise reading on 144.390 (low = a carrier)
	// AFSK
	uint8_t tones[600];
	uint16_t nbits;
	int starts, stops;
	uint32_t afsk_end_ms;
	bool afsk_busy;
	uint8_t level;
	// Faults
	bool tx_fail;		// the transmitter won't key
	bool afsk_fail;		// the AFSK transmitter won't start
	bool afsk_stuck;	// ... or never finishes
} hw;

static uint32_t now;
static char frames[8][AX25_TNC2_MAX];	// what went out, decoded from the tones
static int nframes;

static void r_init(void) {}
static void r_set_mode(radio_mode_t m) { (void)m; }
static bool r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
	(void)bw;
	(void)t;
	hw.rx_hz = hz;
	return true;
}
static bool r_tx(uint32_t hz, radio_bw_t bw, uint8_t p, const subtone_t *t)
{
	(void)bw;
	(void)t;
	if (hw.tx_fail) {
		return false;
	}
	hw.tx_calls++;
	hw.tx_hz = hz;
	hw.tx_power = p;
	hw.keyed = true;
	return true;
}
static void r_tx_off(void)
{
	hw.tx_off_calls++;
	hw.keyed = false;
}
static void r_status(radio_status_t *st)
{
	st->noise = hw.rx_hz == APRS_HZ ? hw.noise_on_aprs : 120;
	st->rssi_dbm = -120;
	st->carrier = false;
	st->tone_ok = true;
}
static void r_audio(bool on) { (void)on; }
static const hal_radio_t fake_radio = {
	.init = r_init, .set_mode = r_set_mode, .rx = r_rx, .tx = r_tx, .tx_off = r_tx_off, .status = r_status,
	.audio = r_audio,
};

static void p_led(uint8_t which, bool on) { (void)which; (void)on; }
static uint16_t p_battery_mv(void) { return 7600; }
static uint8_t p_battery_pct(void) { return 80; }
static uint8_t p_mic(void) { return 0; }
static const hal_power_t fake_power = {
	.led = p_led, .battery_mv = p_battery_mv, .battery_pct = p_battery_pct, .mic_level = p_mic,
};

// The tones back to frames, as a receiver would
static void decode_tones(void)
{
	hdlc_dec_t d;
	hdlc_dec_init(&d);
	for (uint32_t i = 0; i < hw.nbits; i++) {
		size_t n = hdlc_dec_bit(&d, hdlc_tone_bit(hw.tones, i));
		ax25_frame_t f;
		if (n != 0 && nframes < 8 && ax25_decode(d.buf, n, &f)) {
			ax25_to_tnc2(&f, frames[nframes++], AX25_TNC2_MAX);
		}
	}
}

static bool a_tx_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	if (hw.afsk_fail || !hw.keyed || baud != 1200 || mark != 1200 || space != 2200 || (n + 7u) / 8u > sizeof(hw.tones)) {
		return false;
	}
	memcpy(hw.tones, t, (n + 7u) / 8u);
	hw.nbits = n;
	hw.level = level;
	hw.starts++;
	hw.afsk_busy = true;
	hw.afsk_end_ms = now + (uint32_t)n * 1000u / 1200u;
	decode_tones();
	return true;
}
static bool a_tx_busy(void)
{
	if (hw.afsk_busy && !hw.afsk_stuck && (int32_t)(now - hw.afsk_end_ms) >= 0) {
		hw.afsk_busy = false;
	}
	return hw.afsk_busy;
}
static void a_tx_stop(void)
{
	hw.stops++;
	hw.afsk_busy = false;
}
static const hal_afsk_t fake_afsk = { .tx_start = a_tx_start, .tx_busy = a_tx_busy, .tx_stop = a_tx_stop };

// The PCM stream (no receive path of the AFSK's own): FM receive audio on its input when the tap says so
static struct {
	audio_block_cb_t cb;
	void *ctx;
	int starts, stops;
} pcm;
static bool s_start(audio_block_cb_t cb, void *ctx)
{
	pcm.cb = cb;
	pcm.ctx = ctx;
	pcm.starts++;
	return true;
}
static void s_stop(void)
{
	pcm.cb = NULL;
	pcm.stops++;
}
static void s_speaker(bool on) { (void)on; }
static const hal_audio_t fake_audio = { .start = s_start, .stop = s_stop, .speaker_pcm = s_speaker };
static hal_audio_tap_t fake_tap;	// caps 0: the stream's input is the microphone

static const band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
static const char *const power_names[] = { "L", "H" };

const board_t board = {
	.name = "fake",
	.bands = bands,
	.band_count = 2,
	.power_count = 2,
	.power_names = power_names,
	.power_default = 0,
	.radio = &fake_radio,
	.power = &fake_power,
	.battery_low_mv = 6400,
	.battery_critical_mv = 6000,
	.afsk = &fake_afsk,
	.audio = &fake_audio,
	.audio_tap = &fake_tap,
};

// ---- fake codeplug: one channel ----
static channel_t chan;
static zone_t zone;
static char cp_name[CODEPLUG_NAME_LEN + 1];
codeplug_source_t codeplug_source(void) { return CODEPLUG_OPENFW; }
static uint32_t cp_serial = 1;
static char cp_path[2][10];		// the APRS record's path entries
uint32_t codeplug_serial(void) { return cp_serial; }
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint16_t codeplug_channel_count(void) { return 1; }
bool codeplug_channel(uint16_t n, channel_t *out)
{
	if (n != 1) {
		return false;
	}
	*out = chan;
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
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1]) { memcpy(out, cp_name, sizeof(cp_name)); }
bool codeplug_aprs(uint8_t n, codeplug_aprs_t *out)
{
	memset(out, 0, sizeof(*out));
	if (n != 1) {
		return false;
	}
	strcpy(out->comment, "ODR test");
	strcpy(out->path[0], cp_path[0]);
	strcpy(out->path[1], cp_path[1]);
	return true;
}
bool codeplug_set_aprs_path(uint8_t n, const char *path1, const char *path2)
{
	if (n != 1) {
		return false;
	}
	strcpy(cp_path[0], path1);
	strcpy(cp_path[1], path2);
	cp_serial++;
	return true;
}
void codeplug_boot_text(char line1[CODEPLUG_NAME_LEN + 1], char line2[CODEPLUG_NAME_LEN + 1])
{
	line1[0] = line2[0] = '\0';
}

// The host link isn't here: aprs_host.c registers with it
#include "hostlink/hostlink.h"
void hostlink_set_tnc(const hostlink_tnc_t *tnc) { (void)tnc; }

// ---- helpers ----
static radio_sm_t sm;

static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
		now += RADIO_POLL_MS;
		radio_sm_poll(&sm, now);
	}
}

// Run until the AFSK transmitter has started n times in all (CSMA waits a random number of slots)
static bool wait_start(int n, uint32_t max_ms)
{
	for (uint32_t t = 0; t < max_ms && hw.starts < n; t += RADIO_POLL_MS) {
		run(RADIO_POLL_MS);
	}
	return hw.starts >= n;
}

// ... and until it is back on the channel
static void wait_idle(void)
{
	for (int i = 0; i < 400 && (sm.aprs_phase != 0 || hw.keyed); i++) {
		run(RADIO_POLL_MS);
	}
}

static void req(uint8_t type, int32_t arg)
{
	radio_req_t r = { type, arg, 0 };
	radio_sm_request(&sm, &r, now);
}

static void reset(void)
{
	memset(&hw, 0, sizeof(hw));
	hw.noise_on_aprs = 120;
	nframes = 0;
	memset(frames, 0, sizeof(frames));
	now = 1000;
	memset(&chan, 0, sizeof(chan));
	strcpy(chan.name, "Home");
	chan.rx_hz = chan.tx_hz = CH_HZ;
	chan.mode = RADIO_MODE_FM;
	chan.bw = RADIO_BW_25K;
	chan.power = CODEPLUG_POWER_DEFAULT;
	memset(&zone, 0, sizeof(zone));
	strcpy(zone.name, "Z");
	zone.count = 1;
	zone.channels[0] = 1;
	strcpy(cp_name, "");
	strcpy(cp_path[0], "WIDE1-1");
	strcpy(cp_path[1], "WIDE2-1");
	settings_init();
	radio_sm_init(&sm, now);
	aprs_set_test_call("");
}

// Callsign, a fixed position, automatic beacons every interval_s
static void configure(uint16_t interval_s, bool smart)
{
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.tx_on = true;
	c.interval_s = interval_s;
	c.smart = smart;
	c.fix_lat = 490583333;		// 49 03.50 N
	c.fix_lon = -720291667;		// 072 01.75 W
	c.compressed = false;
	aprs_set_cfg(&c, now);
}

static bool host_out_has(const char *needle)
{
	static uint8_t buf[2048];
	size_t n = aprs_host_output(buf, sizeof(buf) - 1);
	buf[n] = 0;
	for (size_t i = 0; i + strlen(needle) <= n; i++) {
		if (memcmp(buf + i, needle, strlen(needle)) == 0) {
			return true;
		}
	}
	return false;
}

static void rx_line(const char *tnc2)
{
	ax25_frame_t f;
	uint8_t buf[AX25_FRAME_MAX];
	CHECK(ax25_from_tnc2(tnc2, strlen(tnc2), &f));
	f.dst.h = true;
	size_t n = ax25_encode(&f, buf, sizeof(buf));
	aprs_rx_frame(buf, n, now);
}

// ---- tests ----

static void test_defaults(void)
{
	reset();
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.block == APRS_NO_CALL);		// no callsign in the settings or the codeplug
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	CHECK(!c.tx_on && c.region == APRS_REGION_NA && aprs_cfg_freq(&c) == APRS_HZ && c.ssid == 7);
	// Callsign and a position, but transmit off: nothing in an hour
	strcpy(cp_name, "N0TST");
	c.fix_lat = 490583333;
	c.fix_lon = -720291667;
	aprs_set_cfg(&c, now);
	run(3600u * 1000u);
	CHECK(hw.tx_calls == 0 && hw.starts == 0);
	aprs_get_view(&v);
	CHECK(v.block == APRS_OK && strcmp(v.call, "N0TST-7") == 0 && v.pos_valid && v.pos_src == APRS_SRC_FIXED);
	CHECK(hw.rx_hz == CH_HZ);
	// N0CALL and a long name are no callsign
	CHECK(!aprs_callsign_ok("N0CALL") && !aprs_callsign_ok("SPENCER") && !aprs_callsign_ok("1ABC") &&
	      aprs_callsign_ok("K1ABC") && aprs_callsign_ok("2E0XYZ") && aprs_callsign_ok("VK2ABC"));
}

static void test_beacon_on_air(void)
{
	reset();
	aprs_set_test_call("n0tst");
	configure(60, false);
	run(59u * 1000u);
	CHECK(hw.starts == 0 && hw.rx_hz == CH_HZ);
	CHECK(wait_start(1, 5000));
	CHECK(hw.tx_calls == 1 && hw.tx_hz == APRS_HZ && hw.starts == 1);
	CHECK(sm.view.state == RS_TX);
	CHECK(nframes == 1);
	CHECK(strcmp(frames[0], "N0TST-7>APZODR,WIDE1-1,WIDE2-1:!4903.50N/07201.75W[ODR test") == 0);
	if (nframes > 0) {
		printf("# on air: %s\n", frames[0]);
	}
	// preamble 300 ms = 45 flags: the first frame bit starts after 360 symbols
	CHECK(hw.nbits > 45u * 8u && hw.level == APRS_LEVEL_DEFAULT);
	wait_idle();
	CHECK(hw.stops == 1 && hw.tx_off_calls == 1 && !hw.keyed && hw.rx_hz == CH_HZ && sm.view.state == RS_IDLE);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.rf_sent == 1 && v.last_why == APRS_WHY_INTERVAL);
	// the next one a minute after that one
	run(55u * 1000u);
	CHECK(hw.starts == 1);
	CHECK(wait_start(2, 8000));
}

static void test_min_interval(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(1, false);	// asks for every second: the hard minimum is 30 s
	run(10u * 60u * 1000u);
	CHECK(hw.starts >= 18 && hw.starts <= 21);
}

static void test_busy_channel(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(60, false);
	hw.noise_on_aprs = 30;	// a carrier on 144.390
	run(61u * 1000u);
	CHECK(hw.rx_hz == APRS_HZ && hw.tx_calls == 0);	// listening
	run(10u * 1000u);
	CHECK(hw.tx_calls == 0 && hw.rx_hz == CH_HZ);	// gave up, back on the channel
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.rf_busy == 1);
	hw.noise_on_aprs = 120;
	run(20u * 1000u);	// tried again after the retry wait
	CHECK(hw.starts == 1);
}

static void test_ptt_cuts(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(60, false);
	run(60u * 1000u + 100u);	// on the APRS frequency, listening or keyed
	CHECK(hw.rx_hz == APRS_HZ || hw.keyed);
	req(RREQ_PTT, 1);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == CH_HZ);	// talking on the channel
	run(3000);
	CHECK(hw.starts == 0 || hw.stops >= 1);
	req(RREQ_PTT, 0);
	run(100);
	CHECK(sm.view.state == RS_IDLE && hw.rx_hz == CH_HZ);
	run(20u * 1000u);	// the beacon goes again later
	CHECK(hw.starts >= 1);
}

static void test_not_amateur(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(60, false);
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.region = APRS_REGION_CUSTOM;
	c.freq_hz = 150000000u;		// in the radio's band, not amateur
	aprs_set_cfg(&c, now);
	run(10u * 60u * 1000u);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.block == APRS_NOT_AMATEUR && hw.tx_calls == 0);
	c.freq_hz = 144800000u;
	aprs_set_cfg(&c, now);
	run(30u * 1000u);
	CHECK(wait_start(1, 40000) && hw.tx_calls == 1 && hw.tx_hz == 144800000u);
}

static void test_manual_and_ptt_trigger(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);	// no interval
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.tx_on = false;
	aprs_set_cfg(&c, now);
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 5000));
	wait_idle();
	// after PTT, with automatic beacons on
	c.tx_on = true;
	c.after_ptt = true;
	aprs_set_cfg(&c, now);
	run(200u * 1000u);
	CHECK(hw.starts == 1);
	req(RREQ_PTT, 1);
	run(2000);
	req(RREQ_PTT, 0);
	CHECK(wait_start(2, 6000));
	wait_idle();
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.last_why == APRS_WHY_PTT);
	// again right away: the gap isn't reached
	req(RREQ_PTT, 1);
	run(2000);
	req(RREQ_PTT, 0);
	run(5000);
	CHECK(hw.starts == 2);
}

// APRS keys at the radio's current power level, a temporary change included (it used the settings' default, so a
// radio stepped down to its lowest level still sent APRS at the default: 1 W on the DM-1701)
static void test_tx_power(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	CHECK(sm.view.power == 0);
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 5000));
	CHECK(hw.tx_power == 0);
	wait_idle();
	req(RREQ_POWER, 1);		// temporary: H
	CHECK(sm.view.power == 1 && sm.view.temporary);
	run(6000);			// past the 5 s "send now" repeat guard
	CHECK(aprs_send_now());
	CHECK(wait_start(2, 5000));
	CHECK(hw.tx_power == 1);
	wait_idle();
	CHECK(sm.view.power == 1 && hw.rx_hz == CH_HZ);
}

// APRS's own power setting (aprs_power, 0 = the radio's level) overrides the radio's current level
static void test_tx_power_own(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.power = 2;			// level 2 of 2: H
	aprs_set_cfg(&c, now);
	CHECK(sm.view.power == 0);
	CHECK(aprs_send_now());
	CHECK(wait_start(1, 5000));
	CHECK(hw.tx_power == 1);
	wait_idle();
	CHECK(sm.view.power == 0);	// the radio's own level is untouched
	CHECK(aprs_tx_power(0) == 1);
	c.power = 1;
	aprs_set_cfg(&c, now);
	CHECK(aprs_tx_power(1) == 0);
	c.power = 0;
	aprs_set_cfg(&c, now);
	CHECK(aprs_tx_power(1) == 1);
}

static void test_host_kiss_nmea(void)
{
	reset();
	aprs_set_test_call("N0TST");
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.kiss = true;
	c.to_is = true;
	c.tx_on = true;
	c.interval_s = 600;
	c.smart = true;
	c.compressed = true;
	aprs_set_cfg(&c, now);
	CHECK(aprs_host_claims(0xC0) && aprs_host_claims('$') && !aprs_host_claims('R'));
	// NMEA from the host: the position, moving at 22.4 kn
	const char *rmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
	aprs_host_feed((const uint8_t *)rmc, strlen(rmc), now);
	run(100);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.pos_valid && v.pos_src == APRS_SRC_HOST && v.lat == 481173000 && v.speed_kmh == 41 && v.course == 84);
	// a KISS data frame from the host, from our call (another SSID)
	ax25_frame_t f;
	uint8_t frame[AX25_FRAME_MAX], kiss[2 * AX25_FRAME_MAX];
	const char *line = "N0TST-9>APRS,WIDE2-1:>from the host";
	CHECK(ax25_from_tnc2(line, strlen(line), &f));
	f.dst.h = true;
	size_t n = ax25_encode(&f, frame, sizeof(frame));
	size_t k = kiss_encode(KISS_CMD_DATA, frame, n, kiss, sizeof(kiss));
	nframes = 0;
	aprs_host_feed(kiss, k, now);
	CHECK(wait_start(hw.starts + 1, 5000));
	wait_idle();
	bool seen = false;
	for (int i = 0; i < nframes; i++) {
		seen = seen || strcmp(frames[i], line) == 0;
	}
	CHECK(seen);
	run(3000);
	// someone else's call: refused
	line = "K1ABC>APRS:>not ours";
	CHECK(ax25_from_tnc2(line, strlen(line), &f));
	n = ax25_encode(&f, frame, sizeof(frame));
	CHECK(!aprs_host_frame(frame, n, now));
	// the rate limit: a burst of 3, then one per 10 s
	line = "N0TST>APRS:>burst";
	CHECK(ax25_from_tnc2(line, strlen(line), &f));
	n = ax25_encode(&f, frame, sizeof(frame));
	int ok = 0;
	for (int i = 0; i < 6; i++) {
		ok += aprs_host_frame(frame, n, now) ? 1 : 0;
		run(2000);
	}
	CHECK(ok >= 2 && ok <= 4);
	// our beacon also went to APRS-IS as a 'G' line
	host_out_has("");
	CHECK(aprs_send_now());
	run(1000);
	CHECK(host_out_has("GN0TST-7>APZODR,TCPIP*:!/"));	// compressed, no messaging (no receive path)
}

static void test_messages(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	CHECK(aprs_send_message("K1ABC-9", "Hello"));
	CHECK(!aprs_send_message("K1ABC-9", "Again"));	// one at a time
	CHECK(wait_start(1, 5000));
	CHECK(nframes == 1);
	char msgno[8] = "";
	const char *brace = nframes > 0 ? strchr(frames[0], '{') : NULL;
	CHECK(brace != NULL && strstr(frames[0], "::K1ABC-9  :Hello{") != NULL);
	if (brace != NULL) {
		strncpy(msgno, brace + 1, sizeof(msgno) - 1);
	}
	wait_idle();
	run(28u * 1000u);
	CHECK(wait_start(2, 6000));	// no ack: tried again at 30 s
	wait_idle();
#if CONFIG_APRS_RX
	// the ack comes: no third try
	char ack[64];
	snprintf(ack, sizeof(ack), "K1ABC-9>APRS::N0TST-7  :ack%s", msgno);
	rx_line(ack);
	run(200u * 1000u);
	CHECK(hw.starts == 2);
	aprs_view_t v;
	aprs_get_view(&v);
	CHECK(v.msg_acked || !v.msg_pending);
	// a message to us: kept and acked
	nframes = 0;
	rx_line("K1ABC-9>APRS,WIDE1-1::N0TST-7  :Hi there{42");
	CHECK(wait_start(3, 5000));
	wait_idle();
	CHECK(aprs_msg_count() == 1);
	aprs_msg_t m;
	CHECK(aprs_msg(0, &m) && strcmp(m.from, "K1ABC-9") == 0 && strcmp(m.text, "Hi there") == 0);
	CHECK(nframes == 1 && strstr(frames[0], "::K1ABC-9  :ack42") != NULL);
	// its retry: acked again, not kept twice
	rx_line("K1ABC-9>APRS,WIDE1-1::N0TST-7  :Hi there{42");
	CHECK(wait_start(4, 5000));
	wait_idle();
	CHECK(aprs_msg_count() == 1 && nframes == 2);
#else
	(void)rx_line;
#endif
}

#if CONFIG_APRS_RX
static void test_stations_digi_igate(void)
{
	reset();
	aprs_set_test_call("N0TST");
	configure(0, false);
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.digi = 1;		// fill-in
	c.kiss = true;
	c.igate = true;
	c.igate_tx = true;
	aprs_set_cfg(&c, now);
	host_out_has("");
	rx_line("K1ABC-9>APZODR,WIDE1-1,WIDE2-1:=4903.50N/07201.75W>Mobile");
	CHECK(aprs_station_count() == 1);
	aprs_station_t s;
	CHECK(aprs_station(0, &s) && strcmp(s.call, "K1ABC-9") == 0 && s.has_pos && s.direct && s.sym_code == '>');
	// gated to APRS-IS with our q construct; the raw frame to the KISS host too
	CHECK(host_out_has("GK1ABC-9>APZODR,WIDE1-1,WIDE2-1,qAR,N0TST-7:=4903.50N/07201.75W>Mobile"));
	// digipeated (fill-in): our call in the path, used (TNC2 stars the last used entry)
	CHECK(wait_start(1, 5000));
	wait_idle();
	CHECK(nframes == 1 && strcmp(frames[0], "K1ABC-9>APZODR,N0TST-7,WIDE1*,WIDE2-1:=4903.50N/07201.75W>Mobile") == 0);
	// a message from APRS-IS for the station we just heard: to RF as a third-party packet
	nframes = 0;
	const char *is = "W9XYZ>APRS,TCPIP*,qAC,T2TEST::K1ABC-9  :From the net{7";
	aprs_host_is_line(is, strlen(is), now);
	CHECK(wait_start(2, 5000));
	wait_idle();
	CHECK(nframes == 1 && strcmp(frames[0], "N0TST-7>APZODR:}W9XYZ>APRS,TCPIP,N0TST-7*::K1ABC-9  :From the net{7") == 0);
	// RFONLY stays local
	rx_line("K2DEF>APRS,RFONLY:>local only");
	CHECK(!host_out_has("K2DEF"));
}
#endif

// The position DMR GPS Info takes (aprs_position_now): the fixed position, the setting, ambiguity; and DMR callers'
// positions in the station list
static void test_dmr_position(void)
{
	aprs_cfg_t c;
	int32_t lat, lon;
	uint32_t err;
	bool send = true;

	reset();
	CHECK(!aprs_position_now(&lat, &lon, &err, &send) && !send);	// no position, setting off by default
	configure(0, false);
	CHECK(aprs_position_now(&lat, &lon, &err, &send) && lat == 490583333 && lon == -720291667 && err == 200 &&
	      !send);
	aprs_get_cfg(&c);
	c.dmr_gps = true;
	c.ambiguity = 2;	// whole minutes: the middle of the minute, about 1.9 km
	aprs_set_cfg(&c, now);
	CHECK(aprs_position_now(&lat, &lon, &err, &send) && send && err == 1852);
	CHECK(lat == 490583334 && lon == -720250000);	// 49 03.5 N (already mid-minute), 072 01.5 W
	aprs_get_cfg(&c);
	CHECK(c.dmr_gps);	// saved and read back
#if CONFIG_APRS_RX
	uint8_t n0 = aprs_station_count();
	aprs_dmr_heard(3112345, "kd9xyz Bob", 337490000, -843880000, now);
	aprs_dmr_heard(3112345, "kd9xyz Bob", 337490000, -843880000, now + 1000);	// the same: not counted again
	aprs_dmr_heard(3100001, "", 337000000, -843000000, now);
	aprs_dmr_heard(3100002, "Not a call", 337000000, -843000000, now);
	aprs_station_t st;
	CHECK(aprs_station_count() == n0 + 3);
	CHECK(aprs_station(0, &st) && strcmp(st.call, "3100002") == 0 && st.has_pos);
	CHECK(aprs_station(1, &st) && strcmp(st.call, "3100001") == 0);
	CHECK(aprs_station(2, &st) && strcmp(st.call, "KD9XYZ") == 0 && st.count == 1 && st.lat == 337490000);
#endif
}

// The configuration kept in the settings (settings_t aprs_*): back exactly after a reload, and a stored record with
// values out of range (another firmware's, a damaged one) loads field by field with the defaults for the bad ones
static void test_cfg_store(void)
{
	reset();
	aprs_cfg_t c, b;
	aprs_get_cfg(&c);
	c.tx_on = c.smart = c.compressed = c.after_ptt = c.boot = c.kiss = c.narrow = c.rx_on = true;
	c.igate = c.igate_tx = c.messaging = c.dmr_udt = c.to_is = c.dmr_gps = true;
	c.region = APRS_REGION_CUSTOM;
	c.freq_hz = 145825000;
	c.ssid = 9;
	c.sym_table = '\\';
	c.sym_code = 'k';
	c.path = APRS_PATH_WIDE2_2;
	c.ambiguity = 3;
	c.pos_src = APRS_POS_FIXED;
	c.fix_lat = -337000000;
	c.fix_lon = 1512000000;
	c.fix_alt_m = -12;
	c.interval_s = 900;
	c.distance_m = 2500;
	c.level = 63;
	c.txdelay = 45;
	c.via = APRS_VIA_BOTH;
	c.dmr_dst = 5057;
	c.digi = 2;
	aprs_set_cfg(&c, now);
	aprs_reload();
	run(200);			// (taken on the radio task's next tick)
	aprs_get_cfg(&b);
	CHECK(b.tx_on && b.smart && b.compressed && b.after_ptt && b.boot && b.kiss && b.narrow && b.rx_on);
	CHECK(b.igate && b.igate_tx && b.messaging && b.dmr_udt && b.to_is && b.dmr_gps);
	CHECK(b.region == APRS_REGION_CUSTOM && b.freq_hz == 145825000 && b.ssid == 9 && b.sym_table == '\\' &&
	      b.sym_code == 'k' && b.path == APRS_PATH_WIDE2_2 && b.ambiguity == 3 && b.pos_src == APRS_POS_FIXED);
	CHECK(b.fix_lat == -337000000 && b.fix_lon == 1512000000 && b.fix_alt_m == -12 && b.interval_s == 900 &&
	      b.distance_m == 2500 && b.level == 63 && b.txdelay == 45 && b.via == APRS_VIA_BOTH && b.dmr_dst == 5057 &&
	      b.digi == 2);
	// Out of range on the way in: kept in range
	c.ssid = 40;
	c.level = 200;
	c.ambiguity = 9;
	aprs_set_cfg(&c, now);
	aprs_get_cfg(&b);
	CHECK(b.ssid == 7 && b.level <= 100 && b.ambiguity == 4);
	// A damaged record
	settings_t *st = settings_begin();
	st->aprs_region = 99;
	st->aprs_ssid = 77;
	st->aprs_dmr_dst = 0x1000000u;
	st->aprs_symbol[0] = '/';
	st->aprs_symbol[1] = 0x7F;
	st->aprs_path = 50;
	st->aprs_pos_src = 50;
	st->aprs_level = 101;
	st->aprs_txdelay = 0;
	st->aprs_misc = 3u | (3u << 2) | (7u << 4);	// via, digipeater mode and ambiguity all out of range
	settings_end(now);
	aprs_reload();
	run(200);			// (taken on the radio task's next tick)
	aprs_get_cfg(&b);
	aprs_cfg_t d;
	aprs_cfg_default(&d);
	CHECK(b.region == APRS_REGION_NA && b.ssid == d.ssid && b.dmr_dst == d.dmr_dst && b.sym_code == d.sym_code &&
	      b.sym_table == d.sym_table);
	CHECK(b.path == APRS_PATH_WIDE1_WIDE2 && b.pos_src == APRS_POS_AUTO && b.level == d.level &&
	      b.txdelay == d.txdelay && b.via == APRS_VIA_RF && b.digi == 0 && b.ambiguity == 0);
	CHECK(b.tx_on && b.freq_hz == 145825000);	// the good fields stay
	// Not stored at all: the defaults
	st = settings_begin();
	st->aprs_stored = 0;
	settings_end(now);
	aprs_reload();
	run(200);			// (taken on the radio task's next tick)
	aprs_get_cfg(&b);
	CHECK(!b.tx_on && b.region == d.region && b.interval_s == d.interval_s);
}

// The path lives in the settings and in the codeplug's APRS record; the one changed last wins (aprs.md 11)
static void test_codeplug_path(void)
{
	aprs_cfg_t c;
	reset();
	strcpy(cp_path[0], "WIDE2-1");		// a CPS wrote WIDE2-1
	cp_path[1][0] = '\0';
	cp_serial++;
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.path == APRS_PATH_WIDE2);
	settings_t st;
	settings_read(&st);
	CHECK(st.aprs_stored == 1 && st.aprs_path == APRS_PATH_WIDE2 && st.aprs_cp_path == APRS_PATH_WIDE2 + 1u);
	// The menu changes it: the codeplug's unchanged path doesn't take it back
	c.path = APRS_PATH_WIDE2_2;
	aprs_set_cfg(&c, now);
	aprs_reload();
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.path == APRS_PATH_WIDE2_2);
	// ... and the storage job writes it into the codeplug
	CHECK(aprs_path_to_codeplug());
	CHECK(strcmp(cp_path[0], "WIDE2-2") == 0 && cp_path[1][0] == '\0');
	run(200);
	aprs_get_cfg(&c);
	settings_read(&st);
	CHECK(c.path == APRS_PATH_WIDE2_2 && st.aprs_cp_path == APRS_PATH_WIDE2_2 + 1u);
	// Two entries go into both slots
	c.path = APRS_PATH_WIDE1_WIDE2;
	aprs_set_cfg(&c, now);
	CHECK(aprs_path_to_codeplug() && strcmp(cp_path[0], "WIDE1-1") == 0 && strcmp(cp_path[1], "WIDE2-1") == 0);
	// None: both empty
	c.path = APRS_PATH_NONE;
	aprs_set_cfg(&c, now);
	CHECK(aprs_path_to_codeplug() && cp_path[0][0] == '\0' && cp_path[1][0] == '\0');
	// The CPS writes a new path: taken
	strcpy(cp_path[0], "WIDE1-1");
	cp_serial++;
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.path == APRS_PATH_WIDE1);
	// A path we don't offer is left alone
	strcpy(cp_path[0], "RELAY");
	strcpy(cp_path[1], "WIDE2-1");
	cp_serial++;
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.path == APRS_PATH_WIDE1);
	strcpy(cp_path[0], "");			// a second entry without a first
	cp_serial++;
	run(200);
	aprs_get_cfg(&c);
	CHECK(c.path == APRS_PATH_WIDE1);
}

// The transmission's failures: the transmitter won't key, the AFSK won't start, the AFSK never finishes. Each time
// the radio goes back to its channel unkeyed, and the frame is tried again later, then given up
static void test_rf_failures(void)
{
	aprs_view_t v;
	reset();
	strcpy(cp_name, "N0TST");
	configure(0, false);
	for (int fault = 0; fault < 3; fault++) {
		hw.tx_fail = fault == 0;
		hw.afsk_fail = fault == 1;
		hw.afsk_stuck = fault == 2;
		aprs_get_view(&v);
		uint32_t failed = v.rf_failed;
		CHECK(aprs_send_now());
		run(30000);
		aprs_get_view(&v);
		CHECK(v.rf_failed > failed && v.rf_sent == 0);
		CHECK(!hw.keyed && hw.rx_hz == CH_HZ && sm.view.state == RS_IDLE && sm.aprs_phase == 0);
		run(60000);		// the retries, then given up
		aprs_get_view(&v);
		CHECK(!aprs_rf_pending() && !v.rf_active && !hw.keyed && hw.rx_hz == CH_HZ);
	}
	hw.tx_fail = hw.afsk_fail = hw.afsk_stuck = false;
	// Working again: a status goes out; with the longest preamble a long one still fits (the preamble shortened)
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	c.txdelay = 255;
	aprs_set_cfg(&c, now);
	int starts = hw.starts;
	nframes = 0;
	CHECK(aprs_send_status("A status text long enough to make the frame and a 2.5 s preamble overflow the tones"));
	CHECK(wait_start(starts + 1, 20000));
	wait_idle();
	aprs_get_view(&v);
	CHECK(v.rf_sent >= 1 && nframes >= 1 && strstr(frames[0], ">A status text") != NULL);
	// No callsign: a status isn't sent
	strcpy(cp_name, "");
	aprs_set_cfg(&c, now);
	CHECK(!aprs_send_status("x"));
}

#if CONFIG_APRS_RX
// One 20 ms block of receive audio through the stream, then a radio poll
static void feed(const int16_t *s)
{
	int16_t spk[AUDIO_BLOCK_SAMPLES];
	if (pcm.cb != NULL) {
		pcm.cb(s, spk, pcm.ctx);
	}
	run(RADIO_POLL_MS);
}

// FM receive audio on the PCM stream (AUDIO_TAP_FM_RX) into the demodulator: only on the APRS frequency, battery saving
// held off while it listens (docs/spec/aprs.md 5.3)
static void test_rx_stream(void)
{
	fake_tap.caps = AUDIO_TAP_FM_RX;
	memset(&pcm, 0, sizeof(pcm));
	reset();
	aprs_cfg_t c;
	aprs_get_cfg(&c);
	CHECK(aprs_can_receive() && c.rx_on && c.messaging);	// a receive path: on by default
	// Not on the APRS frequency: the stream stays off
	run(500);
	CHECK(!aprs_rx_listening() && pcm.starts == 0);
	// A channel on 144.390
	chan.rx_hz = chan.tx_hz = APRS_HZ;
	settings_init();
	radio_sm_init(&sm, now);
	run(200);
	CHECK(aprs_rx_listening() && pcm.starts == 1 && pcm.cb != NULL);
	// A packet: silence, the tones (flags first, as a transmitter's TX delay), silence
	static uint8_t tones[256];
	static int16_t audio[24000];
	const char *line = "K1ABC-9>APRS,WIDE2-1:!4903.50N/07201.75W-Heard on air";
	ax25_frame_t f;
	uint8_t frame[AX25_FRAME_MAX];
	CHECK(ax25_from_tnc2(line, strlen(line), &f));
	f.dst.h = true;
	size_t n = ax25_encode(&f, frame, sizeof(frame));
	hdlc_enc_t e;
	hdlc_enc_init(&e, tones, sizeof(tones));
	CHECK(hdlc_enc_flags(&e, 30) && hdlc_enc_frame(&e, frame, n) && hdlc_enc_flags(&e, 3));
	afsk_mod_t m;
	afsk_mod_init(&m, 8000);
	memset(audio, 0, sizeof(audio));
	size_t k = afsk_mod(&m, tones, e.nbits, 8000, audio + 800, sizeof(audio) / sizeof(audio[0]) - 1600);
	CHECK(k > 0);
	CHECK(aprs_station_count() == 0);
	for (size_t i = 0; i + AUDIO_BLOCK_SAMPLES <= k + 1600; i += AUDIO_BLOCK_SAMPLES) {
		feed(audio + i);
	}
	run(100);
	aprs_station_t st;
	CHECK(aprs_station_count() == 1 && aprs_station(0, &st) && strcmp(st.call, "K1ABC-9") == 0);
	// Receive off: the stream stops
	aprs_get_cfg(&c);
	c.rx_on = false;
	aprs_set_cfg(&c, now);
	run(200);
	CHECK(!aprs_rx_listening() && pcm.stops == 1 && pcm.cb == NULL);
	fake_tap.caps = 0;
}
#endif

int main(void)
{
	test_defaults();
	test_beacon_on_air();
	test_min_interval();
	test_busy_channel();
	test_ptt_cuts();
	test_not_amateur();
	test_manual_and_ptt_trigger();
	test_tx_power();
	test_tx_power_own();
	test_host_kiss_nmea();
	test_messages();
	test_dmr_position();
	test_cfg_store();
	test_codeplug_path();
	test_rf_failures();
#if CONFIG_APRS_RX
	test_stations_digi_igate();
	test_rx_stream();
#endif
	printf("# aprsRadioTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
