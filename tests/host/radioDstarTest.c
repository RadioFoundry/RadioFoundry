// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for D-STAR in the radio core (core/radio/radio_dstar.c, docs/spec/dstar-modem.md 5) with the real engine,
 * framing, codeplug codec and software voice codec, on a fake board: its flat transmit path is an interrupt the test
 * calls every 32 ms for 256 samples, as the HR_C7000's TX RAM asks for halves, and what it collects is what the flat
 * receive path hands back. The audio stream is a 20 ms block callback the test calls, with a voiced test signal as
 * the microphone. So an over goes out the way the radio sends one and comes back the way it receives one:
 *
 *   - a whole over: header (MY, suffix, UR, RPT1, RPT2, flag), message, voice and end, decoded on the way back with the
 *     caller shown, the message shown and the voice played at a level near the microphone's
 *   - UR's choices: a reflector command for one over, the caller's callsign for now, BACK on Home's clear
 *   - no codec RAM from the vocoder: the over still goes out (the silence frame) and the header still comes back
 *   - the bench's settings over the board's calibration: the deviation scale and an inverted transmit path (the
 *     receiver finds the polarity), and PTT refused without a scale or a callsign
 *   - the bench's test signals: a tone at the 1 kHz deviation scale
 *   - a transmit interrupt that stops pulling: the over still ends after PTT release, within the drain bound
 *   - the debug link's reset of the counters, preempted by the audio task: the counters stay consistent
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "board.h"
#include "radio/radio.h"
#include "radio/radio_dstar.h"
#include "radio/beep.h"
#include "settings/settings.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "dstar/dstar_codeplug.h"
#include "dstar/dstar_codec.h"
#include "debuglink/dl_frame.h"
#include "storage/storage.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- the fake board ---

#define CAPTURE_MAX	(8000u * 12u)	// 12 s of flat transmit samples
static int16_t capture[CAPTURE_MAX];
static uint32_t captured, rx_pos, rx_avail;	// rx_avail: what the board's ring holds by now (8 samples a ms)
static audio_tap_pull_t tx_pull_fn;
static bool rx_flat_on, tx_flat;
static bool tx_stalled;		// the transmit interrupt stops asking for halves (test_stalled_route)
static audio_tap_flat_cal_t cal = { .tx_1khz = 9000, .tx_latency_ms = 64 };

static bool fm_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
	if (pull != NULL && mode != AUDIO_TAP_FLAT) {
		return false;
	}
	tx_pull_fn = pull;
	tx_flat = pull != NULL;
	return true;
}

static bool rx_start(void) { rx_flat_on = true; return true; }
static void rx_stop(void) { rx_flat_on = false; }

// What came since the last read: the board's ring hands over at most a poll's worth
static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	uint32_t end = rx_avail < captured ? rx_avail : captured;
	uint32_t n = end > rx_pos ? end - rx_pos : 0;
	if (n > max) {
		n = max;
	}
	if (!rx_flat_on) {
		return 0;
	}
	memcpy(buf, &capture[rx_pos], n * sizeof(int16_t));
	rx_pos += n;
	return (uint16_t)n;
}

static const audio_tap_flat_cal_t *flat_cal(void) { return &cal; }

static const hal_audio_tap_t tap = {
	.caps = AUDIO_TAP_FM_RX_RAW | AUDIO_TAP_FM_TX_FLAT, .fm_tx_inject = fm_tx_inject, .fm_rx_flat_rate_hz = 8000,
	.fm_rx_flat_start = rx_start, .fm_rx_flat_read = rx_read, .fm_rx_flat_stop = rx_stop, .flat_cal = flat_cal,
};

// The vocoder's state RAM, lent (or not), the size the HR_C7000's vocoder lends (HAL_VOCODER_LEND_BYTES): the codec's
// working memory fits it, its state doesn't. The lend has no give-back: the vocoder rebuilds its state at its next call
static uint64_t voc_ram[0x1C00 / 8];
static bool lend_ok = true;
static unsigned lends;
static void *lend(size_t *bytes)
{
	if (!lend_ok) {
		return NULL;
	}
	lends++;
	*bytes = sizeof voc_ram;
	return voc_ram;
}
static bool v_present(void) { return false; }
static const hal_vocoder_t vocoder = { .present = v_present, .lend = lend };

