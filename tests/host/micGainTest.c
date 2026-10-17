// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The mic gain setting (core/radio/mic_gain.c, docs/spec/radio-features.md 3.6): a step 0..10 goes to the board's
// hal_radio_t.mic_gain as a dB offset with DMR's sample scaling left at unity, or, without the hook, to the sample
// scaling alone
#include <stdio.h>
#include "radio/mic_gain.h"
#include "dmr/ambe/ambeVoice.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static hal_radio_t radio;
const board_t board = { .radio = &radio };

static int hook_calls, ambe_calls;
static int8_t hook_db;
static uint8_t ambe_step = 0xFF;

static void hook(int8_t db)
{
	hook_calls++;
	hook_db = db;
}

void ambeVoiceSetMicGain(uint8_t step)
{
	ambe_calls++;
	ambe_step = step;
}

int main(void)
{
	CHECK(radio_mic_gain() == MIC_GAIN_UNITY);
	CHECK(mic_gain_offset_db(0) == -10 && mic_gain_offset_db(5) == 0 && mic_gain_offset_db(10) == 10);
	CHECK(mic_gain_offset_db(7) == 4 && mic_gain_offset_db(200) == 10);

	// No board hook: DMR scales the samples by the step
	CHECK(!mic_gain_on_board() && mic_gain_available());
	radio_set_mic_gain(8);
	CHECK(ambe_step == 8 && hook_calls == 0 && radio_mic_gain() == 8);

	// The board's gain: the offset to the board, the samples at unity (not the step twice)
	radio.mic_gain = hook;
	CHECK(mic_gain_on_board());
	radio_set_mic_gain(8);
	CHECK(hook_calls == 1 && hook_db == 6 && ambe_step == MIC_GAIN_UNITY);
	radio_set_mic_gain(0);
	CHECK(hook_calls == 2 && hook_db == -10 && ambe_step == MIC_GAIN_UNITY && radio_mic_gain() == 0);
	radio_set_mic_gain(42);		// clamped
	CHECK(hook_calls == 3 && hook_db == 10 && radio_mic_gain() == MIC_GAIN_STEPS);
	CHECK(ambe_calls == 4);

	if (failures) {
		printf("micGainTest: %d failures\n", failures);
		return 1;
	}
	printf("micGainTest: all passed\n");
	return 0;
}
