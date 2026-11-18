<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# NXDN

NXDN conventional voice (the NXDN Technical Specifications: TS 1-A Common Air Interface, TS 1-B Basic Operation), on
the air interface ([air-interface.md](air-interface.md)). Switch: `CONFIG_NXDN`, which also builds the air interface
([build-config.md](../build-config.md)). Which radios could carry it: [feature-matrix.md](../feature-matrix.md).
Clause numbers below are TS 1-A's unless they say TS 1-B.

State:

| Part | State |
|---|---|
| Frame codec: FSW, LICH, scrambler, SACCH, FACCH1, the four VCH (1, 2) | written, host tested |
| Receive, the call layer, the engine (3, 4, 7) | written, host tested; 4800 bit/s, and 9600 bit/s with half-rate voice |
| Voice (5) | through the DMR side's voice path to the vocoder the board hosts (`hal_vocoder_t`); host tested |
| The radio core: mode, settings, PTT, debug link, home screen (6) | written, host tested |
| Codeplug channels, the site's Codeplug Editor ([nxdn-codeplug.md](nxdn-codeplug.md)) | written, host tested |
| Scan (6.4) | written, host tested; not checked on a radio |
| Dual watch | written, host tested: a stored NXDN channel on either side, the receiver alternating onto it ([radio-features.md](radio-features.md) 2.1); not checked on a radio |
| Channel editor on the radio | written, host tested: RAN, rate, group, unit ID ([ui.md](ui.md) 2.1) |
| On air | transmit checked off air on a radio at both rates (`test_nxdn_tx_off_air`); receive checked at both rates with a bench transmitter, speech frames through the vocoder included (`test_nxdn_rx_from_bench`, `test_nxdn_rx_speech`); the sound at the speaker is not checked (8) |
| 9600 bit/s full-rate voice (EFR), data, trunking, encryption | not written (10). Encrypted calls are recognized and not played |

## 1. On air

| Item | 4800 bit/s | 9600 bit/s | Clause |
|---|---|---|---|
| Channel | 6.25 kHz | 12.5 kHz | table 2.3-1 |
| Symbol rate | 2400 symbols/s | 4800 symbols/s | table 2.3-1 |
| Deviation of the outer symbols (+3, -3) | 1050 Hz | 2400 Hz | table 3.3-1 |
| Transmit filter | root raised cosine, roll-off 0.2, times the shaper sin(πfT)/(πfT) | same | 3.4 |
| Receive filter | root raised cosine times the inverse shaper | same | 3.5 |

The dibit map is the air interface's (`01` +3, `00` +1, `10` -1, `11` -3; 3.3).

A frame is 192 symbols (80 ms at 4800 bit/s, 40 ms at 9600):

| Symbols | Field | Bits |
|---|---|---|
| 0-9 | Frame sync word (FSW), `CDF59` | 20 |
| 10-17 | LICH | 16 |
| 18-47 | SACCH | 60 |
| 48-191 | four VCH of 72 bits, or a FACCH1 in VCH 1-2 and/or 3-4 | 288 |

