// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 2400 bit/s MBE front end (core/mbe/mbe2400.h, docs/spec/mbe-2400.md): the frame's bit map, FEC and interleave
// restated from the spec, the quantizer against the dequantizer, frames that play as silence, concealment, and the
// test voice set through the encoder and the decoder (spectral distance and level against the input). Built with
// -fsanitize=address,undefined by the host test run.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbe/mbe2400.h"
#include "mbe/mbe_est.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0x2400;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static const int FIELD_BITS[MBE2400_FIELDS] = { 7, 4, 6, 9, 7, 4, 4, 4, 4 };

static void random_fields(uint16_t b[MBE2400_FIELDS])
{
	for (int i = 0; i < MBE2400_FIELDS; i++)
		b[i] = (uint16_t)(rnd() & ((1u << FIELD_BITS[i]) - 1));
	b[8] &= 14;
}

static int air_bit(const uint8_t *f, int a) { return (f[a >> 3] >> (a & 7)) & 1; }
static void flip(uint8_t *f, int a) { f[a >> 3] ^= (uint8_t)(1 << (a & 7)); }

// The spec's air positions of each word's bits 23..0 (spec 3.4)
static const uint8_t AIR[3][24] = {
	{ 7, 1, 11, 21, 31, 25, 35, 45, 55, 49, 59, 69, 6, 0, 10, 20, 30, 24, 34, 44, 54, 48, 58, 68 },
	{ 5, 15, 9, 19, 29, 39, 33, 43, 53, 63, 57, 67, 4, 14, 8, 18, 28, 38, 32, 42, 52, 62, 56, 66 },
	{ 3, 13, 23, 17, 27, 37, 47, 41, 51, 61, 71, 65, 2, 12, 22, 16, 26, 36, 46, 40, 50, 60, 70, 64 },
};

// the silence frame of the spec: its fields, no corrections, and back bit for bit
static void test_silence_frame(void)
{
	static const uint16_t want[MBE2400_FIELDS] = { 124, 0, 4, 165, 41, 10, 7, 7, 10 };
	uint16_t b[MBE2400_FIELDS];
	uint8_t f[MBE2400_BYTES], e = mbe2400_frame_unpack(mbe2400_silence, b);

	CHECK(e == 0, "silence frame: errs %02x", e);
	CHECK(memcmp(b, want, sizeof want) == 0, "silence frame: %d %d %d %d %d %d %d %d %d", b[0], b[1], b[2], b[3], b[4],
	      b[5], b[6], b[7], b[8]);
	mbe2400_frame_pack(b, f);
	CHECK(memcmp(f, mbe2400_silence, MBE2400_BYTES) == 0, "silence frame does not pack back");
}

// random fields through a frame and back
static void test_frame_round_trip(void)
{
	int bad = 0;

	for (int n = 0; n < 20000; n++) {
		uint16_t b[MBE2400_FIELDS], o[MBE2400_FIELDS];
		uint8_t f[MBE2400_BYTES];

		random_fields(b);
		mbe2400_frame_pack(b, f);
		bad += mbe2400_frame_unpack(f, o) != 0 || memcmp(b, o, sizeof b) != 0;
	}
	CHECK(bad == 0, "%d of 20000 frames did not come back", bad);
}

