<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codec maps

This folder holds one `rfoundry-codec-map/1` file per supported manufacturer build, at `<board>/<profile>.json`. A map
says where the build keeps its AMBE+2 codec and which words and instructions in it hold addresses. The maps hold
addresses, lengths, offsets, and hashes only, never codec bytes.

A map is found by the SHA-256 of the build's code window, so one map serves every build whose code window is
byte-identical. Each map's `stock` field names the file it was made for. A build whose code window differs, even only in
words outside the codec, gets its own map.

`rfoundry/codecmap.py`, `tools/hrc7000_codec.py`, and the web flasher (through the site's `data/flash-boards.json`)
read these files. `tools/rfoundry/tests/codecmap_vectors.json` holds the test vectors the Python and JavaScript readers
share.

For the format, how a merge uses a map, and how to add one, see [The AMBE codec](../../../../docs/dev/codec.md).
