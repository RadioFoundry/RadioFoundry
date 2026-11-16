// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000 as an air interface provider and receive tap (see hrc7000_air.h; docs/spec/modem-hrc7000.md 10)
#include <stddef.h>
#include <string.h>
#include "hrc7000_air.h"

#if CONFIG_AIR	// the whole file

#define WORK_MODE_PHY		C7_WORK_MODE_PHY	// timeslot mode: on air back to back with every slot sent (10.2)
#define L2_PHY_TX_MASTER	0x82u	// txen, tx_master_mode: own TX slot timing
#define L2_PASSIVE		0xC0u
#define TXRX_NONE		0x00u
#define TXRX_SEND		0x80u	// txnextsloten
#define TXRX_SYNC_RELEASE	0xC00000u	// b23, b22: release the receive and transmit slot syncs
#define SYS_PHY_RDY		(1u << 12)
#define BW_COPY			0x00u	// FM_BANDWIDTH: band-pass off (b6), 12.5 kHz receive, no pre-emphasis
#define DEV_COEF_NARROW		0x40A25u	// FM_DEV_COEF for 12.5 kHz receive (8.2)
#define RAM_HALF0		0x00u
#define RAM_HALF1		0x24u
#define SYS_LOOPS		4u

#define SLOT_US			30000u
#define SAMPLE_US		125u	// 8 kHz
#define HALF			HRC7000_FM_RX_SAMPLES	// samples per copy half
#define HALF_SYM_US		104u	// half a symbol at 4800 symbols/s: t_us is a symbol's center
#define TX_AIR_DELAY_US		5000u	// a slot set up at a boundary goes on air about 5 ms later (2.10)
#define STALL_US		200000u
#define NO_KEY_US		200000u	// transmitting this long without an RF TX interrupt: dropped

_Static_assert((HRC7000_AIR_RING % HALF) == 0 && HRC7000_AIR_RING / HALF == 2, "the ring is two copy halves");

// The interrupt path runs from RAM on the SoC: flash busy time would add jitter to the 5 ms a slot boundary leaves for
// the next half (modem-hrc7000.md 9 #15). Host builds have no such section
#if defined(__csky__)
#define AIR_ISR		__attribute__((section(".ramfunc"), noinline))
#else
#define AIR_ISR
#endif

static inline void w(hrc7000_air_t *a, uint32_t off, uint32_t v) { hrc7000_wr(a->dev, off, v); }
static inline uint32_t r(hrc7000_air_t *a, uint32_t off) { return hrc7000_rd(a->dev, off); }
static inline uint32_t now(const hrc7000_air_t *a) { return a->ops->now_us(a->ops->ctx); }
static void lock(hrc7000_air_t *a) { if (a->ops->lock != NULL) a->ops->lock(a->ops->ctx); }
static void unlock(hrc7000_air_t *a) { if (a->ops->unlock != NULL) a->ops->unlock(a->ops->ctx); }
static AIR_ISR void notify(hrc7000_air_t *a) { if (a->ops->notify != NULL) a->ops->notify(a->ops->ctx); }

// --- Pure ---

uint32_t hrc7000_air_tx_fit(const air_profile_t *p)
{
	if (p->timing != AIR_TIMING_CONTINUOUS || (p->levels != 2 && p->levels != 4)) {
		return 0;	// the chip keys slot by slot only as the DMR engine; its own slot clock is not the profile's
	}
	uint32_t f = AIR_FIT_TX;
	if (p->sym_rate == 2400u) {
		f |= AIR_FIT_RATE_DOUBLED | AIR_FIT_SHAPE_APPROX;	// the pulse filter is the 4800 one
	} else if (p->sym_rate != 4800u) {
		return 0;
	}
	if (p->shape != AIR_SHAPE_RRC || p->shape_param != 20u) {
		f |= AIR_FIT_SHAPE_APPROX;	// the chip's own DMR pulse: root raised cosine, roll-off 0.2
	}
	// The modulation bytes scale from a byte calibrated near 0x4A at 1944 Hz: 1 to 255 covers about 26 Hz to 6.7 kHz
	if (p->dev_outer_hz < 100u || p->dev_outer_hz > 6000u) {
		f |= AIR_FIT_DEV_APPROX;
	}
	return f;
}

