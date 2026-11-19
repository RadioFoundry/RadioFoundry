// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 DMR modem glue (dmr_glue.h): the slot engine (drivers/modem/hrc7000/hrc7000_dmr.c) on the SoC's modem
// interrupts, the DMR PA one-shot on Timer 3, and the deferred event task that calls the DMR stack (docs/spec/dmr-modem.md
// 6, modem-hrc7000.md 3, hardware-dm32.md 8.5, 8.6).
//
// What runs where (all interrupts at one level, never nested; this file and the engine run from RAM,
// the board's linker script, but for the task-side code marked HRC_FLASHFUNC):
//   PIC 29 (system)      received bursts, late entry, slot abnormal; clears the chip's flags
//   PIC 30 / 31 (TS)     decide the coming slot (TX burst, receive, nothing), PA off after a burst, slot tick event
//   PIC 32 (RF TX)       FD6818 idle -> TX for our burst
//   Timer 3              PA one-shot, armed with every burst: PA select and drive "DMR PA" ms later
//   "dmr" task           drains the engine's events into the stack's callback; every 10 ms the stall watchdog
// Task-level calls into the engine mask the modem sources at the PIC (hrc_modem_irqs_mask) for their register work.
#include <stddef.h>
#include <string.h>
#include "config.h"
#include "dmr_glue.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "platform.h"
#include "modem/hrc7000/hrc7000_dmr.h"
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
#include "modem/hrc7000/hrc7000_bs.h"
#include "dmr/bsOutbound.h"
#endif
#include "audio_pcm.h"
#if CONFIG_AIR
#include "modem/hrc7000/hrc7000_air.h"
#endif
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define DMR_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define DMR_TASK_STACK		512	// words: the stack's callback runs on it
#define DMR_POLL_MS		10

static hrc7000_dmr_t eng;
static const hrc_dmr_board_t *brd;
static TaskHandle_t task;
static volatile bool active;
static volatile bool rf_on_rx;	// the last burst switch put the RF back to receive (hrc_dmr_tx_end)
static dmr_event_cb_t volatile stack_cb;
static void *volatile stack_ctx;

#if CONFIG_AIR
// An air interface session (hrc_air) or the receive tap (hrc_baseband) holds the chip: the interrupts and the task go to
// it, and its state lives in the slot engine's queues, which a halted engine leaves unused (hrc7000_air.h)
typedef struct {
	hrc7000_air_t a;
	hrc7000_air_buf_t b;
} air_mem_t;
_Static_assert(sizeof(air_mem_t) <= sizeof(eng.evq), "the air session no longer fits the slot engine's event queue");
// The soft modem lives in the engine's transmit queue, a queue of bytes: the state starts at its first address aligned
// for it, or the first word store faults (the queue sits at an odd offset in the engine)
#define AIR_SM_ALIGN	(_Alignof(air_softmodem_t))
_Static_assert(sizeof(air_softmodem_t) + AIR_SM_ALIGN - 1u <= sizeof(eng.txq),
	       "the soft modem no longer fits the slot engine's TX queue");
#define AIR_SM \
	((air_softmodem_t *)(void *)(((uintptr_t)(void *)eng.txq + AIR_SM_ALIGN - 1u) & ~(uintptr_t)(AIR_SM_ALIGN - 1u)))
#define AIR		(&((air_mem_t *)(void *)eng.evq)->a)
static volatile bool air_on;
static air_event_cb_t air_cb;
static void *air_ctx;
#define AIR_ON()	air_on
#else
#define AIR_ON()	false
#endif

// --- Engine ops ---

static uint32_t now_us(void *ctx)
{
	(void)ctx;
	return hrc_micros();
}

static uint32_t lock_saved;
static uint32_t lock_depth;

static void lock(void *ctx)
{
	(void)ctx;
	uint32_t s = hrc_irq_save();
	if (lock_depth++ == 0) {
		lock_saved = hrc_modem_irqs_mask();
	}
	hrc_irq_restore(s);
}

static void unlock(void *ctx)
{
	(void)ctx;
	uint32_t s = hrc_irq_save();
	if (lock_depth > 0 && --lock_depth == 0) {
		hrc_modem_irqs_restore(lock_saved);
	}
	hrc_irq_restore(s);
}

