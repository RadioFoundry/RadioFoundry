<!-- SPDX-License-Identifier: MIT -->
# DM-1701: hosting the AMBE+2 codec

The codec has run on a radio at the address the merge places it (2026-10-19, "Open items").

Rule: the radio runs the codec that its own Baofeng firmware shipped. The user's machine extracts it from the firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

## Firmware files

The archive github.com/daniel5600/BAOFENG-DM1701-Original-Firmware holds V02.06, in a CSV and a noCSV variant, plus the
manufacturer's upgrade tool installer.

The V02.01 to V02.03 files are downloadable from Radioddity's bucket `dm-1701.s3.amazonaws.com` (the zips named
"DM-1701 Programming Software V1.0x & Firmware V02.0x"). No V02.04, V02.05 or V02.07 file is known.

| version | file | size | sha256 | codec start | codec source |
|---|---|---|---|---|---|
| **V02.06 CSV (primary)** | `DM-1701(CSV)-V02.06.bin` | 1272320 | `0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d` | 0x08074714 | yes, map `dm1701-v0206-csv` |
| V02.06 noCSV | `DM-1701(noCSV)-V02.06.bin` | 1283584 | `4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9` | 0x080770F0 | yes, map `dm1701-v0206-nocsv` |
| V02.03 CSV | `DM-1701(CSV)-V02.03 Only for Radioddity.bin` | 1272320 | `35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948` | 0x080746D4 | yes, map `dm1701-v0203-csv` |
| V02.03 noCSV | `DM-1701-V02.03 Only for Radioddity.bin` | 1283584 | `0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a` | 0x080770B0 | yes, map `dm1701-v0203-nocsv` |
| V02.02 CSV | `DM-1701(CSV)-V02.02.bin` | 1272320 | `643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7` | 0x080746D4 | yes, map `dm1701-v0203-csv` |
| V02.01 CSV | `Firmware-DM-1701(CSV)-V02.01.bin` | 1272320 | `cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713` | 0x080746D4 | yes, map `dm1701-v0203-csv` |

