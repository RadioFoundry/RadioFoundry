<!-- SPDX-License-Identifier: MIT -->
# MD-9600: hosting the AMBE+2 codec

Status: format description and a map and install profile for each of 32 TYT and Retevis original firmware files (03.28
to 26.57, below). The codecs were run in a host instruction set emulator (measured); no MD-9600 has run any of them, the V5
(26.xx) ones included.

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
| 03.28 REC | older (board not known) | `MD9600-REC-D03.28.bin` | 1089024 | `b25c573013b79f58a65629f0df85f5aced57c89eabde40465331984bb14dcc38` |
| 03.29 REC | older (board not known) | `MD9600-REC-D03.29.bin` | 1089024 | `9035209ffb2ecd08999cea93bfe5d255006e783f0a4da11354390c9ecbcb8521` |
| 04.03 REC | older (board not known) | `MD9600-REC-D04.03.bin` | 1089024 | `32044e55879afa44c7a525a9a53633a51e38c3ae01ff512d2d0971117a97a75b` |
| 06.03 CSV | V1-V3 | `MD9600-CSV-V06.03.bin` | 1093120 | `cf52b678629466bac593d533abc87fd7524c6e123b608dce6ef0d4e685a1d79e` |
| 06.03 CSV + GPS | V1-V3 | `MD9600-CSV-GPS-P06.03.bin` | 1099264 | `50eed60616030f5ca110db649ec938782d90b23d7c911d05e388a0a7cd20af53` |
| 06.03 REC | V1-V3 | `MD9600-REC-D06.03.bin` | 1102336 | `6356f4f467942ea0a1137d625071945ad47c0af72320bfc4f7890d99371cd8ca` |
| 06.03 GPS + REC | V1-V3 | `MD9600-GPS-REC-S06.03.bin` | 1108480 | `4cb9f3bd483eb998a96c2700b070af94393f91aab688dd5b0670c85e379f1452` |
| **06.09 CSV (first supported)** | V1-V3 | `MD9600-CSV-V06.09.bin` | 1094144 | `79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09` |
| 06.09 CSV + GPS | V1-V3 | `MD9600-CSV-GPS-P06.09.bin` | 1100288 | `9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5` |
| 06.09 REC | V1-V3 | `MD9600-REC-D06.09.bin` | 1103360 | `7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f` |
| 06.09 GPS + REC | V1-V3 | `MD9600-GPS-REC-S06.09.bin` | 1109504 | `4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7` |
| 06.24 CSV | V1-V3 | `MD9600-CSV-V06.24.bin` | 1094144 | `194526d60f796cc0f7339d14c1efeedc267731d108fd66a96876b459408de0ba` |
| 06.24 CSV + GPS | V1-V3 | `MD9600-CSV-GPS-P06.24.bin` | 1100288 | `1eafdb774fd71cb7bc3334d06617ed42264ee25df1c73f61744f6b21742bbd20` |
| 06.24 REC | V1-V3 | `MD9600-REC-D06.24.bin` | 1104384 | `6643261e62b9778a5df43b1e5c3428d6d308bf54b34478513df3e08f6f4dbb63` |
| 06.24 GPS + REC | V1-V3 | `MD9600-GPS-REC-S06.24.bin` | 1110528 | `ec44929e855442ad248f1a5c171a467922d6cb5742fc35f759db269c645e22ab` |
| 16.24 CSV | V4 | `MD9600-CSV(V4)-V16.24.bin` | 1095168 | `98302fbc26fa0db93bd7ecbd29a410ac9fbb42cbd7d6ce15d54ce085fefff329` |
| 16.24 CSV + GPS | V4 | `MD9600-CSV-GPS(V4)-P16.24.bin` | 1101312 | `e86b9e9775758548dd2d01334cfacc96c640fbf843aa6c5fa009b4bc7598e98d` |
| 16.24 REC | V4 | `MD9600-REC(V4)-D16.24.bin` | 1105408 | `e21c72e30716fb43e4d15798012aec15e964b9ec8c1a534ede5d0b0fe73f3acb` |
| 16.24 GPS + REC | V4 | `MD9600-GPS-REC(V4)-S16.24.bin` | 1111552 | `4e1c5c081d5bdf4bef8f205668fd7381391187d8b07150df7e99275b030b811b` |
| **16.45 CSV (first supported)** | V4 | `MD9600-CSV(V4)-V16.45.bin` | 1099264 | `de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532` |
| 16.45 CSV + GPS | V4 | `MD9600-CSV-GPS(V4)-P16.45.bin` | 1105408 | `7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f` |
| 16.45 REC | V4 | `MD9600-REC(V4)-D16.45.bin` | 1108480 | `d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c` |
| 16.45 GPS + REC | V4 | `MD9600-GPS-REC(V4)-S16.45.bin` | 1114624 | `3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff` |
| 26.38 REC | V5, Retevis RT90 ("2571") | `RT90-REC(2571V5)-D26.38.bin` | 1105408 | `267dac314c1aa217ab259a6f9709f2e19342ff3a79aa18a2bf8b501939347e49` |
| 26.41 CSV | V5, Retevis RT90 ("2571") | `RT90-CSV(2571V5)-V26.41.bin` | 1096192 | `1536ea3093394859c29ec6a522464c1b3f87a516e1a73b2cbf4f7be254b2dec4` |
| 26.41 CSV + GPS | V5, Retevis RT90 ("2571") | `RT90-CSV-GPS(2571V5)-P26.41.bin` | 1102336 | `8acfedbc60fe8dec2d88ed1f00a1444d6f8f243590d967a1a59b121f5d6dfef4` |
| 26.42 GPS + REC | V5, Retevis RT90 ("2571") | `RT90-GPS-REC(2571V5)-S26.42.bin` | 1111552 | `a10bad809048c8bbd9602c7b50ca4c824a6dbdee34b2167b872ec86c0affbbe1` |
| 26.45 CSV | V5 ("2571") | `MD9600-CSV(2571V5)-V26.45.bin` | 1096192 | `d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f` |
| 26.45 CSV + GPS | V5 ("2571") | `MD9600-CSV-GPS(2571V5)-P26.45.bin` | 1102336 | `02e366687f122ed752581344db8316666d21782d9353334518b69237a81e74dc` |
| 26.45 REC | V5 ("2571") | `MD9600-REC(2571V5)-D26.45.bin` | 1106432 | `baee36b0fd852dfc1fb14952242a1231dbfa4fabc37cba57c61f1b8d3e0d830d` |
| 26.45 GPS + REC | V5 ("2571") | `MD9600-GPS-REC(2571V5)-S26.45.bin` | 1112576 | `6df34152e9786530d61032ab71f1962cf194aca0b99cf0a54e3802fd2cd4ac72` |
| 26.57 REC | V4A ("2571V4") | `MD9600-REC(2571V4)-D26.57.bin` | 1107456 | `2522008737d7845e43ab47a307d38498ee783c788a377fac855dff94dea53af0` |

