<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# MD-2017 / RT82: hosting the AMBE+2 codec

Status: format description, against the reference build ([codec-placement.md](codec-placement.md)). The codec of
every build below was run in a host instruction set emulator (measured); nothing has run on a radio.

Rule: the radio runs exactly the codec its own vendor firmware shipped. The user's machine extracts it from a firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

## Stock files

TYT published 4.05 for the MD-2017, Retevis 4.15 and EXXT_V2 for the RT82; each comes as a zip of the vendor `.bin`
files, mirrored by third parties. TYT's site lists no MD-2017 firmware today. Every file is encrypted with the MD-UV380
key (below).

| version | file | size | sha256 | radios |
|---|---|---|---|---|
| V4.05 CSV | `TYT2017-UV(CSV)-V4.05.bin` | 1276416 | `51944821b12066654101d3c9889472fd111525c2ed114ebeeb67bf1ff1bfc871` | TYT MD-2017, non-GPS, 100,000 contacts |
| D4.05 REC | `TYT2017-UV(REC)-D4.05.bin` | 1286656 | `25ee8be1643381fabb12c8139decb82a177c21c20089856fe9fdfa9574dd2264` | TYT MD-2017, non-GPS, recording, 10,000 contacts |
| P4.05 CSV-GPS | `TYT2017-UV(CSV-GPS)-P4.05.bin` | 1282560 | `95e299ed9419278f32779da1e10af0c7c1270c70374141b465cdb21a718566f6` | TYT MD-2017, GPS, 100,000 contacts |
| S4.05 GPS-REC | `TYT2017-UV(GPS-REC)-S4.05.bin` | 1293824 | `4e4ba370b81b02c27f75e814e07ef0ef0a417d439a3a79824061ca3171c325bc` | TYT MD-2017, GPS, recording, 10,000 contacts |
| V4.15 CSV | `RT82 (CSV)-V4.15.bin` | 1280512 | `16c8ce5cabdac8c0f4642aa57411fd270e42558550ad91eae453b06a27747e67` | Retevis RT82, non-GPS, 100,000 contacts |
| D4.15 REC | `RT82 (REC)-D4.15.bin` | 1291776 | `cd2e310562b8ad9bb78e9b87ea9499eafef4ab4e3539f81b49e308749e8d65ca` | Retevis RT82, non-GPS, recording, 10,000 contacts |
| P4.15 CSV-GPS | `RT82 (CSV-GPS)-P4.15.bin` | 1287680 | `7eb8d28fa1251fdbc19c58c364b3e252d4176f972f326c37e4af12329cccebb4` | Retevis RT82, GPS, 100,000 contacts |
| S4.15 GPS-REC | `RT82 (GPS-REC)-S4.15.bin` | 1298944 | `14d90ee24ccd6c5217ada3c0290c9eb7901853905bf565ef1c56b7276f33fbde` | Retevis RT82, GPS, recording, 10,000 contacts |
| EXXT_V2 GPS | `RT82_EXXT_V2_GPS.bin` | 1282560 | `ebd8bc4b75c03498cc0092daa5594400ad2eb3ebead48e57247bfb772c981c41` | Retevis RT82 GPS (variant letters unknown) |

Builds: V CSV (large contact list), D REC (recording), P CSV-GPS, S GPS-REC. The header carries no model string. Any
of the files is only a codec donor for RadioFoundry and works on any MD-2017 or RT82: the codec behaves identically in
every build (below).

## Container and encryption

The "OutSecurityBin" two-segment form of the MD-UV380 files ([codec-mduv380.md](codec-mduv380.md)): (0x00060000,
0x5D400) SPI flash resources, (0x0800C000, length) the application. The payload is XORed with the MD-UV380 bootloader
key (SHA-256 `739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991`, the `md2017` model of
`tools/stm32_dfu.py`).

**Key recovery (measured).** `rfoundry/stockfw.derive_key(stock, <the hash above>)` recovers the key from every file
and checks it by that hash: 0.1 s for the TYT and RT82 4.15 files. `RT82_EXXT_V2_GPS.bin` is about 178 KB of 0xFF
padding, so most key columns vote key ^ 0xFF; `derive_key` refines from the flipped vote (about 40 s).

## Codec in the vendor images

Every build holds the UV380 family's codec: the same entry offsets as the MD-UV380 builds, and the same output.

