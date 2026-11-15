<!-- SPDX-License-Identifier: MIT -->

# CHIRP image of the Baofeng UV-5R

The layout `site/static/js/codeplug/chirp_img.js` reads. It was learned from one `.img` and the CSV CHIRP wrote from
it, by matching each CSV row to the bytes; CHIRP's source was not read. Only what that pair confirms is listed.

An image is the radio's memory, then a footer. All values are little endian.

## Footer

At offset 0x194A: the bytes `63 68 69 72 70 EE 69 6D 67 00` ("chirp", 0xEE, "img", 0), a version byte, then a base64
JSON object that names the radio: `rclass` (`BaofengUV5RGeneric`), `vendor`, `model` (`UV-5R`), `chirp_version`. The
editor reads an image only when the model, class and footer offset all match; any other is refused by its model name.

## Channels

128 channels, number n = 0 to 127.

| Offset | Size | Content |
|---|---|---|
| 0x0008 + 16 n | 4 | RX frequency: BCD, 10 Hz units, low digits first (`00 25 27 46` = 462.725 MHz). All `FF` = channel unused |
| +4 | 4 | TX frequency, same coding |
| +8 | 2 | RX tone |
| +10 | 2 | TX tone |
| +12 | 2 | 0 in every sample |
| +14 | 1 | bit 0: low power (1.0 W; clear is 4.0 W) |
| +15 | 1 | bit 6: wide (clear is narrow); bit 2: in the scan list (clear is skipped) |
| 0x1008 + 16 n | 16 | Name, Latin-1, padded with `FF` |

Tone words: 0 or `FFFF` none; 600 to 3000 a CTCSS tone in tenths of a hertz (1035 = `0x040B` = 103.5 Hz);
1 to 104 the DCS code at that place in the standard list (023, 025, ... 754). A tone-only channel has RX tone 0 and a
TX tone; tone squelch has both the same. Reversed DCS polarity is not in the sample and is not read.

## Not read

Everything else: VFOs, the radio's settings, the second flag word of one channel in a later image.