All 32 on hand (the 03.28, 03.29 and 04.03 ones from a dealer's page, the 06.03 ones from a community page that mirrors TYT's update, the RT90 ones from the Retevis firmware download, the others as before): TYT "OutSecurityBin" container, two segments (0x00060000, 0x59800) and (0x0800C000, app length),
payload at 0x100, 0x100 trailing bytes ([hardware-md9600.md](hardware-md9600.md) 14.3).

## Container and encryption

plain[i] = file[0x100 + i] ^ key[i mod 1024]; the application starts at payload offset 0x59800, so for an application
address A: **file offset = A - 0x0800C000 + 0x59900, key index = (A - 0x0800C000) mod 1024** (the resource segment is a
multiple of 1024 long). Every 26.xx file has the same layout; V26.45's codec code starts at 0x0807537C (checked in the
decoded image).

Key: SHA-256 `c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae`, first bytes A2 FA BB 4B. The plain
column vote (most common cipher byte per key column) gives it on all the files without any refinement; this is what
`rfoundry/stockfw.derive_key` does first. No key file is needed.

## Same codec, 32 builds

The codec does not sit at one address: every version and variant links it elsewhere, and the library's object code
differs slightly between builds (the head of V06.09 has 0x14 extra bytes at +0x9D2 against V26.45, which moves the FEC
decode; literal pools and relocations differ throughout). So **no two codec windows are
byte-identical**: they are different links of the same library, as the reference V02.06 and V26.45
codecs are.

