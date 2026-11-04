// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DB25-D DMR: the HR_C7000's modem on the shared glue (platform/hrc7000/dmr_glue.c) with the main transceiver (BK4819
// or FD6818, rfchip_db25d.c) as the RF front end (docs/spec/hardware-db25d.md 4.5, 9).
//
// Transmit only with make DB25D_DMR_TX=1 (off by default until checked on air); without it tx() refuses in DMR mode
// and the glue's RF hooks do nothing. With it the stock firmware's scheme (4.5): the modem's 4FSK goes on the
// reference oscillator, and every burst runs the whole TX sequence of the analog transmitter, keyed from the modem's
// RF TX interrupt (3.1 ms ahead of the burst) and ended at its RF RX interrupt plus the page's TX-off delay. The
// sequences take a few milliseconds with waits in them, so they run in a task of their own at the highest priority,
// as the stock firmware's "rfint" task does; the interrupts only wake it.
//
// Compiled only with CONFIG_DMR (off by default on this board).
#include "db25d.h"
#include "dmr_db25d.h"
#include "rfchip_db25d.h"
#include "config.h"
#include "pins.h"
#include "platform.h"

#if CONFIG_DMR
#if DB25D_DMR_TX
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#endif

#define MIC_GAIN_DB		DB25D_MIC_GAIN_DB
#define DMR_PA_DELAY_US		28000u	// the glue's PA one-shot; the PA is keyed by the burst sequence instead
#define C7_PHASE_OUT		0x128u	// RO: largest / smallest 14-bit AF phase seen (modem-hrc7000.md 2.1)

// The modem's receive mode word as the stock firmware builds it for this board (hardware-db25d.md 4.5): AF receive with
// averaging, AF gain 0x4C (4.75; the other HR_C7000 boards' word has 0x26), RX IQ inverted, dynamic sync threshold, dual receive
// channels, two-point TX. make DB25D_DMR_RF_MODE=0x... replaces it on the bench
#ifndef DB25D_DMR_RF_MODE
#define DB25D_DMR_RF_MODE	0x034C9860u
#endif
// The main chip's AF DAC gain in DMR (0x48 bits 3:0): the modem's input level, whatever the speaker volume. The stock
// digital set's value is 4. make DB25D_DMR_AF_GAIN=N replaces it
#ifndef DB25D_DMR_AF_GAIN
#define DB25D_DMR_AF_GAIN	4u
#endif

static hrc7000_t *modem;

#if DB25D_DMR_TX
static void rf_post(uint32_t ev);
static void rf_burst_start(void);
#define EV_ON		0x01u	// RF TX interrupt: the burst's TX on
#define EV_OFF_TICK	0x02u	// the engine ends the burst (slot tick 20 ms on, or a halt)
#define EV_OFF_RFRX	0x04u	// RF RX interrupt after the burst: the stock firmware's TX-off point
#define EV_ABORT	0x08u	// disarm: TX off without the hold
#define EV_TIMER	0x10u	// TIMER_BOARD expired
#endif

// From the modem interrupts, per burst (ISR, or task level for a halt): wake the RF task for our burst's TX on, or for
// its TX off. Without DB25D_DMR_TX nothing is armed and there is nothing to switch
static void dmr_rf_switch(bool tx)
{
#if DB25D_DMR_TX
	if (tx) {
		rf_burst_start();
	} else {
		rf_post(EV_OFF_TICK);
	}
#else
	(void)tx;
#endif
}

// The PA one-shot: nothing. The burst sequence keys the PA (stock: no PA timer on this board)
static void dmr_pa_on(void)
{
}

static const hrc_dmr_board_t dmr_board = {
	.rf_switch = dmr_rf_switch, .pa_on = dmr_pa_on, .pa_delay_us = DMR_PA_DELAY_US,
};

// The speaker source for the modem's PCM (audio_pcm.c): the ring (prompt FIFO) on, or off. The modem's FM audio path
// isn't used on this board (the main chip's audio goes by its own route, 3.3), so it stays off. The modem's line out
// reaches the speaker amplifier with PTC18 low (radio_db25d.c speaker_pins())
static void speaker_pcm(bool on)
{
	// beep.c repeats the call every block: rewriting AUDIO_CONTROL 50 times a second made its tones scratchy
	static int8_t last = -1;
	if (last == (int8_t)on) {
		return;
	}
	last = (int8_t)on;
	uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
	if (on) {
		hrc7000_audio_bits(modem, C7_AUDIO_RING_PLAY, C7_AUDIO_FM_PLAY);
	} else {
		hrc7000_audio_bits(modem, 0, C7_AUDIO_RING_PLAY | C7_AUDIO_FM_PLAY);
	}
	hrc_irq_restore(s);
	db25d_speaker_pcm(on);	// the amplifier and its source: the squelch path never opens for DMR voice
}

#if DB25D_DMR_TX
static void dmr_tx_init(void);
#endif

