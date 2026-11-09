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
#if CONFIG_MD9600_DMR_TX
#include "hrc/dmr_glue.h"
#endif
#if CONFIG_DTMF
#include "hal_dtmf.h"
#endif

// The revision driven when none is stored and the probe has no verdict: one constant, set by the build
// (board.mk MD9600_REV_DEFAULT): V5, the current production board
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

// Whole milliseconds sleep the task once the RTOS runs; the rest is a busy wait. In an interrupt (the DMR transmit
// side tuned from the timeslot interrupt) all of it is a busy wait
static void op_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	if (__get_IPSR() != 0u) {
		hal_delay_us(us);
		return;
	}
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

// The PA drive set over the debug link for the next transmission (power calibration; hal_radio_t.tx_drive), 0 = the
// level's. Any task sets it; tx_off clears it
static volatile uint16_t drive_override;

static bool op_apc(void *ctx, uint32_t hz, uint8_t power, uint16_t *dac)
{
	(void)ctx;
	uint16_t o = drive_override;
	if (o != 0) {
		uint16_t lv;	// only where the level itself would transmit: band, calibration
		if (!md9600_cal_apc(hz, power, &lv)) {
			return false;
		}
		*dac = o;
		return true;
	}
	return md9600_cal_apc(hz, power, dac);
}

static uint16_t op_apc_floor(void *ctx, uint8_t band)
{
	(void)ctx;
	return md9600_cal_apc_floor(band);
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
	.apc_floor = op_apc_floor,
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
	d->apc = rf.apc;
	d->rx_hz = rf.rx_hz;
}

bool md9600_radio_tx_arm(bool on)
{
	bool ok = md9600_rf_tx_arm(&rf, on);
#if CONFIG_MD9600_DMR_TX
	md9600_dmr_tx_allow(on && ok);	// DMR bursts follow the same arm
#endif
	return ok;
}

// --- DMR transmit: the per-burst RF switching (hardware-md9600.md 5.5, 19.8) ---
//
// tx() in DMR only arms. The glue's tx_prepare (at tx_begin under the modem lock, or in the timeslot interrupt for an
// aligned start) puts the TX VCO on the carrier without the waits; tx_wait_lock (task level) waits the synthesizer's
// settle time. Each burst then switches the band's transmitter supply, the PA loop and a short APC ramp from the modem
// interrupts (md9600_rf_pa_burst). The receiver comes back at each transmission's end (md9600_rf_dmr_done), simplex
// too, and the core ends the transmission through the glue (hal_radio_t.dmr_tx_end), whose tx_end hook
// (md9600_dmr_tx_end) leaves the receiver where that put it (gotchas 3.14)
#if CONFIG_MD9600_DMR_TX
static volatile bool dmr_armed;
static volatile bool dmr_tuned;
static uint32_t dmr_hz;
static uint8_t dmr_power;
static uint32_t dmr_skipped, dmr_unarmed;
static uint32_t dmr_rx_back;	// returns to receive at a transmission's end
static volatile bool dmr_rx_early;	// ... done at the last burst's PA drop (md9600_rf_dmr_key)
#if CONFIG_SFR
// An SFR receives between its bursts (docs/spec/sfr.md 3 item 1): one synthesizer, so each burst's end retunes to
// the receive LO, and the next RF TX interrupt, moved this far ahead of the slot, retunes to the carrier and waits for
// the lock before the PA (gotchas 14.6)
// Swept on air (V1-V3, UHF, 2026-11-03): 3.0 ms lead and the 2 ms settle cut the end of the inbound burst (its third
// voice frame took most of the corrected bits); 1.5 ms with 0.6 ms repeats every burst and leaves the third frame clean
#define SFR_TX_LEAD	15u	// RF_TX_INTER 1.5 ms ahead of the slot: the retune, the lock, T5 (0.5 ms) and the ramp
#define SFR_SETTLE_US	600u	// the synthesizer's lock from the receive LO to the carrier (no lock detect, 5.3)
static bool dmr_lead;		// the lead is set for this transmission
static uint32_t dmr_rx_bursts;	// receive set-ups between bursts
#endif