static void notify(void *ctx)
{
	(void)ctx;
	if (task == NULL) {
		return;
	}
	if (hrc_in_isr()) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	} else {
		xTaskNotifyGive(task);
	}
}

// A split duplex hotspot's base-station outbound keys once for its whole length (hrc7000_bs.c)
// An air interface transmission too: keyed at its first RF TX interrupt, held to its end (hrc7000_air.c)
static bool outbound_continuous(void)
{
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
	return eng.bs != NULL || AIR_ON();
#else
	return AIR_ON();
#endif
}

static void rf(void *ctx, bool tx)
{
	(void)ctx;
	if (brd->burst != NULL) {
#if CONFIG_AIR
		uint32_t tick = air_on ? AIR->t_tick : eng.t_tick;
#else
		uint32_t tick = eng.t_tick;
#endif
		hrc_rfburst_switch(tx, outbound_continuous(), tick);
	} else if (brd->rf_switch != NULL) {
		brd->rf_switch(tx);
	}
	rf_on_rx = !tx;
}

static void pa_arm(void *ctx)
{
	(void)ctx;
	// The burst sequences key the PA themselves; a board with neither has no PA key (and no pa_delay_us)
	if (brd->burst == NULL && brd->pa_on != NULL) {
		hrc_timer_oneshot(TIMER_PA, brd->pa_delay_us);
	}
}

static const hrc7000_dmr_ops_t ops = {
	.rf = rf, .pa_arm = pa_arm, .now_us = now_us, .lock = lock, .unlock = unlock, .notify = notify, .ctx = NULL,
};

// --- Interrupts ---

#if CONFIG_AIR
static void isr_sys(void) { if (air_on) hrc7000_air_isr_sys(AIR); else hrc7000_dmr_isr_sys(&eng); }
static void isr_ts_tx(void) { if (air_on) hrc7000_air_isr_timeslot(AIR); else hrc7000_dmr_isr_timeslot(&eng, true); }
static void isr_ts_rx(void) { if (air_on) hrc7000_air_isr_timeslot(AIR); else hrc7000_dmr_isr_timeslot(&eng, false); }
static void isr_rftx(void) { if (air_on) hrc7000_air_isr_rftx(AIR); else hrc7000_dmr_isr_rftx(&eng); }
#else
static void isr_sys(void) { hrc7000_dmr_isr_sys(&eng); }
static void isr_ts_tx(void) { hrc7000_dmr_isr_timeslot(&eng, true); }
static void isr_ts_rx(void) { hrc7000_dmr_isr_timeslot(&eng, false); }
static void isr_rftx(void) { hrc7000_dmr_isr_rftx(&eng); }
#endif

static void isr_pa_timer(void)
{
	hrc_timer_ack(TIMER_PA);
	hrc_timer_stop(TIMER_PA);
	if (active && brd->pa_on != NULL) {
		brd->pa_on();
	}
}

static void irqs_enable(bool on)
{
	hrc_irq_enable(IRQ_MODEM_SYS, on || eng.fm_rx != NULL);	// FM receive audio uses it outside DMR too
	hrc_irq_enable(IRQ_MODEM_TS_TX, on);
	hrc_irq_enable(IRQ_MODEM_TS_RX, on);
	hrc_irq_enable(IRQ_MODEM_RF_TX, on);
	if (!on) {
		hrc_timer_stop(TIMER_PA);
	}
}

// --- Lost interrupts (hal_dmr_modem_t.health) ---

static uint32_t lost_seen;	// stalls + stuck requests at the last check
static uint32_t lost_ms;	// the first one's time (never 0 once set) and the PIC's words just before it was handled
static uint32_t lost_regs[4];

// The PIC's mask and request words, read before each poll: after it, the poll's recovery has dropped the requests
HRC_FLASHFUNC static void pic_words(uint32_t w[4])
{
	w[0] = PIC_MASK;
	w[1] = PIC_MASK_1;
	w[2] = PIC_INT_ST;
	w[3] = PIC_INT_ST_1;
}

