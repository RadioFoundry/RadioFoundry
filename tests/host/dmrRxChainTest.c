// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR receive chain end to end on the host: a model of the HR_C6000's receive reports (the bench call of
// 2026-10-10: 8 voice LC headers, 16 superframes, 3 terminators, CC 2, MS sync, direct mode) drives the real slot engine
// (drivers/modem/hrc_common/hrc6000_dmr.c) through its interrupt entry points; a board-like task drains the engine's
// events into the real call stack (core/dmr/call/dmr_call.c) at its own pace (several bursts per batch when it runs
// late), and the stack's poll runs on yet another cadence. The voice sink checks that every burst on air comes out
// once, in order, with nothing concealed.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem/hrc_common/hrc6000_dmr.h"
#include "modem/hrc6000/hrc6000.h"
#include "dmr/call/dmr_call.h"
#include "dmr/call/dmr_lc.h"

static int failures;

// The stack's RX trace lines (dlog, built with DMR_RX_TRACE=1)
static char trace[16384];
static size_t trace_len;

void dlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(trace + trace_len, sizeof(trace) - trace_len, fmt, ap);
	va_end(ap);
	if (n > 0 && trace_len + (size_t)n + 1 < sizeof(trace)) {
		trace_len += (size_t)n;
		trace[trace_len++] = '\n';
		trace[trace_len] = '\0';
	}
}

// Trace entries (words after "crx @slot"), and how many have the given 'what' character (third character)
static int trace_entries(char what)
{
	int n = 0;
	const char *p = trace;
	while ((p = strstr(p, "crx @")) != NULL) {
		const char *eol = strchr(p, '\n');
		const char *q = strchr(p + 5, ' ');
		while (q != NULL && q < eol) {
			q++;
			if (what == 0 || q[2] == what) {
				n++;
			}
			q = strchr(q, ' ');
		}
		p = eol;
	}
	return n;
}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// --- Fake chip buses: reads come from rd[page][reg], voice reads from voice_rx ---

static uint8_t rd[8][256];
static uint8_t voice_rx[DMR_VOICE_BYTES];

static bool fake_transfer(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
	uint8_t page = hdr[0] & 7u, reg = hdr[1];
	if ((hdr[0] & 0x80u) != 0 && rx != NULL) {
		for (uint16_t i = 0; i < len; i++) {
			rx[i] = rd[page][(uint8_t)(reg + i)];
		}
	} else if (page == 4 && reg == 0x83 && tx != NULL && len == 1) {
		rd[4][0x82] &= (uint8_t)~tx[0];	// flags cleared: the system interrupt line goes high when none is left
	}
	return true;
}

static bool fake_voice(void *ctx, bool write, uint8_t *buf, uint8_t len)
{
	if (!write) {
		memcpy(buf, voice_rx, len);
	}
	return true;
}

static void no_delay(uint32_t v) { (void)v; }

static const hrc_bus_t bus = { fake_transfer, no_delay, no_delay, NULL, fake_voice };
static hrc_t dev;
static hrc6000_dmr_t eng;
static uint32_t t_us;

static bool fake_rf(void *ctx, bool tx) { return true; }
static uint32_t fake_now(void *ctx) { return t_us; }

static bool fake_sys_line_low(void *ctx) { return rd[4][0x82] != 0; }

static const hrc6000_dmr_ops_t ops = { fake_rf, fake_now, NULL, NULL, NULL, fake_sys_line_low, NULL, NULL };

// --- The board glue (what boards/dm1701/dmr_dm1701.c does): hal_dmr_modem_t over the engine, a draining task ---

static dmr_event_cb_t stack_cb;
static void *stack_ctx;

