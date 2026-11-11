// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 radio: SKY72310 synthesizer + GT3136 receiver + HR_C5000 (the HR_C6000 driver in its C5000 variant) + RF path
// GPIOs + the two DACs + calibration (docs/spec/hardware-md380.md 4-8, 11). Nothing here has run on a radio.
//
// Unlike the other STM32 radios there is no RF transceiver chip: the MCU tunes the synthesizer itself, selects the receive or
// transmit VCO, switches the receiver and transmitter supplies, and drives the PA power loop from DAC1 (which tunes
// the receiver front end while receiving). The HR_C5000 does the FM and DMR modulation (two-point, MOD1 to the VCO,
// MOD2 to the reference) and takes the receiver's IF for DMR.
//
// The frequency plan (hardware-md380.md 5.3, unconfirmed): a 16.8 MHz TCXO; receive on a 49.95 MHz first IF with the
// UHF LO below the signal and the VHF VCO at twice the LO above it; transmit with the VCO on the carrier (UHF) or at
// twice it (VHF). MD380_IF_HZ, MD380_RX_LO_ABOVE and MD380_VCO_MULT hold the plan for each band.
//
// DMR (modem-hrc6000.md 4.3 with the HR_C5000 notes of hardware-md380.md 4): the synthesizer is moved to the TX
// frequency at the first burst of a transmission (from the RF TX interrupt, without waiting for lock: the interrupt
// comes some milliseconds before the burst). Between bursts only the PA supply and loop are switched; the end of the
// transmission puts the receiver back on the RX frequency at once (rf_dmr_tx_done), simplex too. Each burst's PA is dropped
// 30 ms after its RF TX interrupt by the TIM7 key timer, which also runs the key-up points (gotchas 3.4: the phase of the HR_C5000's timeslot interrupt
// against the burst is unknown); the slot engine's own PA-off at the next timeslot interrupt is the fallback.
#include <stddef.h>
#include "config.h"
#include "md380.h"
#include "pins.h"
#include "rfbus_md380.h"
#include "hal_time.h"
#include "rf/sky72310/sky72310.h"
#include "modem/hrc5000/hrc5000.h"
#include "modem/hrc_common/hrc6000_dmr.h"
#include "dmr_md380.h"
#include "radio/pa_keyup.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// Frequency plan (hardware-md380.md 5.3, all ?)
#ifndef MD380_TCXO_HZ
#define MD380_TCXO_HZ		16800000u
#endif
#ifndef MD380_IF_HZ
#define MD380_IF_HZ		49950000u
#endif
#if RFOUNDRY_BAND_VHF
#define MD380_RX_LO_ABOVE	1	// LO = f + IF
#define MD380_VCO_MULT		2u	// the VCO runs at twice the LO / carrier
#else
#define MD380_RX_LO_ABOVE	0	// LO = f - IF
#define MD380_VCO_MULT		1u
#endif

// TX carrier correction (Hz, added to the TX frequency). One radio needed -1900 for a hotspot to decode it
// (hardware-md380.md 5.2): no default until a second radio shows whether it is the unit or the reference tune
#ifndef MD380_TX_OFFSET_HZ
#define MD380_TX_OFFSET_HZ	0
#endif

// Reference trim (hal_radio_t.ref_trim): DAC2 moves the reference about 36.1 ppb a count, higher = higher (16.2 Hz a
// count at 448.7 MHz, hardware-md380.md 5.2), added to the calibration's tune
#define REF_PPB_X10_PER_COUNT	361

#define DAC_MAX			4095u
#define PLL_PD_GAIN		31u	// phase detector gain, the data sheet's largest (1 mA/rad): a guess (?)
#define PLL_LOCK_US		5000u	// task-level wait for lock detect after a retune
#define C5000_MIC_GAIN		0xC8	// HR_C5000 4:0x0F, the value the MD-380 sets
#define C5000_PWD_WAIT_MS	12	// at least 10 ms after PWD low before the first access (?)
#define PA_HOLD_US		30000u	// TIM7 at 1 MHz: the PA is dropped 30 ms after the RF TX interrupt
#define PA_TIMER_PRIORITY	5	// the modem interrupts' level
// The RF TX interrupt comes about this long before the burst's first symbol (the figure of the HR_C6000 radios; the
// key-up points count back from it). Keyed at the interrupt, the PA was up 1.1-1.6 ms before the first symbol
#define DMR_KEY_INT_AHEAD_US	2530u
#define DMR_KEY_MIN_US		30u	// the key timer's shortest step; closer points run together
// The earliest key-up point (radio/pa_keyup.h): the timer's first step after the interrupt
#define KEYUP_MAX_AHEAD_US	(DMR_KEY_INT_AHEAD_US - DMR_KEY_MIN_US)
// The key-up constants (docs/spec/pa-keyup.md), measured with tools/bench/pa_cal.py on a UHF radio up to its 2.5 W
// level: the window and the PA 1 ms after the interrupt as before; the lowest level, 0.95 ms late on its own drive,
// gets a boost
static const pa_keyup_cfg_t keyup_cfg = { .dead_pm = 562, .knee_us = 90, .slope_us = 1013,
					  .window_us = DMR_KEY_INT_AHEAD_US - 1000u, .settle_us = 306,
					  .pa_us = DMR_KEY_INT_AHEAD_US - 1000u };

// Voice SPI2 (hardware-md380.md 4.4): PB13 SCK, PB14 MISO, PB15 MOSI (AF5), chip select PIN_C5000_VCS
#define VOICE_SPI_PINS		(GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15)
#define VOICE_SPI_TIMEOUT_MS	2

