<!-- SPDX-License-Identifier: MIT -->
# MD-9600: hosting the AMBE+2 codec

Status: format description for eight TYT images (V06.09 and V16.45, four variants each) and V26.45. The codecs were run
in a host instruction set emulator (measured); nothing has run on a radio.

Rule: the radio runs exactly the codec its own TYT firmware shipped. The user's machine extracts it from a firmware
file the user supplies. No codec bytes go in this repository or in released binaries. Any MD-9600 firmware version is the
MD-9600's own donor; the codec is board-independent code, so a file for one board revision serves every revision.

These files are also a codec source for another radio: an open-firmware install made from this firmware leaves this
codec on that radio. Its tables then move with the code (`codecmap.move_window`), and the open-firmware backup finds it
wherever the install put it (`stockfw.from_moved`). The radio's own codec page lists the files and placements;
[codec-placement.md](codec-placement.md) has the shared format.

## Stock files

| version | boards | file | size | sha256 |
|---|---|---|---|---|
| **06.09 CSV (supported)** | V1-V3 | `MD9600-CSV-V06.09.bin` | 1094144 | `79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09` |
| 06.09 CSV + GPS | V1-V3 | `MD9600-CSV-GPS-P06.09.bin` | 1100288 | `9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5` |
| 06.09 REC | V1-V3 | `MD9600-REC-D06.09.bin` | 1103360 | `7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f` |
| 06.09 GPS + REC | V1-V3 | `MD9600-GPS-REC-S06.09.bin` | 1109504 | `4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7` |
| **16.45 CSV (supported)** | V4 ("PLL1590") | `MD9600-CSV(V4)-V16.45.bin` | 1099264 | `de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532` |
| 16.45 CSV + GPS | V4 | `MD9600-CSV-GPS(V4)-P16.45.bin` | 1105408 | `7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f` |
| 16.45 REC | V4 | `MD9600-REC(V4)-D16.45.bin` | 1108480 | `d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c` |
| 16.45 GPS + REC | V4 | `MD9600-GPS-REC(V4)-S16.45.bin` | 1114624 | `3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff` |
| 26.45 CSV | V5 ("2571") | `MD9600-CSV(2571V5)-V26.45.bin` | ? | `d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f` (not described) |

All eight on hand: TYT "OutSecurityBin" container, two segments (0x00060000, 0x59800) and (0x0800C000, app length),
payload at 0x100, 0x100 trailing bytes ([hardware-md9600.md](hardware-md9600.md) 14.3).

## Container and encryption

plain[i] = file[0x100 + i] ^ key[i mod 1024]; the application starts at payload offset 0x59800, so for an application
address A: **file offset = A - 0x0800C000 + 0x59900, key index = (A - 0x0800C000) mod 1024** (the resource segment is a
multiple of 1024 long). V26.45 has the same layout (its codec is at 0x0807537C).

Key: SHA-256 `c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae`, first bytes A2 FA BB 4B. The plain
column vote (most common cipher byte per key column) gives it on all eight files without any refinement; this is what
`rfoundry/stockfw.derive_key` does first. No key file is needed.

## Same codec, eight builds

The codec does not sit at one address: every version and variant links it elsewhere, and the library's object code
differs slightly between builds (the head of V06.09 has 0x14 extra bytes at +0x9D2 against V26.45, which moves the FEC
decode; literal pools and relocations differ throughout). So **none of the V06.09 / V16.45 codec windows is
byte-identical to the V26.45 carve**: they are different links of the same library, as the reference V02.06 and V26.45
codecs are.

Functionally they are the same codec: with the same init and per-frame calls, all eight builds, the V26.45 carve and
the reference V02.06 codec give **bit-identical** air bits and decoded PCM (below, "Verification").

## Entry points

Offsets from the code start C (the first word 0x4FF0E92D, `push.w {r4-r11, lr}`), . The init order is encoder init, FEC init x2, decoder init, encoder parameters, encoder set, decoder set and one
decode; the per-frame calls are encode, encode ECC, FEC decode and decode:

