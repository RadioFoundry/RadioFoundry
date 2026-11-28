// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The provider on a board's sample taps (air_tap.h)
#include "air_tap.h"

#include <stddef.h>
#include <string.h>

#include "air_syms.h"

// Samples per read: a poll reads until the tap is empty or READS_MAX reads, this many at a time (on the polling
// task's stack)
#define READ_SAMPLES	32
#define READS_MAX	16	// per poll: 512 samples, 32 ms at 16 kHz
#define POLL_WORK_US	1000	// per poll, then the task's own wait (2 ms on the boards that poll every 2 ms)
// t_us accuracy: the delay through the RF chip and the tap's filters is not measured
#define TIME_US		1000

// Transmit: the queue of chunks, and how far ahead of the air the transmit tap is kept. AHEAD_US covers the board's
// poll and the work in it; when the queue is empty and the tap less than LOW_US ahead, idle symbols go out
#define TX_CHUNK	48
#define TX_DEPTH	4
#define TX_LEAD_MS	20
#define AHEAD_US	30000
#define LOW_US		8000
#define TX_SPS_MAX	10

enum { TX_OFF, TX_QUEUED, TX_RUN, TX_DRAIN };

static const air_tap_cfg_t *cfg;
static const air_profile_t *prof;
static volatile bool is_open;
static volatile bool restart;		// rx_restart asked: the next poll drops what the tap holds
static bool rx_on;			// the receive tap runs (stopped while transmitting)
static air_event_cb_t cb;
static void *cb_ctx;
static air_rx_counts_t counts;		// halves: the reads that gave samples
static uint32_t work_us, span_us;
static uint32_t us_q16;			// microseconds a sample, x 65536
static uint32_t next_us;		// where the next read should start, when have_next
static bool have_next;

// The transmit side. Before tx_begin only the radio core's task queues; after it, the provider's (inside a poll)
typedef struct {
	uint8_t syms[TX_DEPTH][TX_CHUNK / 4];
	uint8_t len[TX_DEPTH];
	uint8_t head, count, at;	// the oldest chunk, the chunks queued, the next symbol in the oldest
	uint8_t state;
	bool last;			// the newest queued chunk is the last (AIR_TX_LAST or tx_end)
	bool started;			// AIR_EV_TX_STARTED went
	bool dry;			// idle symbols are going out: AIR_EV_TX_UNDERRUN went
	bool done;			// AIR_EV_TX_DONE to send at the next poll
	bool quiet;			// inside tx_begin: the events wait for the poll
	uint8_t space_due;		// chunks that left the queue while quiet
	uint8_t tail;			// symbols of nothing still to send after the last
	uint8_t idle_at;
	uint8_t pend_n, pend_at;	// samples of the last symbol the tap has not taken yet
	uint32_t t0_us;			// when sample 0 went out
	uint32_t written;		// samples the tap took
	uint32_t us_q16;		// microseconds a sample, x 65536
	int16_t pend[TX_SPS_MAX];
} tx_t;

static tx_t tx;

static void lock(void)
{
	if (cfg->lock != NULL)
		cfg->lock();
}

static void unlock(void)
{
	if (cfg->unlock != NULL)
		cfg->unlock();
}

void air_tap_setup(const air_tap_cfg_t *c)
{
	cfg = c;
}

bool air_tap_is_open(void)
{
	return is_open;
}

bool air_tap_transmitting(void)
{
	return is_open && tx.state >= TX_RUN;
}

static uint32_t fit(const air_profile_t *p)
{
	uint32_t f;

	if (cfg == NULL)
		return 0;
	f = air_softmodem_rx_fit(p, cfg->bb);
	if (cfg->tx != NULL)
		f |= air_softmodem_tx_fit(p, cfg->bb);
	return f ? f | cfg->fit_flags : 0;
}

