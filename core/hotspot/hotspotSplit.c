// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex extension of the MMDVM hotspot (see hotspotSplit.h; docs/spec/split-duplex.md)
#if CONFIG_HOTSPOT_SPLIT	// the whole file

#include <string.h>
#include "hotspot/hotspotSplit.h"
#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "hotspot/hotspotUplink.h"
#include "hotspot/dmrDataFrame.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/fec/dmrCach.h"
#include "dmr/call/dmr_lc.h"

#define LC_LEN		9
#define SLOT_US		30000u

typedef struct {
	hotspotUplink_t up;
	uint8_t sf_frag[4][4];		// the embedded LC fragments of B-E for the superframe going up
	uint8_t alias_lc[LC_LEN];	// a talker alias / GPS embedded LC heard: sent in the next superframe
	bool alias_pending;
	uint32_t last_ms;		// last burst heard on this slot
	uint32_t data_ms;		// last data burst heard
	bool heard;
} rx_slot_t;

// A scheduled burst (spec 3.9), held until its slot is handed to the driver
typedef struct {
	bool used;
	uint32_t counter;		// ALIGN's numbering
	dmr_tx_burst_t b;
	uint8_t lc[LC_LEN];
	bool lc_valid;
} sched_t;

static struct {
	uint8_t role;
	uint32_t lease_ms;
	uint32_t last_cmd_ms;		// last 0xD6 request (the lease)
	uint32_t last_host_ms;		// last frame of any kind
	uint32_t now_ms;		// last time seen (poll or frame)
	uint16_t lease_expiries;
	uint32_t time_requests;

	// TX half
	uint8_t power_cap, max_key_10s, cooldown_s;
	bsOutbound_t out;
	bool bs_running;
	bool want_key;			// DMR_START 01 / DMR_DATA asked for it
	bool keyed;			// bs_key(true) done
	bool unkey_req;			// DMR_START 00: unkey once drained
	uint32_t unkey_ms;
	uint32_t key_ms;		// when keyed
	bool cooldown;
	uint32_t cooldown_ms;		// when it ends
	bool tx_overflow;		// a NAK 5 since the last status
	uint8_t net_index[2];		// last voice position the host sent per slot (control byte 0)
	bool net_term[2];		// the host's last frame on the slot was a terminator: its repeats are dropped
	dmr_emb_rx_t emb_rx[2];		// the host's embedded LC per slot
	uint8_t emb_lc[2][LC_LEN];
	bool emb_valid[2];
	uint16_t slc_taken, slc_bad;
	uint8_t variant, variant_started;
	uint8_t nregs;
	uint8_t regs[DMR_BS_REGS_MAX][2];

	// TX half: ALIGN and the scheduled stream (spec 3.8, 3.9)
	uint8_t stream;
	bool al_set;			// a grid from ALIGN
	uint32_t al_t, al_c, al_period;	// one of its boundaries (modem clock), that slot's number, the period (ns)
	bool al_base;			// the running slot clock is numbered from the grid: counter = driver counter + cbase
	uint32_t cbase;
	bool snapping;			// restarted onto the grid, checking the first boundaries
	bool snap_failed;		// ... and it stayed off after the retries: no more until the next ALIGN or restart
	uint8_t snap_tries;
	uint32_t snap_c;		// the driver counter when it restarted
	int32_t snap_adj;		// the chip's restart delay, learned (µs)
	sched_t sched[SPLIT_SCHED_N];
	bool commit_valid;
	uint32_t commit_next;		// the driver counter of the next slot to hand over
	bool sched_key;			// the key-up came from a queued burst
	uint32_t sched_last_ms;		// last scheduled burst handed over
	uint16_t sch_sent, sch_late, sch_refused, sch_replaced, sch_cool;
	// ... and the slewing (spec 3.8)
	bool slewing;			// stepping the slot clock toward the grid
	uint8_t align_mode;		// SPLIT_ALIGN_xxx of the last ALIGN
	uint16_t max_slew;		// µs per slot, on average
	uint32_t slew_c;		// the driver counter at the last step (or the start)
	uint16_t slew_steps;
	int32_t slew_us;

	// RX half
	slotEpoch_t ep;
	rx_slot_t rx[2];
	uint16_t labeled[2], unlabeled;
	uint8_t options;		// SET_ROLE byte 7
	uint16_t meta_seq;		// RX_META events sent
} sp;

// ---- Helpers ----

static const hal_dmr_modem_t *modem(void)
{
	return hotspot_modem();
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v);
	put16(p + 2, v >> 16);
}

static int32_t abs32(int32_t v)
{
	return (v < 0) ? -v : v;
}

static void ack(uint8_t command)
{
	mmdvm_send(MMDVM_ACK, &command, 1);
}

static void nak(uint8_t command, uint8_t reason)
{
	uint8_t p[2] = { command, reason };

	mmdvm_send(MMDVM_NAK, p, 2);
}

static void reply(uint8_t sub, uint8_t result, const uint8_t *data, uint8_t len)
{
	uint8_t p[2 + SPLIT_STATUS_LEN];

	p[0] = sub;
	p[1] = result;
	if ((data != NULL) && (len > 0)) {
		memcpy(p + 2, data, len);
	}
	mmdvm_send(MMDVM_ODR_SPLIT, p, (uint8_t)(2 + len));
}

static uint8_t roles_offered(void)
{
	const hal_dmr_modem_t *m = modem();
	uint8_t r = 0;

	if ((m == NULL) || (m->now_us == NULL) || (m->slot_clock == NULL)) {
		return 0;
	}
	if ((m->bs_start != NULL) && (m->bs_key != NULL) && (m->bs_queue != NULL) && (m->bs_space != NULL) &&
	    (m->bs_stop != NULL)) {
		r |= 1u;
	}
	r |= 2u;
	return r;
}

static bool slot_clock(uint32_t *counter, uint32_t *t_us, int8_t *ts)
{
	const hal_dmr_modem_t *m = modem();

	return (m != NULL) && (m->slot_clock != NULL) && m->slot_clock(counter, t_us, ts);
}

static uint16_t caps(void)
{
	const hal_dmr_modem_t *m = modem();
	uint16_t c = 0;

	uint8_t r = roles_offered();

	if (m == NULL) {
		return 0;
	}
	if (r & 2u) {
		c |= SPLIT_CAP_RX_META;
	}
	if ((r & 1u) && (m->bs_restart != NULL)) {
		c |= SPLIT_CAP_ALIGN;
		if (m->bs_queue_at != NULL) {
			c |= SPLIT_CAP_SCHED;	// slots are numbered only after an ALIGN
		}
		if ((m->bs_slew != NULL) && (m->bs_slew_step_us > 0)) {
			c |= SPLIT_CAP_SLEW;
		}
	}
	return c;
}

static uint32_t now_us(void)
{
	const hal_dmr_modem_t *m = modem();

	return ((m != NULL) && (m->now_us != NULL)) ? m->now_us() : 0;
}

// ---- TX half: the ALIGN grid (spec 3.8) ----


// Where boundary t lies on the grid: its slot number there and t minus that slot's grid boundary (±half a period)
static int32_t grid_phase(uint32_t al_t, uint32_t al_c, uint32_t period, uint32_t t, uint32_t *counter)
{
	int64_t d = (int64_t)(int32_t)(t - al_t) * 1000;	// ns from the grid's boundary
	int64_t k = (d >= 0) ? (d + period / 2) / period : -((-d + period / 2) / period);

	if (counter != NULL) {
		*counter = al_c + (uint32_t)k;
	}
	return (int32_t)((d - k * (int64_t)period) / 1000);
}

static int8_t ts_of_counter(uint32_t counter)
{
	return ((counter & 1u) == 0) ? 1 : 2;	// even: outbound TS1
}

// The slot clock against the grid now (0 without one, grid_c and drv_c then untouched); ok: the grid's parity matches
// the slot set up there
static int32_t phase_now(bool *ok, uint32_t *grid_c, uint32_t *drv_c)
{
	uint32_t c, t, g;
	int8_t ts;

	*ok = false;
	if (!sp.al_set || !sp.bs_running || !slot_clock(&c, &t, &ts) || (ts == 0)) {
		return 0;
	}
	int32_t ph = grid_phase(sp.al_t, sp.al_c, sp.al_period, t, &g);

	*ok = ts_of_counter(g) == ts;
	if (grid_c != NULL) {
		*grid_c = g;
	}
	if (drv_c != NULL) {
		*drv_c = c;
	}
	return ph;
}