static bool m_start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	hrc6000_dmr_configure(&eng, cfg);
	stack_cb = cb;
	stack_ctx = ctx;
	return true;
}
static void m_stop(void) { stack_cb = NULL; }
static void m_configure(const dmr_config_t *cfg) { hrc6000_dmr_configure(&eng, cfg); }
static void m_rx_restart(void) { hrc6000_dmr_rx_restart(&eng); }
static void m_rx_lock_slot(int8_t ts) { hrc6000_dmr_rx_lock_slot(&eng, ts); }
static bool m_tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a) { return hrc6000_dmr_tx_begin(&eng, t, ts, a); }
static bool m_tx_queue(const dmr_tx_burst_t *b) { return hrc6000_dmr_tx_queue(&eng, b); }
static uint8_t m_tx_queue_space(void) { return hrc6000_dmr_tx_queue_space(&eng); }
static void m_tx_end(void) { hrc6000_dmr_tx_end(&eng); }
static void m_tx_abort(void) { hrc6000_dmr_tx_abort(&eng); }
static void m_set_embedded_lc(const uint8_t lc[9]) { hrc6000_dmr_set_embedded_lc(&eng, lc); }
static void m_set_cc_filter(int8_t cc) { hrc6000_dmr_set_cc_filter(&eng, cc); }
static bool m_is_transmitting(void) { return hrc6000_dmr_is_transmitting(&eng); }

static const hal_dmr_modem_t modem = {
	.start = m_start, .stop = m_stop, .configure = m_configure, .rx_restart = m_rx_restart,
	.rx_lock_slot = m_rx_lock_slot, .tx_begin = m_tx_begin, .tx_queue = m_tx_queue,
	.tx_queue_space = m_tx_queue_space, .tx_end = m_tx_end, .tx_abort = m_tx_abort,
	.set_embedded_lc = m_set_embedded_lc, .set_cc_filter = m_set_cc_filter, .is_transmitting = m_is_transmitting,
};

static unsigned task_voice_events;

static void board_task(void)
{
	dmr_event_t ev;
	dmr_burst_t b;
	if (rd[4][0x82] != 0) {
		hrc6000_dmr_isr_sys(&eng);	// a flag still holds the line low: no edge will come (the board pends it)
	}
	while (hrc6000_dmr_pop(&eng, &ev, &b)) {
		if (ev.kind == DMR_EV_BURST_RX && b.kind == DMR_BURST_VOICE) {
			task_voice_events++;
		}
		if (stack_cb != NULL) {
			stack_cb(&ev, stack_ctx);
		}
	}
}

// --- Voice sink: frames carry (burst number, frame in burst) in their first two bytes ---

static bool sink_verbose = true;	// print the first frame out of order

static struct {
	unsigned begins, ends, frames, losts;
	unsigned next_burst;	// burst number expected next (1-based)
	unsigned next_frame;
	bool in_order;
	bool from_any;		// a late entry: the first frame played sets the burst expected (later ones in order)
	unsigned first_burst;	// the burst number of the first frame played
} sk;

static void s_begin(const dmr_caller_t *c, void *ctx) { sk.begins++; }
static void s_end(void *ctx) { sk.ends++; }
static void s_lost(void *ctx) { sk.losts++; }
static void s_frame(const uint8_t f[DMR_AMBE_FRAME_BYTES], void *ctx)
{
	if (sk.frames == 0) {
		sk.first_burst = f[0];
		if (sk.from_any) {
			sk.next_burst = f[0];
		}
	}
	if (f[0] != (uint8_t)sk.next_burst || f[1] != sk.next_frame) {
		if (sk.in_order && sink_verbose) {
			printf("  sink: frame %u.%u, expected %u.%u\n", f[0], f[1], sk.next_burst & 0xFFu, sk.next_frame);
		}
		sk.in_order = false;
	}
	sk.frames++;
	if (++sk.next_frame == 3) {
		sk.next_frame = 0;
		sk.next_burst++;
	}
}

static const dmr_voice_sink_t sink = { .begin = s_begin, .frame = s_frame, .lost = s_lost, .end = s_end };

// --- The chip model ---

