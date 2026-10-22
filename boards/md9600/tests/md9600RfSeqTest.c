// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 RF sequencing (boards/md9600/rfseq_md9600.c) and calibration rows (cal_md9600.c) against a fake board: every
// line change, bus word, DAC write and delay is logged, and the tests check the order of the receive, transmit and
// return sequences per board revision, the transmit gates, the error paths and the receiver probe. Nothing here
// drives hardware: the "transmitter" is a log. Built with transmit in the build (md9600RfSeqTest) and without it
// (md9600RfSeqTestNoTx); in both, a line watcher fails the test if the PA loop or a transmitter supply comes on
// where it must not.
#include <stdio.h>
#include <string.h>
#include "rfseq_md9600.h"
#include "md9600.h"
#include "config.h"

#ifndef CONFIG_MD9600_TX
#define CONFIG_MD9600_TX	0
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- The fake board ---

enum { EV_LINE, EV_PLL, EV_IF, EV_DAC, EV_DELAY };

typedef struct {
	uint8_t kind, a;	// line / band / DAC channel
	uint32_t v;		// level / word / value / microseconds
} ev_t;

#define EV_MAX	600
static ev_t ev[EV_MAX];
static int nev;
static bool level[MD9600_LN_COUNT];
static bool pll_fail, if_fail;
static int pll_fail_after = -1;		// fail the bus at this pll word (counted from the log start)
static int npll;
static int lock_after = -1;		// with the lock op: true after this many polls, -1 never
static int lock_polls;
static bool use_lock;
static uint32_t delay_total;
static bool pa_allowed;			// the watcher: the PA loop or a T5 line may be on
static uint16_t dac_now[2];
static uint32_t last_uhf_word;		// the probe's noise model
static uint32_t probe_match_word;
static uint16_t apc_value = 2400;
static bool apc_ok = true;

static void log_ev(uint8_t kind, uint8_t a, uint32_t v)
{
	if (nev < EV_MAX) {
		ev[nev] = (ev_t){ kind, a, v };
	}
	nev++;
}

static void reset_log(void)
{
	nev = 0;
	npll = 0;
	delay_total = 0;
	lock_polls = 0;
}

static void f_line(void *ctx, uint8_t line, bool lv)
{
	(void)ctx;
	level[line] = lv;
	log_ev(EV_LINE, line, lv);
	if (lv && (line == MD9600_LN_APC || line == MD9600_LN_T5_U || line == MD9600_LN_T5_V)) {
		if (!pa_allowed) {
			printf("PA line %u on at event %d\n", line, nev);
		}
		CHECK(pa_allowed);
		CHECK(CONFIG_MD9600_TX);
	}
}

static bool f_pll(void *ctx, uint8_t band, uint32_t word, uint8_t bits)
{
	(void)ctx;
	(void)bits;
	log_ev(EV_PLL, band, word);
	if (band == MD9600_UHF) {
		last_uhf_word = word;
	}
	return !pll_fail && npll++ != pll_fail_after;
}

static bool f_if(void *ctx, uint8_t band, uint16_t word)
{
	(void)ctx;
	log_ev(EV_IF, band, word);
	return !if_fail;
}

static void f_dac(void *ctx, uint8_t ch, uint16_t v)
{
	(void)ctx;
	dac_now[ch] = v;
	log_ev(EV_DAC, ch, v);
}

static void f_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	delay_total += us;
	log_ev(EV_DELAY, 0, us);
}

static bool f_locked(void *ctx, uint8_t band)
{
	(void)ctx;
	(void)band;
	return lock_after >= 0 && lock_polls++ >= lock_after;
}

static uint16_t f_rx_tune(void *ctx, uint32_t hz)
{
	(void)ctx;
	return (uint16_t)(1000u + hz / 1000000u);
}

static bool f_apc(void *ctx, uint32_t hz, uint8_t power, uint16_t *dac)
{
	(void)ctx;
	(void)hz;
	*dac = (uint16_t)(apc_value + power);
	return apc_ok;
}

static uint16_t f_noise(void *ctx, uint8_t band)
{
	(void)ctx;
	(void)band;
	return probe_match_word != 0 && last_uhf_word == probe_match_word ? 1200 : 2000;
}

static md9600_rf_ops_t ops = {
	.line = f_line, .pll = f_pll, .ifw = f_if, .dac = f_dac, .delay_us = f_delay, .locked = NULL,
	.rx_tune = f_rx_tune, .apc = f_apc, .noise = f_noise,
};

// --- Log queries ---

// First event index at or after from matching kind / a / v (v_any: any value), -1 when none
static int find(int from, uint8_t kind, int a, long v)
{
	for (int i = from < 0 ? 0 : from; i < nev && i < EV_MAX; i++) {
		if (ev[i].kind == kind && (a < 0 || ev[i].a == a) && (v < 0 || ev[i].v == (uint32_t)v)) {
			return i;
		}
	}
	return -1;
}

