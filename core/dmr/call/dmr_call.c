// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR voice call stack (see dmr_call.h). Two independent halves share this file: the RX side (rx_*: is a call
// on our slot, and do we play it) and the TX side (tx_*: from PTT to the modem's tx_done). RX bursts are ignored while
// the modem transmits (their status can't be trusted then, docs/spec/dmr-modem.md 6.5).
#include <string.h>
#include "dmr/call/dmr_call.h"

const uint8_t dmr_ambe_silence[DMR_AMBE_FRAME_BYTES] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };

#if (DMR_CALL_EVQ_DEPTH & (DMR_CALL_EVQ_DEPTH - 1)) != 0 || DMR_CALL_EVQ_DEPTH > 128
#error "DMR_CALL_EVQ_DEPTH must be a power of two up to 128"
#endif

enum { RXC_NONE, RXC_PLAY, RXC_OTHER };
enum { TX_NONE, TX_ACCESS, TX_WAKE, TX_WAKE_LISTEN, TX_ON, TX_END, TX_DENIED };

#define VOICE_PERIOD_MS		60	// one burst per frame on our slot
#define DUPLICATE_MS		40	// the same burst index again within this: a duplicate report

// --- Event queue: the modem callback is the only producer, dmr_call_poll() the only consumer ---

typedef struct {
	uint8_t kind;
	int8_t ts;
	bool has_burst;
	uint32_t arg;
	dmr_burst_t burst;
} evq_item_t;

static evq_item_t evq[DMR_CALL_EVQ_DEPTH];
static uint8_t evq_head;	// written by the producer
static uint8_t evq_tail;	// written by the consumer
static uint32_t evq_drops;
static dmr_call_stats_t stats;
static uint32_t evq_drops_base, tx_underruns_base;	// at the last dmr_call_reset_stats
static uint32_t tick_count;	// slot ticks, counted by the producer
static uint8_t tick_alt;	// consecutive ticks with the slot number alternating 1 / 2
static int8_t tick_ts;

// --- Stack state ---

static struct {
	const hal_dmr_modem_t *modem;
	const dmr_voice_sink_t *sink;
	const dmr_voice_source_t *source;
	dmr_call_config_t cfg;
	bool configured;
	bool running;
	uint32_t now;		// time of the current / last poll
	dmr_call_view_t view;
	bool dirty;

	bool ptt;
	bool ptt_seen;		// PTT state as last handled by the poll

	// RX
	uint8_t rx;
	uint8_t call_lc[DMR_LC_BYTES];
	int8_t call_ts;
	bool have_index;
	uint8_t next_index;
	uint32_t last_voice_ms;	// last voice burst of the call played
	uint32_t last_rx_ms;	// last burst of the call (played or not)
	uint32_t last_activity_ms;	// last non-idle burst on our slot with our CC
	bool activity_seen;
	uint32_t last_any_ms;	// last burst of any kind / CC (polite to all)
	bool any_seen;
	uint32_t last_call_end_ms;
	bool call_end_seen;
	uint8_t stale_lc[DMR_LC_BYTES];	// the previous call's LC (late entry must not take it for the new call)
	bool stale_valid;
	dmr_ta_t ta;
	// late entry
	bool le_active;
	bool le_pair;		// two consecutive voice bursts seen
	uint8_t le_last_index;
	uint32_t le_last_ms;
	uint8_t le_cand[DMR_LC_BYTES];
	bool le_cand_valid;

	// repeater outbound
	uint32_t rpt_last_ms;
	bool rpt_seen;
	uint32_t tick_seen;

	// TX
	uint8_t tx;
	uint8_t tx_lc[DMR_LC_BYTES];
	dmr_target_t tx_target_used;
	uint8_t headers_left;
	uint8_t voice_index;
	uint32_t queued;
	uint32_t started;
	uint8_t queue_cap;	// the modem's free queue space right after tx_begin
	bool stop_req;
	bool tot_hit;
	uint8_t stage[DMR_AMBE_FRAMES_PER_BURST][DMR_AMBE_FRAME_BYTES];
	uint8_t stage_n;
	uint32_t tx_since;	// PTT handling start (access wait)
	uint32_t deadline;
	uint32_t wake_sent_ms;
	uint8_t wake_tries;
	uint32_t tx_start_ms;

	// reply window
	dmr_target_t reply;
	uint32_t reply_until;
	bool reply_set;
} s;

static bool elapsed(uint32_t now, uint32_t since, bool valid, uint32_t ms)
{
	return !valid || (uint32_t)(now - since) >= ms;
}

static bool before(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) < 0;
}

static bool same_target(dmr_target_t a, dmr_target_t b)
{
	return a.id == b.id && a.private_call == b.private_call;
}

static uint16_t fade_ms(void)
{
	return s.cfg.fade_ms ? s.cfg.fade_ms : DMR_CALL_FADE_MS;
}

static bool reply_running(uint32_t now)
{
	return s.reply_set && s.cfg.reply_window_s && before(now, s.reply_until) &&
		!same_target(s.reply, s.cfg.talk_target);
}

