// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DMR voice glue: call stack frames <-> FEC <-> vocoder <-> PCM blocks (ambeVoice.h)
#include "dmr/ambe/ambeVoice.h"
#include "dmr/ambe/ambeFec.h"

#include <stddef.h>
#include <string.h>

#if (AMBE_VOICE_QUEUE & (AMBE_VOICE_QUEUE - 1)) != 0 || AMBE_VOICE_QUEUE > 128
#error "AMBE_VOICE_QUEUE must be a power of two up to 128"
#endif

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

typedef struct {
	uint8_t seq;		// call it belongs to
	uint8_t bad;		// 1 uncorrectable, 2 lost
	uint8_t bits[VOCODER_BITS];
} rx_entry_t;

typedef struct {
	uint8_t seq;
	uint8_t frame[AMBE_FRAME_BYTES];
} tx_entry_t;

static const hal_audio_t *aud;
static const hal_vocoder_t *voc;
static bool streaming;			// call task only

// RX queue: producer the call task (head), consumer the audio callback (tail)
static rx_entry_t rxq[AMBE_VOICE_QUEUE];
static uint8_t rx_head, rx_tail;
// TX queue: producer the audio callback (head), consumer the call task (tail)
static tx_entry_t txq[AMBE_VOICE_QUEUE];
static uint8_t tx_head, tx_tail;

// Call boundaries, written by the call task only: begin bumps the sequence, end copies it
static uint8_t rx_seq, rx_end_seq;
static uint8_t rx_place;	// the next frame's place in its burst (0-2); a call starts at a burst's first
static uint8_t rx_prev[VOCODER_BITS];	// the call's previous good frame (rx_repeats)
static bool rx_have_prev;
static uint8_t tx_seq, tx_end_seq;	// equal: no call (also at start-up)

// Audio callback state
static uint8_t cb_rx_seq, cb_tx_seq;
static bool cb_rx_playing, cb_rx_primed, cb_rx_live;
static bool cb_speaker;
static uint8_t cb_bad_run;
static uint32_t cb_call_frames;		// frames played in this call (rx_underrun_at)
static uint8_t last_bits[VOCODER_BITS];
static bool have_last;

static ambe_voice_stats_t stats;
static ambe_voice_mixer_t mixer;
static bool cb_mixing;
static uint16_t mic_gain_q8 = 256;	// ambeVoiceSetMicGain: the mic samples times this / 256 before encoding
static bool test_signal;		// ambeVoiceSetTestSignal
static uint16_t test_phase;		// the test signal's phase (a full cycle is 65536) and its pitch step
static uint8_t test_blocks;
static uint8_t tx_prev[VOCODER_BITS];	// the transmission's previous frame (tx_repeats)
static bool tx_have_prev;
static uint32_t enc_opts = VOCODER_OPT_DEFAULT;	// ambeVoiceSetEncodeOptions: any task
static uint32_t cb_enc_opts;		// what the vocoder was last given (audio callback)
static bool cb_enc_opts_set;		// cb_enc_opts is valid

static void ensure_streaming(void)
{
	if (!streaming && aud != NULL) {
		streaming = aud->start(ambeVoiceAudioBlock, NULL);
	}
}

// --- sink (call task) ---

static void rx_push(const uint8_t bits[VOCODER_BITS], uint8_t bad)
{
	uint8_t h = rx_head;
	if ((uint8_t)(h - LOAD(rx_tail)) >= AMBE_VOICE_QUEUE) {
		stats.rx_overflows++;
		return;
	}
	if ((uint8_t)(h + 1 - LOAD(rx_tail)) > stats.rx_queue_max) {
		stats.rx_queue_max = (uint8_t)(h + 1 - LOAD(rx_tail));
	}
	rx_entry_t *e = &rxq[h & (AMBE_VOICE_QUEUE - 1)];
	e->seq = rx_seq;
	e->bad = bad;
	if (bits != NULL) {
		memcpy(e->bits, bits, VOCODER_BITS);
	}
	STORE(rx_head, (uint8_t)(h + 1));
}

static void sink_begin(const dmr_caller_t *caller, void *ctx)
{
	(void)caller;
	(void)ctx;
	rx_place = 0;
	rx_have_prev = false;
	STORE(rx_seq, (uint8_t)(rx_seq + 1));
	ensure_streaming();
}

static void sink_frame(const uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx)
{
	(void)ctx;
	uint8_t bits[VOCODER_BITS];
	ambe_fec_result_t r;
	if (rx_end_seq == rx_seq) {
		sink_begin(NULL, NULL);	// frames with no call open: a new call
	}
	stats.rx_in++;
	ambeFecDecode(ambe72, bits, &r);
	if (r.c0_failed) {
		stats.rx_bad++;
		stats.rx_bad_at[rx_place]++;
	} else {
		stats.rx_bit_errors += r.errors;
		if (rx_have_prev && memcmp(bits, rx_prev, VOCODER_BITS) == 0) {
			stats.rx_repeats++;
		}
		memcpy(rx_prev, bits, VOCODER_BITS);
		rx_have_prev = true;
	}
	rx_push(bits, r.c0_failed ? 1 : 0);
	rx_place = (uint8_t)((rx_place + 1u) % 3u);
}