static int last(uint8_t kind, int a, long v)
{
	int r = -1;
	for (int i = 0; i < nev && i < EV_MAX; i++) {
		if (ev[i].kind == kind && (a < 0 || ev[i].a == a) && (v < 0 || ev[i].v == (uint32_t)v)) {
			r = i;
		}
	}
	return r;
}

static int count(uint8_t kind, int a)
{
	int n = 0;
	for (int i = 0; i < nev && i < EV_MAX; i++) {
		n += ev[i].kind == kind && (a < 0 || ev[i].a == a);
	}
	return n;
}

static bool pa_off_now(void)
{
	return !level[MD9600_LN_APC] && !level[MD9600_LN_T5_U] && !level[MD9600_LN_T5_V];
}

// The plan's words appear in order on the band's chip select from index from
static bool words_follow(int from, const md9600_pll_plan_t *p)
{
	int at = from;
	for (uint8_t i = 0; i < p->count; i++) {
		at = find(at, EV_PLL, p->band, p->words[i]);
		if (at < 0) {
			return false;
		}
		at++;
	}
	return true;
}

static md9600_rf_t rf;

static void fresh(md9600_rev_t rev, bool confirmed)
{
	memset(level, 0, sizeof(level));
	pll_fail = if_fail = false;
	pll_fail_after = -1;
	use_lock = false;
	ops.locked = NULL;
	apc_ok = true;
	pa_allowed = false;
	md9600_rf_init(&rf, &ops, rev, confirmed);
	reset_log();
}

// --- Tests ---

static void test_init(void)
{
	reset_log();
	md9600_rf_init(&rf, &ops, MD9600_REV_V5, false);
	// The PA loop and the transmitter supplies go off before anything else
	CHECK(ev[0].kind == EV_LINE && ev[0].a == MD9600_LN_APC && ev[0].v == 0);
	CHECK(find(0, EV_LINE, MD9600_LN_T5_U, 0) < find(0, EV_LINE, MD9600_LN_POWER, 0));
	CHECK(count(EV_PLL, -1) == 0 && count(EV_IF, -1) == 0);
	CHECK(rf.state == MD9600_RF_OFF && !rf.tx_armed);

	// An unknown or unbuilt revision: everything off and every request refused
	fresh(MD9600_REV_UNKNOWN, true);
	CHECK(rf.desc == NULL && !rf.confirmed);
	CHECK(md9600_rf_power_up(&rf) == MD9600_RF_E_REV);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_E_REV);
	CHECK(count(EV_PLL, -1) == 0 && !level[MD9600_LN_POWER]);
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 0) != MD9600_RF_OK && pa_off_now());
	CHECK(rf.refused == 3 && rf.last_err != MD9600_RF_OK);
	md9600_rf_init(&rf, &ops, MD9600_REV_COUNT, true);
	CHECK(rf.desc == NULL);
}

static void test_power_up(void)
{
	md9600_pll_plan_t p;
#if CONFIG_MD9600_V1V3
	fresh(MD9600_REV_V1V3, false);
	CHECK(md9600_rf_power_up(&rf) == MD9600_RF_OK);
	// Receivers on, then the AK2365A reset pulse; no synthesizer words
	int on = find(0, EV_LINE, MD9600_LN_R5_U, 1), rst = find(0, EV_LINE, MD9600_LN_IF_RST, 0);
	CHECK(on >= 0 && rst > on && find(rst, EV_DELAY, -1, MD9600_IF_RESET_US) == rst + 1);
	CHECK(find(rst, EV_LINE, MD9600_LN_IF_RST, 1) == rst + 2);
	CHECK(count(EV_PLL, -1) == 0);
#endif
#if CONFIG_MD9600_V4
	fresh(MD9600_REV_V4, false);
	CHECK(md9600_rf_power_up(&rf) == MD9600_RF_OK);
	// The VHF synthesizer set right after power-on (145 MHz receive)
	CHECK(md9600_pll_plan(MD9600_REV_V4, MD9600_V4_KICK_HZ, false, &p));
	CHECK(p.band == MD9600_VHF && words_follow(0, &p) && count(EV_PLL, -1) == p.count);
	CHECK(find(0, EV_LINE, MD9600_LN_IF_RST, 0) < find(0, EV_PLL, -1, -1));
	CHECK(rf.lo_hz == p.lo_hz);
	// A bus failure there is reported
	fresh(MD9600_REV_V4, false);
	pll_fail = true;
	CHECK(md9600_rf_power_up(&rf) == MD9600_RF_E_BUS && rf.errors == 1);
#endif
#if CONFIG_MD9600_V5
	fresh(MD9600_REV_V5, false);
	CHECK(md9600_rf_power_up(&rf) == MD9600_RF_OK);
	// V5: PE15's use is unknown, so it is never driven; no words at power-up
	CHECK(count(EV_LINE, MD9600_LN_IF_RST) == 0 && count(EV_PLL, -1) == 0);
	CHECK(level[MD9600_LN_R5_V] && level[MD9600_LN_C5_U] && level[MD9600_LN_POWER] && pa_off_now());
#endif
	(void)p;
}