static dmr_target_t effective_target(uint32_t now)
{
	return reply_running(now) ? s.reply : s.cfg.talk_target;
}

static void set_reply(dmr_target_t t, uint32_t now)
{
	if (!s.cfg.reply_window_s)
		return;
	s.reply = t;
	s.reply_until = now + (uint32_t)s.cfg.reply_window_s * 1000u;
	s.reply_set = true;
	s.dirty = true;
}

static bool transmitting(void)
{
	return s.tx == TX_WAKE || s.tx == TX_ON || s.tx == TX_END;
}

// --- Modem event callback (producer) ---

static void on_event(const dmr_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->kind == DMR_EV_SLOT_TICK) {
		int8_t ts = ev->ts;

		if ((ts == 1 || ts == 2) && tick_ts != 0 && ts != tick_ts) {
			if (tick_alt < 255)
				tick_alt++;
		} else {
			tick_alt = 0;
		}
		tick_ts = ts;
		__atomic_store_n(&tick_count, tick_count + 1, __ATOMIC_RELEASE);
		return;
	}

	uint8_t head = evq_head;
	uint8_t tail = __atomic_load_n(&evq_tail, __ATOMIC_ACQUIRE);

	if ((uint8_t)(head - tail) >= DMR_CALL_EVQ_DEPTH) {
		evq_drops++;
		return;
	}
	evq_item_t *it = &evq[head & (DMR_CALL_EVQ_DEPTH - 1)];

	it->kind = ev->kind;
	it->ts = ev->ts;
	it->arg = ev->arg;
	it->has_burst = ev->kind == DMR_EV_BURST_RX && ev->burst != NULL;
	if (it->has_burst)
		it->burst = *ev->burst;
	__atomic_store_n(&evq_head, (uint8_t)(head + 1), __ATOMIC_RELEASE);
}

static void evq_reset(void)
{
	evq_head = evq_tail = 0;
	tick_alt = 0;
	tick_ts = 0;
}

// --- RX side ---

static void sink_lost(uint8_t frames)
{
	stats.rx_lost_frames += frames;
	if (s.sink && s.sink->lost)
		while (frames--)
			s.sink->lost(s.sink->ctx);
}

static void le_reset(void)
{
	s.le_active = false;
	s.le_pair = false;
	s.le_cand_valid = false;
}

static bool group_listened(uint32_t tg, uint32_t now)
{
	const dmr_listen_filter_t *f = &s.cfg.listen;

	if (f->hear_all || tg == DMR_ID_ALL_CALL)
		return true;
	if (!s.cfg.talk_target.private_call && tg == s.cfg.talk_target.id)
		return true;
	if (reply_running(now) && !s.reply.private_call && tg == s.reply.id)
		return true;
	for (uint16_t i = 0; f->groups && i < f->group_count; i++)
		if (f->groups[i] == tg)
			return true;
	return f->group_ok && f->group_ok(tg, f->group_ctx);
}

static bool rx_accept(const dmr_lc_t *l, uint32_t now)
{
	if (l->src == s.cfg.own_id)
		return false;	// our own call echoed (repeater)
	if (l->flco == DMR_FLCO_GROUP_VOICE)
		return group_listened(l->dst, now);
	return l->dst == s.cfg.own_id || l->dst == DMR_ID_ALL_CALL || s.cfg.listen.hear_all;
}

static void rx_end(uint32_t now)
{
	if (s.rx == RXC_NONE)
		return;
	if (s.rx == RXC_PLAY) {
		if (s.sink && s.sink->end)
			s.sink->end(s.sink->ctx);
		s.view.last_heard = s.view.caller;
		s.view.last_heard_ms = now;
		dmr_target_t t = { .id = s.view.caller.group ? s.view.caller.dst : s.view.caller.src,
				   .private_call = !s.view.caller.group };
		if (s.view.caller.group || s.view.caller.to_us)
			set_reply(t, now);
	}
	memcpy(s.stale_lc, s.call_lc, DMR_LC_BYTES);
	s.stale_valid = true;
	s.last_call_end_ms = now;
	s.call_end_seen = true;
	s.rx = RXC_NONE;
	s.have_index = false;
	le_reset();
	s.dirty = true;
}

