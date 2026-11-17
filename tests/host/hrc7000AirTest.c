// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 air interface provider and receive tap (drivers/modem/hrc7000/hrc7000_air.c; docs/spec/modem-hrc7000.md 10)
// on plain arrays standing in for the modem registers and buffers, with a stub soft modem that records what it is fed:
// the copy set-up and its undoing, every sample once and in order from the interrupt to the tap and the soft modem,
// time stamps, overrun and the receiver's restart at the gap, transmit halves per slot boundary with no stale half,
// idle on underrun, last-chunk padding, the modulation bytes set and put back, abort, the watchdog, suspend / resume,
// open / close / open, the DMR engine on the same memory after close, and a seeded fuzz of interrupt and task order.
#include <stdio.h>
#include <string.h>
#include "modem/hrc7000/hrc7000_air.h"
#include "fuzz.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	failures++; } } while (0)

#define REG(off)	regs[(off) / 4]
#define PHY_RDY		(1u << 12)
#define HALF		HRC7000_FM_RX_SAMPLES
#define DMR_MOD_BIAS	0x01E94A4Au	// a board's DMR value: modulation bytes 0x4A at 1944 Hz
#define IDLE		0x77u

static uint32_t regs[0x600 / 4];
static uint8_t buf[0xA00];
static hrc7000_t dev;

// The board's memory lent to the session, as the glue lays it: the slot engine's queues
typedef struct {
	hrc7000_air_t a;
	hrc7000_air_buf_t b;
} air_mem_t;
static hrc7000_dmr_t eng;
_Static_assert(sizeof(air_mem_t) <= sizeof(eng.evq), "the session no longer fits the engine's event queue");
// The glue (platform/hrc7000/dmr_glue.c) puts the soft modem in the engine's TX queue at its first aligned address and
// checks the fit for the target. Here pointers are 64-bit and the state larger, so it gets its own aligned memory
static air_softmodem_t sm_mem;
#define A	(&((air_mem_t *)(void *)eng.evq)->a)
#define B	(&((air_mem_t *)(void *)eng.evq)->b)
#define SM	(&sm_mem)

// --- ops ---

static uint32_t now;
static int rf_on, rf_off, pa_arms, lock_depth, lock_max, notifies;
static bool rf_state;

static void op_rf(void *ctx, bool tx)
{
	if (tx) {
		rf_on++;
	} else {
		rf_off++;
	}
	rf_state = tx;
}
static void op_pa_arm(void *ctx) { pa_arms++; }
static uint32_t op_now(void *ctx) { return now; }
static void op_lock(void *ctx) { if (++lock_depth > lock_max) lock_max = lock_depth; }
static void op_unlock(void *ctx) { lock_depth--; }
// The chip clears SYS_INTERP_LIST when the CPU writes the bits back; an array can't, so the event hook does it
static void op_notify(void *ctx) { notifies++; REG(C7_SYS_INTERP_LIST) = 0; }

static const hrc7000_dmr_ops_t ops = {
	.rf = op_rf, .pa_arm = op_pa_arm, .now_us = op_now, .lock = op_lock, .unlock = op_unlock, .notify = op_notify,
};

// --- The stub soft modem: every sample it is given, and where it was reset ---

#define GOT_MAX	200000
static int16_t got[GOT_MAX];
static int ngot, nresets, sm_opens;
static int reset_at[4096];
static uint32_t sample_t[0x8000];	// the time each sample value was made (values wrap at 0x8000)
static int bad_times;

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	return p->sym_rate * 5u <= bb->rx_rate_hz * 3u ? AIR_FIT_RX | AIR_FIT_SOFT : 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	memset(m, 0, sizeof(*m));
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	m->rate_hz = rate_hz;
	sm_opens++;
	return rate_hz == HRC7000_AIR_RATE_HZ;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	if (nresets < (int)(sizeof(reset_at) / sizeof(reset_at[0]))) {
		reset_at[nresets] = ngot;
	}
	nresets++;
}