static audio_block_cb_t stream_cb;
static bool speaker_on;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; stream_cb = cb; return true; }
static void a_stop(void) { stream_cb = NULL; }
static void a_speaker(bool on) { speaker_on = on; }
static const hal_audio_t audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_speaker };

const board_t board = { .name = "test", .audio_tap = &tap, .vocoder = &vocoder, .audio = &audio };

// --- the rest of the radio, faked ---

static settings_t st;
settings_t *settings_begin(void) { return &st; }
void settings_end(uint32_t now_ms) { (void)now_ms; }
bool beep_mix(int16_t *spk) { (void)spk; return false; }
static char radio_name[CODEPLUG_NAME_LEN + 1] = "Q1RAD";
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1]) { strcpy(out, radio_name); }

// The channel's tag parameter block and the radio-wide record as the codeplug would hand them over
static uint8_t chan_block[DSTAR_CP_CHAN_LEN];
static int chan_len = -1;
static uint8_t station_rec[DSTAR_CP_STATION_LEN];
static int station_len = -1;
int codeplug_tag_params(uint16_t number, const channel_t *ch, uint8_t *buf, uint16_t cap)
{
	(void)number;
	if (chan_len < 0 || ch->kind != CODEPLUG_KIND_DSTAR) {
		return -1;
	}
	memcpy(buf, chan_block, (size_t)chan_len < cap ? (size_t)chan_len : cap);
	return chan_len;
}
int codeplug_mode_settings(uint8_t kind, uint8_t *buf, uint16_t cap)
{
	if (station_len < 0 || kind != CODEPLUG_KIND_DSTAR) {
		return -1;
	}
	memcpy(buf, station_rec, (size_t)station_len < cap ? (size_t)station_len : cap);
	return station_len;
}
bool codeplug_tag_write(uint16_t number, const char *name, const codeplug_tag_t *t, const uint8_t *params, uint16_t len)
{
	(void)number; (void)name; (void)t; (void)params; (void)len;
	return false;
}
bool codeplug_tag_read(uint16_t number, codeplug_tag_t *t) { (void)number; (void)t; return false; }
uint16_t codeplug_name_check(const char *name) { (void)name; return 0; }
bool codeplug_tag_live(const codeplug_tag_t *t, const char *name, uint8_t mode, bool rx_only)
{
	(void)t; (void)name; (void)mode; (void)rx_only;
	return false;
}

// --- the simulated radio: 1 ms steps, the audio block every 20, the transmit interrupt every 32, the poll every 25 ---

static uint32_t now_ms = 1000;
static double mic_phase;
static int16_t mic_amp = 6000;
static double spk_energy;
static uint32_t spk_blocks, spk_loud_blocks;

// A voiced test signal: 140 Hz harmonics shaped by two formants, as a vowel
static void mic_block(int16_t *mic)
{
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		double v = 0;
		for (int h = 1; h <= 25; h++) {
			double f = 140.0 * h;
			double g = 1.0 / (1.0 + pow((f - 700.0) / 150.0, 2)) + 0.5 / (1.0 + pow((f - 1200.0) / 200.0, 2));
			v += g * sin(h * mic_phase);
		}
		mic_phase += 2.0 * M_PI * 140.0 / 8000.0;
		mic[i] = (int16_t)(v * mic_amp / 2.0);
	}
}

static void step(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t++) {
		now_ms++;
		rx_avail += 8u;
		if (now_ms % 20u == 0 && stream_cb != NULL) {
			int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];
			mic_block(mic);
			stream_cb(mic, spk, NULL);
			double e = 0;
			for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				e += (double)spk[i] * spk[i];
			}
			spk_energy += e;
			spk_blocks++;
			if (sqrt(e / AUDIO_BLOCK_SAMPLES) > 200.0) {
				spk_loud_blocks++;
			}
		}
		if (now_ms % 32u == 0 && tx_flat && tx_pull_fn != NULL && !tx_stalled) {
			int16_t half[256];
			tx_pull_fn(half, 256);
			if (captured + 256u <= CAPTURE_MAX) {
				memcpy(&capture[captured], half, sizeof half);
				captured += 256u;
			}
		}
		if (now_ms % 25u == 0) {
			radio_dstar_poll(now_ms);
		}
	}
}

