// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C6000's physical layer mode as an air interface provider (see hrc6000_air.h; docs/spec/modem-hrc6000.md 10)
#include <stddef.h>
#include <string.h>
#include "hrc6000_air.h"

#if CONFIG_AIR	// the whole file

// Page 4 registers
#define R_MODE		0x10
#define R_ACCESS0	0x20
#define R_ACCESS1	0x21
#define R_FM_FILT	0x34	// FM audio filters: bit 7 band-pass, 6 expander, 5 de-emphasis, 2 receive low-pass
#define R_FM_AUDIO	0x36	// the FM feed-through to the codec's line out
#define R_L2_CTL	0x40
#define R_SLOT_CTL	0x41
#define R_MOD2		0x45
#define R_MOD1		0x46
#define R_RX_TYPE	0x51
#define R_SYNC_TYPE	0x5F
#define R_FM_TX		0x60
#define R_INT_MASK	0x81
#define R_INT_FLAGS	0x82
#define R_INT_CLEAR	0x83
// Page 1: the correlator's pattern (write only), page 2: the receive record and the two TX RAM halves
#define AUX_PATTERN	0x47
#define RX_RECORD	0x30
#define TX_HALF0	0x00
#define TX_HALF1	0x24

#define MODE_PHY_RX	0x02	// physical layer, continuous receive
#define MODE_PHY_TX	0x66	// physical layer, the DMR transmit set-up otherwise (LayerMode bits 4-3 = 00)
#define L2_PHY_RX	0x40	// RX enabled, the chip's own record clock
#define L2_ACTIVE	0xE3	// TX + RX enabled, own timing
#define L2_PASSIVE	0xC3	// the DMR receive set-up the slot engine starts from
#define CTL_NONE	0x00
#define CTL_SOFT	0x40	// soft records
#define CTL_REARM	0x60	// ... with SyncFail: drop the record timing and search again
#define CTL_TX		0x80	// send in the next slot
#define SYNC_CMP_ALL	0xF0	// every comparator enabled (bits 1-0 read back which one matched)
#define ACCESS_IMPOLITE	0x02
#define INT_PHY		0x01	// 0x82 bit 0: a physical layer record is ready
#define CLASS_SYNC	2	// 0x51 bits 1-0 on a record with a sync detection
#define MODE_FM		0x80
#define FILT_COPY	0x00	// 0x34 in the copy set-up: every receive filter off (bits 4-3 are transmit only)
#define INT_FM_RX	0x02	// 0x82 bit 1 in FM: a receive RAM half is full
#define COPY_RAM	0x030u	// page 2: the two FM receive halves, 512 bytes each, big endian samples
#define COPY_HALF_BYTES	(2u * HRC6000_AIR_COPY_HALF)
#define SAMPLE_US	125u	// 8 kHz
#define HALF		HRC6000_AIR_COPY_HALF
// The copy's sign against the carrier: positive = above it, as hal_baseband.h has it (10.6)
#define COPY_SIGN	1

#define SLOT_US		30000u
#define HALF_SYM_US	104u	// half a symbol at 4800 symbols/s: t_us is a symbol's center
#define TX_AIR_DELAY_US	2200u	// a slot boundary to that slot's first symbol on air (as the BS outbound's)
#define RX_DELAY_US	0u	// on air to the record clock: not measured (HRC6000_AIR_TIME_US covers it)
#define SYS_AFTER_TICK_US 2420u	// a record's interrupt after the timeslot interrupt that ends it
#define TICK_FRESH_US	10000u	// a timeslot interrupt this recent is the record's own
#define GAP_US		45000u	// records 30 ms apart: one later than this lost one
#define REARM_HOLD_US	40000u
#define STALL_US	200000u
#define REARM_FIRST	1u	// spare records past the sync spacing before a re-arm
#define SYNC_CHIP_AT	22u	// a detected sync in the record as the chip stores it
#define TRIM_MIN	2	// symbols: a smaller step of the record clock is within the interrupt's own jitter
#define TRIM_MAX	48
#define FILL_MAX	192	// symbols: a re-arm's hole (about 124 with a sync every 6 records) or a record lost

// The half slot n reads: counted from the own timing write (first timeslot interrupt = 1, so the first slot set up is
// 2 and reads 0x00). The same in every key-up checked off air, wherever in the slot the first interrupt came and
// whatever the transmission before sent (modem-hrc6000.md 10.3). The other half is the one on air: a chunk written
// there goes out with its first 32 bits two slots old
#define HALF0_PARITY	0u

// Profiles the chip takes (modem-hrc6000.md 10.4)
#define CAL_DEV_HZ	1944u	// the DMR outer deviation the calibrated pair gives
#define C4FM_SCALE	946u	// a C4FM receiver reads this much (/1000) of the chip's outer deviation (measured, 10.3)
#define TX_DEV_MIN	1050u	// the pair scales linearly over this range
#define TX_DEV_MAX	2400u
#define RX_DEV_MIN	1700u	// the correlator: syncs at 1800 Hz C4FM, 1944 and 2700 Hz; never at 1350 or 1050
#define RX_DEV_MAX	2900u
#define RX_SPACING_MAX	1728u	// sync spacing checked on air (12 records)
#define SOFT_PER_KHZ	11u	// soft counts per kHz of deviation
#define DC_MAX16	(8 * 16)

static bool w(hrc6000_air_t *a, uint8_t reg, uint8_t val)
{
	return hrc_write(a->dev, HRC_PAGE_SYS, reg, val);
}

static uint32_t now(hrc6000_air_t *a)
{
	return a->ops->now_us(a->ops->ctx);
}

static void lock(hrc6000_air_t *a)
{
	if (a->ops->lock != NULL) {
		a->ops->lock(a->ops->ctx);
	}
}

static void unlock(hrc6000_air_t *a)
{
	if (a->ops->unlock != NULL) {
		a->ops->unlock(a->ops->ctx);
	}
}