static void check_rx_order(md9600_rev_t r, uint32_t hz, bool wide)
{
	fresh(r, false);
	md9600_pll_plan_t p;
	CHECK(md9600_pll_plan(r, hz, false, &p));
	CHECK(md9600_rf_rx(&rf, hz, wide) == MD9600_RF_OK);
	const md9600_rev_desc_t *d = md9600_rev_desc(r);
	int apc = find(0, EV_LINE, MD9600_LN_APC, 0);
	int pwr = find(0, EV_LINE, MD9600_LN_POWER, 1);
	int vco = find(0, EV_LINE, p.band == MD9600_VHF ? MD9600_LN_VCO_V : MD9600_LN_VCO_U, 1);
	int w0 = find(0, EV_PLL, p.band, -1);
	int wl = last(EV_PLL, p.band, -1);
	uint8_t tune_ch = p.band == MD9600_VHF ? MD9600_DAC_APC_TUNE_V : MD9600_DAC_TUNE_U;
	int tune = find(0, EV_DAC, tune_ch, f_rx_tune(NULL, hz));
	int af = find(0, EV_LINE, MD9600_LN_AF_VHF, p.band == MD9600_VHF);
	int fm = find(0, EV_LINE, MD9600_LN_FM_SW, 1);
	// PA off, supplies and RX VCO, the words, the settle, front-end tuning, IF, audio band, FM stages
	CHECK(apc == 0 && apc < pwr && pwr < vco && vco < w0);
	CHECK(words_follow(w0, &p) && count(EV_PLL, -1) == p.count);
	CHECK(find(wl, EV_DELAY, -1, -1) == wl + 1);
	CHECK(tune > wl && af > tune && fm > af);
	if (d->if_chip) {
		uint16_t w[MD9600_IF_WORDS];
		uint8_t n = md9600_if_words(wide, w);
		int i0 = find(0, EV_IF, p.band, w[0]);
		CHECK(i0 > tune && i0 < af && count(EV_IF, p.band) == n && count(EV_IF, 1 - p.band) == 0);
		CHECK(find(i0, EV_IF, p.band, w[1]) == i0 + 1);
		CHECK(count(EV_LINE, MD9600_LN_V5_BW_V) == 0);
	} else {
		uint8_t lines = md9600_if_lines_v5(p.band, wide);
		CHECK(count(EV_IF, -1) == 0);
		CHECK(level[MD9600_LN_V5_BW_V] == ((lines & 1u) != 0) && level[MD9600_LN_V5_BW_U] == ((lines & 2u) != 0));
		CHECK(level[MD9600_LN_V5_ROUTE_V] == (p.band == MD9600_VHF) && level[MD9600_LN_V5_ROUTE_U] == (p.band == MD9600_UHF));
	}
	CHECK(pa_off_now() && rf.state == MD9600_RF_RX && rf.lo_hz == p.lo_hz && rf.retunes == 1);
	CHECK(!level[MD9600_LN_T5_U] && level[MD9600_LN_R5_U] && level[MD9600_LN_R5_V]);
	// Every wait bounded and short: a retune is a few milliseconds
	CHECK(delay_total <= 20000u);
}

static void test_rx(void)
{
	for (int r = MD9600_REV_V1V3; r < MD9600_REV_COUNT; r++) {
		if (!md9600_rev_built((md9600_rev_t)r)) {
			continue;
		}
		check_rx_order((md9600_rev_t)r, 145500000u, true);
		check_rx_order((md9600_rev_t)r, 446006250u, false);
	}
	md9600_rev_t r = MD9600_REV_V5;
	// DMR: narrow IF and the FM audio stages off
	fresh(r, false);
	md9600_rf_set_dmr(&rf, true);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK);
	CHECK(!level[MD9600_LN_FM_SW] && !level[MD9600_LN_V5_BW_U]);
	md9600_rf_set_dmr(&rf, false);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK && level[MD9600_LN_FM_SW] && level[MD9600_LN_V5_BW_U]);

	// Out of band: refused before any word
	fresh(r, false);
	CHECK(md9600_rf_rx(&rf, 300000000u, true) == MD9600_RF_E_BAND);
	CHECK(count(EV_PLL, -1) == 0 && rf.refused == 1 && rf.last_err == MD9600_RF_E_BAND);
	// A bus failure: an error, the PA lines stay off
	fresh(r, false);
	pll_fail = true;
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_E_BUS && rf.errors == 1 && pa_off_now());
#if CONFIG_MD9600_V1V3 || CONFIG_MD9600_V4
	fresh(md9600_rev_built(MD9600_REV_V4) ? MD9600_REV_V4 : MD9600_REV_V1V3, false);
	if_fail = true;
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_E_BUS);
#endif
	// Sleep powers the receivers down
	fresh(r, false);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK);
	md9600_rf_sleep(&rf);
	CHECK(!level[MD9600_LN_POWER] && !level[MD9600_LN_R5_U] && rf.state == MD9600_RF_OFF && pa_off_now());
}

