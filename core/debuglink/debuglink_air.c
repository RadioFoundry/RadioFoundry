// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Air interface bench access on the debug link (see debuglink_air.h; docs/spec/debuglink.md 3.28). Requests come from
// the debug link task; the provider's events from the board's modem task. The receive chunk ring between them is one
// producer, one consumer, and takes its memory from the RTOS heap only while a session is open (no static RAM).
#include "config.h"
#if CONFIG_AIR_RAW	// the whole file
#include <string.h>
#include "board.h"
#include "debuglink/debuglink_air.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "debuglink/dlog.h"
#include "radio/radio.h"
#include "FreeRTOS.h"
#include "task.h"

#define RING_BYTES	1280u	// receive records: 18 soft modem chunks of 48 symbols with soft values, 58 without
#define OPEN_SOFT	0x01u	// OPEN flags: soft values in the receive records (7 kB/s at 4800 symbols/s, 1.8 without)
#define REC_HDR		10u	// t_us u32, nsyms u16, flags u8, sync_at i16, sync_id u8 (bit 7 of flags: soft follows)
#define REC_SOFT	0x80u
#define TX_MAX_MS	30000u	// a bench transmission's cap when the host gives none

typedef struct {
	air_profile_t p;	// the session's: valid until close
	volatile uint32_t head, tail;	// free running byte counts
	uint8_t ring[RING_BYTES];
} rx_mem_t;

static const debuglink_air_core_t *core;
static rx_mem_t *mem;
static bool tap_on, open_on, entered, keyed, want_soft;
static uint8_t tap_bits = 16, tap_shift;
static uint32_t tap_seq, tap_next_us, tap_gaps;

// Session counters (written by the modem task's callback, read here)
static volatile uint32_t ev_rx, ev_dropped, ev_started, ev_underrun, ev_space, ev_done;
static volatile uint32_t t_started, t_done;

void debuglink_air_set_core(const debuglink_air_core_t *c)
{
	core = c;
}