static void notify(hrc6000_air_t *a)
{
	if (a->ops->notify != NULL) {
		a->ops->notify(a->ops->ctx);
	}
}

_Static_assert(HRC6000_AIR_RING == 2 * HRC6000_AIR_COPY_HALF, "the ring is two copy halves");

// --- Pure: fit, deviation, record layout ---

static bool outer_only(const air_sync_t *s)
{
	for (uint8_t i = 0; i < s->nsyms; i++) {
		uint8_t d = (uint8_t)((s->syms[i / 4] >> (6 - 2 * (i % 4))) & 3u);
		if ((d & 1u) == 0) {
			return false;	// an inner symbol: the correlator's match falls off with each (10.2)
		}
	}
	return true;
}

// The pattern for the correlator: the first 24-symbol one on air all through a transmission, -1 none
static int rx_sync(const air_profile_t *p)
{
	for (uint8_t i = 0; i < p->nsyncs && i < AIR_SYNCS_MAX; i++) {
		const air_sync_t *s = &p->sync[i];
		if (s->nsyms == AIR_SYNC_SYMS_MAX && (s->flags & AIR_SYNC_START_ONLY) == 0 && outer_only(s)) {
			return i;
		}
	}
	return -1;
}

static uint32_t tx_target_hz(const air_profile_t *p)
{
	uint32_t d = p->dev_outer_hz;
	return p->shape == AIR_SHAPE_C4FM ? (d * 1000u + C4FM_SCALE / 2) / C4FM_SCALE : d;
}

uint32_t hrc6000_air_fit(const air_profile_t *p)
{
	if (p == NULL || p->timing != AIR_TIMING_CONTINUOUS || (p->levels != 2 && p->levels != 4)
	    || p->dev_outer_hz == 0 || p->nsyncs > AIR_SYNCS_MAX) {
		return 0;
	}
	uint32_t f;
	if (p->sym_rate == 4800) {
		f = AIR_FIT_TX;
		if (rx_sync(p) >= 0 && p->sync_every_syms > 0 && p->sync_every_syms <= RX_SPACING_MAX
		    && p->dev_outer_hz >= RX_DEV_MIN && p->dev_outer_hz <= RX_DEV_MAX) {
			f |= AIR_FIT_RX | AIR_FIT_SOFT | AIR_FIT_NEEDS_SYNC;
		}
	} else if (p->sym_rate == 2400) {
		f = AIR_FIT_TX | AIR_FIT_RATE_DOUBLED;	// receive at half rate never synced reliably
	} else {
		return 0;
	}
	if (p->shape != AIR_SHAPE_RRC || p->shape_param != 20) {
		f |= AIR_FIT_SHAPE_APPROX;	// the chip's own pulse is the DMR one
	}
	uint32_t t = tx_target_hz(p);
	if (t * 10u < TX_DEV_MIN * 9u || t * 10u > TX_DEV_MAX * 11u) {
		f |= AIR_FIT_DEV_APPROX;	// outside the pair's checked range
	}
	return f;
}

// What the chip itself does with p: a chip without physical layer receive keeps only transmit
static uint32_t chip_fit(const air_profile_t *p, const hrc_chip_t *chip)
{
	uint32_t f = hrc6000_air_fit(p);

	if (chip != NULL && chip->air_tx_only) {
		f &= ~(uint32_t)(AIR_FIT_RX | AIR_FIT_SOFT | AIR_FIT_NEEDS_SYNC);
	}
	return f;
}

uint32_t hrc6000_air_fit_tap(const air_profile_t *p, const hrc_chip_t *chip, const hal_baseband_t *tap)
{
	uint32_t f = chip_fit(p, chip);

	// The copy receives what the correlator can't; transmit stays the chip's, so a profile it can't send gets nothing
	if (f != 0 && (f & AIR_FIT_RX) == 0 && tap != NULL) {
		f |= air_softmodem_rx_fit(p, tap);
	}
	return f;
}

static uint8_t scale_mod(uint8_t cal, uint32_t target)
{
	uint32_t v = ((uint32_t)cal * target + CAL_DEV_HZ / 2) / CAL_DEV_HZ;
	return (uint8_t)(v < 1u ? 1u : v > 255u ? 255u : v);
}

void hrc6000_air_mod_pair(const air_profile_t *p, uint8_t cal2, uint8_t cal1, uint8_t *mod2, uint8_t *mod1)
{
	// Mod1 moves the deviation linearly through zero; Mod2 must keep its ratio to it (10.3)
	uint32_t t = tx_target_hz(p);
	*mod2 = scale_mod(cal2, t);
	*mod1 = scale_mod(cal1, t);
}

// A record in chip order to air order: (first symbol, count) runs. With a sync detection the 24-symbol pattern lands
// at HRC6000_AIR_SYNC_AT
static const uint8_t LAYOUT_SYNC[][2] = { { 0, 12 }, { 46, 49 }, { 12, 5 }, { 22, 24 }, { 17, 5 }, { 95, 49 } };
static const uint8_t LAYOUT_PLAIN[][2] = { { 0, 12 }, { 36, 54 }, { 12, 24 }, { 90, 54 } };

