<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR voice codec

A software codec for D-STAR digital voice: AMBE 3600x2400, 160 samples of 8 kHz speech to one 72-bit voice frame
every 20 ms and back, in integer C with no heap, no globals and no RTOS calls. It fills the codec interface of the DV
layer, `dstar_vocoder_t` in `dstar_voice.h` ([dstar-modem.md](dstar-modem.md) 4), so a radio without a vocoder chip
can talk D-STAR.

Code: `core/dstar`, built with `CONFIG_DSTAR` ([build-config.md](../build-config.md)):

| File | Part |
|---|---|
| `dstar_codec.c` | the public interface: one state for both directions, the bad-frame rule (1) |
| `dstar_mbe_analysis.c` | speech to MBE model parameters (2) |
| `dstar_mbe_level.c` | the encoder's speech leveler, between the analysis and the quantizer (2) |
| `dstar_ambe_quant.c`, `dstar_ambe_tables.inc` | parameters to the nine frame fields and back (3) |
| `dstar_mbe_synth.c` | parameters to speech, and the concealment of bad frames (4) |
| `dstar_ambe_frame.c` | the fields to a 72-bit frame with FEC and back (5, [dstar-ambe-frame.md](dstar-ambe-frame.md)); the Golay coder and whitening are `core/dmr/ambe/ambeFec.c`'s |
| `dstar_mbe_fixed.c`, `dstar_mbe_tables.inc` | the fixed-point helpers and tables of analysis and synthesis |

Tests: `tests/host/dstarCodecTest.c` and `tests/host/dstarCodecQualityTest.c` (`make -C tests/host run-dstar`, 7).

## 1. Interface

```c
void dstar_codec_start(dstar_codec_t *c, bool encode);
bool dstar_codec_encode(void *ctx, const int16_t pcm[160], uint8_t frame[9]);
bool dstar_codec_decode(void *ctx, const uint8_t frame[9], int16_t pcm[160]);
void dstar_codec_lost(dstar_codec_t *c, int16_t pcm[160]);
void dstar_codec_vocoder(dstar_codec_t *c, dstar_vocoder_t *v);
uint8_t dstar_codec_errors(const dstar_codec_t *c);
```

| Call | What |
|---|---|
| `dstar_codec_start` | (re)initializes the direction about to run. D-STAR is half duplex and one mode runs at a time, so encoder and decoder share one union; the other direction's state is gone |
| `dstar_codec_encode` | one frame from 160 samples. The output lags the input by 60 ms (2). False, doing nothing, when started for decoding |
| `dstar_codec_decode` | 160 samples from one frame, with its own bad-frame rule and concealment (below). False when started for encoding |
| `dstar_codec_lost` | a frame that never arrived: concealed as a bad one |
| `dstar_codec_errors` | the last frame's bit errors corrected in C0 and C1 (0..7: a C1 that failed counts 4), or `DSTAR_CODEC_FAILED` (0xFF) when the frame was concealed or lost, for the debug link and a bit error rate. The 24 unprotected bits can't be counted |

`dstar_codec_t` is defined in full in `dstar_codec.h`, so a caller can place it in a region it lends to the running
mode. Everything a call needs, its working memory included, is in it (6).

A frame is **bad** when C0 failed (4 or more errors detected), or when the frame before it was bad or lost and C0 and C1
needed 4 or more corrections between them (a C1 that failed counts 3, what the 23-bit decoder would correct). Every
other frame is decoded, a C1 that failed included: its data is then the decode of its 23-bit Golay word without the
parity bit (5).

| Rule | At 5 % random bit errors (research, 12 inputs, 3 seeds): frames concealed, MCD, STOI |
|---|---|
| mbelib's: more than 3 corrections in C0 and C1 together (a failed C0 counts 4) | 18 %, 29.2 dB, 0.742 |
| Any word with 3 corrections, or C1 failed, also bad | worse: 27.7 dB and 26.0 dB |
| C0 failed only | 2 %, 25.8 dB, 0.796 |
| C0 failed, or 4 or more corrections right after a bad frame (this codec) | the same for random errors; in fades (25 % errors for 80 ms on average) 27.9 -> 27.6 dB, STOI 0.762 -> 0.764 |

mbelib's rule throws away frames the Golay decoder got right; a corrected frame with a few wrong unprotected bits
sounds better than a repeat. In a fade most frames are garbage that passes C0's check by chance, which is what the
second clause catches; 4 corrections right after a loss are rare without a fade.

A bad or lost frame is **concealed** (`dstar_mbe_synth_bad`): the last good frame repeated 1 dB quieter each time for
three frames, then its spectrum as unvoiced noise (comfort noise) still fading 1 dB a frame, and at the 29th bad frame
in a row (0.56 s) silence and a reset of the synthesizer and the prediction memory, as mbelib resets after its mute, so
both ends restart from the same state. mbelib repeats at full level three times and then cuts to silence: a held buzz,
then a click. A concealed frame leaves the prediction memory where it was. A tone or reserved frame (b0 126 or 127)
plays silence and resets.

## 2. Analysis

