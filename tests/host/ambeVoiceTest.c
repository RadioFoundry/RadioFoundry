// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DMR voice glue (core/dmr/ambe/ambeVoice.c) against a fake audio driver and a fake vocoder
#include <stdio.h>
#include <string.h>

#include "dmr/ambe/ambeVoice.h"
#include "dmr/ambe/ambeFec.h"
#include "vocoder/ambe2_flags.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- fakes ---
static audio_block_cb_t cb;
static int starts, stops, speaker_on, speaker_calls;
static bool fake_start(audio_block_cb_t c, void *ctx) { (void)ctx; cb = c; starts++; return true; }
static void fake_stop(void) { stops++; cb = NULL; }
static void fake_speaker(bool on) { speaker_on = on; speaker_calls++; }
static const hal_audio_t audio = { fake_start, fake_stop, fake_speaker };

// Vocoder: encode copies the low bit of the first 49 samples; decode writes the bits into samples 0..48 and a frame
// marker into sample 159
static int resets, decodes;
static bool fake_present(void) { return true; }
static void fake_reset(void) { resets++; }
static void fake_encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	for (int i = 0; i < VOCODER_BITS; i++)
		bits[i] = (uint8_t)(pcm[i] & 1);
}
static void fake_decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	decodes++;
	memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
	for (int i = 0; i < VOCODER_BITS; i++)
		pcm[i] = bits[i];
	pcm[159] = 1000;
}
static const hal_vocoder_t vocoder = { fake_present, fake_reset, fake_encode, fake_decode, NULL, NULL, NULL, NULL };

// A vocoder with encode options, built as the AMBE+2 hosts are: set_options keeps the flags word, and each frame is
// two codec calls (two halves) that both pass it. Every call's flags are recorded
static uint32_t opt_flags = 0xDEADBEEFu;	// "never set" until set_options
static uint32_t opt_sets, opt_last_set;
static uint32_t calls_flags[64];
static int ncalls;
static uint32_t opt_options(void) { return AMBE2_OPTIONS; }
static void opt_set(uint32_t opts) { opt_sets++; opt_last_set = opts; opt_flags = ambe2_enc_flags(opts); }
static uint32_t opt_encode_flags(void) { return opt_flags; }
static void opt_encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	const uint32_t flags = opt_flags;
	for (int h = 0; h < 2; h++)
		if (ncalls < 64)
			calls_flags[ncalls++] = flags;
	fake_encode(pcm, bits);
}
static const hal_vocoder_t opt_vocoder = {
	.present = fake_present, .reset = fake_reset, .encode = opt_encode, .decode = fake_decode,
	.options = opt_options, .set_options = opt_set, .encode_flags = opt_encode_flags,
};

static void makeFrame(int n, uint8_t bits[49], uint8_t frame[9])
{
	for (int i = 0; i < 49; i++)
		bits[i] = (uint8_t)(((n * 7 + i * 3) >> 2) & 1);
	bits[0] = (uint8_t)(n & 1);
	bits[1] = (uint8_t)((n >> 1) & 1);
	bits[2] = (uint8_t)((n >> 2) & 1);
	ambeFecEncode(bits, frame);
}

static int16_t mic[160], spk[160];

static void block(void)
{
	memset(spk, 0x55, sizeof spk);
	cb(mic, spk, NULL);
}

static bool spkIs(const uint8_t bits[49])
{
	if (spk[159] != 1000)
		return false;
	for (int i = 0; i < 49; i++)
		if (spk[i] != bits[i])
			return false;
	return true;
}

static bool spkSilent(void)
{
	for (int i = 0; i < 160; i++)
		if (spk[i] != 0)
			return false;
	return true;
}

