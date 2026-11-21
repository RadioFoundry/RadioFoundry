// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Speech quality of the software D-STAR voice codec (core/dstar/dstar_codec.c, docs/spec/dstar-codec.md 7): signals
 * through dstar_codec_encode -> 72-bit frames -> dstar_codec_decode, compared with the input.
 *
 * MBE vocoders don't keep the waveform's phase, so a waveform SNR says nothing (near 0 dB even for a good codec). The
 * measures are spectral, after aligning the two signals on their frame-energy envelopes:
 *   LSD    log-spectral distance, dB RMS over 0-4 kHz, active frames only
 *   MCD    mel-cepstral distortion, dB (24 mel bands 100-3800 Hz, 13 coefficients without c0)
 *   level  decoded minus input power over the active frames, dB
 * (256-point Hann frames every 80 samples; the method of the codec's research, test/quality.py, ported to C.) They
 * rank variants and catch regressions; they are not a MOS.
 *
 * Generated signals: steady vowels (harmonics under three formants) at three pitches, a pitch glide, white noise and
 * silence; pitch, level and the harmonic envelope (LSD for the noise) checked. Then, when ffmpeg with its flite filter
 * is on the machine, the four flite voices (CMU Flite's slt, kal, awb and rms, generated at run time through a pipe and
 * never written to disk; audio recordings are never committed, AGENTS.md) with margins over the research's measured
 * figures, so a regression fails; and the same frames through a channel with bit errors (1, 3 and 5 % random, and
 * fades) into the decoder's bad-frame rule and concealment, the concealed share and MCD checked.
 * Without ffmpeg or flite that part is skipped with a note.
 *
 *   make -C tests/host run-dstar          dstarCodecQualityTest -v    per-voice details
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dstar/dstar_codec.h"

static int failures = 0;
static int verbose = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS	8000
#define N	256
#define HOP	80
#define NB	(N / 2 + 1)
#define NMEL	24
#define NCEP	14

static dstar_codec_t codec;
static dstar_codec_scratch_t scratch;

// Input through the encoder, 160 samples at a time, with three frames of silence after it to flush the analysis's
// look-ahead (the output is 60 ms late). Returns the frames (malloc'd) and their count
static uint8_t *encode_all(const int16_t *in, long n, long *count)
{
	long frames = (n + 159) / 160 + 3;
	uint8_t *buf = malloc((size_t)frames * DSTAR_VOICE_BYTES);
	dstar_codec_start(&codec, true);
	// The codec as a coder: the leveler (dstar_mbe_level.c) changes the level on purpose, which the level checks here
	// would read as an error, and the research's figures these margins sit over were measured without it.
	// dstarCodecTest checks the leveler itself. Likewise DVSI's band balance (21 Nov, dstar_mbe_analysis.c), a deliberate
	// colouring for DVSI decoders that ours does not undo: with it these figures move far past their limits (vowels'
	// harmonic distance 6-8 dB, flite MCD 40.7 dB over 22.5), by design; dstarCodecTest checks the balance
	codec.u.enc.level.off = 1;
	codec.u.enc.ana.flat = 1;
	for (long f = 0; f < frames; f++) {
		int16_t pcm[160];
		for (int i = 0; i < 160; i++) {
			long k = f * 160 + i;
			pcm[i] = k < n ? in[k] : 0;
		}
		CHECK(dstar_codec_encode(&codec, pcm, buf + f * DSTAR_VOICE_BYTES), "encode refused");
	}
	*count = frames;
	return buf;
}

// The channel: independent bit errors at ber, or (fade) a two-state Gilbert-Elliott channel per frame: good to fade
// with probability 0.03, fade to good 0.25 (fades of 80 ms on average, 11 % of the time), 0.1 % bit errors in good
// frames and 25 % in a fade. The research's channels (mbe/test/eval2.c)
typedef struct {
	const char *name;
	double ber;
	int fade;
	double mcd, concealed;	// limits: mean MCD over the four voices (dB), share of frames concealed
} channel_t;

static uint64_t ch_state;

static double ch_uniform(void)
{
	ch_state ^= ch_state << 13;
	ch_state ^= ch_state >> 7;
	ch_state ^= ch_state << 17;
	return (double)(ch_state >> 11) / 9007199254740992.0;
}

// The frames through the channel (NULL: none) and the decoder, one direction at a time as on the radio. Returns the
// output length; *concealed counts the frames the decoder concealed
static long decode_all(const uint8_t *frames, long count, const channel_t *ch, int16_t *out, long *concealed)
{
	int in_fade = 0;
	*concealed = 0;
	dstar_codec_start(&codec, false);
	for (long f = 0; f < count; f++) {
		uint8_t air[DSTAR_VOICE_BYTES];
		memcpy(air, frames + f * DSTAR_VOICE_BYTES, DSTAR_VOICE_BYTES);
		if (ch != NULL) {
			double ber = ch->ber;
			if (ch->fade) {
				in_fade = in_fade ? ch_uniform() >= 0.25 : ch_uniform() < 0.03;
				ber = in_fade ? 0.25 : 0.001;
			}
			for (unsigned i = 0; i < DSTAR_VOICE_BITS; i++) {
				if (ch_uniform() < ber)
					air[i >> 3] ^= (uint8_t)(1u << (i & 7u));
			}
		}
		CHECK(dstar_codec_decode(&codec, air, out + f * 160), "decode refused");
		if (dstar_codec_errors(&codec) == DSTAR_CODEC_FAILED)
			++*concealed;
		else if (ch == NULL)
			CHECK(dstar_codec_errors(&codec) == 0, "errors on a clean frame");
	}
	return count * 160;
}

static long run_codec(const int16_t *in, long n, int16_t *out)
{
	long count, concealed;
	uint8_t *frames = encode_all(in, n, &count);
	long m = decode_all(frames, count, NULL, out, &concealed);
	free(frames);
	return m;
}

// --- the measures (test/quality.py's method) ---

static double hann[N];
static double fb[NMEL][NB];
static double dct[NCEP][NMEL];

static double mel(double f)
{
	return 2595.0 * log10(1.0 + f / 700.0);
}

static double imel(double m)
{
	return 700.0 * (pow(10.0, m / 2595.0) - 1.0);
}

static void measures_init(void)
{
	for (int i = 0; i < N; i++)
		hann[i] = 0.5 - 0.5 * cos(2 * M_PI * i / (N - 1));
	double pts[NMEL + 2];
	for (int i = 0; i < NMEL + 2; i++)
		pts[i] = imel(mel(100) + (mel(3800) - mel(100)) * i / (NMEL + 1));
	for (int i = 0; i < NMEL; i++) {
		double l = pts[i], c = pts[i + 1], r = pts[i + 2];
		for (int k = 0; k < NB; k++) {
			double f = k * (double)FS / N;
			double v = fmin((f - l) / (c - l), (r - f) / (r - c));
			fb[i][k] = v > 0 ? v : 0;
		}
	}
	for (int j = 0; j < NCEP; j++) {
		for (int m = 0; m < NMEL; m++)
			dct[j][m] = sqrt(2.0 / NMEL) * cos(M_PI * j * (m + 0.5) / NMEL);
	}
}

// Radix-2 FFT, in place
static void fft(double *re, double *im)
{
	for (int i = 1, j = 0; i < N; i++) {
		int bit = N >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			double t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}
	for (int len = 2; len <= N; len <<= 1) {
		double a = -2 * M_PI / len;
		for (int i = 0; i < N; i += len) {
			for (int k = 0; k < len / 2; k++) {
				double wr = cos(a * k), wi = sin(a * k);
				double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
				double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
				re[i + k + len / 2] = re[i + k] - xr;
				im[i + k + len / 2] = im[i + k] - xi;
				re[i + k] += xr;
				im[i + k] += xi;
			}
		}
	}
}

static long nframes(long n)
{
	return n < N ? 0 : 1 + (n - N) / HOP;
}

// The power spectrum of each frame (+1e-3, as the reference does) and the frame's windowed energy
static void power_spec(const int16_t *x, long n, double (*P)[NB], double *en)
{
	long nf = nframes(n);
	for (long f = 0; f < nf; f++) {
		double re[N], im[N], e = 0;
		for (int i = 0; i < N; i++) {
			re[i] = x[f * HOP + i] * hann[i];
			im[i] = 0;
			e += re[i] * re[i];
		}
		if (en)
			en[f] = log10(e + 1);
		if (P) {
			fft(re, im);
			for (int k = 0; k < NB; k++)
				P[f][k] = re[k] * re[k] + im[k] * im[k] + 1e-3;
		}
	}
}

typedef struct {
	long lag;	// samples the output is late
	double lsd, mcd, level;
} quality_t;

static double corr(const double *a, const double *b, long n)
{
	double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
	for (long i = 0; i < n; i++) {
		ma += a[i];
		mb += b[i];
	}
	ma /= n;
	mb /= n;
	for (long i = 0; i < n; i++) {
		sab += (a[i] - ma) * (b[i] - mb);
		saa += (a[i] - ma) * (a[i] - ma);
		sbb += (b[i] - mb) * (b[i] - mb);
	}
	return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : -1;
}

// lag < 0: align on the energy envelopes (speech); else the output is that many samples late (a steady signal, whose
// envelope is flat and can't be aligned on)
static quality_t compare(const int16_t *ref, long nr, const int16_t *dec, long nd, long lag)
{
	quality_t q = { 0 };
	if (lag >= 0) {
		q.lag = lag;
		goto aligned;
	}
	long fr = nframes(nr), fd = nframes(nd);
	double *er = malloc(sizeof(double) * (size_t)fr), *ed = malloc(sizeof(double) * (size_t)fd);
	power_spec(ref, nr, NULL, er);
	power_spec(dec, nd, NULL, ed);
	double best = -1e9;
	for (long d = 0; d < 60; d++) {	// the output is late, up to 600 ms
		long n = fr < fd - d ? fr : fd - d;
		double c = corr(er, ed + d, n);
		if (c > best) {
			best = c;
			q.lag = d * HOP;
		}
	}
	free(er);
	free(ed);
aligned:
	dec += q.lag;
	nd -= q.lag;
	long n = nr < nd ? nr : nd, nf = nframes(n);
	double (*Pr)[NB] = malloc(sizeof(*Pr) * (size_t)nf), (*Pd)[NB] = malloc(sizeof(*Pd) * (size_t)nf);
	power_spec(ref, n, Pr, NULL);
	power_spec(dec, n, Pd, NULL);
	double emax = -1e9;
	for (long f = 0; f < nf; f++) {
		double s = 0;
		for (int k = 0; k < NB; k++)
			s += Pr[f][k];
		emax = fmax(emax, 10 * log10(s));
	}
	double lsd = 0, mcd = 0, sr = 0, sd = 0;
	long act = 0;
	for (long f = 0; f < nf; f++) {
		double s = 0;
		for (int k = 0; k < NB; k++)
			s += Pr[f][k];
		if (!(10 * log10(s) > emax - 35))
			continue;
		act++;
		double d2 = 0;
		for (int k = 0; k < NB; k++) {
			double d = 10 * log10(Pr[f][k]) - 10 * log10(Pd[f][k]);
			d2 += d * d;
			sr += Pr[f][k];
			sd += Pd[f][k];
		}
		lsd += sqrt(d2 / NB);
		double Er[NMEL], Ed[NMEL];
		for (int i = 0; i < NMEL; i++) {
			double a = 0, b = 0;
			for (int k = 0; k < NB; k++) {
				a += Pr[f][k] * fb[i][k];
				b += Pd[f][k] * fb[i][k];
			}
			Er[i] = log(a);
			Ed[i] = log(b);
		}
		double c2 = 0;
		for (int j = 1; j < NCEP; j++) {
			double cr = 0, cd = 0;
			for (int m = 0; m < NMEL; m++) {
				cr += Er[m] * dct[j][m];
				cd += Ed[m] * dct[j][m];
			}
			c2 += (cr - cd) * (cr - cd);
		}
		mcd += 10 / log(10) * sqrt(2 * c2);
	}
	free(Pr);
	free(Pd);
	q.lsd = act ? lsd / act : 0;
	q.mcd = act ? mcd / act : 0;
	q.level = 10 * log10(sd / sr);
	return q;
}

// The pitch of x[0..n) in Hz from its normalized autocorrelation, 62 to 400 Hz, interpolated between lags
static double pitch_of(const int16_t *x, long n)
{
	double best = -1, r[132] = { 0 };
	int lag = 0;
	for (int t = 18; t <= 131; t++) {
		double s = 0, e0 = 0, e1 = 0;
		for (long i = 0; i + t < n; i++) {
			s += (double)x[i] * x[i + t];
			e0 += (double)x[i] * x[i];
			e1 += (double)x[i + t] * x[i + t];
		}
		r[t] = e0 > 0 && e1 > 0 ? s / sqrt(e0 * e1) : 0;
	}
	for (int t = 20; t <= 129; t++) {
		if (r[t] > r[t - 1] && r[t] >= r[t + 1] && r[t] > best + 0.02) {	// the first of near-equal peaks
			best = r[t];
			lag = t;
		}
	}
	if (lag == 0)
		return 0;
	// A period between two integer lags (53.5 samples, 149.6 Hz) correlates worse at either neighbor than twice it
	// does at the integer near 107: prefer half the lag when a peak there is within 0.06 (seen since the pitch grid
	// moved to DVSI's, 18 Nov 2026, which put the 150 Hz grid point on such a period)
	for (int t = lag / 2 - 1; t <= lag / 2 + 1 && lag >= 40; t++) {
		if (t >= 20 && r[t] > r[t - 1] && r[t] >= r[t + 1] && r[t] > r[lag] - 0.06) {
			lag = t;
			break;
		}
	}
	double a = r[lag - 1], b = r[lag], c = r[lag + 1], d = a - 2 * b + c;
	double frac = d != 0 ? 0.5 * (a - c) / d : 0;
	return FS / (lag + frac);
}

// The harmonic envelope distance for a signal of known pitch f0 (Hz): per frame, the power in each harmonic's band
// [k - 1/2, k + 1/2) f0 below 3.7 kHz, input against output in dB, over the harmonics within 40 dB of the frame's
// strongest; RMS over harmonics, mean over frames. LSD over every bin says little for a synthetic harmonic signal,
// whose spectrum between harmonics and above its last one is empty: there it measures only the codec's noise floor
static double harmonic_distance(const int16_t *ref, const int16_t *dec, long n, double f0)
{
	long nf = nframes(n);
	double (*Pr)[NB] = malloc(sizeof(*Pr) * (size_t)nf), (*Pd)[NB] = malloc(sizeof(*Pd) * (size_t)nf);
	power_spec(ref, n, Pr, NULL);
	power_spec(dec, n, Pd, NULL);
	double sum = 0;
	for (long f = 0; f < nf; f++) {
		double hr[64], hd[64], top = 0;
		int nh = 0;
		for (int k = 1; k < 64 && (k + 0.5) * f0 < 3700; k++, nh++) {
			hr[nh] = hd[nh] = 0;
			for (int b = 0; b < NB; b++) {
				double hz = b * (double)FS / N;
				if (hz >= (k - 0.5) * f0 && hz < (k + 0.5) * f0) {
					hr[nh] += Pr[f][b];
					hd[nh] += Pd[f][b];
				}
			}
			top = fmax(top, hr[nh]);
		}
		double d2 = 0;
		int used = 0;
		for (int k = 0; k < nh; k++) {
			if (hr[k] < top * 1e-4)
				continue;
			double d = 10 * log10(hd[k] / hr[k]);
			d2 += d * d;
			used++;
		}
		sum += used ? sqrt(d2 / used) : 0;
	}
	free(Pr);
	free(Pd);
	return nf ? sum / nf : 0;
}

static double rms_db(const int16_t *x, long n)
{
	double e = 0;
	for (long i = 0; i < n; i++)
		e += (double)x[i] * x[i];
	return 10 * log10(e / n + 1e-9);
}

// --- generated signals ---

static uint32_t rng = 0x1234567u;

static double frand(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) / 16777216.0;
}