// RSSI and noise from the ADC (hardware-md380.md 5.5): placeholders until a bench measurement gives the curves
#define RSSI_FLOOR_DBM		(-130)
#define RSSI_SPAN_DB		90	// full ADC scale above the floor (?)
#define CARRIER_DBM		(-118)

static struct {
	sky72310_t pll;
	hrc5000_t modem;
	radio_mode_t mode;
	bool tx_on;
	bool rx_valid;
	uint32_t rx_hz;
	radio_bw_t rx_bw;
	bool asleep;
	uint32_t rx_tune_dac;	// DAC1 for the receiver front end at rx_hz (the PA drive shares DAC1)
	volatile uint8_t ref_tune;	// the calibration's reference tune at rx_hz
	volatile int16_t ref_trim_ppb;	// hal_radio_t.ref_trim
	md380_radio_diag_t diag;
	uint32_t bus_errors;
	subtone_t rx_tone;	// the RX sub-audio the detector is set for (CONFIG_SUBTONE)

	// DMR per-burst keying (shared with the modem ISRs)
	volatile bool dmr_armed;	// tx() in DMR mode: TX frequency and drive known
	volatile bool dmr_tx_tuned;	// the synthesizer and the HR_C5000 are on the transmit side
	volatile uint32_t dmr_tx_hz;
	volatile uint32_t dmr_drive;
	pa_keyup_point_t key_pt[PA_KEYUP_POINTS];	// the transmission's key-up points, earliest first, set before it is armed
	pa_keyup_step_t key_st[PA_KEYUP_POINTS_MAX];	// and as TIM7's steps (pa_keyup_steps)
	uint8_t key_ns;
	pa_keyup_t ku;
	volatile uint8_t key_i;		// the next step; key_ns: the steps are done and the timer runs to the PA drop
	volatile uint16_t key_base_us;	// the timer's last update, in us before the first symbol
	volatile uint32_t key_apc;	// the drive of the last point, for the PA's own point
	uint8_t dmr_mod2, dmr_mod1;
	uint32_t rf_skipped, rf_unarmed, rf_rx_back;
} r;

static DAC_HandleTypeDef dac;
static SPI_HandleTypeDef spi2;

static void count(bool ok)
{
	if (!ok) {
		r.bus_errors++;
	}
}

static const sky72310_bus_t pll_bus = {
	.write = md380_rfbus_pll_write, .locked = md380_rfbus_pll_locked, .delay_us = hal_delay_us,
};

// Voice port: STM32 SPI2 master, mode 2, prescaler 32 (1.125 MHz). At 4.5 MHz (prescaler 8) received voice read
// back with the first bits of every byte wrong (on air 2026-10-18: C0 words clean only two bits off their place, C1
// broken); at 1.125 MHz the three AMBE frames are in place and C1 checks
#ifndef MD380_VOICE_SPI_PRESCALER
#define MD380_VOICE_SPI_PRESCALER	SPI_BAUDRATEPRESCALER_32
#endif
static void voice_spi_setup(void)
{
	__HAL_RCC_SPI2_CLK_ENABLE();
	stm32_pin_af(GPIOB, VOICE_SPI_PINS, GPIO_AF5_SPI2, false, GPIO_NOPULL);
	PIN_WRITE(PIN_C5000_VCS, true);
	spi2.Instance = SPI2;
	spi2.Init.Mode = SPI_MODE_MASTER;
	spi2.Init.Direction = SPI_DIRECTION_2LINES;
	spi2.Init.DataSize = SPI_DATASIZE_8BIT;
	spi2.Init.CLKPolarity = SPI_POLARITY_HIGH;
	spi2.Init.CLKPhase = SPI_PHASE_1EDGE;
	spi2.Init.NSS = SPI_NSS_SOFT;
	spi2.Init.BaudRatePrescaler = MD380_VOICE_SPI_PRESCALER;
	spi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
	spi2.Init.TIMode = SPI_TIMODE_DISABLE;
	spi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	spi2.Init.CRCPolynomial = 7;
	HAL_SPI_Init(&spi2);
}

// One voice frame: 0x03 / 0x83, 0x00, then the data. Atomic with respect to the modem ISRs
static bool voice_xfer(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	uint8_t hdr[2] = { write ? 0x03 : 0x83, 0x00 };
	uint32_t mask = md380_rfbus_lock();
	PIN_WRITE(PIN_C5000_VCS, false);
	bool ok = HAL_SPI_Transmit(&spi2, hdr, 2, VOICE_SPI_TIMEOUT_MS) == HAL_OK;
	if (ok && write) {
		ok = HAL_SPI_Transmit(&spi2, buf, len, VOICE_SPI_TIMEOUT_MS) == HAL_OK;
	} else if (ok) {
		ok = HAL_SPI_Receive(&spi2, buf, len, VOICE_SPI_TIMEOUT_MS) == HAL_OK;
	}
	PIN_WRITE(PIN_C5000_VCS, true);
	md380_rfbus_unlock(mask);
	return ok;
}

static const hrc_bus_t modem_bus = {
	.transfer = md380_rfbus_modem_transfer, .delay_ms = hal_delay_ms, .delay_us = hal_delay_us, .ctx = NULL,
	.voice = voice_xfer,
};

// --- DACs and RF path pins (hardware-md380.md 5.4, 5.6) ---