static uint8_t align_flags_now(void)
{
	bool ok;
	int32_t ph = phase_now(&ok, NULL, NULL);
	uint8_t f = 0;

	if (sp.al_base && ok && (abs32(ph) < SPLIT_ALIGNED_US)) {
		f |= SPLIT_AL_ALIGNED;
	}
	if (sp.snapping) {
		f |= SPLIT_AL_SNAP;
	}
	if (sp.slewing) {
		f |= SPLIT_AL_SLEWING;
	}
	if (sp.al_base) {
		f |= SPLIT_AL_BASE;
	}
	return f;
}

// Restart the slot clock onto the grid's next boundary a few ms ahead (the driver waits for it)
static bool snap_start(void)
{
	const hal_dmr_modem_t *m = modem();
	uint32_t c, t;
	int8_t ts;

	if ((m == NULL) || (m->bs_restart == NULL) || sp.keyed) {
		return false;
	}
	if (!slot_clock(&c, &t, &ts)) {
		return false;
	}
	// A grid boundary far enough ahead that the request, earlier by the learned delay, is still ahead too
	uint32_t now = now_us();
	int64_t d = (int64_t)(int32_t)(now + 5000u + (uint32_t)((sp.snap_adj > 0) ? sp.snap_adj : 0) - sp.al_t) * 1000;
	int64_t k = (d >= 0) ? (d + sp.al_period - 1) / sp.al_period : -((-d) / sp.al_period);
	uint32_t target = sp.al_t + (uint32_t)(int32_t)((k * (int64_t)sp.al_period) / 1000);

	if (!m->bs_restart(target - (uint32_t)sp.snap_adj, ts_of_counter(sp.al_c + (uint32_t)k))) {
		return false;
	}
	sp.snapping = true;
	sp.snap_c = c;
	sp.al_base = false;
	sp.commit_valid = false;
	return true;
}

// After a restart: the first boundaries on the new clock tell how far off it came up; again with the delay learned
static void snap_check(void)
{
	bool ok;
	uint32_t g, c, t;
	int8_t ts;

	// The drivers give no slot clock from the restart until the new clock's first boundary
	if (!sp.snapping || !slot_clock(&c, &t, &ts) || (ts == 0) || ((int32_t)(c - sp.snap_c) < 2)) {
		return;
	}
	int32_t ph = phase_now(&ok, &g, &c);

	sp.snapping = false;
	if (((abs32(ph) >= SPLIT_SNAP_TOL_US) || !ok) && !sp.keyed) {
		if (sp.snap_tries < SPLIT_SNAP_TRIES) {
			sp.snap_tries++;
			sp.snap_adj += ph;	// the chip's restart delay (taken as under half a slot)
			if (snap_start()) {
				return;
			}
		}
		sp.snap_failed = true;
	}
	if (ok) {
		sp.cbase = g - c;
		sp.al_base = true;
	}
}

static void align_lost(void)
{
	sp.al_base = false;
	sp.snapping = false;
	sp.snap_failed = false;
	sp.commit_valid = false;
	sp.slewing = false;
}

// Slewing (spec 3.8): the slot clock stepped toward the grid, never faster than max_slew per slot on average, a step
// only once the last one shows in the phase. The numbering (counter base) holds throughout: the nearest grid slot
// stays the same while the phase shrinks
static void slew_poll(void)
{
	const hal_dmr_modem_t *m = modem();
	bool ok;
	uint32_t c;
	int32_t ph;

	if (!sp.slewing) {
		return;
	}
	ph = phase_now(&ok, NULL, &c);
	if (!ok || !sp.al_base) {
		sp.slewing = false;	// no clock, or on the other slot: ALIGN again
		return;
	}
	int32_t step = (int32_t)m->bs_slew_step_us;

	if (abs32(ph) <= step / 2) {
		sp.slewing = false;	// as close as the driver's step gets
		return;
	}
	uint32_t slots = c - sp.slew_c;

	if (slots < SPLIT_SLEW_GAP) {
		return;
	}
	// What the rate allows since the last step, in whole driver steps, and no more than the phase
	int64_t budget = (int64_t)slots * sp.max_slew;
	int32_t want = (abs32(ph) / step + ((abs32(ph) % step) * 2 >= step ? 1 : 0)) * step;

	if (budget < step) {
		return;
	}
	if ((int64_t)want > budget) {
		want = (int32_t)(budget / step) * step;
	}
	int32_t got = m->bs_slew((ph > 0) ? -want : want);	// late: shorter slots

	if (got != 0) {
		sp.slew_c = c;
		sp.slew_steps++;
		sp.slew_us += got;
	}
}

// Start slewing toward the grid just set (al_* and the counter base)
static bool slew_start(uint32_t c)
{
	if (!(caps() & SPLIT_CAP_SLEW)) {
		return false;
	}
	sp.slewing = true;
	sp.slew_c = c;
	return true;
}

static uint8_t flags_now(void)
{
	uint8_t f = 0;

	if ((sp.role == SPLIT_ROLE_RX) && slotEpochValid(&sp.ep, sp.now_ms)) {
		f |= SPLIT_FLAG_EPOCH;
	}
	if ((sp.role == SPLIT_ROLE_TX) && sp.keyed) {
		f |= SPLIT_FLAG_KEYED;
	}
	if ((sp.role == SPLIT_ROLE_TX) && sp.cooldown) {
		f |= SPLIT_FLAG_COOLDOWN;
	}
	if ((sp.role == SPLIT_ROLE_TX) && sp.bs_running) {
		uint32_t c, t;
		int8_t ts;

		if (slot_clock(&c, &t, &ts) && (ts != 0)) {
			f |= SPLIT_FLAG_CLOCK;
		}
	}
	return f;
}

static uint8_t inbound_busy(void)
{
	uint8_t m = 0;

	if (sp.role != SPLIT_ROLE_RX) {
		return 0;
	}
	for (int i = 0; i < 2; i++) {
		rx_slot_t *r = &sp.rx[i];

		if (hotspotUplinkCallOpen(&r->up) ||
		    (r->heard && ((uint32_t)(sp.now_ms - r->data_ms) < SPLIT_BUSY_DATA_MS) && (r->data_ms != 0))) {
			m |= (uint8_t)(1u << i);
		}
	}
	return m;
}

// ---- TX half ----

static void tx_unkey(void)
{
	const hal_dmr_modem_t *m = modem();

	if (sp.keyed && (m != NULL) && (m->bs_key != NULL)) {
		m->bs_key(false);
	}
	sp.keyed = false;
	sp.want_key = false;
	sp.unkey_req = false;
	sp.sched_key = false;
	sp.commit_valid = false;
}

static void tx_stop_clock(void)
{
	const hal_dmr_modem_t *m = modem();

	tx_unkey();
	if (sp.bs_running && (m != NULL) && (m->bs_stop != NULL)) {
		m->bs_stop();
	}
	sp.bs_running = false;
	bsOutboundReset(&sp.out);
	align_lost();
}

static bool tx_start_clock(void)
{
	const hal_dmr_modem_t *m = modem();
	dmr_bs_config_t c;

	if (sp.bs_running) {
		return true;
	}
	if (!hotspot_running() || (m == NULL) || (m->bs_start == NULL)) {
		return false;
	}
	memset(&c, 0, sizeof(c));
	c.cc = hotspot_color_code();
	c.variant = sp.variant;
	c.nregs = sp.nregs;
	memcpy(c.regs, sp.regs, sizeof(c.regs));
	sp.bs_running = m->bs_start(&c);
	sp.variant_started = sp.variant;
	return sp.bs_running;
}

static void tx_feed(uint32_t now_ms)
{
	const hal_dmr_modem_t *m = modem();

	for (int8_t ts = 1; ts <= 2; ts++) {
		for (int n = 0; (n < 4) && (m->bs_space(ts) > 0); n++) {
			dmr_tx_burst_t b;
			dmr_cach_t c;
			const uint8_t *lc;

			bsOutboundNext(&sp.out, ts, now_ms, &b, &c, &lc);
			if (!m->bs_queue(ts, &b, &c, lc)) {
				break;
			}
		}
	}
}

// ---- TX half: the scheduled stream (spec 3.9) ----

static sched_t *sched_find(uint32_t counter)
{
	sched_t *e = &sp.sched[counter % SPLIT_SCHED_N];

	return (e->used && (e->counter == counter)) ? e : NULL;
}

