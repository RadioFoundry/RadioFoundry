// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A conventional NXDN voice call (nxdn.h): on receive, the frames of nxdn_rx.c become a call with its fields and its
// voice frames in order; on transmit, the frames are built and released symbol by symbol. Section numbers are NXDN
// TS 1-B's (Basic Operation) where the text says so, else TS 1-A's.

#include "nxdn.h"
#include "config.h"
#if CONFIG_NXDN_SCRAMBLER
#include "nxdn_scramble.h"
#endif

#include <string.h>

static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bit_put(uint8_t *p, uint16_t i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	p[i >> 3] = (uint8_t)(b ? (p[i >> 3] | m) : (p[i >> 3] & ~m));
}

#if CONFIG_NXDN_OTA_ALIAS
// Observed conventional PROP_FORM layout: index 1..4, four text bytes per part except the last,
// which carries two text bytes and a big-endian sum. The 14-byte text is zero padded.
static const uint8_t alias_signature[] = { NXDN_MSG_PROP_FORM, 0x68, 0x82, 0x04 };

static uint16_t alias_sum(const uint8_t *data)
{
	uint16_t sum = 0;

	for (unsigned i = 0; i < NXDN_ALIAS_LEN; i++)
		sum += data[i];
	return sum;
}

static bool alias_publish(nxdn_call_rx_t *c, nxdn_alias_rx_t *alias)
{
	if (!alias->ready || !c->call.have_vcall)
		return false;
	alias->ready = false;
	if (memcmp(c->call.alias, alias->data, NXDN_ALIAS_LEN) == 0)
		return false;
	memcpy(c->call.alias, alias->data, NXDN_ALIAS_LEN);
	return true;
}

static void alias_rx(nxdn_call_rx_t *c, nxdn_alias_rx_t *alias, const uint8_t *msg)
{
	if (!c->active || memcmp(msg, alias_signature, sizeof alias_signature) != 0)
		return;

	uint8_t index = msg[4] >> 4;
	if ((msg[4] & 0x0F) != 4 || index == 0 || index > 4)
		return;
	if (index == 1) {
		alias->next = 1;
		alias->ready = false;
	}
	if (index != alias->next) {
		alias->next = 0;
		return;
	}
	memcpy(alias->data + 4 * (index - 1), msg + 5, index == 4 ? 2u : 4u);
	alias->next = index == 4 ? 0 : (uint8_t)(index + 1);
	if (index != 4 || alias_sum(alias->data) != ((uint16_t)msg[7] << 8 | msg[8]))
		return;

	bool padding = false;
	for (unsigned i = 0; i < NXDN_ALIAS_LEN; i++) {
		uint8_t ch = alias->data[i];
		if (ch == 0)
			padding = true;
		else if (padding || ch < 0x20 || ch > 0x7E)
			return;
	}
	if (alias->data[0] == 0)
		return;
	alias->ready = true;
	if (alias_publish(c, alias) && c->sink->update)
		c->sink->update(&c->call, c->ctx);
}

// The same nine-byte message fits in SACCH. FACCH1 adds one zero byte.
static void alias_build(const nxdn_call_tx_t *t, uint8_t index, uint8_t *msg)
{
	memcpy(msg, alias_signature, sizeof alias_signature);
	msg[4] = (uint8_t)((index << 4) | 4);
	memcpy(msg + 5, t->alias + 4 * (index - 1), index == 4 ? 2u : 4u);
	if (index == 4) {
		uint16_t sum = alias_sum((const uint8_t *)t->alias);
		msg[7] = (uint8_t)(sum >> 8);
		msg[8] = (uint8_t)sum;
	}
}

static void alias_tx_put(nxdn_call_tx_t *t, uint8_t half)
{
	uint8_t index = (uint8_t)(2 * (2 - t->alias_frames) + half + 1);
	uint8_t msg[NXDN_FACCH1_BYTES] = { 0 };

	alias_build(t, index, msg);
	nxdn_facch1_put(t->frame, half, msg);
}
#endif