static double gauss(void)
{
	double s = 0;
	for (int i = 0; i < 12; i++)
		s += frand();
	return s - 6.0;
}

// A vowel: the harmonics of f0 (gliding to f1 over the signal) under three formant resonances and a -6 dB an octave
// source, scaled to a peak near 12000
typedef struct {
	const char *name;
	double F[3], B[3];
} vowel_t;

static const vowel_t vowels[] = {
	{ "a", { 730, 1090, 2440 }, { 80, 90, 120 } },
	{ "i", { 270, 2290, 3010 }, { 60, 100, 120 } },
	{ "u", { 300, 870, 2240 }, { 60, 90, 120 } },
};

static double formant_gain(const vowel_t *v, double f)
{
	double g = 1;
	for (int k = 0; k < 3; k++) {
		double x = f / v->F[k], y = f * v->B[k] / (v->F[k] * v->F[k]);
		g /= sqrt((1 - x * x) * (1 - x * x) + y * y);
	}
	return g;
}

static void make_vowel(const vowel_t *v, double f0, double f1, int16_t *x, long n)
{
	double ph[64] = { 0 }, peak = 0;
	double *y = malloc(sizeof(double) * (size_t)n);
	for (int k = 0; k < 64; k++)
		ph[k] = 2 * M_PI * frand();
	for (long i = 0; i < n; i++) {
		double f = f0 + (f1 - f0) * i / n, s = 0;
		for (int k = 1; k < 64 && k * f < 3800; k++) {
			ph[k] += 2 * M_PI * k * f / FS;
			s += formant_gain(v, k * f) / k * cos(ph[k]);
		}
		y[i] = s;
		peak = fmax(peak, fabs(s));
	}
	for (long i = 0; i < n; i++)
		x[i] = (int16_t)lrint(y[i] * 12000.0 / peak);
	free(y);
}