static int8_t clamp8(int32_t v)
{
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

static uint8_t slice(int8_t s, uint8_t levels)
{
	if (levels == 2) {
		return s >= 0 ? 1u : 3u;
	}
	int32_t t = (AIR_SOFT_OUTER * 2) / 3;	// halfway between the inner and outer levels
	return s >= t ? 1u : s >= 0 ? 0u : s >= -t ? 2u : 3u;
}

int16_t hrc6000_air_unpack(const uint8_t *raw, bool synced, uint8_t skip, int16_t dc16, int16_t outer16,
			   uint8_t levels, uint8_t *hard, int8_t *soft)
{
	const uint8_t (*seg)[2] = synced ? LAYOUT_SYNC : LAYOUT_PLAIN;
	uint8_t nseg = synced ? (uint8_t)(sizeof(LAYOUT_SYNC) / sizeof(LAYOUT_SYNC[0]))
			      : (uint8_t)(sizeof(LAYOUT_PLAIN) / sizeof(LAYOUT_PLAIN[0]));
	int32_t sum = 0;
	uint16_t j = 0;

	if (outer16 < 16) {
		outer16 = 16;
	}
	memset(hard, 0, HRC6000_AIR_SYMS / 4);
	for (uint8_t k = 0; k < nseg; k++) {
		for (uint8_t i = seg[k][0]; i < seg[k][0] + seg[k][1]; i++) {
			// The level is the first byte's 6-bit two's complement value; the second byte only repeats the slice
			int32_t lv = raw[2 * i] & 0x3Fu;
			lv -= (lv & 0x20) != 0 ? 64 : 0;
			sum += lv;
			if (j >= skip) {
				uint16_t o = (uint16_t)(j - skip);
				int8_t s = clamp8((lv * 16 - dc16) * AIR_SOFT_OUTER / outer16);
				soft[o] = s;
				hard[o / 4] |= (uint8_t)(slice(s, levels) << (6 - 2 * (o % 4)));
			}
			j++;
		}
	}
	return (int16_t)(sum * 16 / HRC6000_AIR_SYMS);
}

int16_t hrc6000_air_sync_outer16(const uint8_t *raw, const uint8_t *syms)
{
	int32_t pos = 0, neg = 0;
	int32_t npos = 0;

	for (uint8_t i = 0; i < AIR_SYNC_SYMS_MAX; i++) {
		int32_t lv = raw[2 * (SYNC_CHIP_AT + i)] & 0x3Fu;
		lv -= (lv & 0x20) != 0 ? 64 : 0;
		if (((syms[i / 4] >> (6 - 2 * (i % 4))) & 3u) == 1u) {
			pos += lv;
			npos++;
		} else {
			neg += lv;
		}
	}
	if (npos == 0 || npos == AIR_SYNC_SYMS_MAX) {
		return 0;
	}
	return (int16_t)((pos * 16 / npos - neg * 16 / (AIR_SYNC_SYMS_MAX - npos)) / 2);
}

static const uint8_t ERASED_HARD[HRC6000_AIR_SYMS / 4];
static const int8_t ERASED_SOFT[HRC6000_AIR_SYMS];

// --- Queues (free-running indexes: a power of two) ---

// TX_SPACE events in a row are one (the count is tx_space's), and the last place is kept for TX_DONE: the core's TX
// off waits on it
static void ev_push(hrc6000_air_t *a, uint8_t kind, uint32_t t, uint8_t arg)
{
	uint8_t used = (uint8_t)(a->ev_head - a->ev_tail);

	if (kind == AIR_EV_TX_SPACE && used > 0 && a->evq[(uint8_t)(a->ev_head - 1u) % HRC6000_AIR_EVQ].kind == kind) {
		return;
	}
	if (used >= HRC6000_AIR_EVQ - (kind == AIR_EV_TX_DONE ? 0u : 1u)) {
		a->st.ev_overflow++;
		return;
	}
	hrc6000_air_ev_t *e = &a->evq[a->ev_head % HRC6000_AIR_EVQ];
	e->kind = kind;
	e->t_us = t;
	e->arg = arg;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->ev_head++;
}

static uint8_t txq_count(const hrc6000_air_t *a)
{
	return (uint8_t)(a->txq_head - a->txq_tail);
}

// The newest chunk queued ends the transmission: nothing more is taken
static bool last_queued(const hrc6000_air_t *a)
{
	return txq_count(a) > 0 && (a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC6000_AIR_TXQ].flags & AIR_TX_LAST) != 0;
}

// --- Chip set-ups ---

static void write_pattern(hrc6000_air_t *a, const uint8_t *syms)
{
	(void)hrc_write_burst(a->dev, HRC_PAGE_AUX, AUX_PATTERN, syms, AIR_SYNC_SYMS_MAX / 4);
}

// --- The FM copy (10.6) ---

// The time of ring sample i: the halves are whole in the ring, each stamped when its interrupt came
static uint32_t sample_time(const hrc6000_air_t *a, uint32_t i)
{
	return a->t_half[(i / HALF) % 2u] + (i % HALF) * SAMPLE_US;
}

// The chip from DMR (or the physical layer) to the copy set-up: FM receive with every audio filter off, nothing to
// the line out, the FM receive RAM's interrupt on
static void copy_on(hrc6000_air_t *a)
{
	if (!a->copy_saved) {
		uint8_t f = 0, m = 0;
		a->copy_saved = hrc_read(a->dev, HRC_PAGE_SYS, R_FM_FILT, &f) && hrc_read(a->dev, HRC_PAGE_SYS, R_INT_MASK, &m);
		a->saved_34 = f;
		a->saved_81 = m;
	}
	w(a, R_SLOT_CTL, CTL_NONE);
	w(a, R_FM_AUDIO, 0x00);		// rule 1 (hal_air.h) before the mode changes: FM receive never to the line out
	w(a, R_FM_TX, 0x00);
	w(a, R_FM_FILT, FILT_COPY);
	w(a, R_MODE, MODE_FM);
	w(a, R_INT_MASK, (uint8_t)((a->copy_saved ? a->saved_81 : 0x19u) | INT_FM_RX));
	w(a, R_INT_CLEAR, 0xFF);
	a->last_any = false;	// which half fills next is found again (take_half)
	a->cgap = true;
	a->rx_paused = false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->copy = true;
}

