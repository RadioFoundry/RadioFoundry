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
#define WAIT_SPIN_US		50u		// shorter waits spin, as the original firmware's delay routine does
#define WAIT_TIMEOUT_MS		10u		// the timer never fired this long after it should have: go on
#define DISARM_WAIT_MS		20u
#define FRAME_US		60000u		// one RF TX interrupt per TDMA frame while transmitting
#define GRID_SLACK_US		30u		// an RF TX interrupt later than this off the frame grid was held up

#define EV_ON		0x01u	// RF TX interrupt: the burst's TX on
#define EV_OFF_TICK	0x02u	// the engine ends the burst (slot tick 20 ms on, or a halt)
#define EV_OFF_RFRX	0x04u	// RF RX interrupt after the burst: the original firmware's TX-off point
#define EV_ABORT	0x08u	// disarm: TX off without the hold
#define EV_TIMER	0x10u	// TIMER_BOARD expired

static hrc7000_t *modem;
static const hrc_rfburst_board_t *board;
static TaskHandle_t rf_task;
static volatile bool keyed;		// armed: an RF TX interrupt starts a burst
static volatile bool rf_tx;		// the RF is in its TX state (written by the task)
static volatile bool rf_busy;		// the task is running a sequence
static volatile bool cont;		// the keyed outbound is continuous (a split hotspot's base station)
static volatile uint32_t t_on;		// hrc_micros() at the burst's RF TX interrupt
static uint32_t pending;		// events that came while the task waited for its timer
static uint32_t t_on_prev;		// the burst before's t_on, for the grid check (0: none since the arm)
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
void hrc_rfburst_switch(bool tx, bool continuous)
{
	if (tx) {
		if (keyed) {
			cont = continuous;
			t_on = hrc_micros();
			rf_post(EV_ON);
		}
	} else {
		rf_post(EV_OFF_TICK);
	}
}

// RF RX interrupt (ISR). The original firmware ends each burst here; RadioFoundry's engine ends it at the next slot tick too, so
// whichever comes first wakes the TX-off, and the board's hold in it keeps the original firmware's timing either way
static void isr_rfrx(void)
{
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_RF_RX);
	stats.rfrx_ints++;
	if (rf_tx && !cont && hrc_micros() - t_on >= RF_GUARD_US) {
		rf_post(EV_OFF_RFRX);
	}
}

static void isr_timer(void)
{
	hrc_timer_ack(TIMER_BOARD);
	hrc_timer_stop(TIMER_BOARD);
	rf_post(EV_TIMER);
}

// The sequences' waits: a spin below 50 us, else the board timer while the task sleeps (the original firmware: the same split)
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
	// The time-out is the wait plus WAIT_TIMEOUT_MS: a hold to a burst's last symbol can be longer than 10 ms
	TickType_t limit = pdMS_TO_TICKS(us / 1000u + WAIT_TIMEOUT_MS) + 1u;
	while (xTaskNotifyWait(0, UINT32_MAX, &v, limit) == pdTRUE) {
		pending |= v & ~EV_TIMER;
		if ((v & EV_TIMER) != 0) {
			return;
		}
	}
	hrc_timer_stop(TIMER_BOARD);
}

// The TX-on timing (hrc_rfburst_stats_t)
static void note_burst(uint32_t t0, uint32_t wake, uint32_t up)
{
	if (wake > stats.wake_max_us) {
		stats.wake_max_us = wake;
	}
	if (stats.up_min_us == 0 || up < stats.up_min_us) {
		stats.up_min_us = up;
	}
	if (up > stats.up_max_us) {
		stats.up_max_us = up;
	}
	t_on_prev = t0;
}

// The burst's RF TX interrupt time. The modem's interrupts are masked while the slot engine holds its lock, so the
// interrupt can come a few hundred us late, and the key-up and the hold to the burst's end with it. The bursts keep
// the frame grid (on the SDR bench: 60 ms, sd 25 us), so a stamp later than the grid's point by more than the slack is
// taken back to it. The first burst after the arm has no grid yet
static uint32_t on_time(uint32_t stamp)
{
	uint32_t d = stamp - t_on_prev;
	if (t_on_prev == 0 || d <= FRAME_US / 2 || d >= FRAME_US * 3 / 2) {
		return stamp;
	}
	uint32_t off = d > FRAME_US ? d - FRAME_US : FRAME_US - d;
	if (off > stats.grid_max_us) {
		stats.grid_max_us = off;
	}
	if (d > FRAME_US + GRID_SLACK_US) {
		stats.regridded++;
		return t_on_prev + FRAME_US;
	}
	return stamp;
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
			uint32_t t0 = on_time(t_on);
			t_on = t0;
			uint32_t wake = hrc_micros() - t0;
			board->on(rf_wait, wake);
			uint32_t up = hrc_micros() - t0;
			rf_tx = true;
			stats.bursts++;
			note_burst(t0, wake, up);
		}
		if ((v & (EV_OFF_TICK | EV_OFF_RFRX | EV_ABORT)) != 0 && rf_tx) {
			bool abort = (v & EV_ABORT) != 0;
			bool now = abort || cont;	// a continuous outbound ends when it stops: nothing to hold for
			uint32_t late = board->off(rf_wait, hrc_micros() - t_on, now);
			rf_tx = false;
			stats.ends++;
			stats.last_by = abort ? 'A' : cont ? 'B' : (v & EV_OFF_RFRX) != 0 ? 'R' : 'T';
			cont = false;
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
	t_on_prev = 0;
	stats.wake_max_us = stats.up_min_us = stats.up_max_us = stats.grid_max_us = stats.regridded = 0;
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