// A lock indication: polled up to the timeout, an error past it
static void test_lock(void)
{
	fresh(MD9600_REV_V5, false);
	ops.locked = f_locked;
	lock_after = 3;
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK);
	CHECK(lock_polls == 4);
	fresh(MD9600_REV_V5, false);
	ops.locked = f_locked;
	lock_after = -1;
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_E_LOCK);
	CHECK(rf.errors == 1 && pa_off_now());
	CHECK(delay_total >= MD9600_LOCK_TIMEOUT_US && delay_total <= MD9600_LOCK_TIMEOUT_US + 10000u);
	ops.locked = NULL;
}

static void test_tx_gates(void)
{
	fresh(MD9600_REV_V5, false);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK);
	reset_log();
#if !CONFIG_MD9600_TX
	// Not in this build: nothing arms it, every request is refused, no line moves toward transmit
	CHECK(!md9600_rf_tx_arm(&rf, true));
	md9600_rf_t c = rf;
	c.tx_armed = true;
	c.confirmed = true;
	CHECK(md9600_rf_tx_check(&c, 446000000u, 0) == MD9600_RF_E_TX_BUILD);
	CHECK(md9600_rf_tx(&c, 446000000u, true, 0) == MD9600_RF_E_TX_BUILD);
	CHECK(md9600_rf_pa(&c, true) != MD9600_RF_OK);
	CHECK(count(EV_PLL, -1) == 0 && pa_off_now());
#else
	// Unconfirmed revision: no arming, and the request names the arm first
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 0) == MD9600_RF_E_TX_ARM);
	CHECK(!md9600_rf_tx_arm(&rf, true));
	md9600_rf_t c = rf;
	c.tx_armed = true;
	CHECK(md9600_rf_tx_check(&c, 446000000u, 0) == MD9600_RF_E_TX_REV);
	// Confirmed: arms; out of band and missing calibration refuse
	fresh(MD9600_REV_V5, true);
	CHECK(md9600_rf_tx_check(&rf, 446000000u, 0) == MD9600_RF_E_TX_ARM);
	CHECK(md9600_rf_tx_arm(&rf, true));
	CHECK(md9600_rf_tx(&rf, 300000000u, true, 0) == MD9600_RF_E_BAND);
	apc_ok = false;
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 0) == MD9600_RF_E_TX_CAL);
	apc_ok = true;
	CHECK(md9600_rf_tx_check(&rf, 446000000u, 0) == MD9600_RF_OK);
	CHECK(count(EV_PLL, -1) == 0 && pa_off_now());
	// The PA alone outside transmit
	CHECK(md9600_rf_pa(&rf, true) == MD9600_RF_E_STATE && pa_off_now());
#endif
	CHECK(rf.state != MD9600_RF_TX);
}