A transmission opens with a preamble: the reference deviation pattern (`5F` three times, 3.6), then `5775FD` (4.4.3),
24 symbols. Every frame but the FSW is scrambled (4.6: X⁹ + X⁴ + 1, starting at `0 1 1 1 0 0 1 0 0`, one bit per
dibit's high bit).

The LICH (4.5.1, figure 4.5-7): seven bits (RF channel type 2, functional channel 2, steal option 2, direction 1) and
even parity over the first four, each bit sent as an outer symbol (0 as `01`, 1 as `11`). A conventional voice
channel's frames are RDCH (`10`).

| Functional channel (USC) | Meaning |
|---|---|
| `00` | SACCH non-superframe: a call's first and last frames |
| `10` | SACCH superframe: a call's voice frames; four make a superframe |
| `11` | SACCH superframe whose FACCH1 frame is idle (9600 bit/s, TS 1-B 4.1.2.1) |
| `01` | UDCH (data): refused |

| Steal option | VCH 1-2 | VCH 3-4 |
|---|---|---|
| `00` | FACCH1 | FACCH1 |
| `01` | FACCH1 | voice |
| `10` | voice | FACCH1 |
| `11` | voice | voice |

## 2. Fields and FEC (`core/nxdn/nxdn_frame.c`, `core/fec/conv.c`)

| Field | Content | Code | Clause |
|---|---|---|---|
| SACCH | SR (structure 2 bits, RAN 6), 18 bits of message | CRC6 (x⁶+x⁵+x²+x+1), 4 tail bits, rate 1/2 K=5, punctured to 60 bits (`111111 / 110110`), interleaved in rows of 12 | 4.5.3 |
| FACCH1 | one 80-bit layer 3 message | CRC12 (x¹²+x¹¹+x³+x²+x+1), 4 tail bits, rate 1/2 K=5, punctured to 144 bits (`11 / 01`), interleaved in rows of 16 | 4.5.4 |
| VCH | 72 bits of voice with the voice frame's own FEC | none on the frame level (5) | 4.5.2, 7.2 |

The convolutional code is G1 = 1 + D³ + D⁴, G2 = 1 + D + D² + D⁴ (4.5.3). The CRCs start with all ones and are sent
highest register bit first (figures 4.5-10, 4.5-11).

The Viterbi decoder is shared (`core/fec/conv.c`): any rate 1/2 code of constraint length 5, soft or hard, punctured
bits as erasures. Its memory is the caller's: one `uint16_t` of decisions per input bit (192 bytes for a FACCH1),
plus 64 bytes of path metrics and 32 of branch outputs on the stack (160 bytes of stack on Cortex-M4 at `-Os`). About
160 operations per input bit:

| Decode | Input bits | Operations |
|---|---|---|
| SACCH | 36 | about 5,800 |
| FACCH1 | 96 | about 15,400 |
| A voice frame (SACCH) | | about 5,800 per frame: 72,000/s at 4800 bit/s |
| A header or FACCH1 frame (SACCH, two FACCH1) | | about 36,500 |

Soft values come from the frame's soft symbols: a dibit's high bit by the sign, its low bit by the distance from the
inner level. The free distance after puncturing is 5 (SACCH) and 4 (FACCH1).

## 3. Receive (`core/nxdn/nxdn_rx.c`)

The framer looks for two patterns:

| Pattern | Symbols | Errors | Where |
|---|---|---|---|
| FSW | 10 | 2 bits | every frame (5.3.1.2, N5) |
| Preamble tail and FSW | 19 | 3 bits | a transmission's start only (N6 = 1) |

A 10-symbol pattern turns up in noise and in payload, so the head hook decides whether a frame is there:

| Case | Result |
|---|---|
| LICH parity holds, RDCH, not UDCH, at most one LICH symbol inner | the frame is taken; the SACCH is decoded |
| SACCH CRC holds and its RAN is not ours (unless ours is 0) | refused, counted (`ran_rejects`): another system on the channel |
| LICH lost inside a call's superframes | up to two frames in a row are placed by the call's order |
| Five frames without a sync (N7) | the signal is lost |

The profile: 4-level, root raised cosine 0.2, the deviations of 1, `bw` 12.5 kHz (the narrowest a radio sets; 6.25
kHz is open, 9), and `sync_any_syms` = 192: a sync opens every frame, though none is 24 symbols long
([air-interface.md](air-interface.md) 2).

## 4. The call layer (`core/nxdn/nxdn_call.c`)

Messages (TS 1-B 6.4): VCALL `01` and TX_REL `08` carry the same octets (type, control option, call type and voice
option, source ID, destination ID, cipher and key ID in VCALL only). VCALL_IV `03` carries an encrypted call's
initialization vector. IDs are 16 bits; `FFFF` is all.

Receive:

| Frame | Effect |
|---|---|
| Header (non-superframe, two FACCH1 VCALL) | a call starts with its fields |
| A superframe frame without a call | a call starts as a late entry; its fields come once the four SACCH parts (structures 1/4 to 4/4) are in, in order (TS 1-B 4.1.6) |
| VCALL_IV in either FACCH1 | the call is encrypted |
| Header VCALL with other fields during a call | the old call ended unseen: a new one starts |
| TX_REL, or the signal lost | the call ends |

Voice is handed on only once the VCALL is in, the cipher is none, no VCALL_IV was seen and the voice option is half
rate (4800 or 9600 EHR). Everything else is counted (`voice_held`), not played.

Transmit (TS 1-B 4.1.3): preamble, a header frame (SACCH IDLE, VCALL in both FACCH1), superframe frames (SACCH VCALL
part 1/4 to 4/4, four VCH), and a release frame (SACCH IDLE, TX_REL in both FACCH1). Frames go out symbol by symbol:
a voice frame's FSW, LICH, SACCH and first VCH leave with its first voice frame, each later voice frame releases the
next 36 symbols. At 9600 bit/s every second frame is a FACCH1 frame (TS 1-B 4.1.2.1). When the key is let go, the
frame in hand is filled with silence; a frame with nothing on air yet is dropped and the release frame follows the
one before. The direction bit is inbound (table 5.2-2).

## 5. Voice

A VCH is one 20 ms voice frame of the half-rate vocoder (3600 bit/s with FEC, 2450 without; 7.2), 72 bits. NXDN
carries it scrambled with the frame (2) and otherwise as the vocoder gives it: its FEC is the vocoder's. The
standard's two FEC test frames (7.2.1) give the same 72 bits through the DMR side's frame FEC (`nxdnTest`), so the
frame is the one the board's DMR vocoder takes.

The voice path is the DMR side's (`core/dmr/ambe/ambeVoice.c`, [dmr-voice.md](dmr-voice.md)): the engine's `rx_voice`
hook hands each 72-bit frame to its sink, which takes the FEC off (`ambeFec`, 72 to 49 bits) and queues the 49
parameter bits for `hal_vocoder_t`; `tx_voice` takes the next frame from its source, which encodes the microphone's
49 bits and adds the FEC. The audio stream runs while the engine does. Nothing in the tree encodes or decodes NXDN
speech: the engine, the call layer, the tests and `nxdntool` move made-up patterns.

| Item | State |
|---|---|
| Bit order | settled: the standard's FEC test frames (7.2.1) are what `ambeFecEncode` gives for their parameter bits (`nxdnTest`), so the VCH is the DMR frame bit for bit |
| Silence | the DMR side's silence frame (`dmr_ambe_silence`), which is 7.2.1's second test frame |
| 9600 bit/s full rate (EFR, 144-bit VCH, 7200 bit/s) | out of scope: such calls are held, not played |

## 6. The radio core (`core/radio/radio_nxdn.c`)

`RADIO_MODE_NXDN` runs the engine on the board's air interface with the board in its digital mode and the DMR stack
stopped, as P25 does ([p25.md](p25.md) 7). The engine's memory is its own, or with P25 built and no lending vocoder
the memory P25 keeps for itself (one engine runs at a time).

| State | Engine | PTT |
|---|---|---|
| NXDN, the provider's fit has TX at the rate | running | keys: preamble, header, voice, release frame |
| NXDN, the fit has TX only | running, nothing is received | keys |
| NXDN, the fit has RX only | running, receive only | refused, `TXDENY_AIR_NO_TX` ("This mode: receive only") |
| NXDN, the fit has neither | not started (`nxdn.begin_fails`) | refused, `TXDENY_REFUSED` |
| No codec on a board that needs one | running | refused, `TXDENY_NO_CODEC` |
| Leaving NXDN, a retune | stopped, started again on the new frequency | |

Transmit timing, the time-out and the release follow P25's (one table of operations per air mode, `radio_air.h`).
Settings sent while transmitting take effect when the transmission ends; a rate change starts the engine over.

### 6.1 Selecting it

| Where | How |
|---|---|
| VFO | the mode tile: FM, DMR, P25, NXDN in turn, each where the board runs it (`radio_nxdn_fits`); saved with the VFO |
| A channel | a channel of kind 4 ([nxdn-codeplug.md](nxdn-codeplug.md)), or a temporary mode change (debug link op 31, mode 3), which Save to channel stores as one |
| Debug link | `rfoundry_debug.py radio mode nxdn` ([debuglink.md](debuglink.md) 3.8) |

Refused in the listen-only bands, and where the board's air interface carries neither rate.

### 6.2 Settings

RAM only, one set for the VFO and for channels in NXDN for now (`radio_req_nxdn_settings`, debug link op 43,
`rfoundry_debug.py radio nxdn`). A stored NXDN channel runs on its own: the rate, both RANs, the squelch and the talk
target from its record, the unit ID from it or the radio-wide record ([nxdn-codeplug.md](nxdn-codeplug.md)); these
settings don't reach it. The debug link view shows what is in force (`nxdn.run.*`).

| Setting | Values | Default | Notes |
|---|---|---|---|
| Rate | 4800 bit/s, 9600 bit/s | 9600 | the engine's `rate9600`; a rate change is a new session |
| RAN, receive | 0 to 63 | 0 | 0 takes every RAN (TS 1-B 4.1.7.1) |
| RAN, transmit | 0 to 63 | 1 | |
| Unit ID | 1 to 65519 (9) | 1 | the source of what the radio sends |
| Call | group or unit, ID | group 1 | `tx_call_type`, `tx_dst` |
| Group squelch | 0 (RAN only) or a group ID | 0 | `rx_gid`: group calls to it or to all, unit calls to the unit (TS 1-B 4.1.7) |
| Emergency | on, off | off | the control option's bit |
| Bandwidth | 12.5 kHz | | radios set 12.5 kHz at both rates (9) |
| Talk-around | | | not a setting: the frames are inbound either way (table 5.2-2) |

### 6.3 What it shows

| Where | What |
|---|---|
| Home screen | the mode name, the group or unit PTT calls on the talk line, the power, transmit RAN and rate in the chips; a call heard: who calls whom, or why it isn't played (encrypted, another group) |
| Debug link | the `nxdn` view section ([debuglink.md](debuglink.md) 3.7): the call, the settings, the provider's fit at each rate, counters |

### 6.4 Scan

A zone scan (or a VFO scan in NXDN) stops on a stored NXDN channel as on any other: the tune starts the engine at the
channel's rate. As P25's ([p25.md](p25.md) 7.1): nothing heard moves on after the dwell; a carrier (the noise reading or
the signal strength) probes up to 700 ms for a call of the channel's RAN; such a call probes up to 1.5 s while its
squelch waits for the VCALL (late entry: from the SACCH superframe); a call that plays holds; one that doesn't (another
group, encrypted) moves on. A channel at a rate the board's provider can't receive (`radio_nxdn_receives`: a transmit
only fit) is never a stop.