// The words' bits on the air, restated: the third word's (no code) by single field bits; the first two by single
// errors, which the decoder corrects and counts in the right word
static void test_interleave(void)
{
	// third word bit k <- field bit, as spec 3.2 lists it from bit 23 down
	static const struct { uint8_t field, bit; } W2[24] = {
		{ 5, 1 }, { 5, 0 }, { 6, 3 }, { 6, 2 }, { 6, 1 }, { 6, 0 }, { 7, 3 }, { 7, 2 }, { 7, 1 }, { 7, 0 }, { 8, 3 },
		{ 8, 2 }, { 8, 1 }, { 1, 3 }, { 1, 2 }, { 1, 1 }, { 1, 0 }, { 2, 1 }, { 2, 0 }, { 3, 1 }, { 3, 0 }, { 4, 1 },
		{ 4, 0 }, { 0, 0 },
	};
	int used[72] = { 0 };

	for (int k = 0; k < 24; k++) {
		uint16_t b[MBE2400_FIELDS] = { 0 };
		uint8_t f0[MBE2400_BYTES], f1[MBE2400_BYTES];
		int diff = 0, at = -1;

		mbe2400_frame_pack(b, f0);
		b[W2[k].field] = (uint16_t)(1u << W2[k].bit);
		// b5's top bits and b8's are elsewhere or not sent: these are the low ones
		mbe2400_frame_pack(b, f1);
		for (int a = 0; a < 72; a++)
			if (air_bit(f0, a) != air_bit(f1, a)) {
				diff++;
				at = a;
			}
		CHECK(diff == 1 && at == AIR[2][k], "third word bit %d: %d air bits differ, at %d (want %d)", 23 - k, diff,
		      at, AIR[2][k]);
	}
	for (int w = 0; w < 3; w++)
		for (int k = 0; k < 24; k++)
			used[AIR[w][k]]++;
	for (int a = 0; a < 72; a++)
		CHECK(used[a] == 1, "air bit %d used %d times", a, used[a]);
	for (int w = 0; w < 2; w++)
		for (int k = 0; k < 24; k++) {
			uint16_t b[MBE2400_FIELDS], o[MBE2400_FIELDS];
			uint8_t f[MBE2400_BYTES], e;

			random_fields(b);
			mbe2400_frame_pack(b, f);
			flip(f, AIR[w][k]);
			e = mbe2400_frame_unpack(f, o);
			CHECK(memcmp(b, o, sizeof b) == 0 && e == MBE2400_ERRS(w == 0, w == 1),
			      "word %d bit %d: errs %02x", w, 23 - k, e);
		}
}

// Up to three errors in each Golay word corrected and counted; four detected (the first word's make the frame bad)
static void test_fec(void)
{
	int wrong = 0, missed = 0;

	for (int n = 0; n < 4000; n++) {
		for (int w = 0; w < 2; w++) {
			int nerr = 1 + n % 4, pos[4];
			uint16_t b[MBE2400_FIELDS], o[MBE2400_FIELDS];
			uint8_t f[MBE2400_BYTES], e;

			random_fields(b);
			mbe2400_frame_pack(b, f);
			for (int i = 0; i < nerr; i++) {
				int again;

				do {
					pos[i] = (int)(rnd() % 24);
					again = 0;
					for (int j = 0; j < i; j++)
						again |= pos[j] == pos[i];
				} while (again);
				flip(f, AIR[w][pos[i]]);
			}
			e = mbe2400_frame_unpack(f, o);
			if (nerr <= 3) {
				wrong += memcmp(b, o, sizeof b) != 0 || e != MBE2400_ERRS(w ? 0 : nerr, w ? nerr : 0);
			} else {
				missed += (w ? MBE2400_ERRS_C1(e) : MBE2400_ERRS_C0(e)) != 4;
				if (!w)
					missed += !MBE2400_ERRS_BAD(e);
			}
		}
	}
	CHECK(wrong == 0, "%d frames with 1-3 errors in a word not corrected or miscounted", wrong);
	CHECK(missed == 0, "%d frames with 4 errors in a word not detected", missed);
}

// The second word's whitening restated: with its data 0, the word on the air is the sequence itself (seeded from the
// first word's data), and flipping the first data bit inverts all 24 steps
static void test_whitening(void)
{
	for (int n = 0; n < 200; n++) {
		uint16_t b[MBE2400_FIELDS] = { 0 }, o[MBE2400_FIELDS];
		uint8_t f[MBE2400_BYTES];
		uint32_t x, want = 0, got = 0;
		uint16_t d0;

		b[0] = (uint16_t)(rnd() & 126);
		b[2] = (uint16_t)(rnd() & 60);
		b[3] = (uint16_t)(rnd() & 384);
		d0 = (uint16_t)((b[0] >> 1) << 6 | (b[2] >> 2) << 2 | (b[3] >> 7));
		x = (uint32_t)d0 * 16;
		for (int s = 1; s <= 24; s++) {
			x = (173 * x + 13849) % 65536;
			want |= (x >> 15) << (24 - s);
		}
		mbe2400_frame_pack(b, f);
		for (int k = 0; k < 24; k++)
			got |= (uint32_t)air_bit(f, AIR[1][k]) << (23 - k);
		CHECK(got == want, "whitening of %03x: %06x, want %06x", d0, got, want);
		b[0] ^= 64;
		mbe2400_frame_pack(b, f);
		x = 0;
		for (int k = 0; k < 24; k++)
			x |= (uint32_t)air_bit(f, AIR[1][k]) << (23 - k);
		CHECK(x == (want ^ 0xFFFFFF), "d0 flipped: whitening %06x, want %06x", x, want ^ 0xFFFFFFu);
		CHECK(mbe2400_frame_unpack(f, o) == 0, "whitened frame does not decode clean");
	}
}

