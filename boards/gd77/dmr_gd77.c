// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 DMR modem: the shared HR_C6000 slot engine (drivers/modem/hrc_common/hrc6000_dmr.c) on the board's four modem
// interrupt lines, the voice port on SPI1 and the deferred event task that calls the DMR stack
// (docs/spec/dmr-modem.md 6, docs/spec/hardware-gd77.md 5.3). The structure is that of boards/dm1701/dmr_dm1701.c; what
// differs is the wiring: all four lines are on port C and share one interrupt, the timeslot line interrupts on both
// edges, and the RF RX line is routed, so the PA comes down when our burst ends instead of at the next slot boundary.
//
// What runs where:
//   PORTC (priority 5)   PTC18 timeslot: retry a skipped RF back-to-receive switch, the engine's slot step
//                        PTC17 system: copy out a received burst (status, page 2 or the voice port), clear the flags
//                        PTC16 RF TX: key our burst (gd77_rf_dmr_switch)
//                        PTC7 RF RX: our burst is over, drive down and back to receive
//   "dmr" task           drains the engine's events into the stack's callback; every 10 ms the watchdog
// Priority 5 is configMAX_SYSCALL, so the handler may notify the task. Task-level engine calls raise BASEPRI to 5 for
// their register work (lock / unlock below), and every configuration port transaction masks to that level too
// (bus_gd77.c), so a handler never lands inside one.
//
// Every pin role here is unverified (hardware-gd77.md 11).
#include <stddef.h>
#include <string.h>
#include "gd77.h"
#include "dmr_gd77.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "pins.h"
#include "platform.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "FreeRTOS.h"
#include "task.h"

#define DMR_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define DMR_TASK_STACK		512	// words: the stack's callback runs on it
#define DMR_POLL_MS		10
#define MODEM_IRQ_PRIORITY	5	// must stay >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY

// Voice port (V_SPI) on SPI1: mode 1, bus / 5 / 8 (about 1.2 MHz at the 49.152 MHz bus), PTB10 driven as the chip select
static const k22f_dspi_cfg_t vspi_cfg = { .inst = 1, .cpol = false, .cpha = true, .pbr = 2, .br = 3, .dbr = false };

#define MODEM_PINS	(k22f_pin_bit(PIN_C6K_RF_RX) | k22f_pin_bit(PIN_C6K_RF_TX) | k22f_pin_bit(PIN_C6K_SYS) | \
			 k22f_pin_bit(PIN_C6K_SLOT))

static hrc6000_dmr_t eng ODR_CPU_RAM;
static TaskHandle_t task;
static volatile bool active;
static volatile bool sys_poke;		// the task saw the system line held low: service it without an edge
static dmr_event_cb_t volatile stack_cb;
static void *volatile stack_ctx;
static struct {
	uint32_t slot_ints, slot_dups, rfrx_ints, voice_errors;
	uint32_t last_slot_us;
	bool have_slot;
} cnt;

// --- Engine ops ---

static bool in_isr(void)
{
	return __get_IPSR() != 0;
}

// Free-running microseconds from the cycle counter; called at least every 10 ms by the task, so no wrap is missed
static uint32_t now_us(void *ctx)
{
	(void)ctx;
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
	(void)ctx;
	UBaseType_t m = portSET_INTERRUPT_MASK_FROM_ISR();
	if (lock_depth++ == 0) {
		lock_mask = m;
	}
}

static void unlock(void *ctx)
{
	(void)ctx;
	if (lock_depth > 0 && --lock_depth == 0) {
		portCLEAR_INTERRUPT_MASK_FROM_ISR(lock_mask);
	}
}

static void notify(void *ctx)
{
	(void)ctx;
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
	(void)ctx;
	return gd77_rf_dmr_switch(tx);
}

static bool sys_line_low(void *ctx)
{
	(void)ctx;
	return !k22f_pin_read(PIN_C6K_SYS);
}

