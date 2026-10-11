<!-- SPDX-License-Identifier: MIT -->
# DM-1701: hosting the stock AMBE+2 codec

Status: research, 2026-10-09. Static analysis only: decryption, Thumb disassembly with arm-none-eabi-objdump,
function matching against the codec the old builds used, and literal cross references. Nothing has run on a radio.

Rule: the radio runs the codec that its own Baofeng stock firmware shipped. The user's machine extracts it from
a stock file the user supplies. No codec bytes go in this repository or in released binaries. The old builds took
the codec from the TYT MD-9600 V26.45 firmware. That source is no longer used.

## Stock files

Primary source: BG7QVB's archive, github.com/daniel5600/BAOFENG-DM1701-Original-Firmware. It holds only
V02.06, in a CSV and a noCSV variant, plus the TYT/Baofeng upgrade tool installer.

The official V02.01 to V02.03 files are still downloadable from Radioddity's bucket `dm-1701.s3.amazonaws.com`
(the zips named "DM-1701 Programming Software V1.0x & Firmware V02.0x"). No V02.04, V02.05 or V02.07 was found.

| version | file | size | sha256 | codec start |
|---|---|---|---|---|
| **V02.06 CSV (primary)** | `DM-1701(CSV)-V02.06.bin` | 1272320 | `0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d` | 0x08074714 |
| V02.06 noCSV | `DM-1701(noCSV)-V02.06.bin` | 1283584 | `4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9` | 0x080770F0 |
| V02.03 CSV | `DM-1701(CSV)-V02.03 Only for Radioddity.bin` | 1272320 | `35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948` | 0x080746D4 |
| V02.03 without CSV | `DM-1701-V02.03 Only for Radioddity.bin` | 1283584 | `0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a` | 0x080770B0 |
| V02.02 CSV | `DM-1701(CSV)-V02.02.bin` | 1272320 | `643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7` | 0x080746D4 |
| V02.01 CSV | `Firmware-DM-1701(CSV)-V02.01.bin` | 1272320 | `cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713` | 0x080746D4 |

V02.01, V02.02 and V02.03 CSV have byte-identical codec code and table windows.

## Container and encryption

The files are TYT "OutSecurityBin" upgrade files:

| offset | contents |
|---|---|
| 0x00 | `OutSecurityBin` |
| 0x10 | model `DM1701` (blank in the V02.06 files) |
| 0x7C | u32 segment count (2) |
| 0x80 | segments as (u32 address, u32 length) |
| 0x100 | payload |

The two segments are:

1. (0x00060000, 0x5D400): resources for the external SPI flash, such as fonts.
2. (0x0800C000, 0xD9400 for CSV or 0xDC000 for noCSV): the application in internal flash.

The payload is XORed with the 1024-byte `DM1701_ENCODE_CIPHER` from the MIT-licensed STM32 firmware loader of the aftermarket firmware
script:

```
plain[i] = file[0x100 + i] ^ DM1701_ENCODE_CIPHER[i % 1024]
```

The application starts at payload offset 0x5D400 (file offset 0x5D500) and is linked at 0x0800C000. The V02.06
CSV vector table holds SP 0x20003618 and reset 0x080E120D. For any app address A,
file offset = A - 0x0800C000 + 0x5D500, and the cipher index = (file offset - 0x100) % 1024.

## Codec in the stock image (V02.06 CSV)

| item | value |
|---|---|
| code window | flash 0x08074714..0x08084930, file offset 0xC5C14, length 0x1021C, cipher phase 0x314 |
| code sha256 (decrypted) | `1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd` |
| first word | 0x4FF0E92D (`push.w {r4-r11, lr}`) |
| encode | 0x08074844 (+0x130) |
| encode ECC | 0x08074BFC (+0x4E8) |
| decode | 0x08074CEC (+0x5D8) |
| tables referenced | 0x080D19F0..0x080E3A6B (46 distinct literal targets) |
| table window (estimate) | flash 0x080D19F0..0x080E4240, file offset 0x122EF0, length 0x12850, cipher phase 0x1F0, sha256 `b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894` |

