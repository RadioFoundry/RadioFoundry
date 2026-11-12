// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-88 SoC B: the PA of A's transmitter (docs/spec/hardware-gd88.md 10). A's chip makes the signal, but the PA, its
// supplies and its drive (DAC B) are wired to B. A hands B the drive and the page's delays before it transmits (RX2_TX,
// gd88b_pa_arm) and then raises its TX line, PTB2 on both SoCs. B follows the line from a both-edge interrupt, as the
// original firmware does ("path 0"):
//
//   rising:  path pins for A's chip in its TX band, A's RX path off; 50 us + the TX-on delay; PA select; 200 us; PA
//            supply; DAC B ramped up to the drive (180 steps of 4 us)
//            With the drive's points (rx2_tx_key_t: a DMR burst A keys by the key-up rule, docs/spec/pa-keyup.md) no
//            ramp: the PA select and supply wait until SUPPLY_AHEAD_US before the first point, and DAC B steps to the
//            drive at its time after the edge, after a boost when A sent one. The PA loop's low-pass shapes the
//            rise, and the rule times a step
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
#include "debuglink/debuglink.h"
#include "FreeRTOS.h"
#include "task.h"

#define PA_TASK_STACK		160u	// words
#define PA_TASK_PRIORITY	(configMAX_PRIORITIES - 1)
#define PA_POLL_MS		50u	// the line read again without an edge
#define UHF_FROM_HZ		280000000u
#define PA_DRIVE_MAX		4095u	// DAC B's range
#define PA_DELAY_MAX_US		5000u	// the page's delays; longer ones are refused
#define PA_KEY_AT_MAX_US	8000u	// a drive point after the edge: the modem's longest lead is 6.3 ms + 0.85 ms
#define TX_RX_OFF_US		50u
#define TX_SUPPLY_US		200u
#define TX_OFF_PATH_US		(300u + 1000u + 300u)
#define TX_OFF_END_US		200u
// With the drive's points, the PA's supply comes on this long before the first of them. Sooner, the PA passes A's
// carrier at drive 0 from the supply on: 40-44 dB under the level from 1.75 ms before the first symbol (in the slot
// before) with the supply on at the sequence's start, a 0.3 ms step at -40 dB with it 300 us ahead. From 50 us to the
// point itself the rise is the same (UHF, both levels: -3 dB 0.27-0.34 ms before the first symbol). Not static, so
// the bench can try others through the debug link's poke
#define SUPPLY_AHEAD_US		50u
volatile uint16_t gd88b_supply_ahead_us = SUPPLY_AHEAD_US;

static TaskHandle_t task;
static volatile bool armed;
static volatile uint32_t arm_hz;
static volatile uint16_t arm_pa, arm_on_us, arm_off_us;
static volatile rx2_tx_key_t arm_key;	// drive_at_us 0: the ramp
static volatile uint32_t edge_us;	// hrc_micros() at the TX line's last rising edge
static bool keyed, key_uhf;
// Keys with the drive's points, those whose first point had passed when the sequence got there, and the most by
static uint32_t key_pts, key_late, key_late_max_us;
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

// Until us after the TX line's edge; at once when that is past. Returns by how much it was
static uint32_t wait_edge(uint32_t t0, uint16_t us)
{
	uint32_t e = hrc_micros() - t0;
	if (e >= us) {
		return e - us;
	}
	hal_delay_us(us - e);
	return 0;
}

