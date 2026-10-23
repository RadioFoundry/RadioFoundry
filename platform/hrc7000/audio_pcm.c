// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 PCM audio (audio_pcm.h): mic samples from the modem's PCM buffer, speaker samples to its prompt FIFO
// (docs/spec/hardware-dm32.md 9; modem-hrc7000.md 3, 8.1, 8.4).
//
// With AUDIO_CONTROL b4 set the modem fills 80 mic samples at 0x18000000 every 10 ms and raises PCM_wr (PIC 28). The
// mic interrupt paces everything: two halves make a 20 ms block, the task runs the block callback (never in the ISR)
// and queues the speaker block it returned. While the speaker is routed (AUDIO_CONTROL b7, set by the board's hook)
// the task pushes the queue to the prompt FIFO, 32 samples at a time while b31 says there is room, as stock does. The
// CPU-fed speaker buffer (b5, 0x180000A0) sounded rough and choppy on the bench with every sample layout tried.
//
// Sample format: mic buffer 16-bit native words; prompt FIFO 16-bit samples, low byte first (bench).
//
// Mic level for VOX (CONFIG_VOX, hrc_audio_pcm_mic_level): these radios have no analog mic level tap, so the level is
// measured on the same mic PCM. The first call turns the PCM interrupt on without the stream (the meter), and the
// interrupt turns it off again once nobody has asked for METER_IDLE_BLOCKS halves. The stream and the meter share
// AUDIO_CONTROL b4 and the interrupt: each turns them off only when the other isn't using them.
#include <string.h>
#include "config.h"
#include "audio_pcm.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_VOX
#include "audio/miclevel/miclevel.h"
#endif

#define AUDIO_TASK_PRIORITY	5	// above the radio task: a late block is an audible gap
#define AUDIO_TASK_STACK	1536	// words: the vocoder in the callback
#define HALF			C7_PCM_SAMPLES	// 80
#define SPK_BLOCKS		3

static int16_t mic_blk[2][AUDIO_BLOCK_SAMPLES];	// double buffered: the ISR fills one while the task reads the other
static int16_t spk_q[SPK_BLOCKS][AUDIO_BLOCK_SAMPLES];
static volatile uint8_t spk_head, spk_tail;	// task writes head, ISR reads tail
static uint8_t spk_pos;			// samples of spk_q[spk_tail] already pushed
static volatile uint8_t mic_fill, mic_half;
static volatile int8_t mic_ready = -1;		// block index ready for the task

static hrc7000_t *modem;
static void (*speaker_pcm_cb)(bool on);
static TaskHandle_t task;
static audio_block_cb_t block_cb;
static void *block_ctx;
static volatile bool running;
static bool spk_routed;		// the last speaker_pcm() state
static volatile bool fifo_held;	// another user (the board's AFSK) owns the FIFO: speaker PCM waits
static volatile uint32_t blocks, overruns, underruns;	// underruns: the FIFO does not report them (stays 0)
static bool isr_registered;

#if CONFIG_VOX
#define METER_IDLE_BLOCKS	100u	// halves (1 s) without a mic_level() call: the meter stops
static miclevel_t meter;
static volatile bool meter_on;
static volatile uint8_t meter_idle;	// halves since the last mic_level() call
#endif

static volatile int16_t *const pcm_mic = (volatile int16_t *)C7_PCM_MIC_ADDR;

static void isr_pcm_wr(void)
{
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_PCM_WR);
#if CONFIG_VOX
	if (meter_on) {
		miclevel_block(&meter, pcm_mic, HALF);
		if (++meter_idle >= METER_IDLE_BLOCKS) {
			meter_on = false;	// nobody reads it (VOX off): the stream's use alone decides now
			if (!running) {
				hrc7000_audio_bits(modem, 0, C7_AUDIO_PCM_MIC);
				hrc_irq_enable(IRQ_PCM_WR, false);
			}
		}
	}
#endif
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

// Push 32-sample chunks while the FIFO has room (AUDIO_CONTROL b31 clear). Task context: the task tops it up every
// 2 ms while the speaker is routed, faster than a chunk (4 ms) plays
static void fifo_feed(void)
{
	if (!spk_routed || fifo_held) {
		return;
	}
	while (spk_tail != spk_head && hrc7000_prompt_push(modem, &spk_q[spk_tail][spk_pos])) {
		spk_pos = (uint8_t)(spk_pos + C7_PROMPT_CHUNK);
		if (spk_pos >= AUDIO_BLOCK_SAMPLES) {
			spk_pos = 0;
			spk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);
		}
	}
}