static void dac_setup(void)
{
	__HAL_RCC_DAC_CLK_ENABLE();
	stm32_pin_analog(PIN_PORT(PIN_APC_DAC), PIN_MASK(PIN_APC_DAC) | PIN_MASK(PIN_REF_DAC));
	dac.Instance = DAC1;
	HAL_DAC_Init(&dac);
	DAC_ChannelConfTypeDef ch = { .DAC_Trigger = DAC_TRIGGER_NONE, .DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE };
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_1);
	HAL_DAC_ConfigChannel(&dac, &ch, DAC_CHANNEL_2);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 0);
	HAL_DAC_SetValue(&dac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, hrc5000_ref_dac(0x80));
	HAL_DAC_Start(&dac, DAC_CHANNEL_1);
	HAL_DAC_Start(&dac, DAC_CHANNEL_2);
}

// DAC1: the PA power reference while transmitting, the receiver front-end tuning voltage while receiving. Direct
// register write so the modem ISRs can use it
static void dac1_set(uint32_t value)
{
	DAC->DHR12R1 = value > DAC_MAX ? DAC_MAX : value;
}

static void dac2_set(uint32_t value)
{
	DAC->DHR12R2 = value > DAC_MAX ? DAC_MAX : value;
}

// DAC2 for the calibration's reference tune plus the user's trim
static uint32_t ref_dac(uint8_t tune, int16_t ppb)
{
	int32_t num = (int32_t)ppb * 10;
	int32_t counts = (num + (num < 0 ? -REF_PPB_X10_PER_COUNT / 2 : REF_PPB_X10_PER_COUNT / 2)) / REF_PPB_X10_PER_COUNT;
	int32_t v = (int32_t)hrc5000_ref_dac(tune) + counts;
	return v < 0 ? 0u : (uint32_t)v;	// dac2_set clamps the top
}

// DAC2 from r.ref_tune and r.ref_trim_ppb. The radio task (rx()) and the trim's caller (UI or debug link task) both
// write it: whoever writes rechecks both inputs afterwards and writes again if one moved, so the last write always
// matches the current tune and trim
static void ref_dac_apply(void)
{
	uint8_t tune;
	int16_t ppb;
	do {
		tune = r.ref_tune;
		ppb = r.ref_trim_ppb;
		dac2_set(ref_dac(tune, ppb));
	} while (tune != r.ref_tune || ppb != r.ref_trim_ppb);
}

static volatile bool pa_timer_running;

// TIM7: 1 MHz count (APB1 timer clock 72 MHz); each update is a key-up point or the PA drop
static void pa_timer_setup(void)
{
	__HAL_RCC_TIM7_CLK_ENABLE();
	TIM7->CR1 = 0;
	TIM7->PSC = (uint16_t)(2u * HAL_RCC_GetPCLK1Freq() / 1000000u - 1u);
	TIM7->ARR = 0xFFFFu;
	TIM7->EGR = TIM_EGR_UG;		// load the prescaler
	TIM7->SR = 0;
	TIM7->DIER = TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM7_IRQn, PA_TIMER_PRIORITY, 0);
	HAL_NVIC_EnableIRQ(TIM7_IRQn);
}

static void pa_timer_stop(void)
{
	TIM7->CR1 = 0;
	TIM7->SR = 0;
	pa_timer_running = false;
}

// Started at the RF TX interrupt: the first key-up point comes DMR_KEY_INT_AHEAD_US - its time later
static void pa_timer_start(void)
{
	uint16_t first = r.key_st[0].us;	// no earlier than DMR_KEY_MIN_US after the interrupt
	TIM7->CR1 = 0;
	TIM7->CNT = 0;
	TIM7->ARR = (uint32_t)(DMR_KEY_INT_AHEAD_US - first) - 1u;
	TIM7->SR = 0;
	r.key_i = 0;
	r.key_base_us = first;
	r.key_apc = r.dmr_drive;
	pa_timer_running = true;
	TIM7->CR1 = TIM_CR1_CEN;
}

// PA loop and transmitter supply off; DAC1 left at 0 until the receiver takes it
static void pa_off(void)
{
	PIN_WRITE(PIN_APC_EN, false);
	PIN_WRITE(PIN_TX_5V, false);
	dac1_set(0);
}

static void pa_on(uint32_t drive)
{
	dac1_set(drive);
	PIN_WRITE(PIN_TX_5V, true);
	PIN_WRITE(PIN_APC_EN, true);
}

// One key-up point (radio/pa_keyup.h): the drive on DAC1, and at the PA's own point the supply and the loop
static void key_point(uint8_t kind)
{
	switch (kind) {
	case PA_KEYUP_PT_BOOST:
		r.key_apc = r.ku.boost;
		break;
	case PA_KEYUP_PT_PA:
		pa_on(r.key_apc);
		return;
	default:
		r.key_apc = r.ku.drive;
		break;
	}
	dac1_set(r.key_apc);
}

void TIM7_IRQHandler(void)
{
	TIM7->ARR = 0xFFFFu;	// count on from this update while the points run: a short step must not come round again
	TIM7->SR = 0;
	if (!pa_timer_running) {
		TIM7->CR1 = 0;
		return;
	}
	if (r.key_i >= r.key_ns) {	// the burst is over
		pa_timer_stop();
		pa_off();
		return;
	}
	for (;;) {
		uint8_t i = r.key_i;
		for (uint8_t p = i != 0u ? r.key_st[i - 1u].end : 0u; p < r.key_st[i].end; p++) {
			key_point(r.key_pt[p].kind);
		}
		r.key_i = ++i;
		if (i >= r.key_ns) {
			// The PA drop, PA_HOLD_US after the interrupt
			TIM7->ARR = PA_HOLD_US - (DMR_KEY_INT_AHEAD_US - r.key_base_us) - 1u;
			return;
		}
		// From the last update; a step already due goes now
		uint32_t step = pa_keyup_step_wait(r.key_base_us, r.key_st[i].us, TIM7->CNT, DMR_KEY_MIN_US);
		if (step != 0u) {
			TIM7->ARR = step - 1u;
			r.key_base_us = r.key_st[i].us;
			return;
		}
	}
}

