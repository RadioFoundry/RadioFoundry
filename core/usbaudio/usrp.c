// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USRP voice-over-UDP framing and the radio side's link state (usrp.h, docs/spec/usb-audio.md 4.2)
#include "usrp.h"

#include <string.h>

#ifndef USRP_SAMPLES_BIG_ENDIAN
#define USRP_SAMPLES_BIG_ENDIAN	0
#endif

#define SEQ_RESTART		32	// a sequence number this far behind is a restarted peer, not a late frame

static const uint8_t magic[4] = { 'U', 'S', 'R', 'P' };

// --- G.711 mu-law ---

#define ULAW_BIAS	132
#define ULAW_CLIP	32635

uint8_t usrp_ulaw_encode(int16_t pcm)
{
	int32_t mag = pcm;
	uint8_t sign = 0;
	if (mag < 0) {
		mag = -mag;
		sign = 0x80;
	}
	if (mag > ULAW_CLIP) {
		mag = ULAW_CLIP;
	}
	mag += ULAW_BIAS;
	// Segment: the highest set bit among 14..7
	uint8_t seg = 7;
	for (int32_t bit = 0x4000; seg > 0 && (mag & bit) == 0; bit >>= 1) {
		seg--;
	}
	uint8_t mant = (uint8_t)((mag >> (seg + 3)) & 0x0F);
	return (uint8_t)~(sign | (uint8_t)(seg << 4) | mant);
}

int16_t usrp_ulaw_decode(uint8_t code)
{
	code = (uint8_t)~code;
	uint8_t seg = (code >> 4) & 7u;
	int32_t mag = ((((int32_t)code & 0x0F) << 3) + ULAW_BIAS) << seg;
	mag -= ULAW_BIAS;
	return (int16_t)((code & 0x80) ? -mag : mag);
}

// --- framing ---

static void put32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_sample(uint8_t *p, int16_t s)
{
	uint16_t u = (uint16_t)s;
#if USRP_SAMPLES_BIG_ENDIAN
	p[0] = (uint8_t)(u >> 8);
	p[1] = (uint8_t)u;
#else
	p[0] = (uint8_t)u;
	p[1] = (uint8_t)(u >> 8);
#endif
}

static int16_t get_sample(const uint8_t *p)
{
#if USRP_SAMPLES_BIG_ENDIAN
	return (int16_t)(uint16_t)(((uint16_t)p[0] << 8) | p[1]);
#else
	return (int16_t)(uint16_t)(((uint16_t)p[1] << 8) | p[0]);
#endif
}

size_t usrp_pack(uint8_t *out, size_t cap, const usrp_header_t *h, const int16_t *pcm)
{
	size_t len = USRP_HEADER_LEN;
	if (h->type == USRP_TYPE_VOICE) {
		len = USRP_VOICE_LEN;
	} else if (h->type == USRP_TYPE_VOICE_ULAW) {
		len = USRP_ULAW_LEN;
	}
	if (cap < len) {
		return 0;
	}
	memcpy(out, magic, 4);
	put32be(out + 4, h->seq);
	put32be(out + 8, h->memory);
	put32be(out + 12, h->keyup ? 1u : 0u);
	put32be(out + 16, h->talkgroup);
	put32be(out + 20, h->type);
	put32be(out + 24, h->mpxid);
	put32be(out + 28, 0);
	uint8_t *p = out + USRP_HEADER_LEN;
	for (int i = 0; len > USRP_HEADER_LEN && i < USRP_FRAME_SAMPLES; i++) {
		int16_t s = pcm != NULL ? pcm[i] : 0;
		if (h->type == USRP_TYPE_VOICE) {
			put_sample(p + 2 * i, s);
		} else {
			p[i] = usrp_ulaw_encode(s);
		}
	}
	return len;
}

