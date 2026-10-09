// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The audio probe (CONFIG_AUDIO_PROBE; docs/spec/debuglink.md 3.14, docs/spec/hardware-dm1701.md 21.3): a bench tool to find
// out which audio-chip settings put FM receive audio on the PCM stream and PCM into the FM modulator. It reads and
// writes the audio chip's registers (board.audio_tap->modem_reg), runs the board's PCM stream with its own callback
// (optionally sending a test tone), and reports the levels of both I2S channels. It also reads and writes the chip's
// RAM (board.audio_tap->modem_ram: the HR_C6000's FM data buffers) and watches its interrupt flags and one RAM word
// for a while, to find the chip's own FM sample path (hardware-dm1701.md 21.3, candidate C). Diagnostics only: it borrows the
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
	PROBE_OP_MOD		= 10,	// ms u16, page u8, reg u8, tone Hz u16, amplitude u8 (register steps)[, flags u8]
					// -> writes u32, elapsed ms u32, base u8
	PROBE_OP_RAMFEED	= 11,	// hold ms u16, gap ms u16, tone Hz u16, amplitude u16, n u8, n x (page, reg, xor)
					// -> half switches u16, writes u16, max write lag ms u16, flips done u8
	PROBE_OP_PIN_TONE	= 12,	// hz u16 (0 = off): square wave on the board's spare output (DM-1701: PC8)
	PROBE_OP_PIN_SCAN	= 13,	// ms u16 -> n u8, n x changed-pin mask u16 (GPIOA, GPIOB, ...)
	PROBE_OP_PIN_PWM	= 14,	// top u16, tone Hz u16 (0 = off), amplitude per mille u16 -> carrier Hz u32
};

#define PROBE_MOD_SHAPED	0x01u	// MOD flags: second order noise shaping of the register steps
#define PROBE_FEED_MAX_FLIPS	16u

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