// pitch codes: the fundamental falls 1.49 % a step from 409 Hz; harmonics stay under 4 kHz
static void test_pitch(void)
{
	for (int b0 = 0; b0 < MBE2400_PITCHES; b0++) {
		double f0 = mbe2400_w0(b0) / 4294967296.0 * 8000, want = 8000 * pow(2, -4.311767578125 - 0.021336 * (b0 - 1));
		int L = mbe2400_pitch_harmonics(b0);

		CHECK(fabs(f0 / want - 1) < 2e-4, "b0 %d: f0 %.3f Hz, want %.3f", b0, f0, want);
		CHECK(L >= 9 && L <= 56 && L * f0 < 4000, "b0 %d: L %d at %.1f Hz", b0, L, f0);
		if (b0)
			CHECK(L >= mbe2400_pitch_harmonics(b0 - 1), "b0 %d: L falls", b0);
	}
}

// The voicing code: four 1 kHz bands, the highest bit the lowest band (the decoder's eight 500 Hz bands in pairs)
static void test_voicing(void)
{
	static const uint8_t BANDS[16] = { 0, 192, 48, 240, 12, 204, 60, 252, 3, 195, 51, 243, 15, 207, 63, 255 };
	mbe2400_state_t s;
	mbe_params_t p;

	for (int b1 = 0; b1 < 16; b1++)
		for (int b0 = 0; b0 < MBE2400_PITCHES; b0 += 5) {
			uint16_t b[MBE2400_FIELDS] = { (uint16_t)b0, (uint16_t)b1, 10, 0, 0, 0, 0, 0, 0 };
			double f0 = mbe2400_w0(b0) / 4294967296.0;
			int bad = 0;

			mbe2400_state_reset(&s);
			CHECK(mbe2400_unpack(&s, b, &p) == MBE2400_FRAME_OK, "b0 %d", b0);
			for (int l = 1; l <= p.L; l++)
				bad += p.v[l - 1] != ((BANDS[b1] >> (int)(16 * l * f0)) & 1);
			CHECK(bad == 0, "b1 %d b0 %d: %d harmonics voiced wrongly", b1, b0, bad);
		}
}

// b0 126 and 127 play as silence and reset the prediction
static void test_other_frames(void)
{
	mbe2400_state_t s, r;
	mbe_params_t p;
	uint16_t b[MBE2400_FIELDS] = { 60, 15, 40, 100, 50, 3, 4, 5, 6 };

	mbe2400_state_reset(&r);
	for (int b0 = 126; b0 <= 127; b0++) {
		mbe2400_state_reset(&s);
		mbe2400_unpack(&s, b, &p);
		b[0] = (uint16_t)b0;
		CHECK(mbe2400_unpack(&s, b, &p) == MBE2400_FRAME_SILENCE && p.L == 0, "b0 %d not silence", b0);
		CHECK(memcmp(&s, &r, sizeof s) == 0, "b0 %d: state not reset", b0);
		b[0] = 60;
	}
}