usrp_parse_t usrp_parse(const uint8_t *buf, size_t len, usrp_header_t *h, int16_t *pcm, size_t *samples)
{
	*samples = 0;
	if (len < USRP_HEADER_LEN) {
		return USRP_PARSE_SHORT;
	}
	if (memcmp(buf, magic, 4) != 0) {
		return USRP_PARSE_MAGIC;
	}
	h->seq = get32be(buf + 4);
	h->memory = get32be(buf + 8);
	h->keyup = get32be(buf + 12) != 0;
	h->talkgroup = get32be(buf + 16);
	h->type = get32be(buf + 20);
	h->mpxid = get32be(buf + 24);
	const uint8_t *p = buf + USRP_HEADER_LEN;
	if (h->type == USRP_TYPE_VOICE) {
		// Exactly one frame. Some senders put a keyup-0 header with no samples at the end of a transmission
		if (len == USRP_HEADER_LEN) {
			return USRP_PARSE_OK;
		}
		if (len != USRP_VOICE_LEN) {
			return USRP_PARSE_LENGTH;
		}
		for (int i = 0; pcm != NULL && i < USRP_FRAME_SAMPLES; i++) {
			pcm[i] = get_sample(p + 2 * i);
		}
		*samples = USRP_FRAME_SAMPLES;
	} else if (h->type == USRP_TYPE_VOICE_ULAW) {
		if (len == USRP_HEADER_LEN) {
			return USRP_PARSE_OK;
		}
		if (len != USRP_ULAW_LEN) {
			return USRP_PARSE_LENGTH;
		}
		for (int i = 0; pcm != NULL && i < USRP_FRAME_SAMPLES; i++) {
			pcm[i] = usrp_ulaw_decode(p[i]);
		}
		*samples = USRP_FRAME_SAMPLES;
	}
	return USRP_PARSE_OK;
}

// --- link state ---

void usrp_link_init(usrp_link_t *l, const usrp_link_cfg_t *cfg)
{
	memset(l, 0, sizeof *l);
	l->cfg.keyup_timeout_ms = USRP_KEYUP_TIMEOUT_MS;
	l->cfg.tx_max_ms = USRP_TX_MAX_MS;
	if (cfg != NULL) {
		l->cfg = *cfg;
		if (l->cfg.keyup_timeout_ms == 0) {
			l->cfg.keyup_timeout_ms = USRP_KEYUP_TIMEOUT_MS;	// the watchdog can't be turned off
		}
	}
}

usrp_event_t usrp_link_rx(usrp_link_t *l, const uint8_t *buf, size_t len, uint32_t now_ms, int16_t *pcm, bool *voice)
{
	usrp_header_t h;
	size_t samples;
	if (voice != NULL) {
		*voice = false;
	}
	if (usrp_parse(buf, len, &h, pcm, &samples) != USRP_PARSE_OK) {
		l->stats.bad++;
		return USRP_EV_NONE;
	}
	if (l->have_seq) {
		int32_t diff = (int32_t)(h.seq - l->next_seq);
		if (diff < 0 && diff > -SEQ_RESTART) {
			l->stats.out_of_order++;	// late or repeated: its audio is stale, its keyup no news
			return USRP_EV_NONE;
		}
		if (diff > 0 && diff < SEQ_RESTART) {
			l->stats.lost += (uint32_t)diff;
		}
	}
	l->have_seq = true;
	l->next_seq = h.seq + 1;
	l->stats.frames++;
	l->last_rx_ms = now_ms;
	if (samples > 0) {
		l->stats.voice++;
		if (voice != NULL) {
			*voice = true;
		}
	}

	if (h.keyup) {
		l->last_keyup_ms = now_ms;
		if (!l->keyed && !l->locked_out) {
			l->keyed = true;
			l->key_ms = now_ms;
			l->stats.keys++;
			return USRP_EV_KEY;
		}
		return USRP_EV_NONE;
	}
	l->locked_out = false;
	if (l->keyed) {
		l->keyed = false;
		return USRP_EV_UNKEY;
	}
	return USRP_EV_NONE;
}

usrp_event_t usrp_link_tick(usrp_link_t *l, uint32_t now_ms)
{
	uint32_t quiet = now_ms - l->last_keyup_ms;
	if (l->keyed) {
		if (quiet >= l->cfg.keyup_timeout_ms) {
			l->keyed = false;
			l->stats.timeouts++;
			return USRP_EV_TIMEOUT;
		}
		if (l->cfg.tx_max_ms != 0 && (uint32_t)(now_ms - l->key_ms) >= l->cfg.tx_max_ms) {
			l->keyed = false;
			l->locked_out = true;
			l->stats.tx_limits++;
			return USRP_EV_TX_LIMIT;
		}
	} else if (l->locked_out && quiet >= l->cfg.keyup_timeout_ms) {
		l->locked_out = false;	// the peer let go (it stopped sending keyup frames)
	}
	return USRP_EV_NONE;
}

size_t usrp_link_tx(usrp_link_t *l, uint8_t *out, size_t cap, const int16_t *pcm, bool cos, bool ulaw)
{
	usrp_header_t h = {
		.seq = l->tx_seq,
		.keyup = cos,
		.type = ulaw ? USRP_TYPE_VOICE_ULAW : USRP_TYPE_VOICE,
	};
	size_t n = usrp_pack(out, cap, &h, pcm);
	if (n != 0) {
		l->tx_seq++;
	}
	return n;
}
