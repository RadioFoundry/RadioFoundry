// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 RF sequencing (rfseq_md9600.h; docs/spec/hardware-md9600.md 5.5, 19.8)
#include <stddef.h>
#include "rfseq_md9600.h"
#include "config.h"

// Per band: index md9600_band_t (VHF 0, UHF 1)
static const uint8_t ln_t5[2] = { MD9600_LN_T5_V, MD9600_LN_T5_U };
#if CONFIG_MD9600_TX
static const uint8_t ln_c5[2] = { MD9600_LN_C5_V, MD9600_LN_C5_U };
static const uint8_t ln_vco[2] = { MD9600_LN_VCO_V, MD9600_LN_VCO_U };
#endif

static void set(const md9600_rf_ops_t *o, uint8_t line, bool level)
{
	o->line(o->ctx, line, level);
}

static void wait_us(const md9600_rf_ops_t *o, uint32_t us)
{
	if (us != 0) {
		o->delay_us(o->ctx, us);
	}
}

static md9600_rf_err_t fail(md9600_rf_t *rf, md9600_rf_err_t e)
{
	rf->last_err = (uint8_t)e;
	if (e == MD9600_RF_E_BUS || e == MD9600_RF_E_LOCK) {
		rf->errors++;
	} else {
		rf->refused++;
	}
	return e;
}

// --- Lines ---

static void pa_lines_off(const md9600_rf_ops_t *o)
{
	set(o, MD9600_LN_APC, false);
	set(o, MD9600_LN_T5_U, false);
	set(o, MD9600_LN_T5_V, false);
}

// Both receivers and both C5 supplies are powered together; the RX VCOs selected
static void receivers_on(const md9600_rf_ops_t *o)
{
	pa_lines_off(o);
	set(o, MD9600_LN_POWER, true);
	set(o, MD9600_LN_R5_U, true);
	set(o, MD9600_LN_R5_V, true);
	set(o, MD9600_LN_C5_U, true);
	set(o, MD9600_LN_C5_V, true);
	set(o, MD9600_LN_VCO_U, true);
	set(o, MD9600_LN_VCO_V, true);
}

static void receivers_off(const md9600_rf_ops_t *o)
{
	pa_lines_off(o);
	set(o, MD9600_LN_FM_SW, false);
	set(o, MD9600_LN_R5_U, false);
	set(o, MD9600_LN_R5_V, false);
	set(o, MD9600_LN_C5_U, false);
	set(o, MD9600_LN_C5_V, false);
	set(o, MD9600_LN_POWER, false);
}

// The APC reference from one value to another in steps (the PA's power ramp)
static void apc_ramp(const md9600_rf_ops_t *o, uint16_t from, uint16_t to)
{
	for (uint32_t i = 1; i <= MD9600_APC_RAMP_STEPS; i++) {
		int32_t v = (int32_t)from + ((int32_t)to - (int32_t)from) * (int32_t)i / (int32_t)MD9600_APC_RAMP_STEPS;
		o->dac(o->ctx, MD9600_DAC_APC_TUNE_V, (uint16_t)v);
		if (i < MD9600_APC_RAMP_STEPS) {
			wait_us(o, MD9600_APC_RAMP_STEP_US);
		}
	}
}

void md9600_rf_pa_safe(md9600_rf_t *rf)
{
	const md9600_rf_ops_t *o = rf->ops;
	pa_lines_off(o);
	o->dac(o->ctx, MD9600_DAC_APC_TUNE_V, 0);
	rf->pa_on = false;
}

// --- Synthesizer and IF ---

static uint32_t settle_us(const md9600_rev_desc_t *d)
{
	switch (d->pll) {
	case MD9600_PLL_SKY72310: return MD9600_SETTLE_SKY72310_US;
	case MD9600_PLL_PLL1590: return MD9600_SETTLE_PLL1590_US;
	default: return MD9600_SETTLE_LMX2571_US;
	}
}

// The words, then the settle time or the lock indication (bounded)
static md9600_rf_err_t pll_program(const md9600_rf_ops_t *o, const md9600_rev_desc_t *d, const md9600_pll_plan_t *p)
{
	for (uint8_t i = 0; i < p->count; i++) {
		if (!o->pll(o->ctx, p->band, p->words[i], p->bits)) {
			return MD9600_RF_E_BUS;
		}
	}
	if (o->locked == NULL) {
		wait_us(o, settle_us(d));
		return MD9600_RF_OK;
	}
	for (uint32_t t = 0; t < MD9600_LOCK_TIMEOUT_US; t += MD9600_LOCK_POLL_US) {
		if (o->locked(o->ctx, p->band)) {
			return MD9600_RF_OK;
		}
		wait_us(o, MD9600_LOCK_POLL_US);
	}
	return o->locked(o->ctx, p->band) ? MD9600_RF_OK : MD9600_RF_E_LOCK;
}