V02.01, V02.02 and V02.03 CSV have byte-identical codec code and table windows, so one codec map serves all three
(`tools/rfoundry/rfoundry/codec_maps/dm1701/dm1701-v0203-csv.json`, found by the code window's SHA-256). The noCSV
files keep the codec at other addresses, each with its own map (`dm1701-v0206-nocsv.json`, `dm1701-v0203-nocsv.json`).
These six files are every DM-1701 stock build known (2026-10-25); all six are codec sources.

### Files without the codec

rfoundry and the web flasher name these by SHA-256 (`stockfw.py` `NO_CODEC_FILES`) and refuse them as a codec source,
a key source or original firmware, as for any file that is not the manufacturer's.

| file | size | sha256 | codec |
|---|---|---|---|
| an open-firmware build | 804256 | `a09ab4d364d199853204ebdf068ba0886d7e7bb99689f08437aae52104474a7e` | none |

* A plain application image (no container) linked at 0x0800C000, ending at 0x080D05A0. It may come zipped with small
  `.gla` language files, which the opener sets aside.
* It holds neither part of the codec: no 64-byte block of the V02.06 CSV code or table window is in it. The file ends
  below the stock table window (0x080D19F0).
* It expects the code at 0x0807537C, which it leaves blank (0xFF from there to 0x080BDF2C). It checks the codec's first
  word there and calls encode (+0x130) and decode (+0x5D8), the V02.06 CSV entry offsets. What puts the codec there is
  outside the file.

## Container and encryption

The files are "OutSecurityBin" upgrade files:

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

The payload is XORed with the model's 1024-byte key ([hardware-dm1701.md](hardware-dm1701.md) 15.3):

```
plain[i] = file[0x100 + i] ^ key[i % 1024]
```

The application starts at payload offset 0x5D400 (file offset 0x5D500) and is linked at 0x0800C000. The V02.06
CSV vector table holds SP 0x20003618 and reset 0x080E120D. For any app address A,
file offset = A - 0x0800C000 + 0x5D500, and the cipher index = (file offset - 0x100) % 1024.

## Codec in the V02.06 CSV file

All addresses in this section are where the file holds the codec. The merge moves the code
([codec-placement.md](codec-placement.md), "STM32 radios"; "Hosting" below).

| item | value |
|---|---|
| code window | flash 0x08074714..0x08084930, file offset 0xC5C14, length 0x1021C, cipher phase 0x314 |
| code sha256 (decrypted) | `1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd` |
| first word | 0x4FF0E92D (`push.w {r4-r11, lr}`) |
| encode | 0x08074844 (+0x130) |
| encode ECC | 0x08074BFC (+0x4E8) |
| decode | 0x08074CEC (+0x5D8) |
| tables referenced | 0x080D19F0..0x080E3A6B (46 distinct literal targets) |
| table window (as carved) | flash 0x080D19F0..0x080E4240, file offset 0x122EF0, length 0x12850, cipher phase 0x1F0, sha256 `b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894` |
| table window (codec data only) | flash 0x080D19F0..0x080E3A72, length 0x12082: the codec map's minimal window, from the first data object to the end of the last |

The V02.01, V02.02 and V02.03 CSV values are the same, minus 0x40. Their code window is 0x080746D4
(file 0xC5BD4, sha256 `2ff7b7a8...a86a4cc9`) and their table window is 0x080D19B0 (file 0x122EB0, sha256
`ab3cfb49...54f1379b`). V02.06 is not only V02.03 shifted. 119 halfwords of the code window differ. Those are the
table literals (+0x40) and calls from non-codec code inside the window.

The codec maps of the two layouts agree site for site: the same entry offsets, the same 1 self reference, 2 island
pointers, 70 table references, 4 island references, 2 islands, 12 island calls and 52 data objects at the same offsets,
every address 0x40 lower in V02.01 to V02.03. Three objects' trimmed lengths differ, because their bound is the next
address that stock code outside the codec loads; the minimal table window (0x12082 bytes) is the same.

The noCSV files have a larger application segment (0xDC000) and the codec higher up:

| file | code window (address, length, sha256) | table window (address, length 0x12850, sha256) | codec data |
|---|---|---|---|
| V02.06 noCSV | 0x080770F0, 0x10200, `c34e4588...0b870566` | 0x080D4794, `de6e8494...d1683acf` | 0x080D4794..0x080E6826 (0x12092) |
| V02.03 noCSV | 0x080770B0, 0x10200, `4fccfdda...f756a3ce` | 0x080D4754, `b5a73a19...425f537a` | 0x080D4754..0x080E67E6 (0x12092) |

The noCSV code is the CSV code of its version with 0x1C bytes fewer at offset 0x4A00, past every entry point. The
entry offsets are the same. Inside the table window, other data between the codec's objects is sized differently, so
the codec's objects sit at +0x2DA4, +0x2E20 and +0x2DB4 from the CSV file's, and the codec data is 0x10 bytes longer.
The noCSV maps agree with the CSV maps site for site: every reference in the code points to the same object at the
same offset, and every island call to the same code offset. V02.03 noCSV is V02.06 noCSV 0x40 lower, the same
relation as between the CSV files.

### Position dependence

* Calls and branches inside the codec are PC-relative. The three entry points make no calls outside the window.
  Other code inside the window does call out, but the codec never runs it.
* The code reaches its tables through 46 distinct absolute literals at 63 sites. The code is therefore tied to the
  table address. Moving the code block and the table block as two contiguous blocks requires patching those 63 words.
* The codec touches no fixed RAM address (unverified for every path).

## Extraction recipe (V02.06 CSV)

1. Check the file's sha256 (`0314efd4...`) and the header (segment table as above).
2. Decrypt file[0xC5C14 .. +0x1021C] with cipher index (offset - 0x100) % 1024. Expect the first word to be
   0x4FF0E92D and the sha256 to be `1d8d0460...`.
3. Decrypt the table window file[0x122EF0 .. +0x12850] the same way. Expect sha256 `b99f14e7...`.
4. Place them as [codec-placement.md](codec-placement.md) "STM32 radios" says: the tables, cut to 0x12082 bytes,
   stay at 0x080D19F0; the code moves to 0x080E3A74 (V02.01 to V02.03: every address 0x40 lower, map
   `dm1701-v0203-csv.json`; noCSV: the table below "Hosting", maps `dm1701-v0206-nocsv.json` and
   `dm1701-v0203-nocsv.json`). Rewrite the one literal in the code that points into the code
   and the 12 calls (BL) from the islands in the table window back into the code (the codec map
   `tools/rfoundry/rfoundry/codec_maps/dm1701/dm1701-v0206-csv.json`, `self_refs` and `branches`).
5. Write the descriptor at 0x080FFF80 (`platform/stm32f4/codec_desc.h`): code and table window with their CRC-32s.
   The merge happens on the user's machine (rfoundry, `tools/codec_extract.py`, the web flasher, or `make` with the
   firmware file in `codecs/`), never in a release.

