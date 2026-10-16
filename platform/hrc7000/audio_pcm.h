// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 PCM audio shared by the boards (audio_pcm.c): the modem's two 10 ms PCM buffers in the shared SRAM as
// hal_audio_t, paced by the microphone interrupt
#ifndef HRC_AUDIO_PCM_H
#define HRC_AUDIO_PCM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_audio.h"
#include "modem/hrc7000/hrc7000.h"

#if CONFIG_DMR
// Once, from the radio init, after the modem is attached. speaker_pcm(on) switches the modem's speaker source between
// its FM demodulator and the CPU's PCM buffer (the radio owns the AUDIO_CONTROL bits); it runs with interrupts on
void hrc_audio_pcm_attach(hrc7000_t *modem, void (*speaker_pcm)(bool on));
// Keep speaker PCM out of the prompt FIFO while another user owns it (APRS AFSK); what is queued plays after
void hrc_audio_pcm_hold(bool on);

extern const hal_audio_t hrc_audio_pcm;

// Block, overrun and underrun counters for diagnostics
void hrc_audio_pcm_stats(uint32_t *blocks, uint32_t *overruns, uint32_t *underruns);
void hrc_audio_pcm_reset_stats(void);
#endif

// The microphone level for VOX (hal_power_t.mic_level), from the mic PCM (audio_pcm.c, drivers/audio/miclevel).
// HRC_MIC_LEVEL is the hook for a board's hal_power_t, NULL in a build without the PCM stream or VOX
#if CONFIG_DMR && CONFIG_VOX
uint8_t hrc_audio_pcm_mic_level(void);
// The meter now: its level, the last half's peak from its mean (0..32767), halves measured, running. Doesn't start it
void hrc_audio_pcm_mic_stats(uint8_t *level, uint16_t *peak, uint32_t *halves, bool *on);
#define HRC_MIC_LEVEL		hrc_audio_pcm_mic_level
#else
#define HRC_MIC_LEVEL		NULL
#endif

#if !CONFIG_DMR
static inline void hrc_audio_pcm_attach(hrc7000_t *modem, void (*speaker_pcm)(bool on)) { (void)modem; (void)speaker_pcm; }
static inline void hrc_audio_pcm_hold(bool on) { (void)on; }
#endif

#endif