static uint32_t scale_byte(uint32_t b, uint16_t dev)
{
	uint32_t v = (b * dev + HRC7000_AIR_DMR_DEV_HZ / 2u) / HRC7000_AIR_DMR_DEV_HZ;
	return v < 1u ? 1u : v > 255u ? 255u : v;
}

uint32_t hrc7000_air_mod_bias(const air_profile_t *p, uint32_t dmr_mod_bias)
{
	uint32_t mod2 = scale_byte((dmr_mod_bias >> 8) & 0xFFu, p->dev_outer_hz);
	uint32_t mod1 = scale_byte(dmr_mod_bias & 0xFFu, p->dev_outer_hz);
	return (dmr_mod_bias & 0xFFFF0000u) | mod2 << 8 | mod1;
}

// --- Queues (free-running indexes: a power of two) ---

// TX_SPACE events in a row are one (the count is tx_space's), and the last place is kept for TX_DONE: the core's TX
// off waits on it
static AIR_ISR void ev_push(hrc7000_air_t *a, uint8_t kind, uint32_t t)
{
	uint8_t used = (uint8_t)(a->ev_head - a->ev_tail);
	if (kind == AIR_EV_TX_SPACE && used > 0 && a->evq[(uint8_t)(a->ev_head - 1u) % HRC7000_AIR_EVQ].kind == kind) {
		return;
	}
	if (used >= HRC7000_AIR_EVQ - (kind == AIR_EV_TX_DONE ? 0u : 1u)) {
		a->st.ev_overflow++;
		return;
	}
	hrc7000_air_ev_t *e = &a->evq[a->ev_head % HRC7000_AIR_EVQ];
	e->kind = kind;
	e->t_us = t;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->ev_head++;
}

static uint8_t txq_count(const hrc7000_air_t *a)
{
	return (uint8_t)(a->txq_head - a->txq_tail);
}

static bool last_queued(const hrc7000_air_t *a)
{
	return txq_count(a) > 0 && (a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC7000_AIR_TXQ].flags & AIR_TX_LAST) != 0;
}

// --- The receive copy (10.1) ---

// The time of sample i: the halves are whole in the ring, each stamped when its interrupt came
static uint32_t sample_time(const hrc7000_air_t *a, uint32_t i)
{
	return a->t_half[(i / HALF) % 2u] + (i % HALF) * SAMPLE_US;
}

// The chip from DMR to the copy set-up: the FM receive path on the RF chip's DMR output, band-pass and playback off
static AIR_ISR void copy_on(hrc7000_air_t *a)
{
	if (!a->chip_saved) {
		a->saved_mask = r(a, C7_SYS_INTERP_MASK);
		a->saved_bw = r(a, C7_FM_BANDWIDTH);
		a->saved_coef = r(a, C7_FM_DEV_COEF);
		a->chip_saved = true;
	}
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_LAYER2_SLOTON, 0);
	w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(a, C7_FM_PTT, 0);
	// Rule 1 (hal_air.h): the copy goes to the codec DAC too while FM playback is on. Off before the mode changes
	hrc7000_audio_bits(a->dev, 0, C7_AUDIO_FM_PLAY);
	w(a, C7_FM_BANDWIDTH, BW_COPY);
	w(a, C7_FM_DEV_COEF, DEV_COEF_NARROW);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(a, C7_SYS_INTERP_CLEAR, C7_SYS_FM_RX | SYS_PHY_RDY);
	w(a, C7_SYS_INTERP_MASK, C7_SYS_FM_RX | SYS_PHY_RDY);
	a->gap = true;		// the next half does not follow what the ring holds
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->copy = true;
}