typedef struct {
	bool a_cc_junk;		// burst A (no EMB, no CC on air): 0x52 holds a junk color code at its report
	uint8_t empty;		// the empty slot's report: 0 none, 1 class 0 without a number, 2 the last number again
	bool echo_a;		// the empty slot after an A reports voice sync again (class 1)
	uint8_t noise_a;	// lone voice-sync reports before and after the call
	uint32_t task_ms;	// the board task's period (it drains everything each time)
	uint32_t task_late_every;	// every n-th run is delayed by task_late_ms (0 = never)
	uint32_t task_late_ms;
	uint32_t poll_ms;	// the stack's poll period
	bool same_lc_before;	// the previous call heard had the same LC (late entry's stale LC)
	unsigned voice_lost_at;	// voice burst number whose report also flags voice sync lost on its slot (0 = none)
	uint32_t late_us;	// reports after the first serviced this long after their timeslot interrupt (0 = 3.5 ms)
	bool late_junk;		// ... and read then: 0x52 holds CC 5 for voice, class 2 types read as 0 (the chip moved on)
	bool sticky;		// no system interrupt edge: a report waits with the line low until someone looks
	unsigned late;		// late entry: no headers, the first n voice bursts not sent (the receiver joins at n + 1)
	const uint8_t *page2_before;	// late: page 2 until this call's embedded LC is first complete (the sticky LC)
	bool page2_at_f;	// ... and the chip has written the LC to page 2 by the first F's report (else only after it:
				// the HIL behavior, see test_late_entry)
	bool post_access;	// the post-access interrupt flagged with the report after that F
} chip_t;

typedef struct {
	uint8_t r51, r52;
	bool voice;
	unsigned burst_no;	// voice: 1-based number of the burst on air
	uint8_t page2[12];
	bool post_access;
} report_t;

static report_t sched[4096];	// per tick: the report at that tick (r51 0xFF = none)

static const uint8_t call_lc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x23, 0x45, 0x67 };	// TG 9 from 0x234567

static unsigned build(const chip_t *c, unsigned start)
{
	for (unsigned i = 0; i < sizeof(sched) / sizeof(sched[0]); i++) {
		memset(&sched[i], 0, sizeof(sched[i]));
		sched[i].r51 = 0xFF;
	}
	unsigned k = start;
	uint8_t hdr[12], term[12];
	dmr_lc_encode_full(call_lc, DMR_RS_MASK_VOICE_HEADER, hdr);
	dmr_lc_encode_full(call_lc, DMR_RS_MASK_TERMINATOR, term);
	uint8_t last51 = 0;
	unsigned burst_no = 0;
	// noise before the call: lone voice sync reports, some ticks apart
	for (unsigned i = 0; i < c->noise_a; i++) {
		sched[start - 40 + i * 7].r51 = 0x91;
		sched[start - 40 + i * 7].r52 = 0x20;
		sched[start - 40 + i * 7].voice = true;
	}
	unsigned on_air_from = c->late;	// the first voice burst on air (0-based)
	for (int i = 0; i < 8 + 96 + 3; i++) {
		if (c->late != 0 && (i < 8 || (i < 8 + 96 && (unsigned)(i - 8) < c->late))) {
			if (i >= 8) {
				++burst_no;	// left out on air, numbered all the same
			}
			continue;
		}
		report_t *r = &sched[k];
		bool late_f = false;
		r->r52 = 0x20;
		if (i < 8) {
			r->r51 = 0x12;
			memcpy(r->page2, hdr, 12);
		} else if (i < 8 + 96) {
			int v = (i - 8) % 6, sf = (i - 8) / 6;
			r->voice = true;
			r->burst_no = ++burst_no;
			if (v == 0) {
				r->r51 = 0x91;
				if (c->a_cc_junk) {
					r->r52 = (uint8_t)((sf % 3) == 0 ? 0x00 : 0x50);
				}
			} else {
				bool bit7 = ((sf + v) & 1) != 0;
				r->r51 = (uint8_t)(((bit7 ? 0x8 : 0x0) | (v + 1)) << 4);
			}
			memcpy(r->page2, call_lc, 9);	// the embedded LC, complete at F (and sticky)
			if (c->page2_before != NULL) {
				// Complete at the first F whose B-E were all on air: until then the chip holds the old LC (and
				// at that F's own report too, unless page2_at_f)
				unsigned first_full_f = (on_air_from / 6u + (on_air_from % 6u <= 1u ? 0u : 1u)) * 6u + 5u;
				unsigned n = (unsigned)(i - 8);
				if (n < first_full_f || (n == first_full_f && !c->page2_at_f)) {
					memcpy(r->page2, c->page2_before, 9);
				}
				late_f = n == first_full_f;
			}
		} else {
			r->r51 = 0x22;
			memcpy(r->page2, term, 12);
		}
		last51 = r->r51;
		// the empty slot
		report_t *e = &sched[k + 1];
		if (c->echo_a && (last51 & 3u) == 1u) {
			*e = *r;
			e->voice = false;
		} else if (c->empty == 1) {
			e->r51 = 0x00;
			e->r52 = 0x20;
		} else if (c->empty == 2) {
			e->r51 = (uint8_t)(last51 & 0x70);
			e->r52 = 0x20;
		}
		memcpy(e->page2, r->page2, 12);
		if (late_f) {
			memcpy(e->page2, call_lc, 9);	// written after F's report
			if (c->post_access) {
				e->post_access = true;
				if (e->r51 == 0xFF) {
					e->r51 = 0x00;	// a report for the interrupt to come with
					e->r52 = 0x20;
				}
			}
		}
		k += 2;
	}
	unsigned end = k;
	for (int i = 0; i < 20; i++) {	// after the call: the last status repeated
		sched[k].r51 = 0x60;
		sched[k].r52 = 0x50;
		k++;
	}
	for (unsigned i = 0; i < c->noise_a; i++) {
		sched[k + 30 + i * 9].r51 = 0x91;
		sched[k + 30 + i * 9].r52 = 0x20;
	}
	return end;
}

