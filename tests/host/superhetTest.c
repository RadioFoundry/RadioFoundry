// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Superhet chip ops (drivers/rf/superhet): every required op is filled in, and each driver's ops write what the
// driver's own calls write, with the tone and AF kinds mapped. The AT1141's LO retunes only on a change. The chips are
// register files with a write log
#include <stdio.h>
#include <string.h>
#include "rf/superhet/superhet.h"
#include "rf/bk4819/bk4819.h"
#include "rf/bk4819/bk4819_superhet.h"
#include "rf/fd6818/fd6818sh.h"
#include "rf/fd6818/fd6818sh_superhet.h"
#include "rf/at1141/at1141.h"
#include "rf/at1141/at1141_superhet.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fake chip: one register file, a write log ---

#define LOG_MAX 512
typedef struct {
	uint8_t reg[LOG_MAX];
	uint16_t val[LOG_MAX];
	unsigned n;
} wlog_t;

static uint16_t regs[128];
static wlog_t cur;

static void w(rfbus_t *b, uint8_t reg, uint16_t val)
{
	(void)b;
	if (cur.n < LOG_MAX) {
		cur.reg[cur.n] = reg;
		cur.val[cur.n] = val;
	}
	cur.n++;
	regs[reg & 0x7F] = val;
}

static uint16_t r(rfbus_t *b, uint8_t reg) { (void)b; return regs[reg & 0x7F]; }
static void d(uint32_t ms) { (void)ms; }

static rfbus_t bus = { .write = w, .read = r, .delay_ms = d };

static void reset(void)
{
	memset(regs, 0, sizeof regs);
	memset(&cur, 0, sizeof cur);
}

static bool same(const wlog_t *a, const wlog_t *b)
{
	return a->n == b->n && a->n <= LOG_MAX && memcmp(a->reg, b->reg, a->n) == 0 &&
	       memcmp(a->val, b->val, a->n * sizeof a->val[0]) == 0;
}

// --- tests ---

static void test_filled(void)
{
	const superhet_main_ops_t *m[] = { &bk4819_superhet_main, &fd6818sh_superhet_main };
	for (unsigned i = 0; i < 2; i++) {
		CHECK(m[i]->idle && m[i]->rx && m[i]->to_if && m[i]->bandwidth && m[i]->rx_tone && m[i]->af &&
		      m[i]->af_gain && m[i]->tx_prepare && m[i]->tx_tune && m[i]->tx_end && m[i]->tail && m[i]->tone &&
		      m[i]->tone_on && m[i]->tone_key && m[i]->dtmf && m[i]->dmr_enter && m[i]->dmr_leave &&
		      m[i]->dmr_tx_prepare && m[i]->burst);
	}
	const superhet_lo_ops_t *l[] = { &bk4819_superhet_lo, &fd6818sh_superhet_lo, &at1141_superhet_lo, &at1141_superhet_lo_restart };
	for (unsigned i = 0; i < sizeof l / sizeof l[0]; i++) {
		CHECK(l[i]->tune && l[i]->off && l[i]->resume);
	}
	CHECK(bk4819_superhet_main.gpio == NULL && fd6818sh_superhet_main.gpio != NULL);
	CHECK(at1141_superhet_lo.gpio != NULL && at1141_superhet_lo_restart.gpio != NULL);
}

static void test_fd6818_main(void)
{
	fd6818sh_t a, b;
	wlog_t direct;

	reset();
	fd6818sh_attach(&a, &bus, FD6818SH_MAIN);
	fd6818sh_set_state(&a, FD6818SH_STATE_IDLE);
	fd6818sh_tune(&a, 51550000u);
	fd6818sh_set_rx_tone(&a, FD6818SH_TONE_DCS_INV, 0x023);
	fd6818sh_set_af(&a, FD6818SH_AF_RX);
	fd6818sh_tx_prepare(&a, FD6818SH_TONE_CTCSS, 1000, 446000000u, 9);
	fd6818sh_tx_tune(&a, 446000000u);
	fd6818sh_tx_end(&a);
	fd6818sh_dmr_mode(&a);
	fd6818sh_burst_tune(&a, 446000000u, true);
	direct = cur;

	reset();
	fd6818sh_attach(&b, &bus, FD6818SH_MAIN);
	superhet_main_t m = { &fd6818sh_superhet_main, &b };
	m.ops->to_if(m.chip, 51550000u);
	m.ops->rx_tone(m.chip, SUPERHET_TONE_DCS_INV, 0x023);
	m.ops->af(m.chip, SUPERHET_AF_FM);
	m.ops->tx_prepare(m.chip, SUPERHET_TONE_CTCSS, 1000, 446000000u, 9);
	m.ops->tx_tune(m.chip, 446000000u);
	m.ops->tx_end(m.chip);
	m.ops->dmr_enter(m.chip, 7);	// the digital set keeps its own gain: no 0x53 write
	m.ops->burst(m.chip, 446000000u, true);
	CHECK(same(&direct, &cur));
}