// The registers copy_on changed back (chip: the chip is still in DMR, else the board's mode entry wrote its own)
static void copy_restore(hrc6000_air_t *a, bool chip)
{
	a->copy = false;
	if (chip && a->copy_saved) {
		w(a, R_INT_MASK, a->saved_81);
		w(a, R_FM_FILT, a->saved_34);
		w(a, R_INT_CLEAR, 0xFF);
	}
	a->copy_saved = false;
}

// Back to the DMR receive set-up; the board restarts the slot engine (5.1) after
static void dmr_setup(hrc6000_air_t *a)
{
	w(a, R_SLOT_CTL, CTL_NONE);
	w(a, R_MODE, a->dev->chip->dmr_mode(false));
	write_pattern(a, hrc_ms_data_sync);
	w(a, R_L2_CTL, L2_PASSIVE);
}

// A RAM half is full (0x82 bit 1): into the ring, or dropped when the task has not made room. Which half: the one
// whose last sample changed since the last interrupt (the other one's last sample is 32 ms older than its first);
// when both or neither did (a steady input), the other half than the last one
static void take_half(hrc6000_air_t *a, uint32_t t)
{
	uint16_t last[2];

	for (uint8_t i = 0; i < 2; i++) {
		uint8_t b[2] = { 0, 0 };
		(void)hrc_ram(a->dev, HRC_PAGE_DATA, (uint16_t)(COPY_RAM + i * COPY_HALF_BYTES + COPY_HALF_BYTES - 2u), b, 2,
			      false);
		last[i] = (uint16_t)(b[0] << 8 | b[1]);
	}
	bool first = !a->last_any;
	uint8_t h = a->ram_half;
	if (!first && (last[0] != a->last_word[0]) != (last[1] != a->last_word[1])) {
		uint8_t seen = last[1] != a->last_word[1] ? 1u : 0u;
		if (seen != h) {
			a->st.half_fixes++;
			h = seen;
		}
	}
	a->last_word[0] = last[0];
	a->last_word[1] = last[1];
	a->last_any = true;
	a->ram_half = (uint8_t)(h ^ 1u);
	if (first) {
		return;		// the half is not known yet: the next one is
	}
	uint32_t hd = a->rhead;
	if (hd - a->rtail > HRC6000_AIR_RING - HALF) {
		a->st.overruns++;
		a->cgap = true;		// the samples after the ring's are not the next ones
		return;
	}
	int16_t *dst = &a->buf->ring[hd % HRC6000_AIR_RING];
	(void)hrc_ram(a->dev, HRC_PAGE_DATA, (uint16_t)(COPY_RAM + h * COPY_HALF_BYTES), (uint8_t *)(void *)dst,
		      COPY_HALF_BYTES, false);
	for (uint16_t i = 0; i < HALF; i++) {
		const uint8_t *b = (const uint8_t *)(const void *)&dst[i];
		dst[i] = (int16_t)(COPY_SIGN * (int16_t)(uint16_t)(b[0] << 8 | b[1]));
	}
	a->t_half[(hd / HALF) % 2u] = t - (HALF - 1u) * SAMPLE_US;
	a->gap_half[(hd / HALF) % 2u] = a->cgap;
	a->cgap = false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->rhead = hd + HALF;
	a->st.halves++;
}

// Receive (10.2). The timing starts again: the next chunk is a gap, FREE_RUN until a sync
static void rx_setup(hrc6000_air_t *a)
{
	if (a->rx_copy) {
		copy_on(a);	// receive through the FM copy (10.6); the soft modem starts over at its gap
		return;
	}
	w(a, R_SLOT_CTL, CTL_NONE);
	w(a, R_MODE, MODE_PHY_RX);
	if (a->rx_on) {
		write_pattern(a, a->p->sync[a->sync_id].syms);
	}
	if (a->dev->chip->sync_reg) {
		w(a, R_SYNC_TYPE, SYNC_CMP_ALL);	// a chip without 0x5F runs its comparators as reset left them
	}
	w(a, R_L2_CTL, L2_PHY_RX);
	w(a, R_SLOT_CTL, CTL_SOFT);
	a->unsynced = 0;
	a->since_sync = (uint8_t)(a->sync_per + 1u);
	a->rearm_hold = false;
	a->rec_any = false;
	a->next_any = false;
	a->gap = true;
	a->rx_paused = false;
}

static void idle_half(const hrc6000_air_t *a, uint8_t *half)
{
	memset(half, a->p->idle, HRC6000_AIR_HALF_BYTES);
}

// One TX RAM half: the chunk, idle after its last symbol; NULL = all idle
static void write_half(hrc6000_air_t *a, uint8_t addr, const air_tx_chunk_t *c)
{
	uint8_t half[HRC6000_AIR_HALF_BYTES];

	idle_half(a, half);
	if (c != NULL) {
		uint16_t n = c->nsyms > HRC6000_AIR_SYMS ? HRC6000_AIR_SYMS : c->nsyms;
		memcpy(half, c->syms, n / 4u);
		if ((n % 4u) != 0) {
			uint8_t keep = (uint8_t)(0xFFu << (8u - 2u * (n % 4u)));
			half[n / 4u] = (uint8_t)((c->syms[n / 4u] & keep) | (a->p->idle & (uint8_t)~keep));
		}
	}
	(void)hrc_write_burst(a->dev, HRC_PAGE_DATA, addr, half, sizeof(half));
	a->st.tx_slots++;
}

// The transmission ends now: PA off, the queue dropped, the pair back, receiving (chip: the registers may be written)
static void tx_stop(hrc6000_air_t *a, uint32_t t, bool chip)
{
	a->last_out = true;
	if (a->pa) {
		if (!a->ops->rf(a->ops->ctx, false)) {
			a->st.rf_fail++;
		}
		a->pa = false;
	}
	a->last_out = false;
	a->txq_tail = a->txq_head;
	a->end_req = false;
	if (chip) {
		if (a->mod_saved) {
			(void)hrc_set_mod_gain(a->dev, a->mod2, a->mod1);
		}
		rx_setup(a);
	}
	a->mod_saved = false;
	a->tx = HRC6000_AIR_RX;
	a->gap = true;
	a->st.tx_done++;
	ev_push(a, AIR_EV_TX_DONE, t, 0);
}

