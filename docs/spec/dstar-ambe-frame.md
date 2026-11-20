<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR AMBE voice frame: bit layout, FEC and interleave

The 72-bit voice frame of D-STAR digital voice (AMBE 3600x2400: 48 speech bits and 24 FEC bits every 20 ms) between
the quantizer's nine fields b0..b8 ([dstar-codec.md](dstar-codec.md) 3) and the nine bytes the DV framing carries
([dstar-modem.md](dstar-modem.md) 4). Code: `core/dstar/dstar_ambe_frame.c`, interface `dstar_ambe_frame.h`
(`dstar_ambe_pack`, `dstar_ambe_unpack`). Tests: `tests/host/dstarCodecTest.c` (6).

This page was written clean room for that file's rewrite of 19 November 2026 (7): from mbelib (ISC), the JARL
standard, and real frames received off air, without reading any earlier implementation of this frame.

## 1. The three words

The frame is three 24-bit words. Bit 23 is each word's most significant bit.

| Word | Bits | Content |
|---|---|---|
| W0 (C0) | 23..12 | d0..d11 (d0 on bit 23) |
| | 11..1 | Golay(23,12) parity of bits 23..12 |
| | 0 | overall parity: the word has even weight |
| W1 (C1) | 23..12 | d12..d23 |
| | 11..1 | Golay(23,12) parity of bits 23..12 |
| | 0 | overall parity |
| | all | then XORed with the whitening sequence (3), seeded from d0..d11 |
| W2 (C2, C3) | 23..14 | d25..d34 (C2), unprotected |
| | 13..0 | d35..d48 (C3), unprotected |

d0..d48 is mbelib's 49-bit parameter vector `ambe_d` (`mbe_eccAmbe3600x2400Data`). Its d24 is W1's overall parity bit,
not a speech bit: mbelib reads it as the first bit of its third row (`ambe_fr[2][10]`) and ignores it. mbelib's rows
map to the words as: `ambe_fr[0][k]` = W0 bit k; `ambe_fr[1][k]` = W1 bit k + 1 (k = 0..22); `ambe_fr[2][10]` = W1
bit 0; `ambe_fr[2][k]` = W2 bit k + 14 (k = 0..9); `ambe_fr[3][k]` = W2 bit k (k = 0..13).

## 2. Fields

From `mbe_decodeAmbe2400Parms` (the voice path; b8's lowest bit is not sent and reads as 0):

| Field | Bits | d bits, most significant first |
|---|---|---|
| b0 pitch | 7 | d0 d1 d2 d3 d4 d5 d48 |
| b1 voicing | 4 | d38 d39 d40 d41 |
| b2 gain | 6 | d6 d7 d8 d9 d42 d43 |
| b3 PRBA 1 | 9 | d10 d11 d12 d13 d14 d15 d16 d44 d45 |
| b4 PRBA 2 | 7 | d17 d18 d19 d20 d21 d46 d47 |
| b5 HOC 1 | 4 | d22 d23 d25 d26 |
| b6 HOC 2 | 4 | d27 d28 d29 d30 |
| b7 HOC 3 | 4 | d31 d32 d33 d34 |
| b8 HOC 4 | 4 | d35 d36 d37, then 0 |

So the 12 bits of W0's data are `b0 >> 1` (6), `b2 >> 2` (4), `b3 >> 7` (2); W1's are `(b3 >> 2) & 31` (5),
`b4 >> 2` (5), `b5 >> 2` (2); W2 is `(b5 & 3)`, `b6`, `b7`, `b8 >> 1`, `b1`, `b2 & 3`, `b3 & 3`, `b4 & 3`, `b0 & 1`,
most significant first (2 + 4 + 4 + 3 + 4 + 2 + 2 + 2 + 1 = 24 bits). Tone frames (b0 = 126 or 127) use the same
bits under other names; this layer doesn't look.

## 3. Golay code and whitening

**Golay.** g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75). The parity of 12 data bits D is
(D(x) x^11) mod g(x), so a 23-bit word data << 11 | parity is a multiple of g(x): the cyclic Golay(23,12) code. mbelib's
`golayGenerator` table is exactly x^(22-i) mod g(x) for data bit i (checked by computation), and its decoder
(`mbe_golay2312`, `ecc.c`) is a syndrome table lookup over that code. The overall parity bit makes the extended code,
minimum distance 8: any 1 to 3 errors in a word are corrected, and 4 are detected, never miscorrected, because a
weight-4 error pattern is at distance 4 or more from every other code word.

