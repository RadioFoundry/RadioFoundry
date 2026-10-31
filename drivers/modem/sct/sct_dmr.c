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
	bool waiting;		// a frame is out, its answer not in yet
	uint32_t sent_ms;	// when it went (or when a paced voice burst was taken)
	uint32_t next_ms;	// the next burst goes no earlier than this
	bool paced;		// next_ms is set
	bool voice_on;		// the chip runs the voice of this transmission
	bool audio_tx;		// codec_tx went: codec_rx is due at the return to receive
	bool lc_new;		// set_embedded_lc gave an LC not yet sent
	uint8_t emb_lc[LC_BYTES];
	uint32_t done_ms;	// TX_DRAIN: report TX_DONE by then if the chip stays silent

	// RX
	uint8_t rx_cc;
	uint8_t sync;		// dmr_sync_family_t from the last DMR_SLOT_FOUND
	int8_t ts;
	bool fec_err;		// the last NEW_EVENT was a data sync with an FEC error
	bool rx_voice;		// a received voice call runs: voice bursts are reported in its slots
	uint8_t rx_index;	// the next one's index, 0..5 (A..F)
	uint32_t rx_next_ms;
	uint32_t counter;	// burst counter for event args, never 0
	uint32_t now;

	sct_dmr_stats_t st;
} s;

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

// DMR_CALL_SLOT for receive: 1, 2, or 3 for both
static uint8_t rx_slot_byte(void)
{
	return (uint8_t)(s.rx_slot == 1 || s.rx_slot == 2 ? s.rx_slot : 3);
}

static bool send_rx_mode(void)
{
	sct_pkt_t p;

	sct_pkt_begin(&p, SCT_TYPE_DMR);
	add_work_mode(&p, 1);
	sct_pkt_add1(&p, SCT_F_DMR_CALL_SLOT, rx_slot_byte());
	return send(&p);
}

static void send_config(sct_pkt_t *p, const dmr_config_t *cfg)
{
	uint8_t opt[5];
	uint8_t bcd[4];

	memcpy(opt, s.port->call_option, sizeof(opt));
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
	return true;
}

