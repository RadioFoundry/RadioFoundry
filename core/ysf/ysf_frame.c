// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion framing (ysf_frame.h)
#include <string.h>
#include "config.h"
#include "ysf/ysf_frame.h"
#if CONFIG_YSF_VW
#include "ysf/ysf_vw.h"
#endif

uint32_t ysf_fich_pack(const ysf_fich_t *f)
{
	return ((uint32_t)(f->fi & 3u) << 30) | ((uint32_t)(f->cs & 3u) << 28) | ((uint32_t)(f->cm & 3u) << 26) |
	       ((uint32_t)(f->bn & 3u) << 24) | ((uint32_t)(f->bt & 3u) << 22) | ((uint32_t)(f->fn & 7u) << 19) |
	       ((uint32_t)(f->ft & 7u) << 16) | ((uint32_t)(f->dev & 1u) << 14) | ((uint32_t)(f->mr & 7u) << 11) |
	       ((uint32_t)(f->voip & 1u) << 10) | ((uint32_t)(f->dt & 3u) << 8) | ((uint32_t)(f->sql & 1u) << 7) |
	       (uint32_t)(f->sc & 0x7Fu);
}

void ysf_fich_unpack(uint32_t v, ysf_fich_t *f)
{
	f->fi = (uint8_t)((v >> 30) & 3u);
	f->cs = (uint8_t)((v >> 28) & 3u);
	f->cm = (uint8_t)((v >> 26) & 3u);
	f->bn = (uint8_t)((v >> 24) & 3u);
	f->bt = (uint8_t)((v >> 22) & 3u);
	f->fn = (uint8_t)((v >> 19) & 7u);
	f->ft = (uint8_t)((v >> 16) & 7u);
	f->dev = (uint8_t)((v >> 14) & 1u);
	f->mr = (uint8_t)((v >> 11) & 7u);
	f->voip = (uint8_t)((v >> 10) & 1u);
	f->dt = (uint8_t)((v >> 8) & 3u);
	f->sql = (uint8_t)((v >> 7) & 1u);
	f->sc = (uint8_t)(v & 0x7Fu);
}

void ysf_callsign_set(char out[YSF_CALLSIGN_LEN], const char *s)
{
	memset(out, ' ', YSF_CALLSIGN_LEN);
	for (uint32_t i = 0; s != NULL && s[i] != '\0' && i < YSF_CALLSIGN_LEN; i++) {
		char c = s[i];
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 'a' + 'A');
		}
		out[i] = (c > ' ' && c < 0x7F) ? c : ' ';
	}
}

void ysf_callsign_get(const char in[YSF_CALLSIGN_LEN], char *out)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < YSF_CALLSIGN_LEN; i++) {
		char c = in[i];
		out[i] = (c >= ' ' && c < 0x7F) ? c : ' ';	// what the air brought is shown, never trusted
		if (out[i] != ' ') {
			n = i + 1u;
		}
	}
	out[n] = '\0';
}

void ysf_csd_init(ysf_csd_t *c, const char *src, const char *dest)
{
	// Direct: no repeater callsigns or IDs, spaces as the standard asks (4.2.1 (1))
	memset(c, ' ', sizeof(*c));
	ysf_callsign_set(c->src, src);
	ysf_callsign_set(c->dest, dest);
}

void ysf_csd_radio_id(ysf_csd_t *c, const char dest_id[YSF_RADIO_ID_LEN])
{
	memcpy(c->dest, dest_id, YSF_RADIO_ID_LEN);
	memcpy(c->dest + YSF_RADIO_ID_LEN, c->rem[3], YSF_RADIO_ID_LEN);
}

void ysf_dest_show(uint8_t cm, const char dest[YSF_CALLSIGN_LEN], char *out)
{
	if (cm != YSF_CM_RADIO_ID) {
		ysf_callsign_get(dest, out);
		return;
	}
	if (memcmp(dest, YSF_RADIO_ID_ALL, YSF_RADIO_ID_LEN) == 0) {
		memcpy(out, "ALL", 4);
		return;
	}
	char id[YSF_CALLSIGN_LEN];
	memset(id, ' ', sizeof id);
	memcpy(id, dest, YSF_RADIO_ID_LEN);
	ysf_callsign_get(id, out);
}

// --- transmitter ---