#define GEN_N	(2 * FS)
// Limits a little over what was measured before the perceptual search and the gain match (vowels: harmonic distance
// 2.5 to 5.4 dB, level -3.2 to +0.7 dB, the worst both where one harmonic sits on a formant, /a/ and /i/ at 220 Hz;
// noise LSD 8.3 dB, level -0.7 dB). Now: harmonic distance 2.4 to 5.2 dB, level -1.2 to +0.1 dB (the gain match), noise
// LSD 8.5 dB, level -0.4 dB. The generators are seeded, so the figures repeat exactly; a change to the codec that moves
// them past these is a regression to look at
#define HARM_LIMIT	6.0
#define LEVEL_LIMIT	3.8
// The decoder plays everything this much down, at an IC-9700's level (dstar_mbe_synth.c, OUT_GAIN_Q15): the level
// checks are about it
#define OUT_DB		(-4.4)
#define NOISE_LSD_LIMIT	13.0	// 9.07 since unvoiced bands became true noise, whose spectrum differs from the input's by its
				// own variance; 11.4 through the DVSI match and its expansion, 9.8 on DVSI's pitch grid, 9.7
				// with the expansion at 1/0.73 (vowels' worst harmonic distance 4.17 -> 5.00 dB with it); 12.0
				// with the output 4.4 dB down and noise 1.5 dB down (LSD is not level matched), 10.0 without

