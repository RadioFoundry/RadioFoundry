// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 PCM framing (boards/gd77/pcm_gd77.c, docs/spec/hardware-gd77.md 8): slot 0 kept from the receive frames, the
// speaker sample duplicated into both transmit slots, two 10 ms DMA halves per 20 ms block, the block hand-over
// between the DMA side and the audio task, dropped blocks and transmit underruns
#include <stdio.h>
#include <string.h>
#include "pcm_gd77.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define HALF_WORDS	(GD77_I2S_HALF_FRAMES * GD77_I2S_SLOTS)

static gd77_pcm_t p;

// One received half: slot 0 counts up from base, slot 1 holds a marker that must never reach the microphone block
static void rx_half(int16_t *half, int16_t base)
{
	for (uint16_t i = 0; i < GD77_I2S_HALF_FRAMES; i++) {
		half[2 * i] = (int16_t)(base + i);
		half[2 * i + 1] = -1;
	}
}

static void test_geometry(void)
{
	CHECK(GD77_I2S_RING_HALFWORDS == 320);			// 20 ms ring
	CHECK(GD77_I2S_HALF_FRAMES * 2u == GD77_PCM_BLOCK);	// two halves make a block
	CHECK(GD77_I2S_RING_FRAMES == 160);
}

static void test_slot_helpers(void)
{
	int16_t in[8] = { 1, 100, 2, 200, 3, 300, 4, 400 }, out[4];
	gd77_pcm_slot0(in, out, 4);
	CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
	int16_t mono[3] = { 7, -8, 9 }, st[6];
	gd77_pcm_dup(mono, st, 3);
	CHECK(st[0] == 7 && st[1] == 7 && st[2] == -8 && st[3] == -8 && st[4] == 9 && st[5] == 9);
}

static void test_rx_blocks(void)
{
	int16_t half[HALF_WORDS];
	gd77_pcm_reset(&p);
	CHECK(gd77_pcm_mic_take(&p) == -1);
	rx_half(half, 0);
	CHECK(!gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES));	// half a block: nothing yet
	CHECK(gd77_pcm_mic_take(&p) == -1);
	rx_half(half, 80);
	CHECK(gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES));	// the block is complete
	int8_t b = gd77_pcm_mic_take(&p);
	CHECK(b == 0);
	for (int i = 0; i < 160; i++) {
		CHECK(p.mic[b][i] == i);				// slot 0 only, in order
	}
	CHECK(gd77_pcm_mic_take(&p) == -1);
	CHECK(p.blocks == 1 && p.dropped == 0);

	// Two blocks before the task looks: taken oldest first
	rx_half(half, 1000);
	gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
	rx_half(half, 1080);
	gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
	rx_half(half, 2000);
	gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
	rx_half(half, 2080);
	gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
	b = gd77_pcm_mic_take(&p);
	CHECK(b >= 0 && p.mic[b][0] == 1000);
	b = gd77_pcm_mic_take(&p);
	CHECK(b >= 0 && p.mic[b][0] == 2000);
	CHECK(p.dropped == 0);

	// Three blocks: the oldest is overwritten and counted, the newer two remain in order
	for (int blk = 0; blk < 3; blk++) {
		rx_half(half, (int16_t)(3000 + 1000 * blk));
		gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
		rx_half(half, (int16_t)(3080 + 1000 * blk));
		gd77_pcm_rx_half(&p, half, GD77_I2S_HALF_FRAMES);
	}
	CHECK(p.dropped == 1);
	b = gd77_pcm_mic_take(&p);
	CHECK(b >= 0 && p.mic[b][0] == 4000);
	b = gd77_pcm_mic_take(&p);
	CHECK(b >= 0 && p.mic[b][0] == 5000);
	CHECK(gd77_pcm_mic_take(&p) == -1);
}

static void test_tx_blocks(void)
{
	int16_t half[HALF_WORDS];
	gd77_pcm_reset(&p);
	// Nothing written yet: silence and an underrun
	memset(half, 0x55, sizeof half);
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	for (unsigned i = 0; i < HALF_WORDS; i++) {
		CHECK(half[i] == 0);
	}
	CHECK(p.underruns == 1);

	// One block written: sent over two halves, both slots, then silence again
	int8_t s = gd77_pcm_spk_slot(&p);
	CHECK(s == 0);
	for (int i = 0; i < 160; i++) {
		p.spk[s][i] = (int16_t)(i + 1);
	}
	gd77_pcm_spk_commit(&p);
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	CHECK(half[0] == 1 && half[1] == 1 && half[158] == 80 && half[159] == 80);
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	CHECK(half[0] == 81 && half[1] == 81 && half[159] == 160);
	CHECK(p.underruns == 1);
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	CHECK(half[0] == 0 && p.underruns == 2);

	// Both blocks waiting: no slot until one has gone out
	s = gd77_pcm_spk_slot(&p);
	CHECK(s >= 0);
	gd77_pcm_spk_commit(&p);
	s = gd77_pcm_spk_slot(&p);
	CHECK(s >= 0);
	gd77_pcm_spk_commit(&p);
	CHECK(gd77_pcm_spk_slot(&p) == -1);
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	CHECK(gd77_pcm_spk_slot(&p) == -1);	// half a block sent: still waiting
	gd77_pcm_tx_half(&p, half, GD77_I2S_HALF_FRAMES);
	CHECK(gd77_pcm_spk_slot(&p) >= 0);
}

// The stream in step: each received block comes back as the speaker block one block later
static void test_loop(void)
{
	int16_t rxh[HALF_WORDS], txh[HALF_WORDS];
	gd77_pcm_reset(&p);
	int16_t heard[4] = { 0 };
	for (int blk = 0; blk < 4; blk++) {
		for (int h = 0; h < 2; h++) {
			gd77_pcm_tx_half(&p, txh, GD77_I2S_HALF_FRAMES);
			if (h == 0) {
				heard[blk] = txh[0];
			}
			rx_half(rxh, (int16_t)(100 * (blk + 1) + 80 * h));
			if (gd77_pcm_rx_half(&p, rxh, GD77_I2S_HALF_FRAMES)) {
				int8_t b = gd77_pcm_mic_take(&p);
				int8_t s = gd77_pcm_spk_slot(&p);
				CHECK(b >= 0 && s >= 0);
				memcpy(p.spk[s], p.mic[b], sizeof p.spk[s]);
				gd77_pcm_spk_commit(&p);
			}
		}
	}
	CHECK(heard[0] == 0);		// nothing to play in the first block
	CHECK(heard[1] == 100 && heard[2] == 200 && heard[3] == 300);
	CHECK(p.dropped == 0);
}

int main(void)
{
	test_geometry();
	test_slot_helpers();
	test_rx_blocks();
	test_tx_blocks();
	test_loop();
	if (failures) {
		printf("gd77PcmTest: %d failures\n", failures);
		return 1;
	}
	printf("gd77PcmTest: all passed\n");
	return 0;
}