static void key(hrc6000_air_t *a)
{
	if (a->ops->rf(a->ops->ctx, true)) {
		a->pa = true;
	} else {
		a->st.rf_fail++;	// tried again at the next slot
	}
}

// --- Interrupts ---

// Slot n starts (own timing): set up slot n + 1 in the half it reads
static void tx_slot(hrc6000_air_t *a, uint32_t t)
{
	a->tx_n++;
	uint32_t slot = a->tx_n + 1u;
	uint8_t addr = (slot & 1u) == HALF0_PARITY ? TX_HALF0 : TX_HALF1;

	if (a->tx == HRC6000_AIR_TAIL) {
		if (a->tail >= 1) {
			// The last chunk left the air 2.2 ms into the slot that started at the previous interrupt
			tx_stop(a, t, true);
			return;
		}
		write_half(a, addr, NULL);
		w(a, R_SLOT_CTL, CTL_TX);
		a->tail++;
		return;
	}
	if (a->tx == HRC6000_AIR_LAST) {
		write_half(a, addr, NULL);	// one idle slot while the last chunk is on air
		w(a, R_SLOT_CTL, CTL_TX);
		a->tx = HRC6000_AIR_TAIL;
		a->tail = 0;
		return;
	}
	if (a->tx == HRC6000_AIR_START && a->tx_n == 1u) {
		// The modulator runs only in a transmit slot. Slot 2 sends idle and the PA is keyed as it starts; the first
		// chunk follows in slot 3. Keyed in slot 1, the carrier sat about 3 kHz off the channel, unmodulated, and
		// stepped onto it under the first chunk's symbols
		write_half(a, addr, NULL);
		w(a, R_SLOT_CTL, CTL_TX);
		return;
	}
	if (txq_count(a) > 0) {
		air_tx_chunk_t *c = &a->buf->txq[a->txq_tail % HRC6000_AIR_TXQ];
		bool last = (c->flags & AIR_TX_LAST) != 0;
		write_half(a, addr, c);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_tail++;
		a->underrun = false;
		if (a->tx == HRC6000_AIR_START) {
			ev_push(a, AIR_EV_TX_STARTED, t + SLOT_US + TX_AIR_DELAY_US + HALF_SYM_US, 0);
		}
		a->tx = last ? HRC6000_AIR_LAST : HRC6000_AIR_RUN;
		// The queue has room again: without this the core refills only after the queue ran dry
		ev_push(a, AIR_EV_TX_SPACE, t, 0);
	} else if (a->end_req) {
		write_half(a, addr, NULL);	// the last chunk went out in the slot now starting
		a->tx = HRC6000_AIR_TAIL;
		a->tail = 0;
	} else {
		write_half(a, addr, NULL);
		a->st.tx_underruns++;
		if (!a->underrun) {
			a->underrun = true;
			ev_push(a, AIR_EV_TX_UNDERRUN, t + SLOT_US + TX_AIR_DELAY_US, 0);
		}
	}
	w(a, R_SLOT_CTL, CTL_TX);
	if (!a->pa) {
		key(a);		// the idle slot starts (or a key the board could not do): the first chunk is a slot away
	}
}

void hrc6000_air_isr_timeslot(hrc6000_air_t *a)
{
	uint32_t t = now(a);

	a->t_tick = t;
	a->tick_any = true;
	if (!a->open || a->tx == HRC6000_AIR_RX) {
		return;
	}
	tx_slot(a, t);
	notify(a);
}

