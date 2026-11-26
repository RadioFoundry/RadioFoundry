// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link over the air, its pure parts (dl_dmrip.h, docs/spec/debuglink.md 7)
#include <string.h>
#include "debuglink/dl_dmrip.h"
#include "debuglink/debuglink.h"	// DL_OP_*

// --- SipHash-2-4 ---

#define ROTL(x, b)	(uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

static uint64_t get64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | p[i];
	}
	return v;
}

#define SIPROUND				\
	do {					\
		v0 += v1; v1 = ROTL(v1, 13);	\
		v1 ^= v0; v0 = ROTL(v0, 32);	\
		v2 += v3; v3 = ROTL(v3, 16);	\
		v3 ^= v2;			\
		v0 += v3; v3 = ROTL(v3, 21);	\
		v3 ^= v0;			\
		v2 += v1; v1 = ROTL(v1, 17);	\
		v1 ^= v2; v2 = ROTL(v2, 32);	\
	} while (0)

uint64_t dl_siphash(const uint8_t key[DL_DMRIP_KEY], const uint8_t *p, size_t n)
{
	uint64_t k0 = get64(key), k1 = get64(key + 8);
	uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
	uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
	uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
	uint64_t v3 = 0x7465646279746573ULL ^ k1;
	uint64_t b = (uint64_t)n << 56;
	size_t whole = n & ~(size_t)7;
	for (size_t i = 0; i < whole; i += 8) {
		uint64_t m = get64(p + i);
		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}
	for (size_t i = whole; i < n; i++) {
		b |= (uint64_t)p[i] << (8 * (i - whole));
	}
	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xFF;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	return v0 ^ v1 ^ v2 ^ v3;
}

static void put64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

// The KDF's fixed key: public, so anyone with the UID has the key (7.2)
static const uint8_t kdf_key[DL_DMRIP_KEY] = {	// "RFoundryDMRIPkey"
	0x52, 0x46, 0x6F, 0x75, 0x6E, 0x64, 0x72, 0x79, 0x44, 0x4D, 0x52, 0x49, 0x50, 0x6B, 0x65, 0x79,
};

void dl_dmrip_kdf(const uint8_t *uid, size_t n, uint8_t key[DL_DMRIP_KEY])
{
	uint8_t m[1 + 32];
	if (n > 32) {
		n = 32;
	}
	memcpy(m + 1, uid, n);
	m[0] = 0;
	put64(key, dl_siphash(kdf_key, m, n + 1));
	m[0] = 1;
	put64(key + 8, dl_siphash(kdf_key, m, n + 1));
}

// --- the datagram ---

size_t dl_dmrip_seal(const uint8_t key[DL_DMRIP_KEY], const dl_dmrip_msg_t *m, uint8_t *out)
{
	out[0] = DL_DMRIP_VERSION;
	out[1] = m->flags;
	out[2] = (uint8_t)m->id;
	out[3] = (uint8_t)(m->id >> 8);
	out[4] = (uint8_t)(m->id >> 16);
	dl_put32(out + 5, m->nonce);
	dl_put32(out + 9, m->counter);
	out[13] = m->chunk;
	size_t n = DL_DMRIP_HDR + m->body_len;
	put64(out + n, dl_siphash(key, out, n));
	return n + DL_DMRIP_TAG;
}

bool dl_dmrip_open(const uint8_t key[DL_DMRIP_KEY], const uint8_t *d, size_t n, dl_dmrip_msg_t *m)
{
	if (n < DL_DMRIP_HDR + DL_DMRIP_TAG || n > DL_DMRIP_DGRAM_MAX || d[0] != DL_DMRIP_VERSION) {
		return false;
	}
	size_t body = n - DL_DMRIP_HDR - DL_DMRIP_TAG;
	uint8_t tag[DL_DMRIP_TAG];
	put64(tag, dl_siphash(key, d, n - DL_DMRIP_TAG));
	uint8_t diff = 0;
	for (unsigned i = 0; i < DL_DMRIP_TAG; i++) {
		diff |= (uint8_t)(tag[i] ^ d[n - DL_DMRIP_TAG + i]);	// no early exit: the time says nothing
	}
	if (diff != 0) {
		return false;
	}
	m->flags = d[1];
	m->id = (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16);
	m->nonce = dl_get32(d + 5);
	m->counter = dl_get32(d + 9);
	m->chunk = d[13];
	m->body = d + DL_DMRIP_HDR;
	m->body_len = (uint16_t)body;
	return true;
}

