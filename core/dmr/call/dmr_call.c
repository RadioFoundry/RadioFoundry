// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR voice call stack (see dmr_call.h). Two independent halves share this file: the RX side (rx_*: is a call
// on our slot, and do we play it) and the TX side (tx_*: from PTT to the modem's tx_done). RX bursts are ignored while
// the modem transmits (their status can't be trusted then, docs/spec/dmr-modem.md 6.5).
#include <string.h>
#include "dmr/call/dmr_call.h"
#include "config.h"

// RX trace into the debug link's log (see dmr_call.h). Host tests that don't link dlog build with DMR_RX_TRACE=0
#ifndef DMR_RX_TRACE
#define DMR_RX_TRACE	CONFIG_DEBUGLINK
#endif
#if DMR_RX_TRACE
#include "debuglink/dlog.h"
#endif

const uint8_t dmr_ambe_silence[DMR_AMBE_FRAME_BYTES] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };

#if (DMR_CALL_EVQ_DEPTH & (DMR_CALL_EVQ_DEPTH - 1)) != 0 || DMR_CALL_EVQ_DEPTH > 128
#error "DMR_CALL_EVQ_DEPTH must be a power of two up to 128"
#endif

enum { RXC_NONE, RXC_PLAY, RXC_OTHER };
enum { TX_NONE, TX_ACCESS, TX_WAKE, TX_WAKE_LISTEN, TX_ON, TX_END, TX_DENIED, TX_DATA, TX_DATA_END };

#define VOICE_PERIOD_MS		60	// one burst per frame on our slot
#define DUPLICATE_MS		40	// no slot counter from the modem: the same burst index again within this is a repeat
#define SUPERFRAME_SLOTS	12	// with the slot counter: the same index again sooner than this is a repeat

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
	uint32_t last_voice_ms;	// last voice burst of the call played (arrival)
	uint32_t last_voice_slot;	// ... its slot counter from the modem, 0 = none
	uint32_t last_rx_ms;	// last burst of the call (played or not)
	uint32_t last_activity_ms;	// last non-idle burst on our slot with our CC
	bool activity_seen;
	uint32_t last_any_ms;	// last burst of any kind / CC (polite to all)
	bool any_seen;
	uint32_t last_call_end_ms;
	bool call_end_seen;
	uint8_t term_lc[DMR_LC_BYTES];	// the LC of the last call a terminator ended, and when
	uint32_t term_ms;
	bool term_seen;
	dmr_ta_t ta;
	// late entry
	bool le_active;
	bool le_pair;		// two consecutive voice bursts seen
	uint8_t le_last_index;
	uint32_t le_last_ms;
	bool le_held;		// le_burst: the last burst, its LC taken before the pair was complete
	dmr_burst_t le_burst;
	uint32_t le_burst_slot;
	// PI header heard before the call was (late entry after a missed voice LC header)
	bool pi_pending;
	uint32_t pi_pending_ms;
	uint8_t pi_pending_alg, pi_pending_key;

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
	uint32_t tx_progress_ms;	// the modem last took a burst (queued or started): none for long and it has stalled
	// talker alias (and GPS Info) in alternate superframes
	uint8_t tx_ta[DMR_TA_LC_MAX + 1][DMR_LC_BYTES];	// the alias LCs, then our GPS Info
	uint8_t tx_ta_n;	// LCs in the rotation (alias and GPS)
	uint8_t tx_gps_at;	// tx_ta index of the GPS Info, 0xFF none
	uint8_t tx_sf;		// superframes started in this transmission
	uint8_t tx_emb_cur;	// what the modem spreads now: 0 the voice LC, 1 + n rotation LC n
	bool tx_gps_set;	// dmr_call_set_tx_gps
	dmr_gps_t tx_gps;

	// reply window
	dmr_target_t reply;
	uint32_t reply_until;
	bool reply_set;

	// data (dmr_call_data_tx): the list being sent, and where received data bursts go
	dmr_data_source_t data;
	bool data_active;	// a list is being sent or its repeater woken (TX_WAKE / TX_WAKE_LISTEN / TX_DATA*)
	bool data_staged;	// data_stage taken from the list but not queued yet
	dmr_tx_burst_t data_stage;
	dmr_data_rx_fn data_rx;
	void *data_rx_ctx;
} s;

