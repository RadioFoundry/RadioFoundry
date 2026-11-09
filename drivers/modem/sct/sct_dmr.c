// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stddef.h>
#include <string.h>
#include "sct_dmr.h"
#include "sct_pkt.h"

#define TXQ_DEPTH	8
#define LC_BYTES	9
// DMR_CALL_OPTION byte 0 bits 1-0: direct mode without the reverse channel, for both: the MS side of a repeater is
// bursts too
#define TRAFFIC_MASK	0x03
#define TRAFFIC_DIRECT	0x01
// REPORT_FIELD bits the engine needs: NEW_EVENT (7), DMR_FLC (6), DMR_CSBK (5), slot type and EMB (0)
#define REPORT_FIELD	0xE1
#define SCT_DMR_MODE_WAIT_MS	40	// the Packet Interface's answer time for a field
#define SCT_DMR_RESTART_MS	1000	// a chip that keeps restarting gets a start this often at most
#define CR_UNSENT	0xFF	// CARRIER_READY not sent since the chip went to receive

typedef enum { TX_IDLE, TX_ON, TX_ENDING, TX_DRAIN } tx_state_t;

static struct {
	const sct_dmr_port_t *port;
	dmr_event_cb_t cb;
	void *cb_ctx;
	bool running;
	dmr_config_t cfg;
	int8_t cc_filter;
	int8_t rx_slot;		// 0 both

	// TX
	tx_state_t tx;
	int8_t tx_ts;
	dmr_tx_burst_t q[TXQ_DEPTH];
	uint8_t q_head, q_count;
	bool waiting;		// a frame is out: the next waits for its answer or its slot to pass
	uint32_t sent_ms;	// when it went (or when a paced voice burst was taken)
	uint32_t next_ms;	// the next burst goes no earlier than this
	bool paced;		// next_ms is set
	bool voice_on;		// the chip runs the voice of this transmission
	bool tx_voice;		// ... and has, at some point: the end waits for the chip
	uint32_t tx_edges;	// the port's burst_edges() at the key-up
	uint32_t tx_sent;	// frames sent since
	bool audio_tx;		// codec_tx went: codec_rx is due at the return to receive
	bool lc_new;		// set_embedded_lc gave an LC not yet sent
	uint8_t emb_lc[LC_BYTES];
	uint32_t done_ms;	// TX_DRAIN: report TX_DONE by then if the chip stays silent
	bool drain_heard;	// ... a packet came since the drain began

	// RX
	uint8_t rx_cc;
	uint8_t sync;		// dmr_sync_family_t from the last DMR_SLOT_FOUND
	int8_t ts;
	bool slot_verified;	// a repeater's outbound: DMR_SLOT_FOUND said its slot numbering is settled, since the last return to receive
	bool fec_err;		// the last NEW_EVENT was a data sync with an FEC error
	bool mode_wait;		// transmit: WORK_MODE not answered yet, so no frame goes (since mode_ms)
	uint32_t mode_ms;
	uint8_t slot_hunt;		// DMR_SLOT_FOUND reports since the receiver started, none verified yet
	bool rehunted;			// that hunt was long and has been started over once
	bool voice_start;	// the frame out is the one that starts a call's voice
	bool start_wait;	// the start's fields not all answered yet
	bool start_retried;	// ... and the one retry a refused one gets has gone
	bool restart;		// the chip restarted or refused the start: run it again from the poll
	uint32_t restart_ms;
	// A received voice call per slot (a repeater's outbound can carry one in each): while on, a voice burst is
	// reported in its slot every 60 ms
	struct {
		bool on;
		int8_t ts;		// the slot it came in (0: direct mode without a slot)
		uint8_t index;		// the next burst's index, 0..5 (A..F)
		uint8_t lc[LC_BYTES];	// its LC
		uint32_t next_ms;
	} rv[2];
	uint32_t counter;	// burst counter for event args, never 0
	uint32_t now;
	bool cr_used;		// the board reports its carrier (sct_dmr_carrier)
	uint8_t cr_want;	// ... its last CARRIER_READY value
	uint32_t cr_seen;	// ... when it last said a carrier is there
	uint8_t cr_sent;	// ... the chip's, CR_UNSENT after each WORK_MODE receive
	bool cr_settling;	// back from a transmission: the port's rx_ready goes out at cr_due
	uint32_t cr_due;

	uint8_t option[5];	// DMR_CALL_OPTION: the port's, unless a bench poke changed a byte

	// One field asked from the debug link (sct_dmr_field_begin)
	struct {
		uint8_t state;	// FIELD_*
		uint8_t id;
		bool read;
		uint8_t n;
		uint8_t data[4];
	} field;

	sct_dmr_stats_t st;
} s;

enum { FIELD_IDLE, FIELD_WAIT, FIELD_DONE, FIELD_REFUSED };

static void emit(uint8_t kind, int8_t ts, uint32_t arg, const dmr_burst_t *b)
{
	if (s.cb == NULL)
		return;
	dmr_event_t ev = { .kind = kind, .ts = ts, .arg = arg, .burst = b };

	s.cb(&ev, s.cb_ctx);
}

static uint32_t next_counter(void)
{
	if (++s.counter == 0)
		s.counter = 1;
	return s.counter;
}

static bool send(sct_pkt_t *p)
{
	uint16_t n = sct_pkt_end(p, false);

	return n != 0 && s.port != NULL && s.port->send(p->buf, n, s.port->ctx);
}