// One chunk out per call: its first symbol's time is the first sample's, so the event path is checked too
void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	CHECK(n > 0 && n <= HALF);
	if (sample_t[(uint16_t)samples[0] & 0x7FFFu] != t_us) {
		bad_times++;
	}
	for (uint16_t i = 0; i < n && ngot < GOT_MAX; i++) {
		got[ngot++] = samples[i];
	}
	static const uint8_t hard[1] = { 0 };
	air_chunk_t c = { .t_us = t_us, .nsyms = 1, .sync_at = -1, .hard = hard };
	m->out(&c, m->ctx);
}

// --- Events ---

static air_event_t evs[4096];
static int nevs;

static void on_event(const air_event_t *ev, void *ctx)
{
	CHECK(lock_depth == 0);	// never called with the interrupts held off
	if (nevs < (int)(sizeof(evs) / sizeof(evs[0]))) {
		evs[nevs] = *ev;
		evs[nevs].chunk = NULL;
	}
	nevs++;
}

static int count_ev(uint8_t kind)
{
	int c = 0;
	for (int i = 0; i < nevs && i < (int)(sizeof(evs) / sizeof(evs[0])); i++) {
		c += evs[i].kind == kind;
	}
	return c;
}

// --- The chip ---

static uint16_t gen;	// the next sample value (15 bits)
static uint8_t next_sw = 1;

