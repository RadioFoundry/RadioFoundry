// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR off the air: a real dPMR446 transmission through the chain a board without a symbol pipe runs - the shared
// soft modem (core/air/air_softmodem.c) on a discriminator tap, the framer, the dPMR call layer - and its voice frames
// through the AMBE+2 frame's own FEC. The one check of the framing against a radio that is not ours.
//
// The recording is not in the tree: it is someone's transmission, and the tree keeps no audio. The test reads it from
// the path given (argument or $DPMR_CAPTURE) and says it was skipped and passes without one. The recording it was
// written against is the Signal ID Wiki's dPMR example (DPMR.zip: an SDR# I/Q file, 48 kHz, 16 bits, I left, Q
// right, about 30 s). What it holds, read off it once by hand (ETSI TS 102 490): nine headers (eight extended wake-up
// headers counting down, then a normal one) on a common ID to the same common ID, channel code 0x1A4 (group A,
// 446.109375 MHz), then 90 superframes of voice (1440 AMBE+2 frames) and an end frame. The few coded blocks the
// vectors keep from it are the air* lines of tests/vectors/dpmr.txt.
//
// The test does what a tap does: an FM discriminator, a low-pass to the tap's band, every sixth sample (8 kHz), at a
// scale of 2000 counts per kHz. The recording's spectrum is inverted (its I and Q are swapped against the
// discriminator's sense); a board's tap has one sense, set by its calibration, so the test turns the samples over.
//
//   dpmrCaptureTest [file.wav]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dpmr/dpmr.h"
#include "air/air_softmodem.h"
#include "dmr/ambe/ambeFec.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IQ_RATE		48000u
#define TAP_RATE	8000u
#define COUNTS_PER_KHZ	2000.0

static struct {
	dpmr_call_t call;
	uint32_t starts, ends, voice;
	uint32_t fec[8];		// AMBE+2 frames by the corrections their FEC made (7: C0 past correcting)
} got;

static void on_start(const dpmr_call_t *c, void *ctx) { (void)ctx; got.starts++; got.call = *c; }
static void on_update(const dpmr_call_t *c, void *ctx) { (void)ctx; got.call = *c; }
static void on_end(const dpmr_call_t *c, void *ctx) { (void)ctx; got.ends++; got.call = *c; }
static void on_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	uint8_t bits[AMBE_PARAM_BITS];
	ambe_fec_result_t r;

	(void)ctx;
	ambeFecDecode(ambe, bits, &r);
	got.fec[r.c0_failed ? 7 : r.errors > 6 ? 6 : r.errors]++;
	got.voice++;
}
static const dpmr_call_sink_t sink = { on_start, on_update, on_voice, on_end };

static float *load(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	uint8_t hdr[44];
	if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
		fclose(f);
		return NULL;
	}
	// Walk the chunks to "data": SDR# writes a plain header, but a tool that touched the file may have added more
	fseek(f, 12, SEEK_SET);
	uint8_t ch[8];
	uint32_t len = 0, rate = 0, chans = 0, bits = 0;
	while (fread(ch, 1, 8, f) == 8) {
		len = (uint32_t)ch[4] | (uint32_t)ch[5] << 8 | (uint32_t)ch[6] << 16 | (uint32_t)ch[7] << 24;
		if (memcmp(ch, "fmt ", 4) == 0) {
			uint8_t fmt[16];
			if (len < 16 || fread(fmt, 1, 16, f) != 16) {
				break;
			}
			chans = fmt[2] | fmt[3] << 8;
			rate = (uint32_t)fmt[4] | (uint32_t)fmt[5] << 8 | (uint32_t)fmt[6] << 16 | (uint32_t)fmt[7] << 24;
			bits = fmt[14] | fmt[15] << 8;
			fseek(f, (long)(len - 16u + (len & 1u)), SEEK_CUR);
		} else if (memcmp(ch, "data", 4) == 0) {
			break;
		} else {
			fseek(f, (long)(len + (len & 1u)), SEEK_CUR);
		}
	}
	if (rate != IQ_RATE || chans != 2 || bits != 16 || len == 0) {
		printf("dpmrCaptureTest: %s is not 48 kHz 16-bit I/Q (%u Hz, %u channels, %u bits)\n", path, rate, chans, bits);
		fclose(f);
		return NULL;
	}
	size_t frames = len / 4u;
	int16_t *iq = malloc(frames * 4u);
	float *fm = malloc(frames * sizeof(float));
	frames = fread(iq, 4, frames, f);
	fclose(f);
	double pi_prev = 0, pq_prev = 0;
	for (size_t i = 0; i < frames; i++) {
		double ii = iq[2 * i], qq = iq[2 * i + 1];
		// x[n] conj(x[n-1]): its angle is the phase step
		double re = ii * pi_prev + qq * pq_prev, im = qq * pi_prev - ii * pq_prev;
		fm[i] = (float)(atan2(im, re) * IQ_RATE / (2.0 * M_PI));
		pi_prev = ii;
		pq_prev = qq;
	}
	free(iq);
	*n = frames;
	return fm;
}

