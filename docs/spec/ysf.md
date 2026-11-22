<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# System Fusion (YSF)

System Fusion's digital voice (C4FM, DN mode) on the air interface ([air-interface.md](air-interface.md)): frames,
calls, the radio core's YSF mode and the radio ID. Switch: `CONFIG_YSF`, which also builds the air interface
([build-config.md](../build-config.md)). Which radios could carry it: [feature-matrix.md](../feature-matrix.md). The
codeplug's records: [ysf-codeplug.md](ysf-codeplug.md).

There is no published standard for System Fusion. Everything here was measured on air against other System Fusion
equipment (a handheld direct and through a repeater) or follows from those measurements; what isn't known says so.

State:

| Part | State |
|---|---|
| Frame codec: sync, FICH, header and terminator channels, DCH, VCH (2, 3) | written, host tested |
| Receive, the call layer, the engine (4, 5) | written, host tested: header and late entry, the DG-ID squelch |
| Voice | through the DMR side's voice path to the vocoder the board hosts (`hal_vocoder_t`), as NXDN's (5.2) |
| The radio core: mode, PTT, scan, dual watch, debug link, Home (5) | written, host tested |
| The radio ID (6) | written, host tested |
| Codeplug channels, the site's Codeplug Editor ([ysf-codeplug.md](ysf-codeplug.md)) | written, host tested |
| On air | not checked with this code; see the radio's bring-up record |
| VW (full rate voice), V/D mode 1, data mode, Wires-X, GPS | not written (8). Shown on Home, not played |

## 1. On air

| Item | Value |
|---|---|
| Channel | 12.5 kHz |
| Modulation | 4-level FSK, 4800 symbols/s (9600 bit/s) |
| Deviation | ±2700 Hz outer, ±900 Hz inner |
| Pulse | root raised cosine, roll-off 0.2 |
| Dibits | the air interface's: `01` +3, `00` +1, `10` -1, `11` -3 |
| Timing | continuous, 100 ms frames |

Bits are numbered from the most significant bit of a frame's first byte; a symbol is two bits, the first the higher.
Frame buffers hold the bits as they go on air, which is also the air interface's packed dibits.

## 2. The frame

One frame is 480 symbols (960 bits, 120 bytes, 100 ms):

| Symbols | Bytes | Part |
|---|---|---|
| 0-19 | 0-4 | Sync, `D4 71 C9 63 4D` |
| 20-119 | 5-29 | FICH (3.1) |
| 120-479 | 30-119 | Payload: five blocks of 72 symbols (144 bits) |

What a payload block holds depends on the FICH:

| Frame | Each block |
|---|---|
| Header, terminator | 36 symbols of CSD1, then 36 of CSD2 (3.2) |
| Communications, DN (DT 2) | 20 symbols of the DCH (3.2), then 52 of one VCH (3.4) |

## 3. Channels

### 3.1 FICH