typedef struct {
	unsigned voice_events;	// voice bursts the engine delivered
	dmr_call_stats_t cs;
} result_t;

static void run_call(const chip_t *c, result_t *res)
{
	memset(rd, 0, sizeof(rd));
	trace_len = 0;
	trace[0] = '\0';
	memset(&sk, 0, sizeof(sk));
	sk.next_burst = 1;
	sk.in_order = true;
	sk.from_any = c->late != 0;
	task_voice_events = 0;
	t_us = 1000000;
	hrc6000_dmr_init(&eng, &dev, &ops);
	hrc6000_dmr_run(&eng);

	dmr_call_init(&modem);
	dmr_call_set_voice(&sink, NULL);
	dmr_call_config_t cfg;
	dmr_call_config_default(&cfg);
	cfg.own_id = 1234;
	cfg.cc = 2;
	cfg.ts = 1;
	cfg.talk_target.id = 9;
	dmr_call_configure(&cfg);
	CHECK(dmr_call_start());

	const unsigned start = 100;
	unsigned end = build(c, start);
	unsigned last_tick = end + 20 + 30 + c->noise_a * 9 + 40;

	if (c->same_lc_before) {
		// an earlier identical call that ended: the stack's stale LC is this call's
		uint32_t t0 = 0;
		dmr_burst_t h = { .kind = DMR_BURST_VOICE_LC_HEADER, .cc = 2, .sync = DMR_SYNC_MS, .crc_ok = true,
			.len = 12 };
		dmr_lc_encode_full(call_lc, DMR_RS_MASK_VOICE_HEADER, h.payload);
		dmr_event_t ev = { .kind = DMR_EV_BURST_RX, .burst = &h };
		stack_cb(&ev, stack_ctx);
		dmr_call_poll(t0 + 1);
		dmr_call_poll(t0 + 1000);	// faded out
		memset(&sk, 0, sizeof(sk));
		sk.next_burst = 1;
		sk.in_order = true;
		sk.from_any = c->late != 0;
	}
	dmr_call_reset_stats();
	trace_len = 0;
	trace[0] = '\0';

	uint32_t t_task = t_us, t_poll = t_us + 3000;
	unsigned runs = 0;
	uint32_t tick_t = t_us + 30000;
	bool r_seen = false;
	for (unsigned tick = 1; tick <= last_tick; tick++) {
		// everything due before this tick's interrupt
		while (t_task < tick_t || t_poll < tick_t) {
			if (t_task <= t_poll) {
				t_us = t_task;
				board_task();
				runs++;
				uint32_t d = c->task_ms * 1000u;
				if (c->task_late_every && runs % c->task_late_every == 0) {
					d += c->task_late_ms * 1000u;
				}
				t_task += d;
			} else {
				t_us = t_poll;
				dmr_call_poll(t_poll / 1000u);
				t_poll += c->poll_ms * 1000u;
			}
		}
		t_us = tick_t;
		hrc6000_dmr_isr_timeslot(&eng);
		const report_t *r = &sched[tick];
		if (r->r51 != 0xFF) {
			memcpy(&rd[2][0], r->page2, 12);
			memset(voice_rx, 0, sizeof(voice_rx));
			if (r->voice) {
				for (int f = 0; f < 3; f++) {
					voice_rx[f * 9] = (uint8_t)r->burst_no;
					voice_rx[f * 9 + 1] = (uint8_t)f;
				}
			}
			bool first = !r_seen;
			r_seen = true;
			uint32_t delay = first || c->late_us == 0 ? 3500 : c->late_us;
			t_us = tick_t + delay;
			rd[4][0x82] = r->post_access ? 0x18 : 0x08;
			if (c->voice_lost_at != 0 && r->voice && r->burst_no == c->voice_lost_at) {
				rd[4][0x82] = 0x08 | 0x02;	// received data, and voice abnormal: slot 0 voice sync lost
				rd[4][0x98] = 0x02;
			}
			rd[4][0x51] = r->r51;
			rd[4][0x52] = r->r52;
			rd[4][0x5F] = 0xF0;
			if (c->late_junk && !first && delay >= 12000) {
				if ((r->r51 & 3u) == 2u) {
					rd[4][0x51] = (uint8_t)(r->r51 & 0x0Fu);
				} else {
					rd[4][0x52] = 0x50;
				}
			}
			if (!c->sticky || first) {
				hrc6000_dmr_isr_sys(&eng);
				rd[4][0x82] = 0;
			}
		}
		tick_t += 30000;
	}
	for (int i = 0; i < 100; i++) {	// let the stack fade the call out
		t_us += 10000;
		board_task();
		dmr_call_poll(t_us / 1000u);
	}
	res->voice_events = task_voice_events;
	dmr_call_get_stats(&res->cs);
}

