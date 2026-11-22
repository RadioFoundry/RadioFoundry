<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Yaesu System Fusion: hardware path, modem and framing

Yaesu System Fusion (YSF, C4FM) digital voice on a radio built for DMR: which way C4FM reaches the air, the software
modem, the framing (frame sync, FICH, callsign data, V/D mode 2 "DN" voice and Voice FR "VW" voice), the engine that
joins them to a board, and the radio core's Fusion channels. The codeplug records are [ysf-codeplug.md](ysf-codeplug.md); the bench plan is
[docs/testing/ysf-bench.md](../testing/ysf-bench.md).

Code: `core/ysf` (portable, integer only, host tested): `ysf_fec.c` (coding), `ysf_frame.c` (framing, transmit and
receive), `ysf_vw.c` (Voice FR's frame and layout, 4.1), `ysf_c4fm.c` (modem), `ysf_engine.c` (both joined for a
board), `ysf_codeplug.c`. The radio core's part is `core/radio/radio_ysf.c` and, for VW's codec, `radio_ysf_vw.c` on
the full-rate codec `core/mbe`; the screens' `core/ui/ui_screens.c`. Build switches: `CONFIG_YSF` (needs DMR; on a
board only where its glue runs it and its RAM fits, [build-config.md](../build-config.md)) and `CONFIG_YSF_VW` (VW,
with YSF by default). Tests: `tests/host/ysfTest.c`, `ysfModemTest.c`, `ysfVwTest.c`, `ysfCodeplugTest.c`,
`ysfRadioTest.c`, `ysfVwRadioTest.c`; vectors in `tests/vectors/ysf.txt` and `ysf-vw.txt` from the independent
references `tools/ysf_ref.py` and `tools/ysf_vw_ref.py` (6).

**Sources.** Yaesu Musen, "Amateur Radio Digital Standards", v1.0 (15 January 2013), revision 1.02 (29 July 2015),
38 pages: Yaesu's own description of the air interface ("the standard" below; its section numbers are given as Y 4.5
and so on). Public copy: `https://www.rcqsl.com/manuals/78.pdf`. Its printed root raised cosine equation is garbled
(the symbol rate and roll-off are clear); a public write-up says the same
(`https://hb9uf.github.io/gr-ysf/`, prose only: that project is GPL and its code was not opened). Written clean room
([CLEANROOM.md](../CLEANROOM.md)): no other YSF implementation was read. Where the standard is silent, the choice made
and what settles it are said in place.

## 1. Design

### 1.1 What a board must provide

Two ways a radio can carry C4FM, both supported by the engine (5):

| Path | What it is | Board work |
|---|---|---|
| **Flat FM audio** | the software modulator's samples into the FM modulator, the discriminator's samples into the software demodulator | `hal_audio_tap_t` with `AUDIO_TAP_FM_TX_FLAT` and `AUDIO_TAP_FM_RX_RAW`: the same glue D-STAR uses ([dstar-modem.md](dstar-modem.md) 1.1), built by `CONFIG_FM_FLAT` for either mode |
| **A modem chip's raw 4FSK** | dibits straight into the chip's physical layer (4800 symbols/s, its own root raised cosine pulse), 144 symbols a block; the chip's decided dibits back | `ysf_engine_tx_block` / `ysf_engine_rx_dibits`: no board glue written yet (1.2) |

Rate: 8 kHz, 16-bit samples each way are enough (2.5). The board gives its paths' calibration (`audio_tap_flat_cal_t`):
the sample value for 1 kHz of deviation, the polarity each way, a high-pass to undo.

### 1.2 On an HR_C7000 with a separate FM RF chip

C4FM and DMR are close relatives: both 4800 symbols/s, four levels, a root raised cosine pulse with roll-off 0.2, and
the same dibit to level mapping (00 +1, 01 +3, 10 -1, 11 -3: Y 3.2 table 3-1; the HR_C7000's physical layer,
[modem-hrc7000.md](modem-hrc7000.md) 2.10). They differ in the deviation (DMR's +-648 / +-1944 Hz; Fusion's wide
+-900 / +-2700, narrow +-450 / +-1350), the 40-bit frame sync against DMR's 48, and the frame (480 symbols, 100 ms,
continuous, against 144-symbol bursts). The routes:

