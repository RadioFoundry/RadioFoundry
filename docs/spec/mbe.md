<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# The MBE voice core

The multiband excitation (MBE) speech model in integers (`core/mbe`), shared by the voice codecs of this family. A
codec is a front end on the core: its quantizer, dequantizer, tables and frame format. The core does the rest.

| Front end | Files | Switch | Spec |
|---|---|---|---|
| Full rate, 88 bits a frame (TIA-102.BABA) | `imbe_*.c`, `imbe.h` | `CONFIG_IMBE`, with `CONFIG_P25` | [p25.md](p25.md) 5 |
| 2400 bit/s, 48 bits in a 72-bit frame | `mbe2400*.c`, `mbe2400.h` | `CONFIG_MBE2400` | [mbe-2400.md](mbe-2400.md) |

`CONFIG_MBE` builds the core, on with either front end ([build-config.md](../build-config.md)). No heap, no
float, no libm. The memory a call needs is placed by the caller; the scratch can be shared with other code.

## 1. The model

A frame is 20 ms (160 samples at 8 kHz) of parameters (`mbe_params_t`):

| Field | What |
|---|---|
| `L` | harmonics, 0 to 56; 0 is a silent frame |
| `w0` | the fundamental as a phase step per sample, 2^32 a turn |
| `a[l]` | harmonic l + 1's amplitude: log2 in Q11 of its level in PCM units. 15 * 2048 is a full-scale sinusoid |
| `v[l]` | harmonic l + 1 voiced (1) or unvoiced (0) |
| `flags` | how the core reads the frame (1.1) |

### 1.1 Options a front end chooses

| Flag | Effect |
|---|---|
| `MBE_NO_ENHANCE` | no spectral amplitude enhancement: the front end already enhanced the amplitudes |
| `MBE_UV_DENSITY` | an unvoiced amplitude is a spectral density over the whole band: the band's noise power is the amplitude's times w0 / 2^32, the harmonic's share of the spectrum. Without the flag an unvoiced amplitude is the band's level, like a voiced one's. The estimator reads the flag too, so an encoder codes what its decoder plays |
| `MBE_PHASE_REGEN` | voiced phases from the amplitudes (1.2), not random offsets scaled by the frame's unvoiced share |

The last two are built, and defined, only with a front end that sets them: `CONFIG_MBE_OPTS`, derived in
`core/config.h`, on with `CONFIG_MBE2400`. A build with the full-rate front end alone leaves their code out. Another
front end that sets one adds its switch to `CONFIG_MBE_OPTS`; without that the flag is not defined and the build stops.

### 1.2 Regenerated phases

Harmonic l's phase offset is a share of the minimum phase the spectral envelope implies, plus a fixed dispersion:

th_l = 0.44 / pi * sum over m = 1..8 of (A_{l+m} - A_{l-m}) / m radians + 0.006 * l^2 turns

A is the natural log amplitude, held at the first and last harmonic past the ends and floored 72 dB under the frame's
loudest. The sum is a short discrete Hilbert transform of the log envelope: a harmonic on a rising slope leads, a
harmonic on a falling slope lags, as through the resonance that shapes the envelope. Through a two-pole resonance
the phases are about a third of the filter's own. The dispersion term spreads the pulse the harmonics otherwise make
once a period, which is sharper than speech's and sounds as a buzz.

| Choice | Measured on the test voices |
|---|---|
| The sign and the share 0.44 | the output's waveform follows the input's best (normalized correlation over voiced frames 0.86 and 0.78); the opposite sign 0.82 and 0.76, three times the share 0.84 and 0.77, no such term 0.84 and 0.77 |
| The dispersion 0.006 | the crest factor within 1 dB of the input's; without the term 1.5 dB over, with twice the term the correlation falls to 0.82 and 0.73 |

The phases follow the envelope from frame to frame and do not depend on the noise seed.

## 2. Synthesis (`mbe_synth.c`)

`mbe_synth_frame` takes one frame and writes 160 samples:

| Step | What |
|---|---|
| Enhancement | the amplitudes are weighted by the envelope's shape, the frame's energy kept (TIA-102.BABA) |
| Voiced part | an oscillator per voiced harmonic, amplitudes and frequency moved linearly across the frame from the last frame's |
| Unvoiced part | 320 samples of white noise under a Hann window, its spectrum set band by band to the unvoiced harmonics' levels, back to time, overlap-added |
| Output | a DC blocker, clipped to 16 bits |

### 2.1 Concealment

A front end passes a bad frame as `bad` (or `in` NULL). The core repeats the last good frame, then plays silence.
`mbe_synth_frame` repeats up to `MBE_REPEAT_MAX` (4) frames at full level. `mbe_synth_conceal` takes a stream's own
policy (`mbe_conceal_t`), in every build: up to `repeat_max` repeats, each `fade` (log2 Q11) quieter than the one
before. Both return the count of bad frames in a row; past the repeats the front end resets its prediction state.

A frame with L 0 is a good frame that plays silence: a repeat after such a frame is silent too. A front end that
conceals another way (a frame of its own making in place of the lost one) passes that frame as a good one.

## 3. Analysis (`mbe_est.c`)

The estimator finds the model's parameters in 20 ms of speech. The steps are in [p25.md](p25.md) 5.1.

| Call | What |
|---|---|
| `mbe_est_frame` | the high-passed window's spectrum and a pitch period from the autocorrelation |
| `mbe_est_pitch` | the front end's pitch code (its `mbe_grid_t`: the code of a period, the fundamental of a code, the count of codes, the shortest period): the code nearest the period, a neighbor the spectrum fits better, or the code of half the period when the spectrum fits that as well. -1: no pitch |
| `mbe_est_params` | voicing and amplitudes at the chosen fundamental, for bands of a count of harmonics (the full-rate front end: 3) |
| `mbe_est_params_hz` | the same for bands of a width in Hz from 0 Hz (the 2400 bit/s front end: four of 1 kHz) |

The last band takes every harmonic above; at most 16 bands. A band is voiced when the window's transform at each of its
harmonics explains the band's bins to within a threshold.

## 4. Shared by the quantizers (`mbe_q.c`)

`mbe_predict` gives the prediction both ends of a link compute from the last frame's log amplitudes (`mbe_pred_t`):
rho times those amplitudes resampled to this frame's harmonic count (linear between neighbors), mean removed. Each
front end sets rho and the value at harmonic 0.

## 5. Tests

| Test | What |
|---|---|
| `tests/host/mbeTest.c` | the synthesis and the full-rate decoder against golden vectors; silence, concealment, a stream's own concealment policy, levels, extreme inputs |
| `tests/host/mbeExactTest.c` | the full-rate encoder and decoder sample for sample against fixed CRCs (speech, random frames, channel errors), with and without the halfword multiplies. Built again with the per-frame options (`CONFIG_MBE_OPTS`): the full-rate CRCs stay the same, and the 2400 bit/s codec's own CRCs hold both ways |
| `tests/host/imbeEncTest.c` | the full-rate quantizer against its dequantizer; the estimator on made-up signals |
| `tests/host/mbe2400Test.c` | the 2400 bit/s front end ([mbe-2400.md](mbe-2400.md) 6); the density option; the regenerated phases against 1.2's formula and against a resonator's own phases; parameters no front end makes |