// --- RX trace: one entry per burst event, packed into lines for the debug link's log ---
//
//   crx @<slot> <entry> <entry> ...
//   entry = <burst><ts><what>[<frames concealed before it>][/<slots since the previous entry>]
//   burst: A-F voice; data by type (never A-F): P PI header, H voice LC header, T terminator, K CSBK, M MBC header,
//          N MBC continuation, U data header, R rate 1/2, S rate 3/4, W rate 1, I idle, X other; L voice lost (event)
//   what:  p played, d repeat dropped, e not played (privacy, no key), x other CC, s other slot, o call not played
//          (filters), n no call yet (late entry waiting), t ignored while transmitting, h call (re)started from it,
//          k ended the call, - nothing to do
// The driver side logs the same entries ("drx", platform/stm32f4/hrc/dmr_glue.c), so the two can be diffed.

#if DMR_RX_TRACE
#define TRACE_PER_LINE	10
#define TRACE_IDLE_MS	300	// flush a part line after this long without a burst

static struct {
	bool off;
	char line[72];
	uint8_t len, n;
	uint32_t last_slot, last_ms;
	char what;		// for the event being handled
	uint16_t lost;
} tr;

static void tr_put(char c)
{
	if (tr.len < sizeof(tr.line) - 1)
		tr.line[tr.len++] = c;
}

static void tr_put_u(uint32_t v)
{
	char t[10];
	int n = 0;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 10);
	while (n)
		tr_put(t[--n]);
}

static void tr_flush(void)
{
	if (tr.n) {
		tr.line[tr.len] = '\0';
		dlog("%s", tr.line);
	}
	tr.n = 0;
	tr.len = 0;
}

static void tr_begin(void)
{
	tr.what = '-';
	tr.lost = 0;
}

static void tr_add(char burst, int8_t ts, uint32_t slot, uint32_t now)
{
	if (tr.off)
		return;
	if (tr.n == 0) {
		tr.len = 0;
		tr_put('c');
		tr_put('r');
		tr_put('x');
		tr_put(' ');
		tr_put('@');
		tr_put_u(slot);
	}
	tr_put(' ');
	tr_put(burst);
	tr_put((char)('0' + (ts >= 0 && ts <= 2 ? ts : 9)));
	tr_put(tr.what);
	if (tr.lost)
		tr_put_u(tr.lost);
	if (tr.n) {
		tr_put('/');
		tr_put_u(slot - tr.last_slot);
	}
	tr.last_slot = slot;
	tr.last_ms = now;
	if (++tr.n >= TRACE_PER_LINE)
		tr_flush();
}

static char tr_burst(const dmr_burst_t *b)
{
	static const char data[11] = { 'P', 'H', 'T', 'K', 'M', 'N', 'U', 'R', 'S', 'I', 'W' };

	if (b->kind == DMR_BURST_VOICE)
		return (char)('A' + b->voice_index % 6);
	return b->kind < sizeof(data) ? data[b->kind] : 'X';
}

#define TR_WHAT(c)	(tr.what = (c))
#define TR_LOST(n)	(tr.lost = (uint16_t)(tr.lost + (n)))
#else
#define TR_WHAT(c)	((void)(c))
#define TR_LOST(n)	((void)(n))
#endif

void dmr_call_set_rx_trace(bool on)
{
#if DMR_RX_TRACE
	tr.off = !on;
	if (!on)
		tr.n = 0;
#else
	(void)on;
#endif
}

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

static uint8_t reply_window_s(bool private_call)
{
	return private_call && s.cfg.reply_window_private_s ? s.cfg.reply_window_private_s : s.cfg.reply_window_s;
}

static bool reply_running(uint32_t now)
{
	return s.reply_set && reply_window_s(s.reply.private_call) && before(now, s.reply_until) &&
		!same_target(s.reply, s.cfg.talk_target);
}

static dmr_target_t effective_target(uint32_t now)
{
	return reply_running(now) ? s.reply : s.cfg.talk_target;
}

