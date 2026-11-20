// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR voice: engine frames <-> half-rate decoder <-> PCM blocks (dstarVoice.h)
#include "config.h"
#if CONFIG_DSTAR	// the whole file
#include "dstarVoice.h"

#include <stddef.h>
#include <string.h>

#if (DSTAR_VOICE_QUEUE & (DSTAR_VOICE_QUEUE - 1)) != 0 || DSTAR_VOICE_QUEUE > 128 || DSTAR_VOICE_QUEUE < 2 * DSTAR_VOICE_PRIME
#error "DSTAR_VOICE_QUEUE must be a power of two up to 128 holding the prime twice"
#endif
#if (DSTAR_VOICE_TX_QUEUE & (DSTAR_VOICE_TX_QUEUE - 1)) != 0 || DSTAR_VOICE_TX_QUEUE > 128
#error "DSTAR_VOICE_TX_QUEUE must be a power of two up to 128"
#endif

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static const hal_audio_t *aud;
static bool streaming;			// radio task only
static dstar_voice_mem_t *mem;		// set before the stream starts, cleared after it stops
static ambe_mem_t dec;			// the decoder's pointers into mem
static ambe_enc_mem_t enc;		// the encoder's: the decoder's prediction state and scratch, its own history

// Queue: producer the provider's task (head), consumer the audio callback (tail)
static uint8_t rx_head, rx_tail;
// Call boundaries, written by the provider's task only: a start bumps the sequence, an end copies it
static uint8_t rx_seq, rx_end_seq;

// Transmit. The radio task counts transmissions (odd while one is on); the queue's producer is the audio callback
// (head), its consumer the provider's task (tail). Frames of an earlier transmission are dropped by their count
static uint8_t tx_gen;
static uint8_t tx_head, tx_tail;
static uint16_t mic_gain_q8 = 256;	// dstarVoiceSetMicGain: the mic samples times this / 256 before encoding

// Audio callback state
static uint8_t cb_tx_gen;		// the transmission the encoder runs for (even: none)
static uint8_t cb_seq;
static bool cb_live, cb_primed, cb_playing, cb_speaker, cb_mixing;
static uint32_t cb_call_frames;

static dstar_voice_stats_t stats;
static dstar_voice_mixer_t mixer;

// --- hooks (provider's task) ---

static void rx_start(const dstar_call_t *c, void *ctx)
{
	(void)c;
	(void)ctx;
	STORE(rx_seq, (uint8_t)(rx_seq + 1));
}

static void rx_voice(const uint8_t voice[DSTAR_VOICE_BYTES], void *ctx)
{
	(void)ctx;
	if (rx_end_seq == rx_seq) {
		rx_start(NULL, NULL);	// frames with no call open: a new call
	}
	stats.rx_in++;
	uint8_t h = rx_head, used = (uint8_t)(h - LOAD(rx_tail));
	if (used >= DSTAR_VOICE_QUEUE) {
		stats.rx_overflows++;
		return;
	}
	if (used + 1u > stats.rx_queue_max) {
		stats.rx_queue_max = used + 1u;
	}
	dstar_voice_entry_t *e = &mem->q[h & (DSTAR_VOICE_QUEUE - 1)];
	e->seq = rx_seq;
	memcpy(e->voice, voice, DSTAR_VOICE_BYTES);
	STORE(rx_head, (uint8_t)(h + 1));
}

static void rx_end(void *ctx)
{
	(void)ctx;
	STORE(rx_end_seq, rx_seq);
}

// The encoder's next frame (false: none ready; the engine waits while the air can spare it, then sends silence and
// counts it as filler)
static bool tx_voice(uint8_t voice[DSTAR_VOICE_BYTES], void *ctx)
{
	(void)ctx;
	uint8_t g = LOAD(tx_gen), t = tx_tail, h = LOAD(tx_head);

	while (t != h && mem->tq[t & (DSTAR_VOICE_TX_QUEUE - 1)].gen != g) {
		t++;	// left over from an earlier transmission
	}
	if (t == h) {
		STORE(tx_tail, t);
		return false;
	}
	memcpy(voice, mem->tq[t & (DSTAR_VOICE_TX_QUEUE - 1)].voice, DSTAR_VOICE_BYTES);
	STORE(tx_tail, (uint8_t)(t + 1));
	stats.tx_frames++;
	return true;
}

