// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Authors
// Simulated 8 kHz / 64-sample FIFO around the real PCM queue and timer code.
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
static unsigned now, last_sample, fill, gaps, played, writes, timer_on, irq_off;
static bool started, gated;
static jmp_buf task_done;
static unsigned task_wakes;
static void advance(unsigned t)
{
	assert(t >= now); now = t;
	while (last_sample + 125 <= now) {
		last_sample += 125;
		if (fill) { fill--; played++; }
		else if (started) gaps++;
	}
}
static uint32_t fake_rd(hrc7000_t *m, uint32_t off) { (void)m;(void)off; return fill > 32 ? C7_AUDIO_RING_FULL : 0; }
static bool fake_push(hrc7000_t *m, const int16_t *p)
{
	(void)m; assert(!gated); assert(fill <= 32); assert(p[0] != 0);
	fill += 32; writes++; started = true; return true;
}
#define hrc7000_rd fake_rd
#define hrc7000_prompt_push fake_push
static uint32_t hrc_irq_save(void) { unsigned s=irq_off;irq_off=1;return s; }
static void hrc_irq_restore(uint32_t s) { irq_off=s; }
static void hrc_irq_register(uint8_t n, void (*f)(void)) { handlers[n]=f; }
static void hrc_irq_enable(uint8_t n, bool on) { (void)n;(void)on; }
static void hrc_timer_ack(uint8_t n) { assert(n==6); }
static void hrc_timer_periodic(uint8_t n,uint32_t us) { assert(n==6&&us==2000);timer_on=1; }
static void hrc_timer_stop(uint8_t n) { assert(n==6);timer_on=0; }
static uint32_t hrc_micros(void) { return now; }
static void hrc_barrier(void) {}
void hal_delay_us(uint32_t us) { advance(now+us); }
static bool hrc_dmr_tx_active(void) { return gated; }
static unsigned ulTaskNotifyTake(int clear,unsigned timeout)
{ (void)clear; (void)timeout; if (task_wakes++) longjmp(task_done, 1); return 1; }
static void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
static void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w) { (void)t;*w=0; }
static int xTaskCreate(void (*f)(void*),const char *n,unsigned z,void *a,unsigned p,TaskHandle_t *h)
{ (void)f;(void)n;(void)z;(void)a;(void)p;*h=(void*)1;return pdPASS; }
void hrc7000_set_crit(hrc7000_t *m,uint32_t (*s)(void),void (*r)(uint32_t)) { (void)m;(void)s;(void)r; }
void hrc7000_audio_bits(hrc7000_t *m,uint32_t set,uint32_t clear) { m->audio_ctl=(m->audio_ctl|set)&~clear; }
#include "../../platform/hrc7000/audio_pcm.c"
static void block(const int16_t *in,int16_t *out,void *ctx) { (void)in;(void)ctx;memset(out,0,320); }
static void seed(void)
{
	for (unsigned b=0;b<2;b++) {
		for(unsigned i=0;i<160;i++) spk_q[b][i]=1000;
		spk_loud[b]=true;
	}
	spk_tail=0;spk_head=2;spk_pos=0;spk_primed=false;spk_routed=true;
	now=last_sample=fill=gaps=played=writes=0; started=false;gated=false;
	tx_gated=false;fifo_held=false;
}
// The queue is full when decoding starts, but one queued block drains during that work.
static void decode_while_full(const int16_t *in, int16_t *out, void *ctx)
{
	(void)in; (void)ctx;
	for (unsigned i = 0; i < 160; i++) out[i] = 2000;
	handlers[6]();
	for (unsigned t = 2000; t <= 20000; t += 2000) { advance(t); handlers[6](); }
}
static void fast_decode(const int16_t *in, int16_t *out, void *ctx)
{
	(void)in; (void)ctx;
	for (unsigned i = 0; i < 160; i++) out[i] = 3000;
}
int main(void)
{
	static uint32_t regs[0x1000/4];hrc7000_t m={.regs=regs};
	hrc_audio_pcm_attach(&m,NULL);
	assert(hrc_audio_pcm_start(block,NULL));
	assert(timer_on && handlers[6]);
	// Old task-only service: the decoder blocks for 12 ms after topping up an 8 ms FIFO.
	seed();fifo_feed();advance(12000);assert(gaps>=28);
	unsigned old_gaps=gaps;
	// The real timer keeps feeding during the same blocked task interval.
	seed();handlers[6]();
	for(unsigned t=2000;t<=16000;t+=2000) { advance(t);handlers[6](); }
	assert(gaps==0 && played>=126 && writes>=4);
	// Pausing for AFSK and the TX gate keep every shared speaker user off the FIFO.
	unsigned before=writes;hrc_audio_pcm_hold(true);advance(18000);handlers[6]();assert(writes==before);
	hrc_audio_pcm_hold(false);gated=true;hrc_audio_pcm_tx_gate();advance(20000);handlers[6]();assert(writes==before);
	gated=false;hrc_audio_pcm_speaker(false);assert(!irq_off);
	hrc_audio_pcm_stop();assert(!timer_on);
	assert(hrc_audio_pcm_start(decode_while_full, NULL));
	seed();
	for (unsigned i = 0; i < 160; i++) spk_q[2][i] = 1000;
	spk_loud[2] = true; spk_head = 3; mic_ready = 0; task_wakes = 0;
	if (!setjmp(task_done)) audio_task(NULL);
	assert(spk_head == 0 && spk_tail == 1 && spk_q[3][0] == 2000);
	hrc_audio_pcm_stop(); assert(!timer_on);
	// Decode cost varies: the next output may be ready before either primed block's slot is released.
	assert(hrc_audio_pcm_start(fast_decode, NULL));
	seed(); handlers[6]();
	for (unsigned t = 2000; t <= 10000; t += 2000) { advance(t); handlers[6](); }
	mic_ready = 0; task_wakes = 0;
	if (!setjmp(task_done)) audio_task(NULL);
	assert(spk_tail == 0 && spk_head == 3 && spk_q[2][0] == 3000);
	hrc_audio_pcm_stop(); assert(!timer_on);
	printf("hrc7000PcmTest: task-only gap=%u samples; timer gap=0; holds/gates/restart/full-queue decode passed\n",old_gaps);
	return 0;
}