int dl_dmrip_check(const uint8_t key[DL_DMRIP_KEY], const uint8_t *d, size_t n, uint32_t my_id, dl_dmrip_msg_t *m)
{
	if (!dl_dmrip_open(key, d, n, m) || (m->flags & DL_DMRIP_F_REPLY) != 0) {
		return DL_DMRIP_BAD;
	}
	// DMR IDs are unique: one radio answers, never a group or the all-call (0xFFFFFF and the like are not ours)
	if (my_id == 0 || my_id > 0xFFFFFEu || m->id != my_id) {
		return DL_DMRIP_NOT_OURS;
	}
	return DL_DMRIP_ACCEPT;
}

int dl_dmrip_replay(dl_dmrip_replay_t *r, uint32_t nonce, uint32_t counter, uint64_t body)
{
	if (r->nonce == 0 || nonce != r->nonce) {
		return DL_DMRIP_OLD;
	}
	if (counter == r->last) {
		return body == r->body ? DL_DMRIP_REPEAT : DL_DMRIP_OLD;
	}
	if ((int32_t)(counter - r->last) > 0) {
		r->last = counter;
		r->body = body;
		return DL_DMRIP_NEW;
	}
	return DL_DMRIP_OLD;
}

bool dl_dmrip_frame(const uint8_t *b, size_t n, uint8_t *cmd, uint8_t *seq, const uint8_t **payload, uint16_t *len)
{
	if (n < DL_HDR + 2u || b[0] != DL_SOF1 || b[1] != DL_SOF2) {
		return false;
	}
	uint16_t l = get16(b + 4);
	if (l > DL_REQ_MAX || n != DL_HDR + l + 2u) {
		return false;
	}
	uint16_t crc = dl_crc16(DL_CRC_INIT, b + 2, 4u + l);
	if (crc != get16(b + DL_HDR + l)) {
		return false;
	}
	*cmd = b[2];
	*seq = b[3];
	*payload = b + DL_HDR;
	*len = l;
	return true;
}

// --- what the air may ask (7.3) ---

// RADIO's operations the air may ask directly: those that leave the radio on the link's channel, mode, color code and
// slot, still able to answer, change nothing the owner's own transmissions carry, and store nothing but the speaker
// volume (a radio without a knob keeps the step, as from its UI; the next VOLUME sets it back). Channel, zone, VFO,
// frequency, bandwidth, clear and the DMR fields move the radio (a detour's items, which it undoes); scan and roaming
// move it by themselves; the tone burst, DTMF and the PA operations transmit or set the drive; privacy holds packet
// data back and the test TX limit stops the replies; the USB and PPP operations have nothing to do with the air and
// store settings. The voice test signal, the mic gain, the encode options and the reference trim stay until a restart,
// which the air can't ask, and change every later transmission (the trim also moves the link's channel): cable only
static bool radio_op_direct(const uint8_t *p, uint16_t len)
{
	switch (p[0]) {
	case DL_OP_POWER:
	case DL_OP_SQUELCH:
	case DL_OP_TONE:
	case DL_OP_MONITOR:
	case DL_OP_REPLY_CANCEL:
	case DL_OP_HOTSPOT_EXIT:
	case DL_OP_DMR_TARGET:		// the DMR talk target: voice calls, not the link's packets
	case DL_OP_DMR_CONTACT:
	case DL_OP_DMR_CALL_BACK:
	case DL_OP_DMR_TARGET_CLEAR:
	case DL_OP_BEEP:		// a sound that ends by itself
	case DL_OP_VIBRATE:
	case DL_OP_VOLUME:		// the speaker's level, for tuning it against a microphone
	case DL_OP_VOICE_CAPTURE:	// a recording of what the radio hears
		return true;
	case DL_OP_SETTING:
		// Zone share offers only. The stored settings (TX inhibit, battery save, the time-out among them) could cut
		// the link for good, so they go in a detour, which puts them back
		return len >= 2 && p[1] == DL_SETTING_ZS_OFFERS;
	default:
		return false;
	}
}

