// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The 2400 bit/s MBE front end (core/mbe/mbe2400.h, docs/spec/mbe-2400.md): the frame's bit map, FEC and interleave
// restated from the spec, the quantizer against the dequantizer, frames that play as silence, concealment sequences,
// random frames and wild parameters through the decoder, the core's options against what they are defined to do (the
// regenerated phases against a resonator's own), and the test voice set through the encoder and the decoder
// (spectral distance and level against the input, clean and with bit errors). CI runs this under
// -fsanitize=address,undefined too (make SANITIZE=1).
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

// pitch codes: the fundamental falls 1.47 % a step from 409 Hz; harmonics stay under 4 kHz
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

// One all-voiced frame with regenerated phases through the synthesis: the phase offsets it used, in turns
static void regen_of(const double *log2a, int L, uint32_t w0, double *th)
{
	rig_t *r = malloc(sizeof *r);
	mbe_params_t p;
	int16_t pcm[MBE_FRAME_LEN];

	rig_init(r);
	memset(&p, 0, sizeof p);
	p.w0 = w0;
	p.L = (int16_t)L;
	p.flags = MBE_PHASE_REGEN | MBE_NO_ENHANCE;
	for (int l = 0; l < L; l++) {
		p.a[l] = (int16_t)lrint(log2a[l] * 2048);
		p.v[l] = 1;
	}
	mbe_synth_frame(&r->d.mbe, &p, 0, pcm);
	for (int l = 0; l < L; l++)
		th[l] = (int16_t)r->prev.th[l] / 65536.0;
	free(r);
}