static void add_work_mode(sct_pkt_t *p, uint8_t mode)
{
	const uint8_t wm[3] = { mode, 0, 0 };

	sct_pkt_add(p, SCT_F_WORK_MODE, wm, 3);
}

// DMR_CALL_SLOT for receive: 1, 2, or 3 for both; direct mode has no slot
static uint8_t rx_slot_byte(void)
{
	if (!s.cfg.repeater)
		return 0;			// the Packet Interface: 0 for a mobile to mobile call
	return (uint8_t)(s.rx_slot == 1 || s.rx_slot == 2 ? s.rx_slot : 3);
}

static bool send_rx_mode(void)
{
	sct_pkt_t p;

	s.cr_sent = CR_UNSENT;
	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	add_work_mode(&p, 1);
	if (!send(&p))
		return false;
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add1(&p, SCT_F_DMR_CALL_SLOT, rx_slot_byte());
	return send(&p);
}

// Receive again after a transmission: with rx_ready, CARRIER_READY waits for the receiver to settle
static void rx_after_tx(void)
{
	s.slot_verified = false;
	s.slot_hunt = 0;
	s.rehunted = false;
	send_rx_mode();
	s.cr_settling = s.port->rx_ready != 0;
	s.cr_due = s.now + s.port->rx_settle_ms;
}

// The board's control fields for a transmission (tx_fields) in one packet. True if there are none or they went
static bool send_tx_fields(void)
{
	if (s.port->tx_fields == NULL)
		return true;
	const sct_field_t *f = NULL;
	uint8_t n = s.port->tx_fields(&f, s.port->ctx);

	if (n == 0 || f == NULL)
		return true;
	sct_pkt_t p;

	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	for (uint8_t i = 0; i < n; i++)
		sct_pkt_add(&p, f[i].id, f[i].data, f[i].len);
	return send(&p);
}

static void rx_voice_clear(void);

// CARRIER_READY when the board's value differs from the chip's, in receive only. After a transmission, nothing until
// the receiver has settled, then the port's rx_ready (the chip calibrates on what it hears from there)
static void carrier_step(void)
{
	if (!s.running || s.tx != TX_IDLE)
		return;
	if (s.cr_settling) {
		if ((int32_t)(s.now - s.cr_due) < 0)
			return;
		s.cr_settling = false;
		s.cr_want = s.port->rx_ready;
		s.cr_seen = s.now;
		s.cr_sent = CR_UNSENT;
	} else if (!s.cr_used || s.cr_want == s.cr_sent) {
		return;
	}
	// A direct-mode station sends one slot in two, so the reading drops between its bursts. Each "lost" makes the
	// chip look for sync again, so a loss goes out only once it has lasted SCT_DMR_CR_HOLD_MS
	if (s.cr_want == 0 && s.cr_sent != CR_UNSENT && s.cr_sent != 0 &&
	    (int32_t)(s.now - s.cr_seen) < SCT_DMR_CR_HOLD_MS)
		return;
	sct_pkt_t p;

	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	sct_pkt_add1(&p, SCT_F_CARRIER_READY, s.cr_want);
	if (send(&p)) {
		s.cr_sent = s.cr_want;
		if (s.cr_want == 0) {
			s.slot_verified = false;
			s.slot_hunt = 0;
			s.rehunted = false;
		}
		s.st.carrier = s.cr_want;
		s.st.carrier_sent++;
		// The board's carrier gone for the hold ends a call the chip plays: on a weak signal the chip can miss the
		// terminators and say SYNC_LOST only seconds later, and the stack would hold the call that long. A call that
		// comes back is a late entry (the chip's next LC)
		if (s.cr_want == 0 && (s.rv[0].on || s.rv[1].on)) {
			rx_voice_clear();
			emit(DMR_EV_SYNC_LOST, s.ts, next_counter(), NULL);
			s.sync = DMR_SYNC_NONE;
		}
	}
}

static void send_config(sct_pkt_t *p, const dmr_config_t *cfg)
{
	uint8_t opt[5];
	uint8_t bcd[4];

	memcpy(opt, s.option, sizeof(opt));
	opt[0] = (uint8_t)((opt[0] & ~TRAFFIC_MASK) | TRAFFIC_DIRECT);

	sct_pkt_add(p, SCT_F_DMR_CALL_OPTION, opt, 5);
	sct_pkt_add1(p, SCT_F_DMR_CC, (uint8_t)(cfg->cc >= 0 ? cfg->cc & 0x0F : (s.rx_cc & 0x0F)));
	sct_id_bcd(cfg->own_id, bcd);
	sct_pkt_add(p, SCT_F_DMR_OWN_ID, bcd, 4);
}

// One codec write; its answer comes in a codec packet, which packet() skips, so it never reads as a frame's answer
static void codec_write(const sct_codec_write_t *w)
{
	uint8_t p[SCT_CODEC_PKT_BYTES];

	if (w == NULL)
		return;
	sct_pkt_codec(p, w);
	s.port->send(p, sizeof(p), s.port->ctx);
}

// The board's codec write for transmit, and its undo once the chip is back in receive
static void audio_tx(void)
{
	if (s.port->codec_tx == NULL)
		return;
	codec_write(s.port->codec_tx);
	s.audio_tx = true;
}

static void audio_rx(void)
{
	if (!s.audio_tx)
		return;
	s.audio_tx = false;
	codec_write(s.port->codec_rx);
}

// --- TX ---