// The output's pitch over the steady part, compared frame by frame with the input's, after the codec's delay
static void test_vowels(void)
{
	static int16_t in[GEN_N], out[GEN_N + 8 * 160];
	const double f0s[3] = { 100, 150, 220 };
	double worst_lsd = 0, worst_lvl = 0, worst_pitch = 0;
	for (unsigned v = 0; v < sizeof(vowels) / sizeof(vowels[0]); v++) {
		for (int p = 0; p < 3; p++) {
			make_vowel(&vowels[v], f0s[p], f0s[p], in, GEN_N);
			long m = run_codec(in, GEN_N, out);
			double hd = harmonic_distance(in + FS / 2, out + 480 + FS / 2, FS, f0s[p]);
			(void)m;
			// Pitch of the output's steady part, 0.5 s to 1.5 s of the input
			double po = pitch_of(out + 480 + FS / 2, FS);
			double pe = fabs(po / f0s[p] - 1);
			double lvl = rms_db(out + 480 + FS / 2, FS) - rms_db(in + FS / 2, FS);
			if (verbose)
				printf("  vowel /%s/ %3.0f Hz: output pitch %.1f Hz, level %+.2f dB, harmonic distance %.2f dB\n",
				       vowels[v].name, f0s[p], po, lvl, hd);
			CHECK(pe < 0.02, "vowel /%s/ %.0f Hz: output pitch %.1f Hz", vowels[v].name, f0s[p], po);
			CHECK(fabs(lvl - OUT_DB) < LEVEL_LIMIT, "vowel /%s/ %.0f Hz: level %+.2f dB", vowels[v].name, f0s[p], lvl);
			CHECK(hd < HARM_LIMIT, "vowel /%s/ %.0f Hz: harmonic distance %.2f dB", vowels[v].name, f0s[p], hd);
			worst_lsd = fmax(worst_lsd, hd);
			worst_lvl = fmax(worst_lvl, fabs(lvl - OUT_DB));
			worst_pitch = fmax(worst_pitch, pe);
		}
	}
	printf("  vowels /a/ /i/ /u/ at 100, 150, 220 Hz: pitch within %.2f %%, level within %.2f dB, "
	       "harmonic distance at most %.2f dB\n",
	       100 * worst_pitch, worst_lvl, worst_lsd);
}