**Whitening.** x0 = 16 u, where u is W0's 12 data bits (d0 most significant); then x(s) = 173 x(s-1) + 13849 mod
65536, and step s gives bit 15 of x(s). Step s (1..24) is XORed onto W1 bit 24 - s: steps 1..23 on its 23-bit Golay
word from the most significant bit down, as `mbe_demodulateAmbe3600x2400Data` does, and step 24 on the overall parity
bit, which mbelib leaves alone. The receiver corrects W0 first and seeds the sequence from the corrected data.

Two consequences used below:

- the sequence of x(s) mod 16 doesn't depend on u, and adding 2^15 to x0 adds 2^15 to every x(s) (173^s is odd), so
  flipping d0 flips all 24 whitening bits. The all-ones word is an extended Golay code word, so that change is
  invisible to W1's syndrome.
- whitening is the only thing that ties W1 to W0. Two frames with the same W0 have the same whitening, and the XOR of
  their W1s is a code word.

**Step 24 is real.** The standard's lost-packet frame can't show it (its 24th step is 0), but frames from a DVSI
encoder do: with step 24 left out, W1 fails its overall parity on 2,054 of the 3,947 frames of 5; with it, on none.

## 4. Interleave

Air bit a = 0..71 is the a-th bit sent: in `core/dstar`'s byte order (`dstar_voice.h`), bit a mod 8 of byte a / 8,
least significant first. Let block = a / 24, o = a mod 24, pair p = o / 2, half h = o mod 2. Air bit a carries bit

    k = 8 - 4 * block + r[p] + 12 * h      of word w[p]

| p | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w | W0 | W2 | W1 | W0 | W1 | W0 | W2 | W1 | W2 | W1 | W0 | W2 |
| r | 2 | 3 | 3 | 3 | 1 | 1 | 2 | 2 | 0 | 0 | 0 | 1 |

Every word sends 8 bits in each 24-bit block (block 0 its bits 8..11 and 20..23, block 1 bits 4..7 and 16..19,
block 2 bits 0..3 and 12..15), as adjacent pairs: bit k at an even air position and bit k + 12 just after it. Air bit
order, most significant word bit first:

| Word | Air bits for word bits 23, 22, ..., 0 |
|---|---|
| W0 | 7 1 11 21 31 25 35 45 55 49 59 69 6 0 10 20 30 24 34 44 54 48 58 68 |
| W1 | 5 15 9 19 29 39 33 43 53 63 57 67 4 14 8 18 28 38 32 42 52 62 56 66 |
| W2 | 3 13 23 17 27 37 47 41 51 61 71 65 2 12 22 16 26 36 46 40 50 60 70 64 |

The 12-entry table plus the formula is what the code holds, rather than a 72-entry table.

## 5. How the interleave was derived

Neither the JARL standard nor mbelib gives the interleave: the standard says only that the 72 bits are AMBE-2020 or
a compatible mode (JARL STD 6.0, 4.1.2 b), and mbelib starts from its 4 x 24 rows. It was derived here from 3,947 voice
frames received off air on 18 November 2026 from the DVSI encoders of two Icom radios: an IC-9700 (347 frames of speech
and background, 491 of a whistle), an ID-51 (681), and the research tree's two 1,214-frame ground-truth DVSI runs.
The recordings stay outside the repository (AGENTS.md rule 2); the test keeps four of the background frames (6). Each
step is a computation over GF(2) any reader can repeat with such a capture:

1. **W0's air bits.** The frames as rows of a 3947 x 72 matrix have rank 60. Its null space is 12-dimensional and
   lives on 24 air bits only, the ones the table gives W0. A self-dual [24,12,8] code (every null vector has weight 8,
   12, 16 or 24) is the extended Golay code, so these are W0.
2. **W1's air bits.** The 869 frame groups with equal W0 bits have equal whitening, so the XORs within each group
   (3,078 rows) cancel it. Over the 48 remaining air bits they have rank 36, and the 12-dimensional null space lives on
   24 air bits, another extended Golay code: W1. That leaves 24 air bits for W2.
