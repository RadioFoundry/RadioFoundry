// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Fusion in the radio core (core/radio/radio_ysf.c; docs/spec/ysf-modem.md 5) against a fake board: flat FM paths
 * (hal_audio_tap.h) that a second engine feeds and reads, a fake vocoder whose frames are numbered, the real codeplug on
 * a simulated flash. Receive: a transmission from another station shows its callsigns and mode, plays its voice in
 * order and ends at its terminator; a DG-ID filter mutes another group's. Transmit: the over the module sends decodes
 * in an independent receiver with our callsigns, the encoded voice and the terminator, and tx_done follows it; the
 * first over's silence frame, written while the interrupt pulls, never goes out half written; the terminator goes out
 * whole when the board pulls in blocks longer than the audio's. The debug link's read
 * of the counters, preempted by a switch to D-STAR, still gives Fusion's. The
 * refusals: no codec, no callsign, no engine area. The ends: a transmit route that stops pulling still ends after PTT
 * release, and a stop in mid-transmission leaves the next start receiving.
 *
 *   make -C tests/host run-ysf
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "radio/radio.h"
#include "radio/radio_ysf.h"
#include "radio/radio_mode_ram.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "ysf/ysf_codeplug.h"
#include "ysf/ysf_engine.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "radio/mic_gain.h"
#include "audio/miclimit/miclimit.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
bool beep_mix(int16_t *spk) { (void)spk; return false; }

// --- the fake vocoder: frame n of an over is voice_pattern(n); the decoder records what it was given ---

static void voice_pattern(uint32_t n, uint8_t bits[YSF_VOICE_BITS])
{
	uint32_t x = n * 2654435761u + 777u;
	for (uint32_t i = 0; i < YSF_VOICE_BITS; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		bits[i] = (uint8_t)(x & 1u);
	}
}

static bool voc_present = true;
static uint32_t enc_n, dec_n, dec_match, dec_resets;
static uint8_t dec_want_base;	// the n the next decoded frame should be

// The state block a lending vocoder hands out (hal_vocoder_t.lend; HAL_VOCODER_LEND_BYTES builds). Every call of the
// fake writes all over it, as the real one rebuilds and then works in its state: a borrower still using the block
// after a vocoder call would read this pattern
static uint32_t voc_state[0x1C00 / 4];
static uint32_t voc_lends;
static void voc_scribble(void) { memset(voc_state, 0xA5, sizeof voc_state); }
static void *v_lend(size_t *len)
{
	voc_lends++;
	*len = sizeof voc_state;
	return voc_state;
}

static bool v_present(void) { return voc_present; }
static void v_reset(void) { dec_resets++; voc_scribble(); }
// Run once, halfway through writing the next silence frame: what preempts the audio task there on the radio (the
// board's transmit interrupt)
static void (*mid_silence)(void);
// The microphone block carries its frame number in its first sample; a silent block is the codec's silence
static int32_t enc_peak;	// the largest sample the encoder was given (the mic gain's output)
static void v_encode(const int16_t *pcm, uint8_t bits[49])
{
	voc_scribble();
	for (int i = 0; i < 160; i++) {
		enc_peak = abs(pcm[i]) > enc_peak ? abs(pcm[i]) : enc_peak;
	}
	if (pcm[0] == 0) {
		// this codec's own silence frame: not 49 zeros
		void (*f)(void) = mid_silence;
		mid_silence = NULL;
		memset(bits, 1, 25);
		if (f != NULL) {
			f();
		}
		memset(bits + 25, 1, 49 - 25);
		return;
	}
	voice_pattern((uint32_t)pcm[0], bits);
	enc_n++;
}
static void v_decode(const uint8_t bits[49], int16_t *pcm)
{
	voc_scribble();
	uint8_t want[49];
	voice_pattern(dec_want_base + dec_n, want);
	dec_match += memcmp(want, bits, 49) == 0;
	dec_n++;
	for (int i = 0; i < 160; i++) {
		pcm[i] = 1000;
	}
}

static const hal_vocoder_t vocoder = { .present = v_present, .reset = v_reset, .encode = v_encode, .decode = v_decode,
					.lend = v_lend };

// --- the fake audio stream: the test calls the block callback itself ---

static audio_block_cb_t stream_cb;
static bool speaker_on;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; stream_cb = cb; return true; }
static void a_stop(void) { stream_cb = NULL; }
static void a_speaker(bool on) { speaker_on = on; }
static const hal_audio_t audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_speaker };