// The modem clock time of driver counter x's boundary, from the slot clock's last one
static int32_t until_boundary(uint32_t x, uint32_t c, uint32_t t)
{
	return (int32_t)(t + (uint32_t)((int32_t)(x - c) * (int32_t)SLOT_US) - now_us());
}

// Hand the driver every slot whose boundary is near, each tagged with its counter (a scheduled burst, else what the
// stream rules give); a slot already passed is skipped, so nothing is ever moved to another slot
static void sched_commit(uint32_t now_ms)
{
	const hal_dmr_modem_t *m = modem();
	uint32_t c, t;
	int8_t ts;

	if (!sp.al_base || !slot_clock(&c, &t, &ts) || (ts == 0)) {
		return;
	}
	if (!sp.commit_valid || ((int32_t)(sp.commit_next - (c + 1u)) < 0)) {
		sp.commit_next = c + 1u;
		sp.commit_valid = true;
	}
	while (until_boundary(sp.commit_next, c, t) < SPLIT_SCHED_COMMIT_US) {
		uint32_t x = sp.commit_next;
		int8_t xts = (((x - c) & 1u) != 0) ? (int8_t)(3 - ts) : ts;
		dmr_tx_burst_t b;
		dmr_cach_t cach;
		const uint8_t *lc;

		if (m->bs_space(xts) == 0) {
			break;		// the driver is behind: the next poll
		}
		sched_t *e = sched_find(x + sp.cbase);

		if (e != NULL) {
			bsOutboundPush(&sp.out, xts, &e->b, e->lc_valid ? e->lc : NULL, now_ms);
			e->used = false;
			sp.sch_sent++;
			sp.sched_last_ms = now_ms;
		}
		bsOutboundNext(&sp.out, xts, now_ms, &b, &cach, &lc);
		if (!m->bs_queue_at(x, xts, &b, &cach, lc)) {
			break;
		}
		sp.commit_next++;
	}
}

// Scheduled bursts whose slot can no longer be handed over: late
static void sched_expire(void)
{
	uint32_t c, t;
	int8_t ts;

	if (!sp.al_base || !slot_clock(&c, &t, &ts)) {
		return;
	}
	for (int i = 0; i < SPLIT_SCHED_N; i++) {
		sched_t *e = &sp.sched[i];

		if (!e->used) {
			continue;
		}
		uint32_t x = e->counter - sp.cbase;
		bool passed = sp.commit_valid ? ((int32_t)(x - sp.commit_next) < 0)
					      : (until_boundary(x, c, t) < SPLIT_SCHED_COMMIT_US);

		if (passed) {
			e->used = false;
			sp.sch_late++;
		}
	}
}

// The earliest queued burst's boundary, µs from now (INT32_MAX: none)
static int32_t sched_first_us(void)
{
	uint32_t c, t;
	int8_t ts;
	int32_t best = INT32_MAX;

	if (!sp.al_base || !slot_clock(&c, &t, &ts)) {
		return best;
	}
	for (int i = 0; i < SPLIT_SCHED_N; i++) {
		if (sp.sched[i].used) {
			int32_t u = until_boundary(sp.sched[i].counter - sp.cbase, c, t);

			best = (u < best) ? u : best;
		}
	}
	return best;
}

static void sched_clear(void)
{
	for (int i = 0; i < SPLIT_SCHED_N; i++) {
		sp.sched[i].used = false;
	}
}

static void tx_poll(uint32_t now_ms)
{
	const hal_dmr_modem_t *m = modem();

	if (sp.cooldown && ((int32_t)(now_ms - sp.cooldown_ms) >= 0)) {
		sp.cooldown = false;
	}
	if (!sp.bs_running && !tx_start_clock()) {
		return;
	}
	snap_check();
	slew_poll();
	if (sp.al_set && !sp.al_base && !sp.snapping && !sp.snap_failed && !sp.keyed && !sp.want_key) {
		sp.snap_tries = 0;
		snap_start();		// the slot clock restarted (a retune): back onto the last ALIGN's grid
	}
	if (sp.stream == SPLIT_STREAM_SCHED) {
		if (!sp.keyed) {
			sched_expire();
		}
		if (!sp.keyed && !sp.want_key && !sp.cooldown && (sched_first_us() < SPLIT_SCHED_KEY_US)) {
			sp.want_key = true;	// a queued burst keys up in time for its slot (a TS1 slot first)
			sp.sched_key = true;
			sp.sched_last_ms = now_ms;
		}
	}
	if (sp.want_key && !sp.keyed && !sp.cooldown) {
		if (sp.variant != sp.variant_started) {
			tx_stop_clock();	// a new experiment variant: start the clock again with it (a new epoch)
			sp.want_key = true;
			if (!tx_start_clock()) {
				return;
			}
		}
		if (sp.stream == SPLIT_STREAM_BUFFER) {
			tx_feed(now_ms);	// two bursts ahead per slot, then key
		}
		m->bs_key(true);
		sp.keyed = true;
		sp.key_ms = now_ms;
		sp.commit_valid = false;
	}
	if (!sp.keyed) {
		return;
	}
	// Unkey: DMR_START 00 once drained, the host silent, the longest key-up (then the cool-down)
	bool drained = !bsOutboundBusy(&sp.out);

	if (sp.unkey_req && (drained || ((uint32_t)(now_ms - sp.unkey_ms) >= SPLIT_UNKEY_DRAIN_MS))) {
		tx_unkey();
		bsOutboundReset(&sp.out);
		return;
	}
	if ((uint32_t)(now_ms - sp.last_host_ms) >= SPLIT_HOST_SILENT_MS) {
		tx_unkey();
		bsOutboundReset(&sp.out);
		return;
	}
	if ((uint32_t)(now_ms - sp.key_ms) >= (uint32_t)sp.max_key_10s * 10000u) {
		tx_unkey();
		bsOutboundReset(&sp.out);
		sp.cooldown = true;
		sp.cooldown_ms = now_ms + (uint32_t)sp.cooldown_s * 1000u;
		return;
	}
	if (sp.stream == SPLIT_STREAM_SCHED) {
		sched_commit(now_ms);
		sched_expire();
		if (sp.sched_key && drained && (sched_first_us() == INT32_MAX) &&
		    ((uint32_t)(now_ms - sp.sched_last_ms) >= SPLIT_SCHED_HANG_MS)) {
			tx_unkey();	// a key-up the queued bursts made ends after them
			bsOutboundReset(&sp.out);
		}
		return;
	}
	tx_feed(now_ms);
}

// The 32 embedded bits of a voice frame (B-E): between the EMB halves, at frame bits 116-147
static void frame_emb(const uint8_t *frame, uint8_t *emb_hi, uint8_t frag[4], uint8_t *emb_lo)
{
	uint8_t mid[6];

	for (int k = 0; k < 6; k++) {
		mid[k] = (uint8_t)((frame[13 + k] << 4) | (frame[14 + k] >> 4));
	}
	*emb_hi = mid[0];
	memcpy(frag, mid + 1, 4);
	*emb_lo = mid[5];
}

#define NET_SKIP	0xFF	// net_burst: nothing to send (the outbound makes its own)