// TIM5: one-shot at 1 MHz for the burst's key-up (MD9600_BURST_KEY_DELAY_US, then md9600_rf_pa_step's waits), at the
// modem interrupts' priority so neither preempts the other. Nothing waits in an interrupt: the timeslot interrupt
// falls inside the key-up
#define KEY_IRQ_PRIORITY	5
static volatile bool key_pending;
static uint32_t key_step;
static bool key_timer_ready;

static void key_timer_stop(void)
{
	TIM5->CR1 = 0;
	TIM5->SR = 0;
	key_pending = false;
}

static void key_timer_start(uint32_t us)
{
	if (!key_timer_ready) {
		__HAL_RCC_TIM5_CLK_ENABLE();
		uint32_t pclk = HAL_RCC_GetPCLK1Freq();
		uint32_t clk = ((RCC->CFGR & RCC_CFGR_PPRE1) == RCC_CFGR_PPRE1_DIV1) ? pclk : 2u * pclk;	// APB1 timer clock
		TIM5->CR1 = 0;
		TIM5->PSC = clk / 1000000u - 1u;
		TIM5->EGR = TIM_EGR_UG;		// load the prescaler
		TIM5->SR = 0;
		TIM5->DIER = TIM_DIER_UIE;
		HAL_NVIC_SetPriority(TIM5_IRQn, KEY_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(TIM5_IRQn);
		key_timer_ready = true;
	}
	TIM5->CR1 = 0;
	TIM5->ARR = (us > 1u ? us : 2u) - 1u;
	TIM5->CNT = 0;
	TIM5->SR = 0;
	TIM5->CR1 = TIM_CR1_OPM | TIM_CR1_CEN;
}

void TIM5_IRQHandler(void)
{
	TIM5->SR = 0;
	if (!key_pending) {
		return;
	}
	uint32_t us = 0;
	if (md9600_rf_pa_step(&rf, key_step++, &us) != MD9600_RF_OK) {
		dmr_skipped++;
		key_pending = false;
	} else if (us != 0) {
		key_timer_start(us);
	} else {
		key_pending = false;
	}
}

void md9600_rf_dmr_prepare(void)
{
	if (!dmr_armed || dmr_tuned) {
		return;
	}
	if (md9600_rf_tx_tune(&rf, dmr_hz, dmr_power, false) != MD9600_RF_OK) {
		dmr_skipped++;
		return;
	}
	dmr_tuned = true;
}

void md9600_rf_dmr_wait(void)
{
	if (dmr_tuned) {
		hal_delay_us(md9600_rf_tx_settle_us(&rf));
	}
}

bool md9600_rf_dmr_key(bool tx)
{
	if (!tx) {
		key_timer_stop();
		(void)md9600_rf_pa_burst(&rf, false);
#if CONFIG_SFR
		if (dmr_tuned && stm32_hrc_dmr_tx_listening()) {
			if (!dmr_lead) {
				md9600_c6000_tx_lead(SFR_TX_LEAD);
				dmr_lead = true;
			}
			if (md9600_rf_burst_rx(&rf) == MD9600_RF_OK) {
				dmr_tuned = false;
				dmr_rx_bursts++;
			}
		}
#endif
		if (dmr_tuned && stm32_hrc_dmr_tx_last_out() && md9600_rf_burst_rx(&rf) == MD9600_RF_OK) {
			// The last burst: the receiver back now, a slot before the engine's TX done gets to the task. An answer
			// one frame later (a confirmed data link ACK, 30 ms after this burst) found the receiver set up only
			// as it came in, and was taken on about one try in three
			dmr_tuned = false;
			dmr_rx_early = true;
		}
		return true;
	}
	if (!dmr_armed) {
		dmr_unarmed++;
		return false;
	}
	if (!dmr_tuned) {
		md9600_rf_dmr_prepare();	// unlocked for this burst: better than none (the glue normally did it)
		if (!dmr_tuned) {
			return false;
		}
#if CONFIG_SFR
		if (dmr_lead) {
			// Back from receive: the lock, inside the lead, then the PA at once (the lead was swept with it)
			hal_delay_us(SFR_SETTLE_US);
			if (md9600_rf_pa_burst(&rf, true) != MD9600_RF_OK) {
				dmr_skipped++;
				return false;
			}
			return true;
		}
#endif
	}
	key_step = 0;
	key_pending = true;
	uint32_t until;

	if (stm32_hrc_dmr_bs_boundary_us(&until)) {
		// The split duplex BS outbound keys once, and its slot starts with the CACH, not a guard. Its RF TX interrupt
		// comes once, anywhere in the slot: the key-up is timed off the slot clock. Full power at the boundary
		// clipped the first CACH; during the slot's sync, the receiver took that burst with noise for its CACH
		key_timer_start(until > MD9600_BS_KEY_LEAD_US ? until - MD9600_BS_KEY_LEAD_US
							      : until + MD9600_SLOT_US - MD9600_BS_KEY_LEAD_US);
	} else {
		key_timer_start(MD9600_BURST_KEY_DELAY_US);
	}
	return true;
}

// A transmission is over (the slot engine's TX done, task level under the modem lock): the receiver back on the RX
// frequency. On a split channel the radio hears the repeater answer its wake-up: left on the carrier, every key-up
// while the repeater was down ended in no_repeater. On simplex too: the receive LO is an IF away from the carrier, and
// a confirmed data response comes in the next frame, 30 ms after our last burst, before the radio task's tx_off() and
// rx() get here (a radio that returns that late misses nearly every one). Without the waits, as between an SFR's bursts
// (hardware-md9600.md 19.8): the listen that follows is far longer than the lock. Not per burst outside an SFR.
// True when the receiver is back on the RX frequency (the glue's tx_done)
bool md9600_rf_dmr_done(void)
{
	if (dmr_rx_early) {
		dmr_rx_early = false;	// the last burst's PA drop put it back (md9600_rf_dmr_key)
		if (dmr_armed && !dmr_tuned && rf.state == MD9600_RF_RX) {
			dmr_rx_back++;
			return true;
		}
	}
	if (!dmr_armed || !dmr_tuned || !rf.rx_valid) {
		return false;
	}
	key_timer_stop();
	(void)md9600_rf_pa_burst(&rf, false);
	if (md9600_rf_burst_rx(&rf) != MD9600_RF_OK) {
		return false;
	}
	dmr_tuned = false;
	dmr_rx_back++;
	return true;
}

void md9600_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx)
{
	*skipped = dmr_skipped;
	*unarmed = dmr_unarmed;
	*rx = dmr_rx_back;
#if CONFIG_SFR
	*rx += dmr_rx_bursts;
#endif
}

