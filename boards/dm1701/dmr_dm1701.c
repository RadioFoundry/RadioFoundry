// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 DMR modem: the HR_C6000 slot engine (drivers/modem/hrc6000/hrc6000_dmr.c) on the board's three modem
// interrupt lines, with the deferred event task that calls the DMR stack (docs/spec/dmr-modem.md 6,
// docs/spec/hardware-dm1701.md 3.1, 4, 6).
//
// What runs where:
//   EXTI0 (PC0, timeslot, priority 5)  retry a skipped RF back-to-RX write, drop the PA of the burst that ended, set
//                                      up the next slot (TX burst or receive), slot tick event
//   EXTI1 (PC1, system, priority 5)    copy out a received burst (status, page 2 payload or the 27-byte voice frame
//                                      over SPI2), send rejected, voice lost; clears the chip's flags
//   EXTI2 (PC2, RF TX, priority 5)     key the PA for our burst (AT1846S reg 0x30 over I2C3, then the PA pins)
//   "dmr" task (priority 5, 512 words) drains the engine's events and calls the stack's callback; every 10 ms the
//                                      watchdog (timing stall, PA left on)
// Priority 5 is configMAX_SYSCALL, so the ISRs may notify the task. Task-level calls into the engine raise BASEPRI to 5
// for the duration of their register work, which keeps the three ISRs out (they share the bit-banged config port).
#include <stddef.h>
#include <string.h>
#include "dm1701.h"
#include "dmr_dm1701.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "pins.h"
#include "FreeRTOS.h"
#include "task.h"

#define DMR_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define DMR_TASK_STACK		512	// words: the stack's callback runs on it
#define DMR_POLL_MS		10
#define MODEM_IRQ_PRIORITY	5	// hardware-dm1701.md 3.1; must stay >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY

static hrc6000_dmr_t eng;
static TaskHandle_t task;
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
	return dm1701_rf_dmr_switch(tx);
}

static bool sys_line_low(void *ctx)
{
	return !PIN_READ(PIN_C6000_SYS_INT);
}

static const hrc6000_dmr_ops_t ops = {
	.rf = rf, .now_us = now_us, .lock = lock, .unlock = unlock, .notify = notify, .sys_line_low = sys_line_low,
	.ctx = NULL,
};

// --- Interrupts ---

#define MODEM_INT_PINS	(PIN_MASK(PIN_C6000_TS_INT) | PIN_MASK(PIN_C6000_SYS_INT) | PIN_MASK(PIN_C6000_RFTX_INT))

static void exti_setup(void)
{
	__HAL_RCC_SYSCFG_CLK_ENABLE();
	stm32_gpio_clock(PIN_PORT(PIN_C6000_TS_INT));
	GPIO_InitTypeDef g = {
		.Pin = MODEM_INT_PINS, .Mode = GPIO_MODE_IT_FALLING, .Pull = GPIO_PULLUP, .Speed = GPIO_SPEED_FREQ_LOW,
	};
	HAL_GPIO_Init(PIN_PORT(PIN_C6000_TS_INT), &g);
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
	__HAL_GPIO_EXTI_CLEAR_IT(PIN_MASK(PIN_C6000_TS_INT));
	dm1701_rf_dmr_service();
	hrc6000_dmr_isr_timeslot(&eng);
}

void EXTI1_IRQHandler(void)
{
	__HAL_GPIO_EXTI_CLEAR_IT(PIN_MASK(PIN_C6000_SYS_INT));
	hrc6000_dmr_isr_sys(&eng);
}

void EXTI2_IRQHandler(void)
{
	__HAL_GPIO_EXTI_CLEAR_IT(PIN_MASK(PIN_C6000_RFTX_INT));
	hrc6000_dmr_isr_rftx(&eng);
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
			if (!PIN_READ(PIN_C6000_SYS_INT)) {
				NVIC_SetPendingIRQ(EXTI1_IRQn);	// a flag still holds the line low: no edge will come
			}
		}
		while (hrc6000_dmr_pop(&eng, &ev, &burst)) {
			dmr_event_cb_t cb = stack_cb;
			if (cb != NULL) {
				cb(&ev, stack_ctx);
			}
		}
	}
}

// --- radio_dm1701.c interface ---

void dm1701_dmr_board_init(hrc6000_t *modem)
{
	hrc6000_dmr_init(&eng, modem, &ops);
	exti_setup();
	exti_enable(false);
	if (task == NULL) {
		xTaskCreate(dmr_task, "dmr", DMR_TASK_STACK, NULL, DMR_TASK_PRIORITY, &task);
	}
}

void dm1701_dmr_enter(void)
{
	hrc6000_dmr_run(&eng);
	active = true;
	exti_enable(true);
}

void dm1701_dmr_leave(void)
{
	exti_enable(false);
	active = false;
	hrc6000_dmr_halt(&eng);
}

void dm1701_dmr_rx_restart(void)
{
	hrc6000_dmr_rx_restart(&eng);
}

bool dm1701_dmr_tx_active(void)
{
	return active && hrc6000_dmr_is_transmitting(&eng);
}

void dm1701_dmr_tx_drop(void)
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
		dm1701_dmr_tx_drop();
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
	return active && hrc6000_dmr_tx_begin(&eng, timing, ts, access);
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

const hal_dmr_modem_t dm1701_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
};

// --- Bring-up counters ---

// The RF switch counters live in radio_dm1701.c: a reset keeps their values here and reports the difference
static uint32_t rf_base[3];

void dm1701_dmr_stats(dm1701_dmr_stats_t *out)
{
	out->active = active;
	out->stack_attached = stack_cb != NULL;
	hrc6000_dmr_stats(&eng, &out->modem);
	dm1701_rf_dmr_counters(&out->rf_skipped, &out->rf_unarmed, &out->rf_rx_retries);
	out->rf_skipped -= rf_base[0];
	out->rf_unarmed -= rf_base[1];
	out->rf_rx_retries -= rf_base[2];
}

void dm1701_dmr_stats_reset(void)
{
	// Every field before last_r5f is a uint32_t counter (hrc6000_dmr.h); the ISRs update them, so under the lock
	lock(NULL);
	memset(&eng.st, 0, offsetof(hrc6000_dmr_stats_t, last_r5f));
	unlock(NULL);
	dm1701_rf_dmr_counters(&rf_base[0], &rf_base[1], &rf_base[2]);
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

// Lines of at most about 26 characters (the DM-1701 diagnostics page)
bool dm1701_dmr_stats_line(uint8_t i, char *buf, uint8_t len)
{
	static const char *const sync_names[] = { "MS", "BS", "T1", "T2", "--" };
	if (len == 0) {
		return false;
	}
	dm1701_dmr_stats_t s;
	dm1701_dmr_stats(&s);
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
	case 10:	// ... by 0x5F bits 1-0 (expected for the HAT's MS sync: all in column 0)
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
	case 15:
		put_kv(&o, "tx", m->tx_bursts);
		put_kv(&o, " rf", m->rftx_ints);
		put_kv(&o, " pa", m->last_pa_ms);
		put_kv(&o, " rj", m->tx_rejected);
		put_kv(&o, " sk", s.rf_skipped);
		put_kv(&o, " na", s.rf_unarmed);
		break;
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

#endif	// CONFIG_DMR