static void testRx(void)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();
	uint8_t bits[8][49], frame[9];

	s->begin(NULL, s->ctx);
	CHECK(starts == 1 && cb != NULL, "audio started on the first begin");
	block();
	CHECK(spkSilent() && resets == 1 && !speaker_on, "new call: reset, silent while priming");

	for (int n = 0; n < 2; n++) {
		makeFrame(n, bits[n], frame);
		s->frame(frame, s->ctx);
	}
	block();
	CHECK(spkSilent() && !speaker_on, "2 frames: still priming");
	makeFrame(2, bits[2], frame);
	frame[0] ^= 0x80;	// one corrected error
	s->frame(frame, s->ctx);
	block();
	CHECK(speaker_on && spkIs(bits[0]), "primed: speaker on, frame 0 plays");
	block();
	CHECK(spkIs(bits[1]), "frame 1");
	block();
	CHECK(spkIs(bits[2]), "frame 2 (with a corrected bit error)");

	// Underrun: repeat the last good frame up to 3 times, then silence
	block();
	CHECK(spkIs(bits[2]), "underrun 1 repeats");
	block();
	block();
	CHECK(spkIs(bits[2]), "underrun 3 repeats");
	block();
	CHECK(spkSilent(), "underrun 4 silent");

	// A frame with 4 errors in C0 and a lost frame are concealed
	makeFrame(3, bits[3], frame);
	s->frame(frame, s->ctx);
	makeFrame(4, bits[4], frame);
	for (int k = 0; k < 4; k++) {
		uint8_t a = ambeAirBitOfSeq((uint8_t)k);
		frame[a >> 3] ^= (uint8_t)(0x80 >> (a & 7));
	}
	s->frame(frame, s->ctx);
	s->lost(s->ctx);
	makeFrame(5, bits[5], frame);
	s->frame(frame, s->ctx);
	block();
	CHECK(spkIs(bits[3]), "frame 3");
	block();
	CHECK(spkIs(bits[3]), "bad frame 4 concealed with 3");
	block();
	CHECK(spkIs(bits[3]), "lost frame concealed with 3");
	block();
	CHECK(spkIs(bits[5]), "frame 5");

	ambe_voice_stats_t st;
	ambeVoiceGetStats(&st);
	CHECK(st.rx_underrun_at == 3, "underruns after the call's 3rd frame: %u", st.rx_underrun_at);
	CHECK(st.rx_peak == 1000, "decoded peak: the fake decoder's frame marker: %u", st.rx_peak);
	CHECK(st.rx_bad == 1 && st.rx_lost == 1 && st.rx_frames == 5 && st.rx_bit_errors == 1 && st.rx_underruns == 4,
	      "stats bad %u lost %u frames %u biterr %u underruns %u", st.rx_bad, st.rx_lost, st.rx_frames,
	      st.rx_bit_errors, st.rx_underruns);
	// Frames 0-2 fill a burst, frame 3 starts the next: the bad frame 4 is a burst's second
	CHECK(st.rx_bad_at[0] == 0 && st.rx_bad_at[1] == 1 && st.rx_bad_at[2] == 0, "bad by place %u,%u,%u",
	      st.rx_bad_at[0], st.rx_bad_at[1], st.rx_bad_at[2]);

	// End: drain what's queued, then speaker off
	makeFrame(6, bits[6], frame);
	s->frame(frame, s->ctx);
	s->end(s->ctx);
	block();
	CHECK(spkIs(bits[6]) && speaker_on, "drains after end");
	block();
	CHECK(spkSilent() && !speaker_on, "speaker off once drained");

	// Frames left from an old call are dropped when a new call starts
	int r0 = resets;
	makeFrame(7, bits[7], frame);
	s->begin(NULL, s->ctx);
	s->frame(frame, s->ctx);
	s->end(s->ctx);
	block();
	CHECK(resets == r0 + 1 && spkIs(bits[7]), "short call plays once ended, with a reset");
	block();
	CHECK(!speaker_on, "and ends");
}

