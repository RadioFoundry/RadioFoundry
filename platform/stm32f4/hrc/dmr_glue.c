// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HR_C6000 / HR_C5000 DMR glue the STM32F4 radios share (dmr_glue.h): the slot engine on the chip's three
// interrupt lines and the deferred event task that calls the DMR stack (docs/spec/dmr-modem.md 6).
//
// What runs where:
//   EXTI0 (pin 0, timeslot, priority 5)  the board's service hook, drop the PA of the burst that ended, set up the next
//                                        slot (TX burst or receive), slot tick event
//   EXTI1 (pin 1, system, priority 5)    copy out a received burst (status, page 2 payload or the 27-byte voice frame
//                                        over the voice port), send rejected, voice lost; clears the chip's flags
//   EXTI2 (pin 2, RF TX, priority 5)     key the PA for our burst (the board's RF switch)
//   "dmr" task (priority 5, 512 words)   drains the engine's events and calls the stack's callback; every 10 ms the
//                                        watchdog (timing stall, PA left on)
// Priority 5 is configMAX_SYSCALL, so the ISRs may notify the task. Task-level calls into the engine raise BASEPRI to 5
// for the duration of their register work, which keeps the three ISRs out (they share the bit-banged config port).
#include <stddef.h>
#include <string.h>
#include "hrc/dmr_glue.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "FreeRTOS.h"
#include "task.h"
#include "debuglink/dlog.h"
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
#include "modem/hrc_common/hrc6000_bs.h"
#include "dmr/bsOutbound.h"
#endif

#define DMR_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define DMR_TASK_STACK		512	// words: the stack's callback runs on it
#define DMR_POLL_MS		10
// The three lines on pins 0-2 of the board's port: EXTI0-2
#define INT_TS		GPIO_PIN_0
#define INT_SYS		GPIO_PIN_1
#define INT_RFTX	GPIO_PIN_2
#define MODEM_IRQ_PRIORITY	5	// the chips' three lines, one level; must stay >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY

static hrc6000_dmr_t eng ODR_CPU_RAM;	// the slot ISRs' state: CCM, off the DMA bus
static TaskHandle_t task;
static const stm32_hrc_dmr_board_t *hooks;
static volatile bool active;
static dmr_event_cb_t volatile stack_cb;
static void *volatile stack_ctx;

// --- Engine ops ---

static bool in_isr(void)
{
	return __get_IPSR() != 0;
}

// Free-running microseconds from the cycle counter; called at least every 10 ms by the task, so no wrap is missed
static uint32_t now_us(void *ctx)
{
	static uint32_t last_cyc, us, rem;
	UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
	uint32_t c = DWT->CYCCNT;
	uint32_t per_us = SystemCoreClock / 1000000u;
	uint32_t d = (uint32_t)(c - last_cyc) + rem;
	last_cyc = c;
	us += d / per_us;
	rem = d % per_us;
	uint32_t v = us;
	portCLEAR_INTERRUPT_MASK_FROM_ISR(m);
	return v;
}

static UBaseType_t lock_mask;
static uint32_t lock_depth;

static void lock(void *ctx)
{
	UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
	if (lock_depth++ == 0) {
		lock_mask = m;
	}
}

static void unlock(void *ctx)
{
	if (lock_depth > 0 && --lock_depth == 0) {
		portCLEAR_INTERRUPT_MASK_FROM_ISR(lock_mask);
	}
}

static void notify(void *ctx)
{
	if (task == NULL) {
		return;
	}
	if (in_isr()) {
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	} else {
		xTaskNotifyGive(task);
	}
}

static bool rf(void *ctx, bool tx)
{
	return hooks->rf_switch(tx);
}

static bool sys_line_low(void *ctx)
{
	return (hooks->int_port->IDR & INT_SYS) == 0;
}

// For a board with tx_prepare (one synthesizer): the transmission's RF switch to TX is due (stm32_hrc_dmr_tx_prepare): at
// tx_begin for own timing, at the engine's tx_slot_found for an aligned start. Before that the receiver stays on: an
// aligned start waits for the repeater's slot timing, which the HR_C5000 only gives while it receives (issue 108).
// Cleared at TX done
static volatile bool tx_rf_due;