// The receiving band's IF: the AK2365A set-up (V1-V4) or the V5 filter and route lines
static md9600_rf_err_t if_setup(const md9600_rf_ops_t *o, const md9600_rev_desc_t *d, uint8_t band, bool wide)
{
	if (d->if_chip) {
		uint16_t w[MD9600_IF_WORDS];
		uint8_t n = md9600_if_words(wide, w);
		for (uint8_t i = 0; i < n; i++) {
			if (!o->ifw(o->ctx, band, w[i])) {
				return MD9600_RF_E_BUS;
			}
		}
		wait_us(o, MD9600_IF_CAL_US);
		return MD9600_RF_OK;
	}
	uint8_t lines = md9600_if_lines_v5(band, wide);
	set(o, MD9600_LN_V5_BW_V, (lines & 1u) != 0);
	set(o, MD9600_LN_V5_BW_U, (lines & 2u) != 0);
	set(o, MD9600_LN_V5_ROUTE_V, (lines & 4u) != 0);
	set(o, MD9600_LN_V5_ROUTE_U, (lines & 8u) != 0);
	return MD9600_RF_OK;
}

// --- Set-up ---

void md9600_rf_init(md9600_rf_t *rf, const md9600_rf_ops_t *ops, md9600_rev_t rev, bool confirmed)
{
	*rf = (md9600_rf_t){ .ops = ops, .rev = MD9600_REV_UNKNOWN };
	const md9600_rev_desc_t *d = md9600_rev_desc(rev);
	if (d != NULL && md9600_rev_built(rev)) {
		rf->rev = rev;
		rf->desc = d;
		rf->confirmed = confirmed;
	}
	md9600_rf_pa_safe(rf);
	receivers_off(ops);
	rf->state = MD9600_RF_OFF;
}

md9600_rf_err_t md9600_rf_power_up(md9600_rf_t *rf)
{
	const md9600_rf_ops_t *o = rf->ops;
	if (rf->desc == NULL) {
		return fail(rf, MD9600_RF_E_REV);
	}
	receivers_on(o);
	if (rf->desc->if_chip) {
		// The AK2365A needs a hardware reset after its supply comes up
		set(o, MD9600_LN_IF_RST, false);
		wait_us(o, MD9600_IF_RESET_US);
		set(o, MD9600_LN_IF_RST, true);
	}
	if (rf->desc->vhf_tune_at_power_on) {
		md9600_pll_plan_t p;
		if (!md9600_pll_plan(rf->rev, MD9600_V4_KICK_HZ, false, &p)) {
			return fail(rf, MD9600_RF_E_BAND);
		}
		md9600_rf_err_t e = pll_program(o, rf->desc, &p);
		if (e != MD9600_RF_OK) {
			return fail(rf, e);
		}
		rf->lo_hz = p.lo_hz;
		rf->band = p.band;
	}
	rf->state = MD9600_RF_RX;
	return MD9600_RF_OK;
}

void md9600_rf_set_dmr(md9600_rf_t *rf, bool dmr)
{
	rf->dmr = dmr;
}

void md9600_rf_sleep(md9600_rf_t *rf)
{
	md9600_rf_pa_safe(rf);
	receivers_off(rf->ops);
	rf->state = MD9600_RF_OFF;
}

// --- Receive ---

md9600_rf_err_t md9600_rf_rx(md9600_rf_t *rf, uint32_t hz, bool wide)
{
	const md9600_rf_ops_t *o = rf->ops;
	if (rf->state == MD9600_RF_TX) {
		(void)md9600_rf_pa(rf, false);	// the PA ramped down before anything else moves
		rf->state = MD9600_RF_OFF;
	}
	md9600_rf_pa_safe(rf);
	if (rf->desc == NULL) {
		return fail(rf, MD9600_RF_E_REV);
	}
	md9600_pll_plan_t p;
	if (!md9600_pll_plan(rf->rev, hz, false, &p)) {
		return fail(rf, MD9600_RF_E_BAND);
	}
	bool w = wide && !rf->dmr;	// DMR is a 12.5 kHz channel
	rf->rx_valid = true;
	rf->rx_hz = hz;
	rf->rx_wide = wide;

	// Supplies and the RX VCOs before the words, so the loop locks onto the VCO it will use
	receivers_on(o);
	md9600_rf_err_t e = pll_program(o, rf->desc, &p);
	if (e != MD9600_RF_OK) {
		md9600_rf_pa_safe(rf);
		return fail(rf, e);
	}
	o->dac(o->ctx, p.band == MD9600_VHF ? MD9600_DAC_APC_TUNE_V : MD9600_DAC_TUNE_U, o->rx_tune(o->ctx, hz));
	e = if_setup(o, rf->desc, p.band, w);
	if (e != MD9600_RF_OK) {
		return fail(rf, e);
	}
	set(o, MD9600_LN_AF_VHF, p.band == MD9600_VHF);
	set(o, MD9600_LN_FM_SW, !rf->dmr);	// FM audio comes from the IF chip's discriminator; DMR from the HR_C6000
	wait_us(o, MD9600_RX_SETTLE_US);
	rf->state = MD9600_RF_RX;
	rf->lo_hz = p.lo_hz;
	rf->band = p.band;
	rf->retunes++;
	return MD9600_RF_OK;
}

