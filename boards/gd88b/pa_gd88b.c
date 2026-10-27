// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B: the PA of A's transmitter (docs/spec/hardware-gd88.md 10). A's chip makes the signal, but the PA, its
// supplies and its drive (DAC B) are wired to B. A hands B the drive and the page's delays before it transmits (RX2_TX,
// gd88b_pa_arm) and then raises its TX line, PTB2 on both SoCs. B follows the line from a both-edge interrupt, as the
// original firmware does ("path 0"):
//
//   rising:  path pins for A's chip in its TX band, A's RX path off; 50 us + the TX-on delay; PA select; 200 us; PA
//            supply; DAC B ramped up to the drive (180 steps of 4 us)
//   falling: the TX-off delay; DAC B ramped down; supply and select off; 300 + 1000 + 300 us; the path pins back; 200 us
//
// B's own transmitter (GD88_DMR_TX: A's second transmitter for an SFR's repeat, the original firmware's path 1) uses
// the same PA with B's own drive, keyed per burst by the shared DMR code (gd88_pa_own, from dmr_gd88.c's RF task);
// it never keys while A's transmission has the PA.
//
// A disarmed B keys nothing, whatever the line does. Disarming while keyed (A ended, HELLO_REQ, A restarted) ramps
// down at once. The sequence runs in its own task at the top priority, woken by the interrupt: busy waits of at most a
// few ms, as in A's DMR burst sequence.
#include "gd88b.h"
#include "gd88_rf.h"
#include "gd88_cal.h"
#include "pins.h"
#include "platform.h"
#include "hal_time.h"
#include "debuglink/dl_fmt.h"
#include "FreeRTOS.h"
#include "task.h"

#define PA_TASK_STACK		160u	// words
#define PA_TASK_PRIORITY	(configMAX_PRIORITIES - 1)
#define PA_POLL_MS		50u	// the line read again without an edge
#define UHF_FROM_HZ		280000000u
#define PA_DRIVE_MAX		4095u	// DAC B's range
#define PA_DELAY_MAX_US		5000u	// the page's delays; longer ones are refused
#define TX_RX_OFF_US		50u
#define TX_SUPPLY_US		200u
#define TX_OFF_PATH_US		(300u + 1000u + 300u)
#define TX_OFF_END_US		200u

static TaskHandle_t task;
static volatile bool armed;
static volatile uint32_t arm_hz;
static volatile uint16_t arm_pa, arm_on_us, arm_off_us;
static bool keyed, key_uhf;
#if GD88_DMR_TX
static volatile bool own_armed;
static volatile bool own_keyed;
static bool own_uhf;
static uint16_t own_pa;
static uint32_t own_keys, own_skips;
#endif
static uint16_t key_pa;
static uint32_t keys, edges, refused;

static void ramp(uint16_t target, bool up)
{
	for (uint16_t i = 0; i <= GD88_APC_STEPS; i++) {
		DAC_DATA_B = gd88_apc_level(target, up ? i : (uint16_t)(GD88_APC_STEPS - i));
		hal_delay_us(GD88_APC_STEP_US);
	}
}

static void key_on(void)
{
	uint32_t s = hrc_irq_save();
	uint32_t hz = arm_hz;
	uint16_t on_us = arm_on_us;
	key_pa = arm_pa;
	hrc_irq_restore(s);
	key_uhf = hz >= UHF_FROM_HZ;
	gd88b_path_a_tx(true, key_uhf);
	hal_delay_us(TX_RX_OFF_US + on_us);
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	gd88b_path_pa(key_uhf, true, false);
	hal_delay_us(TX_SUPPLY_US);
	gd88b_path_pa(key_uhf, true, true);
	ramp(key_pa, true);
	keyed = true;
	keys++;
}

// now: disarmed while keyed, no TX-off delay
static void key_off(bool now)
{
	if (!now) {
		hal_delay_us(arm_off_us);
	}
	ramp(key_pa, false);
	DAC_DATA_B = 0;
	hrc_dac_b_power(false);
	gd88b_path_pa(key_uhf, true, false);
	gd88b_path_pa(key_uhf, false, false);
	hal_delay_us(TX_OFF_PATH_US);
	gd88b_path_a_tx(false, key_uhf);
	hal_delay_us(TX_OFF_END_US);
	keyed = false;
}