static void test_glide(void)
{
	static int16_t in[GEN_N], out[GEN_N + 8 * 160];
	make_vowel(&vowels[0], 90, 240, in, GEN_N);
	long m = run_codec(in, GEN_N, out);
	(void)m;
	double worst = 0;
	int bad = 0;
	// Frames of 40 ms, the output 480 samples late
	for (long s = 1600; s + 320 <= GEN_N - 1600; s += 320) {
		double pi = pitch_of(in + s, 320), po = pitch_of(out + s + 480, 320);
		double e = fabs(po / pi - 1);
		worst = fmax(worst, e);
		bad += e > 0.03;
	}
	// (One frame in the 46 read at half the pitch for a while: the reader's half-sample period weakness, fixed in pitch_of)
	CHECK(bad == 0, "glide: %d frames more than 3 %% off the input's pitch", bad);
	printf("  glide 90->240 Hz in 2 s: worst output pitch error %.2f %%\n", 100 * worst);
}

static void test_noise_and_silence(void)
{
	static int16_t in[GEN_N], out[GEN_N + 8 * 160];
	for (long i = 0; i < GEN_N; i++)
		in[i] = (int16_t)lrint(2000 * gauss());
	long m = run_codec(in, GEN_N, out);
	quality_t q = compare(in, GEN_N, out, m, 480);
	double lvl = rms_db(out + 480 + 1600, GEN_N - 3200) - rms_db(in + 1600, GEN_N - 3200);
	printf("  white noise: level %+.2f dB, LSD %.2f dB\n", lvl, q.lsd);
	// Noise comes back 1.5 dB quieter by design (dstarCodecTest's NOISE_RT_DB, docs/spec/dstar-codec.md 4)
	CHECK(fabs(lvl - OUT_DB + 1.5) < 1.5, "noise: level %+.2f dB", lvl);
	CHECK(q.lsd < NOISE_LSD_LIMIT, "noise: LSD %.2f dB", q.lsd);

	// Digital silence comes out as a faint noise, not zeros: the smallest gain step is 0 (mbelib's Dg[0]), so the
	// decoded mean log magnitude never falls below -log2(L) / 2 (docs/spec/dstar-codec.md 8). About -65 dBFS
	memset(in, 0, sizeof(in));
	m = run_codec(in, GEN_N, out);
	int peak = 0;
	for (long i = 0; i < m; i++)
		peak = abs(out[i]) > peak ? abs(out[i]) : peak;
	printf("  digital silence: output peak %d\n", peak);
	CHECK(peak < 40, "silence: output peak %d", peak);
}

