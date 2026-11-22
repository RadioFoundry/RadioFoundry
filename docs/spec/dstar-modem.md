<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# D-STAR DV: modem and framing

D-STAR digital voice (DV) on a radio whose only way to the air is an FM modulator and an FM discriminator: the GMSK
modem, the DV framing (radio header, voice and data frames, slow data, the end pattern) and the engine that joins
them to a board's flat FM audio taps. The voice codec is outside this layer, behind an interface (4); a software one
is in [dstar-codec.md](dstar-codec.md).

Code: `core/dstar` (portable, integer only, host tested): `dstar_gmsk.c` (modem), `dstar_fec.c` (header coding),
`dstar_frame.c` (framing, transmit and receive), `dstar_engine.c` (both joined for a board), `dstar_voice.h` (the codec
interface), `dstar_codec.c` and the `dstar_ambe_*` and `dstar_mbe_*` files (the software codec,
[dstar-codec.md](dstar-codec.md)), `dstar_codeplug.c` (the channel kind, [dstar-codeplug.md](dstar-codeplug.md)); the
radio core's side in `core/radio/radio_dstar.c` (5.3). Build switch: `CONFIG_DSTAR`, off by default and on where a board
with the glue has room ([build-config.md](../build-config.md), Variants; [feature-matrix.md](../feature-matrix.md)).
Tests: `tests/host/dstarTest.c`, `tests/host/dstarModemTest.c`, `tests/host/radioDstarTest.c`, vectors in
`tests/vectors/dstar.txt` from the independent reference `tools/dstar_ref.py` (6).

Sources: the JARL D-STAR standard, "Digitalization Technology Standard for Amateur Radio" (STD 6.0, English edition,
jarl.org), chapter 4.1, chapter 6 and appendices 1-2, and the appendix figures of its older English translation
(`shogen.pdf`). Written clean room ([CLEANROOM.md](../CLEANROOM.md)): no other D-STAR implementation was read. Where
the standard is silent, the choice made and what settles it are said in place.

## 1. Design

### 1.1 What a board must provide

The modem is audio. A board carries it through two taps of `hal/hal_audio_tap.h`:

| Direction | Capability | What it is |
|---|---|---|
| Transmit | `AUDIO_TAP_FM_TX_FLAT`, `fm_tx_inject(pull, AUDIO_TAP_FLAT)` | samples straight into the FM modulator: no pre-emphasis, voice band-pass, compressor or limiter in the way. 1 is a positive deviation (JARL appendix 1.5) |
| Receive | `AUDIO_TAP_FM_RX_RAW` | the discriminator's output before de-emphasis and the voice filter, squelch open (the demodulator works below squelch) |

8 kHz, 16-bit samples each way are enough (2.6). Higher rates work and help a little (`DSTAR_RATE_MAX` 96 kHz). The
board also gives its path's calibration (`dstar_path_t`, 5.2): the sample value for 1200 Hz, the polarity each way, and
any high-pass it can't switch off.

A path that keeps a first-order high-pass (a coupling capacitor, a voice filter without a bypass) is usable through
the restorer (2.4) at a cost of about 2 dB; without it a 300 Hz high-pass leaves an error floor near 2 % (2.6).

### 1.2 On an HR_C7000 with a separate FM RF chip

The HR_C7000 has three ways audio or bits reach the modulator. Which radios have this pairing is in
[feature-matrix.md](../feature-matrix.md); one radio's wiring is on its hardware page.

| Route | Verdict |
|---|---|
| The built-in 4FSK modem in raw mode | **Not usable.** 4800 symbols/s from a fixed divider of the modem clock, a fixed DMR pulse shape, 30 ms blocks: it sends 4FSK with an RRC pulse, never GMSK. Other projects send P25 through it |
| The prompt FIFO with ring play into the modulator ([modem-hrc7000.md](modem-hrc7000.md) 8.4) | Works on some boards and not others; 32 samples per push every 4 ms with a 200 µs wait (about 5 % CPU). Measured off air by another project's bench work: a rising response (pre-emphasis) until the RF chip's transmit audio bypass bits are set, then about a **290 Hz first-order high-pass** that nothing removed. A 4FSK mode failed through it. Fallback only, with the restorer |
| **The TX RAM as the FM transmit source** (AUDIO_CONTROL b0 = 1) | **Chosen for transmit.** Two halves of 256 samples (32 ms at 8 kHz) at 0x16000030, 16-bit, high byte at the lower address; SYS_INTERP b15 (FM TX) asks for a half, FM_ADDR_SW names which. The datasheet documents it as an FM transmit source. Another project sent a 4FSK mode through it that its own receiver and an SDR decoder decoded; its frequency response and deviation scale are **unmeasured** |