static void emit(uint8_t kind, uint32_t t_us)
{
	air_event_t ev = { .kind = kind, .t_us = t_us, .arg = 0, .chunk = NULL };

	cb(&ev, cb_ctx);
}

// The soft modem's chunks to the session (inside air_tap_poll, on the board's task)
static void sm_out(const air_chunk_t *c, void *ctx)
{
	air_event_t ev = { .kind = AIR_EV_RX, .t_us = c->t_us, .arg = 0, .chunk = c };

	(void)ctx;
	counts.chunks++;
	cb(&ev, cb_ctx);
}

// The receiver from its start: its state may have been the transmitter's
static bool rx_begin(void)
{
	const hal_baseband_t *bb = cfg->bb;

	if (bb->rx_start == NULL)
		return false;
	if (!air_softmodem_rx_open(cfg->sm, prof, bb->rx_rate_hz, bb->rx_counts_per_khz, bb->rx_slope_hz, sm_out, NULL))
		return false;
	have_next = false;
	restart = false;
	rx_on = bb->rx_start();
	return rx_on;
}

static void rx_end(void)
{
	if (rx_on)
		cfg->bb->rx_stop();
	rx_on = false;
}

static void tx_reset(void)
{
	memset(&tx, 0, offsetof(tx_t, pend));
}

static bool tap_open(const air_profile_t *p, air_event_cb_t event_cb, void *ctx)
{
	bool ok = true;

	if (is_open || event_cb == NULL || !fit(p))
		return false;
	lock();
	prof = p;
	cb = event_cb;
	cb_ctx = ctx;
	memset(&counts, 0, sizeof counts);
	work_us = span_us = 0;
	tx_reset();
	if (cfg->bb->rx_start != NULL) {
		us_q16 = (uint32_t)((1000000ull << 16) / cfg->bb->rx_rate_hz);
		// The tap last: its samples are read only once the state above is set
		ok = rx_begin();
	}
	is_open = ok;
	unlock();
	return ok;
}

static void tx_halt(void)
{
	if (tx.state >= TX_RUN)
		cfg->bb->tx_stop();
	tx_reset();
}

static void tap_close(void)
{
	if (!is_open)
		return;
	lock();		// a poll inside the soft modem finishes first
	is_open = false;
	tx_halt();
	rx_end();
	unlock();
}

static void tap_rx_restart(void)
{
	restart = true;
}

// ── Transmit ──
static bool tx_queue(const air_tx_chunk_t *c)
{
	bool ok = false;

	if (!is_open || cfg->tx == NULL || c->nsyms == 0 || c->nsyms > TX_CHUNK)
		return false;
	lock();
	if (tx.state <= TX_RUN && !tx.last && tx.count < TX_DEPTH) {
		uint8_t slot = (uint8_t)((tx.head + tx.count) % TX_DEPTH);

		memcpy(tx.syms[slot], c->syms, (size_t)(c->nsyms + 3) / 4);
		tx.len[slot] = (uint8_t)c->nsyms;
		tx.count++;
		tx.last = (c->flags & AIR_TX_LAST) != 0;
		if (tx.state == TX_OFF)
			tx.state = TX_QUEUED;
		ok = true;
	}
	unlock();
	return ok;
}

static uint8_t tx_space(void)
{
	return is_open && cfg->tx != NULL && !tx.last && tx.state <= TX_RUN ? (uint8_t)(TX_DEPTH - tx.count) : 0;
}

static void tx_end(void)
{
	lock();
	if (tx.state != TX_OFF)
		tx.last = true;
	unlock();
}

// The tap's time of the next sample it takes
static uint32_t tx_time(void)
{
	return tx.t0_us + (uint32_t)(((uint64_t)tx.written * tx.us_q16) >> 16);
}