static channel_t dstar_channel(void)
{
	channel_t ch;
	memset(&ch, 0, sizeof ch);
	strcpy(ch.name, "DV simplex");
	ch.rx_hz = ch.tx_hz = 145375000u;
	ch.kind = CODEPLUG_KIND_DSTAR;
	ch.system = CODEPLUG_SYSTEM_NONE;
	return ch;
}

static void reset(void)
{
	memset(&st, 0, sizeof st);
	memset(capture, 0, sizeof capture);
	captured = rx_pos = 0;
	tx_flat = false;
	tx_stalled = false;
	tx_pull_fn = NULL;
	lend_ok = true;
	cal = (audio_tap_flat_cal_t){ .tx_1khz = 9000, .tx_latency_ms = 64 };
	chan_len = station_len = -1;
	strcpy(radio_name, "Q1RAD");
	spk_energy = 0;
	spk_blocks = spk_loud_blocks = 0;
	radio_dstar_test(RDSTAR_TEST_OFF, 0, 0);
	radio_dstar_reset_stats();
}

// One over: PTT for ms, then held until the end pattern is on air and the board's latency waited out
static void over(uint32_t ms)
{
	CHECK(radio_dstar_tx_check() == TXDENY_NONE);
	CHECK(radio_dstar_tx_start(now_ms));
	step(ms);
	radio_dstar_tx_release();
	uint32_t t = 0;
	while (!radio_dstar_tx_done(now_ms) && t < 3000u) {
		step(1);
		t++;
	}
	CHECK(radio_dstar_tx_done(now_ms));
	radio_dstar_rx_again(now_ms);
}

// The collected over back through the flat receive path, as fast as the ring would hand it over
static void hear(void)
{
	rx_pos = rx_avail = 0;
	spk_energy = 0;
	spk_blocks = spk_loud_blocks = 0;
	for (uint32_t t = 0; t < captured / 8u + 1500u; t++) {
		step(1);
	}
}

static void test_whole_over(void)
{
	reset();
	dstar_chan_t c;
	memset(&c, ' ', sizeof c);
	c.flags = DSTAR_CP_VIA_RPT;
	memcpy(c.ur, "CQCQCQ  ", 8);
	memcpy(c.rpt1, "Q1RPT  B", 8);
	memcpy(c.rpt2, "Q1RPT  G", 8);
	dstar_chan_encode(&c, chan_block);
	chan_len = DSTAR_CP_CHAN_LEN;
	dstar_station_t s;
	dstar_field_set(s.my, sizeof s.my, "Q1ABC");
	dstar_field_set(s.suffix, sizeof s.suffix, "ID51");
	dstar_msg_set(s.msg, "Hello from the bench");
	dstar_station_encode(&s, station_rec);
	station_len = DSTAR_CP_STATION_LEN;
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_kind(&ch));
	CHECK(radio_dstar_start(5, &ch, now_ms));
	CHECK(rx_flat_on);
	radio_dstar_view_t v;
	radio_dstar_view(&v);
	CHECK(v.on && v.fault == RDSTAR_OK && v.voice_off == RDSTAR_VOICE_OK);
	CHECK(strcmp(v.tx_my, "Q1ABC") == 0 && strcmp(v.tx_suffix, "ID51") == 0 && strcmp(v.tx_ur, "CQCQCQ") == 0);
	CHECK(strcmp(v.tx_rpt1, "Q1RPT  B") == 0 && v.tx_repeater);
	CHECK(lends == 1);
	over(3000);
	CHECK(!rx_flat_on || captured > 8000u * 3u);
	radio_dstar_stats_t sx;
	radio_dstar_stats(&sx);
	CHECK(sx.tx_overs == 1 && sx.tx_frames >= 140u && sx.tx_overflows == 0);
	// A few frames wanted while the encoder was still filling: the silence frame then, never more than the header's time
	CHECK(sx.tx_underruns < 10u);
	hear();
	radio_dstar_view(&v);
	radio_dstar_stats(&sx);
	CHECK(v.heard && !v.heard_slow);
	CHECK(strcmp(v.my, "Q1ABC") == 0 && strcmp(v.suffix, "ID51") == 0 && strcmp(v.ur, "CQCQCQ") == 0);
	CHECK(strcmp(v.rpt1, "Q1RPT  B") == 0 && strcmp(v.rpt2, "Q1RPT  G") == 0);
	CHECK(strcmp(v.msg, "Hello from the bench") == 0);
	CHECK(v.rx == RDSTAR_RX_IDLE);		// the end pattern was heard
	CHECK(sx.rx_overs == 1 && sx.rx_frames >= 140u);
	CHECK(sx.rx_bad == 0 && sx.rx_bit_errors == 0);	// a clean loop: no frame needed correcting
	CHECK(sx.rx_played >= 140u && sx.rx_overflows == 0);
	CHECK(sx.polarity == 1);
	// The voice played at the microphone's order of level: an MBE codec keeps the spectrum, not the waveform
	printf("# whole over: %u frames out, %u back, %u played, %u loud blocks, rms %.0f\n", (unsigned)sx.tx_frames,
	       (unsigned)sx.rx_frames, (unsigned)sx.rx_played, (unsigned)spk_loud_blocks,
	       sqrt(spk_energy / (spk_blocks * (double)AUDIO_BLOCK_SAMPLES)));
	CHECK(spk_loud_blocks >= 100u);
	CHECK(!speaker_on);			// the over played out: the route back to idle
	radio_dstar_stop();
	CHECK(lends == 1 && !rx_flat_on && stream_cb == NULL);
}