// A host frame (control byte and 33 bytes, as DMR_DATA1/2) for outbound slot ts as a burst: 0, NET_SKIP, or a NAK
// reason. lc: for voice A, the host's embedded LC (else NULL)
static uint8_t net_burst(int8_t ts, uint8_t control, const uint8_t *frame, dmr_tx_burst_t *burst, const uint8_t **lc_out)
{
	int k = ts - 1;
	uint8_t c = hotspot_frame_control(control, frame);
	dmr_tx_burst_t b;
	const uint8_t *lc = NULL;

	memset(&b, 0, sizeof(b));
	if (c & MMDVM_DMR_SYNC_DATA) {
		uint8_t dt = c & 0x0F;

		if (dt == DT_IDLE) {
			return NET_SKIP;	// the outbound makes its own idle bursts
		}
		if ((dt == DT_TERMINATOR_WITH_LC) && sp.net_term[k]) {
			return NET_SKIP;	// the second and third terminators
		}
		b.kind = dt;
		b.host_crc = true;		// the host's CRC goes out as sent (CRC_MCU_Control)
		if ((dt == DT_VOICE_LC_HEADER) || (dt == DT_TERMINATOR_WITH_LC) || (dt == DT_VOICE_PI_HEADER)) {
			dmrFecBptcDecode(frame, b.payload);
			b.len = 12;
			if (dt == DT_VOICE_LC_HEADER) {
				dmr_emb_rx_reset(&sp.emb_rx[k]);
				sp.emb_valid[k] = false;
			}
		} else {
			dmrBurst_t d;

			if (!dmrDataFrameToBurst(dt, frame, &d) || (d.length > DMR_PAYLOAD_MAX)) {
				return MMDVM_NAK_INVALID;
			}
			b.len = d.length;
			memcpy(b.payload, d.payload, d.length);
		}
	} else {
		uint8_t index;

		if (c & MMDVM_DMR_SYNC_AUDIO) {
			index = 0;
		} else if (((c & 0x0F) >= 1) && ((c & 0x0F) <= 5)) {
			index = c & 0x0F;
		} else {
			index = (uint8_t)((sp.net_index[k] + 1) % 6);	// control byte 0: B-F follow in order
		}
		b.kind = DMR_BURST_VOICE;
		b.voice_index = index;
		b.len = DMR_VOICE_BYTES;
		hotspot_frame_voice(frame, b.payload);
		sp.net_index[k] = index;
		if ((index >= 1) && (index <= 4)) {
			// The host's embedded LC (talker alias and GPS pass through): used from the next superframe
			uint8_t hi, lo, frag[4], cc, lcss, out[LC_LEN];
			bool pi;

			frame_emb(frame, &hi, frag, &lo);
			uint16_t emb = (uint16_t)((hi << 8) | lo);

			if ((dmrFecEmbDecode(emb, &cc, &pi, &lcss) >= 0) &&
			    dmr_emb_rx_push(&sp.emb_rx[k], frag, lcss, out)) {
				memcpy(sp.emb_lc[k], out, LC_LEN);
				sp.emb_valid[k] = true;
			}
		} else if ((index == 0) && sp.emb_valid[k]) {
			lc = sp.emb_lc[k];
		}
	}
	sp.net_term[k] = (b.kind == DMR_BURST_TERMINATOR_LC);
	*burst = b;
	*lc_out = lc;
	return 0;
}

static uint8_t tx_net_frame(uint8_t command, uint8_t control, const uint8_t *frame, uint32_t now_ms)
{
	int8_t ts = (command == MMDVM_DMR_DATA1) ? 1 : 2;
	dmr_tx_burst_t b;
	const uint8_t *lc;

	if (sp.cooldown || (sp.stream == SPLIT_STREAM_SCHED)) {
		return MMDVM_NAK_WRONG_MODE;	// the scheduled stream takes TX_SCHED only
	}
	uint8_t r = net_burst(ts, control, frame, &b, &lc);

	if (r == NET_SKIP) {
		return 0;
	}
	if (r != 0) {
		return r;
	}
	r = bsOutboundPush(&sp.out, ts, &b, lc, now_ms);
	if (r != 0) {
		sp.tx_overflow = true;
		return r;
	}
	if (!sp.keyed) {
		sp.want_key = true;	// the first frame keys up too
	}
	sp.unkey_req = false;
	return 0;
}

// DMR_SHORTLC (spec 5.3): the 68 payload bits; else the 28-bit form with its CRC-8 in the 5th byte
static void tx_short_lc(const uint8_t *p, uint8_t len)
{
	uint32_t rows[4], slc;

	if (len < DMR_CACH_SLC_BYTES) {
		sp.slc_bad++;
		return;
	}
	dmrShortLcUnpack(p, rows);
	if (dmrShortLcDecode(rows, &slc)) {
		dmrShortLcEncode(slc, rows);	// single errors corrected
		bsOutboundSetShortLc(&sp.out, rows);
		sp.slc_taken++;
		return;
	}
	sp.slc_bad++;
	slc = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]) & 0x0FFFFFFFu;
	if (dmrShortLcCrc8(slc) == p[4]) {
		dmrShortLcEncode(slc, rows);
		bsOutboundSetShortLc(&sp.out, rows);
		sp.slc_taken++;
	}
}

// ---- RX half ----

static uint8_t up_cmd(int k, bool lost)
{
	if (lost) {
		return (k == 0) ? MMDVM_DMR_LOST1 : MMDVM_DMR_LOST2;
	}
	return (k == 0) ? MMDVM_DMR_DATA1 : MMDVM_DMR_DATA2;
}

static void rx_lost(int k)
{
	hotspot_up_lost(up_cmd(k, true));
	sp.rx[k].alias_pending = false;
}

static void rx_reset(void)
{
	for (int k = 0; k < 2; k++) {
		if (hotspotUplinkCallOpen(&sp.rx[k].up)) {
			rx_lost(k);
		}
		memset(&sp.rx[k], 0, sizeof(sp.rx[k]));
		hotspotUplinkReset(&sp.rx[k].up);
	}
}

static const uint8_t MS_VOICE_SYNC[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };

// A voice burst up, as the simplex uplink frames it (mmdvm-protocol.md 2.8) with this slot's own embedded LC
static void rx_voice(int k, const uint8_t *voice, uint8_t index, int16_t rssi_dbm, bool heard)
{
	rx_slot_t *r = &sp.rx[k];
	uint8_t frame[MMDVM_DMR_FRAME_LEN];
	uint8_t middle[6] = { 0, 0, 0, 0, 0, 0 };
	uint8_t payload[1 + MMDVM_DMR_FRAME_LEN + 2];
	uint8_t len = 1 + MMDVM_DMR_FRAME_LEN;
	uint8_t raw;

	if (index == 0) {
		dmr_emb_lc_encode(r->alias_pending ? r->alias_lc : hotspotUplinkLc(&r->up), r->sf_frag);
		r->alias_pending = false;
		memcpy(middle, MS_VOICE_SYNC, 6);
	} else {
		uint16_t emb = dmrFecEmbEncode(hotspot_color_code(), false, dmr_emb_lcss(index));

		middle[0] = (uint8_t)(emb >> 8);
		if (index <= 4) {
			memcpy(middle + 1, r->sf_frag[index - 1], 4);
		}
		middle[5] = (uint8_t)emb;
	}
	memcpy(frame, voice, 13);
	frame[13] = (uint8_t)((voice[13] & 0xF0) | (middle[0] >> 4));
	for (int i = 0; i < 5; i++) {
		frame[14 + i] = (uint8_t)((middle[i] << 4) | (middle[i + 1] >> 4));
	}
	frame[19] = (uint8_t)((middle[5] << 4) | (voice[13] & 0x0F));
	memcpy(frame + 20, voice + 14, 13);

	payload[0] = (index == 0) ? MMDVM_DMR_SYNC_AUDIO : index;
	memcpy(payload + 1, frame, MMDVM_DMR_FRAME_LEN);
	if (heard && hotspot_rssi_byte(rssi_dbm, &raw)) {
		payload[len++] = 0;
		payload[len++] = raw;
	}
	mmdvm_send(up_cmd(k, false), payload, len);
}

static bool data_with_crc(uint8_t kind)
{
	return (kind == DMR_BURST_PI_HEADER) || (kind == DMR_BURST_CSBK) || (kind == DMR_BURST_MBC_HEADER) ||
	       (kind == DMR_BURST_DATA_HEADER);
}

// RX_META (spec 3.10): what the RX half knows of one burst, sent before its uplink frames
static void rx_meta(const dmr_burst_t *b, bool timed, uint32_t t_slot, int8_t label, int32_t res)
{
	uint8_t p[2 + SPLIT_META_LEN];
	uint8_t *d = p + 2;
	uint8_t f = 0;

	memset(p, 0, sizeof(p));
	p[0] = SPLIT_EV_RX_META;
	p[1] = SPLIT_RESULT_OK;
	put32(d, t_slot);
	res = (res > 32767) ? 32767 : ((res < -32768) ? -32768 : res);
	put16(d + 4, (uint16_t)(int16_t)res);
	d[6] = (uint8_t)label;
	d[7] = (b->kind == DMR_BURST_VOICE) ? (uint8_t)(DMR_BURST_VOICE + (b->voice_index % 6u)) : b->kind;
	d[8] = b->cc;
	f |= b->crc_ok ? SPLIT_META_CRC_OK : 0u;
	f |= b->in_step ? SPLIT_META_IN_STEP : 0u;
	f |= b->pi ? SPLIT_META_PI : 0u;
	f |= b->lc_valid ? SPLIT_META_LC : 0u;
	f |= (b->sync == DMR_SYNC_BS) ? SPLIT_META_BS_SYNC : 0u;
	f |= (label != 0) ? SPLIT_META_EPOCH : 0u;
	f |= timed ? SPLIT_META_TIMED : 0u;
	d[9] = f;
	put16(d + 10, (uint16_t)b->rssi_dbm);
	put16(d + 12, sp.meta_seq);
	sp.meta_seq++;
	mmdvm_send(MMDVM_ODR_SPLIT, p, sizeof(p));
}