The V02.01, V02.02 and V02.03 CSV values are the same, minus 0x40. Their code window is 0x080746D4
(file 0xC5BD4, sha256 `2ff7b7a8...a86a4cc9`) and their table window is 0x080D19B0 (file 0x122EB0, sha256
`ab3cfb49...54f1379b`). V02.06 is not only V02.03 shifted. 119 halfwords of the code window differ. Those are the
table literals (+0x40) and calls from stock wrapper code inside the window.

### Comparison with the MD-9600 V26.45 codec (old builds, 0x0807537C)

* It is the same object code, relinked, not byte-identical. 149 functions are reachable from the 3 entries.
  136 of them map one-to-one to stock functions with the same mnemonic sequence; the other 13 are short and match
  2 or 3 places. Byte for byte, 98.2% match (41382 of 42136 bytes). Function order differs between the two images.
* The entry offsets from the code start (+0x130, +0x4E8, +0x5D8) and the prologues are the same. The
  register and stack layout in `codec_interface.c` therefore applies unchanged. Only the addresses change.
* The old 0x48BB0-byte "codec" carve from the MD-9600 file is mostly something else. The vocoder code is the first
  0xFD6E bytes and its tables sit at +0x36640..+0x4766F. In between is MD-9600 code that the DM-1701 image
  doesn't contain.

### Position dependence

* Calls and branches inside the codec are PC-relative. No code-address literals were found, and no calls from the
  reachable functions to stock code. Stock wrapper and unreachable code inside the window does call out
  (0x08040A30 and others), but the codec never runs it.
* The code reaches its tables through 46 distinct absolute literals at 63 sites. The code is therefore tied to the
  stock table address. Moving the code block and the table block as two contiguous blocks requires patching those
  63 words.
* Some aligned words fall in the RAM range, but they look like Thumb-2 instruction pairs. The old builds ran this
  object code with the aftermarket firmware's own RAM layout, so no fixed RAM addresses are expected. This is unverified.

## C. Codec state init tables

None of the STM32 `codec.c` arrays (decode 0x7EC, encode 0x2000, ECC 0x100) occur in the decrypted stock image or
in the MD-9600 carve. None of their non-zero 16-byte chunks match. They are run-time state snapshots, not stock
data. The DM-32 stock firmware builds this state with the library's own init functions. On the DM-1701, the
equivalent init functions are in the code window. Stock V02.06 calls them from its wrapper at 0x08068D4C. The
sequence, the state sizes and the call arguments are in "Codec state init and calls (stock V02.06)" below. The
`codec.c` tables are no longer needed.

## Extraction recipe (V02.06 CSV)

1. Check the file's sha256 (`0314efd4...`) and the header (segment table as above).
2. Decrypt file[0xC5C14 .. +0x1021C] with cipher index (offset - 0x100) % 1024. Expect the first word to be
   0x4FF0E92D and the sha256 to be `1d8d0460...`.
3. Decrypt the table window file[0x122EF0 .. +0x12850] the same way. Expect sha256 `b99f14e7...`.
4. Write both, unmodified, to flash 0x08074714 and 0x080D19F0 in the image (no relocation). The merge happens on
   the user's machine (loader or `make codec`), never in a release.

## Recommended hosting

Run in place from flash at the stock addresses, as stock does. The STM32 executes from internal flash, so there is
no copy and no patching.

* The linker script for the V02.06 CSV build keeps the application out of every flash sector the windows touch.

  | region | sectors | range |
  |---|---|---|
  | code window | 7, 8 | 0x08060000..0x0809FFFF |
  | table window | 10, 11 | 0x080C0000..0x080FFFFF |
  | application | 3-6 | 0x0800C000..0x0805FFFF (336 KB) |
  | free | 9 | 0x080A0000..0x080BFFFF (128 KB) |

  A firmware update then erases and writes only sectors 3-6 and leaves the
  codec in place (hardware-dm1701.md 1.4, docs/dev/release.md "Codec handling"). Reserving only the windows would leave
  about 426 KB below the code window, but every update would have to erase and rewrite the codec.
