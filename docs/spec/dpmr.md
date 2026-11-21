<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# dPMR

dPMR is ETSI's digital FDMA mode for 6.25 kHz channels: ETSI TS 102 490, the licence-exempt dPMR446 air interface
("Mode 1"). TS 102 658's licensed modes 2 and 3 are not built. Its physical layer is NXDN 4800's, and its voice frame
is the 72-bit AMBE+2 frame DMR carries, so it runs as a profile on the shared air interface
([air-interface.md](air-interface.md)) with the voice through the radio's DMR vocoder path, as NXDN does
([nxdn.md](nxdn.md) 5). Built with `CONFIG_DPMR`. Which radios have an air interface provider:
[feature-matrix.md](../feature-matrix.md).

Section numbers below are TS 102 490 V1.7.1's.

State: host tested, through the framer, the session and the soft modem, and against a real dPMR446 radio's
transmission (10). Not yet run on a radio.

## 1. On air

| Item | Value |
|---|---|
| Modulation | 4FSK, 2400 symbols/s, +-1050 and +-350 Hz, root raised cosine 0.2 (4.1, 12.2): the profile is NXDN 4800's but for its patterns |
| Symbols | dibit `01` +3, `00` +1, `10` -1, `11` -3: the air interface's map |
| Channel | 6.25 kHz; the radios tune 12.5 kHz, their narrowest |

| Frame | Symbols | Content (4.2.3, 6.1) |
|---|---|---|
| Header | 192 | preamble (36 or more, +3 +3 -3 -3) - FS1 `57FF5F75D577` (24) - HI0 (60) - channel code (12) - HI1 (60) |
| Payload | 192 | FS2 `5FF77D` (frames 1 and 3) or the channel code (frames 2 and 4) (12) - CCH (36) - four TCH (36 each) |
| End | 48 | FS3 `7DDFF5` (12) - END (36) |

A voice call is one or more headers (extended wake-up headers count down in their call information, 5.10.1), whole
superframes of four payload frames (320 ms), and an end frame. FS4 (`FD55F5DF7FDD`, a packet data header) is FS1 with
every symbol's sign turned; packet data is not built.

## 2. Fields and FEC (`core/dpmr/dpmr_frame.c`, `dpmr_fec.c`)

| Field | Information | Coding (7) |
|---|---|---|
| HI | header type 4, called ID 24, own ID 24, mode 3, version 2, format 2, reserved 2, call information 11 | + CRC8 (x^8+x^2+x+1), Hamming (12,8) per byte, 12 x 10 interleave, scrambled |
| CCH | frame number 2, a quarter of the IDs 12 (called upper, called lower, own upper, own lower by frame), mode 3, version 2, format 2, reserved 2, slow data 18 | + CRC7 (x^7+x^3+1), Hamming per byte, 12 x 6 interleave, scrambled |
| END | end type 2, ARQ 2, Tx wait 4, status 5, reserved 4 | + CRC7, Hamming per byte, the 36 bits twice, the 72 scrambled as one block |
| Channel code | 12 bits | one per symbol: 0 as +3, 1 as -3 |
| TCH | a 72-bit AMBE+2 frame with its FEC | none here: the frame as the vocoder path makes it, not scrambled |

The scrambler is x^9 + x^5 + 1, preset to all ones and restarted for each block. Where clause 7 leaves a choice open,
a real dPMR446 transmission settled it, and `tools/dpmr_ref.py --self-test` holds the reference to its blocks: both
CRCs start from zero; the scrambler's output is its last stage, so its first nine bits are ones; Hamming sends the
eight data bits, then C3..C0; the end frame's two copies are scrambled as one block (7.8; 9.6 and figure 11 scramble
each on its own, and the radio does not).

Identities (8.1.1): an initial-services radio (every licence-free dPMR446 handheld) addresses with a common ID, 1-254,
sent as the top 8 bits of the 24 with sixteen ones after it; 255 calls everyone. A common ID addresses as both the
called and the own ID. Channel codes (6.1.5): table 6.1 ties one to each RF channel, group A (initial services) and
group B (configured services), 16 each, for 446.103125 + 6.25 kHz n. A channel outside 446.1-446.2 MHz has no code
of its own: both ends only need to agree, and the default is group A's first (`dpmr_cc_for_hz`).