static void rx_record(hrc6000_air_t *a, uint32_t t)
{
	uint8_t r51 = 0;

	(void)hrc_read(a->dev, HRC_PAGE_SYS, R_RX_TYPE, &r51);
	bool synced = (r51 & 3u) == CLASS_SYNC;
	(void)hrc_ram(a->dev, HRC_PAGE_DATA, RX_RECORD, a->buf->raw, HRC6000_AIR_RAW_BYTES, false);
	a->st.records++;
	// The record is the 144 symbols before the boundary its interrupt follows
	uint32_t base = a->tick_any && (uint32_t)(t - a->t_tick) < TICK_FRESH_US ? a->t_tick : t - SYS_AFTER_TICK_US;
	uint32_t t0 = base - SLOT_US + HALF_SYM_US - RX_DELAY_US;
	if (a->rec_any && (uint32_t)(t - a->t_rec) > GAP_US) {
		a->gap = true;
	}
	a->rec_any = true;
	a->t_rec = t;
	// The chip moves its record clock by whole symbols at a sync (10.2), and a record is always the 144 symbols
	// before its boundary: one that starts before the last one ended repeats that one's end, a later one lost some
	uint8_t skip = 0, fill = 0;
	if (a->next_any) {
		int32_t over = (int32_t)(a->t_next - t0);
		int32_t n = over > 100000 ? 1000 : over < -100000 ? -1000
			    : (over * 3 + (over >= 0 ? 312 : -312)) / 625;	// in symbols of 208.33 us

		if (n <= -TRIM_MIN && n >= -FILL_MAX) {
			// Lost, but how many is known (the clock step at a sync, a re-arm's hole, one record missed):
			// erasures in their place keep a framer's count, so it loses those symbols and not its frame
			fill = (uint8_t)-n;
			a->gap = false;
			a->st.fills++;
		} else if (a->gap) {
			// already a hole of unknown size
		} else if (n >= TRIM_MIN && n <= TRIM_MAX) {
			skip = (uint8_t)n;
			a->st.trims++;
		} else if (n >= TRIM_MIN || n <= -TRIM_MIN) {
			a->gap = true;
		}
	}
	a->next_any = true;
	a->t_next = t0 + SLOT_US;
	if (synced) {
		// The level scale from the sync's own symbols: the counts per kHz are the board's receiver's, and
		// the sender's deviation is not exact either. At once on a new signal, slowly while in step
		int16_t o = hrc6000_air_sync_outer16(a->buf->raw, a->p->sync[a->sync_id].syms);

		if (o >= a->outer_nom16 / 2 && o <= a->outer_nom16 * 2) {
			a->outer16 = (int16_t)(a->since_sync > a->sync_per ? o : a->outer16 + (o - a->outer16) / 4);
		}
		a->st.synced++;
		a->unsynced = 0;
		a->since_sync = 0;
	} else {
		a->unsynced++;
		if (a->since_sync < 255u) {
			a->since_sync++;
		}
	}
	bool in_step = a->since_sync <= a->sync_per;
	if ((uint8_t)(a->rec_head - a->rec_tail) >= HRC6000_AIR_RXQ) {
		a->st.rx_overflow++;
		a->gap = true;
	} else {
		uint8_t i = (uint8_t)(a->rec_head % HRC6000_AIR_RXQ);
		hrc6000_air_rec_t *r = &a->buf->rec[i];
		int16_t mean = hrc6000_air_unpack(a->buf->raw, synced, skip, a->dc16, a->outer16, a->p->levels,
						  r->hard, r->soft);
		r->t_us = t0 + (uint32_t)skip * 625u / 3u;
		r->nsyms = (uint8_t)(HRC6000_AIR_SYMS - skip);
		r->fill = fill;
		r->sync_at = synced ? (int16_t)(HRC6000_AIR_SYNC_AT - skip) : -1;
		r->flags = (uint8_t)((a->gap ? AIR_CHUNK_GAP : 0) | (in_step ? 0 : AIR_CHUNK_FREE_RUN));
		a->st.gaps += a->gap;
		a->gap = false;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->rec_head++;
		ev_push(a, AIR_EV_RX, r->t_us, i);
		if (in_step) {
			// The receive offset (a carrier off frequency) from the records of a signal, slowly
			int32_t dc = a->dc16 + (mean - a->dc16) / 16;
			a->dc16 = (int16_t)(dc > DC_MAX16 ? DC_MAX16 : dc < -DC_MAX16 ? -DC_MAX16 : dc);
		}
	}
	// After a timing step the chip never finds the sync again by itself
	if (!synced && a->unsynced >= a->rearm_k) {
		w(a, R_SLOT_CTL, CTL_REARM);
		a->rearm_hold = true;
		a->t_rearm = t;
		a->unsynced = 0;
		a->since_sync = (uint8_t)(a->sync_per + 1u);
		a->gap = true;
		a->st.rearms++;
	}
}

void hrc6000_air_isr_sys(hrc6000_air_t *a)
{
	uint32_t t = now(a);
	uint8_t flags = 0;

	(void)hrc_read(a->dev, HRC_PAGE_SYS, R_INT_FLAGS, &flags);
	if (flags != 0) {
		w(a, R_INT_CLEAR, flags);	// else the line stays low and no later record interrupts
	}
	if (a->copy) {
		if ((flags & INT_FM_RX) != 0) {
			take_half(a, t);
			notify(a);
		}
		return;
	}
	if (!a->open || !a->rx_on || a->rx_paused || a->rearm_hold || a->tx != HRC6000_AIR_RX
	    || (flags & INT_PHY) == 0) {
		return;
	}
	rx_record(a, t);
	notify(a);
}

void hrc6000_air_isr_rftx(hrc6000_air_t *a)
{
	// Once per key-up, anywhere in the slot. No key before the idle slot starts (tx_slot): this only tries again a
	// key the board could not do there
	if (a->open && a->tx != HRC6000_AIR_RX && !a->pa && a->tx_n >= 2u) {
		key(a);
	}
}

// --- Task side ---

void hrc6000_air_init(hrc6000_air_t *a, hrc_t *dev, const hrc6000_dmr_ops_t *ops, hrc6000_air_buf_t *buf,
		      air_softmodem_t *sm, const hal_baseband_t *tap)
{
	memset(a, 0, sizeof(*a));
	a->dev = dev;
	a->ops = ops;
	a->buf = buf;
	a->sm = sm;
	a->tap_desc = sm != NULL ? tap : NULL;
}

bool hrc6000_air_tap_start(hrc6000_air_t *a)
{
	if (a->open || a->tap || a->tap_desc == NULL) {
		return false;
	}
	lock(a);
	a->tap = true;
	a->rhead = a->rtail = 0;
	a->copy_saved = false;
	memset(&a->st, 0, sizeof(a->st));
	copy_on(a);
	unlock(a);
	return true;
}