static void rx_burst(const dmr_burst_t *b, uint32_t arg, uint32_t now_ms)
{
	uint32_t c_last, t_last, t_slot = 0;
	int8_t ts_dummy;
	int8_t label = 0;
	int32_t res = 0;
	bool timed = false;

	// The start of the slot the burst was in: the report comes in the slot after it (hrc6000_dmr.h)
	if (slot_clock(&c_last, &t_last, &ts_dummy)) {
		uint32_t back = (arg != 0) ? (c_last - arg + 1u) : 1u;

		if (back < 64u) {
			t_slot = t_last - back * SLOT_US;
			timed = true;
			label = slotEpochLabel(&sp.ep, t_slot, now_ms, &res);
		}
	}
	if (sp.options & SPLIT_OPT_RX_META) {
		rx_meta(b, timed, t_slot, label, res);
	}
	if (label == 0) {
		sp.unlabeled++;
		label = 2;		// no epoch: DMR_DATA2 (spec 7)
	} else {
		sp.labeled[label - 1]++;
		if (b->in_step && (res > -10000) && (res < 10000)) {
			slotEpochTrack(&sp.ep, res);
		}
	}
	int k = label - 1;
	rx_slot_t *r = &sp.rx[k];
	hotspotUplinkBurst_t u;
	uint8_t fill = 0;

	r->last_ms = now_ms;
	r->heard = true;
	memset(&u, 0, sizeof(u));
	u.ms = now_ms;
	switch (b->kind) {
	case DMR_BURST_VOICE:
		u.kind = HOTSPOT_UPLINK_VOICE;
		u.seq = b->voice_index;
		u.flags = (uint8_t)((b->lc_valid ? HOTSPOT_UPLINK_FLAG_LC_VALID : 0) |
				    (((b->voice_index == 0) || b->crc_ok) ? HOTSPOT_UPLINK_FLAG_SYNC_OK : 0));
		memcpy(u.lc, b->lc, LC_LEN);
		memcpy(u.ambe, b->payload, HOTSPOT_UPLINK_AMBE_LENGTH);
		if (b->lc_valid) {
			uint8_t flco = b->lc[0] & 0x3F;

			if ((flco >= DMR_FLCO_TA_HEADER) && (flco <= DMR_FLCO_GPS)) {
				memcpy(r->alias_lc, b->lc, LC_LEN);
				r->alias_pending = true;
			}
		}
		break;
	case DMR_BURST_VOICE_LC_HEADER:
	case DMR_BURST_TERMINATOR_LC:
		if (!b->crc_ok) {
			return;
		}
		u.kind = (b->kind == DMR_BURST_VOICE_LC_HEADER) ? HOTSPOT_UPLINK_HEADER : HOTSPOT_UPLINK_TERMINATOR;
		u.flags = HOTSPOT_UPLINK_FLAG_LC_VALID;
		memcpy(u.lc, b->lc_valid ? b->lc : b->payload, LC_LEN);
		break;
	default:
		if ((b->kind > DMR_BURST_RATE_1_DATA) || (b->kind == DMR_BURST_IDLE)) {
			return;
		}
		if (data_with_crc(b->kind) && !b->crc_ok) {
			return;
		}
		r->data_ms = now_ms;
		if (hotspotUplinkData(&r->up, b->kind, now_ms)) {
			hotspot_up_data(up_cmd(k, false), b->kind, b->payload, b->len, (b->rssi_dbm != 0) ? b->rssi_dbm : 0);
		}
		return;
	}

	uint8_t actions = hotspotUplinkBurst(&r->up, &u, &fill);
	const uint8_t *lc = hotspotUplinkLc(&r->up);

	if (actions & HOTSPOT_UP_LOST) {
		rx_lost(k);
	}
	if (actions & HOTSPOT_UP_HEADER) {
		hotspot_up_lc(up_cmd(k, false), DT_VOICE_LC_HEADER, lc,
			      (b->kind == DMR_BURST_VOICE_LC_HEADER) ? b->rssi_dbm : 0);
		dmr_emb_lc_encode(lc, r->sf_frag);
	}
	if (actions & HOTSPOT_UP_FILL) {
		for (uint8_t i = fill, n = 0; (i != u.seq) && (n < 6); i = (uint8_t)((i + 1) % 6), n++) {
			rx_voice(k, HOTSPOT_UPLINK_SILENCE, i, 0, false);
		}
	}
	if (actions & HOTSPOT_UP_VOICE) {
		rx_voice(k, u.ambe, u.seq, b->rssi_dbm, true);
	}
	if (actions & HOTSPOT_UP_TERMINATOR) {
		hotspot_up_lc(up_cmd(k, false), DT_TERMINATOR_WITH_LC, lc, b->rssi_dbm);
		r->alias_pending = false;
	}
}

static void rx_poll(uint32_t now_ms)
{
	for (int k = 0; k < 2; k++) {
		if (hotspotUplinkLost(&sp.rx[k].up, now_ms)) {
			rx_lost(k);
		}
	}
}

// ---- Roles ----

static void role_end(void)
{
	uint8_t was = sp.role;

	if (was == SPLIT_ROLE_TX) {
		tx_stop_clock();
		hotspot_set_power_cap(255);
	} else if (was == SPLIT_ROLE_RX) {
		rx_reset();
		slotEpochClear(&sp.ep);
	}
	sp.role = SPLIT_ROLE_NORMAL;
	sp.cooldown = false;
	sp.options = 0;
	if ((was != SPLIT_ROLE_NORMAL) && hotspot_running()) {
		const hal_dmr_modem_t *m = modem();

		if (m != NULL) {
			m->rx_restart();	// the simplex engine starts clean
		}
		mmdvm_ext_refresh();		// the SFR may be wanted again
	}
}

static void role_begin(uint8_t role)
{
	hotspot_alt_takeover();		// the simplex engine drops what it had
	sp.role = role;
	if (role == SPLIT_ROLE_TX) {
		bsOutboundInit(&sp.out, sp.out.prebuffer);
		hotspot_set_power_cap(sp.power_cap);
		sp.bs_running = false;
		sp.keyed = false;
		sp.want_key = false;
		sp.unkey_req = false;
		sp.tx_overflow = false;
		sp.al_set = false;	// the learned restart delay stays: it is the chip's
		align_lost();
		sched_clear();
		if (sp.stream == SPLIT_STREAM_SCHED) {
			bsOutboundSetPrebuffer(&sp.out, 1);	// a scheduled burst plays in its own slot
		}
		for (int k = 0; k < 2; k++) {
			sp.net_index[k] = 5;
			sp.net_term[k] = false;
			dmr_emb_rx_reset(&sp.emb_rx[k]);
			sp.emb_valid[k] = false;
		}
		tx_start_clock();
	} else {
		slotEpochClear(&sp.ep);
		rx_reset();
		hotspot_modem()->rx_lock_slot(0);
	}
}

// ---- Command 0xD6 ----

static uint8_t info_block(uint8_t *b)
{
	const hal_dmr_modem_t *m = modem();

	b[0] = SPLIT_VERSION;
	b[1] = roles_offered();
	b[2] = sp.role;
	b[3] = BS_OUT_DEPTH;
	b[4] = ((m != NULL) && (b[1] & 1u)) ? m->bs_cach : DMR_BS_CACH_NONE;
	put32(b + 5, ((m != NULL) && (m->now_us != NULL)) ? m->now_us() : 0);
	b[9] = flags_now();
	put16(b + 10, caps());
	int32_t air = (m != NULL) ? m->bs_air_delay_us : 0;

	b[12] = (uint8_t)((SPLIT_SCHED_COMMIT_US + air + 5000 + 999) / 1000);	// the hand-over, the delay, one poll
	b[13] = SPLIT_SCHED_N;
	put16(b + 14, (uint16_t)(int16_t)air);
	return SPLIT_INFO_LEN;
}

