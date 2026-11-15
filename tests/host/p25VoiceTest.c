// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 voice path (core/p25/p25Voice.c) on a made-up PCM stream: the engine's hooks feed frames a LDU at a time,
// the audio callback is driven block by block. Playback waits for a whole LDU and its lead-in, plays every frame in
// order, conceals a late LDU through the decoder and mutes; bad frames, overflow, a call that ends early, the speaker
// route, the mixer and the stream's start and stop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25/p25Voice.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── The board's PCM stream ──
static bool running, spk_on;
static int starts, stops;
static bool fake_start(audio_block_cb_t cb, void *ctx)
{
	(void)cb;
	(void)ctx;
	running = true;
	starts++;
	return true;
}
static void fake_stop(void) { running = false; stops++; }
static void fake_speaker(bool on) { spk_on = on; }
static const hal_audio_t audio = { .start = fake_start, .stop = fake_stop, .speaker_pcm = fake_speaker };

static p25_voice_mem_t vmem;
static const p25_engine_hooks_t *hk;

// A voiced frame: pitch code 60, mid gain, every band voiced, the rest of the bits varied by n
static void frame(uint8_t f[P25_IMBE_BYTES], int n)
{
	for (int i = 0; i < P25_IMBE_BYTES; i++)
		f[i] = (uint8_t)(0x35 * (i + 1) + 7 * n);
	f[0] = 60;
}

static void ldu(uint8_t errs)
{
	uint8_t f[P25_IMBE_BYTES];
	for (int i = 0; i < P25_VOICE_FRAMES; i++) {
		frame(f, i);
		hk->rx_voice(f, errs, NULL);
	}
}

// One audio block; true when it is not silent
static bool block(void)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
	memset(spk, 0x55, sizeof spk);
	p25VoiceAudioBlock(mic, spk, NULL);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		if (spk[i] != 0)
			return true;
	return false;
}

static void begin(void)
{
	p25VoiceEnd();
	p25VoiceResetStats();
	running = spk_on = false;
	starts = stops = 0;
	p25VoiceBegin(&vmem);
}