uint16_t hrc6000_air_tap_read(hrc6000_air_t *a, int16_t *buf, uint16_t max)
{
	if (!a->tap) {
		return 0;
	}
	uint32_t t = a->rtail, n = a->rhead - t;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (n > max) {
		n = max;
	}
	for (uint32_t i = 0; i < n; i++) {
		buf[i] = a->buf->ring[(t + i) % HRC6000_AIR_RING];
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	a->rtail = t + n;
	return (uint16_t)n;
}

uint32_t hrc6000_air_tap_time(const hrc6000_air_t *a)
{
	return sample_time(a, a->rtail);
}

void hrc6000_air_tap_stop(hrc6000_air_t *a, bool chip)
{
	lock(a);
	if (a->tap) {
		copy_restore(a, chip);
		if (chip) {
			dmr_setup(a);
		}
		a->tap = false;
	}
	unlock(a);
}

// The soft modem's chunks to the session's callback (task level, inside hrc6000_air_poll)
static void sm_out(const air_chunk_t *c, void *ctx)
{
	hrc6000_air_t *a = ctx;
	air_event_cb_t cb = a->cb;

	a->st.chunks++;
	if (cb != NULL) {
		air_event_t ev = { .kind = AIR_EV_RX, .t_us = c->t_us, .arg = 0, .chunk = c };
		cb(&ev, a->cb_ctx);
	}
}

// Copy samples the interrupt put in the ring, through the soft modem a half at a time (each has its own time stamp
// and gap flag)
static void rx_samples(hrc6000_air_t *a)
{
	for (;;) {
		uint32_t h = a->rhead, t = a->rtail;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (t == h || !a->open) {
			return;
		}
		if (a->gap_half[(t / HALF) % 2u]) {
			air_softmodem_rx_reset(a->sm);	// lost samples or a new set-up before this half: timing starts over
		}
		uint32_t t0 = now(a);
		air_softmodem_rx(a->sm, &a->buf->ring[t % HRC6000_AIR_RING], HALF, sample_time(a, t));
		a->st.modem_us += now(a) - t0;
		a->st.modem_span_us += HALF * SAMPLE_US;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->rtail = t + HALF;
	}
}

bool hrc6000_air_open(hrc6000_air_t *a, const air_profile_t *p, air_event_cb_t cb, void *ctx)
{
	uint32_t f = hrc6000_air_fit_tap(p, a->dev->chip, a->tap_desc);

	if (f == 0 || cb == NULL || a->open || a->tap) {
		return false;
	}
	bool copy = (chip_fit(p, a->dev->chip) & AIR_FIT_RX) == 0 && (f & AIR_FIT_RX) != 0;
	if (copy && !air_softmodem_rx_open(a->sm, p, HRC6000_AIR_COPY_HZ, a->tap_desc->rx_counts_per_khz, sm_out, a)) {
		return false;
	}
	lock(a);
	a->rx_copy = copy;
	a->rhead = a->rtail = 0;
	a->copy_saved = false;
	a->p = p;
	a->cb = cb;
	a->cb_ctx = ctx;
	a->fit = f;
	a->rx_on = (f & AIR_FIT_RX) != 0 && !copy;
	int s = rx_sync(p);
	a->sync_id = (uint8_t)(s < 0 ? 0 : s);
	uint32_t per = (p->sync_every_syms + HRC6000_AIR_SYMS - 1u) / HRC6000_AIR_SYMS;
	a->sync_per = (uint8_t)(per < 1u ? 1u : per > 200u ? 200u : per);
	a->rearm_k = (uint8_t)(a->sync_per + REARM_FIRST);
	a->outer_nom16 = (int16_t)((uint32_t)p->dev_outer_hz * SOFT_PER_KHZ * 16u / 1000u);
	a->outer16 = a->outer_nom16;
	a->dc16 = 0;
	a->rec_head = a->rec_tail = 0;
	a->ev_head = a->ev_tail = 0;
	a->txq_head = a->txq_tail = 0;
	a->tx = HRC6000_AIR_RX;
	a->pa = false;
	a->end_req = false;
	a->mod_saved = false;
	a->tick_any = false;
	memset(&a->st, 0, sizeof(a->st));
	w(a, R_FM_AUDIO, 0x00);	// nothing of the receiver to the line out: the speaker carries only hal_audio_t
	rx_setup(a);
	a->open = true;
	unlock(a);
	return true;
}

void hrc6000_air_close(hrc6000_air_t *a, bool chip)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC6000_AIR_RX) {
			tx_stop(a, now(a), chip);	// its TX_DONE goes nowhere: the session ends
		}
		copy_restore(a, chip);
		if (chip) {
			dmr_setup(a);
		}
		a->open = false;
		a->cb = NULL;
		a->ev_tail = a->ev_head;
		a->rec_tail = a->rec_head;
	}
	unlock(a);
}

void hrc6000_air_rx_restart(hrc6000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (a->tx == HRC6000_AIR_RX) {
			rx_setup(a);
		} else {
			a->gap = true;	// receive starts again at the end of the transmission (tx_stop)
		}
	}
	unlock(a);
}

void hrc6000_air_suspend(hrc6000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC6000_AIR_RX) {
			tx_stop(a, now(a), false);
		}
		a->copy = false;	// the board's mode entry rewrote what copy_on changed; rx_restart sets it up again
		a->copy_saved = false;
		a->rx_paused = true;
	}
	unlock(a);
	notify(a);
}

void hrc6000_air_poll(hrc6000_air_t *a)
{
	lock(a);
	if (a->open) {
		uint32_t t = now(a);
		if (a->rearm_hold && (uint32_t)(t - a->t_rearm) >= REARM_HOLD_US) {
			w(a, R_SLOT_CTL, CTL_SOFT);
			a->rearm_hold = false;
		}
		uint32_t last = a->tx_n > 0 ? a->t_tick : a->t_begin;
		if (a->tx != HRC6000_AIR_RX && (uint32_t)(t - last) > STALL_US) {
			a->st.stalls++;		// no slot clock: never leave the PA up without it
			tx_stop(a, t, true);
		}
	}
	unlock(a);
	if (a->open && a->rx_copy && !a->rx_paused && a->tx == HRC6000_AIR_RX) {
		rx_samples(a);
	}
	while (a->open && a->ev_tail != a->ev_head) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		hrc6000_air_ev_t e = a->evq[a->ev_tail % HRC6000_AIR_EVQ];
		a->ev_tail++;
		air_event_t ev = { .kind = e.kind, .t_us = e.t_us, .arg = 0, .chunk = NULL };
		air_chunk_t c;
		if (e.kind == AIR_EV_RX) {
			const hrc6000_air_rec_t *r = &a->buf->rec[e.arg % HRC6000_AIR_RXQ];
			// The symbols lost before the record, as erasures (soft 0)
			for (uint16_t left = r->fill; left > 0 && a->cb != NULL;) {
				uint16_t n = left > HRC6000_AIR_SYMS ? HRC6000_AIR_SYMS : left;
				air_chunk_t f = { .t_us = r->t_us - (uint32_t)left * 625u / 3u, .nsyms = n, .sync_at = -1,
						  .sync_id = a->p->sync[a->sync_id].id,
						  .flags = (uint8_t)(r->flags & AIR_CHUNK_FREE_RUN), .rssi_dbm = 0,
						  .hard = ERASED_HARD, .soft = ERASED_SOFT };
				air_event_t fe = { .kind = AIR_EV_RX, .t_us = f.t_us, .arg = 0, .chunk = &f };
				a->cb(&fe, a->cb_ctx);
				left = (uint16_t)(left - n);
			}
			c.t_us = r->t_us;
			c.nsyms = r->nsyms;
			c.sync_at = r->sync_at;
			c.sync_id = a->p->sync[a->sync_id].id;
			c.flags = r->flags;
			c.rssi_dbm = 0;
			c.hard = r->hard;
			c.soft = r->soft;
			ev.chunk = &c;
			a->st.chunks++;
		} else {
			ev.arg = e.arg;
		}
		air_event_cb_t cb = a->cb;
		if (cb != NULL) {
			cb(&ev, a->cb_ctx);
		}
		if (e.kind == AIR_EV_RX) {
			__atomic_thread_fence(__ATOMIC_RELEASE);
			a->rec_tail++;	// the record's slot is free once the callback is through
		}
	}
}

