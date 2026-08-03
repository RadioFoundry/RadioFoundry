// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The simplex DMR hotspot engine (see hotspot.h)
#include <string.h>
#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "hotspot/hotspotUplink.h"
#include "hotspot/hotspotData.h"
#include "hotspot/dmrDataFrame.h"
#include "dmr/dmrData.h"
#include "dmr/fec/dmrFec.h"
#include "dmr/call/dmr_lc.h"

#define VOICE_RING		24	// host frames buffered for RF: 1.44 s of voice
#define PREBUFFER_FRAMES	5	// a call keys up with more than 4 frames buffered, or on its terminator
#define PREBUFFER_MS		240	// or once its first frame has waited this long (a slow host)
#define NET_LOST_MS		500	// no frame from the host for this long in a call: end it with our terminator
#define EVENT_QUEUE		16	// modem events between the driver's task and ours (480 ms of bursts)
#define CARRIER_HOLD_MS		200	// carrier detect stays on this long after the last burst heard
#define SPACE_MARGIN		2	// frames the host may have written while our status was on its way (spec 2.4)
#define RSSI_UHF_HZ		300000000u	// the AT1846S RSSI scale changes here (docs/spec/mmdvm-protocol.md 5.4)

#define LC_LEN			9
#define FLCO_PRIVATE		0x03

// ---- State ----

typedef struct {
	uint8_t kind;		// dmr_burst_kind_t
	uint8_t voice_index;
	uint8_t len;
	uint8_t payload[DMR_VOICE_BYTES];
} tx_item_t;

typedef struct {
	uint8_t kind;
	int8_t ts;
	bool has_burst;
	uint32_t arg;
	dmr_burst_t burst;
} queued_event_t;

typedef enum { TX_IDLE, TX_VOICE, TX_DATA, TX_ENDING } tx_state_t;

static const hal_dmr_modem_t *modem;
static const hotspot_radio_ops_t *radio;
static hotspot_send_fn send_up;
static bool running;
static bool enter_tried;		// enter() was refused at enter_ms: wait HOTSPOT_ENTER_RETRY_MS before the next try
static uint32_t enter_ms;

static uint8_t cc = 1;
static bool simplex = true;
static uint32_t rx_hz, tx_hz;
static uint8_t rf_level = 255;

// Host -> RF
static DMR_DATA_BUFFER tx_item_t ring[VOICE_RING];		// big buffers in CCM RAM where there is one (never read before written)
static uint8_t ring_head, ring_count;
static tx_state_t tx_state;
static bool tx_is_data;
static uint32_t first_frame_ms, last_net_ms;
static bool enq_open;			// a call from the host is open on the queue side (no terminator yet)
static uint8_t enq_lc[LC_LEN];
static uint8_t enq_index;		// last voice burst index queued
static uint8_t cur_lc[LC_LEN];		// the call on air
static DMR_DATA_BUFFER dmrBurst_t data_store[DMR_DATA_MAX_BURSTS];
static const dmrBurst_t *data_list;
static int data_count, data_next;
static bool data_done;
static bool tx_overflow;

// Modem events (single producer: the driver's task; single consumer: ours)
static DMR_DATA_BUFFER queued_event_t events[EVENT_QUEUE];
static volatile uint8_t ev_in, ev_out;
static volatile bool rx_overflow;

// RF -> host
static hotspotUplink_t up;
static uint32_t last_rx_ms;
static uint32_t poll_ms;		// the time of the last hotspot_poll()
static bool heard_any;
static bool uplink_on = true;		// false in the POCSAG state: heard, not passed up
static uint8_t sf_frag[4][4];		// the embedded signalling of B-E for the superframe going up
static uint8_t alias_lc[LC_LEN];	// an embedded LC heard that isn't the call's (talker alias, GPS): sent next
static bool alias_pending;

static hotspot_view_t view;
static hotspot_stats_t stats;

static void changed(void)
{
	if ((radio != NULL) && (radio->changed != NULL)) {
		radio->changed();
	}
}

