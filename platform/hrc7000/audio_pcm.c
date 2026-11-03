// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
// The prompt FIFO also feeds the TX modulator (modem-hrc7000.md 8.4), so while the DMR engine transmits (the gate,
// hrc_audio_pcm_tx_gate, from the DMR glue) ring play stays clear and nothing is pushed. Blocks queued meanwhile are
// dropped when it ends: a beep still running plays on from where it is, not from where it was cut. The boards set
// ring play through hrc_audio_pcm_ring, which applies the gate.
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
#include "dmr_glue.h"
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
static bool spk_loud[SPK_BLOCKS];	// the queued block has a sample other than 0 (a beep, voice)
static volatile uint32_t loud_pushed;	// hrc_micros() when the last chunk of a loud block went to the FIFO
static volatile bool tx_gated;		// the DMR engine transmits: no ring play, no pushes
static bool ring_want;			// the board's ring play (hrc_audio_pcm_ring), applied when not gated
static uint32_t gate_at;		// hrc_micros() at the last close
// Gate closings; of those, with loud audio still queued or playing; checks that found ring play on while gated
static volatile uint32_t gates, loud_at_gate, ring_on_air;
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

static void gate_check(void);

// Push 32-sample chunks while the FIFO has room (AUDIO_CONTROL b31 clear). Task context: the task tops it up every
// 2 ms while the speaker is routed, faster than a chunk (4 ms) plays
static void fifo_feed(void)
{
	if (!spk_routed || fifo_held || tx_gated) {
		return;
	}
	while (spk_tail != spk_head && hrc7000_prompt_push(modem, &spk_q[spk_tail][spk_pos])) {
		if (spk_loud[spk_tail]) {
			loud_pushed = hrc_micros();
		}
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
		ulTaskNotifyTake(pdTRUE, spk_routed || tx_gated ? pdMS_TO_TICKS(2) : portMAX_DELAY);
		gate_check();
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
			int16_t *q = spk_q[spk_head];
			cb(mic, q, block_ctx);
			bool loud = false;
			for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES && !loud; i++) {
				loud = q[i] != 0;
			}
			spk_loud[spk_head] = loud;
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

// Empty the speaker queue. Callers have interrupts off; a block the task is filling just then may still land, and at
// most one chunk in the middle of a push reaches the FIFO (with ring play clear when gated, so it is never sent)
static void spk_drop(void)
{
	spk_head = spk_tail = 0;
	spk_pos = 0;
}

static void ring_apply(void)
{
	bool on = ring_want && !tx_gated;
	hrc7000_audio_bits(modem, on ? C7_AUDIO_RING_PLAY : 0, on ? 0 : C7_AUDIO_RING_PLAY);
}

// The board's ring play changes (a beep, APRS AFSK): the gate may be due to open first. The audio task, which opens it
// otherwise, starts with the first stream (a beep, voice): with none since boot, a DMR data or hotspot transmission
// left the gate closed, and APRS's tones off the modulator
void hrc_audio_pcm_ring(hrc7000_t *m, bool want)
{
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	modem = m;
	ring_want = want;
	gate_check();
	ring_apply();
	hrc_irq_restore(s);
}

// Speaker audio queued or still in the FIFO: the FIFO has no empty flag, so a push is taken to play out within
// FIFO_PLAY_MS (an upper bound on its depth, a few 4 ms chunks)
#define FIFO_PLAY_MS	32u

bool hrc_audio_pcm_spk_pending(void)
{
	uint32_t s = hrc_irq_save();
	bool loud = false;
	for (uint8_t i = spk_tail; i != spk_head && !loud; i = (uint8_t)((i + 1u) % SPK_BLOCKS)) {
		loud = spk_loud[i];
	}
	hrc_irq_restore(s);
	return loud || (hrc_micros() - loud_pushed) < FIFO_PLAY_MS * 1000u;
}

// Closed by the DMR glue as the engine starts to transmit; opened here once it has stopped (gate_check, from the
// audio task and every speaker_pcm call). GATE_MIN_US: the engine's TX start runs right after the close, and the gate
// must not open before it does
#define GATE_MIN_US	50000u

void hrc_audio_pcm_tx_gate(void)
{
	uint32_t s = hrc_irq_save();
	gate_at = hrc_micros();
	if (!tx_gated) {
		gates++;
		if (spk_routed && hrc_audio_pcm_spk_pending()) {
			loud_at_gate++;	// a beep was still sounding: its tail is cut, never sent
		}
		tx_gated = true;
		spk_drop();
		if (modem != NULL) {
			ring_apply();
		}
	}
	hrc_irq_restore(s);
	if (task != NULL) {
		xTaskNotifyGive(task);	// to open it again once the engine is done
	}
}

// Interrupts on or off. All of it under the lock: a close from another task between a check and the opening (a new
// tx_begin moves gate_at) must leave the gate closed
static void gate_check(void)
{
	if (!tx_gated || modem == NULL) {
		return;
	}
	uint32_t s = hrc_irq_save();
	if ((modem->audio_ctl & C7_AUDIO_RING_PLAY) != 0) {
		ring_on_air++;	// never: ring play came on while gated
	}
	if (tx_gated && hrc_micros() - gate_at >= GATE_MIN_US && !hrc_dmr_tx_active()) {
		tx_gated = false;
		spk_drop();	// what was queued meanwhile is stale
		ring_apply();
	}
	hrc_irq_restore(s);
}

static char *put_u(char *p, char *end, uint32_t v)
{
	char t[10];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0 && n < 10);
	while (n > 0 && p < end) {
		*p++ = t[--n];
	}
	return p;
}

static char *put_s(char *p, char *end, const char *s)
{
	while (*s != '\0' && p < end) {
		*p++ = *s++;
	}
	return p;
}

bool hrc_audio_pcm_gate_line(char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	char *p = buf, *end = buf + len - 1;
	p = put_s(p, end, "pg g");
	p = put_u(p, end, gates);
	p = put_s(p, end, " l");
	p = put_u(p, end, loud_at_gate);
	p = put_s(p, end, " ra");
	p = put_u(p, end, ring_on_air);
	*p = '\0';
	return true;
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
	spk_drop();
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
		spk_drop();
	}
	spk_routed = on;
	if (speaker_pcm_cb != NULL) {
		speaker_pcm_cb(on);
	}
	gate_check();
}

const hal_audio_t hrc_audio_pcm = {
	.start = start,
	.stop = stop,
	.speaker_pcm = speaker_pcm,
	.spk_pending = hrc_audio_pcm_spk_pending,
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
	gates = loud_at_gate = ring_on_air = 0;
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