// Timeslot ISR (the modem interrupts are in already): RF to TX two ticks ahead of the first burst, the synthesizer
// locking meanwhile
static void tx_slot_found(void *ctx)
{
	(void)ctx;
	tx_rf_due = true;
	hooks->tx_prepare();
}

// tx_slot_found only for a board with tx_prepare (stm32_hrc_dmr_init): without it the engine starts an aligned
// transmission at the slot, as before
static hrc6000_dmr_ops_t ops = {
	.rf = rf, .now_us = now_us, .lock = lock, .unlock = unlock, .notify = notify, .sys_line_low = sys_line_low,
	.ctx = NULL,
};

// --- Interrupts ---

#define MODEM_INT_PINS	(INT_TS | INT_SYS | INT_RFTX)

static void exti_setup(void)
{
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	stm32_gpio_clock(hooks->int_port);
	GPIO_InitTypeDef g = {
		.Pin = MODEM_INT_PINS, .Mode = GPIO_MODE_IT_FALLING, .Pull = GPIO_PULLUP, .Speed = GPIO_SPEED_FREQ_LOW,
	};
	HAL_GPIO_Init(hooks->int_port, &g);
	HAL_NVIC_SetPriority(EXTI0_IRQn, MODEM_IRQ_PRIORITY, 0);
	HAL_NVIC_SetPriority(EXTI1_IRQn, MODEM_IRQ_PRIORITY, 0);
	HAL_NVIC_SetPriority(EXTI2_IRQn, MODEM_IRQ_PRIORITY, 0);
}

static void exti_enable(bool on)
{
	if (on) {
		__HAL_GPIO_EXTI_CLEAR_IT(MODEM_INT_PINS);
		NVIC_ClearPendingIRQ(EXTI0_IRQn);
		NVIC_ClearPendingIRQ(EXTI1_IRQn);
		NVIC_ClearPendingIRQ(EXTI2_IRQn);
		HAL_NVIC_EnableIRQ(EXTI0_IRQn);
		HAL_NVIC_EnableIRQ(EXTI1_IRQn);
		HAL_NVIC_EnableIRQ(EXTI2_IRQn);
	} else {
		HAL_NVIC_DisableIRQ(EXTI0_IRQn);
		HAL_NVIC_DisableIRQ(EXTI1_IRQn);
		HAL_NVIC_DisableIRQ(EXTI2_IRQn);
	}
}

void EXTI0_IRQHandler(void)
{
	__HAL_GPIO_EXTI_CLEAR_IT(INT_TS);
	if (hooks->ts_service != NULL) {
		hooks->ts_service();
	}
	hrc6000_dmr_isr_timeslot(&eng);
}

void EXTI1_IRQHandler(void)
{
	__HAL_GPIO_EXTI_CLEAR_IT(INT_SYS);
	hrc6000_dmr_isr_sys(&eng);
}

void EXTI2_IRQHandler(void)
{
	__HAL_GPIO_EXTI_CLEAR_IT(INT_RFTX);
	hrc6000_dmr_isr_rftx(&eng);
}

// --- RX trace (debug link log) ---
//
// What the engine delivered, one entry per burst event, in the call stack's trace format (core/dmr/call/dmr_call.c)
// so the two can be diffed:  drx @<slot> <burst><ts><cc>~<ms>[/<slots since the previous entry>] ...
// burst: as the stack's (A-F voice, P H T K M N U R S W I X data by type, L voice lost), lower case when the report
// was not in step (outside 1-12 ms after the timeslot interrupt); cc: the burst's color code (hex); ms: when the
// report came after its timeslot interrupt (? = unknown: serviced at the next timeslot interrupt)

#if CONFIG_DEBUGLINK
#define TRACE_PER_LINE	6
#define TRACE_IDLE_TICKS	10	// flush a part line after this many slots without a burst

static struct {
	bool off;
	char line[88];
	uint8_t len, n;
	uint32_t last_slot;
} tr;

static void tr_put(char c)
{
	if (tr.len < sizeof(tr.line) - 1) {
		tr.line[tr.len++] = c;
	}
}

static void tr_put_u(uint32_t v)
{
	char t[10];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v && n < 10);
	while (n) {
		tr_put(t[--n]);
	}
}

static void tr_flush(void)
{
	if (tr.n) {
		tr.line[tr.len] = '\0';
		dlog("%s", tr.line);
	}
	tr.n = 0;
	tr.len = 0;
}