static void note_call(const uint8_t *lc, bool from_net)
{
	view.group = (lc[0] & 0x3F) != FLCO_PRIVATE;
	view.dst = ((uint32_t)lc[3] << 16) | ((uint32_t)lc[4] << 8) | lc[5];
	view.src = ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 8) | lc[8];
	view.from_net = from_net;
}

// ---- Frame <-> burst ----

// The 216 voice bits of a 264-bit burst: bits 0-107 and 156-263
static void frame_to_voice(const uint8_t *frame, uint8_t *voice)
{
	memcpy(voice, frame, 13);
	voice[13] = (uint8_t)((frame[13] & 0xF0) | (frame[19] & 0x0F));
	memcpy(voice + 14, frame + 20, 13);
}

static const uint8_t MS_VOICE_SYNC[6] = { 0x7F, 0x7D, 0x5D, 0xD5, 0x7D, 0xFD };

// A voice burst for the host, well formed as a transmitter makes it (docs/spec/mmdvm-protocol.md 2.8): A gets the MS
// voice sync; B-F the EMB (our colour code, PI 0, the LCSS of the position: B first, C-D continuation, E last,
// F single) with its QR(16,7,6) parity, B-E the four fragments of the superframe's embedded LC, F null embedded
// signalling
static void voice_to_frame(const uint8_t *voice, uint8_t index, uint8_t *frame)
{
	uint8_t middle[6] = { 0, 0, 0, 0, 0, 0 };

	if (index == 0) {
		memcpy(middle, MS_VOICE_SYNC, 6);
	} else {
		uint16_t emb = dmrFecEmbEncode(cc, false, dmr_emb_lcss(index));

		middle[0] = (uint8_t)(emb >> 8);
		if (index <= 4) {
			memcpy(middle + 1, sf_frag[index - 1], 4);
		}
		middle[5] = (uint8_t)emb;
	}
	memcpy(frame, voice, 13);
	frame[13] = (uint8_t)((voice[13] & 0xF0) | (middle[0] >> 4));
	frame[14] = (uint8_t)((middle[0] << 4) | (middle[1] >> 4));
	frame[15] = (uint8_t)((middle[1] << 4) | (middle[2] >> 4));
	frame[16] = (uint8_t)((middle[2] << 4) | (middle[3] >> 4));
	frame[17] = (uint8_t)((middle[3] << 4) | (middle[4] >> 4));
	frame[18] = (uint8_t)((middle[4] << 4) | (middle[5] >> 4));
	frame[19] = (uint8_t)((middle[5] << 4) | (voice[13] & 0x0F));
	memcpy(frame + 20, voice + 14, 13);
}

// The superframe's embedded LC, chosen at its burst A: a talker alias or GPS block heard in the last superframe, else
// the call's LC
static void superframe_start(void)
{
	const uint8_t *lc = alias_pending ? alias_lc : hotspotUplinkLc(&up);

	dmr_emb_lc_encode(lc, sf_frag);
	alias_pending = false;
}

// The RSSI bytes after an uplink burst: 00, then the AT1846S raw scale rfgw converts back to dBm (raw - 151 at
// 300 MHz and up, -164 + raw x 32 / 27 below; docs/spec/mmdvm-protocol.md 5.4). False when the RSSI is unknown.
// Radios with another RF chip use the same scale, so the host's conversion stays right for every radio.
static bool rssi_raw(int16_t dbm, uint8_t *raw)
{
	int v;

	if ((dbm == 0) && (radio != NULL) && (radio->rssi_dbm != NULL)) {
		dbm = radio->rssi_dbm();
	}
	if ((dbm == 0) || (rx_hz == 0)) {
		return false;
	}
	if (rx_hz >= RSSI_UHF_HZ) {
		v = dbm + 151;
	} else {
		v = ((dbm + 164) * 27 + 16) / 32;
	}
	*raw = (uint8_t)((v < 0) ? 0 : ((v > 255) ? 255 : v));
	return true;
}

