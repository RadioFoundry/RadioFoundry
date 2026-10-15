// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 PCM audio shared by the boards (audio_pcm.c): the modem's two 10 ms PCM buffers in the shared SRAM as
// hal_audio_t, paced by the microphone interrupt
#ifndef HRC_AUDIO_PCM_H
#define HRC_AUDIO_PCM_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_audio.h"
#include "modem/hrc7000/hrc7000.h"

#if CONFIG_DMR
// Once, from the radio init, after the modem is attached. speaker_pcm(on) switches the modem's speaker source between
// its FM demodulator and the CPU's PCM buffer (the radio owns the AUDIO_CONTROL bits); it runs with interrupts on
void hrc_audio_pcm_attach(hrc7000_t *modem, void (*speaker_pcm)(bool on));

extern const hal_audio_t hrc_audio_pcm;

// Block, overrun and underrun counters for diagnostics
void hrc_audio_pcm_stats(uint32_t *blocks, uint32_t *overruns, uint32_t *underruns);
void hrc_audio_pcm_reset_stats(void);
#else
static inline void hrc_audio_pcm_attach(hrc7000_t *modem, void (*speaker_pcm)(bool on)) { (void)modem; (void)speaker_pcm; }
#endif

#endif