static void sink_lost(void *ctx)
{
	(void)ctx;
	stats.rx_lost++;
	rx_push(NULL, 2);
	rx_place = (uint8_t)((rx_place + 1u) % 3u);
}

static void sink_end(void *ctx)
{
	(void)ctx;
	STORE(rx_end_seq, rx_seq);
}

// --- source (call task) ---

static void source_begin(void *ctx)
{
	(void)ctx;
	STORE(tx_seq, (uint8_t)(tx_seq + 1));
	STORE(tx_tail, LOAD(tx_head));	// drop what an earlier transmission left (next() also skips by sequence)
	ensure_streaming();
}

static bool source_next(uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx)
{
	(void)ctx;
	for (;;) {
		uint8_t t = tx_tail;
		if (t == LOAD(tx_head)) {
			return false;
		}
		const tx_entry_t *e = &txq[t & (AMBE_VOICE_QUEUE - 1)];
		bool current = e->seq == tx_seq;
		if (current) {
			memcpy(ambe72, e->frame, AMBE_FRAME_BYTES);
		}
		STORE(tx_tail, (uint8_t)(t + 1));
		if (current) {
			return true;
		}
		// a frame left over from an earlier transmission: drop it
	}
}

static void source_end(void *ctx)
{
	(void)ctx;
	STORE(tx_end_seq, tx_seq);
}

// --- audio callback (audio task) ---

static void speaker(bool on)
{
	if (cb_speaker != on) {
		cb_speaker = on;
		if (aud != NULL && aud->speaker_pcm != NULL) {
			aud->speaker_pcm(on);
		}
	}
}

static bool voc_ok(void)
{
	return voc != NULL && voc->present();
}

// The test signal: a triangle wave at about -12 dBFS whose pitch moves to the next of five values every block (one
// vocoder frame, 20 ms). Consecutive frames then differ, so a stuck encoder, or a modem that sends one frame again,
// shows as repeats
static void test_block(int16_t *out)
{
	// 300, 500, 700, 1100 and 1500 Hz at 8 kHz, a cycle being 65536
	static const uint16_t step[5] = { 2458, 4096, 5734, 9011, 12288 };
	uint16_t inc = step[test_blocks];
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t tri = test_phase < 32768u ? (int32_t)test_phase - 16384 : 49151 - (int32_t)test_phase;
		out[i] = (int16_t)(tri / 2);	// +-8192
		test_phase = (uint16_t)(test_phase + inc);
	}
	test_blocks = (uint8_t)((test_blocks + 1u) % 5u);
}

static void conceal(int16_t *spk)
{
	if (have_last && cb_bad_run < AMBE_VOICE_REPEAT && voc_ok()) {
		stats.rx_concealed++;
		voc->decode(last_bits, spk);
	} else {
		stats.rx_silence++;
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	}
	if (cb_bad_run < 255) {
		cb_bad_run++;
	}
}

static void rx_block(int16_t *spk)
{
	uint8_t seq = LOAD(rx_seq);
	if (seq != cb_rx_seq) {
		// a new call. The speaker route is off after an end or ambeVoiceStop(); re-send it in any case
		cb_rx_seq = seq;
		cb_speaker = false;
		cb_rx_live = true;
		cb_rx_playing = false;
		cb_rx_primed = false;
		cb_bad_run = 0;
		cb_call_frames = 0;
		have_last = false;
		stats.rx_peak = 0;
		if (voc_ok()) {
			voc->reset();
		}
	}
	// Drop entries of earlier calls
	uint8_t t = rx_tail, h = LOAD(rx_head);
	while (t != h && rxq[t & (AMBE_VOICE_QUEUE - 1)].seq != cb_rx_seq) {
		t++;
	}
	STORE(rx_tail, t);

	bool ended = LOAD(rx_end_seq) == cb_rx_seq;
	uint8_t queued = (uint8_t)(h - t);
	if (!cb_rx_live) {
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		return;
	}
	if (!cb_rx_primed) {
		if (queued >= AMBE_VOICE_PRIME || (ended && queued > 0)) {
			cb_rx_primed = true;
			cb_rx_playing = true;
			stats.rx_calls++;
			speaker(true);
		} else {
			if (ended) {
				cb_rx_live = false;	// a call that ended before playing anything
			}
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
			return;
		}
	}
	if (queued == 0) {
		if (ended) {
			cb_rx_live = false;
			cb_rx_playing = false;
			speaker(false);
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		} else {
			stats.rx_underruns++;
			stats.rx_underrun_at = cb_call_frames;
			conceal(spk);
		}
		return;
	}
	const rx_entry_t *e = &rxq[t & (AMBE_VOICE_QUEUE - 1)];
	if (e->bad == 0) {
		memcpy(last_bits, e->bits, VOCODER_BITS);
		have_last = true;
		cb_bad_run = 0;
		STORE(rx_tail, (uint8_t)(t + 1));
		stats.rx_frames++;
		cb_call_frames++;
		if (voc_ok()) {
			voc->decode(last_bits, spk);
			// A decoder fed the wrong bits plays silence with every counter clean (the DB25-D's stale voice RAM)
			for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				uint32_t a = (uint32_t)(spk[i] < 0 ? -spk[i] : spk[i]);
				if (a > stats.rx_peak) {
					stats.rx_peak = a;
				}
			}
		} else {
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		}
	} else {
		STORE(rx_tail, (uint8_t)(t + 1));
		conceal(spk);
	}
}