## 7. The engine (`core/nxdn/nxdn_engine.c`)

`nxdn_engine` is NXDN on a channel: the call layer on an air interface session, as `p25_engine` is P25's
([p25.md](p25.md) 6). The settings are those of 6 plus `silence`, the hooks `rx_start`, `rx_update`, `rx_voice`,
`rx_end` and `tx_voice`. A call that is not played is still tracked: the view says a call is on the channel, from
whom, to whom, encrypted or not. `nxdn_engine_set` changes the RAN and squelch while running; the call in hand is
judged again at its next voice frame.

Transmit follows p25.md 6: prepare (preamble and header queued), the radio core keys, go, voice pulled as the queue
drains (a frame not ready is waited for while the queue holds more than the provider's lead plus a chunk, then
silence goes), stop (the frame on air finished, the release frame), `AIR_TX_DONE`, clear.

Memory on Cortex-M4 (`-Os`):

| Item | Bytes |
|---|---|
| `nxdn_engine_t` (session, call layer both ways, frame buffers) | 1104 |
| of it `nxdn_call_rx_t` / `nxdn_rx_t` / the decoders' memory | 488 / 416 / 384 |
| `nxdn_call_tx_t` | 82 |
| Code: frame codec, receive, call layer, engine, Viterbi | 1658 + 615 + 1659 + 1054 + 712 |
| Deepest receive stack below the framer | about 310 (call layer 80, field decoder 72, Viterbi 160) |

