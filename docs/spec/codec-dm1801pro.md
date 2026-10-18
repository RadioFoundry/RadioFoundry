<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# DM-1801 Pro: the AMBE+2 codec and how it is called

Status: research, 2026-10-15. Taken from the radio's complete program flash image (1,048,576 bytes, sha256
`072ece15...9f29e42a`). Nothing here has run on a radio.

The rule of [codec-dm32.md](codec-dm32.md) holds: the radio runs the codec it already carries, the user's machine
carves it from the radio's own flash image or an original update file, and no codec bytes go in this repository or in
a release.

**Result.** The DM-1801 Pro's codec, its init routine and the wrappers that call it are the DM-32UV 049 build, with
the data tables and the state blocks at different RAM addresses. ODR's DM-32UV hosting
([codec-dm32.md](codec-dm32.md), `platform/hrc7000/vocoder.c`) therefore interfaces with it the same way. The changes
for this board are addresses. One thing in that interface was wrong: the encode flags word
([section 3](#3-how-the-codec-is-called) and [section 5](#5-cross-check-against-an-independent-build)).

Tags: K = known from the image, I = inferred, ? = not determined.

## 1. Where it is

| Item | Value | Tag |
|---|---|---|
| Code, flash | 0xA22F0..0xB68F0 (0x14600 bytes) | K |
| Code, RAM (run address) | 0x00016CDC..0x0002B2DC; the start-up code copies flash 0x9B614.. (0x22918 bytes) to RAM 0x10000 | K |
| Signature | first words 0x152514D8, 0x74C776CD | K |
| Code sha256 | `8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42` | K |
| Tables, flash | 0x68770, 0x2F78 bytes | K |
| Tables, RAM | 0x18000EF0..0x18003E68 (the data image is flash 0x68080 to RAM 0x18000800, 0x43FC bytes) | K |
| Tables sha256 | `10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1`, equal to the DM-32UV 049 tables | K |
| Self-referencing absolute addresses | the three pool words at RAM 0x187B0, 0x187B4, 0x19B68 hold 0x19540, 0x22754, 0x26CAC, as in the 049 code | K |
| Decoder state D, encoder state E | D at RAM 0x4885C, E at 0x4B2FC (049 DM-32UV: 0x456C0 and 0x48160, both 0x319C lower) | K |

### Against the DM-32UV 049 codec

| | |
|---|---|
| Identical bytes | 83,298 of 83,456 |
| Table pointers | 82 literal words point into the tables and are 0x48 higher (the tables start at 0x18000EF0, not 0x18000EA8) |
| Non-codec code inside the range | 29 words differ inside the three ranges that are not codec code (RAM 0x17270..0x1732C, 0x1CDBC..0x1D4C8, 0x1E8BC..0x1FE64) |
| Everything else | equal |

The whole RAM code block shares 82.7% of its 16-byte chunks with the DM-32UV 049 image. The shared part includes every
function the codec calls and all the wrappers in section 3. The differences are hardware-specific code (UART, the
modem and display glue) and addresses.

## 2. Entry points

All eight sit at the 049 offsets from the code start and begin with the same prologues (K).

| Function | Offset | RAM | Prologue |
|---|---|---|---|
| decode | +0x0000 | 0x16CDC | `push r4-r11, r15` |
| encode | +0x0260 | 0x16F3C | `push r4-r10, r15` |
| ECC | +0x1C60 | 0x1893C | `push r4-r11, r15` |
| decoder init | +0x00B4 | 0x16D90 | `push r4-r5, r15` |
| decoder set | +0x0084 | 0x16D60 | `push r4-r6, r15` |
| encoder init | +0x0314 | 0x16FF0 | `push r4-r10, r15` |
| encoder set | +0x02E4 | 0x16FC0 | `push r4-r6, r15` |
| FEC init | +0x1BF8 | 0x188D4 | `push r4-r5, r15` |

## 3. How the codec is called

### Init

Byte for byte the DM-32UV 049 routine, with D and E at the addresses in section 1. The parameter values and the order
are those of [codec-dm32.md](codec-dm32.md) C: E[0x172C] = 0x1840, E[0x173A] = 0x2000, E[0x173C] = 128, E[0x173E] =
-20, E[0x1740] = -22, E[0x1742] = 20, E[0x1744] = -10, E[0x1746] = -40, E[0x1748] = 0x4000, E[0x174A] = E[0x174C] =
3276, E[0x174E] = 1310, E[0x1750] = 10, E[0x1752] = 20, D[0x1152] = 0, D[0x116A] = 128, D[0x116C] = -20. Then:

1. encoder init (E, 1, 1)
2. FEC init (E + 0x16E0, 1)
3. encoder set (E, E[0x173C], E[0x173E])
4. decoder init (D, 1)
5. FEC init (D + 0xFD0, 1)
6. decode (D + 0x12AE, 80, D + 0x1354, 0; stack 8, 0, D)
7. decoder set (D, D[0x116A], D[0x116C])
8. clear the state flags (halfwords and bytes around E + 0x1732..0x1736 and D + 0x1162..0x1168, and the bytes at E + 0x1728..0x19D6 and D + 0x1150..0x1498)

`platform/hrc7000/vocoder.c` `reset()` makes the same calls with the same values (K).

### Encode

| Argument | Value |
|---|---|
| r0 | the output bit buffer |
| r1 | E[0x1736] |
| r2 | input samples: E + 2 * (E[0x1896] + 2986) * 2 |
| r3 | 80 |
| sp+0 | the flags word, see below (0x800 in this pattern) |
| sp+4 | 1 (0 on the first half) |
| sp+8 | E[0x173A] |
| sp+12 | E |

This is the DM-32UV call ([codec-dm32.md](codec-dm32.md), calling convention) and what `encode()` in `platform/hrc7000/vocoder.c` passes (K).

**The flags word (sp+0).** The wrapper has six encode calls in two patterns. Each pattern passes one value on every
call; none passes 0x1840 first and 0x800 afterwards. Every call is followed by the XOR step and ECC.

| Pattern | sp+0 | Calls |
|---|---|---|
| A | a literal 0x800 | three (both halves of a frame, and one more path) |
| B | E[0x172E], a copy of E[0x172C] made on the first call of the sequence | three |

E[0x172C] is 0x1840 after init, and the one routine that writes it again has no caller, so pattern B always passes
0x1840. The wrapper picks the pattern from the bytes E[0x1728] and E[0x172A] and the halfword E[0x1732], which code
outside the codec sets; which pattern DMR voice takes is **not traced**.

The two values give different parameter bits ([section 5](#5-cross-check-against-an-independent-build)): over 234 frames
of speech, 0x800 and 0x1840 differ in 174 frames (74%). On speech bit 0x1000 has no effect (0x840 equals 0x1840): it is
tone detection, which acts on a steady tone or tone pair ([codec-md380.md](codec-md380.md), "Encode flags"). Bits 0x40, 0x80,
0x100, 0x200 and 0x800 are tested in the parameter stage; 0x800 only counts while E[0x138A] is not 0. Both patterns
produce frames the decoder accepts.

**What ODR's hosts do.** `platform/hrc7000/vocoder.c` (this radio and the DM-32UV), `boards/dm1701/vocoder_dm1701.c`
and `boards/md380/vocoder_md380.c` pass one word on every call, from `drivers/vocoder/ambe2_flags.h`: 0x800, plus
0x40 for noise reduction, 0x1000 for tone frames and 0x2000 for AGC. The default is 0x1840, pattern B's value
([dmr-voice.md](dmr-voice.md) 6).

Afterwards the wrapper XORs the bit buffer with a second buffer 0x90 bytes above it, then calls ECC (0x1893C) with
r0 = r1 = the bit buffer, r2 = E[0x1738], r3 = the buffer minus 440 bytes. The XOR and the ECC step belong to the
wrapper's own frame building (72 bits); ODR takes the 49 parameter bits from the encode call and builds the frame in
`core/dmr/ambe`.

### Decode

The normal frame path:

| Argument | Value |
|---|---|
| r0 | output samples: D + 2 * (D[0x134E] + 2230) + 2 |
| r1 | 80 |
| r2 | the input bit buffer |
| r3 | D[0x1166] |
| sp+0 | a flags word, see below |
| sp+4 | 1 |
| sp+8 | D |

One other call passes sp+4 = 0. **The flags word** is the status the wrapper gets back from a 72-bit error-correcting
decode of the received frame (RAM 0x182BC, run on each of the three sub-frames, I). The wrapper stores each result in D
at 0x1156 + 6 * k (+ 2 for the second and third sub-frame), and the decode call reads one back (the halfword at
D + 0x1154 + 2 * (3 * k + D[0x1164]) + 2) and ORs in D[0x1154], a copy of D[0x1152] (0 after init). So the wrapper tells
the codec how damaged the frame was, and the codec can conceal damage itself.

**Measured effect of the flags word** (values 0..0xF, each single bit and random words, clean and noisy input, same on
the DM-1801 Pro codec and an independent ARM build): bits 0 and 1 change nothing, so 0..3 decode like a good frame; any
value with bit 2 or 3 set (4..0xF) makes the decoder repeat the previous frame, which is silence when there is none;
0x4000 replaces the output with a steady noise at a fixed level (RMS about 1190); every other bit does nothing.
`platform/hrc7000/vocoder.c` passes 0 and does its own error correction and concealment in `core/dmr/ambe`. That is right for a
good frame, and 0 is also the status an error-free decode returns (see [codec-md380.md](codec-md380.md)). It gives the
codec no damage information, so the repeat-previous-frame concealment (4 or 8) never runs and ODR conceals before the
codec instead. Whether the two sound the same at the edge of coverage is a hardware question, not an interface error.

### Level setter

`set(level, which)`. With which = 0: E[0x1736] = D[0x1166] = min(level, 4). With which != 0: E[0x1738] = D[0x1168] =
min(level, 6). The wrapper calls it from four places and only with which = 1: level 0 twice and level 4 twice. So
**E[0x1736] and D[0x1166] stay 0** (the host's use of the state's own value is right), and E[0x1738] / D[0x1168], the
ECC parameter, is 0 or 4 by mode. ODR never calls ECC, so it doesn't matter unless a later version moves frame building
into the codec.

### Not part of the interface

The wrapper's start-up also creates three RTOS semaphores (names in the data image at RAM 0x18000EA0, 0xEAC, 0xEB8) and
stops with an error code 1, 2 or 3 if one fails; that is task start-up, not the codec.

## 4. How ODR hosts it

| Part | What it does |
|---|---|
| Slot tool | `tools/dm32_codec.py --board dm1801pro` takes a 1 MB program flash read (offset 0 = 0x03000000), recognizes it by the sha256 of the code window (`8f7641b6…`), checks the tables' sha256 (`10684c33…`) and carves the two windows of section 1 into the slot at 0x030E8000, in the DM-32UV slot format. `tools/codec_auto.py` runs it on `make` when the read is in `codecs/` |
| Host | The shared `platform/hrc7000/vocoder.c`, the DM-32UV's. It takes its RAM windows from the linker (`__codec_run_lo/hi`, `__codec_tab_lo/hi`) and its profile prefix from `HRC_CODEC_BOARD`, so it reports `codec=dm1801pro-049` |
| Linker | `boards/dm1801pro/dm1801pro.ld`: code RAM 0x16CDC..0x2B2DC and table RAM 0x18000EF0..0x18003E68 are left free; ODR's own RAM starts at 0x2B2DC; the low-RAM state (`ODR_CPU_RAM_SECTION`) goes below 0x16CDC |
| Flashing | `tools/odrflash` profile `dm1801pro-049`. Through update mode the slot is merged on every flash, as on the DM-32UV (the updater erases the whole application area). Through the stub (`stub/stubtool.py --write-app`) only the sectors the image covers are written, so an image without the slot keeps the one in the radio |

Measured on the radio: encode about 12.3 ms per 20 ms frame at the top priority, decode about 2.4 ms, stack use about
3.9 KB. RX and TX voice work on the bench (hardware-dm1801pro.md 5).

## 5. Cross-check against an independent build

The DM-1801 Pro codec was run on a host and compared with the MD-380-generation build of the same codec, run in an
emulator from a running radio's RAM image. The host used the reset sequence of `platform/hrc7000/vocoder.c` on state blocks at the
addresses in section 1. Neither is part of this repository.

| Check | Result |
|---|---|
| Decode, 234 frames of a real call, fresh state | 37,440 of 37,440 samples identical |
| Decode, flags 0..0xF, each single bit and 12 random words, clean and 10% bit errors, 120 frames each | identical for all 40 values |
| Encode, speech plus tone, noise, full-scale noise, silence, square wave and sweep (1,184 frames), flags 0x1840 | 0 frames differ |
| Encode, 43 flags values (every single bit, 0x800 and 0x1840, 24 random words), 120 frames each | identical for 33; the 10 that differ all have bit 0x2000 set |
| Code covered by those runs | 83% of the codec's instructions executed; every multiply-accumulate and saturating variant ran thousands of times |

What this settles:

* The DM-1801 Pro codec is functionally the same codec as the MD-380 one, for decode and encode with any flags. The
  differences with bit 0x2000 set came from different values in the AGC parameter block (E+0x16A2..0x16B6), not from
  different code ([codec-md380.md](codec-md380.md), "Encode flags").
* The reset sequence in section 3 and the argument values `platform/hrc7000/vocoder.c` passes (r1 = E[0x1736], sp+8 = E[0x173A],
  decode r3 = D[0x1166], two 80-sample calls per frame) are right. They produced identical output to a radio that
  had been running.
* The encoder delays its output by one frame: a tone that starts in input frame 10 first changes output frame 11.
* The hosts pass the encode flags word as the wrapper does: one value on every call (section 3).

## 6. Open

- Which encode flags pattern DMR voice uses (0x800 or 0x1840): what sets E[0x1728], E[0x172A] and E[0x1732] before the
  encode calls. A bench transmission with each value, decoded by a second radio, would also settle it.
- The return value of the error-correcting decode (0x182BC) on this build. The flags measurement above shows 0..3 are
  harmless and 4..0xF repeat the frame; the MD-380 build returns 0 good and 4 for an uncorrectable C0.
- What the XOR step before ECC does with the second buffer.
- Whether this radio has the 046 HRVocoder variant of the codec in some firmware versions. The image is one version of
  the radio's firmware.
- Audio quality and timing on hardware.