#if CONFIG_MD9600_TX
static void check_tx(md9600_rev_t r, uint32_t rx_hz, uint32_t tx_hz, uint8_t power)
{
	fresh(r, true);
	CHECK(md9600_rf_rx(&rf, rx_hz, true) == MD9600_RF_OK);
	CHECK(md9600_rf_tx_arm(&rf, true));
	reset_log();
	pa_allowed = true;
	md9600_pll_plan_t p;
	CHECK(md9600_pll_plan(r, tx_hz, true, &p));
	uint8_t b = p.band, o = (uint8_t)(1u - b);
	const md9600_rev_desc_t *d = md9600_rev_desc(r);
	CHECK(md9600_rf_tx(&rf, tx_hz, true, power) == MD9600_RF_OK);
	int r5 = find(0, EV_LINE, MD9600_LN_R5_U, 0);
	int c5 = find(0, EV_LINE, b == MD9600_VHF ? MD9600_LN_C5_V : MD9600_LN_C5_U, 1);
	int c5o = find(0, EV_LINE, o == MD9600_VHF ? MD9600_LN_C5_V : MD9600_LN_C5_U, -1);
	int mod = find(0, EV_LINE, MD9600_LN_MOD_VHF, b == MD9600_VHF);
	int vco = find(0, EV_LINE, b == MD9600_VHF ? MD9600_LN_VCO_V : MD9600_LN_VCO_U, 0);
	int w0 = find(0, EV_PLL, b, -1), wl = last(EV_PLL, b, -1);
	int t5 = find(0, EV_LINE, b == MD9600_VHF ? MD9600_LN_T5_V : MD9600_LN_T5_U, 1);
	int apc = find(0, EV_LINE, MD9600_LN_APC, 1);
	// PA safe, receivers off, C5 per revision, MOD2 and the TX VCO, the words, then T5, the loop, the ramp
	CHECK(find(0, EV_LINE, MD9600_LN_APC, 0) == 0);
	CHECK(r5 > 0 && c5 > r5 && c5o > r5 && mod > c5 && vco > mod && w0 > vco && words_follow(w0, &p));
	CHECK(ev[c5o].v == (d->tx_other_c5_off ? 0u : 1u));
	CHECK(t5 > wl && apc > t5 && find(t5, EV_DELAY, -1, MD9600_TX_SUPPLY_US) > t5);
	// The reference: zero before T5, then rising in steps to the calibrated value
	int z = last(EV_DAC, MD9600_DAC_APC_TUNE_V, 0);
	CHECK(z >= 0 && z < t5);
	int steps = 0;
	uint32_t prev = 0;
	for (int i = apc; i < nev; i++) {
		if (ev[i].kind == EV_DAC && ev[i].a == MD9600_DAC_APC_TUNE_V) {
			CHECK(ev[i].v >= prev);
			prev = ev[i].v;
			steps++;
		}
	}
	CHECK(steps == (int)MD9600_APC_RAMP_STEPS && prev == (uint32_t)(apc_value + power));
	CHECK(level[b == MD9600_VHF ? MD9600_LN_T5_V : MD9600_LN_T5_U] && !level[o == MD9600_VHF ? MD9600_LN_T5_V : MD9600_LN_T5_U]);
	CHECK(level[MD9600_LN_APC] && rf.state == MD9600_RF_TX && rf.lo_hz == tx_hz);
	// No receiver word in transmit; the other band's synthesizer untouched
	CHECK(count(EV_PLL, o) == 0 && count(EV_IF, -1) == 0);

	// DMR bursts: the PA alone, no retune
	reset_log();
	CHECK(md9600_rf_pa(&rf, false) == MD9600_RF_OK && pa_off_now());
	CHECK(md9600_rf_pa(&rf, true) == MD9600_RF_OK && level[MD9600_LN_APC]);
	CHECK(count(EV_PLL, -1) == 0);

	// Back: ramp down, loop and supplies off, then the receive words for the stored frequency
	reset_log();
	md9600_rf_tx_off(&rf);
	md9600_pll_plan_t q;
	CHECK(md9600_pll_plan(r, rx_hz, false, &q));
	int off = find(0, EV_LINE, MD9600_LN_APC, 0);
	int t5off = find(0, EV_LINE, b == MD9600_VHF ? MD9600_LN_T5_V : MD9600_LN_T5_U, 0);
	int down = -1;	// the last reference write before the loop opens: the end of the ramp, at zero
	for (int i = 0; i < off; i++) {
		down = ev[i].kind == EV_DAC && ev[i].a == MD9600_DAC_APC_TUNE_V ? i : down;
	}
	CHECK(find(0, EV_DAC, MD9600_DAC_APC_TUNE_V, -1) == 0 && down > 0 && ev[down].v == 0 && off < t5off);
	CHECK(words_follow(t5off, &q) && find(t5off, EV_PLL, -1, -1) > t5off);
	CHECK(pa_off_now() && rf.state == MD9600_RF_RX && level[MD9600_LN_VCO_U] && level[MD9600_LN_VCO_V]);
	CHECK(level[MD9600_LN_C5_U] && level[MD9600_LN_C5_V] && level[MD9600_LN_R5_U]);
	pa_allowed = false;
}

