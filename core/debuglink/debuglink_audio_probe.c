// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The audio probe (CONFIG_AUDIO_PROBE; docs/spec/debuglink.md 3.14, docs/spec/usb-audio.md 2.3): a bench tool to find
// out which audio-chip settings put FM receive audio on the PCM stream and PCM into the FM modulator. It reads and
// writes the audio chip's registers (board.audio_tap->modem_reg), runs the board's PCM stream with its own callback
// (optionally sending a test tone), and reports the levels of both I2S channels. It also reads and writes the chip's
// RAM (board.audio_tap->modem_ram: the HR_C6000's FM data buffers) and watches its interrupt flags and one RAM word
// for a while, to find the chip's own FM sample path (usb-audio.md 2.3, candidate C). Diagnostics only: it borrows the
// stream, so it refuses while DMR voice owns it, and a beep in the meantime may stop it (status says so).
#include <string.h>
#include <math.h>
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#include "hal_time.h"
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
	PROBE_OP_RAM_READ	= 6,	// page u8, addr u16, len u16 -> the bytes
	PROBE_OP_RAM_WRITE	= 7,	// page u8, addr u16, the bytes
	PROBE_OP_WATCH		= 8,	// ms u16, flag page u8, flag reg u8, clear reg u8, mask u8, RAM page u8, RAM addr u16
					// -> elapsed ms u32, polls u16, 8 x (count u16, first ms u16), RAM changes u16
	PROBE_OP_MUSIC		= 9,	// ms u16, tone Hz u16, amplitude u16, flags u8 (bit 0 little endian)
					// -> writes u16, polls u16, elapsed ms u32
};

#define PROBE_MUSIC_SAMPLES	32u	// per FIFO write (manual appendix A1: 32 samples when 0x88 bit 0 reads 0)
#define PROBE_MUSIC_LE		0x01u

#define PROBE_WATCH_MAX_MS	2000u
#define PROBE_NO_RAM		0xFFFFu

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