Functionally they are the same codec: with the same init and per-frame calls, all 32 builds and the reference V02.06
codec give **bit-identical** air bits and decoded PCM (below, "Verification"). The 26.xx (V5) builds use the same entry
offsets as the rest, so the calls in `boards/md9600/vocoder_md9600.c` fit them as they are; only the FEC decode
differs (+0xB64 in the 26.45 CSV builds, +0xB54 in the 26.xx REC ones), and the firmware does not call it. The three REC
builds 03.28, 03.29 and 04.03 have a code window 0x1000 bytes longer than the others (0x102xx), because the library link
puts more of the codec's routines past the entries; their entry offsets are the 06.09 REC build's. Checked on 03.28 by comparing each
entry's first 40 bytes with the 06.09 REC build (four entries match in all 40, the other six in at least 35, the
difference being literal pool offsets) and on all three by the emulator run; the map tool's carve could not place the
decoder set in them, so these three maps take the entry offsets from that comparison.

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

FEC decode: **+0xB78** in the CSV and CSV + GPS builds of the 06.xx and 16.xx versions, **+0xB68** in their REC and GPS +
REC builds, +0xB64 in the 26.45 CSV and CSV + GPS builds, +0xB54 in the 26.xx REC and GPS + REC builds (the 03.28, 03.29
and 04.03 value, +0xB68, is the 06.09 REC build's and was not checked). +0x1C0 is the encoder set (its head differs from the decoder set's only in the BL target).

State: D 0x7EC, E 0x16E0, FEC 0x44 each (guard pages: 0x7E8, 0x16DC and 0x40 fault in decoder init, encoder init and
FEC init); stack 3822 bytes peak; no fixed RAM address; no FPU instruction executed. Identical to the reference build.
**This is RadioFoundry's STM32 vocoder call interface unchanged** (`platform/stm32f4/codec_call.c` and the boards'
glue): a board only needs the code base, the FEC decode offset and the window table.

Encode flags: the shared host (`platform/stm32f4/hrc/vocoder.c`) passes one flags word on every encode call, the
same word as on the DM-1701 and MD-380 (`drivers/vocoder/ambe2_flags.h`, 0x1840 by default; the bits:
[codec-md380.md](codec-md380.md), "Encode flags"). On the MD-9600 builds the bits are unverified: the code is the
same family, but they were not measured on it.

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

Every other build has a map and a profile too:

| file | code | tables | FEC decode | code sha256 | tables sha256 |
|---|---|---|---|---|---|
| REC-D03.28 | 0x080724B4+0x102F6 | 0x080A9F5C+0x10C8E | +0xB68 (as 06.09 REC, not checked) | `79f12a84...` | `897fa811...` |
| REC-D03.29 | 0x080724C0+0x102EA | 0x080A9F5C+0x10C8E | +0xB68 (as 06.09 REC, not checked) | `066d2c85...` | `07f412b7...` |
| REC-D04.03 | 0x080727CC+0x102DE | 0x080AA25C+0x10C7E | +0xB68 (as 06.09 REC, not checked) | `6cdc6fad...` | `484461f3...` |
| CSV-V06.03 | 0x080746B0+0xF23A | 0x080AACFC+0x10FE6 | +0xB78 | `8ebfa48e...` | `65595c1b...` |
| CSV-GPS-P06.03 | 0x08075C80+0xF234 | 0x080AC2BC+0x11066 | +0xB78 | `01b4e193...` | `0912f563...` |
| REC-D06.03 | 0x0807682C+0xF210 | 0x080AD1E0+0x10FF6 | +0xB68 | `1bfe2e1e...` | `ed2b8cea...` |
| GPS-REC-S06.03 | 0x08077E3C+0xF22E | 0x080AE820+0x11076 | +0xB68 | `f6c803bd...` | `624a0d72...` |
| CSV-GPS-P06.09 | 0x080760B8+0xF234 | 0x080AC6FC+0x11066 | +0xB78 | `1836c087...` | `f2b8bda5...` |
| REC-D06.09 | 0x08076C9C+0xF210 | 0x080AD660+0x10FF6 | +0xB68 | `75a93730...` | `d61403ec...` |
| GPS-REC-S06.09 | 0x08078280+0xF22C | 0x080AEC60+0x11076 | +0xB68 | `a3d76822...` | `2fe79fad...` |
| CSV-V06.24 | 0x08074C78+0xF232 | 0x080AB2BC+0x10FE6 | +0xB78 | `cc3372d8...` | `3e19a511...` |
| CSV-GPS-P06.24 | 0x080763CC+0xF21E | 0x080AC9FC+0x11066 | +0xB78 | `403b3df9...` | `be95f990...` |
| REC-D06.24 | 0x08076DFC+0xF22E | 0x080AD7E0+0x10FF6 | +0xB68 | `c5b5123c...` | `8db73635...` |
| GPS-REC-S06.24 | 0x08078594+0xF216 | 0x080AEF60+0x11076 | +0xB68 | `16fbf16c...` | `10db31bf...` |
| CSV(V4)-V16.24 | 0x08074FB0+0xF23A | 0x080AB5FC+0x10FE6 | +0xB78 | `f68bd78b...` | `df2ca5d5...` |
| CSV-GPS(V4)-P16.24 | 0x08076704+0xF226 | 0x080ACD3C+0x11066 | +0xB78 | `bd0d774b...` | `545966b6...` |
| REC(V4)-D16.24 | 0x08077134+0xF236 | 0x080ADB20+0x10FF6 | +0xB68 | `d7f73d85...` | `c989388d...` |
| GPS-REC(V4)-S16.24 | 0x080788CC+0xF21E | 0x080AF2A0+0x11076 | +0xB68 | `181da1f9...` | `1af5f6df...` |
| CSV-GPS(V4)-P16.45 | 0x08077434+0xF238 | 0x080ADA7C+0x110B6 | +0xB78 | `70bbb7be...` | `b2949489...` |
| REC(V4)-D16.45 | 0x08077E38+0xF234 | 0x080AE820+0x11046 | +0xB68 | `c870d2b5...` | `e1baec74...` |
| GPS-REC(V4)-S16.45 | 0x08079630+0xF1FC | 0x080AFFE0+0x110C6 | +0xB68 | `5a7f1d52...` | `2172ecdd...` |
| RT90-REC(2571V5)-D26.38 | 0x080771B0+0xF210 | 0x080ADB60+0x1100E | +0xB54 | `96e8464d...` | `dda353cf...` |
| RT90-CSV(2571V5)-V26.41 | 0x080751E0+0xF214 | 0x080AB7FC+0x11036 | +0xB64 | `52d94dde...` | `356f6ae0...` |
| RT90-CSV-GPS(2571V5)-P26.41 | 0x08076980+0xF234 | 0x080ACFBC+0x110B6 | +0xB64 | `a09d8a83...` | `085caab1...` |
| RT90-GPS-REC(2571V5)-S26.42 | 0x08078C18+0xF22C | 0x080AF5E0+0x110C6 | +0xB54 | `74395e46...` | `8f193578...` |
| CSV(2571V5)-V26.45 | 0x0807537C+0xF22E | 0x080AB9BC+0x11036 | +0xB64 | `c437494b...` | `b7017c23...` |
| CSV-GPS(2571V5)-P26.45 | 0x08076B18+0xF212 | 0x080AD13C+0x110B6 | +0xB64 | `72dd6322...` | `b6c9bf64...` |
| REC(2571V5)-D26.45 | 0x0807751C+0xF20E | 0x080ADEE0+0x11046 | +0xB54 | `8974385c...` | `fa652e60...` |
| GPS-REC(2571V5)-S26.45 | 0x08078D14+0xF216 | 0x080AF6E0+0x110C6 | +0xB54 | `7789393c...` | `17ad382f...` |
| REC(2571V4)-D26.57 | 0x08077C3C+0xF1EE | 0x080AE5E0+0x11032 | +0xB54 | `c43426f0...` | `87786610...` |