`dstar_mbe_analyze`, a fixed-point MBE analyzer (Griffin and Lim's model). Per 20 ms:

| Step | What |
|---|---|
| History | a 20 Hz DC blocker, then a 640-sample history |
| Pitch error | E(P) of the newest frame on a half-sample grid, 20 to 127 samples, from a 1 kHz low-passed copy: a normalized difference summed over the multiples of P, each lag normalized by the energy of the two segments it compares (as YIN does). Griffin and Lim's whole-window normalization gave E > 1 at the right pitch on 10-15 % of voiced frames (a quiet frame between loud ones) |
| Tracking | the frame two frames back, from E of it, two frames of look-ahead and two of look-back, with a check for pitch doubling (the structure of the expired MBE pitch-tracking patents) |
| Refinement | a 256-point FFT of that frame (221-point Hamming window); the pitch refined to 1/8 sample by fitting harmonics |
| Voicing | per 1 kHz pair of bands (the voicing field's resolution): the fit's residual against a measured white-noise baseline, scaled by the low band's periodicity and its loudness against the upper bands |
| Magnitudes | f0 snapped to the D-STAR pitch grid, band energies measured at those harmonics, converted to mbelib's log2 magnitude scale (derived, not swept: `dstar_mbe_analysis.h`); unvoiced bands in DVSI's convention, a spectral density (4), 4.5 dB over it |
| DVSI match | each frame's log magnitudes compressed to 0.6 about their mean plus DVSI's balance per 500 Hz band (-5.2 dB below 500 Hz, +1.4 to +2.1 dB at 1-3 kHz), the frame's power kept; all-unvoiced frames sent at b0 34, voiced pitch never above b0 119 (below) |
| Level | `dstar_mbe_level`: one gain added to every log magnitude, so speech goes out at one level in the speech band (below 500 Hz weighted down) whatever the microphone's, and no frame over a ceiling 10 dB above it (below) |

**Matching a DVSI encoder.** Our frames were coded correctly to the bit, yet an Icom ID-51 (a DVSI decoder) played them
audibly but unintelligibly, while it plays an IC-9700's DVSI frames fine. Read with mbelib's tables, DVSI frames have a
much flatter spectrum than real speech through this analyzer: voiced harmonics at 1-3 kHz about 15 dB under the
strongest, against 30-40 dB here. So DVSI's analysis is flatter, and its decoders put the depth back, over-deepening
ours. On the ID-51 (18 Nov 2026, real male speech, one listener): full depth 3-4 out of 5 (1 out of 5 with a synthetic
voice); 0.5, 0.55 and 0.6 with power kept 4, a little better, and 5 out of 5, and the operator's own voice at 0.6
"wasn't bad"; without the power kept, intelligible but quiet. The decoder puts depth back (4), by 1/0.73 rather than the
exact inverse. The factor is one
number for every band (`dvsi_shape_q12`, per 500 Hz band): comparing the same audio through an IC-9700 and through us
frame by frame (18 Nov) found the compression the same in every band, and a balance across the bands on top of it
(`dvsi_eq_q12`), left off at first. It is on since 21 Nov (below), re-measured that day against the current encoder:
-5.2 +0.3 +1.8 +2.1 +1.4 +1.6 -1.7 -4.9 dB. Our decoder doesn't undo it, so on our own round trip it costs STOI
0.866 -> 0.810 (the 500 Hz cut alone 0.820); an ID-51 and one of our own radios hearing the same over decide whether to keep all of
it. `flat` in the analyzer's state leaves it out, for the quality tests (7). The pitch conventions are what every IC-9700 frame
showed (347 of speech, 490 of whistle and hum): on their own they did not make our frames intelligible, but they keep
us inside what DVSI decoders are known to receive. `full_depth` in the analyzer's state turns the match off, for tests
of the measurement.

**Leveling the speech** (19 Nov 2026). A DVSI decoder plays loud frames into a hard ceiling: through an IC-9700
(a Pluto sending our frames, the 9700 decoding, 18 takes) a loud talker at one fixed level reached it (about 26,900;
18 to 1,352 samples over 24,000 a take, heard as distortion) while a quiet reader never passed 19,200. The level our
frames carry, read as a DVSI decoder plays them (log magnitudes expanded 1/0.73 about their mean, power not kept:
`dstar_mbe_level_of`), predicts the 9700's output level within 1.5 dB, so the leveler controls that level, measured on
the parameters rather than the microphone. Per frame: a background estimate follows the level down at once and up at
0.05 dB a frame; a frame 15 dB over it is speech; the speech level follows speech frames up by 1/4 of the difference
(about 80 ms) and down by 1/64 (about 1.3 s) and holds between words; the gain is the target minus it, within +-20 dB,
and never more than brings the frame to 10 dB over the target, so a loud onset is capped at once rather than after the
attack. The gain goes on after the DVSI match, before the quantizer, so the quantizer's gain index codes the leveled
frame. On air through the same loop (three sources, each at its own level and 10 dB under and 6 dB over it) every
source came back within about 1.5 dB over that 16 dB range; a clean voice and a LibriSpeech reader never reached the
ceiling, a voice recorded through a messaging app's compressed audio touched it on 10 to 17 samples a take. The target
is 1 dB over that test's (the reader sat 2 dB under its best fixed level; +2 dB left the compressed voice's loudest
frames no margin under the take that clipped, +1 dB 1.1-1.5 dB). The frames are the research encoder's bit for bit.

**Leveling the speech band** (21 Nov 2026). Icom handhelds played us quietly. Offline, decoded through mbelib, in the
1-3.4 kHz band a handheld's speaker reproduces our overs were no louder than an ID-51's own and about 15 dB under an
IC-9700's; our extra level was all below 500 Hz, and the full-band measure let that bass set the gain. So the measure
now weights each harmonic below 500 Hz by (f / 500)^2 (a second-order high-pass), the DVSI balance above cuts 5 dB
below 500 Hz, and the target is 6 dB higher (`LV_LOUDER`), the first of two steps, each followed by a listen on an
ID-51 and the 9700's speaker; the next makes it 12 dB. The 9700's LAN-audio ceiling that set the 19 Nov target is not
what a handheld user hears, and loud talkers are expected to reach it. The gain's top moves from +20 to +30 dB so the
quiet talkers the leveler brought up before still reach the higher target.
`off` in the leveler's state passes frames unchanged, for the quality tests (7), whose figures were measured without
it. On air on 21 Nov 2026 (one of our own radios on a build with this codec, an IC-9700 decoding over its LAN
audio, an ID-51 beside it): the speech band came up 12.3 dB (-45.2 to -32.9 dBFS; the whole band -30.4 to -20.5),
above an ID-51 over's -40.0 in the same session, and the operator heard it louder on the ID-51. The peak, 26,409, sits
just under the 9700's ceiling, so the second step is not taken.

Delay: the parameters belong to the frame centered 480 samples (60 ms) before the end of the newest input.

## 3. Quantizer

`dstar_ambe_quantize` and `dstar_ambe_dequantize`: parameters to the nine fields b0..b8 (48 bits) and back. The
bitstream is mbelib's decoder (`mbe_decodeAmbe2400Parms`); the dequantizer restates it in integer arithmetic and the
quantizer is its inverse.

| Field | Bits | Decoded as |
|---|---|---|
| b0 | 7 | f0 = 2^(-4.3118 - 0.021336 (b0 + 0.5 - 1.5)), L from a table; 126 and 127 are tone and reserved frames |
| b1 | 4 | voicing of eight 500 Hz bands from a table |
| b2 | 6 | gain: gamma = Dg[b2] + gamma_prev / 2 |
| b3, b4 | 9, 7 | PRBA vectors G2..G4, G5..G8 -> an 8-point DCT -> the first two DCT coefficients of four blocks |
| b5..b8 | 4, 4, 4, 3 | higher-order coefficients 3..6 of the blocks (b8's low bit is not sent) |

**The pitch grid is DVSI's, not mbelib's.** mbelib's own comment calls its pitch formula a guess. An IC-9700 sent
`ground-truth.wav` (bench) through its DVSI chip twice, at audio levels 6 dB apart (18 Nov 2026): vowels at 100, 125,
160 and 200 Hz came out at b0 95, 80, 64 and 48, where mbelib's formula gives 93.7, 78.6, 61.9 and 46.8, and an
80-250 Hz glide fitted b0 = 0.98 x mbelib's + 2.9 over 195 frames. 1.5 steps on average, the same in both takes: index
b0 means mbelib's f0 at b0 - 1.5, 2.2 % higher. Read with mbelib's grid, a DVSI station sounds 2.2 % low; coded with it,
we sounded 2.2 % high on a DVSI radio. Everything else in the frame matched mbelib's decoder (the voicing table indexed
at the shifted pitch).

The blocks (lengths J1..J4, summing to L) are inverse transformed to a shape T, and
log2 M_l = T_l - mean(T) + P_l - mean(P) + gamma - log2(L) / 2, with P 0.65 times the previous frame's log magnitudes
interpolated onto the new harmonics. The encoder keeps its own copy of the decoder's memory (closed loop).

The quantizer, in order:

| Step | What |
|---|---|
| Pitch, voicing | pitch nearest in log frequency; voicing by the amplitude it would label wrongly |
| Gain, first pass | exactly from the frame mean |
| PRBA search | a weighted quadratic form built once a frame (64-bit setup, then a small matrix-vector product per candidate, the trick of CELP codebook searches): all 512 PRBA24 candidates, the best 4 carried into a full search of the 128 PRBA58. Block weights are the square root of the mel scale's resolution at the block's center (mean MCD 25.04 dB against 25.20 dB unweighted, in the research's comparison) |
| Higher-order coefficients | plain squared error per block, b8 restricted to even indices |
| Perceptual re-ranking | the 16 best PRBA pairs of the search are scored under a band-energy measure, and the best 3 of those get their higher-order vectors re-chosen block by block under it (below) |
| Gain match | b2 chosen again after the shape (below) |

The **band-energy measure** is the error in the log energy of 24 triangular mel bands (100 Hz to 3.8 kHz, the bands of
the MCD measure), each harmonic's share of a band's energy taken from the target's spectrum, each band weighted by
(its energy / the frame's peak)^0.2 with a floor at -55 dB, the frame mean left out (that is the gain's). It is the
mel-cepstral distance's leading term without c0. Plain squared log-magnitude error counts a 3 dB error on a harmonic
50 dB down in a valley the same as one on a formant peak; the ear, and MCD, don't. Every decoded quantity is linear in
the block DCT coefficients, so the frame setup computes each coefficient's band response once (at most 24 columns of
24) and a candidate costs one 24-point combination. Research figures (12 inputs, mean MCD / STOI): plain search 22.51
dB / 0.855, re-ranked 21.70 dB / 0.865, unquantized 20.70 dB / 0.891. Re-ranking under the plain error gained nothing
(22.49 dB): the gain is the measure's. Refining all 8 or 16 pairs, a second higher-order pass and more PRBA24
survivors each changed MCD by under 0.05 dB.

The **gain match**: the decoder fixes the frame's mean log magnitude from the gain alone, so a coarse shape, which
flattens peaks, loses power with the mean held (0.3 to 1.4 dB on the test speech). The quantizer decodes its choice
once on a copy of the prediction memory and re-chooses b2 to make up half of that loss in log terms: STOI 0.8606 ->
0.8632 (the full correction gave 0.8592, overshooting where the shape was already right). The prediction memory then
holds the corrected gain at both ends.

## 4. Synthesis

`dstar_mbe_synth_frame`, an integer port of mbelib's `mbe_spectralAmpEnhance`, `mbe_synthesizeSpeech` (uvquality 3) and
a concealment of its own (1). Phases are uint32 (2^32 one cycle), so they wrap exactly where mbelib's floats lose
precision over a long transmission; each frame's amplitudes share one exponent, so the sample loop is 16 x 16-bit
products with no 64-bit arithmetic. Departures from mbelib, all measured:

| Departure | Why |
|---|---|
| No high-band random term | mbelib adds a uniform random value with a non-zero mean to every unvoiced band above 2.7 kHz, a wandering DC offset and broadband noise (about 1.4 dB of MCD) |
| Enhancement before the unvoiced scale | mbelib scales unvoiced bands by 0.2046 / sqrt(w0) first, so its enhancer sees them as valleys and cuts them (about 0.1 dB of MCD) |
| Concealment | fading repeats, then comfort noise, then silence, instead of three full-level repeats and a cut (1). Repetition with fading and noise substitution are the textbook packet-loss remedies (Perkins, Hodson and Hardman, IEEE Network 12(5), 1998); the constants (3 repeats, 1 dB a frame, 25 frames of noise) are the best of the research's sweep at 1, 3 and 5 % bit errors |
| Shape expanded to 1/0.73 | depth put back after the analyzer's DVSI match (2) and a DVSI encoder's flatter analysis, power kept. 1/0.6 (the exact inverse of our 0.6) until 19 Nov 2026; measured on both kinds of frame, 1/0.73 took our own round trip on the research's 12 inputs from MCD 22.96 to 20.75 dB and STOI 0.861 to 0.864 (10 of 12 inputs gain; flite slt loses 1.25 dB), and an IC-9700's DVSI frames of known speech from STOI 0.610 to 0.647 (MCD 66.5 to 62.2 dB). 0.73 is DVSI's harmonic spread against our full-depth analysis on those frames. No expansion at all suits DVSI's frames best (STOI 0.694) but flattens ours (MCD 33.6) |
| Voiced harmonics interpolated | a harmonic whose pitch moved under 10 % is one sinusoid with its amplitude and frequency interpolated across the frame (the IMBE interpolation mbelib leaves commented out), not a cross-fade. IMBE limits it to the 7 lowest; all measured better (MCD -0.15 dB, STOI +0.003) |
| Voiced phase from the envelope | mbelib re-randomizes the upper harmonics' phases every frame, in proportion to the unvoiced band count, heard as roughness across the cross-fade. Here the minimum phase the envelope implies (a 19-term discrete Hilbert transform of the log magnitudes, gamma 0.44; regenerating MBE phase from the magnitudes is published, US 5,701,390, expired). It halved the 50/100 Hz modulation of the output envelope on real DVSI frames |
| Unvoiced bands as noise | mbelib's three fixed sines a band sound tonal; here a random-phase spectrum over the unvoiced bins (magnitudes interpolated between band centers), a 256-point inverse FFT and power-complementary windows aligned with the voiced cross-fade, at the multisine's power. STOI +0.009; cheaper too |
| Soft limiter | real frames peak above full scale on loud vowels (291 clipped samples in 7 s of an IC-9700's); a knee from -2.5 dBFS instead of the hard clip |
| Voiced phase dispersed | a fixed 0.012 l^2 of a cycle on top of the envelope's phase (the same every frame): the minimum phase lines the harmonics up into one sharp pulse a period, heard as buzz. Voiced-frame crest factor against an IC-9700's decode of the same DVSI frames 9.7 dB to its 8.4; on DVSI frames of known speech 12.0 dB to the source's 10.1, and 9.8 with the dispersion (3 to 40 thousandths measured). Ground-truth STOI 0.686 -> 0.696, the rest unchanged |
| A noise floor under voiced harmonics | from 1 kHz up, 17 dB under each harmonic's power, in the unvoiced noise's inverse FFT: our voiced harmonics stood 4.5 dB (1-2 kHz) and 2.5 dB (2-3.5 kHz) further out of the gaps between them than an IC-9700's decode of the same frames, and 5 dB further than the source's on DVSI frames of known speech, which is a vocoder's buzz; with the floor 2.1 and 0.7 dB. -15 dB matched the 9700 at 1-2 kHz but cost our round trip 0.15 dB of MCD; -17 dB costs 0.08 (STOI +0.0007). Cross-fading the upper harmonics instead (mbelib's way) overshot |
| Output 4.4 dB down | mbelib's output gain of 7 was never matched to a DVSI decoder. Over an ID-51's live over, ours played 4.4 dB louder than an IC-9700's decode of the same frames (its LAN audio) over active speech, and was heard as "slightly louder"; now matched. Our own round trip plays 4.4 dB under its input |
| Unvoiced bands as a density | DVSI codes an unvoiced band as a spectral density; mbelib plays it as a band amplitude (band power M^2 / w0 against a density's M^2 w0), too loud by a factor that grows as the pitch falls. Against an IC-9700's own decode of the same 681 DVSI frames (an ID-51 talking, BER 0): all-unvoiced frames +6.6 dB re voiced ones, DVSI's background frames (b0 120, 69 Hz) +13 dB, frames coded all unvoiced at a voice pitch +10 dB; DVSI frames of known speech said the same against their source (+8.7 dB at b0 34, +11.5 dB at a voice pitch). Here the unvoiced amplitude is mbelib's times w0 / w0(b0 34), and the noise 6 dB down; the analyzer codes the same law, 4.5 dB over (2). Against the 9700's decode STOI 0.763 -> 0.795 and LSD 12.8 -> 10.3 dB; ground truth STOI 0.646 -> 0.686; our own round trip MCD 20.75 -> 20.28 dB, STOI 0.864 -> 0.865, and white noise comes back 1.5 dB quieter |

The noise source is xorshift32. With the first two changes undone and mbelib fed the same random numbers, the research
measured its synthesizer against mbelib's rebuilt in double at 54.9 to 56.4 dB SNR on the four test voices (the research
keeps an mbelib mode for that; this port has only the path above, which matches the research's output sample for
sample). The last four (18 Nov 2026), on the research's 12 inputs: STOI 0.865 -> 0.876, MCD 21.70 -> 21.51 dB, and
higher STOI at every channel error rate (1 %: 0.852 -> 0.865, 3 %: 0.825 -> 0.843, 5 %: 0.778 -> 0.797, fades: 0.744 ->
0.753); on 357 voice frames off air from an IC-9700, frame-rate modulation 0.183 -> 0.104 (mbelib 0.138) and clipped
samples 291 -> 0. MCD rose on two female readers (f1 22.93 -> 23.34 dB), where the multisine's sine at the harmonic had
masked our analyzer's voicing errors.

## 5. Frame FEC and bit order

`dstar_ambe_pack` and `dstar_ambe_unpack`. The frame is the DMR full-rate frame's structure (`core/dmr/ambe/ambeFec.h`)
with another interleave and parameter map:

| Word | Bits | Content |
|---|---|---|
| C0 | 24 | extended Golay(24,12) of parameter bits d0..d11 |
| C1 | 24 | extended Golay(24,12) of d12..d23, XORed with 24 steps of the whitening sequence seeded from C0's data (x0 = 16 data, x' = 173 x + 13849 mod 65536, a bit = bit 15) |
| C2 | 10 | d25..d34, unprotected |
| C3 | 14 | d35..d48, unprotected |