// One burst up; rssi_dbm 0 for bursts we made (headers from the embedded LC, silence fill)
static void send_frame(uint8_t control, const uint8_t *frame, int16_t rssi_dbm, bool heard)
{
	uint8_t payload[1 + MMDVM_DMR_FRAME_LEN + 2];
	uint8_t len = 1 + MMDVM_DMR_FRAME_LEN;
	uint8_t raw;

	payload[0] = control;
	memcpy(payload + 1, frame, MMDVM_DMR_FRAME_LEN);
	if (heard && rssi_raw(rssi_dbm, &raw)) {
		payload[len++] = 0;
		payload[len++] = raw;
	}
	send_up(MMDVM_DMR_DATA2, payload, len);
	stats.rf_frames_up++;
}

static void send_lost(void)
{
	send_up(MMDVM_DMR_LOST2, NULL, 0);
	stats.rf_lost++;
}

static void send_lc_frame(uint8_t data_type, const uint8_t *lc, int16_t rssi_dbm, bool heard)
{
	dmrBurst_t b;
	uint8_t frame[MMDVM_DMR_FRAME_LEN];

	memset(frame, 0, sizeof(frame));
	b.dataType = data_type;
	b.length = 12;
	b.flags = 0;
	dmr_lc_encode_full(lc, (data_type == DT_VOICE_LC_HEADER) ? DMR_RS_MASK_VOICE_HEADER : DMR_RS_MASK_TERMINATOR,
			   b.payload);
	dmrDataBurstToFrame(&b, cc, frame);
	send_frame((uint8_t)(MMDVM_DMR_SYNC_DATA | data_type), frame, rssi_dbm, heard);
}

static void send_voice(const uint8_t *voice, uint8_t index, int16_t rssi_dbm, bool heard)
{
	uint8_t frame[MMDVM_DMR_FRAME_LEN];

	if (index == 0) {
		superframe_start();
	}
	voice_to_frame(voice, index, frame);
	send_frame((index == 0) ? MMDVM_DMR_SYNC_AUDIO : index, frame, rssi_dbm, heard);
}

// ---- Setup ----

void hotspot_set_radio(const hotspot_radio_ops_t *ops)
{
	radio = ops;
}

static void reset_tx(void)
{
	ring_head = 0;
	ring_count = 0;
	tx_state = TX_IDLE;
	tx_is_data = false;
	enq_open = false;
	data_list = NULL;
	data_count = 0;
	data_next = 0;
	data_done = false;
	hotspotDataReset(data_store);
}

static void reset_uplink(void)
{
	ev_out = ev_in;
	hotspotUplinkReset(&up);
	heard_any = false;
	alias_pending = false;
	memset(sf_frag, 0, sizeof(sf_frag));
}

static void reset_all(void)
{
	reset_tx();
	reset_uplink();
}

void hotspot_init(const hal_dmr_modem_t *m, hotspot_send_fn send)
{
	modem = m;
	send_up = send;
	running = false;
	cc = 1;
	simplex = true;
	rx_hz = 0;
	tx_hz = 0;
	rf_level = 255;
	enter_tried = false;
	uplink_on = true;
	memset(&stats, 0, sizeof(stats));
	memset(&view, 0, sizeof(view));
	reset_all();
}

static dmr_config_t modem_config(void)
{
	dmr_config_t c = {
		.own_id = 0,			// the hotspot passes every call
		.cc = (int8_t)cc,
		.access = DMR_ACCESS_IMPOLITE,	// the host does its own listen-before-talk (carrier detect)
		.repeater = false,		// simplex, DMO framing
	};
	return c;
}

