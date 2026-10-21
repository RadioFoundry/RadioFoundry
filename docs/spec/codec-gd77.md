<!-- SPDX-License-Identifier: MIT -->
# GD-77: hosting the radio's AMBE+2 codec

Status: written and checked on the host, 2026-10-21. Nothing has run on a radio.

Rule: the radio runs exactly the codec its own firmware carries. The user's machine extracts it from a stock firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

The codec is the library the DM-1701 carries ([codec-dm1701.md](codec-dm1701.md)), relinked: the same code, the same
entry offsets, the same state sizes and the same calls. It runs on the radio's MCU. The HR_C6000 only frames and
deframes DMR: it hands the 216 voice bits of a burst to the MCU over its voice port (SPI1, 27 bytes, three 72-bit air
frames) and the PCM goes over I2S0, with the HR_C6000 as clock master ([hardware-gd77.md](hardware-gd77.md) 5 and 8).
Nothing is loaded into the HR_C6000.

## Where the codec sits in the stock firmware

Three parts, in every build. The flat stock image has its flash address as file offset + 0x4000.

| Part | Content | Stock address (V4.3.9) | Length |
|---|---|---|---|
| Low block | the fixed-point helper functions | 0x4400 | 3,360 |
| Main block | the codec proper, 192 functions; 13 KB of the stock firmware's own code lies inside it and is carried along | 0x54E6C | 69,174 (69,998 in V3.2.1 to V4.2.8) |
| Table window | the codec's tables, with two small code islands | 0x684D0 | 78,120 |

The main block's address moves from build to build; the low block does not. The code reaches its tables through
absolute addresses, and three parts call each other: 171 branches cross between the parts, and 3 to 6 literal words hold
a code address.

## Placement

The low block's place is the start of ODR's application, so the merge moves the code and leaves the tables.

| Part | Where it goes |
|---|---|
| Main block | `CODE_BASE` = 0x55D00 |
| Low block | right after the main block, keeping its address modulo 4 |
| Table window | stays at the build's stock address, 0x67C58..0x7B5F7 over all builds; its two islands' calls into the code are rewritten |
| Descriptor | 0x7B780, 128 bytes (`platform/k22f/codec_desc.h`, same layout as the STM32 radios'): tag, code window, table window, a CRC-32 of each |

The application ends at 0x55D00 (`boards/gd77/gd77.ld`): 335,104 bytes. The code region is at most 73,360 bytes, so
the whole codec footprint is the region plus the 80 KB the table windows of the seven builds span. The merged image is
0x77800 bytes from flash 0x4000, which is inside every stock build's own image length.

What the merge rewrites (the pack spec of each build, `tools/odrflash/odrflash/codec_packs/gd77/<profile>.json`):

| What | Count | Rewritten to |
|---|---|---|
| Literal words that hold an address in the main or low block | 3 (V4.3.3 and later) or 6 | the new address, Thumb bit kept |
| BL and B.W instructions whose source or target moved | 171 | the new distance (the encoding kind is kept) |

Not rewritten: words in the table window that look like addresses. Two of them (0x471D and 0x5000 in V4.3.9) are filter
coefficients and a number.

The firmware (`boards/gd77/vocoder_gd77.c`) checks the descriptor, that the code and the table window are where the
build puts them with the CRCs below, and the code's first word. Only then does it call the codec, and it reports the
profile `gd77-p1`. Without a valid codec the radio is FM only.

## Supported stock builds

Seven builds; each is recognized by the SHA-256 of its three parts, not by its file name. The numbers are what the merge
produces (`gd77codec.PLACED`); the firmware pins the same ones.

| Build | Tag | Code region | Code CRC-32 | Table window | Tables CRC-32 |
|---|---|---|---|---|---|
| V3.2.1 | 0x3201 | 0x11E90 | 0xBC78498B | 0x67C58, 78,891 | 0xE81AFF5D |
| V3.2.2 | 0x3202 | 0x11E90 | 0xDBBA763D | 0x67C70, 78,867 | 0x4051B25A |
| V4.2.8 | 0x4208 | 0x11E90 | 0x1CA7B3D4 | 0x68134, 77,647 | 0xAF1FEB2E |
| V4.3.3 | 0x4303 | 0x11B58 | 0xC5EB3113 | 0x68268, 77,339 | 0xB9AE6A44 |
| V4.3.6 | 0x4306 | 0x11B58 | 0x8BBE7F89 | 0x684C8, 78,128 | 0xDCEC0213 |
| V4.3.9 | 0x4309 | 0x11B58 | 0x9F4B97AC | 0x684D0, 78,120 | 0xC59EC61F |
| V6.0.1 (Part 90) | 0x6001 | 0x11B58 | 0x28E1981D | 0x68480, 78,200 | 0xBBC2BCA7 |

The GD-77S carries the same codec at other addresses (its image is smaller); it is not supported yet.

## State and calls

As the DM-1701: encoder state 0x16E0 bytes, decoder state 0x7EC, two FEC states of 0x44, 4 KB of stack for the caller; the
same init sequence and two calls per 20 ms frame. The code uses no FPU instruction and touches no fixed RAM address.
About 30 million instructions a second at 8 kHz, a third of the 95 MHz core clock.

## The merge

`tools/gd77_codec.py --stock FLAT.bin` (also run by `make BOARD=gd77` when a stock file is in `codecs/`) reads the
stock firmware as a flat flash image, places the codec as above and writes `build/gd77/odr_gd77_codec.bin`, a flat image
from flash 0x4000 for the radio's update path ([hardware-gd77.md](hardware-gd77.md) 10). The tool refuses a file that is
not one of the seven builds, writes only under `build/` or outside the tree, and never stores the codec anywhere else.
Unpacking the radio's update file into the flat image is a separate step that this repository does not provide yet.

## Verification

The seven builds were run on the host in a Cortex-M4 model: the stock-address codec against the placed codec, 60 frames
of tone, noise and silence through encode, encode ECC, FEC decode and decode. The air bits and the PCM are identical for
every build, and identical between builds. The placed CRC-32s above come from that same pipeline
(`tools/odrflash/tests/test_gd77codec.py` pins the arithmetic with a synthetic image).

## Open items

- A radio test of voice RX and TX (levels, timing, the I2S slot).
- Whether the stock bootloader accepts the merged image.
- The GD-77S builds.