// B's own transmitter has the PA (never at once with A's: A doesn't key while B transmits for it)
static bool own_busy(void)
{
#if GD88_DMR_TX
	return own_keyed;
#else
	return false;
#endif
}

static void gpiob_isr(void)
{
	uint32_t pending = hrc_gpio_pending(GPIOB_BASE);
	hrc_gpio_ack(GPIOB_BASE, pending);
	if ((pending & hrc_gpio_bit(PIN_TX_LINE)) != 0 && task != NULL) {
		edges++;
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

static void task_fn(void *arg)
{
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PA_POLL_MS));
		bool line = hrc_pin_read(PIN_TX_LINE);
		if (!keyed && line && armed && !own_busy()) {
			key_on();
		} else if (keyed && (!line || !armed)) {
			key_off(!armed);
		}
	}
}

void gd88b_pa_init(void)
{
	hrc_pin_in(PIN_TX_LINE);
	xTaskCreate(task_fn, "pa", PA_TASK_STACK, NULL, PA_TASK_PRIORITY, &task);
	hrc_irq_register(IRQ_GPIOB, gpiob_isr);
	hrc_pin_irq(PIN_TX_LINE, true, true, false);
	hrc_irq_enable(IRQ_GPIOB, true);
}

bool gd88b_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us)
{
	if (hz != 0 && (pa > PA_DRIVE_MAX || on_us > PA_DELAY_MAX_US || off_us > PA_DELAY_MAX_US)) {
		refused++;
		return false;
	}
	uint32_t s = hrc_irq_save();
	arm_hz = hz;
	arm_pa = pa;
	arm_on_us = on_us;
	arm_off_us = off_us;
	armed = hz != 0;
	hrc_irq_restore(s);
	if (task != NULL) {
		xTaskNotifyGive(task);	// a disarm while keyed: down now
	}
	return true;
}

#if GD88_DMR_TX
// ---- B's own transmitter ----


// The drive for B's own bursts (radio_gd88.c dmr_tx, B's calibration); the delays are the radio code's
bool gd88_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us)
{
	(void)on_us;
	(void)off_us;
	if (pa > PA_DRIVE_MAX) {
		refused++;
		return false;
	}
	own_uhf = hz >= UHF_FROM_HZ;
	own_pa = pa;
	own_armed = true;
	return true;
}

void gd88_pa_disarm(void)
{
	own_armed = false;	// the burst in progress (if any) ends with its gd88_pa_own(false)
}

// From the RF task, per burst: up (select, 200 us, supply, the ramp) or down (the ramp, supply and select off)
void gd88_pa_own(bool on)
{
	if (on) {
		if (!own_armed || keyed || own_keyed) {
			own_skips++;
			return;		// not armed, or A's transmission has the PA
		}
		DAC_DATA_B = 0;
		hrc_dac_b_power(true);
		gd88b_path_pa(own_uhf, true, false);
		hal_delay_us(TX_SUPPLY_US);
		gd88b_path_pa(own_uhf, true, true);
		ramp(own_pa, true);
		own_keyed = true;
		own_keys++;
		return;
	}
	if (!own_keyed) {
		return;
	}
	ramp(own_pa, false);
	DAC_DATA_B = 0;
	hrc_dac_b_power(false);
	gd88b_path_pa(own_uhf, true, false);
	gd88b_path_pa(own_uhf, false, false);
	own_keyed = false;
}
#endif

#if GD88_DMR_TX
void gd88b_pa_own_counts(uint32_t *keys_out, uint32_t *skips_out)
{
	*keys_out = own_keys;
	*skips_out = own_skips;
}
#endif

bool gd88b_pa_diag_line(char *buf, uint8_t len)
{
#if GD88_DMR_TX
	// B's own transmitter: the dmrtx.* view keys (the line is full)
	dl_fmt(buf, len, "b pa %s %s drive %u on %u off %u keys %u edges %u ref %u", armed ? "armed" : "off",
	       keyed ? "keyed" : "idle", arm_pa, arm_on_us, arm_off_us, (unsigned)keys, (unsigned)edges,
	       (unsigned)refused);
#else
	dl_fmt(buf, len, "b pa %s %s drive %u on %u off %u keys %u edges %u ref %u", armed ? "armed" : "off",
	       keyed ? "keyed" : "idle", arm_pa, arm_on_us, arm_off_us, (unsigned)keys, (unsigned)edges,
	       (unsigned)refused);
#endif
	return true;
}
