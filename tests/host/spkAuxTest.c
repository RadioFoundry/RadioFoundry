// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker's second output (core/radio/spk_aux.c): the power sequence around a sound, the blocks it moves and the
// silence the stream plays instead, a sound that comes back in the tail, the output going inactive mid-sound, and the
// wrapped stream.
#include <stdio.h>
#include <string.h>
#include "radio/spk_aux.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool act = true;
static uint8_t pw;		// the power last set
static int powers;		// power calls
static int writes;		// blocks written
static int16_t last[AUDIO_BLOCK_SAMPLES];

static bool a_active(void) { return act; }
static void a_power(uint8_t s) { pw = s; powers++; }
static void a_write(const int16_t *pcm) { memcpy(last, pcm, sizeof last); writes++; }

static const hal_spk_aux_t aux = { .active = a_active, .power = a_power, .write = a_write, .lead_ms = 60,
				   .tail_ms = 40, .core = SPK_AUX_CORE };

static int16_t blk[AUDIO_BLOCK_SAMPLES];

static void fill(bool sound)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		blk[i] = sound ? (int16_t)(i % 2 ? 1000 : -1000) : 0;
	}
}

static bool zero(void)
{
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		if (blk[i] != 0) {
			return false;
		}
	}
	return true;
}

static void step(bool sound)
{
	fill(sound);
	spk_aux_block(&aux, blk);
}

static void test_sequence(void)
{
	act = true;
	spk_aux_reset(&aux);
	powers = writes = 0;
	uint32_t n0 = spk_aux_sounds();
	step(false);
	CHECK(powers == 0 && writes == 0 && !spk_aux_pending());	// silence: nothing powered
	step(true);
	CHECK(spk_aux_sounds() == n0 + 1);				// counted once per power-up
	CHECK(pw == SPK_AUX_ARMED && writes == 1 && zero());		// the amplifier up, the block queued, the stream silent
	CHECK(last[1] == 1000 && spk_aux_pending());
	step(true);
	CHECK(pw == SPK_AUX_ARMED);
	step(false);
	CHECK(pw == SPK_AUX_OPEN && writes == 3);			// 60 ms queued: the speaker opens
	for (int i = 0; i < 3; i++) {
		step(false);
		CHECK(pw == SPK_AUX_OPEN);				// the queue (lead + 1, a block early) still plays the sound
	}
	step(false);
	CHECK(pw == SPK_AUX_ARMED && writes == 7);			// only silence left: muted
	step(false);
	CHECK(pw == SPK_AUX_ARMED && writes == 7 && spk_aux_pending());	// the tail: nothing more queued
	step(false);
	CHECK(pw == SPK_AUX_OFF && !spk_aux_pending());		// 40 ms: the amplifier off
}

static void test_back_in_tail(void)
{
	act = true;
	spk_aux_reset(&aux);
	step(true);
	for (int i = 0; i < 6; i++) {
		step(false);
	}
	CHECK(pw == SPK_AUX_ARMED && spk_aux_pending());	// in the tail
	powers = 0;
	step(true);			// a second beep: queued again, the amplifier still up
	CHECK(pw == SPK_AUX_ARMED && powers == 0 && zero());
	step(false);
	step(false);
	CHECK(pw == SPK_AUX_OPEN);
}

static void test_inactive(void)
{
	act = true;
	spk_aux_reset(&aux);
	step(true);
	CHECK(spk_aux_pending());
	act = false;			// the stream's output reaches the speaker again: it plays from here
	step(true);
	CHECK(pw == SPK_AUX_OFF && !spk_aux_pending() && !zero());
	act = true;
}

static audio_block_cb_t hw_cb;
static int hw_stops;
static bool hw_start(audio_block_cb_t cb, void *ctx) { (void)ctx; hw_cb = cb; return true; }
static void hw_stop(void) { hw_stops++; }
static const hal_audio_t *wrapped_stream;
static int user_blocks;
static bool user_stops;		// the user stops the stream from its callback (as the DMR voice does)
static void user(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	CHECK(ctx == &user_blocks);
	user_blocks++;
	spk[0] = 500;
	if (user_stops) {
		wrapped_stream->stop();
	}
}

#ifdef SPK_AUX_MUX
#include "usbaudio/audio_mux.h"
#define WRAP(a)	audio_mux_wrap(a)	// the audio mux runs the second output's step in its own wrapper
#else
#define WRAP(a)	spk_aux_stream(a)
#endif

static void test_wrap(void)
{
	static const hal_audio_t plain = { .start = hw_start, .stop = hw_stop };
	static const hal_audio_t board = { .start = hw_start, .stop = hw_stop, .spk_aux = &aux };
#ifndef SPK_AUX_MUX
	CHECK(spk_aux_stream(&plain) == &plain);	// no second output: the board's stream as it is
	CHECK(spk_aux_stream(NULL) == NULL);
#else
	(void)plain;
#endif
	const hal_audio_t *w = WRAP(&board);
	wrapped_stream = w;
	CHECK(w != &board);
	act = true;
	spk_aux_reset(&aux);
	CHECK(w->start(user, &user_blocks));
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 };
	memset(blk, 0, sizeof blk);
	hw_cb(mic, blk, NULL);
	CHECK(user_blocks == 1 && zero() && last[0] == 500 && pw == SPK_AUX_ARMED);	// the user's sound moved over
	w->stop();
	CHECK(hw_stops == 1 && pw == SPK_AUX_OFF && !spk_aux_pending());
	// A stop from the user's own callback: the block's sound doesn't power the output again
	CHECK(w->start(user, &user_blocks));
	user_stops = true;
	memset(blk, 0, sizeof blk);
	hw_cb(mic, blk, NULL);
	user_stops = false;
	CHECK(hw_stops == 2 && pw == SPK_AUX_OFF && !spk_aux_pending());
}

int main(void)
{
	test_sequence();
	test_back_in_tail();
	test_inactive();
	test_wrap();
	if (failures) {
		printf("spkAuxTest: %d FAILED\n", failures);
		return 1;
	}
	printf("spkAuxTest: PASSED\n");
	return 0;
}