// A voice LC (header, or embedded) for our slot: a new call, or a new talker on the call being heard
static void rx_lc(const uint8_t lc[DMR_LC_BYTES], const dmr_burst_t *b, bool late, uint32_t now)
{
	dmr_lc_t l;

	if (!dmr_lc_is_voice(lc))
		return;
	if (s.rx != RXC_NONE && memcmp(lc, s.call_lc, DMR_LC_BYTES) == 0)
		return;
	dmr_lc_unpack(lc, &l);

	bool continuing = s.rx != RXC_NONE;
	bool new_talker = !continuing || l.src != s.view.caller.src;

	if (s.rx == RXC_PLAY && s.sink && s.sink->end)
		s.sink->end(s.sink->ctx);

	memcpy(s.call_lc, lc, DMR_LC_BYTES);
	s.call_ts = b->ts;
	s.last_rx_ms = now;
	if (new_talker)
		dmr_ta_reset(&s.ta);

	dmr_caller_t *c = &s.view.caller;
	c->src = l.src;
	c->dst = l.dst;
	c->group = l.flco == DMR_FLCO_GROUP_VOICE;
	c->to_us = !c->group && l.dst == s.cfg.own_id;
	c->fid = l.fid;
	c->svc = l.svc;
	c->encrypted = (l.svc & DMR_SVC_PRIVACY) != 0;
	c->late_entry = late;
	memcpy(c->alias, s.ta.text, sizeof(c->alias));
	c->alias_complete = s.ta.complete;
	if (!continuing) {
		s.view.call_start_ms = now;
		stats.rx_calls++;
		if (late)
			stats.rx_late_entries++;
	}
	if (s.cfg.listen.any_cc)
		s.view.cc = b->cc;

	s.rx = rx_accept(&l, now) ? RXC_PLAY : RXC_OTHER;
	if (s.rx == RXC_PLAY && !continuing)
		stats.rx_calls_played++;
	if (s.rx == RXC_PLAY && s.sink && s.sink->begin)
		s.sink->begin(c, s.sink->ctx);
	le_reset();
	s.dirty = true;
}

static void rx_play(const dmr_burst_t *b, uint32_t now)
{
	uint8_t i = b->voice_index % 6;

	if (s.have_index) {
		uint8_t missing = (uint8_t)((i + 6 - s.next_index) % 6);

		if (missing == 5 && (uint32_t)(now - s.last_voice_ms) < DUPLICATE_MS) {
			stats.rx_dup_dropped++;
			return;	// the same burst reported twice
		}
		sink_lost((uint8_t)(missing * DMR_AMBE_FRAMES_PER_BURST));
	}
	s.next_index = (uint8_t)((i + 1) % 6);
	s.have_index = true;
	s.last_voice_ms = now;

	bool enc = b->pi || (s.view.caller.svc & DMR_SVC_PRIVACY);

	if (enc && !s.view.caller.encrypted) {
		s.view.caller.encrypted = true;
		s.dirty = true;
	}
	if (enc && !s.cfg.privacy) {
		stats.rx_enc_dropped++;
		sink_lost(DMR_AMBE_FRAMES_PER_BURST);	// no key: never play the scrambled voice
		return;
	}

	uint8_t v[DMR_VOICE_BYTES];

	memcpy(v, b->payload, DMR_VOICE_BYTES);
	if (enc)
		s.cfg.privacy(v, i, false, s.cfg.privacy_ctx);
	stats.rx_voice_bursts++;
	if (s.sink && s.sink->frame)
		for (int f = 0; f < DMR_AMBE_FRAMES_PER_BURST; f++)
			s.sink->frame(&v[f * DMR_AMBE_FRAME_BYTES], s.sink->ctx);
}

// Voice with no call: start one from the embedded LC once two consecutive bursts were seen (a lone burst is noise)
// and the LC is not the previous call's, or is the same in two superframes (docs/spec/dmr-modem.md 2.4)
static void rx_late_entry(const dmr_burst_t *b, uint32_t now)
{
	uint8_t i = b->voice_index % 6;

	if (!s.le_active || elapsed(now, s.le_last_ms, true, fade_ms())) {
		le_reset();
		s.le_active = true;
	} else if (i == (s.le_last_index + 1) % 6) {
		s.le_pair = true;
	}
	s.le_last_index = i;
	s.le_last_ms = now;

	if (!b->lc_valid || !dmr_lc_is_voice(b->lc))
		return;

	bool fresh = !s.stale_valid || memcmp(b->lc, s.stale_lc, DMR_LC_BYTES) != 0;
	bool confirmed = s.le_cand_valid && memcmp(b->lc, s.le_cand, DMR_LC_BYTES) == 0;
	bool pair = s.le_pair;

	memcpy(s.le_cand, b->lc, DMR_LC_BYTES);
	s.le_cand_valid = true;
	if (!pair || !(fresh || confirmed))
		return;
	rx_lc(b->lc, b, true, now);
	s.have_index = false;
	if (s.rx == RXC_PLAY)
		rx_play(b, now);
}

static void rx_embedded(const dmr_burst_t *b, uint32_t now)
{
	uint8_t flco = b->lc[0] & 0x3F;

	if (dmr_lc_is_voice(b->lc)) {
		if (memcmp(b->lc, s.call_lc, DMR_LC_BYTES) != 0) {
			rx_lc(b->lc, b, true, now);	// a new talker without a header (repeater)
			s.have_index = false;
		}
	} else if (flco >= DMR_FLCO_TA_HEADER && flco <= DMR_FLCO_TA_BLOCK3) {
		if (dmr_ta_push(&s.ta, b->lc)) {
			memcpy(s.view.caller.alias, s.ta.text, sizeof(s.view.caller.alias));
			s.view.caller.alias_complete = s.ta.complete;
			s.dirty = true;
		}
	}
}