// The discriminator's Hz at the tap's rate and scale, turned over: a 63-tap windowed-sinc low-pass at 3 kHz, then
// every sixth sample
static int16_t *to_tap(const float *fm, size_t n, size_t *out_n)
{
	enum { TAPS = 63 };
	uint32_t m = IQ_RATE / TAP_RATE;
	double h[TAPS], sum = 0;
	int16_t *out = malloc((n / m + 1) * sizeof(int16_t));
	size_t j = 0;

	for (int k = 0; k < TAPS; k++) {
		double t = k - (TAPS - 1) / 2.0, fc = 3000.0 / IQ_RATE;
		double s = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);

		h[k] = s * (0.54 - 0.46 * cos(2 * M_PI * k / (TAPS - 1)));
		sum += h[k];
	}
	for (size_t i = TAPS; i < n; i += m) {
		double acc = 0;

		for (int k = 0; k < TAPS; k++)
			acc += h[k] * fm[i - (size_t)k];
		acc = -acc / sum * COUNTS_PER_KHZ / 1000.0;
		out[j++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : lrint(acc));
	}
	*out_n = j;
	return out;
}

static void out_chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : getenv("DPMR_CAPTURE");
	static uint8_t fbuf[DPMR_FRAME_BYTES];
	static dpmr_call_rx_t c;
	air_softmodem_t m;
	air_framer_t fr;
	size_t n = 0, tn = 0;
	float *fm = path && path[0] ? load(path, &n) : NULL;
	int16_t *tap;
	uint32_t clean;

	if (fm == NULL) {
		printf("dpmrCaptureTest: skipped (no recording: give its path or set DPMR_CAPTURE)\n");
		return 0;
	}
	tap = to_tap(fm, n, &tn);
	dpmr_call_rx_init(&c, &sink, NULL);
	air_framer_init(&fr, &dpmr_air_mode, &c.rx, fbuf, NULL, DPMR_FRAME_SYMS);
	CHECK(air_softmodem_rx_open(&m, &dpmr_air_mode.profile, TAP_RATE, (uint16_t)COUNTS_PER_KHZ, out_chunk, &fr), "open");
	for (size_t i = 0; i < tn; i += 256)
		air_softmodem_rx(&m, tap + i, (uint16_t)(tn - i < 256 ? tn - i : 256), (uint32_t)(i * 125u));
	clean = got.fec[0];
	printf("dpmr capture through the soft modem at %u Hz:\n", TAP_RATE);
	printf("  framer: syncs %u, locks %u, misses %u, rejects %u; units: headers %u (%u failed), payload frames %u (CCH %u "
	       "failed), ends %u (%u failed); Hamming words fixed %u, past fixing %u\n", fr.stats.syncs, fr.stats.locks,
	       fr.stats.misses, fr.stats.rejects, c.stats.headers, c.stats.hi_fail, c.stats.frames, c.stats.cch_fail,
	       c.stats.ends, c.stats.end_fail, c.stats.ham_fixed, c.stats.ham_bad);
	printf("  calls %u, ended %u (by an end frame %d); called %06X own %06X (have %02X), channel code %03X\n", got.starts,
	       got.ends, got.call.ended, got.call.called, got.call.own, got.call.have, got.call.cc);
	printf("  voice frames %u: AMBE+2 FEC corrections 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u, C0 lost %u\n", got.voice,
	       got.fec[0], got.fec[1], got.fec[2], got.fec[3], got.fec[4], got.fec[5], got.fec[6], got.fec[7]);
	// The soft modem first finds its timing in the sixth of the nine headers (about 400 ms into the transmission; its
	// symbols before that come out free running, FS1 with three or four symbols wrong): the last four headers are
	// read, and the call is whole from its first payload frame
	CHECK(c.stats.headers >= 4 && c.stats.hi_fail == 0, "headers %u, %u failed", c.stats.headers, c.stats.hi_fail);
	CHECK(got.starts == 1 && got.ends == 1 && got.call.ended, "starts %u ends %u ended %d", got.starts, got.ends,
	      got.call.ended);
	CHECK((got.call.have & DPMR_HAVE_HEADER) && got.call.called == 0x57FFFFu && got.call.own == 0x57FFFFu,
	      "IDs %06X %06X", got.call.called, got.call.own);
	CHECK((got.call.have & DPMR_HAVE_CC) && got.call.cc == 0x1A4u, "channel code %03X", got.call.cc);
	CHECK(c.stats.frames >= 355u && c.stats.cch_fail <= 5u, "%u payload frames, %u CCH failed", c.stats.frames,
	      c.stats.cch_fail);
	CHECK(got.voice >= 1430u && clean * 100u >= got.voice * 85u && got.fec[7] == 0, "%u voice frames, %u clean, %u lost",
	      got.voice, clean, got.fec[7]);
	free(tap);
	free(fm);
	printf("dpmrCaptureTest: %d failed\n", failures);
	return failures ? 1 : 0;
}