static void test_tx(void)
{
	for (int r = MD9600_REV_V1V3; r < MD9600_REV_COUNT; r++) {
		if (!md9600_rev_built((md9600_rev_t)r)) {
			continue;
		}
		check_tx((md9600_rev_t)r, 446000000u, 446000000u, 0);
		check_tx((md9600_rev_t)r, 145500000u, 146520000u, 3);
	}

	// A bus failure in transmit: the PA never comes on and the receiver returns
	fresh(MD9600_REV_V5, true);
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK);
	CHECK(md9600_rf_tx_arm(&rf, true));
	reset_log();
	pll_fail_after = 2;
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 1) == MD9600_RF_E_BUS);
	CHECK(find(0, EV_LINE, MD9600_LN_APC, 1) < 0 && find(0, EV_LINE, MD9600_LN_T5_U, 1) < 0 && pa_off_now() && rf.state == MD9600_RF_RX && rf.errors == 1);
	CHECK(rf.last_err == MD9600_RF_E_BUS && level[MD9600_LN_VCO_U]);

	// Disarming during a transmission drops the PA at once and refuses it again
	fresh(MD9600_REV_V5, true);
	pa_allowed = true;
	CHECK(md9600_rf_rx(&rf, 446000000u, true) == MD9600_RF_OK && md9600_rf_tx_arm(&rf, true));
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 2) == MD9600_RF_OK && level[MD9600_LN_APC]);
	reset_log();
	CHECK(md9600_rf_tx_arm(&rf, false));
	CHECK(pa_off_now() && dac_now[MD9600_DAC_APC_TUNE_V] == 0 && count(EV_PLL, -1) == 0);
	CHECK(md9600_rf_pa(&rf, true) == MD9600_RF_E_TX_ARM && pa_off_now());
	md9600_rf_tx_off(&rf);
	CHECK(rf.state == MD9600_RF_RX);

	// A receive request in transmit ends the transmission first
	CHECK(md9600_rf_tx_arm(&rf, true));
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 2) == MD9600_RF_OK);
	reset_log();
	CHECK(md9600_rf_rx(&rf, 440000000u, true) == MD9600_RF_OK);
	CHECK(find(0, EV_LINE, MD9600_LN_APC, 0) < find(0, EV_PLL, -1, -1) && pa_off_now() && rf.rx_hz == 440000000u);
	// tx_off without a stored receive frequency still powers the receivers
	fresh(MD9600_REV_V5, true);
	pa_allowed = true;
	CHECK(md9600_rf_tx_arm(&rf, true));
	CHECK(md9600_rf_tx(&rf, 446000000u, true, 0) == MD9600_RF_OK);
	md9600_rf_tx_off(&rf);
	CHECK(rf.state == MD9600_RF_RX && level[MD9600_LN_R5_U] && pa_off_now());
	pa_allowed = false;
}
#endif

static void test_probe(void)
{
	md9600_pll_plan_t p;
	md9600_rev_t want = MD9600_REV_V5;
#if CONFIG_MD9600_V4
	want = MD9600_REV_V4;
#endif
	CHECK(md9600_pll_plan(want, MD9600_PROBE_HZ, false, &p));
	probe_match_word = p.words[p.count - 1];	// the last word of the matching revision moves the noise reading
	memset(level, 0, sizeof(level));
	last_uhf_word = 0;
	reset_log();
	CHECK(md9600_rf_probe(&ops, MD9600_PROBE_HZ, MD9600_PROBE_THRESHOLD) == want);
	// Receive side only: no transmitter line on, never PE15, the radio left off
	CHECK(find(0, EV_LINE, MD9600_LN_APC, 1) < 0 && find(0, EV_LINE, MD9600_LN_T5_U, 1) < 0);
	CHECK(count(EV_LINE, MD9600_LN_IF_RST) == 0 && !level[MD9600_LN_POWER]);
	CHECK(count(EV_PLL, MD9600_VHF) == 0);
	// No candidate moves it: no verdict
	probe_match_word = 0;
	CHECK(md9600_rf_probe(&ops, MD9600_PROBE_HZ, MD9600_PROBE_THRESHOLD) == MD9600_REV_UNKNOWN);
	CHECK(md9600_rf_probe(&ops, 300000000u, MD9600_PROBE_THRESHOLD) == MD9600_REV_UNKNOWN);
}

static void test_readings(void)
{
	CHECK(md9600_rf_rssi_dbm(MD9600_REV_V4, MD9600_VHF, 0) == -135);
	CHECK(md9600_rf_rssi_dbm(MD9600_REV_V1V3, MD9600_UHF, 0x400) == -145 + 32);
	CHECK(md9600_rf_rssi_dbm(MD9600_REV_V5, MD9600_UHF, 0x400) == -155 + 64 * 100 / 195);
	// Uncalibrated: raw / 16
	CHECK(md9600_rf_noise(0x800, 0, 0) == 0x80);
	// Calibrated, either direction: level 1's line maps to 64, level 9's to 40
	CHECK(md9600_rf_noise(2400, 2400, 1600) == 64 && md9600_rf_noise(1600, 2400, 1600) == 40);
	CHECK(md9600_rf_noise(1600, 1600, 2400) == 64 && md9600_rf_noise(2400, 1600, 2400) == 40);
	CHECK(md9600_rf_noise(0, 2400, 1600) == 0 && md9600_rf_noise(0, 1600, 2400) == 112);
	CHECK(md9600_rf_noise(4095, 2400, 2390) == 255);
	CHECK(strcmp(md9600_rf_err_name(MD9600_RF_E_TX_REV), "rev not set") == 0);
	CHECK(strcmp(md9600_rf_err_name(200), "?") == 0);
}