static uint8_t frame_len(uint8_t kind)
{
	switch (kind) {
	case DMR_BURST_PI_HEADER:
	case DMR_BURST_VOICE_LC_HEADER:
	case DMR_BURST_TERMINATOR_LC:
		return 9;	// the LC (or PI) without its RS parity / CRC: the chip adds it
	case DMR_BURST_CSBK:
	case DMR_BURST_MBC_HEADER:
	case DMR_BURST_DATA_HEADER:
		return 10;
	case DMR_BURST_MBC_CONT:
	case DMR_BURST_RATE_12_DATA:
	case DMR_BURST_IDLE:
		return 12;
	case DMR_BURST_RATE_34_DATA:
		return 18;
	case DMR_BURST_RATE_1_DATA:
		return 24;
	default:
		return 0;
	}
}

static void q_pop(void)
{
	s.q_head = (uint8_t)((s.q_head + 1) % TXQ_DEPTH);
	s.q_count--;
}

static void slot_started(void)
{
	emit(DMR_EV_TX_SLOT_STARTED, s.tx_ts, next_counter(), NULL);
}

static bool send_frame(uint8_t prop, const uint8_t *data, uint8_t len)
{
	sct_pkt_t p;

	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add_frame(&p, prop, data, len);
	if (!send(&p))
		return false;
	s.st.tx_frames++;
	s.waiting = true;
	s.sent_ms = s.now;
	s.tx_sent++;
	return true;
}

// The next queued burst, when the chip is ready for it
static void tx_pump(void)
{
	while (s.q_count != 0 && !s.mode_wait && !s.waiting && (!s.paced || (int32_t)(s.now - s.next_ms) >= 0)) {
		const dmr_tx_burst_t *b = &s.q[s.q_head];

		if (b->kind == DMR_BURST_VOICE) {
			// The LC for the embedded signaling goes with the call's first voice burst, and again at a burst A
			// after a new one. Every other voice burst is the chip's: take it in its slot
			bool first = !s.voice_on;
			bool lc = first || (s.lc_new && b->voice_index % 6 == 0);

			s.voice_on = true;
			s.tx_voice = true;
			if (lc) {
				s.lc_new = false;
				q_pop();
				s.voice_start = first;
				if (!send_frame(SCT_PROP_VOICE_LC, s.emb_lc, LC_BYTES))
					s.waiting = false;
				return;
			}
			q_pop();
			s.st.tx_paced++;
			s.sent_ms = s.now;
			s.next_ms = s.now + SCT_DMR_SLOT_MS;
			s.paced = true;
			slot_started();
			continue;
		}
		uint8_t n = frame_len(b->kind);

		if (n == 0 || b->len < n) {
			q_pop();	// nothing the chip can send (USBD, EMPTY) or a short payload
			continue;
		}
		if (b->kind == DMR_BURST_VOICE_LC_HEADER && !s.lc_new)
			memcpy(s.emb_lc, b->payload, LC_BYTES);	// the embedded LC is the header's until told otherwise
		if (b->kind == DMR_BURST_TERMINATOR_LC)
			s.voice_on = false;
		uint8_t data[24];

		memcpy(data, b->payload, n);
		q_pop();
		if (!send_frame(b->kind & SCT_PROP_TYPE, data, n))
			s.waiting = false;
		return;
	}
}

// The chip refused the transmission: back to receive, and the stack hears it wasn't sent
static void tx_reject(void)
{
	s.st.tx_rejected++;
	s.tx = TX_IDLE;
	s.waiting = false;
	s.paced = false;
	s.voice_on = false;
	s.voice_start = false;
	s.mode_wait = false;
	s.q_count = 0;
	rx_after_tx();
	audio_rx();
	emit(DMR_EV_TX_REJECTED, s.tx_ts, next_counter(), NULL);
}

static void tx_finish(void)
{
	s.tx = TX_IDLE;
	s.waiting = false;
	s.paced = false;
	s.voice_on = false;
	s.q_count = 0;
	rx_after_tx();
	audio_rx();
	emit(DMR_EV_TX_DONE, s.tx_ts, next_counter(), NULL);
}

// The frame out has its slot: the next one goes a slot after it
static void frame_slot(void)
{
	s.voice_start = false;
	if (!s.waiting)
		return;
	s.waiting = false;
	s.next_ms = s.sent_ms + SCT_DMR_SLOT_MS;
	s.paced = true;
	slot_started();
}

// The chip answered the frame out
static void frame_taken(void)
{
	if (s.waiting)
		s.st.tx_acks++;
	frame_slot();
}

// The transmission's last burst has ended, by the port's burst line: each frame goes out in a burst of its own, in
// order, so once nothing is left to send, the end is the fall that completes as many bursts as frames went. Not with
// voice: the chip's own voice bursts don't go by frames
static bool last_burst_over(void)
{
	if (s.port->burst_edges == NULL || s.tx_voice || s.tx_sent == 0 || s.q_count != 0 ||
	    (s.tx != TX_ENDING && s.tx != TX_DRAIN))
		return false;
	return s.port->burst_edges() - s.tx_edges >= 2u * s.tx_sent;
}

// The first harm the health keys count keeps its moment
static void health_mark(void)
{
	if (s.st.first_ms == 0) {
		s.st.first_ms = s.now != 0 ? s.now : 1u;
		s.st.first[0] = (uint8_t)s.tx;
		s.st.first[1] = s.q_count;
		s.st.first[2] = s.st.last_event;
		s.st.first[3] = s.st.last_nak;
	}
}

