// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 radio: the hal_radio_t on top of the RF sequencer (rfseq_md9600.c), for every board revision
// (docs/spec/hardware-md9600.md 5, 6, 16, 19).
//
// Each band has its own synthesizer, RX and TX VCOs, IF receiver and supplies; the HR_C6000 isn't in the FM receive
// path (the IF chip's discriminator feeds the audio switches). This file maps the sequencer's lines to pins, its buses
// to the bit-banged three-wire writer, and picks the revision at start-up: the stored record, the optional receiver
// probe, the build default (rev_md9600.c). Transmit stays refused unless the build has CONFIG_MD9600_TX, a person
// armed it for this power cycle and the revision is a stored one (rfseq_md9600.h).
//
// Both synthesizers share one bit-banged bus (chip selects PD8 / PD9), and so do both IF chips (PE11 / PE12). Only the
// radio task and board_init use them.
#include <stddef.h>
#include "md9600.h"
#include "rfseq_md9600.h"
#include "config.h"
#include "pins.h"
#include "periph_md9600.h"
#include "hal_time.h"

// The revision driven when none is stored and the probe has no verdict: one constant, set by the build
// (board.mk MD9600_REV_DEFAULT). Which revision that should be is the owner's decision
#ifndef MD9600_REV_DEFAULT
#define MD9600_REV_DEFAULT	MD9600_REV_V5
#endif
#ifndef MD9600_REV_PROBE
#define MD9600_REV_PROBE	0
#endif

#define PLL_HALF_NS		1000u	// about 500 kHz: well inside every synthesizer's timing
#define IF_HALF_NS		600u	// the AK2365A needs 500 ns high and low
#define CARRIER_DBM		(-118)	// placeholder until the RSSI curves are measured

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

// md9600_line_t to pins
typedef struct {
	GPIO_TypeDef *port;
	uint16_t pin;
} pin_t;

static const pin_t lines[MD9600_LN_COUNT] = {
	[MD9600_LN_POWER] = { PIN_POWER },
	[MD9600_LN_R5_V] = { PIN_R5_V },
	[MD9600_LN_R5_U] = { PIN_R5_U },
	[MD9600_LN_C5_V] = { PIN_C5_V },
	[MD9600_LN_C5_U] = { PIN_C5_U },
	[MD9600_LN_T5_V] = { PIN_T5_V },
	[MD9600_LN_T5_U] = { PIN_T5_U },
	[MD9600_LN_APC] = { PIN_APC_EN },
	[MD9600_LN_VCO_V] = { PIN_VCO_V },
	[MD9600_LN_VCO_U] = { PIN_VCO_U },
	[MD9600_LN_MOD_VHF] = { PIN_MOD_SW },
	[MD9600_LN_IF_RST] = { PIN_IF_RST },
	[MD9600_LN_V5_BW_V] = { PIN_IF_CS_V },
	[MD9600_LN_V5_BW_U] = { PIN_IF_CS_U },
	[MD9600_LN_V5_ROUTE_V] = { PIN_IF_DATA },
	[MD9600_LN_V5_ROUTE_U] = { PIN_IF_CLK },
	[MD9600_LN_FM_SW] = { PIN_FM_SW },
	[MD9600_LN_AF_VHF] = { PIN_AF_BAND },
};

static md9600_rev_state_t rev;
static md9600_rf_t rf;
static int16_t probe_result = -1;
static bool dmr_mode;
static uint16_t sql_l1, sql_l9;		// the squelch thresholds at the receive frequency (equal: uncalibrated)

// --- Sequencer ops ---

static void op_line(void *ctx, uint8_t line, bool level)
{
	(void)ctx;
	if (line < MD9600_LN_COUNT) {
		stm32_pin_write(lines[line].port, lines[line].pin, level);
	}
}

static bool op_pll(void *ctx, uint8_t band, uint32_t word, uint8_t bits)
{
	(void)ctx;
	return stm32_wire3_write((void *)&pll_wire, band, word, bits);
}

static bool op_if(void *ctx, uint8_t band, uint16_t word)
{
	(void)ctx;
	return stm32_wire3_write((void *)&if_wire, band, word, 16);
}

static void op_dac(void *ctx, uint8_t ch, uint16_t value)
{
	(void)ctx;
	if (ch == MD9600_DAC_TUNE_U) {
		DAC->DHR12R1 = value;
	} else {
		DAC->DHR12R2 = value;
	}
}

// Whole milliseconds sleep the task once the RTOS runs; the rest is a busy wait
static void op_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	if (us >= 1000u) {
		hal_delay_ms(us / 1000u);
		us %= 1000u;
	}
	if (us != 0) {
		hal_delay_us(us);
	}
}

static uint16_t op_rx_tune(void *ctx, uint32_t hz)
{
	(void)ctx;
	return md9600_cal_rx_tune(hz);
}

static bool op_apc(void *ctx, uint32_t hz, uint8_t power, uint16_t *dac)
{
	(void)ctx;
	return md9600_cal_apc(hz, power, dac);
}

static uint16_t op_noise(void *ctx, uint8_t band)
{
	(void)ctx;
	return md9600_adc_raw(band == MD9600_VHF ? MD9600_ADC_NOISE_V : MD9600_ADC_NOISE_U);
}

static const md9600_rf_ops_t ops = {
	.line = op_line,
	.pll = op_pll,
	.ifw = op_if,
	.dac = op_dac,
	.delay_us = op_delay,
	.locked = NULL,		// no lock detect line is known on any revision
	.rx_tune = op_rx_tune,
	.apc = op_apc,
	.noise = op_noise,
};

