<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR

D-STAR digital voice (DV), the amateur mode of the JARL standard ("Digitalization Technology Standard for Amateur
Radio", English edition 6.0), on the air interface ([air-interface.md](air-interface.md)). Switch: `CONFIG_DSTAR`,
which also builds the air interface and the MBE core ([build-config.md](../build-config.md)). Which radios could carry
it: [feature-matrix.md](../feature-matrix.md). Section numbers below are the standard's.

State:

| Part | State |
|---|---|
| Header: fields, P_FCS, FEC, interleave, scrambler (2) | written, host tested |
| Voice frames, data synchronization, slow data (3) | written, host tested |
| Receive, the call layer, late entry, the engine (4) | written, host tested through the framer |
| Voice: the half-rate vocoder's FEC, decoder and encoder on the MBE core (5) | written, host tested |
| The radio core: mode, settings, PTT, debug link, home screen (6) | written; the radio core's other tests run with it built |
| On air | not checked on a radio, against another make or a repeater (8) |
| Stored channels, settings kept over a restart, data (DD), fast data, D-PRS transmit, code squelch | not written (9) |

## 1. On air

| Item | Value | Section |
|---|---|---|
| Modulation | GMSK, 4800 bit/s, BT 0.5; a 1 is the positive deviation | 2, Ap1.5 |
| Deviation | ±1200 Hz (modulation index 0.5) | 2 |
| Bit order | every field least significant bit first | Ap1.4 |
| Bit synchronization | `1010...`, 64 bits | 4.1.1 a |
| Frame synchronization | `111011001010000` | 4.1.1 b |
| Header | 41 bytes, 660 bits coded | 4.1.1, Ap2 |
| Voice frame | 72 bits of voice and 24 of slow data, every 20 ms | 4.1.2 b |
| Data synchronization | `101010101011010001101000` (bytes 55 2D 16) in the slow data of every 21st frame, the first frame after the header among them | 4.1.2 c, d |
| Last frame | 32 bits of `1010`, `000100110101111`, `0` (bytes 55 55 55 55 C8 7A), after the last voice frame | 4.1.2 g, h |

The profile (`dstar_air_mode`, `core/dstar/dstar_call.c`): 4800 symbols a second, two levels, a bit a symbol (`01` +3
for a 1, `11` -3 for a 0), Gaussian BT 0.5, 1200 Hz, continuous, the data synchronization recurring every 2016
symbols, idle `1010`. Its patterns, each 24 symbols with two errors tolerated:

| Pattern | Where it sits | Frame start |
|---|---|---|
| Data synchronization | the last 24 bits of frame 0 of a superframe | 72 symbols before it |
| Last frame | the last 24 bits of the last frame's 48 | 24 symbols before it |
| Header start (start only) | the bit synchronization's last 9 bits and the frame synchronization | the pattern's first symbol: the unit is the pattern and the 660 header symbols |

## 2. The header

### 2.1 Fields

| Byte | Field |
|---|---|
| 0 | Flag 1: bit 7 data, 6 through a repeater, 5 break-in, 4 control, 3 urgent; bits 2-0 the function (0 none) |
| 1, 2 | Flags 2 and 3: 0 |
| 3-10 | Destination repeater (RPT2): `DIRECT` direct |
| 11-18 | Departure repeater (RPT1): `DIRECT` direct |
| 19-26 | Companion (UR): `CQCQCQ` to all |
| 27-34 | Own callsign 1 (MY) |
| 35-38 | Own callsign 2 (the suffix) |
| 39, 40 | P_FCS, low byte first |

Callsigns are ASCII, space padded.

### 2.2 P_FCS

CRC-CCITT (x^16 + x^12 + x^5 + 1, 4.1.1 k) over bytes 0-38, reflected (the standard's 0x8408, Ap2.1). The standard
names the polynomial only. Sent: from all ones and inverted, as X.25's frame check (`dstar_crc`; "123456789" gives
906E). Taken: that, or the same not inverted.

### 2.3 FEC, interleave, scrambler

| Step | Rule | Section |
|---|---|---|
| Bits | the 41 bytes, each least significant bit first, then two zeros: 330 bits | Ap2.1 |
| Code | rate 1/2, constraint length 3, registers from zero: two output bits for each input bit | Ap2.1 |
| Interleave | code bit k goes in column k / 24, row k % 24 of a 24-row matrix (the 28th column holds 12); the rows are sent in turn, the first 12 with 28 bits, the others with 27 | Ap2.2 |
| Scrambler | the 660 bits on air XOR S(x) = x^7 + x^4 + 1 from all ones, restarted at the header | Ap1.1, Ap1.3 |

The standard's text on the code's generators repeats the CRC's value. The code uses the constraint-length-3 code's usual
generators, 1 + D + D^2 for the first bit of each pair and 1 + D^2 for the second. The receiver decodes with a soft
Viterbi (four states, the soft values read from the framer's buffer) and, when the P_FCS fails, tries the two output
bits the other way round.

## 3. Voice frames and slow data

A voice frame's 72 voice bits are the half-rate vocoder's frame as on air (5). Its 24 slow data bits are the data
synchronization in frame 0 of each superframe; in frames 1-20 they are data scrambled with the first 24 bits of the
scrambler's sequence, bytes 70 4F 93 (Ap1.3). Frames 1 and 2, 3 and 4 ... 19 and 20 make 6-byte blocks; the first
byte is the mini-header (6.1.3):

| Mini-header | Block | Receive | Send |
|---|---|---|---|
| `3n` | simple data, n bytes (D-PRS, GPS) | collected into lines; the last line is in the view | not sent |
| `40`-`43` | the message, 5 characters each of its 20 | assembled; shown with the call | in the first four blocks of every even superframe when the settings have one |
| `5n` | the header again, n bytes (5 each, 1 in the last of nine) | taken when the header on air was missed (a late entry) | in every odd superframe, or every one without a message |
| `66` | no data | | the rest of a superframe |

## 4. Receive and the call layer (`core/dstar/dstar_call.c`)

| Event | What happens |
|---|---|
| Header start | the framer asks for the whole unit (684 symbols); the header is decoded, a call starts (the last one ended), the next frame is frame 0 |
| A frame with the data synchronization | frame 0; a call that wasn't open starts here, joined late (its fields from the slow data) |
| Any other frame | counted on from the last synchronization; its voice goes on, its slow data from frame 1 on |
| The last frame's pattern | the head hook refuses the unit: the call ends |
| Two data synchronizations missed (42 frames) | the framer says the signal is lost: the call ends |

Every call plays: simplex D-STAR has no squelch of its own, and a repeater's calls are for its listeners.

## 5. Voice (`core/mbe/ambe*.c`, `core/dstar/dstarVoice.c`)

D-STAR carries the half-rate vocoder of 3600 x 2400 bit/s (the AMBE-2020 compatible mode of 4.1.2 b), not the AMBE+2
of DMR and NXDN: the board's vocoder is not used. Its front end on the MBE core ([p25.md](p25.md) 5):

| Part | Rule |
|---|---|
| FEC | c0, a (24,12) Golay word; c1, a (23,12) Golay word whitened by a sequence seeded from c0's data (x' = 173 x + 13849 mod 65536 from 16 times the data, the top bit each step); c2 (11 bits) and c3 (14), unprotected. The bit order on air is DSD's table (ISC) |
| Information bits | 49: pitch (7), voicing codebook (4), gain step (6), PRBA vectors (9, 7), higher-order coefficients (4 x 4, the last's lowest bit 0) |
| Pitch | codes 0-119 speech (f0 = 2^-(4.3118 + 0.021336 (b + 0.5)) of the sample rate), 120-123 erasure, 124-125 silence, 126-127 tone |
| Amplitudes | the gain, a step on half the last frame's; the residual from an 8-point PRBA vector and four blocks of DCT coefficients; 0.65 of the last frame's amplitudes, resampled, predicted |
| Decoder | mbelib's dequantizer (ISC), in integers; the core's enhancement and synthesis. A frame whose Golay words corrected more than three bits, or an erasure, repeats the last good one (then mutes); a silence or tone frame plays silence; over a tenth of the protected bits corrected, the output mutes |
| Encoder | the core's estimator (pitch, voicing by 500 Hz band against the codebook, amplitudes), then the inverse of the dequantizer: the gain step nearest, full searches of the PRBA and coefficient codebooks; the frame is run through the dequantizer so the prediction is the receiver's. Zeros are sent as the silence frame (9E 8D 32 88 26 1A 3F 61 E8, 6.6) |
| Scale | the full-rate decoder's: a model amplitude times 7 is PCM |

The voice path queues frames from the provider's task and decodes one each 20 ms block from the third queued on; the
encoder runs on the mic blocks while a transmission is on, as P25's ([p25.md](p25.md) 5). Its memory, and the
engine's, is what an idle vocoder lends (`hal_vocoder_t.lend`) where the platform lends, else P25's or its own.

## 6. The radio core (`core/radio/radio_dstar.c`)

### 6.1 Selecting it

The VFO's mode tile steps FM, DMR, P25, NXDN, D-STAR where the board's air interface carries each; the debug link's
`DMR_FIELDS` sets it too (mode 4). D-STAR runs with the board in its digital mode, the DMR stack stopped, at 12.5 kHz,
like P25 and NXDN ([air-interface.md](air-interface.md) 7).

### 6.2 What we send

The D-STAR settings (RAM; Settings > My radio > D-STAR, or the debug link's `DSTAR` operation, [debuglink.md](debuglink.md)):

| Setting | Empty |
|---|---|
| My callsign | the callsign in the radio name (its first word, when it is one) |
| Suffix | four spaces |
| Your call (UR) | `CQCQCQ` |
| Repeater (RPT1): the repeater and its module letter in the 8th place | simplex: both repeater fields `DIRECT`, flag 1 0 |
| Gateway (RPT2) | the repeater's callsign with `G` in the 8th place; flag 1 has the repeater bit |
| Message | none: the slow data sends the header again instead |

PTT without a callsign (none set and none in the radio name) is refused (`no_id`): nothing goes on air without one.

### 6.3 What it shows

Home shows the caller (`MY/SUFFIX > UR`, then their message) while a call plays, `D-STAR` until a late entry's header is
in; the talk line shows UR, the strip the repeater or `Simplex`. The debug link's `dstar` section has the call, the
settings, what is sent and the counters.

## 7. Tests

| Test | What |
|---|---|
| `tests/host/ambeTest.c` | the silence frame through the FEC; the interleave; the Golay words with up to three errors; 20000 random frames through the dequantizer and back; envelopes; tones at every fifth pitch through the encoder and decoder (pitch, voicing, level); noise; zeros; an error burst |
| `tests/host/dstarTest.c` | the P_FCS against the CRC's check value; the scrambler; headers through the FEC with errors; the message and the header in slow data; whole calls through the framer: clean, with noise, joined late, cut off without the last frame |
| `radioCoreTestDstar` | the radio core's tests with D-STAR built |
| `uiFlowTest` | the mode tile, the strip, Settings > My radio > D-STAR |

## 8. Unconfirmed

| Item | Assumed | Where |
|---|---|---|
| The code's generators and the order of a pair's bits | 1 + D + D^2 first, 1 + D^2 second; the receiver also tries the other order | 2.3 |
| P_FCS: start and inversion | all ones, inverted; the receiver takes it not inverted too | 2.2 |
| The header's scrambler: after the interleave, from all ones at the header's first bit | as Ap1.3 reads | 2.3 |
| The half-rate vocoder's level against other makes' | the full-rate decoder's scale | 5 |
| The information bit d24 | not used, sent 0 (mbelib reads no value from it) | 5 |
| Tone frames | played as silence | 5 |
| A board's transmit pulse | the provider's own where its fit says `AIR_FIT_SHAPE_APPROX` (a modem chip's raised cosine instead of the Gaussian); whether a receiver of another make takes it is untested | 1 |
| The bit synchronization's phase before the frame synchronization | `...0101010` (64 bits from a 1); a header after a preamble of the other phase is missed and the call joined late | 1 |

## 9. Not written

Stored D-STAR channels (a codeplug kind and the Codeplug Editor, [channel-modes.md](channel-modes.md)), the settings kept
over a restart, scan of D-STAR channels, data (DD) and fast data, D-PRS and GPS transmit, code squelch, a list of
callsigns heard, repeater and gateway routing helpers (callsign routing, link commands).