static bool rx_header_lc(const dmr_burst_t *b, uint8_t mask, uint8_t lc[DMR_LC_BYTES])
{
	// The LC from the burst's own info bits (the modem's LC buffer may still hold the previous call, gotcha 1.4)
	if (b->len >= DMR_LC_FULL_BYTES)
		return dmr_lc_decode_full(b->payload, mask, lc) >= 0 || dmr_lc_decode_full(b->payload, 0, lc) >= 0;
	if (b->lc_valid && b->crc_ok) {
		memcpy(lc, b->lc, DMR_LC_BYTES);
		return true;
	}
	return false;
}

static void rx_burst(const dmr_burst_t *b, uint32_t now)
{
	if (transmitting())
		return;
	s.last_any_ms = now;
	s.any_seen = true;

	if (!s.cfg.listen.any_cc && b->cc != s.cfg.cc)
		return;
	if (b->sync == DMR_SYNC_BS) {
		s.rpt_last_ms = now;
		s.rpt_seen = true;
	}
	// The slot number is only known through a repeater (CACH) or from the TDMA direct mode sync
	bool ts_known = b->ts != 0 && (s.cfg.rmo || b->sync == DMR_SYNC_BS || b->sync == DMR_SYNC_TS1 ||
				       b->sync == DMR_SYNC_TS2);
	if (ts_known && b->ts != s.cfg.ts)
		return;
	if (s.rx != RXC_NONE && b->ts != 0 && s.call_ts != 0 && b->ts != s.call_ts)
		return;
	if (b->kind != DMR_BURST_IDLE) {
		s.last_activity_ms = now;
		s.activity_seen = true;
	}
	if (b->rssi_dbm != 0 && b->rssi_dbm != s.view.rssi_dbm) {
		int d = b->rssi_dbm - s.view.rssi_dbm;

		if (d >= 2 || d <= -2)
			s.dirty = true;
		s.view.rssi_dbm = b->rssi_dbm;
	}

	uint8_t lc[DMR_LC_BYTES];

	switch (b->kind) {
	case DMR_BURST_VOICE_LC_HEADER:
		if (rx_header_lc(b, DMR_RS_MASK_VOICE_HEADER, lc)) {
			rx_lc(lc, b, false, now);
			if (s.rx != RXC_NONE) {
				s.last_rx_ms = now;
				s.have_index = true;
				s.next_index = 0;
				s.last_voice_ms = now;
			}
		}
		break;
	case DMR_BURST_TERMINATOR_LC:
		if (s.rx != RXC_NONE && rx_header_lc(b, DMR_RS_MASK_TERMINATOR, lc)) {
			dmr_lc_t l;

			dmr_lc_unpack(lc, &l);
			if (l.src == s.cfg.own_id)
				break;	// our own terminator echoed by the repeater
			// Through a repeater only the call's own terminator ends it (the timeout does the rest)
			if (s.cfg.rmo && memcmp(lc, s.call_lc, DMR_LC_BYTES) != 0)
				break;
			rx_end(now);
		}
		break;
	case DMR_BURST_VOICE:
		if (s.rx == RXC_NONE) {
			rx_late_entry(b, now);
			break;
		}
		s.last_rx_ms = now;
		if (b->lc_valid)
			rx_embedded(b, now);
		if (s.rx == RXC_PLAY)
			rx_play(b, now);
		break;
	default:
		if (s.rx != RXC_NONE && b->kind != DMR_BURST_IDLE && b->kind != DMR_BURST_PI_HEADER)
			s.last_rx_ms = now;
		break;
	}
}

// --- TX side ---

static void source_end(void)
{
	if (s.source && s.source->end)
		s.source->end(s.source->ctx);
}

static void tx_deny(uint8_t err)
{
	stats.tx_denied++;
	if (s.tx == TX_WAKE || s.tx == TX_ON || s.tx == TX_END)
		s.modem->tx_abort();
	if (s.tx == TX_ON)
		source_end();
	s.tx = s.ptt ? TX_DENIED : TX_NONE;
	s.view.tx_error = err;
	s.dirty = true;
}

static bool repeater_up(uint32_t now)
{
	return s.rpt_seen && !elapsed(now, s.rpt_last_ms, true, DMR_CALL_REPEATER_HOLD_MS);
}

static bool voice_channel_busy(uint32_t now)
{
	switch (s.cfg.access) {
	case DMR_ACCESS_POLITE_CC:
		return s.rx != RXC_NONE;
	case DMR_ACCESS_POLITE_ALL:
		return s.rx != RXC_NONE || !elapsed(now, s.last_any_ms, s.any_seen, DMR_CALL_LBT_DMO_MS);
	default:
		return false;
	}
}

static uint32_t tx_inflight(void)
{
	uint32_t n = s.queued - s.started;
	uint8_t space = s.modem->tx_queue_space();

	if (s.queue_cap >= space && (uint32_t)(s.queue_cap - space) < n)
		n = (uint32_t)(s.queue_cap - space);	// a driver that doesn't report every TX slot
	return n;
}