// The copy fills a half and raises system interrupt bit 14
static void copy_half(void)
{
	now += 32000;
	uint32_t t0 = now - (HALF - 1u) * 125u;
	uint32_t base = C7_FM_RX_AUDIO + (next_sw - 1u) * HALF * 2u;
	for (uint32_t i = 0; i < HALF; i++) {
		uint16_t v = gen;
		gen = (uint16_t)((gen + 1u) & 0x7FFFu);
		buf[base + 2u * i] = (uint8_t)(v >> 8);
		buf[base + 2u * i + 1u] = (uint8_t)v;
		sample_t[v] = t0 + i * 125u;
	}
	REG(C7_FM_ADDR_SW) = next_sw;
	next_sw = next_sw == 1 ? 2 : 1;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void phy_rdy(void)
{
	REG(C7_SYS_INTERP_LIST) = PHY_RDY;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
}

static void slot(void)
{
	now += 30000;
	hrc7000_air_isr_timeslot(A);
}

static const volatile uint8_t *ram_half(int h) { return &buf[C7_TX_RAM + (h == 0 ? 0x00u : 0x24u)]; }

// The bytes a boundary writes: all but the last, which waits for the next boundary (half_full)
static bool half_bytes(int h, const air_tx_chunk_t *c, int nbytes)
{
	const volatile uint8_t *ram = ram_half(h);
	for (int i = 0; i < nbytes; i++) {
		uint8_t want = IDLE;
		int n = c == NULL ? 0 : c->nsyms;
		if (4 * i + 4 <= n) {
			want = c->syms[i];
		} else if (4 * i < n) {
			uint8_t keep = (uint8_t)(0xFFu << (8 - 2 * (n - 4 * i)));
			want = (uint8_t)((c->syms[i] & keep) | (IDLE & ~keep));
		}
		if (ram[i] != want) {
			return false;
		}
	}
	return true;
}

static bool half_is(int h, const air_tx_chunk_t *c) { return half_bytes(h, c, HRC7000_AIR_HALF_BYTES - 1); }
static bool half_full(int h, const air_tx_chunk_t *c) { return half_bytes(h, c, HRC7000_AIR_HALF_BYTES); }

// The idle slots after the RF TX interrupt: the queue waits, nothing counted
static void lead(void)
{
	uint32_t under = A->st.tx_underruns;
	for (int i = 0; i < HRC7000_AIR_TX_LEAD_SLOTS; i++) {
		slot();
		CHECK(half_full(0, NULL) && half_full(1, NULL));
	}
	CHECK(A->st.tx_underruns == under);
}

static air_tx_chunk_t chunk(int k, uint16_t nsyms, uint8_t flags)
{
	air_tx_chunk_t c;
	memset(&c, 0, sizeof(c));
	c.nsyms = nsyms;
	c.flags = flags;
	for (int i = 0; i < HRC7000_AIR_HALF_BYTES; i++) {
		c.syms[i] = (uint8_t)(k * 37 + i * 5 + 1);
	}
	return c;
}

static air_profile_t prof;

static void setup(void)
{
	memset(regs, 0, sizeof regs);
	memset(buf, 0, sizeof buf);
	memset(&eng, 0x5A, sizeof eng);	// the engine's leftovers: the session must not care
	now = 1000000;
	rf_on = rf_off = pa_arms = lock_depth = lock_max = notifies = 0;
	rf_state = false;
	ngot = nresets = sm_opens = nevs = bad_times = 0;
	gen = 0;
	next_sw = 1;
	hrc7000_attach(&dev, regs, buf);
	// The chip as DMR receive leaves it
	REG(C7_SYS_INTERP_MASK) = 0x1234u;
	REG(C7_FM_BANDWIDTH) = 0x40u;
	REG(C7_FM_DEV_COEF) = 0x33500u;
	REG(C7_WORK_MODE) = C7_WORK_MODE_DMR;
	REG(C7_RF_MOD_BIAS) = DMR_MOD_BIAS;
	hrc7000_audio_bits(&dev, C7_AUDIO_FM_PLAY | 1u, 0);
	hrc7000_air_init(A, &dev, &ops, B, SM);

	memset(&prof, 0, sizeof prof);
	prof.sym_rate = 4800;
	prof.levels = 4;
	prof.shape = AIR_SHAPE_C4FM;
	prof.dev_outer_hz = 1800;
	prof.timing = AIR_TIMING_CONTINUOUS;
	prof.idle = IDLE;
	prof.nsyncs = 1;
	prof.sync[0].nsyms = 24;
}

static bool copy_set_up(void)
{
	return REG(C7_WORK_MODE) == C7_WORK_MODE_FM && REG(C7_FM_BANDWIDTH) == 0 &&
	       REG(C7_SYS_INTERP_MASK) == (C7_SYS_FM_RX | PHY_RDY) && (REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0 &&
	       REG(C7_LAYER2_CONTROL) == 0xC0u && REG(C7_LAYER2_TXRX_CTRL) == 0 && REG(C7_FM_PTT) == 0;
}

static bool dmr_set_up(void)
{
	return REG(C7_WORK_MODE) == C7_WORK_MODE_DMR && REG(C7_FM_BANDWIDTH) == 0x40u && REG(C7_FM_DEV_COEF) == 0x33500u &&
	       REG(C7_SYS_INTERP_MASK) == 0x1234u && REG(C7_LAYER2_TXRX_CTRL) == 0;
}

// Samples got in order, one apart, except right at a reset
static bool continuous(int from)
{
	int r = 0;
	for (int i = from + 1; i < ngot; i++) {
		while (r < nresets && reset_at[r] < i) {
			r++;
		}
		bool at_reset = r < nresets && reset_at[r] == i;
		if (!at_reset && got[i] != (int16_t)((got[i - 1] + 1) & 0x7FFF)) {
			printf("  sample %d: %d after %d\n", i, got[i], got[i - 1]);
			return false;
		}
	}
	return true;
}

// --- Tests ---

static void test_pure(void)
{
	air_profile_t p = { .sym_rate = 4800, .levels = 4, .shape = AIR_SHAPE_RRC, .shape_param = 20,
			    .dev_outer_hz = 1944, .timing = AIR_TIMING_CONTINUOUS };
	CHECK(hrc7000_air_tx_fit(&p) == AIR_FIT_TX);
	p.shape = AIR_SHAPE_C4FM;
	CHECK(hrc7000_air_tx_fit(&p) == (AIR_FIT_TX | AIR_FIT_SHAPE_APPROX));
	p.sym_rate = 2400;
	CHECK(hrc7000_air_tx_fit(&p) == (AIR_FIT_TX | AIR_FIT_SHAPE_APPROX | AIR_FIT_RATE_DOUBLED));
	p.sym_rate = 1200;
	CHECK(hrc7000_air_tx_fit(&p) == 0);
	p.sym_rate = 4800;
	p.timing = AIR_TIMING_TDMA;
	CHECK(hrc7000_air_tx_fit(&p) == 0);
	p.timing = AIR_TIMING_CONTINUOUS;
	p.dev_outer_hz = 7000;
	CHECK((hrc7000_air_tx_fit(&p) & AIR_FIT_DEV_APPROX) != 0);

	// Deviation is linear in the modulation bytes: both scaled, the bias half kept
	p.dev_outer_hz = 1944;
	CHECK(hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) == DMR_MOD_BIAS);
	p.dev_outer_hz = 1800;	// 0x4A * 1800 / 1944 = 68.5 -> 69
	CHECK(hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) == 0x01E94545u);
	p.dev_outer_hz = 2880;	// 109.6 -> 110
	CHECK(hrc7000_air_mod_bias(&p, 0x01E94A21u) == (0x01E90000u | 110u << 8 | 49u));
	p.dev_outer_hz = 6000;	// clamped
	CHECK((hrc7000_air_mod_bias(&p, 0x0000FFFFu) & 0xFFFFu) == 0xFFFFu);
	p.dev_outer_hz = 1;
	CHECK((hrc7000_air_mod_bias(&p, DMR_MOD_BIAS) & 0xFFFFu) == 0x0101u);
}