static const hrc6000_dmr_ops_t ops = {
	.rf = rf, .now_us = now_us, .lock = lock, .unlock = unlock, .notify = notify, .sys_line_low = sys_line_low,
	.ctx = NULL,
};

// --- Voice port ---

static void vspi_select(bool on)
{
	k22f_pin_write(PIN_VSPI_PCS0, !on);
}

// One page 3 frame: 0x03 / 0x83, 0x00, then the data. Atomic with respect to the modem interrupts
bool gd77_vspi_xfer(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	(void)ctx;
	uint8_t hdr[2] = { write ? 0x03 : 0x83, 0x00 };
	UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
	vspi_select(true);
	bool ok = k22f_dspi_xfer(1, hdr, NULL, sizeof hdr) &&
		  (write ? k22f_dspi_xfer(1, buf, NULL, len) : k22f_dspi_xfer(1, NULL, buf, len));
	vspi_select(false);
	portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
	if (!ok) {
		cnt.voice_errors++;
	}
	return ok;
}

static void vspi_setup(void)
{
	k22f_pin_config(PIN_VSPI_PCS0, K22F_MUX_GPIO | K22F_OUT | K22F_HIGH);	// deselected
	k22f_pin_config(PIN_VSPI_SCK, K22F_MUX(2) | K22F_DRIVE_HIGH);
	k22f_pin_config(PIN_VSPI_SOUT, K22F_MUX(2) | K22F_DRIVE_HIGH);
	k22f_pin_config(PIN_VSPI_SIN, K22F_MUX(2));
	k22f_dspi_init(&vspi_cfg);
}

// --- Interrupts ---

static void irq_setup(void)
{
	k22f_pin_config(PIN_C6K_RF_RX, K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_FALLING));
	k22f_pin_config(PIN_C6K_RF_TX, K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_FALLING));
	k22f_pin_config(PIN_C6K_SYS, K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_FALLING));
	k22f_pin_config(PIN_C6K_SLOT, K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_EITHER));
	NVIC_SetPriority(PORTC_IRQn, MODEM_IRQ_PRIORITY);
}

static void irq_enable(bool on)
{
	if (on) {
		k22f_port_irq_ack(K22F_PORT_C, MODEM_PINS);
		cnt.have_slot = false;
		NVIC_ClearPendingIRQ(PORTC_IRQn);
		NVIC_EnableIRQ(PORTC_IRQn);
	} else {
		NVIC_DisableIRQ(PORTC_IRQn);
	}
}

