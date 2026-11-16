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
// The FIFO takes only two of those chunks (8 ms), less than a block, and the blocks come at the rate it plays them.
// Pushed as each block came, it was nearly empty at every block edge and ran dry there, which broke each tone up at
// 20 ms intervals (modem-hrc7000.md 8.4). So after the route turns on (or a gate edge) nothing is pushed until
// SPK_PRIME_BLOCKS are queued: the queue stays a block ahead of the FIFO, at the cost of one block of latency. Each
// push waits a moment after the FIFO showed room (prompt_fifo.h): written at once, a chunk could land in the half
// being played, and the audio then jumped by a chunk every 4 ms.
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
#include "prompt_fifo.h"

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
#define SPK_PRIME_BLOCKS	2	// queued before the first push after the queue was emptied (spk_drop)

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
static volatile uint16_t spk_gain = 32768u;	// Q15, hrc_audio_pcm_spk_gain
// underruns: pushes that emptied the queue while loud audio played and the FIFO still had room (it ran dry next)
static volatile uint32_t blocks, overruns, underruns;
static bool spk_primed;		// SPK_PRIME_BLOCKS were queued since the last spk_drop: the pushes go on
static bool spk_loud[SPK_BLOCKS];	// the queued block has a sample other than 0 (a beep, voice)
static volatile uint32_t loud_pushed;	// hrc_micros() when the last chunk of a loud block went to the FIFO
static volatile bool loud_recent;	// loud_pushed is set and less than FIFO_PLAY_MS old when last looked at
static volatile uint8_t spk_gen;	// bumped by spk_drop: a block the task filled meanwhile is not queued
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
	if (!spk_primed) {
		if ((uint8_t)((spk_head + SPK_BLOCKS - spk_tail) % SPK_BLOCKS) < SPK_PRIME_BLOCKS) {
			return;
		}
		spk_primed = true;
	}
	bool full = false, pushed = false;
	while (spk_tail != spk_head) {
		uint8_t gen = spk_gen;
		if (!hrc_prompt_push(modem, &spk_q[spk_tail][spk_pos])) {
			full = true;
			break;
		}
		if (gen != spk_gen) {
			return;	// a gate edge emptied the queue during the push's wait: its state is spk_drop's, not ours
		}
		pushed = true;
		if (spk_loud[spk_tail]) {
			loud_pushed = hrc_micros();
			loud_recent = true;
		}
		spk_pos = (uint8_t)(spk_pos + C7_PROMPT_CHUNK);
		if (spk_pos >= AUDIO_BLOCK_SAMPLES) {
			spk_pos = 0;
			spk_tail = (uint8_t)((spk_tail + 1u) % SPK_BLOCKS);
		}
	}
	if (pushed && !full && loud_recent) {
		underruns++;	// this push emptied the queue (once per run-out, not per poll)
	}
}

static void audio_task(void *arg)
{
	(void)arg;
	static int16_t mic[AUDIO_BLOCK_SAMPLES];
	for (;;) {
		// Gated without the speaker stream only the gate's opening is waited for: 10 ms late is fine for the beep
		// after TX, and a hotspot keeps the gate closed for hours
		ulTaskNotifyTake(pdTRUE, spk_routed ? pdMS_TO_TICKS(2) : tx_gated ? pdMS_TO_TICKS(10) : portMAX_DELAY);
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
			uint8_t gen = spk_gen;
			cb(mic, q, block_ctx);
			bool loud = false;
			uint32_t g = spk_gain;
			for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
				if (q[i] != 0) {
					loud = true;	// before the gain: a quiet beep still counts as sound for the gate
					if (g < 32768u) {
						q[i] = (int16_t)(((int32_t)q[i] * (int32_t)g) >> 15);
					}
				} else if (loud && g >= 32768u) {
					break;
				}
			}
			// A gate edge emptied the queue during cb() (another task): this block is as stale as the rest
			s = hrc_irq_save();
			if (gen == spk_gen) {
				spk_loud[spk_head] = loud;
				spk_head = next;
			}
			hrc_irq_restore(s);
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

void hrc_audio_pcm_spk_gain(uint16_t q15)
{
	spk_gain = q15;
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
	spk_primed = false;
	spk_gen++;
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
	if (loud_recent && hrc_micros() - loud_pushed >= FIFO_PLAY_MS * 1000u) {
		loud_recent = false;	// played out: an old time can't look recent again when the counter wraps
	}
	loud = loud || loud_recent;
	hrc_irq_restore(s);
	return loud;
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

void hrc_audio_pcm_gate_stats(uint32_t *closings, uint32_t *late, uint32_t *ring_on)
{
	*closings = gates;
	*late = loud_at_gate;
	*ring_on = ring_on_air;
}

void hrc_audio_pcm_gate_reset(void)
{
	gates = loud_at_gate = ring_on_air = 0;
	underruns = 0;	// on the same line
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

bool hrc_audio_pcm_start(audio_block_cb_t cb, void *ctx)
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

void hrc_audio_pcm_stop(void)
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
void hrc_audio_pcm_speaker(bool on)
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

const hal_audio_t hrc_audio_pcm = { HRC_AUDIO_PCM_OPS };

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