void db25d_dmr_init(hrc7000_t *m)
{
	modem = m;	// attached and powered on by the radio init (modem-hrc7000.md 4 steps 4-32)
	hrc_dmr_board_init(modem, &dmr_board);
	hrc_audio_pcm_attach(modem, speaker_pcm);
#if DB25D_DMR_TX
	dmr_tx_init();
#endif
}

void db25d_dmr_enter(uint32_t rx_hz)
{
	db25d_main_idle();
	db25d_main_dmr_enter(DB25D_DMR_AF_GAIN);
	hrc7000_dmr_enter(modem, hrc7000_mod_bias_dc(db25d_cal_tx_center(), db25d_cal_mod(rx_hz)), MIC_GAIN_DB);
	hrc7000_wr(modem, C7_RF_MODE, DB25D_DMR_RF_MODE);	// the stock writes it once at start-up; it only acts in DMR
	hrc_dmr_enter();
}

void db25d_dmr_leave(void)
{
	hrc_dmr_leave();
	hrc7000_fm_idle(modem);
	db25d_main_idle();
	db25d_main_dmr_leave();
}

uint32_t db25d_dmr_phase(void)
{
	return hrc7000_rd(modem, C7_PHASE_OUT);
}

#if DB25D_DMR_TX
// ---- DMR transmit: the RF task (hardware-db25d.md 4.5) ----

#define RF_TASK_PRIORITY	(configMAX_PRIORITIES - 1)	// above the glue's DMR task: the burst timing comes first
#define RF_TASK_STACK		256		// words
#define RF_GUARD_US		20000u		// an RF RX interrupt ends the burst only this long after its RF TX interrupt
#define WAIT_SPIN_US		50u		// shorter waits spin, as the stock delay routine does
#define WAIT_TIMEOUT_MS		10u		// the timer never fired: go on
#define DISARM_WAIT_MS		20u
#define RF_MODE_TX_INVERT	(1u << 7)	// RF_MODE rf_transiq_mode: the symbol-to-deviation mapping inverted
// Stock: RF_MODE bit 7 clear on this board (the other HR_C7000 boards' value has it set). make DB25D_DMR_TX_INVERT=1 sets it, to
// try the other 4FSK polarity on the bench
#ifndef DB25D_DMR_TX_INVERT
#define DB25D_DMR_TX_INVERT	0
#endif

static TaskHandle_t rf_task;
static volatile bool keyed;		// armed: an RF TX interrupt starts a burst
static volatile bool rf_tx;		// the RF is in its TX state (written by the task)
static volatile bool rf_busy;		// the task is running a sequence
static volatile uint32_t t_on;		// hrc_micros() at the burst's RF TX interrupt
static uint32_t pending;		// events that came while the task waited for its timer
static db25d_dmr_tx_stats_t stats;

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

// RF TX interrupt (ISR): the burst's start time, then the task
static void rf_burst_start(void)
{
	if (keyed) {
		t_on = hrc_micros();
		rf_post(EV_ON);
	}
}

// RF RX interrupt (ISR). The stock firmware ends each burst here; RadioFoundry's engine ends it at the next slot tick too, so
// whichever comes first wakes the TX-off, and the hold in it keeps the stock timing either way
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
			db25d_burst_on(rf_wait);
			rf_tx = true;
			stats.bursts++;
		}
		if ((v & (EV_OFF_TICK | EV_OFF_RFRX | EV_ABORT)) != 0 && rf_tx) {
			bool now = (v & EV_ABORT) != 0;
			uint32_t late = db25d_burst_off(rf_wait, hrc_micros() - t_on, now);
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

static void dmr_tx_init(void)
{
	hrc_irq_register(IRQ_TIMER5, isr_timer);
	hrc_irq_enable(IRQ_TIMER5, true);
	hrc_irq_register(IRQ_MODEM_RF_RX, isr_rfrx);	// enabled while armed
	if (rf_task == NULL) {
		xTaskCreate(rf_task_fn, "dmrrf", RF_TASK_STACK, NULL, RF_TASK_PRIORITY, &rf_task);
	}
}

void db25d_dmr_tx_arm(void)
{
	uint32_t m = hrc7000_rd(modem, C7_RF_MODE);
	hrc7000_wr(modem, C7_RF_MODE, DB25D_DMR_TX_INVERT ? (m | RF_MODE_TX_INVERT) : (m & ~RF_MODE_TX_INVERT));
	hrc7000_wr(modem, C7_INTERP_CLEAR, C7_INT_RF_RX);
	keyed = true;
	hrc_irq_enable(IRQ_MODEM_RF_RX, true);
}

void db25d_dmr_tx_disarm(void)
{
	keyed = false;
	rf_post(EV_ABORT);	// the task outranks the caller: a burst not in a wait is done when this returns
	for (uint32_t i = 0; i < DISARM_WAIT_MS && (rf_tx || rf_busy); i++) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	hrc_irq_enable(IRQ_MODEM_RF_RX, false);
}

void db25d_dmr_tx_stats(db25d_dmr_tx_stats_t *st)
{
	*st = stats;
}
#endif	// DB25D_DMR_TX

#endif	// CONFIG_DMR