## Hosting

The code runs in place from internal flash, at the address the merge moves it to. The tables stay at their original
address, so the code's 63 table literals stay as they are.

* `boards/dm1701/memory.ld` keeps the application out of every flash sector the codec uses:

  | region | sectors | V02.06 CSV | V02.01, V02.02, V02.03 CSV | V02.06 noCSV | V02.03 noCSV |
  |---|---|---|---|---|---|
  | application | 3-9 | 0x0800C000..0x080BFFFF (720 KB) | the same | the same | the same |
  | table window | 10, 11 | 0x080D19F0..0x080E3A72 | 0x080D19B0..0x080E3A32 | 0x080D4794..0x080E6826 | 0x080D4754..0x080E67E6 |
  | code window | 11 | 0x080E3A74..0x080F3C90 | 0x080E3A34..0x080F3C50 | 0x080E6828..0x080F6A28 | 0x080E67E8..0x080F69E8 |
  | descriptor | 11 | 0x080FFF80, 128 bytes | the same | the same | the same |

  The board's codec windows are the union of the layouts; a merge writes the layout of the file it merges.

  A firmware update then erases and writes only sectors 3-9 and leaves the codec in place (hardware-dm1701.md 1.4,
  docs/dev/release.md "Codec handling").
* The firmware takes the code's address from the descriptor and calls the entries at their offsets from it.
* Before the move, the original addresses kept sectors 7, 8, 10 and 11 (512 KB) for about 140 KB of codec, and the application
  got sectors 3-6 (336 KB) with sector 9 apart.
* Each supported file is keyed by its sha256 (`tools/rfoundry/rfoundry/stockfw.py` `PROFILES`). One codec id for
  every file, `dm1701-v0206-csv-s11`; the descriptor's tag tells the layouts apart, and
  `boards/dm1701/vocoder_dm1701.c` pins each:

  | files | tag | tables (address, length, CRC-32) | code (address, length, CRC-32) |
  |---|---|---|---|
  | V02.06 CSV | 0x0206 | 0x080D19F0, 0x12082, 0xC1EEFA8B | 0x080E3A74, 0x1021C, 0x485BFC13 |
  | V02.01, V02.02, V02.03 CSV | 0x0203 | 0x080D19B0, 0x12082, 0x811EC3C0 | 0x080E3A34, 0x1021C, 0x99B02204 |
  | V02.06 noCSV | 0x10206 | 0x080D4794, 0x12092, 0x3EDE91FF | 0x080E6828, 0x10200, 0xAF82F7D0 |
  | V02.03 noCSV | 0x10203 | 0x080D4754, 0x12092, 0x217E610A | 0x080E67E8, 0x10200, 0x96264D90 |

  The three older files put the same bytes at the same addresses, so they share one tag. A release accepts only the
  tags it was built with: a release from before 0x0203 finds no codec in a radio merged from V02.01 to V02.03 and runs
  FM only until the codec is merged again. The same holds for the noCSV tags (bit 16 set) and releases from before
  them.