static void testPlayback(void)
{
	p25_voice_stats_t s;
	bool heard = false;

	begin();
	p25VoicePoll();
	CHECK(!running, "the stream started before any call");
	CHECK(!block(), "sound before any call");
	hk->rx_start(NULL, NULL);
	p25VoicePoll();
	CHECK(running && starts == 1, "the stream did not start with the call");
	// Eight frames are not a LDU: nothing plays yet
	uint8_t f[P25_IMBE_BYTES];
	for (int i = 0; i < P25_VOICE_FRAMES - 1; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	CHECK(!block() && !spk_on, "played before a whole LDU was in");
	frame(f, 8);
	hk->rx_voice(f, 0, NULL);
	// The lead-in: the speaker route on, silence
	for (int i = 0; i < P25_VOICE_LEAD; i++)
		CHECK(!block(), "lead-in block %d not silent", i);
	CHECK(spk_on, "the speaker route is off while the call plays");
	for (int i = 0; i < P25_VOICE_FRAMES; i++)
		heard |= block();
	p25VoiceGetStats(&s);
	CHECK(heard, "a voiced LDU played silent");
	CHECK(s.rx_frames == 9 && s.rx_in == 9 && s.rx_calls == 1, "frames %u in %u calls %u", (unsigned)s.rx_frames,
	      (unsigned)s.rx_in, (unsigned)s.rx_calls);
	CHECK(s.rx_queue_max == 9, "queue max %u", (unsigned)s.rx_queue_max);
	CHECK(s.rx_peak > 0, "no peak");
	// The next LDU is late: the decoder conceals, then mutes
	for (int i = 0; i < MBE_REPEAT_MAX + 2; i++)
		block();
	p25VoiceGetStats(&s);
	CHECK(s.rx_underruns == MBE_REPEAT_MAX + 2 && s.rx_concealed == MBE_REPEAT_MAX + 2,
	      "underruns %u concealed %u", (unsigned)s.rx_underruns, (unsigned)s.rx_concealed);
	CHECK(s.rx_underrun_at == 9, "underrun at %u", (unsigned)s.rx_underrun_at);
	CHECK(!block(), "concealment did not mute");
	// The LDU arrives after all: it plays at once (the call is primed), then the end turns the route off
	ldu(0);
	hk->rx_end(NULL);
	heard = false;
	for (int i = 0; i < P25_VOICE_FRAMES; i++)
		heard |= block();
	CHECK(heard, "the late LDU did not play");
	CHECK(spk_on, "route off before the queue ran dry");
	CHECK(!block() && !spk_on, "the route stayed on after the call");
	p25VoiceGetStats(&s);
	CHECK(s.rx_frames == 18, "frames %u", (unsigned)s.rx_frames);
	p25VoiceEnd();
	CHECK(!running && stops == 1 && !spk_on, "end did not stop the stream");
}

static void testBadAndOverflow(void)
{
	p25_voice_stats_t s;

	begin();
	hk->rx_start(NULL, NULL);
	ldu(IMBE_ERRS_MAX + 1);	// FEC gave up on every frame
	for (int i = 0; i < P25_VOICE_LEAD + P25_VOICE_FRAMES; i++)
		CHECK(!block(), "a bad frame with nothing to repeat sounded");
	p25VoiceGetStats(&s);
	CHECK(s.rx_bad == 9 && s.rx_concealed == 9 && s.rx_frames == 0, "bad %u concealed %u frames %u",
	      (unsigned)s.rx_bad, (unsigned)s.rx_concealed, (unsigned)s.rx_frames);
	// Corrected bits are counted on good frames
	ldu(2);
	p25VoiceGetStats(&s);
	CHECK(s.rx_bit_errors == 18, "bit errors %u", (unsigned)s.rx_bit_errors);
	// More than the queue holds: the rest is dropped and counted
	for (int i = 0; i < 4; i++)
		ldu(0);
	p25VoiceGetStats(&s);
	CHECK(s.rx_queue_max == P25_VOICE_QUEUE, "queue max %u", (unsigned)s.rx_queue_max);
	CHECK(s.rx_overflows == 5 * 9 - P25_VOICE_QUEUE, "overflows %u", (unsigned)s.rx_overflows);
}

static void testShortCallAndNewCall(void)
{
	p25_voice_stats_t s;
	uint8_t f[P25_IMBE_BYTES];
	bool heard = false;

	begin();
	// A call of three frames that ends: they play without waiting for a LDU
	hk->rx_start(NULL, NULL);
	for (int i = 0; i < 3; i++) {
		frame(f, i);
		hk->rx_voice(f, 0, NULL);
	}
	hk->rx_end(NULL);
	for (int i = 0; i < P25_VOICE_LEAD + 3; i++)
		heard |= block();
	CHECK(heard, "a short call was not played");
	CHECK(!block() && !spk_on, "the short call did not end");
	// A call that ends before any frame: nothing
	hk->rx_start(NULL, NULL);
	hk->rx_end(NULL);
	CHECK(!block() && !spk_on, "an empty call sounded");
	// Frames left of a call that was cut off are dropped when the next starts
	hk->rx_start(NULL, NULL);
	ldu(0);
	block();
	hk->rx_start(NULL, NULL);	// (the engine starts a new call without an end: a squelch decision)
	for (int i = 0; i < 3; i++)
		CHECK(!block(), "the cut call's frames played in the next");
	p25VoiceGetStats(&s);
	CHECK(s.rx_calls == 2, "calls %u", (unsigned)s.rx_calls);
	// Frames with no call open start one
	hk->rx_end(NULL);
	ldu(0);
	heard = false;
	for (int i = 0; i < P25_VOICE_LEAD + 9; i++)
		heard |= block();
	CHECK(heard, "frames without a start did not play");
}

static bool mixing;
static bool mix(int16_t *spk)
{
	if (mixing)
		spk[0] = 1000;
	return mixing;
}

static void testMixerAndTx(void)
{
	uint8_t f[P25_IMBE_BYTES];
	p25_voice_stats_t s;

	begin();
	p25VoiceSetMixer(mix);
	mixing = true;
	CHECK(block() && spk_on, "the mixer's sound is not routed");
	mixing = false;
	CHECK(!block() && !spk_on, "the route stayed on after the mixer");
	p25VoiceSetMixer(NULL);
	// No encoder: the transmit hook gives silence frames
	CHECK(hk->tx_voice(f, NULL) && memcmp(f, imbe_silence, sizeof f) == 0, "tx frame is not silence");
	p25VoiceGetStats(&s);
	CHECK(s.tx_frames == 1, "tx frames %u", (unsigned)s.tx_frames);
	// Without memory (P25 not running) the callback is silent and safe
	p25VoiceEnd();
	CHECK(!block(), "sound after the end");
}

int main(void)
{
	p25VoiceInit(&audio);
	hk = p25VoiceHooks();
	testPlayback();
	testBadAndOverflow();
	testShortCallAndNewCall();
	testMixerAndTx();
	printf("p25VoiceTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