3. **Which W0 air bit is which.** Any of the 244,823,040 permutations in M24 maps one copy of the extended Golay code
   onto another, so the code alone can't order W0. The whitening can: W1's syndrome under the parity checks of step 2
   is a fixed function of W0, the same for every frame of a group, and it must equal M times the 24 whitening bits for
   some fixed 12 x 24 matrix M. A wrong order of W0's data bits gives the wrong seed and no M fits 869 groups.
   - The search was cut by two facts. Flipping d0 complements the whitening (3); the one W0 difference that never
     changed W1's syndrome over 57 pairs of groups is then d0's generator row, an octad. And the standard's lost-packet
     frame (JARL STD 6.0, 6.6: `9E 8D 32 88 26 1A 3F 61 E8`) decodes to b0..b8 = 124 0 4 165 41 10 7 7 10
     ([dstar-codec.md](dstar-codec.md) 5), so its W0 data must read 0xF85.
   - Mappings of mbelib's 12 data positions that agree with those values and keep every octad an octad: 95,040, all
     true code isomorphisms (each parity position's coefficients over the data bits found exactly once). **One** of
     them gives a consistent M.
4. **Which W1 air bit is which.** M's 24 columns are then W1's parity-check columns in whitening-step order, all
   distinct, so each step names its air bit. All 3,947 frames then decode with W0 and W1 both valid extended Golay words
   and no corrections, step 24 on the overall parity bit included.
5. **The pattern, and W2.** W0 and W1 follow the formula of 4 with r = 2 3 1 0 at pairs 0 3 5 10 (W0) and 3 1 2 0 at
   pairs 2 4 7 9 (W1). W2 has no code to test against. Taking the same form for it (its pairs 1 6 8 11, its bits read
   from d25 at bit 23 down to d48 at bit 0, as W0 and W1 read theirs), 48 variants remain (24 orders of r, either
   direction); of these, only r = 3 2 0 1 reproduces all 24 W2 bits of the lost-packet frame (the best of the other 47
   gets 20). The whole frame is then checked end to end against the research decoder (6).

## 6. Tests

`tests/host/dstarCodecTest.c` (`make -C tests/host run-dstar`):

| Test | What |
|---|---|
| `test_frame_round_trip` | random fields through a frame and back; each air bit flipped alone is corrected in W0 or W1 or changes one field bit (24 bits each) |
| `test_frame_errors` | up to 3 errors in each Golay word corrected and counted, 4 in W0 detected, 4 in W1 detected and decoded as its 23-bit word, so exactly one W1 air bit acts as the parity bit |
| `test_bit_order` | the lost-packet frame: no corrections, packs back bit for bit, and fails in the other bit order |
| `test_dvsi_frames` | the standard's lost-packet frame: no corrections, re-packed bit for bit, 4 W1 errors detected (the off-air frames that confirmed step 24 stay out of the tree: synthetic and standard vectors only) |
| `test_frame_golay_patterns` | every 1-, 2- and 3-bit error pattern in each word (2,324 each) corrected and counted; every 4-bit pattern (10,626 each) detected |
| `test_frame_spec` | the test restates this page: the field table of 2, Golay parity by polynomial division, the whitening sequence and the air table of 4 written out. 20,000 random frames pack identically, and the five real frames rebuild from their fields |
| `test_frame_whitening` | with W1's data 0 the air carries the sequence itself: all 4,096 seeds give the restated 24 steps, and flipping d0 inverts all 24 |

The firmware's codec is also compared with the research decoder sample for sample on real frames (dstar-codec.md 10;
the research tree's `fwcheck.sh`).

## 7. Sources and clean room

| Source | License | Used for |
|---|---|---|
| mbelib (`ambe3600x2400.c`: `mbe_eccAmbe3600x2400C0`, `mbe_eccAmbe3600x2400Data`, `mbe_demodulateAmbe3600x2400Data`, `mbe_decodeAmbe2400Parms`; `ecc.c`, `ecc_const.h` `golayGenerator`) | ISC | the rows and d0..d48, the field map, the Golay generator, the whitening sequence |
| JARL STD 6.0 (English), 4.1.2 b and 6.6 | standard | the 72-bit frame, the lost-packet frame |
| [dstar-codec.md](dstar-codec.md) 5 | MIT (this tree) | the bit order, the lost-packet frame's fields, the step-24 finding |
| 3,947 off-air frames of DVSI encoders (5), kept out of the tree | data | the interleave and the step-24 check |
| `core/dmr/ambe/ambeFec.h` (this tree, its interface only) | MIT | the shared Golay coder and whitening generator the code calls: the same code and sequence as 3, by their stated contract |

The author of this page and of the 19 November `dstar_ambe_frame.c` did not open the earlier `dstar_ambe_frame.c`, in
any tree or in git history, nor its commit diffs; did not read DSD, DSD-FME, MMDVM, OP25, Codec2 or any other GPL or
LGPL source; and did not read the research tree's frame code. The research tree was only run, as a black box, for the
end-to-end comparison.