static void test_tap(void)
{
	setup();
	CHECK(hrc7000_air_tap_start(A));
	CHECK(copy_set_up() && lock_depth == 0);
	CHECK(!hrc7000_air_tap_start(A));
	CHECK(!hrc7000_air_open(A, &prof, AIR_FIT_RX, on_event, NULL));	// one at a time

	// Halves read back in pieces of every size, each sample once, timed
	int16_t out[600];
	uint16_t want = 0;
	bool ok = true, times = true;
	for (int k = 0; k < 40; k++) {
		copy_half();
		uint16_t sizes[3] = { (uint16_t)(1 + k % 7), (uint16_t)(100 + k), 600 };
		for (int s = 0; s < 3; s++) {
			uint32_t t = hrc7000_air_tap_time(A);
			uint16_t n = hrc7000_air_tap_read(A, out, sizes[s]);
			if (n > 0 && t != sample_t[(uint16_t)out[0]]) {
				times = false;
			}
			for (uint16_t i = 0; i < n; i++) {
				ok &= out[i] == (int16_t)want;
				want = (uint16_t)((want + 1u) & 0x7FFFu);
			}
		}
	}
	CHECK(ok && times && want == (uint16_t)(40u * HALF));
	CHECK(A->st.halves == 40 && A->st.overruns == 0);

	// Not read: the ring takes two halves, the third is dropped, and the read after shows the jump by its time
	copy_half();
	copy_half();
	copy_half();
	CHECK(A->st.overruns == 1);
	CHECK(hrc7000_air_tap_read(A, out, 600) == 2 * HALF);
	CHECK(out[0] == (int16_t)want && out[2 * HALF - 1] == (int16_t)((want + 2 * HALF - 1) & 0x7FFF));
	copy_half();
	uint32_t t = hrc7000_air_tap_time(A);
	CHECK(hrc7000_air_tap_read(A, out, 600) == HALF);
	CHECK(out[0] == (int16_t)((want + 3 * HALF) & 0x7FFF) && t == sample_t[(uint16_t)out[0]]);

	// FM_ADDR_SW naming neither half: nothing taken
	REG(C7_FM_ADDR_SW) = 0;
	REG(C7_SYS_INTERP_LIST) = C7_SYS_FM_RX;
	hrc7000_air_isr_sys(A);
	REG(C7_SYS_INTERP_LIST) = 0;
	CHECK(A->st.no_half == 1 && hrc7000_air_tap_read(A, out, 600) == 0);

	hrc7000_air_tap_stop(A);
	CHECK(dmr_set_up() && lock_depth == 0 && rf_on == 0);
	CHECK((REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0);	// the board's DMR entry sets its own audio paths
	copy_half();
	CHECK(hrc7000_air_tap_read(A, out, 600) == 0);
}

static void test_rx_session(void)
{
	setup();
	CHECK(!hrc7000_air_open(A, &prof, 0, on_event, NULL));
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	CHECK(copy_set_up() && sm_opens == 1 && lock_depth == 0);

	for (int k = 0; k < 30; k++) {
		copy_half();
		if (k % 3 != 2) {
			hrc7000_air_poll(A);
		}
	}
	hrc7000_air_poll(A);
	CHECK(ngot == 30 * HALF && got[0] == 0 && continuous(0) && bad_times == 0);
	CHECK(nresets == 1 && reset_at[0] == 0);	// the first half follows a new set-up
	CHECK(count_ev(AIR_EV_RX) > 0 && count_ev(AIR_EV_RX) == (int)A->st.chunks);

	// The task behind: the ring holds two halves, the third is lost, and the receiver restarts exactly where the
	// samples jump
	copy_half();
	copy_half();
	copy_half();
	hrc7000_air_poll(A);
	CHECK(A->st.overruns == 1 && ngot == 32 * HALF && nresets == 1 && continuous(0));
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == 33 * HALF && nresets == 2 && reset_at[1] == 32 * HALF);
	CHECK(got[32 * HALF] == (int16_t)((33 * HALF) & 0x7FFF) && continuous(0));

	// A restart drops what the ring holds and starts the receiver over at the next half
	copy_half();
	hrc7000_air_rx_restart(A);
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == 34 * HALF && nresets == 3 && reset_at[2] == 33 * HALF && continuous(0));
	CHECK(got[33 * HALF] == (int16_t)((35 * HALF) & 0x7FFF));
	CHECK(bad_times == 0 && lock_max == 1);

	hrc7000_air_close(A, true);
	CHECK(dmr_set_up());
	int before = nevs;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(nevs == before);
}

