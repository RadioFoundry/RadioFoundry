// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Microphone gain (Settings > Talk & listen > Mic gain; docs/spec/radio-features.md 3.6): a step 0..10, 2 dB a step,
// 5 = the board's own levels. A board with hal_radio_t.mic_gain gets the step as a dB offset for FM and DMR (the
// modem's microphone gain); otherwise DMR scales the mic samples before the vocoder (ambeVoiceSetMicGain) and FM
// isn't changed.
//
// Threading: any task. Both paths only store a value; the next transmission takes it. RAM only: the UI stores the
// setting (settings_t.mic_gain) and applies it at start-up.
#ifndef RADIO_MIC_GAIN_H
#define RADIO_MIC_GAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"
#include "config.h"

#define MIC_GAIN_STEPS		10u	// steps 0..10
#define MIC_GAIN_UNITY		5u	// the board's levels
#define MIC_GAIN_STEP_DB	2

// The dB offset of a step (clamped to 0..10): -10..+10
static inline int8_t mic_gain_offset_db(uint8_t step)
{
	return (int8_t)(((int)(step > MIC_GAIN_STEPS ? MIC_GAIN_STEPS : step) - (int)MIC_GAIN_UNITY) * MIC_GAIN_STEP_DB);
}

// The board sets the gain in its audio path (FM and DMR)
static inline bool mic_gain_on_board(void)
{
	return board.radio != NULL && board.radio->mic_gain != NULL;
}

// The setting does something in this build on this radio
static inline bool mic_gain_available(void)
{
	return CONFIG_DMR || mic_gain_on_board();
}

void radio_set_mic_gain(uint8_t step);
// The step last set (MIC_GAIN_UNITY until the first call)
uint8_t radio_mic_gain(void);

#endif