| build | code window | length | code sha256 | table window (0x12850 bytes) | codec data length |
|---|---|---|---|---|---|
| V4.05 CSV | 0x08075808 | 0xF6E2 | `dda72c91f42456deac38f596fdfdf5f04c01cac071e177b750f4d7006cbeec49` | 0x080D2AF4 | 0x11F56 |
| D4.05 REC | 0x08077D00 | 0xF6EA | `3c3042fc84cb13503ee19160ba05e62b644827d37d666c944dd862943d94564b` | 0x080D53D8 | 0x11F5E |
| P4.05 CSV-GPS | 0x08077164 | 0xF706 | `bb9f7b44e1bd6e398f6c3a2ea7843bfba4ba94963143dc87e5769a69c3c2dd88` | 0x080D4474 | 0x11FD6 |
| S4.05 GPS-REC | 0x08079654 | 0xF6D6 | `bead720dee75e66df7cfb3d0c55eb7036b204273ecce30566b72b5579e27b5b4` | 0x080D6D18 | 0x11FEE |
| V4.15 CSV | 0x0807690C | 0xF6DE | `46a8c76549dc387f5dc970c5e5a83d57504fc9d330893da37b8db180b51d9263` | 0x080D3BF4 | 0x11F4A |
| D4.15 REC | 0x080790D8 | 0xF6D2 | `663232750ce8a5e5f714b561939b82b3a956633650c953730f07639023f934a5` | 0x080D6798 | 0x11F52 |
| P4.15 CSV-GPS | 0x080782F0 | 0xF6FA | `fe2f0d54edec58c312bcd86ece9e685082dd4b942299cf1fea1d80d09850538f` | 0x080D55F4 | 0x11FCA |
| S4.15 GPS-REC | 0x0807AAB8 | 0xF6F2 | `146ffb2c208d42015762e856b9ae77f5d81638248506cafad9b2c6159cf760d3` | 0x080D8198 | 0x11FE2 |
| EXXT_V2 GPS | as P4.05 | | | | |

EXXT_V2's code and table windows are byte for byte P4.05's, at the same addresses: P4.05's map and profile serve it.
The GPS builds' code windows are longer than the MD-UV380 P18.11's (0xF6E2) because the codec calls a library shift
helper placed right after it.

Entry offsets from the code start: encode +0x130, encoder set +0x1C0, encoder init +0x284, FEC init +0x4CC, encode
ECC +0x4E8, decode +0x5D8, decoder set +0x688, decoder init +0x6D0, encoder parameters +0x848; FEC decode +0xB78 (V and
P builds) or +0xB68 (D and S builds). The calling convention is the reference build's.

Codec maps: `tools/rfoundry/rfoundry/codec_maps/md2017/`, one per build (EXXT_V2 uses P4.05's).

## Placement

As the MD-UV380's: the application is sectors 3-9 (`boards/md2017/memory.ld`), sectors 10 and 11 are the codec's. The
stock code window is in sectors 7-8, inside the application RadioFoundry replaces. The merge writes the tables at
their stock address in sectors 10-11, cut to the codec's data, the code right after them (word aligned, all in sector
11), and the descriptor at 0x080FFF80. No build needs the code at the start of sector 10.

| build | placed code | code ends | free below the descriptor |
|---|---|---|---|
| V4.05 CSV | 0x080E4A4C | 0x080F412E | 48722 B |
| D4.05 REC | 0x080E7338 | 0x080F6A22 | 38238 B |
| P4.05 CSV-GPS | 0x080E644C | 0x080F5B52 | 42030 B |
| S4.05 GPS-REC | 0x080E8D08 | 0x080F83DE | 31650 B |
| V4.15 CSV | 0x080E5B40 | 0x080F521E | 44386 B |
| D4.15 REC | 0x080E86EC | 0x080F7DBE | 33218 B |
| P4.15 CSV-GPS | 0x080E75C0 | 0x080F6CBA | 37574 B |
| S4.15 GPS-REC | 0x080EA17C | 0x080F986E | 26386 B |

The profiles (descriptor tag, placed addresses, CRC-32 of each placed window): `boards/md2017/vocoder_md2017.c` and
`tools/rfoundry/rfoundry/stockfw.py` (`MD2017_CODEC`, id `md2017-405-s11`). Tags: the build letter (V 0x56, D 0x44,
P 0x50, S 0x53) shifted left 16, or the version in BCD (0x0405, 0x0415).

## Verification (measured)

Each build run in a Cortex-M4 emulator three ways (the whole decrypted application, the two stock windows alone, the
placed layout after the merge), with the MD-UV380 maps' call sequence: a 30-frame 1 kHz tone and a 300-frame stress run
(noise, silence, full-scale sweeps, bit errors, random frames). The air bits and PCM of every build equal the MD-UV380
V19.26 CSV run and the DM-1701 V02.06 run. No memory fault; stack peak 3822 bytes. Not run on a radio.