// Four chunks, the last short: each boundary writes only the half the next slot reads
static void test_tx(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c[4] = { chunk(0, 144, 0), chunk(1, 144, 0), chunk(2, 144, 0), chunk(3, 50, AIR_TX_LAST) };
	CHECK(!hrc7000_air_tx_begin(A));	// nothing queued
	CHECK(hrc7000_air_tx_space(A) == HRC7000_AIR_TXQ);
	for (int i = 0; i < 4; i++) {
		CHECK(hrc7000_air_tx_queue(A, &c[i]));
	}
	CHECK(hrc7000_air_tx_space(A) == 0 && !hrc7000_air_tx_queue(A, &c[0]));

	CHECK(hrc7000_air_tx_begin(A));
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_PHY && REG(C7_LAYER2_CONTROL) == 0x82u && REG(C7_AUDIO_BUFFER_CLR) == 3);
	CHECK(REG(C7_RF_MOD_BIAS) == 0x01E94545u);
	CHECK(half_is(0, NULL) && half_is(1, NULL));
	CHECK(!hrc7000_air_tx_begin(A) && hrc7000_air_tx_active(A));
	uint32_t t_begin = now;

	// Halves while the chip sends: ignored, not counted lost
	copy_half();
	CHECK(A->st.halves == 0 && A->st.overruns == 0);

	// A boundary before the RF TX interrupt: idle, the queue waits
	REG(C7_LAYER2_TXRX_CTRL) = 0;
	now = t_begin + 10000;
	hrc7000_air_isr_timeslot(A);
	CHECK(half_full(0, NULL) && half_full(1, NULL) && REG(C7_LAYER2_TXRX_CTRL) == 0x80u);
	CHECK(hrc7000_air_tx_space(A) == 0);

	// The RF TX interrupt keys the RF once, then idle slots while the RF chain comes up
	hrc7000_air_isr_rftx(A);
	hrc7000_air_isr_rftx(A);
	CHECK(rf_on == 1 && pa_arms == 1 && rf_state);
	lead();
	CHECK(hrc7000_air_tx_space(A) == 0);
	uint32_t slots0 = A->st.tx_slots;
	int h = A->half, g = h ^ 1;	// the idle slots moved which half comes first

	// Each boundary writes the half the next slot reads, its last byte at the boundary after
	slot();
	uint32_t b1 = now;
	CHECK(half_is(h, &c[0]) && !half_full(h, &c[0]) && half_full(g, NULL));
	// The same boundary's PHY_RDY: nothing more
	now += 800;
	phy_rdy();
	CHECK(A->st.tx_slots == slots0 + 1);

	slot();
	CHECK(half_is(g, &c[1]) && half_full(h, &c[0]));
	slot();
	CHECK(half_is(h, &c[2]) && half_full(g, &c[1]));
	slot();
	CHECK(half_is(g, &c[3]) && half_full(h, &c[2]));	// the last one padded with idle
	CHECK(rf_state && rf_off == 0);
	slot();
	CHECK(half_is(h, NULL) && half_full(g, &c[3]) && rf_state);	// the tail: idle while the last chunk leaves the air
	CHECK(A->st.tx_slots == slots0 + 5 && A->st.tx_underruns == 0);
	slot();
	CHECK(!rf_state && rf_off == 1 && !hrc7000_air_tx_active(A));
	CHECK(REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS && copy_set_up());

	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_STARTED) == 1 && count_ev(AIR_EV_TX_SPACE) == 1 && count_ev(AIR_EV_TX_DONE) == 1);	// in a row: one
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 0);
	CHECK(evs[0].kind == AIR_EV_TX_STARTED && evs[0].t_us == b1 + 5000u + 104u);
	CHECK(evs[nevs - 1].kind == AIR_EV_TX_DONE);

	// Receiving again, the receiver restarted
	int resets = nresets;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == HALF && nresets == resets + 1 && lock_depth == 0);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up() && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
}