// The sync's 40 bits, leftmost first
static void put_sync(uint8_t *frame)
{
	for (uint32_t i = 0; i < 40u; i++) {
		ysf_bit_put(frame, i, ((YSF_SYNC >> (39u - i)) & 1u) != 0);
	}
}

static void put_bits(uint8_t *frame, uint32_t at, const uint8_t *src, uint32_t from, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		ysf_bit_put(frame, at + i, ysf_bit(src, from + i));
	}
}

#define FICH_AT		40u	// bits
#define PAYLOAD_AT	240u
#define VD2_FT		5u	// no data: Dest, Src, Down, Up, Rem1+2, Rem3+4 (table 4-4)

static void tx_fich(ysf_tx_t *t, uint8_t fi, uint8_t fn)
{
	ysf_fich_t f = t->cfg.fich;
	uint8_t air[YSF_FICH_BITS / 8u];
	f.fi = fi;
	f.cs = YSF_CS_CALLSIGNS;
	f.bn = 0;
	f.bt = 0;
	f.fn = fn;
	f.ft = VD2_FT;		// the header's FT isn't defined; the rolling pattern's is the useful one
	f.dt = YSF_DT_VD2;
#if CONFIG_YSF_VW
	if (t->cfg.vw) {
		// Voice FR: DT 3 in every frame; the header and terminator FN 0 FT 0, the voice frames ysf_vw_tx_fn's
		f.dt = YSF_DT_VOICE_FR;
		f.fn = f.ft = 0;
		if (fi == YSF_FI_COMM) {
			ysf_vw_tx_fn(t->fn, &f.fn, &f.ft);
		}
	}
#endif
	ysf_fich_encode(ysf_fich_pack(&f), air);
	put_bits(t->frame, FICH_AT, air, 0, YSF_FICH_BITS);
}

// A header or terminator: CSD1 (destination, source) in DCH-1, CSD2 (downlink, uplink) in DCH-2, each a 360-bit unit
// cut into five 72-bit pieces (figure 4-7)
static void tx_header(ysf_tx_t *t, uint8_t fi)
{
	uint8_t u1[YSF_DCH_BITS_LONG / 8u], u2[YSF_DCH_BITS_LONG / 8u];
	uint8_t d[YSF_DCH_BYTES_LONG];

	memset(t->frame, 0, sizeof(t->frame));
	put_sync(t->frame);
	tx_fich(t, fi, 0);
	memcpy(d, t->cfg.csd.dest, YSF_CALLSIGN_LEN);
	memcpy(d + YSF_CALLSIGN_LEN, t->cfg.csd.src, YSF_CALLSIGN_LEN);
	ysf_dch_encode(d, YSF_DCH_BYTES_LONG, t->cfg.order, u1);
	memcpy(d, t->cfg.csd.down, YSF_CALLSIGN_LEN);
	memcpy(d + YSF_CALLSIGN_LEN, t->cfg.csd.up, YSF_CALLSIGN_LEN);
	ysf_dch_encode(d, YSF_DCH_BYTES_LONG, t->cfg.order, u2);
	for (uint32_t s = 0; s < YSF_SUBFRAMES; s++) {
		put_bits(t->frame, PAYLOAD_AT + ysf_sub_bit(s, 0), u1, 72u * s, 72u);
		put_bits(t->frame, PAYLOAD_AT + ysf_sub_bit(s, 72u), u2, 72u * s, 72u);
	}
}

// A V/D mode 2 frame without its voice: the DCH's ten bytes for this frame number in five 40-bit pieces; each
// subframe's voice goes in as it is reached (tx_voice), so a frame's last voice is pulled 80 ms after its first
static void tx_comm(ysf_tx_t *t)
{
	uint8_t u[YSF_DCH_BITS_SHORT / 8u];
	uint8_t d[YSF_DCH_BYTES_SHORT];
	const ysf_csd_t *c = &t->cfg.csd;

	memset(t->frame, 0, sizeof(t->frame));
	put_sync(t->frame);
	tx_fich(t, YSF_FI_COMM, t->fn);
#if CONFIG_YSF_VW
	if (t->cfg.vw) {
		ysf_vw_tx_comm(t);
		return;
	}
#endif
	switch (t->fn) {
	case 0: memcpy(d, c->dest, sizeof(d)); break;
	case 1: memcpy(d, c->src, sizeof(d)); break;
	case 2: memcpy(d, c->down, sizeof(d)); break;
	case 3: memcpy(d, c->up, sizeof(d)); break;
	case 4: memcpy(d, c->rem[0], 5); memcpy(d + 5, c->rem[1], 5); break;
	default: memcpy(d, c->rem[2], 5); memcpy(d + 5, c->rem[3], 5); break;
	}
	ysf_dch_encode(d, YSF_DCH_BYTES_SHORT, t->cfg.order, u);
	for (uint32_t s = 0; s < YSF_SUBFRAMES; s++) {
		put_bits(t->frame, PAYLOAD_AT + ysf_sub_bit(s, 0), u, 40u * s, 40u);
	}
}