static uint16_t sat16(uint32_t v)
{
	return (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
}

// Polls an interrupt flag register about once a millisecond for ms: counts each flag in mask that it finds set and
// clears it (writing the flags to the clear register), and counts the changes of a two-byte RAM word. With the chip's
// interrupt lines unused in FM (the DMR engine's handlers are off), this gives the rate of the FM data interrupts and
// shows whether the chip refills its RAM buffers. Blocks the debug link task meanwhile
static size_t probe_watch(const uint8_t *req, uint8_t *out, size_t cap)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	uint16_t ms = get16(req);
	uint8_t fpage = req[2], freg = req[3], creg = req[4], mask = req[5], rpage = req[6];
	uint16_t raddr = get16(req + 7);
	if (ms == 0 || ms > PROBE_WATCH_MAX_MS || cap < 41 || (raddr != PROBE_NO_RAM && tap->modem_ram == NULL)) {
		out[0] = ms == 0 || ms > PROBE_WATCH_MAX_MS ? DL_ST_BAD_ARG : DL_ST_UNAVAILABLE;
		return 1;
	}
	uint32_t count[8] = { 0 }, first[8] = { 0 }, polls = 0, changes = 0;
	uint8_t word[2] = { 0, 0 }, prev[2] = { 0, 0 };
	bool have_prev = false, ok = true;
	uint32_t t0 = hal_millis(), t = 0;
	do {
		uint8_t f = 0;
		ok = tap->modem_reg(fpage, freg, &f, false);
		f &= mask;
		if (ok && f != 0) {
			ok = tap->modem_reg(fpage, creg, &f, true);
			for (int b = 0; b < 8; b++) {
				if ((f >> b) & 1u) {
					if (count[b]++ == 0) {
						first[b] = t;
					}
				}
			}
		}
		if (ok && raddr != PROBE_NO_RAM) {
			ok = tap->modem_ram(rpage, raddr, word, 2, false);
			if (have_prev && (word[0] != prev[0] || word[1] != prev[1])) {
				changes++;
			}
			prev[0] = word[0];
			prev[1] = word[1];
			have_prev = true;
		}
		polls++;
		hal_delay_ms(1);
		t = hal_millis() - t0;
	} while (ok && t < ms);
	if (!ok) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	out[0] = DL_ST_OK;
	put32(out + 1, t);
	put16(out + 5, sat16(polls));
	for (int b = 0; b < 8; b++) {
		put16(out + 7 + 4 * b, sat16(count[b]));
		put16(out + 9 + 4 * b, sat16(first[b]));
	}
	put16(out + 39, sat16(changes));
	return 41;
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

// Feeds the HR_C6000's prompt tone FIFO (manual appendix A1) with a tone for ms: every millisecond or so, when
// 0x88 bit 0 reads 0, 32 samples go to page 3 address 0 over the config port. The caller opens the path first
// (0x06 bit 1). Blocks the debug link task meanwhile
static size_t probe_music(const uint8_t *req, uint8_t *out)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	uint16_t ms = get16(req), hz = get16(req + 2);
	float amp = (float)(get16(req + 4) > 32767 ? 32767 : get16(req + 4));
	bool le = (req[6] & PROBE_MUSIC_LE) != 0;
	if (tap->modem_burst == NULL || tap->modem_reg == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (ms == 0 || ms > PROBE_WATCH_MAX_MS || hz > 3800) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	float step = 2.0f * 3.14159265f * (float)hz / (float)AUDIO_RATE_HZ, ph = 0.0f;
	uint8_t buf[2 * PROBE_MUSIC_SAMPLES];
	uint32_t writes = 0, polls = 0, t0 = hal_millis(), t = 0;
	bool ok = true;
	do {
		uint8_t st = 1;
		ok = tap->modem_reg(4, 0x88, &st, false);
		polls++;
		if (ok && (st & 1u) == 0) {
			for (uint32_t i = 0; i < PROBE_MUSIC_SAMPLES; i++) {
				uint16_t v = (uint16_t)(int16_t)(sinf(ph) * amp);
				ph += step;
				if (ph > 6.2831853f) {
					ph -= 6.2831853f;
				}
				buf[2 * i] = le ? (uint8_t)v : (uint8_t)(v >> 8);
				buf[2 * i + 1] = le ? (uint8_t)(v >> 8) : (uint8_t)v;
			}
			ok = tap->modem_burst(3, 0x00, buf, sizeof buf);
			writes++;
			t = hal_millis() - t0;
			continue;	// the FIFO holds 64: see whether it takes another 32 at once
		}
		hal_delay_ms(1);
		t = hal_millis() - t0;
	} while (ok && t < ms);
	if (!ok) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	out[0] = DL_ST_OK;
	put16(out + 1, sat16(writes));
	put16(out + 3, sat16(polls));
	put32(out + 5, hal_millis() - t0);
	return 9;
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
	case PROBE_OP_RAM_READ: {
		uint16_t n = len == 6 ? get16(req + 4) : 0;
		if (tap->modem_ram == NULL || n == 0 || n > cap - 1u || len != 6) {
			out[0] = tap->modem_ram == NULL ? DL_ST_UNAVAILABLE : DL_ST_BAD_ARG;
			return 1;
		}
		bool ok = tap->modem_ram(req[1], get16(req + 2), out + 1, n, false);
		out[0] = ok ? DL_ST_OK : DL_ST_UNAVAILABLE;
		return ok ? 1u + n : 1u;
	}
	case PROBE_OP_RAM_WRITE: {
		if (tap->modem_ram == NULL || len < 5) {
			out[0] = tap->modem_ram == NULL ? DL_ST_UNAVAILABLE : DL_ST_BAD_ARG;
			return 1;
		}
		uint8_t buf[DL_REQ_MAX];
		uint16_t n = (uint16_t)(len - 4u);
		memcpy(buf, req + 4, n);
		out[0] = tap->modem_ram(req[1], get16(req + 2), buf, n, true) ? DL_ST_OK : DL_ST_UNAVAILABLE;
		return 1;
	}
	case PROBE_OP_WATCH:
		if (len != 10 || tap->modem_reg == NULL) {
			out[0] = tap->modem_reg == NULL ? DL_ST_UNAVAILABLE : DL_ST_BAD_ARG;
			return 1;
		}
		return probe_watch(req + 1, out, cap);
	case PROBE_OP_MUSIC:
		if (len != 8) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		return probe_music(req + 1, out);
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