static void test_ur_choices(void)
{
	reset();
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));	// no tag block: direct, CQCQCQ, MY from the radio name
	radio_dstar_view_t v;
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_my, "Q1RAD") == 0 && strcmp(v.tx_ur, "CQCQCQ") == 0 && !v.tx_repeater);
	CHECK(strcmp(v.tx_rpt1, "DIRECT") == 0 && strcmp(v.tx_rpt2, "DIRECT") == 0);
	radio_dstar_ur(RDSTAR_UR_LINK, "REF030C");
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "REF030CL") == 0 && v.ur_mode == RDSTAR_UR_LINK);
	radio_dstar_ur(RDSTAR_UR_LINK, "XRF7A");
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "XRF7  AL") == 0);	// the name padded to six, the module seventh
	over(400);
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "CQCQCQ") == 0 && v.ur_mode == RDSTAR_UR_CHANNEL);	// one over only
	hear();
	radio_dstar_view(&v);
	CHECK(strcmp(v.ur, "XRF7  AL") == 0 && strcmp(v.my, "Q1RAD") == 0 && strcmp(v.rpt1, "DIRECT") == 0);
	radio_dstar_ur(RDSTAR_UR_ECHO, NULL);
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "E") == 0 || strcmp(v.tx_ur, "       E") == 0);
	radio_dstar_clear_temporary();
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "CQCQCQ") == 0);
	radio_dstar_ur(RDSTAR_UR_REPLY, NULL);	// the caller heard last (ourselves, looped back)
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "Q1RAD") == 0 && v.ur_mode == RDSTAR_UR_REPLY);
	over(400);
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "Q1RAD") == 0);	// for now: it stays until the channel changes
	radio_dstar_start(5, &ch, now_ms);	// a retune of the same channel keeps it too
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "Q1RAD") == 0);
	radio_dstar_start(6, &ch, now_ms);	// another channel doesn't
	radio_dstar_view(&v);
	CHECK(strcmp(v.tx_ur, "CQCQCQ") == 0);
	radio_dstar_stop();
}

static void test_no_codec_ram(void)
{
	reset();
	lend_ok = false;
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));
	radio_dstar_view_t v;
	radio_dstar_view(&v);
	CHECK(v.voice_off == RDSTAR_VOICE_NO_RAM && v.fault == RDSTAR_OK);
	over(1000);
	hear();
	radio_dstar_view(&v);
	radio_dstar_stats_t sx;
	radio_dstar_stats(&sx);
	CHECK(v.heard && strcmp(v.my, "Q1RAD") == 0);
	CHECK(sx.rx_frames >= 40u && sx.rx_played == 0);	// heard, nothing to decode with
	CHECK(spk_loud_blocks == 0);
	radio_dstar_stop();
}