static void trace_event(const dmr_event_t *ev)
{
	if (tr.off) {
		return;
	}
	if (ev->kind == DMR_EV_SLOT_TICK) {
		if (tr.n && (uint32_t)(ev->arg - tr.last_slot) > TRACE_IDLE_TICKS) {
			tr_flush();
		}
		return;
	}
	static const char data[11] = { 'P', 'H', 'T', 'K', 'M', 'N', 'U', 'R', 'S', 'I', 'W' };
	char burst;
	uint8_t cc = 0;
	uint8_t ms = 255;
	if (ev->kind == DMR_EV_BURST_RX && ev->burst != NULL) {
		const dmr_burst_t *b = ev->burst;
		if (b->kind == DMR_BURST_VOICE) {
			burst = (char)('A' + b->voice_index % 6u);
		} else {
			burst = b->kind < sizeof(data) ? data[b->kind] : 'X';
		}
		if (!b->in_step) {
			burst = (char)(burst - 'A' + 'a');
		}
		cc = b->cc & 0x0Fu;
		ms = hrc6000_dmr_pop_ms(&eng);
	} else if (ev->kind == DMR_EV_VOICE_LOST) {
		burst = 'L';
	} else {
		return;
	}
	if (tr.n == 0) {
		tr.len = 0;
		tr_put('d');
		tr_put('r');
		tr_put('x');
		tr_put(' ');
		tr_put('@');
		tr_put_u(ev->arg);
	}
	tr_put(' ');
	tr_put(burst);
	tr_put((char)('0' + (ev->ts >= 0 && ev->ts <= 2 ? ev->ts : 9)));
	tr_put("0123456789ABCDEF"[cc]);
	if (burst != 'L') {
		tr_put('~');
		if (ms == 255) {
			tr_put('?');
		} else {
			tr_put_u(ms);
		}
	}
	if (tr.n) {
		tr_put('/');
		tr_put_u(ev->arg - tr.last_slot);
	}
	tr.last_slot = ev->arg;
	if (++tr.n >= TRACE_PER_LINE) {
		tr_flush();
	}
}

// The sticky LC trace (hrc6000_dmr.h, hrc6000_lc_trace_t): one line per capture and one per look at page 2 until the
// reception's LC was handed over:
//   dle cap<how> <src>                             how: 0 status report, A lone burst A, T burst taken
//   dle <ev><res> @<ticks since the capture> v<voice bursts taken> <page 2's source ID>
static struct {
	uint16_t seq, serial;
	uint8_t n;
} lt;

static void trace_lc(void)
{
	if (tr.off) {
		return;
	}
	hrc6000_lc_trace_t t[HRC6000_LC_TRACE_N];
	lock(NULL);
	uint16_t serial = eng.st.lc_serial, seq = eng.st.lc_cap_seq;
	uint8_t n = eng.st.lc_n;
	char how = eng.st.lc_cap_how;
	uint32_t cap = eng.st.lc_cap_src;
	memcpy(t, eng.st.lc_tr, sizeof(t));
	unlock(NULL);
	if (serial == lt.serial) {
		return;
	}
	lt.serial = serial;
	if (seq != lt.seq) {
		lt.seq = seq;
		lt.n = 0;
		tr_flush();
		dlog("dle cap%c %u", how != 0 ? how : '?', (unsigned)cap);
	}
	for (; lt.n < n && lt.n < HRC6000_LC_TRACE_N; lt.n++) {
		const hrc6000_lc_trace_t *x = &t[lt.n];
		dlog("dle %c%c @%u v%u %u", x->ev, x->res, x->tk, x->vss, (unsigned)x->src);
	}
}
#endif

void stm32_hrc_dmr_set_trace(bool on)
{
#if CONFIG_DEBUGLINK
	tr.off = !on;
	if (!on) {
		tr.n = 0;
	}
#else
	(void)on;
#endif
}

// --- Deferred event task ---

