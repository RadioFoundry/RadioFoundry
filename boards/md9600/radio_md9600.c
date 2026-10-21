// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 radio: FM receive on either band, for every board revision (docs/spec/hardware-md9600.md 5, 6, 16).
//
// Each band has its own synthesizer, RX and TX VCOs, IF receiver and supplies; the HR_C6000 isn't in the FM receive
// path (the IF chip's discriminator feeds the audio switches). rx() programs the band's synthesizer to the LO from the
// revision's plan (rfplan_md9600.c), selects the RX VCO, sets the front-end tuning DAC and the IF stage, and routes
// that band's audio. Transmit is refused: it needs a confirmed revision, the PA calibration and the HR_C6000 glue.
//
// Both synthesizers share one bit-banged bus (chip selects PD8 / PD9), and so do both IF chips (PE11 / PE12). Only the
// radio task and board_init use them.
#include <stddef.h>
#include "md9600.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"

#ifndef MD9600_REV_DEFAULT
#define MD9600_REV_DEFAULT	MD9600_REV_V5
#endif
#ifndef MD9600_REV_PROBE
#define MD9600_REV_PROBE	0
#endif

#define PLL_HALF_NS		1000u	// about 500 kHz: well inside every synthesizer's timing
#define IF_HALF_NS		600u	// the AK2365A needs 500 ns high and low
#define SETTLE_MS		5u	// after a retune, before the receiver is trusted (unverified)
#define LMX_CAL_MS		2u	// LMX2571 calibration after a new N (unverified)
#define IF_CAL_MS		2u	// AK2365A discriminator calibration takes 1.3 ms
#define V4_KICK_HZ		145000000u	// V4: VHF programmed at power-on or it never locks

// RSSI curves (hardware-md9600.md 5.7, a fit, unverified): dBm = floor + raw8 / 2 (V5: / 1.95)
#define RSSI_FLOOR_V		(-135)
#define RSSI_FLOOR_U		(-145)
#define RSSI_FLOOR_V5		(-155)
#define CARRIER_DBM		(-118)	// placeholder until the curves are measured

#define PROBE_HZ		446000000u
#define PROBE_THRESHOLD		200u	// ADC counts; unmeasured, so the probe is off by default (board.mk)

static const stm32_wire3_t pll_wire = {
	.clk_port = GPIOD, .clk_pin = GPIO_PIN_11,
	.data_port = GPIOD, .data_pin = GPIO_PIN_10,
	.cs_port = { GPIOD, GPIOD }, .cs_pin = { GPIO_PIN_8, GPIO_PIN_9 },	// index = md9600_band_t
	.cs_count = 2,
	.half_period_ns = PLL_HALF_NS,
};

static const stm32_wire3_t if_wire = {
	.clk_port = GPIOE, .clk_pin = GPIO_PIN_14,
	.data_port = GPIOE, .data_pin = GPIO_PIN_13,
	.cs_port = { GPIOE, GPIOE }, .cs_pin = { GPIO_PIN_11, GPIO_PIN_12 },
	.cs_count = 2,
	.half_period_ns = IF_HALF_NS,
};

static md9600_rev_state_t rev;
static const md9600_rev_desc_t *desc;
static md9600_radio_diag_t diag = { .probe = -1 };
static uint32_t rx_hz;
static radio_bw_t rx_bw;
static bool rx_on;

md9600_rev_state_t md9600_rev_state(void)
{
	return rev;
}

void md9600_radio_diag(md9600_radio_diag_t *d)
{
	*d = diag;
}

// --- Lines ---

static void tx_lines_off(void)
{
	PIN_WRITE(PIN_APC_EN, false);
	PIN_WRITE(PIN_T5_V, false);
	PIN_WRITE(PIN_T5_U, false);
}

