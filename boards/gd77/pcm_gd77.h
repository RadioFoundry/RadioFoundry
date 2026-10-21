// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 PCM framing between the I2S0 DMA rings and the core's 20 ms blocks (docs/spec/hardware-gd77.md 8). Pure: no
// hardware access, so tests/host/gd77PcmTest.c runs it.
//
// Each I2S frame carries two 32-bit slots with a 16-bit sample in the low half, and the DMA moves that half: one ring
// holds 160 frames (320 halfwords, 20 ms), and its half and full interrupts each hand over 80 frames (10 ms). The
// receive side keeps slot 0 of every frame and collects two halves into one 160-sample microphone block; the transmit
// side writes each speaker sample into both slots. Two blocks each way: the DMA side fills (or drains) one while the
// audio task works on the other.
//
// Threading: gd77_pcm_rx_half and gd77_pcm_tx_half run in the DMA interrupts; the mic_take / spk_* calls run in the
// audio task with those interrupts masked around the call (they only move indices; the copy is outside).
#ifndef GD77_PCM_H
#define GD77_PCM_H

#include <stdint.h>
#include <stdbool.h>

#define GD77_PCM_BLOCK		160u	// samples per block, AUDIO_BLOCK_SAMPLES
#define GD77_I2S_SLOTS		2u
#define GD77_I2S_RING_FRAMES	160u
#define GD77_I2S_RING_HALFWORDS	(GD77_I2S_RING_FRAMES * GD77_I2S_SLOTS)	// 320 = 20 ms
#define GD77_I2S_HALF_FRAMES	(GD77_I2S_RING_FRAMES / 2u)		// 80 = 10 ms

typedef struct {
	int16_t mic[2][GD77_PCM_BLOCK];
	int16_t spk[2][GD77_PCM_BLOCK];
	uint16_t mic_fill;		// samples in mic[mic_w]
	uint8_t mic_w;			// block the receive side fills
	volatile uint8_t mic_ready;	// bit b: mic[b] complete and not taken yet
	uint8_t mic_next;		// the older ready block, taken first
	uint16_t spk_pos;		// next sample of spk[spk_r] the transmit side sends
	uint8_t spk_r, spk_w;
	volatile uint8_t spk_full;	// bit b: spk[b] written and not sent yet
	uint32_t blocks;		// microphone blocks completed
	uint32_t dropped;		// microphone blocks overwritten before the task took them
	uint32_t underruns;		// transmit halves sent as silence: no speaker block ready
} gd77_pcm_t;

void gd77_pcm_reset(gd77_pcm_t *p);

// Slot 0 of each of frames interleaved frames
void gd77_pcm_slot0(const int16_t *in, int16_t *out, uint16_t frames);
// Each sample into both slots of a frame
void gd77_pcm_dup(const int16_t *in, int16_t *out, uint16_t frames);

// DMA side. rx_half: one received half (frames x 2 halfwords); true when it completed a microphone block.
// tx_half: fill one half that has just been sent
bool gd77_pcm_rx_half(gd77_pcm_t *p, const int16_t *half, uint16_t frames);
void gd77_pcm_tx_half(gd77_pcm_t *p, int16_t *half, uint16_t frames);

// Task side. mic_take: the oldest complete microphone block (its index), -1 when none. The block stays valid for one
// block period (the receive side writes the other one meanwhile)
int8_t gd77_pcm_mic_take(gd77_pcm_t *p);
// spk_slot: the speaker block to write next, -1 when both are still waiting to be sent; spk_commit hands it over
int8_t gd77_pcm_spk_slot(const gd77_pcm_t *p);
void gd77_pcm_spk_commit(gd77_pcm_t *p);

#endif