static void testTx(void)
{
	const dmr_voice_source_t *src = ambeVoiceSource();
	uint8_t frame[9], bits[49];
	int r0 = resets;
	CHECK(!src->next(frame, src->ctx), "nothing before begin");
	block();
	CHECK(!src->next(frame, src->ctx), "no encoding while idle");

	src->begin(src->ctx);
	for (int n = 0; n < 3; n++) {
		for (int i = 0; i < 160; i++)
			mic[i] = (int16_t)((i * 5 + n) & 1 ? 301 : 300);
		block();
	}
	CHECK(resets == r0 + 1, "encoder reset at the start of a transmission");
	for (int n = 0; n < 3; n++) {
		CHECK(src->next(frame, src->ctx), "frame %d ready", n);
		int e = ambeFecDecode(frame, bits, NULL);
		int ok = e == 0;
		for (int i = 0; i < 49; i++)
			if (bits[i] != (((i * 5 + n) & 1) ? 1 : 0))
				ok = 0;
		CHECK(ok, "TX frame %d carries the encoded mic block", n);
	}
	CHECK(!src->next(frame, src->ctx), "queue empty");

	// Overflow when the stack doesn't pull
	for (int n = 0; n < AMBE_VOICE_QUEUE + 2; n++)
		block();
	ambe_voice_stats_t st;
	ambeVoiceGetStats(&st);
	CHECK(st.tx_overflows == 2, "tx overflows %u", st.tx_overflows);
	src->end(src->ctx);
	block();
	ambeVoiceGetStats(&st);
	uint32_t f = st.tx_frames;
	block();
	ambeVoiceGetStats(&st);
	CHECK(st.tx_frames == f, "no encoding after end");

	// Stale frames of the previous transmission are dropped at the next one
	src->begin(src->ctx);
	block();
	int got = 0;
	while (src->next(frame, src->ctx))
		got++;
	CHECK(got == 1, "only the new transmission's frame (%d)", got);
	src->end(src->ctx);
}

// The option -> flags word mapping of the AMBE+2 hosts (drivers/vocoder/ambe2_flags.h)
static void testFlagsMap(void)
{
	CHECK(ambe2_enc_flags(0) == 0x0800u, "no options: noise gate only (%#x)", (unsigned)ambe2_enc_flags(0));
	CHECK(ambe2_enc_flags(VOCODER_OPT_NOISE_REDUCTION) == 0x0840u, "noise reduction");
	CHECK(ambe2_enc_flags(VOCODER_OPT_TONE_FRAMES) == 0x1800u, "tone frames");
	CHECK(ambe2_enc_flags(VOCODER_OPT_DEFAULT) == 0x1840u, "the defaults are the full pattern");
	CHECK(AMBE2_ENC_DEFAULT == ambe2_enc_flags(VOCODER_OPT_DEFAULT), "the hosts' initial word");
	CHECK(ambe2_enc_flags(VOCODER_OPT_AGC) == 0x2800u, "AGC");
	CHECK(ambe2_enc_flags(VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC) == 0x3840u, "all three");
	// Nothing ever sets the DC filter off or test mode bits, whatever is asked for
	CHECK((ambe2_enc_flags(0xFFFFFFFFu) & 0x4100u) == 0, "never 0x100, 0x4000");
	CHECK(ambe2_enc_flags(0xFFFFFFFFu) == 0x3840u, "all options");
	CHECK(AMBE2_OPTIONS == (VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES | VOCODER_OPT_AGC), "offered");
}