static bool tx_put(uint8_t kind, uint8_t index, bool host_crc, const uint8_t *payload, uint8_t len)
{
	dmr_tx_burst_t tb;

	memset(&tb, 0, sizeof(tb));
	tb.kind = kind;
	tb.voice_index = index;
	tb.host_crc = host_crc;
	tb.sync_override = DMR_SYNC_NONE;
	tb.len = len;
	memcpy(tb.payload, payload, len);
	if (!s.modem->tx_queue(&tb))
		return false;
	s.queued++;
	return true;
}

static void tx_fill(uint32_t now)
{
	while (s.tx == TX_ON) {
		uint32_t inflight = tx_inflight();

		if (inflight >= DMR_CALL_TX_MAX_INFLIGHT || s.modem->tx_queue_space() == 0)
			return;
		if (s.headers_left) {
			uint8_t full[DMR_LC_FULL_BYTES];

			dmr_lc_encode_full(s.tx_lc, DMR_RS_MASK_VOICE_HEADER, full);
			if (!tx_put(DMR_BURST_VOICE_LC_HEADER, 0, true, full, sizeof(full)))
				return;
			s.headers_left--;
			continue;
		}
		if (s.stop_req && s.voice_index == 0) {
			// Superframe boundary: terminator, then let the modem drain (tx_done)
			uint8_t full[DMR_LC_FULL_BYTES];

			dmr_lc_encode_full(s.tx_lc, DMR_RS_MASK_TERMINATOR, full);
			for (int t = 0; t < DMR_CALL_TX_TERMINATORS; t++)
				tx_put(DMR_BURST_TERMINATOR_LC, 0, true, full, sizeof(full));
			s.modem->tx_end();
			source_end();
			s.tx = TX_END;
			s.deadline = now + (inflight + DMR_CALL_TX_TERMINATORS) * VOICE_PERIOD_MS + DMR_CALL_TX_DONE_GUARD_MS;
			s.dirty = true;
			return;
		}
		while (s.stage_n < DMR_AMBE_FRAMES_PER_BURST && s.source && s.source->next &&
		       s.source->next(s.stage[s.stage_n], s.source->ctx))
			s.stage_n++;
		if (s.stage_n < DMR_AMBE_FRAMES_PER_BURST && inflight > 1)
			return;	// not due yet: wait for the encoder
		while (s.stage_n < DMR_AMBE_FRAMES_PER_BURST) {
			memcpy(s.stage[s.stage_n++], dmr_ambe_silence, DMR_AMBE_FRAME_BYTES);
			s.view.tx_underruns++;
		}

		uint8_t v[DMR_VOICE_BYTES];

		memcpy(v, s.stage, DMR_VOICE_BYTES);
		if (s.cfg.privacy && (s.cfg.tx_svc & DMR_SVC_PRIVACY))
			s.cfg.privacy(v, s.voice_index, true, s.cfg.privacy_ctx);
		if (!tx_put(DMR_BURST_VOICE, s.voice_index, false, v, DMR_VOICE_BYTES))
			return;	// keep the staged frames for the next try
		s.stage_n = 0;
		s.voice_index = (uint8_t)((s.voice_index + 1) % 6);
	}
}

static void tx_start_voice(uint32_t now)
{
	rx_end(now);	// keying over a call (impolite): it ends here, and its caller becomes the reply target

	dmr_target_t t = effective_target(now);
	dmr_lc_t l = {
		.flco = t.private_call ? DMR_FLCO_UNIT_VOICE : DMR_FLCO_GROUP_VOICE,
		.fid = s.cfg.tx_fid,
		.svc = s.cfg.tx_svc,
		.dst = t.id & DMR_ID_MAX,
		.src = s.cfg.own_id & DMR_ID_MAX,
	};
	dmr_timing_t timing;

	if (s.cfg.rmo)
		timing = DMR_TIMING_ALIGNED;
	else	// DMO: answer in step with a station just heard, else on our own timing
		timing = elapsed(now, s.last_activity_ms, s.activity_seen, DMR_CALL_ALIGN_MS) ? DMR_TIMING_OWN
											    : DMR_TIMING_ALIGNED;
	// Our own check is done; through a repeater the modem must not refuse because of the hang time (gotcha 1.8)
	dmr_access_t access = s.cfg.rmo ? DMR_ACCESS_IMPOLITE : s.cfg.access;

	if (!s.modem->tx_begin(timing, (int8_t)s.cfg.ts, access)) {
		tx_deny(DMR_TX_ERR_REJECTED);
		return;
	}
	dmr_lc_pack(&l, s.tx_lc);
	s.modem->set_embedded_lc(s.tx_lc);
	s.queue_cap = s.modem->tx_queue_space();
	s.headers_left = DMR_CALL_TX_HEADERS;
	s.voice_index = 0;
	s.queued = s.started = 0;
	s.stop_req = false;
	s.tot_hit = false;
	s.stage_n = 0;
	s.tx = TX_ON;
	stats.tx_calls++;
	s.tx_start_ms = now;
	s.tx_target_used = t;
	s.view.tx_error = DMR_TX_OK;

	dmr_caller_t *c = &s.view.caller;
	memset(c, 0, sizeof(*c));
	c->src = l.src;
	c->dst = l.dst;
	c->group = !t.private_call;
	c->fid = l.fid;
	c->svc = l.svc;
	c->encrypted = (l.svc & DMR_SVC_PRIVACY) != 0;
	s.view.call_start_ms = now;
	s.dirty = true;

	if (s.source && s.source->begin)
		s.source->begin(s.source->ctx);
	tx_fill(now);
}