static void tx_step(void)
{
	if (s.tx == TX_IDLE)
		return;
	// Unanswered within the Packet Interface's time: the packet may be lost, but a transmission is under way
	if (s.mode_wait && (int32_t)(s.now - s.mode_ms) >= SCT_DMR_MODE_WAIT_MS) {
		s.mode_wait = false;
		s.st.mode_timeouts++;
	}
	// A chip may answer only once its frames are on the air (ACK 0x0A, two slots or more after the send) and hold
	// the next frame meanwhile: unanswered, the next one goes a slot after it anyway
	if (s.waiting && (int32_t)(s.now - s.sent_ms) >= SCT_DMR_SLOT_MS) {
		s.st.tx_timeouts++;
		frame_slot();
	}
	tx_pump();
	if (last_burst_over()) {
		s.st.tx_early++;
		tx_finish();
		return;
	}
	if (s.tx == TX_ENDING && s.q_count == 0 && !s.waiting) {
		s.tx = TX_DRAIN;
		s.done_ms = s.now + SCT_DMR_DONE_MS;
		s.drain_heard = false;
	}
	if (s.tx == TX_DRAIN && (int32_t)(s.now - s.done_ms) >= 0) {
		if (!s.drain_heard && s.tx_sent != 0) {
			health_mark();
			s.st.drain_silent++;
		}
		tx_finish();
	}
}

// --- RX ---