// The poll found a lost interrupt (a stalled transmission or a request pending with no handler run): keep the first
// one's PIC words, and unmask the modem sources again unless a task is inside the engine
HRC_FLASHFUNC static void lost_check(const uint32_t before[4])
{
	uint32_t n = eng.st.stalls + eng.st.sys_stuck;
	if (n == lost_seen) {
		return;
	}
	lost_seen = n;
	if (lost_ms == 0) {
		memcpy(lost_regs, before, sizeof lost_regs);
		lost_ms = hal_millis() | 1u;
	}
	uint32_t s = hrc_irq_save();
	if (active && lock_depth == 0) {
		irqs_enable(true);
	}
	hrc_irq_restore(s);
}

HRC_FLASHFUNC static void health(dmr_modem_health_t *h)
{
	h->stalls = eng.st.stalls;
	h->stuck = eng.st.sys_stuck;
	h->first_ms = lost_ms;
	memcpy(h->first_regs, lost_regs, sizeof h->first_regs);
}

// --- Deferred event task ---

HRC_FLASHFUNC static void dmr_task(void *arg)
{
	(void)arg;
	dmr_event_t ev;
	dmr_burst_t burst;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DMR_POLL_MS));
		if (active) {
			uint32_t pic[4];
			pic_words(pic);
			hrc7000_dmr_poll(&eng);
			lost_check(pic);
		}
#if CONFIG_AIR
		if (air_on) {
			hrc7000_air_poll(AIR);	// the engine is halted, and its queue is the session's memory
			continue;
		}
#endif
		while (hrc7000_dmr_pop(&eng, &ev, &burst)) {
			dmr_event_cb_t cb = stack_cb;
			if (cb != NULL) {
				cb(&ev, stack_ctx);
			}
		}
		if (hrc7000_dmr_short_lc(&eng, &ev)) {
			dmr_event_cb_t cb = stack_cb;
			if (cb != NULL) {
				cb(&ev, stack_ctx);
			}
		}
	}
}

// --- The radio's interface ---

HRC_FLASHFUNC void hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks)
{
	brd = hooks;
	hrc7000_dmr_init(&eng, modem, &ops);
	hrc_irq_register(IRQ_MODEM_SYS, isr_sys);
	hrc_irq_register(IRQ_MODEM_TS_TX, isr_ts_tx);
	hrc_irq_register(IRQ_MODEM_TS_RX, isr_ts_rx);
	hrc_irq_register(IRQ_MODEM_RF_TX, isr_rftx);
	hrc_irq_register(IRQ_TIMER3, isr_pa_timer);
	hrc_irq_enable(IRQ_TIMER3, true);
	irqs_enable(false);
	if (hooks->burst != NULL) {
		hrc_rfburst_init(modem, hooks->burst);
	}
	if (task == NULL) {
		xTaskCreate(dmr_task, "dmr", DMR_TASK_STACK, NULL, DMR_TASK_PRIORITY, &task);
	}
}

void hrc_dmr_enter(void)
{
	rf_on_rx = false;
#if CONFIG_AIR
	if (air_on) {
		// Back in DMR under a session or the tap (the board wrote the chip's DMR set-up): the copy set-up again
		active = true;
		irqs_enable(true);
		hrc7000_air_resume(AIR);
		return;
	}
#endif
	hrc7000_dmr_run(&eng);
	active = true;
	irqs_enable(true);
}

void hrc_dmr_leave(void)
{
	irqs_enable(false);
	active = false;
#if CONFIG_AIR
	if (air_on) {
		hrc7000_air_suspend(AIR);	// a transmission ends (TX_DONE); the engine is halted already
		rf_on_rx = false;
		return;
	}
#endif
	hrc7000_dmr_halt(&eng);
	rf_on_rx = false;	// after the halt: its switch back to RX doesn't count outside DMR
}

bool hrc_dmr_fm_rx_hook(void (*fn)(void))
{
	if (fn != NULL && AIR_ON()) {
		return false;	// the copy interrupt is the session's
	}
	uint32_t s = hrc_irq_save();
	eng.fm_rx = fn;
	hrc_irq_restore(s);
	hrc_irq_enable(IRQ_MODEM_SYS, active || fn != NULL);
	return true;
}

bool hrc_dmr_tx_end(void)
{
	if (!rf_on_rx || brd == NULL || brd->tx_end == NULL) {
		return false;
	}
	rf_on_rx = false;	// once: a later rx() is a full set-up
	brd->tx_end();
	return true;
}