static void tx_wakeup(uint32_t now)
{
	uint8_t csbk[12];

	dmr_csbk_bs_activation(s.cfg.own_id, csbk);
	if (!s.modem->tx_begin(DMR_TIMING_ASAP, (int8_t)s.cfg.ts, DMR_ACCESS_IMPOLITE)) {
		tx_deny(DMR_TX_ERR_REJECTED);
		return;
	}
	s.queue_cap = s.modem->tx_queue_space();
	s.queued = s.started = 0;
	tx_put(DMR_BURST_CSBK, 0, true, csbk, sizeof(csbk));
	s.modem->tx_end();
	s.wake_tries++;
	s.wake_sent_ms = now;
	s.tx = TX_WAKE;
	s.deadline = now + DMR_CALL_TX_DONE_GUARD_MS;
	s.dirty = true;
}

static void tx_access(uint32_t now)
{
	if (voice_channel_busy(now)) {
		if (elapsed(now, s.tx_since, true, s.cfg.access_wait_ms))
			tx_deny(DMR_TX_ERR_BUSY);
		return;
	}
	if (s.cfg.rmo && !repeater_up(now))
		tx_wakeup(now);
	else
		tx_start_voice(now);
}

static void tx_ptt_pressed(uint32_t now)
{
	if (s.tx != TX_NONE)
		return;		// a transmission is ending: tx_finished() restarts if PTT is still held
	if (!s.modem || !s.running || !s.configured) {
		s.tx = TX_DENIED;
		s.view.tx_error = DMR_TX_ERR_NO_MODEM;
		s.dirty = true;
		return;
	}
	s.view.tx_error = DMR_TX_OK;
	s.tx_since = now;
	s.wake_tries = 0;
	s.tx = TX_ACCESS;
	s.dirty = true;
	tx_access(now);
}

static void tx_ptt_released(void)
{
	switch (s.tx) {
	case TX_ACCESS:
	case TX_WAKE_LISTEN:
	case TX_DENIED:
		s.tx = TX_NONE;
		break;
	case TX_WAKE:
		s.modem->tx_abort();
		s.tx = TX_NONE;
		break;
	case TX_ON:
		s.stop_req = true;	// at the next superframe boundary
		break;
	default:
		break;
	}
	s.dirty = true;
}

static void tx_finished(uint32_t now)
{
	s.tx = TX_NONE;
	set_reply(s.tx_target_used, now);
	s.last_call_end_ms = now;
	s.call_end_seen = true;
	s.dirty = true;
	if (s.tot_hit) {
		s.tx = s.ptt ? TX_DENIED : TX_NONE;
		s.view.tx_error = DMR_TX_ERR_TIMEOUT;
	} else if (s.ptt) {
		tx_ptt_pressed(now);	// pressed again while the last one was ending
	}
}

static void tx_poll(uint32_t now)
{
	switch (s.tx) {
	case TX_ACCESS:
		tx_access(now);
		break;
	case TX_WAKE:
		if (!before(now, s.deadline)) {	// tx_done never came: listen anyway
			s.tx = TX_WAKE_LISTEN;
			s.deadline = now + DMR_CALL_WAKEUP_LISTEN_MS;
		}
		break;
	case TX_WAKE_LISTEN:
		if (s.rpt_seen && (int32_t)(s.rpt_last_ms - s.wake_sent_ms) >= 0 && repeater_up(now)) {
			s.tx = TX_ACCESS;
			s.dirty = true;
			tx_access(now);
		} else if (!before(now, s.deadline)) {
			uint8_t tries = s.cfg.wakeup_attempts ? s.cfg.wakeup_attempts : DMR_CALL_WAKEUP_ATTEMPTS;

			if (s.wake_tries < tries)
				tx_wakeup(now);
			else
				tx_deny(DMR_TX_ERR_NO_REPEATER);
		}
		break;
	case TX_ON:
		if (s.cfg.tot_s && !s.stop_req && (uint32_t)(now - s.tx_start_ms) >= (uint32_t)s.cfg.tot_s * 1000u) {
			s.stop_req = true;
			s.tot_hit = true;
		}
		tx_fill(now);
		break;
	case TX_END:
		if (!before(now, s.deadline)) {
			s.modem->tx_abort();	// tx_done overdue
			tx_finished(now);
		}
		break;
	default:
		break;
	}
}

// --- Events (consumer) ---