// The interrupt mask as it was before the session. Only power-on sets it otherwise, so it goes back whatever mode the
// chip is in now; the FM bits only in DMR (an FM entry writes its own)
static void restore(hrc7000_air_t *a, bool fm_bits)
{
	if (a->chip_saved) {
		w(a, C7_SYS_INTERP_MASK, a->saved_mask);
		if (fm_bits) {
			w(a, C7_FM_BANDWIDTH, a->saved_bw);
			w(a, C7_FM_DEV_COEF, a->saved_coef);
		}
		a->chip_saved = false;
	}
}

// Back to the DMR receive set-up the slot engine starts from (it restarts its receiver after)
static void copy_off(hrc7000_air_t *a)
{
	a->copy = false;
	restore(a, true);
	w(a, C7_SYS_INTERP_CLEAR, C7_SYS_FM_RX | SYS_PHY_RDY);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	w(a, C7_WORK_MODE, C7_WORK_MODE_FM);	// through FM, as every DMR entry: resets the state machine (5.1)
	w(a, C7_WORK_MODE, C7_WORK_MODE_DMR);
	w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
}

// A half is full (system interrupt bit 14): into the ring, or dropped when the task has not made room
static AIR_ISR void take_half(hrc7000_air_t *a, uint32_t t)
{
	uint32_t h = a->head;
	if (h - a->tail > HRC7000_AIR_RING - HALF) {
		a->st.overruns++;
		a->gap = true;		// the samples after the ring's are not the next ones
		return;
	}
	if (hrc7000_fm_rx_audio(a->dev, &a->buf->ring[h % HRC7000_AIR_RING]) == 0) {
		a->st.no_half++;
		return;
	}
	a->t_half[(h / HALF) % 2u] = t - (HALF - 1u) * SAMPLE_US;
	a->gap_half[(h / HALF) % 2u] = a->gap;	// the task's receiver starts over at this half
	a->gap = false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->head = h + HALF;
	a->st.halves++;
}

// --- Transmit (10.2) ---

static inline volatile uint8_t *half_ram(hrc7000_air_t *a, uint8_t h)
{
	return &a->dev->buf[C7_TX_RAM + (h == 0 ? RAM_HALF0 : RAM_HALF1)];
}

// The last byte a boundary held back, written at the next one: the chip reads a half's last byte after the boundary
// that rewrites the half, so written with the rest it went out with the chunk two slots later (10.2)
static AIR_ISR void last_byte_out(hrc7000_air_t *a)
{
	if (a->last_due) {
		half_ram(a, a->last_half)[HRC7000_AIR_HALF_BYTES - 1] = a->last_byte;
		a->last_due = false;
	}
}

// The TX RAM half the next slot reads: the chunk, idle after its last symbol; NULL = all idle. late: the last byte
// waits for the next boundary (last_byte_out)
static AIR_ISR void fill_half(hrc7000_air_t *a, const air_tx_chunk_t *c, bool late)
{
	volatile uint8_t *ram = half_ram(a, a->half);
	uint16_t n = c == NULL ? 0 : c->nsyms > HRC7000_AIR_SYMS ? HRC7000_AIR_SYMS : c->nsyms;
	uint8_t idle = a->p->idle;

	for (uint16_t i = 0; i < HRC7000_AIR_HALF_BYTES; i++) {
		uint8_t b = idle;
		if (4u * i + 4u <= n) {
			b = c->syms[i];
		} else if (4u * i < n) {
			// The chunk ends inside this byte: idle after its last symbol
			uint8_t keep = (uint8_t)(0xFFu << (8u - 2u * (n - 4u * i)));
			b = (uint8_t)((c->syms[i] & keep) | (idle & (uint8_t)~keep));
		}
		if (late && i == HRC7000_AIR_HALF_BYTES - 1) {
			a->last_byte = b;
			a->last_half = a->half;
			a->last_due = true;
		} else {
			ram[i] = b;
		}
	}
	a->half ^= 1u;		// only the half the next slot reads: writing both corrupted the end of the slot on air
}

