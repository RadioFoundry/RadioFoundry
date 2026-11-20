// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Microphone gain (mic_gain.h; docs/spec/radio-features.md 3.6)
#include "radio/mic_gain.h"
#include "audio/miclimit/miclimit.h"
#if CONFIG_DMR
#include "dmr/ambe/ambeVoice.h"
#endif
#if CONFIG_P25 && CONFIG_IMBE
#include "p25/p25Voice.h"
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
	ambeVoiceSetMicGainQ8(radio_mic_pcm_q8());
#endif
#if CONFIG_P25 && CONFIG_IMBE
	p25VoiceSetMicGain(hw ? (uint8_t)MIC_GAIN_UNITY : step);
#endif
}

uint8_t radio_mic_gain(void)
{
	return mic_step;
}

int radio_mic_pcm_db(void)
{
	// With the board's gain the step isn't in the samples: scaling them too would count it twice
	return mic_gain_board_pcm_db() + (mic_gain_on_board() ? 0 : mic_gain_offset_db(mic_step));
}

uint16_t radio_mic_pcm_q8(void)
{
	return miclimit_db_q8(radio_mic_pcm_db());
}