Four octets of fields, a CRC-16 (3.2) over them, as four 12-bit words each in an extended Golay(24,12) code (the
generator `0xC75`, systematic, the overall parity bit last: the code DMR's voice frames use for their first word).
The 96 bits and four zero tail bits go through the convolutional code (3.2), and the 200 coded bits are interleaved
as a data channel of 5 columns, into symbols 20-119 in order.

| Octet | Bits | Field |
|---|---|---|
| 0 | 7-6 | FI: 0 header, 1 communications, 2 terminator, 3 test |
| 0 | 5-4 | CS: callsign information. Every radio heard sends 2 |
| 0 | 3-2 | CM: call mode. 1 with a destination of `*****` and the radio ID (4.2) |
| 0 | 1-0 | BN: block number (data mode) |
| 1 | 7-6 | BT: block total (data mode) |
| 1 | 5-3 | FN: frame number in the DCH cycle |
| 1 | 2-0 | FT: the cycle's last FN. DN sends 6: FN 0-6 |
| 2 | 6 | DEV: narrow deviation. 0 |
| 2 | 5-3 | MR: message route. 0 direct, 1 to an idle repeater, 2 to a busy one |
| 2 | 2 | VoIP |
| 2 | 1-0 | DT: 0 V/D mode 1, 1 data at full rate, 2 V/D mode 2 (DN), 3 voice at full rate (VW) |
| 3 | 7 | SQL: the DG-ID is in use |
| 3 | 6-0 | DG-ID, 0-127 (radios use 0-99) |

A receiver takes a frame only when all four Golay words decode and the CRC holds: that also confirms the 20-symbol
sync, which turns up in noise and payload.

### 3.2 Data channels: CRC, code, interleaving

The header's two long channels (CSD1, CSD2) and the DN data channel (DCH) are coded alike:

1. The content (20 bytes for a CSD, 10 for the DCH) is whitened (3.3).
2. A CRC-16 over the whitened bytes is appended, high byte first: polynomial x^16 + x^12 + x^5 + 1, the register
   cleared at the start, the result inverted. Its check value over the ASCII "123456789" is `0xCE3C`.
3. Four zero tail bits follow, and the whole goes through the convolutional code: rate 1/2, constraint length 5,
   G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4 (the code NXDN uses; `core/fec/conv.c`), G1's bit first.
4. The coded bits are interleaved two at a time (a dibit): coded dibit i goes to channel position
   (i mod c) x 20 + i / c, where c is the channel's column count.
5. The channel's positions are spread over the five payload blocks, 4 x c symbols in each, at the channel's offset in
   the block.

| Channel | Content | Input bits (with CRC and tail) | Columns | In each block |
|---|---|---|---|---|
| FICH | 4 octets | 48 + 48 Golay parity + 4 | 5 | (symbols 20-119 instead) |
| CSD1 | destination, source (10 bytes each) | 160 + 16 + 4 | 9 | symbols 0-35 |
| CSD2 | downlink, uplink | 160 + 16 + 4 | 9 | symbols 36-71 |
| DCH | 10 bytes, by FN (4.1) | 80 + 16 + 4 | 5 | symbols 0-19 |

The decoder is the shared soft-decision Viterbi decoder. A received symbol's soft value gives its two bits: the first
from its sign, the second from its size against the level between inner and outer.

### 3.3 Whitening

The content of the CSD, the DCH and the VCH is XORed with one sequence, from its first bit for each channel: the
outputs of x^9 + x^5 + 1, each the XOR of the outputs 5 and 9 before it, the first nine being 1 0 0 1 0 0 1 1 1. Its
first 160 bits:

```
93 D7 51 21 9C 2F 6C D0 EF 0F F8 3D F1 73 20 94 ED 1E 7C D8
```

### 3.4 Voice channel (VCH)

A VCH carries one 20 ms frame of the AMBE+2 vocoder at 2450 bit/s: 49 parameter bits, in the order DMR's voice
frames carry them (parameter bits 0-11 first; `core/dmr/ambe/ambeFec.h`). The same 49 bits go in both systems: only the
protection around them differs.

1. Parameter bits 0-26, each sent three times in a row (81 bits), then bits 27-48 once (22 bits), then a zero bit:
   104 bits.
2. Whitened (3.3).
3. Interleaved 26 rows by 4 columns: bit i goes to position (i mod 26) x 4 + i / 26.

Received, each of the 27 repeated bits is the sign of the sum of its three copies' soft values (the whitening
undone by turning a copy's sign); bits 27-48 have no protection. The 49 bits then go to the DMR side's voice path in
its 72-bit frame (`ambeFecEncode`), which carries them without loss either way.

## 4. A DN call

### 4.1 The transmission

| Part | Frames | Contents |
|---|---|---|
| Preamble | 100 ms | `0x77` (+3 -3 +3 -3): the receivers' level and timing reference |
| Header | 1 | FI 0, FN 0; CSD1 = destination and source, CSD2 = downlink and uplink |
| Communications | as long as PTT is held | FI 1, DT 2, FN 0, 1 ... 6, 0, 1 ...; the DCH by FN, five VCH |
| Terminator | 1 | FI 2; the header's channels again |

A communications frame goes on air only once its first voice frame is in; an over that ends before then sends the
terminator straight after the frame before. A frame still short of voice when the air would run dry is filled with
silence.

The DCH cycle:

| FN | DCH content (10 bytes) |
|---|---|
| 0 | destination |
| 1 | source: our callsign, space padded |
| 2 | downlink: spaces |
| 3 | uplink: spaces |
| 4 | remarks 1-2: spaces |
| 5 | remarks 3-4: five spaces and our radio ID |
| 6 | the fixed block of 4.2 |

### 4.2 The fields as one maker's handheld sends them

RadioFoundry fills every field as a Yaesu FT-70D handheld does (recorded on air), so that repeaters, Wires-X nodes and
other radios take its transmissions as they take that radio's. What the handheld sends that a minimal transmitter
would leave blank:

| Field | What we send, as the handheld | Why it matters |
|---|---|---|
| FICH CM | 1 | Says the destination field holds `*****` and the radio ID rather than a callsign |
| FICH CS | 2 | Every radio heard sends 2 |
| Destination (header CSD1, DCH FN 0) | `*****` and our radio ID, e.g. `*****K7Q2Z` | Carries the radio ID (6) from the header on |
| Remarks 3-4 (DCH FN 5) | five spaces and the radio ID | Gives the radio ID to a receiver that missed the header (late entry) |
| DCH FN 6 | `01 22 61 5F 2B 03 11 00 00 00` | Copied from the handheld byte for byte |
| FICH DEV, VoIP, SQL | 0 | As the handheld |

Which field tells a receiver the radio's model is not known. The CM and destination layout tell a radio-ID sender
from a callsign-only one. The FN 6 block is the likeliest to name the model or its settings, but its bytes have not
been decoded: other models' FN 6 still has to be recorded to compare. Until then it is sent as recorded, never
changed per radio. A receiver reads none of it except the radio ID.

Received, the caller's radio ID comes from a destination that starts with `*****`, or from the last five characters of
remarks 3-4, whichever arrives first; five characters that aren't all 0-9 or A-Z are no radio ID.

### 4.3 Receive

| Event | What the call layer does |
|---|---|
| Header | A new call (one already running ends: its terminator was lost); the callsigns from CSD1 |
| Communications frame, no call | A late entry: the call starts, its fields come from the DCH as the FNs pass |
| Communications frame | The DG-ID, DT and MR from the FICH; the DCH by FN; five voice frames if DT is 2, else none (shown as not DN voice) |
| A frame whose FICH fails | Taken as the communications frame due, for up to three in a row: its DCH as the next FN, its voice played |
| Terminator, or 400 ms without a sync | The call ends |

## 5. The radio core

| Item | Rule |
|---|---|
| Mode | `RADIO_MODE_YSF`, a channel's mode only: the board stays in its digital mode, its DMR stack stopped, as for P25 and NXDN. Offered where the board's air interface fits the profile (`radio_ysf_fits`) |
| Channels | A stored channel of kind 103 runs in YSF with its own DG-IDs ([ysf-codeplug.md](ysf-codeplug.md)); the VFO and a channel in YSF for now take the RAM settings (DG-ID 0 both ways until the debug link sets others) |
| Memory | The engine runs in P25's memory where P25's isn't lent to the vocoder, else in one block it shares with NXDN (`radio_air_mem.c`): one air mode runs at a time |
| PTT | Refused without a callsign (`TXDENY_NO_ID`), without the vocoder (`TXDENY_NO_CODEC`), and where the provider can't transmit the profile |
| Scan, dual watch | As NXDN: a stop holds on a call it would play |

### 5.1 The DG-ID squelch

| Receive DG-ID | Played |
|---|---|
| 0 | every DN call, whatever its DG-ID |
| 1-99 | DN calls with that DG-ID only; the rest show as "Other DG-ID: not played" |

PTT sends the transmit DG-ID. A channel stores both; the channel editor sets one value for both.

### 5.2 Callsign

The callsign sent is the codeplug's YSF callsign ([ysf-codeplug.md](ysf-codeplug.md) 1), else the radio name's first
word when it reads as a callsign (3-10 letters and digits, at least one of each; `ysf_callsign_from_name`). With neither,
Home says "No callsign" and PTT is refused.

### 5.3 Debug link

The `ysf` view section (DL_VIEW_YSF, [debuglink.md](debuglink.md)): the call heard (`ysf.src`, `ysf.src_rid`,
`ysf.dgid`, `ysf.played`, `ysf.late`, `ysf.not_dn`), who we are (`ysf.callsign`, `ysf.rid`, `ysf.rid_weak`), the
DG-IDs set and in force, the provider's fit and the counters (`ysf.frames`, `ysf.fich_fail`, `ysf.dch_fail`,
`ysf.csd_fail`, `ysf.voice`, `ysf.fec_bits`, `ysf.vch_fixed`, ...) and the receive options (5.4: `ysf.rx_eq`,
`ysf.rx_tone_on`, `ysf.rx_tone` the tone notched, `ysf.rx_eq_center`). RADIO op 44 sets the RAM DG-IDs: u8 transmit,
u8 receive, 0-99 each, and optionally u8 bench: bit 0 turns the equalizer off, bit 1 the tone notch, for an A/B. The
callsign and the radio ID can't be set there.