| Route | Transmit | Receive |
|---|---|---|
| **The 4FSK modem's physical layer** (WORK_MODE 0x02, continuous) | **Possible, not written.** The chip sends the CPU's 288 bits per 30 ms block with its own pulse and clock; RadioFoundry's split duplex TX half sends a continuous, arbitrary symbol stream this way, decoded off air (modem-hrc7000.md 2.10, findings 6-7), and another project sends P25 C4FM through it on a radio of this kind. Costs: the levels are DMR's, 0.72 of Fusion wide (2.5: about 2.5 dB at a receiver that scales its thresholds to the levels, more at one that doesn't); a mode switch each over (the radio receives in FM, 2nd column); its own driver in the DMR engine beside the split duplex one (e->bs). RF_MOD_BIAS_CTRL's modulation amplitudes might scale the levels; unmeasured | **Not usable as the chip frames it.** The chip's receiver aligns its blocks on a 48-bit sync comparator (two 24-bit halves; the P25 sync fits it). Fusion's sync is 40 bits and the 8 bits either side vary with the frame type and the data, so no fixed 48-bit pattern matches, and how far THRESHOLD_VALUE lets errors through is undocumented |
| **The flat FM paths** (the TX RAM as the FM source, the RX RAM copy of the discriminator; D-STAR's) | **Chosen.** The software modulator's samples; the chip's FM modulator adds no shaping of its own once the filters are bypassed. Unmeasured: the route's frequency response (a high-pass hurts 4FSK more than GMSK, 2.5), the deviation scale, whether the FM limiter acts on it | **Chosen.** The software demodulator on the 8 kHz copy, the path APRS receive already runs. Unmeasured: its low-frequency response with the bypass bits |
| The prompt FIFO's ring play | Fallback only: a 290 Hz high-pass measured on it, which leaves 4FSK an error floor of several percent even restored (2.5) | |

So the first radio build sends and receives through the flat paths, sharing D-STAR's glue, and the physical layer
transmit is the second step if the flat transmit route proves poor on the bench: `ysf_engine_tx_block` already streams
the transmission in the chip's 144-symbol blocks across Fusion's frame boundaries, and the receive side stays on the
flat path either way.

### 1.3 Settings

The flat paths' register choices are D-STAR's, as the radio's hardware page lists them. Fusion adds one: the channel
runs at the **25 kHz setting**. Wide C4FM's outer symbols are 2.7 kHz and the shaped waveform peaks near 4 kHz, while a
12.5 kHz setting's FM limiter is set for voice at about 2.5 kHz; the simulated 12.5 kHz receive filter costs nothing
(2.5), so the bench may narrow it once the limiter's effect on the flat route is known.

### 1.4 Cost

An HR_C7000 radio's build, csky-elf-gcc 14.2, `-Os`, the multimode variant (which leaves the IP gateway out and so turns Fusion on), against
the same build with `CONFIG_YSF=n`:

| Item | Cost |
|---|---|
| Flash | 18,436 B (482,196 against 463,760: 53.5 %). This includes the flat path glue (`CONFIG_FM_FLAT`), which a build with D-STAR has anyway |
| RAM, static | 2,448 B (179,720 against 177,272: 85.0 %): the engine 952 B (receive and transmit share a union: the demodulator 368, the receiver 560 of which 360 hold soft bits; the transmitter 296), the voice rings 224 B, the view in the radio view and its copies, the flat path glue |
| Stack | the soft Viterbi decoder's survivors, 360 B, at the deepest (a DCH unit decoded from the demodulator through the receiver, in the radio task) |
| CPU | estimated, not measured: the matched filter is about 20 multiply-adds at each of 9,600 instants a second (the symbol and the half-symbol for the clock), under 1 % of 168 MHz; the Viterbi decoder about 30 k cycles per DCH unit, 10 a second; the modulator about 12 table lookups a sample at 8 kHz |
| Voice codec | none: the board's DMR vocoder, unchanged |

Voice FR (`CONFIG_YSF_VW`, on with Fusion) adds, measured the same way on the multimode build (`CONFIG_YSF_VW=n`
against on): 16,116 B of flash (552,224 to 568,340; 63.1 % of 880 KB) and 4,152 B of static RAM (182,528 to 186,680;
24.8 KB still free). The RAM is the codec's 3,964 B (4.3) and the larger voice queue slots (13 bytes against 7, 16
slots each way); the flash is `core/mbe` (decoder, encoder, synthesis, tables) and `ysf_vw.c`. CPU and stack: 4.3.
Since PR #833 the codec's 3,964 B are the DMR vocoder's lent state block for the length of a VW over, not static RAM
(4.3): the multimode build went from 184,816 to 183,736 B (the shared area is now D-STAR's size), and the same radio's
image that keeps the IP gateway and adds Fusion (`CONFIG_YSF=y`) from 214,032 B, which no longer fit, to 210,072 B,
then 206,416 B (5 KB free) with three cold task-side buffers moved to the board's cold RAM (`RFOUNDRY_COLD_RAM`).

Both sets of figures were measured with 16 slots each way. The transmit queue now holds 32 (`RADIO_YSF_TX_Q`): from
PTT to the first voice subframe go the preamble, both headers and, with VW, the CSD3 frame's three data subframes,
about 16 frames of speech already encoded, and 16 slots dropped one frame of every VW over once the preamble came in.
That is 16 more slots: 112 B of RAM with DN only, 208 B with VW.

The default image (with the IP gateway) doesn't build Fusion and keeps its 0.5 KB of free RAM.

**Sharing per-mode state.** Only one of DMR, D-STAR and Fusion runs at a time. D-STAR's codec borrows the DMR
vocoder's state RAM while D-STAR runs (`hal_vocoder_t.lend`); Fusion runs DN on the vocoder itself, so it borrows the
block only for a VW over (4.3). Fusion's engine (`ysf_engine_t`, 952 B on the host) and D-STAR's engine and codec state share
one area, the size of the larger, that the radio core lends to the mode that runs (`core/radio/radio_mode_ram.c`): each
module takes it when its channel is tuned and gives it back when it stops, and a take while the other mode holds it is
refused rather than granted, since the holder's transmit interrupt may still pull from its engine. The core stops every
mode before starting one (`tune()`), which hands both loans over in order.

### 1.5 What only a radio can settle

| Question | First test ([ysf-bench.md](../testing/ysf-bench.md)) |
|---|---|
| The flat transmit route's response and the deviation scale (`tx_1khz`) for 900 Hz symbols | Our transmission on the SDR: `tools/ysf_ref.py decode` reports the inner and outer levels in Hz and their ratio (3 is right) |
| ~~The DCH whitening order~~ | Answered (1.6): figure 4-27's, what the transmitter sends |
| ~~The CRC's bit order within its 16 bits~~ | Answered (1.6): as 3.2 |
| Polarity of each path | The receiver finds it from the sync (`ysf.rx_invert`); transmit: whether a Yaesu radio decodes us |
| Whether radios carry the DG-ID in the FICH squelch code | A Yaesu radio set to DG-ID 20: `ysf.dgid` reads 20; ours set to 20 opens its DG-ID 20 squelch and not its 30 |
| Whether a Yaesu radio decodes our levels (wide) and its own receiver's tolerance | The visiting radio shows our callsign and plays our voice |
| The header's FN / FT (the standard doesn't define them for a header; we send FN 0, FT 5 with DN; a Yaesu radio sends FT 6, 1.6. With VW both send FN 0, FT 0) | The Yaesu radio's display of our header; a repeater passing us |
| ~~The destination a Yaesu radio sends for a CQ call~~ | Answered (1.6): radio ID mode, `*****` and its radio ID. We send the same, with spaces for the ID when none is set (3.5) |
| V/D mode 1's 72-bit frame: the DMR frame's bit order (3.4) | A Yaesu radio set to VD1 (if it can be: most send DN), the voice clear |
| Turnaround times on the flat route | The preamble and the header's two copies cover key-up; the SDR shows how much of them is on air |
| The receive path's low-frequency response | A Yaesu transmission's sync bit errors (`ysf.sync_errors` / `ysf.sync_bits`) at a strong signal: 0 on a flat path; a floor means a high-pass (set `rx_hpf_hz`, 2.4) |
| VW received on the radio: the full-rate decoder in real time beside the demodulator, and inside the speaker FIFO's 4 ms (4.3, from a model of the core) | A Yaesu radio in VW: the voice plays without gaps; the audio section's `ysf.vw_dec_max_us` under 4,000, `audio.cb_long` and `ysf.rx_late` stay 0 during the over |
| Whether a Yaesu radio plays our VW (4.1: the layout is read off one radio's transmission, ours matches it bit for bit in the host test) | A Yaesu radio hears our VW over: callsign shown, voice clear |
| The full-rate encoder's speed on the radio (4.3, from a model of the core) | Our VW over on the SDR, decoded by the bench: no silence frames in the speech (`ysf.tx_underruns` stays 0) |

### 1.6 What a Yaesu radio sends

The first real recordings: a Yaesu handheld (FT-5D) on 2 m simplex, four DN overs (619 frames) and three VW overs
(511 frames), received by an SDR on an outdoor antenna and decoded with `tools/ysf_ref.py`'s coding. Every FICH
decoded; the BER estimate was at most 2e-4. The recordings hold another station's callsign and voice and are not
committed; the layout they showed is in `tests/vectors/ysf.txt`'s `yaesu` line with made-up content (6).

| Item | The Yaesu radio | RadioFoundry |
|---|---|---|
| FICH CRC bit order | as 3.2 | as 3.2 |
| DCH whitening order | whitening before the CRC (figure 4-27): every unit (`order_all` stayed 0) | the same |
| Polarity | +3 is the higher frequency (normal) | the same |
| Levels | +-819 / +-2499 Hz: 0.91 of wide, ratio 3.05; FICH Dev 0 | +-900 / +-2700 Hz by the board's scale |
| CS, BN, BT, MR, VoIP, reserved bit 15 | 2, 0, 0, direct, 0, 0 | the same |
| SQL, SC | 0, 0 with its DG-ID 0 | 0, the DG-ID sent |
| CM | 1, radio ID mode, in every frame of a call to everyone | the same (3.5) |
| CSD1 destination | `*****` (the destination radio ID: everyone) and its own radio ID | the same when a radio ID is set |
| CSD2, Rem1-Rem3 | spaces | spaces |
| Rem4 | its radio ID (DN FN 5; VW CC0) | the radio-wide radio ID |
| Header and terminator FN / FT | 0 / 6 (DN), 0 / 0 (VW) | 0 / 5 (DN), 0 / 0 (VW, 4.1) |
| DN rolling pattern | FT 6: FN 0-5 as table 4-4, FN 6 ten bytes of data (DT1) | FT 5: no data, a pattern the standard defines |
| DT1 | seven bytes, the seventh the sum of the six before it, then three zeros; the first and the sum changed by 2 from over to over, the five between stayed the same. Not described by the standard | none |
| Key-up | carrier, about 50 ms of it unmodulated after the synthesizer settles, then 240 symbols (50 ms) alternating -3 / +3, the last +3, straight into the header's sync | the same preamble (3.5) |
| Headers | one | two (3.5) |
| Key-down | the terminator, then 35-45 ms of unmodulated carrier | the terminator, then the flat route's latency of silence |
| Frame timing | 100.000 ms, the symbol clock within the capture's resolution | |

The DT1 data is the one difference left: its meaning isn't public, and the standard's FT 5 (no data) is what a
receiver must handle anyway. The receiver keeps FN 6 out of the callsign fields, also when it has to assume that
frame's number after a lost FICH (from the FT the frames carry, 3.6).

## 2. The modem

### 2.1 Parameters

| | Value | Source |
|---|---|---|
| Modulation | C4FM: four-level FM | Y 3.1 |
| Symbol rate | 4800 symbols/s (9600 bit/s), within +-5 ppm | Y 3.1, 3.3 (7) |
| Levels | +1, +3, -1, -3 = dibits 00, 01, 10, 11, the dibit's first bit sent first | Y 3.2 table 3-1 |
| Deviation | wide (FICH Dev 0): +-900 / +-2700 Hz; narrow (Dev 1): +-450 / +-1350 Hz; at most +-4950 Hz | Y 3.2 table 3-1, 3.3 (8) |
| Pulse | root raised cosine, roll-off 0.2, the same filter on transmit and receive | Y 3.2 (3), (4) |
| Frame | 480 symbols, 100 ms | Y 4.1 |

### 2.2 Modulator

Each symbol adds the root raised cosine pulse times its level, from 6 symbols before its center to 6 after (the cut
leaves intersymbol interference of 0.015 of a level when both ends use it; at 4 symbols it was 0.083). The pulse is a
385-point table, 32 points a symbol, interpolated linearly; `tools/ysf_ref.py rrc` makes it and the host test checks
the output against the formula to 0.3 % of a +1 level. The amplitude is the sample value of a +1 symbol at the matched
filter's output; the waveform itself peaks near 4.5 times it. A transmit restorer (2.4) undoes a path's high-pass.

### 2.3 Demodulator

| Stage | What | Why |
|---|---|---|
| Restorer | the receive path's high-pass undone, when the board set one (2.4) | |
| Matched filter | the pulse evaluated at each symbol instant straight from the input samples: the samples within 6 symbols, each weighted by the pulse at its distance from the instant | the pulse is band-limited to 2.88 kHz, so 8 kHz samples hold all of it and no separate interpolator is needed |
| Symbol clock, searching | Gardner: the value half a symbol before the instant times the change across it, scaled by the level squared | needs no decisions; its pattern noise with roll-off 0.2 jitters the clock about 0.06 of a symbol, enough to find a sync |
| Symbol clock, locked | Mueller and Mueller: the previous value against this decision less this value against the previous decision; a proportional and an integral path, the integral (the transmitter's clock error, +-1000 ppm) only while locked | little pattern noise: a clean wire comes to 0.017 of a level rms |
| DC and level, searching | the envelope: the highest and lowest symbol values (each following a value beyond it at 1/4 and the rest at 1/128) are the outer levels; their middle is the DC, a sixth of their span a +1 level | a plain 64-symbol average swung 0.4 of a level on the data; a longer one missed both headers of a carrier 1 kHz off |
| DC and level, locked | decision directed: the error from the nearest level moves the DC at 1/16 a symbol and the level at 1/256 | 4FSK has as much power near DC as anywhere: a fast DC loop follows what a path's high-pass leaves |
| Output | the value at the instant less the DC, and the tracked +1 level, to the receiver | the receiver makes soft bits from both |

The loops' arithmetic divides rather than shifts, and the clock integrator keeps 8 fractional bits: a shift's rounding
held the clock 0.13 of a symbol late, and without the fraction the integrator read 300 ppm as 36.

### 2.4 Low-frequency restorer

H(s) = (s + 2 pi fc) / (s + 2 pi ff): unity above fc, a gain of fc / ff at DC, built as the input plus a leaky
integrator (as D-STAR's, [dstar-modem.md](dstar-modem.md) 2.4; Fusion's own copy, since the two modes build apart).
The floor defaults to 30 Hz: what the DC loop then follows with a small error floor.

### 2.5 Measured on a simulated link

`ysfModemTest --sweep`: the transmitter's dibits through the modulator at 48 kHz (or at 8 kHz and interpolated, as the
radio's flat path does), an FM modulator, white noise at Eb/N0 (Eb = half a symbol's energy), a 12.5 kHz channel filter,
a discriminator, a 3.6 kHz audio filter and 8 kHz samples into the demodulator and receiver. Each cell is four
transmissions of 250 voice frames (50 V/D frames); the BER is over the voice frames of those caught from their header:
**raw** over the 22 bits V/D mode 2 sends once (the channel's own bit error rate), **voice** over all 49 after the
repetition vote (what the codec gets). In brackets: transmissions caught from their header, when fewer than four (the
rest by late entry, whose voice can't be lined up with what was sent).

| Path | 6 dB | 8 dB | 10 dB | 12 dB | 14 dB | 16 dB | 18 dB |
|---|---|---|---|---|---|---|---|
| Clean | 6.2e-2 / 3.6e-2 (2/4) | 2.1e-2 / 1.0e-2 | 5.2e-3 / 2.3e-3 | 1.0e-3 / 4.5e-4 | 9.1e-5 / 4.1e-5 | 0 | 0 |
| +1 kHz carrier offset | 7.4e-2 / 4.5e-2 (2/4) | 2.2e-2 / 1.1e-2 | 6.2e-3 / 3.0e-3 | 1.3e-3 / 5.9e-4 | 4.5e-5 / 2.0e-5 | 0 | 0 |
| 500 Hz drift over the transmission | 5.9e-2 / 3.3e-2 (3/4) | 2.1e-2 / 1.0e-2 | 6.3e-3 / 2.9e-3 | 6.4e-4 / 2.9e-4 | 0 | 0 | 0 |
| +200 ppm symbol clock | 6.0e-2 / 3.5e-2 | 2.0e-2 / 9.6e-3 | 5.5e-3 / 2.5e-3 | 7.3e-4 / 3.3e-4 | 0 | 0 | 0 |
| Modulator at 8 kHz | 1.8e-1 / 1.6e-1 (3/4) | 2.1e-2 / 1.1e-2 | 4.7e-3 / 2.2e-3 | 9.5e-4 / 4.3e-4 | 1.4e-4 / 6.1e-5 | 0 | 0 |
| DMR modem's levels (0.72 of wide) | 1.1e-1 / 6.5e-2 (2/4) | 4.9e-2 / 2.3e-2 | 2.6e-2 / 1.2e-2 | 1.0e-2 / 4.5e-3 | 1.8e-3 / 8.0e-4 | 9.1e-5 / 4.1e-5 | 0 |
| Narrow deviation (0.5 of wide) | 4.0e-1 / 3.8e-1 (1/4) | 1.1e-1 / 5.1e-2 | 6.4e-2 / 2.9e-2 (3/4) | 3.6e-2 / 1.6e-2 | 1.6e-2 / 7.1e-3 | 4.9e-3 / 2.2e-3 | 9.1e-4 / 4.1e-4 |
| RX 30 Hz high-pass | 8.4e-2 / 5.0e-2 (1/4) | 3.8e-2 / 1.8e-2 (3/4) | 2.0e-2 / 9.1e-3 | 1.2e-2 / 5.6e-3 | 6.2e-3 / 2.8e-3 | 3.5e-3 / 1.6e-3 | 1.8e-3 / 8.4e-4 |
| RX 100 Hz high-pass | 1.5e-1 / 8.6e-2 (1/4) | 1.1e-1 / 5.6e-2 (3/4) | 9.4e-2 / 4.7e-2 | 8.8e-2 / 4.4e-2 | 8.5e-2 / 4.3e-2 | 8.4e-2 / 4.3e-2 | 7.7e-2 / 3.9e-2 |
| RX 300 Hz high-pass, restored | 9.2e-2 / 5.4e-2 (1/4) | 4.3e-2 / 2.1e-2 | 2.5e-2 / 1.1e-2 | 1.6e-2 / 7.4e-3 | 8.5e-3 / 3.9e-3 | 7.0e-3 / 3.2e-3 | 3.3e-3 / 1.6e-3 |
| 16 kHz channel filter | (0/4) | 3.0e-2 / 1.7e-2 | 6.2e-3 / 3.0e-3 | 1.0e-3 / 4.5e-4 | 9.1e-5 / 4.1e-5 | 0 | 0 |

AMBE+2 stays usable to a voice BER of about 2e-2, so a clean path is good to about 8 dB. Carrier offset, drift and a
clock error cost under 0.5 dB; the 8 kHz modulator nothing measurable above 8 dB. The DMR modem's levels cost about
2.5 dB and narrow deviation about 6 dB (the levels are closer together; the noise is the same). A high-pass in the
receive path is what hurts: 4FSK carries as much power near DC as anywhere in its band, so 100 Hz leaves an error floor
near 8 %, and even restored to 30 Hz a 300 Hz high-pass leaves about 3e-3. The bench's first receive test reads the
path's response for this reason (1.5). The host test asserts margins over these at 14 dB.

## 3. Framing

### 3.1 The frame (Y 4.1, 4.2)

| Part | Bits | Content |
|---|---|---|
| FS | 40 | 0xD471C9634D, left to right |
| FICH | 200 | 3.2 |
| Payload | 720 | five subframes of 144 bits, laid out by the frame type and data type (3.3) |

Bits go on air most significant first: a byte of a callsign from its top bit down, the FICH from bit 31.

### 3.2 FICH (Y 4.2.1 (4), 4.5 (1))

| Bits (31 first) | Field | Values |
|---|---|---|
| 31-30 | FI | 0 header, 1 communication, 2 terminator, 3 test |
| 29-28 | CS | 2: callsigns 1, 2, 3 (the only defined value) |
| 27-26 | CM | 0 group / CQ, 1 radio ID, 3 individual |
| 25-24, 23-22 | BN, BT | block number and total (data) |
| 21-19, 18-16 | FN, FT | frame number, and the last frame number of the rolling pattern |
| 15 | | reserved: written 0, not kept |
| 14 | Dev | 0 wide, 1 narrow |
| 13-11 | MR | 0 direct, 1 downlink (uplink not busy), 2 downlink (uplink busy) |
| 10 | VoIP | 1 via the internet |
| 9-8 | DT | 0 V/D mode 1, 1 Data FR, 2 V/D mode 2 (DN), 3 Voice FR (VW) |
| 7 | SQL | 0 squelch code on, 1 off |
| 6-0 | SC | squelch code 1-126, 127 break out. Radios' DG-ID is taken to be this field (sent with SQL 0); the standard predates the name, so 1.5 lists it |

Coding: a CRC-16 (x^16 + x^12 + x^5 + 1, registers from zero, the result inverted, appended top bit first: the catalog's
CRC-16/GSM, "123456789" gives 0xCE3C); the 48 bits as four 12-bit words, each an extended Golay (24,12) code word by
appendix A's matrix (data first, then the parity, p11..p0 = the rows C75 63B F68 7B4 3DA D99 6CD 367 DC6 A97 93E 8EB);
four zero tail bits; the rate 1/2, K = 5 convolutional code (G1 = 1 + x^3 + x^4 then G2 = 1 + x + x^2 + x^4 for each
bit, appendix B); 200 bits as 100 dibits written into rows of 5 and read by columns (the air's dibit k is coded dibit
(k % 20) x 5 + k / 20). The text says "depth 10"; 100 dibits need 20 rows, and figure 4-26 draws 20. No whitening.

The receiver decodes the convolutional code with a soft Viterbi decoder (16 states, 4-bit soft values), the Golay words
up to three errors each (four are always detected), then checks the CRC.

### 3.3 Payload layouts

| Frame | Each of the five subframes | The units |
|---|---|---|
| Header, terminator | DCH-1 72 bits, DCH-2 72 bits | CSD1 (destination, source) and CSD2 (downlink, uplink), each a 160-bit unit coded to 360 bits and cut into five |
| V/D mode 2 (DN) | DCH 40, VCH 72, VeCH 32 | one 80-bit DCH unit coded to 200 bits; a voice frame's 104 bits in VCH and VeCH |
| V/D mode 1 | DCH 72, VCH 72 | one 160-bit DCH unit; the codec's 72-bit frame with its own FEC |
| Voice FR (VW) | VCH 144: one full-rate voice frame | the first communication frame after the header: CSD3 in subframes 0-2 (4.1) |
| Data FR | | recognized by the FICH, not decoded (4) |

A DCH unit (Y 4.5 (2)): whitened by PN(9,5) (S08..S00 from 111001001, out S00, feedback S00 XOR S04 into S08: the
sequence starts 93 D7 51 21 9C 2F 6C D0), a CRC-16 as the FICH's, four zero tail bits, the convolutional code, the
coded dibits into rows of 9 (160-bit unit) or 5 (80-bit) and read by columns. Figure 4-27 puts the whitening before the
CRC (the CRC over the whitened data); figures 4-30 and 4-31 leave it out. The receiver accepts either order and counts
which it saw (`order_all`); the transmitter uses figure 4-27's until a Yaesu radio's transmission says otherwise.

The V/D mode 2 voice (Y 4.5 (3)): the 49 codec bits, the first 27 three times each (81 bits), the other 22 once, a zero
(104), whitened by the same sequence, written into four rows of 26 and read by columns (the air's bit k is bit
(k % 4) x 26 + k / 4); the first 72 in VCH, the last 32 in VeCH. The receiver votes each triple by the sum of its soft
values and takes the other 22 by sign.

### 3.4 Callsign data (Y 4.2.1 (5), 4.2.2 (6))

| Unit | Content |
|---|---|
| CSD1 | destination (10 characters, space filled; in radio ID mode (CM 1) the destination's radio ID, `*****` for everyone, then the source's radio ID), source callsign (10) |
| CSD2 | downlink callsign, uplink callsign (10 each; spaces when direct) |
| CSD3 | Rem1 downlink ID, Rem2 uplink ID, Rem3 the VoIP station's ID, Rem4 the source's radio ID (5 each) |

V/D mode 2 carries ten bytes a frame by FN: 0 destination, 1 source, 2 downlink, 3 uplink, 4 Rem1 + Rem2, 5 Rem3 +
Rem4, 6-7 data (FT 5 without data). The receiver shows a radio ID mode destination as the destination ID, `ALL` for
`*****` (`ysf_dest_show`): the field's second half is the caller's own ID, not who was called. V/D mode 1 carries 20 a frame: FN 0 CSD1, 1 CSD2, 2 CSD3. A receiver that joins
late has the caller's callsign within two frames.

V/D mode 1's voice is the codec's 72-bit frame with DVSI's own FEC, which Yaesu doesn't describe; it is taken to be
DMR's 72-bit frame (the same codec, AMBE+2 3600x2450) and decoded with the DMR FEC (`ambeFec.h`), unconfirmed (1.5).

### 3.5 The transmission

| Part | Content |
|---|---|
| Preamble | 240 symbols (50 ms) alternating -3 / +3, ending on +3 so the alternation runs into the sync's first symbol: what a Yaesu radio sends after its key-up (1.6). The standard has no preamble; a receiver's clock and level loops settle on it instead of in the header. On the simulated link at 10 dB (`ysfModemTest`, 40 overs each), one header after the preamble was caught 39 times, without it 9 |
| Headers | two (the standard recommends one, Y 4.6, and a Yaesu radio sends one): the second covers the flat route's unmeasured key-up. FI 0, FN 0, FT 5, DT 2 |
| V/D mode 2 frames | FN 0 to 5 rolling (FT 5), each with its DCH unit for that FN and five voice frames, each pulled from the codec as its subframe is reached (the last 80 ms after the first) |
| Terminator | as the header with FI 2, after PTT is released and the queued voice has gone out (the frame it ends in is finished with the codec's silence) |

Every frame: CS 2, MR direct, VoIP 0, Dev from the channel, SQL 0, SC = the channel's DG-ID. A call to everyone
(destination `ALL`) goes as a Yaesu radio's (1.6): CM radio ID, the destination field `*****` and our radio ID, five
spaces when none is set (Rem4 too). To a named station or group, CM group and the destination as set. Until PR #833
a call to everyone without a radio ID went as CM group to `ALL`: the standard allows it, but no Yaesu radio sends it
(each has a radio ID), and a Yaesu FTM-510DE showed no caller for it, with the source callsign in its usual place.

### 3.6 Receive

| State | Leaves on |
|---|---|
| Search | the frame sync with 3 of its 40 bits wrong or fewer, in either polarity (+3 and -3 swapped flip the first bit of each dibit): FICH |
| FICH | 100 dibits decoded; CRC good: the frame's payload (a new transmission on the first good one). Bad: in a call with the sync where it was due, the next frame of the rolling pattern is assumed (its voice still plays); otherwise Search |
| Payload | 360 dibits, decoded as they arrive (each voice frame at the end of its subframe, the DCH unit at the end of the frame); a terminator ends the call |
| Sync | where the next sync is due, 8 of 40 bits wrong accepted; four missed in a row (Y 4.7: N2 = 4) lose the call |

Ten minutes of noise in the host test gave no sync within 3 bits; a false one also needs a FICH with a good CRC.

## 4. Voice

DN's voice is the AMBE+2 3600x2450 codec's 49 parameter bits, the codec DMR uses; RadioFoundry runs the radio's own
(`hal_vocoder.h`), unchanged, in the module's audio callback. The transmitter needs no codec of its own for silence: the
vocoder's encoding of a silent block is taken at the first transmission. The audio callback writes it while that
first over is already on air, so until the whole frame is written the transmitter sends 49 zero bits in its place,
never a frame half written.

The mic samples reach DN's encoder through the mic gain's PCM gain and peak limiter, as DMR's do
([radio-features.md](radio-features.md) 3.6): the board's `hal_radio_t.mic_pcm_db` plus the Mic gain step where the
board doesn't take it. Without it one radio's DN speech decoded at -31 dBFS against a Yaesu radio's -18 dBFS at the
far end. `ysf.tx_mic_peak tx_enc_peak tx_mic_low_db` give the latest over's mic peak, the encoder's peak after the
gain, and the limiter's lowest gain in dB (`-` before the first over). VW's encoder takes the samples as delivered.

Voice FR (VW) is 4.1. What the modes not built would need:

| Mode | Needs |
|---|---|
| Data FR | the DCH-1 / DCH-2 pairs of 160-bit units and the block management (Y 4.3); the coding is the same as V/D mode 1's DCH, so the decoder exists; what Yaesu radios put in it (pictures, messages) is not public |
| Wires-X | no public air or network protocol; on air a Wires-X node is a repeater (MR, CSD2, CSD3), which the receiver already shows. Its "X" button commands are not documented |

### 4.1 Voice FR (VW)

The standard gives VW's rate (Y 4.4: 7200 bit/s, 88 voice and 56 FEC bits each 20 ms) and nothing of its frame. The
88 + 56 is P25 Phase 1's full-rate voice frame (TIA-102.BAAA 5.3, TIA-102.BABA): four Golay (23,12) words carry
12 bits each (4 x 11 parity bits), three Hamming (15,11) words 11 each (3 x 4), seven bits go bare: 88 + 44 + 12 = 144.
The layout below was found by trying that frame against a real radio's VW transmission (an FT-5D, 120 s on the SDR
bench, 511 frames, every FICH good, a raw bit error rate near 1e-5): with it every voice frame's seven code words have
a zero syndrome but one bit in 2,524 frames, and the other hypotheses tried (YSF's PN (9,5) whitening on top, the
words in order without the interleave, both) gave no valid u0 word at all.

**A VW communication frame** (FI 1, DT 3): the payload's five 144-bit subframes are five full-rate frames, one per
20 ms, each coded exactly as P25 codes its voice:

| Step | What |
|---|---|
| Words | u0..u3 12 bits, u4..u6 11, u7 7: the vocoder's 88 bits in order |
| FEC | c0..c3 = Golay (23,12) of u0..u3 (the extended code of 3.2 without its last parity bit: TIA-102.BAAA table 5-3 prints the same matrix as Yaesu's appendix A); c4..c6 = Hamming (15,11) of u4..u6 (table 5-4: the parity of each data bit F E D C B A 9 7 6 5 3); c7 = u7 |
| Scrambling | c1..c6 XORed with a sequence seeded from u0: p(0) = 16 u0, p(n) = 173 p(n-1) + 13849 mod 65536, bit 15 of p(1), p(2), ... in turn, 23 bits for each Golay word, 15 for each Hamming word. c0 and c7 are sent as they are |
| Interleave | P25's voice interleave (TIA-102.BAAA table 5-1): the 144 bits as 72 dibits, each from fixed bits of the words (`core/ysf/ysf_vw.c` `il`, `tools/ysf_vw_ref.py` `INTERLEAVE`) |
| Whitening | none: YSF's PN (9,5) is not applied |

**The first communication frame after the header** (FN 0 of FT 1) carries CSD3 (Rem1-Rem4, the four radio IDs,
table 4-2) in place of its first three voice frames: payload bits 0-359 are one 160-bit DCH unit coded as any other
(3.3: whitened, CRC, the convolutional code, rows of 9; the CRC after the whitening, as the header's units), bits
360-431 are 72 zeros, and subframes 3-4 are voice. It is the only frame with data: the radio sends no callsign data in
the voice frames, so a receiver that joins late has the callsigns only from the terminator. Nothing else in a VW frame
names the caller (the FICH has the call mode, DG-ID, deviation and frame numbers). The radio then shows the caller's
radio ID from CSD3 (Rem4, `ID E0abc`; debug link `ysf.src_id`) when it caught that frame, otherwise `Caller unknown`
and the destination `unknown`, until the terminator brings the callsigns. Of the three replay clips cut from the FT-5D's VW
capture for on-air tests, the first has neither the header nor the CSD3 frame (the capture began during that over); the
other two have both.

**Frame numbers.** The FT-5D sends its header and terminator with FN 0 FT 0 and DT 3, then two frames FT 1 (FN 0 with
CSD3, FN 1) and FN 0 FT 0 for the rest of the over. Its other fields as DN's from the same radio: CM 1 (radio ID, the
destination `*****` and its own ID), Dev wide, MR direct, VoIP 0, SQL 0, SC 0. The transmitter sends this pattern
(`ysf_vw_tx_fn`); the receiver takes CSD3 from the frame whose FICH says FN 0 FT 1 and, with a FICH lost, assumes the
next frame of this pattern (`ysf_vw_rx_next`) rather than DN's rolling count, which would read voice as CSD3.

**Receive.** Each voice subframe is decided bit by bit and decoded: the Golay words correct up to three errors each,
the Hamming words one. The corrections go with the 88 bits to the codec as the P25 decoder takes them (the total, those
in c0 and those in c4: `YSF_VW_ERRS`, the codec's `IMBE_ERRS`), whose rules repeat a frame whose u0 word took two or
more corrections in a frame of eleven or more (u0 seeds the other words' scrambling, so a wrong u0 makes the rest
garbage), and mute past 8.5 % corrected. The FT-5D capture: 2,524 voice frames, 1 bit corrected, none repeated.

**Transmit.** A Fusion channel says DN or VW (the flag in [ysf-codeplug.md](ysf-codeplug.md) 2.1; DN when clear).
With VW, PTT sends headers and terminator with DT 3 as above, CSD3 in the first communication frame (our radio ID in
Rem4), and the encoder's full-rate frames, each pulled as its subframe starts. With none ready the codec's silence frame
goes (pitch code 216). The callsign data of the header and terminator are DN's (3.5).

**Codec.** The full-rate vocoder (7200 x 4400) is upstream RadioFoundry's `core/mbe`: an integer decoder and encoder on
an MBE synthesis core (no float, no division in the per-sample loops). Its products and its synthesis loops are
reworked for a core without a wide multiply (4.3), its output upstream's bit for bit (`mbeExactTest`). It runs in the radio
core's audio block callback, where a DN over runs the board's AMBE+2 vocoder (`radio_ysf_vw.c`), so VW needs no codec
in flash: a radio without the stock codec receives and sends VW (PTT on a DN channel still needs it). The bench decoded
the FT-5D's 2,524 frames with it and with mbelib's decoder (ISC, as a reference): the two outputs' frame energies
correlate at 0.976, no frame was concealed, the level a median of about 2,600 rms on speech (-22 dBFS), 3 samples of
403,840 at full scale; STOI 0.939 against mbelib's output as the reference (the bench's STOI, written from Taal et al.
2011, 8 kHz), 0.8 dB quieter (rms 2,822 to 3,075).

### 4.2 Bench

A capture's VW frames were decoded off line through the same `core/mbe` decoder to check the layout above against a
Yaesu radio's transmission (the tests in section 6 use synthetic frames only).

### 4.3 Cost

| Item | VW |
|---|---|
| Flash | 16,116 B on the multimode build (1.4), and 3,316 B more since the decoder was made faster (568,708 to 572,024 B; the cosine table 2 KB) |
| RAM, static | 4,152 B: the codec 3,964 B (the estimator's history 516, the decoder's state 132, a frame's parameters 176, the synthesis core's state 516, its last and current frame 288 each, its scratch 2,048), the larger queue slots 192 B. Where the platform's vocoder lends its state block (`HAL_VOCODER_LEND_BYTES`, the HR_C7000's 7 KB) the codec is that block, taken per over, not per channel, since a DN over may follow at any moment: the audio callback takes it at a VW over's first frame (or a VW transmission's first block) and starts the codec there, and lets it go before its next call of the vocoder, which then rebuilds its own state as after any lend; the queue slots are then all the static RAM VW adds. Elsewhere the codec is in the mode area (`radio_ysf.h` `RADIO_YSF_VW_IN_AREA`). The encoder works in the decoder's state and scratch (one direction at a time); only the estimator's history is its own |
| Stack | the audio task: 1.8 KB at the deepest through the encoder (with every 64-bit library call charged 256 B; the codec makes none now), under the stock codec's 4.1 KB that task already allows for |
| CPU | measured in a model of the core, not on a radio: below |

The CPU figures: the codec built for the CK803S as the firmware builds it (csky-elf-gcc 14.2, `-Os`, `-mcpu=ck803s`),
run instruction by instruction in a model of that core on the FT-5D capture's 2,524 frames (decode) and 300 frames of
the speech decoded from it (encode). The model counts what the code executes; its cycles take each instruction as 1, a
load 2, a taken branch, call or return 2 more, a multiply 1 (the multiplier's speed on this core is not documented: 4
cycles adds about 15 %), push and pop 1 per register, and no flash wait states. Its output matched the host decoder's
sample for sample on every frame, so the code measured is the code that runs. Of 168 MHz (3.36 M cycles a 20 ms
frame):

| | Cycles a frame, mean / 99th percentile / worst | Of 168 MHz, mean / worst | Worst frame |
|---|---|---|---|
| Decode, upstream's code | 726 k / 1,276 k / 2,025 k | 21.6 % / 60.3 % | 12.1 ms |
| Decode, products from 16-bit halves | 461 k / 808 k / 1,273 k | 13.7 % / 37.9 % | 7.6 ms |
| Decode, and the synthesis loops | 293 k / 470 k / 641 k | 8.7 % / 19.1 % | 3.8 ms |
| Encode, upstream's code | 586 k / 677 k / 701 k | 17.4 % / 20.9 % | 4.2 ms |
| Encode now | 383 k / 465 k / 480 k | 11.4 % / 14.3 % | 2.9 ms |

The CK803S has no 32 x 32 -> 64 multiply (csky-elf-gcc names the core with the DSP extension's HI/LO unit ck803se, and
the radio's is a ck803s), so `(int64_t)a * b` was a `__muldi3` call of six multiplies; on the worst frames those were
37 % of the decode. `core/dsp/fxp.h` now makes those products from 16-bit halves on such a core (`fxp_mul64_h`,
`fxp_mulshr_h`, exactly the 64-bit results), and two multiplies for a 16-bit factor (`fxp_mulshr16`). The synthesis's
oscillators (one per voiced harmonic per sample, both sets across a pitch jump) take a full-wave cosine table, one
32-bit multiply where the amplitude allows and two where not (17 instructions a sample, against about 70 with the
library call). The worst frame is a pitch jump with unvoiced bands: the oscillators 243 k instructions, the two FFTs
113 k. A cheaper synthesis (D-STAR's 16 x 16-bit oscillators, `dstar-codec.md`) was not taken: it would change the
output, and this keeps upstream's.

Why the worst frame matters more than the mean: the decoder runs in the audio block callback, and on the HR_C7000
nothing feeds the speaker FIFO while it runs. The FIFO holds two 4 ms chunks and may hold only one when the callback
starts (8 ms when it is full), so a block longer than 4 ms can let it run dry, a gap, and one over 8 ms will. By the
model upstream's code took more than 4 ms on 44 % of the capture's frames and more than 8 ms on 14 of its 2,524 (up to
12 ms), which fits the "few small stutters" of the first VW over heard on a radio. The worst is now 3.8 ms, inside the
4 ms but not by much, and the model leaves out flash wait states; the radio shows which (`audio.cb_max_us`,
`audio.cb_long`, `ysf.vw_dec_max_us`: [debuglink.md](debuglink.md), the audio section). If it still runs over, the next step is to take
the decode out of the callback: decode ahead at a lower priority than the FIFO's feed.

One direction runs at a time. The C4FM demodulator and receiver add under 1 % (1.4), in the radio task, below the audio
task.

## 5. The engine and the radio core

`ysf_engine_t` runs one side at a time: `ysf_engine_rx` with discriminator samples (or `ysf_engine_rx_dibits` with a
chip's decided dibits), `ysf_engine_tx` filling samples for a flat path (or `ysf_engine_tx_block` 144 dibits for a
chip's physical layer). Its path (`ysf_path_t`): the rate, the sample value of a +1 symbol, the polarity each way, the
high-pass to undo each way. `radio_ysf.c` takes them from the board's flat path calibration with the bench's settings
over it (`radio_flat.h`, the same function D-STAR uses: the calibration is the paths', so a scale measured on one mode
holds for the other). A change from the debug link or Settings re-tunes a Fusion channel as it does a D-STAR one.

`radio_ysf.c` runs a Fusion channel through the same hooks in `radio_sm.c` as D-STAR's module: the channel is tuned as
FM (25 kHz, no sub-audio), the module opens the squelch on a good FICH and closes it on the terminator or a lost sync,
plays the voice (a DG-ID heard other than 0 plays only transmissions of that DG-ID; the others are shown; DN and V/D
mode 1 through the board's vocoder, VW through the full-rate codec, 4.1), and fills the transmission (DN, or VW where
the channel says so); the core keeps PTT, the time-out and the keying. After PTT release the core unkeys once the terminator is
on air, or `RADIO_YSF_DRAIN_MS` after the release if the board stops pulling; PTT pressed again before then cuts the
transmission (no terminator) and starts from receive. PTT is refused without a codec on a DN channel (`TXDENY_NO_CODEC`; VW needs none),
without a callsign (`TXDENY_NO_CALL`), or where the board has no flat paths or no deviation scale. Debug link keys
`ysf.*` in the `ysf` view (40, [debuglink.md](debuglink.md) 3): the state, the transmission heard, the receiver's counters (syncs, FICH and DCH good and
bad, the whitening order, the sync bit errors, the clock and carrier offsets, as of the radio task's last poll: the
debug link never reads the engine, whose RAM D-STAR may hold by then) and the voice counters.

## 6. Tests

| Test | Checks |
|---|---|
| `ysfTest` | the CRC, whitening, Golay code, FICH, DCH units (both whitening orders) and DN voice against `tests/vectors/ysf.txt`, and a whole seven-frame transmission bit for bit; the Golay decoder over 40,000 words with 0-4 errors; the FICH and DCH decoders with random errors and erasures; noise never passing a FICH; whole transmissions through the receiver: clean, inverted, late entry, a damaged FICH, a lost signal, back to back, hard dibits, V/D mode 1, ten minutes of noise, a fuzz loop; a Yaesu radio's over as recorded (the vectors' `yaesu` line, 1.6: radio ID mode, FT 6 with data at FN 6), also with FN 6's FICH lost; our radio ID mode |
| `ysfModemTest` | the modulator against the root raised cosine; a clean wire; the simulated link with noise, offset, drift, clock error, inversion, an 8 kHz modulator, DMR's levels, high-passed paths; the preamble with one header and with two, each over caught from its header; the clock and offset readings; the engine on a symbol path and on an 8 kHz audio path. `--sweep` prints 2.5's table |
| `ysfVwTest` | Voice FR (4.1): the frame against `tests/vectors/ysf-vw.txt` from `tools/ysf_vw_ref.py`, and a whole VW transmission bit for bit; the FEC over 4,000 frames with up to three errors in each Golay word and one in each Hamming word, the corrections reported; u7 bare; four errors in u0's word seen by the codec's rule; VW transmissions through the receiver: clean, inverted, late entry, each FICH position lost, errors on the air, DN and VW back to back; the codec over the air against the clean decode |
| `ysfVwRadioTest` | VW in the radio core on ysfRadioTest's fake board: another station's VW played sample for sample as the codec makes it, with and without the board's codec; DN, VW and DN on one channel; our VW over heard by an independent receiver (DT 3, CSD3, the encoder's frames in order), PTT allowed without a codec on a VW channel and refused on a DN one |
| `ysfCodeplugTest` | the codeplug records ([ysf-codeplug.md](ysf-codeplug.md)) |
| `ysfRadioTest` | the radio core's module against a fake board: callsigns (a radio ID mode destination shown as `ALL`), the voice in order, the end, the DG-ID filter; our transmission decoded by an independent receiver (radio ID mode to everyone, with a radio ID and without); the first over's silence frame never sent half written; the counters read while the radio task switches to D-STAR; the refusals; DN's mic gain (a quiet mic lifted by the board's 12 dB and the step, a shout held under the limiter's ceiling, unity untouched) |
| `tools/ysf_ref.py --self-test` | the reference against itself and the CRC catalog; `run-ysf` fails when the vectors file differs from what it writes |
| `tools/ysf_vw_ref.py --self-test` | the VW reference: the interleave covers every code bit once, the codes' distances, round trips with errors, CSD3's place; `run-ysfvw` fails when `ysf-vw.txt` differs |

`tools/ysf_ref.py` is written another way than the C, from the same standard: bit lists, polynomial division for the
CRC, the printed Golay matrix with a brute-force decoder, the convolution by coefficient lists, the interleavers as
matrices, the whitening register simulated by name. It also makes signals for the bench (`synth`) and decodes captures
(`decode`).

No permissively licensed recording of a real Fusion transmission was found to test against. Recordings made on the
bench are for local tests only; audio recordings are never committed (AGENTS.md). What the first ones showed (1.6) is
in the vectors as a synthetic over with their layout and made-up content. The radio's own receiver (this code,
compiled for the host) decoded every frame of them: all 619 DN FICHs and 3,060 voice frames, the callsigns, the radio
ID and the terminators; its voice frames matched the reference decoder's bit for bit except 9 bits in 4 frames of a
two-second fade, all symbols near zero in both.