// The regenerated phases (docs/spec/mbe.md 1.2) against what they are defined as. A flat envelope leaves the
// dispersion alone, k l^2. A slope of s log2 units a harmonic adds 0.44 / pi * 16 s ln 2 radians where all 8 terms
// are inside the frame: a lead on a rising slope, a lag on a falling one. And through a resonance the phases follow
// the resonator's own (a two-pole filter's, which is minimum phase): the same sign, about a third of the size
static void test_regen_phase(void)
{
	enum { L = 38 };
	double a[L], th[L], flat[L], k;
	int bad = 0;

	for (int l = 0; l < L; l++)
		a[l] = 9;
	regen_of(a, L, 0x03333333u, flat);
	k = flat[0];
	CHECK(k > 0.002 && k < 0.02, "dispersion %.4f of a turn at the first harmonic", k);
	for (int l = 1; l <= 9; l++)		// the 16-bit phase wraps above
		bad += fabs(flat[l - 1] - k * l * l) > 2e-4;
	CHECK(bad == 0, "flat envelope: %d of 9 phases off k l^2", bad);
	for (int sign = -1; sign <= 1; sign += 2) {
		double want = sign * 0.44 / M_PI * 16 * 0.125 * log(2) / (2 * M_PI), worst = 0;

		for (int l = 0; l < L; l++)
			a[l] = 9 + sign * 0.125 * (l - L / 2);
		regen_of(a, L, 0x03333333u, th);
		for (int l = 9; l <= L - 8; l++) {
			double d = th[l - 1] - flat[l - 1];

			d -= floor(d + 0.5);
			if (fabs(d - want) > worst)
				worst = fabs(d - want);
		}
		CHECK(worst < 0.03 * fabs(want), "slope %+d: phases %.5f of a turn off %.5f", sign, worst, want);
	}
	for (int f0 = 80; f0 <= 125; f0 += 15) {
		// H(z) = 1 / (1 - 2 r cos(wc) z^-1 + r^2 z^-2), a resonance at 1.8 kHz
		double rr = 0.9, wc = 2 * M_PI * 1800 / 8000, sxy = 0, sxx = 0, syy = 0, mx = 0, my = 0, tru[L], slope, corr;
		int n = 3400 / f0 > L ? L : 3400 / f0, cnt = 0;

		for (int l = 1; l <= n; l++) {
			double w = 2 * M_PI * l * f0 / 8000;
			double re = 1 - 2 * rr * cos(wc) * cos(w) + rr * rr * cos(2 * w);
			double im = 2 * rr * cos(wc) * sin(w) - rr * rr * sin(2 * w);

			a[l - 1] = 9 - 0.5 * log2(re * re + im * im);
			tru[l - 1] = -atan2(im, re) / (2 * M_PI);
		}
		regen_of(a, n, (uint32_t)(f0 * 536870.912), th);
		regen_of((double[L]){ 0 }, n, (uint32_t)(f0 * 536870.912), flat);
		// the harmonics with all 8 terms inside the frame
		for (int l = 9; l <= n - 8; l++) {
			th[l - 1] -= flat[l - 1];
			th[l - 1] -= floor(th[l - 1] + 0.5);
			mx += tru[l - 1];
			my += th[l - 1];
			cnt++;
		}
		mx /= cnt;
		my /= cnt;
		for (int l = 9; l <= n - 8; l++) {
			sxy += (tru[l - 1] - mx) * (th[l - 1] - my);
			sxx += (tru[l - 1] - mx) * (tru[l - 1] - mx);
			syy += (th[l - 1] - my) * (th[l - 1] - my);
		}
		slope = sxy / sxx;
		corr = sxy / sqrt(sxx * syy);
		printf("# regenerated phases, resonance at %d Hz pitch: %.2f of the filter's, correlation %.2f\n", f0, slope, corr);
		CHECK(corr > 0.9, "f0 %d: correlation %.2f with the resonator's phases", f0, corr);
		CHECK(slope > 0.25 && slope < 0.5, "f0 %d: %.2f of the resonator's phases", f0, slope);
	}
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
	for (int ber = 0; ber <= 5; ber += ber ? 3 : 2) {
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
			CHECK(fabs(level) < 1.5, "%s: level %+.1f dB", set, level);
			CHECK(memcmp(&r->es, &r->ds, sizeof r->es) == 0, "%s: the two ends' prediction differs", set);
		} else if (ber == 2) {
			CHECK(dist < 6.5, "%s at 2 %% bit errors: band distance %.2f dB", set, dist);
			CHECK(fabs(level) < 1.5, "%s at 2 %% bit errors: level %+.1f dB", set, level);
		} else {
			// frames are lost here (the first word fails), repeated and muted: still speech, and no louder
			CHECK(bad > 0, "%s at 5 %% bit errors: no bad frame", set);
			CHECK(dist < 7.5, "%s at 5 %% bit errors: band distance %.2f dB", set, dist);
			CHECK(level < 3 && level > -6, "%s at 5 %% bit errors: level %+.1f dB", set, level);
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
	CHECK(rms[4] < 20 && rms[5] < 5, "not muted: %.1f %.1f", rms[4], rms[5]);	// the output filter's tail
	CHECK(mbe2400_dec_frame(&r->d, fr, NULL, pcm) == 0, "a good frame does not resume");
	free(r);
}

static double frame_rms(const int16_t *x)
{
	double e = 0;

	for (int i = 0; i < MBE_FRAME_LEN; i++)
		e += (double)x[i] * x[i];
	return sqrt(e / MBE_FRAME_LEN);
}

// a steady two-harmonic tone through the encoder and the decoder; the last frame sent is left in fr
static void play_tone(rig_t *r, int frames, uint8_t fr[MBE2400_BYTES], int16_t pcm[MBE_FRAME_LEN])
{
	int16_t tone[MBE_FRAME_LEN];

	for (int f = 0; f < frames; f++) {
		for (int i = 0; i < MBE_FRAME_LEN; i++)
			tone[i] = (int16_t)(6000 * sin(2 * M_PI * 150 * (f * MBE_FRAME_LEN + i) / 8000.0) +
					    3000 * sin(2 * M_PI * 450 * (f * MBE_FRAME_LEN + i) / 8000.0));
		mbe2400_enc_frame(&r->e, tone, fr);
		mbe2400_dec_frame(&r->d, fr, NULL, pcm);
	}
}

// Concealment at the edges: before any good frame, across a short gap, after a frame that plays as silence, and by
// what the FEC found (which frames count as bad). The prediction is held through the repeats and dropped after them
static void test_conceal_sequences(void)
{
	rig_t *r = malloc(sizeof *r);
	mbe2400_state_t fresh, held;
	int16_t pcm[MBE_FRAME_LEN];
	uint8_t fr[MBE2400_BYTES], bad[MBE2400_BYTES], e;
	uint16_t b[MBE2400_FIELDS] = { 60, 15, 40, 100, 50, 3, 4, 5, 6 };
	double lvl, rep[3];

	mbe2400_state_reset(&fresh);
	// nothing to repeat yet: silent from the first lost frame, the count as ever
	rig_init(r);
	for (int f = 0; f < 6; f++) {
		int run = mbe2400_dec_frame(&r->d, NULL, &e, pcm);

		CHECK(run == (f < 4 ? f + 1 : 4) && e == 0xFF, "lost frame %d at the start: run %d errs %02x", f, run, e);
		CHECK(frame_rms(pcm) == 0, "lost frame %d at the start plays at %.1f", f, frame_rms(pcm));
	}
	// a gap of two: 1 dB a repeat, the prediction held, and the next good frame resumes at once
	play_tone(r, 30, fr, pcm);
	lvl = frame_rms(pcm);
	held = r->ds;
	for (int f = 0; f < 3; f++) {
		CHECK(mbe2400_dec_frame(&r->d, NULL, NULL, pcm) == f + 1, "repeat %d", f + 1);
		rep[f] = frame_rms(pcm);
		CHECK(memcmp(&r->ds, &held, sizeof held) == 0, "repeat %d changed the prediction", f + 1);
	}
	// a frame's level moves from the last frame's to its own, so each is half a step behind: 0.5, 1.5, 2.5 dB down
	for (int f = 0; f < 3; f++) {
		CHECK(fabs(20 * log10(lvl / rep[f]) - (f + 0.5)) < 0.4, "repeat %d is %.2f dB under the tone, want %.1f", f + 1,
		      20 * log10(lvl / rep[f]), f + 0.5);
	}
	CHECK(mbe2400_dec_frame(&r->d, fr, NULL, pcm) == 0 && frame_rms(pcm) > rep[2], "the frame after a gap");
	// past the repeats: silent, the prediction dropped
	for (int f = 0; f < 5; f++)
		mbe2400_dec_frame(&r->d, NULL, NULL, pcm);
	CHECK(frame_rms(pcm) < 20, "the fifth lost frame plays at %.1f", frame_rms(pcm));
	CHECK(memcmp(&r->ds, &fresh, sizeof fresh) == 0, "the prediction is kept past the repeats");

	// a frame that plays as silence is a good frame: what is repeated after it is silence, not the speech before
	play_tone(r, 30, fr, pcm);
	b[0] = 126;
	mbe2400_frame_pack(b, bad);
	CHECK(mbe2400_dec_frame(&r->d, bad, &e, pcm) == 0 && e == 0, "a frame of another kind: errs %02x", e);
	CHECK(memcmp(&r->ds, &fresh, sizeof fresh) == 0, "a frame of another kind keeps the prediction");
	mbe2400_dec_frame(&r->d, NULL, NULL, pcm);	// the tone's tail fades through the frame of silence
	CHECK(mbe2400_dec_frame(&r->d, NULL, NULL, pcm) == 2 && frame_rms(pcm) < 20, "speech repeated after a silent frame: %.1f",
	      frame_rms(pcm));

	// what the FEC found: the first word failed, or it took three corrections and the second word failed: bad. The
	// second word failed alone: used
	for (int c = 0; c < 3; c++) {
		static const uint8_t NERR[3][2] = { { 4, 0 }, { 3, 4 }, { 2, 4 } };
		int run;

		rig_init(r);
		play_tone(r, 30, fr, pcm);
		held = r->ds;
		memcpy(bad, fr, sizeof bad);
		for (int w = 0; w < 2; w++)
			for (int k = 0; k < NERR[c][w]; k++)
				flip(bad, AIR[w][5 * k + c]);
		run = mbe2400_dec_frame(&r->d, bad, &e, pcm);
		// (with the first word failed the second is descrambled by a wrong seed: its count says nothing)
		CHECK(MBE2400_ERRS_C0(e) == NERR[c][0] && (!c || MBE2400_ERRS_C1(e) == NERR[c][1]), "case %d: errs %02x", c, e);
		CHECK(run == (c < 2), "case %d: run %d", c, run);
		CHECK((memcmp(&r->ds, &held, sizeof held) == 0) == (c < 2), "case %d: the prediction %s", c,
		      c < 2 ? "moved on a bad frame" : "did not move on a used frame");
	}
	free(r);
}

// Random bytes, lost frames, loud frames, silence and frames of another kind in any order: the
// count of bad frames follows what the FEC found, the state stays a frame's, and silence plays as silence after it all
static void test_random_frames(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN];
	int wrong = 0, state = 0, used = 0, lost = 0, peak = 0;

	rig_init(r);
	rng_state = 0xF00D;
	for (int n = 0; n < 12000; n++) {
		uint8_t fr[MBE2400_BYTES], e = 0;
		uint16_t b[MBE2400_FIELDS];
		uint32_t kind = rnd() % 16;
		int run, was = r->ms.run;

		for (int i = 0; i < MBE2400_BYTES; i++)
			fr[i] = (uint8_t)rnd();
		if (kind >= 10 && kind < 14) {
			random_fields(b);
			if (kind == 10)
				b[0] = (uint16_t)(126 + (rnd() & 1));
			if (kind == 11)
				b[2] = 63;		// the loudest gain, frame after frame
			mbe2400_frame_pack(b, fr);
			if (kind == 12)
				memcpy(fr, mbe2400_silence, sizeof fr);
		}
		run = mbe2400_dec_frame(&r->d, kind < 14 ? fr : NULL, &e, pcm);
		if (kind >= 14 || MBE2400_ERRS_BAD(e)) {
			wrong += run != (was < 4 ? was + 1 : 4) || (kind >= 14) != (e == 0xFF);
			lost++;
		} else {
			wrong += run != 0;
			used++;
		}
		state += r->ds.q.L < 9 || r->ds.q.L > MBE_MAXL || r->prev.L < 0 || r->prev.L > MBE_MAXL ||
			 r->prev.w0 < 0x00400000u || r->prev.w0 > 0x1FFFFFFFu;
	}
	CHECK(wrong == 0, "%d of 12000 frames with the wrong count of bad frames", wrong);
	CHECK(state == 0, "%d of 12000 frames left a state out of range", state);
	CHECK(used > 4000 && lost > 4000, "%d frames used, %d lost: the mix is off", used, lost);
	for (int f = 0; f < 12; f++) {
		mbe2400_dec_frame(&r->d, mbe2400_silence, NULL, pcm);
		if (f >= 6)
			for (int i = 0; i < MBE_FRAME_LEN; i++)
				peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
	}
	CHECK(peak < 300, "silence after random frames plays at a peak of %d", peak);
	free(r);
}