void hrc_dmr_rx_restart(void)
{
	rf_on_rx = false;
#if CONFIG_AIR
	if (air_on) {
		hrc7000_air_rx_restart(AIR);
		return;
	}
#endif
	hrc7000_dmr_rx_restart(&eng);
}

bool hrc_dmr_tx_active(void)
{
#if CONFIG_AIR
	if (air_on) {
		return active && hrc7000_air_tx_active(AIR);
	}
#endif
	return active && hrc7000_dmr_is_transmitting(&eng);
}

void hrc_dmr_tx_drop(void)
{
	if (!active) {
		return;
	}
#if CONFIG_AIR
	if (air_on) {
		hrc7000_air_tx_abort(AIR);	// PA off now, TX_DONE to the session, receiving
		return;
	}
#endif
	hrc7000_dmr_halt(&eng);		// PA off now, TX_DONE to the stack, engine off ...
	hrc7000_dmr_run(&eng);		// ... and on again, receiving
}

// --- hal_dmr_modem_t ---

// While a session or the tap holds the chip the engine's calls do nothing: its queues are their memory
static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	if (!active || AIR_ON()) {
		return false;	// the radio is not in DMR mode (set_mode(RADIO_MODE_DMR) first)
	}
	hrc7000_dmr_configure(&eng, cfg);
	stack_ctx = ctx;
	hrc_barrier();
	stack_cb = cb;
	return true;
}

static void stop(void)
{
	stack_cb = NULL;
	if (active && !AIR_ON() && hrc7000_dmr_is_transmitting(&eng)) {
		hrc_dmr_tx_drop();
	}
}

static void configure(const dmr_config_t *cfg) { if (!AIR_ON()) hrc7000_dmr_configure(&eng, cfg); }
static void rx_restart(void) { if (!AIR_ON()) hrc7000_dmr_rx_restart(&eng); }
static void rx_lock_slot(int8_t ts) { if (!AIR_ON()) hrc7000_dmr_rx_lock_slot(&eng, ts); }

// The prompt FIFO also feeds the modulator: its gate closes before the first burst can be armed, and audio_pcm.c opens
// it once the engine has stopped transmitting
static bool tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	if (AIR_ON()) {
		return false;
	}
	hrc_audio_pcm_tx_gate();
	return active && hrc7000_dmr_tx_begin(&eng, timing, ts, access);
}

static bool tx_queue(const dmr_tx_burst_t *b) { return !AIR_ON() && hrc7000_dmr_tx_queue(&eng, b); }
static uint8_t tx_queue_space(void) { return AIR_ON() ? 0 : hrc7000_dmr_tx_queue_space(&eng); }
static void tx_listen(void) { if (!AIR_ON()) hrc7000_dmr_tx_listen(&eng); }
HRC_FLASHFUNC static bool tx_listen_split(void)
{
	if (brd == NULL || !brd->listen_split || AIR_ON()) {
		return false;
	}
	tx_listen();
	return true;
}
static int8_t tx_fill(const dmr_tx_burst_t *b) { return AIR_ON() ? -1 : hrc7000_dmr_tx_fill(&eng, b); }
static void tx_end(void) { if (!AIR_ON()) hrc7000_dmr_tx_end(&eng); }
static void tx_abort(void) { if (!AIR_ON()) hrc7000_dmr_tx_abort(&eng); }
static void set_embedded_lc(const uint8_t lc[9]) { if (!AIR_ON()) hrc7000_dmr_set_embedded_lc(&eng, lc); }
static void set_burst_f(const uint8_t field[4]) { if (!AIR_ON()) hrc7000_dmr_set_burst_f(&eng, field); }
static void set_cc_filter(int8_t cc) { if (!AIR_ON()) hrc7000_dmr_set_cc_filter(&eng, cc); }
static bool is_transmitting(void) { return !AIR_ON() && hrc7000_dmr_is_transmitting(&eng); }

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
// --- Split duplex (docs/spec/split-duplex.md 5): the slot clock and the TX half's BS outbound (hrc7000_bs.c) ---

static hrc7000_bs_t bs;

static uint32_t split_now_us(void)
{
	return hrc_micros();
}