// The microsecond clock (hal_time.h), which the module reads around each VW decode: it moves on by micros_step at
// every read, so a decode between two reads measures exactly that
static uint32_t micros_now, micros_step;
uint32_t hal_micros(void)
{
	micros_now += micros_step;
	return micros_now;
}

// --- the fake flat paths ---

#define RXBUF	(8000u * 8u)
static int16_t rxbuf[RXBUF];
static uint32_t rx_w, rx_r;
static bool flat_rx;
static audio_tap_pull_t tx_pull_fn;

static bool t_rx_start(void) { flat_rx = true; return true; }
static void t_rx_stop(void) { flat_rx = false; }
static uint16_t t_rx_read(int16_t *buf, uint16_t max)
{
	uint16_t n = 0;
	while (flat_rx && rx_r < rx_w && n < max) {
		buf[n++] = rxbuf[rx_r++ % RXBUF];
	}
	return n;
}
static bool t_inject(audio_tap_pull_t pull, audio_tap_mode_t mode) { (void)mode; tx_pull_fn = pull; return true; }
static const audio_tap_flat_cal_t cal = { .tx_1khz = 3000 };
static const audio_tap_flat_cal_t *t_cal(void) { return &cal; }
static const hal_audio_tap_t tap = {
	.caps = AUDIO_TAP_FM_RX_RAW | AUDIO_TAP_FM_TX_FLAT,
	.fm_tx_inject = t_inject,
	.fm_rx_flat_rate_hz = 8000,
	.fm_rx_flat_start = t_rx_start,
	.fm_rx_flat_read = t_rx_read,
	.fm_rx_flat_stop = t_rx_stop,
	.flat_cal = t_cal,
};

#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
// No hooks: only the mic PCM gain (hal_radio_t.mic_pcm_db) is read, by radio/mic_gain.c
static hal_radio_t radio;
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash,
		  .audio_tap = &tap, .vocoder = &vocoder, .radio = &radio };

// DMR's half of radio_set_mic_gain: no DMR voice here
void ambeVoiceSetMicGainQ8(uint16_t q8) { (void)q8; }

// The settings the bench's calibration lives in (radio_flat.c reads them over the board's)
static settings_t sets;
settings_t *settings_begin(void) { return &sets; }
void settings_end(uint32_t now_ms) { (void)now_ms; }

// --- set-up ---

static channel_t ch;
static const char *station_id = "E0123";	// the radio ID channel() stores with a callsign

static void channel(uint8_t tx_dgid, uint8_t rx_dgid, const char *call)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init(), "storage");
	codeplug_init();
	memset(&ch, 0, sizeof ch);
	strcpy(ch.name, "C4FM");
	ch.rx_hz = ch.tx_hz = 438625000;
	ch.mode = RADIO_MODE_DMR;
	ch.rx_only = true;
	ch.power = CODEPLUG_POWER_DEFAULT;
	ch.color_code = 1;
	ch.timeslot = 1;
	ch.system = CODEPLUG_SYSTEM_NONE;
	CHECK(codeplug_set_channel(5, &ch), "channel");
	ysf_chan_t yc;
	ysf_field_set(yc.dest, YSF_CALLSIGN_LEN, "ALL");
	yc.tx_dgid = tx_dgid;
	yc.rx_dgid = rx_dgid;
	yc.flags = 0;
	CHECK(ysf_chan_write(5, "C4FM", &yc, true), "tag");
	if (call != NULL) {
		ysf_station_t st;
		ysf_field_set(st.call, YSF_CALLSIGN_LEN, call);
		ysf_field_set(st.radio_id, YSF_RADIO_ID_LEN, station_id);
		uint8_t r[YSF_CP_STATION_LEN];
		ysf_station_encode(&st, r);
		CHECK(codeplug_kv_set((uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_YSF), r, sizeof r), "station");
	}
	CHECK(codeplug_channel(5, &ch) && ch.kind == CODEPLUG_KIND_YSF, "kind");
}

// --- another station's transmission into the flat receive path ---

typedef struct {
	uint32_t n, limit;
} src_t;

static bool other_pull(void *ctx, uint8_t bits[YSF_VOICE_BITS])
{
	src_t *s = ctx;
	if (s->n >= s->limit) {
		return false;
	}
	voice_pattern(s->n++, bits);
	return true;
}

static uint32_t now_ms;

// Air time in 20 ms steps: 160 samples into the path, the radio task's poll, one audio block
static void step(void)
{
	int16_t mic[160], spk[160];
	memset(mic, 0, sizeof mic);
	radio_ysf_poll(now_ms);
	if (stream_cb != NULL) {
		stream_cb(mic, spk, NULL);
	}
	now_ms += 20;
}