static uint16_t crc_ccitt(const uint8_t *buf, uint8_t len)
{
	uint16_t crc = 0;

	for (uint8_t i = 0; i < len; i++) {
		crc ^= (uint16_t)(buf[i] << 8);
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return (uint16_t)~crc;
}

// TS 102 361-1 B.3.12 CRC masks of the 10-octet kinds
static uint16_t crc_mask(uint8_t kind)
{
	switch (kind) {
	case DMR_BURST_PI_HEADER:
		return 0x6969;
	case DMR_BURST_CSBK:
		return 0xA5A5;
	case DMR_BURST_MBC_HEADER:
		return 0xAAAA;
	case DMR_BURST_DATA_HEADER:
		return 0xCCCC;
	default:
		return 0;
	}
}

static void rx_voice_clear(void)
{
	s.rv[0].on = false;
	s.rv[1].on = false;
}

static void rx_voice_start(uint8_t slot, int8_t ts)
{
	s.rv[slot].on = true;
	s.rv[slot].ts = ts;
	s.rv[slot].index = 0;
	s.rv[slot].next_ms = s.now + SCT_DMR_SLOT_MS;
}

static void rx_frame(const uint8_t *f, uint8_t flen)
{
	uint8_t prop = f[0];
	uint8_t n = f[1];
	const uint8_t *data = &f[2];
	dmr_burst_t b;

	(void)flen;
	memset(&b, 0, sizeof(b));
	b.sync = s.sync;
	b.cc = s.rx_cc;
	b.ts = s.ts;
	if (s.sync == DMR_SYNC_BS)
		b.ts = (int8_t)((prop & SCT_PROP_TS2) ? 2 : 1);	// each frame's own slot: both slots come in turn
	b.in_step = true;
	b.crc_ok = !s.fec_err;
	if (s.fec_err)
		s.st.rx_fec++;
	s.fec_err = false;
	s.st.rx_frames++;

	if ((prop & SCT_PROP_VOICE) != 0 || (prop & SCT_PROP_TYPE) == DMR_BURST_VOICE_LC_HEADER ||
	    (prop & SCT_PROP_TYPE) == DMR_BURST_TERMINATOR_LC) {
		// A voice call: its header, its terminator, or the LC of a superframe (a late entry, or a new talker without a
		// header). The chip plays the voice; the stack gets the LC to show the call
		bool embedded = (prop & SCT_PROP_VOICE) != 0;

		if (n < LC_BYTES)
			return;
		uint8_t slot = b.ts == 2 ? 1 : 0;

		if (embedded && s.rv[slot].on && memcmp(data, s.rv[slot].lc, LC_BYTES) == 0)
			return;	// the running call's own LC from a superframe: nothing new
		memcpy(b.lc, data, LC_BYTES);
		b.lc_valid = true;
		memcpy(s.rv[slot].lc, data, LC_BYTES);
		if (embedded) {
			// To the stack the next voice burst, with the LC embedded as a burst modem reports it, so the call is
			// taken as a late entry (a header here was a call heard from its start). A late entry's LC came with
			// burst A; a new talker's takes the place of the next burst, and the bursts go on from it
			if (!s.rv[slot].on)
				rx_voice_start(slot, b.ts);
			b.kind = DMR_BURST_VOICE;
			b.voice_index = s.rv[slot].index;
			b.len = DMR_VOICE_BYTES;
			s.rv[slot].index = (uint8_t)((s.rv[slot].index + 1) % 6);
			s.rv[slot].next_ms = s.now + SCT_DMR_SLOT_MS;
			s.st.rx_voice++;
		} else {
			b.kind = prop & SCT_PROP_TYPE;
			memcpy(b.payload, data, LC_BYTES);
			b.len = LC_BYTES;
			if (b.kind == DMR_BURST_TERMINATOR_LC) {
				s.rv[slot].on = false;
			} else {
				// The chip voices the call from here on and says no more until it ends: the stack gets a voice
				// burst per slot, as it would from a burst modem, so it holds the call. A repeated header (when
				// the board has every one reported) starts the count again: the voice follows the last header
				rx_voice_start(slot, b.ts);
			}
		}
	} else {
		b.kind = prop & SCT_PROP_TYPE;
		if (n > DMR_PAYLOAD_MAX)
			n = DMR_PAYLOAD_MAX;
		memcpy(b.payload, data, n);
		b.len = n;
		uint16_t mask = crc_mask(b.kind);

		if (mask != 0 && n == 10) {
			// The chip checked and stripped the CRC; rebuild it so the burst reads as from any other modem
			uint16_t crc = (uint16_t)(crc_ccitt(b.payload, 10) ^ mask);

			b.payload[10] = (uint8_t)(crc >> 8);
			b.payload[11] = (uint8_t)crc;
			b.len = 12;
		}
	}
	emit(DMR_EV_BURST_RX, b.ts, next_counter(), &b);
}

// The voice bursts of a call the chip plays: no payload (the voice never reaches the MCU), one per 60 ms
static void rx_voice_step(void)
{
	for (uint8_t i = 0; i < 2; i++) {
		for (int n = 0; s.rv[i].on && s.tx == TX_IDLE && (int32_t)(s.now - s.rv[i].next_ms) >= 0 && n < 4; n++) {
			dmr_burst_t b;

			memset(&b, 0, sizeof(b));
			b.kind = DMR_BURST_VOICE;
			b.voice_index = s.rv[i].index;
			b.sync = s.sync;
			b.cc = s.rx_cc;
			b.ts = s.rv[i].ts;
			b.crc_ok = true;
			b.in_step = true;
			b.len = DMR_VOICE_BYTES;
			s.rv[i].index = (uint8_t)((s.rv[i].index + 1) % 6);
			s.rv[i].next_ms += SCT_DMR_SLOT_MS;
			s.st.rx_voice++;
			emit(DMR_EV_BURST_RX, b.ts, next_counter(), &b);
		}
		if (s.rv[i].on && (int32_t)(s.now - s.rv[i].next_ms) >= 0) {
			s.rv[i].next_ms = s.now + SCT_DMR_SLOT_MS;	// fell behind (a stalled task): no burst storm
			if (s.tx == TX_IDLE) {	// four bursts caught up and still late (keyed, none are due)
				health_mark();
				s.st.rx_late++;
			}
		}
	}
}

static void rx_slot_found(uint8_t v)
{
	s.st.last_slot_found = v;
	if (v & SCT_SLOT_BS) {
		// Verified (SCT_SLOT_VERIFIED): the chip's slot numbering is settled, and only now does an aligned start
		// land on the slot asked; a report after that without the flag is a CACH that changed, not a new doubt
		// A hunt this long ends, about one time in three, with a verified slot that is the other one (modem-sct.md 6):
		// start it over, once
		if (!s.slot_verified && !(v & SCT_SLOT_VERIFIED) && !s.rehunted && s.tx == TX_IDLE &&
		    ++s.slot_hunt >= SCT_DMR_HUNT_LONG) {
			s.rehunted = true;
			s.slot_hunt = 0;
			s.st.rehunts++;
			send_rx_mode();
			return;
		}
		if (v & SCT_SLOT_VERIFIED)
			s.slot_verified = true;
		s.sync = DMR_SYNC_BS;
		s.ts = (int8_t)((v & 0x04) ? 2 : 1);	// TC: the outbound burst after the CACH
		// A repeater's idle output reports no frames: the stack sees its CACH this way, or it would wake a
		// repeater that is up while a key-up waits for the verified report
		emit(DMR_EV_SLOT_TICK, s.ts, s.counter != 0 ? s.counter : 1u, NULL);
	} else {
		switch (v & 0x03) {
		case 1:
			s.sync = DMR_SYNC_TS1;
			s.ts = 1;
			break;
		case 2:
			s.sync = DMR_SYNC_TS2;
			s.ts = 2;
			break;
		default:
			s.sync = DMR_SYNC_MS;
			s.ts = 0;
			break;
		}
	}
}

// A frame refused. The one that starts a call's voice ends the transmission: no burst of the call would go out
static void frame_refused(uint8_t code)
{
	s.st.naks++;
	s.st.last_nak = code;
	s.waiting = false;	// the next one may go
	if (s.voice_start)
		tx_reject();
}

// One retry of a start the chip refused part of
static void start_refused(void)
{
	s.start_wait = false;
	if (!s.start_retried) {
		s.start_retried = true;
		s.restart = true;
	}
}

static void rx_ack(uint8_t id, uint8_t code)
{
	if (id == SCT_F_DATA_FRAME) {
		if (code == SCT_ACK_OK)
			frame_taken();
		else
			frame_refused(code);
		return;
	}
	if (code != SCT_ACK_OK) {
		s.st.naks++;
		s.st.last_nak = code;
		if (s.start_wait)
			start_refused();
	} else if (id == SCT_F_DMR_CALL_SLOT && s.start_wait) {	// the last of the start
		s.start_wait = false;
		s.start_retried = false;
	}
	if (id == SCT_F_WORK_MODE && s.mode_wait) {
		s.mode_wait = false;
		if (code != SCT_ACK_OK)
			tx_reject();
	}
}

static void rx_status(uint8_t code)
{
	switch (code) {
	case SCT_ACK_OK:
		break;
	case SCT_ACK_RESET_READY:
		if (s.waiting) {	// a frame out: its refusal (a frame the chip can't send is answered so)
			frame_refused(code);
			break;
		}
		// The chip answers the start and a new configuration with this. Later, it restarted under us: what the
		// start set is gone
		if (s.running && (int32_t)(s.now - s.restart_ms) >= SCT_DMR_RESTART_MS) {
			s.st.chip_restarts++;
			health_mark();
			s.restart = true;
		}
		break;
	case SCT_ACK_FAR_DONE: {
		// The frames sent have gone on the air. After the last one (a slot after it at the soonest, so not an
		// earlier frame's) that ends the transmission, also when it comes before the drain
		bool last = s.tx == TX_DRAIN || (s.tx == TX_ENDING && s.q_count == 0);

		frame_taken();
		if (last && (int32_t)(s.now - s.sent_ms) >= SCT_DMR_SLOT_MS)
			tx_finish();
		break;
	}
	default:
		if (s.waiting) {
			s.st.tx_timeouts++;
			frame_refused(code);
			break;
		}
		s.st.naks++;
		s.st.last_nak = code;
		if (s.start_wait)
			start_refused();
		break;
	}
}

static void rx_event(uint8_t code)
{
	s.st.last_event = code;
	switch (code) {
	case SCT_EV_DATA_SYNC_FEC:
		s.fec_err = true;
		break;
	case SCT_EV_DATA_SYNC:
	case SCT_EV_VOICE_SYNC:
		s.fec_err = false;
		break;
	case SCT_EV_SYNC_LOST:
	case SCT_EV_CARRIER_LOST:
		s.slot_verified = false;
		s.slot_hunt = 0;
		s.rehunted = false;
		rx_voice_clear();
		if (s.tx == TX_IDLE)
			emit(DMR_EV_SYNC_LOST, s.ts, next_counter(), NULL);
		s.sync = DMR_SYNC_NONE;
		break;
	case SCT_EV_UNSUPPORTED_CALL:
	case SCT_EV_WRONG_VOCODER:
		s.st.rx_unsupported++;
		break;
	default:
		break;
	}
}

// The answer to the field the debug link asked for. Not the engine's: a refusal isn't counted as a NAK
static bool field_answer(uint8_t id, const uint8_t *d, uint8_t n, bool ack)
{
	if (s.field.state != FIELD_WAIT)
		return false;
	if (id == SCT_F_ACK && !ack && d[0] != SCT_ACK_OK && d[0] != SCT_ACK_FAR_DONE && d[0] != SCT_ACK_RESET_READY) {
		s.field.state = FIELD_REFUSED;	// the chip's general error for a packet it couldn't take
		return true;
	}
	if (id != s.field.id || ack == s.field.read)
		return false;
	if (ack) {
		s.field.state = d[0] == SCT_ACK_OK ? FIELD_DONE : FIELD_REFUSED;
		return true;
	}
	s.field.n = n < sizeof(s.field.data) ? n : (uint8_t)sizeof(s.field.data);
	memcpy(s.field.data, d, s.field.n);
	s.field.state = FIELD_DONE;
	return true;
}

static void packet(const uint8_t *pkt, uint16_t len, uint32_t now_ms)
{
	sct_fields_t it;
	uint8_t id, n;
	const uint8_t *d;
	bool ack;

	s.now = now_ms;
	if (!sct_fields_begin(&it, pkt, len, false)) {
		s.st.rx_bad++;
		return;
	}
	if (it.type & SCT_TYPE_CODEC)
		return;		// a codec register answer: not the DMR side's
	while (sct_fields_next(&it, &id, &d, &n, &ack)) {
		if (field_answer(id, d, n, ack))
			continue;
		if (id == SCT_F_ACK) {
			rx_status(d[0]);
			continue;
		}
		if (ack) {
			rx_ack(id, d[0]);
			continue;
		}
		if (!s.running)
			continue;
		switch (id) {
		case SCT_F_DMR_CC:
			s.rx_cc = d[0] & 0x0F;
			break;
		case SCT_F_DMR_SLOT_TYPE:
			s.rx_cc = d[0] >> 4;
			break;
		case SCT_F_DMR_SLOT_FOUND:
			rx_slot_found(d[0]);
			break;
		case SCT_F_NEW_EVENT:
			rx_event(d[0]);
			break;
		case SCT_F_DATA_FRAME:
			if (s.tx == TX_IDLE)
				rx_frame(d, n);
			break;
		default:
			break;
		}
	}
	if (it.left != 0)
		s.st.rx_bad++;
	tx_step();
	rx_voice_step();
}


// --- hal_dmr_modem ---

// The board's start fields of one packet type (TYPE 5 or TYPE 0), in its order
static void add_start_fields(sct_pkt_t *p, bool dmr)
{
	for (uint8_t i = 0; i < s.port->start_count; i++) {
		const sct_field_t *f = &s.port->start_fields[i];

		if (sct_field_is_dmr(f->id) == dmr)
			sct_pkt_add(p, f->id, f->data, f->len);
	}
}

static bool x_start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	if (s.port == NULL || (s.port->ready != NULL && !s.port->ready(s.port->ctx)))
		return false;
	sct_pkt_t p;

	s.cfg = *cfg;
	s.cb = cb;
	s.cb_ctx = ctx;
	s.tx = TX_IDLE;
	s.q_count = 0;
	s.waiting = false;
	s.mode_wait = false;
	s.voice_start = false;
	s.restart = false;
	s.restart_ms = s.now;
	s.start_wait = true;
	s.start_retried = false;
	s.sync = DMR_SYNC_NONE;
	s.slot_verified = false;
	s.slot_hunt = 0;
	s.rehunted = false;
	s.ts = 0;
	s.rx_slot = 0;
	rx_voice_clear();
	s.cr_settling = false;
	sct_pkt_begin(&p, SCT_TYPE_CONTROL);
	sct_pkt_add1(&p, SCT_F_PROCESS_MODE, 0x02);	// layer 2
	sct_pkt_add1(&p, SCT_F_REPORT_FIELD, (uint8_t)(REPORT_FIELD | s.port->report_field));
	add_start_fields(&p, false);
	if (!send(&p))
		return false;
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	send_config(&p, cfg);
	add_start_fields(&p, true);
	if (!send(&p))
		return false;
	s.running = send_rx_mode();
	audio_rx();	// a start in the middle of a transmission
	for (uint8_t i = 0; s.running && i < s.port->codec_start_count; i++)
		codec_write(&s.port->codec_start[i]);
	// The tx_fields once now as well: the first transmission after the chip's boot whose own came first went out
	// 0.65 ms late on the repeater's slot grid, every later one on time (modem-sct.md 5)
	if (s.running)
		send_tx_fields();
	return s.running;
}