bool hrc6000_air_tx_begin(hrc6000_air_t *a)
{
	lock(a);
	bool ok = a->open && a->tx == HRC6000_AIR_RX && !a->rx_paused && (a->fit & AIR_FIT_TX) != 0 && txq_count(a) > 0;

	if (ok) {
		// The pair the board set for this channel's DMR, scaled to the profile; back after the transmission
		uint8_t m2 = 0, m1 = 0;
		if (hrc_read(a->dev, HRC_PAGE_SYS, R_MOD2, &m2) && hrc_read(a->dev, HRC_PAGE_SYS, R_MOD1, &m1)) {
			a->mod2 = m2;
			a->mod1 = m1;
			a->mod_saved = true;
			hrc6000_air_mod_pair(a->p, m2, m1, &a->tx_mod2, &a->tx_mod1);
			(void)hrc_set_mod_gain(a->dev, a->tx_mod2, a->tx_mod1);
		}
		uint8_t half[HRC6000_AIR_HALF_BYTES];
		a->copy = false;	// the chip leaves FM: no copy halves while it sends
		w(a, R_SLOT_CTL, CTL_NONE);
		w(a, R_MODE, MODE_PHY_TX);
		// The chip's own transmission start (the HR_C6000's: the access policy and its encoder start, 0x22 = 0x86);
		// a chip whose vocoder runs on the MCU (the HR_C5000) has none, and gets the access policy alone
		if (a->dev->chip->tx_begin != NULL) {
			a->dev->chip->tx_begin(a->dev, ACCESS_IMPOLITE);
		} else {
			w(a, R_ACCESS0, 0x00);
			w(a, R_ACCESS1, ACCESS_IMPOLITE);
		}
		// The chip sends TX RAM whenever the PA is up: idle in both halves until the first chunk's slot
		idle_half(a, half);
		(void)hrc_write_burst(a->dev, HRC_PAGE_DATA, TX_HALF0, half, sizeof(half));
		(void)hrc_write_burst(a->dev, HRC_PAGE_DATA, TX_HALF1, half, sizeof(half));
		a->tx_n = 0;
		a->t_begin = now(a);
		a->rearm_hold = false;
		a->underrun = false;
		a->pa = false;
		a->tx = HRC6000_AIR_START;
		w(a, R_L2_CTL, L2_ACTIVE);	// own timing: the first timeslot interrupt comes up to 30 ms after
	}
	unlock(a);
	return ok;
}

bool hrc6000_air_tx_queue(hrc6000_air_t *a, const air_tx_chunk_t *c)
{
	lock(a);
	bool ok = a->open && (a->tx == HRC6000_AIR_RX || a->tx == HRC6000_AIR_START || a->tx == HRC6000_AIR_RUN)
		  && !a->end_req && !last_queued(a) && c->nsyms > 0 && c->nsyms <= HRC6000_AIR_SYMS && txq_count(a) < HRC6000_AIR_TXQ;

	if (ok) {
		a->buf->txq[a->txq_head % HRC6000_AIR_TXQ] = *c;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		a->txq_head++;
	}
	unlock(a);
	return ok;
}

uint8_t hrc6000_air_tx_space(const hrc6000_air_t *a)
{
	if (!a->open || a->end_req || last_queued(a) || a->tx == HRC6000_AIR_LAST || a->tx == HRC6000_AIR_TAIL) {
		return 0;
	}
	return (uint8_t)(HRC6000_AIR_TXQ - txq_count(a));
}

void hrc6000_air_tx_end(hrc6000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (txq_count(a) > 0) {
			a->buf->txq[(uint8_t)(a->txq_head - 1u) % HRC6000_AIR_TXQ].flags |= AIR_TX_LAST;
		} else if (a->tx == HRC6000_AIR_START || a->tx == HRC6000_AIR_RUN) {
			a->end_req = true;
		}
	}
	unlock(a);
}

void hrc6000_air_tx_abort(hrc6000_air_t *a)
{
	lock(a);
	if (a->open) {
		if (a->tx != HRC6000_AIR_RX) {
			tx_stop(a, now(a), true);
		} else {
			a->txq_tail = a->txq_head;
			a->end_req = false;
		}
	}
	unlock(a);
	notify(a);
}

bool hrc6000_air_tx_active(const hrc6000_air_t *a)
{
	return a->open && a->tx != HRC6000_AIR_RX;
}

#endif	// CONFIG_AIR