// --- Transmit ---

bool md9600_rf_tx_arm(md9600_rf_t *rf, bool on)
{
	if (!on) {
		// Lines and the DAC only (no bus), so any task may disarm: a running transmission loses its PA at once and
		// the radio task's tx_off retunes later
		rf->tx_armed = false;
		md9600_rf_pa_safe(rf);
		return true;
	}
	if (!CONFIG_MD9600_TX || rf->desc == NULL || !rf->confirmed) {
		return false;
	}
	rf->tx_armed = true;
	return true;
}

md9600_rf_err_t md9600_rf_tx_check(const md9600_rf_t *rf, uint32_t hz, uint8_t power)
{
	if (!CONFIG_MD9600_TX) {
		return MD9600_RF_E_TX_BUILD;
	}
	if (!rf->tx_armed) {
		return MD9600_RF_E_TX_ARM;
	}
	if (rf->desc == NULL) {
		return MD9600_RF_E_REV;
	}
	if (!rf->confirmed) {
		return MD9600_RF_E_TX_REV;
	}
	uint8_t band;
	if (!md9600_band_of(hz, &band)) {
		return MD9600_RF_E_BAND;
	}
	uint16_t apc;
	const md9600_rf_ops_t *o = rf->ops;
	if (o->apc == NULL || !o->apc(o->ctx, hz, power, &apc) || apc == 0) {
		return MD9600_RF_E_TX_CAL;
	}
	return MD9600_RF_OK;
}

md9600_rf_err_t md9600_rf_tx(md9600_rf_t *rf, uint32_t hz, bool wide, uint8_t power)
{
	(void)wide;	// FM deviation is the HR_C6000's; the IF filter is a receive setting
	md9600_rf_pa_safe(rf);
	md9600_rf_err_t e = md9600_rf_tx_check(rf, hz, power);
	if (e != MD9600_RF_OK) {
		return fail(rf, e);
	}
#if CONFIG_MD9600_TX
	const md9600_rf_ops_t *o = rf->ops;
	md9600_pll_plan_t p;
	uint16_t apc = 0;
	if (!md9600_pll_plan(rf->rev, hz, true, &p) || !o->apc(o->ctx, hz, power, &apc)) {
		return fail(rf, MD9600_RF_E_BAND);
	}
	uint8_t b = p.band, other = (uint8_t)(1u - b);
	// Receivers off, the band's C5 on and (V4, V5) the other band's off, MOD2 to the band, its TX VCO
	set(o, MD9600_LN_R5_U, false);
	set(o, MD9600_LN_R5_V, false);
	set(o, MD9600_LN_FM_SW, false);
	set(o, ln_c5[b], true);
	set(o, ln_c5[other], !rf->desc->tx_other_c5_off);
	set(o, MD9600_LN_MOD_VHF, b == MD9600_VHF);
	set(o, ln_vco[b], false);
	set(o, ln_vco[other], true);
	e = pll_program(o, rf->desc, &p);
	if (e != MD9600_RF_OK) {
		// No carrier off frequency: back to receive, and report the transmit failure
		md9600_rf_pa_safe(rf);
		if (rf->rx_valid) {
			(void)md9600_rf_rx(rf, rf->rx_hz, rf->rx_wide);
		}
		return fail(rf, e);
	}
	rf->state = MD9600_RF_TX;
	rf->tx_hz = hz;
	rf->tx_band = b;
	rf->apc = apc;
	rf->lo_hz = p.lo_hz;
	rf->band = b;
	rf->retunes++;
	return md9600_rf_pa(rf, true);
#else
	return fail(rf, MD9600_RF_E_TX_BUILD);
#endif
}