// The receive side of both bands: both receivers and C5 supplies are always powered together
static void receivers_on(void)
{
	tx_lines_off();
	PIN_WRITE(PIN_POWER, true);
	PIN_WRITE(PIN_R5_U, true);
	PIN_WRITE(PIN_R5_V, true);
	PIN_WRITE(PIN_C5_U, true);
	PIN_WRITE(PIN_C5_V, true);
	PIN_WRITE(PIN_VCO_U, true);
	PIN_WRITE(PIN_VCO_V, true);
}

static void dac_init(void)
{
	stm32_pin_analog(GPIOA, PIN_MASK(PIN_TUNE_U_DAC) | PIN_MASK(PIN_APC_TUNE_V_DAC));
	RCC->APB1ENR |= RCC_APB1ENR_DACEN;
	(void)RCC->APB1ENR;
	DAC->DHR12R1 = 0;
	DAC->DHR12R2 = 0;
	DAC->CR = DAC_CR_EN1 | DAC_CR_EN2;
}

// --- Synthesizer and IF ---

static bool pll_write(const md9600_pll_plan_t *p)
{
	bool ok = true;
	for (uint8_t i = 0; i < p->count; i++) {
		ok &= stm32_wire3_write((void *)&pll_wire, p->band, p->words[i], p->bits);
	}
	if (desc->pll == MD9600_PLL_LMX2571) {
		hal_delay_ms(LMX_CAL_MS);
	}
	return ok;
}

static void if_set(md9600_rev_t r, uint8_t band, bool wide)
{
	const md9600_rev_desc_t *d = md9600_rev_desc(r);
	if (d == NULL) {
		return;
	}
	if (d->if_chip) {
		uint16_t w[MD9600_IF_WORDS];
		uint8_t n = md9600_if_words(wide, w);
		for (uint8_t i = 0; i < n; i++) {
			(void)stm32_wire3_write((void *)&if_wire, band, w[i], 16);
		}
		hal_delay_ms(IF_CAL_MS);
	} else {
		uint8_t lines = md9600_if_lines_v5(band, wide);
		PIN_WRITE(PIN_IF_CS_V, (lines & 1u) != 0);
		PIN_WRITE(PIN_IF_CS_U, (lines & 2u) != 0);
		PIN_WRITE(PIN_IF_DATA, (lines & 4u) != 0);
		PIN_WRITE(PIN_IF_CLK, (lines & 8u) != 0);
	}
}

// --- Revision: stored record, optional probe, default ---

#if MD9600_REV_PROBE
// Program the UHF synthesizer with each built revision's words for a receive frequency and watch the UHF noise level
// against the unprogrammed baseline. Receive side only: the transmitter supplies and the PA loop are held off, the RX
// VCOs selected, and PE15 (unknown on V5) is never driven
static md9600_rev_t probe(void)
{
	md9600_rev_t cand[MD9600_REV_COUNT];
	uint16_t level[MD9600_REV_COUNT];
	uint8_t n = 0;
	receivers_on();
	hal_delay_ms(50);
	uint16_t base = md9600_adc_raw(MD9600_ADC_NOISE_U);
	for (int r = MD9600_REV_COUNT - 1; r > MD9600_REV_UNKNOWN; r--) {
		md9600_pll_plan_t p;
		if (!md9600_pll_plan((md9600_rev_t)r, PROBE_HZ, false, &p)) {
			continue;
		}
		tx_lines_off();
		desc = md9600_rev_desc((md9600_rev_t)r);
		(void)pll_write(&p);
		if_set((md9600_rev_t)r, MD9600_UHF, true);
		hal_delay_ms(50);
		cand[n] = (md9600_rev_t)r;
		level[n++] = md9600_adc_raw(MD9600_ADC_NOISE_U);
	}
	desc = NULL;
	md9600_rev_t pick = md9600_rev_probe_pick(base, level, cand, n, PROBE_THRESHOLD);
	diag.probe = (int16_t)pick;
	return pick;
}
#endif