static void send_other(const char *src, uint8_t dgid, uint32_t frames)
{
	static ysf_engine_t e;
	src_t s = { 0, frames };
	ysf_tx_config_t c;
	memset(&c, 0, sizeof c);
	ysf_csd_init(&c.csd, src, "ALL");
	c.headers = 2;
	c.fich.sc = dgid;
	c.pull = other_pull;
	c.ctx = &s;
	ysf_path_t p = { .rate = 8000, .tx_amp = 2700 };
	CHECK(ysf_engine_tx_start(&e, &p, &c), "other tx");
	bool more = true;
	while (more) {
		if (s.n >= s.limit) {
			ysf_engine_tx_stop(&e);
		}
		int16_t pcm[160];
		more = ysf_engine_tx(&e, pcm, 160);
		for (int i = 0; i < 160; i++) {
			rxbuf[rx_w++ % RXBUF] = pcm[i];
		}
		step();
	}
}

static void quiet(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 20) {
		for (int i = 0; i < 160; i++) {
			rxbuf[rx_w++ % RXBUF] = 0;
		}
		step();
	}
}

static void test_receive(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_kind(&ch), "the kind is built");
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(v.on && v.fault == RYSF_OK && v.codec && strcmp(v.tx_src, "G4XYZ") == 0 && strcmp(v.tx_dest, "ALL") == 0,
	      "view: on %d fault %u src '%s' dest '%s'", v.on, v.fault, v.tx_src, v.tx_dest);
	CHECK(stream_cb != NULL && flat_rx, "the stream and flat receive run");

	dec_n = dec_match = 0;
	dec_want_base = 0;
	bool seen_rx = false, seen_play = false;
	quiet(200);
	CHECK(!radio_ysf_receiving(), "nothing heard yet");
	// 30 voice frames (6 V/D frames) sent as a Yaesu radio sends a call to everyone: its preamble, one header, radio
	// ID mode (the destination field "*****" and its radio ID)
	static ysf_engine_t e;
	src_t s = { 0, 30 };
	ysf_tx_config_t c;
	memset(&c, 0, sizeof c);
	ysf_csd_init(&c.csd, "M0ABC", NULL);
	memcpy(c.csd.rem[3], "E0456", 5);
	ysf_csd_radio_id(&c.csd, YSF_RADIO_ID_ALL);
	c.fich.cm = YSF_CM_RADIO_ID;
	c.preamble = 240;
	c.headers = 1;
	c.pull = other_pull;
	c.ctx = &s;
	ysf_path_t p = { .rate = 8000, .tx_amp = 2700 };
	ysf_engine_tx_start(&e, &p, &c);
	bool more = true;
	while (more) {
		if (s.n >= s.limit) {
			ysf_engine_tx_stop(&e);
		}
		int16_t pcm[160];
		more = ysf_engine_tx(&e, pcm, 160);
		for (int i = 0; i < 160; i++) {
			rxbuf[rx_w++ % RXBUF] = pcm[i];
		}
		step();
		seen_rx = seen_rx || radio_ysf_receiving();
		seen_play = seen_play || (radio_ysf_playing() && speaker_on);
	}
	quiet(600);
	radio_ysf_view(&v);
	CHECK(seen_rx && seen_play, "received %d, played %d", seen_rx, seen_play);
	CHECK(v.heard && v.mode == RYSF_MODE_DN && strcmp(v.src, "M0ABC") == 0 && strcmp(v.dest, "ALL") == 0,
	      "heard: mode %u src '%s' dest '%s' (radio ID mode's '*****' shown as ALL)", v.mode, v.src, v.dest);
	CHECK(!radio_ysf_receiving() && v.rx == RYSF_RX_IDLE, "ended at the terminator");
	// The transmitter's last frame is completed with frames of no voice (all zeros from this sender)
	CHECK(dec_match == 30 && dec_n >= 30 && dec_n <= 35, "played %u frames, %u in order", dec_n, dec_match);
	CHECK(!speaker_on, "the speaker off after the over");
	radio_ysf_stats_t st;
	radio_ysf_stats(&st);
	CHECK(st.rx_overs == 1 && st.rx_frames == dec_n && st.rx_played == dec_n, "stats overs %u frames %u played %u",
	      st.rx_overs, st.rx_frames, st.rx_played);
	radio_ysf_stop();
	CHECK(stream_cb == NULL && !flat_rx, "stopped");
}