### 5.4 Receive through a repeater

A repeater's chain cuts the upper band of the 4FSK (inter-symbol interference) and some repeaters send a CTCSS tone
under C4FM, which other System Fusion radios ignore. YSF's profile asks a soft modem for its equalizer and tone notch
(`AIR_RX_EQ | AIR_RX_TONE`, [air-interface.md](air-interface.md) 6.1), and the engine lends their memory for its
session. A board whose provider receives with its own demodulator ignores them.

## 6. The radio ID (RID)

### 6.1 What it is and why it must be unique

Every System Fusion radio sends a radio ID: five characters, each 0-9 or A-Z, in the header's destination and again in
the DCH (4.2). Repeaters, Wires-X nodes and other radios keep it with the callsign, and whatever picks out one radio
from another goes by it: lists of radios heard, a node's view of who is on it, a repeater's list of the radios it
lets in. Two radios with the same radio ID look like one radio to all of them: their entries merge, one's calls are
shown as the other's, and a list that admits one admits both.

**Every radio running RadioFoundry therefore has a radio ID of its own, different from every other radio's**, and
nothing a user does can make two radios share one. That is the whole design of this section.

### 6.2 Where it comes from

The radio ID is made from the MCU's factory-unique ID (`hal_chip_t.uid`, 96 bits: set when the chip is made and
different for every chip) and from nothing else:

