// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Microphone gain (mic_gain.h; docs/spec/radio-features.md 3.6)
#include "radio/mic_gain.h"
#if CONFIG_DMR
#include "dmr/ambe/ambeVoice.h"
#endif

static volatile uint8_t mic_step = MIC_GAIN_UNITY;

void radio_set_mic_gain(uint8_t step)
{
	if (step > MIC_GAIN_STEPS) {
		step = MIC_GAIN_STEPS;
	}
	mic_step = step;
	bool hw = mic_gain_on_board();
	if (hw) {
		board.radio->mic_gain(mic_gain_offset_db(step));
	}
#if CONFIG_DMR
	// With the board's gain the samples stay as they are: scaling them too would count the step twice
	ambeVoiceSetMicGain(hw ? (uint8_t)MIC_GAIN_UNITY : step);
#endif
}

uint8_t radio_mic_gain(void)
{
	return mic_step;
}
