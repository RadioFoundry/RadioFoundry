// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR on the framer (dstar.h): the mode's profile and hooks, a call received from its header or joined late at a
// data synchronization, and a call sent from its preamble to its last frame.
#include "dstar.h"
#include <string.h>
#include "../air/air_syms.h"
#include "../../hal/hal_radio.h"

// ── The mode ──
// A frame is a voice frame (96 symbols); the unit the header's sync opens is that sync and the header (684). The data
// synchronization ends frame 0 of each superframe, the last frame's pattern sits 24 symbols into a frame's place
#define HEAD_SYMS	48		// where the head hook looks: the last frame's pattern is in by then
#define HDR_UNIT	(24 + DSTAR_HDR_SYMS)
#define LOST_FRAMES	(2 * DSTAR_SUPERFRAME)	// one data synchronization may be missed

static uint16_t rx_head(const air_frame_t *f, void *ctx);
static void rx_frame(const air_frame_t *f, void *ctx);
static void rx_lost(void *ctx);

const air_mode_t dstar_air_mode = {
	.name = "dstar",
	.profile = {
		.sym_rate = 4800,
		.levels = 2,
		.shape = AIR_SHAPE_GAUSS,
		.shape_param = 50,		// BT 0.5
		.dev_outer_hz = 1200,		// GMSK at 4800 bit/s: a modulation index of 0.5
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,
		.slot_syms = 0,
		.sync_every_syms = DSTAR_SUPERFRAME * DSTAR_FRAME_SYMS,
		.sync_any_syms = 0,
		.idle = 0x77,			// 1010...
		.nsyncs = 3,
		.sync = {
			// the data synchronization (4.1.2 d): 1010101010 and 1101000 twice, the last 24 bits of frame 0
			{ DSTAR_SYNC_DATA, 24, 2, 0, -(DSTAR_VOICE_BYTES * 8), { 0x77, 0x77, 0x75, 0xDF, 0xD7, 0x7F } },
			// the last frame (4.1.2 h): its last 8 bits of 1010, then 000100110101111 and 0
			{ DSTAR_SYNC_END, 24, 2, 0, -24, { 0x77, 0x77, 0xFD, 0xF5, 0xDD, 0x57 } },
			// the bit synchronization's last 9 bits and the frame synchronization (4.1.1 a, b): the header follows
			{ DSTAR_SYNC_HEADER, 24, 2, AIR_SYNC_START_ONLY, 0, { 0xDD, 0xDD, 0xD5, 0xD7, 0xDD, 0xFF } },
		},
	},
	.frame_syms = DSTAR_FRAME_SYMS,
	.head_syms = HEAD_SYMS,
	.lost_frames = LOST_FRAMES,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

// ── Receive ──
void dstar_call_rx_init(dstar_call_rx_t *c, const dstar_call_sink_t *sink, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->sink = sink;
	c->ctx = ctx;
	c->n = -1;
}

static bool synced(const air_frame_t *f, uint8_t id)
{
	return (f->flags & AIR_FRAME_NO_SYNC) == 0 && f->sync_id == id;
}

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	dstar_call_rx_t *c = ctx;

	if (synced(f, DSTAR_SYNC_END)) {
		c->stats.ends++;
		return 0;		// the transmission's end: not a frame, and the signal is gone
	}
	if (synced(f, DSTAR_SYNC_HEADER))
		return f->nsyms < HDR_UNIT ? (uint16_t)(AIR_HEAD_AGAIN | HDR_UNIT) : HDR_UNIT;
	return DSTAR_FRAME_SYMS;
}

static void start(dstar_call_rx_t *c, bool late, int16_t rssi)
{
	memset(&c->call, 0, sizeof c->call);
	dstar_slow_rx_reset(&c->slow);
	c->call.late = late;
	c->call.rssi_dbm = rssi;
	c->active = true;
	c->stats.calls++;
	if (late)
		c->stats.late++;
}

static void end(dstar_call_rx_t *c)
{
	if (!c->active)
		return;
	c->active = false;
	c->n = -1;
	if (c->sink->end)
		c->sink->end(c->ctx);
}

static void header_rx(dstar_call_rx_t *c, const air_frame_t *f)
{
	uint8_t raw[DSTAR_HDR_BYTES];
	int errs = dstar_header_decode(f->hard, f->soft, 24, &c->work, raw);

	// A new transmission: whatever was heard before it has ended
	end(c);
	start(c, false, f->rssi_dbm);
	c->n = DSTAR_SUPERFRAME - 1;	// the next frame is frame 0
	if (errs >= 0 && dstar_header_unpack(raw, &c->call.hdr)) {
		c->stats.headers++;
		c->stats.hdr_fec_bits += (uint32_t)errs;
		c->call.have_header = true;
	} else {
		c->stats.header_fail++;
	}
	if (c->sink->start)
		c->sink->start(&c->call, c->ctx);
}