static void tx_voice(ysf_tx_t *t, uint32_t sub)
{
	uint8_t bits[YSF_VOICE_BITS], air[YSF_VD2_VOICE_BITS / 8u];
	if (t->cfg.pull == NULL || !t->cfg.pull(t->cfg.ctx, bits)) {
		memcpy(bits, t->cfg.silence, sizeof(bits));
		t->silences++;
	}
	t->voice_frames++;
	ysf_vd2_voice_encode(bits, air);
	put_bits(t->frame, PAYLOAD_AT + ysf_sub_bit(sub, 40u), air, 0, YSF_VD2_VOICE_BITS);
}

static void tx_build(ysf_tx_t *t)
{
	t->pos = 0;
	t->frames++;
	switch (t->state) {
	case YSF_TX_HEADER:
		tx_header(t, YSF_FI_HEADER);
		break;
	case YSF_TX_VOICE:
		tx_comm(t);
		break;
	case YSF_TX_TERM:
		tx_header(t, YSF_FI_TERM);
		break;
	default:
		break;
	}
}

void ysf_tx_start(ysf_tx_t *t, const ysf_tx_config_t *cfg)
{
	memset(t, 0, sizeof(*t));
	t->cfg = *cfg;
	if (t->cfg.headers == 0) {
		t->cfg.headers = 1;	// N0 (4.6 (1)): at least one
	}
	t->cfg.preamble = (uint16_t)((t->cfg.preamble + 1u) & ~1u);
	if (t->cfg.preamble != 0) {
		t->state = YSF_TX_PREAMBLE;
		return;
	}
	t->state = YSF_TX_HEADER;
	tx_build(t);
}

void ysf_tx_stop(ysf_tx_t *t)
{
	t->stop = true;
}

int ysf_tx_dibit(void *tx)
{
	ysf_tx_t *t = tx;
	if (t->state == YSF_TX_DONE) {
		return -1;
	}
	if (t->state == YSF_TX_PREAMBLE) {
		// -3 first, +3 last: the frame sync's first symbol is -3, so the alternation runs on into it (11 is -3, 01 +3)
		if (t->pos < t->cfg.preamble) {
			return (t->pos++ & 1u) ? 1 : 3;
		}
		t->state = YSF_TX_HEADER;
		tx_build(t);
	}
	if (t->pos >= YSF_FRAME_DIBITS) {
		// The next frame
		switch (t->state) {
		case YSF_TX_HEADER:
			if (++t->headers_sent >= t->cfg.headers) {
				t->state = t->stop ? YSF_TX_TERM : YSF_TX_VOICE;
			}
			break;
		case YSF_TX_VOICE:
			t->fn = (uint8_t)(t->fn >= VD2_FT ? 0u : t->fn + 1u);
#if CONFIG_YSF_VW
			if (t->cfg.vw) {
				t->fn = (uint8_t)(t->fn > 2u ? 2u : t->fn);	// frames sent, held at 2 (ysf_vw_tx_fn)
			}
#endif
			if (t->stop) {
				t->state = YSF_TX_TERM;
			}
			break;
		default:
			t->state = YSF_TX_DONE;
			return -1;
		}
		tx_build(t);
	}
	// A subframe's voice is pulled as its first voice dibit is due (payload bit 144 s + 40)
	if (t->state == YSF_TX_VOICE && t->pos >= PAYLOAD_AT / 2u) {
		uint32_t pb = 2u * t->pos - PAYLOAD_AT;
#if CONFIG_YSF_VW
		if (t->cfg.vw) {
			// A full-rate frame fills its subframe: pulled as the subframe starts
			if (pb % YSF_SUB_BITS == 0u && pb / YSF_SUB_BITS >= ysf_vw_tx_first_voice(t)) {
				ysf_vw_tx_voice(t, pb / YSF_SUB_BITS);
			}
		} else
#endif
		if (pb % YSF_SUB_BITS == 40u) {
			tx_voice(t, pb / YSF_SUB_BITS);
		}
	}
	uint32_t b = 2u * t->pos++;
	return (ysf_bit(t->frame, b) ? 2 : 0) | (ysf_bit(t->frame, b + 1u) ? 1 : 0);
}