static AIR_ISR void write_half(hrc7000_air_t *a, const air_tx_chunk_t *c)
{
	fill_half(a, c, true);
	w(a, C7_LAYER2_TXRX_CTRL, TXRX_SEND);
	a->st.tx_slots++;
}

// The transmission ends now: PA off, the queue dropped, the modulation bytes back, receiving (chip: the registers may
// be written)
static AIR_ISR void tx_stop(hrc7000_air_t *a, uint32_t t, bool chip)
{
	if (chip) {
		w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	}
	if (a->pa) {
		a->ops->rf(a->ops->ctx, false);
		a->pa = false;
	}
	a->txq_tail = a->txq_head;
	a->end_req = false;
	a->last_due = false;
	if (chip) {
		if (a->mod_saved) {
			w(a, C7_RF_MOD_BIAS, a->mod_bias);
		}
		if ((a->fit & AIR_FIT_RX) != 0 && !a->suspended) {
			copy_on(a);	// receiving again: the soft modem starts over at the gap
		} else {
			w(a, C7_LAYER2_CONTROL, L2_PASSIVE);
			w(a, C7_WORK_MODE, C7_WORK_MODE_FM);
		}
	}
	a->mod_saved = false;
	a->tx = HRC7000_AIR_RX;
	a->st.tx_done++;
	ev_push(a, AIR_EV_TX_DONE, t);
}

// A slot boundary: set up the slot that goes on air about 5 ms from now
static AIR_ISR void boundary(hrc7000_air_t *a, uint32_t t)
{
	if (a->tick_any && t - a->t_tick < SLOT_US / 2u) {
		return;		// the second interrupt of the same slot
	}
	a->tick_any = true;
	a->t_tick = t;
	last_byte_out(a);	// first thing: its half was set up at the boundary before
	if (a->tx == HRC7000_AIR_START && (!a->pa || a->lead > 0)) {
		// The RF chain is not modulating yet: idle until it is (the queue waits, no underrun)
		if (a->pa) {
			a->lead--;
		}
		write_half(a, NULL);
		return;
	}
	switch (a->tx) {
	case HRC7000_AIR_TAIL:
		tx_stop(a, t, true);	// the idle slot after the last chunk is on air: the PA ramps down in it
		return;
	case HRC7000_AIR_LAST:
		write_half(a, NULL);
		a->tx = HRC7000_AIR_TAIL;
		return;
	default:
		break;
	}
	if (txq_count(a) > 0) {
		air_tx_chunk_t *c = &a->buf->txq[a->txq_tail % HRC7000_AIR_TXQ];
		bool last = (c->flags & AIR_TX_LAST) != 0;
		write_half(a, c);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_tail++;
		a->underrun = false;
		if (a->tx == HRC7000_AIR_START) {
			ev_push(a, AIR_EV_TX_STARTED, t + TX_AIR_DELAY_US + HALF_SYM_US);
		}
		a->tx = last ? HRC7000_AIR_LAST : HRC7000_AIR_RUN;
		ev_push(a, AIR_EV_TX_SPACE, t);
	} else if (a->end_req) {
		write_half(a, NULL);	// the last chunk is on air in the slot now starting
		a->tx = HRC7000_AIR_TAIL;
	} else {
		write_half(a, NULL);
		a->st.tx_underruns++;
		if (!a->underrun) {
			a->underrun = true;
			ev_push(a, AIR_EV_TX_UNDERRUN, t + TX_AIR_DELAY_US);
		}
	}
}

// --- Interrupts ---

