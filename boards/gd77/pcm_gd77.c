// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 PCM framing between the I2S0 DMA rings and 20 ms blocks (pcm_gd77.h)
#include <string.h>
#include "pcm_gd77.h"

void gd77_pcm_reset(gd77_pcm_t *p)
{
	memset(p, 0, sizeof(*p));
}

void gd77_pcm_slot0(const int16_t *in, int16_t *out, uint16_t frames)
{
	for (uint16_t i = 0; i < frames; i++) {
		out[i] = in[GD77_I2S_SLOTS * i];
	}
}

void gd77_pcm_dup(const int16_t *in, int16_t *out, uint16_t frames)
{
	for (uint16_t i = 0; i < frames; i++) {
		out[GD77_I2S_SLOTS * i] = in[i];
		out[GD77_I2S_SLOTS * i + 1u] = in[i];
	}
}

bool gd77_pcm_rx_half(gd77_pcm_t *p, const int16_t *half, uint16_t frames)
{
	if (p->mic_fill == 0 && (p->mic_ready & (1u << p->mic_w)) != 0) {
		// The task never took this block: it is overwritten now, and the other one is the oldest
		p->mic_ready &= (uint8_t)~(1u << p->mic_w);
		p->mic_next = (uint8_t)(p->mic_w ^ 1u);
		p->dropped++;
	}
	uint16_t room = (uint16_t)(GD77_PCM_BLOCK - p->mic_fill);
	uint16_t n = frames < room ? frames : room;
	gd77_pcm_slot0(half, &p->mic[p->mic_w][p->mic_fill], n);
	p->mic_fill = (uint16_t)(p->mic_fill + n);
	if (p->mic_fill < GD77_PCM_BLOCK) {
		return false;
	}
	if (p->mic_ready == 0) {
		p->mic_next = p->mic_w;
	}
	p->mic_ready |= (uint8_t)(1u << p->mic_w);
	p->mic_w ^= 1u;
	p->mic_fill = 0;
	p->blocks++;
	return true;
}

void gd77_pcm_tx_half(gd77_pcm_t *p, int16_t *half, uint16_t frames)
{
	if ((p->spk_full & (1u << p->spk_r)) == 0) {
		memset(half, 0, (size_t)frames * GD77_I2S_SLOTS * sizeof(int16_t));
		p->underruns++;
		return;
	}
	uint16_t left = (uint16_t)(GD77_PCM_BLOCK - p->spk_pos);
	uint16_t n = frames < left ? frames : left;
	gd77_pcm_dup(&p->spk[p->spk_r][p->spk_pos], half, n);
	if (n < frames) {
		memset(&half[GD77_I2S_SLOTS * n], 0, (size_t)(frames - n) * GD77_I2S_SLOTS * sizeof(int16_t));
	}
	p->spk_pos = (uint16_t)(p->spk_pos + n);
	if (p->spk_pos >= GD77_PCM_BLOCK) {
		p->spk_full &= (uint8_t)~(1u << p->spk_r);
		p->spk_r ^= 1u;
		p->spk_pos = 0;
	}
}

int8_t gd77_pcm_mic_take(gd77_pcm_t *p)
{
	if (p->mic_ready == 0) {
		return -1;
	}
	uint8_t b = (p->mic_ready & (1u << p->mic_next)) != 0 ? p->mic_next : (uint8_t)(p->mic_next ^ 1u);
	p->mic_ready &= (uint8_t)~(1u << b);
	p->mic_next = (uint8_t)(b ^ 1u);
	return (int8_t)b;
}

int8_t gd77_pcm_spk_slot(const gd77_pcm_t *p)
{
	return (p->spk_full & (1u << p->spk_w)) != 0 ? -1 : (int8_t)p->spk_w;
}

void gd77_pcm_spk_commit(gd77_pcm_t *p)
{
	p->spk_full |= (uint8_t)(1u << p->spk_w);
	p->spk_w ^= 1u;
}