// --- receiver ---

// The sync with its levels inverted: +3 <-> -3, +1 <-> -1 flips the first bit of every dibit and keeps the second
#define SYNC_MASK	0xFFFFFFFFFFULL
#define SYNC_INV	(YSF_SYNC ^ 0xAAAAAAAAAAULL)

void ysf_rx_init(ysf_rx_t *r, const ysf_rx_cb_t *cb)
{
	memset(r, 0, sizeof(*r));
	r->cb = *cb;
}

static void rx_search(ysf_rx_t *r)
{
	r->state = YSF_RX_SEARCH;
	r->misses = 0;
	r->hard = 0;
}

static void rx_end(ysf_rx_t *r, ysf_end_t why)
{
	if (r->in_call) {
		r->in_call = false;
		if (why == YSF_END_TERMINATOR) {
			r->ends++;
		} else {
			r->losts++;
		}
		if (r->cb.end != NULL) {
			r->cb.end(r->cb.ctx, why);
		}
	}
	rx_search(r);
}

static void rx_csd(ysf_rx_t *r, uint8_t have, char *dst, const uint8_t *d, uint32_t n)
{
	memcpy(dst, d, n);
	r->call.have |= have;
}

static void rx_unit_count(ysf_rx_t *r, const ysf_unit_result_t *u)
{
	if (u->crc_ok) {
		r->csd_good++;
		if (u->order == YSF_WHITEN_ALL) {
			r->order_all++;
		}
	} else {
		r->csd_bad++;
	}
}

// The end of a frame's payload: the data units it carried
static void rx_payload_done(ysf_rx_t *r)
{
	const ysf_fich_t *f = &r->call.fich;
	ysf_csd_t *c = &r->call.csd;
	uint8_t before = r->call.have;
	uint8_t d[YSF_DCH_BYTES_LONG];
	ysf_unit_result_t u;

	if (f->fi == YSF_FI_HEADER || f->fi == YSF_FI_TERM) {
		u = ysf_dch_decode(r->soft, YSF_DCH_BYTES_LONG, d);
		rx_unit_count(r, &u);
		if (u.crc_ok) {
			rx_csd(r, YSF_HAVE_DEST, c->dest, d, YSF_CALLSIGN_LEN);
			rx_csd(r, YSF_HAVE_SRC, c->src, d + YSF_CALLSIGN_LEN, YSF_CALLSIGN_LEN);
		}
		u = ysf_dch_decode(r->soft + YSF_DCH_BITS_LONG / 2u, YSF_DCH_BYTES_LONG, d);
		rx_unit_count(r, &u);
		if (u.crc_ok) {
			rx_csd(r, YSF_HAVE_DOWN, c->down, d, YSF_CALLSIGN_LEN);
			rx_csd(r, YSF_HAVE_UP, c->up, d + YSF_CALLSIGN_LEN, YSF_CALLSIGN_LEN);
		}
	} else if (f->dt == YSF_DT_VD2) {
		u = ysf_dch_decode(r->soft, YSF_DCH_BYTES_SHORT, d);
		rx_unit_count(r, &u);
		if (u.crc_ok) {
			switch (f->fn) {
			case 0: rx_csd(r, YSF_HAVE_DEST, c->dest, d, 10); break;
			case 1: rx_csd(r, YSF_HAVE_SRC, c->src, d, 10); break;
			case 2: rx_csd(r, YSF_HAVE_DOWN, c->down, d, 10); break;
			case 3: rx_csd(r, YSF_HAVE_UP, c->up, d, 10); break;
			case 4: rx_csd(r, YSF_HAVE_REM12, c->rem[0], d, 10); break;
			case 5: rx_csd(r, YSF_HAVE_REM34, c->rem[2], d, 10); break;
			default: break;	// DT1, DT2: data, not decoded
			}
		}
	} else if (f->dt == YSF_DT_VD1) {
		u = ysf_dch_decode(r->soft, YSF_DCH_BYTES_LONG, d);
		rx_unit_count(r, &u);
		if (u.crc_ok) {
			switch (f->fn) {
			case 0:
				rx_csd(r, YSF_HAVE_DEST, c->dest, d, 10);
				rx_csd(r, YSF_HAVE_SRC, c->src, d + 10, 10);
				break;
			case 1:
				rx_csd(r, YSF_HAVE_DOWN, c->down, d, 10);
				rx_csd(r, YSF_HAVE_UP, c->up, d + 10, 10);
				break;
			case 2:
				rx_csd(r, YSF_HAVE_REM12 | YSF_HAVE_REM34, c->rem[0], d, 20);
				break;
			default:
				break;
			}
		}
#if CONFIG_YSF_VW
	} else if (f->dt == YSF_DT_VOICE_FR) {
		ysf_vw_rx_done(r);
#endif
	}
	if (r->call.have != before && r->cb.call != NULL) {
		r->cb.call(r->cb.ctx, &r->call);
	}
}