// The next symbol to modulate: a queued one, the tail after the last, or idle symbols when the queue ran dry. False:
// nothing to send now (more may come), or the tail is out (state TX_DRAIN)
static bool next_sym(uint8_t *dibit, int32_t ahead)
{
	if (tx.count) {
		*dibit = air_sym_get(tx.syms[tx.head], tx.at);
		if (++tx.at == tx.len[tx.head]) {
			tx.at = 0;
			tx.head = (uint8_t)((tx.head + 1) % TX_DEPTH);
			tx.count--;
			tx.dry = false;
			if (tx.quiet)
				tx.space_due++;
			else
				emit(AIR_EV_TX_SPACE, tx_time());	// the session may queue the next one from inside
		}
		return true;
	}
	if (tx.last) {
		if (!tx.tail) {
			tx.state = TX_DRAIN;
			return false;
		}
		tx.tail--;
		*dibit = AIR_SOFTMODEM_TX_NONE;
		return true;
	}
	if (ahead >= LOW_US)
		return false;
	if (!tx.dry) {
		tx.dry = true;
		if (!tx.quiet) {
			emit(AIR_EV_TX_UNDERRUN, tx_time());
			if (tx.count)
				return next_sym(dibit, ahead);	// the session queued from inside the event
		}
	}
	*dibit = (uint8_t)((prof->idle >> (6 - 2 * tx.idle_at)) & 3);
	tx.idle_at = (uint8_t)((tx.idle_at + 1) & 3);
	return true;
}

// Samples to the tap until it is AHEAD_US ahead of the air at now (or full)
static void tx_fill(uint32_t now)
{
	const hal_baseband_t *bb = cfg->bb;

	for (;;) {
		int32_t ahead = (int32_t)(tx_time() - now);
		uint8_t dibit;
		uint16_t n;

		if (tx.pend_at == tx.pend_n) {
			if (ahead >= AHEAD_US || tx.state != TX_RUN || !next_sym(&dibit, ahead))
				return;
			tx.pend_n = air_softmodem_tx_sym(cfg->tx, dibit, tx.pend);
			tx.pend_at = 0;
		}
		n = bb->tx_write(tx.pend + tx.pend_at, (uint16_t)(tx.pend_n - tx.pend_at));
		tx.pend_at = (uint8_t)(tx.pend_at + n);
		tx.written += n;
		if (tx.pend_at < tx.pend_n)
			return;			// the tap is full
	}
}

static bool tx_begin(void)
{
	const hal_baseband_t *bb = cfg->bb;
	bool ok = false;

	if (!is_open || cfg->tx == NULL)
		return false;
	lock();
	if (tx.state == TX_QUEUED && tx.count) {
		rx_end();
		if (air_softmodem_tx_open(cfg->tx, prof, bb)) {
			tx.state = TX_RUN;
			tx.tail = air_softmodem_tx_tail(cfg->tx);
			tx.us_q16 = (uint32_t)((1000000ull << 16) / bb->tx_rate_hz);
			tx.written = 0;
			tx.pend_n = tx.pend_at = 0;
			// The first AHEAD_US of samples before the tap's clock starts (t0 0 until then), so sample 0 goes out
			// at its start. Their events wait for the poll: this is the radio core's task
			tx.quiet = true;
			tx_fill(0);
			tx.quiet = false;
			ok = bb->tx_start();
			tx.t0_us = bb->now_us();
		}
		if (!ok) {
			tx_reset();
			rx_begin();
		}
	}
	unlock();
	return ok;
}

static void tx_abort(void)
{
	lock();
	if (is_open && tx.state != TX_OFF) {
		bool was_on = tx.state >= TX_RUN;

		tx_halt();
		if (was_on)
			rx_begin();
		tx.done = true;
	}
	unlock();
}

