// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 cleanq contributors
// SPDX-FileCopyrightText: 2026 mbe contributors
// SPDX-FileCopyrightText: 2010 mbelib Author (the decoder equations the reference restates, ISC, see LICENSES/ISC.txt)
/*
 * Host tests for the software D-STAR voice codec (core/dstar/dstar_codec.c and its parts, docs/spec/dstar-codec.md).
 * Every check runs the code; none reads source text.
 *
 * Quantizer (dstar_ambe_quant.c):
 *   - round trip: random speech-like parameters -> quantize -> an independent decoder state dequantizes the bits to
 *     exactly what the encoder believes it sent; requantizing the decoded parameters reproduces them
 *   - agreement: random 48-bit frames through the integer dequantizer and through a double-precision restatement of
 *     mbelib's decoder equations written here, frame after frame with both predictors running, field by field
 * Analysis and synthesis (dstar_mbe_analysis.c, dstar_mbe_synth.c), on synthetic signals whose parameters are known:
 *   harmonic tones 70..390 Hz (pitch, L, voicing, magnitudes), white noise (voicing, level), a voiced/unvoiced mix
 *   and a fricative, a pitch glide, a weak fundamental (no octave error), the snapped pitch surviving the quantizer,
 *   silence, a tone's level, the concealment of bad frames (fading repeats, comfort noise, then mute), full-scale input
 * Frame FEC (dstar_ambe_frame.c): every field through a frame and back, Golay correction and detection in C0 and C1,
 *   C1's data when it failed, the unprotected bits, and the air bit order proved on the standard's lost-packet frame
 * Leveler (dstar_mbe_level.c): a 16 dB step at the input, up and down, comes out within 2 dB; extra bass doesn't turn
 *   the speech band down; the decoded speech band sits at the pinned target; and a loud onset after quiet speech is
 *   capped at once, through the encoder and the decoder
 * Codec (dstar_codec.c): the vocoder interface, the direction switch, the bad-frame rule (C0 failed, a doubtful frame
 *   after a loss) and the concealment on failed and lost frames, the error count, tone frames
 *
 *   make -C tests/host run-dstar
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dstar/dstar_codec.h"
#include "dstar/dstar_ambe_frame.h"
#include "dstar/dstar_ambe_quant.h"
#include "dstar/dstar_mbe_analysis.h"
#include "dstar/dstar_mbe_fixed.h"
#include "dstar/dstar_mbe_level.h"
#include "dstar/dstar_mbe_synth.h"
#include "dmr/ambe/ambeFec.h"

// The quantizer's codebooks, for the double-precision reference decoder below (it computes everything else itself)
#include "dstar/dstar_ambe_tables.inc"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; if (failures < 40) { \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// --- quantizer ---

// Tolerances in log2 units unless stated. Q12 resolution is 2.4e-4; the transforms round a few times a value and the
// predictor carries 0.65 of the previous error forward, so the steady-state bound is a few 1e-3
#define TOL_LOG2	4e-3
#define TOL_W0_REL	1e-5
#define TOL_GAMMA	1e-4

static uint32_t qrng = 12345u;

static uint32_t qrnd(void)
{
	qrng ^= qrng << 13;
	qrng ^= qrng >> 17;
	qrng ^= qrng << 5;
	return qrng;
}

static double qfrand(void)
{
	return (qrnd() & 0xFFFFFF) / 16777216.0;
}

static dstar_ambe_scratch_t qscratch;

// Speech-like random parameters: a smooth spectral tilt plus formant bumps, banded voicing, pitch 60 to 400 Hz
static void random_params(dstar_ambe_params_t *p, double level)
{
	double f0 = (64.0 + 330.0 * pow(qfrand(), 1.5)) / 8000.0;
	unsigned L = (unsigned)(0.4627 / f0);
	if (L > 56)
		L = 56;
	if (L < 9)
		L = 9;
	p->f0_q24 = (uint32_t)(f0 * 16777216.0 + 0.5);
	p->L = (uint8_t)L;
	double tilt = -2.0 - 4.0 * qfrand(), f1 = 300 + 600 * qfrand(), f2 = 900 + 1800 * qfrand();
	double vcut = 4000 * qfrand();
	for (unsigned l = 1; l <= L; ++l) {
		double hz = l * f0 * 8000.0;
		double m = level + tilt * hz / 4000.0 + 3.0 * exp(-pow((hz - f1) / 150.0, 2)) +
			   2.0 * exp(-pow((hz - f2) / 250.0, 2)) + 0.3 * (qfrand() - 0.5);
		p->log2_amp[l] = (int32_t)lrint(m * 4096.0);
		p->voiced[l] = hz < vcut ? 1 : (qfrand() < 0.1);
	}
}

static void test_quant_round_trip(void)
{
	dstar_ambe_state_t enc, dec, enc2;
	dstar_ambe_init(&enc);
	dstar_ambe_init(&dec);
	double se = 0, sw = 0, level = 4.0;
	long n = 0, idem = 0, frames = 20000;
	for (long f = 0; f < frames; ++f) {
		dstar_ambe_params_t in, d1, d2, d3;
		uint16_t b[9], b2[9];
		level += 0.6 * (qfrand() - 0.5);
		if (level < -2)
			level = -2;
		if (level > 10)
			level = 10;
		random_params(&in, level);
		enc2 = enc;
		dstar_ambe_quantize(&enc, &qscratch, &in, b, &d1);
		CHECK(b[0] < 126, "tone index emitted b0=%u", b[0]);
		CHECK((b[8] & 1) == 0, "odd b8");
		int r = dstar_ambe_dequantize(&dec, &qscratch, b, &d2);
		CHECK(r == DSTAR_AMBE_VOICE, "voice frame decoded as tone");
		CHECK(d1.L == d2.L && d1.f0_q24 == d2.f0_q24, "pitch mismatch frame %ld", f);
		CHECK(memcmp(d1.voiced + 1, d2.voiced + 1, d1.L) == 0, "voicing mismatch frame %ld", f);
		CHECK(memcmp(d1.log2_amp + 1, d2.log2_amp + 1, d1.L * sizeof(int32_t)) == 0,
		      "encoder and decoder disagree on magnitudes, frame %ld", f);
		CHECK(memcmp(&enc, &dec, sizeof(enc)) == 0, "encoder and decoder state diverged, frame %ld", f);
		// Requantize what was decoded, from the same starting state
		dstar_ambe_quantize(&enc2, &qscratch, &d1, b2, &d3);
		if (memcmp(d3.log2_amp + 1, d1.log2_amp + 1, d1.L * 4) == 0 &&
		    memcmp(d3.voiced + 1, d1.voiced + 1, d1.L) == 0 && d3.f0_q24 == d1.f0_q24)
			++idem;
		// The quantization error, at the input's own harmonics where both exist
		double rel = (double)d1.f0_q24 / in.f0_q24;
		CHECK(fabs(log2(rel)) <= 0.5 * 0.021336 + 1e-4, "pitch error %.5f octaves", log2(rel));
		for (unsigned l = 1; l <= d1.L && l <= in.L; ++l) {
			double x = l * rel;	// where decoded harmonic l falls on the input grid
			unsigned i0 = (unsigned)x;
			if (i0 < 1 || i0 + 1 > in.L)
				continue;
			double fr = x - i0;
			double t = ((1 - fr) * in.log2_amp[i0] + fr * in.log2_amp[i0 + 1]) / 4096.0;
			double e = d1.log2_amp[l] / 4096.0 - t;
			se += e * e;
			++n;
			sw += d1.voiced[l] != in.voiced[i0 + (fr >= 0.5)];
		}
	}
	printf("  quantizer round trip: %ld frames, magnitude error RMS %.3f log2 (%.2f dB), voicing mismatch %.1f %%, "
	       "requantizing reproduces %.2f %%\n", frames, sqrt(se / n), 6.0206 * sqrt(se / n), 100.0 * sw / n,
	       100.0 * idem / frames);
	CHECK(idem * 1000 >= frames * 999, "requantizing decoded parameters changed them in %ld frames", frames - idem);
	CHECK(sqrt(se / n) < 0.8, "magnitude error too large");
}

// mbelib's mbe_decodeAmbe2400Parms restated in double from its equations (docs/spec/dstar-codec.md 3), with its
// parameter memory: an independent check of the integer dequantizer's arithmetic
typedef struct {
	int L;
	double gamma;
	double log2M[DSTAR_AMBE_MAX_L + 2];
} ref_state_t;

static void ref_init(ref_state_t *s)
{
	s->L = 30;
	s->gamma = 0;
	for (int l = 0; l < DSTAR_AMBE_MAX_L + 2; l++)
		s->log2M[l] = 0;
}

// Returns 0 for voice, 3 for a tone or reserved frame (mbelib's codes); out_* receive the decoded parameters
static int ref_decode(ref_state_t *s, const uint16_t b[9], double *w0, int *L_out, int voiced[], double log2M[])
{
	int b0 = b[0] & 127;
	if (b0 >= 126) {
		ref_init(s);
		return 3;
	}
	double f0 = tab_f0_q24[b0] / 16777216.0;	// mbelib's float32 value
	int L = tab_L[b0];
	*w0 = 2 * M_PI * f0;
	*L_out = L;
	for (int l = 1; l <= L; l++) {
		int jl = (int)((float)l * 16.0f * (float)f0);
		voiced[l] = (tab_vuv[b[1] & 15] >> jl) & 1;
	}
	double gamma = tab_dg_q16[b[2] & 63] / 65536.0 + 0.5 * s->gamma;
	double G[9] = { 0 };
	for (int m = 0; m < 3; m++)
		G[2 + m] = tab_prba24_q7[(b[3] & 511) * 3 + m] / 128.0;
	for (int m = 0; m < 4; m++)
		G[5 + m] = tab_prba58_q7[(b[4] & 127) * 4 + m] / 128.0;
	double R[9];
	for (int i = 1; i <= 8; i++) {
		R[i] = 0;
		for (int m = 1; m <= 8; m++)
			R[i] += (m == 1 ? 1.0 : 2.0) * G[m] * cos(M_PI * (m - 1) * (i - 0.5) / 8.0);
	}
	const uint8_t *J = &tab_J[(L - 9) * 4];
	int hidx[4] = { b[5] & 15, b[6] & 15, b[7] & 15, b[8] & 14 };
	double T[DSTAR_AMBE_MAX_L + 1], sumT = 0;
	int pos = 1;
	for (int i = 0; i < 4; i++) {
		double C[18] = { 0 };
		C[1] = (R[2 * i + 1] + R[2 * i + 2]) / 2.0;
		C[2] = (R[2 * i + 1] - R[2 * i + 2]) / (2.0 * sqrt(2.0));
		for (int k = 3; k <= J[i] && k <= 6; k++)
			C[k] = tab_hoc_q7[(i * 16 + hidx[i]) * 4 + k - 3] / 128.0;
		for (int j = 1; j <= J[i]; j++) {
			double c = C[1];
			for (int k = 2; k <= J[i]; k++)
				c += 2.0 * C[k] * cos(M_PI * (k - 1) * (j - 0.5) / J[i]);
			T[pos++] = c;
			sumT += c;
		}
	}
	// Prediction from the previous frame: log2M[0] = log2M[1], and past the previous L its last value repeated
	double prev[DSTAR_AMBE_MAX_L + 2];
	for (int l = 1; l <= DSTAR_AMBE_MAX_L + 1; l++)
		prev[l] = l <= s->L ? s->log2M[l] : s->log2M[s->L];
	prev[0] = prev[1];
	double P[DSTAR_AMBE_MAX_L + 1], sumP = 0;
	for (int l = 1; l <= L; l++) {
		double k = (double)l * s->L / L;
		int ik = (int)k;
		double d = k - ik;
		P[l] = 0.65 * ((1 - d) * prev[ik] + d * prev[ik + 1]);
		sumP += P[l];
	}
	double big = gamma - 0.5 * log2((double)L) - sumT / L;
	for (int l = 1; l <= L; l++)
		log2M[l] = T[l] + P[l] - sumP / L + big;
	s->L = L;
	s->gamma = gamma;
	for (int l = 1; l <= L; l++)
		s->log2M[l] = log2M[l];
	return 0;
}

static void test_quant_agreement(void)
{
	dstar_ambe_state_t s;
	ref_state_t r;
	dstar_ambe_init(&s);
	ref_init(&r);
	double emax = 0, esum = 0, w0max = 0, gmax = 0;
	long n = 0, frames = 200000, tones = 0;
	(void)tab_f0_mid_q24;
	(void)tab_B_q14;
	(void)tab_cos_off;
	(void)tab_cos_q14;
	(void)tab_half_log2L_q12;
	(void)tab_exp2neg_q15;
	for (long f = 0; f < frames; ++f) {
		uint16_t b[9];
		b[0] = qrnd() % 128;
		b[1] = qrnd() % 16;
		b[2] = qrnd() % 64;
		b[3] = qrnd() % 512;
		b[4] = qrnd() % 128;
		b[5] = qrnd() % 16;
		b[6] = qrnd() % 16;
		b[7] = qrnd() % 16;
		b[8] = qrnd() % 16 & 14;
		// Mostly voice: tone frames reset both sides, so keep them rare to let the predictors run long sequences
		if (b[0] >= 126 && (qrnd() % 64))
			b[0] = qrnd() % 126;
		dstar_ambe_params_t o;
		double w0 = 0, lm[DSTAR_AMBE_MAX_L + 1];
		int L = 0, v[DSTAR_AMBE_MAX_L + 1];
		int rq = dstar_ambe_dequantize(&s, &qscratch, b, &o);
		int rm = ref_decode(&r, b, &w0, &L, v, lm);
		if (rm == 3) {
			CHECK(rq == DSTAR_AMBE_TONE, "tone disagreement");
			++tones;
			continue;
		}
		CHECK(rq == DSTAR_AMBE_VOICE, "frame type disagreement b0=%u", b[0]);
		CHECK(o.L == L, "L %u vs %d", o.L, L);
		double wr = fabs(2 * M_PI * o.f0_q24 / 16777216.0 / w0 - 1);
		if (wr > w0max)
			w0max = wr;
		double ge = fabs(s.gamma_q16 / 65536.0 - r.gamma);
		if (ge > gmax)
			gmax = ge;
		for (int l = 1; l <= L; ++l) {
			CHECK(o.voiced[l] == v[l], "voicing of harmonic %d, frame %ld", l, f);
			double e = fabs(o.log2_amp[l] / 4096.0 - lm[l]);
			if (e > emax)
				emax = e;
			esum += e;
			++n;
		}
	}
	printf("  agreement with the reference decoder: %ld random frames (%ld tone), L and every voicing bit identical; "
	       "log2M |err| max %.2e mean %.2e; w0 relative error max %.1e; gamma error max %.1e\n",
	       frames, tones, emax, esum / n, w0max, gmax);
	CHECK(emax <= TOL_LOG2, "log2M error %.2e above %.0e", emax, TOL_LOG2);
	CHECK(w0max <= TOL_W0_REL, "w0 error");
	CHECK(gmax <= TOL_GAMMA, "gamma error");
}

// --- analysis and synthesis ---

static uint32_t mrng = 987654321u;

static double mfrand(void)
{
	mrng ^= mrng << 13;
	mrng ^= mrng >> 17;
	mrng ^= mrng << 5;
	return (mrng >> 8) / 16777216.0;
}

static double gauss(void)
{
	double s = 0;
	for (int i = 0; i < 12; ++i)
		s += mfrand();
	return s - 6.0;
}

// A test signal; t is the sample index
typedef struct {
	double f0, f0_end;	// Hz; a linear glide over frames if they differ
	double amp1, tilt;	// the first harmonic's amplitude; dB an octave
	double fmax;		// harmonics below this
	double noise, nlo, nhi;	// noise rms and band (Hz); nlo = 0: white
	int frames;
	double weak1;		// the fundamental's gain (1 = normal)
} sig_t;

// Noise for a whole test signal, made once: white Gaussian, or band-passed by a 255-tap windowed sinc (a band-limited
// Gaussian process, independent from frame to frame)
#define NMAX	(200 * 160)
static double noise_buf[NMAX];

static void make_noise(const sig_t *g)
{
	static double w[NMAX + 256];
	for (int i = 0; i < NMAX + 256; ++i)
		w[i] = gauss();
	if (g->nlo == 0) {
		for (int i = 0; i < NMAX; ++i)
			noise_buf[i] = g->noise * w[i];
		return;
	}
	double h[255], e = 0;
	for (int k = 0; k < 255; ++k) {
		double t = k - 127, a = 2 * g->nhi / 8000.0, b = 2 * g->nlo / 8000.0;
		double s = t == 0 ? a - b : (sin(M_PI * a * t) - sin(M_PI * b * t)) / (M_PI * t);
		h[k] = s * (0.54 - 0.46 * cos(2 * M_PI * k / 254));
		e += h[k] * h[k];
	}
	// Unit-variance input through h has variance sum h^2; normalized so the band noise has rms g->noise
	for (int i = 0; i < NMAX; ++i) {
		double s = 0;
		for (int k = 0; k < 255; ++k)
			s += h[k] * w[i + k];
		noise_buf[i] = g->noise * s / sqrt(e);
	}
}

static void render(const sig_t *g, int16_t *out, long n)
{
	static double phase[64];
	if (n == 0) {
		for (int k = 0; k < 64; ++k)
			phase[k] = 2 * M_PI * mfrand();
		if (g->noise > 0)
			make_noise(g);
	}
	double total = g->frames * 160.0;
	for (int i = 0; i < 160; ++i, ++n) {
		double f = g->f0 + (g->f0_end - g->f0) * fmin(1.0, n / total), v = 0;
		for (int k = 1; k < 64 && k * f < g->fmax; ++k) {
			phase[k] += 2 * M_PI * k * f / 8000.0;
			double a = g->amp1 * pow(2.0, g->tilt / 6.02 * log2(k)) * (k == 1 ? g->weak1 : 1.0);
			v += a * cos(phase[k]);
		}
		if (g->noise > 0 && n < NMAX)
			v += noise_buf[n];
		out[i] = (int16_t)fmax(-32767, fmin(32767, v));
	}
}

// The D-STAR grid pitch b0 nearest f (cycles per sample), in log terms
static double grid_f0(int b)
{
	return pow(2.0, -4.311767578125 - 2.1336e-2 * (b + 0.5 - 1.5));	// DVSI's grid (dstar_ambe_tables.inc)
}

static int grid_index(double f)
{
	int best = 0;
	for (int b = 0; b < 126; ++b) {
		if (fabs(log(grid_f0(b) / f)) < fabs(log(grid_f0(best) / f)))
			best = b;
	}
	return best;
}

static dstar_mbe_ana_t ana;
static dstar_mbe_ana_scratch_t ana_scratch;
static dstar_mbe_synth_scratch_t synth_scratch;

// Call index -> the sample at the analyzed frame's center (480 samples before the end of the newest block)
static long center_of(int call)
{
	return (call + 1) * 160L - 1 - 480;
}

static void test_tones(void)
{
	const double f0s[] = { 70, 82, 100, 117, 133, 150, 175, 200, 230, 265, 300, 345, 390 };
	for (unsigned i = 0; i < sizeof(f0s) / sizeof(f0s[0]); ++i) {
		sig_t g = { f0s[i], f0s[i], 3000, -6, 3900, 0, 0, 0, 30, 1 };
		dstar_mbe_ana_init(&ana);
		ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
		int16_t pcm[160];
		int bad_pitch = 0, bad_v = 0, n = 0;
		double amp_err = 0, ref_err = 0;
		for (int f = 0; f < g.frames; ++f) {
			dstar_ambe_params_t p;
			render(&g, pcm, f * 160L);
			dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
			if (f < 5)
				continue;
			++n;
			double fe = p.f0_q24 / 16777216.0, fb = grid_f0(grid_index(f0s[i] / 8000.0));
			if (fabs(fe / fb - 1) > 1e-4)
				++bad_pitch;
			double fr = 64000.0 / ana.p8;
			if (fabs(fr / f0s[i] - 1) > ref_err)
				ref_err = fabs(fr / f0s[i] - 1);
			CHECK(p.L >= 9 && p.L <= 56, "L %d", p.L);
			for (int l = 1; l <= p.L; ++l)
				bad_v += !p.voiced[l];
			for (int l = 1; l <= 8 && l * f0s[i] < 3500; ++l) {
				// Magnitudes are measured at the grid harmonics; the tone is within half a grid step (0.75 %)
				double a = g.amp1 * pow(2.0, g.tilt / 6.02 * log2(l));
				double e = fabs(p.log2_amp[l] / 4096.0 - log2(a / 7.0));
				if (e > amp_err)
					amp_err = e;
			}
		}
		CHECK(bad_pitch == 0, "%.0f Hz: %d of %d frames off the nearest grid pitch", f0s[i], bad_pitch, n);
		CHECK(ref_err < 0.004, "%.0f Hz: refined pitch off by %.2f %%", f0s[i], 100 * ref_err);
		CHECK(bad_v == 0, "%.0f Hz: %d harmonics unvoiced", f0s[i], bad_v);
		// Below 90 Hz harmonics are closer than the window's main lobe (2.2 bins against +-2.3), so band energies
		// carry phase-dependent cross terms between neighbors: a limit of the 221-point window
		CHECK(amp_err < (f0s[i] < 90 ? 0.34 : 0.17), "%.0f Hz: magnitude error %.3f log2 (%.2f dB)", f0s[i], amp_err,
		      6.02 * amp_err);
		printf("  tone %3.0f Hz: refined pitch within %.2f %%, magnitudes within %.2f dB\n", f0s[i], 100 * ref_err,
		       6.02 * amp_err);
	}
}

// Synthesized output power over input power, dB, frames [skip, end)
static double level_db(const sig_t *g, int skip, int *voiced_harm, int *harm, uint8_t vband[8])
{
	dstar_mbe_ana_init(&ana);
	ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
	dstar_mbe_synth_t sy;
	dstar_mbe_synth_init(&sy);
	int16_t pcm[160], out[160];
	double pin = 0, pout = 0;
	int vc[8] = { 0 }, nf = 0;
	*voiced_harm = *harm = 0;
	for (int f = 0; f < g->frames; ++f) {
		dstar_ambe_params_t p;
		render(g, pcm, f * 160L);
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		if (f < skip)
			continue;
		++nf;
		for (int i = 0; i < 160; ++i) {
			pin += (double)pcm[i] * pcm[i];
			pout += (double)out[i] * out[i];
		}
		for (int l = 1; l <= p.L; ++l) {
			*voiced_harm += p.voiced[l];
			++*harm;
		}
		int seen[8] = { 0 };
		for (int l = 1; l <= p.L; ++l) {
			int k = (int)((l * (uint64_t)p.f0_q24) >> 20);
			if (k < 8 && !seen[k]) {
				seen[k] = 1;
				vc[k] += p.voiced[l];
			}
		}
	}
	if (vband) {
		for (int k = 0; k < 8; ++k)
			vband[k] = (uint8_t)(100 * vc[k] / (nf ? nf : 1));
	}
	return 10 * log10(pout / pin);
}

// Unvoiced bands are coded and played as DVSI codes them, as a spectral density (dstar_mbe_synth.c, "Unvoiced bands at
// DVSI's level"), and the pair returns noise this much quieter than it came in: the decoder plays DVSI's noise 6 dB
// under mbelib's scale and the analyzer codes ours 4.5 dB over it, the gain our own speech round trip measured best
#define NOISE_RT_DB	(-1.5)
// And the decoder plays everything 4.4 dB down, at an IC-9700's level (dstar_mbe_synth.c, OUT_GAIN_Q15): the level
// checks below expect it
#define OUT_DB		(-4.4)

static void test_noise(void)
{
	sig_t g = { 100, 100, 0, 0, 0, 2000, 0, 0, 100, 1 };
	int vh, h;
	double lv = level_db(&g, 6, &vh, &h, NULL);
	CHECK(vh * 100 < h * 5, "noise: %d of %d harmonics voiced", vh, h);
	CHECK(fabs(lv - OUT_DB - NOISE_RT_DB) < 1.5, "noise: level %+.2f dB through analysis and synthesis", lv);
	printf("  white noise: %.1f %% harmonics voiced, level %+.2f dB\n", 100.0 * vh / h, lv);
}

static void test_mix(void)
{
	sig_t g = { 140, 140, 4000, -6, 1900, 1000, 2100, 3900, 100, 1 };
	int vh, h;
	uint8_t vb[8];
	double lv = level_db(&g, 6, &vh, &h, vb);
	for (int k = 0; k < 8; ++k) {
		if (k < 4)
			CHECK(vb[k] >= 90, "mix: band %d voiced in only %d %% of frames", k, vb[k]);
		else
			CHECK(vb[k] <= 10, "mix: band %d voiced in %d %% of frames", k, vb[k]);
	}
	CHECK(fabs(lv - OUT_DB) < 1.5, "mix: level %+.2f dB", lv);
	printf("  mix: voiced %% a 500 Hz band %d %d %d %d | %d %d %d %d, level %+.2f dB\n", vb[0], vb[1], vb[2], vb[3],
	       vb[4], vb[5], vb[6], vb[7], lv);
}

static void test_fricative(void)
{
	// The low band only weakly periodic (E around 0.4, like the fricatives of the test speech), so the E gate does
	// not protect the upper bands: their own test must call the noise unvoiced
	sig_t g = { 75, 75, 250, 0, 950, 3000, 2000, 3900, 120, 1 };
	int vh, h;
	uint8_t vb[8];
	double lv = level_db(&g, 6, &vh, &h, vb);
	for (int k = 4; k < 8; ++k)
		CHECK(vb[k] <= 10, "fricative: band %d voiced in %d %% of frames", k, vb[k]);
	CHECK(fabs(lv - OUT_DB - NOISE_RT_DB) < 1.5, "fricative: level %+.2f dB", lv);
	printf("  fricative: voiced %% a 500 Hz band %d %d %d %d | %d %d %d %d, level %+.2f dB\n", vb[0], vb[1], vb[2],
	       vb[3], vb[4], vb[5], vb[6], vb[7], lv);
}

static void test_glide(void)
{
	sig_t g = { 100, 220, 3000, -6, 3900, 0, 0, 0, 60, 1 };
	dstar_mbe_ana_init(&ana);
	ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
	int16_t pcm[160];
	int bad = 0, n = 0;
	double worst = 0;
	for (int f = 0; f < g.frames; ++f) {
		dstar_ambe_params_t p;
		render(&g, pcm, f * 160L);
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
		if (f < 5)
			continue;
		double ft = g.f0 + (g.f0_end - g.f0) * fmin(1.0, center_of(f) / (g.frames * 160.0));
		double e = fabs(64000.0 / ana.p8 / ft - 1);
		if (e > worst)
			worst = e;
		bad += e > 0.02;
		++n;
	}
	CHECK(bad == 0, "glide: %d of %d frames more than 2 %% off", bad, n);
	printf("  glide 100->220 Hz in 1.2 s: worst pitch error %.2f %%\n", 100 * worst);
}

static void test_weak_fundamental(void)
{
	// The fundamental 20 dB down (a telephone-band voice): the error function also dips at twice the period, and the
	// tracker must not take it
	sig_t g = { 110, 110, 3000, -3, 3900, 0, 0, 0, 30, 0.1 };
	dstar_mbe_ana_init(&ana);
	ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
	int16_t pcm[160];
	int bad = 0;
	for (int f = 0; f < g.frames; ++f) {
		dstar_ambe_params_t p;
		render(&g, pcm, f * 160L);
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
		if (f >= 5)
			bad += fabs(64000.0 / ana.p8 / 110.0 - 1) > 0.02;
	}
	CHECK(bad == 0, "weak fundamental: %d frames off by more than 2 %%", bad);
}

static void test_quantizer_keeps_pitch(void)
{
	sig_t g = { 90, 300, 3000, -6, 3900, 300, 0, 0, 100, 1 };
	dstar_mbe_ana_init(&ana);
	ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
	dstar_ambe_state_t qs;
	dstar_ambe_init(&qs);
	int16_t pcm[160];
	int bad = 0;
	for (int f = 0; f < g.frames; ++f) {
		dstar_ambe_params_t p, d;
		uint16_t b[9];
		render(&g, pcm, f * 160L);
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
		dstar_ambe_quantize(&qs, &qscratch, &p, b, &d);
		// The two pitch tables round alike to 1 LSB (one float32, one double)
		bad += (d.f0_q24 > p.f0_q24 + 2 || d.f0_q24 + 2 < p.f0_q24) || d.L != p.L;
	}
	CHECK(bad == 0, "the quantizer moved the snapped pitch in %d frames", bad);
}

static void test_silence(void)
{
	dstar_mbe_ana_init(&ana);
	ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
	dstar_mbe_synth_t sy;
	dstar_mbe_synth_init(&sy);
	int16_t pcm[160] = { 0 }, out[160];
	int loud = 0, voiced = 0;
	for (int f = 0; f < 20; ++f) {
		dstar_ambe_params_t p;
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		for (int l = 1; l <= p.L; ++l)
			voiced += p.voiced[l];
		// the analyzer's floor comes out as noise about 1 LSB rms; with unvoiced bands true noise its rare peaks reach 3
		for (int i = 0; i < 160; ++i)
			loud += abs(out[i]) > 4;
	}
	CHECK(voiced == 0, "silence: %d voiced harmonics", voiced);
	CHECK(loud == 0, "silence: %d output samples above 4", loud);
}

static void test_tone_level(void)
{
	sig_t g = { 150, 150, 2000, -6, 3900, 0, 0, 0, 50, 1 };
	int vh, h;
	double lv = level_db(&g, 8, &vh, &h, NULL);
	CHECK(fabs(lv - OUT_DB) < 0.5, "tone: level %+.2f dB through analysis and synthesis", lv);
	printf("  tone 150 Hz: level %+.2f dB through analysis and synthesis\n", lv);
}

static double energy(const int16_t *x)
{
	double e = 0;
	for (int i = 0; i < 160; ++i)
		e += (double)x[i] * x[i];
	return e;
}

static void test_bad_frames(void)
{
	dstar_mbe_synth_t sy;
	dstar_mbe_synth_init(&sy);
	dstar_ambe_params_t p;
	p.f0_q24 = 262144;
	p.L = 30;
	for (int l = 1; l <= 30; ++l) {
		p.voiced[l] = 1;
		p.log2_amp[l] = (9 << 12) - l * 400;
	}
	int16_t out[160];
	double good = 0;
	for (int f = 0; f < 4; ++f) {
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		if (f >= 2)
			good += energy(out) / 2;
	}
	// Three repeats, each 1 dB quieter, still voiced; then the spectrum as unvoiced noise, still fading 1 dB a frame;
	// the 29th bad frame mutes. A frame's energy also depends on how it overlaps the one before (phases, and the voiced
	// to unvoiced cross-fade), and at this 125 Hz pitch a frame holds 2.5 periods, so one frame's energy depends on
	// where the periods fall in it. So the levels are checked over pairs of frames and more: repeats 2 and 3 against
	// the last two good frames, 2 dB down (each repeat ramps from 1 dB more to 1 dB less than the one before: 1.5 and
	// 2.5 dB), and the noise of frames 24-28 18 dB below that of frames 6-10
	double e3 = 0, e_early = 0, e_late = 0;
	for (int f = 1; f <= 29; ++f) {
		dstar_mbe_synth_bad(&sy, &synth_scratch, out);
		double e = energy(out);
		int voiced = 0;
		for (int l = 1; l <= 30; ++l)
			voiced += sy.voiced[l];
		if (f <= 3) {
			CHECK(voiced == 30, "repeat %d: %d harmonics voiced, 30 expected", f, voiced);
			if (f >= 2)
				e3 += e / 2;
		} else if (f <= 28) {
			CHECK(e > 0 && voiced == 0, "bad frame %d: unvoiced noise expected (energy %.0f, %d voiced)", f, e,
			      voiced);
			if (f >= 6 && f <= 10)
				e_early += e;
			if (f >= 24)
				e_late += e;
		} else {
			CHECK(e == 0 && !sy.have_last, "bad frame %d: mute and reset expected, energy %.0f", f, e);
		}
	}
	double rep_db = 10 * log10(good / e3), fade_db = 10 * log10(e_early / (e_late + 1e-9));
	CHECK(rep_db > 1.0 && rep_db < 3.0, "repeats 2-3 are %.1f dB below the last good frames (2 expected)", rep_db);
	CHECK(fade_db > 14 && fade_db < 22, "comfort noise faded %.1f dB over 18 frames (18 expected)", fade_db);
	printf("  concealment: repeats 2-3 %.1f dB down, comfort noise faded %.1f dB in 18 frames, silence at the 29th "
	       "bad frame\n", rep_db, fade_db);
	// A good frame after a mute starts from mbelib's reset state
	dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
	CHECK(sy.repeat == 0, "repeat count not cleared");
	// With no good frame yet there is nothing to conceal: silence
	dstar_mbe_synth_init(&sy);
	out[3] = 77;
	dstar_mbe_synth_bad(&sy, &synth_scratch, out);
	CHECK(energy(out) == 0, "a bad first frame isn't silent");
}

static void test_full_scale(void)
{
	for (int kind = 0; kind < 2; ++kind) {
		dstar_mbe_ana_init(&ana);
		ana.full_depth = 1;	// the measurement itself (test_dvsi: the DVSI match)
		dstar_mbe_synth_t sy;
		dstar_mbe_synth_init(&sy);
		double pin = 0, pout = 0;
		int bad = 0;
		for (int f = 0; f < 60; ++f) {
			int16_t pcm[160], out[160];
			for (int i = 0; i < 160; ++i) {
				long n = f * 160L + i;
				pcm[i] = kind == 0 ? ((n / 25) & 1 ? 32767 : -32768)			// a 160 Hz square
						   : (int16_t)(mfrand() < 0.5 ? -32768 : 32767);	// full-scale noise
			}
			dstar_ambe_params_t p;
			dstar_mbe_analyze(&ana, &ana_scratch, pcm, &p);
			for (int l = 1; l <= p.L; ++l)
				bad += p.log2_amp[l] < -20 * 4096 || p.log2_amp[l] > 16 * 4096;
			bad += p.L < 9 || p.L > 56;
			dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
			if (f < 8)
				continue;
			for (int i = 0; i < 160; ++i) {
				pin += (double)pcm[i] * pcm[i];
				pout += (double)out[i] * out[i];
			}
		}
		double lv = 10 * log10(pout / pin);
		CHECK(bad == 0, "full scale %s: %d out-of-range parameters", kind ? "noise" : "square", bad);
		// The square wave's harmonics above 3.7 kHz are not coded and the output clips at +-32760: a coarse check. Noise
		// comes back NOISE_RT_DB quieter by design, and everything OUT_DB
		double lo = (kind ? -4 + NOISE_RT_DB : -4) + OUT_DB;
		CHECK(lv > lo && lv < 1 + OUT_DB, "full scale %s: level %+.2f dB", kind ? "noise" : "square", lv);
		printf("  full-scale %s: level %+.2f dB\n", kind ? "noise" : "square", lv);
	}
}

// --- frame FEC ---

static void random_fields(uint16_t b[9])
{
	static const uint16_t widths[9] = { 7, 4, 6, 9, 7, 4, 4, 4, 4 };
	for (int i = 0; i < 9; i++)
		b[i] = (uint16_t)(qrnd() & ((1u << widths[i]) - 1u));
	b[8] &= 14u;
}

static void flip(uint8_t *f, unsigned i)
{
	f[i >> 3] ^= (uint8_t)(1u << (i & 7u));
}

// Which code word each air bit belongs to, found by experiment: flip it and see which of C0 / C1 reports an error, or
// which field changes
static int word_of_air_bit[72];

static void test_frame_round_trip(void)
{
	for (int n = 0; n < 20000; n++) {
		uint16_t b[9], back[9];
		uint8_t f[DSTAR_VOICE_BYTES];
		dstar_ambe_fec_t fec;
		random_fields(b);
		dstar_ambe_pack(b, f);
		dstar_ambe_unpack(f, back, &fec);
		CHECK(memcmp(b, back, sizeof(b)) == 0, "fields changed through a clean frame");
		CHECK(!fec.c0_failed && !fec.c1_failed && fec.c0_errors == 0 && fec.c1_errors == 0, "errors on a clean frame");
	}
	// Each air bit: a flip in C0 or C1 is corrected and counted there, a flip elsewhere changes exactly one field bit
	uint16_t b[9] = { 77, 9, 33, 300, 100, 5, 10, 15, 6 }, back[9];
	uint8_t f[DSTAR_VOICE_BYTES];
	int n0 = 0, n1 = 0, nu = 0;
	dstar_ambe_pack(b, f);
	for (unsigned i = 0; i < DSTAR_VOICE_BITS; i++) {
		uint8_t g[DSTAR_VOICE_BYTES];
		dstar_ambe_fec_t fec;
		memcpy(g, f, sizeof(g));
		flip(g, i);
		dstar_ambe_unpack(g, back, &fec);
		if (fec.c0_errors == 1 && fec.c1_errors == 0) {
			word_of_air_bit[i] = 0;
			n0++;
			CHECK(memcmp(b, back, sizeof(b)) == 0, "air bit %u: C0 error not corrected", i);
		} else if (fec.c1_errors == 1 && fec.c0_errors == 0) {
			word_of_air_bit[i] = 1;
			n1++;
			CHECK(memcmp(b, back, sizeof(b)) == 0, "air bit %u: C1 error not corrected", i);
		} else {
			word_of_air_bit[i] = 2;
			nu++;
			int changed = 0;
			for (int k = 0; k < 9; k++)
				changed += __builtin_popcount(b[k] ^ back[k]);
			CHECK(changed == 1 && !fec.c0_failed && !fec.c1_failed, "air bit %u: %d field bits changed", i, changed);
		}
	}
	CHECK(n0 == 24 && n1 == 24 && nu == 24, "C0 %d, C1 %d, unprotected %d air bits (24 each)", n0, n1, nu);
}

static void test_frame_errors(void)
{
	int c0[24], c1[24], k0 = 0, k1 = 0;
	for (unsigned i = 0; i < DSTAR_VOICE_BITS; i++) {
		if (word_of_air_bit[i] == 0)
			c0[k0++] = (int)i;
		else if (word_of_air_bit[i] == 1)
			c1[k1++] = (int)i;
	}
	if (k0 != 24 || k1 != 24)
		return;
	int bad3 = 0, undetected4 = 0, wrong = 0;
	for (int n = 0; n < 4000; n++) {
		uint16_t b[9], back[9];
		uint8_t f[DSTAR_VOICE_BYTES];
		dstar_ambe_fec_t fec;
		random_fields(b);
		dstar_ambe_pack(b, f);
		// Up to three errors in each of C0 and C1 together: all corrected, counted in the right word
		int e0 = (int)(qrnd() % 4), e1 = (int)(qrnd() % 4), used[72] = { 0 };
		for (int k = 0; k < e0;) {
			int i = c0[qrnd() % 24];
			if (!used[i]) {
				used[i] = 1;
				flip(f, (unsigned)i);
				k++;
			}
		}
		for (int k = 0; k < e1;) {
			int i = c1[qrnd() % 24];
			if (!used[i]) {
				used[i] = 1;
				flip(f, (unsigned)i);
				k++;
			}
		}
		dstar_ambe_unpack(f, back, &fec);
		bad3 += memcmp(b, back, sizeof(b)) != 0;
		wrong += fec.c0_errors != e0 || fec.c1_errors != e1 || fec.c0_failed || fec.c1_failed;
		// Four in C0: detected
		dstar_ambe_pack(b, f);
		memset(used, 0, sizeof(used));
		for (int k = 0; k < 4;) {
			int i = c0[qrnd() % 24];
			if (!used[i]) {
				used[i] = 1;
				flip(f, (unsigned)i);
				k++;
			}
		}
		dstar_ambe_unpack(f, back, &fec);
		undetected4 += !fec.c0_failed;
	}
	CHECK(bad3 == 0, "%d frames with up to 3 errors a word not corrected", bad3);
	CHECK(wrong == 0, "%d frames with the errors counted wrong", wrong);
	CHECK(undetected4 == 0, "%d frames with 4 errors in C0 not detected", undetected4);

	// Four errors in C1: detected, and the data is the 23-bit word's decode (the codec plays such a frame). That is
	// right exactly when C1's parity bit was one of the four, so exactly one of C1's air bits, flipped with any three
	// others, leaves the fields intact: the parity bit
	int parity_like = 0;
	for (int p = 0; p < 24; p++) {
		int intact = 0, detected = 0;
		for (int n = 0; n < 50; n++) {
			uint16_t b[9], back[9];
			uint8_t f[DSTAR_VOICE_BYTES];
			dstar_ambe_fec_t fec;
			int used[24] = { 0 };
			random_fields(b);
			dstar_ambe_pack(b, f);
			used[p] = 1;
			flip(f, (unsigned)c1[p]);
			for (int k = 0; k < 3;) {
				int j = (int)(qrnd() % 24);
				if (!used[j]) {
					used[j] = 1;
					flip(f, (unsigned)c1[j]);
					k++;
				}
			}
			dstar_ambe_unpack(f, back, &fec);
			detected += fec.c1_failed && !fec.c0_failed;
			intact += memcmp(b, back, sizeof(b)) == 0;
		}
		CHECK(detected == 50, "C1 air bit %d with three others: %d of 50 detected", p, detected);
		parity_like += intact == 50;
	}
	CHECK(parity_like == 1, "%d of C1's air bits behave as its parity bit (1 expected)", parity_like);
}

// The proof of the bit order: the standard's lost-packet frame is a valid AMBE frame, so read in the right order its
// two Golay words decode with no corrections. Read with each byte's bits the other way round (most significant first,
// as the codec's research code packed them) the same bytes give errors
static void test_bit_order(void)
{
	uint16_t b[9];
	dstar_ambe_fec_t fec;
	dstar_ambe_unpack(dstar_voice_silence, b, &fec);
	CHECK(!fec.c0_failed && !fec.c1_failed && fec.c0_errors == 0 && fec.c1_errors == 0,
	      "lost-packet frame, least significant bit first: C0 %u%s, C1 %u%s", fec.c0_errors,
	      fec.c0_failed ? " failed" : "", fec.c1_errors, fec.c1_failed ? " failed" : "");
	printf("  lost-packet frame (LSB first): C0 %u, C1 %u errors; fields b0..b8 = %u %u %u %u %u %u %u %u %u\n",
	       fec.c0_errors, fec.c1_errors, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);

	uint8_t rev[DSTAR_VOICE_BYTES];
	for (unsigned i = 0; i < DSTAR_VOICE_BYTES; i++) {
		uint8_t x = dstar_voice_silence[i], y = 0;
		for (int k = 0; k < 8; k++)
			y |= (uint8_t)(((x >> k) & 1u) << (7 - k));
		rev[i] = y;
	}
	uint16_t b2[9];
	dstar_ambe_fec_t fec2;
	dstar_ambe_unpack(rev, b2, &fec2);
	CHECK(fec2.c0_failed || fec2.c1_failed || fec2.c0_errors + fec2.c1_errors >= 3,
	      "the other bit order also decodes cleanly (C0 %u, C1 %u)", fec2.c0_errors, fec2.c1_errors);
	printf("  the same bytes MSB first: C0 %u%s, C1 %u%s\n", fec2.c0_errors, fec2.c0_failed ? " (failed)" : "",
	       fec2.c1_errors, fec2.c1_failed ? " (failed)" : "");

	// Packing the decoded fields gives the frame back bit for bit, so the parity bit's whitening matches the frame too;
	// it can't tell whether that bit is whitened, though: the 24th step of this frame's sequence is 0
	uint8_t again[DSTAR_VOICE_BYTES];
	dstar_ambe_pack(b, again);
	CHECK(memcmp(again, dstar_voice_silence, DSTAR_VOICE_BYTES) == 0, "the lost-packet frame doesn't pack back");
	uint16_t u0 = 0;
	for (int i = 0; i < 6; i++)
		u0 = (uint16_t)((u0 << 1) | ((b[0] >> (6 - i)) & 1u));
	for (int i = 0; i < 4; i++)
		u0 = (uint16_t)((u0 << 1) | ((b[2] >> (5 - i)) & 1u));
	u0 = (uint16_t)((u0 << 2) | ((b[3] >> 7) & 3u));
	printf("  the 24th whitening step for its C0 data 0x%03X is %u\n", u0, (unsigned)(ambeWhitening(u0, 24) & 1u));

	// What it decodes to: the gain, through the dequantizer and the synthesizer from the start state
	dstar_ambe_state_t s;
	dstar_ambe_params_t p;
	dstar_ambe_init(&s);
	int r = dstar_ambe_dequantize(&s, &qscratch, b, &p);
	CHECK(r == DSTAR_AMBE_VOICE, "lost-packet frame decoded as a tone");
	double mean = 0;
	for (int l = 1; l <= p.L; l++)
		mean += p.log2_amp[l] / 4096.0;
	mean /= p.L ? p.L : 1;
	dstar_mbe_synth_t sy;
	dstar_mbe_synth_init(&sy);
	int16_t out[160];
	double peak = 0;
	for (int f = 0; f < 10; f++) {
		dstar_ambe_init(&s);
		dstar_ambe_dequantize(&s, &qscratch, b, &p);
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		for (int i = 0; i < 160; i++)
			peak = fmax(peak, fabs((double)out[i]));
	}
	printf("  it dequantizes to f0 %.1f Hz, L %u, gamma %.2f, mean log2 magnitude %.2f; synthesized peak %.0f\n",
	       p.f0_q24 * 8000.0 / 16777216.0, p.L, s.gamma_q16 / 65536.0, mean, peak);
	CHECK(peak < 100, "the lost-packet frame plays at peak %.0f (near silence expected)", peak);
}

// --- the codec ---

static dstar_codec_t codec;
static dstar_codec_scratch_t scratch;

static void test_codec_interface(void)
{
	dstar_vocoder_t v;
	int16_t pcm[160] = { 0 };
	uint8_t frame[DSTAR_VOICE_BYTES];
	dstar_codec_vocoder(&codec, &v);
	CHECK(v.ctx == &codec && v.encode == dstar_codec_encode && v.decode == dstar_codec_decode, "interface");
	memset(&codec, 0, sizeof(codec));
	CHECK(!v.encode(v.ctx, pcm, frame) && !v.decode(v.ctx, frame, pcm), "a codec never started works");
	// Without working memory (a radio that couldn't borrow it) a start leaves the codec stopped, never coding into NULL
	dstar_codec_start(&codec, true);
	CHECK(!v.encode(v.ctx, pcm, frame), "encode with no working memory bound");
	dstar_codec_bind(&codec, &scratch);
	dstar_codec_start(&codec, true);
	CHECK(v.encode(v.ctx, pcm, frame), "encode after start(encode)");
	CHECK(!v.decode(v.ctx, frame, pcm), "decode while encoding");
	dstar_codec_start(&codec, false);
	CHECK(!v.encode(v.ctx, pcm, frame), "encode while decoding");
	CHECK(v.decode(v.ctx, frame, pcm), "decode after start(decode)");
	// Unbinding stops it: the radio takes the working memory back when it leaves the channel
	dstar_codec_bind(&codec, NULL);
	CHECK(!v.decode(v.ctx, frame, pcm), "decode after the working memory was taken back");
	dstar_codec_bind(&codec, &scratch);
	dstar_codec_start(&codec, false);
	printf("  sizeof(dstar_codec_t) %zu B on the host (encoder %zu, decoder %zu), working memory %zu\n",
	       sizeof(dstar_codec_t), sizeof(dstar_codec_enc_t), sizeof(dstar_codec_dec_t), sizeof(dstar_codec_scratch_t));
}

// A 150 Hz vowel-like tone through the encoder, then the frames through the decoder
static void encode_tone(uint8_t frames[][DSTAR_VOICE_BYTES], int n)
{
	sig_t g = { 150, 150, 3000, -6, 3900, 0, 0, 0, n, 1 };
	int16_t pcm[160];
	dstar_codec_start(&codec, true);
	for (int f = 0; f < n; f++) {
		render(&g, pcm, f * 160L);
		dstar_codec_encode(&codec, pcm, frames[f]);
	}
}

// n distinct air bits of word w (word_of_air_bit: 0 C0, 1 C1) flipped in f, starting at the k-th of them
static void flip_word(uint8_t *f, int w, int n, int k)
{
	int bits[24], m = 0;
	for (unsigned i = 0; i < DSTAR_VOICE_BITS && m < 24; i++) {
		if (word_of_air_bit[i] == w)
			bits[m++] = (int)i;
	}
	for (int j = 0; j < n; j++)
		flip(f, (unsigned)bits[(k + 5 * j) % 24]);
}

static void test_codec_bad_frames(void)
{
	uint8_t frames[80][DSTAR_VOICE_BYTES], g[DSTAR_VOICE_BYTES];
	int16_t pcm[160];
	encode_tone(frames, 80);
	dstar_codec_start(&codec, false);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED, "errors before any frame");
	for (int f = 0; f < 20; f++)
		dstar_codec_decode(&codec, frames[f], pcm);
	CHECK(dstar_codec_errors(&codec) == 0, "clean frame: errors %u", dstar_codec_errors(&codec));
	CHECK(energy(pcm) > 1e6, "the tone isn't playing");
	double level = energy(pcm);

	// C0 failed: concealed, the prediction memory held. Three fading repeats, then comfort noise well below them, and
	// silence from the 29th bad frame on
	dstar_codec_t before = codec;
	for (int f = 20; f < 50; f++) {
		memcpy(g, frames[f], sizeof(g));
		flip_word(g, 0, 4, f);
		dstar_codec_decode(&codec, g, pcm);
		int n = f - 19;
		CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED, "C0 failure: errors %u", dstar_codec_errors(&codec));
		int voiced = 0;
		for (int l = 1; l <= codec.u.dec.synth.L; ++l)
			voiced += codec.u.dec.synth.voiced[l];
		if (n <= 3)
			CHECK(energy(pcm) > level / 4 && voiced > 0, "bad frame %d: a repeat expected", n);
		else if (n <= 28)
			CHECK(energy(pcm) > 0 && voiced == 0, "bad frame %d: comfort noise expected", n);
		else
			CHECK(energy(pcm) == 0, "bad frame %d: silence expected", n);
		if (n == 28)
			CHECK(energy(pcm) < level / 30, "bad frame 28: comfort noise only %.1f dB down",
			      10 * log10(level / energy(pcm)));
		if (n == 1)
			CHECK(memcmp(&before.u.dec.q, &codec.u.dec.q, sizeof(codec.u.dec.q)) == 0,
			      "a concealed frame moved the prediction memory");
	}
	// Good frames again: playing again
	for (int f = 50; f < 56; f++)
		dstar_codec_decode(&codec, frames[f], pcm);
	CHECK(energy(pcm) > 1e6 && dstar_codec_errors(&codec) == 0, "no recovery after a mute");

	// Four corrections (two in each word) and C1 failed are both decoded, not concealed, when the frame before was
	// good: mbelib conceals the first, and the research measured that costing quality
	memcpy(g, frames[56], sizeof(g));
	flip_word(g, 0, 2, 1);
	flip_word(g, 1, 2, 2);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == 4 && codec.u.dec.synth.repeat == 0, "four corrected errors after a good "
	      "frame: errors %u, repeat %u", dstar_codec_errors(&codec), codec.u.dec.synth.repeat);
	memcpy(g, frames[57], sizeof(g));
	flip_word(g, 1, 4, 3);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == 4 && codec.u.dec.synth.repeat == 0, "C1 failed after a good frame: "
	      "errors %u, repeat %u", dstar_codec_errors(&codec), codec.u.dec.synth.repeat);
	CHECK(energy(pcm) > 1e6, "C1 failed: the frame isn't played");

	// After a concealed frame a doubtful one is concealed too: four corrections, or C0 clean and C1 failed with one
	// in C0 (3 + 1); three corrections are decoded
	memcpy(g, frames[58], sizeof(g));
	flip_word(g, 0, 4, 0);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED, "C0 failure");
	memcpy(g, frames[59], sizeof(g));
	flip_word(g, 0, 2, 4);
	flip_word(g, 1, 2, 6);
	before = codec;
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED && codec.u.dec.synth.repeat == 2,
	      "four corrections after a loss: errors %u, repeat %u", dstar_codec_errors(&codec), codec.u.dec.synth.repeat);
	CHECK(memcmp(&before.u.dec.q, &codec.u.dec.q, sizeof(codec.u.dec.q)) == 0, "a repeat moved the prediction memory");
	memcpy(g, frames[60], sizeof(g));
	flip_word(g, 0, 1, 7);
	flip_word(g, 1, 4, 8);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED, "C1 failed and one in C0 after a loss: errors %u",
	      dstar_codec_errors(&codec));
	memcpy(g, frames[61], sizeof(g));
	flip_word(g, 0, 1, 9);
	flip_word(g, 1, 2, 10);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == 3 && codec.u.dec.synth.repeat == 0, "three corrections after a loss: "
	      "errors %u, repeat %u", dstar_codec_errors(&codec), codec.u.dec.synth.repeat);
	// And once a frame was decoded the rule forgets the loss: four corrections are decoded again
	memcpy(g, frames[62], sizeof(g));
	flip_word(g, 0, 2, 11);
	flip_word(g, 1, 2, 12);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == 4, "four corrections after a decoded frame: errors %u",
	      dstar_codec_errors(&codec));

	// Lost frames: the same concealment, and they count as a loss for the next frame
	dstar_codec_lost(&codec, pcm);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED && energy(pcm) > 1e6, "lost frame: a repeat expected");
	memcpy(g, frames[64], sizeof(g));
	flip_word(g, 0, 2, 13);
	flip_word(g, 1, 2, 14);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED, "four corrections after a lost frame: errors %u",
	      dstar_codec_errors(&codec));
	for (int f = 0; f < 30; f++)
		dstar_codec_lost(&codec, pcm);
	CHECK(energy(pcm) == 0, "a long run of lost frames isn't silent");
	// Zeroed first: memcmp reads the padding after L too, which dstar_ambe_init leaves as it finds it and which
	// dstar_codec_start zeroed in the codec (an uninitialized stack copy passed or failed with the stack's history)
	dstar_ambe_state_t fresh;
	memset(&fresh, 0, sizeof(fresh));
	dstar_ambe_init(&fresh);
	CHECK(memcmp(&fresh, &codec.u.dec.q, sizeof(fresh)) == 0, "the mute didn't reset the prediction memory");

	// A tone frame (b0 = 126) plays silence
	uint16_t b[9] = { 126, 0, 0, 0, 0, 0, 0, 0, 0 };
	for (int f = 65; f < 70; f++)
		dstar_codec_decode(&codec, frames[f], pcm);
	dstar_ambe_pack(b, g);
	dstar_codec_decode(&codec, g, pcm);
	CHECK(energy(pcm) == 0, "a tone frame isn't silent");
	// Not decoding: lost gives silence
	dstar_codec_start(&codec, true);
	pcm[5] = 1234;
	dstar_codec_lost(&codec, pcm);
	CHECK(energy(pcm) == 0, "lost while encoding isn't silence");
}


// The one frame here a real D-STAR encoder is defined to make: the standard's lost-packet frame (JARL STD 6.0 6.6,
// dstar_voice_silence), coded by the standard, not by us. It pins what self-consistent round trips cannot: C1's
// parity bit sits where mbelib reads its d24 and is whitened by the sequence's 24th step. Off-air frames of DVSI
// encoders confirmed the same (docs/spec/dstar-ambe-frame.md 5); they are kept out of the tree, which carries
// synthetic and standard vectors only
static const uint8_t *const std_frames[] = { dstar_voice_silence };

static void test_dvsi_frames(void)
{
	for (unsigned t = 0; t < sizeof(std_frames) / sizeof(std_frames[0]); t++) {
		const uint8_t *f = std_frames[t];
		uint16_t b[9];
		uint8_t g[DSTAR_VOICE_BYTES];
		dstar_ambe_fec_t fec;
		dstar_ambe_unpack(f, b, &fec);
		CHECK(!fec.c0_failed && !fec.c1_failed && fec.c0_errors == 0 && fec.c1_errors == 0,
		      "standard frame %u: C0 %u%s, C1 %u%s", t, fec.c0_errors, fec.c0_failed ? " failed" : "", fec.c1_errors,
		      fec.c1_failed ? " failed" : "");
		dstar_ambe_pack(b, g);
		CHECK(!memcmp(f, g, DSTAR_VOICE_BYTES), "standard frame %u does not re-pack bit for bit", t);
		// Four errors in C1: Golay(23,12) alone would "correct" three onto a wrong word; its parity bit catches it
		memcpy(g, f, DSTAR_VOICE_BYTES);
		int flipped = 0;
		for (unsigned i = 0; i < DSTAR_VOICE_BITS && flipped < 4; i++) {
			uint8_t h[DSTAR_VOICE_BYTES];
			memcpy(h, f, DSTAR_VOICE_BYTES);
			h[i >> 3] ^= (uint8_t)(1u << (i & 7));
			dstar_ambe_fec_t e1;
			dstar_ambe_unpack(h, b, &e1);
			if (e1.c0_errors == 0 && !e1.c0_failed && e1.c1_errors == 1) {
				g[i >> 3] ^= (uint8_t)(1u << (i & 7));
				flipped++;
			}
		}
		dstar_ambe_unpack(g, b, &fec);
		CHECK(flipped == 4 && fec.c1_failed, "standard frame %u with four C1 errors: not detected", t);
	}
	printf("  the standard's frame: no corrections, re-packed bit for bit, four C1 errors detected\n");
}

// The frame layout as docs/spec/dstar-ambe-frame.md writes it out, restated here independently of the code: each word's
// air bits from its bit 23 down (spec 4), the Golay parity as polynomial division (spec 3) and the whitening sequence
// (spec 3). The code holds the interleave as a formula and calls the shared DMR Golay coder, so agreement is a check
// of both against the spec's own numbers
static const uint8_t spec_air[3][24] = {
	{ 7, 1, 11, 21, 31, 25, 35, 45, 55, 49, 59, 69, 6, 0, 10, 20, 30, 24, 34, 44, 54, 48, 58, 68 },
	{ 5, 15, 9, 19, 29, 39, 33, 43, 53, 63, 57, 67, 4, 14, 8, 18, 28, 38, 32, 42, 52, 62, 56, 66 },
	{ 3, 13, 23, 17, 27, 37, 47, 41, 51, 61, 71, 65, 2, 12, 22, 16, 26, 36, 46, 40, 50, 60, 70, 64 },
};

static uint32_t spec_golay24(uint32_t data)
{
	uint32_t r = data << 11;
	for (int bit = 22; bit >= 11; bit--)
		if (r & (1u << bit))
			r ^= 0xC75u << (bit - 11);
	uint32_t w23 = data << 11 | r;
	return w23 << 1 | (uint32_t)(__builtin_popcount(w23) & 1);
}

static uint32_t spec_whitening(uint32_t u0)
{
	uint32_t x = 16u * u0, w = 0;
	for (int s = 1; s <= 24; s++) {
		x = (173u * x + 13849u) & 0xFFFFu;
		w |= (x >> 15) << (24 - s);
	}
	return w;
}

static void spec_frame(const uint32_t w[3], uint8_t f[DSTAR_VOICE_BYTES])
{
	memset(f, 0, DSTAR_VOICE_BYTES);
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 24; k++)
			if (w[i] >> k & 1u)
				flip(f, spec_air[i][23 - k]);
}

static uint32_t spec_word(const uint8_t f[DSTAR_VOICE_BYTES], int i)
{
	uint32_t w = 0;
	for (int k = 0; k < 24; k++)
		w |= (uint32_t)(f[spec_air[i][23 - k] >> 3] >> (spec_air[i][23 - k] & 7) & 1u) << k;
	return w;
}

// Fields -> the three words by the spec's table of 2 (d0..d48 per field), one d bit at a time
static void spec_words(const uint16_t b[9], uint32_t w[3])
{
	// For each field, its d bits from the most significant; d24 is W1's parity and never a field bit
	static const uint8_t dmap[9][9] = {
		{ 0, 1, 2, 3, 4, 5, 48 }, { 38, 39, 40, 41 }, { 6, 7, 8, 9, 42, 43 },
		{ 10, 11, 12, 13, 14, 15, 16, 44, 45 }, { 17, 18, 19, 20, 21, 46, 47 }, { 22, 23, 25, 26 },
		{ 27, 28, 29, 30 }, { 31, 32, 33, 34 }, { 35, 36, 37, 99 },
	};
	static const uint8_t widths[9] = { 7, 4, 6, 9, 7, 4, 4, 4, 4 };
	uint8_t d[49] = { 0 };
	for (int i = 0; i < 9; i++)
		for (int j = 0; j < widths[i]; j++)
			if (dmap[i][j] < 49)
				d[dmap[i][j]] = (uint8_t)(b[i] >> (widths[i] - 1 - j) & 1u);
	uint32_t u0 = 0, u1 = 0, u2 = 0;
	for (int j = 0; j < 12; j++) {
		u0 = u0 << 1 | d[j];
		u1 = u1 << 1 | d[12 + j];
	}
	for (int j = 25; j < 49; j++)
		u2 = u2 << 1 | d[j];
	w[0] = spec_golay24(u0);
	w[1] = spec_golay24(u1) ^ spec_whitening(u0);
	w[2] = u2;
}

// pack against the spec's restatement, on random fields and on the standard's frame, both ways
static void test_frame_spec(void)
{
	int bad = 0;
	for (int n = 0; n < 20000; n++) {
		uint16_t b[9];
		uint32_t w[3];
		uint8_t f[DSTAR_VOICE_BYTES], g[DSTAR_VOICE_BYTES];
		random_fields(b);
		spec_words(b, w);
		spec_frame(w, f);
		dstar_ambe_pack(b, g);
		bad += memcmp(f, g, DSTAR_VOICE_BYTES) != 0;
	}
	CHECK(bad == 0, "%d random frames pack otherwise than the spec's table", bad);
	const uint8_t *real[1] = { dstar_voice_silence };
	for (int t = 0; t < 1; t++) {
		uint16_t b[9];
		uint32_t w[3];
		uint8_t f[DSTAR_VOICE_BYTES];
		dstar_ambe_unpack(real[t], b, NULL);
		spec_words(b, w);
		spec_frame(w, f);
		CHECK(memcmp(f, real[t], DSTAR_VOICE_BYTES) == 0, "standard frame %d: the spec's words don't rebuild it", t);
		CHECK(spec_word(real[t], 0) == spec_golay24(spec_word(real[t], 0) >> 12), "standard frame %d: W0 by the table "
		      "is not a Golay word", t);
	}
	printf("  frame: 20000 random frames as the spec's table and restated Golay and whitening give them; the "
	       "standard's frame rebuilt\n");
}

// Every 1-, 2- and 3-bit error pattern in W0 and in W1 corrected and counted, every 4-bit one detected. A few base
// frames, since the decoder sees only the error pattern once the code word is removed, except for W1, where W0's data
// picks the whitening
static void test_frame_golay_patterns(void)
{
	int miss[5] = { 0 }, n[5] = { 0 };
	for (int base = 0; base < 3; base++) {
		uint16_t b[9];
		uint8_t f[DSTAR_VOICE_BYTES];
		random_fields(b);
		dstar_ambe_pack(b, f);
		for (int word = 0; word < 2; word++) {
			for (uint32_t m = 1; m < (1u << 24); m++) {
				int e = __builtin_popcount(m);
				if (e > 4)
					continue;
				uint8_t g[DSTAR_VOICE_BYTES];
				memcpy(g, f, sizeof(g));
				for (int i = 0; i < 24; i++)
					if (m >> i & 1u)
						flip(g, spec_air[word][i]);
				uint16_t back[9];
				dstar_ambe_fec_t fec;
				dstar_ambe_unpack(g, back, &fec);
				n[e]++;
				bool failed = word ? fec.c1_failed : fec.c0_failed;
				unsigned got = word ? fec.c1_errors : fec.c0_errors;
				unsigned other = word ? fec.c0_errors : fec.c1_errors;
				if (e == 4)
					miss[e] += !failed;
				else
					miss[e] += failed || got != (unsigned)e || other != 0 || memcmp(b, back, sizeof(b)) != 0;
			}
		}
	}
	CHECK(n[1] == 3 * 2 * 24 && n[2] == 3 * 2 * 276 && n[3] == 3 * 2 * 2024 && n[4] == 3 * 2 * 10626,
	      "pattern counts %d %d %d %d", n[1], n[2], n[3], n[4]);
	CHECK(miss[1] + miss[2] + miss[3] == 0, "patterns not corrected or miscounted: %d of 1, %d of 2, %d of 3 bits",
	      miss[1], miss[2], miss[3]);
	CHECK(miss[4] == 0, "%d four-bit patterns not detected", miss[4]);
	printf("  Golay: all %d one- to three-bit patterns in W0 and W1 corrected and counted, all %d four-bit ones "
	       "detected\n", n[1] + n[2] + n[3], n[4]);
}

// W1 as sent is its Golay word XOR the whitening, and Golay of zero data is zero: with W1's data 0 the air carries
// the sequence itself. For every one of the 4096 seeds, against the restatement; flipping d0 inverts all 24 steps
static void test_frame_whitening(void)
{
	int bad = 0, inv = 0;
	for (uint32_t u0 = 0; u0 < 4096; u0++) {
		uint16_t b[9] = { (uint16_t)((u0 >> 6) << 1), 0, (uint16_t)(((u0 >> 2) & 15u) << 2),
				  (uint16_t)((u0 & 3u) << 7), 0, 0, 0, 0, 0 };
		uint8_t f[DSTAR_VOICE_BYTES];
		dstar_ambe_pack(b, f);
		uint32_t seq = spec_word(f, 1);
		bad += seq != spec_whitening(u0);
		inv += (seq ^ spec_whitening(u0 ^ 0x800u)) != 0xFFFFFFu;
	}
	CHECK(bad == 0, "%d seeds whiten otherwise than the spec's sequence", bad);
	CHECK(inv == 0, "%d seeds where flipping d0 doesn't invert the sequence", inv);
	printf("  whitening: all 4096 seeds give the spec's 24-step sequence, 24th on W1's parity bit\n");
}

// The DVSI match (dstar_mbe_analysis.c), on by default: a harmonic signal's log magnitudes come out as 0.6 times their
// spread about the mean as measured, plus DVSI's balance for the harmonic's 500 Hz band (on since 21 Nov: -5.2 +0.3
// +1.8 +2.1 +1.4 +1.6 -1.7 -4.9 dB, the 9700's frames against ours on the same audio), plus one shift for the whole
// frame, at the same power; white noise (all unvoiced) is sent at b0 34; a 60 Hz buzz is not coded above b0 119.
// Before the balance this checked the spread ratio alone (0.6); with it the ratio is no longer 0.6 (0.673 on this
// signal), so the check is the whole model's residual, as the research codec's test_dvsi has it
static void dvsi_params(int full_depth, double f0, int noise, dstar_ambe_params_t *last)
{
	dstar_mbe_ana_init(&ana);
	ana.full_depth = (uint8_t)full_depth;
	for (int f = 0; f < 30; ++f) {
		int16_t pcm[160];
		for (int i = 0; i < 160; ++i) {
			double t = (f * 160.0 + i) / 8000, x = 0;
			if (noise)
				x = 6000.0 * ((double)qrnd() / 4294967296.0 - 0.5);
			else
				for (int k = 1; k * f0 < 3800; ++k)
					x += 6000.0 / k * cos(2 * M_PI * k * f0 * t + k * k);
			pcm[i] = (int16_t)x;
		}
		dstar_mbe_analyze(&ana, &ana_scratch, pcm, last);
	}
}

static void test_dvsi(void)
{
	static const double eq_db[8] = { -5.2, 0.3, 1.8, 2.1, 1.4, 1.6, -1.7, -4.9 };
	dstar_ambe_params_t a, b;
	dvsi_params(1, 125, 0, &a);
	dvsi_params(0, 125, 0, &b);
	double ma = 0, pa = 0, pb = 0, r[DSTAR_AMBE_MAX_L + 1], rm = 0, rs = 0, lo = 0, hi = 0;
	int nlo = 0, nhi = 0;
	for (int l = 1; l <= a.L; ++l)
		ma += a.log2_amp[l] / 4096.0 / a.L;
	for (int l = 1; l <= a.L; ++l) {
		double x = a.log2_amp[l] / 4096.0, y = b.log2_amp[l] / 4096.0;
		int k = (int)((l * a.f0_q24) >> 20);
		k = k > 7 ? 7 : k;
		r[l] = y - (0.6 * (x - ma) + eq_db[k] / 6.0206);
		rm += r[l] / a.L;
		pa += pow(2, 2 * x);
		pb += pow(2, 2 * y);
		// the balance's own effect, by band: matched minus 0.6 x measured, below 500 Hz and at 1-3 kHz
		if (k == 0)
			lo += 6.0206 * (y - 0.6 * (x - ma)), nlo++;
		else if (k >= 2 && k <= 5)
			hi += 6.0206 * (y - 0.6 * (x - ma)), nhi++;
	}
	for (int l = 1; l <= a.L; ++l)
		rs += (r[l] - rm) * (r[l] - rm) / a.L;
	double res = sqrt(rs), dp = 10 * log10(pb / pa), tilt = lo / nlo - hi / nhi;
	CHECK(a.L == b.L && res < 0.01 && fabs(dp) < 0.3, "DVSI match: L %d/%d, residual %.4f log2, power %+.2f dB", a.L,
	      b.L, res, dp);
	// at least the 5 dB cut below 500 Hz against the speech band the measurement asked for
	CHECK(tilt < -6.0, "DVSI balance: below 500 Hz only %+.2f dB against 1-3 kHz", tilt);
	printf("  DVSI match: 0.6 x measured + band balance within %.4f log2, power %+.2f dB; below 500 Hz %+.1f dB "
	       "against 1-3 kHz\n", res, dp, tilt);
	dvsi_params(0, 0, 1, &b);
	CHECK(b.f0_q24 == dstar_mbe_f0_q24[34], "DVSI: white noise at f0 %u, not b0 34's", b.f0_q24);
	dvsi_params(0, 60, 0, &b);
	CHECK(b.f0_q24 >= dstar_mbe_f0_q24[119], "DVSI: a 60 Hz buzz coded below b0 119's pitch");
}

// The synthesizer's changes of 18 Nov 2026 (dstar_mbe_synth.c), by behavior: a harmonic whose pitch alternates by 4 %
// a frame keeps a steady envelope (interpolated, not cross-faded); all-unvoiced frames give noise with a flat spectrum,
// not lines; a frame too loud for 16 bits is limited without flat-topped clipping
static void test_synth_changes(void)
{
	dstar_mbe_synth_t sy;
	dstar_ambe_params_t p;
	static int16_t out[200 * 160];
	dstar_mbe_synth_init(&sy);
	p.L = 30;
	// The loud harmonic is the 8th at 960-998 Hz, under the voiced noise floor's 1 kHz (test_voiced_floor), so the
	// envelope measured is the interpolation's alone; about two cycles a 16-sample block, so a block's energy doesn't
	// ripple with the phase
	for (int l = 1; l <= 30; ++l) {
		p.voiced[l] = 1;
		p.log2_amp[l] = l == 8 ? 10 << 12 : 0;
	}
	for (int f = 0; f < 50; ++f) {
		p.f0_q24 = f & 1 ? 261725 : 251658;	// 124.8 and 120 Hz
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out + 160 * f);
	}
	double lo = 1e30, hi = 0;
	for (int b = 100; b < 500; ++b) {
		double e = 0;
		for (int i = 0; i < 16; ++i)
			e += (double)out[16 * b + i] * out[16 * b + i];
		lo = e < lo ? e : lo;
		hi = e > hi ? e : hi;
	}
	double spread = 10 * log10(hi / lo);
	CHECK(spread < 1.0, "a harmonic at pitch +-4 %% a frame: envelope varies %.2f dB", spread);
	printf("  a harmonic at pitch +-4 %% a frame: envelope varies %.2f dB\n", spread);

	dstar_mbe_synth_init(&sy);
	p.f0_q24 = 400000;
	p.L = 19;
	for (int l = 1; l <= 19; ++l) {
		p.voiced[l] = 0;
		p.log2_amp[l] = 8 << 12;
	}
	for (int f = 0; f < 200; ++f)
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out + 160 * f);
	double P[96] = { 0 }, mx = 0, mean = 0;
	for (int s0 = 1600; s0 + 256 <= 200 * 160; s0 += 128) {
		for (int k = 16; k < 96; ++k) {
			double re = 0, im = 0;
			for (int n = 0; n < 256; ++n) {
				double x = out[s0 + n] * (0.5 - 0.5 * cos(2 * M_PI * n / 256));
				re += x * cos(2 * M_PI * k * n / 256);
				im -= x * sin(2 * M_PI * k * n / 256);
			}
			P[k] += re * re + im * im;
		}
	}
	for (int k = 16; k < 96; ++k) {
		mean += P[k] / 80;
		mx = P[k] > mx ? P[k] : mx;
	}
	double pm = 10 * log10(mx / mean);
	CHECK(pm < 4.0, "unvoiced: spectrum peak/mean %.1f dB (500-3000 Hz), expected noise-flat", pm);
	printf("  unvoiced: spectrum peak/mean %.1f dB over 500-3000 Hz\n", pm);

	dstar_mbe_synth_init(&sy);
	p.f0_q24 = 262144;
	p.L = 30;
	for (int l = 1; l <= 30; ++l) {
		p.voiced[l] = 1;
		p.log2_amp[l] = (12 << 12) - l * 200;
	}
	int flat = 0, peak = 0;
	for (int f = 0; f < 20; ++f) {
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		for (int i = 0; i < 160; ++i) {
			int a = abs(out[i]);
			flat += a >= 32760;
			peak = a > peak ? a : peak;
		}
	}
	CHECK(flat == 0 && peak > 28000, "a loud frame: %d samples at the clip level, peak %d", flat, peak);
	printf("  a loud frame: peak %d, %d samples at the clip level\n", peak, flat);
}

// The same unvoiced spectral density plays at the same level whatever the frame's pitch: DVSI codes an unvoiced band as
// a density, so its silence frames (b0 120, 69 Hz) and frames coded all unvoiced at a voice pitch are as loud as the
// noise they stand for. mbelib's scale, a band amplitude, played the same magnitudes 8 dB louder at 100 Hz than at
// b0 34's 247 Hz (band power M^2 / w0 over 1 / w0 bands)
static double unvoiced_level(unsigned b0)
{
	dstar_mbe_synth_t sy;
	dstar_ambe_params_t p;
	static int16_t out[160];
	dstar_mbe_synth_init(&sy);
	p.f0_q24 = dstar_mbe_f0_q24[b0];
	p.L = dstar_mbe_L[b0];
	for (int l = 1; l <= p.L; ++l) {
		p.voiced[l] = 0;
		p.log2_amp[l] = 6 << 12;
	}
	double e = 0;
	for (int f = 0; f < 100; ++f) {
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		for (int i = 0; f >= 10 && i < 160; ++i)
			e += (double)out[i] * out[i];
	}
	return 10 * log10(e / (90 * 160));
}

static void test_unvoiced_density(void)
{
	unsigned b0_100 = 0;	// the grid point nearest 100 Hz
	for (unsigned b = 0; b < 120; ++b) {
		if (fabs(dstar_mbe_f0_q24[b] * 8000.0 / 16777216 - 100) < fabs(dstar_mbe_f0_q24[b0_100] * 8000.0 / 16777216 - 100))
			b0_100 = b;
	}
	double a = unvoiced_level(34), b = unvoiced_level(b0_100), c = unvoiced_level(120);
	CHECK(fabs(b - a) < 1.5 && fabs(c - a) < 1.5, "unvoiced density: %.1f dB at b0 34, %+.1f at 100 Hz, %+.1f at b0 120",
	      a, b - a, c - a);
	printf("  unvoiced density: %.1f dB at b0 34, %+.1f dB at 100 Hz (b0 %u), %+.1f dB at b0 120\n", a, b - a, b0_100,
	       c - a);
}

// Voiced harmonics are spread in time (dstar_mbe_synth.c, env_phase's dispersion), not lined up into one sharp pulse a
// period, which is heard as buzz: a flat 100 Hz spectrum of 37 harmonics, in phase, is a pulse train with a crest factor
// of 19 dB, and the envelope's phase alone left it at 14.9 dB; spread, it is under 13 dB (5.5 when written)
static void test_voiced_dispersion(void)
{
	dstar_mbe_synth_t sy;
	dstar_ambe_params_t p;
	static int16_t out[160];
	dstar_mbe_synth_init(&sy);
	p.f0_q24 = 209715;	// 100 Hz
	p.L = 37;
	for (int l = 1; l <= p.L; ++l) {
		p.voiced[l] = 1;
		p.log2_amp[l] = 6 << 12;
	}
	double e = 0, pk = 0;
	for (int f = 0; f < 30; ++f) {
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out);
		for (int i = 0; f >= 10 && i < 160; ++i) {
			e += (double)out[i] * out[i];
			pk = fmax(pk, abs(out[i]));
		}
	}
	double rms = sqrt(e / (20 * 160)), crest = 20 * log10(pk / rms);
	CHECK(crest < 13.0, "voiced dispersion: crest factor %.1f dB, a pulse train's", crest);
	printf("  voiced dispersion: flat 100 Hz spectrum, crest factor %.1f dB (in phase: 19 dB)\n", crest);
}

// Voiced harmonics from 1 kHz up carry a noise floor (dstar_mbe_synth.c, VN_FLOOR), so they don't stand out of the
// silence between them as a vocoder's buzz does; below 1 kHz they are clean. A steady 125 Hz frame, flat harmonics:
// the power midway between harmonics against the power at them, averaged over 512-point Hann spectra
static void test_voiced_floor(void)
{
	dstar_mbe_synth_t sy;
	dstar_ambe_params_t p;
	static int16_t out[120 * 160];
	dstar_mbe_synth_init(&sy);
	p.f0_q24 = 262144;	// 125 Hz
	p.L = 30;
	for (int l = 1; l <= p.L; ++l) {
		p.voiced[l] = 1;
		p.log2_amp[l] = 6 << 12;
	}
	for (int f = 0; f < 120; ++f)
		dstar_mbe_synth_frame(&sy, &synth_scratch, &p, out + 160 * f);
	static double P[257];
	for (int s0 = 1600; s0 + 512 <= 120 * 160; s0 += 256) {
		for (int k = 0; k <= 256; ++k) {
			double re = 0, im = 0;
			for (int n = 0; n < 512; ++n) {
				double x = out[s0 + n] * (0.5 - 0.5 * cos(2 * M_PI * n / 512));
				re += x * cos(2 * M_PI * k * n / 512);
				im -= x * sin(2 * M_PI * k * n / 512);
			}
			P[k] += re * re + im * im;
		}
	}
	// 125 Hz is bin 8 of 512 at 8 kHz: harmonics at 8h, midway at 8h + 4
	double hl = 0, ml = 0, hh = 0, mh = 0;
	for (int h = 2; h <= 6; ++h) {
		hl += P[8 * h];
		ml += P[8 * h + 4];
	}
	for (int h = 10; h <= 22; ++h) {
		hh += P[8 * h];
		mh += P[8 * h + 4];
	}
	double lo = 10 * log10(hl / (ml + 1e-9)), hi = 10 * log10(hh / (mh + 1e-9));
	CHECK(lo > 40 && hi > 12 && hi < 30, "voiced floor: harmonics %.1f dB over the gaps at 250-750 Hz, %.1f at 1.25-2.75 kHz",
	      lo, hi);
	printf("  voiced floor: harmonics %.1f dB over the gaps at 250-750 Hz, %.1f dB at 1.25-2.75 kHz\n", lo, hi);
}


// --- leveler (dstar_mbe_level.c) ---

// Speech-like input for the leveler: a 120 Hz voiced sound (falling spectrum, slow vibrato) in half-sine syllables,
// 240 ms on and 120 ms off, over a background 30-40 dB down; the syllables at db1 dB before frame step and db2 after.
// The half-sine gives each syllable soft and loud frames, as speech has
#define LV_FRAMES	800	// 16 s: eight seconds a level, so even the leveler's slow fall (1/64 a speech frame) settles
#define LV_CYCLE	18	// frames a syllable and its gap
static int16_t lv_in[LV_FRAMES * 160];
static uint8_t lv_frames[LV_FRAMES][DSTAR_VOICE_BYTES];
static int16_t lv_out[LV_FRAMES * 160];

static double lv_bass_db;	// added to the voice's two lowest harmonics (120 and 240 Hz): a bass-heavy voice or mic

static void lv_syllables(double db1, double db2, int step, double onset_db)
{
	double ph[32] = { 0 };
	for (long i = 0; i < LV_FRAMES * 160L; ++i) {
		long f = i / 160, k = i % (LV_CYCLE * 160L);
		double env = k < 12 * 160 ? sin(M_PI * k / (12 * 160.0)) : 0;
		double db = f < step ? db1 : db2;
		if (onset_db != 0 && f >= step && f < step + LV_CYCLE) {
			db = onset_db;	// one loud syllable at full level from its first sample, then back to db2
			env = k < 12 * 160 ? 1 : 0;
		}
		double f0 = 120 * (1 + 0.03 * sin(2 * M_PI * 3 * i / 8000.0)), v = 0;
		for (int h = 1; h < 32 && h * f0 < 3800; ++h) {
			ph[h] += 2 * M_PI * h * f0 / 8000;
			v += cos(ph[h]) / h * (h <= 2 ? pow(10, lv_bass_db / 20) : 1);
		}
		v = 2000 * pow(10, db / 20) * env * v + 3 * gauss();
		lv_in[i] = (int16_t)fmax(-32767, fmin(32767, v));
	}
}

// lv_in through the encoder (the leveler on, or off) and the frames back through the decoder into lv_out
static void lv_round_trip(int leveler_off)
{
	dstar_codec_start(&codec, true);
	codec.u.enc.level.off = (uint8_t)leveler_off;
	for (int f = 0; f < LV_FRAMES; ++f)
		dstar_codec_encode(&codec, &lv_in[f * 160], lv_frames[f]);
	dstar_codec_start(&codec, false);
	for (int f = 0; f < LV_FRAMES; ++f)
		dstar_codec_decode(&codec, lv_frames[f], &lv_out[f * 160]);
}

// Power of x over n whole syllable cycles ending at frame end, dB
static double lv_db(const int16_t *x, int end, int cycles)
{
	double e = 0;
	for (long i = (long)(end - cycles * LV_CYCLE) * 160; i < end * 160L; ++i)
		e += (double)x[i] * x[i];
	return 10 * log10(e / (cycles * LV_CYCLE * 160.0) + 1e-9);
}

// Power of x between lo and hi Hz over n whole syllable cycles ending at frame end, dB: a Hann-windowed 160-point DFT a
// frame (50 Hz bins, both band edges included), one-sided, scaled by the window's power so a band covering everything
// gives about lv_db's figure. The window matters: unwindowed, a strong 240 Hz harmonic leaks into 1-3.4 kHz enough to
// move that band 3.7 dB on its own
static double lv_band_db(const int16_t *x, int end, int cycles, int lo, int hi)
{
	double e = 0;
	for (long f = end - cycles * LV_CYCLE; f < end; ++f)
		for (int k = lo / 50; k <= hi / 50; ++k) {
			double re = 0, im = 0;
			for (int n = 0; n < 160; ++n) {
				double a = 2 * M_PI * k * n / 160, w = 0.5 - 0.5 * cos(2 * M_PI * n / 160);
				re += w * x[f * 160 + n] * cos(a);
				im -= w * x[f * 160 + n] * sin(a);
			}
			e += (k == 0 || k == 80 ? 1 : 2) * (re * re + im * im) / (160 * 0.375);	// 0.375: mean of w^2
		}
	return 10 * log10(e / (cycles * LV_CYCLE * 160.0) + 1e-9);
}

// A step of 16 dB at the input, up and down, the range the leveler held on air within about 1.5 dB: the decoded speech
// level before and after the step within 2 dB, measured over the last six syllables at each level. The same input with
// the leveler off shows the full step, so the measure sees it; and the two runs' first levels, 16 dB apart at the
// input, come out within 2 dB of each other too
static void test_level_step(void)
{
	const int step = LV_FRAMES / 2, end = LV_FRAMES - 3;	// 3 frames: the encoder's 60 ms delay
	double before[2], after[2], first[2];
	for (int dir = 0; dir < 2; ++dir) {
		double db1 = dir ? 8 : -8, db2 = dir ? -8 : 8;
		lv_syllables(db1, db2, step, 0);
		double in_step = lv_db(lv_in, LV_FRAMES, 6) - lv_db(lv_in, step, 6);
		lv_round_trip(1);
		double raw = lv_db(lv_out, end, 6) - lv_db(lv_out, step, 6);
		lv_round_trip(0);
		before[dir] = lv_db(lv_out, step, 6);
		after[dir] = lv_db(lv_out, end, 6);
		first[dir] = before[dir];
		CHECK(fabs(raw - in_step) < 2.0, "level step %+.0f dB: %+.2f dB through the codec without the leveler",
		      db2 - db1, raw);
		CHECK(fabs(after[dir] - before[dir]) <= 2.0, "level step %+.0f dB at the input: %+.2f dB out", db2 - db1,
		      after[dir] - before[dir]);
		printf("  leveler: a %+.0f dB step at the input (%+.1f dB measured) comes out %+.2f dB (%+.1f without it)\n",
		       db2 - db1, in_step, after[dir] - before[dir], raw);
	}
	CHECK(fabs(first[0] - first[1]) <= 2.0, "leveler: inputs 16 dB apart come out %.2f dB apart",
	      fabs(first[0] - first[1]));
}

// The leveler sets the gain from the speech band (21 Nov 2026, dstar_mbe_level.c): Icom handhelds played us quietly
// because our level sat below 500 Hz, which their speakers don't reproduce, and a full-band measure let that bass turn
// the speech down. The same voice twice, the second with its two lowest harmonics (120, 240 Hz) 12 dB louder: the
// input's 1-3.4 kHz band is unchanged and its full band 11 dB up. The 19 Nov leveler cut the decoded 1-3.4 kHz band
// 12.4 dB for it; now it moves under 3 dB. Not to 0: the weighting is a gentle (f / 500)^2, so the 240 Hz harmonic
// still counts at -6.4 dB, and the DVSI match, with the frame's power kept, lifts the upper harmonics as the bass
// rises (+6.4 dB in that band with the leveler off)
static void test_level_speech_band(void)
{
	const int end = LV_FRAMES - 3;
	double in_sb[2], in_fb[2], sb[2];
	for (int bass = 0; bass < 2; ++bass) {
		lv_bass_db = bass ? 12 : 0;
		lv_syllables(0, 0, LV_FRAMES / 2, 0);
		in_sb[bass] = lv_band_db(lv_in, LV_FRAMES, 6, 1000, 3400);
		in_fb[bass] = lv_db(lv_in, LV_FRAMES, 6);
		lv_round_trip(0);
		sb[bass] = lv_band_db(lv_out, end, 6, 1000, 3400);
	}
	lv_bass_db = 0;
	CHECK(fabs(in_sb[1] - in_sb[0]) < 0.5 && in_fb[1] - in_fb[0] > 6, "bass boost: the test's input is wrong (speech "
	      "band %+.2f dB, full band %+.2f dB)", in_sb[1] - in_sb[0], in_fb[1] - in_fb[0]);
	CHECK(fabs(sb[1] - sb[0]) <= 3.0, "leveler: 12 dB more bass moves the decoded 1-3.4 kHz band %+.2f dB",
	      sb[1] - sb[0]);
	printf("  leveler: 12 dB more bass (input full band %+.1f dB) moves the decoded 1-3.4 kHz band %+.2f dB\n",
	       in_fb[1] - in_fb[0], sb[1] - sb[0]);
}

// Where the leveler puts speech, decoded by our decoder, in the 1-3.4 kHz band: pinned, so each step is made on
// purpose. The 19 Nov target and full-band measure put the test's voice at LV_SB_19NOV. 21 Nov: the target 6 dB up
// (LV_LOUDER), and the speech-band measure and the DVSI balance's 5 dB cut below 500 Hz stop this voice's bass (its
// two lowest harmonics carry most of its power) holding it down, which on this bass-heavy synthetic voice is worth
// another 6 dB: LV_SB_21NOV, 12 dB up. The next step of the target (DB(12)) moves this 6 dB more. Within 1 dB at the
// three input levels the step test uses, so it is the leveler's level, not the input's
#define LV_SB_19NOV	57.1	// dB, our decoder's int16 samples squared
#define LV_SB_21NOV	69.1
static void test_level_target(void)
{
	const int end = LV_FRAMES - 3;
	for (int in = -8; in <= 8; in += 8) {
		lv_syllables(in, in, LV_FRAMES / 2, 0);
		lv_round_trip(0);
		double sb = lv_band_db(lv_out, end, 6, 1000, 3400);
		CHECK(fabs(sb - LV_SB_21NOV) <= 1.0, "leveler: input %+d dB, 1-3.4 kHz at %.2f dB, not %.1f", in, sb,
		      LV_SB_21NOV);
		printf("  leveler: input %+d dB, 1-3.4 kHz band out at %.2f dB (%+.1f dB over the 19 Nov target's)\n", in,
		       sb, sb - LV_SB_19NOV);
	}
}

// A loud onset is capped at once: after eight seconds of quiet speech (the leveler's gain raised for it), one syllable
// 24 dB louder from its first sample. The leveler's attack alone (1/4 of the way a frame) lets its first frames through
// about 15 dB over the steady speech (measured with the ceiling taken out); the ceiling (10 dB over the target) holds
// every frame of it, as a DVSI decoder plays it (dstar_mbe_level_of), within 10 dB of the loudest steady frame.
// Measured on the leveler's output, before the quantizer, whose gain index tops out near there and would hide the cap
static void test_level_onset(void)
{
	const int step = LV_FRAMES / 2;
	lv_syllables(-8, -8, step, 16);
	dstar_mbe_level_t lv;
	dstar_mbe_ana_init(&ana);
	dstar_mbe_level_init(&lv);
	double steady = -1e9, onset = -1e9, later = -1e9;
	for (int f = 0; f < LV_FRAMES; ++f) {
		dstar_ambe_params_t p;
		dstar_mbe_analyze(&ana, &ana_scratch, &lv_in[f * 160], &p);
		dstar_mbe_level(&lv, &p);
		int32_t la = dstar_mbe_level_of(&p);
		if (la == INT32_MIN)
			continue;
		// the analysis's output lags its input by two to three frames, so call step is still the quiet speech
		double db = la / 680.4;
		if (f >= step - 6 * LV_CYCLE && f <= step && db > steady)
			steady = db;
		else if (f > step && f <= step + LV_CYCLE + 3 && db > onset)
			onset = db;
		else if (f >= LV_FRAMES - 6 * LV_CYCLE && db > later)
			later = db;
	}
	CHECK(onset - steady <= 10.0, "a syllable 24 dB louder: its loudest frame %+.2f dB over the steady speech's",
	      onset - steady);
	CHECK(onset - steady > 3.0, "the onset barely shows (%+.2f dB): the test's input is wrong", onset - steady);
	CHECK(fabs(later - steady) <= 2.0, "after the onset the speech comes back %+.2f dB off", later - steady);
	printf("  leveler: a syllable 24 dB louder after quiet speech peaks %+.2f dB over it, then %+.2f dB\n",
	       onset - steady, later - steady);
}

int main(void)
{
	printf("dstarCodecTest\n");
	dstar_codec_bind(&codec, &scratch);
	test_quant_round_trip();
	test_quant_agreement();
	test_tones();
	test_noise();
	test_mix();
	test_fricative();
	test_glide();
	test_weak_fundamental();
	test_quantizer_keeps_pitch();
	test_silence();
	test_tone_level();
	test_bad_frames();
	test_full_scale();
	test_dvsi();
	test_synth_changes();
	test_unvoiced_density();
	test_voiced_dispersion();
	test_voiced_floor();
	test_frame_round_trip();
	test_frame_errors();
	test_bit_order();
	test_dvsi_frames();
	test_frame_spec();
	test_frame_golay_patterns();
	test_frame_whitening();
	test_level_step();
	test_level_speech_band();
	test_level_target();
	test_level_onset();
	test_codec_interface();
	test_codec_bad_frames();
	printf("%s dstarCodecTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