// The next queued burst, when the chip is ready for it
static void tx_pump(void)
{
	while (s.q_count != 0 && !s.waiting && (!s.paced || (int32_t)(s.now - s.next_ms) >= 0)) {
		const dmr_tx_burst_t *b = &s.q[s.q_head];

		if (b->kind == DMR_BURST_VOICE) {
			// The LC for the embedded signaling goes with the call's first voice burst, and again at a burst A
			// after a new one. Every other voice burst is the chip's: take it in its slot
			bool lc = !s.voice_on || (s.lc_new && b->voice_index % 6 == 0);

			s.voice_on = true;
			if (lc) {
				s.lc_new = false;
				q_pop();
				if (!send_frame(SCT_PROP_VOICE, s.emb_lc, LC_BYTES))
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

static void tx_finish(void)
{
	s.tx = TX_IDLE;
	s.waiting = false;
	s.paced = false;
	s.voice_on = false;
	s.q_count = 0;
	send_rx_mode();
	audio_rx();
	emit(DMR_EV_TX_DONE, s.tx_ts, next_counter(), NULL);
}

// A frame was taken: the next one goes a slot after this one
static void frame_taken(void)
{
	if (!s.waiting)
		return;
	s.waiting = false;
	s.st.tx_acks++;
	s.next_ms = s.sent_ms + SCT_DMR_SLOT_MS;
	s.paced = true;
	slot_started();
}

static void tx_step(void)
{
	if (s.tx == TX_IDLE)
		return;
	if (s.waiting && (int32_t)(s.now - s.sent_ms) >= SCT_DMR_ACK_MS) {
		s.waiting = false;
		s.st.tx_timeouts++;
	}
	tx_pump();
	if (s.tx == TX_ENDING && s.q_count == 0 && !s.waiting) {
		s.tx = TX_DRAIN;
		s.done_ms = s.now + SCT_DMR_DONE_MS;
	}
	if (s.tx == TX_DRAIN && (int32_t)(s.now - s.done_ms) >= 0)
		tx_finish();
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
	b.in_step = true;
	b.crc_ok = !s.fec_err;
	if (s.fec_err)
		s.st.rx_fec++;
	s.fec_err = false;
	s.st.rx_frames++;

	if ((prop & SCT_PROP_VOICE) != 0 || (prop & SCT_PROP_TYPE) == DMR_BURST_VOICE_LC_HEADER ||
	    (prop & SCT_PROP_TYPE) == DMR_BURST_TERMINATOR_LC) {
		// A voice call: its header, its terminator, or the LC of a superframe (a late entry). The stack gets an LC
		// header for the last, so it shows the call; the chip plays the voice
		if (n < LC_BYTES)
			return;
		b.kind = (prop & SCT_PROP_VOICE) != 0 ? DMR_BURST_VOICE_LC_HEADER : (prop & SCT_PROP_TYPE);
		memcpy(b.lc, data, LC_BYTES);
		memcpy(b.payload, data, LC_BYTES);
		b.len = LC_BYTES;
		b.lc_valid = true;
		if (b.kind == DMR_BURST_TERMINATOR_LC) {
			s.rx_voice = false;
		} else if (!s.rx_voice || (prop & SCT_PROP_VOICE) == 0) {
			// The chip voices the call from here on and says no more until it ends: the stack gets a voice
			// burst per slot, as it would from a burst modem, so it holds the call. A repeated header (when
			// the board has every one reported) starts the count again: the voice follows the last header
			s.rx_voice = true;
			s.rx_index = (prop & SCT_PROP_VOICE) != 0 ? 1 : 0;	// a late entry's LC came with burst A
			s.rx_next_ms = s.now + SCT_DMR_SLOT_MS;
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
	for (int n = 0; s.rx_voice && s.tx == TX_IDLE && (int32_t)(s.now - s.rx_next_ms) >= 0 && n < 4; n++) {
		dmr_burst_t b;

		memset(&b, 0, sizeof(b));
		b.kind = DMR_BURST_VOICE;
		b.voice_index = s.rx_index;
		b.sync = s.sync;
		b.cc = s.rx_cc;
		b.ts = s.ts;
		b.crc_ok = true;
		b.in_step = true;
		b.len = DMR_VOICE_BYTES;
		s.rx_index = (uint8_t)((s.rx_index + 1) % 6);
		s.rx_next_ms += SCT_DMR_SLOT_MS;
		s.st.rx_voice++;
		emit(DMR_EV_BURST_RX, b.ts, next_counter(), &b);
	}
	if (s.rx_voice && (int32_t)(s.now - s.rx_next_ms) >= 0)
		s.rx_next_ms = s.now + SCT_DMR_SLOT_MS;	// fell behind (a stalled task): no burst storm
}

static void rx_slot_found(uint8_t v)
{
	s.st.last_slot_found = v;
	if (v & 0x80) {
		s.sync = DMR_SYNC_BS;
		s.ts = (int8_t)((v & 0x04) ? 2 : 1);	// TC: the outbound burst after the CACH
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

static void rx_ack(uint8_t id, uint8_t code)
{
	if (id == SCT_F_DATA_FRAME) {
		if (code == SCT_ACK_OK)
			frame_taken();
		else {
			s.st.naks++;
			s.st.last_nak = code;
			s.waiting = false;	// refused: the next one may go
		}
		return;
	}
	if (code != SCT_ACK_OK) {
		s.st.naks++;
		s.st.last_nak = code;
	}
}

static void rx_status(uint8_t code)
{
	switch (code) {
	case SCT_ACK_OK:
	case SCT_ACK_RESET_READY:
		break;
	case SCT_ACK_FAR_DONE:
		// The frame (or the call's last one) has gone to the far end
		frame_taken();
		if (s.tx == TX_DRAIN)
			tx_finish();
		break;
	default:
		s.st.naks++;
		s.st.last_nak = code;
		if (s.waiting) {
			s.waiting = false;
			s.st.tx_timeouts++;
		}
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
		s.rx_voice = false;
		if (s.tx == TX_IDLE)
			emit(DMR_EV_SYNC_LOST, s.ts, next_counter(), NULL);
		s.sync = DMR_SYNC_NONE;
		break;
	default:
		break;
	}
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
	s.sync = DMR_SYNC_NONE;
	s.ts = 0;
	s.rx_slot = 0;
	s.rx_voice = false;
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	sct_pkt_add1(&p, SCT_F_PROCESS_MODE, 0x02);	// layer 2
	sct_pkt_add1(&p, SCT_F_REPORT_FIELD, (uint8_t)(REPORT_FIELD | s.port->report_field));
	send_config(&p, cfg);
	for (uint8_t i = 0; i < s.port->start_count; i++) {
		const sct_field_t *f = &s.port->start_fields[i];

		sct_pkt_add(&p, f->id, f->data, f->len);
	}
	if (!send(&p))
		return false;
	s.running = send_rx_mode();
	audio_rx();	// a start in the middle of a transmission
	return s.running;
}

static void x_stop(void)
{
	sct_pkt_t p;

	s.running = false;
	s.rx_voice = false;
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
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	send_config(&p, cfg);
	send(&p);
}

static void x_rx_restart(void)
{
	s.sync = DMR_SYNC_NONE;
	s.fec_err = false;
	if (s.tx == TX_IDLE)
		send_rx_mode();
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
	uint8_t slot = (uint8_t)(0x80 | (timing == DMR_TIMING_ALIGNED ? 0x40 : 0) | (ts == 2 ? 2 : 1));

	audio_tx();
	sct_pkt_begin(&p, SCT_TYPE_DMR);
	add_work_mode(&p, 2);
	sct_pkt_add1(&p, SCT_F_DMR_CALL_SLOT, slot);
	if (!send(&p)) {
		audio_rx();
		return false;
	}
	s.tx = TX_ON;
	s.rx_voice = false;
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
	send_rx_mode();
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
	packet(pkt, len, now_ms);
	unlock();
}

void sct_dmr_poll(uint32_t now_ms)
{
	lock();
	s.now = now_ms;
	tx_step();
	rx_voice_step();
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
LOCKED(bool, tx_begin, (dmr_timing_t timing, int8_t ts, dmr_access_t access), (timing, ts, access))
LOCKED(bool, tx_queue, (const dmr_tx_burst_t *b), (b))
LOCKED(uint8_t, tx_queue_space, (void), ())
LOCKED_VOID(tx_end, (void), ())
LOCKED_VOID(tx_abort, (void), ())
LOCKED_VOID(set_embedded_lc, (const uint8_t lc[9]), (lc))
LOCKED_VOID(set_cc_filter, (int8_t cc), (cc))
LOCKED(bool, is_transmitting, (void), ())

const hal_dmr_modem_t sct_dmr_modem = {
	.start = m_start,
	.stop = m_stop,
	.configure = m_configure,
	.rx_restart = m_rx_restart,
	.rx_lock_slot = m_rx_lock_slot,
	.tx_begin = m_tx_begin,
	.tx_queue = m_tx_queue,
	.tx_queue_space = m_tx_queue_space,
	.tx_end = m_tx_end,
	.tx_abort = m_tx_abort,
	.set_embedded_lc = m_set_embedded_lc,
	.set_cc_filter = m_set_cc_filter,
	.is_transmitting = m_is_transmitting,
};

void sct_dmr_init(const sct_dmr_port_t *port)
{
	memset(&s, 0, sizeof(s));
	s.port = port;
	s.cc_filter = -1;
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