// A chip register request the air may send: never the SoC's (its windows hold the flash controller and the reset
// register), and on the RF chips a write only as a detour item, whose return sets the mode's registers again. A wrong
// write outside a detour could leave the radio deaf to the link until someone restarts it by hand
static bool chip_reg_air(const uint8_t *p, uint16_t len, bool detour)
{
	return len >= 1 && p[0] != DL_CHIP_SOC && (len == 3 || (detour && len == 7));
}

// AUDIO_PROBE: its status and the audio path's level registers (a write as a detour item, as chip_reg_air). Not the
// stream it can take over, the modem RAM or the pins
static bool audio_probe_air(const uint8_t *p, uint16_t len, bool detour)
{
	return len >= 1 && (p[0] == DL_PROBE_OP_STATUS || p[0] == DL_PROBE_OP_REG_READ ||
			    (detour && p[0] == DL_PROBE_OP_REG_WRITE));
}

bool dl_dmrip_air_allowed(uint8_t cmd, const uint8_t *req, uint16_t len)
{
	switch (cmd) {
	case DL_CMD_PING:
	case DL_CMD_VERSION:
	case DL_CMD_GET_DIAG:
	case DL_CMD_RESET_COUNTERS:
	case DL_CMD_SCREENSHOT:
	case DL_CMD_GET_LOG:
	case DL_CMD_GET_VIEW:
	case DL_CMD_IDDB:		// read only
	case DL_CMD_DETOUR:		// the link-cutting changes, under its deadline
		return true;
	case DL_CMD_DMRIP:
		return len >= 1 && req[0] == DL_DMRIP_OP_STATUS;	// the key is set over the cable only
	case DL_CMD_RADIO:
		return len >= 1 && radio_op_direct(req, len);
	case DL_CMD_CHIP_REG:		// a register read; a write only inside a detour
		return chip_reg_air(req, len, false);
	case DL_CMD_AUDIO_PROBE:
		return audio_probe_air(req, len, false);
	default:
		// RADIO (but radio_op_direct's), PTT, KEY: only inside a detour. INFO_CHIP (the UID gives the default key:
		// 7.2) and READ_MEM (its windows hold the codec on a codec-merged image), nothing being encrypted on the air.
		// CODEPLUG, CPKV, T3, STORAGE, HWOPT (flash and stored keys), REBOOT, STUB, CRASH, WATCHDOG, RX2 (a
		// restart or the radio handed over), DMR_DATA, APRS, SNMP, SPLIT, AIR (transmissions or modes outside the
		// radio core's checks), SATELLITE (stored records), and SUPPORT and anything newer until listed here: never
		return false;
	}
}

uint32_t dl_dmrip_reply_ip(uint32_t id, uint32_t src_ip)
{
	if (src_ip == 0 || src_ip >= 0xE0000000u || (src_ip >> 24) == 127) {
		return 0x0C000000u | (id & 0xFFFFFFu);	// 12.<ID>
	}
	return src_ip;
}

// --- the detour (7.4) ---

// RADIO's operations a detour may run: the changes its return undoes, and beeps
static bool radio_op_allowed(const uint8_t *p, uint8_t len)
{
	if (len < 1) {
		return false;
	}
	uint8_t op = p[0];
	if (op >= DL_OP_CHANNEL_STEP && op <= DL_OP_CLEAR_TEMP) {	// channel, zone, VFO, frequency, power, squelch,
		return true;							// bandwidth, tone, monitor, clear
	}
	if (op >= DL_OP_DMR_TARGET && op <= DL_OP_DMR_TARGET_CLEAR) {	// DMR talk target: an ID, a contact, call back,
		return true;							// clear
	}
	switch (op) {
	case DL_OP_DMR_FIELDS:		// mode, color code, timeslot
	case DL_OP_VFO_COPY:
	case DL_OP_P25:			// RAM settings, kept and put back
	case DL_OP_NXDN:
		return true;
	case DL_OP_BEEP:		// a sound, which ends by itself: nothing to undo (the speaker tests on air)
		return true;
	case DL_OP_SETTING: {		// radio_settings_t's fields only (TX inhibit among them): the settings come back
		if (len < 2) {
			return false;
		}
		uint8_t id = p[1];
		return (id >= 1 && id <= 28) || id == 30 || id == 33 || id == 34;
	}
	default:
		return false;
	}
}