static void dmr_task(void *arg)
{
	(void)arg;
	dmr_event_t ev;
	dmr_burst_t burst;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DMR_POLL_MS));
		if (active) {
			hrc6000_dmr_poll(&eng);
			if (sys_line_low(NULL)) {
				NVIC_SetPendingIRQ(EXTI1_IRQn);	// a flag still holds the line low: no edge will come
			}
		}
		while (hrc6000_dmr_pop(&eng, &ev, &burst)) {
#if CONFIG_DEBUGLINK
			trace_lc();	// before the burst: an LC looked at when its report was taken
			trace_event(&ev);
#endif
			if (ev.kind == DMR_EV_TX_DONE && hooks->tx_prepare != NULL) {
				lock(NULL);
				tx_rf_due = false;
				if (hooks->tx_done != NULL) {
					hooks->tx_done();
				}
				unlock(NULL);
			}
			dmr_event_cb_t cb = stack_cb;
			if (cb != NULL) {
				cb(&ev, stack_ctx);
			}
		}
	}
}

// --- The board's radio code ---

void stm32_hrc_dmr_init(hrc_t *modem, const stm32_hrc_dmr_board_t *b)
{
	hooks = b;
	ops.tx_slot_found = b->tx_prepare != NULL ? tx_slot_found : NULL;
	hrc6000_dmr_init(&eng, modem, &ops);
	exti_setup();
	exti_enable(false);
	if (task == NULL) {
		xTaskCreate(dmr_task, "dmr", DMR_TASK_STACK, NULL, DMR_TASK_PRIORITY, &task);
	}
}

void stm32_hrc_dmr_enter(void)
{
	hrc6000_dmr_run(&eng);
	active = true;
	exti_enable(true);
}

void stm32_hrc_dmr_leave(void)
{
	exti_enable(false);
	active = false;
	hrc6000_dmr_halt(&eng);
}

void stm32_hrc_dmr_rx_restart(void)
{
	hrc6000_dmr_rx_restart(&eng);
}

void stm32_hrc_dmr_tx_prepare(void)
{
	if (hooks == NULL || hooks->tx_prepare == NULL || !tx_rf_due) {
		return;
	}
	lock(NULL);
	hooks->tx_prepare();
	unlock(NULL);
	if (hooks->tx_wait_lock != NULL) {
		hooks->tx_wait_lock();
	}
}

bool stm32_hrc_dmr_tx_active(void)
{
	return active && hrc6000_dmr_is_transmitting(&eng);
}

void stm32_hrc_dmr_tx_drop(void)
{
	if (!active) {
		return;
	}
	hrc6000_dmr_halt(&eng);		// PA off now, TX_DONE to the stack, engine off ...
	hrc6000_dmr_run(&eng);		// ... and on again, receiving
}

// --- hal_dmr_modem_t ---

static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	if (!active) {
		return false;	// the radio is not in DMR mode (set_mode(RADIO_MODE_DMR) first)
	}
	hrc6000_dmr_configure(&eng, cfg);
	stack_ctx = ctx;
	__DMB();
	stack_cb = cb;
	return true;
}

static void stop(void)
{
	stack_cb = NULL;
	if (active && hrc6000_dmr_is_transmitting(&eng)) {
		stm32_hrc_dmr_tx_drop();
	}
}

static void configure(const dmr_config_t *cfg)
{
	hrc6000_dmr_configure(&eng, cfg);
}

static void rx_restart(void)
{
	hrc6000_dmr_rx_restart(&eng);
}

static void rx_lock_slot(int8_t ts)
{
	hrc6000_dmr_rx_lock_slot(&eng, ts);
}

static bool tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	if (!active) {
		return false;
	}
	tx_rf_due = timing != DMR_TIMING_ALIGNED;
	stm32_hrc_dmr_tx_prepare();
	return hrc6000_dmr_tx_begin(&eng, timing, ts, access);
}

static void tx_listen(void)
{
	hrc6000_dmr_tx_listen(&eng);
}

static int8_t tx_fill(const dmr_tx_burst_t *b)
{
	return hrc6000_dmr_tx_fill(&eng, b);
}

static bool tx_queue(const dmr_tx_burst_t *b)
{
	return hrc6000_dmr_tx_queue(&eng, b);
}

static uint8_t tx_queue_space(void)
{
	return hrc6000_dmr_tx_queue_space(&eng);
}

static void tx_end(void)
{
	hrc6000_dmr_tx_end(&eng);
}

static void tx_abort(void)
{
	hrc6000_dmr_tx_abort(&eng);
}

static void set_embedded_lc(const uint8_t lc[9])
{
	hrc6000_dmr_set_embedded_lc(&eng, lc);
}

