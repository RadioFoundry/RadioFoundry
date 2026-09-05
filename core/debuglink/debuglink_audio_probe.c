// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The audio probe (CONFIG_AUDIO_PROBE; docs/spec/debuglink.md 3.14, docs/spec/usb-audio.md 2.3): a bench tool to find
// out which audio-chip settings put FM receive audio on the PCM stream and PCM into the FM modulator. It reads and
// writes the audio chip's registers (board.audio_tap->modem_reg), runs the board's PCM stream with its own callback
// (optionally sending a test tone), and reports the levels of both I2S channels. Diagnostics only: it borrows the
// stream, so it refuses while DMR voice owns it, and a beep in the meantime may stop it (status says so).
#include <string.h>
#include <math.h>
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#if CONFIG_AUDIO_PROBE
#include "dmr/ambe/ambeVoice.h"
#endif

#if CONFIG_AUDIO_PROBE

enum {
	PROBE_OP_STATUS		= 0,	// -> running u8, blocks u32, then per channel L, R: peak u16, rms u16, peak_max u16
	PROBE_OP_REG_READ	= 1,	// page u8, reg u8 -> value u8
	PROBE_OP_REG_WRITE	= 2,	// page u8, reg u8, value u8 -> value read back u8
	PROBE_OP_START		= 3,	// tone_hz u16, amplitude u16 (0 = silence out), flags u8 (bit 0: speaker PCM route)
	PROBE_OP_STOP		= 4,
	PROBE_OP_SAMPLES	= 5,	// -> blocks u32, 160 left then 160 right samples (i16 little endian)
};

#define PROBE_FLAG_SPEAKER	0x01u

typedef struct {
	uint16_t peak, rms, peak_max;
} chan_level_t;

static volatile bool probing;		// our callback owns the stream
static bool speaker_route;
static float osc_c, osc_s;		// quadrature oscillator: rotate (c, s) by the tone's step each sample
static int16_t amplitude;
static chan_level_t level[2];
static int16_t blk_l[AUDIO_BLOCK_SAMPLES], blk_r[AUDIO_BLOCK_SAMPLES];
static float osc_cos_w, osc_sin_w;
static int16_t scr_l[AUDIO_BLOCK_SAMPLES], scr_r[AUDIO_BLOCK_SAMPLES];	// the debug link task's copies

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void measure(const int16_t *x, chan_level_t *c)
{
	int32_t peak = 0;
	uint64_t sq = 0;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		int32_t v = x[i] < 0 ? -(int32_t)x[i] : x[i];
		peak = v > peak ? v : peak;
		sq += (uint64_t)((int64_t)x[i] * x[i]);
	}
	c->peak = (uint16_t)(peak > 0xFFFF ? 0xFFFF : peak);
	c->rms = (uint16_t)sqrtf((float)(sq / AUDIO_BLOCK_SAMPLES));
	if (c->peak > c->peak_max) {
		c->peak_max = c->peak;
	}
}

// The board's audio task, once per 20 ms block
static void probe_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	const hal_audio_tap_t *tap = board.audio_tap;
	uint32_t n;
	if (tap->last_block(blk_l, blk_r, &n)) {
		measure(blk_l, &level[0]);
		measure(blk_r, &level[1]);
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		spk[i] = (int16_t)(osc_s * (float)amplitude);
		float c = osc_c * osc_cos_w - osc_s * osc_sin_w;
		float s = osc_s * osc_cos_w + osc_c * osc_sin_w;
		// Keep the amplitude at 1 (rounding would make it drift): one Newton step on c^2 + s^2
		float osc_k = 1.5f - 0.5f * (c * c + s * s);
		osc_c = c * osc_k;
		osc_s = s * osc_k;
	}
}

