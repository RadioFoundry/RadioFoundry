// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 PCM audio (hal_audio_t): I2S0 as a slave of the HR_C6000's codec, with one DMA ring each way
// (docs/spec/hardware-gd77.md 8). Beeps and DMR voice use it; FM receive audio doesn't (the HR_C6000 passes it to the
// line out itself).
//
// The codec drives a 512 kHz bit clock and an 8 kHz frame sync. platform/k22f/i2s.c runs the receiver on those clocks
// and the transmitter in step with it; DMA channel 0 fills rx_ring and channel 1 drains tx_ring, 320 halfwords
// (20 ms) each, with an interrupt every half (10 ms). The interrupt does the framing (pcm_gd77.c: slot 0 of the
// receive frames into 160-sample blocks, speaker samples into both transmit slots) and wakes the audio task once a
// microphone block is complete. The task runs the block callback, never the interrupt. One block of latency each way.
//
// Unverified: the pins (hardware-gd77.md 11), and which slot the codec fills with the microphone.
#include "gd77.h"
#include "config.h"

#if CONFIG_BEEP || CONFIG_DMR	// the PCM stream's only users
#include <string.h>
#include "pcm_gd77.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	640	// words: the block callbacks (beeps, the DMR voice path)
#define CLOCK_WAIT_MS		25u	// a half interrupt comes every 10 ms once the codec clocks run

static int16_t rx_ring[GD77_I2S_RING_HALFWORDS] __attribute__((aligned(4)));
static int16_t tx_ring[GD77_I2S_RING_HALFWORDS] __attribute__((aligned(4)));
static gd77_pcm_t pcm;
static int16_t mic[GD77_PCM_BLOCK];
static int16_t spk[GD77_PCM_BLOCK];

static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static volatile uint32_t halves;	// receive halves seen since the last start
static volatile uint32_t processed;	// blocks handed to the callback

static void dma_half(bool rx, bool second_half);

// The I2S frame: two 32-bit words, the 16-bit sample in the low half, MSB first, frame sync one bit early (I2S style)
static const k22f_i2s_stream_t stream = {
	.fmt = { .words = 2, .word_bits = 32, .first_bit = 15, .early_fs = true, .fs_active_low = false },
	.rx_ring = rx_ring,
	.tx_ring = tx_ring,
	.ring_samples = GD77_I2S_RING_HALFWORDS,
	.half = dma_half,
};

// DMA interrupt (priority 6, below the RTOS syscall level)
static void dma_half(bool rx, bool second_half)
{
	uint32_t off = second_half ? GD77_I2S_RING_HALFWORDS / 2u : 0u;
	if (!rx) {
		gd77_pcm_tx_half(&pcm, &tx_ring[off], GD77_I2S_HALF_FRAMES);
		return;
	}
	halves++;
	if (gd77_pcm_rx_half(&pcm, &rx_ring[off], GD77_I2S_HALF_FRAMES) && task != NULL) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

static void process(void)
{
	for (;;) {
		taskENTER_CRITICAL();
		int8_t b = gd77_pcm_mic_take(&pcm);
		taskEXIT_CRITICAL();
		if (b < 0 || !running) {
			return;
		}
		memcpy(mic, pcm.mic[b], sizeof mic);	// the receive side writes the other block meanwhile
		audio_block_cb_t cb = block_cb;
		if (cb != NULL) {
			cb(mic, spk, block_ctx);
		} else {
			memset(spk, 0, sizeof spk);
		}
		taskENTER_CRITICAL();
		int8_t s = gd77_pcm_spk_slot(&pcm);
		taskEXIT_CRITICAL();
		if (s >= 0) {
			memcpy(pcm.spk[s], spk, sizeof spk);	// not sent until committed
			taskENTER_CRITICAL();
			gd77_pcm_spk_commit(&pcm);
			taskEXIT_CRITICAL();
		}
		processed++;
	}
}

static void audio_task(void *arg)
{
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		process();
	}
}

// --- hal_audio_t ---

static bool start(audio_block_cb_t cb, void *ctx)
{
	if (task == NULL &&
	    xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &task) != pdPASS) {
		task = NULL;
		return false;
	}
	block_ctx = ctx;
	block_cb = cb;
	if (running) {
		return true;	// already streaming: just the new callback
	}
	gd77_pcm_reset(&pcm);
	memset(tx_ring, 0, sizeof tx_ring);
	halves = 0;
	running = true;
	if (k22f_i2s_start(&stream) != K22F_OK) {
		running = false;
		return false;
	}
	// No receive half within a few periods: the codec isn't clocking (powered down, or its I2S port is off)
	uint32_t t0 = hal_millis();
	while (halves == 0) {
		if ((uint32_t)(hal_millis() - t0) > CLOCK_WAIT_MS) {
			k22f_i2s_stop();
			running = false;
			return false;
		}
		hal_delay_ms(1);
	}
	return true;
}

// Safe to call from the block callback itself (the audio task): streaming stops, the task stays for the next start()
static void stop(void)
{
	running = false;
	k22f_i2s_stop();
	block_cb = NULL;
}

static void speaker_pcm(bool on)
{
	gd77_speaker_pcm(on);
}

void gd77_audio_stats(gd77_audio_stats_t *out)
{
	out->blocks = processed;
	out->dropped = pcm.dropped;
	out->underruns = pcm.underruns;
	out->i2s_errors = k22f_i2s_errors();
	out->running = running;
}

const hal_audio_t gd77_audio = { .start = start, .stop = stop, .speaker_pcm = speaker_pcm };

#endif	// CONFIG_BEEP || CONFIG_DMR