static void handle_event(const evq_item_t *it, uint32_t now)
{
	switch (it->kind) {
	case DMR_EV_BURST_RX:
		if (it->has_burst)
			rx_burst(&it->burst, now);
		break;
	case DMR_EV_VOICE_LOST:
		if (s.rx == RXC_PLAY && !transmitting() && (it->ts == 0 || s.call_ts == 0 || it->ts == s.call_ts)) {
			sink_lost(DMR_AMBE_FRAMES_PER_BURST);
			if (s.have_index)
				s.next_index = (uint8_t)((s.next_index + 1) % 6);
		}
		break;
	case DMR_EV_TX_SLOT_STARTED:
		if (transmitting())
			s.started++;
		break;
	case DMR_EV_TX_REJECTED:
		if (transmitting())
			tx_deny(DMR_TX_ERR_REJECTED);
		break;
	case DMR_EV_TX_DONE:
		if (s.tx == TX_END) {
			tx_finished(now);
		} else if (s.tx == TX_WAKE) {
			s.tx = TX_WAKE_LISTEN;
			s.deadline = now + DMR_CALL_WAKEUP_LISTEN_MS;
		}
		break;
	case DMR_EV_SYNC_LOST:
		if (!transmitting())
			rx_end(now);
		break;
	default:
		break;
	}
}

// --- API ---

void dmr_call_config_default(dmr_call_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->cc = 1;
	cfg->ts = 1;
	cfg->talk_target.id = 9;
	cfg->access = DMR_ACCESS_POLITE_CC;
	cfg->reply_window_s = DMR_CALL_REPLY_WINDOW_S;
	cfg->wakeup_attempts = DMR_CALL_WAKEUP_ATTEMPTS;
	cfg->fade_ms = DMR_CALL_FADE_MS;
}

void dmr_call_init(const hal_dmr_modem_t *modem)
{
	memset(&s, 0, sizeof(s));
	evq_reset();
	evq_drops = 0;
	memset(&stats, 0, sizeof(stats));
	evq_drops_base = tx_underruns_base = 0;
	tick_count = 0;
	s.modem = modem;
	dmr_call_config_default(&s.cfg);
	s.view.cc = s.cfg.cc;
	s.view.ts = s.cfg.ts;
}

void dmr_call_set_voice(const dmr_voice_sink_t *sink, const dmr_voice_source_t *source)
{
	s.sink = sink;
	s.source = source;
}

static void modem_config(dmr_config_t *mc)
{
	mc->own_id = s.cfg.own_id;
	mc->cc = (s.cfg.listen.any_cc || s.cfg.access == DMR_ACCESS_POLITE_ALL) ? -1 : (int8_t)s.cfg.cc;
	mc->access = s.cfg.access;
	mc->repeater = s.cfg.rmo;
}

static void modem_apply(void)
{
	dmr_config_t mc;

	modem_config(&mc);
	s.modem->configure(&mc);
	s.modem->set_cc_filter(mc.cc);
	s.modem->rx_lock_slot(s.cfg.rmo ? (int8_t)s.cfg.ts : 0);
}

void dmr_call_configure(const dmr_call_config_t *cfg)
{
	bool rf_change = !s.configured || cfg->cc != s.cfg.cc || cfg->ts != s.cfg.ts || cfg->rmo != s.cfg.rmo ||
			 cfg->listen.any_cc != s.cfg.listen.any_cc || cfg->access != s.cfg.access ||
			 cfg->own_id != s.cfg.own_id;
	bool target_change = !same_target(cfg->talk_target, s.cfg.talk_target);

	if (rf_change && s.configured) {
		if (transmitting() && s.modem)
			s.modem->tx_abort();
		if (s.tx == TX_ON)
			source_end();
		if (s.tx != TX_NONE)
			s.tx = s.ptt ? TX_DENIED : TX_NONE;
		rx_end(s.now);
		s.rpt_seen = false;
		s.stale_valid = false;
	}
	s.cfg = *cfg;
	if (s.cfg.ts != 2)
		s.cfg.ts = 1;
	s.cfg.cc &= 0x0F;
	if (s.cfg.reply_window_s > 30)
		s.cfg.reply_window_s = 30;
	if (rf_change || target_change)
		s.reply_set = false;	// temporary state ends with the channel (docs/ux 1)
	s.configured = true;
	s.view.cc = s.cfg.cc;
	s.view.ts = s.cfg.ts;
	s.view.rmo = s.cfg.rmo;
	s.dirty = true;
	if (s.running && rf_change)
		modem_apply();
}

bool dmr_call_start(void)
{
	dmr_config_t mc;

	if (!s.modem)
		return false;
	if (s.running)
		return true;
	evq_reset();
	modem_config(&mc);
	if (!s.modem->start(&mc, on_event, NULL))
		return false;
	s.running = true;
	s.modem->set_cc_filter(mc.cc);
	s.modem->rx_lock_slot(s.cfg.rmo ? (int8_t)s.cfg.ts : 0);
	s.dirty = true;
	return true;
}

void dmr_call_stop(void)
{
	if (!s.running)
		return;
	if (transmitting())
		s.modem->tx_abort();
	if (s.tx == TX_ON)
		source_end();
	s.tx = TX_NONE;
	rx_end(s.now);
	s.modem->stop();
	s.running = false;
	evq_reset();
	s.dirty = true;
}