static bool slot_clock(uint32_t *counter, uint32_t *t_us, int8_t *ts)
{
	return active && hrc7000_dmr_slot_clock(&eng, counter, t_us, ts);
}

static bool bs_start(const dmr_bs_config_t *cfg)
{
	return active && !AIR_ON() && hrc7000_bs_start(&bs, &eng, cfg, bsOutboundRawSlot);
}

static void bs_key(bool on)
{
	if (on) {
		hrc_audio_pcm_tx_gate();	// as tx_begin
	}
	hrc7000_bs_key(&eng, on);
}

static bool bs_queue(int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach, const uint8_t *emb_lc)
{
	return hrc7000_bs_queue(&eng, ts, b, cach, emb_lc);
}

static bool bs_queue_at(uint32_t counter, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
			const uint8_t *emb_lc)
{
	return hrc7000_bs_queue_at(&eng, counter, ts, b, cach, emb_lc);
}

static bool bs_restart(uint32_t t_us, int8_t ts) { return active && hrc7000_bs_restart(&eng, t_us, ts); }

static uint8_t bs_space(int8_t ts) { return hrc7000_bs_space(&eng, ts); }

static void bs_stop(void)
{
	if (hrc7000_bs_active(&eng)) {
		hrc7000_bs_stop(&eng);
	}
}

static bool bs_keyed(void) { return hrc7000_bs_keyed(&eng); }

#define SPLIT_OPS	.now_us = split_now_us, .slot_clock = slot_clock, .bs_start = bs_start, .bs_key = bs_key, \
			.bs_queue = bs_queue, .bs_space = bs_space, .bs_stop = bs_stop, .bs_keyed = bs_keyed, \
			.bs_cach = HRC7000_BS_CACH_METHOD, .bs_queue_at = bs_queue_at, .bs_restart = bs_restart, \
			.bs_air_delay_us = HRC7000_BS_AIR_DELAY_US,
#else
#define SPLIT_OPS
#endif

const hal_dmr_modem_t hrc_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_listen = tx_listen, .tx_end = tx_end,
	.tx_listen_split = tx_listen_split,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_burst_f = set_burst_f, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting, .tx_fill = tx_fill, .health = health,
	.tx_rate34_bad = true,	// modem-hrc7000.md 6.4
	.tx_listen_gaps = true,	// modem-hrc7000.md 5.4
	SPLIT_OPS
};

#if CONFIG_AIR
// --- hal_air_t and hal_baseband_t: the receive copy and the physical layer (hrc7000_air.c) ---
// Task level, so in flash; only the interrupts above and hrc7000_air.c's handlers run from RAM

// The chip from the halted engine to the session or the tap: the engine's queues become their memory
HRC_FLASHFUNC static bool air_take(void)
{
	if (!active || air_on || hrc7000_dmr_is_transmitting(&eng) || eng.fm_rx != NULL) {
		return false;	// not in DMR, a DMR transmission, or FM receive audio (APRS) holds the copy interrupt
	}
	hrc7000_dmr_halt(&eng);
	lock(NULL);
	eng.evq_tail = eng.evq_head;	// what the engine left is dropped
	hrc7000_air_init(AIR, eng.dev, &ops, &((air_mem_t *)(void *)eng.evq)->b, AIR_SM);
	return true;	// locked
}

// ... and back: the engine's queues empty again, the engine running when the chip is in DMR
HRC_FLASHFUNC static void air_give(void)
{
	air_on = false;
	eng.evq_head = eng.evq_tail = 0;
	eng.txq_head = eng.txq_tail = 0;
	unlock(NULL);
	if (active) {
		hrc7000_dmr_run(&eng);
	}
}

HRC_FLASHFUNC static void air_event(const air_event_t *ev, void *ctx)
{
	(void)ctx;
	air_event_cb_t cb = air_cb;
	if (cb != NULL) {
		cb(ev, air_ctx);
	}
}

HRC_FLASHFUNC static uint32_t air_fit(const air_profile_t *p)
{
	uint32_t f = air_softmodem_rx_fit(p, &hrc_baseband) | hrc7000_air_tx_fit(p);
	// Untested until the board says its receive and transmit were checked on air (modem-hrc7000.md 10.4)
	return f != 0 && (brd == NULL || !brd->air_verified) ? f | AIR_FIT_UNTESTED : f;
}

