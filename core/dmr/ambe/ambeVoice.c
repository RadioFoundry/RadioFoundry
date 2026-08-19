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
static uint8_t tx_seq, tx_end_seq;	// equal: no call (also at start-up)

// Audio callback state
static uint8_t cb_rx_seq, cb_tx_seq;
static bool cb_rx_playing, cb_rx_primed, cb_rx_live;
static bool cb_speaker;
static uint8_t cb_bad_run;
static uint8_t last_bits[VOCODER_BITS];
static bool have_last;

static ambe_voice_stats_t stats;
static ambe_voice_mixer_t mixer;
static bool cb_mixing;

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
	} else {
		stats.rx_bit_errors += r.errors;
	}
	rx_push(bits, r.c0_failed ? 1 : 0);
}

static void sink_lost(void *ctx)
{
	(void)ctx;
	stats.rx_lost++;
	rx_push(NULL, 2);
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
		have_last = false;
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
		if (voc_ok()) {
			voc->decode(last_bits, spk);
		} else {
			memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
		}
	} else {
		STORE(rx_tail, (uint8_t)(t + 1));
		conceal(spk);
	}
}

static void tx_block(const int16_t *mic)
{
	uint8_t seq = LOAD(tx_seq);
	if (LOAD(tx_end_seq) == seq) {
		return;	// not transmitting
	}
	if (seq != cb_tx_seq) {
		cb_tx_seq = seq;
		if (voc_ok()) {
			voc->reset();	// fresh encoder: the first frame of a transmission
		}
	}
	uint8_t bits[VOCODER_BITS];
	if (voc_ok()) {
		voc->encode(mic, bits);
	} else {
		memset(bits, 0, sizeof bits);
	}
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

void ambeVoiceResetStats(void)
{
	memset(&stats, 0, sizeof(stats));
}

void ambeVoiceGetStats(ambe_voice_stats_t *out)
{
	*out = stats;
}