// ── Receive ──
static void call_start(nxdn_call_rx_t *c, const nxdn_rx_frame_t *f, bool late)
{
	memset(&c->call, 0, sizeof c->call);
	c->call.late = late;
	c->call.ran = f->sacch_errs >= 0 ? NXDN_SR_RAN(f->sacch[0]) : c->rx.ran;
	c->call.rssi_dbm = f->rssi_dbm;
#if CONFIG_NXDN_OTA_ALIAS
	c->facch_alias.next = c->sacch_alias.next = 0;
	c->facch_alias.ready = c->sacch_alias.ready = false;
	c->seen_superframe = false;
#endif
	c->active = true;
	c->stats.calls++;
	if (c->sink->start)
		c->sink->start(&c->call, c->ctx);
}

static void call_end(nxdn_call_rx_t *c)
{
	c->sf_parts = 0;
	c->sf_next = -1;
	if (!c->active)
		return;
	c->active = false;
	if (c->sink->end)
		c->sink->end(c->ctx);
}

// Voice that can be played: the call's VCALL is in (a late entry waits for a whole superframe of SACCH, TS 1-B
// 4.1.6), clear (TS 1-B 4.1.7: encrypted voice is noise without the key) and half rate (the vocoder's)
static bool playable(const nxdn_call_rx_t *c)
{
	const nxdn_call_t *call = &c->call;
	uint8_t mode = call->vcall.voice_opt & 7;

	return call->have_vcall && (call->vcall.cipher == NXDN_CIPHER_NONE ||
	       (CONFIG_NXDN_SCRAMBLER && call->vcall.cipher == NXDN_CIPHER_SCRAMBLE && c->scramble_key)) && !call->iv &&
	       (mode == NXDN_VOICE_4800_EHR || mode == NXDN_VOICE_9600_EHR);
}

// A layer 3 message from a FACCH1 or a superframe's SACCH. head: from a call's first frame (non-superframe)
static void on_msg(nxdn_call_rx_t *c, const nxdn_rx_frame_t *f, const uint8_t *msg, bool head)
{
	nxdn_vcall_t v;

	switch (nxdn_msg_type(msg)) {
	case NXDN_MSG_VCALL:
		if (!nxdn_vcall_parse(msg, &v))
			return;
		// A header after voice: a new call whose start the old one's release was lost before
		bool new_call = c->call.have_vcall && memcmp(&v, &c->call.vcall, sizeof v) != 0;
#if CONFIG_NXDN_OTA_ALIAS
		new_call = new_call || c->seen_superframe;
#endif
		if (c->active && head && new_call)
			call_end(c);
		if (!c->active)
			call_start(c, f, !head);
		if (!c->call.have_vcall || memcmp(&v, &c->call.vcall, sizeof v)) {
#if CONFIG_NXDN_OTA_ALIAS
			if (c->call.have_vcall) {
				// A new identity from SACCH or FACCH1 cannot inherit the previous caller's alias.
				c->call.alias[0] = '\0';
				c->facch_alias.next = c->sacch_alias.next = 0;
				c->facch_alias.ready = c->sacch_alias.ready = false;
			}
#endif
			c->call.vcall = v;
			c->call.have_vcall = true;
#if CONFIG_NXDN_OTA_ALIAS
			alias_publish(c, &c->facch_alias);
			alias_publish(c, &c->sacch_alias);
#endif
			if (c->sink->update)
				c->sink->update(&c->call, c->ctx);
		}
		break;
	case NXDN_MSG_VCALL_IV:
		// DES and AES calls send it (TS 1-B 4.1.5): the call is encrypted whatever else is lost
		if (c->active && !c->call.iv) {
			c->call.iv = true;
			if (c->sink->update)
				c->sink->update(&c->call, c->ctx);
		}
		break;
	case NXDN_MSG_TX_REL:
		call_end(c);
		break;
	default:
		break;
	}
}

// One SACCH of a superframe: its 18 bits go to their part of the message, which is whole once the four came in order
static void sf_part(nxdn_call_rx_t *c, const nxdn_rx_frame_t *f)
{
	uint8_t part = NXDN_SR_PART(f->sacch[0]);

	if (part == 0) {
		memset(c->sf, 0, sizeof c->sf);
		c->sf_parts = 0;
	} else if (part != c->sf_next) {
		c->sf_parts = 0;
		c->sf_next = -1;
		return;
	}
	for (uint8_t i = 0; i < 18; i++)
		bit_put(c->sf, (uint16_t)(part * 18 + i), bit_get(f->sacch, (uint16_t)(8 + i)));
	c->sf_parts |= (uint8_t)(1u << part);
	c->sf_next = (int8_t)(part + 1);
	if (part == 3 && c->sf_parts == 0x0F) {
		c->sf_next = -1;
#if CONFIG_NXDN_OTA_ALIAS
		if (nxdn_msg_type(c->sf) == NXDN_MSG_PROP_FORM) {
			alias_rx(c, &c->sacch_alias, c->sf);
			return;
		}
#endif
		on_msg(c, f, c->sf, false);
	}
}