static void test_dgid(void)
{
	channel(20, 20, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	dec_n = dec_match = 0;
	dec_want_base = 0;
	quiet(200);
	send_other("M0ABC", 10, 15);	// another group: shown, not played
	quiet(600);
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(v.heard && v.muted && v.dgid == 10 && strcmp(v.src, "M0ABC") == 0 && dec_n == 0,
	      "other DG-ID: muted %d dgid %u played %u", v.muted, v.dgid, dec_n);
	send_other("M0DEF", 20, 15);	// ours: played
	quiet(600);
	radio_ysf_view(&v);
	CHECK(!v.muted && v.dgid == 20 && strcmp(v.src, "M0DEF") == 0 && dec_n >= 15 && dec_n <= 20 && dec_match == 15,
	      "our DG-ID: muted %d played %u matched %u", v.muted, dec_n, dec_match);
	radio_ysf_stop();
}

// --- our transmission, decoded by an independent receiver ---

typedef struct {
	uint32_t voices, match, silence, zeros, other, ends, firsts;
	ysf_call_t call;
	uint8_t sc, cm;
} far_t;

static far_t far;
static uint32_t far_next;

static void f_frame(void *ctx, const ysf_call_t *c, bool first)
{
	(void)ctx;
	far.firsts += first;
	far.sc = c->fich.sc;
	far.cm = c->fich.cm;
}
static void f_call(void *ctx, const ysf_call_t *c) { (void)ctx; far.call = *c; }
static void f_voice(void *ctx, const uint8_t bits[49], uint8_t q)
{
	(void)ctx;
	(void)q;
	far.voices++;
	uint8_t ones = 0;
	for (int i = 0; i < 49; i++) {
		ones += bits[i];
	}
	if (ones == 49) {
		far.silence++;	// the fake codec's silence: a late encoder at the start, or the end of the last frame
		return;
	}
	if (ones == 0) {
		far.zeros++;	// the first over before the codec's silence frame exists
		return;
	}
	uint8_t want[49];
	voice_pattern(far_next++, want);
	bool same = memcmp(want, bits, 49) == 0;
	far.match += same;
	far.other += !same;
}
static void f_end(void *ctx, ysf_end_t why) { (void)ctx; far.ends += why == YSF_END_TERMINATOR; }

static ysf_engine_t far_rx;

static void far_start(void)
{
	memset(&far, 0, sizeof far);
	far_next = 1;
	static const ysf_rx_cb_t cb = { f_frame, f_call, f_voice, NULL, f_end, NULL };
	ysf_path_t p = { .rate = 8000 };
	ysf_engine_rx_start(&far_rx, &p, &cb);
}

// The board's interrupt: 800 ms of modulation, headers and then voice frames, none of them encoded yet
static void isr_burst(void)
{
	int16_t pcm[160];
	for (int i = 0; i < 40; i++) {
		tx_pull_fn(pcm, 160);
		ysf_engine_rx(&far_rx, pcm, 160);
	}
}

// The first over after start-up: the audio task encodes the codec's silence frame at its first block, with the pull
// already hooked, and the transmit interrupt can preempt it mid-frame. Every frame the interrupt sends meanwhile must
// be whole (zeros, the silence not being ready yet), never half the silence frame over the zeros it replaces. Runs
// before any other transmission: the silence frame is encoded once per boot
static void test_silence_first_over(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	far_start();
	mid_silence = isr_burst;
	int16_t mic[160], spk[160];
	memset(mic, 0, sizeof mic);
	stream_cb(mic, spk, NULL);
	CHECK(mid_silence == NULL, "the silence frame was encoded at the first block");
	CHECK(far.voices >= 5, "the interrupt sent %u voice frames while it was encoded", far.voices);
	CHECK(far.other == 0 && far.zeros + far.silence == far.voices, "%u torn frames of %u (%u zeros, %u silence)",
	      far.other, far.voices, far.zeros, far.silence);
	// From here on the frame is whole: the next frames the interrupt sends are the codec's silence
	uint32_t silent = far.silence;
	isr_burst();
	CHECK(far.other == 0 && far.silence > silent, "silence after the encode: %u more, %u torn", far.silence - silent,
	      far.other);
	radio_ysf_tx_abort();
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
}

static void test_transmit(void)
{
	channel(7, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_check() == TXDENY_NONE, "tx allowed");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	CHECK(!flat_rx, "receive off while transmitting");

	static ysf_engine_t rx;
	memset(&far, 0, sizeof far);
	far_next = 1;
	static const ysf_rx_cb_t cb = { f_frame, f_call, f_voice, NULL, f_end, NULL };
	ysf_path_t p = { .rate = 8000 };
	ysf_engine_rx_start(&rx, &p, &cb);

	// One second of speech (50 microphone blocks numbered 1..50), then PTT released
	uint32_t block = 0, samples = 0;
	bool done = false;
	for (int t = 0; t < 400 && !done; t++) {
		int16_t mic[160], spk[160];
		memset(mic, 0, sizeof mic);
		if (block < 50) {
			mic[0] = (int16_t)(++block);
		} else if (block == 50) {
			radio_ysf_tx_release();
			block++;
		}
		stream_cb(mic, spk, NULL);
		// The board pulls the modulation at the air rate
		int16_t pcm[160];
		tx_pull_fn(pcm, 160);
		samples += 160;
		ysf_engine_rx(&rx, pcm, 160);
		now_ms += 20;
		done = radio_ysf_tx_done(now_ms);
	}
	CHECK(done, "tx done after the terminator");
	CHECK(far.firsts == 1 && far.ends == 1, "far end: %u overs, %u terminators", far.firsts, far.ends);
	char s[11], d[11];
	ysf_callsign_get(far.call.csd.src, s);
	ysf_callsign_get(far.call.csd.dest, d);
	// With a radio ID, a call to everyone goes as a Yaesu radio's: radio ID mode, "*****" and our ID, the ID in Rem4
	CHECK(strcmp(s, "G4XYZ") == 0 && strcmp(d, "*****E0123") == 0 && far.cm == YSF_CM_RADIO_ID && far.sc == 7,
	      "far end heard '%s' -> '%s' CM %u dgid %u", s, d, far.cm, far.sc);
	CHECK(memcmp(far.call.csd.rem[3], "E0123", 5) == 0, "far end: Rem4 '%.5s'", far.call.csd.rem[3]);
	CHECK(far.match == 50 && far.match + far.silence == far.voices, "far end: %u voice frames, %u ours in order, %u silent",
	      far.voices, far.match, far.silence);
	CHECK(enc_n >= 50, "encoded %u", enc_n);
	radio_ysf_rx_again(now_ms);
	CHECK(flat_rx, "receive again");
	radio_ysf_stop();
}

// No radio ID: a call to everyone still goes in radio ID mode, "*****" and our ID's five spaces. A Yaesu radio
// (an FTM-510DE, PR #833) showed no caller for the group / CQ form we sent here before, which no Yaesu radio sends: its
// own call to everyone is radio ID mode (ysf-modem.md 1.6). The source callsign is in the same place either way
static void test_transmit_no_id(void)
{
	station_id = "";
	channel(0, 0, "G4XYZ");
	station_id = "E0123";
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	far_start();
	for (int t = 0; t < 40; t++) {
		int16_t mic[160], spk[160], pcm[160];
		memset(mic, 0, sizeof mic);
		stream_cb(mic, spk, NULL);
		tx_pull_fn(pcm, 160);
		ysf_engine_rx(&far_rx, pcm, 160);
		now_ms += 20;
	}
	char s[11], d[11];
	ysf_callsign_get(far.call.csd.src, s);
	ysf_callsign_get(far.call.csd.dest, d);
	CHECK(far.firsts == 1 && far.cm == YSF_CM_RADIO_ID && strcmp(d, "*****") == 0 && strcmp(s, "G4XYZ") == 0,
	      "no radio ID: %u overs, CM %u, '%s' -> '%s'", far.firsts, far.cm, s, d);
	CHECK(memcmp(far.call.csd.dest + 5, "     ", 5) == 0 && memcmp(far.call.csd.rem[3], "     ", 5) == 0,
	      "no radio ID: the source ID half '%.5s', Rem4 '%.5s'", far.call.csd.dest + 5, far.call.csd.rem[3]);
	radio_ysf_tx_abort();
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
}

// The terminator goes out whole when the board pulls in blocks longer than the audio's, as a modem chip's transmit
// RAM asks for them (halves of 256 samples, 32 ms): the terminator ends anywhere inside one, and the block it ends in
// must be sent as the engine filled it. Its second data unit, the frame's last 30 ms or so, then decodes at the far
// end with a good CRC, and no unit of the over fails
static void test_transmit_whole_terminator(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	far_start();
	uint32_t block = 0, owed = 0;
	bool done = false;
	for (int t = 0; t < 400 && !done; t++) {
		int16_t mic[160], spk[160], pcm[256];
		memset(mic, 0, sizeof mic);
		if (block < 50) {
			mic[0] = (int16_t)(++block);
		} else if (block == 50) {
			radio_ysf_tx_release();
			block++;
		}
		stream_cb(mic, spk, NULL);
		for (owed += 160; owed >= 256; owed -= 256) {
			tx_pull_fn(pcm, 256);
			ysf_engine_rx(&far_rx, pcm, 256);
		}
		now_ms += 20;
		done = radio_ysf_tx_done(now_ms);
	}
	const ysf_rx_t *r = &far_rx.u.rx.fr;
	CHECK(done && far.ends == 1, "done %d, far end: %u terminators", done, far.ends);
	CHECK(r->csd_bad == 0 && r->csd_good >= 4, "far end: %u data units good, %u bad (the terminator cut short)",
	      r->csd_good, r->csd_bad);
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
}

// One over of a 400 Hz tone at a peak of `amp` from the mic: the largest sample the encoder got, and the stats
static int32_t dn_over(int16_t amp, radio_ysf_stats_t *st)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	enc_peak = 0;
	for (int t = 0; t < 40; t++) {
		int16_t mic[160], spk[160], pcm[160];
		for (int i = 0; i < 160; i++) {	// a cosine: the first sample isn't 0, which the fake codec takes as silence
			mic[i] = (int16_t)lrint(amp * cos(2.0 * M_PI * 400.0 * (t * 160 + i) / 8000.0));
		}
		stream_cb(mic, spk, NULL);
		tx_pull_fn(pcm, 160);
		now_ms += 20;
	}
	radio_ysf_stats(st);
	radio_ysf_tx_abort();
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
	return enc_peak;
}