static void set_reply(dmr_target_t t, uint32_t now)
{
	uint8_t w = reply_window_s(t.private_call);

	if (!w)
		return;
	s.reply = t;
	s.reply_until = now + (uint32_t)w * 1000u;
	s.reply_set = true;
	s.dirty = true;
}

static bool transmitting(void)
{
	return s.tx == TX_WAKE || s.tx == TX_ON || s.tx == TX_END || s.tx == TX_DATA || s.tx == TX_DATA_END;
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
	TR_LOST(frames);
	if (s.sink && s.sink->lost)
		while (frames--)
			s.sink->lost(s.sink->ctx);
}

static void le_reset(void)
{
	s.le_active = false;
	s.le_pair = false;
	s.le_held = false;
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
	if (new_talker) {
		dmr_ta_reset(&s.ta);
		s.view.caller.gps_valid = false;
	}

	dmr_caller_t *c = &s.view.caller;
	c->src = l.src;
	c->dst = l.dst;
	c->group = l.flco == DMR_FLCO_GROUP_VOICE;
	c->to_us = !c->group && l.dst == s.cfg.own_id;
	c->fid = l.fid;
	c->svc = l.svc;
	bool was_encrypted = continuing && c->encrypted;

	c->encrypted = (l.svc & DMR_SVC_PRIVACY) != 0 || was_encrypted;
	c->emergency = (l.svc & DMR_SVC_EMERGENCY) != 0;
	c->late_entry = late;
	memcpy(c->alias, s.ta.text, sizeof(c->alias));
	c->alias_complete = s.ta.complete;
	if (!continuing) {
		c->pi_header = false;
		c->pi_alg = c->pi_key = 0;
		if (s.pi_pending && !elapsed(now, s.pi_pending_ms, true, fade_ms())) {
			c->pi_header = true;	// heard just before the call was acquired
			c->pi_alg = s.pi_pending_alg;
			c->pi_key = s.pi_pending_key;
			c->encrypted = true;
		}
		s.view.call_start_ms = now;
		stats.rx_calls++;
		if (late)
			stats.rx_late_entries++;
		if (!c->group)
			stats.rx_private_calls++;
		if (c->emergency)
			stats.rx_emergency_calls++;
		if (c->encrypted)
			stats.rx_encrypted_calls++;
	} else if (c->encrypted && !was_encrypted) {
		stats.rx_encrypted_calls++;
	}
	s.pi_pending = false;
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

// The listen rules changed with the RF unchanged (a scan stop on another channel of the same frequency, color code and
// slot, a new talk target): a call being heard as not ours may be ours now. A call already playing keeps playing
static void rx_recheck(uint32_t now)
{
	dmr_lc_t l;

	dmr_lc_unpack(s.call_lc, &l);
	if (!rx_accept(&l, now))
		return;
	s.rx = RXC_PLAY;
	stats.rx_calls_played++;
	if (s.sink && s.sink->begin)
		s.sink->begin(&s.view.caller, s.sink->ctx);
	s.dirty = true;
}

// slot: the modem's slot counter for the burst (0 = none). Bursts may arrive in batches, so a repeat is told by the
// burst index and the slot counter, never by arrival time (that is only the fallback for a modem without a counter)
static void rx_play(const dmr_burst_t *b, uint32_t now, uint32_t slot)
{
	uint8_t i = b->voice_index % 6;

	if (s.have_index) {
		uint8_t missing = (uint8_t)((i + 6 - s.next_index) % 6);

		if (missing == 5) {
			// The index of the burst just played (or just concealed for a voice-lost report): the same burst
			// again unless a whole superframe went by
			bool repeat = slot != 0 && s.last_voice_slot != 0
				? (uint32_t)(slot - s.last_voice_slot) < SUPERFRAME_SLOTS
				: (uint32_t)(now - s.last_voice_ms) < DUPLICATE_MS;

			if (repeat) {
				stats.rx_dup_dropped++;
				TR_WHAT('d');
				return;
			}
		}
		sink_lost((uint8_t)(missing * DMR_AMBE_FRAMES_PER_BURST));
	}
	s.next_index = (uint8_t)((i + 1) % 6);
	s.have_index = true;
	s.last_voice_ms = now;
	s.last_voice_slot = slot;

	bool enc = b->pi || (s.view.caller.svc & DMR_SVC_PRIVACY) || s.view.caller.pi_header;

	if (enc && !s.view.caller.encrypted) {
		s.view.caller.encrypted = true;
		stats.rx_encrypted_calls++;
		s.dirty = true;
	}
	// No key, or a PI header: an MI-based scheme the static key hook can't undo (docs/spec/gotchas.md 6.4)
	if (enc && (!s.cfg.privacy || s.view.caller.pi_header)) {
		stats.rx_enc_dropped++;
		TR_WHAT('e');
		sink_lost(DMR_AMBE_FRAMES_PER_BURST);	// no key: never play the scrambled voice
		return;
	}

	uint8_t v[DMR_VOICE_BYTES];

	memcpy(v, b->payload, DMR_VOICE_BYTES);
	if (enc)
		s.cfg.privacy(v, i, false, s.cfg.privacy_ctx);
	stats.rx_voice_bursts++;
	TR_WHAT('p');
	if (s.sink && s.sink->frame)
		for (int f = 0; f < DMR_AMBE_FRAMES_PER_BURST; f++)
			s.sink->frame(&v[f * DMR_AMBE_FRAME_BYTES], s.sink->ctx);
}

// Voice with no call: start one from the embedded LC once two consecutive bursts were seen (a lone burst is noise).
// An LC that comes with the first burst is held with it until the next one makes the pair, then both are played: the
// HR_C7000 reports voice only once it has the LC, so its first burst carries it (HIL 2026-10-15: dropping it made the
// call wait for the next LC at F, 13 bursts instead of 18).
// The stale LC (a modem's sticky LC buffer still holding the previous call's) is the driver's to filter: lc_valid on
// a voice burst is an LC of this reception (docs/spec/dmr-modem.md 2.4, 6.1). Checking again here against the
// previous call's LC cost a whole superframe more whenever the same talker called again (HIL 2026-10-10: a late entry
// played from the fourth superframe heard instead of the third)
static void rx_late_entry(const dmr_burst_t *b, uint32_t now, uint32_t slot)
{
	uint8_t i = b->voice_index % 6;

	TR_WHAT('n');
	bool held = false;

	if (!s.le_active || elapsed(now, s.le_last_ms, true, fade_ms())) {
		le_reset();
		s.le_active = true;
	} else if (i == (s.le_last_index + 1) % 6) {
		s.le_pair = true;
		held = s.le_held;
	}
	s.le_held = false;
	s.le_last_index = i;
	s.le_last_ms = now;

	const dmr_burst_t *from = b;	// the burst whose LC starts the call

	if (!b->lc_valid || !dmr_lc_is_voice(b->lc)) {
		if (!held)
			return;
		from = &s.le_burst;
	}
	// The call a terminator just ended: its last voice bursts reported after the terminator (the modem's reports come
	// in batches) are not a new call. HIL 2026-10-10: one bench call counted as two or three, the extra ones without
	// voice of their own
	if (s.term_seen && !elapsed(now, s.term_ms, true, fade_ms()) && memcmp(from->lc, s.term_lc, DMR_LC_BYTES) == 0) {
		stats.rx_term_dropped++;
		return;
	}

	if (!s.le_pair) {
		s.le_burst = *b;
		s.le_burst_slot = slot;
		s.le_held = true;
		return;
	}
	rx_lc(from->lc, from, true, now);
	s.have_index = false;
	TR_WHAT('o');
	if (s.rx == RXC_PLAY) {
		if (held)
			rx_play(&s.le_burst, now, s.le_burst_slot);
		rx_play(b, now, slot);
	}
#if DMR_RX_TRACE
	if (tr.what == 'p')
		tr.what = 'h';
#endif
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
#if CONFIG_DMR_TALKER_ALIAS
		bool was = s.ta.complete;

		if (dmr_ta_push(&s.ta, b->lc)) {
			memcpy(s.view.caller.alias, s.ta.text, sizeof(s.view.caller.alias));
			s.view.caller.alias_complete = s.ta.complete;
			if (s.ta.complete && !was)
				stats.rx_alias_complete++;
			s.dirty = true;
		}
#endif
	} else if (flco == DMR_FLCO_GPS) {
#if CONFIG_DMR_GPS
		dmr_gps_t g;

		if (dmr_gps_decode(b->lc, &g)) {
			stats.rx_gps++;
			if (!s.view.caller.gps_valid || memcmp(&g, &s.view.caller.gps, sizeof(g)) != 0) {
				s.view.caller.gps = g;
				s.view.caller.gps_valid = true;
				s.dirty = true;
			}
		}
#endif
	}
}