static void set_cc_filter(int8_t cc)
{
	hrc6000_dmr_set_cc_filter(&eng, cc);
}

static bool is_transmitting(void)
{
	return hrc6000_dmr_is_transmitting(&eng);
}

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
// --- Split duplex (docs/spec/split-duplex.md 5): the slot clock and the TX half's BS outbound (hrc6000_bs.c) ---

static hrc6000_bs_t bs;

static uint32_t split_now_us(void)
{
	return now_us(NULL);
}

static bool slot_clock(uint32_t *counter, uint32_t *t_us, int8_t *ts)
{
	return active && hrc6000_bs_slot_clock(&eng, counter, t_us, ts);
}

static bool bs_start(const dmr_bs_config_t *cfg)
{
	return active && hrc6000_bs_start(&bs, &eng, cfg, bsOutboundRawSlot);
}

static void bs_key(bool on)
{
	hrc6000_bs_key(&eng, on);
}

static bool bs_queue(int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach, const uint8_t *emb_lc)
{
	return hrc6000_bs_queue(&eng, ts, b, cach, emb_lc);
}

static bool bs_queue_at(uint32_t counter, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
			const uint8_t *emb_lc)
{
	return hrc6000_bs_queue_at(&eng, counter, ts, b, cach, emb_lc);
}

static bool bs_restart(uint32_t t_us, int8_t ts)
{
	return active && hrc6000_bs_restart(&eng, t_us, ts);
}

static uint8_t bs_space(int8_t ts)
{
	return hrc6000_bs_space(&eng, ts);
}

static void bs_stop(void)
{
	if (hrc6000_bs_active(&eng)) {
		hrc6000_bs_stop(&eng);
	}
}

static bool bs_keyed(void)
{
	return hrc6000_bs_keyed(&eng);
}
#define SPLIT_OPS	.now_us = split_now_us, .slot_clock = slot_clock, .bs_start = bs_start, .bs_key = bs_key, \
			.bs_queue = bs_queue, .bs_space = bs_space, .bs_stop = bs_stop, .bs_keyed = bs_keyed, \
			.bs_cach = HRC6000_BS_CACH_METHOD, .bs_queue_at = bs_queue_at, .bs_restart = bs_restart, \
			.bs_air_delay_us = HRC6000_BS_AIR_DELAY_US,
#else
#define SPLIT_OPS
#endif

const hal_dmr_modem_t stm32_hrc_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting, .tx_listen = tx_listen, .tx_fill = tx_fill,
	SPLIT_OPS
};

// --- Bring-up counters ---

// The RF switch counters live in the board's radio code: a reset keeps their values here and reports the difference
static uint32_t rf_base[3];

static void rf_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx)
{
	*skipped = *unarmed = *rx = 0;
	if (hooks != NULL && hooks->rf_counters != NULL) {
		hooks->rf_counters(skipped, unarmed, rx);
	}
}

void stm32_hrc_dmr_stats(stm32_hrc_dmr_stats_t *out)
{
	out->active = active;
	out->stack_attached = stack_cb != NULL;
	hrc6000_dmr_stats(&eng, &out->modem);
	rf_counters(&out->rf_skipped, &out->rf_unarmed, &out->rf_rx);
	out->rf_skipped -= rf_base[0];
	out->rf_unarmed -= rf_base[1];
	out->rf_rx -= rf_base[2];
}

void stm32_hrc_dmr_stats_reset(void)
{
	// Every field before last_r5f is a uint32_t counter (hrc6000_dmr.h); the ISRs update them, so under the lock
	lock(NULL);
	memset(&eng.st, 0, offsetof(hrc6000_dmr_stats_t, last_r5f));
	unlock(NULL);
	rf_counters(&rf_base[0], &rf_base[1], &rf_base[2]);
}

typedef struct {
	char *p, *end;
} out_t;

static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end) {
		*o->p++ = *s++;
	}
}

static void put_c(out_t *o, char c)
{
	if (o->p < o->end) {
		*o->p++ = c;
	}
}

static void put_u(out_t *o, uint32_t v)
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

static void put_x2(out_t *o, uint8_t v)
{
	if (o->p + 2 <= o->end) {
		*o->p++ = "0123456789ABCDEF"[v >> 4];
		*o->p++ = "0123456789ABCDEF"[v & 15u];
	}
}