static uint8_t status_block(uint8_t *b)
{
	memset(b, 0, SPLIT_STATUS_LEN);
	b[0] = sp.role;
	b[1] = flags_now();
	b[2] = inbound_busy();
	if (sp.role == SPLIT_ROLE_TX) {
		b[3] = sp.out.at_on_air;
		b[4] = bsOutboundCount(&sp.out, 1);
		b[5] = bsOutboundCount(&sp.out, 2);
		put16(b + 6, sp.out.slot[0].underruns);
		put16(b + 8, sp.out.slot[1].underruns);
		put16(b + 10, sp.out.st.idle);
		put16(b + 12, sp.out.st.cach);
		put16(b + 14, sp.slc_taken);
		put16(b + 26, sp.keyed ? (sp.now_ms - sp.key_ms) / 1000u : 0u);
		put16(b + 28, sp.slc_bad);
	}
	if (sp.role == SPLIT_ROLE_RX) {
		put16(b + 16, sp.labeled[0]);
		put16(b + 18, sp.labeled[1]);
		put16(b + 20, sp.unlabeled);
		int32_t r = sp.ep.last_residual_us;

		r = (r > 32767) ? 32767 : ((r < -32768) ? -32768 : r);
		put16(b + 22, (uint16_t)(int16_t)r);
		uint32_t age = sp.ep.valid ? (sp.now_ms - sp.ep.set_ms) / 1000u : 65535u;

		put16(b + 24, (age > 65535u) ? 65535u : age);
	}
	put16(b + 30, sp.lease_expiries);
	if (sp.role == SPLIT_ROLE_TX) {
		bool ok;

		put16(b + 32, sp.sch_sent);
		put16(b + 34, sp.sch_late);
		put16(b + 36, sp.sch_refused);
		put16(b + 38, sp.sch_replaced);
		put16(b + 40, sp.sch_cool);
		put32(b + 42, (uint32_t)phase_now(&ok, NULL, NULL));
		b[46] = align_flags_now();
	}
	return SPLIT_STATUS_LEN;
}

static void set_role(const uint8_t *p, uint8_t len, uint8_t *res)
{
	if (len < 1) {
		*res = SPLIT_RESULT_INVALID;
		return;
	}
	uint8_t role = p[0];

	if (role > SPLIT_ROLE_RX) {
		*res = SPLIT_RESULT_INVALID;
		return;
	}
	if (role == SPLIT_ROLE_NORMAL) {
		role_end();
		return;
	}
	if (!hotspot_running() || !(roles_offered() & (1u << (role - 1)))) {
		*res = SPLIT_RESULT_STATE;
		return;
	}
	uint8_t lease = ((len > 1) && (p[1] != 0)) ? p[1] : SPLIT_LEASE_S;
	uint8_t cap = ((len > 2) && (p[2] != 0)) ? p[2] : SPLIT_POWER_CAP;
	uint8_t pre = ((len > 3) && (p[3] != 0)) ? p[3] : BS_OUT_PREBUFFER;
	uint8_t mk = ((len > 4) && (p[4] != 0)) ? p[4] : SPLIT_MAX_KEY_10S;
	uint8_t cool = ((len > 5) && (p[5] != 0)) ? p[5] : SPLIT_COOLDOWN_S;
	uint8_t stream = (len > 6) ? p[6] : SPLIT_STREAM_BUFFER;
	uint8_t options = (len > 7) ? p[7] : 0;

	if ((options & (uint8_t)~SPLIT_OPT_RX_META) != 0) {
		*res = SPLIT_RESULT_INVALID;
		return;
	}
	if ((stream == SPLIT_STREAM_SCHED) && !(caps() & SPLIT_CAP_SCHED) && (role == SPLIT_ROLE_TX)) {
		*res = SPLIT_RESULT_STATE;
		return;
	}
	if ((pre > BS_OUT_PREBUFFER_MAX) || (mk > SPLIT_MAX_KEY_10S_MAX) || (stream > SPLIT_STREAM_SCHED)) {
		*res = SPLIT_RESULT_INVALID;
		return;
	}
	sp.lease_ms = (uint32_t)lease * 1000u;
	sp.power_cap = cap;
	sp.max_key_10s = mk;
	sp.cooldown_s = cool;
	if (sp.role != role) {
		role_end();
		sp.out.prebuffer = pre;
		sp.stream = stream;
		role_begin(role);
	} else {
		if ((role == SPLIT_ROLE_TX) && (stream != sp.stream)) {
			tx_unkey();		// the other stream starts clean
			bsOutboundReset(&sp.out);
			sched_clear();
			sp.stream = stream;
		}
		bsOutboundSetPrebuffer(&sp.out, (sp.stream == SPLIT_STREAM_SCHED) ? 1 : pre);
		if (role == SPLIT_ROLE_TX) {
			hotspot_set_power_cap(cap);
		}
	}
	sp.options = options;	// after role_end, which clears them
}

// ALIGN (spec 3.8): the reply's phase is measured before anything changes
static uint8_t align(const uint8_t *p, uint8_t len, uint8_t *data, uint8_t *res)
{
	uint32_t c, t, g;
	int8_t ts;

	if (len < 12) {
		*res = SPLIT_RESULT_INVALID;
		return 0;
	}
	uint32_t at = le32(p), period = le32(p + 4), counter = le32(p + 8);
	uint16_t max_slew = (len >= 14) ? (uint16_t)(p[12] | (p[13] << 8)) : 0;
	uint8_t mode = (len >= 15) ? p[14] : SPLIT_ALIGN_SNAP;

	if (period == 0) {
		period = SLOT_US * 1000u;
	}
	if (max_slew == 0) {
		max_slew = SPLIT_SLEW_MAX_US;
	}
	if ((period < SLOT_EPOCH_PERIOD_MIN_NS) || (period > SLOT_EPOCH_PERIOD_MAX_NS) || !(caps() & SPLIT_CAP_ALIGN) ||
	    (max_slew > SPLIT_SLEW_MAX_US) || (mode > SPLIT_ALIGN_SLEW) ||
	    ((mode == SPLIT_ALIGN_SLEW) && !(caps() & SPLIT_CAP_SLEW))) {
		*res = SPLIT_RESULT_INVALID;
		return 0;
	}
	if ((sp.role != SPLIT_ROLE_TX) || !sp.bs_running || !slot_clock(&c, &t, &ts) || (ts == 0)) {
		*res = SPLIT_RESULT_STATE;
		return 0;
	}
	int32_t ph = grid_phase(at, counter, period, t, &g);
	bool parity_ok = ts_of_counter(g) == ts;
	bool slew = (caps() & SPLIT_CAP_SLEW) != 0;
	uint8_t f = 0;

	sp.max_slew = max_slew;
	sp.align_mode = mode;
	if (sp.keyed || sp.want_key || (mode == SPLIT_ALIGN_SLEW)) {
		if (parity_ok && ((abs32(ph) < SPLIT_ALIGNED_US) || slew)) {
			sp.al_t = at;		// the numbering changes; the phase, if it can, by slewing
			sp.al_c = counter;
			sp.al_period = period;
			sp.al_set = true;
			sp.cbase = g - c;
			sp.al_base = true;
			sp.snapping = false;
			sp.snap_failed = false;
			sp.slewing = false;
			if (abs32(ph) < SPLIT_ALIGNED_US) {
				f |= SPLIT_AL_ALIGNED;
			}
			if (slew && (abs32(ph) > (int32_t)modem()->bs_slew_step_us / 2) && slew_start(c)) {
				f |= SPLIT_AL_SLEWING;
			}
		} else {
			f |= SPLIT_AL_NOT_APPLIED;
		}
	} else {
		sp.slewing = false;
		sp.al_t = at;
		sp.al_c = counter;
		sp.al_period = period;
		sp.al_set = true;
		sp.snap_tries = 0;
		sp.snap_failed = false;
		if (snap_start()) {
			f |= SPLIT_AL_SNAP;
		}
	}
	put32(data, (uint32_t)ph);
	data[4] = f;
	return 5;
}