// Rule 1 (hal_air.h): in DMR the speaker carries only the codec's PCM, and the session keeps FM playback off. Rule 2:
// in the physical layer the modulator sends TX RAM only, and the prompt FIFO's gate is closed for each transmission
HRC_FLASHFUNC static bool air_open(const air_profile_t *p, air_event_cb_t cb, void *ctx)
{
	uint32_t f = air_fit(p) & ~(uint32_t)AIR_FIT_UNTESTED;
	if (f == 0 || cb == NULL || !air_take()) {
		return false;
	}
	air_cb = cb;
	air_ctx = ctx;
	bool ok = hrc7000_air_open(AIR, p, f, air_event, NULL);
	if (!ok) {
		air_cb = NULL;
		air_give();
		return false;
	}
	air_on = true;
	unlock(NULL);
	return true;
}

HRC_FLASHFUNC static void air_close(void)
{
	if (!air_on || !AIR->open) {
		return;
	}
	lock(NULL);
	hrc7000_air_close(AIR, active);
	air_cb = NULL;
	air_give();
}

HRC_FLASHFUNC static void air_rx_restart(void)
{
	if (air_on) {
		hrc7000_air_rx_restart(AIR);
	}
}

HRC_FLASHFUNC static bool air_tx_begin(void)
{
	if (!active || !air_on) {
		return false;
	}
	hrc_audio_pcm_tx_gate();	// the prompt FIFO also feeds the modulator (as tx_begin)
	return hrc7000_air_tx_begin(AIR);
}

HRC_FLASHFUNC static bool air_tx_queue(const air_tx_chunk_t *c) { return air_on && hrc7000_air_tx_queue(AIR, c); }
HRC_FLASHFUNC static uint8_t air_tx_space(void) { return air_on ? hrc7000_air_tx_space(AIR) : 0; }
HRC_FLASHFUNC static void air_tx_end(void) { if (air_on) hrc7000_air_tx_end(AIR); }
HRC_FLASHFUNC static void air_tx_abort(void) { if (air_on) hrc7000_air_tx_abort(AIR); }
static uint32_t air_now_us(void) { return hrc_micros(); }

static bool air_rx_load(uint32_t *work_us, uint32_t *span_us)
{
	*work_us = air_on ? AIR->st.modem_us : 0;
	*span_us = air_on ? AIR->st.modem_span_us : 0;
	return true;
}

static bool air_rx_counts(air_rx_counts_t *c)
{
	if (!air_on) {
		return false;
	}
	*c = (air_rx_counts_t){ .halves = AIR->st.halves, .overruns = AIR->st.overruns, .chunks = AIR->st.chunks };
	return true;
}

const hal_air_t hrc_air = {
	.fit = air_fit, .open = air_open, .close = air_close, .rx_restart = air_rx_restart, .tx_begin = air_tx_begin,
	.tx_queue = air_tx_queue, .tx_space = air_tx_space, .tx_end = air_tx_end, .tx_abort = air_tx_abort,
	.now_us = air_now_us, .slot_clock = NULL, .slot_slew = NULL, .rx_load = air_rx_load, .rx_counts = air_rx_counts,
	.tx_chunk_syms = HRC7000_AIR_SYMS,
	.tx_queue_depth = HRC7000_AIR_TXQ, .tx_lead_ms = HRC7000_AIR_TX_LEAD_MS, .time_accuracy_us = HRC7000_AIR_TIME_US,
};

HRC_FLASHFUNC static bool tap_start(void)
{
	if (!air_take()) {
		return false;
	}
	bool ok = hrc7000_air_tap_start(AIR);
	if (!ok) {
		air_give();
		return false;
	}
	air_on = true;
	unlock(NULL);
	return true;
}

HRC_FLASHFUNC static uint16_t tap_read(int16_t *buf, uint16_t max) { return air_on ? hrc7000_air_tap_read(AIR, buf, max) : 0; }

HRC_FLASHFUNC static void tap_stop(void)
{
	if (!air_on || !AIR->tap) {
		return;
	}
	lock(NULL);
	hrc7000_air_tap_stop(AIR);
	air_give();
}

HRC_FLASHFUNC static uint32_t tap_time(void) { return air_on ? hrc7000_air_tap_time(AIR) : hrc_micros(); }