static void set_wide(bool wide)
{
#ifndef MD380_KEEP_SWD
	PIN_WRITE(PIN_WIDE, wide);	// polarity ?
#else
	(void)wide;
#endif
}

// --- Synthesizer ---

static uint32_t rx_vco_hz(uint32_t hz)
{
	uint32_t lo = MD380_RX_LO_ABOVE ? hz + MD380_IF_HZ : hz - MD380_IF_HZ;
	return lo * MD380_VCO_MULT;
}

static uint32_t tx_vco_hz(uint32_t hz)
{
	return (uint32_t)((int32_t)hz + MD380_TX_OFFSET_HZ) * MD380_VCO_MULT;
}

// The smallest reference divider that keeps the divide ratio in the fractional range (finest resolution, fastest
// loop): at 16.8 MHz and a VCO of 270-480 MHz that is 2 or 3
static uint8_t pick_ref_div(uint32_t vco_hz)
{
	sky72310_ratio_t ratio;
	for (uint8_t d = 1; d <= 32; d++) {
		if (sky72310_ratio(MD380_TCXO_HZ, d, vco_hz, &ratio)) {
			return d;
		}
	}
	return 0;
}

// Retune; tx selects the transmit VCO. wait: task level, wait for lock detect
static bool pll_tune(uint32_t vco_hz, bool tx, bool wait)
{
	uint8_t d = pick_ref_div(vco_hz);
	md380_rfbus_vco_select(tx);
	if (d == 0 || !sky72310_set_vco(&r.pll, d, vco_hz)) {
		r.diag.pll_fails++;
		r.diag.pll_locked = false;
		return false;
	}
	r.diag.pll_vco_hz = vco_hz;
	r.diag.pll_locked = wait ? sky72310_wait_lock(&r.pll, PLL_LOCK_US) : PIN_READ(PIN_PLL_LD);
	if (wait && !r.diag.pll_locked) {
		r.diag.pll_fails++;
	}
	return !wait || r.diag.pll_locked;
}

#if CONFIG_REF_TRIM
// hal_radio_t.ref_trim: DAC2 at once (one register; receive and transmit share it), and at every rx(): no retune
static bool ref_trim(int16_t ppb)
{
	r.ref_trim_ppb = ppb;
	if (r.rx_valid) {
		ref_dac_apply();
	}
	return false;
}
#endif

// --- Helpers ---

static const band_t *find_band(uint32_t hz)
{
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (hz >= board.bands[i].lo_hz && hz <= board.bands[i].hi_hz) {
			return &board.bands[i];
		}
	}
	return NULL;
}

// Receiver on for the current mode: supply, IF to the HR_C5000, FM or DMR audio path
static void rx_path(bool on)
{
	PIN_WRITE(PIN_RX_5V, on);
	PIN_WRITE(PIN_DMR_SW, on);			// high: receiver IF and audio to the HR_C5000
	PIN_WRITE(PIN_FM_SW, on && r.mode == RADIO_MODE_FM);	// polarity ?
}

// --- DMR per-burst RF switching: modem ISR context ---

bool md380_rf_dmr_switch(bool tx)
{
	if (!tx) {
		if (!pa_timer_running) {
			pa_off();	// else the one-shot ends the burst on time
		}
		return true;
	}
	if (!r.dmr_armed) {
		r.rf_unarmed++;
		return false;
	}
	if (!r.dmr_tx_tuned) {
		// First burst: receiver off, the HR_C5000's transmit set, the synthesizer to the transmit VCO
		rx_path(false);
		count(hrc5000_dmr_path(&r.modem, true, r.dmr_mod2, r.dmr_mod1, C5000_MIC_GAIN));
		if (!pll_tune(tx_vco_hz(r.dmr_tx_hz), true, false)) {
			r.rf_skipped++;
			return false;	// out of range: no RF
		}
		r.dmr_tx_tuned = true;
	}
	pa_timer_start();	// the key-up points, then the PA drop
	return true;
}

// The transmit side ready before a transmission's first burst (task level with the modem interrupts masked, or the
// timeslot ISR at tx_slot_found): the receiver off, the HR_C5000's transmit set, the synthesizer on the TX VCO and the
// transmitter supply up. Done from the RF TX interrupt instead, the first burst of every transmission went out unlocked
// and with a cold transmitter: a single-burst CSBK (repeater wake-up, radio check) reached no receiver, so a repeater
// or duplex hotspot never woke (issue 108). The supply alone doesn't radiate: APC_EN keys the PA at the burst
static void rf_dmr_prepare(void)
{
	if (!r.dmr_armed || r.dmr_tx_tuned) {
		return;
	}
	rx_path(false);
	count(hrc5000_dmr_path(&r.modem, true, r.dmr_mod2, r.dmr_mod1, C5000_MIC_GAIN));
	r.dmr_tx_tuned = pll_tune(tx_vco_hz(r.dmr_tx_hz), true, false);
	if (r.dmr_tx_tuned) {
		PIN_WRITE(PIN_TX_5V, true);
	}
}