static void x_stop(void)
{
	sct_pkt_t p;

	s.running = false;
	rx_voice_clear();
	s.tx = TX_IDLE;
	s.q_count = 0;
	s.waiting = false;
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	add_work_mode(&p, 0);
	send(&p);
	audio_rx();
}

static void x_configure(const dmr_config_t *cfg)
{
	sct_pkt_t p;

	s.cfg = *cfg;
	s.restart_ms = s.now;	// the chip answers these with ACK 0x01 too: not a restart
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	send_config(&p, cfg);
	send(&p);
}

static void x_rx_restart(void)
{
	s.slot_verified = false;
	s.slot_hunt = 0;
	s.rehunted = false;
	s.sync = DMR_SYNC_NONE;
	s.fec_err = false;
	if (s.tx == TX_IDLE)
		send_rx_mode();
}

static bool x_slot_verified(void)
{
	return s.slot_verified;
}

static void x_rx_lock_slot(int8_t ts)
{
	s.rx_slot = ts;
	if (s.tx == TX_IDLE && s.running) {
		sct_pkt_t p;

		sct_pkt_begin(&p, SCT_TYPE_DMR);
		sct_pkt_add1(&p, SCT_F_DMR_CALL_SLOT, rx_slot_byte());
		send(&p);
	}
}