AIR_ISR void hrc7000_air_isr_sys(hrc7000_air_t *a)
{
	uint32_t t = now(a), seen = 0;
	bool any = false;

	for (uint32_t n = 0; n < SYS_LOOPS; n++) {
		uint32_t list = r(a, C7_SYS_INTERP_LIST);
		if (list == 0) {
			break;
		}
		w(a, C7_SYS_INTERP_CLEAR, list);	// else the line stays up and nothing later interrupts
		// Each event once per entry: a half takes 32 ms to fill and a slot 30 ms, so a bit set again within one entry
		// is the same event, read before its clear took
		uint32_t fresh = list & ~seen;
		seen |= list;
		if ((fresh & C7_SYS_FM_RX) != 0 && a->copy) {
			take_half(a, t);
			any = true;
		}
		if ((fresh & SYS_PHY_RDY) != 0 && a->tx != HRC7000_AIR_RX) {
			boundary(a, t);
			any = true;
		}
	}
	if (any) {
		notify(a);
	}
}

AIR_ISR void hrc7000_air_isr_timeslot(hrc7000_air_t *a)
{
	w(a, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX);
	if (a->tx != HRC7000_AIR_RX) {
		boundary(a, now(a));
		notify(a);
	}
}

AIR_ISR void hrc7000_air_isr_rftx(hrc7000_air_t *a)
{
	w(a, C7_INTERP_CLEAR, C7_INT_RF_TX);
	// Once per key-up, the RF TX lead before the first slot sent: the board's TX on runs from it, then held to the end
	if (a->tx != HRC7000_AIR_RX && !a->pa) {
		a->ops->rf(a->ops->ctx, true);
		a->ops->pa_arm(a->ops->ctx);
		a->pa = true;
	}
}

// --- Task side ---

void hrc7000_air_init(hrc7000_air_t *a, hrc7000_t *dev, const hrc7000_dmr_ops_t *ops, hrc7000_air_buf_t *buf,
		      air_softmodem_t *sm)
{
	memset(a, 0, sizeof(*a));
	a->dev = dev;
	a->ops = ops;
	a->buf = buf;
	a->sm = sm;
}

bool hrc7000_air_tap_start(hrc7000_air_t *a)
{
	if (a->open || a->tap) {
		return false;
	}
	lock(a);
	a->tap = true;
	a->head = a->tail = 0;
	memset(&a->st, 0, sizeof(a->st));
	copy_on(a);
	unlock(a);
	return true;
}

uint16_t hrc7000_air_tap_read(hrc7000_air_t *a, int16_t *buf, uint16_t max)
{
	if (!a->tap) {
		return 0;
	}
	uint32_t t = a->tail, n = a->head - t;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (n > max) {
		n = max;
	}
	for (uint32_t i = 0; i < n; i++) {
		buf[i] = a->buf->ring[(t + i) % HRC7000_AIR_RING];
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->tail = t + n;
	return (uint16_t)n;
}

uint32_t hrc7000_air_tap_time(const hrc7000_air_t *a)
{
	return sample_time(a, a->tail);
}

void hrc7000_air_tap_stop(hrc7000_air_t *a)
{
	lock(a);
	if (a->tap) {
		if (!a->suspended) {
			copy_off(a);
		} else {
			restore(a, false);
		}
		a->tap = false;
		a->suspended = false;
	}
	unlock(a);
}

// The soft modem's chunks to the session's callback (task level, inside hrc7000_air_poll)
static void sm_out(const air_chunk_t *c, void *ctx)
{
	hrc7000_air_t *a = ctx;
	air_event_cb_t cb = a->cb;

	a->st.chunks++;
	if (cb != NULL) {
		air_event_t ev = { .kind = AIR_EV_RX, .t_us = c->t_us, .arg = 0, .chunk = c };
		cb(&ev, a->cb_ctx);
	}
}

bool hrc7000_air_open(hrc7000_air_t *a, const air_profile_t *p, uint32_t fit, air_event_cb_t cb, void *ctx)
{
	if (fit == 0 || cb == NULL || a->open || a->tap) {
		return false;
	}
	if ((fit & AIR_FIT_RX) != 0
	    && !air_softmodem_rx_open(a->sm, p, HRC7000_AIR_RATE_HZ, 0, sm_out, a)) {
		return false;
	}
	lock(a);
	a->p = p;
	a->cb = cb;
	a->cb_ctx = ctx;
	a->fit = fit;
	a->ev_head = a->ev_tail = 0;
	a->txq_head = a->txq_tail = 0;
	a->tx = HRC7000_AIR_RX;
	a->pa = false;
	a->end_req = false;
	a->mod_saved = false;
	a->suspended = false;
	a->head = a->tail = 0;
	memset(&a->st, 0, sizeof(a->st));
	if ((fit & AIR_FIT_RX) != 0) {
		copy_on(a);
	} else {
		hrc7000_audio_bits(a->dev, 0, C7_AUDIO_FM_PLAY);	// rule 1 without the copy too
	}
	a->open = true;
	unlock(a);
	return true;
}

void hrc7000_air_close(hrc7000_air_t *a, bool chip)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC7000_AIR_RX) {
			bool mod = a->mod_saved;
			tx_stop(a, now(a), false);	// its TX_DONE goes nowhere: the session ends
			if (chip) {
				w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
				if (mod) {
					w(a, C7_RF_MOD_BIAS, a->mod_bias);
				}
			}
		}
		if (chip && !a->suspended) {
			copy_off(a);
		} else {
			a->copy = false;
			restore(a, false);
		}
		a->open = false;
		a->cb = NULL;
		a->ev_tail = a->ev_head;
	}
	unlock(a);
}

