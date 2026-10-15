// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 DMR modem glue (dmr_glue.h): the slot engine (drivers/modem/hrc7000/hrc7000_dmr.c) on the SoC's modem
// interrupts, the DMR PA one-shot on Timer 3, and the deferred event task that calls the DMR stack (docs/spec/dmr-modem.md
// 6, modem-hrc7000.md 3, hardware-dm32.md 8.5, 8.6).
//
// What runs where (all interrupts at one level, never nested; this file and the engine run from RAM,
// the board's linker script):
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
#include "FreeRTOS.h"
#include "task.h"

#define DMR_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define DMR_TASK_STACK		512	// words: the stack's callback runs on it
#define DMR_POLL_MS		10

static hrc7000_dmr_t eng;
static const hrc_dmr_board_t *brd;
static TaskHandle_t task;
static volatile bool active;
static dmr_event_cb_t volatile stack_cb;
static void *volatile stack_ctx;

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

static void rf(void *ctx, bool tx)
{
	(void)ctx;
	brd->rf_switch(tx);
}

static void pa_arm(void *ctx)
{
	(void)ctx;
	hrc_timer_oneshot(TIMER_PA, brd->pa_delay_us);
}

static const hrc7000_dmr_ops_t ops = {
	.rf = rf, .pa_arm = pa_arm, .now_us = now_us, .lock = lock, .unlock = unlock, .notify = notify, .ctx = NULL,
};

// --- Interrupts ---

static void isr_sys(void) { hrc7000_dmr_isr_sys(&eng); }
static void isr_ts_tx(void) { hrc7000_dmr_isr_timeslot(&eng, true); }
static void isr_ts_rx(void) { hrc7000_dmr_isr_timeslot(&eng, false); }
static void isr_rftx(void) { hrc7000_dmr_isr_rftx(&eng); }

static void isr_pa_timer(void)
{
	hrc_timer_ack(TIMER_PA);
	hrc_timer_stop(TIMER_PA);
	if (active) {
		brd->pa_on();
	}
}

static void irqs_enable(bool on)
{
	hrc_irq_enable(IRQ_MODEM_SYS, on);
	hrc_irq_enable(IRQ_MODEM_TS_TX, on);
	hrc_irq_enable(IRQ_MODEM_TS_RX, on);
	hrc_irq_enable(IRQ_MODEM_RF_TX, on);
	if (!on) {
		hrc_timer_stop(TIMER_PA);
	}
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
			hrc7000_dmr_poll(&eng);
		}
		while (hrc7000_dmr_pop(&eng, &ev, &burst)) {
			dmr_event_cb_t cb = stack_cb;
			if (cb != NULL) {
				cb(&ev, stack_ctx);
			}
		}
	}
}

// --- The radio's interface ---

void hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks)
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
	if (task == NULL) {
		xTaskCreate(dmr_task, "dmr", DMR_TASK_STACK, NULL, DMR_TASK_PRIORITY, &task);
	}
}

void hrc_dmr_enter(void)
{
	hrc7000_dmr_run(&eng);
	active = true;
	irqs_enable(true);
}

void hrc_dmr_leave(void)
{
	irqs_enable(false);
	active = false;
	hrc7000_dmr_halt(&eng);
}

void hrc_dmr_rx_restart(void)
{
	hrc7000_dmr_rx_restart(&eng);
}

bool hrc_dmr_tx_active(void)
{
	return active && hrc7000_dmr_is_transmitting(&eng);
}

void hrc_dmr_tx_drop(void)
{
	if (!active) {
		return;
	}
	hrc7000_dmr_halt(&eng);		// PA off now, TX_DONE to the stack, engine off ...
	hrc7000_dmr_run(&eng);		// ... and on again, receiving
}

// --- hal_dmr_modem_t ---

static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	if (!active) {
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
	if (active && hrc7000_dmr_is_transmitting(&eng)) {
		hrc_dmr_tx_drop();
	}
}

static void configure(const dmr_config_t *cfg) { hrc7000_dmr_configure(&eng, cfg); }
static void rx_restart(void) { hrc7000_dmr_rx_restart(&eng); }
static void rx_lock_slot(int8_t ts) { hrc7000_dmr_rx_lock_slot(&eng, ts); }

static bool tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	return active && hrc7000_dmr_tx_begin(&eng, timing, ts, access);
}

static bool tx_queue(const dmr_tx_burst_t *b) { return hrc7000_dmr_tx_queue(&eng, b); }
static uint8_t tx_queue_space(void) { return hrc7000_dmr_tx_queue_space(&eng); }
static void tx_end(void) { hrc7000_dmr_tx_end(&eng); }
static void tx_abort(void) { hrc7000_dmr_tx_abort(&eng); }
static void set_embedded_lc(const uint8_t lc[9]) { hrc7000_dmr_set_embedded_lc(&eng, lc); }
static void set_cc_filter(int8_t cc) { hrc7000_dmr_set_cc_filter(&eng, cc); }
static bool is_transmitting(void) { return hrc7000_dmr_is_transmitting(&eng); }

const hal_dmr_modem_t hrc_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
};

// --- Bring-up counters (the board's diagnostics page shows these lines first) ---

void hrc_dmr_stats_reset(void)
{
	// Every field before last_rx_type_info is a uint32_t counter (hrc7000_dmr.h); the ISRs update them
	lock(NULL);
	memset(&eng.st, 0, offsetof(hrc7000_dmr_stats_t, last_rx_type_info));
	unlock(NULL);
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

static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xFu];
	}
}

static void put_kv(out_t *o, const char *k, uint32_t v)
{
	put(o, k);
	put_u(o, v);
}

// Lines of at most about 20 characters (the 120-pixel-wide diagnostics page)
bool hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len)
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
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

#endif	// CONFIG_DMR