static bool expect_clean(const char *name, const chip_t *c)
{
	result_t r;
	run_call(c, &r);
	bool ok = r.voice_events == 96 && eng.st.rx_voice == 96 && r.cs.rx_voice_bursts == 96 &&
		  r.cs.rx_lost_frames == 0 && sk.frames == 288 && sk.losts == 0 && sk.in_order && sk.begins == 1 &&
		  r.cs.rx_late_entries == 0;
	if (!ok) {
		printf("  %s: driver V%u (events %u) nc %u st %u sf %u sj %u dup %u | call voice %u lost %u dup %u late %u"
		       " calls %u | sink frames %u lost %u begins %u in_order %d\n", name, eng.st.rx_voice,
		       r.voice_events, eng.st.rx_no_call, eng.st.rx_stale, eng.st.rx_seq_fill, eng.st.rx_seq_jump,
		       eng.st.rx_dup, r.cs.rx_voice_bursts, r.cs.rx_lost_frames, r.cs.rx_dup_dropped,
		       r.cs.rx_late_entries, r.cs.rx_calls, sk.frames, sk.losts, sk.begins, sk.in_order);
		printf("    restarts:");
		for (int k = 0; k < HRC6000_RS_COUNT; k++) {
			printf(" %u", eng.st.rs_why[k]);
		}
		printf(" oos %u at %u\n", eng.st.rx_out_of_step, eng.st.sys_at_tick);
	}
	return ok;
}

static const chip_t base = { .empty = 1, .task_ms = 10, .poll_ms = 20 };

static void test_clean_chain(void)
{
	chip_t c = base;
	CHECK(expect_clean("clean", &c));
	// The trace: every burst once (8 headers, 96 voice, 3 terminators), the first header starts the call, all voice
	// played, the first terminator ends it
	CHECK(trace_entries(0) == 107);
	CHECK(trace_entries('p') == 96 && trace_entries('h') == 1 && trace_entries('k') == 1);
	CHECK(strstr(trace, " H1h") != NULL && strstr(trace, " A1p/2 B1p/2") != NULL);
	c.empty = 2;	// the empty slot repeats the last number
	CHECK(expect_clean("empty repeats", &c));
	c.empty = 0;
	CHECK(expect_clean("no empty reports", &c));
}