static void key_on(void)
{
	uint32_t s = hrc_irq_save();
	uint32_t hz = arm_hz;
	uint16_t on_us = arm_on_us;
	rx2_tx_key_t key = arm_key;
	uint32_t t0 = edge_us;
	key_pa = arm_pa;
	hrc_irq_restore(s);
	key_uhf = hz >= UHF_FROM_HZ;
	gd88b_path_a_tx(true, key_uhf);
	hal_delay_us(TX_RX_OFF_US + on_us);
	DAC_DATA_B = 0;
	hrc_dac_b_power(true);
	if (key.drive_at_us != 0) {
		uint32_t first = key.boost != 0 ? key.boost_at_us : key.drive_at_us;
		uint32_t ahead = (uint32_t)gd88b_supply_ahead_us + TX_SUPPLY_US;
		if (first > ahead) {
			(void)wait_edge(t0, (uint16_t)(first - ahead));
		}
	}
	gd88b_path_pa(key_uhf, true, false);
	hal_delay_us(TX_SUPPLY_US);
	gd88b_path_pa(key_uhf, true, true);
	if (key.drive_at_us == 0) {
		ramp(key_pa, true);
	} else {
		// A keyed without an edge seen (the poll) has an old edge time: every point is past and goes at once
		uint32_t late = 0;
		if (key.boost != 0) {
			late = wait_edge(t0, key.boost_at_us);
			DAC_DATA_B = key.boost;
			(void)wait_edge(t0, key.drive_at_us);
		} else {
			late = wait_edge(t0, key.drive_at_us);
		}
		DAC_DATA_B = key_pa;
		key_pts++;
		if (late != 0) {
			key_late++;
			if (late > key_late_max_us) {
				key_late_max_us = late;
			}
		}
	}
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
		if (hrc_pin_read(PIN_TX_LINE)) {
			edge_us = hrc_micros();	// the drive's points count from here
		}
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

#if CONFIG_RX2_DL_SERVER
// The debug link's TX guard (debuglink.h): B's SoC registers stay unwritten while the PA is keyed, as on A
bool debuglink_transmitting(void)
{
	return keyed || own_busy();
}
#endif

void gd88b_pa_init(void)
{
	hrc_pin_in(PIN_TX_LINE);
	xTaskCreate(task_fn, "pa", PA_TASK_STACK, NULL, PA_TASK_PRIORITY, &task);
	hrc_irq_register(IRQ_GPIOB, gpiob_isr);
	hrc_pin_irq(PIN_TX_LINE, true, true, false);
	hrc_irq_enable(IRQ_GPIOB, true);
}

bool gd88b_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us, const rx2_tx_key_t *key)
{
	if (hz != 0 && (pa > PA_DRIVE_MAX || on_us > PA_DELAY_MAX_US || off_us > PA_DELAY_MAX_US ||
			key->boost > PA_DRIVE_MAX || key->drive_at_us > PA_KEY_AT_MAX_US ||
			(key->boost != 0 && key->boost_at_us > key->drive_at_us))) {
		refused++;
		return false;
	}
	uint32_t s = hrc_irq_save();
	arm_hz = hz;
	arm_pa = pa;
	arm_on_us = on_us;
	arm_off_us = off_us;
	arm_key = *key;
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
bool gd88_pa_arm(uint32_t hz, uint16_t pa, uint16_t on_us, uint16_t off_us, const rx2_tx_key_t *key)
{
	(void)on_us;
	(void)off_us;
	(void)key;	// B's own bursts ramp
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
	// B's own transmitter: the dmrtx.* view keys (the line is full)
	if (key_pts == 0) {
		dl_fmt(buf, len, "b pa %s %s drive %u on %u off %u keys %u edges %u ref %u", armed ? "armed" : "off",
		       keyed ? "keyed" : "idle", arm_pa, arm_on_us, arm_off_us, (unsigned)keys, (unsigned)edges,
		       (unsigned)refused);
		return true;
	}
	// Once a burst was keyed with the drive's points (rx2_tx_key_t): how many were, how many reached their first
	// point late and the most in us
	dl_fmt(buf, len, "b pa %s %s drive %u keys %u pts %u late %u max %u", armed ? "armed" : "off",
	       keyed ? "keyed" : "idle", arm_pa, (unsigned)keys, (unsigned)key_pts, (unsigned)key_late,
	       (unsigned)key_late_max_us);
	return true;
}