static bool x_tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	(void)access;	// the chip has no polite access of its own; the stack's channel check stands
	if (!s.running || s.tx != TX_IDLE)
		return false;
	sct_pkt_t p;
	// TX on the slot asked; with aligned timing, take the TX timing from what is received
	uint8_t slot = (uint8_t)(0x80 | (timing == DMR_TIMING_ALIGNED ? 0x40 : 0) | (!s.cfg.repeater ? 0 : ts == 2 ? 2 : 1));

	if (!send_tx_fields())
		return false;
	audio_tx();
	// The slot first: the Packet Interface's transmit sequence, and WORK_MODE is what starts it
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add1(&p, SCT_F_DMR_CALL_SLOT, slot);
	bool sent = send(&p);

	if (sent) {
		s.mode_wait = true;	// before the send: its answer can be the next thing in
		s.mode_ms = s.now;
		sct_pkt_begin(&p, SCT_TYPE_CONTROL);
		add_work_mode(&p, 2);
		sent = send(&p);
	}
	if (!sent) {
		s.mode_wait = false;
		audio_rx();
		return false;
	}
	s.tx = TX_ON;
	s.tx_voice = false;
	s.tx_sent = 0;
	s.tx_edges = s.port->burst_edges != NULL ? s.port->burst_edges() : 0;
	rx_voice_clear();
	s.cr_want = 0;	// the board's next reading after the transmission says what is there
	s.tx_ts = (int8_t)(ts == 2 ? 2 : 1);
	s.q_count = 0;
	s.waiting = false;
	s.paced = false;
	s.voice_on = false;
	s.lc_new = false;
	return true;
}

static bool x_tx_queue(const dmr_tx_burst_t *b)
{
	if (s.tx == TX_IDLE || s.tx == TX_DRAIN || s.q_count >= TXQ_DEPTH)
		return false;
	s.q[(s.q_head + s.q_count) % TXQ_DEPTH] = *b;
	s.q_count++;
	tx_pump();
	return true;
}

static uint8_t x_tx_queue_space(void)
{
	return (uint8_t)(TXQ_DEPTH - s.q_count);
}

static void x_tx_end(void)
{
	if (s.tx == TX_ON)
		s.tx = TX_ENDING;
	tx_step();
}

static void x_tx_abort(void)
{
	if (s.tx == TX_IDLE)
		return;
	s.tx = TX_IDLE;
	s.q_count = 0;
	s.waiting = false;
	s.voice_on = false;
	rx_after_tx();
	audio_rx();
}

static void x_set_embedded_lc(const uint8_t lc[9])
{
	memcpy(s.emb_lc, lc, LC_BYTES);
	s.lc_new = true;
}

static void x_set_cc_filter(int8_t cc)
{
	// The chip's color code is its TX color code too, so it stays at the channel's; with -1 the stack filters
	s.cc_filter = cc;
}

static bool x_is_transmitting(void)
{
	return s.tx != TX_IDLE;
}

// --- Locked entries ---

static void lock(void)
{
	if (s.port != NULL && s.port->lock != NULL)
		s.port->lock(s.port->ctx);
}

static void unlock(void)
{
	if (s.port != NULL && s.port->unlock != NULL)
		s.port->unlock(s.port->ctx);
}

void sct_dmr_packet(const uint8_t *pkt, uint16_t len, uint32_t now_ms)
{
	lock();
	if (s.tx == TX_DRAIN)
		s.drain_heard = true;
	packet(pkt, len, now_ms);
	unlock();
}