Executed code in 06.09 CSV: 0x08074B18..0x08083D2A and a code island 0x080B25B8..0x080B26A0 inside the table window
(reached by BL from the code window, as in the hosted builds). The code also holds two code pointers, one into
each window (0x0807A40D and 0x080B01BD in 06.09 CSV).

## Position dependence

As in the hosted builds: calls inside the code are PC-relative, but the code reaches its tables through absolute
literals, the table window holds a short code island (executed; reached by BL from the code window), and the code holds
absolute code pointers into both windows. So the windows must sit at their vendor addresses. Unlike one hosted family there
are **no small tables outside the two windows** (every literal target lies in them).

## Hosting

Decided as the MD-380 (`docs/spec/codec-placement.md`). The code window starts in sector 7
(0x08074B18 or 0x08075C98), which the application owns (sectors 3-9, `boards/md9600/memory.ld`), so the merge moves
both windows as blocks:

| Part | Where it goes | Why |
|---|---|---|
| Code (0xF1EE to 0x102F6 bytes by build) | start of sector 11, 0x080E0000 | the code is position independent except for 1 self reference, its absolute references into the table window and 12 calls from the table window's island; the codec map lists them |
| Table window (0x10FE6 to 0x110C6 bytes by build) | start of sector 10, 0x080C0000 plus the build's address modulo 16 (06.09 and 16.45: 0x080C000C) | too long to share sector 11 with the code (the two windows together are up to 135,044 bytes); every build leaves at least 61 KB of sector 10 and 64 KB of sector 11 free |
| Descriptor | end of sector 11, 0x080FFF80 | `platform/stm32f4/codec_desc.h`: tag, code window, table window, a CRC-32 of each |

The codec's flash is sectors 10 and 11 (`CODEC_FLASH_LO` 0x080C0000 in the firmware profiles). The application never
links past 0x080BFFFF, so an update erases sectors 3-9 only and keeps the codec.

**Earlier layout.** Releases before this one kept the table window at the build's own address in sector 9 (four GPS-REC
builds, S16.24, S16.45, S26.42 and S26.45, ran 0x316 to 0x10A6 bytes into sector 10), gave the application sectors 3-8
and reported `md9600-s11`. The flashing tools read such a codec back and move it on the first install of this layout
([codec-placement.md](codec-placement.md), "Moving to a new layout"); its profiles are the `former` entries in
`stockfw.py`.

One profile per file, one codec id (`md9600-s11v2`): the tag in the descriptor says which build the radio holds. The tag
is the variant in bits 16-17 (0 CSV, 1 CSV-GPS, 2 REC, 3 GPS-REC) and the version in BCD below, so 0x0609 is 06.09 CSV,
0x1645 is 16.45 CSV and 0x11645 its CSV-GPS build.

The codec in the earlier layout was run on the host (Cortex-M model) against the stock-address run: the same 60-frame
vector (encode and decode, tone plus noise) gives the same air bits and PCM for every build, with the code at
0x080E0000 and the table window's island calls rewritten. The current layout (the table window moved as well) has not
had that run yet. The placed CRC-32s are in `stockfw.PROFILES` and
`boards/md9600/vocoder_md9600.c` and must match.

