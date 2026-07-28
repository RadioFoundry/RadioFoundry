<!-- SPDX-License-Identifier: MIT -->
# DM-1701: hosting the stock AMBE+2 codec

Status: research, 2026-10-09. Static analysis only: decryption, Thumb disassembly with arm-none-eabi-objdump,
function matching against the codec the old builds used, and literal cross references. Nothing has run on a radio.

Rule: the radio runs exactly the codec its own Baofeng stock firmware shipped. The user's machine extracts it from
a stock file the user supplies. No codec bytes go in this repository or in released binaries. The old builds took
the codec from the TYT MD-9600 V26.45 firmware; that has to go.

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

The payload is XORed with the 1024-byte `DM1701_ENCODE_CIPHER` from the MIT loader
the legacy STM32 firmware loader script:

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
`ab3cfb49...54f1379b`). V02.06 isn't just V02.03 shifted: 119 halfwords of the code window differ. Those are the
table literals (+0x40) and calls from stock wrapper code inside the window.

### Comparison with the MD-9600 V26.45 codec (old builds, 0x0807537C)

* It's **the same object code, relinked**, not byte-identical. 149 functions are reachable from the 3 entries.
  136 of them map one-to-one to stock functions with the same mnemonic sequence; the other 13 are short and match
  2 or 3 places. Byte for byte, 98.2% match (41382 of 42136 bytes). Function order differs between the two images.
* The entry offsets from the code start (+0x130, +0x4E8, +0x5D8) and the prologues are the same. So the
  register and stack layout in `codec_interface.c` applies unchanged; only the addresses change.
* The old 0x48BB0-byte "codec" carve from the MD-9600 file is mostly something else. The vocoder code is the first
  0xFD6E bytes and its tables sit at +0x36640..+0x4766F. In between is MD-9600 code that the DM-1701 image
  doesn't contain.

### Position dependence

* Calls and branches inside the codec are PC-relative. No code-address literals were found, and no calls from the
  reachable functions to stock code. Stock wrapper and unreachable code inside the window does call out
  (0x08040A30 and others), but the codec never runs it.
* The code reaches its tables through 46 distinct absolute literals at 63 sites. The code is therefore tied to the
  stock table address. If you moved the code block and the table block as two contiguous blocks, you would patch
  those 63 words.
* Some aligned words fall in the RAM range, but they look like Thumb-2 instruction pairs. The old builds ran this
  object code with the legacy firmware's own RAM layout, so no fixed RAM addresses are expected. This is unverified.

## C. Codec state init tables

None of the STM32 `codec.c` arrays (decode 0x7EC, encode 0x2000, ECC 0x100) occur in the decrypted stock image or
in the MD-9600 carve: none of their non-zero 16-byte chunks match. They are run-time state snapshots, not stock
data. The DM-32 stock firmware builds this state with the library's own init functions; on the DM-1701, the
equivalent init functions are probably among the unreachable code in the code window. Finding them and their
parameters is still open. Until then the DM-1701 still needs the `codec.c` tables, which come from our source, not
the stock file.

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

* The linker script for the V02.06 CSV build reserves the two windows. The app gets 0x0800C000..0x08074714
  (about 426 KB), 0x08084930..0x080D19F0 (about 309 KB) and 0x080E4240..0x08100000 (about 111 KB).
* Call the entries with `BL`/`BLX` to the stock addresses above.
* Support one version per build, keyed by sha256. The noCSV variants and older versions sit at other addresses and
  would need their own windows; patching the 63 literals is the fallback if a single layout is required.

## Open items

* Exact end of the last table: the window end 0x080E4240 is a conservative estimate. Both windows also include
  some stock non-codec bytes, which are never executed.
* Locating the stock codec init functions and their parameters, to drop the `codec.c` tables.
* A hardware test: TX/RX audio with the stock V02.06 codec at its stock addresses.