bool hotspot_start(uint32_t now_ms)
{
	if (running) {
		return true;
	}
	if ((modem == NULL) || (radio == NULL) || (radio->enter == NULL)) {
		return false;
	}
	if (enter_tried && ((now_ms - enter_ms) < HOTSPOT_ENTER_RETRY_MS)) {
		return false;	// the radio refused a moment ago: don't hammer it on every frame (enter must not block)
	}
	enter_tried = true;
	enter_ms = now_ms;
	stats.enter_attempts++;
	if (!radio->enter(rx_hz)) {
		return false;
	}
	reset_all();
	dmr_config_t c = modem_config();
	if (!modem->start(&c, hotspot_modem_event, NULL)) {
		radio->leave();
		return false;
	}
	enter_tried = false;
	modem->rx_lock_slot(0);
	modem->set_cc_filter((int8_t)cc);
	if (rx_hz != 0) {
		radio->set_freq(rx_hz, tx_hz);
	}
	if (radio->set_power != NULL) {
		radio->set_power(rf_level);
	}
	running = true;
	view.running = true;
	changed();
	return true;
}

void hotspot_stop(void)
{
	enter_tried = false;	// a new session tries at once
	uplink_on = true;
	if (!running) {
		return;
	}
	if (tx_state != TX_IDLE) {
		modem->tx_abort();
	}
	modem->stop();
	running = false;
	reset_all();
	view.running = false;
	view.transmitting = false;
	view.receiving = false;
	radio->leave();
	changed();
}

bool hotspot_running(void)
{
	return running;
}

void hotspot_config(uint8_t color_code, bool is_simplex, uint16_t tx_delay_ms)
{
	(void)tx_delay_ms;	// the modem keys from its own slot timing
	cc = color_code & 0x0F;
	simplex = is_simplex;	// duplex + RX = TX will select SFR once it is ported (mmdvm_ext_t.config_changed)
	view.cc = cc;
	if (running) {
		dmr_config_t c = modem_config();
		modem->configure(&c);
		modem->set_cc_filter((int8_t)cc);
	}
}

bool hotspot_set_freq(uint32_t rx, uint32_t tx, uint8_t level)
{
	if (running) {
		// A retune drops what is in flight (docs/spec/gotchas.md 8.6: the host waits for idle first)
		if (tx_state != TX_IDLE) {
			modem->tx_abort();
		}
		reset_tx();
		// The call being heard ends here for the host (it has no RF watchdog), before the ACK; what was heard on
		// the old frequency and is still queued never goes up after it (docs/spec/mmdvm-protocol.md 2.7)
		if (hotspotUplinkCallOpen(&up)) {
			send_lost();
		}
		reset_uplink();
		if (!radio->set_freq(rx, tx)) {
			return false;
		}
		if (radio->set_power != NULL) {
			radio->set_power(level);
		}
		modem->rx_restart();
	}
	rx_hz = rx;
	tx_hz = tx;
	rf_level = level;
	view.rx_hz = rx;
	view.tx_hz = tx;
	changed();
	return true;
}

// ---- Host -> RF ----

static tx_item_t *ring_push(void)
{
	tx_item_t *it = &ring[(ring_head + ring_count) % VOICE_RING];

	ring_count++;
	return it;
}

static void ring_pop(void)
{
	ring_head = (uint8_t)((ring_head + 1) % VOICE_RING);
	ring_count--;
}

static bool ring_has_terminator(void)
{
	for (uint8_t i = 0; i < ring_count; i++) {
		if (ring[(ring_head + i) % VOICE_RING].kind == DMR_BURST_TERMINATOR_LC) {
			return true;
		}
	}
	return false;
}

// Headers and terminators go to the modem as their 9-byte LC: the modem adds the RS(12,9) with the burst type's
// mask (every DMR modem can; not all can send a host-made checksum)
static void push_lc(uint8_t kind, const uint8_t *lc)
{
	tx_item_t *it = ring_push();

	it->kind = kind;
	it->voice_index = 0;
	it->len = LC_LEN;
	memcpy(it->payload, lc, LC_LEN);
}