static void test_calibration(void)
{
	reset();
	channel_t ch = dstar_channel();
	// No scale anywhere: receive runs, PTT is refused rather than sent at a guessed deviation
	cal.tx_1khz = 0;
	CHECK(radio_dstar_start(5, &ch, now_ms));
	radio_dstar_view_t v;
	radio_dstar_view(&v);
	CHECK(v.fault == RDSTAR_NO_CAL && rx_flat_on);
	CHECK(radio_dstar_tx_check() == TXDENY_REFUSED);
	// The bench's setting supplies it, and inverts the transmit path: the receiver finds the polarity itself
	st.flat_tx_1khz = 7000;
	st.flat_flags = SETTINGS_FLAT_TX_INV_SET | SETTINGS_FLAT_TX_INV;
	radio_dstar_cal_reload();
	radio_dstar_view(&v);
	CHECK(v.fault == RDSTAR_OK);
	over(800);
	int16_t peak = 0;
	for (uint32_t i = 0; i < captured; i++) {
		int16_t a = (int16_t)abs(capture[i]);
		peak = a > peak ? a : peak;
	}
	CHECK(peak >= 7000 * 6 / 5 * 9 / 10 && peak <= 7000 * 6 / 5 * 11 / 10);	// +-1200 Hz: 1.2 x the 1 kHz scale
	hear();
	radio_dstar_stats_t sx;
	radio_dstar_stats(&sx);
	radio_dstar_view(&v);
	CHECK(v.heard && sx.polarity == -1);
	// No callsign anywhere: PTT says so
	strcpy(radio_name, "");
	radio_dstar_start(5, &ch, now_ms);
	radio_dstar_view(&v);
	CHECK(v.no_call && radio_dstar_tx_check() == TXDENY_NO_CALL);
	// Set on the radio, MY wins over the codeplug and the radio name
	memcpy(st.dstar_my, "Q1SET   ", 8);
	radio_dstar_start(5, &ch, now_ms);
	radio_dstar_view(&v);
	CHECK(!v.no_call && strcmp(v.tx_my, "Q1SET") == 0);
	radio_dstar_stop();
}

static void test_tone(void)
{
	reset();
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));
	radio_dstar_test(RDSTAR_TEST_TONE, 1000, 50);
	CHECK(radio_dstar_tx_start(now_ms));
	step(500);
	radio_dstar_tx_release();
	uint32_t t = 0;
	while (!radio_dstar_tx_done(now_ms) && t < 500u) {
		step(1);
		t++;
	}
	CHECK(radio_dstar_tx_done(now_ms));
	radio_dstar_rx_again(now_ms);
	// Half the 1 kHz scale, a 1 kHz sine: count the zero crossings and the peak over the first 400 ms
	int16_t peak = 0;
	uint32_t zc = 0, n = captured < 3200u ? captured : 3200u;
	for (uint32_t i = 1; i < n; i++) {
		int16_t a = (int16_t)abs(capture[i]);
		peak = a > peak ? a : peak;
		if ((capture[i] >= 0) != (capture[i - 1] >= 0)) {
			zc++;
		}
	}
	CHECK(peak >= 4400 && peak <= 4600);
	CHECK(zc >= 790 && zc <= 810);	// 1 kHz: two crossings a millisecond
	radio_dstar_test(RDSTAR_TEST_OFF, 0, 0);
	radio_dstar_stop();
}

// The bench changes the deviation scale mid-over (the debug link's calibration command reloads it in RS_TX too): the
// tone on air keeps the level it started with. The interrupt read the live calibration, which the reload zeroes before
// refilling, so a sample in between divided by a zero rate (a fault in the radio's interrupt) and the rest of the over
// came out at the new level
static void test_tone_cal_mid_over(void)
{
	reset();
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));
	radio_dstar_test(RDSTAR_TEST_TONE, 1000, 50);
	CHECK(radio_dstar_tx_start(now_ms));
	step(300);
	uint32_t half = captured;
	cal.tx_1khz = 3000;
	radio_dstar_cal_reload();
	step(300);
	radio_dstar_tx_release();
	for (uint32_t t = 0; !radio_dstar_tx_done(now_ms) && t < 500u; t++) {
		step(1);
	}
	CHECK(radio_dstar_tx_done(now_ms));
	radio_dstar_rx_again(now_ms);
	int16_t before = 0, after = 0;
	for (uint32_t i = 0; i < captured; i++) {
		int16_t a = (int16_t)abs(capture[i]);
		if (i < half) {
			before = a > before ? a : before;
		} else {
			after = a > after ? a : after;
		}
	}
	CHECK(before >= 4400 && before <= 4600);
	CHECK(after >= 4400 && after <= 4600);	// still half the 9000 scale, not half the 3000 one
	radio_dstar_test(RDSTAR_TEST_OFF, 0, 0);
	radio_dstar_stop();
}