// Burst A carries no color code on air (voice sync, no EMB): whatever 0x52 holds at its report must not drop it
static void test_burst_a_cc(void)
{
	chip_t c = base;
	c.a_cc_junk = true;
	CHECK(expect_clean("A cc junk", &c));
}

// The board task runs late (higher priority work, flash writes): bursts reach the stack several at a time, with the
// same arrival time; the stack's poll is slower still
static void test_batched_delivery(void)
{
	chip_t c = base;
	c.a_cc_junk = true;
	c.task_ms = 10;
	c.task_late_every = 5;
	c.task_late_ms = 110;	// four bursts in one batch
	c.poll_ms = 25;
	CHECK(expect_clean("batched", &c));
	c.task_ms = 45;
	c.task_late_every = 0;
	c.poll_ms = 50;
	CHECK(expect_clean("slow task", &c));
}

// Lone voice sync reports (noise, the other slot's echo of A) are not bursts on air
static void test_lone_voice_sync(void)
{
	chip_t c = base;
	c.noise_a = 3;
	CHECK(expect_clean("noise A", &c));
	c.noise_a = 0;
	c.echo_a = true;
	CHECK(expect_clean("echo A", &c));
}

// The same call again (same LC as the last one heard): the header starts it, nothing lost to the stale LC rule
// The chip flags voice sync lost after a burst: the next burst is concealed (three frames), and when it still comes
// it is that burst again by its index and slot counter, so it is dropped, whatever its arrival time. Before, a burst
// arriving 40 ms or more after the last one in arrival time (the task batching) cost five more bursts of concealment
// Bench 2026-10-10 on 90ce091: reports 12-22 ms after the timeslot interrupt (outside the chip's normal 1-12 ms),
// read when 0x52 held CC 5 and class 2 types had gone to 0. The driver took only every other voice burst (B, D, F:
// the ones between were "extra reports" by delay alone) and gave them CC 5, which the stack dropped:
//   drx @16995 F15 B15/4 D15/4 F15/4 ...      crx @16995 F1x B1x/4 D1x/4 ...
// Every burst must still come through once, with the call's CC
static void test_late_reports(void)
{
	chip_t c = base;
	c.late_us = 15000;
	c.late_junk = true;
	CHECK(expect_clean("late reports", &c));
	if (getenv("RXCHAIN_TRACE") != NULL) {
		printf("%s", trace);
	}
	// No edge at all: each report waits with the line low; the timeslot interrupt services it for its own slot
	c = base;
	c.sticky = true;
	c.task_ms = 40;
	CHECK(expect_clean("sticky line", &c));
	CHECK(eng.st.sys_at_tick >= 40);	// the rest were pended by the board's task before the next tick
	c.task_ms = 95;
	CHECK(expect_clean("sticky line, slow task", &c));
}

static void test_voice_lost(void)
{
	chip_t c = base;
	c.voice_lost_at = 40;
	sink_verbose = false;
	c.task_late_every = 3;
	c.task_late_ms = 50;
	result_t r;
	run_call(&c, &r);
	CHECK(r.voice_events == 96 && r.cs.rx_voice_bursts == 95 && r.cs.rx_lost_frames == 3 && r.cs.rx_dup_dropped == 1);
	CHECK(sk.frames == 285 && sk.losts == 3);
	sink_verbose = true;
}

static void test_same_call_again(void)
{
	chip_t c = base;
	c.same_lc_before = true;
	CHECK(expect_clean("same LC again", &c));
}

// The LC trace entry i of the last reception as "<ev><res>" (or "" past the end)
static const char *lc_tr(unsigned i)
{
	static char t[3];
	t[0] = t[1] = t[2] = '\0';
	if (i < eng.st.lc_n) {
		t[0] = eng.st.lc_tr[i].ev;
		t[1] = eng.st.lc_tr[i].res;
	}
	return t;
}