static uint8_t probe_start(uint16_t hz, uint16_t amp, uint8_t flags)
{
	const hal_audio_t *aud = board.audio;
	if (aud == NULL || board.audio_tap == NULL || board.audio_tap->last_block == NULL) {
		return DL_ST_UNAVAILABLE;
	}
	if (ambeVoiceStreaming() && !probing) {
		return DL_ST_BUSY;	// DMR voice owns the stream: leave DMR (an FM channel) first
	}
	if (hz > 3800) {
		return DL_ST_BAD_ARG;
	}
	float w = 2.0f * 3.14159265f * (float)hz / (float)AUDIO_RATE_HZ;
	osc_cos_w = cosf(w);
	osc_sin_w = sinf(w);
	osc_c = 1.0f;
	osc_s = 0.0f;
	amplitude = (int16_t)(amp > 32767 ? 32767 : amp);
	memset(level, 0, sizeof level);
	if (!aud->start(probe_block, NULL)) {
		probing = false;
		return DL_ST_UNAVAILABLE;	// no I2S clock from the codec (the answer to one of the questions)
	}
	probing = true;
	speaker_route = (flags & PROBE_FLAG_SPEAKER) != 0;
	if (aud->speaker_pcm != NULL) {
		aud->speaker_pcm(speaker_route);
	}
	return DL_ST_OK;
}

static void probe_stop(void)
{
	const hal_audio_t *aud = board.audio;
	if (!probing || aud == NULL) {
		probing = false;
		return;
	}
	probing = false;
	if (speaker_route && aud->speaker_pcm != NULL) {
		aud->speaker_pcm(false);
	}
	speaker_route = false;
	aud->stop();
}

size_t debuglink_cmd_audio_probe(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	if (tap == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	switch (req[0]) {
	case PROBE_OP_STATUS: {
		uint32_t blocks = 0;
		bool running = tap->last_block != NULL && tap->last_block(scr_l, scr_r, &blocks);
		out[0] = DL_ST_OK;
		out[1] = (uint8_t)((probing ? 1u : 0u) | (running ? 2u : 0u));	// bit 0 ours, bit 1 the stream runs
		put32(out + 2, blocks);
		for (int c = 0; c < 2; c++) {
			put16(out + 6 + 6 * c, level[c].peak);
			put16(out + 8 + 6 * c, level[c].rms);
			put16(out + 10 + 6 * c, level[c].peak_max);
		}
		return 18;
	}
	case PROBE_OP_REG_READ:
	case PROBE_OP_REG_WRITE: {
		bool write = req[0] == PROBE_OP_REG_WRITE;
		if (len != (write ? 4u : 3u) || tap->modem_reg == NULL) {
			out[0] = tap->modem_reg == NULL ? DL_ST_UNAVAILABLE : DL_ST_BAD_ARG;
			return 1;
		}
		uint8_t v = write ? req[3] : 0;
		bool ok = !write || tap->modem_reg(req[1], req[2], &v, true);
		ok = ok && tap->modem_reg(req[1], req[2], &v, false);
		if (write) {
			dlog("probe: reg %u:0x%02X <- 0x%02X, reads 0x%02X", req[1], req[2], req[3], v);
		}
		out[0] = ok ? DL_ST_OK : DL_ST_UNAVAILABLE;
		out[1] = v;
		return 2;
	}
	case PROBE_OP_START:
		if (len != 6) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		out[0] = probe_start(get16(req + 1), get16(req + 3), req[5]);
		return 1;
	case PROBE_OP_STOP:
		probe_stop();
		out[0] = DL_ST_OK;
		return 1;
	case PROBE_OP_SAMPLES: {
		if (cap < 5 + 4 * AUDIO_BLOCK_SAMPLES || tap->last_block == NULL) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		uint32_t blocks;
		if (!tap->last_block(scr_l, scr_r, &blocks)) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		out[0] = DL_ST_OK;
		put32(out + 1, blocks);
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
			put16(out + 5 + 2 * i, (uint16_t)scr_l[i]);
			put16(out + 5 + 2 * (AUDIO_BLOCK_SAMPLES + i), (uint16_t)scr_r[i]);
		}
		return 5 + 4 * AUDIO_BLOCK_SAMPLES;
	}
	default:
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
}

#else	// !CONFIG_AUDIO_PROBE

size_t debuglink_cmd_audio_probe(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