uint8_t hotspot_net_frame(uint8_t control, const uint8_t *frame, uint32_t now_ms)
{
	uint8_t c = hotspotNetFrameControl(control, frame);
	uint8_t index;

	stats.net_frames++;
	if (c & MMDVM_DMR_SYNC_DATA) {
		uint8_t dt = c & 0x0F;
		uint8_t info[12];

		switch (dt) {
		case DT_VOICE_LC_HEADER: {
			dmrFecBptcDecode(frame, info);
			bool other_call = enq_open && (memcmp(enq_lc, info, LC_LEN) != 0);
			// Another call's header while one is still queued (it lost its terminator, or the host sends back to
			// back): finish that one with our terminator first (docs/spec/gotchas.md 8.3)
			if ((ring_count + (other_call ? 2 : 1)) > VOICE_RING) {
				tx_overflow = true;
				return MMDVM_NAK_FULL;
			}
			if (other_call) {
				push_lc(DMR_BURST_TERMINATOR_LC, enq_lc);
			}
			if ((ring_count == 0) && (tx_state == TX_IDLE)) {
				first_frame_ms = now_ms;
			}
			push_lc(DMR_BURST_VOICE_LC_HEADER, info);
			memcpy(enq_lc, info, LC_LEN);
			enq_open = true;
			enq_index = 5;	// burst A next
			last_net_ms = now_ms;
			return 0;
		}
		case DT_TERMINATOR_WITH_LC:
			if (!enq_open) {
				return 0;	// the second and third terminators of a call, or one without a call
			}
			if (ring_count >= VOICE_RING) {
				tx_overflow = true;
				return MMDVM_NAK_FULL;
			}
			push_lc(DMR_BURST_TERMINATOR_LC, enq_lc);
			enq_open = false;
			last_net_ms = now_ms;
			return 0;
		case DT_VOICE_PI_HEADER:
		case DT_IDLE:
			return 0;		// taken and dropped: the radio makes its own (gotchas 8.2)
		default: {
			uint8_t r = hotspotDataQueue(dt, frame, now_ms);

			if (r == HOTSPOT_DATA_NAK_FULL) {
				tx_overflow = true;
			}
			return r;
		}
		}
	}

	if (c & MMDVM_DMR_SYNC_AUDIO) {
		index = 0;
	} else if (((c & 0x0F) >= 1) && ((c & 0x0F) <= 5)) {
		index = c & 0x0F;
	} else {
		index = (uint8_t)((enq_index + 1) % 6);	// MMDVMHost's control byte 0: B-F follow in order
	}

	if (!enq_open) {
		stats.net_dropped++;	// voice without its header: nothing to key up with
		return 0;
	}
	if (ring_count >= VOICE_RING) {
		tx_overflow = true;
		return MMDVM_NAK_FULL;
	}
	tx_item_t *it = ring_push();
	it->kind = DMR_BURST_VOICE;
	it->voice_index = index;
	it->len = DMR_VOICE_BYTES;
	frame_to_voice(frame, it->payload);
	enq_index = index;
	last_net_ms = now_ms;
	return 0;
}

void hotspot_net_abort(void)
{
	if (running && (tx_state != TX_IDLE)) {
		modem->tx_abort();
	}
	reset_tx();
}

// SET_MODE idle (docs/spec/mmdvm-protocol.md 2.6): nothing new goes out; a call on air ends with its terminator after
// what the modem already holds (under a superframe), a data transmission after the burst on air; the rest is dropped
void hotspot_net_idle(void)
{
	tx_state_t st = tx_state;

	if (!running || (st == TX_IDLE)) {
		reset_tx();		// buffering only: nothing on air yet
		return;
	}
	ring_head = 0;
	ring_count = 0;
	enq_open = false;
	hotspotDataReset(data_store);
	if (st == TX_VOICE) {
		push_lc(DMR_BURST_TERMINATOR_LC, cur_lc);	// feed() queues it and ends the transmission
	} else if (st == TX_DATA) {
		data_count = data_next;				// feed() ends the transmission
	}
}

void hotspot_set_uplink(bool on)
{
	if (on == uplink_on) {
		return;
	}
	if (!on && running && hotspotUplinkCallOpen(&up)) {
		send_lost();
	}
	uplink_on = on;
	if (running) {
		reset_uplink();
	}
}