// The quantizer against the dequantizer: what pack leaves is what the receiver reconstructs from the fields, and the
// two prediction states stay equal; smooth envelopes come back close
static void test_quantizer(void)
{
	mbe2400_state_t se, sd;
	double err = 0;
	int n = 0, mism = 0;

	mbe2400_state_reset(&se);
	mbe2400_state_reset(&sd);
	for (int fr = 0; fr < 400; fr++) {
		int b0 = (int)(rnd() % MBE2400_PITCHES), L = mbe2400_pitch_harmonics(b0);
		double tilt = -(double)(rnd() % 1000) / 1000 * 6, lvl = 2 + (double)(rnd() % 1000) / 1000 * 6;
		double fp = 300 + rnd() % 500, fq = 1200 + rnd() % 1500, f0 = mbe2400_w0(b0) / 4294967296.0 * 8000;
		mbe_params_t p, q;
		uint16_t b[MBE2400_FIELDS];
		double want[MBE_MAXL];

		memset(&p, 0, sizeof p);
		for (int l = 1; l <= L; l++) {
			double f = l * f0, a = lvl + tilt * f / 4000 + 2.5 * exp(-pow((f - fp) / 200, 2)) +
				   1.5 * exp(-pow((f - fq) / 300, 2));

			want[l - 1] = a;
			p.a[l - 1] = (int16_t)lrint(a * 2048);
			p.v[l - 1] = f < 1500;
		}
		mbe2400_pack(&se, b0, &p, b);
		CHECK(mbe2400_unpack(&sd, b, &q) == MBE2400_FRAME_OK, "frame %d", fr);
		mism += q.L != p.L || q.w0 != p.w0 || memcmp(q.a, p.a, (size_t)L * 2) != 0 || memcmp(q.v, p.v, (size_t)L) != 0;
		for (int l = 1; l <= L; l++) {
			double d = p.a[l - 1] / 2048.0 - want[l - 1];

			if (fr >= 10) {
				err += d * d;
				n++;
			}
		}
	}
	CHECK(mism == 0, "%d frames reconstructed differently at the two ends", mism);
	CHECK(memcmp(&se, &sd, sizeof se) == 0, "prediction states differ");
	err = sqrt(err / n) * 6.02;
	printf("# quantizer: smooth envelopes back within %.2f dB rms\n", err);
	CHECK(err < 4.0, "envelopes %.2f dB rms off", err);
}

// --- speech ---

typedef struct {
	mbe2400_state_t es, ds;
	mbe_est_state_t est;
	mbe_params_t ep, dp;
	int32_t ebuf[MBE_BUF_WORDS], dbuf[MBE_BUF_WORDS];
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	mbe2400_enc_mem_t e;
	mbe2400_mem_t d;
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof *r);
	r->e = (mbe2400_enc_mem_t){ &r->es, &r->est, &r->ep, r->ebuf };
	r->d = (mbe2400_mem_t){ &r->ds, &r->dp, { &r->ms, &r->prev, &r->cur, r->dbuf } };
	mbe2400_enc_init(&r->e);
	mbe2400_dec_init(&r->d);
}

// The core's options the front end uses. MBE_UV_DENSITY: the same unvoiced amplitudes play 3 dB quieter an octave
// down. MBE_PHASE_REGEN: voiced phases follow from the amplitudes alone, not the noise seed
static double synth_rms(uint32_t w0, int voiced, uint16_t flags, uint32_t seed)
{
	rig_t *r = malloc(sizeof *r);
	mbe_params_t p;
	int16_t pcm[MBE_FRAME_LEN];
	double e = 0;

	rig_init(r);
	r->ms.rng = seed;
	memset(&p, 0, sizeof p);
	p.w0 = w0;
	p.L = (int16_t)(0x70000000u / w0);
	p.flags = flags;
	for (int l = 0; l < p.L; l++) {
		p.a[l] = 9 * 2048;
		p.v[l] = (uint8_t)voiced;
	}
	for (int f = 0; f < 20; f++) {
		mbe_synth_frame(&r->d.mbe, &p, 0, pcm);
		if (f >= 8)
			for (int i = 0; i < MBE_FRAME_LEN; i++)
				e += (double)pcm[i] * pcm[i];
	}
	free(r);
	return e;
}

static void test_core_options(void)
{
	double hi = synth_rms(0x08000000u, 0, MBE_UV_DENSITY, 1), lo = synth_rms(0x04000000u, 0, MBE_UV_DENSITY, 1);
	double band = synth_rms(0x08000000u, 0, 0, 1) / synth_rms(0x04000000u, 0, 0, 1);
	double d = 10 * log10(hi / lo / band);

	CHECK(fabs(d - 3.01) < 0.5, "density: an octave down %.2f dB quieter than band amplitudes, want 3", d);
	d = 10 * log10(synth_rms(0x08000000u, 0, 0, 1) / synth_rms(0x08000000u, 0, MBE_UV_DENSITY, 1));
	CHECK(fabs(d - 10 * log10(32)) < 0.5, "density at 250 Hz: %.2f dB under band amplitudes, want 15.05", d);
	CHECK(synth_rms(0x06000000u, 1, MBE_PHASE_REGEN, 1) == synth_rms(0x06000000u, 1, MBE_PHASE_REGEN, 77),
	      "regenerated phases depend on the noise seed");
}