// The mic gain on Fusion DN (radio/mic_gain.h): a quiet mic (-31 dBFS of speech on one radio, 13 dB under a Yaesu radio's DN) lifted by the
// board's 12 dB and the setting's step, a loud one held under the limiter's ceiling, unity left alone
static void test_dn_mic_gain(void)
{
	radio_ysf_stats_t st;
	radio.mic_pcm_db = 0;
	radio_set_mic_gain(MIC_GAIN_UNITY);
	CHECK(dn_over(1300, &st) == 1300, "unity: the samples as delivered");
	CHECK(st.tx_mic_peak == 1300 && st.tx_enc_peak == 1300 && st.tx_mic_low_q8 == MICLIMIT_UNITY,
	      "unity stats: %u %u %u", (unsigned)st.tx_mic_peak, (unsigned)st.tx_enc_peak, (unsigned)st.tx_mic_low_q8);

	radio.mic_pcm_db = 12;
	int32_t p = dn_over(1300, &st);
	CHECK(abs(p - 1300 * 1019 / 256) <= 2, "+12 dB: %d, want about %d", p, 1300 * 1019 / 256);
	CHECK(st.tx_mic_peak == 1300 && st.tx_enc_peak == (uint32_t)p && st.tx_mic_low_q8 == miclimit_db_q8(12),
	      "+12 dB stats: %u %u %u", (unsigned)st.tx_mic_peak, (unsigned)st.tx_enc_peak, (unsigned)st.tx_mic_low_q8);

	// The setting's step goes on top of the board's: step 0 is 12 - 10 = +2 dB
	radio_set_mic_gain(0);
	p = dn_over(1300, &st);
	CHECK(abs(p - 1300 * 322 / 256) <= 2, "step 0: %d", p);
	radio_set_mic_gain(MIC_GAIN_UNITY);

	// A shout: 20000 x 4 would clip; the limiter keeps the encoder's input under its ceiling and says it cut
	p = dn_over(20000, &st);
	CHECK(p <= MICLIMIT_CEILING && p > MICLIMIT_CEILING - 300, "loud: %d", p);
	CHECK(st.tx_mic_peak == 20000 && st.tx_mic_low_q8 < miclimit_db_q8(12) && miclimit_q8_db(st.tx_mic_low_q8) <= 3,
	      "loud stats: %u, lowest %u", (unsigned)st.tx_mic_peak, (unsigned)st.tx_mic_low_q8);
	radio.mic_pcm_db = 0;
}