bool hotspot_cwid(const uint8_t *callsign, uint8_t len)
{
	stats.cwid_requests++;
	if (running && (radio->cw_id != NULL) && radio->cw_id((const char *)callsign, len)) {
		return true;
	}
	stats.cwid_unsent++;
	return false;
}

static bool queue_item(const tx_item_t *it)
{
	dmr_tx_burst_t b;

	b.kind = it->kind;
	b.voice_index = it->voice_index;
	b.host_crc = false;
	b.sync_override = DMR_SYNC_NONE;
	b.len = it->len;
	memcpy(b.payload, it->payload, it->len);
	return modem->tx_queue(&b);
}

// The data list (hotspotData.c) going out: bursts with their info bits as the host made them, CRCs included
bool hotspotDataTxStart(const dmrBurst_t *bursts, int count)
{
	if (!running || (tx_state != TX_IDLE) || (ring_count > 0)) {
		return false;
	}
	if (!modem->tx_begin(DMR_TIMING_OWN, 0, DMR_ACCESS_IMPOLITE)) {
		stats.tx_rejected++;
		return false;
	}
	data_list = bursts;
	data_count = count;
	data_next = 0;
	data_done = false;
	tx_is_data = true;
	tx_state = TX_DATA;
	return true;
}

bool hotspotDataTxDone(void)
{
	return data_done;
}

static void end_transmission(void)
{
	modem->tx_end();
	tx_state = TX_ENDING;
}

// Drop the call at the head of the ring, up to and including its terminator
static void drop_call(void)
{
	while (ring_count > 0) {
		uint8_t kind = ring[ring_head].kind;

		ring_pop();
		if (kind == DMR_BURST_TERMINATOR_LC) {
			return;
		}
	}
	enq_open = false;
}

static void feed(uint32_t now_ms)
{
	switch (tx_state) {
	case TX_IDLE:
		// Voice the host sent without a header (or a stray terminator) can't start a transmission
		while ((ring_count > 0) && (ring[ring_head].kind != DMR_BURST_VOICE_LC_HEADER)) {
			ring_pop();
			stats.net_dropped++;
		}
		if (ring_count == 0) {
			return;
		}
		if ((ring_count < PREBUFFER_FRAMES) && !ring_has_terminator() &&
		    ((now_ms - first_frame_ms) < PREBUFFER_MS)) {
			return;		// buffering
		}
		memcpy(cur_lc, ring[ring_head].payload, LC_LEN);
		modem->set_embedded_lc(cur_lc);
		if (!modem->tx_begin(DMR_TIMING_OWN, 0, DMR_ACCESS_IMPOLITE)) {
			stats.tx_rejected++;
			drop_call();
			return;
		}
		tx_is_data = false;
		tx_state = TX_VOICE;
		stats.net_calls++;
		note_call(cur_lc, true);
		/* fall through */
	case TX_VOICE:
		while ((ring_count > 0) && (modem->tx_queue_space() > 0)) {
			const tx_item_t *it = &ring[ring_head];
			bool last = it->kind == DMR_BURST_TERMINATOR_LC;

			if (!queue_item(it)) {
				return;
			}
			ring_pop();
			if (last) {
				end_transmission();
				return;
			}
		}
		// The host went quiet mid-call without a terminator: end the call with ours
		if ((ring_count == 0) && ((now_ms - last_net_ms) > NET_LOST_MS) && (modem->tx_queue_space() > 0)) {
			tx_item_t t = { .kind = DMR_BURST_TERMINATOR_LC, .voice_index = 0, .len = LC_LEN };

			memcpy(t.payload, cur_lc, LC_LEN);
			if (queue_item(&t)) {
				enq_open = false;
				end_transmission();
			}
		}
		return;
	case TX_DATA:
		while ((data_next < data_count) && (modem->tx_queue_space() > 0)) {
			const dmrBurst_t *d = &data_list[data_next];
			dmr_tx_burst_t b;

			b.kind = d->dataType;
			b.voice_index = 0;
			b.host_crc = (d->flags & DMR_BURST_FLAG_MCU_CRC) != 0;
			b.sync_override = DMR_SYNC_NONE;
			b.len = d->length;
			memcpy(b.payload, d->payload, d->length);
			if (!modem->tx_queue(&b)) {
				return;
			}
			data_next++;
		}
		if (data_next >= data_count) {
			end_transmission();
		}
		return;
	case TX_ENDING:
		return;		// until DMR_EV_TX_DONE
	}
}