| call | offset | reference | arguments (as the reference build's codec page) |
|---|---|---|---|
| encode | +0x130 | +0x130 | (bits out, 0, PCM in, 80, stack: flags, half, 0x2000, E) |
| encoder set | **+0x1C0** | +0x1C0 | (E, 128, -20) |
| encoder init | +0x284 | +0x284 | (E, 1, 1) |
| FEC init | +0x4CC | +0x4CC | (F, 1) |
| encode ECC | +0x4E8 | +0x4E8 | (bit buffer, same buffer, 0, F_enc) |
| decode | +0x5D8 | +0x5D8 | (PCM out, 80, bits in, 0, stack: flags, half, D) |
| decoder set | +0x688 | +0x688 | (D, 128, -20) |
| decoder init | +0x6D0 | +0x6D0 | (D, 1) |
| encoder parameters | +0x848 | +0x848 | (E, -22, 20, -10, stack: -40, 0x4000, 3276, 3276, 1310, 10, 20) |
| FEC decode | **per build** | +0xB54 | (49 bits out, 0, 72 bits in, 1, stack: flags, F_dec): 0 good, 4 C0 uncorrectable |

FEC decode: **+0xB78** in the CSV and CSV + GPS builds of both versions, **+0xB68** in the REC and GPS + REC builds,
+0xB64 in V26.45. +0x1C0 is the encoder set (its head differs from the decoder set's only in the BL target).

State: D 0x7EC, E 0x16E0, FEC 0x44 each (guard pages: 0x7E8, 0x16DC and 0x40 fault in decoder init, encoder init and
FEC init); stack 3822 bytes peak; no fixed RAM address; no FPU instruction executed. Identical to the reference build.
**This is RadioFoundry's STM32 vocoder call interface unchanged** (`platform/stm32f4/codec_call.c` and the boards'
glue): a board only needs the code base, the FEC decode offset and the window table.

## Codec windows

Two windows per build, cut by execution (below) and checked against the literal targets: the code window from the
first word to the end of the last executed instruction and literal pool; the table window from the lowest table
literal to the end of the last table (14 bytes at the window's end, the same bytes the reference V02.06 table window
ends with; what follows differs between builds). Both hold some vendor bytes the codec never uses.
File offset = A - 0x0800C000 + 0x59900; "key phase" = (A - 0x0800C000) mod 1024.

**06.09 CSV (`79a561ed...`), FEC decode +0xB78:**

| window | flash | length | file offset | key phase | sha256 (decrypted) | CRC-32 |
|---|---|---|---|---|---|---|
| code | 0x08074B18..0x08083D2C | 0xF214 | 0xC2418 | 0x318 | `c729480a47c7444c03eff1ec6b176a76d071f72b64d2f0fe3965e517c85a1215` | 0x5D8FB9E0 |
| tables | 0x080AB13C..0x080BC122 | 0x10FE6 | 0xF8A3C | 0x13C | `0b431c14ef7e32fe8a0915976c250f127a757ed4cc5af2bc7f867ae261c6f1ca` | 0x8BE15327 |

**16.45 CSV(V4) (`de368437...`), FEC decode +0xB78:**

| window | flash | length | file offset | key phase | sha256 (decrypted) | CRC-32 |
|---|---|---|---|---|---|---|
| code | 0x08075C98..0x08084EAC | 0xF214 | 0xC3598 | 0x098 | `4bcfec3e1113d17bb53bee1543039894db00c00c32d1b7dfd258f224043f52cd` | 0xB212C7A1 |
| tables | 0x080AC2BC..0x080BD2F2 | 0x11036 | 0xF9BBC | 0x2BC | `a101749f26c6df7d784130c45570e25abc5c88e02bee6f2510beb0a6c3ad4dfa` | 0xAAFA995B |

The other six builds (not proposed as profiles):

| file | code | tables | FEC decode | code sha256 | tables sha256 |
|---|---|---|---|---|---|
| CSV-GPS-P06.09 | 0x080760B8+0xF234 | 0x080AC6FC+0x11066 | +0xB78 | `1836c087...` | `f2b8bda5...` |
| REC-D06.09 | 0x08076C9C+0xF210 | 0x080AD660+0x10FF6 | +0xB68 | `75a93730...` | `d61403ec...` |
| GPS-REC-S06.09 | 0x08078280+0xF22C | 0x080AEC60+0x11076 | +0xB68 | `a3d76822...` | `2fe79fad...` |
| CSV-GPS(V4)-P16.45 | 0x08077434+0xF238 | 0x080ADA7C+0x110B6 | +0xB78 | `70bbb7be...` | `b2949489...` |
| REC(V4)-D16.45 | 0x08077E38+0xF234 | 0x080AE820+0x11046 | +0xB68 | `c870d2b5...` | `e1baec74...` |
| GPS-REC(V4)-S16.45 | 0x08079630+0xF1FC | 0x080AFFE0+0x110C6 | +0xB68 | `5a7f1d52...` | `2172ecdd...` |

Executed code in 06.09 CSV: 0x08074B18..0x08083D2A and a code island 0x080B25B8..0x080B26A0 inside the table window
(reached by BL from the code window, as in the hosted builds). The code also holds two code pointers, one into
each window (0x0807A40D and 0x080B01BD in 06.09 CSV).

## Position dependence

As in the hosted builds: calls inside the code are PC-relative, but the code reaches its tables through absolute
literals, the table window holds a short code island (executed; reached by BL from the code window), and the code holds
absolute code pointers into both windows. So the windows must sit at their vendor addresses. Unlike one hosted family there
are **no small tables outside the two windows** (every literal target lies in them).

## Hosting (proposal)

Run in place at the vendor addresses. The code window starts at 0x08074B18 (06.09 CSV) or 0x08075C98 (16.45 CSV), inside
flash sector 7 (0x08060000..0x0807FFFF), and runs into sector 8; the table window lies in sector 9 (0x080A0000..
0x080BFFFF). So:

* The application gets 0x0800C000 up to the code window (0x68B18 = 428,824 bytes for 06.09 CSV, 0x69C98 = 433,304 bytes
  for 16.45 CSV) if every
  flash re-merges the codec, (`keep_codec_supported=False`), or 0x0800C000..0x0805FFFF (336 KB,
  sectors 3-6) if the codec sectors are to survive updates, as for the reference build.
* The gap between the windows (0x08083D2C..0x080AB13B for 06.09 CSV) and sectors 10-11 are free for the application's
  read-only data if the linker splits it.
* A board builds for one profile at a time (the windows differ per file). Proposed profile ids `md9600-v0609-csv` and
  `md9600-v1645-csv`; the first is the default.
* Boot check: first word 0x4FF0E92D at the code start, then the CRC-32 of both windows.

**Supported donor: 06.09 CSV**, with 16.45 CSV as the second profile. Reasons: both are the plain CSV builds (no
recorder or GPS code moving the codec), both are bit-identical in function to V26.45 and the reference build, and between them
they cover the files an owner of a V1-V4 radio has. Any of the other six works the same way with its own windows from
the table above, and V26.45 can be added (its windows: the code at 0x0807537C, the FEC decode at
+0xB64; the table window still to be cut).

## Extraction recipe (06.09 CSV)

1. Check the file's SHA-256 (`79a561ed...`), its size 1094144, and the segment table (0x00060000, 0x59800),
   (0x0800C000, 0xB1800).
2. Derive the key (plain column vote) and check it by SHA-256 (`c3d665b2...`), or use a key file with that hash.
3. Decrypt each window: file offset = A - 0x0800C000 + 0x59900, key index (A - 0x0800C000) mod 1024. Check each
   window's SHA-256 and the code's first word.
4. Write them unmodified at their vendor addresses after the application image (0xFF between). On the user's machine
   only (`make`, `tools/codec_extract.py` or rfoundry), never in a release.

## Verification

The decrypted images were run in a host emulator (Thumb, Cortex-M4 model); the per-run hash covers the 72 air bits
and the decoded PCM of every frame.

| run | reference V02.06 | V26.45 carve | each of the 8 V06.09 / V16.45 builds |
|---|---|---|---|
| 1 kHz tone, 30 frames | `32b5fdbac4a399c9` | same | same |
| stress, 300 frames (noise, silence, full-scale sweeps, 0-4 injected bit errors, random frames, flags 8) | `40d0ca30a8c2f7e0` | same | same |

FEC decode results 0 and 4 as in the reference build. The minimal windows were found by bisection: each end was moved until the
stress run (60 frames) changed, then the final windows were checked with 300 stress frames with the rest of the mapped
pages filled three ways (0x00 and two random patterns); all three give the reference hash, so nothing outside the
windows is read.

## Open items

* RadioFoundry side: a profile per supported file in `stockfw.PROFILES` (with the FEC decode offset, which the reference profile
  does not carry because it is fixed there), `boards.py`, the linker script end and the vocoder glue.

## Only a radio can check

Audio level and quality through the MD-9600 path, timing at 72 MHz, and that the loader accepts the merged sectors.