void dmr_call_ptt(bool down)
{
	s.ptt = down;
}

void dmr_call_reply_cancel(void)
{
	s.reply_set = false;
	s.dirty = true;
}

static void view_update(uint32_t now)
{
	dmr_call_view_t *v = &s.view;
	uint8_t state, phase;

	if (transmitting() || s.tx == TX_WAKE_LISTEN)
		state = DMR_CALL_STATE_TX;
	else
		state = s.rx != RXC_NONE ? DMR_CALL_STATE_RX : DMR_CALL_STATE_IDLE;

	switch (s.tx) {
	case TX_ACCESS:
		phase = DMR_CALL_PHASE_TX_ACCESS;
		break;
	case TX_WAKE:
	case TX_WAKE_LISTEN:
		phase = DMR_CALL_PHASE_TX_WAKEUP;
		break;
	case TX_ON:
		phase = DMR_CALL_PHASE_TX_VOICE;
		break;
	case TX_END:
		phase = DMR_CALL_PHASE_TX_ENDING;
		break;
	case TX_DENIED:
		phase = DMR_CALL_PHASE_TX_DENIED;
		break;
	default:
		phase = s.rx == RXC_PLAY ? DMR_CALL_PHASE_RX_VOICE
					 : (s.rx == RXC_OTHER ? DMR_CALL_PHASE_RX_BUSY : DMR_CALL_PHASE_IDLE);
		break;
	}

	bool reply = reply_running(now);
	uint8_t reply_s = reply ? (uint8_t)((s.reply_until - now + 999u) / 1000u) : 0;
	dmr_target_t target = effective_target(now);
	bool rpt = s.cfg.rmo && repeater_up(now);
	uint32_t call_ms = 0;

	if ((state != DMR_CALL_STATE_IDLE) && (s.rx != RXC_NONE || s.tx == TX_ON || s.tx == TX_END))
		call_ms = now - v->call_start_ms;

	if (state != v->state || phase != v->phase || reply != v->reply_active || reply_s != v->reply_s ||
	    !same_target(target, v->tx_target) || rpt != v->repeater_up || call_ms / 1000u != v->call_ms / 1000u ||
	    evq_drops != v->evq_overflows)
		s.dirty = true;
	v->state = state;
	v->phase = phase;
	v->reply_active = reply;
	v->reply_s = reply_s;
	v->tx_target = target;
	v->repeater_up = rpt;
	v->call_ms = call_ms;
	v->evq_overflows = evq_drops;
	if (s.dirty) {
		v->serial++;
		s.dirty = false;
	}
}

void dmr_call_poll(uint32_t now_ms)
{
	s.now = now_ms;
	if (s.running) {
		uint8_t head = __atomic_load_n(&evq_head, __ATOMIC_ACQUIRE);

		while (evq_tail != head) {
			handle_event(&evq[evq_tail & (DMR_CALL_EVQ_DEPTH - 1)], now_ms);
			__atomic_store_n(&evq_tail, (uint8_t)(evq_tail + 1), __ATOMIC_RELEASE);
		}

		uint32_t tc = __atomic_load_n(&tick_count, __ATOMIC_ACQUIRE);

		if (tc != s.tick_seen) {
			s.tick_seen = tc;
			if (s.cfg.rmo && tick_alt > 3) {	// slot number toggling: a repeater's CACH
				s.rpt_last_ms = now_ms;
				s.rpt_seen = true;
			}
		}
	}

	// Fade: no burst on the call for a while ends it
	if (s.rx != RXC_NONE && elapsed(now_ms, s.last_rx_ms, true, fade_ms()))
		rx_end(now_ms);
	if (s.le_active && elapsed(now_ms, s.le_last_ms, true, fade_ms()))
		le_reset();

	if (s.ptt != s.ptt_seen) {
		s.ptt_seen = s.ptt;
		if (s.ptt)
			tx_ptt_pressed(now_ms);
		else
			tx_ptt_released();
	}
	if (s.running)
		tx_poll(now_ms);
	view_update(now_ms);
}

const dmr_call_view_t *dmr_call_view(void)
{
	return &s.view;
}

bool dmr_call_channel_clear(uint32_t now_ms)
{
	if (transmitting() || s.rx != RXC_NONE)
		return false;
	if (!s.cfg.rmo)
		return elapsed(now_ms, s.last_activity_ms, s.activity_seen, DMR_CALL_LBT_DMO_MS);
	return elapsed(now_ms, s.last_activity_ms, s.activity_seen, DMR_CALL_LBT_RMO_MS) &&
	       elapsed(now_ms, s.last_call_end_ms, s.call_end_seen, DMR_CALL_RMO_HANG_MS);
}

void dmr_call_get_stats(dmr_call_stats_t *out)
{
	*out = stats;
	out->tx_underruns = s.view.tx_underruns - tx_underruns_base;
	out->evq_overflows = evq_drops - evq_drops_base;
}

void dmr_call_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
	tx_underruns_base = s.view.tx_underruns;
	evq_drops_base = evq_drops;
}