// --- State for the diagnostics page and the board option ---

md9600_rev_state_t md9600_rev_state(void)
{
	return rev;
}

void md9600_radio_diag(md9600_radio_diag_t *d)
{
	d->lo_hz = rf.lo_hz;
	d->band = rf.band;
	d->state = rf.state;
	d->retunes = rf.retunes;
	d->refused = rf.refused;
	d->errors = rf.errors;
	d->last_err = rf.last_err;
	d->tx_armed = rf.tx_armed;
	d->probe = probe_result;
}

bool md9600_radio_tx_arm(bool on)
{
	return md9600_rf_tx_arm(&rf, on);
}

// --- Start-up ---

static void dac_init(void)
{
	stm32_pin_analog(GPIOA, PIN_MASK(PIN_TUNE_U_DAC) | PIN_MASK(PIN_APC_TUNE_V_DAC));
	RCC->APB1ENR |= RCC_APB1ENR_DACEN;
	(void)RCC->APB1ENR;
	DAC->DHR12R1 = 0;
	DAC->DHR12R2 = 0;
	DAC->CR = DAC_CR_EN1 | DAC_CR_EN2;
}

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
		probed = md9600_rf_probe(&ops, MD9600_PROBE_HZ, MD9600_PROBE_THRESHOLD);
		probe_result = (int16_t)probed;
	}
#endif
	rev = md9600_rev_resolve(stored, probed, MD9600_REV_DEFAULT);
	const md9600_rev_desc_t *d = rev.usable ? md9600_rev_desc(rev.rev) : NULL;
	if (d != NULL && d->if_chip) {
		PIN_OUT(PIN_IF_RST, true);	// V1-V4: the AK2365A reset; left an input on V5 (use unknown)
	}
	// An unusable revision (stored, but not in this image) gets MD9600_REV_UNKNOWN: everything stays off
	md9600_rf_init(&rf, &ops, d != NULL ? rev.rev : MD9600_REV_UNKNOWN, rev.confirmed);
	(void)md9600_rf_power_up(&rf);
}

// --- hal_radio_t ---

static void init(void)
{
	md9600_rf_pa_safe(&rf);
	md9600_c6000_init();
}

static void set_mode(radio_mode_t mode)
{
	dmr_mode = mode == RADIO_MODE_DMR;
	md9600_rf_set_dmr(&rf, dmr_mode);
	md9600_c6000_mode(dmr_mode, rf.rx_hz);
}

static void audio(bool on)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(rf.rx_hz, &band);
	md9600_speaker_fm(on, band == MD9600_VHF);	// with the PCM requests (c6000_md9600.c)
}

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
	(void)rx_tone;	// no sub-audio decode yet (CONFIG_SUBTONE is off on this board)
	if (md9600_rf_rx(&rf, hz, bw == RADIO_BW_25K) != MD9600_RF_OK) {
		return false;
	}
	if (!md9600_cal_squelch(hz, &sql_l1, &sql_l9)) {
		sql_l1 = sql_l9 = 0;	// once per retune, not per status poll
	}
	md9600_c6000_retune(hz);
	return true;
}

static bool tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone)
{
	(void)tx_tone;
	return md9600_rf_tx(&rf, hz, bw == RADIO_BW_25K, power) == MD9600_RF_OK;
}

static void tx_off(void)
{
	md9600_rf_tx_off(&rf);
}

static void rx_sleep(bool sleep)
{
	if (sleep) {
		md9600_rf_sleep(&rf);
	} else if (rf.rx_valid) {
		(void)md9600_rf_rx(&rf, rf.rx_hz, rf.rx_wide);
	}
}

static void status(radio_status_t *st)
{
	uint8_t band = MD9600_UHF;
	(void)md9600_band_of(rf.rx_hz, &band);
	uint16_t rssi = md9600_adc_raw(band == MD9600_VHF ? MD9600_ADC_RSSI_V : MD9600_ADC_RSSI_U);
	uint16_t noise = md9600_adc_raw(band == MD9600_VHF ? MD9600_ADC_NOISE_V : MD9600_ADC_NOISE_U);
	st->rssi_dbm = md9600_rf_rssi_dbm(rf.rev, band, rssi);
	st->noise = md9600_rf_noise(noise, sql_l1, sql_l9);
	st->carrier = rf.state == MD9600_RF_RX && st->rssi_dbm >= CARRIER_DBM;
	st->tone_ok = true;
}

// ADC3 IN2 (PA2, the sub-audio input), one software-started conversion. The common prescaler is the ADC1 scan's
uint16_t md9600_adc3_qt(void)
{
	if (!(RCC->APB2ENR & RCC_APB2ENR_ADC3EN)) {
		stm32_pin_analog(PIN_PORT(PIN_QT_IN), PIN_MASK(PIN_QT_IN));
		RCC->APB2ENR |= RCC_APB2ENR_ADC3EN;
		(void)RCC->APB2ENR;
		ADC3->CR1 = 0;
		ADC3->SMPR2 = 7u << ADC_SMPR2_SMP2_Pos;	// 480 cycles
		ADC3->SQR1 = 0;
		ADC3->SQR3 = 2;
		ADC3->CR2 = ADC_CR2_ADON;
	}
	ADC3->SR = 0;
	ADC3->CR2 |= ADC_CR2_SWSTART;
	for (uint32_t i = 0; i < 100000u && !(ADC3->SR & ADC_SR_EOC); i++) {
	}
	return (uint16_t)(ADC3->DR & 0xFFFu);
}

const hal_radio_t md9600_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
};