## 3. Receive (`core/dpmr/dpmr_rx.c`)

The framer ([air-interface.md](air-interface.md) 5) cuts units, each opened by one of the profile's three patterns,
so a lock is always on a unit's first symbol:

| Unit | Opened by | Symbols |
|---|---|---|
| Header | FS1 | 156 (FS1 to HI1) |
| Pair of payload frames | FS2 of frame 1 or 3 | 384, handed over in two pieces of one frame each |
| End | FS3 | 48 |
| Preamble of a header behind a header | (no pattern) | 36 |

| Rule | Why |
|---|---|
| Pairs, not frames | frames 2 and 4 open with the channel code, which belongs to the channel and can't be a pattern of a profile every channel shares. Cutting pairs puts FS2 at the head of every payload unit; the second frame is placed by counting, as the sender placed it |
| FS2 and FS3 take one wrong symbol of twelve | with two, FS2 is found one symbol into FS1 and FS3 across a preamble's end, and a lock there costs the header; two of table 6.1's codes (`1D4`, `392`) are two symbols from FS2. A pair whose FS2 is lost is still placed by counting and read by its CCH |
| The head hook reads a pair's CCH | it takes a pair only when the CCH behind its FS2 decodes and names frame 1 or 3: a channel code read as FS2 is followed by the CCH of frame 2 or 4, and the search goes on to the real FS2 a frame later |
| A preamble behind a header is a unit | so the next header opens with its FS1 where the framer looks for one, and a run of wake-up headers is one lock. The header can't take it in: a unit's length is fixed by its head hook, and can't end before the symbols the hook has seen |
| Coasting | a pair with neither its FS2 nor a readable CCH is taken once inside a call; `lost_frames` 2 ends the call after two pairs without a sync (up to 320 ms) |
| An end frame | only inside a call |

FS1 is the profile's one 24-symbol pattern, all outer: a chip correlator takes it. It opens headers only, so nothing
recurs at 24 symbols inside a call (`sync_every_syms` 0); `sync_any_syms` is a pair, 384.

The framer gets no soft buffer from the mode: dPMR's block codes take hard bits, sliced with the levels the framer
measures on each sync.

## 4. The call layer (`core/dpmr/dpmr_call.c`)