static void dmr_disarm(void)
{
	dmr_armed = false;
	if (stm32_hrc_dmr_tx_active()) {
		stm32_hrc_dmr_tx_drop();
	}
	key_timer_stop();
	(void)md9600_rf_pa_burst(&rf, false);
	dmr_tuned = false;
	dmr_rx_early = false;
#if CONFIG_SFR
	if (dmr_lead) {
		md9600_c6000_tx_lead(0);
		dmr_lead = false;
	}
#endif
}
#endif

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

#if CONFIG_SUBTONE
static subtone_t rx_tone_now;	// the RX sub-audio the detector is set for
#endif

static md9600_noise_win_t noise_win;	// the squelch's noise window (rfseq_md9600.h)

static bool rx(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone)
{
#if CONFIG_SUBTONE
	// NULL (back from transmit, wake-up) keeps the last tone; a new tone or channel restarts the detector
	bool same = rf.rx_valid && hz == rf.rx_hz && (bw == RADIO_BW_25K) == rf.rx_wide;
	if (rx_tone != NULL && (!same || rx_tone->type != rx_tone_now.type || rx_tone->value != rx_tone_now.value)) {
		rx_tone_now = *rx_tone;
		md9600_subtone_rx_set(&rx_tone_now);
	} else if (!same) {
		md9600_subtone_rx_set(&rx_tone_now);
	}
#else
	(void)rx_tone;
#endif
	md9600_noise_clear(&noise_win);
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
	if (dmr_mode) {
#if CONFIG_MD9600_DMR_TX
		// Arm only: the receiver stays until the glue's tx_prepare (md9600_rf_dmr_prepare)
		if (md9600_rf_tx_check(&rf, hz, power) != MD9600_RF_OK) {
			return false;
		}
		dmr_hz = hz;
		dmr_power = power;
		dmr_tuned = false;
		dmr_rx_early = false;
		__DMB();
		dmr_armed = true;
		stm32_hrc_dmr_tx_prepare();	// a transmission the stack has already begun gets its first burst too
		return true;
#else
		return false;
#endif
	}
	if (md9600_rf_tx_check(&rf, hz, power) != MD9600_RF_OK) {
		return md9600_rf_tx(&rf, hz, bw == RADIO_BW_25K, power) == MD9600_RF_OK;	// records the refusal
	}
	md9600_c6000_fm_tx(hz, bw == RADIO_BW_25K);	// the modulator first: the PA keys at the end of rf_tx
	if (md9600_rf_tx(&rf, hz, bw == RADIO_BW_25K, power) != MD9600_RF_OK) {
		md9600_c6000_fm_tx_end();
		return false;
	}
#if CONFIG_SUBTONE && CONFIG_MD9600_TX
	if (tx_tone != NULL) {
		uint8_t band = MD9600_UHF;
		(void)md9600_band_of(hz, &band);
		md9600_subtone_tx_start(tx_tone, bw == RADIO_BW_25K, band == MD9600_VHF);
	}
#else
	(void)tx_tone;
#endif
	return true;
}