## Open items

* Done: the codec map's minimal table window ends at 0x080E3A72, and the merge writes only that much. The code window
  still includes some non-codec bytes, which are never executed.
* Done 2026-10-19 with the code moved to 0x080E3A74: the debug link reported `dm1701-v0206-csv-s11`, on-air speech RX
  decoded 234 of 234 frames, and the SDR bench modem decoded the radio's voice TX.
* V02.01 to V02.03 CSV (2026-10-25): checked in a host emulator only, not yet on the radio. With the code window and
  the minimal table window only, and with the codec as the merge places it (code at 0x080E3A34, the 12 calls
  re-encoded), a 300 frame stress run gives air bits and decoded PCM bit-identical to the V02.06 codec's, and the FEC
  decode results agree. Each real file decrypts, places and matches the profile's hashes in rfoundry and
  `tools/codec_extract.py`.
* V02.06 and V02.03 noCSV (2026-10-25): checked the same way, in a host emulator only, not yet on the radio. Placed
  (code at 0x080E6828 and 0x080E67E8), the 300 frame stress run gives air bits and decoded PCM bit-identical to the
  V02.06 CSV codec's; each real file decrypts, places and matches its profile's hashes.

## Codec state init and calls

The sequence below is what `boards/dm1701/vocoder_dm1701.c` and `platform/stm32f4/codec_call.c` run. It is checked on a
host against the codec windows, and has run on a radio ("Open items").

### Init

E is the encoder state block, D the decoder state block.

1. Set E+0x1730 = 0x1840 (first encode flags), E+0x173E = 0x2000, E+0x1744.. = -22, 20, -10, -40, 0x4000, 3276, 3276,
   1310, 10, 20, E+0x1740 = 128, E+0x1742 = -20, D+0x97A = 0, D+0x992 = 128, D+0x994 = -20.
2. Calls (offsets from the codec start, 0x08074714 in the V02.06 CSV file):

   | call | address | arguments |
   |---|---|---|
   | encoder init | 0x08074998 (+0x284) | (E, 1, 1) |
   | FEC init | 0x08074BE0 (+0x4CC) | (E+0x16E4, 1): the encoder side's FEC state, used by encode ECC |
   | decoder init | 0x08074DE4 (+0x6D0) | (D, 1) |
   | FEC init | 0x08074BE0 (+0x4CC) | (D+0x7F8, 1): the decoder side's FEC state, used by the FEC decode below |
   | encoder parameters | 0x08074F5C (+0x848) | (E, -22, 20, -10, then on the stack -40, 0x4000, 3276, 3276, 1310, 10, 20) |
   | encoder set | 0x080748D4 (+0x1C0) | (E, 128, -20) |
   | decoder set | 0x08074D9C (+0x688) | (D, 128, -20) |
   | decode | 0x08074CEC (+0x5D8) | (D+0xAD6, 80, D+0xB7C, 0, stack 8, 0, D): one decode of an all-zero frame, flags 8 |

   The DM-32UV codec takes the same parameter values ([codec-dm32.md](codec-dm32.md) section C).

### State sizes (measured)

Each block was placed so that it ends at an unmapped page. The codec then faults on any access past the end.

| block | size | notes |
|---|---|---|
| decoder state D | 0x7EC (2028) bytes | decode, decoder init and set touch nothing past it (0x7E8 faults) |
| encoder state E | 0x16E0 (5856) bytes | 0x16D0 faults |
| FEC state | 0x44 bytes each | only for the codec's own FEC routines (encode ECC, FEC decode) |
| stack | 3822 bytes peak | measured over init, encode and decode. Allow 4 KB plus the caller. |

The blocks are passed by pointer and can live anywhere (RadioFoundry uses CCM). No fixed RAM address is touched. With
everything else unmapped, init, 30 frames of encode and decode and the ECC routine ran without a fault. The code uses
no FPU instructions.

