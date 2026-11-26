// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 voice path (core/p25/p25Voice.c) on a made-up PCM stream: the engine's hooks feed frames a LDU at a time,
// the audio callback is driven block by block. Playback waits for a whole LDU and its lead-in, plays every frame in
// order, conceals a late LDU through the decoder and mutes; bad frames, overflow, a call that ends early, the speaker
// route, the mixer and the stream's start and stop.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "p25/p25Voice.h"
#include "dsp/miclimit.h"

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
	ldu(IMBE_ERRS(11, 2, 0));	// frames the decoder does not trust
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
	// No transmission: the transmit hook has nothing (the engine sends its own silence)
	CHECK(!hk->tx_voice(f, NULL), "a tx frame with no transmission on");
	p25VoiceGetStats(&s);
	CHECK(s.tx_frames == 0, "tx frames %u", (unsigned)s.tx_frames);
	// Without memory (P25 not running) the callback is silent and safe
	p25VoiceEnd();
	CHECK(!block(), "sound after the end");
}

// A vowel-like mic block: harmonics of 125 Hz (a 64-sample period), the phase running on from block to block
static int mic_n;
static void mic_block(int16_t *mic, int amp)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++, mic_n++) {
		double v = 0;
		for (int h = 1; h <= 20; h++)
			v += sin(2 * M_PI * h * mic_n / 64.0 + h * h * 0.3) / (1 + 0.2 * h);
		mic[i] = (int16_t)(amp * v);
	}
}

static bool gate_on(void) { return true; }

// One block while transmitting: the speaker must stay silent
static void tx_blocks(int n, int amp)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];

	for (int i = 0; i < n; i++) {
		mic_block(mic, amp);
		memset(spk, 0x55, sizeof spk);
		p25VoiceAudioBlock(mic, spk, NULL);
		for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++)
			if (spk[k] != 0) {
				CHECK(false, "speaker not silent while transmitting");
				return;
			}
	}
}