static void put_kv(out_t *o, const char *k, uint32_t v)
{
	put(o, k);
	put_u(o, v);
}

static void put_four(out_t *o, const char *label, const uint32_t *v)
{
	put(o, label);
	for (int k = 0; k < 4; k++) {
		put(o, " ");
		put_u(o, v[k]);
	}
}

// Lines of at most about 26 characters (the diagnostics page)
bool stm32_hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len)
{
	static const char *const sync_names[] = { "MS", "BS", "T1", "T2", "--" };
	if (len == 0) {
		return false;
	}
	stm32_hrc_dmr_stats_t s;
	stm32_hrc_dmr_stats(&s);
	const hrc6000_dmr_stats_t *m = &s.modem;
	out_t o = { buf, buf + len - 1 };
	switch (i) {
	case 0:		// alive: slot ticks climb by 33 per second once a DMR signal gave the chip timing
		put(&o, s.active ? "DMR on" : "DMR off");
		put(&o, s.stack_attached ? "+cb" : "");
		put_kv(&o, " tk", m->ticks);
		put_kv(&o, " si", m->sys_ints);
		break;
	case 1:		// bursts delivered
		put_kv(&o, "V", m->rx_voice);
		put_kv(&o, " H", m->rx_lc_header);
		put_kv(&o, " T", m->rx_terminator);
		put_kv(&o, " I", m->rx_idle);
		break;
	case 2:
		put_kv(&o, "C", m->rx_csbk);
		put_kv(&o, " D", m->rx_data);
		put_kv(&o, " crc", m->rx_crc_err);
		put_kv(&o, " rc", m->rx_rc);
		break;
	case 3:		// last burst: CC, sync family as decoded, slot, delay; raw 0x5F
		put_kv(&o, "cc", m->last_cc);
		put(&o, " ");
		put(&o, sync_names[m->last_sync >= DMR_SYNC_MS && m->last_sync <= DMR_SYNC_TS2 ? m->last_sync - 1 : 4]);
		put_kv(&o, " ts", (uint32_t)(m->last_ts < 0 ? 0 : m->last_ts));
		put_kv(&o, " @", m->last_report_ms);
		put(&o, " 5F=");
		put_x2(&o, m->last_r5f);
		break;
	case 4:		// raw 0x51 of the last report and of the last non-voice one; 0x42 at the last tick
		put(&o, "51=");
		put_x2(&o, m->last_r51);
		put(&o, " d51=");
		put_x2(&o, m->last_data_r51);
		put(&o, " 42=");
		put_x2(&o, m->last_r42);
		break;
	case 5:		// every report by 0x51 sync class (bits 1-0), before filtering
		put_four(&o, "cls", m->by_class);
		break;
	case 6:		// ... by 0x51 type (bits 7-4): data types 0-8, voice 9-E
	case 7:
	case 8:
	case 9: {
		static const char *const labels[] = { "t0-3", "t4-7", "t8-B", "tC-F" };
		put_four(&o, labels[i - 6], &m->by_type[(i - 6) * 4]);
		break;
	}
	case 10:	// ... by 0x5F bits 1-0 (an HR_C5000 has no 0x5F: all in column 0)
		put_four(&o, "5F", m->by_r5f);
		break;
	case 11:	// ... by delay after the tick: <1 <3 <6 ms (then line 12: <12 <22 >=22); post-access interrupts
		put_kv(&o, "ms ", m->dt_hist[0]);
		put_kv(&o, " ", m->dt_hist[1]);
		put_kv(&o, " ", m->dt_hist[2]);
		put_kv(&o, " le", m->late_entry);
		break;
	case 12:
		put_kv(&o, "ms12 ", m->dt_hist[3]);
		put_kv(&o, " 22 ", m->dt_hist[4]);
		put_kv(&o, " >", m->dt_hist[5]);
		put_kv(&o, " oos", m->rx_out_of_step);
		break;
	case 13:	// dropped duplicates: same tick, off timing, repeat
		put_kv(&o, "dup s", m->rx_dup_slot);
		put_kv(&o, " x", m->rx_dup_extra);
		put_kv(&o, " r", m->rx_dup_repeat);
		put_kv(&o, " ov", m->ev_overflow);
		put_kv(&o, " st", m->stalls);
		break;
	case 14:	// restarts by reason: idle, lost, out of step, cc, command, stall, tx end
		put_kv(&o, "rs i", m->rs_why[HRC6000_RS_IDLE]);
		put_kv(&o, " l", m->rs_why[HRC6000_RS_LOST]);
		put_kv(&o, " o", m->rs_why[HRC6000_RS_OOS]);
		put_kv(&o, " c", m->rs_why[HRC6000_RS_CC]);
		put_kv(&o, " m", m->rs_why[HRC6000_RS_CMD]);
		put_kv(&o, " s", m->rs_why[HRC6000_RS_STALL] + m->rs_why[HRC6000_RS_TX_END]);
		break;
	case 16:	// class 0 reports: no superframe on their slot, stale number, taken by sequence, number skipped
		put_kv(&o, "v0 nc", m->rx_no_call);
		put_kv(&o, " st", m->rx_stale);
		put_kv(&o, " sf", m->rx_seq_fill);
		put_kv(&o, " sj", m->rx_seq_jump);
		break;
	case 17:	// lone voice syncs dropped (no B after); voice CCs replaced (filter / superframe); system interrupts
			// serviced at the timeslot interrupt (line still low, no edge)
		put_kv(&o, "v1 lone", m->rx_lone);
		put_kv(&o, " acc", m->rx_a_cc);
		put_kv(&o, " at", m->sys_at_tick);
		break;
	case 18:	// the sticky LC rule at burst F (no LC yet): new (taken), same as the capture (held back), same
			// but taken by the voice count, CRC error
		put_kv(&o, "lc n=", m->lc_f_new);
		put_kv(&o, " s=", m->lc_f_stale);
		put_kv(&o, " v=", m->lc_f_count);
		put_kv(&o, " c=", m->lc_f_crc);
		break;
	case 19:	// ... a new LC found at the next A, at F's repeated report, at a post-access interrupt; kept ones
			// handed over with the next voice burst
		put_kv(&o, "lc2 a=", m->lc_a_new);
		put_kv(&o, " f=", m->lc_rep_new);
		put_kv(&o, " p=", m->lc_pa_new);
		put_kv(&o, " k=", m->lc_kept);
		break;
	case 20: {	// the last reception's LC trace: capture (how, source ID), then <ev><res><ticks> per look
		put(&o, "lt ");
		put_c(&o, m->lc_cap_how != 0 ? m->lc_cap_how : '?');
		put_u(&o, m->lc_cap_src);
		for (uint8_t k = 0; k < m->lc_n && k < HRC6000_LC_TRACE_N; k++) {
			put_c(&o, ' ');
			put_c(&o, m->lc_tr[k].ev);
			put_c(&o, m->lc_tr[k].res);
			put_u(&o, m->lc_tr[k].tk);
		}
		break;
	}
	case 21:	// the split duplex TX half's BS outbound (split-duplex.md 11.6)
#if CONFIG_HOTSPOT_SPLIT
		put_kv(&o, "bs sl", bs.st.slots);
		put_kv(&o, " b", bs.st.bursts);
		put_kv(&o, " u", bs.st.underruns);
		put_kv(&o, " k", bs.st.keyups);
		put_kv(&o, " c", bs.st.cach_written);
		put_kv(&o, " rf", bs.st.rf_fail);
		put_kv(&o, " st", bs.st.stalls);
		put_kv(&o, " rs", bs.st.restarts);
		put_kv(&o, " ri", bs.st.rs_ignored);
		put_kv(&o, " r1", (uint32_t)bs.st.rs_dt[0]);
		put_kv(&o, " r2", (uint32_t)bs.st.rs_dt[1]);
		put_kv(&o, " r3", (uint32_t)bs.st.rs_dt[2]);
#else
		put(&o, "bs -");
#endif
		break;
	case 15:
		put_kv(&o, "tx", m->tx_bursts);
		put_kv(&o, " rf", m->rftx_ints);
		put_kv(&o, " pa", m->last_pa_ms);
		put_kv(&o, " rj", m->tx_rejected);
		put_kv(&o, " sk", s.rf_skipped);
		put_kv(&o, " na", s.rf_unarmed);
		put_kv(&o, " rb", s.rf_rx);
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

#endif	// CONFIG_DMR