// One over's first 800 ms (preamble, headers, voice) into the independent receiver: the largest sample sent and the
// polarity the receiver found
static void tx_level(int32_t *peak, bool *inverted)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	far_start();
	*peak = 0;
	for (int t = 0; t < 40; t++) {
		int16_t mic[160], spk[160], pcm[160];
		memset(mic, 0, sizeof mic);
		stream_cb(mic, spk, NULL);
		tx_pull_fn(pcm, 160);
		for (int i = 0; i < 160; i++) {
			*peak = abs(pcm[i]) > *peak ? abs(pcm[i]) : *peak;
		}
		ysf_engine_rx(&far_rx, pcm, 160);
		now_ms += 20;
	}
	*inverted = far_rx.u.rx.fr.invert;
	CHECK(far.firsts == 1, "the far end heard the over");
	radio_ysf_tx_abort();
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
}

// The bench's calibration in the settings (the deviation scale, the transmit polarity) reaches Fusion's transmitter
// as it does D-STAR's: the board says 3000 a kHz, the settings 1500 and inverted
static void test_settings_cal(void)
{
	int32_t peak0, peak1;
	bool inv0, inv1;
	memset(&sets, 0, sizeof sets);
	tx_level(&peak0, &inv0);
	sets.flat_tx_1khz = 1500;
	sets.flat_flags = SETTINGS_FLAT_TX_INV_SET | SETTINGS_FLAT_TX_INV;
	tx_level(&peak1, &inv1);
	memset(&sets, 0, sizeof sets);
	CHECK(peak1 * 100 > peak0 * 45 && peak1 * 100 < peak0 * 55, "settings' scale: peak %d against the board's %d",
	      (int)peak1, (int)peak0);
	CHECK(!inv0 && inv1, "settings' polarity: received inverted %d (board's: %d)", inv1, inv0);
}