static void late_run(const char *name, const chip_t *c, result_t *r)
{
	run_call(c, r);
	printf("  late entry, %s: played %u from burst %u (late %u, calls %u, in order %d) cap %c lc", name,
	       r->cs.rx_voice_bursts, sk.first_burst, r->cs.rx_late_entries, r->cs.rx_calls, sk.in_order,
	       eng.st.lc_cap_how ? eng.st.lc_cap_how : '?');
	for (unsigned i = 0; i < eng.st.lc_n; i++) {
		printf(" %c%c@%u", eng.st.lc_tr[i].ev, eng.st.lc_tr[i].res, eng.st.lc_tr[i].tk);
	}
	printf("\n");
	CHECK(r->cs.rx_calls == 1 && r->cs.rx_late_entries == 1 && sk.begins == 1 && sk.in_order && sk.losts == 0);
}

// Late entry (HIL test_late_entry). No header; the first 8 bursts are not sent, so the receiver hears from burst C of
// the second superframe. The engine takes voice only from a burst A on (C-F before it are status the chip reports with
// no superframe open: the capture of the sticky LC is made at the first of them), so the first embedded LC it can give
// comes from the third superframe's B-E (bursts 14-17).
//   - HIL 2026-10-10 on c8c9b23: a new LC (another talker before) was taken only at burst 24, 13 bursts played, where
//     this model then had page 2 updated by the first F's report and predicted 18 / 19 bursts. The chip writes the LC
//     after F's (first) report: page 2 still holds the old LC when F is read, and the new one from the next report on.
//     The engine looks again at the next burst A while it has no LC: played from burst 19, 18 bursts.
//   - With page 2 written by F's report (the earlier model): played from F, burst 18.
//   - The previous call had the same LC: the engine trusts an unchanged LC only after more than one superframe of
//     voice (the next F, burst 24), 13 bursts
//   - The post-access interrupt with the report after F: the LC is kept and handed over with the next voice burst (A)
static void test_late_entry(void)
{
	static const uint8_t other_lc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x11, 0x11, 0x11 };
	chip_t c = base;
	c.late = 8;
	c.page2_before = other_lc;
	result_t r;
	late_run("new LC", &c, &r);
	CHECK(sk.first_burst == 19 && r.cs.rx_voice_bursts == 96 - 18);
	// The trace: capture at a status report, held back at F (same as the capture), taken at the next A
	CHECK(eng.st.lc_cap_how == '0' && eng.st.lc_cap_src == 0x111111);
	CHECK(eng.st.lc_n == 3 && strcmp(lc_tr(0), "As") == 0 && strcmp(lc_tr(1), "Fs") == 0 &&
	      strcmp(lc_tr(2), "An") == 0);
	CHECK(eng.st.lc_tr[2].src == 0x234567 && eng.st.lc_tr[2].vss == 7 && eng.st.lc_f_stale == 1 &&
	      eng.st.lc_a_new == 1);

	c.page2_at_f = true;
	late_run("new LC, page 2 by F", &c, &r);
	CHECK(sk.first_burst == 18 && r.cs.rx_voice_bursts == 96 - 17);
	CHECK(eng.st.lc_n == 2 && strcmp(lc_tr(1), "Fn") == 0);

	c.page2_at_f = false;
	c.post_access = true;
	late_run("new LC, post access", &c, &r);
	CHECK(sk.first_burst == 19 && r.cs.rx_voice_bursts == 96 - 18);
	CHECK(eng.st.late_entry == 1 && eng.st.lc_pa_new == 1 && eng.st.lc_kept == 1);
	CHECK(eng.st.lc_n == 3 && strcmp(lc_tr(2), "Pk") == 0);

	c.post_access = false;
	c.page2_before = call_lc;
	c.same_lc_before = true;
	late_run("same LC", &c, &r);
	CHECK(sk.first_burst == 24 && r.cs.rx_voice_bursts == 96 - 23);
	CHECK(eng.st.lc_n == 4 && strcmp(lc_tr(1), "Fs") == 0 && strcmp(lc_tr(2), "As") == 0 &&
	      strcmp(lc_tr(3), "Fv") == 0);
}

int main(void)
{
	hrc6000_attach(&dev, &bus);
	test_clean_chain();
	test_burst_a_cc();
	test_batched_delivery();
	test_lone_voice_sync();
	test_same_call_again();
	test_voice_lost();
	test_late_reports();
	test_late_entry();
	if (failures) {
		printf("# dmrRxChainTest: %d FAILED\n", failures);
		return 1;
	}
	printf("# dmrRxChainTest: PASSED\n");
	return 0;
}