// --- The RF code's calibration rows (rfcal_md9600.c over cal_md9600.c) on a synthetic table ---

static uint8_t sec[3][256];
static bool sec_ok = true;

bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	if (!sec_ok || reg < 1 || reg > 3 || offset + len > 256) {
		return false;
	}
	memcpy(buf, sec[reg - 1] + offset, len);
	return true;
}

static void cal_put(uint32_t off, uint8_t v, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		sec[(off + i) / 256][(off + i) % 256] = (uint8_t)(v + i);
	}
}

static void test_cal(void)
{
	memset(sec, 0xFF, sizeof(sec));
	md9600_cal_load();
	CHECK(md9600_cal_rf_rows(MD9600_UHF) == 0 && md9600_cal_rf_rows(MD9600_VHF) == 0);
	CHECK(strcmp(md9600_cal_source(), "blank") == 0);
	CHECK(md9600_cal_rx_tune(446000000u) == 2048);
	uint16_t dac, l1, l9;
	CHECK(!md9600_cal_apc(446000000u, 0, &dac) && !md9600_cal_squelch(446000000u, &l1, &l9));

	// Front end, PA rising with the level, squelch rows
	cal_put(0x30, 100, 9);
	cal_put(0x39, 50, 5);
	const uint16_t pa_u[4] = { 0x20, 0x1A0, 0x190, 0x10 }, pa_v[4] = { 0x29, 0x1A9, 0x199, 0x19 };
	for (int l = 0; l < 4; l++) {
		cal_put(pa_u[l], (uint8_t)(40 + 30 * l), 9);
		cal_put(pa_v[l], (uint8_t)(20 + 30 * l), 5);
	}
	cal_put(0x40, 60, 9);
	cal_put(0x60, 140, 9);
	cal_put(0xE0, 70, 5);
	cal_put(0xEA, 150, 5);
	md9600_cal_load();
	CHECK(md9600_cal_rf_rows(MD9600_UHF) == (MD9600_CALOK_RX | MD9600_CALOK_PA | MD9600_CALOK_SQL));
	CHECK(md9600_cal_rf_rows(MD9600_VHF) == (MD9600_CALOK_RX | MD9600_CALOK_PA | MD9600_CALOK_SQL));
	CHECK(strcmp(md9600_cal_source(), "factory") == 0);
	CHECK(md9600_cal_rx_tune(400000000u) == 1600 && md9600_cal_rx_tune(405000000u) == 1616);
	CHECK(md9600_cal_rx_tune(136000000u) == 800);
	CHECK(md9600_cal_apc(400000000u, 0, &dac) && dac == 40 * 16);
	CHECK(md9600_cal_apc(410000000u, 3, &dac) && dac == (130 + 1) * 16);
	CHECK(md9600_cal_apc(136000000u, 1, &dac) && dac == 50 * 16);
	CHECK(!md9600_cal_apc(446000000u, 4, &dac));
	CHECK(md9600_cal_squelch(400000000u, &l1, &l9) && l1 == 140 * 16 && l9 == 60 * 16);

	// A PA row that doesn't rise with the level: transmit has no calibration
	sec[1][0xA0 + 4] = (uint8_t)(40 + 4);	// 10 W at 440 MHz down to the 5 W value
	md9600_cal_load();
	CHECK(!(md9600_cal_rf_rows(MD9600_UHF) & MD9600_CALOK_PA) && !md9600_cal_apc(446000000u, 0, &dac));
	CHECK((md9600_cal_rf_rows(MD9600_UHF) & MD9600_CALOK_RX) && (md9600_cal_rf_rows(MD9600_VHF) & MD9600_CALOK_PA));
	CHECK(md9600_cal_apc(136000000u, 0, &dac));	// the other band's rows still count
	// The flash unreadable
	sec_ok = false;
	md9600_cal_load();
	CHECK(md9600_cal_rf_rows(MD9600_UHF) == 0 && strcmp(md9600_cal_source(), "unreadable") == 0);
	CHECK(!md9600_cal_squelch(446000000u, &l1, &l9));
	sec_ok = true;
}

// --- The diagnostics page's revision lines (rev_md9600.c): every state fits the 128x64 panel's 25 characters ---