static void test_refusals(void)
{
	channel(0, 0, NULL);	// no Fusion record and no radio name: no callsign
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_check() == TXDENY_NO_CALL, "no callsign: %u", radio_ysf_tx_check());
	radio_ysf_stop();
	channel(0, 0, "G4XYZ");
	voc_present = false;
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_check() == TXDENY_NO_CODEC, "no codec: %u", radio_ysf_tx_check());
	radio_ysf_stop();
	voc_present = true;
}

// After PTT release the board's interrupt stops asking for halves (nothing of the route is measured on a radio yet):
// the transmission still ends within the drain bound and the pull is stopped, so the core unkeys rather than holding
// a dead carrier until the time-out. Held PTT stays the core's to time out
static void test_stalled_route(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	int16_t pcm[160];
	for (int t = 0; t < 25; t++) {
		step();
		tx_pull_fn(pcm, 160);
	}
	radio_ysf_tx_release();
	uint32_t t = 0, bound = RADIO_YSF_DRAIN_MS + cal.tx_latency_ms;
	while (!radio_ysf_tx_done(now_ms) && t < bound + 2000u) {
		step();
		t += 20;
	}
	CHECK(radio_ysf_tx_done(now_ms) && t >= bound && t <= bound + 40u, "done after %u ms (bound %u)", t, bound);
	CHECK(tx_pull_fn == NULL, "the pull stopped");
	radio_ysf_rx_again(now_ms);
	CHECK(flat_rx, "receive again");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start again");
	for (t = 0; t < bound + 500u; t += 20) {
		step();
	}
	CHECK(!radio_ysf_tx_done(now_ms) && tx_pull_fn != NULL, "held PTT: still on");
	radio_ysf_tx_abort();
	CHECK(radio_ysf_tx_done(now_ms) && tx_pull_fn == NULL, "aborted: done");
	radio_ysf_rx_again(now_ms);
	radio_ysf_stop();
}

// The mode stopped in mid-transmission (a retune, the scope, a beacon): the pull stopped, and the next start receives
// at once and has nothing on air to wait for. The stop once left the transmission flagged on, so the restarted mode
// read nothing from the receiver and said "not done" with nothing to finish
static void test_stop_mid_tx(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	CHECK(radio_ysf_tx_start(now_ms) && tx_pull_fn != NULL, "tx start");
	int16_t pcm[160];
	for (int t = 0; t < 10; t++) {
		step();
		tx_pull_fn(pcm, 160);
	}
	radio_ysf_stop();
	CHECK(tx_pull_fn == NULL && !flat_rx, "stopped mid-transmission: the pull gone");
	CHECK(radio_ysf_start(5, &ch, 0), "start again");
	CHECK(radio_ysf_tx_done(now_ms), "nothing on air to wait for");
	quiet(200);
	send_other("M0RST", 0, 15);
	quiet(600);
	radio_ysf_view_t v;
	radio_ysf_view(&v);
	CHECK(v.heard && strcmp(v.src, "M0RST") == 0, "heard after the restart: %d '%s'", v.heard, v.src);
	radio_ysf_stop();
}