void md9600_radio_early(void)
{
	dac_init();
	// The receiver supplies are the SWD pins: from here a debugger connects under reset only
	PIN_OUT(PIN_R5_U, false);
	PIN_OUT(PIN_R5_V, false);

	md9600_rev_t probed = MD9600_REV_UNKNOWN;
	md9600_rev_t stored = md9600_flash_rev_load();
#if MD9600_REV_PROBE
	if (stored == MD9600_REV_UNKNOWN) {
		probed = probe();
	}
#endif
	rev = md9600_rev_resolve(stored, probed, MD9600_REV_DEFAULT);
	desc = rev.usable ? md9600_rev_desc(rev.rev) : NULL;
	if (desc == NULL) {
		return;		// no receiver until the revision is one this image drives
	}
	if (desc->if_chip) {
		// V1-V4: the AK2365A's reset line, pulsed after power-up
		receivers_on();
		PIN_OUT(PIN_IF_RST, false);
		hal_delay_us(10);
		PIN_WRITE(PIN_IF_RST, true);
	}
	if (desc->vhf_tune_at_power_on) {
		md9600_pll_plan_t p;
		receivers_on();
		if (md9600_pll_plan(rev.rev, V4_KICK_HZ, false, &p)) {
			(void)pll_write(&p);
		}
	}
}

// --- hal_radio_t ---

static void init(void)
{
	tx_lines_off();
}

static void set_mode(radio_mode_t mode)
{
	(void)mode;	// FM only for now
}

static void audio(bool on)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(rx_hz, &band);
	PIN_WRITE(PIN_AF_BAND, band == MD9600_VHF);
	PIN_WRITE(PIN_FM_MUTE, on);
	PIN_WRITE(PIN_AF_MUTE, !on);
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	(void)rx_tone;	// no sub-audio decode yet (CONFIG_SUBTONE is off on this board)
	md9600_pll_plan_t p;
	if (desc == NULL || !md9600_pll_plan(rev.rev, hz, false, &p)) {
		diag.refused++;
		return false;
	}
	receivers_on();
	if (!pll_write(&p)) {
		diag.refused++;
		return false;
	}
	uint16_t tune = md9600_cal_rx_tune(hz);
	if (p.band == MD9600_VHF) {
		DAC->DHR12R2 = tune;
	} else {
		DAC->DHR12R1 = tune;
	}
	if_set(rev.rev, p.band, bw == RADIO_BW_25K);
	PIN_WRITE(PIN_FM_SW, true);
	PIN_WRITE(PIN_AF_BAND, p.band == MD9600_VHF);
	hal_delay_ms(SETTLE_MS);
	rx_hz = hz;
	rx_bw = bw;
	rx_on = true;
	diag.lo_hz = p.lo_hz;
	diag.band = p.band;
	diag.retunes++;
	return true;
}

// Refused: no confirmed PA calibration yet, and the HR_C6000 (modulation) isn't driven
static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)hz;
	(void)bw;
	(void)power;
	(void)tx_tone;
	tx_lines_off();
	diag.refused++;
	return false;
}

static void tx_off(void)
{
	tx_lines_off();
	if (rx_on) {
		(void)rx(rx_hz, rx_bw, NULL);
	}
}

static void status(radio_status_t *st)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(rx_hz, &band);
	uint16_t rssi = md9600_adc_raw(band == MD9600_VHF ? MD9600_ADC_RSSI_V : MD9600_ADC_RSSI_U);
	uint16_t noise = md9600_adc_raw(band == MD9600_VHF ? MD9600_ADC_NOISE_V : MD9600_ADC_NOISE_U);
	int32_t raw8 = rssi >> 4;
	int32_t dbm;
	if (desc != NULL && desc->pll == MD9600_PLL_LMX2571) {
		dbm = RSSI_FLOOR_V5 + raw8 * 100 / 195;
	} else {
		dbm = (band == MD9600_VHF ? RSSI_FLOOR_V : RSSI_FLOOR_U) + raw8 / 2;
	}
	st->rssi_dbm = (int16_t)dbm;
	st->noise = (uint8_t)(noise >> 4);
	st->carrier = rx_on && dbm >= CARRIER_DBM;
	st->tone_ok = true;
}

const hal_radio_t md9600_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
};