#if CONFIG_SUBTONE && CONFIG_MD9600_TX
// The DCS turn-off tail from the PWM generator; the core holds the carrier for it
static bool tx_tail(bool on)
{
	if (dmr_mode || rf.state != MD9600_RF_TX || !md9600_subtone_tx_tail(on)) {
		return false;
	}
	md9600_c6000_mic(!on);	// PTT is up: the microphone off for the tail
	return true;
}
#endif

#if CONFIG_MD9600_TX && CONFIG_FM_TX_TONES
// The 1750 Hz burst, the roger beep and Morse: one tone from the HR_C6000's generator, the microphone off
static bool tx_tone(uint16_t hz)
{
	return !dmr_mode && rf.state == MD9600_RF_TX && md9600_c6000_tx_pair(hz, hz);
}

static bool tx_tone_key(bool down)
{
	return !dmr_mode && rf.state == MD9600_RF_TX && md9600_c6000_tx_key(down);
}
#endif

#if CONFIG_MD9600_TX && CONFIG_DTMF
static bool tx_dtmf(char digit)
{
	uint16_t low = 0, high = 0;
	if (dmr_mode || rf.state != MD9600_RF_TX || (digit != 0 && !dtmf_pair(digit, &low, &high))) {
		return false;
	}
	return md9600_c6000_tx_pair(low, high);
}
#endif

// keep (the end of a DMR transmission, md9600_dmr_tx_end): the TX done already put the receiver back on the RX
// frequency; only the VHF front-end tune, which shares the DAC with the PA drive, is set again
static void tx_end(bool keep)
{
#if CONFIG_SUBTONE && CONFIG_MD9600_TX
	md9600_subtone_tx_stop();
#endif
#if CONFIG_MD9600_DMR_TX
	if (dmr_mode) {
		dmr_disarm();	// after the stack's TX done this only disarms; before it, the transmission is cut
	}
#endif
	if (!keep || md9600_rf_rx_resume(&rf) != MD9600_RF_OK) {
		md9600_rf_tx_off(&rf);
	}
	md9600_c6000_fm_tx_end();
	drive_override = 0;	// one transmission only
}

