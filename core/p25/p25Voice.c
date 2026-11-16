// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 voice: engine frames <-> full-rate decoder <-> PCM blocks (p25Voice.h)
#include "config.h"
#if CONFIG_P25 && CONFIG_IMBE	// the whole file
#include "p25Voice.h"

#include <stddef.h>
#include <string.h>

#if (P25_VOICE_QUEUE & (P25_VOICE_QUEUE - 1)) != 0 || P25_VOICE_QUEUE > 128 || P25_VOICE_QUEUE < 2 * P25_VOICE_FRAMES
#error "P25_VOICE_QUEUE must be a power of two up to 128 holding two LDUs"
#endif

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static const hal_audio_t *aud;
static bool streaming;			// radio task only
static p25_voice_mem_t *mem;		// set before the stream starts, cleared after it stops
static imbe_mem_t dec;			// the decoder's pointers into mem

// Queue: producer the provider's task (head), consumer the audio callback (tail)
static uint8_t rx_head, rx_tail;
// Call boundaries, written by the provider's task only: a start bumps the sequence, an end copies it
static uint8_t rx_seq, rx_end_seq;

// Audio callback state
static uint8_t cb_seq;
static bool cb_live, cb_primed, cb_playing, cb_speaker, cb_mixing;
static uint8_t cb_lead;			// silent blocks left before the first frame plays
static uint32_t cb_call_frames;

static p25_voice_stats_t stats;
static p25_voice_mixer_t mixer;

// --- hooks (provider's task) ---

static void rx_start(const p25_call_t *c, void *ctx)
{
	(void)c;
	(void)ctx;
	STORE(rx_seq, (uint8_t)(rx_seq + 1));
}

static void rx_update(const p25_call_t *c, void *ctx)
{
	(void)c;
	(void)ctx;
}

static void rx_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	(void)ctx;
	if (rx_end_seq == rx_seq) {
		rx_start(NULL, NULL);	// frames with no call open: a new call
	}
	stats.rx_in++;
	if (errs > IMBE_ERRS_MAX) {
		stats.rx_bad++;
	} else {
		stats.rx_bit_errors += errs;
	}
	uint8_t h = rx_head, used = (uint8_t)(h - LOAD(rx_tail));
	if (used >= P25_VOICE_QUEUE) {
		stats.rx_overflows++;
		return;
	}
	if (used + 1u > stats.rx_queue_max) {
		stats.rx_queue_max = used + 1u;
	}
	p25_voice_entry_t *e = &mem->q[h & (P25_VOICE_QUEUE - 1)];
	e->seq = rx_seq;
	e->errs = errs;
	memcpy(e->imbe, imbe, P25_IMBE_BYTES);
	STORE(rx_head, (uint8_t)(h + 1));
}

static void rx_end(void *ctx)
{
	(void)ctx;
	STORE(rx_end_seq, rx_seq);
}

// No encoder yet: every frame is silence. An encoder hands over its next frame here (false: none ready, the engine
// sends silence and counts it as filler)
static bool tx_voice(uint8_t imbe[P25_IMBE_BYTES], void *ctx)
{
	(void)ctx;
	memcpy(imbe, imbe_silence, P25_IMBE_BYTES);
	stats.tx_frames++;
	return true;
}

static const p25_engine_hooks_t hooks = { .rx_start = rx_start, .rx_update = rx_update, .rx_voice = rx_voice,
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
		cb_lead = 0;
		cb_call_frames = 0;
		stats.rx_peak = 0;
		imbe_dec_init(&dec);
	}
	// Drop entries of earlier calls
	uint8_t t = rx_tail, h = LOAD(rx_head);
	while (t != h && mem->q[t & (P25_VOICE_QUEUE - 1)].seq != cb_seq) {
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
		if (queued < P25_VOICE_PRIME && !(ended && queued > 0)) {
			if (ended) {
				cb_live = false;	// a call that ended before playing anything
			}
			silence(spk);
			return;
		}
		cb_primed = cb_playing = true;
		cb_lead = P25_VOICE_LEAD;
		stats.rx_calls++;
		speaker(true);
	}
	if (cb_lead != 0) {
		cb_lead--;
		silence(spk);
		return;
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
		imbe_dec_frame(&dec, imbe_silence, 0xFF, spk);
		return;
	}
	const p25_voice_entry_t *e = &mem->q[t & (P25_VOICE_QUEUE - 1)];
	int run = imbe_dec_frame(&dec, e->imbe, e->errs, spk);
	STORE(rx_tail, (uint8_t)(t + 1));
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

void p25VoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;	// the encoder's input, once there is one
	(void)ctx;
	if (LOAD(mem) == NULL) {
		silence(spk);
		return;
	}
	rx_block(spk);
	p25_voice_mixer_t mix = LOAD(mixer);
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

void p25VoiceInit(const hal_audio_t *audio)
{
	aud = audio;
}

void p25VoiceBegin(p25_voice_mem_t *m)
{
	// The stream is stopped (p25VoiceEnd) and the engine not started: nothing else touches this state now
	rx_head = rx_tail = 0;
	rx_seq = rx_end_seq = 0;
	cb_seq = 0;
	cb_live = cb_primed = cb_playing = cb_speaker = cb_mixing = false;
	dec.s = &m->st;
	dec.p = &m->pr;
	dec.mbe.s = &m->ms;
	dec.mbe.prev = &m->prev;
	dec.mbe.cur = &m->cur;
	dec.mbe.buf = m->buf;
	imbe_dec_init(&dec);
	STORE(mem, m);
}

void p25VoicePoll(void)
{
	if (!streaming && aud != NULL && LOAD(mem) != NULL && LOAD(rx_seq) != 0) {
		streaming = aud->start(p25VoiceAudioBlock, NULL);
	}
}

void p25VoiceEnd(void)
{
	if (streaming && aud != NULL) {
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
	streaming = false;
	STORE(mem, (p25_voice_mem_t *)NULL);
}

bool p25VoiceStreaming(void)
{
	return streaming;
}

void p25VoiceSetMixer(p25_voice_mixer_t m)
{
	STORE(mixer, m);
}

const p25_engine_hooks_t *p25VoiceHooks(void)
{
	return &hooks;
}

void p25VoiceGetStats(p25_voice_stats_t *out)
{
	*out = stats;
}

void p25VoiceResetStats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_P25 && CONFIG_IMBE