// --- flite voices ---

#define FLITE_TEXT "The quick brown fox jumps over the lazy dog. This is a test of the D-STAR voice codec, " \
	"running on the host. Please say again, your signal is five and nine. Seventy three, and good luck with the radio."

// Measured by this test on the same text and voices; the limits are those figures plus a margin, so a change that costs
// quality fails. Since 18 Nov 2026 the encoder matches DVSI's flatter analysis (shape to 0.6, dstar_mbe_analysis.c) and
// the decoder expands it back (dstar_mbe_synth.c): an ID-51 could not understand full-depth frames. Reference metrics
// count the round trip through that pair a little worse (before it, with the same synthesizer: MCD 22.23, 21.25, 20.53,
// 22.93, mean 21.73; LSD 10.94, 8.57, 8.99, 9.16), since the expansion multiplies the shape's quantization error by
// 1/0.6. The research's STOI on its 12 inputs: 0.876 before the match, 0.861 with it (0.865 before the synthesizer's
// changes too); on the operator's own voice, against the codec before both, MCD 24.0 -> 23.0 dB, STOI 0.836 -> 0.871.
// Since 19 Nov 2026 the decoder expands by 1/0.73 instead (dstar_mbe_synth.c): MCD 23.08, 22.87, 21.84, 23.98, mean
// 22.94, and LSD 10.32, 8.53, 8.89, 9.08 with 1/0.6. slt is the one voice that loses (MCD +1.25 dB; in the research's
// 12 inputs it is one of two, with 10 gaining 0.7 to 4.0 dB), against an IC-9700's frames' STOI 0.610 -> 0.647.
// Since the changes of 19 Nov 2026 matched to an IC-9700's decoder (unvoiced bands as a density, the voiced phase
// dispersed, a noise floor under voiced harmonics, the output 4.4 dB down; dstar_mbe_synth.c): MCD 24.33, 19.67, 21.16,
// 23.29 -> 23.71, 19.57, 20.42, 23.14, mean 22.11 -> 21.71; LSD 11.15, 8.58, 9.43, 9.24 -> 10.09, 8.46, 9.13, 8.86 (LSD
// is not level matched, and moved with the output level too)
static const struct {
	const char *voice;
	double mcd, lsd;	// measured, dB
} flite_ref[] = {
	{ "slt", 23.71, 10.09 },
	{ "kal", 19.57, 8.46 },
	{ "awb", 20.42, 9.13 },
	{ "rms", 23.14, 8.86 },
};
#define MCD_MARGIN	1.0	// per voice
#define MEAN_MCD	21.71	// measured mean (22.11 before 19 Nov 2026's decoder match, 22.94 with the 1/0.6 expansion,
				// 22.80 before the pitch grid moved to DVSI's)