// One handler for the four lines; the flags are cleared before the work, so an edge during it comes back. Order:
// the slot boundary first, then a received burst, then the RF lines
void PORTC_IRQHandler(void)
{
	uint32_t pend = k22f_port_irq_pending(K22F_PORT_C) & MODEM_PINS;
	k22f_port_irq_ack(K22F_PORT_C, pend);
	if (sys_poke) {
		sys_poke = false;
		pend |= k22f_pin_bit(PIN_C6K_SYS);
	}
	if (pend & k22f_pin_bit(PIN_C6K_SLOT)) {
		uint32_t t = now_us(NULL);
		cnt.slot_ints++;
		if (gd77_slot_edge_new(t, cnt.last_slot_us, cnt.have_slot)) {
			cnt.last_slot_us = t;
			cnt.have_slot = true;
			gd77_rf_dmr_service();
			hrc6000_dmr_isr_timeslot(&eng);
		} else {
			cnt.slot_dups++;	// the line's other edge at the same boundary
		}
	}
	if (pend & k22f_pin_bit(PIN_C6K_SYS)) {
		hrc6000_dmr_isr_sys(&eng);
	}
	if (pend & k22f_pin_bit(PIN_C6K_RF_TX)) {
		hrc6000_dmr_isr_rftx(&eng);
	}
	if (pend & k22f_pin_bit(PIN_C6K_RF_RX)) {
		cnt.rfrx_ints++;
		if (gd77_rf_dmr_keyed()) {
			(void)gd77_rf_dmr_switch(false);	// our burst is over; the engine's own drop at the tick finds it done
		}
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
			hrc6000_dmr_poll(&eng);
			if (!k22f_pin_read(PIN_C6K_SYS)) {
				sys_poke = true;	// a flag still holds the line low: no edge will come
				NVIC_SetPendingIRQ(PORTC_IRQn);
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

// --- radio_gd77.c interface ---

void gd77_dmr_board_init(hrc_t *modem)
{
	hrc6000_dmr_init(&eng, modem, &ops);
	vspi_setup();
	irq_setup();
	irq_enable(false);
	if (task == NULL) {
		xTaskCreate(dmr_task, "dmr", DMR_TASK_STACK, NULL, DMR_TASK_PRIORITY, &task);
	}
}

void gd77_dmr_enter(void)
{
	hrc6000_dmr_run(&eng);
	active = true;
	irq_enable(true);
}

void gd77_dmr_leave(void)
{
	irq_enable(false);
	active = false;
	hrc6000_dmr_halt(&eng);
}

void gd77_dmr_rx_restart(void)
{
	hrc6000_dmr_rx_restart(&eng);
}

bool gd77_dmr_tx_active(void)
{
	return active && hrc6000_dmr_is_transmitting(&eng);
}

void gd77_dmr_tx_drop(void)
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
		gd77_dmr_tx_drop();
	}
}

static void configure(const dmr_config_t *cfg) { hrc6000_dmr_configure(&eng, cfg); }
static void rx_restart(void) { hrc6000_dmr_rx_restart(&eng); }
static void rx_lock_slot(int8_t ts) { hrc6000_dmr_rx_lock_slot(&eng, ts); }
static bool tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	return active && hrc6000_dmr_tx_begin(&eng, timing, ts, access);
}
static void tx_listen(void) { hrc6000_dmr_tx_listen(&eng); }
static int8_t tx_fill(const dmr_tx_burst_t *b) { return hrc6000_dmr_tx_fill(&eng, b); }
static bool tx_queue(const dmr_tx_burst_t *b) { return hrc6000_dmr_tx_queue(&eng, b); }
static uint8_t tx_queue_space(void) { return hrc6000_dmr_tx_queue_space(&eng); }
static void tx_end(void) { hrc6000_dmr_tx_end(&eng); }
static void tx_abort(void) { hrc6000_dmr_tx_abort(&eng); }
static void set_embedded_lc(const uint8_t lc[9]) { hrc6000_dmr_set_embedded_lc(&eng, lc); }
static void set_cc_filter(int8_t cc) { hrc6000_dmr_set_cc_filter(&eng, cc); }
static bool is_transmitting(void) { return hrc6000_dmr_is_transmitting(&eng); }

// No split duplex roles on this board yet (the BS outbound needs a radio to bring up on): those ops stay NULL
const hal_dmr_modem_t gd77_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting, .tx_listen = tx_listen, .tx_fill = tx_fill,
};

#endif	// CONFIG_DMR

void gd77_dmr_stats(gd77_dmr_stats_t *out)
{
	memset(out, 0, sizeof(*out));
#if CONFIG_DMR
	hrc6000_dmr_stats_t m;
	hrc6000_dmr_stats(&eng, &m);
	out->active = active;
	out->stack_attached = stack_cb != NULL;
	out->slot_ints = cnt.slot_ints;
	out->slot_dups = cnt.slot_dups;
	out->sys_ints = m.sys_ints;
	out->rftx_ints = m.rftx_ints;
	out->rfrx_ints = cnt.rfrx_ints;
	out->ticks = m.ticks;
	out->rx_voice = m.rx_voice;
	out->rx_data = m.rx_data + m.rx_csbk + m.rx_lc_header + m.rx_terminator;
	out->rx_crc_err = m.rx_crc_err;
	out->tx_bursts = m.tx_bursts;
	out->voice_errors = cnt.voice_errors;
	gd77_rf_dmr_counters(&out->rf_skipped, &out->rf_unarmed);
#endif
}