// After rf_dmr_prepare, outside the lock: lock detect, as tx() waits for it in FM
static void rf_dmr_wait_lock(void)
{
	if (r.dmr_tx_tuned && !sky72310_wait_lock(&r.pll, PLL_LOCK_US)) {
		r.diag.pll_fails++;
	}
	r.diag.pll_locked = PIN_READ(PIN_PLL_LD);
}

// A transmission is over (the slot engine's TX done, task level, the modem interrupts masked by the caller): the
// receiver back on the RX frequency. On a split channel (repeater, duplex hotspot) the radio hears the repeater's
// answer to its wake-up: left on the TX side, every key-up there ended in no_repeater (issue 108). On simplex too: the
// receive LO is an IF away from the carrier, and a confirmed data response comes in the next frame, 30 ms after our
// last burst, before tx_off() (the radio task) gets here. Left on the TX side until then, the radio missed nearly every
// one (a receive CRC error instead), and the TX VCO kept leaking the carrier. Not per burst: the switch there caught
// the burst on air
static bool rf_dmr_tx_done(void)
{
	if (!r.dmr_armed || !r.dmr_tx_tuned || !r.rx_valid) {
		return false;
	}
	pa_timer_stop();
	pa_off();
	(void)pll_tune(rx_vco_hz(r.rx_hz), false, false);
	count(hrc5000_dmr_path(&r.modem, false, r.dmr_mod2, r.dmr_mod1, C5000_MIC_GAIN));
	dac1_set(r.rx_tune_dac);
	rx_path(true);
	r.dmr_tx_tuned = false;
	r.rf_rx_back++;
	return true;
}

static void rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx_back)
{
	*skipped = r.rf_skipped;
	*unarmed = r.rf_unarmed;
	*rx_back = r.rf_rx_back;
}

static void dmr_tx_end(void);

// The shared DMR glue's view of this board: the modem's interrupt lines are PC0-PC2
static const stm32_hrc_dmr_board_t dmr_board = {
	.int_port = GPIOC,
	.rf_switch = md380_rf_dmr_switch,
	.rf_counters = rf_dmr_counters,
	.tx_prepare = rf_dmr_prepare,
	.tx_wait_lock = rf_dmr_wait_lock,
	.tx_done = rf_dmr_tx_done,
	.tx_end = dmr_tx_end,
};

// Stop per-burst keying (task level): disarm, drop a transmission still running, PA off
static void dmr_disarm(void)
{
	r.dmr_armed = false;
	if (stm32_hrc_dmr_tx_active()) {
		stm32_hrc_dmr_tx_drop();
	}
	pa_timer_stop();
	pa_off();
}

static void dmr_enter(void)
{
	md380_cal_point_t cal;
	md380_cal_at(r.rx_valid ? r.rx_hz : board.bands[0].lo_hz, &cal);	// on false it fills safe defaults
#if CONFIG_DMR
	count(hrc6000_dmr_enter(hrc5000_hrc(&r.modem), cal.dmr_i, cal.dmr_q, C5000_MIC_GAIN));
#endif
	stm32_hrc_dmr_enter();
}

// --- Speaker (hardware-md380.md 6) ---
//
// FM receive audio reaches the amplifier through FM_MUTE and FM_SW from the FM receiver, DMR voice PCM through the
// HR_C5000's codec DAC (set by hrc5000_dmr_path) and DMR_SW (?). The volume is the analog knob after the amplifier
// input.
static bool audio_wanted;
static bool pcm_wanted;
static SemaphoreHandle_t speaker_lock;

static void speaker_apply(void)
{
	bool locked = speaker_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
	if (locked) {
		xSemaphoreTake(speaker_lock, portMAX_DELAY);
	}
	bool pcm = pcm_wanted;
	bool fm = audio_wanted && !pcm;
	if (fm || pcm) {
		PIN_WRITE(PIN_FM_MUTE, fm);
		PIN_WRITE(PIN_AMP_EN, true);
		PIN_WRITE(PIN_SPK_MUTE, false);
	} else {
		PIN_WRITE(PIN_SPK_MUTE, true);
		PIN_WRITE(PIN_AMP_EN, false);
		PIN_WRITE(PIN_FM_MUTE, false);
	}
	if (locked) {
		xSemaphoreGive(speaker_lock);
	}
}