// TX_SCHED (spec 3.9)
static uint8_t tx_sched(const uint8_t *p, uint8_t len, uint8_t *data, uint8_t *res, uint32_t now_ms)
{
	uint32_t c, t;
	int8_t ts;

	if ((len != 6 + MMDVM_DMR_FRAME_LEN) || (p[4] != (uint8_t)ts_of_counter(le32(p)))) {
		*res = SPLIT_RESULT_INVALID;
		return 0;
	}
	if ((sp.role != SPLIT_ROLE_TX) || (sp.stream != SPLIT_STREAM_SCHED) || !sp.al_base ||
	    !slot_clock(&c, &t, &ts) || (ts == 0)) {
		*res = SPLIT_RESULT_STATE;
		return 0;
	}
	uint32_t g = le32(p);
	uint32_t x = g - sp.cbase;
	uint8_t outcome = SPLIT_SCHED_QUEUED;

	put32(data, g);
	if (sp.cooldown) {
		outcome = SPLIT_SCHED_FULL;
		sp.sch_cool++;
	} else if ((sp.commit_valid && ((int32_t)(x - sp.commit_next) < 0)) ||
		   (until_boundary(x, c, t) < SPLIT_SCHED_COMMIT_US)) {
		outcome = SPLIT_SCHED_LATE;
		sp.sch_late++;
	} else if ((int32_t)(x - c) > SPLIT_SCHED_N) {
		outcome = SPLIT_SCHED_EARLY;
		sp.sch_refused++;
	} else {
		dmr_tx_burst_t b;
		const uint8_t *lc;
		uint8_t r = net_burst(ts_of_counter(g), p[5], p + 6, &b, &lc);

		if ((r != 0) && (r != NET_SKIP)) {
			*res = SPLIT_RESULT_INVALID;
			return 0;
		}
		sched_t *e = &sp.sched[g % SPLIT_SCHED_N];
		bool same = e->used && (e->counter == g);

		if (e->used && !same) {
			outcome = SPLIT_SCHED_FULL;
			sp.sch_refused++;
		} else if (r == NET_SKIP) {
			e->used = false;	// an idle (or a repeated terminator) for that slot: the stream rules fill it
			outcome = same ? SPLIT_SCHED_REPLACED : SPLIT_SCHED_QUEUED;
		} else {
			e->used = true;
			e->counter = g;
			e->b = b;
			e->lc_valid = lc != NULL;
			if (lc != NULL) {
				memcpy(e->lc, lc, LC_LEN);
			}
			outcome = same ? SPLIT_SCHED_REPLACED : SPLIT_SCHED_QUEUED;
		}
		if (outcome == SPLIT_SCHED_REPLACED) {
			sp.sch_replaced++;
		}
		if (outcome != SPLIT_SCHED_FULL) {
			sp.unkey_req = false;
		}
	}
	(void)now_ms;
	data[4] = outcome;
	return 5;
}

static void split_command(const uint8_t *p, uint8_t len, uint32_t t_rx, uint32_t now_ms)
{
	uint8_t data[SPLIT_STATUS_LEN];
	uint8_t n = 0;
	uint8_t res = SPLIT_RESULT_OK;
	const hal_dmr_modem_t *m = modem();

	if (len < 1) {
		reply(0xFF, SPLIT_RESULT_INVALID, NULL, 0);
		return;
	}
	uint8_t sub = p[0];

	p++;
	len--;
	sp.last_cmd_ms = now_ms;	// every request refreshes the lease
	switch (sub) {
	case SPLIT_SUB_INFO:
		n = info_block(data);
		break;
	case SPLIT_SUB_SET_ROLE:
		set_role(p, len, &res);
		if (res == SPLIT_RESULT_OK) {
			n = info_block(data);
		}
		break;
	case SPLIT_SUB_TIME:
		if (len < 8) {
			res = SPLIT_RESULT_INVALID;
		} else if ((m == NULL) || (m->now_us == NULL)) {
			res = SPLIT_RESULT_STATE;
		} else {
			memcpy(data, p, 8);
			put32(data + 8, t_rx);
			put32(data + 12, m->now_us());	// the reply goes out at once after this
			n = 16;
			sp.time_requests++;
		}
		break;
	case SPLIT_SUB_GET_EPOCH: {
		uint32_t c, t;
		int8_t ts;

		if ((sp.role != SPLIT_ROLE_TX) || !sp.bs_running || !slot_clock(&c, &t, &ts) || (ts == 0)) {
			res = SPLIT_RESULT_STATE;
			break;
		}
		put32(data, sp.al_base ? c + sp.cbase : c);
		put32(data + 4, t);
		data[8] = (uint8_t)ts;
		data[9] = (uint8_t)((sp.keyed ? 1u : 0u) | 2u);
		n = 10;
		break;
	}
	case SPLIT_SUB_SET_EPOCH:
		if (sp.role != SPLIT_ROLE_RX) {
			res = SPLIT_RESULT_STATE;
		} else if (len < 4) {
			res = SPLIT_RESULT_INVALID;
		} else if (!slotEpochSet(&sp.ep, le32(p), (len >= 8) ? le32(p + 4) : 0, now_ms)) {
			res = SPLIT_RESULT_INVALID;
		}
		break;
	case SPLIT_SUB_SET_AT:
		if (sp.role != SPLIT_ROLE_TX) {
			res = SPLIT_RESULT_STATE;
		} else if ((len < 1) || ((len > 1) && (p[1] > BS_AT_MODE_MASK))) {
			res = SPLIT_RESULT_INVALID;
		} else {
			uint8_t mode = (len > 1) ? p[1] : BS_AT_MODE_OR;

			bsOutboundSetAt(&sp.out, p[0], mode, now_ms);
			data[0] = (uint8_t)((p[0] & 3u) | ((mode == BS_AT_MODE_OR) ? sp.out.at_auto : 0u));
			n = 1;
		}
		break;
	case SPLIT_SUB_STATUS:
		n = status_block(data);
		break;
	case SPLIT_SUB_ALIGN:
		n = align(p, len, data, &res);
		break;
	case SPLIT_SUB_TX_SCHED:
		n = tx_sched(p, len, data, &res, now_ms);
		break;
	default:
		res = SPLIT_RESULT_UNKNOWN;
		break;
	}
	reply(sub, res, (res == SPLIT_RESULT_OK) ? data : NULL, (res == SPLIT_RESULT_OK) ? n : 0);
}

// ---- mmdvm_ext_t ----

static bool ext_frame(uint8_t command, const uint8_t *p, uint8_t len, uint32_t now_ms)
{
	const hal_dmr_modem_t *m = modem();
	uint32_t t_rx = ((m != NULL) && (m->now_us != NULL)) ? m->now_us() : 0;	// first: TIME's receive stamp

	sp.now_ms = now_ms;
	sp.last_host_ms = now_ms;
	if (command == MMDVM_ODR_SPLIT) {
		split_command(p, len, t_rx, now_ms);
		return true;
	}
	if (sp.role == SPLIT_ROLE_TX) {
		switch (command) {
		case MMDVM_DMR_DATA1:
		case MMDVM_DMR_DATA2: {
			if (len != (1 + MMDVM_DMR_FRAME_LEN)) {
				nak(command, MMDVM_NAK_INVALID);
				return true;
			}
			uint8_t r = tx_net_frame(command, p[0], p + 1, now_ms);

			if (r != 0) {
				nak(command, r);	// silent on success, as MMDVM boards
			}
			return true;
		}
		case MMDVM_DMR_START:
			if ((len >= 1) && (p[0] != 0)) {
				if (!sp.cooldown) {
					sp.want_key = true;
					sp.unkey_req = false;
				}
			} else if (sp.keyed || sp.want_key) {
				sp.unkey_req = true;
				sp.unkey_ms = now_ms;
			}
			ack(command);
			return true;
		case MMDVM_DMR_SHORTLC:
			tx_short_lc(p, len);
			ack(command);
			return true;
		case MMDVM_DMR_ABORT:
			if (len >= 1) {
				bsOutboundDrop(&sp.out, (p[0] == 1) ? 1 : 2);
			} else {
				bsOutboundDrop(&sp.out, 1);
				bsOutboundDrop(&sp.out, 2);
			}
			ack(command);
			return true;
		default:
			return false;
		}
	}
	if (sp.role == SPLIT_ROLE_RX) {
		switch (command) {
		case MMDVM_DMR_DATA1:
		case MMDVM_DMR_DATA2:
			nak(command, MMDVM_NAK_WRONG_MODE);	// receive only
			return true;
		case MMDVM_DMR_START:
		case MMDVM_DMR_SHORTLC:
		case MMDVM_DMR_ABORT:
			ack(command);
			return true;
		default:
			return false;
		}
	}
	return false;
}

static bool ext_owns(void)
{
	return sp.role != SPLIT_ROLE_NORMAL;
}

static void ext_end(void)
{
	role_end();
}

static uint8_t ext_version_suffix(char *buf, uint8_t max)
{
	static const char s[] = " SPLIT1";
	uint8_t n = (uint8_t)(sizeof(s) - 1);

	if (n > max) {
		return 0;
	}
	memcpy(buf, s, n);
	return n;
}