// The receive copy on the RF chip's DMR output (modem-hrc7000.md 10.1); no transmit path from the MCU
const hal_baseband_t hrc_baseband = {
	.rx_rate_hz = HRC7000_AIR_RATE_HZ, .rx_channels = 1, .rx_lo_hz = HRC7000_AIR_RX_LO_HZ,
	.rx_hi_hz = HRC7000_AIR_RX_HI_HZ, .rx_counts_per_khz = 0, .rx_start = tap_start, .rx_read = tap_read,
	.rx_stop = tap_stop, .tx_start = NULL, .tx_write = NULL, .tx_stop = NULL, .now_us = air_now_us,
	.rx_time_us = tap_time,
};

HRC_FLASHFUNC void hrc_air_stats(hrc7000_air_stats_t *st)
{
	if (air_on) {
		*st = AIR->st;
	} else {
		memset(st, 0, sizeof(*st));
	}
}
#endif

// --- Bring-up counters (the board's diagnostics page shows these lines first) ---

HRC_FLASHFUNC void hrc_dmr_stats_reset(void)
{
	// Every field before last_rx_type_info is a uint32_t counter (hrc7000_dmr.h); the ISRs update them
	lock(NULL);
	memset(&eng.st, 0, offsetof(hrc7000_dmr_stats_t, last_rx_type_info));
	lost_seen = 0;	// the loss record goes with its counters: health() would show a first loss from before the reset
	lost_ms = 0;
	memset(lost_regs, 0, sizeof lost_regs);
	unlock(NULL);
	hrc_audio_pcm_gate_reset();	// its line is one of ours
}

typedef struct {
	char *p, *end;
} out_t;

HRC_FLASHFUNC static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end) {
		*o->p++ = *s++;
	}
}

HRC_FLASHFUNC static void put_u(out_t *o, uint32_t v)
{
	char t[11];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && n < 10);
	while (n && o->p < o->end) {
		*o->p++ = t[--n];
	}
}

HRC_FLASHFUNC static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xFu];
	}
}

HRC_FLASHFUNC static void put_kv(out_t *o, const char *k, uint32_t v)
{
	put(o, k);
	put_u(o, v);
}

