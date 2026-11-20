// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mic gain setting (core/radio/mic_gain.c, docs/spec/radio-features.md 3.6): a step 0..10 goes to the board's
// hal_radio_t.mic_gain as a dB offset with the PCM gain before the encoders left at unity, or, without the hook, to the
// PCM gain alone; a board's hal_radio_t.mic_pcm_db adds to the PCM gain at every step
#include <stdio.h>
#include "radio/mic_gain.h"
#include "dmr/ambe/ambeVoice.h"
#include "audio/miclimit/miclimit.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static hal_radio_t radio;
const board_t board = { .radio = &radio };

static int hook_calls, ambe_calls;
static int8_t hook_db;
static uint16_t ambe_q8;

static void hook(int8_t db)
{
	hook_calls++;
	hook_db = db;
}

void ambeVoiceSetMicGainQ8(uint16_t q8)
{
	ambe_calls++;
	ambe_q8 = q8;
}

int main(void)
{
	CHECK(radio_mic_gain() == MIC_GAIN_UNITY);
	CHECK(mic_gain_offset_db(0) == -10 && mic_gain_offset_db(5) == 0 && mic_gain_offset_db(10) == 10);
	CHECK(mic_gain_offset_db(7) == 4 && mic_gain_offset_db(200) == 10);

	// No board hook: the step scales the PCM before the encoders (DMR pushed, Fusion DN reads it)
	CHECK(!mic_gain_on_board() && mic_gain_available());
	CHECK(radio_mic_pcm_db() == 0 && radio_mic_pcm_q8() == MICLIMIT_UNITY);
	radio_set_mic_gain(8);
	CHECK(ambe_q8 == miclimit_db_q8(6) && hook_calls == 0 && radio_mic_gain() == 8);
	CHECK(radio_mic_pcm_db() == 6 && radio_mic_pcm_q8() == ambe_q8);
	radio_set_mic_gain(0);
	CHECK(ambe_q8 == miclimit_db_q8(-10) && radio_mic_pcm_db() == -10);

	// A quiet board (12 dB, as one radio has): its gain at the middle step, the step around it
	radio.mic_pcm_db = 12;
	radio_set_mic_gain(MIC_GAIN_UNITY);
	CHECK(radio_mic_pcm_db() == 12 && ambe_q8 == miclimit_db_q8(12) && ambe_q8 > 1000);
	radio_set_mic_gain(MIC_GAIN_STEPS);
	CHECK(radio_mic_pcm_db() == 22 && ambe_q8 == miclimit_db_q8(22));
	radio_set_mic_gain(0);
	CHECK(radio_mic_pcm_db() == 2 && ambe_q8 == miclimit_db_q8(2));
	radio.mic_pcm_db = 0;

	// The board's gain: the offset to the board, the samples at unity (not the step twice)
	radio.mic_gain = hook;
	CHECK(mic_gain_on_board());
	radio_set_mic_gain(8);
	CHECK(hook_calls == 1 && hook_db == 6 && ambe_q8 == MICLIMIT_UNITY && radio_mic_pcm_db() == 0);
	radio_set_mic_gain(0);
	CHECK(hook_calls == 2 && hook_db == -10 && ambe_q8 == MICLIMIT_UNITY && radio_mic_gain() == 0);
	radio_set_mic_gain(42);		// clamped
	CHECK(hook_calls == 3 && hook_db == 10 && radio_mic_gain() == MIC_GAIN_STEPS);
	// The board's hook and a PCM gain together: the step in the board's path, only the board's dB on the PCM
	radio.mic_pcm_db = 4;
	radio_set_mic_gain(8);
	CHECK(hook_db == 6 && radio_mic_pcm_db() == 4 && ambe_q8 == miclimit_db_q8(4));
	CHECK(ambe_calls == 9);

	if (failures) {
		printf("micGainTest: %d failures\n", failures);
		return 1;
	}
	printf("micGainTest: all passed\n");
	return 0;
}