// Plays a tone into a register for ms: the register is read as a signed base, then written base + amplitude x sine
// back to back, as fast as the config port goes (about 37,000 writes a second), and restored. The phase follows the
// microsecond clock. Found on the bench (hardware-dm1701.md 21.3.1): the HR_C6000's 0x04 (two-point
// modulation's Mod2 offset) moves the FM carrier about 75 Hz a step, so this is MCU audio on air. Blocks the debug
// link task meanwhile and takes the CPU from lower priority tasks
static size_t probe_mod(const uint8_t *req, uint16_t len, uint8_t *out)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	uint16_t ms = get16(req), hz = get16(req + 4);
	uint8_t page = req[2], reg = req[3], base = 0;
	float amp = (float)req[6];
	bool shaped = len > 7u && (req[7] & PROBE_MOD_SHAPED) != 0;
	if (tap->modem_reg == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (ms == 0 || ms > PROBE_WATCH_MAX_MS || hz > 8000) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	bool ok = tap->modem_reg(page, reg, &base, false);
	float e1 = 0.0f, e2 = 0.0f;				// the last two quantization errors (shaping)
	uint32_t writes = 0, t0 = hal_millis(), now = t0, u0 = hal_micros();
	while (ok && now - t0 < ms) {
		// the phase from the microsecond clock: each write carries the tone's value at the moment it is made
		uint32_t us = hal_micros() - u0;
		float ph = 6.2831853f * (float)(((uint64_t)us * hz) % 1000000u) / 1e6f;
		float x = (float)(int8_t)base + sinf(ph) * amp;
		if (shaped) {
			// error feedback with (1 - z^-1)^2: the quantization noise moves up, above the voice band, at
			// the write rate (about 37 kHz, 4-5 writes a sample)
			x = x - 2.0f * e1 + e2;
		}
		int32_t v = (int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
		v = v > 127 ? 127 : v < -128 ? -128 : v;
		e2 = e1;
		e1 = (float)v - x;
		uint8_t b = (uint8_t)v;
		ok = tap->modem_reg(page, reg, &b, true);
		writes++;
		now = hal_millis();
	}
	ok = tap->modem_reg(page, reg, &base, true) && ok;
	if (!ok) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	out[0] = DL_ST_OK;
	put32(out + 1, writes);
	put32(out + 5, hal_millis() - t0);
	out[9] = base;
	return 10;
}

// The HR_C6000 TX RAM (manual 5.2: page 2 writes it, page 6 reads it) FM halves, 0x030 and 0x230, 512 bytes each:
// in FM transmit the chip writes the codec's samples there, one half per 32 ms (hardware-dm1701.md 21.3.1)
#define FEED_HALF0		0x030u
#define FEED_HALF_LEN		512u
#define FEED_PAGE_WRITE		2u
#define FEED_PAGE_READ		6u

// Keeps a big endian tone in the TX RAM FM halves while the codec fills them: each time the codec moves to the
// other half, the half it finished is overwritten at once (about 4 ms over the config port). Meanwhile each flip
// (page, register, xor mask) is applied for hold ms, then undone for gap ms, in order, starting with gap ms of
// nothing. If a setting makes the modulator read the RAM after the codec, the tone goes on air during its flip.
// Blocks the debug link task meanwhile and takes the CPU from lower priority tasks
static size_t probe_ramfeed(const uint8_t *req, uint8_t *out)
{
	const hal_audio_tap_t *tap = board.audio_tap;
	uint16_t hold = get16(req), gap = get16(req + 2), hz = get16(req + 4);
	float amp = (float)(get16(req + 6) > 32767 ? 32767 : get16(req + 6));
	uint8_t n = req[8];
	const uint8_t *flips = req + 9;
	if (tap->modem_ram == NULL || tap->modem_reg == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (hold > PROBE_WATCH_MAX_MS || gap > PROBE_WATCH_MAX_MS || hz > 3800 ||
	    (uint32_t)n * (hold + gap) + gap > 9000u) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	static uint8_t buf[FEED_HALF_LEN];
	float step = 2.0f * 3.14159265f * (float)hz / (float)AUDIO_RATE_HZ, ph = 0.0f;
	uint8_t prev[2][4] = { { 0 } }, cur[2][4];
	int active = -1;
	uint32_t switches = 0, writes = 0, max_lag = 0;
	bool ok = true;
	uint8_t done = 0, saved = 0;
	bool flipped = false;
	uint32_t t0 = hal_millis(), t_next = t0 + gap;
	for (int h = 0; h < 2; h++) {
		ok = ok && tap->modem_ram(FEED_PAGE_READ, (uint16_t)(FEED_HALF0 + h * FEED_HALF_LEN), prev[h], 4, false);
	}
	while (ok && (done < n || flipped || hal_millis() < t_next)) {
		uint32_t now = hal_millis();
		if ((int32_t)(now - t_next) >= 0) {
			const uint8_t *f = flips + 3u * done;
			if (!flipped && done < n) {
				ok = tap->modem_reg(f[0], f[1], &saved, false);
				uint8_t v = (uint8_t)(saved ^ f[2]);
				ok = ok && tap->modem_reg(f[0], f[1], &v, true);
				flipped = true;
				t_next = now + hold;
			} else if (flipped) {
				ok = tap->modem_reg(f[0], f[1], &saved, true);
				flipped = false;
				done++;
				t_next = now + gap;
			} else {
				break;
			}
		}
		for (int h = 0; ok && h < 2; h++) {
			ok = tap->modem_ram(FEED_PAGE_READ, (uint16_t)(FEED_HALF0 + h * FEED_HALF_LEN), cur[h], 4, false);
		}
		bool mv0 = memcmp(cur[0], prev[0], 4) != 0, mv1 = memcmp(cur[1], prev[1], 4) != 0;
		memcpy(prev, cur, sizeof prev);
		if (mv0 == mv1) {
			continue;
		}
		int now_h = mv0 ? 0 : 1;
		if (active >= 0 && now_h != active) {
			switches++;
			uint32_t ts = hal_millis();
			for (uint32_t i = 0; i < FEED_HALF_LEN / 2u; i++) {
				uint16_t v = (uint16_t)(int16_t)(sinf(ph) * amp);
				ph += step;
				if (ph > 6.2831853f) {
					ph -= 6.2831853f;
				}
				buf[2 * i] = (uint8_t)(v >> 8);
				buf[2 * i + 1] = (uint8_t)v;
			}
			ok = tap->modem_ram(FEED_PAGE_WRITE, (uint16_t)(FEED_HALF0 + active * FEED_HALF_LEN), buf,
					    FEED_HALF_LEN, true);
			writes++;
			// our own write changes the half's first word: read it back so it isn't taken for the codec's
			ok = ok && tap->modem_ram(FEED_PAGE_READ, (uint16_t)(FEED_HALF0 + active * FEED_HALF_LEN),
						  prev[active], 4, false);
			uint32_t lag = hal_millis() - ts;
			max_lag = lag > max_lag ? lag : max_lag;
		}
		active = now_h;
	}
	if (flipped) {
		const uint8_t *f = flips + 3u * done;
		(void)tap->modem_reg(f[0], f[1], &saved, true);
	}
	if (!ok) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	out[0] = DL_ST_OK;
	put16(out + 1, sat16(switches));
	put16(out + 3, sat16(writes));
	put16(out + 5, sat16(max_lag));
	out[7] = done;
	return 8;
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
	case PROBE_OP_MOD:
		if (len != 8 && len != 9) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		return probe_mod(req + 1, (uint16_t)(len - 1u), out);
	case PROBE_OP_RAMFEED:
		if (len < 10 || len != 10u + 3u * req[9] || req[9] > PROBE_FEED_MAX_FLIPS) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		return probe_ramfeed(req + 1, out);
	case PROBE_OP_PIN_PWM: {
		const hal_audio_tap_t *t = board.audio_tap;
		if (len != 7 || t == NULL || t->pin_pwm == NULL) {
			out[0] = len != 7 ? DL_ST_BAD_ARG : DL_ST_UNAVAILABLE;
			return 1;
		}
		uint32_t fs = t->pin_pwm((uint16_t)(req[1] | (req[2] << 8)), (uint16_t)(req[3] | (req[4] << 8)),
					 (uint16_t)(req[5] | (req[6] << 8)));
		out[0] = DL_ST_OK;
		memcpy(out + 1, &fs, 4);
		return 5;
	}
	case PROBE_OP_PIN_TONE:
	case PROBE_OP_PIN_SCAN: {
		bool tone = req[0] == PROBE_OP_PIN_TONE;
		if (len != 3 || tap == NULL || (tone ? tap->pin_tone == NULL : tap->pin_scan == NULL)) {
			out[0] = len != 3 ? DL_ST_BAD_ARG : DL_ST_UNAVAILABLE;
			return 1;
		}
		uint16_t v = (uint16_t)(req[1] | (req[2] << 8));
		out[0] = DL_ST_OK;
		if (tone) {
			tap->pin_tone(v);
			return 1;
		}
		uint16_t changed[8];
		uint8_t n = tap->pin_scan(v > 2000u ? 2000u : v, changed, 8);
		out[1] = n;
		for (uint8_t i = 0; i < n; i++) {
			out[2 + 2 * i] = (uint8_t)changed[i];
			out[3 + 2 * i] = (uint8_t)(changed[i] >> 8);
		}
		return 2u + 2u * n;
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