* Call the entries with `BL`/`BLX` to the stock addresses above.
* Support one version per build, keyed by sha256. The noCSV variants and older versions sit at other addresses and
  would need their own windows. Patching the 63 literals is the fallback if a single layout is required.

## Open items

* Exact end of the last table. The window end 0x080E4240 is a conservative estimate. Both windows also include
  some stock non-codec bytes, which are never executed.
* ~~Locating the stock codec init functions and their parameters~~: done, see below.
* A hardware test: TX/RX audio with the stock V02.06 codec at its stock addresses.

## Codec state init and calls (stock V02.06)

Status: 2026-10-09. Static analysis of the stock V02.06 CSV image (decrypted app `app_0314efd4.bin`, listing
`app0206.asm`), then verified by execution. The decrypted image was run on the host in an instruction set
emulator (Unicorn, Thumb / M-profile). Only the two codec windows, the state blocks, the I/O buffers and a stack
were mapped, so any other access faults. Our own `boards/dm1701/vocoder_dm1701.c` and `platform/stm32f4/codec_call.c`,
cross-compiled with the firmware flags, were run the same way against the stock windows and give bit-identical
results to the reference run. Nothing here has run on a radio yet.

### Stock call sites

All 21 calls into the codec are in the stock voice module at 0x08068D4C..0x0806A160: decode 7 (one in init),
encode 10, encode ECC 4. The stock wrapper keeps an encoder block E at RAM 0x2000A528 and a decoder block D at
0x2000F794. Both also hold stock fields after the codec state (PCM ring, bit buffers, privacy keystream, flags).

### Init (stock function 0x08068D4C)

1. Wrapper fields: E+0x1730 = 0x1840 (first encode flags), E+0x173E = 0x2000, E+0x1744.. = -22, 20, -10, -40, 0x4000,
   3276, 3276, 1310, 10, 20, E+0x1740 = 128, E+0x1742 = -20, D+0x97A = 0, D+0x992 = 128, D+0x994 = -20.
2. Calls (offsets from the codec start 0x08074714):

   | call | stock call | arguments |
   |---|---|---|
   | encoder init | 0x08074998 (+0x284) | (E, 1, 1) |
   | FEC init | 0x08074BE0 (+0x4CC) | (E+0x16E4, 1): the encoder side's FEC state, used by encode ECC |
   | decoder init | 0x08074DE4 (+0x6D0) | (D, 1) |
   | FEC init | 0x08074BE0 (+0x4CC) | (D+0x7F8, 1): the decoder side's FEC state, used by the FEC decode below |
   | encoder parameters | 0x08074F5C (+0x848) | (E, -22, 20, -10, then on the stack -40, 0x4000, 3276, 3276, 1310, 10, 20) |
   | encoder set | 0x080748D4 (+0x1C0) | (E, 128, -20) |
   | decoder set | 0x08074D9C (+0x688) | (D, 128, -20) |
   | decode | 0x08074CEC (+0x5D8) | (D+0xAD6, 80, D+0xB7C, 0, stack 8, 0, D): one decode of an all-zero frame, flags 8 |

   This matches the DM-32 finding (codec-dm32.md section C), with the same parameter values. The DM-1701 build calls
   the 10-parameter encoder function where the DM-32 wrapper stored the values in E.

### State sizes (measured)

Each block was placed so that it ends at an unmapped page. The codec then faults on any access past the end.

| block | size | notes |
|---|---|---|
| decoder state D | 0x7EC (2028) bytes | decode, decoder init and set touch nothing past it (0x7E8 faults) |
| encoder state E | 0x16E0 (5856) bytes | 0x16D0 faults |
| FEC state | 0x44 bytes each | only for the codec's own FEC routines (encode ECC, FEC decode) |
| stack | 3822 bytes peak | measured over init, encode and decode. Allow 4 KB plus the caller. |

The blocks are passed by pointer and can live anywhere (ODR uses CCM). No fixed RAM address is touched. With
everything else unmapped, init, 30 frames of encode and decode and the ECC routine ran without a fault. The code uses
no FPU instructions.

### Per-frame calls (two per 20 ms frame, 80 samples each)

