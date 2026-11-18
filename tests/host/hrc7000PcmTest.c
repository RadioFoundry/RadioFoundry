// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
// Event-driven hardware/RTOS doubles around the actual PCM producer and FIFO timer.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000.h"
#define PLATFORM_HRC7000_H
#define HRC_DMR_GLUE_H
#define CONFIG_VOX 0
#define IRQ_PCM_WR 28
#define IRQ_TIMER6 6
#define TIMER_PCM 6
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
#define portYIELD_FROM_ISR(x) ((void)(x))
typedef void *TaskHandle_t;
typedef int BaseType_t;
static void (*handlers[64])(void);
static unsigned now_us, last_sample, fill, gaps, played, writes, irq_off;
static unsigned timer_due, timer_period, task_wakes, room_at;
static unsigned irq_delays, max_irq_pushes, early_pushes, task_pushes;
static bool timer_on, in_irq, started, gated, room_seen;
static jmp_buf task_done;
static unsigned checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
	printf("FAIL PCM: "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static void consume_to(unsigned t)
{
	assert(t >= now_us);
	now_us = t;
	while (last_sample + 125u <= now_us) {
		last_sample += 125u;
		if (fill) { fill--; played++; }
		else if (started) gaps++;
	}
}
static void timer_event(void)
{
	unsigned before = writes, at = now_us;
	if (timer_period) timer_due += timer_period;
	else timer_on = false;
	in_irq = true;
	assert(handlers[IRQ_TIMER6] != NULL);
	handlers[IRQ_TIMER6]();
	in_irq = false;
	unsigned pushed = writes - before;
	if (pushed > max_irq_pushes) max_irq_pushes = pushed;
	if (now_us != at) irq_delays++;
}
static void advance(unsigned t)
{
	while (timer_on && timer_due <= t) {
		if (timer_due > now_us) consume_to(timer_due);
		timer_event();
	}
	if (now_us < t) consume_to(t);
}
static uint32_t fake_rd(hrc7000_t *m, uint32_t off)
{
	(void)m; (void)off;
	if (fill > C7_PROMPT_CHUNK) { room_seen = false; return C7_AUDIO_RING_FULL; }
	if (!room_seen) { room_seen = true; room_at = now_us; }
	return 0;
}
static bool fake_push(hrc7000_t *m, const int16_t *p)
{
	(void)m;
	assert(!gated);
	if (fill > C7_PROMPT_CHUNK) { room_seen = false; return false; }
	if (!room_seen || now_us - room_at < 200u) early_pushes++;
	if (!in_irq) task_pushes++;
	assert(p[0] != 0);
	fill += C7_PROMPT_CHUNK;
	writes++;
	started = true;
	room_seen = false;
	return true;
}
#define hrc7000_rd fake_rd
#define hrc7000_prompt_push fake_push
static uint32_t hrc_irq_save(void) { unsigned s = irq_off; irq_off = 1; return s; }
static void hrc_irq_restore(uint32_t s) { irq_off = s; }
static void hrc_irq_register(uint8_t n, void (*f)(void)) { handlers[n] = f; }
static void hrc_irq_enable(uint8_t n, bool on) { (void)n; (void)on; }
static void hrc_timer_ack(uint8_t n) { assert(n == TIMER_PCM); }
static void hrc_timer_periodic(uint8_t n, uint32_t us)
{ assert(n == TIMER_PCM && us == 2000); timer_period = us; timer_due = now_us + us; timer_on = true; }
static void hrc_timer_oneshot(uint8_t n, uint32_t us)
{ assert(n == TIMER_PCM && us > 0); timer_period = 0; timer_due = now_us + us; timer_on = true; }
static void hrc_timer_stop(uint8_t n) { assert(n == TIMER_PCM); timer_on = false; }
static uint32_t hrc_micros(void) { return now_us; }
static void hrc_barrier(void) { }
void hal_delay_us(uint32_t us) { if (in_irq) irq_delays++; consume_to(now_us + us); }
static bool hrc_dmr_tx_active(void) { return gated; }
static unsigned ulTaskNotifyTake(int clear, unsigned timeout)
{ (void)clear; (void)timeout; if (task_wakes++) longjmp(task_done, 1); return 1; }
static void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
static void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { (void)t; *w = 0; }
static int xTaskCreate(void (*f)(void *), const char *n, unsigned z, void *a, unsigned p, TaskHandle_t *h)
{ (void)f; (void)n; (void)z; (void)a; (void)p; *h = (void *)1; return pdPASS; }
void hrc7000_set_crit(hrc7000_t *m, uint32_t (*s)(void), void (*r)(uint32_t))
{ (void)m; (void)s; (void)r; }
void hrc7000_audio_bits(hrc7000_t *m, uint32_t set, uint32_t clear)
{ m->audio_ctl = (m->audio_ctl | set) & ~clear; }
#ifndef AUDIO_PCM_SOURCE
#define AUDIO_PCM_SOURCE "../../platform/hrc7000/audio_pcm.c"
#endif
#include AUDIO_PCM_SOURCE

static unsigned decode_us, action;
static int16_t value;
static void decode(const int16_t *in, int16_t *out, void *ctx)
{
	(void)in; (void)ctx;
	for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; i++) out[i] = value;
	if (decode_us) advance(now_us + decode_us);
	if (action == 1) hrc_audio_pcm_speaker(false);
	if (action == 2) { gated = true; hrc_audio_pcm_tx_gate(); }
	if (action == 3) { hrc_audio_pcm_stop(); hrc_audio_pcm_start(decode, NULL); }
}
static void produce(void)
{
	mic_ready = 0;
	task_wakes = 0;
	if (!setjmp(task_done)) audio_task(NULL);
}
static void reset(void)
{
	hrc_audio_pcm_stop();
	hrc_audio_pcm_speaker(false);
	hrc_audio_pcm_hold(false);
	now_us = last_sample = fill = gaps = played = writes = irq_off = 0;
	timer_on = in_irq = started = gated = room_seen = false;
	irq_delays = max_irq_pushes = early_pushes = task_pushes = 0;
	tx_gated = false;
	decode_us = action = 0;
	value = 1000;
	CHECK(hrc_audio_pcm_start(decode, NULL), "stream starts");
	hrc_audio_pcm_speaker(true);
}
static void prime(unsigned blocks_to_queue)
{
	// No feeder may drain the queue while arranging the producer's starting condition.
	hrc_audio_pcm_hold(true);
	for (unsigned i = 0; i < blocks_to_queue; i++) produce();
	hrc_audio_pcm_hold(false);
}
static bool first_check(void)
{
	if (!timer_on || !handlers[IRQ_TIMER6]) {
		CHECK(false, "the actual source starts an independent PCM timer");
		return false;
	}
	advance(timer_due);
	return true;
}
static void test_timer(void)
{
	reset(); prime(2);
	CHECK(first_check(), "first FIFO check runs");
	CHECK(writes == 0, "room check arms settle without writing");
	if (timer_on && writes == 0) {
		unsigned at = now_us;
		consume_to(now_us + 50u);
		timer_event(); // an early/spurious interrupt must not bypass the pending deadline
		CHECK(writes == 0, "early IRQ cannot write before settle deadline");
		advance(at + 199u);
		CHECK(writes == 0, "no write at 199 us");
		advance(at + 200u);
		CHECK(writes == 1, "one chunk writes after 200 us");
	}
	advance(now_us + 12000u);
	CHECK(irq_delays == 0, "ISR never waits or advances time (%u delay observations)", irq_delays);
	CHECK(max_irq_pushes <= 1, "at most one raw FIFO push per IRQ (%u)", max_irq_pushes);
	CHECK(early_pushes == 0, "every push follows a settled room observation (%u early)", early_pushes);
	CHECK(task_pushes == 0, "audio task never pushes speaker FIFO (%u)", task_pushes);
}
static void test_decoder(void)
{
	reset(); prime(2);
	decode_us = 12000;
	produce(); // the same actual decoder callback blocks task-based refill in the pre-PR source
	advance(now_us + 4000u);
	CHECK(started && played > 0, "queued speaker samples reach FIFO");
	CHECK(gaps == 0, "12 ms decode must not starve an 8 ms FIFO (%u missing samples)", gaps);
	printf("decoder stall: played=%u gap_samples=%u writes=%u\n", played, gaps, writes);
	reset(); prime(SPK_BLOCKS - 1u);
	uint8_t next = (uint8_t)((spk_head + 1u) % SPK_BLOCKS);
	decode_us = 20000; value = 2000;
	produce();
	CHECK(spk_head == next && spk_q[(spk_head + SPK_BLOCKS - 1u) % SPK_BLOCKS][0] == 2000,
	      "publish decoded block when space frees during callback");
	reset(); prime(2);
	advance(now_us + 10000u);
	value = 3000; produce();
	CHECK((uint8_t)((spk_head + SPK_BLOCKS - spk_tail) % SPK_BLOCKS) == 3,
	      "third output queues before the first primed slot is released");
}
static void test_cancel(void)
{
	for (unsigned kind = 0; kind < 7; kind++) {
		reset(); prime(2);
		if (!first_check()) continue;
		unsigned before = writes;
		if (kind == 0) hrc_audio_pcm_speaker(false);
		if (kind == 1) { gated = true; hrc_audio_pcm_tx_gate(); }
		if (kind == 2) hrc_audio_pcm_hold(true);
		if (kind == 3) hrc_audio_pcm_stop();
		if (kind == 4) { hrc_audio_pcm_stop(); hrc_audio_pcm_start(decode, NULL); }
		if (kind == 5) { hrc_audio_pcm_hold(true); hrc_audio_pcm_hold(false); }
		if (kind == 6) {
			hrc_audio_pcm_speaker(false); hrc_audio_pcm_speaker(true); prime(2);
		}
		advance(now_us + 200u);
		CHECK(writes == before, "pending write canceled by route/gate/hold/stop/restart transition %u", kind);
		if (kind < 4) {
			advance(now_us + 4000u);
			CHECK(writes == before, "disabled owner %u never writes", kind);
			if (kind == 0) hrc_audio_pcm_speaker(true);
			if (kind == 1) {
				gated = false;
				advance(now_us + 50000u);
				hrc_audio_pcm_speaker(true);	// after the minimum gate time, as the audio owner resumes
			}
			if (kind == 2) hrc_audio_pcm_hold(false);
			if (kind == 3) hrc_audio_pcm_start(decode, NULL);
			if (kind != 2) prime(2);
		}
		if (kind == 4) prime(2);
		advance(now_us + 6000u);
		CHECK(writes > before, "owner %u resumes with a fresh settled write", kind);
		CHECK(early_pushes == 0, "transition %u does not reuse an unsettled room observation", kind);
	}
	reset(); prime(2);
	if (first_check()) {
		unsigned before = writes;
		fill = 64; // the other FIFO user fills it during the settle window
		advance(now_us + 200u);
		CHECK(writes == before, "write phase rechecks hardware full");
	}
}
static void test_callback_edges(void)
{
	for (unsigned edge = 1; edge <= 3; edge++) {
		reset(); prime(2);
		action = edge; value = 4000;
		produce();
		CHECK(spk_head == spk_tail && !spk_primed,
		      "callback route/gate/restart generation edge %u cannot publish stale PCM", edge);
		CHECK(!irq_off, "callback edge %u restores interrupt state", edge);
	}
}
int main(void)
{
	(void)hrc_timer_ack; (void)hrc_timer_periodic; (void)hrc_timer_oneshot; (void)hrc_timer_stop;
	static uint32_t regs[0x1000 / 4];
	hrc7000_t m = { .regs = regs };
	hrc_audio_pcm_attach(&m, NULL);
	test_timer(); test_decoder(); test_cancel(); test_callback_edges();
	hrc_audio_pcm_stop();
	CHECK(!timer_on, "stop disarms timer");
	printf("hrc7000PcmTest: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