// The board's transmit interrupt stops asking for halves mid-over (nothing of the route is measured on a radio yet):
// after PTT release the over still ends, within the drain bound, and the pull is stopped, so the core unkeys rather
// than holding a dead carrier until the time-out
static void test_stalled_route(void)
{
	reset();
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));
	CHECK(radio_dstar_tx_start(now_ms));
	step(500);
	tx_stalled = true;
	step(200);
	radio_dstar_tx_release();
	uint32_t t = 0, bound = RADIO_DSTAR_DRAIN_MS + cal.tx_latency_ms;
	while (!radio_dstar_tx_done(now_ms) && t < bound + 2000u) {
		step(1);
		t++;
	}
	CHECK(radio_dstar_tx_done(now_ms));
	CHECK(t >= bound - 30u && t <= bound + 30u);	// not before the queue and the end could have gone, nor long after
	CHECK(!tx_flat && tx_pull_fn == NULL);		// the pull stopped
	radio_dstar_rx_again(now_ms);
	CHECK(rx_flat_on);
	// Held PTT is the core's to time out: no release, no end
	tx_stalled = false;
	CHECK(radio_dstar_tx_start(now_ms));
	tx_stalled = true;
	step(bound + 500u);
	CHECK(!radio_dstar_tx_done(now_ms) && tx_flat);
	radio_dstar_tx_abort();
	CHECK(radio_dstar_tx_done(now_ms) && !tx_flat);
	radio_dstar_rx_again(now_ms);
	radio_dstar_stop();
}

// --- the debug link's reset of the counters, preempted by the audio task ---

// Run by the module's next memset of a whole radio_dstar_stats_t (radioDstarSeam.c), halfway through, then cleared
static void (*mid_stats_write)(void);

void *dstar_seam_memset(void *dst, int c, size_t n)
{
	void (*f)(void) = mid_stats_write;
	if (f == NULL || n != sizeof(radio_dstar_stats_t)) {
		return memset(dst, c, n);
	}
	mid_stats_write = NULL;
	memset(dst, c, n / 2u);
	f();
	memset((uint8_t *)dst + n / 2u, c, n - n / 2u);
	return dst;
}

static void audio_block(void)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];
	mic_block(mic);
	stream_cb(mic, spk, NULL);
}

// The debug link resets the counters on the host task while an over plays; the audio task, above it, can run a block
// in the middle of the reset. Every block that plays or conceals a frame is a block the callback ran, so played and
// concealed never pass blocks. The reset zeroed the counters one after another with a memset: a block in between
// counted its played frame (zeroed already) and lost its block (zeroed after)
static void test_reset_mid_block(void)
{
	reset();
	channel_t ch = dstar_channel();
	CHECK(radio_dstar_start(5, &ch, now_ms));
	over(2000);
	rx_pos = rx_avail = 0;
	for (uint32_t t = 0; t < 1500u && !radio_dstar_playing(); t++) {
		step(1);
	}
	step(400);
	CHECK(radio_dstar_playing());
	mid_stats_write = audio_block;
	radio_dstar_reset_stats();
	if (mid_stats_write != NULL) {
		mid_stats_write = NULL;	// the reset wrote no counters: the block comes after it
		audio_block();
	}
	radio_dstar_stats_t sx;
	radio_dstar_stats(&sx);
	CHECK(sx.rx_played == 1 && sx.blocks == 1);
	CHECK(sx.rx_played + sx.rx_concealed <= sx.blocks);
	hear();
	radio_dstar_stats(&sx);
	CHECK(sx.rx_played > 20u && sx.rx_played + sx.rx_concealed <= sx.blocks);
	radio_dstar_reset_stats();
	radio_dstar_stats(&sx);
	CHECK(sx.rx_played == 0 && sx.blocks == 0 && sx.rx_frames == 0 && sx.tx_overs == 0 && sx.tx_peak == 0);
	over(500);
	radio_dstar_stats(&sx);
	CHECK(sx.tx_overs == 1 && sx.tx_peak > 1000u);	// the new over's microphone peak
	radio_dstar_stop();
}

int main(void)
{
	radio_dstar_init(&audio);
	test_whole_over();
	test_ur_choices();
	test_no_codec_ram();
	test_calibration();
	test_tone();
	test_tone_cal_mid_over();
	test_stalled_route();
	test_reset_mid_block();
	printf("# radioDstarTest: %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