static int16_t *read_pcm(const char *path, size_t *frames)
{
	FILE *f = fopen(path, "rb");
	int16_t *x;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f) / 2;
	fseek(f, 0, SEEK_SET);
	x = malloc((size_t)n * 2);
	if (fread(x, 2, (size_t)n, f) != (size_t)n)
		n = 0;
	fclose(f);
	*frames = (size_t)n / MBE_FRAME_LEN;
	return x;
}

#define NBANDS	16
// log power of 16 bands of 250 Hz (100 Hz up) of a 256-sample Hann window centered at sample c
static void bands(const int16_t *x, long n, long c, double *out)
{
	double re[64], im[64];

	for (int k = 0; k < 64; k++)
		re[k] = im[k] = 0;
	for (int i = 0; i < 256; i++) {
		long t = c - 128 + i;
		double v = t >= 0 && t < n ? x[t] * (0.5 - 0.5 * cos(2 * M_PI * i / 256)) : 0;

		for (int k = 0; k < 64; k++) {
			re[k] += v * cos(2 * M_PI * k * i / 128);
			im[k] -= v * sin(2 * M_PI * k * i / 128);
		}
	}
	for (int b = 0; b < NBANDS; b++) {
		double p = 1;

		for (int k = 2 + 4 * b * 15 / 16; k < 2 + 4 * (b + 1) * 15 / 16; k++)
			p += re[k] * re[k] + im[k] * im[k];
		out[b] = 10 * log10(p);
	}
}

// Mean band distance in dB over the frames where the input is speech (within 40 dB of its loudest), level removed per
// frame, at the delay that fits best; and the level difference of those frames
static void distance(const int16_t *in, const int16_t *out, long n, double *dist, double *level)
{
	double best = 1e9, lv = 0;
	double peak = 0;
	long nf = n / MBE_FRAME_LEN;
	double (*bi)[NBANDS] = malloc(sizeof(double[NBANDS]) * (size_t)nf);

	for (long f = 0; f < nf; f++) {
		double s = 0;

		bands(in, n, f * MBE_FRAME_LEN + 80, bi[f]);
		for (int b = 0; b < NBANDS; b++)
			s += pow(10, bi[f][b] / 10);
		if (s > peak)
			peak = s;
	}
	for (int dly = 0; dly <= 400; dly += 20) {
		double sum = 0, ls = 0;
		int cnt = 0;

		for (long f = 2; f < nf - 4; f++) {
			double bo[NBANDS], si = 0, so = 0, m = 0, d = 0;

			for (int b = 0; b < NBANDS; b++)
				si += pow(10, bi[f][b] / 10);
			if (si < peak * 1e-4)
				continue;
			bands(out, n, f * MBE_FRAME_LEN + 80 + dly, bo);
			for (int b = 0; b < NBANDS; b++) {
				so += pow(10, bo[b] / 10);
				m += bo[b] - bi[f][b];
			}
			m /= NBANDS;
			for (int b = 0; b < NBANDS; b++)
				d += pow(bo[b] - bi[f][b] - m, 2);
			sum += sqrt(d / NBANDS);
			ls += 10 * log10(so / si);
			cnt++;
		}
		if (cnt && sum / cnt < best) {
			best = sum / cnt;
			lv = ls / cnt;
		}
	}
	free(bi);
	*dist = best;
	*level = lv;
}