bool dl_detour_item_allowed(uint8_t cmd, const uint8_t *p, uint8_t len)
{
	switch (cmd) {
	case DL_DETOUR_ITEM_WAIT:
		return len == 2;
	case DL_CMD_PTT:
		return len >= 1;
	case DL_CMD_RADIO:
		return radio_op_allowed(p, len);
	case DL_CMD_CHIP_REG:		// a level tuned in the detour's mode, which entering the mode sets again, so a
		return chip_reg_air(p, len, true);	// write before the detour wouldn't stay
	case DL_CMD_AUDIO_PROBE:
		return audio_probe_air(p, len, true);
	default:
		return false;
	}
}

uint8_t dl_detour_items_check(const uint8_t *items, uint16_t n)
{
	uint16_t at = 0;
	while (at < n) {
		uint16_t left = (uint16_t)(n - at);
		if (left < 2u || (unsigned)(left - 2u) < items[at + 1]) {
			return DL_ST_BAD_ARG;
		}
		if (!dl_detour_item_allowed(items[at], items + at + 2, items[at + 1])) {
			return DL_ST_REFUSED;
		}
		at = (uint16_t)(at + 2u + items[at + 1]);
	}
	return DL_ST_OK;
}

void dl_detour_init(dl_detour_t *d)
{
	memset(d, 0, sizeof(*d));
}

static void set_phase(dl_detour_t *d, uint8_t phase, uint32_t now_ms)
{
	d->phase = phase;
	d->phase_ms = now_ms;
}

static void begin_return(dl_detour_t *d, const dl_detour_ops_t *o, bool asked, uint32_t now_ms)
{
	d->asked = asked;
	d->n = d->at = 0;		// items not run yet are dropped
	o->release();
	o->ret();
	set_phase(d, DL_DETOUR_RETURNING, now_ms);
}

// Seconds from now, capped (0: refused by the caller)
static uint32_t deadline(uint16_t s, uint32_t now_ms)
{
	return now_ms + (uint32_t)(s > DL_DETOUR_MAX_S ? DL_DETOUR_MAX_S : s) * 1000u;
}