// Parameters no front end makes, straight into the synthesis with the options on and any concealment policy: the
// frame kept is always one the synthesis can play
static void test_wild_params(void)
{
	static const mbe_conceal_t POL[] = { { 0, 0 }, { 3, 340 }, { 32767, 32767 }, { -1, -32768 }, { 1, -2048 } };
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN];
	int state = 0;

	rig_init(r);
	rng_state = 0xBAD5EED;
	for (int n = 0; n < 4000; n++) {
		mbe_params_t p;
		uint32_t k = rnd();
		int run;

		for (size_t i = 0; i < sizeof p; i++)
			((uint8_t *)&p)[i] = (uint8_t)rnd();
		p.L = (int16_t)(k % 4 == 0 ? (int)(rnd() % 400) - 100 : 1 + (int)(rnd() % MBE_MAXL));
		if (k & 16)
			p.w0 >>= rnd() % 12;
		for (int l = 0; l < MBE_MAXL; l++) {
			if ((k & 32) && l % 3)
				p.a[l] = MBE_A_SILENT;
			if (k & 64)
				p.a[l] = (int16_t)(32767 - (int)(rnd() % 3000));
			if (k & 128)
				p.v[l] &= 1;
		}
		run = mbe_synth_conceal(&r->d.mbe, (k >> 8) % 6 ? &POL[(k >> 8) % 6 - 1] : NULL, (k >> 12) % 9 ? &p : NULL,
					(k >> 16) % 3 == 0, pcm);
		state += run < 0 || r->prev.L < 0 || r->prev.L > MBE_MAXL || r->prev.w0 < 0x00400000u ||
			 r->prev.w0 > 0x1FFFFFFFu;
	}
	CHECK(state == 0, "%d of 4000 frames left a state out of range", state);
	free(r);
}