void hrc7000_air_rx_restart(hrc7000_air_t *a)
{
	lock(a);
	if (a->open && a->copy) {
		a->tail = a->head;	// what the ring holds is from the old channel
		a->gap = true;
	}
	unlock(a);
}

void hrc7000_air_suspend(hrc7000_air_t *a)
{
	lock(a);
	if (a->open || a->tap) {
		if (a->tx != HRC7000_AIR_RX) {
			tx_stop(a, now(a), false);
		}
		a->copy = false;	// the saved registers stay: they are put back at the end
		a->suspended = true;
	}
	unlock(a);
	notify(a);
}

void hrc7000_air_resume(hrc7000_air_t *a)
{
	lock(a);
	if ((a->open || a->tap) && a->suspended) {
		a->suspended = false;
		if (a->tap || (a->fit & AIR_FIT_RX) != 0) {
			copy_on(a);
		}
	}
	unlock(a);
}

// Samples the interrupt put in the ring, through the soft modem, a copy half at a time (each has its own time stamp
// and gap flag). Head, tail and a restart all move by whole halves, so each pass is one
static void rx_samples(hrc7000_air_t *a)
{
	for (;;) {
		uint32_t h = a->head, t = a->tail;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (t == h || !a->open) {
			return;
		}
		if (a->gap_half[(t / HALF) % 2u]) {
			air_softmodem_rx_reset(a->sm);	// lost samples or a new set-up before this one: timing starts over
		}
		uint32_t t0 = now(a);
		air_softmodem_rx(a->sm, &a->buf->ring[t % HRC7000_AIR_RING], HALF, sample_time(a, t));
		a->st.modem_us += now(a) - t0;
		a->st.modem_span_us += HALF * SAMPLE_US;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->tail = t + HALF;
	}
}

void hrc7000_air_poll(hrc7000_air_t *a)
{
	lock(a);
	if (a->open && a->tx != HRC7000_AIR_RX) {
		uint32_t t = now(a);
		uint32_t last = a->tick_any ? a->t_tick : a->t_begin;
		if ((uint32_t)(t - last) > STALL_US || (!a->pa && (uint32_t)(t - a->t_begin) > NO_KEY_US)) {
			a->st.stalls++;		// no slot clock or no RF TX interrupt: never leave the PA up without them
			tx_stop(a, t, true);
		}
	}
	unlock(a);
	if (a->open && !a->tap && (a->fit & AIR_FIT_RX) != 0) {
		rx_samples(a);
	}
	while (a->open && a->ev_tail != a->ev_head) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		hrc7000_air_ev_t e = a->evq[a->ev_tail % HRC7000_AIR_EVQ];
		a->ev_tail++;
		air_event_t ev = { .kind = e.kind, .t_us = e.t_us, .arg = 0, .chunk = NULL };
		air_event_cb_t cb = a->cb;
		if (cb != NULL) {
			cb(&ev, a->cb_ctx);
		}
	}
}