// A PI header (data type 0, TS 102 361-1 7.1.2): 80 bits of privacy information and a CRC-CCITT masked 0x6969. It
// marks the call as protected; the octets are kept for the screens and the debug link but not interpreted (the
// static key schemes need none, MI-based ones would, docs/spec/gotchas.md 6.4). It comes between the voice LC header
// and burst A, so the call it belongs to is usually running; if not (header missed), it is held for the late entry
static void rx_pi_header(const dmr_burst_t *b, uint32_t now)
{
	bool ok = b->crc_ok;

	if (b->len >= 12) {
		uint16_t crc = (uint16_t)(dmr_crc_ccitt(b->payload, 10) ^ DMR_CRC_MASK_PI);

		ok = b->payload[10] == (uint8_t)(crc >> 8) && b->payload[11] == (uint8_t)crc;
	}
	if (!ok || b->len < 2)
		return;
	stats.rx_pi_headers++;
	if (s.rx == RXC_NONE) {
		s.pi_pending = true;
		s.pi_pending_ms = now;
		s.pi_pending_alg = b->payload[0];
		s.pi_pending_key = b->payload[1];
		return;
	}

	dmr_caller_t *c = &s.view.caller;

	if (!c->encrypted)
		stats.rx_encrypted_calls++;
	c->pi_header = true;
	c->pi_alg = b->payload[0];
	c->pi_key = b->payload[1];
	c->encrypted = true;
	s.dirty = true;
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

static void rx_burst(const dmr_burst_t *b, uint32_t now, uint32_t slot)
{
	if (transmitting()) {
		TR_WHAT('t');
		return;
	}
	s.last_any_ms = now;
	s.any_seen = true;

	// Burst A has no color code on air (voice sync where B-F have the EMB): whatever CC the modem gives it is not
	// the burst's, so it is never filtered on it (bench 2026-10-10: 0x52 at A's report held a junk CC, and every
	// A was dropped here, three frames concealed per superframe)
	bool cc_known = !(b->kind == DMR_BURST_VOICE && b->voice_index % 6 == 0);

	if (!s.cfg.listen.any_cc && cc_known && b->cc != s.cfg.cc) {
		stats.rx_cc_dropped++;
		TR_WHAT('x');
		return;
	}
	if (b->sync == DMR_SYNC_BS) {
		s.rpt_last_ms = now;
		s.rpt_seen = true;
	}
	// The slot number is only known through a repeater (CACH) or from the TDMA direct mode sync
	bool ts_known = b->ts != 0 && (s.cfg.rmo || b->sync == DMR_SYNC_BS || b->sync == DMR_SYNC_TS1 ||
				       b->sync == DMR_SYNC_TS2);
	if ((ts_known && b->ts != s.cfg.ts) || (s.rx != RXC_NONE && b->ts != 0 && s.call_ts != 0 && b->ts != s.call_ts)) {
		stats.rx_ts_dropped++;
		TR_WHAT('s');
		return;
	}
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
			bool was = s.rx != RXC_NONE && memcmp(lc, s.call_lc, DMR_LC_BYTES) == 0;

			rx_lc(lc, b, false, now);
			if (s.rx != RXC_NONE) {
				s.last_rx_ms = now;
				s.have_index = true;
				s.next_index = 0;
				s.last_voice_ms = now;
				s.last_voice_slot = slot;
				TR_WHAT(was ? '-' : (s.rx == RXC_PLAY ? 'h' : 'o'));
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
			memcpy(s.term_lc, s.call_lc, DMR_LC_BYTES);
			s.term_ms = now;
			s.term_seen = true;
			rx_end(now);
			TR_WHAT('k');
		}
		break;
	case DMR_BURST_VOICE:
		if (s.rx == RXC_NONE) {
			rx_late_entry(b, now, slot);
			break;
		}
		s.last_rx_ms = now;
		if (b->lc_valid)
			rx_embedded(b, now);
		TR_WHAT('o');
		if (s.rx == RXC_PLAY)
			rx_play(b, now, slot);
		break;
	default:
		// Any other burst on the call's slot keeps the call (a PI header precedes the voice of a call too; bench
		// 2026-10-10: headers read late came as type 0, and eight of them outlast the fade time)
		if (s.rx != RXC_NONE && b->kind != DMR_BURST_IDLE)
			s.last_rx_ms = now;
		if (b->kind == DMR_BURST_PI_HEADER)
			rx_pi_header(b, now);
		if (s.data_rx && b->kind >= DMR_BURST_CSBK && b->kind <= DMR_BURST_USBD && b->kind != DMR_BURST_IDLE)
			s.data_rx(b, now, s.data_rx_ctx);
		break;
	}
}