static void test_underrun_and_end(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_TX, on_event, NULL));	// transmit only: no copy set-up
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_DMR && (REG(C7_AUDIO_CONTROL) & C7_AUDIO_FM_PLAY) == 0);
	air_tx_chunk_t c0 = chunk(0, 144, 0), c1 = chunk(1, 144, 0);
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	lead();
	slot();
	CHECK(half_is(0, &c0));
	slot();
	CHECK(half_is(1, NULL) && half_full(0, &c0) && A->st.tx_underruns == 1);
	slot();
	CHECK(half_is(0, NULL) && A->st.tx_underruns == 2);
	CHECK(hrc7000_air_tx_queue(A, &c1));
	slot();
	CHECK(half_is(1, &c1));
	hrc7000_air_tx_end(A);	// nothing queued: ends with what went
	CHECK(hrc7000_air_tx_space(A) == 0 && !hrc7000_air_tx_queue(A, &c0));
	slot();
	CHECK(half_is(0, NULL) && rf_state);
	slot();
	CHECK(!rf_state && !hrc7000_air_tx_active(A));
	CHECK(REG(C7_WORK_MODE) == C7_WORK_MODE_FM && REG(C7_LAYER2_CONTROL) == 0xC0u);	// no copy: out of the PHY
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_UNDERRUN) == 1 && count_ev(AIR_EV_TX_SPACE) == 2 && count_ev(AIR_EV_TX_DONE) == 1);
	int order[5] = { AIR_EV_TX_STARTED, AIR_EV_TX_SPACE, AIR_EV_TX_UNDERRUN, AIR_EV_TX_SPACE, AIR_EV_TX_DONE };
	bool in_order = nevs == 5;
	for (int i = 0; i < 5 && in_order; i++) {
		in_order = evs[i].kind == order[i];
	}
	CHECK(in_order);

	// tx_end with a chunk queued makes it the last
	nevs = 0;
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_queue(A, &c1));
	hrc7000_air_tx_end(A);
	CHECK(hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	lead();
	slot();
	slot();
	CHECK(half_is(1, &c1));
	slot();
	slot();
	CHECK(!rf_state);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 1 && count_ev(AIR_EV_TX_UNDERRUN) == 0);
	hrc7000_air_close(A, true);
}