static uint8_t ext_status_flags(void)
{
	uint8_t f = 0;

	if (sp.role == SPLIT_ROLE_TX) {
		if (sp.tx_overflow) {
			f |= MMDVM_STATUS_TX_OVERFLOW;
			sp.tx_overflow = false;
		}
	} else if (sp.role == SPLIT_ROLE_RX) {
		for (int k = 0; k < 2; k++) {
			if (hotspotUplinkCallOpen(&sp.rx[k].up) ||
			    (sp.rx[k].heard && ((uint32_t)(sp.now_ms - sp.rx[k].last_ms) < SPLIT_CD_HOLD_MS))) {
				f |= MMDVM_STATUS_CD;
			}
		}
	}
	return f;
}

static void ext_space(uint8_t *slot1, uint8_t *slot2)
{
	if (sp.role == SPLIT_ROLE_TX) {
		int s1 = bsOutboundSpace(&sp.out, 1) - SPLIT_SPACE_MARGIN;
		int s2 = bsOutboundSpace(&sp.out, 2) - SPLIT_SPACE_MARGIN;

		*slot1 = (uint8_t)((s1 < 0) || sp.cooldown ? 0 : s1);
		*slot2 = (uint8_t)((s2 < 0) || sp.cooldown ? 0 : s2);
	} else if (sp.role == SPLIT_ROLE_RX) {
		*slot1 = 0;
		*slot2 = 0;
	}
}

static const mmdvm_ext_t ext = {
	.version_suffix = ext_version_suffix, .status_flags = ext_status_flags, .space = ext_space,
	.frame = ext_frame, .owns = ext_owns, .end = ext_end,
};

// ---- hotspot_alt_t ----

static bool alt_active(void)
{
	return sp.role != SPLIT_ROLE_NORMAL;
}

static void alt_poll(uint32_t now_ms)
{
	sp.now_ms = now_ms;
	if (sp.role == SPLIT_ROLE_NORMAL) {
		return;
	}
	if ((uint32_t)(now_ms - sp.last_cmd_ms) >= sp.lease_ms) {
		sp.lease_expiries++;	// the split host is gone: back to the standard hotspot (spec 2.3)
		role_end();
		return;
	}
	if (sp.role == SPLIT_ROLE_TX) {
		tx_poll(now_ms);
	} else {
		rx_poll(now_ms);
	}
}

static void alt_event(uint8_t kind, const dmr_burst_t *burst, uint32_t now_ms)
{
	(void)kind;
	(void)burst;
	sp.now_ms = now_ms;	// event_arg below takes the bursts
}

static void alt_event_arg(uint8_t kind, const dmr_burst_t *burst, uint32_t arg, uint32_t now_ms)
{
	sp.now_ms = now_ms;
	if ((sp.role == SPLIT_ROLE_RX) && (kind == DMR_EV_BURST_RX) && (burst != NULL)) {
		rx_burst(burst, arg, now_ms);
	}
}

static uint8_t alt_net_frame(uint8_t command, uint8_t control, const uint8_t *frame, uint32_t now_ms)
{
	if (sp.role == SPLIT_ROLE_TX) {
		return tx_net_frame(command, control, frame, now_ms);
	}
	return MMDVM_NAK_WRONG_MODE;
}

static void alt_net_abort(void)
{
	// SET_MODE idle, DMR_ABORT through hotspot.c: buffers dropped, keying ends after the burst on air
	if (sp.role == SPLIT_ROLE_TX) {
		bsOutboundDrop(&sp.out, 1);
		bsOutboundDrop(&sp.out, 2);
		tx_unkey();
		bsOutboundReset(&sp.out);
	}
}

static bool alt_transmitting(void)
{
	return (sp.role == SPLIT_ROLE_TX) && (sp.keyed || sp.want_key || bsOutboundBusy(&sp.out));
}

static void alt_stopped(void)
{
	// The modem is given back or retuned: the TX half's slot clock stops (restarted by the poll: a new epoch), the RX
	// half's calls end; the role stays (the session's end comes through ext_end)
	if (sp.role == SPLIT_ROLE_TX) {
		tx_stop_clock();
	} else if (sp.role == SPLIT_ROLE_RX) {
		rx_reset();
	}
}

static const hotspot_alt_t alt = {
	.active = alt_active, .poll = alt_poll, .event = alt_event, .net_frame = alt_net_frame,
	.net_abort = alt_net_abort, .transmitting = alt_transmitting, .stopped = alt_stopped,
	.event_arg = alt_event_arg,
};

// ---- Public ----

void hotspot_split_init(void)
{
	memset(&sp, 0, sizeof(sp));
	sp.lease_ms = SPLIT_LEASE_S * 1000u;
	sp.power_cap = SPLIT_POWER_CAP;
	sp.max_key_10s = SPLIT_MAX_KEY_10S;
	sp.cooldown_s = SPLIT_COOLDOWN_S;
	bsOutboundInit(&sp.out, BS_OUT_PREBUFFER);
	slotEpochInit(&sp.ep);
	for (int k = 0; k < 2; k++) {
		hotspotUplinkReset(&sp.rx[k].up);
	}
	hotspot_set_alt_priority(&alt);
	mmdvm_set_ext_priority(&ext);
}

void hotspot_split_set_variant(uint8_t variant, const uint8_t regs[][2], uint8_t nregs)
{
	sp.variant = variant;
	sp.nregs = (nregs > DMR_BS_REGS_MAX) ? DMR_BS_REGS_MAX : nregs;
	for (uint8_t i = 0; i < sp.nregs; i++) {
		sp.regs[i][0] = regs[i][0];
		sp.regs[i][1] = regs[i][1];
	}
	if (sp.variant_started == variant) {
		sp.variant_started = (uint8_t)~variant;	// raw registers changed: restart at the next key-up as well
	}
}

void hotspot_split_get_view(hotspot_split_view_t *v)
{
	const hal_dmr_modem_t *m = modem();

	memset(v, 0, sizeof(*v));
	v->role = sp.role;
	v->flags = flags_now();
	v->roles_offered = roles_offered();
	v->variant = sp.variant;
	v->cach_method = ((m != NULL) && (v->roles_offered & 1u)) ? m->bs_cach : DMR_BS_CACH_NONE;
	v->at_host = bsOutboundAtMask(&sp.out, sp.now_ms);
	v->at_on_air = sp.out.at_on_air;
	v->inbound_busy = inbound_busy();
	for (int k = 0; k < 2; k++) {
		v->buffered[k] = sp.out.slot[k].count;
		v->underruns[k] = sp.out.slot[k].underruns;
		v->labeled[k] = sp.labeled[k];
	}
	v->idle = sp.out.st.idle;
	v->cach = sp.out.st.cach;
	v->silence = sp.out.st.silence;
	v->closed = sp.out.st.closed;
	v->slc_taken = sp.slc_taken;
	v->slc_bad = sp.slc_bad;
	v->unlabeled = sp.unlabeled;
	v->residual_us = sp.ep.last_residual_us;
	v->track_us = sp.ep.track_us;
	v->epoch_us = sp.ep.epoch_us;
	v->period_ns = sp.ep.period_ns;
	v->epoch_age_s = sp.ep.valid ? (uint16_t)(((sp.now_ms - sp.ep.set_ms) / 1000u > 65534u) ? 65534u
										 : (sp.now_ms - sp.ep.set_ms) / 1000u)
				     : 65535u;
	v->keyed_s = sp.keyed ? (uint16_t)((sp.now_ms - sp.key_ms) / 1000u) : 0;
	v->lease_expiries = sp.lease_expiries;
	v->time_requests = sp.time_requests;
	v->stream = sp.stream;
	if (sp.role == SPLIT_ROLE_TX) {
		bool ok;

		v->align_flags = align_flags_now();
		v->phase_us = phase_now(&ok, NULL, NULL);
	}
	v->snap_adj_us = sp.snap_adj;
	v->cbase = sp.cbase;
	v->sch_sent = sp.sch_sent;
	v->sch_late = sp.sch_late;
	v->sch_refused = sp.sch_refused;
	v->sch_replaced = sp.sch_replaced;
	v->sch_cool = sp.sch_cool;
	v->slew_steps = sp.slew_steps;
	v->slew_us = sp.slew_us;
	v->meta_sent = sp.meta_seq;
}

#endif	// CONFIG_HOTSPOT_SPLIT