// --- TX side ---

static void source_end(void)
{
	if (s.source && s.source->end)
		s.source->end(s.source->ctx);
}

static void data_end(uint8_t err);

static void tx_deny(uint8_t err)
{
	if (s.data_active) {	// a burst list, not PTT: its owner hears about it, nothing is shown
		data_end(err);
		return;
	}
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
	s.tx_progress_ms = s.now;
	return true;
}

// What bursts B-E of the superframe about to start carry: the voice LC, or with a talker alias set, one alias LC in
// every other superframe (LC, header, LC, block 1, ... from the second superframe on, docs/spec/dmr-modem.md 4).
// Our GPS Info, when set, is one more LC at the end of that rotation (LC, GPS, LC, GPS ... without an alias).
// Given to the modem before burst A is queued: it takes a new embedded LC at the next burst A it sets up
static void tx_superframe_lc(void)
{
	uint8_t want = 0;

	if (s.tx_ta_n && (s.tx_sf & 1))
		want = (uint8_t)(1 + (s.tx_sf / 2) % s.tx_ta_n);
	if (want == s.tx_emb_cur)
		return;
	s.modem->set_embedded_lc(want ? s.tx_ta[want - 1] : s.tx_lc);
	s.tx_emb_cur = want;
	if (want && want - 1 == s.tx_gps_at)
		stats.tx_gps_lcs++;
	else if (want)
		stats.tx_alias_lcs++;
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

		if (s.voice_index == 0)
			tx_superframe_lc();

		uint8_t v[DMR_VOICE_BYTES];

		memcpy(v, s.stage, DMR_VOICE_BYTES);
		if (s.cfg.privacy && (s.cfg.tx_svc & DMR_SVC_PRIVACY))
			s.cfg.privacy(v, s.voice_index, true, s.cfg.privacy_ctx);
		if (!tx_put(DMR_BURST_VOICE, s.voice_index, false, v, DMR_VOICE_BYTES))
			return;	// keep the staged frames for the next try
		s.stage_n = 0;
		if (s.voice_index == 0 && s.tx_sf < 255)
			s.tx_sf++;
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
	s.tx_emb_cur = 0;
	s.tx_sf = 0;
	s.tx_ta_n = 0;
#if CONFIG_DMR_TALKER_ALIAS
	s.tx_ta_n = (uint8_t)dmr_ta_encode(s.cfg.tx_alias, s.cfg.tx_alias_format, s.tx_ta);
#endif
	s.tx_gps_at = 0xFF;
#if CONFIG_DMR_GPS
	if (s.tx_gps_set) {
		dmr_gps_encode(&s.tx_gps, s.tx_ta[s.tx_ta_n]);
		s.tx_gps_at = s.tx_ta_n++;
	}
#endif
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
	s.tx_progress_ms = now;
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
	c->emergency = (l.svc & DMR_SVC_EMERGENCY) != 0;
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

static void tx_start_data(uint32_t now);

static void tx_access(uint32_t now)
{
	if (s.data_active) {	// the repeater is up now (the list's owner did the listen before talk)
		tx_start_data(now);
		return;
	}
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

// --- Data TX: a burst list from the data service, one burst per frame on our slot (dmr-modem.md 2.6) ---

static void data_end(uint8_t err)
{
	if (transmitting())
		s.modem->tx_abort();
	dmr_data_source_t d = s.data;

	s.data_active = false;
	s.data_staged = false;
	s.tx = TX_NONE;
	s.dirty = true;
	if (d.done)
		d.done(err, d.ctx);
	if (s.ptt)
		s.ptt_seen = false;	// PTT pressed during the list: handled at this poll's PTT check
}

static void tx_data_fill(uint32_t now)
{
	while (s.tx == TX_DATA && s.modem->tx_queue_space() > 0) {
		if (!s.data_staged) {
			if (!s.data.next || !s.data.next(&s.data_stage, s.data.ctx)) {
				s.modem->tx_end();
				s.tx = TX_DATA_END;
				s.deadline = now + (tx_inflight() + 1u) * VOICE_PERIOD_MS + DMR_CALL_TX_DONE_GUARD_MS;
				s.dirty = true;
				return;
			}
			s.data_staged = true;
		}
		if (!s.modem->tx_queue(&s.data_stage))
			return;	// kept staged for the next poll
		s.data_staged = false;
		s.queued++;
	}
}

static void tx_start_data(uint32_t now)
{
	dmr_timing_t timing;

	if (s.cfg.rmo)
		timing = DMR_TIMING_ALIGNED;
	else	// DMO: in step with a station just heard (an ACK to its data), else on our own timing. The modem keeps the
		// slot timing of a reception only briefly (the HR_C6000 engine: 390 ms), so not DMR_CALL_ALIGN_MS here: an
		// aligned start after that waits for a slot that never comes and is refused (found by dmrDataChainTest)
		timing = elapsed(now, s.last_activity_ms, s.activity_seen, DMR_CALL_DATA_ALIGN_MS) ? DMR_TIMING_OWN
												 : DMR_TIMING_ALIGNED;
	if (!s.modem->tx_begin(timing, (int8_t)s.cfg.ts, DMR_ACCESS_IMPOLITE)) {
		tx_deny(DMR_TX_ERR_REJECTED);
		return;
	}
	s.queue_cap = s.modem->tx_queue_space();
	s.queued = s.started = 0;
	s.tx = TX_DATA;
	s.deadline = now + DMR_CALL_DATA_TIMEOUT_MS + (uint32_t)s.data.count * VOICE_PERIOD_MS;
	s.dirty = true;
	tx_data_fill(now);
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
	case TX_DENIED:
		s.tx = TX_NONE;
		break;
	case TX_WAKE:
	case TX_WAKE_LISTEN:
		break;		// a short press still wakes the repeater, as other radios do; tx_poll ends it once up
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
			s.dirty = true;
			if (!s.ptt && !s.data_active) {
				s.tx = TX_NONE;	// PTT let go during the wake-up: the repeater is up, nothing more to send
				s.view.wake_admits++;	// but the user hears that it would have gone through
				break;
			}
			s.tx = TX_ACCESS;
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
		// PTT release and the time-out end the call at a superframe boundary, which only a modem that sends bursts
		// reaches: one without slot timing would hold us here, keyed, for good
		if (elapsed(now, s.tx_progress_ms, true, DMR_CALL_TX_STALL_MS)) {
			tx_deny(DMR_TX_ERR_STALLED);
			break;
		}
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
	case TX_DATA:
		if (!before(now, s.deadline))
			tx_deny(DMR_TX_ERR_TIMEOUT);
		else
			tx_data_fill(now);
		break;
	case TX_DATA_END:
		if (!before(now, s.deadline))
			data_end(DMR_TX_OK);	// tx_done overdue: the bursts went (data_end aborts the modem)
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
		if (it->has_burst) {
#if DMR_RX_TRACE
			tr_begin();
#endif
			rx_burst(&it->burst, now, it->arg);
#if DMR_RX_TRACE
			tr_add(tr_burst(&it->burst), it->ts, it->arg, now);
#endif
		}
		break;
	case DMR_EV_VOICE_LOST:
#if DMR_RX_TRACE
		tr_begin();
#endif
		if (s.rx == RXC_PLAY && !transmitting() && (it->ts == 0 || s.call_ts == 0 || it->ts == s.call_ts)) {
			// The next burst is concealed now; if it still comes, the repeat rule drops it (same index)
			sink_lost(DMR_AMBE_FRAMES_PER_BURST);
			if (s.have_index)
				s.next_index = (uint8_t)((s.next_index + 1) % 6);
		}
#if DMR_RX_TRACE
		tr_add('L', it->ts, it->arg, now);
#endif
		break;
	case DMR_EV_TX_SLOT_STARTED:
		if (transmitting()) {
			s.started++;
			s.tx_progress_ms = now;
		}
		break;
	case DMR_EV_TX_REJECTED:
		if (transmitting())
			tx_deny(DMR_TX_ERR_REJECTED);
		break;
	case DMR_EV_TX_DONE:
		if (s.tx == TX_END) {
			tx_finished(now);
		} else if (s.tx == TX_DATA_END) {
			data_end(DMR_TX_OK);
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
		if (s.data_active && s.modem)
			data_end(DMR_TX_ERR_REJECTED);
		if (transmitting() && s.modem)
			s.modem->tx_abort();
		if (s.tx == TX_ON)
			source_end();
		if (s.tx != TX_NONE)
			s.tx = s.ptt ? TX_DENIED : TX_NONE;
		rx_end(s.now);
		s.rpt_seen = false;
	}
	s.cfg = *cfg;
	if (s.cfg.ts != 2)
		s.cfg.ts = 1;
	s.cfg.cc &= 0x0F;
	if (s.cfg.reply_window_s > 30)
		s.cfg.reply_window_s = 30;
	if (s.cfg.reply_window_private_s > 30)
		s.cfg.reply_window_private_s = 30;
	s.cfg.tx_alias[DMR_TA_TEXT_MAX] = '\0';
	if (rf_change || target_change)
		s.reply_set = false;	// temporary state ends with the channel (docs/ux 1)
	s.configured = true;
	s.view.cc = s.cfg.cc;
	s.view.ts = s.cfg.ts;
	s.view.rmo = s.cfg.rmo;
	s.dirty = true;
	if (s.running && rf_change)
		modem_apply();
	if (!rf_change && s.rx == RXC_OTHER)
		rx_recheck(s.now);
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
	if (s.data_active)
		data_end(DMR_TX_ERR_REJECTED);
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

void dmr_call_set_tx_gps(const dmr_gps_t *gps)
{
	s.tx_gps_set = gps != NULL;
	if (gps)
		s.tx_gps = *gps;
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
		phase = s.data_active ? DMR_CALL_PHASE_TX_DATA : DMR_CALL_PHASE_TX_WAKEUP;
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
	case TX_DATA:
	case TX_DATA_END:
		phase = DMR_CALL_PHASE_TX_DATA;
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

#if DMR_RX_TRACE
	if (tr.n && elapsed(now_ms, tr.last_ms, true, TRACE_IDLE_MS))
		tr_flush();
#endif

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

bool dmr_call_last_activity(uint32_t *ms)
{
	*ms = s.last_activity_ms;
	return s.activity_seen;
}

void dmr_call_tx_alias(char out[DMR_TA_TEXT_MAX + 1])
{
	memcpy(out, s.cfg.tx_alias, DMR_TA_TEXT_MAX + 1);
	out[DMR_TA_TEXT_MAX] = '\0';
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

void dmr_call_set_data_rx(dmr_data_rx_fn fn, void *ctx)
{
	s.data_rx = fn;
	s.data_rx_ctx = ctx;
}

bool dmr_call_data_tx(const dmr_data_source_t *src)
{
	if (!s.modem || !s.running || !s.configured || s.tx != TX_NONE || s.ptt || s.data_active || src == NULL ||
	    src->next == NULL)
		return false;
	s.data = *src;
	s.data_active = true;
	s.data_staged = false;
	s.tx_since = s.now;
	s.wake_tries = 0;
	s.dirty = true;
	if (s.cfg.rmo && !repeater_up(s.now))
		tx_wakeup(s.now);
	else
		tx_start_data(s.now);
	return true;
}

bool dmr_call_data_busy(void)
{
	return s.data_active;
}
