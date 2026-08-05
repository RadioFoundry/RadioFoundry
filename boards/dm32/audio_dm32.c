// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV PCM audio: the HR_C7000's own codec through its two 10 ms PCM buffers in the shared SRAM
// (docs/spec/hardware-dm32.md 9; modem-hrc7000.md 3, 8.1).
//
// With AUDIO_CONTROL b4 set the modem fills 80 mic samples at 0x18000000 every 10 ms and raises PCM_wr (PIC 28); with
// b5 set it plays 80 samples from 0x180000A0 and raises PCM_rd (PIC 27) for the next ones. The mic interrupt paces
// everything: two halves make a 20 ms block, the task runs the block callback (never in the ISR) and queues the
// speaker block it returned, which the PCM_rd interrupt plays out half by half (one block of latency).
//
// Sample format in the buffers: 16-bit native (little-endian) words assumed; check on the first DMR call.
#include <string.h>
#include "dm32.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1536	// words: the vocoder in the callback
#define HALF			C7_PCM_SAMPLES	// 80
#define SPK_BLOCKS		3

static int16_t mic_blk[2][AUDIO_BLOCK_SAMPLES];	// double buffered: the ISR fills one while the task reads the other
static int16_t spk_q[SPK_BLOCKS][AUDIO_BLOCK_SAMPLES];
static volatile uint8_t spk_head, spk_tail;	// task writes head, ISR reads tail
static volatile uint8_t spk_half;
static volatile uint8_t mic_fill, mic_half;
static volatile int8_t mic_ready = -1;		// block index ready for the task

static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static volatile uint32_t blocks, overruns, underruns;

static volatile int16_t *const pcm_mic = (volatile int16_t *)C7_PCM_MIC_ADDR;
static volatile int16_t *const pcm_spk = (volatile int16_t *)C7_PCM_SPK_ADDR;

static void isr_pcm_wr(void)
{
	hrc7000_wr(dm32_modem(), C7_INTERP_CLEAR, C7_INT_PCM_WR);
	if (!running) {
		return;
	}
	int16_t *dst = &mic_blk[mic_fill][mic_half * HALF];
	for (uint32_t i = 0; i < HALF; i++) {
		dst[i] = pcm_mic[i];
	}
	if (++mic_half < 2) {
		return;
	}
	mic_half = 0;
	if (mic_ready >= 0) {
		overruns++;	// the task hasn't taken the previous block: it is overwritten
	}
	mic_ready = (int8_t)mic_fill;
	mic_fill ^= 1u;
	if (task != NULL) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

static void isr_pcm_rd(void)
{
	hrc7000_wr(dm32_modem(), C7_INTERP_CLEAR, C7_INT_PCM_RD);
	if (spk_tail == spk_head) {
		for (uint32_t i = 0; i < HALF; i++) {
			pcm_spk[i] = 0;
		}
		underruns++;
		return;
	}
	const int16_t *src = &spk_q[spk_tail][spk_half * HALF];
	for (uint32_t i = 0; i < HALF; i++) {
		pcm_spk[i] = src[i];
	}
	if (++spk_half >= 2) {
		spk_half = 0;
		spk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);
	}
}

static void audio_task(void *arg)
{
	(void)arg;
	static int16_t mic[AUDIO_BLOCK_SAMPLES];
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t s = hrc_irq_save();
		int8_t idx = mic_ready;
		if (idx >= 0) {
			memcpy(mic, mic_blk[idx], sizeof mic);
			mic_ready = -1;
		}
		hrc_irq_restore(s);
		audio_block_cb_t cb = block_cb;
		if (idx < 0 || !running || cb == NULL) {
			continue;
		}
		uint8_t next = (uint8_t)((spk_head + 1u) % SPK_BLOCKS);
		if (next == spk_tail) {
			static int16_t discard[AUDIO_BLOCK_SAMPLES];
			cb(mic, discard, block_ctx);	// the speaker side is full (PCM not routed): keep the mic side going
		} else {
			cb(mic, spk_q[spk_head], block_ctx);
			spk_head = next;
		}
		blocks++;
	}
}

static void audio_bits(uint32_t set, uint32_t clear)
{
	uint32_t s = hrc_irq_save();
	hrc7000_audio_bits(dm32_modem(), set, clear);
	hrc_irq_restore(s);
}

static bool start(audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		hrc_irq_register(IRQ_PCM_WR, isr_pcm_wr);
		hrc_irq_register(IRQ_PCM_RD, isr_pcm_rd);
		if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIORITY, &task) != pdPASS) {
			task = NULL;
			return false;
		}
	}
	block_ctx = ctx;
	block_cb = cb;
	mic_fill = 0;
	mic_half = 0;
	mic_ready = -1;
	spk_head = spk_tail = 0;
	spk_half = 0;
	hrc_barrier();
	running = true;
	hrc7000_wr(dm32_modem(), C7_INTERP_CLEAR, C7_INT_PCM_WR | C7_INT_PCM_RD);
	hrc_irq_enable(IRQ_PCM_WR, true);
	hrc_irq_enable(IRQ_PCM_RD, true);
	audio_bits(C7_AUDIO_PCM_MIC, 0);
	return true;
}

static void stop(void)
{
	audio_bits(0, C7_AUDIO_PCM_MIC);
	hrc_irq_enable(IRQ_PCM_WR, false);
	running = false;
	block_cb = NULL;
}

// Speaker PCM: the modem plays the CPU's buffer instead of its FM demodulator (radio_dm32.c owns the bits)
static void speaker_pcm(bool on)
{
	if (on) {
		spk_head = spk_tail = 0;
		spk_half = 0;
	}
	dm32_speaker_pcm(on);
}

const hal_audio_t dm32_audio = {
	.start = start,
	.stop = stop,
	.speaker_pcm = speaker_pcm,
};

void dm32_audio_stats(uint32_t *b, uint32_t *ov, uint32_t *un)
{
	*b = blocks;
	*ov = overruns;
	*un = underruns;
}

void dm32_audio_reset_stats(void)
{
	blocks = overruns = underruns = 0;
}

#endif	// CONFIG_DMR