#define MEAN_MCD_LIMIT	22.50
#define LSD_MARGIN	0.5

// Measured here with the research's seed 1, over the four voices (MCD 22.80, 25.36, 28.91 and 29.69 dB; 0.0, 0.5, 2.3
// and 6.5 % of frames concealed; 23.00, 25.14, 28.52 and 29.65 before 19 Nov 2026's decoder match); the limits are
// those plus a margin. Before the DVSI match (18 Nov 2026) the MCDs were
// 22.33, 23.68, 26.21 and 28.05, and with its first expansion, 1/0.6, 24.08, 26.28, 29.94 and 30.58: the decoder's
// expansion enlarges a corrupted shape index's error by as much, as a DVSI decoder does with our frames. The concealment is unchanged. The research measured the same channels over 12
// inputs and three seeds (docs/spec/dstar-codec.md 7). With mbelib's rule 5 % bit errors concealed 18 % of frames, so
// the concealed share catches a rule that throws good frames away
static const channel_t channels[] = {
	{ "1 % bit errors", 0.01, 0, 23.6, 0.005 },
	{ "3 % bit errors", 0.03, 0, 25.6, 0.015 },
	{ "5 % bit errors", 0.05, 0, 29.4, 0.040 },
	{ "fades", 0, 1, 30.7, 0.100 },
};

static int flite_available(void)
{
	FILE *p = popen("ffmpeg -hide_banner -filters 2>/dev/null", "r");
	if (!p)
		return 0;
	char line[512];
	int found = 0;
	while (fgets(line, sizeof(line), p)) {
		if (strstr(line, " flite "))
			found = 1;
	}
	return pclose(p) == 0 && found;
}

// The voice as 8 kHz 16-bit samples from ffmpeg's stdout; returns the count, 0 on failure
static long flite_speech(const char *voice, int16_t *buf, long max)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "ffmpeg -hide_banner -loglevel error -f lavfi -i \"flite=text='%s':voice=%s\" "
		 "-ar 8000 -ac 1 -f s16le - 2>/dev/null", FLITE_TEXT, voice);
	FILE *p = popen(cmd, "r");
	if (!p)
		return 0;
	long n = (long)fread(buf, sizeof(int16_t), (size_t)max, p);
	if (pclose(p) != 0)
		return 0;
	return n;
}