static void testTransmit(void)
{
	static mbe_params_t pr;
	imbe_state_t st;
	uint8_t f[P25_IMBE_BYTES];
	p25_voice_stats_t s;
	int got = 0, voiced = 0, pitch_ok = 0;

	begin();
	CHECK(!running, "the stream runs before anything happened");
	p25VoiceTx(true);
	CHECK(running && starts == 1, "the transmission did not start the stream");
	p25VoiceTx(true);
	CHECK(starts == 1, "a second start");
	// The engine takes each frame as it is made: all of them arrive, and they say what the mic heard
	imbe_state_reset(&st);
	for (int i = 0; i < 30; i++) {
		tx_blocks(1, 6000);
		while (hk->tx_voice(f, NULL)) {
			got++;
			if (imbe_unpack(&st, f, &pr) == IMBE_FRAME_OK && i >= 4) {
				// (125 Hz: w0 = 2 pi / 64 in the core's units, 32 harmonics by the standard's count)
				pitch_ok += pr.L >= 28 && pr.L <= 30;
				for (int k = 1; k <= 6; k++)
					voiced += pr.v[k] != 0;
			}
		}
	}
	CHECK(got == 30, "frames taken %d of 30", got);
	CHECK(pitch_ok >= 24, "the pitch was found in %d of 26 frames", pitch_ok);
	CHECK(voiced >= 24 * 6, "low harmonics voiced %d of %d", voiced, 26 * 6);
	p25VoiceGetStats(&s);
	CHECK(s.tx_frames == 30 && s.tx_encoded == 30 && s.tx_dropped == 0, "tx counts %u %u %u", (unsigned)s.tx_frames,
	      (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	CHECK(s.tx_mic_peak > 6000 && s.tx_mic_peak < 32768, "mic peak %u", (unsigned)s.tx_mic_peak);
	// The engine not taking them: the queue fills, the rest are dropped, the encoder keeps running
	tx_blocks(P25_VOICE_TX_QUEUE + 3, 6000);
	p25VoiceGetStats(&s);
	CHECK(s.tx_encoded == 30 + P25_VOICE_TX_QUEUE + 3 && s.tx_dropped == 3, "full queue: %u encoded, %u dropped",
	      (unsigned)s.tx_encoded, (unsigned)s.tx_dropped);
	// The end: what is still queued is not sent, in this transmission or the next
	p25VoiceTx(false);
	CHECK(!hk->tx_voice(f, NULL), "a frame after the end");
	p25VoiceTx(true);
	CHECK(!hk->tx_voice(f, NULL), "a frame of the last transmission in a new one");
	// An all-zero mic (the route off) gives silence frames
	{
		int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
		p25VoiceAudioBlock(mic, spk, NULL);
	}
	CHECK(hk->tx_voice(f, NULL) && memcmp(f, imbe_silence, sizeof f) == 0, "a zero block is not a silence frame");
	p25VoiceGetStats(&s);
	CHECK(s.tx_silent == 1, "silent frames %u", (unsigned)s.tx_silent);
	// The mic gate on (a beep on the speaker): the mic's block goes out as a silence frame, and is counted
	p25VoiceSetMicGate(gate_on);
	tx_blocks(1, 6000);
	p25VoiceSetMicGate(NULL);
	CHECK(hk->tx_voice(f, NULL) && memcmp(f, imbe_silence, sizeof f) == 0, "a gated block is not a silence frame");
	p25VoiceGetStats(&s);
	CHECK(s.tx_silent == 2 && s.tx_mic_muted == 1, "gated: silent %u, muted %u", (unsigned)s.tx_silent,
	      (unsigned)s.tx_mic_muted);
	tx_blocks(1, 6000);
	CHECK(hk->tx_voice(f, NULL) && memcmp(f, imbe_silence, sizeof f) != 0, "the gate held after it went off");
	// The mic gain: 6 dB up doubles what the encoder sees
	p25VoiceTx(false);
	p25VoiceTx(true);
	mic_n = 0;
	tx_blocks(2, 1000);
	p25VoiceGetStats(&s);
	uint32_t unity = s.tx_mic_peak;
	p25VoiceTx(false);
	p25VoiceSetMicGainQ8(miclimit_db_q8(6));
	p25VoiceTx(true);
	mic_n = 0;
	tx_blocks(2, 1000);
	p25VoiceGetStats(&s);
	CHECK(unity > 1000 && s.tx_mic_peak >= 2 * unity - 16 && s.tx_mic_peak <= 2 * unity, "mic peak %u, with gain %u",
	      (unsigned)unity, (unsigned)s.tx_mic_peak);
	CHECK(s.tx_mic_low_q8 == miclimit_db_q8(6), "the limiter cut a quiet mic: lowest %u", (unsigned)s.tx_mic_low_q8);
	p25VoiceSetMicGainQ8(MICLIMIT_UNITY);
	p25VoiceTx(false);
	// Reception after it: a call plays as before (the decoder starts over)
	hk->rx_start(NULL, NULL);
	ldu(0);
	for (int i = 0; i < P25_VOICE_LEAD; i++)
		CHECK(!block(), "sound in the lead");
	CHECK(block(), "no sound in a call after a transmission");
	p25VoiceEnd();
	p25VoiceTx(true);	// not running: nothing happens
	CHECK(!running, "a transmission started the stream with P25 off");
}

// One transmission of `blocks` mic blocks of the test voice at amplitude amp and gain q8, played back through this
// module's own receive path (the real decoder): the rms of what the speaker gets once the decoder has settled, and
// the transmission's statistics. The level that comes out of the decoder is what a listener hears, so it shows the
// gain reached the encoder, not just the samples
#define LOOP_BLOCKS	36
#define LOOP_SETTLE	8	// decoded frames skipped: the encoder's and the decoder's predictions start from none
static double loopback_rms(uint16_t q8, int amp, p25_voice_stats_t *tx)
{
	static uint8_t frames[LOOP_BLOCKS][P25_IMBE_BYTES];
	int n = 0, fed = 0;
	double sum = 0;
	long count = 0;
	p25_voice_stats_t s;

	begin();
	p25VoiceSetMicGainQ8(q8);
	p25VoiceTx(true);
	mic_n = 0;
	for (int i = 0; i < LOOP_BLOCKS; i++) {
		tx_blocks(1, amp);
		while (n < LOOP_BLOCKS && hk->tx_voice(frames[n], NULL))
			n++;
	}
	p25VoiceGetStats(tx);
	p25VoiceTx(false);
	CHECK(n == LOOP_BLOCKS, "frames taken %d of %d", n, LOOP_BLOCKS);

	// Two LDUs' worth queued, then one more a block: playback primes on one LDU, so the queue never overflows
	hk->rx_start(NULL, NULL);
	for (; fed < 2 * P25_VOICE_FRAMES && fed < n; fed++)
		hk->rx_voice(frames[fed], 0, NULL);
	for (int b = 0; b < 4 * LOOP_BLOCKS; b++) {
		int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 }, spk[AUDIO_BLOCK_SAMPLES];
		if (fed < n)
			hk->rx_voice(frames[fed++], 0, NULL);
		p25VoiceGetStats(&s);
		uint32_t before = s.rx_frames;
		p25VoiceAudioBlock(mic, spk, NULL);
		p25VoiceGetStats(&s);
		if (s.rx_frames != before && before >= LOOP_SETTLE)
			for (int k = 0; k < AUDIO_BLOCK_SAMPLES; k++, count++)
				sum += (double)spk[k] * spk[k];
		if (s.rx_frames == (uint32_t)n)
			break;
	}
	hk->rx_end(NULL);
	CHECK(s.rx_frames == (uint32_t)n, "frames played %u of %d", (unsigned)s.rx_frames, n);
	p25VoiceSetMicGainQ8(MICLIMIT_UNITY);
	p25VoiceEnd();
	return count ? sqrt(sum / count) : 0;
}

// The mic gain and its limiter on P25 (as DMR's, core/dsp/miclimit.h): a board whose mic reaches the PCM quiet gets
// its hal_radio_t.mic_pcm_db here too. A quiet voice lifted 12 dB comes out of the decoder about 12 dB louder; a loud
// one at the same gain is held at the ceiling, with no sample past it, and comes out about as loud as at unity
static void testMicGainLimiter(void)
{
	p25_voice_stats_t s;
	uint16_t g12 = miclimit_db_q8(12);

	double quiet = loopback_rms(MICLIMIT_UNITY, 500, &s);
	uint32_t quiet_peak = s.tx_mic_peak;
	CHECK(quiet > 50, "the quiet voice decoded silent: rms %.1f", quiet);
	CHECK(s.tx_mic_low_q8 == MICLIMIT_UNITY, "unity: lowest gain %u", (unsigned)s.tx_mic_low_q8);

	double lifted = loopback_rms(g12, 500, &s);
	double db = 20 * log10(lifted / quiet);
	CHECK(db > 11 && db < 13, "12 dB of gain came out as %.1f dB (rms %.1f against %.1f)", db, lifted, quiet);
	CHECK(s.tx_mic_low_q8 == g12, "the limiter cut a quiet voice: lowest %u of %u", (unsigned)s.tx_mic_low_q8, g12);
	CHECK(s.tx_mic_peak > 3 * quiet_peak && s.tx_mic_peak <= MICLIMIT_CEILING, "lifted peak %u, unity %u",
	      (unsigned)s.tx_mic_peak, (unsigned)quiet_peak);

	// Loud: at unity its peaks are near full scale, so +12 dB would clip
	double loud = loopback_rms(MICLIMIT_UNITY, 7000, &s);
	uint32_t loud_peak = s.tx_mic_peak;
	CHECK(loud_peak > MICLIMIT_CEILING / 2, "the loud voice is not loud: peak %u", (unsigned)loud_peak);
	double held = loopback_rms(g12, 7000, &s);
	CHECK(s.tx_mic_peak <= MICLIMIT_CEILING && s.tx_mic_peak > MICLIMIT_CEILING - 2000, "loud at +12 dB: peak %u",
	      (unsigned)s.tx_mic_peak);
	CHECK(s.tx_mic_low_q8 < g12 && miclimit_q8_db((uint16_t)s.tx_mic_low_q8) <= 2, "the limiter's lowest gain %d dB",
	      miclimit_q8_db((uint16_t)s.tx_mic_low_q8));
	db = 20 * log10(held / loud);
	// Held to the 1-2 dB its peak allows, not the 12 asked for, and not pumped down below unity
	CHECK(db > -0.5 && db < 3, "the held voice came out %.1f dB over unity", db);
}

// A beep's own stream left the speaker route on and the transmission took the running stream over (#705): the
// first block gives the route back
static void testRouteTakeover(void)
{
	begin();
	spk_on = true;		// the beep's callback routed the speaker
	p25VoiceTx(true);
	block();
	CHECK(!spk_on, "the beep's route left on after the takeover");
	p25VoiceTx(false);
	p25VoiceEnd();
}

int main(void)
{
	p25VoiceInit(&audio);
	hk = p25VoiceHooks();
	testPlayback();
	testBadAndOverflow();
	testShortCallAndNewCall();
	testMixerAndTx();
	testTransmit();
	testMicGainLimiter();
	testRouteTakeover();
	printf("p25VoiceTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