Receive has one raw route: the RX RAM copy of the demodulated audio, 256-sample halves at 8 kHz, SYS_INTERP b14
([modem-hrc7000.md](modem-hrc7000.md) 8.5). APRS receive already uses it (`platform/hrc7000/fm_rx_audio.c`).

Settings for a flat path, each from the datasheets or other projects' code, none measured by RadioFoundry yet:

| Where | Transmit | Receive |
|---|---|---|
| HR_C7000 FM_BANDWIDTH (0x500) | b6 (band-pass) and b4 (pre-emphasis) clear | b6 clear |
| HR_C7000 AUDIO_CONTROL (0x080) | b0 = 1 (TX RAM source); b7 clear (no prompt audio on air) | as for FM receive |
| BK4819-class RF chip, register 0x2B | the three transmit audio filter bypass bits (2:0) set | the receive high-pass, low-pass and de-emphasis bypass bits (10:8) set |
| HR_C7000 FM_DEV_COEF (0x504) | whether the TX coefficient and limiter act on the TX RAM route is unknown | |

Written for this platform, not yet checked on a radio: the TX RAM driver ([modem-hrc7000.md](modem-hrc7000.md) 8.6;
the SYS b15 hook beside the FM receive hook the DMR glue already calls, `platform/hrc7000/fm_flat_tx.c`), and on one
board `fm_tx_inject` with `AUDIO_TAP_FLAT` and the flat receive switch (`hal_audio_tap_t.fm_rx_flat_*`). The interrupt
pulls each 256-sample half straight from the board's `pull` into the modem's RAM in 32-sample chunks (no CPU buffer);
receive reads the RX RAM ring APRS uses, one of the two at a time. Each register choice in the table above is a
run-time bench setting on the board (`hal_audio_tap_t.flat_knob`), the path's calibration a copy the core can replace
(`flat_cal`, `set_flat_cal`); what is still unverified is listed on the radio's hardware page.

### 1.3 Cost

Measured on an HR_C7000 radio's build (csky-elf-gcc 14.2, `-Os`), with a stand-in for the board glue that starts both
sides of the engine (without it the switch adds nothing: gc-sections drops the unused code):