// The DMR AF gain doesn't reach the FD6818's 0x53: the digital set has its own, and the analog volume comes back after
static void test_fd6818_dmr_volume(void)
{
	fd6818sh_t c;
	reset();
	fd6818sh_attach(&c, &bus, FD6818SH_MAIN);
	superhet_main_t m = { &fd6818sh_superhet_main, &c };
	m.ops->af_gain(m.chip, 8);
	m.ops->dmr_enter(m.chip, 4);
	CHECK(regs[0x53] == 0x957D);
	m.ops->dmr_leave(m.chip);
	CHECK(regs[0x53] == fd6818sh_volume_word(8));
}

static void test_bk4819_main(void)
{
	bk4819_t a, b;
	wlog_t direct;

	reset();
	bk4819_attach(&a, &bus);
	bk4819_set_rx_tone(&a, BK4819_TONE_DCS, 0x023);
	bk4819_set_af(&a, BK4819_AF_DMR);
	bk4819_set_bandwidth(&a, BK4819_BW_12K5);
	bk4819_tx_prepare(&a, BK4819_TONE_CTCSS, 885);
	bk4819_burst_tune(&a, 446000000u, BK4819_STATE_TX_MAIN);
	direct = cur;

	reset();
	bk4819_attach(&b, &bus);
	superhet_main_t m = { &bk4819_superhet_main, &b };
	m.ops->rx_tone(m.chip, SUPERHET_TONE_DCS, 0x023);
	m.ops->af(m.chip, SUPERHET_AF_DMR);
	m.ops->bandwidth(m.chip, false);
	m.ops->tx_prepare(m.chip, SUPERHET_TONE_CTCSS, 885, 446000000u, 9);	// the PA gain is not the BK4819's
	m.ops->burst(m.chip, 446000000u, true);
	CHECK(same(&direct, &cur));
}

static void test_at1141_lo(void)
{
	at1141_t a;
	reset();
	at1141_attach(&a, &bus);
	superhet_lo_t lo = { &at1141_superhet_lo, &a };
	lo.ops->tune(lo.chip, 497550000u);
	unsigned first = cur.n;
	CHECK(first > 0 && a.lo_hz == 497550000u);

	// The same frequency again: enable only, as at1141_enable writes
	cur.n = 0;
	lo.ops->tune(lo.chip, 497550000u);
	wlog_t again = cur;
	cur.n = 0;
	at1141_enable(&a);
	CHECK(same(&again, &cur));
	CHECK(again.n < first);

	// Off and resume are the live disable / enable
	cur.n = 0;
	lo.ops->off(lo.chip);
	wlog_t off = cur;
	cur.n = 0;
	at1141_disable(&a);
	CHECK(same(&off, &cur));
}

// The restart variant: every tune writes the whole tuning, off for TX, then the restart (with its wait)
static void test_at1141_lo_restart(void)
{
	at1141_t a;
	reset();
	at1141_attach(&a, &bus);
	superhet_lo_t lo = { &at1141_superhet_lo_restart, &a };
	lo.ops->tune(lo.chip, 497550000u);
	cur.n = 0;
	lo.ops->tune(lo.chip, 497550000u);
	wlog_t again = cur;
	cur.n = 0;
	at1141_tune(&a, 497550000u);
	CHECK(same(&again, &cur) && a.lo_hz == 497550000u);

	cur.n = 0;
	lo.ops->off(lo.chip);
	wlog_t off = cur;
	cur.n = 0;
	at1141_off(&a);
	CHECK(same(&off, &cur));

	cur.n = 0;
	lo.ops->resume(lo.chip);
	wlog_t on = cur;
	cur.n = 0;
	at1141_restart(&a);
	CHECK(same(&on, &cur));
}

int main(void)
{
	test_filled();
	test_fd6818_main();
	test_fd6818_dmr_volume();
	test_bk4819_main();
	test_at1141_lo();
	test_at1141_lo_restart();
	printf("superhetTest: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
