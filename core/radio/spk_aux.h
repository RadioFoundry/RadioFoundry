// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker's second output (hal_spk_aux_t, docs/spec/radio-features.md 4.4): while the board says the stream's
// speaker output can't be heard (a speaker with no level while DMR is on air), each block's sound goes to the second
// output instead, so a beep, a prompt or voice reaches the speaker on whichever path works. The users of the stream
// (beeps, DMR and P25 voice) don't know: they get a wrapped hal_audio_t.
//
// No build switch: the core reaches the step only through the table a board with the output names
// (hal_spk_aux_t.core = SPK_AUX_CORE), so another board links none of it.
#ifndef RFOUNDRY_SPK_AUX_H
#define RFOUNDRY_SPK_AUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal_audio.h"

struct spk_aux_core {
	// The stream wrapped around the step; NULL with the audio mux (CONFIG_NET_USRP, CONFIG_APRS_RX), whose own
	// wrapper calls block and reset
	const hal_audio_t *(*wrap)(const hal_audio_t *hw);
	// After the users filled spk: moves the block to the second output while that is active, with the power
	// sequence around the sound, and silences the stream's copy
	void (*block)(const hal_spk_aux_t *aux, int16_t *spk);
	void (*reset)(const hal_spk_aux_t *aux);
	bool (*pending)(void);		// powered: a sound queued, playing or in its tail, which the mic may hear
	uint32_t (*sounds)(void);	// sounds played since boot (each power-up from off)
};
extern const struct spk_aux_core spk_aux_core;
#define SPK_AUX_CORE	(&spk_aux_core)

// The board's second output, or NULL
static inline const hal_spk_aux_t *spk_aux_of(const hal_audio_t *a)
{
	return a != NULL ? a->spk_aux : NULL;
}

// The stream the radio's users take: a itself when the board has no second output, or with the audio mux
static inline const hal_audio_t *spk_aux_stream(const hal_audio_t *a)
{
	const hal_spk_aux_t *x = spk_aux_of(a);
	return x != NULL && x->core->wrap != NULL ? x->core->wrap(a) : a;
}

// The second output is powered (the mic gate, debug link view audio.spk_aux_on). Audio task
static inline bool spk_aux_busy(const hal_audio_t *a)
{
	const hal_spk_aux_t *x = spk_aux_of(a);
	return x != NULL && x->core->pending();
}

// The step's parts, exposed for the host tests: reached through SPK_AUX_CORE everywhere else
void spk_aux_block(const hal_spk_aux_t *aux, int16_t *spk);
void spk_aux_reset(const hal_spk_aux_t *aux);
bool spk_aux_pending(void);
uint32_t spk_aux_sounds(void);

#endif