static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static size_t status(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

// The radio handed over: through the core when it registered, else the radio must be idle (receiving, no call). A
// provider refuses outside DMR by itself
static uint8_t take_radio(void)
{
	if (core != NULL) {
		if (!entered && !core->enter()) {
			return DL_ST_BUSY;	// asked: the host tries again
		}
		entered = true;
		return DL_ST_OK;
	}
	return radio_get_state() == RS_IDLE ? DL_ST_OK : DL_ST_BUSY;
}

static void give_radio(void)
{
	if (core != NULL && entered) {
		core->leave();
	}
	entered = false;
}

// --- The tap ---

static size_t tap_start(const uint8_t *req, uint16_t len, uint8_t *out)
{
	const hal_baseband_t *bb = board.baseband;
	if (bb == NULL || bb->rx_start == NULL) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	if (len != 2 || (req[0] != 8 && req[0] != 16) || req[1] > 15) {
		return status(out, DL_ST_BAD_ARG);
	}
	if (tap_on || open_on) {
		return status(out, DL_ST_BUSY);
	}
	uint8_t st = take_radio();
	if (st != DL_ST_OK) {
		return status(out, st);
	}
	if (!bb->rx_start()) {
		give_radio();
		return status(out, DL_ST_REFUSED);
	}
	tap_on = true;
	tap_bits = req[0];
	tap_shift = req[1];
	tap_seq = 0;
	tap_gaps = 0;
	tap_next_us = bb->rx_time_us();
	dlog("debuglink: air tap on, %u bit", (unsigned)tap_bits);
	out[0] = DL_ST_OK;
	put32(out + 1, bb->rx_rate_hz);
	put16(out + 5, bb->rx_lo_hz);
	put16(out + 7, bb->rx_hi_hz);
	put16(out + 9, bb->rx_counts_per_khz);
	out[11] = bb->rx_channels;
	return 12;
}

// Samples since the last read: seq u32 (the first's index), t_us u32 (its time), gaps u32 (reads whose first sample was
// not where the last read left off by the clock), n u16, then n samples (int16 LE, or int8 after the shift)
static size_t tap_read(uint8_t *out, size_t cap)
{
	const hal_baseband_t *bb = board.baseband;
	if (!tap_on) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	// The samples land 2-aligned at 16 or 17 and move down to 15 (forward, so nothing unread is overwritten)
	uint8_t *at = out + 16 + ((uintptr_t)(out + 16) & 1u);
	size_t room = cap - 17u;
	uint16_t max = (uint16_t)(room / 2u);	// read as 16 bit either way: 8 bit replies are half full
	uint32_t t = bb->rx_time_us();
	int16_t *s = (int16_t *)(void *)at;
	uint16_t n = bb->rx_read(s, max);
	int32_t late = (int32_t)(t - tap_next_us);
	int32_t per_us = (int32_t)(1000000u / bb->rx_rate_hz);
	if (late > per_us || late < -per_us) {
		tap_gaps++;
	}
	tap_next_us = t + (uint32_t)((uint64_t)n * 1000000u / bb->rx_rate_hz);
	out[0] = DL_ST_OK;
	put32(out + 1, tap_seq);
	put32(out + 5, t);
	put32(out + 9, tap_gaps);
	put16(out + 13, n);
	tap_seq += n;
	if (tap_bits == 16) {
		memmove(out + 15, at, 2u * n);	// every target is little endian: the wire order already
		return 15u + 2u * n;
	}
	int8_t *d = (int8_t *)(out + 15);
	for (uint16_t i = 0; i < n; i++) {
		int32_t v = s[i] >> tap_shift;
		d[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
	}
	return 15u + n;
}

static size_t tap_stop(uint8_t *out)
{
	if (tap_on) {
		board.baseband->rx_stop();
		tap_on = false;
		give_radio();
		dlog("debuglink: air tap off, %u gaps", (unsigned)tap_gaps);
	}
	return status(out, DL_ST_OK);
}

// --- A session ---

// The provider's events (its modem task). Receive chunks go to the ring whole or not at all
static volatile uint8_t in_event;	// the modem task is inside on_event: the ring's memory is in use

static void on_event(const air_event_t *ev, void *ctx)
{
	(void)ctx;
	in_event = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rx_mem_t *m = mem;
	switch (ev->kind) {
	case AIR_EV_RX: {
		const air_chunk_t *c = ev->chunk;
		bool soft = c->soft != NULL && want_soft;
		uint32_t hard = (c->nsyms + 3u) / 4u;
		uint32_t need = REC_HDR + hard + (soft ? c->nsyms : 0u);
		if (m == NULL) {
			break;
		}
		uint32_t h = m->head;
		if (RING_BYTES - (h - m->tail) < need) {
			ev_dropped++;
			break;
		}
		uint8_t hdr[REC_HDR];
		put32(hdr, c->t_us);
		put16(hdr + 4, c->nsyms);
		hdr[6] = (uint8_t)((c->flags & 0x7Fu) | (soft ? REC_SOFT : 0u));
		put16(hdr + 7, (uint16_t)c->sync_at);
		hdr[9] = c->sync_id;
		for (uint32_t i = 0; i < need; i++) {
			uint8_t b = i < REC_HDR ? hdr[i] : i < REC_HDR + hard ? c->hard[i - REC_HDR] :
				    (uint8_t)c->soft[i - REC_HDR - hard];
			m->ring[(h + i) % RING_BYTES] = b;
		}
		__atomic_thread_fence(__ATOMIC_RELEASE);
		m->head = h + need;
		ev_rx++;
		break;
	}
	case AIR_EV_TX_STARTED:
		ev_started++;
		t_started = ev->t_us;
		break;
	case AIR_EV_TX_UNDERRUN:
		ev_underrun++;
		break;
	case AIR_EV_TX_SPACE:
		ev_space++;
		break;
	case AIR_EV_TX_DONE:
		ev_done++;
		t_done = ev->t_us;
		if (keyed) {
			keyed = false;
			core->tx(false, 0);	// the transmitter off, by the core (posted)
		}
		break;
	default:
		break;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	in_event = 0;
}

// The profile from the host: rate u16, levels, shape, shape_param, dev_outer_hz u16, timing, bw, slot_syms u16,
// sync_every_syms u16, idle, nsyncs (15 bytes), then per sync: id, nsyms, max_errs, flags, frame_at, syms[6]
#define PROF_HDR	15u
#define PROF_SYNC	(5u + AIR_SYNC_SYMS_MAX / 4u)

static bool profile(const uint8_t *b, uint16_t len, air_profile_t *p)
{
	if (len < PROF_HDR) {
		return false;
	}
	memset(p, 0, sizeof(*p));
	p->sym_rate = get16(b);
	p->levels = b[2];
	p->shape = b[3];
	p->shape_param = b[4];
	p->dev_outer_hz = get16(b + 5);
	p->timing = b[7];
	p->bw = b[8];
	p->slot_syms = get16(b + 9);
	p->sync_every_syms = get16(b + 11);
	p->idle = b[13];
	p->nsyncs = b[14];
	if (p->nsyncs > AIR_SYNCS_MAX || len != PROF_HDR + p->nsyncs * PROF_SYNC) {
		return false;
	}
	for (uint8_t i = 0; i < p->nsyncs; i++) {
		const uint8_t *s = b + PROF_HDR + i * PROF_SYNC;
		air_sync_t *y = &p->sync[i];
		y->id = s[0];
		y->nsyms = s[1];
		y->max_errs = s[2];
		y->flags = s[3];
		y->frame_at = (int8_t)s[4];
		memcpy(y->syms, s + 5, sizeof(y->syms));
		if (y->nsyms < 8 || y->nsyms > AIR_SYNC_SYMS_MAX) {
			return false;
		}
	}
	return p->sym_rate != 0 && (p->levels == 2 || p->levels == 4);
}

static void session_end(void)
{
	if (keyed) {
		board.air->tx_abort();
	}
	board.air->close();
	if (keyed) {
		keyed = false;
		core->tx(false, 0);
	}
	open_on = false;
	rx_mem_t *m = mem;
	mem = NULL;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (int i = 0; in_event && i < 50; i++) {
		vTaskDelay(1);	// the callback the close raced with (the provider calls none after close)
	}
	vPortFree(m);
	give_radio();
}

static size_t air_open(const uint8_t *req, uint16_t len, uint8_t *out)
{
	const hal_air_t *air = board.air;
	if (air == NULL) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	if (tap_on || open_on) {
		return status(out, DL_ST_BUSY);
	}
	if (len < 1) {
		return status(out, DL_ST_BAD_ARG);
	}
	uint8_t flags = req[0];
	rx_mem_t *m = pvPortMalloc(sizeof(rx_mem_t));
	if (m == NULL) {
		dlog("debuglink: air open, no heap for %u bytes", (unsigned)sizeof(rx_mem_t));
		return status(out, DL_ST_BUSY);
	}
	if (!profile(req + 1, (uint16_t)(len - 1u), &m->p)) {
		vPortFree(m);
		return status(out, DL_ST_BAD_ARG);
	}
	uint32_t fit = air->fit(&m->p);
	if (fit == 0) {
		vPortFree(m);
		out[0] = DL_ST_REFUSED;
		put32(out + 1, 0);
		return 5;
	}
	uint8_t st = take_radio();
	if (st != DL_ST_OK) {
		vPortFree(m);
		return status(out, st);
	}
	m->head = m->tail = 0;
	want_soft = (flags & OPEN_SOFT) != 0;
	ev_rx = ev_dropped = ev_started = ev_underrun = ev_space = ev_done = 0;
	mem = m;
	if (!air->open(&m->p, on_event, NULL)) {
		mem = NULL;
		vPortFree(m);
		give_radio();
		out[0] = DL_ST_REFUSED;
		put32(out + 1, fit);
		return 5;
	}
	open_on = true;
	dlog("debuglink: air open, fit 0x%x", (unsigned)fit);
	out[0] = DL_ST_OK;
	put32(out + 1, fit);
	put16(out + 5, air->tx_chunk_syms);
	out[7] = air->tx_queue_depth;
	return 8;
}

// Whole records from the ring: dropped u32, count u8, then the records as stored (REC_HDR, hard, soft)
static size_t rx_read(uint8_t *out, size_t cap)
{
	rx_mem_t *m = mem;
	if (!open_on || m == NULL) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	size_t at = 6;
	uint8_t count = 0;
	uint32_t t = m->tail;
	for (;;) {
		uint32_t h = m->head;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (t == h || count == 255u) {
			break;
		}
		uint8_t f = m->ring[(t + 6u) % RING_BYTES];
		uint32_t n = (uint32_t)m->ring[(t + 4u) % RING_BYTES] | (uint32_t)m->ring[(t + 5u) % RING_BYTES] << 8;
		uint32_t size = REC_HDR + (n + 3u) / 4u + ((f & REC_SOFT) != 0 ? n : 0u);
		if (at + size > cap) {
			break;
		}
		for (uint32_t i = 0; i < size; i++) {
			out[at + i] = m->ring[(t + i) % RING_BYTES];
		}
		at += size;
		t += size;
		count++;
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	m->tail = t;
	out[0] = DL_ST_OK;
	put32(out + 1, ev_dropped);
	out[5] = count;
	return at;
}

// One transmit chunk: flags u8 (AIR_TX_xxx), nsyms u8, the symbols packed (hal_air.h). Answers the space left
static size_t tx_data(const uint8_t *req, uint16_t len, uint8_t *out)
{
	const hal_air_t *air = board.air;
	if (!open_on) {
		return status(out, DL_ST_UNAVAILABLE);
	}
	if (len < 2 || req[1] == 0 || req[1] > air->tx_chunk_syms || len != 2u + (req[1] + 3u) / 4u) {
		return status(out, DL_ST_BAD_ARG);
	}
	air_tx_chunk_t c;
	memset(&c, 0, sizeof(c));
	c.flags = req[0] & (AIR_TX_LAST | AIR_TX_SLOT_EMPTY);
	c.nsyms = req[1];
	memcpy(c.syms, req + 2, (req[1] + 3u) / 4u);
	if (!air->tx_queue(&c)) {
		out[0] = DL_ST_BUSY;
		out[1] = air->tx_space();
		return 2;
	}
	out[0] = DL_ST_OK;
	out[1] = air->tx_space();
	return 2;
}

// Key: max_ms u16 (0 = TX_MAX_MS). At least one chunk queued first
static size_t tx_key(const uint8_t *req, uint16_t len, uint8_t *out)
{
	if (!open_on || core == NULL || core->tx == NULL) {
		return status(out, DL_ST_UNAVAILABLE);	// keying is the radio core's, never the bench's own
	}
	if (len != 2) {
		return status(out, DL_ST_BAD_ARG);
	}
	if (keyed) {
		return status(out, DL_ST_BUSY);
	}
	uint16_t ms = get16(req);
	keyed = true;	// before: a TX_DONE from the modem task may come at once
	if (!core->tx(true, ms != 0 ? ms : (uint16_t)TX_MAX_MS)) {
		keyed = false;
		return status(out, DL_ST_REFUSED);
	}
	dlog("debuglink: air key, %u ms at most", (unsigned)(ms != 0 ? ms : TX_MAX_MS));
	return status(out, DL_ST_OK);
}

static void status_text(dl_text_t *t)
{
	dl_text(t, "air.built=1\nair.provider=%u\nair.tap=%u\nair.core=%u\n", board.air != NULL,
		board.baseband != NULL && board.baseband->rx_start != NULL, core != NULL);
	dl_text(t, "air.state=%s\nair.keyed=%u\n", open_on ? "open" : tap_on ? "tap" : "idle", keyed);
	dl_text(t, "air.rx_chunks=%u\nair.rx_dropped=%u\n", (unsigned)ev_rx, (unsigned)ev_dropped);
	dl_text(t, "air.tx_started=%u\nair.tx_started_us=%u\nair.tx_underruns=%u\nair.tx_space_events=%u\n",
		(unsigned)ev_started, (unsigned)t_started, (unsigned)ev_underrun, (unsigned)ev_space);
	dl_text(t, "air.tx_done=%u\nair.tx_done_us=%u\nair.tap_gaps=%u\n", (unsigned)ev_done, (unsigned)t_done,
		(unsigned)tap_gaps);
	if (board.air != NULL) {
		dl_text(t, "air.tx_chunk_syms=%u\nair.tx_queue_depth=%u\n", board.air->tx_chunk_syms,
			board.air->tx_queue_depth);
	}
	if (open_on && board.air != NULL && board.air->rx_load != NULL) {
		uint32_t work, span;

		if (board.air->rx_load(&work, &span)) {
			dl_text(t, "air.rx_work_us=%u\nair.rx_span_us=%u\n", (unsigned)work, (unsigned)span);
		}
	}
	if (open_on && board.air != NULL) {
		dl_text(t, "air.tx_space=%u\nair.now_us=%u\n", board.air->tx_space(), (unsigned)board.air->now_us());
	}
}

size_t debuglink_cmd_air(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len < 1) {
		return status(out, DL_ST_BAD_ARG);
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);
	switch (req[0]) {
	case DL_AIR_OP_STATUS: {
		dl_text_t t;
		out[0] = DL_ST_OK;
		dl_text_init(&t, (char *)out + 1, cap - 1);
		status_text(&t);
		return 1 + t.len;
	}
	case DL_AIR_OP_TAP_START:
		return tap_start(a, n, out);
	case DL_AIR_OP_TAP_READ:
		return tap_read(out, cap);
	case DL_AIR_OP_TAP_STOP:
		return tap_stop(out);
	case DL_AIR_OP_OPEN:
		return air_open(a, n, out);
	case DL_AIR_OP_CLOSE:
		if (open_on) {
			session_end();
			dlog("debuglink: air close");
		}
		return status(out, DL_ST_OK);
	case DL_AIR_OP_RX_READ:
		return rx_read(out, cap);
	case DL_AIR_OP_RX_RESTART:
		if (!open_on) {
			return status(out, DL_ST_UNAVAILABLE);
		}
		board.air->rx_restart();
		return status(out, DL_ST_OK);
	case DL_AIR_OP_TX_DATA:
		return tx_data(a, n, out);
	case DL_AIR_OP_TX_KEY:
		return tx_key(a, n, out);
	case DL_AIR_OP_TX_END:
		if (!open_on) {
			return status(out, DL_ST_UNAVAILABLE);
		}
		board.air->tx_end();
		return status(out, DL_ST_OK);
	case DL_AIR_OP_TX_ABORT:
		if (!open_on) {
			return status(out, DL_ST_UNAVAILABLE);
		}
		board.air->tx_abort();	// TX_DONE follows: the core's tx_off from on_event
		return status(out, DL_ST_OK);
	default:
		return status(out, DL_ST_BAD_ARG);
	}
}

#endif	// CONFIG_AIR_RAW