| Rule | Why |
|---|---|
| Made from the MCU's unique ID | The one value that differs from radio to radio by construction |
| Never stored in the codeplug | A codeplug is copied from radio to radio (a CPS file, zone sharing, a club's file): a stored ID would be copied with it |
| Never typed or set: no menu, no editor field, no debug link command | A hand-set ID is copied from a radio to another sooner or later |
| The same on every start, after every update and codeplug write | It is a function of the chip alone |
| Not carried by zone sharing | Zone sharing never sends the radio's own IDs ([zone-share.md](zone-share.md)) |

### 6.3 How it is made

`core/ysf/ysf_id.c`, `ysf_rid_from_uid`:

1. h = `0x9E3779B9`.
2. For each of the ID's three 32-bit words, lowest first, then the number 3: h = mix(h, word), where mix(h, w) is
   h ^= w; h x= `0x85EBCA6B`; h ^= h >> 13; h x= `0xC2B2AE35`; h ^= h >> 16 (32-bit arithmetic).
3. h modulo 36^5 (60,466,176), written as five base-36 digits, the most significant first, with the digits 0-9 then
   A-Z.

The mixing gives a full avalanche: chips whose IDs differ in one bit (neighbors on a wafer) get unrelated radio IDs,
and every one of the 60,466,176 values is about as likely as any other (`ysfTest` checks both).

| Radios that hear each other | Chance that any two of them share a radio ID |
|---|---|
| 2 | 1 in 60 million |
| 10 | 0.0001 % |
| 100 | 0.008 % |
| 1,000 | 0.8 % |

Five characters is all the field holds: the chance can't be made smaller without changing the format. It is the same
for any maker's radios whose IDs are spread evenly over the field.

### 6.4 A board without a unique ID

A board whose MCU gives no usable unique ID (none, or all zeros or all ones) falls back to a radio ID made the same
way from its DMR ID and radio name (`ysf_rid_fallback`). That one is only as unique as those two, which a copied
codeplug shares, so it is marked everywhere it shows: a `?` after it in Settings > My radio, `ysf.rid_weak=1` on the
debug link, `RADIO_YSF_RID_WEAK` in the radio view. Which boards have a unique ID: their hardware pages.

### 6.5 Where it shows

| Place | Shows |
|---|---|
| Settings > My radio > Radio ID | The radio ID; OK says it is made from the chip and can't be edited |
| Home, on a YSF channel | `RID` and the radio ID in the chips |
| Debug link | `ysf.rid`, `ysf.rid_weak` |

A caller's radio ID shows as `ysf.src_rid` on the debug link.

## 7. Choices

| Choice | Why |
|---|---|
| The engine shares NXDN's memory | One air mode runs at a time; a block of its own cost 1.9 KB of RAM |
| No transmit level, preamble or field-layout settings | The values here are the measured ones; a setting would only let a radio drift from them |
| One DG-ID value for both ways in the channel editor | What a group on a repeater uses; the codeplug keeps both (the Codeplug Editor sets them apart) |

## 8. Not written

VW (voice at full rate), V/D mode 1, data at full rate and the Wires-X commands it carries, positions (GPS), the
repeater side (MR other than direct on transmit). A call of another data type is shown, not played.
