<!-- SPDX-License-Identifier: MIT -->
# Codec maps

One `rfoundry-codec-map/1` file per supported stock build, `<board>/<profile>.json`: where the build keeps its AMBE+2
codec and which words and instructions in it hold addresses. Format and use:
[docs/spec/codec-placement.md](../../../../docs/spec/codec-placement.md). Read by `rfoundry/codecmap.py`,
`tools/hrc7000_codec.py` and the web flasher (`data/flash-boards.json`).

A map is found by the SHA-256 of the stock code window, so one map serves every stock build whose code window is
byte-identical: `dm32-049` also covers DM32.01.01.047, `dm32-037` covers 039 and 040, `dm32-046` covers 01.02.046.

The maps hold addresses, lengths, offsets and hashes only, never codec bytes. They come from static analysis of the
stock builds, which is kept outside this repository.