static void on_frame(const nxdn_rx_frame_t *f, void *ctx)
{
	nxdn_call_rx_t *c = ctx;
	// A frame placed without its LICH continues the superframe: four voice channels
	int16_t lich = f->lich >= 0 ? f->lich : NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_SF, NXDN_STEAL_NONE, 0);
	uint8_t usc = NXDN_LICH_USC(lich), opt = NXDN_LICH_OPT(lich), msg[NXDN_FACCH1_BYTES];
	bool sf = usc == NXDN_USC_SACCH_SF || usc == NXDN_USC_SACCH_SF_IDLE;

	c->stats.frames++;
	if (f->lich < 0)
		c->stats.lich_fail++;
	if (f->rssi_dbm)
		c->call.rssi_dbm = f->rssi_dbm;
	if (f->sacch_errs < 0) {
		c->stats.sacch_fail++;
		c->sf_parts = 0;
		c->sf_next = -1;
	} else {
		c->stats.fec_bits += (uint32_t)f->sacch_errs;
		if (sf) {
			// A late entry: the call is there, its fields come with the superframe's last SACCH
			if (!c->active)
				call_start(c, f, true);
			sf_part(c, f);
		} else {
			c->sf_parts = 0;
			c->sf_next = -1;
		}
	}
#if CONFIG_NXDN_OTA_ALIAS
	if (sf && c->active)
		c->seen_superframe = true;
#endif
	// FACCH1 in the halves the LICH names; an idle FACCH1 frame carries nothing (TS 1-B 4.1.2.1)
	for (uint8_t h = 0; h < 2; h++) {
		int errs;

		if (opt == NXDN_STEAL_NONE || (opt == NXDN_STEAL_FIRST && h == 1) || (opt == NXDN_STEAL_SECOND && h == 0))
			continue;
		if (usc == NXDN_USC_SACCH_SF_IDLE && opt == NXDN_STEAL_BOTH)
			continue;
		errs = nxdn_facch1_get(f->hard, f->soft, h, &c->rx.work, msg);
		if (errs < 0) {
			c->stats.facch_fail++;
			continue;
		}
		c->stats.fec_bits += (uint32_t)errs;
		// (both halves are read: an encrypted call's header carries its VCALL_IV in the second, TS 1-B 4.1.5)
#if CONFIG_NXDN_OTA_ALIAS
		if (sf && nxdn_msg_type(msg) == NXDN_MSG_PROP_FORM) {
			if (msg[9] == 0)
				alias_rx(c, &c->facch_alias, msg);
			continue;
		}
#endif
		on_msg(c, f, msg, !sf);
	}
	// Voice in the halves not taken
	for (uint8_t n = 0; n < NXDN_VOICE_FRAMES; n++) {
		uint8_t ehr[NXDN_EHR_BYTES];

		if (opt == NXDN_STEAL_BOTH || (opt == NXDN_STEAL_FIRST && n < 2) || (opt == NXDN_STEAL_SECOND && n >= 2))
			continue;
		if (!c->active || !playable(c)) {
			c->stats.voice_held++;
			continue;
		}
		nxdn_vch_get(f->hard, n, ehr);
#if CONFIG_NXDN_SCRAMBLER
		if (c->call.vcall.cipher == NXDN_CIPHER_SCRAMBLE &&
		    (!sf || f->sacch_errs < 0 || !nxdn_scramble_vch(c->scramble_key, NXDN_SR_PART(f->sacch[0]), n, ehr))) {
			c->stats.voice_held++;
			continue;
		}
#endif
		c->stats.voice++;
		if (c->sink->voice)
			c->sink->voice(ehr, c->ctx);
	}
}

static void on_lost(void *ctx)
{
	call_end(ctx);
}

