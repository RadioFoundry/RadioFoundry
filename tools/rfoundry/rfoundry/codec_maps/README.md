<!-- SPDX-License-Identifier: MIT -->
# Codec maps

One `rfoundry-codec-map/1` file per supported stock build, `<board>/<profile>.json`: where the build keeps its AMBE+2
codec and which words and instructions in it hold addresses. Format and use:
[docs/spec/codec-placement.md](../../../../docs/spec/codec-placement.md). Read by `rfoundry/codecmap.py`,
`tools/hrc7000_codec.py` and the web flasher (`data/flash-boards.json`).

A map is found by the SHA-256 of the stock code window, so one map serves every stock build whose code window is
byte-identical: `dm32-049` also covers DM32.01.01.047, `dm32-037` covers 039 and 040, `dm32-046` covers 01.02.046,
`dm1701-v0203-csv` covers DM-1701 V02.01 and V02.02 CSV. Builds whose codec code differs get a map each: the DM-1701
noCSV files have `dm1701-v0206-nocsv` and `dm1701-v0203-nocsv`.

`md9600/` maps the eight TYT MD-9600 files (V06.09 and V16.45, four variants each). No board builds for the MD-9600; the
DM-1701 takes its codec from these files, since an open-firmware install leaves that codec on the radio
([docs/spec/codec-dm1701.md](../../../../docs/spec/codec-dm1701.md)). Each was checked by running the codec, moved as
the merge moves it, in a host emulator with only the moved windows mapped: bit-identical to the stock build.

The maps hold addresses, lengths, offsets and hashes only, never codec bytes. They come from static analysis of the
stock builds, which is kept outside this repository.