static void tx_off(void)
{
	tx_end(false);
}

#if CONFIG_MD9600_DMR_TX
// stm32_hrc_dmr_board_t.tx_end: md9600_rf_dmr_done left the receiver on the channel
void md9600_dmr_tx_end(void)
{
	tx_end(true);
}
#endif

#if CONFIG_MD9600_TX
static uint16_t tx_drive_level(uint32_t hz, uint8_t level)
{
	uint16_t d;
	return md9600_cal_apc(hz, level, &d) ? d : 0u;
}

static bool tx_drive(uint16_t drive)
{
	if (drive > MD9600_APC_MAX) {
		return false;
	}
	drive_override = drive;
	return true;
}
#endif

static void rx_sleep(bool sleep)
{
	if (sleep) {
		md9600_rf_sleep(&rf);
	} else if (rf.rx_valid) {
		md9600_noise_clear(&noise_win);
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
	bool wide = rf.rx_wide && !rf.dmr;	// the same test as the IF filter's: DMR is always narrow
	st->noise = md9600_rf_squelch_noise(&noise_win, rf.state == MD9600_RF_RX, noise, st->rssi_dbm, hal_millis(),
					    wide ? md9600_rf_sql_wide(sql_l1) : sql_l1,
					    wide ? md9600_rf_sql_wide(sql_l9) : sql_l9);
	st->carrier = rf.state == MD9600_RF_RX && st->rssi_dbm >= CARRIER_DBM;
#if MD9600_TX_SIG
	if (rf.state == MD9600_RF_TX) {
		md9600_c6000_tx_service(hal_millis());
	}
#endif
#if CONFIG_SUBTONE
	st->tone_ok = md9600_subtone_rx_match();	// true for a channel without an RX tone
#else
	st->tone_ok = true;
#endif
}

// ADC3 IN2 (PA2, the sub-audio input), one software-started conversion. The common prescaler is the ADC1 scan's.
// While the discriminator capture has ADC3 (disc_md9600.c): the last reading
uint16_t md9600_adc3_qt(void)
{
	static uint16_t last;
#if MD9600_DISC
	if (md9600_disc_running()) {
		return last;
	}
#endif
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
	last = (uint16_t)(ADC3->DR & 0xFFFu);
	return last;
}

#if CONFIG_REF_TRIM
// Taken at the next tune (rfseq_md9600.h), so the RF buses stay the radio task's: true asks the core for one
static bool ref_trim(int16_t ppb)
{
	rf.trim_ppb = ppb;
	return true;
}
#endif

const hal_radio_t md9600_radio = {
	.init = init,
	.set_mode = set_mode,
	.rx = rx,
	.tx = tx,
	.tx_off = tx_off,
#if CONFIG_MD9600_DMR_TX
	.dmr_tx_end = stm32_hrc_dmr_tx_end,
#endif
#if CONFIG_SUBTONE && CONFIG_MD9600_TX
	.tx_tail = tx_tail,
#endif
#if CONFIG_MD9600_TX && CONFIG_FM_TX_TONES
	.tx_tone = tx_tone,
	.tx_tone_key = tx_tone_key,
#endif
#if CONFIG_MD9600_TX && CONFIG_DTMF
	.tx_dtmf = tx_dtmf,
#endif
	.status = status,
	.audio = audio,
	.rx_sleep = rx_sleep,
#if CONFIG_REF_TRIM
	.ref_trim = ref_trim,
#endif
#if CONFIG_MD9600_TX
	.tx_drive_level = tx_drive_level,
	.tx_drive = tx_drive,
#endif
};
