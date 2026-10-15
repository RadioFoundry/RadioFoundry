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
([codec-dm32.md](codec-dm32.md), `boards/dm32/vocoder_dm32.c`) therefore interfaces with it the same way. The changes
for this board are addresses.

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

`boards/dm32/vocoder_dm32.c` `reset()` makes the same calls with the same values (K).

### Encode

| Argument | Value |
|---|---|
| r0 | the output bit buffer |
| r1 | E[0x1736] |
| r2 | input samples: E + 2 * (E[0x1896] + 2986) * 2 |
| r3 | 80 |
| sp+0 | 0x800 |
| sp+4 | 1 (0 on the first half) |
| sp+8 | E[0x173A] |
| sp+12 | E |

This is the DM-32UV call ([codec-dm32.md](codec-dm32.md), calling convention) and what `encode()` in `vocoder_dm32.c` passes (K).

The first call of a transmission passes 0x1840 (E[0x172C]) at sp+0, as the host does; later calls pass 0x800.
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

`vocoder_dm32.c` passes 0 and does its own error correction and concealment in `core/dmr/ambe`. That is the right value for a frame with no errors if the status is 0 there (not checked: the return value of 0x182BC was not traced). It gives the codec no damage information on a bad frame, so its internal concealment never runs; ODR conceals before the codec instead. Whether the two sound the same at the edge of coverage is a hardware question, not an interface error: every other argument matches.

### Level setter

`set(level, which)`. With which = 0: E[0x1736] = D[0x1166] = min(level, 4). With which != 0: E[0x1738] = D[0x1168] =
min(level, 6). The wrapper calls it from four places and only with which = 1: level 0 twice and level 4 twice. So
**E[0x1736] and D[0x1166] stay 0** (the host's use of the state's own value is right), and E[0x1738] / D[0x1168], the
ECC parameter, is 0 or 4 by mode. ODR never calls ECC, so it doesn't matter unless a later version moves frame building
into the codec.

### Not part of the interface

The wrapper's start-up also creates three RTOS semaphores (names in the data image at RAM 0x18000EA0, 0xEAC, 0xEB8) and
stops with an error code 1, 2 or 3 if one fails; that is task start-up, not the codec.

## 4. What this means for ODR on this board

| Need | Why |
|---|---|
| Table RAM window 0x18000EF0..0x18003E68 | The code's literal pointers expect it. The DM-32UV host accepts tables only inside 0x18000E48..0x18003E20 (`TAB_HI`), so it would reject this slot unless the window widens or the tables are carved for a moved base |
| Code RAM window 0x16CDC..0x2B2DC | Inside the DM-32UV host's 0x16CDC..0x2F914 window |
| A slot tool for this radio | `tools/dm32_codec.py` carves by the hash of an update file; this radio's source is a flash image. The windows above are the recipe; carve the code and tables, not the RAM image around them |
| Linker | Reserve RAM 0x16CDC..0x2B2DC and the table window, as `boards/dm32/dm32.ld` does, and leave out the 0x18000800.. words this firmware's own `.data` uses |
| Version check | Two signature words, then the sha256 of the carved windows from the table in section 1 |

## 5. Open

- The return value of the error-correcting decode (0x182BC): is it 0 for a clean frame, and which values mean erasure or
  repeat? That settles whether passing 0 is exact for good frames and whether feeding the codec a status on bad ones would
  sound better than ODR's own concealment.
- What the XOR step before ECC does with the second buffer.
- Whether this radio has the 046 HRVocoder variant of the codec in some firmware versions. The image is one version of
  the radio's firmware.
- Audio quality and timing on hardware.