// The test voice set through the encoder and the decoder, clean and at 2 % bit errors. The full-rate codec's
// decoded files in tests/data/voice score about 5.4-5.8 dB on this measure
static void test_voice(const char *set)
{
	char path[256];
	size_t frames;
	int16_t *in, *out;
	rig_t *r = malloc(sizeof *r);
	double dist, level;
	long n;

	snprintf(path, sizeof path, "../data/voice/%s.pcm", set);
	in = read_pcm(path, &frames);
	CHECK(in != NULL && frames > 0, "%s: no input", path);
	if (!in) {
		free(r);
		return;
	}
	n = (long)(frames * MBE_FRAME_LEN);
	out = calloc((size_t)n, 2);
	for (int ber = 0; ber <= 2; ber += 2) {
		int bad = 0;

		rig_init(r);
		rng_state = 0x5EED;
		for (size_t f = 0; f < frames; f++) {
			uint8_t fr[MBE2400_BYTES], e;

			mbe2400_enc_frame(&r->e, in + f * MBE_FRAME_LEN, fr);
			for (int a = 0; a < 72; a++)
				if ((int)(rnd() % 100) < ber)
					flip(fr, a);
			mbe2400_dec_frame(&r->d, fr, &e, out + f * MBE_FRAME_LEN);
			bad += MBE2400_ERRS_BAD(e);
		}
		distance(in, out, n, &dist, &level);
		printf("# %s, %d %% bit errors: band distance %.2f dB, level %+.1f dB, %d bad frames\n", set, ber, dist, level,
		       bad);
		if (!ber) {
			CHECK(dist < 6.0, "%s: band distance %.2f dB", set, dist);
			CHECK(fabs(level) < 6, "%s: level %+.1f dB", set, level);
		} else {
			CHECK(dist < 6.5, "%s at 2 %% bit errors: band distance %.2f dB", set, dist);
		}
	}
	free(in);
	free(out);
	free(r);
}

// Bad frames repeat the last good one, each quieter, then the output is silent; a good frame resumes
static void test_concealment(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN], tone[MBE_FRAME_LEN];
	uint8_t fr[MBE2400_BYTES];
	double rms[6];

	rig_init(r);
	for (int f = 0; f < 30; f++) {
		for (int i = 0; i < MBE_FRAME_LEN; i++)
			tone[i] = (int16_t)(6000 * sin(2 * M_PI * 150 * (f * MBE_FRAME_LEN + i) / 8000.0) +
					    3000 * sin(2 * M_PI * 450 * (f * MBE_FRAME_LEN + i) / 8000.0));
		mbe2400_enc_frame(&r->e, tone, fr);
		mbe2400_dec_frame(&r->d, fr, NULL, pcm);
	}
	for (int f = 0; f < 6; f++) {
		double s = 0;
		int run = mbe2400_dec_frame(&r->d, NULL, NULL, pcm);

		CHECK(run == (f < 4 ? f + 1 : 4), "bad frame %d: run %d", f, run);
		for (int i = 0; i < MBE_FRAME_LEN; i++)
			s += (double)pcm[i] * pcm[i];
		rms[f] = sqrt(s / MBE_FRAME_LEN);
	}
	CHECK(rms[0] > 500 && rms[1] < rms[0] && rms[2] < rms[1], "repeats do not fade: %.0f %.0f %.0f", rms[0], rms[1],
	      rms[2]);
	CHECK(rms[4] < 5 && rms[5] < 5, "not muted: %.1f %.1f", rms[4], rms[5]);
	CHECK(mbe2400_dec_frame(&r->d, fr, NULL, pcm) == 0, "a good frame does not resume");
	free(r);
}

// The encoder sends the silence frame for digital silence, and the decoder plays it near silent
static void test_digital_silence(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t z[MBE_FRAME_LEN] = { 0 }, pcm[MBE_FRAME_LEN];
	uint8_t fr[MBE2400_BYTES];
	int peak = 0;

	rig_init(r);
	for (int f = 0; f < 10; f++) {
		mbe2400_enc_frame(&r->e, z, fr);
		CHECK(memcmp(fr, mbe2400_silence, MBE2400_BYTES) == 0, "frame %d is not the silence frame", f);
		mbe2400_dec_frame(&r->d, fr, NULL, pcm);
		for (int i = 0; i < MBE_FRAME_LEN; i++)
			peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
	}
	CHECK(peak < 300, "silence plays at a peak of %d", peak);
	CHECK(memcmp(&r->es, &r->ds, sizeof r->es) == 0, "the two ends' prediction differs after silence");
	free(r);
}

int main(void)
{
	test_silence_frame();
	test_frame_round_trip();
	test_interleave();
	test_fec();
	test_whitening();
	test_pitch();
	test_voicing();
	test_other_frames();
	test_quantizer();
	test_core_options();
	test_concealment();
	test_digital_silence();
	test_voice("test_transmission");
	test_voice("ten_second_countdown");
	printf("# %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