d0..d48 is mbelib's parameter vector; its d24 is C1's overall parity bit, not a speech bit, and the map from it to
b0..b8 is mbelib's decoder's. The 72 bits reach the air through an interleave no public source gives, derived from
off-air frames in [dstar-ambe-frame.md](dstar-ambe-frame.md) (4, 5), which has the full layout. Golay generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1, the DMR coder's
(`ambeGolay24Encode`, `ambeWhitening`); the decoder corrects 3 errors a word and detects 4. A C1 that failed is passed
on as its 23-bit word decodes without the parity bit, as mbelib reads C1: right whenever the parity bit was one of
the four errors, and the codec plays such a frame (1).

**Bit order.** Frames are in `core/dstar`'s air order: byte 0 first, each byte least significant bit first. The
standard's lost-packet frame `9E 8D 32 88 26 1A 3F 61 E8` (JARL 6.6) proves it: read that way both Golay words decode
with no corrections and it packs back bit for bit; read with each byte most significant bit first, C0 has 3 errors
and C1 fails. It decodes to b0..b8 = 124 0 4 165 41 10 7 7 10: a 64 Hz pitch, every band unvoiced, a small gain step
(gamma 0.59); through the decoder it plays at a peak of 56 (near silence). The research code packed air bit 0
into the most significant bit of byte 0; this tree converts at the boundary.