static const dstar_engine_hooks_t hooks = { .rx_start = rx_start, .rx_update = NULL, .rx_voice = rx_voice,
					     .rx_end = rx_end, .tx_voice = tx_voice };

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

static void silence(int16_t *spk)
{
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
}

static void rx_block(int16_t *spk)
{
	uint8_t seq = LOAD(rx_seq);
	if (seq != cb_seq) {
		// A new call: the decoder starts over. The speaker route may be off (an end, a stop): sent again in any case
		cb_seq = seq;
		cb_live = true;
		cb_primed = cb_playing = cb_speaker = false;
		cb_call_frames = 0;
		stats.rx_peak = 0;
		ambe_dec_init(&dec);
	}
	// Drop entries of earlier calls
	uint8_t t = rx_tail, h = LOAD(rx_head);
	while (t != h && mem->q[t & (DSTAR_VOICE_QUEUE - 1)].seq != cb_seq) {
		t++;
	}
	STORE(rx_tail, t);

	bool ended = LOAD(rx_end_seq) == cb_seq;
	uint8_t queued = (uint8_t)(h - t);
	if (!cb_live) {
		silence(spk);
		return;
	}
	if (!cb_primed) {
		if (queued < DSTAR_VOICE_PRIME && !(ended && queued > 0)) {
			if (ended) {
				cb_live = false;	// a call that ended before playing anything
			}
			silence(spk);
			return;
		}
		cb_primed = cb_playing = true;
		stats.rx_calls++;
		speaker(true);
	}
	if (queued == 0) {
		if (ended) {
			cb_live = cb_playing = false;
			speaker(false);
			silence(spk);
			return;
		}
		// Nothing in time: the decoder's bad-frame path (repeat the last good frame, then mute)
		stats.rx_underruns++;
		stats.rx_underrun_at = cb_call_frames;
		stats.rx_concealed++;
		ambe_dec_data(&dec, NULL, AMBE_ERRS_LOST, spk);
		return;
	}
	const dstar_voice_entry_t *e = &mem->q[t & (DSTAR_VOICE_QUEUE - 1)];
	uint8_t d[AMBE_DATA_BYTES];
	uint8_t errs = ambe_fec_decode(e->voice, d);
	int run = ambe_dec_data(&dec, d, errs, spk);
	STORE(rx_tail, (uint8_t)(t + 1));
	if (AMBE_ERRS_REPEAT(errs)) {
		stats.rx_bad++;
	} else {
		stats.rx_bit_errors += AMBE_ERRS_TOTAL(errs);
	}
	if (run != 0) {
		stats.rx_concealed++;
		return;
	}
	stats.rx_frames++;
	cb_call_frames++;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		uint32_t a = (uint32_t)(spk[i] < 0 ? -spk[i] : spk[i]);
		if (a > stats.rx_peak) {
			stats.rx_peak = a;
		}
	}
}

// One mic block into the transmission g: encoded and queued for the engine
static void tx_block(uint8_t g, const int16_t *mic)
{
	int16_t scaled[AUDIO_BLOCK_SAMPLES];
	uint16_t gain = LOAD(mic_gain_q8);
	uint32_t peak = 0;

	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t v = gain == 256 ? mic[i] : ((int32_t)mic[i] * gain) / 256;

		v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
		scaled[i] = (int16_t)v;
		if ((uint32_t)(v < 0 ? -v : v) > peak) {
			peak = (uint32_t)(v < 0 ? -v : v);
		}
	}
	if (peak > stats.tx_mic_peak) {
		stats.tx_mic_peak = peak;
	}
	uint8_t h = tx_head;
	if ((uint8_t)(h - LOAD(tx_tail)) >= DSTAR_VOICE_TX_QUEUE) {
		// The engine is not taking frames (not started yet, or ending): the encoder still runs, to keep its history
		uint8_t drop[DSTAR_VOICE_BYTES];

		ambe_enc_frame(&enc, scaled, drop);
		stats.tx_encoded++;
		stats.tx_dropped++;
		return;
	}
	dstar_voice_tx_entry_t *e = &mem->tq[h & (DSTAR_VOICE_TX_QUEUE - 1)];
	e->gen = g;
	ambe_enc_frame(&enc, scaled, e->voice);
	stats.tx_encoded++;
	if (memcmp(e->voice, ambe_silence, DSTAR_VOICE_BYTES) == 0) {
		stats.tx_silent++;
	}
	STORE(tx_head, (uint8_t)(h + 1));
}

void dstarVoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	if (LOAD(mem) == NULL) {
		silence(spk);
		return;
	}
	uint8_t g = LOAD(tx_gen);
	if ((g & 1u) != 0) {
		if (g != cb_tx_gen) {
			cb_tx_gen = g;
			stats.tx_mic_peak = 0;
			ambe_enc_init(&enc);
		}
		tx_block(g, mic);
		silence(spk);
	} else {
		if ((cb_tx_gen & 1u) != 0) {
			// The encoder used the decoder's prediction state: a call still coming in starts from none
			cb_tx_gen = g;
			ambe_dec_init(&dec);
		}
		rx_block(spk);
	}
	dstar_voice_mixer_t mix = LOAD(mixer);
	if (mix != NULL && mix(spk)) {
		cb_mixing = true;
		speaker(true);
	} else if (cb_mixing) {
		cb_mixing = false;
		if (!cb_playing) {
			speaker(false);
		}
	}
}

// --- radio task ---

void dstarVoiceInit(const hal_audio_t *audio)
{
	aud = audio;
}

void dstarVoiceBegin(dstar_voice_mem_t *m)
{
	// The stream is stopped (dstarVoiceEnd) and the engine not started: nothing else touches this state now
	rx_head = rx_tail = 0;
	rx_seq = rx_end_seq = 0;
	tx_head = tx_tail = 0;
	tx_gen = cb_tx_gen = 0;
	cb_seq = 0;
	cb_live = cb_primed = cb_playing = cb_speaker = cb_mixing = false;
	dec.s = &m->st;
	dec.p = &m->pr;
	dec.mbe.s = &m->ms;
	dec.mbe.prev = &m->prev;
	dec.mbe.cur = &m->cur;
	dec.mbe.buf = m->buf;
	ambe_dec_init(&dec);
	enc.s = &m->st.q;
	enc.est = &m->est;
	enc.p = &m->pr;
	enc.buf = m->buf;
	STORE(mem, m);
}

void dstarVoicePoll(void)
{
	if (!streaming && aud != NULL && LOAD(mem) != NULL && LOAD(rx_seq) != 0) {
		streaming = aud->start(dstarVoiceAudioBlock, NULL);
	}
}

void dstarVoiceTx(bool on)
{
	if (LOAD(mem) == NULL || on == ((tx_gen & 1u) != 0)) {
		return;
	}
	STORE(tx_gen, (uint8_t)(tx_gen + 1));
	if (on && !streaming && aud != NULL) {
		streaming = aud->start(dstarVoiceAudioBlock, NULL);
	}
}

void dstarVoiceSetMicGain(uint8_t step)
{
	// 2 dB a step around unity at 5, as the DMR voice path's
	static const uint16_t q8[11] = { 81, 102, 128, 161, 203, 256, 322, 406, 511, 643, 810 };

	STORE(mic_gain_q8, q8[step > 10 ? 10 : step]);
}

void dstarVoiceEnd(void)
{
	if (streaming && aud != NULL) {
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
	streaming = false;
	STORE(mem, (dstar_voice_mem_t *)NULL);
}

bool dstarVoiceStreaming(void)
{
	return streaming;
}

void dstarVoiceSetMixer(dstar_voice_mixer_t m)
{
	STORE(mixer, m);
}

const dstar_engine_hooks_t *dstarVoiceHooks(void)
{
	return &hooks;
}

void dstarVoiceGetStats(dstar_voice_stats_t *out)
{
	*out = stats;
}

void dstarVoiceResetStats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_DSTAR