// The mic statistics (ambe_voice_stats_t.tx_mic_*): histograms of the blocks' levels and zero crossings, the audio
// callback only
#define MIC_LEVEL_BINS	32u	// floor(log2(mean square)): 3 dB a bin
#define MIC_ZC_BINS	21u	// zero crossings / 8
static uint16_t mic_level_hist[MIC_LEVEL_BINS];
static uint16_t mic_zc_hist[MIC_ZC_BINS];
static int16_t mic_prev;		// the last sample of the previous block

// The bin holding the given fraction (per 1000) of the counts
static uint32_t hist_at(const uint16_t *h, uint32_t bins, uint32_t total, uint32_t permille)
{
	uint32_t want = (total * permille + 999u) / 1000u, sum = 0;
	for (uint32_t i = 0; i < bins; i++) {
		sum += h[i];
		if (sum >= want && sum > 0) {
			return i;
		}
	}
	return bins - 1u;
}

static void mic_stats(const int16_t *mic)
{
	int32_t mean = 0;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mean += mic[i];
	}
	mean /= AUDIO_BLOCK_SAMPLES;
	if (stats.tx_mic_blocks == 0) {
		mic_prev = mic[0];
	}
	// The level from the sample-to-sample difference: a first-order high pass (gain 2 sin(pi f / 8 kHz)) that leaves
	// mains hum and rumble 30 dB down and the voice band within about 6 dB of flat
	uint64_t e = 0;
	uint32_t zc = 0;
	bool neg = mic[0] < mean;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t v = mic[i] - mean, d = (int32_t)mic[i] - mic_prev;
		mic_prev = mic[i];
		e += (uint64_t)((int64_t)d * d);
		if ((v < 0) != neg) {
			neg = v < 0;
			zc++;
		}
	}
	uint32_t ms = (uint32_t)(e / AUDIO_BLOCK_SAMPLES), bin = 0;
	while (ms > 1u && bin < MIC_LEVEL_BINS - 1u) {
		ms >>= 1;
		bin++;
	}
	if (stats.tx_mic_blocks == 0) {	// a new transmission, or ambeVoiceResetStats
		memset(mic_level_hist, 0, sizeof mic_level_hist);
		memset(mic_zc_hist, 0, sizeof mic_zc_hist);
	}
	if (stats.tx_mic_blocks >= 0xFFFFu) {
		return;	// 21 minutes: the counters would wrap
	}
	mic_level_hist[bin]++;
	mic_zc_hist[zc / 8u < MIC_ZC_BINS ? zc / 8u : MIC_ZC_BINS - 1u]++;
	uint32_t n = ++stats.tx_mic_blocks;
	uint32_t loud = hist_at(mic_level_hist, MIC_LEVEL_BINS, n, 900u);
	stats.tx_mic_level_db = 3u * loud;
	stats.tx_mic_spread_db = 3u * (loud - hist_at(mic_level_hist, MIC_LEVEL_BINS, n, 100u));
	stats.tx_mic_zc = 8u * hist_at(mic_zc_hist, MIC_ZC_BINS, n, 500u) + 4u;
}