static void test_rev_text(void)
{
	char buf[64];
	size_t longest = 0;
	for (int r = MD9600_REV_UNKNOWN; r <= MD9600_REV_COUNT; r++) {
		for (uint8_t src = 0; src <= 4; src++) {
			for (int f = 0; f < 4; f++) {
				for (int16_t probe = -1; probe <= MD9600_REV_COUNT; probe++) {
					md9600_rev_state_t st = { (md9600_rev_t)r, src, (f & 1) != 0, (f & 2) != 0 };
					for (uint8_t line = 0; line < MD9600_REV_TEXT_LINES; line++) {
						CHECK(md9600_rev_text(line, st, probe, buf, sizeof buf));
						size_t n = strlen(buf);
						CHECK(n > 0 && n <= MD9600_REV_TEXT_MAX);
						longest = n > longest ? n : longest;
					}
				}
			}
		}
	}
	CHECK(longest == 21);	// the longest of all: "Set rev: Adv>Hardware"
	md9600_rev_state_t st = { MD9600_REV_V5, MD9600_REV_SRC_STORED, false, true };
	CHECK(md9600_rev_text(0, st, -1, buf, sizeof buf) && strcmp(buf, "rev V4A/V5 unbuilt") == 0);
	st = (md9600_rev_state_t){ MD9600_REV_V5, MD9600_REV_SRC_DEFAULT, true, false };
	CHECK(md9600_rev_text(0, st, -1, buf, sizeof buf) && strcmp(buf, "rev V4A/V5 default") == 0);
	CHECK(md9600_rev_text(1, st, -1, buf, sizeof buf) && strcmp(buf, "Set rev: Adv>Hardware") == 0);
	CHECK(md9600_rev_text(2, st, -1, buf, sizeof buf) && strcmp(buf, "probe off") == 0);
	CHECK(md9600_rev_text(2, st, 0, buf, sizeof buf) && strcmp(buf, "probe none") == 0);
	st.confirmed = true;
	CHECK(md9600_rev_text(1, st, -1, buf, sizeof buf) && strcmp(buf, "rev confirmed") == 0);
	// Past the last line; a short buffer is cut, not overrun
	CHECK(!md9600_rev_text(MD9600_REV_TEXT_LINES, st, -1, buf, sizeof buf));
	CHECK(!md9600_rev_text(0, st, -1, buf, 0));
	CHECK(md9600_rev_text(0, st, -1, buf, 5) && strcmp(buf, "rev ") == 0);
}

// --- The hardware options (hwopt_md9600.c) over a fake record and arm ---

static md9600_rev_t stored_rev = MD9600_REV_UNKNOWN;
static bool store_ok = true, arm_state, arm_ok = true;

md9600_rev_t md9600_flash_rev_load(void)
{
	return stored_rev;
}

bool md9600_flash_rev_store(md9600_rev_t r)
{
	if (store_ok && md9600_rev_desc(r) != NULL) {
		stored_rev = r;
		return true;
	}
	return false;
}

bool md9600_radio_tx_arm(bool on)
{
	if (on && !arm_ok) {
		return false;
	}
	arm_state = on;
	return true;
}

void md9600_radio_diag(md9600_radio_diag_t *d)
{
	memset(d, 0, sizeof(*d));
	d->tx_armed = arm_state;
}

static void test_hwopt(void)
{
	const hal_hwopt_t *o = &md9600_hwopts[0];
	CHECK(MD9600_HWOPT_COUNT == (CONFIG_MD9600_TX ? 2u : 1u));
	CHECK(strcmp(o->name, "Board revision") == 0 && o->count == 3 && o->restart);
	CHECK(o->get() == HWOPT_NONE);
	CHECK(o->set(2) && stored_rev == MD9600_REV_V5 && o->get() == 2 && strcmp(o->choices[2], "V4A/V5") == 0);
	CHECK(o->set(0) && stored_rev == MD9600_REV_V1V3 && o->get() == 0);
	CHECK(!o->set(3) && stored_rev == MD9600_REV_V1V3);
	store_ok = false;
	CHECK(!o->set(1) && o->get() == 0);
	store_ok = true;
#if CONFIG_MD9600_TX
	const hal_hwopt_t *t = &md9600_hwopts[1];
	CHECK(!t->restart && t->count == 2 && t->get() == 0);
	CHECK(t->set(1) && arm_state && t->get() == 1);
	CHECK(t->set(0) && !arm_state && t->get() == 0);
	arm_ok = false;
	CHECK(!t->set(1) && t->get() == 0);
	CHECK(!t->set(2));
#endif
}

int main(void)
{
	test_init();
	test_power_up();
	test_rx();
	test_lock();
	test_tx_gates();
#if CONFIG_MD9600_TX
	test_tx();
#endif
	test_probe();
	test_readings();
	test_cal();
	test_hwopt();
	test_rev_text();
	printf("md9600RfSeqTest (TX %s): %s (%d failures)\n", CONFIG_MD9600_TX ? "built" : "not built",
	       failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