**C1's parity.** mbelib reads C1's parity bit as the first bit of C2 and ignores it, and whitens only 23 bits. Here the
parity bit is whitened with the sequence's 24th step. The lost-packet frame doesn't settle that (its 24th step is 0); a
real DVSI encoder does: 347 voice frames off air from an IC-9700 (18 Nov 2026) all decode with no corrections and pack
back bit for bit, parity included (`dstarCodecTest` carries four of its background frames). mbelib reports fewer
corrections than this decoder on the same frames because it counts only corrected data bits, not because it reads
them differently.

## 6. Cost

Measured on C-SKY CK803S (csky-elf-gcc 14.2, the firmware's flags: `-Os`, function and data sections, garbage
collected at the link).

**RAM**: one `dstar_codec_t` (the state, 3,168 B) and one `dstar_codec_scratch_t` (each call's working memory, 5,304
B), no other static data. The encoder and decoder share a union in each. The two are apart because a radio keeps them
in different places: the working memory fits the DMR vocoder's state RAM, which the HR_C7000 radios lend while DMR is
off (`hal_vocoder_t.lend`, 7,168 B since the vocoder's encoder and decoder share one block), and the whole codec
(8.4 KB) doesn't; the state goes in the radio core's mode area (`radio_mode_ram.h`). Nothing in the working memory
outlives a call, so it needs no saving when DMR takes the block back.

| Part | Encoder (B) | Decoder (B) |
|---|---:|---:|
| Analysis state (input history, three frames of pitch error) | 2,604 | |
| Leveler state (speech and background levels, the gain) | 16 | |
| Synthesis state (phases, previous frame, the last good frame for repeats, the unvoiced noise's fade-out) | | 1,468 |
| Prediction memory | 236 | 236 |
| Parameters between the stages, the bad-frame rule's flag | 292 | 296 |
| Per-call scratch, shared by the two stages of a direction | 5,248 (the analysis's FFT and pitch error; the quantizer needs 3,396) | 5,300 (the synthesizer's, 2 KB of it the unvoiced noise's FFT; the dequantizer needs 3,396) |
| **Total** | **8,396** | **7,300** |

The perceptual re-ranking and the gain match added 1,944 B of working memory to the quantizer (the band responses
1,508 B, the setup's per-band and per-harmonic values, which take the dead quadratic form's place, and a copy of the
prediction memory for the trial decode, 236 B), and the concealment 292 B to the synthesizer (the concealed frame's
parameters). Both went into the per-call scratch rather than the stack: the encoder's scratch is sized by the analysis,
so `dstar_codec_t` didn't grow, and the research versions' stack (2.4 KB, then 4.6 KB with this work) stays out of the
audio task. The synthesizer's changes of 18 Nov 2026 (4) added 420 B of state and 3.3 KB of scratch (the noise's
256-point FFT, its bins and the envelope's phases); the decoder is still smaller than the encoder, so `dstar_codec_t`
didn't grow either. The research synthesizer keeps those arrays on the stack (5.0 KB worst case); here they never are.

**Stack**, worst case per entry point from `-fcallgraph-info` (libgcc helpers charged 64 B):

| Entry | Bytes | Deepest path |
|---|---:|---|
| `dstar_codec_encode` | 1,288 | the quantizer's frame (1,040 B: its searches, the perceptual pass inlined, the 16 pairs' list and two band residuals), then its closing decode |
| `dstar_codec_decode` | 248 | concealment, synthesis with the noise's FFT inlined |
| `dstar_codec_lost` | 212 | the same |
| `dstar_codec_start` | 28 | |

The research versions needed 4.6 KB (the quantizer's quadratic form and band responses on the stack) and 1,720 B (the
synthesizer's arrays); their big arrays are in the scratch above. On the audio task of the first board to run it (6,144 B, sized for
the stock codec's call; [feature-matrix.md](../feature-matrix.md)) the D-STAR chain is 1,452 B with the port overhead (`audio_task > block > radio_dstar_audio_block >
dstar_codec_encode`); the task's deepest chain is still the DMR codec's, 4,340 B, so the task keeps its size.

**Flash**, linked:

| Object | Code (B) | Tables (B) |
|---|---:|---:|
| `dstar_ambe_quant.o` | 7,230 | 7,294 |
| `dstar_mbe_analysis.o` | 4,180 | 136 |
| `dstar_mbe_synth.o` | 3,700 | 40 |
| `dstar_mbe_fixed.o` | 160 | 4,642 |
| `dstar_ambe_frame.o` | 608 | 193 |
| `dstar_codec.o` | 428 | |
| `dstar_mbe_level.o` | 336 | |
| **Total** | **16,662** | **12,305** |

28.9 KB in all (the leveler 336 B, 1.3 KB of it the synthesizer's changes and the DVSI match of 18 Nov 2026, 3.6 KB the perceptual re-ranking and gain match, the mel band edges 104 B of the tables), plus `ambeFec.o`'s Golay(24,12) and whitening (506 B) and libgcc's 64-bit divide and multiply
(1,030 B) and count-leading-zeros table (256 B) when the image doesn't have them already (a DMR image does). No floating point: the linked image has no
soft-float or libm symbol. The 64-bit arithmetic is once a frame, in the quantizer's setup. Two tables are near
duplicates across the parts (the pitch grid, 1 KB, differing by 1 LSB in a few entries); sharing them would change the
analyzer's arithmetic slightly and is left for a measured change.

## 7. Quality

MBE vocoders don't keep the waveform's phase, so the measures are spectral, after aligning the signals on their
frame-energy envelopes: LSD (log-spectral distance, dB RMS over 0-4 kHz, active frames), MCD (mel-cepstral distortion,
24 mel bands 100-3800 Hz, 13 coefficients without c0) and level, from 256-point Hann frames every 80 samples. The
research added STOI (short-time objective intelligibility, Taal et al., IEEE TASLP 19(7), 2011, narrowband; 0 to 1,
higher is better). They rank variants and catch regressions; they are not a MOS.

On CMU Flite's four voices reading the same text (synthetic speech, cleaner than a microphone):

| Voice | This codec: LSD / MCD (dB) | Before the perceptual re-ranking and gain match | Unquantized ceiling, same synthesis |
|---|---|---|---|
| slt | 10.37 / 22.87 | 10.39 / 24.11 | 10.32 / 22.14 |
| kal | 8.62 / 21.60 | 8.64 / 23.10 | 8.21 / 20.32 |
| awb | 8.62 / 20.73 | 8.57 / 21.06 | 8.49 / 19.92 |
| rms | 9.19 / 24.22 | 9.02 / 24.95 | 8.87 / 23.09 |
| **Mean MCD** | **22.36** | 23.31 | 21.37 |

(Before the synthesizer's changes and the DVSI match of 18 Nov 2026; both are below.)

Through unmodified mbelib's float decoder (a third-party receiver), the same voices' mean MCD went from 25.07 to
24.14 dB with this quantizer.

The research also measured 8 real recordings (LibriSpeech readers at normal and fast pace and very quiet, one in a
reverberant room with a television at 10 dB SNR, one in a car at 5 dB SNR; CC BY 4.0 and CC0, kept in the research tree,
never here), MCD / STOI:

| Inputs | Before | This codec | Unquantized ceiling |
|---|---|---|---|
| 4 flite voices | 23.31 dB / 0.894 | 22.36 dB / 0.907 | 21.37 dB / 0.933 |
| 8 real recordings | 22.11 dB / 0.836 | 21.37 dB / 0.844 | 20.36 dB / 0.869 |
| All 12 | 22.51 dB / 0.855 | 21.70 dB / 0.865 | 20.70 dB / 0.891 |

Every input improved in STOI; MCD improved on 11 of 12 (f1, a female reader, 22.87 -> 22.93 dB). The noisy room and car
recordings stay the hardest (STOI 0.69 and 0.62), as much from the analysis as the quantizer (their ceiling is 0.75 and
0.65).

**Channel errors** (research: 12 inputs, 3 seeds; fades a two-state channel, 25 % bit errors for 80 ms on average, 11 %
of the time, 0.1 % otherwise). Before: the plain quantizer with mbelib's rule and concealment; after: this codec:

| Channel | Before: MCD, STOI, concealed | After: MCD, STOI, concealed |
|---|---|---|
| 1 % bit errors | 23.08 dB, 0.845, 0.1 % | 22.31 dB, 0.855, 0.0 % |
| 3 % | 25.20 dB, 0.808, 4.7 % | 23.64 dB, 0.829, 0.6 % |
| 5 % | 29.71 dB, 0.735, 18.1 % | 25.87 dB, 0.795, 2.4 % |
| Fades | 28.88 dB, 0.739, 11.9 % | 27.57 dB, 0.764, 9.0 % |

Pitch against an offline reference (YIN) on the flite speech: no gross (over 20 %) or octave errors in 1,148 voiced
frames, mean error 0.74 %, 95th percentile 2.3 %; 1.2 % of reference-voiced frames had their first harmonic called
unvoiced, and 7.7 % of fricative-like frames had a band above 2 kHz voiced (the analysis is unchanged by this work).

**Since the DVSI match (2, 4).** Through the matched encoder and the expanding decoder, reference metrics count our own
round trip a little worse, since the expansion multiplies the shape's quantization error by 1/0.6: on the research's 12
inputs MCD 22.85 dB and STOI 0.861 (21.51 / 0.876 with the new synthesis at full depth); at 3 % bit errors 26.07 dB /
0.825, in fades 32.2 dB / 0.735. On the operator's own voice (in neither tree), MCD 24.0 -> 23.0 dB and STOI 0.836 ->
0.871 against the codec before 18 Nov 2026. What matters more is that a DVSI decoder now understands us (2).

`dstarCodecQualityTest` measures LSD and MCD in C (it reproduced the flite column to 0.01 dB; with the synthesizer's
changes, the DVSI match, its pitch grid, the 1/0.73 expansion and the decoder match of 19 Nov 2026 it measures slt
10.09 / 23.71, kal 8.46 / 19.57, awb 9.13 / 20.42, rms 8.86 / 23.14 dB, mean MCD 21.71; 22.11 before the decoder
match, 22.94 with 1/0.6); it fails above each voice's figure plus 1.0 dB MCD and 0.5 dB LSD, a mean above 22.50 dB, a
level beyond 2 dB of the decoder's -4.4 dB or a delay other than 60 ms.
It sends the same frames through the channels above (one seed, the four voices) into the decoder and fails above these
limits (the old rule conceals 18 % at 5 %, so a rule that throws good frames away fails):

| Channel | Measured: mean MCD, concealed | Limit | With the 1/0.6 expansion | Before the DVSI match |
|---|---|---|---|---|
| 1 % bit errors | 22.80 dB, 0.0 % (23.00 before 19 Nov 2026) | 23.6 dB, 0.5 % | 24.08 dB | 22.33 dB |
| 3 % | 25.36 dB, 0.5 % (25.14) | 25.6 dB, 1.5 % | 26.28 dB | 23.68 dB |
| 5 % | 28.91 dB, 2.3 % (28.52) | 29.4 dB, 4 % | 29.94 dB | 26.21 dB |
| Fades | 29.69 dB, 6.5 % (29.65) | 30.7 dB, 10 % | 30.58 dB | 28.05 dB |

Without ffmpeg's flite filter those parts are skipped. STOI and the real recordings stay in the research (no recordings
in this tree). Its generated signals, through the whole codec:

| Signal | Measured | Limit |
|---|---|---|
| Vowels /a/ /i/ /u/ at 100, 150, 220 Hz | pitch within 0.32 %; level within 0.49 dB of -4.4 dB; harmonic envelope distance at most 5.3 dB | 2 %, 3.8 dB, 6.0 dB |
| Glide 90 to 240 Hz in 2 s | pitch within 0.6 % | 3 % |
| White noise | level -5.8 dB, LSD 12.0 dB | -5.9 +-1.5 dB (the output's -4.4 and the noise's -1.5, 4), 13.0 dB (LSD is not level matched) |
| Digital silence | peak 9 | 40 |

(The test's autocorrelation pitch reader now prefers half its lag when a peak there is within 0.06: a period between
two integer lags, as DVSI's 150 Hz grid point is, otherwise reads at half the pitch.)

The worst vowels are those with one harmonic on a formant (/a/ at 220 Hz): the quantizer's 48 bits can't follow a
single 30 dB peak exactly. The gain match took the vowels' worst level error from -3.2 to -1.2 dB.

## 8. CPU

Estimated, not measured on a radio: the research counted host instructions a frame on the 12 test inputs (6,701
frames; the four flite voices alone before this work) and scaled them by 0.843 radio cycles per host instruction, a
ratio measured for this core on other code with a similar instruction mix (16 x 16-bit multiply-accumulates and table
loads). The port's own count matches within 0.2 %. Of a 168 MHz processor (3.36 M cycles a 20 ms frame):

| Direction | Host instructions a frame, mean / max | Cycles a frame, mean / max | MHz to keep up, mean / max | Of 168 MHz, mean / max |
|---|---|---|---|---|
| Encode (analysis 507 k + quantizer 239 k) | 745 k / 829 k | 628 k / 699 k | 31.4 / 34.9 | 18.7 % / 20.8 % |
| Encode before the perceptual re-ranking (quantizer 135 k) | 639 k / 689 k | 539 k / 581 k | 27 / 29 | 16.0 % / 17.3 % |
| Decode (dequantizer 3 k + synthesis 162 k) | 166 k / 337 k | 140 k / 284 k | 7.0 / 14.2 | 4.2 % / 8.5 % |
| Decode before 18 Nov 2026 (synthesis 325 k) | 329 k / 723 k | 278 k / 609 k | 14 / 30 | 8.3 % / 18.1 % |

The perceptual pass costs about 110 k host instructions a frame (16 pairs scored, 3 refined); refining all 16 cost
twice that for under 0.05 dB. Synthesis is the decoder's variable part: voiced harmonics cost a sinusoid each, unvoiced
bands together one 256-point inverse FFT (three sines each before 18 Nov 2026, which made comfort noise the dearest
case), so a concealed frame now costs about what a decoded one does. One direction runs at a time. Since 19 Nov 2026
the inverse FFT also runs on fully voiced frames (the voiced noise floor, 4): host CPU time, research decoder, four
streams (an IC-9700's frames and three of ours), mean +2 to +30 % (about +15 %), worst frame +4 to +8 %; not
re-counted in instructions (no counter access on the host it was measured on), so about 4.9 % / 9.2 % of 168 MHz.

## 9. Limits

- **Clean-room AMBE, not DVSI's.** The bitstream follows mbelib's decoder, so frames should decode on any AMBE 3600x2400
  receiver. The research's interoperability test (rerun for this port, whose frames match the research framing's bit
  for bit) passes 20,000 quantized frames through unmodified mbelib's own FEC and decoder: no corrections, identical
  pitch and voicing, log2 magnitudes within 1.2e-3; the re-ranked quantizer's frames decode there too (the research's
  mbelib figures, 7). With commercial radios: an IC-9700's frames decode here (5), and an ID-51 understands our frames
  since the DVSI match (2), judged by one listener; the match's factor is one number pending a frame-by-frame
  comparison with a DVSI encoder.
- **Tuned mostly on synthetic speech.** The voicing and tracking constants were swept on the four Flite voices, under
  the constraint that the noise, mix and fricative tests keep a margin; the quantizer's measure and the concealment on
  those and 8 read-speech recordings (two with noise). Accents, children's voices and a radio's own microphone are
  untested.
- **Low pitches.** Below 90 Hz harmonics are closer than the analysis window's main lobe, and magnitudes are off by up
  to 1.7 dB (70 Hz).
- **No silence below the gain floor.** Digital silence decodes as noise at a peak of about 25: the smallest gain step
  is 0, so the decoded level never falls below -log2(L) / 2 in log2 magnitude.
- **No tone frames.** The encoder never sends DTMF or other tone frames (b0 126 or 127); received ones play silence.
- **Bad frames.** Fading repeats and comfort noise, no interpolation between good frames (it would need a frame of
  look-ahead, 20 ms more delay). The rule and the concealment were tuned on simulated channels (random errors and a
  two-state fade), not on a radio's: real fades, and a receiver that loses sync, may want other constants.
- **CPU estimated** (8).

## 10. Provenance

Written clean room ([CLEANROOM.md](../CLEANROOM.md)). No GPL code was opened by any author of this codec: not OP25,
mbelib-rs, mbelib-neo, imbe_vocoder, imbe_embedded, DSD's GPL files, OpenRTX, MMDVM, ircDDBGateway or codec2. No DVSI
patent text was consulted for the code.

The codec is a port of two modules written for it in a separate research tree (16-17 November 2026), restyled to this
tree without changing their arithmetic (a side-by-side harness matched every parameter, field and output sample of the
originals on 200,000 random frames and the test speech). Their quality work of 17 November (the perceptual re-ranking,
the gain match, the bad-frame rule and the concealment) was ported the same way on 18 November: the harness matched
every field and decoded parameter on 200,000 random frames and on all 6,745 frames of the 12 research inputs, and every
output sample with runs of concealed frames mixed in. Their own provenance records follow.

### The quantizer (`dstar_ambe_quant.c`, `dstar_ambe_tables.inc`)

From the research module cleanq, MIT ("2026 cleanq contributors"); its codebook values are mbelib's, ISC. Its author
read only:

| Source | License | Used for |
|---|---|---|
| mbelib (github.com/szechyjs/mbelib, commit 9a04ed5): `ambe3600x2400.c`, `ambe3600x2400_const.h`, `mbelib.c`, `mbelib.h` | ISC | The bitstream: field bit positions, the pitch formula, the L table, the voicing rule, the gain recursion, the PRBA and higher-order transforms, the prediction. The dequantizer restates `mbe_decodeAmbe2400Parms` and the quantizer is its inverse; the codebooks are rescaled to integers (every PRBA and higher-order entry is a multiple of 1/128, so Q7 is exact) |
| The research tree's framing (MIT) | MIT | Linked by its tests only |
| An MIT-licensed IMBE analyzer's interface (Open25's port to one HR_C7000 radio, its repository named in the research tree's `cleanq/SOURCES.md`; used only by a research speech test) | MIT | Nothing in this tree: neither cleanq nor this codec contains or needs it |
| Gersho and Gray, *Vector Quantization and Signal Compression* (1992); Griffin and Lim, IEEE Trans. ASSP 36(8), 1988 | literature | Weighted-MSE vector search, M-best survivors, a precomputed quadratic form; the MBE model |

It did not open any GPL source, and it did not open an earlier prototype quantizer of the research tree, nor that
prototype's codec glue or notes.

**The quality work** (17 November: the perceptual re-ranking, the gain match, the bad-frame rule, and the concealment
and test harness in the module below) was written by a second author, who read only:

| Source | License | Used for |
|---|---|---|
| The two modules (sources, tables, tests, provenance records) | MIT | The code being changed |
| mbelib 9a04ed5: `mbe_processAmbe2400Dataf` (the tone path's bit unpacking), `mbe_useLastMbeParms` | ISC | mbelib's bad-frame rule (more than 3 Golay corrections: repeat; the fourth repeat: mute), the baseline the new rule is measured against |
| The research tree's framing (MIT) | MIT | Which bits Golay protects, and the error counts it reports |
| The research tree's quality metric, bit-error script and speech script | MIT | MCD (unchanged) and the test conventions |
| Gersho and Gray (1992) | literature | M-best search, re-ranking survivors under a second distortion measure |
| S. Davis and P. Mermelstein, IEEE Trans. ASSP 28(4), 1980; the HTK mel formula 2595 log10(1 + f / 700) | literature | The mel filter bank of the band-energy measure (the MCD measure's 24 bands) |
| E. Zwicker and H. Fastl, *Psychoacoustics*, general | literature, from memory | Weighting error by band energy and ignoring bands far below the peak; no model or constant taken (the floor and the exponent were swept) |
| E. N. Gilbert, BSTJ 39, 1960; E. O. Elliott, BSTJ 42, 1963 | literature | The two-state fading channel |
| C. Perkins, O. Hodson and V. Hardman, IEEE Network 12(5), 1998 | literature | Repetition with fading and noise substitution |
| Taal et al., IEEE TASLP 19(7), 2011 | literature | STOI, written from the paper; no implementation consulted |

That author did not open any GPL source (OP25, imbe_vocoder, imbe_embedded, mbelib-rs, mbelib-neo, DSD's GPL files,
OpenRTX, codec2), the Open25 port's vocoder sources or their research-tree copy, the earlier prototype's quantizer, glue
or notes, or the research tests that link the IMBE analyzer. The research's real-speech inputs are LibriSpeech (CC BY
4.0) with a VOiCES room response and television recording (CC BY 4.0) and a Freesound car recording (CC0), used as test
audio only.

**The earlier prototype.** Before the decision to use no GPL code, the agent that wrote that earlier prototype
quantizer (the research tree's `lib/dstar_quant.c`) had read OP25's GPL-3 AMBE encoder and D-STAR bit tables. No OP25
code or table was copied, and all of it was deleted from the research tree, but that quantizer's structure follows
the decoder equations OP25 also inverts. **That prototype is not used here**: this codec's quantizer is cleanq, written
afresh by an author who had not seen OP25 and did not open the prototype. The prototype's framing was re-derived from
the ISC sources; this codec's framing (below) uses only the ISC values and the DMR coder already in this tree.

### Analysis and synthesis (`dstar_mbe_analysis.c`, `dstar_mbe_synth.c`, `dstar_mbe_fixed.c`, `dstar_mbe_tables.inc`)

From the research module mbe, MIT ("2026 mbe contributors"); the synthesizer and three tables are ports of mbelib, ISC.
Its author read only:

| Source | License | Used for |
|---|---|---|
| mbelib 9a04ed5: `mbelib.c`, `mbelib.h`, `mbelib_const.h` (Ws), `ambe3600x2400.c`, `ambe3600x2400_const.h` (the L and voicing tables) | ISC | The synthesizer ports `mbe_spectralAmpEnhance`, `mbe_synthesizeSpeech` (uvquality 3) and the repeat and mute logic of `mbe_processAmbe2400Dataf`; the pitch formula, L table and Ws ramp are tables; mbelib's output scaling fixes the analysis's output scale |
| cleanq (above) | MIT | The parameter struct and how the quantizer votes voicing per band pair |
| The research tree's quality metric, an mbelib reference decoder, a stack tool, and the 0.843 cycles per host instruction ratio | MIT / ISC | Measurements only |
| Griffin and Lim, "Multiband Excitation Vocoder", IEEE Trans. ASSP 36(8), 1988 | literature | The MBE model; the harmonic fit and its residual as a voicing measure; the period-fit error (measured and rejected, 2) |
| Hardwick, MIT thesis, 1988; DVSI's expired US patents on MBE analysis and pitch tracking (e.g. US 5,226,108) | literature, from memory | The structure of look-back and look-ahead tracking with a sub-multiple check, and an initial estimate refined in the frequency domain. The patent texts were not re-read, and no threshold was taken from one |
| de Cheveigne and Kawahara, "YIN", JASA 111(4), 2002 | literature | The per-lag energy normalization of the pitch error; the offline pitch reference |
| Marsaglia, "Xorshift RNGs", 2003; general DSP texts (radix-2 FFT, Hamming window, windowed-sinc filters) | literature | The noise generator, the FFT, the windows, the pitch path's low-pass |

It did not open any GPL source, that Open25 port's vocoder sources or their research-tree copy, the earlier prototype's
quantizer, glue or notes, or the research tests that link them. Earlier output recordings were used as audio only.
The output scale is derived; the voicing and tracking constants were set by sweeps on the four test voices (9).

### The frame (`dstar_ambe_frame.c`)

Rewritten clean room on 19 November 2026 from [dstar-ambe-frame.md](dstar-ambe-frame.md), whose section 7 lists its
sources (mbelib, ISC; the JARL standard; off-air DVSI frames; the shared DMR Golay coder's interface) and the clean-room
statement. The earlier file, which took the air interleave's values from DSD's `dstar_const.h` as carried by the GPL
DSD-FME repository, was deleted unread; no DSD value or table is used.

### This port

The porting agent read the two modules above (sources, tables, tests, provenance records and measured outputs), the
research framing code and its provenance record, the research quality metric and speech script, and mbelib 9a04ed5
(ISC: its notice, the decoder, its FEC and frame logic, `mbe_processAmbe2400Dataf`). It did not open the earlier
prototype quantizer or glue, the research notes, the tests that link the IMBE analyzer, or any GPL code. The ISC
notices are in the files that carry ISC-derived content and in [LICENSES/ISC.txt](../../LICENSES/ISC.txt).

The agent that ported the quality work read the two modules' 17 November sources and their copies from before it (to
take the difference), their provenance records, the quality harness (`mbe/test/eval2.c`, `run_quality2.sh`), the
research's quality and STOI scripts and results, the real inputs' source list, and the research framing's unpack (for
the error counts the new rule reads). It did not open the earlier prototype quantizer or glue (`lib/dstar_quant.c`,
`lib/dstar_codec.c`), `lib/NOTES.md`, the tests that link the IMBE analyzer, or any GPL code.