// Inside a poll, transmitting: the events, the samples, and the end
static void tx_poll(void)
{
	const hal_baseband_t *bb = cfg->bb;
	uint32_t now = bb->now_us();

	if (!tx.started) {
		tx.started = true;
		// The first queued symbol's center: after the transmitter's look-ahead
		emit(AIR_EV_TX_STARTED, tx.t0_us + (uint32_t)(((uint64_t)cfg->tx->lead * cfg->tx->sps * tx.us_q16) >> 16));
	}
	while (tx.space_due && tx.state >= TX_RUN) {
		tx.space_due--;
		emit(AIR_EV_TX_SPACE, now);
	}
	// The tap ran dry (a poll came late): its clock went on without samples, so the next one goes out now
	if (tx.state == TX_RUN && (int32_t)(tx_time() - now) < 0) {
		tx.t0_us += now - tx_time();
		emit(AIR_EV_TX_UNDERRUN, now);
	}
	tx_fill(now);
	if (tx.state == TX_DRAIN && (int32_t)(now - tx_time()) >= 0) {
		bb->tx_stop();
		tx_reset();
		rx_begin();
		emit(AIR_EV_TX_DONE, now);
	}
}

bool air_tap_poll(void)
{
	int16_t buf[READ_SAMPLES];
	const hal_baseband_t *bb;

	if (!is_open)
		return false;
	lock();
	if (!is_open) {
		unlock();
		return false;
	}
	bb = cfg->bb;
	if (tx.done) {
		tx.done = false;
		emit(AIR_EV_TX_DONE, bb->now_us());
	}
	if (tx.state >= TX_RUN) {
		tx_poll();
		unlock();
		return true;
	}
	if (!rx_on) {
		unlock();
		return true;
	}
	if (restart) {
		restart = false;
		while (bb->rx_read(buf, READ_SAMPLES) != 0)
			;
		air_softmodem_rx_reset(cfg->sm);
		have_next = false;
	}
	// At most READS_MAX reads and POLL_WORK_US of work a poll: a soft modem slower than the tap must not hold the
	// board's task (a high priority one) for good. The tap then loses samples, which a later read counts as an overrun
	uint32_t poll_work = 0;
	for (uint8_t reads = 0; reads < READS_MAX && poll_work < POLL_WORK_US; reads++) {
		uint32_t t = bb->rx_time_us(), t0, dur;
		uint16_t n = bb->rx_read(buf, READ_SAMPLES);
		int32_t miss;

		if (n == 0)
			break;
		counts.halves++;
		// A read that doesn't start where the last one ended: the tap lost samples (the soft modem starts over)
		miss = (int32_t)(t - next_us);
		if (have_next && (miss > (int32_t)(us_q16 >> 17) || miss < -(int32_t)(us_q16 >> 17)))
			counts.overruns++;
		t0 = bb->now_us();
		air_softmodem_rx(cfg->sm, buf, n, t);
		t0 = bb->now_us() - t0;
		work_us += t0;
		poll_work += t0;
		dur = (n * us_q16) >> 16;
		span_us += dur;
		next_us = t + dur;
		have_next = true;
		if (!rx_on || !is_open)
			break;		// an event inside closed the session or started a transmission
	}
	unlock();
	return true;
}

static uint32_t tap_now_us(void)
{
	return cfg->bb->now_us();
}

static bool tap_rx_load(uint32_t *w, uint32_t *s)
{
	*w = work_us;
	*s = span_us;
	return true;
}

static bool tap_rx_counts(air_rx_counts_t *c)
{
	if (!is_open)
		return false;
	*c = counts;
	return true;
}

const hal_air_t air_tap = {
	.fit = fit, .open = tap_open, .close = tap_close, .rx_restart = tap_rx_restart, .tx_begin = tx_begin,
	.tx_queue = tx_queue, .tx_space = tx_space, .tx_end = tx_end, .tx_abort = tx_abort,
	.now_us = tap_now_us, .slot_clock = NULL, .slot_slew = NULL, .rx_load = tap_rx_load, .rx_counts = tap_rx_counts,
	.tx_chunk_syms = TX_CHUNK, .tx_queue_depth = TX_DEPTH, .tx_lead_ms = TX_LEAD_MS, .time_accuracy_us = TIME_US,
};