// Lines of at most about 20 characters (the 120-pixel-wide diagnostics page)
HRC_FLASHFUNC bool hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	hrc7000_dmr_stats_t m;
	lock(NULL);
	m = eng.st;
	unlock(NULL);
	out_t o = { buf, buf + len - 1 };
	switch (i) {
	case 0:		// alive: ticks climb by 33 per second once a DMR signal gave the chip timing
		put(&o, active ? "DMR on" : "DMR off");
		put(&o, stack_cb != NULL ? "+cb" : "");
		put_kv(&o, " tk", m.ticks);
		break;
	case 1:
		put_kv(&o, "ts ", m.ts_tx_ints);
		put_kv(&o, "/", m.ts_rx_ints);
		put_kv(&o, " si", m.sys_ints);
		break;
	case 2:
		put_kv(&o, "V", m.rx_voice);
		put_kv(&o, " H", m.rx_lc_header);
		put_kv(&o, " T", m.rx_terminator);
		put_kv(&o, " I", m.rx_idle);
		break;
	case 3:
		put_kv(&o, "C", m.rx_csbk);
		put_kv(&o, " D", m.rx_data);
		put_kv(&o, " crc", m.rx_crc_err);
		put_kv(&o, " u", m.rx_unusable);
		break;
	case 4:
		put_kv(&o, "le", m.late_entry);
		put_kv(&o, " ab", m.slot_abnormal);
		put_kv(&o, " rs", m.rx_restarts);
		put_kv(&o, "/", m.rx_lost);
		put_kv(&o, "/", m.rx_stale);
		put_kv(&o, " h", m.rx_holds);
		put_kv(&o, " st", m.stalls);
		break;
	case 5:		// last burst: CC, TS; raw RX_TYPE_INFO
		put_kv(&o, "cc", m.last_cc);
		put_kv(&o, " ts", (uint32_t)(m.last_ts < 0 ? 0 : m.last_ts));
		put(&o, " ");
		put_x(&o, m.last_rx_type_info, 8);
		break;
	case 6:		// raw SYS_INTERP list and LAYER2 status of the last interrupts
		put_x(&o, m.last_sys_list, 5);
		put(&o, " ");
		put_x(&o, m.last_l2_status, 8);
		put_kv(&o, " k", m.sys_stuck);
		break;
	case 7:
		put_kv(&o, "tx", m.tx_bursts);
		put_kv(&o, " rf", m.rftx_ints);
		put_kv(&o, " pa", m.last_pa_ms);
		put_kv(&o, " s", m.tx_state);
		break;
	case 8:
		put_kv(&o, "un", m.tx_underrun);
		put_kv(&o, " rj", m.tx_rejected);
		put_kv(&o, " ok", m.tx_done);
		put_kv(&o, " ov", m.ev_overflow);
		put_kv(&o, " L", m.rx_in_tx[0]);
		put_kv(&o, "/", m.rx_in_tx[1]);
		break;
	case 9:		// the last TX start's slot: timing (0 own, 1 aligned, 2 ASAP), TS asked, our chip slot; the reception
			// it came from: chip slot, TS, slot ticks since (255 none)
		put_kv(&o, "txb m", m.txb_timing);
		put_kv(&o, " ts", (uint32_t)(m.txb_ts < 0 ? 0 : m.txb_ts));
		put_kv(&o, " c", m.txb_chip_slot);
		put_kv(&o, " r", m.txb_rx_chip_slot);
		put_kv(&o, "/", (uint32_t)(m.txb_last_ts < 0 ? 0 : m.txb_last_ts));
		put_kv(&o, " a", m.txb_rx_age);
		break;
	case 10:	// its reception's raw RX_TYPE_INFO (b22 CACH TC, b19 chip slot); deframe ms into the slot at arming
		put(&o, "txb ");
		put_x(&o, m.txb_rx_type_info, 8);
		put_kv(&o, " d", m.txb_deframe_ms);
		break;
	case 11:	// through a repeater: TC lock count, ticks waited for it; an aligned start's first TX tick: the TC
			// estimate (repeater) or LAYER2_STATUS b31 (SFR) in b0, the next slot ours in b1, and its 0.1 ms after
			// the last receive tick (a multiple of 300 is on the received slot grid: 300 through a repeater, 600
			// for an SFR, whose first TX-vector interrupt skips a slot)
		put_kv(&o, "txh k", m.txb_tc_agree);
		put_kv(&o, " w", m.txb_lock_wait);
		put_kv(&o, " e", m.txa_est);
		put_kv(&o, " u", m.txa_rx_us);
		break;
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
	case 12:	// the split duplex TX half's BS outbound (split-duplex.md 11.6), as the HR_C6000 radios' "bs" line
		put_kv(&o, "bs sl", bs.st.slots);
		put_kv(&o, " b", bs.st.bursts);
		put_kv(&o, " u", bs.st.underruns);
		put_kv(&o, " k", bs.st.keyups);
		put_kv(&o, " c", bs.st.cach_written);
		put_kv(&o, " st", bs.st.stalls);
		break;
	case 13:	// its slot clock: timeslot interrupts and the boundaries they made, PHY_RDY, RF TX, late slots, the
			// longest gap (us)
		put_kv(&o, "bs ti", bs.st.ts_ints);
		put_kv(&o, " tt", bs.st.ts_ticks);
		put_kv(&o, " ph", bs.st.phy_ints);
		put_kv(&o, " ri", bs.st.rftx_ints);
		put_kv(&o, " lt", bs.st.late);
		put_kv(&o, " mg", bs.st.max_gap_us);
		break;
#endif
	case HRC_DMR_DIAG_LINES - 1: {	// the prompt FIFO's TX gate (audio_pcm.c): closings, late (a beep still
					// sounding, its tail cut), ring play on while closed (never); the speaker queue's underruns (never)
		uint32_t g, l, ra, b, ov, un;
		hrc_audio_pcm_gate_stats(&g, &l, &ra);
		hrc_audio_pcm_stats(&b, &ov, &un);
		put_kv(&o, "pg g", g);
		put_kv(&o, " l", l);
		put_kv(&o, " ra", ra);
		put_kv(&o, " un", un);
		break;
	}
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

#endif	// CONFIG_DMR