size_t dl_detour_request(dl_detour_t *d, const dl_detour_ops_t *o, const uint8_t *req, uint16_t len, bool clear_wait,
			 uint8_t *out, size_t cap, uint32_t now_ms)
{
	if (len < 1 || cap < 16) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	bool active = d->phase != DL_DETOUR_IDLE && d->phase != DL_DETOUR_RETURNING;
	switch (req[0]) {
	case DL_DETOUR_RUN: {
		if (len < 3) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		uint16_t s = get16(req + 1);
		if (s == 0 || s > DL_DETOUR_MAX_S) {
			out[0] = DL_ST_BAD_ARG;	// a longer one is refused, not cut: the host asked for something else
			return 1;
		}
		uint8_t st = dl_detour_items_check(req + 3, (uint16_t)(len - 3u));
		if (st != DL_ST_OK) {
			out[0] = st;
			return 1;
		}
		if (d->phase == DL_DETOUR_RETURNING) {
			out[0] = DL_ST_BUSY;	// the last one is still going back
			return 1;
		}
		d->n = (uint8_t)(len - 3u);
		memcpy(d->items, req + 3, d->n);
		d->at = 0;
		d->index = 0;
		d->failed = 0;
		d->busy_since = 0;
		d->wait_until = now_ms;
		d->deadline_ms = deadline(s, now_ms);
		d->runs++;
		o->mark(s * 1000u + DL_DETOUR_BACKSTOP_MS);	// armed already: only the core's backstop moves
		if (!active) {
			d->result = DL_ST_OK;
			d->asked = false;
			set_phase(d, DL_DETOUR_ARMING, now_ms);
		}
		d->clear_wait = clear_wait;
		if (clear_wait && d->phase == DL_DETOUR_RUNNING) {
			set_phase(d, DL_DETOUR_CLEAR, now_ms);	// the new items wait for this reply too
		}
		out[0] = DL_ST_OK;
		out[1] = (uint8_t)s;
		out[2] = (uint8_t)(s >> 8);
		return 3;
	}
	case DL_DETOUR_EXTEND: {
		if (len < 3 || get16(req + 1) == 0 || get16(req + 1) > DL_DETOUR_MAX_S) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		if (!active) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		uint16_t s = get16(req + 1);
		d->deadline_ms = deadline(s, now_ms);
		o->mark(s * 1000u + DL_DETOUR_BACKSTOP_MS);
		out[0] = DL_ST_OK;
		return 1;
	}
	case DL_DETOUR_RETURN:
		if (active) {
			begin_return(d, o, true, now_ms);
		}
		out[0] = DL_ST_OK;
		return 1;
	case DL_DETOUR_STATUS: {
		uint32_t left = 0;
		if (active && (int32_t)(d->deadline_ms - now_ms) > 0) {
			left = d->deadline_ms - now_ms;
		}
		out[0] = DL_ST_OK;
		out[1] = d->phase;
		dl_put32(out + 2, left);
		out[6] = d->index;
		out[7] = d->failed;
		out[8] = d->result;
		dl_put32(out + 9, d->runs);
		return 13;
	}
	default:
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
}

// The next item, if its time came: false when there is none left
static bool next_item(dl_detour_t *d, const dl_detour_ops_t *o, uint32_t now_ms)
{
	if (d->at >= d->n) {
		return false;
	}
	if ((int32_t)(now_ms - d->wait_until) < 0) {
		return true;
	}
	uint8_t cmd = d->items[d->at], len = d->items[d->at + 1];
	const uint8_t *p = d->items + d->at + 2;
	uint8_t st;
	if (cmd == DL_DETOUR_ITEM_WAIT) {
		d->wait_until = now_ms + get16(p);
		st = DL_ST_OK;
	} else {
		st = o->dispatch(cmd, p, len);
	}
	if (st == DL_ST_BUSY) {
		// The radio requests' rate limit, an injected key still held: again shortly, for a while
		if (d->busy_since == 0) {
			d->busy_since = now_ms | 1u;
		}
		if ((now_ms - (d->busy_since & ~1u)) < DL_DETOUR_BUSY_MS) {	// bit 0 only says "set"
			return true;
		}
	}
	d->busy_since = 0;
	if (st != DL_ST_OK) {
		d->result = st;
		d->failed = (uint8_t)(d->index + 1u);
		begin_return(d, o, false, now_ms);	// a test that could not be set up doesn't stay away
		return false;
	}
	d->at = (uint8_t)(d->at + 2u + len);
	d->index++;
	return true;
}

void dl_detour_poll(dl_detour_t *d, const dl_detour_ops_t *o, uint32_t now_ms)
{
	switch (d->phase) {
	case DL_DETOUR_ARMING:
		if (o->armed()) {
			bool wait = d->clear_wait && o->clear != NULL;
			d->wait_until = now_ms;
			set_phase(d, wait ? DL_DETOUR_CLEAR : DL_DETOUR_RUNNING, now_ms);
		} else if ((now_ms - d->phase_ms) >= DL_DETOUR_ARM_MS) {
			d->result = DL_ST_BUSY;	// settings waiting for their save, a transmission, hotspot mode
			d->n = d->at = 0;
			o->ret();		// in case the mark landed after all
			set_phase(d, DL_DETOUR_IDLE, now_ms);
		}
		break;
	case DL_DETOUR_CLEAR:
		if ((now_ms - d->phase_ms) >= DL_DETOUR_CLEAR_MS || o->clear == NULL || o->clear()) {
			d->wait_until = now_ms;
			set_phase(d, DL_DETOUR_RUNNING, now_ms);
		}
		if ((int32_t)(now_ms - d->deadline_ms) >= 0) {
			begin_return(d, o, false, now_ms);
		}
		break;
	case DL_DETOUR_RUNNING:
		if (!o->armed()) {
			d->n = d->at = 0;	// the core went back by itself (its backstop): nothing to do
			o->release();
			set_phase(d, DL_DETOUR_IDLE, now_ms);
			break;
		}
		if ((int32_t)(now_ms - d->deadline_ms) >= 0) {
			begin_return(d, o, false, now_ms);
			break;
		}
		(void)next_item(d, o, now_ms);
		break;
	case DL_DETOUR_RETURNING:
		if (!o->armed()) {
			set_phase(d, DL_DETOUR_IDLE, now_ms);
		} else if ((now_ms - d->phase_ms) >= DL_DETOUR_REPOST_MS) {
			o->release();
			o->ret();		// a request lost to a full queue, or a transmission still ending
			d->phase_ms = now_ms;
		}
		break;
	default:
		break;
	}
}