void nxdn_call_rx_init(nxdn_call_rx_t *c, uint8_t ran, const nxdn_call_sink_t *sink, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->sink = sink;
	c->ctx = ctx;
	c->sf_next = -1;
	// (on_frame's context is c; the framer's is c->rx, the same address)
	nxdn_rx_init(&c->rx, ran, on_frame, on_lost, c);
}

// ── Transmit (TS 1-B 4.1.3, figure 4.1-7) ──
enum { TX_PRE, TX_HEAD, TX_SF, TX_END, TX_DONE };

// A FACCH1 frame of a superframe at 9600 bit/s: the second and fourth (TS 1-B 4.1.2.1)
static bool facch_frame(const nxdn_call_tx_t *t)
{
#if CONFIG_NXDN_OTA_ALIAS
	if (t->alias_frames)
		return true;
#endif
	return t->rate9600 && (t->sf_frame & 1);
}

// The SACCH: SR, then 18 bits of msg from bit `from` (an IDLE message: from = 0 of a one-octet IDLE)
static void sacch(nxdn_call_tx_t *t, uint8_t part, const uint8_t *msg, uint16_t from)
{
	uint8_t in[NXDN_SACCH_BYTES] = { NXDN_SR(part, t->ran), 0, 0, 0 };

	for (uint8_t i = 0; i < 18; i++)
		bit_put(in, (uint16_t)(8 + i), bit_get(msg, (uint16_t)(from + i)));
	nxdn_sacch_put(t->frame, in);
}

static void tx_frame(nxdn_call_tx_t *t, uint8_t state)
{
	static const uint8_t idle[3] = { NXDN_MSG_IDLE, 0, 0 };
	const uint8_t *sf_msg = t->msg;
#if CONFIG_NXDN_OTA_ALIAS
	uint8_t alias_msg[NXDN_SACCH_MSG_BYTES];
#endif

	t->state = state;
	t->sent = 0;
	t->nvoice = 0;
	switch (state) {
	case TX_PRE:
		memcpy(t->frame, nxdn_preamble, sizeof nxdn_preamble);
		t->nsyms = t->ready = NXDN_PREAMBLE_SYMS;
		break;
	case TX_HEAD:
	case TX_END:
		// Non-superframe: SACCH IDLE alone (structure 4/4), the message twice in FACCH1
		nxdn_frame_begin(t->frame, NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_NS, NXDN_STEAL_BOTH, t->dir));
		sacch(t, 3, idle, 0);
		nxdn_facch1_put(t->frame, 0, state == TX_HEAD ? t->msg : t->rel);
		nxdn_facch1_put(t->frame, 1, state == TX_HEAD ? t->msg : t->rel);
		t->nsyms = t->ready = NXDN_FRAME_SYMS;
		break;
	case TX_SF:
		// SACCH carries one message per superframe (structure 1/4 .. 4/4).
#if CONFIG_NXDN_OTA_ALIAS
		if (t->alias_sf && !(t->alias_sf & 1)) {
			alias_build(t, t->alias_sf / 2, alias_msg);
			sf_msg = alias_msg;
		}
#endif
		t->nsyms = NXDN_FRAME_SYMS;
		if (facch_frame(t)) {
			// (what a FACCH1 frame carries in a call is open: the VCALL again, nxdn.md 9)
			nxdn_frame_begin(t->frame, NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_SF, NXDN_STEAL_BOTH, t->dir));
			sacch(t, t->sf_frame, sf_msg, (uint16_t)(18 * t->sf_frame));
#if CONFIG_NXDN_OTA_ALIAS
			if (t->alias_frames) {
				alias_tx_put(t, 0);
				alias_tx_put(t, 1);
			} else
#endif
			{
				nxdn_facch1_put(t->frame, 0, t->msg);
				nxdn_facch1_put(t->frame, 1, t->msg);
			}
			t->ready = NXDN_FRAME_SYMS;
		} else {
			// Nothing of a voice frame goes out before its first VCH: an end that comes first sends the release
			// frame right after the frame before
			nxdn_frame_begin(t->frame, NXDN_LICH(NXDN_RF_RDCH, NXDN_USC_SACCH_SF, NXDN_STEAL_NONE, t->dir));
			sacch(t, t->sf_frame, sf_msg, (uint16_t)(18 * t->sf_frame));
			t->ready = 0;
		}
		break;
	default:
		t->nsyms = t->ready = 0;
		break;
	}
}