| call | r0 | r1 | r2 | r3 | stack [0], [1], [2], [3] |
|---|---|---|---|---|---|
| encode | 49-bit out buffer (one bit per halfword) | 0 (stock: E+0x173A, zero) | PCM in, this half | 80 | flags, half (0 / 1), 0x2000, E |
| decode | PCM out, this half | 80 | 49-bit in buffer (one bit per halfword) | 0 (stock: D+0x98E, zero) | flags, half (0 / 1), D |
| encode ECC | bit buffer (49 in, 72 out) | same buffer | 0 (stock: E+0x173C, zero) | FEC state | |

* Encode flags are 0x1840 on the first call of a transmission (stock copies E+0x1730) and 0x800 on every later
  call. The encoder reads the PCM and never writes it. Its output is the 49 parameter bits only. Stock applies its
  privacy XOR to them (E+0x189C ^= E+0x19BC, 49 halfwords) and then calls encode ECC. Encode ECC turns them into the
  72 air bits in air order (one per halfword, the MSB of byte 0 first).
* Decode flags: stock unpacks the 72 received bits and runs its own FEC decode. It then XORs the privacy keystream
  onto the 49 bits and passes flags = D+0x97C (0) OR the FEC routine's result. The FEC decode is at 0x08075268
  (+0xB54) with these arguments:

  | r0 | r1 | r2 | r3 | stack |
  |---|---|---|---|---|
  | out | 0 | in | 1 (bits per symbol; it accepts soft bits) | flags, D+0x7F8 |

  The result is 0 for a good frame and 4 when C0 is uncorrectable (4 or more errors). This was measured over 300
  frames with 0-4 injected errors per code word. With flags 4 (or 8) the decoder repeats the previous frame. The two
  decode calls of a frame use the same bit buffer, and the first one rewrites it.
* The codec's 49-bit order is the standard AMBE+2 order (C0 data, C1 data, C2, C3, MSB first). Its ECC output
  matches `core/dmr/ambe/ambeFec.c` on 249 of 249 vectors (random, all zeros, all ones, each single bit). Seven of
  them are in `tests/host/ambeFecTest.c`. The firmware therefore runs encode / decode only and does the air frame FEC
  in core/dmr/ambe.
* The AMBE silence frame the call stack sends (B9 E8 81 52 61 73 00 2A 6B) is a valid frame (0 FEC errors) and decodes
  to silence (peak |sample| 1 after 10 frames) with the stock decoder.
* A 1 kHz tone at -12 dBFS through encode and decode comes back at about 945 zero crossings per second (the first
  frames are the codec's delay). This confirms the init sequence and arguments.

### Hosting in OpenDigitalRadio

* `boards/dm1701/vocoder_dm1701.c`:

  | function | does |
  |---|---|
  | `present()` | checks the first word 0x4FF0E92D and the CRC-32 of both windows (code 0x42CF501D, tables 0x673D0006, computed once) |
  | `reset()` | runs the init sequence above on zeroed CCM blocks |
  | `encode()` / `decode()` | make the two calls per frame |

  Without the codec it reports not present (FM only).
* `platform/stm32f4/codec_call.c` holds a file-scope assembly veneer `stm32_codec_call(fn, args, n)` with its own
  frame. It loads r0-r3 from the array and puts the rest on an 8-byte aligned stack. It restores SP from a saved
  frame register. The call therefore does not depend on how the caller was compiled (gotcha 4.1). To the compiler it
  is an ordinary external call, which orders memory around it (gotcha 4.4). It was verified in the emulator with a
  misaligned caller stack.
* `tools/codec_extract.py` builds the merged image (recipe above) on the user's machine. A run with the real stock
  file and a key file matching hardware-dm1701.md 15.3 reproduces both window hashes and the CRCs above.

### Still open

* On the radio: audio quality, levels (decoded output is lower than the input; the encoder input level the codec
  expects is unknown) and timing (encode + decode per 20 ms on the STM32 at 144 MHz).
* The parameters the stock wrapper passes from its fields are all constants after init. Whether any stock code path
  changes them (E+0x173A, E+0x173C, D+0x98E, D+0x97A) during a call was not traced.