void sct_dmr_poll(uint32_t now_ms)
{
	lock();
	s.now = now_ms;
	if (s.restart && s.running && (int32_t)(now_ms - s.restart_ms) >= SCT_DMR_RESTART_MS) {
		dmr_config_t c = s.cfg;
		bool retried = s.start_retried;

		s.restart = false;
		if (s.tx != TX_IDLE)
			tx_reject();
		x_start(&c, s.cb, s.cb_ctx);
		s.start_retried = retried;
	}
	tx_step();
	rx_voice_step();
	carrier_step();
	unlock();
}

void sct_dmr_carrier(uint8_t ready)
{
	lock();
	s.cr_used = true;
	if (s.tx == TX_IDLE) {	// while transmitting the receiver hears this radio: kept at "lost"
		s.cr_want = ready <= 2 ? ready : 1;
		if (ready != 0)
			s.cr_seen = s.now;
	}
	carrier_step();
	unlock();
}

#define LOCKED_VOID(name, params, args) \
	static void m_##name params { lock(); x_##name args; unlock(); }
#define LOCKED(type, name, params, args) \
	static type m_##name params { lock(); type r = x_##name args; unlock(); return r; }

LOCKED(bool, start, (const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx), (cfg, cb, ctx))
LOCKED_VOID(stop, (void), ())
LOCKED_VOID(configure, (const dmr_config_t *cfg), (cfg))
LOCKED_VOID(rx_restart, (void), ())
LOCKED_VOID(rx_lock_slot, (int8_t ts), (ts))
LOCKED(bool, slot_verified, (void), ())
LOCKED(bool, tx_begin, (dmr_timing_t timing, int8_t ts, dmr_access_t access), (timing, ts, access))
LOCKED(bool, tx_queue, (const dmr_tx_burst_t *b), (b))
LOCKED(uint8_t, tx_queue_space, (void), ())
LOCKED_VOID(tx_end, (void), ())
LOCKED_VOID(tx_abort, (void), ())
LOCKED_VOID(set_embedded_lc, (const uint8_t lc[9]), (lc))
LOCKED_VOID(set_cc_filter, (int8_t cc), (cc))
LOCKED(bool, is_transmitting, (void), ())

static void m_health(dmr_modem_health_t *h)
{
	lock();
	memset(h, 0, sizeof(*h));
	h->stalls = s.st.drain_silent;
	h->stuck = s.st.rx_late;
	h->first_ms = s.st.first_ms;
	for (unsigned i = 0; i < 4u; i++)
		h->first_regs[i] = s.st.first[i];
	unlock();
}

const hal_dmr_modem_t sct_dmr_modem = {
	.start = m_start,
	.stop = m_stop,
	.configure = m_configure,
	.rx_restart = m_rx_restart,
	.rx_lock_slot = m_rx_lock_slot,
	.slot_verified = m_slot_verified,
	.tx_begin = m_tx_begin,
	.tx_queue = m_tx_queue,
	.tx_queue_space = m_tx_queue_space,
	.tx_end = m_tx_end,
	.tx_abort = m_tx_abort,
	.set_embedded_lc = m_set_embedded_lc,
	.set_cc_filter = m_set_cc_filter,
	.is_transmitting = m_is_transmitting,
	.health = m_health,
};

void sct_dmr_init(const sct_dmr_port_t *port)
{
	memset(&s, 0, sizeof(s));
	s.port = port;
	s.cc_filter = -1;
	s.cr_sent = CR_UNSENT;
	if (port != NULL)
		memcpy(s.option, port->call_option, sizeof(s.option));
}

bool sct_dmr_field_begin(uint8_t type, uint8_t id, const uint8_t *data, uint8_t n, bool read)
{
	sct_pkt_t p;
	bool ok;

	lock();
	sct_pkt_begin(&p, read ? (uint8_t)(type | SCT_TYPE_READ) : type);
	if (read) {
		sct_pkt_add_raw(&p, &id, 1);
		if (n)			// SCRATCH (never commit): carry read args, e.g. 0x3F READ_MM address
			sct_pkt_add_raw(&p, data, n);
	} else
		sct_pkt_add(&p, id, data, n);
	s.field.state = FIELD_WAIT;
	s.field.id = id;
	s.field.read = read;
	s.field.n = 0;
	ok = s.running && send(&p);
	if (!ok)
		s.field.state = FIELD_IDLE;
	unlock();
	return ok;
}

int sct_dmr_field_poll(uint8_t out[4], uint8_t *n)
{
	int r = 0;

	lock();
	if (s.field.state == FIELD_DONE || s.field.state == FIELD_REFUSED) {
		r = s.field.state == FIELD_DONE ? 1 : -1;
		memcpy(out, s.field.data, sizeof(s.field.data));
		*n = s.field.n;
		s.field.state = FIELD_IDLE;
	}
	unlock();
	return r;
}

void sct_dmr_field_cancel(void)
{
	lock();
	s.field.state = FIELD_IDLE;
	unlock();
}

bool sct_dmr_call_option(uint8_t byte, uint8_t *value, bool write)
{
	if (byte >= sizeof(s.option))
		return false;
	lock();
	if (write) {
		s.option[byte] = *value;
		if (s.running && s.tx == TX_IDLE) {
			sct_pkt_t p;

			sct_pkt_begin(&p, SCT_TYPE_DMR);
			send_config(&p, &s.cfg);
			send(&p);
		}
	}
	*value = s.option[byte];
	unlock();
	return true;
}

void sct_dmr_get_stats(sct_dmr_stats_t *out)
{
	lock();
	*out = s.st;
	out->sending = s.waiting;
	unlock();
}

void sct_dmr_reset_stats(void)
{
	memset(&s.st, 0, sizeof(s.st));
}