// What the encoder makes of signals at the edges of its input: full-scale noise, a full-scale square wave, DC, single
// clicks and a hum under its pitch range. Every frame is a valid one a receiver decodes without corrections, and the
// receiver's prediction stays the encoder's
static void test_encoder_extremes(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t x[MBE_FRAME_LEN], pcm[MBE_FRAME_LEN];
	int bad = 0;

	rig_init(r);
	rng_state = 0xE0E0;
	for (int f = 0; f < 240; f++) {
		uint8_t fr[MBE2400_BYTES], e;
		uint16_t b[MBE2400_FIELDS];
		int kind = f / 40;

		for (int i = 0; i < MBE_FRAME_LEN; i++) {
			int t = f * MBE_FRAME_LEN + i;

			x[i] = kind == 0 ? (int16_t)rnd() : kind == 1 ? (t / 20 % 2 ? 32767 : -32768) : kind == 2 ? 32767 :
			       kind == 3 ? (t % 800 == 0 ? -32768 : 0) : kind == 4 ? (int16_t)(20000 * sin(2 * M_PI * 30 * t / 8000.0)) :
			       (int16_t)(rnd() % 3) - 1;
		}
		mbe2400_enc_frame(&r->e, x, fr);
		e = mbe2400_frame_unpack(fr, b);
		bad += e != 0 || b[0] >= MBE2400_PITCHES || (b[8] & 1);
		mbe2400_dec_frame(&r->d, fr, NULL, pcm);
		bad += memcmp(&r->es, &r->ds, sizeof r->es) != 0;
	}
	CHECK(bad == 0, "%d of 240 frames not valid or not tracked by the receiver", bad);
	free(r);
}