static void test_abort_watchdog_suspend(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c0 = chunk(0, 144, 0);

	// Abort mid-transmission: the RF off now, the bytes back, TX_DONE
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_tx_abort(A);
	CHECK(!rf_state && !hrc7000_air_tx_active(A) && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS && copy_set_up());
	CHECK(hrc7000_air_tx_space(A) == HRC7000_AIR_TXQ);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 1);

	// Never keyed (no RF TX interrupt): dropped after 200 ms
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	slot();
	hrc7000_air_poll(A);
	CHECK(hrc7000_air_tx_active(A));
	now += 200000;
	hrc7000_air_poll(A);
	CHECK(!hrc7000_air_tx_active(A) && A->st.stalls == 1 && rf_on == 1 && count_ev(AIR_EV_TX_DONE) == 2);

	// Keyed, then the slot clock stops: dropped after 200 ms
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	now += 199000;
	hrc7000_air_poll(A);
	CHECK(rf_state);
	now += 2000;
	hrc7000_air_poll(A);
	CHECK(!rf_state && A->st.stalls == 2 && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);

	// The board leaves DMR under the session: the transmission ends, nothing taken until it is back
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_suspend(A);
	CHECK(!rf_state && !hrc7000_air_tx_active(A));
	CHECK(!hrc7000_air_tx_queue(A, &c0) || !hrc7000_air_tx_begin(A));
	hrc7000_air_tx_abort(A);
	REG(C7_WORK_MODE) = 0x55;	// the board's FM set-up
	uint32_t halves = A->st.halves;
	copy_half();
	CHECK(A->st.halves == halves);
	hrc7000_air_resume(A);
	CHECK(copy_set_up());
	int resets = nresets;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(A->st.halves == halves + 1 && nresets == resets + 1);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up() && lock_depth == 0);
}

// Close mid-transmission, open again, then the DMR engine on the same memory
static void test_open_close_dmr(void)
{
	setup();
	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
	air_tx_chunk_t c0 = chunk(0, 144, 0);
	CHECK(hrc7000_air_tx_queue(A, &c0) && hrc7000_air_tx_begin(A));
	hrc7000_air_isr_rftx(A);
	slot();
	hrc7000_air_close(A, true);
	CHECK(!rf_state && dmr_set_up() && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
	hrc7000_air_poll(A);
	CHECK(count_ev(AIR_EV_TX_DONE) == 0);	// the session is over: nothing goes out
	slot();
	CHECK(A->st.tx_slots == 1);

	CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX, on_event, NULL));
	CHECK(copy_set_up() && hrc7000_air_tx_queue(A, &c0));
	CHECK(!hrc7000_air_tx_begin(A));	// receive-only fit
	hrc7000_air_tx_abort(A);
	int from = ngot;
	copy_half();
	hrc7000_air_poll(A);
	CHECK(ngot == from + (int)HALF);
	hrc7000_air_close(A, true);
	CHECK(dmr_set_up());

	// The board gives the queues back: the engine runs and receives as before
	eng.evq_head = eng.evq_tail = 0;
	eng.txq_head = eng.txq_tail = 0;
	hrc7000_dmr_init(&eng, &dev, &ops);
	hrc7000_dmr_run(&eng);
	CHECK(REG(C7_LAYER2_CONTROL) == 0xC0u && REG(C7_LAYER2_SLOTON) == 0x05u);
	for (int i = 0; i < 12; i++) {
		buf[C7_RX_RAM + i] = (uint8_t)(0x10 + i);
	}
	now += 30000;
	hrc7000_dmr_isr_timeslot(&eng, false);
	// A voice LC header: burst type 1, CC 3, MS sync, chip slot 1
	REG(C7_RX_TYPE_INFO) = 1u << 8 | 3u << 4 | 2u | 1u << 19;
	REG(C7_SYS_INTERP_LIST) = 1u << 3;
	hrc7000_dmr_isr_sys(&eng);
	REG(C7_SYS_INTERP_LIST) = 0;
	dmr_event_t ev;
	dmr_burst_t b;
	int bursts = 0;
	while (hrc7000_dmr_pop(&eng, &ev, &b)) {
		if (ev.kind == DMR_EV_BURST_RX) {
			bursts++;
			CHECK(b.kind == DMR_BURST_VOICE_LC_HEADER && b.cc == 3 && b.lc[0] == 0x10);
		}
	}
	if (bursts != 1) {
		printf("  dmr: rx_lc_header %u unusable %u crc %u ticks %u\n", (unsigned)eng.st.rx_lc_header,
		       (unsigned)eng.st.rx_unusable, (unsigned)eng.st.rx_crc_err, (unsigned)eng.st.ticks);
	}
	CHECK(bursts == 1);
	hrc7000_dmr_halt(&eng);
}