// core -> hal: the options reach the vocoder before the first frame and between frames, and every codec call of a
// transmission passes the same flags (also the first call after the reset at the start)
static void testEncodeOptions(void)
{
	const dmr_voice_source_t *src = ambeVoiceSource();
	uint8_t frame[AMBE_FRAME_BYTES];

	// The vocoder without options: nothing in force, nothing supported
	CHECK(ambeVoiceEncodeOptionsSupported() == 0 && ambeVoiceEncodeOptions() == 0, "no options without hooks");

	ambeVoiceInit(&audio, &opt_vocoder);
	CHECK(ambeVoiceEncodeOptionsSupported() == AMBE2_OPTIONS, "supported");
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT, "defaults in force (%#x)", (unsigned)ambeVoiceEncodeOptions());
	opt_sets = 0;
	ncalls = 0;
	int r0 = resets;
	src->begin(src->ctx);
	for (int i = 0; i < 5; i++)
		block();
	CHECK(resets == r0 + 1, "reset at the start of the transmission");
	CHECK(opt_sets == 1 && opt_last_set == VOCODER_OPT_DEFAULT, "options given once (%u)", (unsigned)opt_sets);
	CHECK(ncalls == 10, "two calls per frame (%d)", ncalls);
	for (int i = 0; i < ncalls; i++)
		CHECK(calls_flags[i] == 0x1840u, "call %d flags %#x", i, (unsigned)calls_flags[i]);
	CHECK(opt_encode_flags() == 0x1840u, "the debug word");

	// Noise reduction off mid-transmission: from the next frame on, both halves
	ambeVoiceSetEncodeOptions(VOCODER_OPT_TONE_FRAMES);
	CHECK(opt_sets == 1, "not applied outside the audio callback");
	ncalls = 0;
	block();
	block();
	CHECK(opt_sets == 2 && opt_last_set == VOCODER_OPT_TONE_FRAMES, "applied before the next frame");
	CHECK(ncalls == 4, "calls %d", ncalls);
	for (int i = 0; i < ncalls; i++)
		CHECK(calls_flags[i] == 0x1800u, "call %d flags %#x", i, (unsigned)calls_flags[i]);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_TONE_FRAMES, "in force");
	src->end(src->ctx);
	while (src->next(frame, src->ctx))
		;

	// A new transmission (vocoder reset) keeps them, and the same value isn't given again
	ncalls = 0;
	src->begin(src->ctx);
	block();
	CHECK(opt_sets == 2 && ncalls == 2 && calls_flags[0] == 0x1800u && calls_flags[1] == 0x1800u,
	      "kept across the reset (%u, %#x)", (unsigned)opt_sets, (unsigned)calls_flags[0]);
	src->end(src->ctx);
	while (src->next(frame, src->ctx))
		;

	// AGC on: in force, both halves
	ambeVoiceSetEncodeOptions(VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC);
	CHECK(ambeVoiceEncodeOptions() == (VOCODER_OPT_DEFAULT | VOCODER_OPT_AGC), "AGC in force");
	ncalls = 0;
	src->begin(src->ctx);
	block();
	CHECK(ncalls == 2 && calls_flags[0] == 0x3840u && calls_flags[1] == 0x3840u, "AGC flags (%#x)",
	      (unsigned)calls_flags[0]);
	src->end(src->ctx);
	while (src->next(frame, src->ctx))
		;
	// An option the vocoder doesn't take is not in force
	ambeVoiceSetEncodeOptions(VOCODER_OPT_DEFAULT | 0x80u);
	CHECK(ambeVoiceEncodeOptions() == VOCODER_OPT_DEFAULT, "unknown option not in force");
	ncalls = 0;
	src->begin(src->ctx);
	block();
	CHECK(ncalls == 2 && calls_flags[0] == 0x1840u, "unknown option dropped (%#x)", (unsigned)calls_flags[0]);
	src->end(src->ctx);
	while (src->next(frame, src->ctx))
		;
	ambeVoiceSetEncodeOptions(VOCODER_OPT_DEFAULT);
	ambeVoiceInit(&audio, &vocoder);
}

int main(void)
{
	ambeVoiceInit(&audio, &vocoder);
	testRx();
	testTx();
	testFlagsMap();
	testEncodeOptions();
	ambeVoiceStop();
	CHECK(stops == 1 && !speaker_on, "stop");
	ambeVoiceSink()->begin(NULL, NULL);
	CHECK(starts == 2, "restarted after stop");
	printf("# ambeVoiceTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