static void test_flite(void)
{
	if (!flite_available()) {
		printf("  flite voices: skipped (no ffmpeg with the flite filter on this machine)\n");
		return;
	}
	enum { MAXN = 30 * FS, NCH = sizeof(channels) / sizeof(channels[0]) };
	int16_t *in = malloc(sizeof(int16_t) * MAXN), *out = malloc(sizeof(int16_t) * (MAXN + 8 * 160));
	double sum = 0, ch_mcd[NCH] = { 0 };
	long ch_concealed[NCH] = { 0 }, ch_frames = 0;
	int count = 0;
	for (unsigned v = 0; v < sizeof(flite_ref) / sizeof(flite_ref[0]); v++) {
		long n = flite_speech(flite_ref[v].voice, in, MAXN);
		CHECK(n > 4 * FS, "flite voice %s: %ld samples", flite_ref[v].voice, n);
		if (n <= 4 * FS)
			continue;
		long nf, concealed;
		uint8_t *frames = encode_all(in, n, &nf);
		long m = decode_all(frames, nf, NULL, out, &concealed);
		quality_t q = compare(in, n, out, m, -1);
		for (unsigned c = 0; c < NCH; c++) {
			ch_state = 88172645463325252ull + 0x9E3779B97F4A7C15ull;	// eval2's seed 1
			long mc = decode_all(frames, nf, &channels[c], out, &concealed);
			quality_t qc = compare(in, n, out, mc, 480);
			if (verbose)
				printf("    %s, %s: MCD %5.2f dB, %ld of %ld frames concealed\n", flite_ref[v].voice,
				       channels[c].name, qc.mcd, concealed, nf);
			ch_mcd[c] += qc.mcd / 4;
			ch_concealed[c] += concealed;
		}
		ch_frames += nf;
		free(frames);
		printf("  flite %s: delay %ld ms  LSD %5.2f dB (research %5.2f)  MCD %5.2f dB (research %5.2f)  "
		       "level %+.1f dB\n", flite_ref[v].voice, q.lag / 8, q.lsd, flite_ref[v].lsd, q.mcd, flite_ref[v].mcd,
		       q.level);
		CHECK(q.mcd <= flite_ref[v].mcd + MCD_MARGIN, "flite %s: MCD %.2f dB over %.2f", flite_ref[v].voice, q.mcd,
		      flite_ref[v].mcd + MCD_MARGIN);
		CHECK(q.lsd <= flite_ref[v].lsd + LSD_MARGIN, "flite %s: LSD %.2f dB over %.2f", flite_ref[v].voice, q.lsd,
		      flite_ref[v].lsd + LSD_MARGIN);
		CHECK(fabs(q.level - OUT_DB) < 2.0, "flite %s: level %+.1f dB", flite_ref[v].voice, q.level);
		CHECK(q.lag == 480, "flite %s: delay %ld samples, 480 expected", flite_ref[v].voice, q.lag);
		sum += q.mcd;
		count++;
	}
	if (count == 4) {
		printf("  flite mean MCD %.2f dB (limit %.2f, research %.2f)\n", sum / count, MEAN_MCD_LIMIT, MEAN_MCD);
		CHECK(sum / count <= MEAN_MCD_LIMIT, "flite mean MCD %.2f dB over %.2f", sum / count, MEAN_MCD_LIMIT);
		for (unsigned c = 0; c < NCH; c++) {
			double share = (double)ch_concealed[c] / (double)ch_frames;
			printf("  flite through %s: mean MCD %5.2f dB (limit %5.2f), %4.1f %% of frames concealed (limit %4.1f)\n",
			       channels[c].name, ch_mcd[c], channels[c].mcd, 100 * share, 100 * channels[c].concealed);
			CHECK(ch_mcd[c] <= channels[c].mcd, "%s: mean MCD %.2f dB over %.2f", channels[c].name, ch_mcd[c],
			      channels[c].mcd);
			CHECK(share <= channels[c].concealed, "%s: %.1f %% of frames concealed, over %.1f", channels[c].name,
			      100 * share, 100 * channels[c].concealed);
		}
	}
	free(in);
	free(out);
}

int main(int argc, char **argv)
{
	verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	printf("dstarCodecQualityTest\n");
	dstar_codec_bind(&codec, &scratch);
	measures_init();
	test_vowels();
	test_glide();
	test_noise_and_silence();
	test_flite();
	printf("%s dstarCodecQualityTest: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}