| Item | Cost |
|---|---|
| Flash | 5,992 B (modem 2.0 KB, framing 2.2 KB, header coding 1.2 KB, engine 0.4 KB) |
| RAM, static | 632 B: one `dstar_engine_t`, receive and transmit sharing a union (receive 648 B on the host, of which 330 B is the coded header held four bits a bit) |
| Stack | about 500 B at the deepest: the header decoder (268 B) called from the demodulator through the deframer |
| Sample buffers | none new: receive reads the RX RAM halves the APRS ring already copies (one consumer runs at a time), transmit writes the TX RAM halves in modem memory |
| CPU | estimated from operation counts, not measured: about 1-2 % of 168 MHz receiving (a 64-bit divide per bit for the interpolation and one per zero crossing; the deframer's pattern checks are an XOR and a bit count), under 1 % transmitting, plus an interrupt every 32 ms each way. The header's Viterbi decoder is about 40 k cycles once per transmission |
| Voice codec | not counted. A software AMBE codec needs memory of the order of the DMR vocoder's (13 KB on the fork this was measured on, 7 KB on main since its encoder and decoder share one block) and only one mode runs at a time, so the two can share it |

The default image of that radio uses 98.71 % of its RAM; with the engine 99.01 % (2,088 B left). A voice codec does not
fit without reclaiming RAM: 1.5 lists what could go.

All of D-STAR as built for that radio's multimode variant (the engine, the codec, the channel kind, the radio core's
part, the screens, the debug link and the board's flat paths), measured 2026-11-17 against the same build without it:

| Item | Cost |
|---|---|
| Flash | 47,882 B: the codec 27.3 KB (quantizer and its tables 14.5 KB, analysis 4.0 KB, the shared fixed-point tables 4.8 KB, synthesis 2.7 KB, frame FEC 0.8 KB), the modem, framing and engine 5.8 KB, the radio core's part 5.2 KB, the debug link 2.3 KB, the screens 1.2 KB, the board's flat paths 1.1 KB, the rest in the radio core, Home's drawing and the translations |
| RAM, static | 2,592 B: the radio core's part 1,244 B (the engine 632 B, two voice rings of 16 frames, the view), the copies of the view in the tasks that keep one, the screens 121 B, the board 47 B |
| Codec memory | not static: its 5,304 B of working memory in the DMR vocoder's state block, which it lends while DMR voice is off (`hal_vocoder_t.lend`, 7,168 B on the HR_C7000), and its 3,168 B of state in the radio core's mode area beside the engine (the whole codec, 8.4 KB, doesn't fit the loan). Without the loan D-STAR runs without voice |
| Stack | the encoder 1,288 B and the decoder 336 B on the audio task (1,452 B deepest with the port overhead, of the 6,144 B sized for the stock codec's call: [dstar-codec.md](dstar-codec.md) 6); the receive path's 256 B buffer and the engine on the radio task; the transmit pull on the interrupt stack |
| Settings | 40 B more in every build (the station and the bench's calibration, [settings-store.md](settings-store.md)), so the full image of that radio grew by 280 B of RAM (the copies of `settings_t`) without D-STAR in it |

CPU per 20 ms frame, against the 3.36 M cycles a frame of a 168 MHz processor: estimated, not measured. The codec's
figures are its research measurement on the host (instructions per frame on 12 inputs, four synthetic voices and eight
recordings) times 0.843 cycles per host instruction, a ratio measured for this core elsewhere; the quantizer's 64-bit
multiplies go through a library helper on this core, which the ratio may understate.

| Load | Cycles per frame | Of the processor |
|---|---:|---:|
| Encoder (analysis and quantizer with the perceptual re-ranking), mean / worst | 634 k / 697 k | 18.9 % / 20.7 % |
| Decoder (dequantizer and synthesis), mean / worst | 278 k / 609 k | 8 % / 18 % |
| A concealed frame (fading repeat or comfort noise), mean / worst | 342 k / 603 k | 10 % / 18 % |
| Demodulator and deframer, receiving | 35-70 k | 1-2 % |
| Modulator and framer, transmitting | under 34 k | under 1 % |
| The TX RAM interrupt (a half every 32 ms) / the RX RAM copy | about 2.5 k / 2 k a frame | under 0.1 % each |
| The speaker's PCM through the prompt FIFO, while voice plays (its 200 us wait per push, [modem-hrc7000.md](modem-hrc7000.md) 8.4) | about 170 k | 5 % |
| Transmitting, D-STAR's total, worst | about 710 k | about 21 % |
| Receiving, D-STAR's total, mean / worst | about 500 k / 830 k | about 15 % / 25 % |

DMR's stack and vocoder are idle while D-STAR runs (one mode at a time), so their load doesn't add; the UI, the host
link and the radio task's poll are what the rest of the processor carries, as on an FM channel. Those are unmeasured on
this radio; the first session should read the idle task's share with D-STAR receiving and transmitting.

### 1.4 What only a radio can settle

| Question | First test |
|---|---|
| The TX RAM route: does it reach the modulator on every board, flat to how low, and does FM_DEV_COEF scale it? | A 100 Hz - 3 kHz sweep and a square wave through it, deviation read on the SDR (7) |
| The deviation scale: the sample value for 1200 Hz | The same capture; set `tx_amp` |
| Polarity of each path (the receive copy may follow the chip's IQ inversion, which differs between board revisions) | A received Icom header: the deframer finds the sync in either polarity and reports which; set `tx_invert` by an Icom radio decoding us |
| The receive path's low-frequency response with the bypass bits | GMSK from an Icom radio: the demodulator's level and DC, or the sweep from the SDR's signal generator |
| The CRC's preset and final inversion (3.1) | One clean Icom header: the CRC checks or it doesn't |
| The preamble's phase before the frame sync | The receiver accepts either; our transmitter sends 1010...10 then the sync |
| Turnaround times for this route (key-up to first sample on air, last sample to unkey) | Preamble long enough to cover it; the end pattern fully sent before unkeying |
| The slow data schedule an Icom radio expects (3.5) | Our message and header shown on the Icom radio |

### 1.5 Room for D-STAR and the modes after it

The default image of the HR_C7000 radio leaves 2.7 KB of RAM. The table is what each switch frees when it alone is
turned off, measured by building with it off and comparing the linker's RAM total (static data and bss; RTOS heap
use by tasks a feature starts isn't in it, so a feature with its own task frees more than shown). Sub-features follow
their parent ([build-config.md](../build-config.md)).

Measured 2026-11-16 on the default image (208,736 B RAM, 581,644 B flash), each switch off alone:

| Switch off | RAM freed (B) | Flash freed (B) | For a multimode amateur build |
|---|---:|---:|---|
| `DMR_IP_GATEWAY` (and with it PPP, TCP, the status page and web control, SNMP, TFTP, mDNS, rigctld, USRP, KISS and APRS-IS over TCP) | 17,472 | 73,140 | drop |
| `NET_PPP` (on a radio whose only IP link is PPP, the gateway goes with it) | 17,472 | 73,140 | drop |
| `APRS` (everything APRS) | 11,600 | 34,980 | keep |
| `HOTSPOT` (the MMDVM modem, split duplex with it) | 10,992 | 35,744 | keep |
| `NET_TCP` alone | 9,632 | 31,548 | drop (with the gateway) |
| `APRS_RX` | 7,800 | 12,052 | keep |
| `HOTSPOT_SPLIT` | 6,816 | 18,204 | drop |
| `DUAL_WATCH` | 5,608 | 19,532 | keep; the next candidate if more is needed |
| `ZONE_SHARE` | 3,920 | 12,700 | drop |
| `DMR_ROAMING` | 3,664 | 11,788 | keep |
| `NET_HTTP` alone | 3,504 | 19,512 | drop (with the gateway) |
| `SNMP` alone | 3,384 | 9,224 | drop (with the gateway) |
| `CRASH_LOG` | 2,056 | 5,732 | keep (bring-up) |
| `SFR` | 1,568 | 12,200 | keep |
| `NET_USRP`, `NET_WEBCTL`, `NET_RIGCTL`, `MDNS`, `TFTP` alone | 1,136, 992, 712, 328, 184 | 3,256, 13,912, 4,048, 7,240, 9,072 | drop (with the gateway) |
| `GPS` | 600 | 8,636 | keep |
| `APRS_IGATE` | 536 | 1,584 | keep |
| `DMR_SMS` | 480 | 3,404 | keep |
| `DMR_PRIVACY` | 400 | 2,036 | drop (not for amateur use) |
| `DMR_TMS` | 320 | 1,292 | drop |
| `CPS_FAST`, `STUB_ENTRY`, `DMR_EMERGENCY`, `DMR_LRRP`, `TASK_STACKS` | 208, 176, 168, 112, 64 | 3,816, 1,328, 3,516, 2,328, 1,104 | emergency and LRRP drop; the rest keep |
| `DMR_ARS`, `DMR_CONTROL_STATION`, `DMR_GPS`, `DMR_TALKER_ALIAS`, `DIAG`, `UI_COLOR`, fonts, `LANG_ES` | 0 | 1,456, 276, 920, 976, 2,796, 784, 43,224 (fonts), 40,836 | flash only; ARS and control station drop |

A multimode amateur preset, off: `DMR_IP_GATEWAY` (with `NET_PPP` and every service on it), `HOTSPOT_SPLIT`,
`ZONE_SHARE`, `DMR_LRRP`, `DMR_ARS`, `DMR_EMERGENCY`, `DMR_PRIVACY`, `DMR_TMS`, `DMR_CONTROL_STATION`. Built together:
**31,496 B RAM and 118,356 B flash freed** (RAM 98.7 % to 83.8 %), with the hotspot, APRS (receive and iGate),
roaming, SMS and SFR kept. That is room for the D-STAR engine and a software voice codec's state, and for the next
modes' engines. It would be a variant file (`config/variants/`, [build-config.md](../build-config.md) Variants) next to
`amateur`, which already leaves out privacy, TMS, ARS, LRRP, control station and SNMP; it is proposed here, not
added. Trunking groundwork (`P25_TRUNK`) is not built into any image yet and frees nothing.

The largest single users of RAM, for scale: the frame buffer 38,400 B, the RTOS heap 33,280 B (every task stack and
queue), the DMR vocoder's encoder and decoder states 7,168 + 6,144 B, the APRS state 7,124 B, a 4,096 B buffer, the
TCP connections 3,444 B.

Only one mode runs at a time, so per-mode state can share one region: the DMR vocoder's state block with a D-STAR (or
other) codec's, the DMR data and hotspot rings with the D-STAR engine, the APRS receive ring with the D-STAR
receiver's input. Two of these are done: the codec's working memory is the vocoder's state block (`hal_vocoder_t.lend`,
1.3), and D-STAR's engine and codec state sit in one area the radio core lends to the mode that runs
(`core/radio/radio_mode_ram.c`), which the next modes on the flat paths share.

## 2. The modem

### 2.1 Parameters

| | Value | Source |
|---|---|---|
| Modulation | GMSK, BT = 0.5, modulation index 0.5 | JARL 3, 4.1 |
| Bit rate | 4800 bps | JARL |
| Deviation | +-1200 Hz for a long run of one value (index 0.5 x 4800 / 2) | |
| Polarity | 1 = positive deviation | JARL appendix 1.5 |
| Bit order | each byte least significant bit first; patterns left to right | JARL appendix 1.4, 4.1 |

### 2.2 Modulator

Each bit adds the GMSK frequency pulse, a one-bit rectangle through a Gaussian filter of bandwidth 0.5 x 4800 Hz
(sigma = sqrt(ln 2) / (2 pi BT) = 0.265 bit). The pulse is a 193-point table over three bits (beyond 1.5 bits from its
middle it is under 1e-4 of its peak), interpolated linearly; `tools/dstar_ref.py pulse` makes it and the host test checks
the output against the definition to 0.2 % of full deviation. A long run sums to the full value; an isolated bit peaks
at 0.88 of it. The modulator pulls bits from the framer and writes samples at the path's rate, through the restorer
when the board set one.

### 2.3 Demodulator

| Stage | What | Why |
|---|---|---|
| Restorer | the receive path's high-pass undone (2.4), when the board set one | |
| Pre-filter | a boxcar of half a bit at rates of 16 kHz and up | at 8 kHz the radio's own audio filter has done it |
| DC | searching: the plain average of the input over about 16 bits. Locked: decision-directed, 1/64 of each bit's error | a frequency offset is a DC. A decision-directed loop can't start from a large offset (with every decision a 1 its error averages zero); the preamble and scrambled data are balanced, but the voice frames aren't scrambled, so once locked the decisions take over |
| Bit clock | the zero crossings, located between samples by interpolation; their distance from the expected bit edge corrects the phase (1/4 searching, 1/16 locked) and, while locked only, the rate (1/16384, +-1000 ppm) | an integrator fed noise between transmissions walks to its limit. The phase is never clamped to the bit: clamping dropped the corrections on one side of each edge and the rate settled 500 ppm off |
| Decision | the value at the bit's middle, interpolated between the two samples around it; each bit decided once whatever the corrections do (at 8 kHz one sample interval can hold a middle, an edge and the next middle) | |
| Output | a soft bit (the value) and its unit (the tracked level) to the deframer | the header decoder uses the confidence |

Crossings slower than a quarter of a transition's slope are ignored (noise wandering near zero). The deframer tells
the demodulator when it locks; the loops slow down then.

### 2.4 Low-frequency restorer

H(s) = (s + 2 pi fc) / (s + 2 pi ff): unity above fc, a gain of fc / ff at DC. After a first-order high-pass at fc the
pair is a high-pass at ff. Built as the input plus a leaky integrator, which needs no `tan()` for its coefficients and
is exact enough while fc is far below the sample rate. The floor costs headroom: 60 Hz (the default) under a 290 Hz
high-pass is 4.8 times at DC, which the route has: 1200 Hz is a quarter of the 5.1 kHz the ring play route reached. The lower the
floor, the less wander is left for the demodulator's DC loop.

### 2.5 Polarity

Transmit: `tx_invert` flips the bits before the modulator. Receive: the deframer finds the frame sync or the resync
pattern in either polarity and decodes the rest that way; `rx_invert` only saves it the search.

### 2.6 Measured on a simulated link

`dstarModemTest --sweep`: the transmitter's bits through the modulator at 48 kHz (or at 8 kHz and interpolated, as on a
radio), an FM modulator, white noise at Eb/N0, a 12.5 kHz channel filter, a discriminator, a 3.4 kHz audio filter and
8 kHz samples into the demodulator and deframer. BER is over the 72-bit voice frames of transmissions the deframer
locked to by their header (4 x 500 frames a point); in brackets the headers decoded of 4.

| Path | 8 dB | 10 dB | 11 dB | 12 dB | 13 dB | 14 dB | 16 dB | 18 dB |
|---|---|---|---|---|---|---|---|---|
| Clean | 4.8e-2 (2/4) | 1.3e-2 (4/4) | 5.2e-3 | 1.8e-3 | 5.5e-4 | 1.6e-4 | 6.9e-6 | 0 |
| +700 Hz carrier offset | (0/4) | 1.3e-2 (4/4) | 5.6e-3 | 2.2e-3 | 6.3e-4 | 1.9e-4 | 0 | 0 |
| +500 ppm bit clock | 5.1e-2 (1/4) | 1.3e-2 (3/4) | 5.4e-3 (3/4) | 2.0e-3 | 6.1e-4 | 1.5e-4 | 0 | 0 |
| Modulator at 8 kHz | 6.0e-2 (1/4) | 2.0e-2 (4/4) | 1.0e-2 | 4.4e-3 | 1.8e-3 | 7.6e-4 | 5.6e-5 | 6.9e-6 |
| TX high-pass 290 Hz | (0/4) | 9.1e-2 (2/4) | 5.6e-2 (2/4) | 4.3e-2 (3/4) | 3.5e-2 | 2.9e-2 | 2.0e-2 | 1.4e-2 |
| TX high-pass 290 Hz, restored | 6.2e-2 (1/4) | 2.2e-2 (4/4) | 1.2e-2 | 6.7e-3 | 3.7e-3 | 2.1e-3 | 6.8e-4 | 3.1e-4 |
| RX high-pass 300 Hz | (0/4) | 6.8e-2 (1/4) | 5.3e-2 (3/4) | 4.3e-2 (3/4) | 3.5e-2 (3/4) | 2.8e-2 (3/4) | 1.9e-2 (3/4) | 1.4e-2 (3/4) |
| RX high-pass 300 Hz, restored | 6.3e-2 (1/4) | 2.3e-2 (2/4) | 1.3e-2 | 7.0e-3 | 3.7e-3 | 1.9e-3 | 6.3e-4 | 2.6e-4 |

Unmarked cells decoded 4 of 4 headers. AMBE voice with its own FEC stays clean to about 2e-2. Offset and clock error
cost under 0.3 dB; the 8 kHz modulator (held samples and an interpolation filter, as a radio's DAC does) about 1 dB; a
restored high-pass about 2 dB and a floor near 3e-4 at a 60 Hz restorer floor. The host test asserts margins under
these at 14 dB.

## 3. Framing

### 3.1 Radio header

41 bytes, each sent least significant bit first: flag 1, flag 2, flag 3, RPT2 (the destination repeater, 8
characters), RPT1 (the departure repeater, 8), UR (8), MY (8), MY suffix (4), P_FCS (2). Callsigns are ASCII padded
with spaces. The receiver shows a received field or message with every byte outside printable ASCII as `?`
(`dstar_field_text`): such a byte is a bit error or a radio's own character set, and a control byte would reach the UI
as one of its glyph codes. Flag 1: b7 data, b6 via a repeater, b5 break-in, b4 control, b3 emergency, b2:0 repeater control, auto
reply, resend request, ACK, no reply, unrelayable, null (JARL 4.1.1).

P_FCS is CRC-CCITT, x^16 + x^12 + x^5 + 1, which the standard also gives bit-reversed as 0x8408. It doesn't give the
preset or the final inversion. RadioFoundry uses the ISO 3309 / X.25 FCS: preset 0xFFFF, the reflected polynomial,
complemented, low byte first ("123456789" gives 0x906E). That is the CRC-CCITT the standard's data packets pair with
their ISO 3309 CRC-32. A clean header from an Icom radio settles it (1.4).

### 3.2 Header coding (JARL appendix 2, 1)

| Step | Rule |
|---|---|
| Convolutional code | rate 1/2, K = 3, from zeroed registers: G1 = 1 + D + D^2 then G2 = 1 + D^2 for each bit, the 328 header bits least significant first, then two zero bits: 660 coded bits |
| Interleaver | a 24-row matrix filled column by column (coded bit 24c + r in row r, column c: 28 columns in rows 0-11, 27 in rows 12-23) and sent row by row, as the standard's printed table shows |
| Scrambler | S(x) = x^7 + x^4 + 1 from all ones, as the appendix 1.1 figure draws it: s[n] = s[n-4] XOR s[n-7], XORed into the 660 bits as sent. The sequence starts 0000111 0 1111001 0 (`70 4F 93` packed least significant first) |

The receiver undoes the scrambler and interleaver and runs a soft-decision Viterbi decoder (four states) over 4-bit
soft values; 12 random errors in 660 decode 95 % of the time or more, a 24-bit burst (5 ms) always, a random quarter of
the bits erased still decodes.

### 3.3 The transmission

| Part | Bits | Content |
|---|---|---|
| Preamble | 64 or more | 1010..., ending on 0 (JARL 4.1.1 a); the engine's caller chooses the length to cover key-up |
| Frame sync | 15 | 111011001010000 |
| Header | 660 | 3.2 |
| Frames, every 20 ms | 72 + 24 | the voice frame, then the data frame |
| End | 48 | 32 bits of 1010..., 000100110101111, 0 (JARL 4.1.2 h), in place of the next frame; not scrambled |

Frame 0, the first after the header and every 21st after it (420 ms), carries the resync pattern in its data frame:
1010101010 1101000 1101000, unscrambled. The other data frames carry slow data, each scrambled on its own with the
first 24 bits of the scrambler sequence (appendix 1.3).

### 3.4 Receive

| State | Leaves on |
|---|---|
| Search | 24 bits of preamble (either phase, 2 errors) and the frame sync (1 error), either polarity: Header. The resync pattern (1 error): a late entry, Voice from frame 1 |
| Header | 660 soft bits: decoded; CRC good: Voice from frame 0; bad: Search (the voice still arrives by late entry) |
| Voice | the end pattern (5 errors in 48, checked at every bit): end. At each frame 0 the resync pattern (4 errors in 24); 3 missed in a row (1.26 s): lost |

A late entry hands nothing on until a second resync pattern arrives where it is due: noise matches a 24-bit pattern
about once a minute. Voice then starts at that frame 0 and the header comes from the slow data. Counters for each case
are in `dstar_rx_t`.

### 3.5 Slow data (JARL chapter 6)

Frames 1-2, 3-4, ... 19-20 after the resync frame join into ten six-byte blocks a superframe; the first byte is the
mini header.

| Mini header | Content | Sent | Received |
|---|---|---|---|
| 0x40-0x43 | message block 0-3, five characters each (20 in all) | blocks 0-3 of even superframes when there is a message | delivered once, when the last missing block arrives; a changed block starts the message again |
| 0x51-0x55 | 1-5 bytes of the header, from the first block after the resync frame (6.4) | nine blocks (8 x 5 + 1) of the other superframes, every superframe without a message | collected in order; delivered when 41 bytes with a good CRC are in, once per superframe; a gap spoils that superframe's copy |
| 0x66 | no data (the whole block is 0x66) | the unused blocks | ignored |

The schedule is ours; the standard fixes the blocks' format and where the repeated header starts, not how a radio
interleaves them, and the receiver doesn't depend on any schedule. GPS (D-PRS, 0x3x), code squelch (0xCx) and fast
data (0x8x-0x9x) are not built yet: their blocks are ignored.

## 4. The voice codec

`dstar_voice.h`: 160 samples of 8 kHz speech to one 72-bit frame and back, in air order (byte 0 first, each byte least
significant bit first). The framing never looks inside a frame. Without a codec the transmitter sends the standard's
lost-packet frame (`9E 8D 32 88 26 1A 3F 61 E8`, JARL 6.6) as silence.

A board fills `dstar_vocoder_t` with a codec. `dstar_codec.c` is a software AMBE 3600x2400 codec that does
(`dstar_codec_vocoder`): integer only, 3,168 B of state and 5,304 B of working memory, each shared by both directions, 1.3 KB of stack, about 27 KB of
flash, an estimated 32 MHz (35 at worst) of a C-SKY CK803S to encode and 14 MHz (30 at worst) to decode. Its structure, cost,
quality, limits and provenance are in [dstar-codec.md](dstar-codec.md). It reads the lost-packet frame back as near
silence, and that frame, decoding with no Golay corrections, is what proves the air bit order above
([dstar-codec.md](dstar-codec.md) 5).

## 5. The engine

### 5.1 Use

`dstar_engine_rx_start` with the deframer's callbacks, then `dstar_engine_rx` with each block of received samples.
`dstar_engine_tx_start` with the header, an optional message, the preamble length and a voice frame source, then
`dstar_engine_tx` for each block of samples the board pulls (the shape of `audio_tap_pull_t`); it returns false once
the end pattern has gone, and the radio unkeys. `dstar_engine_tx_stop` ends a transmission with no voice source. One
side at a time: receive and transmit share their state.

### 5.2 A board's calibration (`dstar_path_t`)

| Field | Meaning |
|---|---|
| `rate` | sample rate both ways |
| `tx_amp` | the sample value for +1200 Hz |
| `tx_invert`, `rx_invert` | the paths' polarity |
| `tx_hpf_hz`, `rx_hpf_hz` | a first-order high-pass to restore in each path, 0 when flat |
| `lfr_floor_hz` | the restorers' floor (default 60 Hz) |

### 5.3 In the radio core

`core/radio/radio_dstar.c` runs a D-STAR channel ([dstar-codeplug.md](dstar-codeplug.md)): the radio core tunes it as
narrow FM without sub-audio and hands it over.

| | What |
|---|---|
| Receive | `fm_rx_flat_start` after `rx()`; the radio task reads the board's ring every poll (25 ms) into `dstar_engine_rx`. A header, or a late entry the deframer confirmed, is the squelch (`RS_RX`); the end pattern or a lost carrier closes it. Voice frames go into a ring of 16; the audio task decodes one per 20 ms block once four are queued (the halves come every 32 ms, the poll every 25 ms, so up to 57 ms can pass between deliveries) |
| Transmit | PTT keys as FM (`tx()`), then `fm_tx_inject(pull, AUDIO_TAP_FLAT)`: the board's interrupt pulls samples from `dstar_engine_tx`, whose voice source is a ring of 16 frames the audio task encodes into from the microphone. A frame the encoder hasn't made yet goes out as the silence frame. 128 bits of preamble (twice the standard's minimum) cover the key-up. PTT release lets the ring play out, then the end pattern; the core unkeys `tx_latency_ms` (the board's buffers) after the last sample, or `RADIO_DSTAR_DRAIN_MS` plus that after the release if the board stops pulling. PTT pressed again before the end cuts the over (no end pattern) and starts from receive |
| The header | flag 1 via a repeater from the channel; RPT2, RPT1 from the channel (blank: `DIRECT`, what other radios send on a simplex over); UR: a one-over gateway command, else a choice for now, else the channel's; MY and suffix: set on the radio, else the codeplug's record, else the radio name. PTT is refused without a MY (`TXDENY_NO_CALL`) |
| The calibration | `dstar_path_t` from the board's `flat_cal`, with the settings' `flat_*` over it (the bench, [debuglink.md](debuglink.md) 3.31) |
| The codec's RAM | its working memory from `hal_vocoder_t.lend` at the start (nothing is given back: the vocoder rebuilds its own state at its next call, and the radio core starts DMR only after D-STAR stopped), its state in the mode area |
| Beeps | mixed into the D-STAR stream (`beep_mix`), as into DMR's |
| Left out | Scans and dual watch skip D-STAR channels (a dwell is too short to start the engine and judge); APRS takes the receiver only off a D-STAR channel (one consumer of the ring), and a beacon stops D-STAR for its job; the hotspot stops it |

## 6. Tests

| Test | Checks |
|---|---|
| `dstarTest` | the CRC, the scrambler sequence, header coding and the slow data blocks against `tests/vectors/dstar.txt`; the Viterbi decoder against random errors, bursts, erasures and noise; whole transmissions through the deframer: clean, inverted, late entry, a ruined header, errors, a lost carrier, a minute of noise, a fuzz loop |
| `dstarModemTest` | the modulator against the Gaussian pulse; the restorer against a high-pass; the simulated link with noise, offset, clock error, inversion, an 8 kHz modulator and high-passed paths; the engine end to end at 8 kHz |
| `tools/dstar_ref.py --self-test` | the reference against itself and the CRC catalogue; `run-dstar` fails when the vectors file differs from what it writes |
| `radioDstarTest` | the radio core's part with the real engine, framing and codec on a fake board: whole overs out through the flat transmit interrupt and back through the receive ring (header, message, 150 frames decoded and played at the microphone's level), UR's lifetimes, no codec RAM, the bench's calibration (an inverted transmit path found by the receiver, PTT refused without a scale or a callsign), a test tone at the 1 kHz scale, the debug link's counter reset preempted by an audio block |
| `tools/dstar_bench.py --self-test` | the bench script's analysis on made-up captures: a tone's deviation, a high-pass from a square wave's droop, our frame sync and its polarity |

`tools/dstar_ref.py` is written another way than the C, from the same standard: bit lists, an MSB-first CRC register
over the bits as sent, the printed interleaver matrix, the scrambler figure's seven registers simulated one by one.

No permissively licensed real D-STAR recording was found to test against: the signal identification wiki has an audio
sample but no I/Q file and states no licence for it. Recordings made on the bench are for local tests only; audio
recordings are never committed (AGENTS.md).

## 7. First tests on air

The plan for the first session, scripted where it can be, is [testing/dstar-bench.md](../testing/dstar-bench.md). In
short: into a dummy load, the SDR beside it, lowest power, a simplex amateur frequency:

1. Receive first, no transmitter of ours involved: an Icom radio sends a short DV over; the debug log shows the header
   (CRC good settles 3.1), the polarity found, the frames and the end. Then the same with a message set, then a late
   entry (start listening mid-over).
2. The transmit route: a sweep and a square wave through the TX RAM on the SDR: flatness, the high-pass if any
   (`tx_hpf_hz`), the deviation scale (`tx_amp`).
3. Transmit to the SDR: our GMSK's eye and deviation (+-1.2 kHz), then to the Icom radio: the header and message shown
   on its screen, `tx_invert` if it decodes nothing.
4. Through a repeater or hotspot (RPT1 / RPT2 set) once both directions decode.