static void audio_task(void *arg)
{
	(void)arg;
	static int16_t mic[AUDIO_BLOCK_SAMPLES];
	for (;;) {
		ulTaskNotifyTake(pdTRUE, spk_routed ? pdMS_TO_TICKS(2) : portMAX_DELAY);
		fifo_feed();
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
		fifo_feed();
	}
}

void hrc_audio_pcm_attach(hrc7000_t *m, void (*speaker_pcm)(bool on))
{
	modem = m;
	speaker_pcm_cb = speaker_pcm;
	// The meter's interrupt changes AUDIO_CONTROL b4 (isr_pcm_wr): from now on every shadow update, the board's FM
	// receive, transmit, idle and DMR entry included, runs with interrupts off
	hrc7000_set_crit(m, hrc_irq_save, hrc_irq_restore);
}

void hrc_audio_pcm_hold(bool on)
{
	fifo_held = on;
}

// Once: registering masks the source, so never while the meter or the stream has it on
static void isr_register(void)
{
	uint32_t s = hrc_irq_save();	// the stream (audio users' task) and the meter (radio task) can both get here first
	if (!isr_registered) {
		hrc_irq_register(IRQ_PCM_WR, isr_pcm_wr);
		isr_registered = true;
	}
	hrc_irq_restore(s);
}

static bool start(audio_block_cb_t cb, void *ctx)
{
	if (task == NULL) {
		isr_register();
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
	spk_pos = 0;
	hrc_barrier();
	running = true;
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_PCM_WR);
	hrc_irq_enable(IRQ_PCM_WR, true);
	hrc7000_audio_bits(modem, C7_AUDIO_PCM_MIC, 0);	// under the lock set at attach
	return true;
}

static void stop(void)
{
	uint32_t s = hrc_irq_save();	// the meter's interrupt decides on the same state
	running = false;
	block_cb = NULL;
#if CONFIG_VOX
	bool keep = meter_on;
#else
	bool keep = false;
#endif
	if (!keep) {
		hrc7000_audio_bits(modem, 0, C7_AUDIO_PCM_MIC);
		hrc_irq_enable(IRQ_PCM_WR, false);
	}
	hrc_irq_restore(s);
}

// Speaker PCM: the modem plays the CPU's samples instead of its FM demodulator (the board's hook owns the bits).
// Callers repeat the call every block (beep.c), from inside the block callback, so the queue starts empty only when
// the route turns on: emptying it on every call threw away the block being queued and the one playing (choppy beeps)
static void speaker_pcm(bool on)
{
	if (on != spk_routed) {
		spk_head = spk_tail = 0;
		spk_pos = 0;
	}
	spk_routed = on;
	if (speaker_pcm_cb != NULL) {
		speaker_pcm_cb(on);
	}
}

const hal_audio_t hrc_audio_pcm = {
	.start = start,
	.stop = stop,
	.speaker_pcm = speaker_pcm,
};

void hrc_audio_pcm_stats(uint32_t *b, uint32_t *ov, uint32_t *un)
{
	*b = blocks;
	*ov = overruns;
	*un = underruns;
}

void hrc_audio_pcm_reset_stats(void)
{
	blocks = overruns = underruns = 0;
}

#if CONFIG_VOX
// hal_power_t.mic_level. Task level (the radio task's VOX poll, the debug link). The first call after the meter
// stopped turns it on and returns 0: the level needs a block
uint8_t hrc_audio_pcm_mic_level(void)
{
	if (modem == NULL) {
		return 0;
	}
	meter_idle = 0;
	if (meter_on) {
		return meter.level;
	}
	isr_register();
	uint32_t s = hrc_irq_save();
	miclevel_reset(&meter);
	meter_idle = 0;
	meter_on = true;
	if (!running) {
		hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_PCM_WR);	// a stale flag would count as a block at once
	}
	hrc_irq_enable(IRQ_PCM_WR, true);
	hrc7000_audio_bits(modem, C7_AUDIO_PCM_MIC, 0);
	hrc_irq_restore(s);
	return 0;
}

void hrc_audio_pcm_mic_stats(uint8_t *level, uint16_t *peak, uint32_t *halves, bool *on)
{
	*level = meter.level;
	*peak = meter.peak;
	*halves = meter.blocks;
	*on = meter_on;
}
#endif

#endif	// CONFIG_DMR