// One bit of the payload with its soft value: where it belongs by the frame's type
static void rx_payload_bit(ysf_rx_t *r, uint32_t b, int8_t s)
{
	const ysf_fich_t *f = &r->call.fich;
	uint32_t sub = b / YSF_SUB_BITS, off = b % YSF_SUB_BITS;

	if (f->fi == YSF_FI_HEADER || f->fi == YSF_FI_TERM) {
		uint32_t at = off < 72u ? 72u * sub + off : YSF_DCH_BITS_LONG + 72u * sub + (off - 72u);
		ysf_soft_put(r->soft, at, s);
	} else if (f->dt == YSF_DT_VD2) {
		if (off < 40u) {
			ysf_soft_put(r->soft, 40u * sub + off, s);
		} else {
			ysf_soft_put(r->soft, YSF_DCH_BITS_SHORT + (off - 40u), s);
			if (off == YSF_SUB_BITS - 1u) {
				uint8_t bits[YSF_VOICE_BITS];
				uint8_t q = ysf_vd2_voice_decode(r->soft + YSF_DCH_BITS_SHORT / 2u, bits);
				r->voices++;
				if (r->cb.voice != NULL) {
					r->cb.voice(r->cb.ctx, bits, q);
				}
			}
		}
	} else if (f->dt == YSF_DT_VD1) {
		if (off < 72u) {
			ysf_soft_put(r->soft, 72u * sub + off, s);
		} else {
			// The 72-bit frame as hard bits after the DCH unit's soft bits (the unit needs 180 bytes of 360)
			uint8_t *v = &r->soft[YSF_DCH_BITS_LONG / 2u];
			ysf_bit_put(v, off - 72u, s > 0);
			if (off == YSF_SUB_BITS - 1u) {
				r->voice72s++;
				if (r->cb.voice72 != NULL) {
					r->cb.voice72(r->cb.ctx, v);
				}
			}
		}
	}
#if CONFIG_YSF_VW
	else if (f->dt == YSF_DT_VOICE_FR && f->fi == YSF_FI_COMM) {
		ysf_vw_rx_bit(r, b, s);
	}
#endif
	// Data FR frames (and Voice FR without CONFIG_YSF_VW): recognized by their FICH, their payload not decoded
}

static void rx_fich_done(ysf_rx_t *r)
{
	uint32_t v;
	ysf_fich_result_t res = ysf_fich_decode(r->soft, &v);
	r->pos = 0;
	if (res.crc_ok) {
		r->fich_good++;
		bool first = !r->in_call;
		if (first) {
			memset(&r->call, 0, sizeof(r->call));
			memset(&r->call.csd, ' ', sizeof(r->call.csd));
		}
		ysf_fich_unpack(v, &r->call.fich);
		if (first && r->call.fich.fi != YSF_FI_HEADER) {
			r->late_syncs++;	// joined a transmission after its header
		}
		r->in_call = true;
		r->fich_ok = true;
		r->frames++;
		r->state = YSF_RX_PAYLOAD;
		if (r->cb.frame != NULL) {
			r->cb.frame(r->cb.ctx, &r->call, first);
		}
		return;
	}
	r->fich_bad++;
	if (!r->in_call) {
		rx_search(r);	// a false sync, or a frame too damaged to start a call on
		return;
	}
	// In a call with the sync where it was due: carry on as the next frame of the rolling pattern, so its voice
	// still plays. With the sync missed as well, skip the payload
	r->fich_ok = r->misses == 0;
	if (r->fich_ok) {
		ysf_fich_t *f = &r->call.fich;
#if CONFIG_YSF_VW
		if (f->dt == YSF_DT_VOICE_FR) {
			ysf_vw_rx_next(f);
		} else
#endif
		if (f->fi == YSF_FI_COMM) {
			f->fn = (uint8_t)(f->fn >= f->ft ? 0u : f->fn + 1u);
		} else {
			// After a header, a damaged frame is most likely the first communication frame
			f->fi = YSF_FI_COMM;
			f->fn = 0;
		}
		r->frames++;
	}
	r->state = YSF_RX_PAYLOAD;
}