md9600_rf_err_t md9600_rf_pa(md9600_rf_t *rf, bool on)
{
	const md9600_rf_ops_t *o = rf->ops;
	if (!on) {
		if (rf->pa_on) {
			apc_ramp(o, rf->apc, 0);
		}
		md9600_rf_pa_safe(rf);
		return MD9600_RF_OK;
	}
	if (rf->state != MD9600_RF_TX || !rf->tx_armed || !CONFIG_MD9600_TX) {
		md9600_rf_pa_safe(rf);
		return fail(rf, rf->state != MD9600_RF_TX ? MD9600_RF_E_STATE : MD9600_RF_E_TX_ARM);
	}
	if (rf->pa_on) {
		return MD9600_RF_OK;
	}
	// The reference at zero, the band's transmitter supply, the loop, then the reference up to the level
	o->dac(o->ctx, MD9600_DAC_APC_TUNE_V, 0);
	set(o, ln_t5[1u - rf->tx_band], false);
	set(o, ln_t5[rf->tx_band], true);
	wait_us(o, MD9600_TX_SUPPLY_US);
	set(o, MD9600_LN_APC, true);
	rf->pa_on = true;
	apc_ramp(o, 0, rf->apc);
	return MD9600_RF_OK;
}

void md9600_rf_tx_off(md9600_rf_t *rf)
{
	(void)md9600_rf_pa(rf, false);
	if (rf->state != MD9600_RF_TX) {
		return;
	}
	rf->state = MD9600_RF_OFF;
	if (rf->rx_valid) {
		(void)md9600_rf_rx(rf, rf->rx_hz, rf->rx_wide);
	} else {
		receivers_on(rf->ops);
		rf->state = MD9600_RF_RX;
	}
}

// --- Probe ---

md9600_rev_t md9600_rf_probe(const md9600_rf_ops_t *o, uint32_t hz, uint16_t threshold)
{
	md9600_rev_t cand[MD9600_REV_COUNT];
	uint16_t level[MD9600_REV_COUNT];
	uint8_t n = 0;
	o->dac(o->ctx, MD9600_DAC_APC_TUNE_V, 0);
	receivers_on(o);
	wait_us(o, MD9600_PROBE_SETTLE_US);
	uint16_t base = o->noise(o->ctx, MD9600_UHF);
	for (int r = MD9600_REV_COUNT - 1; r > MD9600_REV_UNKNOWN; r--) {
		const md9600_rev_desc_t *d = md9600_rev_desc((md9600_rev_t)r);
		md9600_pll_plan_t p;
		if (!md9600_rev_built((md9600_rev_t)r) || !md9600_pll_plan((md9600_rev_t)r, hz, false, &p)) {
			continue;
		}
		pa_lines_off(o);	// before every candidate: a wrong word may not leave anything keyed
		if (d->if_chip) {
			// Never drives PE15 on a board that may be V5 (its use there is unknown): no reset pulse
			(void)if_setup(o, d, MD9600_UHF, true);
		}
		(void)pll_program(o, d, &p);
		if (!d->if_chip) {
			(void)if_setup(o, d, MD9600_UHF, true);
		}
		wait_us(o, MD9600_PROBE_SETTLE_US);
		cand[n] = (md9600_rev_t)r;
		level[n++] = o->noise(o->ctx, MD9600_UHF);
	}
	receivers_off(o);
	return md9600_rev_probe_pick(base, level, cand, n, threshold);
}

// --- Readings ---

// hardware-md9600.md 5.7: dBm = floor + raw8 / 2 (V4A / V5: / 1.95), raw8 = the 12-bit average >> 4. A fit, unverified
#define RSSI_FLOOR_V		(-135)
#define RSSI_FLOOR_U		(-145)
#define RSSI_FLOOR_V5		(-155)

int16_t md9600_rf_rssi_dbm(md9600_rev_t rev, uint8_t band, uint16_t raw12)
{
	int32_t raw8 = raw12 >> 4;
	const md9600_rev_desc_t *d = md9600_rev_desc(rev);
	if (d != NULL && d->pll == MD9600_PLL_LMX2571) {
		return (int16_t)(RSSI_FLOOR_V5 + raw8 * 100 / 195);
	}
	return (int16_t)((band == MD9600_VHF ? RSSI_FLOOR_V : RSSI_FLOOR_U) + raw8 / 2);
}

uint8_t md9600_rf_noise(uint16_t raw12, uint16_t l1_open, uint16_t l9_open)
{
	if (l1_open == l9_open) {
		return (uint8_t)(raw12 >> 4);
	}
	// Linear through (l1_open, 64) and (l9_open, 40), whichever way the reading runs
	int32_t v = MD9600_SQL_CORE_L1 + ((int32_t)raw12 - (int32_t)l1_open) * (MD9600_SQL_CORE_L9 - MD9600_SQL_CORE_L1) /
		    ((int32_t)l9_open - (int32_t)l1_open);
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

const char *md9600_rf_err_name(uint8_t err)
{
	static const char *const names[MD9600_RF_E_COUNT] = {
		"ok", "no rev", "band", "bus", "lock", "tx not built", "tx not armed", "rev not set", "no PA cal", "state",
	};
	return err < MD9600_RF_E_COUNT ? names[err] : "?";
}
