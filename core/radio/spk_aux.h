// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker's second output (hal_spk_aux_t, docs/spec/radio-features.md 4.4): while the board says the stream's
// speaker output can't be heard (a speaker with no level while DMR is on air), each block's sound goes to the second
// output instead, so a beep, a prompt or voice reaches the speaker on whichever path works. The users of the stream
// (beeps, DMR and P25 voice) don't know: they get a wrapped hal_audio_t.
#ifndef RFOUNDRY_SPK_AUX_H
#define RFOUNDRY_SPK_AUX_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_audio.h"

// The stream the radio's users take: hw itself when the board has no second output (or CONFIG_SPEAKER_AUX is off).
// Not with the audio mux (CONFIG_NET_USRP, CONFIG_APRS_RX): its wrapper calls spk_aux_block and spk_aux_reset
const hal_audio_t *spk_aux_wrap(const hal_audio_t *hw);
// The second output is powered: a sound is queued, playing or in its tail, which the mic may hear. Audio task
bool spk_aux_pending(void);
// Sounds the second output played since boot (each power-up from off): debug link view audio.spk_aux_sounds
uint32_t spk_aux_sounds(void);

// The block step, after the users filled spk (exposed for the host tests): moves the block to the second output while
// that is active, with the power sequence around the sound, and silences the stream's copy
void spk_aux_block(const hal_spk_aux_t *aux, int16_t *spk);
void spk_aux_reset(const hal_spk_aux_t *aux);

#endif
