// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 DMR transmit with a whole RF key-up and key-down per burst (dmr_rfburst.h). Compiled only with CONFIG_DMR
#include "dmr_rfburst.h"

#if CONFIG_DMR
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define RF_TASK_PRIORITY	(configMAX_PRIORITIES - 1)	// above the glue's DMR task: the burst timing comes first
#define RF_TASK_STACK		256		// words
#define RF_GUARD_US		20000u		// an RF RX interrupt ends the burst only this long after its RF TX interrupt
#define WAIT_SPIN_US		50u		// shorter waits spin, as the stock delay routine does
#define WAIT_TIMEOUT_MS		10u		// the timer never fired: go on
#define DISARM_WAIT_MS		20u

#define EV_ON		0x01u	// RF TX interrupt: the burst's TX on
#define EV_OFF_TICK	0x02u	// the engine ends the burst (slot tick 20 ms on, or a halt)
#define EV_OFF_RFRX	0x04u	// RF RX interrupt after the burst: the stock firmware's TX-off point
#define EV_ABORT	0x08u	// disarm: TX off without the hold
#define EV_TIMER	0x10u	// TIMER_BOARD expired

static hrc7000_t *modem;
static const hrc_rfburst_board_t *board;
static TaskHandle_t rf_task;
static volatile bool keyed;		// armed: an RF TX interrupt starts a burst
static volatile bool rf_tx;		// the RF is in its TX state (written by the task)
static volatile bool rf_busy;		// the task is running a sequence
static volatile uint32_t t_on;		// hrc_micros() at the burst's RF TX interrupt
static uint32_t pending;		// events that came while the task waited for its timer
static hrc_rfburst_stats_t stats;

static void rf_post(uint32_t ev)
{
	if (rf_task == NULL) {
		return;
	}
	if (hrc_in_isr()) {
		BaseType_t woken = pdFALSE;
		xTaskNotifyFromISR(rf_task, ev, eSetBits, &woken);
		portYIELD_FROM_ISR(woken);
	} else {
		xTaskNotify(rf_task, ev, eSetBits);
	}
}

// From the modem interrupts, per burst (ISR, or task level for a halt): the RF TX interrupt's time and a wake for the
// TX on, or a wake for the TX off. Disarmed, nothing is keyed and there is nothing to switch
void hrc_rfburst_switch(bool tx)
{
	if (tx) {
		if (keyed) {
			t_on = hrc_micros();
			rf_post(EV_ON);
		}
	} else {
		rf_post(EV_OFF_TICK);
	}
}

// RF RX interrupt (ISR). The stock firmware ends each burst here; RadioFoundry's engine ends it at the next slot tick too, so
// whichever comes first wakes the TX-off, and the board's hold in it keeps the stock timing either way
static void isr_rfrx(void)
{
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_RF_RX);
	stats.rfrx_ints++;
	if (rf_tx && hrc_micros() - t_on >= RF_GUARD_US) {
		rf_post(EV_OFF_RFRX);
	}
}

static void isr_timer(void)
{
	hrc_timer_ack(TIMER_BOARD);
	hrc_timer_stop(TIMER_BOARD);
	rf_post(EV_TIMER);
}

// The sequences' waits: a spin below 50 us, else the board timer while the task sleeps (stock: the same split)
static void rf_wait(uint32_t us)
{
	if (us < WAIT_SPIN_US) {
		if (us != 0) {
			hal_delay_us(us);
		}
		return;
	}
	hrc_timer_oneshot(TIMER_BOARD, us);
	uint32_t v;
	while (xTaskNotifyWait(0, UINT32_MAX, &v, pdMS_TO_TICKS(WAIT_TIMEOUT_MS) + 1u) == pdTRUE) {
		pending |= v & ~EV_TIMER;
		if ((v & EV_TIMER) != 0) {
			return;
		}
	}
	hrc_timer_stop(TIMER_BOARD);
}

static void rf_task_fn(void *arg)
{
	(void)arg;
	for (;;) {
		uint32_t v = pending;
		pending = 0;
		if (v == 0) {
			xTaskNotifyWait(0, UINT32_MAX, &v, portMAX_DELAY);
		}
		rf_busy = true;
		if ((v & EV_ON) != 0 && keyed && !rf_tx) {
			board->on(rf_wait);
			rf_tx = true;
			stats.bursts++;
		}
		if ((v & (EV_OFF_TICK | EV_OFF_RFRX | EV_ABORT)) != 0 && rf_tx) {
			bool now = (v & EV_ABORT) != 0;
			uint32_t late = board->off(rf_wait, hrc_micros() - t_on, now);
			rf_tx = false;
			stats.ends++;
			stats.last_by = now ? 'A' : (v & EV_OFF_RFRX) != 0 ? 'R' : 'T';
			if (late != 0) {
				stats.late++;
				stats.last_late_us = late;
			}
		}
		rf_busy = false;
	}
}

void hrc_rfburst_init(hrc7000_t *m, const hrc_rfburst_board_t *b)
{
	modem = m;
	board = b;
	hrc_irq_register(IRQ_TIMER5, isr_timer);
	hrc_irq_enable(IRQ_TIMER5, true);
	hrc_irq_register(IRQ_MODEM_RF_RX, isr_rfrx);	// enabled while armed
	if (rf_task == NULL) {
		xTaskCreate(rf_task_fn, "dmrrf", RF_TASK_STACK, NULL, RF_TASK_PRIORITY, &rf_task);
	}
}

void hrc_rfburst_arm(void)
{
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_RF_RX);
	keyed = true;
	hrc_irq_enable(IRQ_MODEM_RF_RX, true);
}

void hrc_rfburst_disarm(void)
{
	keyed = false;
	rf_post(EV_ABORT);	// the task outranks the caller: a burst not in a wait is done when this returns
	for (uint32_t i = 0; i < DISARM_WAIT_MS && (rf_tx || rf_busy); i++) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	hrc_irq_enable(IRQ_MODEM_RF_RX, false);
}

bool hrc_rfburst_armed(void)
{
	return keyed;
}

void hrc_rfburst_stats(hrc_rfburst_stats_t *st)
{
	*st = stats;
}
#endif	// CONFIG_DMR