static void slow_rx(dstar_call_rx_t *c, uint8_t data[DSTAR_DATA_BYTES])
{
	uint8_t done;

	dstar_data_scramble(data);
	done = dstar_slow_rx_frame(&c->slow, data, (uint8_t)c->n);
	if (done & DSTAR_SLOW_MSG) {
		c->stats.slow_msgs++;
		memcpy(c->call.msg, c->slow.msg, DSTAR_MSG_LEN);
		c->call.msg[DSTAR_MSG_LEN] = '\0';
		c->call.have_msg = true;
	}
	if (done & DSTAR_SLOW_HEADER) {
		c->stats.slow_headers++;
		if (!c->call.have_header) {
			(void)dstar_header_unpack(c->slow.hdr, &c->call.hdr);
			c->call.have_header = true;
		} else {
			done &= (uint8_t)~DSTAR_SLOW_HEADER;	// the same one again: nothing new
		}
	}
	if (done & DSTAR_SLOW_TEXT) {
		c->stats.slow_text++;
		memcpy(c->call.text, c->slow.text, DSTAR_TEXT_LEN);
	}
	if (done && c->sink->update)
		c->sink->update(&c->call, c->ctx);
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	dstar_call_rx_t *c = ctx;
	uint8_t voice[DSTAR_VOICE_BYTES], data[DSTAR_DATA_BYTES];

	if (f->offset != 0)
		return;			// (never: the buffer holds the longest unit)
	if (f->nsyms == HDR_UNIT) {
		header_rx(c, f);
		return;
	}
	if (f->nsyms != DSTAR_FRAME_SYMS)
		return;
	c->stats.frames++;
	if (synced(f, DSTAR_SYNC_DATA)) {
		c->n = 0;
		c->stats.syncs++;
	} else if (c->n >= 0) {
		c->n = (int8_t)((c->n + 1) % DSTAR_SUPERFRAME);
	}
	if (!c->active) {
		// Joined late, at a data synchronization: the header may come again in the slow data
		start(c, true, f->rssi_dbm);
		if (c->sink->start)
			c->sink->start(&c->call, c->ctx);
	}
	dstar_frame_get(f->hard, 0, voice, data);
	c->stats.voice++;
	if (c->sink->voice)
		c->sink->voice(voice, c->ctx);
	if (c->n > 0)
		slow_rx(c, data);
}

static void rx_lost(void *ctx)
{
	dstar_call_rx_t *c = ctx;

	if (c->active)
		c->stats.lost++;
	end(c);
}

// ── Transmit ──
enum { TX_START, TX_VOICE, TX_END, TX_DONE };

void dstar_call_tx_begin(dstar_call_tx_t *t, const dstar_header_t *h, const char *msg)
{
	static const uint8_t fs[2] = { 0x37, 0x05 };	// 111011001010000, sent from its first bit
	uint8_t raw[DSTAR_HDR_BYTES], pre = 0x55;

	memset(t, 0, sizeof *t);
	dstar_header_pack(h, raw);
	dstar_slow_tx_init(&t->slow, raw, msg);
	for (uint16_t i = 0; i < DSTAR_PREAMBLE_SYMS; i += 8)
		dstar_bits_put(t->syms, i, &pre, 8);
	dstar_bits_put(t->syms, DSTAR_PREAMBLE_SYMS, fs, DSTAR_FS_SYMS);
	dstar_header_encode(raw, t->syms, DSTAR_PREAMBLE_SYMS + DSTAR_FS_SYMS);
	t->state = TX_START;
	t->nsyms = t->ready = DSTAR_START_SYMS;
}

uint16_t dstar_call_tx_ready(const dstar_call_tx_t *t, uint16_t *first)
{
	*first = t->sent;
	return (uint16_t)(t->ready - t->sent);
}

const uint8_t *dstar_call_tx_syms(const dstar_call_tx_t *t)
{
	return t->syms;
}

static void tx_end_frame(dstar_call_tx_t *t)
{
	dstar_bits_put(t->syms, 0, dstar_end_bytes, DSTAR_END_SYMS);
	t->state = TX_END;
	t->nsyms = t->ready = DSTAR_END_SYMS;
	t->sent = 0;
}

void dstar_call_tx_sent(dstar_call_tx_t *t, uint16_t n)
{
	t->sent = (uint16_t)(t->sent + n);
	if (t->sent < t->nsyms)
		return;
	t->nsyms = t->ready = t->sent = 0;
	if (t->state == TX_END) {
		t->state = TX_DONE;
		return;
	}
	t->state = TX_VOICE;
	if (t->ending)
		tx_end_frame(t);
}

bool dstar_call_tx_wants_voice(const dstar_call_tx_t *t)
{
	return t->state == TX_VOICE && t->nsyms == 0 && !t->ending;
}

bool dstar_call_tx_voice(dstar_call_tx_t *t, const uint8_t voice[DSTAR_VOICE_BYTES])
{
	uint8_t data[DSTAR_DATA_BYTES];

	if (!dstar_call_tx_wants_voice(t))
		return false;
	if (t->n == 0) {
		memcpy(data, dstar_data_sync, DSTAR_DATA_BYTES);
	} else {
		dstar_slow_tx_frame(&t->slow, t->n, data);
		dstar_data_scramble(data);
	}
	dstar_frame_put(t->syms, 0, voice, data);
	t->n = (uint8_t)((t->n + 1) % DSTAR_SUPERFRAME);
	t->nsyms = t->ready = DSTAR_FRAME_SYMS;
	t->sent = 0;
	return true;
}

void dstar_call_tx_end(dstar_call_tx_t *t)
{
	if (t->ending)
		return;
	t->ending = true;
	if (t->state == TX_VOICE && t->nsyms == 0)
		tx_end_frame(t);
}

bool dstar_call_tx_done(const dstar_call_tx_t *t)
{
	return t->state == TX_DONE;
}