The radio core needs from a board: a provider (`board_t.air`) whose fit takes 2400 or 4800 symbols/s with a 10- and a
19-symbol sync, and its vocoder (`hal_vocoder_t`) for the 72-bit frames (6).

## 8. Tests

| Test | Covers |
|---|---|
| `tests/host/fecTest.c` (`test_conv`) | the Viterbi decoder: clean, punctured, soft against hard in noise |
| `tests/host/nxdnTest.c` | the scrambler against its register, the syncs, the LICH and its parity, SACCH and FACCH1 round trips through errors and CRC checks, VCH places, the 7.2.1 FEC frames, messages; calls through the framer: clear, RAN squelch, RAN 0, late entry, LICH coasting, symbol errors, a header sync with an error, encrypted, 9600 bit/s |
| `tests/host/nxdnEngineTest.c` | two engines on made-up providers at both rates: a call heard with its fields and voice in order, an encoder stall covered by silence, no underruns, late entry, group and individual squelch, a squelch change mid-call, an abort, another RAN, RAN 0, an encrypted call; a call through a modeled discriminator tap and the soft modem at 2400 symbols/s, eight calls per noise level |
| `tests/host/radioCoreTest.c` (`radioCoreTestNxdn`, `radioCoreTestP25Nxdn`) | the radio core against an NXDN engine on a fake provider: a call heard, its voice in order to the vocoder, another RAN, group squelch; PTT heard by the far engine with the settings, settings sent mid-transmission, 4800 bit/s, a TX-only and an RX-only fit, no fit, the time-out; the mode by request, the debug link and the VFO saved; P25 and NXDN in turn in the same memory; a zone scan over a stored NXDN channel: a stop with nothing on air, held on a late entry to its group, not on another group or RAN, never a stop at a rate the provider can't receive (`test_nxdn_scan`); dual watch with it as B: a hold on its group's call, not on another group, refused at such a rate, and with P25 on A one engine at a time (`test_nxdn_dual`) |
| `tests/host/uiFlowTest.c` | the mode tile's order, the home screen in NXDN, the FM-only tiles |
| `tests/hil/test_nxdn.py` | on the radio: the mode switch, the settings, a stored channel written over CPS; opt-in, an over decoded off air by the SDR bench's 4FSK receiver and `nxdntool` (one HR_C6000 radio: 9600 bit/s 78 frames, outer deviation 2399 Hz; 4800 bit/s 40 frames, 1012 Hz; no CRC failure), a stored channel that transmits keyed and decoded the same way with the channel's RAN, unit and group (`test_nxdn_stored_channel_tx`), a call the bench plays received (`test_nxdn_rx_from_bench`), and one whose voice frames are a speech recording's, decoded by the vocoder: every frame into the voice path, none silent, bad or lost, the decoded PCM peaking as speech does (`test_nxdn_rx_speech`, with `--ambe FILE`), and a zone scan or dual watch that holds on a stored NXDN channel when the bench calls there (`test_nxdn_scan_from_bench`, with `--allow-codeplug-write`; not yet run on a radio). They skip where the fit has no RX. Nothing measures the sound at the speaker |
| `tools/bench/nxdntool.py selftest` | the bench tool (below) both ways at both rates, with bit errors and through the soft modem, from bench receiver records, the summary line |