// Steady signals come back at their level and stay there: a spectrum of two harmonics over deep gaps, one tone, and
// a full set of harmonics. (The shape codebooks cannot follow the first two; the gain code makes up the energy.)
static void test_steady_levels(void)
{
	rig_t *r = malloc(sizeof *r);

	for (int kind = 0; kind < 3; kind++) {
		double si = 0, so = 0, lo = 1e9, hi = 0;

		rig_init(r);
		for (int f = 0; f < 80; f++) {
			int16_t x[MBE_FRAME_LEN], pcm[MBE_FRAME_LEN];
			uint8_t fr[MBE2400_BYTES];

			for (int i = 0; i < MBE_FRAME_LEN; i++) {
				double t = (f * MBE_FRAME_LEN + i) / 8000.0, v = 0;

				if (kind == 0)
					v = 6000 * sin(2 * M_PI * 150 * t) + 3000 * sin(2 * M_PI * 450 * t);
				else if (kind == 1)
					v = 6000 * sin(2 * M_PI * 1000 * t);
				else
					for (int l = 1; l <= 20; l++)
						v += 4000.0 / l * cos(2 * M_PI * 120 * l * t + 0.7 * l * l);
				x[i] = (int16_t)v;
			}
			mbe2400_enc_frame(&r->e, x, fr);
			mbe2400_dec_frame(&r->d, fr, NULL, pcm);
			if (f >= 30) {
				double v = frame_rms(pcm);

				si += frame_rms(x) * frame_rms(x);
				so += v * v;
				lo = v < lo ? v : lo;
				hi = v > hi ? v : hi;
			}
		}
		printf("# steady signal %d: level %+.2f dB, frames within %.2f dB\n", kind, 10 * log10(so / si), 20 * log10(hi / lo));
		CHECK(fabs(10 * log10(so / si)) < 1.0, "steady signal %d plays %+.2f dB off", kind, 10 * log10(so / si));
		CHECK(20 * log10(hi / lo) < (kind == 2 ? 2.5 : 1.0), "steady signal %d: frame levels %.2f dB apart", kind,
		      20 * log10(hi / lo));
	}
	free(r);
}

// The encoder's pitch code for harmonic signals across its range: the code whose fundamental is nearest (the grid
// starts at code 1's 2^-4.3118 of the sample rate, code 0 a step above), all four bands voiced
static void test_encoder_pitch(void)
{
	static const uint8_t CODES[] = { 2, 15, 40, 70, 100, 122 };
	rig_t *r = malloc(sizeof *r);

	for (size_t c = 0; c < sizeof CODES; c++) {
		double f0 = mbe2400_w0(CODES[c]) / 4294967296.0;
		int16_t x[MBE_FRAME_LEN];
		uint8_t fr[MBE2400_BYTES];
		uint16_t b[MBE2400_FIELDS] = { 0 };
		int off = 0, unv = 0;

		rig_init(r);
		for (int f = 0; f < 14; f++) {
			for (int i = 0; i < MBE_FRAME_LEN; i++) {
				double v = 0;

				for (int l = 1; l * f0 < 0.45; l++)
					v += cos(2 * M_PI * l * f0 * (f * MBE_FRAME_LEN + i) + 0.7 * l * l) / sqrt(l);
				x[i] = (int16_t)(1500 * v);
			}
			mbe2400_enc_frame(&r->e, x, fr);
			if (f >= 4) {
				mbe2400_frame_unpack(fr, b);
				off += abs(b[0] - CODES[c]) > 1;
				unv += b[1] != 15;
			}
		}
		CHECK(off == 0, "code %d (%.1f Hz): %d of 10 frames coded more than a step off (last %d)", CODES[c], f0 * 8000, off,
		      b[0]);
		CHECK(unv == 0, "code %d: %d of 10 frames not all voiced (last b1 %d)", CODES[c], unv, b[1]);
	}
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
	test_regen_phase();
	test_concealment();
	test_conceal_sequences();
	test_random_frames();
	test_wild_params();
	test_digital_silence();
	test_encoder_extremes();
	test_steady_levels();
	test_encoder_pitch();
	test_voice("test_transmission");
	test_voice("ten_second_countdown");
	printf("# %s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