#ifdef MD380_I2S_PROBE
// Bench knob (docs/testing/md380-bringup.md): edges on the I2S clock (PC10) and word select (PA15), read from the input
// registers while the pins stay on I2S, 20000 samples once a second, logged with the audio block count and the mode
#include <string.h>
#include "debuglink/dlog.h"
#include "hrc/i2s_pcm.h"
static void i2s_probe_task(void *arg)
{
	(void)arg;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		uint32_t ck = 0, ws = 0;
		bool lck = (GPIOC->IDR & GPIO_PIN_10) != 0, lws = (GPIOA->IDR & GPIO_PIN_15) != 0;
		for (int i = 0; i < 20000; i++) {
			bool c = (GPIOC->IDR & GPIO_PIN_10) != 0, w = (GPIOA->IDR & GPIO_PIN_15) != 0;
			ck += c != lck;
			ws += w != lws;
			lck = c;
			lws = w;
		}
		stm32_i2s_pcm_stats_t a;
		stm32_i2s_pcm_stats(&a);
		dlog("i2s ck %u ws %u blk %u %s%s", (unsigned)ck, (unsigned)ws, (unsigned)a.blocks,
			r.mode == RADIO_MODE_DMR ? "dmr" : "fm", r.tx_on ? " tx" : "");
#ifdef HRC5000_I2S_CYCLE
		extern volatile uint8_t hrc5000_i2s_cycle_value;
		static uint8_t shown_36 = 0xFF;
		if (hrc5000_i2s_cycle_value != shown_36) {
			shown_36 = hrc5000_i2s_cycle_value;
			dlog("0x36 now %02x", shown_36);
		}
#endif
#ifdef HRC5000_INT_LOG
		static uint8_t shown;
		while (shown < hrc_int_n) {
			const hrc_int_rec_t *q = &hrc_int_log[shown++];
			dlog("si%u f%02x 51=%02x 42=%02x dt%lu lc %02x%02x%02x %02x%02x%02x %02x%02x%02x", q->pass, q->flags,
				q->r51, q->r42, (unsigned long)q->dt, q->lc[0], q->lc[1], q->lc[2], q->lc[3], q->lc[4], q->lc[5],
				q->lc[6], q->lc[7], q->lc[8]);
		}
#endif
#ifdef HRC5000_TICK_LOG
		// A full timeline only (the ISR is done with it), 10 entries a second so the log keeps up; then re-arm
		static uint8_t tshown;
		if (hrc_tick_n >= HRC_TICK_LOG_N) {
			for (int k = 0; k < 10 && tshown < HRC_TICK_LOG_N; k++, tshown++) {
				const hrc_tick_rec_t *q = &hrc_tick_log[tshown];
				char hex[2 * sizeof(q->v) + 1];
				for (unsigned i = 0; i < sizeof(q->v); i++) {
					static const char d[] = "0123456789abcdef";
					hex[2 * i] = d[q->v[i] >> 4];
					hex[2 * i + 1] = d[q->v[i] & 15u];
				}
				hex[sizeof(hex) - 1] = 0;
				dlog("tl%03u %c%u f%02x 42=%02x 51=%02x 52=%02x dt%lu", tshown, q->kind, q->ph, q->flags,
					q->r42, q->r51, q->r52, (unsigned long)q->dt);
				if (q->kind == 'T') {	// the 54 hex digits don't fit one log line
					char half[29];
					memcpy(half, hex, 28);
					half[28] = 0;
					dlog("tv%03u %s", tshown, half);
					dlog("tw%03u %s", tshown, hex + 28);
				}
			}
			if (tshown >= HRC_TICK_LOG_N) {
				for (int sn = 0; sn < 2; sn++) {
					for (int o = 0; o < HRC_SNAP_LEN; o += 16) {
						char line[33];
						for (int i = 0; i < 16; i++) {
							static const char d[] = "0123456789abcdef";
							line[2 * i] = d[hrc_tick_snap[sn][o + i] >> 4];
							line[2 * i + 1] = d[hrc_tick_snap[sn][o + i] & 15u];
						}
						line[32] = 0;
						dlog("sn%u %03x %s", sn, o, line);
					}
				}
				tshown = 0;
				hrc_tick_armed = false;
				hrc_tick_n = 0;
			}
		}
#endif
	}
}
#endif

static void audio(bool on)
{
	audio_wanted = on;
	speaker_apply();
}

// The HR_C5000's I2S clocks run for the length of a PCM stream (audio_md380.c, the stream's clock hook). Its set-ups leave them off, and the
// STM32 is only the slave (hardware-md380.md 6.2)
void md380_i2s_clock(bool on)
{
	count(hrc5000_i2s_clock(&r.modem, on));
}

#if CONFIG_DMR
void md380_speaker_pcm(bool on)
{
	if (on == pcm_wanted) {
		return;
	}
	pcm_wanted = on;
	if (on && r.mode == RADIO_MODE_DMR) {
		count(hrc5000_speaker_voice(&r.modem, true));	// after the clocks run, before the amplifier opens
	}
	speaker_apply();
	if (!on) {
		count(hrc5000_speaker_voice(&r.modem, false));
	}
}
#endif

// --- hal_radio_t ---

static void init(void)
{
	if (speaker_lock == NULL) {
		speaker_lock = xSemaphoreCreateMutex();
#ifdef MD380_I2S_PROBE
		xTaskCreate(i2s_probe_task, "i2sp", 256, NULL, 1, NULL);
#endif
	}
	stm32_pin_in(PIN_PORT(PIN_C5000_MISO), PIN_MASK(PIN_C5000_MISO), GPIO_PULLUP);
	stm32_pin_in(PIN_PORT(PIN_PLL_LD), PIN_MASK(PIN_PLL_LD), GPIO_PULLUP);	// open collector (hardware-md380.md 5.2)
	PIN_WRITE(PIN_C5000_CS, true);
	PIN_WRITE(PIN_PLL_CS, true);
	PIN_WRITE(PIN_PLL_CLK, true);
	PIN_WRITE(PIN_C5000_CLK, false);
#ifndef MD380_KEEP_SWD
	// The SWD pins become the IF bandwidth switch and the microphone supply (hardware-md380.md 2.6)
	stm32_pin_out(PIN_PORT(PIN_WIDE), PIN_MASK(PIN_WIDE), false, false);
	stm32_pin_out(PIN_PORT(PIN_MIC_POWER), PIN_MASK(PIN_MIC_POWER), true, false);
#endif
	rx_path(false);
	dac_setup();
	pa_off();
	pa_timer_setup();
#if CONFIG_DMR
	pa_keyup_init(&keyup_cfg, KEYUP_MAX_AHEAD_US, DAC_MAX);
#endif
	voice_spi_setup();

	PIN_WRITE(PIN_C5000_PWD, false);
	uint32_t pwd_ms = hal_millis();

	r.pll.mux = 0;
	sky72310_attach(&r.pll, &pll_bus, MD380_TCXO_HZ, PLL_PD_GAIN);
	count(sky72310_init(&r.pll));

	while ((uint32_t)(hal_millis() - pwd_ms) < C5000_PWD_WAIT_MS) {
		hal_delay_ms(1);
	}
	hrc5000_attach(&r.modem, &modem_bus, RFOUNDRY_BAND_VHF);
	r.mode = RADIO_MODE_FM;
	count(hrc5000_power_on(&r.modem));		// the clock, then FM receive
	audio(false);
	// 0xFF is a floating data line, never a register value here (FM receive leaves 0x10 = 0x80)
	r.diag.c5000_ok = hrc_read(hrc5000_hrc(&r.modem), HRC_PAGE_SYS, 0x10, &r.diag.c5000_r10) && r.diag.c5000_r10 != 0xFF;
	stm32_hrc_dmr_init(hrc5000_hrc(&r.modem), &dmr_board);
}