// Started while the other mode still holds the engine area (an ordering bug in the core): PTT is refused, and a
// transmission started anyway is refused too rather than writing through a NULL engine
static void test_no_area(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_mode_ram_take(RADIO_MODE_RAM_DSTAR, 1) != NULL, "the other mode holds the area");
	radio_ysf_start(5, &ch, 0);
	CHECK(radio_ysf_tx_check() != TXDENY_NONE, "PTT refused");
	CHECK(!radio_ysf_tx_start(now_ms) && tx_pull_fn == NULL, "tx start refused");
	radio_ysf_stop();
	radio_mode_ram_give(RADIO_MODE_RAM_DSTAR);
}

// --- the debug link's read of the counters, preempted by the radio task ---

// Run by the module's next read of the engine's receiver (ysfRadioSeam.c), then cleared
static void (*at_receiver)(void);

const ysf_rx_t *ysf_seam_receiver(const ysf_engine_t *e)
{
	void (*f)(void) = at_receiver;
	at_receiver = NULL;
	if (f != NULL) {
		f();
	}
	return ysf_engine_receiver(e);
}

// The radio task tunes a D-STAR channel: Fusion stops, D-STAR takes the engine area and fills it with its own engine.
// Its bytes are whatever D-STAR holds; these read as a Fusion receiver with every counter huge, as D-STAR's may
static void dstar_takes_area(void)
{
	radio_ysf_stop();
	uint8_t *a = radio_mode_ram_take(RADIO_MODE_RAM_DSTAR, 1);
	CHECK(a != NULL, "D-STAR takes the area");
	if (a != NULL) {
		memset(a, 0xA5, radio_mode_ram_size());
		((ysf_engine_t *)(void *)a)->mode = YSF_ENGINE_RX;
	}
}

static bool same_counts(const radio_ysf_stats_t *a, const radio_ysf_stats_t *b)
{
	return a->syncs == b->syncs && a->late == b->late && a->fich_good == b->fich_good && a->fich_bad == b->fich_bad &&
	       a->csd_good == b->csd_good && a->csd_bad == b->csd_bad && a->order_all == b->order_all &&
	       a->sync_bits == b->sync_bits && a->sync_errors == b->sync_errors && a->rx_overs == b->rx_overs;
}

// The debug link reads the counters on the host task; the radio task, above it, can stop Fusion and start D-STAR in
// the middle of that read. The counters must be Fusion's own, before and after: the read took eng, was preempted, and
// then added in the receiver's counters from D-STAR's bytes in the shared area
static void test_stats_preempted(void)
{
	channel(0, 0, "G4XYZ");
	radio_ysf_init(&audio);
	CHECK(radio_ysf_start(5, &ch, 0), "start");
	quiet(200);
	send_other("M0ABC", 0, 15);
	quiet(600);
	radio_ysf_stats_t before, st;
	radio_ysf_stats(&before);
	CHECK(before.syncs > 0 && before.fich_good > 0, "the receiver counted: syncs %u fich %u",
	      before.syncs, before.fich_good);
	at_receiver = dstar_takes_area;
	radio_ysf_stats(&st);
	if (at_receiver != NULL) {
		at_receiver = NULL;	// the read never looked at the engine: the radio task's switch comes after it
		dstar_takes_area();
	}
	CHECK(radio_mode_ram_owner() == RADIO_MODE_RAM_DSTAR, "D-STAR holds the area");
	CHECK(same_counts(&st, &before) && st.offset_hz == before.offset_hz && st.ppm == before.ppm,
	      "preempted read: syncs %u (was %u), fich_good %u (was %u), offset %d (was %d)", st.syncs, before.syncs,
	      st.fich_good, before.fich_good, st.offset_hz, before.offset_hz);
	radio_ysf_stats(&st);
	CHECK(same_counts(&st, &before), "read with D-STAR running: syncs %u (was %u)", st.syncs, before.syncs);
	radio_mode_ram_give(RADIO_MODE_RAM_DSTAR);
}

int main(void)
{
	test_receive();
	test_dgid();
	test_silence_first_over();
	test_transmit();
	test_transmit_no_id();
	test_transmit_whole_terminator();
	test_settings_cal();
	test_refusals();
	test_stalled_route();
	test_stop_mid_tx();
	test_no_area();
	test_stats_preempted();
	test_dn_mic_gain();
	sim_flash_free();
	printf("%s ysfRadioTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