| Event | What happens |
|---|---|
| Header, CRC good | kept as pending: a call starts at its first payload frame (a header alone carries no voice, and a run of wake-up headers is one call). A header inside a call from other IDs ends that call |
| Payload frame | starts a call if none (with the pending header's IDs when it is under 200 ms old, else a late entry); its CCH adds a quarter of an ID; the second frame's sync field gives the channel code; its four TCH go to the sink (voice modes only) |
| End frame | the call ends (`ended`), whether or not its END block decodes |
| Lost | the call ends (not `ended`) |

Transmit: one or more headers (all but the last extended wake-up headers counting down), then payload frames released
as their voice frames come in, each frame's sync and CCH held back until its first TCH is in. On the end, the
superframe in hand is finished with silence and the end frame follows (a call is whole superframes, at least one). A
voice call is sent as header type 0, mode 0 (voice), format 1 (peer to peer), as a dPMR446 radio sends it even to a
common ID's whole group.

## 5. The engine (`core/dpmr/dpmr_engine.c`)

NXDN's engine's shape ([nxdn.md](nxdn.md) 7) on the same session: the call layer on one `air_session`, voice frames
to and from the caller's hooks, the transmitter fed from the provider's task. What it adds:

| Item | Rule |
|---|---|
| Filter | `rx_mine` plays only calls to the channel's called ID, to all, and to our own ID; a call whose called ID is not in yet (a late entry) plays until it is. Off: every call plays |
| End of an over | voice the encoder made before PTT was let go still goes out; then silence to the superframe's end and the end frame |
| Keying | none of its own: the radio core's band, inhibit and power checks come first ([air-interface.md](air-interface.md) 7) |

## 6. The codeplug (`core/dpmr/dpmr_codeplug.c`)

A dPMR channel is a channel of kind 7 on the codeplug's channel-mode layer ([channel-modes.md](channel-modes.md)): the
base record is the safe stand-in (DMR, receive only) and the tag carries the block below and whether the channel may
transmit. No system type, no contacts. Multi-byte fields are little endian, as the layer's others.

### 6.1 The channel block (5 bytes)

| Byte | Field | Values |
|---|---|---|
| 0-2 | called ID | 24 bits: who PTT calls. A common ID `n` is `n << 16 \| 0xFFFF` (8.1.1); 0 (reserved) and `FFFFFF` call all |
| 3 | channel code | 0 automatic: the code table 6.1 ties to the transmit frequency (`dpmr_cc_for_hz`, group A's first off the dPMR446 channels); 1-16 group A's codes, 17-32 group B's. Past 32: automatic |
| 4 | flags | bit 0: play only calls to the called ID, to all and to our own ID (else every call); bits 1-6 reserved, kept as found; bit 7 written 0, so the byte is never the erased `FF`, which reads as none |

A short block reads the missing bytes as 0: an all call, the automatic code, every call played.

### 6.2 The radio-wide record (3 bytes)

Our own ID, 24 bits (key `0x0A00` + 7). Absent, short, 0 or `FFFFFF`: none, and each channel's called ID is sent as
our own too. A licence-free dPMR446 radio does just that (it addresses with one common ID, which names both ends), so a
codeplug with no record talks to one as it comes.

The Codeplug Editor's module is `site/static/js/codeplug/mode_dpmr.js`: it shows an ID as a common ID (1-255), with the
full address as a second field. The shared vectors (`dpmrchan`, `dpmrenc`, `dpmrown` in
`tests/vectors/channel-modes.txt`) are read by both its tests and the firmware's.

## 8. Tests

| Test | Covers |
|---|---|
| `tests/host/dpmrTest.c` | Hamming, scrambler, interleaver, channel codes and the text the screens show; every line of `tests/vectors/dpmr.txt` (`tools/dpmr_ref.py`, written from the standard alone): the block codes, whole frames, and seven blocks a real radio sent; whole calls through the framer: hard and soft symbols at other levels and offsets, ten wake-up headers, late entry next to a channel code two symbols from FS2, a lost FS2, a corrected CCH, a call cut off, noise, two calls back to back |
| `tests/host/dpmrEngineTest.c` | two engines on made-up providers: a call with its IDs and voice in order and whole superframes, an encoder that stalls, PTT let go at once, late entry, the filter changed mid-call, an abort, refusals; a call through a modeled discriminator tap and the soft modem, with noise, to where it breaks |
| `tests/host/dpmrCaptureTest.c` | a real dPMR446 radio's transmission through the soft modem, the framer and the call layer, when a recording is given (10); skipped without |
| `tests/host/dpmrCodeplugTest.c` | the codeplug (6): the shared vectors, the channel code a channel sends, a channel's block from its tag, the own ID record, a save from the radio that keeps a later firmware's flags, fallback and bytes; built with `CONFIG_DPMR` off too, where the kind is parked |

## 9. Not written

Slow data and the data modes (mode 2-3, packet data, FS4), the configured-services call set-up (acknowledgements,
status messages, OACSU), powersave, the channel code as a receive filter, and modes 2 and 3 (TS 102 658). No dPMR
voice privacy: TS 102 490 defines none (what some radios call a dPMR scrambler is their maker's own). A header of a
type other than a communication start is read and kept, and answers nothing.

## 10. Checked against a real radio

A public recording of a licence-free dPMR446 handheld (the Signal ID Wiki's dPMR example, an I/Q file) is the one
check against a radio that is not ours. The tree keeps no audio: `dpmrCaptureTest` reads the recording from a path
given to it and skips without one, and `tests/vectors/dpmr.txt` keeps only seven coded blocks read off it (the air*
lines). Through a discriminator at 8 kHz, the soft modem, the framer and the call layer it gives one call, ended by
its end frame: common ID 87 to common ID 87, channel code `1A4` (A2), 360 payload frames with no CCH failure, and all
1440 voice frames, 1285 of them with no AMBE+2 FEC correction and none with C0 lost. The soft modem finds its timing in
the sixth of the transmission's nine headers, about 400 ms in: the last four headers are read, and the call is whole
from its first payload frame.