void nxdn_call_tx_begin(nxdn_call_tx_t *t, const nxdn_vcall_t *v, uint8_t ran, uint8_t dir, bool rate9600)
{
	nxdn_vcall_t rel = *v;

	memset(t, 0, sizeof *t);
	t->ran = ran;
	t->dir = dir;
	t->rate9600 = rate9600;
	nxdn_vcall_build(v, t->msg, sizeof t->msg);
	rel.msg = NXDN_MSG_TX_REL;
	nxdn_vcall_build(&rel, t->rel, sizeof t->rel);
	tx_frame(t, TX_PRE);
}

#if CONFIG_NXDN_OTA_ALIAS
void nxdn_call_tx_alias(nxdn_call_tx_t *t, const char *alias)
{
	unsigned i = 0;

	for (; i < sizeof t->alias && alias[i]; i++) {
		uint8_t ch = (uint8_t)alias[i];
		t->alias[i] = ch >= 0x20 && ch <= 0x7E ? (char)ch : '?';
	}
	memset(t->alias + i, 0, sizeof t->alias - i);
	t->alias_frames = i ? 2 : 0;
	t->alias_sf = i ? 1 : 0;
}
#endif

uint16_t nxdn_call_tx_ready(const nxdn_call_tx_t *t, uint16_t *first)
{
	*first = t->sent;
	return (uint16_t)(t->ready - t->sent);
}

const uint8_t *nxdn_call_tx_syms(const nxdn_call_tx_t *t)
{
	return t->frame;
}

void nxdn_call_tx_sent(nxdn_call_tx_t *t, uint16_t n)
{
	t->sent = (uint16_t)(t->sent + n);
	if (t->sent < t->nsyms || t->state == TX_DONE)
		return;
	switch (t->state) {
	case TX_PRE:
		tx_frame(t, TX_HEAD);
		break;
	case TX_HEAD:
		t->sf_frame = 0;
		tx_frame(t, t->ending ? TX_END : TX_SF);
		break;
	case TX_SF:
#if CONFIG_NXDN_OTA_ALIAS
		if (t->alias_frames)
			t->alias_frames--;
#endif
		// A release may follow any frame of a superframe (TS 1-B 4.1.3)
		t->sf_frame = (uint8_t)((t->sf_frame + 1) & 3);
#if CONFIG_NXDN_OTA_ALIAS
		if (t->alias_sf && t->sf_frame == 0)
			t->alias_sf = t->alias_sf == 8 ? 1 : (uint8_t)(t->alias_sf + 1);
#endif
		tx_frame(t, t->ending ? TX_END : TX_SF);
		break;
	default:
		tx_frame(t, TX_DONE);
		break;
	}
}

bool nxdn_call_tx_wants_voice(const nxdn_call_tx_t *t)
{
	return t->state == TX_SF && !facch_frame(t) && t->nvoice < NXDN_VOICE_FRAMES;
}

bool nxdn_call_tx_voice(nxdn_call_tx_t *t, const uint8_t ehr[NXDN_EHR_BYTES])
{
	if (!nxdn_call_tx_wants_voice(t))
		return false;
#if CONFIG_NXDN_SCRAMBLER
	if (t->scramble_key) {
		uint8_t scrambled[NXDN_EHR_BYTES];
		memcpy(scrambled, ehr, sizeof scrambled);
		if (!nxdn_scramble_vch(t->scramble_key, t->sf_frame, t->nvoice, scrambled))
			return false;
		nxdn_vch_put(t->frame, t->nvoice, scrambled);
	} else
#endif
	nxdn_vch_put(t->frame, t->nvoice, ehr);
	t->nvoice++;
	t->ready = NXDN_VCH_SYM(t->nvoice);
	return true;
}

void nxdn_call_tx_end(nxdn_call_tx_t *t, const uint8_t silence[NXDN_EHR_BYTES])
{
	t->ending = true;
	// A superframe frame with nothing on air yet is not sent at all
	if (t->state == TX_SF && t->sent == 0 && (facch_frame(t) || t->nvoice == 0)) {
		tx_frame(t, TX_END);
		return;
	}
	while (nxdn_call_tx_voice(t, silence))
		;
}

bool nxdn_call_tx_done(const nxdn_call_tx_t *t)
{
	return t->state == TX_DONE;
}