static void set_mode(radio_mode_t mode)
{
	radio_mode_t prev = r.mode;
	r.mode = mode;
	if (mode == RADIO_MODE_DMR) {
		if (prev != RADIO_MODE_DMR) {
			dmr_enter();
		}
	} else if (prev == RADIO_MODE_DMR) {
		dmr_disarm();
		stm32_hrc_dmr_leave();
		r.dmr_tx_tuned = false;
		r.tx_on = false;
		count(hrc5000_set_mode_fm_rx(&r.modem));
	}
	if (r.rx_valid && !r.tx_on && !r.asleep) {
		rx_path(true);	// FM_SW follows the mode
	}
}

// Enter RX: PA off, synthesizer to the receive VCO, receiver supply, front-end tuning, reference, HR_C5000. keep (the
// end of a DMR transmission, dmr_tx_end): the TX done already put the synthesizer on the receive VCO; on the same
// channel it stays as it is
static bool rx_set(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone, bool keep)
{
	if (find_band(hz) == NULL) {
		return false;
	}
	bool same = r.rx_valid && hz == r.rx_hz && bw == r.rx_bw;
#if CONFIG_SUBTONE
	// NULL (back from transmit, wake-up) keeps the last tone; a new tone or channel restarts the detector
	if (rx_tone != NULL && (!same || rx_tone->type != r.rx_tone.type || rx_tone->value != r.rx_tone.value)) {
		r.rx_tone = *rx_tone;
		md380_subtone_rx_set(&r.rx_tone);
	} else if (rx_tone == NULL && !same) {
		md380_subtone_rx_set(&r.rx_tone);
	}
#else
	(void)rx_tone;
#endif
	bool from_tx = r.dmr_tx_tuned;
	// On rx_hz since the TX done: a retune now would hit a reply in the next frame
	bool back = keep && r.mode == RADIO_MODE_DMR && same && !r.asleep;
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();
	}
	r.dmr_tx_tuned = false;
	r.asleep = false;
	r.rx_hz = hz;
	r.rx_bw = bw;
	r.rx_valid = true;

	pa_off();
	set_wide(bw == RADIO_BW_25K);
	if (!back) {
		(void)pll_tune(rx_vco_hz(hz), false, true);	// a failure shows on the diagnostics page
	}
	rx_path(true);

	md380_cal_point_t cal;
	md380_cal_at(hz, &cal);	// on false it fills safe defaults
	r.rx_tune_dac = cal.rx_tune_dac;
	dac1_set(cal.rx_tune_dac);
	r.ref_tune = cal.ref_tune;
	ref_dac_apply();
	count(hrc5000_set_ref(&r.modem, cal.ref_tune));
	if (r.mode == RADIO_MODE_FM) {
		count(hrc5000_set_mode_fm_rx(&r.modem));
	} else {
		if (from_tx || !same) {
			count(hrc5000_dmr_path(&r.modem, false, cal.dmr_i, cal.dmr_q, C5000_MIC_GAIN));
		}
		if (!same) {
			count(hrc_set_mod_gain(hrc5000_hrc(&r.modem), cal.dmr_i, cal.dmr_q));
			stm32_hrc_dmr_rx_restart();
		}
	}
	r.tx_on = false;
	return true;
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	return rx_set(hz, bw, rx_tone, false);
}

// The PA drive set over the debug link for the next transmission (power calibration; hal_radio_t.tx_drive), 0 = the
// level's. Any task sets it; tx_off clears it
static volatile uint16_t drive_override;

// Board power level -> DAC1 value (hardware-md380.md 5.6). Five levels: 2 and 4 are the calibration's 1 W and 5 W
// rows, 0 and 1 fractions of the 1 W row's drive, 3 between the rows. Draft values, to be set with a wattmeter
static uint32_t power_to_dac(const md380_cal_point_t *cal, uint8_t level)
{
	uint32_t lo = cal->power_dac[0], hi = cal->power_dac[1];
	switch (level) {
	case 0: return lo * 565u / 1000u;
	case 1: return lo * 807u / 1000u;
	case 2: return lo;
	case 3: return hi > lo ? lo + (hi - lo) * 45u / 100u : lo;
	default: return hi;
	}
}

// Enter FM TX: receiver off, HR_C5000 FM TX, synthesizer to the transmit VCO and locked, then the PA. Refused
// without an accepted calibration table (the APC values would be guesses)
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
#if !CONFIG_SUBTONE
	(void)tx_tone;