static void tx_block(const int16_t *mic)
{
	uint8_t seq = LOAD(tx_seq);
	if (LOAD(tx_end_seq) == seq) {
		return;	// not transmitting
	}
	if (seq != cb_tx_seq) {
		cb_tx_seq = seq;
		stats.tx_mic_level_db = stats.tx_mic_spread_db = stats.tx_mic_zc = stats.tx_mic_blocks = 0;
		if (voc_ok()) {
			voc->reset();	// fresh encoder: the first frame of a transmission
		}
		stats.tx_mic_peak = 0;
		tx_have_prev = false;
		test_blocks = 0;
	}
	mic_stats(mic);	// the board's samples, before the gain scaling
	// Options change only between frames, in this task, so both halves of a frame see the same ones
	uint32_t opts = LOAD(enc_opts);
	if (voc_ok() && voc->set_options != NULL && (!cb_enc_opts_set || opts != cb_enc_opts)) {
		voc->set_options(opts);
		cb_enc_opts = opts;
		cb_enc_opts_set = true;
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		uint32_t a = (uint32_t)(mic[i] < 0 ? -mic[i] : mic[i]);
		if (a > stats.tx_mic_peak) {
			stats.tx_mic_peak = a;
		}
	}
	uint8_t bits[VOCODER_BITS];
	static int16_t scaled[AUDIO_BLOCK_SAMPLES];	// the audio callback only (off its stack)
	if (LOAD(test_signal)) {
		test_block(scaled);
		mic = scaled;
	}
	uint16_t g = LOAD(mic_gain_q8);
	if (g != 256) {
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			int32_t v = ((int32_t)mic[i] * g) / 256;
			scaled[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		mic = scaled;
	}
	if (voc_ok()) {
		voc->encode(mic, bits);
	} else {
		memset(bits, 0, sizeof bits);
	}
	if (tx_have_prev && memcmp(bits, tx_prev, VOCODER_BITS) == 0) {
		stats.tx_repeats++;
	}
	memcpy(tx_prev, bits, VOCODER_BITS);
	tx_have_prev = true;
	uint8_t h = tx_head;
	if ((uint8_t)(h - LOAD(tx_tail)) >= AMBE_VOICE_QUEUE) {
		stats.tx_overflows++;
		return;
	}
	tx_entry_t *e = &txq[h & (AMBE_VOICE_QUEUE - 1)];
	e->seq = seq;
	ambeFecEncode(bits, e->frame);
	STORE(tx_head, (uint8_t)(h + 1));
	stats.tx_frames++;
}

void ambeVoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	// DMR is simplex: TX first (a fresh transmission resets the vocoder), then RX playback
	tx_block(mic);
	rx_block(spk);
	ambe_voice_mixer_t mix = LOAD(mixer);
	if (mix != NULL && mix(spk)) {
		cb_mixing = true;
		speaker(true);
	} else if (cb_mixing) {
		cb_mixing = false;
		if (!cb_rx_playing) {
			speaker(false);
		}
	}
}

// --- set-up ---

// 2 dB per step around step 5 (unity): -10 dB .. +10 dB, Q8
void ambeVoiceSetMicGain(uint8_t step)
{
	static const uint16_t q8[11] = { 81, 102, 128, 161, 203, 256, 322, 406, 511, 643, 810 };
	STORE(mic_gain_q8, q8[step > 10 ? 10 : step]);
}

void ambeVoiceSetEncodeOptions(uint32_t opts)
{
	STORE(enc_opts, opts);
}

uint32_t ambeVoiceEncodeOptions(void)
{
	return LOAD(enc_opts) & ambeVoiceEncodeOptionsSupported();
}

uint32_t ambeVoiceEncodeOptionsSupported(void)
{
	return voc != NULL && voc->options != NULL && voc->set_options != NULL ? voc->options() : 0;
}

static const dmr_voice_sink_t sink = {
	.begin = sink_begin, .frame = sink_frame, .lost = sink_lost, .end = sink_end, .ctx = NULL,
};
static const dmr_voice_source_t source = {
	.begin = source_begin, .next = source_next, .end = source_end, .ctx = NULL,
};

void ambeVoiceInit(const hal_audio_t *audio, const hal_vocoder_t *vocoder)
{
	aud = audio;
	voc = vocoder;
	cb_enc_opts_set = false;	// a new vocoder is told the options before its first frame
}

const dmr_voice_sink_t *ambeVoiceSink(void)
{
	return &sink;
}

const dmr_voice_source_t *ambeVoiceSource(void)
{
	return &source;
}

void ambeVoiceStop(void)
{
	STORE(rx_end_seq, rx_seq);
	STORE(tx_end_seq, tx_seq);
	if (streaming && aud != NULL) {
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
	streaming = false;
}

bool ambeVoiceStreaming(void)
{
	return streaming;
}

void ambeVoiceSetMixer(ambe_voice_mixer_t m)
{
	STORE(mixer, m);
}

void ambeVoiceSetTestSignal(bool on)
{
	STORE(test_signal, on);
}

bool ambeVoiceTestSignal(void)
{
	return LOAD(test_signal);
}

void ambeVoiceResetStats(void)
{
	memset(&stats, 0, sizeof(stats));
}

void ambeVoiceGetStats(ambe_voice_stats_t *out)
{
	*out = stats;
}