**Supported donors: every file in the first table.** 06.09 CSV (V1-V3 boards) and 16.45 CSV (V4) came first and are the
default; they are the plain CSV builds (no recorder or GPS code moving the codec). The rest work the same way with their
own windows from the table above. An owner of a V5 radio (the TYT 26.45 files, the Retevis RT90 26.38, 26.41 and 26.42
files, or the 26.57 V4A REC build) installs from the file the radio came with: the 26.xx codec calls are the 16.xx ones
at the same entry offsets. **Not covered:** the 05.xx files (05.02, 05.04 and 05.05), whose copies on hand are cut off at 1 MiB, short of their
tables. A file whose hash is none of the above is refused, not guessed at.

## Extraction recipe (06.09 CSV)

1. Check the file's SHA-256 (`79a561ed...`), its size 1094144, and the segment table (0x00060000, 0x59800),
   (0x0800C000, 0xB1800).
2. Derive the key (plain column vote) and check it by SHA-256 (`c3d665b2...`), or use a key file with that hash.
3. Decrypt each window: file offset = A - 0x0800C000 + 0x59900, key index (A - 0x0800C000) mod 1024. Check each
   window's SHA-256 and the code's first word.
4. Write the code at 0x080E0000 and the table window at 0x080C000C with the references between them rewritten (codec
   map), then the descriptor at 0x080FFF80 (0xFF between). On the user's machine
   only (`make`, `tools/codec_extract.py` or rfoundry), never in a release.

## Verification

The decrypted images were run in a host emulator (Thumb, Cortex-M4 model); the per-run hash covers the 72 air bits
and the decoded PCM of every frame.

| run | reference V02.06 | V26.45 carve | each of the 8 V06.09 / V16.45 builds |
|---|---|---|---|
| 1 kHz tone, 30 frames | `32b5fdbac4a399c9` | same | same |
| stress, 300 frames (noise, silence, full-scale sweeps, 0-4 injected bit errors, random frames, flags 8) | `40d0ca30a8c2f7e0` | same | same |

The other 24 builds (all 32 in all) were run again with the vector set below, each as TYT links it and moved as the merge
moves it, with only the moved windows mapped:

| run | each of the 32 builds, at the stock address and as placed |
|---|---|
| 60 frames (encode and decode, tone plus noise), air bits and PCM hashed | `7cb52253c4b2` in all 64 runs |
| 300 frames (random samples, silence, sine sweeps, square waves, low-level noise, flags 8 on every 7th frame), air bits and PCM hashed | `2788dd5ca0ad31c5` in all 64 runs |

This second set has no injected bit errors. A decode of a frame with three injected errors gave different PCM for the
06.09 CSV and the 16.45 CSV builds in a trial run, so the cross-build identity of decoded error frames is not claimed;
the placed run is bit-identical to the stock-address run of its own build, which is what the merge needs.

FEC decode results 0 and 4 as in the reference build. The minimal windows were found by bisection: each end was moved until the
stress run (60 frames) changed, then the final windows were checked with 300 stress frames with the rest of the mapped
pages filled three ways (0x00 and two random patterns); all three give the reference hash, so nothing outside the
windows is read.

## Open items

* Every file on hand has a map and a profile. The 05.xx files are not on hand in full (above).
* The RT90 files' maps were made against the 06.09 builds (the map tool's reference), not against the 26.45 ones; they
  carve without outside references and run to the same hash as the rest.
* The 03.28, 03.29 and 04.03 maps take their entry offsets from a comparison with 06.09 REC, and their FEC decode offset
  was not checked (the firmware does not call it).
* The encode flags (above) are unverified on every build, the V5 ones included.
* The DM-1701's firmware profile list (`boards/dm1701/vocoder_dm1701.c`) takes the 21 files before 06.03; the seven newer
  ones are MD-9600 sources only.

## Only a radio can check

Audio level and quality through the MD-9600 path, timing at 72 MHz, and that the loader accepts the merged sectors.
