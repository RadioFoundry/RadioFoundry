<!-- SPDX-License-Identifier: MIT -->
# MD-9600: hosting the stock AMBE+2 codec

Status: research, 2026-10-12, **incomplete**. Static facts only. The full stock file is not on hand; what exists is the
decrypted 0x48BB0-byte carve the old builds used (outside the repository). Nothing has run on a radio or in the
emulator for this radio yet (TODO below).

Rule: the radio runs exactly the codec its own TYT stock firmware shipped. The user's machine extracts it from a stock
file the user supplies. No codec bytes go in this repository or in released binaries. For the MD-9600 this is the
same file the old trees used as a donor for every STM32 radio: here it is finally the radio's **own** codec.

## Stock files

| version | file | sha256 | on hand |
|---|---|---|---|
| **V26.45, V5 board (only one known)** | `MD9600-CSV(2571V5)-V26.45.bin` | `d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f` | **no** (hash from the old tree's loader and [hardware-dm1701.md](hardware-dm1701.md) 1.4) |

The name reads as: CSV feature set (user database, no GPS, no recorder: the "V" family of [hardware-md9600.md](hardware-md9600.md)
14.3), LMX2571 synthesizer ("2571"), V5 board. Size unknown (TODO). TYT's download page offered no MD-9600 firmware on
2026-10-12 (CPS installers only), and no public mirror was found in a short search. Stock files for V1-V4 boards
(other version numbers) are unknown; whether they carry the codec at another address is unknown.

**Board revision and codec.** The codec is board-independent code: an image built for a V1, V2 or V4 radio can host the
V26.45 codec as long as the windows land at the same flash addresses. The file is only a container for the codec;
ODR never runs the rest of it. So one donor serves all revisions (?: no reason found why not; the old tree did exactly
this for every STM32 radio).

## Container and encryption

TYT "OutSecurityBin" container, payload XORed with the MD-9600 bootloader key (1024 bytes, SHA-256
`c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae`, first bytes A2 FA BB 4B;
[hardware-dm1701.md](hardware-dm1701.md) 15.3).

Known from the old loader's extraction (D): the codec carve sits at **file offset 0xC2C7C**, length **0x48BB0**, and
decrypts with key index (j + 0x37C) mod 1024 for carve byte j. Derived (ours): with the payload at 0x100 the cipher
index of file offset F is (F - 0x100) mod 1024 = 0x37C for F = 0xC2C7C, which matches. The carve's absolute table
references (below) point into the carve when it is placed at 0x0807537C, so the application segment is linked at
0x0800C000 and starts at file offset 0xC2C7C - 0x6937C = **0x59900**: a first (resource) segment of 0x59800 bytes
precedes it, as the DM-1701 files have a 0x5D400 one. So for an application address A: file offset = A - 0x0800C000 +
0x59900, key index = (A - 0x0800C000) mod 1024. TODO: confirm the segment table at 0x7C / 0x80 on the real file.

The key has not been derived from the file (no file); `stockfw.derive_key` should work as for the DM-1701 (plain
column vote) or the MD-380 (byte-pair refinement) (?).

## The carve (old builds)

| item | value |
|---|---|
| flash | 0x0807537C..0x080BDF2C (stock address = the old builds' address) |
| length | 0x48BB0 |
| sha256 (decrypted) | `bc9d90030905545cde3af066b3550e7e0446ceb36419d9750208cc61909c27dd` (checked 2026-10-12 against our copy) |
| first word | 0x4FF0E92D (`push.w {r4-r11, lr}`) |

From [codec-dm1701.md](codec-dm1701.md) ("Comparison with the MD-9600 V26.45 codec"): the vocoder code is the first
0xFD6E bytes (0x0807537C..0x080850EA) and its tables sit at +0x36640..+0x4766F (0x080AB9BC..0x080BC9EB); everything
between is other MD-9600 stock code. It is the same object code as the DM-1701 V02.06 codec, relinked: 136 of 149
reachable functions match one to one by mnemonic sequence, 98.2 % of the bytes are equal.

The old builds ran the whole carve at 0x0807537C and had DMR voice on air on DM-1701, MD-UV380 and MD-9600 radios, so
every flash reference the codec makes lies inside the carve (else it would have read the old application's own code).
That is the strongest evidence today that **no small table windows outside the carve are needed** (unlike the MD-380,
[codec-md380.md](codec-md380.md), which needed four).

## Entry points

Matched 2026-10-12 by searching the DM-1701 V02.06 function heads (first 12-24 bytes, before any BL) in the carve
([codec-dm1701.md](codec-dm1701.md) offsets from 0x08074714):

| call | MD-9600 offset | address | DM-1701 offset | match |
|---|---|---|---|---|
| encode | +0x130 | 0x080754AC | +0x130 | 24 bytes, unique |
| encoder set | **TODO** (+0x1C0 expected) | | +0x1C0 | 12-byte head equals the decoder set's; verify by alignment |
| encoder init | +0x284 | 0x08075600 | +0x284 | 24 bytes, unique |
| FEC init | +0x4CC | 0x08075848 | +0x4CC | 12 bytes, unique |
| encode ECC | +0x4E8 | 0x08075864 | +0x4E8 | 24 bytes, unique |
| decode | +0x5D8 | 0x08075954 | +0x5D8 | 24 bytes, unique |
| decoder set | +0x688 | 0x08075A04 | +0x688 | 12 bytes |
| decoder init | +0x6D0 | 0x08075A4C | +0x6D0 | 12 bytes, unique |
| encoder parameters | +0x848 | 0x08075BC4 | +0x848 | 24 bytes, unique |
| FEC decode | **+0xB64** | 0x08075EE0 | +0xB54 | 24 bytes, unique; moved by +0x10 |

The encode, encode ECC and decode addresses agree with the old builds' entry points ([hardware-dm1701.md](hardware-dm1701.md)
1.4: 0x080754AC, 0x08075864, 0x08075954), which ran on air. The head of the library is laid out as the DM-1701's up to
at least +0x848, so the DM-1701's init sequence and ABI are expected to apply unchanged (?, TODO: prove by execution).

## ABI and init (expected: the DM-1701's)

As in [codec-dm1701.md](codec-dm1701.md) "Codec state init and calls" and [codec-md380.md](codec-md380.md): AAPCS,
Thumb, bits one per halfword, two calls of 80 samples per 20 ms frame; init = encoder init (E, 1, 1), FEC init
(E+0x16E4, 1), decoder init (D, 1), FEC init (D+0x7F8, 1), encoder parameters (E, -22, 20, -10, -40, 0x4000, 3276,
3276, 1310, 10, 20), encoder set (E, 128, -20), decoder set (D, 128, -20), one decode of a zero frame with flags 8.
State sizes on the DM-1701 and MD-380: D 0x7EC, E 0x16E0, FEC 0x44 each, stack about 4 KB. **This matches ODR's
DM-1701 vocoder call interface** (`platform/stm32f4/codec_call.c`, the DM-1701 / MD-380 vocoder glue): only the
addresses and the window table change. Not yet verified for this image (TODO).

The stock wrapper (call sites, RAM blocks) can't be examined without the full image (TODO).

## Hosting (proposal)

Run in place at the stock addresses, as on the DM-1701 and MD-380:

* Code window 0x0807537C+0xFD6E (sector 7 starts at 0x08060000, so the window sits in sector 7, 0x08060000..0x0807FFFF,
  and sector 8). Table window 0x080AB9BC..0x080BC9EB (sector 9 0x080A0000..0x080BFFFF). TODO: cut the exact windows by
  literal scan and execution coverage, as for the MD-380.
* The application then gets 0x0800C000..0x0807537B (about 421 KB, sectors 3-6 whole plus 0x08060000..0x0807537B of
  sector 7). Because sector 7 holds both the application's end and the codec, "keep codec" across updates needs the
  application to end at 0x0805FFFF (336 KB) or every flash re-merges the codec, the MD-380 trade-off. The space between
  the windows (0x080850EA..0x080AB9BB) and above 0x080BC9EC is free for read-only data if the linker splits it.
* Boot check: first word 0x4FF0E92D and CRC-32 per window; profile id proposal `md9600-v2645`.

## Extraction recipe (V26.45)

1. Check the file's SHA-256 (`d8a65330...`) and the segment table (TODO: values).
2. Key from a key file checked by SHA-256 (`c3d665b2...`) or derived from the file.
3. Decrypt each window: file offset = A - 0x0800C000 + 0x59900, key index (A - 0x0800C000) mod 1024. Check the code
   window's first word and each window's hash (TODO: per-window hashes; the whole-carve hash is above).
4. Write them unmodified at their stock addresses after the application image. On the user's machine only.

## TODO (left when this pass stopped)

* Get the stock file (a user's download or TYT / Retevis support), record size and segment table, derive the key.
* Settle the encoder set offset; emulate init + 300 frames with only the windows mapped (the MD-380 harness) and compare
  air bits and PCM with the DM-1701 V02.06 codec; measure state sizes and stack.
* Cut minimal windows (literal scan + coverage) and give their SHA-256 / CRC-32.
* Look for other MD-9600 stock versions (V1-V3 boards) and whether their codec sits elsewhere.

## Only a radio can check

Audio level and quality through the MD-9600 path, timing at 72 MHz, and that the loader accepts the merged sectors.