// ---- Modem events ----

void hotspot_modem_event(const dmr_event_t *ev, void *ctx)
{
	(void)ctx;
	if (ev->kind == DMR_EV_SLOT_TICK) {
		return;		// informative only
	}
	uint8_t in = ev_in;
	uint8_t next = (uint8_t)((in + 1) % EVENT_QUEUE);

	if (next == ev_out) {
		rx_overflow = true;
		stats.events_dropped++;
		return;
	}
	queued_event_t *q = &events[in];
	q->kind = ev->kind;
	q->ts = ev->ts;
	q->arg = ev->arg;
	q->has_burst = (ev->kind == DMR_EV_BURST_RX) && (ev->burst != NULL);
	if (q->has_burst) {
		q->burst = *ev->burst;
	}
	__sync_synchronize();	// the entry is complete before the index says so
	ev_in = next;
}

static bool is_data_with_crc(uint8_t kind)
{
	return (kind == DMR_BURST_PI_HEADER) || (kind == DMR_BURST_CSBK) || (kind == DMR_BURST_MBC_HEADER) ||
	       (kind == DMR_BURST_DATA_HEADER);
}

static void rf_burst(const dmr_burst_t *b, uint32_t now_ms)
{
	hotspotUplinkBurst_t u;
	uint8_t fill = 0;

	if (tx_state != TX_IDLE) {
		return;		// simplex: what the chip reports while we send is our own echo or garbage (dmr-modem.md 6.5)
	}
	stats.rf_bursts++;
	last_rx_ms = now_ms;
	heard_any = true;
	if (!uplink_on) {
		return;		// POCSAG state: carrier detect only
	}

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
		// A talker alias or GPS block in this superframe's embedded LC (the modem gives it with F): the host gets
		// it in the next superframe's B-E, so the alias reaches it (the modem doesn't give the raw fragments)
		if (b->lc_valid) {
			uint8_t flco = b->lc[0] & 0x3F;

			if ((flco >= DMR_FLCO_TA_HEADER) && (flco <= DMR_FLCO_GPS)) {
				memcpy(alias_lc, b->lc, LC_LEN);
				alias_pending = true;
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
		if (is_data_with_crc(b->kind) && !b->crc_ok) {
			return;
		}
		if (hotspotUplinkData(&up, b->kind, now_ms) && (b->len <= DMR_BURST_PAYLOAD_MAX)) {
			dmrBurst_t d;
			uint8_t frame[MMDVM_DMR_FRAME_LEN];

			memset(frame, 0, sizeof(frame));
			d.dataType = b->kind;
			d.length = b->len;
			d.flags = 0;
			memcpy(d.payload, b->payload, b->len);
			if (dmrDataBurstToFrame(&d, cc, frame)) {
				send_frame((uint8_t)(MMDVM_DMR_SYNC_DATA | b->kind), frame, b->rssi_dbm, true);
			}
		}
		return;
	}

	uint8_t actions = hotspotUplinkBurst(&up, &u, &fill);
	const uint8_t *lc = hotspotUplinkLc(&up);

	if (actions & HOTSPOT_UP_LOST) {
		send_lost();
		alias_pending = false;
	}
	if (actions & HOTSPOT_UP_HEADER) {
		// A header heard is sent as heard (with its RSSI); one made from the embedded LC (late entry) is ours
		send_lc_frame(DT_VOICE_LC_HEADER, lc, b->rssi_dbm, b->kind == DMR_BURST_VOICE_LC_HEADER);
		note_call(lc, false);
		dmr_emb_lc_encode(lc, sf_frag);
	}
	if (actions & HOTSPOT_UP_FILL) {
		// The missed positions, stepping modulo 6 across a superframe boundary (E heard, F and A missed, then B)
		for (uint8_t i = fill, n = 0; (i != u.seq) && (n < 6); i = (uint8_t)((i + 1) % 6), n++) {
			send_voice(HOTSPOT_UPLINK_SILENCE, i, 0, false);
		}
	}
	if (actions & HOTSPOT_UP_VOICE) {
		send_voice(u.ambe, u.seq, b->rssi_dbm, true);
	}
	if (actions & HOTSPOT_UP_TERMINATOR) {
		send_lc_frame(DT_TERMINATOR_WITH_LC, lc, b->rssi_dbm, true);
		alias_pending = false;
	}
}

static void handle_event(const queued_event_t *e, uint32_t now_ms)
{
	switch (e->kind) {
	case DMR_EV_BURST_RX:
		if (e->has_burst) {
			rf_burst(&e->burst, now_ms);
		}
		break;
	case DMR_EV_TX_DONE:
		if (tx_state != TX_IDLE) {
			if (tx_is_data) {
				data_done = true;
			}
			tx_state = TX_IDLE;
			// The next call (back to back) needs the full prebuffer again only if the host is slow
			first_frame_ms = now_ms;
		}
		break;
	case DMR_EV_TX_REJECTED:
		stats.tx_rejected++;
		if (tx_state != TX_IDLE) {
			modem->tx_abort();
			if (tx_is_data) {
				data_done = true;
			} else if (tx_state == TX_VOICE) {
				drop_call();
			}
			tx_state = TX_IDLE;
		}
		break;
	default:
		break;
	}
}

void hotspot_poll(uint32_t now_ms)
{
	poll_ms = now_ms;
	if (!running) {
		ev_out = ev_in;
		return;
	}
	while (ev_out != ev_in) {
		__sync_synchronize();
		handle_event(&events[ev_out], now_ms);
		ev_out = (uint8_t)((ev_out + 1) % EVENT_QUEUE);
	}

	feed(now_ms);
	hotspotDataTick(now_ms, (tx_state == TX_IDLE) && (ring_count == 0) && !hotspotUplinkCallOpen(&up));
	feed(now_ms);	// a data list hotspotDataTick just started

	if (hotspotUplinkLost(&up, now_ms)) {
		send_lost();
	}

	view.transmitting = hotspot_transmitting();
	view.receiving = hotspot_carrier();
}

// ---- Status ----

uint8_t hotspot_space(void)
{
	int voice = VOICE_RING - ring_count;
	int data = hotspotDataSpace();
	int s = (voice < data) ? voice : data;

	s -= SPACE_MARGIN;
	if (enq_open) {
		s--;		// room for the terminator we may have to add (a new call's header)
	}
	return (uint8_t)((s < 0) ? 0 : ((s > 255) ? 255 : s));
}

bool hotspot_transmitting(void)
{
	return running && ((tx_state != TX_IDLE) || (ring_count > 0) || hotspotDataIsBusy() || modem->is_transmitting() ||
			   ((radio->cw_busy != NULL) && radio->cw_busy()));
}

bool hotspot_carrier(void)
{
	if (!running || hotspot_transmitting()) {
		return false;
	}
	if (hotspotUplinkCallOpen(&up) || (heard_any && ((poll_ms - last_rx_ms) < CARRIER_HOLD_MS))) {
		return true;
	}
	return (radio->carrier != NULL) && radio->carrier();
}

bool hotspot_rx_overflow(void)
{
	bool o = rx_overflow;

	rx_overflow = false;
	return o;
}

bool hotspot_tx_overflow(void)
{
	bool o = tx_overflow;

	tx_overflow = false;
	return o;
}

void hotspot_get_view(hotspot_view_t *v)
{
	*v = view;
}

void hotspot_get_stats(hotspot_stats_t *s)
{
	*s = stats;
}