### Per-frame calls (two per 20 ms frame, 80 samples each)

| call | r0 | r1 | r2 | r3 | stack [0], [1], [2], [3] |
|---|---|---|---|---|---|
| encode | 49-bit out buffer (one bit per halfword) | 0 (E+0x173A, zero) | PCM in, this half | 80 | flags, half (0 / 1), 0x2000, E |
| decode | PCM out, this half | 80 | 49-bit in buffer (one bit per halfword) | 0 (D+0x98E, zero) | flags, half (0 / 1), D |
| encode ECC | bit buffer (49 in, 72 out) | same buffer | 0 (E+0x173C, zero) | FEC state | |

* Encode flags: a set of feature bits, one value on every call of a transmission (the init value at E+0x1730
  is 0x1840). The bit meanings were measured on the DM-1801 Pro and MD-380 builds of this codec family
  ([codec-md380.md](codec-md380.md), "Encode flags";
  [codec-dm1801pro.md](codec-dm1801pro.md) section 3): 0x40 noise suppression, 0x800 noise gate, 0x1000 tone frames,
  0x2000 input AGC.
  **On this build they are unverified**: the code is the same family, but the bits were not measured on it. RadioFoundry's
  host passes the same word as the MD-380's (`drivers/vocoder/ambe2_flags.h`, 0x1840 by default). The encoder reads
  the PCM and never writes it. Its output is the 49 parameter bits only. A privacy XOR
  (E+0x189C ^= E+0x19BC, 49 halfwords) is applied to them before encode ECC. Encode ECC turns them into the
  72 air bits in air order (one per halfword, the MSB of byte 0 first).
* Decode flags: unpack the 72 received bits and run the codec's FEC decode. XOR the privacy keystream
  onto the 49 bits and pass flags = D+0x97C (0) OR the FEC routine's result. The FEC decode is at 0x08075268
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
  to silence (peak |sample| 1 after 10 frames) with the decoder.
* A 1 kHz tone at -12 dBFS through encode and decode comes back at about 945 zero crossings per second (the first
  frames are the codec's delay). This confirms the init sequence and arguments.

### Hosting in RadioFoundry

* `platform/stm32f4/hrc/vocoder.c` with this build's profile in `boards/dm1701/vocoder_dm1701.c`:

  | function | does |
  |---|---|
  | `present()` | checks the descriptor, that the code and tables are where one of the profiles in "Hosting" puts them, with its placed CRC-32s, the first word 0x4FF0E92D and the CRC of both windows (computed once) |
  | `reset()` | runs the init sequence above on zeroed CCM blocks |
  | `encode()` / `decode()` | make the two calls per frame |

  Without the codec it reports not present (FM only).
* `platform/stm32f4/codec_call.c` holds a file-scope assembly veneer `stm32_codec_call(fn, args, n)` with its own
  frame. It loads r0-r3 from the array and puts the rest on an 8-byte aligned stack. It restores SP from a saved
  frame register. The call therefore does not depend on how the caller was compiled (gotcha 4.1). To the compiler it
  is an ordinary external call, which orders memory around it (gotcha 4.4). It was verified on a host with a
  misaligned caller stack.
* `tools/codec_extract.py` builds the merged image (recipe above) on the user's machine. A run with the real
  firmware file and a key file matching hardware-dm1701.md 15.3 reproduces both window hashes and the placed CRCs above.

### Still open

* On the radio: audio quality, levels (decoded output is lower than the input; the encoder input level the codec
  expects is unknown) and timing (encode + decode per 20 ms on the STM32 at 144 MHz).
* The encode flags bits on this build (see "Per-frame calls"): measure 0x40, 0x800, 0x1000 and 0x2000 as on the
  MD-380 build, or confirm on air.
* The init fields are constants after init. Whether anything changes them (E+0x173A, E+0x173C, D+0x98E, D+0x97A)
  during a call is unverified.