bool hrc7000_air_tx_begin(hrc7000_air_t *a)
{
	lock(a);
	bool ok = a->open && a->tx == HRC7000_AIR_RX && !a->suspended && (a->fit & AIR_FIT_TX) != 0 && txq_count(a) > 0;

	if (ok) {
		a->copy = false;	// the chip leaves FM: no copy halves while it sends
		// The modulation bytes the board wrote at hal_radio_t.tx() for this channel, scaled to the profile; back after
		a->mod_bias = r(a, C7_RF_MOD_BIAS);
		a->mod_saved = true;
		w(a, C7_RF_MOD_BIAS, hrc7000_air_mod_bias(a->p, a->mod_bias));
		w(a, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		w(a, C7_LAYER2_SLOTON, 0);
		w(a, C7_WORK_MODE, C7_WORK_MODE_FM);	// through FM: resets the state machine
		w(a, C7_WORK_MODE, WORK_MODE_PHY);
		// Idle in both halves until the first chunk's slot, so an early key sends idle. Half 0 first
		a->half = 0;
		fill_half(a, NULL, false);
		fill_half(a, NULL, false);
		a->last_due = false;
		a->lead = HRC7000_AIR_TX_LEAD_SLOTS;
		w(a, C7_LAYER2_CONTROL, L2_PHY_TX_MASTER);	// own timing: the first boundary comes within a slot
		w(a, C7_AUDIO_BUFFER_CLR, 0x03);
		a->t_begin = now(a);
		a->tick_any = false;
		a->underrun = false;
		a->pa = false;
		a->tx = HRC7000_AIR_START;
	}
	unlock(a);
	return ok;
}

bool hrc7000_air_tx_queue(hrc7000_air_t *a, const air_tx_chunk_t *c)
{
	lock(a);
	bool ok = a->open && (a->tx == HRC7000_AIR_RX || a->tx == HRC7000_AIR_START || a->tx == HRC7000_AIR_RUN)
		  && !a->end_req && !last_queued(a) && c->nsyms > 0 && c->nsyms <= HRC7000_AIR_SYMS
		  && txq_count(a) < HRC7000_AIR_TXQ;

	if (ok) {
		a->buf->txq[a->txq_head % HRC7000_AIR_TXQ] = *c;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_head++;
	}
	unlock(a);
	return ok;
}

uint8_t hrc7000_air_tx_space(const hrc7000_air_t *a)
{
	if (!a->open || a->end_req || last_queued(a) || a->tx == HRC7000_AIR_LAST || a->tx == HRC7000_AIR_TAIL) {
		return 0;
	}
	return (uint8_t)(HRC7000_AIR_TXQ - txq_count(a));
}

void hrc7000_air_tx_end(hrc7000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (txq_count(a) > 0) {
			a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC7000_AIR_TXQ].flags |= AIR_TX_LAST;
		} else if (a->tx == HRC7000_AIR_START || a->tx == HRC7000_AIR_RUN) {
			a->end_req = true;
		}
	}
	unlock(a);
}

void hrc7000_air_tx_abort(hrc7000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC7000_AIR_RX) {
			tx_stop(a, now(a), true);
		} else {
			a->txq_tail = a->txq_head;
			a->end_req = false;
		}
	}
	unlock(a);
	notify(a);
}

bool hrc7000_air_tx_active(const hrc7000_air_t *a)
{
	return a->open && a->tx != HRC7000_AIR_RX;
}

#endif	// CONFIG_AIR