static void rx_bits(ysf_rx_t *r, uint8_t hard, int8_t s1, int8_t s0)
{
	r->hard = ((r->hard << 2) | hard) & SYNC_MASK;
	switch (r->state) {
	case YSF_RX_SEARCH: {
		int e = __builtin_popcountll(r->hard ^ YSF_SYNC);
		int ei = __builtin_popcountll(r->hard ^ SYNC_INV);
		if (e <= YSF_SYNC_SEARCH_ERRORS || ei <= YSF_SYNC_SEARCH_ERRORS) {
			// invert is applied to the levels before this function, so a sync found inverted flips it
			if (ei < e) {
				r->invert = !r->invert;
			}
			r->syncs++;
			r->state = YSF_RX_FICH;
			r->pos = 0;
		}
		break;
	}
	case YSF_RX_SYNC:
		if (++r->pos < YSF_SYNC_DIBITS) {
			break;
		}
		r->pos = 0;
		{
			int e = __builtin_popcountll(r->hard ^ YSF_SYNC);
			if (e <= YSF_SYNC_TRACK_ERRORS) {
				r->misses = 0;
				r->sync_bits += 40u;
				r->sync_errors += (uint32_t)e;
			} else if (++r->misses >= 4u) {
				rx_end(r, YSF_END_LOST);	// N2 = 4 (4.7 (2))
				break;
			}
		}
		r->state = YSF_RX_FICH;
		break;
	case YSF_RX_FICH:
		ysf_soft_put(r->soft, 2u * r->pos, s1);
		ysf_soft_put(r->soft, 2u * r->pos + 1u, s0);
		if (++r->pos >= YSF_FICH_DIBITS) {
			rx_fich_done(r);
		}
		break;
	case YSF_RX_PAYLOAD:
		if (r->fich_ok) {
			rx_payload_bit(r, 2u * r->pos, s1);
			rx_payload_bit(r, 2u * r->pos + 1u, s0);
		}
		if (++r->pos >= YSF_PAYLOAD_DIBITS) {
			r->pos = 0;
			if (r->fich_ok) {
				rx_payload_done(r);
			}
			if (r->fich_ok && r->call.fich.fi == YSF_FI_TERM) {
				rx_end(r, YSF_END_TERMINATOR);
			} else {
				r->state = YSF_RX_SYNC;
			}
		}
		break;
	default:
		rx_search(r);
		break;
	}
}

void ysf_rx_symbol(void *rx, int16_t soft, int16_t unit)
{
	ysf_rx_t *r = rx;
	int32_t y = r->invert ? -(int32_t)soft : soft;
	int32_t u = unit > 0 ? unit : 1;
	int32_t ay = y < 0 ? -y : y;
	uint8_t hard = (uint8_t)((y < 0 ? 2u : 0u) | (ay > 2 * u ? 1u : 0u));
	// The first bit says the sign, the second whether the level is an outer one; scaled so a clean +-1 level gives
	// 2.5 of the 7 a soft value holds
	int8_t s1 = ysf_soft_clamp(-y * 5 / (2 * u));
	int8_t s0 = ysf_soft_clamp((ay - 2 * u) * 5 / (2 * u));
	rx_bits(r, hard, s1, s0);
}

void ysf_rx_dibit(ysf_rx_t *r, uint8_t dibit)
{
	if (r->invert) {
		dibit ^= 2u;
	}
	rx_bits(r, dibit & 3u, (dibit & 2u) ? 4 : -4, (dibit & 1u) ? 4 : -4);
}