The soft modem test (8 kHz tap, 2000 counts per kHz, so the outer level is 2100 counts and the step between levels
1400): every call whole up to 300 counts of noise; at 400 and 500 one call in eight misses its header and is joined
late; at 600 three in eight are whole; every call is still found up to 700. The voice bits there are raw channel
bits: their own FEC is the vocoder's.

`tools/bench/nxdntool.py` (built from `tools/bench/nxdn/`) runs the firmware's code on the host: `gen` writes a call as
symbols with an expect file, `mod` turns symbols into discriminator samples, `decode` and `disc` read them back
through the framer (and the soft modem) and compare. `decode` reads what `p25tool.py decode` reads: a dibit file, the
bench receiver's records (`--records`) or `rfoundry_debug.py air rx` records (`--air-jsonl`), and ends with a summary:
RAN, source, destination, call type, frames, voice frames, CRC failures. Voice is made-up patterns, or with
`gen --voice-file FILE` the 72-bit frames of a recording as they came off the air (9 bytes each): the tool never looks
inside a voice frame.

An over checked off air: the radio in NXDN on a test frequency, the SDR bench's 4FSK receiver on it (2400 baud at
4800 bit/s, 4800 baud at 9600 bit/s, root raised cosine, the FSW `CDF59` as a 10-symbol sync), its symbols through
`nxdntool.py decode --records` (`test_nxdn_tx_off_air`).

## 9. Unconfirmed

Facts the documents leave open, and what the code assumes. The documents have no frame-level test vectors (no CRC or
scrambler examples), so the codec checks against itself and the clauses only.

| Item | Assumed | Where |
|---|---|---|
| Scrambler: is the first output bit the register's state before or after the first shift | before: the sequence starts `0 1 1 1 0 0 1 0 0` | 4.6 |
| CRC register form (the bit order of the remainder on air) | highest register bit first, as figure 4.5-10 draws it | 4.5.3, 4.5.4 |
| Encoder start state | zero | 4.5.3 |
| What a 9600 bit/s call's FACCH1 frames carry | the VCALL again | TS 1-B 4.1.2.1 |
| The transmit shaper | the soft modem and `nxdntool mod` use root raised cosine alone on receive; the sinc shaper is in the model only | 3.4, 3.5 |
| A 6.25 kHz channel | radios set 12.5 kHz; whether a board's filter can do 6.25 is per board | table 2.3-1 |
| A short sync | one wrong symbol in a header's FSW makes the framer miss the header (10 symbols, the fit's limit); the call is joined late at the next superframe | 5.3.1.2 |
| Unit ID range | 1 to 65519 (the top IDs are special) | TS 1-B |
| A board's transmit pulse | the provider's shaping at the profile's symbol rate and deviation, as its fit reports (`AIR_FIT_SHAPE_APPROX`, `AIR_FIT_DEV_APPROX`); whether a receiver of another make takes it is untested | 3.4, 3.5 |
| Voice on air | the VCH bit order is settled against 7.2.1 (5); a call heard by a radio of another make is untested | 7.2 |

## 10. Not written

Changing an NXDN channel's squelch on the radio, 9600 bit/s full rate, data (UDCH, short data), trunking (RCCH), status and other supplementary messages,
encryption, a transmit filter in a provider.