// Interrupts and task calls in any order: every sample at most once and in order, a jump only at a receiver restart,
// the RF never keyed outside a transmission, every transmission ending in one TX_DONE, the lock always let go
static void test_fuzz(void)
{
	fuzz_t f;
	fuzz_init(&f, 0xA1C7);
	long iters = fuzz_iters(200);
	int begun = 0;
	bool rf_ok = true, locks_ok = true;

	for (long it = 0; it < iters; it++) {
		setup();
		begun = 0;
		CHECK(hrc7000_air_open(A, &prof, AIR_FIT_RX | AIR_FIT_TX, on_event, NULL));
		for (int step = 0; step < 400; step++) {
			uint32_t op = fuzz_below(&f, 12);
			air_tx_chunk_t c = chunk((int)step, (uint16_t)(1 + fuzz_below(&f, 144)), 0);
			switch (op) {
			case 0: case 1: case 2:
				copy_half();
				break;
			case 3:
				hrc7000_air_poll(A);
				break;
			case 4:
				now += fuzz_below(&f, 40000);
				hrc7000_air_isr_timeslot(A);
				break;
			case 5:
				now += fuzz_below(&f, 2000);
				phy_rdy();
				break;
			case 6:
				hrc7000_air_isr_rftx(A);
				break;
			case 7:
				if (fuzz_below(&f, 8) == 0) {
					c.flags = AIR_TX_LAST;
				}
				hrc7000_air_tx_queue(A, &c);
				break;
			case 8:
				begun += hrc7000_air_tx_begin(A);
				break;
			case 9:
				if (fuzz_below(&f, 4) == 0) {
					hrc7000_air_tx_end(A);
				}
				break;
			case 10:
				if (fuzz_below(&f, 16) == 0) {
					hrc7000_air_tx_abort(A);
				} else if (fuzz_below(&f, 16) == 0) {
					hrc7000_air_rx_restart(A);
				}
				break;
			default:
				now += fuzz_below(&f, 100000);
				break;
			}
			rf_ok &= !rf_state || hrc7000_air_tx_active(A);
			locks_ok &= lock_depth == 0;
		}
		hrc7000_air_tx_abort(A);
		hrc7000_air_poll(A);
		CHECK(continuous(0) && bad_times == 0);
		CHECK(count_ev(AIR_EV_TX_DONE) == begun && count_ev(AIR_EV_TX_STARTED) <= begun);
		CHECK(!rf_state && REG(C7_RF_MOD_BIAS) == DMR_MOD_BIAS);
		hrc7000_air_close(A, true);
		CHECK(dmr_set_up());
		if (failures != 0) {
			printf("  fuzz iteration %ld\n", it);
			break;
		}
	}
	CHECK(rf_ok && locks_ok);
}

int main(void)
{
	test_pure();
	test_tap();
	test_rx_session();
	test_tx();
	test_underrun_and_end();
	test_abort_watchdog_suspend();
	test_open_close_dmr();
	test_fuzz();
	printf("hrc7000AirTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