#endif
	const band_t *band = find_band(hz);
	if (band == NULL || !band->tx) {
		return false;
	}
	if (board.power_count > 0 && power >= board.power_count) {
		power = (uint8_t)(board.power_count - 1);
	}
	md380_cal_point_t cal;
	if (!md380_cal_at(hz, &cal) || !cal.valid) {
		return false;
	}
	uint32_t drive = power_to_dac(&cal, power);
	if (drive_override != 0u) {
		drive = drive_override;
	}

	if (r.mode == RADIO_MODE_DMR) {
		// Arm only: the receiver stays on the RX frequency until the first burst (md380_rf_dmr_switch)
		r.dmr_tx_hz = hz;
		r.dmr_drive = drive;
		r.dmr_mod2 = cal.dmr_i;
		r.dmr_mod1 = cal.dmr_q;
		// The burst's key-up points (radio/pa_keyup.h), against the top level's drive here
		uint16_t top = (uint16_t)power_to_dac(&cal, (uint8_t)(board.power_count - 1));
		uint16_t ref = cal.power_dac[0];	// the dead zone's anchor: the calibration's low power drive
#if CONFIG_DMR
		r.ku = pa_keyup_plan((uint16_t)drive, top, ref);
#else
		r.ku = pa_keyup_drives(&keyup_cfg, (uint16_t)drive, top, ref, DAC_MAX);
#endif
		r.key_ns = pa_keyup_steps(r.key_pt, pa_keyup_points(&r.ku, r.key_pt), DMR_KEY_INT_AHEAD_US, DMR_KEY_MIN_US,
					  r.key_st);
		__DMB();
		r.dmr_armed = true;
		r.tx_on = true;
		stm32_hrc_dmr_tx_prepare();	// a transmission the stack has already begun gets its first burst too
		return true;
	}

	rx_path(false);
	count(hrc5000_set_ref(&r.modem, cal.ref_tune));
	count(hrc5000_set_mode_fm_tx(&r.modem, bw == RADIO_BW_25K, cal.fm_i, cal.fm_q));
	set_wide(bw == RADIO_BW_25K);
	if (!pll_tune(tx_vco_hz(hz), true, true)) {
		// Not locked: no carrier off frequency. Back to receive
		r.tx_on = false;
		rx(r.rx_hz, r.rx_bw, NULL);
		return false;
	}
	pa_on(drive);
	r.tx_on = true;
#if CONFIG_SUBTONE
	if (tx_tone != NULL) {
		md380_subtone_tx_start(tx_tone);	// PWM on PC7 into the modulator (hardware-md380.md 6.4)
	}
#endif
	return true;
}

#if CONFIG_SUBTONE
// The DCS turn-off tail from the PWM generator (hardware-md380.md 6.4); the core holds the carrier for it
static bool tx_tail(bool on)
{
	return r.tx_on && r.mode == RADIO_MODE_FM && md380_subtone_tx_tail(on);
}
#endif

// keep: as in rx_set
static void tx_end(bool keep)
{
#if CONFIG_SUBTONE
	md380_subtone_tx_stop();
#endif
	if (r.mode == RADIO_MODE_DMR) {
		dmr_disarm();	// after DMR_EV_TX_DONE this only disarms; before it, the transmission is cut
	}
	pa_off();
	if (r.rx_valid) {
		rx_set(r.rx_hz, r.rx_bw, NULL, keep);
		return;
	}
	r.dmr_tx_tuned = false;
	count(hrc5000_set_mode_fm_rx(&r.modem));
	r.tx_on = false;
}

static void tx_off(void)
{
	tx_end(false);
	drive_override = 0u;
}

static uint16_t tx_drive_level(uint32_t hz, uint8_t level)
{
	md380_cal_point_t cal;
	return md380_cal_at(hz, &cal) && cal.valid ? (uint16_t)power_to_dac(&cal, level) : 0u;
}

static bool tx_drive(uint16_t drive)
{
	if (drive > DAC_MAX) {
		return false;
	}
	drive_override = drive;
	return true;
}

// stm32_hrc_dmr_board_t.tx_end: the TX done left the synthesizer receiving
static void dmr_tx_end(void)
{
	tx_end(true);
}

static void status(radio_status_t *st)
{
	uint32_t rssi = md380_adc_raw(MD380_ADC_RSSI);
	uint32_t busy = md380_adc_raw(MD380_ADC_BUSY);
	st->rssi_dbm = (int16_t)(RSSI_FLOOR_DBM + (int32_t)(rssi * RSSI_SPAN_DB / 4095u));
	st->noise = (uint8_t)(busy >> 4);	// noise detector level, larger = more noise (?)
	st->carrier = st->rssi_dbm >= CARRIER_DBM;
#if CONFIG_SUBTONE
	st->tone_ok = md380_subtone_rx_match();	// true for a channel without an RX tone
#else
	st->tone_ok = true;
#endif
}

// Receiver battery saving (FM receive only): receiver supply off and the synthesizer powered down
static void rx_sleep(bool sleep)
{
	if (r.mode != RADIO_MODE_FM || r.tx_on || !r.rx_valid || sleep == r.asleep) {
		return;
	}
	if (sleep) {
		rx_path(false);
		count(sky72310_power_down(&r.pll, true));
		r.asleep = true;
	} else {
		count(sky72310_power_down(&r.pll, false));
		rx(r.rx_hz, r.rx_bw, NULL);
	}
}

const hal_radio_t md380_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.dmr_tx_end = stm32_hrc_dmr_tx_end,
#if CONFIG_SUBTONE
	.tx_tail = tx_tail,
#endif
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
	.tx_drive_level = tx_drive_level,
	.tx_drive = tx_drive,
};

void md380_radio_diag(md380_radio_diag_t *d)
{
	*d = r.diag;
	d->bus_errors = r.bus_errors;
}
